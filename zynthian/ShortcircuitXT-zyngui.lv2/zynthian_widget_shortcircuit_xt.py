# -*- coding: utf-8 -*-
# ******************************************************************************
# Zynthian user widget for the Shortcircuit XT LV2 plugin
# (maarthome/shortcircuit-xt, my_own branch)
#
# Install: copy the ShortcircuitXT-zyngui.lv2 folder (this file + manifest.ttl)
#   to /usr/local/lib/lv2/ on the Zynthian (zynthian-ui "tomita" widget bundles).
#
# Zynthian reads LV2 knob names only once, from the plugin's .ttl. This widget
# watches which preset is loaded and renames the macro knobs (p0..p255) to the
# macro names stored in that preset's Shortcircuit XT state. Macros without a
# name get "P<part> Macro <n>". The widget area lists the named macros.
# ******************************************************************************

import base64
import json
import logging
import os
import re
import tkinter
import tkinter.font
from urllib.parse import unquote, urlparse

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base
from zyngine.zynthian_signal_manager import zynsigman

STATE_KEY = "https://github.com/maarthome/shortcircuit-xt#lv2-clapstate"
STATE_RE = re.compile(r"<" + re.escape(STATE_KEY) + r'>\s*"""(.*?)"""', re.S)
MACRO_SYMBOL_RE = re.compile(r"^p(\d+)$")
MACROS_PER_PART = 16


def preset_path(preset_info):
    """File path of a jalv preset from Zynthian's preset_info, or None."""
    try:
        url = preset_info[0]
    except (TypeError, IndexError):
        return None
    if not isinstance(url, str) or not url.startswith("file://"):
        return None
    return unquote(urlparse(url).path)


_info_cache = {}  # (path, mtime) -> preset info


def read_preset_info(ttl_path):
    """{'names': {(part, macro): name}, 'parts': [(part, part name), ...]} for a
    preset .ttl holding a Shortcircuit XT state; 'parts' are the parts in use."""
    key = (ttl_path, os.path.getmtime(ttl_path))
    if key not in _info_cache:
        _info_cache[key] = _read_preset_info(ttl_path)
    return _info_cache[key]


def _read_preset_info(ttl_path):
    with open(ttl_path, encoding="utf-8") as f:
        m = STATE_RE.search(f.read())
    if not m:
        return {'names': {}, 'parts': []}
    raw = base64.b64decode("".join(m.group(1).split()))
    state = json.loads(raw.rstrip(b"\0").decode("utf-8"))
    names = {}
    parts = []
    for pi, part in enumerate(state.get("patch", {}).get("parts", [])):
        has_names = False
        for mac in part.get("macros", []):
            nm = (mac.get("nm") or "").strip()
            if nm:
                names[(int(mac.get("p", pi)), int(mac.get("i", 0)))] = nm
                has_names = True
        if part.get("groups") or has_names:
            pname = (part.get("names", {}).get("nm") or "").strip()
            parts.append((pi, pname))
    return {'names': names, 'parts': parts}


def knob_name(symbol, names):
    """Knob name for control port symbol p<N>, or None if not a macro port."""
    m = MACRO_SYMBOL_RE.match(symbol)
    if not m:
        return None
    part, mac = divmod(int(m.group(1)), MACROS_PER_PART)
    return names.get((part, mac)) or f"P{part + 1} Macro {mac + 1}"


def screen_layout(orig_screens, names):
    """Control pages for a preset: [[page name, [symbols]], ...]

    orig_screens: the engine's pages ([[name, [symbols]], ...]).
    Page 1 = the non-macro controls of the first page (MIDI Controllers, e.g.
    modulation wheel) + the first named macros up to 4 knobs, then the other
    named macros 4 per page. Unnamed macros ("Macro xx") are left out; other
    non-macro pages are kept after the macro pages.
    Without any named macro the original pages are returned unchanged."""
    if not names:
        return [[n, list(syms)] for n, syms in orig_screens]
    macro_syms = []
    for (part, mac) in sorted(names):
        macro_syms.append(f"p{part * MACROS_PER_PART + mac}")
    first_extra = []
    other_pages = []
    for i, (n, syms) in enumerate(orig_screens):
        plain = [s for s in syms if s and not MACRO_SYMBOL_RE.match(s)]
        if len(plain) != len([s for s in syms if s]):
            continue  # a macro page: replaced by the new macro pages
        if i == 0 and len(plain) < 4:
            first_extra = plain
        else:
            other_pages.append([n, plain])
    pages = []
    first = first_extra + macro_syms[:4 - len(first_extra)]
    rest = macro_syms[4 - len(first_extra):]
    pages.append(["Macros", first])
    for k in range(0, len(rest), 4):
        pages.append([f"Macros {k // 4 + 2}", rest[k:k + 4]])
    return pages + other_pages


def nice_name(zctrl):
    name = zctrl.short_name or zctrl.name or zctrl.symbol
    return name.title() if name.islower() else name


