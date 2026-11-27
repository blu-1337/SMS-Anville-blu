#pragma once

#include "VintageSettings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sms::dsp {

/** Building blocks, public for tests. Constants: Docs/AI/VINTAGE-FX.md. */
namespace vintage_detail {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kFlush = 1.0e-20f;

inline void flush(float& state) noexcept
{
    if (std::abs(state) < kFlush)
        state = 0.0f;
}

/** Clamp a cutoff into [20 Hz, 0.45 fs]. */
[[nodiscard]] inline double safeCutoff(const double hz, const double fs) noexcept
{
    return std::clamp(hz, 20.0, 0.45 * fs);
}

/** Mid-tread quantiser with scale S = 2^(bits-1): silence stays silent, clips at the converter. */
[[nodiscard]] inline float quantize(const float x, const float scale, const float inverse) noexcept
{
    return std::clamp(std::floor(x * scale + 0.5f), -scale, scale - 1.0f) * inverse;
}

/** Odd soft clipper f(v); exact sign(v) beyond |v| >= 3. */
[[nodiscard]] inline float softClip(const float v) noexcept
{
    if (v >= 3.0f) return 1.0f;
    if (v <= -3.0f) return -1.0f;
    const float square = v * v;
    return v * (27.0f + square) / (27.0f + 9.0f * square);
}

/** Level-compensated drive: a -12 dBFS peak keeps its level; exactly linear at 0 dB. */
struct Drive {
    float gain = 1.0f;
    float norm = 1.0f;
    float amount = 0.0f;

    void set(const float decibels) noexcept
    {
        gain = std::pow(10.0f, decibels / 20.0f);
        norm = 0.25f / softClip(0.25f * gain);
        amount = std::min(decibels / 3.0f, 1.0f);
    }
    [[nodiscard]] float process(const float x) const noexcept
    {
        if (amount <= 0.0f)
            return x;
        return x + amount * (softClip(gain * x) * norm - x);
    }
};

/** Trapezoidal state-variable low-pass (stable under modulation), stereo. */
struct Svf {
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1[2]{}, ic2[2]{};

    void set(const double hz, const double q, const double fs) noexcept
    {
        const double g = std::tan(kPi * safeCutoff(hz, fs) / fs);
        const double first = 1.0 / (1.0 + g * (g + 1.0 / q));
        a1 = static_cast<float>(first);
        a2 = static_cast<float>(g * first);
        a3 = static_cast<float>(g * g * first);
    }
    [[nodiscard]] float process(const float x, const int channel) noexcept
    {
        const float v3 = x - ic2[channel];
        const float v1 = a1 * ic1[channel] + a2 * v3;
        const float v2 = ic2[channel] + a2 * ic1[channel] + a3 * v3;
        ic1[channel] = 2.0f * v1 - ic1[channel];
        ic2[channel] = 2.0f * v2 - ic2[channel];
        return v2;
    }
    void clear() noexcept { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }
    void flushStates() noexcept
    {
        for (int c = 0; c < 2; ++c) { flush(ic1[c]); flush(ic2[c]); }
    }
};

/** Trapezoidal one-pole low-pass, stereo. */
struct OnePole {
    float coefficient = 1.0f;
    float state[2]{};

