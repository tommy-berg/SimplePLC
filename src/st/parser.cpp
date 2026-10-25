#include "st/parser.h"

#include <charconv>

#include <format>
#include <initializer_list>

#include "st/lexer.h"

namespace simpleplc::st {

using namespace ast;

namespace {

/// Binding power of binary operators (IEC 61131-3 Ed. 3, Table 71): higher binds tighter.
int precedence(Tok kind) {
    switch (kind) {
        case Tok::KwOr: return 1;
        case Tok::KwXor: return 2;
        case Tok::KwAnd: case Tok::Ampersand: return 3;
        case Tok::Eq: case Tok::Ne: return 4;
        case Tok::Lt: case Tok::Gt: case Tok::Le: case Tok::Ge: return 5;
        case Tok::Plus: case Tok::Minus: return 6;
        case Tok::Star: case Tok::Slash: case Tok::KwMod: return 7;
        case Tok::Power: return 8;
        default: return 0;
    }
}

BinaryOp binaryOp(Tok kind) {
    switch (kind) {
        case Tok::KwOr: return BinaryOp::Or;
        case Tok::KwXor: return BinaryOp::Xor;
        case Tok::KwAnd: case Tok::Ampersand: return BinaryOp::And;
        case Tok::Eq: return BinaryOp::Eq;
        case Tok::Ne: return BinaryOp::Ne;
        case Tok::Lt: return BinaryOp::Lt;
        case Tok::Gt: return BinaryOp::Gt;
        case Tok::Le: return BinaryOp::Le;
        case Tok::Ge: return BinaryOp::Ge;
        case Tok::Plus: return BinaryOp::Add;
        case Tok::Minus: return BinaryOp::Sub;
        case Tok::Star: return BinaryOp::Mul;
        case Tok::Slash: return BinaryOp::Div;
        case Tok::KwMod: return BinaryOp::Mod;
        default: return BinaryOp::Pow;
    }
}

bool isEndKeyword(Tok k) {
    switch (k) {
        case Tok::KwEndIf: case Tok::KwEndCase: case Tok::KwEndFor: case Tok::KwEndWhile:
        case Tok::KwEndRepeat: case Tok::KwElse: case Tok::KwElsif: case Tok::KwUntil:
        case Tok::KwEndProgram: case Tok::KwEndFunction: case Tok::KwEndFunctionBlock:
        case Tok::KwElseIf:
            return true;
        default:
            return false;
    }
}

bool isTopLevel(Tok k) {
    switch (k) {
        case Tok::KwProgram: case Tok::KwFunction: case Tok::KwFunctionBlock: case Tok::KwType:
        case Tok::KwConfiguration: case Tok::KwClass: case Tok::KwInterface: case Tok::KwNamespace:
        case Tok::KwVarGlobal:
            return true;
        default:
            return false;
    }
}

bool isStatementStart(Tok k) {
    switch (k) {
        case Tok::Identifier: case Tok::KwIf: case Tok::KwCase: case Tok::KwFor: case Tok::KwWhile:
        case Tok::KwRepeat: case Tok::KwExit: case Tok::KwContinue: case Tok::KwReturn: case Tok::Semicolon:
            return true;
        default:
            return false;
    }
}

class Parser {
public:
    Parser(std::vector<Token> tokens, uint16_t file, Diagnostics& diags)
        : toks_(std::move(tokens)), file_(file), d_(diags) {}

    File parseFile() {
        File f;
        f.index = file_;
        while (!at(Tok::End) && !d_.full()) {
            Tok k = cur().kind;
            if (k == Tok::KwType) {
                parseTypeBlock(f);
            } else if (k == Tok::KwProgram) {
                f.pous.push_back(parsePou(PouKind::Program));
            } else if (k == Tok::KwFunction) {
                f.pous.push_back(parsePou(PouKind::Function));
            } else if (k == Tok::KwFunctionBlock) {
                f.pous.push_back(parsePou(PouKind::FunctionBlock));
            } else if (k == Tok::KwVarGlobal) {
                VarSection s;
                if (parseGlobalConstants(s)) f.constants.push_back(std::move(s));
            } else if (isUnsupportedKeyword(k)) {
                notSupported(cur());
                skipTopLevel();
            } else {
                error(cur().loc, std::format("expected PROGRAM, FUNCTION_BLOCK, FUNCTION or TYPE, found {}", describe(cur())));
                skipTopLevel();
            }
        }
        return f;
    }

    ExprPtr parseLoneExpression() {
        ExprPtr e = parseExpr();
        if (!at(Tok::End)) error(cur().loc, std::format("unexpected {} after the expression", describe(cur())));
        return e;
    }

private:
    /// Nesting of expressions, statements, types and initialisers: every level is a recursion in the
    /// parser and later in the analysis and code generation. Deeper input is refused (E_LIMIT) instead
    /// of exhausting the stack; real programs stay far below it.
    static constexpr int kMaxNesting = 100;
    struct Nest {
        Parser& p;
        bool ok;
        explicit Nest(Parser& parser) : p(parser), ok(++parser.depth_ <= kMaxNesting) {
            if (!ok) p.tooDeep(std::format("nested more than {} levels deep", kMaxNesting));
        }
        ~Nest() { --p.depth_; }
        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;
    };

