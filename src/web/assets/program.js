// Program: the Structured Text editor with problems inline, the Watch pane, Download with a
// preview of the online change, Check, Import of .st files, examples, history and the listing
// "as compiled", and the Create tag quick fix.
'use strict';

/// Source text as the editor holds it: CodeMirror turns "\r\n" into "\n", so a CRLF file would
/// otherwise always look unsaved.
function normalizeSource(text) {
    return String(text).replace(/\r\n?/g, '\n');
}

/// The first and last line (1-based, of `after`) where two texts differ, or null when equal.
function changedRange(before, after) {
    if (before === after) return null;
    const a = before.split('\n');
    const b = after.split('\n');
    let first = 0;
    while (first < a.length && first < b.length && a[first] === b[first]) first++;
    let endA = a.length - 1;
    let endB = b.length - 1;
    while (endA >= first && endB >= first && a[endA] === b[endB]) { endA--; endB--; }
    return [first + 1, Math.max(first + 1, endB + 1)];
}

function rangeWords(range) {
    if (!range) return 'no change';
    return range[0] === range[1] ? `line ${range[0]} changed` : `lines ${range[0]} to ${range[1]} changed`;
}

/// The type a name is declared with near `line` (1-based): "lamp : BOOL;" or "a, b : INT;".
function declaredType(source, line, name) {
    const lines = source.split('\n').slice(Math.max(0, line - 1), line + 5).join('\n');
    const at = lines.search(new RegExp(`\\b${name}\\b`, 'i'));
    if (at < 0) return null;
    const m = /:\s*([A-Za-z_][A-Za-z0-9_]*)/.exec(lines.slice(at));
    return m ? m[1].toUpperCase() : null;
}

