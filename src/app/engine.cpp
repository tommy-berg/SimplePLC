#include "app/engine.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#include "app/examples.h"
#include "net/socket.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace simpleplc {
namespace fs = std::filesystem;

Engine::Engine(fs::path dataDir) : Engine(std::move(dataDir), FirstRun{}) {}

Engine::Engine(fs::path dataDir, FirstRun firstRun) : dataDir_(std::move(dataDir)), firstRunDefaults_(std::move(firstRun)) {}

Engine::~Engine() { close(); }

std::string Engine::templateProgram() {
    return R"((* The program runs once every cycle: tags are read in, the statements run,
   and the tags it changed are written out. Declare the tags it uses in
   VAR_EXTERNAL; variables in VAR keep their values from cycle to cycle.
   The Help page describes the language; examples are in the Program menu. *)
PROGRAM main
VAR_EXTERNAL
    (* switch : BOOL; *)
    (* lamp : BOOL; *)
END_VAR
VAR
END_VAR
    (* For example, copy a switch to a lamp: *)
    (* lamp := switch; *)
END_PROGRAM
)";
}

std::expected<void, std::string> Engine::lockDataDir() {
    // Two processes on one data directory would each write the configuration and programs from what they
    // hold in memory, undoing the other's changes: the second one must not start
    fs::path path = dataDir_ / ".lock";
    std::string pid = std::to_string(
#ifdef _WIN32
        GetCurrentProcessId()
#else
        getpid()
#endif
    );
    auto inUse = [&](const std::string& holder) {
        return std::format("the data directory {} is in use by another SimplePLC process{}: one directory belongs to one "
                           "PLC; stop the other process, or give this one its own directory with --data",
                           dataDir_.string(), holder.empty() ? "" : std::format(" (process {})", holder));
    };
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            std::string holder = runtime::readFile(path).value_or("");
            return std::unexpected(inUse(holder.substr(0, holder.find_first_of("\r\n"))));
        }
        return std::unexpected(std::format("cannot lock the data directory {}", dataDir_.string()));
    }
    SetEndOfFile(h);
    DWORD written = 0;
    WriteFile(h, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
    lockHandle_ = h;
#else
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        int error = errno;
        std::string message = std::format("cannot create {}: {}", path.string(), std::strerror(error));
        if (error == EACCES || error == EPERM) {
            // The usual case: a Docker bind mount of a root-owned host directory, while the image runs as uid 65534
            message += std::format("; the data directory is not writable by this user (uid {}); in Docker, chown the mounted "
                                   "directory to 65534 or run the container with --user",
                                   static_cast<unsigned long>(getuid()));
        }
        return std::unexpected(message);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        std::string holder = runtime::readFile(path).value_or("");
        ::close(fd);
        return std::unexpected(inUse(holder.substr(0, holder.find_first_of("\r\n"))));
    }
    if (ftruncate(fd, 0) == 0) (void)!::write(fd, pid.data(), pid.size());
    lockFd_ = fd;
#endif
    return {};
}

void Engine::unlockDataDir() {
#ifdef _WIN32
    if (lockHandle_) CloseHandle(static_cast<HANDLE>(lockHandle_));
    lockHandle_ = nullptr;
#else
    if (lockFd_ >= 0) ::close(lockFd_);  // releases the flock
    lockFd_ = -1;
#endif
}

std::expected<void, std::vector<std::string>> Engine::commit(const Config& candidate) {
    auto problems = validateConfig(candidate, webPortInUse_);
    if (!problems.empty()) return std::unexpected(problems);
    if (auto saved = saveConfig(dataDir_ / "config.json", candidate); !saved) {
        return std::unexpected(std::vector<std::string>{saved.error()});
    }
    config_ = candidate;
    return {};
}

std::expected<void, std::vector<std::string>> Engine::writeMissingPrograms(const PlcConfig& plc) {
    runtime::ProgramStore store(dataDir_ / "programs");
    for (const TaskConfig& task : plc.tasks) {
        if (store.exists(task.program)) continue;
        if (auto ok = store.write(task.program, templateProgram()); !ok) return std::unexpected(std::vector<std::string>{ok.error()});
    }
    return {};
}

