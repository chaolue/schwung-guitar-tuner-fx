#!/usr/bin/env bash
# Offline tests: DSP behaviour, the served contract, help text and the canvas
# drawers. Native build -- no Move, no cross-compiler, no Docker needed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
mkdir -p build

CC="${CC:-cc}"
python3 scripts/gen_contract.py src/module.json build/contract_gen.h

echo "==> DSP"
"$CC" -std=gnu11 -O2 -g -Wall -Wextra -Werror \
    -Ivendor -Ibuild tests/test_tuner.c -o build/test_tuner -lm
./build/test_tuner

echo "==> served contract == module.json"
./build/test_tuner --dump-contract > build/served_contract.txt
python3 tests/check_contract.py build/served_contract.txt

echo "==> help.json fits the screen"
python3 tests/check_help.py

echo "==> canvas.js"
./build/test_tuner --dump-readings > build/readings.json
node tests/test_canvas.mjs build/readings.json

echo "==> all tests passed"
