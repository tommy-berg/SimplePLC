# Third-party components

| Component | Version | License | Where |
|-----------|---------|---------|-------|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.60.1 | MIT | `third_party/httplib/` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | `third_party/json/` |
| [open62541](https://www.open62541.org) | 1.5.9 (or an installed 1.4+) | MPL-2.0 | built from its upstream source, unmodified |
| [musl](https://musl.libc.org) libm | 1.2.6 | MIT | `third_party/musl-math/` (the double-precision elementary functions; symbols renamed, see its README) |
| [CodeMirror](https://codemirror.net/5/) | 5.65.21 | MIT | `src/web/assets/vendor/codemirror/` (the Structured Text mode `st.js` is SimplePLC's own) |

Each directory contains the component's license file. open62541 is used without
changes; its source is available at https://github.com/open62541/open62541 under
the Mozilla Public License 2.0.

The musl files are unchanged except for their includes (marked `SimplePLC`); `libm.h` replaces
musl's internal header and renames every symbol `spl_*` (`third_party/musl-math/README.md`).
