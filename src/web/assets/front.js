// The controller front: a flat technical drawing of a controller whose every light is bound to
// something the device reports. And the fleet's four-light glyph.
'use strict';

/// What the lights show, from the PLC's status:
///   state    'running' | 'stopped' | 'fault' | 'starting' | 'offline'
///   forces   number of forced values (MAINT)
///   linksDown  number of links that get no answer (BF)
///   traffic  a Modbus request or link poll in the last second (ACT, steady)
///   ports    [P1 up, P2 up] from the device's network interfaces (LINK); empty until it reports them
///   di, coils  the process image's Discrete inputs 0–7 and Coils 0–7
function frontLights(st) {
    if (st.state === 'offline') return {};
    const L = {
        run: st.state === 'running' ? 'green' : st.state === 'starting' ? 'blink:start' : 'amber',
        error: st.state === 'fault' ? 'blink:red' : '',
        maint: st.forces > 0 ? 'amber' : '',
        bf: st.linksDown > 0 ? 'red' : '',  // steady: only a Fault flashes
    };
    (st.ports || []).forEach((up, i) => {
        L[`p${i + 1}-link`] = up ? 'green' : '';
        L[`p${i + 1}-act`] = up && st.traffic ? 'green' : '';
    });
    (st.di || []).forEach((v, i) => { L[`di${i}`] = v ? 'green' : ''; });
    (st.coils || []).forEach((v, i) => { L[`coil${i}`] = v ? 'green' : ''; });
    return L;
}

const LIGHT_WORDS = { green: 'green', amber: 'amber', red: 'red', 'blink:red': 'flashing red', 'blink:start': 'flashing green and amber' };

function applyLights(leds, el, title, id, st) {
    const L = frontLights(st);
    for (const [name, led] of Object.entries(leds)) {
        const v = L[name] || '';
        const [on, blink] = v.startsWith('blink:') ? ['', v.slice(6)] : [v, ''];
        if ((led.getAttribute('data-on') || '') === on && (led.getAttribute('data-blink') || '') === blink) continue;
        for (const [attr, value] of [['data-on', on], ['data-blink', blink]]) {
            if (value) led.setAttribute(attr, value);
            else led.removeAttribute(attr);
        }
    }
    el.classList.toggle('is-offline', st.state === 'offline');
    const word = k => LIGHT_WORDS[L[k]] || 'off';
    setText(title, st.state === 'offline'
        ? `Front of ${id}: offline, every light dark`
        : `Front of ${id}: RUN/STOP ${word('run')}, ERROR ${word('error')}, MAINT ${word('maint')}, BF ${word('bf')}`);
}

/// The front of a PLC (320 × 184). update(st) sets its lights; setId(id) its label.
function controllerFront(id) {
    const leds = {};
    const led = (name, x, y, w, ht, rx) => (leds[name] = svg('rect', { class: 'led', 'data-led': name, x, y, width: w, height: ht, rx }));
    const text = (x, y, s, attrs) => svg('text', { x, y, ...(attrs || {}) }, s);
    const title = svg('title');
    const idText = text(150, 62.5, id, { class: 'd-id' });
    const jack = (x, n) => [
        text(x + 2, 113, 'LINK', { class: 'd-tiny' }), text(x + 42, 113, 'ACT', { class: 'd-tiny', 'text-anchor': 'end' }),
        svg('rect', { class: 'd-jack', x, y: 117, width: 44, height: 30, rx: 1.5 }),
        led(`p${n}-link`, x + 3, 120, 9, 4, 0.8), led(`p${n}-act`, x + 32, 120, 9, 4, 0.8),
        svg('path', { class: 'd-hole', d: `M${x + 8} 128 H${x + 36} V140 H${x + 27} V144 H${x + 17} V140 H${x + 8} Z` }),
        text(x, 160, `X1 P${n}`)];
    const ioRow = (prefix, label, y) => [
        text(140, y - 6, label),
        [0, 1, 2, 3, 4, 5, 6, 7].map(i => [
            led(`${prefix}${i}`, 140 + i * 15, y, 10, 6, 1),
            text(145 + i * 15, y + 15, String(i), { class: 'd-tiny', 'text-anchor': 'middle' })])];
    const el = svg('svg', { class: 'device', viewBox: '0 0 320 184', role: 'img' },
        title,
        svg('rect', { class: 'd-front', x: 0.5, y: 0.5, width: 319, height: 183, rx: 3 }),
        svg('path', { class: 'd-band', d: 'M3.5 1 H316.5 A2.5 2.5 0 0 1 319 3.5 V26 H1 V3.5 A2.5 2.5 0 0 1 3.5 1 Z' }),
        svg('line', { class: 'd-edge', x1: 0.5, y1: 26.5, x2: 319.5, y2: 26.5 }),
        svg('line', { class: 'd-edge', x1: 126.5, y1: 26.5, x2: 126.5, y2: 183.5 }),
        text(14, 17.5, 'SimplePLC', { class: 'd-brand' }),
        led('run', 14, 42, 12, 6, 1), text(33, 48.5, 'RUN/STOP'),
        led('error', 14, 58, 12, 6, 1), text(33, 64.5, 'ERROR'),
        led('maint', 14, 74, 12, 6, 1), text(33, 80.5, 'MAINT'),
        led('bf', 14, 90, 12, 6, 1), text(33, 96.5, 'BF'),
        jack(14, 1), jack(66, 2),
        svg('rect', { class: 'd-label', x: 140, y: 40, width: 166, height: 34, rx: 1 }),
        idText,
        ioRow('di', 'Discrete inputs', 102),
        ioRow('coil', 'Coils', 142));
    let current = id;
    function setId(next) {
        current = next;
        setText(idText, next);
        // A long id still fits its label strip
        idText.setAttribute('font-size', String(Math.min(15, Math.floor(146 / Math.max(1, next.length * 0.58)))));
    }
    setId(id);
    return { el, update: st => applyLights(leds, el, title, current, st), setId };
}

