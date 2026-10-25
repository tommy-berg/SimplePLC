#pragma once
// REAL and LREAL elementary functions with the same bits on every platform
// (same inputs, same outputs, on any device). They are musl 1.2.6's
// libm compiled from source (third_party/musl-math/, symbols renamed spl_*) with
// -ffp-contract=off and no fused multiply-add paths, never the platform's libm:
// macOS, glibc and musl give different last bits for many inputs. A golden-bits check
// across eight builds (macOS, Alpine and Ubuntu on arm64, x86-64 and armv7), kept
// outside this repository, shows these functions giving the same bits everywhere.
//
// REAL is computed in double and rounded once to float, so it is the same on every
// platform too, and nearly always the correctly rounded float result.
//
// SQRT is not here: IEEE 754 requires square root to be correctly rounded, so
// std::sqrt (one instruction on arm64, x86-64 and ARM VFP) is already the same everywhere.

extern "C" {
double spl_sin(double) noexcept;
double spl_cos(double) noexcept;
double spl_tan(double) noexcept;
double spl_asin(double) noexcept;
double spl_acos(double) noexcept;
double spl_atan(double) noexcept;
double spl_atan2(double, double) noexcept;
double spl_exp(double) noexcept;
double spl_log(double) noexcept;
double spl_log10(double) noexcept;
double spl_pow(double, double) noexcept;
}

namespace simpleplc::vm::fmath {

inline double sin(double x) { return spl_sin(x); }
inline double cos(double x) { return spl_cos(x); }
inline double tan(double x) { return spl_tan(x); }
inline double asin(double x) { return spl_asin(x); }
inline double acos(double x) { return spl_acos(x); }
inline double atan(double x) { return spl_atan(x); }
/// The angle of (x, y), −π…π.
inline double atan2(double y, double x) { return spl_atan2(y, x); }
inline double exp(double x) { return spl_exp(x); }
/// Natural logarithm.
inline double log(double x) { return spl_log(x); }
inline double log10(double x) { return spl_log10(x); }
inline double pow(double x, double y) { return spl_pow(x, y); }

// REAL: the double function, rounded once to float
inline float sin(float x) { return static_cast<float>(spl_sin(static_cast<double>(x))); }
inline float cos(float x) { return static_cast<float>(spl_cos(static_cast<double>(x))); }
inline float tan(float x) { return static_cast<float>(spl_tan(static_cast<double>(x))); }
inline float asin(float x) { return static_cast<float>(spl_asin(static_cast<double>(x))); }
inline float acos(float x) { return static_cast<float>(spl_acos(static_cast<double>(x))); }
inline float atan(float x) { return static_cast<float>(spl_atan(static_cast<double>(x))); }
inline float atan2(float y, float x) {
    return static_cast<float>(spl_atan2(static_cast<double>(y), static_cast<double>(x)));
}
inline float exp(float x) { return static_cast<float>(spl_exp(static_cast<double>(x))); }
inline float log(float x) { return static_cast<float>(spl_log(static_cast<double>(x))); }
inline float log10(float x) { return static_cast<float>(spl_log10(static_cast<double>(x))); }
inline float pow(float x, float y) {
    return static_cast<float>(spl_pow(static_cast<double>(x), static_cast<double>(y)));
}

}  // namespace simpleplc::vm::fmath
