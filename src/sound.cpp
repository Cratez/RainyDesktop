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

// The pane's lowest bending modes as you hear them through a tap: a window
// pane in a frame rings briefly in the low hundreds of hertz and up, with the
// high modes dying fastest. Frequency (Hz), T60 (s), relative level.
struct Mode {
    float freq, t60, gain;
};
constexpr Mode kGlassModes[7] = {
    {290, 0.10f, 0.5f},  {690, 0.075f, 0.8f}, {1250, 0.055f, 1.0f}, {2050, 0.04f, 1.4f},
    {3150, 0.03f, 1.8f}, {4700, 0.02f, 1.8f}, {6900, 0.014f, 1.4f},
};

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

// ---------------------------------------------------------------- controls

RainSound::RainSound() {
    for (int ch = 0; ch < 2; ++ch) {
        // The right channel's modes are a touch higher, as if heard from a
        // slightly different spot, which widens the sound.
        float detune = ch ? 1.03f : 1.0f;
        for (int k = 0; k < 7; ++k)
            modes_[ch][k].Set(kGlassModes[k].freq * detune, kGlassModes[k].t60, kGlassModes[k].gain);
        bedHp_[ch].Highpass(350, 0.7f);
        bedLp_[ch].Lowpass(6500, 0.6f);
        bodyLp_[ch].Lowpass(700, 0.7f);
        tickHp_[ch].Highpass(2500, 0.7f);
    }
    swell_.Set(0.4f);
    seed_ ^= 0x9E3779B9u;
}

void RainSound::SetLevels(float volume, float glass, float outside, float thunder) {
    std::lock_guard lock(lock_);
    volume_ = volume;
    glassLevel_ = glass;
    outsideLevel_ = outside;
    thunderLevel_ = thunder;
}

void RainSound::SetWeather(float rainRate, float wind) {
    std::lock_guard lock(lock_);
    rainRate_ = rainRate;
    wind_ = wind;
}

float RainSound::Volume() const {
    std::lock_guard lock(lock_);
    return volume_;
}