/// The same four lights on a 12 × 33 front, at the start of each fleet row.
function ledGlyph(id) {
    const leds = {};
    const led = (name, y) => (leds[name] = svg('rect', { class: 'led', 'data-led': name, x: 3, y, width: 6, height: 4, rx: 0.8 }));
    const title = svg('title');
    const el = svg('svg', { class: 'led-glyph', viewBox: '0 0 12 33', role: 'img' },
        title,
        svg('rect', { class: 'd-front', x: 0.5, y: 0.5, width: 11, height: 32, rx: 1.5 }),
        led('run', 4), led('error', 11), led('maint', 18), led('bf', 25));
    return { el, update: st => applyLights(leds, el, title, id, st) };
}

/// The lights' inputs from a PLC summary (and, when known, the device and the first I/O bits).
/// The LINK and ACT lights stay dark until the device has reported its network interfaces.
function lightState(plc, { offline = false, starting = false, info = null, bits = null } = {}) {
    if (offline || plc.online === false) return { state: 'offline' };
    const links = (plc.modbus && plc.modbus.links) || [];
    const recent = ms => ms !== null && ms !== undefined && ms < 1000;
    const traffic = recent(plc.modbus && plc.modbus.last_request_ago_ms) || links.some(l => recent(l.last_request_ago_ms));
    const state = starting ? 'starting' : plc.state === 'fault' ? 'fault' : plc.running ? 'running' : 'stopped';
    return {
        state, forces: plc.forces || 0, linksDown: links.filter(linkDown).length, traffic,
        ports: info ? devicePorts(info) : [],
        di: bits ? bits.di : [], coils: bits ? bits.coils : [],
    };
}

/// A link that has answered before (or tried) and gets no answer now. A link still starting is not down.
function linkDown(l) {
    return !l.ok && ((l.cycles || 0) > 0 || (l.errors || 0) > 0);
}

/// A network interface that is a port of the device: not the loopback, a tunnel (a VPN), a bridge or a
/// container's virtual interface, which would otherwise light a second Ethernet port.
function isPhysicalInterface(name) {
    return !/^(lo|utun|tun|tap|docker|br-|veth|awdl|llw|bridge)/.test(name);
}

/// X1 P1 is the interface with the address this page reached the PLC at (or the first one up),
/// X1 P2 the next one; a port without an interface stays dark.
function devicePorts(info) {
    const nets = (info.network || []).filter(n => isPhysicalInterface(n.name));
    if (!nets.length) return [false, false];
    const host = location.hostname;
    let first = nets.findIndex(n => (n.addresses || []).some(a => a.split('/')[0] === host));
    if (first < 0) first = Math.max(0, nets.findIndex(n => n.up));
    const second = nets.find((n, i) => i !== first);
    return [Boolean(nets[first] && nets[first].up), Boolean(second && second.up)];
}
