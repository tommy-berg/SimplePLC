// I/O: the devices (links) and the tag map, Modify and Force, the tag dialog, the register map
// export, and Add device → Probe → Map.
'use strict';

// ───────────────────────────── Who writes a tag ─────────────────────────────

/// The writer of a tag, from the tag list (GET /plcs/:id/tags): `writer` is the program, a link
/// that reads it from a device, or the clients; `sent_by` names the link that sends it to a device;
/// `status_of` marks a link's status tag. `dir` is 'in' for a field input (a link reads it), 'out'
/// for a field output (a link sends it). A tag that is not from the list has no writer: empty label.
function writerOf(ctx, tag) {
    const links = ctx.config.links;
    const width = tagWidth(tag.type);
    const deviceOf = name => {
        const s = ((ctx.plc.modbus && ctx.plc.modbus.links) || []).find(l => l.name === name);
        const c = links.find(l => l.name === name);
        return s ? linkDevice(s) : c ? `${c.host}:${c.port}` : name;
    };
    const lowFirst = name => (width === 2 && (links.find(l => l.name === name) || {}).word_order === 'cdab' ? ', low word first' : '');
    if (tag.status_of) {
        const link = tag.status_of.link;
        return { kind: 'status', dir: '', link, label: `Link ${link}`, detail: `TRUE while ${deviceOf(link)} answers` };
    }
    const w = tag.writer || null;
    if (w && w.kind === 'link') {
        const c = links.find(l => l.name === w.link) || {};
        return {
            kind: 'link', dir: 'in', link: w.link, period: c.period_ms, label: `Link ${w.link}`,
            detail: `${deviceOf(w.link)}, ${addressText(w.area, w.address, width)}${lowFirst(w.link)}`,
        };
    }
    const sent = tag.sent_by || null;
    const sentLink = sent ? links.find(l => l.name === sent.link) || {} : null;
    const sends = sent ? `${sent.link} sends it to ${deviceOf(sent.link)}, ${addressText(sent.area, sent.address, width)}${lowFirst(sent.link)}` : '';
    const out = sent ? { dir: 'out', link: sent.link, host: sentLink.host, period: sentLink.period_ms, device: `${deviceOf(sent.link)}, ${addressText(sent.area, sent.address, width)}` } : { dir: '' };
    if (w && w.kind === 'program') return { kind: 'program', ...out, program: w.program, label: `Program ${w.program}`, detail: sends };
    if (w && w.kind === 'clients') return { kind: 'clients', ...out, label: 'Clients', detail: sends || 'Modbus, OPC UA, panel' };
    return { kind: '', ...out, label: '', detail: sends };
}

/// A tag a link reads while that link is down: it keeps its last value (OPC UA: Uncertain).
function isStale(ctx, tag, writer) {
    const w = writer || writerOf(ctx, tag);
    if (w.kind !== 'link') return false;
    const l = ((ctx.plc.modbus && ctx.plc.modbus.links) || []).find(x => x.name === w.link);
    return Boolean(l && linkDown(l));
}

// ───────────────────────────── I/O tab ─────────────────────────────