    void set(const double hz, const double fs) noexcept
    {
        const double g = std::tan(kPi * safeCutoff(hz, fs) / fs);
        coefficient = static_cast<float>(g / (1.0 + g));
    }
    [[nodiscard]] float process(const float x, const int channel) noexcept
    {
        const float v = (x - state[channel]) * coefficient;
        const float y = v + state[channel];
        state[channel] = y + v;
        return y;
    }
    void clear() noexcept { state[0] = state[1] = 0.0f; }
    void flushStates() noexcept { flush(state[0]); flush(state[1]); }
};

inline constexpr double kButterworthQ[3]{0.5176380902050415, 0.7071067811865476,
                                          1.9318516525781366};
/** Resonance raises the last stage's Q to this at 1 and trims the level (peak about +5 dB). */
inline constexpr double kMaximumResonanceQ = 8.0;

struct DrumFixedVoicing { double pole, svf1, q1, svf2, q2; };
/** Dark, Mid, Open, Bright: one-pole + two SVFs (5th order) from the output schematics. */
inline constexpr DrumFixedVoicing kDrumVoicings[4]{
    {7410.0, 8320.0, 0.616, 8120.0, 1.58},
    {8820.0, 10130.0, 0.619, 9870.0, 1.58},
    {11640.0, 11830.0, 0.609, 11700.0, 1.58},
    {15280.0, 12340.0, 0.639, 12900.0, 1.58},
};
/** Sweep: four one-poles at 1000 * 18^env Hz; env = 1 on a hit, decays with 70 ms. */
inline constexpr double kSweepRestHz = 1000.0;
inline constexpr double kSweepOpenRatio = 18.0;
inline constexpr double kSweepSeconds = 0.07;

/** A fixed topology chosen by configure*, then only its coefficients change. */
struct ModelFilter {
    OnePole poles[4];
    Svf svfs[3];
    int poleCount = 0;
    int svfCount = 0;
    float gain = 1.0f;

    void bypass() noexcept { poleCount = svfCount = 0; gain = 1.0f; }

    /** 6th-order Butterworth (MF6 style); resonance 0 is the authentic non-resonant response. */
    void butterworth6(const double hz, const double fs, const double resonance = 0.0) noexcept
    {
        poleCount = 0;
        svfCount = 3;
        const double lastQ = kButterworthQ[2] + resonance * (kMaximumResonanceQ - kButterworthQ[2]);
        svfs[0].set(hz, kButterworthQ[0], fs);
        svfs[1].set(hz, kButterworthQ[1], fs);
        svfs[2].set(hz, lastQ, fs);
        gain = static_cast<float>(std::pow(lastQ / kButterworthQ[2], -0.35));
    }

    void drum(const VintageVoicing voicing, const double octaves, const double fs,
              const double sweepEnvelope) noexcept
    {
        const double scale = std::exp2(octaves);
        gain = 1.0f;
        if (voicing == VintageVoicing::raw) {
            poleCount = svfCount = 0;
        } else if (voicing == VintageVoicing::sweep) {
            poleCount = 4;
            svfCount = 0;
            const double hz = kSweepRestHz * std::pow(kSweepOpenRatio, sweepEnvelope) * scale;
            for (auto& pole : poles)
                pole.set(hz, fs);
        } else {
            const auto& v = kDrumVoicings[static_cast<int>(voicing) - 1];
            poleCount = 1;
            svfCount = 2;
            poles[0].set(v.pole * scale, fs);
            svfs[0].set(v.svf1 * scale, v.q1, fs);
            svfs[1].set(v.svf2 * scale, v.q2, fs);
        }
    }

    [[nodiscard]] float process(float x, const int channel) noexcept
    {
        for (int i = 0; i < poleCount; ++i)
            x = poles[i].process(x, channel);
        for (int i = 0; i < svfCount; ++i)
            x = svfs[i].process(x, channel);
        return x * gain;
    }
    void clear() noexcept
    {
        for (auto& pole : poles) pole.clear();
        for (auto& svf : svfs) svf.clear();
    }
    void flushStates() noexcept
    {
        for (int i = 0; i < poleCount; ++i) poles[i].flushStates();
        for (int i = 0; i < svfCount; ++i) svfs[i].flushStates();
    }
};

/** Linear ramp in caller-defined steps; snaps exactly onto the target. */
struct Ramp {
    float value = 0.0f;
    float target = 0.0f;
    float step = 0.0f;
    int left = 0;

    void to(const float next, const int steps) noexcept
    {
        target = next;
        if (std::abs(next - value) <= 1.0e-4f * std::max(1.0f, std::abs(next)) || steps <= 1) {
            snap();
            return;
        }
        left = steps;
        step = (next - value) / static_cast<float>(steps);
    }
    void snap() noexcept { value = target; left = 0; }
    bool advance() noexcept
    {
        if (left == 0)
            return false;
        value += step;
        if (--left == 0)
            value = target;
        return true;
    }
};

} // namespace vintage_detail

