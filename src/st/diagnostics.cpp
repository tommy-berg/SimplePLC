#include "st/diagnostics.h"

#include <algorithm>

namespace simpleplc::st {

void Diagnostics::error(std::string_view code, SourceLoc loc, std::string message, std::string hint) {
    if (full()) return;
    items_.push_back({std::string(code), Severity::Error, loc, std::move(message), std::move(hint)});
    errors_++;
}

void Diagnostics::warning(std::string_view code, SourceLoc loc, std::string message, std::string hint) {
    items_.push_back({std::string(code), Severity::Warning, loc, std::move(message), std::move(hint)});
}

std::vector<Diagnostic> Diagnostics::sorted() const {
    std::vector<Diagnostic> out = items_;
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.loc.file != b.loc.file) return a.loc.file < b.loc.file;
        if (a.loc.line != b.loc.line) return a.loc.line < b.loc.line;
        return a.loc.column < b.loc.column;
    });
    return out;
}

}  // namespace simpleplc::st
