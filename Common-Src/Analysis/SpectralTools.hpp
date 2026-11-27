// Spectral helpers shared by pitch, chroma and sound analysis: a double-precision
// complex FFT, windowed amplitude spectra, interpolated peaks and flatness.
// Instances hold scratch, so one instance must not be shared by threads.
#pragma once

#include "Analysis/RealFft.hpp"

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace sms::analysis {

[[nodiscard]] std::size_t nextPowerOfTwo(std::size_t n) noexcept;

// Signed distance in cents from `from` to `to` (both > 0).
[[nodiscard]] double centsBetween(double from, double to) noexcept;

// Fractional MIDI note number, A4 = 440 Hz = 69.
[[nodiscard]] double hzToMidi(double hz) noexcept;

// In-place radix-2 complex FFT in double precision (size rounded up to a power of two >= 2).
class ComplexFft {
public:
    explicit ComplexFft(std::size_t n);
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    // data.size() >= size(); inverse() includes the 1/n scale. Short spans are ignored.
    void forward(std::span<std::complex<double>> data) const noexcept;
    void inverse(std::span<std::complex<double>> data) const noexcept;

private:
    void transform(std::span<std::complex<double>> data, bool inverse) const noexcept;
    std::size_t n_;
    std::vector<std::complex<double>> twiddle_;
    std::vector<std::size_t> bitReverse_;
};

enum class SpectrumWindow { hann, blackmanHarris };

struct SpectralPeak {
    double hz = 0.0;
    double db = -300.0;     // interpolated level, dB re amplitude 1
    double amplitude = 0.0; // linear, a sinusoid of amplitude A reads about A
};

// Windowed magnitude spectrum of up to fftSize samples, zero-padded. Magnitudes
// are scaled so a stationary sinusoid of amplitude A peaks near A.
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(std::size_t fftSize, std::size_t windowLength, SpectrumWindow window);
    // samples.size() == windowLength() (shorter spans are zero-padded at the end).
    void analyse(std::span<const float> samples, double rate);

    [[nodiscard]] std::size_t fftSize() const noexcept { return fft_.size(); }
    [[nodiscard]] std::size_t windowLength() const noexcept { return window_.size(); }
    [[nodiscard]] double binHz() const noexcept { return binHz_; }
    [[nodiscard]] std::span<const float> magnitudes() const noexcept { return magnitudes_; }

    // Local maxima within [minHz, maxHz] that lie less than rangeDb below the
    // strongest bin of that range, refined by parabolic interpolation in dB.
    // At most maxPeaks, strongest kept, returned sorted by frequency.
    [[nodiscard]] std::vector<SpectralPeak> peaks(double minHz, double maxHz, double rangeDb,
                                                  std::size_t maxPeaks) const;
    // Geometric / arithmetic mean of power over [minHz, maxHz]; 1 for an empty range.
    [[nodiscard]] float flatness(double minHz, double maxHz) const;
    // Median bin level in dB over [minHz, maxHz].
    [[nodiscard]] double medianDb(double minHz, double maxHz) const;

private:
    [[nodiscard]] std::size_t binAt(double hz) const noexcept;
    RealFft fft_;
    std::vector<float> window_;
    std::vector<float> buffer_;
    std::vector<float> magnitudes_;
    float scale_ = 1.0F;
    double binHz_ = 0.0;
};

} // namespace sms::analysis
