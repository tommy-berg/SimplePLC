# REST API, fleet and MCP

A SimplePLC's web server serves three things on one port (8080 by default): the panel, the
REST API under `/api/v1`, and the MCP endpoint `/mcp` for agents. The panel uses the same API,
so everything it does, a script or an agent can do. `GET /api/v1/openapi.json` (public)
describes every route as OpenAPI 3.1 and says which ones need an administrator. The source of
this document is `src/web/web_server.cpp` (routes), `src/web/mcp.cpp` (tools) and
`src/web/fleet.cpp` (the fleet).

Requests and answers are JSON unless a route says otherwise; a request body may have up to
2 MB. An error is `{"error": {"code", "message", "details"}}` with the HTTP status: 400
`bad_request`, 401 `unauthorized`, 403 `forbidden`, 404 `not_found`, 409 `conflict` or
`tag_in_use`, 412 `config_changed` or `program_changed`, 413 `too_large`, 415
`unsupported_media_type`, 422 `invalid` (with `problems`, one line per field), 429
`too_many_attempts`, 503 `busy`; from a fleet also 502 `device_refused` and `device_bad_answer`.

## Authentication

- **First run.** Until a user exists, the console prints a setup code. `POST /auth/setup`
  `{setup_code, username, password}` creates the first administrator and signs in; the panel
  checks the code first with `POST /auth/setup/check {code}`, which does not use it.
- **Users and sessions.** A user is an `admin` or a `viewer`. `POST /auth/login {username,
  password}` sets the session cookie `simpleplc_session` (HttpOnly, SameSite=Strict, seven
  days); sessions live in memory, so a restart signs everybody out. A request with a cookie
  that is not a GET must carry the header `X-Requested-With: SimplePLC`, or it is refused (403):
  browsers send cookies across sites, but never this header. Passwords have at least 8
  characters, are not the user name or a very common password, and changing your own needs
  the current one (`PUT /users/:name/password`).
- **Tokens.** `Authorization: Bearer <token>`. A token is created under *Users and tokens* or
  with `POST /tokens {name, role}`, shown once and stored as a hash. Removing a user revokes
  the tokens that user created. `SIMPLEPLC_TOKEN` in the environment is an administrator
  token for scripts, agents and fleets; it is never written to disk.
- **Limits.** After 5 wrong passwords or setup codes, or 10 wrong tokens, from one address
  within 10 minutes, that address waits 30 seconds (429, with the seconds). A correct token
  nobody could guess (a generated one, or a `SIMPLEPLC_TOKEN` of 16 characters or more) still
  works from a waiting address, so one misconfigured client does not lock out the others
  behind the same address.
- **Access.** *Public* routes need nothing. *Viewer* routes read. *Admin* routes change the
  PLC, its users and tokens, and connect to other hosts (probing, Modbus reads and writes).

## Routes

Paths are under `/api/v1`. `:id` is the PLC id; a device has one PLC, a fleet lists the PLCs
of its devices and passes every `/plcs/:id/...` request on to the device that runs that PLC.

**Authentication** (public)

| Method | Path | What |
|---|---|---|
| GET | `/auth/state` | `{needs_setup, version, mode: "device" or "fleet", user}` |
| POST | `/auth/setup` | create the first administrator with the setup code; sets the cookie |
| POST | `/auth/setup/check` | check `{code}` without using it: 200, 403 wrong, 409 setup done, 429 |
| POST | `/auth/login` | sign in; sets the cookie |
| POST | `/auth/logout` | sign out |

