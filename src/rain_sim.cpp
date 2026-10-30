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

Splat MakeSplat(float x, float y, float rx, float ry, float height) {
    Splat s{};
    s.x = x;
    s.y = y;
    s.rx = rx;
    s.ry = ry;
    s.height = height;
    return s;
}

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

// Water volume is roughly proportional to footprint area for drops this flat.
// Speed follows momentum, so a running drop slows when it swallows a still one.
void RainSim::Merge(Drop& into, Drop& from) {
    float a = into.r * into.r, b = from.r * from.r;
    float speed = (into.vy * a + from.vy * b) / (a + b);
    float r = std::min(std::sqrt(a + b), kMaxRadius * kPinRadius * s_ * std::max(friction, 1.0f));
    from.dead = true;
    into.vy = speed;
    if (into.running) {
        into.r = r;
        return;
    }

    // Keep both drops' blobs, re-centred on the combined centre of mass.
    float cx = (into.x * a + from.x * b) / (a + b), cy = (into.y * a + from.y * b) / (a + b);
    Drop::Lobe all[8];
    int n = 0;
    for (int k = 0; k < into.lobeCount; ++k)
        all[n++] = {into.lobes[k].dx + into.x - cx, into.lobes[k].dy + into.y - cy, into.lobes[k].r};
    for (int k = 0; k < from.lobeCount && n < 8; ++k)
        all[n++] = {from.lobes[k].dx + from.x - cx, from.lobes[k].dy + from.y - cy, from.lobes[k].r};
    if (from.lobeCount == 0 && n < 8) all[n++] = {from.x - cx, from.y - cy, from.r};
    // Too many blobs: fold the two closest together (area-weighted).
    while (n > 4) {
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

// A fresh drop is roughly round. Larger ones get a small bulge where the
// contact line snagged as it spread.
void RainSim::ResetShape(Drop& d, bool ragged) {
    d.lobeCount = 1;
    d.lobes[0] = {0, 0, d.r};
    if (ragged && d.r > 3.0f * s_ && Rand() < 0.85f) {
        float br = d.r * Rand(0.35f, 0.6f), a = Rand(0, 6.2832f), off = d.r * Rand(0.4f, 0.7f);
        d.lobes[0].r = std::sqrt(d.r * d.r - br * br);
        d.lobes[1] = {std::cos(a) * off, std::sin(a) * off, br};
        d.lobeCount = 2;
    }
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
    ResetShape(d, true);
    live_.push_back(d);
}

// A raindrop hitting glass leaves one main drop and sometimes a spray of
// small satellites around it.
void RainSim::Impact(float x, float y, float r) {
    Spawn(x, y, r);
    if (Rand() < 0.35f) {
        int n = 2 + static_cast<int>(Rand() * 4);
        for (int i = 0; i < n; ++i) {
            float a = Rand(0, 6.2832f), dist = r * Rand(1.4f, 3.5f);
            Spawn(x + std::cos(a) * dist, y + std::sin(a) * dist, r * Rand(0.1f, 0.3f));
        }
    }
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
    mist_.clear();
    erase_.clear();
    BuildGrid();

    const float s = s_;
    const float pinR = kPinRadius * s * friction;
    const float baseSpeed = kBaseSpeed * s * runSpeed;

    // New rain. Mostly small drops, the odd big splat.
    dropAccum_ += dt * 10.0f * intensity;
    while (dropAccum_ >= 1.0f) {
        dropAccum_ -= 1.0f;
        float lo = std::min(minDrop, maxDrop), hi = std::max(minDrop, maxDrop);
        float r = s * (lo + (hi - lo) * std::pow(Rand(), 2.5f));
        Impact(Rand(0, w_), Rand(0, h_), r);
    }
    // Fine mist that never runs: it slowly fogs the glass until drops sweep it away.
    mistAccum_ += dt * 250.0f * intensity * mist;
    while (mistAccum_ >= 1.0f) {
        mistAccum_ -= 1.0f;
        AddMist(Rand(0, w_), Rand(0, h_), s * Rand(0.8f, 3.0f));
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
            if (d.r > release) {
                d.running = true;
                d.release = release;
                d.vy = 0;
                d.heading = d.targetHeading = Rand(-0.15f, 0.15f);
                d.steerDist = 0;
                d.trailDist = d.r * Rand(0.5f, 1.5f);
                d.streaky = Rand() < 0.55f;
                ResetShape(d, false);
            }
            continue;
        }

        float wetHere = Wetness(d.x, d.y);
        float wetAhead = Wetness(d.x, d.y + d.r + surfCell_);

        if (d.steerDist <= 0) {
            Steer(d);
            d.steerDist = std::max(surfCell_ * 1.5f, d.r);
        }
        // Fast drops have too much momentum to wander much.
        float wander = 1.0f / (1.0f + d.vy / (400.0f * s));
        d.heading += (d.targetHeading * wander - d.heading) * std::min(1.0f, dt * 5.0f);

        // Speed rises steeply with size: a drop that only just broke loose
        // creeps, one that swallowed its neighbours is gone in a blink. A wet
        // path has almost no grip left, so drops run faster along it.
        float slideR = d.release * 0.8f;
        float drive = std::max((d.r * d.r) / (slideR * slideR) - 1.0f, 0.05f);
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

        float step = d.vy * dt;
        d.x += std::sin(d.heading) * step;
        d.y += std::cos(d.heading) * step;
        d.steerDist -= step;
        Wet(d.x, d.y, d.r * 0.8f);

        // Some runs drag a thin rivulet behind them on dry glass. On a wet
        // path the drop rides earlier water instead and leaves none.
        if (d.streaky && wetHere < 0.5f && step > 0)
            LayRivulet(d, step);
        else
            d.rivulet = -1;

        // On dry glass the trailing film breaks into beads. On a wet path the
        // drop mostly picks earlier beads back up, so it sheds less.
        d.trailDist -= step;
        if (d.trailDist <= 0) {
            // Fast drops outrun the film's breakup, so they leave beads further apart.
            d.trailDist = d.r * Rand(0.3f, 1.0f) * (1.0f + 2.5f * wetHere) * (d.streaky ? 2.5f : 1.0f) *
                          (1.0f + d.vy / (300.0f * s));
            float tr = d.r * Rand(0.08f, 0.22f) * (1.0f - 0.5f * wetHere);
            // Left at the rear contact line, behind the tail, so beads never sit inside it.
            float k = std::clamp(d.vy / (600.0f * s), 0.0f, 1.0f);
            float back = d.r * (1.75f + 1.8f * k) + tr;
            beads.push_back({d.x - std::sin(d.heading) * back + Rand(-0.25f, 0.25f) * d.r,
                             d.y - std::cos(d.heading) * back, tr});
            d.r = std::sqrt(std::max(d.r * d.r - tr * tr * 0.7f, 0.0f));
        }

        // Swallow whatever is in the way, including old rivulets it crosses.
        EraseRivulets(d.x, d.y, d.r * 0.9f, d.rivulet);
        ForNeighbors(d.x, d.y, d.r * 2.0f + 20.0f * s, [&](int j) {
            if (j == i) return;
            Drop& o = live_[j];
            if (o.dead) return;  // beads it just left are spawned out of reach
            float ox = o.x - d.x, oy = o.y - d.y;
            float reach = (d.r + o.r) * 0.95f;
            if (ox * ox + oy * oy < reach * reach) Merge(d, o);
        });

        float k = std::clamp(d.vy / (600.0f * s), 0.0f, 1.0f);
        erase_.push_back(MakeSplat(d.x, d.y - d.r * 0.4f * k, d.r * 1.15f, d.r * (1.15f + 0.8f * k), 1.0f));

        // Pins again once it has shed enough, unless it is riding a wet path.
        // Receding grip is weaker than advancing grip, so a drop keeps going
        // until it has shed down to about 80% of the size it let go at.
        if (d.r < slideR && wetAhead < 0.3f) {
            d.running = false;
            d.vy = 0;
            d.eraseTimer = 0;
            ResetShape(d, false);
        }
        if (d.y - d.r * 3.0f > h_ || d.x < -d.r * 3.0f || d.x > w_ + d.r * 3.0f) d.dead = true;
    }

    // Spawn before compacting: Spawn looks up neighbours through this frame's grid indices.
    for (const Pending& p : beads) Spawn(p.x, p.y, p.r);
    UpdateWiper(dt);
    UpdateRivulets(dt);
    live_.erase(std::remove_if(live_.begin(), live_.end(), [](const Drop& d) { return d.dead; }), live_.end());

    drops_.clear();
    drops_.reserve(live_.size() * 3);
    for (const Rivulet& rv : rivulets_)
        for (const RivuletNode& n : rv.nodes)
            if (n.alive) drops_.push_back(MakeSplat(n.x, n.y, n.w * 1.1f, n.w * 1.1f, n.w * kHeightPerRadius));
    for (const Drop& d : live_) {
        if (!d.running) {
            // Heavy drops sag: gravity pulls them longer and narrower as they near release.
            float sag = 1.0f + 0.3f * std::clamp(d.r / (kPinRadius * s_ * friction) - 0.4f, 0.0f, 1.0f);
            for (int k = 0; k < d.lobeCount; ++k) {
                const Drop::Lobe& l = d.lobes[k];
                drops_.push_back(MakeSplat(d.x + l.dx, d.y + l.dy * sag, l.r * kSpriteScale / std::sqrt(sag),
                                           l.r * kSpriteScale * sag, l.r * kHeightPerRadius));
            }
            continue;
        }
        // A running drop is a teardrop: a fat head with a tapering tail of
        // film dragged out behind it, longer the faster it goes.
        float height = d.r * kHeightPerRadius;
        float k = std::clamp(d.vy / (600.0f * s), 0.0f, 1.0f);
        float dx = std::sin(d.heading), dy = std::cos(d.heading);
        drops_.push_back(MakeSplat(d.x, d.y, d.r * kSpriteScale * (1.0f - 0.12f * k),
                                   d.r * kSpriteScale * (1.0f + 0.15f * k), height));
        if (k > 0.05f) {
            // Close enough together that the three sprites fuse into one outline.
            float t1 = d.r * (0.45f + 0.4f * k), t2 = d.r * (0.9f + 0.9f * k);
            drops_.push_back(MakeSplat(d.x - dx * t1, d.y - dy * t1, d.r * 0.8f * kSpriteScale,
                                       d.r * 0.95f * kSpriteScale, height * 0.7f));
            drops_.push_back(MakeSplat(d.x - dx * t2, d.y - dy * t2, d.r * 0.5f * kSpriteScale,
                                       d.r * 0.85f * kSpriteScale, height * 0.4f));
        }
    }
}

// ---------------------------------------------------------------- rivulets

// A drop running fast over dry glass drags a thin rivulet out of its tail.
// The rivulet costs the drop water as it goes.
void RainSim::LayRivulet(Drop& d, float step) {
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
    float k = std::clamp(d.vy / (600.0f * s_), 0.0f, 1.0f);
    float back = d.r * (1.0f + 1.2f * k);
    float tx = d.x - std::sin(d.heading) * back, ty = d.y - std::cos(d.heading) * back;
    float w0 = d.r * 0.26f;
    float spacing = std::max(w0 * 0.5f, 0.8f * s_);

    auto add = [&](float x, float y, float along) {
        RivuletNode n;
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
        for (int i = 1; i <= n; ++i) {
            float t = i * spacing / dist;
            add(lx + dx * t, ly + dy * t, along + i * spacing);
        }
    }
    d.r = std::sqrt(std::max(d.r * d.r - w0 * step * 0.05f, 0.0f));
}

// Old drops and the wiper soak up any rivulet they pass over.
void RainSim::EraseRivulets(float x, float y, float radius, int skip) {
    for (int i = 0; i < static_cast<int>(rivulets_.size()); ++i) {
        Rivulet& rv = rivulets_[i];
        if (i == skip || x + radius < rv.minX || x - radius > rv.maxX || y + radius < rv.minY ||
            y - radius > rv.maxY)
            continue;
        for (RivuletNode& n : rv.nodes) {
            float dx = n.x - x, dy = n.y - y;
            if (dx * dx + dy * dy < radius * radius) n.alive = false;
        }
    }
}

// A rivulet left on the glass is unstable. It drains from its oldest end
// (the top) first. Further down, surface tension pinches it into segments
// about ten widths long (Plateau-Rayleigh), the necks thin until they snap,
// and each segment pulls itself up into a round droplet. What is left is a
// line of small drops along the path.
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
        for (RivuletNode& n : rv.nodes) {
            if (!n.alive) continue;
            float age = time_ - n.born;
            float pinch = smooth(1.0f * life, 4.0f * life, age);
            float lambda = rv.wavelength * n.w0 * 2.0f;
            float wave = std::sin(6.2832f * n.along / lambda + rv.phase) +
                         0.3f * std::sin(6.2832f * n.along / (lambda * 0.43f) + rv.phase * 2.0f);
            float drain = std::exp(-std::max(age - 3.5f * life, 0.0f) / (3.0f * life));
            n.w = n.w0 * std::max(1.0f + 0.8f * pinch * wave, 0.0f) * drain;
            if (n.w < 0.35f * n.w0 || drain < 0.3f) n.alive = false;
        }

        // Segments that have pinched off pull up into droplets, each at its
        // own moment.
        const int count = static_cast<int>(rv.nodes.size());
        for (int i = 0; i < count;) {
            if (!rv.nodes[i].alive) {
                ++i;
                continue;
            }
            int j = i;
            float youngest = -1e9f;
            while (j < count && rv.nodes[j].alive) youngest = std::max(youngest, rv.nodes[j++].born);
            bool pinchedOff = i > 0 && j < count;
            float span = rv.nodes[j - 1].along - rv.nodes[i].along;
            float lambda = rv.wavelength * rv.nodes[i].w0 * 2.0f;
            if (pinchedOff && time_ - youngest > 3.0f * life && span < 2.0f * lambda &&
                Rand() < dt * 2.5f / life) {
                float area = 0, cx = 0, cy = 0;
                for (int m = i; m < j; ++m) {
                    RivuletNode& n = rv.nodes[m];
                    float step = m + 1 < count ? rv.nodes[m + 1].along - n.along : 1.0f;
                    float a = 2.0f * n.w * step;
                    area += a;
                    cx += n.x * a;
                    cy += n.y * a;
                    n.alive = false;
                }
                if (area > 0) Spawn(cx / area, cy / area, 0.8f * std::sqrt(area / 3.1416f));
            }
            i = j;
        }
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
