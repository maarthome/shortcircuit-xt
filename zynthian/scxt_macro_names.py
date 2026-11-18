#!/usr/bin/env python3
"""
scxt_macro_names.py - give the Shortcircuit XT LV2 knobs on the Zynthian the
macro names used in your Shortcircuit XT presets.

Reads the Zynthian preset list for the plugin
    /zynthian/config/jalv/presets_Shortcircuit_XT.json
decodes the Shortcircuit XT state stored in every preset .ttl it lists, and
writes the macro names into the plugin's .ttl in
    /usr/local/lib/lv2/ShortcircuitXT.lv2/

Zynthian reads knob names only when it starts, so the names are the same for
every preset: a macro named differently in several presets gets all its names,
joined with " / ". Unnamed macros keep "P<part> Macro <n>".

Usage:
    scxt_macro_names.py                 update the names
    scxt_macro_names.py --restart       update, and restart Zynthian if changed
    scxt_macro_names.py --dry-run       only show what the names would be
    scxt_macro_names.py --install       run automatically after every preset save
    scxt_macro_names.py --uninstall     remove the automatic run

Part of the maarthome/shortcircuit-xt "my_own" branch.
"""

import argparse
import base64
import binascii
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from urllib.parse import unquote, urlparse

PRESETS_JSON = "/zynthian/config/jalv/presets_Shortcircuit_XT.json"
BUNDLE = "/usr/local/lib/lv2/ShortcircuitXT.lv2"
STATE_KEY = "https://github.com/maarthome/shortcircuit-xt#lv2-clapstate"
MACROS_PER_PART = 16
UNIT_NAME = "scxt-macro-names"

STATE_RE = re.compile(r"<" + re.escape(STATE_KEY) + r'>\s*"""(.*?)"""', re.S)


def preset_files(presets_json):
    with open(presets_json) as f:
        info = json.load(f)
    files = []
    for bank in info.values():
        for p in bank.get("presets", []):
            url = p.get("url") or ""
            if url.startswith("file://"):
                files.append((p.get("label", ""), unquote(urlparse(url).path)))
    return files


def read_state(ttl_path):
    with open(ttl_path, encoding="utf-8") as f:
        m = STATE_RE.search(f.read())
    if not m:
        return None
    try:
        raw = base64.b64decode("".join(m.group(1).split()))
    except (binascii.Error, ValueError):
        return None
    return json.loads(raw.rstrip(b"\0").decode("utf-8"))


def collect_names(files, verbose=True):
    """{(part, macro): [names in preset order]}"""
    names = {}
    for label, path in files:
        try:
            state = read_state(path)
        except (OSError, ValueError) as e:
            print(f"  skip {label}: {e}", file=sys.stderr)
            continue
        if not state:
            print(f"  skip {label}: no Shortcircuit XT state in {path}", file=sys.stderr)
            continue
        found = []
        for part in state.get("patch", {}).get("parts", []):
            for m in part.get("macros", []):
                nm = (m.get("nm") or "").strip()
                if nm:
                    key = (int(m.get("p", 0)), int(m.get("i", 0)))
                    lst = names.setdefault(key, [])
                    if nm not in lst:
                        lst.append(nm)
                    found.append(f"P{key[0] + 1} M{key[1] + 1}={nm}")
        if verbose:
            print(f"  {label}: {', '.join(found) if found else 'no named macros'}")
    return names


def plugin_ttl(bundle):
    for f in sorted(glob.glob(os.path.join(bundle, "*.ttl"))):
        if os.path.basename(f) != "manifest.ttl":
            return f
    raise FileNotFoundError(f"no plugin .ttl in {bundle}")


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def rewrite_ttl(text, names):
    """Set lv2:name of every control port p<N> (N = part * 16 + macro)."""
    changed = []

    def repl(m):
        idx = int(m.group(2))
        part, mac = divmod(idx, MACROS_PER_PART)
        lst = names.get((part, mac))
        new = " / ".join(lst) if lst else f"P{part + 1} Macro {mac + 1}"
        if new != m.group(4):
            changed.append((idx, m.group(4), new))
        return f'{m.group(1)}{m.group(2)}{m.group(3)}{esc(new)}"'

    out = re.sub(r'(lv2:symbol "p)(\d+)("\s*;\s*lv2:name ")((?:[^"\\]|\\.)*)"', repl, text)
    return out, changed


def install(script):
    target = "/usr/local/bin/scxt_macro_names.py"
    if os.path.abspath(script) != target:
        shutil.copy2(script, target)
        os.chmod(target, 0o755)
    with open(f"/etc/systemd/system/{UNIT_NAME}.path", "w") as f:
        f.write(f"""[Unit]
Description=Update Shortcircuit XT LV2 knob names when its presets change

[Path]
PathChanged={PRESETS_JSON}
Unit={UNIT_NAME}.service

[Install]
WantedBy=multi-user.target
""")
    with open(f"/etc/systemd/system/{UNIT_NAME}.service", "w") as f:
        f.write(f"""[Unit]
Description=Update Shortcircuit XT LV2 knob names from its presets

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 {target}
""")
    subprocess.run(["systemctl", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "enable", "--now", f"{UNIT_NAME}.path"], check=True)
    print(f"Installed: knob names are updated after every Shortcircuit XT preset save.")
    print("They show up on the Zynthian after its next restart.")


def uninstall():
    subprocess.run(["systemctl", "disable", "--now", f"{UNIT_NAME}.path"], check=False)
    for ext in ("path", "service"):
        try:
            os.remove(f"/etc/systemd/system/{UNIT_NAME}.{ext}")
        except FileNotFoundError:
            pass
    subprocess.run(["systemctl", "daemon-reload"], check=False)
    print("Automatic update removed.")


def main():
    ap = argparse.ArgumentParser(description="Shortcircuit XT macro names -> Zynthian knob names")
    ap.add_argument("--presets-json", default=PRESETS_JSON)
    ap.add_argument("--bundle", default=BUNDLE)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--restart", action="store_true", help="restart Zynthian if names changed")
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--uninstall", action="store_true")
    a = ap.parse_args()

    if a.install:
        install(os.path.abspath(__file__))
    if a.uninstall:
        uninstall()
        return 0

    print(f"Presets: {a.presets_json}")
    names = collect_names(preset_files(a.presets_json))

    ttl = plugin_ttl(a.bundle)
    with open(ttl, encoding="utf-8") as f:
        text = f.read()
    out, changed = rewrite_ttl(text, names)

    if not changed:
        print("Knob names already up to date.")
        return 0
    for idx, old, new in changed:
        print(f"  p{idx}: {old} -> {new}")
    if a.dry_run:
        print("(dry run, nothing written)")
        return 0

    shutil.copy2(ttl, ttl + ".bak")
    tmp = ttl + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(out)
    os.replace(tmp, ttl)
    print(f"Updated {ttl} (previous version: {os.path.basename(ttl)}.bak)")

    if a.restart:
        subprocess.run(["systemctl", "restart", "zynthian"], check=False)
        print("Zynthian restarted.")
    else:
        print("The new names show up after the next Zynthian restart.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
