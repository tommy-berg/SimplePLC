#include "modbus/links.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "core/realtime.h"
#include "modbus/pdu.h"

namespace simpleplc::modbus {
namespace {

constexpr uint64_t kRefreshCycles = 10;  // every write tag is written again this often
constexpr size_t kTagsInMessage = 4;     // tags named in an error message

int64_t agoMs(std::chrono::steady_clock::time_point then, std::chrono::steady_clock::time_point now) {
    if (then == std::chrono::steady_clock::time_point{}) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
}

}  // namespace

std::string_view wordOrderName(WordOrder order) { return order == WordOrder::Cdab ? "cdab" : "abcd"; }

std::optional<WordOrder> parseWordOrder(std::string_view name) {
    if (name == "abcd") return WordOrder::Abcd;
    if (name == "cdab") return WordOrder::Cdab;
    return std::nullopt;
}

LinkPoller::LinkPoller(LinkConfig config, std::shared_ptr<ProcessImage> image, const std::string& plcId, int realtimePriority,
                       std::function<Outputs()> outputs)
    : config_(std::move(config)),
      image_(std::move(image)),
      realtimePriority_(realtimePriority),
      outputs_(std::move(outputs)),
      log_("link", plcId),
      client_(TcpClient::Options{config_.host, static_cast<uint16_t>(config_.port), static_cast<uint8_t>(config_.unit),
                                 std::chrono::milliseconds(config_.timeoutMs)}) {
    status_.name = config_.name;
    status_.host = config_.host;
    status_.port = static_cast<uint16_t>(config_.port);
    status_.unit = static_cast<uint8_t>(config_.unit);
    client_.setInterrupt([this] { return keepWaiting(); });
}

LinkPoller::~LinkPoller() { stop(); }

void LinkPoller::start() {
    if (thread_.joinable()) return;
    stopMode_ = kRunning;
    thread_ = std::thread([this] { run(); });
}

void LinkPoller::requestStop(bool flush, bool clearStatus) {
    {
        std::lock_guard lock(wakeMutex_);
        clearStatus_ = clearStatus;
        int mode = flush ? kFlush : kAbort;
        if (stopMode_.load() < mode) stopMode_ = mode;  // an abort is never turned into a flush
    }
    wake_.notify_all();
}

void LinkPoller::stop() {
    if (!thread_.joinable()) return;
    if (stopMode_.load() == kRunning) requestStop(false);  // a requested flush is waited for (bounded by the timeout)
    thread_.join();
}

LinkStatus LinkPoller::status() const {
    std::lock_guard lock(mutex_);
    LinkStatus s = status_;
    auto now = std::chrono::steady_clock::now();
    s.lastRequestAgoMs = agoMs(lastRequestAt_, now);
    s.lastSuccessAgoMs = agoMs(lastSuccessAt_, now);
    return s;
}

void LinkPoller::run() {
    preferInteractive();
    if (realtimePriority_ > 0) {
        if (auto ok = makeThreadRealtime(realtimePriority_, config_.periodMs); !ok) {
            log_.warn("link '{}': real-time scheduling refused ({})", config_.name, ok.error());
        }
    }
    auto period = std::chrono::milliseconds(config_.periodMs);
    auto stale = std::chrono::milliseconds(config_.staleAfterMs());
    auto next = Clock::now();
    while (stopMode_.load() == kRunning) {
        auto now = Clock::now();
        if (now < next) {
            // Until the next cycle; earlier when the status goes stale before it
            auto until = fresh_ ? std::min(next, lastSuccess_ + stale) : next;
            {
                std::unique_lock lock(wakeMutex_);
                wake_.wait_until(lock, until, [this] { return stopMode_.load() != kRunning; });
            }
            checkStale(Clock::now());
            continue;
        }
        cycle(now);
        // Absolute deadlines; after an exceeded period (e.g. a timeout) continue on the grid
        next += period;
        now = Clock::now();
        if (now >= next) {
            auto missed = (now - next) / period + 1;
            next += missed * period;
            std::lock_guard lock(mutex_);
            status_.exceeded += static_cast<uint64_t>(missed);
        }
        identify(next);
    }
    if (stopMode_.load() == kFlush) flush();
    if (clearStatus_.load()) {
        setStatusTag(false);  // a stopped link is not a working link
        if (staleMarked_) image_->setStale(presentReadSlots_, false);  // its read tags are nobody's now
    }
    client_.close();
}

void LinkPoller::resolve() {
    if (staleMarked_) {
        image_->setStale(presentReadSlots_, false);  // the old addresses may belong to other tags now
        staleMarked_ = false;
        published_ = false;
    }
    tagsVersion_ = image_->tagsVersion();
    std::map<std::string, TagDef> byName;
    for (TagDef& t : image_->tags()) byName.emplace(t.name, std::move(t));
    auto target = [&](const LinkMapping& m) {
        Target t;
        t.mapping = &m;
        auto it = byName.find(m.tag);
        if (it != byName.end() && (it->second.type == TagType::Bool) == isBitArea(m.area)) {
            t.local = Slot{it->second.area, it->second.address, static_cast<uint8_t>(tagWidth(it->second.type))};
            t.type = it->second.type;
            t.words = tagWidth(it->second.type);
            t.present = true;
        }
        return t;
    };
    reads_.clear();
    writes_.clear();
    for (const LinkMapping& m : config_.read) reads_.push_back(target(m));
    for (const LinkMapping& m : config_.write) writes_.push_back(target(m));

    // Contiguous (or overlapping) read ranges of one area share a request
    std::vector<size_t> order;
    for (size_t i = 0; i < reads_.size(); i++) {
        if (reads_[i].present) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const LinkMapping& x = *reads_[a].mapping;
        const LinkMapping& y = *reads_[b].mapping;
        return std::pair(x.area, x.address) < std::pair(y.area, y.address);
    });
    requests_.clear();
    for (size_t i : order) {
        const Target& t = reads_[i];
        uint32_t start = static_cast<uint32_t>(t.mapping->address);
        uint32_t end = start + t.words;
        uint32_t max = isBitArea(t.mapping->area) ? kMaxReadBits : kMaxReadRegisters;
        if (!requests_.empty()) {
            Request& r = requests_.back();
            uint32_t rEnd = uint32_t{r.address} + r.count;
            if (r.area == t.mapping->area && start <= rEnd && std::max(end, rEnd) - r.address <= max) {
                r.count = static_cast<uint16_t>(std::max(end, rEnd) - r.address);
                r.targets.push_back(i);
                continue;
            }
        }
        requests_.push_back(Request{t.mapping->area, static_cast<uint16_t>(start), static_cast<uint16_t>(t.words), {i}});
    }

