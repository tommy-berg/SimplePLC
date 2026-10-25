#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/log.h"
#include "core/process_image.h"
#include "modbus/links.h"
#include "modbus/tcp_server.h"
#include "opcua/opcua_server.h"
#include "runtime/program_store.h"
#include "runtime/task.h"
#include "st/compiler.h"

namespace simpleplc {

struct ServerStatus {
    bool enabled = false;
    bool listening = false;
    int configuredPort = 0;
    int port = 0;          ///< port actually used (may differ after a fallback)
    std::string error;
};

struct TaskStatus {
    TaskConfig config;
    runtime::TaskStats stats;
};

/**
 * What the links may send to devices: nothing while RUN waits for the programs' first cycle, the
 * outputs in RUN, the substitute values in STOP and FAULT. Shared with the link threads; it has its own
 * lock, never the instance's.
 */
class OutputGate {
public:
    /// RUN is about to start: nothing goes out until run().
    void hold();
    /// RUN: the outputs go out once each of `tasks` has completed a cycle after `cycles` (its count now).
    void run(std::vector<std::pair<std::shared_ptr<runtime::Task>, uint64_t>> tasks);
    /// STOP or FAULT: the substitute values go out.
    void substitute();
    modbus::Outputs state();

private:
    std::mutex mutex_;
    modbus::Outputs state_ = modbus::Outputs::Substitute;
    std::vector<std::pair<std::shared_ptr<runtime::Task>, uint64_t>> pending_;
};

/// The result of compiling and loading one program into the tasks that run it.
struct ProgramLoad {
    std::string program;
    bool ok = false;
    std::vector<std::string> tasks;     ///< tasks that run it (loaded only when ok)
    std::map<std::string, std::string> taskErrors;  ///< tasks it compiled for but was not loaded into, and why
    st::CompileResult compiled;         ///< diagnostics, listing, memory
    std::vector<std::string> reset;     ///< variables that started from their initial value (online change)
    std::string error;                  ///< not loaded although it compiled (e.g. the file changed meanwhile)
    bool replaced = false;              ///< an older version's values were taken over
};

/// What saving a program file did.
struct SaveOutcome {
    std::string program;                ///< the name the file has (names differing only by case: the existing file's)
    bool saved = false;
    std::string error;                  ///< not saved: an invalid name, too large
    std::optional<std::string> conflictHash;  ///< not saved: the file is not the version the caller based it on (its hash; empty: no file)
    bool deployed = false;              ///< loading was asked for (deploy)
    bool library = false;               ///< no PROGRAM and no task runs it: checked as a library
    ProgramLoad own;                    ///< the file in its own role: loaded into its tasks, or checked
    std::vector<ProgramLoad> dependents;  ///< running programs that use the file as a library, compiled with it
    bool ok() const {
        return saved && own.ok && std::all_of(dependents.begin(), dependents.end(), [](const ProgramLoad& d) { return d.ok; });
    }
};

/// A task whose program would not compile (or load) after a configuration change.
struct ProgramProblem {
    std::string task;
    std::string program;
    st::CompileResult compiled;         ///< its diagnostics (none when `error` says why)
    std::string error;
};

struct ReconfigureOptions {
    bool force = false;                 ///< apply a tag change even if a running program no longer compiles (it faults)
};

struct ReconfigureReport {
    std::vector<ProgramProblem> tagBreaks;     ///< running programs a tag change would break (refused unless forced)
    std::vector<ProgramProblem> taskProblems;  ///< task changes whose program does not compile or load (refused)
    std::vector<std::string> restartedTasks;   ///< tasks that started from initial values because of the change
};

/**
 * One PLC: its process image, programs (tasks), Modbus and OPC UA servers.
 *
 * Like a hardware PLC, communication runs whenever the instance is open; run()
 * and halt() switch the programs between RUN and STOP. A run-time fault in any
 * task puts the PLC in FAULT: every task stops, tags with a substitute value get it,
 * and run() restarts warm: RETAIN variables keep their values, all others start from their initial
 * values. STOP also writes the substitute values.
 * All public methods are thread-safe.
 */
class Instance {
public:
    Instance(PlcConfig config, std::filesystem::path dir);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    /// Starts communication, loads the programs and, when `runProgram`, starts them. `by` is who asked for the
    /// start (a cold restart, a setup): a user's or a token's choice becomes the commanded mode (see commandedMode()).
    void open(bool runProgram, const log::Author& by = log::Author::system());
    void close();

    /// Retentive memory, `<dir>/retain.json`: the values of `retain` tags and of RETAIN variables. Written when
    /// they changed since the last write, at most once a second (call it from the main loop) and once more when
    /// the PLC closes; `now` skips the wait. open() restores the values before the programs start.
    void saveRetained(bool now = false);
    /// Forgets the retentive memory: a cold restart or a setup starts from initial values. Call it closed.
    void clearRetained();

