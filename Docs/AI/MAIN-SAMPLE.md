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

## Tests

`main-sample-core` covers capture reuse, ranged copies, fingerprints, tags
through restores, settings edits, collapse, import, paste, clear and
recording, and `importPads` failures (stale generation, duplicate pad,
capacity, layout, Arm). `sampler-transfer` checks that 64-pad `importPads`
commits allocate and free nothing on the audio thread.