    readSlots_.clear();
    presentReadSlots_.clear();
    readOffsets_.clear();
    size_t words = 0;
    for (const Target& t : reads_) {
        readSlots_.push_back(t.present ? t.local : Slot{});  // absent tags stay masked out
        if (t.present) presentReadSlots_.push_back(t.local);
        readOffsets_.push_back(words);
        words += t.present ? t.words : 1;
    }
    readValues_.assign(words, 0);
    readMask_.assign(reads_.size(), 0);
    writeSlots_.clear();
    words = 0;
    for (const Target& t : writes_) {
        writeSlots_.push_back(t.present ? t.local : Slot{});
        words += t.present ? t.words : 1;
    }
    writeValues_.assign(words, 0);
    written_.assign(words, 0);
    writeOrigins_.assign(writes_.size(), Origin::None);
    writtenOn_.assign(writes_.size(), 0);
}

std::string LinkPoller::describe(const char* what, Area area, uint32_t address, uint32_t count,
                                 const std::vector<size_t>& targets, const std::vector<Target>& list) const {
    std::string names;
    for (size_t k = 0; k < targets.size() && k < kTagsInMessage; k++) {
        names += std::format("{}{}", k ? ", " : "", list[targets[k]].mapping->tag);
    }
    if (targets.size() > kTagsInMessage) names += std::format(" and {} more", targets.size() - kTagsInMessage);
    std::string range = count > 1 ? std::format("{}-{}", address, address + count - 1) : std::to_string(address);
    return std::format("{} {} {} (tag{} {})", what, areaName(area), range, targets.size() == 1 ? "" : "s", names);
}

