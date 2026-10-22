#pragma once
#include <windows.h>
#include <string>

struct AudioConfig {
    std::string driver = "MOTU M Series";
    long inFirst = 2;   // 0-based first channel of the input pair (2 = In 3-4)
    long outFirst = 2;  // 0-based first channel of the output pair (2 = Out 3-4)
    long bufSize = 32;  // requested ASIO buffer size in samples
};

struct AudioStatus {
    bool running = false;
    long numIn = 0, numOut = 0;
    long bufSize = 0;
    double sampleRate = 0;
    double latencyMs = 0;  // round trip reported by the driver
};

// Starts the engine thread, which opens the driver and keeps it open (retrying and
// re-initialising as needed) until audioStop(). owner is passed to the driver's init().
void audioStart(HWND owner, const AudioConfig& cfg);
void audioStop();
void audioSetConfig(const AudioConfig& cfg);  // re-opens the driver if anything changed
void audioSetMute(bool mute);
void audioOpenControlPanel();
AudioStatus audioStatus();

// Provided by the application.
void appLog(const char* fmt, ...);
