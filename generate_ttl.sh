#!/usr/bin/env bash
#
# generate_ttl.sh - rewrite the .ttl files of ShortcircuitXT.lv2 so the LV2
#                   knobs get the macro names you gave them in Shortcircuit XT.
#
# Works on the Pi (in the repository) and on the Zynthian (inside the
# installed bundle, where build_scxt.sh also puts a copy of this script).
#
# The LV2 plugin writes its current setup to
#   ~/.local/share/shortcircuit-xt-lv2/last-state.scxtstate
# whenever the host saves its state and whenever you close the Shortcircuit
# XT editor window. This script reads the macro names from that file.
#
# Usage:
#   ./generate_ttl.sh [state-file]
#
# Options (environment variables):
#   BUNDLE=dir       the .lv2 bundle to update (default: the bundle this
#                    script is in, else dist-linux-aarch64/ShortcircuitXT.lv2)
#   FULL_NAMES=1     keep "P1 M1 - Cutoff" instead of just "Cutoff"
#   MAX_PARAMS=n     number of macro knobs to expose (default: same as now)
#
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

if [ -z "${BUNDLE:-}" ]; then
    if [ -f "$HERE/scxt_lv2.so" ]; then
        BUNDLE="$HERE"
    else
        BUNDLE="$HERE/dist-linux-aarch64/ShortcircuitXT.lv2"
    fi
fi
STATE="${1:-$HOME/.local/share/shortcircuit-xt-lv2/last-state.scxtstate}"

if [ ! -f "$BUNDLE/scxt_lv2.so" ]; then
    echo "No scxt_lv2.so in $BUNDLE - set BUNDLE=/path/to/ShortcircuitXT.lv2" >&2
    exit 1
fi
if [ ! -f "$STATE" ]; then
    echo "No state file at $STATE" >&2
    echo "Open and close the Shortcircuit XT editor window once (or save a snapshot) first." >&2
    exit 1
fi

GEN="$BUNDLE/scxt-lv2-ttl"
[ -x "$GEN" ] || GEN="$(find "$HERE/ignore/build-pi" -name scxt-lv2-ttl -type f -perm -u+x 2>/dev/null | head -n1)"
if [ -z "$GEN" ] || [ ! -x "$GEN" ]; then
    echo "scxt-lv2-ttl not found (looked in $BUNDLE and $HERE/ignore/build-pi)" >&2
    exit 1
fi

# keep the number of knobs the bundle has now, unless MAX_PARAMS is given
TTL="$(ls "$BUNDLE"/*.ttl | grep -v manifest.ttl | head -n1 || true)"
if [ -z "${MAX_PARAMS:-}" ] && [ -n "$TTL" ]; then
    MAX_PARAMS="$(grep -c 'lv2:ControlPort' "$TTL" || true)"
fi

ARGS=(--state "$STATE")
[ "${FULL_NAMES:-0}" = "1" ] || ARGS+=(--short-names)

echo "Bundle: $BUNDLE"
echo "State:  $STATE"
cp "$BUNDLE/manifest.ttl" "$BUNDLE/manifest.ttl.bak"
[ -n "$TTL" ] && cp "$TTL" "$TTL.bak"

"$GEN" "${ARGS[@]}" "$BUNDLE/scxt_lv2.so" "$BUNDLE" scxt_lv2.so ${MAX_PARAMS:+"$MAX_PARAMS"}

echo
echo "Knob names now:"
grep -A1 'lv2:symbol "p[0-9]' "$TTL" | grep -o 'lv2:name "[^"]*"' | sed 's/lv2:name //' | head -n 16
echo "(previous .ttl files kept as .bak)"