async function tabIo(ctx, sub) {
    if (sub === 'add') return addDevice(ctx);
    const id = ctx.plc.id;
    let tags = await api('GET', `/plcs/${enc(id)}/tags`);
    if (ctx.seq !== app.routeSeq) return null;  // the page moved on while the tags were on their way
    let version = ctx.plc.config_version;  // the configuration the table shows: sent as If-Match
    for (const t of tags) if (!ctx.values.has(t.name)) ctx.values.set(t.name, t.value);
    for (const t of tags) if (t.forced) ctx.forced.add(t.name);
    const rows = new Map();
    const list = { get tags() { return tags; }, get version() { return version; }, reload };

    const devicesBody = h('div');
    const downNotes = h('div');
    const filter = h('input', { class: 'input w-md', type: 'search', placeholder: 'Filter by name', 'aria-label': 'Filter tags' });
    const areaFilter = h('select', { class: 'select hide-sm', 'aria-label': 'Area' },
        h('option', { value: '' }, 'All areas'), AREAS.map(a => h('option', { value: a }, areaLabel(a, 2))),
        h('option', { value: 'forced' }, 'Forced'), h('option', { value: 'stale' }, 'Stale'));
    const tagsBox = h('div');
    const el = h('div', { class: 'io-page' },
        section('Devices', isAdmin() ? h('a', { class: 'btn', href: `#/plc/${enc(id)}/io/add` }, 'Add device') : null, devicesBody, downNotes),
        section('Tags', [filter, areaFilter,
            isAdmin() ? h('button', { class: 'btn hide-sm hide-field', type: 'button', onclick: () => tagDialog(ctx, null, list) }, 'Add tag') : null,
            h('button', { class: 'btn hide-sm hide-field', type: 'button', onclick: () => exportRegisterMap(ctx, tags) }, 'Export register map')],
        tagsBox,
        h('p', { class: 'section-note' }, 'Modbus addresses count from 0, as sent on the wire. The number under each is the same entry counted from 1, Modicon style (40001 is Holding register 0), which many SCADA systems ask for. ',
            isAdmin() ? 'Click a value to modify or force it.' : '')));

    function renderDevices() {
        const plc = ctx.plc;
        const links = (plc.modbus && plc.modbus.links) || [];
        const configured = ctx.config.links;
        if (!configured.length && !links.length) {
            fill(devicesBody, h('div', { class: 'empty' },
                h('p', { class: 'empty-title' }, 'No devices'),
                h('p', { class: 'muted' }, 'A device is a Modbus TCP server this PLC reads from and writes to: remote I/O, a drive, a meter or another PLC. Add one, probe it, and pick its entries.')));
            clear(downNotes);
            return;
        }
        fill(devicesBody, h('div', { class: 'table-wrap' }, h('table', { class: 'data', id: 'devices' },
            h('thead', null, h('tr', null, h('th', null, 'Link'), h('th', null, 'Device'), h('th', { class: 'hide-sm' }, 'Address'), h('th', null, 'State'),
                h('th', { class: 'hide-sm r' }, 'Poll'), h('th', { class: 'hide-sm r' }, 'Errors'), h('th', { class: 'hide-sm' }, 'Tags'))),
            h('tbody', null, configured.map(c => {
                // A link just configured has no status until its poller reports: it is starting
                const l = links.find(x => x.name === c.name) || { name: c.name, host: c.host, port: c.port, unit: c.unit, ok: false, cycles: 0, errors: 0 };
                const down = linkDown(l);
                const since = down && l.last_success_ago_ms !== null && l.last_success_ago_ms !== undefined ? formatTime(Date.now() - l.last_success_ago_ms) : null;
                return h('tr', { dataset: { link: c.name } },
                    h('td', { class: 'name' }, c.name),
                    h('td', null, linkDeviceCell(l), h('span', { class: 'secondary only-sm' }, `${c.host}:${c.port}, every ${c.period_ms}${NBSP}ms`)),
                    h('td', { class: 'num hide-sm' }, `${c.host}:${c.port}, unit ${c.unit}`),
                    h('td', { class: 'link-state' }, l.ok ? h('span', { class: 'is-run' }, 'Up') : down
                        ? h('span', { class: 'is-fault' }, 'Down', since ? h('span', { class: 'secondary' }, `since ${since}`) : null)
                        : h('span', { class: 'muted' }, 'Starting')),
                    h('td', { class: 'num r hide-sm' }, `${c.period_ms}${NBSP}ms`),
                    h('td', { class: 'num r hide-sm' }, (l.errors || 0).toLocaleString('en-US')),
                    h('td', { class: 'hide-sm' }, `${(c.read || []).length} read, ${(c.write || []).length} written`));
            })))));
        fill(downNotes, configured.map(c => {
            const l = links.find(x => x.name === c.name);
            if (!l || !linkDown(l)) return null;
            const since = l.last_success_ago_ms !== null && l.last_success_ago_ms !== undefined ? ` since ${formatTime(Date.now() - l.last_success_ago_ms)}` : '';
            const read = (c.read || []).length;
            return h('div', { class: 'problem is-block', role: 'note' }, h('p', null,
                h('span', { class: 'code-id' }, `Link ${c.name} down`), ' ',
                `${c.host}:${c.port} has not answered within ${c.timeout_ms}${NBSP}ms${since}; SimplePLC asks again every ${c.period_ms}${NBSP}ms. `,
                read === 1 ? `The tag ${c.name} reads keeps its last value and shows Stale, and OPC UA clients see it as Uncertain. `
                    : read ? `The ${read} tags ${c.name} reads keep their last value and show Stale, and OPC UA clients see them as Uncertain. ` : '',
                c.status_tag ? `${c.status_tag} is FALSE: a program that checks it can stop safely. ` : '',
                (c.write || []).length ? 'Outputs go to the device again as soon as it answers.' : ''),
                l.last_error ? h('p', { class: 'secondary' }, `${sentence(l.last_error)}.`) : null);
        }));
    }

    function valueControl(tag) {
        if (!isAdmin()) return h('span', { class: 'value value-static' });
        const button = h('button', { class: 'value', type: 'button', 'aria-label': `${tag.name}: modify` });
        button.addEventListener('click', () => modifyTag(ctx, rows.get(tag.name) ? rows.get(tag.name).tag : tag));
        return button;
    }

    /// JSON turns NaN and ±∞ into null: the two registers tell which it is. One read per tag at a
    /// time, and the newest answer wins.
    const nonFinite = new Map();   // tag -> "NaN", "∞" or "−∞" as last read
    const refining = new Map();    // tag -> { again } while a read is on its way
    async function refineNonFinite(tag) {
        if (refining.has(tag.name)) {
            refining.get(tag.name).again = true;
            return;
        }
        const state = { again: true };
        refining.set(tag.name, state);
        try {
            while (state.again) {
                state.again = false;
                const data = await api('GET', `/plcs/${enc(id)}/areas/${tag.area}?start=${tag.address}&count=2`);
                const f = floatFromWords(data.values[0], data.values[1]);
                if (Number.isFinite(f) || ctx.values.get(tag.name) !== null) continue;
                nonFinite.set(tag.name, nonFiniteText(f));
                const r = rows.get(tag.name);
                if (r && r.value.classList.contains('nonfinite')) setText(r.value, nonFiniteText(f));
            }
        } catch {
        } finally {
            refining.delete(tag.name);
        }
    }

    function renderRow(r) {
        const value = ctx.values.has(r.tag.name) ? ctx.values.get(r.tag.name) : r.tag.value;
        const unknown = r.tag.type === 'float32' && (value === null || value === undefined);  // NaN or ±∞ in JSON
        const odd = isNonFinite(r.tag.type, value);
        const text = unknown ? nonFinite.get(r.tag.name) || 'NaN' : formatValue(r.tag.type, value);
        const forced = ctx.forced.has(r.tag.name);
        const writer = writerOf(ctx, r.tag);
        const stale = isStale(ctx, r.tag, writer);
        const key = `${text}|${forced}|${stale}|${writer.label}|${writer.detail}`;
        if (key === r.key) return;
        const changed = r.key !== null && r.key.split('|')[0] !== text;
        r.key = key;
        setText(r.value, text);
        r.value.classList.toggle('nonfinite', odd);
        r.value.classList.toggle('is-attn', forced);
        r.value.classList.toggle('is-stale', stale);
        r.value.title = odd ? 'Not a number or infinite' : '';
        r.forcedMark.hidden = !forced;
        r.staleMark.hidden = !stale;
        r.row.classList.toggle('is-forced', forced);
        r.row.dataset.stale = String(stale);
        fill(r.writer, writer.label, writer.detail ? h('span', { class: 'secondary' }, writer.detail) : null);
        setText(r.smLine, `${addressText(r.tag.area, r.tag.address, tagWidth(r.tag.type))} (${modiconText(r.tag.area, r.tag.address, tagWidth(r.tag.type))}), ${writer.label.replace(/^Link /, 'from ').replace(/^Program /, '')}`);
        if (changed && !reducedMotion.matches) {
            r.value.classList.add('is-changing');
            requestAnimationFrame(() => requestAnimationFrame(() => r.value.classList.remove('is-changing')));
        }
        if (unknown) refineNonFinite(r.tag);
        else nonFinite.delete(r.tag.name);
    }

    function renderTags() {
        rows.clear();
        if (tags.length === 0) {
            fill(tagsBox, h('div', { class: 'empty' },
                h('p', { class: 'empty-title' }, 'No tags yet'),
                h('p', { class: 'muted' }, 'A tag gives an address a name, so the program, Modbus and OPC UA clients and agents all use the same one. Add device creates tags for what it maps, and a program offers to create the tags it declares.'),
                isAdmin() ? h('div', { class: 'actions' }, h('button', { class: 'btn', type: 'button', onclick: () => tagDialog(ctx, null, list) }, 'Add tag')) : null));
            return;
        }
        const tbody = h('tbody');
        for (const tag of tags) {
            const width = tagWidth(tag.type);
            const r = {
                tag, key: null, value: valueControl(tag),
                forcedMark: h('span', { class: 'forced-mark', hidden: true }, 'Forced'),
                staleMark: h('span', { class: 'stale-mark', hidden: true }, 'Stale'),
                writer: h('td', { class: 'hide-sm writer' }),
                smLine: h('span', { class: 'secondary only-sm' }),
            };
            r.row = h('tr', { dataset: { tag: tag.name } },
                h('td', { class: 'name' }, h('span', { class: 'tag-name' }, tag.name),
                    tag.description ? h('span', { class: 'secondary tag-desc' }, tag.description) : null, r.smLine),
                h('td', { class: 'value-cell' }, r.value, r.forcedMark, r.staleMark),
                h('td', { class: 'hide-sm' }, ST_TYPE[tag.type] || tag.type),
                h('td', { class: 'hide-sm modbus-address' }, addressText(tag.area, tag.address, width), h('span', { class: 'secondary' }, modiconText(tag.area, tag.address, width))),
                h('td', { class: 'hide-sm hide-field muted opcua-id' }, `ns=1;s=${tag.name}`),
                r.writer,
                h('td', { class: 'hide-sm substitute-value' }, substituteText(tag) ?? h('span', { class: 'muted' }, '–')),
                h('td', { class: 'hide-sm retain' }, tag.retain ? 'Yes' : h('span', { class: 'muted' }, '–')),
                isAdmin() ? h('td', { class: 'r hide-field' }, h('button', { class: 'link', type: 'button', 'aria-label': `Edit ${tag.name}`, onclick: () => tagDialog(ctx, tag, list) }, 'Edit')) : null);
            r.row.dataset.search = `${tag.name} ${tag.description || ''}`.toLowerCase();
            rows.set(tag.name, r);
            renderRow(r);
            tbody.append(r.row);
        }
        fill(tagsBox, h('div', { class: 'table-wrap' }, h('table', { class: 'data tag-map', id: 'tags' },
            h('thead', null, h('tr', null, h('th', null, 'Name'), h('th', null, 'Value'), h('th', { class: 'hide-sm' }, 'Type'), h('th', { class: 'hide-sm' }, 'Modbus address'),
                h('th', { class: 'hide-sm hide-field' }, 'OPC UA node id'), h('th', { class: 'hide-sm' }, 'Written by'), h('th', { class: 'hide-sm' }, 'Substitute value'), h('th', { class: 'hide-sm' }, 'Retain'),
                isAdmin() ? h('th', { class: 'hide-field' }, h('span', { class: 'visually-hidden' }, 'Edit')) : null)),
            tbody)));
        applyFilter();
    }

    function applyFilter() {
        const q = filter.value.trim().toLowerCase();
        const area = areaFilter.value;
        for (const r of rows.values()) {
            const byArea = !area || (area === 'forced' ? ctx.forced.has(r.tag.name) : area === 'stale' ? r.row.dataset.stale === 'true' : r.tag.area === area);
            r.row.hidden = !byArea || (q !== '' && !r.row.dataset.search.includes(q));
        }
    }

    let reloading = null;
    function reload() {
        if (!reloading) {
            reloading = (async () => {
                try {
                    const v = ctx.plc.config_version;
                    tags = await api('GET', `/plcs/${enc(id)}/tags`);
                    version = v;
                    for (const t of tags) ctx.values.set(t.name, t.value);
                    renderTags();
                } finally {
                    reloading = null;
                }
            })();
        }
        return reloading;
    }

    filter.addEventListener('input', applyFilter);
    areaFilter.addEventListener('change', applyFilter);
    renderDevices();
    renderTags();

    let devicesKey = null;
    return {
        el,
        values() {
            for (const r of rows.values()) renderRow(r);
            if (areaFilter.value === 'forced') applyFilter();
        },
        status(plc) {
            // Another browser or an agent changed the configuration: show the tags it has now
            if (plc.config_version !== version) reload().catch(() => {});
            const key = JSON.stringify([((plc.modbus && plc.modbus.links) || []).map(l => [l.name, l.ok, linkDown(l), l.errors, l.identity || null]), (plc.config || {}).links]);
            if (key !== devicesKey) {
                devicesKey = key;
                renderDevices();
                for (const r of rows.values()) renderRow(r);
                applyFilter();
            }
        },
    };
}

