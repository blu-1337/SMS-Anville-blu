# Sound detection

The header panel, left of **PLAY**, shows the note, chord and key of one pad's
trimmed region: the last played pad, or else the selected pad.

Click **OFF** to switch detection on. The choice is remembered for your user
account, not saved in projects. It stays available in every view.

- **NOTE**: pitch name and cents against A440 (C4 is middle C).
- **CHORD** and **KEY**: shown when the material supports an estimate.
- **~** marks an approximate or low-confidence value.
- **...** means busy: analysing, or the pad is recording.
- **-** means there is nothing to show, for example an empty pad.
- The dot is green for high confidence, yellow for medium and grey otherwise. It
  follows the note, or else the chord or key.

Values update after the pad, its audio or its Start/End changes. Mixer settings
such as TUNE and reverse playback do not change what is detected. At most the
first 30 seconds of the region are analysed.

The **TUNE** button and its arrow in this panel are not available yet.
