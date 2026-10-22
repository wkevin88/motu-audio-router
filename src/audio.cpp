// ASIO engine. One worker thread owns the driver (all COM/ASIO calls happen there);
// the driver's audio thread only runs onBufferSwitch, which never allocates, locks or logs.
#include "audio.h"
#include <objbase.h>
#include <avrt.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include "iasiodrv.h"

static HWND g_owner;
static std::thread g_thread;
static HANDLE g_wake;  // auto-reset event: wakes the worker
static std::atomic<bool> g_quit, g_reopen, g_panel, g_mute;

static std::mutex g_mutex;  // guards g_cfg and g_status
static AudioConfig g_cfg;
static AudioStatus g_status;

// Driver state, owned by the worker. The callback reads it only while the stream runs.
static IASIO* g_drv;
static bool g_buffersCreated, g_started, g_outputReady;
static ASIOBufferInfo g_bufs[4];  // in L, in R, out L, out R
static long g_bufBytes;
static std::atomic<long> g_callbacks;

static ASIOTime* onBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    static thread_local bool mmcss;
    if (!mmcss) {
        DWORD task = 0;
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        mmcss = true;
    }
    bool mute = g_mute.load(std::memory_order_relaxed);
    for (int c = 0; c < 2; c++) {
        void* dst = g_bufs[2 + c].buffers[index];
        if (mute) memset(dst, 0, g_bufBytes);
        else memcpy(dst, g_bufs[c].buffers[index], g_bufBytes);
    }
    if (g_outputReady) g_drv->outputReady();
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    return t;
}

static void onBufferSwitch(long index, ASIOBool direct) { onBufferSwitchTimeInfo(nullptr, index, direct); }

static void requestReopen() {
    g_reopen = true;
    SetEvent(g_wake);
}

static void onSampleRateDidChange(ASIOSampleRate) { requestReopen(); }

static long onAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
               value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion: return 2;
    case kAsioSupportsTimeInfo: return 1;
    case kAsioResetRequest:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged: requestReopen(); return 1;
    }
    return 0;
}

static ASIOCallbacks g_callbacksTable = {onBufferSwitch, onSampleRateDidChange, onAsioMessage, onBufferSwitchTimeInfo};

static bool findClsid(const std::string& name, CLSID* out) {
    char clsid[64];
    DWORD len = sizeof(clsid);
    std::string key = "SOFTWARE\\ASIO\\" + name;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key.c_str(), "CLSID", RRF_RT_REG_SZ, nullptr, clsid, &len) != ERROR_SUCCESS)
        return false;
    wchar_t w[64];
    MultiByteToWideChar(CP_ACP, 0, clsid, -1, w, 64);
    return CLSIDFromString(w, out) == S_OK;
}

static void closeDriver() {
    if (!g_drv) return;
    if (g_started) g_drv->stop();
    if (g_buffersCreated) g_drv->disposeBuffers();
    g_drv->Release();
    g_drv = nullptr;
    g_started = g_buffersCreated = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = AudioStatus();
}

