#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/diagnostics.h"
#include "st/value.h"

namespace simpleplc::st {

struct Type;  // st/types.h
struct Symbol;
struct PouInfo;

namespace ast {

// ───────────── Expressions ─────────────

enum class ExprKind : uint8_t {
    IntLit, RealLit, BoolLit, StringLit, TypedLit, Name, Member, BitAccess, Index, Call, Unary, Binary,
    Convert  ///< inserted by semantic analysis: an implicit (or standard-function) conversion
};
enum class UnaryOp : uint8_t { Neg, Plus, Not };
enum class BinaryOp : uint8_t { Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Add, Sub, Mul, Div, Mod, Pow };

const char* opText(UnaryOp op);
const char* opText(BinaryOp op);
inline bool isComparison(BinaryOp op) {
    return op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Gt ||
           op == BinaryOp::Le || op == BinaryOp::Ge;
}

struct Expr {
    explicit Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;

    const ExprKind kind;
    SourceLoc loc;
    bool parenthesized = false;  ///< written inside ( ): used to refuse ambiguous forms
    uint32_t depth = 1;          ///< height of the expression tree (the parser limits it)

    // Set by semantic analysis
    const Type* type = nullptr;  ///< type of the value
    bool isConstant = false;     ///< folded at compile time into `constant`
    Value constant;

    template <typename T> T& as() { return static_cast<T&>(*this); }
    template <typename T> const T& as() const { return static_cast<const T&>(*this); }
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
    IntLit(SourceLoc l, uint64_t v) : Expr(ExprKind::IntLit, l), value(v) {}
    uint64_t value;  ///< magnitude; a leading '-' is a Unary node
};
struct RealLit : Expr {
    RealLit(SourceLoc l, double v) : Expr(ExprKind::RealLit, l), value(v) {}
    double value;
};
struct BoolLit : Expr {
    BoolLit(SourceLoc l, bool v) : Expr(ExprKind::BoolLit, l), value(v) {}
    bool value;
};
struct StringLit : Expr {
    StringLit(SourceLoc l, std::string v) : Expr(ExprKind::StringLit, l), value(std::move(v)) {}
    std::string value;  ///< decoded bytes
};
/// prefix#body: INT#5, T#1s, BOOL#1, Color#Red, STRING#'x'. Resolved by semantic analysis.
struct TypedLit : Expr {
    TypedLit(SourceLoc l, std::string p, std::string b, bool q)
        : Expr(ExprKind::TypedLit, l), prefix(std::move(p)), body(std::move(b)), quoted(q) {}
    std::string prefix;  ///< upper case
    std::string body;
    bool quoted;
};
struct Name : Expr {
    Name(SourceLoc l, std::string u, std::string s) : Expr(ExprKind::Name, l), upper(std::move(u)), spelled(std::move(s)) {}
    std::string upper;    ///< for lookup
    std::string spelled;  ///< as written, for messages
    Symbol* symbol = nullptr;         ///< a variable, or null for an enumeration value
};
struct Member : Expr {
    Member(SourceLoc l, ExprPtr b, std::string u, std::string s)
        : Expr(ExprKind::Member, l), base(std::move(b)), upper(std::move(u)), spelled(std::move(s)) {}
    ExprPtr base;
    std::string upper;
    std::string spelled;
    uint32_t offset = 0;              ///< of the member within the base (set by semantic analysis)
    Symbol* member = nullptr;         ///< for a function block instance: the block's variable
};
/// Partial access to one bit of a bit string: w.3
struct BitAccess : Expr {
    BitAccess(SourceLoc l, ExprPtr b, uint64_t n) : Expr(ExprKind::BitAccess, l), base(std::move(b)), bit(n) {}
    ExprPtr base;
    uint64_t bit;
};
struct Index : Expr {
    Index(SourceLoc l, ExprPtr b) : Expr(ExprKind::Index, l), base(std::move(b)) {}
    ExprPtr base;
    std::vector<ExprPtr> indexes;
};
struct Arg {
    SourceLoc loc;
    std::string name;     ///< upper case; empty for a positional argument
    std::string spelled;
    bool output = false;  ///< name => variable
    ExprPtr value;
};
/// A call: a function (`ABS(x)`) or a function block instance (`t1(IN := a)`, `timers[i](…)`).
enum class CallKind : uint8_t { Unresolved, Function, Block, Standard };

struct Call : Expr {
    Call(SourceLoc l, ExprPtr c) : Expr(ExprKind::Call, l), callee(std::move(c)) {}
    ExprPtr callee;
    std::vector<Arg> args;

