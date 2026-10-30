// Tray icon, tray menu and settings window. Settings persist in raindesk.ini
// next to the exe.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <functional>

struct Settings {
    float rain = 1.0f;       // rain rate multiplier
    float mist = 1.0f;       // fine mist multiplier
    float friction = 1.0f;   // how hard the glass grips drops
    float runSpeed = 1.0f;   // how fast running drops travel
    float minDrop = 2.5f;    // smallest raindrop radius, pixels at 1440p
    float maxDrop = 11.5f;   // largest raindrop radius, pixels at 1440p
    float streakLife = 1.0f; // how long rivulet trails last before they break up
    int autoWipeMinutes = 0; // 0 = off
    bool paused = false;     // no new rain
    bool hidden = false;     // overlay hidden
};

struct ControlCallbacks {
    std::function<void()> wipe;        // run the wiper
    std::function<void()> clearNow;    // remove all water instantly
    std::function<void()> screenshot;
    std::function<void()> quit;
    std::function<void()> changed;     // any setting changed
};

namespace controls {

void Init(HINSTANCE inst, const std::filesystem::path& iniPath, Settings& settings, ControlCallbacks callbacks);
void Shutdown();
void OpenSettings();
void Save();
HICON AppIcon();

}  // namespace controls
