# Pad quick edit

Dragging a Main-view pad sets its Gain (vertical, top full and bottom muted) or Pan
(horizontal). A press still plays the pad; the gesture starts after 7 logical pixels and
the first dominant axis stays locked until release (`src/ui/PadQuickEdit.hpp`).

While dragging, volume lowers the pad's solid fill from the top down and pan paints a red
gradient on that side (red left, dark red right). Release sends one
`QE1;<gainDb|->;<pan|->` value on the pad's `pad_mix_NN` key
(`src/plugin/PadQuickEditCodec.hpp`). The plugin applies it to the pad's live mixer
settings through `restorePadMixerSettings` as one non-coalescing undo step, then publishes
the full mixer blob back. Nothing is sent mid-drag and the UI keeps no copy that could
overwrite other knobs.