    /// Reports the limit once and stops reading: the rest of the file is not parsed.
    void tooDeep(const std::string& what) {
        if (tooDeep_) return;
        tooDeep_ = true;
        d_.error("E_LIMIT", cur().loc, what, "split the expression or the statements into smaller pieces or functions");
        pos_ = toks_.size() - 1;
    }

    /// An expression built over `children` (null ones ignored): its tree height, limited because the
    /// analysis and code generation recurse along it (a + a + … builds a tree as high as it is long).
    static constexpr uint32_t kMaxExprDepth = 400;
    ExprPtr deepened(ExprPtr e, std::initializer_list<const Expr*> children) {
        uint32_t d = 0;
        for (const Expr* c : children) {
            if (c) d = std::max(d, c->depth);
        }
        e->depth = d + 1;
        if (e->depth > kMaxExprDepth) {
            tooDeep(std::format("an expression more than {} operations deep", kMaxExprDepth));
            return nullptr;
        }
        return e;
    }

    // ── Token access ──
    const Token& cur() const { return toks_[pos_]; }
    const Token& ahead(size_t k) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
    bool at(Tok k) const { return cur().kind == k; }
    const Token& take() {
        const Token& t = toks_[pos_];
        if (pos_ + 1 < toks_.size()) pos_++;
        return t;
    }
    bool accept(Tok k) {
        if (!at(k)) return false;
        take();
        return true;
    }
    bool expect(Tok k, std::string_view context = {}) {
        if (accept(k)) return true;
        std::string msg = std::format("expected {}{}, found {}", quoted(k), context, describe(cur()));
        error(cur().loc, std::move(msg));
        return false;
    }

    static std::string quoted(Tok k) {
        std::string_view n = tokName(k);
        if (!n.empty() && (n.front() == '\'' || n.starts_with("a ") || n.starts_with("an ") || n.starts_with("the ")))
            return std::string(n);
        return std::format("'{}'", n);
    }
    static std::string describe(const Token& t) {
        if (t.kind == Tok::End) return "the end of the file";
        return std::format("'{}'", t.raw);
    }

    void error(SourceLoc loc, std::string msg, std::string hint = {}) {
        // One error per position: cascades from recovery say nothing new; nothing after a limit
        if ((lastErrorPos_ == pos_ && hadError_) || tooDeep_) return;
        hadError_ = true;
        lastErrorPos_ = pos_;
        d_.error("E_SYNTAX", loc, std::move(msg), std::move(hint));
    }
    static bool isKeyword(Tok k) { return k >= Tok::KwProgram; }

    void reserved(const Token& t) {
        d_.error("E_RESERVED", t.loc,
                 std::format("'{}' is a reserved word of IEC 61131-3 and cannot be used as a name", t.raw),
                 std::format("use another name, e.g. {}_no", toUpper(t.raw) == "STEP" ? "step" : std::string(t.raw)));
        hadError_ = true;
        lastErrorPos_ = pos_;
    }

    void notSupported(const Token& t, std::string what = {}) {
        if (what.empty()) what = std::format("'{}'", toUpper(t.raw));
        d_.error("E_NOT_SUPPORTED", t.loc, std::format("{} is not supported by SimplePLC", what),
                 "see docs/st-compliance.md for the supported language");
        hadError_ = true;
        lastErrorPos_ = pos_;
    }

    /// Skips to the end of a statement: past ';', or before a keyword that ends a block.
    void syncStatement() {
        size_t start = pos_;
        while (!at(Tok::End)) {
            if (accept(Tok::Semicolon)) return;
            if (isEndKeyword(cur().kind) || isTopLevel(cur().kind)) break;
            if (pos_ != start && isStatementStart(cur().kind) && cur().kind != Tok::Identifier) break;
            take();
        }
        if (pos_ == start && !at(Tok::End) && !isEndKeyword(cur().kind) && !isTopLevel(cur().kind)) take();
    }

    void skipTopLevel() {
        take();
        while (!at(Tok::End) && !isTopLevel(cur().kind)) take();
    }

    // ── Declarations ──

    void parseTypeBlock(File& f) {
        take();  // TYPE
        while (!at(Tok::KwEndType) && !at(Tok::End) && !isTopLevel(cur().kind) && !d_.full()) {
            if (!at(Tok::Identifier)) {
                error(cur().loc, std::format("expected a type name, found {}", describe(cur())));
                syncDeclaration(Tok::KwEndType);
                continue;
            }
            TypeDecl td;
            const Token& name = take();
            td.loc = name.loc;
            td.upper = name.text;
            td.spelled = std::string(name.raw);
            if (!expect(Tok::Colon, " after the type name")) {
                syncDeclaration(Tok::KwEndType);
                continue;
            }
            td.spec = parseTypeSpec(true);
            bool afterStruct = justClosedStruct_;
            justClosedStruct_ = false;
            if (accept(Tok::Assign)) td.init = parseInitializer();
            // The standard ends every type declaration with ';'; after END_STRUCT it is
            // tolerated without one (common in CODESYS code, and nothing can be misread)
            if (!accept(Tok::Semicolon) && !(afterStruct && (at(Tok::KwEndType) || at(Tok::Identifier)))) {
                expect(Tok::Semicolon, " after the type declaration");
                syncDeclaration(Tok::KwEndType);
            }
            f.types.push_back(std::move(td));
        }
        expect(Tok::KwEndType);
    }

