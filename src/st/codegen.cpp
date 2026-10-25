// Code generation: annotated AST (st/sema.cpp) → verified-image instructions (vm/image.h).
// Every value has a fixed place decided here; nothing is allocated at run time.

#include "st/codegen.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <set>

#include "st/sema.h"

namespace simpleplc::st {

using namespace ast;
using vm::Insn;
using vm::Op;
using vm::Seg;
using vm::operand;

namespace {

uint32_t alignUp(uint32_t v, uint32_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

/// The kind a value is computed in: enumerations as their base, subranges as theirs.
TypeKind scalarKind(const Type* t) {
    t = baseOf(t);
    if (t->kind == TypeKind::Enum) return t->enumBase->kind;
    return t->kind;
}

uint32_t elementStride(const Type* el) { return alignUp(el->size, std::max<uint32_t>(el->align, 1)); }

/// The full definition of a type, independent of its name: two types with the same signature
/// hold the same values in the same bytes (online change keeps an opaque array only then).
std::string typeSignature(const Type* t) {
    std::string range;
    for (const Type* r = t; r; r = r->rangeBase) {
        if (r->hasRange) {
            range = std::format("({}..{})", r->rangeLow, r->rangeHigh);
            break;
        }
    }
    const Type* b = baseOf(t);
    std::string s;
    switch (b->kind) {
        case TypeKind::Array: {
            s = "ARRAY[";
            for (size_t i = 0; i < b->dims.size(); i++) s += std::format("{}{}..{}", i ? "," : "", b->dims[i].low, b->dims[i].high);
            s += "] OF " + typeSignature(b->element);
            break;
        }
        case TypeKind::Struct: {
            s = "STRUCT(";
            for (const Field& f : b->fields) s += std::format("{}:{}@{};", f.upper, typeSignature(f.type), f.offset);
            s += ")";
            break;
        }
        case TypeKind::FunctionBlock: {
            s = std::format("{}(", b->pou ? b->pou->upper : b->name);
            if (b->pou) {
                for (const Symbol& sym : b->pou->symbols) {
                    if (sym.storage != Storage::Self) continue;
                    s += std::format("{}:{}@{}{};", sym.upper, sym.kind == SymKind::InOut ? "REF" : typeSignature(sym.type),
                                     sym.offset, sym.retain ? "R" : "");
                }
            }
            s += ")";
            break;
        }
        case TypeKind::Enum: {
            s = std::format("ENUM {}(", b->enumBase ? elementaryName(b->enumBase->kind) : "INT");
            for (const EnumConstant& c : b->enumValues) s += std::format("{}={};", c.upper, c.value);
            s += ")";
            break;
        }
        case TypeKind::String: s = std::format("STRING[{}]", b->capacity); break;
        default: s = std::string(elementaryName(b->kind)); break;
    }
    return s + range;
}

}  // namespace

// ───────────── Initial values ─────────────

void writeInitialValue(uint8_t* dst, const Type* t, const Initializer* init) {
    const Type* b = baseOf(t);
    std::memset(dst, 0, t->size);
    // Defaults of the type
    switch (b->kind) {
        case TypeKind::Enum:
            if (!b->enumValues.empty()) Value::ofSigned(b->enumBase->kind, b->enumValues.front().value).store(b, dst);
            break;
        case TypeKind::Struct:
            for (const Field& f : b->fields) writeInitialValue(dst + f.offset, f.type, f.init);
            break;
        case TypeKind::FunctionBlock:
            for (const Symbol& s : b->pou->symbols) {
                if (s.storage == Storage::Self && s.kind != SymKind::InOut) {
                    writeInitialValue(dst + s.offset, s.type, s.init);
                }
            }
            break;
        case TypeKind::Array: {
            uint64_t count = 1;
            for (const ArrayDim& d : b->dims) count *= d.count();
            uint32_t stride = elementStride(b->element);
            std::vector<uint8_t> el(b->element->size);
            writeInitialValue(el.data(), b->element, nullptr);
            bool zero = std::all_of(el.begin(), el.end(), [](uint8_t x) { return x == 0; });
            if (!zero) {
                for (uint64_t i = 0; i < count; i++) std::memcpy(dst + i * stride, el.data(), el.size());
            }
            break;
        }
        default:
            // A subrange starts at its lower limit (IEC 61131-3), even when 0 is in range
            if (t->hasRange) {
                if (isUnsigned(b->kind)) Value::ofUnsigned(b->kind, static_cast<uint64_t>(t->rangeLow)).store(b, dst);
                else Value::ofSigned(b->kind, t->rangeLow).store(b, dst);
            }
            break;
    }
    if (t->defaultInit && t->defaultInit != init) writeInitialValue(dst, t, t->defaultInit);
    if (!init) return;
    switch (init->kind) {
        case Initializer::Kind::Expr: {
            Value v;
            if (init->expr && evaluateConstant(*init->expr, v)) {
                if (b->kind == TypeKind::String) {
                    std::memset(dst, 0, t->size);
                    v.store(b, dst);
                } else {
                    v.store(t, dst);
                }
            }
            break;
        }
        case Initializer::Kind::Array: {
            if (b->kind != TypeKind::Array) break;
            uint64_t count = 1;
            for (const ArrayDim& d : b->dims) count *= d.count();
            uint32_t stride = elementStride(b->element);
            uint64_t i = 0;
            for (const ArrayInitElement& el : init->elements) {
                uint64_t n = el.count ? el.count->as<IntLit>().value : 1;
                for (uint64_t k = 0; k < n && i < count; k++, i++) {
                    if (el.value) writeInitialValue(dst + i * stride, b->element, el.value.get());
                }
            }
            break;
        }
        case Initializer::Kind::Struct: {
            for (const StructInitField& f : init->fields) {
                const Field* field = b->field(f.upper);
                if (field && f.value) writeInitialValue(dst + field->offset, field->type, f.value.get());
            }
            break;
        }
    }
}

namespace {

struct Place {
    bool indirect = false;
    uint32_t op = 0;      ///< direct: the operand; indirect: operand of a u32 reference
    uint32_t offset = 0;  ///< indirect: bytes to add to the reference
    const Type* type = nullptr;
    int binding = -1;     ///< a tag (VAR_EXTERNAL): stores mark it for copy-out
};

struct Label {
    int64_t pos = -1;
    std::vector<uint32_t> uses;  ///< instructions whose `a` jumps here
};

struct Loop {
    Label* exit;
    Label* next;  ///< CONTINUE
};

class CodeGen {
public:
    CodeGen(Program& p, vm::Image& img, Diagnostics& d) : p_(p), img_(img), d_(d) {}

