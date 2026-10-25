// IEC 61131-3 Structured Text mode for CodeMirror 5 (SimplePLC). Case-insensitive keywords,
// (* *) and /* */ block comments (state carried across lines), // line comments, typed and
// duration literals, strings with $ escapes, pragmas in { }.
(function (CodeMirror) {
    'use strict';
    const words = list => new Set(list.split(' '));
    const keywords = words(
        'PROGRAM END_PROGRAM FUNCTION END_FUNCTION FUNCTION_BLOCK END_FUNCTION_BLOCK VAR VAR_INPUT VAR_OUTPUT ' +
        'VAR_IN_OUT VAR_TEMP VAR_EXTERNAL VAR_GLOBAL END_VAR CONSTANT RETAIN NON_RETAIN TYPE END_TYPE STRUCT ' +
        'END_STRUCT ARRAY OF IF THEN ELSIF ELSE END_IF CASE END_CASE FOR TO BY DO END_FOR WHILE END_WHILE ' +
        'REPEAT UNTIL END_REPEAT EXIT CONTINUE RETURN AND OR XOR NOT MOD');
    const atoms = words('TRUE FALSE');
    const types = words(
        'BOOL SINT INT DINT LINT USINT UINT UDINT ULINT REAL LREAL BYTE WORD DWORD LWORD TIME LTIME ' +
        'DATE TIME_OF_DAY TOD DATE_AND_TIME DT STRING WSTRING CHAR WCHAR');
    const builtins = words(
        'TON TOF TP R_TRIG F_TRIG SR RS CTU CTD CTUD CTU_DINT CTD_DINT CTUD_DINT CTU_LINT CTD_LINT CTUD_LINT ' +
        'CTU_UDINT CTD_UDINT CTUD_UDINT CTU_ULINT CTD_ULINT CTUD_ULINT ' +
        'ABS SQRT LN LOG EXP SIN COS TAN ASIN ACOS ATAN ATAN2 EXPT ADD SUB MUL DIV MOVE SEL MAX MIN LIMIT MUX ' +
        'SHL SHR ROL ROR GT GE EQ LE LT NE LEN LEFT RIGHT MID CONCAT INSERT DELETE REPLACE FIND ' +
        'TRUNC ROUND ADD_TIME SUB_TIME MUL_TIME DIV_TIME FIRST_CYCLE EN ENO');
    const conversion = /^[A-Z]+_TO_[A-Z]+$/;

    CodeMirror.defineMode('st', () => ({
        startState: () => ({ comment: null }),  // '*)' or '*/' while inside a block comment
        token(stream, state) {
            if (state.comment) {
                while (!stream.eol()) {
                    if (stream.match(state.comment)) {
                        state.comment = null;
                        break;
                    }
                    stream.next();
                }
                return 'comment';
            }
            if (stream.eatSpace()) return null;
            if (stream.match('(*')) { state.comment = '*)'; return this.token(stream, state) || 'comment'; }
            if (stream.match('/*')) { state.comment = '*/'; return this.token(stream, state) || 'comment'; }
            if (stream.match('//')) { stream.skipToEnd(); return 'comment'; }
            if (stream.peek() === '{') {
                stream.skipTo('}') ? stream.next() : stream.skipToEnd();
                return 'meta';
            }
            const quote = stream.peek();
            if (quote === "'" || quote === '"') {
                stream.next();
                while (!stream.eol()) {
                    const c = stream.next();
                    if (c === '$') stream.next();
                    else if (c === quote) break;
                }
                return 'string';
            }
            // Durations and typed or based literals: T#1h2m, TIME#-5s, INT#16#FF, 2#1010, 16#FF_FF
            if (stream.match(/^(?:L?T|L?TIME|DATE|D|TOD|TIME_OF_DAY|DT|DATE_AND_TIME)#[-0-9a-z_.:]+/i)) return 'number';
            if (stream.match(/^[A-Z_][A-Z0-9_]*#(?:\d+#)?[-+0-9a-f_.e]+/i)) return 'number';
            if (stream.match(/^\d[\d_]*#[0-9a-f_]+/i)) return 'number';
            if (stream.match(/^\d[\d_]*(?:\.\d[\d_]*)?(?:e[-+]?\d+)?/i)) return 'number';
            if (stream.match(/^[A-Z_][A-Z0-9_]*/i)) {
                const w = stream.current().toUpperCase();
                if (keywords.has(w)) return 'keyword';
                if (atoms.has(w)) return 'atom';
                if (types.has(w)) return 'type';
                if (builtins.has(w) || conversion.test(w)) return 'builtin';
                return 'variable';
            }
            if (stream.match(':=') || stream.match('=>') || stream.match('<>') || stream.match('<=') ||
                stream.match('>=') || stream.match('**')) return 'operator';
            const c = stream.next();
            return /[-+*/=<>&]/.test(c) ? 'operator' : null;
        },
        lineComment: '//',
        blockCommentStart: '(*',
        blockCommentEnd: '*)',
    }));
    CodeMirror.defineMIME('text/x-structured-text', 'st');
})(CodeMirror);
