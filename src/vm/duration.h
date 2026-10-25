#pragma once
// Duration text → nanoseconds, shared by the compiler (T#… literals, st/value.cpp) and the
// virtual machine (STRING_TO_TIME, vm/vm.cpp), so the same text means the same value in both:
// exact integer arithmetic (no floating point: `long double` is a double on arm64 and 80 bits on
// x86-64, so a product rounded through it differs between devices), no allocation (the cycle path).

#include <cstdint>
#include <string_view>
#include <utility>

namespace simpleplc::vm {

/// Why a duration body was refused; the compiler turns these into messages.
enum class DurationError : uint8_t {
    None,
    Empty,            ///< no part at all
    FractionNotLast,  ///< only the last part may have a fraction
    MissingNumber,    ///< a unit without a number (T#h)
    MissingUnit,      ///< a number without a unit (T#5)
    UnknownUnit,      ///< not d, h, m, s, ms, us or ns
    UnitOrder,        ///< units must go from largest to smallest, each once
    NotANumber,       ///< '.' alone, leading or trailing, or two of them
    AfterLarger,      ///< a part after the first is not within its unit (T#1h75m)
    TooLong,          ///< beyond 2^63 - 1 nanoseconds (or -2^63)
};

/// The number and unit text of the part a parse stopped at (for messages).
struct DurationPart {
    std::string_view number;
    std::string_view unit;
};

namespace duration_detail {

inline bool isDigitChar(char c) { return c >= '0' && c <= '9'; }
inline bool isLetterChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// `word` equals the upper-case `name`, ignoring the case of `word`.
inline bool equalsUpper(std::string_view word, std::string_view name) {
    if (word.size() != name.size()) return false;
    for (size_t i = 0; i < word.size(); i++) {
        char c = word[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != name[i]) return false;
    }
    return true;
}

/// unit × digits / 10^count exactly: the whole nanoseconds and how the rest compares with one half
/// (-1 below, 0 a tie, 1 above). unit < 2^47 and digits < 10^18, so the product needs up to 107
/// bits: a 64 × 64 → 128-bit product and a bitwise division (no __int128, which 32-bit ARM lacks).
inline std::pair<uint64_t, int> scaledFraction(uint64_t unit, uint64_t digits, int count) {
    uint64_t a0 = unit & 0xFFFFFFFFu, a1 = unit >> 32, b0 = digits & 0xFFFFFFFFu, b1 = digits >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    uint64_t lo = (p00 & 0xFFFFFFFFu) | (mid << 32);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    uint64_t den = 1;
    for (int k = 0; k < count; k++) den *= 10;
    uint64_t q = 0, r = 0;
    for (int bit = 127; bit >= 0; bit--) {
        bool top = (r >> 63) != 0;
        r = (r << 1) | ((bit >= 64 ? hi >> (bit - 64) : lo >> bit) & 1);
        q <<= 1;
        if (top || r >= den) {
            r -= den;
            q |= 1;
        }
    }
    uint64_t twice = r * 2;  // r < den ≤ 10^18, so this does not overflow
    return {q, twice < den ? -1 : twice == den ? 0 : 1};
}

}  // namespace duration_detail

/**
 * The body of a duration (after T# or LT#): [+|-]{number unit} with units d h m s ms us ns (any
 * case) from largest to smallest, each once, '_' between parts and digits, a fraction only in the
 * last part, and parts after the first within their unit (1h75m is refused; 90s is fine). The
 * result is exact: a fraction is rounded to the nearest nanosecond, ties to even (as REAL to
 * integer conversions). `part` (optional) names the part a refusal stopped at.
 */
inline DurationError parseDurationBody(std::string_view s, int64_t& out, DurationPart* part = nullptr) {
    using namespace duration_detail;
    struct Unit {
        std::string_view name;
        uint64_t ns;
        uint64_t limit;  ///< exclusive upper bound when not the first part
    };
    static constexpr Unit kUnits[] = {{"D", 86400000000000ULL, 0},  {"H", 3600000000000ULL, 24},
                                      {"M", 60000000000ULL, 60},     {"S", 1000000000ULL, 60},
                                      {"MS", 1000000ULL, 1000},      {"US", 1000ULL, 1000},
                                      {"NS", 1ULL, 1000}};
    auto fail = [&](DurationError e, std::string_view number, std::string_view unit) {
        if (part) *part = DurationPart{number, unit};
        return e;
    };
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        neg = s[i] == '-';
        i++;
    }
    // A duration is a whole number of nanoseconds up to 2^63 - 1 (down to -2^63)
    const uint64_t max = neg ? uint64_t{1} << 63 : static_cast<uint64_t>(INT64_MAX);
    uint64_t total = 0;
    int lastUnit = -1;
    bool any = false, fractional = false;
    auto add = [&](uint64_t ns) {
        if (ns > max - total) return false;
        total += ns;
        return true;
    };
    while (i < s.size()) {
        if (s[i] == '_') {
            i++;
            continue;
        }
        if (fractional) return fail(DurationError::FractionNotLast, {}, {});
        size_t start = i;
        while (i < s.size() && (isDigitChar(s[i]) || s[i] == '.' || s[i] == '_')) i++;
        std::string_view number = s.substr(start, i - start);
        size_t ustart = i;
        while (i < s.size() && isLetterChar(s[i])) i++;
        std::string_view unit = s.substr(ustart, i - ustart);
        // The number: digits of the whole part (exactly, in 64 bits), digits of the fraction
        uint64_t value = 0, digits = 0;
        int wholeDigits = 0, fracDigits = 0;
        bool dot = false, bad = false, tooLong = false, sticky = false;
        for (char c : number) {
            if (c == '_') continue;
            if (c == '.') {
                if (dot) bad = true;
                dot = true;
                continue;
            }
            uint64_t d = static_cast<uint64_t>(c - '0');
            if (!dot) {
                wholeDigits++;
                if (__builtin_mul_overflow(value, uint64_t{10}, &value) || __builtin_add_overflow(value, d, &value)) tooLong = true;
            } else if (fracDigits < 18) {
                // Digits past the 18th are below a nanosecond for every unit: they only tell a tie from
                // a value just above it
                fracDigits++;
                digits = digits * 10 + d;
            } else if (d != 0) {
                sticky = true;
            }
        }
        if (wholeDigits == 0 && fracDigits == 0 && !dot) return fail(DurationError::MissingNumber, number, unit);
        int u = -1;
        for (int k = 0; k < 7; k++) {
            if (equalsUpper(unit, kUnits[k].name)) u = k;
        }
        if (u < 0) return fail(unit.empty() ? DurationError::MissingUnit : DurationError::UnknownUnit, number, unit);
        if (u <= lastUnit) return fail(DurationError::UnitOrder, number, unit);
        if (bad || (dot && (wholeDigits == 0 || fracDigits == 0))) return fail(DurationError::NotANumber, number, unit);
        fractional = dot;
        if (!tooLong && any && kUnits[u].limit && value >= kUnits[u].limit) return fail(DurationError::AfterLarger, number, unit);
        uint64_t ns = 0;
        if (tooLong || __builtin_mul_overflow(value, kUnits[u].ns, &ns) || !add(ns)) return fail(DurationError::TooLong, number, unit);
        if (fracDigits > 0) {
            auto [whole, half] = scaledFraction(kUnits[u].ns, digits, fracDigits);
            if (half == 0 && sticky) half = 1;
            if (!add(whole)) return fail(DurationError::TooLong, number, unit);
            // To the nearest nanosecond, ties to even
            if ((half > 0 || (half == 0 && (total & 1))) && !add(1)) return fail(DurationError::TooLong, number, unit);
        }
        lastUnit = u;
        any = true;
    }
    if (!any) return fail(DurationError::Empty, {}, {});
    out = neg ? static_cast<int64_t>(0 - total) : static_cast<int64_t>(total);
    return DurationError::None;
}

/**
 * Duration text as a program or a user may write it: an optional T#, TIME#, LT# or LTIME# prefix
 * (any case) and the body as parseDurationBody reads it. For STRING_TO_TIME and STRING_TO_LTIME.
 */
inline bool parseDurationText(std::string_view s, int64_t& out) {
    for (std::string_view prefix : {"LTIME#", "LT#", "TIME#", "T#"}) {
        if (s.size() >= prefix.size() && duration_detail::equalsUpper(s.substr(0, prefix.size()), prefix)) {
            s.remove_prefix(prefix.size());
            break;
        }
    }
    return parseDurationBody(s, out) == DurationError::None;
}

}  // namespace simpleplc::vm
