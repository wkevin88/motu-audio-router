#pragma once
#include "audio.h"

// Files live in %APPDATA%\motu-audio-router.
void logOpen();  // keeps the previous session's log as log.old.txt
void logShow();  // opens log.txt in the default editor
void configLoad(AudioConfig& cfg, bool& mute);
void configSave(const AudioConfig& cfg, bool mute);
