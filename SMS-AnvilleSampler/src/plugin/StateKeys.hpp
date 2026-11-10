#pragma once

#include "Configuration.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace midichopper::plugin {

inline std::array<std::string, midichopper::kPadCount> makePadStateKeys(const char* const prefix)
{
    std::array<std::string, midichopper::kPadCount> keys;
    for (std::uint32_t pad = 0; pad < midichopper::kPadCount; ++pad) {
        char key[24];
        std::snprintf(key, sizeof(key), "%s%02u", prefix, pad + 1U);
        keys[pad] = key;
    }
    return keys;
}

inline const auto kPadStateKeys = makePadStateKeys("pad_");
inline const auto kPadEditStateKeys = makePadStateKeys("pad_edit_");
inline const auto kPadMixerStateKeys = makePadStateKeys("pad_mix_");
inline const auto kPadColorStateKeys = makePadStateKeys("pad_color_");

inline constexpr std::array<const char*, 5> kGlobalControlStateKeys{{
    "global_output_settings", "global_pan_settings", "global_tune_settings",
    "global_lowpass_settings", "global_highpass_settings",
}};

inline constexpr const char* kWaveformRequestKey = "waveform_request";
inline constexpr const char* kWaveformDataKey = "waveform_data";
inline constexpr const char* kWaveformDetailRequestKey = "waveform_detail_request";
inline constexpr const char* kWaveformDetailDataKey = "waveform_detail_data";
inline constexpr const char* kPadClearRequestKey = "pad_clear_request";
inline constexpr const char* kPadFileRequestKey = "pad_file_request";
inline constexpr const char* kPadFileBusyKey = "pad_file_busy";
inline constexpr const char* kPadFileStatusKey = "pad_file_status";
inline constexpr const char* kPadClipboardRequestKey = "pad_clipboard_request";
inline constexpr const char* kChopApplyRequestKey = "chop_apply_request";
inline constexpr const char* kChopStatusKey = "chop_status";
inline constexpr const char* kChopSnapshotRequestKey = "chop_snapshot_request";
inline constexpr const char* kChopSnapshotDataKey = "chop_snapshot_data";
inline constexpr const char* kChopPreviewRequestKey = "chop_preview_request";
inline constexpr const char* kChopMidiPreviewKey = "chop_midi_preview";
inline constexpr const char* kPadStructureRequestKey = "pad_structure_request";
inline constexpr const char* kPadStructureStatusKey = "pad_structure_status";
inline constexpr const char* kPlayStopRequestKey = "play_stop_request";
// Main Sample: one long source whose slices link to pads (Docs/AI/MAIN-SAMPLE.md).
inline constexpr const char* kMainSampleRequestKey = "main_sample_request";
inline constexpr const char* kMainPreviewRequestKey = "main_preview_request";
inline constexpr const char* kMainSampleStatusKey = "main_sample_status";
inline constexpr const char* kMainSampleWaveformKey = "main_sample_waveform";
// Durable Main Sample state; an empty value clears (Docs/AI/MAIN-SAMPLE-WORKFLOWS.md).
inline constexpr const char* kMainSampleAudioKey = "main_sample_audio";
inline constexpr const char* kMainSampleSessionKey = "main_sample_session";
inline constexpr const char* kMainSampleLinksKey = "main_sample_links";
// Auto-chop (AC1): request to the worker, status to the UI (Docs/AI/AUTO-CHOP.md).
inline constexpr const char* kAutoChopRequestKey = "auto_chop_request";
inline constexpr const char* kAutoChopStatusKey = "auto_chop_status";
// Undo/redo: request "undo"|"redo" to the worker; UD1 status (depths, colours) to the UI.
inline constexpr const char* kUndoRequestKey = "undo_request";
inline constexpr const char* kUndoStatusKey = "undo_status";
// Main Sample spectrogram background (MG1), worker to UI.
inline constexpr const char* kMainSampleSpectrogramKey = "main_sample_spectrogram";

} // namespace midichopper::plugin
