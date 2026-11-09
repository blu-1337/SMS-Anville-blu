# Auto-chop and analysis

Audience: AI agents. Read when changing onset detection, tempo estimation, slice selection or the auto-chop request.

Upstream issue #54 ("Auto slice on Transient or Note") asks for this feature. It builds on the Main Sample source and slices described in [Main sample](MAIN-SAMPLE.md).

## Analysis module

`Common-Src/Analysis/` builds the static library `sms_analysis`. It has no DPF or plug-in dependency and compiles with strict warnings. Namespace: `sms::analysis`.

- `SplitMix64.hpp`: seeded generator, so tests and Re-roll are deterministic.
- `RealFft.hpp/.cpp`: radix-2 real FFT returning magnitudes. Tables are built in the constructor. One instance is not thread-safe.

Analysis runs once per source on the control worker and is never saved. Selection reuses the cached result.
