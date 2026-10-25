#pragma once
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/instance.h"

namespace simpleplc {

/// How Engine::update may change the configuration.
struct UpdateOptions {
    bool force = false;           ///< apply a tag change that breaks a running program (it faults)
    bool createPrograms = false;  ///< a task may name a program that does not exist: it starts from the template
};

/**
 * The device: its configuration and its one PLC.
 *
 * A SimplePLC process is one PLC, like a hardware PLC or a container running a
 * virtual PLC. Several PLCs are several processes or containers that talk
 * Modbus TCP and OPC UA over the network; `simpleplc fleet` shows them together.
 *
 * Layout of the data directory:
 *   config.json              settings and the PLC
 *   users.json               panel users and API tokens (see web/auth.h)
 *   programs/<name>.st      programs, with .history/ for earlier versions
 *   mode                     "run" or "stop": what an operator last commanded (Instance::commandedMode)
 *
 * Every change is validated, saved, then applied. Thread-safe. One process at a time uses a data
 * directory: open() takes an exclusive lock on its `.lock` file.
 */
class Engine {
public:
    /// What a new data directory starts with (from the command line or the environment).
    struct FirstRun {
        std::string id;                 ///< empty: from the host name
        std::string name;               ///< empty: the example's title, or the id
        std::string example;            ///< empty: a blank PLC with a template program (as shipped)
        int modbusPort = 502;
        int opcuaPort = 4840;
        std::string listen;             ///< address of Modbus and OPC UA; empty: every interface
    };

    /// Why a configuration change was refused.
    struct ChangeError {
        bool stale = false;                 ///< the configuration changed since the version the caller gave
        std::vector<std::string> problems;  ///< what is wrong with the change (or why it is stale)
    };
    /// A change of the PLC's configuration: the next configuration from the current one. It may
    /// throw to refuse the change; nothing else changes the configuration while it runs.
    using Edit = std::function<PlcConfig(const PlcConfig& current)>;

    explicit Engine(std::filesystem::path dataDir);
    Engine(std::filesystem::path dataDir, FirstRun firstRun);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Loads config.json (creating it on first run) and opens the PLC.
    std::expected<void, std::vector<std::string>> open();
    void close();
    /// Writes the PLC's retentive memory when it changed, at most once a second: for the main loop.
    void saveRetained();

    const std::filesystem::path& dataDir() const { return dataDir_; }
    /// True when this run created the data directory's configuration.
    bool firstRun() const { return firstRun_; }

    Config config() const;
    /// The configuration's version (also Config::configVersion of config(), consistent with it there).
    uint64_t configVersion() const;
    std::expected<void, std::vector<std::string>> updateSettings(const WebConfig& web, const std::string& logLevel);
    /// The port the web panel actually listens on: Modbus and OPC UA must not take it (0: none).
    void setWebPortInUse(int port);

    /// The PLC (null before open() and after close()).
    std::shared_ptr<Instance> instance() const;
    /// The PLC when `id` is its id, otherwise null.
    std::shared_ptr<Instance> find(std::string_view id) const;

    /// Replaces the PLC's configuration, changing only what changed (see Instance::reconfigure). A task
    /// that names a program that does not exist is refused unless `createPrograms`. `report` says which
    /// programs refused the change, and which tasks started from initial values.
    std::expected<void, std::vector<std::string>> update(PlcConfig next, const UpdateOptions& options = {},
                                                         ReconfigureReport* report = nullptr);
    /// Changes the PLC's configuration in one step: reads the current configuration, makes the next
    /// with `edit` and applies it as update() does, with no other change in between (no lost updates).
    /// With `ifVersion`, refuses (stale) unless the configuration still has that version. Returns the new version.
    std::expected<uint64_t, ChangeError> change(const Edit& edit, std::optional<uint64_t> ifVersion = std::nullopt,
                                                const UpdateOptions& options = {}, ReconfigureReport* report = nullptr);
    /// Sets the PLC up from scratch: configuration and programs at once, then a cold start in RUN or STOP as
    /// `run` says. `by` is who asked: a user's or a token's `run` is the commanded mode (Instance::commandedMode).
    std::expected<void, std::vector<std::string>> setup(PlcConfig next, const std::map<std::string, std::string>& programs,
                                                        bool run, const log::Author& by = log::Author::system());
    /// setup() unless the configuration's version is no longer `ifVersion` (then stale).
    std::expected<void, ChangeError> setup(PlcConfig next, const std::map<std::string, std::string>& programs, bool run,
                                           std::optional<uint64_t> ifVersion, const log::Author& by = log::Author::system());

    /// Program created for tasks that have none yet.
    static std::string templateProgram();

private:
    std::expected<void, std::vector<std::string>> commit(const Config& candidate);
    std::expected<void, std::vector<std::string>> updateLocked(PlcConfig next, const UpdateOptions& options,
                                                               ReconfigureReport* report);
    std::expected<void, std::vector<std::string>> setupLocked(PlcConfig next, const std::map<std::string, std::string>& programs,
                                                              bool run, const log::Author& by);
    std::expected<void, std::vector<std::string>> writeMissingPrograms(const PlcConfig& plc);
    /// Tasks of `next` that newly name a program that does not exist, as messages.
    std::vector<std::string> missingPrograms(const PlcConfig& next) const;
    std::expected<void, std::string> lockDataDir();
    void unlockDataDir();

    mutable std::mutex mutex_;
    std::filesystem::path dataDir_;
    FirstRun firstRunDefaults_;
    Config config_;
    std::shared_ptr<Instance> instance_;
    bool firstRun_ = false;
    int webPortInUse_ = 0;
#ifdef _WIN32
    void* lockHandle_ = nullptr;  // HANDLE of <data>/.lock, opened without sharing
#else
    int lockFd_ = -1;             // <data>/.lock, flock()ed
#endif
};

}  // namespace simpleplc
