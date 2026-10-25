# SimplePLC compliance statement (IEC 61131-3)

This statement covers SimplePLC 2.0 (compiler `simpleplc-st 1`): the Structured Text language,
its standard functions and function blocks, and the run-time behaviour of programs. IEC 61131-3
asks an implementer to state the features it supports, the values and choices the standard
leaves to the implementation, and how errors are treated. This document does that, item by
item, so it can be checked. How to write programs is in `docs/structured-text.md`.

## Basis and method

- **Edition.** The semantics follow IEC 61131-3 Ed. 3 (2013) as described by public sources:
  PLCopen's copy of Ed. 2 §3.3, the Ed. 3 conformance lists published by Siemens, Omron and
  logi.cals, the IEC SC65B/WG7/TF3 errata, and the documentation of Fernhill, CODESYS,
  PLCnext, MatIEC and RuSTy. The purchased standard text was not available. Ed. 4 (2025) was
  not considered.
- **Table and feature numbers** below are those of Ed. 3 as the vendor conformance lists cite
  them. Every number, and every decision taken from secondary sources, still has to be checked
  against the purchased text: those items are marked **[confirm]** and collected in
  [Open items](#open-items-confirm). As a whole, the numbering is [confirm].
- **Evidence.** The behaviour stated here was checked by running this version: a conformance
  corpus of 716 programs written from the sources above (not from the implementation), 352
  tests of the design decisions, and a check of every example and every stated code or value
  in this document and in `docs/structured-text.md`. Where this version differs from its own
  design, the difference is listed under [Known deviations](#known-deviations-in-this-version).
- **Strictness.** Where the standard is open or vendors disagree, SimplePLC refuses the
  program with a diagnostic rather than pick a meaning silently.

## Scope

| Part of IEC 61131-3 | Status |
|---|---|
| Common elements: types, variables, POUs, standard functions and blocks | Implemented as listed below. |
| Structured Text (ST) | Implemented as listed below. |
| Instruction List, Ladder, Function Block Diagram, Sequential Function Chart | Not implemented. |
| Configurations, resources, tasks (Table 62) | Not in the language: the PLC's configuration declares tasks (name, program, interval) and the tag table. |
| Object-oriented features (classes, methods, interfaces; Tables 48–61) | Not implemented. |
| Namespaces (Table 64) | Not implemented. |

## Implemented features

"Yes" means implemented as the standard describes it, with the implementation choices given in
the next section. "Partial" names what is missing. Unsupported features are refused with a
diagnostic (see [Refused features](#refused-features)).

### Common elements

| Ed. 3 table | Features | Status |
|---|---|---|
| 1 Character set | Source text in UTF-8; ASCII outside comments and string literals | Partial: non-ASCII characters only in comments and strings. A UTF-8 byte-order mark at the start is skipped; bytes that are not UTF-8 are refused (`E_SYNTAX`), and a character outside a string or comment is named (`U+00A0 (no-break space)`). |
| 2 Identifiers | Letters, digits, embedded and leading single underscores; case-insensitive | Yes. `__` and a trailing `_` are refused (`E_SYNTAX`). |
| 3 Comments | 1 `//`; 2a `(* *)`; 2b `/* */` | Yes. 3a, 3b nested comments: no (`E_COMMENT`). |
| 4 Pragmas | `{ … }` | Ignored with `W_PRAGMA`; conditional compilation refused (`E_PRAGMA`). |
| 5 Numeric literals | Integer, real, real with exponent, binary, octal, hexadecimal, `_` separators, typed literals, `TRUE`/`FALSE` | Yes. Feature 7 (Boolean 0 and 1): only as `BOOL#0`, `BOOL#1`. |
| 6 Character string literals | Single-byte strings, typed `STRING#'…'` | Partial: double-byte (WSTRING) literals refused. |
| 7 Two-character combinations | `$$ $' $L $N $P $R $T $hh`, either case | Yes. |
| 8 Duration literals | `T#`, `TIME#`, `LT#`, `LTIME#`; d h m s ms us ns; `_`; sign; fraction in the last unit | Yes. |
| 9 Date and time-of-day literals | | No. |
| 10 Elementary data types | BOOL; SINT, INT, DINT, LINT; USINT, UINT, UDINT, ULINT; REAL, LREAL; TIME, LTIME; BYTE, WORD, DWORD, LWORD; STRING | Partial: DATE, TIME_OF_DAY, DATE_AND_TIME and their L forms, CHAR, WCHAR, WSTRING are refused. |
| 11 User-defined types | Enumerations (with explicit values and base type), subranges, arrays, structures, aliases, initial values in type declarations | Yes. Structures with located members or FB instances: no. |
| 12 References | | No. |
| 13 Variable declaration | `VAR`, `VAR_INPUT`, `VAR_OUTPUT`, `VAR_IN_OUT`, `VAR_EXTERNAL`, `VAR_TEMP`, `CONSTANT`, `RETAIN`, `NON_RETAIN` | Partial: `RETAIN` values survive RUN after a fault and a power cycle (retentive memory, `retain.json`), not a cold restart; `VAR_GLOBAL` only as `VAR_GLOBAL CONSTANT` at file level; `VAR_ACCESS`, `VAR_CONFIG`, `AT` refused. |
| 14 Initialization | Elementary, array (with repetition `n(v)`), structure (named members), FB instance inputs | Yes. |
| 15 Variable-length arrays | `ARRAY[*]` | No. |
| 16 Directly represented variables | `%IX0.0` … | No: programs bind tags by name (see Extensions). |
| 17 Partial access of ANY_BIT | `.%Xn` | Partial: bits only; `.%B`, `.%W`, `.%D`, `.%L` refused. |
| 18 EN/ENO | Textual EN and ENO on every call | Yes, with the extension described below. |
| 19 Function declaration | `FUNCTION` with result type or none; inputs, outputs, in-outs, `VAR`, `VAR_TEMP`, constants | Yes. Temporaries are initialised on every call. |
| 20 Function call | Formal, informal, incomplete formal (omitted inputs take their initial values), outputs with `=>` | Yes. Inverted output (`NOT q =>`) refused. |
| 21 Typed and overloaded functions | Overloaded standard functions | Partial: typed names such as `ADD_INT` are not provided. |
| 22 Type conversion | `*_TO_*`, `TO_*`, `*_TRUNC_*`, `TRUNC_*` | Yes. 2a (overloaded `TRUNC`) and BCD conversions refused. |
| 23 Numeric conversions | All pairs of integer and real types | Yes. |
| 24 Bit string conversions | All pairs of BYTE, WORD, DWORD, LWORD | Yes. |
| 25 Bit string and numeric conversions | Integer ↔ bit string | Partial: REAL/LREAL ↔ bit strings (features 1, 2, 43, 44) and signed integer → wider bit string are refused (`E_AMBIGUOUS`). |
| 26 Date and time conversions | `TIME_TO_LTIME`, `LTIME_TO_TIME` | Partial: date types do not exist. TIME ↔ numbers is provided as an implementation choice. |
| 27 Character conversions | | No. |
| 28 Numerical functions | ABS, SQRT, LN, LOG, EXP, SIN, COS, TAN, ASIN, ACOS, ATAN, ATAN2 (feature 12) | Yes. |
| 29 Arithmetic functions | ADD, MUL (extensible), SUB, DIV, MOD, EXPT, MOVE | Yes. |
| 30 Bit shift functions | SHL, SHR, ROL, ROR | Yes. |
| 31 Bitwise Boolean functions | AND, OR, XOR (extensible), NOT | Yes. |
| 32 Selection functions | SEL, MAX, MIN (extensible), LIMIT, MUX (extensible) | Yes. |
| 33 Comparison functions | GT, GE, EQ, LE, LT (extensible, cascaded), NE | Yes. |
| 34 Character string functions | LEN, LEFT, RIGHT, MID, CONCAT (extensible), INSERT, DELETE, REPLACE, FIND | Yes, for STRING. |
| 35 Functions of time types | ADD_TIME, SUB_TIME, MUL_TIME, DIV_TIME and the LTIME forms; `+ - * /` on durations | Partial: the date and time-of-day functions do not exist. |
| 36 Additional time functions | | No. |
| 37 Endianness conversion | | No. |
| 38 Functions of enumerated types | `=` and `<>` | Partial: SEL, MUX, EQ and NE as functions are refused on enumerations. |
| 39 Validate functions | | No. |
| 40 Function block declaration | `FUNCTION_BLOCK` with inputs, outputs, in-outs, `VAR`, `VAR_TEMP`, `VAR_EXTERNAL`, constants, `RETAIN` | Yes. `R_EDGE`/`F_EDGE` (6a, 6b) refused. |
| 41 Function block instances | Declaration in `VAR`, arrays of instances, instances inside instances, initialisation of inputs (feature 2) | Yes. Instances in `VAR_TEMP`, as parameters or in structures: no. |
| 42 Function block call | Formal call, incomplete formal call (omitted inputs keep their values), `inst.IN := x;` (6a), reading outputs | Yes. Informal (positional) calls of blocks are refused. |
| 43 Bistables | SR, RS | Yes. |
| 44 Edge detection | R_TRIG, F_TRIG | Yes. |
| 45 Counters | CTU, CTD, CTUD (INT) and the DINT, LINT, UDINT, ULINT variants | Yes. |
| 46 Timers | TP, TON, TOF (TIME) | Yes; LTIME timers not provided. |
| 47 Program declaration | `PROGRAM` with `VAR`, `VAR_TEMP`, `VAR_EXTERNAL`, constants, `RETAIN` | Partial: program inputs, outputs, in-outs and `VAR_ACCESS` refused. |
| Figure 11 Implicit conversions | Lossless widening only | A subset [confirm]: see below. |
| Figure 13 FB parameter usage | All rules | Yes. |

### Structured Text

| Ed. 3 table | Features | Status |
|---|---|---|
| 71 Operators | `( )`, call, `**`, unary `-` `+`, `NOT`, `* / MOD`, `+ -`, `< > <= >=`, `= <>`, `AND`/`&`, `XOR`, `OR` | Yes, in the order of Table 71 (relational above equality, as in the grammar), with the refusals below. |
| 72 Statements | 1a assignment; 1b assignment with implicit conversion (Figure 11); 1c assignment of arrays and structures; 2 function and FB call; 3 RETURN; 4 IF; 5 CASE; 6 FOR; 7 WHILE; 8 REPEAT; 9 CONTINUE; 10 EXIT; 11 empty statement | Yes. |

Operator precedence, highest first: `( )` and calls; unary `-`, `+`, `NOT`; `**`;
`* / MOD`; `+ -`; `< > <= >=`; `= <>`; `AND &`; `XOR`; `OR`. Equal precedence associates left
to right. The forms on which Ed. 2, Ed. 3 and vendors disagree are refused (see
[Strictness](#strictness-errors-where-the-standard-gives-a-meaning)), so the difference between
Table 71 (relational and equality operators at one level) and the grammar (relational binds
tighter) cannot change the meaning of an accepted program.

## Implementation-dependent parameters

| Parameter | SimplePLC |
|---|---|
| Characters of identifiers | ASCII letters, digits, `_` |
| Maximum identifier length | 64 characters (`E_LIMIT`) |
| Maximum comment length | none |
| Pragmas | none defined; all ignored with `W_PRAGMA`, conditional compilation refused |
| STRING default length | 254 bytes (`STRING` = `STRING[254]`) |
| STRING maximum length | 65,535 bytes; string literals likewise |
| STRING contents | bytes; text is stored as UTF-8; LEN, positions and comparisons count bytes |
| Maximum array dimensions | 8 |
| Maximum array elements | 16,777,216 per array |
| Maximum size of a type or instance | 16 MB |
| Maximum memory of a program (variables and stack) | 16 MB |
| Enumeration values, structure members, POUs, parameters | no fixed limit (within the memory limit) |
| Nesting of statements, expressions and calls | 100 levels, statements and expressions counted together; an expression at most 400 operations deep (`E_LIMIT`). Recursion is refused, so the stack depth is computed at compile time. |
| Integer types | two's complement, 8/16/32/64 bits |
| REAL, LREAL | IEEE 754 binary32 and binary64; each computed in its own width; round to nearest even. Results are bit-identical on every platform: the transcendental functions (SIN, COS, TAN, ASIN, ACOS, ATAN, ATAN2, EXP, LN, LOG, EXPT) are musl 1.2.6's, built into SimplePLC with no fused multiply-add; a REAL result is the LREAL function rounded once. Measured error on 50,000 inputs each: LREAL at most 0.86 ulp (ATAN2 1.29 ulp), REAL at most 0.5 ulp; SQRT is correctly rounded |
| TIME, LTIME representation | signed 64-bit count of nanoseconds |
| Range of TIME and LTIME values | ±2⁶³ ns (about ±106,751 days) |
| Range of duration literals | the whole range, `T#-106751d23h47m16s854ms775us808ns` to `T#106751d23h47m16s854ms775us807ns` |
| Resolution of TIME and LTIME | 1 ns; fractions in literals round to the nearest ns, ties to even |
| Time seen by timers | one value per cycle: the cycle's planned start; tasks run every 1…60,000 ms |
| Default initial values | 0, 0.0, FALSE, `''`, `T#0s`; first enumeration value; subrange lower limit; an alias's own default |
| Retentive variables | `RETAIN` variables keep their values through RUN after a fault (warm restart) and across a power cycle (written to `retain.json` when they change, at most once a second, and at shutdown); a cold restart resets them |
| Number of tasks | up to 8 per PLC, each running one program |
| Scheduling | with `"realtime": true`, tasks with a `priority` (1…98) run with real-time scheduling (SCHED_FIFO on Linux, the Mach time-constraint policy on macOS); the Modbus server and the links run one priority below the lowest real-time task |
| Cycle instruction budget | 50,000,000 VM instructions per cycle; string functions count one per byte, copies of arrays, structures and strings one more per 8 bytes (`F_BUDGET`) |
| Cycle watchdog | `watchdog_ms` per task, default the larger of 100 ms and 10 × the cycle time (`F_WATCHDOG`) |
| Counter limits PVmax, PVmin | the limits of the counter's type (INT: 32767, −32768; unsigned variants: 0) |
| Value of the FOR control variable after the loop | the first value that failed the test (`FOR i := 0 TO 5` leaves 6); if that value is outside the type, the last value; after EXIT, the value at the EXIT |

## Implementation-dependent behaviour and choices

| Item | SimplePLC |
|---|---|
| Integer overflow (`+ - *`, negation, ABS, TIME arithmetic) | run-time fault `F_OVERFLOW`; no wrap-around. Constant expressions: `E_OVERFLOW`. |
| Integer division | truncates toward zero; by zero `F_DIV_ZERO` (by a constant 0: `E_DIV_ZERO`); `MIN / -1` `F_OVERFLOW` |
| MOD | `IN1 - (IN1 / IN2) * IN2`, so the sign of the dividend; `x MOD 0` gives 0 [confirm]; `MIN MOD -1` gives 0; a constant 0 divisor is `E_DIV_ZERO` |
| REAL and LREAL exceptions | an infinite or NaN result is `F_REAL`; division by zero `F_DIV_ZERO` (by a constant 0.0: `E_DIV_ZERO`); NaN from `EXPT`/`**` `F_DOMAIN` |
| Evaluation of expressions | operands left to right; both operands of AND and OR always evaluated (no short-circuit) |
| Untyped literals | take the type of the other operand, the assignment target or the parameter, also through `**`, the REAL functions, bit logic, the generic functions and calls with EN/ENO (not through ABS); otherwise DINT (LINT, ULINT when needed) and LREAL; never BOOL or TIME; an integer literal used as REAL must be exactly representable |
| Constant folding | in the literal's type with the run-time rules (`7 / 2` = 3; `W_INT_DIVISION` when an integer division meets a REAL target) |
| Type of an operation | the common type of its operands; `INT * INT` is INT even when stored in a DINT (`W_NARROW_ARITH`) |
| REAL → integer | round to nearest, ties to even [confirm]; NaN, infinite or out of range `F_CONVERSION` |
| Integer → integer conversion | value-preserving; not representable `F_CONVERSION` (a constant: `E_OVERFLOW`) |
| Conversions involving bit strings | bit pattern: narrowing keeps the low bits, widening fills with zeros, same width reinterprets |
| TIME ↔ number | milliseconds for TIME, nanoseconds for LTIME, truncated toward zero; `W_TIME_UNIT` on every such conversion |
| TIME × or ÷ number | an integer divisor truncates toward zero; a REAL factor or divisor rounds to the nearest ns (ties to even) |
| Number → STRING | decimal; REAL as the shortest text that reads back to the same value (`'0.1'`, `'2'`, `'1e+20'`); TIME as a duration literal |
| STRING → number | the whole string must be a decimal number with an optional sign; otherwise `F_CONVERSION` |
| String result longer than its target | run-time fault `F_STRING_LENGTH` (no truncation) [confirm]; `W_STRING_CAPACITY` where possible at compile time |
| String positions | 1-based; every position referred to must exist: LEFT/RIGHT `0 ≤ L ≤ LEN`, MID/DELETE/REPLACE `P ≥ 1`, `L ≥ 0`, `P + L − 1 ≤ LEN`, INSERT `0 ≤ P ≤ LEN`; otherwise `F_STRING_POSITION` [confirm] |
| INSERT | IN2 goes after the P-th character (P = 0: in front): `INSERT('ABC', 'XY', 2) = 'ABXYC'` [confirm] |
| LEN, FIND result | INT, or DINT when a string argument's capacity is above 32,767 bytes |
| MUX selector out of range | `F_INDEX` (constant: `E_INDEX`) [confirm] |
| LIMIT with MN > MX | `MIN(MAX(IN, MN), MX)`, i.e. MX |
| SHL, SHR with N ≥ width | 0, for every integer type of N (ULINT included); ROL, ROR use N modulo the width; N < 0 `F_DOMAIN` |
| MAX, MIN on strings | byte-wise comparison |
| Comparison of strings | byte-wise, shorter prefix first |
| FOR loop | end and step evaluated once; a step that would pass the type's limit ends the loop; constant step 0 `E_FOR_STEP`, run-time step 0 `F_FOR_STEP` |
| CONTINUE in REPEAT | goes to the UNTIL test |
| CASE without matching label and without ELSE | nothing runs |
| Function result never assigned | refused at compile time by flow analysis (`E_RESULT_UNASSIGNED`); loops may run zero times; a CASE that lists every value of an enumeration covers every path |
| Standard, conversion or operator function used as a statement | `E_CALL` (its value would be lost); a user function's result may be ignored |
| EN = FALSE | nothing runs, no inputs assigned, result is the type's default, `=>` targets not written, ENO = FALSE |
| Errors inside a call with ENO | caught at any depth: the called instance, in-out actuals and tags touched are restored; result default; ENO = FALSE (extension) |
| Errors outside an ENO call | the PLC goes to FAULT (below) |
| TON, TP with PT = `T#0s` | TON: Q in the same call as IN [confirm]; TP: no pulse, Q stays FALSE (PT = 0 resets the output, as in Fernhill and CODESYS) [confirm]; a negative PT acts as 0 |
| F_TRIG on the first call with CLK FALSE | Q is TRUE (the standard's body, M initially FALSE) [confirm] |
| CTU, CTD, CTUD | CV saturates at the type's limits; CTU counts past PV and CTD below 0; CU and CD edges in one CTUD call cancel |
| Variables of a function | re-initialised on every call |
| Subrange violation | storing a value outside the range is `F_SUBRANGE` |
| Array index out of bounds | `F_INDEX` (constant: `E_INDEX`) |
| Reading an FB input from outside | refused (`E_ACCESS`), as Figure 13 requires |
| Writes to program variables (monitoring) | checked like assignments (type, subrange, enumeration names, REAL range); constants and tags are refused; all values are checked before any is written |
| float32 tags | Modbus writes (FC6, FC16, FC22, FC23) that would make one NaN or infinite get exception 3, and the API's raw writes are refused the same way; a non-finite value is never copied into a program (the last finite value stays) |
| A saved program that does not compile at start-up | the version that last loaded runs, with a warning |
| Another program file with syntax errors | left out of the libraries with `W_LIBRARY` |

**Implicit conversions (Figure 11 subset).** SINT → INT, DINT, LINT, REAL, LREAL; INT → DINT,
LINT, REAL, LREAL; DINT → LINT, LREAL; USINT → UINT, UDINT, ULINT, INT, DINT, LINT, REAL,
LREAL; UINT → UDINT, ULINT, DINT, LINT, REAL, LREAL; UDINT → ULINT, LINT, LREAL; BYTE → WORD →
DWORD → LWORD; REAL → LREAL; TIME → LTIME. Every one is exact. Programs accepted under this
set are valid under any reading of Figure 11; some conversions Figure 11 may allow are refused
[confirm].

**Run-time errors at the PLC level.** A run-time fault in any task puts the PLC in FAULT:
every task stops, tags with a configured substitute value get it, nothing from the faulted cycle is
copied out, and the code, message, file, line, cycle and call chain are recorded.
Communication (Modbus TCP, OPC UA, web panel, API) keeps running. RUN after a fault is a warm
restart: `RETAIN` variables keep their values, all others take their initial values, and
`FIRST_CYCLE()` is TRUE in the first cycle. STOP writes the substitute values too.

## Strictness: errors where the standard gives a meaning

These programs are valid ST, but SimplePLC refuses them (an overflow: stops them at run time),
because vendors or editions read them differently, or because they cause silent errors:

| Construct | Code | Reason |
|---|---|---|
| Unary operator on the left operand of `**` (`-a ** b`) | `E_AMBIGUOUS` | Ed. 2 Table 55 and Ed. 3 Table 71 order negation and `**` differently. |
| Chained `**` (`a ** b ** c`) | `E_AMBIGUOUS` | Associativity of `**` differs between implementations. |
| `NOT a = b` | `E_AMBIGUOUS` | The standard reads `(NOT a) = b`; many programmers read `NOT (a = b)`, and for bit strings the results differ. |
| A comparison as an operand of a comparison (`a < b < c`, `a = b < c`) | `E_CHAINED_COMPARISON` | Table 71 and the grammar disagree on relational vs equality precedence; some vendors treat chains as ranges. |
| A bare enumeration value of two types where no type decides | `E_AMBIGUOUS` | The value's type would be a guess. |
| Integer arithmetic result that overflows | `F_OVERFLOW` | Wrap-around is a silent wrong value. |
| `REAL_TO_DWORD`, `DWORD_TO_REAL`, `LREAL_TO_LWORD`, `LWORD_TO_LREAL` and every REAL/LREAL ↔ bit string conversion | `E_AMBIGUOUS` | Ed. 3 copies the bits; MatIEC and others convert the value. |
| Signed integer → wider bit string (`INT_TO_DWORD`) | `E_AMBIGUOUS` | Sign extension differs between implementations. |
| Two writers of one tag (two tasks, or a task and a link) | program not loaded; `E_BINDING` when checked | The order of the writes would be arbitrary. |
| A name that shadows a POU, type, global constant or enumeration value | `E_DUPLICATE` | One name would have two meanings in one place. |
| Nested comments | `E_COMMENT` | Optional in Ed. 3 (3a, 3b); the text after an inner opener would be live or dead depending on the reader. |
| Conditional-compilation pragmas | `E_PRAGMA` | Ignoring them would compile both branches. |
| Division or MOD by a constant 0 | `E_DIV_ZERO` | The program would stop there every time. |
| A standard function used as a statement (`CONCAT(s, t);`) | `E_CALL` | The value it computes would be lost. |

<!-- wrap: body -->
<!-- vars: a, b, c : REAL; -->
<!-- expect: E_AMBIGUOUS -->
```st
c := -a ** b;
```

## Refused features

| Feature | Ed. 3 reference | Code | Reason |
|---|---|---|---|
| IL, LD, FBD, SFC | the other languages | — | ST only |
| `CONFIGURATION`, `RESOURCE`, `TASK`, `VAR_CONFIG` | Table 62 | `E_NOT_SUPPORTED` | tasks and tags are configuration |
| `VAR_GLOBAL` (other than `CONSTANT`), `VAR_ACCESS` | Tables 13, 62 | `E_NOT_SUPPORTED` | programs share data through tags |
| `AT` and direct addresses (`%IX0.0`) | Table 16 | `E_NOT_SUPPORTED` | tags are bound by name |
| Program `VAR_INPUT`, `VAR_OUTPUT`, `VAR_IN_OUT` | Table 47 | `E_NOT_SUPPORTED` | programs exchange data through tags |
| WSTRING, CHAR, WCHAR and their literals | Tables 6, 10, 27 | `E_NOT_SUPPORTED` | not yet implemented |
| DATE, TIME_OF_DAY, DATE_AND_TIME, LDATE, LTOD, LDT and their literals | Tables 9, 10, 26, 36 | `E_NOT_SUPPORTED` | not yet implemented |
| References (`REF_TO`, `REF`, `^`, `NULL`) | Table 12 | `E_NOT_SUPPORTED` | memory safety by construction |
| `ARRAY[*]` | Table 15 | `E_NOT_SUPPORTED` | |
| Partial access `.%B`, `.%W`, `.%D`, `.%L` | Table 17 | `E_NOT_SUPPORTED` | |
| `R_EDGE`, `F_EDGE` qualifiers | Table 40 (6a, 6b) | `E_NOT_SUPPORTED` | use R_TRIG, F_TRIG |
| `PERSISTENT`, `VAR_STAT` | vendor extensions | `E_NOT_SUPPORTED` | |
| Classes, methods, interfaces, properties, `EXTENDS`, `IMPLEMENTS`, `THIS`, `SUPER`, access specifiers | Tables 48–61 | `E_NOT_SUPPORTED` | |
| `NAMESPACE`, `USING` | Table 64 | `E_NOT_SUPPORTED` | |
| `JMP` and labels | IL | `E_NOT_SUPPORTED` / `E_SYNTAX` | |
| Overloaded `TRUNC` without result type | Table 22 (2a) | `E_NOT_SUPPORTED` | deprecated in Ed. 3 |
| BCD conversions | Table 22 | `E_NOT_SUPPORTED` | not yet implemented |
| Conversions between enumerations and integers | Table 38 | `E_TYPE` | not implemented |
| Typed function names (`ADD_INT`) | Table 21 | `E_UNDECLARED` | use the overloaded names |
| Enumerations and structures declared inside `VAR` | Tables 11, 13 | `E_NOT_SUPPORTED` | declare them in `TYPE` |
| FB instances in `VAR_TEMP`, `VAR_INPUT`, `VAR_OUTPUT`, `VAR_IN_OUT`, structures; any instance in a FUNCTION | Table 41 | `E_NOT_SUPPORTED`, `E_FB_IN_FUNCTION` | |
| Informal function block call | Table 42 | `E_CALL` | |
| Inverted output assignment (`NOT q => x`) | Table 20 | `E_NOT_SUPPORTED` | |
| Recursion (direct, indirect, or an instance containing itself) | forbidden by the standard | `E_RECURSION` | |
| `ELSEIF`, `!=`, `STRING(n)` | vendor dialects | `E_SYNTAX` | the hint gives the IEC form |
| `AND_THEN`, `OR_ELSE` (short-circuit operators) | vendor extensions | `E_SYNTAX` | |

<!-- wrap: decl -->
<!-- expect: E_NOT_SUPPORTED -->
```st
VAR
    today : DATE;
END_VAR
```

## Extensions

| Extension | Description |
|---|---|
| `VAR_EXTERNAL` bound to tags | A `VAR_EXTERNAL` variable binds to the PLC tag of the same name (case-insensitive) when the program is loaded; types must match (bool ↔ BOOL, uint16 ↔ UINT or WORD, int16 ↔ INT, uint32 ↔ UDINT or DWORD, int32 ↔ DINT, float32 ↔ REAL), else `E_BINDING`. `VAR_EXTERNAL CONSTANT` binds read-only. Tags are copied in at the start of a cycle and copied out (only those the program changed) at its end. |
| `VAR_GLOBAL CONSTANT` at file level | Compile-time constants (elementary types, STRING, enumerations) shared by every POU of the PLC. |
| Library files | Every other program file of the PLC is compiled with a program as a library (its PROGRAM excluded). |
| `FIRST_CYCLE()` | TRUE in the first cycle after power-up, a restart or RUN after a fault. |
| Bit access `w.3` | Short form of `w.%X3`. |
| Counters `CTU_DINT`, `CTU_LINT`, `CTU_UDINT`, `CTU_ULINT` and the same for CTD, CTUD | Counters with wider PV and CV. |
| ENO catches at any depth | A fault anywhere inside a call with ENO connected is caught; the call's effects are undone. |
| TIME ↔ number conversions | Milliseconds (LTIME: nanoseconds), with `W_TIME_UNIT`. |
| One writer per tag | Two writers of a tag are refused when a program is loaded. |
| Warnings | `W_NARROW_ARITH`, `W_INT_DIVISION`, `W_TIME_UNIT`, `W_STRING_CAPACITY`, `W_CASE_INCOMPLETE`, `W_UNUSED`, `W_PRAGMA`, `W_LIBRARY`. |

<!-- run: cycles=2 -->
<!-- assert: 1 first=TRUE -->
<!-- assert: 2 first=FALSE -->
```st
PROGRAM main
VAR_EXTERNAL
    status_word : WORD;
END_VAR
VAR
    first : BOOL;
    ready : BOOL;
END_VAR
    first := FIRST_CYCLE();
    ready := status_word.3;
END_PROGRAM
```

## Run-time faults

| Code | Cause | Caught by ENO |
|---|---|---|
| `F_OVERFLOW` | integer or TIME result outside its type | yes |
| `F_DIV_ZERO` | division by zero (integer, REAL, TIME) | yes |
| `F_REAL` | REAL or LREAL result infinite or NaN | yes |
| `F_DOMAIN` | argument outside a function's domain (SQRT, LN, LOG, ASIN, ACOS, EXPT, negative shift count) | yes |
| `F_INDEX` | array index or MUX selector out of range | yes |
| `F_SUBRANGE` | value outside a subrange stored | yes |
| `F_STRING_LENGTH` | string longer than its target | yes |
| `F_STRING_POSITION` | string position or length outside the string | yes |
| `F_CONVERSION` | conversion result not representable | yes |
| `F_FOR_STEP` | FOR step 0 at run time | yes |
| `F_BUDGET` | instruction budget of the cycle spent | no |
| `F_WATCHDOG` | cycle longer than the task's watchdog | no |
| `F_BINDING` | a bound tag was removed or changed type | no |
| `F_INTERNAL` | VM invariant broken (should never happen) | no |

## Compile-time diagnostics

Errors: `E_SYNTAX`, `E_COMMENT`, `E_PRAGMA`, `E_LITERAL`, `E_LITERAL_RANGE`, `E_LIMIT`,
`E_RESERVED`, `E_UNDECLARED`, `E_DUPLICATE`, `E_TYPE`, `E_NARROWING`, `E_AMBIGUOUS`,
`E_CHAINED_COMPARISON`, `E_OVERFLOW`, `E_DIV_ZERO`, `E_INDEX`, `E_CASE_OVERLAP`, `E_FOR_VAR`,
`E_FOR_STEP`, `E_EXIT_OUTSIDE_LOOP`, `E_RECURSION`, `E_CONSTANT_WRITE`, `E_INPUT_WRITE`,
`E_OUTPUT_WRITE`, `E_ACCESS`, `E_BINDING`, `E_FB_IN_FUNCTION`, `E_CALL`,
`E_RESULT_UNASSIGNED`, `E_NOT_SUPPORTED`, `E_INTERNAL`. Warnings: `W_NARROW_ARITH`,
`W_INT_DIVISION`, `W_TIME_UNIT`, `W_STRING_CAPACITY`, `W_CASE_INCOMPLETE`, `W_UNUSED`,
`W_PRAGMA`, `W_LIBRARY`. Each
diagnostic has a code, severity, file, line, column, message and usually a hint; their
meanings are listed in `docs/structured-text.md`.

How the error conditions the standard names are handled:

| Error condition | Detected | Code |
|---|---|---|
| Value outside a subrange | compile time for constants, else run time | `E_LITERAL_RANGE`, `F_SUBRANGE` |
| Typed literal or constant out of its type's range | compile time | `E_LITERAL_RANGE` |
| Numeric result outside its type | compile time for constants, else run time | `E_OVERFLOW`, `F_OVERFLOW`, `F_REAL` |
| Division by zero | compile time for constants, else run time | `E_DIV_ZERO`, `F_DIV_ZERO` |
| Data type not valid for an operation or parameter | compile time | `E_TYPE`, `E_NARROWING` |
| Type conversion result not representable | compile time for constants, else run time | `E_OVERFLOW`, `F_CONVERSION` |
| Array subscript out of range | compile time for constants, else run time | `E_INDEX`, `F_INDEX` |
| MUX selector out of range | compile time for constants, else run time | `E_INDEX`, `F_INDEX` |
| Character position out of range; string too long | run time (warning at compile time) | `F_STRING_POSITION`, `F_STRING_LENGTH` |
| Assignment to a CONSTANT | compile time | `E_CONSTANT_WRITE` |
| Assignment to an input inside its POU | compile time | `E_INPUT_WRITE` |
| FB parameter used against Figure 13 | compile time | `E_ACCESS`, `E_OUTPUT_WRITE` |
| Function returns without assigning its result | compile time | `E_RESULT_UNASSIGNED` |
| Recursion | compile time | `E_RECURSION` |
| CASE value in two labels | compile time | `E_CASE_OVERLAP` |
| FOR control variable or bound changed in the loop | compile time | `E_FOR_VAR` |
| Loop that does not terminate | run time | `F_BUDGET`, `F_WATCHDOG` |
| Ambiguous enumeration value | compile time | `E_AMBIGUOUS` |
| Missing or extra parameters, VAR_IN_OUT not given | compile time | `E_CALL` |
| Nesting or expression depth beyond the limits | compile time | `E_LIMIT` |

## Known deviations in this version

Behaviour of this build that differs from SimplePLC's own design decisions, and features the
design plans but this version does not have yet.

| Area | Expected | This version |
|---|---|---|
| Array index of type ULINT, 2⁶³ or more | `F_INDEX` | `F_CONVERSION`. |
| Enumeration ↔ integer and BCD conversions | Provided | Not implemented (`E_TYPE`, `E_NOT_SUPPORTED`). |
| `RETAIN` | Values kept across a power cycle | Yes: written to `retain.json` when they change (at most once a second) and at shutdown, restored at power-up; a cold restart resets them. |

## Open items [confirm]

Decisions taken from public sources that must be checked against the purchased text of
IEC 61131-3 before an external audit:

1. All Ed. 3 table and feature numbers in this document.
2. Comments: Ed. 3 Table 3 makes nested comments optional (3a, 3b); refusing them is assumed
   conformant.
3. Durations: only the most significant unit may exceed its range (`T#90s` accepted,
   `T#1h75m` refused).
4. String escapes: `$N` and lower-case escapes accepted.
5. Boolean literals 0 and 1 (Table 5 feature 7): accepted only in the typed form `BOOL#0`, `BOOL#1`.
6. Implicit conversions: the lossless set above is a subset of Figure 11.
7. `VAR_INPUT` is read-only inside a FUNCTION as well as inside a FUNCTION_BLOCK (Figure 13
   covers blocks).
8. MOD by zero gives 0 (Ed. 3 Table 29 definition as quoted by Fernhill).
9. REAL → integer rounds half to even (IEC 60559, as Fernhill reads Table 22).
10. Overloaded `TRUNC` deprecated in Ed. 3 (Table 22 2a).
11. REAL/LREAL ↔ DWORD/LWORD are bit copies in Ed. 3 (Table 25 features 1, 2, 43, 44); they are
    refused until confirmed.
12. String functions: positions outside the string are errors; INSERT inserts after position P.
13. A string result longer than its target is an error (TF3 proposed truncation for Ed. 3).
14. MUX with K out of range is an error.
15. TON and TP with PT = `T#0s`: the standard gives timing diagrams only; TON sets Q at once,
    TP gives no pulse (Fernhill: "PT = T#0s resets the output"; CODESYS alike).
16. F_TRIG pulses on the first call with CLK FALSE (the standard's body); IEC 61131-8 recommends
    not to. An owner decision is pending.
17. Counters: PVmax and PVmin are the limits of the counter's type.
18. LEN and FIND return INT (DINT for strings that can exceed 32,767 bytes).
19. Unary minus and `**`: the precedence in Ed. 3 Table 71 vs Ed. 2 Table 55 (reason for the
    `E_AMBIGUOUS` refusal).
20. Relational and equality operators: same level in Table 71, different in the grammar (reason
    for `E_CHAINED_COMPARISON`).
21. Figure 13 (FB parameter usage) as reproduced by logi.cals.
22. CONTINUE is Table 72 feature 9.
23. The default initial value of a subrange is its lower limit.
24. The value of the FOR control variable after the loop is implementation-dependent (Ed. 2
    §3.3.2.4).
25. Changes in Ed. 4 (2025).
