#pragma once
#include <string_view>
#include <vector>

#include "st/diagnostics.h"
#include "st/token.h"

namespace simpleplc::st {

/**
 * Splits Structured Text into tokens (IEC 61131-3 lexical rules, see
 * docs/st-compliance.md). Problems are reported to `diags` and lexing goes on,
 * so one run reports as many problems as possible. The result always ends with
 * a Tok::End token. Tokens point into `source`, which must outlive them.
 * `internal` (the built-in standard library only) allows names containing `__`,
 * which IEC 61131-3 forbids in programs and SimplePLC reserves for itself.
 */
std::vector<Token> lex(std::string_view source, uint16_t fileIndex, Diagnostics& diags, bool internal = false);

/// Upper-cases ASCII letters (identifiers and keywords are case-insensitive).
std::string toUpper(std::string_view text);

}  // namespace simpleplc::st
