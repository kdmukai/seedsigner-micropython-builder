#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKDIR="${1:-$ROOT_DIR/deps}"
MP_DIR="$WORKDIR/micropython/upstream"
CMODS_DIR="$WORKDIR/seedsigner-c-modules"

IDF_DIR="${IDF_DIR:-}"
if [ -z "$IDF_DIR" ]; then
  if [ -d "/opt/toolchains/esp-idf" ]; then
    IDF_DIR="/opt/toolchains/esp-idf"
  else
    IDF_DIR="$WORKDIR/esp-idf"
  fi
fi

BOARD="${BOARD:-WAVESHARE_ESP32_S3_TOUCH_LCD_35B}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/$BOARD}"
LOGS_DIR="${LOGS_DIR:-$ROOT_DIR/logs}"

if [ ! -e "$MP_DIR/.git" ]; then
  echo "ERROR: expected MicroPython repo at $MP_DIR"
  exit 1
fi
if [ ! -e "$CMODS_DIR/.git" ]; then
  echo "ERROR: expected seedsigner-c-modules repo at $CMODS_DIR"
  exit 1
fi
if [ ! -d "$IDF_DIR" ]; then
  echo "ERROR: expected ESP-IDF at $IDF_DIR"
  exit 1
fi

mkdir -p "$BUILD_DIR" "$LOGS_DIR"
TS="$(date -u +%Y-%m-%d_%H%M%SZ)"
BUILD_LOG="$LOGS_DIR/${TS}-build-${BOARD}.log"

echo "Build log: $BUILD_LOG"

if [ -z "${IDF_TOOLS_PATH:-}" ]; then
  if [ -d "/opt/espressif" ]; then
    export IDF_TOOLS_PATH="/opt/espressif"
  else
    export IDF_TOOLS_PATH="$ROOT_DIR/.espressif"
  fi
fi

export IDF_PATH="$IDF_DIR"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
idf.py --version >/dev/null 2>&1 || { echo "ERROR: idf.py not runnable (GHCR base image ESP-IDF toolchain missing/broken)"; exit 1; }

PORTS_ESP32_DIR="$ROOT_DIR/ports/esp32"
USER_C_MODULES_FILE="$ROOT_DIR/usercmodule.cmake"
MICROPY_CMAKE_ARGS="${CMAKE_ARGS:-} -DUSER_C_MODULES=$USER_C_MODULES_FILE"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DMICROPY_EXTRA_COMPONENT_DIRS=${PORTS_ESP32_DIR}\;${CMODS_DIR}/components"
MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DSEEDSIGNER_C_MODULES_DIR=$CMODS_DIR"

# board_common board config: maps MicroPython board name to board_common board dir.
BOARD_COMMON_DIR="$PORTS_ESP32_DIR/board_common"
BOARD_CONFIG_DIR="${BOARD_CONFIG_DIR:-$BOARD_COMMON_DIR/boards/waveshare_s3_lcd35b}"
if [ -d "$BOARD_CONFIG_DIR" ]; then
  MICROPY_CMAKE_ARGS="$MICROPY_CMAKE_ARGS -DBOARD_CONFIG_DIR=$BOARD_CONFIG_DIR"
else
  echo "WARNING: BOARD_CONFIG_DIR not found: $BOARD_CONFIG_DIR"
fi

{
  make -C "$MP_DIR/mpy-cross" USER_C_MODULES= -j"$(nproc)"
  rm -rf "$BUILD_DIR"
  make -C "$MP_DIR/ports/esp32" -j"$(nproc)" \
    BOARD="$BOARD" \
    BUILD="$BUILD_DIR" \
    USER_C_MODULES="$USER_C_MODULES_FILE" \
    CMAKE_ARGS="$MICROPY_CMAKE_ARGS" \
    MICROPY_MPYCROSS="$MP_DIR/mpy-cross/build/mpy-cross" \
    IDF_CCACHE_ENABLE=1

  if ! grep -Rqs "usercmodule.cmake" "$BUILD_DIR"/CMakeCache.txt "$BUILD_DIR"/esp-idf/main/CMakeFiles 2>/dev/null; then
    echo "ERROR: USER_C_MODULES not detected in build metadata (expected $USER_C_MODULES_FILE)."
    exit 1
  fi

  echo "Build complete. Artifacts:"
  ls -lh "$BUILD_DIR"/micropython.bin "$BUILD_DIR"/micropython.elf "$BUILD_DIR"/flash_args

  # Package flash-ready files for easy download/flashing.
  FLASH_DIR="$BUILD_DIR/flash"
  rm -rf "$FLASH_DIR"
  mkdir -p "$FLASH_DIR/bootloader" "$FLASH_DIR/partition_table"
  cp "$BUILD_DIR"/flash_args "$FLASH_DIR/"
  cp "$BUILD_DIR"/micropython.bin "$FLASH_DIR/"
  cp "$BUILD_DIR"/bootloader/bootloader.bin "$FLASH_DIR/bootloader/"
  cp "$BUILD_DIR"/partition_table/partition-table.bin "$FLASH_DIR/partition_table/"
  echo "Flash package: $FLASH_DIR"
  echo "  Flash with: python -m esptool --chip esp32s3 write_flash @flash_args"
  ls -lhR "$FLASH_DIR"
} 2>&1 | tee "$BUILD_LOG"

echo "Log saved to: $BUILD_LOG"
