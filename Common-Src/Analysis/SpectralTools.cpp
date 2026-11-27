#include "Analysis/SpectralTools.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sms::analysis {

std::size_t nextPowerOfTwo(const std::size_t n) noexcept
{
    std::size_t p = 1;
    while (p < n) {
        p <<= 1U;
    }
    return p;
}

double centsBetween(const double from, const double to) noexcept
{
    return 1200.0 * std::log2(to / from);
}

double hzToMidi(const double hz) noexcept
{
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

ComplexFft::ComplexFft(const std::size_t n)
    : n_(std::max<std::size_t>(2, nextPowerOfTwo(n)))
{
    twiddle_.resize(n_ / 2);
    for (std::size_t k = 0; k < twiddle_.size(); ++k) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n_);
        twiddle_[k] = {std::cos(a), std::sin(a)};
    }
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < n_) {
        ++bits;
    }
    bitReverse_.resize(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            if ((i >> b) & 1U) {
                r |= std::size_t{1} << (bits - 1 - b);
            }
        }
        bitReverse_[i] = r;
    }
}

void ComplexFft::forward(const std::span<std::complex<double>> data) const noexcept
{
    transform(data, false);
}

void ComplexFft::inverse(const std::span<std::complex<double>> data) const noexcept
{
    transform(data, true);
    if (data.size() >= n_) {
        const double scale = 1.0 / static_cast<double>(n_);
        for (std::size_t i = 0; i < n_; ++i) {
            data[i] *= scale;
        }
    }
}

