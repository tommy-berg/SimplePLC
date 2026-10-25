// SimplePLC web panel: the core (DOM helpers, API, live streams, routing, dialogs, sign-in).
// Plain JavaScript, no build step. The pages are in front.js, plc.js, io.js, program.js and
// fleet.js, loaded after this file; boot() runs when they all are. Every DOM node is built with
// textContent (never innerHTML), so names and messages cannot inject markup.
'use strict';

// ───────────────────────────── Display: field display and theme ─────────────────────────────

/// The field display (a 10-inch touchscreen in the cabinet door) is set by the kiosk URL (?field)
/// or by this browser's setting (Settings, Display). The theme follows the system unless this
/// browser chose one. Storage can be blocked: every access is guarded.
const display = {
    fieldKey: 'simpleplc.display',
    themeKey: 'simpleplc.theme',
    get fromUrl() { return new URLSearchParams(location.search).has('field'); },
    get stored() {
        try { return localStorage.getItem(this.fieldKey) === 'field'; } catch { return false; }
    },
    get field() { return document.documentElement.dataset.display === 'field'; },
    get theme() {
        try { return localStorage.getItem(this.themeKey) || 'auto'; } catch { return 'auto'; }
    },
    apply() {
        const root = document.documentElement;
        if (this.fromUrl || this.stored) root.dataset.display = 'field';
        else delete root.dataset.display;
        const theme = this.theme;
        if (theme === 'light' || theme === 'dark') root.dataset.theme = theme;
        else delete root.dataset.theme;
    },
    setField(on) {
        try {
            if (on) localStorage.setItem(this.fieldKey, 'field');
            else localStorage.removeItem(this.fieldKey);
        } catch {}
        this.apply();
    },
    setTheme(theme) {
        try {
            if (theme === 'auto') localStorage.removeItem(this.themeKey);
            else localStorage.setItem(this.themeKey, theme);
        } catch {}
        this.apply();
    },
};
display.apply();
const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');

// ───────────────────────────── DOM helpers ─────────────────────────────

function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    setAttrs(el, attrs);
    append(el, children);
    return el;
}

/// An SVG element: attributes are set as they are (viewBox, x, y ...).
function svg(tag, attrs, ...children) {
    const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
    setAttrs(el, attrs);
    append(el, children);
    return el;
}

function setAttrs(el, attrs) {
    for (const [key, value] of Object.entries(attrs || {})) {
        if (value === undefined || value === null || value === false) continue;
        if (key === 'class') el.setAttribute('class', value);
        else if (key === 'dataset') Object.assign(el.dataset, value);
        else if (key.startsWith('on')) el.addEventListener(key.slice(2), value);
        else if (key === 'value' || key === 'checked') el[key] = value;
        else el.setAttribute(key, value === true ? '' : value);
    }
}

function append(el, children) {
    for (const child of children.flat(Infinity)) {
        if (child === null || child === undefined || child === false) continue;
        el.append(child instanceof Node ? child : String(child));
    }
    return el;
}

function clear(el) {
    while (el.firstChild) el.firstChild.remove();
    return el;
}

/// Replaces an element's children; null/false entries are skipped (replaceChildren would print them).
function fill(el, ...children) {
    return append(clear(el), children);
}

function setText(el, text) {
    if (el.textContent !== String(text)) el.textContent = text;
}

const enc = encodeURIComponent;
const NBSP = ' ';

/// A number and its unit: the unit in the secondary ink, joined by a non-breaking space.
function withUnit(value, unit) {
    return [String(value), NBSP, h('span', { class: 'unit' }, unit)];
}

/// A label/value list in the label column.
function dl(pairs, attrs) {
    return h('dl', { class: 'dl', ...(attrs || {}) }, pairs.filter(Boolean).map(([k, v]) => [h('dt', null, k), h('dd', null, v ?? '–')]));
}

/// A row of a .rows list: label (with an optional secondary line), body, end actions. A null label
/// leaves the label column empty (the page title names the row already).
function row(label, body, end, attrs) {
    const [title, note] = Array.isArray(label) ? label : [label, null];
    return h('section', { class: 'row', ...(attrs || {}) },
        title === null ? h('div', { class: 'row-label' }) : h('h2', { class: 'row-label' }, title, note ? h('span', { class: 'secondary' }, note) : null),
        h('div', { class: 'row-body' }, body),
        end ? h('div', { class: 'row-end' }, end) : null);
}

/// A section with a title and actions on the right.
function section(title, actions, ...body) {
    return h('section', { class: 'section' },
        h('div', { class: 'section-head' }, h('h2', { class: 'section-title' }, title),
            actions ? h('div', { class: 'actions' }, actions) : null),
        body);
}