    bool run() {
        if (!p_.main) return false;
        // POUs reachable from the program, in a stable order
        std::vector<PouInfo*> order;
        std::function<void(PouInfo*)> reach = [&](PouInfo* pou) {
            if (pou->index >= 0) return;
            pou->index = static_cast<int>(order.size());
            order.push_back(pou);
            for (PouInfo* c : pou->callees) reach(c);
        };
        reach(p_.main);
        img_.pous.resize(order.size());
        for (PouInfo* pou : order) genPou(*pou);
        if (d_.hasErrors()) return false;

        img_.compiler = "simpleplc-st 1";
        img_.files = p_.fileNames;
        img_.program = p_.main->spelled;
        img_.entry = static_cast<uint32_t>(p_.main->index);
        img_.globalsSize = p_.globalsSize;
        img_.instanceOffset = alignUp(p_.globalsSize, 16);
        img_.instanceSize = p_.main->instanceSize;
        img_.stackOffset = alignUp(img_.instanceOffset + img_.instanceSize, 16);
        std::map<PouInfo*, uint64_t> depth;
        std::function<uint64_t(PouInfo*)> stackOf = [&](PouInfo* pou) -> uint64_t {
            auto it = depth.find(pou);
            if (it != depth.end()) return it->second;
            uint64_t deepest = 0;
            for (PouInfo* c : calls_[pou]) deepest = std::max(deepest, stackOf(c));
            uint64_t total = img_.pous[static_cast<size_t>(pou->index)].frameSize + deepest;
            depth[pou] = total;
            return total;
        };
        uint64_t stack = alignUp(static_cast<uint32_t>(std::max<uint64_t>(stackOf(p_.main), 16)), 16);
        uint64_t block = img_.stackOffset + stack;
        if (block > (16u << 20)) {
            d_.error("E_LIMIT", p_.main->ast->loc,
                     std::format("the program needs {} bytes of memory; the limit is 16 MB", block));
            return false;
        }
        img_.stackSize = static_cast<uint32_t>(stack);
        img_.blockSize = static_cast<uint32_t>(block);
        img_.initialData.assign(img_.stackOffset, 0);
        writeInitialValue(img_.initialData.data() + img_.instanceOffset, p_.main->instanceType, nullptr);

        for (const TagBinding& b : p_.bindings) {
            vm::Binding vb;
            vb.tag = b.tag;
            vb.kind = static_cast<uint8_t>(scalarKind(b.type));
            vb.offset = b.offset;
            // Read-only unless some reachable code stores to it: the PLC refuses two writers of one tag
            size_t index = img_.bindings.size();
            vb.readOnly = index >= stored_.size() || !stored_[index];
            img_.bindings.push_back(vb);
        }
        addVars(p_.main->upper, p_.main->instanceType, img_.instanceOffset, 0);
        for (const TagBinding& b : p_.bindings) {
            vm::VarEntry v;
            v.path = std::format("{}.{}", p_.main->upper, toUpperAscii(b.tag));
            v.typeName = b.type->name;
            v.kind = static_cast<uint8_t>(scalarKind(b.type));
            v.offset = b.offset;
            v.size = b.type->size;
            v.flags = vm::kVarExternal | (b.written ? 0 : vm::kVarConstant);
            img_.vars.push_back(v);
        }
        std::sort(img_.lines.begin(), img_.lines.end(), [](const vm::LineEntry& a, const vm::LineEntry& b) { return a.pc < b.pc; });
        return !d_.hasErrors();
    }

private:
    static std::string toUpperAscii(std::string s) {
        for (char& c : s) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return s;
    }

    // ── Variable table ──

    void addVars(const std::string& path, const Type* t, uint32_t offset, uint8_t flags) {
        if (img_.vars.size() > 20000) return;
        const Type* b = baseOf(t);
        if (b->kind == TypeKind::FunctionBlock) {
            for (const Symbol& s : b->pou->symbols) {
                if (s.storage != Storage::Self || s.kind == SymKind::InOut) continue;
                uint8_t f = flags;
                if (s.retain) f |= vm::kVarRetain;
                if (s.constant) f |= vm::kVarConstant;
                if (s.kind == SymKind::Input) f |= vm::kVarInput;
                if (s.kind == SymKind::Output) f |= vm::kVarOutput;
                addVars(path + "." + s.upper, s.type, offset + s.offset, f);
            }
            return;
        }
        if (b->kind == TypeKind::Struct) {
            for (const Field& f : b->fields) addVars(path + "." + f.upper, f.type, offset + f.offset, flags);
            return;
        }
        if (b->kind == TypeKind::Array) {
            uint64_t count = 1;
            for (const ArrayDim& d : b->dims) count *= d.count();
            if (count > 1024) {
                vm::VarEntry v;
                v.path = path;
                v.typeName = t->name;
                v.kind = static_cast<uint8_t>(TypeKind::Array);
                v.offset = offset;
                v.size = t->size;
                v.flags = flags;
                v.typeSig = typeSignature(t);
                img_.vars.push_back(v);
                return;
            }
            uint32_t stride = elementStride(b->element);
            std::vector<int64_t> idx;
            for (const ArrayDim& d : b->dims) idx.push_back(d.low);
            for (uint64_t i = 0; i < count; i++) {
                std::string p = path + "[";
                for (size_t k = 0; k < idx.size(); k++) p += std::format("{}{}", k ? "," : "", idx[k]);
                addVars(p + "]", b->element, offset + static_cast<uint32_t>(i) * stride, flags);
                for (size_t k = idx.size(); k-- > 0;) {
                    if (++idx[k] <= b->dims[k].high) break;
                    idx[k] = b->dims[k].low;
                }
            }
            return;
        }
        if (b->kind == TypeKind::Enum) {
            bool known = false;
            for (const vm::EnumInfo& e : img_.enums) known = known || e.name == b->name;
            if (!known) {
                vm::EnumInfo e;
                e.name = b->name;
                for (const EnumConstant& c : b->enumValues) e.values.push_back({c.value, c.spelled});
                img_.enums.push_back(std::move(e));
            }
        }
        vm::VarEntry v;
        v.path = path;
        v.typeName = b->kind == TypeKind::Enum ? b->name : t->name;
        v.kind = static_cast<uint8_t>(scalarKind(t));
        v.offset = offset;
        v.size = t->size;
        v.flags = flags;
        for (const Type* r = t; r; r = r->rangeBase) {
            if (r->hasRange) {
                v.hasRange = true;
                v.low = r->rangeLow;
                v.high = r->rangeHigh;
                break;
            }
        }
        img_.vars.push_back(v);
    }

    // ── Emission ──

    uint32_t emit(Op op, TypeKind type = TypeKind::Void, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                  uint32_t d = 0, uint8_t flags = 0) {
        if (op == Op::Dirty) {
            if (stored_.size() <= a) stored_.resize(a + 1, false);
            stored_[a] = true;
        }
        Insn i;
        i.op = op;
        i.type = static_cast<uint8_t>(type);
        i.flags = flags;
        i.a = a;
        i.b = b;
        i.c = c;
        i.d = d;
        img_.code.push_back(i);
        return static_cast<uint32_t>(img_.code.size() - 1);
    }

    uint32_t pc() const { return static_cast<uint32_t>(img_.code.size()); }

    void jumpTo(Op op, Label& l, uint32_t cond = 0) {
        uint32_t at = emit(op, TypeKind::Void, 0, cond);
        if (l.pos >= 0) img_.code[at].a = static_cast<uint32_t>(l.pos);
        else l.uses.push_back(at);
    }
    void place(Label& l) {
        l.pos = pc();
        for (uint32_t at : l.uses) img_.code[at].a = static_cast<uint32_t>(l.pos);
        l.uses.clear();
    }

    void setLine(SourceLoc loc) {
        if (!img_.lines.empty() && img_.lines.back().file == loc.file && img_.lines.back().line == loc.line) return;
        img_.lines.push_back({pc(), loc.file, loc.line});
    }

    // ── Constant pool ──

