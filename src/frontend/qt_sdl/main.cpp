/*
    Copyright 2016-2022 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <vector>
#include <string>
#include <algorithm>

#include <QProcess>
#include <QApplication>
#include <QMessageBox>
#include <QMenuBar>
#include <QFileDialog>
#include <QInputDialog>
#include <QPaintEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QVector>
#include <QCommandLineParser>
#ifndef _WIN32
#include <QGuiApplication>
#include <QSocketNotifier>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#ifndef APPLE
#include <qpa/qplatformnativeinterface.h>
#endif
#endif

#include <SDL2/SDL.h>

#include "OpenGLSupport.h"
#include "duckstation/gl/context.h"

#include "main.h"
#include "Input.h"
#include "CheatsDialog.h"
#include "EmuSettingsDialog.h"
#include "InputConfig/InputConfigDialog.h"
#include "VideoSettingsDialog.h"
#include "CameraSettingsDialog.h"
#include "AudioSettingsDialog.h"
#include "FirmwareSettingsDialog.h"
#include "PathSettingsDialog.h"
#include "MPSettingsDialog.h"
#include "WifiSettingsDialog.h"
#include "InterfaceSettingsDialog.h"
#include "ROMInfoDialog.h"
#include "RAMInfoDialog.h"
#include "TitleManagerDialog.h"
#include "PowerManagement/PowerManagementDialog.h"

#include "types.h"
#include "version.h"

#include "FrontendUtil.h"
#include "OSD.h"

#include "NDS.h"
#include "NDSCart.h"
#include "GBACart.h"
#include "GPU.h"
#include "SPU.h"
#include "Wifi.h"
#include "Platform.h"
#include "LocalMP.h"
#include "Config.h"

#include "Savestate.h"

#include "main_shaders.h"

#include "ROMManager.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"

#include "CLI.h"

// TODO: uniform variable spelling

bool RunningSomething;

MainWindow* mainWindow;
EmuThread* emuThread;

int autoScreenSizing = 0;

int videoRenderer;
GPU::RenderSettings videoSettings;
bool videoSettingsDirty;

SDL_AudioDeviceID audioDevice;
int audioFreq;
bool audioMuted;
SDL_cond* audioSync;
SDL_mutex* audioSyncLock;

SDL_AudioDeviceID micDevice;
s16 micExtBuffer[2048];
u32 micExtBufferWritePos;

u32 micWavLength;
s16* micWavBuffer;

CameraManager* camManager[2];
bool camStarted[2];

const struct { int id; float ratio; const char* label; } aspectRatios[] =
{
    { 0, 1,                       "4:3 (原生)" },
    { 4, (5.f  / 3) / (4.f / 3), "5:3 (3DS)"},
    { 1, (16.f / 9) / (4.f / 3),  "16:9" },
    { 2, (21.f / 9) / (4.f / 3),  "21:9" },
    { 3, 0,                       "window" }
};

void micCallback(void* data, Uint8* stream, int len);



void audioCallback(void* data, Uint8* stream, int len)
{
    len /= (sizeof(s16) * 2);

    // resample incoming audio to match the output sample rate

    int len_in = Frontend::AudioOut_GetNumSamples(len);
    s16 buf_in[1024*2];
    int num_in;

    SDL_LockMutex(audioSyncLock);
    num_in = SPU::ReadOutput(buf_in, len_in);
    SDL_CondSignal(audioSync);
    SDL_UnlockMutex(audioSyncLock);

    if ((num_in < 1) || audioMuted)
    {
        memset(stream, 0, len*sizeof(s16)*2);
        return;
    }

    int margin = 6;
    if (num_in < len_in-margin)
    {
        int last = num_in-1;

        for (int i = num_in; i < len_in-margin; i++)
            ((u32*)buf_in)[i] = ((u32*)buf_in)[last];

        num_in = len_in-margin;
    }

    Frontend::AudioOut_Resample(buf_in, num_in, (s16*)stream, len, Config::AudioVolume);
}

void audioMute()
{
    int inst = Platform::InstanceID();
    audioMuted = false;

    switch (Config::MPAudioMode)
    {
    case 1: // only instance 1
        if (inst > 0) audioMuted = true;
        break;

    case 2: // only currently focused instance
        if (!mainWindow->isActiveWindow()) audioMuted = true;
        break;
    }
}


void micOpen()
{
    if (Config::MicInputType != 1)
    {
        micDevice = 0;
        return;
    }

    SDL_AudioSpec whatIwant, whatIget;
    memset(&whatIwant, 0, sizeof(SDL_AudioSpec));
    whatIwant.freq = 44100;
    whatIwant.format = AUDIO_S16LSB;
    whatIwant.channels = 1;
    whatIwant.samples = 1024;
    whatIwant.callback = micCallback;
    micDevice = SDL_OpenAudioDevice(NULL, 1, &whatIwant, &whatIget, 0);
    if (!micDevice)
    {
        printf("Mic init failed: %s\n", SDL_GetError());
    }
    else
    {
        SDL_PauseAudioDevice(micDevice, 0);
    }
}

void micClose()
{
    if (micDevice)
        SDL_CloseAudioDevice(micDevice);

    micDevice = 0;
}

void micLoadWav(std::string name)
{
    SDL_AudioSpec format;
    memset(&format, 0, sizeof(SDL_AudioSpec));

    if (micWavBuffer) delete[] micWavBuffer;
    micWavBuffer = nullptr;
    micWavLength = 0;

    u8* buf;
    u32 len;
    if (!SDL_LoadWAV(name.c_str(), &format, &buf, &len))
        return;

    const u64 dstfreq = 44100;

    int srcinc = format.channels;
    len /= ((SDL_AUDIO_BITSIZE(format.format) / 8) * srcinc);

    micWavLength = (len * dstfreq) / format.freq;
    if (micWavLength < 735) micWavLength = 735;
    micWavBuffer = new s16[micWavLength];

    float res_incr = len / (float)micWavLength;
    float res_timer = 0;
    int res_pos = 0;

    for (int i = 0; i < micWavLength; i++)
    {
        u16 val = 0;

        switch (SDL_AUDIO_BITSIZE(format.format))
        {
        case 8:
            val = buf[res_pos] << 8;
            break;

        case 16:
            if (SDL_AUDIO_ISBIGENDIAN(format.format))
                val = (buf[res_pos*2] << 8) | buf[res_pos*2 + 1];
            else
                val = (buf[res_pos*2 + 1] << 8) | buf[res_pos*2];
            break;

        case 32:
            if (SDL_AUDIO_ISFLOAT(format.format))
            {
                u32 rawval;
                if (SDL_AUDIO_ISBIGENDIAN(format.format))
                    rawval = (buf[res_pos*4] << 24) | (buf[res_pos*4 + 1] << 16) | (buf[res_pos*4 + 2] << 8) | buf[res_pos*4 + 3];
                else
                    rawval = (buf[res_pos*4 + 3] << 24) | (buf[res_pos*4 + 2] << 16) | (buf[res_pos*4 + 1] << 8) | buf[res_pos*4];

                float fval = *(float*)&rawval;
                s32 ival = (s32)(fval * 0x8000);
                ival = std::clamp(ival, -0x8000, 0x7FFF);
                val = (s16)ival;
            }
            else if (SDL_AUDIO_ISBIGENDIAN(format.format))
                val = (buf[res_pos*4] << 8) | buf[res_pos*4 + 1];
            else
                val = (buf[res_pos*4 + 3] << 8) | buf[res_pos*4 + 2];
            break;
        }

        if (SDL_AUDIO_ISUNSIGNED(format.format))
            val ^= 0x8000;

        micWavBuffer[i] = val;

        res_timer += res_incr;
        while (res_timer >= 1.0)
        {
            res_timer -= 1.0;
            res_pos += srcinc;
        }
    }

    SDL_FreeWAV(buf);
}

void micCallback(void* data, Uint8* stream, int len)
{
    s16* input = (s16*)stream;
    len /= sizeof(s16);

    int maxlen = sizeof(micExtBuffer) / sizeof(s16);

    if ((micExtBufferWritePos + len) > maxlen)
    {
        u32 len1 = maxlen - micExtBufferWritePos;
        memcpy(&micExtBuffer[micExtBufferWritePos], &input[0], len1*sizeof(s16));
        memcpy(&micExtBuffer[0], &input[len1], (len - len1)*sizeof(s16));
        micExtBufferWritePos = len - len1;
    }
    else
    {
        memcpy(&micExtBuffer[micExtBufferWritePos], input, len*sizeof(s16));
        micExtBufferWritePos += len;
    }
}

void micProcess()
{
    int type = Config::MicInputType;
    bool cmd = Input::HotkeyDown(HK_Mic);

    if (type != 1 && !cmd)
    {
        type = 0;
    }

    switch (type)
    {
    case 0: // no mic
        Frontend::Mic_FeedSilence();
        break;

    case 1: // host mic
    case 3: // WAV
        Frontend::Mic_FeedExternalBuffer();
        break;

    case 2: // white noise
        Frontend::Mic_FeedNoise();
        break;
    }
}


EmuThread::EmuThread(QObject* parent) : QThread(parent)
{
    EmuStatus = 0;
    EmuRunning = 2;
    EmuPause = 0;
    RunningSomething = false;

    connect(this, SIGNAL(windowUpdate()), mainWindow->panelWidget, SLOT(repaint()));
    connect(this, SIGNAL(windowTitleChange(QString)), mainWindow, SLOT(onTitleUpdate(QString)));
    connect(this, SIGNAL(windowEmuStart()), mainWindow, SLOT(onEmuStart()));
    connect(this, SIGNAL(windowEmuStop()), mainWindow, SLOT(onEmuStop()));
    connect(this, SIGNAL(windowEmuPause()), mainWindow->actPause, SLOT(trigger()));
    connect(this, SIGNAL(windowEmuReset()), mainWindow->actReset, SLOT(trigger()));
    connect(this, SIGNAL(windowEmuFrameStep()), mainWindow->actFrameStep, SLOT(trigger()));
    connect(this, SIGNAL(windowLimitFPSChange()), mainWindow->actLimitFramerate, SLOT(trigger()));
    connect(this, SIGNAL(screenLayoutChange()), mainWindow->panelWidget, SLOT(onScreenLayoutChanged()));
    connect(this, SIGNAL(windowFullscreenToggle()), mainWindow, SLOT(onFullscreenToggled()));
    connect(this, SIGNAL(swapScreensToggle()), mainWindow->actScreenSwap, SLOT(trigger()));

    static_cast<ScreenPanelGL*>(mainWindow->panel)->transferLayout(this);
}

void EmuThread::updateScreenSettings(bool filter, const WindowInfo& windowInfo, int numScreens, int* screenKind, float* screenMatrix)
{
    screenSettingsLock.lock();

    if (lastScreenWidth != windowInfo.surface_width || lastScreenHeight != windowInfo.surface_height)
    {
        if (oglContext)
            oglContext->ResizeSurface(windowInfo.surface_width, windowInfo.surface_height);
        lastScreenWidth = windowInfo.surface_width;
        lastScreenHeight = windowInfo.surface_height;
    }

    this->filter = filter;
    this->windowInfo = windowInfo;
    this->numScreens = numScreens;
    memcpy(this->screenKind, screenKind, sizeof(int)*numScreens);
    memcpy(this->screenMatrix, screenMatrix, sizeof(float)*numScreens*6);

    screenSettingsLock.unlock();
}

void EmuThread::initOpenGL()
{
    GL::Context* windowctx = mainWindow->getOGLContext();

    oglContext = windowctx;
    oglContext->MakeCurrent();

    OpenGL::BuildShaderProgram(kScreenVS, kScreenFS, screenShaderProgram, "ScreenShader");
    GLuint pid = screenShaderProgram[2];
    glBindAttribLocation(pid, 0, "vPosition");
    glBindAttribLocation(pid, 1, "vTexcoord");
    glBindFragDataLocation(pid, 0, "oColor");

    OpenGL::LinkShaderProgram(screenShaderProgram);

    glUseProgram(pid);
    glUniform1i(glGetUniformLocation(pid, "ScreenTex"), 0);

    screenShaderScreenSizeULoc = glGetUniformLocation(pid, "uScreenSize");
    screenShaderTransformULoc = glGetUniformLocation(pid, "uTransform");

    // to prevent bleeding between both parts of the screen
    // with bilinear filtering enabled
    const int paddedHeight = 192*2+2;
    const float padPixels = 1.f / paddedHeight;

    const float vertices[] =
    {
        0.f,   0.f,    0.f, 0.f,
        0.f,   192.f,  0.f, 0.5f - padPixels,
        256.f, 192.f,  1.f, 0.5f - padPixels,
        0.f,   0.f,    0.f, 0.f,
        256.f, 192.f,  1.f, 0.5f - padPixels,
        256.f, 0.f,    1.f, 0.f,

        0.f,   0.f,    0.f, 0.5f + padPixels,
        0.f,   192.f,  0.f, 1.f,
        256.f, 192.f,  1.f, 1.f,
        0.f,   0.f,    0.f, 0.5f + padPixels,
        256.f, 192.f,  1.f, 1.f,
        256.f, 0.f,    1.f, 0.5f + padPixels
    };

    glGenBuffers(1, &screenVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, screenVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &screenVertexArray);
    glBindVertexArray(screenVertexArray);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(0));
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(2*4));

    glGenTextures(1, &screenTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screenTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, paddedHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    // fill the padding
    u8 zeroData[256*4*4];
    memset(zeroData, 0, sizeof(zeroData));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192, 256, 2, GL_RGBA, GL_UNSIGNED_BYTE, zeroData);

    OSD::Init(true);

    oglContext->SetSwapInterval(Config::ScreenVSync ? Config::ScreenVSyncInterval : 0);
}

void EmuThread::deinitOpenGL()
{
    glDeleteTextures(1, &screenTexture);

    glDeleteVertexArrays(1, &screenVertexArray);
    glDeleteBuffers(1, &screenVertexBuffer);

    OpenGL::DeleteShaderProgram(screenShaderProgram);

    OSD::DeInit();

    oglContext->DoneCurrent();
    oglContext = nullptr;

    lastScreenWidth = lastScreenHeight = -1;
}

void EmuThread::run()
{
    u32 mainScreenPos[3];

    NDS::Init();

    mainScreenPos[0] = 0;
    mainScreenPos[1] = 0;
    mainScreenPos[2] = 0;
    autoScreenSizing = 0;

    videoSettingsDirty = false;
    videoSettings.Soft_Threaded = Config::Threaded3D != 0;
    videoSettings.GL_ScaleFactor = Config::GL_ScaleFactor;
    videoSettings.GL_BetterPolygons = Config::GL_BetterPolygons;

    if (mainWindow->hasOGL)
    {
        initOpenGL();
        videoRenderer = Config::_3DRenderer;
    }
    else
    {
        videoRenderer = 0;
    }

    GPU::InitRenderer(videoRenderer);
    GPU::SetRenderSettings(videoRenderer, videoSettings);

    SPU::SetInterpolation(Config::AudioInterp);

    Input::Init();

    u32 nframes = 0;
    double perfCountsSec = 1.0 / SDL_GetPerformanceFrequency();
    double lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
    double frameLimitError = 0.0;
    double lastMeasureTime = lastTime;

    u32 winUpdateCount = 0, winUpdateFreq = 1;

    char melontitle[100];

    while (EmuRunning != 0)
    {
        Input::Process();

        if (Input::HotkeyPressed(HK_FastForwardToggle)) emit windowLimitFPSChange();

        if (Input::HotkeyPressed(HK_Pause)) emit windowEmuPause();
        if (Input::HotkeyPressed(HK_Reset)) emit windowEmuReset();
        if (Input::HotkeyPressed(HK_FrameStep)) emit windowEmuFrameStep();

        if (Input::HotkeyPressed(HK_FullscreenToggle)) emit windowFullscreenToggle();

        if (Input::HotkeyPressed(HK_SwapScreens)) emit swapScreensToggle();

        if (Input::HotkeyPressed(HK_SolarSensorDecrease))
        {
            int level = GBACart::SetInput(GBACart::Input_SolarSensorDown, true);
            if (level != -1)
            {
                char msg[64];
                sprintf(msg, "Solar sensor level: %d", level);
                OSD::AddMessage(0, msg);
            }
        }
        if (Input::HotkeyPressed(HK_SolarSensorIncrease))
        {
            int level = GBACart::SetInput(GBACart::Input_SolarSensorUp, true);
            if (level != -1)
            {
                char msg[64];
                sprintf(msg, "Solar sensor level: %d", level);
                OSD::AddMessage(0, msg);
            }
        }

        if (EmuRunning == 1 || EmuRunning == 3)
        {
            EmuStatus = 1;
            if (EmuRunning == 3) EmuRunning = 2;

            // update render settings if needed
            // HACK:
            // once the fast forward hotkey is released, we need to update vsync
            // to the old setting again
            if (videoSettingsDirty || Input::HotkeyReleased(HK_FastForward))
            {
                if (oglContext)
                {
                    oglContext->SetSwapInterval(Config::ScreenVSync ? Config::ScreenVSyncInterval : 0);
                    videoRenderer = Config::_3DRenderer;
                }
#ifdef OGLRENDERER_ENABLED
                else
#endif
                {
                    videoRenderer = 0;
                }                

                videoRenderer = oglContext ? Config::_3DRenderer : 0;

                videoSettingsDirty = false;

                videoSettings.Soft_Threaded = Config::Threaded3D != 0;
                videoSettings.GL_ScaleFactor = Config::GL_ScaleFactor;
                videoSettings.GL_BetterPolygons = Config::GL_BetterPolygons;

                GPU::SetRenderSettings(videoRenderer, videoSettings);
            }

            // process input and hotkeys
            NDS::SetKeyMask(Input::InputMask);

            if (Input::HotkeyPressed(HK_Lid))
            {
                bool lid = !NDS::IsLidClosed();
                NDS::SetLidClosed(lid);
                OSD::AddMessage(0, lid ? "Lid closed" : "Lid opened");
            }

            // microphone input
            micProcess();

            // auto screen layout
            if (Config::ScreenSizing == screenSizing_Auto)
            {
                mainScreenPos[2] = mainScreenPos[1];
                mainScreenPos[1] = mainScreenPos[0];
                mainScreenPos[0] = NDS::PowerControl9 >> 15;

                int guess;
                if (mainScreenPos[0] == mainScreenPos[2] &&
                    mainScreenPos[0] != mainScreenPos[1])
                {
                    // constant flickering, likely displaying 3D on both screens
                    // TODO: when both screens are used for 2D only...???
                    guess = screenSizing_Even;
                }
                else
                {
                    if (mainScreenPos[0] == 1)
                        guess = screenSizing_EmphTop;
                    else
                        guess = screenSizing_EmphBot;
                }

                if (guess != autoScreenSizing)
                {
                    autoScreenSizing = guess;
                    emit screenLayoutChange();
                }
            }


            // emulate
            u32 nlines = NDS::RunFrame();

            if (ROMManager::NDSSave)
                ROMManager::NDSSave->CheckFlush();

            if (ROMManager::GBASave)
                ROMManager::GBASave->CheckFlush();

            if (!oglContext)
            {
                FrontBufferLock.lock();
                FrontBuffer = GPU::FrontBuffer;
                FrontBufferLock.unlock();
            }
            else
            {
                FrontBuffer = GPU::FrontBuffer;
                drawScreenGL();
            }

#ifdef MELONCAP
            MelonCap::Update();
#endif // MELONCAP

            if (EmuRunning == 0) break;

            winUpdateCount++;
            if (winUpdateCount >= winUpdateFreq && !oglContext)
            {
                emit windowUpdate();
                winUpdateCount = 0;
            }

            bool fastforward = Input::HotkeyDown(HK_FastForward);

            if (fastforward && oglContext && Config::ScreenVSync)
            {
                oglContext->SetSwapInterval(0);
            }

            if (Config::AudioSync && !fastforward && audioDevice)
            {
                SDL_LockMutex(audioSyncLock);
                while (SPU::GetOutputSize() > 1024)
                {
                    int ret = SDL_CondWaitTimeout(audioSync, audioSyncLock, 500);
                    if (ret == SDL_MUTEX_TIMEDOUT) break;
                }
                SDL_UnlockMutex(audioSyncLock);
            }

            double frametimeStep = nlines / (60.0 * 263.0);

            {
                bool limitfps = Config::LimitFPS && !fastforward;

                double practicalFramelimit = limitfps ? frametimeStep : 1.0 / 1000.0;

                double curtime = SDL_GetPerformanceCounter() * perfCountsSec;

                frameLimitError += practicalFramelimit - (curtime - lastTime);
                if (frameLimitError < -practicalFramelimit)
                    frameLimitError = -practicalFramelimit;
                if (frameLimitError > practicalFramelimit)
                    frameLimitError = practicalFramelimit;

                if (round(frameLimitError * 1000.0) > 0.0)
                {
                    SDL_Delay(round(frameLimitError * 1000.0));
                    double timeBeforeSleep = curtime;
                    curtime = SDL_GetPerformanceCounter() * perfCountsSec;
                    frameLimitError -= curtime - timeBeforeSleep;
                }

                lastTime = curtime;
            }

            nframes++;
            if (nframes >= 30)
            {
                double time = SDL_GetPerformanceCounter() * perfCountsSec;
                double dt = time - lastMeasureTime;
                lastMeasureTime = time;

                u32 fps = round(nframes / dt);
                nframes = 0;

                float fpstarget = 1.0/frametimeStep;

                winUpdateFreq = fps / (u32)round(fpstarget);
                if (winUpdateFreq < 1)
                    winUpdateFreq = 1;

                int inst = Platform::InstanceID();
                if (inst == 0)
                    sprintf(melontitle, "[%d/%.0f] melonDS " MELONDS_VERSION, fps, fpstarget);
                else
                    sprintf(melontitle, "[%d/%.0f] melonDS (%d)", fps, fpstarget, inst+1);
                changeWindowTitle(melontitle);
            }
        }
        else
        {
            // paused
            nframes = 0;
            lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
            lastMeasureTime = lastTime;

            emit windowUpdate();

            EmuStatus = EmuRunning;

            int inst = Platform::InstanceID();
            if (inst == 0)
                sprintf(melontitle, "melonDS " MELONDS_VERSION);
            else
                sprintf(melontitle, "melonDS (%d)", inst+1);
            changeWindowTitle(melontitle);

            SDL_Delay(75);

            if (oglContext)
                drawScreenGL();

            int contextRequest = ContextRequest;
            if (contextRequest == 1)
            {
                initOpenGL();
                ContextRequest = 0;
            }
            else if (contextRequest == 2)
            {
                deinitOpenGL();
                ContextRequest = 0;
            }
        }
    }

    EmuStatus = 0;

    GPU::DeInitRenderer();
    NDS::DeInit();
    //Platform::LAN_DeInit();
}

void EmuThread::changeWindowTitle(char* title)
{
    emit windowTitleChange(QString(title));
}

void EmuThread::emuRun()
{
    EmuRunning = 1;
    EmuPause = 0;
    RunningSomething = true;

    // checkme
    emit windowEmuStart();
    if (audioDevice) SDL_PauseAudioDevice(audioDevice, 0);
    micOpen();
}

void EmuThread::initContext()
{
    ContextRequest = 1;
    while (ContextRequest != 0);
}

void EmuThread::deinitContext()
{
    ContextRequest = 2;
    while (ContextRequest != 0);
}

void EmuThread::emuPause()
{
    EmuPause++;
    if (EmuPause > 1) return;

    PrevEmuStatus = EmuRunning;
    EmuRunning = 2;
    while (EmuStatus != 2);

    if (audioDevice) SDL_PauseAudioDevice(audioDevice, 1);
    micClose();
}

void EmuThread::emuUnpause()
{
    if (EmuPause < 1) return;

    EmuPause--;
    if (EmuPause > 0) return;

    EmuRunning = PrevEmuStatus;

    if (audioDevice) SDL_PauseAudioDevice(audioDevice, 0);
    micOpen();
}

void EmuThread::emuStop()
{
    EmuRunning = 0;
    EmuPause = 0;

    if (audioDevice) SDL_PauseAudioDevice(audioDevice, 1);
    micClose();
}

void EmuThread::emuFrameStep()
{
    if (EmuPause < 1) emit windowEmuPause();
    EmuRunning = 3;
}

bool EmuThread::emuIsRunning()
{
    return (EmuRunning == 1);
}

bool EmuThread::emuIsActive()
{
    return (RunningSomething == 1);
}

void EmuThread::drawScreenGL()
{
    int w = windowInfo.surface_width;
    int h = windowInfo.surface_height;
    float factor = windowInfo.surface_scale;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(false);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(0, 0, w, h);

    glUseProgram(screenShaderProgram[2]);
    glUniform2f(screenShaderScreenSizeULoc, w / factor, h / factor);

    int frontbuf = FrontBuffer;
    glActiveTexture(GL_TEXTURE0);

#ifdef OGLRENDERER_ENABLED
    if (GPU::Renderer != 0)
    {
        // hardware-accelerated render
        GPU::CurGLCompositor->BindOutputTexture(frontbuf);
    }
    else
#endif
    {
        // regular render
        glBindTexture(GL_TEXTURE_2D, screenTexture);

        if (GPU::Framebuffer[frontbuf][0] && GPU::Framebuffer[frontbuf][1])
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA,
                            GL_UNSIGNED_BYTE, GPU::Framebuffer[frontbuf][0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192+2, 256, 192, GL_RGBA,
                            GL_UNSIGNED_BYTE, GPU::Framebuffer[frontbuf][1]);
        }
    }

    screenSettingsLock.lock();

    GLint filter = this->filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    glBindBuffer(GL_ARRAY_BUFFER, screenVertexBuffer);
    glBindVertexArray(screenVertexArray);

    for (int i = 0; i < numScreens; i++)
    {
        glUniformMatrix2x3fv(screenShaderTransformULoc, 1, GL_TRUE, screenMatrix[i]);
        glDrawArrays(GL_TRIANGLES, screenKind[i] == 0 ? 0 : 2*3, 2*3);
    }

    screenSettingsLock.unlock();

    OSD::Update();
    OSD::DrawGL(w, h);

    oglContext->SwapBuffers();
}

ScreenHandler::ScreenHandler(QWidget* widget)
{
    widget->setMouseTracking(true);
    widget->setAttribute(Qt::WA_AcceptTouchEvents);
    QTimer* mouseTimer = setupMouseTimer();
    widget->connect(mouseTimer, &QTimer::timeout, [=] { if (Config::MouseHide) widget->setCursor(Qt::BlankCursor);});
}

ScreenHandler::~ScreenHandler()
{
    mouseTimer->stop();
}

void ScreenHandler::screenSetupLayout(int w, int h)
{
    int sizing = Config::ScreenSizing;
    if (sizing == 3) sizing = autoScreenSizing;

    float aspectTop, aspectBot;

    for (auto ratio : aspectRatios)
    {
        if (ratio.id == Config::ScreenAspectTop)
            aspectTop = ratio.ratio;
        if (ratio.id == Config::ScreenAspectBot)
            aspectBot = ratio.ratio;
    }

    if (aspectTop == 0)
        aspectTop = (float) w / h;

    if (aspectBot == 0)
        aspectBot = (float) w / h;

    Frontend::SetupScreenLayout(w, h,
                                Config::ScreenLayout,
                                Config::ScreenRotation,
                                sizing,
                                Config::ScreenGap,
                                Config::IntegerScaling != 0,
                                Config::ScreenSwap != 0,
                                aspectTop,
                                aspectBot);

    numScreens = Frontend::GetScreenTransforms(screenMatrix[0], screenKind);
}

QSize ScreenHandler::screenGetMinSize(int factor = 1)
{
    bool isHori = (Config::ScreenRotation == 1 || Config::ScreenRotation == 3);
    int gap = Config::ScreenGap * factor;

    int w = 256 * factor;
    int h = 192 * factor;

    if (Config::ScreenSizing == 4 || Config::ScreenSizing == 5)
    {
        return QSize(w, h);
    }

    if (Config::ScreenLayout == 0) // natural
    {
        if (isHori)
            return QSize(h+gap+h, w);
        else
            return QSize(w, h+gap+h);
    }
    else if (Config::ScreenLayout == 1) // vertical
    {
        if (isHori)
            return QSize(h, w+gap+w);
        else
            return QSize(w, h+gap+h);
    }
    else if (Config::ScreenLayout == 2) // horizontal
    {
        if (isHori)
            return QSize(h+gap+h, w);
        else
            return QSize(w+gap+w, h);
    }
    else // hybrid
    {
        if (isHori)
            return QSize(h+gap+h, 3*w + (int)ceil((4*gap) / 3.0));
        else
            return QSize(3*w + (int)ceil((4*gap) / 3.0), h+gap+h);
    }
}

void ScreenHandler::screenOnMousePress(QMouseEvent* event)
{
    event->accept();
    if (event->button() != Qt::LeftButton) return;

    int x = event->pos().x();
    int y = event->pos().y();

    if (Frontend::GetTouchCoords(x, y, false))
    {
        touching = true;
        NDS::TouchScreen(x, y);
    }
}

void ScreenHandler::screenOnMouseRelease(QMouseEvent* event)
{
    event->accept();
    if (event->button() != Qt::LeftButton) return;

    if (touching)
    {
        touching = false;
        NDS::ReleaseScreen();
    }
}

void ScreenHandler::screenOnMouseMove(QMouseEvent* event)
{
    event->accept();

    showCursor();

    if (!(event->buttons() & Qt::LeftButton)) return;
    if (!touching) return;

    int x = event->pos().x();
    int y = event->pos().y();

    if (Frontend::GetTouchCoords(x, y, true))
        NDS::TouchScreen(x, y);
}

void ScreenHandler::screenHandleTablet(QTabletEvent* event)
{
    event->accept();

    switch(event->type())
    {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
        {
            int x = event->x();
            int y = event->y();

            if (Frontend::GetTouchCoords(x, y, event->type()==QEvent::TabletMove))
            {
                touching = true;
                NDS::TouchScreen(x, y);
            }
        }
        break;
    case QEvent::TabletRelease:
        if (touching)
        {
            NDS::ReleaseScreen();
            touching = false;
        }
        break;
    }
}

void ScreenHandler::screenHandleTouch(QTouchEvent* event)
{
    event->accept();

    switch(event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        if (event->touchPoints().length() > 0)
        {
            QPointF lastPosition = event->touchPoints().first().lastPos();
            int x = (int)lastPosition.x();
            int y = (int)lastPosition.y();

            if (Frontend::GetTouchCoords(x, y, event->type()==QEvent::TouchUpdate))
            {
                touching = true;
                NDS::TouchScreen(x, y);
            }
        }
        break;
    case QEvent::TouchEnd:
        if (touching)
        {
            NDS::ReleaseScreen();
            touching = false;
        }
        break;
    }
}

void ScreenHandler::showCursor()
{
    mainWindow->panelWidget->setCursor(Qt::ArrowCursor);
    mouseTimer->start();
}

QTimer* ScreenHandler::setupMouseTimer()
{
    mouseTimer = new QTimer();
    mouseTimer->setSingleShot(true);
    mouseTimer->setInterval(Config::MouseHideSeconds*1000);
    mouseTimer->start();

    return mouseTimer;
}

ScreenPanelNative::ScreenPanelNative(QWidget* parent) : QWidget(parent), ScreenHandler(this)
{
    screen[0] = QImage(256, 192, QImage::Format_RGB32);
    screen[1] = QImage(256, 192, QImage::Format_RGB32);

    screenTrans[0].reset();
    screenTrans[1].reset();

    OSD::Init(false);
}

ScreenPanelNative::~ScreenPanelNative()
{
    OSD::DeInit();
}

void ScreenPanelNative::setupScreenLayout()
{
    int w = width();
    int h = height();

    screenSetupLayout(w, h);

    for (int i = 0; i < numScreens; i++)
    {
        float* mtx = screenMatrix[i];
        screenTrans[i].setMatrix(mtx[0], mtx[1], 0.f,
                                mtx[2], mtx[3], 0.f,
                                mtx[4], mtx[5], 1.f);
    }
}

void ScreenPanelNative::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);

    // fill background
    painter.fillRect(event->rect(), QColor::fromRgb(0, 0, 0));

    if (emuThread->emuIsActive())
    {
        emuThread->FrontBufferLock.lock();
        int frontbuf = emuThread->FrontBuffer;
        if (!GPU::Framebuffer[frontbuf][0] || !GPU::Framebuffer[frontbuf][1])
        {
            emuThread->FrontBufferLock.unlock();
            return;
        }

        memcpy(screen[0].scanLine(0), GPU::Framebuffer[frontbuf][0], 256 * 192 * 4);
        memcpy(screen[1].scanLine(0), GPU::Framebuffer[frontbuf][1], 256 * 192 * 4);
        emuThread->FrontBufferLock.unlock();

        painter.setRenderHint(QPainter::SmoothPixmapTransform, Config::ScreenFilter != 0);

        QRect screenrc(0, 0, 256, 192);

        for (int i = 0; i < numScreens; i++)
        {
            painter.setTransform(screenTrans[i]);
            painter.drawImage(screenrc, screen[screenKind[i]]);
        }
    }

    OSD::Update();
    OSD::DrawNative(painter);
}

void ScreenPanelNative::resizeEvent(QResizeEvent* event)
{
    setupScreenLayout();
}

void ScreenPanelNative::mousePressEvent(QMouseEvent* event)
{
    screenOnMousePress(event);
}

void ScreenPanelNative::mouseReleaseEvent(QMouseEvent* event)
{
    screenOnMouseRelease(event);
}

void ScreenPanelNative::mouseMoveEvent(QMouseEvent* event)
{
    screenOnMouseMove(event);
}

void ScreenPanelNative::tabletEvent(QTabletEvent* event)
{
    screenHandleTablet(event);
}

bool ScreenPanelNative::event(QEvent* event)
{
    if (event->type() == QEvent::TouchBegin
        || event->type() == QEvent::TouchEnd
        || event->type() == QEvent::TouchUpdate)
    {
        screenHandleTouch((QTouchEvent*)event);
        return true;
    }
    return QWidget::event(event);
}

void ScreenPanelNative::onScreenLayoutChanged()
{
    setMinimumSize(screenGetMinSize());
    setupScreenLayout();
}


ScreenPanelGL::ScreenPanelGL(QWidget* parent) : QWidget(parent), ScreenHandler(this)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_KeyCompression, false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(screenGetMinSize());
}

ScreenPanelGL::~ScreenPanelGL()
{}

bool ScreenPanelGL::createContext()
{
    std::optional<WindowInfo> windowInfo = getWindowInfo();
    std::array<GL::Context::Version, 2> versionsToTry = {
        GL::Context::Version{GL::Context::Profile::Core, 4, 3},
        GL::Context::Version{GL::Context::Profile::Core, 3, 2}};
    if (windowInfo.has_value())
    {
        glContext = GL::Context::Create(*getWindowInfo(), versionsToTry);
        glContext->DoneCurrent();
    }

    return glContext != nullptr;
}

qreal ScreenPanelGL::devicePixelRatioFromScreen() const
{
    const QScreen* screen_for_ratio = window()->windowHandle()->screen();
    if (!screen_for_ratio)
        screen_for_ratio = QGuiApplication::primaryScreen();

    return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

int ScreenPanelGL::scaledWindowWidth() const
{
  return std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * devicePixelRatioFromScreen())), 1);
}

int ScreenPanelGL::scaledWindowHeight() const
{
  return std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * devicePixelRatioFromScreen())), 1);
}

std::optional<WindowInfo> ScreenPanelGL::getWindowInfo()
{
    WindowInfo wi;

    // Windows and Apple are easy here since there's no display connection.
    #if defined(_WIN32)
    wi.type = WindowInfo::Type::Win32;
    wi.window_handle = reinterpret_cast<void*>(winId());
    #elif defined(__APPLE__)
    wi.type = WindowInfo::Type::MacOS;
    wi.window_handle = reinterpret_cast<void*>(winId());
    #else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    const QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("xcb"))
    {
        wi.type = WindowInfo::Type::X11;
        wi.display_connection = pni->nativeResourceForWindow("display", windowHandle());
        wi.window_handle = reinterpret_cast<void*>(winId());
    }
    else if (platform_name == QStringLiteral("wayland"))
    {
        wi.type = WindowInfo::Type::Wayland;
        QWindow* handle = windowHandle();
        if (handle == nullptr)
            return std::nullopt;

        wi.display_connection = pni->nativeResourceForWindow("display", handle);
        wi.window_handle = pni->nativeResourceForWindow("surface", handle);
    }
    else
    {
        qCritical() << "Unknown PNI platform " << platform_name;
        return std::nullopt;
    }
    #endif

    wi.surface_width = static_cast<u32>(scaledWindowWidth());
    wi.surface_height = static_cast<u32>(scaledWindowHeight());
    wi.surface_scale = static_cast<float>(devicePixelRatioFromScreen());

    return wi;
}


QPaintEngine* ScreenPanelGL::paintEngine() const
{
  return nullptr;
}

void ScreenPanelGL::setupScreenLayout()
{
    int w = width();
    int h = height();

    screenSetupLayout(w, h);
    if (emuThread)
        transferLayout(emuThread);
}

void ScreenPanelGL::resizeEvent(QResizeEvent* event)
{
    setupScreenLayout();

    QWidget::resizeEvent(event);
}

void ScreenPanelGL::mousePressEvent(QMouseEvent* event)
{
    screenOnMousePress(event);
}

void ScreenPanelGL::mouseReleaseEvent(QMouseEvent* event)
{
    screenOnMouseRelease(event);
}

void ScreenPanelGL::mouseMoveEvent(QMouseEvent* event)
{
    screenOnMouseMove(event);
}

void ScreenPanelGL::tabletEvent(QTabletEvent* event)
{
    screenHandleTablet(event);
}

bool ScreenPanelGL::event(QEvent* event)
{
    if (event->type() == QEvent::TouchBegin
        || event->type() == QEvent::TouchEnd
        || event->type() == QEvent::TouchUpdate)
    {
        screenHandleTouch((QTouchEvent*)event);
        return true;
    }
    return QWidget::event(event);
}

void ScreenPanelGL::transferLayout(EmuThread* thread)
{
    std::optional<WindowInfo> windowInfo = getWindowInfo();
    if (windowInfo.has_value())
        thread->updateScreenSettings(Config::ScreenFilter, *windowInfo, numScreens, screenKind, &screenMatrix[0][0]);
}

void ScreenPanelGL::onScreenLayoutChanged()
{
    setMinimumSize(screenGetMinSize());
    setupScreenLayout();
}

#ifndef _WIN32
static int signalFd[2];
QSocketNotifier *signalSn;

static void signalHandler(int)
{
    char a = 1;
    write(signalFd[0], &a, sizeof(a));
}
#endif

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
#ifndef _WIN32
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalFd))
    {
       qFatal("Couldn't create socketpair");
    }

    signalSn = new QSocketNotifier(signalFd[1], QSocketNotifier::Read, this);
    connect(signalSn, SIGNAL(activated(int)), this, SLOT(onQuit()));

    struct sigaction sa;

    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_flags |= SA_RESTART;
    sigaction(SIGINT, &sa, 0);
#endif

    oldW = Config::WindowWidth;
    oldH = Config::WindowHeight;
    oldMax = Config::WindowMaximized;

    setWindowTitle("melonDS " MELONDS_VERSION);
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);

    int inst = Platform::InstanceID();

    QMenuBar* menubar = new QMenuBar();
    {
        QMenu* menu = menubar->addMenu("文件");

        actOpenROM = menu->addAction("打开ROM...");
        connect(actOpenROM, &QAction::triggered, this, &MainWindow::onOpenFile);
        actOpenROM->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));

        /*actOpenROMArchive = menu->addAction("Open ROM inside archive...");
        connect(actOpenROMArchive, &QAction::triggered, this, &MainWindow::onOpenFileArchive);
        actOpenROMArchive->setShortcut(QKeySequence(Qt::Key_O | Qt::CTRL | Qt::SHIFT));*/

        recentMenu = menu->addMenu("最近打开");
        for (int i = 0; i < 10; ++i)
        {
            std::string item = Config::RecentROMList[i];
            if (!item.empty())
                recentFileList.push_back(QString::fromStdString(item));
        }
        updateRecentFilesMenu();

        //actBootFirmware = menu->addAction("Launch DS menu");
        actBootFirmware = menu->addAction("启动固件");
        connect(actBootFirmware, &QAction::triggered, this, &MainWindow::onBootFirmware);

        menu->addSeparator();

        actCurrentCart = menu->addAction("DS槽位： " + ROMManager::CartLabel());
        actCurrentCart->setEnabled(false);

        actInsertCart = menu->addAction("插入卡带...");
        connect(actInsertCart, &QAction::triggered, this, &MainWindow::onInsertCart);

        actEjectCart = menu->addAction("弹出卡带");
        connect(actEjectCart, &QAction::triggered, this, &MainWindow::onEjectCart);

        menu->addSeparator();

        actCurrentGBACart = menu->addAction("GBA槽位: " + ROMManager::GBACartLabel());
        actCurrentGBACart->setEnabled(false);

        actInsertGBACart = menu->addAction("插入GBA卡带...");
        connect(actInsertGBACart, &QAction::triggered, this, &MainWindow::onInsertGBACart);

        {
            QMenu* submenu = menu->addMenu("插入拓展卡带");

            actInsertGBAAddon[0] = submenu->addAction("内存拓展卡");
            actInsertGBAAddon[0]->setData(QVariant(NDS::GBAAddon_RAMExpansion));
            connect(actInsertGBAAddon[0], &QAction::triggered, this, &MainWindow::onInsertGBAAddon);
        }

        actEjectGBACart = menu->addAction("弹出卡带");
        connect(actEjectGBACart, &QAction::triggered, this, &MainWindow::onEjectGBACart);

        menu->addSeparator();

        actImportSavefile = menu->addAction("导入存档");
        connect(actImportSavefile, &QAction::triggered, this, &MainWindow::onImportSavefile);

        menu->addSeparator();

        {
            QMenu* submenu = menu->addMenu("即时存档");

            for (int i = 1; i < 9; i++)
            {
                actSaveState[i] = submenu->addAction(QString("%1").arg(i));
                actSaveState[i]->setShortcut(QKeySequence(Qt::ShiftModifier | (Qt::Key_F1+i-1)));
                actSaveState[i]->setData(QVariant(i));
                connect(actSaveState[i], &QAction::triggered, this, &MainWindow::onSaveState);
            }

            actSaveState[0] = submenu->addAction("文件...");
            actSaveState[0]->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_F9));
            actSaveState[0]->setData(QVariant(0));
            connect(actSaveState[0], &QAction::triggered, this, &MainWindow::onSaveState);
        }
        {
            QMenu* submenu = menu->addMenu("即时读档");

            for (int i = 1; i < 9; i++)
            {
                actLoadState[i] = submenu->addAction(QString("%1").arg(i));
                actLoadState[i]->setShortcut(QKeySequence(Qt::Key_F1+i-1));
                actLoadState[i]->setData(QVariant(i));
                connect(actLoadState[i], &QAction::triggered, this, &MainWindow::onLoadState);
            }

            actLoadState[0] = submenu->addAction("文件...");
            actLoadState[0]->setShortcut(QKeySequence(Qt::Key_F9));
            actLoadState[0]->setData(QVariant(0));
            connect(actLoadState[0], &QAction::triggered, this, &MainWindow::onLoadState);
        }

        actUndoStateLoad = menu->addAction("撤销即时读档");
        actUndoStateLoad->setShortcut(QKeySequence(Qt::Key_F12));
        connect(actUndoStateLoad, &QAction::triggered, this, &MainWindow::onUndoStateLoad);

        menu->addSeparator();

        actQuit = menu->addAction("退出");
        connect(actQuit, &QAction::triggered, this, &MainWindow::onQuit);
    }
    {
        QMenu* menu = menubar->addMenu("系统");

        actPause = menu->addAction("暂停");
        actPause->setCheckable(true);
        connect(actPause, &QAction::triggered, this, &MainWindow::onPause);

        actReset = menu->addAction("重启");
        connect(actReset, &QAction::triggered, this, &MainWindow::onReset);

        actStop = menu->addAction("停止");
        connect(actStop, &QAction::triggered, this, &MainWindow::onStop);

        actFrameStep = menu->addAction("帧步进");
        connect(actFrameStep, &QAction::triggered, this, &MainWindow::onFrameStep);

        menu->addSeparator();

        actPowerManagement = menu->addAction("模拟电源管理");
        connect(actPowerManagement, &QAction::triggered, this, &MainWindow::onOpenPowerManagement);

        menu->addSeparator();

        actEnableCheats = menu->addAction("开启金手指（慎用）");
        actEnableCheats->setCheckable(true);
        connect(actEnableCheats, &QAction::triggered, this, &MainWindow::onEnableCheats);

        //if (inst == 0)
        {
            actSetupCheats = menu->addAction("设置金手指代码");
            actSetupCheats->setMenuRole(QAction::NoRole);
            connect(actSetupCheats, &QAction::triggered, this, &MainWindow::onSetupCheats);

            menu->addSeparator();
            actROMInfo = menu->addAction("ROM信息");
            connect(actROMInfo, &QAction::triggered, this, &MainWindow::onROMInfo);

            actRAMInfo = menu->addAction("内存(RAM)搜索");
            connect(actRAMInfo, &QAction::triggered, this, &MainWindow::onRAMInfo);

            actTitleManager = menu->addAction("管理DSi应用");
            connect(actTitleManager, &QAction::triggered, this, &MainWindow::onOpenTitleManager);
        }

        {
            menu->addSeparator();
            QMenu* submenu = menu->addMenu("多人联机");

            actMPNewInstance = submenu->addAction("启动新窗口");
            connect(actMPNewInstance, &QAction::triggered, this, &MainWindow::onMPNewInstance);
        }
    }
    {
        QMenu* menu = menubar->addMenu("设置");

        actEmuSettings = menu->addAction("模拟设置");
        connect(actEmuSettings, &QAction::triggered, this, &MainWindow::onOpenEmuSettings);

#ifdef __APPLE__
        actPreferences = menu->addAction("偏好...");
        connect(actPreferences, &QAction::triggered, this, &MainWindow::onOpenEmuSettings);
        actPreferences->setMenuRole(QAction::PreferencesRole);
#endif

        actInputConfig = menu->addAction("控制和热键");
        connect(actInputConfig, &QAction::triggered, this, &MainWindow::onOpenInputConfig);

        actVideoSettings = menu->addAction("视频设置");
        connect(actVideoSettings, &QAction::triggered, this, &MainWindow::onOpenVideoSettings);

        actCameraSettings = menu->addAction("相机设置");
        connect(actCameraSettings, &QAction::triggered, this, &MainWindow::onOpenCameraSettings);

        actAudioSettings = menu->addAction("声音设置");
        connect(actAudioSettings, &QAction::triggered, this, &MainWindow::onOpenAudioSettings);

        actMPSettings = menu->addAction("多人联机设置");
        connect(actMPSettings, &QAction::triggered, this, &MainWindow::onOpenMPSettings);

        actWifiSettings = menu->addAction("Wifi设置");
        connect(actWifiSettings, &QAction::triggered, this, &MainWindow::onOpenWifiSettings);

        actFirmwareSettings = menu->addAction("固件设置");
        connect(actFirmwareSettings, &QAction::triggered, this, &MainWindow::onOpenFirmwareSettings);

        actInterfaceSettings = menu->addAction("界面设置");
        connect(actInterfaceSettings, &QAction::triggered, this, &MainWindow::onOpenInterfaceSettings);

        actPathSettings = menu->addAction("路径设置");
        connect(actPathSettings, &QAction::triggered, this, &MainWindow::onOpenPathSettings);

        {
            QMenu* submenu = menu->addMenu("即时存档设置");

            actSavestateSRAMReloc = submenu->addAction("独立保存文件");
            actSavestateSRAMReloc->setCheckable(true);
            connect(actSavestateSRAMReloc, &QAction::triggered, this, &MainWindow::onChangeSavestateSRAMReloc);
        }

        menu->addSeparator();

        {
            QMenu* submenu = menu->addMenu("窗口大小");

            for (int i = 0; i < 4; i++)
            {
                int data = i+1;
                actScreenSize[i] = submenu->addAction(QString("%1x").arg(data));
                actScreenSize[i]->setData(QVariant(data));
                connect(actScreenSize[i], &QAction::triggered, this, &MainWindow::onChangeScreenSize);
            }
        }
        {
            QMenu* submenu = menu->addMenu("窗口旋转");
            grpScreenRotation = new QActionGroup(submenu);

            for (int i = 0; i < 4; i++)
            {
                int data = i*90;
                actScreenRotation[i] = submenu->addAction(QString("%1°").arg(data));
                actScreenRotation[i]->setActionGroup(grpScreenRotation);
                actScreenRotation[i]->setData(QVariant(i));
                actScreenRotation[i]->setCheckable(true);
            }

            connect(grpScreenRotation, &QActionGroup::triggered, this, &MainWindow::onChangeScreenRotation);
        }
        {
            QMenu* submenu = menu->addMenu("窗口间隔");
            grpScreenGap = new QActionGroup(submenu);

            const int screengap[] = {0, 1, 8, 64, 90, 128};

            for (int i = 0; i < 6; i++)
            {
                int data = screengap[i];
                actScreenGap[i] = submenu->addAction(QString("%1 px").arg(data));
                actScreenGap[i]->setActionGroup(grpScreenGap);
                actScreenGap[i]->setData(QVariant(data));
                actScreenGap[i]->setCheckable(true);
            }

            connect(grpScreenGap, &QActionGroup::triggered, this, &MainWindow::onChangeScreenGap);
        }
        {
            QMenu* submenu = menu->addMenu("窗口布局");
            grpScreenLayout = new QActionGroup(submenu);

            const char* screenlayout[] = {"通常", "竖直", "水平", "混合"};

            for (int i = 0; i < 4; i++)
            {
                actScreenLayout[i] = submenu->addAction(QString(screenlayout[i]));
                actScreenLayout[i]->setActionGroup(grpScreenLayout);
                actScreenLayout[i]->setData(QVariant(i));
                actScreenLayout[i]->setCheckable(true);
            }

            connect(grpScreenLayout, &QActionGroup::triggered, this, &MainWindow::onChangeScreenLayout);

            submenu->addSeparator();

            actScreenSwap = submenu->addAction("交换上下屏");
            actScreenSwap->setCheckable(true);
            connect(actScreenSwap, &QAction::triggered, this, &MainWindow::onChangeScreenSwap);
        }
        {
            QMenu* submenu = menu->addMenu("窗口显示");
            grpScreenSizing = new QActionGroup(submenu);

            const char* screensizing[] = {"正常", "上屏优先", "下屏优先", "自动", "仅上屏", "仅下屏"};

            for (int i = 0; i < screenSizing_MAX; i++)
            {
                actScreenSizing[i] = submenu->addAction(QString(screensizing[i]));
                actScreenSizing[i]->setActionGroup(grpScreenSizing);
                actScreenSizing[i]->setData(QVariant(i));
                actScreenSizing[i]->setCheckable(true);
            }

            connect(grpScreenSizing, &QActionGroup::triggered, this, &MainWindow::onChangeScreenSizing);

            submenu->addSeparator();

            actIntegerScaling = submenu->addAction("强制整数倍缩放");
            actIntegerScaling->setCheckable(true);
            connect(actIntegerScaling, &QAction::triggered, this, &MainWindow::onChangeIntegerScaling);
        }
        {
            QMenu* submenu = menu->addMenu("窗口横纵比");
            grpScreenAspectTop = new QActionGroup(submenu);
            grpScreenAspectBot = new QActionGroup(submenu);
            actScreenAspectTop = new QAction*[sizeof(aspectRatios) / sizeof(aspectRatios[0])];
            actScreenAspectBot = new QAction*[sizeof(aspectRatios) / sizeof(aspectRatios[0])];

            for (int i = 0; i < 2; i++)
            {
                QActionGroup* group = grpScreenAspectTop;
                QAction** actions = actScreenAspectTop;

                if (i == 1)
                {
                    group = grpScreenAspectBot;
                    submenu->addSeparator();
                    actions = actScreenAspectBot;
                }

                for (int j = 0; j < sizeof(aspectRatios) / sizeof(aspectRatios[0]); j++)
                {
                    auto ratio = aspectRatios[j];
                    QString label = QString("%1 %2").arg(i ? "下屏" : "上屏", ratio.label);
                    actions[j] = submenu->addAction(label);
                    actions[j]->setActionGroup(group);
                    actions[j]->setData(QVariant(ratio.id));
                    actions[j]->setCheckable(true);
                }

                connect(group, &QActionGroup::triggered, this, &MainWindow::onChangeScreenAspect);
            }
        }

        actScreenFiltering = menu->addAction("窗口滤镜");
        actScreenFiltering->setCheckable(true);
        connect(actScreenFiltering, &QAction::triggered, this, &MainWindow::onChangeScreenFiltering);

        actShowOSD = menu->addAction("显示OSD");
        actShowOSD->setCheckable(true);
        connect(actShowOSD, &QAction::triggered, this, &MainWindow::onChangeShowOSD);

        menu->addSeparator();

        actLimitFramerate = menu->addAction("限制帧数");
        actLimitFramerate->setCheckable(true);
        connect(actLimitFramerate, &QAction::triggered, this, &MainWindow::onChangeLimitFramerate);

        actAudioSync = menu->addAction("音频同步");
        actAudioSync->setCheckable(true);
        connect(actAudioSync, &QAction::triggered, this, &MainWindow::onChangeAudioSync);
    }
    setMenuBar(menubar);

    resize(Config::WindowWidth, Config::WindowHeight);

    if (Config::FirmwareUsername == "Arisotura")
        actMPNewInstance->setText("Fart");

