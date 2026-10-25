#include "runtime/program_store.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace simpleplc::runtime {
namespace fs = std::filesystem;
namespace {

int64_t toEpochMs(fs::file_time_type time) {
    auto system = std::chrono::file_clock::to_sys(time);
    return std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string versionIdAt(int64_t ms) {
    std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", &tm);
    return std::format("{}{:03}Z", buf, ms % 1000);
}

int64_t versionTimeMs(const std::string& id) {
    std::tm tm{};
    std::istringstream in(id.substr(0, 15));
    in >> std::get_time(&tm, "%Y%m%dT%H%M%S");
    if (in.fail()) return 0;
#ifdef _WIN32
    std::time_t seconds = _mkgmtime(&tm);
#else
    std::time_t seconds = timegm(&tm);
#endif
    int millis = id.size() >= 18 ? std::atoi(id.substr(15, 3).c_str()) : 0;
    return static_cast<int64_t>(seconds) * 1000 + millis;
}

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

/// The names listed in a deployed set's `.order` file, one per line.
std::vector<std::string> orderOf(const fs::path& dir) {
    std::vector<std::string> names;
    auto text = readFile(dir / ".order");
    if (!text) return names;
    for (size_t pos = 0; pos < text->size();) {
        size_t nl = text->find('\n', pos);
        if (nl == std::string::npos) nl = text->size();
        if (nl > pos) names.push_back(text->substr(pos, nl - pos));
        pos = nl + 1;
    }
    return names;
}

}  // namespace

std::expected<void, std::string> writeFileAtomic(const fs::path& path, std::string_view content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(std::format("cannot write {}", temp.string()));
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return std::unexpected(std::format("cannot write {}", temp.string()));
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return std::unexpected(std::format("cannot replace {}: {}", path.string(), ec.message()));
    }
    return {};
}

std::expected<std::string, std::string> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read {}", path.string()));
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

ProgramStore::ProgramStore(fs::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
}

bool ProgramStore::isValidName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool ProgramStore::sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

std::string ProgramStore::canonical(std::string_view name) const {
    std::lock_guard lock(mutex_);
    return canonicalLocked(name);
}

std::string ProgramStore::canonicalLocked(std::string_view name) const {
    // The directory's own spelling: macOS and Windows open main.st for "MAIN.st", Linux does not
    std::string match;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".st") continue;
        std::string stem = entry.path().stem().string();
        if (stem == name) return stem;
        if (match.empty() && sameName(stem, name)) match = stem;
    }
    return match.empty() ? std::string(name) : match;
}

fs::path ProgramStore::pathOf(std::string_view name) const {
    return dir_ / (std::string(name) + ".st");
}

std::vector<ProgramInfo> ProgramStore::list() const {
    std::lock_guard lock(mutex_);
    std::vector<ProgramInfo> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".st") continue;
        std::string name = entry.path().stem().string();
        if (!isValidName(name)) continue;
        out.push_back({name, static_cast<uint64_t>(entry.file_size(ec)), toEpochMs(entry.last_write_time(ec))});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

bool ProgramStore::exists(std::string_view name) const {
    if (!isValidName(name)) return false;
    std::lock_guard lock(mutex_);
    std::error_code ec;
    return fs::is_regular_file(pathOf(canonicalLocked(name)), ec);
}

std::expected<std::string, std::string> ProgramStore::read(std::string_view name) const {
    if (!isValidName(name)) return std::unexpected(std::format("invalid program name '{}'", name));
    std::lock_guard lock(mutex_);
    fs::path path = pathOf(canonicalLocked(name));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::unexpected(std::format("program '{}' does not exist", name));
    }
    return readFile(path);
}

