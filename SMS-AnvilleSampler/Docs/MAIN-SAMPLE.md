# Main Sample

Load one long sample, mark slices on its waveform, and assign slices to pads.
Pads keep their own copy, so you can keep editing them after the Main Sample
is replaced or cleared.

Assigning: drag a selection and click a pad, or press the MIDI note of a pad. A
note on an empty pad assigns the selection and makes no sound; a note on a loaded
pad opens the usual overwrite confirmation and the pad still plays (it previews
what would be replaced). Dragging edges and selections snaps to other slice
boundaries within 8 screen pixels; the SNAP button next to FIT turns that off
(UI-local, on by default). Holding Alt or Shift during a drag turns snapping off for fine-tuning. Wheel-click and drag pans a zoomed waveform; right-drag
inside a selection moves its nearer edge.

## Open and close

Press **SAMPLE** beside **EDITOR** on the main view, or choose **Main
Sample...** in the hamburger menu (disabled while armed). **Close** or
**Escape** returns. Double-clicking a pad opens its Sample Editor on top of
this view; closing the editor brings you back here.

## Load

- **Load** opens the file browser for a WAV. **Clear** removes the sample.
- Drag a WAV onto the waveform or overview. With several files only the first
  is used.
- The side panel shows the name, length, sample rate and slice count, plus
  the current status or error.

The Main Sample is saved with the project. Pads linked to it stay linked when
the project is reopened.

## Waveform

The large waveform shows the visible part of the sample; the strip above
the pads shows the whole sample with a box for the visible part.

- **Ctrl + wheel** zooms around the pointer. **Shift + wheel** scrolls. Drag
  the overview to scroll.
- Click an empty spot to preview from there to the end of its slice.
  **Space** starts or stops the preview (the selection, or the whole sample).
  The playhead follows the preview. Pads stay playable during it.

## Slices

- Each marker is a slice edge, drawn with the slice number and the colour of
  its pad. Drag a marker to move it; the change is sent when you release.
  A slice always keeps at least one frame.
- Click a marker to select its slice and press **Delete** to remove it.
- Drag across the waveform to select a range.

## Pads

Click a pad to hear it. With a selection, click a pad (or release the drag
over one) to cut that range onto it. If the pad already holds other audio,
the view asks you to **Confirm** or **Cancel** before replacing
it. Linked pads show their slice number. Use the bank buttons to reach other
pads.

Double-click an occupied pad to open its Sample Editor.

## Auto chop

Auto chop cuts the main sample into pads in one step (issue #54). Pick a mode, a first pad and a count, then apply:

- **Transient** cuts at detected hits by overall loudness; **Transient 2** compares each frequency band on its own, so a soft hi-hat after a snare gets its own slice; **Find Samples** picks the most useful 1, 2 or 4 beat parts; **Equal** divides the range evenly; **Beat** cuts every N beats from an anchor; **Random** picks seeded parts.
- Beat and Random use the tempo you enter, or the host tempo when it is 0. Without either you get "No tempo".
- Pads are filled from the first pad upward. If a pad already holds other audio you are asked to confirm; pads that already belong to this main sample are replaced without asking.
- Analysis runs once per loaded sample and is not saved with the project.

A pad linked to a slice also moves the playhead through that slice when it is played from MIDI or a pad click (most recent voice).
Drag a slice's number badge to move the whole region (length kept, clamped to the source, snaps with SNAP on); release sends one edit that re-cuts its linked pads and is one undo step.
