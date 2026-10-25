#include "core/process_image.h"

#include <cmath>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace simpleplc {

std::string_view originName(Origin origin) {
    switch (origin) {
        case Origin::None:    return "";
        case Origin::Program: return "program";
        case Origin::Modbus:  return "modbus";
        case Origin::OpcUa:   return "opcua";
        case Origin::Api:     return "api";
        case Origin::Force:   return "force";
        case Origin::Link:    return "link";
        case Origin::Substitute: return "substitute";
        case Origin::Retain:  return "retain";
    }
    return "";
}

namespace {

/// Whether a float32 whose high word (sign, exponent, top of the mantissa) is `high` is finite: it is
/// infinite or NaN exactly when its exponent bits are all ones, whatever the low word holds.
bool finiteFloatHigh(uint16_t high) { return (high & 0x7F80) != 0x7F80; }

}  // namespace

ProcessImage::ProcessImage(uint32_t size) : size_(size) {
    if (size < 1 || size > kMaxSize) {
        throw std::invalid_argument(std::format("image size {} is outside 1-{}", size, kMaxSize));
    }
    for (AreaData& area : areas_) {
        area.values.assign(size, 0);
        area.changedAt.assign(size, 0);
        area.origin.assign(size, Origin::None);
        area.forced.assign(size, 0);
        area.stale.assign(size, 0);
    }
}

void ProcessImage::check(uint64_t address, uint64_t count) const {
    if (!contains(address, count)) {
        throw std::out_of_range(std::format("address {} (count {}) is outside the image (0-{})",
                                            address, count, size_ - 1));
    }
}

uint16_t ProcessImage::read(Area area, uint32_t address) const {
    check(address, 1);
    std::lock_guard lock(mutex_);
    return data(area).values[address];
}

void ProcessImage::read(Area area, uint32_t address, std::span<uint16_t> out) const {
    check(address, out.size());
    std::lock_guard lock(mutex_);
    const auto& values = data(area).values;
    std::copy_n(values.begin() + address, out.size(), out.begin());
}

void ProcessImage::writeLocked(Area area, uint32_t address, uint16_t value, Origin origin) {
    AreaData& d = data(area);
    if (d.forced[address] && origin != Origin::Force) return;
    if (isBitArea(area)) value = value != 0 ? 1 : 0;
    d.origin[address] = origin;
    if (d.values[address] == value) return;
    d.values[address] = value;
    d.changedAt[address] = version_.load() + 1;
    version_.fetch_add(1);
}

void ProcessImage::write(Area area, uint32_t address, uint16_t value, Origin origin) {
    check(address, 1);
    std::lock_guard lock(mutex_);
    writeLocked(area, address, value, origin);
}

void ProcessImage::write(Area area, uint32_t address, std::span<const uint16_t> values,
                         Origin origin) {
    check(address, values.size());
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < values.size(); i++) {
        writeLocked(area, address + static_cast<uint32_t>(i), values[i], origin);
    }
}

const TagDef* ProcessImage::nonFiniteFloatLocked(Area area, uint32_t address, std::span<const uint16_t> values) const {
    // A float32 tag is NaN or infinite exactly when its high word (the first, at the tag's address) has
    // all exponent bits set, whatever its low word holds: the write decides when it covers that word
    // (and the word is not forced); otherwise the word already in the image stays and so does the verdict
    if (isBitArea(area) || values.empty()) return nullptr;
    uint64_t end = uint64_t{address} + values.size();
    const AreaData& d = data(area);
    for (const TagDef& t : *tags_) {
        if (t.type != TagType::Float32 || t.area != area || t.address < address || t.address >= end) continue;
        if (d.forced[t.address]) continue;
        if (!finiteFloatHigh(values[t.address - address])) return &t;
    }
    return nullptr;
}

std::expected<void, std::string> ProcessImage::writeChecked(Area area, uint32_t address, std::span<const uint16_t> values,
                                                            Origin origin) {
    check(address, values.size());
    std::shared_ptr<const std::vector<TagDef>> keep;  // the refused tag's table, for the message
    const TagDef* refused = nullptr;
    {
        std::lock_guard lock(mutex_);
        refused = nonFiniteFloatLocked(area, address, values);
        if (!refused) {
            for (size_t i = 0; i < values.size(); i++) writeLocked(area, address + static_cast<uint32_t>(i), values[i], origin);
            return {};
        }
        keep = tags_;
    }
    return std::unexpected(std::format("tag '{}' (float32 at {} {}-{}) would become NaN or infinite; nothing was written",
                                       refused->name, areaName(area), refused->address, refused->address + 1));
}

