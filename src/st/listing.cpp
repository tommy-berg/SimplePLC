#include "st/listing.h"

#include <format>

#include "st/sema.h"

namespace simpleplc::st {

using namespace ast;

namespace {

std::string kindName(const Type* t) {
    const Type* b = baseOf(t);
    if (!b) return "?";
    if (b->kind == TypeKind::String) return "STRING";
    if (b->kind == TypeKind::Enum) return b->name;
    return b->name;
}

class Lister {
public:
    std::string out;

    /// A type as written, with constants in their analysed form.
    std::string typeSpec(const TypeSpec& t, int depth) {
        switch (t.kind) {
            case TypeSpecKind::Named: return t.spelled;
            case TypeSpecKind::String: return t.length ? std::format("STRING[{}]", expr(*t.length)) : "STRING";
            case TypeSpecKind::Array: {
                std::string r;
                for (const ArrayRange& a : t.ranges) r += std::format("{}{}..{}", r.empty() ? "" : ", ", expr(*a.low), expr(*a.high));
                return std::format("ARRAY[{}] OF {}", r, t.element ? typeSpec(*t.element, depth) : "?");
            }
            case TypeSpecKind::Subrange: return std::format("{}({}..{})", t.spelled, expr(*t.low), expr(*t.high));
            case TypeSpecKind::Enum: {
                std::string v;
                for (const EnumValue& e : t.values) {
                    v += (v.empty() ? "" : ", ") + e.spelled;
                    if (e.value) v += " := " + expr(*e.value);
                }
                return std::format("{}({})", t.spelled.empty() ? "" : t.spelled + " ", v);
            }
            case TypeSpecKind::Struct: {
                std::string ind(static_cast<size_t>(depth + 1) * 4, ' ');
                std::string f = "STRUCT\n";
                for (const VarDecl& d : t.fields) {
                    for (const DeclName& n : d.names) {
                        f += std::format("{}{} : {}{};\n", ind, n.spelled, d.type ? typeSpec(*d.type, depth + 1) : "?",
                                         d.init ? " := " + init(*d.init) : "");
                    }
                }
                return f + std::string(static_cast<size_t>(depth) * 4, ' ') + "END_STRUCT";
            }
        }
        return "?";
    }

    std::string init(const Initializer& i) {
        switch (i.kind) {
            case Initializer::Kind::Expr: return i.expr ? expr(*i.expr) : "?";
            case Initializer::Kind::Array: {
                std::string s;
                for (const ArrayInitElement& e : i.elements) {
                    s += s.empty() ? "" : ", ";
                    if (e.count) s += std::format("{}({})", expr(*e.count), e.value ? init(*e.value) : "");
                    else s += e.value ? init(*e.value) : "";
                }
                return "[" + s + "]";
            }
            case Initializer::Kind::Struct: {
                std::string s;
                for (const StructInitField& f : i.fields) {
                    s += std::format("{}{} := {}", s.empty() ? "" : ", ", f.spelled, f.value ? init(*f.value) : "?");
                }
                return "(" + s + ")";
            }
        }
        return "?";
    }

    void types(const File& f) {
        for (const TypeDecl& t : f.types) {
            out += std::format("TYPE {} : {}{};\nEND_TYPE\n\n", t.spelled, t.spec ? typeSpec(*t.spec, 0) : "?",
                               t.init ? " := " + init(*t.init) : "");
        }
        for (const VarSection& s : f.constants) {
            out += "VAR_GLOBAL CONSTANT\n";
            for (const VarDecl& d : s.decls) {
                for (const DeclName& n : d.names) {
                    out += std::format("    {} : {}{};\n", n.spelled, d.type ? typeSpec(*d.type, 1) : "?",
                                       d.init ? " := " + init(*d.init) : "");
                }
            }
            out += "END_VAR\n\n";
        }
    }

    void pou(const PouInfo& p) {
        out += std::format("{} {}", pouKeyword(p.kind), p.spelled);
        if (p.kind == PouKind::Function && p.result && p.result->kind != TypeKind::Void) {
            out += std::format(" : {}", p.result->name);
        }
        out += "\n";
        for (const VarSection& s : p.ast->sections) {
            out += std::format("    {}", varKindKeyword(s.kind));
            if (s.constant) out += " CONSTANT";
            if (s.retain) out += " RETAIN";
            if (s.nonRetain) out += " NON_RETAIN";
            out += "\n";
            for (const VarDecl& d : s.decls) {
                for (const DeclName& n : d.names) {
                    out += std::format("        {} : {}{};\n", n.spelled, d.type ? typeSpec(*d.type, 2) : "?",
                                       d.init ? " := " + init(*d.init) : "");
                }
            }
            out += "    END_VAR\n";
        }
        stmts(p.ast->body, 1);
        out += std::format("END_{}\n\n", pouKeyword(p.kind));
    }

