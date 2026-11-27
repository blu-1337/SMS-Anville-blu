// Vintage FX: processor checks (Docs/AI/VINTAGE-FX.md, research note section 8) and
// engine integration (Off bit-identical, aux buses, no allocation, rough CPU).
#include "../src/core/SamplerEngine.hpp"
#include "DSP/VintageProcessor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <set>
#include <vector>

namespace {
std::atomic<bool> gCountAllocations{false};
std::atomic<int> gAllocations{0};
}

void* operator new(const std::size_t size)
{
    if (gCountAllocations.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0U ? 1U : size))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }

namespace {
using sms::dsp::VintageProcessor;
using sms::dsp::VintageSettings;
namespace vd = sms::dsp::vintage_detail;
constexpr double kPi = 3.14159265358979323846;

void check(const bool condition, const char* const message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

double db(const double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-30)); }

/** Amplitude of one frequency in a signal (windowless single-bin DFT over whole cycles). */
double amplitude(const std::vector<float>& signal, const double hz, const double fs)
{
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < signal.size(); ++n) {
        re += signal[n] * std::cos(2.0 * kPi * hz * static_cast<double>(n) / fs);
        im -= signal[n] * std::sin(2.0 * kPi * hz * static_cast<double>(n) / fs);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(signal.size());
}

/** |H(f)| of a filter from its impulse response. */
template <class Filter>
std::vector<double> impulse(Filter& filter, const std::size_t length)
{
    std::vector<double> response(length);
    for (std::size_t n = 0; n < length; ++n)
        response[n] = filter.process(n == 0U ? 1.0f : 0.0f, 0);
    return response;
}
double magnitude(const std::vector<double>& response, const double hz, const double fs)
{
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < response.size(); ++n) {
        re += response[n] * std::cos(2.0 * kPi * hz * static_cast<double>(n) / fs);
        im -= response[n] * std::sin(2.0 * kPi * hz * static_cast<double>(n) / fs);
    }
    return std::hypot(re, im);
}
double minus3dB(const std::vector<double>& response, const double fs)
{
    double low = 100.0, high = 0.45 * fs;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (low + high);
        (magnitude(response, mid, fs) > std::sqrt(0.5) ? low : high) = mid;
    }
    return 0.5 * (low + high);
}

std::vector<float> runSine(VintageProcessor& processor, const double fs, const double hz,
                           const float level, const std::size_t frames, const std::size_t skip)
{
    std::vector<float> out;
    out.reserve(frames);
    for (std::size_t n = 0; n < frames + skip; ++n) {
        float l = level * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(n) / fs));
        float r = l;
        processor.process(l, r);
        if (n >= skip) out.push_back(l);
    }
    return out;
}

// 1. Off and Mix 0 are exact bypasses at every host rate, pad and bus instances.
void bypassIsExact()
{
    for (const double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (const bool bus : {false, true}) {
            for (const VintageSettings settings : {VintageSettings{0.0f, 1.0f, 4.0f, 8000.0f, 24.0f, 2.0f, 1.0f, 1.0f},
                                                   VintageSettings{2.0f, 3.0f, 4.0f, 8000.0f, 24.0f, 2.0f, 0.0f, 1.0f}}) {
                VintageProcessor processor;
                processor.prepare(fs, bus);
                processor.setTarget(settings);
                for (int n = 0; n < 4096; ++n) {
                    const float in[2]{0.7f * std::sin(0.01f * static_cast<float>(n)), 1.0e-38f * static_cast<float>(n % 3)};
                    float l = in[0], r = in[1];
                    processor.process(l, r);
                    check(std::memcmp(&l, &in[0], sizeof l) == 0 && std::memcmp(&r, &in[1], sizeof r) == 0,
                          "Off and Mix 0 leave every input bit unchanged");
                }
                check(processor.bypassed(), "Off and Mix 0 report bypass");
            }
        }
    }
}

