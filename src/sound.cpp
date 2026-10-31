#include "sound.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRate = static_cast<float>(RainSound::kRate);

// Equal-power panning: -1 left, 0 centre, 1 right.
void Pan(float pan, float& l, float& r) {
    float a = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * kPi * 0.25f;
    l = std::cos(a);
    r = std::sin(a);
}

// Overall gains of the parts of the mix, set by ear against each other.
constexpr float kTapGain = 0.4f;
constexpr float kGrainGain = 9.0f;
constexpr float kBedGain = 0.12f;
constexpr float kDripGain = 0.25f;
constexpr float kCarGain = 0.12f;
constexpr float kWindGain = 0.05f;
constexpr float kShutGain = 1.0f;  // makes up the level the shut window's filter takes away

}  // namespace

// ---------------------------------------------------------------- filters

// Two-pole resonator: rings at freq, decaying 60 dB in t60 seconds.
void RainSound::Resonator::Set(float freq, float t60, float g) {
    float r = std::exp(-6.91f / (t60 * kRate));
    a1 = 2.0f * r * std::cos(2.0f * kPi * freq / kRate);
    a2 = r * r;
    gain = g * (1.0f - r);
}

void RainSound::OnePole::Set(float cutoff) { a = 1.0f - std::exp(-2.0f * kPi * cutoff / kRate); }

