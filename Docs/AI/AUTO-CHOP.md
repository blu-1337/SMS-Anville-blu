# Auto-chop and analysis

Audience: AI agents. Read when changing onset detection, tempo estimation, slice selection or the auto-chop request.

Upstream issue #54 ("Auto slice on Transient or Note") asks for this feature. It builds on the Main Sample source and slices described in [Main sample](MAIN-SAMPLE.md).

## Analysis module

`Common-Src/Analysis/` builds the static library `sms_analysis`. It has no DPF or plug-in dependency and compiles with strict warnings. Namespace: `sms::analysis`.

- `SplitMix64.hpp`: seeded generator, so tests and Re-roll are deterministic.
- `RealFft.hpp/.cpp`: radix-2 real FFT returning magnitudes. Tables are built in the constructor. One instance is not thread-safe.

Analysis runs once per source on the control worker and is never saved. Selection reuses the cached result.
- `OnsetAnalysis.hpp/.cpp`: `analyzeOnsets` mixes to mono, decimates by an integer factor towards 22.05 kHz (box average, no sinc FIR), runs a 1024-point Hann FFT at hop 256 and stores log-compressed spectral flux, per-hop RMS and three band magnitudes. Flux is divided by max(99th percentile, `fluxFloor`), so silence and steady tones never reach onset level. Candidates are local maxima over 3 hops, refined at full rate: the search runs from one hop before the window centre to its right edge, because log-compressed flux peaks as an attack enters that edge. The first 64-frame block above 20% of the floor-to-peak range wins, then the previous zero crossing within 2 ms. `pickOnsets` applies the sensitivity threshold and minimum gap (greedy by strength) without rerunning the FFT.
- `TempoEstimate.hpp/.cpp`: autocorrelation of the novelty curve, scanned in 0.25 BPM steps with linear lag interpolation and a log-Gaussian prior at 120 BPM (sigma one octave).

A cancelled analysis has `cancelled` set and must not be cached.

## Slice strategies

`SliceStrategies.hpp/.cpp` selects `SliceRegion{start,end}` frames from a cached `OnsetAnalysis`. The `SliceContext` carries the range, `minFrames`, `maxFrames` (`kMaximumPadFrames`) and the BPM. Results are sorted, clamped, and may be fewer than requested; nothing is invented.

- Transient: first N onsets in time order; slice i ends at onset i+1, the last at the range end.
- Find Samples: onsets at medium sensitivity, each 1, 2 or 4 beats long and ending before the next strong onset. Score is onset strength times length fit times RMS; regions under -40 dBFS are rejected. Greedy selection subtracts half the largest band-cosine similarity to picked regions, forbids overlap, requires one beat between starts and at most ceil(N/4) picks per range quarter. A nonzero seed jitters scores by 10%.
- Equal: N equal parts; remainder spread by integer division.
- Beat: boundaries `anchor + round(i * step)` computed cumulatively, so there is no drift; optional snap to an onset within 30 ms.
- Random: seeded starts, optional snap within a quarter beat, bounded retries to avoid overlap.

Beat-based modes return nothing when the BPM is 0.

## Host tempo

`DISTRHO_PLUGIN_WANT_TIMEPOS` makes `run()` store the host BPM (0 when the host gives none) in a lock-free `std::atomic<double> hostBpm_`. It adds no parameter or port: VST3 only gains process-context requirements, and LV2 already has the atom input. This is a separate commit; without it the Host button stays hidden.
