#include "app/instance.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <set>

#include <nlohmann/json.hpp>

#include "app/version.h"
#include "st/lexer.h"
#include "st/outline.h"
#include "st/values.h"
#include "web/crypto.h"

namespace simpleplc {
namespace {

/// Why a program may not write `tag`: a link writes it (its read side or its status); empty when none does.
std::string linkConflict(const std::string& tag, const std::vector<LinkConfig>& links) {
    for (const LinkConfig& link : links) {
        bool writes = link.statusTag == tag ||
                      std::any_of(link.read.begin(), link.read.end(), [&](const LinkMapping& m) { return m.tag == tag; });
        if (writes) {
            return std::format("tag '{}' is written by link '{}'; a tag has one writer: let the program write another tag",
                               tag, link.name);
        }
    }
    return {};
}

/// Tags as the compiler sees them (name and type): a change of anything else (address, description,
/// substitute value) needs no new compile; addresses are bound when a cycle starts.
bool sameForCompiler(const std::vector<TagDef>& a, const std::vector<TagDef>& b) {
    auto key = [](const std::vector<TagDef>& tags) {
        std::vector<std::pair<std::string, TagType>> k;
        for (const TagDef& t : tags) k.push_back({t.name, t.type});
        std::sort(k.begin(), k.end());
        return k;
    };
    return key(a) == key(b);
}

/// The same files with the same content, in any order.
bool sameFiles(const std::vector<runtime::StoredFile>& a, const std::vector<runtime::StoredFile>& b) {
    if (a.size() != b.size()) return false;
    return std::all_of(a.begin(), a.end(), [&](const runtime::StoredFile& f) {
        return std::any_of(b.begin(), b.end(), [&](const runtime::StoredFile& g) {
            return runtime::ProgramStore::sameName(f.name, g.name) && f.text == g.text;
        });
    });
}

/// Advice shown when a port below 1024 needs privileges.
std::string privilegedPortHint() {
#if defined(__linux__)
    return "allow it once with: sudo setcap cap_net_bind_service=+ep \"$(command -v simpleplc || echo ./simpleplc)\"";
#elif defined(_WIN32)
    return "start SimplePLC as administrator";
#else
    return "start SimplePLC with sudo";
#endif
}

template <typename StartFn>
std::expected<uint16_t, net::NetError> startWithFallback(int configured, int fallbackFrom, StartFn start,
                                                         std::string& warning, const char* protocol) {
    auto result = start(static_cast<uint16_t>(configured));
    if (result || configured == 0) return result;
    net::NetError first = result.error();
    if (!first.permissionDenied && !first.addressInUse) return result;

    for (int port = fallbackFrom; port < fallbackFrom + 100; port++) {
        if (port == configured) continue;
        auto retry = start(static_cast<uint16_t>(port));
        if (retry) {
            warning = first.permissionDenied
                          ? std::format("{} port {} needs administrator rights, so {} uses port {}. To use {}, {}.",
                                        protocol, configured, protocol, port, configured, privilegedPortHint())
                          : std::format("{} port {} is already in use (another SimplePLC or {} server?), "
                                        "so {} uses port {}.",
                                        protocol, configured, protocol, protocol, port);
            return retry;
        }
        if (!retry.error().addressInUse && !retry.error().permissionDenied) break;
    }
    return std::unexpected(first);
}

}  // namespace

void OutputGate::hold() {
    std::lock_guard lock(mutex_);
    state_ = modbus::Outputs::Pending;
    pending_.clear();
}

void OutputGate::run(std::vector<std::pair<std::shared_ptr<runtime::Task>, uint64_t>> tasks) {
    std::lock_guard lock(mutex_);
    pending_ = std::move(tasks);
    state_ = pending_.empty() ? modbus::Outputs::Run : modbus::Outputs::Pending;
}

void OutputGate::substitute() {
    std::lock_guard lock(mutex_);
    state_ = modbus::Outputs::Substitute;
    pending_.clear();
}

modbus::Outputs OutputGate::state() {
    std::lock_guard lock(mutex_);
    if (state_ == modbus::Outputs::Pending && !pending_.empty()) {
        std::erase_if(pending_, [](const auto& p) { return p.first->stats().cycles > p.second; });
        if (pending_.empty()) state_ = modbus::Outputs::Run;
    }
    return state_;
}

Instance::Instance(PlcConfig config, std::filesystem::path dir)
    : config_(std::move(config)),
      dir_(std::move(dir)),
      programs_(dir_ / "programs"),
      log_("plc", config_.id) {
    supervisor_ = std::thread(&Instance::supervise, this);
}

Instance::~Instance() {
    close();
    {
        std::lock_guard lock(faultMutex_);
        supervisorStop_ = true;
    }
    faultWake_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();
}

void Instance::onTaskFault(const runtime::FaultInfo& f, uint64_t generation) {
    // Called on the faulting task's thread. The outputs are already safe (the handler put the image
    // in FAULT); the supervisor stops every task and records the fault
    {
        std::lock_guard lock(faultMutex_);
        faults_.push_back({f, generation, ++faultSeq_});
    }
    faultWake_.notify_all();
}

void Instance::supervise() {
    while (true) {
        QueuedFault q;
        {
            std::unique_lock lock(faultMutex_);
            faultWake_.wait(lock, [&] { return supervisorStop_ || !faults_.empty(); });
            if (supervisorStop_) return;
            q = faults_.front();
            faults_.pop_front();
        }
        const runtime::FaultInfo& f = q.info;
        std::lock_guard lock(mutex_);
        {
            std::lock_guard faultLock(faultMutex_);
            if (q.seq <= faultsClearedTo_) continue;  // a RUN after it cleared it already
        }
        // A task of an earlier configuration (since replaced) has no say; the first fault is the one shown.
        // Recorded even if STOP came first: RUN must then restart warm and clear the FAULT
        if (q.generation != generation_ || fault_) continue;
        fault_ = f;
        // One fault, one event: the FAULT is the mode change, so no "STOP" is recorded on the way
        if (running_) stopProgramsLocked();
        log_.event(log::Level::Error, "mode", log::Author::system(), "FAULT in task '{}': {} — {} ({}:{}, cycle {})", f.task, f.code,
                   f.message, f.file, f.line, f.cycle);
        if (modbus_) modbus_->setIdentity(identity());
    }
}

std::optional<runtime::FaultInfo> Instance::fault() const {
    std::lock_guard lock(mutex_);
    return fault_;
}

std::shared_ptr<runtime::Task> Instance::task(const std::string& name) const {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < tasks_.size() && i < config_.tasks.size(); i++) {
        if (config_.tasks[i].name == name) return tasks_[i];
    }
    return nullptr;
}

void Instance::writeSubstituteValues() {
    // STOP or FAULT: tags with a substitute value get it (written as "substitute", not by the program), a force
    // released from now on gives its tag the substitute value, and the links send them to the devices
    if (image_) image_->enterSafeState();
    outputs_->substitute();
}

std::string Instance::id() const {
    std::lock_guard lock(mutex_);
    return config_.id;
}

PlcConfig Instance::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

std::shared_ptr<ProcessImage> Instance::image() const {
    std::lock_guard lock(mutex_);
    return image_;
}

modbus::DeviceIdentity Instance::identity() const {
    modbus::DeviceIdentity id;
    id.serverId = static_cast<uint8_t>(config_.device.unitId);
    id.running = running_;
    id.vendorName = config_.device.vendor;
    id.productCode = config_.device.productCode;
    id.revision = kVersion;
    id.productName = config_.name;
    id.modelName = config_.device.model;
    id.applicationName = config_.tasks.empty() ? std::string() : config_.tasks.front().program;
    return id;
}

void Instance::open(bool runProgram, const log::Author& by) {
    std::lock_guard order(programsMutex_);  // loading the programs: not in the middle of a save
    std::lock_guard lock(mutex_);
    openLocked(runProgram, by);
}