// 2-3. Mid-tread quantiser: exact level count, silence, SNR.
void quantiser()
{
    for (const int bits : {12, 8}) {
        const float scale = std::exp2(static_cast<float>(bits - 1));
        std::set<float> levels;
        for (int i = 0; i <= 2000000; ++i)
            levels.insert(vd::quantize(-1.0f + 2.0f * static_cast<float>(i) / 2000000.0f, scale, 1.0f / scale));
        check(levels.size() == (bits == 12 ? 4096U : 256U), "dense ramp yields exactly 2^bits levels");
        check(vd::quantize(0.0f, scale, 1.0f / scale) == 0.0f, "zero stays zero");
    }
    double signal = 0.0, noise = 0.0;
    for (int n = 0; n < 200000; ++n) {
        const float x = 0.99995f * static_cast<float>(std::sin(2.0 * kPi * 997.0 * n / 48000.0));
        const float e = vd::quantize(x, 2048.0f, 1.0f / 2048.0f) - x;
        signal += static_cast<double>(x) * x;
        noise += static_cast<double>(e) * e;
    }
    const double snr = 10.0 * std::log10(signal / noise);
    check(snr > 72.0 && snr < 76.0, "12-bit full-scale sine SNR is 74 +/- 2 dB");
}

// 4-5. Drum stream, no pre-filter, Raw: hold images.
void holdImages()
{
    VintageSettings raw{1.0f, 5.0f, 16.0f, sms::dsp::kVintageDrumRateHz, 0.0f, 0.0f, 1.0f, 0.0f};
    const auto image = [&](const double fs, const double hz, const double imageHz) {
        VintageProcessor processor;
        processor.prepare(fs, false);
        processor.setTarget(raw, true);
        const auto out = runSine(processor, fs, hz, 0.5f, static_cast<std::size_t>(fs), 4096U);
        return db(amplitude(out, imageHz, fs) / amplitude(out, hz, fs));
    };
    const double fm = sms::dsp::kVintageDrumRateHz;
    const double i1 = image(96000.0, 1000.0, fm - 1000.0);
    const double i10 = image(96000.0, 10000.0, fm - 10000.0);
    std::cout << "images at 96k: 1k -> " << i1 << " dBc, 10k -> " << i10 << " dBc\n";
    check(i1 >= -32.0 && i1 <= -26.0, "1 kHz image at fm - 1k is -26..-32 dBc (96k host)");
    check(i10 >= -6.0 && i10 <= -3.5, "10 kHz image at fm - 10k is -3.5..-6 dBc (96k host)");
    const double folded = image(44100.0, 1000.0, 44100.0 - (fm - 1000.0));
    const double folded48 = image(48000.0, 1000.0, 48000.0 - (fm - 1000.0));
    std::cout << "folded images: 44.1k " << folded << " dBc, 48k " << folded48 << " dBc\n";
    check(folded < i1 - 3.0 && folded48 < i1 - 3.0, "folded images stay well below the 96k level");
}

// 7. Butterworth magnitude; resonance stays bounded.
void butterworth()
{
    for (const double fs : {192000.0, 48000.0}) {
        for (const double fc : {1600.0, 8000.0, 16000.0}) {
            vd::ModelFilter filter;
            filter.butterworth6(fc, fs);
            const auto response = impulse(filter, 1U << 14U);
            for (const double f : {0.25 * fc, 0.5 * fc, fc, 1.5 * fc, 2.0 * fc}) {
                if (f > 0.45 * fs) continue;
                const double ratio = std::tan(kPi * f / fs) / std::tan(kPi * fc / fs);
                const double expected = -10.0 * std::log10(1.0 + std::pow(ratio, 12.0));
                check(std::abs(db(magnitude(response, f, fs)) - expected) < 0.2,
                      "6th-order Butterworth matches |H|^2 = 1/(1+(w/wc)^12) within 0.2 dB");
            }
        }
    }
    vd::ModelFilter filter;
    filter.butterworth6(8000.0, 192000.0);
    const auto response = impulse(filter, 1U << 14U);
    check(std::abs(minus3dB(response, 192000.0) / 8000.0 - 1.0) < 0.01 &&
              db(magnitude(response, 16000.0, 192000.0)) <= -36.0,
          "-3 dB at fc and at least 36 dB down at 2 fc");
    vd::ModelFilter resonant;
    resonant.butterworth6(4000.0, 48000.0, 1.0);
    const auto peaked = impulse(resonant, 1U << 14U);
    double peak = 0.0;
    for (double f = 100.0; f < 20000.0; f += 25.0)
        peak = std::max(peak, magnitude(peaked, f, 48000.0));
    std::cout << "rack resonance 1: peak " << db(peak) << " dB, passband "
              << db(magnitude(peaked, 100.0, 48000.0)) << " dB\n";
    check(db(peak) > 3.0 && db(peak) < 6.5 && std::isfinite(peak),
          "maximum resonance peaks clearly but stays under +6.5 dB");
}

