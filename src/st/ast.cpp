#include "st/ast.h"

namespace simpleplc::st::ast {

const char* opText(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg: return "-";
        case UnaryOp::Plus: return "+";
        case UnaryOp::Not: return "NOT";
    }
    return "?";
}

const char* opText(BinaryOp op) {
    switch (op) {
        case BinaryOp::Or: return "OR";
        case BinaryOp::Xor: return "XOR";
        case BinaryOp::And: return "AND";
        case BinaryOp::Eq: return "=";
        case BinaryOp::Ne: return "<>";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "MOD";
        case BinaryOp::Pow: return "**";
    }
    return "?";
}

const char* varKindKeyword(VarKind kind) {
    switch (kind) {
        case VarKind::Var: return "VAR";
        case VarKind::Input: return "VAR_INPUT";
        case VarKind::Output: return "VAR_OUTPUT";
        case VarKind::InOut: return "VAR_IN_OUT";
        case VarKind::Temp: return "VAR_TEMP";
        case VarKind::External: return "VAR_EXTERNAL";
    }
    return "?";
}

const char* pouKeyword(PouKind kind) {
    switch (kind) {
        case PouKind::Program: return "PROGRAM";
        case PouKind::Function: return "FUNCTION";
        case PouKind::FunctionBlock: return "FUNCTION_BLOCK";
    }
    return "?";
}

}  // namespace simpleplc::st::ast
