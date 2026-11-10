# SMS-Anville Sampler fork: key changes

Fork: https://github.com/blu-1337/SMS-Anville-blu
Base: `sirsipe/SMS-Plugins` `main` @ `3d3ae5b`. Six stacked branches, each builds and passes tests on its own (Windows/MSVC; Linux not built locally).
Pick any branch or patch series; later branches depend on earlier ones.

## 1. `feature/file-drop` (issue #20)
- Drag WAVs onto any pad from Explorer, FL browser and **FL Edison**.
- Native Win32 OLE `IDropTarget` (DPF has no file-drop API). The drop effect is chosen from the source's allowed mask, which is why Edison (MOVE-only) works. MOVE is never reported back, so a source file is never deleted.
- Multi-file drop fills the following visible pads. Non-Windows builds compile a no-op.

## 2. `feature/main-sample` (issue #1)
- New **Main Sample view**: one long source, zoomable waveform, slice markers, preview playhead.
- Slices are **linked** to pads through a provenance tag on pad storage. Moving a marker re-cuts the linked pads in one atomic engine transaction (`importPads`).
- The source is embedded in the project state (SSP1 float32), with no file paths. Link restore is order-independent (VST3 alphabetical vs LV2 index order).
- Old projects load unchanged; new state keys are appended only.

## 3. `feature/auto-chop` (issue #54)
- One-click auto-chop: Transient, Find Samples, Equal, Beat (1/16–4 beats, triplets), Random.
- New DPF-free `Common-Src/Analysis` library: real FFT, spectral-flux onsets with zero-cross refinement, tempo estimate. It runs once per source on the worker and is cached.
- Host tempo via `DISTRHO_PLUGIN_WANT_TIMEPOS`, in its own commit so it can be dropped.

## 4. `feature/choke-groups`
- Per-pad choke group (Off/1–8) with a 5 ms fade, so there are no clicks. Codec bump; old pads decode as Off.

## 5. `feature/multi-out` (issue #5): optional prototype
- 16 auxiliary stereo outputs and a per-pad OUTPUT setting. The main bus is unchanged. Skip this branch if you prefer a separate variant plugin.

## 6. `feature/main-sample-ux2`: workflow and UX
- **Undo/redo**: bounded history (50 steps / 256 MiB) covering pad audio, settings, colours and slice edits. It runs off the audio thread.
- **Editing**:
  - Snap to region edges and zero crossings (Alt/Shift bypasses).
  - Drag a whole region by its badge.
  - Right-drag edges.
  - Live re-cut while dragging.
- **Auto-chop**: new **TRANSIENT 2** mode, a multi-band onset detector for soft hi-hats. On a hard synthetic test, recall went from 9/28 to 25/28 hats.
- **Spectrogram**: optional spectrogram behind the waveform.
- **Pads**:
  - **Favourites**: a starred pad is protected from auto-chop, overwrite and delete.
  - Pad swap by drag.
  - Volume/pan drag gestures on pads, with Ctrl+click to reset.
- **Per-pad settings**:
  - HOLD **Loop on/off** (new pads default Off; old pads keep looping).
  - CHOKE/OUT drop-downs in the Sample Editor header.
- **MIDI**:
  - A MIDI note can assign a selection to a pad.
  - The display follows the bank being played.
  - The playhead follows MIDI playback.
- **Defaults**: Max Voices 16 for new instances.

## Engineering rules followed
- The audio thread never allocates, locks or does I/O. All heavy work runs on the control worker.
- Parameters and state keys are never reordered. Codec versions are bumped, and old blobs decode to the previous behaviour.
- New logic lives in new files; existing files get small hooks. Docs are updated within the word limits, and `check_docs.py` passes.
- Not verified: Linux/LV2 builds, and most features have not had a manual FL Studio pass yet.

Details: `REVIEW-GUIDE.md`. Patches: `patches/01-…06-…`.


# SudoMetalStudio Plugins

_Professionally developed audio plugins for **Linux** (VST3/LV2) and **Windows** (VST3)._

## Plugins

### [SMS-Anville Sampler](SMS-AnvilleSampler/README.md) — live stereo chopping sampler

[Read More!](SMS-AnvilleSampler/README.md)

[<img src="SMS-AnvilleSampler/Docs/main-view.png" width="672" alt="Default Main View">](SMS-AnvilleSampler/Docs/main-view.png)

