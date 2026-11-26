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

    // option values: "B<folder>" open, "L<file>" load, "S" remember, "R" forget
    function fillBrowser(d) {
        var sel = d.browser;
        if (!sel) return;
        sel.innerHTML = '';
        function opt(parent, value, label) { var o = el('option', '', parent, label); o.value = value; return o; }
        function grp(label) { var g = el('optgroup', '', sel); g.label = label; return g; }
        var head = opt(sel, '', d.cwd === undefined ? 'Waiting for the plugin…' :
            '\uD83D\uDCC2 ' + baseName(d.cwd) + (d.complete ? '' : ' (loading…)') + '  \u2014 choose a folder or file');
        head.disabled = true;
        if (d.cwd !== undefined) {
            var here = grp('This folder: ' + d.cwd);
            if (d.cwd !== '/') opt(here, 'B' + parentOf(d.cwd), '\u2B11  .. (up)');
            var dirs = d.entries.filter(function (e) { return e.dir; }), files = d.entries.filter(function (e) { return !e.dir && shown(d, e.name); });
            dirs.forEach(function (e) { opt(here, 'B' + joinPath(d.cwd, e.name), '\uD83D\uDCC1  ' + e.name); });
            files.forEach(function (e) { opt(here, 'L' + joinPath(d.cwd, e.name), '\u266A  ' + e.name); });
            if (!files.length && !dirs.length && d.complete) opt(here, '', '(no ' + (d.kind === 'waves' ? 'waves' : 'instruments') + ' here)').disabled = true;
        }
        var go = grp('Go to');
        if (d.defaultDir) {
            opt(go, 'B' + d.defaultDir, '\u2302  Instruments');
            var home = d.defaultDir.replace(/\/\.local\/share\/ShortCircuit-XT$/, '');
            if (home !== d.defaultDir) opt(go, 'B' + home, '\u2302  Home');
        }
        opt(go, 'B/media', '\u2302  USB / media');
        (d.saved || []).forEach(function (f) { opt(go, 'B' + f, '\u2605  ' + baseName(f) + '   (' + f + ')'); });
        if (d.cwd !== undefined && d.cwd !== d.defaultDir) {
            var isSaved = (d.saved || []).indexOf(d.cwd) >= 0;
            opt(go, isSaved ? 'R' : 'S', isSaved ? '\u2716  Forget this folder' : '\u2606  Remember this folder');
        }
        sel.value = '';
        sel.title = d.cwd || '';
    }

    function browserChoice(d) {
        var v = d.browser.value;
        d.browser.value = '';
        if (!v) return;
        var kind = v[0], path = v.substring(1);
        if (kind === 'B') browse(d, path);
        else if (kind === 'L') askLoad(d, path);
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

    function fxEdit(d, slot, kind, index, v) {
        // at most one message per control every 40 ms while dragging; the last value always goes
        var key = slot + kind + index, q = d.fxQueue = d.fxQueue || {};
        var first = !q[key];
        q[key] = slot + ',' + kind + ',' + index + ',' + v;
        if (!first) return;
        funcs.patch_set(U('fx_edit'), 's', q[key]);
        var last = q[key];
        setTimeout(function flush() {
            if (q[key] !== last) { last = q[key]; funcs.patch_set(U('fx_edit'), 's', last); setTimeout(flush, 40); }
            else delete q[key];
        }, 40);
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
            var onBtn = el('button', 'scx-fx-on on', head, '⏻');
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
        d.browser = el('select', 'scx-browse', br);
        d.browser.addEventListener('mousedown', function (e) { e.stopPropagation(); });
        d.browser.addEventListener('change', function () { browserChoice(d); });
        el('div', 'scx-hint', br, 'Folders open in the same list; a file asks "Add" or "Replace". \u2606 in the list remembers a folder.');
        d.entries = [];
        fillBrowser(d);

        var eg = el('div', 'scx-card scx-eg', mid);
        el('h4', '', eg, 'Group EG1 (amp)');
        var sl = el('div', 'scx-sliders', eg);
        EG.forEach(function (e) {
            var spec = {uri: U(e[0]), label: e[1], min: 0, max: 1, def: e[2], fmt: e[3]};
            d.ctl[spec.uri] = makeVSlider(d, sl, spec);
        });

        buildFx(d, root);

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
        d.groupLabel.textContent = n ? ('Group ' + ((d.group || 0) + 1) + ' / ' + n) : 'No groups';
    }

    function setValue(d, key, v) {
        // key: parameter uri or port symbol
        if (key === U('group')) { d.group = Math.round(v); showGroup(d); return; }
        if (key === U('group_count')) { d.groupCount = Math.round(v); showGroup(d); return; }
        if (key === U('listing')) { gotListing(d, v); return; }
        if (key === U('fx_types')) { var t = joinChunk(d, 'types', v); if (t !== null) gotFxTypes(d, t); return; }
        if (key === U('fx_desc')) { var dd = joinChunk(d, 'desc', v); if (dd !== null) gotFxDesc(d, dd); return; }
        if (key === U('fx_values')) { var fv = joinChunk(d, 'values', v); if (fv !== null) gotFxValues(d, fv); return; }
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
