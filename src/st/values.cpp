#include "st/values.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include "st/lexer.h"
#include "st/parser.h"
#include "st/value.h"
#include "vm/vm.h"

namespace simpleplc::st {

using json = nlohmann::json;

std::string variableText(const vm::Image& image, const vm::VarEntry& v, const uint8_t* data) {
    TypeKind k = static_cast<TypeKind>(v.kind);
    const uint8_t* p = data + v.offset;
    Value val;
    val.kind = k;
    switch (k) {
        case TypeKind::Bool: val.i = p[0] ? 1 : 0; break;
        case TypeKind::Real: { float f; std::memcpy(&f, p, 4); val.r = static_cast<double>(f); break; }
        case TypeKind::LReal: std::memcpy(&val.r, p, 8); break;
        case TypeKind::String: {
            uint16_t len;
            std::memcpy(&len, p, 2);
            uint32_t cap = v.size >= 2 ? v.size - 2 : 0;
            if (len > cap) len = static_cast<uint16_t>(cap);
            val.s.assign(reinterpret_cast<const char*>(p + 2), len);
            break;
        }
        case TypeKind::Array: return "(array)";
        default: {
            uint64_t bits = 0;
            std::memcpy(&bits, p, std::min<uint32_t>(v.size, 8));
            if (isSigned(k) || isDuration(k)) {
                uint32_t w = v.size * 8;
                if (w < 64 && (bits >> (w - 1)) & 1) bits |= ~((uint64_t{1} << w) - 1);
                val.i = static_cast<int64_t>(bits);
            } else {
                val.u = bits;
            }
            for (const vm::EnumInfo& e : image.enums) {
                if (e.name != v.typeName) continue;
                int64_t n = isSigned(k) ? val.i : static_cast<int64_t>(val.u);
                for (const auto& [value, name] : e.values) {
                    if (value == n) return e.name + "#" + name;
                }
            }
        }
    }
    return val.text();
}

namespace {

/// Writes an ST literal into a variable with the rules of an assignment in the program: the
/// literal must fit the type, a subrange's limits and an enumeration's values; CONSTANT variables
/// cannot be written. The variable of a read-only tag (VAR_EXTERNAL CONSTANT) can: this only stores
/// the bytes, and a PLC decides which tags may be written before it gets here. False, with `error`,
/// if the literal does not fit.
bool storeLiteral(const vm::Image& img, const vm::VarEntry& v, const std::string& text, uint8_t* data, std::string& error) {
    TypeKind k = static_cast<TypeKind>(v.kind);
    if ((v.flags & vm::kVarConstant) && !(v.flags & vm::kVarExternal)) {
        error = std::format("{} is CONSTANT", v.path);
        return false;
    }
    Diagnostics d;
    ast::ExprPtr e = parseExpression(text, d);
    if (!e || d.hasErrors()) {
        error = std::format("'{}' is not a literal", text);
        return false;
    }
    bool neg = false;
    if (e->kind == ast::ExprKind::Unary && e->as<ast::Unary>().op == ast::UnaryOp::Neg) {
        neg = true;
        ast::ExprPtr inner = std::move(e->as<ast::Unary>().operand);
        e = std::move(inner);
    }
    uint8_t* p = data + v.offset;
    auto fail = [&](std::string why) {
        error = std::format("{} for {} ({})", why, v.path, v.typeName);
        return false;
    };
    for (const vm::EnumInfo& en : img.enums) {
        if (en.name != v.typeName) continue;
        // An enumeration takes its own value names: Red or Color#Red
        std::string name;
        if (e->kind == ast::ExprKind::Name) name = e->as<ast::Name>().upper;
        else if (e->kind == ast::ExprKind::TypedLit && toUpper(e->as<ast::TypedLit>().prefix) == toUpper(en.name)) {
            name = toUpper(e->as<ast::TypedLit>().body);
        }
        std::string names;
        for (const auto& [value, spelled] : en.values) {
            if (!neg && toUpper(spelled) == name) {
                std::memcpy(p, &value, v.size);  // little-endian: the low bytes hold the value
                return true;
            }
            names += (names.empty() ? "" : ", ") + spelled;
        }
        return fail(std::format("needs one of {}", names));
    }
    if (e->kind == ast::ExprKind::TypedLit && !isDuration(k) && !e->as<ast::TypedLit>().quoted) {
        // INT#5, REAL#1.5, WORD#16#FF, BOOL#TRUE: the form monitoring shows values in. The type must be the variable's
        const auto& t = e->as<ast::TypedLit>();
        if (toUpper(t.prefix) != elementaryName(k)) return fail(std::format("needs a {} value, not {}#", elementaryName(k), t.prefix));
        return storeLiteral(img, v, (neg ? "-" : "") + t.body, data, error);
    }
    if (k == TypeKind::Bool) {
        if (e->kind != ast::ExprKind::BoolLit) return fail("needs TRUE or FALSE");
        p[0] = e->as<ast::BoolLit>().value ? 1 : 0;
        return true;
    }
    if (k == TypeKind::String) {
        if (e->kind != ast::ExprKind::StringLit) return fail("needs a 'string'");
        const std::string& s = e->as<ast::StringLit>().value;
        if (s.size() + 2 > v.size) return fail("string too long");
        uint16_t len = static_cast<uint16_t>(s.size());
        std::memcpy(p, &len, 2);
        std::memcpy(p + 2, s.data(), s.size());
        return true;
    }
    if (isDuration(k)) {
        if (e->kind != ast::ExprKind::TypedLit) return fail("needs a duration such as T#1s");
        const auto& t = e->as<ast::TypedLit>();
        // Reuse the compiler's duration rules through a one-line program
        std::string prog = std::format("PROGRAM p VAR x : {}; END_VAR x := {}{}#{}; END_PROGRAM",
                                       k == TypeKind::Time ? "TIME" : "LTIME", neg ? "-" : "", t.prefix, t.body);
        CompileResult r = compile({{"value.st", prog}}, {});
        if (!r.ok) return fail("invalid duration");
        vm::Machine m(r.image);
        m.cycle(0);
        for (const vm::VarEntry& x : r.image->vars) {
            if (x.path == "P.X") std::memcpy(p, m.data() + x.offset, 8);
        }
        return true;
    }
    if (isReal(k)) {
        double d0 = 0;
        if (e->kind == ast::ExprKind::RealLit) d0 = e->as<ast::RealLit>().value;
        else if (e->kind == ast::ExprKind::IntLit) d0 = static_cast<double>(e->as<ast::IntLit>().value);
        else return fail("needs a number");
        if (neg) d0 = -d0;
        if (!std::isfinite(d0) || (k == TypeKind::Real && std::fabs(d0) > 3.4028234663852886e38)) return fail("out of range");
        if (k == TypeKind::Real) {
            float f = static_cast<float>(d0);
            std::memcpy(p, &f, 4);
        } else {
            std::memcpy(p, &d0, 8);
        }
        return true;
    }
    if (isInteger(k) || isBitString(k)) {
        if (e->kind != ast::ExprKind::IntLit) return fail("needs an integer");
        uint64_t mag = e->as<ast::IntLit>().value;
        uint64_t bits = 0;
        if (neg) {
            if (!isSigned(k) || mag > static_cast<uint64_t>(-(minValue(k) + 1)) + 1) return fail("out of range");
            bits = static_cast<uint64_t>(-static_cast<int64_t>(mag - 1) - 1);
        } else {
            if (mag > maxValue(k)) return fail("out of range");
            bits = mag;
        }
        if (v.hasRange) {
            bool inside = isSigned(k) ? (static_cast<int64_t>(bits) >= v.low && static_cast<int64_t>(bits) <= v.high)
                                      : (bits >= static_cast<uint64_t>(v.low) && bits <= static_cast<uint64_t>(v.high));
            if (!inside) return fail(std::format("outside {}..{}", v.low, v.high));
        }
        std::memcpy(p, &bits, v.size);
        return true;
    }
    return fail("cannot be set");
}

}  // namespace

nlohmann::json variableJson(const vm::Image& image, const vm::VarEntry& v, const uint8_t* data) {
    TypeKind k = static_cast<TypeKind>(v.kind);
    const uint8_t* p = data + v.offset;
    bool isEnum = false;
    for (const vm::EnumInfo& e : image.enums) isEnum = isEnum || e.name == v.typeName;
    if (k == TypeKind::Bool) return p[0] != 0;
    if (!isEnum && k == TypeKind::Real) {
        float f;
        std::memcpy(&f, p, 4);
        return static_cast<double>(f);
    }
    if (!isEnum && k == TypeKind::LReal) {
        double d;
        std::memcpy(&d, p, 8);
        return d;
    }
    if (!isEnum && (isInteger(k) || isBitString(k))) {
        uint64_t bits = 0;
        std::memcpy(&bits, p, std::min<uint32_t>(v.size, 8));
        if (isSigned(k)) {
            uint32_t w = v.size * 8;
            if (w < 64 && (bits >> (w - 1)) & 1) bits |= ~((uint64_t{1} << w) - 1);
            return static_cast<int64_t>(bits);
        }
        return bits;
    }
    return variableText(image, v, data);
}

std::expected<std::vector<uint8_t>, std::string> literalBytes(const vm::Image& image, const vm::VarEntry& v, const std::string& text) {
    vm::VarEntry at0 = v;
    at0.offset = 0;
    std::vector<uint8_t> bytes(v.size, 0);
    std::string error;
    if (!storeLiteral(image, at0, text, bytes.data(), error)) return std::unexpected(error);
    return bytes;
}

}  // namespace simpleplc::st
