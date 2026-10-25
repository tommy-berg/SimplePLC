// The PLC page: the header (the instrument: controller front, state word, cycle figures, facts),
// the tabs Overview, Communication, Diagnostics and Settings, and the confirmations of Stop,
// Run, restarts and Release all. I/O is in io.js, Program in program.js.
'use strict';

const TAB_LABELS = { overview: 'Overview', program: 'Program', io: 'I/O', communication: 'Communication', diagnostics: 'Diagnostics', settings: 'Settings' };

/// The task whose cycle is worst: exceeded cycles first, then the largest share of its cycle spent executing.
function worstTask(plc) {
    const tasks = (plc.tasks || []).filter(t => t.enabled !== false);
    if (!tasks.length) return null;
    const load = t => (t.exec_us && t.exec_us.avg ? t.exec_us.avg : 0) / ((t.cycle_ms || 1) * 1000);
    return tasks.reduce((a, b) => ((b.exceeded || 0) > (a.exceeded || 0) || ((b.exceeded || 0) === (a.exceeded || 0) && load(b) > load(a)) ? b : a));
}

/// Where a fault happened, e.g. "main.st line 12" or "loop_lib.st line 6"; empty for a fault with no
/// place in the code (F_BINDING: file "" and line 0).
function faultWhere(f) {
    return [f.file, f.line ? `line ${f.line}` : ''].filter(Boolean).join(' ');
}

/// A fault in the task's own program file, which its editor can show (not in a library file).
function faultInProgram(f) {
    return !f.file || f.file === `${f.program}.st`;
}

function sentence(text) {
    const s = String(text || '');
    return s ? s[0].toUpperCase() + s.slice(1) : s;
}

/// Nothing in the process image and nothing connected: the three ways forward.
function isEmptyPlc(plc) {
    return plc.tag_count === 0 && plc.config.links.length === 0;
}

function substituteText(tag) {
    if (tag.substitute === undefined || tag.substitute === null) return null;
    if (tag.type === 'bool') return tag.substitute === true || tag.substitute === 1 ? 'TRUE' : 'FALSE';
    return String(tag.substitute);
}

/// The device's address on its network: the one this page reached, else the first address of an
/// interface that is up.
function deviceIp(ctx) {
    const info = ctx.info;
    const host = ctx.plc.device && ctx.plc.device.url ? new URL(ctx.plc.device.url).hostname : location.hostname;
    if (!info) return /^[\d.]+$/.test(host) ? host : null;
    const addresses = (info.network || []).filter(n => n.up && isPhysicalInterface(n.name)).flatMap(n => n.addresses.map(a => a.split('/')[0]));
    if (addresses.includes(host)) return host;
    return addresses.find(a => /^\d+\.\d+\.\d+\.\d+$/.test(a)) || addresses[0] || null;
}

function hardwareText(info) {
    if (!info || !info.device) return null;
    const d = info.device;
    return d.container ? `${d.model || 'Computer'}, in a ${d.container} container` : d.model || null;
}

// ───────────────────────────── Events: the diagnostics buffer ─────────────────────────────

/// One record of the diagnostics buffer, as the events answer gives it (t_ms becomes time).
function normalizeEvent(r) {
    return { time: r.t_ms, message: r.message, level: r.level, event: r.event, by: r.by, seq: r.seq };
}

/// The diagnostics buffer of a PLC (GET /plcs/:id/events?limit=N): every event with who made it,
/// oldest first.
async function plcEvents(id, limit) {
    const data = await api('GET', `/plcs/${enc(id)}/events?limit=${limit}`);
    return data.events.map(normalizeEvent).sort((a, b) => a.time - b.time);
}

/// The group of a record: faults by their level or message, the others by their kind (link, access;
/// config, program, mode, write, force and release are changes).
function eventGroup(e) {
    if (e.level === 'error' || /\bfault/i.test(e.message)) return 'faults';
    if (e.event === 'link') return 'communication';
    if (e.event === 'access') return 'access';
    return 'changes';
}

function eventText(e) {
    const tone = e.level === 'error' ? 'is-fault' : e.level === 'warn' ? 'is-attn' : '';
    return tone ? h('span', { class: tone }, sentence(e.message)) : sentence(e.message);
}

function eventBy(e) {
    if (!e.by) return h('span', { class: 'muted' }, 'system');
    const text = byText(e.by);
    return text === 'system' ? h('span', { class: 'muted' }, 'system') : text;
}

// ───────────────────────────── The PLC page ─────────────────────────────

async function viewPlc(id, tab, sub, query) {
    const seq = app.routeSeq;
    const main = shell('plc');
    let device = null;
    let fleetEntry = null;
    if (app.mode === 'fleet') {
        fleetEntry = (await api('GET', '/plcs')).find(p => p.id === id) || null;
        if (seq !== app.routeSeq) return;
        device = fleetEntry ? fleetEntry.device : null;
    }
    const withDevice = p => (device ? { ...p, device } : p);
    let plc;
    try {
        plc = withDevice(await api('GET', `/plcs/${enc(id)}`));
    } catch (err) {
        if (app.mode !== 'fleet' && err.status === 404) {  // renamed: a device has one PLC
            app.plcId = null;
            const own = await ownPlcId();
            if (seq === app.routeSeq) location.replace(`#/plc/${enc(own)}/${tab}`);
            return;
        }
        if (!fleetEntry || fleetEntry.online !== false) throw err;
        plc = fleetEntry;  // the device does not answer: what the fleet heard last
    }
    if (seq !== app.routeSeq) return;
    if (app.mode !== 'fleet') app.plcId = plc.id;
    setTitle(`${plcLabel(plc)}: ${TAB_LABELS[tab]}`);

    const ctx = {
        id, tab, seq,
        get plc() { return plc; },
        get config() { return plc.config; },
        values: new Map(), forced: new Set(),
        info: null, bits: null, offlineEvent: false, starting: false,
        lastSeen: plc.online === false ? null : Date.now(),
        get offline() { return plc.online === false || ctx.offlineEvent || app.stale.size > 0; },
        applyStatus: next => applyStatus(next),
        setStarting(on) { ctx.starting = on; header.update(); },
        tags: () => tagList(),
    };

    /// The tag list (GET /plcs/:id/tags: each tag with its value and who writes it), fetched once
    /// per configuration version.
    let tagCache = { version: null, list: null, pending: null };
    function tagList() {
        const version = plc.config_version;
        if (tagCache.list && tagCache.version === version) return Promise.resolve(tagCache.list);
        if (!tagCache.pending) {
            tagCache.pending = api('GET', `/plcs/${enc(id)}/tags`).then(list => {
                tagCache = { version, list, pending: null };
                return list;
            }, err => {
                tagCache.pending = null;
                throw err;
            });
        }
        return tagCache.pending;
    }

    const header = plcHeader(ctx);
    const body = h('div', { class: 'tab-body' });
    main.append(header.el, body);

    let view = { el: h('div') };
    // A device the fleet does not reach: nothing of its PLC is known (no configuration, the counts are
    // placeholders), so every tab says so instead of showing it as empty or as programmed
    const unreached = plc.online === false || !plc.config;
    if (unreached) {
        view = { el: offlineEmpty(ctx) };
    } else {
        const views = { overview: tabOverview, program: tabProgram, io: tabIo, communication: tabCommunication, diagnostics: tabDiagnostics, settings: tabSettings };
        view = await views[tab](ctx, sub, query);
        if (seq !== app.routeSeq) return;
    }
    body.append(view.el);
    if (view.mounted) view.mounted();

    function applyStatus(next) {
        plc = withDevice(next);
        if (plc.online !== false) ctx.lastSeen = Date.now();
        setTitle(`${plcLabel(plc)}: ${TAB_LABELS[tab]}`);
        header.update();
        if (view.status) view.status(plc);
    }
    applyStatus(plc);

    // The device: model, address and workload (facts, the LINK lights, the meta line)
    let infoPending = false;
    async function fetchInfo() {
        if (infoPending || ctx.offline) return;
        infoPending = true;
        try {
            ctx.info = await api('GET', `/plcs/${enc(id)}/device`);
            header.update();
            if (view.info) view.info(ctx.info);
        } catch {}
        infoPending = false;
    }
    fetchInfo();
    const infoTimer = setInterval(fetchInfo, 5000);

    // The first eight Discrete inputs and Coils, for the I/O lights of the front (Overview only)
    let bitsPending = false;
    async function fetchBits() {
        if (bitsPending || ctx.offline) return;
        bitsPending = true;
        try {
            const n = Math.min(8, plc.image_size);
            const [di, coils] = await Promise.all([
                api('GET', `/plcs/${enc(id)}/areas/discrete_input?start=0&count=${n}`),
                api('GET', `/plcs/${enc(id)}/areas/coil?start=0&count=${n}`)]);
            ctx.bits = { di: di.values, coils: coils.values };
            header.updateFront();
        } catch {}
        bitsPending = false;
    }
    let bitsTimer = null;
    if (tab === 'overview') {
        fetchBits();
        bitsTimer = setInterval(fetchBits, 700);
    }
    const onStale = () => header.update();
    app.staleListeners.add(onStale);
    onLeave(() => {
        clearInterval(infoTimer);
        if (bitsTimer) clearInterval(bitsTimer);
        app.staleListeners.delete(onStale);
    });

    if (unreached) {
        // The PLC has no stream while its device is unreached (the fleet refuses one, which would read as
        // a lost connection): watch the fleet's list and show the PLC when its device answers. Until then
        // the PLC is named after the device's address, so the device's url is what to look for.
        const url = device ? device.url : null;
        live('/events', { plcs: list => {
            const reached = list.find(p => p.device && p.device.url === url && p.online !== false);
            if (!reached) return;
            if (reached.id === id) route();
            else location.replace(`#/plc/${enc(reached.id)}/${tab}`);
        } }, 2000);
        return;
    }

    live(`/plcs/${enc(id)}/events`, {
        status: next => {
            ctx.offlineEvent = false;
            applyStatus(next);
        },
        values: v => {
            for (const [name, value] of Object.entries(v.values || {})) ctx.values.set(name, value);
            ctx.forced = new Set(v.forced || []);
            if (view.values) view.values(v);
        },
        offline: () => {
            // A fleet: the device stopped answering. Its last report stays, marked Offline
            ctx.offlineEvent = true;
            header.update();
            if (view.status) view.status(plc);
        },
        gone: async () => {
            // A device has one PLC: when its id is gone, it was renamed. Follow it.
            if (app.mode !== 'fleet') {
                const plcs = await api('GET', '/plcs').catch(() => []);
                if (seq !== app.routeSeq) return;
                if (plcs.length === 1) {
                    app.plcId = plcs[0].id;
                    location.hash = `#/plc/${enc(plcs[0].id)}/${tab}`;
                    return;
                }
            }
            toast(`${id} was renamed or left the fleet`);
            location.hash = '#/';
        },
    });
}

/// The page of a device the fleet does not reach (or that refuses the fleet's token): nothing of
/// its PLC is known, so nothing is shown as a fact.
function offlineEmpty(ctx) {
    const refused = ctx.plc.device && ctx.plc.device.state === 'token_refused';
    return h('div', { class: 'empty' },
        h('p', { class: 'empty-title' }, refused ? `${ctx.plc.id} refuses the fleet’s token` : `${ctx.plc.id} does not answer`),
        h('p', { class: 'muted' }, refused
            ? 'The device answers, but not to the token this fleet has for it, so its programs, tags and settings cannot be shown. The PLC itself may still be running.'
            : 'The fleet cannot reach this device, so its programs, tags and settings cannot be shown. The PLC itself may still be running. This page shows it again when the device answers.'));
}

// ───────────────────────────── Header: the instrument ─────────────────────────────

// Latency is latency_us.p99: how late a cycle starts after its planned time (its variation would be jitter)
const FIGURES = ['Cycle', 'Exec', 'Load', 'Latency', 'Exceeded', 'Links', 'Forced'];

