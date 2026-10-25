#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "st/diagnostics.h"

namespace simpleplc::st {

/**
 * Token kinds. Keywords are case-insensitive. Words the standard reserves for
 * features SimplePLC does not implement (OOP, configurations, references) are
 * still keywords, so using them gives "not supported" instead of a confusing
 * syntax error later.
 */
enum class Tok : uint8_t {
    End,
    Identifier,
    IntLiteral,      ///< decimal or based (2#, 8#, 16#); value in Token::intValue
    RealLiteral,     ///< value in Token::realValue
    StringLiteral,   ///< '…', decoded bytes in Token::text
    WStringLiteral,  ///< "…" (not supported; kept for a precise message)
    TypedLiteral,    ///< prefix#body: INT#5, T#1s, Color#Red, BOOL#TRUE (prefix and body in Token)

    // Punctuation
    LParen, RParen, LBracket, RBracket, Comma, Semicolon, Colon, Assign /* := */, Output /* => */,
    Dot, DotDot, Plus, Minus, Star, Slash, Power /* ** */, Eq, Ne /* <> */, Lt, Gt, Le, Ge,
    Ampersand, Caret, At, Hash, Percent,

    // Keywords
    KwProgram, KwEndProgram, KwFunction, KwEndFunction, KwFunctionBlock, KwEndFunctionBlock,
    KwVar, KwVarInput, KwVarOutput, KwVarInOut, KwVarTemp, KwVarExternal, KwVarGlobal,
    KwVarAccess, KwVarConfig, KwVarStat, KwEndVar, KwConstant, KwRetain, KwNonRetain, KwPersistent, KwAt,
    KwType, KwEndType, KwStruct, KwEndStruct, KwArray, KwOf, KwString, KwWString,
    KwIf, KwThen, KwElsif, KwElse, KwEndIf, KwCase, KwEndCase, KwFor, KwTo, KwBy, KwDo, KwEndFor,
    KwWhile, KwEndWhile, KwRepeat, KwUntil, KwEndRepeat, KwExit, KwContinue, KwReturn,
    KwTrue, KwFalse, KwAnd, KwOr, KwXor, KwNot, KwMod,
    KwREdge, KwFEdge,
    // Reserved, not supported in phase 1
    KwConfiguration, KwEndConfiguration, KwResource, KwEndResource, KwTask, KwOn, KwWith,
    KwMethod, KwEndMethod, KwClass, KwEndClass, KwInterface, KwEndInterface, KwProperty, KwEndProperty,
    KwExtends, KwImplements, KwThis, KwSuper, KwRefTo, KwRef, KwNull, KwAbstract, KwFinal,
    KwOverride, KwPublic, KwPrivate, KwProtected, KwInternal, KwNamespace, KwEndNamespace, KwUsing,
    KwAndThen, KwOrElse, KwJmp, KwStep, KwEndStep, KwInitialStep, KwTransition, KwEndTransition,
    KwAction, KwEndAction, KwFrom, KwElseIf /* not IEC: recognised for a hint */,
};

/// Short description for messages: "':='", "'END_IF'", "an identifier".
std::string_view tokName(Tok kind);

/// Keyword for an identifier spelled `upper` (already upper case), or Tok::Identifier.
Tok keywordKind(std::string_view upper);

/// True for keywords of features this implementation rejects with E_NOT_SUPPORTED.
bool isUnsupportedKeyword(Tok kind);

struct Token {
    Tok kind = Tok::End;
    SourceLoc loc;
    std::string_view raw;    ///< exact source text
    std::string text;        ///< identifiers: upper case; strings: decoded; typed literals: the body
    std::string prefix;      ///< typed literals: upper-case prefix (INT, T, TIME, BOOL, enum type name)
    uint64_t intValue = 0;   ///< IntLiteral magnitude
    double realValue = 0;    ///< RealLiteral value (correctly rounded)
    bool quoted = false;     ///< typed literal whose body is a string: STRING#'abc'
    bool followsSpace = false;  ///< whitespace or a comment precedes it
};

}  // namespace simpleplc::st
