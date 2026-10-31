#include "rain_sim.h"

#include <algorithm>
#include <cmath>

namespace {

// Sprites extend past the visible edge so neighbouring drops blend into
// each other smoothly; the composite pass finds the real edge by threshold.
constexpr float kSpriteScale = 1.25f;
constexpr float kHeightPerRadius = 0.5f;
constexpr size_t kMaxDrops = 8000;

// Sizes at 1440p (about 0.233 mm per pixel on a 27" panel), scaled by s_.
//
// Where a drop lets go (Furmidge): gravity rho*g*V against the contact line's
// grip gamma*w*(cos theta_r - cos theta_a). For water on window glass,
// theta_a ~ 50-60 deg and theta_r ~ 20-30 deg give a grip of about
// 0.022 N per metre of width. A spherical cap with a ~40 deg contact angle has
// V ~ 0.6 a^3 and w = 2a, so the balance lands at a contact radius of
// a ~ 2.7 mm: drops bigger than ~5-6 mm across cannot hang on a vertical pane.
constexpr float kPinRadius = 11.0f;    // ~2.6 mm contact radius
// Sliding speed grows with how far gravity exceeds the grip (capillary number
// ~ Bond number minus its critical value), roughly v = v0 * ((r / r_slide)^2 - 1).
// v0 ~ 8 cm/s: a drop barely past the threshold creeps at 1-2 cm/s with
// stick-slip, while one twice the size is gone at 20+ cm/s.
constexpr float kBaseSpeed = 340.0f;   // v0, px/s
constexpr float kMaxSpeed = 1800.0f;   // px/s (about 40 cm/s)
constexpr float kMistMaxRadius = 1.6f;  // anything smaller lives in the mist texture
constexpr float kDryTime = 120.0f;     // seconds for a wet path to mostly dry
constexpr float kMaxRadius = 2.2f;     // running drops never grow past this many pin radii
// A sliding drop's rear contact line can only recede so fast. Slow drops keep
// a rounded back; faster ones draw it into a corner, and past a critical
// speed the corner stretches into a rivulet that pearls into beads
// (Podgorski, Flesselles & Limat 2001). The critical speed is lower where the
// glass grips water harder.
constexpr float kTrailSpeed = 400.0f;  // px/s, about 9 cm/s on clean glass

// Water is counted by volume. Drops on glass keep roughly the same contact
// angle whatever their size, so a drop's volume goes as the cube of its radius.
float Volume(float r) { return r * r * r; }
float RadiusFor(float volume) { return std::cbrt(std::max(volume, 0.0f)); }

Splat MakeSplat(float x, float y, float rx, float ry, float height, float weight = 1.0f, float rough = 0.0f,
                float seed = 0.0f) {
    Splat s{};
    s.x = x;
    s.y = y;
    s.rx = rx;
    s.ry = ry;
    s.height = height;
    s.weight = weight;
    s.rough = rough;
    s.seed = seed;
    return s;
}

// An angle brought into -pi..pi.
float WrapAngle(float a) {
    constexpr float kTwoPi = 6.2831853f;
    return a - kTwoPi * std::floor((a + 3.1415927f) / kTwoPi);
}

float Smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// How long a running drop's tail takes to draw out and to pull back in, seconds.
constexpr float kTailGrow = 0.06f;
constexpr float kTailShrink = 0.3f;
// How long a lump takes to drain into a running drop's head, seconds.
constexpr float kDrain = 0.12f;
// Half the width of a fingertip pressed on the glass, about 7 mm.
constexpr float kFingerHalf = 30.0f;

}  // namespace

void RainSim::Reset(float width, float height) {
    w_ = width;
    h_ = height;
    s_ = height / 1440.0f;
    cell_ = 32.0f * s_;
    gridW_ = static_cast<int>(std::ceil(w_ / cell_)) + 1;
    gridH_ = static_cast<int>(std::ceil(h_ / cell_)) + 1;
    BuildSurface();
    Clear();
}

void RainSim::Clear() {
    live_.clear();
    drops_.clear();
    mist_.clear();
    erase_.clear();
    rivulets_.clear();
    splashes_.clear();
    std::fill(wetTime_.begin(), wetTime_.end(), -1e9f);
    dropAccum_ = mistAccum_ = 0;
}

// ---------------------------------------------------------------- surface