async function tabProgram(ctx) {
    const plc = ctx.plc;
    const id = plc.id;
    const tasks = plc.tasks || [];
    let programs = await api('GET', `/plcs/${enc(id)}/programs`).catch(() => []);
    if (ctx.seq !== app.routeSeq) return null;  // the page moved on while the list was on its way
    const fromTask = tasks[0] ? tasks[0].program : programs[0] ? programs[0].name : 'main';
    let current = fromTask;
    const file = () => `${current}.st`;
    const taskOf = name => (ctx.plc.tasks || []).find(t => t.program.toLowerCase() === name.toLowerCase()) || null;
    const mac = /Mac|iPhone|iPad/.test(navigator.platform || '');

    const filesNav = h('nav', { class: 'files', 'aria-label': 'Files' });
    const dirtyMark = h('span', { class: 'status dirty', hidden: true }, 'Changed, not downloaded');
    const problemCount = h('span', { class: 'status problem-count' });
    const notice = h('div', { class: 'problem is-block notice', role: 'alert', hidden: true });
    const missingBox = h('div', { class: 'problem is-block missing-tags', hidden: true });
    const otherProblems = h('div', { class: 'other-problems' });
    const editorBox = h('div', { class: 'editor' });
    const message = h('span', { class: 'message', role: 'status' });
    const taskInfo = h('span', { class: 'task-info' });
    const cursorInfo = h('span', { class: 'hide-sm cursor-info' });
    const fileInput = h('input', { type: 'file', accept: '.st,text/plain', multiple: true, hidden: true, 'aria-label': 'Import .st files' });
    const downloadButton = h('button', { class: 'btn btn-primary', type: 'button', disabled: !isAdmin() }, 'Download…');
    const watch = watchPane(ctx, () => taskOf(current) || (ctx.plc.tasks || [])[0] || null, () => editor.getValue());
    const programGrid = h('div', { class: 'program' }, editorBox, watch.el);

    const el = h('div', { class: 'program-page' },
        h('div', { class: 'toolbar' },
            filesNav, dirtyMark, problemCount,
            h('div', { class: 'actions' },
                isAdmin() ? h('button', { class: 'btn btn-quiet hide-sm', type: 'button', onclick: () => fileInput.click() }, 'Import…') : null,
                isAdmin() ? h('button', { class: 'btn btn-quiet hide-sm', type: 'button', onclick: () => examplesDialog() }, 'Examples') : null,
                h('button', { class: 'btn btn-quiet hide-sm', type: 'button', onclick: () => historyDialog() }, 'History'),
                h('button', { class: 'btn btn-quiet hide-sm', type: 'button', onclick: () => compiledDialog() }, 'As compiled'),
                h('button', { class: 'btn btn-quiet', type: 'button', onclick: () => check(true) }, 'Check'),
                downloadButton, fileInput)),
        notice, missingBox, otherProblems,
        programGrid,
        h('div', { class: 'statusbar' }, taskInfo, message, cursorInfo),
        h('p', { class: 'editor-hint secondary' },
            `${mac ? '⌘S' : 'Ctrl+S'} downloads, ${mac ? '⌘/' : 'Ctrl+/'} comments lines, Tab indents. To leave the editor with the keyboard, press Esc, then Tab. Drop .st files on the editor to import them.`));

    let escaped = false;  // Esc, then Tab: the browser moves the focus on (no keyboard trap, WCAG 2.1.2)
    const editor = CodeMirror(editorBox, {
        mode: 'st', lineNumbers: true, lineWrapping: true, indentUnit: 4, tabSize: 4, indentWithTabs: false,
        matchBrackets: true, autoCloseBrackets: true, styleActiveLine: true, viewportMargin: Infinity,
        readOnly: !isAdmin(),
        extraKeys: {
            'Ctrl-S': () => downloadDialog(), 'Cmd-S': () => downloadDialog(),
            'Ctrl-/': 'toggleComment', 'Cmd-/': 'toggleComment',
            Tab: cm => (cm.somethingSelected() ? cm.indentSelection('add') : cm.replaceSelection('    ', 'end')),
            'Shift-Tab': cm => cm.indentSelection('subtract'),
        },
    });
    editor.on('keydown', (cm, e) => {
        if (e.key === 'Tab' && escaped) {
            e.codemirrorIgnore = true;
            escaped = false;
            return;
        }
        if (e.key !== 'Shift') escaped = e.key === 'Escape';
    });
    editor.getInputField().setAttribute('aria-label', 'Program source. Press Escape, then Tab, to leave the editor.');
    editor.on('cursorActivity', () => {
        const c = editor.getCursor();
        setText(cursorInfo, `Line ${c.line + 1}, column ${c.ch + 1}`);
    });

    let savedText = '';
    let etag = null;          // ETag of the version the edits start from: sent as If-Match
    let latestEtag = null;    // ETag of the newest version this page has seen
    let stamp = null;         // modified/size of the saved file: a change means someone saved it
    let theirs = null;        // { text, etag } saved by someone else while this page has edits
    let loadedAt = null;      // the task's loaded_at: it changes when a version is loaded
    let lastMessage = null;   // the result of the last action: it stays until an edit
    let shownFault = null;    // the fault (by time) whose line is marked
    let exampleTags = null;   // the tags of the example opened in the editor: offered after Check
    let discarded = false;    // the person left without saving: no draft is kept
    let left = false;         // the tab is closed: a late timer must not write a draft
    let diagnostics = null;   // the last check's diagnostics
    let checkTimer = null;
    let checkSeq = 0;

    const isDirty = () => editor.getValue() !== savedText;
    let draftTimer = null;
    /// Unsaved text is kept per program, so a sign-out or an expired session does not lose it.
    function keepDraft() {
        clearTimeout(draftTimer);
        draftTimer = null;
        if (left || !isAdmin() || !app.user) return;
        if (isDirty()) drafts.set(id, current, { text: editor.getValue(), etag, base: savedText, user: app.user.name, at: Date.now() });
        else drafts.clear(id, current);
    }
    let changedLines = [];
    function markChanged() {
        for (const l of changedLines) editor.removeLineClass(l, 'gutter', 'is-changed');
        changedLines = [];
        const range = changedRange(savedText, editor.getValue());
        if (!range) return;
        for (let l = range[0] - 1; l <= range[1] - 1 && l < editor.lineCount(); l++) {
            editor.addLineClass(l, 'gutter', 'is-changed');
            changedLines.push(l);
        }
    }
    function updateDirty() {
        dirtyMark.hidden = !isDirty();
        markChanged();
    }
    editor.on('change', (cm, change) => {
        updateDirty();
        if (change.origin !== 'setValue') {
            lastMessage = null;  // the result of the last action stays until an edit
            clearTimeout(checkTimer);
            checkTimer = setTimeout(() => check(false), 900);  // problems inline while writing
        }
        clearTimeout(draftTimer);
        draftTimer = setTimeout(keepDraft, 400);
    });
    app.guard = async () => {
        if (!isDirty()) return true;
        const leave = await confirmDialog('Unsaved changes', `Leave without downloading your changes to ${file()}?`, 'Leave', true);
        if (leave) {
            discarded = true;
            clearTimeout(draftTimer);
            drafts.clear(id, current);
        }
        return leave;
    };
    app.unsaved = () => (isDirty() ? file() : null);
    const beforeUnload = e => {
        if (isDirty()) {
            keepDraft();
            e.preventDefault();
            e.returnValue = '';
        }
    };
    window.addEventListener('beforeunload', beforeUnload);
    const watchTimer = setInterval(() => checkForOtherSaves(), 5000);
    onLeave(() => {
        window.removeEventListener('beforeunload', beforeUnload);
        clearInterval(watchTimer);
        clearTimeout(checkTimer);
        app.unsaved = null;
        clearTimeout(draftTimer);
        if (!discarded) keepDraft();  // a sign-out or an expired session (401) keeps the text
        left = true;
    });

    // ── Problems inline ──
    let widgets = [];
    let textMarks = [];
    let lineMarks = [];
    function unmark() {
        for (const w of widgets) w.clear();
        for (const m of textMarks) m.clear();
        for (const [line, cls] of lineMarks) editor.removeLineClass(line, 'wrap', cls);
        widgets = [];
        textMarks = [];
        lineMarks = [];
    }
    function markLine(line, cls) {
        if (line > 0 && line <= editor.lineCount()) {
            editor.addLineClass(line - 1, 'wrap', cls);
            lineMarks.push([line - 1, cls]);
        }
    }
    function goTo(line, column) {
        editor.focus();
        editor.setCursor(Math.max(0, line - 1), Math.max(0, (column || 1) - 1));
        editor.scrollIntoView(null, 80);
    }

    /// The Create tag quick fix of "no tag named X": the type the program declares, the next free
    /// address in a fitting area (Coils for BOOL, Holding registers otherwise), and a question first.
    function quickFix(d) {
        const m = /no tag named '([^']+)'/.exec(d.message);
        if (!m || !isAdmin() || !ctx.config) return null;
        const name = m[1];
        const declared = declaredType(editor.getValue(), d.line, name);
        const type = declared ? TAG_TYPE_OF_ST[declared] : null;
        if (!type) {
            return declared ? h('p', { class: 'secondary' }, `${declared} cannot be a tag: a tag is BOOL, INT, UINT, WORD, DINT, UDINT, DWORD or REAL.`) : null;
        }
        const area = type === 'bool' ? 'coil' : 'holding_register';
        const width = tagWidth(type);
        const address = freeAddress(ctx.config.tags, area, width, ctx.plc.image_size, null);
        const list = { get tags() { return ctx.config.tags; }, get version() { return ctx.plc.config_version; }, reload: null };
        const other = h('button', { class: 'link', type: 'button', onclick: async () => {
            if (await tagDialog(ctx, null, list, { name, type, area, address: address ?? undefined })) await afterTagChange();
        } }, 'Choose another address');
        if (address === null) return h('div', { class: 'actions' }, h('span', { class: 'secondary' }, `No ${areaLabel(area, 2).toLowerCase()} are free.`), other);
        return h('div', { class: 'actions' },
            h('button', { class: 'btn', type: 'button', onclick: () => createTag({ name, type, area, address }, declared) },
                `Create tag ${name}, ${declared}, at ${addressText(area, address, width)}`),
            other);
    }

    async function createTag(tag, declared) {
        const width = tagWidth(tag.type);
        const ok = await confirmDialog(`Create tag ${tag.name}?`, [
            dl([
                ['Type', `${ST_TYPE[tag.type]}${declared !== ST_TYPE[tag.type] ? `, declared ${declared}` : ''}`],
                ['Address', [addressText(tag.area, tag.address, width), h('span', { class: 'secondary' }, `Modicon ${modiconText(tag.area, tag.address, width)}`)]],
                ['OPC UA', `ns=1;s=${tag.name}`]], { style: '--label-column:96px' }),
            h('p', { class: 'secondary' }, tag.area === 'coil' || tag.area === 'holding_register'
                ? 'Modbus and OPC UA clients can read and write it, and the program binds it when it is downloaded.'
                : 'The program binds it when it is downloaded.')], 'Create tag');
        if (!ok) return;
        try {
            const r = await configRequest('POST', `/plcs/${enc(id)}/tags`, tag, ctx.plc.config_version, { what: 'The tag table' });
            if (r === null) return;
            toast(`Created ${tag.name} at ${addressText(tag.area, tag.address, width)}`);
            await afterTagChange();
        } catch (err) {
            toast(err.message, 'error');
        }
    }

    async function afterTagChange() {
        try {
            ctx.applyStatus(await api('GET', `/plcs/${enc(id)}`));  // the new tag table, before the next proposal
        } catch {}
        await check(false);
    }

    function problemNode(d, elsewhere) {
        return h('div', { class: `problem${d.severity === 'error' ? '' : ' is-warning'}${elsewhere ? ' is-block' : ''}`, role: 'note', dataset: { code: d.code || '' } },
            h('p', null, elsewhere ? h('span', { class: 'where' }, `${d.file} line ${d.line}: `) : null,
                h('span', { class: 'code-id' }, d.code), ' ', sentence(d.message), d.message && !/[.!?]$/.test(d.message) ? '.' : ''),
            d.hint ? h('p', { class: 'secondary' }, sentence(d.hint), /[.!?]$/.test(d.hint) ? '' : '.') : null,
            elsewhere ? null : quickFix(d));
    }

    /// Diagnostics of this file appear under their lines; those of other files (libraries) above the
    /// editor, naming their file.
    function showDiagnostics(list) {
        diagnostics = list;
        unmark();
        const all = list || [];
        const own = all.filter(d => !d.file || d.file === file());
        const other = all.filter(d => d.file && d.file !== file());
        for (const d of own) {
            const line = Math.min(Math.max(1, d.line || 1), editor.lineCount());
            markLine(line, d.severity === 'error' ? 'error-line' : 'warn-line');
            const text = editor.getLine(line - 1) || '';
            const col = Math.min(text.length, Math.max(0, (d.column || 1) - 1));
            const word = /^[A-Za-z0-9_#.]+/.exec(text.slice(col));
            const end = word ? col + word[0].length : Math.min(text.length, col + 1);
            if (end > col) textMarks.push(editor.markText({ line: line - 1, ch: col }, { line: line - 1, ch: end }, { className: d.severity === 'error' ? 'cm-problem' : 'cm-warning' }));
            widgets.push(editor.addLineWidget(line - 1, problemNode(d, false), { coverGutter: false, noHScroll: true }));
        }
        fill(otherProblems, other.map(d => problemNode(d, true)));
        const errors = all.filter(d => d.severity === 'error').length;
        const warnings = all.length - errors;
        setText(problemCount, list === null ? '' : [errors ? plural(errors, 'problem') : '', warnings ? plural(warnings, 'warning') : ''].filter(Boolean).join(', '));
        problemCount.className = `status problem-count${errors ? ' is-fault' : warnings ? ' is-attn' : ''}`;
        markFault(ctx.plc, true);
    }

    function markFault(p, force) {
        const t = taskOf(current);
        const fault = t && p.fault && p.fault.task === t.name ? p.fault : null;
        if (!fault) {
            if (shownFault !== null && !force) {
                shownFault = null;
                showDiagnostics(diagnostics);
            }
            return;
        }
        if (isDirty()) return;
        // Only a line of this file is marked; a library's fault names its file
        const own = faultInProgram(fault) && (!fault.file || fault.file === file());
        if (shownFault === fault.at && !force) return;
        const first = shownFault !== fault.at;
        shownFault = fault.at;
        if (own && fault.line > 0) {
            markLine(fault.line, 'is-here');
            widgets.push(editor.addLineWidget(Math.min(fault.line, editor.lineCount()) - 1,
                h('div', { class: 'problem', role: 'note' }, h('p', null, h('span', { class: 'code-id' }, fault.code), ' ',
                    `${sentence(fault.message)} at ${formatTime(fault.at)}, cycle ${(fault.cycle || 0).toLocaleString('en-US')}.`)),
                { coverGutter: false, noHScroll: true }));
            if (first) setTimeout(() => editor.scrollIntoView({ line: fault.line - 1, ch: 0 }, 120), 0);
        }
    }

    function setMessage(kind, parts) {
        lastMessage = { kind, parts };
        showMessage(kind, parts);
    }
    function showMessage(kind, parts) {
        message.className = `message${kind === 'error' ? ' is-fault' : ''}`;
        fill(message, ...parts);
    }
    function lineButton(line) {
        return line ? [h('button', { type: 'button', class: 'link', onclick: () => goTo(line, 1) }, `Line ${line}`), ': '] : null;
    }

    async function fetchProgram(name = current) {
        try {
            const r = await api('GET', `/plcs/${enc(id)}/programs/${enc(name)}`, undefined, { full: true });
            return { text: normalizeSource(r.data), etag: r.headers.get('ETag') };
        } catch (err) {
            if (err.status !== 404) throw err;
            return { text: '', etag: null };
        }
    }

    async function fetchStamp() {
        try {
            programs = await api('GET', `/plcs/${enc(id)}/programs`);
            renderFiles();
            const p = programs.find(x => x.name.toLowerCase() === current.toLowerCase());
            return p ? `${p.modified}/${p.size}` : 'none';
        } catch {
            return null;
        }
    }

    function renderFiles() {
        const names = [...new Set([...(ctx.plc.tasks || []).map(t => t.program), ...programs.map(p => p.name)])];
        if (!names.some(n => n.toLowerCase() === current.toLowerCase())) names.unshift(current);
        fill(filesNav, names.map(n => h('button', {
            type: 'button', class: 'file', 'aria-current': n.toLowerCase() === current.toLowerCase() ? 'page' : null,
            title: taskOf(n) ? `Task ${taskOf(n).name}` : 'A library: no task runs it',
            onclick: () => switchFile(n),
        }, `${n}.st`)));
    }

    async function switchFile(name) {
        if (name.toLowerCase() === current.toLowerCase()) return;
        if (isDirty() && !(await confirmDialog('Unsaved changes', `Switch to ${name}.st without downloading your changes to ${file()}?`, 'Switch', true))) return;
        clearTimeout(draftTimer);
        drafts.clear(id, current);
        current = name;
        loadedAt = null;
        await loadProgram();
    }

    async function loadProgram() {
        renderFiles();
        const [loaded, s] = await Promise.all([fetchProgram(), fetchStamp()]);
        if (ctx.seq !== app.routeSeq) return;  // the page moved on: nothing to show, no Watch to start
        savedText = loaded.text;
        etag = latestEtag = loaded.etag;
        stamp = s;
        theirs = null;
        notice.hidden = true;
        exampleTags = null;
        missingBox.hidden = true;
        lastMessage = null;
        const draft = isAdmin() ? drafts.get(id, current) : null;
        if (draft && draft.user === app.user.name && draft.text !== savedText) {
            editor.setValue(draft.text);
            if (draft.etag) etag = draft.etag;  // the edits start from that version: a save notices a newer one
            lastMessage = { kind: '', parts: [`Restored your unsaved changes from ${formatTime(draft.at)}.`,
                draft.base !== savedText ? ` ${file()} on the PLC changed since then.` : '',
                ' Download to run them, or ', h('button', { type: 'button', class: 'link', onclick: discardDraft }, 'discard them'), '.'] };
        } else {
            if (draft) drafts.clear(id, current);
            editor.setValue(savedText);
        }
        editor.clearHistory();
        updateDirty();
        shownFault = null;
        showDiagnostics(null);
        await refreshDiagnostics();
        if (lastMessage) showMessage(lastMessage.kind, lastMessage.parts);
        updateStatus(ctx.plc);
        watch.refresh();
    }

    async function refreshDiagnostics() {
        if (isDirty()) return check(false);
        try {
            const compiled = await api('GET', `/plcs/${enc(id)}/programs/${enc(current)}/compiled`);
            showDiagnostics(compiled.diagnostics || []);
        } catch {}
    }

    function discardDraft() {
        editor.setValue(savedText);
        etag = latestEtag;
        drafts.clear(id, current);
        lastMessage = null;
        updateDirty();
        refreshDiagnostics();
        updateStatus(ctx.plc);
    }

    /// Someone else (another browser, an agent, the API) may have saved this program: the panel
    /// polls the file's stamp and looks when a task loads a new version.
    let checking = false;
    async function checkForOtherSaves() {
        if (checking) return;
        checking = true;
        try {
            const s = await fetchStamp();
            if (s === null || s === stamp) return;
            stamp = s;
            const latest = await fetchProgram();
            if (latest.etag && latest.etag === latestEtag) return;
            if (!latest.etag && latest.text === savedText) return;
            latestEtag = latest.etag;
            if (latest.text === editor.getValue()) {  // it now holds exactly this text
                savedText = latest.text;
                etag = latest.etag;
                theirs = null;
                notice.hidden = true;
                updateDirty();
                keepDraft();
                return;
            }
            changedOnPlc(latest);
        } catch {
        } finally {
            checking = false;
        }
    }

    function changedOnPlc(latest) {
        const when = formatTime(Date.now());
        if (!isDirty()) {
            const cursor = editor.getCursor();
            const scroll = editor.getScrollInfo();
            savedText = latest.text;
            etag = latest.etag;
            editor.setValue(latest.text);
            editor.setCursor(cursor);
            editor.scrollTo(scroll.left, scroll.top);
            updateDirty();
            setMessage('', [`${file()} was changed on the PLC at ${when} by someone else (another browser, an agent or the API). The editor shows the new version.`]);
            refreshDiagnostics();
            return;
        }
        theirs = latest;
        fill(notice,
            h('p', null, h('strong', null, `${file()} was changed on the PLC at ${when}`),
                ' by someone else (another browser, an agent or the API) while you were editing. Downloading asks before it replaces their version.'),
            h('div', { class: 'actions' },
                h('button', { type: 'button', class: 'btn', onclick: () => reloadTheirs() }, 'Reload theirs'),
                h('button', { type: 'button', class: 'btn', onclick: () => { notice.hidden = true; } }, 'Keep editing')));
        notice.hidden = false;
    }

    async function reloadTheirs() {
        const latest = theirs || await fetchProgram();
        theirs = null;
        notice.hidden = true;
        savedText = latest.text;
        etag = latestEtag = latest.etag;
        editor.setValue(latest.text);  // undoable: Ctrl+Z brings the replaced text back
        updateDirty();
        drafts.clear(id, current);
        setMessage('', [`Reloaded ${file()} as saved on the PLC. ${mac ? '⌘Z' : 'Ctrl+Z'} brings your text back.`]);
        refreshDiagnostics();
    }

    function conflict() {
        return conflictDialog(`${file()} changed on the PLC`,
            `Someone else saved ${file()} after you opened it (another browser, an agent or the API). Reload theirs (${mac ? '⌘Z' : 'Ctrl+Z'} then brings yours back), overwrite it with yours, or keep editing.`);
    }

    /// Saves the editor's text; `deploy` loads it into the tasks that run it (a download).
    async function saveProgram({ deploy = true, cold = false, overwrite = false } = {}) {
        if (!isAdmin()) return;
        if (theirs && !overwrite) {  // their version was noticed before saving
            const choice = await conflict();
            if (choice === 'overwrite') return saveProgram({ deploy, cold, overwrite: true });
            if (choice === 'reload') return reloadTheirs();
            return;
        }
        downloadButton.disabled = true;
        const text = editor.getValue();
        const base = overwrite ? (theirs ? theirs.etag : latestEtag) : etag;
        try {
            const result = await api('PUT', `/plcs/${enc(id)}/programs/${enc(current)}${deploy ? '' : '?deploy=false'}`, text,
                { headers: base ? { 'If-Match': base } : {} });
            savedText = text;
            theirs = null;
            notice.hidden = true;
            etag = latestEtag = `"${result.hash}"`;  // the saved version's hash: what GET quotes as its ETag
            stamp = await fetchStamp();
            drafts.clear(id, current);
            updateDirty();
            showDiagnostics(result.diagnostics || []);
            const results = result.results || [];
            const failed = results.find(x => !x.ok);
            if (!deploy) {
                setMessage('', [`Saved at ${formatTime(Date.now())}, not downloaded. The downloaded version keeps running.`]);
            } else if (failed) {
                setMessage('error', ['Saved, but not running: ', lineButton(failed.line), `${failed.error}. The previous version keeps running.`]);
            } else if (results.length === 0) {
                setMessage('', [result.loaded === false ? 'Saved; it does not compile yet.' : `Saved at ${formatTime(Date.now())}. No task runs ${file()}: the programs that use it load it.`]);
            } else {
                const reset = result.reset || [];
                setMessage('ok', [`Downloaded at ${formatTime(Date.now())}.`,
                    reset.length ? h('span', { title: reset.join('\n') }, ` ${plural(reset.length, 'variable')} started from ${reset.length === 1 ? 'its' : 'their'} initial value: ${reset.slice(0, 4).join(', ')}${reset.length > 4 ? ', …' : ''}.`)
                        : ' Every variable kept its value.']);
                if (cold) await startPlc(ctx, 'restart');
            }
            await showMissingTags();
            watch.refresh();
        } catch (err) {
            if (err.status === 412) {  // saved by someone else since this version was loaded
                const latest = await fetchProgram().catch(() => null);
                if (latest) {
                    theirs = latest;
                    latestEtag = latest.etag;
                }
                downloadButton.disabled = !isAdmin();
                const choice = await conflict();
                if (choice === 'overwrite') return saveProgram({ deploy, cold, overwrite: true });
                if (choice === 'reload') return reloadTheirs();
                return;
            }
            if (err.status !== 401) setMessage('error', [err.message]);
        }
        downloadButton.disabled = !isAdmin();
    }

    /// Download: what the change does before it is made. For a program a task runs, the check answer
    /// has the online change: the variables that keep their values and those that start again.
    async function downloadDialog() {
        if (!isAdmin()) return;
        const text = editor.getValue();
        let result;
        try {
            result = await api('POST', `/plcs/${enc(id)}/programs/${enc(current)}/check`, text);
        } catch (err) {
            if (err.status !== 401) setMessage('error', [err.message]);
            return;
        }
        showDiagnostics(result.diagnostics || []);
        const p = ctx.plc;
        const task = taskOf(current);
        const range = changedRange(savedText, text);
        const errors = (result.diagnostics || []).filter(d => d.severity === 'error');
        const oc = result.online_change || null;
        const list = v => (Array.isArray(v) ? (v.length ? v.join(', ') : h('span', { class: 'muted' }, 'Nothing')) : v === undefined || v === null ? null : String(v));
        let cold = false;
        const coldBox = task && result.ok ? h('label', { class: 'choice' },
            h('input', { type: 'checkbox', id: 'dl-cold', onchange: e => { cold = e.target.checked; } }),
            h('span', null, 'Cold restart instead'),
            h('span', { class: 'secondary' }, 'Every variable starts from its initial value, RETAIN included, and every value from 0.')) : null;
        const label = plcLabel(p);
        const lead = !result.ok ? `${file()} does not compile, so it cannot run. Save it to keep your work; the downloaded version keeps running.`
            : !task ? `${file()} is a library: no task runs it. The programs that use it load it again.`
                : p.state === 'fault' ? 'The PLC is in Fault. The new code loads now; Warm restart then runs it.'
                    : p.running ? 'Online change. The PLC keeps running and the new code runs from the next cycle.'
                        : 'The PLC is stopped. The new code runs when it is started.';
        const ok = await dialog('Download', close => dialogForm({
            title: `Download to ${label}`,
            lead,
            body: [
                dl([
                    ['Code', `${file()}, ${rangeWords(range)}`],
                    !result.ok ? ['Problems', h('span', { class: 'is-fault' }, `${plural(errors.length || 1, 'problem')}: ${result.error || (errors[0] && errors[0].message)}`)] : null,
                    result.ok && task ? ['Runs in', `task ${task.name}, every ${task.cycle_ms}${NBSP}ms`] : null,
                    result.ok && oc ? ['Keeps its value', list(oc.kept)] : null,
                    result.ok && oc ? ['Starts again', list(oc.reset)] : null,
                    result.ok && oc && oc.tasks !== undefined ? ['Tasks', list(oc.tasks)] : null,
                    result.ok ? ['Size', `${result.memory.data_bytes}${NBSP}bytes of data, ${plural(result.memory.instructions, 'instruction')}`] : null,
                ], { style: '--label-column:140px' }),
                coldBox],
            onsubmit: e => { e.preventDefault(); close(true); },
            actions: [
                h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
                h('button', { type: 'submit', class: 'btn btn-primary' }, result.ok ? 'Download' : 'Save only')],
        }), { focus: '[data-cancel]', wide: true });
        if (!ok) return;
        await saveProgram({ deploy: result.ok, cold });
    }

    async function check(explicit) {
        const seq = ++checkSeq;
        try {
            const result = await api('POST', `/plcs/${enc(id)}/programs/${enc(current)}/check`, editor.getValue());
            if (seq !== checkSeq || left) return;
            showDiagnostics(result.diagnostics || []);
            if (explicit) {
                if (result.ok) setMessage('ok', [`Compiles: ${result.memory.data_bytes}${NBSP}bytes of data, ${plural(result.memory.instructions, 'instruction')}.`]);
                else setMessage('error', [lineButton(result.line), result.error]);
            }
        } catch (err) {
            if (explicit && err.status !== 401) setMessage('error', [err.message]);
        }
        if (explicit) await showMissingTags();
    }

    /// After Check or Download: the tags an opened example needs that this PLC lacks, with one button
    /// that adds them at free addresses. Opening an example never changes the tags by itself.
    async function showMissingTags() {
        if (!exampleTags || !isAdmin()) {
            missingBox.hidden = true;
            return;
        }
        let latest;
        try {
            latest = await api('GET', `/plcs/${enc(id)}`);
        } catch {
            return;
        }
        const have = new Set(latest.config.tags.map(t => t.name.toLowerCase()));
        const text = editor.getValue();
        const missing = exampleTags.filter(t => !have.has(t.name.toLowerCase()) && new RegExp(`\\b${t.name}\\b`, 'i').test(text));
        missingBox.hidden = missing.length === 0;
        if (!missing.length) return;
        fill(missingBox,
            h('p', null, h('strong', null, missing.length === 1 ? 'The example needs a tag this PLC does not have: ' : `The example needs ${missing.length} tags this PLC does not have: `),
                missing.map(t => `${t.name} (${areaLabel(t.area)}, ${ST_TYPE[t.type] || t.type})`).join(', '), '.'),
            h('div', { class: 'actions' }, h('button', { type: 'button', class: 'btn', onclick: () => addMissing(latest, missing) }, 'Add the missing tags')));
    }

    async function addMissing(latest, missing) {
        const all = [...latest.config.tags];
        const added = [];
        for (const t of missing) {
            const address = freeAddress(all, t.area, tagWidth(t.type), latest.image_size, null);
            if (address === null) {
                toast(`No free ${areaLabel(t.area, 2).toLowerCase()} are left for ${t.name}`, 'error');
                return;
            }
            const tag = { ...t, address };
            all.push(tag);
            added.push(tag);
        }
        try {
            const r = await configRequest('PUT', `/plcs/${enc(id)}/tags`, all, latest.config_version, { what: 'The tag table' });
            if (r === null) return;
            toast(`Added ${added.map(t => `${t.name} (${addressText(t.area, t.address, tagWidth(t.type))})`).join(', ')}`);
            await afterTagChange();
            await check(true);
        } catch (err) {
            toast(err.message, 'error');
        }
    }

    async function compiledDialog() {
        let compiled;
        try {
            compiled = await api('GET', `/plcs/${enc(id)}/programs/${enc(current)}/compiled`);
        } catch (err) {
            toast(err.message, 'error');
            return;
        }
        await sourceDialog(`${file()} as compiled`,
            'The downloaded program as the compiler understood it: every expression in parentheses, literals with their type, conversions written out. (*L12*) is the line in the source.',
            compiled.ok ? compiled.listing : null, 'The downloaded program does not compile; see the problems in the editor.');
    }

    async function historyDialog() {
        let versions;
        try {
            versions = await api('GET', `/plcs/${enc(id)}/programs/${enc(current)}/history`);
        } catch (err) {
            toast(err.message, 'error');
            return;
        }
        const chosen = await dialog('Earlier versions', close => dialogForm({
            title: `Earlier versions of ${file()}`,
            lead: versions.length ? 'Every download keeps the version it replaced: the last 20.' : null,
            body: versions.length === 0
                ? h('p', { class: 'muted' }, 'None yet. Every download keeps the version it replaced here (the last 20).')
                : h('div', { class: 'table-wrap' }, h('table', { class: 'data data-compact' }, h('tbody', null, versions.map(v => h('tr', null,
                    h('td', null, formatDateTime(v.saved)),
                    h('td', { class: 'muted num' }, `${v.size}${NBSP}bytes`),
                    h('td', { class: 'r' }, h('button', { type: 'button', class: 'btn', onclick: () => close(v) }, isAdmin() ? 'Open' : 'View'))))))),
            actions: [h('button', { type: 'button', class: 'btn', onclick: () => close(null) }, 'Close')],
        }), { wide: true });
        if (!chosen) return;
        let text;
        try {
            text = normalizeSource(await api('GET', `/plcs/${enc(id)}/programs/${enc(current)}/history/${enc(chosen.version)}`));
        } catch (err) {
            toast(err.message, 'error');
            return;
        }
        if (!isAdmin()) {  // a viewer cannot save: show it beside the editor, not in it
            await sourceDialog(`${file()} from ${formatDateTime(chosen.saved)}`, 'An earlier version, read only.', text);
            return;
        }
        if (isDirty() && !(await confirmDialog('Unsaved changes',
            `Replace your unsaved changes to ${file()} with the version from ${formatDateTime(chosen.saved)}?`, 'Replace', true))) return;
        editor.setValue(text);
        updateDirty();
        exampleTags = null;
        missingBox.hidden = true;
        setMessage('', [`Opened the version from ${formatDateTime(chosen.saved)}. Download to run it.`]);
        check(false);
    }

    async function examplesDialog() {
        let examples;
        try {
            examples = await api('GET', '/examples');
        } catch (err) {
            toast(err.message, 'error');
            return;
        }
        const chosen = await dialog('Examples', close => dialogForm({
            title: 'Examples',
            lead: 'Opens an example’s program in the editor. Nothing on the PLC changes until you download; Check lists the tags it needs.',
            body: h('table', { class: 'data data-compact' }, h('tbody', null, examples.map(ex => h('tr', { dataset: { example: ex.name } },
                h('td', null, h('span', { class: 'name' }, ex.title), h('span', { class: 'secondary' }, ex.description)),
                h('td', { class: 'r' }, h('button', { type: 'button', class: 'btn', onclick: () => close(ex) }, 'Open')))))),
            actions: [h('button', { type: 'button', class: 'btn', onclick: () => close(null) }, 'Close')],
        }), { wide: true });
        if (!chosen) return;
        let full;
        try {
            full = await api('GET', `/examples/${enc(chosen.name)}`);
        } catch (err) {
            toast(err.message, 'error');
            return;
        }
        const source = full.programs[current] || Object.values(full.programs)[0];
        if (isDirty() && !(await confirmDialog('Unsaved changes',
            `Replace your unsaved changes to ${file()} with the ${chosen.title} example?`, 'Replace', true))) return;
        editor.setValue(normalizeSource(source));
        updateDirty();
        exampleTags = full.tags || [];
        missingBox.hidden = true;
        setMessage('', [`Opened the ${chosen.title} example. Check lists the tags it needs; Download runs it.`]);
        check(false);
    }

    /// Import .st files: each is PUT under its name. A file a task runs downloads; the others are
    /// libraries (or programs no task runs yet). Asks first, naming what happens to each.
    async function importFiles(files) {
        if (!isAdmin() || !files.length) return;
        const items = [];
        const refused = [];
        for (const f of files) {
            const name = f.name.replace(/\.st$/i, '');
            if (!/\.st$/i.test(f.name)) refused.push(`${f.name} is not a .st file`);
            else if (!NAME_RE.test(name)) refused.push(`${f.name}: a program name is letters, digits and _, starting with a letter`);
            else items.push({ name, text: normalizeSource(await f.text()) });
        }
        if (!items.length) {
            toast(refused.join('; ') || 'Nothing to import', 'error');
            return;
        }
        const known = new Map(programs.map(p => [p.name.toLowerCase(), p]));
        const what = item => {
            const t = taskOf(item.name);
            if (t) return `Replaces ${item.name}.st, which task ${t.name} runs: it downloads now`;
            if (known.has(item.name.toLowerCase())) return 'Replaces this library';
            return /\bPROGRAM\b/i.test(item.text) ? 'A new program; no task runs it until one is added in Settings' : 'A new library: no task runs it';
        };
        const replacesEdits = items.some(i => i.name.toLowerCase() === current.toLowerCase()) && isDirty();
        const ok = await confirmDialog(items.length === 1 ? `Import ${items[0].name}.st to ${plcLabel(ctx.plc)}?` : `Import ${items.length} files to ${plcLabel(ctx.plc)}?`, [
            h('table', { class: 'data data-compact' }, h('tbody', null, items.map(i => h('tr', null,
                h('td', { class: 'name' }, `${i.name}.st`), h('td', { class: 'muted nowrap' }, `${i.text.split('\n').length} lines`), h('td', null, what(i)))))),
            refused.length ? h('p', { class: 'is-attn' }, `Left out: ${refused.join('; ')}.`) : null,
            replacesEdits ? h('p', { class: 'is-attn' }, `Your unsaved changes to ${file()} are replaced.`) : null,
        ], items.length === 1 ? 'Import' : `Import ${items.length} files`, true);
        if (!ok) return;
        const failures = [];
        // Libraries first: a program that uses one finds it when it loads
        const ordered = [...items].sort((a, b) => Number(Boolean(taskOf(a.name))) - Number(Boolean(taskOf(b.name))));
        for (const item of ordered) {
            try {
                const r = await api('PUT', `/plcs/${enc(id)}/programs/${enc(item.name)}`, item.text);
                const failed = (r.results || []).find(x => !x.ok);
                if (failed) failures.push(`${item.name}.st: ${failed.error}`);
            } catch (err) {
                failures.push(`${item.name}.st: ${err.message}`);
            }
        }
        if (failures.length) toast(`Imported with problems. ${failures.join('; ')}`, 'error');
        else toast(items.length === 1 ? `Imported ${items[0].name}.st` : `Imported ${items.length} files`);
        if (items.some(i => i.name.toLowerCase() === current.toLowerCase())) {
            drafts.clear(id, current);  // the imported file replaced the text being edited
            await loadProgram();
        } else {
            stamp = await fetchStamp();
        }
    }
    fileInput.addEventListener('change', () => {
        const files = [...fileInput.files];
        fileInput.value = '';
        importFiles(files);
    });
    programGrid.addEventListener('dragover', e => {
        if (!isAdmin() || !e.dataTransfer || ![...e.dataTransfer.types].includes('Files')) return;
        e.preventDefault();
        programGrid.classList.add('is-drop');
    });
    programGrid.addEventListener('dragleave', e => {
        if (!programGrid.contains(e.relatedTarget)) programGrid.classList.remove('is-drop');
    });
    programGrid.addEventListener('drop', e => {
        programGrid.classList.remove('is-drop');
        if (!isAdmin() || !e.dataTransfer || !e.dataTransfer.files.length) return;
        e.preventDefault();
        e.stopPropagation();
        importFiles([...e.dataTransfer.files]);
    }, true);
    // CodeMirror would insert a dropped file's text: Import handles files
    editor.on('drop', (cm, e) => {
        if (e.dataTransfer && e.dataTransfer.files.length) e.preventDefault();
    });

    function updateStatus(p) {
        const t = taskOf(current);
        if (t) {
            const live = (p.tasks || []).find(x => x.name === t.name) || t;
            if (loadedAt !== null && live.loaded_at !== loadedAt) checkForOtherSaves();  // a version was loaded: whose?
            loadedAt = live.loaded_at ?? null;
            setText(taskInfo, `Task ${live.name}, every ${live.cycle_ms}${NBSP}ms${live.loaded_at ? `, loaded ${formatWhen(live.loaded_at)}` : ''}`);
        } else {
            setText(taskInfo, 'A library: no task runs it');
        }
        const fault = t && p.fault && p.fault.task === t.name ? p.fault : null;
        markFault(p, false);
        if (fault && !isDirty()) {
            const own = faultInProgram(fault) && (!fault.file || fault.file === file());
            showMessage('error', [h('strong', null, `Fault ${fault.code}`), ' ',
                own && fault.line ? lineButton(fault.line) : fault.file ? `in ${faultWhere(fault)}, ` : null,
                `cycle ${(fault.cycle || 0).toLocaleString('en-US')}: ${fault.message}. Warm restart runs the program again (RETAIN variables keep their values).`]);
            return;
        }
        if (lastMessage) return showMessage(lastMessage.kind, lastMessage.parts);
        if (!t) return showMessage('', ['Download saves it; the programs that use it load it again.']);
        const live = (p.tasks || []).find(x => x.name === t.name) || t;
        if (!p.running) showMessage('', ['The PLC is stopped. Run… starts the program.']);
        else if (live.enabled === false) showMessage('', ['This task is disabled.']);
        else if (live.loaded) showMessage('', [`Running, ${formatUs(live.exec_us && live.exec_us.avg)} per cycle.`]);
        else showMessage('error', ['Not running. Download the program to load it.']);
    }

    downloadButton.addEventListener('click', () => downloadDialog());
    await loadProgram();
    setTimeout(() => editor.refresh(), 0);

    return {
        el,
        mounted: () => editor.refresh(),
        status(p) {
            updateStatus(p);
        },
    };
}

