# Main Sample preview path

How a view previews a Main Sample range and shows its playhead. The engine
side (`startSourcePreview`, interpolation, mutual exclusion with the chop
preview) is in [Main sample](MAIN-SAMPLE.md); the transient keys are in
[workflows](MAIN-SAMPLE-WORKFLOWS.md).

## Request

`main_preview_request` carries `MP1;<play 0|1>;<loop 0|1>;<idHex>;<start>;<end>`.
It follows the `chop_preview_request` path and never reaches the control
worker:

1. `MidichopperPlugin::applyState` hands it to `PluginControls::queueStateCommand`.
2. `decodeSourcePreviewRequest` validates it. Invalid values are consumed and
   dropped.
3. A valid value is published to `RealtimeLatestMailbox<SourcePreviewRequest>`.
   An unread value is replaced, so a stop sent after a play wins.
4. `PluginControls::applyToEngine` consumes it in `run()` and calls
   `startSourcePreview` or `stopSourcePreview`. It runs after the chop preview
   mailbox, so a Main Sample play in the same block wins the shared slot.

The engine rejects a stale source id, an empty or out-of-range span, and any
play while armed. The view learns this only from the playhead.

## Playhead

`kParameterMainSamplePreviewPosition` (`main_sample_preview_position`) is a
hidden output parameter appended after `kParameterCaptureStorageExhausted`
(index 72, range 0..2). `run()` publishes `sourcePreviewPosition()`:
0 while idle, else `1 + frame / sourceFrames` over the whole source, not
the previewed range.

Appending is safe for VST3: DPF gives plug-in parameter `i` the id
`kVst3InternalParameterCount + i`, and its internal and MIDI-CC ids form a
fixed block before them. `vst3_state_tests.cpp` asserts this, and
`core_tests.cpp` pins every existing index.

## Tests

`preview_mailbox_tests.cpp` drives `PluginControls` into a real
`SamplerEngine`: play, stop, latest-wins, invalid and stale requests, no
replay, and the parameter index and range.