function plcHeader(ctx) {
    const compact = ctx.tab !== 'overview';
    const id = ctx.id;
    const name = h('h1', { class: 'plc-name' });
    const meta = h('p', { class: 'plc-meta' });
    const word = h('p', { class: 'state-word', 'data-tone': 'off' });
    const figures = h('dl', { class: 'figures' });
    const offlineNote = h('p', { class: 'offline-note', hidden: true });
    const facts = compact ? null : h('div', { class: 'facts' });
    const front = compact ? null : controllerFront(ctx.plc.id);
    const faultBox = h('div', { class: 'fault-detail', hidden: true });
    const restartButton = isAdmin() ? h('button', { class: 'btn', type: 'button', onclick: () => restartDialog(ctx) }, 'Restart…') : null;
    const runButton = isAdmin() ? h('button', { class: 'btn', type: 'button', onclick: () => runOrStop(ctx) }, 'Stop…') : null;

    const figs = {};
    for (const label of FIGURES) {
        const dd = h('dd', { class: 'live' });
        figs[label] = { dd, key: null };
        figures.append(h('div', { dataset: { figure: label.toLowerCase() } }, h('dt', null, label), dd));
    }
    /// Sets a figure only when it changed (a link in it keeps its focus); a change fades in.
    function setFigure(label, key, build, fade) {
        const f = figs[label];
        if (f.key === key) return;
        const first = f.key === null;
        f.key = key;
        fill(f.dd, build());
        if (fade && !first && !reducedMotion.matches) {
            f.dd.classList.add('is-changing');
            requestAnimationFrame(() => requestAnimationFrame(() => f.dd.classList.remove('is-changing')));
        }
    }

    const head = h('div', { class: `plc-head${compact ? ' is-compact' : ''}` },
        h('div', null,
            app.mode === 'fleet' ? h('p', { class: 'crumb' }, h('a', { href: '#/' }, 'Fleet')) : null,
            name, meta),
        h('div', { class: 'actions' }, restartButton, runButton),
        h('div', { class: 'instrument' },
            front ? h('figure', { class: 'device-wrap' }, front.el) : null,
            h('div', { class: 'readings' }, h('div', { class: 'state-line' }, word, figures), offlineNote, facts)),
        faultBox);
    const tabs = h('nav', { class: 'tabs', 'aria-label': 'PLC' },
        TABS.map(t => h('a', { href: `#/plc/${enc(id)}/${t}`, 'aria-current': t === ctx.tab ? 'page' : null, dataset: { tab: t } }, TAB_LABELS[t])));
    const el = h('div', { class: `plc-top${compact ? ' is-compact' : ''}` }, head, tabs);
    requestAnimationFrame(() => {
        const current = tabs.querySelector('[aria-current]');
        if (current && tabs.scrollWidth > tabs.clientWidth) tabs.scrollLeft = current.offsetLeft - 16;
    });

    let factsKey = null;
    let faultKey = null;
    let metaKey = null;

    function stateWord(plc) {
        if (ctx.offline) return ['Offline', 'off'];
        if (ctx.starting) return ['Starting', 'off'];
        if (plc.state === 'fault') return ['Fault', 'fault'];
        return plc.running ? ['Running', 'run'] : ['Stopped', 'off'];
    }

    function updateFigures(plc) {
        const dash = () => h('span', { class: 'muted' }, '–');
        const offline = ctx.offline;
        const halted = offline || plc.state === 'fault' || !plc.running;
        const t = worstTask(plc);
        const title = t && (plc.tasks || []).length > 1 ? `Task ${t.name}, the busiest of ${plc.tasks.length}` : null;
        figs.Cycle.dd.title = title || '';
        setFigure('Cycle', offline || !t ? '–' : `${t.cycle_ms}`, () => (offline || !t ? dash() : withUnit(t.cycle_ms, 'ms')));
        const exec = !halted && t ? usParts(t.exec_us && t.exec_us.avg) : null;
        setFigure('Exec', exec ? exec.join(' ') : '–', () => (exec ? withUnit(exec[0], exec[1]) : dash()), true);
        const load = !halted && t && t.exec_us ? (t.exec_us.avg / (t.cycle_ms * 1000)) * 100 : null;
        const loadText = load === null ? null : load < 10 ? load.toFixed(2) : load.toFixed(1);
        setFigure('Load', loadText || '–', () => (loadText ? withUnit(loadText, '%') : dash()), true);
        const late = !halted && t ? usParts(t.latency_us && t.latency_us.p99) : null;
        setFigure('Latency', late ? late.join(' ') : '–', () => (late ? withUnit(late[0], late[1]) : dash()), true);
        const exceeded = t ? (plc.tasks || []).reduce((n, x) => n + (x.exceeded || 0), 0) : 0;
        setFigure('Exceeded', offline ? '–' : String(exceeded), () => (offline ? dash()
            : h('span', { class: exceeded ? 'is-attn' : '' }, exceeded.toLocaleString('en-US'))));
        const links = (plc.modbus && plc.modbus.links) || [];
        const up = links.filter(l => l.ok).length;
        const down = links.filter(linkDown).length;
        setFigure('Links', offline ? '–' : `${up}/${links.length}/${down}`, () => {
            if (offline) return dash();
            if (!links.length) return h('span', { class: 'muted' }, 'None');
            return h('a', { href: `#/plc/${enc(id)}/io`, class: down ? 'is-fault' : '' }, `${up} of ${links.length}`, NBSP, h('span', { class: 'unit' }, 'up'));
        });
        const forces = plc.forces || 0;
        setFigure('Forced', offline ? '–' : String(forces), () => (offline ? dash() : forces
            ? h('a', { href: `#/plc/${enc(id)}/overview`, class: 'is-attn', onclick: e => {
                // On Overview the forced values are further down the same page
                const list = document.getElementById('forced');
                if (ctx.tab === 'overview' && list) {
                    e.preventDefault();
                    list.scrollIntoView({ block: 'start' });
                }
            } }, String(forces)) : '0'));
    }

    function meter(value, text, max, warn, bad, limit) {
        if (value === null || value === undefined || !Number.isFinite(value)) return h('span', { class: 'muted' }, '–');
        const tone = bad !== undefined && value >= bad ? ' is-fault' : value >= warn ? ' is-attn' : '';
        return h('span', { class: `meter${tone}` },
            h('span', { class: 'bar' },
                h('span', { class: 'fill', style: `width:${Math.min(100, Math.max(0, (value / max) * 100)).toFixed(1)}%` }),
                limit ? h('span', { class: 'limit', style: `left:${((limit / max) * 100).toFixed(1)}%` }) : null),
            h('span', { class: 'v' }, text));
    }

    function updateFacts(plc) {
        if (!facts) return;
        const info = ctx.info;
        const offline = ctx.offline;
        const mode = offline ? null : ctx.starting ? 'STARTUP' : plc.running ? 'RUN' : 'STOP';
        const down = ((plc.modbus && plc.modbus.links) || []).filter(linkDown);
        const forces = plc.forces || 0;
        const f = plc.fault;
        const lc = plc.last_change;
        const stoppedBy = !offline && !plc.running && !f && lc && /\bstop/i.test(lc.message || '') && lc.by
            ? `by ${byText(lc.by)}, ${formatWhen(lc.t_ms)}` : null;
        const cpu = info && info.cpu && info.cpu.load && info.cpu.cores ? (info.cpu.load[0] / info.cpu.cores) * 100 : null;
        const memTotal = info && info.memory ? (info.memory.limit_kb && info.memory.total_kb ? Math.min(info.memory.limit_kb, info.memory.total_kb) : info.memory.total_kb) : null;
        const mem = memTotal && info.memory.available_kb !== null ? ((memTotal - Math.min(info.memory.available_kb, memTotal)) / memTotal) * 100 : null;
        const temp = info && info.cpu ? info.cpu.temperature_c : null;
        const key = JSON.stringify([mode, offline, f && f.at, down.map(l => l.name), forces, stoppedBy, info && Math.round(cpu || 0), info && Math.round(mem || 0),
            temp === null ? null : Math.round(temp), info && info.system && Math.floor((info.system.uptime_s || 0) / 60), deviceIp(ctx), ctx.lastSeen && offline ? ctx.lastSeen : 0]);
        if (key === factsKey) return;
        factsKey = key;
        const forcedLine = forces ? h('span', { class: 'secondary is-attn' }, plural(forces, 'value') + ' forced') : null;
        let status;
        if (offline) {
            status = [h('span', { class: 'muted' }, 'Unknown'), h('span', { class: 'secondary' }, ctx.lastSeen ? `Last seen ${formatWhen(ctx.lastSeen)}` : 'Not seen since this page opened')];
        } else if (f) {
            status = [h('span', { class: 'is-fault' }, 'Error'), h('span', { class: 'secondary' }, [f.code, faultWhere(f)].filter(Boolean).join(', ')), forcedLine];
        } else if (down.length) {
            status = [h('span', { class: 'is-fault' }, 'Error'), h('span', { class: 'secondary' }, `Link ${down[0].name} down${down.length > 1 ? `, and ${down.length - 1} more` : ''}`), forcedLine];
        } else if (forces) {
            status = [h('span', { class: 'is-attn' }, 'Maintenance'), h('span', { class: 'secondary' }, plural(forces, 'value') + ' forced')];
        } else {
            status = 'OK';
        }
        const kv = pairs => h('dl', null, pairs.map(([k, v]) => [h('dt', null, k), h('dd', null, v ?? h('span', { class: 'muted' }, '–'))]));
        fill(facts,
            h('section', null, h('h3', null, 'Status'), kv([
                ['Operating mode', mode ? [mode, stoppedBy ? h('span', { class: 'secondary' }, stoppedBy) : null] : null],
                ['Status', status]])),
            h('section', { class: 'facts-general' }, h('h3', null, 'General'), kv([
                ['Id', plc.id],
                ['IP address', deviceIp(ctx)],
                ['Hardware', hardwareText(info)],
                ['SimplePLC', info && info.software ? info.software.simpleplc : app.mode === 'device' ? app.version : null],
                ['Device uptime', info && info.system && info.system.uptime_s !== null && info.system.uptime_s !== undefined ? formatAge(info.system.uptime_s * 1000) : null]])),
            h('section', null, h('h3', null, 'Workload'), kv([
                ['CPU', offline ? null : meter(cpu, `${Math.round(cpu ?? 0)}${NBSP}%`, 100, 80)],
                ['Memory', offline ? null : meter(mem, `${Math.round(mem ?? 0)}${NBSP}%`, 100, 90)],
                ['Temperature', offline ? null : temp === null || temp === undefined ? h('span', { class: 'muted' }, 'Not reported')
                    : meter(temp, `${Math.round(temp)}${NBSP}°C`, 85, 70, 80, 80)]])));
    }

    function updateFault(plc) {
        const f = !ctx.offline && plc.state === 'fault' ? plc.fault : null;
        const key = f ? `${f.at}|${f.code}|${f.line}` : '';
        if (key === faultKey) return;
        faultKey = key;
        faultBox.hidden = !f;
        if (!f) return clear(faultBox);
        const where = faultWhere(f);
        const showLine = f.line > 0 && faultInProgram(f);
        const config = plc.config;
        const substitute = config.tags.filter(t => substituteText(t) !== null);
        const sent = new Set(config.links.flatMap(l => l.write.map(m => m.tag)));
        const sentSubstitute = [...new Set(substitute.filter(t => sent.has(t.name)).map(t => config.links.find(l => l.write.some(m => m.tag === t.name)).name))];
        const outputs = substitute.length === 0 ? 'The program stopped. No tag has a substitute value, so every output kept its last value.'
            : `The program stopped. Outputs went to their substitute values: ${substitute.slice(0, 6).map(t => `${t.name} ${substituteText(t)}`).join(', ')}${substitute.length > 6 ? `, and ${substitute.length - 6} more` : ''}${sentSubstitute.length ? `, and ${sentSubstitute.length === 1 ? `link ${sentSubstitute[0]} wrote them to its device` : 'the links wrote them to their devices'}` : ''}.`;
        fill(faultBox,
            h('p', { class: 'message' }, h('strong', null, f.code), ` ${sentence(f.message)} in task ${f.task}${where ? `, ${where}` : ''}, cycle ${(f.cycle || 0).toLocaleString('en-US')}${f.at ? `, at ${formatTime(f.at)}` : ''}.`),
            h('p', { class: 'muted' }, `${outputs} Modbus and OPC UA keep serving.`),
            h('div', { class: 'actions' },
                showLine ? h('a', { class: 'btn btn-primary', href: `#/plc/${enc(id)}/program` }, `Show line ${f.line}`) : null,
                isAdmin() ? h('button', { class: 'btn', type: 'button', onclick: () => restartDialog(ctx, 'warm') }, 'Warm restart…') : null,
                h('a', { class: 'link', href: `#/help/reference?q=${enc(f.code)}` }, `What ${f.code} means`)));
    }

    function updateMeta(plc) {
        const info = ctx.info;
        const parts = [plc.id, deviceIp(ctx), hardwareText(info), info && info.software ? `SimplePLC ${info.software.simpleplc}` : null];
        const key = parts.join('|');
        if (key === metaKey) return;
        metaKey = key;
        fill(meta, parts.filter(Boolean).map((p, i) => h('span', { class: i === 3 ? 'hide-sm' : i === 1 ? 'num' : '' }, p)));
    }

    function updateFront() {
        if (front) front.update(lightState(ctx.plc, { offline: ctx.offline, starting: ctx.starting, info: ctx.info, bits: ctx.bits }));
    }

    function update() {
        const plc = ctx.plc;
        setText(name, plcLabel(plc));
        if (front) front.setId(plc.id);
        updateMeta(plc);
        const [text, tone] = stateWord(plc);
        setText(word, text);
        word.dataset.tone = tone;
        if (ctx.offline) document.body.dataset.state = 'offline';
        else delete document.body.dataset.state;
        updateFigures(plc);
        updateFacts(plc);
        updateFault(plc);
        updateFront();
        offlineNote.hidden = !ctx.offline;
        if (ctx.offline) {
            const who = app.mode === 'fleet' ? 'The fleet has' : 'This page has';
            setText(offlineNote, ctx.lastSeen
                ? `${who} not reached ${plc.id} since ${formatTime(ctx.lastSeen)}. The page shows what it reported then.`
                : `${who} not reached ${plc.id} since it opened.`);
        }
        // Nothing can be done to a PLC that does not answer: the buttons leave and come back with it
        const fault = plc.state === 'fault';
        if (runButton) {
            setText(runButton, plc.running ? 'Stop…' : 'Run…');
            runButton.hidden = fault || ctx.offline;
            runButton.disabled = ctx.starting;
        }
        if (restartButton) {
            restartButton.hidden = ctx.offline;
            restartButton.disabled = ctx.starting;
        }
    }

    return { el, update, updateFront };
}