**System**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/status` | viewer | `{version, mode, plc, uptime_s, data_dir, web, running, warnings}`; a fleet: its devices |
| GET | `/logs` | viewer | `{records, last_seq, started_ms}`; `?since=&level=&plc=&limit=` (limit up to 2000) |
| GET | `/events` | viewer | live log records and PLC summaries (Server-Sent Events) |
| GET | `/settings` | admin | `{web: {listen, port}, log: {level}}` |
| PATCH | `/settings` | admin | change them |
| GET | `/examples` | viewer | the bundled examples |
| GET | `/examples/:name` | viewer | one example: tags, tasks, links, programs |
| GET | `/docs/structured-text` | viewer | the Structured Text reference (Markdown) |
| GET | `/docs/st-compliance` | viewer | the IEC 61131-3 compliance statement (Markdown) |
| GET | `/openapi.json` | public | this API |

**The PLC**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/plcs` | viewer | the PLC's summary (a fleet: every device's, with `online`) |
| GET | `/plcs/:id` | viewer | summary, `config`, `config_version` (also the `ETag`), tasks, `fault`, `modbus` (clients, links, identities), `opcua`, `last_change` |
| PATCH | `/plcs/:id` | admin | JSON merge patch of the configuration; the answer lists `restarted_tasks` |
| POST | `/plcs/:id/setup` | admin | set the PLC up from scratch: `{name, tags, tasks, programs, links, ...}` or `{"example": "tank"}`; the network settings stay unless given |
| GET | `/plcs/:id/device` | viewer | the device: model, OS, CPU, memory, storage, network |
| POST | `/plcs/:id/run` | admin | RUN (after a FAULT: a warm restart) |
| POST | `/plcs/:id/stop` | admin | STOP; tags with a substitute value get it. The process powers up in the mode commanded last (`/run`, `/stop`, `/restart`): a STOP survives a restart of the process, like on a hardware PLC |
| POST | `/plcs/:id/restart` | admin | cold restart: tags from 0, variables from their initial values, programs reloaded, RUN |
| GET | `/plcs/:id/events` | viewer | live values and status (Server-Sent Events); with `?limit=N` or `Accept: application/json`, the newest events instead (see Events) |

**Tags and values**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/plcs/:id/tags` | viewer | every tag (`name`, `area`, `address`, `type`, `description`, `substitute`, `retain`) with `value`, `forced`, `origin`, `writable`, and who writes it (`writer`, `sent_by`, `status_of`) |
| PUT | `/plcs/:id/tags` | admin | replace the tag table |
| POST | `/plcs/:id/tags` | admin | add a tag (201) |
| PATCH | `/plcs/:id/tags/:name` | admin | change a tag |
| DELETE | `/plcs/:id/tags/:name` | admin | delete a tag |
| GET | `/plcs/:id/values` | viewer | `?tags=a,b`; all when omitted |
| PUT | `/plcs/:id/values` | admin | `{"tag": value, ...}`, written together; a float32 value is rounded to float32, and NaN, infinity and values beyond ±3.4028235e+38 are refused |
| GET | `/plcs/:id/areas/:area` | viewer | raw entries: `?start=&count=` (count up to 2000) |
| PUT | `/plcs/:id/areas/:area` | admin | `{"start", "values"}`: bits as true/false, registers −32768…65535; a write that would make a float32 tag NaN or infinite is refused |
| GET | `/plcs/:id/forces` | viewer | forced entries with their tags |
| POST | `/plcs/:id/forces` | admin | force tags: `{"tag": value}` |
| DELETE | `/plcs/:id/forces` | admin | release `{"tags": [...]}`, or all |

**Tasks and programs**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/plcs/:id/tasks` | viewer | tasks with cycle statistics, `watchdog_effective_ms`, errors |
| PATCH | `/plcs/:id/tasks/:task` | admin | `cycle_ms`, `program`, `enabled`, `priority`, `watchdog_ms` |
| GET | `/plcs/:id/variables` | viewer | program variables: `?task=&prefix=`; at most 5000, then `truncated` |
| PUT | `/plcs/:id/variables` | admin | `{"task", "values": {"MAIN.X": 1}}`; ST literals are accepted; all checked, then all written |
| GET | `/plcs/:id/programs` | viewer | program files with size, last change and `used_by` |
| GET | `/plcs/:id/programs/:name` | viewer | the source as `text/plain`; `ETag`: its version |
| PUT | `/plcs/:id/programs/:name` | admin | save and load (see Programs) |
| GET | `/plcs/:id/programs/:name/deployed` | viewer | the files the tasks run the program with (what a restart runs) |
| POST | `/plcs/:id/programs/:name/check` | viewer | compile without saving: `text/plain` or `{"source", "tags"}`; diagnostics, memory, `online_change` |
| GET | `/plcs/:id/programs/:name/compiled` | viewer | the program as compiled: diagnostics, memory, listing |
| DELETE | `/plcs/:id/programs/:name` | admin | delete a program no task runs and no program uses; its history keeps it |
| GET | `/plcs/:id/programs/:name/history` | viewer | earlier versions |
| GET | `/plcs/:id/programs/:name/history/:version` | viewer | the source of one |
| POST | `/plcs/:id/programs/:name/restore` | admin | `{"version", "deploy"}`: make it the current one and load it |

