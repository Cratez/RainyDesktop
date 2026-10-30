// What happens beyond the glass: showers that come and go, lightning, and the
// rain falling outside the window. Runs on the render thread next to RainSim.
//
//   - Showers: the rain rate wanders on its own, as a slow random walk in
//     log space, so the rain builds, eases off and comes back.
//   - Lightning: each flash is a train of return strokes with realistic
//     timing (about 60 ms apart, each fading in tens of milliseconds, often
//     with a dimmer glow of continuing current), or a softer flickering
//     cloud flash. Its thunder follows at the speed of sound.
//   - Rain outside: drops falling at their terminal speed through the space
//     behind the glass, seen in perspective and slightly out of focus, drawn
//     as the short streaks the eye (or a camera) sees. In gusty wind, sheets
//     of heavier rain sweep past.
//   - Daylight: how bright the sky is and what colour, from where the sun is
//     for your clock and region (or set by hand), darker under heavy rain.
//     Drops show it as the bright crescent along their lower edge.
#pragma once

#include <random>
#include <vector>

// One falling drop as the GPU draws it: a soft line from (x0, y0) to (x1, y1).
struct Streak {
    float x0, y0, x1, y1;  // pixels
    float width;           // full width at half maximum, pixels
    float alpha;           // peak coverage
    float pad[2];
};
static_assert(sizeof(Streak) == 32, "matches the HLSL struct");

struct ThunderEvent {
    float delay;     // seconds from now until it is heard
    float distance;  // km
    float power;     // 0..1, how big the stroke was
    float pan;       // -1 left .. 1 right
    float rattle;    // 0..1, how hard it shakes the window
};

struct SkyColor {
    float r, g, b;
};

class Sky {
public:
    void Reset(float width, float height);
    // windNow: the wind along the glass right now (RainSim units, about -3..3).
    void Update(float dt, float windNow);

    // Tunables.
    float rain = 1.0f;       // the user's rain amount
    float showers = 1.0f;    // how much the rain comes and goes, 0 = steady
    float outside = 1.0f;    // how much rain to show falling outside, 0 = none
    float lightning = 0.0f;  // how often lightning flashes, 0 = never
    float gusts = 1.0f;      // how gusty the wind is, 0..2, for sheets of rain
    float daylight = -1.0f;  // 0 = night .. 1 = day; negative follows the sun
    bool raining = true;     // false while paused

    // Where the sun is, for daylight that follows it: day of the year (1 =
    // 1 January), the time in hours UTC, and degrees east and north.
    void SetClock(int dayOfYear, float utcHours, float longitude, float latitude);

    // Rain rate multiplier from passing showers, around 1.
    float Showers() const { return showerNow_; }
    // Rain rate in mm per hour, for sizes, speeds and sound.
    float RainRate() const;
    // Lightning brightness right now (0 = none, about 1 for a close strike)
    // and where it is, as a direction on the screen pointing toward the flash.
    float Flash() const { return flash_; }
    float FlashDirX() const { return flashDirX_; }
    float FlashDirY() const { return flashDirY_; }
    // Thunder from flashes since the last call.
    std::vector<ThunderEvent> TakeThunder();
    // How hard close thunder is shaking the pane right now, 0..1.
    float Shake() const { return shake_; }

    // 0 = night .. 1 = full day, and the colour of the light from the sky
    // that the drops refract.
    float Daylight() const { return daylightNow_; }
    SkyColor Light() const { return sky_; }
    // The sun's height above the horizon in degrees.
    static float SunElevation(int dayOfYear, float utcHours, float longitude, float latitude);

    const std::vector<Streak>& Streaks() const { return streaks_; }

private:
    struct Stroke {
        float at;     // seconds after the flash starts
        float peak;   // brightness
        float decay;  // seconds
        float glow;   // continuing current: brightness held after the stroke
        float glowFor;
    };
    struct Bolt {
        float age = 0;
        float dirX = 0, dirY = -1;
        std::vector<Stroke> strokes;
        float length = 0;  // seconds until it is dark again
    };
    struct Drop {
        float x, y, z;  // metres: across, down, and depth behind the glass
        float radius;   // metres
        float speed;    // terminal fall speed, m/s
        float drift;    // personal sideways drift, m/s
    };

    float Rand(float lo = 0, float hi = 1) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }
    void UpdateShowers(float dt);
    void UpdateLightning(float dt);
    void StartBolt();
    void UpdateOutside(float dt, float windNow);
    void PlaceDrop(Drop& d, bool anywhere);
    void UpdateLight(float dt);
    void UpdateShake(float dt);

    std::mt19937 rng_{0x51c7};
    float w_ = 0, h_ = 0;
    float time_ = 0;

    float showerLog_ = 0;   // the random walk
    float showerNow_ = 1;
    float boltTimer_ = 0;   // seconds until the next flash may start
    std::vector<Bolt> bolts_;
    float flash_ = 0, flashDirX_ = 0, flashDirY_ = -1;
    std::vector<ThunderEvent> thunder_;
    struct Rumble {
        float wait, strength;
    };
    std::vector<Rumble> rumbles_;  // thunder on its way that will shake the pane
    float shake_ = 0;

    float sunElevation_ = 30;  // degrees, from SetClock
    float daylightNow_ = 1;
    SkyColor sky_{0.80f, 0.84f, 0.90f};
    float sheetShift_ = 0;  // metres the wind has carried the sheets of rain

    std::vector<Drop> drops_;
    std::vector<Streak> streaks_;
};