// ───────────────────────────── Run, Stop, restarts, release ─────────────────────────────

async function runOrStop(ctx) {
    const plc = ctx.plc;
    if (plc.state === 'fault') return restartDialog(ctx, 'warm');
    if (plc.running) {
        if (!(await confirmStop(plc))) return;
        try {
            ctx.applyStatus(await api('POST', `/plcs/${enc(plc.id)}/stop`));
        } catch (err) {
            toast(err.message, 'error');
        }
        return;
    }
    if (!(await confirmDialog(`Run ${plcLabel(plc)}?`,
        'The programs start again from the values they had at STOP, and outputs follow the program from the first cycle.',
        `Run ${plcLabel(plc)}`, true))) return;
    await startPlc(ctx, 'run');
}

/// RUN or a restart, with the RUN/STOP light flashing until the PLC answers.
async function startPlc(ctx, how) {
    ctx.setStarting(true);
    try {
        ctx.applyStatus(await api('POST', `/plcs/${enc(ctx.plc.id)}/${how}`));
        if (how === 'restart') toast('Cold restart done');
    } catch (err) {
        toast(err.message, 'error');
    } finally {
        ctx.setStarting(false);
    }
}

/// Asks before STOP: names the PLC and the outputs that get their substitute value.
async function confirmStop(plc) {
    const config = plc.config;
    let values = {};
    try {
        values = await api('GET', `/plcs/${enc(plc.id)}/values`);
    } catch {}
    const label = plcLabel(plc);
    const substitute = config.tags.filter(t => substituteText(t) !== null);
    const sendingLinks = config.links.filter(l => l.write.some(m => substitute.some(t => t.name === m.tag)));
    const body = [];
    let lead;
    if (substitute.length === 0) {
        lead = 'The program stops. No tag has a substitute value, so every output keeps its last value.';
        body.push(h('p', { class: 'secondary' }, 'Modbus and OPC UA keep serving, and links keep polling.'));
    } else {
        lead = `The program stops. ${substitute.length === 1 ? 'This output goes to its substitute value' : `These ${substitute.length} outputs go to their substitute values`}${sendingLinks.length
            ? `, and ${sendingLinks.length === 1 ? `link ${sendingLinks[0].name} writes them to ${sendingLinks[0].host}` : 'their links write them to the devices'}` : ''}.`;
        const tagType = name => (config.tags.find(t => t.name === name) || {}).type;
        body.push(h('table', { class: 'data data-compact' },
            h('thead', null, h('tr', null, h('th', null, 'Output'), h('th', null, 'Now'), h('th', null, 'Substitute value'))),
            h('tbody', null, substitute.slice(0, 8).map(t => h('tr', null,
                h('td', { class: 'name' }, t.name),
                h('td', null, t.name in values ? formatValue(tagType(t.name), values[t.name]) : '–'),
                h('td', null, h('strong', null, substituteText(t))))))));
        if (substitute.length > 8) body.push(h('p', { class: 'secondary' }, `And ${substitute.length - 8} more.`));
        body.push(h('p', { class: 'secondary' }, 'Every other tag keeps its value. Modbus and OPC UA keep serving.'));
    }
    return dialog(`Stop ${label}?`, close => dialogForm({
        title: `Stop ${label}?`, lead, body,
        onsubmit: e => { e.preventDefault(); close(true); },
        actions: [
            h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, `Stop ${label}`)],
    }), { focus: '[data-cancel]' });
}

/// Restart: a cold restart; after a Fault also a warm one (RETAIN variables keep their values).
async function restartDialog(ctx, preset) {
    const plc = ctx.plc;
    const label = plcLabel(plc);
    const fault = plc.state === 'fault';
    let kind = fault ? (preset || 'warm') : 'cold';
    const config = plc.config;
    const forces = plc.forces || 0;
    const submit = h('button', { type: 'submit', class: 'btn btn-primary' });
    const sync = () => setText(submit, kind === 'cold' ? 'Cold restart' : 'Warm restart');
    sync();
    const choice = (value, title, text) => h('label', { class: 'choice' },
        h('input', { type: 'radio', name: 'restart', value, checked: kind === value, onchange: () => { kind = value; sync(); } }),
        h('span', null, title), h('span', { class: 'secondary' }, text));
    const coldText = `Every value starts from 0 and every variable from its initial value, RETAIN included.${forces ? ` The ${plural(forces, 'forced value')} ${forces === 1 ? 'is' : 'are'} released.` : ''} The programs load again and start.`;
    const body = [];
    if (fault) {
        body.push(h('div', { role: 'radiogroup', 'aria-label': 'Kind of restart' },
            choice('warm', 'Warm restart', 'RETAIN variables keep their values; every other variable starts from its initial value. Tags keep their values. FIRST_CYCLE() is TRUE on the first cycle.'),
            choice('cold', 'Cold restart', coldText)));
    } else {
        body.push(h('p', null, coldText));
        const substitute = config.tags.filter(t => substituteText(t) !== null);
        if (substitute.length) body.push(h('p', { class: 'secondary' }, `While the programs load, outputs go to their substitute values: ${substitute.slice(0, 6).map(t => `${t.name} ${substituteText(t)}`).join(', ')}${substitute.length > 6 ? ', …' : ''}.`));
    }
    body.push(h('p', { class: 'secondary' }, 'Links, Modbus and OPC UA keep running.'));
    const ok = await dialog('Restart', close => dialogForm({
        title: fault ? `Restart ${label}?` : `Cold restart ${label}?`,
        lead: fault ? `The program that faulted is still the downloaded one. Unless it changed, it may fault again: ${plc.fault.code} at ${faultWhere(plc.fault) || 'the same place'}.` : null,
        body,
        onsubmit: e => { e.preventDefault(); close(true); },
        actions: [h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'), submit],
    }), { focus: '[data-cancel]' });
    if (!ok) return;
    await startPlc(ctx, kind === 'cold' ? 'restart' : 'run');
}

/// Releases every force of a PLC after asking; resolves true when they were released.
async function releaseAllForces(ctx) {
    const plc = ctx.plc;
    let entries = [];
    let tags = [];
    try {
        [entries, tags] = await Promise.all([api('GET', `/plcs/${enc(plc.id)}/forces`), ctx.tags()]);
    } catch {}
    const names = [...new Set(entries.map(f => f.tag).filter(Boolean))];
    const raw = entries.filter(f => !f.tag);
    const count = names.length + raw.length || plc.forces || 0;
    const config = plc.config;
    // A forced tag the list lacks: the list could not be read (above), or the tag was created and forced
    // just now and the list of the new configuration comes with the next status. Shown as a number until then.
    const tagOf = name => tags.find(t => t.name === name) || { name, type: 'uint16' };
    const sentBy = name => config.links.find(l => l.write.some(m => m.tag === name));
    const outputs = names.filter(sentBy);
    const label = plcLabel(plc);
    const ok = await dialog('Release all forces', close => dialogForm({
        title: `Release ${count === 1 ? 'the forced value' : `${count} forced values`} of ${label}?`,
        lead: 'Each value goes back to its writer at once: the program, a link or the clients.',
        body: [
            names.length ? h('table', { class: 'data data-compact' },
                h('thead', null, h('tr', null, h('th', null, 'Tag'), h('th', null, 'Forced'), h('th', { class: 'hide-sm' }, 'Written by'))),
                h('tbody', null, names.map(name => {
                    const tag = tagOf(name);
                    return h('tr', null, h('td', { class: 'name' }, name),
                        h('td', { class: 'is-attn' }, ctx.values.has(name) ? formatValue(tag.type, ctx.values.get(name)) : '–'),
                        h('td', { class: 'hide-sm' }, writerOf(ctx, tag).label));
                }))) : null,
            raw.length ? h('p', { class: 'secondary' }, `${plural(raw.length, 'entry', 'entries')} without a tag: ${raw.map(f => addressText(f.area, f.address)).join(', ')}.`) : null,
            outputs.length ? h('p', null, h('strong', null, 'Outputs can change at once.'), ' ',
                h('span', { class: 'muted' }, `${outputs.map(n => `link ${sentBy(n).name} sends ${n} to ${sentBy(n).host}`).join(', ')}, within one poll.`)) : null],
        onsubmit: e => { e.preventDefault(); close(true); },
        actions: [
            h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, count === 1 ? 'Release 1 value' : `Release ${count} values`)],
    }), { focus: '[data-cancel]' });
    if (!ok) return false;
    try {
        await api('DELETE', `/plcs/${enc(plc.id)}/forces`);
        toast(count === 1 ? 'Released 1 value' : `Released ${count} values`);
        return true;
    } catch (err) {
        toast(err.message, 'error');
        return false;
    }
}

async function releaseOne(ctx, name) {
    try {
        await api('DELETE', `/plcs/${enc(ctx.plc.id)}/forces`, { tags: [name] });
        toast(`Released ${name}`);
    } catch (err) {
        toast(err.message, 'error');
    }
}

// ───────────────────────────── Overview ─────────────────────────────

async function tabOverview(ctx) {
    const el = h('div', { class: 'overview' });
    let shown = null;
    let current = null;
    function render() {
        if (ctx.plc.online === false) return;  // what the fleet heard last stays; its counts say nothing
        const empty = isEmptyPlc(ctx.plc);
        if (empty === shown) return;
        shown = empty;
        if (current && current.leave) current.leave();
        current = empty ? emptyHome(ctx) : programmedHome(ctx);
        fill(el, current.el);
    }
    render();
    onLeave(() => { if (current && current.leave) current.leave(); });
    return {
        el,
        status() {
            render();
            if (current.status) current.status();
        },
        values(v) {
            if (current.values) current.values(v);
        },
    };
}

