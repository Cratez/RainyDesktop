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
//     trailing film into a line of small beads, or the drop drags a thin
//     rivulet behind it. A rivulet drains from its old end, then pinches into
//     segments (Plateau-Rayleigh) that pull up into a line of droplets.
//   - Merging conserves momentum, so a fast drop lurches when it swallows a
//     still one, then speeds up because it is heavier.
//   - A jolt (a raindrop landing on or splashing into a drop, or the pane
//     shaking in close thunder) helps a drop past its hysteresis for a
//     moment, so drops that were nearly heavy enough to run are knocked loose.
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
    void AddRaindrop(float x, float y, float r) { Impact(x, y, r); }  // a landing raindrop; for tests
    float WaterOnGlass() const;  // drops and rivulets, as volume (sum of r^3); for tests
    float WindNow() const { return windNow_; }  // the wind along the glass this frame, gusts included

    // Raindrops that hit the glass this frame, for the sound.
    struct Hit {
        float x, y;    // pixels
        float r;       // radius, pixels at this resolution
        float glance;  // 0 = head on .. 1 = fast and steep
        bool splash;   // a big drop that splashed
    };
    const std::vector<Hit>& Hits() const { return hits_; }

    // Tunables, all 1.0 = default.
    float intensity = 1.0f;  // how hard it rains
    float mist = 1.0f;       // how much fine mist settles
    float friction = 1.0f;   // how hard the glass holds drops before they run
    float runSpeed = 1.0f;   // how fast running drops travel
    float minDrop = 2.5f;    // smallest raindrop radius, pixels at 1440p
    float maxDrop = 11.5f;   // largest raindrop radius, pixels at 1440p
    float streakLife = 1.0f; // how long rivulets last before they drain and break up
    float trailSpeed = 1.0f; // how fast a drop must run before it draws out a rivulet
    float beadSpeed = 1.0f;  // how fast a broken-up rivulet pulls itself into beads
    float splash = 1.0f;     // how much raindrops splash on impact, 0 = they just appear
    float wind = 0.0f;       // steady wind along the glass, + blows to the right; about -3..3
    float gusts = 1.0f;      // how much the wind gusts and lulls around that, 0..2
    float shake = 0.0f;      // how hard the pane is shaking right now (close thunder), 0..1

    // A windscreen wiper pivoting below the bottom edge sweeps across and
    // back, pushing water ahead of the blade and off the screen.
    struct Wiper {
        bool active = false;
        float angle = 0;           // radians from straight up, positive = right
        float pivotX = 0, pivotY = 0;
        float inner = 0, outer = 0;  // blade span, distance from the pivot
    };
    void StartWipe();
    const Wiper& GetWiper() const { return wiper_; }

    const std::vector<Splat>& Drops() const { return drops_; }
    const std::vector<Splat>& Mist() const { return mist_; }
    const std::vector<Splat>& Erase() const { return erase_; }
    size_t DropCount() const { return live_.size(); }

