#include "Analysis/PitchTracker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <utility>

namespace sms::analysis {

namespace {
constexpr double kMinHz = 25.0;
constexpr double kMaxHz = 4000.0;
constexpr std::size_t kMaxFrames = 64;
constexpr double kHopSeconds = 0.020;
constexpr std::size_t kLongDataMax = 32768;
constexpr std::size_t kLongFftMax = 131072;
constexpr std::size_t kLongFftMin = 8192;
constexpr double kHarmonicDecay = 0.84;
constexpr int kSumHarmonics = 8;
constexpr int kCandidateDivisors = 6;
constexpr double kMatchCents = 30.0;
constexpr float kNoisyFlatness = 0.03F;
// Above this, YIN's integer lags (5..22 at ~22 kHz) favour a multiple of the
// period; an octave/twelfth relation to a clean spectral f0 is then expected.
constexpr double kYinUnreliableHz = 1000.0;

bool cancelled(const std::atomic<bool>* cancel) noexcept
{
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

struct Region {
    std::size_t start = 0;
    std::size_t end = 0;
    [[nodiscard]] std::size_t size() const noexcept { return end > start ? end - start : 0; }
};

// Section 5.1: 10 ms RMS envelope; from the peak + 30 ms to the last point
// above peak - 40 dB. The peak is the first block within 1 dB of the maximum,
// so a steady tone does not start its region at a random late ripple.
Region usableRegion(const std::span<const float> mono, const double fs)
{
    const std::size_t n = mono.size();
    const std::size_t block = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(0.01 * fs)));
    const std::size_t blocks = (n + block - 1) / block;
    std::vector<double> rms(blocks, 0.0);
    double maxRms = 0.0;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t first = b * block;
        const std::size_t last = std::min(n, first + block);
        double sum = 0.0;
        for (std::size_t i = first; i < last; ++i) {
            sum += static_cast<double>(mono[i]) * static_cast<double>(mono[i]);
        }
        rms[b] = std::sqrt(sum / static_cast<double>(last - first));
        maxRms = std::max(maxRms, rms[b]);
    }
    std::size_t peakBlock = 0;
    while (peakBlock + 1 < blocks && rms[peakBlock] < 0.891 * maxRms) {
        ++peakBlock;
    }
    std::size_t lastBlock = blocks - 1;
    while (lastBlock > peakBlock && rms[lastBlock] < 0.01 * maxRms) {
        --lastBlock;
    }
    const std::size_t peak = std::min(n - 1, peakBlock * block + block / 2);
    const std::size_t end = std::min(n, (lastBlock + 1) * block);
    const std::size_t minLength = static_cast<std::size_t>(0.08 * fs);
    for (const double skip : {0.030, 0.005}) {
        const std::size_t start = peak + static_cast<std::size_t>(skip * fs);
        if (end > start && end - start >= minLength) {
            return {start, end};
        }
    }
    return {0, n};
}

// YIN difference function through one packed complex FFT, then the cumulative
// mean normalised d' and a Beta(2,18)-weighted vote over 100 thresholds.
class Yin {
public:
    struct Frame {
        double hz = 0.0;
        double periodicity = 0.0;  // 1 - d'
        double energy = 0.0;
    };

    Yin(const std::size_t window, const std::size_t tauMin, const std::size_t tauMax)
        : window_(window), tauMin_(tauMin), tauMax_(tauMax), fft_(window + tauMax),
          packed_(fft_.size()), product_(fft_.size()), prefix_(window + tauMax + 1),
          d_(tauMax + 1), dn_(tauMax + 1)
    {
        double total = 0.0;
        for (std::size_t k = 0; k < weights_.size(); ++k) {
            const double t = 0.01 * static_cast<double>(k + 1);
            weights_[k] = t * std::pow(1.0 - t, 17.0);
            total += weights_[k];
        }
        for (double& w : weights_) {
            w /= total;
        }
    }

    [[nodiscard]] std::size_t span() const noexcept { return window_ + tauMax_; }