    void syncDeclaration(Tok end) {
        size_t start = pos_;
        while (!at(Tok::End) && !at(end) && !isTopLevel(cur().kind)) {
            if (accept(Tok::Semicolon)) return;
            take();
        }
        if (pos_ == start && !at(Tok::End) && !at(end)) take();
    }

    std::unique_ptr<Pou> parsePou(PouKind kind) {
        auto pou = std::make_unique<Pou>();
        pou->kind = kind;
        pou->loc = take().loc;
        if (at(Tok::Identifier)) {
            const Token& name = take();
            pou->upper = name.text;
            pou->spelled = std::string(name.raw);
        } else if (isKeyword(cur().kind) && cur().kind != Tok::KwVar && !isVarSectionStart(cur().kind)) {
            reserved(cur());
            take();
        } else {
            error(cur().loc, std::format("expected the {} name, found {}", pouKeyword(kind), describe(cur())));
            if (!isStatementStart(cur().kind)) take();
        }
        if (kind == PouKind::Function && accept(Tok::Colon)) pou->returnType = parseTypeSpec(false);

        Tok end = kind == PouKind::Program ? Tok::KwEndProgram
                  : kind == PouKind::Function ? Tok::KwEndFunction : Tok::KwEndFunctionBlock;
        while (isVarSectionStart(cur().kind) && !d_.full()) {
            VarSection s;
            if (parseVarSection(s)) pou->sections.push_back(std::move(s));
        }
        pou->body = parseStatements({end});
        pou->endLoc = cur().loc;
        if (!accept(end)) {
            error(cur().loc, std::format("expected '{}' to end {} '{}', found {}", tokName(end), pouKeyword(kind),
                                         pou->spelled, describe(cur())));
            while (!at(Tok::End) && !isTopLevel(cur().kind)) {
                if (accept(end)) break;
                take();
            }
        }
        return pou;
    }

    static bool isVarSectionStart(Tok k) {
        switch (k) {
            case Tok::KwVar: case Tok::KwVarInput: case Tok::KwVarOutput: case Tok::KwVarInOut:
            case Tok::KwVarTemp: case Tok::KwVarExternal: case Tok::KwVarGlobal: case Tok::KwVarAccess:
            case Tok::KwVarConfig: case Tok::KwVarStat:
                return true;
            default:
                return false;
        }
    }

    /// VAR_GLOBAL CONSTANT … END_VAR at the top of a file: project constants for every POU.
    bool parseGlobalConstants(VarSection& s) {
        const Token& kw = take();
        s.loc = kw.loc;
        s.kind = VarKind::Var;
        if (!accept(Tok::KwConstant)) {
            notSupported(kw, "VAR_GLOBAL without CONSTANT (programs share data through tags: VAR_EXTERNAL)");
            while (!at(Tok::End) && !at(Tok::KwEndVar) && !isTopLevel(cur().kind)) take();
            accept(Tok::KwEndVar);
            return false;
        }
        s.constant = true;
        while (at(Tok::Identifier) && !d_.full()) {
            VarDecl decl;
            if (parseVarDecl(decl)) s.decls.push_back(std::move(decl));
        }
        expect(Tok::KwEndVar, " to end VAR_GLOBAL CONSTANT");
        return true;
    }

    /// Returns false (after skipping it) for an unsupported section.
    bool parseVarSection(VarSection& s) {
        const Token& kw = take();
        s.loc = kw.loc;
        switch (kw.kind) {
            case Tok::KwVar: s.kind = VarKind::Var; break;
            case Tok::KwVarInput: s.kind = VarKind::Input; break;
            case Tok::KwVarOutput: s.kind = VarKind::Output; break;
            case Tok::KwVarInOut: s.kind = VarKind::InOut; break;
            case Tok::KwVarTemp: s.kind = VarKind::Temp; break;
            case Tok::KwVarExternal: s.kind = VarKind::External; break;
            default:
                notSupported(kw, kw.kind == Tok::KwVarGlobal
                                     ? std::string("VAR_GLOBAL (use tags: VAR_EXTERNAL binds a program to the PLC's tags)")
                                     : std::format("'{}'", toUpper(kw.raw)));
                while (!at(Tok::End) && !at(Tok::KwEndVar) && !isTopLevel(cur().kind)) take();
                accept(Tok::KwEndVar);
                return false;
        }
        while (true) {
            if (accept(Tok::KwConstant)) s.constant = true;
            else if (accept(Tok::KwRetain)) s.retain = true;
            else if (accept(Tok::KwNonRetain)) s.nonRetain = true;
            else if (at(Tok::KwPersistent)) { notSupported(cur()); take(); }
            else break;
        }
        while (at(Tok::Identifier) && !d_.full()) {
            VarDecl decl;
            if (parseVarDecl(decl)) s.decls.push_back(std::move(decl));
        }
        if (!accept(Tok::KwEndVar)) {
            if (isKeyword(cur().kind) && !isVarSectionStart(cur().kind) && ahead(1).kind == Tok::Colon) {
                reserved(cur());
            } else {
                error(cur().loc, std::format("expected a variable declaration or END_VAR, found {}", describe(cur())));
            }
            while (!at(Tok::End) && !at(Tok::KwEndVar) && !isTopLevel(cur().kind) && !isStatementStart(cur().kind))
                take();
            accept(Tok::KwEndVar);
        }
        return true;
    }