#ifdef Q_OS_MAC
    QPoint screenCenter = screen()->availableGeometry().center();
    QRect frameGeo = frameGeometry();
    frameGeo.moveCenter(screenCenter);
    move(frameGeo.topLeft());
#endif

    if (oldMax)
        showMaximized();
    else
        show();

    createScreenPanel();

    actEjectCart->setEnabled(false);
    actEjectGBACart->setEnabled(false);

    if (Config::ConsoleType == 1)
    {
        actInsertGBACart->setEnabled(false);
        for (int i = 0; i < 1; i++)
            actInsertGBAAddon[i]->setEnabled(false);
    }

    for (int i = 0; i < 9; i++)
    {
        actSaveState[i]->setEnabled(false);
        actLoadState[i]->setEnabled(false);
    }
    actUndoStateLoad->setEnabled(false);
    actImportSavefile->setEnabled(false);

    actPause->setEnabled(false);
    actReset->setEnabled(false);
    actStop->setEnabled(false);
    actFrameStep->setEnabled(false);

    actPowerManagement->setEnabled(false);

    actSetupCheats->setEnabled(false);
    actTitleManager->setEnabled(!Config::DSiNANDPath.empty());

    actEnableCheats->setChecked(Config::EnableCheats);

    actROMInfo->setEnabled(false);
    actRAMInfo->setEnabled(false);

    actSavestateSRAMReloc->setChecked(Config::SavestateRelocSRAM);

    actScreenRotation[Config::ScreenRotation]->setChecked(true);

    for (int i = 0; i < 6; i++)
    {
        if (actScreenGap[i]->data().toInt() == Config::ScreenGap)
        {
            actScreenGap[i]->setChecked(true);
            break;
        }
    }

    actScreenLayout[Config::ScreenLayout]->setChecked(true);
    actScreenSizing[Config::ScreenSizing]->setChecked(true);
    actIntegerScaling->setChecked(Config::IntegerScaling);

    actScreenSwap->setChecked(Config::ScreenSwap);

    for (int i = 0; i < sizeof(aspectRatios) / sizeof(aspectRatios[0]); i++)
    {
        if (Config::ScreenAspectTop == aspectRatios[i].id)
            actScreenAspectTop[i]->setChecked(true);
        if (Config::ScreenAspectBot == aspectRatios[i].id)
            actScreenAspectBot[i]->setChecked(true);
    }

    actScreenFiltering->setChecked(Config::ScreenFilter);
    actShowOSD->setChecked(Config::ShowOSD);

    actLimitFramerate->setChecked(Config::LimitFPS);
    actAudioSync->setChecked(Config::AudioSync);

    if (inst > 0)
    {
        actEmuSettings->setEnabled(false);
        actVideoSettings->setEnabled(false);
        actMPSettings->setEnabled(false);
        actWifiSettings->setEnabled(false);
        actInterfaceSettings->setEnabled(false);

#ifdef __APPLE__
        actPreferences->setEnabled(false);
#endif // __APPLE__
    }
}

