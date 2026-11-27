# Pad sound detection

Note, chord and key of one pad's trimmed region. There is no UI yet.

## Analysis contract

[`analyzeSound()`](../../Common-Src/Analysis/SoundAnalysis.hpp) is DPF-free (`sms_analysis`). It takes interleaved stereo at 8k-192k, allocates, and runs on a worker only. It polls `cancel`; a cancelled result is neither published nor cached. Silent audio returns `silent` with every estimate invalid. Notes are fractional MIDI against A440; [`NoteNames.hpp`](../../Common-Src/Analysis/NoteNames.hpp) formats them with C4 = 60. Tests: [`sound_analysis_tests.cpp`](../../SMS-AnvilleSampler/tests/sound_analysis_tests.cpp).

## Protocol

State keys `pad_analysis_request` (DSP only) and `pad_analysis_status` (UI only) end the state table; append later keys after them. Both read back empty and are never saved. [Codec](../../SMS-AnvilleSampler/src/plugin/PadAnalysisProtocol.hpp):

- Request: `AQ1;seq;pad`, pad 0-63; the UI picks the pad.
- Status: `AR1;seq;pad;generation;state;noteMidi;noteConf;flags;chordRoot;quality;chordConf;keyTonic;minor;keyConf;runnerTonic;runnerMinor`.
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

One in-memory entry per pad, keyed by storage generation, trim start/end and a hash of every analysed sample. The hash is needed because a staged host restore replaces audio without moving the generation. Mixer edits also move the generation, so they recompute. Reverse is ignored. Nothing is persisted.

## Limits

- At most the first 30 s of the trimmed region.
- Analysis occupies the control worker: later transient commands (undo, file actions) wait for it, and nothing cancels a superseded request. Requesters should debounce.
- The whole pad is copied even for a short region.