    bool parseVarDecl(VarDecl& decl) {
        do {
            if (!at(Tok::Identifier)) {
                if (isKeyword(cur().kind)) {
                    reserved(cur());
                } else {
                    error(cur().loc, std::format("expected a variable name, found {}", describe(cur())));
                }
                syncDeclaration(Tok::KwEndVar);
                return false;
            }
            const Token& n = take();
            decl.names.push_back({n.loc, n.text, std::string(n.raw)});
        } while (accept(Tok::Comma));
        if (at(Tok::KwAt)) {
            notSupported(cur(), "AT (direct addresses; bind tags with VAR_EXTERNAL)");
            take();
            if (!at(Tok::Colon)) take();
        }
        if (!expect(Tok::Colon, " after the variable name")) {
            syncDeclaration(Tok::KwEndVar);
            return false;
        }
        decl.type = parseTypeSpec(false);
        if (at(Tok::KwREdge) || at(Tok::KwFEdge)) {
            notSupported(cur());
            take();
        }
        if (accept(Tok::Assign)) {
            decl.init = parseInitializer();
        } else if (at(Tok::Eq)) {
            // A common slip: report it and read the initial value anyway, so later uses are not "undeclared"
            error(cur().loc, "an initial value is written with ':=', not '='", "write x : INT := 5;");
            take();
            decl.init = parseInitializer();
        }
        if (!expect(Tok::Semicolon, " after the declaration")) {
            syncDeclaration(Tok::KwEndVar);
            return false;
        }
        return true;
    }

    TypeSpecPtr parseTypeSpec(bool inTypeDecl) {
        Nest nest(*this);
        if (!nest.ok) return nullptr;
        auto spec = std::make_unique<TypeSpec>();
        spec->loc = cur().loc;
        justClosedStruct_ = false;
        if (accept(Tok::KwString)) {
            spec->kind = TypeSpecKind::String;
            spec->upper = "STRING";
            spec->spelled = "STRING";
            if (accept(Tok::LBracket)) {
                spec->length = parseExpr();
                expect(Tok::RBracket, " after the string length");
            } else if (at(Tok::LParen)) {
                error(cur().loc, "the string length goes in square brackets", "write STRING[80], not STRING(80)");
                take();
                spec->length = parseExpr();
                accept(Tok::RParen);
            }
            return spec;
        }
        if (at(Tok::KwWString) || at(Tok::KwRefTo) || at(Tok::KwRef)) {
            notSupported(cur());
            take();
            if (at(Tok::Identifier)) take();
            spec->upper = "?";
            return spec;
        }
        if (accept(Tok::KwArray)) {
            spec->kind = TypeSpecKind::Array;
            expect(Tok::LBracket, " after ARRAY");
            do {
                if (at(Tok::Star)) {
                    notSupported(cur(), "ARRAY[*] (variable-length arrays)");
                    take();
                    continue;
                }
                ArrayRange r;
                r.low = parseExpr();
                expect(Tok::DotDot, " in the array bounds (low..high)");
                r.high = parseExpr();
                spec->ranges.push_back(std::move(r));
            } while (accept(Tok::Comma));
            expect(Tok::RBracket, " after the array bounds");
            expect(Tok::KwOf, " after the array bounds");
            spec->element = parseTypeSpec(false);
            return spec;
        }
        if (inTypeDecl && accept(Tok::KwStruct)) {
            spec->kind = TypeSpecKind::Struct;
            while (at(Tok::Identifier) && !d_.full()) {
                VarDecl field;
                if (parseVarDecl(field)) spec->fields.push_back(std::move(field));
            }
            expect(Tok::KwEndStruct, " to end the structure");
            justClosedStruct_ = true;
            return spec;
        }
        if (at(Tok::KwStruct)) {
            notSupported(cur(), "a STRUCT declared outside TYPE … END_TYPE");
            while (!at(Tok::End) && !at(Tok::KwEndStruct)) take();
            accept(Tok::KwEndStruct);
            return spec;
        }
        if (at(Tok::LParen)) {
            if (!inTypeDecl) {
                notSupported(cur(), "an enumeration declared outside TYPE … END_TYPE");
            }
            spec->kind = TypeSpecKind::Enum;
            parseEnumValues(*spec);
            return spec;
        }
        if (at(Tok::Identifier)) {
            const Token& n = take();
            spec->upper = n.text;
            spec->spelled = std::string(n.raw);
            if (at(Tok::LParen)) {
                // INT (0..100): subrange; INT (A := 1, B := 2): enumeration with a base type
                bool isEnum = ahead(1).kind == Tok::Identifier &&
                              (ahead(2).kind == Tok::Comma || ahead(2).kind == Tok::RParen ||
                               ahead(2).kind == Tok::Assign);
                if (isEnum) {
                    if (!inTypeDecl) notSupported(cur(), "an enumeration declared outside TYPE … END_TYPE");
                    spec->kind = TypeSpecKind::Enum;
                    parseEnumValues(*spec);
                } else {
                    spec->kind = TypeSpecKind::Subrange;
                    take();
                    spec->low = parseExpr();
                    expect(Tok::DotDot, " in the subrange (low..high)");
                    spec->high = parseExpr();
                    expect(Tok::RParen, " after the subrange");
                }
            }
            return spec;
        }
        error(cur().loc, std::format("expected a type, found {}", describe(cur())));
        spec->upper = "?";
        return spec;
    }

