// Semantic analysis of Structured Text: names, types, conversions and the strict
// rules of the compliance statement. Every rule that rejects a program says why and how to write
// it; every implicit conversion becomes an explicit Convert node, so code
// generation and the "as compiled" listing never guess.

#include "st/sema.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <functional>
#include <cstdlib>
#include <set>

#include "app/embedded.h"
#include "st/lexer.h"
#include "st/parser.h"
#include "vm/arith.h"

namespace simpleplc::st {

using namespace ast;

std::string_view standardLibrarySource() {
    const embedded::File* f = embedded::find("st/stdlib.st");
    return f ? f->text() : std::string_view{};
}

namespace {

/// A CASE label as an order-preserving 64-bit key: unsigned values as they are, signed ones in
/// offset binary (the labels of one CASE all have the selector's type). No __int128: 32-bit ARM lacks it.
using LabelKey = uint64_t;
constexpr LabelKey signedKey(int64_t v) { return static_cast<uint64_t>(v) ^ (uint64_t{1} << 63); }


size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string tname(const Type* t) { return t ? t->name : "?"; }

bool isArithmetic(BinaryOp op) {
    return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
           op == BinaryOp::Mod;
}

/// Standard functions by name; parameter names for formal calls ("IN" … , "*" = extensible IN1, IN2 …).
struct StdSpec {
    std::string_view name;
    StdFn fn;
    std::vector<std::string_view> params;
    bool extensible = false;  ///< more inputs of the last kind may follow
};

const std::vector<StdSpec>& stdSpecs() {
    static const std::vector<StdSpec> specs = {
        {"ABS", StdFn::Abs, {"IN"}}, {"SQRT", StdFn::Sqrt, {"IN"}}, {"LN", StdFn::Ln, {"IN"}},
        {"LOG", StdFn::Log, {"IN"}}, {"EXP", StdFn::Exp, {"IN"}}, {"SIN", StdFn::Sin, {"IN"}},
        {"COS", StdFn::Cos, {"IN"}}, {"TAN", StdFn::Tan, {"IN"}}, {"ASIN", StdFn::Asin, {"IN"}},
        {"ACOS", StdFn::Acos, {"IN"}}, {"ATAN", StdFn::Atan, {"IN"}}, {"EXPT", StdFn::Expt, {"IN1", "IN2"}},
        {"SHL", StdFn::Shl, {"IN", "N"}}, {"SHR", StdFn::Shr, {"IN", "N"}}, {"ROL", StdFn::Rol, {"IN", "N"}},
        {"ROR", StdFn::Ror, {"IN", "N"}}, {"SEL", StdFn::Sel, {"G", "IN0", "IN1"}},
        {"MAX", StdFn::Max, {"IN1", "IN2"}, true}, {"MIN", StdFn::Min, {"IN1", "IN2"}, true},
        {"LIMIT", StdFn::Limit, {"MN", "IN", "MX"}}, {"MUX", StdFn::Mux, {"K", "IN0", "IN1"}, true},
        {"GT", StdFn::Gt, {"IN1", "IN2"}, true}, {"GE", StdFn::Ge, {"IN1", "IN2"}, true},
        {"EQ", StdFn::Eq, {"IN1", "IN2"}, true}, {"LE", StdFn::Le, {"IN1", "IN2"}, true},
        {"LT", StdFn::Lt, {"IN1", "IN2"}, true}, {"NE", StdFn::Ne, {"IN1", "IN2"}},
        {"LEN", StdFn::Len, {"IN"}}, {"LEFT", StdFn::Left, {"IN", "L"}}, {"RIGHT", StdFn::Right, {"IN", "L"}},
        {"MID", StdFn::Mid, {"IN", "L", "P"}}, {"CONCAT", StdFn::Concat, {"IN1", "IN2"}, true},
        {"INSERT", StdFn::Insert, {"IN1", "IN2", "P"}}, {"DELETE", StdFn::Delete, {"IN", "L", "P"}},
        {"REPLACE", StdFn::Replace, {"IN1", "IN2", "L", "P"}}, {"FIND", StdFn::Find, {"IN1", "IN2"}},
        {"ATAN2", StdFn::Atan2, {"Y", "X"}},
        {"FIRST_CYCLE", StdFn::FirstCycle, {}},
        {"TIME", StdFn::Time, {}}, {"LTIME", StdFn::LTime, {}},
        {"__NOW", StdFn::Now, {}},
    };
    return specs;
}

const StdSpec* findStd(std::string_view upper) {
    for (const StdSpec& s : stdSpecs()) {
        if (s.name == upper) return &s;
    }
    return nullptr;
}

/// Operator functions rewritten to operators: ADD(a, b, c) is a + b + c.
bool isOperatorFunction(std::string_view u) {
    return u == "ADD" || u == "SUB" || u == "MUL" || u == "DIV" || u == "MOD" || u == "AND" || u == "OR" ||
           u == "XOR" || u == "NOT" || u == "MOVE" || u == "ADD_TIME" || u == "SUB_TIME" || u == "MUL_TIME" ||
           u == "DIV_TIME" || u == "ADD_LTIME" || u == "SUB_LTIME" || u == "MUL_LTIME" || u == "DIV_LTIME";
}

/// X_TO_Y, TO_Y, X_TRUNC_Y, TRUNC_Y, TRUNC (X and Y type names, incl. BCD forms such as WORD_BCD).
bool isConversionNameIn(const TypeTable& types, std::string_view u) {
    auto isTypeName = [&](std::string_view n) {
        if (n.ends_with("_BCD")) n.remove_suffix(4);
        return n == "STRING" || n == "WSTRING" || types.elementaryByName(n) != nullptr;
    };
    if (u == "TRUNC") return true;
    if (u.starts_with("TO_")) return isTypeName(u.substr(3));
    if (u.starts_with("TRUNC_")) return isTypeName(u.substr(6));
    if (u.starts_with("BCD_TO_")) return isTypeName(u.substr(7));
    for (std::string_view sep : {std::string_view("_TRUNC_"), std::string_view("_TO_")}) {
        size_t p = u.find(sep);
        if (p != std::string_view::npos && isTypeName(u.substr(0, p)) && isTypeName(u.substr(p + sep.size())))
            return true;
    }
    return false;
}

struct ForGuard {
    Symbol* control = nullptr;
    std::set<Symbol*> bounds;
};

struct Ctx {
    PouInfo* pou = nullptr;
    const Member* memberTarget = nullptr;  ///< the member being assigned (inst.IN := x)
    int loopDepth = 0;
    std::vector<ForGuard> fors;
    bool initializer = false;  ///< checking an initial value: only constants
};

class Analyzer {
public:
    Analyzer(Program& p, const std::vector<TagDef>& tags, Diagnostics& d, bool library)
        : p_(p), tags_(tags), d_(d), library_(library) {}

    bool run(const std::vector<SourceFile>& files) {
        parseAll(files);
        if (d_.full()) return false;
        collect();
        if (d_.full()) return false;
        resolveAllTypes();
        declareGlobalConstants();
        for (PouInfo& pou : p_.pous) declare(pou, pou.ast->loc);
        if (d_.full()) return false;
        bindTags();
        for (PouInfo& pou : p_.pous) checkBody(pou);
        checkRecursion();
        return !d_.hasErrors();
    }

private:
    // ───────────── Parsing and collection ─────────────

    void parseAll(const std::vector<SourceFile>& files) {
        p_.files.reserve(files.size() + 1);
        for (size_t i = 0; i < files.size(); i++) {
            p_.fileNames.push_back(files[i].name);
            p_.files.push_back(parse(files[i].text, static_cast<uint16_t>(i), d_));
        }
        uint16_t lib = static_cast<uint16_t>(files.size());
        p_.fileNames.push_back("(standard library)");
        p_.files.push_back(parse(standardLibrarySource(), lib, d_, true));
        for (auto& pou : p_.files.back().pous) pou->library = true;
    }

    bool reserved(std::string_view upper) const {
        return p_.types.elementaryByName(upper) || upper == "STRING" || findStd(upper) ||
               isOperatorFunction(upper) || isConversionName(upper) || standardBlocks_.count(std::string(upper));
    }

    bool isConversionName(std::string_view u) const { return isConversionNameIn(p_.types, u); }

    void collect() {
        // The standard function blocks (TON, CTU, …) are standard names like the standard functions
        for (auto& pouAst : p_.files.back().pous) {
            if (!pouAst->upper.empty() && !pouAst->upper.starts_with("__")) standardBlocks_.insert(pouAst->upper);
        }
        std::map<std::string, SourceLoc> names;
        auto claim = [&](const std::string& upper, const std::string& spelled, SourceLoc loc, bool library) {
            if (!library && reserved(upper)) {
                d_.error("E_RESERVED", loc, std::format("'{}' is the name of a standard type or function", spelled),
                         "choose another name");
                return false;
            }
            auto [it, fresh] = names.emplace(upper, loc);
            if (!fresh) {
                bool inStdlib = it->second.file == static_cast<uint16_t>(p_.files.size() - 1);
                d_.error("E_DUPLICATE", loc,
                         inStdlib ? std::format("'{}' is a standard function block", spelled)
                         : it->second.file != loc.file && it->second.file < p_.fileNames.size()
                             ? std::format("'{}' is already declared ({} line {})", spelled, p_.fileNames[it->second.file], it->second.line)
                             : std::format("'{}' is already declared (line {})", spelled, it->second.line),
                         "choose another name");
                return false;
            }
            return true;
        };
        for (File& f : p_.files) {
            bool library = f.index == p_.files.size() - 1;
            for (TypeDecl& td : f.types) {
                if (claim(td.upper, td.spelled, td.loc, library)) typeDecls_[td.upper] = &td;
            }
            for (auto& pouAst : f.pous) {
                Pou& a = *pouAst;
                if (a.upper.empty()) continue;
                // Another program's file: its blocks and functions are shared, its PROGRAM is not ours
                if (a.kind == PouKind::Program && f.index != 0) continue;
                if (!claim(a.upper, a.spelled, a.loc, library)) continue;
                PouInfo& info = p_.pous.emplace_back();
                info.ast = &a;
                info.kind = a.kind;
                info.upper = a.upper;
                info.spelled = a.spelled;
                info.library = a.library;
                p_.pouByName[a.upper] = &info;
                if (a.kind == PouKind::Program) {
                    if (p_.main) {
                        d_.error("E_DUPLICATE", a.loc, "a program file holds exactly one PROGRAM",
                                 "put the second PROGRAM in its own file and give it its own task");
                    } else {
                        p_.main = &info;
                    }
                }
            }
        }
        if (!p_.main && !d_.hasErrors() && !library_) {
            d_.error("E_SYNTAX", SourceLoc{0, 1, 1}, "the program file has no PROGRAM",
                     "write PROGRAM name … END_PROGRAM");
        }
    }

    // ───────────── Types ─────────────

    void resolveAllTypes() {
        for (auto& [upper, td] : typeDecls_) resolveUserType(upper, td->loc);
    }

    const Type* resolveUserType(const std::string& upper, SourceLoc use) {
        auto done = p_.typeByName.find(upper);
        if (done != p_.typeByName.end()) return done->second;
        auto it = typeDecls_.find(upper);
        if (it == typeDecls_.end()) return nullptr;
        if (inProgress_.count(upper)) {
            d_.error("E_RECURSION", use, std::format("type '{}' contains itself", it->second->spelled));
            return nullptr;
        }
        inProgress_.insert(upper);
        TypeDecl& td = *it->second;
        const Type* t = buildType(*td.spec, nullptr, td.spelled, true);
        inProgress_.erase(upper);
        if (!t) t = p_.types.elementary(TypeKind::Int);  // keep going after an error
        // A named enum/struct/subrange/array declared here takes the declared name; an alias shares the target
        if (td.spec->kind != TypeSpecKind::Named && td.spec->kind != TypeSpecKind::String) {
            const_cast<Type*>(t)->name = td.spelled;
        }
        if (td.init) {
            if (td.spec->kind == TypeSpecKind::Named || td.spec->kind == TypeSpecKind::String) {
                // An alias with its own default: a type of its own that behaves as its target. A
                // subrange target keeps its limits (copied with the rest of `t`): a value outside them
                // is refused in the alias too, as initial value (E_LITERAL_RANGE) and at run time (F_SUBRANGE)
                if (baseOf(t)->kind == TypeKind::FunctionBlock) {
                    d_.error("E_TYPE", td.init->loc, "a function block type cannot have a default value here");
                } else {
                    Type* alias = p_.types.newType();
                    *alias = *t;
                    alias->name = td.spelled;
                    alias->rangeBase = baseOf(t);
                    t = alias;
                }
            }
            if (t->kind != TypeKind::FunctionBlock) {
                const_cast<Type*>(t)->defaultInit = td.init.get();
                Ctx ctx;
                ctx.initializer = true;
                checkInitializer(*td.init, t, ctx);
            }
        }
        p_.typeByName[upper] = t;
        return t;
    }

    /// Resolves a type written in a declaration. `ctx` gives constants for array bounds.
    const Type* buildType(TypeSpec& spec, PouInfo* ctx, const std::string& what, bool inTypeDecl = false) {
        switch (spec.kind) {
            case TypeSpecKind::Named: {
                if (spec.upper == "?") return nullptr;
                if (const Type* t = p_.types.elementaryByName(spec.upper)) return t;
                if (const Type* t = resolveUserType(spec.upper, spec.loc)) return t;
                if (PouInfo* fb = p_.findPou(spec.upper)) {
                    if (fb->kind == PouKind::FunctionBlock) return declare(*fb, spec.loc) ? fb->instanceType : nullptr;
                    d_.error("E_TYPE", spec.loc,
                             std::format("'{}' is a {}, not a type", spec.spelled, pouKeyword(fb->kind)));
                    return nullptr;
                }
                if (spec.upper == "WSTRING" || spec.upper == "CHAR" || spec.upper == "WCHAR" ||
                    spec.upper == "DATE" || spec.upper == "TIME_OF_DAY" || spec.upper == "TOD" ||
                    spec.upper == "DATE_AND_TIME" || spec.upper == "DT" || spec.upper == "LDATE" ||
                    spec.upper == "LTOD" || spec.upper == "LDT" || spec.upper.starts_with("ANY")) {
                    d_.error("E_NOT_SUPPORTED", spec.loc, std::format("type {} is not supported yet", spec.spelled),
                             "see docs/st-compliance.md");
                    return nullptr;
                }
                d_.error("E_UNDECLARED", spec.loc, std::format("unknown type '{}'", spec.spelled), suggestType(spec.upper));
                return nullptr;
            }
            case TypeSpecKind::String: {
                uint32_t n = TypeTable::kDefaultStringLength;
                if (spec.length) {
                    int64_t v = 0;
                    if (!constInteger(spec.length, ctx, v, "the string length")) return nullptr;
                    if (v < 1 || v > TypeTable::kMaxStringLength) {
                        d_.error("E_LIMIT", spec.length->loc,
                                 std::format("string length {} is outside 1…{}", v, TypeTable::kMaxStringLength));
                        return nullptr;
                    }
                    n = static_cast<uint32_t>(v);
                }
                return p_.types.string(n);
            }
            case TypeSpecKind::Array: {
                std::vector<ArrayDim> dims;
                if (spec.ranges.size() > 8) {
                    d_.error("E_LIMIT", spec.loc, "an array has at most 8 dimensions");
                    return nullptr;
                }
                uint64_t count = 1;
                for (ArrayRange& r : spec.ranges) {
                    int64_t lo = 0, hi = 0;
                    if (!constInteger(r.low, ctx, lo, "an array bound") || !constInteger(r.high, ctx, hi, "an array bound"))
                        return nullptr;
                    if (lo > hi) {
                        d_.error("E_TYPE", r.low->loc, std::format("array bounds {}..{}: the low bound is above the high bound", lo, hi));
                        return nullptr;
                    }
                    ArrayDim d{lo, hi};
                    count *= d.count();
                    if (count > (uint64_t{1} << 24)) {
                        d_.error("E_LIMIT", spec.loc, "an array has at most 16,777,216 elements");
                        return nullptr;
                    }
                    dims.push_back(d);
                }
                const Type* el = spec.element ? buildType(*spec.element, ctx, what) : nullptr;
                if (!el) return nullptr;
                if (el->kind == TypeKind::Void) return nullptr;
                const Type* t = p_.types.array(el, std::move(dims));
                if (t->size > kMaxDataBytes) {
                    d_.error("E_LIMIT", spec.loc, std::format("{} takes {} bytes; the limit is 16 MB", t->name, t->size));
                    return nullptr;
                }
                return t;
            }
            case TypeSpecKind::Subrange: {
                const Type* base = p_.types.elementaryByName(spec.upper);
                if (!base || !isInteger(base->kind)) {
                    d_.error("E_TYPE", spec.loc, std::format("a subrange needs an integer base type, not '{}'", spec.spelled));
                    return nullptr;
                }
                int64_t lo = 0, hi = 0;
                if (!constInteger(spec.low, ctx, lo, "a subrange bound", base) ||
                    !constInteger(spec.high, ctx, hi, "a subrange bound", base))
                    return nullptr;
                if (lo > hi) {
                    d_.error("E_TYPE", spec.loc, std::format("subrange {}..{}: the low bound is above the high bound", lo, hi));
                    return nullptr;
                }
                Type* t = p_.types.newType();
                *t = *base;
                t->name = std::format("{}({}..{})", base->name, lo, hi);
                t->hasRange = true;
                t->rangeLow = lo;
                t->rangeHigh = hi;
                t->rangeBase = base;
                return t;
            }
            case TypeSpecKind::Enum: {
                if (!inTypeDecl) return nullptr;  // reported by the parser
                const Type* base = p_.types.elementary(TypeKind::Int);
                if (!spec.upper.empty()) {
                    base = p_.types.elementaryByName(spec.upper);
                    if (!base || !isInteger(base->kind)) {
                        d_.error("E_TYPE", spec.loc, "an enumeration's base type must be an integer type");
                        return nullptr;
                    }
                }
                Type* t = p_.types.newType();
                t->kind = TypeKind::Enum;
                t->name = what;
                t->enumBase = base;
                t->size = base->size;
                t->align = base->align;
                int64_t next = 0;
                for (EnumValue& v : spec.values) {
                    if (reserved(v.upper)) {
                        d_.error("E_RESERVED", v.loc, std::format("'{}' is the name of a standard type or function", v.spelled),
                                 "choose another name");
                        continue;
                    }
                    if (p_.findPou(v.upper) || typeDecls_.count(v.upper) || globals_.find(v.upper)) {
                        d_.error("E_DUPLICATE", v.loc,
                                 std::format("'{}' is already the name of a type, function, block or global constant", v.spelled),
                                 "choose another name, so the two cannot be confused");
                        continue;
                    }
                    if (t->enumValue(v.upper)) {
                        d_.error("E_DUPLICATE", v.loc, std::format("'{}' appears twice in the enumeration", v.spelled));
                        continue;
                    }
                    int64_t val = next;
                    if (v.value && !constInteger(v.value, nullptr, val, "an enumeration value", base)) continue;
                    if (!vm::fitsSigned(base->kind, val) && isSigned(base->kind)) {
                        d_.error("E_LITERAL_RANGE", v.loc, std::format("{} does not fit {}", val, base->name));
                        continue;
                    }
                    for (const EnumConstant& other : t->enumValues) {
                        if (other.value == val) {
                            d_.error("E_DUPLICATE", v.loc, std::format("'{}' has the same value as '{}'", v.spelled, other.spelled));
                        }
                    }
                    t->enumValues.push_back({v.upper, v.spelled, val});
                    next = val + 1;
                }
                if (t->enumValues.empty()) return nullptr;
                return t;
            }
            case TypeSpecKind::Struct: {
                if (!inTypeDecl) return nullptr;
                Type* t = p_.types.newType();
                t->kind = TypeKind::Struct;
                t->name = what;
                uint32_t offset = 0;
                uint32_t align = 1;
                for (VarDecl& decl : spec.fields) {
                    const Type* ft = decl.type ? buildType(*decl.type, nullptr, what) : nullptr;
                    if (!ft) continue;
                    if (ft->kind == TypeKind::FunctionBlock) {
                        d_.error("E_NOT_SUPPORTED", decl.type->loc, "a function block instance inside a STRUCT");
                        continue;
                    }
                    for (DeclName& n : decl.names) {
                        if (reserved(n.upper)) {
                            d_.error("E_RESERVED", n.loc, std::format("'{}' is the name of a standard type or function", n.spelled),
                                     "choose another name");
                            continue;
                        }
                        if (t->field(n.upper)) {
                            d_.error("E_DUPLICATE", n.loc, std::format("member '{}' is declared twice", n.spelled));
                            continue;
                        }
                        offset = alignUp(offset, ft->align);
                        t->fields.push_back({n.upper, n.spelled, ft, offset, decl.init.get()});
                        offset += ft->size;
                        align = std::max(align, ft->align);
                    }
                }
                t->align = align;
                t->size = alignUp(offset, align);
                for (Field& f : t->fields) {
                    if (f.init) {
                        Ctx ictx;
                        ictx.initializer = true;
                        checkInitializer(*f.init, f.type, ictx);
                    }
                }
                return t;
            }
        }
        return nullptr;
    }