std::optional<std::string> Instance::commandedMode(const std::filesystem::path& dir) {
    auto text = runtime::readFile(dir / "mode");
    if (!text) return std::nullopt;
    std::string mode = text->substr(0, text->find_first_of(" \r\n"));
    if (mode != "run" && mode != "stop") return std::nullopt;  // not ours: as if nothing were recorded
    return mode;
}

void Instance::recordModeLocked(bool run) {
    // After a power cut the service restarts the process, and the PLC must come back in the mode an operator
    // (or an agent) left it in: a STOP for maintenance must not end in RUN. Only their commands are recorded;
    // a fault, a reconfiguration or the shutdown itself stop the programs without changing what was commanded
    if (auto ok = runtime::writeFileAtomic(dir_ / "mode", run ? "run\n" : "stop\n"); !ok) {
        log_.warn("the commanded mode could not be recorded: {} (a restart of the process starts the PLC as before)", ok.error());
    }
}

void Instance::close() {
    RetiredLinks retired;  // destroyed after the lock is released: their final writes wait for the devices
    std::lock_guard lock(mutex_);
    closeLocked(retired, true);
}

// ───────────────────────────── retentive memory ─────────────────────────────

namespace {

using json = nlohmann::json;
constexpr const char* kRetainFile = "retain.json";

/// Up to five names for a log line, then how many more.
std::string someOf(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < 5; i++) out += (i ? ", " : "") + names[i];
    if (names.size() > 5) out += std::format(" and {} more", names.size() - 5);
    return out;
}

/// The retentive memory as the file holds it: `retain` tags as JSON values, RETAIN variables as ST literals
/// by task (the PLC's time is added when it is written). None when a running task delivered no snapshot in
/// time: nothing is written then, the next second tries again.
std::optional<json> retainedContent(const ProcessImage& image, const std::vector<std::shared_ptr<runtime::Task>>& tasks,
                                    const std::vector<std::string>& names) {
    json out = {{"tags", json::object()}, {"variables", json::object()}};
    for (const TagDef& t : image.tags()) {
        if (!t.retain) continue;
        double v = image.readTag(t);
        out["tags"][t.name] = t.type == TagType::Bool ? json(v != 0) : json(v);
    }
    for (size_t i = 0; i < tasks.size() && i < names.size(); i++) {
        if (!tasks[i]->image()) continue;  // no program
        runtime::Task::Snapshot snap = tasks[i]->snapshot();
        if (!snap.image || !snap.data) return std::nullopt;
        json vars = json::object();
        for (const vm::VarEntry& v : snap.image->vars) {
            if ((v.flags & vm::kVarRetain) && snap.data->size() >= v.offset + v.size) {
                vars[v.path] = st::variableText(*snap.image, v, snap.data->data());
            }
        }
        out["variables"][names[i]] = std::move(vars);
    }
    return out;
}

constexpr const char* kPlcTimeKey = "plc_time_ns";

/// What retain.json holds; an empty object when there is none, or when it is damaged (said in the log).
json readRetained(const std::filesystem::path& dir, log::Scope& log) {
    auto text = runtime::readFile(dir / kRetainFile);
    if (!text) return json::object();
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log.warn("retentive memory: {} is damaged; tags and variables start from their initial values", (dir / kRetainFile).string());
        return json::object();
    }
    return j;
}

/// Gives the `retain` tags the values the file holds. Returns how many; what it skipped goes to `skipped`
/// with the reason.
size_t restoreTags(ProcessImage& image, const json& stored, std::vector<std::string>& skipped) {
    size_t n = 0;
    if (!stored.is_object()) return n;
    std::vector<TagDef> tags = image.tags();
    for (const auto& [name, value] : stored.items()) {
        auto it = std::find_if(tags.begin(), tags.end(), [&](const TagDef& t) { return t.name == name; });
        if (it == tags.end() || !it->retain) {
            skipped.push_back(std::format("tag {} (not a retentive tag any more)", name));
        } else if (!value.is_number() && !value.is_boolean()) {
            skipped.push_back(std::format("tag {} (not a value)", name));
        } else if (auto ok = image.writeTag(*it, value.is_boolean() ? (value.get<bool>() ? 1.0 : 0.0) : value.get<double>(),
                                            Origin::Retain); !ok) {
            skipped.push_back(std::format("tag {} ({})", name, ok.error()));
        } else {
            n++;
        }
    }
    return n;
}

/// Gives the RETAIN variables of each task's program the values the file holds (ST literals by path). The
/// tasks are not running yet: the values go straight into the programs' data.
size_t restoreVariables(const std::vector<std::shared_ptr<runtime::Task>>& tasks, const std::vector<std::string>& names,
                        const json& stored, std::vector<std::string>& skipped) {
    size_t n = 0;
    if (!stored.is_object()) return n;
    for (size_t i = 0; i < tasks.size() && i < names.size(); i++) {
        auto it = stored.find(names[i]);
        if (it == stored.end() || !it->is_object()) continue;
        std::shared_ptr<const vm::Image> image = tasks[i]->image();
        for (const auto& [path, text] : it->items()) {
            const vm::VarEntry* var = nullptr;
            if (image) {
                for (const vm::VarEntry& v : image->vars) {
                    if (v.path == path && (v.flags & vm::kVarRetain)) var = &v;
                }
            }
            if (!var || !text.is_string()) {
                skipped.push_back(std::format("{} (task '{}' has no RETAIN variable of that name)", path, names[i]));
                continue;
            }
            auto bytes = st::literalBytes(*image, *var, text.get<std::string>());
            if (!bytes) {
                skipped.push_back(std::format("{} ({})", path, bytes.error()));
                continue;
            }
            if (auto ok = tasks[i]->write(path, std::move(*bytes)); !ok) {
                skipped.push_back(std::format("{} ({})", path, ok.error()));
                continue;
            }
            n++;
        }
    }
    return n;
}

}  // namespace

std::vector<std::string> Instance::taskNamesLocked() const {
    std::vector<std::string> names;
    for (const TaskConfig& tc : config_.tasks) names.push_back(tc.name);
    return names;
}

void Instance::saveRetained(bool now) {
    std::shared_ptr<ProcessImage> image;
    std::vector<std::shared_ptr<runtime::Task>> tasks;
    std::vector<std::string> names;
    {
        std::lock_guard lock(mutex_);
        if (!open_ || (!now && std::chrono::steady_clock::now() - retainSavedAt_ < std::chrono::seconds(1))) return;
        image = image_;
        tasks = tasks_;
        names = taskNamesLocked();
    }
    std::optional<json> content = retainedContent(*image, tasks, names);  // waits for a cycle of each task: unlocked
    std::lock_guard lock(mutex_);
    if (content && open_ && image == image_) writeRetainedLocked(*content, false);  // not after a cold restart meanwhile
}

void Instance::writeRetainedLocked(json content, bool always) {
    retainSavedAt_ = std::chrono::steady_clock::now();
    // Compared without the time, which always moves: values that did not change are not written again
    std::string text = content.dump(-1, ' ', false, json::error_handler_t::replace);
    if (text == retainWritten_ && !always) return;
    // The PLC's time goes with the values: timers in RETAIN instances go on from it after a power cycle
    content[kPlcTimeKey] = std::chrono::duration_cast<std::chrono::nanoseconds>(retainSavedAt_ - timeOrigin_).count();
    if (auto ok = runtime::writeFileAtomic(dir_ / kRetainFile, content.dump(-1, ' ', false, json::error_handler_t::replace))) {
        retainWritten_ = text;
        retainError_.clear();
    } else if (ok.error() != retainError_) {
        retainError_ = ok.error();
        log_.warn("retentive memory: {}", retainError_);
    }
}

void Instance::clearRetained() {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::remove(dir_ / kRetainFile, ec);
    retainWritten_.clear();
}

