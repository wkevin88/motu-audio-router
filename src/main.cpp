// Tray app: a hidden window owns the notification icon and popup menu; the audio engine does the rest.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include "audio.h"
#include "config.h"
#include "startup.h"

enum : UINT {
    WM_TRAY = WM_APP + 1,
    ID_INPUT = 100,   // + pair index
    ID_OUTPUT = 200,  // + pair index
    ID_BUFFER = 300,  // + index into kBufferSizes
    ID_MUTE = 400,
    ID_MIXPC,
    ID_PANEL,
    ID_LOG,
    ID_STARTUP,
    ID_QUIT,
    TIMER_STATUS = 1,
};
static const long kBufferSizes[] = {16, 32, 64, 128, 256};
static const long kPhysicalInputs = 4;  // M4 inputs 5-8 are loopback channels

static AudioConfig g_cfg;
static bool g_mute;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarCreated;
static long g_numIn = 8, g_numOut = 4;  // M4 channel counts until the driver reports its own

static void statusText(wchar_t* out, size_t len) {
    AudioStatus st = audioStatus();
    if (!st.running) {
        swprintf(out, len, L"Waiting for %hs...", g_cfg.driver.c_str());
        return;
    }
    swprintf(out, len, L"In %ld-%ld%s → Out %ld-%ld · %ld samples · %.1f ms%s", g_cfg.inFirst + 1,
             g_cfg.inFirst + 2, g_cfg.mixPc && mixPcAllowed(g_cfg) ? L" + PC" : L"", g_cfg.outFirst + 1, g_cfg.outFirst + 2, st.bufSize,
             st.latencyMs, g_mute ? L" · muted" : L"");
}

static void updateTooltip() {
    wchar_t tip[128];
    statusText(tip, 128);
    if (wcscmp(tip, g_nid.szTip) == 0) return;
    wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static HMENU pairMenu(const wchar_t* label, long count, long selectedFirst, UINT idBase) {
    HMENU m = CreatePopupMenu();
    for (long p = 0; p < count / 2; p++) {
        wchar_t text[32];
        swprintf(text, 32, L"%s %ld-%ld", label, p * 2 + 1, p * 2 + 2);
        AppendMenuW(m, MF_STRING | (p * 2 == selectedFirst ? MF_CHECKED : 0), idBase + p, text);
    }
    return m;
}

static void showMenu(HWND hwnd) {
    AudioStatus st = audioStatus();
    if (st.running) {
        g_numIn = st.numIn;
        g_numOut = st.numOut;
    }
    HMENU menu = CreatePopupMenu();
    wchar_t text[128];
    statusText(text, 128);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)pairMenu(L"In", min(g_numIn, kPhysicalInputs), g_cfg.inFirst, ID_INPUT), L"Input");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)pairMenu(L"Out", g_numOut, g_cfg.outFirst, ID_OUTPUT), L"Output");

    HMENU buffers = CreatePopupMenu();
    double rate = st.sampleRate > 0 ? st.sampleRate : 48000;
    for (int i = 0; i < _countof(kBufferSizes); i++) {
        swprintf(text, 128, L"%ld samples (%.2f ms buffer)", kBufferSizes[i], kBufferSizes[i] * 1000.0 / rate);
        AppendMenuW(buffers, MF_STRING | (kBufferSizes[i] == g_cfg.bufSize ? MF_CHECKED : 0), ID_BUFFER + i, text);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)buffers, L"Buffer size");
    AppendMenuW(menu, MF_STRING | (g_cfg.mixPc ? MF_CHECKED : 0) | (mixPcAllowed(g_cfg) ? 0 : MF_GRAYED), ID_MIXPC,
                mixPcAllowed(g_cfg) ? L"Mix in PC audio" : L"Mix in PC audio (not on Out 1-2)");
    AppendMenuW(menu, MF_STRING | (g_mute ? MF_CHECKED : 0), ID_MUTE, L"Mute");
    AppendMenuW(menu, MF_STRING, ID_PANEL, L"Open driver settings");
    AppendMenuW(menu, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(menu, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);  // so the menu closes when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);  // also destroys the submenus
}

static void onCommand(HWND hwnd, UINT id) {
    if (id == ID_QUIT) {
        DestroyWindow(hwnd);
        return;
    }
    if (id >= ID_INPUT && id < ID_INPUT + 8) g_cfg.inFirst = (id - ID_INPUT) * 2;
    else if (id >= ID_OUTPUT && id < ID_OUTPUT + 8) g_cfg.outFirst = (id - ID_OUTPUT) * 2;
    else if (id >= ID_BUFFER && id < ID_BUFFER + _countof(kBufferSizes)) g_cfg.bufSize = kBufferSizes[id - ID_BUFFER];
    else if (id == ID_MIXPC) g_cfg.mixPc = !g_cfg.mixPc;
    else if (id == ID_MUTE) audioSetMute(g_mute = !g_mute);
    else if (id == ID_PANEL) audioOpenControlPanel();
    else if (id == ID_LOG) logShow();
    else if (id == ID_STARTUP) startupSet(!startupEnabled());
    audioSetConfig(g_cfg);
    configSave(g_cfg, g_mute);
    updateTooltip();
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) showMenu(hwnd);
        return 0;
    case WM_COMMAND: onCommand(hwnd, LOWORD(wp)); return 0;
    case WM_TIMER: updateTooltip(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg == g_taskbarCreated) {  // Explorer restarted: re-add the icon
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\motu-audio-router");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    logOpen();
    configLoad(g_cfg, g_mute);
    audioSetMute(g_mute);
    if (startupEnabled()) startupSet(true);  // keep the Run entry pointing at this exe if it moved
    appLog("started: in %ld-%ld%s -> out %ld-%ld, buffer %ld%s", g_cfg.inFirst + 1, g_cfg.inFirst + 2,
           g_cfg.mixPc ? " + PC audio" : "", g_cfg.outFirst + 1, g_cfg.outFirst + 2, g_cfg.bufSize,
           g_mute ? ", muted" : "");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MotuAudioRouter";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MOTU Audio Router", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // Windows' own "Speakers" icon from the Sound control panel.
    wchar_t mmres[MAX_PATH];
    GetSystemDirectoryW(mmres, MAX_PATH);
    wcscat_s(mmres, L"\\mmres.dll");
    if (SHDefExtractIconW(mmres, 1, 0, nullptr, &g_nid.hIcon, MAKELONG(0, GetSystemMetrics(SM_CXSMICON))) != S_OK)
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    audioStart(hwnd, g_cfg);
    updateTooltip();
    SetTimer(hwnd, TIMER_STATUS, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    audioStop();
    appLog("quit");
    CloseHandle(mutex);
    return 0;
}
