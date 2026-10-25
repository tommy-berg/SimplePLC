# musl libm (double-precision elementary functions)

The REAL and LREAL functions of SimplePLC's VM (`src/vm/fmath.h`: SIN, COS, TAN, ASIN, ACOS,
ATAN, ATAN2, EXP, LN, LOG, EXPT) are these files, built from source, so a program gives the
same bits on every platform. The platform's libm does not: macOS, glibc and musl differ in
the last bit for many inputs. A golden-bits check across eight builds (macOS, Alpine and
Ubuntu on arm64, x86-64 and armv7), kept outside this repository, shows these files giving
the same bits everywhere.

- Source: musl 1.2.6, https://musl.libc.org/releases/musl-1.2.6.tar.gz
- SHA-256 of the tarball: `d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a`
  (its signature `musl-1.2.6.tar.gz.asc` verified with musl's key
  `8364 8929 0BB6 B70F 99FF DA05 56BC DB59 3020 450F`, https://musl.libc.org/musl.pub)
- License: MIT (`COPYRIGHT`, musl's file, unchanged). The files carry their original notices:
  FreeBSD/fdlibm (Sun Microsystems, permissive) and Arm Limited (MIT).

## Files

From `src/math/`: `sin.c cos.c tan.c __sin.c __cos.c __tan.c __rem_pio2.c __rem_pio2_large.c
asin.c acos.c atan.c atan2.c exp.c exp_data.c exp_data.h log.c log_data.c log_data.h log10.c
pow.c pow_data.c pow_data.h floor.c scalbn.c __math_divzero.c __math_invalid.c __math_oflow.c
__math_uflow.c __math_xflow.c`.

`libm.h` is SimplePLC's replacement for musl's `src/internal/libm.h`: musl's helper macros,
without long double support, plus

- every symbol renamed `spl_<name>` (`spl_sin`, `spl___rem_pio2`, `spl___exp_data`, ...), so
  neither the system libm, nor libc.a in a static musl build, nor a compiler builtin can stand in;
- musl's fused multiply-add paths switched off (`log.c`, `pow.c`, `log_data.h` take them when the
  compiler predefines `__FP_FAST_FMA`, as on arm64), so every platform does the same operations;
- `sqrt` and `fabs` as compiler builtins: both exact by IEEE 754, and musl itself uses the
  hardware square root on aarch64, arm (VFP) and x86_64;
- an `#error` unless `FLT_EVAL_METHOD == 0` (no x87 excess precision) and without `-ffast-math`.

Changes to musl's files, each marked `SimplePLC`:

- `exp_data.h`, `log_data.h`, `pow_data.h`: `#include "libm.h"` instead of `<features.h>`;
- `log10.c`, `scalbn.c`: `#include "libm.h"` added (for the renames).

## Build

`CMakeLists.txt`, target `simpleplc_musl_math`: C99 with `-ffp-contract=off -fno-builtin
-fno-fast-math -frounding-math -fno-math-errno -fno-strict-aliasing` and warnings off (`-w`),
as musl's own build (`-ffp-contract=off` added: Clang contracts `a*b + c` into a fused
multiply-add by default).

## Updating

Take the same files from a newer release, apply the two changes above, compare `libm.h` with
the new `src/internal/libm.h`, then repeat the golden-bits check across the eight builds
(macOS, Alpine and Ubuntu on arm64, x86-64 and armv7; it lives outside this repository). A
change of results needs new golden bits that every build agrees on.
