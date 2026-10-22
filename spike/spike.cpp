// Step 1 spike: load the MOTU ASIO driver, print its capabilities, then copy
// input pair -> output pair for N seconds while counting callbacks and dropouts.
// Usage: spike [seconds=20] [bufSize=preferred] [inFirst=2] [outFirst=2] [driverName="MOTU M Series"]
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "iasiodrv.h"

static IASIO* g_drv;
static ASIOBufferInfo g_bufs[4];  // in L, in R, out L, out R
static long g_bufSize;
static long g_bytesPerSample;
static std::atomic<long> g_callbacks{0}, g_dropouts{0}, g_resetRequests{0};
static long long g_lastPos = -1;
static bool g_outputReady;

static long long toInt64(const ASIOSamples& s) {
    return ((long long)s.hi << 32) | (unsigned long)s.lo;
}

static ASIOTime* bufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    for (int c = 0; c < 2; c++)
        memcpy(g_bufs[2 + c].buffers[index], g_bufs[c].buffers[index], g_bufSize * g_bytesPerSample);
    if (g_outputReady) g_drv->outputReady();

    if (t && (t->timeInfo.flags & kSamplePositionValid)) {
        long long pos = toInt64(t->timeInfo.samplePosition);
        if (g_lastPos >= 0 && pos - g_lastPos != g_bufSize) g_dropouts++;
        g_lastPos = pos;
    }
    g_callbacks++;
    return t;
}

static void bufferSwitch(long index, ASIOBool direct) {
    ASIOTime t = {};
    ASIOTimeStamp ts;
    if (g_drv->getSamplePosition(&t.timeInfo.samplePosition, &ts) == ASE_OK)
        t.timeInfo.flags = kSamplePositionValid;
    bufferSwitchTimeInfo(&t, index, direct);
}

static void sampleRateDidChange(ASIOSampleRate sr) { printf("sample rate changed: %.0f\n", sr); }

static long asioMessage(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioEngineVersion ||
               value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
               value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion: return 2;
    case kAsioSupportsTimeInfo: return 1;
    case kAsioResetRequest: g_resetRequests++; return 1;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged: return 1;
    }
    return 0;
}