    // Set by semantic analysis
    CallKind callKind = CallKind::Unresolved;
    PouInfo* pou = nullptr;           ///< Function/Block
    int standard = 0;                 ///< Standard: StdFn
    const Type* operandType = nullptr;  ///< Standard: the common type of generic inputs
    std::vector<Symbol*> argParam;    ///< Function/Block: the parameter of each argument (in args order)
    int enoArg = -1;                  ///< index of `ENO => x`, or -1
    int enArg = -1;                   ///< index of `EN := x`, or -1
};
struct Unary : Expr {
    Unary(SourceLoc l, UnaryOp o, ExprPtr e) : Expr(ExprKind::Unary, l), op(o), operand(std::move(e)) {}
    UnaryOp op;
    ExprPtr operand;
};
struct Binary : Expr {
    Binary(SourceLoc l, BinaryOp o, ExprPtr a, ExprPtr b)
        : Expr(ExprKind::Binary, l), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
    const Type* operandType = nullptr;  ///< set by semantic analysis: the type the operation works in
};
/// Conversion of `operand` to this node's `type` (inserted by semantic analysis).
struct Convert : Expr {
    Convert(SourceLoc l, ExprPtr e, bool imp) : Expr(ExprKind::Convert, l), operand(std::move(e)), implicit(imp) {}
    ExprPtr operand;
    bool implicit;
    bool truncate = false;  ///< *_TRUNC_* / TRUNC: toward zero instead of to nearest
};

// ───────────── Statements ─────────────

enum class StmtKind : uint8_t { Assign, Call, If, Case, For, While, Repeat, Exit, Continue, Return, Empty };

struct Stmt {
    explicit Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    const StmtKind kind;
    SourceLoc loc;
    template <typename T> T& as() { return static_cast<T&>(*this); }
    template <typename T> const T& as() const { return static_cast<const T&>(*this); }
};
using StmtPtr = std::unique_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

struct AssignStmt : Stmt {
    AssignStmt(SourceLoc l, ExprPtr t, ExprPtr v) : Stmt(StmtKind::Assign, l), target(std::move(t)), value(std::move(v)) {}
    ExprPtr target;
    ExprPtr value;
};
struct CallStmt : Stmt {
    CallStmt(SourceLoc l, ExprPtr c) : Stmt(StmtKind::Call, l), call(std::move(c)) {}
    ExprPtr call;  ///< a Call
};
struct IfBranch {
    ExprPtr condition;
    StmtList body;
};
struct IfStmt : Stmt {
    explicit IfStmt(SourceLoc l) : Stmt(StmtKind::If, l) {}
    std::vector<IfBranch> branches;  ///< IF, then each ELSIF
    bool hasElse = false;
    StmtList elseBody;
};
struct CaseLabel {
    SourceLoc loc;
    ExprPtr low;
    ExprPtr high;  ///< null unless a range low..high
};
struct CaseBranch {
    std::vector<CaseLabel> labels;
    StmtList body;
};
struct CaseStmt : Stmt {
    CaseStmt(SourceLoc l, ExprPtr s) : Stmt(StmtKind::Case, l), selector(std::move(s)) {}
    ExprPtr selector;
    std::vector<CaseBranch> branches;
    bool hasElse = false;
    StmtList elseBody;
    bool complete = false;  ///< set by the analysis: the labels cover every value of an enumeration selector
};
struct ForStmt : Stmt {
    explicit ForStmt(SourceLoc l) : Stmt(StmtKind::For, l) {}
    Symbol* control = nullptr;  ///< set by semantic analysis
    ExprPtr variable;  ///< a Name
    ExprPtr start;
    ExprPtr end;
    ExprPtr step;  ///< null: 1
    StmtList body;
};
struct WhileStmt : Stmt {
    WhileStmt(SourceLoc l, ExprPtr c) : Stmt(StmtKind::While, l), condition(std::move(c)) {}
    ExprPtr condition;
    StmtList body;
};
struct RepeatStmt : Stmt {
    explicit RepeatStmt(SourceLoc l) : Stmt(StmtKind::Repeat, l) {}
    StmtList body;
    ExprPtr condition;
};
struct SimpleStmt : Stmt {  // EXIT, CONTINUE, RETURN, empty
    SimpleStmt(StmtKind k, SourceLoc l) : Stmt(k, l) {}
};

// ───────────── Declarations ─────────────

struct TypeSpec;
using TypeSpecPtr = std::unique_ptr<TypeSpec>;

struct ArrayRange {
    ExprPtr low;
    ExprPtr high;
};
struct EnumValue {
    SourceLoc loc;
    std::string upper;
    std::string spelled;
    ExprPtr value;  ///< explicit value or null
};
struct VarDecl;

enum class TypeSpecKind : uint8_t { Named, String, Array, Subrange, Enum, Struct };

struct TypeSpec {
    TypeSpecKind kind = TypeSpecKind::Named;
    SourceLoc loc;
    std::string upper;    ///< Named: the type name; Subrange/Enum: the base type name (may be empty for Enum)
    std::string spelled;
    ExprPtr length;       ///< String: [n] or null for the default
    std::vector<ArrayRange> ranges;  ///< Array
    TypeSpecPtr element;  ///< Array
    ExprPtr low, high;    ///< Subrange
    std::vector<EnumValue> values;   ///< Enum
    std::vector<VarDecl> fields;     ///< Struct
};

struct Initializer;
using InitPtr = std::unique_ptr<Initializer>;
struct ArrayInitElement {
    ExprPtr count;  ///< repetition count n(…) or null
    InitPtr value;  ///< null for n() (defaults)
};
struct StructInitField {
    SourceLoc loc;
    std::string upper;
    std::string spelled;
    InitPtr value;
};
struct Initializer {
    enum class Kind : uint8_t { Expr, Array, Struct } kind = Kind::Expr;
    SourceLoc loc;
    ExprPtr expr;
    std::vector<ArrayInitElement> elements;
    std::vector<StructInitField> fields;
};

struct DeclName {
    SourceLoc loc;
    std::string upper;
    std::string spelled;
};
struct VarDecl {
    std::vector<DeclName> names;
    TypeSpecPtr type;
    InitPtr init;  ///< null: the type's default
};

enum class VarKind : uint8_t { Var, Input, Output, InOut, Temp, External };
const char* varKindKeyword(VarKind kind);

struct VarSection {
    SourceLoc loc;
    VarKind kind = VarKind::Var;
    bool constant = false;
    bool retain = false;
    bool nonRetain = false;
    std::vector<VarDecl> decls;
};

struct TypeDecl {
    SourceLoc loc;
    std::string upper;
    std::string spelled;
    TypeSpecPtr spec;
    InitPtr init;  ///< default value of the type
};

enum class PouKind : uint8_t { Program, Function, FunctionBlock };
const char* pouKeyword(PouKind kind);

struct Pou {
    SourceLoc loc;
    SourceLoc endLoc;
    PouKind kind = PouKind::Program;
    std::string upper;
    std::string spelled;
    TypeSpecPtr returnType;  ///< functions only; null for no result
    std::vector<VarSection> sections;
    StmtList body;
    bool library = false;    ///< from the built-in standard library
};

/// One parsed source file.
struct File {
    uint16_t index = 0;
    std::vector<TypeDecl> types;
    std::vector<VarSection> constants;  ///< VAR_GLOBAL CONSTANT at file level
    std::vector<std::unique_ptr<Pou>> pous;
};

}  // namespace ast
}  // namespace simpleplc::st
