# File drop onto pads

Implemented contract for dropping WAV files from the host or the desktop onto
pads. It reopens [issue #20](https://github.com/sirsipe/SMS-Plugins/issues/20),
which dropped drag-and-drop because the pinned DPF/pugl has no external
file-drop API (`PUGL_DATA_OFFER` is clipboard-only). Drops therefore use a
native per-window target in [Common-UI](../../Common-UI/FileDropTarget.hpp).

## Interface

`sms::ui::FileDropTarget::attach(nativeWindow, listener)` registers one editor
window and returns null when the platform has no implementation or
registration fails. Callers treat null as "drop unavailable" and change nothing
else. Destroy the target before the window. The listener outlives the target.

`FileDropListener` receives `fileDragOver` (returns accept), `fileDragLeave` and
`filesDropped` on the UI thread. Points are physical client pixels, the same
space as DPF mouse events, so the adapter maps them with the mouse transform.
Paths are UTF-8. `filesDropped` must only queue work.

`releaseDroppedFile(path)` deletes a private copy (see below). It ignores every
path outside the private drop folder, so callers may pass any completed path.

## Drop effect rule

[`FileDropEffect.hpp`](../../Common-UI/FileDropEffect.hpp) is pure and tested on
every platform.

- `chooseDropEffect(allowed, accept)` returns COPY if the source allows it, else
  MOVE, else LINK, else NONE; NONE when not accepted. **Never write COPY
  unconditionally.** OLE treats an effect outside the source's allowed mask as
  "no drop", and the drag then ends in `DragLeave` instead of `Drop`. This is
  the known failure of JUCE plugins with FL Studio Edison, whose drags appear to
  allow MOVE only.
- `reportedDropEffect(chosen)` returns COPY for COPY and NONE otherwise. A
  reported MOVE asks the source to delete its original (an unoptimized move).
  NONE after a successful read is the documented optimized-move answer, so the
  source keeps its file.
- `dropNeedsPrivateCopy(chosen)` is true for MOVE and LINK. Such sources may
  delete or reuse the file as soon as `Drop` returns, before the worker reads it.
- `isSupportedAudioPath` accepts `.wav` and `.wave`, case-insensitively, with a
  non-empty file name.

## Windows target

[`FileDropTargetWin32.cpp`](../../Common-UI/FileDropTargetWin32.cpp) implements
`IDropTarget`. OLE calls it on the thread that registered the window, which is
the host UI thread that also delivers DPF events, so the listener needs no
locking.

- `attach` calls `OleInitialize` and balances it in the destructor, including
  the `S_FALSE` case. `RPC_E_CHANGED_MODE` (a multithreaded UI thread) returns
  null and logs once with `OutputDebugStringA`. `RegisterDragDrop` failure
  returns null. The destructor clears the listener, revokes, releases the COM
  object, then uninitializes; a late OLE call after detach rejects the drag.
- `DragEnter` reads `CF_HDROP` (at most 1024 paths) with exact
  `DragQueryFileW` lengths. Paths convert to UTF-8 with
  `WC_ERR_INVALID_CHARS`; an invalid name is skipped rather than replaced.
- `DragEnter` and `DragOver` convert the screen point with `ScreenToClient`,
  ask `fileDragOver`, and answer `chooseDropEffect(*pdwEffect, accept)`.
- `Drop` re-reads the paths from its own data object. For COPY it passes the
  originals. For MOVE or LINK it copies each supported WAV with `CopyFileW` to
  `%TEMP%\SMS-Anville\drops\<pid>-<counter>.wav`, refreshes the copy's write
  time (`CopyFileW` keeps the source's), and passes the copies. The answer is
  `reportedDropEffect`. A drop ends with exactly one of `filesDropped` or
  `fileDragLeave`.
- Every COM method catches exceptions and returns `E_UNEXPECTED` with
  `DROPEFFECT_NONE`.
- `attach` deletes private copies older than 24 hours, left by a crash or an
  unfinished batch. `releaseDroppedFile` deletes only files in that folder
  named `<digits>-<digits>.wav`.

Tests drive the COM object with an in-memory `CF_HDROP` source: MOVE-only,
COPY-preferred, rejected and null-effect cases, the private copy and its
release, UTF-8 round trips, registration on a real window, the stale sweep,
and revocation.
