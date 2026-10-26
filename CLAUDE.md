# CLAUDE.md — Guitar Tuner FX Module

External **audio FX** module for Schwung (`component_type: "audio_fx"`,
chainable). Chromatic pitch detection via YIN on the audio passing through its
chain position; passes that audio through untouched unless Mute Output is on.

## Module Structure

```
src/
  module.json        # Metadata + chain_params + ui_hierarchy (the contract)
  canvas.js          # custom:tuner cell widget + the Tuner page (drawPage)
  help.json          # On-device help (top-level `children`, or it is ignored)
  dsp/
    guitar_tuner.c   # audio_fx_api_v2 plugin: YIN, mute fade, readings
vendor/host/         # Pinned copies of Schwung's plugin_api_v1.h / audio_fx_api_v2.h
scripts/
  build.sh           # Docker (or CROSS_PREFIX) cross-compile + package
  gen_contract.py    # module.json -> build/contract_gen.h (served contract)
  install.sh         # scp to a Move for testing
tests/               # run_tests.sh: DSP, contract, help, canvas
tools/preview.mjs    # render the pages with a Schwung checkout's renderer
```

## Build & Test

```bash
bash tests/run_tests.sh    # native: DSP tests, contract, help width, canvas
./scripts/build.sh         # Docker cross-compile -> dist/guitar-tuner-fx-module.tar.gz
```

## Rules that matter

- **The library is `<id>.so`** (`guitar-tuner-fx.so`), and module.json's
  `dsp` names the same file: a slot chain opens `modules/audio_fx/<id>/<id>.so`
  regardless of `dsp`, while Master FX / sends read `dsp`.
- **Every entry point runs on the SPI audio callback** (see the contract at
  the top of `vendor/host/plugin_api_v1.h`). No allocation, file I/O or
  logging outside create/destroy. The YIN difference function is sliced
  across the four blocks of each analysis cycle — keep it that way.
- **The contract is served by the DSP** (`chain_params`, `ui_hierarchy`),
  generated from `src/module.json` by `scripts/gen_contract.py`. Edit
  module.json only; `tests/check_contract.py` fails if they drift.
- **`tuner` is one packed string** (`"E2 +3.4c 82.6Hz"` / `"--"`), declared
  `live`. canvas.js parses it; `tests/test_canvas.mjs` checks the parser
  against strings the DSP really produced (`test_tuner --dump-readings`).
- **Pass-through is bit-exact** while Mute is Off and not fading; analysis
  always taps the input before the mute.
- Analysis is analysis-only state: the 1.5 s hold lives in the DSP, and the
  reading clears after `READING_STALE_READS` reads with no `process_block`
  (idle-parked or bypassed FX), so the display never freezes.

## Release

1. Update version in `src/module.json`
2. `git commit -am "bump to vX.Y.Z"`
3. `git tag vX.Y.Z && git push --tags`
4. GitHub Actions tests, builds, attaches the tarball, and rewrites
   `release.json` on `main` so the Module Store catalog resolves it.