// 8. Drum fixed voicings and Sweep at rest.
void drumFilters()
{
    const double fs = 192000.0;
    const double table[4]{8000.0, 10000.0, 11600.0, 13000.0};
    for (int voicing = 1; voicing <= 4; ++voicing) {
        vd::ModelFilter filter;
        filter.drum(static_cast<sms::dsp::VintageVoicing>(voicing), 0.0, fs, 0.0);
        const double corner = minus3dB(impulse(filter, 1U << 13U), fs);
        std::cout << "drum voicing " << voicing << ": -3 dB at " << corner << " Hz\n";
        check(std::abs(corner / table[voicing - 1] - 1.0) < 0.05, "drum -3 dB points within 5% of the table");
    }
    vd::ModelFilter sweep;
    sweep.drum(sms::dsp::VintageVoicing::sweep, 0.0, fs, 0.0);
    check(std::abs(db(magnitude(impulse(sweep, 1U << 14U), 1000.0, fs)) + 12.0) < 0.3,
          "Sweep at rest is -12 dB at 1 kHz");
}

// 9. Drive keeps a -12 dBFS peak and stays bounded.
void drive()
{
    for (float decibels = 0.0f; decibels <= 24.0f; decibels += 3.0f) {
        vd::Drive stage;
        stage.set(decibels);
        float peak = 0.0f;
        for (int n = 0; n < 48000; ++n)
            peak = std::max(peak, std::abs(stage.process(0.25f * static_cast<float>(std::sin(2.0 * kPi * 101.0 * n / 48000.0)))));
        check(std::abs(db(peak / 0.25)) < 0.1, "-12 dBFS peak unchanged within 0.1 dB across 0-24 dB");
        for (int i = -1000; i <= 1000; ++i) {
            const float y = stage.process(static_cast<float>(i) / 1000.0f);
            check(std::isfinite(y) && std::abs(y) <= 1.0f, "drive output is finite and within +-1");
        }
    }
}

// 10. Switching models and knobs every 50 ms: no NaN, no jump beyond steady state + 0.02.
void switching()
{
    const double fs = 48000.0;
    const std::vector<VintageSettings> configs{
        {0.0f, 5.0f, 16.0f, 26041.67f, 0.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 5.0f, 16.0f, 26041.67f, 0.0f, 0.0f, 1.0f, 0.0f},
        {2.0f, 5.0f, 16.0f, 40000.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 3.0f, 16.0f, 26041.67f, 12.0f, -1.0f, 0.5f, 0.0f},
        {2.0f, 5.0f, 16.0f, 12000.0f, 6.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 0.0f, 16.0f, 8000.0f, 0.0f, 0.0f, 0.75f, 0.0f},
    };
    const auto sine = [&](const std::size_t n) {
        return 0.25f * static_cast<float>(std::sin(2.0 * kPi * 100.0 * static_cast<double>(n) / fs));
    };
    std::vector<float> steady;
    for (const auto& config : configs) {
        VintageProcessor processor;
        processor.prepare(fs, false);
        processor.setTarget(config, true);
        float previous = 0.0f, largest = 0.0f;
        for (std::size_t n = 0; n < 24000U; ++n) {
            float l = sine(n), r = l;
            processor.process(l, r);
            if (n > 4800U) largest = std::max(largest, std::abs(l - previous));
            previous = l;
        }
        steady.push_back(largest);
    }
    VintageProcessor processor;
    processor.prepare(fs, false);
    float previous = 0.0f;
    std::size_t current = 0U, before = 0U;
    for (std::size_t n = 0; n < 48000U * 6U; ++n) {
        if (n % 2400U == 0U) {
            before = current;
            current = (current * 5U + 3U + n / 2400U) % configs.size();
        }
        if (n % 64U == 0U) processor.setTarget(configs[current]); // once per host block
        float l = sine(n), r = l;
        processor.process(l, r);
        check(std::isfinite(l) && std::isfinite(r), "switching never produces NaN");
        if (n > 0U)
            check(std::abs(l - previous) <= std::max(steady[current], steady[before]) + 0.02f,
                  "switching never jumps beyond the steady-state delta plus 0.02");
        previous = l;
    }
}

