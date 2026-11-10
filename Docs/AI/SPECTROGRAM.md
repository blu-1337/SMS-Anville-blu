# Main Sample spectrogram

`analyzeOnsets` also stores 48 log bins (30 Hz-10 kHz, 8-bit, -84..0 dBFS) per hop, reusing the cached FFT frames. Request verb `G` (`MS1;seq;G;start;end;columns`, columns <= 120) makes the worker max-pool hops into columns and publish `main_sample_spectrogram` as `MG1;seq;start;end;cols;<base64>` (about 7.7 KB worst case, under the 8191 B bus limit). The UI uploads a texture only when data changes and draws it by its own frame range. Transport index 6 is the toggle.
