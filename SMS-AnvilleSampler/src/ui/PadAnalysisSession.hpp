#pragma once

// UI side of pad sound detection: when to send AQ1 and which AR1 to show.
// DPF-free (Docs/AI/SOUND-ANALYSIS.md).

#include "Analysis/NoteNames.hpp"
#include "EditorSnapshotSession.hpp"
#include "../plugin/PadAnalysisProtocol.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace midichopper::ui {

/** What the header panel shows. */
struct PadAnalysisView {
    bool on = false;
    bool busy = false;  // waiting for a result, or the pad is recording
    bool valid = false; // result belongs to the followed pad
    sms::analysis::SoundAnalysis result{};
};

/** Readout text; field 0 note, 1 chord, 2 key. "~" marks a weak estimate. */
[[nodiscard]] inline std::string analysisReadout(const PadAnalysisView& view, const int field)
{
    if (!view.on) return "-";
    if (view.busy) return "...";
    if (!view.valid) return "-";
    if (field == 0) return sms::analysis::noteText(view.result.note); // adds its own "~"
    const auto& chord = view.result.chord;
    const auto& key = view.result.key;
    const bool weak = field == 1 ? chord.valid && chord.confidence < 0.75F
                                 : key.valid && key.confidence < 0.75F;
    return (weak ? "~" : "") +
        (field == 1 ? sms::analysis::chordText(chord) : sms::analysis::keyText(key));
}

/** Confidence behind the dot: the note's, else the chord's, else the key's; 0 without any. */
[[nodiscard]] inline float analysisConfidence(const PadAnalysisView& view) noexcept
{
    if (!view.on || view.busy || !view.valid) return 0.0F;
    const auto& result = view.result;
    return result.note.valid ? result.note.confidence
         : result.chord.valid ? result.chord.confidence
         : result.key.valid ? result.key.confidence : 0.0F;
}

/** At most one request is in flight, so fast pad changes never queue up on the worker. */
class PadAnalysisSession {
public:
    using Time = std::chrono::steady_clock::time_point;
    static constexpr auto kDebounce = std::chrono::milliseconds(150);
    static constexpr auto kReplyTimeout = std::chrono::seconds(10);

    [[nodiscard]] const PadAnalysisView& view() const noexcept { return fView; }
    [[nodiscard]] int pad() const noexcept { return fPad; }

    void setOn(const bool on) noexcept
    {
        *this = {};
        fView.on = on;
    }

    /** The followed pad's content may have changed. A quiet change keeps the shown values. */
    bool invalidate(const Time now, const bool showBusy) noexcept
    {
        if (!fView.on || fPad < 0) return false;
        fDirty = true;
        fDue = now + kDebounce;
        if (!showBusy || fView.busy) return false;
        fView.busy = true;
        return true;
    }

    /** A reply may have been lost: ask again. */
    void retry(const Time now) noexcept
    {
        if (!fInFlight) return;
        fInFlight = false;
        fDirty = true;
        fDue = now;
    }

    /** Follows `pad` (-1 none) and returns the request that is due, if any. */
    [[nodiscard]] std::optional<plugin::PadAnalysisRequest> poll(const int pad, const Time now,
                                                                bool& changed) noexcept
    {
        changed = false;
        if (!fView.on) return std::nullopt;
        if (pad != fPad) {
            fPad = pad;
            fView.valid = false;
            fView.busy = false;
            fDirty = false;
            changed = true;
            if (pad >= 0) static_cast<void>(invalidate(now, true));
            // A reply still in flight belongs to the old pad; its pad number rejects it.
        }
        if (fInFlight && now >= fTimeout) retry(now);
        if (!fDirty || fInFlight || fPad < 0 || now < fDue) return std::nullopt;
        fDirty = false;
        fInFlight = true;
        fTimeout = now + kReplyTimeout;
        fSequence = nextEditorSnapshotSequence();
        return plugin::PadAnalysisRequest{fSequence, static_cast<std::uint32_t>(fPad)};
    }

    /** True when the shown values changed; stale and outdated replies are dropped. */
    bool accept(const plugin::PadAnalysisStatus& status) noexcept
    {
        if (!fInFlight || status.sequence != fSequence) return false;
        fInFlight = false;
        // The pad or its content changed after this request: the next one replaces it.
        if (fDirty || static_cast<int>(status.pad) != fPad) return false;
        fView.busy = status.state == plugin::PadAnalysisState::busy;
        fView.valid = status.state == plugin::PadAnalysisState::ready;
        fView.result = status.result;
        return true;
    }

private:
    PadAnalysisView fView{};
    int fPad = -1;
    bool fDirty = false;
    bool fInFlight = false;
    std::uint64_t fSequence = 0U;
    Time fDue{};
    Time fTimeout{};
};

} // namespace midichopper::ui
