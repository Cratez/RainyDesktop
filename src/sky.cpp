#include "sky.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// The viewer sits about 65 cm from the screen, which is the window pane. A
// 27" 16:9 panel is about 33.6 cm tall, which sets how many pixels a metre
// at the glass covers.
constexpr float kEye = 0.65f;          // m, eye to glass
constexpr float kPanelHeight = 0.336f; // m
constexpr float kNear = 0.25f;         // m behind the glass: nearest drops drawn
constexpr float kFar = 3.5f;           // m: further ones are too faint and blurred to see
constexpr float kPupil = 0.003f;       // m, the eye's aperture, which sets the defocus blur
constexpr float kExposure = 1.0f / 50.0f;  // s, how long the eye smears a moving drop into a streak
// Real streaks are faint (a drop covers a pixel for well under a millisecond
// of the exposure) but they show because each drop refracts the whole sky,
// far brighter than anything on a monitor. This makes up for that.
constexpr float kStreakGain = 22.0f;
constexpr int kBaseDrops = 1350;       // drawn at the default rain amount
constexpr float kSpan = 1.25f;         // drops fill a little more than the view's width, for streaks entering at the sides
constexpr int kMaxDrops = 6000;

constexpr float kSpeedOfSound = 0.343f;  // km/s
constexpr float kPi = 3.14159265f;

float Smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Smooth random values in -1..1 on a grid, interpolated between (value noise).
float Hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h) * (2.0f / 4294967295.0f) - 1.0f;
}
float ValueNoise(float x, float y) {
    float fx = std::floor(x), fy = std::floor(y);
    int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    float u = x - fx, v = y - fy;
    u = u * u * (3.0f - 2.0f * u);
    v = v * v * (3.0f - 2.0f * v);
    float a = Hash(ix, iy) + (Hash(ix + 1, iy) - Hash(ix, iy)) * u;
    float b = Hash(ix, iy + 1) + (Hash(ix + 1, iy + 1) - Hash(ix, iy + 1)) * u;
    return a + (b - a) * v;
}

}  // namespace

void Sky::Reset(float width, float height) {
    w_ = width;
    h_ = height;
    drops_.clear();
    streaks_.clear();
    bolts_.clear();
    rumbles_.clear();
    flash_ = 0;
    shake_ = 0;
    boltTimer_ = 5.0f;
}

float Sky::RainRate() const {
    // The default rain amount is a steady moderate rain, about 6 mm an hour.
    return raining ? 6.0f * rain * showerNow_ : 0.0f;
}

std::vector<ThunderEvent> Sky::TakeThunder() {
    std::vector<ThunderEvent> out;
    out.swap(thunder_);
    return out;
}

void Sky::Update(float dt, float windNow) {
    time_ += dt;
    UpdateShowers(dt);
    UpdateLightning(dt);
    UpdateShake(dt);
    UpdateLight(dt);
    UpdateOutside(dt, windNow);
}

// ---------------------------------------------------------------- daylight

// NOAA's general solar position formulas: good to a fraction of a degree,
// far more than daylight needs.
float Sky::SunElevation(int dayOfYear, float utcHours, float longitude, float latitude) {
    float g = 2.0f * kPi / 365.0f * (dayOfYear - 1 + (utcHours - 12.0f) / 24.0f);  // fractional year
    // The equation of time (minutes): the sun runs up to 16 minutes ahead of
    // or behind the clock over the year, because the orbit is elliptical and
    // the axis tilted.
    float eqTime = 229.18f * (0.000075f + 0.001868f * std::cos(g) - 0.032077f * std::sin(g) -
                              0.014615f * std::cos(2 * g) - 0.040849f * std::sin(2 * g));
    // Declination: how far north of the equator the sun is overhead today.
    float decl = 0.006918f - 0.399912f * std::cos(g) + 0.070257f * std::sin(g) - 0.006758f * std::cos(2 * g) +
                 0.000907f * std::sin(2 * g) - 0.002697f * std::cos(3 * g) + 0.00148f * std::sin(3 * g);
    // Hour angle: how far the sun has turned past local noon.
    float solarMinutes = utcHours * 60.0f + eqTime + 4.0f * longitude;
    float hourAngle = (solarMinutes / 4.0f - 180.0f) * kPi / 180.0f;
    float lat = latitude * kPi / 180.0f;
    float sinEl = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(hourAngle);
    return std::asin(std::clamp(sinEl, -1.0f, 1.0f)) * 180.0f / kPi;
}

void Sky::SetClock(int dayOfYear, float utcHours, float longitude, float latitude) {
    sunElevation_ = SunElevation(dayOfYear, utcHours, longitude, latitude);
}