    std::string suggestType(const std::string& upper) const {
        std::string best;
        size_t bestD = 3;
        for (std::string_view n : {"BOOL", "SINT", "INT", "DINT", "LINT", "USINT", "UINT", "UDINT", "ULINT", "BYTE",
                                   "WORD", "DWORD", "LWORD", "REAL", "LREAL", "TIME", "LTIME", "STRING"}) {
            size_t dist = editDistance(upper, n);
            if (dist < bestD) { bestD = dist; best = n; }
        }
        for (const auto& [n, t] : p_.typeByName) {
            size_t dist = editDistance(upper, n);
            if (dist < bestD) { bestD = dist; best = t->name; }
        }
        return best.empty() ? std::string() : std::format("did you mean {}?", best);
    }

    static uint32_t alignUp(uint32_t v, uint32_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }
    static constexpr uint32_t kMaxDataBytes = 16u * 1024 * 1024;

    /// A constant integer expression (array bounds, string lengths, enum values).
    bool constInteger(ExprPtr& e, PouInfo* pou, int64_t& out, std::string_view what,
                      const Type* type = nullptr) {
        if (!e) return false;
        Ctx ctx;
        ctx.pou = pou;
        ctx.initializer = true;
        if (!type) type = p_.types.elementary(TypeKind::DInt);
        const Type* t = typeExpr(e, type, ctx);
        if (!t) return false;
        if (!isInteger(baseOf(t)->kind)) {
            d_.error("E_TYPE", e->loc, std::format("{} must be an integer, not {}", what, tname(t)));
            return false;
        }
        Value v;
        if (!evaluateConstant(*e, v)) {
            d_.error("E_TYPE", e->loc, std::format("{} must be a constant", what),
                     "use a literal or a VAR CONSTANT");
            return false;
        }
        if (isUnsigned(v.kind)) {
            if (v.u > static_cast<uint64_t>(INT64_MAX)) {
                d_.error("E_LITERAL_RANGE", e->loc, std::format("{} is too large", what));
                return false;
            }
            out = static_cast<int64_t>(v.u);
        } else {
            out = v.i;
        }
        return true;
    }

    // ───────────── POU declarations and layout ─────────────

    /// Declares a POU's variables and computes its layout (once). False on a cycle of instances.
    bool declare(PouInfo& pou, SourceLoc use) {
        if (pou.checked) return pou.instanceType != nullptr || pou.kind == PouKind::Function;
        if (declaring_.count(&pou)) {
            d_.error("E_RECURSION", use,
                     std::format("function block '{}' contains an instance of itself", pou.spelled),
                     "an instance cannot contain itself, directly or through other blocks");
            return false;
        }
        declaring_.insert(&pou);
        Pou& a = *pou.ast;
        DeclState& state = declStates_[&pou];
        // A VAR CONSTANT may come below its use (TIA Portal keeps a block's constants last, CODESYS takes
        // either order): an array bound or initial value that names it declares it there
        for (VarSection& sec : a.sections) {
            if (sec.kind != VarKind::Var || !sec.constant) continue;
            for (VarDecl& decl : sec.decls) {
                for (const DeclName& n : decl.names) state.constants.emplace(n.upper, std::pair{&sec, &decl});
            }
        }

        if (pou.kind == PouKind::Function) {
            pou.result = p_.types.voidType();
            if (a.returnType) {
                const Type* rt = buildType(*a.returnType, &pou, pou.spelled);
                if (rt && rt->kind == TypeKind::FunctionBlock) {
                    d_.error("E_TYPE", a.returnType->loc, "a function cannot return a function block instance");
                    rt = nullptr;
                }
                if (rt) pou.result = rt;
                else failed_.insert({&pou, pou.upper});  // reported; its calls and result assignments stay quiet
            }
            if (pou.result->kind != TypeKind::Void) {
                Symbol& r = pou.symbols.emplace_back();
                r.upper = pou.upper;
                r.spelled = pou.spelled;
                r.loc = a.loc;
                r.kind = SymKind::Result;
                r.type = pou.result;
                r.storage = Storage::Frame;
                r.order = state.order++;
                pou.byName[r.upper] = &r;
                pou.resultSymbol = &r;
            }
        }

        for (VarSection& sec : a.sections) {
            SymKind kind = SymKind::Var;
            switch (sec.kind) {
                case VarKind::Var: kind = SymKind::Var; break;
                case VarKind::Input: kind = SymKind::Input; break;
                case VarKind::Output: kind = SymKind::Output; break;
                case VarKind::InOut: kind = SymKind::InOut; break;
                case VarKind::Temp: kind = SymKind::Temp; break;
                case VarKind::External: kind = SymKind::External; break;
            }
            if (pou.kind == PouKind::Program &&
                (kind == SymKind::Input || kind == SymKind::Output || kind == SymKind::InOut)) {
                d_.error("E_NOT_SUPPORTED", sec.loc,
                         std::format("{} in a PROGRAM: SimplePLC programs exchange data through tags",
                                     varKindKeyword(sec.kind)),
                         "declare VAR_EXTERNAL name : type; END_VAR to read and write the tag 'name'");
                continue;
            }
            if (pou.kind == PouKind::Function && kind == SymKind::External) {
                d_.error("E_NOT_SUPPORTED", sec.loc, "VAR_EXTERNAL in a FUNCTION: functions have no state",
                         "pass the value as an input");
                continue;
            }
            if (sec.constant && kind != SymKind::Var && kind != SymKind::External) {
                d_.error("E_SYNTAX", sec.loc, std::format("CONSTANT is not allowed on {}", varKindKeyword(sec.kind)));
            }
            if (sec.retain && (pou.kind == PouKind::Function || kind == SymKind::Temp || kind == SymKind::InOut ||
                               kind == SymKind::External)) {
                d_.error("E_SYNTAX", sec.loc, std::format("RETAIN is not allowed on {} here", varKindKeyword(sec.kind)));
            }
            if (sec.constant && sec.retain) {
                d_.error("E_SYNTAX", sec.loc, "a variable cannot be both CONSTANT and RETAIN");
            }
            for (VarDecl& decl : sec.decls) {
                if (state.done.count(&decl)) continue;  // a constant that a declaration above needed
                state.inProgress.insert(&decl);
                declareNames(pou, sec, decl, kind, state.order);
                state.inProgress.erase(&decl);
                state.done.insert(&decl);
            }
        }
        layout(pou);
        pou.checked = true;
        declaring_.erase(&pou);
        declStates_.erase(&pou);
        return true;
    }

    /// Declares the names of one declaration of `pou` in section `sec` (symbols of `kind`).
    void declareNames(PouInfo& pou, VarSection& sec, VarDecl& decl, SymKind kind, int& order) {
        const Type* t = decl.type ? buildType(*decl.type, &pou, decl.names.front().spelled) : nullptr;
        if (t && pou.kind == PouKind::Function && holdsInstance(t)) {
            d_.error("E_FB_IN_FUNCTION", decl.type->loc,
                     std::format("a FUNCTION cannot hold function block instances ({}): functions have no state",
                                 t->name),
                     "make it a FUNCTION_BLOCK, or declare the instances in the caller");
            t = nullptr;
        }
        if (t && t->kind == TypeKind::FunctionBlock) {
            if (pou.kind == PouKind::Function) {
                d_.error("E_FB_IN_FUNCTION", decl.type->loc,
                         std::format("a FUNCTION cannot hold an instance of '{}': functions have no state",
                                     t->name),
                         "make it a FUNCTION_BLOCK, or declare the instance in the caller");
                t = nullptr;
            } else if (kind != SymKind::Var) {
                d_.error("E_NOT_SUPPORTED", decl.type->loc,
                         std::format("a function block instance in {}", varKindKeyword(sec.kind)),
                         "declare instances in VAR");
                t = nullptr;
            }
        }
        if (t && t->kind == TypeKind::Array && baseOf(t->element)->kind == TypeKind::FunctionBlock &&
            kind != SymKind::Var) {
            d_.error("E_NOT_SUPPORTED", decl.type->loc, "arrays of function block instances outside VAR");
            t = nullptr;
        }
        if (t && kind == SymKind::External && !isElementary(baseOf(t)->kind)) {
            d_.error("E_BINDING", decl.type->loc,
                     std::format("VAR_EXTERNAL binds a tag; {} is not a tag type", t->name));
            t = nullptr;
        }
        for (DeclName& n : decl.names) {
            // The standard library's own names (IN, PT, Q, S …) are local to its blocks: the
            // program's types, enumeration values and constants may use the same names
            const bool user = !pou.library;
            if (user && reserved(n.upper)) {
                d_.error("E_RESERVED", n.loc,
                         std::format("'{}' is the name of a standard type or function", n.spelled),
                         "choose another name");
                failed_.insert({&pou, n.upper});
                continue;
            } else if (user && p_.typeByName.count(n.upper)) {
                // A variable may have the name of a function or function block (the variable is meant
                // inside its block: a call has parentheses, an instance's type follows ':'), so a library
                // that adds a block named like a variable breaks nothing; a type name in a declaration could be either
                d_.error("E_DUPLICATE", n.loc, std::format("'{}' is the name of a type", n.spelled),
                         "choose another name");
                failed_.insert({&pou, n.upper});
                continue;
            }
            if (pou.byName.count(n.upper)) {
                d_.error("E_DUPLICATE", n.loc, std::format("'{}' is declared twice", n.spelled));
                continue;
            }
            if (user && globals_.find(n.upper)) {
                d_.error("E_DUPLICATE", n.loc, std::format("'{}' is a global constant", n.spelled),
                         "choose another name, so the two cannot be confused");
                failed_.insert({&pou, n.upper});
                continue;
            }
            if (const Type* et = user ? enumWithValue(n.upper) : nullptr) {
                d_.error("E_DUPLICATE", n.loc, std::format("'{}' is a value of the enumeration {}", n.spelled, et->name),
                         "choose another name, so the two cannot be confused");
                failed_.insert({&pou, n.upper});
                continue;
            }
            if (!t) {
                failed_.insert({&pou, n.upper});  // reported where it is declared; its uses stay quiet
                continue;
            }
            Symbol& s = pou.symbols.emplace_back();
            s.upper = n.upper;
            s.spelled = n.spelled;
            s.loc = n.loc;
            s.kind = kind;
            s.constant = sec.constant;
            s.retain = sec.retain;
            s.type = t;
            s.init = decl.init.get();
            s.decl = &decl;
            s.order = order++;
            if (kind == SymKind::External) {
                s.storage = Storage::Global;
                s.tag = n.spelled;
            }
            if (kind == SymKind::External && decl.init) {
                d_.error("E_SYNTAX", decl.init->loc, "VAR_EXTERNAL cannot have an initial value",
                         "the tag holds the value");
            }
            pou.byName[s.upper] = &s;
            if (kind == SymKind::Input) pou.inputs.push_back(&s);
            if (kind == SymKind::Output) pou.outputs.push_back(&s);
            if (kind == SymKind::InOut) pou.inOuts.push_back(&s);
            if (s.init && kind == SymKind::InOut) {
                d_.error("E_SYNTAX", s.init->loc, "VAR_IN_OUT cannot have an initial value");
                s.init = nullptr;
            }
            if (s.init) {
                Ctx ctx;
                ctx.pou = &pou;
                ctx.initializer = true;
                checkInitializer(*s.init, s.type, ctx);
                if (s.constant && kind == SymKind::Var && s.init->kind == Initializer::Kind::Expr && s.init->expr &&
                    evaluateConstant(*s.init->expr, s.constValue)) {
                    s.hasConstValue = true;
                }
            } else if (s.constant && kind == SymKind::Var && isElementary(baseOf(t)->kind)) {
                // VAR CONSTANT only: a VAR_EXTERNAL CONSTANT is a read-only view of a live tag, never a fixed value
                s.hasConstValue = true;
                s.constValue = defaultValue(t);
            }
        }
    }

    /// A function block instance, or an array (of arrays) of them.
    static bool holdsInstance(const Type* t) {
        const Type* b = baseOf(t);
        if (b->kind == TypeKind::FunctionBlock) return true;
        return b->kind == TypeKind::Array && b->element && holdsInstance(b->element);
    }

    const Type* enumWithValue(const std::string& upper) const {
        for (const auto& [name, t] : p_.typeByName) {
            if (t->kind == TypeKind::Enum && t->enumValue(upper)) return t;
        }
        return nullptr;
    }

    /// VAR_GLOBAL CONSTANT blocks of all files: compile-time constants visible in every POU.
    void declareGlobalConstants() {
        globals_.upper = "GLOBAL";
        globals_.spelled = "VAR_GLOBAL CONSTANT";
        for (File& f : p_.files) {
            for (VarSection& sec : f.constants) {
                for (VarDecl& decl : sec.decls) {
                    const Type* t = decl.type ? buildType(*decl.type, &globals_, decl.names.front().spelled) : nullptr;
                    if (!t) continue;
                    if (!isElementary(baseOf(t)->kind) && baseOf(t)->kind != TypeKind::String &&
                        baseOf(t)->kind != TypeKind::Enum) {
                        d_.error("E_NOT_SUPPORTED", decl.type->loc,
                                 std::format("a global constant of type {}: only elementary types, STRING and enumerations", t->name));
                        continue;
                    }
                    for (DeclName& n : decl.names) {
                        // Enumeration values too: the bare name would otherwise mean either (whichever is declared first)
                        if (reserved(n.upper) || p_.findPou(n.upper) || p_.typeByName.count(n.upper) || globals_.find(n.upper) ||
                            enumWithValue(n.upper)) {
                            d_.error("E_DUPLICATE", n.loc, std::format("'{}' is already the name of something else", n.spelled));
                            continue;
                        }
                        Symbol& s = globals_.symbols.emplace_back();
                        s.upper = n.upper;
                        s.spelled = n.spelled;
                        s.loc = n.loc;
                        s.constant = true;
                        s.type = t;
                        s.init = decl.init.get();
                        globals_.byName[s.upper] = &s;
                        if (s.init) {
                            Ctx ctx;
                            ctx.pou = &globals_;
                            ctx.initializer = true;
                            checkInitializer(*s.init, t, ctx);
                            if (s.init->kind == Initializer::Kind::Expr && s.init->expr &&
                                evaluateConstant(*s.init->expr, s.constValue)) {
                                s.hasConstValue = true;
                            }
                        } else {
                            s.hasConstValue = true;
                            s.constValue = defaultValue(t);
                        }
                    }
                }
            }
        }
    }

    Value defaultValue(const Type* t) const {
        t = baseOf(t);
        TypeKind k = t->kind;
        if (k == TypeKind::Bool) return Value::ofBool(false);
        if (isReal(k)) return Value::ofReal(k, 0);
        if (isUnsigned(k) || isBitString(k)) return Value::ofUnsigned(k, 0);
        if (k == TypeKind::String) return Value::ofString("");
        return Value::ofSigned(k, 0);
    }

    void layout(PouInfo& pou) {
        uint32_t self = 0, frame = 0, selfAlign = 8;
        auto place = [&](Symbol& s, uint32_t& cursor) {
            uint32_t size = s.kind == SymKind::InOut ? 4 : s.type->size;
            uint32_t align = s.kind == SymKind::InOut ? 4 : std::max<uint32_t>(s.type->align, 1);
            cursor = alignUp(cursor, align);
            s.offset = cursor;
            cursor += size;
            selfAlign = std::max(selfAlign, align);
        };
        if (pou.kind == PouKind::Function) {
            for (Symbol& s : pou.symbols) {
                s.storage = Storage::Frame;
                place(s, frame);
            }
        } else {
            Type* inst = p_.types.newType();
            inst->kind = TypeKind::FunctionBlock;
            inst->name = pou.spelled;
            inst->pou = &pou;
            for (Symbol& s : pou.symbols) {
                if (s.kind == SymKind::External) continue;
                if (s.kind == SymKind::Temp) {
                    s.storage = Storage::Frame;
                    place(s, frame);
                    continue;
                }
                s.storage = Storage::Self;
                place(s, self);
                if (s.kind != SymKind::InOut) inst->fields.push_back({s.upper, s.spelled, s.type, s.offset, s.init});
            }
            inst->align = selfAlign;
            inst->size = alignUp(std::max<uint32_t>(self, 1), selfAlign);
            if (inst->size > kMaxDataBytes) {
                d_.error("E_LIMIT", pou.ast->loc, std::format("'{}' needs {} bytes; the limit is 16 MB", pou.spelled, inst->size));
            }
            pou.instanceType = inst;
            pou.instanceSize = inst->size;
        }
        pou.frameSize = alignUp(frame, 8);
    }

    // ───────────── Tags ─────────────