std::expected<void, std::string> ProcessImage::maskWrite(uint32_t address, uint16_t andMask, uint16_t orMask, Origin origin) {
    check(address, 1);
    std::shared_ptr<const std::vector<TagDef>> keep;
    const TagDef* refused = nullptr;
    {
        std::lock_guard lock(mutex_);
        uint16_t current = data(Area::HoldingRegister).values[address];
        auto result = static_cast<uint16_t>((current & andMask) | (orMask & ~andMask));
        refused = nonFiniteFloatLocked(Area::HoldingRegister, address, std::span<const uint16_t>(&result, 1));
        if (!refused) {
            writeLocked(Area::HoldingRegister, address, result, origin);
            return {};
        }
        keep = tags_;
    }
    return std::unexpected(std::format("tag '{}' (float32 at holding_register {}-{}) would become NaN or infinite; nothing was written",
                                       refused->name, refused->address, refused->address + 1));
}

void ProcessImage::readSlots(std::span<const Slot> slots, uint16_t* out, Origin* origins) const {
    std::lock_guard lock(mutex_);
    for (const Slot& s : slots) {
        const AreaData& d = data(s.area);
        for (uint8_t w = 0; w < s.words; w++) *out++ = d.values[s.address + w];
        if (origins) *origins++ = d.origin[s.address];
    }
}

bool ProcessImage::writeSlots(std::span<const Slot> slots, const uint16_t* values, const uint8_t* mask,
                              Origin origin, uint64_t tagsVersion) {
    std::lock_guard lock(mutex_);
    if (tagsVersion_.load() != tagsVersion || (origin == Origin::Program && faulted_.load())) return false;
    for (size_t i = 0; i < slots.size(); i++) {
        const Slot& s = slots[i];
        if (mask[i]) {
            for (uint8_t w = 0; w < s.words; w++) writeLocked(s.area, s.address + w, values[w], origin);
        }
        values += s.words;
    }
    return true;
}

void ProcessImage::enterFault() {
    std::shared_ptr<const std::vector<TagDef>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = tags_;
    }
    // Encoded outside the lock (validated when the tags were set); written under one lock
    std::vector<std::pair<const TagDef*, std::array<uint16_t, 2>>> values;
    for (const TagDef& t : *snapshot) {
        std::array<uint16_t, 2> w{};
        if (t.substitute && !encodeTagValue(t.type, *t.substitute, w.data())) values.push_back({&t, w});
    }
    std::lock_guard lock(mutex_);
    faulted_ = true;  // before the values: no program write can come between them
    for (const auto& [t, w] : values) {
        if (!contains(t->address, tagWidth(t->type))) continue;
        for (uint32_t i = 0; i < tagWidth(t->type); i++) writeLocked(t->area, t->address + i, w[i], Origin::Substitute);
    }
}

void ProcessImage::clearFault() {
    std::lock_guard lock(mutex_);
    faulted_ = false;
}

void ProcessImage::enterSafeState() {
    std::shared_ptr<const std::vector<TagDef>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = tags_;
    }
    std::vector<std::pair<const TagDef*, std::array<uint16_t, 2>>> values;
    for (const TagDef& t : *snapshot) {
        std::array<uint16_t, 2> w{};
        if (t.substitute && !encodeTagValue(t.type, *t.substitute, w.data())) values.push_back({&t, w});
    }
    std::lock_guard lock(mutex_);
    stopped_ = true;
    for (const auto& [t, w] : values) {
        if (!contains(t->address, tagWidth(t->type))) continue;
        for (uint32_t i = 0; i < tagWidth(t->type); i++) writeLocked(t->area, t->address + i, w[i], Origin::Substitute);
    }
}

void ProcessImage::leaveSafeState() {
    std::lock_guard lock(mutex_);
    stopped_ = false;
}

std::vector<TagDef> ProcessImage::tags() const {
    std::shared_ptr<const std::vector<TagDef>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = tags_;
    }
    return *snapshot;  // copied outside the lock
}

