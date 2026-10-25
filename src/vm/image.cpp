#include "vm/image.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <set>

namespace simpleplc::vm {

const char* faultCode(Fault f) {
    switch (f) {
        case Fault::None: return "";
        case Fault::Overflow: return "F_OVERFLOW";
        case Fault::DivZero: return "F_DIV_ZERO";
        case Fault::Real: return "F_REAL";
        case Fault::Domain: return "F_DOMAIN";
        case Fault::Index: return "F_INDEX";
        case Fault::Subrange: return "F_SUBRANGE";
        case Fault::StringLength: return "F_STRING_LENGTH";
        case Fault::StringPosition: return "F_STRING_POSITION";
        case Fault::Conversion: return "F_CONVERSION";
        case Fault::ForStep: return "F_FOR_STEP";
        case Fault::Watchdog: return "F_WATCHDOG";
        case Fault::Binding: return "F_BINDING";
        case Fault::Reference: return "F_INTERNAL";
        case Fault::Budget: return "F_BUDGET";
    }
    return "F_UNKNOWN";
}

const char* faultText(Fault f) {
    switch (f) {
        case Fault::None: return "no fault";
        case Fault::Overflow: return "integer overflow: the result does not fit its type";
        case Fault::DivZero: return "division by zero";
        case Fault::Real: return "REAL result is infinite or not a number";
        case Fault::Domain: return "argument outside the function's domain";
        case Fault::Index: return "array index out of bounds";
        case Fault::Subrange: return "value outside the variable's subrange";
        case Fault::StringLength: return "string longer than the variable can hold";
        case Fault::StringPosition: return "string position or length out of range";
        case Fault::Conversion: return "value cannot be converted";
        case Fault::ForStep: return "FOR step is 0";
        case Fault::Watchdog: return "watchdog: the cycle took too long";
        case Fault::Binding: return "a bound tag changed";
        case Fault::Reference: return "internal error: a reference outside the program's memory";
        case Fault::Budget: return "the cycle used its whole instruction budget (an endless loop?)";
    }
    return "unknown fault";
}

LineEntry Image::lineAt(uint32_t pc) const {
    LineEntry best{};
    for (const LineEntry& l : lines) {
        if (l.pc > pc) break;
        best = l;
    }
    return best;
}

namespace {

uint32_t kindSize(uint8_t k) {
    switch (static_cast<TypeKind>(k)) {
        case TypeKind::Bool: case TypeKind::SInt: case TypeKind::USInt: case TypeKind::Byte: return 1;
        case TypeKind::Int: case TypeKind::UInt: case TypeKind::Word: return 2;
        case TypeKind::DInt: case TypeKind::UDInt: case TypeKind::DWord: case TypeKind::Real: return 4;
        case TypeKind::LInt: case TypeKind::ULInt: case TypeKind::LWord: case TypeKind::LReal:
        case TypeKind::Time: case TypeKind::LTime: return 8;
        default: return 0;
    }
}

bool scalarKind(uint8_t k) { return kindSize(k) != 0; }

class Verifier {
public:
    explicit Verifier(const Image& img) : img_(img) {}

    std::expected<void, std::string> run() {
        if (img_.pous.empty()) return fail("no POUs");
        if (img_.entry >= img_.pous.size()) return fail("entry POU out of range");
        if (img_.pous[img_.entry].kind != PouKind::Program) return fail("entry is not a PROGRAM");
        if (img_.globalsSize > img_.instanceOffset) return fail("globals overlap the instance");
        if (uint64_t{img_.instanceOffset} + img_.instanceSize > img_.stackOffset) return fail("instance overlaps the stack");
        if (uint64_t{img_.stackOffset} + img_.stackSize != img_.blockSize) return fail("block size mismatch");
        if (img_.blockSize > (64u << 20)) return fail("data block too large");
        if (img_.initialData.size() != img_.stackOffset) return fail("initial data size mismatch");
        if (img_.pous[img_.entry].instanceSize != img_.instanceSize) return fail("entry instance size mismatch");

        // POU ranges: inside the code, not overlapping, ending in Ret or Jmp
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (const PouEntry& p : img_.pous) {
            if (p.codeBegin >= p.codeEnd || p.codeEnd > img_.code.size()) return fail(std::format("POU {} has a bad code range", p.name));
            Op last = img_.code[p.codeEnd - 1].op;
            if (last != Op::Ret && last != Op::Jmp) return fail(std::format("POU {} can run past its end", p.name));
            if (p.kind != PouKind::Function && p.instanceSize == 0) return fail(std::format("block {} has no instance", p.name));
            if (p.frameSize > img_.stackSize) return fail(std::format("POU {} frame larger than the stack", p.name));
            ranges.push_back({p.codeBegin, p.codeEnd});
        }
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size(); i++) {
            if (ranges[i].first < ranges[i - 1].second) return fail("POU code ranges overlap");
        }