MainWindow::~MainWindow()
{
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (hasOGL)
    {
        // we intentionally don't unpause here
        emuThread->emuPause();
        emuThread->deinitContext();
    }

    QMainWindow::closeEvent(event);
}

void MainWindow::createScreenPanel()
{
    hasOGL = (Config::ScreenUseGL != 0) || (Config::_3DRenderer != 0);

    if (hasOGL)
    {
        ScreenPanelGL* panelGL = new ScreenPanelGL(this);
        panelGL->show();

        panel = panelGL;
        panelWidget = panelGL;

        panelGL->createContext();
    }

    if (!hasOGL)
    {
        ScreenPanelNative* panelNative = new ScreenPanelNative(this);
        panel = panelNative;
        panelWidget = panelNative;
        panelWidget->show();
    }
    setCentralWidget(panelWidget);

    connect(this, SIGNAL(screenLayoutChange()), panelWidget, SLOT(onScreenLayoutChanged()));
    emit screenLayoutChange();
}

GL::Context* MainWindow::getOGLContext()
{
    if (!hasOGL) return nullptr;

    ScreenPanelGL* glpanel = static_cast<ScreenPanelGL*>(panel);
    return glpanel->getContext();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    int w = event->size().width();
    int h = event->size().height();

    if (!isFullScreen())
    {
        // this is ugly
        // thing is, when maximizing the window, we first receive the resizeEvent
        // with a new size matching the screen, then the changeEvent telling us that
        // the maximized flag was updated
        oldW = Config::WindowWidth;
        oldH = Config::WindowHeight;
        oldMax = isMaximized();

        Config::WindowWidth = w;
        Config::WindowHeight = h;
    }
}

