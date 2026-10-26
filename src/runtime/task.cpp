#include "runtime/task.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#if defined(__linux__)
#include <sys/mman.h>
#include <time.h>
#elif defined(__APPLE__)
#include <sys/mman.h>
#endif

#include "core/realtime.h"
#include "st/lexer.h"

namespace simpleplc::runtime {

using st::TypeKind;

using Clock = std::chrono::steady_clock;

namespace {

int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t ns(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

/// Sleeps until `t` (absolute); checks `interrupted` at least every 100 ms and returns when it is true.
template <typename Interrupted>
void sleepUntil(Clock::time_point t, Interrupted interrupted) {
    while (!interrupted()) {
        Clock::time_point now = Clock::now();
        if (now >= t) return;
        Clock::time_point until = std::min(t, now + std::chrono::milliseconds(100));
#if defined(__linux__)
        int64_t target = ns(until);
        timespec ts{static_cast<time_t>(target / 1000000000), static_cast<long>(target % 1000000000)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
#else
        // macOS (and others) coalesce timers with a slack of about 10 % of the sleep: a 100 ms sleep
        // wakes up to 10 ms late. Sleeping half the remaining time at a time keeps the last sleep,
        // and so the slack, short; it costs a few more wake-ups per cycle.
        auto remaining = until - now;
        if (until == t && remaining > std::chrono::milliseconds(2)) until = now + remaining / 2;
        std::this_thread::sleep_until(until);
#endif
    }
}

bool tagFits(TagType t, uint8_t kind) {
    TypeKind k = static_cast<TypeKind>(kind);
    switch (t) {
        case TagType::Bool: return k == TypeKind::Bool;
        case TagType::UInt16: return k == TypeKind::UInt || k == TypeKind::Word;
        case TagType::Int16: return k == TypeKind::Int;
        case TagType::UInt32: return k == TypeKind::UDInt || k == TypeKind::DWord;
        case TagType::Int32: return k == TypeKind::DInt;
        case TagType::Float32: return k == TypeKind::Real;
    }
    return false;
}

}  // namespace

std::expected<void, std::string> lockMemory() {
#if defined(__linux__) || defined(__APPLE__)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) return std::unexpected(std::string(std::strerror(errno)));
    return {};
#else
    return std::unexpected("not available on this platform");
#endif
}

Task::Task(TaskOptions options, std::shared_ptr<ProcessImage> image, FaultHandler onFault)
    : options_(std::move(options)),
      image_(std::move(image)),
      onFault_(std::move(onFault)),
      log_("task", options_.plcId),
      cycleMs_(options_.cycleMs),
      watchdogMs_(static_cast<int>(options_.watchdog.count())) {
    stats_.cycleMs = options_.cycleMs;
}

Task::~Task() { stop(); }

std::chrono::milliseconds Task::watchdog() const {
    if (int ms = watchdogMs_.load(); ms > 0) return std::chrono::milliseconds(ms);
    return std::max(std::chrono::milliseconds(100), std::chrono::milliseconds(10 * cycleMs_.load()));
}

void Task::setWatchdog(int watchdogMs) { watchdogMs_ = std::max(0, watchdogMs); }

void Task::setScheduling(int priority, bool realtime) {
    // The scheduling policy is set by the task thread when it starts: a new thread, same machine
    bool wasRunning = running_.load();
    if (wasRunning) stop();
    {
        std::lock_guard control(machineMutex_);
        options_.priority = priority;
        options_.realtime = realtime;
    }
    {
        std::lock_guard lock(mutex_);
        stats_.realtime = false;
    }
    if (wasRunning) start();
}

int Task::cycleMs() const { return cycleMs_.load(); }

void Task::setCycleMs(int cycleMs) {
    cycleMs_ = std::max(1, cycleMs);
    {
        std::lock_guard lock(mutex_);
        stats_.cycleMs = cycleMs_;
    }
    wake_.notify_all();
}

std::shared_ptr<const vm::Image> Task::image() const {
    std::lock_guard lock(mutex_);
    return current_;
}

std::shared_ptr<const vm::Image> Task::latestImage() const {
    std::lock_guard lock(mutex_);
    return pending_ ? pending_->image : current_;
}

namespace {

const vm::EnumInfo* enumOf(const vm::Image& image, const vm::VarEntry& v) {
    for (const vm::EnumInfo& e : image.enums) {
        if (e.name == v.typeName) return &e;
    }
    return nullptr;
}

bool sameEnumValues(const vm::EnumInfo& a, const vm::EnumInfo& b) {
    if (a.values.size() != b.values.size()) return false;
    for (size_t i = 0; i < a.values.size(); i++) {
        if (a.values[i].first != b.values[i].first || st::toUpper(a.values[i].second) != st::toUpper(b.values[i].second)) return false;
    }
    return true;
}

/// The same definition, whatever the type is called: the same bytes mean the same value.
bool sameDefinition(const vm::Image& from, const vm::VarEntry& a, const vm::Image& to, const vm::VarEntry& b) {
    if (a.kind != b.kind || a.size != b.size || a.hasRange != b.hasRange || a.typeSig != b.typeSig) return false;
    if (a.hasRange && (a.low != b.low || a.high != b.high)) return false;
    const vm::EnumInfo* ea = enumOf(from, a);
    const vm::EnumInfo* eb = enumOf(to, b);
    if (!ea || !eb) return !ea && !eb;
    return sameEnumValues(*ea, *eb);
}

/// The initial bytes of a variable (constants live in the data block with them).
bool sameInitialBytes(const vm::Image& from, const vm::VarEntry& a, const vm::Image& to, const vm::VarEntry& b) {
    if (a.size != b.size || a.offset + a.size > from.initialData.size() || b.offset + b.size > to.initialData.size()) return false;
    return std::memcmp(from.initialData.data() + a.offset, to.initialData.data() + b.offset, a.size) == 0;
}

int64_t readInteger(const uint8_t* p, uint32_t size, bool isSigned) {
    uint64_t bits = 0;
    std::memcpy(&bits, p, std::min<uint32_t>(size, 8));  // little-endian
    if (isSigned && size < 8 && (bits >> (size * 8 - 1)) & 1) bits |= ~((uint64_t{1} << (size * 8)) - 1);
    return static_cast<int64_t>(bits);
}

}  // namespace

void Task::onlineChangeOutcome(const vm::Image* from, const vm::Image& to, std::vector<std::string>& reset, size_t* kept) {
    if (kept) *kept = 0;
    if (from) {
        transfers(*from, to, &reset, kept);
        return;
    }
    // No program before: everything starts from its initial value
    for (const vm::VarEntry& v : to.vars) {
        if (!(v.flags & (vm::kVarExternal | vm::kVarConstant)) && v.path.find("__") == std::string::npos) reset.push_back(v.path);
    }
}

Task::Plan Task::transfers(const vm::Image& from, const vm::Image& to, std::vector<std::string>* reset, size_t* kept) {
    // A variable keeps its value when it has the same path and its type the same definition (tags
    // excluded: their mirrors are filled by the next copy-in). Constants are part of the program:
    // they always take the new program's values
    Plan plan;
    std::map<std::string, const vm::VarEntry*> before;
    for (const vm::VarEntry& v : from.vars) {
        if (!(v.flags & vm::kVarExternal)) before[v.path] = &v;
    }
    std::map<std::pair<std::string, std::string>, size_t> mapIndex;  // (old enum, new enum) → plan.maps
    for (const vm::VarEntry& v : to.vars) {
        if (v.flags & vm::kVarExternal) continue;
        bool listed = v.path.find("__") == std::string::npos;  // standard library internals are not listed
        auto it = before.find(v.path);
        const vm::VarEntry* old = it == before.end() ? nullptr : it->second;
        if (v.flags & vm::kVarConstant) {
            if (listed && reset && (!old || !(old->flags & vm::kVarConstant) || !sameDefinition(from, *old, to, v) ||
                                    !sameInitialBytes(from, *old, to, v))) {
                reset->push_back(v.path);
            }
            continue;
        }
        if (old && !(old->flags & vm::kVarConstant)) {
            if (sameDefinition(from, *old, to, v)) {
                plan.copies.push_back({old->offset, v.offset, v.size});
                if (listed && kept) ++*kept;
                continue;
            }
            // An enumeration whose values changed: by name, when every old value still exists
            const vm::EnumInfo* ea = enumOf(from, *old);
            const vm::EnumInfo* eb = enumOf(to, v);
            if (ea && eb && v.size <= 8 && old->size <= 8) {
                auto key = std::make_pair(ea->name, eb->name);
                auto found = mapIndex.find(key);
                if (found == mapIndex.end()) {
                    std::vector<std::pair<int64_t, int64_t>> map;
                    for (const auto& [oldValue, oldName] : ea->values) {
                        for (const auto& [newValue, newName] : eb->values) {
                            if (st::toUpper(oldName) == st::toUpper(newName)) map.push_back({oldValue, newValue});
                        }
                    }
                    size_t index = SIZE_MAX;
                    if (map.size() == ea->values.size()) {
                        index = plan.maps.size();
                        plan.maps.push_back(std::move(map));
                    }
                    found = mapIndex.emplace(key, index).first;
                }
                if (found->second != SIZE_MAX) {
                    plan.enums.push_back({old->offset, old->size, st::isSigned(static_cast<TypeKind>(old->kind)), v.offset,
                                          v.size, found->second});
                    if (listed && kept) ++*kept;
                    continue;
                }
            }
        }
        if (listed && reset) reset->push_back(v.path);
    }
    return plan;
}

void Task::applyPlan(const Plan& plan, const uint8_t* from, uint8_t* to) {
    for (const Transfer& t : plan.copies) std::memcpy(to + t.to, from + t.from, t.size);
    for (const EnumTransfer& e : plan.enums) {
        int64_t value = readInteger(from + e.from, e.fromSize, e.fromSigned);
        for (const auto& [oldValue, newValue] : plan.maps[e.map]) {
            if (oldValue != value) continue;
            uint64_t bits = static_cast<uint64_t>(newValue);
            std::memcpy(to + e.to, &bits, e.toSize);  // little-endian: the low bytes hold the value
            break;
        }
    }
}

Task::LoadOutcome Task::load(std::shared_ptr<const vm::Image> image, std::string programName) {
    LoadOutcome out;
    auto p = std::make_unique<Pending>();
    p->image = image;
    p->machine = std::make_unique<vm::Machine>(image);
    p->snapshotBuffer.assign(p->machine->size(), 0);
    p->program = std::move(programName);
    // Online change: computed against the program this one follows (the pending one, if a load is
    // still waiting for its cycle boundary); the swap checks that it still follows that one
    p->base = latestImage();
    if (p->base) {
        p->transfers = transfers(*p->base, *image, &out.reset);
        out.replaced = true;
    } else {
        onlineChangeOutcome(nullptr, *image, out.reset);
    }
    std::unique_ptr<Pending> dropped;  // a load that never ran: freed outside the lock
    {
        std::lock_guard lock(mutex_);
        dropped = std::move(pending_);
        pending_ = std::move(p);
        hasPending_ = true;
    }
    if (!running_) {
        std::lock_guard lock(machineMutex_);
        swapPending();
    }
    return out;
}

void Task::swapPending() {
    std::unique_ptr<Pending> p;
    {
        std::lock_guard lock(mutex_);
        p = std::move(pending_);
        hasPending_ = false;
    }
    if (!p) return;
    if (machine_) {
        // Loaded twice within one cycle time: the transfers were computed for a program that never
        // ran. Compute them again for the one that did (a program swap, not the steady cycle path)
        if (p->base != program_) p->transfers = transfers(*program_, *p->image, nullptr);
        applyPlan(p->transfers, machine_->data(), p->machine->data());
        // A warm continuation, not a start: unless the old program never ran a cycle since it started
        // (a first load over the template, a load after a restart) or nothing at all was taken over
        if (!machine_->firstCyclePending() && !p->transfers.empty()) p->machine->clearFirstCycle();
    }
    std::unique_ptr<vm::Machine> retired = std::move(machine_);
    machine_ = std::move(p->machine);
    program_ = p->image;
    programName_ = p->program;
    boundTagsVersion_ = UINT64_MAX;  // bind again
    std::vector<Write> stale;  // monitoring writes for the old layout: dropped, freed outside the lock
    {
        std::lock_guard lock(mutex_);
        current_ = program_;
        std::swap(stale, writes_);
        stats_.hasProgram = true;
        stats_.program = programName_;
        stats_.loadedAtMs = epochMs();
        stats_.dataBytes = program_->stackOffset;
        stats_.stackBytes = program_->stackSize;
        stats_.codeInstructions = static_cast<uint32_t>(program_->code.size());
    }
    {
        std::lock_guard lock(snapshotMutex_);
        std::swap(snapshotBuffer_, p->snapshotBuffer);
        snapshotImage_.reset();
    }
    // `retired` and the old snapshot buffer are freed here, outside the image lock and the stats lock
}

bool Task::bind() {
    // The version first: a tag change while binding then makes the next cycle bind again
    uint64_t version = image_->tagsVersion();
    const vm::Image& img = *program_;
    bound_.clear();
    slots_.clear();
    size_t words = 0;
    for (const vm::Binding& b : img.bindings) {
        auto tag = image_->findTag(b.tag);
        if (!tag || !tagFits(tag->type, b.kind) || !image_->contains(tag->address, tagWidth(tag->type))) {
            vm::RunResult r;
            r.fault = vm::Fault::Binding;
            recordFault(r);
            std::lock_guard lock(mutex_);
            if (stats_.fault) {
                stats_.fault->message = tag ? std::format("tag '{}' changed type; the program expects {}", b.tag,
                                                          st::elementaryName(static_cast<TypeKind>(b.kind)))
                                            : std::format("tag '{}' no longer exists", b.tag);
            }
            return false;
        }
        BoundTag bt;
        bt.binding = b;
        bt.tagType = tag->type;
        bt.slot = Slot{tag->area, tag->address, static_cast<uint8_t>(tagWidth(tag->type))};
        bt.word = words;
        words += bt.slot.words;
        bound_.push_back(bt);
        slots_.push_back(bt.slot);
    }
    words_.assign(words + 1, 0);
    inWords_.assign(words + 1, 0);
    mask_.assign(bound_.size() + 1, 0);
    boundTagsVersion_ = version;
    return true;
}

void Task::copyIn() {
    if (bound_.empty()) return;
    image_->readSlots(slots_, words_.data());
    std::copy(words_.begin(), words_.end(), inWords_.begin());
    uint8_t* d = machine_->data();
    uint64_t rejected = 0;
    for (const BoundTag& b : bound_) {
        const uint16_t* w = words_.data() + b.word;
        uint8_t* m = d + b.binding.offset;
        switch (b.tagType) {
            case TagType::Bool: m[0] = w[0] ? 1 : 0; break;
            case TagType::UInt16:
            case TagType::Int16: std::memcpy(m, w, 2); break;
            case TagType::UInt32:
            case TagType::Int32: {
                uint32_t v = (uint32_t{w[0]} << 16) | w[1];
                std::memcpy(m, &v, 4);
                break;
            }
            case TagType::Float32: {
                uint32_t v = (uint32_t{w[0]} << 16) | w[1];
                float f;
                std::memcpy(&f, &v, 4);
                if (std::isfinite(f)) std::memcpy(m, &v, 4);
                else rejected++;  // keep the last good value; a NaN would make every comparison FALSE
                break;
            }
        }
    }
    if (rejected) {
        std::lock_guard lock(mutex_);
        stats_.nanRejected += rejected;
    }
}

void Task::copyOut() {
    if (bound_.empty()) return;
    uint8_t* dirty = machine_->dirty();
    const uint8_t* d = machine_->data();
    bool any = false;
    for (size_t i = 0; i < bound_.size(); i++) {
        mask_[i] = 0;
        if (!dirty[i]) continue;
        dirty[i] = 0;
        const BoundTag& b = bound_[i];
        uint16_t* w = words_.data() + b.word;
        const uint8_t* m = d + b.binding.offset;
        switch (b.tagType) {
            case TagType::Bool: w[0] = m[0] ? 1 : 0; break;
            case TagType::UInt16:
            case TagType::Int16: std::memcpy(w, m, 2); break;
            default: {
                uint32_t v;
                std::memcpy(&v, m, 4);
                w[0] = static_cast<uint16_t>(v >> 16);
                w[1] = static_cast<uint16_t>(v & 0xFFFF);
                break;
            }
        }
        // Only a value the program changed: storing back what it read must not undo a write
        // that Modbus, OPC UA or an operator made during the cycle
        size_t n = b.slot.words;
        if (!std::equal(w, w + n, inWords_.data() + b.word)) {
            mask_[i] = 1;
            any = true;
        }
    }
    // Refused (nothing written) when the tag table changed since bind or the PLC is in FAULT
    if (any) image_->writeSlots(slots_, words_.data(), mask_.data(), Origin::Program, boundTagsVersion_);
}

void Task::recordFault(const vm::RunResult& r) {
    FaultInfo f;
    f.task = options_.name;
    f.program = programName_;
    f.code = vm::faultCode(r.fault);
    f.message = vm::faultText(r.fault);
    f.atMs = epochMs();
    if (program_ && r.fault != vm::Fault::Binding) {
        vm::LineEntry at = program_->lineAt(r.pc);
        f.file = at.file < program_->files.size() ? program_->files[at.file] : "";
        f.line = at.line;
        for (uint8_t i = 0; i < r.depth; i++) {
            vm::LineEntry c = program_->lineAt(r.callers[i]);
            f.calledFrom.push_back({c.file < program_->files.size() ? program_->files[c.file] : "", c.line});
        }
    }
    std::lock_guard lock(mutex_);
    f.cycle = stats_.cycles + 1;
    stats_.fault = f;
}

bool Task::cycleOnce(int64_t nowNs, Clock::time_point deadline) {
    if (!machine_) return true;
    if (boundTagsVersion_ != image_->tagsVersion() && !bind()) return false;

    // Monitoring writes and a requested cold reset, between cycles
    {
        std::lock_guard lock(mutex_);
        if (resetRequested_) {
            machine_->reset(resetKeepsRetain_);
            machine_->markFirstCycle();
            resetRequested_ = false;
        }
        if (!writes_.empty() && writesApplied_.empty()) {
            for (const Write& w : writes_) {
                if (w.image == program_) std::memcpy(machine_->data() + w.offset, w.bytes.data(), w.bytes.size());
            }
            std::swap(writes_, writesApplied_);  // no allocation or freeing here
        }
    }
    copyIn();
    vm::RunLimits limits;
    limits.budget = options_.budget;
    limits.deadline = deadline;
    vm::RunResult r = machine_->cycle(nowNs, limits);
    if (r.fault != vm::Fault::None) {
        recordFault(r);
        return false;
    }
    copyOut();

    uint64_t cycles = 0;
    {
        std::lock_guard lock(mutex_);
        cycles = ++stats_.cycles;
    }
    // A snapshot only when nobody is reading the last one: never wait for a monitoring request
    if (snapshotWanted_.load() && snapshotMutex_.try_lock()) {
        if (snapshotBuffer_.size() == machine_->size()) {
            std::memcpy(snapshotBuffer_.data(), machine_->data(), machine_->size());
            snapshotImage_ = program_;
            snapshotCycle_ = cycles;
            snapshotWanted_ = false;
        }
        snapshotMutex_.unlock();
        wake_.notify_all();
    }
    return true;
}

void Task::recordLateness(double us) {
    // Buckets of 2^(i/4) µs: 1 µs … ~65 ms, the last one open-ended
    int b = us <= 1 ? 0 : std::min(63, static_cast<int>(std::log2(us) * 4) + 1);
    lateHistogram_[static_cast<size_t>(b)]++;
    lateCount_++;
    auto percentile = [&](double q) {
        uint64_t want = static_cast<uint64_t>(std::ceil(q * static_cast<double>(lateCount_)));
        uint64_t seen = 0;
        for (size_t i = 0; i < lateHistogram_.size(); i++) {
            seen += lateHistogram_[i];
            if (seen >= want) return std::pow(2.0, static_cast<double>(i) / 4.0);
        }
        return std::pow(2.0, 63.0 / 4.0);
    };
    stats_.p50LateUs = percentile(0.5);
    stats_.p99LateUs = percentile(0.99);
    stats_.p999LateUs = percentile(0.999);
}


void Task::run() {
    preferInteractive();
    bool constrained = false;  // real-time scheduling in effect (re-applied with a new period on macOS)
    if (options_.realtime && options_.priority > 0) {
        auto ok = makeThreadRealtime(options_.priority, cycleMs_.load());
        constrained = ok.has_value();
        if (!ok) log_.warn("{}: real-time scheduling refused ({}); running with normal scheduling", options_.name, ok.error());
        std::lock_guard lock(mutex_);
        stats_.realtime = constrained;
    }
    using std::chrono::duration;
    using std::chrono::nanoseconds;
    Clock::time_point anchor = Clock::now();
    int period = cycleMs_.load();
    uint64_t slot = 0;
    bool faulted = false;
    while (!stopRequested_.load() && !faulted) {
        if (cycleMs_.load() != period) {  // a new cycle time: a new grid from the next slot
            anchor = Clock::now();
            period = cycleMs_.load();
            slot = 0;
#if defined(__APPLE__)
            if (constrained) (void)makeThreadRealtime(options_.priority, period);  // the policy carries the period
#endif
        }
        Clock::time_point planned = anchor + std::chrono::milliseconds(period) * slot;
        sleepUntil(planned, [&] { return stopRequested_.load() || cycleMs_.load() != period; });
        if (stopRequested_.load()) break;
        if (cycleMs_.load() != period) continue;  // a new grid starts now
        Clock::time_point wake = Clock::now();
        {
            std::lock_guard machineLock(machineMutex_);
            if (hasPending_.load()) swapPending();
            bool ok = cycleOnce(ns(planned) - ns(options_.timeOrigin), wake + watchdog());
            Clock::time_point done = Clock::now();
            double execUs = duration<double, std::micro>(done - wake).count();
            double lateUs = duration<double, std::micro>(wake - planned).count();
            {
                std::lock_guard lock(mutex_);
                stats_.lastExecUs = execUs;
                stats_.maxExecUs = std::max(stats_.maxExecUs, execUs);
                stats_.avgExecUs = stats_.cycles <= 1 ? execUs : stats_.avgExecUs * 0.99 + execUs * 0.01;
                stats_.lastLateUs = lateUs;
                stats_.maxLateUs = std::max(stats_.maxLateUs, lateUs);
                recordLateness(lateUs);
            }
            if (!ok) {
                faulted = true;
                continue;  // leaves the loop; the handler runs without the machine lock
            }
            // The next slot on the grid; after an exceeded cycle time skip the slots already gone (no catch-up)
            slot++;
            Clock::time_point next = anchor + std::chrono::milliseconds(period) * slot;
            if (done > next) {
                uint64_t missed = static_cast<uint64_t>((done - next) / std::chrono::milliseconds(period)) + 1;
                slot += missed;
                std::lock_guard lock(mutex_);
                stats_.exceeded += missed;
            }
        }
    }
    if (faulted) {
        running_ = false;
        FaultInfo f;
        {
            std::lock_guard lock(mutex_);
            stats_.running = false;
            if (stats_.fault) f = *stats_.fault;
        }
        if (onFault_) onFault_(f);
    }
}

void Task::start() {
    std::lock_guard control(machineMutex_);
    if (running_) return;
    if (hasPending_.load()) swapPending();
    if (!machine_) return;  // no program yet: the load that brings one starts the task
    stopRequested_ = false;
    running_ = true;
    {
        std::lock_guard lock(mutex_);
        stats_.running = true;
    }
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&Task::run, this);
}

void Task::stop() {
    stopRequested_ = true;
    wake_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    running_ = false;
    std::vector<Write> queued;  // monitoring writes for this run are void after STOP (freed outside the lock)
    std::lock_guard lock(mutex_);
    stats_.running = false;
    std::swap(queued, writes_);
}

bool Task::step(int64_t nowNs) {
    if (running_) return true;
    std::lock_guard control(machineMutex_);
    if (hasPending_.load()) swapPending();
    if (!machine_) return true;
    Clock::time_point start = Clock::now();
    bool ok = cycleOnce(nowNs, start + watchdog());
    double execUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    std::lock_guard lock(mutex_);
    stats_.lastExecUs = execUs;
    stats_.maxExecUs = std::max(stats_.maxExecUs, execUs);
    stats_.avgExecUs = stats_.cycles <= 1 ? execUs : stats_.avgExecUs * 0.99 + execUs * 0.01;
    return ok;
}

void Task::reset(bool keepRetain) {
    std::lock_guard control(machineMutex_);
    if (!running_ && hasPending_.load()) swapPending();  // the reset (and FIRST_CYCLE) is for the program that will run
    if (machine_ && !running_) {
        machine_->reset(keepRetain);
        machine_->markFirstCycle();
    } else {
        std::lock_guard lock(mutex_);
        resetRequested_ = true;
        resetKeepsRetain_ = keepRetain;
    }
}

void Task::clearFault() {
    std::lock_guard lock(mutex_);
    stats_.fault.reset();
}

TaskStats Task::stats() const {
    std::lock_guard lock(mutex_);
    TaskStats s = stats_;
    s.running = running_.load();
    s.cycleMs = cycleMs_.load();
    s.watchdogMs = static_cast<int>(watchdog().count());
    return s;
}

Task::Snapshot Task::snapshot() const {
    auto stopped = [&]() -> Snapshot {
        std::lock_guard control(machineMutex_);
        if (!machine_) return {};
        return {program_, std::make_shared<std::vector<uint8_t>>(machine_->data(), machine_->data() + machine_->size())};
    };
    if (!running_) return stopped();
    // Ask the task thread for a copy after its next cycle; the copy to the heap happens here, under
    // the snapshot lock only (the task thread skips a snapshot rather than wait for it)
    uint64_t after = 0;
    {
        std::lock_guard lock(snapshotMutex_);
        after = snapshotCycle_;
        snapshotWanted_ = true;
    }
    auto copy = [&]() -> Snapshot {
        std::lock_guard lock(snapshotMutex_);
        if (!snapshotImage_) return {};
        return {snapshotImage_, std::make_shared<std::vector<uint8_t>>(snapshotBuffer_)};
    };
    std::unique_lock wait(wakeMutex_);
    // A slow task (cycle_ms up to 60 s) must not hold a request for minutes: then the last copy, or nothing
    Clock::time_point until = Clock::now() + std::chrono::milliseconds(std::clamp(3 * cycleMs_.load(), 200, 2000));
    while (Clock::now() < until && running_) {
        {
            std::lock_guard lock(snapshotMutex_);
            if (snapshotImage_ && snapshotCycle_ > after) {
                return {snapshotImage_, std::make_shared<std::vector<uint8_t>>(snapshotBuffer_)};
            }
        }
        wake_.wait_for(wait, std::chrono::milliseconds(5));
    }
    if (!running_) return stopped();
    return copy();
}

std::expected<void, std::string> Task::write(const std::string& path, std::vector<uint8_t> bytes) {
    std::shared_ptr<const vm::Image> img = image();
    if (!img) return std::unexpected("the task has no program");
    for (const vm::VarEntry& v : img->vars) {
        if (v.path != path) continue;
        if (v.flags & (vm::kVarConstant | vm::kVarExternal)) {
            return std::unexpected(std::format("{} is {}", path, v.flags & vm::kVarConstant ? "CONSTANT" : "a tag: write the tag instead"));
        }
        if (bytes.size() != v.size) return std::unexpected("value has the wrong size");
        if (!running_) {
            std::lock_guard control(machineMutex_);
            if (machine_ && program_ == img) std::memcpy(machine_->data() + v.offset, bytes.data(), bytes.size());
            return {};
        }
        std::vector<Write> applied;  // freed below, outside the lock
        std::lock_guard lock(mutex_);
        if (current_ != img) return std::unexpected("the program changed meanwhile; read the variables again");
        std::swap(applied, writesApplied_);
        writes_.push_back({img, v.offset, std::move(bytes)});
        return {};
    }
    return std::unexpected(std::format("no variable {}", path));
}

}  // namespace simpleplc::runtime