/// An empty PLC: three ways forward, examples, and where to reach it.
function emptyHome(ctx) {
    const plc = ctx.plc;
    const agentBox = h('div', { class: 'result', hidden: true });
    let tokenId = null;
    let agentTimer = null;
    async function connectAgent(button) {
        button.disabled = true;
        try {
            const d = new Date();
            const name = `agent-${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}-${pad2(d.getHours())}${pad2(d.getMinutes())}`;
            const created = await api('POST', '/tokens', { name, role: 'admin' });
            tokenId = created.id;
            const command = mcpCommand(panelBase(plc), created.token);
            const copied = await copy(command);
            const seen = h('p', { class: 'secondary', role: 'status' }, 'No agent has connected yet.');
            fill(agentBox,
                h('pre', { class: 'command' }, command),
                h('p', { class: 'secondary' }, `${copied ? 'Copied. ' : ''}Paste it in a terminal on your computer. It carries a new token, ${created.name}, with administrator rights; revoke it in Settings. The token is shown only here, once.`),
                seen);
            agentBox.hidden = false;
            button.parentElement.hidden = true;
            // The token's first use: an agent connected
            agentTimer = setInterval(async () => {
                try {
                    const t = (await api('GET', '/tokens')).find(x => x.id === tokenId);
                    if (t && t.last_used) {
                        setText(seen, `An agent connected with ${t.name} at ${formatTime(t.last_used)}.`);
                        clearInterval(agentTimer);
                    }
                } catch {}
            }, 3000);
        } catch (err) {
            toast(err.message, 'error');
            button.disabled = false;
        }
    }
    const agentButton = h('button', { class: 'btn btn-primary', type: 'button', onclick: e => connectAgent(e.currentTarget) }, 'Copy command');
    const examples = h('tbody');
    api('GET', '/examples').then(list => {
        fill(examples, list.map(ex => h('tr', null,
            h('td', { class: 'nowrap' }, h('button', { class: 'link', type: 'button', disabled: !isAdmin(), onclick: () => exampleDialog(ctx, ex) }, ex.title)),
            h('td', { class: 'muted' }, ex.description))));
    }, () => {});
    const mb = modbusEndpoint(plc);
    const ua = opcuaEndpoint(plc);
    const unit = plc.config.device.unit_id;
    const el = h('div', null,
        h('div', { class: 'intro' },
            h('h2', null, 'This PLC is empty'),
            h('p', null, 'It runs, and serves Modbus TCP and OPC UA with nothing in its process image. Give it a program and I/O in one of three ways.')),
        h('div', { class: 'paths' },
            h('section', { class: 'path', dataset: { path: 'agent' } },
                h('h3', null, 'Connect an agent'),
                h('p', null, 'Claude Code or another MCP client creates the tags, the program and the links. You see each change here, with its author.'),
                isAdmin() ? h('div', { class: 'actions' }, agentButton) : h('p', { class: 'secondary' }, 'An administrator can create the token an agent needs.'),
                agentBox),
            h('section', { class: 'path', dataset: { path: 'program' } },
                h('h3', null, 'Write a program'),
                h('p', null, 'Structured Text (IEC 61131-3) in the editor. Paste it or drop .st files. Tags the program needs are offered for creation.'),
                h('div', { class: 'actions' }, h('a', { class: 'btn', href: `#/plc/${enc(plc.id)}/program` }, 'Open editor'))),
            h('section', { class: 'path', dataset: { path: 'machine' } },
                h('h3', null, 'Connect a machine'),
                h('p', null, 'A Modbus TCP remote I/O module, drive, meter or another PLC. Probe it, then choose what to read and write.'),
                h('div', { class: 'actions' }, h('a', { class: 'btn', href: `#/plc/${enc(plc.id)}/io/add` }, 'Add device')))),
        h('div', { class: 'rows rows-after-paths' },
            row('Start from an example', h('table', { class: 'plain' }, examples)),
            row('Reach this PLC', h('table', { class: 'plain' }, h('tbody', null,
                h('tr', null, h('td', { class: 'nowrap' }, 'Modbus TCP'), h('td', { class: 'num' }, mb || 'Off'),
                    h('td', { class: 'muted' }, mb ? `Unit ${unit}, addresses from 0` : 'Turn it on in Settings'),
                    h('td', null, mb ? h('button', { class: 'link', type: 'button', onclick: () => copy(mb) }, 'Copy') : null)),
                h('tr', null, h('td', { class: 'nowrap' }, 'OPC UA'), h('td', { class: 'num' }, ua || 'Off'),
                    h('td', { class: 'muted' }, ua ? `Tags under Objects, ${plc.name || plc.id}` : 'Turn it on in Settings'),
                    h('td', null, ua ? h('button', { class: 'link', type: 'button', onclick: () => copy(ua) }, 'Copy') : null)),
                h('tr', null, h('td', { class: 'nowrap' }, 'Security'), h('td', { class: 'is-attn nowrap' }, 'Not secured'),
                    h('td', { class: 'muted' }, 'Anyone on this network can read and write tags.'),
                    h('td', null, h('a', { class: 'link', href: `#/plc/${enc(plc.id)}/communication` }, 'Communication'))))))));
    return { el, leave: () => clearInterval(agentTimer) };
}

function mcpCommand(base, token) {
    return `claude mcp add --transport http simpleplc ${base}/mcp --header "Authorization: Bearer ${token}"`;
}

/// An example on an empty PLC: what it creates, then a cold start with it.
async function exampleDialog(ctx, ex) {
    let full = ex;
    try {
        full = await api('GET', `/examples/${enc(ex.name)}`);
    } catch {}
    const tasks = full.tasks || [];
    const programs = full.programs || {};
    const lines = name => (programs[name] ? programs[name].split('\n').length : null);
    const ok = await dialog(`Open the example ${ex.title}?`, close => dialogForm({
        title: `Open the example ${ex.title}?`,
        lead: 'This PLC is empty, so nothing is replaced.',
        body: [
            dl([
                ['Program', tasks.length ? tasks.map(t => `${t.program}.st${lines(t.program) ? `, ${lines(t.program)} lines,` : ''} in task ${t.name} every ${t.cycle_ms}${NBSP}ms`).join('; ') : '–'],
                ['Tags', full.tags.length ? full.tags.map(t => `${t.name} (${addressText(t.area, t.address, tagWidth(t.type))})`).join(', ') : 'None'],
                ['Links', (full.links || []).length ? full.links.map(l => `${l.name} to ${l.host}:${l.port}`).join(', ')
                    : h('span', { class: 'muted' }, 'None. It switches nothing outside this PLC.')]], { style: '--label-column:96px' }),
            h('p', { class: 'secondary' }, 'The PLC cold-starts with the example, so it runs from its initial values.')],
        onsubmit: e => { e.preventDefault(); close(true); },
        actions: [
            h('button', { type: 'button', class: 'btn', onclick: () => close(false) }, 'Cancel'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, `Open ${ex.title}`)],
    }), { wide: true });
    if (!ok) return;
    try {
        const done = await configRequest('POST', `/plcs/${enc(ctx.plc.id)}/setup`, { example: ex.name }, ctx.plc.config_version, { what: 'The configuration' });
        if (done === null) return;
        toast(`${ex.title} runs on ${plcLabel(done)}`);
        route();
    } catch (err) {
        toast(err.message, 'error');
    }
}

/// A programmed PLC: program, I/O, communication, forced values, recent events.
function programmedHome(ctx) {
    const id = ctx.plc.id;
    const programBody = h('div', { class: 'row-body-inner' });
    const ioBody = h('div');
    const commBody = h('tbody');
    const forcedBody = h('div', { class: 'row-body-inner' });
    const forcedRow = row('Forced', forcedBody,
        isAdmin() ? h('button', { class: 'btn', type: 'button', onclick: () => releaseAllForces(ctx) }, 'Release all…') : null, { id: 'forced', hidden: true });
    const eventsBody = h('tbody');
    let lastRequests = null;
    let rate = null;

    let lastProgramEvent = null;  // the newest program event: who downloaded what, when
    let programKey = null;
    function updateProgram() {
        const plc = ctx.plc;
        const tasks = plc.tasks || [];
        const f = plc.fault;
        const e = lastProgramEvent;
        const key = JSON.stringify([tasks.map(t => [t.program, t.name, t.cycle_ms, t.enabled]), f && f.at, e && e.time, loadedAt(tasks)]);
        if (key === programKey) return;
        programKey = key;
        fill(programBody,
            tasks.length ? h('p', null, tasks.map(t => `${t.program}.st in task ${t.name}, every ${t.cycle_ms}${NBSP}ms${t.enabled === false ? ' (disabled)' : ''}`).join('; '))
                : h('p', { class: 'muted' }, 'No task runs a program.'),
            f ? h('p', { class: 'secondary' }, h('span', { class: 'is-fault' }, `Stopped by ${f.code}${f.line ? ` at ${faultWhere(f)}` : ''}`), `, ${formatWhen(f.at)}.`) : null,
            e ? h('p', { class: 'secondary' }, `${sentence(e.message)}, ${formatWhen(e.time)}, by ${byText(e.by) || 'system'}.`)
                : loadedAt(tasks) ? h('p', { class: 'secondary' }, `Loaded ${formatWhen(loadedAt(tasks))}.`) : null);
    }

    function updateIo() {
        const config = ctx.config;
        const links = config.links;
        const read = new Set(links.flatMap(l => l.read.map(m => m.tag).concat(l.status_tag ? [l.status_tag] : [])));
        const written = new Set(links.flatMap(l => l.write.map(m => m.tag)));
        const substitute = config.tags.filter(t => substituteText(t) !== null).length;
        const internal = config.tags.filter(t => !read.has(t.name) && !written.has(t.name)).length;
        fill(ioBody, h('dl', { class: 'figures figures-sm' },
            [['Tags', config.tags.length], ['Read from links', read.size], ['Written to links', written.size], ['Substitute values', substitute], ['Internal', internal]]
                .map(([k, v]) => h('div', null, h('dt', null, k), h('dd', null, String(v))))));
    }

    function updateComm() {
        const plc = ctx.plc;
        const now = Date.now();
        if (plc.modbus && lastRequests) rate = Math.max(0, (plc.modbus.requests - lastRequests[0]) / ((now - lastRequests[1]) / 1000));
        if (plc.modbus) lastRequests = [plc.modbus.requests, now];
        const links = (plc.modbus && plc.modbus.links) || [];
        const configured = ctx.config.links;
        fill(commBody,
            links.map(l => {
                const c = configured.find(x => x.name === l.name) || {};
                return h('tr', null, h('td', { class: 'nowrap' }, `Link ${l.name}`), h('td', null, linkDevice(l)),
                    h('td', null, linkStateWord(l)),
                    h('td', { class: 'muted' }, linkDown(l) ? (l.last_error ? sentence(l.last_error) : `No answer within ${c.timeout_ms}${NBSP}ms`)
                        : `Every ${c.period_ms}${NBSP}ms, ${plural(l.errors || 0, 'error')}`));
            }),
            plc.modbus ? h('tr', null, h('td', { class: 'nowrap' }, 'Modbus TCP'), h('td', null, plc.modbus.enabled ? `Port ${plc.modbus.port}` : 'Off'),
                h('td', null, plural(plc.modbus.connections || 0, 'connection')),
                h('td', { class: 'muted' }, rate === null ? '' : `${Math.round(rate)} requests/s`)) : null,
            plc.opcua ? h('tr', null, h('td', { class: 'nowrap' }, 'OPC UA'), h('td', null, plc.opcua.enabled ? `Port ${plc.opcua.port}` : 'Off'),
                h('td', null, plural(plc.opcua.sessions || 0, 'session')),
                h('td', { class: 'muted' }, plural(plc.opcua.variables || 0, 'variable'))) : null);
    }

    let forcedKey = null;
    async function updateForced() {
        const plc = ctx.plc;
        const names = [...ctx.forced];
        const key = JSON.stringify([plc.forces || 0, names, names.map(n => ctx.values.get(n)), plc.config_version]);
        if (key === forcedKey) return;
        forcedKey = key;
        forcedRow.hidden = !(plc.forces || names.length);
        if (forcedRow.hidden) return clear(forcedBody);
        let tags = [];
        if (names.length) {
            try {
                tags = await ctx.tags();  // the tag list names each tag's writer
            } catch {}
            if (forcedKey !== key) return;  // the forces changed while the list was on its way
        }
        // A forced tag the list lacks: created and forced just now, and the list of the new configuration
        // comes with the next status (or the list could not be read). Shown as a number until then.
        const tagOf = n => tags.find(t => t.name === n) || { name: n, type: 'uint16' };
        fill(forcedBody, names.length ? h('table', { class: 'plain' }, h('tbody', null, names.map(n => {
            const tag = tagOf(n);
            return h('tr', { dataset: { tag: n } }, h('td', { class: 'name' }, n),
                h('td', { class: 'is-attn' }, ctx.values.has(n) ? formatValue(tag.type, ctx.values.get(n)) : '–'),
                h('td', { class: 'muted' }, writerOf(ctx, tag).label),
                h('td', null, isAdmin() ? h('button', { class: 'link', type: 'button', onclick: () => releaseOne(ctx, n) }, 'Release') : null));
        }))) : h('p', { class: 'is-attn' }, `${plural(plc.forces, 'entry', 'entries')} forced`));
    }

    async function updateEvents() {
        try {
            const events = await plcEvents(id, 30);
            lastProgramEvent = events.filter(e => e.event === 'program').pop() || null;
            updateProgram();
            fill(eventsBody, events.filter(e => e.level !== 'debug').slice(-5).reverse().map(e => h('tr', null,
                h('td', { class: 'num muted nowrap' }, formatWhen(e.time)), h('td', null, eventText(e)), h('td', { class: 'muted' }, eventBy(e)))));
            if (!eventsBody.firstChild) eventsBody.append(h('tr', null, h('td', { class: 'muted' }, 'Nothing yet.')));
        } catch {}
    }

    const el = h('div', { class: 'rows' },
        row('Program', programBody, [h('a', { class: 'link', href: `#/plc/${enc(id)}/program` }, 'Open editor'), h('a', { class: 'link', href: `#/plc/${enc(id)}/diagnostics` }, 'Diagnostics')]),
        row('I/O', ioBody, [h('a', { class: 'link', href: `#/plc/${enc(id)}/io` }, 'Tags'),
            h('button', { class: 'link', type: 'button', onclick: () => exportRegisterMap(ctx) }, 'Register map (CSV)')]),
        row('Communication', h('table', { class: 'plain' }, commBody), h('a', { class: 'link', href: `#/plc/${enc(id)}/communication` }, 'Communication')),
        forcedRow,
        row('Recent events', h('table', { class: 'plain' }, eventsBody), h('a', { class: 'link', href: `#/plc/${enc(id)}/diagnostics` }, 'Diagnostics buffer')));
    updateProgram();
    updateIo();
    updateComm();
    updateForced();
    updateEvents();
    const eventsTimer = setInterval(updateEvents, 5000);
    return {
        el,
        leave: () => clearInterval(eventsTimer),
        status() {
            updateProgram();
            updateIo();
            updateComm();
            updateForced();
        },
        values() {
            updateForced();
        },
    };
}

/// When the newest program version was loaded into a task.
function loadedAt(tasks) {
    const times = (tasks || []).map(t => t.loaded_at || 0).filter(Boolean);
    return times.length ? Math.max(...times) : null;
}

