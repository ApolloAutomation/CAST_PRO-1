# i2s_audio (full duplex)

ESPHome 2026.9.1's `i2s_audio` with full duplex support from esphome/esphome#19959
(head `04d4759`), so the PCM5122 DAC and PCM1808 ADC can share one I2S port
(common WS/BCLK/MCLK, separate DOUT/DIN).

Only the PR's own changes are applied, on top of 2026.9.1. The PR is built on
ESPHome dev, and dev's other `i2s_audio` changes need a newer esp-audio-libs and
`audio_dac` than 2026.9.1 has. One PR hunk is left out: a guard in the speaker's
`setup()` that skips parking the data pin on a full duplex bus. 2026.9.1 doesn't
park the pin there, so there's nothing to skip.

Delete this folder and the `external_components` block in Core.yaml once #19959
ships in an ESPHome release.