std::optional<TagDef> ProcessImage::findTag(std::string_view name) const {
    std::string key(name);
    std::shared_ptr<const std::vector<TagDef>> snapshot;
    size_t index = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = tagIndex_.find(key);
        if (it == tagIndex_.end()) return std::nullopt;
        snapshot = tags_;
        index = it->second;
    }
    return (*snapshot)[index];
}

std::vector<std::string> ProcessImage::setTags(std::vector<TagDef> tags, std::vector<std::string>* forceChanges) {
    std::vector<std::string> problems = validateTags(tags, size_);
    if (!problems.empty()) return problems;

    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < tags.size(); i++) index.emplace(tags[i].name, i);
    auto fresh = std::make_shared<const std::vector<TagDef>>(std::move(tags));

    // What happened to forces, formatted after the lock
    struct Moved {
        std::string tag;
        Area fromArea, toArea;
        uint32_t from, to;
        bool released;
    };
    std::vector<Moved> changes;
    std::shared_ptr<const std::vector<TagDef>> old;  // released after the lock
    std::unordered_map<std::string, size_t> oldIndex;
    {
        std::lock_guard lock(mutex_);
        if (*fresh == *tags_) return {};
        old = std::exchange(tags_, std::move(fresh));
        oldIndex = std::exchange(tagIndex_, std::move(index));
        tagsVersion_.fetch_add(1);

        // Forces follow their tags (an operator forced a tag, not an address): a tag that moved takes
        // its force along; the force of a tag that is gone or has another type is released
        struct Move {
            size_t entry;
            Area area;
            uint32_t address;
            uint16_t value;
        };
        std::vector<Move> moves;
        std::vector<size_t> releases;
        for (size_t i = 0; i < forced_.size(); i++) {
            const ForceEntry& f = forced_[i];
            if (f.tag.empty()) continue;  // an entry no tag covered: it stays where it is
            auto was = oldIndex.find(f.tag);
            auto now = tagIndex_.find(f.tag);
            const TagDef* before = was != oldIndex.end() ? &(*old)[was->second] : nullptr;
            const TagDef* after = now != tagIndex_.end() ? &(*tags_)[now->second] : nullptr;
            if (!before || !after || before->type != after->type) {
                releases.push_back(i);
                continue;
            }
            if (before->area == after->area && before->address == after->address) continue;
            uint32_t offset = f.address - before->address;
            moves.push_back({i, after->area, after->address + offset, data(f.area).values[f.address]});
        }
        // Every old entry is released first, then the moved forces set: a tag may move onto its own old
        // words. An old entry that no force covers now is free; in STOP or FAULT it gets a substitute value
        struct Freed {
            Area area;
            uint32_t address;
            std::string_view tag;
        };
        std::vector<Freed> freed;
        for (size_t i : releases) {
            ForceEntry& f = forced_[i];
            data(f.area).forced[f.address] = 0;
            freed.push_back({f.area, f.address, f.tag});
            changes.push_back({f.tag, f.area, f.area, f.address, f.address, true});
        }
        for (const Move& m : moves) {
            ForceEntry& f = forced_[m.entry];
            data(f.area).forced[f.address] = 0;
            freed.push_back({f.area, f.address, {}});  // the tag left it: whichever tag covers it now
        }
        for (const Move& m : moves) {
            ForceEntry& f = forced_[m.entry];
            changes.push_back({f.tag, f.area, m.area, f.address, m.address, false});
            f.area = m.area;
            f.address = m.address;
            data(m.area).forced[m.address] = 1;
            writeLocked(m.area, m.address, m.value, Origin::Force);
        }
        for (const Freed& e : freed) {
            if (!data(e.area).forced[e.address]) applySubstituteLocked(e.area, e.address, e.tag);
        }
        // Released entries leave the list; two forces moved onto one entry stay one
        std::vector<uint8_t> drop(forced_.size(), 0);
        for (size_t i : releases) drop[i] = 1;
        for (size_t i = 0; i < forced_.size(); i++) {
            for (size_t j = 0; j < i && !drop[i]; j++) {
                if (!drop[j] && forced_[j].area == forced_[i].area && forced_[j].address == forced_[i].address) drop[i] = 1;
            }
        }
        size_t kept = 0;
        for (size_t i = 0; i < forced_.size(); i++) {
            if (drop[i]) continue;
            if (kept != i) forced_[kept] = std::move(forced_[i]);  // never onto itself: that empties a string
            kept++;
        }
        forced_.resize(kept);
        if (!releases.empty() || !moves.empty()) forcesVersion_.fetch_add(1);
    }
    if (forceChanges) {
        for (const Moved& c : changes) {
            forceChanges->push_back(
                c.released ? std::format("force on tag '{}' ({} {}) released: the tag is gone or has another type", c.tag,
                                         areaName(c.fromArea), c.from)
                           : std::format("force on tag '{}' moved with the tag from {} {} to {} {}", c.tag, areaName(c.fromArea),
                                         c.from, areaName(c.toArea), c.to));
        }
    }
    return {};
}

