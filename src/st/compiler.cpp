#include "st/compiler.h"

#include <format>
#include <semaphore>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include "st/codegen.h"
#include "st/listing.h"
#include "web/crypto.h"

namespace simpleplc::st {

std::string formatDiagnostic(const Diagnostic& d, const std::vector<std::string>& fileNames) {
    std::string file = d.loc.file < fileNames.size() ? fileNames[d.loc.file] : "?";
    std::string out = std::format("{}:{}:{}: {} {}: {}", file, d.loc.line, d.loc.column,
                                  d.severity == Severity::Error ? "error" : "warning", d.code, d.message);
    if (!d.hint.empty()) out += std::format(" (hint: {})", d.hint);
    return out;
}

namespace {

CompileResult compileHere(const std::vector<SourceFile>& files, const std::vector<TagDef>& tags, CompileMode mode) {
    CompileResult r;
    Program program;
    Diagnostics diags;
    r.library = mode == CompileMode::Library;
    bool analysed = analyze(program, files, tags, diags, r.library);
    r.fileNames = program.fileNames;
    if (r.library) {
        // A library has nothing to run: its declarations checked is all there is
        r.diagnostics = diags.sorted();
        r.ok = analysed && !diags.hasErrors();
        if (r.ok) r.listing = listing(program);
        return r;
    }
    if (analysed && !diags.hasErrors()) {
        auto image = std::make_shared<vm::Image>();
        if (generate(program, *image, diags) && !diags.hasErrors()) {
            // The source hash names exactly what was compiled: every file and the tag table
            std::string all = kCompilerVersion;
            for (const SourceFile& f : files) all += std::format("\x1f{}\x1f{}", f.name, f.text);
            for (const TagDef& t : tags) {
                all += std::format("\x1e{}:{}:{}:{}", t.name, areaName(t.area), t.address, tagTypeName(t.type));
            }
            auto digest = crypto::sha256(all);
            std::copy(digest.begin(), digest.end(), image->sourceHash.begin());
            image->compiler = kCompilerVersion;
            if (auto ok = vm::verify(*image); !ok) {
                diags.error("E_INTERNAL", SourceLoc{0, 1, 1},
                            std::format("the compiler produced an invalid program ({}); please report this", ok.error()));
            } else {
                r.dataBytes = image->stackOffset;
                r.stackBytes = image->stackSize;
                r.codeInstructions = static_cast<uint32_t>(image->code.size());
                r.listing = listing(program);
                r.image = std::move(image);
            }
        }
    }
    r.diagnostics = diags.sorted();
    r.ok = r.image != nullptr && !diags.hasErrors();
    return r;
}

}  // namespace

CompileResult compile(const std::vector<SourceFile>& files, const std::vector<TagDef>& tags, CompileMode mode) {
    // At most two compilations at a time in the process: a large program takes up to ~100 MB while it
    // compiles, and checks from many clients must not exhaust a small device's memory
    static std::counting_semaphore<2> slots(2);
    slots.acquire();
    struct Release {
        ~Release() { slots.release(); }
    } release;
    // Parsing, analysis and code generation recurse along the program's nesting (at most 100 levels,
    // E_LIMIT beyond). Callers run on threads with small stacks (web workers: 512 KB on macOS), so
    // the compiler gets a thread with a stack of its own.
    CompileResult result;
#if !defined(_WIN32)
    struct Job {
        const std::vector<SourceFile>* files;
        const std::vector<TagDef>* tags;
        CompileMode mode;
        CompileResult* result;
    } job{&files, &tags, mode, &result};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024 * 1024);
    pthread_t thread;
    int rc = pthread_create(
        &thread, &attr,
        [](void* arg) -> void* {
            auto* j = static_cast<Job*>(arg);
            *j->result = compileHere(*j->files, *j->tags, j->mode);
            return nullptr;
        },
        &job);
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        pthread_join(thread, nullptr);
        return result;
    }
#endif
    std::thread worker([&] { result = compileHere(files, tags, mode); });  // Windows: threads get 1 MB
    worker.join();
    return result;
}

}  // namespace simpleplc::st
