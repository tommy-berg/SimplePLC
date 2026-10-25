// The SimplePLC virtual machine. Executes verified images (vm/image.h): every
// direct operand is known to lie inside its segment, so the only checks here are
// the ones the language defines (overflow, division by zero, indexes, strings)
// and references computed at run time. Compiled with -ffp-contract=off so REAL
// arithmetic is the same on every platform.

#include "vm/vm.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

#include "vm/duration.h"
#include "vm/fmath.h"

namespace simpleplc::vm {

namespace {

using st::isBitString;
using st::isDuration;
using st::isReal;
using st::isSigned;
using st::isUnsigned;

inline uint32_t sizeOf(TypeKind k) {
    switch (k) {
        case TypeKind::Bool: case TypeKind::SInt: case TypeKind::USInt: case TypeKind::Byte: return 1;
        case TypeKind::Int: case TypeKind::UInt: case TypeKind::Word: return 2;
        case TypeKind::DInt: case TypeKind::UDInt: case TypeKind::DWord: case TypeKind::Real: return 4;
        default: return 8;
    }
}

inline bool signedLike(TypeKind k) { return isSigned(k) || isDuration(k); }

inline int64_t getS(TypeKind k, const uint8_t* p) {
    switch (sizeOf(k)) {
        case 1: { int8_t v; std::memcpy(&v, p, 1); return v; }
        case 2: { int16_t v; std::memcpy(&v, p, 2); return v; }
        case 4: { int32_t v; std::memcpy(&v, p, 4); return v; }
        default: { int64_t v; std::memcpy(&v, p, 8); return v; }
    }
}
inline uint64_t getU(TypeKind k, const uint8_t* p) {
    switch (sizeOf(k)) {
        case 1: return p[0];
        case 2: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        default: { uint64_t v; std::memcpy(&v, p, 8); return v; }
    }
}
inline void put(TypeKind k, uint8_t* p, uint64_t v) {
    switch (sizeOf(k)) {
        case 1: p[0] = static_cast<uint8_t>(v); break;
        case 2: { uint16_t x = static_cast<uint16_t>(v); std::memcpy(p, &x, 2); break; }
        case 4: { uint32_t x = static_cast<uint32_t>(v); std::memcpy(p, &x, 4); break; }
        default: std::memcpy(p, &v, 8); break;
    }
}
inline float getF(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
inline double getD(const uint8_t* p) { double d; std::memcpy(&d, p, 8); return d; }
inline void putF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
inline void putD(uint8_t* p, double d) { std::memcpy(p, &d, 8); }
inline uint32_t getU32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void putU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline int64_t getI64(const uint8_t* p) { int64_t v; std::memcpy(&v, p, 8); return v; }
inline void putI64(uint8_t* p, int64_t v) { std::memcpy(p, &v, 8); }

inline Scalar getScalar(TypeKind k, const uint8_t* p) {
    Scalar s;
    if (k == TypeKind::Real) s.real = static_cast<double>(getF(p));
    else if (k == TypeKind::LReal) s.real = getD(p);
    else if (signedLike(k)) s.bits = static_cast<uint64_t>(getS(k, p));
    else s.bits = getU(k, p);
    return s;
}
inline void putScalar(TypeKind k, uint8_t* p, const Scalar& s) {
    if (k == TypeKind::Real) putF(p, static_cast<float>(s.real));
    else if (k == TypeKind::LReal) putD(p, s.real);
    else put(k, p, s.bits);
}

/// Three-way comparison of two scalars of kind k.
inline int compare(TypeKind k, const uint8_t* a, const uint8_t* b) {
    if (k == TypeKind::Real) { float x = getF(a), y = getF(b); return x < y ? -1 : x > y ? 1 : 0; }
    if (k == TypeKind::LReal) { double x = getD(a), y = getD(b); return x < y ? -1 : x > y ? 1 : 0; }
    if (signedLike(k)) { int64_t x = getS(k, a), y = getS(k, b); return x < y ? -1 : x > y ? 1 : 0; }
    uint64_t x = getU(k, a), y = getU(k, b);
    return x < y ? -1 : x > y ? 1 : 0;
}

inline bool cmpResult(int c, uint8_t kind) {
    switch (static_cast<CmpKind>(kind)) {
        case CmpKind::Eq: return c == 0;
        case CmpKind::Ne: return c != 0;
        case CmpKind::Lt: return c < 0;
        case CmpKind::Le: return c <= 0;
        case CmpKind::Gt: return c > 0;
        case CmpKind::Ge: return c >= 0;
    }
    return false;
}

// ── Strings: [u16 length][capacity bytes] ──

inline bool strLen(const uint8_t* slot, uint32_t cap, uint32_t& len) {
    uint16_t l;
    std::memcpy(&l, slot, 2);
    len = l;
    return len <= cap;
}
inline void setLen(uint8_t* slot, uint32_t len) {
    uint16_t l = static_cast<uint16_t>(len);
    std::memcpy(slot, &l, 2);
}

/// Text of a scalar into buf (no allocation); returns its length.
size_t formatScalar(TypeKind k, const uint8_t* p, char* buf, size_t size) {
    char* end = buf + size;
    if (k == TypeKind::Bool) {
        const char* t = p[0] ? "TRUE" : "FALSE";
        size_t n = std::strlen(t);
        std::memcpy(buf, t, n);
        return n;
    }
    if (k == TypeKind::Real) return static_cast<size_t>(std::to_chars(buf, end, getF(p)).ptr - buf);
    if (k == TypeKind::LReal) return static_cast<size_t>(std::to_chars(buf, end, getD(p)).ptr - buf);
    if (isDuration(k)) {
        int64_t ns = getS(k, p);
        char* o = buf;
        auto lit = [&](const char* s) { size_t n = std::strlen(s); std::memcpy(o, s, n); o += n; };
        lit(k == TypeKind::Time ? "T#" : "LT#");
        uint64_t v = 0;
        if (ns < 0) {
            *o++ = '-';
            v = static_cast<uint64_t>(-(ns + 1)) + 1;
        } else {
            v = static_cast<uint64_t>(ns);
        }
        if (v == 0) {
            lit("0s");
            return static_cast<size_t>(o - buf);
        }
        struct U { const char* n; uint64_t ns; };
        static constexpr U units[] = {{"d", 86400000000000ULL}, {"h", 3600000000000ULL}, {"m", 60000000000ULL},
                                      {"s", 1000000000ULL}, {"ms", 1000000ULL}, {"us", 1000ULL}, {"ns", 1ULL}};
        for (const U& u : units) {
            if (v >= u.ns) {
                o = std::to_chars(o, end, v / u.ns).ptr;
                lit(u.n);
                v %= u.ns;
            }
        }
        return static_cast<size_t>(o - buf);
    }
    if (signedLike(k)) return static_cast<size_t>(std::to_chars(buf, end, getS(k, p)).ptr - buf);
    return static_cast<size_t>(std::to_chars(buf, end, getU(k, p)).ptr - buf);
}

bool equalsNoCase(const char* s, size_t n, const char* word) {
    size_t m = std::strlen(word);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != word[i]) return false;
    }
    return true;
}

bool parseScalar(TypeKind k, const char* s, size_t n, uint8_t* dst) {
    const char* end = s + n;
    if (n == 0) return false;
    if (k == TypeKind::Bool) {
        if (equalsNoCase(s, n, "TRUE") || (n == 1 && s[0] == '1')) { dst[0] = 1; return true; }
        if (equalsNoCase(s, n, "FALSE") || (n == 1 && s[0] == '0')) { dst[0] = 0; return true; }
        return false;
    }
    if (isReal(k)) {
        double d = 0;
        auto r = std::from_chars(s, end, d);
        if (r.ec != std::errc() || r.ptr != end || !std::isfinite(d)) return false;
        if (k == TypeKind::Real) {
            float f = static_cast<float>(d);
            if (!std::isfinite(f)) return false;
            putF(dst, f);
        } else {
            putD(dst, d);
        }
        return true;
    }
    if (isDuration(k)) {
        // The compiler's own duration rules (vm/duration.h): the text of a literal converts to the
        // literal's value, exactly, and what a literal refuses is F_CONVERSION
        int64_t ns = 0;
        if (!parseDurationText(std::string_view(s, n), ns)) return false;
        putI64(dst, ns);
        return true;
    }
    if (signedLike(k)) {
        if (s[0] == '+') { s++; if (s == end) return false; }
        int64_t v = 0;
        auto r = std::from_chars(s, end, v);
        if (r.ec != std::errc() || r.ptr != end || !fitsSigned(k, v)) return false;
        put(k, dst, static_cast<uint64_t>(v));
        return true;
    }
    if (s[0] == '+') { s++; if (s == end) return false; }
    uint64_t v = 0;
    auto r = std::from_chars(s, end, v);
    if (r.ec != std::errc() || r.ptr != end || v > st::maxValue(k)) return false;
    put(k, dst, v);
    return true;
}

}  // namespace