/// A link's device as it identifies itself (vendor and product from Read Device Identification), or null.
function linkIdentity(l) {
    const id = l.identity;
    return id && (id.vendor || id.product) ? [id.vendor, id.product].filter(Boolean).join(' ') : null;
}

/// A link's device: its identity, or its address when it reports none (where no address column follows).
function linkDevice(l) {
    return linkIdentity(l) || `${l.host}:${l.port}`;
}

/// The Device cell of a table whose next column is the address: the identity, or that there is none.
function linkDeviceCell(l) {
    return linkIdentity(l) || h('span', { class: 'muted' }, 'No identification');
}

function linkStateWord(l) {
    if (l.ok) return h('span', { class: 'is-run' }, 'Up');
    if (!linkDown(l)) return h('span', { class: 'muted' }, 'Starting');
    const since = l.last_success_ago_ms !== null && l.last_success_ago_ms !== undefined ? Date.now() - l.last_success_ago_ms : null;
    return h('span', { class: 'is-fault nowrap' }, since ? `Down since ${formatTime(since)}` : 'Down');
}

// ───────────────────────────── Communication ─────────────────────────────

function tabCommunication(ctx) {
    const id = ctx.plc.id;
    const linksBody = h('div', { class: 'row-body-inner' });
    const modbusBody = h('div');
    const opcuaBody = h('div');
    const panelBody = h('div');
    const commEventsBody = h('tbody');
    let lastRequests = null;
    let rate = null;

    // In a fleet, name the PLCs behind the clients' addresses: every device reports its own addresses
    let masters = new Map();
    if (app.mode === 'fleet') {
        (async () => {
            const plcs = await api('GET', '/plcs').catch(() => []);
            const infos = await Promise.all(plcs.filter(p => p.online !== false)
                .map(p => api('GET', `/plcs/${enc(p.id)}/device`).then(d => [p.id, d], () => null)));
            const owners = new Map();
            for (const [plcId, d] of infos.filter(Boolean)) {
                for (const n of d.network) for (const a of n.addresses) {
                    const ip = a.split('/')[0];
                    owners.set(ip, owners.has(ip) ? null : plcId);  // shared addresses name nobody
                }
            }
            masters = new Map([...owners].filter(([, plcId]) => plcId));
            update(ctx.plc);
        })();
    }
    const peerHost = peer => (peer.startsWith('[') ? peer.slice(1, peer.indexOf(']')) : peer.slice(0, peer.lastIndexOf(':')));
    const narrow = { style: '--label-column:112px' };

    function update(plc) {
        const now = Date.now();
        const mb = plc.modbus;
        if (lastRequests) rate = Math.max(0, (mb.requests - lastRequests[0]) / ((now - lastRequests[1]) / 1000));
        lastRequests = [mb.requests, now];
        const links = mb.links || [];
        const configured = plc.config.links;
        fill(linksBody, links.length ? h('table', { class: 'plain' }, h('tbody', null, links.map(l => {
            const c = configured.find(x => x.name === l.name) || {};
            return h('tr', null, h('td', { class: 'name' }, l.name), h('td', null, linkDeviceCell(l)),
                h('td', { class: 'num' }, `${l.host}:${l.port}, unit ${l.unit}`), h('td', null, linkStateWord(l)),
                h('td', { class: 'muted' }, linkDown(l)
                    ? `${l.last_error ? sentence(l.last_error) : `No answer within ${c.timeout_ms}${NBSP}ms`}, ${plural(l.errors || 0, 'error')}`
                    : `Every ${c.period_ms}${NBSP}ms${l.last_request_ago_ms !== null && l.last_request_ago_ms !== undefined ? `, last answer ${Math.round(l.last_success_ago_ms ?? l.last_request_ago_ms)}${NBSP}ms ago` : ''}, ${plural(l.errors || 0, 'error')}`));
        }))) : h('p', { class: 'muted' }, 'None yet. A device is a Modbus TCP server this PLC reads from and writes to: Add device probes one and creates a link and its tags.'));

        const mbEndpoint = modbusEndpoint(plc);
        const unit = plc.config.device.unit_id;
        const maxConnections = plc.config.modbus.max_connections;
        fill(modbusBody, !mb.enabled ? h('p', { class: 'muted' }, 'Off. Turn it on in Settings, Network.')
            : dl([
                ['Endpoint', mbEndpoint ? [h('span', { class: 'num' }, mbEndpoint), `, unit ${unit}, addresses from 0`] : h('span', { class: 'is-fault' }, mb.error || 'Not available')],
                mbEndpoint && mb.port !== mb.configured_port ? ['Port', h('span', { class: 'is-attn' }, `Port ${mb.configured_port} was taken, so Modbus uses port ${mb.port}.`)] : null,
                ['Clients', [
                    mb.connections ? `${mb.connections} connection${mb.connections === 1 ? '' : 's'} of ${maxConnections}${rate !== null ? `, ${Math.round(rate)} requests/s` : ''}, ${plural(mb.exceptions || 0, 'exception')}`
                        : h('span', { class: 'muted' }, `None connected${mb.rejected_connections ? `; ${plural(mb.rejected_connections, 'connection')} refused at the limit` : ''}`),
                    mb.clients.length ? h('table', { class: 'plain secondary clients' }, h('tbody', null, mb.clients.map(c => {
                        const owner = masters.get(peerHost(c.peer));
                        return h('tr', null, h('td', { class: 'num' }, c.peer, owner ? [' ', h('a', { class: 'link', href: `#/plc/${enc(owner)}/communication` }, owner)] : null),
                            h('td', null, `since ${formatTime(now - c.connected_ms)}`), h('td', { class: 'num' }, plural(c.requests, 'request')));
                    }))) : null]],
                ['Security', [h('span', { class: 'is-attn' }, 'None.'), ' ', h('span', { class: 'muted' },
                    `Modbus TCP has no sign-in and no encryption: anyone who reaches port ${mb.port} can write Coils and Holding registers.`)]],
            ], narrow));

        const ua = plc.opcua;
        const uaEndpoint = opcuaEndpoint(plc);
        fill(opcuaBody, !ua.enabled ? h('p', { class: 'muted' }, 'Off. Turn it on in Settings, Network.')
            : dl([
                ['Endpoint', uaEndpoint ? h('span', { class: 'num' }, uaEndpoint) : h('span', { class: 'is-fault' }, ua.error || 'Not available')],
                ['Clients', ua.sessions ? plural(ua.sessions, 'session') : h('span', { class: 'muted' }, 'No sessions')],
                ['Variables', [ua.variables ? `${ua.variables}, one per tag. ` : 'None yet. ', h('span', { class: 'muted' },
                    `Node id ns=1;s=<tag>, under Objects, ${plc.name || plc.id}. A stale value has status Uncertain.`)]],
                ['Security', [h('span', { class: 'is-attn' }, 'None.'), ' ', h('span', { class: 'muted' },
                    `Security policy None with anonymous sign-in: messages are neither signed nor encrypted, and any client can write tags. SimplePLC does not offer OPC UA security yet; keep port ${ua.port} on the machine network.`)]],
            ], narrow));

        const base = panelBase(plc);
        const tls = base.startsWith('https:');
        fill(panelBody, dl([
            ['Endpoint', [h('span', { class: 'num' }, base), ' ', h('span', { class: 'muted' }, 'Panel, REST at /api/v1, MCP at /mcp')]],
            ['Clients', `${app.user.name} in this browser${app.mode === 'fleet' ? ', through the fleet' : ''}`],
            ['Security', tls ? 'TLS.' : [h('span', { class: 'is-attn' }, 'No TLS.'), ' ', h('span', { class: 'muted' }, 'Every change needs a password or token, but both cross the network unencrypted.')]],
        ], narrow));
    }

    async function updateCommEvents() {
        try {
            const events = await plcEvents(id, 200);
            const comm = events.filter(e => eventGroup(e) === 'communication').slice(-5).reverse();
            fill(commEventsBody, comm.length ? comm.map(e => h('tr', null,
                h('td', { class: 'num muted nowrap' }, formatWhen(e.time)),
                h('td', null, eventText(e), h('span', { class: 'secondary only-sm' }, byText(e.by) || 'system')),
                h('td', { class: 'hide-sm' }, eventBy(e))))
                : h('tr', null, h('td', { colspan: 3, class: 'muted' }, 'No communication events yet.')));
        } catch {}
    }

    const el = h('div', { class: 'comm-page' },
        h('div', { class: 'rows' },
            row(['Links', 'This PLC as Modbus TCP client'], linksBody, [
                h('a', { class: 'link', href: `#/plc/${enc(id)}/io` }, 'Devices'),
                isAdmin() ? h('a', { class: 'link', href: `#/plc/${enc(id)}/io/add` }, 'Add device') : null]),
            row('Modbus TCP server', modbusBody, h('button', { class: 'link', type: 'button', onclick: () => { const e = modbusEndpoint(ctx.plc); if (e) copy(e); } }, 'Copy endpoint')),
            row('OPC UA server', opcuaBody, h('button', { class: 'link', type: 'button', onclick: () => { const e = opcuaEndpoint(ctx.plc); if (e) copy(e); } }, 'Copy endpoint')),
            row('Panel and agents', panelBody)),
        section('Communication events', h('a', { class: 'btn', href: `#/plc/${enc(id)}/diagnostics` }, 'Diagnostics buffer'),
            h('div', { class: 'table-wrap' }, h('table', { class: 'data' },
                h('thead', null, h('tr', null, h('th', { class: 'shrink' }, 'Time'), h('th', null, 'Event'), h('th', { class: 'hide-sm shrink' }, 'By'))), commEventsBody)),
            h('p', { class: 'section-note' }, 'The communication events of the diagnostics buffer. Connections are listed above as they are now; they do not enter the buffer.')));
    update(ctx.plc);
    updateCommEvents();
    const timer = setInterval(updateCommEvents, 5000);
    onLeave(() => clearInterval(timer));
    return { el, status: update };
}

// ───────────────────────────── Diagnostics ─────────────────────────────