void MainWindow::changeEvent(QEvent* event)
{
    if (isMaximized() && !oldMax)
    {
        Config::WindowWidth = oldW;
        Config::WindowHeight = oldH;
    }

    Config::WindowMaximized = isMaximized() ? 1:0;
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) return;

    // TODO!! REMOVE ME IN RELEASE BUILDS!!
    //if (event->key() == Qt::Key_F11) NDS::debug(0);

    Input::KeyPress(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) return;

    Input::KeyRelease(event);
}


void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.count() > 1) return; // not handling more than one file at once

    QString filename = urls.at(0).toLocalFile();

    QStringList acceptedExts{".nds", ".srl", ".dsi", ".gba", ".rar",
                             ".zip", ".7z", ".tar", ".tar.gz", ".tar.xz", ".tar.bz2"};

    for (const QString &ext : acceptedExts)
    {
        if (filename.endsWith(ext, Qt::CaseInsensitive))
            event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.count() > 1) return; // not handling more than one file at once

    QString filename = urls.at(0).toLocalFile();
    QStringList arcexts{".zip", ".7z", ".rar", ".tar", ".tar.gz", ".tar.xz", ".tar.bz2"};

    emuThread->emuPause();

    if (!verifySetup())
    {
        emuThread->emuUnpause();
        return;
    }

    for (const QString &ext : arcexts)
    {
        if (filename.endsWith(ext, Qt::CaseInsensitive))
        {
            QString arcfile = pickFileFromArchive(filename);
            if (arcfile.isEmpty())
            {
                emuThread->emuUnpause();
                return;
            }

            filename += "|" + arcfile;
        }
    }

    QStringList file = filename.split('|');

    if (filename.endsWith(".gba", Qt::CaseInsensitive))
    {
        if (!ROMManager::LoadGBAROM(file))
        {
            // TODO: better error reporting?
            QMessageBox::critical(this, "melonDS", "加载ROM失败。");
            emuThread->emuUnpause();
            return;
        }

        emuThread->emuUnpause();

        updateCartInserted(true);
    }
    else
    {
        if (!ROMManager::LoadROM(file, true))
        {
            // TODO: better error reporting?
            QMessageBox::critical(this, "melonDS", "加载ROM失败。");
            emuThread->emuUnpause();
            return;
        }

        recentFileList.removeAll(filename);
        recentFileList.prepend(filename);
        updateRecentFilesMenu();

        NDS::Start();
        emuThread->emuRun();

        updateCartInserted(false);
    }
}

