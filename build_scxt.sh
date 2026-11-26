#!/usr/bin/env bash
#
# build_scxt.sh - build Shortcircuit XT as LV2 on a Raspberry Pi 5
#                 (aarch64, Debian trixie) - maarthome/shortcircuit-xt, my_own branch
#
# The LV2 is built from source as part of the Shortcircuit XT build
# (-DSCXT_BUILD_LV2=ON): Shortcircuit's own plugin code is compiled into
# scxt_lv2.so, like upstream builds its VST3. There is no .clap file in the
# bundle and no CLAP is loaded at runtime.
#
# Result: dist-linux-aarch64/ShortcircuitXT.lv2/
#
# Usage: ./build_scxt.sh [options]
#   --no-deps          don't apt-get install the build dependencies (= SKIP_DEPS=1)
#   --deploy           install on the pi-Stomp over ssh (needs PISTOMP=user@host)
#                      and restart mod-host and mod-ui there
#   --lv2-dir DIR      LV2 folder on the pi-Stomp (default: ~/.lv2 of the ssh user)
#                      or, with --deb, the folder the package installs into
#                      (default for --deb: /usr/lib/lv2); also LV2_DIR=DIR
#   --zynthian         install on the Zynthian over ssh (ZYNTHIAN=root@zynthian.local)
#                      with the Zynthian widget, and restart Zynthian
#   --deb              also make a .deb package in dist/
#   --deb-only         only make the .deb from the last build (no build)
#   --instruments DIR  copy the instruments (sfz, samples, subfolders) in DIR to the
#                      pi-Stomp's ~/.local/share/shortcircuit-xt/instruments (with
#                      --deploy; like Surge's ~/.local/share/surge-xt) or into the
#                      .deb as /usr/share/shortcircuit-xt/instruments (with --deb)
#   --with-clap        also build the plain CLAP (dist-linux-aarch64/Shortcircuit XT.clap)
#   -h, --help         this help
#
# Environment variables:
#   JOBS=n             parallel compile jobs (default: all cores)
#   VERBOSE=1          show all compiler output instead of the progress line
#   SWAP_GB=n          size of the temporary swap file (default: 6, 0 = none)
#   MAX_PARAMS=n       number of macro knobs as LV2 control ports
#                      (default: 16 = part 1; 256 = all 16 parts)
#   PISTOMP=user@host  the pi-Stomp, e.g. pistomp@pistomp.local
#   ZYNTHIAN=user@host the Zynthian (default root@zynthian.local)
#
# Examples:
#   JOBS=4 ./build_scxt.sh
#   JOBS=4 PISTOMP=pistomp@pistomp.local ./build_scxt.sh --deploy
#   JOBS=4 ./build_scxt.sh --zynthian
#   JOBS=4 ./build_scxt.sh --deb --lv2-dir /home/pistomp/.lv2
#
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
ROOT="$PWD"

usage() { sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'; }

DO_DEPLOY=0; DO_ZYNTHIAN=0; DO_DEB=0; DEB_ONLY=0; WITH_CLAP=0
LV2_DIR="${LV2_DIR:-}"
INSTRUMENTS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --deps)       SKIP_DEPS=0 ;;
        --no-deps)    SKIP_DEPS=1 ;;
        --deploy)     DO_DEPLOY=1 ;;
        --zynthian)   DO_ZYNTHIAN=1 ;;
        --deb)        DO_DEB=1 ;;
        --deb-only)   DO_DEB=1; DEB_ONLY=1 ;;
        --with-clap)  WITH_CLAP=1 ;;
        --instruments) shift; INSTRUMENTS="${1:?--instruments needs a folder}" ;;
        --instruments=*) INSTRUMENTS="${1#*=}" ;;
        --lv2-dir)    shift; LV2_DIR="${1:?--lv2-dir needs a folder}" ;;
        --lv2-dir=*)  LV2_DIR="${1#*=}" ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

JOBS="${JOBS:-$(nproc)}"
SWAP_GB="${SWAP_GB:-6}"
MAX_PARAMS="${MAX_PARAMS:-16}"
ZYNTHIAN="${ZYNTHIAN:-root@zynthian.local}"
BUILD_DIR="$ROOT/ignore/build-pi"
DIST="$ROOT/dist-linux-aarch64"
BUNDLE_NAME="ShortcircuitXT.lv2"
BUNDLE="$DIST/$BUNDLE_NAME"
WIDGET="$ROOT/zynthian/ShortcircuitXT-zyngui.lv2"
SWAPFILE="/var/tmp/scxt_build.swap"

log() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# Run a ninja build showing one progress line (percentage, steps, time left)
# instead of all compiler output; the full output goes to $BUILD_LOG.
# VERBOSE=1 shows the normal output instead.
BUILD_LOG="$ROOT/ignore/build-pi.log"
fmt_time() {
    local sec=$1
    if [ "$sec" -lt 60 ]; then printf '%ds' "$sec"
    elif [ "$sec" -lt 3600 ]; then printf '%dm' $((sec / 60))
    else printf '%dh%02dm' $((sec / 3600)) $(((sec % 3600) / 60)); fi
}
progress_filter() {
    local line done total pct eta first_done=-1 t_first=0 t0=$SECONDS
    local re='^\[([0-9]+)/([0-9]+)\] '
    while IFS= read -r line; do
        [[ $line =~ $re ]] || continue
        done=${BASH_REMATCH[1]}; total=${BASH_REMATCH[2]}
        [ "$total" -gt 0 ] || continue
        if [ "$first_done" -lt 0 ]; then first_done=$done; t_first=$SECONDS; fi
        pct=$((done * 100 / total))
        eta="estimating..."
        if [ $((done - first_done)) -ge 5 ] && [ $SECONDS -gt "$t_first" ]; then
            eta="about $(fmt_time $(((total - done) * (SECONDS - t_first) / (done - first_done)))) left"
        fi
        printf '\r\033[K  Compiling: %3d%%  (%d/%d)  %s, running %s' \
            "$pct" "$done" "$total" "$eta" "$(fmt_time $((SECONDS - t0)))"
    done
    printf '\n'
}
build_with_progress() {
    mkdir -p "$(dirname "$BUILD_LOG")"
    if [ "${VERBOSE:-0}" = "1" ]; then
        "$@" 2>&1 | tee "$BUILD_LOG"
        return "${PIPESTATUS[0]}"
    fi
    local start=$SECONDS status
    set +e
    NINJA_STATUS="[%f/%t] " "$@" 2>&1 | tee "$BUILD_LOG" | progress_filter
    status=${PIPESTATUS[0]}
    set -e
    if [ "$status" != "0" ]; then
        echo "Build failed - last lines of $BUILD_LOG:" >&2
        grep -E "error|Error|FAILED" "$BUILD_LOG" | head -20 >&2 || true
        echo "..." >&2
        tail -n 30 "$BUILD_LOG" >&2
        return "$status"
    fi
    printf '\033[1A\r\033[K  Compiling: 100%%  done\n'
    echo "  Done in $(fmt_time $((SECONDS - start))) (full output: $BUILD_LOG)"
}

if [ "$DO_DEPLOY" = "1" ] && [ -z "${PISTOMP:-}" ]; then
    die "--deploy needs the pi-Stomp address, e.g. PISTOMP=pistomp@pistomp.local ./build_scxt.sh --deploy"
fi
if [ -n "$INSTRUMENTS" ] && [ ! -d "$INSTRUMENTS" ]; then
    die "--instruments: folder $INSTRUMENTS not found"
fi
if [ "$DO_DEB" = "1" ] && [ -n "$LV2_DIR" ] && [ "${LV2_DIR#/}" = "$LV2_DIR" ]; then
    die "--deb needs a full path for --lv2-dir (a package can't install into ~ or a relative folder)"
fi

# ---------------------------------------------------------------------------
build() {
    # temporary swap (the Pi has 4 GB RAM; some files need a lot to compile)
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

    log "Configuring Shortcircuit XT with LV2"
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSCXT_BUILD_LV2=ON -DSCXT_BUILD_VST3=OFF

    local targets=(scxt_lv2 scxt-lv2-ttl)
    [ "$WITH_CLAP" = "1" ] && targets+=(scxt_clapfirst_clap)
    log "Building ${targets[*]} with $JOBS jobs - the first build takes a long time on a Pi"
    build_with_progress cmake --build "$BUILD_DIR" --target "${targets[@]}" -j "$JOBS"

    local so ttlgen
    so="$(find "$BUILD_DIR" -name scxt_lv2.so -type f | head -n1)"
    ttlgen="$(find "$BUILD_DIR" -name scxt-lv2-ttl -type f -perm -u+x | head -n1)"
    [ -n "$so" ] || die "scxt_lv2.so not found in $BUILD_DIR"
    [ -n "$ttlgen" ] || die "scxt-lv2-ttl not found in $BUILD_DIR"

    log "Assembling $BUNDLE"
    rm -rf "$BUNDLE"
    mkdir -p "$BUNDLE"
    cp "$so" "$BUNDLE/scxt_lv2.so"
    # tools to rename the knobs later, also on the Zynthian (see generate_ttl.sh)
    cp "$ttlgen" "$ROOT/generate_ttl.sh" "$BUNDLE/"
    strip --strip-unneeded "$BUNDLE/scxt_lv2.so" "$BUNDLE/scxt-lv2-ttl" || true
    "$BUNDLE/scxt-lv2-ttl" "$BUNDLE/scxt_lv2.so" "$BUNDLE" scxt_lv2.so "$MAX_PARAMS"

    # the plain CLAP is only kept with --with-clap (an old copy confuses lv2ls)
    rm -f "$DIST/Shortcircuit XT.clap"
    if [ "$WITH_CLAP" = "1" ]; then
        local clap
        clap="$(find "$BUILD_DIR" -name 'Shortcircuit XT.clap' -type f | head -n1)"
        [ -n "$clap" ] && cp "$clap" "$DIST/Shortcircuit XT.clap"
    fi

    log "Checking the bundle with lv2ls"
    if command -v lv2ls >/dev/null; then
        # look at the new bundle only
        local check
        check="$(mktemp -d)"
        ln -s "$BUNDLE" "$check/$BUNDLE_NAME"
        if LV2_PATH="$check" lv2ls | grep -q 'maarthome/shortcircuit-xt'; then
            echo "lv2ls finds: $(LV2_PATH="$check" lv2ls | grep 'maarthome/shortcircuit-xt')"
        else
            echo "WARNING: lv2ls does not list the plugin - check the .ttl files in $BUNDLE" >&2
        fi
        rm -rf "$check"
    fi
    echo "Control ports: $(grep -c 'lv2:ControlPort' "$BUNDLE/shortcircuit_xt.ttl")"
    du -sh "$BUNDLE"
}