async function tabDiagnostics(ctx) {
    const id = ctx.plc.id;
    let group = 'all';
    const groups = [['all', 'All'], ['faults', 'Faults'], ['changes', 'Changes'], ['communication', 'Communication'], ['access', 'Access']];
    const seg = segmented(groups, group, v => { group = v; select.value = v; render(); }, 'Show');
    seg.classList.add('hide-sm');
    const select = h('select', { class: 'select only-sm-inline', 'aria-label': 'Show' }, groups.map(([v, l]) => h('option', { value: v }, v === 'all' ? 'All events' : l)));
    select.addEventListener('change', () => { group = select.value; seg.set(group); render(); });
    const search = h('input', { class: 'input w-md hide-sm', type: 'search', placeholder: 'Search', 'aria-label': 'Search events' });
    search.addEventListener('input', () => render());
    const body = h('tbody');
    const timingBody = h('tbody');
    const clientsBody = h('tbody');
    const deviceBox = h('div');
    let events = [];

    function visible(e) {
        if (group !== 'all' && eventGroup(e) !== group) return false;
        const q = search.value.trim().toLowerCase();
        return !q || `${e.message} ${byText(e.by)}`.toLowerCase().includes(q);
    }
    let showAll = false;
    const more = h('button', { class: 'link', type: 'button', onclick: () => { showAll = !showAll; render(); } });
    const moreLine = h('p', { class: 'secondary' }, more);
    function render() {
        const shown = events.filter(visible).slice().reverse();
        const limit = showAll ? 2000 : 50;
        fill(body, shown.length ? shown.slice(0, limit).map(e => h('tr', { 'data-seq': e.seq ?? '' },
            h('td', { class: 'num muted nowrap' }, formatWhen(e.time)),
            h('td', null, eventText(e), h('span', { class: 'secondary only-sm' }, byText(e.by) || 'system')),
            h('td', { class: 'hide-sm' }, eventBy(e))))
            : h('tr', null, h('td', { colspan: 3, class: 'muted' }, events.length ? 'No event matches.' : 'No events yet.')));
        moreLine.hidden = shown.length <= 50;
        setText(more, showAll ? 'Show the newest 50' : `Show all ${shown.length}`);
    }
    async function load() {
        try {
            events = await plcEvents(id, 500);
            render();
        } catch (err) {
            fill(body, h('tr', null, h('td', { colspan: 3, class: 'problems' }, err.message)));
        }
    }
    function exportBuffer() {
        const lines = [csvLine(['time', 'level', 'group', 'event', 'message', 'by'])];
        for (const e of events) lines.push(csvLine([new Date(e.time).toISOString(), e.level, eventGroup(e), e.event, e.message, byText(e.by) || 'system']));
        download(`${id}-diagnostics.csv`, lines.join('\r\n') + '\r\n');
    }

    /// Who talks to this PLC now: Modbus TCP connections, OPC UA sessions, and the links it polls
    function updateClients(plc) {
        const now = Date.now();
        const mb = plc.modbus || { clients: [] };
        const rows = [
            ...(mb.clients || []).map(c => h('tr', null, h('td', null, 'Modbus TCP'), h('td', { class: 'num' }, c.peer),
                h('td', { class: 'num' }, `since ${formatTime(now - c.connected_ms)}`), h('td', { class: 'num r' }, plural(c.requests, 'request')))),
            plc.opcua && plc.opcua.sessions ? h('tr', null, h('td', null, 'OPC UA'), h('td', null, plural(plc.opcua.sessions, 'session')), h('td'), h('td')) : null,
            ...((mb.links || []).map(l => h('tr', null, h('td', null, `Link ${l.name}`), h('td', { class: 'num' }, `${l.host}:${l.port}`),
                h('td', null, linkStateWord(l)), h('td', { class: 'num r' }, plural(l.requests || 0, 'request'))))),
        ].filter(Boolean);
        fill(clientsBody, rows.length ? rows : h('tr', null, h('td', { colspan: 4, class: 'muted' }, 'No client is connected and this PLC polls no device.')));
    }

    function updateTiming(plc) {
        updateClients(plc);
        fill(timingBody, (plc.tasks || []).map(t => {
            const halted = !plc.running || plc.state === 'fault';
            const dash = h('span', { class: 'muted' }, '–');
            const load = !halted && t.exec_us ? (t.exec_us.avg / (t.cycle_ms * 1000)) * 100 : null;
            return h('tr', null,
                h('td', { class: 'name' }, t.name, h('span', { class: 'secondary' }, `${t.program}.st${t.enabled === false ? ', disabled' : ''}`)),
                h('td', { class: 'num r' }, `${t.cycle_ms}${NBSP}ms`),
                h('td', { class: 'num r' }, halted ? dash : formatUs(t.exec_us && t.exec_us.avg)),
                h('td', { class: 'num r' }, formatUs(t.exec_us && t.exec_us.max)),
                h('td', { class: 'num r' }, formatUs(t.latency_us && t.latency_us.p99)),
                h('td', { class: `num r${t.exceeded ? ' is-attn' : ''}` }, (t.exceeded || 0).toLocaleString('en-US')),
                h('td', { class: 'num r' }, load === null ? dash : `${load < 10 ? load.toFixed(2) : load.toFixed(1)}${NBSP}%`),
                h('td', { class: 'num r hide-sm' }, (t.cycles || 0).toLocaleString('en-US')),
                h('td', { class: 'num r hide-sm' }, t.memory ? `${t.memory.data_bytes}${NBSP}B, ${t.memory.instructions}` : '–'));
        }));
    }

    function updateDevice(d) {
        if (!d) return;
        const dev = d.device, sys = d.system, cpu = d.cpu, mem = d.memory, proc = d.process, sw = d.software;
        const memTotal = mem.limit_kb && mem.total_kb ? Math.min(mem.limit_kb, mem.total_kb) : mem.total_kb;
        const clockSkew = sys.time_ms ? Math.round((sys.time_ms - Date.now()) / 1000) : 0;
        const busy = cpu.load && cpu.cores ? Math.round((cpu.load[0] / cpu.cores) * 100) : null;
        fill(deviceBox, dl([
            ['Host name', dev.hostname],
            ['Model', dev.model],
            ['Runs in', dev.container ? `A ${dev.container} container` : 'The operating system directly'],
            ['CPU', `${cpu.model ? `${cpu.model}, ` : ''}${cpu.cores} cores${busy !== null ? `, ${busy}${NBSP}% busy` : ''}`],
            ['Load', cpu.load ? `${cpu.load.map(x => x.toFixed(2)).join(', ')} (1, 5 and 15 minutes)` : null],
            ['Temperature', cpu.temperature_c !== null && cpu.temperature_c !== undefined
                ? [h('span', { class: cpu.temperature_c >= 80 ? 'is-fault' : cpu.temperature_c >= 70 ? 'is-attn' : '' }, `${cpu.temperature_c.toFixed(1)}${NBSP}°C`), ' ', h('span', { class: 'muted' }, '(throttles at 80 °C)')]
                : h('span', { class: 'muted' }, 'Not reported')],
            ['Memory', memTotal && mem.available_kb !== null ? `${formatKb(memTotal - Math.min(mem.available_kb, memTotal))} used of ${formatKb(memTotal)}${mem.limit_kb ? ' (container limit)' : ''}` : formatKb(memTotal)],
            ['Storage', d.storage.total_kb ? `${formatKb(d.storage.total_kb - d.storage.free_kb)} used of ${formatKb(d.storage.total_kb)}` : null],
            ['Uptime', sys.uptime_s !== null && sys.uptime_s !== undefined ? formatAge(sys.uptime_s * 1000) : null],
            ['System', [sys.os, sys.kernel, sys.arch].filter(Boolean).join(', ')],
            ['Network', d.network.length ? h('table', { class: 'plain' }, h('tbody', null, d.network.map(n => h('tr', null,
                h('td', null, n.name), h('td', { class: 'num' }, n.addresses.join(', ') || '–'),
                h('td', { class: n.up ? 'is-run' : 'is-off' }, n.up ? 'Up' : 'Down'))))) : h('span', { class: 'muted' }, 'No interfaces reported')],
            ['Clock', [formatDateTime(sys.time_ms), Math.abs(clockSkew) > 5 ? [' ', h('span', { class: 'is-attn' }, `${clockSkew > 0 ? '+' : ''}${clockSkew}${NBSP}s from this browser`)] : null]],
            ['Real-time', ctx.plc.realtime ? 'On: tasks with a priority run with real-time scheduling' : 'Off'],
            ['SimplePLC', `${sw.simpleplc}, running for ${formatAge(proc.uptime_s * 1000)}, ${formatKb(proc.rss_kb)} of memory`],
            ['Components', `${sw.structured_text}, open62541 ${sw.open62541}`],
            ['Data folder', h('code', null, d.storage.path)],
        ]));
    }

    const el = h('div', { class: 'diagnostics-page' },
        section('Diagnostics buffer', [seg, select, search, h('button', { class: 'btn hide-sm', type: 'button', onclick: exportBuffer }, 'Export')],
            h('div', { class: 'table-wrap' }, h('table', { class: 'data', id: 'diagnostics-buffer' },
                h('thead', null, h('tr', null, h('th', { class: 'shrink' }, 'Time'), h('th', null, 'Event'), h('th', { class: 'hide-sm shrink' }, 'By'))), body)),
            moreLine,
            h('p', { class: 'section-note' }, 'What happened, newest first, with who did it: a user, a token or the system. Debug details appear when SimplePLC is started with -v.')),
        section('Timing', null,
            h('div', { class: 'table-wrap' }, h('table', { class: 'data' },
                h('thead', null, h('tr', null, h('th', null, 'Task'), h('th', { class: 'r' }, 'Cycle'), h('th', { class: 'r' }, 'Exec'), h('th', { class: 'r' }, 'Exec max'),
                    h('th', { class: 'r' }, 'Latency p99'), h('th', { class: 'r' }, 'Exceeded'), h('th', { class: 'r' }, 'Load'), h('th', { class: 'r hide-sm' }, 'Cycles'),
                    h('th', { class: 'r hide-sm' }, 'Data, code'))), timingBody)),
            h('p', { class: 'section-note' }, 'Latency is how late a cycle starts after its planned time; p99: 99 of 100 cycles start no later than this. Exceeded counts the cycles still running when the next should have started; that next cycle is skipped. Load is execution time as a share of the cycle.')),
        section('Clients', h('a', { class: 'btn', href: `#/plc/${enc(id)}/communication` }, 'Communication'),
            h('div', { class: 'table-wrap' }, h('table', { class: 'data', id: 'clients' },
                h('thead', null, h('tr', null, h('th', null, 'Kind'), h('th', null, 'Peer'), h('th', null, 'State'), h('th', { class: 'r' }, 'Requests'))), clientsBody)),
            h('p', { class: 'section-note' }, 'As they are now, every second. Connections do not enter the diagnostics buffer.')),
        section('Device', null, deviceBox));
    updateTiming(ctx.plc);
    await load();
    if (ctx.seq !== app.routeSeq) return null;  // the page moved on while the buffer was on its way
    // A new record of this PLC on the log stream: read the buffer again (its answer has who did it);
    // the PLC summaries every 2 s show the stream is alive
    live('/events', {
        logs: records => {
            if (records.some(r => !r.plc || r.plc === id)) load();
        },
        plcs: () => {},
    }, 2000);
    return { el, status: updateTiming, info: updateDevice };
}

/// A segmented control: one choice of a few; the chosen one is inverted ink.
function segmented(options, current, onChange, label) {
    const el = h('div', { class: 'seg', role: 'group', 'aria-label': label });
    const buttons = options.map(([value, text]) => h('button', {
        type: 'button', 'aria-pressed': String(value === current), dataset: { value },
        onclick: () => { el.set(value); onChange(value); },
    }, text));
    append(el, buttons);
    el.set = value => buttons.forEach(b => b.setAttribute('aria-pressed', String(b.dataset.value === value)));
    return el;
}

// ───────────────────────────── Settings ─────────────────────────────

/// What a settings change does to the tasks. The PLC stops and restarts tasks when the task list,
/// a task's name, program, enabled, priority or watchdog, or real-time scheduling changes (only the
/// interval changes in place). Returns the affected tasks with the reason, or [].
function taskRestarts(before, after) {
    const old = new Map(before.tasks.map(t => [t.name, t]));
    const out = [];
    for (const t of after.tasks) {
        const was = old.get(t.name);
        if (!was) out.push([t.name, 'new: its program starts from initial values']);
        else if (was.program !== t.program) out.push([t.name, `switches to program ${t.program}: it starts from initial values`]);
        else if (was.enabled && !t.enabled) out.push([t.name, 'disabled: it stops']);
    }
    for (const t of before.tasks) if (!after.tasks.some(x => x.name === t.name)) out.push([t.name, 'removed: it stops']);
    return out;
}