    uint32_t constBytes(const void* data, size_t n, size_t align = 8) {
        std::string key(static_cast<const char*>(data), n);
        auto it = constIndex_.find(key);
        if (it != constIndex_.end()) return it->second;
        size_t off = (img_.constants.size() + align - 1) / align * align;
        img_.constants.resize(off + n);
        std::memcpy(img_.constants.data() + off, data, n);
        uint32_t o = static_cast<uint32_t>(off);
        constIndex_[key] = o;
        return o;
    }
    uint32_t constOperand(const Value& v, const Type* t) {
        std::vector<uint8_t> bytes(std::max<uint32_t>(t->size, 1));
        if (baseOf(t)->kind == TypeKind::String) {
            v.store(baseOf(t), bytes.data());
        } else {
            v.store(t, bytes.data());
        }
        return operand(Seg::Const, constBytes(bytes.data(), bytes.size()));
    }
    uint32_t constLint(int64_t x) {
        return operand(Seg::Const, constBytes(&x, 8));
    }
    uint32_t constBool(bool x) {
        uint8_t v = x ? 1 : 0;
        return operand(Seg::Const, constBytes(&v, 1, 1));
    }
    template <typename T> uint32_t desc(const T& d) { return constBytes(&d, sizeof(T)); }

    // ── Temporaries in the frame ──

    uint32_t temp(uint32_t size, uint32_t align = 8) {
        frameTop_ = alignUp(frameTop_, std::max<uint32_t>(align, 1));
        uint32_t off = frameTop_;
        frameTop_ += std::max<uint32_t>(size, 1);
        frameMax_ = std::max(frameMax_, frameTop_);
        return operand(Seg::Frame, off);
    }
    uint32_t tempFor(const Type* t) { return temp(t->size, std::max<uint32_t>(t->align, 1)); }
    bool isTemp(uint32_t op) const { return vm::segOf(op) == Seg::Frame && vm::offsetOf(op) >= frameBase_; }

    // ── POUs ──

    void genPou(PouInfo& pou) {
        vm::PouEntry& e = img_.pous[static_cast<size_t>(pou.index)];
        e.name = pou.upper;
        e.kind = pou.kind == PouKind::Program ? vm::PouKind::Program
                 : pou.kind == PouKind::Function ? vm::PouKind::Function : vm::PouKind::FunctionBlock;
        e.instanceSize = pou.kind == PouKind::Function ? 0 : pou.instanceSize;
        e.codeBegin = pc();
        pou_ = &pou;
        frameBase_ = pou.frameSize;
        frameTop_ = frameBase_;
        frameMax_ = frameBase_;
        setLine(pou.ast->loc);

        // Prologue: variables that start fresh on every call
        for (Symbol& s : pou.symbols) {
            bool fresh = pou.kind == PouKind::Function
                             ? (s.kind != SymKind::Input && s.kind != SymKind::InOut)
                             : s.kind == SymKind::Temp;
            if (!fresh) continue;
            initVariable(operand(Seg::Frame, s.offset), s.type, s.init);
        }
        Label ret;
        returnLabel_ = &ret;
        genStmts(pou.ast->body);
        place(ret);
        setLine(pou.ast->endLoc);
        emit(Op::Ret);
        e.codeEnd = pc();
        e.frameSize = alignUp(frameMax_, 8);
        returnLabel_ = nullptr;
    }

    void initVariable(uint32_t dst, const Type* t, const Initializer* init) {
        std::vector<uint8_t> bytes(t->size);
        writeInitialValue(bytes.data(), t, init);
        bool zero = std::all_of(bytes.begin(), bytes.end(), [](uint8_t x) { return x == 0; });
        if (zero) emit(Op::Zero, TypeKind::Void, dst, 0, 0, t->size);
        else emit(Op::Mov, TypeKind::Void, dst, operand(Seg::Const, constBytes(bytes.data(), bytes.size())), 0, t->size);
    }

    // ── Statements ──

    void genStmts(StmtList& list) {
        for (StmtPtr& s : list) {
            if (!s) continue;
            uint32_t mark = frameTop_;
            genStmt(*s);
            frameTop_ = mark;
        }
    }

    void genStmt(Stmt& s) {
        setLine(s.loc);
        switch (s.kind) {
            case StmtKind::Assign: {
                auto& a = s.as<AssignStmt>();
                if (a.target->kind == ExprKind::BitAccess) {
                    storeBit(a.target->as<BitAccess>(), gen(*a.value));
                    return;
                }
                uint32_t v = gen(*a.value);
                if (hasCall(*a.target) && !isTemp(v)) v = copyToTemp(v, a.value->type);
                Place t = placeOf(*a.target);
                store(t, v, a.value->type);
                return;
            }
            case StmtKind::Call:
                genCall(s.as<CallStmt>().call->as<Call>(), true);
                return;
            case StmtKind::If: {
                auto& i = s.as<IfStmt>();
                Label end;
                for (IfBranch& b : i.branches) {
                    Label next;
                    setLine(b.condition->loc);
                    uint32_t c = gen(*b.condition);
                    jumpTo(Op::Jz, next, c);
                    genStmts(b.body);
                    jumpTo(Op::Jmp, end);
                    place(next);
                }
                genStmts(i.elseBody);
                place(end);
                return;
            }
            case StmtKind::Case: genCase(s.as<CaseStmt>()); return;
            case StmtKind::For: genFor(s.as<ForStmt>()); return;
            case StmtKind::While: {
                auto& w = s.as<WhileStmt>();
                Label top, exit;
                place(top);
                setLine(w.condition->loc);
                uint32_t c = gen(*w.condition);
                jumpTo(Op::Jz, exit, c);
                loops_.push_back({&exit, &top});
                genStmts(w.body);
                loops_.pop_back();
                jumpTo(Op::Jmp, top);
                place(exit);
                return;
            }
            case StmtKind::Repeat: {
                auto& r = s.as<RepeatStmt>();
                Label top, next, exit;
                place(top);
                loops_.push_back({&exit, &next});
                genStmts(r.body);
                loops_.pop_back();
                place(next);
                setLine(r.condition->loc);
                uint32_t c = gen(*r.condition);
                jumpTo(Op::Jz, top, c);
                place(exit);
                return;
            }
            case StmtKind::Exit:
                jumpTo(Op::Jmp, *loops_.back().exit);
                return;
            case StmtKind::Continue:
                jumpTo(Op::Jmp, *loops_.back().next);
                return;
            case StmtKind::Return:
                jumpTo(Op::Jmp, *returnLabel_);
                return;
            case StmtKind::Empty:
                return;
        }
    }

    void genCase(CaseStmt& c) {
        const Type* st = c.selector->type;
        TypeKind k = scalarKind(st);
        uint32_t sel = gen(*c.selector);
        if (!isTemp(sel)) sel = copyToTemp(sel, st);
        Label end;
        std::vector<Label> bodies(c.branches.size());
        for (size_t i = 0; i < c.branches.size(); i++) {
            for (CaseLabel& lab : c.branches[i].labels) {
                Value lo;
                evaluateConstant(*lab.low, lo);
                uint32_t t = temp(1, 1);
                if (lab.high) {
                    Value hi;
                    evaluateConstant(*lab.high, hi);
                    uint32_t t2 = temp(1, 1);
                    emit(Op::Ge, k, t, sel, constOperand(lo, st));
                    emit(Op::Le, k, t2, sel, constOperand(hi, st));
                    emit(Op::And, TypeKind::Bool, t, t, t2);
                } else {
                    emit(Op::Eq, k, t, sel, constOperand(lo, st));
                }
                jumpTo(Op::Jnz, bodies[i], t);
            }
        }
        genStmts(c.elseBody);
        jumpTo(Op::Jmp, end);
        for (size_t i = 0; i < c.branches.size(); i++) {
            place(bodies[i]);
            genStmts(c.branches[i].body);
            jumpTo(Op::Jmp, end);
        }
        place(end);
    }

