#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simpleplc::runtime {

struct ProgramInfo {
    std::string name;
    uint64_t size = 0;
    int64_t modifiedMs = 0;  ///< Unix epoch ms
};

struct ProgramVersion {
    std::string id;          ///< sortable: the time that version was saved, e.g. "20261023T201530123Z" (+ "001"… when several share a millisecond)
    uint64_t size = 0;
    int64_t savedMs = 0;     ///< when that version was saved
};

/// A program file by name (without ".st") and content.
struct StoredFile {
    std::string name;
    std::string text;

    bool operator==(const StoredFile&) const = default;
};

/// The files a program was last loaded with.
struct DeployedSet {
    std::vector<StoredFile> files;  ///< the program's own file first, then the libraries it used
    bool legacy = false;            ///< written by an older version: the program's own file only
};

/**
 * Structured Text files of one PLC, stored as `<dir>/<name>.st`. Names are
 * case-insensitive, as on the file systems of macOS and Windows: a name that
 * differs from an existing file's only by case means that file, on every system.
 *
 * Every save keeps the previous version in `<dir>/.history/<name>/` (the last 20),
 * named by the time it was saved; deleting a program keeps its history too. The set of
 * files a program was last loaded with (its own file and the libraries it used) is kept in
 * `<dir>/.deployed/<name>/`, so a PLC that restarts runs exactly that, whatever was saved
 * since. Thread-safe.
 */
class ProgramStore {
public:
    static constexpr size_t kMaxSourceBytes = 1024 * 1024;
    /// All program files together: a Raspberry Pi must be able to compile a program that uses them all.
    static constexpr size_t kMaxTotalBytes = 2 * kMaxSourceBytes;
    static constexpr size_t kHistoryLimit = 20;

    explicit ProgramStore(std::filesystem::path dir);

    /// Names: letters, digits, '_' and '-', 1-64 characters.
    static bool isValidName(std::string_view name);
    /// The name of the existing program that `name` means (one that differs only by case), else `name`.
    std::string canonical(std::string_view name) const;
    /// Program names are equal when they differ only by case.
    static bool sameName(std::string_view a, std::string_view b);

    std::vector<ProgramInfo> list() const;
    bool exists(std::string_view name) const;
    std::expected<std::string, std::string> read(std::string_view name) const;
    std::expected<void, std::string> write(std::string_view name, std::string_view source);
    /// Deletes a program's file and its deployed set; its content stays in its history, where it can be restored.
    std::expected<void, std::string> remove(std::string_view name);
    std::vector<ProgramVersion> history(std::string_view name) const;
    std::expected<std::string, std::string> readVersion(std::string_view name, std::string_view id) const;
    /// Records the files the tasks run `name` with (its own file first).
    std::expected<void, std::string> markDeployed(std::string_view name, const std::vector<StoredFile>& files);
    /// The files `name` last loaded with, if any.
    std::optional<DeployedSet> deployed(std::string_view name) const;
    /// Forgets every deployed version (a PLC set up from scratch must not fall back to the old programs).
    void clearDeployed();
    /// Removes a program but keeps its last content in its history, where it can be restored.
    std::expected<void, std::string> retire(std::string_view name);

    const std::filesystem::path& dir() const { return dir_; }

private:
    std::string canonicalLocked(std::string_view name) const;
    std::filesystem::path pathOf(std::string_view name) const;
    std::expected<void, std::string> keepVersionLocked(const std::string& name, const std::filesystem::path& file,
                                                       const std::string& content);

    std::filesystem::path dir_;
    mutable std::mutex mutex_;
};

/// Writes a file atomically (temporary file + rename).
std::expected<void, std::string> writeFileAtomic(const std::filesystem::path& path, std::string_view content);
std::expected<std::string, std::string> readFile(const std::filesystem::path& path);

}  // namespace simpleplc::runtime
