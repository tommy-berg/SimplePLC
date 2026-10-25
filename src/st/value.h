#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "st/types.h"

namespace simpleplc::st {

/**
 * A compile-time value: a literal, a folded constant expression or an initial
 * value. Integers are kept in the field matching their signedness; REAL values
 * are already rounded to float precision.
 */
struct Value {
    TypeKind kind = TypeKind::Void;
    int64_t i = 0;       ///< signed integers, durations (ns), BOOL (0/1), enumeration values
    uint64_t u = 0;      ///< unsigned integers and bit strings
    double r = 0;        ///< REAL, LREAL
    std::string s;       ///< STRING bytes

    static Value ofBool(bool b) { Value v; v.kind = TypeKind::Bool; v.i = b ? 1 : 0; return v; }
    static Value ofSigned(TypeKind k, int64_t x) { Value v; v.kind = k; v.i = x; return v; }
    static Value ofUnsigned(TypeKind k, uint64_t x) { Value v; v.kind = k; v.u = x; return v; }
    static Value ofReal(TypeKind k, double x) {
        Value v;
        v.kind = k;
        v.r = k == TypeKind::Real ? static_cast<double>(static_cast<float>(x)) : x;
        return v;
    }
    static Value ofString(std::string x) { Value v; v.kind = TypeKind::String; v.s = std::move(x); return v; }

    /// The 64-bit two's complement pattern of an integer, bit string, BOOL or duration.
    uint64_t bits() const {
        if (isUnsigned(kind) || isBitString(kind)) return u;
        return static_cast<uint64_t>(i);
    }
    /// Stores the value as `t` occupies memory (little endian, natural layout).
    void store(const Type* t, uint8_t* dst) const;
    /// Text in ST syntax, e.g. INT#-3, REAL#1.5, T#1s500ms, 'abc'.
    std::string text() const;
};

/// A duration in nanoseconds as ST text: T#1h2m3s4ms (LT# for LTIME).
std::string durationText(int64_t ns, bool ltime);

/**
 * The body of a duration literal (after T# or LT#): [-]{number unit} with units
 * d h m s ms us ns (any case) from largest to smallest, each once, '_' between
 * parts, a fraction only in the last part, and parts after the first within
 * their unit (T#1h75m is refused; T#90s is fine). False with `error` set otherwise.
 */
bool parseDuration(std::string_view body, int64_t& ns, std::string& error);

}  // namespace simpleplc::st