void ComplexFft::transform(const std::span<std::complex<double>> data, const bool inverse) const noexcept
{
    if (data.size() < n_) {
        return;
    }
    for (std::size_t i = 0; i < n_; ++i) {
        const std::size_t j = bitReverse_[i];
        if (j > i) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t len = 2; len <= n_; len <<= 1U) {
        const std::size_t half = len / 2;
        const std::size_t step = n_ / len;
        for (std::size_t base = 0; base < n_; base += len) {
            for (std::size_t k = 0; k < half; ++k) {
                std::complex<double> w = twiddle_[k * step];
                if (inverse) {
                    w = std::conj(w);
                }
                const std::complex<double> a = data[base + k];
                const std::complex<double> b = data[base + k + half] * w;
                data[base + k] = a + b;
                data[base + k + half] = a - b;
            }
        }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer(const std::size_t fftSize, const std::size_t windowLength,
                                   const SpectrumWindow window)
    : fft_(fftSize)
{
    const std::size_t length = std::clamp<std::size_t>(windowLength, 1, fft_.size());
    window_.resize(length);
    double sum = 0.0;
    const double denom = static_cast<double>(length);
    for (std::size_t i = 0; i < length; ++i) {
        // Periodic windows centred on the frame; a one-sample frame is rectangular.
        const double x = 2.0 * std::numbers::pi * (static_cast<double>(i) + 0.5) / denom;
        double w = 1.0;
        if (length > 1) {
            if (window == SpectrumWindow::hann) {
                w = 0.5 - 0.5 * std::cos(x);
            } else {
                w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
            }
        }
        window_[i] = static_cast<float>(w);
        sum += w;
    }
    scale_ = static_cast<float>(sum > 0.0 ? 2.0 / sum : 1.0);
    buffer_.assign(fft_.size(), 0.0F);
    magnitudes_.assign(fft_.size() / 2 + 1, 0.0F);
}

void SpectrumAnalyzer::analyse(const std::span<const float> samples, const double rate)
{
    binHz_ = rate / static_cast<double>(fft_.size());
    const std::size_t n = std::min(samples.size(), window_.size());
    std::fill(buffer_.begin(), buffer_.end(), 0.0F);
    for (std::size_t i = 0; i < n; ++i) {
        buffer_[i] = samples[i] * window_[i];
    }
    fft_.magnitudes(buffer_, magnitudes_);
    for (float& m : magnitudes_) {
        m *= scale_;
    }
}

std::size_t SpectrumAnalyzer::binAt(const double hz) const noexcept
{
    if (!(binHz_ > 0.0) || !(hz > 0.0)) {
        return 0;
    }
    const double bin = std::round(hz / binHz_);
    return static_cast<std::size_t>(std::min(bin, static_cast<double>(magnitudes_.size() - 1)));
}

std::vector<SpectralPeak> SpectrumAnalyzer::peaks(const double minHz, const double maxHz, const double rangeDb,
                                                  const std::size_t maxPeaks) const
{
    std::vector<SpectralPeak> out;
    const std::size_t lo = std::max<std::size_t>(1, binAt(minHz));
    const std::size_t hi = std::min(binAt(maxHz), magnitudes_.size() - 2);
    if (hi <= lo || maxPeaks == 0) {
        return out;
    }
    float strongest = 0.0F;
    for (std::size_t k = lo; k <= hi; ++k) {
        strongest = std::max(strongest, magnitudes_[k]);
    }
    if (!(strongest > 0.0F)) {
        return out;
    }
    const auto toDb = [](const float m) { return 20.0 * std::log10(static_cast<double>(m) + 1e-12); };
    const double floorDb = toDb(strongest) - rangeDb;
    for (std::size_t k = lo; k <= hi; ++k) {
        const float m = magnitudes_[k];
        if (!(m > magnitudes_[k - 1]) || m < magnitudes_[k + 1]) {
            continue;
        }
        const double b = toDb(m);
        if (b < floorDb) {
            continue;
        }
        const double a = toDb(magnitudes_[k - 1]);
        const double c = toDb(magnitudes_[k + 1]);
        const double denom = a - 2.0 * b + c;
        const double p = denom < 0.0 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
        SpectralPeak peak;
        peak.hz = (static_cast<double>(k) + p) * binHz_;
        peak.db = b - 0.25 * (a - c) * p;
        peak.amplitude = std::pow(10.0, peak.db / 20.0);
        out.push_back(peak);
    }
    if (out.size() > maxPeaks) {
        std::nth_element(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(maxPeaks), out.end(),
                         [](const SpectralPeak& x, const SpectralPeak& y) { return x.db > y.db; });
        out.resize(maxPeaks);
    }
    std::sort(out.begin(), out.end(), [](const SpectralPeak& x, const SpectralPeak& y) { return x.hz < y.hz; });
    return out;
}

float SpectrumAnalyzer::flatness(const double minHz, const double maxHz) const
{
    const std::size_t lo = std::max<std::size_t>(1, binAt(minHz));
    const std::size_t hi = binAt(maxHz);
    if (hi <= lo) {
        return 1.0F;
    }
    double logSum = 0.0;
    double sum = 0.0;
    for (std::size_t k = lo; k <= hi; ++k) {
        const double power = static_cast<double>(magnitudes_[k]) * static_cast<double>(magnitudes_[k]);
        logSum += std::log(power + 1e-24);
        sum += power;
    }
    const double count = static_cast<double>(hi - lo + 1);
    const double arithmetic = sum / count;
    if (!(arithmetic > 1e-20)) {
        return 1.0F;
    }
    return static_cast<float>(std::clamp(std::exp(logSum / count) / arithmetic, 0.0, 1.0));
}

double SpectrumAnalyzer::medianDb(const double minHz, const double maxHz) const
{
    const std::size_t lo = std::max<std::size_t>(1, binAt(minHz));
    const std::size_t hi = binAt(maxHz);
    if (hi <= lo) {
        return -240.0;
    }
    std::vector<float> values(magnitudes_.begin() + static_cast<std::ptrdiff_t>(lo),
                              magnitudes_.begin() + static_cast<std::ptrdiff_t>(hi) + 1);
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return 20.0 * std::log10(static_cast<double>(*mid) + 1e-12);
}

} // namespace sms::analysis
