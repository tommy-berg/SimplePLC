#include "st/value.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>

#include "vm/duration.h"

namespace simpleplc::st {

void Value::store(const Type* t, uint8_t* dst) const {
    t = baseOf(t);
    TypeKind k = t->kind == TypeKind::Enum ? t->enumBase->kind : t->kind;
    switch (k) {
        case TypeKind::Real: {
            float f = static_cast<float>(r);
            std::memcpy(dst, &f, 4);
            return;
        }
        case TypeKind::LReal:
            std::memcpy(dst, &r, 8);
            return;
        case TypeKind::String: {
            size_t n = std::min<size_t>(s.size(), t->capacity);
            uint16_t len = static_cast<uint16_t>(n);
            std::memcpy(dst, &len, 2);
            std::memcpy(dst + 2, s.data(), n);
            return;
        }
        default: {
            uint64_t b = t->kind == TypeKind::Enum ? static_cast<uint64_t>(i) : bits();
            uint32_t size = isElementary(k) ? baseOf(t)->size : 0;
            if (t->kind == TypeKind::Enum) size = t->enumBase->size;
            std::memcpy(dst, &b, size);  // little endian: the low bytes
        }
    }
}

std::string durationText(int64_t ns, bool ltime) {
    std::string out = ltime ? "LT#" : "T#";
    uint64_t v = 0;
    if (ns < 0) {
        out += '-';
        v = static_cast<uint64_t>(-(ns + 1)) + 1;
    } else {
        v = static_cast<uint64_t>(ns);
    }
    if (v == 0) return out + "0s";
    struct Unit {
        const char* name;
        uint64_t ns;
    };
    static constexpr Unit kUnits[] = {{"d", 86400000000000ULL}, {"h", 3600000000000ULL}, {"m", 60000000000ULL},
                                      {"s", 1000000000ULL},      {"ms", 1000000ULL},       {"us", 1000ULL},
                                      {"ns", 1ULL}};
    for (const Unit& u : kUnits) {
        if (v >= u.ns) {
            out += std::format("{}{}", v / u.ns, u.name);
            v %= u.ns;
        }
    }
    return out;
}

std::string Value::text() const {
    switch (kind) {
        case TypeKind::Bool: return i ? "TRUE" : "FALSE";
        case TypeKind::Real:
        case TypeKind::LReal: {
            char buf[64];
            double d = r;
            std::to_chars_result res = kind == TypeKind::Real
                                           ? std::to_chars(buf, buf + sizeof buf, static_cast<float>(d))
                                           : std::to_chars(buf, buf + sizeof buf, d);
            std::string num(buf, res.ptr);
            // As an ST literal: a mantissa with a point and an E exponent (3e+38 → 3.0E38, 1e-05 → 1.0E-5)
            std::string mantissa = num, exponent;
            if (size_t e = num.find('e'); e != std::string::npos) {
                mantissa = num.substr(0, e);
                exponent = std::format("E{}", std::stoi(num.substr(e + 1)));
            }
            if (mantissa.find('.') == std::string::npos) mantissa += ".0";
            return std::format("{}#{}{}", elementaryName(kind), mantissa, exponent);
        }
        case TypeKind::Time: return durationText(i, false);
        case TypeKind::LTime: return durationText(i, true);
        case TypeKind::String: {
            std::string out = "'";
            for (char c : s) {
                if (c == '\'') out += "$'";
                else if (c == '$') out += "$$";
                else if (c == '\n') out += "$N";
                else if (c == '\r') out += "$R";
                else if (c == '\t') out += "$T";
                else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F)
                    out += std::format("${:02X}", static_cast<unsigned char>(c));
                else out += c;
            }
            return out + "'";
        }
        default:
            if (isBitString(kind)) return std::format("{}#16#{:X}", elementaryName(kind), u);  // bit patterns in hex
            if (isUnsigned(kind)) return std::format("{}#{}", elementaryName(kind), u);
            if (isSigned(kind)) return std::format("{}#{}", elementaryName(kind), i);
            return std::to_string(i);
    }
}

bool parseDuration(std::string_view text, int64_t& out, std::string& error) {
    // The one parser for durations: the VM's STRING_TO_TIME reads the same function (vm/duration.h),
    // so a literal and the same text converted at run time can never differ
    vm::DurationPart part;
    vm::DurationError e = vm::parseDurationBody(text, out, &part);
    if (e == vm::DurationError::None) return true;
    std::string number;
    for (char c : part.number) {
        if (c != '_') number += c;
    }
    std::string unit(part.unit);
    for (char& c : unit) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    switch (e) {
        case vm::DurationError::None: break;
        case vm::DurationError::Empty: error = "it is empty"; break;
        case vm::DurationError::FractionNotLast: error = "only the last part may have a fraction"; break;
        case vm::DurationError::MissingNumber: error = "a number is missing"; break;
        case vm::DurationError::MissingUnit: error = "a unit is missing (d, h, m, s, ms, us, ns)"; break;
        case vm::DurationError::UnknownUnit: error = std::format("unknown unit '{}'", unit); break;
        case vm::DurationError::UnitOrder: error = "units must go from largest to smallest, each once"; break;
        case vm::DurationError::NotANumber: error = std::format("'{}' is not a number", number); break;
        case vm::DurationError::AfterLarger: error = std::format("{}{} is out of range after a larger unit", number, unit); break;
        case vm::DurationError::TooLong: error = "it is too long"; break;
    }
    return false;
}

}  // namespace simpleplc::st