    Frame analyse(const float* const x, const double fs)
    {
        Frame frame;
        const std::size_t span = window_ + tauMax_;
        const std::size_t m = fft_.size();
        prefix_[0] = 0.0;
        for (std::size_t i = 0; i < span; ++i) {
            prefix_[i + 1] = prefix_[i] + static_cast<double>(x[i]) * static_cast<double>(x[i]);
        }
        const double ea = prefix_[window_];
        frame.energy = ea / static_cast<double>(window_);
        if (!(ea > 1e-12)) {
            return frame;
        }
        // a = first `window` samples, b = the whole span; r(tau) = sum a[j] b[j + tau].
        for (std::size_t i = 0; i < m; ++i) {
            const double a = i < window_ ? static_cast<double>(x[i]) : 0.0;
            const double b = i < span ? static_cast<double>(x[i]) : 0.0;
            packed_[i] = {a, b};
        }
        fft_.forward(packed_);
        for (std::size_t k = 0; k < m; ++k) {
            const std::complex<double> zk = packed_[k];
            const std::complex<double> zc = std::conj(packed_[(m - k) % m]);
            const std::complex<double> a = 0.5 * (zk + zc);
            const std::complex<double> b = std::complex<double>{0.0, -0.5} * (zk - zc);
            product_[k] = std::conj(a) * b;
        }
        fft_.inverse(product_);
        d_[0] = 0.0;
        dn_[0] = 1.0;
        double running = 0.0;
        for (std::size_t tau = 1; tau <= tauMax_; ++tau) {
            const double eb = prefix_[tau + window_] - prefix_[tau];
            d_[tau] = std::max(0.0, ea + eb - 2.0 * product_[tau].real());
            running += d_[tau];
            dn_[tau] = running > 0.0 ? d_[tau] * static_cast<double>(tau) / running : 1.0;
        }
        // Local minima of d' in lag order, plus the global minimum.
        minima_.clear();
        std::size_t global = tauMin_;
        for (std::size_t tau = tauMin_; tau < tauMax_; ++tau) {
            if (dn_[tau] < dn_[global]) {
                global = tau;
            }
            if (dn_[tau] < dn_[tau - 1] && dn_[tau] <= dn_[tau + 1]) {
                minima_.push_back(tau);
            }
        }
        masses_.assign(minima_.size() + 1, 0.0);
        for (std::size_t k = 0; k < weights_.size(); ++k) {
            const double threshold = 0.01 * static_cast<double>(k + 1);
            bool found = false;
            for (std::size_t i = 0; i < minima_.size(); ++i) {
                if (dn_[minima_[i]] < threshold) {
                    masses_[i] += weights_[k];
                    found = true;
                    break;
                }
            }
            if (!found) {
                masses_.back() += 0.01 * weights_[k];  // no trough: global minimum
            }
        }
        std::size_t best = global;
        double bestMass = masses_.back();
        for (std::size_t i = 0; i < minima_.size(); ++i) {
            if (masses_[i] > bestMass) {
                bestMass = masses_[i];
                best = minima_[i];
            }
        }
        // Parabolic interpolation: dip found on d', abscissa from the raw d.
        const double a = d_[best - 1];
        const double b = d_[best];
        const double c = d_[best + 1];
        const double denom = a - 2.0 * b + c;
        const double p = denom > 0.0 ? std::clamp(0.5 * (a - c) / denom, -1.0, 1.0) : 0.0;
        frame.hz = fs / (static_cast<double>(best) + p);
        frame.periodicity = std::clamp(1.0 - dn_[best], 0.0, 1.0);
        return frame;
    }

private:
    std::size_t window_;
    std::size_t tauMin_;
    std::size_t tauMax_;
    ComplexFft fft_;
    std::vector<std::complex<double>> packed_;
    std::vector<std::complex<double>> product_;
    std::vector<double> prefix_;
    std::vector<double> d_;
    std::vector<double> dn_;
    std::vector<std::size_t> minima_;
    std::vector<double> masses_;
    std::array<double, 100> weights_{};
};

struct TrackFrame {
    std::size_t start = 0;
    double midi = 0.0;
    double hz = 0.0;
    double periodicity = 0.0;
    double energy = 0.0;
    bool voiced = false;
};

double weightedMedian(std::vector<std::pair<double, double>> valueWeight)
{
    if (valueWeight.empty()) {
        return 0.0;
    }
    std::sort(valueWeight.begin(), valueWeight.end());
    double total = 0.0;
    for (const auto& vw : valueWeight) {
        total += vw.second;
    }
    double running = 0.0;
    for (const auto& vw : valueWeight) {
        running += vw.second;
        if (running >= 0.5 * total) {
            return vw.first;
        }
    }
    return valueWeight.back().first;
}

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

