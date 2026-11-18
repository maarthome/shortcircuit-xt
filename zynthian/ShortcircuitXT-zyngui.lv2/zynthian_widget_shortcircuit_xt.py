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
from urllib.parse import unquote, urlparse

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

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


_names_cache = {}  # (path, mtime) -> names


def read_macro_names(ttl_path):
    """{(part, macro): name} from a preset .ttl holding a Shortcircuit XT state."""
    key = (ttl_path, os.path.getmtime(ttl_path))
    if key not in _names_cache:
        _names_cache[key] = _read_macro_names(ttl_path)
    return _names_cache[key]


def _read_macro_names(ttl_path):
    with open(ttl_path, encoding="utf-8") as f:
        m = STATE_RE.search(f.read())
    if not m:
        return {}
    raw = base64.b64decode("".join(m.group(1).split()))
    state = json.loads(raw.rstrip(b"\0").decode("utf-8"))
    names = {}
    for part in state.get("patch", {}).get("parts", []):
        for mac in part.get("macros", []):
            nm = (mac.get("nm") or "").strip()
            if nm:
                names[(int(mac.get("p", 0)), int(mac.get("i", 0)))] = nm
    return names


def knob_name(symbol, names):
    """Knob name for control port symbol p<N>, or None if not a macro port."""
    m = MACRO_SYMBOL_RE.match(symbol)
    if not m:
        return None
    part, mac = divmod(int(m.group(1)), MACROS_PER_PART)
    return names.get((part, mac)) or f"P{part + 1} Macro {mac + 1}"


class zynthian_widget_shortcircuit_xt(zynthian_widget_base.zynthian_widget_base):

    MULTI_INSTANCE = True  # one widget per Shortcircuit XT processor

    def __init__(self, parent):
        super().__init__(parent)
        self.applied_key = None  # (preset path, mtime) last applied
        self.names = {}
        self.preset_label = ""

        self.widget_canvas = tkinter.Canvas(self, highlightthickness=0, relief='flat',
                                            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.title_text = self.widget_canvas.create_text(
            0, 0, anchor="nw", fill=zynthian_gui_config.color_tx_off, text="Macros")
        self.list_text = self.widget_canvas.create_text(
            0, 0, anchor="nw", fill=zynthian_gui_config.color_tx, text="")

    def on_size(self, event):
        if event.width == self.width and event.height == self.height:
            return
        super().on_size(event)
        x0 = int(0.05 * self.width)
        fs = max(8, self.height // 16)
        self.widget_canvas.coords(self.title_text, x0, int(0.05 * self.height))
        self.widget_canvas.itemconfig(self.title_text, font=(zynthian_gui_config.font_family, fs))
        self.widget_canvas.coords(self.list_text, x0, int(0.05 * self.height) + 2 * fs)
        self.widget_canvas.itemconfig(self.list_text, font=(zynthian_gui_config.font_family, int(fs * 0.8)),
                                      width=int(0.9 * self.width))
        self.widget_canvas.grid(row=0, column=0, sticky='news')

    def set_processor(self, processor):
        if processor is not self.processor:
            self.applied_key = None
        super().set_processor(processor)
        self.apply_preset_names()

    def refresh_gui(self):
        self.apply_preset_names()

    # Called often (every GUI refresh); only does work when the preset changed
    def apply_preset_names(self):
        if not self.processor:
            return
        path = preset_path(getattr(self.processor, "preset_info", None))
        try:
            key = (path, os.path.getmtime(path)) if path else (None, None)
        except OSError:
            key = (path, None)
        if key == self.applied_key:
            return
        self.applied_key = key

        names = {}
        if path:
            try:
                names = read_macro_names(path)
            except Exception as e:
                logging.warning(f"Shortcircuit XT widget: can't read macro names from {path} => {e}")
        self.names = names
        self.preset_label = getattr(self.processor, "preset_name", "") or ""
        self.rename_knobs()
        self.show_names()

    def rename_knobs(self):
        changed = set()
        for symbol, zctrl in self.processor.controllers_dict.items():
            new = knob_name(symbol, self.names)
            if new and (zctrl.name != new or zctrl.short_name != new):
                zctrl.name = zctrl.short_name = new
                changed.add(symbol)
        if not changed:
            return
        # redraw the titles of the knobs currently on screen
        try:
            for zgui_ctrl in self.zyngui_control.zgui_controllers:
                if zgui_ctrl.zctrl is not None and zgui_ctrl.zctrl.symbol in changed:
                    zgui_ctrl.title = zgui_ctrl.zctrl.short_name
                    zgui_ctrl.set_title(zgui_ctrl.title)
        except Exception as e:
            logging.warning(f"Shortcircuit XT widget: can't refresh knob titles => {e}")
        logging.info(f"Shortcircuit XT: renamed {len(changed)} macro knobs for preset '{self.preset_label}'")

    def show_names(self):
        title = f"Macros - {self.preset_label}" if self.preset_label else "Macros"
        lines = [f"P{p + 1} M{m + 1}: {nm}" for (p, m), nm in sorted(self.names.items())]
        self.widget_canvas.itemconfig(self.title_text, text=title)
        self.widget_canvas.itemconfig(self.list_text, text="\n".join(lines) if lines else "(no named macros)")

# ------------------------------------------------------------------------------
