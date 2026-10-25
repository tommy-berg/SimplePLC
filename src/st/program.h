#pragma once
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/tag.h"
#include "st/ast.h"
#include "st/types.h"

namespace simpleplc::st {

/// Where a variable lives (VM segments).
enum class Storage : uint8_t { Self, Frame, Global };

enum class SymKind : uint8_t { Var, Input, Output, InOut, Temp, External, Result };

/// A declared variable of a POU (or a function's result).
struct Symbol {
    std::string upper;
    std::string spelled;
    SourceLoc loc;
    SymKind kind = SymKind::Var;
    bool constant = false;
    bool retain = false;
    const Type* type = nullptr;
    Storage storage = Storage::Self;
    uint32_t offset = 0;                  ///< in its segment; VAR_IN_OUT: a 4-byte reference slot
    ast::Initializer* init = nullptr;
    const ast::VarDecl* decl = nullptr;
    std::string tag;                      ///< VAR_EXTERNAL: the tag's name as in the tag table
    bool assigned = false;                ///< written somewhere (W_RESULT_UNASSIGNED)
    bool used = false;                    ///< named somewhere in a body (W_UNUSED)
    bool hasConstValue = false;           ///< CONSTANT with a folded initial value
    Value constValue;
    int order = 0;                        ///< declaration order within the POU
};

/// A PROGRAM, FUNCTION or FUNCTION_BLOCK after semantic analysis.
struct PouInfo {
    ast::Pou* ast = nullptr;
    ast::PouKind kind = ast::PouKind::Program;
    std::string upper;
    std::string spelled;
    bool library = false;                 ///< from the standard library
    const Type* result = nullptr;         ///< functions: result type (Void for none)
    Symbol* resultSymbol = nullptr;
    std::deque<Symbol> symbols;           ///< stable addresses
    std::map<std::string, Symbol*> byName;
    std::vector<Symbol*> inputs;          ///< declaration order (informal calls use inputs, then in-outs)
    std::vector<Symbol*> outputs;
    std::vector<Symbol*> inOuts;
    Type* instanceType = nullptr;         ///< blocks and programs: the instance layout (fields)
    uint32_t instanceSize = 0;
    uint32_t frameSize = 0;               ///< declared frame variables (codegen adds temporaries)
    std::vector<PouInfo*> callees;
    int index = -1;                       ///< position in the image's POU table
    bool checked = false;

    Symbol* find(const std::string& name) const {
        auto it = byName.find(name);
        return it == byName.end() ? nullptr : it->second;
    }
};

/// A tag the program binds to (VAR_EXTERNAL), with its mirror in the Global segment.
struct TagBinding {
    std::string tag;      ///< name in the tag table
    TagType tagType = TagType::Bool;
    const Type* type = nullptr;
    uint32_t offset = 0;
    bool written = false;  ///< some POU may write it (not CONSTANT everywhere)
};

/// Everything semantic analysis produces for one program.
struct Program {
    TypeTable types;
    std::vector<ast::File> files;
    std::vector<std::string> fileNames;
    std::deque<PouInfo> pous;
    std::map<std::string, PouInfo*> pouByName;
    std::map<std::string, const Type*> typeByName;  ///< user TYPEs
    PouInfo* main = nullptr;                        ///< the PROGRAM
    std::vector<TagBinding> bindings;
    uint32_t globalsSize = 0;

    PouInfo* findPou(const std::string& name) const {
        auto it = pouByName.find(name);
        return it == pouByName.end() ? nullptr : it->second;
    }
};

}  // namespace simpleplc::st