function tabSettings(ctx) {
    const config = ctx.config;
    const version = ctx.plc.config_version;  // the settings shown come from this configuration: sent as If-Match
    const readOnly = !isAdmin();
    const problems = h('div', { class: 'problems', role: 'alert' });
    const changedNote = h('div', { class: 'problem is-block', role: 'status', hidden: true });
    const input = (key, value, attrs = {}) => h('input', {
        class: `input ${attrs.width || ''}`, id: `st-${key}`, name: key, type: attrs.type || 'text', value: value ?? '',
        disabled: readOnly, min: attrs.min, max: attrs.max, 'aria-label': attrs.label,
    });
    const fld = (key, label, value, attrs = {}) => h('div', { class: 'field', style: attrs.style },
        h('label', { for: `st-${key}` }, label), input(key, value, attrs), attrs.hint ? h('p', { class: 'hint' }, attrs.hint) : null);
    const check = (key, label, checked) => h('label', { class: 'check' }, h('input', { type: 'checkbox', id: `st-${key}`, name: key, checked, disabled: readOnly }), label);
    const value = key => document.getElementById(`st-${key}`);

    const taskRows = h('tbody');
    // The cycle times offered; one set through the API (the API takes 1-60000 ms) stays a choice, so saving keeps it
    const CYCLE_CHOICES = [1, 10, 20, 50, 100];
    function cycleSelect(ms) {
        const choices = CYCLE_CHOICES.includes(ms) ? CYCLE_CHOICES : [...CYCLE_CHOICES, ms].sort((a, b) => a - b);
        const select = h('select', { class: 'select', 'aria-label': 'Cycle time', disabled: readOnly, dataset: { k: 'cycle_ms' } },
            choices.map(v => h('option', { value: String(v) }, `${v}${NBSP}ms`)));
        select.value = String(ms);
        return select;
    }
    function taskRow(t) {
        const r = h('tr', null,
            h('td', null, h('input', { class: 'input w-sm', type: 'text', value: t.name, 'aria-label': 'Task name', disabled: readOnly, dataset: { k: 'name' } })),
            h('td', null, h('input', { class: 'input w-sm', type: 'text', value: t.program, 'aria-label': 'Program', disabled: readOnly, dataset: { k: 'program' } })),
            h('td', null, cycleSelect(t.cycle_ms)),
            h('td', { class: 'hide-sm' }, h('input', { class: 'input w-xs', type: 'number', min: 0, max: 98, value: t.priority || 0, 'aria-label': 'Real-time priority (0: normal)', disabled: readOnly, dataset: { k: 'priority' } })),
            h('td', null, h('input', { type: 'checkbox', checked: t.enabled !== false, 'aria-label': 'Enabled', disabled: readOnly, dataset: { k: 'enabled' } })),
            h('td', { class: 'r' }, readOnly ? null : h('button', { type: 'button', class: 'link', onclick: () => r.remove() }, 'Remove')));
        r.dataset.watchdog = t.watchdog_ms || 0;  // not edited here; kept as it is
        return r;
    }
    for (const t of config.tasks) taskRows.append(taskRow(t));

    async function save() {
        const num = k => Number(value(k).value);
        const tasks = [...taskRows.querySelectorAll('tr')].map(r => {
            const get = k => r.querySelector(`[data-k="${k}"]`);
            return {
                name: get('name').value.trim(), program: get('program').value.trim(), cycle_ms: Number(get('cycle_ms').value),
                priority: Number(get('priority').value), watchdog_ms: Number(r.dataset.watchdog || 0), enabled: get('enabled').checked,
            };
        });
        for (const t of tasks) {
            if (!Number.isFinite(t.cycle_ms) || !Number.isFinite(t.priority)) {
                showProblems(problems, new Error(`Task ${t.name || '(no name)'}: give the cycle time and the priority as numbers`));
                return;
            }
        }
        const patch = {
            id: value('id').value.trim(), name: value('name').value.trim(), description: value('description').value.trim(),
            autostart: value('autostart').checked, image_size: num('image_size'), realtime: value('realtime').checked,
            modbus: { enabled: value('mb_enabled').checked, listen: value('mb_listen').value.trim(), port: num('mb_port'), max_connections: num('mb_max'), idle_timeout_s: num('mb_idle') },
            opcua: { enabled: value('ua_enabled').checked, listen: value('ua_listen').value.trim(), port: num('ua_port') },
            device: { unit_id: num('unit_id'), vendor: value('vendor').value.trim(), product_code: value('product_code').value.trim(), model: value('model').value.trim() },
            tasks,
        };
        if (patch.image_size !== config.image_size) {
            if (!(await confirmDialog(`Change the process image of ${plcLabel(ctx.plc)}?`,
                `Changing the image size from ${config.image_size} to ${patch.image_size} entries restarts the PLC and resets every value to 0.`, 'Save and restart', true))) return;
        } else {
            // Tasks restart (STOP, then RUN) for these changes: say which, and what that does
            const restarts = taskRestarts({ ...config, tasks: config.tasks }, { ...patch, tasks });
            if (restarts.length && !(await confirmDialog('Restart tasks?', [
                h('p', null, 'Saving changes these tasks:'),
                h('ul', { class: 'list' }, restarts.map(([n, why]) => h('li', null, h('strong', null, n), ` (${why})`))),
                h('p', { class: 'secondary' }, 'The other tasks keep running with their values. A task that stops leaves the outputs it wrote at their last values.'),
            ], 'Save and change tasks', true))) return;
        }
        // A task naming a program that does not exist yet starts it from the empty template, but only when asked:
        // a typo must not replace the control logic with an empty program
        try {
            const existing = new Set((await api('GET', `/plcs/${enc(ctx.plc.id)}/programs`)).map(p => String(p.name).toLowerCase()));
            const missing = [...new Set(tasks.map(t => t.program).filter(p => p && !existing.has(p.toLowerCase())))];
            if (missing.length) {
                if (!(await confirmDialog(missing.length === 1 ? 'Create the program?' : 'Create the programs?', [
                    h('p', null, missing.length === 1 ? `There is no program named ${missing[0]}.` : `There are no programs named ${missing.join(', ')}.`),
                    h('p', { class: 'secondary' }, 'Create them from the empty template? Check the spelling first: the task would run an empty program.'),
                ], missing.length === 1 ? 'Create the program' : 'Create the programs'))) return;
                patch.create = true;
            }
        } catch (err) {
            showProblems(problems, err);
            return;
        }
        try {
            const saved = await configRequest('PATCH', `/plcs/${enc(ctx.plc.id)}`, patch, version,
                { what: 'The settings', onReload: () => route() });
            if (saved === null) return;
            showProblems(problems, null);
            const restarted = saved.restarted_tasks || [];
            toast(restarted.length ? `Settings saved; tasks restarted: ${restarted.join(', ')}` : 'Settings saved; no task restarted');
            if (saved.id !== ctx.plc.id) {
                app.plcId = saved.id;
                location.hash = `#/plc/${enc(saved.id)}/settings`;
            } else {
                route();
            }
        } catch (err) {
            showProblems(problems, err);
            problems.scrollIntoView({ block: 'nearest' });
        }
    }

    const kiosk = `${location.protocol}//${location.host}/?field`;
    const configRows = h('div', { class: 'rows' },
        row('Identity', [
            h('div', { class: 'fields' },
                fld('name', 'Name', config.name, { width: 'w-md' }),
                fld('id', 'Id', config.id, { width: 'w-sm' }),
                fld('description', 'Description', config.description, { style: 'flex:1 1 280px' })),
            h('p', { class: 'secondary' }, 'The id names this PLC to the fleet, in OPC UA and in agents’ tools: lowercase letters, digits, - and _, starting with a letter, at most 32 characters. Changing it reconnects their clients.')]),
        row('Tasks', [
            h('div', { class: 'table-wrap' }, h('table', { class: 'data data-compact', id: 'st-tasks' },
                h('thead', null, h('tr', null, h('th', null, 'Task'), h('th', null, 'Program'), h('th', null, 'Cycle time'), h('th', { class: 'hide-sm' }, 'Priority'), h('th', null, 'Enabled'), h('th'))),
                taskRows)),
            h('p', { class: 'secondary' }, 'Each task runs one program on its own cycle. A new program name creates an empty program, after asking. ',
                readOnly ? null : h('button', { type: 'button', class: 'link', onclick: () => {
                    const n = taskRows.childElementCount + 1;
                    taskRows.append(taskRow({ name: `task${n}`, program: `task${n}`, cycle_ms: 10, enabled: true }));
                } }, 'Add task'))]),
        row('Network', [
            h('div', { class: 'settings-group' },
                h('h3', null, 'Modbus TCP'), check('mb_enabled', 'Modbus TCP server', config.modbus.enabled),
                h('div', { class: 'fields' },
                    fld('mb_listen', 'Listen on', config.modbus.listen, { width: 'w-sm' }),
                    fld('mb_port', 'Port', config.modbus.port, { type: 'number', min: 1, max: 65535, width: 'w-xs' }),
                    fld('unit_id', 'Unit', config.device.unit_id, { type: 'number', min: 0, max: 255, width: 'w-xs' }),
                    fld('mb_max', 'Connections', config.modbus.max_connections, { type: 'number', min: 1, max: 1000, width: 'w-xs' }),
                    fld('mb_idle', 'Idle timeout (s)', config.modbus.idle_timeout_s, { type: 'number', min: 0, max: 86400, width: 'w-xs' }))),
            h('div', { class: 'settings-group' },
                h('h3', null, 'OPC UA'), check('ua_enabled', 'OPC UA server', config.opcua.enabled),
                h('div', { class: 'fields' },
                    fld('ua_listen', 'Listen on', config.opcua.listen, { width: 'w-sm' }),
                    fld('ua_port', 'Port', config.opcua.port, { type: 'number', min: 1, max: 65535, width: 'w-xs' }))),
            h('p', { class: 'secondary' }, 'Listen on 0.0.0.0 to be reached on every interface, or on one address of this device.'),
            h('p', null, h('span', { class: 'is-attn' }, 'Not secured.'), ' ', h('span', { class: 'muted' },
                'Modbus TCP has no security of its own, and OPC UA offers only security policy None with anonymous sign-in. Keep these ports on the machine network.'), ' ',
            h('a', { class: 'link', href: `#/plc/${enc(ctx.plc.id)}/communication` }, 'Communication')),
            h('p', { class: 'secondary' }, 'For a touchscreen at the machine, open ', h('span', { class: 'num ink' }, kiosk),
                ' in kiosk mode: larger type and targets, and the state stays on screen.')]),
        row('Advanced', dl([
            ['Process image', h('div', { class: 'inline-fields' }, h('label', null, input('image_size', config.image_size, { type: 'number', min: 1, max: 65536, width: 'w-sm', label: 'Entries per area' }), 'entries in each area'))],
            ['Start', check('autostart', 'Run the program when SimplePLC starts', config.autostart)],
            ['Real-time', check('realtime', 'Real-time scheduling for tasks with a priority (Linux: SCHED_FIFO, needs CAP_SYS_NICE; macOS: the time-constraint policy)', config.realtime)],
            ['Identity', [h('div', { class: 'fields' },
                fld('vendor', 'Vendor', config.device.vendor, { width: 'w-sm' }),
                fld('product_code', 'Product code', config.device.product_code, { width: 'w-sm' }),
                fld('model', 'Model', config.device.model, { width: 'w-sm' })),
            h('p', { class: 'hint' }, 'What Modbus functions 17 and 43/14 report.')]],
            ['Links as JSON', [h('span', { class: 'muted' }, 'Every link in one document, for bulk changes. '),
                isAdmin() ? h('button', { class: 'link', type: 'button', onclick: () => linksDialog(ctx) }, 'Edit…') : null]],
            ['Start over', [h('span', { class: 'muted' }, 'Replaces the program, tags and links. Users and tokens stay. '),
                isAdmin() ? h('button', { class: 'link', type: 'button', onclick: () => startOverDialog(ctx) }, 'Start over…') : null]],
        ], { class: 'dl dl-baseline' })));

    const el = h('div', { class: 'settings-page' },
        changedNote,
        configRows,
        readOnly ? null : h('div', { class: 'save-bar' }, problems,
            h('div', { class: 'actions' }, h('button', { type: 'button', class: 'btn btn-primary', onclick: save }, 'Save settings'))),
        h('div', { class: 'rows rows-separate' },
            displayRow(),
            usersRow(),
            pairingRow()));

    // A PLC still named after its first-run id follows a new id
    const idInput = el.querySelector('#st-id');
    const nameInput = el.querySelector('#st-name');
    let lastId = config.id;
    idInput.addEventListener('input', () => {
        if (nameInput.value.trim() === lastId) nameInput.value = idInput.value.trim();
        lastId = idInput.value.trim();
    });
    // Enter in a field saves, as a form would
    el.addEventListener('keydown', e => {
        if (e.key === 'Enter' && !readOnly && e.target.matches('.settings-page .input') && configRows.contains(e.target)) {
            e.preventDefault();
            save();
        }
    });

    return {
        el,
        status(plc) {
            // Someone else saved settings while this page is open: say so before a save overwrites them
            const stale = plc.config_version !== version;
            if (stale === !changedNote.hidden) return;
            changedNote.hidden = !stale;
            if (stale) {
                fill(changedNote, h('p', null, 'The configuration was changed on the PLC (by another browser, an agent or the API) after this page was opened. ',
                    h('button', { type: 'button', class: 'link', onclick: () => route() }, 'Reload the settings')));
            }
        },
    };
}

/// This browser's display: the field display and the theme. Kept in this browser only.
function displayRow() {
    const fromUrl = display.fromUrl;
    return row(['Display', 'This browser'], [
        h('div', { class: 'field' }, h('span', { class: 'label' }, 'Layout'),
            segmented([['standard', 'Standard'], ['field', 'Field display']], display.field ? 'field' : 'standard', v => display.setField(v === 'field'), 'Layout')),
        h('p', { class: 'secondary' }, fromUrl ? 'This page was opened with ?field, so it shows the field display whatever is chosen here.'
            : 'The field display is for a 10-inch touchscreen in the cabinet door: larger type, 48 px targets, a keypad for numbers, and the state kept on screen.'),
        h('div', { class: 'field' }, h('span', { class: 'label' }, 'Theme'),
            segmented([['auto', 'Like the system'], ['light', 'Light'], ['dark', 'Dark']], display.theme, v => display.setTheme(v), 'Theme')),
    ]);
}

