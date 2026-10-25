#pragma once
#include <string_view>

#include "st/ast.h"
#include "st/diagnostics.h"

namespace simpleplc::st {

/**
 * Parses one Structured Text source file. The parser is recursive descent with
 * one function per production of the IEC 61131-3 ST grammar (see
 * docs/st-grammar.txt). It refuses forms whose meaning readers or vendors
 * disagree on (E_AMBIGUOUS, E_CHAINED_COMPARISON) and recovers at ';' and
 * END_ keywords so one run reports as many problems as possible.
 */
ast::File parse(std::string_view source, uint16_t fileIndex, Diagnostics& diags, bool internal = false);

/// Parses a single expression (used by tools and tests); null on error.
ast::ExprPtr parseExpression(std::string_view source, Diagnostics& diags);

}  // namespace simpleplc::st