// 11. A bus goes idle after a tail: no subnormals, no further work.
void silenceTail()
{
    VintageProcessor processor;
    processor.prepare(48000.0, true);
    processor.setTarget({2.0f, 3.0f, 12.0f, 8000.0f, 12.0f, -2.0f, 1.0f, 1.0f}, true);
    for (std::size_t n = 0; n < 48000U * 10U + 4800U; ++n) {
        float l = n < 4800U ? 0.9f * std::sin(0.05f * static_cast<float>(n)) : 0.0f;
        float r = -l;
        processor.process(l, r);
        check(std::fpclassify(l) != FP_SUBNORMAL && std::fpclassify(r) != FP_SUBNORMAL,
              "silence tail produces no subnormals");
    }
    check(processor.idle(), "a silent bus goes idle once its tail is below -120 dB");
}

using namespace midichopper;

std::unique_ptr<SamplerEngine> makeEngine(const bool vintagePads)
{
    auto engine = std::make_unique<SamplerEngine>(48000.0, 4.0);
    auto settings = engine->settings();
    settings.monitorInput = false;
    settings.playbackMode = PlaybackMode::Hold;
    settings.maxVoices = 16U;
    engine->setSettings(settings);
    for (std::uint32_t pad = 0; pad < 16U; ++pad) {
        PadData data;
        data.sampleRate = 44100.0;
        data.frames = 6000U;
        data.stereo.resize(12000U);
        for (std::uint32_t n = 0; n < 6000U; ++n)
            data.stereo[n * 2U] = data.stereo[n * 2U + 1U] =
                0.3f * std::sin(0.013f * static_cast<float>((pad + 1U) * n));
        check(engine->importPad(pad, data), "engine fixture import");
        sms::dsp::SampleMixerSettings mixer;
        mixer.holdLoop = 1.0f;
        mixer.tuneSemitones = 0.31f * static_cast<float>(pad);
        mixer.outputBus = static_cast<float>(pad);
        if (vintagePads)
            mixer.vintage = {pad % 2U ? 2.0f : 1.0f, static_cast<float>(pad % 6U), 12.0f,
                             pad % 2U ? 30000.0f : 26041.67f, 6.0f, 0.0f, 1.0f, 0.5f};
        engine->setPadMixerSettings(pad, mixer);
    }
    return engine;
}

struct Buses {
    std::vector<float> storage = std::vector<float>(34U * 256U);
    std::array<float*, 32> aux{};
    Buses() { for (std::size_t i = 0; i < 32U; ++i) aux[i] = storage.data() + (i + 2U) * 256U; }
    float* left() { return storage.data(); }
    float* right() { return storage.data() + 256U; }
};

void playAll(SamplerEngine& engine, Buses& buses)
{
    std::array<MidiEvent, 16> notes{};
    for (std::uint8_t pad = 0; pad < 16U; ++pad)
        notes[pad] = {0U, static_cast<std::uint8_t>(kDefaultBaseMidiNote + pad), 100U, MidiEventType::NoteOn};
    engine.process(nullptr, nullptr, buses.left(), buses.right(), 256U, notes.data(), 16U);
}