    std::string expr(const Expr& e) {
        if (e.isConstant && e.type && baseOf(e.type)->kind == TypeKind::Enum) {
            // An enumeration value by its name, qualified
            for (const EnumConstant& v : baseOf(e.type)->enumValues) {
                if (v.value == e.constant.i) return std::format("{}#{}", baseOf(e.type)->name, v.spelled);
            }
        }
        if (e.isConstant && e.kind != ExprKind::Name && e.kind != ExprKind::Call) return e.constant.text();
        switch (e.kind) {
            case ExprKind::IntLit: return std::to_string(e.as<IntLit>().value);
            case ExprKind::RealLit: return std::format("{}", e.as<RealLit>().value);
            case ExprKind::BoolLit: return e.as<BoolLit>().value ? "TRUE" : "FALSE";
            case ExprKind::StringLit: return Value::ofString(e.as<StringLit>().value).text();
            case ExprKind::TypedLit: return std::format("{}#{}", e.as<TypedLit>().prefix, e.as<TypedLit>().body);
            case ExprKind::Name: {
                const auto& n = e.as<Name>();
                if (!n.symbol && e.isConstant && e.type && baseOf(e.type)->kind == TypeKind::Enum)
                    return std::format("{}#{}", baseOf(e.type)->name, n.spelled);
                return n.spelled;
            }
            case ExprKind::Member: return std::format("{}.{}", expr(*e.as<Member>().base), e.as<Member>().spelled);
            case ExprKind::BitAccess: return std::format("{}.%X{}", expr(*e.as<BitAccess>().base), e.as<BitAccess>().bit);
            case ExprKind::Index: {
                const auto& ix = e.as<Index>();
                std::string s = expr(*ix.base) + "[";
                for (size_t i = 0; i < ix.indexes.size(); i++) s += (i ? ", " : "") + expr(*ix.indexes[i]);
                return s + "]";
            }
            case ExprKind::Call: {
                const auto& c = e.as<Call>();
                if (c.callee->kind == ExprKind::Name && c.callee->as<Name>().upper == "__EVAL") return eval(c);
                std::string s = expr(*c.callee) + "(";
                for (size_t i = 0; i < c.args.size(); i++) {
                    const Arg& a = c.args[i];
                    if (i) s += ", ";
                    std::string name = a.spelled;
                    if (name.empty() && i < c.argParam.size() && c.argParam[i]) name = c.argParam[i]->spelled;
                    if (!name.empty()) s += name + (a.output ? " => " : " := ");
                    s += expr(*a.value);
                }
                return s + ")";
            }
            case ExprKind::Unary: {
                const auto& u = e.as<Unary>();
                return std::format("({}{}{})", opText(u.op), u.op == UnaryOp::Not ? " " : "", expr(*u.operand));
            }
            case ExprKind::Binary: {
                // A chain of one left-associative operator is written flat, (a + b + c): every reader
                // evaluates it left to right, and a long sum does not become hundreds of parentheses
                const auto& b = e.as<Binary>();
                std::vector<const Expr*> terms{b.rhs.get()};
                const Expr* left = b.lhs.get();
                while (b.op != BinaryOp::Pow && !isComparison(b.op) && left->kind == ExprKind::Binary &&
                       left->as<Binary>().op == b.op && !left->isConstant && !left->parenthesized) {
                    terms.push_back(left->as<Binary>().rhs.get());
                    left = left->as<Binary>().lhs.get();
                }
                std::string s = "(" + expr(*left);
                for (auto it = terms.rbegin(); it != terms.rend(); ++it) s += std::format(" {} {}", opText(b.op), expr(**it));
                return s + ")";
            }
            case ExprKind::Convert: {
                const auto& c = e.as<ast::Convert>();
                // Within one type (a subrange to its base, a STRING to another capacity) nothing is converted
                if (kindName(c.operand->type) == kindName(c.type)) return expr(*c.operand);
                return std::format("{}_{}_{}({})", kindName(c.operand->type), c.truncate ? "TRUNC" : "TO", kindName(c.type),
                                   expr(*c.operand));
            }
        }
        return "?";
    }