/// Every tag with its Modbus address, Modicon number and OPC UA node id, as CSV (for SCADA).
async function exportRegisterMap(ctx, known) {
    let tags = known;
    if (!tags) {
        try {
            tags = await api('GET', `/plcs/${enc(ctx.plc.id)}/tags`);
        } catch (err) {
            toast(err.message, 'error');
            return;
        }
    }
    const lines = [csvLine(['name', 'type', 'area', 'address', 'modicon', 'opcua_node_id', 'written_by', 'substitute_value', 'retain', 'description'])];
    for (const t of tags) {
        const w = writerOf(ctx, t);
        lines.push(csvLine([t.name, ST_TYPE[t.type] || t.type, t.area, tagAddress(t), modiconText(t.area, t.address, tagWidth(t.type)),
            `ns=1;s=${t.name}`, [w.label, w.detail].filter(Boolean).join(': '), substituteText(t) ?? '', t.retain ? 'yes' : '', t.description || '']));
    }
    download(`${ctx.plc.id}-register-map.csv`, lines.join('\r\n') + '\r\n');
}

// ───────────────────────────── Modify and Force ─────────────────────────────

/// Numbers typed without a keyboard (field display): a kiosk browser has none.
function keypad(input) {
    const key = (k, label, cls) => h('button', { type: 'button', class: cls, 'aria-label': k === 'back' ? 'Delete' : null, dataset: { k } }, label);
    const pad = h('div', { class: 'keypad', role: 'group', 'aria-label': 'Keypad' },
        key('7', '7'), key('8', '8'), key('9', '9'), key('back', '⌫'),
        key('4', '4'), key('5', '5'), key('6', '6'), key('-', '−'),
        key('1', '1'), key('2', '2'), key('3', '3'), key('.', '.'),
        key('0', '0', 'wide'), key('clear', 'Clear', 'wide'));
    pad.addEventListener('click', e => {
        const b = e.target.closest('button');
        if (!b) return;
        let v = input.value;
        const k = b.dataset.k;
        if (k === 'back') v = v.slice(0, -1);
        else if (k === 'clear') v = '';
        else if (k === '-') v = v.startsWith('-') ? v.slice(1) : `-${v}`;
        else if (k === '.') { if (!v.includes('.')) v = `${v || '0'}.`; }
        else v += k;
        input.value = v;
        input.dispatchEvent(new Event('input'));
    });
    return pad;
}

/// Enter in a value dialog applies it, wherever the focus is but on another button.
function enterSubmits(form) {
    form.addEventListener('keydown', e => {
        if (e.key !== 'Enter') return;
        const t = e.target;
        if (t.matches('button:not(.seg-choice):not(.btn-primary)')) return;  // Cancel, Force… act as themselves
        e.preventDefault();
        form.requestSubmit();
    });
}

/// Modify a tag: reading never writes. Shows the address, type and writer, the change as
/// old → new, and writes on Enter (or Modify). A program or link that writes the tag overwrites a
/// modified value: Force… holds one.
async function modifyTag(ctx, tag) {
    const plc = ctx.plc;
    const current = ctx.values.has(tag.name) ? ctx.values.get(tag.name) : tag.value;
    const forced = ctx.forced.has(tag.name);
    const writer = writerOf(ctx, tag);
    const width = tagWidth(tag.type);
    const bool = tag.type === 'bool';
    let next = bool ? !current : null;
    const problems = h('div', { class: 'problems', role: 'alert' });
    const toText = h('span', { class: 'to' }, bool ? formatValue('bool', next) : formatValue(tag.type, current));
    const change = h('p', { class: 'change', 'aria-live': 'polite' }, h('span', { class: 'from' }, formatValue(tag.type, current)), h('span', { class: 'muted arrow' }, '→'), toText);
    let input = null;
    let valueField;
    if (bool) {
        const seg = segmented([['true', 'TRUE'], ['false', 'FALSE']], String(next), v => { next = v === 'true'; setText(toText, formatValue('bool', next)); }, 'New value');
        seg.querySelectorAll('button').forEach(b => b.classList.add('seg-choice'));
        valueField = h('div', { class: 'field' }, h('span', { class: 'label' }, 'New value'), seg);
    } else {
        input = h('input', { class: 'input w-sm', id: 'md-v', type: 'text', inputmode: 'decimal', autocomplete: 'off',
            value: isNonFinite(tag.type, current) ? '' : String(current), 'aria-label': `New value for ${tag.name}` });
        input.addEventListener('input', () => setText(toText, input.value.trim() || '–'));
        valueField = h('div', { class: 'field' }, h('label', { for: 'md-v' }, 'New value'), input, keypad(input));
    }
    const who = {
        clients: ' Written by clients; the program reads it.',
        program: ` Written by the program, which overwrites a modified value within one cycle. Force ${tag.name} to hold one.`,
        link: ` Read from link ${writer.link}${writer.period ? ` every ${writer.period}${NBSP}ms` : ''}, which overwrites a modified value. Force ${tag.name} to hold one.`,
        status: ` Written by link ${writer.link}: TRUE while the device answers.`,
    }[writer.kind] || '';
    let forceChosen = false;
    const ok = await dialog(`Modify ${tag.name}`, close => {
        const form = dialogForm({
            title: `Modify ${tag.name}`,
            lead: [h('p', { class: 'muted' }, `${addressText(tag.area, tag.address, width)}, ${ST_TYPE[tag.type]}.${who}`),
                h('p', { class: 'secondary' }, `Modicon ${modiconText(tag.area, tag.address, width)}, OPC UA ns=1;s=${tag.name}`),
                forced ? h('p', { class: 'is-attn' }, `Forced to ${formatValue(tag.type, current)}: a modified value has no effect until it is released. Modify forces the new value instead.`) : null],
            body: [h('div', { class: 'dlg-value' }, valueField, problems), h('div', { class: 'dlg-effects' }, change)],
            className: 'split-form',
            onsubmit: async e => {
                e.preventDefault();
                let value = next;
                if (!bool) {
                    if (input.value.trim() === '') return close(false);  // an empty field is a cancel
                    const n = parseNumber(input.value);
                    if (n.error) {
                        showProblems(problems, new Error(n.error));
                        input.select();
                        return;
                    }
                    value = n.value;
                }
                try {
                    if (forced) await api('POST', `/plcs/${enc(plc.id)}/forces`, { [tag.name]: value });
                    else await api('PUT', `/plcs/${enc(plc.id)}/values`, { [tag.name]: value });
                    close(true);
                } catch (err) {
                    showProblems(problems, err);
                }
            },
            actions: [
                forced ? h('button', { type: 'button', class: 'btn btn-quiet start', onclick: async () => { await releaseOne(ctx, tag.name); close(false); } }, 'Release')
                    : h('button', { type: 'button', class: 'btn btn-quiet start', onclick: () => { forceChosen = true; close(false); } }, 'Force…'),
                h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
                h('button', { type: 'submit', class: 'btn btn-primary' }, forced ? 'Force new value' : 'Modify', h('span', { class: 'key' }, 'Enter'))],
        });
        form.querySelector('.body').classList.add('split');
        enterSubmits(form);
        return form;
    }, { focus: bool ? '.btn-primary' : '#md-v' });
    if (forceChosen) return forceDialog(ctx, tag);
    return ok;
}

