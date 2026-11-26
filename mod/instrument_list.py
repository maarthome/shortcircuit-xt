#!/usr/bin/env python3
"""
instrument_list.py - make the instrument list of the Shortcircuit XT MOD panel.

Reads relative file paths (one per line, as from `find . -type f -printf "%P\\n"`
in the instruments folder) on stdin and writes, into the LV2 bundle given as
argument:
  instruments.txt                        the list the plugin loads from
  modgui/javascript-shortcircuit-xt.js   the same list for the panel

Listed: every .sfz file, and audio files that are not part of an SFZ
instrument: not in a folder (below the top one) that holds a .sfz, and not
in a folder that has a .sfz of the same name next to it ("X.sfz" + "X/").
Part of the maarthome/shortcircuit-xt "my_own" branch.
"""
import json
import os
import re
import sys

AUDIO = ('.wav', '.flac', '.aif', '.aiff', '.ogg', '.mp3')


def natural(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', s)]


def main():
    bundle = sys.argv[1]
    paths = [p.strip() for p in sys.stdin.read().splitlines() if p.strip()]
    paths = [p[2:] if p.startswith('./') else p for p in paths]
    sfz = {p.lower() for p in paths if p.lower().endswith('.sfz')}
    sfz_dirs = {os.path.dirname(p) for p in sfz}

    def in_sfz_instrument(p):
        # a folder (not the top one) with a .sfz in it, or with "<folder>.sfz" next to it
        d = os.path.dirname(p).lower()
        while d:
            if d in sfz_dirs or d + '.sfz' in sfz:
                return True
            d = os.path.dirname(d)
        return False

    items = [p for p in paths if p.lower().endswith('.sfz')
             or (p.lower().endswith(AUDIO) and not in_sfz_instrument(p))]
    items.sort(key=natural)

    with open(os.path.join(bundle, 'instruments.txt'), 'w') as f:
        f.write('# Shortcircuit XT instruments, relative to ~/.local/share/shortcircuit-xt/instruments\n')
        f.write('# (or /usr/share/shortcircuit-xt/instruments); the panel uses the same order\n')
        for p in items:
            f.write(p + '\n')

    js = os.path.join(bundle, 'modgui', 'javascript-shortcircuit-xt.js')
    s = open(js, encoding='utf-8').read()
    s = re.sub(r'/\*__INSTRUMENTS__\*/\[.*?\];', '/*__INSTRUMENTS__*/' + json.dumps(items) + ';', s, flags=re.S)
    open(js, 'w', encoding='utf-8').write(s)
    print(f'{len(items)} instruments in the panel list')


if __name__ == '__main__':
    main()
