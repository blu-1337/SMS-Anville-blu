# Main sample workflows

Plugin side of the [main sample](MAIN-SAMPLE.md) contract.
[MainSampleWorkflows](../../SMS-AnvilleSampler/src/plugin/MainSampleWorkflows.hpp)
runs MS1 requests on the control thread and publishes the replies.

## Keys

| Key | Flags | Path |
| --- | --- | --- |
| `main_sample_request` | DSP-only, transient | `setState` transient list, then the control worker (VST3) or `applyState` directly, then `handleRequest`. `getState` returns `""`. |
| `main_preview_request` | DSP-only, transient | Audio-thread mailbox; see [preview](MAIN-SAMPLE-PREVIEW.md). |
| `main_sample_status` | UI-only | One MS1 status per request; a load publishes LOADING first. |
| `main_sample_waveform` | UI-only | The MW1 reply to `W`. |
| `main_sample_audio` | Base64 blob, DSP-only, durable | SSP1 v2 float32 of the stored source. |
| `main_sample_session` | DSP-only, durable | MSS1. |
| `main_sample_links` | DSP-only, durable | LK1, built at save time. |

The transient, then the durable keys follow the five global keys
(`kMainSampleStateOffset`); `initPluginState` tests them before the global
branch, so no earlier index moves. If the worker cannot run a request, `reportControlFailure` publishes
ERROR "Main sample action failed; retry" without a session.

## Locking

Requests share `PadWorkflows::mutationMutex()` with pad workflows, durable state
reads and `sampleRateChanged`. Two steps run without it: `L` decodes the WAV
first (`readPadWav`, the same formats and 8-minute cap), and `W` reads through
the engine's source pin. `latestLoadSequence_` records the newest `L` or `C`;
a load that finishes decoding after a newer one started publishes nothing more.

## Requests

- `L`: LOADING with the file name, decode, lock, `setSource` under a fresh
  48-bit id (SplitMix64 of the steady clock and a counter, never the current
  id), then reset the session with the stored frames and rate (sources above
  96 kHz are stored at 96 kHz) and publish READY. A read or storage failure
  publishes ERROR and keeps the old source.
- `C`: clear the source and session, then EMPTY. `Q`: READY, or EMPTY.
- `S`: apply one or two edits to a session copy, re-cut the linked pads, then
  commit the copy.
- `A`: add a slice and cut it onto the pad with default editor and mixer
  settings, like a WAV import, tagged with the new slice. An occupied pad that
  is not linked to this source gets CONFIRM (the mask names the pad) unless
  `confirm` is 1. A pad linked to this source is replaced directly, and its old
  slice stays. Arm or a recording pad gives "Disarm recording first"; a 65th
  slice is refused.
- `D`: remove the slice and unlink its pads (`setPadProvenance` with 0 after a
  fingerprint read). They keep their audio and settings.
- `W`: 1 to 4 tiles of the stored source; a stale source or range gives ERROR
  "Waveform unavailable".

## Re-cuts

`S` snapshots provenance and generations. For each pad tagged with the source
and an edited slice it copies the slice (`copySourceRange`) into a `PadImport`
with the same tag, the snapshot generation and the pad's envelope and mixer.
Start/End reset to 0..1 because the markers now define the trim. Settings are
read after the snapshot, so a newer edit fails the generation check instead of
being lost. One `importPads` commits every pad against the current layout. On
failure (Arm, a stale generation, recording, capacity) the session is unchanged
and the status is ERROR "Pads changed, try again".

After a commit, affected pads' editor keys (for `A` also mixer keys) are
published before the status, as after a chop apply.

## Status semantics

Every status carries the session (id, frames, rate, slices) and the links:
pads whose newest tag names the current source and an existing slice. Pads keep
stale tags after `C`, `L` or a failed unlink; these are not listed and never
match a new id. `affectedMask` names the pads re-cut, assigned or unlinked, or
the pad a CONFIRM asks about. ERROR and CONFIRM never change the session, so
the UI keeps its model and shows the text. READY carries the source name.

## Durable state

Without a source the durable keys save `""` (clear). Audio Base64 is cached
per source id (seeded by a restore, dropped by `L`/`C`). LK1 lists pads whose
newest tag names the current source and an existing slice, with frames and
`padAudioFingerprint`.

Restore takes the mutation mutex and accepts any key order: VST3 and CLAP send
keys alphabetically (`main_*` first), LV2 in index order (pads first). Audio and
session wait until both are present with matching frames and rate; then
`restoreSource` queues the source (suspended callbacks never stall it) and the
session is adopted with one sequence-0 status. A links value replaces the staged
entries and tags pads whose audio already matches. Each `pad_NN` restore passes
a matching entry's tag to `restorePad`; other links drop quietly. An empty audio
or session value clears both; invalid values change nothing.

Known limitation: an old preset loaded into a live instance never sends the
missing `main_*` keys, so the previous main sample stays until **Clear**.

## Tests

`pad-workflows` loads real WAV files and covers the empty query, malformed
requests, unreadable files, LOADING then READY, waveform tiles and refusals,
new ids on reload, clear, assign (empty, foreign with CONFIRM,
linked), single and shared-boundary re-cuts that
keep envelope and mixer, refusals while armed, delete, the slice cap, and one
status per request. `global-value-mode-*` checks the key order and hints.
`project-state-*` covers key orders, suspended callbacks, dropped links, clears,
mismatches, the cache and old fixtures; `main-sample-core` covers `restoreSource`.