/// Force a tag: no value is chosen for you, Cancel has the focus, and the dialog says what the
/// machine and the program do while it holds.
async function forceDialog(ctx, tag) {
    const plc = ctx.plc;
    const label = plcLabel(plc);
    const current = ctx.values.has(tag.name) ? ctx.values.get(tag.name) : tag.value;
    const now = formatValue(tag.type, current);
    const writer = writerOf(ctx, tag);
    const width = tagWidth(tag.type);
    const bool = tag.type === 'bool';
    let to = null;
    const problems = h('div', { class: 'problems', role: 'alert' });
    const toCells = [];
    const toCell = () => {
        const s = h('span', { class: 'to' }, '–');
        toCells.push(s);
        return s;
    };
    const submit = h('button', { type: 'submit', class: 'btn btn-primary', disabled: true }, `Force ${tag.name}`);
    const sync = () => {
        for (const s of toCells) setText(s, to === null ? '–' : to);
        submit.disabled = to === null;
        setText(submit, to === null ? `Force ${tag.name}` : `Force ${tag.name} to ${to}`);
    };
    let input = null;
    let valueField;
    if (bool) {
        const seg = segmented([['TRUE', 'TRUE'], ['FALSE', 'FALSE']], null, v => { to = v; sync(); }, 'Force to');
        seg.querySelectorAll('button').forEach(b => b.classList.add('seg-choice'));
        valueField = h('div', { class: 'field' }, h('span', { class: 'label' }, 'Force to'), seg);
    } else {
        input = h('input', { class: 'input w-sm', id: 'fc-v', type: 'text', inputmode: 'decimal', autocomplete: 'off', placeholder: 'Value', 'aria-label': `Force ${tag.name} to` });
        input.addEventListener('input', () => { to = input.value.trim() || null; sync(); });
        valueField = h('div', { class: 'field' }, h('label', { for: 'fc-v' }, 'Force to'), input, keypad(input));
    }
    const what = h('span', null, h('span', { class: 'name' }, tag.name), h('span', { class: 'secondary' }, addressText(tag.area, tag.address, width)));
    const programName = writer.program ? `Program ${writer.program}` : 'The program';
    let lead;
    let rows;
    let warn = null;
    if (writer.dir === 'out') {
        lead = `${tag.name} is a field output. A forced value holds against ${writer.kind === 'clients' ? 'every writer' : 'the program'} until it is released.`;
        rows = [[what, now, toCell()],
            [h('span', null, writer.device, writer.host ? h('span', { class: 'secondary' }, `at ${writer.host}`) : null), now,
                h('span', null, toCell(), h('span', { class: 'secondary' }, `within ${writer.period || 100}${NBSP}ms`))],
            writer.kind === 'program' ? [programName, `writes ${tag.name}`, 'keeps running; its writes have no effect'] : ['Clients', `write ${tag.name}`, 'their writes have no effect']];
        warn = [h('strong', null, 'The machine acts on this output.'), ' ', h('span', { class: 'muted' }, 'Make sure nobody is within its reach and that switching it is safe.')];
    } else if (writer.dir === 'in' || writer.kind === 'status') {
        lead = `${tag.name} is a field input. The program sees the forced value instead of the device's; nothing is written to the device.`;
        rows = [[what, now, toCell()],
            ['The program', `sees ${now}`, h('span', null, 'sees ', toCell(), ', whatever the device reports')]];
        warn = [h('strong', null, 'The program stops seeing the real signal.'), ' ', h('span', { class: 'muted' }, 'Never force an input that guards people or the machine.')];
    } else {
        lead = `A forced value holds against every writer until it is released: ${writer.kind === 'clients' ? 'Modbus and OPC UA clients, agents and the panel' : 'the program'}.`;
        rows = [[what, now, toCell()],
            writer.kind === 'clients' ? ['Clients', `write ${tag.name}`, 'their writes have no effect'] : [programName, `writes ${tag.name}`, 'keeps running; its writes have no effect']];
    }
    const effects = h('table', { class: 'data data-compact effects' },
        h('thead', null, h('tr', null, h('th', null, 'What'), h('th', null, 'Now'), h('th', null, 'Forced'))),
        h('tbody', null, rows.map(r => h('tr', null, r.map(c => h('td', null, c))))));
    return dialog(`Force ${tag.name}`, close => {
        const form = dialogForm({
            title: `Force ${tag.name} on ${label}?`,
            className: 'force-form',
            lead,
            body: [h('div', { class: 'dlg-value' }, valueField, problems),
                h('div', { class: 'dlg-effects' }, effects, warn ? h('p', { class: 'force-warn' }, warn) : null,
                    h('p', { class: 'secondary' }, 'Release it under Forced on Overview.'))],
            onsubmit: async e => {
                e.preventDefault();
                if (to === null) return;
                let value = to === 'TRUE';
                if (!bool) {
                    const n = parseNumber(input.value);
                    if (n.error) {
                        showProblems(problems, new Error(n.error));
                        return;
                    }
                    value = n.value;
                }
                try {
                    await api('POST', `/plcs/${enc(plc.id)}/forces`, { [tag.name]: value });
                    close(true);
                } catch (err) {
                    showProblems(problems, err);
                }
            },
            actions: [h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'), submit],
        });
        form.querySelector('.body').classList.add('split');
        return form;
    }, { focus: '[data-cancel]', wide: true });
}

// ───────────────────────────── The tag dialog ─────────────────────────────

/// What uses a tag: the running programs that bind it (VAR_EXTERNAL) and the links that read or write it.
async function tagUsers(ctx, name) {
    const plc = ctx.plc;
    const upper = name.toUpperCase();
    const programs = [];
    for (const t of plc.tasks || []) {
        if (!t.loaded) continue;
        try {
            const data = await api('GET', `/plcs/${enc(plc.id)}/variables?task=${enc(t.name)}`);
            if (data.variables.some(v => (v.flags || []).includes('tag') && v.path.split('.').pop().toUpperCase() === upper)) {
                programs.push({ task: t.name, program: t.program });
            }
        } catch {}
    }
    const links = plc.config.links.filter(l => l.status_tag === name ||
        (l.read || []).some(m => m.tag === name) || (l.write || []).some(m => m.tag === name)).map(l => l.name);
    return { programs, links };
}

/// A running program uses the tag (409 tag_in_use, or found beforehand): changing or deleting it
/// stops the PLC with F_BINDING. Resolves true when the person still wants it.
function tagInUseDialog(name, programs, action) {
    const verb = action === 'delete' ? 'Deleting' : 'Changing';
    return dialog(`${name} is in use`, close => dialogForm({
        title: `${name} is used by a running program`,
        body: [
            h('ul', { class: 'list' }, programs.map(p => h('li', null, h('strong', null, `${p.program}.st`), ` (task ${p.task})`,
                p.diagnostics && p.diagnostics.length ? h('span', { class: 'secondary' }, p.diagnostics[0].message || String(p.diagnostics[0])) : null))),
            h('p', null, `${verb} ${name} makes the PLC fault (F_BINDING): the programs stop and every tag with a substitute value gets it, until the program is changed to match.`)],
        onsubmit: e => { e.preventDefault(); close(true); },
        actions: [
            h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, `${action === 'delete' ? 'Delete' : 'Change'} anyway (the PLC will fault)`)],
    }), { focus: '[data-cancel]' });
}

