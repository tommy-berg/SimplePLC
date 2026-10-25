#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/pi_mutex.h"
#include "core/tag.h"

namespace simpleplc {

/// Who made a change; shown in the panel and the API. Substitute: a tag's substitute value, written at STOP or FAULT.
enum class Origin : uint8_t { None, Program, Modbus, OpcUa, Api, Force, Link, Substitute, Retain };

std::string_view originName(Origin origin);

/// One forced entry. A force belongs to the tag it was set on (`tag`, empty for an entry no tag
/// covered) and follows that tag when the tag table changes.
struct ForceEntry {
    Area area;
    uint32_t address;
    uint16_t value;
    std::string tag;
};

struct Change {
    Area area;
    uint32_t address;
    uint16_t value;
};

struct TagRead {
    TagType type;
    double value;
};

/// Raw entries of one tag for the cycle's exchange: `words` entries from `address` (bits: 1).
struct Slot {
    Area area = Area::Coil;
    uint32_t address = 0;
    uint8_t words = 1;
};

/**
 * The PLC's data: four areas of `size` entries (coils, discrete inputs, holding
 * registers, input registers) plus the tag table that names them.
 *
 * This is the only data store. Modbus, OPC UA, the programs, the panel and the
 * API all read and write through it, so a change made anywhere is visible
 * everywhere. Every method is thread-safe; each call holds one internal lock (with
 * priority inheritance) for a few microseconds, never allocating while it holds
 * it, and multi-entry calls are atomic. A cycle exchanges all its tags under one
 * lock (readSlots, writeSlots).
 *
 * Bits are stored as 0/1. A forced entry keeps its forced value and ignores all
 * writes until it is released; released in STOP or FAULT, it gets its tag's substitute value.
 */
class ProcessImage {
public:
    static constexpr uint32_t kMaxSize = 65536;

    /// `size` entries per area, 1 to 65536.
    explicit ProcessImage(uint32_t size);

    ProcessImage(const ProcessImage&) = delete;
    ProcessImage& operator=(const ProcessImage&) = delete;

    uint32_t size() const { return size_; }
    bool contains(uint64_t address, uint64_t count = 1) const {
        return count >= 1 && address + count <= size_;
    }

    // Raw entries. These throw std::out_of_range outside the image.
    uint16_t read(Area area, uint32_t address) const;
    void read(Area area, uint32_t address, std::span<uint16_t> out) const;
    void write(Area area, uint32_t address, uint16_t value, Origin origin);
    void write(Area area, uint32_t address, std::span<const uint16_t> values, Origin origin);
    /// A client's write of raw entries (Modbus, the API): checked and written under one lock. Refused,
    /// with nothing written, when it would make a float32 tag NaN or infinite; a tag the write covers
    /// only half of is judged with its other word as the image holds it.
    std::expected<void, std::string> writeChecked(Area area, uint32_t address, std::span<const uint16_t> values,
                                                  Origin origin);
    /// Modbus FC22 on a holding register: (current AND andMask) OR (orMask AND NOT andMask), read and
    /// written under one lock, refused like writeChecked().
    std::expected<void, std::string> maskWrite(uint32_t address, uint16_t andMask, uint16_t orMask, Origin origin);

    /// The cycle's copy-in: the raw entries of `slots`, consecutively into `out`, under one lock.
    /// `origins`, when given, receives who last wrote the first entry of each slot.
    void readSlots(std::span<const Slot> slots, uint16_t* out, Origin* origins = nullptr) const;
    /// The cycle's copy-out: writes the raw entries of the slots whose `mask` is set, under one lock.
    /// Forced entries keep their forced value. Slots must be inside the image (checked when bound).
    /// Writes nothing and returns false when the tag table is no longer `tagsVersion` (the slots were
    /// resolved against an older one), or when a program writes while the PLC is in FAULT.
    bool writeSlots(std::span<const Slot> slots, const uint16_t* values, const uint8_t* mask, Origin origin,
                    uint64_t tagsVersion);

    /// FAULT: in one step under the lock, every tag with a substitute value gets it (Origin::Substitute), and from now
    /// on program writes (Origin::Program in writeSlots) are refused, from every task, until clearFault().
    void enterFault();
    void clearFault();
    bool faulted() const { return faulted_.load(); }
    /// STOP: every tag with a substitute value gets it (Origin::Substitute), under one lock, and until
    /// leaveSafeState() a released force gives its tag the substitute value.
    void enterSafeState();
    /// RUN: the program writes the outputs again.
    void leaveSafeState();
    /// STOP or FAULT: the outputs hold their substitute values.
    bool inSafeState() const { return stopped_.load() || faulted_.load(); }