// The light the drops refract is the sky's. Under rain cloud it is a flat
// grey in the day; it dims through dusk (civil twilight ends with the sun
// 6 degrees below the horizon) to the dull orange of clouds lit from below by
// streetlights at night. Heavy rain falls from thicker, darker cloud.
void Sky::UpdateLight(float dt) {
    float target = daylight >= 0 ? std::min(daylight, 1.0f) : Smoothstep(-6.0f, 12.0f, sunElevation_);
    // Follow changes gently, so dragging the setting or the sun setting never jumps.
    daylightNow_ += (target - daylightNow_) * std::min(1.0f, dt * 2.0f);
    float d = daylightNow_;
    const SkyColor night{0.26f, 0.21f, 0.16f}, day{0.80f, 0.84f, 0.90f};
    float dusk = 4.0f * d * (1.0f - d);  // the blue hour, strongest halfway
    float cloud = 1.0f / (1.0f + 0.1f * std::max(RainRate() / 6.0f - 1.0f, 0.0f));
    cloud = std::max(cloud, 0.7f);
    sky_.r = (night.r + (day.r - night.r) * d - 0.03f * dusk) * cloud;
    sky_.g = (night.g + (day.g - night.g) * d - 0.01f * dusk) * cloud;
    sky_.b = (night.b + (day.b - night.b) * d + 0.05f * dusk) * cloud;
}

// ---------------------------------------------------------------- shaking

// Close thunder shakes the window: the boom arrives with the sound and the
// pane vibrates for a second or two. Vibration helps drops past their
// contact-angle hysteresis, so drops that were nearly ready to go let go.
void Sky::UpdateShake(float dt) {
    shake_ *= std::exp(-dt / 1.2f);
    for (size_t i = 0; i < rumbles_.size();) {
        rumbles_[i].wait -= dt;
        if (rumbles_[i].wait <= 0) {
            shake_ = std::max(shake_, rumbles_[i].strength);
            rumbles_[i] = rumbles_.back();
            rumbles_.pop_back();
            continue;
        }
        ++i;
    }
}

// ---------------------------------------------------------------- showers

// Rain rarely holds steady: cells pass over, so it builds for a minute or
// two, eases off and comes back. The log of the rate does a random walk that
// is pulled back to the setting with a time constant of about 75 s; its
// spread grows with the "showers" setting. exp(sigma x - sigma^2 / 2) keeps
// the long-run average at the rain amount the user picked.
void Sky::UpdateShowers(float dt) {
    const float tau = 75.0f;
    std::normal_distribution<float> normal(0.0f, 1.0f);
    showerLog_ += -showerLog_ * dt / tau + std::sqrt(2.0f * dt / tau) * normal(rng_);
    float sigma = 0.6f * showers;
    showerNow_ = std::clamp(std::exp(sigma * showerLog_ - 0.5f * sigma * sigma), 0.05f, 5.0f);
}

// ---------------------------------------------------------------- lightning

void Sky::UpdateLightning(float dt) {
    // Flashes arrive at random (a Poisson process), more often in the
    // heaviest rain. At 1x about one every 25 seconds.
    boltTimer_ -= dt;
    if (lightning > 0 && raining && boltTimer_ <= 0) {
        float storm = std::clamp(0.3f + 0.7f * showerNow_ * showerNow_, 0.3f, 3.0f);
        float rate = lightning * storm / 25.0f;
        if (Rand() < rate * dt) {
            StartBolt();
            boltTimer_ = 1.5f;  // the sky takes a moment to recharge
        }
    }

    // Brightness averaged over this frame, so a stroke shorter than a frame
    // still shows at its true strength: the integral of
    // peak * exp(-(t - at) / decay) over the frame, divided by dt.
    flash_ = 0;
    float strongest = 0;
    for (Bolt& b : bolts_) {
        b.age += dt;
        float t1 = b.age, t0 = b.age - dt;
        float sum = 0;
        for (const Stroke& s : b.strokes) {
            if (t1 <= s.at) continue;
            float a = std::max(t0 - s.at, 0.0f), c = t1 - s.at;
            sum += s.peak * s.decay * (std::exp(-a / s.decay) - std::exp(-c / s.decay)) / dt;
            // Continuing current: a dimmer glow that holds, then fades.
            if (s.glow > 0 && a < s.glowFor * 2.0f) {
                float mid = 0.5f * (a + c);
                sum += s.glow * std::exp(-mid / s.glowFor) * (c - a) / dt;
            }
        }
        flash_ += sum;
        if (sum > strongest) {
            strongest = sum;
            flashDirX_ = b.dirX;
            flashDirY_ = b.dirY;
        }
    }
    bolts_.erase(std::remove_if(bolts_.begin(), bolts_.end(), [](const Bolt& b) { return b.age > b.length; }),
                 bolts_.end());
}

