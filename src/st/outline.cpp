#include "st/outline.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include "st/lexer.h"
#include "st/parser.h"
#include "web/crypto.h"

namespace simpleplc::st {
namespace {

FileOutline makeOutline(std::string_view source) {
    FileOutline o;
    Diagnostics diags;
    ast::File file = parse(source, 0, diags);
    o.parses = !diags.hasErrors();
    std::set<std::string> declares;
    for (const ast::TypeDecl& t : file.types) {
        if (!t.upper.empty()) declares.insert(t.upper);
        // Enumeration values are global names too (a program may write Auto for Mode#Auto)
        if (t.spec && t.spec->kind == ast::TypeSpecKind::Enum) {
            for (const ast::EnumValue& v : t.spec->values) declares.insert(v.upper);
        }
    }
    for (const ast::VarSection& s : file.constants) {
        for (const ast::VarDecl& d : s.decls) {
            for (const ast::DeclName& n : d.names) declares.insert(n.upper);
        }
    }
    for (const auto& pou : file.pous) {
        if (pou->upper.empty()) continue;
        if (pou->kind == ast::PouKind::Program) {
            if (!o.hasProgram) o.program = pou->spelled;
            o.hasProgram = true;
            continue;  // another file's PROGRAM is never shared
        }
        declares.insert(pou->upper);
    }
    o.declares.assign(declares.begin(), declares.end());

    std::set<std::string> names;
    Diagnostics lexDiags;
    for (const Token& t : lex(source, 0, lexDiags)) {
        if (t.kind == Tok::Identifier) names.insert(t.text);
        else if (t.kind == Tok::TypedLiteral) {
            names.insert(t.prefix);  // Mode#Auto names the type Mode
            if (!t.quoted) names.insert(toUpper(t.text));
        }
    }
    o.names.assign(names.begin(), names.end());
    return o;
}

bool intersects(const std::vector<std::string>& sortedA, const std::set<std::string>& b) {
    for (const std::string& s : sortedA) {
        if (b.count(s)) return true;
    }
    return false;
}

}  // namespace

FileOutline outline(std::string_view source) {
    // Keyed by the content's SHA-256: the same text always has the same outline
    static std::mutex mutex;
    static std::map<crypto::Digest, FileOutline> cache;
    crypto::Digest key = crypto::sha256(source);
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    FileOutline o = makeOutline(source);
    std::lock_guard lock(mutex);
    if (cache.size() >= 64) cache.clear();  // a PLC has a few dozen files at most; old versions go
    cache.emplace(key, o);
    return o;
}

std::vector<SourceFile> usedFiles(const std::vector<SourceFile>& files) {
    if (files.size() <= 1) return files;
    std::vector<FileOutline> outlines;
    outlines.reserve(files.size());
    for (const SourceFile& f : files) outlines.push_back(outline(f.text));
    std::vector<bool> used(files.size(), false);
    used[0] = true;
    std::set<std::string> mentioned(outlines[0].names.begin(), outlines[0].names.end());
    // A file is needed when it declares a name a needed file mentions; repeat until nothing is added
    for (bool added = true; added;) {
        added = false;
        for (size_t i = 1; i < files.size(); i++) {
            if (used[i] || !intersects(outlines[i].declares, mentioned)) continue;
            used[i] = true;
            added = true;
            mentioned.insert(outlines[i].names.begin(), outlines[i].names.end());
        }
    }
    std::vector<SourceFile> out;
    for (size_t i = 0; i < files.size(); i++) {
        if (used[i]) out.push_back(files[i]);
    }
    return out;
}

}  // namespace simpleplc::st