    /// RUN and STOP, recorded as "mode" events made by `by`. Commanded by a user or a token, the mode is also
    /// the commanded mode; SimplePLC's own halts (a fault, a reconfiguration, shutdown) leave it as it is.
    void run(const log::Author& by = log::Author::system());
    void halt(const log::Author& by = log::Author::system());
    bool running() const;
    /// The mode a user or a token last commanded on the PLC of data directory `dir`, kept in `<dir>/mode`: "run"
    /// or "stop", none when nobody has commanded one. The process starts in it, as a hardware PLC powers up
    /// in the mode it was in when the power went.
    static std::optional<std::string> commandedMode(const std::filesystem::path& dir);

    /**
     * Applies a validated configuration, changing only what changed: tasks that stay keep their
     * program and its values. First every program a task will run is compiled against the new
     * tags; a change that breaks one is refused (see ReconfigureReport). `commit` then saves the
     * configuration; its error refuses the change too.
     */
    std::expected<void, std::string> reconfigure(const PlcConfig& next, const ReconfigureOptions& options = {},
                                                 ReconfigureReport* report = nullptr,
                                                 const std::function<std::expected<void, std::string>()>& commit = {});

    /**
     * Saves a program file and, with `deploy`, loads it: into the tasks that run it, and into the
     * running programs that use it as a library (online changes). A version that does not compile
     * or load stays a draft: nothing runs it, now or after a restart. Saves apply one at a time,
     * in order. `ifMatch`: the SHA-256 (hex) of the saved version the caller changed.
     */
    SaveOutcome saveProgram(const std::string& name, const std::string& source, bool deploy,
                            const std::optional<std::string>& ifMatch = std::nullopt);
    /// Deletes a program file that no task runs and no loaded program uses; the error has an HTTP status.
    std::expected<std::string, std::pair<int, std::string>> deleteProgram(const std::string& name);
    /// Compiles without loading (a check), against the PLC's tags or the given ones: as a program when
    /// a task runs it or it holds a PROGRAM, otherwise as a library.
    st::CompileResult checkProgram(const std::string& program, const std::string& source,
                                   const std::optional<std::vector<TagDef>>& tags = std::nullopt) const;
    /// What loading `next` as `program` would do to the values of the tasks that run it: computed as an online
    /// change takes values over (same path and identical type definition; an enumeration by name; a changed
    /// constant is reset). None when no task runs `program`.
    struct OnlineChangePreview {
        std::vector<std::string> tasks;
        size_t kept = 0;                  ///< variables that keep their values
        std::vector<std::string> reset;   ///< variables that start from their initial value
    };
    std::optional<OnlineChangePreview> previewOnlineChange(const std::string& program, const vm::Image& next) const;
    /// The files a compilation of `program` (with `source`, else its saved text) uses: the program first,
    /// then the libraries it needs. Libraries with syntax errors are left out (`skipped`).
    std::vector<st::SourceFile> programFiles(const std::string& program, const std::optional<std::string>& source,
                                             std::vector<std::string>* skipped = nullptr) const;
    /// The compilation of the version that runs (for a program no task runs: its last one), if any.
    std::optional<st::CompileResult> compiled(const std::string& program) const;
    /// The files the tasks run `program` with (its own first), if it is deployed.
    std::optional<std::vector<runtime::StoredFile>> deployedFiles(const std::string& program) const;
    /// The main task's program (the first task's): the default of the program tools.
    std::string mainProgram() const;
    /// The fault that stopped the PLC, if it is in FAULT.
    std::optional<runtime::FaultInfo> fault() const;
    std::shared_ptr<runtime::Task> task(const std::string& name) const;

    PlcConfig config() const;
    std::string id() const;
    std::shared_ptr<ProcessImage> image() const;
    runtime::ProgramStore& programs() { return programs_; }
    const std::filesystem::path& dir() const { return dir_; }

    std::vector<TaskStatus> tasks() const;

    ServerStatus modbusStatus() const;
    ServerStatus opcuaStatus() const;
    modbus::TcpServer::Stats modbusStats() const;
    /// This PLC as Modbus TCP client: its links to other devices.
    std::vector<modbus::LinkStatus> modbusLinks() const;
    /// Tags that `next`'s links would write although a loaded program writes them too.
    std::vector<std::string> linkConflicts(const PlcConfig& next) const;
    opcua::Server::Stats opcuaStats() const;
    /// Problems the user should know about (port fallbacks, programs that failed to load).
    std::vector<std::string> warnings() const;
    /// Tasks whose program did not load (at open, or at the last reload), with the reason.
    std::map<std::string, std::string> programErrors() const;

private:
    /// Links taken out of service under the lock; the caller destroys them after releasing it, so the
    /// lock never waits for a device (a link stops within milliseconds, or within its timeout with a flush).
    using RetiredLinks = std::vector<std::unique_ptr<modbus::LinkPoller>>;

