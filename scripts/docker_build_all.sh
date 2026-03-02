#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Avoid git ownership issues with host-mounted workspace.
git config --global --add safe.directory '*' || true

# Some images may not include sudo; setup_env.sh uses it on apt-based hosts.
if ! command -v sudo >/dev/null 2>&1; then
  cat >/usr/local/bin/sudo <<'EOS'
#!/usr/bin/env bash
exec "$@"
EOS
  chmod +x /usr/local/bin/sudo
fi

mkdir -p sources
./scripts/prepare_sources_from_image.sh "$ROOT_DIR/sources"

if [ ! -d "$ROOT_DIR/sources/seedsigner-c-modules/.git" ]; then
  git clone https://github.com/kdmukAI-bot/seedsigner-c-modules.git "$ROOT_DIR/sources/seedsigner-c-modules"
fi

./scripts/setup_env.sh "$ROOT_DIR/sources"
./scripts/ci_build.sh "$ROOT_DIR/sources"
./scripts/run_screenshot_generator.sh "$ROOT_DIR/sources"

echo "DONE: firmware + screenshots built."
