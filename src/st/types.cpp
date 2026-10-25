#include "st/types.h"

#include <format>
#include <limits>

namespace simpleplc::st {

namespace {

struct ElementaryInfo {
    std::string_view name;
    uint32_t size;
};

constexpr ElementaryInfo kElementary[] = {
    {"BOOL", 1},
    {"SINT", 1}, {"INT", 2}, {"DINT", 4}, {"LINT", 8},
    {"USINT", 1}, {"UINT", 2}, {"UDINT", 4}, {"ULINT", 8},
    {"BYTE", 1}, {"WORD", 2}, {"DWORD", 4}, {"LWORD", 8},
    {"REAL", 4}, {"LREAL", 8},
    {"TIME", 8}, {"LTIME", 8},
};
constexpr size_t kElementaryCount = sizeof(kElementary) / sizeof(kElementary[0]);

size_t idx(TypeKind k) { return static_cast<size_t>(k); }

}  // namespace

const Field* Type::field(std::string_view upper) const {
    for (const Field& f : fields) {
        if (f.upper == upper) return &f;
    }
    return nullptr;
}

const EnumConstant* Type::enumValue(std::string_view upper) const {
    for (const EnumConstant& v : enumValues) {
        if (v.upper == upper) return &v;
    }
    return nullptr;
}

bool isElementary(TypeKind k) { return idx(k) < kElementaryCount; }
bool isInteger(TypeKind k) { return k >= TypeKind::SInt && k <= TypeKind::ULInt; }
bool isSigned(TypeKind k) { return k >= TypeKind::SInt && k <= TypeKind::LInt; }
bool isUnsigned(TypeKind k) { return k >= TypeKind::USInt && k <= TypeKind::ULInt; }
bool isBitString(TypeKind k) { return k >= TypeKind::Byte && k <= TypeKind::LWord; }
bool isReal(TypeKind k) { return k == TypeKind::Real || k == TypeKind::LReal; }
bool isNumeric(TypeKind k) { return isInteger(k) || isReal(k); }
bool isDuration(TypeKind k) { return k == TypeKind::Time || k == TypeKind::LTime; }
bool isMagnitude(TypeKind k) { return isNumeric(k) || isDuration(k); }

uint32_t bitWidth(TypeKind k) {
    if (k == TypeKind::Bool) return 1;
    if (isInteger(k) || isBitString(k)) return kElementary[idx(k)].size * 8;
    return 0;
}

int64_t minValue(TypeKind k) {
    switch (k) {
        case TypeKind::SInt: return std::numeric_limits<int8_t>::min();
        case TypeKind::Int: return std::numeric_limits<int16_t>::min();
        case TypeKind::DInt: return std::numeric_limits<int32_t>::min();
        case TypeKind::LInt: return std::numeric_limits<int64_t>::min();
        default: return 0;
    }
}

uint64_t maxValue(TypeKind k) {
    switch (k) {
        case TypeKind::Bool: return 1;
        case TypeKind::SInt: return static_cast<uint64_t>(std::numeric_limits<int8_t>::max());
        case TypeKind::Int: return static_cast<uint64_t>(std::numeric_limits<int16_t>::max());
        case TypeKind::DInt: return static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
        case TypeKind::LInt: return static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        case TypeKind::USInt: case TypeKind::Byte: return std::numeric_limits<uint8_t>::max();
        case TypeKind::UInt: case TypeKind::Word: return std::numeric_limits<uint16_t>::max();
        case TypeKind::UDInt: case TypeKind::DWord: return std::numeric_limits<uint32_t>::max();
        case TypeKind::ULInt: case TypeKind::LWord: return std::numeric_limits<uint64_t>::max();
        default: return 0;
    }
}

std::string_view elementaryName(TypeKind k) {
    if (isElementary(k)) return kElementary[idx(k)].name;
    switch (k) {
        case TypeKind::String: return "STRING";
        case TypeKind::Array: return "ARRAY";
        case TypeKind::Struct: return "STRUCT";
        case TypeKind::Enum: return "enumeration";
        case TypeKind::FunctionBlock: return "FUNCTION_BLOCK";
        default: return "(no value)";
    }
}

bool implicitlyConvertible(TypeKind from, TypeKind to) {
    if (from == to) return true;
    using K = TypeKind;
    switch (from) {
        case K::SInt: return to == K::Int || to == K::DInt || to == K::LInt || to == K::Real || to == K::LReal;
        case K::Int: return to == K::DInt || to == K::LInt || to == K::Real || to == K::LReal;
        case K::DInt: return to == K::LInt || to == K::LReal;
        case K::USInt:
            return to == K::UInt || to == K::UDInt || to == K::ULInt || to == K::Int || to == K::DInt ||
                   to == K::LInt || to == K::Real || to == K::LReal;
        case K::UInt:
            return to == K::UDInt || to == K::ULInt || to == K::DInt || to == K::LInt || to == K::Real ||
                   to == K::LReal;
        case K::UDInt: return to == K::ULInt || to == K::LInt || to == K::LReal;
        case K::Byte: return to == K::Word || to == K::DWord || to == K::LWord;
        case K::Word: return to == K::DWord || to == K::LWord;
        case K::DWord: return to == K::LWord;
        case K::Real: return to == K::LReal;
        case K::Time: return to == K::LTime;
        default: return false;
    }
}

TypeTable::TypeTable() {
    for (size_t i = 0; i < kElementaryCount; i++) {
        auto t = std::make_unique<Type>();
        t->kind = static_cast<TypeKind>(i);
        t->name = std::string(kElementary[i].name);
        t->size = kElementary[i].size;
        t->align = kElementary[i].size;
        elementary_[i] = t.get();
        owned_.push_back(std::move(t));
    }
    auto v = std::make_unique<Type>();
    v->kind = TypeKind::Void;
    v->name = "(no value)";
    elementary_[idx(TypeKind::Void)] = v.get();
    owned_.push_back(std::move(v));
}

const Type* TypeTable::elementaryByName(std::string_view upper) const {
    for (size_t i = 0; i < kElementaryCount; i++) {
        if (kElementary[i].name == upper) return elementary_[i];
    }
    return nullptr;
}

const Type* TypeTable::string(uint32_t capacity) {
    for (const auto& t : owned_) {
        if (t->kind == TypeKind::String && t->capacity == capacity) return t.get();
    }
    auto t = std::make_unique<Type>();
    t->kind = TypeKind::String;
    t->capacity = capacity;
    t->name = std::format("STRING[{}]", capacity);
    t->size = 2 + capacity;
    t->align = 2;
    const Type* out = t.get();
    owned_.push_back(std::move(t));
    return out;
}

const Type* TypeTable::array(const Type* element, std::vector<ArrayDim> dims) {
    auto t = std::make_unique<Type>();
    t->kind = TypeKind::Array;
    t->element = element;
    std::string bounds;
    uint64_t count = 1;
    for (const ArrayDim& d : dims) {
        if (!bounds.empty()) bounds += ", ";
        bounds += std::format("{}..{}", d.low, d.high);
        count *= d.count();
    }
    t->name = std::format("ARRAY[{}] OF {}", bounds, element->name);
    uint64_t stride = (static_cast<uint64_t>(element->size) + element->align - 1) / element->align * element->align;
    uint64_t total = stride * count;
    t->size = total > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                            : static_cast<uint32_t>(total);
    t->align = element->align;
    t->dims = std::move(dims);
    const Type* out = t.get();
    owned_.push_back(std::move(t));
    return out;
}

Type* TypeTable::newType() {
    owned_.push_back(std::make_unique<Type>());
    return owned_.back().get();
}

bool sameType(const Type* a, const Type* b) {
    a = baseOf(a);
    b = baseOf(b);
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    if (a->kind == TypeKind::String) return a->capacity == b->capacity;
    if (a->kind == TypeKind::Array) {
        if (a->dims.size() != b->dims.size()) return false;
        for (size_t i = 0; i < a->dims.size(); i++) {
            if (a->dims[i].low != b->dims[i].low || a->dims[i].high != b->dims[i].high) return false;
        }
        return sameType(a->element, b->element);
    }
    return false;
}

}  // namespace simpleplc::st