void Instance::openLocked(bool runProgram, const log::Author& by) {
    if (open_) return;
    if (by.kind != "system") recordModeLocked(runProgram);  // a cold restart or a setup someone asked for
    image_ = std::make_shared<ProcessImage>(static_cast<uint32_t>(config_.imageSize));
    for (const auto& problem : image_->setTags(config_.tags)) log_.warn("{}", problem);
    // Retentive memory, as it was when the power went: the tags now, before STOP writes the substitute values
    // and before the links send anything; the programs' RETAIN variables once the programs are loaded
    json retained = readRetained(dir_, log_);
    // The PLC's time continues from what retentive memory kept (it stands still while the PLC is off), so a
    // timer in a RETAIN instance goes on where it stopped; without retentive memory it starts at 0
    int64_t keptNs = 0;
    if (auto it = retained.find(kPlcTimeKey); it != retained.end() && it->is_number_integer() && it->get<int64_t>() > 0) {
        keptNs = it->get<int64_t>();
    }
    timeOrigin_ = std::chrono::steady_clock::now() - std::chrono::nanoseconds(keptNs);
    std::vector<std::string> skipped;
    size_t tagsRestored = retained.contains("tags") ? restoreTags(*image_, retained["tags"], skipped) : 0;
    commWarnings_.clear();
    programErrors_.clear();
    fault_.reset();
    if (config_.realtime) {
        if (auto locked = runtime::lockMemory(); !locked) {
            commWarnings_.push_back("real-time mode: memory could not be locked (" + locked.error() + ")");
            log_.warn("real-time mode: memory could not be locked ({})", locked.error());
        }
    }
    // Before the links start: a PLC that powers up in STOP holds its substitute values, like after a STOP, and
    // one that starts in RUN sends nothing until its programs have computed the outputs
    if (runProgram) {
        outputs_->hold();
    } else {
        image_->enterSafeState();
        outputs_->substitute();
    }
    startModbus();
    startOpcUa();
    RetiredLinks none;  // closed before: no links yet
    startLinks(none, true);
    createTasks();
    loadPrograms();
    size_t varsRestored = retained.contains("variables") ? restoreVariables(tasks_, taskNamesLocked(), retained["variables"], skipped) : 0;
    if (tagsRestored || varsRestored || !skipped.empty()) {
        log_.info("retentive memory: {} tag{} and {} variable{} restored", tagsRestored, tagsRestored == 1 ? "" : "s",
                  varsRestored, varsRestored == 1 ? "" : "s");
    }
    if (!skipped.empty()) log_.warn("retentive memory: not restored: {}", someOf(skipped));
    retainWritten_.clear();  // the next save writes what this configuration keeps; entries of what is gone drop out
    open_ = true;
    if (runProgram) runLocked();
}

void Instance::closeLocked(RetiredLinks& retired, bool flush) {
    if (!open_) return;
    // Retentive memory as it is now, before STOP writes the substitute values
    if (auto content = retainedContent(*image_, tasks_, taskNamesLocked())) writeRetainedLocked(std::move(*content), true);
    auto start = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        auto now = std::chrono::steady_clock::now();
        log_.debug("{} stopped in {} ms", what, std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        start = now;
    };
    if (!flush) {
        // A restart (new image size or id): the links stop before STOP writes the substitute values, so the
        // devices keep the outputs until the restarted programs compute them again
        for (auto& link : links_) link->requestStop(false);
    }
    haltLocked();
    tasks_.clear();  // tasks and servers reference the image: release them first
    lap("programs");
    // Shutdown: every link sends the substitute values once more (all of them at once, each within its
    // timeout) and then stops; they are joined after the instance lock is released
    for (auto& link : links_) {
        if (flush) link->requestStop(true);
        retired.push_back(std::move(link));
    }
    links_.clear();
    modbus_.reset();
    lap("Modbus");
    opcua_.reset();
    lap("OPC UA");
    modbusStatus_.listening = opcuaStatus_.listening = false;
    open_ = false;
}

int Instance::commPriority() const { return commPriorityOf(config_); }

int Instance::commPriorityOf(const PlcConfig& config) {
    // Communication runs just below the lowest real-time task: it feeds the tasks, so it must not
    // wait behind ordinary threads, and it must not delay a task either
    if (!config.realtime) return 0;
    int lowest = 0;
    for (const TaskConfig& t : config.tasks) {
        if (t.priority > 0 && (lowest == 0 || t.priority < lowest)) lowest = t.priority;
    }
    return lowest > 1 ? lowest - 1 : lowest;
}

void Instance::startLinks(RetiredLinks& retired, bool restartAll) {
    // A link whose configuration did not change keeps running: editing one link restarts only that one
    std::vector<std::unique_ptr<modbus::LinkPoller>> next(config_.links.size());
    for (size_t i = 0; i < config_.links.size() && !restartAll; i++) {
        for (auto& old : links_) {
            if (old && old->config() == config_.links[i]) {
                next[i] = std::move(old);
                break;
            }
        }
    }
    // The others stop first (without waiting here), then their successors start
    for (auto& old : links_) {
        if (!old) continue;
        const std::string& status = old->config().statusTag;
        bool successor = !status.empty() && std::any_of(config_.links.begin(), config_.links.end(),
                                                        [&](const LinkConfig& l) { return l.statusTag == status; });
        old->requestStop(false, !successor);  // a successor writes the status tag: the old link leaves it alone
        retired.push_back(std::move(old));
    }
    for (size_t i = 0; i < next.size(); i++) {
        if (next[i]) continue;
        next[i] = std::make_unique<modbus::LinkPoller>(config_.links[i], image_, config_.id, commPriority(),
                                                       [gate = outputs_] { return gate->state(); });
        next[i]->start();
    }
    links_ = std::move(next);
}

std::vector<modbus::LinkStatus> Instance::modbusLinks() const {
    std::lock_guard lock(mutex_);
    std::vector<modbus::LinkStatus> out;
    for (const auto& link : links_) out.push_back(link->status());
    return out;
}

std::string Instance::writerConflictLocked(const vm::Image& image, size_t task, const std::vector<LinkConfig>& links) const {
    // One writer per tag: two writers would overwrite each other in an order nobody chose
    for (const vm::Binding& b : image.bindings) {
        if (b.readOnly) continue;
        for (size_t j = 0; j < tasks_.size(); j++) {
            if (j == task) continue;
            auto other = tasks_[j]->latestImage();  // a program waiting for its cycle boundary counts too
            if (!other) continue;
            for (const vm::Binding& ob : other->bindings) {
                if (ob.tag == b.tag && !ob.readOnly) {
                    return std::format("tag '{}' is written by task '{}' (program '{}') too; a tag has one writer: "
                                       "remove the write from one program (or declare it VAR_EXTERNAL CONSTANT there)",
                                       b.tag, config_.tasks[j].name, other->program);
                }
            }
        }
        if (std::string c = linkConflict(b.tag, links); !c.empty()) return c;
    }
    return {};
}

std::vector<std::string> Instance::linkConflicts(const PlcConfig& next) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> problems;
    for (size_t i = 0; i < tasks_.size(); i++) {
        auto image = tasks_[i]->latestImage();
        if (!image) continue;
        for (const vm::Binding& b : image->bindings) {
            if (b.readOnly) continue;
            if (std::string c = linkConflict(b.tag, next.links); !c.empty()) {
                problems.push_back(std::format("task '{}' (program '{}'): {}", config_.tasks[i].name, image->program, c));
            }
        }
    }
    return problems;
}

