/*
 * SimplePLC's replacement for musl 1.2.6 src/internal/libm.h (MIT, see COPYRIGHT).
 *
 * The helper macros and inline functions below are musl's, unchanged. Changed:
 * - long double support (ldshape, <endian.h>, "fp_arch.h") is gone: these files
 *   need none, and macOS has no <endian.h>; the generic fp_barrier is used;
 * - every symbol is renamed to spl_<name> so the system libm, libc.a (static
 *   musl builds) and compiler builtins never stand in for these functions;
 * - musl's fused multiply-add paths (log.c, pow.c, log_data.h use them when the
 *   compiler predefines __FP_FAST_FMA, as on arm64) are switched off, so every
 *   platform runs the same operations and gets the same bits;
 * - sqrt and fabs are the compiler's: both are exact by IEEE 754 (sqrt is
 *   correctly rounded), and musl itself uses the hardware instruction on
 *   aarch64, arm (VFP) and x86_64.
 *
 * Build with -ffp-contract=off -fno-builtin and never with -ffast-math
 * (CMakeLists.txt, target simpleplc_musl_math).
 */
#ifndef _LIBM_H
#define _LIBM_H

#include <stdint.h>
#include <float.h>
#include <math.h>

#ifdef __FAST_MATH__
#error "musl-math must not be built with -ffast-math"
#endif
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "musl-math needs FLT_EVAL_METHOD == 0 (no excess precision, e.g. SSE2 rather than x87)"
#endif

/* The same operations everywhere: no fused multiply-add, even where it is fast */
#undef __FP_FAST_FMA
#undef __FP_FAST_FMAF

#if defined(__GNUC__) || defined(__clang__)
#define hidden __attribute__((__visibility__("hidden")))
#define sqrt(x) __builtin_sqrt(x)
#define fabs(x) __builtin_fabs(x)
#else
#define hidden
#endif

/* Symbol renames (after <math.h>, whose declarations keep the system names) */
#define sin spl_sin
#define cos spl_cos
#define tan spl_tan
#define asin spl_asin
#define acos spl_acos
#define atan spl_atan
#define atan2 spl_atan2
#define exp spl_exp
#define log spl_log
#define log10 spl_log10
#define pow spl_pow
#define floor spl_floor
#define scalbn spl_scalbn
#define __sin spl___sin
#define __cos spl___cos
#define __tan spl___tan
#define __rem_pio2 spl___rem_pio2
#define __rem_pio2_large spl___rem_pio2_large
#define __math_xflow spl___math_xflow
#define __math_uflow spl___math_uflow
#define __math_oflow spl___math_oflow
#define __math_divzero spl___math_divzero
#define __math_invalid spl___math_invalid
#define __exp_data spl___exp_data
#define __log_data spl___log_data
#define __pow_log_data spl___pow_log_data

double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double exp(double);
double log(double);
double log10(double);
double pow(double, double);
double floor(double);
double scalbn(double, int);

/* Support non-nearest rounding mode.  */
#define WANT_ROUNDING 1
/* Support signaling NaNs.  */
#define WANT_SNAN 0

#if WANT_SNAN
#error SNaN is unsupported
#else
#define issignalingf_inline(x) 0
#define issignaling_inline(x) 0
#endif

#ifndef TOINT_INTRINSICS
#define TOINT_INTRINSICS 0
#endif

#if TOINT_INTRINSICS
#error "musl-math uses the portable code paths only"
#endif

/* Helps static branch prediction so hot path can be better optimized.  */
#ifdef __GNUC__
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x) (x)
#define predict_false(x) (x)
#endif

/* Evaluate an expression as the specified type. With standard excess
   precision handling a type cast or assignment is enough (with
   -ffloat-store an assignment is required, in old compilers argument
   passing and return statement may not drop excess precision).  */

static inline float eval_as_float(float x)
{
	float y = x;
	return y;
}

static inline double eval_as_double(double x)
{
	double y = x;
	return y;
}

/* fp_barrier returns its input, but limits code transformations
   as if it had a side-effect (e.g. observable io) and returned
   an arbitrary value.  */

static inline float fp_barrierf(float x)
{
	volatile float y = x;
	return y;
}

static inline double fp_barrier(double x)
{
	volatile double y = x;
	return y;
}

/* fp_force_eval ensures that the input value is computed when that's
   otherwise unused.  To prevent the constant folding of the input
   expression, an additional fp_barrier may be needed or a compilation
   mode that does so (e.g. -frounding-math in gcc). Then it can be
   used to evaluate an expression for its fenv side-effects only.   */

static inline void fp_force_evalf(float x)
{
	volatile float y;
	y = x;
}

static inline void fp_force_eval(double x)
{
	volatile double y;
	y = x;
}

#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		fp_force_evalf(x);                \
	} else {                                  \
		fp_force_eval(x);                 \
	}                                         \
} while(0)

#define asuint(f) ((union{float _f; uint32_t _i;}){f})._i
#define asfloat(i) ((union{uint32_t _i; float _f;}){i})._f
#define asuint64(f) ((union{double _f; uint64_t _i;}){f})._i
#define asdouble(i) ((union{uint64_t _i; double _f;}){i})._f

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64_t __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint32_t)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint32_t)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

#define GET_FLOAT_WORD(w,d)                       \
do {                                              \
  (w) = asuint(d);                                \
} while (0)

#define SET_FLOAT_WORD(d,w)                       \
do {                                              \
  (d) = asfloat(w);                               \
} while (0)

hidden int    __rem_pio2_large(double*,double*,int,int,int);

hidden int    __rem_pio2(double,double*);
hidden double __sin(double,double,int);
hidden double __cos(double,double);
hidden double __tan(double,double,int);

/* error handling functions */
hidden double __math_xflow(uint32_t, double);
hidden double __math_uflow(uint32_t);
hidden double __math_oflow(uint32_t);
hidden double __math_divzero(uint32_t);
hidden double __math_invalid(double);

#endif
