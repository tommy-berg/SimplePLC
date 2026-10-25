// Constant folding over typed expressions. Uses the same arithmetic as the
// virtual machine (vm/arith.h), so a folded constant equals the run-time value.

#include <cmath>

#include "st/sema.h"
#include "vm/arith.h"

namespace simpleplc::st {

using namespace ast;

namespace {

const Type* valueType(const Expr& e) {
    const Type* t = baseOf(e.type);
    return t;
}

bool convertValue(const Value& in, const Type* to, bool truncate, Value& out) {
    to = baseOf(to);
    TypeKind k = to->kind == TypeKind::Enum ? to->enumBase->kind : to->kind;
    if (k == TypeKind::String || in.kind == TypeKind::String) {
        if (k == TypeKind::String && in.kind == TypeKind::String) {
            out = in;
            return true;
        }
        return false;  // number <-> text: folded at run time only
    }
    if (!isElementary(k) || !isElementary(in.kind)) return false;
    vm::Scalar s;
    if (isReal(in.kind)) s.real = in.r;
    else s.bits = in.bits();
    vm::Scalar r;
    if (vm::convertScalar(in.kind, s, k, truncate, r) != vm::Fault::None) return false;
    out = Value{};
    out.kind = k;
    if (isReal(k)) out = Value::ofReal(k, r.real);
    else if (isUnsigned(k) || isBitString(k)) out.u = r.bits;
    else out.i = static_cast<int64_t>(r.bits);
    return true;
}

bool compareValues(BinaryOp op, const Value& a, const Value& b, TypeKind k) {
    int c = 0;
    if (isReal(k)) c = a.r < b.r ? -1 : a.r > b.r ? 1 : 0;
    else if (isUnsigned(k) || isBitString(k)) c = a.u < b.u ? -1 : a.u > b.u ? 1 : 0;
    else if (k == TypeKind::String) c = a.s < b.s ? -1 : a.s > b.s ? 1 : 0;
    else c = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
    switch (op) {
        case BinaryOp::Eq: return c == 0;
        case BinaryOp::Ne: return c != 0;
        case BinaryOp::Lt: return c < 0;
        case BinaryOp::Gt: return c > 0;
        case BinaryOp::Le: return c <= 0;
        default: return c >= 0;
    }
}

}  // namespace

bool evaluateConstant(const Expr& e, Value& out, int* fault) {
    auto failed = [&](vm::Fault f) {
        if (fault) *fault = static_cast<int>(f);
        return false;
    };
    if (e.isConstant) {
        out = e.constant;
        return true;
    }
    const Type* t = valueType(e);
    if (!t) return false;
    switch (e.kind) {
        case ExprKind::Convert: {
            const auto& c = e.as<ast::Convert>();
            Value in;
            if (!evaluateConstant(*c.operand, in, fault)) return false;
            if (!convertValue(in, t, c.truncate, out)) {
                if (in.kind != TypeKind::String && baseOf(t)->kind != TypeKind::String) return failed(vm::Fault::Conversion);
                return false;
            }
            return true;
        }
        case ExprKind::Unary: {
            const auto& u = e.as<Unary>();
            Value v;
            if (!evaluateConstant(*u.operand, v, fault)) return false;
            TypeKind k = t->kind;
            switch (u.op) {
                case UnaryOp::Plus: out = v; return true;
                case UnaryOp::Neg:
                    if (isReal(k)) { out = Value::ofReal(k, -v.r); return true; }
                    if (isSigned(k) || isDuration(k)) {
                        int64_t r = 0;
                        if (vm::signedOp('-', k, 0, v.i, r) != vm::Fault::None) return failed(vm::Fault::Overflow);
                        out = Value::ofSigned(k, r);
                        return true;
                    }
                    return false;
                case UnaryOp::Not:
                    if (k == TypeKind::Bool) { out = Value::ofBool(v.i == 0); return true; }
                    if (isBitString(k)) { out = Value::ofUnsigned(k, ~v.u & maxValue(k)); return true; }
                    return false;
            }
            return false;
        }
        case ExprKind::Binary: {
            const auto& b = e.as<Binary>();
            Value l, r;
            if (!evaluateConstant(*b.lhs, l, fault) || !evaluateConstant(*b.rhs, r, fault)) return false;
            const Type* ot = baseOf(b.operandType ? b.operandType : t);
            TypeKind k = ot->kind;
            if (isComparison(b.op)) {
                out = Value::ofBool(compareValues(b.op, l, r, k));
                return true;
            }
            char op = 0;
            switch (b.op) {
                case BinaryOp::Add: op = '+'; break;
                case BinaryOp::Sub: op = '-'; break;
                case BinaryOp::Mul: op = '*'; break;
                case BinaryOp::Div: op = '/'; break;
                case BinaryOp::Mod: op = '%'; break;
                case BinaryOp::And:
                case BinaryOp::Or:
                case BinaryOp::Xor: {
                    uint64_t x = k == TypeKind::Bool ? static_cast<uint64_t>(l.i) : l.u;
                    uint64_t y = k == TypeKind::Bool ? static_cast<uint64_t>(r.i) : r.u;
                    uint64_t z = b.op == BinaryOp::And ? (x & y) : b.op == BinaryOp::Or ? (x | y) : (x ^ y);
                    out = k == TypeKind::Bool ? Value::ofBool(z != 0) : Value::ofUnsigned(k, z);
                    return true;
                }
                default: return false;  // ** and duration scaling are left to run time
            }
            if (isDuration(t->kind) && !isDuration(baseOf(b.rhs->type)->kind)) return false;
            if (isReal(k)) {
                if (k == TypeKind::Real) {
                    float f = 0;
                    if (vm::Fault x = vm::realOp(op, static_cast<float>(l.r), static_cast<float>(r.r), f); x != vm::Fault::None) return failed(x);
                    out = Value::ofReal(k, f);
                } else {
                    double d = 0;
                    if (vm::Fault x = vm::realOp(op, l.r, r.r, d); x != vm::Fault::None) return failed(x);
                    out = Value::ofReal(k, d);
                }
                return true;
            }
            if (isUnsigned(k)) {
                uint64_t z = 0;
                if (vm::Fault x = vm::unsignedOp(op, k, l.u, r.u, z); x != vm::Fault::None) return failed(x);
                out = Value::ofUnsigned(k, z);
                return true;
            }
            if (isSigned(k) || isDuration(k)) {
                int64_t z = 0;
                if (vm::Fault x = vm::signedOp(op, k, l.i, r.i, z); x != vm::Fault::None) return failed(x);
                out = Value::ofSigned(k, z);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

}  // namespace simpleplc::st