    void bindTags() {
        std::map<std::string, size_t> byUpper;  // tag name upper → index in bindings
        uint32_t offset = 0;
        for (PouInfo& pou : p_.pous) {
            for (Symbol& s : pou.symbols) {
                if (s.kind != SymKind::External) continue;
                const TagDef* tag = nullptr;
                int matches = 0;
                for (const TagDef& t : tags_) {
                    if (toUpper(t.name) == s.upper) {
                        tag = &t;
                        matches++;
                    }
                }
                if (matches == 0) {
                    std::string hint;
                    size_t best = 3;
                    for (const TagDef& t : tags_) {
                        size_t dist = editDistance(s.upper, toUpper(t.name));
                        if (dist < best) { best = dist; hint = std::format("did you mean the tag '{}'?", t.name); }
                    }
                    if (hint.empty()) hint = "add the tag to the PLC's tag table, or correct the name";
                    d_.error("E_BINDING", s.loc, std::format("there is no tag named '{}'", s.spelled), hint);
                    continue;
                }
                if (matches > 1) {
                    d_.error("E_BINDING", s.loc,
                             std::format("several tags are named '{}' when case is ignored", s.spelled),
                             "rename one of the tags");
                    continue;
                }
                const Type* expected = nullptr;
                std::string allowed;
                switch (tag->type) {
                    case TagType::Bool: allowed = "BOOL"; break;
                    case TagType::UInt16: allowed = "UINT or WORD"; break;
                    case TagType::Int16: allowed = "INT"; break;
                    case TagType::UInt32: allowed = "UDINT or DWORD"; break;
                    case TagType::Int32: allowed = "DINT"; break;
                    case TagType::Float32: allowed = "REAL"; break;
                }
                TypeKind k = baseOf(s.type)->kind;
                bool ok = (tag->type == TagType::Bool && k == TypeKind::Bool) ||
                          (tag->type == TagType::UInt16 && (k == TypeKind::UInt || k == TypeKind::Word)) ||
                          (tag->type == TagType::Int16 && k == TypeKind::Int) ||
                          (tag->type == TagType::UInt32 && (k == TypeKind::UDInt || k == TypeKind::DWord)) ||
                          (tag->type == TagType::Int32 && k == TypeKind::DInt) ||
                          (tag->type == TagType::Float32 && k == TypeKind::Real);
                if (!ok || s.type->hasRange) {
                    d_.error("E_BINDING", s.loc,
                             std::format("tag '{}' is {}; declare it as {}", tag->name, tagTypeName(tag->type), allowed));
                    continue;
                }
                s.tag = tag->name;
                auto [it, fresh] = byUpper.emplace(s.upper, p_.bindings.size());
                if (fresh) {
                    TagBinding b;
                    b.tag = tag->name;
                    b.tagType = tag->type;
                    b.type = s.type;
                    offset = alignUp(offset, s.type->align);
                    b.offset = offset;
                    offset += s.type->size;
                    p_.bindings.push_back(b);
                    expected = s.type;
                } else {
                    expected = p_.bindings[it->second].type;
                    if (!sameType(expected, s.type)) {
                        d_.error("E_BINDING", s.loc,
                                 std::format("tag '{}' is declared as {} elsewhere in this program; use the same type",
                                             tag->name, expected->name));
                        continue;
                    }
                }
                TagBinding& b = p_.bindings[it->second];
                s.offset = b.offset;
                if (!s.constant) b.written = true;
            }
        }
        p_.globalsSize = alignUp(offset, 8);
    }

    // ───────────── Bodies ─────────────

    void checkBody(PouInfo& pou) {
        if (!pou.checked) return;
        Ctx ctx;
        ctx.pou = &pou;
        checkStmts(pou.ast->body, ctx);
        if (!pou.library && !d_.hasErrors()) {
            // Parameters may go unused (an interface); local variables and tags that nothing names are a slip
            for (const Symbol& s : pou.symbols) {
                if (s.used || (s.kind != SymKind::Var && s.kind != SymKind::Temp && s.kind != SymKind::External)) continue;
                d_.warning("W_UNUSED", s.loc,
                           std::format("{} '{}' is declared but not used", s.kind == SymKind::External ? "tag" : "variable", s.spelled),
                           s.kind == SymKind::External ? "remove it from VAR_EXTERNAL, or use it" : "remove it, or use it");
            }
        }
        if (pou.resultSymbol && !pou.library) {
            // IEC 61131-3: returning without having assigned the result is an error
            Paths body = resultFlow(pou.ast->body, Flow{true, false}, pou.resultSymbol, pou);
            if (body.fall.reachable && !body.fall.assigned) {
                d_.error("E_RESULT_UNASSIGNED", pou.ast->endLoc,
                         std::format("function '{}' can reach its end without assigning its result", pou.spelled),
                         std::format("assign {} := … on every path, e.g. at the top of the function", pou.spelled));
            }
        }
    }

    /// Whether the result is assigned on every path reaching a point; `reachable` false: no path does.
    struct Flow {
        bool reachable = false;
        bool assigned = true;
    };
    static Flow merge(Flow a, Flow b) {
        if (!a.reachable) return b;
        if (!b.reachable) return a;
        return {true, a.assigned && b.assigned};
    }
    /// The states where a statement list ends: falling off its end, at its EXITs, at its CONTINUEs.
    struct Paths {
        Flow fall, exits, continues;
    };

    /// Follows every path through `list` from state `in` and reports RETURNs reached before `r` is
    /// assigned. A REPEAT body runs at least once; FOR and WHILE bodies may not run at all; EXIT
    /// leaves the innermost loop; CONTINUE goes to the loop's test, which may end the loop.
    Paths resultFlow(const StmtList& list, Flow in, Symbol* r, PouInfo& pou) {
        Paths p{in, {}, {}};
        // `complete`: one of the branches always runs (an ELSE, or every value of an enumeration covered)
        auto branches = [&](auto&& bodies, bool hasElse, const StmtList* elseBody, bool complete = false) {
            Flow out = hasElse || complete ? Flow{} : p.fall;
            auto take = [&](const StmtList& body) {
                Paths b = resultFlow(body, p.fall, r, pou);
                out = merge(out, b.fall);
                p.exits = merge(p.exits, b.exits);
                p.continues = merge(p.continues, b.continues);
            };
            for (const auto& b : bodies) take(b.body);
            if (hasElse) take(*elseBody);
            p.fall = out;
        };
        for (const StmtPtr& sp : list) {
            if (!sp || !p.fall.reachable) continue;
            const Stmt& s = *sp;
            switch (s.kind) {
                case StmtKind::Assign:
                    if (rootOf(*s.as<AssignStmt>().target) == r) p.fall.assigned = true;
                    break;
                case StmtKind::Call: {
                    const auto& c = s.as<CallStmt>().call->as<Call>();
                    for (size_t i = 0; i < c.args.size(); i++) {
                        bool writes = c.args[i].output ||
                                      (i < c.argParam.size() && c.argParam[i] && c.argParam[i]->kind == SymKind::InOut);
                        if (writes && c.args[i].value && rootOf(*c.args[i].value) == r) p.fall.assigned = true;
                    }
                    break;
                }
                case StmtKind::If: {
                    const auto& i = s.as<IfStmt>();
                    branches(i.branches, i.hasElse, &i.elseBody);
                    break;
                }
                case StmtKind::Case: {
                    const auto& c = s.as<CaseStmt>();
                    branches(c.branches, c.hasElse, &c.elseBody, c.complete);
                    break;
                }
                case StmtKind::For:
                case StmtKind::While: {
                    const StmtList& body = s.kind == StmtKind::For ? s.as<ForStmt>().body : s.as<WhileStmt>().body;
                    Paths b = resultFlow(body, p.fall, r, pou);
                    // After the loop: no iteration at all, or the test failed after the body or a CONTINUE, or an EXIT
                    p.fall = merge(p.fall, merge(b.fall, merge(b.continues, b.exits)));
                    break;
                }
                case StmtKind::Repeat: {
                    // The first iteration always runs; later ones only add assignments
                    Paths b = resultFlow(s.as<RepeatStmt>().body, p.fall, r, pou);
                    p.fall = merge(b.fall, merge(b.continues, b.exits));
                    break;
                }
                case StmtKind::Return:
                    if (!p.fall.assigned) {
                        d_.error("E_RESULT_UNASSIGNED", s.loc,
                                 std::format("RETURN before the result of '{}' is assigned", pou.spelled),
                                 std::format("assign {} := … before returning", pou.spelled));
                    }
                    p.fall = Flow{};
                    break;
                case StmtKind::Exit:
                    p.exits = merge(p.exits, p.fall);
                    p.fall = Flow{};
                    break;
                case StmtKind::Continue:
                    p.continues = merge(p.continues, p.fall);
                    p.fall = Flow{};
                    break;
                case StmtKind::Empty:
                    break;
            }
        }
        return p;
    }

    void checkRecursion() {
        std::map<PouInfo*, int> state;
        std::vector<PouInfo*> path;
        std::function<void(PouInfo*)> visit = [&](PouInfo* p) {
            state[p] = 1;
            path.push_back(p);
            for (PouInfo* c : p->callees) {
                if (state[c] == 1) {
                    std::string cycle;
                    bool in = false;
                    for (PouInfo* q : path) {
                        if (q == c) in = true;
                        if (in) cycle += q->spelled + " → ";
                    }
                    cycle += c->spelled;
                    auto loc = callLoc_.find({p, c});
                    d_.error("E_RECURSION", loc != callLoc_.end() ? loc->second : p->ast->loc,
                             std::format("recursive call: {}", cycle),
                             "IEC 61131-3 does not allow recursion; rewrite it as a loop");
                } else if (state[c] == 0) {
                    visit(c);
                }
            }
            path.pop_back();
            state[p] = 2;
        };
        for (PouInfo& p : p_.pous) {
            if (state[&p] == 0) visit(&p);
        }
    }

    void checkStmts(StmtList& list, Ctx& ctx) {
        for (StmtPtr& s : list) {
            if (s) checkStmt(*s, ctx);
            if (d_.full()) return;
        }
    }

    void checkStmt(Stmt& s, Ctx& ctx) {
        switch (s.kind) {
            case StmtKind::Assign: {
                auto& a = s.as<AssignStmt>();
                if (a.target->kind == ExprKind::Member) ctx.memberTarget = &a.target->as<Member>();
                const Type* target = typeExpr(a.target, nullptr, ctx);
                ctx.memberTarget = nullptr;
                if (!target) {
                    if (a.value) typeExpr(a.value, nullptr, ctx);
                    return;
                }
                if (!checkWritable(*a.target, ctx)) {
                    typeExpr(a.value, target, ctx);
                    return;
                }
                if (baseOf(target)->kind == TypeKind::FunctionBlock) {
                    d_.error("E_TYPE", a.target->loc, "function block instances cannot be assigned");
                    return;
                }
                if (!typeExpr(a.value, target, ctx)) return;
                coerce(a.value, target, "assign");
                return;
            }
            case StmtKind::Call: {
                auto& c = s.as<CallStmt>();
                if (c.call->kind != ExprKind::Call) return;
                const Call& call = c.call->as<Call>();
                if (call.callee->kind == ExprKind::Name) {
                    // Standard, conversion and operator functions only compute a value (they have no outputs)
                    const auto& n = call.callee->as<Name>();
                    bool own = (ctx.pou && ctx.pou->find(n.upper)) || p_.findPou(n.upper);
                    if (!own && (findStd(n.upper) || isOperatorFunction(n.upper) || isConversionName(n.upper))) {
                        d_.error("E_CALL", call.loc, std::format("{} computes a value that is not used", n.spelled),
                                 std::format("assign it, e.g. x := {}(…);", n.spelled));
                        return;
                    }
                }
                typeCall(c.call, ctx, true);
                return;
            }
            case StmtKind::If: {
                auto& i = s.as<IfStmt>();
                for (IfBranch& b : i.branches) {
                    condition(b.condition, ctx, "IF");
                    checkStmts(b.body, ctx);
                }
                checkStmts(i.elseBody, ctx);
                return;
            }
            case StmtKind::Case: checkCase(s.as<CaseStmt>(), ctx); return;
            case StmtKind::For: checkFor(s.as<ForStmt>(), ctx); return;
            case StmtKind::While: {
                auto& w = s.as<WhileStmt>();
                condition(w.condition, ctx, "WHILE");
                ctx.loopDepth++;
                checkStmts(w.body, ctx);
                ctx.loopDepth--;
                return;
            }
            case StmtKind::Repeat: {
                auto& r = s.as<RepeatStmt>();
                ctx.loopDepth++;
                checkStmts(r.body, ctx);
                ctx.loopDepth--;
                condition(r.condition, ctx, "UNTIL");
                return;
            }
            case StmtKind::Exit:
            case StmtKind::Continue:
                if (ctx.loopDepth == 0) {
                    d_.error("E_EXIT_OUTSIDE_LOOP", s.loc,
                             std::format("{} is only allowed inside FOR, WHILE or REPEAT",
                                         s.kind == StmtKind::Exit ? "EXIT" : "CONTINUE"));
                }
                return;
            case StmtKind::Return:
            case StmtKind::Empty:
                return;
        }
    }

    void condition(ExprPtr& e, Ctx& ctx, std::string_view what) {
        if (!e) return;
        const Type* t = typeExpr(e, p_.types.elementary(TypeKind::Bool), ctx);
        if (t && baseOf(t)->kind != TypeKind::Bool) {
            d_.error("E_TYPE", e->loc, std::format("the {} condition must be BOOL, not {}", what, tname(t)),
                     isInteger(baseOf(t)->kind) ? "compare it, e.g. x <> 0" : "");
        }
    }

    void checkCase(CaseStmt& c, Ctx& ctx) {
        const Type* sel = c.selector ? typeExpr(c.selector, nullptr, ctx) : nullptr;
        TypeKind k = sel ? baseOf(sel)->kind : TypeKind::Void;
        if (sel && !isInteger(k) && !isBitString(k) && k != TypeKind::Enum) {
            d_.error("E_TYPE", c.selector->loc,
                     std::format("the CASE selector must be an integer, a bit string or an enumeration, not {}", tname(sel)));
            sel = nullptr;
        }
        struct Range {
            LabelKey lo, hi;
            SourceLoc loc;
        };
        std::vector<Range> ranges;
        for (CaseBranch& b : c.branches) {
            for (CaseLabel& lab : b.labels) {
                if (!sel) {
                    if (lab.low) typeExpr(lab.low, nullptr, ctx);
                    if (lab.high) typeExpr(lab.high, nullptr, ctx);
                    continue;
                }
                LabelKey lo = 0, hi = 0;
                if (!caseLabel(lab.low, sel, ctx, lo)) continue;
                hi = lo;
                if (lab.high) {
                    if (k == TypeKind::Enum) {
                        d_.error("E_TYPE", lab.loc, "ranges are not allowed for enumeration labels");
                        continue;
                    }
                    if (!caseLabel(lab.high, sel, ctx, hi)) continue;
                    if (hi < lo) {
                        d_.error("E_TYPE", lab.loc, "the range's low bound is above its high bound");
                        continue;
                    }
                }
                for (const Range& r : ranges) {
                    if (lo <= r.hi && r.lo <= hi) {
                        d_.error("E_CASE_OVERLAP", lab.loc,
                                 std::format("this label overlaps the label on line {}", r.loc.line),
                                 "each value may appear in one label only");
                        break;
                    }
                }
                ranges.push_back({lo, hi, lab.loc});
            }
            checkStmts(b.body, ctx);
        }
        checkStmts(c.elseBody, ctx);
        if (sel && k == TypeKind::Enum && !c.hasElse) {
            std::string missing;
            for (const EnumConstant& v : baseOf(sel)->enumValues) {
                bool covered = false;
                for (const auto& r : ranges) covered = covered || (r.lo <= signedKey(v.value) && signedKey(v.value) <= r.hi);
                if (!covered) missing += (missing.empty() ? "" : ", ") + v.spelled;
            }
            c.complete = missing.empty();
            if (!missing.empty()) {
                d_.warning("W_CASE_INCOMPLETE", c.loc, std::format("CASE without ELSE does not handle {}", missing),
                           "add the missing values, or an ELSE");
            }
        }
    }

    bool caseLabel(ExprPtr& e, const Type* sel, Ctx& ctx, LabelKey& out) {
        const Type* t = typeExpr(e, sel, ctx);
        if (!t) return false;
        if (!coerce(e, sel, "use as a CASE label for")) return false;
        Value v;
        if (!evaluateConstant(*e, v)) {
            d_.error("E_TYPE", e->loc, "a CASE label must be a constant", "use a literal, an enumeration value or a CONSTANT");
            return false;
        }
        // Enumeration values are compared by their declared value, whatever the base type
        if (baseOf(sel)->kind == TypeKind::Enum) out = signedKey(v.i);
        else out = isUnsigned(v.kind) || isBitString(v.kind) ? v.u : signedKey(v.i);
        return true;
    }

    void checkFor(ForStmt& f, Ctx& ctx) {
        Symbol* ctl = nullptr;
        if (f.variable) {
            const Type* t = typeExpr(f.variable, nullptr, ctx);
            auto& n = f.variable->as<Name>();
            if (t) {
                if (!n.symbol || (n.symbol->kind != SymKind::Var && n.symbol->kind != SymKind::Temp) ||
                    n.symbol->constant) {
                    d_.error("E_FOR_VAR", f.variable->loc,
                             std::format("the FOR control variable '{}' must be a local VAR or VAR_TEMP", n.spelled));
                } else if (!isInteger(t->kind) || t->hasRange) {
                    d_.error(isInteger(t->kind) ? "E_FOR_VAR" : "E_TYPE", f.variable->loc,
                             std::format("the FOR control variable must be an integer{}, not {}",
                                         t->hasRange ? " without a subrange" : "", tname(t)));
                } else {
                    ctl = n.symbol;
                    for (const ForGuard& g : ctx.fors) {
                        if (g.control == ctl || g.bounds.count(ctl)) {
                            d_.error("E_FOR_VAR", f.variable->loc,
                                     std::format("'{}' is already used by an enclosing FOR loop", n.spelled));
                        }
                    }
                }
            }
        }
        f.control = ctl;
        const Type* ct = ctl ? ctl->type : nullptr;
        for (ExprPtr* e : {&f.start, &f.end, &f.step}) {
            if (!*e) continue;
            if (typeExpr(*e, ct, ctx) && ct) coerce(*e, ct, "use as a FOR bound of");
        }
        if (f.step && ct) {
            Value v;
            if (evaluateConstant(*f.step, v) && v.bits() == 0) {
                d_.error("E_FOR_STEP", f.step->loc, "the FOR step is 0: the loop would never end");
            }
        }
        ForGuard g;
        g.control = ctl;
        collectNames(f.end.get(), g.bounds);
        collectNames(f.step.get(), g.bounds);
        ctx.fors.push_back(std::move(g));
        ctx.loopDepth++;
        checkStmts(f.body, ctx);
        ctx.loopDepth--;
        ctx.fors.pop_back();
    }

    void collectNames(const Expr* e, std::set<Symbol*>& out) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Name:
                if (e->as<Name>().symbol) out.insert(e->as<Name>().symbol);
                return;
            case ExprKind::Member: collectNames(e->as<Member>().base.get(), out); return;
            case ExprKind::BitAccess: collectNames(e->as<BitAccess>().base.get(), out); return;
            case ExprKind::Index:
                collectNames(e->as<Index>().base.get(), out);
                for (const ExprPtr& i : e->as<Index>().indexes) collectNames(i.get(), out);
                return;
            case ExprKind::Call:
                for (const Arg& a : e->as<Call>().args) collectNames(a.value.get(), out);
                return;
            case ExprKind::Unary: collectNames(e->as<Unary>().operand.get(), out); return;
            case ExprKind::Binary:
                collectNames(e->as<Binary>().lhs.get(), out);
                collectNames(e->as<Binary>().rhs.get(), out);
                return;
            case ExprKind::Convert: collectNames(e->as<ast::Convert>().operand.get(), out); return;
            default: return;
        }
    }

    // ───────────── Writability ─────────────