    void genFor(ForStmt& f) {
        Symbol* ctl = f.control;
        const Type* ct = ctl->type;
        TypeKind k = scalarKind(ct);
        Place cp = symbolPlace(*ctl);
        uint32_t start = gen(*f.start);
        store(cp, start, f.start->type);
        uint32_t end = gen(*f.end);
        if (!isTemp(end)) end = copyToTemp(end, ct);
        uint32_t step = 0;
        Value stepValue;
        bool constStep = false;
        int sign = 1;
        if (f.step) {
            step = gen(*f.step);
            if (!isTemp(step) && !f.step->isConstant) step = copyToTemp(step, ct);
            if (evaluateConstant(*f.step, stepValue)) {
                constStep = true;
                bool negative = isSigned(k) ? stepValue.i < 0 : false;
                sign = negative ? -1 : 1;
            } else {
                uint32_t z = temp(1, 1);
                emit(Op::Eq, k, z, step, constOperand(Value::ofSigned(k, 0), ct));
                if (isUnsigned(k)) emit(Op::Eq, k, z, step, constOperand(Value::ofUnsigned(k, 0), ct));
                emit(Op::FaultIf, TypeKind::Void, z, 0, 0, 0, static_cast<uint8_t>(vm::Fault::ForStep));
            }
        } else {
            constStep = true;
            Value one = isUnsigned(k) ? Value::ofUnsigned(k, 1) : Value::ofSigned(k, 1);
            step = constOperand(one, ct);
        }
        uint32_t ctlOp = cp.op;  // a local variable: always direct
        Label top, next, exit;
        place(top);
        uint32_t t = temp(1, 1);
        if (constStep) {
            emit(sign > 0 ? Op::Gt : Op::Lt, k, t, ctlOp, end);
        } else {
            // (step > 0 AND ctl > end) OR (step < 0 AND ctl < end)
            uint32_t a = temp(1, 1), b = temp(1, 1), z = constOperand(isUnsigned(k) ? Value::ofUnsigned(k, 0) : Value::ofSigned(k, 0), ct);
            emit(Op::Gt, k, a, step, z);
            emit(Op::Gt, k, b, ctlOp, end);
            emit(Op::And, TypeKind::Bool, a, a, b);
            emit(Op::Lt, k, t, step, z);
            emit(Op::Lt, k, b, ctlOp, end);
            emit(Op::And, TypeKind::Bool, t, t, b);
            emit(Op::Or, TypeKind::Bool, t, t, a);
        }
        jumpTo(Op::Jnz, exit, t);
        loops_.push_back({&exit, &next});
        genStmts(f.body);
        loops_.pop_back();
        place(next);
        setLine(f.loc);
        // The next value; a step past the type's limit ends the loop instead of overflowing
        uint32_t at = emit(Op::ForNext, k, 0, ctlOp, step);
        exit.uses.push_back(at);
        jumpTo(Op::Jmp, top);
        place(exit);
    }

    // ── Places ──

    int bindingOf(const Symbol& s) const {
        if (s.kind != SymKind::External) return -1;
        for (size_t i = 0; i < p_.bindings.size(); i++) {
            if (p_.bindings[i].offset == s.offset) return static_cast<int>(i);
        }
        return -1;
    }

    Place symbolPlace(const Symbol& s) {
        Place p;
        p.type = s.type;
        p.binding = bindingOf(s);
        Seg seg = s.storage == Storage::Self ? Seg::Self : s.storage == Storage::Frame ? Seg::Frame : Seg::Global;
        if (s.kind == SymKind::InOut) {
            p.indirect = true;
            p.op = operand(seg, s.offset);
            return p;
        }
        p.op = operand(seg, s.offset);
        return p;
    }

    Place placeOf(Expr& e) {
        switch (e.kind) {
            case ExprKind::Name: return symbolPlace(*e.as<Name>().symbol);
            case ExprKind::Member: {
                auto& m = e.as<Member>();
                Place p = placeOf(*m.base);
                if (p.indirect) p.offset += m.offset;
                else p.op += m.offset;
                p.type = e.type;
                return p;
            }
            case ExprKind::Index: {
                auto& ix = e.as<Index>();
                Place p = placeOf(*ix.base);
                const Type* at = baseOf(ix.base->type);
                uint32_t stride = elementStride(at->element);
                std::vector<uint64_t> strides(at->dims.size());
                uint64_t s = stride;
                for (size_t i = at->dims.size(); i-- > 0;) {
                    strides[i] = s;
                    s *= at->dims[i].count();
                }
                for (size_t i = 0; i < ix.indexes.size(); i++) {
                    Value v;
                    if (evaluateConstant(*ix.indexes[i], v)) {
                        uint64_t add = static_cast<uint64_t>(v.i - at->dims[i].low) * strides[i];
                        if (p.indirect) p.offset += static_cast<uint32_t>(add);
                        else p.op += static_cast<uint32_t>(add);
                        continue;
                    }
                    uint32_t idx = gen(*ix.indexes[i]);
                    uint32_t ref = refOf(p);
                    uint32_t out = temp(4, 4);
                    vm::IndexDesc dsc{at->dims[i].low, at->dims[i].count(), static_cast<uint32_t>(strides[i]), 0};
                    emit(Op::Index, TypeKind::Void, out, ref, idx, desc(dsc));
                    p.indirect = true;
                    p.op = out;
                    p.offset = 0;
                }
                p.type = e.type;
                return p;
            }
            default:
                break;
        }
        // Not a variable: compute it into a temporary
        uint32_t v = gen(e);
        Place p;
        p.op = v;
        p.type = e.type;
        return p;
    }

    /// A u32 reference to a place (an address in the data block), in a temporary.
    uint32_t refOf(const Place& p) {
        if (!p.indirect) {
            uint32_t r = temp(4, 4);
            emit(Op::Addr, TypeKind::Void, r, p.op);
            return r;
        }
        if (p.offset == 0) return p.op;
        uint32_t r = temp(4, 4);
        emit(Op::RefAdd, TypeKind::Void, r, p.op, 0, p.offset);
        return r;
    }

    uint32_t load(const Place& p) {
        if (!p.indirect) return p.op;
        uint32_t ref = refOf(p);
        uint32_t t = tempFor(p.type);
        emit(Op::Load, TypeKind::Void, t, ref, 0, p.type->size);
        return t;
    }

    /// Stores `v` (of type `vt`) into place `p`: strings by capacity, subranges checked.
    void store(const Place& p, uint32_t v, const Type* vt) {
        const Type* tt = p.type;
        const Type* bt = baseOf(tt);
        if (bt->kind == TypeKind::String) {
            const Type* sv = baseOf(vt);
            vm::StrDesc dsc{bt->capacity, sv->capacity, 0, 0};
            if (!p.indirect) {
                emit(Op::StrMov, TypeKind::String, p.op, v, 0, desc(dsc));
            } else {
                uint32_t t = tempFor(tt);
                emit(Op::StrMov, TypeKind::String, t, v, 0, desc(dsc));
                emit(Op::Store, TypeKind::Void, refOf(p), t, 0, tt->size);
            }
            return;
        }
        if (tt->hasRange) {
            vm::RangeDesc r{tt->rangeLow, tt->rangeHigh};
            emit(Op::CheckRange, scalarKind(tt), 0, v, 0, desc(r));
        }
        if (!p.indirect) emit(Op::Mov, TypeKind::Void, p.op, v, 0, tt->size);
        else emit(Op::Store, TypeKind::Void, refOf(p), v, 0, tt->size);
        if (p.binding >= 0) emit(Op::Dirty, TypeKind::Void, static_cast<uint32_t>(p.binding));
    }

