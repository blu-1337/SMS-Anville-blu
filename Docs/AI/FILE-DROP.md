# File drop onto pads

Contract for dropping WAV files from the host or desktop onto pads. It reopens
[issue #20](https://github.com/sirsipe/SMS-Plugins/issues/20): the pinned
DPF/pugl has no external file-drop API (`PUGL_DATA_OFFER` is clipboard-only),
so drops use a native per-window target in
[Common-UI](../../Common-UI/FileDropTarget.hpp).

## Interface

`sms::ui::FileDropTarget::attach(nativeWindow, listener)` registers one editor
window; null means "drop unavailable" (no platform support or registration
failed) and changes nothing else. Destroy the target before the window; the
listener outlives it. `FileDropListener` gets `fileDragOver` (returns accept),
`fileDragLeave` and `filesDropped` on the UI thread, with physical client
pixels (the DPF mouse space) and UTF-8 paths. `filesDropped` only queues work.
`releaseDroppedFile(path)` deletes a private copy and ignores any other path.

## Drop effect rule

[`FileDropEffect.hpp`](../../Common-UI/FileDropEffect.hpp) is pure and tested
everywhere.

- `chooseDropEffect(allowed, accept)`: COPY if allowed, else MOVE, else LINK,
  else NONE; NONE when rejected. **Never write COPY unconditionally.** OLE
  treats an effect outside the source's mask as "no drop" and ends the drag in
  `DragLeave`: the known JUCE failure with FL Studio Edison, whose drags appear
  to allow MOVE only.
- `reportedDropEffect(chosen)`: COPY for COPY, else NONE. A reported MOVE asks
  the source to delete its original; NONE after a successful read is the
  documented optimized-move answer, so the source keeps its file.
- `dropNeedsPrivateCopy(chosen)`: MOVE and LINK sources may delete the file as
  soon as `Drop` returns, before the worker reads it.
- `isSupportedAudioPath`: `.wav`/`.wave`, any case, non-empty name.

## Windows target

[`FileDropTargetWin32.cpp`](../../Common-UI/FileDropTargetWin32.cpp) implements
`IDropTarget`. OLE calls it on the registering thread, the host UI thread that
also delivers DPF events, so nothing needs locking.

- `attach` balances `OleInitialize` (including `S_FALSE`) in the destructor.
  `RPC_E_CHANGED_MODE` returns null and logs once (`OutputDebugStringA`), as
  does a failed `RegisterDragDrop`. The destructor clears the listener,
  revokes, releases, then uninitializes; a late OLE call rejects the drag.
- `DragEnter` reads up to 1024 `CF_HDROP` paths with exact `DragQueryFileW`
  lengths and strict UTF-8 conversion; invalid names are skipped, not replaced.
  `DragEnter`/`DragOver` apply `ScreenToClient`, ask `fileDragOver`, and answer
  `chooseDropEffect(*pdwEffect, accept)`.
- `Drop` re-reads its own data object. COPY passes the originals. MOVE/LINK
  copies each supported WAV with `CopyFileW` to
  `%TEMP%\SMS-Anville\drops\<pid>-<counter>.wav`, dated now (`CopyFileW` keeps
  the old time), and passes the copies. It answers `reportedDropEffect` and
  ends with exactly one of `filesDropped` or `fileDragLeave`.
- COM methods catch exceptions and return `E_UNEXPECTED` with no effect.
- `attach` deletes copies older than 24 hours; release deletes only
  `<digits>-<digits>.wav` files in that folder.

## Controller rules

[`FileDropController`](../../SMS-AnvilleSampler/src/ui/FileDropController.hpp)
is DPF-free (`anvillesampler_ui_controllers`).

- **Accept** a visible pad (`padTarget`) when a path is a supported WAV.
  **Reject** while armed or capturing, in the Cut Point Editor, while a pad
  action is busy, a dialog is pending or a batch is loading, and for
  `mainSample` targets (no such view yet). Accepted hovers highlight the pad.
- **Order:** files keep the source order and fill consecutive visible pads
  from the drop pad (`advanceVisiblePad`) across banks, skipping hidden slots.
  At most 64; files past the last pad are released at once and the batch ends
  with "Loaded k of n files".
- **Confirmation:** one file replaces its pad without asking, like Import. A
  batch that would replace another occupied pad, or one in an unseen bank,
  first arms; the same file count dropped on the same pad within 5 seconds
  confirms. This follows the click-again confirmations; there is no modal.
- **FIFO:** `idle` sends one file through `PadActionController::importFile`
  (the Import dialog's request) when nothing is busy, in flight or pending.
  Completion is the import result, not the busy flag, which can arrive late.
  Failures advance; arming stops the rest.
- **Release:** every path is released once: rejected files at once, imported
  files on their result. Destruction leaves the in-flight copy to the sweep.

## Editor wiring

`MidichopperUI` attaches to `getWindow().getNativeWindowHandle()` at the end of
its constructor, retries from `onUiIdle` while the handle is 0, and resets the
target first in its destructor. Drop points go through the mouse transform
(`toLogicalPosition` minus `contentOffsetX`) and `targetAt`, so open menus and
non-pad controls reject drops. `ViewState::dropHoverPad` draws a 2 px
`controlAccent` outline in the main and Sample Editor grids.

## Limitations

- Windows only; macOS and Linux (XDND) compile the null target.
- Only WAV formats the Import dialog reads; other files are ignored.
- Batch confirmation cannot see other banks' occupancy, so it asks.

## Tests and manual check

`ui-file-drop` covers every effect mask and drives the COM object with an
in-memory `CF_HDROP` source (MOVE-only, COPY, rejection, private copy and
release, UTF-8, real-window registration, sweep, revocation).
`ui-file-drop-controller` covers acceptance, bank/layout mapping, hidden slots,
gating, late busy flags, overflow, confirmation and release. `ui-geometry`
maps window points to pads at scales 1.0, 1.5 and 2.0.

In FL Studio on Windows: drag from Edison (MOVE-only), the FL browser and
Explorer onto a pad, then several files across a bank boundary. Check the
highlight, the load, that originals survive, and that `%TEMP%\SMS-Anville\drops`
empties. [Platform builds](PLATFORM-BUILDS.md) list host checks.
