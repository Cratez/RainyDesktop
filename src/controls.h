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
    float trailSpeed = 1.0f; // how fast a drop must run before it leaves a trail
    float beadSpeed = 1.0f;  // how fast broken-up trails pull themselves into beads
    float splash = 1.0f;     // how much raindrops splash on impact, 0..2
    float refraction = 1.0f; // how strongly drops bend what is behind them
    float wind = 0.0f;       // steady wind along the glass, + blows right, -3..3
    float gusts = 1.0f;      // how much the wind gusts around that, 0..2
    float weather = 0.7f;    // how much the rain comes and goes on its own, 0 = steady
    float outside = 1.0f;    // rain falling outside, behind the glass, 0 = off
    float lightning = 0.0f;  // how often lightning flashes, 0 = never
    float flash = 1.0f;      // how bright the flashes are
    float shake = 1.0f;      // how hard close thunder shakes the window
    float daylight = -0.1f;  // 0 = night .. 1 = day; negative follows the sun
    float volume = 0.0f;     // sound volume, 0 = silent
    float glassSound = 1.0f; // mix: drops tapping on the glass
    float outsideSound = 1.0f; // mix: rain falling outside
    float thunder = 1.0f;    // mix: thunder after lightning
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
