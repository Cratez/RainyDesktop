// Rain sound, synthesised as it plays (no recordings):
//   - drops tapping on the glass, one tap per raindrop that lands on screen,
//     panned to where it hit: a soft tick and a short, dense ring of the pane,
//     never quite the same twice; splashes crackle as their droplets land again;
//   - rain falling outside: thousands of separate impacts a second on paving,
//     leaves and roofs near and far, each a tiny burst of coloured noise at a
//     random moment, over a soft wash; now and then one lands in a puddle and
//     traps a bubble that rings;
//   - the gutter dripping into a puddle, faster as it fills, slowing for a
//     minute or two after the rain stops;
//   - wind, swelling and dying with the gusts, whistling at the frame in a gale;
//   - now and then a car swishing past on the wet street;
//   - all of that heard through the window glass, which lets the low sounds
//     through and muffles the high ones, unless the window is open;
//   - thunder after each lightning flash: the crack of a close strike, a train
//     of N-shaped shock waves from the different parts of the channel, a deep
//     boom and a long rumble, all darker and later the further away it is.
//     A close one rattles the window: the pane chatters in its frame.
//
// RainSound is the synthesiser and works on any platform (the offline tools
// use it to render WAV previews). SoundOutput plays it on Windows.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

class RainSound {
public:
    static constexpr int kRate = 48000;

    RainSound();

    // Levels, all 1 = default. volume 0 is silent.
    void SetLevels(float volume, float glass, float outside, float thunder);
    // The rest of what you hear outside, each 1 = default, 0 = off: wind,
    // the gutter dripping, cars on the wet street. window: 0 = shut, so all
    // of it comes through the glass, .. 1 = open.
    void SetAmbience(float wind, float drips, float cars, float window);
    // Rain rate in mm/h (0 when it isn't raining), the wind (about -3..3)
    // and daylight (0 = night .. 1 = day; fewer cars at night).
    void SetWeather(float rainRate, float wind, float daylight);
    // A raindrop hit the glass. pan -1..1, radius in mm, glance 0..1.
    void Tap(float pan, float radiusMm, float glance, bool splash, bool burst);
    // Thunder from a flash that has just been seen: it starts after `delay`
    // seconds. rattle 0..1 is how hard it shakes the window.
    void Thunder(float delay, float distanceKm, float power, float pan, float rattle);

