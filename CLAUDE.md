# CLAUDE.md — Guitar Tuner Module

External tool module for Schwung. Chromatic pitch detection via YIN on the
Move's mic / line-in input.

## Module Structure

```
src/
  module.json        # Module metadata (component_type: "tool", interactive)
  ui.js              # QuickJS UI (OLED readout + pad glyph)
  help.json          # On-device help content
  dsp/
    guitar_tuner.c   # YIN-based pitch detection plugin (v2 API)
vendor/
  host/
    plugin_api_v1.h  # Pinned copy of Schwung's plugin API header
```

## Build & Deploy

```bash
./scripts/build.sh    # Docker cross-compile → dist/guitar-tuner-module.tar.gz
```

The resulting tarball is what the Schwung Module Store installs. No host-side
dependencies other than the Schwung plugin API header, which is vendored in
`vendor/host/` so the build is self-contained.

## Release

1. Update version in `src/module.json`
2. `git commit -am "bump to vX.Y.Z"`
3. `git tag vX.Y.Z && git push --tags`
4. GitHub Actions builds, attaches the tarball to the release, and rewrites
   `release.json` on `main` so the Module Store catalog resolves the new
   version.

## How It Works

- DSP plugin taps Move's hardware mic/line-in via `host_api_v1_t->mapped_memory`
  at the audio input offset and pulls samples every block.
- YIN pitch detection (cumulative-mean-normalized difference function) runs
  over a 2048-sample window, producing a frequency estimate with an RMS gate
  to avoid noise triggering.
- UI reads frequency/cents via `get_param` and paints:
  - OLED: note name + signed cents + needle meter
  - Pad grid: 3×4 letter glyph + accidental column
- Accidental style (flat vs sharp) is read from Schwung's global settings
  via `host_get_setting("tuner_accidental_style")`.
