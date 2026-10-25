#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simpleplc {

/// The four Modbus data areas that make up a process image.
enum class Area : uint8_t { Coil, DiscreteInput, HoldingRegister, InputRegister };

inline constexpr Area kAllAreas[] = {Area::Coil, Area::DiscreteInput, Area::HoldingRegister,
                                     Area::InputRegister};

/// "coil", "discrete_input", "holding_register", "input_register"
std::string_view areaName(Area area);
std::optional<Area> parseArea(std::string_view name);

constexpr bool isBitArea(Area area) {
    return area == Area::Coil || area == Area::DiscreteInput;
}

/// Coils and holding registers can be written by Modbus and OPC UA clients; the
/// input areas only by the PLC program, the API and forcing.
constexpr bool isClientWritable(Area area) {
    return area == Area::Coil || area == Area::HoldingRegister;
}

/// Data type of a tag. 32-bit types occupy two registers, high word first.
enum class TagType : uint8_t { Bool, UInt16, Int16, UInt32, Int32, Float32 };

/// "bool", "uint16", "int16", "uint32", "int32", "float32"
std::string_view tagTypeName(TagType type);
std::optional<TagType> parseTagType(std::string_view name);

/// Number of entries (bits or registers) a tag of this type occupies.
constexpr uint32_t tagWidth(TagType type) {
    return type == TagType::UInt32 || type == TagType::Int32 || type == TagType::Float32 ? 2 : 1;
}

/// Default type for an area: bool for bit areas, uint16 for registers.
constexpr TagType defaultTagType(Area area) {
    return isBitArea(area) ? TagType::Bool : TagType::UInt16;
}

struct TagDef {
    std::string name;
    Area area = Area::Coil;
    uint32_t address = 0;
    TagType type = TagType::Bool;
    std::string description;
    /// Substitute value: written to the tag when the PLC stops or faults (Siemens: substitute value); none: left as it is
    std::optional<double> substitute;
    /// Retentive: the value is kept in retentive memory (`<data>/retain.json`) and restored at power-up
    bool retain = false;

    bool operator==(const TagDef&) const = default;
};

/// Tag and task names: a letter or underscore, then letters, digits and underscores; at most 64.
bool isValidName(std::string_view name);

/// Problems with one tag in an image of `size` entries per area; empty when valid.
std::string validateTag(const TagDef& tag, uint32_t size);

/// Problems with a whole tag table: each tag, names used twice (case does not matter) and tags
/// that share an address; empty when valid.
std::vector<std::string> validateTags(const std::vector<TagDef>& tags, uint32_t size);

/// Tag value from its raw entries (`tagWidth(type)` words, high word first).
double decodeTagValue(TagType type, const uint16_t* words);

/// Raw entries for a tag value; returns an error message when the value does not fit the type.
std::optional<std::string> encodeTagValue(TagType type, double value, uint16_t* words);

}  // namespace simpleplc