    /// A call with EN/ENO as written: the standard or conversion function with its EN and ENO; an
    /// operator function (rewritten to an operator) as MOVE of it, which evaluates it the same way.
    std::string eval(const Call& wrapper) {
        std::string control;
        for (size_t i = 1; i < wrapper.args.size(); i++) {
            const Arg& a = wrapper.args[i];
            control += std::format(", {} {} {}", a.output ? "ENO" : "EN", a.output ? "=>" : ":=", expr(*a.value));
        }
        const Expr& inner = *wrapper.args[0].value;
        if (inner.kind == ExprKind::Call) {
            std::string s = expr(inner);
            s.pop_back();  // the closing parenthesis: EN and ENO go inside
            return s + control + ")";
        }
        if (inner.kind == ExprKind::Convert) {
            const auto& c = inner.as<ast::Convert>();
            return std::format("{}_{}_{}(IN := {}{})", kindName(c.operand->type), c.truncate ? "TRUNC" : "TO", kindName(c.type),
                               expr(*c.operand), control);
        }
        return std::format("MOVE(IN := {}{})", expr(inner), control);
    }

    void line(SourceLoc loc, int depth) { out += std::format("{}(*L{}*) ", std::string(static_cast<size_t>(depth) * 4, ' '), loc.line); }

    void stmts(const StmtList& list, int depth) {
        for (const StmtPtr& s : list) {
            if (s) stmt(*s, depth);
        }
    }

    void stmt(const Stmt& s, int depth) {
        std::string ind(static_cast<size_t>(depth) * 4, ' ');
        switch (s.kind) {
            case StmtKind::Assign:
                line(s.loc, depth);
                out += std::format("{} := {};\n", expr(*s.as<AssignStmt>().target), expr(*s.as<AssignStmt>().value));
                return;
            case StmtKind::Call:
                line(s.loc, depth);
                out += expr(*s.as<CallStmt>().call) + ";\n";
                return;
            case StmtKind::If: {
                const auto& i = s.as<IfStmt>();
                for (size_t k = 0; k < i.branches.size(); k++) {
                    line(i.branches[k].condition ? i.branches[k].condition->loc : s.loc, depth);
                    out += std::format("{} {} THEN\n", k == 0 ? "IF" : "ELSIF", expr(*i.branches[k].condition));
                    stmts(i.branches[k].body, depth + 1);
                }
                if (i.hasElse) {
                    out += ind + "ELSE\n";
                    stmts(i.elseBody, depth + 1);
                }
                out += ind + "END_IF;\n";
                return;
            }
            case StmtKind::Case: {
                const auto& c = s.as<CaseStmt>();
                line(s.loc, depth);
                out += std::format("CASE {} OF\n", expr(*c.selector));
                for (const CaseBranch& b : c.branches) {
                    out += ind + "    ";
                    for (size_t k = 0; k < b.labels.size(); k++) {
                        if (k) out += ", ";
                        out += expr(*b.labels[k].low);
                        if (b.labels[k].high) out += ".." + expr(*b.labels[k].high);
                    }
                    out += ":\n";
                    stmts(b.body, depth + 2);
                }
                if (c.hasElse) {
                    out += ind + "ELSE\n";
                    stmts(c.elseBody, depth + 1);
                }
                out += ind + "END_CASE;\n";
                return;
            }
            case StmtKind::For: {
                const auto& f = s.as<ForStmt>();
                line(s.loc, depth);
                out += std::format("FOR {} := {} TO {}", f.variable ? expr(*f.variable) : "?", expr(*f.start), expr(*f.end));
                if (f.step) out += " BY " + expr(*f.step);
                out += " DO (* end and step are evaluated once *)\n";
                stmts(f.body, depth + 1);
                out += ind + "END_FOR;\n";
                return;
            }
            case StmtKind::While:
                line(s.loc, depth);
                out += std::format("WHILE {} DO\n", expr(*s.as<WhileStmt>().condition));
                stmts(s.as<WhileStmt>().body, depth + 1);
                out += ind + "END_WHILE;\n";
                return;
            case StmtKind::Repeat:
                line(s.loc, depth);
                out += "REPEAT\n";
                stmts(s.as<RepeatStmt>().body, depth + 1);
                out += std::format("{}UNTIL {}\n{}END_REPEAT;\n", ind, expr(*s.as<RepeatStmt>().condition), ind);
                return;
            case StmtKind::Exit: line(s.loc, depth); out += "EXIT;\n"; return;
            case StmtKind::Continue: line(s.loc, depth); out += "CONTINUE;\n"; return;
            case StmtKind::Return: line(s.loc, depth); out += "RETURN;\n"; return;
            case StmtKind::Empty: return;
        }
    }
};

}  // namespace

std::string listExpression(const ast::Expr& e) {
    Lister l;
    return l.expr(e);
}

std::string listing(const Program& program) {
    Lister l;
    for (size_t i = 0; i + 1 < program.files.size(); i++) l.types(program.files[i]);  // the last file is the standard library
    for (const PouInfo& p : program.pous) {
        if (p.library || !p.checked) continue;
        l.pou(p);
    }
    return l.out;
}

}  // namespace simpleplc::st
