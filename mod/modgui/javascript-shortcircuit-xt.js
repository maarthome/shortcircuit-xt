function (event, funcs) {
    // Shortcircuit XT panel for MOD (maarthome/shortcircuit-xt, My_Changes branch).
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
        if (c.uri && d.ctl) {
            [d.ctl[c.uri]].concat(d.ctlMore[c.uri] || []).forEach(function (o) { if (o && o !== c && o.set) o.set(v); });
            (d.hooks[c.uri] || []).forEach(function (fn) { fn(v); });
        }
        if (c.sendFn) c.sendFn(v);
        else if (c.uri) funcs.patch_set(c.uri, 'f', v);
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

    // ---- file browser: one drop-down list ----
    // The plugin lists the folders (browse -> listing chunks "<n>/<total>|<folder>|D<dir>|F<file>...")
    // and remembers the last folder and the saved ones (folders) on the device.
    function joinPath(dir, name) { return dir === '/' ? '/' + name : dir + '/' + name; }
    function parentOf(dir) { var i = dir.lastIndexOf('/'); return i <= 0 ? '/' : dir.substring(0, i); }
    function baseName(p) { return p === '/' ? '/' : p.substring(p.lastIndexOf('/') + 1); }
    function browse(d, dir) { funcs.patch_set(U('browse'), 's', dir); }

    function gotListing(d, text) {
        var parts = String(text).split('|');
        if (parts.length < 2) return;
        var seq = parts[0].split('/'), n = parseInt(seq[0], 10), total = parseInt(seq[1], 10), dir = parts[1];
        if (n === 1 || dir !== d.cwd) { d.cwd = dir; d.entries = []; d.got = 0; }
        d.got++;
        for (var i = 2; i < parts.length; i++) {
            var e = parts[i];
            if (e.length > 1) d.entries.push({dir: e[0] === 'D', name: e.substring(1)});
        }
        d.complete = d.got >= total;
        fillBrowser(d);
    }

    function gotFolders(d, text) {
        var f = String(text).split('|').filter(function (x) { return x.length; });
        d.defaultDir = f[0] || d.defaultDir;
        d.saved = f.slice(1);
        fillBrowser(d);
    }

    // what the list shows besides folders: instruments (sfz, sf2, akp, exs, gig, multisample,
    // Shortcircuit's own scp/scm) or waves (single samples)
    var WAVE_RE = /\.(wav|flac|mp3|opus|aif|aiff)$/i;
    function shown(d, name) { return (d.kind === 'waves') === WAVE_RE.test(name); }
    function setKind(d, kind) {
        d.kind = kind;
        d.kindBtns.forEach(function (b) { b.classList.toggle('on', b.kind === kind); });
        fillBrowser(d);
    }

    // The list stays open while you open folders (also ".."), remember/forget a folder or
    // switch Instruments/Waves; choosing a file, clicking outside or Esc closes it.
    // item values: "B<folder>" open, "L<file>" load, "S" remember, "R" forget
    function fillBrowser(d) {
        if (!d.ddBtn) return;
        d.ddBtn.textContent = d.cwd === undefined ? 'Waiting for the plugin…' :
            '\uD83D\uDCC2 ' + baseName(d.cwd) + (d.complete ? '' : ' (loading…)');
        d.ddBtn.title = d.cwd || '';
        var pop = d.ddPop, keepScroll = d.ddDir === d.cwd ? pop.scrollTop : 0;
        d.ddDir = d.cwd;
        pop.innerHTML = '';
        function item(value, label, cls) {
            var it = el('div', 'scx-dd-item' + (cls ? ' ' + cls : ''), pop, label);
            if (value) it.addEventListener('click', function (e) { e.stopPropagation(); browserChoice(d, value); });
            else it.classList.add('disabled');
            return it;
        }
        function head(label) { el('div', 'scx-dd-head', pop, label); }
        if (d.cwd !== undefined) {
            head(d.cwd);
            if (d.cwd !== '/') item('B' + parentOf(d.cwd), '\u2B11  ..', 'dir');
            var dirs = d.entries.filter(function (e) { return e.dir; }), files = d.entries.filter(function (e) { return !e.dir && shown(d, e.name); });
            dirs.forEach(function (e) { item('B' + joinPath(d.cwd, e.name), '\uD83D\uDCC1  ' + e.name, 'dir'); });
            files.forEach(function (e) { item('L' + joinPath(d.cwd, e.name), '\u266A  ' + e.name, 'file'); });
            if (!files.length && !dirs.length && d.complete) item('', '(no ' + (d.kind === 'waves' ? 'waves' : 'instruments') + ' here)');
        }
        head('Go to');
        if (d.defaultDir) {
            item('B' + d.defaultDir, '\u2302  Instruments', 'dir');
            var home = d.defaultDir.replace(/\/\.local\/share\/ShortCircuit-XT$/, '');
            if (home !== d.defaultDir) item('B' + home, '\u2302  Home', 'dir');
        }
        item('B/media', '\u2302  USB / media', 'dir');
        (d.saved || []).forEach(function (f) { item('B' + f, '\u2605  ' + baseName(f) + '   (' + f + ')', 'dir'); });
        if (d.cwd !== undefined && d.cwd !== d.defaultDir) {
            var isSaved = (d.saved || []).indexOf(d.cwd) >= 0;
            item(isSaved ? 'R' : 'S', isSaved ? '\u2716  Forget this folder' : '\u2606  Remember this folder', 'act');
        }
        pop.scrollTop = keepScroll;
    }

    function openBrowser(d, open) {
        d.ddPop.hidden = !open;
        d.ddBtn.classList.toggle('open', open);
        if (open) fillBrowser(d);
    }

    function browserChoice(d, v) {
        var kind = v[0], path = v.substring(1);
        if (kind === 'B') { d.complete = false; browse(d, path); }
        else if (kind === 'L') { openBrowser(d, false); askLoad(d, path); }
        else if (kind === 'S') funcs.patch_set(U('folder_add'), 's', d.cwd);
        else if (kind === 'R') funcs.patch_set(U('folder_remove'), 's', d.cwd);
    }

    function askLoad(d, path) {
        d.dlgName.textContent = path;
        d.pendingLoad = path;
        // Shortcircuit's own .scp (part) / .scm (multi) always replace: no "Add"
        var own = /\.sc[mp]$/i.test(path);
        d.addBtn.hidden = own;
        d.dlgTitle.textContent = /\.scm$/i.test(path) ? 'Load multi (replaces all parts)' : 'Load into part 1';
        d.modal.hidden = false;
    }
    function doLoad(d, mode) {
        d.modal.hidden = true;
        if (d.pendingLoad === undefined) return;
        funcs.patch_set(U('load_mode'), 'f', mode);
        funcs.patch_set(U('load_path'), 's', d.pendingLoad);
        d.status.textContent = (mode || /\.sc[mp]$/i.test(d.pendingLoad) ? 'Loading ' : 'Adding to part 1: ') + baseName(d.pendingLoad);
        d.pendingLoad = undefined;
    }

    // ---- several controls and hooks may follow one parameter ----
    function bind(d, key, c) {
        if (!d.ctl[key]) d.ctl[key] = c;
        else (d.ctlMore[key] = d.ctlMore[key] || []).push(c);
        return c;
    }
    function hook(d, key, fn) { (d.hooks[key] = d.hooks[key] || []).push(fn); }
    function pset(d, key, v) { funcs.patch_set(U(key), 'f', v); setValue(d, U(key), v, true); }

    // on/off button for a 0/1 parameter
    function makeToggle(d, parent, spec) {
        var c = spec;
        c.el = el('button', 'scx-tgl ' + (spec.cls || ''), parent, spec.label);
        if (spec.title) c.el.title = spec.title;
        c.v = 0;
        c.set = function (v) { c.v = v > 0.5 ? 1 : 0; c.el.classList.toggle('on', !!c.v); };
        c.el.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        c.el.addEventListener('click', function (e) { e.stopPropagation(); c.set(1 - c.v); send(d, c, c.v); });
        return c;
    }

    // a row of buttons, one per value (routing S1..BYP, LFO trigger modes, tabs)
    function makeSegment(d, parent, spec) {
        var c = spec;
        c.el = el('div', 'scx-seg ' + (spec.cls || ''), parent);
        c.btns = spec.options.map(function (o) {
            var b = el('button', '', c.el, o[1]);
            b.value = o[0];
            if (o[2]) b.title = o[2];
            b.addEventListener('mousedown', function (e) { e.stopPropagation(); });
            b.addEventListener('click', function (e) { e.stopPropagation(); c.set(o[0]); send(d, c, o[0]); });
            return b;
        });
        c.set = function (v) {
            c.v = Math.round(v);
            c.btns.forEach(function (b) { b.classList.toggle('on', +b.value === c.v); });
            if (c.onSet) c.onSet(c.v);
        };
        c.set(spec.def || 0);
        return c;
    }

    // horizontal bar 0..1 (velocity sensitivity)
    function makeHBar(d, parent, spec) {
        var c = spec;
        c.el = el('div', 'scx-hbar', parent);
        var fill = el('div', 'scx-hbar-fill', c.el);
        c.v = spec.def || 0;
        c.set = function (v) { c.v = Math.max(0, Math.min(1, v)); fill.style.width = (c.v * 100) + '%'; c.el.title = spec.label + ' ' + Math.round(c.v * 100) + '%'; };
        function setFrom(e) {
            var p = e.touches ? e.touches[0] : e, r = c.el.getBoundingClientRect();
            c.set((p.clientX - r.left) / r.width); send(d, c, c.v);
        }
        function move(e) { setFrom(e); e.preventDefault(); }
        function up() {
            document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
            document.removeEventListener('touchmove', move); document.removeEventListener('touchend', up);
        }
        function down(e) {
            setFrom(e); e.preventDefault(); e.stopPropagation();
            document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
            document.addEventListener('touchmove', move, {passive: false}); document.addEventListener('touchend', up);
        }
        c.el.addEventListener('mousedown', down);
        c.el.addEventListener('touchstart', down, {passive: false});
        c.set(c.v);
        return c;
    }

    // ---- routing: the 4 effect slots as Shortcircuit draws them ----
    // stages left to right; each stage holds parallel lanes; a lane is slots in series
    var ROUTES = [
        ['S1', '1 > 2 > 3 > 4', [[[0, 1, 2, 3]]]],
        ['S2', '(1|2) > (3|4)', [[[0], [1]], [[2], [3]]]],
        ['S3', '1 > (2|3) > 4', [[[0]], [[1], [2]], [[3]]]],
        ['P1', '1 | 2 | 3 | 4', [[[0], [1], [2], [3]]]],
        ['P2', '(1>2) | (3>4)', [[[0, 1], [2, 3]]]],
        ['P3', '(1|2|3) > 4', [[[0], [1], [2]], [[3]]]],
        ['BYP', 'bypass all effects', [[[0, 1, 2, 3]]]]
    ];

    function drawRouting(d) {
        if (!d.routeSvg) return;
        var mode = d.routeMode || 0, r = ROUTES[mode] || ROUTES[0], stages = r[2];
        var W = 330, H = 132, bw = 44, bh = 34, gap = 22, lane = 46;
        var widths = stages.map(function (st) { return Math.max.apply(null, st.map(function (l) { return l.length; })) * (bw + gap) - gap; });
        var inner = widths.reduce(function (a, b) { return a + b; }, 0), sgap = Math.min(40, (W - 20 - inner) / (2 * stages.length));
        var x0all = (W - inner - 2 * sgap * stages.length) / 2;
        var cy = H / 2, x = x0all, h = '', blocks = '';
        var line = mode === 6 ? '#6d7580' : '#8a929c';
        function curve(x0, y0, x1, y1) { var mx = (x0 + x1) / 2; return '<path d="M' + x0 + ' ' + y0 + ' C' + mx + ' ' + y0 + ' ' + mx + ' ' + y1 + ' ' + x1 + ' ' + y1 + '" stroke="' + line + '" stroke-width="2.5" fill="none"/>'; }
        stages.forEach(function (st, si) {
            var x0 = x, xs = x + sgap, n = st.length;
            st.forEach(function (ln, li) {
                var y = cy + (li - (n - 1) / 2) * lane, bx = xs;
                h += curve(x0, cy, bx, y);
                ln.forEach(function (slot, k) {
                    var f = d.fx && d.fx[slot], used = f && f.type, on = f && f.active, mix = f && f.mixKnob ? f.mixKnob.v : 0;
                    var cls = 'scx-rb' + (used ? '' : ' empty') + (used && !on ? ' off' : '') + (mode === 6 ? ' byp' : '');
                    blocks += '<g class="' + cls + '"><rect x="' + bx + '" y="' + (y - bh / 2) + '" width="' + bw + '" height="' + bh + '" rx="5"/>' +
                        '<text x="' + (bx + bw / 2 - 4) + '" y="' + (y + 5) + '">' + (slot + 1) + '</text>' +
                        '<rect class="mixbg" x="' + (bx + bw - 9) + '" y="' + (y - bh / 2 + 3) + '" width="6" height="' + (bh - 6) + '" rx="1"/>' +
                        (used ? '<rect class="mix" x="' + (bx + bw - 9) + '" y="' + (y + bh / 2 - 3 - (bh - 6) * mix) + '" width="6" height="' + ((bh - 6) * mix) + '" rx="1"/>' : '') + '</g>';
                    if (k < ln.length - 1) h += '<line x1="' + (bx + bw) + '" y1="' + y + '" x2="' + (bx + bw + gap) + '" y2="' + y + '" stroke="' + line + '" stroke-width="2.5"/>';
                    bx += bw + gap;
                });
                var xe = xs + widths[si] + sgap;
                h += curve(bx - gap, y, xe, cy);
            });
            x = xs + widths[si] + sgap;
        });
        h += '<line x1="' + x + '" y1="' + cy + '" x2="' + W + '" y2="' + cy + '" stroke="' + line + '" stroke-width="2.5"/>';
        h += '<line x1="0" y1="' + cy + '" x2="' + x0all + '" y2="' + cy + '" stroke="' + line + '" stroke-width="2.5"/>';
        if (mode === 6) h += '<path d="M0 ' + 14 + ' H' + W + '" stroke="#f0a83a" stroke-width="2.5" fill="none"/>';
        d.routeSvg.innerHTML = h + blocks;
    }

    function buildRouting(d, parent) {
        var card = el('div', 'scx-card scx-routing-card', parent);
        var h4 = el('h4', '', card, 'Routing');
        var hq = makeSelect(d, h4, {uri: U('group_oversample'), label: 'HQ', min: 0, max: 2, def: 1, options: [[0, 'OFF'], [1, 'AUTO'], [2, 'ON']]});
        hq.el.classList.add('scx-hq');
        bind(d, hq.uri, hq);
        var top = el('div', 'scx-rt-top', card);
        var seg = makeSegment(d, top, {uri: U('group_proc_routing'), cls: 'scx-rt-seg', def: 0,
            options: ROUTES.map(function (r, i) { return [i, r[0], r[1]]; })});
        seg.onSet = function (v) { d.routeMode = v; drawRouting(d); };
        bind(d, seg.uri, seg);
        el('div', 'scx-rt-vel', top, 'Vel');
        bind(d, U('group_vel_sens'), makeHBar(d, top, {uri: U('group_vel_sens'), label: 'Velocity sensitivity', def: 0.6}));
        var body = el('div', 'scx-rt-body', card);
        d.routeSvg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
        d.routeSvg.setAttribute('viewBox', '0 0 330 132');
        d.routeSvg.setAttribute('class', 'scx-rt-svg');
        body.appendChild(d.routeSvg);
        var right = el('div', 'scx-rt-right', body);
        var knobs = el('div', 'scx-rt-knobs', right);
        bind(d, U('group_volume'), makeKnob(d, knobs, {uri: U('group_volume'), label: 'Volume', min: 0, max: 1.5849, def: 1, fmt: fmtDb}));
        bind(d, U('group_pan'), makeKnob(d, knobs, {uri: U('group_pan'), label: 'Pan', min: -1, max: 1, def: 0, fmt: fmtPan}));
        var out = makeSelect(d, right, {uri: U('group_route_to'), label: 'Output', min: -1, max: 20, def: -1,
            options: [[-1, 'Part Output'], [0, 'Main']].concat(range(1, 16, function (i) { return 'Bus ' + i; }), range(17, 20, function (i) { return 'Aux ' + (i - 16); }))});
        out.el.classList.add('scx-rt-out');
        bind(d, out.uri, out);
        drawRouting(d);
    }

    // ---- group EGs 1..5 ----
    var EGKEYS = ['eg1_delay', 'eg1_attack', 'eg1_hold', 'eg1_decay', 'eg1_sustain', 'eg1_release'];
    function buildEG(d, parent) {
        var card = el('div', 'scx-card scx-eg', parent);
        var h4 = el('h4', '', card, '');
        d.egTitle = el('span', '', h4, 'Amp EG (zones)');
        // AMP = the zones' amp envelope (what you hear); EG 1..5 = the group's envelopes, which
        // only act through a mod matrix row
        var tabs = makeSegment(d, card, {uri: U('eg_index'), cls: 'scx-tabs', def: -1,
            options: [[-1, 'AMP', 'The amp envelope of every zone in the edited group(s)']].concat(
                range(0, 4, function (i) { return 'EG ' + (i + 1); }).map(function (o) { return [o[0], o[1], 'Group EG: a modulation source, use it in the mod matrix']; }))});
        tabs.onSet = function (v) {
            d.egTitle.textContent = v < 0 ? 'Amp EG (zones)' : 'Group EG ' + (v + 1);
            d.egHint.textContent = v < 0 ? '' : 'modulation source: route it in the mod matrix';
            if (d.egGateAny) d.egGateAny.el.hidden = v < 0;
        };
        bind(d, tabs.uri, tabs);
        d.egHint = el('span', 'scx-eg-hint', h4, '');
        var opts = el('div', 'scx-eg-opts', card);
        var gate = makeSelect(d, opts, {uri: U('eg_gate_mode'), label: 'Gate', min: 0, max: 2, def: 0, options: [[0, 'Gated (DAHDSR)'], [1, 'No sustain'], [2, 'Oneshot']]});
        bind(d, gate.uri, gate);
        hook(d, gate.uri, function (v) { if (d.egS) d.egS.lblEl.textContent = v >= 1 ? 'B' : 'S'; });
        d.egGateAny = bind(d, U('eg_gate_any'), makeToggle(d, opts, {uri: U('eg_gate_any'), label: 'Gate on voice', title: 'Gate on voice sounding'}));
        d.egGateAny.el.hidden = true; // group EGs only
        bind(d, U('eg_temposync'), makeToggle(d, opts, {uri: U('eg_temposync'), label: '\u2669 Sync', title: 'Tempo sync the times'}));
        var sl = el('div', 'scx-sliders', card);
        var shapes = {1: 'eg_attack_shape', 3: 'eg_decay_shape', 5: 'eg_release_shape'};
        d.egSliders = [];
        EG.forEach(function (e, i) {
            var col = el('div', 'scx-egcol', sl);
            if (shapes[i]) {
                var k = makeKnob(d, col, {uri: U(shapes[i]), label: '', min: -1, max: 1, def: 0, fmt: function () { return ''; }});
                k.el.classList.add('scx-shape'); k.el.title = 'Shape';
                bind(d, k.uri, k);
            } else el('div', 'scx-shape-gap', col);
            var spec = {uri: U(e[0]), label: e[1], min: 0, max: 1, def: e[2], fmt: e[3]};
            var c = makeVSlider(d, col, spec);
            c.lblEl = c.el.querySelector('.scx-lbl');
            var fmt0 = spec.fmt;
            c.fmt = function (v) { return (c.text && !(c.local && Date.now() - c.local < 400)) ? c.text : fmt0(v); };
            bind(d, spec.uri, c);
            d.egSliders.push(c);
            if (i === 4) d.egS = c;
        });
    }

    // ---- group LFOs 1..4 ----
    var LFO_SHAPES = [[1, 'SINE'], [2, 'RAMP'], [3, 'TRI'], [4, 'SAW-RMP'], [5, 'SQR-TRI'], [6, 'NOISE'], [7, 'S&H'], [8, 'ENV'], [0, 'STEP']];
    function lfoWave(shape, deform, phase, uni) {
        // a rough picture of one shape (Shortcircuit's exact curve also bends with deform/angle)
        var pts = [], n = 120, seed = 1;
        function rnd() { seed = (seed * 16807) % 2147483647; return seed / 2147483647 * 2 - 1; }
        var noise = [];
        for (var k = 0; k < 9; k++) noise.push(rnd());
        for (var i = 0; i <= n; i++) {
            var t = i / n * 2 + phase, f = t - Math.floor(t), y;
            switch (shape) {
            case 2: y = 1 - 2 * f; break;
            case 3: y = f < 0.5 ? 4 * f - 1 : 3 - 4 * f; break;
            case 4: y = f < 0.5 ? 1 - 4 * f : 4 * f - 3; y = 0.5 * y + 0.5 * (1 - 2 * f); break;
            case 5: y = f < 0.5 ? 1 : -1; break;
            case 6: var p = i / n * 8, a = Math.floor(p), b = p - a; y = noise[a] + (noise[a + 1] - noise[a]) * (3 * b * b - 2 * b * b * b); break;
            case 7: y = noise[Math.floor(i / n * 8)]; break;
            case 8: y = i < n * 0.15 ? i / (n * 0.15) : i < n * 0.3 ? 1 - (i - n * 0.15) / (n * 0.15) * 0.4 : i < n * 0.75 ? 0.6 : 0.6 * (1 - (i - n * 0.75) / (n * 0.25)); y = y * 2 - 1; break;
            case 0: y = [0.6, -0.2, 0.9, -0.7, 0.3, -0.5, 0.8, -0.1][Math.floor(i / n * 8) % 8]; break;
            default: y = Math.sin(2 * Math.PI * f); if (deform) y = Math.sign(y) * Math.pow(Math.abs(y), Math.pow(2, -deform * 1.5));
            }
            if (uni && shape !== 8) y = (y + 1) / 2;
            if (uni || shape === 8) y = y * 2 - 1;
            pts.push(y);
        }
        return pts;
    }
    function drawLfo(d) {
        if (!d.lfoSvg) return;
        var v = d.lfoVals || {}, pts = lfoWave(v.shape === undefined ? 1 : v.shape, v.deform || 0, v.phase || 0, v.uni);
        var W = 300, H = 90, path = '';
        pts.forEach(function (y, i) { path += (i ? 'L' : 'M') + (i / (pts.length - 1) * W).toFixed(1) + ' ' + (H / 2 - y * (H / 2 - 4)).toFixed(1); });
        d.lfoSvg.innerHTML = '<line x1="0" y1="' + H / 2 + '" x2="' + W + '" y2="' + H / 2 + '" stroke="#5a626c" stroke-width="1"/>' +
            '<path d="' + path + ' L' + W + ' ' + H / 2 + ' L0 ' + H / 2 + ' Z" fill="rgba(61,143,224,.35)"/>' +
            '<path d="' + path + '" stroke="#eef1f4" stroke-width="2" fill="none"/>';
    }
    function showLfoKind(d) {
        var sh = d.lfoVals ? d.lfoVals.shape : 1, env = sh === 8, step = sh === 0;
        d.lfoCurve.hidden = env;
        d.lfoEnv.hidden = !env;
        d.lfoUni.el.hidden = env;
        d.lfoUseEnv.el.hidden = env || step;
        d.lfoDar.hidden = env || step || !(d.lfoVals && d.lfoVals.useenv);
        d.lfoStepNote.hidden = !step;
        drawLfo(d);
    }
    function buildLFO(d, parent) {
        var card = el('div', 'scx-card scx-lfo', parent);
        var h4 = el('h4', '', card, '');
        d.lfoTitle = el('span', '', h4, 'Group LFO 1');
        bind(d, U('lfo_temposync'), makeToggle(d, h4, {uri: U('lfo_temposync'), label: '\u2669 Sync', title: 'Tempo sync the rate', cls: 'scx-h4btn'}));
        var tabs = makeSegment(d, card, {uri: U('lfo_index'), cls: 'scx-tabs', def: 0, options: range(0, 3, function (i) { return 'GLFO ' + (i + 1); })});
        tabs.onSet = function (v) { d.lfoTitle.textContent = 'Group LFO ' + (v + 1); };
        bind(d, tabs.uri, tabs);
        d.lfoVals = {shape: 1};
        var body = el('div', 'scx-lfo-body', card);
        var left = el('div', 'scx-lfo-left', body);
        var shape = makeSelect(d, left, {uri: U('lfo_shape'), label: '', min: 0, max: 8, def: 1, options: LFO_SHAPES});
        shape.el.classList.add('scx-lfo-shape');
        bind(d, shape.uri, shape);
        hook(d, shape.uri, function (v) { d.lfoVals.shape = Math.round(v); showLfoKind(d); });
        var btns = el('div', 'scx-lfo-btns', left);
        d.lfoUni = bind(d, U('lfo_unipolar'), makeToggle(d, btns, {uri: U('lfo_unipolar'), label: 'UNI', title: 'Unipolar'}));
        hook(d, U('lfo_unipolar'), function (v) { d.lfoVals.uni = v > 0.5; drawLfo(d); });
        d.lfoUseEnv = bind(d, U('lfo_useenv'), makeToggle(d, btns, {uri: U('lfo_useenv'), label: 'ENV', title: 'Delay / attack / release envelope'}));
        hook(d, U('lfo_useenv'), function (v) { d.lfoVals.useenv = v > 0.5; showLfoKind(d); });
        d.lfoSvg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
        d.lfoSvg.setAttribute('viewBox', '0 0 300 90');
        d.lfoSvg.setAttribute('preserveAspectRatio', 'none');
        d.lfoSvg.setAttribute('class', 'scx-lfo-svg');
        left.appendChild(d.lfoSvg);

        var mid = el('div', 'scx-lfo-mid', body);
        d.lfoCurve = el('div', 'scx-lfo-knobs', mid);
        d.lfoTextCtl = [];
        [['lfo_rate', 'Rate', -7, 9, 0], ['lfo_phase', 'Phase', 0, 1, 0], ['lfo_deform', 'Deform', -1, 1, 0], ['lfo_angle', 'Angle', -1, 1, 0]].forEach(function (k, i) {
            var c = {uri: U(k[0]), label: k[1], min: k[2], max: k[3], def: k[4]};
            c.fmt = fmtText(c);
            bind(d, c.uri, makeKnob(d, d.lfoCurve, c));
            d.lfoTextCtl[i] = c;
            if (k[0] === 'lfo_deform') hook(d, c.uri, function (v) { d.lfoVals.deform = v; drawLfo(d); });
            if (k[0] === 'lfo_phase') hook(d, c.uri, function (v) { d.lfoVals.phase = v; drawLfo(d); });
        });
        d.lfoStepNote = el('div', 'scx-lfo-note', mid, 'Step sequence: edit the steps in the Shortcircuit editor.');
        d.lfoEnv = el('div', 'scx-sliders scx-lfo-env', mid);
        [['lfoenv_delay', 'Dly', 0], ['lfoenv_attack', 'A', 0], ['lfoenv_hold', 'H', 0], ['lfoenv_decay', 'D', 0], ['lfoenv_sustain', 'S', 1], ['lfoenv_release', 'R', 1]].forEach(function (k, i) {
            var c = {uri: U(k[0]), label: k[1], min: 0, max: 1, def: k[2], fmt: i === 4 ? fmtPct : fmtTime};
            var f0 = c.fmt;
            var s2 = makeVSlider(d, d.lfoEnv, c);
            s2.fmt = function (v) { return (s2.text && !(s2.local && Date.now() - s2.local < 400)) ? s2.text : f0(v); };
            bind(d, c.uri, s2);
            d.lfoTextCtl[7 + i] = s2;
        });
        bind(d, U('lfoenv_loop'), makeToggle(d, d.lfoEnv, {uri: U('lfoenv_loop'), label: 'LOOP', cls: 'scx-loop'}));

        var right = el('div', 'scx-lfo-right', body);
        var trig = makeSegment(d, right, {uri: U('lfo_trigger'), cls: 'scx-trig', def: 0,
            options: [[0, 'KEYTRIG'], [1, 'SONGPOS'], [2, 'RANDOM'], [3, 'RELEASE'], [4, 'ONESHOT']]});
        bind(d, trig.uri, trig);
        el('div', 'scx-trig-lbl', right, 'Trigger');
        d.lfoDar = el('div', 'scx-sliders scx-lfo-dar', body);
        [['lfo_delay', 'D', 0], ['lfo_attack', 'A', 0], ['lfo_release', 'R', 1]].forEach(function (k, i) {
            var c = {uri: U(k[0]), label: k[1], min: 0, max: 1, def: k[2], fmt: fmtTime};
            var s3 = makeVSlider(d, d.lfoDar, c);
            s3.fmt = function (v) { return (s3.text && !(s3.local && Date.now() - s3.local < 400)) ? s3.text : fmtTime(v); };
            bind(d, c.uri, s3);
            d.lfoTextCtl[4 + i] = s3;
        });
        showLfoKind(d);
    }

    // "E|<6 EG texts>|L|<rate phase deform angle d a r>|V|<6 env-LFO texts>"
    function gotModsText(d, text) {
        var p = String(text).split('|'), m = {};
        for (var i = 0; i + 1 < p.length; i += 2) m[p[i]] = p[i + 1].split('~');
        (m.E || []).forEach(function (t, i) { var c = d.egSliders && d.egSliders[i]; if (c) { c.text = t; c.show(); } });
        (m.L || []).forEach(function (t, i) { var c = d.lfoTextCtl && d.lfoTextCtl[i]; if (c) { c.text = t; if (c.show) c.show(); } });
        (m.V || []).forEach(function (t, i) { var c = d.lfoTextCtl && d.lfoTextCtl[7 + i]; if (c) { c.text = t; if (c.show) c.show(); } });
    }

    // ---- what is loaded: part 1's groups "<name>^<zones>^<zone>^<zone>...~..." ----
    function gotNames(d, text) {
        d.groupNames = String(text).split('~').filter(function (x) { return x.length; }).map(function (g) {
            var f = g.split('^'); return {name: f[0], zones: +f[1] || 0, zoneNames: f.slice(2)};
        });
        showGroup(d);
        var box = d.loadedBox;
        if (!box) return;
        box.innerHTML = '';
        if (!d.groupNames.length) { el('div', 'scx-empty', box, 'Nothing loaded in part 1 yet.'); return; }
        d.groupNames.forEach(function (g, i) {
            var row = el('div', 'scx-ld-row' + (d.group < 0 || i === d.group ? ' sel' : ''), box);
            el('span', 'scx-ld-num', row, '' + (i + 1));
            el('span', 'scx-ld-name', row, g.name);
            var z = g.zoneNames.slice(0, 3).join(', ') + (g.zones > 3 ? ' … (' + g.zones + ' zones)' : '');
            el('span', 'scx-ld-zones', row, z);
            row.title = g.zoneNames.join('\n');
            row.addEventListener('mousedown', function (e) { e.stopPropagation(); });
            row.addEventListener('click', function (e) { e.stopPropagation(); d.group = i; showGroup(d); funcs.patch_set(U('group'), 'f', i); });
        });
    }

    // ---- effects: the selected group's 4 processor slots ----
    // The plugin sends text in pieces "<tag> <n>/<total>|<piece>" (fx_types, fx_desc,
    // fx_values); edits go back as fx_edit "<slot>,<t|a|m|l|f|i>,<index>,<value>".
    function joinChunk(d, key, text) {
        var t = String(text), bar = t.indexOf('|'), head = t.substring(0, bar).split(' ');
        var tag = head[0], nt = (head[1] || '1/1').split('/'), n = +nt[0], total = +nt[1];
        var k = key + ':' + tag;
        d.chunks = d.chunks || {};
        if (n === 1) d.chunks[k] = '';
        d.chunks[k] = (d.chunks[k] || '') + t.substring(bar + 1);
        if (n < total) return null;
        var whole = d.chunks[k]; delete d.chunks[k];
        return whole;
    }

    // at most one message per control every 40 ms while dragging; the last value always goes
    function sendText(d, uriKey, key, text) {
        var q = d.textQueue = d.textQueue || {};
        key = uriKey + ':' + key;
        var first = !q[key];
        q[key] = text;
        if (!first) return;
        funcs.patch_set(U(uriKey), 's', text);
        var last = text;
        setTimeout(function flush() {
            if (q[key] !== last) { last = q[key]; funcs.patch_set(U(uriKey), 's', last); setTimeout(flush, 40); }
            else delete q[key];
        }, 40);
    }
    function fxEdit(d, slot, kind, index, v) {
        sendText(d, 'fx_edit', slot + kind + index, slot + ',' + kind + ',' + index + ',' + v);
    }

    function gotFxTypes(d, text) {
        d.fxTypes = String(text).split('~').map(function (e) { var p = e.split('^'); return {id: +p[0], group: p[1], name: p[2]}; });
        for (var s = 0; s < 4; s++) fillTypeSelect(d, s);
    }

    function fillTypeSelect(d, s) {
        var slot = d.fx && d.fx[s];
        if (!slot || !d.fxTypes) return;
        var sel = slot.typeSel, cur = slot.type || 0, groups = {};
        sel.innerHTML = '';
        d.fxTypes.forEach(function (t) {
            var parent = sel;
            if (t.id !== 0) {
                if (!groups[t.group]) { groups[t.group] = el('optgroup', '', sel); groups[t.group].label = t.group; }
                parent = groups[t.group];
            }
            var o = el('option', '', parent, t.id === 0 ? '— none —' : t.name); o.value = t.id;
        });
        sel.value = '' + cur;
    }

    function fmtText(c) { return function (v) { return (c.text && !(c.local && Date.now() - c.local < 400)) ? c.text : (Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(2)); }; }

    // "<slot>|<type>|<name>|<floats>|<ints>"
    function gotFxDesc(d, text) {
        var f = String(text).split('|'), s = +f[0], slot = d.fx && d.fx[s];
        if (!slot) return;
        slot.type = +f[1];
        slot.typeSel.value = '' + slot.type;
        slot.body.innerHTML = '';
        slot.floats = []; slot.ints = [];
        slot.card.classList.toggle('empty', slot.type === 0);
        if (slot.type === 0) { el('div', 'scx-fx-none', slot.body, 'Empty slot'); slot.mixKnob = slot.levelKnob = null; return; }
        var knobs = el('div', 'scx-fx-knobs', slot.body);
        (f[3] ? f[3].split('~') : []).forEach(function (e, i) {
            var p = e.split('^');
            if (!p[0] || p[4] === 'N') { slot.floats.push(null); return; }
            var c = {label: p[0], min: +p[1], max: +p[2], def: +p[3]};
            if (c.max <= c.min) c.max = c.min + 1;
            c.fmt = fmtText(c);
            c.sendFn = function (v) {
                if (p[4] !== 'F') { v = Math.round(v); }
                fxEdit(d, s, 'f', i, v);
            };
            slot.floats.push(makeKnob(d, knobs, c));
        });
        ['Mix', 'Level'].forEach(function (name) {
            var c = {label: name, min: 0, max: 1, def: name === 'Mix' ? 1 : 0.501187};
            c.fmt = fmtText(c);
            c.sendFn = function (v) { fxEdit(d, s, name === 'Mix' ? 'm' : 'l', 0, v); };
            var k = makeKnob(d, knobs, c);
            k.el.classList.add('scx-fx-out');
            if (name === 'Mix') slot.mixKnob = k; else slot.levelKnob = k;
        });
        var sels = el('div', 'scx-fx-sels', slot.body);
        (f[4] ? f[4].split('~') : []).forEach(function (e, i) {
            var p = e.split('^');
            if (!p[0]) { slot.ints.push(null); return; }
            var row = el('label', 'scx-fx-int', sels), lo = Math.round(+p[1]);
            el('span', '', row, p[0]);
            var sel = el('select', '', row);
            p.slice(4).forEach(function (name, k) { var o = el('option', '', sel, name); o.value = lo + k; });
            sel.addEventListener('mousedown', function (ev) { ev.stopPropagation(); });
            sel.addEventListener('change', function () { sel.local = Date.now(); fxEdit(d, s, 'i', i, parseInt(sel.value, 10)); });
            slot.ints.push(sel);
        });
        if (slot.lastValues) gotFxValues(d, slot.lastValues);
    }

    // "<slot>|<type>|<active>|<mix>|<level>|<floats>|<ints>|<float texts>|<mix text>~<level text>"
    function gotFxValues(d, text) {
        var f = String(text).split('|'), s = +f[0], slot = d.fx && d.fx[s];
        if (!slot) return;
        slot.lastValues = text;
        if (+f[1] !== slot.type) return; // its description comes first
        slot.active = f[2] === '1';
        slot.onBtn.classList.toggle('on', slot.active);
        slot.card.classList.toggle('off', !slot.active);
        var fv = f[5] ? f[5].split('~') : [], iv = f[6] ? f[6].split('~') : [], ft = f[7] ? f[7].split('~') : [], ot = (f[8] || '').split('~');
        function put(c, v, t) {
            if (!c) return;
            c.text = t;
            if (c.local && Date.now() - c.local < 800) { c.show(); return; } // our own edit coming back
            c.set(+v);
        }
        slot.floats.forEach(function (c, i) { put(c, fv[i], ft[i]); });
        put(slot.mixKnob, f[3], ot[0]);
        put(slot.levelKnob, f[4], ot[1]);
        slot.ints.forEach(function (sel, i) {
            if (sel && !(sel.local && Date.now() - sel.local < 800) && iv[i] !== undefined) sel.value = '' + iv[i];
        });
        drawRouting(d);
    }

    var ROUTING = [
        ['group_proc_routing', 'Effect routing', 0, 6, 0, [[0, 'S1: 1 > 2 > 3 > 4'], [1, 'S2: (1|2) > (3|4)'], [2, 'S3: 1 > (2|3) > 4'],
            [3, 'P1: 1 | 2 | 3 | 4'], [4, 'P2: (1>2) | (3>4)'], [5, 'P3: (1|2|3) > 4'], [6, 'Bypass all']]],
        ['group_route_to', 'Output', -1, 20, -1, [[-1, 'Default (part)'], [0, 'Main']].concat(
            range(1, 16, function (i) { return 'Bus ' + i; }), range(17, 20, function (i) { return 'Aux ' + (i - 16); }))],
        ['group_oversample', 'Oversample', 0, 2, 1, [[0, 'Off'], [1, 'Auto'], [2, 'On']]]
    ];

    // ---- group mod matrix: 18 rows (source, via, curve, target, depth) ----
    // mod_meta "S|<id>^<path>^<name>~...|T|...|C|..."; mod_rows "<on>^<src>^<via>^<curve>^<tgt>^<depth>~..."
    // edits: mod_edit "<row>,<a|s|v|c|t|d>,<value>"
    function optionsHtml(list, none, withPath) {
        function esc(t) { return String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;'); }
        var h = '<option value="">' + none + '</option>', path = null;
        list.forEach(function (e) {
            if (e.path !== path) { if (path !== null) h += '</optgroup>'; path = e.path; h += '<optgroup label="' + esc(path || '-') + '">'; }
            h += '<option value="' + e.id + '">' + esc(withPath && e.path ? e.path + ' \u203A ' + e.name : e.name) + '</option>';
        });
        if (path !== null) h += '</optgroup>';
        return h;
    }

    function gotModMeta(d, text) {
        var p = String(text).split('|'), m = {};
        for (var i = 0; i + 1 < p.length; i += 2)
            m[p[i]] = p[i + 1] ? p[i + 1].split('~').map(function (e) { var q = e.split('^'); return {id: q[0], path: q[1], name: q[2]}; }) : [];
        d.modMeta = m;
        var src = optionsHtml(m.S || [], '—'), tgt = optionsHtml(m.T || [], '—', true), crv = optionsHtml(m.C || [], 'x');
        (d.modRows || []).forEach(function (r) {
            r.src.innerHTML = src; r.via.innerHTML = src; r.tgt.innerHTML = tgt; r.crv.innerHTML = crv;
        });
        if (d.modLast) gotModRows(d, d.modLast);
    }

    function setSel(sel, v) {
        sel.value = v;
        if (sel.value !== v && v) { var o = el('option', '', sel, '(' + v + ')'); o.value = v; sel.value = v; }
    }

    function gotModRows(d, text) {
        d.modLast = text;
        String(text).split('~').forEach(function (row, i) {
            var r = d.modRows && d.modRows[i];
            if (!r) return;
            var f = row.split('^');
            if (r.local && Date.now() - r.local < 800) return; // our own edit coming back
            r.on.classList.toggle('on', f[0] === '1');
            r.el.classList.toggle('off', f[0] !== '1');
            setSel(r.src, f[1] || ''); setSel(r.via, f[2] || ''); setSel(r.crv, f[3] || ''); setSel(r.tgt, f[4] || '');
            r.depth.set(+f[5] || 0);
            r.el.classList.toggle('used', !!(f[1] && f[4]));
        });
    }

    function makeDepth(parent, onChange) {
        var c = {v: 0};
        c.el = el('div', 'scx-depth', parent);
        var bar = el('div', 'scx-depth-bar', c.el), txt = el('div', 'scx-depth-txt', c.el, '0 %');
        c.set = function (v) {
            c.v = Math.max(-1, Math.min(1, v));
            var l = c.v < 0 ? 50 + c.v * 50 : 50, w = Math.abs(c.v) * 50;
            bar.style.left = l + '%'; bar.style.width = w + '%';
            txt.textContent = (c.v * 100).toFixed(1) + ' %';
        };
        var startX = 0, startV = 0;
        function move(e) {
            var p = e.touches ? e.touches[0] : e;
            var v = startV + (p.clientX - startX) / c.el.getBoundingClientRect().width * 2 * (e.shiftKey ? 0.2 : 1);
            c.set(v); onChange(c.v); e.preventDefault();
        }
        function up() {
            document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
            document.removeEventListener('touchmove', move); document.removeEventListener('touchend', up);
        }
        function down(e) {
            var p = e.touches ? e.touches[0] : e;
            startX = p.clientX; startV = c.v; e.preventDefault(); e.stopPropagation();
            document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
            document.addEventListener('touchmove', move, {passive: false}); document.addEventListener('touchend', up);
        }
        c.el.addEventListener('mousedown', down);
        c.el.addEventListener('touchstart', down, {passive: false});
        c.el.addEventListener('dblclick', function (e) { e.stopPropagation(); c.set(0); onChange(0); });
        c.set(0);
        return c;
    }

    function buildMatrix(d, root) {
        var card = el('div', 'scx-card scx-matrix', root);
        el('h4', '', card, 'Group mod matrix');
        var hdr = el('div', 'scx-mrow scx-mhead', card);
        ['#', '', 'Source', 'Via', 'Curve', 'Target', 'Depth'].forEach(function (t) { el('div', '', hdr, t); });
        var list = el('div', 'scx-mlist', card);
        list.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        list.addEventListener('wheel', function (e) { e.stopPropagation(); });
        d.modRows = [];
        for (var i = 0; i < 18; i++) (function (i) {
            var r = {el: el('div', 'scx-mrow', list)};
            el('div', 'scx-mnum', r.el, '' + (i + 1));
            r.on = el('button', 'scx-mon on', r.el, 'ON');
            r.src = el('select', '', r.el); r.via = el('select', '', r.el);
            r.crv = el('select', '', r.el); r.tgt = el('select', '', r.el);
            [r.src, r.via, r.crv, r.tgt].forEach(function (x) { x.innerHTML = '<option value="">—</option>'; });
            function edit(field, v) { r.local = Date.now(); sendText(d, 'mod_edit', i + field, i + ',' + field + ',' + v); }
            r.on.addEventListener('click', function (e) {
                e.stopPropagation();
                var on = !r.on.classList.contains('on');
                r.on.classList.toggle('on', on); r.el.classList.toggle('off', !on);
                edit('a', on ? 1 : 0);
            });
            r.src.addEventListener('change', function () { edit('s', r.src.value); r.el.classList.toggle('used', !!(r.src.value && r.tgt.value)); });
            r.via.addEventListener('change', function () { edit('v', r.via.value); });
            r.crv.addEventListener('change', function () { edit('c', r.crv.value); });
            r.tgt.addEventListener('change', function () { edit('t', r.tgt.value); r.el.classList.toggle('used', !!(r.src.value && r.tgt.value)); });
            r.depth = makeDepth(r.el, function (v) { edit('d', v.toFixed(4)); });
            d.modRows.push(r);
        })(i);
    }

    function buildFx(d, root) {
        var fx = el('div', 'scx-card scx-fx', root);
        el('h4', '', fx, 'Group effects');
        var row = el('div', 'scx-fx-row', fx);
        d.fx = [];
        for (var s = 0; s < 4; s++) (function (s) {
            var card = el('div', 'scx-fx-slot empty', row);
            var head = el('div', 'scx-fx-head', card);
            el('span', 'scx-fx-num', head, 'FX' + (s + 1));
            var onBtn = el('button', 'scx-fx-on on', head, 'ON');
            onBtn.title = 'On / off';
            var typeSel = el('select', 'scx-fx-type', head);
            el('option', '', typeSel, '— none —').value = 0;
            var slot = {card: card, onBtn: onBtn, typeSel: typeSel, type: 0, active: true, floats: [], ints: []};
            slot.body = el('div', 'scx-fx-body', card);
            el('div', 'scx-fx-none', slot.body, 'Empty slot');
            onBtn.addEventListener('mousedown', function (e) { e.stopPropagation(); });
            onBtn.addEventListener('click', function (e) {
                e.stopPropagation();
                slot.active = !slot.active;
                onBtn.classList.toggle('on', slot.active);
                card.classList.toggle('off', !slot.active);
                fxEdit(d, s, 'a', 0, slot.active ? 1 : 0);
            });
            typeSel.addEventListener('mousedown', function (e) { e.stopPropagation(); });
            typeSel.addEventListener('change', function () { fxEdit(d, s, 't', 0, parseInt(typeSel.value, 10)); });
            d.fx.push(slot);
            fillTypeSelect(d, s);
        })(s);
    }

    // ---- build ----
    function build(d) {
        var root = d.icon.find('.scx-root')[0];
        root.innerHTML = '';
        d.ctl = {}; d.ctlMore = {}; d.hooks = {};

        var top = el('div', 'scx-top', root);
        el('div', 'scx-title', top, 'SHORTCIRCUIT XT');
        d.status = el('div', 'scx-sub scx-status', top, '');
        var grp = el('div', 'scx-group scx-ctl', top);
        var prev = el('button', '', grp, '◀'), gname = el('select', 'scx-gsel', grp), next = el('button', '', grp, '▶');
        d.groupLabel = gname;
        d.group = -1; // All groups
        gname.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        gname.addEventListener('change', function () { d.group = parseInt(gname.value, 10); showGroup(d); funcs.patch_set(U('group'), 'f', d.group); });
        function selGroup(delta) {
            // order: All, 1, 2, ... n
            var n = Math.max(1, d.groupCount || 1), g = Math.max(-1, Math.min(n - 1, (d.group === undefined ? -1 : d.group) + delta));
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
            if (g[2] === 'k') { spec.fmt = g[6]; bind(d, spec.uri, makeKnob(d, row, spec)); }
            else { spec.options = g[6]; bind(d, spec.uri, makeSelect(d, row, spec)); }
        });

        // instrument browser + EG1
        var mid = el('div', 'scx-mid', root);
        var br = el('div', 'scx-card scx-browser', mid);
        el('h4', '', br, 'Instruments & samples');
        var kinds = el('div', 'scx-kinds', br);
        d.kind = 'instruments';
        d.kindBtns = [['instruments', 'Instruments', 'sfz, sf2, akp, exs, gig, multisample, scp, scm'],
                      ['waves', 'Waves', 'wav, flac, aif, aiff, mp3, opus']].map(function (k) {
            var b = el('button', k[0] === d.kind ? 'on' : '', kinds, k[1]);
            b.kind = k[0]; b.title = k[2];
            b.addEventListener('mousedown', function (e) { e.stopPropagation(); });
            b.addEventListener('click', function (e) { e.stopPropagation(); setKind(d, k[0]); });
            return b;
        });
        var dd = el('div', 'scx-dd', br);
        d.ddBtn = el('button', 'scx-dd-btn', dd, '');
        d.ddPop = el('div', 'scx-dd-pop', dd); d.ddPop.hidden = true;
        [d.ddBtn, d.ddPop].forEach(function (x) { x.addEventListener('mousedown', function (e) { e.stopPropagation(); }); });
        d.ddPop.addEventListener('wheel', function (e) { e.stopPropagation(); });
        d.ddBtn.addEventListener('click', function (e) { e.stopPropagation(); openBrowser(d, d.ddPop.hidden); });
        // close when clicking anywhere else, or with Esc
        document.addEventListener('mousedown', function (e) { if (!d.ddPop.hidden && !dd.contains(e.target)) openBrowser(d, false); }, true);
        document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && !d.ddPop.hidden) openBrowser(d, false); });
        el('div', 'scx-hint', br, 'Click to open. Folders (and ..) open in the same list, which stays open; a file asks "Add" or "Replace".');
        el('div', 'scx-ld-title', br, 'Loaded in part 1');
        d.loadedBox = el('div', 'scx-loaded', br);
        d.loadedBox.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        d.loadedBox.addEventListener('wheel', function (e) { e.stopPropagation(); });
        el('div', 'scx-empty', d.loadedBox, 'Nothing loaded in part 1 yet.');
        d.entries = [];
        fillBrowser(d);
        buildRouting(d, mid);

        var mods = el('div', 'scx-mods', root);
        buildEG(d, mods);
        buildLFO(d, mods);

        buildFx(d, root);
        buildMatrix(d, root);

        // macros (control ports p0..p15)
        var mc = el('div', 'scx-card scx-macros', root);
        el('h4', '', mc, 'Macros');
        var grid = el('div', 'scx-grid', mc);
        for (var m = 0; m < 16; m++) {
            var sym = 'p' + m;
            if (d.ports && d.ports[sym] === undefined) continue;
            bind(d, sym, makeKnob(d, grid, {symbol: sym, label: 'Macro ' + (m + 1), min: 0, max: 1, def: 0,
                fmt: function (v) { return Math.round(v * 100) + '%'; }}));
        }

        // load dialog
        d.modal = el('div', 'scx-modal', root); d.modal.hidden = true;
        var dlg = el('div', 'scx-dialog', d.modal);
        d.dlgTitle = el('div', 'scx-dlg-title', dlg, 'Load into part 1');
        d.dlgName = el('div', 'scx-dlg-name', dlg, '');
        var btns = el('div', 'scx-dlg-btns', dlg);
        d.addBtn = el('button', '', btns, 'Add to part 1');
        d.addBtn.addEventListener('click', function (e) { e.stopPropagation(); doLoad(d, 0); });
        el('button', '', btns, 'Replace part 1').addEventListener('click', function (e) { e.stopPropagation(); doLoad(d, 1); });
        el('button', '', btns, 'Cancel').addEventListener('click', function (e) { e.stopPropagation(); d.modal.hidden = true; });
        d.modal.addEventListener('mousedown', function (e) { e.stopPropagation(); });
    }

    function showGroup(d) {
        if (!d.groupLabel) return;
        var n = d.groupCount || 0;
        var g = d.group === undefined ? -1 : d.group, sel = d.groupLabel, h = '';
        function esc(t) { return String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;'); }
        if (!n) h = '<option value="-1">No groups</option>';
        else {
            h = '<option value="-1">All groups (' + n + ')</option>';
            for (var i = 0; i < n; i++) {
                var nm = d.groupNames && d.groupNames[i];
                h += '<option value="' + i + '">Group ' + (i + 1) + (nm ? ': ' + esc(nm.name) : '') + '</option>';
            }
        }
        if (sel.innerHTML !== h) sel.innerHTML = h;
        sel.value = '' + (g < n ? g : -1);
        var cur = g >= 0 && d.groupNames && d.groupNames[g];
        sel.title = cur ? cur.zoneNames.join('\n') : 'Edits go to every group of part 1; the panel shows group 1';
        if (d.loadedBox) [].forEach.call(d.loadedBox.querySelectorAll('.scx-ld-row'), function (r, i) { r.classList.toggle('sel', g < 0 || i === g); });
    }

    function setValue(d, key, v, fromPanel) {
        // key: parameter uri or port symbol
        if (key === U('group')) { d.group = Math.round(v); showGroup(d); return; }
        if (key === U('group_count')) { d.groupCount = Math.round(v); showGroup(d); return; }
        if (key === U('listing')) { gotListing(d, v); return; }
        if (key === U('fx_types')) { var t = joinChunk(d, 'types', v); if (t !== null) gotFxTypes(d, t); return; }
        if (key === U('fx_desc')) { var dd = joinChunk(d, 'desc', v); if (dd !== null) gotFxDesc(d, dd); return; }
        if (key === U('mod_meta')) { var mm = joinChunk(d, 'meta', v); if (mm !== null) gotModMeta(d, mm); return; }
        if (key === U('mod_rows')) { var mr = joinChunk(d, 'rows', v); if (mr !== null) gotModRows(d, mr); return; }
        if (key === U('fx_values')) { var fv = joinChunk(d, 'values', v); if (fv !== null) gotFxValues(d, fv); return; }
        if (key === U('folders')) { gotFolders(d, v); return; }
        if (key === U('loaded')) { if (d.loaded !== undefined && v !== d.loaded) d.status.textContent = ''; d.loaded = v; return; }
        if (key === U('names')) { var nm = joinChunk(d, 'names', v); if (nm !== null) gotNames(d, nm); return; }
        if (key === U('mods_text')) { gotModsText(d, v); return; }
        if (!d.ctl) return;
        var list = [d.ctl[key]].concat(d.ctlMore[key] || []);
        list.forEach(function (c) {
            if (!c || (!fromPanel && c.local && Date.now() - c.local < 800)) return; // our own edit coming back
            c.set(v);
        });
        (d.hooks[key] || []).forEach(function (fn) { fn(v); });
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