    // Fills `frames` stereo frames (interleaved L R) of float samples.
    void Render(float* out, int frames);
    float Volume() const;

private:
    struct Resonator {
        float a1 = 0, a2 = 0, gain = 0, y1 = 0, y2 = 0;
        void Set(float freq, float t60, float g);
        float Run(float x) {
            float y = x * gain + a1 * y1 - a2 * y2;
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    struct OnePole {
        float a = 0, y = 0;
        void Set(float cutoff);
        float Run(float x) { return y += a * (x - y); }
    };
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        void Bandpass(float freq, float q);
        void Highpass(float freq, float q);
        void Lowpass(float freq, float q);
        void Peak(float freq, float q, float gainDb);
        float Run(float x) {
            float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    // Pink noise (equal energy per octave), Paul Kellet's filter.
    struct Pink {
        float b0 = 0, b1 = 0, b2 = 0;
        float Run(float white) {
            b0 = 0.99765f * b0 + white * 0.0990460f;
            b1 = 0.96300f * b1 + white * 0.2965164f;
            b2 = 0.57000f * b2 + white * 1.0526913f;
            return (b0 + b1 + b2 + white * 0.1848f) * 0.25f;
        }
    };
    struct PendingTap {
        float pan, radius, glance;
        bool splash, burst;
    };
    // A drop striking the pane: a half-sine of contact force, then the pane
    // ringing around the strike. Window glass has a hundred bending modes in
    // every kilohertz, so that ring is a short burst of dense, noise-like
    // sound, darker for a bigger drop (a longer contact), with a couple of
    // the nearer modes standing out a little, different at every spot.
    struct TapVoice {
        int wait;     // samples until it lands
        int t = 0;
        int contact;  // samples the drop presses on the glass
        float force;
        float gainL, gainR;
        float ring, ringDecay;
        OnePole ringLp;
        Resonator body[2];
        float spray, sprayDecay;  // the hiss of the drop shattering
        int life;                 // samples until it has died away
    };
    // A tiny click: a splash droplet landing again.
    struct Tick {
        int wait, length, pos = 0;
        float amp, gainL, gainR;
    };
    // One impact outside: a burst of noise, coloured by what it hit (paving
    // ticks, leaves pat, roofs ring a little), quieter and more reverberant
    // the further away.
    struct Grain {
        Biquad bp;
        float env, decay, gainL, gainR, send;
    };
    // A bubble trapped by a drop landing in water rings at its Minnaert
    // frequency, f0 = 3.26 m/s / radius, and rises in pitch as it settles
    // (van den Doel, Physically based models for liquid sounds).
    struct Bubble {
        float phase = 0, freq, rise, amp, decay, gainL, gainR, send;
    };
    struct DripSource {
        float pan, amp, radiusMm, interval, next;
    };
    struct Car {
        float t = 0, length, closest, pass, dir, amp;
        Biquad hiss[2];
        OnePole bright[2], rumble[2];
    };
    // Echoes off the buildings across the street: four delay lines feeding
    // each other through a Hadamard matrix, each losing its highs as it goes
    // (a feedback delay network).
    struct Reverb {
        std::vector<float> line[4];
        size_t pos[4] = {};
        float feedback[4] = {};
        OnePole damp[4];
        void Init(float t60);
        void Run(float in, float& l, float& r);
    };
    struct NWave {
        int at;      // samples after the thunder starts
        int length;  // samples
        float amp;
    };
    struct ThunderVoice {
        int wait;    // samples until it starts
        int t = 0;   // samples since it started
        int length;  // total samples
        float distance, power, gainL, gainR, rattle;
        float crack, crackDecay, rumbleAttack, rumbleDecay, boomDecay;
        std::vector<NWave> waves;
        size_t nextWave = 0;
        std::vector<std::pair<int, NWave>> active;  // waves sounding now: (start sample, wave)
        float held = 0;
        int holdLeft = 0;
        OnePole rumbleLp[2], boomLp[3], distLp[2];
        Biquad crackBp;
        std::vector<float> echo;  // a single reflection off the ground or buildings
        size_t echoPos = 0;
        float echoGain = 0;
        OnePole echoLp;
    };

    float Noise() {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return static_cast<float>(static_cast<int32_t>(seed_)) * (1.0f / 2147483648.0f);
    }
    float Rand(float lo = 0, float hi = 1) { return lo + (hi - lo) * (Noise() * 0.5f + 0.5f); }
    // Seconds until the next event of a Poisson stream at `rate` per second.
    float Interval(float rate);
    void StartTap(const PendingTap& tp, int frames);
    void StartKnock(float amp);
    void StartGrain();
    void StartBubble(float freq, float amp, float pan, float send);
    void StartCar();
    void UpdateDrips(float dt, float level);
    void StartThunder(ThunderVoice& v);
    float RunThunder(ThunderVoice& v, float& right, float& rattle);

    mutable std::mutex lock_;
    // Shared with the caller, under lock_.
    float volume_ = 0, glassLevel_ = 1, outsideLevel_ = 1, thunderLevel_ = 1;
    float windLevel_ = 1, dripLevel_ = 1, carLevel_ = 1, window_ = 0;
    float rainRate_ = 0, wind_ = 0, daylight_ = 1;
    std::vector<PendingTap> taps_;
    std::vector<ThunderVoice> pendingThunder_;

    // Synth state, render thread only.
    uint32_t seed_ = 0x2545F491u;
    float volNow_ = 0, glassNow_ = 1, thunderNow_ = 1, windowNow_ = 0;
    float bedNow_ = 0, grainLevel_ = 0, untilGrain_ = 0;
    float windNow_ = 0;
    float gutter_ = 0, roadWet_ = 0, untilCar_ = 20;
    std::vector<TapVoice> voices_;
    std::vector<Tick> ticks_;
    std::vector<Grain> grains_;
    std::vector<Bubble> bubbles_;
    std::vector<Car> cars_;
    DripSource drips_[3];
    Pink pink_[2], windPink_[2];
    Biquad tickHp_[2], bodyHp_[2];
    Biquad bedHp_[2], bedLp_[2], roarLp_[2], windLp_[2], windBp_[2], whistle_;
    OnePole texture_[2];
    OnePole glassLp_[2];
    Biquad glassPeak_[2];
    Reverb reverb_;
    std::vector<ThunderVoice> thunder_;
};

#ifdef _WIN32
// Plays a RainSound through the default output device with waveOut, on its
// own thread. The device is opened only while the volume is above zero.
class SoundOutput {
public:
    void Start(RainSound* sound);
    void Stop();

private:
    void Run();
    RainSound* sound_ = nullptr;
    void* thread_ = nullptr;
    void* wake_ = nullptr;
    std::atomic<bool> running_{false};
};
#endif
