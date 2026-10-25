#pragma once
#include <string>

#include "st/program.h"

namespace simpleplc::st {

/**
 * The program "as compiled": every statement of the user's files re-printed as
 * valid Structured Text with full parentheses, every literal typed (INT#20) and
 * every implicit conversion written out (INT_TO_REAL(x)). Each statement starts
 * with a comment giving its source line. Engineers review exactly what will run;
 * auditors check the parse without reading the compiler.
 */
std::string listing(const Program& program);

/// One expression in listing form (used by tests and diagnostics).
std::string listExpression(const ast::Expr& e);

}  // namespace simpleplc::st
