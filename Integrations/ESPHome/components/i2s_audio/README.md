# i2s_audio (full duplex)

ESPHome 2026.9.0's `i2s_audio` with full duplex support, so the PCM5122 DAC and
PCM1808 ADC can share one I2S port (common WS/BCLK/MCLK, separate DOUT/DIN).

Based on esphome/esphome#16882 (head `3db4594`), with these changes:

- The speaker joins a TX channel the microphone already started without
  skewing its playback timestamps (Sendspin sync), and realigns its write
  position each session since TX is never reset.
- The DMA ring is fixed at the speaker's 5 x 10 ms, whichever side allocates
  first, so stall tolerance doesn't depend on start order.
- TX always auto-clears, so the DAC plays silence rather than looping the last
  buffer when the microphone allocated the channels first.
- Both channels get both data pins, whichever side initializes first.
- Mismatched speaker/microphone formats are rejected at config time and at
  runtime, instead of playing at the wrong speed.

Drop this override once full duplex lands upstream.
