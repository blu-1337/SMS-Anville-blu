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

Values update after the pad, its audio or its Start/End changes. Detection
listens to the stored audio; reverse playback and mixer settings do not change
it. **NOTE** then adds the pad's own TUNE, so it shows what the pad sounds like.
CHORD and KEY stay those of the stored audio, and global TUNE is not included.
At most the first 30 seconds of the region are analysed.

## Tune a pad

Both controls change the TUNE knob of the pad shown in the panel, which may not
be the selected pad.

- **TUNE** moves the sounding note to the nearest semitone, so NOTE reads +0c.
- The arrow opens a list of notes. Picking one tunes the pad to that note in the
  nearest octave (a tie goes down). If that would pass the TUNE range of -24 to
  +24 semitones, the next octave inside it is used.
- A click outside the list or **Escape** closes it without doing anything else.

Each use is one **Undo** step. Both controls are greyed out while detection is
off or refreshing, without a note, for a very uncertain **~** note, for a
favorite pad and in the Cut Point Editor.
