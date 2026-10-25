#include "core/tag.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_map>

namespace simpleplc {

std::string_view areaName(Area area) {
    switch (area) {
        case Area::Coil:            return "coil";
        case Area::DiscreteInput:   return "discrete_input";
        case Area::HoldingRegister: return "holding_register";
        case Area::InputRegister:   return "input_register";
    }
    return "coil";
}

std::optional<Area> parseArea(std::string_view name) {
    for (Area area : kAllAreas) {
        if (areaName(area) == name) return area;
    }
    // Short forms people type
    if (name == "coils") return Area::Coil;
    if (name == "discrete_inputs" || name == "input") return Area::DiscreteInput;
    if (name == "holding" || name == "holding_registers") return Area::HoldingRegister;
    if (name == "input_registers") return Area::InputRegister;
    return std::nullopt;
}

std::string_view tagTypeName(TagType type) {
    switch (type) {
        case TagType::Bool:    return "bool";
        case TagType::UInt16:  return "uint16";
        case TagType::Int16:   return "int16";
        case TagType::UInt32:  return "uint32";
        case TagType::Int32:   return "int32";
        case TagType::Float32: return "float32";
    }
    return "bool";
}

std::optional<TagType> parseTagType(std::string_view name) {
    for (TagType type : {TagType::Bool, TagType::UInt16, TagType::Int16, TagType::UInt32,
                         TagType::Int32, TagType::Float32}) {
        if (tagTypeName(type) == name) return type;
    }
    if (name == "float" || name == "real") return TagType::Float32;
    if (name == "boolean") return TagType::Bool;
    return std::nullopt;
}

bool isValidName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    auto isAlpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!isAlpha(name[0])) return false;
    for (char c : name) {
        if (!isAlpha(c) && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

std::string validateTag(const TagDef& tag, uint32_t size) {
    if (!isValidName(tag.name)) {
        return std::format("tag name '{}' is invalid: use letters, digits and _, starting with a "
                           "letter or _, at most 64 characters",
                           tag.name);
    }
    // Programs bind tags by name (VAR_EXTERNAL), so a tag name follows IEC 61131-3's rules for names
    if (tag.name.find("__") != std::string::npos) {
        return std::format("tag name '{}' has two underscores in a row, which Structured Text does not allow "
                           "in a name, so no program could use the tag; use single underscores", tag.name);
    }
    if (tag.name.back() == '_') {
        return std::format("tag name '{}' ends with '_', which Structured Text does not allow in a name, so no "
                           "program could use the tag; remove the last '_'", tag.name);
    }
    if (isBitArea(tag.area) && tag.type != TagType::Bool) {
        return std::format("tag '{}': {} entries are single bits, the type must be bool", tag.name,
                           areaName(tag.area));
    }
    if (!isBitArea(tag.area) && tag.type == TagType::Bool) {
        return std::format("tag '{}': {} entries are 16-bit registers, use uint16, int16, uint32, "
                           "int32 or float32",
                           tag.name, areaName(tag.area));
    }
    uint64_t end = static_cast<uint64_t>(tag.address) + tagWidth(tag.type);
    if (end > size) {
        return std::format("tag '{}': address {} ({} entr{}) is outside the {} area (0-{})",
                           tag.name, tag.address, tagWidth(tag.type),
                           tagWidth(tag.type) == 1 ? "y" : "ies", areaName(tag.area), size - 1);
    }
    if (tag.substitute) {
        uint16_t words[2];
        if (auto error = encodeTagValue(tag.type, *tag.substitute, words)) {
            return std::format("tag '{}': substitute value: {}", tag.name, *error);
        }
    }
    if (tag.description.size() > 256) {
        return std::format("tag '{}': description is longer than 256 characters", tag.name);
    }
    return {};
}

std::vector<std::string> validateTags(const std::vector<TagDef>& tags, uint32_t size) {
    std::vector<std::string> problems;
    // Names are compared without case, as programs see them: Level, LEVEL and level are one name
    std::unordered_map<std::string, const TagDef*> names;
    for (const TagDef& tag : tags) {
        if (std::string problem = validateTag(tag, size); !problem.empty()) {
            problems.push_back(std::move(problem));
        }
        std::string key = tag.name;
        for (char& c : key) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        auto [it, fresh] = names.emplace(key, &tag);
        if (fresh) continue;
        if (it->second->name == tag.name) {
            problems.push_back(std::format("tag name '{}' is used more than once", tag.name));
        } else {
            problems.push_back(std::format("tag names '{}' and '{}' are the same name: case does not matter in names, "
                                           "so programs could not tell them apart; rename one", it->second->name, tag.name));
        }
    }
    // One address belongs to one tag: overlapping tags would let a write to one change the other
    // (a 32-bit tag covers two registers)
    std::vector<const TagDef*> sorted;
    for (const TagDef& tag : tags) sorted.push_back(&tag);
    std::stable_sort(sorted.begin(), sorted.end(), [](const TagDef* a, const TagDef* b) {
        return a->area != b->area ? a->area < b->area : a->address < b->address;
    });
    auto range = [](const TagDef& t) {
        uint32_t width = tagWidth(t.type);
        return width == 1 ? std::format("{} {}", areaName(t.area), t.address)
                          : std::format("{} {}-{}", areaName(t.area), t.address, t.address + width - 1);
    };
    const TagDef* reach = nullptr;  // the tag of this area that reaches furthest so far
    for (const TagDef* tag : sorted) {
        if (reach && reach->area == tag->area &&
            static_cast<uint64_t>(reach->address) + tagWidth(reach->type) > tag->address) {
            problems.push_back(std::format("tag '{}' ({}) overlaps tag '{}' ({}): each address belongs to one tag; "
                                           "move one of them", tag->name, range(*tag), reach->name, range(*reach)));
        }
        if (!reach || reach->area != tag->area ||
            static_cast<uint64_t>(tag->address) + tagWidth(tag->type) >
                static_cast<uint64_t>(reach->address) + tagWidth(reach->type)) {
            reach = tag;
        }
    }
    return problems;
}

namespace {

/// The double closest to the float's shortest decimal form, so 23.45f reads as 23.45
/// everywhere (JSON, OPC UA conversions) instead of 23.450000762939453.
double shortestDouble(float value) {
    char buffer[32];
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
    if (ec != std::errc()) return static_cast<double>(value);
    *end = '\0';
    return std::strtod(buffer, nullptr);
}

}  // namespace

double decodeTagValue(TagType type, const uint16_t* words) {
    uint32_t pair = (static_cast<uint32_t>(words[0]) << 16) |
                    (tagWidth(type) == 2 ? static_cast<uint32_t>(words[1]) : 0u);
    switch (type) {
        case TagType::Bool:    return words[0] != 0 ? 1.0 : 0.0;
        case TagType::UInt16:  return words[0];
        case TagType::Int16:   return static_cast<int16_t>(words[0]);
        case TagType::UInt32:  return pair;
        case TagType::Int32:   return static_cast<int32_t>(pair);
        case TagType::Float32: return shortestDouble(std::bit_cast<float>(pair));
    }
    return 0.0;
}

std::optional<std::string> encodeTagValue(TagType type, double value, uint16_t* words) {
    if (!std::isfinite(value)) {
        return std::string("value must be a finite number");
    }
    auto integerIn = [&](double lo, double hi) -> std::optional<std::string> {
        if (value != std::trunc(value)) {
            return std::format("{} requires a whole number, got {}", tagTypeName(type), value);
        }
        if (value < lo || value > hi) {
            return std::format("{} {} is outside the range {} to {}", tagTypeName(type), value,
                               lo, hi);
        }
        return std::nullopt;
    };
    auto storePair = [&](uint32_t bits) {
        words[0] = static_cast<uint16_t>(bits >> 16);
        words[1] = static_cast<uint16_t>(bits & 0xFFFF);
    };

    switch (type) {
        case TagType::Bool:
            if (value != 0.0 && value != 1.0) {
                return std::format("bool requires true/false or 0/1, got {}", value);
            }
            words[0] = value != 0.0 ? 1 : 0;
            return std::nullopt;
        case TagType::UInt16:
            if (auto e = integerIn(0, 65535)) return e;
            words[0] = static_cast<uint16_t>(value);
            return std::nullopt;
        case TagType::Int16:
            if (auto e = integerIn(-32768, 32767)) return e;
            words[0] = static_cast<uint16_t>(static_cast<int16_t>(value));
            return std::nullopt;
        case TagType::UInt32:
            if (auto e = integerIn(0, 4294967295.0)) return e;
            storePair(static_cast<uint32_t>(value));
            return std::nullopt;
        case TagType::Int32:
            if (auto e = integerIn(-2147483648.0, 2147483647.0)) return e;
            storePair(static_cast<uint32_t>(static_cast<int32_t>(value)));
            return std::nullopt;
        case TagType::Float32: {
            // Rounded to float first (to nearest, ties to even): every double below FLT_MAX + half an
            // ULP becomes at most FLT_MAX, so the decimal "3.4028235e+38" that FLT_MAX prints as is
            // accepted back. 0x1.ffffffp127 = FLT_MAX + 2^103 is the first one that rounds to infinity.
            constexpr double kRoundsToInfinity = 0x1.ffffffp127;
            if (value <= -kRoundsToInfinity || value >= kRoundsToInfinity) {
                return std::format("float32 {} is out of range (the largest is ±{})", value,
                                   std::numeric_limits<float>::max());
            }
            storePair(std::bit_cast<uint32_t>(static_cast<float>(value)));
            return std::nullopt;
        }
    }
    return std::string("unknown type");
}

}  // namespace simpleplc