void Instance::startModbus() {
    modbusStatus_ = {config_.modbus.enabled, false, config_.modbus.port, 0, {}};
    if (!config_.modbus.enabled) return;
    modbus_ = std::make_unique<modbus::TcpServer>(*image_, log::Scope("modbus", config_.id));
    modbus_->setIdentity(identity());

    modbus::TcpServer::Options options;
    options.listen = config_.modbus.listen;
    options.maxConnections = config_.modbus.maxConnections;
    options.idleTimeoutMs = config_.modbus.idleTimeoutS * 1000;
    options.realtimePriority = commPriority();
    std::string warning;
    auto port = startWithFallback(
        config_.modbus.port, 5020,
        [&](uint16_t p) {
            options.port = p;
            return modbus_->start(options);
        },
        warning, "Modbus");
    if (!port) {
        modbusStatus_.error = port.error().message;
        commWarnings_.push_back("Modbus is not available: " + port.error().message);
        log_.warn("Modbus is not available: {}", port.error().message);
        modbus_.reset();
        return;
    }
    if (!warning.empty()) {
        commWarnings_.push_back(warning);
        log_.warn("{}", warning);
    }
    modbusStatus_.listening = true;
    modbusStatus_.port = *port;
}

void Instance::startOpcUa() {
    opcuaStatus_ = {config_.opcua.enabled, false, config_.opcua.port, 0, {}};
    if (!config_.opcua.enabled) return;
    opcua_ = std::make_unique<opcua::Server>(*image_, log::Scope("opcua", config_.id));

    opcua::Server::Options options;
    options.listen = config_.opcua.listen;
    options.plcId = config_.id;
    options.name = config_.name;
    options.applicationUri = "urn:simpleplc:" + config_.id;
    std::string warning;
    auto port = startWithFallback(
        config_.opcua.port, 4841,
        [&](uint16_t p) {
            options.port = p;
            return opcua_->start(options);
        },
        warning, "OPC UA");
    if (!port) {
        opcuaStatus_.error = port.error().message;
        commWarnings_.push_back("OPC UA is not available: " + port.error().message);
        log_.warn("OPC UA is not available: {}", port.error().message);
        opcua_.reset();
        return;
    }
    if (!warning.empty()) {
        commWarnings_.push_back(warning);
        log_.warn("{}", warning);
    }
    opcuaStatus_.listening = true;
    opcuaStatus_.port = *port;
}

std::shared_ptr<runtime::Task> Instance::makeTask(const TaskConfig& tc) {
    runtime::TaskOptions options;
    options.plcId = config_.id;
    options.name = tc.name;
    options.cycleMs = tc.cycleMs;
    options.priority = tc.priority;
    options.realtime = config_.realtime;
    options.watchdog = std::chrono::milliseconds(tc.watchdogMs);
    options.timeOrigin = timeOrigin_;
    std::shared_ptr<ProcessImage> image = image_;
    uint64_t generation = generation_;  // this task set's
    return std::make_shared<runtime::Task>(options, image_, [this, image, generation](const runtime::FaultInfo& f) {
        image->enterFault();  // safe outputs now, on this thread: no lock but the image's
        onTaskFault(f, generation);
    });
}

void Instance::createTasks() {
    tasks_.clear();
    generation_++;  // faults of the tasks just destroyed no longer count
    for (const TaskConfig& tc : config_.tasks) tasks_.push_back(makeTask(tc));
}

std::vector<TagDef> Instance::currentTagsLocked() const { return image_ ? image_->tags() : config_.tags; }

bool Instance::runByTaskLocked(const std::string& program) const {
    return std::any_of(config_.tasks.begin(), config_.tasks.end(),
                       [&](const TaskConfig& tc) { return runtime::ProgramStore::sameName(tc.program, program); });
}

std::vector<runtime::StoredFile> Instance::savedFiles() const {
    // No Instance state but the program store, which is thread-safe: callers compile without the lock
    std::vector<runtime::StoredFile> out;
    for (const runtime::ProgramInfo& info : programs_.list()) {
        if (auto text = programs_.read(info.name)) out.push_back({info.name, *text});
    }
    return out;
}

std::vector<runtime::StoredFile> Instance::selectFiles(const std::string& program, const std::optional<std::string>& source,
                                                      std::vector<runtime::StoredFile> base, std::vector<std::string>* skipped) {
    // The program's file first; every other file is a library, and the ones it uses go with it
    std::optional<std::string> own = source;
    std::vector<st::SourceFile> files{{program + ".st", ""}};
    for (runtime::StoredFile& f : base) {
        if (runtime::ProgramStore::sameName(f.name, program)) {
            if (!own) own = std::move(f.text);
            continue;
        }
        files.push_back({f.name + ".st", std::move(f.text)});
    }
    if (!own) return {};
    files[0].text = std::move(*own);
    std::vector<runtime::StoredFile> out;
    for (st::SourceFile& f : st::usedFiles(files)) {
        std::string name = f.name.substr(0, f.name.size() - 3);
        // A library it needs that does not even parse (an edit in progress) is left out with a warning
        if (!out.empty() && !st::outline(f.text).parses) {
            if (skipped) skipped->push_back(name);
            continue;
        }
        out.push_back({std::move(name), std::move(f.text)});
    }
    return out;
}

st::CompileResult Instance::compileFiles(const std::vector<runtime::StoredFile>& files, const std::vector<TagDef>& tags,
                                         st::CompileMode mode, const std::vector<std::string>& skipped) {
    std::vector<st::SourceFile> sources;
    for (const runtime::StoredFile& f : files) sources.push_back({f.name + ".st", f.text});
    st::CompileResult r = st::compile(sources, tags, mode);
    for (const std::string& f : skipped) {
        r.fileNames.push_back(f + ".st");
        r.diagnostics.push_back({"W_LIBRARY", st::Severity::Warning, {static_cast<uint16_t>(r.fileNames.size() - 1), 1, 1},
                                 std::format("{}.st has syntax errors: its declarations are not available here", f),
                                 "fix that file; this program uses names it declares"});
    }
    return r;
}

st::CompileResult Instance::checkProgram(const std::string& program, const std::string& source,
                                         const std::optional<std::vector<TagDef>>& tags) const {
    // Compiled without the Instance lock: a long compile must not delay a fault's reaction or a STOP
    std::string name = runtime::ProgramStore::isValidName(program) ? programs_.canonical(program) : program;
    std::vector<TagDef> against;
    bool runByTask = false;
    {
        std::lock_guard lock(mutex_);
        against = tags ? *tags : currentTagsLocked();
        runByTask = runByTaskLocked(name);
    }
    // A file no task runs and without a PROGRAM is a library: checked as one
    st::CompileMode mode = runByTask || st::outline(source).hasProgram ? st::CompileMode::Program : st::CompileMode::Library;
    std::vector<std::string> skipped;
    st::CompileResult r = compileFiles(selectFiles(name, source, savedFiles(), &skipped), against, mode, skipped);
    std::lock_guard lock(mutex_);
    // What a save would refuse beyond compiling: a tag that another task or a link writes too
    if (r.ok && r.image && !tags) {
        for (size_t i = 0; i < tasks_.size() && i < config_.tasks.size(); i++) {
            if (!runtime::ProgramStore::sameName(config_.tasks[i].program, name)) continue;
            if (std::string conflict = writerConflictLocked(*r.image, i, config_.links); !conflict.empty()) {
                r.diagnostics.push_back({"E_BINDING", st::Severity::Error, {0, 1, 1},
                                         std::format("task '{}' would not load it: {}", config_.tasks[i].name, conflict), ""});
                r.ok = false;
                break;
            }
        }
    }
    return r;
}

std::optional<Instance::OnlineChangePreview> Instance::previewOnlineChange(const std::string& program, const vm::Image& next) const {
    std::string name = runtime::ProgramStore::isValidName(program) ? programs_.canonical(program) : program;
    OnlineChangePreview out;
    std::shared_ptr<const vm::Image> base;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < tasks_.size() && i < config_.tasks.size(); i++) {
            if (!runtime::ProgramStore::sameName(config_.tasks[i].program, name)) continue;
            out.tasks.push_back(config_.tasks[i].name);
            if (!base) base = tasks_[i]->latestImage();  // the program it follows: a pending one, or the running one
        }
    }
    if (out.tasks.empty()) return std::nullopt;
    // The load's own rule (runtime::Task::onlineChangeOutcome), so the preview says what the load then reports
    runtime::Task::onlineChangeOutcome(base.get(), next, out.reset, &out.kept);
    return out;
}

