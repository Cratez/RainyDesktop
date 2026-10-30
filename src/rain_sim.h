// Rain-on-glass simulation. Runs on the CPU and produces three lists of
// splats per frame that the GPU turns into a water surface:
//   drops  - every drop on the glass, redrawn each frame (merged by blending)
//   mist   - new tiny droplets, stamped once into a persistent texture
//   erase  - where drops wiped the mist off the glass this frame
//
// The behaviour follows how water actually sits on a vertical pane:
//   - Contact-angle hysteresis pins a drop until gravity (volume, ~r^3) beats
//     the pinning force along its contact line (~r). Every spot on the glass
//     pins a little differently (dirt, coatings), so some drops hang on far
//     past the usual size and then let go all at once.
//   - Once moving, a drop keeps going until it has shed enough water to pin
//     again (the receding threshold is lower than the advancing one).
//   - Moving drops prefer wet glass: earlier runs leave a film that has
//     almost no hysteresis, so later drops follow the same meandering paths
//     faster and more smoothly. On dry glass they stutter (stick-slip).
//   - Running on dry glass costs water: the rear contact line breaks the
//     trailing film into a line of small beads.
//   - Merging conserves momentum, so a fast drop lurches when it swallows a
//     still one, then speeds up because it is heavier.
#pragma once

#include <cstdint>
#include <random>
#include <vector>

struct Splat {
    float x, y;    // center, pixels
    float rx, ry;  // sprite half-size, pixels
    float height;  // peak water thickness, pixels
    float pad[3];
};
static_assert(sizeof(Splat) == 32, "matches the HLSL struct");

class RainSim {
public:
    void Reset(float width, float height);
    void Clear();  // wipe the glass
    void Update(float dt);
    void AddDrop(float x, float y, float r) { Spawn(x, y, r); }  // for tests and tools

    float intensity = 1.0f;  // rain rate multiplier

    const std::vector<Splat>& Drops() const { return drops_; }
    const std::vector<Splat>& Mist() const { return mist_; }
    const std::vector<Splat>& Erase() const { return erase_; }
    size_t DropCount() const { return live_.size(); }

private:
    struct Drop {
        float x, y, r;
        float pin = 1;          // this drop's grip relative to the surface's, from contact line luck
        float vx = 0, vy = 0;   // pixels per second
        float heading = 0;      // radians from straight down, positive = to the right
        float stickTimer = 0;   // > 0 while a running drop is momentarily snagged
        float trailDist = 0;    // distance left until the next trail bead
        float steerDist = 0;    // distance left until the next path decision
        float eraseTimer = 0;   // resting drops slowly soak up mist around them
        float targetHeading = 0;
        // A resting drop's outline is the union of up to four blobs. When two
        // drops coalesce their blobs are kept, and because the contact line is
        // pinned the combined shape only slowly pulls itself round, so merged
        // drops stay lumpy, peanut-shaped or ragged for a long time.
        struct Lobe {
            float dx, dy, r;
        };
        Lobe lobes[4] = {};
        int lobeCount = 0;
        bool streaky = false;   // this run drags a continuous film instead of only beads
        bool running = false;
        bool dead = false;
    };

    float Rand(float lo = 0, float hi = 1) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }
    void Impact(float x, float y, float r);
    void Spawn(float x, float y, float r);
    void AddMist(float x, float y, float r);
    void BuildGrid();
    template <typename F> void ForNeighbors(float x, float y, float radius, F&& f);
    void Merge(Drop& into, Drop& from);
    void ResetShape(Drop& d, bool ragged);
    void Steer(Drop& d);

    // Glass surface, on a coarse grid.
    void BuildSurface();
    int SurfaceCell(float x, float y) const;
    float Affinity(float x, float y) const;  // 0..1, how strongly the spot holds water
    float Wetness(float x, float y) const;   // 0..1, film left by earlier drops, dries over time
    void Wet(float x, float y, float radius);

    float w_ = 0, h_ = 0, s_ = 1;  // screen size, and scale relative to 1440p
    float time_ = 0;
    float dropAccum_ = 0, mistAccum_ = 0;
    std::mt19937 rng_{std::random_device{}()};
    std::vector<Drop> live_;

    float cell_ = 32;
    int gridW_ = 0, gridH_ = 0;
    std::vector<int> gridHead_, gridNext_;

    float surfCell_ = 6;
    int surfW_ = 0, surfH_ = 0;
    std::vector<float> affinity_;
    std::vector<float> wetTime_;  // when each cell was last wetted

    std::vector<Splat> drops_, mist_, erase_;
};
