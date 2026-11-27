#include "Analysis/Chroma.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace sms::analysis {

namespace {
constexpr std::size_t kFrameSize = 8192;
constexpr std::size_t kHop = 4096;
constexpr std::size_t kMaxFrames = 2000;
constexpr double kLowHz = 55.0;
constexpr double kHighHz = 5000.0;
constexpr double kKernelWidth = 4.0 / 3.0;  // semitones (L)
constexpr float kNoisyFlatness = 0.3F;
constexpr double kOnsetSkipSeconds = 0.050;

std::vector<double> onsetTimes(const std::span<const float> mono, const double fs)
{
    const std::size_t block = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(0.01 * fs)));
    const std::size_t blocks = mono.size() / block;
    std::vector<double> rms(blocks, 0.0);
    double maxRms = 0.0;
    for (std::size_t b = 0; b < blocks; ++b) {
        double sum = 0.0;
        for (std::size_t i = b * block; i < (b + 1) * block; ++i) {
            sum += static_cast<double>(mono[i]) * static_cast<double>(mono[i]);
        }
        rms[b] = std::sqrt(sum / static_cast<double>(block));
        maxRms = std::max(maxRms, rms[b]);
    }
    std::vector<double> times;
    for (std::size_t b = 0; b < blocks; ++b) {
        const double before = std::max(b >= 1 ? rms[b - 1] : 0.0, b >= 2 ? rms[b - 2] : 0.0);
        if (rms[b] > 2.0 * before && rms[b] > 0.01 * maxRms) {
            times.push_back(static_cast<double>(b * block) / fs);
        }
    }
    return times;
}

double peakPitch(const SpectralPeak& peak, const float tuningCents)
{
    return hzToMidi(peak.hz) - static_cast<double>(tuningCents) / 100.0;
}

std::size_t clampLast(const std::span<const ChromaFrame> frames, const std::size_t last)
{
    return std::min(last, frames.size());
}
} // namespace

ChromaFrames chromaFrames(const std::span<const float> mono, const double fs, const std::atomic<bool>* cancel)
{
    ChromaFrames out;
    if (mono.empty() || !(fs > 0.0)) {
        return out;
    }
    const std::size_t length = std::min(mono.size(), kFrameSize);
    std::size_t count = mono.size() > kFrameSize ? (mono.size() - kFrameSize) / kHop + 1 : 1;
    std::size_t hop = kHop;
    if (count > kMaxFrames) {
        hop = (mono.size() - kFrameSize) / (kMaxFrames - 1);
        count = kMaxFrames;
    }
    const std::vector<double> onsets = onsetTimes(mono, fs);
    SpectrumAnalyzer analyzer(kFrameSize, length, SpectrumWindow::hann);
    const double highHz = std::min(kHighHz, 0.45 * fs);
    out.frames.resize(count);
    bool anyUsed = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            out.cancelled = true;
            out.frames.clear();
            return out;
        }
        ChromaFrame& frame = out.frames[i];
        const std::size_t start = i * hop;
        const std::span<const float> samples = mono.subspan(start, length);
        double sum = 0.0;
        for (const float v : samples) {
            sum += static_cast<double>(v) * static_cast<double>(v);
        }
        frame.seconds = static_cast<double>(start) / fs;
        frame.rms = static_cast<float>(std::sqrt(sum / static_cast<double>(length)));
        analyzer.analyse(samples, fs);
        frame.flatness = analyzer.flatness(40.0, highHz);
        frame.peaks = analyzer.peaks(kLowHz, highHz, 50.0, 60);
        bool afterOnset = false;
        for (const double t : onsets) {
            afterOnset = afterOnset || (frame.seconds >= t && frame.seconds < t + kOnsetSkipSeconds);
        }
        frame.used = frame.flatness <= kNoisyFlatness && !afterOnset && !frame.peaks.empty();
        anyUsed = anyUsed || frame.used;
    }
    if (!anyUsed) {
        // Short clips: every frame touches an onset; keep the tonal ones.
        for (ChromaFrame& frame : out.frames) {
            frame.used = frame.flatness <= kNoisyFlatness && !frame.peaks.empty();
        }
    }
    return out;
}

float tuningOffset(const std::span<const ChromaFrame> frames)
{
    std::complex<double> z{0.0, 0.0};
    double total = 0.0;
    for (const ChromaFrame& frame : frames) {
        if (!frame.used) {
            continue;
        }
        for (const SpectralPeak& peak : frame.peaks) {
            const double x = 12.0 * std::log2(peak.hz / 440.0);
            z += peak.amplitude * std::polar(1.0, 2.0 * std::numbers::pi * x);
            total += peak.amplitude;
        }
    }
    if (!(total > 0.0) || std::abs(z) / total < 0.4) {
        return 0.0F;
    }
    return static_cast<float>(100.0 * std::arg(z) / (2.0 * std::numbers::pi));
}

Chroma chroma(const std::span<const ChromaFrame> frames, const float tuningCents, const std::size_t first,
              const std::size_t last)
{
    Chroma sum{};
    const std::size_t end = clampLast(frames, last);
    for (std::size_t i = first; i < end; ++i) {
        const ChromaFrame& frame = frames[i];
        if (!frame.used) {
            continue;
        }
        std::array<double, 12> bins{};
        for (const SpectralPeak& peak : frame.peaks) {
            const double pitch = peakPitch(peak, tuningCents);
            for (int pc = 0; pc < 12; ++pc) {
                const double offset = pitch - static_cast<double>(pc);
                const double d = offset - 12.0 * std::round(offset / 12.0);
                if (std::abs(d) < 0.5 * kKernelWidth) {
                    const double c = std::cos(0.5 * std::numbers::pi * d / (0.5 * kKernelWidth));
                    bins[static_cast<std::size_t>(pc)] += peak.amplitude * c * c;
                }
            }
        }
        const double maxBin = *std::max_element(bins.begin(), bins.end());
        if (!(maxBin > 0.0)) {
            continue;
        }
        for (std::size_t pc = 0; pc < 12; ++pc) {
            sum[pc] += static_cast<float>(frame.rms * bins[pc] / maxBin);
        }
    }
    const float maxSum = *std::max_element(sum.begin(), sum.end());
    if (maxSum > 0.0F) {
        for (float& v : sum) {
            v /= maxSum;
        }
    }
    return sum;
}

int bassPitchClass(const std::span<const ChromaFrame> frames, const float tuningCents, const std::size_t first,
                   const std::size_t last)
{
    std::array<double, 12> votes{};
    bool any = false;
    const std::size_t end = clampLast(frames, last);
    for (std::size_t i = first; i < end; ++i) {
        const ChromaFrame& frame = frames[i];
        if (!frame.used || frame.peaks.empty()) {
            continue;
        }
        double maxDb = -300.0;
        for (const SpectralPeak& peak : frame.peaks) {
            maxDb = std::max(maxDb, peak.db);
        }
        for (const SpectralPeak& peak : frame.peaks) {  // sorted by frequency
            if (peak.db >= maxDb - 20.0) {
                const long note = std::lround(peakPitch(peak, tuningCents));
                votes[static_cast<std::size_t>(((note % 12) + 12) % 12)] += frame.rms + 1e-12;
                any = true;
                break;
            }
        }
    }
    if (!any) {
        return -1;
    }
    return static_cast<int>(std::max_element(votes.begin(), votes.end()) - votes.begin());
}

} // namespace sms::analysis