/// A read-only listing in a dialog.
async function sourceDialog(title, note, text, otherwise) {
    await dialog(title, close => {
        const box = h('div', { class: 'listing' });
        const form = dialogForm({
            title, lead: note,
            body: text !== null ? box : h('p', { class: 'problems' }, otherwise),
            actions: [h('button', { type: 'button', class: 'btn', onclick: () => close(false) }, 'Close')],
        });
        if (text !== null) {
            setTimeout(() => {
                const cm = CodeMirror(box, { value: text, mode: 'st', readOnly: 'nocursor', lineNumbers: false, lineWrapping: true });
                cm.refresh();
                form.scrollTop = 0;
            }, 0);
        }
        return form;
    }, { wide: true });
}

// ───────────────────────────── Watch ─────────────────────────────

/// Large arrays are listed as one row without their elements: "(array of 10,000, not shown)".
function arraySize(type) {
    const m = /^ARRAY\s*\[([^\]]+)\]/i.exec(type || '');
    if (!m) return null;
    let n = 1;
    for (const range of m[1].split(',')) {
        const [lo, hi] = range.split('..').map(x => Number(x.trim()));
        if (!Number.isInteger(lo) || !Number.isInteger(hi)) return null;
        n *= hi - lo + 1;
    }
    return n;
}

