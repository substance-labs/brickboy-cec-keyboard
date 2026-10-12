#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
THIRD_PARTY_DIR="$SCRIPT_DIR/third_party"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/build}"
JOBS="${JOBS:-2}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1782562569}"
RPI_RUNTIME_LIB_DIR="${RPI_RUNTIME_LIB_DIR:-$REPOSITORY_ROOT/brickboy-home-folder/brickboy/program/brickboy-cec-keyboard/lib/rpi}"
BUILD_WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/brickboy-cec-keyboard.XXXXXX")"

cleanup() {
  if [[ "${KEEP_BUILD_WORK_DIR:-0}" = "1" ]]; then
    printf 'Kept build workspace: %s\n' "$BUILD_WORK_DIR"
  else
    rm -rf "$BUILD_WORK_DIR"
  fi
}
trap cleanup EXIT INT TERM

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'Missing required command: %s\n' "$1" >&2
    exit 1
  }
}

require_file() {
  [[ -f "$1" ]] || {
    printf 'Missing required file: %s\n' "$1" >&2
    exit 1
  }
}

for command_name in cmake g++ make readelf sha256sum; do
  require_command "$command_name"
done

if [[ "$(uname -m)" != "aarch64" ]]; then
  printf 'This native build must run on an aarch64 BrickBoy-compatible Debian system.\n' >&2
  exit 1
fi

LIBCEC_SOURCE="$THIRD_PARTY_DIR/libcec"
P8_PLATFORM_SOURCE="$THIRD_PARTY_DIR/p8-platform"
RPI_INCLUDE_SOURCE="$THIRD_PARTY_DIR/raspberrypi-userland/include"
RPI_STAGE="$BUILD_WORK_DIR/rpi"
PREFIX="$BUILD_WORK_DIR/prefix"

require_file "$LIBCEC_SOURCE/LICENSE.md"
require_file "$P8_PLATFORM_SOURCE/LICENSE.md"
require_file "$RPI_INCLUDE_SOURCE/bcm_host.h"
for library_name in libbcm_host.so.0 libvchiq_arm.so.0 libvcos.so.0; do
  require_file "$RPI_RUNTIME_LIB_DIR/$library_name"
done

mkdir -p "$BUILD_WORK_DIR/source" "$BUILD_WORK_DIR/build" "$PREFIX" "$RPI_STAGE/include" "$RPI_STAGE/lib"
cp -R "$LIBCEC_SOURCE/." "$BUILD_WORK_DIR/source/libcec"
cp -R "$P8_PLATFORM_SOURCE/." "$BUILD_WORK_DIR/source/p8-platform"
cp -R "$RPI_INCLUDE_SOURCE/." "$RPI_STAGE/include"

for library_name in libbcm_host.so.0 libvchiq_arm.so.0 libvcos.so.0; do
  cp "$RPI_RUNTIME_LIB_DIR/$library_name" "$RPI_STAGE/lib/$library_name"
done
ln -s libbcm_host.so.0 "$RPI_STAGE/lib/libbcm_host.so"
ln -s libvchiq_arm.so.0 "$RPI_STAGE/lib/libvchiq_arm.so"
ln -s libvcos.so.0 "$RPI_STAGE/lib/libvcos.so"

export SOURCE_DATE_EPOCH
# Prevent libCEC's revision probe from accidentally reading the enclosing
# BrickBoy checkout. The exact upstream revision is recorded in SOURCES.json.
export GIT_CEILING_DIRECTORIES="$BUILD_WORK_DIR"

REMAPPED_SOURCE_ROOT="/usr/src/brickboy-cec-keyboard"
PREFIX_MAP_FLAGS="-ffile-prefix-map=$BUILD_WORK_DIR=$REMAPPED_SOURCE_ROOT -fdebug-prefix-map=$BUILD_WORK_DIR=$REMAPPED_SOURCE_ROOT"
PREFIX_MAP_FLAGS+=" -ffile-prefix-map=$SCRIPT_DIR=$REMAPPED_SOURCE_ROOT/helper -fdebug-prefix-map=$SCRIPT_DIR=$REMAPPED_SOURCE_ROOT/helper"

cmake \
  -S "$BUILD_WORK_DIR/source/p8-platform" \
  -B "$BUILD_WORK_DIR/build/p8-platform" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_CXX_FLAGS="$PREFIX_MAP_FLAGS"
cmake --build "$BUILD_WORK_DIR/build/p8-platform" --parallel "$JOBS"
cmake --install "$BUILD_WORK_DIR/build/p8-platform"

cmake \
  -S "$BUILD_WORK_DIR/source/libcec" \
  -B "$BUILD_WORK_DIR/build/libcec" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_C_FLAGS="$PREFIX_MAP_FLAGS" \
  -DCMAKE_CXX_FLAGS="$PREFIX_MAP_FLAGS" \
  -DSKIP_PYTHON_WRAPPER=1 \
  -DHAVE_RPI_API=ON \
  -DRPI_INCLUDE_DIR="$RPI_STAGE/include" \
  -DRPI_LIB_DIR="$RPI_STAGE/lib"
cmake --build "$BUILD_WORK_DIR/build/libcec" --target cec-static --parallel "$JOBS"

mkdir -p "$PREFIX/include/libcec" "$PREFIX/lib"
cp "$BUILD_WORK_DIR/source/libcec/include/"*.h "$PREFIX/include/libcec/"
cp "$BUILD_WORK_DIR/build/libcec/src/libcec/libcec.a" "$PREFIX/lib/libcec.a"

cmake \
  -S "$SCRIPT_DIR" \
  -B "$BUILD_WORK_DIR/build/helper" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_CXX_FLAGS="$PREFIX_MAP_FLAGS" \
  -DBRICKBOY_CEC_WITH_LIBCEC=ON \
  -DBRICKBOY_CEC_STATIC_LIBCEC=ON \
  -DBRICKBOY_CEC_PREFIX="$PREFIX" \
  -DBRICKBOY_RPI_LIB_DIR="$RPI_STAGE/lib"
cmake --build "$BUILD_WORK_DIR/build/helper" --parallel "$JOBS"

mkdir -p "$OUTPUT_DIR"
install -m 0755 "$BUILD_WORK_DIR/build/helper/brickboy-cec-keyboard" "$OUTPUT_DIR/brickboy-cec-keyboard"

if readelf -d "$OUTPUT_DIR/brickboy-cec-keyboard" | grep -Eq 'RPATH|RUNPATH'; then
  printf 'Unexpected RPATH/RUNPATH in output binary\n' >&2
  exit 1
fi

printf 'Built %s\n' "$OUTPUT_DIR/brickboy-cec-keyboard"
sha256sum "$OUTPUT_DIR/brickboy-cec-keyboard"
