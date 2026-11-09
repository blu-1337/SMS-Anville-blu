# Multi-out

`DISTRHO_PLUGIN_NUM_OUTPUTS` is 34: ports 0 and 1 are the main stereo group; ports 2..33 are 16
stereo groups (ids 1..16, `initPortGroup`). VST3 marks them `kAudioPortIsSidechain`, so DPF
exposes aux buses that are inactive by default; disabled buses receive DPF's dummy buffer.
`SampleMixerSettings::outputBus` (0 = main, 1..16) is stored in `MX6` (`MX1` to `MX5` decode as
main). The plugin passes `outputs + 2` to `SamplerEngine::setAuxOutputs` for each block; the
engine renders a pad into its bus accumulator instead of the main mix and writes every aux
buffer (global gain applied). Without aux buffers every pad plays on main. Tests:
`multi_output_routing` in `core_tests.cpp`, `audio_port_tests.cpp`.
