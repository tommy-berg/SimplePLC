#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simpleplc::st {

namespace ast {
struct Initializer;
}

/// Kinds of types. The order of the elementary kinds is used by tables in types.cpp.
enum class TypeKind : uint8_t {
    Bool,
    SInt, Int, DInt, LInt,
    USInt, UInt, UDInt, ULInt,
    Byte, Word, DWord, LWord,
    Real, LReal,
    Time, LTime,
    String,
    Array, Struct, Enum, FunctionBlock,
    Void,  ///< a FUNCTION without a result
};

struct Type;
struct PouInfo;

struct Field {
    std::string upper;
    std::string spelled;
    const Type* type = nullptr;
    uint32_t offset = 0;
    ast::Initializer* init = nullptr;  ///< STRUCT member's declared initial value
};

struct EnumConstant {
    std::string upper;
    std::string spelled;
    int64_t value = 0;
};

struct ArrayDim {
    int64_t low = 0;
    int64_t high = 0;
    uint64_t count() const { return static_cast<uint64_t>(high - low) + 1; }
};

/**
 * A type. Types are owned by a TypeTable and compared by pointer, except arrays
 * and strings, which are compared structurally (sameType()). Subranges are a
 * Type with the base's kind and `hasRange`.
 */
struct Type {
    TypeKind kind = TypeKind::Void;
    std::string name;          ///< as shown in messages: INT, STRING[80], ARRAY[1..3] OF INT, Valve
    uint32_t size = 0;         ///< bytes
    uint32_t align = 1;

    // STRING[capacity]: 2-byte length followed by `capacity` bytes
    uint32_t capacity = 0;

    // ARRAY
    const Type* element = nullptr;
    std::vector<ArrayDim> dims;

    // STRUCT and FUNCTION_BLOCK (instance layout)
    std::vector<Field> fields;
    const PouInfo* pou = nullptr;  ///< FUNCTION_BLOCK

    // ENUM
    std::vector<EnumConstant> enumValues;
    const Type* enumBase = nullptr;  ///< integer type holding the value (INT unless declared)

    /// Default value given in TYPE … := value; END_TYPE
    ast::Initializer* defaultInit = nullptr;

    // Subrange of an integer type
    bool hasRange = false;
    int64_t rangeLow = 0;
    int64_t rangeHigh = 0;
    const Type* rangeBase = nullptr;  ///< subranges and aliases with a default: the underlying type

    const Field* field(std::string_view upper) const;
    const EnumConstant* enumValue(std::string_view upper) const;
};

// ── Classification ──
bool isElementary(TypeKind k);
bool isInteger(TypeKind k);        ///< ANY_INT: SINT … ULINT
bool isSigned(TypeKind k);         ///< SINT INT DINT LINT
bool isUnsigned(TypeKind k);       ///< USINT … ULINT
bool isBitString(TypeKind k);      ///< BYTE WORD DWORD LWORD (BOOL excluded)
bool isReal(TypeKind k);           ///< REAL LREAL
bool isNumeric(TypeKind k);        ///< ANY_NUM
bool isDuration(TypeKind k);       ///< TIME LTIME
bool isMagnitude(TypeKind k);      ///< ANY_MAGNITUDE: ANY_NUM and durations
uint32_t bitWidth(TypeKind k);     ///< integer and bit-string widths; BOOL 1
int64_t minValue(TypeKind k);      ///< integer types
uint64_t maxValue(TypeKind k);     ///< integer and bit-string types (as unsigned)
std::string_view elementaryName(TypeKind k);

/**
 * True when a value of `from` converts to `to` implicitly without loss:
 * SINT→INT→DINT→LINT, USINT→UINT→UDINT→ULINT, unsigned to a wider signed
 * type, 8/16-bit integers→REAL, integers up to 32 bits→LREAL, REAL→LREAL,
 * BYTE→WORD→DWORD→LWORD, TIME→LTIME. Same kind is trivially true.
 */
bool implicitlyConvertible(TypeKind from, TypeKind to);

/// Owns every type of a compilation; elementary types are singletons.
class TypeTable {
public:
    TypeTable();
    TypeTable(const TypeTable&) = delete;
    TypeTable& operator=(const TypeTable&) = delete;

    const Type* elementary(TypeKind k) const { return elementary_[static_cast<size_t>(k)]; }
    /// Elementary type by upper-case name (INT, REAL, TIME …), or null.
    const Type* elementaryByName(std::string_view upper) const;
    const Type* voidType() const { return elementary(TypeKind::Void); }

    const Type* string(uint32_t capacity);
    const Type* array(const Type* element, std::vector<ArrayDim> dims);
    Type* newType();  ///< for structs, enums, subranges and FB types; caller fills it in

    static constexpr uint32_t kDefaultStringLength = 254;
    static constexpr uint32_t kMaxStringLength = 65535;

private:
    std::deque<std::unique_ptr<Type>> owned_;
    const Type* elementary_[static_cast<size_t>(TypeKind::Void) + 1] = {};
};

/// Structural equality for strings and arrays, identity otherwise (subranges compare by base).
bool sameType(const Type* a, const Type* b);

/// The type ignoring a subrange restriction or an alias's own default (rangeBase links both to their target).
inline const Type* baseOf(const Type* t) { return t && t->rangeBase ? t->rangeBase : t; }

}  // namespace simpleplc::st