**Users and tokens**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/users` | admin | users |
| POST | `/users` | admin | `{name, password, role}` (201) |
| DELETE | `/users/:name` | admin | remove a user and the tokens they created |
| PUT | `/users/:name/password` | viewer | your own password (with `current_password`), or any as administrator |
| GET | `/tokens` | admin | tokens: id, name, role, created by, last used |
| POST | `/tokens` | admin | `{name, role}`; the token is in the answer, once (201) |
| DELETE | `/tokens/:token` | admin | revoke by id |

**Probing and the Modbus client** (admin)

| Method | Path | What |
|---|---|---|
| POST | `/plcs/:id/probe` | `{host, port, unit, timeout_ms}`: a device's identification (function 43/14) and up to 64 entries of each area from address 0, read from this PLC's place in the network; never writes |
| POST | `/modbus/read` | `{host, port, unit, area, address, count, timeout_ms}`: read any Modbus TCP device |
| POST | `/modbus/write` | `{host, port, unit, area, address, values, timeout_ms}`: write coils or holding registers |

**Adoption by a fleet**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/hello` | public | `{product, version, plc, needs_setup, mode}` |
| POST | `/pairing` | admin | a pairing code for a fleet: `{code, expires_ms}` |
| POST | `/pair` | public | `{code, fleet}`: what a fleet does with the code; answers `{token, plc}`; limited like the setup code |
| DELETE | `/pair` | admin | leave the fleet: revokes the token this request signs in with |

**Fleet only**

| Method | Path | Access | What |
|---|---|---|---|
| GET | `/fleet/devices` | viewer | `[{url, plc, online, error, source}]`; `source` is `flag` for `--device`, `added` for adopted |
| POST | `/fleet/devices` | admin | adopt: `{url, code}` or `{url, token}` (201) |
| DELETE | `/fleet/devices/:plc` | admin | remove the device that runs this PLC; its token is revoked on it when it is reachable |

## Changing the configuration

`GET /plcs/:id` returns the configuration with its `config_version`, also as the `ETag`.
`PATCH /plcs/:id`, `POST /plcs/:id/setup`, the tag routes and `PATCH /plcs/:id/tasks/:task`
accept `If-Match: "<config_version>"`: when the configuration has changed since, nothing
happens and the answer is 412 `config_changed` with the current version. Changes are checked
field by field; an unknown key is refused and the answer names the closest known one. Only
what changed restarts: a task that stays keeps its program and values, and the answer lists
`restarted_tasks`. A task naming a program that does not exist is refused unless `?create=1`
(the program starts from an empty template); one whose program does not compile is refused.
A tag change that would make a running program fail to compile is refused with 409
`tag_in_use`, with the programs and their diagnostics; `?force=1` applies it anyway, and the
program faults with `F_BINDING`.

## Programs