static bool findClsid(const char* name, CLSID* out) {
    HKEY root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return false;
    char sub[256];
    bool found = false;
    for (DWORD i = 0; !found; i++) {
        DWORD len = sizeof(sub);
        if (RegEnumKeyExA(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        printf("  ASIO driver: %s\n", sub);
        if (_stricmp(sub, name) != 0) continue;
        char clsid[64];
        DWORD clen = sizeof(clsid);
        if (RegGetValueA(root, sub, "CLSID", RRF_RT_REG_SZ, nullptr, clsid, &clen) == ERROR_SUCCESS) {
            wchar_t w[64];
            MultiByteToWideChar(CP_ACP, 0, clsid, -1, w, 64);
            found = CLSIDFromString(w, out) == S_OK;
        }
    }
    RegCloseKey(root);
    return found;
}

static const char* typeName(ASIOSampleType t) {
    switch (t) {
    case ASIOSTInt16LSB: return "Int16LSB";
    case ASIOSTInt24LSB: return "Int24LSB";
    case ASIOSTInt32LSB: return "Int32LSB";
    case ASIOSTFloat32LSB: return "Float32LSB";
    default: return "other";
    }
}

static long bytesFor(ASIOSampleType t) {
    switch (t) {
    case ASIOSTInt16LSB: return 2;
    case ASIOSTInt24LSB: return 3;
    case ASIOSTFloat64LSB: return 8;
    default: return 4;
    }
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    long reqBuf = argc > 2 ? atol(argv[2]) : 0;
    long inFirst = argc > 3 ? atol(argv[3]) : 2;
    long outFirst = argc > 4 ? atol(argv[4]) : 2;
    const char* name = argc > 5 ? argv[5] : "MOTU M Series";

    CoInitialize(nullptr);
    CLSID clsid;
    if (!findClsid(name, &clsid)) { printf("driver '%s' not found\n", name); return 1; }

    // ASIO quirk: the CLSID doubles as the IID.
    if (CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&g_drv) != S_OK) {
        printf("CoCreateInstance failed\n"); return 1;
    }
    if (!g_drv->init(GetConsoleWindow())) {
        char msg[128] = {};
        g_drv->getErrorMessage(msg);
        printf("init failed: %s\n", msg); return 1;
    }
    char drvName[64] = {};
    g_drv->getDriverName(drvName);
    printf("driver: %s v%ld\n", drvName, g_drv->getDriverVersion());

    long nIn, nOut;
    g_drv->getChannels(&nIn, &nOut);
    printf("channels: %ld in, %ld out\n", nIn, nOut);
    for (int isIn = 1; isIn >= 0; isIn--)
        for (long i = 0; i < (isIn ? nIn : nOut); i++) {
            ASIOChannelInfo ci = {};
            ci.channel = i;
            ci.isInput = isIn;
            g_drv->getChannelInfo(&ci);
            printf("  %s %2ld: %-24s %s\n", isIn ? "in " : "out", i, ci.name, typeName(ci.type));
        }

    long minSz, maxSz, prefSz, gran;
    g_drv->getBufferSize(&minSz, &maxSz, &prefSz, &gran);
    ASIOSampleRate sr = 0;
    g_drv->getSampleRate(&sr);
    printf("buffer: min %ld max %ld preferred %ld granularity %ld; sample rate %.0f\n",
           minSz, maxSz, prefSz, gran, sr);

    if (inFirst + 1 >= nIn || outFirst + 1 >= nOut) { printf("channel index out of range\n"); return 1; }
    for (int c = 0; c < 4; c++) {
        g_bufs[c].isInput = c < 2;
        g_bufs[c].channelNum = (c < 2 ? inFirst : outFirst) + (c % 2);
    }
    ASIOChannelInfo inInfo = {inFirst, ASIOTrue}, outInfo = {outFirst, ASIOFalse};
    g_drv->getChannelInfo(&inInfo);
    g_drv->getChannelInfo(&outInfo);
    if (inInfo.type != outInfo.type) { printf("input/output sample types differ\n"); return 1; }
    g_bytesPerSample = bytesFor(inInfo.type);

    static ASIOCallbacks cb = {bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo};
    g_bufSize = reqBuf ? reqBuf : prefSz;
    if (g_drv->createBuffers(g_bufs, 4, g_bufSize, &cb) != ASE_OK) {
        char msg[128] = {};
        g_drv->getErrorMessage(msg);
        printf("createBuffers failed: %s\n", msg); return 1;
    }
    long inLat, outLat;
    g_drv->getLatencies(&inLat, &outLat);
    printf("latency: in %ld + out %ld samples = %.2f ms round trip (buffer %.2f ms)\n",
           inLat, outLat, (inLat + outLat) * 1000.0 / sr, g_bufSize * 1000.0 / sr);
    g_outputReady = g_drv->outputReady() == ASE_OK;
    printf("outputReady supported: %s\n", g_outputReady ? "yes" : "no");

    if (g_drv->start() != ASE_OK) { printf("start failed\n"); return 1; }
    printf("routing in %ld-%ld -> out %ld-%ld for %d s...\n", inFirst + 1, inFirst + 2, outFirst + 1, outFirst + 2, seconds);
    for (int s = 1; s <= seconds; s++) {
        Sleep(1000);
        printf("  %2ds callbacks %ld (expected ~%.0f) dropouts %ld resets %ld\n", s, g_callbacks.load(),
               s * sr / g_bufSize, g_dropouts.load(), g_resetRequests.load());
    }
    g_drv->stop();
    g_drv->disposeBuffers();
    g_drv->Release();
    CoUninitialize();
    return 0;
}
