#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace simpleplc::st {

/// A position in a source file: 1-based line and column (column counts bytes).
struct SourceLoc {
    uint16_t file = 0;  ///< index into the compilation's file list
    uint32_t line = 0;
    uint32_t column = 0;
};

enum class Severity : uint8_t { Error, Warning };

/**
 * One compiler message. `code` is stable (tests and agents key on it, e.g.
 * "E_TYPE"); `message` says what is wrong in plain words, `hint` how to write it.
 */
struct Diagnostic {
    std::string code;
    Severity severity = Severity::Error;
    SourceLoc loc;
    std::string message;
    std::string hint;
};

/// Collects diagnostics; compilation stops producing code once an error is recorded.
class Diagnostics {
public:
    /// The compilation gives up after this many errors (cascades are not useful).
    static constexpr size_t kMaxErrors = 50;

    void error(std::string_view code, SourceLoc loc, std::string message, std::string hint = {});
    void warning(std::string_view code, SourceLoc loc, std::string message, std::string hint = {});

    bool hasErrors() const { return errors_ > 0; }
    bool full() const { return errors_ >= kMaxErrors; }
    size_t errorCount() const { return errors_; }
    const std::vector<Diagnostic>& all() const { return items_; }
    /// Sorted by file, line and column (stable for equal positions).
    std::vector<Diagnostic> sorted() const;

private:
    std::vector<Diagnostic> items_;
    size_t errors_ = 0;
};

}  // namespace simpleplc::st
