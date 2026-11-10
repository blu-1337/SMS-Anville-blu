# Main Sample view controller

`src/ui/MainSampleController.hpp/.cpp` is the view side of
[Main sample](MAIN-SAMPLE.md). It is DPF-free, lives in
`anvillesampler_ui_controllers` and is owned by `SamplerUiSession`, which
implements `MainSampleHost` (send state, repaint). Requests are MS1 on
`main_sample_request` and MP1 on `main_preview_request`; see
[workflows](MAIN-SAMPLE-WORKFLOWS.md) and [preview](MAIN-SAMPLE-PREVIEW.md).

## Mirror

- The first `idle()` after construction and every `open()` send `Q`.
- `acceptStatus` ignores a sequence older than the newest accepted one.
  Sequence 0 is a restore broadcast: the next `idle()` sends `Q`, so links
  tagged by later `pad_NN` keys appear.
- LOADING, ERROR and CONFIRM update only the state, text and mask; the
  mirrored source and slices stay. ERROR also re-queries, which undoes a
  refused optimistic edit.
- A new source id or frame count resets the viewport, waveforms, drag,
  selection and pending confirm.
- `pad_mask` from the plug-in calls `padsChanged`: one `Q` 250 ms later.

## Waveforms

The overview is one `W` over the whole source (4 tiles). The
detail is a `W` for the zoomed viewport, debounced 50 ms. Both retry every
250 ms until a matching reply arrives.

## Editing

- Marker and badge drags are live: the first move sends `S;...;B`, then `;U` at most
  every 80 ms, and `endDrag()` sends `;E` (`S` alone if nothing was sent). The linked
  pad is re-cut each time, so a note played mid-drag sounds the dragged range, and
  `UndoController::recordGesture` makes the drag one undo step. Replies cannot
  disturb the drag: it clamps and builds requests from `fDragBase`, the slices at
  the press. Each `S` holds the dragged slice, plus the neighbour on a shared
  boundary. `cancelDrag()` after a send restores the range with an `E`.
  - `assignSelection(pad)` sends `A`. A CONFIRM whose mask is that pad sets
  `confirmPending()`; `confirmAssign(true)` resends with confirm.
- `deleteSlice(id)` sends `D` for a known slice.
- `previewFrom(frame)` previews to the end of the containing slice, or of
  the source. The playhead is `kParameterMainSamplePreviewPosition`.

## View, input

`mainSampleMode` sits below the Cut Point and Sample Editors, so closing
either returns here. The SAMPLE button or menu item opens it; Escape or
Close leaves it. 

`MainSampleRenderer.cpp` draws (at most 512 columns, 64 markers). Targets
(`mainSample*`) resolve in `interactiveTargetAt`, precedence chop, editor,
Main Sample, main; CONFIRM captures all.

Gestures: empty waveform previews, Space toggles, drag selects, a pad click
assigns or auditions, a pad double-click opens the Sample Editor, marker
drag edits, Delete removes, Ctrl+wheel zooms, Shift+wheel pans. Load uses
the pad file browser. Dropping a WAV on the waveform or overview loads
the first (`Kind::mainSample`).

Tests: `main-sample-ui`, `ui-input-routing`.