/// Users, tokens and your own password. An administrator manages them; a viewer changes their password.
/// Untitled on the fleet's access page, whose title names it.
function usersRow(titled = true) {
    const passwordProblems = h('div', { class: 'problems', role: 'alert' });
    const current = h('input', { class: 'input w-sm', id: 'pw-current', type: 'password', autocomplete: 'current-password', 'aria-label': 'Current password' });
    const next = h('input', { class: 'input w-md', id: 'pw-new', type: 'password', autocomplete: 'new-password', minlength: 8, 'aria-label': 'New password', placeholder: '8 characters or more' });
    const passwordForm = h('form', {
        class: 'fields password-form',
        onsubmit: async e => {
            e.preventDefault();
            try {
                await api('PUT', `/users/${enc(app.user.name)}/password`, { current_password: current.value, password: next.value });
                current.value = '';
                next.value = '';
                showProblems(passwordProblems, null);
                toast('Password changed');
            } catch (err) {
                showProblems(passwordProblems, err);
            }
        },
    },
    h('div', { class: 'field' }, h('label', { for: 'pw-current' }, 'Current password'), current),
    h('div', { class: 'field' }, h('label', { for: 'pw-new' }, 'New password'), next),
    h('button', { type: 'submit', class: 'btn' }, 'Change password'));
    const password = h('div', { class: 'settings-group' }, h('h3', null, `Your password (${app.user.name})`), passwordProblems, passwordForm);
    if (!isAdmin()) return row(titled ? 'Your account' : null, password);

    const users = h('div');
    const tokens = h('div');
    async function renderUsers() {
        const list = await api('GET', '/users');
        fill(users, h('div', { class: 'table-wrap' }, h('table', { class: 'data data-compact', id: 'users' },
            h('thead', null, h('tr', null, h('th', null, 'User'), h('th', null, 'Role'), h('th', { class: 'hide-sm' }, 'Created'), h('th'))),
            h('tbody', null, list.map(u => h('tr', null,
                h('td', { class: 'name' }, u.name),
                h('td', null, u.role === 'admin' ? 'Administrator' : 'Viewer'),
                h('td', { class: 'hide-sm' }, formatWhen(u.created)),
                h('td', { class: 'r' }, u.name === app.user.name ? h('span', { class: 'muted' }, 'You') : h('button', {
                    class: 'link', type: 'button', onclick: async () => {
                        if (!(await confirmDialog(`Remove ${u.name}?`, `${u.name} is signed out at once, and the tokens ${u.name} created stop working.`, `Remove ${u.name}`, true))) return;
                        await api('DELETE', `/users/${enc(u.name)}`).catch(err => toast(err.message, 'error'));
                        renderUsers();
                        renderTokens();
                    },
                }, 'Remove'))))))));
    }
    async function renderTokens() {
        const list = await api('GET', '/tokens');
        fill(tokens, list.length === 0 ? h('p', { class: 'muted' }, 'No tokens. A token lets a script or an AI agent use the API and the MCP server.')
            : h('div', { class: 'table-wrap' }, h('table', { class: 'data data-compact', id: 'tokens' },
                h('thead', null, h('tr', null, h('th', null, 'Token'), h('th', null, 'Role'), h('th', { class: 'hide-sm' }, 'Last used'), h('th'))),
                h('tbody', null, list.map(t => h('tr', null,
                    h('td', { class: 'name' }, t.name, h('span', { class: 'secondary' }, `Created by ${t.created_by}, ${formatWhen(t.created)}`)),
                    h('td', null, t.role === 'admin' ? 'Administrator' : 'Viewer'),
                    h('td', { class: 'hide-sm' }, t.last_used ? formatWhen(t.last_used) : h('span', { class: 'muted' }, 'Never')),
                    h('td', { class: 'r' }, h('button', {
                        class: 'link', type: 'button', onclick: async () => {
                            if (!(await confirmDialog(`Revoke ${t.name}?`, 'Anything that uses this token stops working at once.', 'Revoke token', true))) return;
                            await api('DELETE', `/tokens/${enc(t.id)}`).catch(err => toast(err.message, 'error'));
                            renderTokens();
                        },
                    }, 'Revoke'))))))));
    }
    renderUsers().catch(() => {});
    renderTokens().catch(() => {});
    return row(titled ? 'Users and tokens' : null, [users, tokens,
        h('div', { class: 'actions' },
            h('button', { class: 'btn', type: 'button', onclick: async () => { if (await addUserDialog()) renderUsers(); } }, 'Add user'),
            h('button', { class: 'btn', type: 'button', onclick: async () => { if (await createTokenDialog()) renderTokens(); } }, 'Create token')),
        password]);
}

async function addUserDialog() {
    const problems = h('div', { class: 'problems', role: 'alert' });
    return dialog('Add user', close => {
        const form = dialogForm({
            title: 'Add user',
            body: [problems,
                field('us-name', 'User name', h('input', { class: 'input', id: 'us-name', name: 'name', type: 'text', autocomplete: 'off', required: true })),
                field('us-pw', 'Password', h('input', { class: 'input', id: 'us-pw', name: 'password', type: 'password', autocomplete: 'new-password', minlength: 8, required: true }), 'At least 8 characters.'),
                field('us-role', 'Role', h('select', { class: 'select', id: 'us-role', name: 'role' },
                    h('option', { value: 'viewer' }, 'Viewer: reads values and programs'), h('option', { value: 'admin' }, 'Administrator: changes programs, values and settings')))],
            onsubmit: async e => {
                e.preventDefault();
                const data = new FormData(form);
                try {
                    await api('POST', '/users', { name: data.get('name').trim(), password: data.get('password'), role: data.get('role') });
                    close(true);
                } catch (err) {
                    showProblems(problems, err);
                }
            },
            actions: [h('button', { type: 'button', class: 'btn', onclick: () => close(false) }, 'Cancel'), h('button', { type: 'submit', class: 'btn btn-primary' }, 'Add user')],
        });
        return form;
    });
}

/// Creates a token and shows it once, with the command that gives it to Claude Code.
async function createTokenDialog() {
    const problems = h('div', { class: 'problems', role: 'alert' });
    const created = await dialog('Create token', close => {
        const form = dialogForm({
            title: 'Create token',
            lead: 'A token lets a script or an AI agent use the API and the MCP server with the rights of its role.',
            body: [problems,
                field('tk-name', 'Name', h('input', { class: 'input', id: 'tk-name', name: 'name', type: 'text', placeholder: 'For example claude-code or laptop', required: true })),
                field('tk-role', 'Role', h('select', { class: 'select', id: 'tk-role', name: 'role' },
                    h('option', { value: 'admin' }, 'Administrator'), h('option', { value: 'viewer' }, 'Viewer (read only)')))],
            onsubmit: async e => {
                e.preventDefault();
                const data = new FormData(form);
                try {
                    close(await api('POST', '/tokens', { name: data.get('name').trim(), role: data.get('role') }));
                } catch (err) {
                    showProblems(problems, err);
                }
            },
            actions: [h('button', { type: 'button', class: 'btn', onclick: () => close(null) }, 'Cancel'), h('button', { type: 'submit', class: 'btn btn-primary' }, 'Create token')],
        });
        return form;
    });
    if (!created) return false;
    const base = `${location.protocol}//${location.host}`;
    const claude = mcpCommand(base, created.token);
    await dialog('Your new token', close => dialogForm({
        title: `Token ${created.name}`,
        lead: 'Copy it now: it is not shown again.',
        body: [
            h('pre', { class: 'command secret' }, created.token),
            h('div', { class: 'actions' }, h('button', { type: 'button', class: 'btn', onclick: () => copy(created.token) }, 'Copy token')),
            h('h3', { class: 'dlg-sub' }, 'Use it from Claude Code'),
            h('pre', { class: 'command' }, claude),
            h('div', { class: 'actions' }, h('button', { type: 'button', class: 'btn', onclick: () => copy(claude) }, 'Copy command')),
            h('h3', { class: 'dlg-sub' }, 'Or from a script'),
            h('pre', { class: 'command' }, `curl -H "Authorization: Bearer ${created.token}" ${base}/api/v1/plcs`)],
        actions: [h('button', { type: 'button', class: 'btn btn-primary', onclick: () => close(true) }, 'Done')],
    }), { wide: true });
    return true;
}

/// Pair with a fleet: a short code that lets a fleet adopt this device.
function pairingRow() {
    if (!isAdmin() || app.mode === 'fleet') return null;
    const body = h('div', { class: 'row-body-inner' },
        h('p', null, 'Pair with a fleet: create a code, then enter it with this PLC’s address in the fleet’s Add device.'),
        h('p', { class: 'secondary' }, 'The fleet gets a token of its own; revoke it under Users and tokens to leave the fleet.'));
    const end = h('div', { class: 'row-end-inner' }, h('button', { class: 'btn', type: 'button', onclick: newCode }, 'Pair with a fleet'));
    const r = row('Fleet', body, end);
    async function newCode() {
        try {
            const r2 = await api('POST', '/pairing');
            const code = String(r2.code);
            // Read out in groups of four: ABCD EFGH (a fleet takes it in any case, with or without the space)
            const grouped = code.length > 4 ? code.match(/.{1,4}/g).join(' ') : code;
            fill(body,
                h('p', null, 'Pairing code ', h('strong', { class: 'num pairing-code', dataset: { code } }, grouped),
                    `, valid until ${formatTime(r2.expires_ms)}.`),
                h('p', { class: 'secondary' }, 'Enter it in a fleet’s Add device to let that fleet manage this PLC. The fleet gets a token of its own; revoke it under Users and tokens to leave.'));
            fill(end, h('button', { class: 'link', type: 'button', onclick: newCode }, 'New code'));
        } catch (err) {
            toast(err.message, 'error');
        }
    }
    return r;
}

/// Links as JSON: a short form would hide what a link can do; the server checks every field.
async function linksDialog(ctx) {
    const plc = ctx.plc;
    const example = [{
        name: 'io1', host: '192.168.1.31', port: 502, unit: 1, period_ms: 100, timeout_ms: 200, status_tag: 'io1_ok',
        read: [{ tag: 'door_closed', area: 'discrete_input', address: 0 }],
        write: [{ tag: 'lamp', area: 'coil', address: 0 }],
    }];
    const current = plc.config.links;
    const version = plc.config_version;  // the links shown come from this configuration: sent as If-Match
    // The text is the PLC's links as they are ([] when it has none): Save sends what is shown, so an
    // example is shown beside it, never in it
    const text = h('textarea', { class: 'input textarea code', rows: current.length ? 18 : 6, spellcheck: 'false', 'aria-label': 'Links (JSON)' },
        JSON.stringify(current, null, 2));
    const hint = current.length ? null : h('div', { class: 'field' },
        h('p', { class: 'hint' }, 'This PLC has no links. One link looks like this (the tags it names must exist):'),
        h('pre', { class: 'command' }, JSON.stringify(example, null, 2)));
    const problems = h('div', { class: 'problems', role: 'alert' });
    await dialog('Links', close => dialogForm({
        title: `Links of ${plcLabel(plc)}`,
        lead: 'Each link polls one Modbus TCP device every period_ms: read copies the device’s entries into tags, write sends tags to its Coils or Holding registers when they change. status_tag, a Discrete input tag, is TRUE while the device answers. word_order "cdab" reads 32-bit values low word first.',
        body: [problems, text, hint],
        onsubmit: async e => {
            e.preventDefault();
            let links;
            try {
                links = JSON.parse(text.value);
            } catch (err) {
                showProblems(problems, new Error(`Not valid JSON: ${err.message}`));
                return;
            }
            try {
                const saved = await configRequest('PATCH', `/plcs/${enc(plc.id)}`, { links }, version,
                    { what: 'The configuration', onReload: () => { close(false); route(); } });
                if (saved === null) return;
                toast('Links saved');
                close(true);
                route();
            } catch (err) {
                showProblems(problems, err);
            }
        },
        actions: [h('button', { type: 'button', class: 'btn', onclick: () => close(false) }, 'Cancel'), h('button', { type: 'submit', class: 'btn btn-primary' }, 'Save links')],
    }), { wide: true });
}

/// Sets the PLC up from scratch from an example, or blank ("none": one task, main, with the PLC's own
/// template program and no tags): tags, tasks, programs and links are replaced.
async function startOverDialog(ctx) {
    let examples = [];
    try {
        examples = await api('GET', '/examples');
    } catch {}
    const label = plcLabel(ctx.plc);
    const problems = h('div', { class: 'problems', role: 'alert' });
    const select = h('select', { class: 'select', id: 'so-ex', name: 'example' },
        h('option', { value: '' }, 'An empty program, no tags'),
        examples.map(e => h('option', { value: e.name }, e.title)));
    const info = h('p', { class: 'hint' });
    select.addEventListener('change', () => {
        const e = examples.find(x => x.name === select.value);
        setText(info, e ? `${e.description} ${plural(e.tags.length, 'tag')}.` : 'One task, main, with an empty Structured Text program: PROGRAM main … END_PROGRAM.');
    });
    select.dispatchEvent(new Event('change'));
    const done = await dialog('Start over', close => dialogForm({
        title: `Start ${label} over?`,
        lead: 'Replaces the tags, tasks, programs and links, and restarts the PLC with every value at 0. The id and the network ports stay, so other devices still reach it. Earlier program versions stay in the history.',
        body: [problems, field('so-ex', 'Start with', select), info],
        onsubmit: async e => {
            e.preventDefault();
            try {
                close(await api('POST', `/plcs/${enc(ctx.plc.id)}/setup`, { example: select.value || 'none' }));
            } catch (err) {
                showProblems(problems, err);
            }
        },
        actions: [h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(null) }, 'Cancel'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, 'Start over')],
    }), { focus: '[data-cancel]' });
    if (done) {
        toast(`${done.name || done.id} started over`);
        const target = `#/plc/${enc(done.id)}/overview`;
        if (location.hash === target) route();
        else location.hash = target;
    }
}

// ───────────────────────────── Users and tokens of a fleet ─────────────────────────────

async function viewAccess() {
    const title = isAdmin() ? 'Users and tokens' : 'Account';
    setTitle(title);
    const main = shell('');
    main.append(h('div', { class: 'page-head' }, h('h1', { class: 'page-title' }, title)),
        h('div', { class: 'rows rows-top' }, usersRow(false), displayRow()));
}
