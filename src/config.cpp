#include "config.h"
#include <shellapi.h>
#include <shlobj.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>

static FILE* g_log;

static std::wstring appPath(const wchar_t* file) {
    static std::wstring dir;
    if (dir.empty()) {
        PWSTR appData;
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);
        dir = std::wstring(appData) + L"\\motu-audio-router";
        CoTaskMemFree(appData);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir + L"\\" + file;
}

void logOpen() {
    std::wstring log = appPath(L"log.txt");
    MoveFileExW(log.c_str(), appPath(L"log.old.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen(log.c_str(), L"w", _SH_DENYNO);  // any editor can open it while we write
}

void logShow() { ShellExecuteW(nullptr, L"open", appPath(L"log.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// Called from the UI and engine threads; each line goes out in a single write.
void appLog(const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
    va_end(args);
    fprintf(g_log, "%s\n", buf);
    fflush(g_log);
}

void configLoad(AudioConfig& cfg, bool& mute) {
    std::wstring ini = appPath(L"config.ini");
    wchar_t driver[128];
    GetPrivateProfileStringW(L"audio", L"driver", L"", driver, 128, ini.c_str());
    if (driver[0]) {
        char narrow[128];
        WideCharToMultiByte(CP_ACP, 0, driver, -1, narrow, sizeof(narrow), nullptr, nullptr);
        cfg.driver = narrow;
    }
    cfg.inFirst = GetPrivateProfileIntW(L"audio", L"inFirst", cfg.inFirst, ini.c_str());
    cfg.outFirst = GetPrivateProfileIntW(L"audio", L"outFirst", cfg.outFirst, ini.c_str());
    cfg.bufSize = GetPrivateProfileIntW(L"audio", L"bufSize", cfg.bufSize, ini.c_str());
    cfg.mixPc = GetPrivateProfileIntW(L"audio", L"mixPc", cfg.mixPc, ini.c_str()) != 0;
    mute = GetPrivateProfileIntW(L"audio", L"mute", mute, ini.c_str()) != 0;
}

void configSave(const AudioConfig& cfg, bool mute) {
    std::wstring ini = appPath(L"config.ini");
    wchar_t driver[128];
    MultiByteToWideChar(CP_ACP, 0, cfg.driver.c_str(), -1, driver, 128);
    WritePrivateProfileStringW(L"audio", L"driver", driver, ini.c_str());
    WritePrivateProfileStringW(L"audio", L"inFirst", std::to_wstring(cfg.inFirst).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"audio", L"outFirst", std::to_wstring(cfg.outFirst).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"audio", L"bufSize", std::to_wstring(cfg.bufSize).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"audio", L"mixPc", cfg.mixPc ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"audio", L"mute", mute ? L"1" : L"0", ini.c_str());
}
