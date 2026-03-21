#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Avoid git ownership issues with host-mounted workspace and prebaked IDF checkout.
git config --global --add safe.directory '*' 2>/dev/null || true

./scripts/prepare_sources_from_image.sh "$ROOT_DIR/deps"
./scripts/setup_env.sh "$ROOT_DIR/deps"
./scripts/ci_build.sh "$ROOT_DIR/deps"
./scripts/run_screenshot_generator.sh "$ROOT_DIR/deps"

echo "DONE: firmware + screenshots built."