/// Adds a tag (tag null) or changes one. `list` is the tag table: its tags, the configuration
/// version they come from, and reload(). `preset` fills a new tag (name, type, area, address).
async function tagDialog(ctx, tag, list, preset) {
    const tags = list.tags;
    const problems = h('div', { class: 'problems', role: 'alert' });
    const overlap = h('p', { class: 'is-attn overlap', role: 'status', hidden: true });
    const area = h('select', { class: 'select', id: 'tg-area', name: 'area' }, AREAS.map(a => h('option', { value: a }, areaLabel(a))));
    const type = h('select', { class: 'select', id: 'tg-type', name: 'type' });
    const address = h('input', { class: 'input w-xs', id: 'tg-addr', name: 'address', type: 'number', min: 0, max: 65535, value: tag ? tag.address : 0, required: true });
    const modiconNote = h('p', { class: 'hint' });
    const substituteInput = h('input', { class: 'input w-sm', id: 'tg-substitute', name: 'substitute', type: 'text', value: tag && tag.substitute !== undefined && tag.substitute !== null ? String(tag.substitute) : '', placeholder: 'For example false or 0' });
    const retainInput = h('input', { type: 'checkbox', id: 'tg-retain', name: 'retain', checked: !!(tag && tag.retain) });
    const nameInput = h('input', { class: 'input', id: 'tg-name', name: 'name', type: 'text', value: tag ? tag.name : (preset && preset.name) || '', required: true,
        pattern: '[A-Za-z_][A-Za-z0-9_]{0,63}', title: 'Letters, digits and _, starting with a letter or _', autocomplete: 'off', spellcheck: 'false' });
    const desc = h('input', { class: 'input', id: 'tg-desc', name: 'description', type: 'text', value: tag ? tag.description || '' : '' });
    let addressTouched = Boolean(tag) || Boolean(preset && preset.address !== undefined);  // a new tag's address follows area and type until it is typed
    const syncTypes = keep => {
        const bit = area.value === 'coil' || area.value === 'discrete_input';
        const options = bit ? ['bool'] : REGISTER_TYPES;
        const previous = keep ? type.value : null;
        fill(type, ...options.map(t => h('option', { value: t }, ST_TYPE[t])));
        if (previous && options.includes(previous)) type.value = previous;
    };
    function checkOverlap() {
        const others = address.value === '' ? [] : overlappingTags(tags, area.value, Number(address.value), tagWidth(type.value), tag && tag.name);
        overlap.hidden = others.length === 0;
        if (others.length) {
            setText(overlap, `${areaLabel(area.value)} ${address.value} is used by ${others.map(t => t.name).join(', ')}: two tags on one address change each other.`);
        }
        const a = Number(address.value);
        setText(modiconNote, address.value === '' ? '' : `Modicon ${modiconText(area.value, a, tagWidth(type.value))}, counted from 1. OPC UA ns=1;s=${nameInput.value.trim() || '<name>'}.`);
    }
    /// The next free address for the area and type: a new tag never lands on one in use.
    function propose() {
        if (addressTouched) return checkOverlap();
        const free = freeAddress(tags, area.value, tagWidth(type.value), ctx.plc.image_size, null);
        address.value = free === null ? '' : String(free);
        checkOverlap();
    }
    area.value = tag ? tag.area : (preset && preset.area) || 'coil';
    syncTypes(false);
    if (tag) type.value = tag.type;
    else if (preset && preset.type) type.value = preset.type;
    if (preset && preset.address !== undefined && preset.address !== null) address.value = String(preset.address);
    area.addEventListener('change', () => {
        syncTypes(true);
        propose();
    });
    type.addEventListener('change', propose);
    address.addEventListener('input', () => {
        addressTouched = true;
        checkOverlap();
    });
    nameInput.addEventListener('input', checkOverlap);
    if (!tag) propose();
    else checkOverlap();

    const changed = await dialog(tag ? `Edit ${tag.name}` : 'Add tag', close => {
        let reloadRequested = false;
        /// Sends the change with the configuration version; a running program that uses the tag
        /// (409 tag_in_use) is named and the change is sent again only when the person wants it.
        async function send(method, path, body, force, action) {
            try {
                const r = await configRequest(method, force ? `${path}?force=1` : path, body, list.version,
                    { what: 'The tag table', onReload: () => { reloadRequested = true; } });
                if (r === null && reloadRequested) close(true);
                return r !== null;
            } catch (err) {
                if (err.status === 409 && err.code === 'tag_in_use' && !force) {
                    if (!(await tagInUseDialog(tag ? tag.name : body.name, err.details.programs || [], action))) return false;
                    return send(method, path, body, true, action);
                }
                showProblems(problems, err);
                return false;
            }
        }

        async function deleteTag() {
            const users = await tagUsers(ctx, tag.name);
            const uses = [...users.programs.map(p => `program ${p.program}.st (task ${p.task})`), ...users.links.map(l => `link ${l}`)];
            const message = [
                h('p', null, uses.length ? `${tag.name} is used by:` : `No running program and no link uses ${tag.name}.`),
                uses.length ? h('ul', { class: 'list' }, uses.map(u => h('li', null, u))) : null,
                users.programs.length ? h('p', null, 'Deleting it makes the PLC fault (F_BINDING): the programs stop and every tag with a substitute value gets it.') : null,
                users.links.length ? h('p', { class: 'secondary' }, 'A tag a link uses cannot be deleted: change the link first.') : null,
            ];
            if (!(await confirmDialog(`Delete ${tag.name}?`, message, users.programs.length ? 'Delete anyway (the PLC will fault)' : `Delete ${tag.name}`, true))) return;
            if (await send('DELETE', `/plcs/${enc(ctx.plc.id)}/tags/${enc(tag.name)}`, undefined, users.programs.length > 0, 'delete')) close(true);
        }

        const form = dialogForm({
            title: tag ? `Edit ${tag.name}` : 'Add tag',
            body: [problems,
                h('div', { class: 'fields' },
                    h('div', { class: 'field', style: 'flex:1 1 200px' }, h('label', { for: 'tg-name' }, 'Name'), nameInput),
                    h('div', { class: 'field' }, h('label', { for: 'tg-type' }, 'Type'), type)),
                h('div', { class: 'fields' },
                    h('div', { class: 'field' }, h('label', { for: 'tg-area' }, 'Area'), area),
                    h('div', { class: 'field' }, h('label', { for: 'tg-addr' }, 'Address'), address)),
                modiconNote,
                overlap,
                field('tg-desc', 'Description (optional)', desc),
                field('tg-substitute', 'Substitute value (optional)', substituteInput,
                    'true, false or a number, written to the tag at STOP and FAULT: false for a motor, for example. Leave it empty for a tag that keeps its last value.'),
                h('div', { class: 'field' }, h('label', { class: 'check' }, retainInput, 'Retain'),
                    h('p', { class: 'hint' }, 'Retentive memory: the value is saved when it changes and restored at power-up, for setpoints and totals. A cold restart starts it at 0.')),
                h('p', { class: 'secondary' }, 'Modbus and OPC UA clients can write Coils and Holding registers; Discrete inputs and Input registers are read-only for them. A 32-bit type takes two registers.')],
            onsubmit: async e => {
                e.preventDefault();
                const substitute = parseSubstitute(substituteInput.value);
                if (substitute.error) {
                    showProblems(problems, new Error(substitute.error));
                    substituteInput.focus();
                    return;
                }
                if (tag && tag.substitute !== undefined && tag.substitute !== null && substitute.value === null &&
                    !(await confirmDialog('Remove the substitute value?',
                        `Without a substitute value, ${tag.name} keeps its last value when the PLC stops or faults, instead of getting ${String(tag.substitute)}.`,
                        'Remove the substitute value', true))) return;
                const body = {
                    name: nameInput.value.trim(), area: area.value, type: type.value,
                    address: Number(address.value), description: desc.value.trim(), substitute: substitute.value,
                    retain: retainInput.checked,
                };
                showProblems(problems, null);
                if (tag) {
                    // A new name or type breaks the binding of a program that uses the tag
                    let force = false;
                    if (body.name !== tag.name || body.type !== tag.type) {
                        const users = await tagUsers(ctx, tag.name);
                        if (users.programs.length) {
                            if (!(await tagInUseDialog(tag.name, users.programs, 'change'))) return;
                            force = true;
                        }
                    }
                    if (await send('PATCH', `/plcs/${enc(ctx.plc.id)}/tags/${enc(tag.name)}`, body, force, 'change')) close(true);
                } else if (await send('POST', `/plcs/${enc(ctx.plc.id)}/tags`, body, false, 'add')) {
                    close(true);
                }
            },
            actions: [
                tag ? h('button', { type: 'button', class: 'btn btn-quiet start', onclick: deleteTag }, 'Delete tag') : null,
                h('button', { type: 'button', class: 'btn', onclick: () => close(false) }, 'Cancel'),
                h('button', { type: 'submit', class: 'btn btn-primary' }, tag ? 'Save tag' : 'Add tag')],
        });
        return form;
    });
    if (changed && list.reload) await list.reload();
    return changed;
}

