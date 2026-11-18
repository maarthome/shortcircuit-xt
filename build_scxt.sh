#!/usr/bin/env bash
#
# build_scxt.sh - build Shortcircuit XT on a Raspberry Pi 5 (aarch64, Debian trixie)
#                 as CLAP + LV2 (maarthome/shortcircuit-xt, my_own branch)
#
# Result: dist-linux-aarch64/
#           ShortcircuitXT.lv2/         <- LV2 bundle (copy this to the Zynthian)
#           Shortcircuit XT.clap        <- the plain CLAP plugin
#
# Options (environment variables):
#   JOBS=n          parallel compile jobs (default: all cores)
#   SWAP_GB=n       size of the temporary swap file (default: 6, 0 = none)
#   SKIP_DEPS=1     don't apt-get install the build dependencies
#   BUILD_ALL=1     also build VST3 and the standalone app
#   MAX_PARAMS=n    only expose the first n macro knobs as LV2 controls
#                   (default: all 256 = 16 parts x 16 macros; 16 = part 1 only)
#
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
ROOT="$PWD"
JOBS="${JOBS:-$(nproc)}"
SWAP_GB="${SWAP_GB:-6}"
BUILD_DIR="$ROOT/ignore/build-pi"
LV2_BUILD_DIR="$ROOT/ignore/build-lv2"
DIST="$ROOT/dist-linux-aarch64"
BUNDLE="$DIST/ShortcircuitXT.lv2"
SWAPFILE="/var/tmp/scxt_build.swap"

log() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }

# --- temporary swap (the Pi has 4 GB RAM; some files need a lot to compile) ---
cleanup_swap() {
    if [ -f "$SWAPFILE" ]; then
        log "Removing temporary swap file"
        sudo swapoff "$SWAPFILE" 2>/dev/null || true
        sudo rm -f "$SWAPFILE"
    fi
}
if [ "$SWAP_GB" != "0" ]; then
    trap cleanup_swap EXIT
    if ! swapon --show=NAME --noheadings | grep -qx "$SWAPFILE"; then
        log "Adding temporary ${SWAP_GB} GB swap file at $SWAPFILE"
        sudo fallocate -l "${SWAP_GB}G" "$SWAPFILE"
        sudo chmod 600 "$SWAPFILE"
        sudo mkswap "$SWAPFILE" >/dev/null
        sudo swapon "$SWAPFILE"
    fi
fi

# --- dependencies ---
if [ "${SKIP_DEPS:-0}" != "1" ]; then
    log "Installing build dependencies"
    sudo apt-get update
    sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
        lv2-dev lilv-utils \
        libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev \
        libfreetype-dev libfontconfig1-dev libasound2-dev libjack-jackd2-dev
fi

log "Updating submodules"
git submodule update --init --recursive

# --- Shortcircuit XT itself (CLAP) ---
CMAKE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release)
TARGET=scxt_clapfirst_clap
if [ "${BUILD_ALL:-0}" = "1" ]; then
    TARGET=scxt_clapfirst_all
else
    CMAKE_ARGS+=(-DSCXT_BUILD_VST3=OFF)
fi

log "Configuring Shortcircuit XT"
cmake -S "$ROOT" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"

log "Building Shortcircuit XT ($TARGET) with $JOBS jobs - this takes a long time on a Pi"
cmake --build "$BUILD_DIR" --target "$TARGET" -j "$JOBS"

CLAP_FILE="$(find "$BUILD_DIR" -name 'Shortcircuit XT.clap' -type f | head -n1)"
if [ -z "$CLAP_FILE" ]; then
    echo "Could not find 'Shortcircuit XT.clap' in $BUILD_DIR" >&2
    exit 1
fi
echo "CLAP: $CLAP_FILE"

# --- LV2 wrapper ---
log "Building the LV2 wrapper"
cmake -S "$ROOT/src/clients/lv2" -B "$LV2_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$LV2_BUILD_DIR" -j "$JOBS"

log "Assembling $BUNDLE"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp "$CLAP_FILE" "$DIST/Shortcircuit XT.clap"
cp "$CLAP_FILE" "$BUNDLE/scxt.clap"
cp "$LV2_BUILD_DIR/scxt_lv2.so" "$BUNDLE/"
# tools to rename the knobs later, also on the Zynthian (see generate_ttl.sh)
cp "$LV2_BUILD_DIR/clap2lv2-ttl" "$ROOT/generate_ttl.sh" "$BUNDLE/"
strip --strip-unneeded "$BUNDLE/scxt_lv2.so" "$BUNDLE/scxt.clap" "$BUNDLE/clap2lv2-ttl" || true
if [ -n "${MAX_PARAMS:-}" ]; then
    "$LV2_BUILD_DIR/clap2lv2-ttl" "$BUNDLE/scxt.clap" "$BUNDLE" scxt_lv2.so "$MAX_PARAMS"
else
    "$LV2_BUILD_DIR/clap2lv2-ttl" "$BUNDLE/scxt.clap" "$BUNDLE" scxt_lv2.so
fi

log "Checking the bundle with lv2ls"
if command -v lv2ls >/dev/null; then
    if LV2_PATH="$DIST" lv2ls | grep -q 'maarthome/shortcircuit-xt'; then
        echo "lv2ls finds: $(LV2_PATH="$DIST" lv2ls | grep 'maarthome/shortcircuit-xt')"
    else
        echo "WARNING: lv2ls does not list the plugin - check the .ttl files in $BUNDLE" >&2
    fi
fi

log "Done"
du -sh "$BUNDLE"
cat <<EOF

LV2 bundle: $BUNDLE

Copy it to the Zynthian with:
  scp -r "$BUNDLE" root@zynthian.local:/usr/local/lib/lv2/

Then search for new plugins in the Zynthian webconf (Software > LV2 Plugins)
and enable "Shortcircuit XT".
EOF
