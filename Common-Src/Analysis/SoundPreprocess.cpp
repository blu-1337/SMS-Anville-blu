#include "Analysis/SoundPreprocess.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sms::analysis {

namespace {
constexpr double kTargetRate = 22050.0;
constexpr double kSilencePeak = 0.001;  // -60 dBFS

float finiteOrZero(const float v) noexcept
{
    return std::isfinite(v) ? v : 0.0F;
}

std::vector<double> lowpassKernel(const std::size_t factor)
{
    std::size_t taps = 63 * factor;
    taps |= 1U;  // odd, symmetric, integer group delay
    const double cutoff = 0.45 / static_cast<double>(factor);  // cycles per input sample
    const double centre = static_cast<double>(taps - 1) / 2.0;
    std::vector<double> h(taps);
    double sum = 0.0;
    for (std::size_t i = 0; i < taps; ++i) {
        const double t = static_cast<double>(i) - centre;
        const double sinc = t == 0.0 ? 2.0 * cutoff
                                     : std::sin(2.0 * std::numbers::pi * cutoff * t) / (std::numbers::pi * t);
        const double x = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(taps - 1);
        const double w = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
        h[i] = sinc * w;
        sum += h[i];
    }
    for (double& v : h) {
        v /= sum;
    }
    return h;
}
} // namespace

std::vector<float> monoDecimated(const std::span<const float> interleavedStereo, const double rate,
                                 double& outRate, bool& silent)
{
    outRate = rate;
    silent = true;
    const std::size_t frames = interleavedStereo.size() / 2;
    if (!(rate >= kAnalysisMinRate && rate <= kAnalysisMaxRate) || frames == 0) {
        return {};
    }
    double energyL = 0.0;
    double energyR = 0.0;
    double energyMix = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double l = finiteOrZero(interleavedStereo[2 * i]);
        const double r = finiteOrZero(interleavedStereo[2 * i + 1]);
        energyL += l * l;
        energyR += r * r;
        energyMix += 0.25 * (l + r) * (l + r);
    }
    // Weights for L and R: plain mix, or the louder channel when anti-phase content cancels.
    double wl = 0.5;
    double wr = 0.5;
    if (energyMix < 0.25 * 0.5 * (energyL + energyR)) {
        wl = energyL >= energyR ? 1.0 : 0.0;
        wr = 1.0 - wl;
    }
    std::vector<double> mono(frames);
    double mean = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        mono[i] = wl * finiteOrZero(interleavedStereo[2 * i]) + wr * finiteOrZero(interleavedStereo[2 * i + 1]);
        mean += mono[i];
    }
    mean /= static_cast<double>(frames);
    double peak = 0.0;
    for (double& v : mono) {
        v -= mean;
        peak = std::max(peak, std::abs(v));
    }
    if (!(peak >= kSilencePeak)) {
        return {};
    }
    silent = false;
    const std::size_t factor = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(rate / kTargetRate)));
    outRate = rate / static_cast<double>(factor);
    if (factor == 1) {
        std::vector<float> out(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            out[i] = static_cast<float>(mono[i]);
        }
        return out;
    }
    const std::vector<double> h = lowpassKernel(factor);
    const std::ptrdiff_t half = static_cast<std::ptrdiff_t>(h.size() / 2);
    const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(frames);
    const std::size_t outFrames = (frames + factor - 1) / factor;
    std::vector<float> out(outFrames);
    for (std::size_t m = 0; m < outFrames; ++m) {
        const std::ptrdiff_t centre = static_cast<std::ptrdiff_t>(m * factor);
        const std::ptrdiff_t first = std::max<std::ptrdiff_t>(0, centre - half);
        const std::ptrdiff_t last = std::min<std::ptrdiff_t>(count - 1, centre + half);
        double acc = 0.0;
        for (std::ptrdiff_t j = first; j <= last; ++j) {
            acc += h[static_cast<std::size_t>(j - centre + half)] * mono[static_cast<std::size_t>(j)];
        }
        out[m] = static_cast<float>(acc);
    }
    return out;
}

} // namespace sms::analysis