void LinkPoller::addError(std::vector<std::string>& errors, std::string error) {
    // A link that had no error gets this cycle's first problem at once: a cycle against a hung device takes
    // several timeouts, and the status tag may drop (stale) before it ends; it must not drop without a reason.
    // A link that already has one keeps it until the cycle's own (one text per cycle, no flicker)
    if (errors.empty() && lastErrorText_.empty()) {
        lastErrorText_ = error;
        std::lock_guard lock(mutex_);
        status_.lastError = error;
    }
    errors.push_back(std::move(error));
}

void LinkPoller::readAll(std::vector<std::string>& errors, bool& sent) {
    std::fill(readMask_.begin(), readMask_.end(), 0);
    for (const Request& r : requests_) {
        if (stopMode_.load() != kRunning) return;
        auto values = client_.read(r.area, r.address, r.count);
        sent = true;
        {
            std::lock_guard lock(mutex_);
            status_.requests++;
        }
        // Each request stands on its own: one the device refuses costs only its own tags
        if (!values) {
            addError(errors, std::format("{}: {}", describe("read", r.area, r.address, r.count, r.targets, reads_), values.error()));
            continue;
        }
        for (size_t i : r.targets) {
            const Target& t = reads_[i];
            size_t from = static_cast<size_t>(t.mapping->address - r.address);
            uint16_t* to = readValues_.data() + readOffsets_[i];
            std::copy_n(values->begin() + static_cast<std::ptrdiff_t>(from), t.words, to);
            if (t.words == 2 && config_.wordOrder == WordOrder::Cdab) std::swap(to[0], to[1]);
            // A device's NaN or infinity never reaches a float tag (the program would compute with it)
            if (t.type == TagType::Float32 && !std::isfinite(decodeTagValue(TagType::Float32, to))) {
                addError(errors, std::format("{}: the device sent a value that is not a number; the tag keeps its last value",
                                             describe("read", t.mapping->area, static_cast<uint32_t>(t.mapping->address), 2, {i}, reads_)));
                continue;
            }
            readMask_[i] = 1;
        }
    }
    // All values of the cycle in one step: a program never sees half of them
    // Not after a tag change since resolve(): the next cycle resolves again
    if (!image_->writeSlots(readSlots_, readValues_.data(), readMask_.data(), Origin::Link, tagsVersion_)) {
        addError(errors, "the tag table changed; the next cycle uses the new one");
    }
}

void LinkPoller::writeChanged(bool all, Outputs outputs, std::vector<std::string>& errors, bool& sent) {
    // Before the programs' first cycle the tags hold nothing they computed: nothing goes out
    if (writes_.empty() || outputs == Outputs::Pending) return;
    image_->readSlots(writeSlots_, writeValues_.data(), writeOrigins_.data());
    size_t off = 0;
    for (size_t i = 0; i < writes_.size(); i++) {
        const Target& t = writes_[i];
        size_t words = t.present ? t.words : 1;
        // In STOP or FAULT only what something wrote (a substitute value, a client): a tag nobody has written
        // since power-up has nothing to say, and the device keeps its own value
        bool send = t.present && !(outputs == Outputs::Substitute && writeOrigins_[i] == Origin::None);
        if (send) {
            bool changed = !std::equal(writeValues_.begin() + static_cast<std::ptrdiff_t>(off),
                                       writeValues_.begin() + static_cast<std::ptrdiff_t>(off + words),
                                       written_.begin() + static_cast<std::ptrdiff_t>(off));
            // A new connection may be to a device that restarted with its own defaults: send it everything
            bool known = writtenOn_[i] != 0 && writtenOn_[i] == client_.connects() && client_.connected();
            if (all || !known || changed) {
                if (stopMode_.load() != kRunning) return;
                uint16_t out[2] = {writeValues_[off], words == 2 ? writeValues_[off + 1] : uint16_t{0}};
                if (words == 2 && config_.wordOrder == WordOrder::Cdab) std::swap(out[0], out[1]);
                auto ok = client_.write(t.mapping->area, static_cast<uint16_t>(t.mapping->address),
                                        std::span<const uint16_t>(out, words));
                sent = true;
                {
                    std::lock_guard lock(mutex_);
                    status_.requests++;
                }
                if (!ok) {
                    addError(errors, std::format("{}: {}", describe("write", t.mapping->area, static_cast<uint32_t>(t.mapping->address),
                                                                    static_cast<uint32_t>(words), {i}, writes_),
                                                 ok.error()));
                    writtenOn_[i] = 0;
                } else {
                    std::copy_n(writeValues_.begin() + static_cast<std::ptrdiff_t>(off), words,
                                written_.begin() + static_cast<std::ptrdiff_t>(off));
                    writtenOn_[i] = client_.connects();
                }
            }
        }
        off += words;
    }
}