const TagDef* ProcessImage::coveringTagLocked(Area area, uint32_t address) const {
    for (const TagDef& t : *tags_) {
        if (t.area == area && address >= t.address && address < t.address + tagWidth(t.type)) return &t;
    }
    return nullptr;
}

void ProcessImage::applySubstituteLocked(Area area, uint32_t address, std::string_view tag) {
    if (!stopped_.load() && !faulted_.load()) return;
    const TagDef* owner = nullptr;
    if (!tag.empty()) {
        auto it = tagIndex_.find(std::string(tag));
        if (it != tagIndex_.end()) {
            const TagDef& t = (*tags_)[it->second];
            if (t.area == area && address >= t.address && address < t.address + tagWidth(t.type)) owner = &t;
        }
    }
    if (!owner || !owner->substitute) {
        owner = nullptr;
        for (const TagDef& t : *tags_) {
            if (t.substitute && t.area == area && address >= t.address && address < t.address + tagWidth(t.type)) {
                owner = &t;
                break;
            }
        }
    }
    if (!owner || !owner->substitute) return;
    uint16_t words[2] = {0, 0};
    if (encodeTagValue(owner->type, *owner->substitute, words)) return;  // validated with the table; cannot happen
    writeLocked(area, address, words[address - owner->address], Origin::Substitute);
}

double ProcessImage::readTagLocked(const TagDef& tag) const {
    const auto& values = data(tag.area).values;
    uint16_t words[2] = {values[tag.address], 0};
    if (tagWidth(tag.type) == 2) words[1] = values[tag.address + 1];
    return decodeTagValue(tag.type, words);
}

std::expected<TagRead, std::string> ProcessImage::readTag(std::string_view name) const {
    std::string key(name);
    TagRead read{};
    {
        std::lock_guard lock(mutex_);
        auto it = tagIndex_.find(key);
        if (it != tagIndex_.end()) {
            const TagDef& tag = (*tags_)[it->second];
            read = TagRead{tag.type, readTagLocked(tag)};
            return read;
        }
    }
    return std::unexpected(std::format("unknown tag '{}'", name));
}

double ProcessImage::readTag(const TagDef& tag) const {
    check(tag.address, tagWidth(tag.type));
    std::lock_guard lock(mutex_);
    return readTagLocked(tag);
}

std::expected<void, std::string> ProcessImage::writeTag(const TagDef& tag, double value,
                                                        Origin origin) {
    check(tag.address, tagWidth(tag.type));
    uint16_t words[2] = {0, 0};
    if (auto error = encodeTagValue(tag.type, value, words)) {
        return std::unexpected(std::format("tag '{}': {}", tag.name, *error));
    }
    std::lock_guard lock(mutex_);
    for (uint32_t i = 0; i < tagWidth(tag.type); i++) {
        writeLocked(tag.area, tag.address + i, words[i], origin);
    }
    return {};
}

std::expected<void, std::string> ProcessImage::writeTag(std::string_view name, double value,
                                                        Origin origin) {
    return writeTags({{std::string(name), value}}, origin);
}