// ───────────────────────────── Add device → Probe → Map ─────────────────────────────

const PROBE_AREAS = ['discrete_input', 'input_register', 'coil', 'holding_register'];
const NAME_RE = /^[A-Za-z_][A-Za-z0-9_]{0,63}$/;

function addDevice(ctx) {
    const plc = ctx.plc;
    const config = ctx.config;
    const id = plc.id;
    if (!isAdmin()) {
        return { el: h('div', { class: 'empty' }, h('p', { class: 'empty-title' }, 'Add device'), h('p', { class: 'muted' }, 'An administrator adds devices.')) };
    }
    /// Reads what the device offers (its identity and the first entries of each area) without writing anything
    const probe = body => api('POST', `/plcs/${enc(id)}/probe`, body);
    const existing = new Set(config.tags.map(t => t.name.toLowerCase()));
    const linkNames = new Set(config.links.map(l => l.name));
    let n = 1;
    while (linkNames.has(`io${n}`)) n++;

    const hostIn = h('input', { class: 'input w-md', id: 'dev-host', autocomplete: 'off', spellcheck: 'false', placeholder: '192.168.1.31 or io1.local' });
    const portIn = h('input', { class: 'input w-xs', id: 'dev-port', type: 'number', min: 1, max: 65535, value: 502 });
    const unitIn = h('input', { class: 'input w-xs', id: 'dev-unit', type: 'number', min: 0, max: 255, value: 1 });
    const probeButton = h('button', { class: 'btn btn-primary', type: 'submit', id: 'dev-probe' }, 'Probe');
    const deviceBody = h('div', { class: 'row-body-inner' }, h('p', { class: 'muted' }, 'Probe the device to see what it offers.'));
    const entriesBody = h('div', { class: 'row-body-inner' });
    const entriesRow = row(['Entries', 'Live, every second'], entriesBody, null, { hidden: true });
    const linkName = h('input', { class: 'input w-sm', id: 'link-name', value: `io${n}`, spellcheck: 'false' });
    const linkPoll = h('input', { class: 'input w-xs', id: 'link-poll', type: 'number', min: 10, max: 60000, value: 100 });
    const linkStatus = h('input', { class: 'input w-sm', id: 'link-status', value: `io${n}_ok`, spellcheck: 'false' });
    const statusNote = h('p', { class: 'secondary' });
    const summary = h('p', { class: 'secondary summary' });
    const problems = h('div', { class: 'problems', role: 'alert' });
    const create = h('button', { class: 'btn btn-primary', type: 'button', disabled: true }, 'Create the link');
    let wordOrder = 'abcd';
    let probed = null;  // the last answer
    let target = null;  // {host, port, unit} that answered
    let showAll = false;
    const entries = new Map();  // `${area}:${address}` -> entry
    let liveTimer = null;
    let pending = false;
    let statusTouched = false;

    linkName.addEventListener('input', () => {
        if (!statusTouched) linkStatus.value = `${linkName.value.trim()}_ok`;
        updateSummary();
    });
    linkStatus.addEventListener('input', () => { statusTouched = true; updateSummary(); });

    const wordSeg = segmented([['abcd', 'High word first'], ['cdab', 'Low word first']], wordOrder, v => { wordOrder = v; renderValues(); }, 'Word order');
    const probeBody = h('tbody');
    const showAllLink = h('button', { class: 'link', type: 'button', onclick: () => { showAll = !showAll; renderEntries(); } });

    const bit = area => area === 'coil' || area === 'discrete_input';
    const isInput = area => area === 'discrete_input' || area === 'input_register';
    const valuesOf = area => (probed && probed.areas && probed.areas[area] && probed.areas[area].values) || null;

    function words(area, address) {
        const v = valuesOf(area);
        return v ? [v[address], v[address + 1]] : [undefined, undefined];
    }
    function readingText(e, order) {
        const v = valuesOf(e.area);
        if (!v || v[e.address] === undefined) return '–';
        if (bit(e.area)) return v[e.address] ? 'TRUE' : 'FALSE';
        if (tagWidth(e.type) === 2) {
            const [a, b] = words(e.area, e.address);
            if (b === undefined) return '–';
            return formatReading(e.type, decode32([a, b], order, e.type));
        }
        const raw = v[e.address] & 0xffff;
        return String(e.type === 'int16' && raw > 0x7fff ? raw - 0x10000 : raw);
    }
    /// A register and the next one that read as a sensible REAL: offered as a hint, never chosen for you.
    function floatHint(e) {
        if (tagWidth(e.type) === 2 || bit(e.area)) return null;
        const [a, b] = words(e.area, e.address);
        if (a === undefined || b === undefined || !a || !b) return null;
        const plausible = x => Number.isFinite(x) && Math.abs(x) >= 0.01 && Math.abs(x) <= 100000;
        const here = decode32([a, b], wordOrder, 'float32');
        const other = decode32([a, b], wordOrder === 'abcd' ? 'cdab' : 'abcd', 'float32');
        if (plausible(here)) return `With ${e.address + 1} as REAL: ${formatReading('float32', here)}`;
        if (plausible(other)) return `With ${e.address + 1} as REAL, ${wordOrder === 'abcd' ? 'low' : 'high'} word first: ${formatReading('float32', other)}`;
        return null;
    }

    function entryFor(area, address) {
        const key = `${area}:${address}`;
        let e = entries.get(key);
        if (e) return e;
        e = { area, address, type: bit(area) ? 'bool' : 'uint16', ticked: false, use: isInput(area) ? 'read' : 'write-substitute' };
        e.tick = h('input', { type: 'checkbox', 'aria-label': `Map ${addressText(area, address)}` });
        e.name = h('input', { class: 'input', placeholder: 'Name', 'aria-label': `Name for ${addressText(area, address)}`, spellcheck: 'false', autocomplete: 'off' });
        e.value = h('span', { class: 'probe-value' });
        e.other = h('span', { class: 'secondary' });
        e.entry = h('td', { class: 'entry' });
        e.typeCell = h('td', { class: 'type' });
        e.useCell = h('td', { class: 'use' });
        e.row = h('tr', { dataset: { entry: key } },
            h('td', { class: 'tick' }, e.tick), e.entry, h('td', { class: 'reading' }, e.value, e.other), e.typeCell,
            h('td', { class: 'name' }, e.name), e.useCell);
        e.tick.addEventListener('change', () => {
            e.ticked = e.tick.checked;
            if (e.ticked && !e.name.value.trim()) e.name.value = suggestName(e);
            renderEntry(e);
            updateSummary();
        });
        e.name.addEventListener('input', () => {
            if (e.name.value.trim() && !e.ticked) {
                e.ticked = true;
                e.tick.checked = true;
                renderEntry(e);
            }
            updateSummary();
        });
        entries.set(key, e);
        return e;
    }

    function suggestName(e) {
        const prefix = { discrete_input: 'di', input_register: 'ir', coil: 'coil', holding_register: 'hr' }[e.area];
        return `${linkName.value.trim() || 'io'}_${prefix}${e.address}`;
    }

    function renderEntry(e) {
        const width = tagWidth(e.type);
        setText(e.entry, addressText(e.area, e.address, width));
        e.row.classList.toggle('is-off', !e.ticked);
        if (bit(e.area)) {
            fill(e.typeCell, h('span', { class: 'muted' }, 'BOOL'));
        } else {
            const select = h('select', { class: 'select', 'aria-label': `Read ${addressText(e.area, e.address)} as` },
                REGISTER_TYPES.map(t => h('option', { value: t }, ST_TYPE[t])));
            select.value = e.type;
            select.addEventListener('change', () => {
                e.type = select.value;
                const second = entries.get(`${e.area}:${e.address + 1}`);
                if (tagWidth(e.type) === 2 && second) {  // the next register is now part of this value
                    second.ticked = false;
                    second.tick.checked = false;
                }
                renderEntries();
                updateSummary();
            });
            fill(e.typeCell, select);
        }
        if (isInput(e.area)) {
            fill(e.useCell, e.ticked ? h('span', { class: 'muted' }, 'Read') : null);
        } else if (e.ticked) {
            const zero = bit(e.area) ? 'FALSE' : '0';
            const select = h('select', { class: 'select', 'aria-label': `Use of ${addressText(e.area, e.address)}` },
                h('option', { value: 'write-substitute' }, `Write, substitute value ${zero}`), h('option', { value: 'write-keep' }, 'Write, no substitute value'), h('option', { value: 'read' }, 'Read'));
            select.value = e.use;
            select.addEventListener('change', () => { e.use = select.value; updateSummary(); });
            fill(e.useCell, select);
        } else {
            clear(e.useCell);
        }
        renderValue(e);
    }

    function renderValue(e) {
        setText(e.value, readingText(e, wordOrder));
        if (tagWidth(e.type) === 2 && !bit(e.area)) {
            setText(e.other, `${wordOrder === 'abcd' ? 'Low' : 'High'} word first: ${readingText(e, wordOrder === 'abcd' ? 'cdab' : 'abcd')}`);
        } else {
            setText(e.other, floatHint(e) || '');
        }
    }

    function renderValues() {
        for (const e of entries.values()) if (e.row.isConnected) renderValue(e);
    }

    function renderEntries() {
        const rowsOut = [];
        let total = 0;
        let hiddenCount = 0;
        for (const area of PROBE_AREAS) {
            const values = valuesOf(area);
            const info = probed && probed.areas ? probed.areas[area] : null;
            rowsOut.push(h('tr', { class: 'area' }, h('td', { colspan: 6 }, areaLabel(area, 2),
                !values && info && info.error ? h('span', { class: 'secondary' }, `Not readable: ${info.error}`) : null)));
            if (!values) continue;
            total += values.length;
            for (let address = 0; address < values.length; address++) {
                const prev = entries.get(`${area}:${address - 1}`);
                if (prev && tagWidth(prev.type) === 2) continue;  // the second register of a 32-bit value
                const e = entryFor(area, address);
                const interesting = address < 8 || e.ticked || values[address] || (!bit(area) && values[address + 1]);
                if (!showAll && !interesting) {
                    hiddenCount++;
                    continue;
                }
                renderEntry(e);
                rowsOut.push(e.row);
            }
        }
        fill(probeBody, rowsOut);
        setText(showAllLink, showAll ? 'Show fewer entries' : `Show all ${total} entries`);
        showAllLink.hidden = !showAll && hiddenCount === 0;
    }

    /// The tags and the link the ticked entries make, with the next free local addresses.
    function plan() {
        const ticked = [...entries.values()].filter(e => e.ticked).sort((a, b) => PROBE_AREAS.indexOf(a.area) - PROBE_AREAS.indexOf(b.area) || a.address - b.address);
        const all = [...config.tags];
        const tags = [];
        const read = [];
        const write = [];
        const problemsFound = [];
        const names = new Set();
        const take = (name, area, type, extra) => {
            const address = freeAddress(all, area, tagWidth(type), plc.image_size, null);
            if (address === null) {
                problemsFound.push(`No free ${areaLabel(area, 2).toLowerCase()} are left for ${name}: make the process image larger in Settings.`);
                return null;
            }
            const t = { name, area, address, type, ...(extra || {}) };
            all.push(t);
            tags.push(t);
            return t;
        };
        for (const e of ticked) {
            const name = e.name.value.trim();
            if (!NAME_RE.test(name)) {
                problemsFound.push(name ? `${name} is not a valid tag name: letters, digits and _, starting with a letter or _.` : `Give ${addressText(e.area, e.address, tagWidth(e.type))} a name.`);
                continue;
            }
            if (existing.has(name.toLowerCase()) || names.has(name.toLowerCase())) {
                problemsFound.push(`There is a tag named ${name} already.`);
                continue;
            }
            names.add(name.toLowerCase());
            const writes = !isInput(e.area) && e.use !== 'read';
            const local = writes ? (bit(e.area) ? 'coil' : 'holding_register') : (bit(e.area) ? 'discrete_input' : 'input_register');
            const extra = writes && e.use === 'write-substitute' ? { substitute: e.type === 'bool' ? false : 0 } : {};
            const t = take(name, local, e.type, extra);
            if (!t) continue;
            (writes ? write : read).push({ tag: name, area: e.area, address: e.address });
        }
        const status = linkStatus.value.trim();
        let statusTag = null;
        if (status) {
            if (!NAME_RE.test(status)) problemsFound.push(`${status} is not a valid name for the status tag.`);
            else if (existing.has(status.toLowerCase()) || names.has(status.toLowerCase())) problemsFound.push(`There is a tag named ${status} already: name the status tag differently.`);
            else statusTag = take(status, 'discrete_input', 'bool');
        }
        const name = linkName.value.trim();
        if (!NAME_RE.test(name)) problemsFound.push('Give the link a name: letters, digits and _.');
        else if (linkNames.has(name)) problemsFound.push(`There is a link named ${name} already.`);
        const link = target ? {
            name, host: target.host, port: target.port, unit: target.unit, period_ms: Number(linkPoll.value) || 100,
            ...(statusTag ? { status_tag: statusTag.name } : {}),
            ...(wordOrder === 'cdab' ? { word_order: 'cdab' } : {}),
            read, write,
        } : null;
        return { tags, link, problems: problemsFound, mapped: read.length + write.length, statusTag };
    }

    function rangeText(list) {
        const byArea = new Map();
        for (const t of list) {
            if (!byArea.has(t.area)) byArea.set(t.area, []);
            for (let i = 0; i < tagWidth(t.type); i++) byArea.get(t.area).push(t.address + i);
        }
        return [...byArea].map(([area, addresses]) => {
            addresses.sort((a, b) => a - b);
            const ranges = [];
            for (const a of addresses) {
                const last = ranges[ranges.length - 1];
                if (last && a === last[1] + 1) last[1] = a;
                else ranges.push([a, a]);
            }
            const count = addresses.length;
            return `${areaLabel(area, count === 1 ? 1 : 2)} ${ranges.map(([a, b]) => (a === b ? a : `${a}–${b}`)).join(', ')}`;
        }).join(', ');
    }

    function updateSummary() {
        const p = plan();
        setText(statusNote, p.statusTag || linkStatus.value.trim() ? `${linkStatus.value.trim()} is TRUE while the device answers. The program can use it to stop safely.`
            : 'Without a status tag, nothing tells the program that the device stopped answering.');
        const name = linkName.value.trim() || 'the link';
        if (!target) {
            setText(summary, '');
            create.disabled = true;
            setText(create, 'Create the link');
            return;
        }
        if (!p.mapped) {
            setText(summary, 'Tick the entries to read or write, and name them.');
            create.disabled = true;
            setText(create, `Create link ${name}`);
            return;
        }
        const status = p.statusTag ? ` (${p.statusTag.area === 'discrete_input' ? `${addressText(p.statusTag.area, p.statusTag.address)} is ${p.statusTag.name}` : p.statusTag.name})` : '';
        setText(summary, `Creates ${plural(p.tags.length, 'tag')} at the next free addresses of this PLC: ${rangeText(p.tags)}${status}.`);
        create.disabled = false;
        setText(create, `Create ${plural(p.tags.length, 'tag')} and link ${name}`);
    }

    async function runProbe() {
        const host = hostIn.value.trim();
        if (!host) {
            hostIn.focus();
            return;
        }
        const body = { host, port: Number(portIn.value) || 502, unit: unitIn.value.trim() === '' ? 1 : Number(unitIn.value), timeout_ms: 1000 };
        probeButton.disabled = true;
        setText(probeButton, 'Probing…');
        fill(deviceBody, h('p', { class: 'muted' }, `Asking ${host}:${body.port}…`));
        clearInterval(liveTimer);
        try {
            const r = await probe(body);
            probed = r;
            if (!r.ok) {
                target = null;
                entriesRow.hidden = true;
                fill(deviceBody, h('p', { class: 'is-fault' }, `No answer from ${host}:${body.port}, unit ${body.unit}`),
                    h('p', { class: 'secondary' }, `${r.error ? sentence(r.error) : 'The device did not answer'}. Check the address, the port and the unit id; nothing was written.`));
            } else {
                target = { host, port: body.port, unit: body.unit };
                const readable = PROBE_AREAS.filter(a => valuesOf(a)).map(a => `${areaLabel(a, 2)} 0–${valuesOf(a).length - 1}`);
                const idy = r.identity;
                fill(deviceBody,
                    idy && (idy.vendor || idy.product) ? h('p', null, h('strong', null, [idy.vendor, idy.product].filter(Boolean).join(' ')), idy.revision ? `, revision ${idy.revision}` : '')
                        : h('p', null, `${host}:${body.port} answers`),
                    h('p', { class: 'secondary' }, `Answered in ${r.latency_ms ?? '–'}${NBSP}ms. `,
                        idy ? 'Identity from Read Device Identification (function 43/14). ' : 'It does not report its name (Read Device Identification, function 43/14). ',
                        readable.length ? `Readable: ${readable.join(', ')}.` : ''));
                entriesRow.hidden = false;
                renderEntries();
                liveTimer = setInterval(refresh, 1000);
            }
        } catch (err) {
            fill(deviceBody, h('p', { class: 'is-fault' }, err.message));
        }
        probeButton.disabled = false;
        setText(probeButton, 'Probe again');
        probeButton.classList.remove('btn-primary');
        updateSummary();
    }

    /// The values once a second while the page is open: reading only
    async function refresh() {
        if (pending || !target || !el.isConnected) return;
        pending = true;
        try {
            const r = await probe({ ...target, timeout_ms: 1000 });
            if (r.ok) {
                probed = { ...r, identity: r.identity || (probed && probed.identity) };
                renderValues();
            }
        } catch {}
        pending = false;
    }

    async function createLink() {
        const p = plan();
        if (p.problems.length) {
            showProblems(problems, { message: p.problems[0], problems: p.problems });
            return;
        }
        create.disabled = true;
        try {
            const r = await configRequest('PATCH', `/plcs/${enc(id)}`, { tags: [...config.tags, ...p.tags], links: [...config.links, p.link] },
                plc.config_version, { what: 'The configuration', onReload: () => route() });
            if (r === null) {
                create.disabled = false;
                return;
            }
            toast(`Created link ${p.link.name} and ${plural(p.tags.length, 'tag')}`);
            location.hash = `#/plc/${enc(id)}/io`;
        } catch (err) {
            showProblems(problems, err);
            create.disabled = false;
        }
    }
    create.addEventListener('click', createLink);

    fill(entriesBody,
        h('div', { class: 'inline-fields' }, h('span', { class: 'muted' }, '32-bit values'), wordSeg),
        h('div', { class: 'table-wrap' }, h('table', { class: 'data probe', id: 'probe' },
            h('thead', null, h('tr', null, h('th', { class: 'tick' }, h('span', { class: 'visually-hidden' }, 'Use')), h('th', null, 'Entry'), h('th', null, 'Value'),
                h('th', null, 'Read as'), h('th', null, 'Name'), h('th', null, 'Use'))),
            probeBody)),
        h('p', { class: 'secondary' }, showAllLink));

    const el = h('div', { class: 'add-device' },
        h('p', { class: 'crumb' }, h('a', { href: `#/plc/${enc(id)}/io` }, 'I/O')),
        h('div', { class: 'page-head' }, h('h2', { class: 'page-title' }, 'Add device')),
        h('div', { class: 'rows' },
            row('Address', [
                h('form', { class: 'fields', onsubmit: e => { e.preventDefault(); runProbe(); } },
                    h('div', { class: 'field' }, h('label', { for: 'dev-host' }, 'Host'), hostIn),
                    h('div', { class: 'field' }, h('label', { for: 'dev-port' }, 'Port'), portIn),
                    h('div', { class: 'field' }, h('label', { for: 'dev-unit' }, 'Unit'), unitIn),
                    probeButton),
                h('p', { class: 'secondary' }, 'Probing only reads. Nothing is written to the device until you create the link.')]),
            row('Device', deviceBody),
            entriesRow,
            row('Link', [
                h('div', { class: 'fields' },
                    h('div', { class: 'field' }, h('label', { for: 'link-name' }, 'Name'), linkName),
                    h('div', { class: 'field' }, h('label', { for: 'link-poll' }, 'Poll every (ms)'), linkPoll),
                    h('div', { class: 'field' }, h('label', { for: 'link-status' }, 'Status tag'), linkStatus)),
                statusNote])),
        h('div', { class: 'probe-foot' }, summary, problems,
            h('div', { class: 'actions' }, h('a', { class: 'btn', href: `#/plc/${enc(id)}/io` }, 'Cancel'), create)));
    linkPoll.addEventListener('input', updateSummary);
    updateSummary();
    onLeave(() => clearInterval(liveTimer));
    return { el, mounted: () => hostIn.focus() };
}