/// A variable's value as the Watch shows it: a typed literal's prefix (UINT#500) is in the Type column.
function plainText(v) {
    const text = v.text ?? '';
    return /^[A-Z]+#[-+0-9.]/.test(text) && !/^(T|TIME|LT|LTIME|D|DATE|TOD|DT)#/i.test(text) ? text.replace(/^[A-Z]+#/, '') : text;
}

/// The Watch pane: the program's variables, then the tags it uses, live with their types. A value
/// opens Modify; arrays and constants are not writable.
function watchPane(ctx, taskNow, sourceNow) {
    const body = h('tbody');
    const note = h('span', { class: 'secondary' });
    const problems = h('p', { class: 'problems', role: 'alert' });
    const pause = h('button', { class: 'btn btn-quiet', type: 'button' }, 'Pause');
    const emptyNote = h('p', { class: 'secondary empty-note', hidden: true });
    const el = h('aside', { class: 'watch', 'aria-label': 'Watch' },
        h('div', { class: 'watch-head' }, h('h3', null, 'Watch'), note, pause),
        problems, emptyNote,
        h('table', null, h('thead', { class: 'visually-hidden' }, h('tr', null, h('th', null, 'Name'), h('th', null, 'Type'), h('th', null, 'Value'))), body));
    const rows = new Map();  // path -> { row, value, text }
    let paused = false;
    let timer = null;
    let shownTask = null;
    let caseMap = new Map();
    let caseSource = null;
    const groupRow = h('tr', { class: 'group' }, h('td', { colspan: 3 }, 'Tags'));
    pause.addEventListener('click', () => {
        paused = !paused;
        setText(pause, paused ? 'Resume' : 'Pause');
        clearTimeout(timer);  // a refresh already planned must not run after Pause
        if (!paused) refresh();
    });

    /// Names as the source writes them: the API answers in upper case.
    function displayName(path, program) {
        const source = sourceNow();
        if (source !== caseSource) {
            caseSource = source;
            caseMap = new Map();
            for (const m of source.matchAll(/[A-Za-z_][A-Za-z0-9_]*/g)) if (!caseMap.has(m[0].toUpperCase())) caseMap.set(m[0].toUpperCase(), m[0]);
        }
        const parts = path.split('.');
        if (parts[0] === program.toUpperCase()) parts.shift();
        return parts.map(p => caseMap.get(p.replace(/\[.*$/, '')) ? p.replace(/^[A-Z0-9_]+/, caseMap.get(p.replace(/\[.*$/, ''))) : p).join('.');
    }

    async function refresh(explicit = false) {
        clearTimeout(timer);
        if (ctx.seq !== app.routeSeq) return;  // the page moved on: no further poll
        const task = taskNow();
        if (!task) {
            clear(body);
            setText(note, 'No task');
            return;
        }
        if (shownTask !== task.name) {
            shownTask = task.name;
            rows.clear();
            clear(body);
        }
        try {
            const data = await api('GET', `/plcs/${enc(ctx.plc.id)}/variables?task=${enc(task.name)}`);
            if (ctx.seq !== app.routeSeq) return;
            if (paused && !explicit) return;  // paused while the request was on its way
            setText(problems, '');
            const seen = new Set();
            const vars = data.variables.filter(v => !(v.flags || []).includes('tag'));
            const tags = data.variables.filter(v => (v.flags || []).includes('tag'));
            const order = [...vars, null, ...tags];
            let at = 0;
            for (const v of order) {
                if (v === null) {
                    groupRow.hidden = tags.length === 0;
                    if (body.children[at] !== groupRow) body.insertBefore(groupRow, body.children[at] || null);
                    at++;
                    continue;
                }
                seen.add(v.path);
                let r = rows.get(v.path);
                if (!r) r = makeRow(v, data.program);
                if (body.children[at] !== r.row) body.insertBefore(r.row, body.children[at] || null);
                at++;
                if (r.text !== v.text || r.forced !== ctx.forced.has(r.tagName)) {
                    r.text = v.text;
                    r.forced = ctx.forced.has(r.tagName);
                    const shown = v.text === '(array)' ? `(array${arraySize(v.type) ? ` of ${arraySize(v.type).toLocaleString('en-US')}` : ''}, not shown)` : plainText(v);
                    setText(r.value, shown);
                    r.value.classList.toggle('muted', v.text === '(array)');
                    r.value.classList.toggle('is-attn', r.forced);
                }
                r.v = v;
            }
            for (const [path, r] of rows) {
                if (!seen.has(path)) {
                    r.row.remove();
                    rows.delete(path);
                }
            }
            emptyNote.hidden = data.variables.length > 0;
            setText(emptyNote, `The variables of ${data.program} appear here, live, once the program declares them and is downloaded.`);
            const p = ctx.plc;
            setText(note, p.fault && p.fault.task === task.name ? `At the fault, ${formatTime(p.fault.at)}` : p.running ? `Live, program ${data.program}` : 'Stopped');
        } catch (err) {
            if (err.status !== 401) setText(problems, err.message);
        }
        if (!paused) timer = setTimeout(refresh, 1000);
    }

    function makeRow(v, program) {
        const flags = v.flags || [];
        const isTag = flags.includes('tag');
        const tagName = isTag ? v.path.split('.').pop() : null;
        const writable = isAdmin() && !flags.includes('constant') && v.text !== '(array)' && !/^ARRAY/i.test(v.type || '');
        const value = writable ? h('button', { class: 'value', type: 'button' }) : h('span', { class: 'value-static' });
        const r = { row: null, value, text: null, v, tagName, forced: false };
        if (writable) {
            value.addEventListener('click', () => {
                if (isTag) {
                    // The tag list names the tag's writer, which Modify explains
                    ctx.tags().then(list => {
                        const tag = list.find(t => t.name.toUpperCase() === tagName.toUpperCase());
                        if (tag) modifyTag(ctx, tag);
                    }, err => toast(err.message, 'error'));
                } else {
                    modifyVariable(ctx, shownTask, r.v, displayName(r.v.path, program)).then(done => { if (done) refresh(true); });
                }
            });
        }
        r.row = h('tr', { dataset: { path: v.path } },
            h('td', { class: 'watch-name' }, displayName(v.path, program)),
            h('td', { class: 'type' }, v.type),
            h('td', null, value));
        rows.set(v.path, r);
        return r;
    }

    onLeave(() => clearTimeout(timer));
    return {
        el,
        refresh: () => { shownTask = null; return refresh(true); },
    };
}

/// Modify a program variable: a Structured Text literal (or TRUE/FALSE), checked by the PLC like an
/// assignment. The program sees it at its next cycle and may change it again.
async function modifyVariable(ctx, task, v, name) {
    const bool = /^BOOL$/i.test(v.type);
    let next = bool ? (v.text === 'TRUE' ? 'FALSE' : 'TRUE') : null;
    const problems = h('div', { class: 'problems', role: 'alert' });
    const toText = h('span', { class: 'to' }, bool ? next : plainText(v));
    let input = null;
    let valueField;
    if (bool) {
        const seg = segmented([['TRUE', 'TRUE'], ['FALSE', 'FALSE']], next, x => { next = x; setText(toText, x); }, 'New value');
        seg.querySelectorAll('button').forEach(b => b.classList.add('seg-choice'));
        valueField = h('div', { class: 'field' }, h('span', { class: 'label' }, 'New value'), seg);
    } else {
        input = h('input', { class: 'input w-md code', id: 'mv-v', type: 'text', value: plainText(v), autocomplete: 'off', spellcheck: 'false', 'aria-label': `New value for ${name}` });
        input.addEventListener('input', () => setText(toText, input.value.trim() || '–'));
        valueField = h('div', { class: 'field' }, h('label', { for: 'mv-v' }, 'New value'), input,
            h('p', { class: 'hint' }, 'A Structured Text literal: 42, -1.5, TRUE, T#2s, \'text\' or an enumeration value.'),
            /INT|REAL|WORD|BYTE/i.test(v.type) ? keypad(input) : null);
    }
    return dialog(`Modify ${name}`, close => {
        const form = dialogForm({
            title: `Modify ${name}`,
            lead: [h('p', { class: 'muted' }, `${v.type}, a variable of the program in task ${task}. The program sees the new value at its next cycle and may change it again.`),
                h('p', { class: 'secondary' }, 'Not a tag: Modbus and OPC UA do not see it.')],
            body: [h('div', { class: 'dlg-value' }, valueField, problems),
                h('div', { class: 'dlg-effects' }, h('p', { class: 'change' }, h('span', { class: 'from' }, plainText(v)), h('span', { class: 'muted arrow' }, '→'), toText))],
            onsubmit: async e => {
                e.preventDefault();
                const literal = bool ? next : input.value.trim();
                if (!literal) return close(false);
                try {
                    await api('PUT', `/plcs/${enc(ctx.plc.id)}/variables`, { task, values: { [v.path]: literal } });
                    toast(`Wrote ${name}`);
                    close(true);
                } catch (err) {
                    showProblems(problems, err);
                }
            },
            actions: [h('button', { type: 'button', class: 'btn', 'data-cancel': '', onclick: () => close(false) }, 'Cancel'),
                h('button', { type: 'submit', class: 'btn btn-primary' }, 'Modify', h('span', { class: 'key' }, 'Enter'))],
        });
        form.querySelector('.body').classList.add('split');
        enterSubmits(form);
        return form;
    }, { focus: bool ? '.btn-primary' : '#mv-v' });
}
