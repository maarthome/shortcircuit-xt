# My_Changes branch — Shortcircuit XT as LV2

This branch of [maarthome/shortcircuit-xt](https://github.com/maarthome/shortcircuit-xt) adds an LV2
build of [Shortcircuit XT](https://github.com/surge-synthesizer/shortcircuit-xt) for the
Raspberry Pi 5 (aarch64, Debian trixie), used on a pi-Stomp (MOD) and a Zynthian.

## What is changed

| File | Change |
|---|---|
| `src/clients/CMakeLists.txt` | option `SCXT_BUILD_LV2` (off by default) adds `src/clients/lv2` to the build — the only change to an upstream file |
| `src/clients/lv2/clap2lv2.cpp` | the LV2 plugin and X11 UI, with Shortcircuit XT's plugin code linked in |
| `src/clients/lv2/clap_host.h` | the host side the plugin code talks to (threads, timers, editor window) |
| `src/clients/lv2/scxt_params.h` | the panel's LV2 parameters (group, file browser, group settings, EG1) |
| `src/clients/lv2/scxt_bridge.cpp`, `.h` | turns panel parameter changes into the engine messages Shortcircuit XT's editor sends, and reports values back |
| `mod/modgui.ttl`, `mod/modgui/` | the MOD panel (pi-Stomp) |
| `src/clients/lv2/clap2lv2_ttl.cpp` | `scxt-lv2-ttl`: writes the bundle's `.ttl` files from the plugin's own knob list |
| `src/clients/lv2/CMakeLists.txt` | targets `scxt_lv2` and `scxt-lv2-ttl` |
| `src/clients/lv2/README.md` | technical description |
| `build_scxt.sh` | build, package and install script for the Pi 5 |
| `generate_ttl.sh` | rewrite the knob names from a saved state |
| `zynthian/ShortcircuitXT-zyngui.lv2/` | Zynthian widget: knob names and pages follow the loaded preset |
| `zynthian/scxt_macro_names.py` | older tool: fixed knob names from all Zynthian presets |
| `MY_CHANGES.md` | this document |

### How the LV2 is made

Shortcircuit XT is written as a CLAP plugin. Upstream makes its other formats with clap-wrapper,
which compiles Shortcircuit's plugin code (the `scxt-plugin` library) straight into the VST3 or AU
binary. clap-wrapper has no LV2, so this branch does the same for LV2 itself: `scxt_lv2.so` links
`scxt-plugin` and drives it directly in the same process. There is no `.clap` file in the bundle and
no CLAP plugin is loaded at runtime; the sound, the engine and the editor are the same code as in
the CLAP and VST3 versions.

The plugin has:

- MIDI in (with host tempo/transport), stereo main out;
- one LV2 control port per macro: 16 by default (part 1, "Macro 1".."Macro 16"), 256 with
  `MAX_PARAMS=256`. These ports appear on the pi-Stomp display and can be assigned to its knobs;
- the whole Shortcircuit XT state in the LV2 state (presets, pedalboards, snapshots);
- the Shortcircuit XT editor as LV2 X11 UI (Zynthian over VNC, jalv, Carla; not MOD);
- LV2 URI `https://github.com/maarthome/shortcircuit-xt#lv2`, bundle `ShortcircuitXT.lv2`.

### The MOD panel (pi-Stomp)

The bundle includes a panel for MOD-UI. It is wide and low (1650 × 1010) to suit a wide screen,
with three rows: group settings | browser | part 1, then group EG | group LFO (+ MISC, AUDIO) |
mod matrix, then group effects | routing, and the 16 macros as one strip along the bottom. The parts:

- **Group selector** (◀ list ▶): **All groups** (the default: every edit goes to all groups of
  part 1, like a multi-selection in Shortcircuit; the panel shows group 1's values) or one
  group, listed with its name.
- **Group settings:** volume, pan, tuning, pitch bend down/up, glide time and mode, velocity
  sensitivity, play mode (poly / piano / mono / legato), note priority, voices, MIDI channel,
  exclusive group.
- **Instruments & samples browser:** two buttons, **Instruments** (`.sfz .sf2 .akp .exs .gig
  .multisample`, and Shortcircuit's own `.scp` part / `.scm` multi, which replace) and **Waves**
  (`.wav .flac .aif .aiff .mp3 .opus`), and one drop-down list with the current folder:
  `..`, its sub-folders and the files of the chosen kind, then "Go to": ⌂ Instruments
  (`~/.local/share/ShortCircuit-XT`), ⌂ Home, ⌂ USB / media (`/media`), your ★ saved folders
  and ☆ Remember / ✖ Forget this folder. The list stays open while you open folders (also
  `..`) or switch Instruments/Waves; choosing a file closes it and asks "Add to part 1" (new group), "Replace group N" (only when one group is
  selected: a wave replaces that group's samples and the group keeps its settings and takes the
  wave's name; an instrument takes the group's place), "Replace part 1" or "Cancel". The last folder and the saved
  folders are kept in `~/.config/ShortCircuit-XT/browser.txt` on the pi-Stomp.
  Below it, **Loaded in part 1**: each group with its name and its samples (zones); click
  one to edit that group. Each row has **M** (mute), **S** (solo), **✎** (rename) and **✕**
  (delete, asks first). Groups you do not hear are struck through: the muted ones, or while
  a group is soloed every group that is not soloed.
- **Part 1** (top right), with tabs:
  - **Settings:** volume, pan, tuning, transpose, MIDI channel (omni or 1–16), voices (no
    limit or 1–256), mute, and the **sends** to aux 1–4 (Shortcircuit's mixer page).
  - **FX 1–4:** part 1's own effects (the mixer page's part effects): on/off, type (reverb 1/2,
    flanger, phaser, dual delay, treemonster, nimbus, rotary speaker, floaty delay, bonsai),
    tempo sync and the effect's controls with Shortcircuit's value texts. The tab shows the
    effect's name.
  - **Save .scp:** saves part 1 with all your edits as a Shortcircuit part in the Instruments
    folder (or a full path you type), so it loads back exactly. Normally the file refers to the
    samples where they are; tick "put the samples in the file too" for one file that can be
    moved anywhere.
- **Routing** (as in Shortcircuit): S1 S2 S3 P1 P2 P3 BYP with a drawing of the 4 effect
  blocks (their mix as a bar, grey when empty or off), Vel (velocity sensitivity), Volume,
  Pan, output (Part Output, Main, Bus 1–16, Aux 1–4) and HQ (oversampling off / auto / on).
- **Group EG 1–5**, as in Shortcircuit's group view. They are modulation sources: they only
  change the sound through a mod matrix row (e.g. "Group EG 1 → Output › Amplitude"). Each has
  Dly A H D S R sliders, shape knobs above A, D and R, gate mode (gated / no sustain /
  oneshot), tempo sync (times then show as note lengths) and "gate on voice".
- **Group LFO 1–4:** shape (sine, ramp, tri, saw-ramp, square-tri, noise, S&H, env, step) with
  a picture, UNI, ENV (delay/attack/release envelope, sliders D A R), Rate, Phase, Deform,
  Angle, tempo sync and the trigger (keytrig, songpos, random, release, oneshot). The ENV shape
  shows its DAHDSR and loop instead; step sequences are edited in Shortcircuit's own editor.
  Next to the GLFO tabs: **MISC** (phasors A–D: voice or song sync, rate as n/m, note /
  triplet / dotted / x BPM, up or down, phase; random sources A–D: unipolar, bipolar, normal,
  half-normal, boolean +/−, ternary) and **AUDIO** (envelope followers A and B: attack,
  release, gain, follow pre- or post-effects, stereo link). These are mod matrix sources.
- **Group effects FX1–FX4:** per slot an on/off button and the effect type (Shortcircuit's own
  list, grouped: filters, delay, distortion, EQ, modulation, reverb, generators, ...). Below it
  that effect's own controls with their names, ranges and value texts from Shortcircuit
  (e.g. Cutoff "43.65 Hz"), Mix and Level knobs, and its choice settings (e.g. Passband) as
  drop-downs. Double-click a knob for its default.
- **Group mod matrix:** the group's 18 rows, each with on/off, source, via, curve, target and
  depth (drag left/right, Shift for fine, double-click for 0). The lists are Shortcircuit's own
  (LFOs, EGs, MIDI, macros, random, ... and every effect parameter as target, shown as
  "P1 Fast SVF › Cutoff").
- **Macros 1–16**, one strip along the bottom (the control ports, also on the pi-Stomp display
  and knobs). Click a macro's name to rename it; the name is saved with the part.

How it works: the panel sets LV2 patch parameters (`scxt_params.h`). The plugin hands them to a
worker thread (`scxt_bridge.cpp`) that sends the same engine messages as Shortcircuit XT's own
editor: select group, change a group field, change an EG value, set an effect type or value,
change a mod matrix row, clear part, load a sample or `.sfz`. Every 200 ms it reads the selected group's values back and sends changes to the panel
(atom output port `events_out`), so the panel follows presets, loads and the editor. The effect
controls (names, ranges, choices) are copied in the audio thread between two process calls when
an effect type changes, and sent to the panel as text.

The plugin lists the folders itself (the listing goes to the panel in pieces of about 400
characters, because mod-host passes at most about 500 per message) and loads files by their full
path, so new files on the pi-Stomp or a USB stick show up straight away.

Still to come: selecting files with the pi-Stomp's own display and buttons.

## Building on the dev Pi 5

First time:

```bash
cd ~/git
git clone -b My_Changes https://github.com/maarthome/shortcircuit-xt.git
cd shortcircuit-xt
JOBS=4 ./build_scxt.sh
```

The script installs the build packages, updates the submodules, adds a temporary 6 GB swap file,
builds and assembles `dist-linux-aarch64/ShortcircuitXT.lv2`. While compiling it shows one line
with the percentage, the step count and an estimate of the time left; the compiler output goes to
`ignore/build-pi.log` (on an error the script shows the error lines and the end of that log). The first build takes hours; later
builds only recompile what changed.

### Options

| Option | Effect |
|---|---|
| `--no-deps` | don't install the build packages (same as `SKIP_DEPS=1`) |
| `--deploy` | install on the pi-Stomp over ssh (needs `PISTOMP=user@host`) and restart mod-host and mod-ui |
| `--lv2-dir DIR` | LV2 folder on the pi-Stomp (default `~/.lv2`), or the install folder of the `.deb` (default `/usr/lib/lv2`); also `LV2_DIR=DIR` |
| `--zynthian` | install on the Zynthian (`ZYNTHIAN=root@zynthian.local`) with the widget and restart Zynthian |
| `--deb` | also make `dist/shortcircuit-xt-lv2_<version>_arm64.deb` |
| `--deb-only` | only make the `.deb` from the last build |
| `--instruments DIR` | copy the instruments in DIR (sfz, samples, subfolders) to the pi-Stomp's `~/.local/share/ShortCircuit-XT` (with `--deploy`) or into the `.deb` as `/usr/share/ShortCircuit-XT` (with `--deb`) — next to Surge's `~/.local/share/surge-xt` |
| `--with-clap` | also build the plain CLAP (`dist-linux-aarch64/Shortcircuit XT.clap`) |
| `JOBS=n` | parallel compile jobs |
| `VERBOSE=1` | show all compiler output instead of the progress line (the full output is always in `ignore/build-pi.log`) |
| `SWAP_GB=n` | temporary swap size (default 6, 0 = none) |
| `MAX_PARAMS=n` | number of macro control ports (default 16) |

## Installing on the pi-Stomp

From the dev Pi 5 (once: `ssh-copy-id pistomp@pistomp.local`):

```bash
cd ~/git/shortcircuit-xt
JOBS=4 PISTOMP=pistomp@pistomp.local ./build_scxt.sh --deploy
```

This copies the bundle to `~/.lv2` on the pi-Stomp and restarts mod-host and mod-ui. Then add
"Shortcircuit XT" (brand Surge Synth Team) to a pedalboard. Check the ports on the pi-Stomp:

```bash
ssh pistomp@pistomp.local 'grep -c ControlPort ~/.lv2/ShortcircuitXT.lv2/shortcircuit_xt.ttl'
```

It prints 16 (one per macro). Samples must be on the pi-Stomp at the same path as when the patch was
saved.

Instruments for the panel's instrument browser live on the pi-Stomp in
`~/.local/share/ShortCircuit-XT` (next to Surge's `~/.local/share/surge-xt`). Copy a
folder of instruments there from the dev Pi:

```bash
JOBS=4 PISTOMP=pistomp@pistomp.local ./build_scxt.sh --deploy --instruments ~/instruments
```

With a package instead:

```bash
JOBS=4 ./build_scxt.sh --deb --lv2-dir /home/pistomp/.lv2
scp dist/shortcircuit-xt-lv2_*.deb pistomp@pistomp.local:
ssh -t pistomp@pistomp.local 'sudo apt install ./shortcircuit-xt-lv2_*.deb'
```

## Installing on the Zynthian

```bash
cd ~/git/shortcircuit-xt
JOBS=4 ./build_scxt.sh --zynthian
```

This copies `ShortcircuitXT.lv2` and the widget `ShortcircuitXT-zyngui.lv2` to
`/usr/local/lib/lv2` and restarts Zynthian. The first time, enable "Shortcircuit XT" in webconf
(Software > LV2 Plugins). The widget (zynthian-ui `tomita`) renames the knobs to the macro names of
the loaded preset and puts only named macros on the control pages.

## Knob names

- **Zynthian:** handled per preset by the widget.
- **Fixed names in the `.ttl`** (pi-Stomp, other hosts): name the macros in the Shortcircuit XT
  editor, close the editor window (the plugin then saves its setup to
  `~/.local/share/shortcircuit-xt-lv2/last-state.scxtstate`) and run `generate_ttl.sh` in the
  bundle, then restart the host.

## Syncing with the original project

The `main` branch of the fork mirrors upstream; `My_Changes` gets upstream merged in:

```bash
cd ~/git/shortcircuit-xt
git remote add upstream https://github.com/surge-synthesizer/shortcircuit-xt.git   # once
git fetch upstream
git checkout My_Changes
git merge upstream/main
git push origin My_Changes
git push origin upstream/main:main
```

Then rebuild (`JOBS=4 ./build_scxt.sh ...`). The only upstream file this branch changes is
`src/clients/CMakeLists.txt` (a few lines at the end), so merges rarely conflict. After a merge,
check `src/scxt-plugin/clap/` for changes in how the plugin talks to its host.

## Troubleshooting

- Plugin not listed: check the bundle on the dev Pi with
  `LV2_PATH=~/git/shortcircuit-xt/dist-linux-aarch64 lv2ls`.
- pi-Stomp: restart MOD after installing (`sudo systemctl restart mod-host mod-ui`); the build
  script does this with `--deploy`.
- Zynthian log:
  `journalctl -u zynthian -b --no-pager | grep -i -E "shortcircuit|zynthian widget" | tail -20`.