void LinkPoller::probe(std::vector<std::string>& errors) {
    // Its first write address (or read address): reading an output back is harmless, and any answer,
    // an exception too, shows the device is there
    const LinkMapping* m = !config_.write.empty() ? &config_.write.front() : !config_.read.empty() ? &config_.read.front() : nullptr;
    if (!m) return;
    auto r = client_.read(m->area, static_cast<uint16_t>(m->address), 1);
    {
        std::lock_guard lock(mutex_);
        status_.requests++;
    }
    if (!r && client_.lastFailure() != TcpClient::Failure::Device) {
        addError(errors, std::format("read {} {} (to check that the device answers): {}", areaName(m->area), m->address, r.error()));
    }
}

void LinkPoller::flush() {
    // The final cycle at shutdown: every write tag once more (the substitute values STOP has just written),
    // whatever was sent before, within the link's timeout
    flushing_ = true;
    flushDeadline_ = Clock::now() + std::chrono::milliseconds(config_.timeoutMs);
    Outputs outputs = outputs_ ? outputs_() : Outputs::Run;
    if (outputs == Outputs::Pending) return;
    if (image_->tagsVersion() != tagsVersion_) resolve();
    if (writes_.empty()) return;
    image_->readSlots(writeSlots_, writeValues_.data(), writeOrigins_.data());
    size_t off = 0;
    size_t sentCount = 0;
    std::string failure;
    for (size_t i = 0; i < writes_.size(); i++) {
        const Target& t = writes_[i];
        size_t words = t.present ? t.words : 1;
        if (t.present && !(outputs == Outputs::Substitute && writeOrigins_[i] == Origin::None)) {
            if (stopMode_.load() == kAbort || Clock::now() >= flushDeadline_) {
                if (failure.empty()) failure = "no time left";
                break;
            }
            uint16_t out[2] = {writeValues_[off], words == 2 ? writeValues_[off + 1] : uint16_t{0}};
            if (words == 2 && config_.wordOrder == WordOrder::Cdab) std::swap(out[0], out[1]);
            auto ok = client_.write(t.mapping->area, static_cast<uint16_t>(t.mapping->address), std::span<const uint16_t>(out, words));
            if (ok) sentCount++;
            else if (failure.empty()) failure = std::format("{}: {}", describe("write", t.mapping->area, static_cast<uint32_t>(t.mapping->address),
                                                                          static_cast<uint32_t>(words), {i}, writes_),
                                                            ok.error());
        }
        off += words;
    }
    if (!failure.empty()) {
        log_.warn("link '{}' to {}:{}: not every output reached the device before stopping ({} sent): {}", config_.name, config_.host,
                  config_.port, sentCount, failure);
    } else {
        log_.debug("link '{}': {} output{} sent before stopping", config_.name, sentCount, sentCount == 1 ? "" : "s");
    }
}

bool LinkPoller::keepWaiting() {
    int mode = stopMode_.load();
    if (mode == kAbort) return false;
    if (mode == kFlush) return flushing_ && Clock::now() < flushDeadline_;  // the cycle in progress gives way to the final one
    checkStale(Clock::now());
    return true;
}

void LinkPoller::checkStale(Clock::time_point now) {
    if (!fresh_ || now - lastSuccess_ < std::chrono::milliseconds(config_.staleAfterMs())) return;
    publish(false, lastErrorText_.empty() ? std::format("no exchange succeeded for {} ms", config_.staleAfterMs()) : lastErrorText_);
}