std::vector<st::SourceFile> Instance::programFiles(const std::string& program, const std::optional<std::string>& source,
                                                   std::vector<std::string>* skipped) const {
    std::string name = runtime::ProgramStore::isValidName(program) ? programs_.canonical(program) : program;
    std::vector<st::SourceFile> out;
    for (runtime::StoredFile& f : selectFiles(name, source, savedFiles(), skipped)) out.push_back({f.name + ".st", std::move(f.text)});
    return out;
}

std::optional<st::CompileResult> Instance::compiled(const std::string& program) const {
    std::string name = programs_.canonical(program);
    std::lock_guard lock(mutex_);
    for (const auto& [p, d] : loaded_) {
        if (runtime::ProgramStore::sameName(p, name)) return d.compiled;
    }
    for (const auto& [p, r] : attempts_) {
        if (runtime::ProgramStore::sameName(p, name)) return r;
    }
    return std::nullopt;
}

std::optional<std::vector<runtime::StoredFile>> Instance::deployedFiles(const std::string& program) const {
    std::string name = programs_.canonical(program);
    {
        std::lock_guard lock(mutex_);
        for (const auto& [p, d] : loaded_) {
            if (runtime::ProgramStore::sameName(p, name)) return d.files;
        }
    }
    if (auto set = programs_.deployed(name)) return set->files;
    return std::nullopt;
}

std::string Instance::mainProgram() const {
    std::string program;
    {
        std::lock_guard lock(mutex_);
        program = config_.tasks.empty() ? std::string("main") : config_.tasks.front().program;
    }
    return programs_.canonical(program);
}

ProgramLoad Instance::installLocked(const std::string& program, st::CompileResult compiled,
                                    const std::vector<runtime::StoredFile>& files, bool checkWriters) {
    ProgramLoad out;
    out.program = program;
    out.compiled = std::move(compiled);
    out.ok = out.compiled.ok && out.compiled.image;
    attempts_[program] = out.compiled;
    std::vector<size_t> targets;
    for (size_t i = 0; i < tasks_.size() && i < config_.tasks.size(); i++) {
        if (!runtime::ProgramStore::sameName(config_.tasks[i].program, program)) continue;
        out.tasks.push_back(config_.tasks[i].name);
        targets.push_back(i);
    }
    if (!out.ok) {
        std::string first;
        for (const st::Diagnostic& d : out.compiled.diagnostics) {
            if (d.severity == st::Severity::Error) {
                first = st::formatDiagnostic(d, out.compiled.fileNames);
                break;
            }
        }
        for (size_t i : targets) {
            programErrors_[config_.tasks[i].name] =
                tasks_[i]->image() ? std::format("program '{}' failed to compile; the task runs the version loaded before: {}", program, first)
                                   : std::format("program '{}' failed to compile: {}", program, first);
        }
        return out;
    }
    // One writer per tag, checked for every task first: the program loads into all of them or none
    for (size_t i : checkWriters ? targets : std::vector<size_t>{}) {
        if (std::string conflict = writerConflictLocked(*out.compiled.image, i, config_.links); !conflict.empty()) {
            out.taskErrors[config_.tasks[i].name] = conflict;
            programErrors_[config_.tasks[i].name] = std::format("program '{}' was not loaded: {}", program, conflict);
        }
    }
    if (!out.taskErrors.empty()) {
        out.ok = false;
        return out;
    }
    for (size_t i : targets) {
        const TaskConfig& tc = config_.tasks[i];
        runtime::Task::LoadOutcome r = tasks_[i]->load(out.compiled.image, program);
        out.reset.insert(out.reset.end(), r.reset.begin(), r.reset.end());
        out.replaced = out.replaced || r.replaced;
        programErrors_.erase(tc.name);
        // A task that had no program (it did not compile, or was refused) starts with this one; not
        // in FAULT, where RUN restarts every task
        if (running_ && !fault_ && !image_->faulted() && tc.enabled &&
            !tasks_[i]->running() && !tasks_[i]->stats().fault) {
            tasks_[i]->start();
        }
        // The audit trail of deployments
        log_.info("task '{}' runs program '{}' ({}{})", tc.name, program, r.replaced ? "online change" : "loaded",
                  r.reset.empty() || !r.replaced ? ""
                                   : std::format(", {} variable{} from initial values", r.reset.size(), r.reset.size() == 1 ? "" : "s"));
    }
    if (!targets.empty()) {
        loaded_[program] = {files, out.compiled};
        // What a restart runs: exactly these files, whatever is saved later
        if (auto ok = programs_.markDeployed(program, files); !ok) log_.warn("program '{}': {}", program, ok.error());
    }
    return out;
}

void Instance::loadPrograms() {
    // Power-up: every program runs the files it last loaded with. A draft saved since (refused, or
    // saved without deploying) does not run until it is saved and loads
    loaded_.clear();
    std::vector<TagDef> tags = currentTagsLocked();
    std::vector<std::string> done;
    for (const TaskConfig& tc : config_.tasks) {
        if (std::any_of(done.begin(), done.end(), [&](const std::string& d) { return runtime::ProgramStore::sameName(d, tc.program); })) continue;
        std::string name = programs_.canonical(tc.program);
        done.push_back(name);
        auto deployed = programs_.deployed(name);
        auto saved = programs_.read(name);
        auto warnTasks = [&] {
            for (const TaskConfig& t : config_.tasks) {
                auto it = programErrors_.find(t.name);
                if (runtime::ProgramStore::sameName(t.program, name) && it != programErrors_.end()) log_.warn("task '{}': {}", t.name, it->second);
            }
        };
        if (deployed && !deployed->legacy) {
            ProgramLoad load = installLocked(name, compileFiles(deployed->files, tags), deployed->files);
            if (load.ok) {
                if (!saved || *saved != deployed->files.front().text) {
                    log_.info("program '{}' runs the version deployed last; the saved file differs (a draft: save it to load it)", name);
                }
                continue;
            }
            // It no longer compiles (tags changed while the PLC was off, another compiler): the saved files then
        }
        if (!saved) {
            for (const TaskConfig& t : config_.tasks) {
                if (runtime::ProgramStore::sameName(t.program, name)) programErrors_[t.name] = std::format("program '{}' does not exist yet", tc.program);
            }
            warnTasks();
            continue;
        }
        std::vector<std::string> skipped;
        std::vector<runtime::StoredFile> files = selectFiles(name, std::nullopt, savedFiles(), &skipped);
        ProgramLoad load = installLocked(name, compileFiles(files, tags, st::CompileMode::Program, skipped), files);
        if (!load.ok && deployed && deployed->legacy && deployed->files.front().text != *saved) {
            // An older version kept the program's own file only: that one, with today's libraries
            std::map<std::string, std::string> why = programErrors_;
            st::CompileResult attempt = attempts_[name];
            std::vector<std::string> skippedLegacy;
            std::vector<runtime::StoredFile> legacy = selectFiles(name, deployed->files.front().text, savedFiles(), &skippedLegacy);
            ProgramLoad fallback = installLocked(name, compileFiles(legacy, tags, st::CompileMode::Program, skippedLegacy), legacy);
            attempts_[name] = attempt;
            if (fallback.ok) {
                for (const std::string& t : fallback.tasks) {
                    programErrors_[t] = std::format("runs the version of '{}' that last loaded; the saved one does not: {}",
                                                    name, why.count(t) ? why[t] : "see its diagnostics");
                }
            }
        }
        warnTasks();
    }
}