    void storeBit(BitAccess& b, uint32_t bit) {
        if (!isTemp(bit)) bit = copyToTemp(bit, b.type);
        Place p = placeOf(*b.base);
        TypeKind k = scalarKind(b.base->type);
        if (!p.indirect) {
            emit(Op::SetBit, k, p.op, bit, 0, static_cast<uint32_t>(b.bit));
            if (p.binding >= 0) emit(Op::Dirty, TypeKind::Void, static_cast<uint32_t>(p.binding));
            return;
        }
        uint32_t ref = refOf(p);
        uint32_t t = tempFor(b.base->type);
        emit(Op::Load, TypeKind::Void, t, ref, 0, b.base->type->size);
        emit(Op::SetBit, k, t, bit, 0, static_cast<uint32_t>(b.bit));
        emit(Op::Store, TypeKind::Void, ref, t, 0, b.base->type->size);
    }

    uint32_t copyToTemp(uint32_t v, const Type* t) {
        uint32_t c = tempFor(t);
        emit(Op::Mov, TypeKind::Void, c, v, 0, t->size);
        return c;
    }

    static bool hasCall(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Call: return true;
            case ExprKind::Member: return hasCall(*e.as<Member>().base);
            case ExprKind::BitAccess: return hasCall(*e.as<BitAccess>().base);
            case ExprKind::Index: {
                if (hasCall(*e.as<Index>().base)) return true;
                for (const ExprPtr& i : e.as<Index>().indexes) {
                    if (hasCall(*i)) return true;
                }
                return false;
            }
            case ExprKind::Unary: return hasCall(*e.as<Unary>().operand);
            case ExprKind::Binary: return hasCall(*e.as<Binary>().lhs) || hasCall(*e.as<Binary>().rhs);
            case ExprKind::Convert: return hasCall(*e.as<ast::Convert>().operand);
            default: return false;
        }
    }

    // ── Expressions ──

    uint32_t gen(Expr& e) {
        if (e.isConstant && e.kind != ExprKind::Call) return constOperand(e.constant, e.type);
        switch (e.kind) {
            case ExprKind::Name:
            case ExprKind::Member:
            case ExprKind::Index:
                return load(placeOf(e));
            case ExprKind::BitAccess: {
                auto& b = e.as<BitAccess>();
                uint32_t base = gen(*b.base);
                uint32_t t = temp(1, 1);
                emit(Op::GetBit, scalarKind(b.base->type), t, base, 0, static_cast<uint32_t>(b.bit));
                return t;
            }
            case ExprKind::Unary: {
                auto& u = e.as<Unary>();
                uint32_t v = gen(*u.operand);
                if (u.op == UnaryOp::Plus) return v;
                uint32_t t = tempFor(e.type);
                emit(u.op == UnaryOp::Neg ? Op::Neg : Op::Not, scalarKind(e.type), t, v);
                return t;
            }
            case ExprKind::Binary: return genBinary(e.as<Binary>());
            case ExprKind::Convert: return genConvert(e.as<ast::Convert>());
            case ExprKind::Call: return genCall(e.as<Call>(), false);
            default:
                return constOperand(e.constant, e.type);
        }
    }

    uint32_t genBinary(Binary& b) {
        uint32_t l = gen(*b.lhs);
        if (hasCall(*b.rhs) && !isTemp(l) && vm::segOf(l) != Seg::Const) l = copyToTemp(l, b.lhs->type);
        uint32_t r = gen(*b.rhs);
        const Type* ot = baseOf(b.operandType);
        TypeKind k = scalarKind(ot);
        uint32_t t = tempFor(b.type);
        if (isComparison(b.op)) {
            Op op = b.op == BinaryOp::Eq ? Op::Eq : b.op == BinaryOp::Ne ? Op::Ne : b.op == BinaryOp::Lt ? Op::Lt
                    : b.op == BinaryOp::Le ? Op::Le : b.op == BinaryOp::Gt ? Op::Gt : Op::Ge;
            if (k == TypeKind::String) {
                vm::StrDesc dsc{0, baseOf(b.lhs->type)->capacity, baseOf(b.rhs->type)->capacity, 0};
                emit(Op::StrCmp, TypeKind::String, t, l, r, desc(dsc),
                     static_cast<uint8_t>(static_cast<int>(op) - static_cast<int>(Op::Eq)));
            } else {
                emit(op, k, t, l, r);
            }
            return t;
        }
        if (b.op == BinaryOp::Pow) {
            emit(Op::Pow, k, t, l, r);
            return t;
        }
        if (isDuration(k) && (b.op == BinaryOp::Mul || b.op == BinaryOp::Div)) {
            bool durLeft = isDuration(scalarKind(b.lhs->type));
            uint32_t dur = durLeft ? l : r;
            uint32_t num = durLeft ? r : l;
            TypeKind nk = scalarKind(durLeft ? b.rhs->type : b.lhs->type);
            emit(b.op == BinaryOp::Mul ? Op::TimeMul : Op::TimeDiv, k, t, dur, num, static_cast<uint32_t>(nk));
            return t;
        }
        Op op = Op::Add;
        switch (b.op) {
            case BinaryOp::Add: op = Op::Add; break;
            case BinaryOp::Sub: op = Op::Sub; break;
            case BinaryOp::Mul: op = Op::Mul; break;
            case BinaryOp::Div: op = Op::Div; break;
            case BinaryOp::Mod: op = Op::Mod; break;
            case BinaryOp::And: op = Op::And; break;
            case BinaryOp::Or: op = Op::Or; break;
            case BinaryOp::Xor: op = Op::Xor; break;
            default: break;
        }
        emit(op, k, t, l, r);
        return t;
    }

    uint32_t genConvert(ast::Convert& c) {
        uint32_t v = gen(*c.operand);
        const Type* from = baseOf(c.operand->type);
        const Type* to = baseOf(c.type);
        TypeKind fk = scalarKind(from), tk = scalarKind(to);
        if (fk == TypeKind::String && tk == TypeKind::String) return v;
        uint32_t t = tempFor(c.type);
        if (fk == TypeKind::String) {
            vm::StrDesc dsc{0, from->capacity, 0, 0};
            emit(Op::StrToValue, tk, t, v, 0, desc(dsc));
            return t;
        }
        if (tk == TypeKind::String) {
            vm::StrDesc dsc{to->capacity, 0, 0, 0};
            emit(Op::StrFromValue, fk, t, v, 0, desc(dsc));
            return t;
        }
        if (fk == tk) return v;
        emit(Op::Conv, fk, t, v, 0, static_cast<uint32_t>(tk), c.truncate ? vm::kConvTruncate : 0);
        return t;
    }

    // ── Calls ──