// RBJ cookbook biquads.
void RainSound::Biquad::Bandpass(float freq, float q) {
    float w = 2.0f * kPi * freq / kRate, alpha = std::sin(w) / (2.0f * q), a0 = 1.0f + alpha;
    b0 = alpha / a0;
    b1 = 0;
    b2 = -alpha / a0;
    a1 = -2.0f * std::cos(w) / a0;
    a2 = (1.0f - alpha) / a0;
}
void RainSound::Biquad::Highpass(float freq, float q) {
    float w = 2.0f * kPi * freq / kRate, alpha = std::sin(w) / (2.0f * q), c = std::cos(w), a0 = 1.0f + alpha;
    b0 = (1.0f + c) * 0.5f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
}
void RainSound::Biquad::Lowpass(float freq, float q) {
    float w = 2.0f * kPi * freq / kRate, alpha = std::sin(w) / (2.0f * q), c = std::cos(w), a0 = 1.0f + alpha;
    b0 = (1.0f - c) * 0.5f / a0;
    b1 = (1.0f - c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
}

// Boosts (or cuts) a band around freq by gainDb.
void RainSound::Biquad::Peak(float freq, float q, float gainDb) {
    float A = std::pow(10.0f, gainDb / 40.0f);
    float w = 2.0f * kPi * freq / kRate, alpha = std::sin(w) / (2.0f * q), c = std::cos(w), a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = -2.0f * c / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = b1;
    a2 = (1.0f - alpha / A) / a0;
}

// ---------------------------------------------------------------- controls

RainSound::RainSound() {
    seed_ ^= 0x9E3779B9u;
    for (int ch = 0; ch < 2; ++ch) {
        tickHp_[ch].Highpass(1500, 0.7f);
        bodyHp_[ch].Highpass(120, 0.7f);
        bedHp_[ch].Highpass(120, 0.7f);
        bedLp_[ch].Lowpass(4000, 0.6f);
        roarLp_[ch].Lowpass(250, 0.7f);
        windLp_[ch].Lowpass(300, 0.8f);
        windBp_[ch].Bandpass(400, 2.0f);
        texture_[ch].Set(6);
        // A shut window: glass passes sound less the higher it goes, about
        // 6 dB an octave (the mass law), except near its coincidence
        // frequency (about 3 kHz for window glass), where bending waves in
        // the pane match the sound in the air and it lets more through.
        glassLp_[ch].Set(650);
        glassPeak_[ch].Peak(3000, 1.4f, 8);
    }
    whistle_.Bandpass(600, 30);
    reverb_.Init(0.9f);
    // Where the gutter drips: a couple of outlets into puddles and a sill,
    // each with its own drop size and pace.
    for (DripSource& d : drips_) {
        d.pan = Rand(-0.8f, 0.8f);
        d.amp = Rand(0.4f, 1.0f);
        d.radiusMm = Rand(2.0f, 4.5f);
        d.interval = Rand(0.35f, 1.3f);
        d.next = Rand(0, 2);
    }
}

void RainSound::SetLevels(float volume, float glass, float outside, float thunder) {
    std::lock_guard lock(lock_);
    volume_ = volume;
    glassLevel_ = glass;
    outsideLevel_ = outside;
    thunderLevel_ = thunder;
}

void RainSound::SetAmbience(float wind, float drips, float cars, float window) {
    std::lock_guard lock(lock_);
    windLevel_ = wind;
    dripLevel_ = drips;
    carLevel_ = cars;
    window_ = window;
}

void RainSound::SetWeather(float rainRate, float wind, float daylight) {
    std::lock_guard lock(lock_);
    rainRate_ = rainRate;
    wind_ = wind;
    daylight_ = daylight;
}

float RainSound::Volume() const {
    std::lock_guard lock(lock_);
    return volume_;
}

void RainSound::Tap(float pan, float radiusMm, float glance, bool splash, bool burst) {
    std::lock_guard lock(lock_);
    if (volume_ <= 0 || glassLevel_ <= 0 || taps_.size() > 256) return;
    taps_.push_back({pan, radiusMm, glance, splash, burst});
}

void RainSound::Thunder(float delay, float distanceKm, float power, float pan, float rattle) {
    std::lock_guard lock(lock_);
    if (volume_ <= 0 || thunderLevel_ <= 0 || pendingThunder_.size() > 4) return;
    pendingThunder_.emplace_back();
    ThunderVoice& v = pendingThunder_.back();
    v.wait = static_cast<int>(delay * kRate);
    v.distance = distanceKm;
    v.power = power;
    v.rattle = rattle;
    Pan(pan * 0.5f, v.gainL, v.gainR);
}

float RainSound::Interval(float rate) {
    if (rate <= 0) return 1e9f;
    return -std::log(Rand(1e-6f, 1.0f)) / rate;
}

// ---------------------------------------------------------------- glass

void RainSound::StartTap(const PendingTap& tp, int frames) {
    if (voices_.size() >= 96) return;
    TapVoice v;
    // Each tap starts somewhere in this buffer: the frame they came from is
    // about as long as the buffer, and spreading them avoids a machine-gun beat.
    v.wait = static_cast<int>(Rand() * frames);
    // Contact time of the impact: bigger drops press longer, so they sound lower.
    v.contact = std::max(2, static_cast<int>((0.00012f + 0.00025f * tp.radius) * kRate));
    v.force = std::min(std::pow(tp.radius / 1.2f, 1.5f), 4.0f) * (0.6f + 0.6f * tp.glance) *
              (tp.splash ? 1.5f : 1.0f) * 0.5f * kTapGain;
    Pan(tp.pan, v.gainL, v.gainR);
    // The pane rings for a few milliseconds around the strike, no higher
    // than the contact can excite (a half-sine has little above 1.5 / its length).
    v.ring = v.force * 0.6f;
    v.ringDecay = std::exp(-1.0f / (Rand(1.5f, 4.0f) * 0.001f * (0.7f + 0.3f * std::min(tp.radius, 2.0f)) * kRate));
    v.ringLp.Set(std::clamp(0.6f * kRate / v.contact, 900.0f, 6000.0f));
    // Two of the nearer modes, wherever it struck: never the same pair twice.
    for (Resonator& b : v.body) b.Set(220.0f * std::exp(Rand() * 2.2f), Rand(0.008f, 0.03f), 0.8f);
    // The drop shatters into spray, a brief hiss that gives the tap its
    // crisp edge; splashes hiss longer.
    v.spray = 0.6f * (0.5f + tp.glance) * (tp.splash ? 1.4f : 1.0f);
    v.sprayDecay = std::exp(-1.0f / ((tp.splash ? 0.004f : 0.0012f) * kRate));
    v.life = v.contact + static_cast<int>(0.06f * kRate);
    voices_.push_back(v);

    // A splash throws droplets that land again around it a moment later: a
    // crackle of tiny clicks, spread wide for a burst.
    int clicks = tp.burst ? 6 + static_cast<int>(Rand() * 7) : tp.splash ? 2 + static_cast<int>(Rand() * 4) : 0;
    for (int k = 0; k < clicks && ticks_.size() < 256; ++k) {
        Tick t;
        t.wait = v.wait + v.contact + static_cast<int>(Rand(0.004f, tp.burst ? 0.12f : 0.05f) * kRate);
        t.length = 2 + static_cast<int>(Rand() * 3);
        t.amp = v.force * Rand(0.05f, 0.25f);
        Pan(tp.pan + Rand(-1.0f, 1.0f) * (tp.burst ? 0.5f : 0.25f), t.gainL, t.gainR);
        ticks_.push_back(t);
    }
}

// The pane knocking against its frame as close thunder shakes it: a hard,
// brief knock (bright), a dry buzz, and a loose pane ringing a little longer.
void RainSound::StartKnock(float amp) {
    if (voices_.size() >= 96) return;
    TapVoice v;
    v.wait = 0;
    v.contact = 3 + static_cast<int>(Rand() * 6);
    v.force = amp;
    Pan(Rand(-0.6f, 0.6f), v.gainL, v.gainR);
    v.ring = amp * 0.5f;
    v.ringDecay = std::exp(-1.0f / (0.006f * kRate));
    v.ringLp.Set(5000);
    for (Resonator& b : v.body) b.Set(500.0f * std::exp(Rand() * 2.0f), Rand(0.03f, 0.08f), 1.0f);
    v.spray = 0.5f;
    v.sprayDecay = std::exp(-1.0f / (0.0006f * kRate));
    v.life = v.contact + static_cast<int>(0.12f * kRate);
    voices_.push_back(v);
}

// ---------------------------------------------------------------- outside

// One raindrop landing somewhere outside.
void RainSound::StartGrain() {
    if (grains_.size() >= 128) return;
    Grain g;
    // Where it lands, from the street below the window (6 m) out to 30 m:
    // drops fall evenly over the ground, so there are more of them the
    // further out you listen (their number grows with distance) but each is
    // fainter (1 / distance). That alone gives the patter its few louder,
    // nearer ticks among many soft, far ones.
    constexpr float d0 = 6.0f, d1 = 30.0f;
    float d = std::sqrt(d0 * d0 + Rand() * (d1 * d1 - d0 * d0));
    float far = (d - d0) / (d1 - d0);
    float size = Rand();
    float amp = (0.25f + 0.75f * size * size * size) * 4.0f / d * kGrainGain * grainLevel_;  // most raindrops are small
    float u = Rand(), fc, q, tau;
    if (u < 0.55f) {  // paving and stone: a crisp tick
        fc = 900.0f * std::exp(Rand() * 1.9f);
        q = 0.9f;
        tau = Rand(0.3f, 1.2f);
    } else if (u < 0.85f) {  // leaves and grass: a softer pat
        fc = 450.0f * std::exp(Rand() * 1.4f);
        q = 1.3f;
        tau = Rand(1.0f, 3.0f);
    } else {  // roofs, cars, bins: rings a little
        fc = 1800.0f * std::exp(Rand() * 1.1f);
        q = 3.5f;
        tau = Rand(0.8f, 2.5f);
    }
    fc *= 1.0f - 0.35f * far;  // the furthest are a little duller
    g.bp.Bandpass(fc, q);
    g.env = amp;
    g.decay = std::exp(-1.0f / (tau * 0.001f * kRate));
    float pan = Rand(-1.0f, 1.0f);
    Pan(pan, g.gainL, g.gainR);
    g.send = 0.1f + 0.6f * far;
    grains_.push_back(g);
    // Now and then one lands in a puddle and traps a bubble.
    if (Rand() < 0.02f) StartBubble(Rand(1200.0f, 4200.0f), amp * 0.35f, pan, g.send);
}

void RainSound::StartBubble(float freq, float amp, float pan, float send) {
    if (bubbles_.size() >= 32) return;
    Bubble b;
    b.freq = freq;
    // Damping from viscosity, heat and radiation: d = 0.043 f + 0.0014 f^1.5 per second.
    float d = 0.043f * freq + 0.0014f * std::pow(freq, 1.5f);
    b.decay = std::exp(-d / kRate);
    // f(t) = f0 (1 + 0.1 d t): it rises about a third while it rings.
    b.rise = freq * 0.1f * d / kRate;
    b.amp = amp;
    Pan(pan, b.gainL, b.gainR);
    b.send = send;
    bubbles_.push_back(b);
}

// The gutter fills while it rains, more in heavier rain, and keeps draining
// for a minute or two after it stops. Each outlet drips faster the more
// water there is, never quite regularly.
void RainSound::UpdateDrips(float dt, float level) {
    float target = std::clamp(rainRate_ / 8.0f, 0.0f, 1.0f);
    gutter_ += (target - gutter_) * std::min(1.0f, dt / (target > gutter_ ? 20.0f : 45.0f));
    for (DripSource& s : drips_) {
        s.next -= dt;
        if (s.next > 0) continue;
        if (gutter_ < 0.03f || level <= 0) {
            s.next = Rand(0.5f, 1.5f);
            continue;
        }
        s.next = s.interval / std::pow(gutter_, 0.8f) * Rand(0.8f, 1.25f);
        float amp = s.amp * level * (0.6f + 0.4f * gutter_) * kDripGain;
        // The drop hitting the puddle: a soft plop...
        if (grains_.size() < 128) {
            Grain g;
            g.bp.Bandpass(Rand(500.0f, 900.0f), 1.2f);
            g.env = amp * 0.5f;
            g.decay = std::exp(-1.0f / (0.003f * kRate));
            Pan(s.pan, g.gainL, g.gainR);
            g.send = 0.35f;
            grains_.push_back(g);
        }
        // ...and usually a bubble: a plink, or a bloop from a big drop.
        if (Rand() < 0.75f) StartBubble(3260.0f / s.radiusMm * Rand(0.94f, 1.06f), amp, s.pan, 0.35f);
    }
}

// A car swishing past on the wet street. Its sound grows and fades as 1 /
// distance as it comes past along the road, and pans across with it.
void RainSound::StartCar() {
    Car c;
    float speed = Rand(8.0f, 16.0f), distance = Rand(8.0f, 30.0f);  // m/s, m
    c.pass = distance / speed;  // seconds from closest to 45 degrees off
    c.length = 9.0f * c.pass + 2.0f;
    c.closest = c.length * 0.5f;
    c.dir = Rand() < 0.5f ? -1.0f : 1.0f;
    c.amp = 8.0f / distance * kCarGain;
    float tyres = Rand(800.0f, 1500.0f);
    for (int ch = 0; ch < 2; ++ch) {
        c.hiss[ch].Bandpass(tyres, 0.6f);
        c.bright[ch].Set(5000);
        c.rumble[ch].Set(110);
    }
    cars_.push_back(c);
}

void RainSound::Reverb::Init(float t60) {
    const int lengths[4] = {1601, 1999, 2377, 2843};  // 33 to 59 ms, no common factors
    for (int k = 0; k < 4; ++k) {
        line[k].assign(static_cast<size_t>(lengths[k]), 0.0f);
        feedback[k] = std::pow(10.0f, -3.0f * lengths[k] / (t60 * kRate));
        damp[k].Set(3000);
    }
}

void RainSound::Reverb::Run(float in, float& l, float& r) {
    float o[4];
    for (int k = 0; k < 4; ++k) o[k] = line[k][pos[k]];
    float h[4] = {0.5f * (o[0] + o[1] + o[2] + o[3]), 0.5f * (o[0] - o[1] + o[2] - o[3]),
                  0.5f * (o[0] + o[1] - o[2] - o[3]), 0.5f * (o[0] - o[1] - o[2] + o[3])};
    for (int k = 0; k < 4; ++k) {
        line[k][pos[k]] = damp[k].Run(h[k] * feedback[k]) + in * ((k & 1) ? -0.5f : 0.5f);
        if (++pos[k] >= line[k].size()) pos[k] = 0;
    }
    l = 0.5f * (o[0] + o[2]);
    r = 0.5f * (o[1] + o[3]);
}

// ---------------------------------------------------------------- thunder

// Thunder is the sound of the whole channel, kilometres long, so it arrives
// from each part at a different time: the nearest part first as a sharp
// crack (if it is close), then a long train of N-shaped shock waves from the
// rest, softened by distance, over a deep boom and rumble. Air absorbs high
// frequencies, so the further away, the darker and more drawn out it is.
// (After Farnell, Designing Sound, and Few, Acoustic radiations from lightning.)
void RainSound::StartThunder(ThunderVoice& v) {
    float distance = v.distance;
    float distant = std::clamp(distance / 12.0f, 0.0f, 1.0f);
    float duration = 3.0f + 6.0f * Rand() + 4.0f * distant;
    v.length = static_cast<int>((duration * 1.6f + 1.5f) * kRate);
    v.crack = distance < 2.5f ? 1.0f - distance / 2.5f : 0.0f;
    v.crackDecay = std::exp(-1.0f / (0.12f * kRate));
    v.crackBp.Bandpass(1400, 0.8f);
    v.rumbleAttack = 0.15f + 0.6f * distant;
    v.rumbleDecay = duration * 0.55f;
    v.boomDecay = duration * 0.8f;

    int count = 60 + static_cast<int>(Rand() * 80);
    for (int i = 0; i < count; ++i) {
        float t = duration * std::pow(Rand(), 1.7f);  // bunched toward the start
        float amp = std::exp(-t / (duration * 0.45f)) * (0.3f + 0.7f * Rand()) * (Rand() < 0.1f ? 2.5f : 1.0f);
        float lengthMs = 2.0f + 30.0f * distant + 8.0f * Rand();
        v.waves.push_back({static_cast<int>(t * kRate), std::max(static_cast<int>(lengthMs * 0.001f * kRate), 4), amp});
    }
    std::sort(v.waves.begin(), v.waves.end(), [](const NWave& a, const NWave& b) { return a.at < b.at; });

    v.rumbleLp[0].Set(90 + 120 * (1 - distant));
    v.rumbleLp[1].Set(90 + 120 * (1 - distant));
    for (OnePole& p : v.boomLp) p.Set(60);
    float cutoff = (1.0f - distant) * 4500.0f + 150.0f;
    v.distLp[0].Set(cutoff);
    v.distLp[1].Set(cutoff);
    v.echo.assign(static_cast<size_t>(Rand(0.25f, 0.6f) * kRate), 0.0f);
    v.echoGain = 0.35f;
    v.echoLp.Set(900);
}

float RainSound::RunThunder(ThunderVoice& v, float& right, float& rattle) {
    rattle = 0;
    if (v.wait > 0) {
        --v.wait;
        right = 0;
        return 0;
    }
    float t = static_cast<float>(v.t) / kRate;
    float x = 0;

    if (v.crack > 0) {
        v.crack *= v.crackDecay;
        x += v.crack * v.crackBp.Run(Noise()) * 1.6f;
    }
    // N-waves: each a sharp rise then a straight fall, +1 to -1.
    while (v.nextWave < v.waves.size() && v.waves[v.nextWave].at <= v.t) {
        v.active.push_back({v.t, v.waves[v.nextWave]});
        ++v.nextWave;
    }
    for (size_t i = 0; i < v.active.size();) {
        int s = v.t - v.active[i].first;
        const NWave& w = v.active[i].second;
        if (s >= w.length) {
            v.active[i] = v.active.back();
            v.active.pop_back();
            continue;
        }
        x += w.amp * 0.75f * (1.0f - 2.0f * static_cast<float>(s) / w.length);
        ++i;
    }
    // Rumble: noise held for random stretches and smoothed, which gives the
    // irregular rolling of many overlapping arrivals.
    if (--v.holdLeft <= 0) {
        v.held = Noise();
        v.holdLeft = static_cast<int>(kRate * Rand(0.008f, 0.05f));
    }
    float rumble = v.rumbleLp[1].Run(v.rumbleLp[0].Run(v.held));
    float rumbleEnv = (1.0f - std::exp(-t / v.rumbleAttack)) * std::exp(-t / v.rumbleDecay);
    x += rumble * rumbleEnv * 2.4f;
    // A deep boom, felt as much as heard, swelling over the first second.
    float boom = v.boomLp[2].Run(v.boomLp[1].Run(v.boomLp[0].Run(Noise())));
    float boomEnv = (1.0f - std::exp(-t / 0.8f)) * std::exp(-t / v.boomDecay);
    x += boom * boomEnv * 14.0f;
    // A close strike rattles the window. The pressure of the boom shakes the
    // pane against its frame, so it chatters in bursts while the boom and
    // rumble are loudest: a train of sharp knocks that ring the glass.
    if (v.rattle > 0) {
        float drive = v.rattle * boomEnv * rumbleEnv * (std::fabs(v.held) > 0.4f ? 1.0f : 0.08f);
        if (Rand() < drive * 500.0f / kRate) rattle = v.rattle * boomEnv * Rand(0.4f, 1.0f);
    }

    x = v.distLp[1].Run(v.distLp[0].Run(x));
    float echoed = v.echoLp.Run(v.echo[v.echoPos]);
    v.echo[v.echoPos] = x;
    v.echoPos = (v.echoPos + 1) % v.echo.size();
    x += echoed * v.echoGain;

    float gain = v.power * 0.9f / (1.0f + v.distance / 3.0f);
    ++v.t;
    right = x * gain * v.gainR;
    return x * gain * v.gainL;
}

// ---------------------------------------------------------------- render

void RainSound::Render(float* out, int frames) {
    float volume, glass, outside, thunder, rate, wind, daylight, windLevel, dripLevel, carLevel, window;
    std::vector<PendingTap> taps;
    {
        std::lock_guard lock(lock_);
        volume = volume_;
        glass = glassLevel_;
        outside = outsideLevel_;
        thunder = thunderLevel_;
        rate = rainRate_;
        wind = wind_;
        daylight = daylight_;
        windLevel = windLevel_;
        dripLevel = dripLevel_;
        carLevel = carLevel_;
        window = std::clamp(window_, 0.0f, 1.0f);
        taps.swap(taps_);
        for (ThunderVoice& v : pendingThunder_) {
            thunder_.push_back(std::move(v));
            StartThunder(thunder_.back());
        }
        pendingThunder_.clear();
    }
    for (const PendingTap& tp : taps) StartTap(tp, frames);

    const float dt = static_cast<float>(frames) / kRate;
    // Rain outside: a wash of very distant rain whose loudness goes as the
    // square root of the rate (many independent impacts add in power), and
    // on top the separate impacts nearer by, a few hundred a second in
    // moderate rain, more in a downpour.
    float bedTarget = outside > 0 && rate > 0 ? outside * kBedGain * std::sqrt(rate / 6.0f) : 0.0f;
    float bedSmooth = 1.0f / (1.5f * kRate);
    float grainRate = outside > 0 && rate > 0 ? 120.0f * std::pow(rate, 0.8f) : 0.0f;
    grainLevel_ += (outside - grainLevel_) * std::min(1.0f, dt / 0.5f);
    // A downpour drums on the roofs: a low roar.
    float roar = std::clamp((rate - 8.0f) / 20.0f, 0.0f, 1.0f);

    // The gutter, and the street: wet while it rains, drying slowly after.
    UpdateDrips(dt, dripLevel);
    roadWet_ += ((rate > 0.2f ? 1.0f : 0.0f) - roadWet_) * std::min(1.0f, dt / (rate > 0.2f ? 60.0f : 900.0f));
    if (carLevel > 0) {
        untilCar_ -= dt;
        if (untilCar_ <= 0) {
            // About one a minute by day, a few an hour late at night.
            float mean = (35.0f + 120.0f * (1.0f - daylight)) / carLevel;
            untilCar_ = Interval(1.0f / mean);
            if (cars_.size() < 2) StartCar();
        }
    }

    // Wind: louder and brighter the harder it blows, so the gusts are heard
    // swelling and dying. In a gale, it whistles through a gap in the frame.
    float windSmooth = std::min(1.0f, dt / 0.3f);
    windNow_ += (std::fabs(wind) - windNow_) * windSmooth;
    float speed = windNow_;
    float windAmp = windLevel * std::pow(std::clamp((speed - 0.25f) / 2.75f, 0.0f, 1.2f), 1.5f) * kWindGain;
    for (int ch = 0; ch < 2; ++ch) {
        windLp_[ch].Lowpass(160.0f + 260.0f * speed, 0.8f);
        windBp_[ch].Bandpass(300.0f + 140.0f * speed, 2.0f);
    }
    whistle_.Bandpass(520.0f + 160.0f * (speed - 1.6f), 30.0f);
    float whistleAmp = windLevel * std::pow(std::clamp(speed - 1.7f, 0.0f, 1.3f), 2.0f) * kWindGain * 0.6f;

    float volSmooth = 1.0f / (0.05f * kRate);
    float windowSmooth = 1.0f / (0.3f * kRate);
    for (int i = 0; i < frames; ++i) {
        volNow_ += (volume - volNow_) * volSmooth;
        glassNow_ += (glass - glassNow_) * volSmooth;
        thunderNow_ += (thunder - thunderNow_) * volSmooth;
        windowNow_ += (window - windowNow_) * windowSmooth;
        bedNow_ += (bedTarget - bedNow_) * bedSmooth;

        // Thunder, and the window rattling in it.
        float thunderL = 0, thunderR = 0;
        for (size_t k = 0; k < thunder_.size();) {
            ThunderVoice& v = thunder_[k];
            float tr, knock;
            float tl = RunThunder(v, tr, knock);
            thunderL += tl * thunderNow_;
            thunderR += tr * thunderNow_;
            if (knock > 0) StartKnock(knock * thunderNow_ * 8.0f);
            if (v.wait <= 0 && v.t >= v.length) {
                thunder_.erase(thunder_.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
            ++k;
        }

        // Glass: each tap's contact click and spray go through a high-pass
        // (the crisp tick), its ring and body through a gentler one.
        float tickL = 0, tickR = 0, bodyL = 0, bodyR = 0;
        for (size_t k = 0; k < voices_.size();) {
            TapVoice& v = voices_[k];
            if (v.wait > 0) {
                --v.wait;
                ++k;
                continue;
            }
            float e = v.t < v.contact ? std::sin(kPi * (v.t + 0.5f) / v.contact) * v.force : 0.0f;
            float ring = v.ringLp.Run(Noise() * v.ring);
            v.ring *= v.ringDecay;
            float spray = Noise() * v.spray;
            v.spray *= v.sprayDecay;
            float tick = e * 0.7f + spray;
            float body = ring + v.body[0].Run(e) + v.body[1].Run(e) + e * 0.3f;
            tickL += tick * v.gainL;
            tickR += tick * v.gainR;
            bodyL += body * v.gainL;
            bodyR += body * v.gainR;
            if (++v.t >= v.life) {
                v = voices_.back();
                voices_.pop_back();
                continue;
            }
            ++k;
        }
        for (size_t k = 0; k < ticks_.size();) {
            Tick& t = ticks_[k];
            if (t.wait > 0) {
                --t.wait;
                ++k;
                continue;
            }
            float e = std::sin(kPi * (t.pos + 0.5f) / t.length) * t.amp;
            tickL += e * t.gainL;
            tickR += e * t.gainR;
            if (++t.pos >= t.length) {
                t = ticks_.back();
                ticks_.pop_back();
                continue;
            }
            ++k;
        }
        float glassL = tickHp_[0].Run(tickL) + bodyHp_[0].Run(bodyL);
        float glassR = tickHp_[1].Run(tickR) + bodyHp_[1].Run(bodyR);

        // Outside. New impacts arrive as a Poisson stream: at random moments,
        // never on a beat.
        untilGrain_ -= 1.0f / kRate;
        while (untilGrain_ <= 0) {
            StartGrain();
            untilGrain_ += Interval(grainRate);
        }
        float l = 0, r = 0, send = 0;
        for (size_t k = 0; k < grains_.size();) {
            Grain& g = grains_[k];
            float x = g.bp.Run(Noise() * g.env);
            l += x * g.gainL;
            r += x * g.gainR;
            send += x * g.send;
            g.env *= g.decay;
            if (g.env < 1e-5f) {
                g = grains_.back();
                grains_.pop_back();
                continue;
            }
            ++k;
        }
        for (size_t k = 0; k < bubbles_.size();) {
            Bubble& b = bubbles_[k];
            b.phase += 2.0f * kPi * b.freq / kRate;
            if (b.phase > 2.0f * kPi) b.phase -= 2.0f * kPi;
            b.freq += b.rise;
            float x = std::sin(b.phase) * b.amp;
            l += x * b.gainL;
            r += x * b.gainR;
            send += x * b.send;
            b.amp *= b.decay;
            if (b.amp < 1e-5f) {
                b = bubbles_.back();
                bubbles_.pop_back();
                continue;
            }
            ++k;
        }
        // The wash, breathing a little as sheets of rain come and go.
        for (int ch = 0; ch < 2; ++ch) {
            float n = Noise();
            float p = pink_[ch].Run(n);
            float wash = bedLp_[ch].Run(bedHp_[ch].Run(p)) + roar * 2.0f * roarLp_[ch].Run(p);
            float breathe = 1.0f + std::clamp(20.0f * texture_[ch].Run(Noise()), -0.4f, 0.4f) + 0.06f * speed;
            (ch ? r : l) += wash * bedNow_ * breathe;
        }
        // Cars on the wet street.
        for (size_t k = 0; k < cars_.size();) {
            Car& c = cars_[k];
            float x = (c.t - c.closest) / c.pass, dist = std::sqrt(1.0f + x * x);
            float a = c.amp * carLevel / dist * (0.3f + 0.7f * roadWet_);
            float cl, cr;
            Pan(c.dir * x / dist * 0.85f, cl, cr);
            // Coming closer it sounds brighter; going away, duller: the
            // tyres throw their spray backwards, and the Doppler shift.
            if ((i & 255) == 0)
                for (OnePole& b : c.bright) b.Set(3500.0f - 1500.0f * std::tanh(x));
            for (int ch = 0; ch < 2; ++ch) {
                float n = Noise();
                float s = c.bright[ch].Run(c.hiss[ch].Run(n)) + 2.5f * c.rumble[ch].Run(Noise());
                (ch ? r : l) += s * a * (ch ? cr : cl);
                send += s * a * 0.1f;
            }
            c.t += 1.0f / kRate;
            if (c.t >= c.length) {
                c = cars_.back();
                cars_.pop_back();
                continue;
            }
            ++k;
        }
        // Wind.
        if (windAmp > 0) {
            for (int ch = 0; ch < 2; ++ch) {
                float p = windPink_[ch].Run(Noise());
                (ch ? r : l) += (windLp_[ch].Run(p) + 0.6f * windBp_[ch].Run(p)) * windAmp * 4.0f;
            }
        }
        // Echoes off the street.
        float revL, revR;
        reverb_.Run(send, revL, revR);
        l += revL * 0.5f;
        r += revR * 0.5f;

        // Heard through the glass unless the window is open.
        float shutL = glassPeak_[0].Run(glassLp_[0].Run(l)) * kShutGain;
        float shutR = glassPeak_[1].Run(glassLp_[1].Run(r)) * kShutGain;
        l = shutL + (l - shutL) * windowNow_;
        r = shutR + (r - shutR) * windowNow_;
        // The whistle is at the frame itself.
        if (whistleAmp > 0) {
            float w = whistle_.Run(Noise()) * whistleAmp * 6.0f;
            l += w * 0.8f;
            r += w * 0.6f;
        }

        l += glassL * glassNow_ + thunderL;
        r += glassR * glassNow_ + thunderR;

        // A soft limiter keeps a loud clap from clipping.
        out[2 * i] = std::tanh(l * volNow_);
        out[2 * i + 1] = std::tanh(r * volNow_);
    }
}

// ---------------------------------------------------------------- output

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

void SoundOutput::Start(RainSound* sound) {
    sound_ = sound;
    running_ = true;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(
        nullptr, 0,
        [](void* self) -> DWORD {
            static_cast<SoundOutput*>(self)->Run();
            return 0;
        },
        this, 0, nullptr);
}

void SoundOutput::Stop() {
    if (!thread_) return;
    running_ = false;
    SetEvent(static_cast<HANDLE>(wake_));
    WaitForSingleObject(static_cast<HANDLE>(thread_), 3000);
    CloseHandle(static_cast<HANDLE>(thread_));
    CloseHandle(static_cast<HANDLE>(wake_));
    thread_ = wake_ = nullptr;
}

// Four buffers of about 21 ms keep the latency under 100 ms, so a tap is
// heard close enough to when its drop lands.
void SoundOutput::Run() {
    constexpr int kFrames = 1024, kBuffers = 4;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = RainSound::kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = RainSound::kRate * 4;

    HWAVEOUT device = nullptr;
    WAVEHDR headers[kBuffers]{};
    std::vector<int16_t> data[kBuffers];
    std::vector<float> mix(kFrames * 2);
    auto fill = [&](WAVEHDR& h, std::vector<int16_t>& d) {
        sound_->Render(mix.data(), kFrames);
        for (int i = 0; i < kFrames * 2; ++i)
            d[i] = static_cast<int16_t>(std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));
        waveOutWrite(device, &h, sizeof(h));
    };
    auto close = [&] {
        waveOutReset(device);
        for (WAVEHDR& h : headers) waveOutUnprepareHeader(device, &h, sizeof(h));
        waveOutClose(device);
        device = nullptr;
    };

    DWORD retryAt = 0;
    while (running_) {
        bool wanted = sound_->Volume() > 0;
        if (wanted && !device && GetTickCount() >= retryAt) {
            if (waveOutOpen(&device, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(wake_), 0, CALLBACK_EVENT) !=
                MMSYSERR_NOERROR) {
                device = nullptr;
                retryAt = GetTickCount() + 3000;  // no audio device right now; try again later
            } else {
                for (int b = 0; b < kBuffers; ++b) {
                    data[b].assign(kFrames * 2, 0);
                    headers[b] = {};
                    headers[b].lpData = reinterpret_cast<LPSTR>(data[b].data());
                    headers[b].dwBufferLength = kFrames * 4;
                    waveOutPrepareHeader(device, &headers[b], sizeof(WAVEHDR));
                    fill(headers[b], data[b]);
                }
            }
        }
        if (!wanted && device) close();
        if (!device) {
            WaitForSingleObject(static_cast<HANDLE>(wake_), 100);
            continue;
        }
        WaitForSingleObject(static_cast<HANDLE>(wake_), 100);
        for (int b = 0; b < kBuffers; ++b)
            if (headers[b].dwFlags & WHDR_DONE) fill(headers[b], data[b]);
    }
    if (device) close();
}
#endif