`GET /plcs/:id/programs/:name` gives the source as text with an `ETag`. `PUT` takes the new
source as `text/plain` or as `{"source": "..."}` (a form body is refused, 415); with
`If-Match: <the ETag read>` the save fails with 412 `program_changed` when someone else saved
the file since. The file is saved and loaded at the next cycle boundary into the tasks that
run it and the programs that use it as a library; the answer says whether it `loaded`, lists
every diagnostic (`code`, `severity`, `file`, `line`, `column`, `message`, `hint`) and, for an
online change, the variables that start again from their initial value (`reset`). A program
that does not compile is kept as a draft: the previous version keeps running, also after a
restart. `?deploy=false` saves without loading. Program files hold at most 1 MB each and 2 MB
together. `POST .../check` compiles the same way without saving; for a program a task runs it
adds `online_change: {tasks, kept, reset}`.

## Events: who changed what

Every change is an event with its author: program downloads and online changes (with how many
values were kept and reset), draft saves, forces and releases, value writes, changes of tags,
links, tasks and settings, RUN, STOP and cold restart, faults, links going up and down, and
changes of users and tokens. `GET /plcs/:id/events?limit=50` (1 to 500; or
`Accept: application/json`) returns the newest, oldest first:

```json
{"events": [{"seq": 412, "t_ms": 1761380000000, "event": "write", "level": "info",
             "message": "wrote level = 42.5", "by": {"kind": "token", "name": "agent"}}]}
```

`event` is `program`, `force`, `release`, `write`, `config`, `mode`, `link` or `access`; `by`
is `{"kind": "user" | "token" | "system", "name"}`: a signed-in user, an API token (give each
agent its own) or SimplePLC itself (faults, links). The PLC's summary carries the newest change
a user or token made as `last_change`, and the log records of `GET /logs` carry `event` and
`by` too. Through a fleet, the author is who asked the fleet and the token the fleet signs in
with: `"tommy via fleet:line2"`. The fleet sends that name in the header `X-SimplePLC-For`;
a device records it only from a request with an admin token, and it grants nothing.

## Live streams

`GET /events` streams log records as they come and every PLC's summary every two seconds;
`GET /plcs/:id/events` streams the tag values that changed (with their origin and the forced
entries) and the PLC's status once a second. Both are Server-Sent Events, send a comment every
15 seconds when nothing happens, check the caller's credentials every two seconds, and end
with `gone` when the PLC is set up anew or renamed. At most 32 streams are open at a time
(503 `busy`).

## The fleet

`simpleplc fleet --device URL ...` (or `SIMPLEPLC_DEVICES=tank,alarms`) runs a fleet: one
panel, API and MCP endpoint for the PLCs of many devices, like an engineering station on the
plant network. It has no PLC of its own; its users live in its own data directory
(`./simpleplc-fleet`). It signs in to the devices given with `--device` using
`SIMPLEPLC_DEVICE_TOKEN` (default: its `SIMPLEPLC_TOKEN`) and does not start without one. It
polls every device once a second for its PLC and its log, so `GET /plcs`, `GET /status` and
`GET /logs` show the whole network; a device it cannot reach shows as an offline PLC. What the
fleet does not know about such a device is `null`, not a value: `running`, `tag_count`,
`image_size` and the Modbus and OPC UA `enabled`, `listening`, `port` and `url` of a device
never reached, and `running` of one that stopped answering (the rest is its last answer). Two
devices whose PLCs have the same id are a warning, and requests for that id are refused (409)
rather than sent to one of them; a rename through the fleet to an id another device has is
refused the same way.

**Adoption.** A fleet can start without devices and adopt them: an administrator of the device
asks it for a pairing code (its panel, or `POST /pairing`: 8 characters without look-alikes,
valid for 10 minutes and once; a new code replaces the old one) and gives the fleet the
device's address and the code (`POST /fleet/devices {url, code}`, the panel, or the MCP tool
`add_device`). The fleet trades the code with the device's `POST /pair {code, fleet}` for an
admin token of its own, named `fleet:<fleet name>` and listed and revocable on the device like
any token; `{url, token}` adopts a device with an admin token you already have. Adopted
devices are kept in the fleet's `fleet.json`, readable by its owner only. Removing a device
(`DELETE /fleet/devices/:plc`, `remove_device`) revokes the fleet's token on it when it is
reachable; devices from the command line cannot be removed at run time.

