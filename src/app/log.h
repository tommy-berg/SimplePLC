#pragma once
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Process-wide logger.
 *
 * Records go to the console (stderr) and to a ring buffer that the web panel
 * and the API read. Every function here is thread-safe.
 */
namespace simpleplc::log {

enum class Level : uint8_t { Error, Warn, Info, Debug };

std::string_view levelName(Level level);
std::optional<Level> parseLevel(std::string_view name);

/// Who made a change that an event records: a signed-in user, an API token (agents use tokens) or
/// SimplePLC itself (a fault, a link going up or down).
struct Author {
    std::string kind = "system";  ///< "user", "token" or "system"
    std::string name = "SimplePLC";

    static Author system() { return {}; }
    bool operator==(const Author&) const = default;
};

struct Record {
    uint64_t seq = 0;
    int64_t timeMs = 0;       ///< Unix epoch milliseconds
    Level level = Level::Info;
    std::string component;    ///< "modbus", "opcua", "task", "web", ...
    std::string plc;          ///< PLC id, empty for process-wide records
    std::string message;
    /// An event (a change, or a change of state): "program", "force", "release", "write", "config", "mode",
    /// "link" or "access"; empty for other records
    std::string event;
    std::optional<Author> by;  ///< who made it (events only)
};

/// Records at or below this level are kept (default Info).
void setLevel(Level level);
/// Console threshold; defaults to the general level.
void setConsoleLevel(Level level);
void setJsonConsole(bool json);
void setBufferCapacity(size_t records);

bool enabled(Level level);
/// Adds a record. The message is cut to 4 KB and control characters become spaces, so text
/// from programs or requests cannot forge lines or send terminal escapes.
void write(Level level, std::string_view component, std::string_view plc, std::string message);

/// Adds an event: a record of kind `kind` made by `by`. Events are kept whatever the log level, and
/// also apart from the other records, so a flood of those does not push the PLC's history out.
void event(Level level, std::string_view component, std::string_view plc, std::string_view kind, const Author& by,
           std::string message);
/// The newest events, oldest first, at most `limit`. An empty `plc` matches every event.
std::vector<Record> events(size_t limit, std::string_view plc = {});
/// The newest event a user or a token made (not SimplePLC itself), if one is still kept.
std::optional<Record> lastChange();

/// Cuts `text` to at most `maxBytes` at a UTF-8 character boundary, marking the cut with "...".
void truncate(std::string& text, size_t maxBytes);

/// Records newer than `afterSeq`, oldest first, at most `limit`, at or below `maxLevel`.
/// An empty `plc` matches every record.
std::vector<Record> since(uint64_t afterSeq, size_t limit, Level maxLevel = Level::Debug,
                          std::string_view plc = {});
uint64_t lastSeq();
/// Blocks until a record newer than `afterSeq` exists or the timeout expires.
bool waitForNewer(uint64_t afterSeq, std::chrono::milliseconds timeout);

/// Logger bound to a component and optionally a PLC id.
class Scope {
public:
    Scope(std::string component, std::string plc = {})
        : component_(std::move(component)), plc_(std::move(plc)) {}

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        emit(Level::Error, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        emit(Level::Warn, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        emit(Level::Info, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        emit(Level::Debug, fmt, std::forward<Args>(args)...);
    }

    /// An event of this component and PLC (see log::event).
    template <typename... Args>
    void event(Level level, std::string_view kind, const Author& by, std::format_string<Args...> fmt, Args&&... args) const {
        log::event(level, component_, plc_, kind, by, std::format(fmt, std::forward<Args>(args)...));
    }

    const std::string& plc() const { return plc_; }

private:
    template <typename... Args>
    void emit(Level level, std::format_string<Args...> fmt, Args&&... args) const {
        if (enabled(level)) {
            write(level, component_, plc_, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    std::string component_;
    std::string plc_;
};

}  // namespace simpleplc::log