    /// Evaluates `exprs` left to right; values that a later call could change are copied first.
    std::vector<uint32_t> evalAll(const std::vector<Expr*>& exprs) {
        std::vector<uint32_t> ops(exprs.size());
        for (size_t i = 0; i < exprs.size(); i++) {
            ops[i] = gen(*exprs[i]);
            bool laterCall = false;
            for (size_t j = i + 1; j < exprs.size(); j++) laterCall = laterCall || hasCall(*exprs[j]);
            if (laterCall && !isTemp(ops[i]) && vm::segOf(ops[i]) != Seg::Const) ops[i] = copyToTemp(ops[i], exprs[i]->type);
        }
        return ops;
    }

    /// Copies a value of type `vt` into a slot of type `slot` at operand `dst` (strings by capacity).
    void copyValue(uint32_t dst, const Type* slot, uint32_t v, const Type* vt) {
        const Type* bs = baseOf(slot);
        if (bs->kind == TypeKind::String) {
            vm::StrDesc dsc{bs->capacity, baseOf(vt)->capacity, 0, 0};
            emit(Op::StrMov, TypeKind::String, dst, v, 0, desc(dsc));
            return;
        }
        if (slot->hasRange) {
            vm::RangeDesc r{slot->rangeLow, slot->rangeHigh};
            emit(Op::CheckRange, scalarKind(slot), 0, v, 0, desc(r));
        }
        emit(Op::Mov, TypeKind::Void, dst, v, 0, slot->size);
    }

    /// Converts `v` from type `from` to the place's type if they differ (an output widening).
    uint32_t widen(uint32_t v, const Type* from, const Type* to) {
        TypeKind fk = scalarKind(from), tk = scalarKind(to);
        if (fk == tk || fk == TypeKind::String) return v;
        uint32_t t = tempFor(to);
        emit(Op::Conv, fk, t, v, 0, static_cast<uint32_t>(tk));
        return t;
    }

    /// EN/ENO around a standard function (StdFn::Eval): a fault inside sets ENO := FALSE and the
    /// result to its type's default; EN = FALSE skips the evaluation.
    uint32_t genWrapped(Call& c) {
        Expr& inner = *c.args[0].value;
        uint32_t result = tempFor(c.type);
        Label skip, handler, done, end;
        if (c.enArg >= 0) {
            uint32_t en = gen(*c.args[static_cast<size_t>(c.enArg)].value);
            jumpTo(Op::Jz, skip, en);
        }
        uint32_t enoTemp = 0;
        if (c.enoArg >= 0) {
            enoTemp = temp(1, 1);
            handler.uses.push_back(emit(Op::Try, TypeKind::Void, 0, enoTemp));
        }
        uint32_t v = gen(inner);
        copyValue(result, c.type, v, inner.type);
        if (c.enoArg >= 0) {
            emit(Op::EndTry);
            emit(Op::Mov, TypeKind::Void, enoTemp, constBool(true), 0, 1);
            jumpTo(Op::Jmp, done);
            place(handler);
            initVariable(result, c.type, nullptr);  // the type's default (a subrange: its lower limit)
            place(done);
            Expr& eno = *c.args[static_cast<size_t>(c.enoArg)].value;
            store(placeOf(eno), enoTemp, eno.type);
        }
        if (c.enArg >= 0) {
            jumpTo(Op::Jmp, end);
            place(skip);
            initVariable(result, c.type, nullptr);  // the type's default (a subrange: its lower limit)
            if (c.enoArg >= 0) {
                Expr& eno = *c.args[static_cast<size_t>(c.enoArg)].value;
                store(placeOf(eno), constBool(false), eno.type);
            }
            place(end);
        }
        return result;
    }