Machine::Machine(std::shared_ptr<const Image> image) : image_(std::move(image)) {
    storage_.assign((image_->blockSize + 7) / 8 + 1, 0);
    data_ = reinterpret_cast<uint8_t*>(storage_.data());
    frames_.reserve(image_->pous.size() + 2);
    size_t tries = 0;
    for (const Insn& in : image_->code) tries += in.op == Op::Try ? 1 : 0;
    catches_.reserve(tries + 1);
    dirty_.assign(image_->bindings.size() + 1, 0);
    reset();
}

void Machine::reset(bool keepRetain) {
    // RETAIN variables of a warm start keep their bytes (the variable table lists them, FB members
    // and array elements included). A restart, not a cycle: it may allocate.
    std::vector<std::pair<const VarEntry*, std::vector<uint8_t>>> kept;
    if (keepRetain) {
        for (const VarEntry& v : image_->vars) {
            if ((v.flags & kVarRetain) && v.offset + v.size <= image_->blockSize) {
                kept.push_back({&v, std::vector<uint8_t>(data_ + v.offset, data_ + v.offset + v.size)});
            }
        }
    }
    std::memset(data_, 0, image_->blockSize);
    std::memcpy(data_, image_->initialData.data(), image_->initialData.size());
    for (const auto& [v, bytes] : kept) std::memcpy(data_ + v->offset, bytes.data(), bytes.size());
}