#### Alternative skins
| Finnish Dream | Looks like Wu | White Cadillac Vinyl |
| --- | --- | --- |
| [<img src="SMS-AnvilleSampler/Docs/main-view-finnish-dream.png" width="224" alt="Finnish Dream Main View">](SMS-AnvilleSampler/Docs/main-view-finnish-dream.png) | [<img src="SMS-AnvilleSampler/Docs/main-view-looks-like-wu.png" width="224" alt="Looks like Wu Main View">](SMS-AnvilleSampler/Docs/main-view-looks-like-wu.png) | [<img src="SMS-AnvilleSampler/Docs/main-view-white-cadillac-vinyl.png" width="224" alt="White Cadillac Vinyl Main View">](SMS-AnvilleSampler/Docs/main-view-white-cadillac-vinyl.png) |

## About the developer & this project
> Hi,
>
> I'm Simo "Sipe" Erkinheimo, a professional software engineer with an M.Sc. in Information Technology
> and roughly two decades of experience in software development, architecture, and product engineering.
>
> ### 🤖🤮 Yet another f*cking AI-generated project?
>
> There's a strong anti-AI sentiment in parts of the creative and software development communities.
> I understand many of the concerns, and even share some of them. The barrier to creating software
> has never been lower, and many communities are now flooded with mediocre projects.
> People are understandably getting tired of it.
>
> I use AI-assisted development tools extensively. AI significantly speeds up implementation, but it
> doesn't replace engineering experience, architecture, testing, judgment, or responsibility for the end result.
>
> So, is this another slop project? **Absolutely not.**
>
> What matters isn't whether AI was used, but the quality of the result and the commitment to maintaining it.
>
> ### My commitment
>
> My name is real, and you can easily find my professional background and development history.
>
> All SMS-Plugins source code is publicly available, including its development history. You're welcome to inspect it, review it, and challenge my engineering decisions.
>
> **Whether a line of code was written by me or generated by AI, the responsibility for what I publish is entirely mine.**
>
> ### And how is this funded?
>
> It isn't. 😅💸
>
> At least for now, this is a 100% pro bono hobby project, driven by my passion for music, software engineering, and building tools I'd actually want to use myself.
>
> I choose **quality over quantity**. Slow and steady. I only build what I'm willing and able to maintain, with a focus on usability and simple, clear functionality.
>

## License

Copyright (c) 2026 SudoMetalStudio.

Repository content (excluding [third_party](third_party/) content): [AGPL-3.0-only](LICENSE).

> **NOTE:** This project was developed extensively with AI assistance, which introduces some legal uncertainty regarding copyright protection for AI-generated contributions.
>
> While I retain copyright over my original creative contributions, the extent to which individual AI-generated portions qualify for copyright protection remains legally unsettled.
>
> Since copyleft obligations rely on copyright, this uncertainty may also affect the enforceability of the AGPL license for certain portions of the code.
>
> My intention is simple: I built something useful for the community, and I want anyone building upon it to keep their improvements available to that community.
>
> If you discover a legal loophole and deliberately exploit it against that intention, you're a dick. 🫵

Additional permission: [DAW host-linking exception](HOST-LINKING-EXCEPTION.txt).
Third-party licenses remain unchanged, but listed for convenience.

Bundled dependencies:

| Component | License |
| --- | --- |
| DPF/DGL | [ISC](third_party/DPF/LICENSE) |
| travesty (VST3) | [ISC](third_party/DPF/distrho/src/travesty/base.h) |
| LV2 headers | [ISC](third_party/DPF/distrho/src/lv2/lv2.h) |
| CLAP headers | [MIT](licenses/CLAP-MIT.txt) |
| Pugl | [ISC](third_party/DPF/dgl/src/pugl-upstream/COPYING) |
| NanoVG / Fontstash | [zlib](third_party/DPF/dgl/src/nanovg/LICENSE.txt) / [zlib](third_party/DPF/dgl/src/nanovg/fontstash.h) |
| stb_image | [Public-domain; fallback permission](third_party/DPF/dgl/src/nanovg/stb_image.h) |
| stb_truetype | [MIT or public-domain dedication](third_party/DPF/dgl/src/nanovg/stb_truetype.h) |
| libSOFD (X11 file dialog) | [MIT](third_party/DPF/distrho/extra/sofd/libsofd.c) |
| DejaVu Sans font | [Bitstream Vera / Arev terms; DejaVu changes public domain](third_party/DPF/dgl/src/resources/LICENSE-DejaVuSans.ttf.txt) |
