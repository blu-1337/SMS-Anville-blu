#pragma once

// DPF-free AC1 codec: auto-chop requests (UI to worker) and status replies
// (worker to UI). Fields are ASCII and ';'-separated; numbers are integers so
// no locale or float parsing is involved (Docs/AI/AUTO-CHOP.md).

#include "MainSampleProtocol.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace midichopper::plugin {

enum class AutoChopMode : char { transient = 'T', findSamples = 'F', equal = 'E', beat = 'B', random = 'R' };

inline constexpr std::uint32_t kAutoChopMaxCount = 64U;

struct AutoChopRequest {
    std::uint64_t sequence = 0U;
    AutoChopMode mode = AutoChopMode::transient;
    std::uint32_t firstPad = 0U;
    std::uint32_t count = 0U;       // 1..64 pads to fill
    bool confirm = false;
    std::uint32_t rangeStart = 0U;  // source frames; rangeEnd == 0 means whole source
    std::uint32_t rangeEnd = 0U;
    std::uint32_t sensitivityPermille = 500U; // T: 0..1000
    std::uint32_t minGapMs = 50U;             // T
    std::uint64_t seed = 0U;                  // F, R
    std::uint32_t bpmCenti = 0U;              // B, R: BPM * 100; 0 = host tempo
    std::uint32_t beatsMilli = 1000U;         // B: beats per slice * 1000; R: length in beats * 1000
    std::uint32_t anchor = 0U;                // B: anchor frame
    bool snap = false;                        // B, R
};

[[nodiscard]] inline std::string encodeAutoChopRequest(const AutoChopRequest& r)
{
    std::string s = "AC1;" + std::to_string(r.sequence) + ";";
    s += static_cast<char>(r.mode);
    s += ";" + std::to_string(r.firstPad) + ";" + std::to_string(r.count) + ";" +
         (r.confirm ? "1" : "0") + ";" + std::to_string(r.rangeStart) + ";" +
         std::to_string(r.rangeEnd);
    switch (r.mode) {
    case AutoChopMode::transient:
        s += ";" + std::to_string(r.sensitivityPermille) + ";" + std::to_string(r.minGapMs);
        break;
    case AutoChopMode::findSamples: s += ";" + std::to_string(r.seed); break;
    case AutoChopMode::equal: break;
    case AutoChopMode::beat:
        s += ";" + std::to_string(r.bpmCenti) + ";" + std::to_string(r.beatsMilli) + ";" +
             std::to_string(r.anchor) + ";" + (r.snap ? "1" : "0");
        break;
    case AutoChopMode::random:
        s += ";" + std::to_string(r.bpmCenti) + ";" + std::to_string(r.beatsMilli) + ";" +
             std::to_string(r.seed) + ";" + (r.snap ? "1" : "0");
        break;
    }
    return s;
}

[[nodiscard]] inline bool decodeAutoChopRequest(const std::string_view encoded,
                                                AutoChopRequest& request)
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("AC1;"))
        return false;
    Fields f(encoded.substr(4U));
    AutoChopRequest d;
    std::string_view mode;
    unsigned confirm = 2U;
    if (!f.number(d.sequence) || !f.next(mode) || mode.size() != 1U ||
        !f.number(d.firstPad) || !f.number(d.count) || !f.number(confirm) ||
        !f.number(d.rangeStart))
        return false;
    // The range end is the last common field only when the mode has no extras.
    const char m = mode[0];
    unsigned snap = 2U;
    if (!f.number(d.rangeEnd))
        return false;
    switch (m) {
    case 'T':
        if (!f.number(d.sensitivityPermille) || !f.number(d.minGapMs) ||
            d.sensitivityPermille > 1000U || d.minGapMs > 10000U)
            return false;
        d.mode = AutoChopMode::transient;
        break;
    case 'F':
        if (!f.number(d.seed)) return false;
        d.mode = AutoChopMode::findSamples;
        break;
    case 'E': d.mode = AutoChopMode::equal; break;
    case 'B':
        if (!f.number(d.bpmCenti) || !f.number(d.beatsMilli) || !f.number(d.anchor) ||
            !f.number(snap) || snap > 1U || d.bpmCenti > 100000U ||
            d.beatsMilli == 0U || d.beatsMilli > 64000U)
            return false;
        d.mode = AutoChopMode::beat;
        d.snap = snap == 1U;
        break;
    case 'R':
        if (!f.number(d.bpmCenti) || !f.number(d.beatsMilli) || !f.number(d.seed) ||
            !f.number(snap) || snap > 1U || d.bpmCenti > 100000U ||
            d.beatsMilli == 0U || d.beatsMilli > 64000U)
            return false;
        d.mode = AutoChopMode::random;
        d.snap = snap == 1U;
        break;
    default: return false;
    }
    if (!f.finished() || confirm > 1U || d.count == 0U || d.count > kAutoChopMaxCount ||
        d.firstPad >= kPadCount || (d.rangeEnd != 0U && d.rangeStart >= d.rangeEnd))
        return false;
    d.confirm = confirm == 1U;
    request = d;
    return true;
}

enum class AutoChopState : std::uint8_t { ok, confirm, busy, error };

struct AutoChopStatus {
    std::uint64_t sequence = 0U;
    AutoChopState state = AutoChopState::error;
    std::uint64_t mask = 0U;       // ok: pads filled; confirm: pads that hold other audio
    std::uint32_t found = 0U;      // ok: regions found; confirm: pads to replace
    std::uint32_t requested = 0U;  // ok
    std::string text;              // error
};

[[nodiscard]] inline std::string encodeAutoChopStatus(const AutoChopStatus& s)
{
    auto hex = [](std::uint64_t v) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        do { out.insert(out.begin(), digits[v & 15U]); v >>= 4U; } while (v != 0U);
        return out;
    };
    std::string out = "AC1;" + std::to_string(s.sequence) + ";";
    switch (s.state) {
    case AutoChopState::ok:
        return out + "OK;" + hex(s.mask) + ";" + std::to_string(s.found) + ";" +
               std::to_string(s.requested);
    case AutoChopState::confirm:
        return out + "CONFIRM;" + hex(s.mask) + ";" + std::to_string(s.found);
    case AutoChopState::busy: return out + "BUSY";
    case AutoChopState::error: break;
    }
    return out + "ERROR;" + main_sample_detail::truncateUtf8(s.text, 200U);
}

[[nodiscard]] inline bool decodeAutoChopStatus(const std::string_view encoded, AutoChopStatus& status)
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("AC1;"))
        return false;
    Fields f(encoded.substr(4U));
    AutoChopStatus d;
    std::string_view kind;
    if (!f.number(d.sequence) || !f.next(kind))
        return false;
    if (kind == "OK") {
        d.state = AutoChopState::ok;
        if (!f.number(d.mask, 16) || !f.number(d.found) || !f.number(d.requested) || !f.finished())
            return false;
    } else if (kind == "CONFIRM") {
        d.state = AutoChopState::confirm;
        if (!f.number(d.mask, 16) || !f.number(d.found) || !f.finished())
            return false;
    } else if (kind == "BUSY") {
        d.state = AutoChopState::busy;
        if (!f.finished())
            return false;
    } else if (kind == "ERROR") {
        std::string_view text;
        if (!f.last(text) || text.size() > 200U)
            return false;
        d.state = AutoChopState::error;
        d.text = std::string(text);
    } else {
        return false;
    }
    status = std::move(d);
    return true;
}

} // namespace midichopper::plugin