        // Callees first: NEXT operands are checked against the largest callee frame
        callees_.assign(img_.pous.size(), {});
        for (size_t p = 0; p < img_.pous.size(); p++) {
            const PouEntry& pe = img_.pous[p];
            for (uint32_t pc = pe.codeBegin; pc < pe.codeEnd; pc++) {
                const Insn& in = img_.code[pc];
                if (in.op == Op::Call || in.op == Op::CallFb || in.op == Op::CallFbRef) {
                    if (in.a >= img_.pous.size()) return fail(std::format("call to POU {} out of range at pc {}", in.a, pc));
                    const PouEntry& callee = img_.pous[in.a];
                    if (in.op == Op::Call && callee.kind != PouKind::Function) return fail(std::format("Call of a non-function at pc {}", pc));
                    if (in.op != Op::Call && callee.kind != PouKind::FunctionBlock) return fail(std::format("CallFb of a non-block at pc {}", pc));
                    callees_[p].insert(in.a);
                }
            }
        }
        // Acyclic call graph and stack depth
        std::vector<int> state(img_.pous.size(), 0);
        std::vector<uint64_t> depth(img_.pous.size(), 0);
        std::function<bool(uint32_t)> visit = [&](uint32_t p) {
            if (state[p] == 1) return false;
            if (state[p] == 2) return true;
            state[p] = 1;
            uint64_t deepest = 0;
            for (uint32_t c : callees_[p]) {
                if (!visit(c)) return false;
                deepest = std::max(deepest, depth[c]);
            }
            depth[p] = img_.pous[p].frameSize + deepest;
            state[p] = 2;
            return true;
        };
        if (!visit(img_.entry)) return fail("recursive calls");
        if (depth[img_.entry] > img_.stackSize) return fail("stack too small for the deepest call");
        for (size_t p = 0; p < img_.pous.size(); p++) {
            if (state[p] == 0 && !visit(static_cast<uint32_t>(p))) return fail("recursive calls");
        }

        for (size_t p = 0; p < img_.pous.size(); p++) {
            if (auto r = checkPou(static_cast<uint32_t>(p)); !r) return r;
        }
        for (const Binding& b : img_.bindings) {
            if (!scalarKind(b.kind) || uint64_t{b.offset} + kindSize(b.kind) > img_.globalsSize) return fail("bad binding");
        }
        for (const VarEntry& v : img_.vars) {
            if (uint64_t{v.offset} + v.size > img_.stackOffset) return fail(std::format("variable {} outside the data", v.path));
        }
        for (const LineEntry& l : img_.lines) {
            if (l.pc > img_.code.size()) return fail("line table outside the code");
        }
        return {};
    }

private:
    static std::unexpected<std::string> fail(std::string msg) { return std::unexpected("invalid image: " + std::move(msg)); }

    uint32_t maxCalleeFrame(uint32_t p) const {
        uint32_t m = 0;
        for (uint32_t c : callees_[p]) m = std::max(m, img_.pous[c].frameSize);
        return m;
    }

    /// Checks a direct operand of `size` bytes; `write` refuses the constant pool.
    bool operandOk(uint32_t p, uint32_t op, uint64_t size, bool write) const {
        const PouEntry& pe = img_.pous[p];
        uint64_t off = offsetOf(op);
        switch (segOf(op)) {
            case Seg::Const: return !write && off + size <= img_.constants.size();
            case Seg::Global: return off + size <= img_.globalsSize;
            case Seg::Self: return pe.kind != PouKind::Function && off + size <= pe.instanceSize;
            case Seg::Frame: return off + size <= pe.frameSize;
            case Seg::Next: return off + size <= maxCalleeFrame(p);
        }
        return false;
    }