    void parseEnumValues(TypeSpec& spec) {
        expect(Tok::LParen);
        do {
            if (!at(Tok::Identifier)) {
                error(cur().loc, std::format("expected an enumeration value, found {}", describe(cur())));
                break;
            }
            const Token& n = take();
            EnumValue v;
            v.loc = n.loc;
            v.upper = n.text;
            v.spelled = std::string(n.raw);
            if (accept(Tok::Assign)) v.value = parseExpr();
            spec.values.push_back(std::move(v));
        } while (accept(Tok::Comma));
        expect(Tok::RParen, " after the enumeration values");
    }

    InitPtr parseInitializer() {
        Nest nest(*this);
        if (!nest.ok) return nullptr;
        auto init = std::make_unique<Initializer>();
        init->loc = cur().loc;
        if (accept(Tok::LBracket)) {
            init->kind = Initializer::Kind::Array;
            if (!at(Tok::RBracket)) {
                do {
                    ArrayInitElement el;
                    if (at(Tok::IntLiteral) && ahead(1).kind == Tok::LParen) {
                        const Token& n = take();
                        el.count = std::make_unique<IntLit>(n.loc, n.intValue);
                        take();  // (
                        if (!at(Tok::RParen)) el.value = parseInitializer();
                        expect(Tok::RParen, " after the repeated value");
                    } else {
                        el.value = parseInitializer();
                    }
                    init->elements.push_back(std::move(el));
                } while (accept(Tok::Comma));
            }
            expect(Tok::RBracket, " to end the array initial values");
            return init;
        }
        if (at(Tok::LParen) && ahead(1).kind == Tok::Identifier && ahead(2).kind == Tok::Assign) {
            init->kind = Initializer::Kind::Struct;
            take();
            do {
                if (!at(Tok::Identifier)) {
                    error(cur().loc, std::format("expected a structure member name, found {}", describe(cur())));
                    break;
                }
                const Token& n = take();
                StructInitField f;
                f.loc = n.loc;
                f.upper = n.text;
                f.spelled = std::string(n.raw);
                expect(Tok::Assign, " after the member name");
                f.value = parseInitializer();
                init->fields.push_back(std::move(f));
            } while (accept(Tok::Comma));
            expect(Tok::RParen, " to end the structure initial values");
            return init;
        }
        init->expr = parseExpr();
        return init;
    }

    // ── Statements ──

    StmtList parseStatements(std::initializer_list<Tok> stops, bool caseBranch = false) {
        StmtList list;
        while (!at(Tok::End) && !d_.full()) {
            Tok k = cur().kind;
            bool stop = false;
            for (Tok s : stops) stop = stop || k == s;
            if (stop) break;
            if (isTopLevel(k) || k == Tok::KwEndProgram || k == Tok::KwEndFunction ||
                k == Tok::KwEndFunctionBlock)
                break;
            if (caseBranch && isCaseLabelStart()) break;
            if (isEndKeyword(k)) {
                if (k == Tok::KwElseIf) {
                    d_.error("E_SYNTAX", cur().loc, "ELSEIF is not Structured Text", "write ELSIF");
                } else {
                    error(cur().loc, std::format("'{}' does not match an open statement", cur().raw));
                }
                take();
                continue;
            }
            size_t before = pos_;
            StmtPtr s = parseStatement();
            if (s) list.push_back(std::move(s));
            if (pos_ == before) take();  // always make progress
        }
        return list;
    }

    bool isCaseLabelStart() const {
        size_t k = 0;
        if (ahead(0).kind == Tok::Minus || ahead(0).kind == Tok::Plus) k = 1;
        Tok t = ahead(k).kind;
        if (t != Tok::IntLiteral && t != Tok::TypedLiteral && t != Tok::Identifier && t != Tok::RealLiteral)
            return false;
        Tok n = ahead(k + 1).kind;
        return n == Tok::Colon || n == Tok::Comma || n == Tok::DotDot;
    }

    StmtPtr parseStatement() {
        Nest nest(*this);
        if (!nest.ok) return nullptr;
        const Token& t = cur();
        SourceLoc loc = t.loc;
        switch (t.kind) {
            case Tok::Semicolon:
                take();
                return std::make_unique<SimpleStmt>(StmtKind::Empty, loc);
            case Tok::KwIf: return parseIf();
            case Tok::KwCase: return parseCase();
            case Tok::KwFor: return parseFor();
            case Tok::KwWhile: return parseWhile();
            case Tok::KwRepeat: return parseRepeat();
            case Tok::KwExit:
            case Tok::KwContinue:
            case Tok::KwReturn: {
                StmtKind k = t.kind == Tok::KwExit ? StmtKind::Exit
                             : t.kind == Tok::KwContinue ? StmtKind::Continue : StmtKind::Return;
                take();
                endStatement();
                return std::make_unique<SimpleStmt>(k, loc);
            }
            case Tok::Identifier: return parseAssignOrCall();
            default:
                if (isUnsupportedKeyword(t.kind)) {
                    notSupported(t);
                } else {
                    error(loc, std::format("expected a statement, found {}", describe(t)));
                }
                syncStatement();
                return nullptr;
        }
    }