void MainWindow::focusInEvent(QFocusEvent* event)
{
    audioMute();
}

void MainWindow::focusOutEvent(QFocusEvent* event)
{
    audioMute();
}

void MainWindow::onAppStateChanged(Qt::ApplicationState state)
{
    if (state == Qt::ApplicationInactive)
    {
        if (Config::PauseLostFocus && emuThread->emuIsRunning())
            emuThread->emuPause();
    }
    else if (state == Qt::ApplicationActive)
    {
        if (Config::PauseLostFocus && !pausedManually)
            emuThread->emuUnpause();
    }
}

bool MainWindow::verifySetup()
{
    QString res = ROMManager::VerifySetup();
    if (!res.isEmpty())
    {
         QMessageBox::critical(this, "melonDS", res);
         return false;
    }

    return true;
}

bool MainWindow::preloadROMs(QStringList file, QStringList gbafile, bool boot)
{
    if (!verifySetup())
    {
        return false;
    }

    bool gbaloaded = false;
    if (!gbafile.isEmpty())
    {
        if (!ROMManager::LoadGBAROM(gbafile))
        {
            // TODO: better error reporting?
            QMessageBox::critical(this, "melonDS", "加载GBA ROM失败。");
            return false;
        }

        gbaloaded = true;
    }

    bool ndsloaded = false;
    if (!file.isEmpty())
    {
        if (!ROMManager::LoadROM(file, true))
        {
            // TODO: better error reporting?
            QMessageBox::critical(this, "melonDS", "加载ROM失败。");
            return false;
        }
        recentFileList.removeAll(file.join("|"));
        recentFileList.prepend(file.join("|"));
        updateRecentFilesMenu();
        ndsloaded = true;
    }

    if (boot)
    {
        if (ndsloaded)
        {
            NDS::Start();
            emuThread->emuRun();
        }
        else
        {
            onBootFirmware();
        }
    }

    updateCartInserted(false);

    if (gbaloaded)
    {
        updateCartInserted(true);
    }

    return true;
}