void RainSound::Tap(float pan, float radiusMm, float glance, bool splash) {
    std::lock_guard lock(lock_);
    if (volume_ <= 0 || glassLevel_ <= 0 || taps_.size() > 256) return;
    taps_.push_back({pan, radiusMm, glance, splash});
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
    float volume, glass, outside, thunder, rate, wind;
    std::vector<PendingTap> taps;
    {
        std::lock_guard lock(lock_);
        volume = volume_;
        glass = glassLevel_;
        outside = outsideLevel_;
        thunder = thunderLevel_;
        rate = rainRate_;
        wind = wind_;
        taps.swap(taps_);
        for (ThunderVoice& v : pendingThunder_) {
            thunder_.push_back(std::move(v));
            StartThunder(thunder_.back());
        }
        pendingThunder_.clear();
    }

    // Each tap starts somewhere in this buffer: the frame they came from is
    // about as long as the buffer, and spreading them avoids a machine-gun beat.
    for (const PendingTap& tp : taps) {
        Pulse p;
        p.at = static_cast<int>(Rand() * frames);
        // Contact time of the impact: bigger drops press longer, so they sound lower.
        p.length = std::max(2, static_cast<int>((0.00012f + 0.00025f * tp.radius) * kRate));
        float amp = std::min(std::pow(tp.radius / 1.2f, 1.5f), 4.0f) * (0.6f + 0.6f * tp.glance) *
                    (tp.splash ? 1.5f : 1.0f) * 0.5f;
        Pan(tp.pan, p.gainL, p.gainR);
        p.gainL *= amp;
        p.gainR *= amp;
        // The drop also shatters into spray, a brief burst of hiss that
        // gives the tap its crisp edge; splashes hiss longer.
        p.spray = 0.6f * (0.5f + tp.glance);
        p.sprayDecay = std::exp(-1.0f / ((tp.splash ? 0.004f : 0.0012f) * kRate));
        pulses_.push_back(p);
    }

    // Outside, the rain is a hiss whose loudness goes as the square root of
    // the rate (many independent impacts add in power), with a patter of
    // small impacts on top, a few hundred a second in moderate rain.
    float bedTarget = outside > 0 && rate > 0 ? outside * 0.05f * std::sqrt(rate / 6.0f) : 0.0f;
    float bedSmooth = 1.0f / (1.5f * kRate);
    plinkRate_ = outside > 0 ? 500.0f * rate / 6.0f : 0.0f;
    float volSmooth = 1.0f / (0.05f * kRate);

    for (int i = 0; i < frames; ++i) {
        volNow_ += (volume - volNow_) * volSmooth;
        glassNow_ += (glass - glassNow_) * volSmooth;
        thunderNow_ += (thunder - thunderNow_) * volSmooth;
        bedNow_ += (bedTarget - bedNow_) * bedSmooth;

        // Thunder, and the window rattling in it.
        float thunderL = 0, thunderR = 0;
        for (size_t k = 0; k < thunder_.size();) {
            ThunderVoice& v = thunder_[k];
            float tr, knock;
            float tl = RunThunder(v, tr, knock);
            thunderL += tl * thunderNow_;
            thunderR += tr * thunderNow_;
            if (knock > 0 && pulses_.size() < 512) {
                Pulse p;
                p.at = 0;
                p.length = 3 + static_cast<int>(Rand() * 6);  // a hard, brief knock: bright
                float amp = knock * thunderNow_ * 8.0f;
                Pan(Rand(-0.6f, 0.6f), p.gainL, p.gainR);
                p.gainL *= amp;
                p.gainR *= amp;
                p.spray = 0.5f;  // with a dry buzz of glass against the frame
                p.sprayDecay = std::exp(-1.0f / (0.0006f * kRate));
                pulses_.push_back(p);
            }
            if (v.wait <= 0 && v.t >= v.length) {
                thunder_.erase(thunder_.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
            ++k;
        }

        // Glass: excitation from the taps rings the pane's modes.
        float exL = 0, exR = 0, hissL = 0, hissR = 0;
        for (size_t k = 0; k < pulses_.size();) {
            Pulse& p = pulses_[k];
            if (p.at > 0) {
                --p.at;
                ++k;
                continue;
            }
            if (p.pos < p.length) {
                float e = std::sin(kPi * (p.pos + 0.5f) / p.length);  // half-sine contact force
                exL += e * p.gainL;
                exR += e * p.gainR;
            }
            float n = Noise() * p.spray;
            hissL += n * p.gainL;
            hissR += n * p.gainR;
            p.spray *= p.sprayDecay;
            if (++p.pos >= p.length && p.spray < 1e-3f) {
                p = pulses_.back();
                pulses_.pop_back();
                continue;
            }
            ++k;
        }
        // The tick and hiss of the impact itself, then the ring of the pane.
        float glassL = tickHp_[0].Run(exL + hissL), glassR = tickHp_[1].Run(exR + hissR);
        for (int m = 0; m < 7; ++m) {
            glassL += modes_[0][m].Run(exL);
            glassR += modes_[1][m].Run(exR);
        }

        // Outside: hiss and body, swelling a little with the gusts.
        float swell = 1.0f + 40.0f * swell_.Run(Noise()) + 0.08f * std::fabs(wind);
        float nL = Noise(), nR = Noise();
        float bedL = bedLp_[0].Run(bedHp_[0].Run(nL)) + 0.6f * bodyLp_[0].Run(nL);
        float bedR = bedLp_[1].Run(bedHp_[1].Run(nR)) + 0.6f * bodyLp_[1].Run(nR);
        float l = bedL * bedNow_ * swell, r = bedR * bedNow_ * swell;

        plinkAccum_ += plinkRate_ / kRate;
        while (plinkAccum_ >= 1.0f) {
            plinkAccum_ -= 1.0f;
            if (plinks_.size() >= 64) continue;
            // A small drop striking water or a hard surface: a click that
            // rings briefly, rising slightly in pitch like the bubble a drop
            // traps in a puddle.
            Plink pk;
            pk.freq = 1500.0f * std::exp(Rand() * 1.6f);
            float decay = Rand(0.002f, 0.008f);
            pk.decay = std::exp(-1.0f / (decay * kRate));
            pk.rise = pk.freq * Rand(0.0f, 0.4f) / (decay * kRate);
            float a = Rand();
            pk.amp = a * a * 4.0f;
            Pan(Rand(-1.0f, 1.0f), pk.gainL, pk.gainR);
            plinks_.push_back(pk);
        }
        for (size_t k = 0; k < plinks_.size();) {
            Plink& pk = plinks_[k];
            pk.phase += 2.0f * kPi * pk.freq / kRate;
            pk.freq += pk.rise;
            float s = std::sin(pk.phase) * pk.amp * bedNow_;
            l += s * pk.gainL;
            r += s * pk.gainR;
            pk.amp *= pk.decay;
            if (pk.amp < 1e-3f) {
                pk = plinks_.back();
                plinks_.pop_back();
                continue;
            }
            ++k;
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