    template <typename T> bool descriptor(uint32_t off, T& out) const {
        if (uint64_t{off} + sizeof(T) > img_.constants.size()) return false;
        std::memcpy(&out, img_.constants.data() + off, sizeof(T));
        return true;
    }

    std::expected<void, std::string> checkPou(uint32_t p) {
        const PouEntry& pe = img_.pous[p];
        auto bad = [&](uint32_t pc, std::string_view what) {
            return fail(std::format("{} at pc {} in {}", what, pc, pe.name));
        };
        for (uint32_t pc = pe.codeBegin; pc < pe.codeEnd; pc++) {
            const Insn& in = img_.code[pc];
            if (in.op >= Op::Count_) return bad(pc, "unknown instruction");
            auto R = [&](uint32_t op, uint64_t size) { return operandOk(p, op, size, false); };
            auto W = [&](uint32_t op, uint64_t size) { return operandOk(p, op, size, true); };
            auto target = [&](uint32_t t) { return t >= pe.codeBegin && t < pe.codeEnd; };
            uint32_t sz = kindSize(in.type);
            TypeKind k = static_cast<TypeKind>(in.type);
            bool ok = true;
            switch (in.op) {
                case Op::Nop: case Op::Ret: case Op::EndTry: break;
                case Op::Mov: ok = in.d > 0 && W(in.a, in.d) && R(in.b, in.d); break;
                case Op::Zero: ok = in.d > 0 && W(in.a, in.d); break;
                case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
                    ok = sz && !st::isBitString(k) && k != TypeKind::Bool &&
                         (in.op != Op::Mod || st::isInteger(k)) && W(in.a, sz) && R(in.b, sz) && R(in.c, sz);
                    break;
                case Op::Min: case Op::Max:
                    ok = sz && W(in.a, sz) && R(in.b, sz) && R(in.c, sz);
                    break;
                case Op::And: case Op::Or: case Op::Xor:
                    ok = (k == TypeKind::Bool || st::isBitString(k)) && W(in.a, sz) && R(in.b, sz) && R(in.c, sz);
                    break;
                case Op::Not:
                    ok = (k == TypeKind::Bool || st::isBitString(k)) && W(in.a, sz) && R(in.b, sz);
                    break;
                case Op::Neg: case Op::Abs:
                    ok = sz && k != TypeKind::Bool && W(in.a, sz) && R(in.b, sz);
                    break;
                case Op::Pow:
                    ok = st::isReal(k) && W(in.a, sz) && R(in.b, sz) && R(in.c, sz);
                    break;
                case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
                    ok = sz && W(in.a, 1) && R(in.b, sz) && R(in.c, sz);
                    break;
                case Op::Shl: case Op::Shr: case Op::Rol: case Op::Ror:
                    ok = st::isBitString(k) && W(in.a, sz) && R(in.b, sz) && R(in.c, 8);
                    break;
                case Op::Conv:
                    ok = sz && in.d <= 0xff && kindSize(static_cast<uint8_t>(in.d)) &&
                         W(in.a, kindSize(static_cast<uint8_t>(in.d))) && R(in.b, sz);
                    break;
                case Op::TimeMul: case Op::TimeDiv:
                    ok = st::isDuration(k) && in.d <= 0xff && kindSize(static_cast<uint8_t>(in.d)) &&
                         st::isNumeric(static_cast<TypeKind>(in.d)) && W(in.a, 8) && R(in.b, 8) &&
                         R(in.c, kindSize(static_cast<uint8_t>(in.d)));
                    break;
                case Op::Math: ok = st::isReal(k) && in.flags <= static_cast<uint8_t>(MathFn::Atan) && W(in.a, sz) && R(in.b, sz); break;
                case Op::GetBit: ok = st::isBitString(k) && in.d < sz * 8 && W(in.a, 1) && R(in.b, sz); break;
                case Op::SetBit: ok = st::isBitString(k) && in.d < sz * 8 && W(in.a, sz) && R(in.b, 1); break;
                case Op::Addr: ok = W(in.a, 4) && segOf(in.b) != Seg::Const && R(in.b, 0); break;
                case Op::RefAdd: ok = W(in.a, 4) && R(in.b, 4); break;
                case Op::Index: {
                    IndexDesc dsc{};
                    ok = W(in.a, 4) && R(in.b, 4) && R(in.c, 8) && descriptor(in.d, dsc) && dsc.count > 0 && dsc.stride > 0;
                    break;
                }
                case Op::Load: ok = in.d > 0 && W(in.a, in.d) && R(in.b, 4); break;
                case Op::Store: ok = in.d > 0 && R(in.a, 4) && R(in.b, in.d); break;
                case Op::StrMov: case Op::StrConcat: case Op::StrLeft: case Op::StrRight: case Op::StrMid:
                case Op::StrInsert: case Op::StrDelete: case Op::StrReplace: case Op::StrFind: case Op::StrLen:
                case Op::StrCmp: case Op::StrFromValue: case Op::StrToValue: {
                    StrDesc s{};
                    if (!descriptor(in.d, s)) { ok = false; break; }
                    auto str = [&](uint32_t op, uint32_t cap, bool w) {
                        return cap > 0 && cap <= 65535 && operandOk(p, op, uint64_t{cap} + 2, w);
                    };
                    switch (in.op) {
                        case Op::StrMov: ok = str(in.a, s.capA, true) && str(in.b, s.capB, false); break;
                        case Op::StrConcat: ok = str(in.a, s.capA, true) && str(in.b, s.capB, false) && str(in.c, s.capC, false); break;
                        case Op::StrLeft: case Op::StrRight: ok = str(in.a, s.capA, true) && str(in.b, s.capB, false) && R(in.c, 8); break;
                        case Op::StrMid: case Op::StrDelete: ok = str(in.a, s.capA, true) && str(in.b, s.capB, false) && R(in.c, 16); break;
                        case Op::StrInsert: ok = str(in.a, s.capA, true) && str(in.b, s.capB, false) && str(in.c, s.capC, false) && R(s.x, 8); break;
                        case Op::StrReplace: ok = str(in.a, s.capA, true) && str(in.b, s.capB, false) && str(in.c, s.capC, false) && R(s.x, 16); break;
                        case Op::StrFind: ok = W(in.a, 8) && str(in.b, s.capB, false) && str(in.c, s.capC, false); break;
                        case Op::StrLen: ok = W(in.a, 8) && str(in.b, s.capB, false); break;
                        case Op::StrCmp: ok = W(in.a, 1) && str(in.b, s.capB, false) && str(in.c, s.capC, false) && in.flags < 6; break;
                        case Op::StrFromValue: ok = str(in.a, s.capA, true) && sz && R(in.b, sz); break;
                        case Op::StrToValue: ok = sz && W(in.a, sz) && str(in.b, s.capB, false); break;
                        default: break;
                    }
                    break;
                }
                case Op::CheckRange: {
                    RangeDesc r{};
                    ok = sz && st::isInteger(k) && R(in.b, sz) && descriptor(in.d, r) && r.low <= r.high;
                    break;
                }
                case Op::FaultIf: ok = R(in.a, 1) && in.flags >= 1 && in.flags <= static_cast<uint8_t>(Fault::Binding); break;
                case Op::Jmp: ok = target(in.a); break;
                case Op::Jz: case Op::Jnz: ok = target(in.a) && R(in.b, 1); break;
                case Op::Call: break;  // checked above
                case Op::CallFb: ok = segOf(in.b) == Seg::Self && W(in.b, img_.pous[in.a].instanceSize); break;
                case Op::CallFbRef: ok = R(in.b, 4); break;
                case Op::Try: ok = target(in.a) && W(in.b, 1) && segOf(in.b) == Seg::Frame; break;
                case Op::Now: ok = W(in.a, 8); break;
                case Op::ForNext: ok = target(in.a) && st::isInteger(k) && W(in.b, sz) && R(in.c, sz); break;
                case Op::Atan2: ok = st::isReal(k) && W(in.a, sz) && R(in.b, sz) && R(in.c, sz); break;
                case Op::FirstCycle: ok = W(in.a, 1); break;
                case Op::Dirty: ok = in.a < img_.bindings.size() && !img_.bindings[in.a].readOnly; break;
                case Op::Count_: ok = false; break;
            }
            if (!ok) return bad(pc, std::format("invalid operands for instruction {}", static_cast<int>(in.op)));
        }
        return {};
    }

