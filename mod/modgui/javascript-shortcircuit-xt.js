function (event, funcs) {
    // Shortcircuit XT panel for MOD (maarthome/shortcircuit-xt, my_own branch).
    // Group settings, the file browser and EG1 are LV2 patch parameters (scxt_params.h in
    // the plugin source); the 16 macros are the plugin's control ports p0..p15.
    // The plugin lists folders itself (browse -> listing chunks) and remembers the last
    // folder and the saved folders in ~/.config/ShortCircuit-XT/browser.txt on the device.
    var BASE = 'https://github.com/maarthome/shortcircuit-xt#lv2-';

    function U(key) { return BASE + key; }
    function expTime(x) { return (Math.exp(0.6931471824646 + x * (10.1267113685608 - 0.6931471824646)) - 2) / 1000; }
    function fmtTime(x) { var s = expTime(x); if (s < 0.001) return '0 ms'; return s < 1 ? (s * 1000).toFixed(s < 0.01 ? 1 : 0) + ' ms' : s.toFixed(s < 10 ? 2 : 1) + ' s'; }
    function fmtDb(v) { if (v <= 0.0001) return '-inf'; var db = 60 * Math.log(v) / Math.LN10; return (db > 0 ? '+' : '') + db.toFixed(2) + ' dB'; }
    function fmtPan(v) { if (Math.abs(v) < 0.005) return 'C'; return (v < 0 ? 'L ' : 'R ') + Math.round(Math.abs(v) * 100); }
    function fmtSt(v) { return (v > 0 ? '+' : '') + v.toFixed(2) + ' st'; }
    function fmtPct(v) { return Math.round(v * 100) + '%'; }
    function range(a, b, f) { var r = []; for (var i = a; i <= b; i++) r.push([i, f(i)]); return r; }

    // [key, label, kind, min, max, default, format or option list]
    var GROUP = [
        ['group_volume', 'Volume', 'k', 0, 1.5849, 1, fmtDb],
        ['group_pan', 'Pan', 'k', -1, 1, 0, fmtPan],
        ['group_tuning', 'Tuning', 'k', -24, 24, 0, fmtSt],
        ['group_pb_down', 'Bend down', 'k', -48, 0, -2, fmtSt],
        ['group_pb_up', 'Bend up', 'k', 0, 48, 2, fmtSt],
        ['group_glide_time', 'Glide', 'k', 0, 1, 0, fmtTime],
        ['group_vel_sens', 'Velocity', 'k', 0, 1, 0.6, fmtPct],
        ['group_glide_mode', 'Glide', 's', 0, 1, 0, [[0, 'TIME'], [1, 'RATE']]],
        ['group_play_mode', 'Mode', 's', 0, 3, 0, [[0, 'POLY'], [3, 'PIANO'], [1, 'MONO'], [2, 'LEGATO']]],
        ['group_note_priority', 'Priority', 's', 0, 2, 0, [[0, 'LAST'], [1, 'HIGH'], [2, 'LOW']]],
        ['group_poly_limit', 'Voices', 's', 0, 128, 0, [[0, 'PART']].concat([1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128].map(function (n) { return [n, '' + n]; }))],
        ['group_midi_channel', 'MIDI', 's', -1, 15, -1, [[-1, 'PART']].concat(range(0, 15, function (i) { return 'CH ' + (i + 1); }))],
        ['group_exclusive', 'Excl.', 's', 0, 32, 0, [[0, 'OFF']].concat(range(1, 16, function (i) { return '' + i; }))]
    ];
    var EG = [
        ['eg1_delay', 'Dly', 0, fmtTime], ['eg1_attack', 'A', 0, fmtTime], ['eg1_hold', 'H', 0, fmtTime],
        ['eg1_decay', 'D', 0, fmtTime], ['eg1_sustain', 'S', 1, fmtPct], ['eg1_release', 'R', 0.5, fmtTime]
    ];

    function el(tag, cls, parent, text) {
        var e = document.createElement(tag);
        if (cls) e.className = cls;
        if (text !== undefined) e.textContent = text;
        if (parent) parent.appendChild(e);
        return e;
    }

    // ---- a control bound to a patch parameter (uri) or a port (symbol) ----
    function send(d, c, v) {
        c.local = Date.now();
        if (c.uri) funcs.patch_set(c.uri, 'f', v);
        else funcs.set_port_value(c.symbol, v);
    }

    function arcPath(f) {
        var a0 = 0.75 * Math.PI, a1 = a0 + f * 1.5 * Math.PI, r = 19, cx = 24, cy = 24;
        var x0 = cx + r * Math.cos(a0), y0 = cy + r * Math.sin(a0), x1 = cx + r * Math.cos(a1), y1 = cy + r * Math.sin(a1);
        return 'M' + x0 + ' ' + y0 + ' A' + r + ' ' + r + ' 0 ' + (f > 2 / 3 ? 1 : 0) + ' 1 ' + x1 + ' ' + y1;
    }

    function makeKnob(d, parent, spec) {
        // spec: {uri|symbol, label, min, max, def, fmt}
        var c = spec;
        c.el = el('div', 'scx-knob', parent);
        c.el.innerHTML = '<svg viewBox="0 0 48 48"><path class="bg" d="' + arcPath(1) + '" stroke="#3a4048" stroke-width="5" fill="none" stroke-linecap="round"/>' +
            '<path class="fg" stroke="#f0a83a" stroke-width="5" fill="none" stroke-linecap="round"/>' +
            '<circle cx="24" cy="24" r="12" fill="#2d3238"/><line class="ptr" x1="24" y1="24" stroke="#eef1f4" stroke-width="2.5" stroke-linecap="round"/></svg>';
        c.lbl = el('div', 'scx-lbl', c.el, c.label);
        c.valEl = el('div', 'scx-val', c.el, '');
        c.v = c.def;
        c.show = function () {
            var f = (c.v - c.min) / (c.max - c.min); f = Math.max(0, Math.min(1, f));
            var fg = c.el.querySelector('.fg'), ptr = c.el.querySelector('.ptr');
            fg.setAttribute('d', f > 0.002 ? arcPath(f) : '');
            var a = 0.75 * Math.PI + f * 1.5 * Math.PI;
            ptr.setAttribute('x2', 24 + 10 * Math.cos(a)); ptr.setAttribute('y2', 24 + 10 * Math.sin(a));
            c.valEl.textContent = c.fmt ? c.fmt(c.v) : c.v.toFixed(2);
        };
        var startY = 0, startV = 0, dragging = false;
        function down(e) {
            var p = e.touches ? e.touches[0] : e;
            dragging = true; startY = p.clientY; startV = c.v; e.preventDefault(); e.stopPropagation();
            document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
            document.addEventListener('touchmove', move, {passive: false}); document.addEventListener('touchend', up);
        }
        function move(e) {
            if (!dragging) return;
            var p = e.touches ? e.touches[0] : e;
            var fine = e.shiftKey ? 0.2 : 1;
            var v = startV + (startY - p.clientY) / 200 * (c.max - c.min) * fine;
            v = Math.max(c.min, Math.min(c.max, v));
            if (v !== c.v) { c.v = v; c.show(); send(d, c, v); }
            e.preventDefault();
        }
        function up() {
            dragging = false;
            document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
            document.removeEventListener('touchmove', move); document.removeEventListener('touchend', up);
        }
        c.el.addEventListener('mousedown', down);
        c.el.addEventListener('touchstart', down, {passive: false});
        c.el.addEventListener('dblclick', function (e) { c.v = c.def; c.show(); send(d, c, c.v); e.stopPropagation(); });
        c.set = function (v) { c.v = v; c.show(); };
        c.show();
        return c;
    }

    function makeSelect(d, parent, spec) {
        var c = spec;
        c.el = el('div', 'scx-sel', parent, '');
        var sel = el('select', '', c.el);
        spec.options.forEach(function (o) { var op = el('option', '', sel, o[1]); op.value = o[0]; });
        el('div', '', c.el, c.label);
        sel.addEventListener('change', function () { c.v = parseInt(sel.value, 10); send(d, c, c.v); });
        sel.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        c.set = function (v) { c.v = v; sel.value = '' + Math.round(v); };
        c.set(c.def);
        return c;
    }

    function makeVSlider(d, parent, spec) {
        var c = spec;
        c.el = el('div', 'scx-vs', parent);
        var track = el('div', 'scx-track', c.el), fill = el('div', 'scx-fill', track), thumb = el('div', 'scx-thumb', track);
        el('div', 'scx-lbl', c.el, c.label);
        c.valEl = el('div', 'scx-val', c.el, '');
        c.v = c.def;
        c.show = function () {
            var f = Math.max(0, Math.min(1, c.v));
            fill.style.height = (f * 100) + '%'; thumb.style.bottom = (f * 100) + '%';
            c.valEl.textContent = c.fmt(c.v);
        };
        function setFrom(e) {
            var p = e.touches ? e.touches[0] : e, r = track.getBoundingClientRect();
            var f = 1 - (p.clientY - r.top) / r.height; f = Math.max(0, Math.min(1, f));
            c.v = f; c.show(); send(d, c, f);
        }
        var dragging = false;
        function down(e) { dragging = true; setFrom(e); e.preventDefault(); e.stopPropagation();
            document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
            document.addEventListener('touchmove', move, {passive: false}); document.addEventListener('touchend', up); }
        function move(e) { if (dragging) { setFrom(e); e.preventDefault(); } }
        function up() { dragging = false;
            document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
            document.removeEventListener('touchmove', move); document.removeEventListener('touchend', up); }
        c.el.addEventListener('mousedown', down);
        c.el.addEventListener('touchstart', down, {passive: false});
        c.el.addEventListener('dblclick', function (e) { c.v = c.def; c.show(); send(d, c, c.v); e.stopPropagation(); });
        c.set = function (v) { c.v = v; c.show(); };
        c.show();
        return c;
    }

    // ---- file browser ----
    function joinPath(dir, name) { return dir === '/' ? '/' + name : dir + '/' + name; }
    function parentOf(dir) { var i = dir.lastIndexOf('/'); return i <= 0 ? '/' : dir.substring(0, i); }
    function baseName(p) { return p === '/' ? '/' : p.substring(p.lastIndexOf('/') + 1); }
    function browse(d, dir) {
        d.status.textContent = '';
        d.wantDir = dir;
        funcs.patch_set(U('browse'), 's', dir);
    }

    // "<n>/<total>|<folder>|D<dir>|F<file>|..."
    function gotListing(d, text) {
        var parts = String(text).split('|');
        if (parts.length < 2) return;
        var seq = parts[0].split('/'), n = parseInt(seq[0], 10), total = parseInt(seq[1], 10), dir = parts[1];
        if (n === 1 || dir !== d.cwd) { d.cwd = dir; d.entries = []; d.got = 0; d.search.value = ''; }
        d.got++;
        for (var i = 2; i < parts.length; i++) {
            var e = parts[i];
            if (e.length > 1) d.entries.push({dir: e[0] === 'D', name: e.substring(1)});
        }
        d.complete = d.got >= total;
        showList(d);
    }

    function gotFolders(d, text) {
        var f = String(text).split('|').filter(function (x) { return x.length; });
        d.defaultDir = f[0] || d.defaultDir;
        d.saved = f.slice(1);
        showFolders(d);
    }

    function showFolders(d) {
        var sel = d.folderSel;
        sel.innerHTML = '';
        function opt(value, label) { var o = el('option', '', sel, label); o.value = value; o.title = value; return o; }
        opt('', d.cwd ? baseName(d.cwd) + '  (' + d.cwd + ')' : '…').disabled = true;
        if (d.defaultDir) {
            opt(d.defaultDir, '\u2302 Instruments');
            var home = d.defaultDir.replace(/\/\.local\/share\/ShortCircuit-XT$/, '');
            if (home !== d.defaultDir) opt(home, '\u2302 Home');
        }
        opt('/media', '\u2302 USB / media');
        (d.saved || []).forEach(function (f) { opt(f, '\u2605 ' + baseName(f) + '  (' + f + ')'); });
        sel.value = '';
        var isSaved = (d.saved || []).indexOf(d.cwd) >= 0;
        d.starBtn.textContent = isSaved ? '\u2605' : '\u2606';
        d.starBtn.title = isSaved ? 'Forget this folder' : 'Remember this folder';
    }

    function showList(d) {
        var list = d.list, q = d.search.value.trim().toLowerCase();
        list.innerHTML = '';
        d.pathEl.value = d.cwd || '';
        showFolders(d);
        if (d.cwd === undefined) { el('div', 'scx-empty', list, 'Waiting for the plugin…'); return; }
        if (!q && d.cwd !== '/') {
            el('div', 'scx-item dir', list, '..').addEventListener('click', function () { browse(d, parentOf(d.cwd)); });
        }
        var shown = 0;
        d.entries.forEach(function (e) {
            if (q && e.name.toLowerCase().indexOf(q) < 0) return;
            shown++;
            var it = el('div', 'scx-item ' + (e.dir ? 'dir' : 'file'), list, e.name);
            var full = joinPath(d.cwd, e.name);
            it.title = full;
            it.addEventListener('click', function () { if (e.dir) browse(d, full); else askLoad(d, full); });
        });
        if (!shown) el('div', 'scx-empty', list, q ? 'Nothing matches "' + q + '" in this folder.' :
            (d.complete ? 'No instruments or samples here (sfz, sf2, multisample, wav, flac, aiff, ogg, mp3).' : 'Loading…'));
    }

    function askLoad(d, path) {
        d.dlgName.textContent = path;
        d.pendingLoad = path;
        d.modal.hidden = false;
    }
    function doLoad(d, mode) {
        d.modal.hidden = true;
        if (d.pendingLoad === undefined) return;
        funcs.patch_set(U('load_mode'), 'f', mode);
        funcs.patch_set(U('load_path'), 's', d.pendingLoad);
        d.status.textContent = (mode ? 'Replacing part 1 with ' : 'Adding to part 1: ') + baseName(d.pendingLoad);
        d.pendingLoad = undefined;
    }

    // ---- build ----
    function build(d) {
        var root = d.icon.find('.scx-root')[0];
        root.innerHTML = '';
        d.ctl = {};

        var top = el('div', 'scx-top', root);
        el('div', 'scx-title', top, 'SHORTCIRCUIT XT');
        d.status = el('div', 'scx-sub scx-status', top, '');
        var grp = el('div', 'scx-group scx-ctl', top);
        var prev = el('button', '', grp, '◀'), gname = el('span', '', grp, 'Group 1'), next = el('button', '', grp, '▶');
        d.groupLabel = gname;
        function selGroup(delta) {
            var n = Math.max(1, d.groupCount || 1), g = Math.max(0, Math.min(n - 1, (d.group || 0) + delta));
            d.group = g; showGroup(d); funcs.patch_set(U('group'), 'f', g);
        }
        prev.addEventListener('click', function (e) { e.stopPropagation(); selGroup(-1); });
        next.addEventListener('click', function (e) { e.stopPropagation(); selGroup(1); });
        [prev, next].forEach(function (b) { b.addEventListener('mousedown', function (e) { e.stopPropagation(); }); });

        // group settings
        var gs = el('div', 'scx-card', root);
        el('h4', '', gs, 'Group settings');
        var row = el('div', 'scx-gs', gs);
        GROUP.forEach(function (g) {
            var spec = {uri: U(g[0]), label: g[1], min: g[3], max: g[4], def: g[5]};
            if (g[2] === 'k') { spec.fmt = g[6]; d.ctl[spec.uri] = makeKnob(d, row, spec); }
            else { spec.options = g[6]; d.ctl[spec.uri] = makeSelect(d, row, spec); }
        });

        // instrument browser + EG1
        var mid = el('div', 'scx-mid', root);
        var br = el('div', 'scx-card scx-browser', mid);
        el('h4', '', br, 'Instruments & samples');
        var bar = el('div', 'scx-bar', br);
        d.folderSel = el('select', 'scx-folders', bar);
        d.folderSel.title = 'Folders';
        d.folderSel.addEventListener('change', function () { if (d.folderSel.value) browse(d, d.folderSel.value); });
        d.starBtn = el('button', 'scx-star', bar, '\u2606');
        d.starBtn.addEventListener('click', function (e) {
            e.stopPropagation();
            if (!d.cwd) return;
            var isSaved = (d.saved || []).indexOf(d.cwd) >= 0;
            funcs.patch_set(U(isSaved ? 'folder_remove' : 'folder_add'), 's', d.cwd);
        });
        d.pathEl = el('input', 'scx-pathin', br);
        d.pathEl.placeholder = 'Folder, e.g. /media/usb';
        d.pathEl.title = 'Type a folder and press Enter';
        d.pathEl.addEventListener('keydown', function (e) {
            e.stopPropagation();
            if (e.key === 'Enter' && d.pathEl.value.trim()) browse(d, d.pathEl.value.trim().replace(/\/+$/, '') || '/');
        });
        d.search = el('input', 'scx-search', br); d.search.placeholder = 'Search in this folder';
        d.search.addEventListener('input', function () { showList(d); });
        d.search.addEventListener('keydown', function (e) { e.stopPropagation(); });
        [bar, d.pathEl, d.search].forEach(function (x) { x.addEventListener('mousedown', function (e) { e.stopPropagation(); }); });
        d.list = el('div', 'scx-list', br);
        d.list.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        d.entries = [];
        showList(d);

        var eg = el('div', 'scx-card scx-eg', mid);
        el('h4', '', eg, 'Group EG1 (amp)');
        var sl = el('div', 'scx-sliders', eg);
        EG.forEach(function (e) {
            var spec = {uri: U(e[0]), label: e[1], min: 0, max: 1, def: e[2], fmt: e[3]};
            d.ctl[spec.uri] = makeVSlider(d, sl, spec);
        });

        // macros (control ports p0..p15)
        var mc = el('div', 'scx-card scx-macros', root);
        el('h4', '', mc, 'Macros');
        var grid = el('div', 'scx-grid', mc);
        for (var m = 0; m < 16; m++) {
            var sym = 'p' + m;
            if (d.ports && d.ports[sym] === undefined) continue;
            d.ctl[sym] = makeKnob(d, grid, {symbol: sym, label: 'Macro ' + (m + 1), min: 0, max: 1, def: 0,
                fmt: function (v) { return Math.round(v * 100) + '%'; }});
        }

        // load dialog
        d.modal = el('div', 'scx-modal', root); d.modal.hidden = true;
        var dlg = el('div', 'scx-dialog', d.modal);
        el('div', 'scx-dlg-title', dlg, 'Load into part 1');
        d.dlgName = el('div', 'scx-dlg-name', dlg, '');
        var btns = el('div', 'scx-dlg-btns', dlg);
        el('button', '', btns, 'Add to part 1').addEventListener('click', function (e) { e.stopPropagation(); doLoad(d, 0); });
        el('button', '', btns, 'Replace part 1').addEventListener('click', function (e) { e.stopPropagation(); doLoad(d, 1); });
        el('button', '', btns, 'Cancel').addEventListener('click', function (e) { e.stopPropagation(); d.modal.hidden = true; });
        d.modal.addEventListener('mousedown', function (e) { e.stopPropagation(); });
    }

    function showGroup(d) {
        if (!d.groupLabel) return;
        var n = d.groupCount || 0;
        d.groupLabel.textContent = n ? ('Group ' + ((d.group || 0) + 1) + ' / ' + n) : 'No groups';
    }

    function setValue(d, key, v) {
        // key: parameter uri or port symbol
        if (key === U('group')) { d.group = Math.round(v); showGroup(d); return; }
        if (key === U('group_count')) { d.groupCount = Math.round(v); showGroup(d); return; }
        if (key === U('listing')) { gotListing(d, v); return; }
        if (key === U('folders')) { gotFolders(d, v); return; }
        if (key === U('loaded')) { if (d.loaded !== undefined && v !== d.loaded) d.status.textContent = ''; d.loaded = v; return; }
        var c = d.ctl && d.ctl[key];
        if (!c) return;
        if (c.local && Date.now() - c.local < 800) return; // our own edit coming back
        c.set(v);
    }

    var d = event.data;
    if (event.type === 'start') {
        d.icon = event.icon;
        d.ports = {};
        (event.ports || []).forEach(function (p) { d.ports[p.symbol] = p.value; });
        if (event.values) { for (var s in event.values) d.ports[s] = event.values[s]; }
        if (!Object.keys(d.ports).length) d.ports = null; // unknown: show all 16 macros
        build(d);
        if (d.ports) for (var sym in d.ports) setValue(d, sym, d.ports[sym]);
        (event.parameters || []).forEach(function (p) { setValue(d, p.uri, p.value); });
        // ask the plugin for the current values of the selected group, the saved folders
        // and the listing of the last folder
        funcs.patch_get(U('group'));
    } else if (event.type === 'change') {
        if (!d.ctl) return;
        if (event.uri) setValue(d, event.uri, event.value);
        else if (event.symbol) setValue(d, event.symbol, event.value);
    }
}
