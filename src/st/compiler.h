#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/tag.h"
#include "st/diagnostics.h"
#include "st/sema.h"
#include "vm/image.h"

namespace simpleplc::st {

/// Version of the compiler and image contract; part of every image.
inline constexpr const char* kCompilerVersion = "simpleplc-st 1";

/// What files[0] of a compilation is.
enum class CompileMode : uint8_t {
    Program,  ///< a program file: one PROGRAM, compiled to a verified image
    Library,  ///< a library file (no PROGRAM): every declaration checked, no image
};

struct CompileResult {
    bool ok = false;
    bool library = false;                  ///< compiled as a library: `image` is null even when ok
    std::vector<Diagnostic> diagnostics;   ///< sorted by position
    std::vector<std::string> fileNames;    ///< index → name, for SourceLoc::file
    std::shared_ptr<const vm::Image> image;  ///< verified; null when !ok
    std::string listing;                   ///< the program "as compiled"
    uint32_t dataBytes = 0;                ///< data block (globals, instance)
    uint32_t stackBytes = 0;
    uint32_t codeInstructions = 0;
};

/**
 * Compiles a Structured Text program: `files[0]` holds the PROGRAM, the others
 * are libraries. Binds VAR_EXTERNAL to `tags`. The image is verified before it
 * is returned, so a compiler bug shows up as an error here, not as a running
 * program that does something else. With CompileMode::Library, `files[0]` is a
 * library file: its declarations are checked as if a program used all of them.
 */
CompileResult compile(const std::vector<SourceFile>& files, const std::vector<TagDef>& tags,
                      CompileMode mode = CompileMode::Program);

/// "main.st:12:5: error E_TYPE: … (hint: …)"
std::string formatDiagnostic(const Diagnostic& d, const std::vector<std::string>& fileNames);

}  // namespace simpleplc::st
