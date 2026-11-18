# Shortcircuit XT as LV2 (my_own branch)

Shortcircuit XT is a CLAP plugin; upstream builds CLAP, VST3, AUv2 and a standalone app, but no LV2.
This folder adds a small LV2 wrapper so it can run on a Zynthian (or any LV2 host on aarch64/x86_64).

## How it works

The `ShortcircuitXT.lv2` bundle contains:

| file | what |
|---|---|
| `scxt.clap` | the normal Shortcircuit XT CLAP build |
| `scxt_lv2.so` | the LV2 plugin + LV2 X11 UI (`clap2lv2.cpp`), which loads `scxt.clap` |
| `manifest.ttl`, `shortcircuit_xt.ttl` | written at build time by `clap2lv2-ttl` from the plugin's own parameter list |

- **Ports:** MIDI in (with host tempo/transport), stereo main out, and one control per Shortcircuit XT
  macro knob (16 parts × 16 macros, grouped per part). `MAX_PARAMS=16 ./build_scxt.sh` exposes part 1 only.
- **State:** the complete Shortcircuit XT state (patch, sample paths, macros) is saved in the LV2 state, so
  host presets / Zynthian snapshots restore it. Samples are referenced by path, so keep them at the same
  location on the machine that loads the snapshot.
- **UI:** `ui:X11UI` embedding the normal Shortcircuit XT editor. It needs a host that offers
  `instance-access`, `ui:parent` and `ui:idleInterface` (jalv, Carla, Ardour, Zynthian). Set
  `SCXT_LV2_UI_SCALE=0.75` (or similar) in the host's environment to scale the window.
- Extra outputs (Out 1..20) are not exposed; route everything to the main bus.
- Knob moves made inside the Shortcircuit XT window are not sent back to the LV2 control ports.

## Your own macro names on the LV2 knobs

The knob names live in the .ttl file, so they are fixed until you regenerate it:

1. Name the macros in the Shortcircuit XT editor (right-click a macro knob).
2. Close the editor window. The plugin then writes its setup to
   `~/.local/share/shortcircuit-xt-lv2/last-state.scxtstate` (also on every host state save).
3. Run `generate_ttl.sh` from the bundle (on the Zynthian: `/usr/local/lib/lv2/ShortcircuitXT.lv2/generate_ttl.sh`)
   or from the repository root on the Pi, and restart the host.

`FULL_NAMES=1` keeps names like "P1 M1 - Cutoff"; `MAX_PARAMS=n` changes the number of knobs.

## Build

On the Pi 5, from the repository root:

```bash
./build_scxt.sh
```

The bundle ends up in `dist-linux-aarch64/ShortcircuitXT.lv2`.