    uint32_t genCall(Call& c, bool statement) {
        setLine(c.loc);
        if (c.callKind == CallKind::Standard && c.standard == static_cast<int>(StdFn::Eval)) return genWrapped(c);
        if (c.callKind == CallKind::Standard) return genStd(c);
        PouInfo& pou = *c.pou;
        bool block = c.callKind == CallKind::Block;
        calls_[pou_].insert(&pou);

        Place inst;
        if (block) inst = placeOf(*c.callee);

        // Inputs and in-out references, evaluated before anything is written
        std::vector<Expr*> inputs;
        std::vector<size_t> inputArg;
        for (size_t i = 0; i < c.args.size(); i++) {
            Symbol* p = c.argParam[i];
            if (static_cast<int>(i) == c.enArg) {
                inputs.push_back(c.args[i].value.get());
                inputArg.push_back(i);
            } else if (p && p->kind == SymKind::Input) {
                inputs.push_back(c.args[i].value.get());
                inputArg.push_back(i);
            }
        }
        std::vector<uint32_t> values = evalAll(inputs);
        std::map<size_t, uint32_t> refs;
        for (size_t i = 0; i < c.args.size(); i++) {
            Symbol* p = c.argParam[i];
            if (p && p->kind == SymKind::InOut) refs[i] = refOf(placeOf(*c.args[i].value));
        }

        Label skip, done;
        uint32_t enOp = 0;
        bool hasEn = c.enArg >= 0;
        for (size_t k = 0; k < inputArg.size(); k++) {
            if (static_cast<int>(inputArg[k]) == c.enArg) enOp = values[k];
        }
        if (hasEn) jumpTo(Op::Jz, skip, enOp);

        // ENO: a caught fault undoes the call: the called block's instance, its in-out arguments
        // and the tag mirrors are restored from snapshots taken here
        uint32_t enoTemp = 0;
        Label handler;
        uint32_t instSnap = 0, globSnap = 0;
        std::vector<std::pair<size_t, uint32_t>> inOutSnaps;
        if (c.enoArg >= 0) {
            if (block) {
                instSnap = temp(pou.instanceSize, 8);
                if (!inst.indirect) emit(Op::Mov, TypeKind::Void, instSnap, inst.op, 0, pou.instanceSize);
                else emit(Op::Load, TypeKind::Void, instSnap, refOf(inst), 0, pou.instanceSize);
            }
            if (p_.globalsSize > 0) {
                globSnap = temp(p_.globalsSize, 8);
                emit(Op::Mov, TypeKind::Void, globSnap, operand(Seg::Global, 0), 0, p_.globalsSize);
            }
            for (auto& [i, ref] : refs) {
                const Type* pt = c.argParam[i]->type;
                uint32_t snap = temp(pt->size, 8);
                emit(Op::Load, TypeKind::Void, snap, ref, 0, pt->size);
                inOutSnaps.push_back({i, snap});
            }
            enoTemp = temp(1, 1);
            uint32_t at = emit(Op::Try, TypeKind::Void, 0, enoTemp);
            handler.uses.push_back(at);
        }

        auto paramPlace = [&](const Symbol& p) -> Place {
            Place pl;
            pl.type = p.type;
            if (!block) {
                pl.op = operand(Seg::Next, p.offset);
                return pl;
            }
            pl = inst;
            if (pl.indirect) pl.offset += p.offset;
            else pl.op += p.offset;
            pl.type = p.type;
            return pl;
        };

        // Write inputs; formal calls of functions give missing inputs their initial values
        std::set<const Symbol*> given;
        for (size_t k = 0; k < inputArg.size(); k++) {
            size_t i = inputArg[k];
            if (static_cast<int>(i) == c.enArg) continue;
            Symbol* p = c.argParam[i];
            given.insert(p);
            Place pl = paramPlace(*p);
            if (!pl.indirect) copyValue(pl.op, p->type, values[k], c.args[i].value->type);
            else store(pl, values[k], c.args[i].value->type);
        }
        if (!block) {
            for (Symbol* p : pou.inputs) {
                if (!given.count(p)) initVariable(operand(Seg::Next, p->offset), p->type, p->init);
            }
        }
        std::vector<int> inOutBindings;  // tags passed by reference: marked after the call
        for (auto& [i, ref] : refs) {
            // Tags are elementary, so a tag passed by reference is always a plain name
            const Expr& actual = *c.args[i].value;
            if (actual.kind == ExprKind::Name && actual.as<Name>().symbol) {
                int bnd = bindingOf(*actual.as<Name>().symbol);
                if (bnd >= 0) inOutBindings.push_back(bnd);
            }
        }
        for (auto& [i, ref] : refs) {
            Symbol* p = c.argParam[i];
            Place pl = paramPlace(*p);
            pl.type = p_.types.elementary(TypeKind::UDInt);
            if (!pl.indirect) emit(Op::Mov, TypeKind::Void, pl.op, ref, 0, 4);
            else emit(Op::Store, TypeKind::Void, refOf(pl), ref, 0, 4);
        }

        if (!block) {
            emit(Op::Call, TypeKind::Void, static_cast<uint32_t>(pou.index));
        } else if (!inst.indirect) {
            emit(Op::CallFb, TypeKind::Void, static_cast<uint32_t>(pou.index), inst.op);
        } else {
            emit(Op::CallFbRef, TypeKind::Void, static_cast<uint32_t>(pou.index), refOf(inst));
        }
        if (c.enoArg >= 0) {
            emit(Op::EndTry);
            emit(Op::Mov, TypeKind::Void, enoTemp, constBool(true), 0, 1);
        }
        for (int b : inOutBindings) emit(Op::Dirty, TypeKind::Void, static_cast<uint32_t>(b));

        // Result and outputs: copied out of the callee frame before anything else can call
        uint32_t result = 0;
        if (!block && pou.resultSymbol) {
            result = tempFor(pou.result);
            emit(Op::Mov, TypeKind::Void, result, operand(Seg::Next, pou.resultSymbol->offset), 0, pou.result->size);
        }
        std::vector<std::pair<size_t, uint32_t>> outs;
        for (size_t i = 0; i < c.args.size(); i++) {
            Symbol* p = c.argParam[i];
            if (!p || p->kind != SymKind::Output) continue;
            Place pl = paramPlace(*p);
            uint32_t v = load(pl);
            if (!isTemp(v)) v = copyToTemp(v, p->type);
            outs.push_back({i, v});
        }
        for (auto& [i, v] : outs) {
            Expr& target = *c.args[i].value;
            Symbol* p = c.argParam[i];
            uint32_t w = widen(v, p->type, target.type);
            store(placeOf(target), w, baseOf(p->type)->kind == TypeKind::String ? p->type : target.type);
        }
        if (c.enoArg >= 0) {
            jumpTo(Op::Jmp, done);
            place(handler);  // the VM wrote FALSE to enoTemp
            if (block) {
                if (!inst.indirect) emit(Op::Mov, TypeKind::Void, inst.op, instSnap, 0, pou.instanceSize);
                else emit(Op::Store, TypeKind::Void, refOf(inst), instSnap, 0, pou.instanceSize);
            }
            if (globSnap) emit(Op::Mov, TypeKind::Void, operand(Seg::Global, 0), globSnap, 0, p_.globalsSize);
            for (auto& [i, snap] : inOutSnaps) emit(Op::Store, TypeKind::Void, refs[i], snap, 0, c.argParam[i]->type->size);
            if (result) initVariable(result, pou.result, nullptr);
            place(done);
            Expr& eno = *c.args[static_cast<size_t>(c.enoArg)].value;
            store(placeOf(eno), enoTemp, eno.type);
        }
        if (hasEn) {
            Label end;
            jumpTo(Op::Jmp, end);
            place(skip);
            if (result) initVariable(result, pou.result, nullptr);
            if (c.enoArg >= 0) {
                Expr& eno = *c.args[static_cast<size_t>(c.enoArg)].value;
                store(placeOf(eno), constBool(false), eno.type);
            }
            place(end);
        }
        (void)statement;
        return result;
    }

    uint32_t lintBlock(uint32_t a, uint32_t b) {
        uint32_t blk = temp(16, 8);
        emit(Op::Mov, TypeKind::Void, blk, a, 0, 8);
        emit(Op::Mov, TypeKind::Void, blk + 8, b, 0, 8);
        return blk;
    }