std::expected<void, std::vector<std::string>> Engine::open() {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::create_directories(dataDir_, ec);
    if (ec) {
        return std::unexpected(std::vector<std::string>{
            std::format("cannot create the data directory {}: {}", dataDir_.string(), ec.message())});
    }
    if (auto locked = lockDataDir(); !locked) return std::unexpected(std::vector<std::string>{locked.error()});
    auto fail = [&](std::vector<std::string> problems) {
        unlockDataDir();
        return std::unexpected(std::move(problems));
    };

    fs::path configPath = dataDir_ / "config.json";
    Config config;
    if (fs::exists(configPath)) {
        auto loaded = loadConfig(configPath);
        if (!loaded) return fail(loaded.error());
        config = std::move(*loaded);
    } else {
        // First run: the PLC is named after the host (a container's or a Raspberry Pi's name) and starts
        // blank, unless told to start from an example
        firstRun_ = true;
        const FirstRun& d = firstRunDefaults_;
        PlcConfig& plc = config.plc;
        plc.id = d.id.empty() ? plcIdFromHostname(net::hostname()) : d.id;
        plc.modbus.port = d.modbusPort;
        plc.opcua.port = d.opcuaPort;
        if (!d.listen.empty()) plc.modbus.listen = plc.opcua.listen = d.listen;
        runtime::ProgramStore store(dataDir_ / "programs");
        if (!d.example.empty()) {
            auto example = findExample(d.example);
            if (!example) return fail({std::format("no example named '{}'", d.example)});
            plc.name = example->title;
            plc.description = example->description;
            plc.tasks = example->tasks;
            plc.tags = example->tags;
            plc.links = example->links;
            for (const auto& [name, source] : example->programs) {
                if (auto ok = store.write(name, source); !ok) return fail({ok.error()});
            }
        }
        if (!d.name.empty()) plc.name = d.name;
        if (plc.name.empty()) plc.name = plc.id;
        if (auto ok = writeMissingPrograms(plc); !ok) return fail(ok.error());
        if (auto ok = commit(config); !ok) return fail(ok.error());
    }

    if (auto level = log::parseLevel(config.logLevel)) log::setLevel(*level);
    config_ = config;
    instance_ = std::make_shared<Instance>(config_.plc, dataDir_);
    // Like a hardware PLC, the process powers up in the mode it was commanded last: a STOP for maintenance
    // survives a power cut and the service's restart. autostart false is STOP at every power-up
    bool run = config_.plc.autostart;
    if (run && Instance::commandedMode(dataDir_) == "stop") {
        run = false;
        log::write(log::Level::Info, "plc", config_.plc.id,
                   "starts in STOP: it was stopped when the process last ended; Run starts it");
    }
    instance_->open(run);
    return {};
}

void Engine::saveRetained() {
    if (auto instance = this->instance()) instance->saveRetained();
}

void Engine::close() {
    std::lock_guard lock(mutex_);
    if (instance_) instance_->close();
    instance_.reset();
    unlockDataDir();
}

Config Engine::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

uint64_t Engine::configVersion() const {
    std::lock_guard lock(mutex_);
    return config_.configVersion;
}

void Engine::setWebPortInUse(int port) {
    std::lock_guard lock(mutex_);
    webPortInUse_ = port;
}

std::expected<void, std::vector<std::string>> Engine::updateSettings(const WebConfig& web,
                                                                     const std::string& logLevel) {
    std::lock_guard lock(mutex_);
    Config candidate = config_;
    candidate.web = web;
    candidate.logLevel = logLevel;
    if (auto ok = commit(candidate); !ok) return ok;
    if (auto level = log::parseLevel(logLevel)) log::setLevel(*level);
    return {};
}

std::shared_ptr<Instance> Engine::instance() const {
    std::lock_guard lock(mutex_);
    return instance_;
}

std::shared_ptr<Instance> Engine::find(std::string_view id) const {
    std::lock_guard lock(mutex_);
    return instance_ && config_.plc.id == id ? instance_ : nullptr;
}

std::expected<void, std::vector<std::string>> Engine::update(PlcConfig next, const UpdateOptions& options,
                                                             ReconfigureReport* report) {
    std::lock_guard lock(mutex_);
    return updateLocked(std::move(next), options, report);
}

std::expected<void, std::vector<std::string>> Engine::updateLocked(PlcConfig next, const UpdateOptions& options,
                                                                   ReconfigureReport* report) {
    if (!instance_) return std::unexpected(std::vector<std::string>{"the PLC is not open"});
    Config candidate = config_;
    candidate.plc = next;
    if (next != config_.plc) candidate.configVersion++;
    if (auto problems = validateConfig(candidate, webPortInUse_); !problems.empty()) return std::unexpected(problems);
    if (auto conflicts = instance_->linkConflicts(next); !conflicts.empty()) return std::unexpected(conflicts);
    // A task that newly names a program that does not exist is most likely a typo: an empty program
    // would replace the control logic. Only an explicit request starts one from the template
    std::vector<std::string> missing = missingPrograms(next);
    if (!missing.empty() && !options.createPrograms) return std::unexpected(missing);
    if (!missing.empty()) {
        if (auto ok = writeMissingPrograms(next); !ok) return ok;
    }
    std::vector<std::string> commitProblems;
    auto applied = instance_->reconfigure(next, {options.force}, report, [&]() -> std::expected<void, std::string> {
        auto ok = commit(candidate);
        if (ok) return {};
        commitProblems = ok.error();
        return std::unexpected(commitProblems.empty() ? std::string("the configuration could not be saved") : commitProblems.front());
    });
    if (!applied) return std::unexpected(commitProblems.empty() ? std::vector<std::string>{applied.error()} : commitProblems);
    return {};
}

