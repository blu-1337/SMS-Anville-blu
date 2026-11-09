# Choke groups

`SampleMixerSettings::chokeGroup` (0 = off, 1..8) is stored in `MX5`; `MX1` to `MX4` decode as off.
`SamplerEngine::startVoice` calls `chokeGroupMembers`, which gives other playing pads in the
group a `PlaybackVoice::choke` fade (`kChokeFadeSeconds`, linear, freed at the end). Choking
voices are not counted by `enforceVoiceLimit`. The UI cycles the group in
`SamplerUiSession::cycleSampleChokeGroup`. Tests: `choke_groups` in `core_tests.cpp`.