    uint32_t genStd(Call& c) {
        StdFn fn = static_cast<StdFn>(c.standard);
        std::vector<Expr*> args;
        for (Arg& a : c.args) args.push_back(a.value.get());
        if (fn == StdFn::Now) {
            uint32_t t = temp(8, 8);
            emit(Op::Now, TypeKind::Time, t);
            return t;
        }
        if (fn == StdFn::FirstCycle) {
            uint32_t t = temp(1, 1);
            emit(Op::FirstCycle, TypeKind::Bool, t);
            return t;
        }
        std::vector<uint32_t> v = evalAll(args);
        const Type* ot = c.operandType ? baseOf(c.operandType) : nullptr;
        TypeKind k = ot ? scalarKind(ot) : TypeKind::Void;
        auto cap = [&](size_t i) { return baseOf(args[i]->type)->capacity; };
        auto toInt = [&](uint32_t lint) {  // to the call's INT or DINT result (sema picks the width)
            TypeKind rk = scalarKind(c.type);
            uint32_t t = tempFor(c.type);
            emit(Op::Conv, TypeKind::LInt, t, lint, 0, static_cast<uint32_t>(rk));
            return t;
        };
        switch (fn) {
            case StdFn::Abs: {
                uint32_t t = tempFor(ot);
                emit(Op::Abs, k, t, v[0]);
                return t;
            }
            case StdFn::Sqrt: case StdFn::Ln: case StdFn::Log: case StdFn::Exp: case StdFn::Sin: case StdFn::Cos:
            case StdFn::Tan: case StdFn::Asin: case StdFn::Acos: case StdFn::Atan: {
                vm::MathFn m = fn == StdFn::Sqrt ? vm::MathFn::Sqrt : fn == StdFn::Ln ? vm::MathFn::Ln
                               : fn == StdFn::Log ? vm::MathFn::Log : fn == StdFn::Exp ? vm::MathFn::Exp
                               : fn == StdFn::Sin ? vm::MathFn::Sin : fn == StdFn::Cos ? vm::MathFn::Cos
                               : fn == StdFn::Tan ? vm::MathFn::Tan : fn == StdFn::Asin ? vm::MathFn::Asin
                               : fn == StdFn::Acos ? vm::MathFn::Acos : vm::MathFn::Atan;
                uint32_t t = tempFor(ot);
                emit(Op::Math, k, t, v[0], 0, 0, static_cast<uint8_t>(m));
                return t;
            }
            case StdFn::Expt: {
                uint32_t t = tempFor(ot);
                emit(Op::Pow, k, t, v[0], v[1]);
                return t;
            }
            case StdFn::Atan2: {
                uint32_t t = tempFor(ot);
                emit(Op::Atan2, k, t, v[0], v[1]);
                return t;
            }
            case StdFn::Shl: case StdFn::Shr: case StdFn::Rol: case StdFn::Ror: {
                Op op = fn == StdFn::Shl ? Op::Shl : fn == StdFn::Shr ? Op::Shr : fn == StdFn::Rol ? Op::Rol : Op::Ror;
                uint32_t t = tempFor(ot);
                bool unsignedCount = args[1]->type && baseOf(args[1]->type)->kind == TypeKind::ULInt;
                emit(op, k, t, v[0], v[1], 0, unsignedCount ? 1 : 0);
                return t;
            }
            case StdFn::Sel: {
                uint32_t t = tempFor(c.type);
                Label one, end;
                jumpTo(Op::Jnz, one, v[0]);
                copyValue(t, c.type, v[1], args[1]->type);
                jumpTo(Op::Jmp, end);
                place(one);
                copyValue(t, c.type, v[2], args[2]->type);
                place(end);
                return t;
            }
            case StdFn::Max: case StdFn::Min: {
                if (k == TypeKind::String) {
                    uint32_t t = tempFor(ot);
                    copyValue(t, ot, v[0], args[0]->type);
                    for (size_t i = 1; i < v.size(); i++) {
                        uint32_t better = temp(1, 1);
                        Label keep;
                        vm::StrDesc dsc{0, cap(i), ot->capacity, 0};
                        emit(Op::StrCmp, TypeKind::String, better, v[i], t, desc(dsc),
                             static_cast<uint8_t>(fn == StdFn::Max ? vm::CmpKind::Gt : vm::CmpKind::Lt));
                        jumpTo(Op::Jz, keep, better);
                        copyValue(t, ot, v[i], args[i]->type);
                        place(keep);
                    }
                    return t;
                }
                uint32_t t = tempFor(ot);
                emit(Op::Mov, TypeKind::Void, t, v[0], 0, ot->size);
                for (size_t i = 1; i < v.size(); i++) emit(fn == StdFn::Max ? Op::Max : Op::Min, k, t, t, v[i]);
                return t;
            }
            case StdFn::Limit: {
                uint32_t t = tempFor(ot);
                emit(Op::Max, k, t, v[1], v[0]);
                emit(Op::Min, k, t, t, v[2]);
                return t;
            }
            case StdFn::Mux: {
                uint32_t t = tempFor(c.type);
                Label end;
                std::vector<Label> picks(v.size() - 1);
                for (size_t i = 1; i < v.size(); i++) {
                    uint32_t b = temp(1, 1);
                    emit(Op::Eq, TypeKind::LInt, b, v[0], constLint(static_cast<int64_t>(i - 1)));
                    jumpTo(Op::Jnz, picks[i - 1], b);
                }
                emit(Op::FaultIf, TypeKind::Void, constBool(true), 0, 0, 0, static_cast<uint8_t>(vm::Fault::Index));
                for (size_t i = 1; i < v.size(); i++) {
                    place(picks[i - 1]);
                    copyValue(t, c.type, v[i], args[i]->type);
                    jumpTo(Op::Jmp, end);
                }
                place(end);
                return t;
            }
            case StdFn::Gt: case StdFn::Ge: case StdFn::Eq: case StdFn::Le: case StdFn::Lt: case StdFn::Ne: {
                Op op = fn == StdFn::Gt ? Op::Gt : fn == StdFn::Ge ? Op::Ge : fn == StdFn::Eq ? Op::Eq
                        : fn == StdFn::Le ? Op::Le : fn == StdFn::Lt ? Op::Lt : Op::Ne;
                uint32_t res = temp(1, 1);
                for (size_t i = 0; i + 1 < v.size(); i++) {
                    uint32_t t = i == 0 ? res : temp(1, 1);
                    if (k == TypeKind::String) {
                        vm::StrDesc dsc{0, cap(i), cap(i + 1), 0};
                        emit(Op::StrCmp, TypeKind::String, t, v[i], v[i + 1], desc(dsc),
                             static_cast<uint8_t>(static_cast<int>(op) - static_cast<int>(Op::Eq)));
                    } else {
                        emit(op, k, t, v[i], v[i + 1]);
                    }
                    if (i > 0) emit(Op::And, TypeKind::Bool, res, res, t);
                }
                return res;
            }
            case StdFn::Len: {
                uint32_t n = temp(8, 8);
                emit(Op::StrLen, TypeKind::String, n, v[0], 0, desc(vm::StrDesc{0, cap(0), 0, 0}));
                return toInt(n);
            }
            case StdFn::Left: case StdFn::Right: {
                uint32_t t = tempFor(c.type);
                emit(fn == StdFn::Left ? Op::StrLeft : Op::StrRight, TypeKind::String, t, v[0], v[1],
                     desc(vm::StrDesc{baseOf(c.type)->capacity, cap(0), 0, 0}));
                return t;
            }
            case StdFn::Mid: {
                uint32_t t = tempFor(c.type);
                emit(Op::StrMid, TypeKind::String, t, v[0], lintBlock(v[1], v[2]),
                     desc(vm::StrDesc{baseOf(c.type)->capacity, cap(0), 0, 0}));
                return t;
            }
            case StdFn::Concat: {
                uint32_t t = tempFor(c.type);
                uint32_t tc = baseOf(c.type)->capacity;
                emit(Op::StrMov, TypeKind::String, t, v[0], 0, desc(vm::StrDesc{tc, cap(0), 0, 0}));
                for (size_t i = 1; i < v.size(); i++) {
                    emit(Op::StrConcat, TypeKind::String, t, t, v[i], desc(vm::StrDesc{tc, tc, cap(i), 0}));
                }
                return t;
            }
            case StdFn::Insert: {
                uint32_t t = tempFor(c.type);
                emit(Op::StrInsert, TypeKind::String, t, v[0], v[1],
                     desc(vm::StrDesc{baseOf(c.type)->capacity, cap(0), cap(1), v[2]}));
                return t;
            }
            case StdFn::Delete: {
                uint32_t t = tempFor(c.type);
                emit(Op::StrDelete, TypeKind::String, t, v[0], lintBlock(v[1], v[2]),
                     desc(vm::StrDesc{baseOf(c.type)->capacity, cap(0), 0, 0}));
                return t;
            }
            case StdFn::Replace: {
                uint32_t t = tempFor(c.type);
                emit(Op::StrReplace, TypeKind::String, t, v[0], v[1],
                     desc(vm::StrDesc{baseOf(c.type)->capacity, cap(0), cap(1), lintBlock(v[2], v[3])}));
                return t;
            }
            case StdFn::Find: {
                uint32_t n = temp(8, 8);
                emit(Op::StrFind, TypeKind::String, n, v[0], v[1], desc(vm::StrDesc{0, cap(0), cap(1), 0}));
                return toInt(n);
            }
            default:
                break;
        }
        d_.error("E_NOT_SUPPORTED", c.loc, "this standard function is not implemented");
        return constBool(false);
    }

    Program& p_;
    vm::Image& img_;
    Diagnostics& d_;
    std::map<std::string, uint32_t> constIndex_;
    PouInfo* pou_ = nullptr;
    uint32_t frameBase_ = 0, frameTop_ = 0, frameMax_ = 0;
    std::vector<Loop> loops_;
    Label* returnLabel_ = nullptr;
    std::map<PouInfo*, std::set<PouInfo*>> calls_;
    std::vector<bool> stored_;  // per binding: some code stores to it (Dirty)
};

}  // namespace

bool generate(Program& program, vm::Image& image, Diagnostics& diags) {
    return CodeGen(program, image, diags).run();
}

}  // namespace simpleplc::st