void Sky::StartBolt() {
    Bolt b;
    // Strikes spread over the area of the storm around you, so distant ones
    // are more common, out to about 12 km. (Evenly over the area would be
    // 12 sqrt(u); the storm is densest overhead, which this leans toward.)
    float distance = 0.5f + 11.5f * std::pow(Rand(), 0.8f);
    // Seen from under a cloud deck, a nearby strike lights everything; a
    // distant one lights the clouds, which spread its light over the whole
    // sky, so it fades with distance far more slowly than a bare point would.
    float scale = 1.4f / (1.0f + distance / 4.0f);
    float angle = Rand(-1.2f, 1.2f);  // where in the sky, from straight up
    b.dirX = std::sin(angle);
    b.dirY = -std::cos(angle);

    std::lognormal_distribution<float> gap(std::log(0.055f), 0.5f);
    if (Rand() < 0.6f) {
        // A cloud flash: most lightning never reaches the ground. It lights
        // the clouds from inside in a soft, irregular flicker.
        scale *= 0.6f;
        int pulses = 3 + static_cast<int>(Rand() * 7);
        float t = 0;
        for (int k = 0; k < pulses; ++k) {
            b.strokes.push_back({t, scale * Rand(0.3f, 1.0f), Rand(0.04f, 0.12f), 0, 0});
            t += Rand(0.03f, 0.12f);
        }
    } else {
        // Cloud to ground: a train of return strokes down the same channel.
        // About one flash in five has a single stroke; the rest have three or
        // four on average, roughly 60 ms apart. The first is usually the
        // brightest, and many strokes are followed by continuing current.
        int strokes = Rand() < 0.2f ? 1 : 2;
        while (strokes > 1 && strokes < 9 && Rand() < 0.6f) ++strokes;
        float t = 0;
        for (int k = 0; k < strokes; ++k) {
            Stroke s{t, scale * (k == 0 ? 1.0f : Rand(0.35f, 0.85f)), Rand(0.025f, 0.05f), 0, 0};
            if (Rand() < 0.3f) {
                s.glow = s.peak * Rand(0.15f, 0.35f);
                s.glowFor = Rand(0.04f, 0.25f);
            }
            b.strokes.push_back(s);
            t += std::clamp(gap(rng_), 0.015f, 0.2f);
        }
    }
    for (const Stroke& s : b.strokes) b.length = std::max(b.length, s.at + 6.0f * s.decay + 2.0f * s.glowFor);
    bolts_.push_back(b);

    // Sound travels about 343 m a second, so thunder trails the flash by
    // about 3 seconds per kilometre. Only a close strike is loud enough to
    // shake the window, and its boom builds over a fraction of a second.
    float power = std::min(scale, 1.0f);
    float rattle = power * std::exp(-distance / 1.5f);
    thunder_.push_back({distance / kSpeedOfSound, distance, power, b.dirX * 0.7f, rattle});
    if (rattle > 0.05f) rumbles_.push_back({distance / kSpeedOfSound + 0.3f, rattle});
}

// ---------------------------------------------------------------- rain outside

// Puts a drop somewhere in the visible space behind the glass: at the top,
// ready to fall into view, or (when filling the sky at the start) anywhere.
void Sky::PlaceDrop(Drop& d, bool anywhere) {
    // Equal numbers per unit volume, and the visible volume at depth z grows
    // as (eye distance + z)^2, so sample depth by inverting that cubic CDF.
    float a = kEye + kNear, b = kEye + kFar;
    float u = Rand();
    d.z = std::cbrt(a * a * a + u * (b * b * b - a * a * a)) - kEye;

    // Drop sizes follow Marshall and Palmer: exponentially fewer big drops,
    // N(D) ~ exp(-L D) with L = 4.1 R^-0.21 per mm at R mm/h. Only drops over
    // half a millimetre are big enough to see, and the exponential is
    // memoryless, so their diameters are 0.5 mm plus an exponential sample.
    float rate = std::max(RainRate(), 0.5f);
    float lambda = 4.1f * std::pow(rate, -0.21f);
    float diameter = std::min(0.5f + std::exponential_distribution<float>(lambda)(rng_), 6.0f);  // mm
    d.radius = diameter * 0.0005f;
    // Terminal fall speed (Atlas et al. 1973): v = 9.65 - 10.3 exp(-0.6 D), m/s.
    d.speed = std::max(9.65f - 10.3f * std::exp(-0.6f * diameter), 0.5f);
    d.drift = Rand(-0.3f, 0.3f);

    float metresPerPixel = kPanelHeight / h_;
    float grow = (kEye + d.z) / kEye;  // the view widens with depth
    float halfW = 0.5f * w_ * metresPerPixel * grow, halfH = 0.5f * h_ * metresPerPixel * grow;
    d.x = Rand(-halfW * kSpan, halfW * kSpan);
    d.y = anywhere ? Rand(-halfH, halfH) : -halfH - Rand(0.0f, d.speed * 0.15f);
}