struct LongSpectrum {
    std::vector<SpectralPeak> peaks;  // sorted by frequency
    float flatness = 1.0F;      // 40..5000 Hz (no-pitch rule)
    float wideFlatness = 1.0F;  // 20..5000 Hz, so a 25 Hz tone is inside the band (noisy-body rule)
    double medianDb = -240.0;
};

// Section 5.5: one long Blackman-Harris FFT (<= 32768 samples of data, zero-padded
// >= 4x). Blackman-Harris instead of Hann keeps side lobes out of the peak list.
LongSpectrum longSpectrum(const std::span<const float> mono, const Region region, const double fs)
{
    LongSpectrum out;
    const std::size_t length = std::min(region.size(), kLongDataMax);
    if (length < 8) {
        return out;
    }
    const std::size_t fftSize = std::clamp(nextPowerOfTwo(4 * length), kLongFftMin, kLongFftMax);
    SpectrumAnalyzer analyzer(fftSize, length, SpectrumWindow::blackmanHarris);
    analyzer.analyse(mono.subspan(region.start, length), fs);
    out.peaks = analyzer.peaks(20.0, 0.45 * fs, 50.0, 60);
    out.flatness = analyzer.flatness(40.0, std::min(5000.0, 0.45 * fs));
    out.wideFlatness = analyzer.flatness(20.0, std::min(5000.0, 0.45 * fs));
    out.medianDb = analyzer.medianDb(40.0, std::min(5000.0, 0.45 * fs));
    return out;
}

// Strongest peak within +-tolerance cents of hz, or nullptr.
const SpectralPeak* peakNear(const std::span<const SpectralPeak> peaks, const double hz, const double toleranceCents)
{
    const double ratio = std::exp2(toleranceCents / 1200.0);
    const double lo = hz / ratio;
    const double hi = hz * ratio;
    auto it = std::lower_bound(peaks.begin(), peaks.end(), lo,
                               [](const SpectralPeak& p, const double v) { return p.hz < v; });
    const SpectralPeak* best = nullptr;
    for (; it != peaks.end() && it->hz <= hi; ++it) {
        if (best == nullptr || it->amplitude > best->amplitude) {
            best = &*it;
        }
    }
    return best;
}

double partialWeight(const std::span<const SpectralPeak> peaks, const double hz)
{
    const SpectralPeak* p = peakNear(peaks, hz, kMatchCents);
    return p != nullptr ? std::sqrt(p->amplitude) : 0.0;
}

double harmonicSum(const std::span<const SpectralPeak> peaks, const double f0)
{
    double sum = 0.0;
    double weight = 1.0;
    for (int h = 1; h <= kSumHarmonics; ++h) {
        sum += weight * partialWeight(peaks, f0 * h);
        weight *= kHarmonicDecay;
    }
    return sum;
}

// Octave tests (section 4). tooLow: odd partials hold < 10% of the matched amplitude.
bool tooLow(const std::span<const SpectralPeak> peaks, const double f)
{
    double matched = 0.0;
    double odd = 0.0;
    for (int h = 1; h <= kSumHarmonics; ++h) {
        const double a = partialWeight(peaks, f * h);
        matched += a;
        if (h % 2 == 1) {
            odd += a;
        }
    }
    return matched > 0.0 && odd < 0.1 * matched;
}

// tooHigh: f/2 has its partial 1 or 3 holding >= 15% of its matched amplitude.
bool tooHigh(const std::span<const SpectralPeak> peaks, const double f)
{
    const double half = 0.5 * f;
    double matched = 0.0;
    for (int h = 1; h <= kSumHarmonics; ++h) {
        matched += partialWeight(peaks, half * h);
    }
    const double odd = partialWeight(peaks, half) + partialWeight(peaks, 3.0 * half);
    return matched > 0.0 && odd >= 0.15 * matched;
}

double spectralF0(const std::span<const SpectralPeak> peaks, const double lo, const double hi)
{
    double best = 0.0;
    double bestScore = 0.0;
    for (const SpectralPeak& peak : peaks) {
        for (int h = 1; h <= kCandidateDivisors; ++h) {
            const double candidate = peak.hz / h;
            if (candidate < lo || candidate > hi) {
                continue;
            }
            const double score = harmonicSum(peaks, candidate);
            if (score > bestScore * (1.0 + 1e-9)) {
                bestScore = score;
                best = candidate;
            }
        }
    }
    if (!(best > 0.0)) {
        return 0.0;
    }
    for (int guard = 0; guard < 4 && tooLow(peaks, best) && 2.0 * best <= hi; ++guard) {
        best *= 2.0;
    }
    if (0.5 * best >= lo && tooHigh(peaks, best)) {
        best *= 0.5;
    }
    return best;
}