SaveOutcome Instance::saveProgram(const std::string& requested, const std::string& source, bool deploy,
                                  const std::optional<std::string>& ifMatch) {
    SaveOutcome out;
    std::lock_guard order(programsMutex_);  // saves apply one at a time, in the order they came
    bool valid = runtime::ProgramStore::isValidName(requested);
    out.program = valid ? programs_.canonical(requested) : requested;
    out.deployed = deploy;
    out.own.program = out.program;
    if (ifMatch && valid) {
        // Optimistic concurrency: only over the version the caller read
        auto current = programs_.read(out.program);
        std::string hash = current ? crypto::toHex(crypto::sha256(*current)) : std::string();
        std::string want = *ifMatch;
        std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!current || hash != want) {
            out.conflictHash = hash;
            return out;
        }
    }
    if (auto ok = programs_.write(out.program, source); !ok) {
        out.error = ok.error();
        return out;
    }
    out.saved = true;
    if (!deploy) return out;

    // Compiled without the Instance lock (see checkProgram), installed with it
    std::vector<TagDef> tags;
    bool runByTask = false;
    std::map<std::string, std::vector<runtime::StoredFile>> running;  // program -> the files its tasks run
    {
        std::lock_guard lock(mutex_);
        tags = currentTagsLocked();
        runByTask = runByTaskLocked(out.program);
        for (const auto& [p, d] : loaded_) running[p] = d.files;
    }
    std::vector<runtime::StoredFile> saved = savedFiles();
    st::CompileMode mode = runByTask || st::outline(source).hasProgram ? st::CompileMode::Program : st::CompileMode::Library;
    out.library = mode == st::CompileMode::Library;
    std::vector<std::string> skipped;
    std::vector<runtime::StoredFile> files = selectFiles(out.program, source, saved, &skipped);
    st::CompileResult own = compileFiles(files, tags, mode, skipped);

    // The running programs that use this file: each with the files it runs (its own deployed version, not a
    // draft of it), this file's new text, and the saved files for anything the new text needs besides
    struct Dependent {
        std::string program;
        std::vector<runtime::StoredFile> files;
        st::CompileResult compiled;
    };
    std::vector<Dependent> dependents;
    std::vector<std::string> settled;
    if (own.ok) {
        for (const auto& [program, deployedFiles] : running) {
            if (runtime::ProgramStore::sameName(program, out.program)) continue;
            std::vector<runtime::StoredFile> base = saved;
            auto put = [&](const runtime::StoredFile& f) {
                auto it = std::find_if(base.begin(), base.end(), [&](const runtime::StoredFile& b) { return runtime::ProgramStore::sameName(b.name, f.name); });
                if (it == base.end()) base.push_back(f);
                else *it = f;
            };
            for (const runtime::StoredFile& f : deployedFiles) put(f);
            put({out.program, source});
            std::vector<std::string> skippedHere;
            std::vector<runtime::StoredFile> next = selectFiles(program, std::nullopt, std::move(base), &skippedHere);
            // It uses the file when the file is among those it needs, now or before (then its new text may lack a name)
            auto hasFile = [&](const std::vector<runtime::StoredFile>& set) {
                return std::any_of(set.begin() + 1, set.end(), [&](const runtime::StoredFile& f) { return runtime::ProgramStore::sameName(f.name, out.program); });
            };
            if (!hasFile(next) && !hasFile(deployedFiles)) continue;
            if (sameFiles(next, deployedFiles)) {
                settled.push_back(program);  // the saved files fit what it runs again
                continue;
            }
            st::CompileResult r = compileFiles(next, tags, st::CompileMode::Program, skippedHere);
            dependents.push_back({program, std::move(next), std::move(r)});
        }
    }

    std::lock_guard lock(mutex_);
    // Install only what the file still holds: a later save (or an edit on disk) wins
    if (auto now = programs_.read(out.program); !now || *now != source) {
        out.own.compiled = std::move(own);
        out.own.error = "the file changed while this version compiled (it was saved again); this version was not loaded";
        return out;
    }
    if (mode == st::CompileMode::Program && runByTaskLocked(out.program)) {
        out.own = installLocked(out.program, std::move(own), files);
    } else {
        out.own.compiled = std::move(own);
        out.own.ok = out.own.compiled.ok;
        attempts_[out.program] = out.own.compiled;
        // A program no task runs yet: this is the version a task that is pointed at it will run
        if (out.own.ok && mode == st::CompileMode::Program) {
            if (auto ok = programs_.markDeployed(out.program, files); !ok) log_.warn("program '{}': {}", out.program, ok.error());
        }
    }
    if (!out.own.ok) return out;  // a draft: it reaches no other program either
    for (const std::string& program : settled) {
        for (const TaskConfig& t : config_.tasks) {
            auto it = programErrors_.find(t.name);
            if (it != programErrors_.end() && it->second.starts_with(std::format("program '{}' does not compile with the saved", program))) {
                programErrors_.erase(it);
            }
        }
    }
    for (Dependent& d : dependents) {
        if (d.compiled.ok) {
            out.dependents.push_back(installLocked(d.program, std::move(d.compiled), d.files));
            continue;
        }
        // It keeps the version it runs; the saved file and the program no longer fit together
        ProgramLoad failed;
        failed.program = d.program;
        failed.compiled = std::move(d.compiled);
        std::string first;
        for (const st::Diagnostic& diag : failed.compiled.diagnostics) {
            if (diag.severity == st::Severity::Error) {
                first = st::formatDiagnostic(diag, failed.compiled.fileNames);
                break;
            }
        }
        for (size_t i = 0; i < tasks_.size() && i < config_.tasks.size(); i++) {
            if (!runtime::ProgramStore::sameName(config_.tasks[i].program, d.program)) continue;
            failed.tasks.push_back(config_.tasks[i].name);
            programErrors_[config_.tasks[i].name] = std::format(
                "program '{}' does not compile with the saved {}.st, so it runs the version loaded before: {}", d.program, out.program, first);
        }
        out.dependents.push_back(std::move(failed));
    }
    return out;
}

std::expected<std::string, std::pair<int, std::string>> Instance::deleteProgram(const std::string& requested) {
    std::lock_guard order(programsMutex_);
    if (!runtime::ProgramStore::isValidName(requested)) return std::unexpected(std::pair(404, std::format("invalid program name '{}'", requested)));
    std::string name = programs_.canonical(requested);
    if (!programs_.exists(name)) return std::unexpected(std::pair(404, std::format("program '{}' does not exist", requested)));
    {
        std::lock_guard lock(mutex_);
        for (const TaskConfig& t : config_.tasks) {
            if (runtime::ProgramStore::sameName(t.program, name)) {
                return std::unexpected(std::pair(409, std::format("task '{}' runs program '{}'; change the task first", t.name, name)));
            }
        }
        for (const auto& [program, d] : loaded_) {
            for (size_t i = 1; i < d.files.size(); i++) {
                if (runtime::ProgramStore::sameName(d.files[i].name, name)) {
                    return std::unexpected(std::pair(409, std::format(
                        "program '{}' uses {}.st (its types, functions or blocks); remove that use and save '{}' first",
                        program, name, program)));
                }
            }
        }
    }
    if (auto ok = programs_.remove(name); !ok) return std::unexpected(std::pair(404, ok.error()));
    std::lock_guard lock(mutex_);
    attempts_.erase(name);
    log_.info("program '{}' deleted (its history keeps it)", name);
    return name;
}

void Instance::run(const log::Author& by) {
    std::lock_guard lock(mutex_);
    // Recorded whether or not it changes anything: a STOP in FAULT keeps the PLC stopped across a power cut
    if (by.kind != "system") recordModeLocked(true);
    runLocked(by);
}

void Instance::halt(const log::Author& by) {
    std::lock_guard lock(mutex_);
    if (by.kind != "system") recordModeLocked(false);
    haltLocked(by);
}