// Global FX Off is bit-identical (after an On/Off round trip), On reaches main and aux.
void engineIntegration()
{
    auto reference = makeEngine(false);
    auto roundTrip = makeEngine(false);
    auto wet = makeEngine(false);
    Buses a, b, c;
    for (auto* buses : {&a, &b, &c}) static_cast<void>(buses);
    reference->setAuxOutputs(a.aux.data());
    roundTrip->setAuxOutputs(b.aux.data());
    wet->setAuxOutputs(c.aux.data());
    roundTrip->setGlobalVintage({2.0f, 3.0f, 8.0f, 20000.0f, 6.0f, 0.0f, 1.0f, 0.3f});
    for (int block = 0; block < 4; ++block)
        roundTrip->process(nullptr, nullptr, b.left(), b.right(), 256U);
    roundTrip->setGlobalVintage({0.0f, 3.0f, 8.0f, 20000.0f, 6.0f, 0.0f, 1.0f, 0.3f});
    for (int block = 0; block < 4; ++block) // 10 ms fade = 480 frames
        roundTrip->process(nullptr, nullptr, b.left(), b.right(), 256U);
    wet->setGlobalVintage({1.0f, 1.0f, 8.0f, 12000.0f, 0.0f, 0.0f, 1.0f, 0.0f});
    check(roundTrip->globalVintage().model == 0.0f && wet->globalVintage().bits == 8.0f,
          "the engine reports the requested global vintage");
    playAll(*reference, a);
    playAll(*roundTrip, b);
    playAll(*wet, c);
    bool auxDiffers = false;
    for (int block = 0; block < 8; ++block) {
        check(std::memcmp(a.storage.data(), b.storage.data(), a.storage.size() * sizeof(float)) == 0,
              "global vintage Off is bit-identical on main and every aux bus");
        for (std::size_t i = 0; i < c.storage.size(); ++i)
            check(std::isfinite(c.storage[i]), "global vintage output stays finite");
        for (std::size_t i = 2U * 256U; i < a.storage.size(); ++i)
            auxDiffers = auxDiffers || a.storage[i] != c.storage[i];
        reference->process(nullptr, nullptr, a.left(), a.right(), 256U);
        roundTrip->process(nullptr, nullptr, b.left(), b.right(), 256U);
        wet->process(nullptr, nullptr, c.left(), c.right(), 256U);
    }
    check(auxDiffers, "global vintage processes the auxiliary buses");

    auto pads = makeEngine(true);
    Buses d;
    pads->setAuxOutputs(d.aux.data());
    pads->setGlobalVintage({2.0f, 3.0f, 12.0f, 40000.0f, 3.0f, 0.0f, 1.0f, 1.0f});
    playAll(*pads, d);
    gAllocations = 0;
    gCountAllocations = true;
    for (int block = 0; block < 64; ++block)
        pads->process(nullptr, nullptr, d.left(), d.right(), 256U);
    gCountAllocations = false;
    check(gAllocations.load() == 0, "process allocates nothing with pad and bus vintage on");
    for (const float sample : d.storage)
        check(std::isfinite(sample), "pad vintage output stays finite");
}

/** Rough CPU: 16 engine voices (the engine maximum) + 17 buses, then 64 bare voice processors. */
void cpuEstimate()
{
    const auto timeEngine = [](const bool vintage) {
        auto engine = makeEngine(vintage);
        Buses buses;
        engine->setAuxOutputs(buses.aux.data());
        if (vintage)
            engine->setGlobalVintage({2.0f, 3.0f, 12.0f, 40000.0f, 3.0f, 0.0f, 1.0f, 0.5f});
        playAll(*engine, buses);
        constexpr int kBlocks = 1875; // 10 s at 48 kHz
        const auto start = std::chrono::steady_clock::now();
        for (int block = 0; block < kBlocks; ++block)
            engine->process(nullptr, nullptr, buses.left(), buses.right(), 256U);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const double dry = timeEngine(false);
    const double wet = timeEngine(true);
    std::array<VintageProcessor, 81> processors;
    for (std::size_t i = 0; i < processors.size(); ++i) {
        processors[i].prepare(48000.0, i >= 64U);
        processors[i].setTarget({i % 2U ? 2.0f : 1.0f, static_cast<float>(i % 6U), 12.0f,
                                 i % 2U ? 40000.0f : 26041.67f, 6.0f, 0.0f, 1.0f, 0.5f}, true);
    }
    float sink = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < 480000; ++n) {
        const float x = 0.3f * std::sin(0.01f * static_cast<float>(n));
        for (auto& processor : processors) {
            float l = x, r = -x;
            processor.process(l, r);
            sink += l;
        }
    }
    const double bare = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "CPU for 10 s of audio at 48 kHz: engine 16 voices + 17 buses dry " << dry
              << " s, with pad+bus vintage " << wet << " s; 64 voice + 17 bus processors alone "
              << bare << " s (sink " << (std::isfinite(sink) ? "ok" : "nan") << ")\n";
    check(wet < 10.0 && bare < 10.0, "vintage processing runs faster than real time");
}
} // namespace

int main()
{
    bypassIsExact();
    quantiser();
    holdImages();
    butterworth();
    drumFilters();
    drive();
    switching();
    silenceTail();
    engineIntegration();
    cpuEstimate();
    std::cout << "vintage processor tests passed\n";
}
