# Guitar Tuner FX for Schwung

A chromatic tuner as an **audio FX** for
[Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. Put it
in a Signal Chain and it listens to whatever reaches it, showing the nearest
note, the cents deviation and a needle in the chain editor.

- **Audio passes through untouched** by default (bit-for-bit), so it can stay
  in your chain while you play
- **Mute Output** silences the output for private tuning, with a 10 ms fade
  each way so switching never clicks. It keeps tuning while muted
- **Tuner page** (the page you land on): the note in big letters, accidental
  and octave, cents and Hz, a needle with ticks at 10 and 25 cents that fills
  its centre when you are within 3 cents, an input-level meter, and a
  **MUTED** tag while the output is muted
- **Note cell** on the Main page: the same reading as a small meter
- Sharps or flats (**Acc** knob)
- Chromatic, about 70 to 1200 Hz (guitar low E to well above the top string)

Based on [Guitar Tuner](https://github.com/eightfour-dev/schwung-guitar-tuner),
a Schwung tool module. The pitch detector is the same; the module around it is
rewritten as a chainable audio effect.

## Using it

Put **Line In** in a slot and add **Guitar Tuner FX** as an Audio FX after it.
Plug a guitar into the 3.5 mm input (or play near the Move's mic) and open
the FX: you land on the **Tuner** page.

To tune without the room hearing you, turn **knob 1 (Mute)** on. MUTED shows
at the top right until you turn it off again.

It also works anywhere else audio flows through a chain: on a Move track's
slot with Global Settings > Audio > **Move->Schwung** on (to tune what that
track plays), after a synth, or in Master FX.

### Pages

| Page | What it shows | Knobs |
|---|---|---|
| **Tuner** | note, cents, Hz, needle, input level, MUTED | Mute · Acc · Note |
| **Main** | the knob grid, with the reading as a small meter in the Note cell | Mute · Acc · Note |

| Knob | Values | Notes |
|---|---|---|
| Mute Output | Off / On | Off (the default) passes audio through untouched. On fades the output to silence over 10 ms. Saved with the set |
| Accidentals | Sharps / Flats | how notes are spelled: C# or Db |
| Note | read-only | the current reading, e.g. `E2 +3.4c 82.6Hz` when touched |

### Reading the display

- **Big letter**: the nearest note, with its accidental (superscript) and
  octave (subscript). A4 is 440 Hz.
- **Needle**: left of centre is flat (tune up), right is sharp (tune down).
  Full deflection is 50 cents. In tune (under 3 cents off), the centre window
  fills solid.
- **Level ladder** (right edge): the input peak. If it moves but no note
  shows, the signal is under the noise gate or has no clear pitch.
- A reading stays up for 1.5 s after the note fades, so it doesn't flicker
  out at the end of each pluck.

## Install

### Via Schwung Manager

Once published in the Module Catalog: open `http://move.local:7700`, find
**Guitar Tuner FX** under Audio FX, and install.

### Build from source

```bash
./scripts/build.sh            # Docker cross-compile -> dist/guitar-tuner-fx-module.tar.gz
./scripts/install.sh          # copy to ableton@move.local (MOVE_HOST=... to override)
```

The module installs to `/data/UserData/schwung/modules/audio_fx/guitar-tuner-fx/`.
With an aarch64 cross-compiler already installed, skip Docker:
`CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh`.

Needs Schwung **1.6.2** or newer (for `page_first`, which makes the Tuner
page open first).

## Development

```bash
bash tests/run_tests.sh       # DSP, served contract, help text, canvas drawers
SCHWUNG_DIR=../schwung node tools/preview.mjs --png build/preview
```

`tools/preview.mjs` renders the pages with a Schwung checkout's own planner
and renderer, using readings you choose (`--tuner "A2 -12.3c 108.5Hz"`,
`--mute 1`, `--level -20`, `--touch 0`).

### How it works

- `process_block` mixes the incoming block to mono and appends it to a
  2048-sample ring *before* applying Mute, so muting never affects tuning.
- Every 512 samples (~11.6 ms) the ring is snapshotted. Below the RMS gate
  that is the whole cycle; otherwise the YIN difference function is computed
  in four slices, one per block, so no single audio block carries the whole
  ~1.1M multiply-adds. Then CMNDF, the absolute threshold (0.15), parabolic
  refinement, and a confidence check.
- The DSP owns the 1.5 s hold, and clears the reading when the host stops
  calling `process_block` (an idle or bypassed FX), so the display never
  freezes on a stale note.
- The reading is one string, `tuner`, declared `live` so the chain editor
  re-reads it every frame. The DSP serves its own `chain_params` and
  `ui_hierarchy` (generated from `src/module.json` at build time), because the
  chain host's module.json fallback drops `viz`, `live` and `access`.

### Release

1. Update `version` in `src/module.json`
2. `git commit -am "bump to vX.Y.Z"`
3. `git tag vX.Y.Z && git push --tags`
4. GitHub Actions runs the tests, builds, attaches
   `guitar-tuner-fx-module.tar.gz` to the release, and rewrites `release.json`
   on `main` for Schwung Manager.

## License

MIT, see [LICENSE](LICENSE).

## AI Assistance Disclaimer

Developed with AI assistance. All architecture, implementation, and release
decisions are reviewed by human maintainers. AI-assisted content may still
contain errors, so please validate functionality, security, and license
compatibility before production use.