    const Image& img_;
    std::vector<std::set<uint32_t>> callees_;
};

// ── Serialization ──

constexpr char kMagic[4] = {'S', 'P', 'L', 'B'};

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

class Writer {
public:
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); out.insert(out.end(), s.begin(), s.end()); }
    void bytes(const std::vector<uint8_t>& b) { u32(static_cast<uint32_t>(b.size())); out.insert(out.end(), b.begin(), b.end()); }
};

class Reader {
public:
    explicit Reader(std::string_view d) : d_(d) {}
    bool ok = true;
    uint8_t u8() {
        if (pos_ + 1 > d_.size()) { ok = false; return 0; }
        return static_cast<uint8_t>(d_[pos_++]);
    }
    uint32_t u32() {
        if (pos_ + 4 > d_.size()) { ok = false; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(d_[pos_ + static_cast<size_t>(i)])) << (8 * i);
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        return lo | (uint64_t{u32()} << 32);
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || pos_ + n > d_.size()) { ok = false; return {}; }
        std::string s(d_.substr(pos_, n));
        pos_ += n;
        return s;
    }
    std::vector<uint8_t> bytes() {
        std::string s = str();
        return {s.begin(), s.end()};
    }
    size_t pos() const { return pos_; }

private:
    std::string_view d_;
    size_t pos_ = 0;
};

}  // namespace