private:
    struct Drop {
        float x, y, r;
        float pin = 1;          // this drop's grip relative to the surface's, from contact line luck
        float release = 0;      // radius at which this drop let go; it slides while above 80% of it
        float vx = 0, vy = 0;   // pixels per second
        float heading = 0;      // radians from straight down, positive = to the right
        float stickTimer = 0;   // > 0 while a running drop is momentarily snagged
        float trailDist = 0;    // distance left until the next trail bead
        float steerDist = 0;    // distance left until the next path decision
        float eraseTimer = 0;   // resting drops slowly soak up mist around them
        float targetHeading = 0;
        float jolt = 0;         // fraction of its grip a recent knock has shaken loose, decays fast
        // A resting drop's outline is the union of up to four blobs. When two
        // drops coalesce their blobs are kept, and because the contact line is
        // pinned the combined shape only slowly pulls itself round, so merged
        // drops stay lumpy, peanut-shaped or ragged for a long time.
        struct Lobe {
            float dx, dy, r;
        };
        Lobe lobes[4] = {};
        int lobeCount = 0;
        int rivulet = -1;       // index of the rivulet this drop is drawing out, or -1
        bool running = false;
        bool dead = false;
    };

    float Rand(float lo = 0, float hi = 1) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }
    void Impact(float x, float y, float r);
    void Knock(float x, float y, float reach, float r, float glance);
    void Spawn(float x, float y, float r);
    void AddMist(float x, float y, float r);
    void BuildGrid();
    void UpdateWiper(float dt);
    template <typename F> void ForNeighbors(float x, float y, float radius, F&& f);
    void Merge(Drop& into, Drop& from);
    void ResetShape(Drop& d, bool ragged);
    void Steer(Drop& d);
    void UpdateWind(float dt);
    float WindAt(float x, float y) const;    // wind along the glass at a point, same scale as `wind`
    float Tilt(const Drop& d) const;         // sideways push on a drop relative to its weight
    void UpdateRivulets(float dt);
    float EraseRivulets(float x, float y, float radius, int skip = -1);  // returns the water soaked up

    // A thin line of water a running drop leaves behind it, as a chain of
    // nodes about a pixel apart.
    struct RivuletNode {
        float x, y;
        float w0;      // half-width when laid down, pixels
        float water;   // water held, as volume in the same units as a drop's r^3
        float water0;  // water when laid down
        float born;    // time_ when laid down
        float along;   // distance from the rivulet's start, pixels
        float w = 0;   // current half-width
        // Beading up: a segment pulls back from both ends into a bead at its
        // middle. gather is the index of that middle node; the middle node
        // holds the bead's water in pool and how far the ends have to go in retract.
        int gather = -1;
        float pool = 0;
        float retract = 0;
        bool alive = true;
    };
    struct Rivulet {
        std::vector<RivuletNode> nodes;
        float wavelength;  // spacing of the segments it pinches into
        float phase;
        float wobble;      // phase of the sideways meander
        float carry = 0;   // water the drop has given up that no node holds yet
        float minX, minY, maxX, maxY;  // bounds, for cheap overlap tests
        bool open = true;  // still being drawn out by its drop
    };
    void LayRivulet(Drop& d, float step, float excess);

    // A big raindrop's impact, which plays out over a fraction of a second:
    // the drop flattens into a thin sheet thrown out mostly in the direction
    // it was travelling (downward, and with the wind), flings a spray of
    // micro droplets off the rim, then one of three things happens: surface
    // tension pulls the sheet back into a drop at the top, the rim breaks up
    // and the sheet scatters into separate drops, or the drop came in so fast
    // and steep that most of it keeps going and shoots off down the glass.
    enum class Fate { Pool, Scatter, Shoot };
    struct Splash {
        Fate fate = Fate::Pool;
        float x, y;        // where it hit
        float dx, dy;      // direction of travel along the glass, unit length
        float glance;      // 0 = head on, 1 = arrived at a steep angle and speed
        float r;           // radius of the drop that landed
        float volume;      // water still in the sheet
        float age = 0;
        float duration;
        struct Arm {
            float angle, length, width;
        };
        Arm arms[8];
        int armCount = 0;
        bool sprayed = false, torn = false;
    };
    void Launch(float x, float y, float r, float speed, float heading);  // a drop already running at speed
    void StartSplash(float x, float y, float r, float dx, float dy, float glance);
    void UpdateSplashes(float dt);
    float SplashReach(const Splash& sp) const;  // how far out the sheet is now, 0..1
    void StartBead(Rivulet& rv, int a, int b);
    void Gather(Rivulet& rv, float dt);

    // Glass surface, on a coarse grid.
    void BuildSurface();
    int SurfaceCell(float x, float y) const;
    float Affinity(float x, float y) const;  // 0..1, how strongly the spot holds water
    float Wetness(float x, float y) const;   // 0..1, film left by earlier drops, dries over time
    void Wet(float x, float y, float radius);

    float w_ = 0, h_ = 0, s_ = 1;  // screen size, and scale relative to 1440p
    float time_ = 0;
    float dropAccum_ = 0, mistAccum_ = 0;
    float runnerAccum_ = 0;              // drops running down into view from above the screen
    float gustSlow_ = 0, gustFast_ = 0;  // smoothed random gust state
    float windNow_ = 0;                  // wind with gusts, this frame
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
    std::vector<Rivulet> rivulets_;
    std::vector<Splash> splashes_;
    std::vector<Hit> hits_;
    Wiper wiper_;
    float wipeTime_ = 0, wipePrevAngle_ = 0;
};