**Relay.** Every `/plcs/:id/...` request goes to the device that runs the PLC, after the same
access check as on a device. `If-Match` and the query (`?force=1`, `?create=1`, `?deploy=false`)
reach the device, and its `ETag` comes back. Only JSON and text answers are passed on (anything
else is 502 `device_bad_answer`); a device that refuses the fleet's token is 502
`device_refused`, not a sign-out of the user. At most 8 requests wait for one device at a time
(503 `busy`). The fleet trusts a device with its own PLC only: what it answers is checked
before use, and its log records are filed under its PLC.

## MCP

`POST /mcp` is a Model Context Protocol endpoint (Streamable HTTP with JSON answers; `GET`
and `DELETE` answer 405). It takes an API token only (`Authorization: Bearer`; a session
cookie is refused), refuses a browser `Origin` that is not its own host, and speaks the
protocol versions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05 (an `MCP-Protocol-Version`
header naming another is 400). JSON-RPC batches are accepted; a notification is answered with
202. On stdio, `simpleplc mcp` runs the same server with a PLC of its own (blank on the first
run, Modbus and OPC UA on 127.0.0.1 unless `--listen`, no web panel unless `--web` or `--port`),
and `simpleplc mcp --url URL --token TOKEN` relays stdio to a running SimplePLC or fleet.

**Tools.** Every argument is checked against the tool's schema before anything runs: a
missing, misspelt or wrongly typed one is refused by name. `plc` may be left out on a device
(it has one PLC); in a fleet it picks the device. A viewer token may use the read-only tools;
the others, and the tools that connect to other hosts (`modbus_read`, `modbus_write`,
`probe_device`, `add_device`), need an admin token. Each tool carries the annotations
`readOnlyHint`, `destructiveHint`, `idempotentHint` and `openWorldHint`.

| Tools | What |
|---|---|
| `list_plcs`, `get_plc`, `get_device` | the PLCs with state, ports and `config_version`; one PLC in detail; the device it runs on |
| `setup_plc`, `update_plc`, `set_tags`, `update_task` | the configuration: from scratch (or from an `example`), as a merge patch, the tag table, one task; all take `config_version` and `set_tags`/`update_plc` take `force` |
| `run_plc`, `stop_plc`, `restart_plc` | RUN, STOP, cold restart |
| `read_values`, `write_values`, `read_area`, `write_area` | tags by name; raw addresses |
| `force_values`, `release_forces`, `list_forces` | forcing |
| `get_variables`, `set_variables` | program variables of a task, between two cycles |
| `list_programs`, `get_program`, `set_program`, `check_program`, `get_compiled`, `delete_program`, `get_program_history`, `restore_program` | programs: `get_program` gives the source and its `hash`, `set_program` takes it as `base_hash` and `deploy: false` keeps a draft, `check_program` compiles without saving |
| `get_events`, `get_logs` | who changed what; the log |
| `list_examples`, `get_example`, `get_language_reference`, `get_compliance` | the examples and the two documents |
| `probe_device`, `modbus_read`, `modbus_write` | other Modbus TCP devices |
| `add_device`, `remove_device` | a fleet's devices (fleet only) |

**Resources** are `simpleplc://docs/structured-text`, `simpleplc://docs/st-compliance`,
`simpleplc://examples/<name>`, `simpleplc://plcs/<id>/tags` and
`simpleplc://plcs/<id>/programs/<name>` (the last two also as templates). **Prompts**:
`write_plc_program` (`plc`, `behaviour`) writes, checks, deploys and verifies a program;
`build_network` (`scenario`) sets up several PLCs that talk Modbus TCP to each other. The
server's `instructions` on `initialize` describe the workflow.

In a fleet, `list_plcs` answers from the fleet, tools about one PLC run on its device (with
`X-SimplePLC-For` naming who asked), and the rest (examples, documents, the fleet's log, Modbus
from the fleet's place in the network) run on the fleet.
