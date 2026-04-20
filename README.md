# Guitar Tuner for Schwung

A chromatic tuner for [Schwung](https://github.com/charlesvestal/schwung) on
Ableton Move. Uses the Move's internal mic or 3.5mm line-in to detect pitch
and shows the nearest note, cents deviation, a needle meter, and a big
letter glyph on the pad grid.

## Features

- Chromatic detection across the guitar range (~70–1200 Hz) via YIN pitch
  tracking on a 2048-sample window
- OLED shows note name + signed cents + a live needle meter
- Pad grid lights up a 3×4 letter glyph with a sharp/flat accidental beside it
- Color grading by accuracy: green (locked), white (close), amber (off),
  red (very off)
- Accidental style toggle (flats vs sharps) in Schwung's web config UI
- Works with mic or line-in automatically (plug detect)

## Install

### Via Module Store (recommended)

1. Open Schwung's Module Store
2. Navigate to **Tools** → **Guitar Tuner**
3. Select **Install**

### Build from source

```bash
git clone https://github.com/eightfour-dev/schwung-guitar-tuner
cd schwung-guitar-tuner
./scripts/build.sh
```

The build produces `dist/guitar-tuner-module.tar.gz`, which the Module Store
installs into `/data/UserData/schwung/modules/tools/guitar-tuner/` on the
device.

## Usage

1. Open the Tools menu (Shift+Vol+Step13)
2. Select **Guitar Tuner**
3. Play a note — OLED shows the detected note and cents offset; the pad
   grid lights up the letter glyph

Accidental style (flats vs sharps) can be switched in the web config:
`http://move.local:7700/config` → Tuner → Accidental Notation.

## License

MIT — see [LICENSE](LICENSE).

## AI Assistance Disclaimer

Developed with AI assistance. All architecture, implementation, and release
decisions are reviewed by human maintainers. AI-assisted content may still
contain errors, so please validate functionality, security, and license
compatibility before production use.