/// Parses a number a person typed: "12", "-1.5", "1,5" (a decimal comma) or "2e3". Refuses what could
/// be read two ways ("1,000" is one thousand or 1.0), thousands separators, hex, words and empty text.
/// Returns { value } or { error }.
function parseNumber(text) {
    const s = String(text ?? '').trim();
    if (s === '') return { error: 'Enter a number' };
    if (/^[+-]?\d+(,\d{3}){2,}$/.test(s)) return { error: `Write “${s}” without thousands separators: ${s.replace(/,/g, '')}` };
    if (/^[+-]?[1-9]\d{0,2},\d{3}$/.test(s)) {
        return { error: `“${s}” can be read two ways: write ${s.replace(',', '')} for the whole number, or ${s.replace(',', '.')} with a point` };
    }
    if (!/^[+-]?(\d+([.,]\d*)?|[.,]\d+)([eE][+-]?\d+)?$/.test(s)) return { error: `“${s}” is not a number` };
    const value = Number(s.replace(',', '.'));
    if (!Number.isFinite(value)) return { error: `“${s}” is too large` };
    return { value };
}

/// A tag's substitute value as typed: empty (none), true, false or a number.
function parseSubstitute(text) {
    const s = String(text ?? '').trim();
    if (s === '') return { value: null };
    if (/^(true|false)$/i.test(s)) return { value: s.toLowerCase() === 'true' };
    const n = parseNumber(s);
    if (n.error) {
        return { error: /^[+-]?[\d.,]/.test(s) && !n.error.includes('is not a number') ? `Substitute value: ${n.error}.`
            : `Substitute value “${s}” is not true, false or a number. Write for example false, 0 or 21.5, or leave the field empty for no substitute value.` };
    }
    return n;
}

// ───────────────────────────── API ─────────────────────────────

class ApiError extends Error {
    constructor(status, body) {
        super(body && body.error ? body.error.message : `The server answered ${status}`);
        this.status = status;
        this.code = body && body.error ? body.error.code : '';
        this.details = (body && body.error && body.error.details) || {};
        this.problems = this.details.problems || [];
    }
}

/// Calls the REST API. opts.headers adds request headers (If-Match); opts.full resolves with
/// { data, headers } instead of the data alone.
async function api(method, path, body, opts = {}) {
    const options = { method, headers: { 'X-Requested-With': 'SimplePLC', ...(opts.headers || {}) }, credentials: 'same-origin' };
    if (body !== undefined) {
        if (typeof body === 'string') {
            options.headers['Content-Type'] = 'text/plain; charset=utf-8';
            options.body = body;
        } else {
            options.headers['Content-Type'] = 'application/json';
            options.body = JSON.stringify(body);
        }
    }
    let response;
    try {
        response = await fetch('/api/v1' + path, options);
    } catch (e) {
        throw new ApiError(0, { error: { message: 'SimplePLC is not reachable. Is it still running?' } });
    }
    const type = response.headers.get('Content-Type') || '';
    const data = type.includes('application/json') ? await response.json() : await response.text();
    if (response.status === 401 && !path.startsWith('/auth/')) {
        showSignIn({ expired: true });
        throw new ApiError(401, data);
    }
    if (!response.ok) throw new ApiError(response.status, typeof data === 'object' ? data : null);
    return opts.full ? { data, headers: response.headers } : data;
}

/// Whether someone is still signed in here; null when the server does not answer in time.
async function signedInUser(timeoutMs = 5000) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
        const r = await fetch('/api/v1/auth/state', { credentials: 'same-origin', signal: controller.signal });
        return r.ok ? { user: (await r.json()).user || false } : null;
    } catch {
        return null;
    } finally {
        clearTimeout(timer);
    }
}

/// Server-Sent Events with named handlers; closed when the view changes. The server sends an event
/// at least every `expectMs`: after three missed ones, or when the stream fails, the page says its
/// values are stale and reconnects (or asks to sign in again when the session ended).
function live(path, handlers, expectMs = 1000) {
    let source = null;
    let lastEvent = Date.now();
    let retry = null;
    let delay = 1000;
    let closed = false;
    const key = {};

    const fresh = () => {
        lastEvent = Date.now();
        delay = 1000;
        setStale(key, false);
    };
    function open() {
        source = new EventSource('/api/v1' + path);
        app.streams.add(source);
        for (const [name, fn] of Object.entries(handlers)) {
            source.addEventListener(name, event => {
                // A fleet says a device went silent with an event of its own: that is not fresh data
                if (name === 'offline') setStale(key, true);
                else fresh();
                try {
                    fn(JSON.parse(event.data));
                } catch (e) {
                    console.error(e);
                }
            });
        }
        source.addEventListener('error', () => {
            setStale(key, true);
            // CLOSED: the browser gave up (a 401 or an error status); CONNECTING: it retries by itself
            if (source.readyState === EventSource.CLOSED) reconnect();
        });
    }
    function close() {
        if (!source) return;
        source.close();
        app.streams.delete(source);
    }
    async function reconnect() {
        if (closed || retry) return;
        retry = true;
        close();
        const state = await signedInUser();
        if (closed) return;
        if (state && state.user === false) {
            showSignIn({ expired: true });
            return;
        }
        retry = setTimeout(() => {
            retry = null;
            lastEvent = Date.now();  // give the new connection its own three intervals
            if (!closed) open();
        }, delay);
        delay = Math.min(delay * 2, 10000);
    }
    const watchdog = setInterval(() => {
        if (Date.now() - lastEvent > 3 * expectMs) {
            setStale(key, true);
            reconnect();
        }
    }, 500);
    open();
    onLeave(() => {
        closed = true;
        clearInterval(watchdog);
        if (retry && retry !== true) clearTimeout(retry);
        close();
        setStale(key, false);
    });
}