    // Tag table
    std::vector<TagDef> tags() const;
    std::optional<TagDef> findTag(std::string_view name) const;
    /// Validates and replaces the whole table; returns the problems and keeps the old table on error.
    /// Forces follow their tags: a tag that moved takes its force along, and the force of a tag that
    /// is gone or changed type is released. `forceChanges`, when given, receives a line for each.
    std::vector<std::string> setTags(std::vector<TagDef> tags, std::vector<std::string>* forceChanges = nullptr);
    /// Increases whenever the tag table changes.
    uint64_t tagsVersion() const { return tagsVersion_.load(); }

    // Tag values (bools as 0/1)
    std::expected<TagRead, std::string> readTag(std::string_view name) const;
    double readTag(const TagDef& tag) const;
    std::expected<void, std::string> writeTag(std::string_view name, double value, Origin origin);
    std::expected<void, std::string> writeTag(const TagDef& tag, double value, Origin origin);
    /// Writes several tags atomically: either all values are valid and written, or none.
    std::expected<void, std::string> writeTags(const std::vector<std::pair<std::string, double>>& values,
                                               Origin origin);

    // Forcing
    /// Forces one entry of `tag` (empty: the first tag that covers the entry, if any).
    void force(Area area, uint32_t address, uint16_t value, std::string_view tag = {});
    /// Releases a forced entry; in STOP or FAULT it then gets its tag's substitute value.
    bool release(Area area, uint32_t address);
    void releaseAll();
    /// Every forced entry with its value and tag, sorted by area and address.
    std::vector<ForceEntry> forces() const;
    size_t forceCount() const;
    bool isForced(Area area, uint32_t address) const;
    /// Increases whenever an entry is forced or released (also by a tag change that moves or releases a
    /// force). With version() and tagsVersion(), tells a reader whether anything it shows has changed.
    uint64_t forcesVersion() const { return forcesVersion_.load(); }

    /// Links: marks the entries of `slots` as holding values that a link could not refresh (its device
    /// does not answer), or as fresh again. OPC UA reports stale entries as Uncertain.
    void setStale(std::span<const Slot> slots, bool stale);
    bool isStale(Area area, uint32_t address) const;

    // Change tracking
    /// Increases on every write that changes a value.
    uint64_t version() const { return version_.load(); }
    Origin lastOrigin(Area area, uint32_t address) const;

private:
    struct AreaData {
        std::vector<uint16_t> values;
        std::vector<uint64_t> changedAt;  // version of the last change
        std::vector<Origin> origin;
        std::vector<uint8_t> forced;
        std::vector<uint8_t> stale;       // a link could not refresh it (setStale)
    };

    void check(uint64_t address, uint64_t count) const;
    void writeLocked(Area area, uint32_t address, uint16_t value, Origin origin);
    double readTagLocked(const TagDef& tag) const;
    /// The float32 tag that `values` from `address` would make NaN or infinite (with the other words
    /// as the image holds them), or nullptr. Under the lock.
    const TagDef* nonFiniteFloatLocked(Area area, uint32_t address, std::span<const uint16_t> values) const;
    /// A released entry in STOP or FAULT: the substitute value of `tag` (or of the first tag with a substitute value
    /// that covers it) for that entry. Under the lock.
    void applySubstituteLocked(Area area, uint32_t address, std::string_view tag);
    /// The first tag of the current table that covers the entry, or nullptr. Under the lock.
    const TagDef* coveringTagLocked(Area area, uint32_t address) const;
    AreaData& data(Area area) { return areas_[static_cast<size_t>(area)]; }
    const AreaData& data(Area area) const { return areas_[static_cast<size_t>(area)]; }

    const uint32_t size_;
    mutable PiMutex mutex_;
    std::array<AreaData, 4> areas_;
    std::shared_ptr<const std::vector<TagDef>> tags_ = std::make_shared<const std::vector<TagDef>>();
    std::unordered_map<std::string, size_t> tagIndex_;
    std::vector<ForceEntry> forced_;  // where entries are forced (value filled in by forces())
    std::atomic<uint64_t> version_{0};
    std::atomic<uint64_t> tagsVersion_{0};
    std::atomic<uint64_t> forcesVersion_{0};  // changed under mutex_
    std::atomic<bool> faulted_{false};  // changed under mutex_; read without it for status
    std::atomic<bool> stopped_{false};  // STOP (enterSafeState); changed under mutex_
};

}  // namespace simpleplc