QString MainWindow::pickFileFromArchive(QString archiveFileName)
{
    QVector<QString> archiveROMList = Archive::ListArchive(archiveFileName);

    QString romFileName = ""; // file name inside archive

    if (archiveROMList.size() > 2)
    {
        archiveROMList.removeFirst();

        bool ok;
        QString toLoad = QInputDialog::getItem(this, "melonDS",
                                  "该档案包含多个文件。选择要加载的ROM。", archiveROMList.toList(), 0, false, &ok);
        if (!ok) // User clicked on cancel
            return QString();

        romFileName = toLoad;
    }
    else if (archiveROMList.size() == 2)
    {
        romFileName = archiveROMList.at(1);
    }
    else if ((archiveROMList.size() == 1) && (archiveROMList[0] == QString("OK")))
    {
        QMessageBox::warning(this, "melonDS", "该档案是空的。");
    }
    else
    {
        QMessageBox::critical(this, "melonDS", "无法读取此档案。它可能已损坏或您没有权限。");
    }

    return romFileName;
}

QStringList MainWindow::pickROM(bool gba)
{
    QString console;
    QStringList romexts;
    QStringList arcexts{"*.zip", "*.7z", "*.rar", "*.tar", "*.tar.gz", "*.tar.xz", "*.tar.bz2"};
    QStringList ret;

    if (gba)
    {
        console = "GBA";
        romexts.append("*.gba");
    }
    else
    {
        console = "DS";
        romexts.append({"*.nds", "*.dsi", "*.ids", "*.srl"});
    }

    QString filter = romexts.join(' ') + " " + arcexts.join(' ');
    filter = console + " ROMs (" + filter + ");;Any file (*.*)";

    QString filename = QFileDialog::getOpenFileName(this,
                                                    "Open "+console+" ROM",
                                                    QString::fromStdString(Config::LastROMFolder),
                                                    filter);
    if (filename.isEmpty())
        return ret;

    int pos = filename.length() - 1;
    while (filename[pos] != '/' && filename[pos] != '\\' && pos > 0) pos--;
    QString path_dir = filename.left(pos);
    QString path_file = filename.mid(pos+1);

    Config::LastROMFolder = path_dir.toStdString();

    bool isarc = false;
    for (const auto& ext : arcexts)
    {
        int l = ext.length() - 1;
        if (path_file.right(l).toLower() == ext.right(l))
        {
            isarc = true;
            break;
        }
    }

    if (isarc)
    {
        path_file = pickFileFromArchive(filename);
        if (path_file.isEmpty())
            return ret;

        ret.append(filename);
        ret.append(path_file);
    }
    else
    {
        ret.append(filename);
    }

    return ret;
}

void MainWindow::updateCartInserted(bool gba)
{
    bool inserted;
    if (gba)
    {
        inserted = ROMManager::GBACartInserted() && (Config::ConsoleType == 0);
        actCurrentGBACart->setText("GBA槽位：" + ROMManager::GBACartLabel());
        actEjectGBACart->setEnabled(inserted);
    }
    else
    {
        inserted = ROMManager::CartInserted();
        actCurrentCart->setText("DS槽位：" + ROMManager::CartLabel());
        actEjectCart->setEnabled(inserted);
        actImportSavefile->setEnabled(inserted);
        actSetupCheats->setEnabled(inserted);
        actROMInfo->setEnabled(inserted);
        actRAMInfo->setEnabled(inserted);
    }
}

void MainWindow::onOpenFile()
{
    emuThread->emuPause();

    if (!verifySetup())
    {
        emuThread->emuUnpause();
        return;
    }

    QStringList file = pickROM(false);
    if (file.isEmpty())
    {
        emuThread->emuUnpause();
        return;
    }

    if (!ROMManager::LoadROM(file, true))
    {
        // TODO: better error reporting?
        QMessageBox::critical(this, "melonDS", "加载ROM失败。");
        emuThread->emuUnpause();
        return;
    }

    QString filename = file.join('|');
    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();

    NDS::Start();
    emuThread->emuRun();

    updateCartInserted(false);
}

void MainWindow::onClearRecentFiles()
{
    recentFileList.clear();
    for (int i = 0; i < 10; i++)
        Config::RecentROMList[i] = "";
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    recentMenu->clear();

    for (int i = 0; i < recentFileList.size(); ++i)
    {
        if (i >= 10) break;

        QString item_full = recentFileList.at(i);
        QString item_display = item_full;
        int itemlen = item_full.length();
        const int maxlen = 100;
        if (itemlen > maxlen)
        {
            int cut_start = 0;
            while (item_full[cut_start] != '/' && item_full[cut_start] != '\\' &&
                   cut_start < itemlen)
                cut_start++;

            int cut_end = itemlen-1;
            while (((item_full[cut_end] != '/' && item_full[cut_end] != '\\') ||
                    (cut_start+4+(itemlen-cut_end) < maxlen)) &&
                   cut_end > 0)
                cut_end--;

            item_display.truncate(cut_start+1);
            item_display += "...";
            item_display += QString(item_full).remove(0, cut_end);
        }

        QAction *actRecentFile_i = recentMenu->addAction(QString("%1.  %2").arg(i+1).arg(item_display));
        actRecentFile_i->setData(item_full);
        connect(actRecentFile_i, &QAction::triggered, this, &MainWindow::onClickRecentFile);

        Config::RecentROMList[i] = recentFileList.at(i).toStdString();
    }

    while (recentFileList.size() > 10)
        recentFileList.removeLast();

    recentMenu->addSeparator();

    QAction *actClearRecentList = recentMenu->addAction("清空");
    connect(actClearRecentList, &QAction::triggered, this, &MainWindow::onClearRecentFiles);

    if (recentFileList.empty())
        actClearRecentList->setEnabled(false);

    Config::Save();
}

void MainWindow::onClickRecentFile()
{
    QAction *act = (QAction *)sender();
    QString filename = act->data().toString();
    QStringList file = filename.split('|');

    emuThread->emuPause();

    if (!verifySetup())
    {
        emuThread->emuUnpause();
        return;
    }

    if (!ROMManager::LoadROM(file, true))
    {
        // TODO: better error reporting?
        QMessageBox::critical(this, "melonDS", "加载ROM失败。");
        emuThread->emuUnpause();
        return;
    }

    recentFileList.removeAll(filename);
    recentFileList.prepend(filename);
    updateRecentFilesMenu();

    NDS::Start();
    emuThread->emuRun();

    updateCartInserted(false);
}

void MainWindow::onBootFirmware()
{
    emuThread->emuPause();

    if (!verifySetup())
    {
        emuThread->emuUnpause();
        return;
    }

    if (!ROMManager::LoadBIOS())
{
        // TODO: better error reporting?
        QMessageBox::critical(this, "melonDS", "这个固件不可启动。");
        emuThread->emuUnpause();
        return;
    }

    NDS::Start();
    emuThread->emuRun();
}

void MainWindow::onInsertCart()
{
    emuThread->emuPause();

    QStringList file = pickROM(false);
    if (file.isEmpty())
    {
        emuThread->emuUnpause();
        return;
    }

    if (!ROMManager::LoadROM(file, false))
    {
        // TODO: better error reporting?
        QMessageBox::critical(this, "melonDS", "加载ROM失败。");
        emuThread->emuUnpause();
        return;
    }

    emuThread->emuUnpause();

    updateCartInserted(false);
}

void MainWindow::onEjectCart()
{
    emuThread->emuPause();

    ROMManager::EjectCart();

    emuThread->emuUnpause();

    updateCartInserted(false);
}

void MainWindow::onInsertGBACart()
{
    emuThread->emuPause();

    QStringList file = pickROM(true);
    if (file.isEmpty())
    {
        emuThread->emuUnpause();
        return;
    }

    if (!ROMManager::LoadGBAROM(file))
    {
        // TODO: better error reporting?
        QMessageBox::critical(this, "melonDS", "加载ROM失败。");
        emuThread->emuUnpause();
        return;
    }

    emuThread->emuUnpause();

    updateCartInserted(true);
}

void MainWindow::onInsertGBAAddon()
{
    QAction* act = (QAction*)sender();
    int type = act->data().toInt();

    emuThread->emuPause();

    ROMManager::LoadGBAAddon(type);

    emuThread->emuUnpause();

    updateCartInserted(true);
}

void MainWindow::onEjectGBACart()
{
    emuThread->emuPause();

    ROMManager::EjectGBACart();

    emuThread->emuUnpause();

    updateCartInserted(true);
}

void MainWindow::onSaveState()
{
    int slot = ((QAction*)sender())->data().toInt();

    emuThread->emuPause();

    std::string filename;
    if (slot > 0)
    {
        filename = ROMManager::GetSavestateName(slot);
    }
    else
    {
        // TODO: specific 'last directory' for savestate files?
        QString qfilename = QFileDialog::getSaveFileName(this,
                                                         "即时存档",
                                                         QString::fromStdString(Config::LastROMFolder),
                                                         "melonDS savestates (*.mln);;Any file (*.*)");
        if (qfilename.isEmpty())
        {
            emuThread->emuUnpause();
            return;
        }

        filename = qfilename.toStdString();
    }

    if (ROMManager::SaveState(filename))
    {
        char msg[64];
        if (slot > 0) sprintf(msg, "即时状态保存至槽位 %d", slot);
        else          sprintf(msg, "即使状态保存至文件");
        OSD::AddMessage(0, msg);

        actLoadState[slot]->setEnabled(true);
    }
    else
    {
        OSD::AddMessage(0xFFA0A0, "即时存档失败");
    }

    emuThread->emuUnpause();
}