    /// The variable an assignable expression writes, or null (with an error) if it cannot be written.
    Symbol* rootOf(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Name: return e.as<Name>().symbol;
            case ExprKind::Member: return rootOf(*e.as<Member>().base);
            case ExprKind::BitAccess: return rootOf(*e.as<BitAccess>().base);
            case ExprKind::Index: return rootOf(*e.as<Index>().base);
            default: return nullptr;
        }
    }

    bool checkWritable(const Expr& e, Ctx& ctx, std::string_view how = "assigned") {
        bool parenthesized = e.parenthesized;
        if (e.kind == ExprKind::Member) {
            const auto& m = e.as<Member>();
            if (m.member) {
                if (m.member->kind != SymKind::Input) {
                    d_.error(m.member->kind == SymKind::Output ? "E_OUTPUT_WRITE" : "E_ACCESS", e.loc,
                             std::format("'{}' of a function block instance cannot be {} from outside", m.spelled, how),
                             m.member->kind == SymKind::Output ? "outputs are written by the block itself" : "");
                    return false;
                }
            }
            return checkWritable(*m.base, ctx, how);
        }
        if (e.kind == ExprKind::Index) return checkWritable(*e.as<Index>().base, ctx, how);
        if (e.kind == ExprKind::BitAccess) return checkWritable(*e.as<BitAccess>().base, ctx, how);
        if (e.kind != ExprKind::Name || parenthesized) {
            d_.error("E_TYPE", e.loc, std::format("this expression cannot be {}", how), "assign to a variable");
            return false;
        }
        const auto& n = e.as<Name>();
        Symbol* s = n.symbol;
        if (!s) {
            d_.error("E_TYPE", e.loc, std::format("'{}' is a constant value and cannot be {}", n.spelled, how));
            return false;
        }
        if (s->constant) {
            d_.error("E_CONSTANT_WRITE", e.loc, std::format("'{}' is CONSTANT and cannot be {}", s->spelled, how));
            return false;
        }
        if (s->kind == SymKind::Input) {
            d_.error("E_INPUT_WRITE", e.loc,
                     std::format("'{}' is a VAR_INPUT and is read-only inside {}", s->spelled, ctx.pou->spelled),
                     "copy it into a VAR to change it");
            return false;
        }
        for (const ForGuard& g : ctx.fors) {
            if (g.control == s) {
                d_.error("E_FOR_VAR", e.loc,
                         std::format("'{}' is the FOR control variable and cannot be {} inside the loop", s->spelled, how));
                return false;
            }
            if (g.bounds.count(s)) {
                d_.error("E_FOR_VAR", e.loc,
                         std::format("'{}' is read by the FOR loop's end or step and cannot be {} inside the loop",
                                     s->spelled, how),
                         "copy it into another variable before the loop");
                return false;
            }
        }
        s->assigned = true;
        return true;
    }

    // ───────────── Initial values ─────────────

    void checkInitializer(Initializer& init, const Type* t, Ctx& ctx) {
        const Type* b = baseOf(t);
        switch (init.kind) {
            case Initializer::Kind::Expr: {
                if (!init.expr) return;
                if (b->kind == TypeKind::FunctionBlock || b->kind == TypeKind::Struct || b->kind == TypeKind::Array) {
                    d_.error("E_TYPE", init.loc,
                             std::format("{} is initialised with {}", t->name,
                                         b->kind == TypeKind::Array ? "[ … ]" : "( member := value, … )"));
                    return;
                }
                if (!typeExpr(init.expr, t, ctx)) return;
                if (!coerce(init.expr, t, "initialise")) return;
                Value v;
                if (!evaluateConstant(*init.expr, v)) {
                    d_.error("E_TYPE", init.loc, "an initial value must be a constant expression");
                    return;
                }
                // An unsigned value is in v.u (v.i is not its value)
                int64_t iv = isUnsigned(baseOf(t)->kind) ? static_cast<int64_t>(v.u) : v.i;
                if (t->hasRange && (iv < t->rangeLow || iv > t->rangeHigh)) {
                    d_.error("E_LITERAL_RANGE", init.loc, std::format("{} is outside {}", v.text(), t->name));
                }
                return;
            }
            case Initializer::Kind::Array: {
                if (b->kind != TypeKind::Array) {
                    d_.error("E_TYPE", init.loc, std::format("[ … ] initialises an array, but this is {}", t->name));
                    return;
                }
                uint64_t total = 1;
                for (const ArrayDim& d : b->dims) total *= d.count();
                uint64_t given = 0;
                for (ArrayInitElement& el : init.elements) {
                    uint64_t n = 1;
                    if (el.count) {
                        const auto& lit = el.count->as<IntLit>();
                        n = lit.value;
                    }
                    given += n;
                    if (el.value) checkInitializer(*el.value, b->element, ctx);
                }
                if (given > total) {
                    d_.error("E_INDEX", init.loc,
                             std::format("{} initial values for an array of {} elements", given, total));
                }
                return;
            }
            case Initializer::Kind::Struct: {
                if (b->kind != TypeKind::Struct && b->kind != TypeKind::FunctionBlock) {
                    d_.error("E_TYPE", init.loc, std::format("( … ) initialises a structure, but this is {}", t->name));
                    return;
                }
                std::set<std::string> seen;
                for (StructInitField& f : init.fields) {
                    if (!seen.insert(f.upper).second) {
                        d_.error("E_DUPLICATE", f.loc, std::format("'{}' is initialised twice", f.spelled));
                        continue;
                    }
                    const Field* field = b->field(f.upper);
                    if (b->kind == TypeKind::FunctionBlock) {
                        Symbol* s = b->pou->find(f.upper);
                        if (!s || s->kind != SymKind::Input) {
                            d_.error("E_UNDECLARED", f.loc,
                                     std::format("'{}' is not an input of {}", f.spelled, b->name),
                                     "only inputs of a function block instance can be initialised");
                            continue;
                        }
                    }
                    if (!field) {
                        d_.error("E_UNDECLARED", f.loc, std::format("{} has no member '{}'", t->name, f.spelled));
                        continue;
                    }
                    if (f.value) checkInitializer(*f.value, field->type, ctx);
                }
                return;
            }
        }
    }

    // ───────────── Expressions ─────────────

    /// Untyped constant of literals only: takes its type from where it is used.
    static bool isFlexible(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:
            case ExprKind::RealLit:
                return true;
            case ExprKind::Unary: {
                const auto& u = e.as<Unary>();
                return u.op != UnaryOp::Not && isFlexible(*u.operand);
            }
            case ExprKind::Binary: {
                const auto& b = e.as<Binary>();
                return isArithmetic(b.op) && isFlexible(*b.lhs) && isFlexible(*b.rhs);
            }
            default:
                return false;
        }
    }

    /// An expression of untyped literals only: literals, and operators and standard functions applied to them.
    bool literalOnly(const Expr& e) const {
        switch (e.kind) {
            case ExprKind::IntLit:
            case ExprKind::RealLit: return true;
            case ExprKind::Unary: return literalOnly(*e.as<Unary>().operand);
            case ExprKind::Binary: return literalOnly(*e.as<Binary>().lhs) && literalOnly(*e.as<Binary>().rhs);
            case ExprKind::Call: {
                const Call& c = e.as<Call>();
                if (c.callee->kind != ExprKind::Name || !findStd(c.callee->as<Name>().upper) || c.args.empty()) return false;
                for (const Arg& a : c.args) {
                    if (a.output || !a.value || !literalOnly(*a.value)) return false;
                }
                return true;
            }
            default: return false;
        }
    }

    static bool hasRealLiteral(const Expr& e) {
        switch (e.kind) {
            case ExprKind::RealLit: return true;
            case ExprKind::Unary: return hasRealLiteral(*e.as<Unary>().operand);
            case ExprKind::Binary: return hasRealLiteral(*e.as<Binary>().lhs) || hasRealLiteral(*e.as<Binary>().rhs);
            default: return false;
        }
    }
    static bool hasOperator(const Expr& e, BinaryOp op) {
        if (e.kind == ExprKind::Unary) return hasOperator(*e.as<Unary>().operand, op);
        if (e.kind != ExprKind::Binary) return false;
        const auto& b = e.as<Binary>();
        return b.op == op || hasOperator(*b.lhs, op) || hasOperator(*b.rhs, op);
    }
    static bool fitsIn(const Type* t, bool negative, uint64_t magnitude) {
        TypeKind k = t->kind;
        if (isReal(k)) {
            // Exact when its significant bits fit the mantissa: 16777218 (2^24 + 2) is a REAL, 16777217 is not
            if (magnitude == 0) return true;
            uint64_t significant = magnitude >> std::countr_zero(magnitude);
            return significant < (uint64_t{1} << (k == TypeKind::Real ? 24 : 53));
        }
        if (negative) return isSigned(k) && magnitude <= static_cast<uint64_t>(-(minValue(k) + 1)) + 1;
        return magnitude <= maxValue(k);
    }

    /**
     * Evaluates a constant of literals in working type `w` with the run-time rules:
     * integer literals take `w`, integer '/' truncates, REAL arithmetic is
     * done in REAL. A literal that does not fit `w` and any fault are compile errors.
     */
    bool evalLiterals(const Expr& e, const Type* w, Value& out) {
        TypeKind k = w->kind;
        switch (e.kind) {
            case ExprKind::IntLit:
            case ExprKind::Unary: {
                bool neg = false;
                const Expr* x = &e;
                if (e.kind == ExprKind::Unary) {
                    const auto& u = e.as<Unary>();
                    if (u.operand->kind == ExprKind::IntLit || u.operand->kind == ExprKind::RealLit) {
                        neg = u.op == UnaryOp::Neg;
                        x = u.operand.get();
                    } else {
                        Value v;
                        if (!evalLiterals(*u.operand, w, v)) return false;
                        if (u.op == UnaryOp::Plus) { out = v; return true; }
                        if (isReal(k)) { out = Value::ofReal(k, -v.r); return true; }
                        int64_t r = 0;
                        if (!isSigned(k) || vm::signedOp('-', k, 0, v.i, r) != vm::Fault::None) {
                            d_.error(isSigned(k) ? "E_OVERFLOW" : "E_TYPE", e.loc,
                                     isSigned(k) ? std::format("the negation overflows {}", w->name)
                                                 : std::format("{} has no negative values", w->name));
                            return false;
                        }
                        out = Value::ofSigned(k, r);
                        return true;
                    }
                }
                if (x->kind == ExprKind::RealLit) {
                    if (!isReal(k)) {
                        d_.error("E_TYPE", x->loc, std::format("a real number cannot be used as {}", w->name),
                                 isInteger(k) ? std::format("convert it explicitly, e.g. LREAL_TO_{}(…)", w->name) : "");
                        return false;
                    }
                    double r = x->as<RealLit>().value;
                    if (neg) r = -r;
                    if (k == TypeKind::Real && std::fabs(r) > 3.4028234663852886e38) {
                        d_.error("E_LITERAL_RANGE", x->loc, std::format("{} is outside the range of REAL", r));
                        return false;
                    }
                    out = Value::ofReal(k, r);
                    return true;
                }
                uint64_t m = x->as<IntLit>().value;
                if (!fitsIn(w, neg, m)) {
                    std::string text = std::format("{}{}", neg ? "-" : "", m);
                    if (isReal(k)) {
                        d_.error("E_LITERAL_RANGE", x->loc, std::format("{} cannot be represented exactly as {}", text, w->name),
                                 std::format("write it as a real literal, e.g. {}.0", text));
                    } else if (neg && !isSigned(k)) {
                        d_.error("E_LITERAL_RANGE", x->loc, std::format("{} does not fit {}: it has no negative values", text, w->name));
                    } else {
                        d_.error("E_LITERAL_RANGE", x->loc, std::format("{} does not fit {} ({}…{})", text, w->name,
                                                                        isSigned(k) ? minValue(k) : 0, maxValue(k)));
                    }
                    return false;
                }
                if (isReal(k)) {
                    double d = static_cast<double>(m);
                    out = Value::ofReal(k, neg ? -d : d);
                } else if (isSigned(k)) {
                    out = Value::ofSigned(k, neg ? static_cast<int64_t>(0 - m) : static_cast<int64_t>(m));
                } else {
                    out = Value::ofUnsigned(k, m);
                }
                return true;
            }
            case ExprKind::RealLit: {
                Value v;
                ExprPtr dummy;
                if (!isReal(k)) {
                    d_.error("E_TYPE", e.loc, std::format("a real number cannot be used as {}", w->name),
                             isInteger(k) ? std::format("convert it explicitly, e.g. LREAL_TO_{}(…)", w->name) : "");
                    return false;
                }
                double r = e.as<RealLit>().value;
                if (k == TypeKind::Real && std::fabs(r) > 3.4028234663852886e38) {
                    d_.error("E_LITERAL_RANGE", e.loc, std::format("{} is outside the range of REAL", r));
                    return false;
                }
                out = Value::ofReal(k, r);
                return true;
            }
            case ExprKind::Binary: {
                const auto& b = e.as<Binary>();
                Value l, r;
                if (!evalLiterals(*b.lhs, w, l) || !evalLiterals(*b.rhs, w, r)) return false;
                if (isBitString(k)) {
                    d_.error("E_TYPE", b.loc, std::format("{} is a bit string and has no arithmetic", w->name),
                             "compute in an integer type and convert");
                    return false;
                }
                char op = b.op == BinaryOp::Add ? '+' : b.op == BinaryOp::Sub ? '-' : b.op == BinaryOp::Mul ? '*'
                          : b.op == BinaryOp::Div ? '/' : '%';
                vm::Fault f = vm::Fault::None;
                if (op == '%' && (isReal(k) || (isUnsigned(k) ? r.u == 0 : r.i == 0))) {
                    d_.error(isReal(k) ? "E_TYPE" : "E_DIV_ZERO", b.rhs->loc,
                             isReal(k) ? "MOD needs integers" : "MOD by a constant 0 always gives 0");
                    return false;
                }
                if (k == TypeKind::Real) {
                    float z = 0;
                    f = vm::realOp(op, static_cast<float>(l.r), static_cast<float>(r.r), z);
                    out = Value::ofReal(k, z);
                } else if (k == TypeKind::LReal) {
                    double z = 0;
                    f = vm::realOp(op, l.r, r.r, z);
                    out = Value::ofReal(k, z);
                } else if (isSigned(k)) {
                    int64_t z = 0;
                    f = vm::signedOp(op, k, l.i, r.i, z);
                    out = Value::ofSigned(k, z);
                } else {
                    uint64_t z = 0;
                    f = vm::unsignedOp(op, k, l.u, r.u, z);
                    out = Value::ofUnsigned(k, z);
                }
                if (f == vm::Fault::DivZero) {
                    d_.error("E_DIV_ZERO", b.rhs->loc, "division by zero in a constant expression");
                    return false;
                }
                if (f != vm::Fault::None) {
                    d_.error("E_OVERFLOW", b.loc, std::format("the constant expression overflows {}", w->name),
                             "literals take the type of their context; widen it or use a typed literal such as LINT#…");
                    return false;
                }
                return true;
            }
            default:
                return false;
        }
    }

    static uint64_t biggestLiteral(const Expr& x) {
        if (x.kind == ExprKind::IntLit) return x.as<IntLit>().value;
        if (x.kind == ExprKind::Unary) return biggestLiteral(*x.as<Unary>().operand);
        if (x.kind == ExprKind::Binary) return std::max(biggestLiteral(*x.as<Binary>().lhs), biggestLiteral(*x.as<Binary>().rhs));
        return 0;
    }

    /// The type literals have without context: LREAL if there is a real literal, else
    /// DINT, or LINT/ULINT when a literal needs it. Two expressions get one type for both.
    const Type* literalDefault(const Expr& a, const Expr& b) const {
        if (hasRealLiteral(a) || hasRealLiteral(b)) return p_.types.elementary(TypeKind::LReal);
        uint64_t m = std::max(biggestLiteral(a), biggestLiteral(b));
        return p_.types.elementary(m <= 2147483648ULL ? TypeKind::DInt
                                   : m <= 9223372036854775808ULL ? TypeKind::LInt : TypeKind::ULInt);
    }

    /// Types an untyped constant of literals by its context.
    const Type* typeFlexible(ExprPtr& e, const Type* expected) {
        const Type* t = expected ? baseOf(expected) : nullptr;
        if (t && !isElementary(t->kind)) {
            d_.error("E_TYPE", e->loc, std::format("a number cannot be used as {}", expected->name));
            return nullptr;
        }
        if (t && (t->kind == TypeKind::Bool || isDuration(t->kind))) {
            d_.error("E_TYPE", e->loc, std::format("a number cannot be used as {}", t->name),
                     t->kind == TypeKind::Bool ? "write TRUE or FALSE" : "write a duration such as T#500ms");
            return nullptr;
        }
        bool real = hasRealLiteral(*e);
        const Type* work = nullptr;
        if (real) {
            if (t && !isReal(t->kind)) {
                d_.error("E_TYPE", e->loc, std::format("a real number cannot be used as {}", t->name),
                         isInteger(t->kind) ? std::format("convert it explicitly, e.g. LREAL_TO_{}(…)", t->name) : "");
                return nullptr;
            }
            work = t ? t : p_.types.elementary(TypeKind::LReal);
        } else if (t && (isInteger(t->kind) || isBitString(t->kind))) {
            work = t;
        } else {
            work = literalDefault(*e, *e);
        }
        Value v;
        if (!evalLiterals(*e, work, v)) return nullptr;
        const Type* result = work;
        if (t && isReal(t->kind) && !isReal(work->kind)) {
            // An integer constant used as REAL: exact, and integer division warned about
            if (hasOperator(*e, BinaryOp::Div)) {
                d_.warning("W_INT_DIVISION", e->loc,
                           std::format("integer division in a REAL context drops the fraction: this is {}", v.text()),
                           "write real literals (7.0/2.0) for a real result");
            }
            double d = isSigned(work->kind) ? static_cast<double>(v.i) : static_cast<double>(v.u);
            uint64_t mag = isSigned(work->kind) ? (v.i < 0 ? static_cast<uint64_t>(-(v.i + 1)) + 1 : static_cast<uint64_t>(v.i)) : v.u;
            if (!fitsIn(t, false, mag)) {
                d_.error("E_LITERAL_RANGE", e->loc, std::format("{} cannot be represented exactly as {}", v.text(), t->name));
                return nullptr;
            }
            v = Value::ofReal(t->kind, d);
            result = t;
        }
        e->type = result;
        e->isConstant = true;
        e->constant = v;
        return result;
    }

    /// Types an expression. `expected` guides untyped literals and enumeration names only;
    /// the caller converts the result (coerce) where a type is required.
    const Type* typeExpr(ExprPtr& e, const Type* expected, Ctx& ctx) {
        if (!e) return nullptr;
        if (e->type) return e->type;
        if (isFlexible(*e)) return typeFlexible(e, expected);
        const Type* t = nullptr;
        switch (e->kind) {
            case ExprKind::BoolLit:
                t = p_.types.elementary(TypeKind::Bool);
                e->isConstant = true;
                e->constant = Value::ofBool(e->as<BoolLit>().value);
                break;
            case ExprKind::StringLit: {
                const std::string& s = e->as<StringLit>().value;
                if (s.size() > TypeTable::kMaxStringLength) {
                    d_.error("E_LIMIT", e->loc, "string literal is longer than 65535 bytes");
                    return nullptr;
                }
                t = p_.types.string(std::max<uint32_t>(1, static_cast<uint32_t>(s.size())));
                e->isConstant = true;
                e->constant = Value::ofString(s);
                break;
            }
            case ExprKind::TypedLit: t = typedLiteral(e->as<TypedLit>(), *e); break;
            case ExprKind::Name: t = typeName(e->as<Name>(), expected, ctx); break;
            case ExprKind::Member: t = typeMember(e->as<Member>(), ctx); break;
            case ExprKind::BitAccess: {
                auto& b = e->as<BitAccess>();
                const Type* bt = typeExpr(b.base, nullptr, ctx);
                if (!bt) return nullptr;
                if (!isBitString(baseOf(bt)->kind)) {
                    d_.error("E_TYPE", e->loc, std::format("bit access needs BYTE, WORD, DWORD or LWORD, not {}", bt->name));
                    return nullptr;
                }
                if (b.bit >= bitWidth(baseOf(bt)->kind)) {
                    d_.error("E_INDEX", e->loc, std::format("{} has bits 0…{}", bt->name, bitWidth(baseOf(bt)->kind) - 1));
                    return nullptr;
                }
                t = p_.types.elementary(TypeKind::Bool);
                break;
            }
            case ExprKind::Index: t = typeIndex(e->as<Index>(), ctx); break;
            case ExprKind::Call: {
                if (const Type* w = wrapEnEno(e, expected, ctx)) return w;
                if (e->type) return e->type;
                // The EN/ENO wrapper of a call that failed: the error is reported, there is nothing more to type
                if (e->as<Call>().callKind == CallKind::Standard && e->as<Call>().standard == static_cast<int>(StdFn::Eval)) {
                    return nullptr;
                }
                Rewrite r = rewriteOperatorFunction(e);
                if (r == Rewrite::Failed) return nullptr;
                if (r == Rewrite::Done) return typeExpr(e, expected, ctx);
                if (rewriteConversion(e, ctx)) {
                    // A conversion of a constant is folded (and refused when it cannot be done) like any constant
                    t = e->type;
                    if (!t) return nullptr;
                    break;
                }
                t = typeCall(e, ctx, false, expected);
                break;
            }
            case ExprKind::Unary: t = typeUnary(e->as<Unary>(), expected, ctx); break;
            case ExprKind::Binary: t = typeBinary(e->as<Binary>(), expected, ctx); break;
            case ExprKind::Convert: return e->type;
            case ExprKind::IntLit:
            case ExprKind::RealLit:
                break;
        }
        if (!t) return nullptr;
        e->type = t;
        if (!e->isConstant && e->kind != ExprKind::Call) {
            Value v;
            int fault = 0;
            if (evaluateConstant(*e, v, &fault)) {
                // keep the tree for the listing; record the value
                e->isConstant = true;
                e->constant = v;
            } else if (fault && !constantFaultReported(*e)) {
                reportConstantFault(*e, static_cast<vm::Fault>(fault));
                return nullptr;
            }
            if (e->kind == ExprKind::Binary && e->as<Binary>().op == BinaryOp::Mod && e->as<Binary>().rhs->isConstant &&
                e->as<Binary>().rhs->constant.bits() == 0 && !isReal(baseOf(e->as<Binary>().rhs->type)->kind)) {
                d_.error("E_DIV_ZERO", e->as<Binary>().rhs->loc, "MOD by a constant 0 always gives 0",
                         "this is almost certainly a mistake");
                return nullptr;
            }
            if (e->kind == ExprKind::Binary && e->as<Binary>().op == BinaryOp::Div && e->as<Binary>().rhs->isConstant) {
                const Expr& rhs = *e->as<Binary>().rhs;
                bool zero = isReal(baseOf(rhs.type)->kind) ? rhs.constant.r == 0.0 : rhs.constant.bits() == 0;
                if (zero) {
                    d_.error("E_DIV_ZERO", rhs.loc, "division by a constant 0: the program would stop here every time",
                             "divide by a variable that cannot be 0, or check it first");
                    return nullptr;
                }
            }
        }
        return t;
    }

    bool constantFaultReported(const Expr&) const { return false; }

    /// The innermost constant conversion in `e` whose value does not fit its target.
    static const Expr* failingConversion(const Expr& e) {
        auto children = [](const Expr& x) -> std::vector<const Expr*> {
            switch (x.kind) {
                case ExprKind::Convert: return {x.as<ast::Convert>().operand.get()};
                case ExprKind::Unary: return {x.as<Unary>().operand.get()};
                case ExprKind::Binary: return {x.as<Binary>().lhs.get(), x.as<Binary>().rhs.get()};
                case ExprKind::Call: {
                    std::vector<const Expr*> out;
                    for (const Arg& a : x.as<Call>().args) out.push_back(a.value.get());
                    return out;
                }
                default: return {};
            }
        };
        for (const Expr* c : children(e)) {
            if (c) {
                if (const Expr* inner = failingConversion(*c)) return inner;
            }
        }
        if (e.kind == ExprKind::Convert) {
            Value v;
            int fault = 0;
            if (!evaluateConstant(e, v, &fault) && fault == static_cast<int>(vm::Fault::Conversion)) return &e;
        }
        return nullptr;
    }

    void reportConstantFault(const Expr& e, vm::Fault f) {
        switch (f) {
            case vm::Fault::DivZero:
                d_.error("E_DIV_ZERO", e.loc, "division by zero in a constant expression");
                break;
            case vm::Fault::Conversion: {
                // Name the conversion that fails, not the expression around it
                const Expr* at = failingConversion(e);
                const Expr& c = at ? *at : e;
                Value in;
                std::string what = at && evaluateConstant(*at->as<ast::Convert>().operand, in) ? in.text() : "the constant";
                d_.error("E_OVERFLOW", c.loc, std::format("{} cannot be converted to {}: it is outside its range", what,
                                                          c.type ? c.type->name : "the type"));
                break;
            }
            default:
                d_.error("E_OVERFLOW", e.loc,
                         std::format("the constant expression overflows {}", e.type ? e.type->name : "its type"),
                         "the result of an operation has the type of its operands; widen them first");
                break;
        }
    }

    /// Converts a typed expression to `target`: same type, a lossless widening (a Convert node),
    /// a string of any capacity (checked when stored), or an error.
    bool coerce(ExprPtr& e, const Type* target, std::string_view verb) {
        if (!e || !e->type || !target) return false;
        const Type* from = e->type;
        const Type* bf = baseOf(from);
        const Type* bt = baseOf(target);
        if (sameType(bf, bt)) {
            if (target->hasRange && e->isConstant && isInteger(bt->kind)) {
                int64_t v = isUnsigned(bt->kind) ? static_cast<int64_t>(e->constant.u) : e->constant.i;
                if (v < target->rangeLow || v > target->rangeHigh) {
                    d_.error("E_LITERAL_RANGE", e->loc, std::format("{} is outside {}", v, target->name));
                    return false;
                }
            }
            return true;
        }
        if (bf->kind == TypeKind::String && bt->kind == TypeKind::String) {
            if (!e->isConstant && bf->capacity > bt->capacity) {
                d_.warning("W_STRING_CAPACITY", e->loc,
                           std::format("a {} can be longer than {} holds; a longer value stops the program (F_STRING_LENGTH)",
                                       bf->name, target->name),
                           "make the target longer, or take LEFT(…) of the value");
            }
            if (e->isConstant && e->constant.s.size() > bt->capacity) {
                d_.error("E_LITERAL_RANGE", e->loc,
                         std::format("the string is {} bytes; {} holds {}", e->constant.s.size(), target->name, bt->capacity));
                return false;
            }
            return true;  // capacity checked at run time (F_STRING_LENGTH)
        }
        if (isElementary(bf->kind) && isElementary(bt->kind) && implicitlyConvertible(bf->kind, bt->kind)) {
            wrapConvert(e, target, true);
            return true;
        }
        std::string hint;
        if (isElementary(bf->kind) && isElementary(bt->kind)) {
            if (implicitlyConvertible(bt->kind, bf->kind)) {
                d_.error("E_NARROWING", e->loc,
                         std::format("cannot {} {} {} without a conversion: values may not fit",
                                     verb, bf->name, verb == "assign" ? std::format("to {}", bt->name) : bt->name),
                         std::format("write {}_TO_{}(…)", bf->name, bt->name));
                return false;
            }
            hint = std::format("convert explicitly, e.g. {}_TO_{}(…)", bf->name, bt->name);
            if (isBitString(bf->kind) != isBitString(bt->kind) && isInteger(bf->kind) != isInteger(bt->kind) &&
                bf->size == bt->size) {
                hint = std::format("bit strings and integers are different types; write {}_TO_{}(…)", bf->name, bt->name);
            }
        }
        d_.error("E_TYPE", e->loc,
                 std::format("{} is {}, but {} is needed", describeExpr(*e), from->name, target->name), hint);
        return false;
    }

    std::string describeExpr(const Expr& e) const {
        if (e.kind == ExprKind::Name) return std::format("'{}'", e.as<Name>().spelled);
        if (e.isConstant) return "the value";
        return "the expression";
    }

    /// `warn`: W_NARROW_ARITH when integer arithmetic is widened afterwards. Not for index, position
    /// and count arguments: an overflow there faults like the out-of-range value it would become.
    void wrapConvert(ExprPtr& e, const Type* to, bool implicit, bool warn = true) {
        SourceLoc loc = e->loc;
        if (implicit && warn && !e->isConstant && e->kind == ExprKind::Binary && isArithmetic(e->as<Binary>().op) &&
            e->type && isInteger(baseOf(e->type)->kind)) {
            d_.warning("W_NARROW_ARITH", loc,
                       std::format("this is computed in {} and can overflow before it becomes {}", baseOf(e->type)->name, to->name),
                       std::format("convert an operand first, e.g. {}_TO_{}(…), to compute in {}", baseOf(e->type)->name,
                                   to->name, to->name));
        }
        bool constant = e->isConstant;
        auto c = std::make_unique<ast::Convert>(loc, std::move(e), implicit);
        c->type = to;
        e = std::move(c);
        if (constant) {
            Value v;
            if (evaluateConstant(*e, v)) {
                e->isConstant = true;
                e->constant = v;
            } else if (implicit) {
                d_.error("E_LITERAL_RANGE", loc, std::format("the value does not fit {}", to->name));
            }
        }
    }

    // ── Literals ──

    const Type* typedLiteral(TypedLit& lit, Expr& e) {
        const std::string& p = lit.prefix;
        std::string body = lit.body;
        if (p == "T" || p == "TIME" || p == "LT" || p == "LTIME") {
            int64_t ns = 0;
            if (!parseDuration(body, lit.loc, ns)) return nullptr;
            TypeKind k = (p == "T" || p == "TIME") ? TypeKind::Time : TypeKind::LTime;
            e.isConstant = true;
            e.constant = Value::ofSigned(k, ns);
            return p_.types.elementary(k);
        }
        if (const Type* t = p_.types.elementaryByName(p)) {
            TypeKind k = t->kind;
            if (lit.quoted) {
                d_.error("E_LITERAL", lit.loc, std::format("{}#'…' is not a literal", t->name));
                return nullptr;
            }
            if (k == TypeKind::Bool) {
                std::string u = toUpper(body);
                if (u == "TRUE" || u == "1") e.constant = Value::ofBool(true);
                else if (u == "FALSE" || u == "0") e.constant = Value::ofBool(false);
                else {
                    bool digits = !u.empty() && u.find_first_not_of("0123456789_") == std::string::npos;
                    d_.error(digits ? "E_LITERAL_RANGE" : "E_LITERAL", lit.loc, "BOOL# takes TRUE, FALSE, 0 or 1");
                    return nullptr;
                }
                e.isConstant = true;
                return t;
            }
            if (isDuration(k)) {
                d_.error("E_LITERAL", lit.loc, "write durations as T#… or LT#…");
                return nullptr;
            }
            // Numeric: re-lex the body as a (signed) number literal
            bool neg = false;
            std::string num = body;
            if (!num.empty() && (num[0] == '-' || num[0] == '+')) {
                neg = num[0] == '-';
                num.erase(0, 1);
            }
            Diagnostics local;
            ExprPtr parsed = parseExpression(num, local);
            if (local.hasErrors() || !parsed || (parsed->kind != ExprKind::IntLit && parsed->kind != ExprKind::RealLit)) {
                bool range = false;
                for (const Diagnostic& ld : local.all()) range = range || ld.code == "E_LITERAL_RANGE";
                d_.error(range ? "E_LITERAL_RANGE" : "E_LITERAL", lit.loc,
                         range ? std::format("'{}' is outside the range of {}", body, t->name)
                               : std::format("'{}' is not a valid {} literal", body, t->name));
                return nullptr;
            }
            parsed->loc = lit.loc;
            if (neg) parsed = std::make_unique<Unary>(lit.loc, UnaryOp::Neg, std::move(parsed));
            if (parsed->kind != ExprKind::Unary && parsed->kind == ExprKind::RealLit && !isReal(k)) {
                d_.error("E_LITERAL", lit.loc, std::format("{}# needs an integer", t->name));
                return nullptr;
            }
            if (!typeFlexible(parsed, t)) return nullptr;
            e.isConstant = true;
            e.constant = parsed->constant;
            return t;
        }
        if (p == "STRING") {
            if (!lit.quoted) {
                d_.error("E_LITERAL", lit.loc, "STRING# needs a quoted string: STRING#'text'");
                return nullptr;
            }
            e.isConstant = true;
            e.constant = Value::ofString(lit.body);
            return p_.types.string(std::max<uint32_t>(1, static_cast<uint32_t>(lit.body.size())));
        }
        if (const Type* t = resolveUserType(p, lit.loc)) {
            const Type* bt = baseOf(t);
            if (bt->kind == TypeKind::Enum) {
                const EnumConstant* c = bt->enumValue(toUpper(body));
                if (!c) {
                    d_.error("E_UNDECLARED", lit.loc, std::format("{} has no value '{}'", t->name, body));
                    return nullptr;
                }
                e.isConstant = true;
                e.constant = Value::ofSigned(bt->enumBase->kind, c->value);
                e.constant.kind = bt->enumBase->kind;
                return t;
            }
            d_.error("E_LITERAL", lit.loc, std::format("{}# literals are only for enumerations and elementary types", t->name));
            return nullptr;
        }
        if (p == "D" || p == "DATE" || p == "TOD" || p == "TIME_OF_DAY" || p == "DT" || p == "DATE_AND_TIME" ||
            p == "WSTRING" || p == "CHAR" || p == "WCHAR" || p == "LDATE" || p == "LTOD" || p == "LDT") {
            d_.error("E_NOT_SUPPORTED", lit.loc, std::format("{}# literals are not supported yet", p));
            return nullptr;
        }
        d_.error("E_UNDECLARED", lit.loc, std::format("unknown type '{}' in a typed literal", p));
        return nullptr;
    }

    bool parseDuration(const std::string& text, SourceLoc loc, int64_t& out) {
        std::string error;
        if (st::parseDuration(text, out, error)) return true;
        bool range = error.find("out of range") != std::string::npos || error.find("too long") != std::string::npos;
        d_.error(range ? "E_LITERAL_RANGE" : "E_LITERAL", loc, std::format("duration '{}': {}", text, error),
                 "write e.g. T#1h30m, T#250ms or T#1.5s");
        return false;
    }

    // ── Names ──

    /// Whether `pou` declares `upper` at all (the caller found no symbol yet, so: further down).
    static bool declaredLater(const ast::Pou& pou, const std::string& upper) {
        for (const VarSection& sec : pou.sections) {
            for (const VarDecl& decl : sec.decls) {
                for (const DeclName& d : decl.names) {
                    if (d.upper == upper) return true;
                }
            }
        }
        return false;
    }

    const Type* typeName(Name& n, const Type* expected, Ctx& ctx) {
        if (ctx.pou) {
            if (Symbol* s = ctx.pou->find(n.upper)) {
                if (ctx.initializer && !(s->constant && s->hasConstValue)) {
                    if (s->constant && s->kind == SymKind::Var && isElementary(baseOf(s->type)->kind)) {
                        auto st = declStates_.find(ctx.pou);
                        if (st != declStates_.end() && st->second.inProgress.count(s->decl)) {
                            d_.error("E_TYPE", n.loc, std::format("the constant '{}' is defined through itself", n.spelled),
                                     "give one of the constants a value that does not depend on the other");
                            return nullptr;
                        }
                        if (s->init) return nullptr;  // its initial value has the error
                    }
                    d_.error("E_TYPE", n.loc, std::format("'{}' is not a constant", n.spelled),
                             "initial values, array bounds and case labels need constants");
                    return nullptr;
                }
                n.symbol = s;
                s->used = true;  // in an initial value or an array bound too: a constant counts as used there
                if (s->constant && s->hasConstValue) {
                    n.isConstant = true;
                    n.constant = s->constValue;
                }
                return s->type;
            }
            // Its declaration has the error: the name means that variable, not a POU or type of the same name
            if (failed_.count({ctx.pou, n.upper})) return nullptr;
            if (ctx.initializer) {
                if (auto st = declStates_.find(ctx.pou); st != declStates_.end()) {
                    if (auto c = st->second.constants.find(n.upper); c != st->second.constants.end()) {
                        auto [sec, decl] = c->second;
                        if (st->second.inProgress.count(decl)) {
                            d_.error("E_TYPE", n.loc, std::format("the constant '{}' is defined through itself", n.spelled),
                                     "give one of the constants a value that does not depend on the other");
                            failed_.insert({ctx.pou, n.upper});
                            return nullptr;
                        }
                        if (!st->second.done.count(decl)) {  // declared below: declare it now, then use it
                            DeclState& state = st->second;
                            state.inProgress.insert(decl);
                            declareNames(*ctx.pou, *sec, *decl, SymKind::Var, state.order);
                            state.inProgress.erase(decl);
                            state.done.insert(decl);
                            return typeName(n, expected, ctx);
                        }
                    }
                }
            }
        }
        if (Symbol* g = globals_.find(n.upper); g && g->hasConstValue) {
            n.symbol = g;
            n.isConstant = true;
            n.constant = g->constValue;
            return g->type;
        }
        // An enumeration value: unique among all enumerations, or the expected one
        const Type* found = nullptr;
        const EnumConstant* value = nullptr;
        int count = 0;
        const Type* want = expected ? baseOf(expected) : nullptr;
        if (want && want->kind == TypeKind::Enum) {
            if (const EnumConstant* c = want->enumValue(n.upper)) {
                found = expected;
                value = c;
                count = 1;
            }
        }
        if (!found) {
            for (const auto& [name, t] : p_.typeByName) {
                if (t->kind != TypeKind::Enum || t != baseOf(t)) continue;
                if (const EnumConstant* c = t->enumValue(n.upper)) {
                    if (found == t) continue;
                    found = t;
                    value = c;
                    count++;
                }
            }
        }
        if (count > 1) {
            d_.error("E_AMBIGUOUS", n.loc, std::format("'{}' is a value of several enumerations", n.spelled),
                     std::format("qualify it: Type#{}", n.spelled));
            return nullptr;
        }
        if (found) {
            n.isConstant = true;
            n.constant = Value::ofSigned(baseOf(found)->enumBase->kind, value->value);
            return found;
        }
        if (p_.findPou(n.upper) || findStd(n.upper) || isConversionName(n.upper)) {
            d_.error("E_TYPE", n.loc, std::format("'{}' is a function or block, not a value", n.spelled),
                     "call a function with ( … ); call a function block through an instance");
            return nullptr;
        }
        std::string hint;
        if (ctx.pou) {
            size_t best = 3;
            for (const auto& [name, s] : ctx.pou->byName) {
                size_t dist = editDistance(n.upper, name);
                if (dist < best) {
                    best = dist;
                    hint = std::format("did you mean '{}'?", s->spelled);
                }
            }
        }
        if (hint.empty() && ctx.pou && ctx.pou->kind == PouKind::Program) {
            for (const TagDef& t : tags_) {
                if (toUpper(t.name) == n.upper) {
                    hint = std::format("to use the tag '{}', declare VAR_EXTERNAL {} : {}; END_VAR", t.name, t.name,
                                       t.type == TagType::Bool ? "BOOL" : t.type == TagType::Float32 ? "REAL"
                                       : t.type == TagType::Int16 ? "INT" : t.type == TagType::Int32 ? "DINT"
                                       : t.type == TagType::UInt32 ? "UDINT" : "UINT");
                }
            }
        }
        if (ctx.initializer && ctx.pou && ctx.pou->ast && declaredLater(*ctx.pou->ast, n.upper)) {
            d_.error("E_TYPE", n.loc, std::format("'{}' is not a constant", n.spelled),
                     "initial values, array bounds and case labels need constants (VAR CONSTANT)");
            return nullptr;
        }
        d_.error("E_UNDECLARED", n.loc, std::format("'{}' is not declared", n.spelled), hint);
        return nullptr;
    }

    const Type* typeMember(Member& m, Ctx& ctx) {
        // CODESYS habits: E_State.Running for an enumeration value, GVL.gMax for a global constant
        if (m.base && m.base->kind == ExprKind::Name && !(ctx.pou && ctx.pou->find(m.base->as<Name>().upper))) {
            const Name& base = m.base->as<Name>();
            auto type = p_.typeByName.find(base.upper);
            if (type != p_.typeByName.end() && baseOf(type->second)->kind == TypeKind::Enum && baseOf(type->second)->enumValue(m.upper)) {
                d_.error("E_UNDECLARED", base.loc, std::format("'{}' is an enumeration type, not a variable", base.spelled),
                         std::format("write an enumeration value with #: {}#{}", base.spelled, m.spelled));
                return nullptr;
            }
            if (type == p_.typeByName.end() && !p_.findPou(base.upper) && globals_.find(m.upper)) {
                d_.error("E_UNDECLARED", base.loc, std::format("'{}' is not declared", base.spelled),
                         std::format("global constants are used without a prefix: write {}", m.spelled));
                return nullptr;
            }
        }
        const Type* bt = typeExpr(m.base, nullptr, ctx);
        if (!bt) return nullptr;
        const Type* b = baseOf(bt);
        if (b->kind == TypeKind::Struct) {
            const Field* f = b->field(m.upper);
            if (!f) {
                d_.error("E_UNDECLARED", m.loc, std::format("{} has no member '{}'", b->name, m.spelled));
                return nullptr;
            }
            m.offset = f->offset;
            return f->type;
        }
        if (b->kind == TypeKind::FunctionBlock) {
            Symbol* s = b->pou->find(m.upper);
            if (!s || s->kind == SymKind::Result) {
                d_.error("E_UNDECLARED", m.loc, std::format("{} has no variable '{}'", b->name, m.spelled));
                return nullptr;
            }
            // IEC 61131-3 Ed. 3 Figure 13: outside an instance, outputs may be read and inputs
            // assigned (inst.IN := x); inputs may not be read, in-outs and internals not used
            bool writingInput = s->kind == SymKind::Input && ctx.memberTarget == &m;
            if (s->kind == SymKind::Input && !writingInput) {
                d_.error("E_ACCESS", m.loc,
                         std::format("reading the input '{}' of an instance from outside is not allowed (IEC 61131-3 Figure 13)", m.spelled),
                         "keep the value you pass in a variable of your own (CODESYS allows this read; the standard does not)");
                return nullptr;
            }
            if (s->kind != SymKind::Input && s->kind != SymKind::Output) {
                d_.error("E_ACCESS", m.loc,
                         std::format("'{}' is {} of {}: only outputs can be read from outside", m.spelled,
                                     s->kind == SymKind::InOut ? "a VAR_IN_OUT" : "internal", b->name));
                return nullptr;
            }
            m.member = s;
            m.offset = s->offset;
            return s->type;
        }
        d_.error("E_TYPE", m.loc, std::format("{} has no members", bt->name));
        return nullptr;
    }

    const Type* typeIndex(Index& ix, Ctx& ctx) {
        const Type* bt = typeExpr(ix.base, nullptr, ctx);
        if (!bt) return nullptr;
        const Type* b = baseOf(bt);
        if (b->kind != TypeKind::Array) {
            d_.error("E_TYPE", ix.loc, std::format("{} is not an array", bt->name),
                     b->kind == TypeKind::String ? "use MID(s, 1, n) to take characters of a string" : "");
            return nullptr;
        }
        if (ix.indexes.size() != b->dims.size()) {
            d_.error("E_TYPE", ix.loc, std::format("{} needs {} index{}", b->name, b->dims.size(), b->dims.size() == 1 ? "" : "es"));
            return nullptr;
        }
        for (size_t i = 0; i < ix.indexes.size(); i++) {
            ExprPtr& e = ix.indexes[i];
            const Type* it = typeExpr(e, p_.types.elementary(TypeKind::DInt), ctx);
            if (!it) return nullptr;
            TypeKind k = baseOf(it)->kind;
            if (!isInteger(k)) {
                d_.error("E_TYPE", e->loc, std::format("an array index must be an integer, not {}", it->name));
                return nullptr;
            }
            if (k != TypeKind::LInt) {
                if (k == TypeKind::ULInt) {
                    // ULINT indexes beyond LINT are out of any array's bounds anyway; checked at run time
                    wrapConvert(e, p_.types.elementary(TypeKind::LInt), false);
                } else {
                    wrapConvert(e, p_.types.elementary(TypeKind::LInt), true, false);
                }
            }
            Value v;
            if (evaluateConstant(*e, v)) {
                int64_t idx = v.i;
                if (idx < b->dims[i].low || idx > b->dims[i].high) {
                    d_.error("E_INDEX", e->loc, std::format("index {} is outside {}..{}", idx, b->dims[i].low, b->dims[i].high));
                    return nullptr;
                }
            }
        }
        return b->element;
    }

    // ── Operators ──

    const Type* typeUnary(Unary& u, const Type* expected, Ctx& ctx) {
        // NOT 16#1 into a WORD: the literal takes the context, as NOT's result has its operand's type
        const Type* t = typeExpr(u.operand, expected, ctx);
        if (!t) return nullptr;
        TypeKind k = baseOf(t)->kind;
        switch (u.op) {
            case UnaryOp::Neg:
                if (isSigned(k) || isReal(k) || isDuration(k)) return baseOf(t);
                d_.error("E_TYPE", u.loc, std::format("'-' needs a signed number or a duration, not {}", t->name),
                         isUnsigned(k) ? std::format("convert it to a signed type first, e.g. {}_TO_DINT(…)", t->name) : "");
                return nullptr;
            case UnaryOp::Plus:
                if (isMagnitude(k)) return baseOf(t);
                d_.error("E_TYPE", u.loc, std::format("'+' needs a number, not {}", t->name));
                return nullptr;
            case UnaryOp::Not:
                if (k == TypeKind::Bool || isBitString(k)) return baseOf(t);
                d_.error("E_TYPE", u.loc, std::format("NOT needs BOOL or a bit string, not {}", t->name),
                         isInteger(k) ? "for integers, compare instead (x = 0)" : "");
                return nullptr;
        }
        return nullptr;
    }

    /// The common type of two operands: one widens losslessly to the other.
    const Type* commonType(const Type* a, const Type* b) {
        const Type* x = baseOf(a);
        const Type* y = baseOf(b);
        if (sameType(x, y)) return x;
        if (isElementary(x->kind) && isElementary(y->kind)) {
            if (implicitlyConvertible(x->kind, y->kind)) return y;
            if (implicitlyConvertible(y->kind, x->kind)) return x;
        }
        return nullptr;
    }

    const Type* typeBinary(Binary& b, const Type* expected, Ctx& ctx) {
        bool lf = isFlexible(*b.lhs), rf = isFlexible(*b.rhs);
        // An operand of literals only (SQRT(2.0), NOT 16#1, (16#F0 XOR 16#1)) takes its type from the other one too
        bool lc = !lf && literalOnly(*b.lhs), rc = !rf && literalOnly(*b.rhs);
        if (lc && rc) lc = rc = false;
        const Type* lt = lf || lc ? nullptr : typeExpr(b.lhs, nullptr, ctx);
        const Type* rt = rf || rc ? nullptr : typeExpr(b.rhs, nullptr, ctx);
        if (lc) lt = typeExpr(b.lhs, rt ? rt : expected, ctx);
        if (rc) rt = typeExpr(b.rhs, lt ? lt : expected, ctx);
        if ((!lf && !lt) || (!rf && !rt)) {
            if (lf) typeExpr(b.lhs, nullptr, ctx);
            if (rf) typeExpr(b.rhs, nullptr, ctx);
            return nullptr;
        }
        const char* op = opText(b.op);

        if (b.op == BinaryOp::Pow) {
            const Type* base = lt;
            if (lf) {
                // A literal base takes the type of the context (r := 2.0 ** x is REAL), else of a real exponent
                base = expected && isReal(baseOf(expected)->kind) ? baseOf(expected)
                       : (rt && isReal(baseOf(rt)->kind))           ? baseOf(rt)
                                                                    : p_.types.elementary(TypeKind::LReal);
                if (!typeExpr(b.lhs, base, ctx)) return nullptr;
            }
            base = baseOf(base);
            if (!isReal(base->kind)) {
                d_.error("E_TYPE", b.lhs->loc, std::format("'**' needs a REAL or LREAL base, not {}", base->name),
                         std::format("convert it, e.g. {}_TO_LREAL(…)", base->name));
                return nullptr;
            }
            if (rf && !typeExpr(b.rhs, base, ctx)) return nullptr;
            const Type* et = baseOf(b.rhs->type);
            if (!isNumeric(et->kind)) {
                d_.error("E_TYPE", b.rhs->loc, std::format("the exponent must be a number, not {}", et->name));
                return nullptr;
            }
            if (!sameType(et, base)) wrapConvert(b.rhs, base, false);  // EXPT's own conversion of IN2
            b.operandType = base;
            return base;
        }

        // Durations: TIME ± TIME, TIME * n, n * TIME, TIME / n
        bool ld = lt && isDuration(baseOf(lt)->kind), rd = rt && isDuration(baseOf(rt)->kind);
        if (ld || rd) {
            if (b.op == BinaryOp::Mul || b.op == BinaryOp::Div) {
                if (ld && rd) {
                    d_.error("E_TYPE", b.loc, std::format("'{}' of two durations is not defined", op));
                    return nullptr;
                }
                if (rd && b.op == BinaryOp::Div) {
                    d_.error("E_TYPE", b.loc, "a number cannot be divided by a duration");
                    return nullptr;
                }
                ExprPtr& num = ld ? b.rhs : b.lhs;
                bool numFlex = ld ? rf : lf;
                const Type* nt = numFlex ? typeExpr(num, nullptr, ctx) : num->type;
                if (!nt) return nullptr;
                if (numFlex && isInteger(baseOf(nt)->kind)) {
                    num->type = nullptr;
                    num->isConstant = false;
                    nt = typeExpr(num, p_.types.elementary(TypeKind::LInt), ctx);
                }
                if (!nt || !isNumeric(baseOf(nt)->kind)) {
                    d_.error("E_TYPE", num->loc, std::format("a duration can be multiplied or divided by a number, not {}", nt ? nt->name : "?"));
                    return nullptr;
                }
                const Type* dt = baseOf(ld ? lt : rt);
                b.operandType = dt;
                return dt;
            }
            if (b.op == BinaryOp::Add || b.op == BinaryOp::Sub || isComparison(b.op)) {
                if (lf || rf || !ld || !rd) {
                    d_.error("E_TYPE", b.loc, std::format("'{}' needs a duration on both sides", op), "write T#… for a duration");
                    return nullptr;
                }
            } else {
                d_.error("E_TYPE", b.loc, std::format("'{}' is not defined for durations", op));
                return nullptr;
            }
        }

        // An untyped literal takes the type of the other side
        if (lf && rf && isComparison(b.op)) {
            // Literals on both sides: both take the type literals have without context:
            // LREAL with a real literal, else DINT, or LINT/ULINT when a literal needs it
            const Type* work = literalDefault(*b.lhs, *b.rhs);
            lt = typeExpr(b.lhs, work, ctx);
            rt = typeExpr(b.rhs, work, ctx);
            lf = rf = false;
            if (!lt || !rt) return nullptr;
        }
        if (lf && rf && (b.op == BinaryOp::And || b.op == BinaryOp::Or || b.op == BinaryOp::Xor) && expected &&
            isBitString(baseOf(expected)->kind)) {
            // 16#F000 XOR 16#1 into a WORD: both literals take the context
            lt = typeExpr(b.lhs, baseOf(expected), ctx);
            rt = typeExpr(b.rhs, baseOf(expected), ctx);
            lf = rf = false;
            if (!lt || !rt) return nullptr;
        }
        if (lf && rf) {
            d_.error("E_TYPE", b.loc, "an operation needs at least one typed operand", "write a typed literal such as WORD#16#FF");
            return nullptr;
        }
        if (lf) lt = typeExpr(b.lhs, rt, ctx);
        if (rf) rt = typeExpr(b.rhs, lt, ctx);
        if (!lt || !rt) return nullptr;

        const Type* L = baseOf(lt);
        const Type* R = baseOf(rt);

        if (isComparison(b.op) && L->kind == TypeKind::String && R->kind == TypeKind::String) {
            b.operandType = L->capacity >= R->capacity ? L : R;
            return p_.types.elementary(TypeKind::Bool);
        }
        if (L->kind == TypeKind::Enum || R->kind == TypeKind::Enum) {
            if (L != R) {
                d_.error("E_TYPE", b.loc, std::format("cannot compare {} with {}", lt->name, rt->name));
                return nullptr;
            }
            if (b.op != BinaryOp::Eq && b.op != BinaryOp::Ne) {
                d_.error("E_TYPE", b.loc, std::format("enumeration values can only be compared with = and <>, not '{}'", op));
                return nullptr;
            }
            b.operandType = L;
            return p_.types.elementary(TypeKind::Bool);
        }

        const Type* common = commonType(L, R);
        if (!common) {
            std::string hint;
            if (isElementary(L->kind) && isElementary(R->kind))
                hint = std::format("convert one side explicitly, e.g. {}_TO_{}(…)", R->name, L->name);
            d_.error("E_TYPE", b.loc, std::format("'{}' between {} and {}: the types have no common type", op, lt->name, rt->name), hint);
            return nullptr;
        }
        if (!sameType(L, common)) wrapConvert(b.lhs, common, true);
        if (!sameType(R, common)) wrapConvert(b.rhs, common, true);
        TypeKind k = common->kind;
        b.operandType = common;

        if (isComparison(b.op)) {
            if (!isElementary(k)) {
                d_.error("E_TYPE", b.loc, std::format("{} values cannot be compared", common->name));
                return nullptr;
            }
            if (k == TypeKind::Bool && b.op != BinaryOp::Eq && b.op != BinaryOp::Ne) {
                // allowed by the standard (FALSE < TRUE); kept
            }
            return p_.types.elementary(TypeKind::Bool);
        }
        switch (b.op) {
            case BinaryOp::And:
            case BinaryOp::Or:
            case BinaryOp::Xor:
                if (k == TypeKind::Bool || isBitString(k)) return common;
                d_.error("E_TYPE", b.loc, std::format("'{}' needs BOOL or bit strings (BYTE…LWORD), not {}", op, common->name),
                         isInteger(k) ? std::format("for bitwise logic convert to a bit string, e.g. {}_TO_WORD(…)", common->name) : "");
                return nullptr;
            case BinaryOp::Mod:
                if (isInteger(k)) return common;
                d_.error("E_TYPE", b.loc, std::format("MOD needs integers, not {}", common->name));
                return nullptr;
            default:
                if (isNumeric(k) || (isDuration(k) && (b.op == BinaryOp::Add || b.op == BinaryOp::Sub))) return common;
                d_.error("E_TYPE", b.loc, std::format("'{}' needs numbers, not {}", op, common->name),
                         isBitString(k) ? std::format("{} is a bit string; convert it to an integer, e.g. {}_TO_UDINT(…)", common->name, common->name)
                         : k == TypeKind::String ? "join strings with CONCAT(a, b)" : "");
                return nullptr;
        }
    }

    // ── Calls ──

    /**
     * EN/ENO on a standard, operator or conversion function: the call without them is typed
     * as usual and evaluated inside an internal Eval call that carries EN and ENO, so a fault
     * in it sets ENO := FALSE instead of stopping the program. Returns the type, or null if
     * the call has no EN/ENO (or on error, with e->type left null and an error reported).
     */
    const Type* wrapEnEno(ExprPtr& e, const Type* expected, Ctx& ctx) {
        auto& c = e->as<Call>();
        if (c.callee->kind != ExprKind::Name) return nullptr;
        const std::string name = c.callee->as<Name>().upper;
        if (p_.findPou(name) || (ctx.pou && ctx.pou->find(name))) return nullptr;
        if (!findStd(name) && !isOperatorFunction(name) && !isConversionName(name)) return nullptr;
        int en = -1, eno = -1;
        for (size_t i = 0; i < c.args.size(); i++) {
            if (c.args[i].name == "EN") en = static_cast<int>(i);
            if (c.args[i].name == "ENO") eno = static_cast<int>(i);
        }
        if (en < 0 && eno < 0) return nullptr;
        std::vector<Arg> control;
        std::vector<Arg> rest;
        for (size_t i = 0; i < c.args.size(); i++) {
            (static_cast<int>(i) == en || static_cast<int>(i) == eno ? control : rest).push_back(std::move(c.args[i]));
        }
        c.args = std::move(rest);
        SourceLoc loc = e->loc;
        ExprPtr inner = std::move(e);
        const Type* t = typeExpr(inner, expected, ctx);  // literal arguments keep the context, as without EN/ENO
        auto wrapper = std::make_unique<Call>(loc, std::make_unique<Name>(loc, "__EVAL", "__EVAL"));
        Arg value;
        value.loc = loc;
        value.value = std::move(inner);
        wrapper->args.push_back(std::move(value));
        bool ok = t != nullptr;
        for (Arg& a : control) {
            if (a.name == "EN") {
                if (a.output) { d_.error("E_CALL", a.loc, "EN is an input: EN := condition"); ok = false; }
                ok = ok && typeExpr(a.value, p_.types.elementary(TypeKind::Bool), ctx) &&
                     coerce(a.value, p_.types.elementary(TypeKind::Bool), "use as EN");
                wrapper->enArg = static_cast<int>(wrapper->args.size());
            } else {
                if (!a.output) { d_.error("E_CALL", a.loc, "ENO is an output: ENO => variable"); ok = false; }
                const Type* bt = typeExpr(a.value, nullptr, ctx);
                if (bt && baseOf(bt)->kind != TypeKind::Bool) {
                    d_.error("E_TYPE", a.loc, "ENO => needs a BOOL variable");
                    ok = false;
                }
                ok = ok && bt && checkWritable(*a.value, ctx, "set by ENO");
                wrapper->enoArg = static_cast<int>(wrapper->args.size());
            }
            wrapper->args.push_back(std::move(a));
        }
        wrapper->callKind = CallKind::Standard;
        wrapper->standard = static_cast<int>(StdFn::Eval);
        e = std::move(wrapper);
        if (!ok || !t) return nullptr;
        if (t->kind == TypeKind::Void) {
            d_.error("E_CALL", loc, "this call returns no value");
            return nullptr;
        }
        e->type = t;
        return t;
    }

    enum class Rewrite { NotApplicable, Done, Failed };

    /// ADD(a, b, c) → a + b + c and the like; MOVE(x) → x.
    Rewrite rewriteOperatorFunction(ExprPtr& e) {
        auto& c = e->as<Call>();
        if (c.callee->kind != ExprKind::Name) return Rewrite::NotApplicable;
        const std::string n = c.callee->as<Name>().upper;
        if (!isOperatorFunction(n) || p_.findPou(n)) return Rewrite::NotApplicable;
        for (const Arg& a : c.args) {
            if (a.name == "EN" || a.name == "ENO") {
                d_.error("E_NOT_SUPPORTED", a.loc, std::format("EN/ENO on {}", n), "use the operator inside an IF");
                return Rewrite::Failed;
            }
        }
        // Formal names IN, IN1, IN2 … in order
        std::vector<ExprPtr> ins;
        for (size_t i = 0; i < c.args.size(); i++) {
            Arg& a = c.args[i];
            std::string want = (n == "NOT" || n == "MOVE") ? "IN" : std::format("IN{}", i + 1);
            if ((!a.name.empty() && a.name != want) || a.output) {
                d_.error("E_CALL", a.loc, std::format("{} expects {} here, not '{}'", n, want, a.spelled));
                return Rewrite::Failed;
            }
            ins.push_back(std::move(a.value));
        }
        SourceLoc loc = e->loc;
        size_t need = (n == "NOT" || n == "MOVE") ? 1 : 2;
        bool extensible = n == "ADD" || n == "MUL" || n == "AND" || n == "OR" || n == "XOR";
        std::string base = n.substr(0, n.find('_'));  // ADD_TIME → ADD
        if (ins.size() < need || (!extensible && ins.size() != need)) {
            d_.error("E_CALL", loc, std::format("{} takes {}{} input{}", n, need, extensible ? " or more" : "", need == 1 ? "" : "s"));
            return Rewrite::Failed;
        }
        ExprPtr out;
        if (n == "MOVE") {
            out = std::move(ins[0]);
        } else if (n == "NOT") {
            out = std::make_unique<Unary>(loc, UnaryOp::Not, std::move(ins[0]));
        } else {
            BinaryOp op = base == "ADD" ? BinaryOp::Add : base == "SUB" ? BinaryOp::Sub : base == "MUL" ? BinaryOp::Mul
                          : base == "DIV" ? BinaryOp::Div : base == "MOD" ? BinaryOp::Mod : base == "AND" ? BinaryOp::And
                          : base == "OR" ? BinaryOp::Or : BinaryOp::Xor;
            out = std::move(ins[0]);
            for (size_t i = 1; i < ins.size(); i++) {
                out->parenthesized = true;
                ins[i]->parenthesized = true;
                out = std::make_unique<Binary>(loc, op, std::move(out), std::move(ins[i]));
            }
        }
        out->parenthesized = true;
        e = std::move(out);
        return Rewrite::Done;
    }

    /// X_TO_Y(v), TO_Y(v), X_TRUNC_Y(v), TRUNC_Y(v), TRUNC(v) → a Convert node.
    bool rewriteConversion(ExprPtr& e, Ctx& ctx) {
        auto& c = e->as<Call>();
        if (c.callee->kind != ExprKind::Name) return false;
        const std::string n = c.callee->as<Name>().upper;
        if (!isConversionName(n) || p_.findPou(n)) return false;
        std::string from, to;
        bool truncate = false;
        if (n == "TRUNC") {
            truncate = true;
            to = "DINT";
        } else if (n.starts_with("TRUNC_")) {
            truncate = true;
            to = n.substr(6);
        } else if (n.starts_with("TO_")) {
            to = n.substr(3);
        } else if (size_t p = n.find("_TRUNC_"); p != std::string::npos) {
            truncate = true;
            from = n.substr(0, p);
            to = n.substr(p + 7);
        } else if (size_t q = n.find("_TO_"); q != std::string::npos) {
            from = n.substr(0, q);
            to = n.substr(q + 4);
        }
        SourceLoc loc = e->loc;
        if (n == "TRUNC") {
            d_.error("E_NOT_SUPPORTED", loc, "TRUNC without a result type is deprecated in IEC 61131-3 Ed. 3",
                     "write TRUNC_DINT(…) or REAL_TRUNC_DINT(…)");
            return true;
        }
        auto unsupported = [&](std::string why) {
            d_.error("E_NOT_SUPPORTED", loc, std::format("{}: {}", c.callee->as<Name>().spelled, why),
                     "see docs/st-compliance.md for the supported conversions");
            return true;
        };
        if (from.find("BCD") != std::string::npos || to.find("BCD") != std::string::npos)
            return unsupported("BCD conversions are not supported yet");
        auto typeOf = [&](const std::string& name) -> const Type* {
            if (name == "STRING") return p_.types.string(TypeTable::kDefaultStringLength);
            return p_.types.elementaryByName(name);
        };
        const Type* toT = typeOf(to);
        const Type* fromT = from.empty() ? nullptr : typeOf(from);
        if (!toT || (!from.empty() && !fromT)) {
            d_.error("E_UNDECLARED", loc, std::format("unknown conversion function '{}'", c.callee->as<Name>().spelled));
            return true;
        }
        if (c.args.size() != 1 || (!c.args[0].name.empty() && c.args[0].name != "IN") || c.args[0].output) {
            d_.error("E_CALL", loc, std::format("{} takes one input (IN)", c.callee->as<Name>().spelled));
            return true;
        }
        ExprPtr arg = std::move(c.args[0].value);
        const Type* at = typeExpr(arg, fromT, ctx);
        if (!at) return true;
        if (fromT) {
            if (!coerce(arg, fromT, "convert")) return true;
        } else {
            fromT = baseOf(at);
        }
        TypeKind fk = baseOf(fromT)->kind, tk = toT->kind;
        if (truncate && (!isReal(fk) || !isInteger(tk)))
            return unsupported("TRUNC converts REAL or LREAL to an integer type");
        bool ok = isElementary(fk) || fk == TypeKind::String;
        ok = ok && (isElementary(tk) || tk == TypeKind::String);
        if (ok && ((isReal(fk) && isBitString(tk)) || (isBitString(fk) && isReal(tk)))) {
            // Ed. 3 defines only the same-width pairs (REAL ↔ DWORD, LREAL ↔ LWORD), as a copy of the bits
            const TypeKind rk = isReal(fk) ? fk : tk, bk = isReal(fk) ? tk : fk;
            const bool sameWidth = bk == (rk == TypeKind::Real ? TypeKind::DWord : TypeKind::LWord);
            d_.error("E_AMBIGUOUS", loc,
                     sameWidth ? std::format("{}: implementations disagree: IEC 61131-3 Ed. 3 copies the bits, others convert the value",
                                             c.callee->as<Name>().spelled)
                               : std::format("{}: not defined by IEC 61131-3 (only {} ↔ {} copies the bits); implementations differ",
                                             c.callee->as<Name>().spelled, isReal(fk) ? fromT->name : toT->name,
                                             rk == TypeKind::Real ? "DWORD" : "LWORD"),
                     std::format("for the number, write {0}_TO_{2}(…) then {2}_TO_{1}(…)", fromT->name, toT->name,
                                 bk == TypeKind::LWord || rk == TypeKind::LReal ? "ULINT" : "UDINT"));
            return true;
        }
        if (ok && isSigned(fk) && isBitString(tk) && bitWidth(tk) > bitWidth(fk)) {
            std::string same = fk == TypeKind::SInt ? "BYTE" : fk == TypeKind::Int ? "WORD" : fk == TypeKind::DInt ? "DWORD" : "LWORD";
            d_.error("E_AMBIGUOUS", loc,
                     std::format("{} to the wider {}: implementations disagree on sign extension", fromT->name, toT->name),
                     std::format("write {}_TO_{}({}_TO_{}(…)) to fill with zeros", same, toT->name, fromT->name, same));
            return true;
        }
        if (ok && ((isDuration(fk) && (tk == TypeKind::Bool || isBitString(tk))) ||
                   ((fk == TypeKind::Bool || isBitString(fk)) && isDuration(tk))))
            ok = false;
        if (ok && ((isDuration(fk) && isNumeric(tk)) || (isNumeric(fk) && isDuration(tk)))) {
            bool lt = fk == TypeKind::LTime || tk == TypeKind::LTime;
            d_.warning("W_TIME_UNIT", loc,
                       std::format("{} counts {}: the unit of TIME ↔ number conversions is implementation-defined",
                                   c.callee->as<Name>().spelled, lt ? "nanoseconds" : "milliseconds"),
                       "MatIEC/OpenPLC use seconds; CODESYS uses milliseconds as SimplePLC does");
        }
        if (!ok) {
            std::string hint;
            if ((isDuration(fk) && isBitString(tk)) || (isBitString(fk) && isDuration(tk))) {
                // The number in between says what the bits mean (milliseconds)
                const TypeKind bits = isBitString(fk) ? fk : tk;
                std::string_view num = bits == TypeKind::Byte ? "USINT" : bits == TypeKind::Word ? "UINT"
                                   : bits == TypeKind::DWord ? "UDINT" : "ULINT";
                hint = std::format("go through an unsigned integer: {}_TO_{}(…) then {}_TO_{}(…)", fromT->name, num, num, toT->name);
            }
            d_.error("E_TYPE", loc, std::format("{} cannot be converted to {}", fromT->name, toT->name), hint);
            return true;
        }
        auto conv = std::make_unique<ast::Convert>(loc, std::move(arg), false);
        conv->truncate = truncate;
        conv->type = toT;
        e = std::move(conv);
        Value v;
        if (evaluateConstant(*e, v)) {
            e->isConstant = true;
            e->constant = v;
        }
        return true;
    }

    /// Records a call for the recursion check.
    void noteCall(Ctx& ctx, PouInfo* callee, SourceLoc loc) {
        if (!ctx.pou || !callee) return;
        auto& list = ctx.pou->callees;
        if (std::find(list.begin(), list.end(), callee) == list.end()) {
            list.push_back(callee);
            callLoc_[{ctx.pou, callee}] = loc;
        }
    }

    const Type* typeCall(ExprPtr& e, Ctx& ctx, bool statement, const Type* expected = nullptr) {
        auto& c = e->as<Call>();
        if (ctx.initializer) {
            d_.error("E_TYPE", c.loc, "a call is not a constant");
            return nullptr;
        }
        // Which callee?
        if (c.callee->kind == ExprKind::Name) {
            auto& n = c.callee->as<Name>();
            Symbol* s = ctx.pou ? ctx.pou->find(n.upper) : nullptr;
            if (s && s->kind == SymKind::Result) s = nullptr;  // F(…) inside F: a (recursive) call
            if (!s && ctx.pou && failed_.count({ctx.pou, n.upper})) return nullptr;  // an instance whose declaration has the error
            if (!s) {
                if (PouInfo* p = p_.findPou(n.upper)) {
                    if (p->kind == PouKind::Function) return typeUserCall(c, *p, ctx, statement, false);
                    if (p->kind == PouKind::FunctionBlock) {
                        d_.error("E_CALL", c.loc, std::format("'{}' is a function block type: declare an instance and call that", n.spelled),
                                 std::format("VAR t : {}; END_VAR … t(…);", n.spelled));
                        return nullptr;
                    }
                    d_.error("E_CALL", c.loc, std::format("PROGRAM '{}' cannot be called", n.spelled));
                    return nullptr;
                }
                if (const StdSpec* spec = findStd(n.upper)) return typeStdCall(c, *spec, ctx, statement, expected);
                d_.error("E_UNDECLARED", c.loc, std::format("'{}' is not declared", n.spelled), suggestCallable(n.upper));
                return nullptr;
            }
        }
        const Type* inst = typeExpr(c.callee, nullptr, ctx);
        if (!inst) return nullptr;
        const Type* b = baseOf(inst);
        if (b->kind != TypeKind::FunctionBlock) {
            std::string hint;
            if (c.callee->kind == ExprKind::Name) {
                const auto& n = c.callee->as<Name>();
                if (const PouInfo* f = p_.findPou(n.upper); f && f->kind == PouKind::Function)
                    hint = std::format("the variable '{}' hides the function {} in this block: rename the variable",
                                       n.symbol ? n.symbol->spelled : n.spelled, f->spelled);
            }
            d_.error("E_CALL", c.loc, std::format("{} cannot be called: it is {}", describeExpr(*c.callee), inst->name), hint);
            return nullptr;
        }
        if (!statement) {
            d_.error("E_CALL", c.loc, "a function block call is a statement, not a value",
                     "call it on its own line (t(…);) and then read its outputs (t.Q)");
            return nullptr;
        }
        return typeUserCall(c, *const_cast<PouInfo*>(b->pou), ctx, statement, true);
    }

    std::string suggestCallable(const std::string& upper) const {
        std::string best;
        size_t bestD = 3;
        for (const StdSpec& s : stdSpecs()) {
            if (s.name.starts_with("__")) continue;
            size_t d = editDistance(upper, s.name);
            if (d < bestD) { bestD = d; best = std::string(s.name); }
        }
        for (const auto& [name, p] : p_.pouByName) {
            if (p->kind != PouKind::Function) continue;
            size_t d = editDistance(upper, name);
            if (d < bestD) { bestD = d; best = p->spelled; }
        }
        return best.empty() ? std::string() : std::format("did you mean {}?", best);
    }

    const Type* typeUserCall(Call& c, PouInfo& pou, Ctx& ctx, bool statement, bool block) {
        noteCall(ctx, &pou, c.loc);
        c.callKind = block ? CallKind::Block : CallKind::Function;
        c.pou = &pou;
        c.argParam.assign(c.args.size(), nullptr);
        bool formal = false, informal = false;
        for (const Arg& a : c.args) (a.name.empty() ? informal : formal) = true;
        if (formal && informal) {
            d_.error("E_CALL", c.loc, "a call names all its parameters or none of them");
            return nullptr;
        }
        bool ok = true;
        if (informal) {
            if (block) {
                d_.error("E_CALL", c.loc, "function block calls name their parameters: t(IN := x, PT := T#1s)");
                return nullptr;
            }
            std::vector<Symbol*> params = pou.inputs;
            params.insert(params.end(), pou.inOuts.begin(), pou.inOuts.end());
            if (c.args.size() != params.size()) {
                if (hasFailed(pou)) return nullptr;  // a parameter's declaration has the error
                d_.error("E_CALL", c.loc,
                         std::format("{} takes {} input{} (VAR_INPUT then VAR_IN_OUT); {} given", pou.spelled,
                                     params.size(), params.size() == 1 ? "" : "s", c.args.size()),
                         "or name the parameters: f(x := 1, y := 2)");
                return nullptr;
            }
            for (size_t i = 0; i < c.args.size(); i++) ok = bindArg(c, i, *params[i], ctx) && ok;
        } else {
            std::set<std::string> seen;
            for (size_t i = 0; i < c.args.size(); i++) {
                Arg& a = c.args[i];
                if (!seen.insert(a.name).second) {
                    d_.error("E_CALL", a.loc, std::format("'{}' is given twice", a.spelled));
                    ok = false;
                    continue;
                }
                if (a.name == "EN") {
                    if (a.output) { d_.error("E_CALL", a.loc, "EN is an input: EN := condition"); ok = false; continue; }
                    c.enArg = static_cast<int>(i);
                    ok = typeExpr(a.value, p_.types.elementary(TypeKind::Bool), ctx) &&
                         coerce(a.value, p_.types.elementary(TypeKind::Bool), "use as EN") && ok;
                    continue;
                }
                if (a.name == "ENO") {
                    if (!a.output) { d_.error("E_CALL", a.loc, "ENO is an output: ENO => variable"); ok = false; continue; }
                    c.enoArg = static_cast<int>(i);
                    const Type* t = typeExpr(a.value, nullptr, ctx);
                    if (t && baseOf(t)->kind != TypeKind::Bool) {
                        d_.error("E_TYPE", a.loc, "ENO => needs a BOOL variable");
                        ok = false;
                    }
                    ok = t && checkWritable(*a.value, ctx, "set by ENO") && ok;
                    continue;
                }
                Symbol* p = pou.find(a.name);
                if (!p && failed_.count({&pou, a.name})) {
                    ok = false;  // the parameter's declaration has the error
                    continue;
                }
                if (!p || (p->kind != SymKind::Input && p->kind != SymKind::Output && p->kind != SymKind::InOut)) {
                    d_.error("E_CALL", a.loc, std::format("{} has no parameter '{}'", pou.spelled, a.spelled),
                             suggestParam(pou, a.name));
                    ok = false;
                    continue;
                }
                ok = bindArg(c, i, *p, ctx) && ok;
            }
            for (Symbol* io : pou.inOuts) {
                if (!seen.count(io->upper)) {
                    d_.error("E_CALL", c.loc, std::format("VAR_IN_OUT '{}' of {} must be passed", io->spelled, pou.spelled));
                    ok = false;
                }
            }
        }
        if (!ok) return nullptr;
        if (block) return p_.types.voidType();
        if (!statement && pou.result->kind == TypeKind::Void) {
            if (failed_.count({&pou, pou.upper})) return nullptr;  // its result type has the error
            d_.error("E_CALL", c.loc, std::format("{} returns no value", pou.spelled));
            return nullptr;
        }
        return pou.result;
    }

    std::string suggestParam(const PouInfo& pou, const std::string& upper) const {
        std::string list;
        for (const Symbol& s : pou.symbols) {
            if (s.kind == SymKind::Input || s.kind == SymKind::Output || s.kind == SymKind::InOut) {
                if (!list.empty()) list += ", ";
                list += s.spelled;
            }
        }
        (void)upper;
        return list.empty() ? std::string() : std::format("its parameters are {}", list);
    }

    bool bindArg(Call& c, size_t i, Symbol& p, Ctx& ctx) {
        Arg& a = c.args[i];
        c.argParam[i] = &p;
        if (p.kind == SymKind::Output) {
            if (!a.output) {
                d_.error("E_CALL", a.loc, std::format("'{}' is an output: write {} => variable", p.spelled, p.spelled));
                return false;
            }
            const Type* t = typeExpr(a.value, nullptr, ctx);
            if (!t || !checkWritable(*a.value, ctx, "set by an output")) return false;
            const Type* bt = baseOf(t);
            const Type* pt = baseOf(p.type);
            bool fits = sameType(pt, bt) || (pt->kind == TypeKind::String && bt->kind == TypeKind::String) ||
                        (isElementary(pt->kind) && isElementary(bt->kind) && implicitlyConvertible(pt->kind, bt->kind));
            if (!fits) {
                d_.error("E_TYPE", a.loc, std::format("output '{}' is {}; {} cannot hold it", p.spelled, p.type->name, t->name));
                return false;
            }
            return true;
        }
        if (a.output) {
            d_.error("E_CALL", a.loc, std::format("'{}' is an input: write {} := value", p.spelled, p.spelled));
            return false;
        }
        if (p.kind == SymKind::InOut) {
            const Type* t = typeExpr(a.value, nullptr, ctx);
            if (!t) return false;
            if (!checkWritable(*a.value, ctx, "passed as VAR_IN_OUT")) return false;
            bool identical = sameType(t, p.type) && t->hasRange == p.type->hasRange &&
                             (!t->hasRange || (t->rangeLow == p.type->rangeLow && t->rangeHigh == p.type->rangeHigh));
            if (!identical) {
                d_.error("E_TYPE", a.loc,
                         std::format("VAR_IN_OUT '{}' is {}; pass a variable of exactly that type, not {}", p.spelled,
                                     p.type->name, t->name),
                         "an in-out is passed by reference, so its range and capacity must match exactly");
                return false;
            }
            return true;
        }
        if (!typeExpr(a.value, p.type, ctx)) return false;
        return coerce(a.value, p.type, std::format("pass as '{}'", p.spelled));
    }

    // ── Standard functions ──

    const Type* typeStdCall(Call& c, const StdSpec& spec, Ctx& ctx, bool statement, const Type* expected) {
        (void)statement;
        if (spec.fn == StdFn::Now) {
            if (!ctx.pou || !ctx.pou->library) {
                d_.error("E_UNDECLARED", c.loc, "'__NOW' is internal to the standard library");
                return nullptr;
            }
            c.callKind = CallKind::Standard;
            c.standard = static_cast<int>(StdFn::Now);
            return p_.types.elementary(TypeKind::Time);
        }
        // Order the inputs: positional, or by formal name
        std::vector<ExprPtr*> in;
        bool formal = false, informal = false;
        for (const Arg& a : c.args) (a.name.empty() ? informal : formal) = true;
        if (formal && informal) {
            d_.error("E_CALL", c.loc, "a call names all its parameters or none of them");
            return nullptr;
        }
        for (const Arg& a : c.args) {
            if (a.name == "EN" || a.name == "ENO" || a.output) {
                d_.error("E_NOT_SUPPORTED", a.loc, std::format("EN/ENO and outputs on standard function {}", spec.name),
                         "check the inputs with IF before the call");
                return nullptr;
            }
        }
        if (informal) {
            for (Arg& a : c.args) in.push_back(&a.value);
        } else {
            // Formal: names in the order of the specification; extensible INn continue the numbering
            std::vector<std::string> names(spec.params.begin(), spec.params.end());
            if (spec.extensible) {
                std::string last(spec.params.back());
                std::string stem = last.substr(0, last.find_first_of("0123456789"));
                int first = std::stoi(last.substr(stem.size()));
                for (size_t k = names.size(); k < c.args.size(); k++) names.push_back(std::format("{}{}", stem, first + static_cast<int>(k - spec.params.size()) + 1));
            }
            in.assign(names.size(), nullptr);
            for (Arg& a : c.args) {
                auto it = std::find(names.begin(), names.end(), a.name);
                if (it == names.end()) {
                    d_.error("E_CALL", a.loc, std::format("{} has no parameter '{}'", spec.name, a.spelled));
                    return nullptr;
                }
                size_t k = static_cast<size_t>(it - names.begin());
                if (in[k]) {
                    d_.error("E_CALL", a.loc, std::format("'{}' is given twice", a.spelled));
                    return nullptr;
                }
                in[k] = &a.value;
            }
            for (size_t k = 0; k < in.size(); k++) {
                if (!in[k]) {
                    d_.error("E_CALL", c.loc, std::format("{} needs '{}'", spec.name, names[k]));
                    return nullptr;
                }
            }
            // Re-order the AST arguments into specification order for code generation
            std::vector<Arg> ordered;
            for (ExprPtr* p : in) {
                for (Arg& a : c.args) {
                    if (&a.value == p) {
                        ordered.push_back(std::move(a));
                        break;
                    }
                }
            }
            c.args = std::move(ordered);
            in.clear();
            for (Arg& a : c.args) in.push_back(&a.value);
        }
        size_t need = spec.params.size();
        if (in.size() < need || (!spec.extensible && in.size() != need)) {
            d_.error("E_CALL", c.loc, std::format("{} takes {}{} input{}", spec.name, need,
                                                  spec.extensible ? " or more" : "", need == 1 ? "" : "s"));
            return nullptr;
        }
        c.callKind = CallKind::Standard;
        c.standard = static_cast<int>(spec.fn);

        const Type* lint = p_.types.elementary(TypeKind::LInt);
        auto typed = [&](ExprPtr& x, const Type* exp) { return typeExpr(x, exp, ctx); };
        // A literal argument takes the type the result is used as (r := SIN(0.5) is REAL), else LREAL
        const Type* realContext = expected && isReal(baseOf(expected)->kind) ? baseOf(expected) : p_.types.elementary(TypeKind::LReal);
        auto real1 = [&](ExprPtr& x) -> const Type* {
            const Type* t = typed(x, isFlexible(*x) ? realContext : nullptr);
            if (!t) return nullptr;
            if (!isReal(baseOf(t)->kind)) {
                d_.error("E_TYPE", x->loc, std::format("{} needs REAL or LREAL, not {}", spec.name, t->name),
                         std::format("convert it, e.g. {}_TO_LREAL(…)", t->name));
                return nullptr;
            }
            return baseOf(t);
        };
        auto integerTo = [&](ExprPtr& x, std::string_view what) -> bool {
            const Type* t = typed(x, lint);
            if (!t) return false;
            if (!isInteger(baseOf(t)->kind)) {
                d_.error("E_TYPE", x->loc, std::format("{} of {} must be an integer, not {}", what, spec.name, t->name));
                return false;
            }
            if (baseOf(t)->kind != TypeKind::LInt) wrapConvert(x, lint, baseOf(t)->kind != TypeKind::ULInt, false);
            return true;
        };
        auto string1 = [&](ExprPtr& x) -> const Type* {
            const Type* t = typed(x, nullptr);
            if (!t) return nullptr;
            if (baseOf(t)->kind != TypeKind::String) {
                d_.error("E_TYPE", x->loc, std::format("{} needs a STRING, not {}", spec.name, t->name));
                return nullptr;
            }
            return baseOf(t);
        };
        auto commonOf = [&](std::vector<ExprPtr*> xs, std::string_view kinds) -> const Type* {
            const Type* common = nullptr;
            for (ExprPtr* x : xs) {
                if (isFlexible(**x)) continue;
                const Type* t = typed(*x, nullptr);
                if (!t) return nullptr;
                if (!common) {
                    common = baseOf(t);
                } else {
                    const Type* n = commonType(common, t);
                    if (!n) {
                        d_.error("E_TYPE", (*x)->loc, std::format("the inputs of {} have no common type ({} and {})", spec.name, common->name, t->name));
                        return nullptr;
                    }
                    common = n;
                }
            }
            if (!common) {
                // Only literals: they take the type the result is used as, else DINT
                const Type* want = expected ? baseOf(expected) : nullptr;
                common = want && isElementary(want->kind) && want->kind != TypeKind::Bool && !isDuration(want->kind)
                             ? want : p_.types.elementary(TypeKind::DInt);
            }
            for (ExprPtr* x : xs) {
                if (isFlexible(**x)) {
                    if (!typed(*x, common)) return nullptr;
                } else if (!sameType(baseOf((*x)->type), common)) {
                    wrapConvert(*x, common, true);
                }
            }
            TypeKind k = common->kind;
            bool ok = (kinds.find('n') != std::string_view::npos && isNumeric(k)) ||
                      (kinds.find('d') != std::string_view::npos && isDuration(k)) ||
                      (kinds.find('b') != std::string_view::npos && isBitString(k)) ||
                      (kinds.find('B') != std::string_view::npos && k == TypeKind::Bool) ||
                      (kinds.find('s') != std::string_view::npos && k == TypeKind::String);
            if (!ok) {
                d_.error("E_TYPE", c.loc, std::format("{} is not defined for {}", spec.name, common->name));
                return nullptr;
            }
            return common;
        };

        switch (spec.fn) {
            case StdFn::Abs: {
                // ABS(-2.5) into a REAL: the literal takes the context, as the result has its argument's type
                const Type* t = typed(*in[0], expected && isNumeric(baseOf(expected)->kind) ? expected : nullptr);
                if (!t) return nullptr;
                if (!isNumeric(baseOf(t)->kind)) {
                    d_.error("E_TYPE", (*in[0])->loc, std::format("ABS needs a number, not {}", t->name));
                    return nullptr;
                }
                c.operandType = baseOf(t);
                return baseOf(t);
            }
            case StdFn::Sqrt: case StdFn::Ln: case StdFn::Log: case StdFn::Exp: case StdFn::Sin: case StdFn::Cos:
            case StdFn::Tan: case StdFn::Asin: case StdFn::Acos: case StdFn::Atan: {
                const Type* t = real1(*in[0]);
                c.operandType = t;
                return t;
            }
            case StdFn::Atan2: {
                const Type* y = real1(*in[0]);
                const Type* x = y ? real1(*in[1]) : nullptr;
                if (!x) return nullptr;
                const Type* t = commonType(y, x);
                if (!sameType(baseOf((*in[0])->type), t)) wrapConvert(*in[0], t, true);
                if (!sameType(baseOf((*in[1])->type), t)) wrapConvert(*in[1], t, true);
                c.operandType = t;
                return t;
            }
            case StdFn::FirstCycle:
                return p_.types.elementary(TypeKind::Bool);
            case StdFn::Time:
                return p_.types.elementary(TypeKind::Time);
            case StdFn::LTime:
                return p_.types.elementary(TypeKind::LTime);
            case StdFn::Expt: {
                const Type* base = real1(*in[0]);
                if (!base) return nullptr;
                const Type* et = typed(*in[1], base);
                if (!et) return nullptr;
                if (!isNumeric(baseOf(et)->kind)) {
                    d_.error("E_TYPE", (*in[1])->loc, "the exponent of EXPT must be a number");
                    return nullptr;
                }
                if (!sameType(baseOf(et), base)) wrapConvert(*in[1], base, false);
                c.operandType = base;
                return base;
            }
            case StdFn::Shl: case StdFn::Shr: case StdFn::Rol: case StdFn::Ror: {
                const Type* t = typed(*in[0], nullptr);
                if (!t) return nullptr;
                if (!isBitString(baseOf(t)->kind)) {
                    d_.error("E_TYPE", (*in[0])->loc, std::format("{} needs a bit string (BYTE…LWORD), not {}", spec.name, t->name),
                             isFlexible(**in[0]) ? "write a typed literal, e.g. WORD#16#00FF" : "");
                    return nullptr;
                }
                // A ULINT count stays ULINT (the VM reads it unsigned): converting 2^63 or more to LINT would fault
                const Type* nt = typed(*in[1], lint);
                if (!nt) return nullptr;
                if (baseOf(nt)->kind != TypeKind::ULInt && !integerTo(*in[1], "N")) return nullptr;
                c.operandType = baseOf(t);
                return baseOf(t);
            }
            case StdFn::Sel: {
                const Type* g = typed(*in[0], p_.types.elementary(TypeKind::Bool));
                if (!g || !coerce(*in[0], p_.types.elementary(TypeKind::Bool), "use as G of SEL")) return nullptr;
                const Type* t = commonOf({in[1], in[2]}, "ndbBs");
                c.operandType = t;
                return t;
            }
            case StdFn::Max: case StdFn::Min: {
                std::vector<ExprPtr*> xs(in.begin(), in.end());
                bool strings = true;
                for (ExprPtr* x : xs) {
                    const Type* t = isFlexible(**x) ? nullptr : typed(*x, nullptr);
                    if (!t || baseOf(t)->kind != TypeKind::String) strings = false;
                }
                if (strings) {
                    uint32_t cap = 1;
                    for (ExprPtr* x : xs) cap = std::max(cap, baseOf((*x)->type)->capacity);
                    c.operandType = p_.types.string(cap);
                    return c.operandType;
                }
                const Type* t = commonOf(xs, "ndb");
                c.operandType = t;
                return t;
            }
            case StdFn::Limit: {
                const Type* t = commonOf({in[0], in[1], in[2]}, "ndb");
                c.operandType = t;
                return t;
            }
            case StdFn::Mux: {
                if (!integerTo(*in[0], "K")) return nullptr;
                if (Value k; evaluateConstant(**in[0], k) && (k.i < 0 || static_cast<size_t>(k.i) >= in.size() - 1)) {
                    d_.error("E_INDEX", (*in[0])->loc,
                             std::format("MUX selects input {} of {} (0…{})", k.i, in.size() - 1, in.size() - 2));
                    return nullptr;
                }
                std::vector<ExprPtr*> xs(in.begin() + 1, in.end());
                const Type* t = commonOf(xs, "ndbBs");
                c.operandType = t;
                return t;
            }
            case StdFn::Gt: case StdFn::Ge: case StdFn::Eq: case StdFn::Le: case StdFn::Lt: case StdFn::Ne: {
                std::vector<ExprPtr*> xs(in.begin(), in.end());
                bool strings = true;
                for (ExprPtr* x : xs) {
                    const Type* t = isFlexible(**x) ? nullptr : typed(*x, nullptr);
                    if (!t || baseOf(t)->kind != TypeKind::String) strings = false;
                }
                if (strings) {
                    const Type* widest = nullptr;
                    for (ExprPtr* x : xs) {
                        if (!widest || baseOf((*x)->type)->capacity > widest->capacity) widest = baseOf((*x)->type);
                    }
                    c.operandType = widest;
                } else {
                    const Type* t = commonOf(xs, "ndbB");
                    if (!t) return nullptr;
                    c.operandType = t;
                }
                return p_.types.elementary(TypeKind::Bool);
            }
            case StdFn::Len: {
                // INT, or DINT when the string can be longer than INT holds (STRING[n] up to 65535)
                const Type* s = string1(*in[0]);
                if (!s) return nullptr;
                return p_.types.elementary(s->capacity > 32767 ? TypeKind::DInt : TypeKind::Int);
            }
            case StdFn::Left: case StdFn::Right: {
                const Type* s = string1(*in[0]);
                if (!s || !integerTo(*in[1], "L")) return nullptr;
                return s;
            }
            case StdFn::Mid: {
                const Type* s = string1(*in[0]);
                if (!s || !integerTo(*in[1], "L") || !integerTo(*in[2], "P")) return nullptr;
                return s;
            }
            case StdFn::Concat: {
                uint64_t cap = 0;
                for (ExprPtr* x : in) {
                    const Type* s = string1(*x);
                    if (!s) return nullptr;
                    cap += s->capacity;
                }
                return p_.types.string(static_cast<uint32_t>(std::min<uint64_t>(cap, TypeTable::kMaxStringLength)));
            }
            case StdFn::Insert: {
                const Type* a = string1(*in[0]);
                const Type* b = a ? string1(*in[1]) : nullptr;
                if (!b || !integerTo(*in[2], "P")) return nullptr;
                return p_.types.string(std::min<uint32_t>(a->capacity + b->capacity, TypeTable::kMaxStringLength));
            }
            case StdFn::Delete: {
                const Type* s = string1(*in[0]);
                if (!s || !integerTo(*in[1], "L") || !integerTo(*in[2], "P")) return nullptr;
                return s;
            }
            case StdFn::Replace: {
                const Type* a = string1(*in[0]);
                const Type* b = a ? string1(*in[1]) : nullptr;
                if (!b || !integerTo(*in[2], "L") || !integerTo(*in[3], "P")) return nullptr;
                return p_.types.string(std::min<uint32_t>(a->capacity + b->capacity, TypeTable::kMaxStringLength));
            }
            case StdFn::Find: {
                const Type* s = string1(*in[0]);
                if (!s || !string1(*in[1])) return nullptr;
                return p_.types.elementary(s->capacity > 32767 ? TypeKind::DInt : TypeKind::Int);
            }
            default:
                break;
        }
        return nullptr;
    }

    Program& p_;
    const std::vector<TagDef>& tags_;
    Diagnostics& d_;
    bool library_ = false;  ///< files[0] is a library file: it has no PROGRAM
    std::map<std::string, TypeDecl*> typeDecls_;
    std::set<std::string> standardBlocks_;  // names of the standard library's function blocks
    std::set<std::pair<PouInfo*, std::string>> failed_;  // variables whose declaration was refused
    /// Whether a parameter or variable of `pou` was refused (its result type, filed under its own name, is not one).
    bool hasFailed(PouInfo& pou) const {
        for (auto it = failed_.lower_bound({&pou, std::string()}); it != failed_.end() && it->first == &pou; ++it) {
            if (it->second != pou.upper) return true;
        }
        return false;
    }
    std::set<std::string> inProgress_;
    std::set<PouInfo*> declaring_;
    /// A POU being declared: its VAR CONSTANT declarations by name, which declarations are done or under way.
    struct DeclState {
        int order = 0;
        std::map<std::string, std::pair<VarSection*, VarDecl*>> constants;
        std::set<const VarDecl*> done, inProgress;
    };
    std::map<PouInfo*, DeclState> declStates_;
    std::map<std::pair<PouInfo*, PouInfo*>, SourceLoc> callLoc_;
    PouInfo globals_;  ///< VAR_GLOBAL CONSTANT
};

}  // namespace