void RainSim::BuildSurface() {
    surfCell_ = 6.0f * s_;
    surfW_ = static_cast<int>(std::ceil(w_ / surfCell_)) + 1;
    surfH_ = static_cast<int>(std::ceil(h_ / surfCell_)) + 1;

    // Two octaves of value noise: broad patches plus fine grime.
    auto octave = [&](int step, float weight, std::vector<float>& out) {
        int lw = surfW_ / step + 2, lh = surfH_ / step + 2;
        std::vector<float> lattice(static_cast<size_t>(lw) * lh);
        for (float& v : lattice) v = Rand();
        for (int y = 0; y < surfH_; ++y)
            for (int x = 0; x < surfW_; ++x) {
                float fx = static_cast<float>(x) / step, fy = static_cast<float>(y) / step;
                int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                float tx = fx - ix, ty = fy - iy;
                tx = tx * tx * (3 - 2 * tx);
                ty = ty * ty * (3 - 2 * ty);
                auto at = [&](int a, int b) { return lattice[static_cast<size_t>(b) * lw + a]; };
                float v = (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) +
                          (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
                out[static_cast<size_t>(y) * surfW_ + x] += v * weight;
            }
    };
    affinity_.assign(static_cast<size_t>(surfW_) * surfH_, 0.0f);
    octave(12, 0.65f, affinity_);
    octave(3, 0.35f, affinity_);
    wetTime_.assign(affinity_.size(), -1e9f);
}

int RainSim::SurfaceCell(float x, float y) const {
    int cx = std::clamp(static_cast<int>(x / surfCell_), 0, surfW_ - 1);
    int cy = std::clamp(static_cast<int>(y / surfCell_), 0, surfH_ - 1);
    return cy * surfW_ + cx;
}

float RainSim::Affinity(float x, float y) const { return affinity_[SurfaceCell(x, y)]; }

float RainSim::Wetness(float x, float y) const {
    if (y >= h_) return 0;
    float age = time_ - wetTime_[SurfaceCell(x, y)];
    // Film younger than this is almost always the asking drop's own wake.
    if (age < 0.75f) return 0;
    return std::exp(-age / kDryTime);
}

void RainSim::Wet(float x, float y, float radius) {
    int x0 = std::clamp(static_cast<int>((x - radius) / surfCell_), 0, surfW_ - 1);
    int x1 = std::clamp(static_cast<int>((x + radius) / surfCell_), 0, surfW_ - 1);
    int y0 = std::clamp(static_cast<int>((y - radius) / surfCell_), 0, surfH_ - 1);
    int y1 = std::clamp(static_cast<int>((y + radius) / surfCell_), 0, surfH_ - 1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) wetTime_[static_cast<size_t>(cy) * surfW_ + cx] = time_;
}

// ---------------------------------------------------------------- drops

void RainSim::BuildGrid() {
    gridHead_.assign(static_cast<size_t>(gridW_) * gridH_, -1);
    gridNext_.assign(live_.size(), -1);
    for (int i = 0; i < static_cast<int>(live_.size()); ++i) {
        const Drop& d = live_[i];
        int cx = std::clamp(static_cast<int>(d.x / cell_), 0, gridW_ - 1);
        int cy = std::clamp(static_cast<int>(d.y / cell_), 0, gridH_ - 1);
        int& head = gridHead_[static_cast<size_t>(cy) * gridW_ + cx];
        gridNext_[i] = head;
        head = i;
    }
}

template <typename F>
void RainSim::ForNeighbors(float x, float y, float radius, F&& f) {
    if (gridHead_.empty()) return;  // before the first Update
    int x0 = std::clamp(static_cast<int>((x - radius) / cell_), 0, gridW_ - 1);
    int x1 = std::clamp(static_cast<int>((x + radius) / cell_), 0, gridW_ - 1);
    int y0 = std::clamp(static_cast<int>((y - radius) / cell_), 0, gridH_ - 1);
    int y1 = std::clamp(static_cast<int>((y + radius) / cell_), 0, gridH_ - 1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
            for (int j = gridHead_[static_cast<size_t>(cy) * gridW_ + cx]; j >= 0; j = gridNext_[j]) f(j);
}

// Volumes add. Speed follows momentum, so a running drop slows when it
// swallows a still one.
void RainSim::Merge(Drop& into, Drop& from) {
    float a = Volume(into.r), b = Volume(from.r);
    float speed = (into.vy * a + from.vy * b) / (a + b);
    float r = std::min(RadiusFor(a + b), kMaxRadius * kPinRadius * s_ * std::max(friction, 1.0f));
    from.dead = true;
    into.vy = speed;
    into.rough = std::max(into.rough, from.rough);
    if (into.running) {
        if (into.flung) {
            // Momentum adds up as vectors.
            float fx = from.flung ? from.fvx : from.running ? std::sin(from.heading) * from.vy : 0.0f;
            float fy = from.flung ? from.fvy : from.running ? std::cos(from.heading) * from.vy : 0.0f;
            into.fvx = (into.fvx * a + fx * b) / (a + b);
            into.fvy = (into.fvy * a + fy * b) / (a + b);
        }
        // The swallowed drop shows for a moment as a lump on the runner,
        // which drains into its head.
        if (into.lobeCount < Drop::kMaxLobes) into.lobes[into.lobeCount++] = {from.x - into.x, from.y - into.y, from.r};
        into.r = r;
        return;
    }

    // Keep both drops' blobs, re-centred on the combined centre of mass.
    float cx = (into.x * a + from.x * b) / (a + b), cy = (into.y * a + from.y * b) / (a + b);
    Drop::Lobe all[2 * Drop::kMaxLobes];
    int n = 0;
    for (int k = 0; k < into.lobeCount; ++k)
        all[n++] = {into.lobes[k].dx + into.x - cx, into.lobes[k].dy + into.y - cy, into.lobes[k].r};
    for (int k = 0; k < from.lobeCount; ++k)
        all[n++] = {from.lobes[k].dx + from.x - cx, from.lobes[k].dy + from.y - cy, from.lobes[k].r};
    if (from.lobeCount == 0) all[n++] = {from.x - cx, from.y - cy, from.r};
    // Too many blobs: fold the two closest together (area-weighted).
    while (n > Drop::kMaxLobes) {
        int bi = 0, bj = 1;
        float best = 1e30f;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                float dx = all[i].dx - all[j].dx, dy = all[i].dy - all[j].dy;
                if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, bi = i, bj = j;
            }
        float wa = all[bi].r * all[bi].r, wb = all[bj].r * all[bj].r;
        all[bi] = {(all[bi].dx * wa + all[bj].dx * wb) / (wa + wb), (all[bi].dy * wa + all[bj].dy * wb) / (wa + wb),
                   std::sqrt(wa + wb)};
        all[bj] = all[--n];
    }
    // Scale blob sizes so their areas add up to the drop's, and keep them
    // overlapping so the outline stays one piece.
    float area = 0;
    for (int k = 0; k < n; ++k) area += all[k].r * all[k].r;
    float scale = r / std::sqrt(std::max(area, 1e-6f));
    into.lobeCount = n;
    for (int k = 0; k < n; ++k) {
        float dx = all[k].dx, dy = all[k].dy, len = std::sqrt(dx * dx + dy * dy), lim = r * 0.6f;
        if (len > lim) dx *= lim / len, dy *= lim / len;
        into.lobes[k] = {dx, dy, all[k].r * scale};
    }
    into.x = cx;
    into.y = cy;
    into.r = r;
}

// A fresh drop is roughly round. Larger ones bulge where the contact line
// snagged as it spread: one bump or two, more often on bigger drops.
void RainSim::ResetShape(Drop& d, bool ragged) {
    d.lobeCount = 1;
    d.lobes[0] = {0, 0, d.r};
    if (!ragged || d.r <= 3.0f * s_ || Rand() < 0.15f) return;
    float big = std::clamp((d.r / s_ - 3.0f) / 6.0f, 0.0f, 1.0f);
    int bumps = Rand() < 0.3f + 0.4f * big ? 2 : 1;
    float area = d.r * d.r, cx = 0, cy = 0;
    for (int k = 0; k < bumps; ++k) {
        float br = d.r * Rand(0.3f, 0.55f), a = Rand(0, 6.2832f), off = d.r * Rand(0.4f, 0.7f);
        d.lobes[d.lobeCount++] = {std::cos(a) * off, std::sin(a) * off, br};
        area -= br * br;
        cx += std::cos(a) * off * br * br;
        cy += std::sin(a) * off * br * br;
    }
    // Blob areas add up to the drop's, centred on its centre of mass.
    d.lobes[0].r = std::sqrt(std::max(area, 0.3f * d.r * d.r));
    cx /= d.r * d.r;
    cy /= d.r * d.r;
    for (int k = 0; k < d.lobeCount; ++k) {
        d.lobes[k].dx -= cx;
        d.lobes[k].dy -= cy;
    }
}

// Every drop's outline is its own: the contact line catches on specks and
// scratches, so bigger drops are never quite round. Small ones are.
void RainSim::NewDrop(Drop& d) {
    d.seed = Rand(0, 6.2832f);
    d.rough = Rand(0.15f, 0.55f) * std::clamp((d.r / s_ - 1.2f) / 4.0f, 0.0f, 1.0f);
}

void RainSim::AddMist(float x, float y, float r) {
    mist_.push_back(MakeSplat(x, y, r * kSpriteScale, r * kSpriteScale, r * kHeightPerRadius));
}

void RainSim::Spawn(float x, float y, float r) {
    if (r < kMistMaxRadius * s_) {
        AddMist(x, y, r);
        return;
    }
    if (live_.size() >= kMaxDrops) return;
    // Landing on an existing drop feeds it instead of making a new one.
    int hit = -1;
    ForNeighbors(x, y, r + 24.0f * s_, [&](int j) {
        const Drop& o = live_[j];
        if (o.dead || hit >= 0) return;
        float dx = o.x - x, dy = o.y - y;
        float reach = o.r + r;  // touching drops coalesce at once
        if (dx * dx + dy * dy < reach * reach) hit = j;
    });
    if (hit >= 0) {
        Drop incoming{x, y, r};
        Merge(live_[hit], incoming);
        return;
    }
    Drop d{x, y, r};
    // Most drops let go near the typical size; a few are stubborn and hold on
    // to nearly twice as much water before they suddenly run.
    float u = Rand();
    d.pin = 0.85f + 0.35f * u * u;
    NewDrop(d);
    ResetShape(d, true);
    live_.push_back(d);
}

// ---------------------------------------------------------------- wind

// The wind blowing along the glass. The steady setting is its general
// direction; gusts are its variability around that. Near a building the wind
// swells and lulls (by about a third of its strength) and swings in direction
// (by tens of degrees), and what pushes the water is its part along the
// glass, so a swing can carry it across the window the other way for a
// moment, even when the wind is blowing straight at the glass and has no
// part along it on average. Three smoothed random signals: strength that
// swells and lulls over several seconds, direction that swings over a few,
// and quick buffeting on both.
void RainSim::UpdateWind(float dt) {
    std::normal_distribution<float> normal(0.0f, 1.0f);
    auto drift = [&](float& v, float tau) { v += -v * dt / tau + std::sqrt(2.0f * dt / tau) * normal(rng_); };
    drift(gustSlow_, 5.0f);
    drift(gustFast_, 0.7f);
    drift(gustSwing_, 4.0f);
    float strength = gusts * (0.4f * gustSlow_ + 0.2f * gustFast_);
    // The swing scales with how windy it is overall, which is at least a breeze in a storm.
    float swing = gusts * (0.35f + 0.25f * std::min(std::fabs(wind), 3.0f)) * (0.85f * gustSwing_ + 0.12f * gustFast_);
    windNow_ = wind * std::clamp(1.0f + strength, 0.2f, 2.2f) + swing;
    // Gust fronts travel with the wind.
    sweep_ += dt * 1.7f * std::clamp(windNow_, -1.0f, 1.0f);
}

// Gust fronts sweep across the pane, so the wind is never quite the same
// everywhere at once.
float RainSim::WindAt(float x, float y) const {
    float sweep = std::sin(x * 0.003f / s_ - sweep_ + y * 0.001f / s_);
    return windNow_ + gusts * (0.1f + 0.2f * std::fabs(windNow_)) * sweep;
}

// Wind drag on a drop grows with its frontal area (r^2), its weight with its
// volume (r^3), so the wind leans on small drops relatively harder.
float RainSim::Tilt(const Drop& d) const {
    float pinR = kPinRadius * s_ * friction;
    return std::clamp(0.5f * WindAt(d.x, d.y) * pinR / std::max(d.r, 0.3f * pinR), -3.0f, 3.0f);
}

// ---------------------------------------------------------------- impacts

void RainSim::Impact(float x, float y, float r) {
    // Raindrops arrive along the glass as well as into it: part of their fall
    // and, in wind, a sideways push. Big drops fall fast (terminal speed
    // grows with size) and carry that motion into the glass; fine spray
    // drifts in and lands softly, so the wind steers big drops far more.
    float w = WindAt(x, y);
    float heft = std::clamp((r / s_ - 2.0f) / 6.0f, 0.0f, 1.0f);
    float dx = w * heft * Rand(0.6f, 1.2f) + Rand(-0.25f, 0.25f), dy = Rand(0.4f, 1.0f);
    float len = std::sqrt(dx * dx + dy * dy);
    dx /= len;
    dy /= len;
    // Most hit fairly square on; now and then one comes in fast and steep.
    float glance = std::clamp(Rand() * Rand() * (0.7f + 0.8f * std::fabs(w) * heft), 0.0f, 1.0f);

    bool splashes = r > 3.5f * s_ && Rand() < std::min(splash * 0.9f, 1.0f);
    hits_.push_back({x, y, r, glance, splashes, false});
    if (splashes) {
        // Landing in a drop already on the glass throws much of that drop's
        // water out with its own (a crown splash), and such a splash bursts
        // far more easily. Wet film helps it a little too.
        float extra = 0, wet = 0.5f * Wetness(x, y);
        int into = -1;
        ForNeighbors(x, y, r + 24.0f * s_, [&](int j) {
            const Drop& o = live_[j];
            if (o.dead || o.running || into >= 0) return;
            float ox = o.x - x, oy = o.y - y;
            if (ox * ox + oy * oy < o.r * o.r) into = j;
        });
        if (into >= 0) {
            Drop& o = live_[into];
            extra = std::min(Volume(o.r) * Rand(0.3f, 0.6f), Volume(r) * 2.0f);
            float nr = RadiusFor(Volume(o.r) - extra);
            if (nr < kMistMaxRadius * s_) {
                extra = Volume(o.r);
                o.dead = true;
            } else {
                for (int k = 0; k < o.lobeCount; ++k) o.lobes[k].r *= nr / o.r;
                o.r = nr;
            }
            wet = 1.0f;
        }
        Fate fate = StartSplash(x, y, r, dx, dy, glance, extra, wet);
        hits_.back().burst = fate == Fate::Burst;
        Knock(x, y, r * (fate == Fate::Burst ? 3.5f : 2.5f), r, glance);
        return;
    }
    Knock(x, y, r, r, glance);

    // Smaller drops on a glancing path skid a few millimetres, smear into a
    // streak, and throw their splash forward.
    if (Rand() < (0.12f + 0.6f * std::clamp(std::fabs(w) / 1.5f, 0.0f, 1.0f)) * (0.3f + 0.7f * heft)) {
        float skid = r * Rand(1.0f, 2.6f) * (0.6f + 0.8f * glance);
        float hx = x + dx * skid, hy = y + dy * skid;
        // A few fine droplets are left along the skid; their water comes out of the drop.
        float left = Volume(r);
        for (int k = 0, n = 1 + static_cast<int>(Rand() * 3); k < n; ++k) {
            float t = Rand(0.1f, 0.8f), br = r * Rand(0.08f, 0.16f);
            AddMist(x + dx * skid * t, y + dy * skid * t, br);
            left -= Volume(br);
        }
        float rr = RadiusFor(std::max(left, Volume(r) * 0.5f));
        size_t before = live_.size();
        Spawn(hx, hy, rr);
        if (live_.size() > before) {
            // Smeared back along the skid: a head and a thinning tail. The
            // pinned contact line holds the streak shape for a long time.
            Drop& d = live_.back();
            d.lobes[0] = {0, 0, rr * 0.8f};
            d.lobes[1] = {-dx * skid * 0.45f, -dy * skid * 0.45f, rr * 0.5f};
            d.lobes[2] = {-dx * skid * 0.85f, -dy * skid * 0.85f, rr * 0.33f};
            d.lobeCount = 3;
        }
        return;
    }

    Spawn(x, y, r);
    if (Rand() < 0.35f * splash) {
        int n = 2 + static_cast<int>(Rand() * 4);
        for (int i = 0; i < n; ++i) {
            float a = Rand(0, 6.2832f), dist = r * Rand(1.4f, 3.5f);
            Spawn(x + std::cos(a) * dist, y + std::sin(a) * dist, r * Rand(0.1f, 0.3f));
        }
    }
}

// A raindrop landing on a drop, or splashing into one, jolts it. Most of the
// raindrop's momentum is along the glass (it was falling), far more than the
// contact line can hold for that instant, so for a moment the drop has less
// grip: one that was nearly heavy enough to run is knocked loose. The share
// of the grip lost grows with the raindrop's share of the water.
void RainSim::Knock(float x, float y, float reach, float r, float glance) {
    float v = Volume(r);
    ForNeighbors(x, y, reach + 24.0f * s_, [&](int j) {
        Drop& o = live_[j];
        if (o.dead || o.running) return;
        float dx = o.x - x, dy = o.y - y, touch = o.r + reach;
        if (dx * dx + dy * dy >= touch * touch) return;
        o.jolt = std::min(o.jolt + (0.5f + glance) * v / (v + Volume(o.r)), 0.3f);
    });
}

// ---------------------------------------------------------------- splashes

RainSim::Fate RainSim::StartSplash(float x, float y, float r, float dx, float dy, float glance, float extra,
                                   float wet) {
    Splash sp;
    sp.x = x;
    sp.y = y;
    sp.dx = dx;
    sp.dy = dy;
    sp.glance = glance;
    sp.volume = Volume(r) + extra;
    sp.r = RadiusFor(sp.volume);
    // What becomes of it. A drop that came in fast and steep keeps most of
    // its momentum along the glass and shoots off downhill. A big drop
    // hitting hard has more energy than the sheet's rim can hold together:
    // the rim breaks into separate drops, or, hardest of all, the sheet
    // bursts and throws them off in every direction. Otherwise the sheet pools.
    float big = std::clamp((r / s_ - 4.0f) / 4.0f, 0.0f, 1.0f);
    float u = Rand();
    float pShoot = (0.12f + 0.8f * glance) * dy;
    float pBurst = (1.0f - pShoot) * std::clamp(burst * (0.02f + 0.12f * big + 0.25f * wet), 0.0f, 0.95f);
    float pScatter = (1.0f - pShoot - pBurst) * (0.18f + 0.3f * big);
    sp.fate = u < pShoot                      ? Fate::Shoot
              : u < pShoot + pBurst            ? Fate::Burst
              : u < pShoot + pBurst + pScatter ? Fate::Scatter
                                               : Fate::Pool;
    // Bigger drops take a little longer to spread and gather back up. A drop
    // that shoots off is gone almost at once; a scattering or bursting sheet
    // breaks up at its widest.
    sp.duration = 0.22f + 0.012f * sp.r / s_ + 0.1f * glance;
    if (sp.fate == Fate::Shoot) sp.duration *= 0.45f;
    if (sp.fate == Fate::Scatter) sp.duration *= 0.4f;
    if (sp.fate == Fate::Burst) sp.duration *= 0.35f;
    float ahead = std::atan2(dx, dy);
    if (sp.fate == Fate::Burst) {
        // A burst throws the sheet out all round, furthest the way the drop
        // was going, so its lobes are fingers splayed in every direction.
        sp.armCount = 6 + static_cast<int>(Rand() * 3);
        float start = Rand(0, 6.2832f);
        for (int k = 0; k < sp.armCount; ++k) {
            Splash::Arm& a = sp.arms[k];
            a.angle = start + 6.2832f * (k + Rand(-0.3f, 0.3f)) / sp.armCount;
            float forward = std::cos(a.angle - ahead);
            a.length = sp.r * Rand(1.6f, 3.0f) * (1.0f + 0.4f * forward * (0.5f + glance));
            a.width = sp.r * Rand(0.25f, 0.4f);
        }
        splashes_.push_back(sp);
        return sp.fate;
    }
    // The sheet is thrown out in lobes. Most of it goes the way the drop was
    // travelling, further the faster and steeper it came in; the rest is a
    // short ruff all round.
    sp.armCount = 5 + static_cast<int>(Rand() * 3);
    for (int k = 0; k < sp.armCount; ++k) {
        Splash::Arm& a = sp.arms[k];
        if (k < 3) {
            a.angle = ahead + Rand(-0.55f, 0.55f);
            a.length = sp.r * Rand(1.8f, 3.2f) * (1.0f + 1.2f * glance);
            a.width = sp.r * Rand(0.3f, 0.45f);
        } else {
            a.angle = ahead + 3.1416f * (Rand() < 0.5f ? -1.0f : 1.0f) * Rand(0.35f, 1.0f);
            a.length = sp.r * Rand(0.8f, 1.6f) * (1.0f - 0.5f * glance);
            a.width = sp.r * Rand(0.25f, 0.4f);
        }
    }
    if (sp.fate == Fate::Shoot) sp.arms[0].angle = ahead;  // the main lobe carries on straight
    splashes_.push_back(sp);
    return sp.fate;
}

// 0 at impact, 1 at full spread, back to 0 as it pools.
float RainSim::SplashReach(const Splash& sp) const {
    float spread = sp.duration * 0.18f, hold = sp.duration * 0.3f;
    if (sp.age < spread) {
        float t = sp.age / spread;
        return 1.0f - (1.0f - t) * (1.0f - t);  // flung out fast, slowing
    }
    if (sp.age < hold || sp.fate != Fate::Pool) return 1.0f;
    float t = std::clamp((sp.age - hold) / (sp.duration - hold), 0.0f, 1.0f);
    return 1.0f - t * t * (3 - 2 * t);
}

void RainSim::UpdateSplashes(float dt) {
    for (Splash& sp : splashes_) {
        sp.age += dt;
        auto tip = [&](const Splash::Arm& a, float reach, float& tx, float& ty) {
            tx = sp.x + std::sin(a.angle) * a.length * reach;
            ty = sp.y + std::cos(a.angle) * a.length * reach;
        };

        // As the sheet reaches full spread its rim breaks into a spray of
        // micro droplets, flung on past the rim. Their water leaves the sheet.
        if (!sp.sprayed && sp.age > sp.duration * 0.12f) {
            sp.sprayed = true;
            erase_.push_back(MakeSplat(sp.x, sp.y, sp.r * 2.2f, sp.r * 2.2f, 1.0f));
            int n = static_cast<int>((8 + Rand() * 14) * std::min(splash, 2.0f) * (0.6f + 0.6f * sp.glance) *
                                     (sp.fate == Fate::Burst ? 1.8f : 1.0f));
            for (int k = 0; k < n && sp.volume > Volume(sp.r) * 0.6f; ++k) {
                const Splash::Arm& a = sp.arms[static_cast<int>(Rand() * sp.armCount) % sp.armCount];
                float tx, ty;
                tip(a, Rand(0.9f, 1.7f), tx, ty);
                float side = Rand(-1.0f, 1.0f) * a.width * 1.5f;
                float br = sp.r * Rand(0.06f, 0.17f);
                AddMist(tx + std::cos(a.angle) * side, ty - std::sin(a.angle) * side, br);
                sp.volume -= Volume(br);
            }
        }
        // As it starts to pull back, the long lobes thrown downhill can tear
        // off at their thick rims and stay behind as small drops.
        if (!sp.torn && sp.fate == Fate::Pool && sp.age > sp.duration * 0.3f) {
            sp.torn = true;
            for (int k = 0; k < sp.armCount; ++k) {
                const Splash::Arm& a = sp.arms[k];
                if (a.length < sp.r * 2.0f || Rand() > 0.35f + 0.4f * sp.glance) continue;
                float br = std::min(a.width * Rand(0.5f, 0.8f), RadiusFor(sp.volume * 0.15f));
                if (sp.volume - Volume(br) < Volume(sp.r) * 0.5f) continue;
                float tx, ty;
                tip(a, 1.0f, tx, ty);
                Spawn(tx, ty, br);
                sp.volume -= Volume(br);
            }
        }
        if (sp.age < sp.duration) continue;
        if (sp.fate == Fate::Shoot) {
            // Most of the water carries on at speed from the end of the main
            // lobe and draws a trail as it goes; a little stays where it hit.
            float keep = sp.volume * Rand(0.12f, 0.3f);
            const Splash::Arm& a = sp.arms[0];
            float ax = std::sin(a.angle), ay = std::cos(a.angle);
            float rr = RadiusFor(sp.volume - keep);
            float speed = Rand(900.0f, 1500.0f) * s_ * (0.6f + 0.4f * sp.glance);
            float dist = std::max(a.length * 0.7f, (rr + RadiusFor(keep)) * 1.15f);
            Launch(sp.x + ax * dist, sp.y + ay * dist, rr, speed, a.angle);
            Spawn(sp.x, sp.y, RadiusFor(keep));
        } else if (sp.fate == Fate::Scatter) {
            // The rim breaks into a drop or two at the end of each lobe, the
            // long lobes getting the most water; some stays in the middle.
            float total = 0;
            for (int k = 0; k < sp.armCount; ++k) total += sp.arms[k].length * sp.arms[k].width;
            float shared = sp.volume * Rand(0.55f, 0.8f);
            for (int k = 0; k < sp.armCount; ++k) {
                const Splash::Arm& a = sp.arms[k];
                float share = shared * a.length * a.width / total;
                int n = a.length > sp.r * 2.2f && Rand() < 0.6f ? 2 : 1;
                for (int j = 0; j < n; ++j) {
                    float f = n == 1 ? Rand(0.8f, 1.0f) : (j == 0 ? Rand(0.5f, 0.65f) : Rand(0.9f, 1.05f));
                    float dist = a.length * f, side = Rand(-0.5f, 0.5f) * a.width;
                    Spawn(sp.x + std::sin(a.angle) * dist + std::cos(a.angle) * side,
                          sp.y + std::cos(a.angle) * dist - std::sin(a.angle) * side, RadiusFor(share / n));
                }
                sp.volume -= share;
            }
            Spawn(sp.x, sp.y, RadiusFor(sp.volume));
        } else if (sp.fate == Fate::Burst) {
            // The rim bursts and throws a drop off the end of each of the
            // biggest fingers, the long ones (the way the raindrop was going)
            // throwing theirs fastest. They fly off along the glass in every
            // direction, sideways and even up, and their own momentum carries
            // them until the glass slows them and their weight turns them
            // down. Some water stays in the middle.
            int order[8];
            float pick[8], longest = 0;
            for (int k = 0; k < sp.armCount; ++k) {
                order[k] = k;
                pick[k] = sp.arms[k].length * Rand(0.6f, 1.4f);
                longest = std::max(longest, sp.arms[k].length);
            }
            std::sort(order, order + sp.armCount, [&](int a, int b) { return pick[a] > pick[b]; });
            int n = std::min(sp.armCount, 3 + static_cast<int>(Rand() * 3) + (sp.r > 8.0f * s_ ? 1 : 0));
            float share[8], total = 0;
            for (int j = 0; j < n; ++j) {
                const Splash::Arm& a = sp.arms[order[j]];
                share[j] = a.length * a.width * Rand(0.3f, 1.7f);
                total += share[j];
            }
            float thrown = sp.volume * Rand(0.55f, 0.85f);
            for (int j = 0; j < n; ++j) {
                const Splash::Arm& a = sp.arms[order[j]];
                float v = thrown * share[j] / total;
                float dist = a.length * 0.9f;
                float speed = Rand(500.0f, 1300.0f) * s_ * (0.5f + 0.7f * a.length / longest) * (0.8f + 0.4f * sp.glance);
                Launch(sp.x + std::sin(a.angle) * dist, sp.y + std::cos(a.angle) * dist, RadiusFor(v), speed,
                       a.angle + Rand(-0.15f, 0.15f));
                sp.volume -= v;
            }
            Spawn(sp.x, sp.y, RadiusFor(sp.volume));
        } else {
            // Gathered back up: what is left is a drop at the top, where it hit.
            Spawn(sp.x + sp.dx * sp.r * 0.3f * sp.glance, sp.y + sp.dy * sp.r * 0.3f * sp.glance,
                  RadiusFor(sp.volume));
        }
        sp.volume = 0;
    }
    splashes_.erase(std::remove_if(splashes_.begin(), splashes_.end(), [](const Splash& sp) { return sp.volume <= 0; }),
                    splashes_.end());
}

// A drop thrown along the glass at speed, in any direction. It is carried by
// its own momentum until it either turns down and runs like any other (if it
// is heavy enough) or skids to a stop.
void RainSim::Launch(float x, float y, float r, float speed, float heading) {
    if (r < kMistMaxRadius * s_ * 2.0f || live_.size() >= kMaxDrops) {
        Spawn(x, y, r);
        return;
    }
    Drop d{x, y, r};
    d.running = true;
    d.flung = true;
    // Once it runs normally, it holds on again when it has shed a little.
    d.release = std::min(r, kPinRadius * s_ * friction);
    d.vy = speed;
    d.fvx = std::sin(heading) * speed;
    d.fvy = std::cos(heading) * speed;
    d.heading = d.targetHeading = d.tailHeading = WrapAngle(heading);
    d.trailDist = r * Rand(0.5f, 1.5f);
    NewDrop(d);
    ResetShape(d, false);
    live_.push_back(d);
}

// Pick a direction among a fan of downhill options. Wet film and
// water-loving patches pull the drop toward them, which is why runs meander
// and later drops reuse earlier paths.
void RainSim::Steer(Drop& d) {
    float best = -1e9f, bestAngle = d.targetHeading;
    float look = d.r + surfCell_ * 2.0f;
    for (float a : {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f}) {
        float px = d.x + std::sin(a) * look, py = d.y + std::cos(a) * look;
        float score = std::cos(a) * 1.2f + Wetness(px, py) * 2.0f + Affinity(px, py) * 0.8f -
                      std::fabs(a - d.heading) * 0.6f + Rand(0, 0.5f);
        if (score > best) {
            best = score;
            bestAngle = a;
        }
    }
    d.targetHeading = bestAngle;
}

void RainSim::Update(float dt) {
    time_ += dt;
    UpdateWind(dt);
    mist_.clear();
    hits_.clear();
    erase_.clear();
    BuildGrid();

    const float s = s_;
    const float pinR = kPinRadius * s * friction;
    const float baseSpeed = kBaseSpeed * s * runSpeed;

    // New rain. Mostly small drops, the odd big splat. Gusts bring bursts,
    // more so the windier it is.
    float gusting = std::min(0.4f + 0.4f * std::fabs(wind), 1.0f);
    float rush = std::clamp(1.0f + 0.5f * gusts * gusting * gustSlow_, 0.4f, 2.5f);
    dropAccum_ += dt * 10.0f * intensity * rush;
    const float sizeShape = std::clamp(2.5f / std::pow(std::max(intensity * rush, 0.05f), 0.35f), 1.2f, 6.0f);
    while (dropAccum_ >= 1.0f) {
        dropAccum_ -= 1.0f;
        float lo = std::min(minDrop, maxDrop), hi = std::max(minDrop, maxDrop);
        // Heavier rain has bigger drops (Marshall and Palmer: the mean size
        // grows about as R^0.21), so the sizes lean toward the large end in a
        // downpour and toward the small end in drizzle.
        float r = s * (lo + (hi - lo) * std::pow(Rand(), sizeShape));
        Impact(Rand(0, w_), Rand(0, h_), r);
    }
    // Fine mist that never runs: it slowly fogs the glass until drops sweep it away.
    mistAccum_ += dt * 250.0f * intensity * mist;
    while (mistAccum_ >= 1.0f) {
        mistAccum_ -= 1.0f;
        AddMist(Rand(0, w_), Rand(0, h_), s * Rand(0.8f, 3.0f));
    }
    // The pane goes on above the screen: now and then a drop gathered up
    // there runs down into view.
    runnerAccum_ += dt * 0.12f * intensity * (w_ / (2560.0f * s));
    while (runnerAccum_ >= 1.0f && live_.size() < kMaxDrops) {
        runnerAccum_ -= 1.0f;
        Drop d{Rand(0, w_), 0, pinR * Rand(0.95f, 1.5f)};
        d.y = -d.r * 2.0f;
        d.pin = 1.0f;
        d.running = true;
        d.release = pinR * Rand(0.85f, 1.1f);
        d.vy = baseSpeed;
        d.tail = std::clamp(d.vy / (600.0f * s), 0.0f, 1.0f);
        d.trailDist = d.r;
        NewDrop(d);
        ResetShape(d, false);
        live_.push_back(d);
    }

    struct Pending {
        float x, y, r;
    };
    std::vector<Pending> beads;
    const float growth = dt * 0.012f * s * intensity;

    for (int i = 0; i < static_cast<int>(live_.size()); ++i) {
        Drop& d = live_[i];
        if (d.dead) continue;

        if (!d.running) {
            // Resting drops slowly gather the mist around them.
            float aff = Affinity(d.x, d.y);
            float grown = d.r + growth * (0.5f + aff);
            // ...and slowly evaporate, which wins once the rain stops.
            grown = std::sqrt(std::max(grown * grown - dt * 0.015f * s * s, 0.0f));
            if (grown < kMistMaxRadius * s * 0.7f) {
                d.dead = true;
                continue;
            }
            // A drop that has just stopped pulls its tail back in.
            d.tail *= std::exp(-dt / kTailShrink);
            // Pinned contact lines only slowly let a lumpy drop pull itself round.
            float relax = std::exp(-dt / (60.0f * d.pin));
            for (int k = 0; k < d.lobeCount; ++k) {
                d.lobes[k].r *= grown / d.r;
                d.lobes[k].dx *= relax;
                d.lobes[k].dy *= relax;
            }
            d.r = grown;
            d.eraseTimer -= dt;
            if (d.eraseTimer <= 0) {
                d.eraseTimer = Rand(1.0f, 3.0f);
                erase_.push_back(MakeSplat(d.x, d.y, d.r * 1.15f, d.r * 1.15f, 1.0f));
            }
            // Pinning: water-loving spots grip harder, wet film barely grips at all.
            // Growing drops that touch a neighbour coalesce into one.
            ForNeighbors(d.x, d.y, d.r * 2.0f + 20.0f * s, [&](int j) {
                if (j == i || d.dead) return;
                Drop& o = live_[j];
                if (o.dead || o.running) return;
                float ox = o.x - d.x, oy = o.y - d.y;
                float reach = (d.r + o.r) * 0.95f;
                if (ox * ox + oy * oy >= reach * reach) return;
                if (o.r > d.r)
                    Merge(o, d);
                else
                    Merge(d, o);
            });
            if (d.dead) continue;

            float release = pinR * d.pin * (0.85f + 0.3f * aff) * (1.0f - 0.3f * Wetness(d.x, d.y));
            // A knock or the pane shaking loosens the grip for a moment.
            d.jolt *= std::exp(-dt / 0.15f);
            release *= 1.0f - std::min(d.jolt + 0.25f * shake, 0.4f);
            // Wind adds a sideways pull to the weight the contact line must
            // hold, so heavy drops let go in a gust.
            float tilt = Tilt(d);
            if (d.r * std::pow(1.0f + tilt * tilt, 0.25f) > release) {
                d.running = true;
                d.release = release;
                d.vy = 0;
                d.heading = d.targetHeading = Rand(-0.15f, 0.15f) + std::clamp(std::atan(tilt), -1.2f, 1.2f) * 0.7f;
                d.tailHeading = d.heading;
                d.steerDist = 0;
                d.trailDist = d.r * Rand(0.5f, 1.5f);
                // It goes from its lowest point. That blob becomes the head;
                // the rest of its outline drains into it as it gets going.
                int front = 0;
                float best = -1e9f;
                for (int k = 0; k < d.lobeCount; ++k) {
                    float ahead = d.lobes[k].dx * std::sin(d.heading) + d.lobes[k].dy * std::cos(d.heading);
                    if (ahead > best) best = ahead, front = k;
                }
                std::swap(d.lobes[0], d.lobes[front]);
            }
            continue;
        }

        float wetHere = Wetness(d.x, d.y);
        float reachAhead = d.r + surfCell_;
        float wetAhead = Wetness(d.x + std::sin(d.heading) * reachAhead, d.y + std::cos(d.heading) * reachAhead);
        // The wind leans running drops off the vertical.
        float tilt = Tilt(d);
        float push = std::sqrt(1.0f + tilt * tilt);
        float slideR = d.release * 0.8f;
        float step;

        if (d.flung) {
            // Thrown by a splash, it goes where its momentum takes it. Its
            // weight pulls it down (and the wind sideways), the contact line
            // holds back against the way it is going with a steady force, and
            // viscous drag grows with speed (the same drag that settles
            // running drops at their speed). A drop heavy enough to slide
            // curves round and runs on down; a small one skids to a stop.
            constexpr float kDrag = 8.0f;  // per second
            float grip = kDrag * baseSpeed * (1.0f - 0.5f * wetAhead);
            float heavy = d.r / (pinR * d.pin * 0.8f);
            float weight = kDrag * baseSpeed * heavy * heavy;
            d.fvx += weight * std::clamp(tilt, -3.0f, 3.0f) * dt;
            d.fvy += weight * dt;
            float speed = std::sqrt(d.fvx * d.fvx + d.fvy * d.fvy);
            // The grip takes away speed, never more than there is.
            float keep = std::max(speed - grip * dt, 0.0f) / std::max(speed, 1e-6f) * std::exp(-kDrag * dt);
            d.fvx *= keep;
            d.fvy *= keep;
            speed *= keep;
            if (speed > 1e-3f) d.heading = std::atan2(d.fvx, d.fvy);
            d.vy = speed;
            step = speed * dt;
            d.x += d.fvx * dt;
            d.y += d.fvy * dt;
            float downhill = std::atan(tilt);
            if (weight < grip && speed < 15.0f * s) {
                // Momentum spent and too light to slide: it stays where it is.
                d.flung = false;
                d.vy = 0;
                d.heading = d.targetHeading = std::clamp(d.heading, -1.35f, 1.35f);
            } else if (weight >= grip && std::fabs(WrapAngle(d.heading - downhill)) < 0.3f) {
                // Turned down the glass: from here it runs like any other drop.
                d.flung = false;
                d.heading = d.targetHeading = std::clamp(d.heading, -1.35f, 1.35f);
                d.steerDist = 0;
            }
        } else {
            if (d.steerDist <= 0) {
                Steer(d);
                d.steerDist = std::max(surfCell_ * 1.5f, d.r);
            }
            // Fast drops have too much momentum to wander much.
            float wander = 1.0f / (1.0f + d.vy / (400.0f * s));
            float lean = std::clamp(std::atan(tilt), -1.2f, 1.2f);
            d.heading += (d.targetHeading * wander + lean - d.heading) * std::min(1.0f, dt * 5.0f);
            d.heading = std::clamp(d.heading, -1.35f, 1.35f);  // always somewhat downhill

            // Speed rises steeply with size: a drop that only just broke loose
            // creeps, one that swallowed its neighbours is gone in a blink. A wet
            // path has almost no grip left, so drops run faster along it.
            float drive = std::max((d.r * d.r) / (slideR * slideR) * push - 1.0f, 0.05f);
            float target = std::min(baseSpeed * drive * (1.0f + 0.8f * wetAhead), kMaxSpeed * s * runSpeed);
            if (d.stickTimer > 0) {
                d.stickTimer -= dt;
                d.vy *= std::exp(-dt * 12.0f);
            } else {
                // Dry glass: stick-slip. Wet glass: smooth.
                if (Rand() < dt * 1.2f * friction * (1.0f - wetAhead)) d.stickTimer = Rand(0.05f, 0.35f);
                // Viscous drag settles a drop at its sliding speed within a fraction of a second.
                float rate = wetAhead > 0.5f ? 14.0f : 8.0f;
                d.vy += (target - d.vy) * std::min(1.0f, dt * rate);
            }
            step = d.vy * dt;
            d.x += std::sin(d.heading) * step;
            d.y += std::cos(d.heading) * step;
        }
        d.steerDist -= step;
        Wet(d.x, d.y, d.r * 0.8f);

        // The tail follows the speed, drawing out quickly and pulling back in
        // more slowly (it is film that has to drain into the head), and it
        // trails along the path behind, so a drop that turns has a curved tail.
        float tailTarget = std::clamp(d.vy / (600.0f * s), 0.0f, 1.0f);
        d.tail += (tailTarget - d.tail) * std::min(1.0f, dt / (tailTarget > d.tail ? kTailGrow : kTailShrink));
        d.tailHeading += WrapAngle(d.heading - d.tailHeading) * std::min(1.0f, step / (1.5f * d.r));
        d.tailHeading = WrapAngle(d.tailHeading);
        // Its outline shifts as it goes: the head wobbles from side to side.
        d.seed += step / (4.0f * d.r);
        // Lumps left from when it let go, and drops it has just swallowed,
        // drain into the head.
        float drain = std::exp(-dt / kDrain);
        d.lobes[0].dx *= drain;
        d.lobes[0].dy *= drain;
        d.lobes[0].r = d.r + (d.lobes[0].r - d.r) * drain;
        int lumps = 1;
        for (int k = 1; k < d.lobeCount; ++k) {
            Drop::Lobe l = d.lobes[k];
            l.dx *= drain;
            l.dy *= drain;
            l.r *= drain;
            if (l.r > 0.12f * d.r) d.lobes[lumps++] = l;
        }
        d.lobeCount = lumps;

        // Fast enough over dry glass, the drop's tail stretches into a rivulet,
        // wider the faster it goes. It keeps drawing one until it slows well
        // below the speed that started it. On a wet path the drop rides
        // earlier water instead and leaves none.
        float trailAt = kTrailSpeed * s * trailSpeed / std::sqrt(friction) * (1.2f - 0.4f * Affinity(d.x, d.y));
        bool trailing = d.vy > (d.rivulet >= 0 ? 0.8f : 1.0f) * trailAt;
        if (trailing && wetHere < 0.5f && step > 0)
            LayRivulet(d, step, std::clamp(d.vy / trailAt - 1.0f, 0.0f, 1.0f));
        else
            d.rivulet = -1;

        // On dry glass the trailing film breaks into beads. On a wet path the
        // drop mostly picks earlier beads back up, so it sheds less.
        d.trailDist -= step;
        if (d.trailDist <= 0) {
            // Fast drops outrun the film's breakup, so they leave beads further apart.
            d.trailDist = d.r * Rand(0.3f, 1.0f) * (1.0f + 2.5f * wetHere) * (d.rivulet >= 0 ? 2.5f : 1.0f) *
                          (1.0f + d.vy / (300.0f * s));
            float tr = d.r * Rand(0.08f, 0.22f) * (1.0f - 0.5f * wetHere);
            // Left at the rear contact line, behind the tail, so beads never sit inside it.
            float back = d.r * (1.75f + 1.8f * d.tail) + tr;
            float side = Rand(-0.25f, 0.25f) * d.r;
            float bx = std::sin(d.tailHeading), by = std::cos(d.tailHeading);
            beads.push_back({d.x - bx * back + by * side, d.y - by * back - bx * side, tr});
            tr = std::min(tr, d.r * 0.5f);
            d.r = RadiusFor(Volume(d.r) - Volume(tr));  // the bead's water comes out of the drop
        }

        // Swallow whatever is in the way, including old rivulets it crosses.
        if (float soaked = EraseRivulets(d.x, d.y, d.r * 0.9f, d.rivulet))
            d.r = std::min(RadiusFor(Volume(d.r) + soaked), kMaxRadius * kPinRadius * s_ * std::max(friction, 1.0f));
        ForNeighbors(d.x, d.y, d.r * 2.0f + 20.0f * s, [&](int j) {
            if (j == i) return;
            Drop& o = live_[j];
            if (o.dead) return;  // beads it just left are spawned out of reach
            float ox = o.x - d.x, oy = o.y - d.y;
            float reach = (d.r + o.r) * 0.95f;
            if (ox * ox + oy * oy < reach * reach) Merge(d, o);
        });

        {
            float k = d.tail, bx = std::sin(d.tailHeading), by = std::cos(d.tailHeading);
            float sx = 1.15f + 0.8f * k * bx * bx, sy = 1.15f + 0.8f * k * by * by;
            erase_.push_back(MakeSplat(d.x - bx * d.r * 0.4f * k, d.y - by * d.r * 0.4f * k, d.r * sx, d.r * sy, 1.0f));
        }

        // Pins again once it has shed enough, unless it is riding a wet path.
        // Receding grip is weaker than advancing grip, so a drop keeps going
        // until it has shed down to about 80% of the size it let go at. A
        // flung drop stops when its momentum is spent (above).
        if (!d.flung && (d.vy == 0 || (d.r * std::sqrt(push) < slideR && wetAhead < 0.3f))) {
            d.running = false;
            d.vy = 0;
            d.eraseTimer = 0;
            d.stickTimer = 0;
            d.rivulet = -1;
            // Its outline is caught where the contact line stopped: lumps
            // drained or not, it settles a little irregular, and its tail
            // pulls back in over a moment (see the resting drops above).
            float area = 0;
            for (int k = 0; k < d.lobeCount; ++k) area += d.lobes[k].r * d.lobes[k].r;
            float scale = d.r / std::sqrt(std::max(area, 1e-6f));
            for (int k = 0; k < d.lobeCount; ++k) d.lobes[k].r *= scale;
            d.rough = std::max(d.rough, Rand(0.2f, 0.4f));
        }
        if (d.y - d.r * 3.0f > h_ || d.y < -d.r * 8.0f || d.x < -d.r * 3.0f || d.x > w_ + d.r * 3.0f) d.dead = true;
    }

    // Spawn before compacting: Spawn looks up neighbours through this frame's grid indices.
    for (const Pending& p : beads) Spawn(p.x, p.y, p.r);
    UpdateWiper(dt);
    UpdateFinger(dt);
    UpdateRivulets(dt);
    UpdateSplashes(dt);
    live_.erase(std::remove_if(live_.begin(), live_.end(), [](const Drop& d) { return d.dead; }), live_.end());

    drops_.clear();
    drops_.reserve(live_.size() * 3);
    for (const Rivulet& rv : rivulets_)
        for (const RivuletNode& n : rv.nodes)
            if (n.alive) {
                drops_.push_back(MakeSplat(n.x, n.y, n.w * 1.1f, n.w * 1.1f, n.w * kHeightPerRadius));
                // The bead a broken piece is gathering into, drawn like a resting drop.
                if (n.pool > 0) {
                    float rp = RadiusFor(n.pool);
                    drops_.push_back(MakeSplat(n.x, n.y, rp * kSpriteScale, rp * kSpriteScale, rp * kHeightPerRadius));
                }
            }
    for (const Splash& sp : splashes_) {
        // Spread out, the water is a thin sheet; pulled back, it is a drop again.
        float reach = SplashReach(sp);
        float core = sp.r * (1.0f - 0.25f * reach);
        drops_.push_back(MakeSplat(sp.x, sp.y, core * kSpriteScale, core * kSpriteScale,
                                   core * kHeightPerRadius * (1.0f - 0.6f * reach)));
        for (int k = 0; k < sp.armCount; ++k) {
            const Splash::Arm& a = sp.arms[k];
            float ax = std::sin(a.angle), ay = std::cos(a.angle);
            for (float f : {0.25f, 0.45f, 0.65f, 0.85f, 1.0f}) {
                float dist = a.length * reach * f;
                // The rim gathers water, so each lobe ends in a thicker bead.
                float wr = a.width * (f < 1.0f ? 1.0f - 0.25f * f : 0.95f) * (0.6f + 0.4f * reach);
                drops_.push_back(MakeSplat(sp.x + ax * dist, sp.y + ay * dist, wr * kSpriteScale, wr * kSpriteScale,
                                           wr * kHeightPerRadius * 0.6f));
            }
        }
    }
    for (const Drop& d : live_) DrawDrop(d);
    // Water pushed along by a finger bulges in a crescent at its front.
    if (finger_.down && finger_.carried > 0) {
        const Finger& f = finger_;
        float rc = RadiusFor(f.carried), half = kFingerHalf * s_;
        float fx = f.x + f.dirX * half * 0.8f, fy = f.y + f.dirY * half * 0.8f;
        drops_.push_back(MakeSplat(fx, fy, rc * 0.8f * kSpriteScale, rc * 0.8f * kSpriteScale, rc * 0.8f * kHeightPerRadius));
        for (float side : {-1.0f, 1.0f}) {
            float sx = fx - f.dirY * side * half * 0.6f - f.dirX * half * 0.3f;
            float sy = fy + f.dirX * side * half * 0.6f - f.dirY * half * 0.3f;
            drops_.push_back(MakeSplat(sx, sy, rc * 0.55f * kSpriteScale, rc * 0.55f * kSpriteScale,
                                       rc * 0.55f * kHeightPerRadius));
        }
    }
}

// A drop is drawn as its blobs, the first stretched along its path the
// faster it goes, plus a tail that fades in and out with its length so it
// never pops. The same drawing serves resting and running drops, so nothing
// jumps when a drop starts or stops.
void RainSim::DrawDrop(const Drop& d) {
    // Heavy drops sag: gravity pulls them longer and narrower as they near
    // release. A running drop is drawn out along its path instead.
    float e = d.tail;
    float sag = 1.0f + 0.3f * std::clamp(d.r / (kPinRadius * s_ * friction) - 0.4f, 0.0f, 1.0f) * (1.0f - 0.6f * e);
    float rootSag = std::sqrt(sag);
    // Wind leans the drop over a little downwind.
    float lean = std::clamp(Tilt(d), -1.5f, 1.5f) * 0.12f * d.r;
    float hx = std::sin(d.heading), hy = std::cos(d.heading);
    // Sprites are upright ellipses: stretch the head along the path and
    // narrow it across, whichever way the path goes.
    float along = 0.15f * e, across = 0.12f * e;
    float headX = 1.0f + along * hx * hx - across * hy * hy, headY = 1.0f + along * hy * hy - across * hx * hx;
    float rough = d.rough * (d.running ? 0.6f : 1.0f);
    for (int k = 0; k < d.lobeCount; ++k) {
        const Drop::Lobe& l = d.lobes[k];
        float sx = k == 0 ? headX : 1.0f, sy = k == 0 ? headY : 1.0f;
        drops_.push_back(MakeSplat(d.x + l.dx + lean * (l.r / d.r), d.y + l.dy * sag,
                                   l.r * kSpriteScale * sx / rootSag, l.r * kSpriteScale * sy * sag,
                                   l.r * kHeightPerRadius, 1.0f, rough, d.seed + 2.1f * k));
    }
    if (e < 0.002f) return;

    // A running drop is a teardrop: a fat head with a tapering tail of film
    // dragged out behind it, longer the faster it goes, following the path.
    // The tail sprites are close enough to the head to fuse into one outline.
    const Drop::Lobe& head = d.lobes[0];
    float r = head.r, height = r * kHeightPerRadius;
    float cx = d.x + head.dx + lean * (r / d.r), cy = d.y + head.dy * sag;
    float fade = Smoothstep(0.0f, 0.25f, e);
    float mid = d.heading + 0.5f * WrapAngle(d.tailHeading - d.heading);
    struct Piece {
        float angle, dist, wide, longer, height;
    } pieces[2] = {{mid, r * (0.45f + 0.4f * e), 0.8f, 0.95f, 0.7f},
                   {d.tailHeading, r * (0.9f + 0.9f * e), 0.5f, 0.85f, 0.4f}};
    for (const Piece& p : pieces) {
        float bx = std::sin(p.angle), by = std::cos(p.angle);
        float rx = r * kSpriteScale * (p.wide * by * by + p.longer * bx * bx);
        float ry = r * kSpriteScale * (p.longer * by * by + p.wide * bx * bx);
        drops_.push_back(MakeSplat(cx - bx * p.dist, cy - by * p.dist, rx, ry, height * p.height, fade));
    }
}

float RainSim::WaterOnGlass() const {
    float total = 0;
    for (const Drop& d : live_)
        if (!d.dead) total += Volume(d.r);
    for (const Splash& sp : splashes_) total += sp.volume;
    total += finger_.carried;
    for (const Rivulet& rv : rivulets_) {
        total += rv.carry;
        for (const RivuletNode& n : rv.nodes)
            if (n.alive) total += n.water + n.pool;
    }
    return total;
}

// ---------------------------------------------------------------- rivulets

// A drop running fast over dry glass drags a thin rivulet out of its tail.
// The rivulet costs the drop water as it goes.
void RainSim::LayRivulet(Drop& d, float step, float excess) {
    if (d.rivulet < 0) {
        Rivulet rv;
        rv.wavelength = Rand(8.0f, 13.0f);
        rv.phase = Rand(0, 6.2832f);
        rv.wobble = Rand(0, 6.2832f);
        rv.minX = rv.maxX = d.x;
        rv.minY = rv.maxY = d.y;
        rivulets_.push_back(std::move(rv));
        d.rivulet = static_cast<int>(rivulets_.size()) - 1;
    }
    Rivulet& rv = rivulets_[d.rivulet];
    float back = d.r * (1.0f + 1.2f * d.tail);
    float tx = d.x - std::sin(d.tailHeading) * back, ty = d.y - std::cos(d.tailHeading) * back;
    float w0 = std::max(d.r * (0.12f + 0.14f * excess), 1.6f * s_);  // never thinner than a hairline
    float spacing = std::max(w0 * 0.5f, 0.8f * s_);

    // Water moves from the drop into the rivulet: the drop loses exactly the
    // volume the new nodes hold.
    // A rivulet is a low ridge of water about a third as high as it is
    // wide: a cross-section of about 0.4 w^2, which in drop-volume terms
    // (a cap holds about 0.63 r^3) is about 0.6 w^2 per pixel of length.
    float lost = std::min(0.6f * w0 * w0 * step, Volume(d.r) * 0.5f);
    d.r = RadiusFor(Volume(d.r) - lost);
    rv.carry += lost;

    auto add = [&](float x, float y, float along) {
        RivuletNode n;
        n.water = 0;
        // Width wanders a little, and the line meanders slightly sideways
        // as the contact line catches on the glass.
        n.w0 = w0 * (1.0f + 0.18f * std::sin(along * 0.11f / s_ + rv.phase * 3.0f));
        n.x = x + std::sin(along * 0.045f / s_ + rv.wobble) * w0 * 0.7f;
        n.y = y;
        n.born = time_;
        n.along = along;
        n.w = n.w0;
        rv.nodes.push_back(n);
        rv.minX = std::min(rv.minX, n.x - w0 * 3);
        rv.maxX = std::max(rv.maxX, n.x + w0 * 3);
        rv.minY = std::min(rv.minY, n.y - w0 * 3);
        rv.maxY = std::max(rv.maxY, n.y + w0 * 3);
    };
    if (rv.nodes.empty()) {
        add(tx, ty, 0);
    } else {
        // Fill in from the last node up to the tail, starting from the last
        // node's unjittered position so the meander doesn't accumulate.
        const RivuletNode& last = rv.nodes.back();
        float lx = last.x - std::sin(last.along * 0.045f / s_ + rv.wobble) * w0 * 0.7f, ly = last.y;
        float dx = tx - lx, dy = ty - ly;
        float dist = std::sqrt(dx * dx + dy * dy);
        int n = static_cast<int>(dist / spacing);
        float along = last.along;
        size_t first = rv.nodes.size();
        for (int i = 1; i <= n; ++i) {
            float t = i * spacing / dist;
            add(lx + dx * t, ly + dy * t, along + i * spacing);
        }
        if (n > 0) {
            for (size_t i = first; i < rv.nodes.size(); ++i) rv.nodes[i].water = rv.nodes[i].water0 = rv.carry / n;
            rv.carry = 0;
        }
    }
}

// Old drops and the wiper soak up any rivulet they pass over.
float RainSim::EraseRivulets(float x, float y, float radius, int skip) {
    float soaked = 0;
    for (int i = 0; i < static_cast<int>(rivulets_.size()); ++i) {
        Rivulet& rv = rivulets_[i];
        if (i == skip || x + radius < rv.minX || x - radius > rv.maxX || y + radius < rv.minY ||
            y - radius > rv.maxY)
            continue;
        for (RivuletNode& n : rv.nodes) {
            float dx = n.x - x, dy = n.y - y;
            if (n.alive && dx * dx + dy * dy < radius * radius) {
                n.alive = false;
                soaked += n.water + n.pool;
                n.water = n.pool = 0;
            }
        }
    }
    return soaked;
}

// Starts nodes [a, b) pulling themselves up into one droplet at their
// middle. The water is all still in the nodes; Gather moves it.
void RainSim::StartBead(Rivulet& rv, int a, int b) {
    if (b <= a) return;
    int c = (a + b) / 2;
    for (int m = a; m < b; ++m) rv.nodes[m].gather = c;
    rv.nodes[c].pool = 0;
    rv.nodes[c].retract = 0;
}

// A pinched-off piece of rivulet doesn't turn into a drop all at once: its
// thin ends retract toward the middle, which swells into a bead as it takes
// their water, until only the bead is left (capillary retraction, a few
// centimetres a second for a film this thin). Water is handed over node by
// node, so none is made or lost.
void RainSim::Gather(Rivulet& rv, float dt) {
    std::vector<RivuletNode>& nodes = rv.nodes;
    const int count = static_cast<int>(nodes.size());
    // Pieces whose middle was swallowed by a passing drop go back to being ordinary rivulet.
    for (RivuletNode& n : nodes)
        if (n.gather >= 0 && !nodes[n.gather].alive) n.gather = -1;

    const float speed = 70.0f * s_ * std::max(beadSpeed, 0.05f);
    for (int c = 0; c < count; ++c) {
        RivuletNode& mid = nodes[c];
        if (!mid.alive || mid.gather != c) continue;
        int lo = c, hi = c;
        while (lo > 0 && nodes[lo - 1].alive && nodes[lo - 1].gather == c) --lo;
        while (hi + 1 < count && nodes[hi + 1].alive && nodes[hi + 1].gather == c) ++hi;
        // Retraction speeds up as the bead grows and pulls harder.
        mid.retract += speed * dt * (1.0f + 0.5f * std::min(RadiusFor(mid.pool) / mid.w0, 2.0f));
        while (lo < hi) {
            // Take the end that is further out.
            bool fromLow = (mid.along - nodes[lo].along) >= (nodes[hi].along - mid.along);
            int e = fromLow ? lo : hi;
            float gap = std::fabs(nodes[e].along - nodes[fromLow ? lo + 1 : hi - 1].along);
            if (mid.retract < gap) break;
            mid.retract -= gap;
            // The end's water runs inward: part into the bead, the rest
            // swelling the rim of the end that is left.
            int next = fromLow ? lo + 1 : hi - 1;
            float w = nodes[e].water;
            if (next == c) {
                mid.pool += w;
            } else {
                mid.pool += w * 0.6f;
                nodes[next].water += w * 0.4f;
            }
            nodes[e].water = 0;
            nodes[e].alive = false;
            if (fromLow) ++lo; else --hi;
        }
        // Thinning ends: the closer to the bead, the more of the line has
        // already been drawn into it.
        for (int m = lo; m <= hi; ++m) {
            if (m == c) continue;
            float full = std::sqrt(std::max(nodes[m].water, 0.0f) / std::max(nodes[m].water0, 1e-6f));
            nodes[m].w = nodes[m].w0 * std::clamp(full, 0.3f, 1.8f) * 0.9f;
        }
        mid.w = mid.w0 * 0.9f;
        if (lo == hi) {
            // All gathered: hand it over as an ordinary resting drop.
            float water = mid.pool + mid.water;
            mid.pool = mid.water = 0;
            mid.alive = false;
            if (water > 0) Spawn(mid.x, mid.y, RadiusFor(water));
        }
    }
}

// A rivulet left on the glass is unstable. Its water drains down the line,
// so the oldest part (the top) thins first and the water gathers lower down.
// Surface tension pinches it into segments about twenty widths long
// (Plateau-Rayleigh), the necks thin until they snap, and each segment pulls
// itself up into a droplet. Every drop of water ends up in those droplets:
// none of it is lost on the way.
void RainSim::UpdateRivulets(float dt) {
    const float life = std::max(streakLife, 0.1f);
    auto smooth = [](float a, float b, float x) {
        float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };

    for (Rivulet& rv : rivulets_) rv.open = false;
    for (Drop& d : live_) {
        if (d.rivulet < 0) continue;
        if (d.dead || !d.running)
            d.rivulet = -1;
        else
            rivulets_[d.rivulet].open = true;
    }

    for (Rivulet& rv : rivulets_) {
        std::vector<RivuletNode>& nodes = rv.nodes;
        const int count = static_cast<int>(nodes.size());

        // Drain: older water flows down into the next node. Bottom up, so
        // water moves at most one node per frame.
        for (int m = count - 2; m >= 0; --m) {
            RivuletNode& a = nodes[m];
            RivuletNode& b = nodes[m + 1];
            if (!a.alive || !b.alive || a.gather >= 0 || b.gather >= 0 || time_ - a.born < 2.5f * life) continue;
            float flow = a.water * std::min(1.0f, dt / (2.0f * life));
            a.water -= flow;
            b.water += flow;
        }

        // Shape: the pinching wave, scaled by how much water is left here.
        // A neck that gets too thin snaps and hands its water to whichever
        // neighbour it is still joined to, downhill first.
        for (int m = 0; m < count; ++m) {
            RivuletNode& n = nodes[m];
            if (!n.alive || n.gather >= 0) continue;
            float age = time_ - n.born;
            float pinch = smooth(1.0f * life, 4.0f * life, age);
            float lambda = rv.wavelength * n.w0 * 2.0f;
            float wave = std::sin(6.2832f * n.along / lambda + rv.phase) +
                         0.3f * std::sin(6.2832f * n.along / (lambda * 0.43f) + rv.phase * 2.0f);
            float full = std::sqrt(std::max(n.water, 0.0f) / std::max(n.water0, 1e-6f));
            n.w = n.w0 * std::max(1.0f + 0.8f * pinch * wave, 0.0f) * std::clamp(full, 0.3f, 1.8f);
            if (n.w >= 0.35f * n.w0 && full > 0.25f) continue;
            n.alive = false;
            if (m + 1 < count && nodes[m + 1].alive)
                nodes[m + 1].water += n.water;
            else if (m > 0 && nodes[m - 1].alive)
                nodes[m - 1].water += n.water;
            else if (n.water > 0)
                AddMist(n.x, n.y, RadiusFor(n.water));  // a lone speck of water
            n.water = 0;
        }

        // Pieces pull up into droplets, each at its own moment: a pinched-off
        // segment once it has had time to bead, and anything still joined up
        // once the rivulet is old, one droplet per wavelength of its length.
        for (int i = 0; i < count;) {
            if (!nodes[i].alive) {
                ++i;
                continue;
            }
            int j = i;
            float youngest = -1e9f;
            while (j < count && nodes[j].alive && nodes[j].gather == nodes[i].gather)
                youngest = std::max(youngest, nodes[j++].born);
            if (nodes[i].gather >= 0) {
                i = j;
                continue;
            }
            float age = time_ - youngest;
            bool pinchedOff = i > 0 && j < count;
            float span = nodes[j - 1].along - nodes[i].along;
            float lambda = rv.wavelength * nodes[i].w0 * 2.0f;
            bool bead = pinchedOff && age > 3.0f * life && span < 2.0f * lambda;
            bool old = age > 7.0f * life && (j < count || !rv.open);
            if ((bead || old) && Rand() < dt * 2.5f / life) {
                // One droplet per wavelength, but never so many that each is a mere speck.
                float water = 0;
                for (int m = i; m < j; ++m) water += nodes[m].water;
                float speck = kMistMaxRadius * s_;
                int pieces = std::max(1, std::min(static_cast<int>(std::lround(span / lambda)),
                                                  static_cast<int>(water / (Volume(speck) * 2.0f))));
                for (int p = 0; p < pieces; ++p) {
                    int a = i + (j - i) * p / pieces, b = i + (j - i) * (p + 1) / pieces;
                    StartBead(rv, a, b);
                }
            }
            i = j;
        }
        Gather(rv, dt);
    }

    // Drop rivulets that are gone, keeping drops pointed at the right ones.
    std::vector<int> remap(rivulets_.size(), -1);
    size_t kept = 0;
    for (size_t i = 0; i < rivulets_.size(); ++i) {
        Rivulet& rv = rivulets_[i];
        bool any = rv.open || std::any_of(rv.nodes.begin(), rv.nodes.end(), [](const RivuletNode& n) {
                       return n.alive;
                   });
        if (!any) continue;
        remap[i] = static_cast<int>(kept);
        if (kept != i) rivulets_[kept] = std::move(rv);
        ++kept;
    }
    rivulets_.resize(kept);
    for (Drop& d : live_)
        if (d.rivulet >= 0) d.rivulet = remap[d.rivulet];
}

// ---------------------------------------------------------------- wiper

void RainSim::StartWipe() {
    if (wiper_.active) return;
    wiper_.active = true;
    wiper_.pivotX = w_ * 0.5f;
    wiper_.pivotY = h_ + h_ * 0.08f;
    // Long enough to reach the top corners from the pivot.
    float reach = std::sqrt(w_ * w_ * 0.25f + wiper_.pivotY * wiper_.pivotY);
    wiper_.outer = reach * 1.02f;
    wiper_.inner = h_ * 0.12f;
    wipeTime_ = 0;
    wiper_.angle = wipePrevAngle_ = -1.5708f;
}

// One stroke left to right and one back. The blade carries whatever it
// meets: drops in its path slide along just ahead of it, bump into each other
// and fuse, and at the end of the stroke they are pushed past the bottom edge.
void RainSim::UpdateWiper(float dt) {
    if (!wiper_.active) return;
    constexpr float kStroke = 1.1f;  // seconds per stroke
    constexpr float kHalfPi = 1.5708f;
    wipeTime_ += dt;
    float t = std::min(wipeTime_ / (2 * kStroke), 1.0f);
    float phase = t < 0.5f ? t * 2 : 2 - t * 2;  // 0 -> 1 -> 0
    float ease = phase * phase * (3 - 2 * phase);
    float prev = wipePrevAngle_;
    float angle = -kHalfPi - 0.08f + (2 * kHalfPi + 0.16f) * ease;  // overshoot past horizontal
    float dir = angle >= prev ? 1.0f : -1.0f;
    wiper_.angle = angle;
    wipePrevAngle_ = angle;

    const float px = wiper_.pivotX, py = wiper_.pivotY;
    auto polar = [&](float x, float y, float& a, float& rad) {
        float dx = x - px, dy = py - y;
        rad = std::sqrt(dx * dx + dy * dy);
        a = std::atan2(dx, dy);
    };
    auto place = [&](float a, float rad, float& x, float& y) {
        x = px + std::sin(a) * rad;
        y = py - std::cos(a) * rad;
    };

    // Push drops the blade has reached ahead of it.
    float lo = std::min(prev, angle), hi = std::max(prev, angle);
    for (Drop& d : live_) {
        if (d.dead) continue;
        float a, rad;
        polar(d.x, d.y, a, rad);
        if (rad < wiper_.inner - d.r || rad > wiper_.outer + d.r) continue;
        float halfWidth = d.r / std::max(rad, 1.0f);
        if (a + halfWidth < lo || a - halfWidth > hi) continue;
        place(angle + dir * halfWidth * 1.1f, rad, d.x, d.y);
        d.running = false;
        d.flung = false;
        d.vy = 0;
        d.stickTimer = 0;
        if (d.lobeCount == 0) ResetShape(d, false);
        if (d.y - d.r > h_ || d.x < -d.r || d.x > w_ + d.r) d.dead = true;
    }

    // Wipe the mist and rivulets from the swept wedge, and leave the
    // glass wet so the next drops run easily.
    float spacing = 14.0f * s_;
    for (float rad = wiper_.inner; rad <= wiper_.outer; rad += spacing) {
        float arc = rad * (hi - lo);
        int n = std::max(1, static_cast<int>(std::ceil(arc / spacing)));
        for (int k = 0; k <= n; ++k) {
            float a = lo + (hi - lo) * k / n, x, y;
            place(a, rad, x, y);
            if (x < -spacing || x > w_ + spacing || y < -spacing || y > h_ + spacing) continue;
            erase_.push_back(MakeSplat(x, y, spacing * 1.2f, spacing * 1.2f, 1.0f));
            EraseRivulets(x, y, spacing * 1.2f);
            Wet(x, y, spacing);
        }
    }
    // A real blade is never perfect: it leaves a few fine droplets behind.
    for (int k = 0; k < 6; ++k) {
        float x, y;
        place(angle - dir * 0.02f, Rand(wiper_.inner, wiper_.outer), x, y);
        if (x >= 0 && x < w_ && y >= 0 && y < h_) AddMist(x, y, s_ * Rand(0.6f, 1.6f));
    }

    if (t >= 1.0f) wiper_.active = false;
}

// ---------------------------------------------------------------- finger

void RainSim::SetFinger(bool down, float x, float y) {
    finger_.down = down;
    finger_.x = x;
    finger_.y = y;
}

// A fingertip dragged over wet glass clears a path about as wide as itself.
// The water it meets is pushed along in a bulge at its front; what it can't
// hold spills off its sides as drops along the edges of the path. When it
// stops or lifts, the bulge stays behind as a drop, usually heavy enough to
// run. The swept glass is left wet, so later drops run down the finger's
// path, and the film it leaves beads into a few fine droplets.
void RainSim::UpdateFinger(float dt) {
    Finger& f = finger_;
    const float half = kFingerHalf * s_;
    auto setDown = [&] {
        if (f.carried > 0) Spawn(f.x + f.dirX * half * 0.8f, f.y + f.dirY * half * 0.8f, RadiusFor(f.carried));
        f.carried = 0;
    };
    if (!f.down) {
        if (f.wasDown) setDown();
        f.wasDown = false;
        return;
    }
    if (!f.wasDown) {
        f.lastX = f.x;
        f.lastY = f.y;
        f.wasDown = true;
        f.still = 0;
    }
    float dx = f.x - f.lastX, dy = f.y - f.lastY, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5f * s_) {
        f.still += dt;
        if (f.still > 0.3f) setDown();
        return;
    }
    f.still = 0;
    f.dirX = dx / len;
    f.dirY = dy / len;

    // Everything in the swept capsule is gathered up.
    float mx = (f.x + f.lastX) * 0.5f, my = (f.y + f.lastY) * 0.5f;
    ForNeighbors(mx, my, len * 0.5f + half + 24.0f * s_, [&](int j) {
        Drop& o = live_[j];
        if (o.dead) return;
        float t = std::clamp(((o.x - f.lastX) * dx + (o.y - f.lastY) * dy) / (len * len), 0.0f, 1.0f);
        float px = f.lastX + dx * t - o.x, py = f.lastY + dy * t - o.y, reach = half + o.r * 0.8f;
        if (px * px + py * py >= reach * reach) return;
        f.carried += Volume(o.r);
        o.dead = true;
    });
    int steps = std::max(1, static_cast<int>(std::ceil(len / (half * 0.5f))));
    for (int i = 1; i <= steps; ++i) {
        float px = f.lastX + dx * i / steps, py = f.lastY + dy * i / steps;
        erase_.push_back(MakeSplat(px, py, half * 1.3f, half * 1.3f, 1.0f));
        f.carried += EraseRivulets(px, py, half);
        Wet(px, py, half);
        // The film left behind beads up into fine droplets, mostly near the edges.
        if (f.carried > 0 && Rand() < 0.5f) {
            float side = (Rand() < 0.5f ? -1.0f : 1.0f) * half * Rand(0.4f, 1.0f);
            float br = s_ * Rand(0.6f, 1.4f);
            AddMist(px - f.dirY * side, py + f.dirX * side, br);
            f.carried = std::max(f.carried - Volume(br), 0.0f);
        }
    }
    // A fingertip only holds so much; the rest spills off its sides.
    const float pinR = kPinRadius * s_ * friction;
    while (f.carried > Volume(pinR * 1.5f)) {
        float spill = std::min(Volume(pinR * Rand(0.5f, 0.9f)), f.carried);
        float side = (Rand() < 0.5f ? -1.0f : 1.0f) * half * 1.15f;
        Spawn(f.x - f.dirY * side - f.dirX * half * 0.3f, f.y + f.dirX * side - f.dirY * half * 0.3f, RadiusFor(spill));
        f.carried -= spill;
    }
    f.lastX = f.x;
    f.lastY = f.y;
}
