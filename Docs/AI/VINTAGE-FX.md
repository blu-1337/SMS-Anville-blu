# Vintage FX (1.2.91: DSP and state; UI follows in 1.2.92)

Two 12-bit sampler colours. UI names: model 1 = "GSP1200 Sampler" (drum), model 2 = "GS950 Unit" (rack). Research and constants: `handoff/research/vintage-fx.md`; code: [VintageSettings](../../Common-Src/DSP/VintageSettings.hpp), [VintageProcessor](../../Common-Src/DSP/VintageProcessor.hpp).

## Settings

`sms::dsp::VintageSettings` (floats, `sanitize` clamps, non-finite -> default):

| Field | Range | Default |
| --- | --- | --- |
| model | 0 Off, 1 drum, 2 rack | 0 |
| voicing (drum only) | 0 Sweep, 1 Dark, 2 Mid, 3 Open, 4 Bright, 5 Raw | 3 |
| bits | 4-16 | 12 |
| rateHz | 4000-48000 | 26041.67 (`defaultRateHz`: rack 40000; the UI resets Rate on a model change) |
| driveDb | 0-24 | 0 |
| toneOctaves | -2..2 (scales every model cutoff) | 0 |
| mix | 0-1 | 1 |
| resonance (rack only) | 0-1, 0 = authentic non-resonant Butterworth | 0 |

## Chain and placement

Processor: drive (level-compensated soft clip) -> bus pre-filter -> mid-tread quantiser -> hold at `rateHz` with 2-point polyBLEP (wet one sample late, dry delayed to match) -> model filter -> dry/wet. Drum: Dark..Bright = one-pole + 2 SVF table, Sweep = 4 one-poles opening to 18 kHz per hit and closing to 1 kHz (70 ms), Raw = none. Rack: 6th-order Butterworth at rate/2.5 x 2^tone; resonance raises the last stage Q to 8 and trims level (peak about +5 dB).

- **Pad** (`SampleMixerSettings::vintage`): first on the pad, in `PlaybackVoice::render` right after the read, before pad/global filters, envelope, gain and pan. While on, the read is 2-tap linear (pitched playback keeps aliasing); the sinc and linear reads crossfade over 10 ms on a live change. `start` triggers Sweep; a retrigger keeps filter states. Not applied by Export Processed (like LP/HP/DIRTY).
- **Global**: `SamplerEngine::setGlobalVintage` (any non-audio thread, seqlock of 8 atomics, adopted per block). One bus instance (with pre-filter) on main and on each of the 16 aux outputs, on the summed buses after the voices, before raw previews and output gain. An idle bus (100 ms digital silence, tail < -120 dB) clears its state and costs two compares per frame.

## Real-time rules

No allocation, lock, I/O or throw in `setTarget`/`process`; all state is fixed members. Bits, rate (log), drive, tone, resonance ramp 20 ms with coefficients every 16 samples; mix ramps per sample. A model, voicing or Off/On (or Mix 0) change crossfades old and new chains over 10 ms; further changes wait. Off and Mix 0 are exact bypasses after the fade (tested bit-identical). States below 1e-20 are flushed.

## State

- Pad: mixer codec **MX8** = MX7's 12 floats + the 8 fields above; MX1-MX7 decode Off. Engine `Pad` has 8 `mixerVintage*` atomics. Every path that moves whole `SampleMixerSettings` (clipboard, swap, gap collapse, split shift, undo, history imports) moves vintage like the other mixer fields; TUNE (TN1) and quick edits (QE1) edit the live settings and keep it. The UI re-sends what it decoded (MX8), so it cannot reset vintage.
- Global: key `global_vintage` (last state, hints 0, default and garbage = Off), value `VF1;model;voicing;bits;rate;drive;tone;mix;resonance`. Not a host parameter, not undoable.

Tests: `vintage-processor` (research checks, engine Off bit-identity, aux, zero allocation, CPU print), `playback-voice`, `state-codec`, `pad-workflows`, `project-state`.
