#pragma once
#include <string>
#include <vector>

#include "core/tag.h"
#include "st/diagnostics.h"
#include "st/program.h"

namespace simpleplc::st {

struct SourceFile {
    std::string name;  ///< e.g. "main.st"
    std::string text;
};

/// Standard functions. Stable: codegen switches over them.
enum class StdFn : int {
    None,
    Abs, Sqrt, Ln, Log, Exp, Sin, Cos, Tan, Asin, Acos, Atan, Expt,
    Shl, Shr, Rol, Ror,
    Sel, Max, Min, Limit, Mux,
    Gt, Ge, Eq, Le, Lt, Ne,
    Len, Left, Right, Mid, Concat, Insert, Delete, Replace, Find,
    Atan2,
    FirstCycle,  ///< extension: TRUE in the first cycle after the program starts (FIRST_CYCLE())
    Now,   ///< internal: the cycle's time (standard library timers)
    Eval,  ///< internal: args[0] evaluated under EN/ENO (standard, operator and conversion functions)
};

/**
 * Parses and checks a program: `files[0]` holds the PROGRAM, the other files
 * are libraries (FUNCTIONs, FUNCTION_BLOCKs, TYPEs). The built-in standard
 * library is added automatically. `tags` are the PLC's tags that VAR_EXTERNAL
 * binds to. On success (no errors in `diags`) `program` is ready for code
 * generation; the AST is annotated with types, symbols and conversions. With
 * `library`, `files[0]` is a library file: no PROGRAM is required (nothing is generated).
 */
bool analyze(Program& program, const std::vector<SourceFile>& files, const std::vector<TagDef>& tags,
             Diagnostics& diags, bool library = false);

/// The standard library source (TON, TOF, TP, R_TRIG, F_TRIG, SR, RS, counters), in ST.
std::string_view standardLibrarySource();

/// Why `name` cannot name anything in a program (a tag that VAR_EXTERNAL binds, for example), by the
/// compiler's own rules: not an identifier, `__`, a trailing `_`, more than 64 characters, a keyword or
/// reserved word, or the name of a standard type, conversion, function or function block (TON, TP, ...).
/// Empty when a program can use it.
std::string nameProblem(std::string_view name);

/// Folds a typed expression to a value (literals, constants, operators, conversions); false if not
/// constant. When the operands are constant but the operation faults, `fault` (if given) says why.
bool evaluateConstant(const ast::Expr& e, Value& out, int* fault = nullptr);

}  // namespace simpleplc::st
