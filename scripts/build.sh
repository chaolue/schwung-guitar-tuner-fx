#!/usr/bin/env bash
# Build the Guitar Tuner FX module for Schwung on Ableton Move (ARM64).
#
# Uses Docker for cross-compilation unless CROSS_PREFIX is set (or we are
# already inside the container). Output: dist/guitar-tuner-fx/ and
# dist/guitar-tuner-fx-module.tar.gz
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="guitar-tuner-fx"
IMAGE_NAME="schwung-guitar-tuner-fx-builder"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Guitar Tuner FX build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building Guitar Tuner FX (${CROSS_PREFIX}gcc) ==="
mkdir -p build

# The plugin serves its own chain_params / ui_hierarchy, generated from
# module.json so the two cannot drift (see scripts/gen_contract.py).
if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required to generate the contract header" >&2
    exit 1
fi
python3 scripts/gen_contract.py src/module.json build/contract_gen.h

# A slot's chain opens modules/audio_fx/<id>/<id>.so whatever module.json
# says, and Master FX / sends open module.json's "dsp" -- so the library is
# named after the module id and "dsp" names the same file.
"${CROSS_PREFIX}gcc" -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -Wall -Wextra \
    -Ivendor -Ibuild \
    src/dsp/guitar_tuner.c \
    -o "build/${MODULE_ID}.so" \
    -lm

if command -v file >/dev/null 2>&1; then
    if ! file "build/${MODULE_ID}.so" | grep -q 'ARM aarch64'; then
        echo "ERROR: build/${MODULE_ID}.so is not an aarch64 library:" >&2
        file "build/${MODULE_ID}.so" >&2
        exit 1
    fi
fi

echo "Packaging..."
rm -rf "dist/${MODULE_ID}"
mkdir -p "dist/${MODULE_ID}"
# cat rather than cp: avoids ExtFS deallocation issues on Docker mounts
cat src/module.json         > "dist/${MODULE_ID}/module.json"
cat src/help.json           > "dist/${MODULE_ID}/help.json"
cat src/canvas.js           > "dist/${MODULE_ID}/canvas.js"
cat "build/${MODULE_ID}.so" > "dist/${MODULE_ID}/${MODULE_ID}.so"
chmod +x "dist/${MODULE_ID}/${MODULE_ID}.so"

tar -czf "dist/${MODULE_ID}-module.tar.gz" -C dist "${MODULE_ID}"

echo ""
echo "=== Build complete ==="
echo "Module:  dist/${MODULE_ID}/"
echo "Tarball: dist/${MODULE_ID}-module.tar.gz"
echo "Install: ./scripts/install.sh"