std::expected<void, std::string> ProcessImage::writeTags(
    const std::vector<std::pair<std::string, double>>& values, Origin origin) {
    struct Pending {
        Area area;
        uint32_t address;
        uint32_t width;
        uint16_t words[2];
    };
    std::vector<Pending> pending;
    pending.reserve(values.size());
    std::string problem;  // formatted after the lock is released

    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, value] : values) {
            auto it = tagIndex_.find(name);
            if (it == tagIndex_.end()) {
                problem = name;
                break;
            }
            const TagDef& tag = (*tags_)[it->second];
            Pending p{tag.area, tag.address, tagWidth(tag.type), {0, 0}};
            if (encodeTagValue(tag.type, value, p.words)) {
                problem = name;
                break;
            }
            pending.push_back(p);
        }
        if (problem.empty()) {
            for (const Pending& p : pending) {
                for (uint32_t i = 0; i < p.width; i++) writeLocked(p.area, p.address + i, p.words[i], origin);
            }
            return {};
        }
    }
    // Report the first problem (looked up again without the lock)
    auto tag = findTag(problem);
    if (!tag) return std::unexpected(std::format("unknown tag '{}'", problem));
    for (const auto& [name, value] : values) {
        if (name != problem) continue;
        uint16_t words[2];
        if (auto error = encodeTagValue(tag->type, value, words)) return std::unexpected(std::format("tag '{}': {}", name, *error));
    }
    return std::unexpected(std::format("tag '{}' changed while it was written", problem));
}

void ProcessImage::force(Area area, uint32_t address, uint16_t value, std::string_view tag) {
    check(address, 1);
    std::string owner(tag);
    // Forcing is an operator's action, not the cycle path: the few allocations under the lock are fine
    std::lock_guard lock(mutex_);
    if (owner.empty()) {
        if (const TagDef* t = coveringTagLocked(area, address)) owner = t->name;
    }
    AreaData& d = data(area);
    if (!d.forced[address]) {
        d.forced[address] = 1;
        forced_.push_back({area, address, 0, std::move(owner)});
        forcesVersion_.fetch_add(1);
    } else if (!owner.empty()) {
        for (ForceEntry& f : forced_) {
            if (f.area == area && f.address == address && f.tag != owner) {
                f.tag = owner;
                forcesVersion_.fetch_add(1);
            }
        }
    }
    writeLocked(area, address, value, Origin::Force);
}

bool ProcessImage::release(Area area, uint32_t address) {
    check(address, 1);
    std::string tag;  // freed after the lock
    std::lock_guard lock(mutex_);
    AreaData& d = data(area);
    if (!d.forced[address]) return false;
    d.forced[address] = 0;
    forcesVersion_.fetch_add(1);
    for (auto it = forced_.begin(); it != forced_.end(); ++it) {
        if (it->area == area && it->address == address) {
            tag = std::move(it->tag);
            forced_.erase(it);
            break;
        }
    }
    // Nothing justifies the forced value any more: in STOP or FAULT the entry holds its substitute value
    applySubstituteLocked(area, address, tag);
    return true;
}

void ProcessImage::releaseAll() {
    std::vector<ForceEntry> released;  // freed after the lock
    std::lock_guard lock(mutex_);
    if (forced_.empty()) return;
    for (const ForceEntry& f : forced_) data(f.area).forced[f.address] = 0;
    for (const ForceEntry& f : forced_) applySubstituteLocked(f.area, f.address, f.tag);
    released.swap(forced_);
    forcesVersion_.fetch_add(1);
}

std::vector<ForceEntry> ProcessImage::forces() const {
    std::lock_guard lock(mutex_);
    std::vector<ForceEntry> out = forced_;
    for (ForceEntry& f : out) f.value = data(f.area).values[f.address];
    std::sort(out.begin(), out.end(), [](const ForceEntry& a, const ForceEntry& b) {
        return a.area != b.area ? a.area < b.area : a.address < b.address;
    });
    return out;
}

size_t ProcessImage::forceCount() const {
    std::lock_guard lock(mutex_);
    return forced_.size();
}

void ProcessImage::setStale(std::span<const Slot> slots, bool stale) {
    std::lock_guard lock(mutex_);
    for (const Slot& s : slots) {
        if (!contains(s.address, s.words)) continue;
        for (uint8_t w = 0; w < s.words; w++) data(s.area).stale[s.address + w] = stale ? 1 : 0;
    }
}

bool ProcessImage::isStale(Area area, uint32_t address) const {
    check(address, 1);
    std::lock_guard lock(mutex_);
    return data(area).stale[address] != 0;
}

bool ProcessImage::isForced(Area area, uint32_t address) const {
    check(address, 1);
    std::lock_guard lock(mutex_);
    return data(area).forced[address] != 0;
}

Origin ProcessImage::lastOrigin(Area area, uint32_t address) const {
    check(address, 1);
    std::lock_guard lock(mutex_);
    return data(area).origin[address];
}

}  // namespace simpleplc