// Amplitude-weighted mean of partial frequency / h over partials 1..8 near f0.
double refineFromPartials(const std::span<const SpectralPeak> peaks, const double f0)
{
    double sum = 0.0;
    double weights = 0.0;
    for (int h = 1; h <= kSumHarmonics; ++h) {
        const SpectralPeak* p = peakNear(peaks, f0 * h, kMatchCents);
        if (p != nullptr) {
            sum += p->amplitude * p->hz / h;
            weights += p->amplitude;
        }
    }
    return weights > 0.0 ? sum / weights : f0;
}

bool passesOctaveTests(const std::span<const SpectralPeak> peaks, const double f)
{
    return !tooLow(peaks, f) && !tooHigh(peaks, f);
}

} // namespace

bool explainedBySingleNote(const std::span<const SpectralPeak> peaks)
{
    double total = 0.0;
    for (const SpectralPeak& p : peaks) {
        total += p.amplitude;
    }
    if (!(total > 0.0)) {
        return false;
    }
    const double tolerance = std::exp2(10.0 / 1200.0) - 1.0;
    for (const SpectralPeak& source : peaks) {
        for (int divisor = 1; divisor <= kCandidateDivisors; ++divisor) {
            const double f0 = source.hz / divisor;
            if (f0 < 20.0) {
                break;
            }
            double explained = 0.0;
            std::array<bool, 8> present{};  // partials 1..7
            for (const SpectralPeak& p : peaks) {
                const double ratio = p.hz / f0;
                const double h = std::round(ratio);
                if (h < 1.0 || std::abs(ratio / h - 1.0) > tolerance) {
                    continue;
                }
                explained += p.amplitude;
                if (h < 8.0) {
                    present[static_cast<std::size_t>(h)] = true;
                }
            }
            if (explained >= 0.9 * total && (present[1] || present[2] || present[3])
                && (present[1] || present[5] || present[7])) {
                return true;
            }
        }
    }
    return false;
}

