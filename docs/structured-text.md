# Structured Text in SimplePLC

SimplePLC runs programs written in IEC 61131-3 Structured Text (ST). The compiler checks a
program completely before it runs; the PLC runs it as verified bytecode. This page says what
the compiler accepts, what it refuses, and what each construct does at run time. Every example
was compiled and run with this version. IEC details are in the compliance statement
(`docs/st-compliance.md`, MCP tool `get_compliance`).

The rule: **refuse rather than guess.** Where vendors or readers disagree on what code means,
the compiler refuses it and says how to write it. Overflow, division by zero, a bad index and
similar errors stop the PLC (a fault) instead of producing a wrong value.

## A first program

A conveyor with start and stop buttons that counts parts. The tags (`start_button`,
`motor`, ...) are in the tag table shown under [Tags](#tags-var_external).

<!-- run: cycles=6 set=start_button=TRUE@2 set=start_button=FALSE@3 set=part_sensor=TRUE@4 set=part_sensor=FALSE@5 set=stop_button=TRUE@6 -->
<!-- assert: 3 motor=TRUE part_count=0 -->
<!-- assert: 4 part_count=1 -->
<!-- assert: 6 motor=FALSE part_count=1 -->
```st
(* Conveyor: start/stop with a seal-in, and a part counter. *)
PROGRAM main
VAR_EXTERNAL
    start_button : BOOL;    (* coil: written by the HMI *)
    stop_button : BOOL;
    part_sensor : BOOL;
    motor : BOOL;           (* discrete input: only the program writes it *)
    part_count : UDINT;
END_VAR
VAR
    part_seen : R_TRIG;     (* rising-edge detector *)
END_VAR
    IF stop_button THEN
        motor := FALSE;
    ELSIF start_button THEN
        motor := TRUE;
    END_IF;

    part_seen(CLK := part_sensor);
    IF part_seen.Q AND part_count < 4294967295 THEN
        part_count := part_count + 1;
    END_IF;
END_PROGRAM
```

- `VAR_EXTERNAL` binds a variable to the PLC tag of the same name; `VAR` variables keep their
  values from one cycle to the next.
- `part_seen` is an instance of the standard function block `R_TRIG`. A block is called as a
  statement, and its outputs are read afterwards (`part_seen.Q`).
- Arithmetic never wraps: `part_count + 1` at the maximum would stop the PLC (`F_OVERFLOW`),
  so the program checks the limit first.

## Program files and libraries

- A PLC has up to 8 **tasks**. Each task runs one program every `cycle_ms` milliseconds, 10 by
  default (configuration: `"tasks": [{"name": "main", "program": "main", "cycle_ms": 10}]`).
- A **program file** (`main.st`) holds exactly one `PROGRAM … END_PROGRAM` (give it the file's
  name: variables show as `MAIN.COUNT`), plus any `TYPE`, `FUNCTION`, `FUNCTION_BLOCK` and
  `VAR_GLOBAL CONSTANT`, in any order.
- Every other program file of the PLC is a **library** of it: its types, functions, blocks
  and constants can be used, its PROGRAM is ignored. On the PLC a program compiles with the
  library files it uses (that declare a name it mentions, and the files those use): a library
  nothing uses costs nothing, and its errors do not stop other programs. A library it uses that
  has syntax errors is left out with warning `W_LIBRARY`.
- A file without a PROGRAM that no task runs is a library file: saving or checking it checks
  every declaration in it (`simpleplc check util.st` does the same).
- Names are global across the files a program uses and case-insensitive. Declaring the same
  name twice (in one file or in two) is an error (`E_DUPLICATE`). Program file names are
  case-insensitive too: `MAIN` is the program `main`.
- A program that does not compile (or that a task refuses, see one writer per tag) is not
  loaded: it is kept as a **draft**. The previous version keeps running, also after a
  restart or a power cycle: each program runs the set of files it last loaded with (its
  *deployed* files; API `GET /api/v1/plcs/{id}/programs/{name}/deployed`, MCP `get_program`
  with `deployed`). Saving with `?deploy=false` (MCP `deploy: false`) keeps a draft on purpose.
- Saving a library loads it into the running programs that use it (an online change). A
  program that does not compile with it keeps the version it runs, and the save lists it
  (`dependents`, and `results` with the program's name). A library a loaded program uses
  cannot be deleted; a deleted file stays in its history and can be restored.
- The program files of a PLC hold at most 1 MB each and 2 MB together.

A library file (here `util.st`):

<!-- wrap: lib -->
<!-- name: util -->
```st
TYPE
    Mode : (Off, Manual, Auto);
END_TYPE

VAR_GLOBAL CONSTANT
    MAX_SPEED : UINT := 1500;
END_VAR

FUNCTION Percent : REAL
VAR_INPUT
    part : REAL;
    whole : REAL;
END_VAR
    IF whole = 0.0 THEN
        Percent := 0.0;
    ELSE
        Percent := 100.0 * part / whole;
    END_IF;
END_FUNCTION
```

A program file that uses it:

<!-- with: util -->
<!-- run: cycles=1 -->
<!-- assert: 1 fill=25.0 speed=1500 -->
```st
PROGRAM main
VAR
    op_mode : Mode := Auto;
    fill : REAL;
    speed : UINT;
END_VAR
    fill := Percent(part := 20.0, whole := 80.0);
    IF op_mode = Auto THEN
        speed := MAX_SPEED;
    END_IF;
END_PROGRAM
```

## The cycle

Each cycle of a task does three things:

1. **Copy-in.** Every tag the program binds is read at once, under one lock: a consistent
   snapshot.
2. **Run.** The program runs on its own copy; client writes during the cycle wait for the next.
3. **Copy-out.** The tags the program stored to and changed are written at once, under one
   lock. A client's write during the cycle survives unless the program changed that tag. A
   forced tag keeps its forced value.

So a tag has one value during a cycle, and outputs change together at its end. Every timer in a cycle sees the same time, the cycle's planned start;
time advances in steps of `cycle_ms`. A task whose cycle time is exceeded starts its next cycle at the next slot
of its grid (the slots skipped are counted as exceeded). `VAR` variables, including function block
instances, keep their values between cycles; `VAR_TEMP` variables start afresh in every cycle.
Programs in different tasks have separate variables and share data only through tags. With
`"realtime": true`, tasks with a `priority` get real-time scheduling (Linux SCHED_FIFO, macOS
time-constraint), the Modbus server and the links one level below.

**RUN, STOP and FAULT.**

| Event | What happens |
|---|---|
| STOP | Tasks stop. Tags with a `substitute` value get it (written by `substitute`, not by the program). Variables keep their values. |
| RUN after STOP | Tasks continue with the values they had (warm). |
| Run-time fault in any task | **FAULT**: every task stops, tags with a `substitute` value get it, the fault (code, file, line, cycle) is recorded, nothing from the faulted cycle is copied out. Communication keeps running. |
| RUN after a fault | Warm restart: `RETAIN` variables keep their values, every other variable gets its initial value, and `FIRST_CYCLE()` is TRUE in the first cycle. Tags keep their values. |
| Restart (API `restart`) | Cold start, as at power-up: STOP (the links send the substitute values once more), then communication and the programs close and open again with a new process image, so tags start at 0 and forces are released; every variable, `RETAIN` included, gets its initial value and retentive memory is cleared; `FIRST_CYCLE()` is TRUE. |
| Power-up (process start) | Tags start at 0 and variables from their initial values, except what [retentive memory](#retentive-memory) holds. Every program runs the files it last loaded with; a draft saved since does not run. The PLC powers up in the mode commanded last, as a hardware PLC does: in STOP when an operator or a token stopped it before the process ended (a fault is not a STOP: the PLC powers up in RUN), otherwise in RUN; `"autostart": false` is STOP at every power-up. A PLC that powers up in STOP gives tags with a `substitute` value their substitute value, as STOP does. |
| Process stop (SIGTERM, SIGINT, `docker stop`) | STOP, then every link writes its `write` tags once more, so the devices get the substitute values before the process ends (each link within its `timeout_ms`). |
| Saving a program (online change) | Takes over at the next cycle boundary. A variable keeps its value when it has the same path and its type the same definition (an enumeration whose values changed goes over by name); `VAR CONSTANT` takes the new values. The save lists in `reset` every variable that starts from its initial value. |
| Changing tasks (add, remove, program, priority, enabled, watchdog, real-time) | Only the tasks that change are affected; the others keep running with their values. A task that stays keeps its program's values; one that is new or switches program starts from initial values (the answer lists it in `restarted_tasks`). A task naming a program that does not exist, or one that does not compile, is refused. |
| Changing tags | A change that would make a running program fail to compile is refused (API 409 `tag_in_use`, with the programs and their diagnostics). Forced (`?force=1`, MCP `force`), it is applied and the program faults with `F_BINDING`. |

**Before a download.** Checking a program that a task runs (`POST /api/v1/plcs/<id>/programs/<name>/check`,
MCP `check_program`) also says what saving it would do to the running values, by the same rule:
`"online_change": {"tasks": ["main"], "kept": 12, "reset": ["MAIN.TOTAL"]}`: how many variables keep
their values, and which would start from their initial value.

**Who changed what.** Every change is an event with its author: program downloads and online
changes (with how many values were kept and reset), draft saves, forces and releases, value writes
from the panel or the API, changes of tags, links, tasks and settings, RUN, STOP and cold restart,
faults, links going up or down, and changes of users and tokens. `GET /api/v1/plcs/<id>/events?limit=50`
(MCP `get_events`) gives the newest, oldest first: `{seq, t_ms, event, message, by}`, where `event`
is `program`, `force`, `release`, `write`, `config`, `mode`, `link` or `access`, and `by` is
`{"kind": "user" | "token" | "system", "name": ...}`: a signed-in user, an API token (agents use
tokens), or SimplePLC itself (faults, links). A change made through a fleet names who asked the fleet
and the token the fleet signs in with: `"tommy via fleet:line2"` (the fleet sends the header
`X-SimplePLC-For`, which only an admin token's request may carry and which grants nothing). The PLC's
summary has the newest change a user or token made as `last_change`, and the log records of events
carry `event` and `by` too.

**Forcing.** A forced tag keeps its forced value against the program, clients and links, also in
STOP and FAULT, until it is released. A force belongs to its tag: when the tag
table changes, a tag that moved takes its force along, and the force of a tag that is gone or has
another type is released (both logged). A force released in STOP or FAULT leaves the tag at its
`substitute` value; released in RUN, the tag keeps the value until the program or a client writes it.

## Retentive memory

Tags with `"retain": true` and `RETAIN` variables keep their values across a power cycle. They are
written to `<data>/retain.json` when they change, at most once a second, and once more when the
process stops; at power-up they are restored before the programs start. The tags come back before
STOP writes the substitute values, so a retained output that powers up in STOP gets its substitute
value. A power cut loses at most the last second. A cold restart (API `restart`) and a setup clear
retentive memory. A stored value whose tag or variable is gone, is no longer retentive or has
another type is skipped and named in the log; the next write drops it. Everything else starts at 0
or from its initial value: set other defaults in the first cycle (see the [cookbook](#cookbook)), or
let an HMI or another device write them. On an SD card, keep retentive values to what changes
seldom (setpoints, totals): a value that changes every cycle rewrites the file once a second.

## Tags: VAR_EXTERNAL

Tags name addresses in the PLC's four Modbus areas. Each tag has a `name`, an `area`, an
`address`, a `type`, and optionally a `substitute` value (written at STOP and FAULT) and a
`description`.

| Area | Entries | Written by clients (Modbus, OPC UA) | Use it for |
|---|---|---|---|
| `coil` | bits | yes | commands and inputs that come from outside: buttons, remote sensors |
| `discrete_input` | bits | no | outputs only the program sets: motor commands, alarms |
| `holding_register` | 16-bit words | yes | setpoints, values from remote I/O |
| `input_register` | 16-bit words | no | measurements and states the program computes |

The program can write every area; the input areas only protect a value from clients.
32-bit types use two registers, high word first.

The examples on this page use this tag table:

<!-- tags -->
```json
[
  {"name": "start_button", "area": "coil", "address": 0, "type": "bool"},
  {"name": "stop_button", "area": "coil", "address": 1, "type": "bool"},
  {"name": "part_sensor", "area": "coil", "address": 2, "type": "bool"},
  {"name": "reset_count", "area": "coil", "address": 3, "type": "bool"},
  {"name": "speed_sp", "area": "holding_register", "address": 0, "type": "uint16"},
  {"name": "status_word", "area": "holding_register", "address": 1, "type": "uint16"},
  {"name": "offset", "area": "holding_register", "address": 2, "type": "int16"},
  {"name": "level_raw", "area": "holding_register", "address": 3, "type": "uint16"},
  {"name": "delta", "area": "holding_register", "address": 4, "type": "int32"},
  {"name": "motor", "area": "discrete_input", "address": 0, "type": "bool", "substitute": false},
  {"name": "lamp", "area": "discrete_input", "address": 1, "type": "bool", "substitute": false},
  {"name": "alarm", "area": "discrete_input", "address": 2, "type": "bool"},
  {"name": "remote_ok", "area": "discrete_input", "address": 3, "type": "bool"},
  {"name": "part_count", "area": "input_register", "address": 0, "type": "uint32"},
  {"name": "level", "area": "input_register", "address": 2, "type": "float32"},
  {"name": "state_no", "area": "input_register", "address": 4, "type": "uint16"},
  {"name": "remote_level", "area": "input_register", "address": 5, "type": "float32"}
]
```

**Binding.** `VAR_EXTERNAL name : TYPE;` binds `name` to the tag with that name, ignoring
case. The declared type must match the tag's type:

| Tag type | ST type |
|---|---|
| `bool` | `BOOL` |
| `uint16` | `UINT` or `WORD` |
| `int16` | `INT` |
| `uint32` | `UDINT` or `DWORD` |
| `int32` | `DINT` |
| `float32` | `REAL` |

- A missing tag, or a type that does not match, is `E_BINDING` (the hint names the closest tag
  or the right type). There are no LREAL, LINT, TIME or STRING tags: keep durations in a DINT
  or UDINT tag of milliseconds, for example `T#1ms * speed_sp`.
- `VAR_EXTERNAL CONSTANT` binds read-only: writing it is `E_CONSTANT_WRITE`.
- `VAR_EXTERNAL` is allowed in a PROGRAM and in a FUNCTION_BLOCK, not in a FUNCTION. It has
  no initial value: the tag holds the value.
- **One writer per tag.** A program that stores to a tag writes it. When two tasks' programs
  both write a tag, or a program writes a tag that a link fills, the second one is refused when
  it is loaded (the program does not load; the message names the other writer), and checking
  it reports `E_BINDING`. A program that only reads a tag is not a writer; declare it
  `VAR_EXTERNAL CONSTANT` to say so.
- Modbus writes (FC6, FC16, FC22, FC23) that would make a `float32` tag NaN or infinite get
  exception 3, and so do the API's raw area writes (HTTP 422): a write of one of its two
  registers is judged with the other one as the tag holds it, so a client that writes a float
  one register at a time still can. A tag that still holds one is not copied in: the program
  keeps the last finite value (counted as `nan_rejected`).
- A `float32` value is rounded to the nearest float first: the largest, 3.4028235e+38 as it is
  shown, is accepted back; a value that would round to infinity is refused.
- When the tag table changes, programs are bound again at the next cycle. If a bound tag was
  removed or changed type, the task faults with `F_BINDING`.

<!-- wrap: decl -->
<!-- expect: E_BINDING -->
```st
VAR_EXTERNAL
    speed_sp : INT;     (* tag 'speed_sp' is uint16; declare it as UINT or WORD *)
END_VAR
```

**Links (Modbus TCP client without code).** A PLC reads and writes other Modbus TCP devices through
links in its configuration, not through program code:

```json
"links": [{"name": "tank", "host": "tank", "port": 502, "unit": 1,
           "period_ms": 200, "timeout_ms": 150, "status_tag": "remote_ok",
           "read": [{"tag": "remote_level", "area": "input_register", "address": 0}],
           "write": [{"tag": "motor", "area": "coil", "address": 0}]}]
```

Every `period_ms` the link reads the `read` entries into their tags (in one step) and writes
the `write` tags that changed (all of them every 10 periods and after a new connection). The
link is the writer of its read tags and status tag.

- **Status.** The `status_tag` is TRUE while the link's cycles succeed and goes FALSE the moment
  none has for `stale_ms` (default 3 × `period_ms` + `timeout_ms`; at least `period_ms`). A cycle
  succeeds when every request of it does: a refused request or a value that is not a number
  counts too. The link writes the tag in every cycle, and it must be a `discrete_input` tag, so
  no Modbus or OPC UA client can make it say TRUE. A link with nothing to send in a cycle reads
  its first address, so its status always says whether the device answers.
- **Failures.** Each request stands on its own: one the device refuses (say, an address it does
  not have) costs only its own tags, and the other reads, and above all the writes of the safe
  values at STOP, still go out. A NaN or infinity from the device never reaches a `float32` tag;
  the tag keeps its last value. The link's `last_error` names the request and its tags, for
  example `read input_register 100 (tag far): device returned exception 2 (illegal data address)`.
  One answer that comes too late is retried at the next cycle; only a device that fails three
  times in a row is given a second between attempts.
- **Down.** An unreachable device leaves the read tags at their last value: check the status
  tag (see the [cookbook](#cookbook)). OPC UA serves them as `Uncertain_LastUsableValue` while
  the status is FALSE.
- **Outputs.** In RUN the link sends nothing before the programs' first cycle, so a restart does
  not drive outputs to the zeros of a fresh image. In STOP and FAULT it sends the substitute values
  (tags nothing has written since power-up are left as the device has them), and when the
  process stops it sends them once more before it ends.
- **32-bit values.** `"word_order": "abcd"` (the default) sends and expects the high word first,
  like this PLC; `"cdab"` the low word first, as many meters, drives and I/O modules do. It
  applies to `int32`, `uint32` and `float32` tags.
- **One writer per register.** Two `write` mappings, in one link or in two, may not write the same
  register of one device (host, port, unit, area, address): it would take turns between the
  values.
- Changing one link restarts only that link; the others keep running.
- **Identity.** Once per connection the link asks the device who it is (Read Device
  Identification, function 43/14, basic objects: vendor, product code, revision), only in half the
  time left before its next cycle, so the exchange is never late for it. The PLC's summary shows it as the
  link's `identity` (`{"vendor", "product", "revision"}`), or null for a device that does not
  support it (exception 1) or has not answered.
- **Who writes a tag.** The tag list (`GET /api/v1/plcs/<id>/tags`) gives each tag its `writer`:
  `{"kind": "program", "task", "program"}`, `{"kind": "link", "link", "area", "address", "unit"}`
  (the device register a link reads it from) or `{"kind": "clients"}` (nothing in the PLC writes
  it). A tag a link sends to a device also has `sent_by` (`link`, `area`, `address`), and a link's
  status tag `status_of`.
- **Before linking.** `POST /api/v1/plcs/<id>/probe` `{"host", "port": 502, "unit": 1,
  "timeout_ms": 500}` (MCP `probe_device`, administrators only) looks at a device from this PLC's
  place in the network: its identity and up to 64 entries of every area from address 0 (halving
  down to 1 where the device answers exception 2), each area on its own, within one overall
  deadline. It never writes.

## Names, comments and pragmas

- Names: ASCII letters, digits and single underscores, starting with a letter or `_`. No `__`,
  no `_` at the end, at most 64 characters (`E_SYNTAX`, `E_LIMIT`). Case does not matter:
  `Level`, `LEVEL` and `level` are one name.
- Keywords, standard type names, and the names of standard functions and function blocks
  (including conversion names such as `INT_TO_DINT`, and `TON`, `CTU`, `LT`, `MID`) cannot be
  used as names of anything (variables, POUs, types, enumeration values, structure members):
  `E_RESERVED`. So are IEC words of features that SimplePLC does not support, for example
  `STEP`, `TASK`, `ON`, `ACTION`, `FROM`, `CLASS` (hint: `step_no`).
- A name cannot shadow another: a variable named like a function block type (`valve : Valve`),
  function, type, global constant or enumeration value is `E_DUPLICATE`, and so is an
  enumeration value named like a POU or a type.
- The parameters of the standard blocks (`IN`, `Q`, `PT`, `ET`, `CLK`, `CV`, ...) are local to
  those blocks: use these names freely for variables, types or enumeration values.
- Comments: `(* … *)`, `/* … */` and `// to the end of the line`. Comments do not nest: an
  opener inside a comment is `E_COMMENT`.
- Pragmas `{ … }` are ignored with warning `W_PRAGMA`. Conditional compilation (`{IF …}`,
  `{ELSIF}`, `{ELSE}`, `{END_IF}`, `{define}`, `{undefine}`) is `E_PRAGMA`, because ignoring it
  would compile both branches.

<!-- wrap: body -->
<!-- vars: a : INT; -->
<!-- expect: E_COMMENT -->
```st
(* disabled: (* old note *)
a := 1;
```

## Declarations

| Section | PROGRAM | FUNCTION_BLOCK | FUNCTION | Meaning |
|---|---|---|---|---|
| `VAR` | yes | yes | yes | Keeps its value between cycles (in a FUNCTION: starts afresh on every call). |
| `VAR_TEMP` | yes | yes | yes | Starts from its initial value in every cycle or call. |
| `VAR_INPUT` | no | yes | yes | Parameter in; read-only inside (`E_INPUT_WRITE`). |
| `VAR_OUTPUT` | no | yes | yes | Parameter out. |
| `VAR_IN_OUT` | no | yes | yes | Parameter passed by reference; must be given on every call. |
| `VAR_EXTERNAL` | yes | yes | no | Binds a tag (see above). |
| `VAR CONSTANT` | yes | yes | yes | Compile-time constant; writing it is `E_CONSTANT_WRITE`. |
| `VAR RETAIN`, `NON_RETAIN` | yes | yes | no | A `RETAIN` variable keeps its value through RUN after a fault, a download and a power cycle ([retentive memory](#retentive-memory)); a cold restart resets it. |

- A PROGRAM has no inputs or outputs (`E_NOT_SUPPORTED`): it exchanges data through tags.
- `CONSTANT` is allowed on `VAR` and `VAR_EXTERNAL` only.
- `VAR_GLOBAL CONSTANT … END_VAR` at the top of a file declares project constants (elementary
  types, STRING, enumerations) that every POU of the PLC can use. Other `VAR_GLOBAL` is
  `E_NOT_SUPPORTED`: programs share data through tags.
- Function block instances are declared in `VAR` only, never in a FUNCTION
  (`E_FB_IN_FUNCTION`) or a STRUCT. Arrays of instances are allowed in `VAR`.
- Several names can share a declaration: `a, b, c : INT;`.
- A variable, constant or tag that no statement names warns `W_UNUSED`. Parameters of functions
  and blocks may go unused without a warning.

**Initial values** are constant expressions. Without one, a variable starts at 0, 0.0, FALSE,
`''`, `T#0s`, the first value of its enumeration, the lower limit of its subrange, or the
default of its alias type.

<!-- run: cycles=1 -->
<!-- expect: W_UNUSED -->
<!-- assert: 1 grid[2,1]=4 table[3]=5 table[6]=0 p.x=0.0 p.y=1.5 t.PT=T#5s -->
```st
TYPE
    Point : STRUCT
        x : REAL;
        y : REAL;
    END_STRUCT;
END_TYPE

PROGRAM main
VAR CONSTANT
    N : INT := 3;
END_VAR
VAR
    count : DINT := -1;
    ratio : REAL := 0.5;
    name : STRING[20] := 'pump 1';
    table : ARRAY[1..8] OF INT := [2(1), 3(5)];    (* 1, 1, 5, 5, 5, then 0 *)
    grid : ARRAY[1..2, 1..N] OF INT := [1, 2, 3, 4, 5, 6];  (* row by row *)
    p : Point := (y := 1.5);                       (* members not named: default *)
    t : TON := (PT := T#5s);                       (* inputs of an instance *)
END_VAR
END_PROGRAM
```

(Nothing uses these variables, so it compiles with `W_UNUSED`.) More initial values than elements is `E_INDEX`; a string longer than its variable is
`E_LITERAL_RANGE`. A multi-dimensional array is filled row by row (the last index varies
fastest).

## Data types

| Type | Size | Range | Notes |
|---|---|---|---|
| `BOOL` | 1 byte | FALSE, TRUE | |
| `SINT`, `INT`, `DINT`, `LINT` | 1, 2, 4, 8 bytes | −128…127, −32768…32767, −2³¹…2³¹−1, −2⁶³…2⁶³−1 | two's complement |
| `USINT`, `UINT`, `UDINT`, `ULINT` | 1, 2, 4, 8 bytes | 0…255, 0…65535, 0…2³²−1, 0…2⁶⁴−1 | |
| `BYTE`, `WORD`, `DWORD`, `LWORD` | 1, 2, 4, 8 bytes | 8, 16, 32, 64 bits | bit strings: logic, no arithmetic |
| `REAL`, `LREAL` | 4, 8 bytes | IEEE 754 binary32, binary64 | computed in their own width |
| `TIME`, `LTIME` | 8 bytes | signed 64-bit count of nanoseconds | |
| `STRING[n]` | n + 2 bytes | n bytes, n = 1…65535 | `STRING` alone is `STRING[254]` |

Not supported (`E_NOT_SUPPORTED`): `WSTRING`, `CHAR`, `WCHAR`, date and time-of-day types,
`REF_TO`, `ANY…`, `ARRAY[*]`, `AT`.

**Strings** hold bytes (UTF-8 text as its bytes; `LEN` counts bytes). The length goes in
square brackets: `STRING[80]` (`STRING(80)` is `E_SYNTAX`). Storing a longer string is
`F_STRING_LENGTH` at run time (`E_LITERAL_RANGE` for a literal); where a value can be longer
than its target, the compiler warns (`W_STRING_CAPACITY`). Strings are joined with `CONCAT`,
not `+`.

**User types** are declared in `TYPE … END_TYPE` (one or more per block):

<!-- wrap: lib -->
```st
TYPE
    Valve : (Closed, Opening, Open, Closing);          (* enumeration *)
    Alarm : UINT (NoAlarm := 0, High := 10, Low := 20); (* values and base type *)
    Percent : INT (0..100);                             (* subrange *)
    Speed : INT := 1500;                                (* alias with its own default *)
    Label : STRING[16];                                 (* alias *)
    Row : ARRAY[1..4] OF REAL;                          (* array type *)
    Sample : STRUCT
        value : REAL;
        valid : BOOL := TRUE;
        tags : ARRAY[0..1] OF UINT;
    END_STRUCT;
END_TYPE
```

- **Arrays**: bounds are constant integers (literals or constants), up to 8 dimensions and
  16,777,216 elements. `a[i]`, `m[i, j]`. A constant index outside the bounds is `E_INDEX`; a
  computed one is `F_INDEX` at run time. Arrays of the same element type and bounds can be
  assigned as a whole.
- **Structures**: `s.value`; assigned as a whole when the types are the same. A structure
  cannot contain a function block instance.
- **Enumerations**: a bare value (`Open`) or qualified (`Valve#Open`). A bare value that belongs
  to two enumerations is `E_AMBIGUOUS` where no type decides it; qualify it. Enumerations are
  compared with `=` and `<>` only, and used as CASE selectors. There is no conversion between
  an enumeration and an integer (`E_TYPE`): use a CASE to turn a state into a number for a tag
  (see the [cookbook](#cookbook)).
- **Subranges**: storing a value outside the range is `F_SUBRANGE` (a constant: `E_LITERAL_RANGE`).
  A subrange cannot be a FOR control variable or a tag binding.
- **Bit access** on BYTE, WORD, DWORD and LWORD: `w.%X3` (IEC form) or `w.3`, read and write,
  bits 0 (least significant) to width−1. A bit number past the width is `E_INDEX`.

<!-- wrap: body -->
<!-- vars: w : WORD; flag : BOOL; -->
<!-- run: cycles=1 -->
<!-- assert: 1 w=WORD#9 flag=TRUE -->
```st
w.%X0 := TRUE;
w.3 := TRUE;              (* w = 2#1001 *)
flag := w.%X3 AND NOT w.1;
```

A program's variables and stack take at most 16 MB (`E_LIMIT`).

## Literals

| Kind | Examples | Notes |
|---|---|---|
| Integer | `42`, `-7`, `1_000_000`, `2#1010_1010`, `8#777`, `16#FF` | `_` only between digits; hex digits in either case |
| Real | `1.5`, `-0.25`, `1.0E6`, `2.5e-3` | digits on both sides of the point: `5E3`, `1.` and `.5` are `E_LITERAL` |
| Typed | `INT#-5`, `UINT#16#FF`, `WORD#16#00F0`, `REAL#2`, `LREAL#0.1` | out of range: `E_LITERAL_RANGE` |
| Boolean | `TRUE`, `FALSE`, `BOOL#1`, `BOOL#FALSE` | a bare `1` is not a BOOL |
| Duration | `T#1h30m`, `T#250ms`, `T#1.5s`, `TIME#-2s`, `LT#3us_500ns`, `t#1d2h` | see below |
| String | `'text'`, `'It$'s'`, `'$N'`, `STRING#'x'` | see below |
| Enumeration | `Open`, `Valve#Open` | |

**Durations**: prefix `T#`, `TIME#`, `LT#` or `LTIME#`; units `d`, `h`, `m`, `s`, `ms`, `us`,
`ns` (any case), largest first, each once; `_` may separate parts; a sign only right after
`#` (`T#-1h5m`); a fraction only in the last part (`T#1.5h`, not `T#1.5h30m`). The first unit may exceed its range (`T#90s`,
`T#25h15m`); later units may not (`T#1h75m` is `E_LITERAL_RANGE`). Fractions round to the
nearest nanosecond (ties to even). Literals cover the whole range, up to
`T#106751d23h47m16s854ms775us807ns`.

**Strings** use single quotes and stay on one line. Escapes, in either case: `$$` (dollar),
`$'` (quote), `$L` or `$N` (line feed), `$R` (carriage return), `$T` (tab), `$P` (form
feed), `$hh` (the byte with hex value hh). Any other `$x` is `E_LITERAL`. Double-quoted
(WSTRING) literals and `D#`, `TOD#`, `DT#` literals are `E_NOT_SUPPORTED`.

## Expressions

**Precedence**, highest first. Operators of equal precedence are applied left to right.

| Level | Operators |
|---|---|
| 1 | `( … )`, function call, `[ ]` index, `.` member |
| 2 | unary `-`, unary `+`, `NOT` |
| 3 | `**` |
| 4 | `*`, `/`, `MOD` |
| 5 | `+`, `-` |
| 6 | `<`, `>`, `<=`, `>=` |
| 7 | `=`, `<>` |
| 8 | `AND`, `&` |
| 9 | `XOR` |
| 10 | `OR` |

**Refused forms.** Where readers or vendors disagree, parentheses are required:

| Written | Code | Why | Write instead |
|---|---|---|---|
| `-a ** b` | `E_AMBIGUOUS` | `(-a) ** b` or `-(a ** b)` | either, with parentheses |
| `a ** b ** c` | `E_AMBIGUOUS` | grouping of `**` differs | `(a ** b) ** c` |
| `NOT a = b` | `E_AMBIGUOUS` | means `(NOT a) = b`, often misread | `NOT (a = b)` or `(NOT a) = b` |
| `a < b < c`, `a = b < c`, `p = q = r` | `E_CHAINED_COMPARISON` | compares a BOOL result | `a < b AND b < c` |

`a ** -b` and `a = NOT b` have one reading and are accepted.

<!-- wrap: body -->
<!-- vars: p, q, r : BOOL; -->
<!-- expect: E_CHAINED_COMPARISON -->
```st
p := p = q = r;
```

**Evaluation order**: operands are evaluated left to right, and both operands of `AND` and `OR`
are always evaluated (no short-circuit). Guard an index with a nested IF:

<!-- wrap: body -->
<!-- vars: values : ARRAY[1..3] OF INT; i : INT := 4; found : BOOL; -->
<!-- run: cycles=1 -->
<!-- assert: 1 found=FALSE -->
```st
(* IF i <= 3 AND values[i] > 0 THEN … would read values[4]: F_INDEX *)
IF i >= 1 AND i <= 3 THEN
    IF values[i] > 0 THEN
        found := TRUE;
    END_IF;
END_IF;
```

### Types in expressions

The two operands of an operator must have the same type, after an **implicit widening** that
cannot lose information:

| From | Widens to |
|---|---|
| `SINT` | `INT`, `DINT`, `LINT`, `REAL`, `LREAL` |
| `INT` | `DINT`, `LINT`, `REAL`, `LREAL` |
| `DINT` | `LINT`, `LREAL` |
| `USINT` | `UINT`, `UDINT`, `ULINT`, `INT`, `DINT`, `LINT`, `REAL`, `LREAL` |
| `UINT` | `UDINT`, `ULINT`, `DINT`, `LINT`, `REAL`, `LREAL` |
| `UDINT` | `ULINT`, `LINT`, `LREAL` |
| `BYTE` → `WORD` → `DWORD` → `LWORD` | each wider bit string |
| `REAL` | `LREAL` |
| `TIME` | `LTIME` |

The same rule applies to assignments and arguments. Everything else needs an explicit
conversion: storing into a narrower type of the same family is `E_NARROWING` (`i := d` with
DINT `d`), anything else `E_TYPE` (`INT` ↔ `UINT`, `UINT` ↔ `WORD`, `DINT` → `REAL`, `BOOL` ↔
numbers). The hint names the conversion function.

**An operation keeps the type of its operands.** `INT * INT` is computed in INT, even when the
result is stored in a DINT, and overflows there. The compiler warns:

<!-- wrap: body -->
<!-- vars: a : INT := 300; b : INT := 200; product : DINT; -->
<!-- run: cycles=1 -->
<!-- expect: W_NARROW_ARITH, F_OVERFLOW -->
```st
product := a * b;                (* INT: 60000 does not fit *)
```

Widen an operand first:

<!-- wrap: body -->
<!-- vars: a : INT := 300; b : INT := 200; product : DINT; -->
<!-- run: cycles=1 -->
<!-- assert: 1 product=60000 -->
```st
product := INT_TO_DINT(a) * b;   (* computed in DINT *)
```

**Literals take the type of their context**: the other operand, the assignment target or the
parameter. `n + 1` with an INT `n` adds an INT 1. Rules:

- A literal must fit that type: `i := 40000` for an INT is `E_LITERAL_RANGE`; `-1` for a UINT too.
- An integer literal used as REAL or LREAL must be exactly representable: `16777218` is fine
  as a REAL, `16777217` is `E_LITERAL_RANGE` (write `16777217.0` or use an LREAL).
- A real literal never becomes an integer: `i > 3.5` for an INT `i` is `E_TYPE`; convert `i`.
- A number never becomes a BOOL or a TIME: `t + 100` is `E_TYPE`; write `t + T#100ms`.
- Constant expressions are computed with the run-time rules: `7 / 2` is 3, and `r := 1 / 2`
  gives 0.0 with warning `W_INT_DIVISION` (write `1.0 / 2.0`). A constant that overflows is
  `E_OVERFLOW`; dividing by a constant 0 (`x / 0`, `x MOD 0`, `r / 0.0`) is `E_DIV_ZERO`; a
  conversion of a constant that cannot be done (`DINT_TO_INT(70000)`) is `E_OVERFLOW`.
- With nothing to decide, an integer literal is DINT (LINT or ULINT if it needs more), a real
  literal LREAL: `2147483647 + 1 > 0` is `E_OVERFLOW`, and `0.1 + 0.2 = 0.3` is FALSE.
- Context reaches literals through `**`, the REAL functions (`x := SQRT(2.0) * x` computes in
  REAL), `ABS` (`x := ABS(-2.5)` is REAL), bit logic (`w AND 16#00FF`), the generic functions
  (`LIMIT`, `MAX`, `SEL`, `MUX`) and calls with EN/ENO.

### Arithmetic

| Operation | Rule |
|---|---|
| `+ - *` on integers | A result outside the type's range is `F_OVERFLOW` (no wrap-around). |
| `/` on integers | Truncates toward zero: `-7 / 2 = -3`. Division by zero is `F_DIV_ZERO` (by a constant 0: `E_DIV_ZERO`); `-32768 / -1` (INT) is `F_OVERFLOW`. |
| `MOD` | Sign of the dividend: `-7 MOD 2 = -1`, `7 MOD -2 = 1`. `x MOD 0` is 0 (a constant 0 is `E_DIV_ZERO`). `MIN MOD -1` is 0. Integers only. |
| unary `-` | Signed integers, REAL, LREAL, TIME. Not on unsigned types (`E_TYPE`). `-(-32768)` for INT is `F_OVERFLOW`. |
| REAL, LREAL | IEEE 754 in the type's own width. A result that is infinite or NaN is `F_REAL`; division by zero is `F_DIV_ZERO`. |
| `**` | Base REAL or LREAL; the exponent (any number) is converted to the base's type. NaN result (`(-8.0) ** 0.5`) is `F_DOMAIN`; infinite (`0.0 ** -1.0`) is `F_REAL`. |
| Bit strings | `AND`, `OR`, `XOR`, `NOT`, bit access, shifts. Arithmetic on them is `E_TYPE`. |
| BOOL | `AND`, `&`, `OR`, `XOR`, `NOT`. `AND`/`OR` on integers is `E_TYPE`. |

REAL is computed in binary32: `0.1 + 0.2 = 0.3` holds for REAL but not for LREAL.

**TIME arithmetic**: `TIME + TIME`, `TIME - TIME`, `TIME * number`, `number * TIME`,
`TIME / number`, comparisons; the same for LTIME. `T#1ms * speed_sp` turns a number of
milliseconds from a tag into a TIME. `TIME * TIME` and `number / TIME` are
`E_TYPE`. Dividing by an integer truncates toward zero (`T#1s / 3 = T#333ms333us333ns`);
multiplying or dividing by a REAL rounds to the nearest nanosecond. Overflow is
`F_OVERFLOW`, division by zero `F_DIV_ZERO`.

**Comparisons** (`= <> < > <= >=`) work on every elementary type of the same family after
widening. STRING compares byte by byte (`'a' > 'A'`, `'B' > 'AZZZ'`), BOOL as FALSE < TRUE,
enumerations with `=` and `<>` only. Results are BOOL.

## Conversions

Conversions are explicit function calls: `INT_TO_REAL(i)`, the overloaded `TO_REAL(i)` (the
source type is the argument's), `REAL_TRUNC_INT(x)` and `TRUNC_INT(x)`.

| Conversion | Result |
|---|---|
| integer → integer | The same value. Not representable (`INT_TO_UINT(i)` with `i = -1`) is `F_CONVERSION`; for a constant, `E_OVERFLOW`. |
| integer → REAL, LREAL | Nearest representable value. |
| REAL, LREAL → integer | Rounded to nearest, ties to even: 2.5 → 2, 3.5 → 4, −2.5 → −2. NaN, infinite or out of range is `F_CONVERSION`. |
| `…_TRUNC_…`, `TRUNC_…` | Toward zero: −2.7 → −2. Bare `TRUNC(x)` is `E_NOT_SUPPORTED` (deprecated): write `TRUNC_DINT(x)`. |
| LREAL → REAL | Rounded; beyond REAL's range is `F_CONVERSION`. |
| bit string ↔ integer, bit string ↔ bit string | The bit pattern. Narrower keeps the low bits (`UDINT_TO_WORD(70000) = 16#1170`), wider fills with zeros (`WORD_TO_DINT(16#FFFF) = 65535`), same width reinterprets (`WORD_TO_INT(16#FFFF) = -1`). |
| signed integer → wider bit string | `E_AMBIGUOUS` (vendors disagree on sign extension). Write `WORD_TO_DWORD(INT_TO_WORD(i))`. |
| REAL, LREAL ↔ bit string | `E_AMBIGUOUS` (the standard copies bits, others convert the value). For the number, go through UDINT: `UDINT_TO_DWORD(REAL_TO_UDINT(x))`. |
| BOOL → number or bit string | 0 or 1. Number or bit string → BOOL: 0 is FALSE, anything else TRUE. |
| TIME ↔ number | Milliseconds (LTIME: nanoseconds), truncated toward zero: `TIME_TO_DINT(T#1.9ms) = 1`. Warns `W_TIME_UNIT`, because vendors use different units. |
| TIME ↔ LTIME | The same duration. |
| number, BOOL, TIME → STRING | Decimal text: `'-42'`, `'0.1'` (shortest text that reads back as the same REAL), `'2'`, `'1e+20'`, `'TRUE'`, `'T#1m30s'`. |
| STRING → number | The whole string must be a decimal number (an optional sign, no spaces, no `16#`): otherwise `F_CONVERSION`. |
| STRING → TIME, LTIME | The text of a duration literal, with or without its `T#`/`LT#` prefix, read with the literal's rules and exactly: `STRING_TO_TIME('0.5ms') = T#500us`, `STRING_TO_TIME('T#1_000ms') = T#1s`. Text no literal may be (`'T#1h75m'`, `'T#5s1h'`) is `F_CONVERSION`. |

Not available: conversions between enumerations and integers, BCD conversions
(`WORD_BCD_TO_INT`, `E_NOT_SUPPORTED`), and anything to or from WSTRING, CHAR or date types.

## Statements

Every statement ends with `;`. An empty statement `;` is allowed.

**Assignment** `target := expression;` to a variable, element, member or bit. `=` compares,
`:=` assigns (`x = 1;` is `E_SYNTAX`). Arrays and structures of the same type are copied whole.

**IF**: the conditions must be BOOL (`IF n THEN` is `E_TYPE`; write `IF n <> 0`). `ELSEIF` is
`E_SYNTAX`: the keyword is `ELSIF`.

<!-- wrap: body -->
<!-- vars: level : REAL := 55.0; zone : INT; -->
<!-- run: cycles=1 -->
<!-- assert: 1 zone=2 -->
```st
IF level > 90.0 THEN
    zone := 3;
ELSIF level > 50.0 THEN
    zone := 2;
ELSE
    zone := 1;
END_IF;
```

**CASE**: the selector is an integer, a bit string or an enumeration. Labels are constants:
single values, lists and ranges `low..high` (not for enumerations). Each value may appear once
(`E_CASE_OVERLAP`). When no label matches and there is no ELSE, nothing runs. An enumeration
CASE without ELSE that misses values warns `W_CASE_INCOMPLETE`.

<!-- wrap: body -->
<!-- vars: code : UINT := 7; band : INT; -->
<!-- run: cycles=1 -->
<!-- assert: 1 band=2 -->
```st
CASE code OF
    0:          band := 0;
    1, 2, 3:    band := 1;
    4..9:       band := 2;
ELSE
    band := -1;
END_CASE;
```

**FOR** `FOR i := start TO end BY step DO … END_FOR;` (`BY 1` when omitted):

- The control variable is a local `VAR` or `VAR_TEMP` integer without a subrange (`E_FOR_VAR`;
  a REAL one is `E_TYPE`). It cannot be an element, a member, an input or a tag.
- Start, end and step have the control variable's type; `end` and `step` are evaluated once,
  before the first pass. A constant step 0 is `E_FOR_STEP`; a step that is 0 at run time is
  `F_FOR_STEP`. For an unsigned control variable a negative step is impossible: use a signed type.
- The body must not assign the control variable or a variable read by `end` or `step`, or pass
  them as `VAR_IN_OUT` or `=>` targets (`E_FOR_VAR`). Nested loops need different variables.
- The loop runs while `i <= end` (step > 0) or `i >= end` (step < 0). After normal completion
  `i` holds the first value that failed the test (`FOR i := 0 TO 5` leaves 6). If the next value
  would pass the type's limit, the loop ends and `i` keeps the last value
  (`FOR b := 250 TO 255` with USINT `b` runs 6 times and leaves 255). After `EXIT`, `i` keeps
  its value at the EXIT.

<!-- wrap: body -->
<!-- vars: i, sum : INT; -->
<!-- run: cycles=1 -->
<!-- assert: 1 sum=22 i=-2 -->
```st
sum := 0;
FOR i := 10 TO 1 BY -3 DO    (* 10, 7, 4, 1 *)
    sum := sum + i;
END_FOR;
```

**WHILE** `WHILE condition DO … END_WHILE;` tests before each pass. **REPEAT**
`REPEAT … UNTIL condition END_REPEAT;` runs at least once and tests after each pass.

**EXIT** leaves the innermost loop. **CONTINUE** starts the next pass of the innermost loop
(in REPEAT it goes to the UNTIL test). Outside a loop both are `E_EXIT_OUTSIDE_LOOP`.
**RETURN** leaves the function, function block or program at once.

<!-- wrap: body -->
<!-- vars: data : ARRAY[1..5] OF INT := [3, 8, 0, 9, 2]; i, first_zero : INT; -->
<!-- run: cycles=1 -->
<!-- assert: 1 first_zero=3 -->
```st
first_zero := 0;
i := 1;
WHILE i <= 5 DO
    IF data[i] = 0 THEN
        first_zero := i;
        EXIT;
    END_IF;
    i := i + 1;
END_WHILE;
```

A cycle may run at most 50 million instructions (string functions count one per byte, copies
of arrays, structures and strings one more per 8 bytes); an endless loop stops the PLC with
`F_BUDGET`. A cycle that takes longer than the task's watchdog (`watchdog_ms`, default the
larger of 100 ms and 10 × `cycle_ms`; a task's status shows the one in effect as
`watchdog_effective_ms`) is `F_WATCHDOG`. Loops are for arrays and searches, not
for waiting: to wait, return and check again in the next cycle (use TON).

## Functions and function blocks

A **FUNCTION** has no memory: its `VAR` and `VAR_TEMP` start afresh on every call. It returns
a value by assigning to its own name, on every path to its end and before every RETURN
(`E_RESULT_UNASSIGNED`; loops count as possibly running zero times, a CASE over an
enumeration that lists every value covers every path). A function without a result type
(`FUNCTION Swap`) is called as a statement. A user function's result may be ignored, but a
standard, conversion or operator function used as a statement (`CONCAT(s, t);`) is `E_CALL`:
the value it computes would be lost.

A **FUNCTION_BLOCK** is a type with memory: declare an instance in `VAR`, call the instance as
a statement, then read its outputs.

<!-- wrap: lib -->
<!-- name: blocks -->
```st
FUNCTION Clamp : INT
VAR_INPUT
    x : INT;
    lo : INT := 0;          (* used when a formal call omits it *)
    hi : INT := 100;
END_VAR
VAR_OUTPUT
    clipped : BOOL;
END_VAR
    Clamp := LIMIT(lo, x, hi);
    clipped := Clamp <> x;
END_FUNCTION

FUNCTION Swap
VAR_IN_OUT
    a, b : INT;
END_VAR
VAR
    tmp : INT;
END_VAR
    tmp := a;
    a := b;
    b := tmp;
END_FUNCTION

FUNCTION_BLOCK Debounce
VAR_INPUT
    raw : BOOL;
    settle : TIME := T#50ms;
END_VAR
VAR_OUTPUT
    stable : BOOL;
END_VAR
VAR
    on_delay : TON;
    off_delay : TON;
END_VAR
    on_delay(IN := raw, PT := settle);
    off_delay(IN := NOT raw, PT := settle);
    IF on_delay.Q THEN
        stable := TRUE;
    ELSIF off_delay.Q THEN
        stable := FALSE;
    END_IF;
END_FUNCTION_BLOCK
```

<!-- with: blocks -->
<!-- run: cycles=8 dt=T#20ms set=part_sensor=TRUE@2 -->
<!-- assert: 3 c=0 d=1 clipped=TRUE -->
<!-- assert: 4 b=100 debounced=FALSE -->
<!-- assert: 5 debounced=TRUE -->
```st
PROGRAM main
VAR_EXTERNAL CONSTANT
    part_sensor : BOOL;
END_VAR
VAR
    a, b, c, d : INT;
    clipped, debounced : BOOL;
    sensor : Debounce;
END_VAR
    a := Clamp(150, 0, 100);                  (* informal: every input, in order *)
    b := Clamp(x := 150, clipped => clipped); (* formal: lo and hi take 0 and 100 *)
    c := 1;
    d := 0;
    Swap(c, d);                               (* in-outs are passed by reference *)

    sensor(raw := part_sensor);               (* settle keeps T#50ms *)
    debounced := sensor.stable;
END_PROGRAM
```

**Calls.**

- A function call is either **informal** (positional: every `VAR_INPUT` in declaration order,
  then every `VAR_IN_OUT`) or **formal** (`name := value` for inputs and in-outs,
  `name => variable` for outputs). Mixing the two is `E_CALL`.
- In a formal function call, an omitted input takes its initial value. In a function block
  call (always formal), an omitted input keeps the value it had.
- A `VAR_IN_OUT` must be given on every call, as a variable of exactly the same type (same
  subrange, string length), else `E_TYPE`; a constant is `E_CONSTANT_WRITE`. An output target
  (`=>`) must hold the output's type (widening allowed).
- A function block call is a statement: `x := t(IN := go).Q` and `TON(…)` (a type, not an
  instance) are `E_CALL`.
- No recursion, direct or indirect (`E_RECURSION`), and no block that contains an instance of
  itself.

**Using an instance from outside** (IEC 61131-3 Figure 13; `x := t.PT` is the common mistake):

| Access | Allowed? |
|---|---|
| Read an output: `t.Q`, `t.ET` | yes |
| Assign an input before the call: `t.IN := go;` | yes |
| Read an input: `x := t.PT` | no, `E_ACCESS` (keep the value in a variable of your own) |
| Write an output: `t.Q := TRUE` | no, `E_OUTPUT_WRITE` |
| Read or write an in-out or an internal variable | no, `E_ACCESS` |
| Assign one instance to another | no, `E_TYPE` |

### EN and ENO

Any call of a user function, function block, standard function or conversion can take the
formal inputs `EN := condition` and `ENO => bool_variable`:

- `EN = FALSE`: nothing runs, no inputs are assigned, the result is its type's default (0,
  FALSE, `''`, a subrange's lower limit, an alias's own default), `=>` targets are not
  written, and `ENO` is FALSE.
- With `ENO` connected, a run-time fault inside the call (at any depth) does not stop the PLC.
  The called block's instance, its in-out arguments and the tags it touched are restored to
  their state before the call, the result is the default, `=>` targets are not written, `ENO`
  is FALSE, and the program continues.
- `F_BUDGET`, `F_WATCHDOG`, `F_BINDING` and internal faults are never caught.
- Without `ENO`, a fault stops the PLC as usual.

<!-- wrap: body -->
<!-- vars: big : DINT := 70000; small : INT := 5; ok : BOOL; root : REAL; x : REAL := -1.0; ok2 : BOOL; -->
<!-- run: cycles=1 -->
<!-- assert: 1 small=0 ok=FALSE root=0.0 ok2=FALSE -->
```st
small := DINT_TO_INT(IN := big, ENO => ok);   (* 70000 does not fit: small = 0, ok = FALSE *)
root := SQRT(IN := x, ENO => ok2);            (* SQRT(-1.0): root = 0.0, ok2 = FALSE *)
```

## Standard functions

Inputs of the generic functions must have a common type after widening; the result has that
type. "Extensible" functions take two or more inputs (`IN1`, `IN2`, `IN3`, … in formal calls).

**Numeric** (`IN` REAL or LREAL unless noted; the result has the input's type):

| Function | Result | Faults |
|---|---|---|
| `ABS(IN)` | absolute value; any number | `F_OVERFLOW` for the most negative integer |
| `SQRT(IN)` | square root | `F_DOMAIN` below 0 |
| `LN(IN)`, `LOG(IN)` | natural, base-10 logarithm | `F_DOMAIN` at or below 0 |
| `EXP(IN)` | e to the power IN | `F_REAL` on overflow |
| `SIN`, `COS`, `TAN` | radians | |
| `ASIN`, `ACOS` | radians | `F_DOMAIN` outside −1…1 |
| `ATAN(IN)`, `ATAN2(Y, X)` | radians; ATAN2 gives the angle of (X, Y) in −π…π | |
| `EXPT(IN1, IN2)` | `IN1 ** IN2`, as the operator | `F_DOMAIN`, `F_REAL` |
| `ADD`, `MUL` (extensible), `SUB`, `DIV`, `MOD`, `MOVE` | the operators `+ * - / MOD` and assignment | as the operators |

**Bit strings** (`IN` BYTE, WORD, DWORD or LWORD; `N` any integer):

| Function | Result |
|---|---|
| `SHL(IN, N)`, `SHR(IN, N)` | shift left, right, filling with zeros; N ≥ width gives 0 (any N, even a ULINT) |
| `ROL(IN, N)`, `ROR(IN, N)` | rotate left, right; N is taken modulo the width |
| `AND`, `OR`, `XOR` (extensible), `NOT(IN)` | as the operators; also on BOOL |

A negative N is `F_DOMAIN`. Write typed literals for constants: `SHL(WORD#16#00FF, 4)`.

**Selection**:

| Function | Result |
|---|---|
| `SEL(G, IN0, IN1)` | IN0 when G is FALSE, IN1 when TRUE |
| `MAX(IN1, IN2, …)`, `MIN(…)` | largest, smallest; numbers, bit strings, TIME, or strings (byte order) |
| `LIMIT(MN, IN, MX)` | IN clamped to MN…MX (as `MIN(MAX(IN, MN), MX)`, so MX wins when MN > MX) |
| `MUX(K, IN0, IN1, …)` | input number K (0-based); K outside 0…n−1 is `F_INDEX` (`E_INDEX` when constant) |

**Comparison** as functions: `GT`, `GE`, `EQ`, `LE`, `LT` are extensible and mean a chain:
`GT(a, b, c)` is `a > b AND b > c`. `NE(IN1, IN2)` takes exactly two inputs.

**Strings.** Positions count bytes from 1. Every position a function refers to must exist,
otherwise the call is `F_STRING_POSITION` (no clipping). `LEN` and `FIND` return INT, or DINT
when a string argument can hold more than 32,767 bytes.

| Function | Result | Valid arguments |
|---|---|---|
| `LEN(IN)` | length in bytes | |
| `LEFT(IN, L)`, `RIGHT(IN, L)` | first, last L bytes | 0 ≤ L ≤ LEN(IN) |
| `MID(IN, L, P)` | L bytes starting at P | P ≥ 1, L ≥ 0, P + L − 1 ≤ LEN(IN) |
| `CONCAT(IN1, IN2, …)` | joined (extensible) | |
| `INSERT(IN1, IN2, P)` | IN2 inserted after the P-th byte of IN1 (P = 0: in front) | 0 ≤ P ≤ LEN(IN1) |
| `DELETE(IN, L, P)` | IN without the L bytes starting at P | P ≥ 1, L ≥ 0, P + L − 1 ≤ LEN(IN) |
| `REPLACE(IN1, IN2, L, P)` | L bytes of IN1 from P replaced by all of IN2 | P ≥ 1, L ≥ 0, P + L − 1 ≤ LEN(IN1) |
| `FIND(IN1, IN2)` | position of the first IN2 in IN1, 0 if absent or IN2 is empty | |

A result longer than its target is `F_STRING_LENGTH`.

<!-- wrap: body -->
<!-- vars: s : STRING[20] := 'ABCDE'; a, b, c, d, e : STRING; pos : INT; -->
<!-- run: cycles=1 -->
<!-- assert: 1 a='BC' b='ABXYCDE' c='AE' d='ABXE' e='DE' pos=3 -->
```st
a := MID(s, 2, 2);              (* 'BC' *)
b := INSERT(s, 'XY', 2);        (* 'ABXYCDE' *)
c := DELETE(s, 3, 2);           (* 'AE' *)
d := REPLACE(s, 'X', 2, 3);     (* 'ABXE': 2 bytes from position 3 *)
e := RIGHT(s, 2);               (* 'DE' *)
pos := FIND(s, 'CD');           (* 3 *)
```

**Time**: `ADD_TIME`, `SUB_TIME`, `MUL_TIME`, `DIV_TIME` (and `…_LTIME`) are the operators
`+ - * /` on durations.

**PLC**: `FIRST_CYCLE()` is TRUE in the first cycle after power-up, a restart or RUN after a
fault (see [the cycle](#the-cycle)), FALSE otherwise (also after STOP and RUN, and
after an online change). A program that replaces one that has not run a cycle yet (the first
program saved on an empty PLC), or that takes over no variable of the one it replaces, starts:
its first cycle has it TRUE. It is the same in every program, function
and block of that cycle.

## Standard function blocks

All standard blocks are called with formal arguments. Timers use the cycle's time.

| Block | Inputs | Outputs |
|---|---|---|
| `TON`, `TOF`, `TP` | `IN : BOOL`, `PT : TIME` | `Q : BOOL`, `ET : TIME` |
| `R_TRIG`, `F_TRIG` | `CLK : BOOL` | `Q : BOOL` |
| `SR` | `S1`, `R : BOOL` | `Q1 : BOOL` |
| `RS` | `S`, `R1 : BOOL` | `Q1 : BOOL` |
| `CTU` | `CU`, `R : BOOL`, `PV : INT` | `Q : BOOL`, `CV : INT` |
| `CTD` | `CD`, `LD : BOOL`, `PV : INT` | `Q : BOOL`, `CV : INT` |
| `CTUD` | `CU`, `CD`, `R`, `LD : BOOL`, `PV : INT` | `QU`, `QD : BOOL`, `CV : INT` |

The counters also exist for wider counts: `CTU_DINT`, `CTU_LINT`, `CTU_UDINT`, `CTU_ULINT`, and
the same suffixes for `CTD` and `CTUD` (PV and CV of that type).

- **TON** (on-delay): Q goes TRUE when IN has been TRUE for PT; ET counts from 0 up to PT. IN
  FALSE resets Q and ET at once. With PT = `T#0s`, Q is TRUE in the same call as IN; a negative
  PT acts as 0. PT is read on every call.
- **TOF** (off-delay): Q is TRUE while IN is TRUE and stays TRUE for PT after IN goes FALSE. ET
  counts while Q is held and stays at PT afterwards, until IN is TRUE again.
- **TP** (pulse): a rising edge of IN while no pulse is running starts a pulse of length PT;
  edges during the pulse are ignored. ET counts up to PT and stays there while IN is TRUE; it
  returns to 0 when the pulse is over and IN is FALSE. PT = `T#0s` or negative gives no pulse
  (it resets the output, as in Fernhill and CODESYS).
- **R_TRIG**: Q is TRUE for one call when CLK goes from FALSE to TRUE. On the first call, CLK
  TRUE counts as a rising edge.
- **F_TRIG**: Q is TRUE for one call when CLK goes from TRUE to FALSE. As the standard defines
  it, the first call with CLK FALSE also gives a pulse; ignore Q in the first cycle if that
  matters (`IF f.Q AND NOT FIRST_CYCLE() THEN`).
- **SR**: set-dominant latch, `Q1 := S1 OR (NOT R AND Q1)`. **RS**: reset-dominant latch,
  `Q1 := NOT R1 AND (S OR Q1)`.
- **CTU**: each rising edge of CU adds 1; R sets CV to 0 (R wins). Q is `CV >= PV`. CV stops at
  the type's maximum (INT: 32767); it does not stop at PV.
- **CTD**: each rising edge of CD subtracts 1; LD loads PV. Q is `CV <= 0`. CV goes below 0 and
  stops at the type's minimum (unsigned variants stop at 0 and Q is `CV = 0`).
- **CTUD**: R (first) sets 0, then LD loads PV; rising edges of CU and CD count up and down,
  both in the same call cancel out. QU is `CV >= PV`, QD is `CV <= 0`.
- The counters count an edge on the first call when CU or CD is already TRUE.

<!-- run: cycles=8 dt=T#100ms set=start_button=TRUE@2 set=start_button=FALSE@6 -->
<!-- assert: 4 lamp=FALSE -->
<!-- assert: 5 lamp=TRUE -->
<!-- assert: 6 lamp=FALSE alarm=TRUE -->
<!-- assert: 8 alarm=FALSE -->
```st
PROGRAM main
VAR_EXTERNAL CONSTANT
    start_button : BOOL;
END_VAR
VAR_EXTERNAL
    lamp : BOOL;
    alarm : BOOL;
END_VAR
VAR
    on_delay : TON;
    hold : TOF;
END_VAR
    on_delay(IN := start_button, PT := T#300ms);   (* TRUE from cycle 5: 300 ms after cycle 2 *)
    lamp := on_delay.Q;
    hold(IN := start_button, PT := T#200ms);       (* stays TRUE 200 ms after release *)
    alarm := hold.Q AND NOT start_button;
END_PROGRAM
```

## Extensions to IEC 61131-3

Tags bound by `VAR_EXTERNAL`, file-level `VAR_GLOBAL CONSTANT`, library files,
`FIRST_CYCLE()`, bit access `w.3`, the wider counters, and EN/ENO that undo a failed call.
The compliance statement lists them, and where SimplePLC is stricter than the standard.

## Run-time faults

A fault stops the PLC (see [the cycle](#the-cycle)) unless it happens inside a call
with ENO connected. The panel, the API (`GET /api/v1/plcs/{id}`, field `fault`) and the log
show the code, message, file, line, cycle number and the call chain. Fix the program, save it,
then RUN (a warm restart: only `RETAIN` variables keep their values).

| Code | Cause |
|---|---|
| `F_OVERFLOW` | Integer or TIME result outside its type: `+ - *`, unary minus, `ABS`, `MIN / -1`. |
| `F_DIV_ZERO` | Division by zero: integer, REAL or TIME. |
| `F_REAL` | A REAL or LREAL operation gave infinity or NaN: `EXP(1000.0)`, `x * x` for a REAL `x` of 1.0E30. |
| `F_DOMAIN` | Argument outside a function's domain: `SQRT(-1.0)`, `LN(0.0)`, `ASIN(2.0)`, `**` with a NaN result, negative shift count. |
| `F_INDEX` | Array index outside its bounds; MUX selector out of range. (A ULINT index of 2⁶³ or more gives `F_CONVERSION` in this version.) |
| `F_SUBRANGE` | Value stored into a subrange variable is outside its range. |
| `F_STRING_LENGTH` | A string is longer than the variable it is stored in. |
| `F_STRING_POSITION` | A string position or length outside the string (LEFT, RIGHT, MID, INSERT, DELETE, REPLACE). |
| `F_CONVERSION` | A conversion whose result does not fit: `INT_TO_UINT(i)` with `i = -1`, `REAL_TO_INT(x)` with `x = 1.0E9`, `STRING_TO_INT('4x')`. |
| `F_FOR_STEP` | A FOR step that is 0 at run time. |
| `F_BUDGET` | The cycle ran 50 million instructions (an endless loop). String functions count one per byte, copies one more per 8 bytes. Never caught by ENO. |
| `F_WATCHDOG` | The cycle took longer than the task's `watchdog_ms`. Never caught by ENO. |
| `F_BINDING` | A bound tag was removed or changed type. Never caught by ENO. |
| `F_INTERNAL` | A VM invariant was broken. It should never happen: please report it. |

<!-- wrap: body -->
<!-- vars: n : INT := 32767; -->
<!-- run: cycles=1 -->
<!-- expect: F_OVERFLOW -->
```st
n := n + 1;     (* F_OVERFLOW: INT has no 32768 *)
```

## Diagnostics

Every problem has a stable **code**, a position, a message and usually a **hint** on how to
write it. `simpleplc check` prints one per line:

```
main.st:7:8: error E_AMBIGUOUS: '-a ** b' can mean (-a) ** b or -(a ** b); implementations differ (hint: add parentheses)
```

Lines and columns count from 1; columns count characters (Unicode code points), as an editor
does. A program with errors is not loaded;
warnings do not stop it. The MCP tools and the API return the same fields as JSON (`code`,
`severity`, `file`, `line`, `column`, `message`, `hint`). Fix the first error first.

| Code | Meaning |
|---|---|
| `E_SYNTAX` | Not valid ST: a missing `;`, `ELSEIF`, `!=`, `STRING(80)`, a name with `__`, no PROGRAM in the file. |
| `E_COMMENT` | Nested or unclosed comment. |
| `E_PRAGMA` | Conditional-compilation pragma. |
| `E_LITERAL` | Malformed literal: `5E3`, `.5`, `1__0`, `3#12`, a bad `$` escape or duration. |
| `E_LITERAL_RANGE` | A literal or constant initial value does not fit its type, subrange or string. |
| `E_LIMIT` | A size limit: name over 64 characters, STRING length, array dimensions or elements, 16 MB of memory, statements and expressions nested more than 100 levels, an expression more than 400 operations deep. |
| `E_RESERVED` | A keyword or standard name used as a name. |
| `E_UNDECLARED` | Unknown variable, type, member, enumeration value or function (the hint suggests the closest name). |
| `E_DUPLICATE` | A name declared twice or shadowing a POU, type, global constant or enumeration value; two PROGRAMs in one file. |
| `E_TYPE` | Types that do not go together, or a construct used with the wrong kind of value. |
| `E_NARROWING` | An implicit conversion that could lose information: `INT := DINT`, `REAL := LREAL`. |
| `E_AMBIGUOUS` | A form with two readings (`-a ** b`, `NOT a = b`), an enumeration value of two types, a conversion vendors disagree on. |
| `E_CHAINED_COMPARISON` | A comparison of a comparison (`a < b < c`). |
| `E_OVERFLOW` | A constant expression overflows its type, or a constant cannot be converted. |
| `E_DIV_ZERO` | Division or `MOD` by a constant 0. |
| `E_INDEX` | Constant array index or MUX selector out of range; too many initial values; bit number too large. |
| `E_CASE_OVERLAP` | A CASE value in two labels. |
| `E_FOR_VAR`, `E_FOR_STEP` | FOR control variable or step not allowed. |
| `E_EXIT_OUTSIDE_LOOP` | EXIT or CONTINUE outside a loop. |
| `E_RECURSION` | A function or block calls or contains itself. |
| `E_CONSTANT_WRITE`, `E_INPUT_WRITE`, `E_OUTPUT_WRITE` | Writing a constant, an input inside its block, an instance's output from outside. |
| `E_ACCESS` | Reading an instance's input, in-out or internal variable from outside. |
| `E_BINDING` | VAR_EXTERNAL without a matching tag, or with the wrong type; a write to a tag that another task or a link writes. |
| `E_FB_IN_FUNCTION` | A function block instance inside a FUNCTION. |
| `E_CALL` | Wrong, missing or duplicate arguments; informal FB call; FB call used as a value; a standard function's value not used. |
| `E_RESULT_UNASSIGNED` | A function can end or RETURN without assigning its result. |
| `E_NOT_SUPPORTED` | An IEC feature SimplePLC does not implement (see the compliance statement). |
| `E_INTERNAL` | The compiler produced an invalid program: please report it. |

| Warning | Meaning |
|---|---|
| `W_NARROW_ARITH` | Integer arithmetic computed in a narrower type than where its result goes (`DINT := INT * INT`). |
| `W_INT_DIVISION` | Integer division of literals in a REAL context drops the fraction; the message shows the result. |
| `W_TIME_UNIT` | A TIME ↔ number conversion: the unit is milliseconds (LTIME: nanoseconds). |
| `W_STRING_CAPACITY` | A string value may be longer than its target (`F_STRING_LENGTH` if it is). |
| `W_CASE_INCOMPLETE` | An enumeration CASE without ELSE misses values. |
| `W_UNUSED` | A variable, constant or tag that no statement names. |
| `W_PRAGMA` | A pragma was ignored. |
| `W_LIBRARY` | A library file the program uses has syntax errors and was left out (on the PLC, not in the CLI). |

`simpleplc check --listing` (MCP `get_compiled`) prints the program as compiled, as valid ST
(types and global constants included): every expression fully parenthesised, literals typed
(`INT#1`), initial values and implicit conversions written out (`INT_TO_DINT(…)`). It compiles
to exactly the same program, so it is what runs. Use it to confirm how a line was read.

## Cookbook

Complete programs, using the tag table above.

**Count rising edges, with a reset, without overflow.** CTU_UDINT saturates instead of
overflowing:

<!-- run: cycles=6 set=part_sensor=TRUE@1 set=part_sensor=FALSE@2 set=part_sensor=TRUE@3 set=reset_count=TRUE@5 -->
<!-- assert: 4 part_count=2 -->
<!-- assert: 5 part_count=0 -->
```st
PROGRAM main
VAR_EXTERNAL CONSTANT
    part_sensor : BOOL;
    reset_count : BOOL;
END_VAR
VAR_EXTERNAL
    part_count : UDINT;
END_VAR
VAR
    counter : CTU_UDINT;
END_VAR
    counter(CU := part_sensor, R := reset_count, PV := 1000);
    part_count := counter.CV;
END_PROGRAM
```

To wrap around on purpose, test before adding: `IF n = 65535 THEN n := 0; ELSE n := n + 1; END_IF;`.

**A timer that restarts itself, without losing a cycle.** Calling a TON with `IN := FALSE` and
then `IN := TRUE` in the same cycle restarts it, so every period is exactly PT:

<!-- run: cycles=12 dt=T#100ms set=start_button=TRUE@1 -->
<!-- assert: 5 lamp=FALSE -->
<!-- assert: 6 lamp=TRUE -->
<!-- assert: 10 lamp=TRUE -->
<!-- assert: 11 lamp=FALSE -->
```st
PROGRAM main
VAR_EXTERNAL CONSTANT
    start_button : BOOL;    (* enable *)
END_VAR
VAR_EXTERNAL
    lamp : BOOL;
END_VAR
VAR
    period : TON;
END_VAR
    period(IN := start_button, PT := T#500ms);
    IF NOT start_button THEN
        lamp := FALSE;
    ELSIF period.Q THEN
        lamp := NOT lamp;
        period(IN := FALSE);    (* restart in this cycle *)
        period(IN := TRUE);
    END_IF;
END_PROGRAM
```

**A state machine with an enumeration**, its state shown on a tag as a number:

<!-- run: cycles=9 dt=T#100ms set=start_button=TRUE@2 set=start_button=FALSE@3 -->
<!-- assert: 2 state_no=1 motor=TRUE -->
<!-- assert: 7 state_no=1 -->
<!-- assert: 8 state_no=2 motor=FALSE -->
<!-- assert: 9 state_no=0 -->
```st
TYPE
    Phase : (Idle, Filling, Done);
END_TYPE

PROGRAM main
VAR_EXTERNAL CONSTANT
    start_button : BOOL;
    stop_button : BOOL;
END_VAR
VAR_EXTERNAL
    motor : BOOL;
    state_no : UINT;
END_VAR
VAR
    state : Phase;          (* starts as Idle, the first value *)
    fill_time : TON;
END_VAR
    CASE state OF
        Idle:
            IF start_button THEN
                state := Filling;
            END_IF;
        Filling:
            fill_time(IN := TRUE, PT := T#500ms);
            IF fill_time.Q OR stop_button THEN
                fill_time(IN := FALSE);
                state := Done;
            END_IF;
        Done:
            IF NOT start_button THEN
                state := Idle;
            END_IF;
    END_CASE;

    motor := state = Filling;
    CASE state OF
        Idle: state_no := 0;
        Filling: state_no := 1;
        Done: state_no := 2;
    END_CASE;
END_PROGRAM
```

The `Done` state goes back to `Idle` once the button is released, so the next press starts a
new fill. Change state by assigning `state`; do the work for a state in its branch.

**Scale an analog value.** A 16-bit raw value 0…65535 to 0…100 %, and back to an integer for a
display, with the setpoint clamped:

<!-- run: cycles=1 set=level_raw=32768 set=speed_sp=2000 -->
<!-- assert: 1 level=50.000763 state_no=50 -->
```st
PROGRAM main
VAR_EXTERNAL CONSTANT
    level_raw : UINT;
    speed_sp : UINT;
END_VAR
VAR_EXTERNAL
    level : REAL;
    state_no : UINT;
END_VAR
VAR CONSTANT
    RAW_MAX : REAL := 65535.0;
    SPEED_MAX : UINT := 1500;
END_VAR
VAR
    speed : UINT;
END_VAR
    level := UINT_TO_REAL(level_raw) * 100.0 / RAW_MAX;
    state_no := REAL_TO_UINT(level);          (* rounded to nearest, ties to even *)
    speed := MIN(speed_sp, SPEED_MAX);        (* never above the motor's limit *)
END_PROGRAM
```

**Use a link's status tag.** Values from a link keep their last value when the device stops
answering. Decide nothing on them while the link is down, and raise an alarm when it has been
down for 2 s:

<!-- run: cycles=40 dt=T#100ms set=remote_ok=TRUE@1 set=remote_level=95.0@1 set=remote_ok=FALSE@10 -->
<!-- assert: 5 motor=FALSE alarm=FALSE -->
<!-- assert: 29 alarm=FALSE -->
<!-- assert: 30 alarm=TRUE -->
```st
PROGRAM main
VAR_EXTERNAL CONSTANT
    remote_ok : BOOL;       (* the link's status_tag *)
    remote_level : REAL;    (* read by the link *)
END_VAR
VAR_EXTERNAL
    motor : BOOL;
    alarm : BOOL;
END_VAR
VAR
    link_down : TON;
END_VAR
    link_down(IN := NOT remote_ok, PT := T#2s);
    alarm := link_down.Q;
    IF remote_ok THEN
        motor := remote_level < 80.0;
    ELSE
        motor := FALSE;     (* the level is stale: stop *)
    END_IF;
END_PROGRAM
```

**Defaults on the first cycle.** Setpoints in holding registers start at 0 after a power-up; set
them once, and keep what an operator wrote later:

<!-- run: cycles=2 -->
<!-- assert: 1 speed_sp=1200 -->
```st
PROGRAM main
VAR_EXTERNAL
    speed_sp : UINT;
END_VAR
    IF FIRST_CYCLE() AND speed_sp = 0 THEN
        speed_sp := 1200;
    END_IF;
END_PROGRAM
```

## Testing a program

**Command line.** `simpleplc check` compiles without a PLC. The first file holds the PROGRAM, the
others are libraries; `check` of a first file without a PROGRAM checks it as a library.
`--tags` takes a tag array, an example's `plc.json` or a `config.json`, and binds VAR_EXTERNAL
as on the PLC; `--listing` prints the program as compiled.

```sh
simpleplc check main.st util.st --tags plc.json
simpleplc check --listing main.st --tags plc.json
```

**Against the plant.** SimplePLC has no simulator of its own. Test the way a commissioning
engineer does: run the program on a PLC that drives no equipment (a laptop, a container or a spare
Raspberry Pi), and let a plant model drive its tags over Modbus TCP or OPC UA: Factory I/O, a
Python script with pymodbus, Node-RED, a hardware-in-the-loop rig, or another PLC whose program
models the machine.

```sh
simpleplc --data /tmp/test-plc --example none     # a blank PLC on this computer
```

- The test PLC runs the same compiler, the same verified code and the same cycle grid as the PLC in
  the field: what passes there is what runs.
- Watch program variables (Program tab, `get_variables`), force tags to test interlocks, and save
  changes online: the next cycle takes them over.
- Programs give the same bits on every platform SimplePLC runs on (macOS, Linux with glibc or
  musl, 64- and 32-bit ARM, x86-64): SIN, EXP, LN and the other transcendental functions are
  SimplePLC's own (from musl's maths library), not the system's.

**From the API or an agent.** The same checks, downloads, variable reads and tag writes are REST
routes and MCP tools (`check_program`, `set_program`, `get_variables`, `write_values`, ...),
described in `docs/api.md`.

## Coming from CODESYS or Python

| Habit | In SimplePLC |
|---|---|
| Integers wrap around | Overflow stops the PLC (`F_OVERFLOW`). Test the limit, or use a saturating counter. |
| `INT * INT` computed in 32 bits | Computed in INT. Widen first: `INT_TO_DINT(a) * b`. |
| `REAL_TO_INT(2.5) = 3` | 2: ties go to the even neighbour. |
| `WORD + 1`, `INT AND INT` | Bit strings have no arithmetic and integers have no bit logic: convert (`WORD_TO_UINT`, `INT_TO_WORD`). |
| `ELSEIF`, `!=`, `STRING(80)`, `5E3` | `ELSIF`, `<>`, `STRING[80]`, `5.0E3`. |
| `i := 16#FFFF` for an INT | `E_LITERAL_RANGE`: based literals are numbers (65535). Write `-1` or `WORD_TO_INT(16#FFFF)`. |
| `TIME_TO_DINT` counts seconds (MatIEC, OpenPLC) | Milliseconds (LTIME: nanoseconds). |
| `x MOD 2 = 1` for odd negatives | `-3 MOD 2` is −1: test `x MOD 2 <> 0`. |