bool Instance::running() const {
    std::lock_guard lock(mutex_);
    return running_;
}

void Instance::runLocked(const log::Author& by) {
    if (!open_ || running_) return;
    if (fault_ || image_->faulted()) {
        // After a fault the programs start again, never from a half-run cycle: a warm restart,
        // so RETAIN variables keep their values and every other variable gets its initial value. Faults
        // still queued for the supervisor are part of the one being cleared (every task is stopped now)
        for (auto& task : tasks_) {
            task->warmReset();
            task->clearFault();
        }
        fault_.reset();
        {
            std::lock_guard faultLock(faultMutex_);
            faultsClearedTo_ = faultSeq_;
            faults_.clear();
        }
        image_->clearFault();
    }
    image_->leaveSafeState();
    std::vector<uint64_t> cyclesBefore(tasks_.size());
    for (size_t i = 0; i < tasks_.size(); i++) cyclesBefore[i] = tasks_[i]->stats().cycles;
    std::vector<std::pair<std::shared_ptr<runtime::Task>, uint64_t>> firstCycles;
    for (size_t i = 0; i < tasks_.size(); i++) {
        if (!config_.tasks[i].enabled) continue;
        tasks_[i]->start();
        if (tasks_[i]->running()) firstCycles.push_back({tasks_[i], cyclesBefore[i]});
    }
    // The links send the outputs once every task that runs has completed a cycle: before that the tags
    // hold what the programs have not computed yet (zeros at power-up)
    outputs_->run(std::move(firstCycles));
    running_ = true;
    if (modbus_) modbus_->setIdentity(identity());
    log_.event(log::Level::Info, "mode", by, "RUN");
}

void Instance::haltLocked(const log::Author& by) {
    if (!running_) return;
    stopProgramsLocked();
    log_.event(log::Level::Info, "mode", by, "STOP");
}

void Instance::stopProgramsLocked() {
    for (auto& task : tasks_) task->stop();
    writeSubstituteValues();
    running_ = false;
    if (modbus_) modbus_->setIdentity(identity());
}