/// Greys live values and shows "Connection lost" while any stream of the page is stale.
function setStale(key, stale) {
    if (stale) app.stale.add(key);
    else app.stale.delete(key);
    const on = app.stale.size > 0;
    document.body.classList.toggle('stale', on);
    if (app.staleBanner) app.staleBanner.hidden = !on;
    for (const fn of app.staleListeners) fn(on);
}

// ───────────────────────────── App state and routing ─────────────────────────────

// mode: 'device' (one PLC, this process) or 'fleet' (the PLCs of many devices)
const app = {
    user: null, version: '', mode: 'device', leave: [], guard: null, ignoreHash: false, previousHash: '#/',
    routing: false, signingIn: false, stale: new Set(), staleBanner: null, staleListeners: new Set(),
    unsaved: null,  // () => name of a file with unsaved edits, or null (set by the Program tab)
    plcId: null,    // a device's own PLC, once known
    // The generation of the page being shown. A view takes it as its first statement and, after every
    // await, stops when it changed: the address moved on while the view waited for the server, and what
    // the view would register (streams, pollers, the title) belongs to the page that replaced it.
    routeSeq: 0,
    streams: new Set(),  // the open live streams (the tests count them: a left view keeps none)
};

/// Program text not saved yet, per PLC and program: in memory and in sessionStorage, so a sign-out,
/// an expired session or a reload of this tab does not lose it.
const drafts = {
    memory: new Map(),
    key: (plc, program) => `simpleplc.draft.${plc}/${program}`,
    get(plc, program) {
        const key = this.key(plc, program);
        if (this.memory.has(key)) return this.memory.get(key);
        try {
            const stored = sessionStorage.getItem(key);
            return stored ? JSON.parse(stored) : null;
        } catch {
            return null;
        }
    },
    set(plc, program, draft) {
        const key = this.key(plc, program);
        this.memory.set(key, draft);
        try {
            sessionStorage.setItem(key, JSON.stringify(draft));
        } catch {}  // full or blocked: memory still has it
    },
    clear(plc, program) {
        const key = this.key(plc, program);
        this.memory.delete(key);
        try {
            sessionStorage.removeItem(key);
        } catch {}
    },
    any() {
        if (this.memory.size) return true;
        try {
            for (let i = 0; i < sessionStorage.length; i++) if (sessionStorage.key(i).startsWith('simpleplc.draft.')) return true;
        } catch {}
        return false;
    },
};

function onLeave(fn) {
    app.leave.push(fn);
}

function isAdmin() {
    return app.user && app.user.role === 'admin';
}

async function boot() {
    try {
        const state = await api('GET', '/auth/state');
        app.version = state.version;
        app.mode = state.mode;
        if (state.needs_setup) return renderSetup();
        if (!state.user) return renderSignIn();
        app.user = state.user;
        startRouting();
    } catch (e) {
        fill(document.getElementById('app'), h('main', { class: 'page' }, h('p', { class: 'problems', role: 'alert' }, e.message)));
    }
}

/// After sign-in: show the page of the current address (a second sign-in must not add a second listener).
function startRouting() {
    app.signingIn = false;
    if (!app.routing) {
        window.addEventListener('hashchange', route);
        app.routing = true;
    }
    route();
}

/// The address: path parts and the query after "?" (#/help/reference?q=F_DIV_ZERO).
function parseHash() {
    const [path, query] = (location.hash.slice(1) || '/').split('?');
    return { parts: path.split('/').filter(Boolean).map(decodeURIComponent), query: new URLSearchParams(query || '') };
}

/// A device's own PLC (one process, one PLC).
async function ownPlcId() {
    if (app.plcId) return app.plcId;
    const plcs = await api('GET', '/plcs');
    if (!plcs.length) throw new Error('This SimplePLC has no PLC.');
    app.plcId = plcs[0].id;
    return app.plcId;
}

const TABS = ['overview', 'program', 'io', 'communication', 'diagnostics', 'settings'];