/**
 * Vintage 12-bit colour, one stereo frame at a time. One instance per voice
 * (pad) and one per output bus. prepare() is control-thread only; everything
 * else is real-time safe: no allocation, locks, I/O or exceptions, all state
 * in fixed members. Chain: drive -> [bus: anti-alias pre-filter] -> mid-tread
 * quantiser -> sample-and-hold at rateHz with 2-point polyBLEP (wet is one
 * sample late, dry is delayed to match) -> model filter -> dry/wet mix.
 * Parameter changes ramp over 20 ms (rate, cutoff and tone in the log domain,
 * coefficients every 16 samples); a model, voicing or Off/On change
 * crossfades old and new chains over 10 ms. Off (or Mix 0) is an exact
 * bypass once that crossfade ends.
 */
class VintageProcessor {
public:
    /** bus = true adds the pre-filter and silence detection (idle buses skip all work). */
    void prepare(const double hostRate, const bool bus = false) noexcept
    {
        fs_ = std::isfinite(hostRate) && hostRate > 1.0 ? hostRate : 48000.0;
        bus_ = bus;
        fadeStep_ = static_cast<float>(1.0 / std::max(1.0, 0.01 * fs_));
        rampTicks_ = std::max(1, static_cast<int>(std::lround(0.02 * fs_ / kControlInterval)));
        rampSamples_ = std::max(1, static_cast<int>(std::lround(0.02 * fs_)));
        sweepDecay_ = static_cast<float>(std::exp(-kControlInterval / (vintage_detail::kSweepSeconds * fs_)));
        idleFrames_ = static_cast<std::uint32_t>(std::max(1.0, 0.1 * fs_));
        setTarget(target_, true);
    }

    /** Audio thread; cheap when unchanged. immediate = jump without ramps or crossfade. */
    void setTarget(const VintageSettings& requested, const bool immediate = false) noexcept
    {
        const auto settings = sanitize(requested);
        if (!immediate && settings == target_)
            return;
        target_ = settings;
        targetKey_ = keyOf(settings);
        bits_.to(settings.bits, rampTicks_);
        rateLog_.to(std::log2(settings.rateHz), rampTicks_);
        drive_.to(settings.driveDb, rampTicks_);
        tone_.to(settings.toneOctaves, rampTicks_);
        resonance_.to(settings.resonance, rampTicks_);
        mix_.to(settings.mix, rampSamples_);
        if (immediate || idle_ || bypassed_)
            snapRamps();
        if (immediate || idle_)
            jumpToTarget();
        bypassed_ = lanes_[current_].key == 0 && !fading_ && targetKey_ == 0;
        dirty_ = true;
        if (!bypassed_)
            control();
    }

    /** Pad: a new hit opens the Sweep filter. The envelope restarts; filter states do not. */
    void trigger() noexcept
    {
        sweep_ = 1.0f;
        dirty_ = true;
    }

    /** Clear all audio state and land on the target (no tail, no fade). */
    void reset() noexcept
    {
        sweep_ = 0.0f;
        snapRamps();
        jumpToTarget();
        bypassed_ = targetKey_ == 0;
        dirty_ = true;
        control();
    }

    [[nodiscard]] bool bypassed() const noexcept { return bypassed_; }
    [[nodiscard]] bool idle() const noexcept { return idle_; }
    [[nodiscard]] const VintageSettings& target() const noexcept { return target_; }