# ---------------------------------------------------------------------------
deploy_pistomp() {
    local dest="${LV2_DIR:-.lv2}"   # relative = in the ssh user's home on the pi-Stomp
    log "Installing on the pi-Stomp ($PISTOMP:$dest)"
    ssh "$PISTOMP" "mkdir -p '$dest' && rm -rf '$dest/$BUNDLE_NAME'"
    rsync -a --protect-args "$BUNDLE" "$PISTOMP:$dest/"
    if [ -n "$INSTRUMENTS" ]; then
        log "Copying instruments from $INSTRUMENTS to $PISTOMP:.local/share/shortcircuit-xt/instruments"
        ssh "$PISTOMP" "mkdir -p .local/share/shortcircuit-xt/instruments"
        rsync -a --protect-args "${INSTRUMENTS%/}/" "$PISTOMP:.local/share/shortcircuit-xt/instruments/"
    fi
    log "Restarting mod-host and mod-ui on the pi-Stomp (may ask for its sudo password)"
    ssh -t "$PISTOMP" "sudo systemctl restart mod-host mod-ui"
}

deploy_zynthian() {
    log "Installing on the Zynthian ($ZYNTHIAN:/usr/local/lib/lv2)"
    ssh "$ZYNTHIAN" "rm -rf /usr/local/lib/lv2/$BUNDLE_NAME"
    scp -rq "$BUNDLE" "$WIDGET" "$ZYNTHIAN:/usr/local/lib/lv2/"
    log "Restarting Zynthian"
    ssh "$ZYNTHIAN" "systemctl restart zynthian"
}

make_deb() {
    [ -f "$BUNDLE/scxt_lv2.so" ] || die "no build in $BUNDLE - build first (without --deb-only)"
    local dest="${LV2_DIR:-/usr/lib/lv2}"
    local version
    version="1.0+$(date +%Y%m%d).$(git rev-parse --short HEAD)"
    local pkg="$ROOT/dist/deb-root"
    log "Making .deb (installs into $dest)"
    rm -rf "$pkg"
    mkdir -p "$pkg/DEBIAN" "$pkg$dest"
    cp -r "$BUNDLE" "$pkg$dest/"
    if [ -n "$INSTRUMENTS" ]; then
        mkdir -p "$pkg/usr/share/shortcircuit-xt/instruments"
        cp -r "${INSTRUMENTS%/}/." "$pkg/usr/share/shortcircuit-xt/instruments/"
    fi
    cat > "$pkg/DEBIAN/control" <<EOF
Package: shortcircuit-xt-lv2
Version: $version
Architecture: arm64
Maintainer: maarthome
Section: sound
Priority: optional
Depends: libc6, libstdc++6, libx11-6, libfreetype6, libfontconfig1
Description: Shortcircuit XT sampler as LV2 plugin
 Built from source (maarthome/shortcircuit-xt, my_own branch) for MOD /
 pi-Stomp, Zynthian and other LV2 hosts.
EOF
    cat > "$pkg/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
    for s in mod-host mod-ui; do
        systemctl is-active --quiet "$s" 2>/dev/null && systemctl restart "$s" || true
    done
fi
exit 0
EOF
    chmod 755 "$pkg/DEBIAN/postinst"
    mkdir -p "$ROOT/dist"
    dpkg-deb --root-owner-group --build "$pkg" "$ROOT/dist/shortcircuit-xt-lv2_${version}_arm64.deb"
    rm -rf "$pkg"
}

# ---------------------------------------------------------------------------
[ "$DEB_ONLY" = "1" ] || build
[ "$DO_DEB" = "1" ] && make_deb
[ "$DO_DEPLOY" = "1" ] && deploy_pistomp
[ "$DO_ZYNTHIAN" = "1" ] && deploy_zynthian

log "Done"
cat <<EOF

LV2 bundle: $BUNDLE
  install on the pi-Stomp:  PISTOMP=pistomp@pistomp.local ./build_scxt.sh --deploy --no-deps
  install on the Zynthian:  ./build_scxt.sh --zynthian --no-deps
EOF