async function route() {
    if (app.ignoreHash) {
        app.ignoreHash = false;
        return;
    }
    if (!app.user) return;  // on the sign-in form: signing in shows the page of the address
    if (app.guard && !(await app.guard())) {
        app.ignoreHash = true;
        location.hash = app.previousHash;
        return;
    }
    app.guard = null;
    // A new page: what the one before registered goes, and a view of it still waiting for the server
    // sees the generation move on and stops
    const seq = ++app.routeSeq;
    for (const fn of app.leave.splice(0)) fn();
    for (const d of document.querySelectorAll('dialog[open]')) d.close();
    app.previousHash = location.hash || '#/';

    const { parts, query } = parseHash();
    try {
        const fleet = app.mode === 'fleet';
        // Home is the fleet, or the one PLC of this device
        if (parts.length === 0) {
            if (fleet) return await viewFleet();
            const own = await ownPlcId();
            if (seq !== app.routeSeq) return;
            return location.replace(`#/plc/${enc(own)}/overview`);
        }
        if (parts[0] === 'plc' && parts[1]) {
            const tab = parts[2] || 'overview';
            if (!TABS.includes(tab)) return location.replace(`#/plc/${enc(parts[1])}/overview`);
            return await viewPlc(parts[1], tab, parts[3] || null, query);
        }
        if (parts[0] === 'access') {
            // Users and tokens: a page of the fleet; on a device they are in Settings
            if (fleet) return await viewAccess();
            const own = await ownPlcId();
            if (seq !== app.routeSeq) return;
            return location.replace(`#/plc/${enc(own)}/settings`);
        }
        if (parts[0] === 'help') {
            if (parts[1] === 'reference') return await viewReference(query);
            return await viewHelp();
        }
        location.replace('#/');
    } catch (e) {
        if (e.status === 401 || seq !== app.routeSeq) return;
        const main = shell('');
        main.append(h('div', { class: 'empty' },
            h('p', { class: 'empty-title' }, 'This page could not be shown'),
            h('p', { class: 'problems', role: 'alert' }, e.message),
            h('div', { class: 'actions' }, h('a', { class: 'btn', href: '#/' }, app.mode === 'fleet' ? 'Back to the fleet' : 'Back to the PLC'))));
    }
}

/// The page frame: top bar, the connection banner and the content column.
function shell(active) {
    const main = h('main', { class: 'page' });
    app.staleBanner = h('div', { class: 'banner', role: 'alert', hidden: app.stale.size === 0 },
        h('span', { class: 'banner-inner' }, h('strong', null, 'Connection lost.'), ' The values shown are the last ones received. Reconnecting…'));
    fill(document.getElementById('app'), topbar(active), app.staleBanner, main);
    delete document.body.dataset.state;  // set by a PLC page while its PLC is offline
    window.scrollTo(0, 0);
    return main;
}

function topbar(active) {
    return h('header', { class: 'topbar' },
        h('a', { class: 'wordmark', href: '#/' }, 'SimplePLC'),
        app.mode === 'fleet' ? h('nav', { class: 'topnav', 'aria-label': 'Main' },
            h('a', { href: '#/', 'aria-current': active === 'fleet' ? 'page' : null }, 'Fleet')) : null,
        h('div', { class: 'end' },
            h('a', { href: '#/help', 'aria-current': active === 'help' ? 'page' : null }, 'Help'),
            app.user ? accountMenu() : null));
}

/// The user's name opens the account menu: who is signed in, users and tokens, sign out.
function accountMenu() {
    const settings = app.mode === 'fleet' ? '#/access' : app.plcId ? `#/plc/${enc(app.plcId)}/settings` : '#/access';
    const menu = h('div', { class: 'menu', role: 'menu', hidden: true },
        h('p', null, `Signed in as ${app.user.name}, ${isAdmin() ? 'administrator' : 'viewer'}`),
        h('a', { href: settings, role: 'menuitem' }, isAdmin() ? 'Users and tokens' : 'Your password'),
        h('a', { href: settings, role: 'menuitem' }, 'Display'),
        h('a', { href: '#/help', role: 'menuitem' }, 'Help'),
        h('hr'),
        h('button', { type: 'button', role: 'menuitem', onclick: () => { menu.hidden = true; signOut(); } }, 'Sign out'));
    const button = h('button', { type: 'button', 'aria-haspopup': 'true', 'aria-expanded': 'false', class: 'account-button' }, app.user.name);
    button.addEventListener('click', e => {
        e.stopPropagation();
        menu.hidden = !menu.hidden;
        button.setAttribute('aria-expanded', String(!menu.hidden));
        if (!menu.hidden) {
            const first = menu.querySelector('a, button');
            if (first) first.focus();
        }
    });
    menu.addEventListener('click', e => {
        if (e.target.closest('a')) menu.hidden = true;
    });
    return h('div', { class: 'account' }, button, menu);
}

/// Open menus close on a click elsewhere and on Escape.
document.addEventListener('click', e => {
    for (const menu of document.querySelectorAll('.account .menu:not([hidden])')) {
        if (!menu.parentElement.contains(e.target)) {
            menu.hidden = true;
            menu.parentElement.querySelector('button').setAttribute('aria-expanded', 'false');
        }
    }
});
document.addEventListener('keydown', e => {
    if (e.key !== 'Escape') return;
    for (const menu of document.querySelectorAll('.account .menu:not([hidden])')) {
        menu.hidden = true;
        const button = menu.parentElement.querySelector('button');
        button.setAttribute('aria-expanded', 'false');
        button.focus();
    }
});

function setTitle(text) {
    document.title = text ? `${text} – SimplePLC` : 'SimplePLC';
}

async function signOut() {
    const unsaved = app.unsaved && app.unsaved();
    if (unsaved && !(await confirmDialog('Sign out?',
        `Your changes to ${unsaved} are not downloaded. They stay in this browser tab and come back when you sign in again here.`, 'Sign out'))) return;
    await api('POST', '/auth/logout').catch(() => {});
    // Signing in again leads home, or back to the program whose unsaved text is waiting there
    if (!unsaved) history.replaceState(null, '', '#/');
    showSignIn();
}

