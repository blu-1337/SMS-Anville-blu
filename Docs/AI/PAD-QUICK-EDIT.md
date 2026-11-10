# Pad quick edit

Dragging a Main-view pad sets its Gain (vertical, top full and bottom muted) or Pan
(horizontal). A press still plays the pad; the gesture starts after 7 logical pixels and
the first dominant axis stays locked until release (`src/ui/PadQuickEdit.hpp`).

While dragging, volume lowers the pad's solid fill from the top down and pan paints a red
gradient on that side (red left, dark red right). The value is drawn on the pad
(`VOL 50%` / `-6.0 dB` / `MUTE`; `50% L` / `C` / `30% R`).

The drag is live: the first move sends `QE1;<gainDb|->;<pan|->;B` on the pad's `pad_mix_NN`
key, then `;U` at most every 60 ms when the value moved, and `;E` on release
(`src/plugin/PadQuickEditCodec.hpp`). A drag that ends before its first send, and a
Ctrl+click reset (0 dB and centre, no playback), send one unsuffixed (`S`) message. The
plugin applies each message to the pad's live mixer settings through
`restorePadMixerSettings` (queued for the next audio block, so a note played mid-drag uses
the in-progress value) and publishes the mixer blob. `UndoController::recordGesture` captures
the state at `B` and pushes one step at `E` only if something changed. The UI keeps no copy
that could overwrite other knobs.
