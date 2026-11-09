# Main sample workflows

Plugin side of the [main sample](MAIN-SAMPLE.md) contract.
[MainSampleWorkflows](../../SMS-AnvilleSampler/src/plugin/MainSampleWorkflows.hpp)
runs MS1 requests on the control thread and publishes the replies.

## Keys

| Key | Flags | Path |
| --- | --- | --- |
| `main_sample_request` | DSP-only, transient | `setState` transient list, then the control worker (VST3) or `applyState` directly, then `handleRequest`. `getState` returns `""`. |
| `main_preview_request` | DSP-only, transient | Registered; inert until the preview mailbox lands. |
| `main_sample_status` | UI-only | One MS1 status per request; a load publishes LOADING first. |
| `main_sample_waveform` | UI-only | The MW1 reply to `W`. |

The keys are appended after the five global keys (`kMainSampleStateOffset`),
and `initPluginState` tests them before the global branch, so no earlier index
moves. If the worker cannot run a request, `reportControlFailure` publishes
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

After a commit, each affected pad's editor key is published before the status
(`A` adds the mixer key), as after a chop apply. Each slice feeds its own pad,
so this stays far below the 64-message UI bus.

## Status semantics

Every status carries the session (id, frames, rate, slices) and the links:
pads whose newest tag names the current source and an existing slice. Pads keep
stale tags after `C`, `L` or a failed unlink; these are not listed and never
match a new id. `affectedMask` names the pads re-cut, assigned or unlinked, or
the pad a CONFIRM asks about. ERROR and CONFIRM never change the session, so
the UI keeps its model and shows the text. READY carries the source name.

## Tests

`pad-workflows` loads real WAV files and covers the empty query, malformed
requests, unreadable files, LOADING then READY, waveform tiles and refusals,
new ids on reload, clear, assign onto empty and foreign pads (CONFIRM, then
confirmed), reassigning a linked pad, single and shared-boundary re-cuts that
keep envelope and mixer, refusals while armed, delete, the slice cap, and one
status per request. `global-value-mode-*` checks the key order and hints.