std::expected<void, std::string> ProgramStore::keepVersionLocked(const std::string& name, const fs::path& file,
                                                                 const std::string& content) {
    // Named by the time this content was saved (the file's modification time), not by the time it
    // was replaced; versions saved within one millisecond get a sequence number, so none replaces another
    std::error_code ec;
    auto modified = fs::last_write_time(file, ec);
    int64_t savedMs = ec ? nowMs() : toEpochMs(modified);
    fs::path historyDir = dir_ / ".history" / name;
    std::string id = versionIdAt(savedMs);
    for (int n = 1; fs::exists(historyDir / (id + ".st"), ec) && n < 1000; n++) {
        id = std::format("{}{:03}", versionIdAt(savedMs), n);
    }
    if (auto ok = writeFileAtomic(historyDir / (id + ".st"), content); !ok) return ok;
    std::vector<fs::path> versions;
    for (const auto& entry : fs::directory_iterator(historyDir, ec)) {
        if (entry.path().extension() == ".st") versions.push_back(entry.path());
    }
    std::sort(versions.begin(), versions.end());
    while (versions.size() > kHistoryLimit) {
        fs::remove(versions.front(), ec);
        versions.erase(versions.begin());
    }
    return {};
}

std::expected<void, std::string> ProgramStore::write(std::string_view requested, std::string_view source) {
    if (!isValidName(requested)) {
        return std::unexpected(std::format(
            "invalid program name '{}': use letters, digits, _ and -, at most 64 characters", requested));
    }
    if (source.size() > kMaxSourceBytes) {
        return std::unexpected(std::format("program is larger than {} KB", kMaxSourceBytes / 1024));
    }
    std::lock_guard lock(mutex_);
    std::string name = canonicalLocked(requested);
    fs::path path = pathOf(name);
    std::error_code ec;

    // A program may use every other file as a library: their total is what one compilation may read
    uint64_t others = 0;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".st" || entry.path().stem().string() == name) continue;
        others += static_cast<uint64_t>(entry.file_size(ec));
    }
    if (others + source.size() > kMaxTotalBytes) {
        return std::unexpected(std::format(
            "the PLC's program files would hold {} KB together, more than the {} KB allowed; delete programs or "
            "libraries that are no longer needed (a deleted program stays in its history)",
            (others + source.size() + 1023) / 1024, kMaxTotalBytes / 1024));
    }

    // Keep the previous version, unless the content did not change
    if (fs::is_regular_file(path, ec)) {
        auto previous = readFile(path);
        if (previous && *previous == source) return {};
        if (previous) {
            if (auto ok = keepVersionLocked(name, path, *previous); !ok) return ok;
        }
    }
    return writeFileAtomic(path, source);
}

std::expected<void, std::string> ProgramStore::remove(std::string_view requested) {
    if (!isValidName(requested)) return std::unexpected(std::format("invalid program name '{}'", requested));
    std::lock_guard lock(mutex_);
    std::string name = canonicalLocked(requested);
    fs::path path = pathOf(name);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::unexpected(std::format("program '{}' does not exist", requested));
    // Its last content goes to its history: a delete can be undone with a restore
    auto content = readFile(path);
    if (!content) return std::unexpected(content.error());
    if (auto ok = keepVersionLocked(name, path, *content); !ok) return ok;
    if (!fs::remove(path, ec)) return std::unexpected(std::format("cannot delete program '{}': {}", name, ec.message()));
    fs::remove_all(dir_ / ".deployed" / name, ec);
    fs::remove(dir_ / ".deployed" / (name + ".st"), ec);
    return {};
}