PitchAnalysis analysePitch(const std::span<const float> mono, const double fs, const std::atomic<bool>* cancel)
{
    PitchAnalysis out;
    if (mono.size() < 8 || !(fs > 0.0)) {
        return out;
    }
    const double maxHz = std::min(kMaxHz, 0.45 * fs);
    const Region region = usableRegion(mono, fs);
    out.usableSeconds = static_cast<double>(region.size()) / fs;

    // Lags: 25 Hz (with margin) .. 4 kHz; integration window W = 2 * tauMax.
    const std::size_t tauMin =
        std::max<std::size_t>(2, static_cast<std::size_t>(std::floor(fs / (maxHz * std::exp2(60.0 / 1200.0)))));
    std::size_t tauMax = static_cast<std::size_t>(std::ceil(fs / (kMinHz * std::exp2(-60.0 / 1200.0)))) + 2;
    std::size_t window = 2 * tauMax;
    double minHz = kMinHz;
    if (mono.size() < window + tauMax) {
        // Short clip: W = tauMax = N/2, so the lowest pitch seen is 2 fs / N.
        tauMax = mono.size() / 2;
        window = tauMax;
        minHz = fs / static_cast<double>(tauMax);
        out.estimate.rangeLimited = true;
    }
    if (tauMax < tauMin + 3) {
        return out;
    }
    Yin yin(window, tauMin, tauMax);
    const std::size_t span = yin.span();

    // Frame starts spread over the usable region (<= 64 frames, 20 ms hop).
    std::size_t first = region.start;
    std::size_t last = region.end >= span ? region.end - span : 0;
    if (region.size() < span) {
        const std::size_t centre = region.start + region.size() / 2;
        first = std::min(centre >= span / 2 ? centre - span / 2 : 0, mono.size() - span);
        last = first;
    }
    const std::size_t hop = std::max<std::size_t>(1, static_cast<std::size_t>(kHopSeconds * fs));
    const std::size_t frameCount = std::min(kMaxFrames, (last - first) / hop + 1);
    const double spacing = frameCount > 1 ? static_cast<double>(last - first) / static_cast<double>(frameCount - 1)
                                          : static_cast<double>(hop);
    std::vector<TrackFrame> frames(frameCount);
    double maxEnergy = 0.0;
    for (std::size_t i = 0; i < frameCount; ++i) {
        if (cancelled(cancel)) {
            out.cancelled = true;
            return out;
        }
        TrackFrame& f = frames[i];
        f.start = first + static_cast<std::size_t>(std::llround(spacing * static_cast<double>(i)));
        f.start = std::min(f.start, mono.size() - span);
        const Yin::Frame y = yin.analyse(mono.data() + f.start, fs);
        f.hz = y.hz;
        f.periodicity = y.periodicity;
        f.energy = y.energy;
        f.midi = y.hz > 0.0 ? hzToMidi(y.hz) : 0.0;
        maxEnergy = std::max(maxEnergy, y.energy);
    }
    std::vector<double> loudPeriodicity;
    for (TrackFrame& f : frames) {
        const bool loud = f.energy >= 0.001 * maxEnergy && f.hz > 0.0;
        f.voiced = loud && f.periodicity >= 0.3;
        if (loud) {
            loudPeriodicity.push_back(f.periodicity);
        }
    }

    const LongSpectrum body = longSpectrum(mono, region.size() >= 8 ? region : Region{0, mono.size()}, fs);
    out.flatness = body.flatness;
    out.singleNote = explainedBySingleNote(body.peaks);
    double strongestDb = -300.0;
    for (const SpectralPeak& p : body.peaks) {
        strongestDb = std::max(strongestDb, p.db);
    }
    const bool noPeak = strongestDb < body.medianDb + 15.0;
    const double periodicity = median(loudPeriodicity);

    PitchEstimate& est = out.estimate;
    // Section 5: approximate (drum) mode for weakly periodic sounds. [D] Also when the
    // body spectrum is noisy (20..5000 Hz flatness > 0.03, roughly SNR < 13 dB): YIN over 80 ms
    // still finds a snare's tone 70-80% periodic, so periodicity alone misses it.
    if (periodicity < 0.5 || body.wideFlatness > kNoisyFlatness) {
        const SpectralPeak* dominant = nullptr;
        for (const SpectralPeak& p : body.peaks) {
            if (p.hz >= 30.0 && p.hz <= 1000.0 && (dominant == nullptr || p.db > dominant->db)) {
                dominant = &p;
            }
        }
        if (dominant != nullptr) {
            const SpectralPeak* chosen = dominant;
            for (const SpectralPeak& p : body.peaks) {
                if (p.hz >= 30.0 && p.hz < chosen->hz && p.db >= dominant->db - 6.0) {
                    chosen = &p;
                    break;  // sorted by frequency: the earliest qualifying peak
                }
            }
            est.hz = chosen->hz;
            est.midi = hzToMidi(chosen->hz);
            est.approximate = true;
            est.confidence = static_cast<float>(std::clamp((chosen->db - body.medianDb) / 30.0, 0.0, 0.6));
        }
        est.valid = dominant != nullptr && est.confidence >= 0.40F && !(body.flatness > 0.4F && noPeak);
        out.percussive = true;
        return out;
    }

    // Section 5.3: longest stable run of voiced frames.
    std::size_t runFirst = 0;
    std::size_t runLength = 0;
    for (std::size_t i = 0; i < frameCount; ++i) {
        if (!frames[i].voiced) {
            continue;
        }
        double lo = frames[i].midi;
        double hi = frames[i].midi;
        std::size_t j = i + 1;
        for (; j < frameCount && frames[j].voiced; ++j) {
            if (std::abs(frames[j].midi - frames[j - 1].midi) >= 0.15) {
                break;
            }
            const double nlo = std::min(lo, frames[j].midi);
            const double nhi = std::max(hi, frames[j].midi);
            if (nhi - nlo >= 0.5) {
                break;
            }
            lo = nlo;
            hi = nhi;
        }
        if (j - i > runLength) {
            runLength = j - i;
            runFirst = i;
        }
    }
    const bool stable = runLength >= 4 && static_cast<double>(runLength) * spacing >= 0.08 * fs;
    std::vector<std::size_t> chosen;
    std::vector<std::size_t> voiced;
    for (std::size_t i = 0; i < frameCount; ++i) {
        if (frames[i].voiced) {
            voiced.push_back(i);
        }
    }
    if (voiced.empty()) {
        out.percussive = true;
        return out;
    }
    Region refineRegion = region;
    if (stable) {
        for (std::size_t i = runFirst; i < runFirst + runLength; ++i) {
            chosen.push_back(i);
        }
        refineRegion = {frames[runFirst].start, frames[runFirst + runLength - 1].start + span};
    } else {
        // Section 5.4: the last 35% of voiced frames; gliding if first and last differ > 100 cents.
        const std::size_t take = std::max<std::size_t>(1, (voiced.size() * 35 + 99) / 100);
        chosen.assign(voiced.end() - static_cast<std::ptrdiff_t>(take), voiced.end());
        est.gliding = std::abs(frames[voiced.front()].midi - frames[voiced.back()].midi) > 1.0;
    }
    std::vector<std::pair<double, double>> weighted;
    std::vector<double> chosenPeriodicity;
    for (const std::size_t i : chosen) {
        weighted.emplace_back(frames[i].midi, frames[i].energy * frames[i].periodicity + 1e-30);
        chosenPeriodicity.push_back(frames[i].periodicity);
    }
    const double yinMidi = weightedMedian(weighted);
    const double yinHz = 440.0 * std::exp2((yinMidi - 69.0) / 12.0);
    const double lo = minHz * std::exp2(-50.0 / 1200.0);
    const double hi = maxHz * std::exp2(50.0 / 1200.0);

    double resultHz = yinHz;
    double agreement = 1.0;
    double trackMidi = yinMidi;  // the frame track's value, for the frame-agreement share s
    if (!est.gliding) {
        const LongSpectrum refine = (refineRegion.start == region.start && refineRegion.end == region.end)
                                        ? body
                                        : longSpectrum(mono, refineRegion, fs);
        const double specHz = spectralF0(refine.peaks, lo, hi);
        if (specHz > 0.0) {
            const double cents = centsBetween(yinHz, specHz);
            const double octaves = std::abs(cents) / 1200.0;
            const bool related = std::abs(std::abs(cents) - 1200.0) < 50.0 || std::abs(std::abs(cents) - 2400.0) < 50.0
                                 || std::abs(std::abs(cents) - 1902.0) < 50.0;
            if (std::abs(cents) < 50.0) {
                resultHz = refineFromPartials(refine.peaks, specHz);
            } else if (related && octaves > 0.0) {
                const bool yinPasses = passesOctaveTests(refine.peaks, yinHz);
                const bool specPasses = passesOctaveTests(refine.peaks, specHz);
                resultHz = (yinPasses && !specPasses) ? refineFromPartials(refine.peaks, yinHz)
                                                      : refineFromPartials(refine.peaks, specHz);
                agreement = (specPasses && specHz > kYinUnreliableHz && yinHz < specHz) ? 1.0 : 0.6;
            } else {
                agreement = 0.3;
            }
        }
    }
    est.hz = resultHz;
    est.midi = hzToMidi(resultHz);
    if (agreement < 0.3 + 1e-9 || std::abs(est.midi - yinMidi) < 0.5) {
        trackMidi = est.midi;
    }

    // Section 6: conf = q^0.5 * g * (0.5 + 0.5 s); s is measured on the frame track,
    // so an octave resolved against YIN does not also count as frame disagreement.
    std::size_t near = 0;
    for (const std::size_t i : voiced) {
        if (std::abs(frames[i].midi - trackMidi) < 0.5) {
            ++near;
        }
    }
    const double s = static_cast<double>(near) / static_cast<double>(voiced.size());
    const double q = median(chosenPeriodicity);
    double confidence = std::sqrt(q) * agreement * (0.5 + 0.5 * s);
    if (!stable) {
        confidence = std::min(confidence, 0.6);
    }
    est.confidence = static_cast<float>(confidence);
    est.valid = est.confidence >= 0.40F && !(body.flatness > 0.4F && noPeak);
    out.percussive = out.usableSeconds < 0.15 || est.gliding;
    return out;
}

PitchEstimate trackPitch(const std::span<const float> mono, const double rate, const std::atomic<bool>* cancel)
{
    return analysePitch(mono, rate, cancel).estimate;
}

} // namespace sms::analysis