bool analyze(Program& program, const std::vector<SourceFile>& files, const std::vector<TagDef>& tags,
             Diagnostics& diags, bool library) {
    return Analyzer(program, tags, diags, library).run(files);
}

std::string nameProblem(std::string_view name) {
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (name.empty()) return "a name cannot be empty";
    if (!letter(name[0]) && name[0] != '_') return std::format("'{}' must start with a letter or _", name);
    for (char c : name) {
        if (!letter(c) && c != '_' && !(c >= '0' && c <= '9')) {
            return std::format("'{}' may use only letters, digits and _", name);
        }
    }
    // The lexer's rules (checkIdentifier) and the parser's and sema's reserved names, in that order
    if (name.find("__") != std::string_view::npos) return std::format("'{}' has two underscores in a row", name);
    if (name.back() == '_') return std::format("'{}' ends with '_'", name);
    if (name.size() > 64) return std::format("'{}' is longer than 64 characters", name);
    std::string upper = toUpper(name);
    if (keywordKind(upper) != Tok::Identifier) return std::format("'{}' is a keyword or reserved word of Structured Text", name);
    static const TypeTable types;
    static const std::set<std::string> blocks = [] {
        std::set<std::string> names;
        Diagnostics ignored;
        for (const auto& pou : parse(standardLibrarySource(), 0, ignored, true).pous) {
            if (!pou->upper.empty() && !pou->upper.starts_with("__")) names.insert(pou->upper);
        }
        return names;
    }();
    if (types.elementaryByName(upper) || upper == "STRING" || isConversionNameIn(types, upper)) {
        return std::format("'{}' is the name of a standard type or conversion", name);
    }
    if (findStd(upper) || isOperatorFunction(upper) || blocks.count(upper)) {
        return std::format("'{}' is the name of a standard function or function block", name);
    }
    return {};
}

}  // namespace simpleplc::st
