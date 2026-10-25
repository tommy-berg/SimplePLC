// The fleet (the PLCs of many devices on one page), Help, and the language reference with search.
'use strict';

// ───────────────────────────── Fleet ─────────────────────────────

/// The state of a PLC in a list: a word in its colour, never a badge.
function stateOf(plc) {
    if (plc.online === false) return ['Offline', 'is-off'];
    if (plc.state === 'fault') return ['Fault', 'is-fault'];
    return plc.running ? ['Running', 'is-run'] : ['Stopped', 'is-off'];
}

async function viewFleet() {
    const seq = app.routeSeq;
    setTitle('Fleet');
    const main = shell('fleet');
    const canAdopt = isAdmin();
    const addButton = canAdopt ? h('button', { class: 'btn btn-primary', type: 'button', onclick: () => adoptDialog() }, 'Add device') : null;
    const figures = h('dl', { class: 'figures fleet-figures' });
    const tbody = h('tbody');
    const table = h('div', { class: 'table-wrap fleet-table' }, h('table', { class: 'data', id: 'fleet' },
        h('thead', null, h('tr', null, h('th', { class: 'glyph' }, h('span', { class: 'visually-hidden' }, 'Lights')), h('th', null, 'PLC'), h('th', null, 'State'),
            h('th', { class: 'hide-sm' }, 'Program'), h('th', { class: 'hide-sm' }, 'Cycle'), h('th', { class: 'hide-sm' }, 'Links'),
            h('th', { class: 'hide-sm' }, 'Device'), h('th', { class: 'hide-sm' }, 'Version'), canAdopt ? h('th', { class: 'hide-sm' }) : null)),
        tbody));
    const emptyBox = h('div', { class: 'fleet-empty', hidden: true });
    const warnings = h('div');
    const agents = fleetAgents();
    main.append(
        h('div', { class: 'page-head' }, h('h1', { class: 'page-title' }, 'Fleet'), addButton ? h('div', { class: 'actions' }, addButton) : null),
        figures, table, emptyBox, warnings, agents);

    const rows = new Map();     // device url (or id) -> row
    const seenAt = new Map();   // device url -> when this page last heard from it
    const infos = new Map();    // PLC id -> device info
    let sources = new Map();    // device url -> how it joined: "flag" (the command line) or adopted
    let lastList = [];

    function renderEmpty() {
        fill(emptyBox,
            h('div', { class: 'empty' },
                h('p', { class: 'empty-title' }, 'No PLCs yet'),
                h('p', { class: 'muted' }, 'A fleet shows the PLCs of many devices on one page, and gives agents one MCP endpoint for all of them. ',
                    canAdopt ? 'Add a device by its address and pairing code. The code is on the device’s panel, under Settings, Fleet.'
                        : 'Start the fleet with --device <address> for each device, or list them in SIMPLEPLC_DEVICES.')),
            canAdopt ? adoptForm(() => refresh()) : null);
    }

    function row(plc) {
        const glyph = ledGlyph(plc.id);
        const r = {
            glyph,
            name: h('a', { href: `#/plc/${enc(plc.id)}/overview`, class: 'name' }),
            id: h('span', { class: 'secondary' }),
            sm: h('span', { class: 'secondary only-sm' }),
            state: h('td', { class: 'state' }),
            program: h('td', { class: 'hide-sm' }),
            cycle: h('td', { class: 'hide-sm num' }),
            links: h('td', { class: 'hide-sm' }),
            device: h('td', { class: 'hide-sm' }),
            version: h('td', { class: 'hide-sm num version' }),
            remove: canAdopt ? h('td', { class: 'hide-sm r' }) : null,
        };
        r.tr = h('tr', { class: 'is-link', dataset: { plc: plc.id } },
            h('td', { class: 'glyph' }, glyph.el), h('td', null, r.name, r.id, r.sm), r.state, r.program, r.cycle, r.links, r.device, r.version, r.remove);
        r.tr.addEventListener('click', e => {
            if (!e.target.closest('a, button')) location.hash = `#/plc/${enc(r.plc.id)}/overview`;
        });
        return r;
    }

    function updateRow(r, plc) {
        r.plc = plc;
        const key = plc.device ? plc.device.url : plc.id;
        const offline = plc.online === false;
        if (!offline) seenAt.set(key, Date.now());
        const seen = seenAt.get(key);
        r.glyph.update(lightState(plc, { offline }));
        setText(r.name, plcLabel(plc));
        r.name.setAttribute('href', `#/plc/${enc(plc.id)}/overview`);
        setText(r.id, plc.name && plc.name !== plc.id ? plc.id : plc.device ? plc.device.url.replace(/^https?:\/\//, '') : '');
        const [word, tone] = stateOf(plc);
        const f = plc.fault;
        const t = worstTask(plc);
        const links = (plc.modbus && plc.modbus.links) || [];
        const down = links.filter(linkDown).length;
        const lc = plc.last_change;
        fill(r.state, h('span', { class: tone }, word),
            offline ? h('span', { class: 'secondary' }, seen ? `Last seen ${formatWhen(seen)}` : plc.state === 'token_refused' ? 'Its token was refused' : 'Not seen since this page opened')
                : plc.forces ? h('span', { class: 'secondary is-attn' }, `${plc.forces} forced`) : null);
        const programs = [...new Set((plc.tasks || []).map(x => `${x.program}.st`))].join(', ');
        fill(r.program, offline ? h('span', { class: 'muted' }, '–') : f
            ? [h('span', { class: 'is-fault' }, f.code), h('span', { class: 'secondary' }, [faultWhere(f), formatWhen(f.at)].filter(Boolean).join(', '))]
            : [programs || '–', lc ? h('span', { class: 'secondary' }, `${formatWhen(lc.t_ms)}, ${byText(lc.by)}`) : null]);
        const load = t && t.exec_us && plc.running && !f ? (t.exec_us.avg / (t.cycle_ms * 1000)) * 100 : null;
        fill(r.cycle, offline || !t || f || !plc.running ? h('span', { class: 'muted' }, '–')
            : [`${t.cycle_ms}${NBSP}ms`, t.exceeded ? h('span', { class: 'secondary is-attn' }, plural(t.exceeded, 'exceeded cycle'))
                : h('span', { class: 'secondary' }, `Load ${load < 10 ? load.toFixed(2) : load.toFixed(1)}${NBSP}%`)]);
        fill(r.links, offline ? h('span', { class: 'muted' }, '–') : !links.length ? h('span', { class: 'muted' }, 'None')
            : h('span', { class: down ? 'is-fault' : '' }, `${links.filter(l => l.ok).length} of ${links.length} up`));
        const info = infos.get(plc.id);
        fill(r.device, info ? [hardwareText(info) || '–', info.cpu && info.cpu.temperature_c !== null && info.cpu.temperature_c !== undefined
            ? h('span', { class: 'secondary' }, `${Math.round(info.cpu.temperature_c)}${NBSP}°C`) : h('span', { class: 'secondary' }, info.device.hostname)]
            : h('span', { class: 'muted' }, '–'));
        setText(r.version, info && info.software ? info.software.simpleplc : '–');
        setText(r.sm, offline ? (seen ? `Last seen ${formatWhen(seen)}` : 'Not seen yet')
            : f ? [f.code, faultWhere(f)].filter(Boolean).join(', ') : down ? `Link down: ${links.length - down} of ${links.length} up`
                : t ? `${programs}, ${t.cycle_ms}${NBSP}ms` : '');
        r.sm.className = `secondary only-sm${f || down ? ' is-fault' : ''}`;
        if (r.remove) {
            const url = plc.device ? plc.device.url : null;
            // A device given on the command line (source "flag") stays until the fleet starts without it
            fill(r.remove, url && sources.has(url) && sources.get(url) !== 'flag'
                ? h('button', { class: 'link', type: 'button', onclick: () => removeDevice(plc) }, 'Remove…') : null);
        }
    }

    function update(list) {
        lastList = list;
        const counts = {
            PLCs: list.length,
            Running: list.filter(p => p.online !== false && p.running).length,
            Fault: list.filter(p => p.online !== false && p.state === 'fault').length,
            Stopped: list.filter(p => p.online !== false && !p.running && p.state !== 'fault').length,
            Offline: list.filter(p => p.online === false).length,
            Forced: list.filter(p => p.online !== false).reduce((n, p) => n + (p.forces || 0), 0),
        };
        fill(figures, Object.entries(counts).map(([k, v]) => h('div', null, h('dt', null, k),
            h('dd', { class: v && k === 'Fault' ? 'is-fault' : v && k === 'Forced' ? 'is-attn' : '' }, String(v)))));
        const empty = list.length === 0;
        figures.hidden = empty;
        table.hidden = empty;
        emptyBox.hidden = !empty;
        if (addButton) addButton.hidden = empty;
        if (empty && !emptyBox.firstChild) renderEmpty();
        const seen = new Set();
        let unknown = false;  // a PLC whose device this page has not asked about yet (just added, or back online)
        for (const plc of list) {
            const key = plc.device ? plc.device.url : plc.id;
            seen.add(key);
            let r = rows.get(key);
            if (!r) {
                r = row(plc);
                rows.set(key, r);
            }
            if (plc.online !== false && !infos.has(plc.id)) unknown = true;
            updateRow(r, plc);
            tbody.append(r.tr);  // keeps the server's order
        }
        for (const [key, r] of rows) {
            if (!seen.has(key)) {
                r.tr.remove();
                rows.delete(key);
            }
        }
        fill(warnings, list.flatMap(p => (p.online === false ? [] : p.warnings || []).map(w => h('p', { class: 'secondary is-attn' }, `${p.id}: ${w}`))));
        if (unknown && !fetchingInfos) fetchInfos();
    }

    let fetchingInfos = false;
    async function fetchInfos() {
        fetchingInfos = true;
        try {
            for (const plc of lastList.filter(p => p.online !== false)) {
                try {
                    infos.set(plc.id, await api('GET', `/plcs/${enc(plc.id)}/device`));
                } catch {
                    if (!infos.has(plc.id)) infos.set(plc.id, null);  // asked: the 15 s refresh tries again
                }
            }
        } finally {
            fetchingInfos = false;
        }
        for (const r of rows.values()) updateRow(r, r.plc);
    }

    async function refresh() {
        if (canAdopt) {
            const devices = await api('GET', '/fleet/devices').catch(() => null);  // only the Remove buttons depend on it
            if (devices) sources = new Map(devices.map(d => [d.url, d.source]));
        }
        update(await api('GET', '/plcs'));
    }

    async function removeDevice(plc) {
        if (!(await confirmDialog(`Remove ${plcLabel(plc)} from the fleet?`,
            `The fleet stops showing ${plc.id} and its agents lose it. The PLC itself keeps running; its token for this fleet is revoked.`, `Remove ${plc.id}`, true))) return;
        try {
            await api('DELETE', `/fleet/devices/${enc(plc.id)}`);
            toast(`Removed ${plc.id}`);
            refresh();
        } catch (err) {
            toast(err.message, 'error');
        }
    }

    async function adoptDialog() {
        const added = await dialog('Add device', close => adoptForm(() => close(true), () => close(false)));
        if (added) refresh();
    }

    await refresh();
    if (seq !== app.routeSeq) return;
    fetchInfos();
    const infoTimer = setInterval(fetchInfos, 15000);
    onLeave(() => clearInterval(infoTimer));
    live('/events', { plcs: update }, 2000);
}

/// Address and pairing code: a device joins a fleet only with its code. With `cancel` it is the
/// whole form of the Add device dialog; without, the form of an empty fleet.
function adoptForm(done, cancel) {
    const inDialog = Boolean(cancel);
    const problems = h('div', { class: 'problems', role: 'alert' });
    const addr = h('input', { class: 'input w-lg', id: 'adopt-addr', placeholder: 'pi-07.local or 192.168.1.47', autocomplete: 'off', spellcheck: 'false', required: true });
    const code = h('input', { class: 'input w-sm', id: 'adopt-code', placeholder: 'ABCD EFGH', autocapitalize: 'characters', autocomplete: 'off', spellcheck: 'false', required: true });
    const submit = async e => {
        e.preventDefault();
        let url = addr.value.trim();
        if (!/^https?:\/\//.test(url)) url = `http://${url}`;
        if (!/:\d+$/.test(new URL(url).host)) url = `${url.replace(/\/$/, '')}:8080`;
        try {
            await api('POST', '/fleet/devices', { url, code: code.value.replace(/\s/g, '') });
            toast(`Added ${url}`);
            done();
        } catch (err) {
            showProblems(problems, err);
        }
    };
    const fields = [
        h('div', { class: 'field' }, h('label', { for: 'adopt-addr' }, 'Address'), addr, inDialog ? h('p', { class: 'hint' }, 'With the port when it is not 8080.') : null),
        h('div', { class: 'field' }, h('label', { for: 'adopt-code' }, 'Pairing code'), code,
            inDialog ? h('p', { class: 'hint' }, 'On the device’s panel under Settings, Fleet. The device gives this fleet a token of its own; revoking it there removes the device from the fleet.') : null)];
    if (inDialog) {
        return dialogForm({
            title: 'Add a device to the fleet', body: [problems, fields], onsubmit: submit,
            actions: [h('button', { type: 'button', class: 'btn', onclick: cancel }, 'Cancel'), h('button', { type: 'submit', class: 'btn btn-primary' }, 'Add device')],
        });
    }
    return h('form', { class: 'fields adopt', onsubmit: submit }, fields, h('button', { class: 'btn btn-primary', type: 'submit' }, 'Add device'), problems);
}

/// One MCP endpoint for every PLC of the fleet, with a command that carries a new token.
function fleetAgents() {
    const base = `${location.protocol}//${location.host}`;
    const result = h('div', { class: 'result', hidden: true });
    const button = isAdmin() ? h('button', { class: 'btn', type: 'button', onclick: async () => {
        button.disabled = true;
        try {
            const d = new Date();
            const created = await api('POST', '/tokens', { name: `agent-${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}-${pad2(d.getHours())}${pad2(d.getMinutes())}`, role: 'admin' });
            const command = mcpCommand(base, created.token);
            const copied = await copy(command);
            fill(result, h('pre', { class: 'command' }, command),
                h('p', { class: 'secondary' }, `${copied ? 'Copied. ' : ''}It carries a new fleet token, ${created.name}; revoke it under Users and tokens. It is shown only here, once.`));
            result.hidden = false;
        } catch (err) {
            toast(err.message, 'error');
        }
        button.disabled = false;
    } }, 'Copy command') : null;
    const el = section('Agents', button,
        h('p', null, 'One MCP endpoint for every PLC in this fleet: ', h('span', { class: 'num' }, `${base}/mcp`)),
        result,
        h('p', { class: 'section-note' }, 'The command carries a fleet token. An agent then sees each PLC by its id and acts with the fleet’s rights on that device.'));
    el.classList.add('fleet-agents');
    return el;
}

// ───────────────────────────── Help ─────────────────────────────

async function viewHelp() {
    const seq = app.routeSeq;
    setTitle('Help');
    const main = shell('help');
    const plcs = await api('GET', '/plcs').catch(() => []);
    if (seq !== app.routeSeq) return;
    const base = `${location.protocol}//${location.host}`;
    // In a fleet, the endpoints are those of the PLC picked here
    const picker = app.mode === 'fleet' && plcs.length > 1
        ? h('select', { class: 'select', 'aria-label': 'PLC to connect to' }, plcs.map(p => h('option', { value: p.id }, p.name && p.name !== p.id ? `${p.id} (${p.name})` : p.id)))
        : null;
    const connect = h('div', { class: 'connect' });
    const units = new Map();  // PLC id -> its Modbus unit id, from its configuration (the list carries none)
    async function renderConnect() {
        const plc = picker ? plcs.find(p => p.id === picker.value) : plcs[0];
        const mb = plc && modbusEndpoint(plc);
        const ua = plc && opcuaEndpoint(plc);
        if (mb && !units.has(plc.id)) {
            try {
                units.set(plc.id, (await api('GET', `/plcs/${enc(plc.id)}`)).config.device.unit_id);
            } catch {
                units.set(plc.id, null);  // not known: the row says the endpoint without a unit
            }
            if (seq !== app.routeSeq || (picker && picker.value !== plc.id)) return;
        }
        const unit = mb && units.get(plc.id) !== null ? `, unit ${units.get(plc.id)}` : '';
        fill(connect, dl([
            mb ? [picker ? `Modbus TCP, ${plc.id}` : 'Modbus TCP', `${mb}${unit}. Addresses start at 0. Coils: functions 1, 5, 15. Discrete inputs: 2. Holding registers: 3, 6, 16, 22, 23. Input registers: 4. A 32-bit value takes two registers, high word first.`] : null,
            ua ? [picker ? `OPC UA, ${plc.id}` : 'OPC UA', `${ua}. Each tag is a variable with node id ns=1;s=<tag>, in the folder ${plc.name || plc.id} under Objects. Security policy None, anonymous.`] : null,
            ['Agents (MCP)', [h('pre', { class: 'command' }, mcpCommand(base, '<token>')),
                h('span', { class: 'muted' }, isAdmin() ? (app.mode === 'fleet' ? 'Create the token under Users and tokens. One endpoint for every PLC of the fleet: tools take the PLC id.'
                    : 'Create the token in Settings, Users and tokens, or use Connect an agent on an empty PLC. Without a running SimplePLC, “simpleplc mcp” serves MCP on stdio with its own PLC.')
                    : 'An administrator can create a token for you.')]],
            ['REST', `${base}/api/v1, described in ${base}/api/v1/openapi.json.`],
        ]));
    }
    if (picker) picker.addEventListener('change', renderConnect);
    renderConnect();

    const state = (word, tone, text) => [h('dt', { class: tone }, word), h('dd', null, text)];
    const job = (title, text) => h('div', null, h('h3', null, title), h('p', null, text));
    main.append(
        h('div', { class: 'page-head' }, h('h1', { class: 'page-title' }, 'Help'),
            h('div', { class: 'actions' }, h('a', { class: 'btn', href: '#/help/reference' }, 'Structured Text reference'))),
        h('div', { class: 'rows rows-top' },
            row('States', h('dl', { class: 'states-dl' },
                state('Running', 'is-run', 'The program runs every cycle and drives the outputs.'),
                state('Stopped', 'is-off', 'The program does not run. Outputs with a substitute value get it; the others keep their last value. Modbus and OPC UA keep serving, and links keep polling.'),
                state('Fault', 'is-fault', 'A task stopped on an error, such as F_DIV_ZERO. Outputs went to their substitute values. The PLC page names the task, file and line. Fix the program, download it, then use Warm restart.'),
                state('Offline', 'is-off', 'The page cannot reach the device. It shows the last known state and when it was last seen. The PLC itself may still be running.'),
                state('Forced', 'is-attn', 'A value held by hand. The program and links cannot change it until it is released. Forced values are amber everywhere, and counted in the header.'))),
            row('Everyday jobs', h('div', { class: 'jobs' },
                job('Change a value', 'Click it in I/O or in Watch. Modify shows the current and the new value; Enter writes it.'),
                job('Hold a value', 'Force it from Modify. Release it on the Overview, one at a time or all together.'),
                job('Change the program', 'Edit it under Program, then Download. The preview lists what keeps its value and what starts again.'),
                job('Stop safely', 'Stop lists every output and the substitute value it will get before anything happens.'),
                job('Restart', 'A warm restart keeps RETAIN variables. A cold restart starts every variable from its initial value.'),
                job('Connect a machine', 'I/O, Add device. Probe it, tick and name what to read and write, and create the tags and the link together.'),
                job('Hand the I/O to SCADA', 'I/O, Export register map: every tag with its Modbus address and OPC UA node id, as CSV.'),
                job('Test without a machine', 'Run the program on a PLC that drives no equipment, with a plant model such as Factory I/O, pymodbus or another PLC on its tags.'),
                job('Find out what happened', 'Diagnostics. Every change is in the diagnostics buffer with who made it: a user, a token or the system.'))),
            row(['Connect', picker ? h('span', { class: 'connect-picker' }, picker) : null], connect),
            row('This panel', h('p', { class: 'muted' }, `SimplePLC ${app.version}. The field display for a touchscreen at the machine: open the panel with ?field, or choose it in Settings, Display.`))));
}

// ───────────────────────────── The reference ─────────────────────────────

/// A heading's anchor as GitHub writes it: "Tags: VAR_EXTERNAL" -> "tags-var_external".
function headingSlug(text) {
    return text.toLowerCase().replace(/`/g, '').replace(/[^\p{L}\p{N}\s_-]/gu, '').trim().replace(/\s/g, '-');
}

/// Splits a Markdown document into its ## sections (the text before the first one is the intro).
function markdownSections(text, docTitle) {
    const out = [];
    let currentSection = { title: docTitle, level: 1, lines: [] };
    let fence = false;
    for (const line of text.replace(/\r/g, '').split('\n')) {
        if (line.startsWith('```')) fence = !fence;
        if (!fence && /^## /.test(line)) {
            out.push(currentSection);
            currentSection = { title: line.slice(3).trim(), level: 2, lines: [line] };
        } else {
            currentSection.lines.push(line);
        }
    }
    out.push(currentSection);
    return out.map(s => ({ ...s, text: s.lines.join('\n') })).filter(s => s.text.trim());
}

async function viewReference(query) {
    const seq = app.routeSeq;
    setTitle('Structured Text reference');
    const main = shell('help');
    const [doc, compliance] = await Promise.all([api('GET', '/docs/structured-text'), api('GET', '/docs/st-compliance').catch(() => '')]);
    if (seq !== app.routeSeq) return;
    const docs = [
        { name: 'Structured Text', sections: markdownSections(doc, 'Structured Text in SimplePLC') },
        { name: 'IEC 61131-3 compliance', sections: compliance ? markdownSections(compliance, 'SimplePLC compliance statement') : [] },
    ];
    const search = h('input', { class: 'input', type: 'search', id: 'ref-search', 'aria-label': 'Search the reference', placeholder: 'A keyword, function, function block or fault', value: query.get('q') || '' });
    const hint = h('p', { class: 'hint', role: 'status' });
    const nav = h('nav', { 'aria-label': 'Reference' });
    const article = h('article', { class: 'doc' });
    const all = [];
    for (const d of docs) {
        nav.append(h('span', { class: 'group' }, d.name));
        for (const s of d.sections) {
            const wrap = h('section', { class: 'ref-section' });
            const entry = { ...s, doc: d.name, wrap, plain: s.text.toLowerCase(), link: null };
            entry.link = h('a', { href: '#/help/reference', onclick: e => {
                e.preventDefault();
                if (wrap.hidden) {
                    search.value = '';
                    apply();
                }
                wrap.scrollIntoView({ block: 'start' });
            } }, s.level === 1 ? (d.name === 'Structured Text' ? 'Introduction' : 'Statement') : s.title.replace(/`/g, ''));
            nav.append(entry.link);
            all.push(entry);
            article.append(wrap);
        }
    }
    function render(entry) {
        fill(entry.wrap);
        renderMarkdown(entry.text, entry.wrap, 'ref-', article, () => { search.value = ''; apply(); });
    }
    function highlight(root, q) {
        if (!q) return;
        const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        const nodes = [];
        while (walker.nextNode()) nodes.push(walker.currentNode);
        const lower = q.toLowerCase();
        for (const node of nodes) {
            const text = node.nodeValue;
            let at = text.toLowerCase().indexOf(lower);
            if (at < 0) continue;
            const parts = [];
            let last = 0;
            while (at >= 0) {
                parts.push(text.slice(last, at), h('mark', null, text.slice(at, at + q.length)));
                last = at + q.length;
                at = text.toLowerCase().indexOf(lower, last);
            }
            parts.push(text.slice(last));
            const span = document.createDocumentFragment();
            append(span, parts.filter(p => p !== ''));
            node.replaceWith(span);
        }
    }
    function apply() {
        const q = search.value.trim();
        const matches = q ? all.filter(e => e.plain.includes(q.toLowerCase())) : all;
        for (const e of all) {
            const show = matches.includes(e);
            e.wrap.hidden = !show;
            e.link.hidden = !show && Boolean(q);
            e.link.removeAttribute('aria-current');
            if (show) {
                render(e);
                highlight(e.wrap, q);
            }
        }
        setText(hint, !q ? `${all.length} sections. Search finds keywords, functions, function blocks and faults.`
            : matches.length ? `${plural(matches.length, 'section')}: ${matches.slice(0, 6).map(e => e.title.replace(/`/g, '')).join(', ')}${matches.length > 6 ? ', …' : ''}.`
                : `Nothing matches “${q}”.`);
        if (q && matches.length) matches[0].link.setAttribute('aria-current', 'true');
    }
    let timer = null;
    search.addEventListener('input', () => {
        clearTimeout(timer);
        timer = setTimeout(apply, 150);
    });
    main.append(
        h('p', { class: 'crumb' }, h('a', { href: '#/help' }, 'Help')),
        h('h1', { class: 'page-title' }, 'Structured Text reference'),
        h('div', { class: 'field ref-search' }, search, hint),
        h('div', { class: 'ref' }, nav, article));
    apply();
    if (query.get('q')) {
        const first = all.find(e => !e.wrap.hidden);
        const mark = first && first.wrap.querySelector('mark');
        if (mark) setTimeout(() => mark.scrollIntoView({ block: 'center' }), 0);
    }
    search.focus();
}

/// Markdown subset for the bundled docs: headings, paragraphs, lists, tables, code. Links within the
/// document scroll to their heading (the address bar's # belongs to the panel's pages). `scope` is
/// where those headings are looked up; `reveal` shows them when a search hid them.
function renderMarkdown(text, root, prefix, scope, reveal) {
    const lines = text.replace(/\r/g, '').split('\n');
    let i = 0;
    const inline = s => {
        const out = [];
        const re = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\[[^\]]+\]\([^)]+\))/g;
        let last = 0;
        let m;
        while ((m = re.exec(s))) {
            if (m.index > last) out.push(s.slice(last, m.index));
            const t = m[0];
            if (t.startsWith('`')) out.push(h('code', null, t.slice(1, -1)));
            else if (t.startsWith('**')) out.push(h('strong', null, t.slice(2, -2)));
            else {
                const [, label, url] = /\[([^\]]+)\]\(([^)]+)\)/.exec(t);
                if (/^https?:\/\//.test(url)) out.push(h('a', { href: url, rel: 'noopener', target: '_blank' }, label));
                else if (url.startsWith('#')) {
                    out.push(h('a', {
                        href: location.hash || '#/help/reference', class: 'doc-link', onclick: e => {
                            e.preventDefault();
                            let target = scope.querySelector(`[id="${prefix}${url.slice(1)}"]`);
                            if ((!target || target.closest('[hidden]')) && reveal) {
                                reveal();
                                target = scope.querySelector(`[id="${prefix}${url.slice(1)}"]`);
                            }
                            if (target) {
                                target.scrollIntoView({ block: 'start' });
                                target.focus({ preventScroll: true });
                            }
                        },
                    }, label));
                } else out.push(label);
            }
            last = m.index + t.length;
        }
        if (last < s.length) out.push(s.slice(last));
        return out;
    };
    while (i < lines.length) {
        const line = lines[i];
        if (line.trim().startsWith('<!--')) {
            while (i < lines.length && !lines[i].includes('-->')) i++;  // comments are for tools, not readers
            i++;
        } else if (line.startsWith('```')) {
            const code = [];
            i++;
            while (i < lines.length && !lines[i].startsWith('```')) code.push(lines[i++]);
            i++;
            root.append(h('pre', null, h('code', null, code.join('\n'))));
        } else if (/^#{1,4} /.test(line)) {
            const level = line.indexOf(' ');
            const tag = `h${Math.max(2, level)}`;  // the page has its own h1: a document's title and its sections are h2
            root.append(h(tag, { id: prefix + headingSlug(line.slice(level + 1)), tabindex: '-1', class: level <= 2 ? 'section-title' : null }, inline(line.slice(level + 1))));
            i++;
        } else if (line.startsWith('|')) {
            const rows = [];
            while (i < lines.length && lines[i].startsWith('|')) rows.push(lines[i++]);
            const cells = r => r.replace(/^\||\|$/g, '').split(/(?<!\\)\|/).map(c => c.trim().replace(/\\\|/g, '|'));
            const head = cells(rows[0]);
            const bodyRows = rows.slice(2).map(cells);
            root.append(h('div', { class: 'table-wrap' }, h('table', { class: 'data data-compact' },
                h('thead', null, h('tr', null, head.map(c => h('th', null, inline(c))))),
                h('tbody', null, bodyRows.map(r => h('tr', null, r.map(c => h('td', null, inline(c)))))))));
        } else if (/^(- |\d+\. )/.test(line)) {
            const ordered = !line.startsWith('- ');
            const start = ordered ? /^(- |\d+\. )/ : /^- /;
            const items = [];
            while (i < lines.length && (start.test(lines[i]) || (items.length && /^ {2,}\S/.test(lines[i])))) {
                if (start.test(lines[i])) items.push(lines[i].replace(/^(- |\d+\. )/, ''));
                else items[items.length - 1] += ' ' + lines[i].trim();
                i++;
            }
            root.append(h(ordered ? 'ol' : 'ul', null, items.map(t => h('li', null, inline(t)))));
        } else if (line.trim() === '') {
            i++;
        } else {
            const para = [];
            while (i < lines.length && lines[i].trim() !== '' && !/^(#|```|\||- |\d+\. |<!--)/.test(lines[i])) para.push(lines[i++]);
            root.append(h('p', null, inline(para.join(' '))));
        }
    }
    return root;
}