    void openLocked(bool runProgram, const log::Author& by);
    /// Writes `<dir>/mode` (see commandedMode()); a failure is logged, the PLC runs on.
    void recordModeLocked(bool run);
    /// `flush`: the links send the substitute values once more before they stop (shutdown); without, they stop
    /// before STOP writes the substitute values (a restart, which must not drive the outputs to them).
    void closeLocked(RetiredLinks& retired, bool flush);
    void startModbus();
    void startOpcUa();
    /// Starts a poller for each configured link; one whose configuration is unchanged keeps running
    /// unless `restartAll`. The pollers of removed or changed links go to `retired`.
    void startLinks(RetiredLinks& retired, bool restartAll);
    int commPriority() const;  // real-time priority of the Modbus server and links (0: normal)
    static int commPriorityOf(const PlcConfig& config);
    void createTasks();
    std::shared_ptr<runtime::Task> makeTask(const TaskConfig& tc);
    void loadPrograms();

    void runLocked(const log::Author& by = log::Author::system());
    /// STOP: stops the tasks, writes the substitute values and records the "STOP" event.
    void haltLocked(const log::Author& by = log::Author::system());
    /// What STOP and FAULT share: the tasks stop, the substitute values go out, the identity says "stopped".
    void stopProgramsLocked();
    void writeSubstituteValues();
    /// Writes `text` to retain.json unless it is what was written last; a failure is logged once, not every second.
    void writeRetainedLocked(const std::string& text);
    std::vector<std::string> taskNamesLocked() const;
    std::vector<TagDef> currentTagsLocked() const;
    std::vector<runtime::StoredFile> savedFiles() const;
    /// `program` first (with `source` when given), then the libraries it uses from `base`; unparsable ones go to `skipped`.
    static std::vector<runtime::StoredFile> selectFiles(const std::string& program, const std::optional<std::string>& source,
                                                        std::vector<runtime::StoredFile> base, std::vector<std::string>* skipped);
    static st::CompileResult compileFiles(const std::vector<runtime::StoredFile>& files, const std::vector<TagDef>& tags,
                                          st::CompileMode mode = st::CompileMode::Program,
                                          const std::vector<std::string>& skipped = {});
    bool runByTaskLocked(const std::string& program) const;
    /// Loads a compiled program into the tasks that run it; with every task's consent, records it as deployed.
    ProgramLoad installLocked(const std::string& program, st::CompileResult compiled,
                              const std::vector<runtime::StoredFile>& files, bool checkWriters = true);
    std::string writerConflictLocked(const vm::Image& image, size_t task, const std::vector<LinkConfig>& links) const;
    void onTaskFault(const runtime::FaultInfo& f, uint64_t generation);
    void supervise();
    modbus::DeviceIdentity identity() const;

    mutable std::mutex mutex_;
    PlcConfig config_;
    std::filesystem::path dir_;
    runtime::ProgramStore programs_;
    log::Scope log_;

    std::shared_ptr<ProcessImage> image_;
    std::vector<std::unique_ptr<modbus::LinkPoller>> links_;
    std::shared_ptr<OutputGate> outputs_ = std::make_shared<OutputGate>();  // what the links may send
    std::unique_ptr<modbus::TcpServer> modbus_;
    std::unique_ptr<opcua::Server> opcua_;
    std::vector<std::shared_ptr<runtime::Task>> tasks_;  // shared so reloads can run unlocked
    bool open_ = false;
    bool running_ = false;
    std::optional<runtime::FaultInfo> fault_;
    struct Deployment {
        std::vector<runtime::StoredFile> files;  // the files it was compiled with (its own first)
        st::CompileResult compiled;
    };
    std::map<std::string, Deployment> loaded_;           // program -> the version its tasks run
    std::map<std::string, st::CompileResult> attempts_;  // program -> its last compilation (drafts too)
    // Retentive memory (mutex_): the text retain.json holds, when it was last considered, the failure already reported
    std::string retainWritten_;
    std::chrono::steady_clock::time_point retainSavedAt_{};
    std::string retainError_;
    // Taken first by everything that changes which program versions run (saves, deletes,
    // reconfiguration, open): one at a time, in order. Compiles run under it, not under mutex_
    mutable std::mutex programsMutex_;

    // Faults arrive on task threads; this thread stops the PLC
    std::thread supervisor_;
    std::mutex faultMutex_;
    std::condition_variable faultWake_;
    struct QueuedFault {
        runtime::FaultInfo info;
        uint64_t generation = 0;  // the task set it came from (createTasks)
        uint64_t seq = 0;
    };
    std::deque<QueuedFault> faults_;
    uint64_t faultSeq_ = 0;          // faultMutex_
    uint64_t faultsClearedTo_ = 0;   // faultMutex_: RUN cleared the faults up to this one
    uint64_t generation_ = 0;        // mutex_
    bool supervisorStop_ = false;

    ServerStatus modbusStatus_;
    ServerStatus opcuaStatus_;
    std::vector<std::string> commWarnings_;
    std::map<std::string, std::string> programErrors_;  // task -> error
};

}  // namespace simpleplc