RunResult Machine::cycle(int64_t nowNs, const RunLimits& limits) {
    const Image& img = *image_;
    const Insn* code = img.code.data();
    const uint8_t* constants = img.constants.data();
    uint8_t* d = data_;
    const uint32_t block = img.blockSize;

    uint32_t pou = img.entry;
    uint32_t pc = img.pous[pou].codeBegin;
    uint32_t selfBase = img.instanceOffset;
    uint32_t frameBase = img.stackOffset;
    uint32_t frameSize = img.pous[pou].frameSize;
    frames_.clear();
    catches_.clear();

    RunResult res;
    const bool firstCycle = firstCycle_;
    firstCycle_ = false;
    uint64_t executed = 0;
    uint64_t nextClockCheck = 4096;
    uint32_t faultPc = 0;
    Fault fault = Fault::None;

    auto A = [&](uint32_t op) -> uint8_t* {
        uint32_t off = offsetOf(op);
        switch (segOf(op)) {
            case Seg::Const: return const_cast<uint8_t*>(constants + off);
            case Seg::Global: return d + off;
            case Seg::Self: return d + selfBase + off;
            case Seg::Frame: return d + frameBase + off;
            case Seg::Next: return d + frameBase + frameSize + off;
        }
        return d;
    };
    auto inBlock = [&](uint64_t ref, uint64_t size) { return ref + size <= block; };

    while (true) {
        const Insn& in = code[pc];
        uint32_t here = pc;
        pc++;
        executed++;
        if (executed >= nextClockCheck) {
            nextClockCheck = executed + 4096;
            if (executed > limits.budget) {
                fault = Fault::Budget;
                faultPc = here;
                goto handleFault;
            }
            if (limits.deadline != std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() > limits.deadline) {
                fault = Fault::Watchdog;
                faultPc = here;
                goto handleFault;
            }
        }
        {
            TypeKind k = static_cast<TypeKind>(in.type);
            switch (in.op) {
                case Op::Nop:
                    break;
                // Copies cost one more unit per 8 bytes, so whole arrays, by-value inputs and ENO
                // snapshots count toward the budget like the loops they replace
                case Op::Mov:
                    std::memmove(A(in.a), A(in.b), in.d);
                    executed += in.d >> 3;
                    break;
                case Op::Zero:
                    std::memset(A(in.a), 0, in.d);
                    executed += in.d >> 3;
                    break;
                case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
                    char op = in.op == Op::Add ? '+' : in.op == Op::Sub ? '-' : in.op == Op::Mul ? '*'
                              : in.op == Op::Div ? '/' : '%';
                    uint8_t* a = A(in.a);
                    const uint8_t* b = A(in.b);
                    const uint8_t* c = A(in.c);
                    Fault f = Fault::None;
                    if (k == TypeKind::Real) {
                        float r = 0;
                        f = realOp(op, getF(b), getF(c), r);
                        if (f == Fault::None) putF(a, r);
                    } else if (k == TypeKind::LReal) {
                        double r = 0;
                        f = realOp(op, getD(b), getD(c), r);
                        if (f == Fault::None) putD(a, r);
                    } else if (signedLike(k)) {
                        int64_t r = 0;
                        f = signedOp(op, k, getS(k, b), getS(k, c), r);
                        if (f == Fault::None) put(k, a, static_cast<uint64_t>(r));
                    } else {
                        uint64_t r = 0;
                        f = unsignedOp(op, k, getU(k, b), getU(k, c), r);
                        if (f == Fault::None) put(k, a, r);
                    }
                    if (f != Fault::None) { fault = f; faultPc = here; goto handleFault; }
                    break;
                }
                case Op::Neg: {
                    uint8_t* a = A(in.a);
                    const uint8_t* b = A(in.b);
                    if (k == TypeKind::Real) putF(a, -getF(b));
                    else if (k == TypeKind::LReal) putD(a, -getD(b));
                    else {
                        int64_t r = 0;
                        if (signedOp('-', k, 0, getS(k, b), r) != Fault::None) { fault = Fault::Overflow; faultPc = here; goto handleFault; }
                        put(k, a, static_cast<uint64_t>(r));
                    }
                    break;
                }
                case Op::Abs: {
                    uint8_t* a = A(in.a);
                    const uint8_t* b = A(in.b);
                    if (k == TypeKind::Real) putF(a, std::fabs(getF(b)));
                    else if (k == TypeKind::LReal) putD(a, std::fabs(getD(b)));
                    else if (signedLike(k)) {
                        int64_t v = getS(k, b);
                        int64_t r = v;
                        if (v < 0 && signedOp('-', k, 0, v, r) != Fault::None) { fault = Fault::Overflow; faultPc = here; goto handleFault; }
                        put(k, a, static_cast<uint64_t>(r));
                    } else {
                        std::memmove(a, b, sizeOf(k));
                    }
                    break;
                }
                case Op::Pow: {
                    uint8_t* a = A(in.a);
                    if (k == TypeKind::Real) {
                        float x = getF(A(in.b)), y = getF(A(in.c));
                        float r = fmath::pow(x, y);
                        if (std::isnan(r)) { fault = Fault::Domain; faultPc = here; goto handleFault; }
                        if (!std::isfinite(r)) { fault = Fault::Real; faultPc = here; goto handleFault; }
                        putF(a, r);
                    } else {
                        double x = getD(A(in.b)), y = getD(A(in.c));
                        double r = fmath::pow(x, y);
                        if (std::isnan(r)) { fault = Fault::Domain; faultPc = here; goto handleFault; }
                        if (!std::isfinite(r)) { fault = Fault::Real; faultPc = here; goto handleFault; }
                        putD(a, r);
                    }
                    break;
                }
                case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                    int c = compare(k, A(in.b), A(in.c));
                    uint8_t kind = static_cast<uint8_t>(static_cast<int>(in.op) - static_cast<int>(Op::Eq));
                    *A(in.a) = cmpResult(c, kind) ? 1 : 0;
                    break;
                }
                case Op::And: case Op::Or: case Op::Xor: {
                    uint64_t x = getU(k, A(in.b)), y = getU(k, A(in.c));
                    uint64_t r = in.op == Op::And ? (x & y) : in.op == Op::Or ? (x | y) : (x ^ y);
                    put(k, A(in.a), r);
                    break;
                }
                case Op::Not: {
                    uint64_t x = getU(k, A(in.b));
                    put(k, A(in.a), k == TypeKind::Bool ? (x ? 0 : 1) : (~x & st::maxValue(k)));
                    break;
                }
                case Op::Shl: case Op::Shr: case Op::Rol: case Op::Ror: {
                    // flags 1: the count is ULINT (any value is valid); else LINT, where a negative count is F_DOMAIN
                    uint64_t n = static_cast<uint64_t>(getI64(A(in.c)));
                    if (!(in.flags & 1) && static_cast<int64_t>(n) < 0) { fault = Fault::Domain; faultPc = here; goto handleFault; }
                    char op = in.op == Op::Shl ? 'l' : in.op == Op::Shr ? 'r' : in.op == Op::Rol ? 'L' : 'R';
                    put(k, A(in.a), shiftOp(op, sizeOf(k) * 8, getU(k, A(in.b)), n));
                    break;
                }
                case Op::Min: case Op::Max: {
                    const uint8_t* b = A(in.b);
                    const uint8_t* c = A(in.c);
                    int cmp = compare(k, b, c);
                    const uint8_t* pick = (in.op == Op::Max) == (cmp >= 0) ? b : c;
                    std::memmove(A(in.a), pick, sizeOf(k));
                    break;
                }
                case Op::Conv: {
                    TypeKind to = static_cast<TypeKind>(in.d);
                    Scalar out;
                    Fault f = convertScalar(k, getScalar(k, A(in.b)), to, (in.flags & kConvTruncate) != 0, out);
                    if (f != Fault::None) { fault = f; faultPc = here; goto handleFault; }
                    putScalar(to, A(in.a), out);
                    break;
                }
                case Op::TimeMul: case Op::TimeDiv: {
                    int64_t t = getI64(A(in.b));
                    TypeKind nk = static_cast<TypeKind>(in.d);
                    int64_t r = 0;
                    Fault f = Fault::None;
                    if (isReal(nk)) {
                        double n = nk == TypeKind::Real ? static_cast<double>(getF(A(in.c))) : getD(A(in.c));
                        f = durationScale(in.op == Op::TimeMul ? '*' : '/', t, n, r);
                    } else {
                        Scalar s = getScalar(nk, A(in.c));
                        bool uns = isUnsigned(nk);
                        if (uns && s.bits > static_cast<uint64_t>(INT64_MAX)) {
                            f = Fault::Overflow;
                        } else {
                            int64_t n = static_cast<int64_t>(s.bits);
                            f = signedOp(in.op == Op::TimeMul ? '*' : '/', TypeKind::LInt, t, n, r);
                        }
                    }
                    if (f != Fault::None) { fault = f; faultPc = here; goto handleFault; }
                    putI64(A(in.a), r);
                    break;
                }
                case Op::Math: {
                    double x = k == TypeKind::Real ? static_cast<double>(getF(A(in.b))) : getD(A(in.b));
                    double r = 0;
                    bool domain = false;
                    switch (static_cast<MathFn>(in.flags)) {
                        case MathFn::Sqrt: domain = x < 0; r = std::sqrt(x); break;
                        case MathFn::Ln: domain = x <= 0; r = fmath::log(x); break;
                        case MathFn::Log: domain = x <= 0; r = fmath::log10(x); break;
                        case MathFn::Exp: r = fmath::exp(x); break;
                        case MathFn::Sin: r = fmath::sin(x); break;
                        case MathFn::Cos: r = fmath::cos(x); break;
                        case MathFn::Tan: r = fmath::tan(x); break;
                        case MathFn::Asin: domain = x < -1 || x > 1; r = fmath::asin(x); break;
                        case MathFn::Acos: domain = x < -1 || x > 1; r = fmath::acos(x); break;
                        case MathFn::Atan: r = fmath::atan(x); break;
                    }
                    // REAL: the double result rounded once to float, as fmath's REAL functions
                    // (for SQRT that is exactly sqrtf's correctly rounded result)
                    if (k == TypeKind::Real) r = static_cast<double>(static_cast<float>(r));
                    if (domain || std::isnan(r)) { fault = Fault::Domain; faultPc = here; goto handleFault; }
                    if (!std::isfinite(r)) { fault = Fault::Real; faultPc = here; goto handleFault; }
                    if (k == TypeKind::Real) putF(A(in.a), static_cast<float>(r));
                    else putD(A(in.a), r);
                    break;
                }
                case Op::GetBit:
                    *A(in.a) = (getU(k, A(in.b)) >> in.d) & 1 ? 1 : 0;
                    break;
                case Op::SetBit: {
                    uint8_t* a = A(in.a);
                    uint64_t v = getU(k, a);
                    uint64_t m = uint64_t{1} << in.d;
                    v = *A(in.b) ? (v | m) : (v & ~m);
                    put(k, a, v);
                    break;
                }
                case Op::Addr:
                    putU32(A(in.a), static_cast<uint32_t>(A(in.b) - d));
                    break;
                case Op::RefAdd:
                    putU32(A(in.a), getU32(A(in.b)) + in.d);
                    break;
                case Op::Index: {
                    IndexDesc dsc;
                    std::memcpy(&dsc, constants + in.d, sizeof dsc);
                    int64_t idx = getI64(A(in.c));
                    int64_t rel = 0;
                    if (__builtin_sub_overflow(idx, dsc.low, &rel) || rel < 0 || static_cast<uint64_t>(rel) >= dsc.count) {
                        fault = Fault::Index;
                        faultPc = here;
                        goto handleFault;
                    }
                    uint64_t ref = uint64_t{getU32(A(in.b))} + static_cast<uint64_t>(rel) * dsc.stride;
                    if (ref > block) { fault = Fault::Reference; faultPc = here; goto handleFault; }
                    putU32(A(in.a), static_cast<uint32_t>(ref));
                    break;
                }
                case Op::Load: {
                    uint32_t ref = getU32(A(in.b));
                    if (!inBlock(ref, in.d)) { fault = Fault::Reference; faultPc = here; goto handleFault; }
                    std::memmove(A(in.a), d + ref, in.d);
                    executed += in.d >> 3;
                    break;
                }
                case Op::Store: {
                    uint32_t ref = getU32(A(in.a));
                    if (!inBlock(ref, in.d)) { fault = Fault::Reference; faultPc = here; goto handleFault; }
                    std::memmove(d + ref, A(in.b), in.d);
                    executed += in.d >> 3;
                    break;
                }
                case Op::StrMov: case Op::StrConcat: case Op::StrLeft: case Op::StrRight: case Op::StrMid:
                case Op::StrInsert: case Op::StrDelete: case Op::StrReplace: case Op::StrFind: case Op::StrLen:
                case Op::StrCmp: case Op::StrFromValue: case Op::StrToValue: {
                    StrDesc s;
                    std::memcpy(&s, constants + in.d, sizeof s);
                    Fault f = Fault::None;
                    uint32_t lb = 0, lc = 0;
                    uint8_t* a = A(in.a);
                    uint8_t* b = A(in.b);
                    if (in.op != Op::StrFromValue && !strLen(b, s.capB, lb)) f = Fault::Reference;
                    switch (in.op) {
                        case Op::StrMov:
                            if (f != Fault::None) break;
                            if (lb > s.capA) { f = Fault::StringLength; break; }
                            std::memmove(a + 2, b + 2, lb);
                            setLen(a, lb);
                            break;
                        case Op::StrConcat: {
                            uint8_t* c = A(in.c);
                            if (f != Fault::None || !strLen(c, s.capC, lc)) { f = Fault::Reference; break; }
                            if (uint64_t{lb} + lc > s.capA) { f = Fault::StringLength; break; }
                            if (a != b) std::memmove(a + 2, b + 2, lb);
                            std::memmove(a + 2 + lb, c + 2, lc);
                            setLen(a, lb + lc);
                            break;
                        }
                        case Op::StrLeft: case Op::StrRight: {
                            if (f != Fault::None) break;
                            int64_t L = getI64(A(in.c));
                            if (L < 0 || L > static_cast<int64_t>(lb)) { f = Fault::StringPosition; break; }
                            uint32_t n = static_cast<uint32_t>(L);
                            if (n > s.capA) { f = Fault::StringLength; break; }
                            std::memmove(a + 2, b + 2 + (in.op == Op::StrLeft ? 0 : lb - n), n);
                            setLen(a, n);
                            break;
                        }
                        case Op::StrMid: {
                            if (f != Fault::None) break;
                            const uint8_t* lp = A(in.c);
                            int64_t L = getI64(lp), P = getI64(lp + 8);
                            if (L < 0 || P < 1 || P - 1 > static_cast<int64_t>(lb) - L) { f = Fault::StringPosition; break; }
                            uint32_t start = static_cast<uint32_t>(P - 1);
                            uint32_t n = static_cast<uint32_t>(L);
                            if (n > s.capA) { f = Fault::StringLength; break; }
                            std::memmove(a + 2, b + 2 + start, n);
                            setLen(a, n);
                            break;
                        }
                        case Op::StrInsert: {
                            uint8_t* c = A(in.c);
                            if (f != Fault::None || !strLen(c, s.capC, lc)) { f = Fault::Reference; break; }
                            // IN2 goes after the P-th character of IN1 (P = 0: in front)
                            int64_t P = getI64(A(s.x));
                            if (P < 0 || P > static_cast<int64_t>(lb)) { f = Fault::StringPosition; break; }
                            uint32_t at = static_cast<uint32_t>(P);
                            if (uint64_t{lb} + lc > s.capA) { f = Fault::StringLength; break; }
                            // Build in place from the end so a may alias b
                            std::memmove(a + 2 + at + lc, b + 2 + at, lb - at);
                            if (a != b) std::memmove(a + 2, b + 2, at);
                            std::memmove(a + 2 + at, c + 2, lc);
                            setLen(a, lb + lc);
                            break;
                        }
                        case Op::StrDelete: {
                            if (f != Fault::None) break;
                            const uint8_t* lp = A(in.c);
                            int64_t L = getI64(lp), P = getI64(lp + 8);
                            if (L < 0 || P < 1 || P - 1 > static_cast<int64_t>(lb) - L) { f = Fault::StringPosition; break; }
                            uint32_t at = static_cast<uint32_t>(P - 1);
                            uint32_t n = static_cast<uint32_t>(L);
                            if (a != b) std::memmove(a + 2, b + 2, at);
                            std::memmove(a + 2 + at, b + 2 + at + n, lb - at - n);
                            setLen(a, lb - n);
                            break;
                        }
                        case Op::StrReplace: {
                            uint8_t* c = A(in.c);
                            if (f != Fault::None || !strLen(c, s.capC, lc)) { f = Fault::Reference; break; }
                            const uint8_t* lp = A(s.x);
                            int64_t L = getI64(lp), P = getI64(lp + 8);
                            if (L < 0 || P < 1 || P - 1 > static_cast<int64_t>(lb) - L) { f = Fault::StringPosition; break; }
                            uint32_t at = static_cast<uint32_t>(P - 1);
                            uint32_t n = static_cast<uint32_t>(L);
                            uint64_t total = uint64_t{lb} - n + lc;
                            if (total > s.capA) { f = Fault::StringLength; break; }
                            std::memmove(a + 2 + at + lc, b + 2 + at + n, lb - at - n);
                            if (a != b) std::memmove(a + 2, b + 2, at);
                            std::memmove(a + 2 + at, c + 2, lc);
                            setLen(a, static_cast<uint32_t>(total));
                            break;
                        }
                        case Op::StrFind: {
                            uint8_t* c = A(in.c);
                            if (f != Fault::None || !strLen(c, s.capC, lc)) { f = Fault::Reference; break; }
                            int64_t pos = 0;
                            if (lc > 0 && lc <= lb) {
                                for (uint32_t i = 0; i + lc <= lb; i++) {
                                    if (std::memcmp(b + 2 + i, c + 2, lc) == 0) {
                                        pos = i + 1;
                                        break;
                                    }
                                }
                            }
                            putI64(a, pos);
                            break;
                        }
                        case Op::StrLen:
                            if (f != Fault::None) break;
                            putI64(a, lb);
                            break;
                        case Op::StrCmp: {
                            uint8_t* c = A(in.c);
                            if (f != Fault::None || !strLen(c, s.capC, lc)) { f = Fault::Reference; break; }
                            int r = std::memcmp(b + 2, c + 2, std::min(lb, lc));
                            int cmp = r < 0 ? -1 : r > 0 ? 1 : lb < lc ? -1 : lb > lc ? 1 : 0;
                            *a = cmpResult(cmp, in.flags) ? 1 : 0;
                            break;
                        }
                        case Op::StrFromValue: {
                            char buf[80];
                            size_t n = formatScalar(k, b, buf, sizeof buf);
                            if (n > s.capA) { f = Fault::StringLength; break; }
                            std::memcpy(a + 2, buf, n);
                            setLen(a, static_cast<uint32_t>(n));
                            break;
                        }
                        case Op::StrToValue:
                            if (f != Fault::None) break;
                            if (!parseScalar(k, reinterpret_cast<const char*>(b + 2), lb, a)) f = Fault::Conversion;
                            break;
                        default:
                            break;
                    }
                    if (f != Fault::None) { fault = f; faultPc = here; goto handleFault; }
                    // Cost in proportion to the bytes handled, so the budget also bounds long string work
                    executed += (uint64_t{lb} + lc) / 32 + (in.op == Op::StrFind ? uint64_t{lb} * lc / 64 : 0);
                    break;
                }
                case Op::CheckRange: {
                    RangeDesc r;
                    std::memcpy(&r, constants + in.d, sizeof r);
                    const uint8_t* b = A(in.b);
                    bool ok = false;
                    if (signedLike(k)) {
                        int64_t v = getS(k, b);
                        ok = v >= r.low && v <= r.high;
                    } else {
                        uint64_t v = getU(k, b);
                        ok = r.high >= 0 && v <= static_cast<uint64_t>(r.high) && (r.low <= 0 || v >= static_cast<uint64_t>(r.low));
                    }
                    if (!ok) { fault = Fault::Subrange; faultPc = here; goto handleFault; }
                    break;
                }
                case Op::FaultIf:
                    if (*A(in.a)) { fault = static_cast<Fault>(in.flags); faultPc = here; goto handleFault; }
                    break;
                case Op::Jmp:
                    pc = in.a;
                    break;
                case Op::Jz:
                    if (!*A(in.b)) pc = in.a;
                    break;
                case Op::Jnz:
                    if (*A(in.b)) pc = in.a;
                    break;
                case Op::Call: case Op::CallFb: case Op::CallFbRef: {
                    const PouEntry& callee = img.pous[in.a];
                    uint32_t newSelf = selfBase;
                    if (in.op == Op::CallFb) {
                        newSelf = static_cast<uint32_t>(A(in.b) - d);
                    } else if (in.op == Op::CallFbRef) {
                        newSelf = getU32(A(in.b));
                        if (!inBlock(newSelf, callee.instanceSize)) { fault = Fault::Reference; faultPc = here; goto handleFault; }
                    }
                    uint32_t newFrame = frameBase + frameSize;
                    if (frames_.size() + 1 >= frames_.capacity() ||
                        !inBlock(newFrame, callee.frameSize)) {
                        fault = Fault::Reference;
                        faultPc = here;
                        goto handleFault;
                    }
                    frames_.push_back({pou, pc, selfBase, frameBase});
                    pou = in.a;
                    selfBase = newSelf;
                    frameBase = newFrame;
                    frameSize = callee.frameSize;
                    pc = callee.codeBegin;
                    break;
                }
                case Op::Ret: {
                    if (frames_.empty()) {
                        res.executed = executed;
                        return res;
                    }
                    Frame f = frames_.back();
                    frames_.pop_back();
                    pou = f.pou;
                    pc = f.returnPc;
                    selfBase = f.selfBase;
                    frameBase = f.frameBase;
                    frameSize = img.pous[pou].frameSize;
                    break;
                }
                case Op::Try:
                    if (catches_.size() + 1 >= catches_.capacity()) { fault = Fault::Reference; faultPc = here; goto handleFault; }
                    catches_.push_back({static_cast<uint32_t>(frames_.size()), in.a, static_cast<uint32_t>(A(in.b) - d)});
                    break;
                case Op::EndTry:
                    if (catches_.empty()) { fault = Fault::Reference; faultPc = here; goto handleFault; }
                    catches_.pop_back();
                    break;
                case Op::Now:
                    putI64(A(in.a), nowNs);
                    break;
                case Op::FirstCycle:
                    *A(in.a) = firstCycle ? 1 : 0;
                    break;
                case Op::Dirty:
                    dirty_[in.a] = 1;
                    break;
                case Op::ForNext: {
                    uint8_t* b = A(in.b);
                    const uint8_t* c = A(in.c);
                    bool leaves = false;
                    if (signedLike(k)) {
                        int64_t r = 0;
                        leaves = __builtin_add_overflow(getS(k, b), getS(k, c), &r) || !fitsSigned(k, r);
                        if (!leaves) put(k, b, static_cast<uint64_t>(r));
                    } else {
                        uint64_t r = 0;
                        leaves = __builtin_add_overflow(getU(k, b), getU(k, c), &r) || !fitsUnsigned(k, r);
                        if (!leaves) put(k, b, r);
                    }
                    if (leaves) pc = in.a;
                    break;
                }
                case Op::Atan2: {
                    if (k == TypeKind::Real) {
                        float r = fmath::atan2(getF(A(in.b)), getF(A(in.c)));
                        if (!std::isfinite(r)) { fault = Fault::Domain; faultPc = here; goto handleFault; }
                        putF(A(in.a), r);
                    } else {
                        double r = fmath::atan2(getD(A(in.b)), getD(A(in.c)));
                        if (!std::isfinite(r)) { fault = Fault::Domain; faultPc = here; goto handleFault; }
                        putD(A(in.a), r);
                    }
                    break;
                }
                case Op::Count_:
                    fault = Fault::Reference;
                    faultPc = here;
                    goto handleFault;
            }
        }
        continue;

    handleFault:
        // A call with ENO connected catches the fault: unwind to it and continue
        if (!catches_.empty() && fault != Fault::Watchdog && fault != Fault::Budget && fault != Fault::Reference &&
            fault != Fault::Binding) {
            Catch c = catches_.back();
            catches_.pop_back();
            if (c.depth < frames_.size()) {
                Frame f = frames_[c.depth];
                frames_.resize(c.depth);
                pou = f.pou;
                selfBase = f.selfBase;
                frameBase = f.frameBase;
                frameSize = img.pous[pou].frameSize;
            }
            d[c.enoAddr] = 0;
            pc = c.handler;
            fault = Fault::None;
            continue;
        }
        res.fault = fault;
        res.pc = faultPc;
        res.executed = executed;
        for (size_t i = frames_.size(); i-- > 0 && res.depth < res.callers.size();) {
            res.callers[res.depth++] = frames_[i].returnPc - 1;
        }
        return res;
    }
}

}  // namespace simpleplc::vm