    void process(float& left, float& right) noexcept
    {
        if (bypassed_)
            return;
        if (bus_) {
            if (left == 0.0f && right == 0.0f) {
                if (idle_)
                    return; // silent in, silent out
                ++silentFrames_;
            } else {
                silentFrames_ = 0U;
                idle_ = false;
            }
        }
        if (--countdown_ == 0) {
            countdown_ = kControlInterval;
            control();
        }
        mix_.advance();
        const float in[2]{left, right};
        float out[2];
        run(lanes_[current_], in, out);
        if (fading_) {
            float old[2];
            run(lanes_[1 - current_], in, old);
            out[0] = old[0] + fade_ * (out[0] - old[0]);
            out[1] = old[1] + fade_ * (out[1] - old[1]);
            fade_ += fadeStep_;
            if (fade_ >= 1.0f) {
                fading_ = false;
                bypassed_ = lanes_[current_].key == 0 && targetKey_ == 0;
            }
        }
        left = out[0];
        right = out[1];
        if (bus_ && silentFrames_ >= idleFrames_ &&
            std::abs(out[0]) < 1.0e-6f && std::abs(out[1]) < 1.0e-6f) {
            // Tail is below -120 dB: drop it and skip work until audio returns.
            idle_ = true;
            snapRamps();
            jumpToTarget();
            bypassed_ = targetKey_ == 0;
            left = right = 0.0f;
        }
    }

private:
    static constexpr int kControlInterval = 16;
    static constexpr int kRackKey = 7; // keys: 0 Off, 1..6 drum voicing + 1, 7 rack

    struct Lane {
        int key = 0;
        vintage_detail::ModelFilter pre;  // bus only
        vintage_detail::ModelFilter post;
        float previous[2]{};  // x[n-1] for the edge interpolation
        float held[2]{};
        float pending[2]{};   // wet sample n-1, still open for its BLEP correction
        float dry[2]{};       // dry delayed one sample to match the wet path
        double phase = 0.0;

        void clear() noexcept
        {
            pre.clear();
            post.clear();
            previous[0] = previous[1] = held[0] = held[1] = 0.0f;
            pending[0] = pending[1] = dry[0] = dry[1] = 0.0f;
            phase = 0.0;
        }
    };

    [[nodiscard]] static int keyOf(const VintageSettings& settings) noexcept
    {
        if (!vintageAudible(settings))
            return 0;
        return vintageModel(settings) == VintageModel::rack12 ? kRackKey
                                                              : 1 + static_cast<int>(settings.voicing);
    }

    void snapRamps() noexcept
    {
        bits_.snap();
        rateLog_.snap();
        drive_.snap();
        tone_.snap();
        resonance_.snap();
        mix_.snap();
    }

    void jumpToTarget() noexcept
    {
        fading_ = false;
        lanes_[0].clear();
        lanes_[1].clear();
        lanes_[current_].key = targetKey_;
        silentFrames_ = 0U;
        dirty_ = true;
    }

    /** Control rate: ramps, sweep, pending model changes, coefficients, denormal flush. */
    void control() noexcept
    {
        bool moved = bits_.advance();
        moved = rateLog_.advance() || moved;
        moved = drive_.advance() || moved;
        moved = tone_.advance() || moved;
        moved = resonance_.advance() || moved;
        if (sweep_ > 0.0f) {
            sweep_ *= sweepDecay_;
            if (sweep_ < 1.0e-4f)
                sweep_ = 0.0f;
            moved = true;
        }
        if (!fading_ && targetKey_ != lanes_[current_].key) {
            // Further changes wait until this crossfade ends.
            current_ = 1 - current_;
            lanes_[current_].clear();
            lanes_[current_].key = targetKey_;
            fading_ = true;
            fade_ = 0.0f;
            bypassed_ = false;
            moved = true;
        }
        if (moved || dirty_) {
            dirty_ = false;
            scale_ = std::exp2(bits_.value - 1.0f);
            inverse_ = 1.0f / scale_;
            driveStage_.set(drive_.value);
            rateHz_ = std::exp2(static_cast<double>(rateLog_.value));
            increment_ = rateHz_ / fs_;
            configure(lanes_[current_]);
            if (fading_)
                configure(lanes_[1 - current_]);
        }
        for (auto& lane : lanes_) {
            lane.pre.flushStates();
            lane.post.flushStates();
            for (int c = 0; c < 2; ++c) {
                vintage_detail::flush(lane.pending[c]);
                vintage_detail::flush(lane.held[c]);
                vintage_detail::flush(lane.previous[c]);
                vintage_detail::flush(lane.dry[c]);
            }
        }
    }

