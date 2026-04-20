#!/usr/bin/env bash
# Build Guitar Tuner module for Schwung (ARM64)
#
# Cross-compiles the tuner DSP plugin via Docker.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="guitar-tuner"
IMAGE_NAME="schwung-guitar-tuner-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Guitar Tuner Module Build (via Docker) ==="
    echo ""

    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Guitar Tuner Module ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p "build"
mkdir -p "dist/$MODULE_ID"

# Compile the DSP plugin (single source file, needs only libm)
echo "Compiling guitar_tuner.c..."
"${CROSS_PREFIX}gcc" -O3 -shared -fPIC \
    -Ivendor \
    src/dsp/guitar_tuner.c \
    -o build/dsp.so \
    -lm

# Package: module.json + ui.js + help.json + dsp.so
echo "Packaging..."
cp src/module.json   "dist/$MODULE_ID/module.json"
cp src/ui.js         "dist/$MODULE_ID/ui.js"
cp src/help.json     "dist/$MODULE_ID/help.json"
cp build/dsp.so      "dist/$MODULE_ID/dsp.so"
chmod +x "dist/$MODULE_ID/dsp.so"

# Create release tarball
cd dist
tar -czvf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
echo "Tarball: dist/$MODULE_ID-module.tar.gz"