void Sky::UpdateOutside(float dt, float windNow) {
    streaks_.clear();
    float rate = RainRate();
    int target = 0;
    if (outside > 0 && rate > 0)
        target = std::min(static_cast<int>(kBaseDrops * outside * std::sqrt(rate / 6.0f)), kMaxDrops);
    // More drops arrive at the top as the rain builds; when it eases, those
    // that fall out of view are not replaced.
    // A big jump (just switched on, or the first frame) fills the whole view
    // at once rather than sending a curtain of rain down from the top.
    bool fill = target - static_cast<int>(drops_.size()) > target / 5;
    while (static_cast<int>(drops_.size()) < target) {
        Drop d{};
        PlaceDrop(d, fill);
        drops_.push_back(d);
    }

    const float pxPerMetre = h_ / kPanelHeight;
    const float cx = 0.5f * w_, cy = 0.5f * h_;
    // Along the glass the wind is in RainSim units (about -3..3); outside it
    // blows the falling rain sideways at a couple of metres a second per unit.
    const float windX = windNow * 2.0f;
    const float exposure = std::max(kExposure, dt);
    // Gusty wind drives sheets of heavier rain past: the density of the rain
    // varies over a few metres and the pattern blows along with the wind.
    sheetShift_ += windX * dt;
    const float sheets = std::min(0.5f * gusts * std::min(std::fabs(windX) / 2.0f, 1.0f), 0.9f);

    for (size_t i = 0; i < drops_.size();) {
        Drop& d = drops_[i];
        float vx = windX + d.drift;
        d.x += vx * dt;
        d.y += d.speed * dt;
        float grow = (kEye + d.z) / kEye;
        float halfH = 0.5f * h_ / pxPerMetre * grow, halfW = 0.5f * w_ / pxPerMetre * grow;
        // The view is a slice of rain that goes on to either side: a drop blown
        // out of one side comes back in at the other, so the wind doesn't
        // thin the rain on the upwind side.
        if (std::fabs(d.x) > halfW * kSpan) d.x -= std::copysign(halfW * 2.0f * kSpan, d.x);
        if (d.y - d.speed * exposure > halfH) {
            if (static_cast<int>(drops_.size()) > target) {
                d = drops_.back();
                drops_.pop_back();
                continue;
            }
            PlaceDrop(d, false);
        }

        // Perspective: a point (x, y) metres off the line of sight at depth z
        // lands on the glass at eye / (eye + z) of that.
        float k = pxPerMetre * kEye / (kEye + d.z);
        Streak s{};
        s.x1 = cx + d.x * k;
        s.y1 = cy + d.y * k;
        s.x0 = cx + (d.x - vx * exposure) * k;
        s.y0 = cy + (d.y - d.speed * exposure) * k;
        // Seen sharp, a drop is its own diameter wide. The eye is focused on
        // the glass, so a drop z behind it blurs into a disc of
        // pupil * z / (eye + z) on the glass, which widens the streak and
        // spreads the same light thinner.
        float size = 2.0f * d.radius * k;
        float blur = kPupil * d.z / (kEye + d.z) * pxPerMetre;
        s.width = std::max(std::sqrt(size * size + blur * blur), 1.0f);
        // Each pixel on the path is covered for (size / speed) of the
        // exposure, so the streak's opacity is size / length, thinned by the blur.
        float length = std::max(std::hypot(s.x1 - s.x0, s.y1 - s.y0), size);
        // The setting also makes each streak a little stronger or fainter.
        float gain = kStreakGain * (0.6f + 0.4f * outside);
        if (sheets > 0)
            gain *= std::clamp(1.0f + sheets * 1.6f * ValueNoise((d.x - sheetShift_) / 6.0f + d.z * 0.15f, time_ / 7.0f),
                               0.1f, 2.2f);
        s.alpha = std::min(gain * size * size / (length * s.width), 0.35f);
        if (s.alpha > 0.004f) streaks_.push_back(s);
        ++i;
    }
}
