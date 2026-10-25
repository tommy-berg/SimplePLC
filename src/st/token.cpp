#include "st/token.h"

#include <array>
#include <utility>

namespace simpleplc::st {

namespace {

struct KeywordEntry {
    std::string_view word;
    Tok kind;
};

constexpr std::array kKeywords = std::to_array<KeywordEntry>({
    {"PROGRAM", Tok::KwProgram}, {"END_PROGRAM", Tok::KwEndProgram},
    {"FUNCTION", Tok::KwFunction}, {"END_FUNCTION", Tok::KwEndFunction},
    {"FUNCTION_BLOCK", Tok::KwFunctionBlock}, {"END_FUNCTION_BLOCK", Tok::KwEndFunctionBlock},
    {"VAR", Tok::KwVar}, {"VAR_INPUT", Tok::KwVarInput}, {"VAR_OUTPUT", Tok::KwVarOutput},
    {"VAR_IN_OUT", Tok::KwVarInOut}, {"VAR_TEMP", Tok::KwVarTemp}, {"VAR_EXTERNAL", Tok::KwVarExternal},
    {"VAR_GLOBAL", Tok::KwVarGlobal}, {"VAR_ACCESS", Tok::KwVarAccess}, {"VAR_CONFIG", Tok::KwVarConfig},
    {"VAR_STAT", Tok::KwVarStat}, {"END_VAR", Tok::KwEndVar}, {"CONSTANT", Tok::KwConstant},
    {"RETAIN", Tok::KwRetain}, {"NON_RETAIN", Tok::KwNonRetain}, {"PERSISTENT", Tok::KwPersistent},
    {"AT", Tok::KwAt}, {"TYPE", Tok::KwType}, {"END_TYPE", Tok::KwEndType}, {"STRUCT", Tok::KwStruct},
    {"END_STRUCT", Tok::KwEndStruct}, {"ARRAY", Tok::KwArray}, {"OF", Tok::KwOf}, {"STRING", Tok::KwString},
    {"WSTRING", Tok::KwWString}, {"IF", Tok::KwIf}, {"THEN", Tok::KwThen}, {"ELSIF", Tok::KwElsif},
    {"ELSE", Tok::KwElse}, {"END_IF", Tok::KwEndIf}, {"CASE", Tok::KwCase}, {"END_CASE", Tok::KwEndCase},
    {"FOR", Tok::KwFor}, {"TO", Tok::KwTo}, {"BY", Tok::KwBy}, {"DO", Tok::KwDo}, {"END_FOR", Tok::KwEndFor},
    {"WHILE", Tok::KwWhile}, {"END_WHILE", Tok::KwEndWhile}, {"REPEAT", Tok::KwRepeat},
    {"UNTIL", Tok::KwUntil}, {"END_REPEAT", Tok::KwEndRepeat}, {"EXIT", Tok::KwExit},
    {"CONTINUE", Tok::KwContinue}, {"RETURN", Tok::KwReturn}, {"TRUE", Tok::KwTrue}, {"FALSE", Tok::KwFalse},
    {"AND", Tok::KwAnd}, {"OR", Tok::KwOr}, {"XOR", Tok::KwXor}, {"NOT", Tok::KwNot}, {"MOD", Tok::KwMod},
    {"R_EDGE", Tok::KwREdge}, {"F_EDGE", Tok::KwFEdge},
    {"CONFIGURATION", Tok::KwConfiguration}, {"END_CONFIGURATION", Tok::KwEndConfiguration},
    {"RESOURCE", Tok::KwResource}, {"END_RESOURCE", Tok::KwEndResource}, {"TASK", Tok::KwTask},
    {"ON", Tok::KwOn}, {"WITH", Tok::KwWith}, {"METHOD", Tok::KwMethod}, {"END_METHOD", Tok::KwEndMethod},
    {"CLASS", Tok::KwClass}, {"END_CLASS", Tok::KwEndClass}, {"INTERFACE", Tok::KwInterface},
    {"END_INTERFACE", Tok::KwEndInterface}, {"PROPERTY", Tok::KwProperty}, {"END_PROPERTY", Tok::KwEndProperty},
    {"EXTENDS", Tok::KwExtends}, {"IMPLEMENTS", Tok::KwImplements}, {"THIS", Tok::KwThis},
    {"SUPER", Tok::KwSuper}, {"REF_TO", Tok::KwRefTo}, {"REF", Tok::KwRef}, {"NULL", Tok::KwNull},
    {"ABSTRACT", Tok::KwAbstract}, {"FINAL", Tok::KwFinal}, {"OVERRIDE", Tok::KwOverride},
    {"PUBLIC", Tok::KwPublic}, {"PRIVATE", Tok::KwPrivate}, {"PROTECTED", Tok::KwProtected},
    {"INTERNAL", Tok::KwInternal}, {"NAMESPACE", Tok::KwNamespace}, {"END_NAMESPACE", Tok::KwEndNamespace},
    {"USING", Tok::KwUsing}, {"AND_THEN", Tok::KwAndThen}, {"OR_ELSE", Tok::KwOrElse}, {"JMP", Tok::KwJmp},
    {"STEP", Tok::KwStep}, {"END_STEP", Tok::KwEndStep}, {"INITIAL_STEP", Tok::KwInitialStep},
    {"TRANSITION", Tok::KwTransition}, {"END_TRANSITION", Tok::KwEndTransition},
    {"ACTION", Tok::KwAction}, {"END_ACTION", Tok::KwEndAction}, {"FROM", Tok::KwFrom},
    {"ELSEIF", Tok::KwElseIf},
});

}  // namespace

Tok keywordKind(std::string_view upper) {
    for (const KeywordEntry& k : kKeywords) {
        if (k.word == upper) return k.kind;
    }
    return Tok::Identifier;
}

bool isUnsupportedKeyword(Tok kind) {
    switch (kind) {
        case Tok::KwVarGlobal: case Tok::KwVarAccess: case Tok::KwVarConfig: case Tok::KwVarStat:
        case Tok::KwPersistent: case Tok::KwAt: case Tok::KwWString:
        case Tok::KwREdge: case Tok::KwFEdge:
        case Tok::KwConfiguration: case Tok::KwEndConfiguration: case Tok::KwResource:
        case Tok::KwEndResource: case Tok::KwTask: case Tok::KwOn: case Tok::KwWith:
        case Tok::KwMethod: case Tok::KwEndMethod: case Tok::KwClass: case Tok::KwEndClass:
        case Tok::KwInterface: case Tok::KwEndInterface: case Tok::KwProperty: case Tok::KwEndProperty:
        case Tok::KwExtends: case Tok::KwImplements: case Tok::KwThis: case Tok::KwSuper:
        case Tok::KwRefTo: case Tok::KwRef: case Tok::KwNull: case Tok::KwAbstract: case Tok::KwFinal:
        case Tok::KwOverride: case Tok::KwPublic: case Tok::KwPrivate: case Tok::KwProtected:
        case Tok::KwInternal: case Tok::KwNamespace: case Tok::KwEndNamespace: case Tok::KwUsing:
        case Tok::KwAndThen: case Tok::KwOrElse: case Tok::KwJmp: case Tok::KwStep: case Tok::KwEndStep:
        case Tok::KwInitialStep: case Tok::KwTransition: case Tok::KwEndTransition: case Tok::KwAction:
        case Tok::KwEndAction: case Tok::KwFrom:
            return true;
        default:
            return false;
    }
}

std::string_view tokName(Tok kind) {
    switch (kind) {
        case Tok::End: return "the end of the file";
        case Tok::Identifier: return "a name";
        case Tok::IntLiteral: return "an integer literal";
        case Tok::RealLiteral: return "a real literal";
        case Tok::StringLiteral: return "a string literal";
        case Tok::WStringLiteral: return "a WSTRING literal";
        case Tok::TypedLiteral: return "a typed literal";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Comma: return "','";
        case Tok::Semicolon: return "';'";
        case Tok::Colon: return "':'";
        case Tok::Assign: return "':='";
        case Tok::Output: return "'=>'";
        case Tok::Dot: return "'.'";
        case Tok::DotDot: return "'..'";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'";
        case Tok::Slash: return "'/'";
        case Tok::Power: return "'**'";
        case Tok::Eq: return "'='";
        case Tok::Ne: return "'<>'";
        case Tok::Lt: return "'<'";
        case Tok::Gt: return "'>'";
        case Tok::Le: return "'<='";
        case Tok::Ge: return "'>='";
        case Tok::Ampersand: return "'&'";
        case Tok::Caret: return "'^'";
        case Tok::At: return "'@'";
        case Tok::Hash: return "'#'";
        case Tok::Percent: return "'%'";
        default: break;
    }
    for (const KeywordEntry& k : kKeywords) {
        if (k.kind == kind) return k.word;
    }
    return "?";
}

}  // namespace simpleplc::st