    void endStatement() {
        if (accept(Tok::Semicolon)) return;
        error(cur().loc, std::format("expected ';' after the statement, found {}", describe(cur())),
              "every statement ends with ';'");
        syncStatement();
    }

    StmtPtr parseAssignOrCall() {
        SourceLoc loc = cur().loc;
        ExprPtr target = parsePostfix(parsePrimary());
        if (!target) {
            syncStatement();
            return nullptr;
        }
        if (accept(Tok::Assign)) {
            ExprPtr value = parseExpr();
            endStatement();
            return std::make_unique<AssignStmt>(loc, std::move(target), std::move(value));
        }
        if (target->kind == ExprKind::Call) {
            endStatement();
            return std::make_unique<CallStmt>(loc, std::move(target));
        }
        if (at(Tok::Eq)) {
            error(cur().loc, "expected ':=' to assign", "'=' compares two values; ':=' assigns");
        } else if (at(Tok::Output)) {
            error(cur().loc, "'=>' is only allowed inside a call, for an output");
        } else {
            error(cur().loc, std::format("expected ':=' or a call, found {}", describe(cur())));
        }
        syncStatement();
        return nullptr;
    }

    StmtPtr parseIf() {
        auto s = std::make_unique<IfStmt>(take().loc);
        IfBranch first;
        first.condition = parseExpr();
        expect(Tok::KwThen, " after the IF condition");
        first.body = parseStatements({Tok::KwElsif, Tok::KwElse, Tok::KwEndIf, Tok::KwElseIf});
        s->branches.push_back(std::move(first));
        while (at(Tok::KwElsif) || at(Tok::KwElseIf)) {
            if (at(Tok::KwElseIf)) d_.error("E_SYNTAX", cur().loc, "ELSEIF is not Structured Text", "write ELSIF");
            take();
            IfBranch b;
            b.condition = parseExpr();
            expect(Tok::KwThen, " after the ELSIF condition");
            b.body = parseStatements({Tok::KwElsif, Tok::KwElse, Tok::KwEndIf, Tok::KwElseIf});
            s->branches.push_back(std::move(b));
        }
        if (accept(Tok::KwElse)) {
            s->hasElse = true;
            s->elseBody = parseStatements({Tok::KwEndIf});
        }
        if (expect(Tok::KwEndIf, " to end the IF statement")) endStatement();
        return s;
    }

    StmtPtr parseCase() {
        SourceLoc loc = take().loc;
        auto s = std::make_unique<CaseStmt>(loc, parseExpr());
        expect(Tok::KwOf, " after the CASE selector");
        while (!at(Tok::KwElse) && !at(Tok::KwEndCase) && !at(Tok::End) && !isTopLevel(cur().kind) && !d_.full()) {
            CaseBranch b;
            size_t before = pos_;
            do {
                CaseLabel lab;
                lab.loc = cur().loc;
                lab.low = parseExpr();
                if (accept(Tok::DotDot)) lab.high = parseExpr();
                b.labels.push_back(std::move(lab));
            } while (accept(Tok::Comma));
            if (!expect(Tok::Colon, " after the case labels")) {
                syncStatement();
                if (pos_ == before) take();
                continue;
            }
            b.body = parseStatements({Tok::KwElse, Tok::KwEndCase}, true);
            s->branches.push_back(std::move(b));
        }
        if (accept(Tok::KwElse)) {
            s->hasElse = true;
            s->elseBody = parseStatements({Tok::KwEndCase});
        }
        if (expect(Tok::KwEndCase, " to end the CASE statement")) endStatement();
        return s;
    }

    StmtPtr parseFor() {
        auto s = std::make_unique<ForStmt>(take().loc);
        if (at(Tok::Identifier)) {
            const Token& n = take();
            s->variable = std::make_unique<Name>(n.loc, n.text, std::string(n.raw));
        } else {
            error(cur().loc, std::format("expected the FOR control variable, found {}", describe(cur())));
        }
        if (!at(Tok::Assign) && (at(Tok::Dot) || at(Tok::LBracket))) {
            d_.error("E_FOR_VAR", cur().loc, "the FOR control variable must be a plain local variable",
                     "not an array element or a structure member");
            hadError_ = true;
            lastErrorPos_ = pos_;
            while (!at(Tok::Assign) && !at(Tok::End) && !at(Tok::KwDo)) take();
        }
        expect(Tok::Assign, " after the FOR control variable");
        s->start = parseExpr();
        expect(Tok::KwTo, " in the FOR statement");
        s->end = parseExpr();
        if (accept(Tok::KwBy)) s->step = parseExpr();
        expect(Tok::KwDo, " in the FOR statement");
        s->body = parseStatements({Tok::KwEndFor});
        if (expect(Tok::KwEndFor, " to end the FOR loop")) endStatement();
        return s;
    }

    StmtPtr parseWhile() {
        SourceLoc loc = take().loc;
        auto s = std::make_unique<WhileStmt>(loc, parseExpr());
        expect(Tok::KwDo, " after the WHILE condition");
        s->body = parseStatements({Tok::KwEndWhile});
        if (expect(Tok::KwEndWhile, " to end the WHILE loop")) endStatement();
        return s;
    }