std::expected<void, std::string> ProgramStore::markDeployed(std::string_view requested, const std::vector<StoredFile>& files) {
    if (!isValidName(requested)) return std::unexpected(std::format("invalid program name '{}'", requested));
    for (const StoredFile& f : files) {
        if (!isValidName(f.name)) return std::unexpected(std::format("invalid program name '{}'", f.name));
    }
    std::lock_guard lock(mutex_);
    std::string name = canonicalLocked(requested);
    fs::path base = dir_ / ".deployed";
    fs::path target = base / name;
    std::error_code ec;
    // Unchanged (an online change of the same files, e.g. after a tag change): nothing to write
    {
        std::vector<std::string> order = orderOf(target);
        bool same = order.size() == files.size();
        for (size_t i = 0; same && i < files.size(); i++) {
            auto text = readFile(target / (files[i].name + ".st"));
            same = order[i] == files[i].name && text && *text == files[i].text;
        }
        if (same) return {};
    }
    // A new directory, then one rename: a restart in between finds the old set or the new one, never half of each
    fs::path fresh = base / (name + ".new");
    fs::path old = base / (name + ".old");
    fs::remove_all(fresh, ec);
    std::string order;
    for (const StoredFile& f : files) {
        if (auto ok = writeFileAtomic(fresh / (f.name + ".st"), f.text); !ok) return ok;
        order += f.name + "\n";
    }
    if (auto ok = writeFileAtomic(fresh / ".order", order); !ok) return ok;
    fs::remove_all(old, ec);
    ec.clear();
    if (fs::exists(target, ec)) fs::rename(target, old, ec);
    ec.clear();
    fs::rename(fresh, target, ec);
    if (ec) return std::unexpected(std::format("cannot record the deployed version of '{}': {}", name, ec.message()));
    fs::remove_all(old, ec);
    fs::remove(base / (name + ".st"), ec);  // the single file older versions kept
    return {};
}

std::optional<DeployedSet> ProgramStore::deployed(std::string_view requested) const {
    if (!isValidName(requested)) return std::nullopt;
    std::lock_guard lock(mutex_);
    std::string name = canonicalLocked(requested);
    fs::path base = dir_ / ".deployed";
    // .old: the previous set, when a restart came between the two renames of markDeployed
    for (const fs::path& dir : {base / name, base / (name + ".old")}) {
        std::vector<std::string> order = orderOf(dir);
        DeployedSet set;
        bool complete = !order.empty();
        for (const std::string& file : order) {
            auto text = isValidName(file) ? readFile(dir / (file + ".st")) : std::unexpected(std::string("invalid name"));
            if (!text) {
                complete = false;
                break;
            }
            set.files.push_back({file, *text});
        }
        if (complete) return set;
    }
    if (auto single = readFile(base / (name + ".st"))) {
        DeployedSet set;
        set.files.push_back({name, *single});
        set.legacy = true;
        return set;
    }
    return std::nullopt;
}

void ProgramStore::clearDeployed() {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::remove_all(dir_ / ".deployed", ec);
}

std::expected<void, std::string> ProgramStore::retire(std::string_view requested) {
    if (!isValidName(requested)) return std::unexpected(std::format("invalid program name '{}'", requested));
    std::lock_guard lock(mutex_);
    std::string name = canonicalLocked(requested);
    fs::path path = pathOf(name);
    auto content = readFile(path);
    if (!content) return std::unexpected(content.error());
    if (auto ok = keepVersionLocked(name, path, *content); !ok) return ok;
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove_all(dir_ / ".deployed" / name, ec);
    fs::remove(dir_ / ".deployed" / (name + ".st"), ec);
    return {};
}

std::vector<ProgramVersion> ProgramStore::history(std::string_view requested) const {
    std::vector<ProgramVersion> out;
    if (!isValidName(requested)) return out;
    std::lock_guard lock(mutex_);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_ / ".history" / canonicalLocked(requested), ec)) {
        if (entry.path().extension() != ".st") continue;
        std::string id = entry.path().stem().string();
        out.push_back({id, static_cast<uint64_t>(entry.file_size(ec)), versionTimeMs(id)});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    return out;
}

std::expected<std::string, std::string> ProgramStore::readVersion(std::string_view requested,
                                                                  std::string_view id) const {
    bool validId = !id.empty() && id.size() <= 32 &&
                   std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || c == 'T' || c == 'Z'; });
    if (!isValidName(requested) || !validId) {
        return std::unexpected(std::format("version '{}' of '{}' does not exist", id, requested));
    }
    std::lock_guard lock(mutex_);
    fs::path path = dir_ / ".history" / canonicalLocked(requested) / (std::string(id) + ".st");
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::unexpected(std::format("version '{}' of '{}' does not exist", id, requested));
    }
    return readFile(path);
}

}  // namespace simpleplc::runtime