// Returns an empty string on success, otherwise the reason it failed.
static std::string openDriver() {
    AudioConfig cfg;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        cfg = g_cfg;
    }
    CLSID clsid;
    if (!findClsid(cfg.driver, &clsid)) return "driver '" + cfg.driver + "' not installed";
    // ASIO quirk: the CLSID doubles as the IID.
    if (CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&g_drv) != S_OK) {
        g_drv = nullptr;
        return "CoCreateInstance failed";
    }
    char msg[128] = {};
    if (!g_drv->init(g_owner)) {
        g_drv->getErrorMessage(msg);
        closeDriver();
        return std::string("init failed: ") + msg;
    }

    long numIn, numOut;
    g_drv->getChannels(&numIn, &numOut);
    if (cfg.inFirst < 0 || cfg.inFirst + 1 >= numIn || cfg.outFirst < 0 || cfg.outFirst + 1 >= numOut) {
        closeDriver();
        return "channel pair out of range";
    }
    ASIOChannelInfo inInfo = {cfg.inFirst, ASIOTrue}, outInfo = {cfg.outFirst, ASIOFalse};
    g_drv->getChannelInfo(&inInfo);
    g_drv->getChannelInfo(&outInfo);
    if (inInfo.type != outInfo.type) {
        closeDriver();
        return "input and output sample types differ";
    }
    long bytesPerSample = inInfo.type == ASIOSTInt16LSB ? 2 : inInfo.type == ASIOSTInt24LSB ? 3
                        : inInfo.type == ASIOSTFloat64LSB ? 8 : 4;

    for (int c = 0; c < 4; c++) {
        g_bufs[c].isInput = c < 2;
        g_bufs[c].channelNum = (c < 2 ? cfg.inFirst : cfg.outFirst) + c % 2;
    }
    long minSize, maxSize, prefSize, granularity;
    g_drv->getBufferSize(&minSize, &maxSize, &prefSize, &granularity);
    long bufSize = cfg.bufSize;
    if (g_drv->createBuffers(g_bufs, 4, bufSize, &g_callbacksTable) != ASE_OK) {
        appLog("buffer size %ld rejected, using driver default %ld", bufSize, prefSize);
        bufSize = prefSize;
        if (g_drv->createBuffers(g_bufs, 4, bufSize, &g_callbacksTable) != ASE_OK) {
            g_drv->getErrorMessage(msg);
            closeDriver();
            return std::string("createBuffers failed: ") + msg;
        }
    }
    g_buffersCreated = true;
    g_bufBytes = bufSize * bytesPerSample;
    g_outputReady = g_drv->outputReady() == ASE_OK;

    ASIOSampleRate rate = 0;
    long inLatency = 0, outLatency = 0;
    g_drv->getSampleRate(&rate);
    g_drv->getLatencies(&inLatency, &outLatency);

    if (g_drv->start() != ASE_OK) {
        closeDriver();
        return "start failed";
    }
    g_started = true;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = {true, numIn, numOut, bufSize, rate, rate > 0 ? (inLatency + outLatency) * 1000.0 / rate : 0};
    appLog("running: in %ld-%ld -> out %ld-%ld, buffer %ld @ %.0f Hz, latency in %ld + out %ld samples = %.2f ms",
           cfg.inFirst + 1, cfg.inFirst + 2, cfg.outFirst + 1, cfg.outFirst + 2, bufSize, rate, inLatency,
           outLatency, g_status.latencyMs);
    return "";
}

static void worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string lastError;
    long lastCallbacks = 0;
    ULONGLONG lastProgress = 0;

    while (!g_quit) {
        if (!g_drv) {
            g_reopen = false;
            std::string err = openDriver();
            if (!err.empty()) {
                if (err != lastError) appLog("open failed (retrying every 2 s): %s", err.c_str());
                lastError = err;
                WaitForSingleObject(g_wake, 2000);
                continue;
            }
            lastError.clear();
            lastCallbacks = g_callbacks;
            lastProgress = GetTickCount64();
        }

        WaitForSingleObject(g_wake, 500);
        if (g_reopen) {
            appLog("re-opening driver");
            closeDriver();
            continue;
        }
        if (g_panel.exchange(false)) g_drv->controlPanel();

        // Watchdog: callbacks stop on unplug or sleep without always sending a reset request.
        long n = g_callbacks;
        ULONGLONG now = GetTickCount64();
        if (n != lastCallbacks) {
            lastCallbacks = n;
            lastProgress = now;
        } else if (now - lastProgress > 2000) {
            appLog("no audio callbacks for 2 s, re-opening driver");
            closeDriver();
        }
    }
    closeDriver();
    CoUninitialize();
}

void audioStart(HWND owner, const AudioConfig& cfg) {
    g_owner = owner;
    g_cfg = cfg;
    g_quit = false;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = std::thread(worker);
}

void audioStop() {
    if (!g_thread.joinable()) return;
    g_quit = true;
    SetEvent(g_wake);
    g_thread.join();
    CloseHandle(g_wake);
}

void audioSetConfig(const AudioConfig& cfg) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (cfg.driver == g_cfg.driver && cfg.inFirst == g_cfg.inFirst && cfg.outFirst == g_cfg.outFirst &&
            cfg.bufSize == g_cfg.bufSize)
            return;
        g_cfg = cfg;
    }
    requestReopen();
}

void audioSetMute(bool mute) { g_mute = mute; }

void audioOpenControlPanel() {
    g_panel = true;
    SetEvent(g_wake);
}

AudioStatus audioStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}