std::expected<void, std::string> verify(const Image& image) { return Verifier(image).run(); }

std::vector<uint8_t> serialize(const Image& img) {
    Writer w;
    for (char c : kMagic) w.u8(static_cast<uint8_t>(c));
    w.u32(Image::kFormat);
    w.str(img.compiler);
    for (uint8_t b : img.sourceHash) w.u8(b);
    w.u32(static_cast<uint32_t>(img.files.size()));
    for (const std::string& f : img.files) w.str(f);
    w.str(img.program);
    for (uint32_t v : {img.globalsSize, img.instanceOffset, img.instanceSize, img.stackOffset, img.stackSize, img.blockSize, img.entry})
        w.u32(v);
    w.bytes(img.initialData);
    w.bytes(img.constants);
    w.u32(static_cast<uint32_t>(img.code.size()));
    for (const Insn& in : img.code) {
        w.u32(static_cast<uint32_t>(in.op) | (uint32_t{in.type} << 16) | (uint32_t{in.flags} << 24));
        w.u32(in.a);
        w.u32(in.b);
        w.u32(in.c);
        w.u32(in.d);
    }
    w.u32(static_cast<uint32_t>(img.pous.size()));
    for (const PouEntry& p : img.pous) {
        w.str(p.name);
        w.u8(static_cast<uint8_t>(p.kind));
        w.u32(p.codeBegin);
        w.u32(p.codeEnd);
        w.u32(p.frameSize);
        w.u32(p.instanceSize);
    }
    w.u32(static_cast<uint32_t>(img.vars.size()));
    for (const VarEntry& v : img.vars) {
        w.str(v.path);
        w.str(v.typeName);
        w.u8(v.kind);
        w.u32(v.offset);
        w.u32(v.size);
        w.u8(v.flags);
        w.u8(v.hasRange ? 1 : 0);
        w.u64(static_cast<uint64_t>(v.low));
        w.u64(static_cast<uint64_t>(v.high));
        w.str(v.typeSig);
    }
    w.u32(static_cast<uint32_t>(img.bindings.size()));
    for (const Binding& b : img.bindings) {
        w.str(b.tag);
        w.u8(b.kind);
        w.u32(b.offset);
        w.u8(b.readOnly ? 1 : 0);
    }
    w.u32(static_cast<uint32_t>(img.lines.size()));
    for (const LineEntry& l : img.lines) {
        w.u32(l.pc);
        w.u32(l.file);
        w.u32(l.line);
    }
    w.u32(static_cast<uint32_t>(img.enums.size()));
    for (const EnumInfo& e : img.enums) {
        w.str(e.name);
        w.u32(static_cast<uint32_t>(e.values.size()));
        for (const auto& [v, n] : e.values) {
            w.u32(static_cast<uint32_t>(static_cast<uint64_t>(v)));
            w.u32(static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
            w.str(n);
        }
    }
    w.u32(crc32(w.out.data(), w.out.size()));
    return w.out;
}

std::expected<Image, std::string> deserialize(std::string_view bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 4) != 0) return std::unexpected("not a SimplePLC image");
    uint32_t stored = 0;
    for (int i = 0; i < 4; i++) stored |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[bytes.size() - 4 + static_cast<size_t>(i)])) << (8 * i);
    if (crc32(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() - 4) != stored) return std::unexpected("image checksum mismatch");
    Reader r(bytes.substr(0, bytes.size() - 4));
    for (int i = 0; i < 4; i++) r.u8();
    if (r.u32() != Image::kFormat) return std::unexpected("image format version differs");
    Image img;
    img.compiler = r.str();
    for (uint8_t& b : img.sourceHash) b = r.u8();
    uint32_t nf = r.u32();
    if (nf > 1000) return std::unexpected("bad image");
    for (uint32_t i = 0; i < nf && r.ok; i++) img.files.push_back(r.str());
    img.program = r.str();
    img.globalsSize = r.u32();
    img.instanceOffset = r.u32();
    img.instanceSize = r.u32();
    img.stackOffset = r.u32();
    img.stackSize = r.u32();
    img.blockSize = r.u32();
    img.entry = r.u32();
    img.initialData = r.bytes();
    img.constants = r.bytes();
    uint32_t nc = r.u32();
    if (!r.ok || nc > (1u << 24)) return std::unexpected("bad image");
    img.code.reserve(nc);
    for (uint32_t i = 0; i < nc && r.ok; i++) {
        Insn in;
        uint32_t head = r.u32();
        in.op = static_cast<Op>(head & 0xFFFF);
        in.type = static_cast<uint8_t>(head >> 16);
        in.flags = static_cast<uint8_t>(head >> 24);
        in.a = r.u32();
        in.b = r.u32();
        in.c = r.u32();
        in.d = r.u32();
        img.code.push_back(in);
    }
    uint32_t np = r.u32();
    if (!r.ok || np > 100000) return std::unexpected("bad image");
    for (uint32_t i = 0; i < np && r.ok; i++) {
        PouEntry p;
        p.name = r.str();
        p.kind = static_cast<PouKind>(r.u8());
        p.codeBegin = r.u32();
        p.codeEnd = r.u32();
        p.frameSize = r.u32();
        p.instanceSize = r.u32();
        img.pous.push_back(p);
    }
    uint32_t nv = r.u32();
    if (!r.ok || nv > 1000000) return std::unexpected("bad image");
    for (uint32_t i = 0; i < nv && r.ok; i++) {
        VarEntry v;
        v.path = r.str();
        v.typeName = r.str();
        v.kind = r.u8();
        v.offset = r.u32();
        v.size = r.u32();
        v.flags = r.u8();
        v.hasRange = r.u8() != 0;
        v.low = static_cast<int64_t>(r.u64());
        v.high = static_cast<int64_t>(r.u64());
        v.typeSig = r.str();
        img.vars.push_back(v);
    }
    uint32_t nb = r.u32();
    if (!r.ok || nb > 100000) return std::unexpected("bad image");
    for (uint32_t i = 0; i < nb && r.ok; i++) {
        Binding b;
        b.tag = r.str();
        b.kind = r.u8();
        b.offset = r.u32();
        b.readOnly = r.u8() != 0;
        img.bindings.push_back(b);
    }
    uint32_t nl = r.u32();
    if (!r.ok || nl > (1u << 24)) return std::unexpected("bad image");
    for (uint32_t i = 0; i < nl && r.ok; i++) {
        LineEntry l;
        l.pc = r.u32();
        l.file = static_cast<uint16_t>(r.u32());
        l.line = r.u32();
        img.lines.push_back(l);
    }
    uint32_t ne = r.u32();
    if (!r.ok || ne > 100000) return std::unexpected("bad image");
    for (uint32_t i = 0; i < ne && r.ok; i++) {
        EnumInfo e;
        e.name = r.str();
        uint32_t nv2 = r.u32();
        if (nv2 > 100000) return std::unexpected("bad image");
        for (uint32_t k = 0; k < nv2 && r.ok; k++) {
            uint64_t lo = r.u32();
            uint64_t hi = r.u32();
            e.values.push_back({static_cast<int64_t>(lo | (hi << 32)), r.str()});
        }
        img.enums.push_back(std::move(e));
    }
    if (!r.ok) return std::unexpected("image is truncated");
    if (auto v = verify(img); !v) return std::unexpected(v.error());
    return img;
}

}  // namespace simpleplc::vm
