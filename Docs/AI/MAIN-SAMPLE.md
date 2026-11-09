# Main sample and pad links

Contract for one long source sample whose regions (slices) are linked to pads.
It follows the [vision](../../SMS-AnvilleSampler/Docs/VISION.md): "Keep audio
sources, slices, pad assignments, visual layouts, and MIDI mappings as separate
concepts", and adds the source identifier that the
[Cut Point Editor limitation](CHOP-EDITOR.md#limitation) asks for. Upstream
issues #1 (whole-recording window) and #54 (auto slice) motivate it.

## Pad links (provenance)

Each pad still owns a private copy of its slice audio, so the Sample Editor,
export, clipboard, splits, Adjust Cut Points and saved state work unchanged.
The link is a tag on the pad's immutable `SampleStoragePool::Storage`:
`provenance = (sourceId48 << 16) | sliceId16`, where 0 means unlinked.

- The tag belongs to the PCM. It is set before publication and never changes
  for published audio except through `setPadProvenance`.
- Settings edits (Start/End, ADSR, mixer) keep the tag. Generation is not a
  link identity: every settings change bumps it, so it only guards races.
- Whole-pad moves (collapse, split shifts) move the `Storage*`, so the tag
  follows. Everything that installs new storage starts unlinked: recording
  (`acquireCapture` resets reused capture descriptors), import, paste, clear,
  Adjust Cut Points and split halves.
- `restorePad(pad, data, provenance)` tags the prepared storage before it is
  queued; `PendingState` carries the `Storage*`, so no extra field is needed.
- `padProvenance` reads the newest tags (pending restores first) and, on
  request, generations from the same published snapshot.
- `padAudioFingerprint` and the free `fingerprint(PadData)`
  ([PadFingerprint.hpp](../../SMS-AnvilleSampler/src/core/PadFingerprint.hpp))
  hash the frame count, the rate in whole hertz and the bit patterns of up to
  4096 evenly spaced stereo frames with FNV-1a. `setPadProvenance` tags the
  newest audio only when its fingerprint matches; provenance 0 unlinks.
- `SampleStoragePool::copySamples(storage, firstFrame, out)` reads a frame
  range from retained imported or capture storage and zero-fills past the end.
- A rebuilt bounded pool (`setSampleRate`) re-imports pads and keeps their tags.

## Atomic imports

`SamplerEngine::importPads(span<const PadImport>, expectedSettings)` replaces
or clears (`audio == nullptr`) up to 64 distinct pads in one transaction,
mirroring `rechopPads`. It prepares storage on the control thread, then one
dispatch re-checks Arm, recording, each optional `expectedGeneration`,
capacity (imported pad storage counts against `total_blocks_` exactly as in
`rechopPads`) and the layout/MIDI mapping. Only then does it publish every pad
with its tag and supplied settings. On failure nothing changes and prepared
storage is collected. Publication hard-stops a sounding pad, so a re-cut cuts
that pad's voice; this is intended, not hidden.

## Source

`setSource(audio, sourceId)` stores one long source as imported storage
(`prepareImport`, capped at `kMaximumPadFrames`, rates above 96 kHz stored at
96 kHz); `nullptr` clears it and audio needs a nonzero id. The engine holds one
reference, and the pointer swap runs in a dispatch, so the old source is freed
on the control thread. `restoreSource` queues the same swap for durable state,
like `restorePad`; readers see the queued source at once. Source storage does not count against pad capacity
(`total_blocks_`); it survives collection and a bounded-pool rebuild.

Readers pin the source under the control mutex (a reference plus a copy of its
id, frames, rate, peak and RMS), unlock, read and release. Only collection,
which also runs under the mutex, deletes storage, so a long scan never blocks
pad workflows. `copySourceRange(id, first, count, out)` and
`summarizeSourceRange(id, start, end, tiles)` refuse a stale id or an
out-of-range request; copies measure their own peak and RMS, and 1 to 4 tiles
split the range evenly, binned like `summarizeStereo`. `exportSource` copies
the whole source and returns its id.

## Source preview

`startSourcePreview(id, start, end, loop)`, `stopSourcePreview` and
`sourcePreviewPosition` run on the audio thread only. The preview shares the
chop preview's linear interpolation (`mixInterpolated`) but reads `source_`
with a double frame position over `[start, end)`, so it is frame-exact on long
sources; `PlaybackVoice` is not used. A loop wraps exactly and interpolates its
last frame toward the loop start. Starting either raw preview stops the other.
Arm, `stopAllPlayback`, `reset`, `setSampleRate` and `setSource` (inside its
swap dispatch) stop it; a stale id, Arm or a bad range starts nothing. It
counts in `anyPlaybackActive`, not toward `maxVoices`, does not stop pad
voices, and the playback gain applies. The position is 0 when stopped,
otherwise 1 plus frame / source frames.

## Protocols

[MainSampleProtocol.hpp](../../SMS-AnvilleSampler/src/plugin/MainSampleProtocol.hpp)
holds MS1 requests (`L` path, `C`, `Q`, `S` one or two slice edits, `A` assign,
`D` delete, `W` 1 to 4 tiles), the one-per-operation MS1 status, MW1 tiles
(WS2 summaries) and MP1 preview commands.
[MainSampleSession.hpp](../../SMS-AnvilleSampler/src/plugin/MainSampleSession.hpp)
holds the slice model (at most 64 slices with ids 1 to 65535, sorted by start)
and the durable MSS1 session and LK1 link codecs. Frames are `uint32`, like
the engine. A path, name or message is the last field and may contain `;`.
Names are cut to 128 bytes, and status text to 512, on UTF-8 boundaries. The
worst-case status and four-tile waveform fit under 8191 bytes.
[Main sample workflows](MAIN-SAMPLE-WORKFLOWS.md) covers the keys and requests; [UI](MAIN-SAMPLE-UI.md) the view.

## Tests

`main-sample-core` covers capture reuse, ranged copies, fingerprints, tags
through restores, settings edits, collapse, import, paste, clear and
recording, `importPads` failures (stale generation, duplicate pad,
capacity, layout, Arm), the source (capacity, collection, rebuild, ranges,
tiles, rates, freeing) and its preview (exact one-shot, loop and rate-converted
output, stale ids, exclusivity and every stop). `sampler-transfer` checks that
64-pad `importPads` commits, source swaps and preview start, render and stop
allocate and free nothing on the audio thread. `main-sample-protocol` covers
round-trips, malformed input, UTF-8 cuts and message sizes.