class zynthian_widget_shortcircuit_xt(zynthian_widget_base.zynthian_widget_base):

    MULTI_INSTANCE = True  # one widget per Shortcircuit XT processor

    def __init__(self, parent):
        super().__init__(parent)
        self.applied_key = None  # (preset path, mtime) last applied
        self.names = {}
        self.my_screens = None   # page dict this widget gave the processor
        self.shown_key = None    # knobs + size last drawn
        self.preset_label = ""
        self.pages_pending = False
        self.pending_first = False
        self.widget_canvas = tkinter.Canvas(self, highlightthickness=0, relief='flat',
                                            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')

    def on_size(self, event):
        if event.width == self.width and event.height == self.height:
            return
        super().on_size(event)
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.shown_key = None
        self.show_labels()

    def set_processor(self, processor):
        if processor is not self.processor:
            self.applied_key = None
            self.my_screens = None
        super().set_processor(processor)
        self.apply_preset()
        self.show_labels()

    def refresh_gui(self):
        self.apply_preset()
        self.show_labels()

    # Called on every GUI refresh; only does work when something changed
    def apply_preset(self):
        if not self.processor:
            return
        path = preset_path(getattr(self.processor, "preset_info", None))
        try:
            key = (path, os.path.getmtime(path)) if path else (None, None)
        except OSError:
            key = (path, None)
        preset_changed = key != self.applied_key
        screens_reset = self.processor.ctrl_screens_dict is not self.my_screens
        if not preset_changed and not screens_reset:
            return
        if preset_changed:
            self.applied_key = key
            info = {'names': {}, 'parts': []}
            if path:
                try:
                    info = read_preset_info(path)
                except Exception as e:
                    logging.warning(f"Shortcircuit XT widget: can't read macro names from {path} => {e}")
            self.names = info['names']
            self.preset_label = getattr(self.processor, "preset_name", "") or ""
        self.rename_knobs()
        # Zynthian may be in the middle of building the knob page (this runs
        # from show_widget); change the pages only when it has finished
        self.pending_first = self.pending_first or preset_changed
        if not self.pages_pending:
            self.pages_pending = True
            try:
                self.after_idle(self.apply_pages)
            except Exception:
                self.apply_pages()

    def apply_pages(self):
        self.pages_pending = False
        go_to_first, self.pending_first = self.pending_first, False
        if self.processor:
            self.set_pages(go_to_first)

    def rename_knobs(self):
        for symbol, zctrl in self.processor.controllers_dict.items():
            new = knob_name(symbol, self.names)
            if new and (zctrl.name != new or zctrl.short_name != new):
                zctrl.name = zctrl.short_name = new

    def set_pages(self, go_to_first):
        proc = self.processor
        try:
            orig = getattr(proc.engine, "_ctrl_screens", None) or \
                [[n, [z.symbol for z in zl]] for n, zl in proc.ctrl_screens_dict.items()]
            pages = screen_layout(orig, self.names)
            new_dict = {}
            for name, syms in pages:
                zctrls = proc.build_ctrl_screen(syms)
                if zctrls:
                    new_dict[name] = zctrls
            if not new_dict:
                return
            proc.ctrl_screens_dict = new_dict
            self.my_screens = new_dict
            if go_to_first or not (0 <= proc.current_screen_index < len(new_dict)):
                proc.current_screen_index = 0
            self.redraw_control_screen(proc)
            logging.info(f"Shortcircuit XT: {len(new_dict)} control pages for preset '{self.preset_label}'")
        except Exception as e:
            logging.warning(f"Shortcircuit XT widget: can't set control pages => {e}")

    def redraw_control_screen(self, proc):
        """Rebuild the page list and the knobs on screen for the new pages."""
        ctl = self.zyngui_control
        try:
            if getattr(ctl, "shown", False) and ctl.zyngui.get_current_processor() is proc:
                ctl.update_list()            # page list with the new pages
                ctl.set_controller_screen()  # knobs of the current page
                self.shown_key = None
                self.show_labels()
                return
        except Exception as e:
            logging.warning(f"Shortcircuit XT widget: can't redraw control screen => {e}")
        # not on screen (or failed): let Zynthian refresh when it shows it
        zynsigman.send(zynsigman.S_PROCESSOR, zynsigman.SS_PROCESSOR_CTRL_SCREENS, proc=proc)

    @staticmethod
    def fit_text(text, font, max_w):
        """Shorten text with an ellipsis so it fits max_w pixels on one line."""
        try:
            f = tkinter.font.Font(family=font[0], size=font[1])
            measure = f.measure
        except Exception:
            measure = lambda t: len(t) * font[1] * 0.75  # rough fallback
        if measure(text) <= max_w:
            return text
        while len(text) > 1 and measure(text + "\u2026") > max_w:
            text = text[:-1]
        return text.rstrip() + "\u2026"

    def knob_rows(self):
        """[(name, y centre)] of the knobs on screen, y relative to this widget."""
        rows = []
        try:
            gctrls = list(self.zyngui_control.zgui_controllers)
        except Exception:
            gctrls = []
        n = max(1, len(gctrls))
        for i, gc in enumerate(gctrls):
            zctrl = getattr(gc, "zctrl", None)
            if zctrl is None:
                continue
            try:
                y = gc.winfo_rooty() - self.winfo_rooty() + gc.winfo_height() / 2
                if not (0 <= y <= self.height):
                    raise ValueError
            except Exception:
                y = self.height * (i + 0.5) / n  # knobs stacked in the right column
            rows.append((nice_name(zctrl), y))
        return rows

    def show_labels(self):
        """Name of every knob on screen, level with its block on the right."""
        if self.width < 10 or self.height < 10:
            return
        rows = self.knob_rows()
        key = (tuple(rows), self.width, self.height)
        if key == self.shown_key:
            return
        self.shown_key = key
        c = self.widget_canvas
        c.delete("macros")
        fs = max(8, int(self.height / 4 * 0.28))
        font = (zynthian_gui_config.font_family, fs)
        x0 = int(0.05 * self.width)
        for name, y in rows:
            c.create_text(x0, y, anchor="w", font=font, tags="macros", fill=zynthian_gui_config.color_tx,
                          text=self.fit_text(name, font, self.width - 2 * x0))

# ------------------------------------------------------------------------------
