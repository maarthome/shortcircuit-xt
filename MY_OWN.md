# my_own branch — Shortcircuit XT as LV2

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
| `MY_OWN.md` | this document |

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

The bundle includes a panel for MOD-UI, from top to bottom:

- **Group selector** (◀ Group n / N ▶): the group of part 1 that the panel edits.
- **Group settings:** volume, pan, tuning, pitch bend down/up, glide time and mode, velocity
  sensitivity, play mode (poly / piano / mono / legato), note priority, voices, MIDI channel,
  exclusive group.
- **Instruments & samples browser:** any folder on the pi-Stomp. The folder menu has
  ⌂ Instruments (`~/.local/share/ShortCircuit-XT`), ⌂ Home, ⌂ USB / media (`/media`) and your
  ★ saved folders; you can also type a folder (e.g. `/media/usb0/Samples`) and press Enter.
  It shows sub-folders and everything Shortcircuit XT can load (the same list as its own
  browser): instruments `.sfz .sf2 .akp .exs .gig .multisample`, samples
  `.wav .flac .aif .aiff .mp3 .opus`, and Shortcircuit's own `.scp` (part) and `.scm` (multi;
  replaces all parts), with a search box for the current folder. ☆ remembers the current folder (★ forgets it again).
  The last folder you opened comes back next time. Both are kept in
  `~/.config/ShortCircuit-XT/browser.txt` on the pi-Stomp. Choosing a file asks
  "Add to part 1" (new group) or "Replace part 1".
- **Group EG1 (amp envelope):** Dly, A, H, D, S, R.
- **Macros 1–16** (the control ports, also on the pi-Stomp display and knobs).

How it works: the panel sets LV2 patch parameters (`scxt_params.h`). The plugin hands them to a
worker thread (`scxt_bridge.cpp`) that sends the same engine messages as Shortcircuit XT's own
editor: select group, change a group field, change an EG value, clear part, load a sample or
`.sfz`. Every 200 ms it reads the selected group's values back and sends changes to the panel
(atom output port `events_out`), so the panel follows presets, loads and the editor.

The plugin lists the folders itself (the listing goes to the panel in pieces of about 400
characters, because mod-host passes at most about 500 per message) and loads files by their full
path, so new files on the pi-Stomp or a USB stick show up straight away.

Still to come: the 4 effect slots, mod matrix, routing, EG2–5 and LFO1–4.

## Building on the dev Pi 5

First time:

```bash
cd ~/git
git clone -b my_own https://github.com/maarthome/shortcircuit-xt.git
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

The `main` branch of the fork mirrors upstream; `my_own` gets upstream merged in:

```bash
cd ~/git/shortcircuit-xt
git remote add upstream https://github.com/surge-synthesizer/shortcircuit-xt.git   # once
git fetch upstream
git checkout my_own
git merge upstream/main
git push origin my_own
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