    StmtPtr parseRepeat() {
        auto s = std::make_unique<RepeatStmt>(take().loc);
        s->body = parseStatements({Tok::KwUntil});
        expect(Tok::KwUntil, " in the REPEAT loop");
        s->condition = parseExpr();
        if (expect(Tok::KwEndRepeat, " to end the REPEAT loop")) endStatement();
        return s;
    }

    // ── Expressions ──

    ExprPtr parseExpr() { return parseBinary(1); }

    ExprPtr parseBinary(int minPrec) {
        ExprPtr lhs = parseUnary();
        while (true) {
            int prec = precedence(cur().kind);
            if (prec == 0 || prec < minPrec) break;
            const Token& opTok = take();
            BinaryOp op = binaryOp(opTok.kind);
            ExprPtr rhs = parseBinary(prec + 1);
            if (!lhs || !rhs) return nullptr;
            checkAmbiguity(op, opTok.loc, *lhs, *rhs);
            const Expr* l = lhs.get();
            const Expr* r = rhs.get();
            lhs = deepened(std::make_unique<Binary>(opTok.loc, op, std::move(lhs), std::move(rhs)), {l, r});
            if (!lhs) return nullptr;
        }
        return lhs;
    }

    /// Refuses forms whose meaning depends on the reader.
    void checkAmbiguity(BinaryOp op, SourceLoc loc, const Expr& lhs, const Expr& rhs) {
        auto unparenthesized = [](const Expr& e, ExprKind k) { return e.kind == k && !e.parenthesized; };
        if (op == BinaryOp::Pow) {
            if (unparenthesized(lhs, ExprKind::Unary)) {
                const char* u = opText(lhs.as<Unary>().op);
                d_.error("E_AMBIGUOUS", lhs.loc,
                         std::format("'{0}a ** b' can mean ({0}a) ** b or {0}(a ** b); implementations differ", u),
                         "add parentheses");
            } else if (unparenthesized(lhs, ExprKind::Binary) && lhs.as<Binary>().op == BinaryOp::Pow) {
                d_.error("E_AMBIGUOUS", loc, "'a ** b ** c' can mean (a ** b) ** c or a ** (b ** c)",
                         "add parentheses");
            }
            return;
        }
        if (!isComparison(op)) return;
        for (const Expr* side : {&lhs, &rhs}) {
            if (unparenthesized(*side, ExprKind::Binary) && isComparison(side->as<Binary>().op)) {
                d_.error("E_CHAINED_COMPARISON", loc,
                         "comparisons cannot be chained: 'a < b < c' compares the BOOL result of a < b with c",
                         "write a < b AND b < c, or add parentheses if you mean to compare a BOOL result");
                return;
            }
        }
        if (unparenthesized(lhs, ExprKind::Unary) && lhs.as<Unary>().op == UnaryOp::Not) {
            d_.error("E_AMBIGUOUS", lhs.loc, "'NOT a = b' means (NOT a) = b, which is easy to misread as NOT (a = b)",
                     "add parentheses");
        }
    }

    ExprPtr parseUnary() {
        Nest nest(*this);
        if (!nest.ok) return nullptr;
        if (at(Tok::KwNot) && ahead(1).kind == Tok::LParen && ahead(2).kind == Tok::Identifier &&
            (ahead(3).kind == Tok::Assign || ahead(3).kind == Tok::Output)) {
            // NOT(IN := x): the function form with a formal argument
            const Token& t = take();
            return parsePostfix(std::make_unique<Name>(t.loc, "NOT", std::string(t.raw)));
        }
        if (at(Tok::Minus) || at(Tok::Plus) || at(Tok::KwNot)) {
            const Token& t = take();
            UnaryOp op = t.kind == Tok::Minus ? UnaryOp::Neg : t.kind == Tok::Plus ? UnaryOp::Plus : UnaryOp::Not;
            ExprPtr operand = parseUnary();
            if (!operand) return nullptr;
            const Expr* o = operand.get();
            return deepened(std::make_unique<Unary>(t.loc, op, std::move(operand)), {o});
        }
        return parsePostfix(parsePrimary());
    }

    ExprPtr parsePrimary() {
        const Token& t = cur();
        switch (t.kind) {
            case Tok::IntLiteral: take(); return std::make_unique<IntLit>(t.loc, t.intValue);
            case Tok::RealLiteral: take(); return std::make_unique<RealLit>(t.loc, t.realValue);
            case Tok::KwTrue: take(); return std::make_unique<BoolLit>(t.loc, true);
            case Tok::KwFalse: take(); return std::make_unique<BoolLit>(t.loc, false);
            case Tok::StringLiteral: take(); return std::make_unique<StringLit>(t.loc, t.text);
            case Tok::WStringLiteral:
                notSupported(t, "WSTRING (\"…\" literals)");
                take();
                return std::make_unique<StringLit>(t.loc, t.text);
            case Tok::TypedLiteral:
                take();
                return std::make_unique<TypedLit>(t.loc, t.prefix, t.text, t.quoted);
            case Tok::Identifier:
                take();
                return std::make_unique<Name>(t.loc, t.text, std::string(t.raw));
            case Tok::KwAnd:
            case Tok::KwOr:
            case Tok::KwXor:
            case Tok::KwMod:
                // The operator functions AND(a, b, …), OR(…), XOR(…), MOD(a, b)
                if (ahead(1).kind == Tok::LParen) {
                    take();
                    return std::make_unique<Name>(t.loc, t.text, std::string(t.raw));
                }
                error(t.loc, std::format("expected a value, found {}", describe(t)));
                return nullptr;
            case Tok::LParen: {
                take();
                ExprPtr e = parseExpr();
                expect(Tok::RParen, " to close '('");
                if (e) e->parenthesized = true;
                return e;
            }
            default:
                if (isUnsupportedKeyword(t.kind)) {
                    notSupported(t);
                    take();
                } else {
                    error(t.loc, std::format("expected a value, found {}", describe(t)));
                }
                return nullptr;
        }
    }