std::expected<void, std::string> Instance::reconfigure(const PlcConfig& next, const ReconfigureOptions& options,
                                                       ReconfigureReport* report,
                                                       const std::function<std::expected<void, std::string>()>& commit) {
    RetiredLinks retired;  // declared first: joined after both locks are released, so nothing waits for a device
    ReconfigureReport local;
    ReconfigureReport& rep = report ? *report : local;
    std::lock_guard order(programsMutex_);  // no save comes between the checks below and the change
    PlcConfig previous;
    bool isOpen = false;
    struct Running {
        std::vector<runtime::StoredFile> files;
        std::shared_ptr<const vm::Image> image;
    };
    std::map<std::string, Running> running;
    {
        std::lock_guard lock(mutex_);
        previous = config_;
        isOpen = open_;
        for (const auto& [p, d] : loaded_) running[p] = {d.files, d.compiled.image};
    }
    auto findRunning = [&](const std::string& program) -> Running* {
        for (auto& [p, r] : running) {
            if (runtime::ProgramStore::sameName(p, program)) return &r;
        }
        return nullptr;
    };
    auto previousTask = [&](const std::string& name) -> const TaskConfig* {
        for (const TaskConfig& t : previous.tasks) {
            if (t.name == name) return &t;
        }
        return nullptr;
    };

    // Every program a task of `next` runs, compiled for `next` where something changed for it: the
    // programs that run now against new tags, and programs that tasks newly run
    struct Plan {
        std::vector<runtime::StoredFile> files;
        st::CompileResult compiled;
        bool compiledNow = false;
        std::shared_ptr<const vm::Image> image;  // what the tasks run after the change
    };
    std::map<std::string, Plan> plans;
    bool tagsForCompiler = !sameForCompiler(previous.tags, next.tags);
    bool reopen = next.imageSize != previous.imageSize || next.id != previous.id;
    if (isOpen) {
        for (const TaskConfig& tc : next.tasks) {
            std::string program = programs_.canonical(tc.program);
            if (plans.count(program)) continue;
            Plan& plan = plans[program];
            if (Running* r = findRunning(program)) {
                plan.files = r->files;
                plan.image = r->image;
                if (!tagsForCompiler) continue;
            } else if (auto set = programs_.deployed(program); set && !set->legacy) {
                plan.files = set->files;  // what last ran: a draft of it does not go live through a task change
            } else {
                plan.files = selectFiles(program, std::nullopt, savedFiles(), nullptr);
            }
            if (plan.files.empty()) {
                plan.compiled.diagnostics.push_back({"E_UNDECLARED", st::Severity::Error, {0, 1, 1},
                                                     std::format("program '{}' does not exist", tc.program), "save it first"});
                plan.compiled.fileNames = {program + ".st"};
            } else {
                plan.compiled = compileFiles(plan.files, next.tags);
            }
            plan.compiledNow = true;
            plan.image = plan.compiled.ok ? plan.compiled.image : nullptr;
        }
        // What does not compile: a running program broken by the tags (refused unless forced), or a
        // program a task would newly run (refused)
        for (const TaskConfig& tc : next.tasks) {
            std::string program = programs_.canonical(tc.program);
            Plan& plan = plans[program];
            if (!plan.compiledNow || plan.compiled.ok) continue;
            const TaskConfig* before = previousTask(tc.name);
            bool kept = before && runtime::ProgramStore::sameName(before->program, tc.program);
            if (kept && !findRunning(program)) continue;  // it did not load before either: not this change's doing
            ProgramProblem problem{tc.name, program, plan.compiled, ""};
            for (const st::Diagnostic& d : plan.compiled.diagnostics) {
                if (d.severity == st::Severity::Error) {
                    problem.error = st::formatDiagnostic(d, plan.compiled.fileNames);
                    break;
                }
            }
            (kept ? rep.tagBreaks : rep.taskProblems).push_back(std::move(problem));
        }
        // One writer per tag among the tasks that will run, for those whose program changes
        for (size_t i = 0; i < next.tasks.size(); i++) {
            const TaskConfig& tc = next.tasks[i];
            const TaskConfig* before = previousTask(tc.name);
            if (before && runtime::ProgramStore::sameName(before->program, tc.program) && !tagsForCompiler) continue;
            const Plan& plan = plans[programs_.canonical(tc.program)];
            if (!plan.image) continue;
            std::string conflict;
            for (const vm::Binding& b : plan.image->bindings) {
                if (b.readOnly || !conflict.empty()) continue;
                for (size_t j = 0; j < next.tasks.size() && conflict.empty(); j++) {
                    if (j == i) continue;
                    const Plan& other = plans[programs_.canonical(next.tasks[j].program)];
                    if (!other.image) continue;
                    for (const vm::Binding& ob : other.image->bindings) {
                        if (ob.tag == b.tag && !ob.readOnly) {
                            conflict = std::format("tag '{}' is written by task '{}' (program '{}') too; a tag has one writer: "
                                                   "remove the write from one program (or declare it VAR_EXTERNAL CONSTANT there)",
                                                   b.tag, next.tasks[j].name, other.image->program);
                            break;
                        }
                    }
                }
                if (conflict.empty()) conflict = linkConflict(b.tag, next.links);
            }
            if (!conflict.empty()) rep.taskProblems.push_back({tc.name, plan.image->program, {}, conflict});
        }
    }
    if (!rep.taskProblems.empty()) {
        const ProgramProblem& p = rep.taskProblems.front();
        return std::unexpected(std::format("task '{}' cannot run program '{}': {}", p.task, p.program, p.error));
    }
    if (!rep.tagBreaks.empty() && !options.force) {
        const ProgramProblem& p = rep.tagBreaks.front();
        return std::unexpected(std::format("the tag change would stop program '{}' (task '{}'): {}", p.program, p.task, p.error));
    }
    if (commit) {
        if (auto ok = commit(); !ok) return std::unexpected(ok.error());
    }

    std::lock_guard lock(mutex_);
    config_ = next;
    if (!open_) return {};

    // The id names the PLC in logs and in OPC UA; the image size shapes everything: start over
    if (reopen) {
        bool wasRunning = running_;
        config_ = previous;
        closeLocked(retired, false);
        config_ = next;
        log_ = log::Scope("plc", next.id);
        openLocked(wasRunning, log::Author::system());
        for (const TaskConfig& tc : next.tasks) rep.restartedTasks.push_back(tc.name);
        return {};
    }

    bool tagsChanged = next.tags != previous.tags;
    if (tagsChanged) {
        std::vector<std::string> forceChanges;  // forces follow their tags; never an invisible one left behind
        for (const auto& problem : image_->setTags(next.tags, &forceChanges)) log_.warn("{}", problem);
        for (const auto& change : forceChanges) log_.warn("{}", change);
    }

    bool identityChanged = next.name != previous.name || next.device != previous.device;
    if (next.modbus != previous.modbus || commPriorityOf(next) != commPriorityOf(previous)) {
        modbus_.reset();
        std::erase_if(commWarnings_, [](const std::string& w) { return w.starts_with("Modbus"); });
        startModbus();
        // Other devices lose or regain this PLC: say so in the log (a failed start has warned already)
        if (!config_.modbus.enabled) log_.info("Modbus off");
        else if (modbusStatus_.listening) log_.info("Modbus on port {} ({})", modbusStatus_.port, config_.modbus.listen);
    } else if (identityChanged && modbus_) {
        modbus_->setIdentity(identity());
    }
    if (next.opcua != previous.opcua || next.name != previous.name) {
        opcua_.reset();
        std::erase_if(commWarnings_, [](const std::string& w) { return w.starts_with("OPC UA"); });
        startOpcUa();
        if (next.opcua != previous.opcua) {  // not for a restart that only follows a new name
            if (!config_.opcua.enabled) log_.info("OPC UA off");
            else if (opcuaStatus_.listening) log_.info("OPC UA on port {} ({})", opcuaStatus_.port, config_.opcua.listen);
        }
    }

    bool priorityChanged = commPriorityOf(next) != commPriorityOf(previous);
    if (next.links != previous.links || priorityChanged) {
        startLinks(retired, priorityChanged);  // only changed links restart; the others keep running
    }

    // Tasks, matched by name: one that stays keeps its program and values; only what changed for it changes
    std::vector<std::shared_ptr<runtime::Task>> kept(next.tasks.size());
    std::vector<const TaskConfig*> before(next.tasks.size(), nullptr);
    std::vector<bool> taken(tasks_.size(), false);
    for (size_t i = 0; i < next.tasks.size(); i++) {
        for (size_t j = 0; j < tasks_.size() && j < previous.tasks.size(); j++) {
            if (taken[j] || previous.tasks[j].name != next.tasks[i].name) continue;
            kept[i] = tasks_[j];
            before[i] = &previous.tasks[j];
            taken[j] = true;
            break;
        }
    }
    for (size_t j = 0; j < tasks_.size() && j < previous.tasks.size(); j++) {
        if (taken[j]) continue;
        tasks_[j]->stop();  // a task that is gone; its outputs keep their last values
        programErrors_.erase(previous.tasks[j].name);
        log_.info("task '{}' removed", previous.tasks[j].name);
    }
    std::vector<bool> fresh(next.tasks.size(), false);
    tasks_.clear();
    for (size_t i = 0; i < next.tasks.size(); i++) {
        const TaskConfig& tc = next.tasks[i];
        if (!kept[i]) {
            tasks_.push_back(makeTask(tc));
            fresh[i] = true;
            continue;
        }
        tasks_.push_back(kept[i]);
        const TaskConfig& was = *before[i];
        if (tc.priority != was.priority || next.realtime != previous.realtime) kept[i]->setScheduling(tc.priority, next.realtime);
        if (tc.watchdogMs != was.watchdogMs) kept[i]->setWatchdog(tc.watchdogMs);
        if (tc.cycleMs != was.cycleMs) kept[i]->setCycleMs(tc.cycleMs);
        if (!tc.enabled) kept[i]->stop();  // keeps its program and values
    }

    // Programs: a new compile goes to every task that runs it (an online change); a program a task
    // newly runs goes to that task (it starts from initial values)
    for (auto& [program, plan] : plans) {
        if (!plan.compiledNow || !plan.compiled.ok) continue;  // forced: the old version runs on (and faults at its binding)
        installLocked(program, plan.compiled, plan.files, false);
    }
    for (size_t i = 0; i < next.tasks.size(); i++) {
        const TaskConfig& tc = next.tasks[i];
        std::string program = programs_.canonical(tc.program);
        bool switched = fresh[i] || !runtime::ProgramStore::sameName(before[i]->program, tc.program);
        if (!switched) continue;
        rep.restartedTasks.push_back(tc.name);
        Plan& plan = plans[program];
        if (plan.compiledNow) continue;  // loaded with the program above
        if (!plan.image) {
            programErrors_[tc.name] = std::format("program '{}' does not exist yet", tc.program);
            continue;
        }
        runtime::Task::LoadOutcome r = tasks_[i]->load(plan.image, program);
        programErrors_.erase(tc.name);
        log_.info("task '{}' runs program '{}' ({})", tc.name, program, r.replaced ? "switched" : "loaded");
        if (!loaded_.count(program)) loaded_[program] = {plan.files, attempts_.count(program) ? attempts_[program] : st::CompileResult{}};
    }
    // Programs no task runs any more are not loaded
    std::erase_if(loaded_, [&](const auto& entry) { return !runByTaskLocked(entry.first); });

    if (running_ && !fault_) {
        for (size_t i = 0; i < next.tasks.size(); i++) {
            const TaskConfig& tc = next.tasks[i];
            if (tc.enabled && !tasks_[i]->running()) tasks_[i]->start();
        }
    }
    return {};
}

std::vector<TaskStatus> Instance::tasks() const {
    std::lock_guard lock(mutex_);
    std::vector<TaskStatus> out;
    for (size_t i = 0; i < config_.tasks.size(); i++) {
        TaskStatus status{config_.tasks[i], {}};
        if (i < tasks_.size()) status.stats = tasks_[i]->stats();
        out.push_back(std::move(status));
    }
    return out;
}

ServerStatus Instance::modbusStatus() const {
    std::lock_guard lock(mutex_);
    return modbusStatus_;
}

ServerStatus Instance::opcuaStatus() const {
    std::lock_guard lock(mutex_);
    return opcuaStatus_;
}

modbus::TcpServer::Stats Instance::modbusStats() const {
    std::lock_guard lock(mutex_);
    return modbus_ ? modbus_->stats() : modbus::TcpServer::Stats{};
}

opcua::Server::Stats Instance::opcuaStats() const {
    std::lock_guard lock(mutex_);
    return opcua_ ? opcua_->stats() : opcua::Server::Stats{};
}

std::map<std::string, std::string> Instance::programErrors() const {
    std::lock_guard lock(mutex_);
    return programErrors_;
}

std::vector<std::string> Instance::warnings() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out = commWarnings_;
    for (const auto& [task, error] : programErrors_) out.push_back(std::format("task '{}': {}", task, error));
    // A task without a program does nothing, whatever the PLC's state says: always a warning
    for (size_t i = 0; open_ && i < tasks_.size() && i < config_.tasks.size(); i++) {
        if (config_.tasks[i].enabled && !tasks_[i]->image() && !programErrors_.count(config_.tasks[i].name)) {
            out.push_back(std::format("task '{}': runs no program", config_.tasks[i].name));
        }
    }
    return out;
}

}  // namespace simpleplc
