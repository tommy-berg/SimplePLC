#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "st/sema.h"

namespace simpleplc::st {

/// What a source file declares and names, read without semantic analysis (cheap: it is cached).
struct FileOutline {
    bool parses = false;          ///< no syntax errors
    bool hasProgram = false;      ///< holds a PROGRAM (a program file; otherwise a library)
    std::string program;          ///< the PROGRAM's name as spelled
    std::vector<std::string> declares;  ///< global names, upper case, sorted: TYPEs, enumeration values, POUs, VAR_GLOBAL CONSTANTs
    std::vector<std::string> names;     ///< every name the file mentions, upper case, sorted (identifiers and typed-literal prefixes)
};

/// The outline of one file (cached by content: the PLC asks again for every compile).
FileOutline outline(std::string_view source);

/**
 * The files a compilation of `files[0]` needs: `files[0]` itself and, transitively, every other file
 * that declares a name an included file mentions. A library file nothing uses is left out, so its
 * size and its errors cost the program nothing. The order of `files` is kept (files[0] first).
 */
std::vector<SourceFile> usedFiles(const std::vector<SourceFile>& files);

}  // namespace simpleplc::st
