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
- Without state transport the session never opens the view or sends.

## Waveforms

The overview is one `W` over the whole source (4 tiles) per source id. The
detail is a `W` for the zoomed viewport, debounced 50 ms. Both retry every
250 ms until a reply with the same sequence and range arrives.

## Editing

- Marker drags are local (`displaySlices()`). `endDrag()` sends exactly one
  `S`: the dragged slice, plus the neighbour when they share the boundary.
  Every slice keeps at least one frame.
- `assignSelection(pad)` sends `A`. A CONFIRM whose mask is that pad sets
  `confirmPending()`; `confirmAssign(true)` resends with confirm.
- `deleteSlice(id)` sends `D` for a known slice.
- `previewFrom(frame)` previews to the end of the containing slice, or of
  the source. The playhead is `kParameterMainSamplePreviewPosition`.

## View state

`SamplerUiState::mainSampleMode` sits below the Cut Point Editor and Sample
Editor, so closing either returns to it. Rects are in `MidichopperLayout.hpp`
(`mainSampleRects`). The side panel keeps y 154-380 free for auto-chop.
Drawing, input and the entry button come with the renderer.

Tests: `tests/main_sample_ui_tests.cpp` (CTest `main-sample-ui`).