void MainWindow::onLoadState()
{
    int slot = ((QAction*)sender())->data().toInt();

    emuThread->emuPause();

    std::string filename;
    if (slot > 0)
    {
        filename = ROMManager::GetSavestateName(slot);
    }
    else
    {
        // TODO: specific 'last directory' for savestate files?
        QString qfilename = QFileDialog::getOpenFileName(this,
                                                         "即时读档",
                                                         QString::fromStdString(Config::LastROMFolder),
                                                         "melonDS savestates (*.ml*);;Any file (*.*)");
        if (qfilename.isEmpty())
        {
            emuThread->emuUnpause();
            return;
        }

        filename = qfilename.toStdString();
    }

    if (!Platform::FileExists(filename))
    {
        char msg[64];
        if (slot > 0) sprintf(msg, "槽位 %d 没有即时存档", slot);
        else          sprintf(msg, "即时存档文件不存在");
        OSD::AddMessage(0xFFA0A0, msg);

        emuThread->emuUnpause();
        return;
    }

    if (ROMManager::LoadState(filename))
    {
        char msg[64];
        if (slot > 0) sprintf(msg, "从槽位 %d 即时读档", slot);
        else          sprintf(msg, "从文件即时读档");
        OSD::AddMessage(0, msg);

        actUndoStateLoad->setEnabled(true);
    }
    else
    {
        OSD::AddMessage(0xFFA0A0, "即时读档失败");
    }

    emuThread->emuUnpause();
}

void MainWindow::onUndoStateLoad()
{
    emuThread->emuPause();
    ROMManager::UndoStateLoad();
    emuThread->emuUnpause();

    OSD::AddMessage(0, "State load undone");
}

void MainWindow::onImportSavefile()
{
    emuThread->emuPause();
    QString path = QFileDialog::getOpenFileName(this,
                                            "选择存档文件",
                                            QString::fromStdString(Config::LastROMFolder),
                                            "Savefiles (*.sav *.bin *.dsv);;Any file (*.*)");

    if (path.isEmpty())
    {
        emuThread->emuUnpause();
        return;
    }

    FILE* f = Platform::OpenFile(path.toStdString(), "rb", true);
    if (!f)
    {
        QMessageBox::critical(this, "melonDS", "无法打开指定存档文件。");
        emuThread->emuUnpause();
        return;
    }

    if (RunningSomething)
    {
        if (QMessageBox::warning(this,
                        "melonDS",
                        "模拟器将重启，当前存档会被覆盖。",
                        QMessageBox::Ok, QMessageBox::Cancel) != QMessageBox::Ok)
        {
            emuThread->emuUnpause();
            return;
        }

        ROMManager::Reset();
    }

    u32 len;
    fseek(f, 0, SEEK_END);
    len = (u32)ftell(f);

    u8* data = new u8[len];
    fseek(f, 0, SEEK_SET);
    fread(data, len, 1, f);

    NDS::LoadSave(data, len);
    delete[] data;

    fclose(f);
    emuThread->emuUnpause();
}

void MainWindow::onQuit()
{
#ifndef _WIN32
    signalSn->setEnabled(false);
#endif
    QApplication::quit();
}


void MainWindow::onPause(bool checked)
{
    if (!RunningSomething) return;

    if (checked)
    {
        emuThread->emuPause();
        OSD::AddMessage(0, "PAUSED");
        pausedManually = true;
    }
    else
    {
        emuThread->emuUnpause();
        OSD::AddMessage(0, "RESUMED");
        pausedManually = false;
    }
}

void MainWindow::onReset()
{
    if (!RunningSomething) return;

    emuThread->emuPause();

    actUndoStateLoad->setEnabled(false);

    ROMManager::Reset();

    OSD::AddMessage(0, "RESET");
    emuThread->emuRun();
}

void MainWindow::onStop()
{
    if (!RunningSomething) return;

    emuThread->emuPause();
    NDS::Stop();
}

void MainWindow::onFrameStep()
{
    if (!RunningSomething) return;

    emuThread->emuFrameStep();
}

void MainWindow::onEnableCheats(bool checked)
{
    Config::EnableCheats = checked?1:0;
    ROMManager::EnableCheats(Config::EnableCheats != 0);
}

void MainWindow::onSetupCheats()
{
    emuThread->emuPause();

    CheatsDialog* dlg = CheatsDialog::openDlg(this);
    connect(dlg, &CheatsDialog::finished, this, &MainWindow::onCheatsDialogFinished);
}

void MainWindow::onCheatsDialogFinished(int res)
{
    emuThread->emuUnpause();
}

void MainWindow::onROMInfo()
{
    ROMInfoDialog* dlg = ROMInfoDialog::openDlg(this);
}

void MainWindow::onRAMInfo()
{
    RAMInfoDialog* dlg = RAMInfoDialog::openDlg(this);
}

void MainWindow::onOpenTitleManager()
{
    TitleManagerDialog* dlg = TitleManagerDialog::openDlg(this);
}

void MainWindow::onMPNewInstance()
{
    //QProcess::startDetached(QApplication::applicationFilePath());
    QProcess newinst;
    newinst.setProgram(QApplication::applicationFilePath());
    newinst.setArguments(QApplication::arguments().mid(1, QApplication::arguments().length()-1));

#ifdef __WIN32__
    newinst.setCreateProcessArgumentsModifier([] (QProcess::CreateProcessArguments *args)
    {
        args->flags |= CREATE_NEW_CONSOLE;
    });
#endif

    newinst.startDetached();
}

void MainWindow::onOpenEmuSettings()
{
    emuThread->emuPause();

    EmuSettingsDialog* dlg = EmuSettingsDialog::openDlg(this);
    connect(dlg, &EmuSettingsDialog::finished, this, &MainWindow::onEmuSettingsDialogFinished);
}

void MainWindow::onEmuSettingsDialogFinished(int res)
{
    emuThread->emuUnpause();

    if (Config::ConsoleType == 1)
    {
        actInsertGBACart->setEnabled(false);
        for (int i = 0; i < 1; i++)
            actInsertGBAAddon[i]->setEnabled(false);
        actEjectGBACart->setEnabled(false);
    }
    else
    {
        actInsertGBACart->setEnabled(true);
        for (int i = 0; i < 1; i++)
            actInsertGBAAddon[i]->setEnabled(true);
        actEjectGBACart->setEnabled(ROMManager::GBACartInserted());
    }

    if (EmuSettingsDialog::needsReset)
        onReset();

    actCurrentGBACart->setText("GBA槽位：" + ROMManager::GBACartLabel());

    if (!RunningSomething)
        actTitleManager->setEnabled(!Config::DSiNANDPath.empty());
}

void MainWindow::onOpenPowerManagement()
{
    PowerManagementDialog* dlg = PowerManagementDialog::openDlg(this);
}

void MainWindow::onOpenInputConfig()
{
    emuThread->emuPause();

    InputConfigDialog* dlg = InputConfigDialog::openDlg(this);
    connect(dlg, &InputConfigDialog::finished, this, &MainWindow::onInputConfigFinished);
}

void MainWindow::onInputConfigFinished(int res)
{
    emuThread->emuUnpause();
}

void MainWindow::onOpenVideoSettings()
{
    VideoSettingsDialog* dlg = VideoSettingsDialog::openDlg(this);
    connect(dlg, &VideoSettingsDialog::updateVideoSettings, this, &MainWindow::onUpdateVideoSettings);
}

void MainWindow::onOpenCameraSettings()
{
    emuThread->emuPause();

    camStarted[0] = camManager[0]->isStarted();
    camStarted[1] = camManager[1]->isStarted();
    if (camStarted[0]) camManager[0]->stop();
    if (camStarted[1]) camManager[1]->stop();

    CameraSettingsDialog* dlg = CameraSettingsDialog::openDlg(this);
    connect(dlg, &CameraSettingsDialog::finished, this, &MainWindow::onCameraSettingsFinished);
}

void MainWindow::onCameraSettingsFinished(int res)
{
    if (camStarted[0]) camManager[0]->start();
    if (camStarted[1]) camManager[1]->start();

    emuThread->emuUnpause();
}

void MainWindow::onOpenAudioSettings()
{
    AudioSettingsDialog* dlg = AudioSettingsDialog::openDlg(this);
    connect(dlg, &AudioSettingsDialog::updateAudioSettings, this, &MainWindow::onUpdateAudioSettings);
    connect(dlg, &AudioSettingsDialog::finished, this, &MainWindow::onAudioSettingsFinished);
}

void MainWindow::onOpenFirmwareSettings()
{
    emuThread->emuPause();

    FirmwareSettingsDialog* dlg = FirmwareSettingsDialog::openDlg(this);
    connect(dlg, &FirmwareSettingsDialog::finished, this, &MainWindow::onFirmwareSettingsFinished);
}

void MainWindow::onFirmwareSettingsFinished(int res)
{
    if (FirmwareSettingsDialog::needsReset)
        onReset();

    emuThread->emuUnpause();
}

void MainWindow::onOpenPathSettings()
{
    emuThread->emuPause();

    PathSettingsDialog* dlg = PathSettingsDialog::openDlg(this);
    connect(dlg, &PathSettingsDialog::finished, this, &MainWindow::onPathSettingsFinished);
}

void MainWindow::onPathSettingsFinished(int res)
{
    if (PathSettingsDialog::needsReset)
        onReset();

    emuThread->emuUnpause();
}

void MainWindow::onUpdateAudioSettings()
{
    SPU::SetInterpolation(Config::AudioInterp);

    if (Config::AudioBitrate == 0)
        SPU::SetDegrade10Bit(NDS::ConsoleType == 0);
    else
        SPU::SetDegrade10Bit(Config::AudioBitrate == 1);
}

void MainWindow::onAudioSettingsFinished(int res)
{
    micClose();

    SPU::SetInterpolation(Config::AudioInterp);

    if (Config::MicInputType == 3)
    {
        micLoadWav(Config::MicWavPath);
        Frontend::Mic_SetExternalBuffer(micWavBuffer, micWavLength);
    }
    else
    {
        delete[] micWavBuffer;
        micWavBuffer = nullptr;

        if (Config::MicInputType == 1)
            Frontend::Mic_SetExternalBuffer(micExtBuffer, sizeof(micExtBuffer)/sizeof(s16));
        else
            Frontend::Mic_SetExternalBuffer(NULL, 0);
    }

    micOpen();
}

void MainWindow::onOpenMPSettings()
{
    emuThread->emuPause();

    MPSettingsDialog* dlg = MPSettingsDialog::openDlg(this);
    connect(dlg, &MPSettingsDialog::finished, this, &MainWindow::onMPSettingsFinished);
}

void MainWindow::onMPSettingsFinished(int res)
{
    audioMute();
    LocalMP::SetRecvTimeout(Config::MPRecvTimeout);

    emuThread->emuUnpause();
}

void MainWindow::onOpenWifiSettings()
{
    emuThread->emuPause();

    WifiSettingsDialog* dlg = WifiSettingsDialog::openDlg(this);
    connect(dlg, &WifiSettingsDialog::finished, this, &MainWindow::onWifiSettingsFinished);
}

void MainWindow::onWifiSettingsFinished(int res)
{
    Platform::LAN_DeInit();
    Platform::LAN_Init();

    if (WifiSettingsDialog::needsReset)
        onReset();

    emuThread->emuUnpause();
}

void MainWindow::onOpenInterfaceSettings()
{
    emuThread->emuPause();
    InterfaceSettingsDialog* dlg = InterfaceSettingsDialog::openDlg(this);
    connect(dlg, &InterfaceSettingsDialog::finished, this, &MainWindow::onInterfaceSettingsFinished);
    connect(dlg, &InterfaceSettingsDialog::updateMouseTimer, this, &MainWindow::onUpdateMouseTimer);
}