/// The session ended (sign-out, 401, expired): the pages close (the Program tab keeps its unsaved
/// text as a draft) and the sign-in form takes their place. Signing in returns to the same page.
function showSignIn({ expired = false } = {}) {
    if (app.signingIn) return;  // several requests can fail with 401 at once
    app.signingIn = true;
    app.guard = null;
    app.routeSeq++;  // a view still waiting for the server stops instead of drawing over the form
    for (const fn of app.leave.splice(0)) fn();
    app.unsaved = null;
    app.user = null;
    for (const d of document.querySelectorAll('dialog[open]')) d.close();
    const notes = [];
    if (expired) notes.push('Your session ended. Sign in again.');
    if (drafts.any()) notes.push('Your unsaved program changes are kept and come back after you sign in.');
    renderSignIn(notes.join(' '));
}

// ───────────────────────────── Feedback and dialogs ─────────────────────────────

/// A short note at the foot of the window, for what needs no answer (Copied, a failed request).
function toast(message, kind) {
    const el = h('div', { class: `toast ${kind || ''}` }, message);
    // One note at a time: the newest replaces the last
    fill(document.getElementById('toasts'), el);
    setTimeout(() => el.remove(), kind === 'error' ? 7000 : 3500);
}

function showProblems(box, error) {
    clear(box);
    if (!error) return;
    if (error.problems && error.problems.length > 1) {
        box.append(h('strong', null, 'Please fix:'), h('ul', null, error.problems.map(p => h('li', null, p))));
    } else {
        box.append(error.message);
    }
}

/// A modal dialog; `build(close)` returns its form. Resolves with what close() is given (undefined
/// for Escape or a click on the scrim). `focus` selects the element that gets the focus first
/// (dangerous actions: Cancel, so Enter does no harm).
function dialog(title, build, { focus, wide = false } = {}) {
    return new Promise(resolve => {
        let result;
        const el = h('dialog', { class: `dlg${wide ? ' dlg-wide' : ''}`, 'aria-label': title });
        const close = value => {
            result = value;
            el.close();
        };
        el.addEventListener('close', () => {
            el.remove();
            resolve(result);
        });
        el.addEventListener('click', e => {
            if (e.target === el) el.close();  // the scrim, outside the form
        });
        el.append(build(close));
        document.body.append(el);
        el.showModal();
        const first = (focus && el.querySelector(focus)) ||
            el.querySelector('input:not([type=checkbox]):not([type=radio]):not([disabled]), select, textarea, .btn-primary:not([disabled])');
        if (first) first.focus();
    });
}

/// The parts of a dialog: title, lead sentence, body, actions (Cancel first).
function dialogForm({ title, lead, body, actions, onsubmit, className }) {
    return h('form', { method: 'dialog', class: `dlg-form${className ? ` ${className}` : ''}`, onsubmit },
        h('h2', null, title),
        lead ? (typeof lead === 'string' ? h('p', { class: 'lead muted' }, lead) : h('div', { class: 'lead' }, lead)) : null,
        body ? h('div', { class: 'body' }, body) : null,
        h('div', { class: 'actions' }, actions));
}

/// Asks before an action. `message` is text or nodes. A dangerous action starts with the focus on
/// Cancel, so Enter does no harm; its button repeats the verb and its object.
function confirmDialog(title, message, action, danger) {
    return dialog(title, close => dialogForm({
        title,
        lead: typeof message === 'string' ? message : null,
        body: typeof message === 'string' ? null : message,
        onsubmit: e => { e.preventDefault(); close(true); },
        actions: [
            h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, action)],
    }), danger ? { focus: '[data-cancel]' } : {});
}

/// Someone else changed what this page edits (HTTP 412): 'reload' theirs, 'overwrite' it, or 'keep' editing.
async function conflictDialog(title, message) {
    const choice = await dialog(title, close => dialogForm({
        title,
        lead: message,
        onsubmit: e => { e.preventDefault(); close('keep'); },
        actions: [
            h('button', { type: 'button', class: 'btn start', onclick: () => close('reload') }, 'Reload theirs'),
            h('button', { type: 'button', class: 'btn', onclick: () => close('overwrite') }, 'Overwrite'),
            h('button', { type: 'submit', class: 'btn btn-primary', 'data-keep': '' }, 'Keep editing')],
    }), { focus: '[data-keep]' });
    return choice || 'keep';
}

/// A configuration change, sent with If-Match: "<config_version>", the version the page started
/// from. When someone else changed the configuration since (412), asks what to do.
/// Resolves with the answer, or null when nothing was sent ('reload' calls onReload).
async function configRequest(method, path, body, version, { what = 'The configuration', onReload } = {}) {
    const headers = version !== undefined && version !== null ? { 'If-Match': `"${version}"` } : {};
    try {
        return await api(method, path, body, { headers });
    } catch (err) {
        if (err.status !== 412) throw err;
        const choice = await conflictDialog('Changed by someone else',
            `${what} of this PLC changed after you opened this page (another browser, an agent or the API). Reload theirs, overwrite it with yours, or keep editing.`);
        if (choice === 'overwrite') return api(method, path, body);
        if (choice === 'reload' && onReload) onReload();
        return null;
    }
}