void LinkPoller::setStatusTag(bool ok) {
    if (config_.statusTag.empty()) return;
    (void)image_->writeTag(config_.statusTag, ok ? 1 : 0, Origin::Link);
}

void LinkPoller::publish(bool fresh, const std::string& why) {
    bool was = fresh_;
    fresh_ = fresh;
    {
        std::lock_guard lock(mutex_);
        status_.ok = fresh;
    }
    setStatusTag(fresh);
    if (!published_ || staleMarked_ == fresh) {
        image_->setStale(presentReadSlots_, !fresh);
        staleMarked_ = !fresh;
    }
    published_ = true;
    if (was && !fresh) {
        log_.event(log::Level::Warn, "link", log::Author::system(), "link '{}' to {}:{} is down: {}", config_.name, config_.host,
                   config_.port, why);
    }
    if (!was && fresh) log_.event(log::Level::Info, "link", log::Author::system(), "link '{}' to {}:{} is up", config_.name, config_.host, config_.port);
}

void LinkPoller::identify(Clock::time_point next) {
    if (stopMode_.load() != kRunning || !client_.connected() || identifiedOn_ == client_.connects()) return;
    auto now = Clock::now();
    if (now < identifyAfter_) return;
    // Only in half the time left before the next cycle: identification never makes an exchange late
    auto slack = std::chrono::duration_cast<std::chrono::milliseconds>(next - now) / 2;
    if (slack < std::chrono::milliseconds(10)) return;
    identifiedOn_ = client_.connects();
    auto id = client_.readIdentity(std::min(slack, std::chrono::milliseconds(config_.timeoutMs)));
    if (id) {
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            changed = status_.identity != *id;
            status_.identity = *id;
        }
        if (changed) log_.debug("link '{}': {}:{} is {} {} {}", config_.name, config_.host, config_.port, id->vendor, id->product, id->revision);
    } else if (client_.lastFailure() == TcpClient::Failure::Device) {
        std::lock_guard lock(mutex_);
        status_.identity.reset();  // it answered: it does not support identification (exception 1), quietly
    } else if (client_.lastFailure() == TcpClient::Failure::Transport) {
        identifyAfter_ = Clock::now() + std::chrono::seconds(60);  // no answer: maybe on a later connection
    }
}

void LinkPoller::cycle(Clock::time_point now) {
    if (image_->tagsVersion() != tagsVersion_) resolve();
    Outputs outputs = outputs_ ? outputs_() : Outputs::Run;
    std::vector<std::string> errors;
    bool refresh = ++sinceRefresh_ >= kRefreshCycles;
    if (refresh) sinceRefresh_ = 0;
    bool sent = false;
    readAll(errors, sent);
    writeChanged(refresh, outputs, errors, sent);
    if (!sent && stopMode_.load() == kRunning) probe(errors);
    if (stopMode_.load() != kRunning) return;  // cut short: says nothing about the device

    bool ok = errors.empty();
    std::string error;
    if (!errors.empty()) {
        error = errors.front();
        if (errors.size() > 1) error += std::format(" (and {} more problem{})", errors.size() - 1, errors.size() == 2 ? "" : "s");
    }
    for (const auto* list : {&reads_, &writes_}) {
        for (const Target& t : *list) {
            if (!t.present && error.empty()) error = std::format("tag '{}' is not in the tag table (or has another type)", t.mapping->tag);
        }
    }
    auto after = Clock::now();
    if (ok) lastSuccess_ = after;
    lastErrorText_ = error;
    {
        std::lock_guard lock(mutex_);
        status_.cycles++;
        if (!ok) status_.errors++;
        status_.connected = client_.connected();
        status_.lastError = error;
        lastRequestAt_ = now;
        lastSuccessAt_ = lastSuccess_;
    }
    bool fresh = lastSuccess_ != Clock::time_point{} && after - lastSuccess_ < std::chrono::milliseconds(config_.staleAfterMs());
    publish(fresh, error);
}

}  // namespace simpleplc::modbus
