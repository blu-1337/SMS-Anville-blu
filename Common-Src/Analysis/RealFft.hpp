// Radix-2 real FFT (magnitudes only) using the N/2-point complex trick.
// Tables are built once in the constructor; magnitudes() does not allocate,
// but it uses internal scratch, so one instance must not be shared by threads.
#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace sms::analysis {

class RealFft {
public:
    // n must be a power of two >= 4; other values are rounded up to one.
    explicit RealFft(std::size_t n);

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    // windowed.size() >= size(); outBins.size() >= size() / 2 + 1.
    // Short spans are ignored (outBins is left untouched).
    void magnitudes(std::span<const float> windowed, std::span<float> outBins) noexcept;

private:
    std::size_t n_;
    std::vector<std::complex<float>> twiddle_;  // e^{-2 pi i k / (n/2)}, k < n/4... see .cpp
    std::vector<std::complex<float>> split_;    // e^{-2 pi i k / n}, k <= n/2
    std::vector<std::size_t> bitReverse_;
    std::vector<std::complex<float>> work_;
};

} // namespace sms::analysis