    void configure(Lane& lane) noexcept
    {
        const double tone = static_cast<double>(tone_.value);
        if (lane.key == 0) {
            lane.pre.bypass();
            lane.post.bypass();
        } else if (lane.key == kRackKey) {
            // Rate = 2.5 x bandwidth; the pre-filter keeps the Butterworth without tone or resonance.
            if (bus_) lane.pre.butterworth6(rateHz_ / 2.5, fs_);
            else lane.pre.bypass();
            lane.post.butterworth6(rateHz_ / 2.5 * std::exp2(tone), fs_,
                                   static_cast<double>(resonance_.value));
        } else {
            if (bus_) lane.pre.butterworth6(0.42 * rateHz_, fs_);
            else lane.pre.bypass();
            lane.post.drum(static_cast<VintageVoicing>(lane.key - 1), tone, fs_,
                           static_cast<double>(sweep_));
        }
    }

    void run(Lane& lane, const float (&in)[2], float (&out)[2]) noexcept
    {
        if (lane.key == 0) {
            out[0] = in[0];
            out[1] = in[1];
            return;
        }
        float x[2];
        for (int c = 0; c < 2; ++c)
            x[c] = lane.pre.process(driveStage_.process(in[c]), c);
        float wet[2];
        if (increment_ >= 1.0) {
            // Converter at or above the host rate: quantise only (same one-sample latency).
            for (int c = 0; c < 2; ++c) {
                wet[c] = lane.pending[c];
                lane.pending[c] = vintage_detail::quantize(x[c], scale_, inverse_);
            }
        } else {
            lane.phase += increment_;
            if (lane.phase >= 1.0) {
                lane.phase -= 1.0;
                // The edge fell d samples before n: sample it there, then band-limit the step.
                const float d = static_cast<float>(std::min(lane.phase / increment_, 1.0));
                const float before = d * d * 0.5f;
                const float after = (1.0f - d) * (1.0f - d) * 0.5f;
                for (int c = 0; c < 2; ++c) {
                    const float sampled = vintage_detail::quantize(
                        x[c] + (lane.previous[c] - x[c]) * d, scale_, inverse_);
                    const float step = sampled - lane.held[c];
                    lane.held[c] = sampled;
                    wet[c] = lane.pending[c] + step * before;
                    lane.pending[c] = sampled - step * after;
                }
            } else {
                for (int c = 0; c < 2; ++c) {
                    wet[c] = lane.pending[c];
                    lane.pending[c] = lane.held[c];
                }
            }
        }
        const float mix = mix_.value;
        for (int c = 0; c < 2; ++c) {
            lane.previous[c] = x[c];
            const float filtered = lane.post.process(wet[c], c);
            out[c] = lane.dry[c] + mix * (filtered - lane.dry[c]);
            lane.dry[c] = in[c];
        }
    }

    double fs_ = 48000.0;
    bool bus_ = false;
    VintageSettings target_{};
    int targetKey_ = 0;
    Lane lanes_[2];
    int current_ = 0;
    bool fading_ = false;
    bool bypassed_ = true;
    bool idle_ = false;
    bool dirty_ = true;
    float fade_ = 0.0f;
    float fadeStep_ = 1.0f / 480.0f;
    int countdown_ = kControlInterval;
    int rampTicks_ = 60;
    int rampSamples_ = 960;
    vintage_detail::Ramp bits_{12.0f, 12.0f};
    vintage_detail::Ramp rateLog_{14.6686f, 14.6686f};
    vintage_detail::Ramp drive_{};
    vintage_detail::Ramp tone_{};
    vintage_detail::Ramp resonance_{};
    vintage_detail::Ramp mix_{1.0f, 1.0f};
    vintage_detail::Drive driveStage_{};
    float scale_ = 2048.0f;
    float inverse_ = 1.0f / 2048.0f;
    double rateHz_ = kVintageDrumRateHz;
    double increment_ = 0.5;
    float sweep_ = 0.0f;
    float sweepDecay_ = 0.995f;
    std::uint32_t silentFrames_ = 0U;
    std::uint32_t idleFrames_ = 4800U;
};

} // namespace sms::dsp
