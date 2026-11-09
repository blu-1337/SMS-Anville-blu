#include "Analysis/RealFft.hpp"

#include <cmath>
#include <numbers>
#include <utility>

namespace sms::analysis {

namespace {
std::size_t roundUpPowerOfTwo(std::size_t n)
{
    std::size_t p = 4;
    while (p < n) {
        p <<= 1U;
    }
    return p;
}
} // namespace

RealFft::RealFft(const std::size_t n)
    : n_(roundUpPowerOfTwo(n))
{
    const std::size_t m = n_ / 2;
    twiddle_.resize(m / 2);
    for (std::size_t k = 0; k < twiddle_.size(); ++k) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(m);
        twiddle_[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    split_.resize(m + 1);
    for (std::size_t k = 0; k <= m; ++k) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n_);
        split_[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    bitReverse_.resize(m);
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < m) {
        ++bits;
    }
    for (std::size_t i = 0; i < m; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            if ((i >> b) & 1U) {
                r |= std::size_t{1} << (bits - 1 - b);
            }
        }
        bitReverse_[i] = r;
    }
    work_.resize(m);
}

void RealFft::magnitudes(const std::span<const float> windowed, const std::span<float> outBins) noexcept
{
    const std::size_t m = n_ / 2;
    if (windowed.size() < n_ || outBins.size() < m + 1) {
        return;
    }
    // Pack even/odd samples into one m-point complex sequence, bit-reversed.
    for (std::size_t i = 0; i < m; ++i) {
        const std::size_t j = bitReverse_[i];
        work_[i] = {windowed[2 * j], windowed[2 * j + 1]};
    }
    for (std::size_t len = 2; len <= m; len <<= 1U) {
        const std::size_t half = len / 2;
        const std::size_t step = m / len;
        for (std::size_t base = 0; base < m; base += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<float> w = twiddle_[k * step];
                const std::complex<float> a = work_[base + k];
                const std::complex<float> b = work_[base + k + half] * w;
                work_[base + k] = a + b;
                work_[base + k + half] = a - b;
            }
        }
    }
    // Split into the spectrum of the real input: X[k] = E[k] + W^k O[k].
    const std::complex<float> iHalf{0.0F, -0.5F};  // 1 / (2i)
    for (std::size_t k = 0; k <= m; ++k) {
        const std::complex<float> zk = work_[k % m];
        const std::complex<float> zc = std::conj(work_[(m - k) % m]);
        const std::complex<float> even = (zk + zc) * 0.5F;
        const std::complex<float> odd = (zk - zc) * iHalf;
        outBins[k] = std::abs(even + split_[k] * odd);
    }
}

} // namespace sms::analysis