void MainWindow::onUpdateMouseTimer()
{
    panel->mouseTimer->setInterval(Config::MouseHideSeconds*1000);
}

void MainWindow::onInterfaceSettingsFinished(int res)
{
    emuThread->emuUnpause();
}

void MainWindow::onChangeSavestateSRAMReloc(bool checked)
{
    Config::SavestateRelocSRAM = checked?1:0;
}

void MainWindow::onChangeScreenSize()
{
    int factor = ((QAction*)sender())->data().toInt();
    QSize diff = size() - panelWidget->size();
    resize(panel->screenGetMinSize(factor) + diff);
}

void MainWindow::onChangeScreenRotation(QAction* act)
{
    int rot = act->data().toInt();
    Config::ScreenRotation = rot;

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenGap(QAction* act)
{
    int gap = act->data().toInt();
    Config::ScreenGap = gap;

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenLayout(QAction* act)
{
    int layout = act->data().toInt();
    Config::ScreenLayout = layout;

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenSwap(bool checked)
{
    Config::ScreenSwap = checked?1:0;

    // Swap between top and bottom screen when displaying one screen.
    if (Config::ScreenSizing == screenSizing_TopOnly)
    {
        // Bottom Screen.
        Config::ScreenSizing = screenSizing_BotOnly;
        actScreenSizing[screenSizing_TopOnly]->setChecked(false);
        actScreenSizing[Config::ScreenSizing]->setChecked(true);
    }
    else if (Config::ScreenSizing == screenSizing_BotOnly)
    {
        // Top Screen.
        Config::ScreenSizing = screenSizing_TopOnly;
        actScreenSizing[screenSizing_BotOnly]->setChecked(false);
        actScreenSizing[Config::ScreenSizing]->setChecked(true);
    }

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenSizing(QAction* act)
{
    int sizing = act->data().toInt();
    Config::ScreenSizing = sizing;

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenAspect(QAction* act)
{
    int aspect = act->data().toInt();
    QActionGroup* group = act->actionGroup();

    if (group == grpScreenAspectTop)
    {
        Config::ScreenAspectTop = aspect;
    }
    else
    {
        Config::ScreenAspectBot = aspect;
    }

    emit screenLayoutChange();
}

void MainWindow::onChangeIntegerScaling(bool checked)
{
    Config::IntegerScaling = checked?1:0;

    emit screenLayoutChange();
}

void MainWindow::onChangeScreenFiltering(bool checked)
{
    Config::ScreenFilter = checked?1:0;

    emit screenLayoutChange();
}

void MainWindow::onChangeShowOSD(bool checked)
{
    Config::ShowOSD = checked?1:0;
}
void MainWindow::onChangeLimitFramerate(bool checked)
{
    Config::LimitFPS = checked?1:0;
}

void MainWindow::onChangeAudioSync(bool checked)
{
    Config::AudioSync = checked?1:0;
}


void MainWindow::onTitleUpdate(QString title)
{
    setWindowTitle(title);
}

void ToggleFullscreen(MainWindow* mainWindow) {
    if (!mainWindow->isFullScreen())
    {
        mainWindow->showFullScreen();
        mainWindow->menuBar()->setFixedHeight(0); // Don't use hide() as menubar actions stop working
    }
    else
    {
        mainWindow->showNormal();
        int menuBarHeight = mainWindow->menuBar()->sizeHint().height();
        mainWindow->menuBar()->setFixedHeight(menuBarHeight);
    }
}

void MainWindow::onFullscreenToggled()
{
    ToggleFullscreen(this);
}

void MainWindow::onEmuStart()
{
    for (int i = 1; i < 9; i++)
    {
        actSaveState[i]->setEnabled(true);
        actLoadState[i]->setEnabled(ROMManager::SavestateExists(i));
    }
    actSaveState[0]->setEnabled(true);
    actLoadState[0]->setEnabled(true);
    actUndoStateLoad->setEnabled(false);

    actPause->setEnabled(true);
    actPause->setChecked(false);
    actReset->setEnabled(true);
    actStop->setEnabled(true);
    actFrameStep->setEnabled(true);

    actPowerManagement->setEnabled(true);

    actTitleManager->setEnabled(false);
}

void MainWindow::onEmuStop()
{
    emuThread->emuPause();

    for (int i = 0; i < 9; i++)
    {
        actSaveState[i]->setEnabled(false);
        actLoadState[i]->setEnabled(false);
    }
    actUndoStateLoad->setEnabled(false);

    actPause->setEnabled(false);
    actReset->setEnabled(false);
    actStop->setEnabled(false);
    actFrameStep->setEnabled(false);

    actPowerManagement->setEnabled(false);

    actTitleManager->setEnabled(!Config::DSiNANDPath.empty());
}

void MainWindow::onUpdateVideoSettings(bool glchange)
{
    if (glchange)
    {
        emuThread->emuPause();
        if (hasOGL) emuThread->deinitContext();

        delete panel;
        createScreenPanel();
        connect(emuThread, SIGNAL(windowUpdate()), panelWidget, SLOT(repaint()));
    }

    videoSettingsDirty = true;

    if (glchange)
    {
        if (hasOGL) emuThread->initContext();
        emuThread->emuUnpause();
    }
}


void emuStop()
{
    RunningSomething = false;

    emit emuThread->windowEmuStop();

    OSD::AddMessage(0xFFC040, "SHUTDOWN");
}

MelonApplication::MelonApplication(int& argc, char** argv)
    : QApplication(argc, argv)
{
    setWindowIcon(QIcon(":/melon-icon"));
}

bool MelonApplication::event(QEvent *event)
{
    if (event->type() == QEvent::FileOpen)
    {
        QFileOpenEvent *openEvent = static_cast<QFileOpenEvent*>(event);

        emuThread->emuPause();
        if (!mainWindow->preloadROMs(openEvent->file().split("|"), {}, true))
            emuThread->emuUnpause();
    }

    return QApplication::event(event);
}

int main(int argc, char** argv)
{
    srand(time(nullptr));

    qputenv("QT_SCALE_FACTOR", "1");

    printf("melonDS " MELONDS_VERSION "\n");
    printf(MELONDS_URL "\n");

    // easter egg - not worth checking other cases for something so dumb
    if (argc != 0 && (!strcasecmp(argv[0], "derpDS") || !strcasecmp(argv[0], "./derpDS")))
        printf("你刚才叫我笨蛋？？？\n");
    
    Platform::Init(argc, argv);

    MelonApplication melon(argc, argv);

    CLI::CommandLineOptions* options = CLI::ManageArgs(melon);

    // http://stackoverflow.com/questions/14543333/joystick-wont-work-using-sdl
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_HAPTIC) < 0)
    {
        printf("SDL无法初始化震动\n");
    }
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0)
    {
        printf("SDL无法初始化手柄控制\n");
    }
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        const char* err = SDL_GetError();
        QString errorStr = "SDL初始化失败。这表明您的音频驱动程序可能存在问题。\n\n错误为：";
        errorStr += err;

        QMessageBox::critical(NULL, "melonDS", errorStr);
        return 1;
    }

    SDL_JoystickEventState(SDL_ENABLE);

    Config::Load();

#define SANITIZE(var, min, max)  { var = std::clamp(var, min, max); }
    SANITIZE(Config::ConsoleType, 0, 1);
    SANITIZE(Config::_3DRenderer,
    0,
    0 // Minimum, Software renderer
    #ifdef OGLRENDERER_ENABLED
    + 1 // OpenGL Renderer
    #endif
    );
    SANITIZE(Config::ScreenVSyncInterval, 1, 20);
    SANITIZE(Config::GL_ScaleFactor, 1, 16);
    SANITIZE(Config::AudioInterp, 0, 3);
    SANITIZE(Config::AudioVolume, 0, 256);
    SANITIZE(Config::MicInputType, 0, 3);
    SANITIZE(Config::ScreenRotation, 0, 3);
    SANITIZE(Config::ScreenGap, 0, 500);
    SANITIZE(Config::ScreenLayout, 0, 3);
    SANITIZE(Config::ScreenSizing, 0, (int)screenSizing_MAX);
    SANITIZE(Config::ScreenAspectTop, 0, 4);
    SANITIZE(Config::ScreenAspectBot, 0, 4);
#undef SANITIZE

    audioMuted = false;
    audioSync = SDL_CreateCond();
    audioSyncLock = SDL_CreateMutex();

    audioFreq = 48000; // TODO: make configurable?
    SDL_AudioSpec whatIwant, whatIget;
    memset(&whatIwant, 0, sizeof(SDL_AudioSpec));
    whatIwant.freq = audioFreq;
    whatIwant.format = AUDIO_S16LSB;
    whatIwant.channels = 2;
    whatIwant.samples = 1024;
    whatIwant.callback = audioCallback;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &whatIwant, &whatIget, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!audioDevice)
    {
        printf("音频初始化失败： %s\n", SDL_GetError());
    }
    else
    {
        audioFreq = whatIget.freq;
        printf("音频输出频率： %d Hz\n", audioFreq);
        SDL_PauseAudioDevice(audioDevice, 1);
    }

    micDevice = 0;

    memset(micExtBuffer, 0, sizeof(micExtBuffer));
    micExtBufferWritePos = 0;
    micWavBuffer = nullptr;

    camStarted[0] = false;
    camStarted[1] = false;
    camManager[0] = new CameraManager(0, 640, 480, true);
    camManager[1] = new CameraManager(1, 640, 480, true);
    camManager[0]->setXFlip(Config::Camera[0].XFlip);
    camManager[1]->setXFlip(Config::Camera[1].XFlip);

    ROMManager::EnableCheats(Config::EnableCheats != 0);

    Frontend::Init_Audio(audioFreq);

    if (Config::MicInputType == 1)
    {
        Frontend::Mic_SetExternalBuffer(micExtBuffer, sizeof(micExtBuffer)/sizeof(s16));
    }
    else if (Config::MicInputType == 3)
    {
        micLoadWav(Config::MicWavPath);
        Frontend::Mic_SetExternalBuffer(micWavBuffer, micWavLength);
    }

    Input::JoystickID = Config::JoystickID;
    Input::OpenJoystick();

    mainWindow = new MainWindow();
    if (options->fullscreen)
        ToggleFullscreen(mainWindow);

    emuThread = new EmuThread();
    emuThread->start();
    emuThread->emuPause();

    audioMute();

    QObject::connect(&melon, &QApplication::applicationStateChanged, mainWindow, &MainWindow::onAppStateChanged);

    mainWindow->preloadROMs(options->dsRomPath, options->gbaRomPath, options->boot);

    int ret = melon.exec();

    emuThread->emuStop();
    emuThread->wait();
    delete emuThread;

    Input::CloseJoystick();

    if (audioDevice) SDL_CloseAudioDevice(audioDevice);
    micClose();

    SDL_DestroyCond(audioSync);
    SDL_DestroyMutex(audioSyncLock);

    if (micWavBuffer) delete[] micWavBuffer;

    delete camManager[0];
    delete camManager[1];

    Config::Save();

    SDL_Quit();
    Platform::DeInit();
    return ret;
}

#ifdef __WIN32__

#include <windows.h>

int CALLBACK WinMain(HINSTANCE hinst, HINSTANCE hprev, LPSTR cmdline, int cmdshow)
{
    int argc = 0;
    wchar_t** argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    char nullarg[] = {'\0'};

    char** argv = new char*[argc];
    for (int i = 0; i < argc; i++)
    {
        if (!argv_w) { argv[i] = nullarg; continue; }
        int len = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, NULL, 0, NULL, NULL);
        if (len < 1) { argv[i] = nullarg; continue; }
        argv[i] = new char[len];
        int res = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, argv[i], len, NULL, NULL);
        if (res != len) { delete[] argv[i]; argv[i] = nullarg; }
    }

    if (argv_w) LocalFree(argv_w);

    //if (AttachConsole(ATTACH_PARENT_PROCESS))
    /*if (AllocConsole())
    {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        printf("\n");
    }*/

    int ret = main(argc, argv);

    printf("\n\n>");

    for (int i = 0; i < argc; i++) if (argv[i] != nullarg) delete[] argv[i];
    delete[] argv;

    return ret;
}

#endif