    ExprPtr parsePostfix(ExprPtr e) {
        while (e) {
            if (at(Tok::Dot)) {
                SourceLoc loc = take().loc;
                if (at(Tok::Identifier)) {
                    const Token& m = take();
                    const Expr* base = e.get();
                    e = deepened(std::make_unique<Member>(loc, std::move(e), m.text, std::string(m.raw)), {base});
                } else if (at(Tok::IntLiteral)) {
                    const Token& n = take();
                    const Expr* base = e.get();
                    e = deepened(std::make_unique<BitAccess>(loc, std::move(e), n.intValue), {base});
                } else if (at(Tok::Percent)) {
                    // Partial access w.%X3 (IEC 61131-3 Ed. 3); %B, %W, %D are not supported
                    take();
                    const Token& x = cur();
                    if (x.kind == Tok::Identifier && x.text.size() > 1 && x.text[0] == 'X' &&
                        x.text.find_first_not_of("0123456789", 1) == std::string::npos) {
                        take();
                        // A number too large for 64 bits stays out of range for the bit check in the analysis
                        uint64_t bit = UINT64_MAX;
                        std::from_chars(x.text.data() + 1, x.text.data() + x.text.size(), bit);
                        const Expr* base = e.get();
                        e = deepened(std::make_unique<BitAccess>(loc, std::move(e), bit), {base});
                    } else {
                        notSupported(x, "partial access other than .%Xn (bits)");
                        take();
                        return nullptr;
                    }
                } else {
                    error(cur().loc, std::format("expected a member name or bit number after '.', found {}", describe(cur())));
                    return nullptr;
                }
            } else if (at(Tok::LBracket)) {
                auto idx = std::make_unique<Index>(take().loc, std::move(e));
                do {
                    ExprPtr i = parseExpr();
                    if (!i) return nullptr;
                    idx->indexes.push_back(std::move(i));
                } while (accept(Tok::Comma));
                expect(Tok::RBracket, " to close the index");
                uint32_t d = idx->base ? idx->base->depth : 0;
                for (const ExprPtr& i : idx->indexes) d = std::max(d, i->depth);
                idx->depth = d + 1;
                if (idx->depth > kMaxExprDepth) {
                    tooDeep(std::format("an expression more than {} operations deep", kMaxExprDepth));
                    return nullptr;
                }
                e = std::move(idx);
            } else if (at(Tok::LParen)) {
                SourceLoc loc = e->loc;
                auto call = std::make_unique<Call>(loc, std::move(e));
                take();
                if (!at(Tok::RParen)) {
                    do {
                        Arg a;
                        a.loc = cur().loc;
                        if (at(Tok::Identifier) && (ahead(1).kind == Tok::Assign || ahead(1).kind == Tok::Output)) {
                            const Token& n = take();
                            a.name = n.text;
                            a.spelled = std::string(n.raw);
                            a.output = take().kind == Tok::Output;
                        } else if (at(Tok::KwNot) && ahead(1).kind == Tok::Identifier && ahead(2).kind == Tok::Output) {
                            notSupported(cur(), "inverted output assignment 'NOT x =>'");
                            take();
                            continue;
                        }
                        a.value = parseExpr();
                        if (!a.value) return nullptr;
                        call->args.push_back(std::move(a));
                    } while (accept(Tok::Comma));
                }
                expect(Tok::RParen, " to close the call");
                uint32_t d = call->callee ? call->callee->depth : 0;
                for (const Arg& a : call->args) d = std::max(d, a.value ? a.value->depth : 0u);
                call->depth = d + 1;
                if (call->depth > kMaxExprDepth) {
                    tooDeep(std::format("an expression more than {} operations deep", kMaxExprDepth));
                    return nullptr;
                }
                e = std::move(call);
            } else if (at(Tok::Caret)) {
                notSupported(cur(), "dereferencing '^'");
                take();
            } else {
                break;
            }
        }
        return e;
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
    int depth_ = 0;
    bool tooDeep_ = false;
    uint16_t file_;
    Diagnostics& d_;
    size_t lastErrorPos_ = SIZE_MAX;
    bool hadError_ = false;
    bool justClosedStruct_ = false;
};

}  // namespace

File parse(std::string_view source, uint16_t fileIndex, Diagnostics& diags, bool internal) {
    return Parser(lex(source, fileIndex, diags, internal), fileIndex, diags).parseFile();
}

ExprPtr parseExpression(std::string_view source, Diagnostics& diags) {
    return Parser(lex(source, 0, diags), 0, diags).parseLoneExpression();
}

}  // namespace simpleplc::st
