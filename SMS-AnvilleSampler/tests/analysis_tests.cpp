// sms_analysis: FFT, PRNG, onsets, tempo and slice strategies.
#include "Analysis/RealFft.hpp"
#include "Analysis/SplitMix64.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <vector>

namespace {
using namespace sms::analysis;

void check(const bool value, const char* const message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void fftMatchesNaiveDft()
{
    for (std::size_t n = 8; n <= 1024; n <<= 1U) {
        SplitMix64 rng(n);
        std::vector<float> x(n);
        for (float& v : x) {
            v = static_cast<float>(rng.nextUnit() * 2.0 - 1.0);
        }
        RealFft fft(n);
        std::vector<float> bins(n / 2 + 1);
        fft.magnitudes(x, bins);
        double worst = 0.0;
        for (std::size_t k = 0; k <= n / 2; ++k) {
            double re = 0.0;
            double im = 0.0;
            for (std::size_t t = 0; t < n; ++t) {
                const double a = -2.0 * std::numbers::pi * static_cast<double>(k * t % n) / static_cast<double>(n);
                re += static_cast<double>(x[t]) * std::cos(a);
                im += static_cast<double>(x[t]) * std::sin(a);
            }
            worst = std::max(worst, std::abs(std::hypot(re, im) - static_cast<double>(bins[k])));
        }
        check(worst < 1e-4, "FFT magnitudes match a naive DFT");
    }
}

void fftIgnoresShortSpans()
{
    RealFft fft(16);
    std::vector<float> in(8, 1.0F);
    std::vector<float> out(9, -1.0F);
    fft.magnitudes(in, out);
    check(out[0] == -1.0F, "short input leaves output untouched");
    check(fft.size() == 16, "size");
    check(RealFft(10).size() == 16, "size rounds up to a power of two");
}

void splitMixIsDeterministic()
{
    SplitMix64 a(42);
    SplitMix64 b(42);
    SplitMix64 c(43);
    check(SplitMix64(0).next() == 0xE220A8397B1DCDAFULL, "known SplitMix64 vector for seed 0");
    bool differs = false;
    for (int i = 0; i < 16; ++i) {
        const auto va = a.next();
        check(va == b.next(), "same seed, same stream");
        differs = differs || va != c.next();
    }
    check(differs, "different seed, different stream");
    SplitMix64 d(7);
    for (int i = 0; i < 1000; ++i) {
        const double u = d.nextUnit();
        check(u >= 0.0 && u < 1.0, "nextUnit range");
        check(d.nextBelow(5) < 5, "nextBelow range");
    }
    check(d.nextBelow(0) == 0, "nextBelow(0)");
}
} // namespace

int main()
{
    fftMatchesNaiveDft();
    fftIgnoresShortSpans();
    splitMixIsDeterministic();
    return EXIT_SUCCESS;
}
