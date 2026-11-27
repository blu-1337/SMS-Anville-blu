# Pad sound detection

Note, chord and key of one pad's trimmed region, shown in the header panel ([user guide](../../SMS-AnvilleSampler/Docs/SOUND-ANALYSIS.md)).

## Analysis contract

[`analyzeSound()`](../../Common-Src/Analysis/SoundAnalysis.hpp) is DPF-free (`sms_analysis`). It takes interleaved stereo at 8k-192k, allocates, and runs on a worker only. It polls `cancel`; a cancelled result is neither published nor cached. Silent audio returns `silent` with every estimate invalid. Notes are fractional MIDI against A440; [`NoteNames.hpp`](../../Common-Src/Analysis/NoteNames.hpp) formats them with C4 = 60. Tests: [`sound_analysis_tests.cpp`](../../SMS-AnvilleSampler/tests/sound_analysis_tests.cpp).

## Protocol

State keys `pad_analysis_request` (DSP only) and `pad_analysis_status` (UI only) end the state table; append later keys after them. Both read back empty and are never saved. [Codec](../../SMS-AnvilleSampler/src/plugin/PadAnalysisProtocol.hpp):

- Request: `AQ1;seq;pad`, pad 0-63; the UI picks the pad.
- Status: `AR1;seq;pad;generation;state;noteMidi;noteConf;flags;chordRoot;quality;chordConf;keyTonic;minor;keyConf;runnerTonic;runnerMinor;tune`. `tune` is the pad's TUNE (-24 to 24); the result is always of the untuned audio.
- `state`: `R` ready, `B` pad is recording, `N` empty pad or empty region, `E` worker failure or full queue (generation 0).
- `noteMidi` is `-` without a note. `flags` is `-` or letters in this order: `a` approximate, `g` gliding, `r` range limited, `v` chord varies.
- Chord root and key tonic are pitch classes (0 = C), -1 when invalid. `quality` is the `ChordQuality` value, 0 exactly when the root is -1.
- Confidences are 0-1. `silent`, `hz` and the tuning offset are not sent.

Decoders reject malformed input; a malformed request gets no reply.

## Service and threading

[`PadAnalysisService`](../../SMS-AnvilleSampler/src/plugin/PadAnalysisService.hpp) handles requests on the instance `ControlWorker`.

- It copies the pad with `exportPublishedPad` under the pad mutation mutex, then releases the mutex before hashing and analysing. It never dispatches to the audio thread, so it works while the host is not processing and sees staged restores.
- Teardown calls `cancel()` before stopping the worker. Cancellation is permanent.
- Formats without direct access (all but VST3) have no instance worker; `setState` analyses on the calling thread.

## Cache

One in-memory entry per pad, keyed by trim start/end and a hash of every analysed sample and the rate. Not by storage generation: mixer edits move it without changing audio, and a staged host restore does the opposite. So TUNE and its undo are served from the cache. Reverse is ignored. Nothing is persisted.

## Header panel

[Rects](../../SMS-AnvilleSampler/src/ui/MidichopperLayout.hpp): `analysisPanel` {560,25,576,32}, between the logo glow and the PLAY LED, in every view. Skin title translations much longer than the default would push the logo under it. [Renderer](../../SMS-AnvilleSampler/src/ui/AnalysisPanelRenderer.cpp). ON/OFF is `analysis` in the user defaults file ([preference](../../SMS-AnvilleSampler/src/plugin/AnalysisPreference.hpp)), default OFF.

## Requests

[`PadAnalysisSession`](../../SMS-AnvilleSampler/src/ui/PadAnalysisSession.hpp) follows the last played pad, else the selected one.

- Changes are debounced 150 ms. One request is in flight; replies with another sequence or pad, or outdated by a later change, are dropped. No reply for 10 s resends.
- The UI has no pad generation, so these mark the pad changed: target change, occupancy, capture start/end, trim edit or host-pushed pad settings (all show `...` except the last), and every `undo_status` (load, chop, paste, move, undo), which keeps the shown values until the reply.
- A new way of replacing pad audio must reach one of these.

## TUNE

- [Math](../../SMS-AnvilleSampler/src/ui/TuneTarget.hpp) returns an absolute pad tune from raw note + current tune, so repeating it cannot double-apply. The note list is modal.
- It targets the analysed pad, and only while the result is ready and not stale (any change above, until its reply).
- The write is `TN1;semitones` on `pad_mix_NN` ([codec](../../SMS-AnvilleSampler/src/plugin/PadQuickEditCodec.hpp)): the UI may not know the other settings of a pad that is not selected. [`applyPadTune`](../../SMS-AnvilleSampler/src/plugin/UndoController.hpp) records it as an action, never coalesced, and publishes the full mixer.
- The UI's tune follows that pad's local edits and pushed mixer state, else the `AR1` value.

## Limits

- At most the first 30 s of the trimmed region.
- Analysis occupies the control worker: later transient commands (undo, file actions) wait for it, and nothing cancels a superseded request. Requesters should debounce.
- The whole pad is copied even for a short region.