std::vector<std::string> Engine::missingPrograms(const PlcConfig& next) const {
    runtime::ProgramStore store(dataDir_ / "programs");
    std::vector<std::string> problems;
    for (const TaskConfig& task : next.tasks) {
        // Only tasks that are new or name another program: a task the PLC already has stays as it is
        bool unchanged = std::any_of(config_.plc.tasks.begin(), config_.plc.tasks.end(), [&](const TaskConfig& t) {
            return t.name == task.name && runtime::ProgramStore::sameName(t.program, task.program);
        });
        if (unchanged || store.exists(task.program)) continue;
        problems.push_back(std::format(
            "task '{}': there is no program named '{}'; save the program first, or pass \"create\": true to start it "
            "from an empty template",
            task.name, task.program));
    }
    return problems;
}

std::expected<uint64_t, Engine::ChangeError> Engine::change(const Edit& edit, std::optional<uint64_t> ifVersion,
                                                          const UpdateOptions& options, ReconfigureReport* report) {
    std::lock_guard lock(mutex_);
    if (ifVersion && *ifVersion != config_.configVersion) {
        return std::unexpected(ChangeError{true, {std::format("the configuration changed since you loaded it (version {}, "
                                                              "now {}); load it again and repeat the change",
                                                              *ifVersion, config_.configVersion)}});
    }
    PlcConfig next = edit(config_.plc);  // may throw: nothing has changed then
    if (auto ok = updateLocked(std::move(next), options, report); !ok) return std::unexpected(ChangeError{false, ok.error()});
    return config_.configVersion;
}

std::expected<void, std::vector<std::string>> Engine::setup(PlcConfig next, const std::map<std::string, std::string>& programs,
                                                            bool run, const log::Author& by) {
    std::lock_guard lock(mutex_);
    return setupLocked(std::move(next), programs, run, by);
}

std::expected<void, Engine::ChangeError> Engine::setup(PlcConfig next, const std::map<std::string, std::string>& programs,
                                                       bool run, std::optional<uint64_t> ifVersion, const log::Author& by) {
    std::lock_guard lock(mutex_);
    if (ifVersion && *ifVersion != config_.configVersion) {
        return std::unexpected(ChangeError{true, {std::format("the configuration changed since you loaded it (version {}, "
                                                              "now {})", *ifVersion, config_.configVersion)}});
    }
    if (auto ok = setupLocked(std::move(next), programs, run, by); !ok) return std::unexpected(ChangeError{false, ok.error()});
    return {};
}

std::expected<void, std::vector<std::string>> Engine::setupLocked(PlcConfig next, const std::map<std::string, std::string>& programs,
                                                                  bool run, const log::Author& by) {
    if (!instance_) return std::unexpected(std::vector<std::string>{"the PLC is not open"});
    Config candidate = config_;
    candidate.plc = next;
    candidate.configVersion++;  // programs change too, so always a new version
    if (auto problems = validateConfig(candidate, webPortInUse_); !problems.empty()) return std::unexpected(problems);

    // Programs first, so the PLC starts with them
    runtime::ProgramStore store(dataDir_ / "programs");
    for (const auto& [name, source] : programs) {
        if (auto ok = store.write(name, source); !ok) return std::unexpected(std::vector<std::string>{ok.error()});
    }
    // From scratch: the old configuration's other programs would be libraries of the new ones, and its
    // deployed versions a fallback. They go to their history (restorable), and nothing old is deployed
    for (const runtime::ProgramInfo& p : store.list()) {
        bool used = programs.count(p.name) > 0;
        for (const TaskConfig& t : next.tasks) used = used || t.program == p.name;
        if (!used) (void)store.retire(p.name);
    }
    store.clearDeployed();
    if (auto ok = writeMissingPrograms(next); !ok) return ok;
    if (auto ok = commit(candidate); !ok) return ok;

    instance_->close();
    instance_->clearRetained();  // a new PLC starts from initial values
    instance_ = std::make_shared<Instance>(next, dataDir_);
    instance_->open(run, by);
    log::write(log::Level::Info, "plc", next.id, std::format("set up as '{}'", next.name));
    return {};
}

}  // namespace simpleplc
