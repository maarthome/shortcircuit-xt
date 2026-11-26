# Shortcircuit XT as LV2 (my_own branch)

Shortcircuit XT is written as a CLAP plugin; upstream builds CLAP, VST3, AUv2 and a standalone app
from it, but no LV2. This folder adds an LV2 build of the same source.

## How it is built

`-DSCXT_BUILD_LV2=ON` (see `src/clients/CMakeLists.txt`) adds this folder to the normal Shortcircuit
XT build. The target `scxt_lv2` links Shortcircuit XT's own plugin code, the `scxt-plugin` library,
into `scxt_lv2.so` and drives it through its plugin interface in the same process. That is the same
way upstream's VST3 is made (clap-wrapper compiles `scxt-plugin` into the VST3 binary). There is no
`.clap` file in the LV2 bundle and no CLAP plugin is loaded at runtime. The engine, the sound and
the editor are the same code as in the CLAP and VST3 versions.

The `ShortcircuitXT.lv2` bundle contains:

| file | what |
|---|---|
| `scxt_lv2.so` | the LV2 plugin and LV2 X11 UI, with Shortcircuit XT compiled in (`clap2lv2.cpp`) |
| `manifest.ttl`, `shortcircuit_xt.ttl` | written at build time by `scxt-lv2-ttl` from the plugin's own parameter list |
| `scxt-lv2-ttl`, `generate_ttl.sh` | tools to rewrite the knob names later (optional) |

## Ports and features

- **Ports:** MIDI in (with host tempo/transport), stereo main out, and one LV2 control port per
  Shortcircuit XT macro knob. Default 16 (part 1, named "Macro 1".."Macro 16"); `MAX_PARAMS=256`
  exposes all 16 parts. Control ports can be shown and assigned to hardware knobs by MOD /
  pi-Stomp and Zynthian.
- **State:** the complete Shortcircuit XT state (patch, sample paths, macros) is saved in the LV2
  state, so host presets, pedalboards and snapshots restore it. Samples are referenced by path.
- **UI:** `ui:X11UI` embedding the normal Shortcircuit XT editor (needs `instance-access`,
  `ui:parent` and `ui:idleInterface`: jalv, Carla, Ardour, Zynthian). MOD does not use it.
  `SCXT_LV2_UI_SCALE=0.75` in the host's environment scales the window.
- **MOD metadata:** `mod:brand`, `mod:label` and LV2 version numbers for MOD / pi-Stomp.
- Extra outputs (Out 1..20) are not exposed; route everything to the main bus.
- Knob moves made inside the Shortcircuit XT window are not sent back to the LV2 control ports.

## Build

On the Pi 5, from the repository root (see `MY_OWN.md` for all options):

```bash
JOBS=4 ./build_scxt.sh
```

The bundle ends up in `dist-linux-aarch64/ShortcircuitXT.lv2`.
