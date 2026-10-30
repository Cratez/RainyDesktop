// Rain sound, synthesised as it plays (no recordings):
//   - drops tapping on the glass, one tap per raindrop that lands on screen,
//     panned to where it hit, ringing the pane's lowest few vibration modes;
//   - rain falling outside: a bed of filtered noise with a patter of small
//     impacts on top, following the rain rate;
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
    // Rain rate in mm/h (0 when it isn't raining) and the wind (about -3..3).
    void SetWeather(float rainRate, float wind);
    // A raindrop hit the glass. pan -1..1, radius in mm, glance 0..1.
    void Tap(float pan, float radiusMm, float glance, bool splash);
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
        float Run(float x) {
            float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    struct PendingTap {
        float pan, radius, glance;
        bool splash;
    };
    struct Pulse {  // an excitation being fed into the glass
        int at;     // samples until it starts
        int length;
        int pos = 0;
        float gainL, gainR;
        float spray = 0;  // the hiss of the drop shattering, decaying
        float sprayDecay = 0;
    };
    struct Plink {  // one small impact outside: a decaying, slightly rising tone
        float phase = 0, freq = 0, rise = 0, amp = 0, decay = 0, gainL = 0, gainR = 0;
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
    void StartThunder(ThunderVoice& v);
    float RunThunder(ThunderVoice& v, float& right, float& rattle);

    mutable std::mutex lock_;
    // Shared with the caller, under lock_.
    float volume_ = 0, glassLevel_ = 1, outsideLevel_ = 1, thunderLevel_ = 1;
    float rainRate_ = 0, wind_ = 0;
    std::vector<PendingTap> taps_;
    std::vector<ThunderVoice> pendingThunder_;

    // Synth state, render thread only.
    uint32_t seed_ = 0x2545F491u;
    float volNow_ = 0, bedNow_ = 0, glassNow_ = 1, thunderNow_ = 1;
    float plinkRate_ = 0, plinkAccum_ = 0;
    Resonator modes_[2][7];
    Biquad bedHp_[2], bedLp_[2], bodyLp_[2], tickHp_[2];
    OnePole swell_;
    std::vector<Pulse> pulses_;
    std::vector<Plink> plinks_;
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