async function copy(text) {
    try {
        if (!window.isSecureContext || !navigator.clipboard) throw new Error('no clipboard API');
        await navigator.clipboard.writeText(text);
        toast('Copied');
        return true;
    } catch {}
    // Plain HTTP on the LAN (how a Raspberry Pi is reached) has no clipboard API: copy a selection.
    // Inside a modal dialog the rest of the page is inert, so the helper goes into that dialog.
    const host = document.querySelector('dialog[open]') || document.body;
    const area = h('textarea', { readonly: true, 'aria-hidden': 'true', class: 'copy-buffer' }, text);
    host.append(area);
    const focused = document.activeElement;
    area.focus();
    area.select();
    let ok = false;
    try {
        ok = document.execCommand('copy');
    } catch {}
    area.remove();
    if (focused && focused.focus) focused.focus();
    toast(ok ? 'Copied' : 'Copy is not available here; select the text instead', ok ? '' : 'error');
    return ok;
}

/// Saves text as a file (a register map, the diagnostics buffer).
function download(name, text, type = 'text/csv') {
    const url = URL.createObjectURL(new Blob([text], { type: `${type};charset=utf-8` }));
    const a = h('a', { href: url, download: name, hidden: true });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
}

function csvLine(cells) {
    return cells.map(c => {
        const s = c === null || c === undefined ? '' : String(c);
        return /[",\n\r]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
    }).join(',');
}

// ───────────────────────────── Formatting ─────────────────────────────

/// JSON has no NaN or infinity: the server sends null for them (a string where it names them).
function isNonFinite(type, value) {
    return type === 'float32' && (typeof value !== 'number' || !Number.isFinite(value));
}

function nonFiniteText(value) {
    if (value === Infinity || /^\+?inf/i.test(String(value))) return '∞';
    if (value === -Infinity || /^-inf/i.test(String(value))) return '−∞';
    return 'NaN';
}

/// A process value as the panel shows it: TRUE and FALSE, never On or High.
function formatValue(type, value) {
    if (type === 'bool') return value ? 'TRUE' : 'FALSE';
    if (type === 'float32') {
        if (isNonFinite(type, value)) return nonFiniteText(value);
        if (Number.isInteger(value)) return value.toFixed(1);
        if (Math.abs(value) < 0.001) return String(Number(value.toPrecision(3)));  // 0.0004 or 2.4e-41, never "0"
        return String(Math.round(value * 1000) / 1000);
    }
    return value === null || value === undefined ? '–' : String(value);
}

/// The float32 in two registers, high word first: tells NaN from +∞ and −∞ where JSON gave null.
function floatFromWords(high, low) {
    const view = new DataView(new ArrayBuffer(4));
    view.setUint16(0, high & 0xffff);
    view.setUint16(2, low & 0xffff);
    return view.getFloat32(0);
}

/// Two registers as a 32-bit value in a word order: 'abcd' high word first, 'cdab' low word first.
function decode32(words, order, type) {
    const [a, b] = order === 'cdab' ? [words[1], words[0]] : [words[0], words[1]];
    const view = new DataView(new ArrayBuffer(4));
    view.setUint16(0, a & 0xffff);
    view.setUint16(2, b & 0xffff);
    if (type === 'float32') return view.getFloat32(0);
    if (type === 'int32') return view.getInt32(0);
    return view.getUint32(0);
}

/// A decoded number for reading: floats with up to 4 significant decimals, huge or tiny ones in exponent form.
function formatReading(type, v) {
    if (type === 'bool') return v ? 'TRUE' : 'FALSE';
    if (type !== 'float32') return String(v);
    if (!Number.isFinite(v)) return nonFiniteText(v);
    if (v === 0) return '0.0';
    const a = Math.abs(v);
    if (a >= 1e6 || a < 1e-3) return v.toExponential(2).replace(/-/g, '−');
    return String(Number(v.toPrecision(6)));
}

/// [value, unit] of a duration in microseconds.
function usParts(v) {
    if (v === undefined || v === null) return null;
    if (v < 1000) return [String(Math.round(v)), 'µs'];
    const ms = v / 1000;
    return [ms < 10 ? ms.toFixed(1) : String(Math.round(ms)), 'ms'];
}

function formatUs(v) {
    const p = usParts(v);
    return p ? `${p[0]}${NBSP}${p[1]}` : '–';
}

function formatAge(ms) {
    const s = Math.round(ms / 1000);
    if (s < 60) return `${s}${NBSP}s`;
    if (s < 3600) return `${Math.floor(s / 60)}${NBSP}min`;
    if (s < 86400) return `${Math.floor(s / 3600)}${NBSP}h ${Math.floor((s % 3600) / 60)}${NBSP}min`;
    const d = Math.floor(s / 86400);
    return `${d} day${d === 1 ? '' : 's'} ${Math.floor((s % 86400) / 3600)} h`;
}

const pad2 = n => String(n).padStart(2, '0');
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/// 14:02:31
function formatTime(ms) {
    if (!ms) return '';
    const d = new Date(ms);
    return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

/// 14:02:31 today, otherwise "24 October 14:02".
function formatWhen(ms) {
    if (!ms) return '';
    const d = new Date(ms);
    const today = new Date();
    if (d.toDateString() === today.toDateString()) return formatTime(ms);
    return `${d.getDate()} ${MONTHS[d.getMonth()]} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}

function formatDateTime(ms) {
    if (!ms) return 'never';
    const d = new Date(ms);
    return `${d.getDate()} ${MONTHS[d.getMonth()]} ${d.getFullYear()}, ${formatTime(ms)}`;
}

function formatKb(kb) {
    if (kb === null || kb === undefined) return '–';
    if (kb >= 1024 * 1024) return `${(kb / 1024 / 1024).toFixed(1)}${NBSP}GB`;
    if (kb >= 1024) return `${Math.round(kb / 1024)}${NBSP}MB`;
    return `${kb}${NBSP}kB`;
}

function plural(n, one, many) {
    return `${n.toLocaleString('en-US')} ${n === 1 ? one : many || `${one}s`}`;
}

/// Who made a change: a user, a token ("Claude Code, token laptop") or the system.
function byText(by) {
    if (!by) return '';
    if (typeof by === 'string') return by;
    if (by.kind === 'system' || !by.name) return 'system';
    if (by.kind === 'token') return `token ${by.name}`;
    return by.name;
}

/// Where a PLC's server is reached: its listen address, or for "every interface" the PLC's device
/// (in a fleet: the device's address on the plant network; on a device: this page's host).
function hostFor(listen, plc) {
    if (listen && listen !== '0.0.0.0' && listen !== '::') return listen;
    if (plc && plc.device && plc.device.url) return new URL(plc.device.url).hostname;
    return location.hostname;
}

function modbusEndpoint(plc) {
    return plc.modbus && plc.modbus.listening ? `${hostFor(plc.modbus.listen, plc)}:${plc.modbus.port}` : null;
}

function opcuaEndpoint(plc) {
    return plc.opcua && plc.opcua.listening ? `opc.tcp://${hostFor(plc.opcua.listen, plc)}:${plc.opcua.port}` : null;
}

/// The panel's own address, as an agent or a script reaches it.
function panelBase(plc) {
    if (plc && plc.device && plc.device.url) return plc.device.url.replace(/\/$/, '');
    return `${location.protocol}//${location.host}`;
}

const AREAS = ['coil', 'discrete_input', 'holding_register', 'input_register'];
const REGISTER_TYPES = ['uint16', 'int16', 'uint32', 'int32', 'float32'];
const ST_TYPE = { bool: 'BOOL', uint16: 'UINT', int16: 'INT', uint32: 'UDINT', int32: 'DINT', float32: 'REAL' };
const TAG_TYPE_OF_ST = { BOOL: 'bool', UINT: 'uint16', WORD: 'uint16', INT: 'int16', UDINT: 'uint32', DWORD: 'uint32', DINT: 'int32', REAL: 'float32' };

function areaLabel(area, count = 1) {
    const one = { coil: 'Coil', discrete_input: 'Discrete input', holding_register: 'Holding register', input_register: 'Input register' }[area] || area;
    return count === 1 ? one : `${one}s`;
}

function tagWidth(type) {
    return ['uint32', 'int32', 'float32'].includes(type) ? 2 : 1;
}

function tagAddress(tag) {
    return tagWidth(tag.type) === 2 ? `${tag.address}–${tag.address + 1}` : String(tag.address);
}

/// "Holding register 0", "Input registers 0–1": the area in full and the 0-based address.
function addressText(area, address, width = 1) {
    return width === 2 ? `${areaLabel(area, 2)} ${address}–${address + 1}` : `${areaLabel(area)} ${address}`;
}

/// The 1-based Modicon number of an entry: Coil 0 is 00001, Discrete input 0 is 10001,
/// Input register 0 is 30001, Holding register 0 is 40001 (6 digits past 9999).
function modicon(area, address) {
    const prefix = { coil: '0', discrete_input: '1', input_register: '3', holding_register: '4' }[area];
    const n = address + 1;
    return `${prefix}${String(n).padStart(n > 9999 ? 5 : 4, '0')}`;
}

function modiconText(area, address, width = 1) {
    return width === 2 ? `${modicon(area, address)}–${modicon(area, address + 1)}` : modicon(area, address);
}

/// The tags (other than `except`) that share an entry with `width` entries at `address` of `area`.
function overlappingTags(tags, area, address, width, except) {
    return tags.filter(t => t.name !== except && t.area === area && t.address < address + width && address < t.address + tagWidth(t.type));
}

/// The lowest address of `area` where `width` entries are free of tags, or null when none is.
function freeAddress(tags, area, width, imageSize, except) {
    const used = tags.filter(t => t.area === area && t.name !== except)
        .map(t => [t.address, t.address + tagWidth(t.type)]).sort((a, b) => a[0] - b[0]);
    let address = 0;
    for (const [start, end] of used) {
        if (address + width <= start) break;
        address = Math.max(address, end);
    }
    return address + width <= imageSize ? address : null;
}

/// The name a PLC goes by in a confirmation: its name, with its id when they differ.
function plcLabel(plc) {
    return plc.name || plc.id;
}

// ───────────────────────────── Sign in and setup ─────────────────────────────

function gate(step, title, formChildren, onSubmit, { lead } = {}) {
    const problems = h('div', { class: 'problems', role: 'alert' });
    const form = h('form', {
        class: 'setup',
        onsubmit: async e => {
            e.preventDefault();
            const button = form.querySelector('button[type=submit]');
            button.disabled = true;
            try {
                await onSubmit(new FormData(form));
            } catch (err) {
                showProblems(problems, err);
                button.disabled = false;
            }
        },
    },
        h('div', null, step ? h('p', { class: 'step' }, step) : null, h('h1', { class: 'page-title' }, title)),
        lead || null,
        problems,
        formChildren);
    fill(document.getElementById('app'),
        h('header', { class: 'topbar' }, h('span', { class: 'wordmark' }, 'SimplePLC')),
        h('main', { class: 'page' }, form));
    const first = form.querySelector('input');
    if (first) first.focus();
}

function field(id, label, input, hint) {
    return h('div', { class: 'field' }, h('label', { for: id }, label), input, hint ? h('p', { class: 'hint' }, hint) : null);
}

function renderSignIn(note) {
    setTitle('Sign in');
    gate(location.host, 'Sign in', [
        note ? h('p', { class: 'muted', role: 'status' }, note) : null,
        field('user', 'User name', h('input', { class: 'input', id: 'user', name: 'username', type: 'text', autocomplete: 'username', required: true })),
        field('pw', 'Password', h('input', { class: 'input', id: 'pw', name: 'password', type: 'password', autocomplete: 'current-password', required: true })),
        h('div', { class: 'actions' }, h('button', { type: 'submit', class: 'btn btn-primary' }, 'Sign in')),
    ], async data => {
        const result = await api('POST', '/auth/login', { username: data.get('username'), password: data.get('password') });
        app.user = result.user;
        startRouting();
    });
}

const SETUP_DONE = 'SimplePLC is set up already (in another tab or by someone else). Sign in with the account it created.';
const what = () => (app.mode === 'fleet' ? 'this fleet' : 'this PLC');

function whereTheCodeIs() {
    return ['Printed where SimplePLC started: its terminal, ', h('code', null, 'journalctl -u simpleplc'),
        ' on a Raspberry Pi, or ', h('code', null, 'docker logs <container>'), ' in a container.'];
}

/// First run, step 1 of 2: only the setup code. The server checks it (POST /auth/setup/check)
/// before the account form appears, so a wrong code is refused before a password is typed.
function renderSetup(code = '') {
    setTitle('Set up');
    gate(`${location.host}, step 1 of 2`, `Set up ${what()}`, [
        field('code', 'Setup code',
            h('input', { class: 'input input-code', id: 'code', name: 'code', type: 'text', autocomplete: 'off', spellcheck: 'false', autocapitalize: 'off', value: code, required: true }),
            whereTheCodeIs()),
        h('div', { class: 'actions' }, h('button', { type: 'submit', class: 'btn btn-primary' }, 'Continue')),
    ], async data => {
        const entered = data.get('code').trim();
        try {
            await api('POST', '/auth/setup/check', { code: entered });
        } catch (err) {
            if (err.status === 409) return renderSignIn(SETUP_DONE);
            throw err;  // a wrong code (403): the server says why
        }
        renderAccountStep(entered);
    });
}

function accountFields() {
    return [
        field('user', 'User name', h('input', { class: 'input', id: 'user', name: 'username', type: 'text', autocomplete: 'username', value: 'admin', required: true })),
        field('pw', 'Password', h('input', { class: 'input', id: 'pw', name: 'password', type: 'password', autocomplete: 'new-password', minlength: 8, required: true }),
            'At least 8 characters.'),
    ];
}

/// First run, step 2 of 2: the administrator account, for a code the server has accepted.
function renderAccountStep(code) {
    setTitle('Set up');
    gate(`${location.host}, step 2 of 2`, 'Create the administrator', [
        accountFields(),
        h('div', { class: 'actions' },
            h('button', { type: 'button', class: 'btn', onclick: () => renderSetup(code) }, 'Back'),
            h('button', { type: 'submit', class: 'btn btn-primary' }, 'Create administrator')),
    ], data => createAccount(code, data), {
        lead: h('p', { class: 'muted' }, 'The code is accepted. This account can change programs, values and settings. Add more users and agents later in Settings.'),
    });
}

async function createAccount(code, data) {
    let result;
    try {
        result = await api('POST', '/auth/setup', { setup_code: code, username: data.get('username'), password: data.get('password') });
    } catch (err) {
        // Set up in another tab (or by someone else) meanwhile: this tab signs in instead
        const state = await api('GET', '/auth/state').catch(() => null);
        if (state && !state.needs_setup) {
            renderSignIn(SETUP_DONE);
            return;
        }
        throw err;
    }
    app.user = result.user;
    startRouting();
}

// The pages are defined by the files loaded after this one: start when the document is complete
document.addEventListener('DOMContentLoaded', boot);
