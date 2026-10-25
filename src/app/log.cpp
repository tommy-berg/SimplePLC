#include "app/log.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

#include <nlohmann/json.hpp>

namespace simpleplc::log {
namespace {

struct State {
    std::mutex mutex;
    std::condition_variable newRecord;
    std::deque<Record> buffer;
    std::deque<Record> events;         // the newest events, also when the buffer has dropped them
    std::optional<Record> lastChange;  // the newest event by a user or a token
    size_t capacity = 2000;
    uint64_t seq = 0;
    Level level = Level::Info;
    std::optional<Level> consoleLevel;
    bool json = false;
};

State& state() {
    static State s;
    return s;
}

std::string localTime(int64_t timeMs) {
    std::time_t seconds = static_cast<std::time_t>(timeMs / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::format("{}.{:03}", buf, timeMs % 1000);
}

void printConsole(const Record& r, bool json) {
    std::string line;
    if (json) {
        nlohmann::json j = {{"time", r.timeMs},        {"level", levelName(r.level)},
                            {"component", r.component}, {"plc", r.plc},
                            {"message", r.message}};
        if (!r.event.empty()) j["event"] = r.event;
        if (r.by) j["by"] = {{"kind", r.by->kind}, {"name", r.by->name}};
        line = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    } else {
        static constexpr char kLetters[] = {'E', 'W', 'I', 'D'};
        std::string scope = r.plc.empty() ? r.component : r.component + "[" + r.plc + "]";
        // An event says who made it: a user, "token <name>", or nothing when SimplePLC did
        std::string by = !r.by || r.by->kind == "system" ? std::string()
                         : r.by->kind == "token"          ? std::format(" (by token {})", r.by->name)
                                                          : std::format(" (by {})", r.by->name);
        line = std::format("{} {} {}: {}{}\n", localTime(r.timeMs),
                           kLetters[static_cast<size_t>(r.level)], scope, r.message, by);
    }
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

constexpr size_t kMaxMessage = 4096;
constexpr size_t kMaxEvents = 1000;

/// A record as it is kept: the time now, the message cut and without control characters.
Record makeRecord(Level level, std::string_view component, std::string_view plc, std::string message);

}  // namespace

void truncate(std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return;
    size_t cut = maxBytes >= 3 ? maxBytes - 3 : 0;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;  // continuation byte
    text.resize(cut);
    text += "...";
}

std::string_view levelName(Level level) {
    switch (level) {
        case Level::Error: return "error";
        case Level::Warn:  return "warn";
        case Level::Info:  return "info";
        case Level::Debug: return "debug";
    }
    return "info";
}

std::optional<Level> parseLevel(std::string_view name) {
    for (Level l : {Level::Error, Level::Warn, Level::Info, Level::Debug}) {
        if (levelName(l) == name) return l;
    }
    if (name == "warning") return Level::Warn;
    return std::nullopt;
}

void setLevel(Level level) {
    std::lock_guard lock(state().mutex);
    state().level = level;
}

void setConsoleLevel(Level level) {
    std::lock_guard lock(state().mutex);
    state().consoleLevel = level;
}

void setJsonConsole(bool json) {
    std::lock_guard lock(state().mutex);
    state().json = json;
}

void setBufferCapacity(size_t records) {
    std::lock_guard lock(state().mutex);
    state().capacity = std::max<size_t>(records, 100);
    while (state().buffer.size() > state().capacity) state().buffer.pop_front();
}

bool enabled(Level level) {
    std::lock_guard lock(state().mutex);
    Level threshold = std::max(state().level, state().consoleLevel.value_or(state().level));
    return level <= threshold;
}

namespace {
Record makeRecord(Level level, std::string_view component, std::string_view plc, std::string message) {
    Record record;
    record.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    record.level = level;
    record.component = component;
    record.plc = plc;
    truncate(message, kMaxMessage);
    for (char& c : message) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    }
    record.message = std::move(message);
    return record;
}
}  // namespace

void event(Level level, std::string_view component, std::string_view plc, std::string_view kind, const Author& by,
           std::string message) {
    State& s = state();
    Record record = makeRecord(level, component, plc, std::move(message));
    record.event = kind;
    Author author = by;
    truncate(author.name, 128);
    for (char& c : author.name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    }
    record.by = std::move(author);
    std::lock_guard lock(s.mutex);
    if (level <= s.consoleLevel.value_or(s.level)) printConsole(record, s.json);
    // Kept whatever the level: an event is the PLC's history, not a diagnostic
    record.seq = ++s.seq;
    s.events.push_back(record);
    if (s.events.size() > kMaxEvents) s.events.pop_front();
    if (record.by->kind != "system") s.lastChange = record;
    s.buffer.push_back(std::move(record));
    if (s.buffer.size() > s.capacity) s.buffer.pop_front();
    s.newRecord.notify_all();
}

std::vector<Record> events(size_t limit, std::string_view plc) {
    std::lock_guard lock(state().mutex);
    std::vector<Record> out;
    for (const Record& r : state().events) {
        if (plc.empty() || r.plc == plc) out.push_back(r);
    }
    if (out.size() > limit) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
    return out;
}

std::optional<Record> lastChange() {
    std::lock_guard lock(state().mutex);
    return state().lastChange;
}

void write(Level level, std::string_view component, std::string_view plc, std::string message) {
    State& s = state();
    Record record = makeRecord(level, component, plc, std::move(message));

    std::lock_guard lock(s.mutex);
    if (level <= s.consoleLevel.value_or(s.level)) {
        printConsole(record, s.json);
    }
    if (level <= s.level) {
        record.seq = ++s.seq;
        s.buffer.push_back(std::move(record));
        if (s.buffer.size() > s.capacity) s.buffer.pop_front();
        s.newRecord.notify_all();
    }
}

std::vector<Record> since(uint64_t afterSeq, size_t limit, Level maxLevel, std::string_view plc) {
    std::lock_guard lock(state().mutex);
    std::vector<Record> out;
    for (const Record& r : state().buffer) {
        if (r.seq <= afterSeq || r.level > maxLevel) continue;
        if (!plc.empty() && r.plc != plc) continue;
        out.push_back(r);
    }
    if (out.size() > limit) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
    return out;
}

uint64_t lastSeq() {
    std::lock_guard lock(state().mutex);
    return state().seq;
}

bool waitForNewer(uint64_t afterSeq, std::chrono::milliseconds timeout) {
    std::unique_lock lock(state().mutex);
    return state().newRecord.wait_for(lock, timeout, [&] { return state().seq > afterSeq; });
}

}  // namespace simpleplc::log
