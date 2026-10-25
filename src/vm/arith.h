#pragma once
// Arithmetic shared by the compiler's constant folding and the virtual machine, so
// a constant expression and the same expression computed at run time can never
// disagree. Every function reports failure instead of wrapping.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "st/types.h"

namespace simpleplc::vm {

using st::TypeKind;

/// Run-time faults. Stable numbers: they are stored in images and logs.
enum class Fault : uint8_t {
    None = 0,
    Overflow,        ///< F_OVERFLOW
    DivZero,         ///< F_DIV_ZERO
    Real,            ///< F_REAL: ±inf or NaN from finite operands
    Domain,          ///< F_DOMAIN: SQRT(-1), LN(0), shift by a negative count …
    Index,           ///< F_INDEX
    Subrange,        ///< F_SUBRANGE
    StringLength,    ///< F_STRING_LENGTH
    StringPosition,  ///< F_STRING_POSITION
    Conversion,      ///< F_CONVERSION
    ForStep,         ///< F_FOR_STEP
    Watchdog,        ///< F_WATCHDOG
    Binding,         ///< F_BINDING
    Reference,       ///< internal: a reference outside the data block (cannot happen in a verified image)
    Budget,          ///< F_BUDGET: the cycle's instruction budget is spent (deterministic watchdog)
};

const char* faultCode(Fault f);
const char* faultText(Fault f);

/// Signed integer result of `kind` (SINT … LINT, TIME, LTIME): in range?
inline bool fitsSigned(TypeKind kind, int64_t v) {
    if (st::isDuration(kind)) return true;
    return v >= st::minValue(kind) && v <= static_cast<int64_t>(st::maxValue(kind));
}
/// Unsigned integer or bit-string result of `kind`: in range?
inline bool fitsUnsigned(TypeKind kind, uint64_t v) { return v <= st::maxValue(kind); }

/// Integer arithmetic on signed kinds (incl. durations); returns Fault::None or the fault.
inline Fault signedOp(char op, TypeKind kind, int64_t a, int64_t b, int64_t& out) {
    switch (op) {
        case '+': if (__builtin_add_overflow(a, b, &out)) return Fault::Overflow; break;
        case '-': if (__builtin_sub_overflow(a, b, &out)) return Fault::Overflow; break;
        case '*': if (__builtin_mul_overflow(a, b, &out)) return Fault::Overflow; break;
        case '/':
            if (b == 0) return Fault::DivZero;
            if (a == std::numeric_limits<int64_t>::min() && b == -1) return Fault::Overflow;
            out = a / b;  // C++ truncates toward zero, as IEC 61131-3 requires
            break;
        case '%':
            // MOD: IN1 - (IN1 / IN2) * IN2; MOD by zero yields 0
            if (b == 0) { out = 0; return Fault::None; }
            if (b == -1) { out = 0; return Fault::None; }
            out = a % b;
            break;
        default: return Fault::Overflow;
    }
    return fitsSigned(kind, out) ? Fault::None : Fault::Overflow;
}

inline Fault unsignedOp(char op, TypeKind kind, uint64_t a, uint64_t b, uint64_t& out) {
    switch (op) {
        case '+': if (__builtin_add_overflow(a, b, &out)) return Fault::Overflow; break;
        case '-': if (__builtin_sub_overflow(a, b, &out)) return Fault::Overflow; break;
        case '*': if (__builtin_mul_overflow(a, b, &out)) return Fault::Overflow; break;
        case '/':
            if (b == 0) return Fault::DivZero;
            out = a / b;
            break;
        case '%':
            out = b == 0 ? 0 : a % b;
            break;
        default: return Fault::Overflow;
    }
    return fitsUnsigned(kind, out) ? Fault::None : Fault::Overflow;
}

/// REAL arithmetic in float, LREAL in double. Results ±inf or NaN are faults.
template <typename F>
inline Fault realOp(char op, F a, F b, F& out) {
    switch (op) {
        case '+': out = a + b; break;
        case '-': out = a - b; break;
        case '*': out = a * b; break;
        case '/':
            if (b == F(0)) return Fault::DivZero;
            out = a / b;
            break;
        default: return Fault::Real;
    }
    return std::isfinite(out) ? Fault::None : Fault::Real;
}

/// Duration arithmetic with a number: TIME * n, TIME / n (n converted to double for REAL operands).
inline Fault durationScale(char op, int64_t t, double n, int64_t& out) {
    double r = 0;
    if (op == '*') {
        r = static_cast<double>(t) * n;
    } else {
        if (n == 0.0) return Fault::DivZero;
        r = static_cast<double>(t) / n;
    }
    if (!std::isfinite(r) || r >= 9.2233720368547758e18 || r < -9.2233720368547758e18) return Fault::Overflow;
    out = static_cast<int64_t>(std::nearbyint(r));
    return Fault::None;
}

/// REAL/LREAL → integer: round to nearest, ties to even (IEC 60559); NaN/∞/out of range fault.
inline Fault realToInteger(double v, TypeKind to, bool truncate, uint64_t& bits) {
    if (!std::isfinite(v)) return Fault::Conversion;
    double r = truncate ? std::trunc(v) : std::nearbyint(v);
    if (st::isSigned(to) || st::isDuration(to)) {
        if (r < -9.2233720368547758e18 || r >= 9.2233720368547758e18) return Fault::Conversion;
        int64_t i = static_cast<int64_t>(r);
        if (!fitsSigned(to, i)) return Fault::Conversion;
        bits = static_cast<uint64_t>(i);
    } else {
        if (r < 0 || r >= 1.8446744073709552e19) return Fault::Conversion;
        uint64_t u = static_cast<uint64_t>(r);
        if (!fitsUnsigned(to, u)) return Fault::Conversion;
        bits = u;
    }
    return Fault::None;
}

/// Shifts and rotates of a `width`-bit value; count must be ≥ 0 (checked by the caller).
inline uint64_t shiftOp(char op, uint32_t width, uint64_t v, uint64_t n) {
    uint64_t mask = width == 64 ? ~uint64_t{0} : ((uint64_t{1} << width) - 1);
    v &= mask;
    switch (op) {
        case 'l': return n >= width ? 0 : (v << n) & mask;
        case 'r': return n >= width ? 0 : (v >> n);
        case 'L': {  // rotate left
            n %= width;
            if (n == 0) return v;
            return ((v << n) | (v >> (width - n))) & mask;
        }
        default: {  // rotate right
            n %= width;
            if (n == 0) return v;
            return ((v >> n) | (v << (width - n))) & mask;
        }
    }
}

/// A scalar in transit: integers, bit strings, BOOL and durations as a 64-bit pattern
/// (signed values sign-extended), reals as double.
struct Scalar {
    uint64_t bits = 0;
    double real = 0;
};

/**
 * Elementary conversion, shared by constant folding and the VM: integers convert
 * by value with a range check; bit strings convert by bit pattern (narrowing keeps
 * the low bits, widening fills with zeros, the same width into a signed type
 * sign-extends); reals round to nearest, ties to even (or truncate); TIME converts
 * to and from milliseconds, LTIME nanoseconds; BOOL is 0/1 and anything non-zero is TRUE.
 */
inline Fault convertScalar(TypeKind from, Scalar in, TypeKind to, bool truncate, Scalar& out) {
    using st::isBitString;
    using st::isDuration;
    using st::isReal;
    using st::isSigned;
    using st::isUnsigned;
    out = Scalar{};
    if (from == to) {
        out = in;
        return Fault::None;
    }
    constexpr double kNsPerMs = 1e6;
    if (isDuration(from) && isDuration(to)) {  // TIME and LTIME both count nanoseconds
        out.bits = in.bits;
        return Fault::None;
    }
    // To BOOL
    if (to == TypeKind::Bool) {
        out.bits = isReal(from) ? (in.real != 0.0) : (in.bits != 0);
        return Fault::None;
    }
    // To a real
    if (isReal(to)) {
        double d = 0;
        if (isReal(from)) d = in.real;
        else if (from == TypeKind::Time) d = static_cast<double>(static_cast<int64_t>(in.bits)) / kNsPerMs;
        else if (isSigned(from) || from == TypeKind::LTime) d = static_cast<double>(static_cast<int64_t>(in.bits));
        else d = static_cast<double>(in.bits);
        if (to == TypeKind::Real) {
            float f = static_cast<float>(d);
            if (!std::isfinite(f)) return Fault::Conversion;
            out.real = static_cast<double>(f);
        } else {
            out.real = d;
        }
        return Fault::None;
    }
    // From a real to an integer, bit string or duration
    if (isReal(from)) {
        if (isBitString(to)) return Fault::Conversion;
        double v = in.real;
        if (to == TypeKind::Time) v *= kNsPerMs;
        return realToInteger(v, isDuration(to) ? TypeKind::LInt : to, truncate, out.bits);
    }
    // Bit strings take part by bit pattern: narrowing keeps the low bits, widening fills with
    // zeros, and a bit string read as a signed type is sign-extended from its width
    if ((isBitString(from) || isBitString(to)) && !isDuration(from) && !isDuration(to) && from != TypeKind::Bool) {
        uint32_t wf = st::bitWidth(from), wt = st::bitWidth(to);
        if (isSigned(from) && !isBitString(from) && wt > wf) return Fault::Conversion;  // refused by the compiler
        uint64_t m = in.bits & (wf == 64 ? ~uint64_t{0} : ((uint64_t{1} << wf) - 1));
        if (wt < 64) m &= (uint64_t{1} << wt) - 1;
        if (isSigned(to) && wt < 64 && (m >> (wt - 1)) & 1) m |= ~((uint64_t{1} << wt) - 1);
        if (isSigned(to) || isUnsigned(to) || isBitString(to)) {
            // An integer target narrower than the source keeps the low bits as a pattern only when the
            // source is a bit string; integer to integer stays value-preserving (below)
            out.bits = m;
            return Fault::None;
        }
    }
    // Integer-like to integer-like: the value as a signed or unsigned 64-bit number
    bool srcSigned = isSigned(from) || isDuration(from);
    int64_t sv = static_cast<int64_t>(in.bits);
    uint64_t uv = in.bits;
    if (from == TypeKind::Time) {
        sv = sv / 1000000;  // whole milliseconds, toward zero
        uv = static_cast<uint64_t>(sv);
    }
    if (srcSigned && sv < 0 && (isUnsigned(to) || isBitString(to))) {
        // Negative into an unsigned type: only the same-width bit pattern rule allows it
        if (isBitString(to) && st::bitWidth(to) == st::bitWidth(from)) {
            out.bits = in.bits & st::maxValue(to);
            return Fault::None;
        }
        return Fault::Conversion;
    }
    if (isBitString(from) && isSigned(to) && st::bitWidth(from) == st::bitWidth(to)) {
        // Same width: the pattern, sign-extended
        uint32_t w = st::bitWidth(to);
        uint64_t m = in.bits & st::maxValue(from);
        if (w < 64 && (m >> (w - 1)) & 1) m |= ~((uint64_t{1} << w) - 1);
        out.bits = m;
        return Fault::None;
    }
    if (isDuration(to)) {
        int64_t v = 0;
        if (srcSigned) v = sv;
        else if (uv > static_cast<uint64_t>(INT64_MAX)) return Fault::Conversion;
        else v = static_cast<int64_t>(uv);
        if (to == TypeKind::Time && __builtin_mul_overflow(v, int64_t{1000000}, &v)) return Fault::Conversion;
        out.bits = static_cast<uint64_t>(v);
        return Fault::None;
    }
    if (isSigned(to)) {
        if (srcSigned) {
            if (!fitsSigned(to, sv)) return Fault::Conversion;
            out.bits = static_cast<uint64_t>(sv);
        } else {
            if (uv > st::maxValue(to)) return Fault::Conversion;
            out.bits = uv;
        }
        return Fault::None;
    }
    // to unsigned or bit string
    if (srcSigned && sv < 0) return Fault::Conversion;
    uint64_t v = srcSigned ? static_cast<uint64_t>(sv) : uv;
    if (v > st::maxValue(to)) return Fault::Conversion;
    out.bits = v;
    return Fault::None;
}

}  // namespace simpleplc::vm
