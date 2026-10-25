#pragma once
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "st/types.h"
#include "vm/arith.h"

namespace simpleplc::vm {

/**
 * Operand segments. An operand is a 32-bit value: segment in the top 3 bits,
 * byte offset in the rest. The verifier checks every direct operand against its
 * segment's size before an image may run.
 */
enum class Seg : uint8_t {
    Const = 0,   ///< constant pool (read-only)
    Global = 1,  ///< tag mirrors (copied in at cycle start, out at cycle end)
    Self = 2,    ///< the running program or function block instance
    Frame = 3,   ///< the running POU's frame (temporaries, function variables)
    Next = 4,    ///< the frame of the function or block about to be called
};
constexpr uint32_t kOffsetBits = 29;
constexpr uint32_t kMaxOffset = (uint32_t{1} << kOffsetBits) - 1;
constexpr uint32_t operand(Seg seg, uint32_t offset) { return (static_cast<uint32_t>(seg) << kOffsetBits) | offset; }
constexpr Seg segOf(uint32_t op) { return static_cast<Seg>(op >> kOffsetBits); }
constexpr uint32_t offsetOf(uint32_t op) { return op & kMaxOffset; }

enum class Op : uint16_t {
    Nop,
    Mov,         ///< a := b, d bytes
    Add, Sub, Mul, Div, Mod,  ///< a := b op c (type)
    Neg, Abs,                 ///< a := op b
    Pow,                      ///< a := b ** c (REAL/LREAL)
    Eq, Ne, Lt, Le, Gt, Ge,   ///< a (BOOL) := b op c, operands of `type`
    And, Or, Xor,             ///< a := b op c (BOOL or bit string)
    Not,                      ///< a := NOT b
    Shl, Shr, Rol, Ror,       ///< a := op(b, c); c is an LINT count
    Min, Max,                 ///< a := min/max(b, c)
    Conv,                     ///< a (type d) := b (type `type`); flags: kConvTruncate
    TimeMul, TimeDiv,         ///< a (duration `type`) := b op c; c of kind d
    Math,                     ///< a := fn(b), fn = flags (MathFn), REAL/LREAL
    GetBit,                   ///< a (BOOL) := bit d of b (`type` bit string)
    SetBit,                   ///< bit d of a (`type`) := b (BOOL)
    Addr,                     ///< a (u32 reference) := absolute address of operand b
    RefAdd,                   ///< a (ref) := b (ref) + d (bytes; checked when used)
    Zero,                     ///< d bytes at a := 0
    Index,                    ///< a (ref) := b (ref) + checked c (LINT); d = descriptor (IndexDesc)
    Load,                     ///< a := d bytes at reference b
    Store,                    ///< d bytes at reference a := b
    StrMov,                   ///< a := b; d = StrDesc{cap a, cap b}
    StrConcat,                ///< a := b & c; d = StrDesc{cap a, cap b, cap c}
    StrLeft, StrRight,        ///< a := LEFT/RIGHT(b, L = c (LINT)); d = StrDesc
    StrMid,                   ///< a := MID(b, L, P): c = two LINTs (L then P); d = StrDesc
    StrInsert,                ///< a := INSERT(b, c, P = x (LINT))
    StrDelete,                ///< a := DELETE(b, L, P): c = two LINTs (L then P)
    StrReplace,               ///< a := REPLACE(b, c, L, P): x = two LINTs (L then P)
    StrFind,                  ///< a (LINT) := FIND(b, c)
    StrLen,                   ///< a (LINT) := LEN(b)
    StrCmp,                   ///< a (BOOL) := b cmp c; flags = comparison (CmpKind)
    StrFromValue,             ///< a (string) := text of b (`type`)
    StrToValue,               ///< a (`type`) := parse b
    CheckRange,               ///< F_SUBRANGE unless b (`type`) in descriptor d [low, high]
    FaultIf,                  ///< fault `flags` if a (BOOL) is TRUE
    Jmp,                      ///< pc := a
    Jz, Jnz,                  ///< if b (BOOL) is FALSE / TRUE: pc := a
    Call,                     ///< call function POU a (its frame is the NEXT segment)
    CallFb,                   ///< call block POU a on the instance at operand b
    CallFbRef,                ///< call block POU a on the instance at reference b
    Ret,
    Try,                      ///< push catch: handler pc a, ENO operand b
    EndTry,                   ///< pop catch
    Now,                      ///< a (TIME) := the cycle's time
    ForNext,                  ///< b (control, `type`) += c; if that leaves the type: pc := a (loop ends)
    Atan2,                    ///< a := ATAN2(b, c) (REAL/LREAL)
    FirstCycle,                ///< a (BOOL) := this is the first cycle since the program started
    Dirty,                    ///< binding a was stored to in this cycle (it is copied out at the end)
    Count_,
};

enum class MathFn : uint8_t { Sqrt, Ln, Log, Exp, Sin, Cos, Tan, Asin, Acos, Atan };
enum class CmpKind : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
constexpr uint8_t kConvTruncate = 1;

struct Insn {
    Op op = Op::Nop;
    uint8_t type = 0;   ///< TypeKind of the operation
    uint8_t flags = 0;
    uint32_t a = 0, b = 0, c = 0, d = 0;
};
static_assert(sizeof(Insn) == 20);

/// Array index check, in the constant pool: index - low must be in [0, count).
struct IndexDesc {
    int64_t low;
    uint64_t count;
    uint32_t stride;
    uint32_t pad;
};

/// Capacities of string operands a, b, c (0 when the operand is not a string) and the operand
/// `x` of extra integer arguments (P, or the pair L, P), in the constant pool.
struct StrDesc {
    uint32_t capA, capB, capC;
    uint32_t x;
};

/// Inclusive range of a subrange type, in the constant pool.
struct RangeDesc {
    int64_t low;
    int64_t high;
};

enum class PouKind : uint8_t { Program, Function, FunctionBlock };

struct PouEntry {
    std::string name;
    PouKind kind = PouKind::Function;
    uint32_t codeBegin = 0;   ///< first instruction
    uint32_t codeEnd = 0;     ///< one past the last
    uint32_t frameSize = 0;   ///< bytes of its own frame
    uint32_t instanceSize = 0;  ///< blocks and programs
};

/// A variable visible to monitoring, online change and retain.
struct VarEntry {
    std::string path;         ///< MAIN.FILL.ET (upper case)
    std::string typeName;     ///< as declared
    uint8_t kind = 0;         ///< TypeKind of the leaf
    uint32_t offset = 0;      ///< absolute in the data block
    uint32_t size = 0;
    uint8_t flags = 0;        ///< VarFlags
    bool hasRange = false;    ///< a subrange: monitoring writes must stay within [low, high]
    int64_t low = 0, high = 0;
    /// Arrays shown as one entry (more than 1024 elements): the full definition of the type (bounds,
    /// element type, struct and block layouts), so an online change keeps the bytes only for the same definition
    std::string typeSig;
};
enum VarFlags : uint8_t { kVarRetain = 1, kVarConstant = 2, kVarExternal = 4, kVarInput = 8, kVarOutput = 16 };

/// A VAR_EXTERNAL bound to a tag: its mirror in the Global segment.
struct Binding {
    std::string tag;         ///< as named in the tag table
    uint8_t kind = 0;        ///< TypeKind in the program
    uint32_t offset = 0;     ///< in the Global segment
    bool readOnly = false;   ///< VAR_EXTERNAL CONSTANT
};

/// An enumeration's value names, so monitoring can show Color#Red instead of 0.
struct EnumInfo {
    std::string name;
    std::vector<std::pair<int64_t, std::string>> values;
};

struct LineEntry {
    uint32_t pc = 0;
    uint16_t file = 0;
    uint32_t line = 0;
};

/**
 * A compiled program: everything the virtual machine needs, nothing it must
 * trust. `verify()` checks it completely before it runs, also when it was read
 * from disk.
 */
struct Image {
    static constexpr uint32_t kFormat = 3;  // 2: subrange limits in the variable table; 3: type signatures

    std::string compiler;               ///< compiler version
    std::array<uint8_t, 32> sourceHash{};  ///< SHA-256 of the sources and the tag table
    std::vector<std::string> files;
    std::string program;                ///< the PROGRAM's name

    // Data block: [0, globalsSize) tag mirrors; [instanceOffset, +instanceSize) the program
    // instance; [stackOffset, +stackSize) frames. blockSize = stackOffset + stackSize.
    uint32_t globalsSize = 0;
    uint32_t instanceOffset = 0;
    uint32_t instanceSize = 0;
    uint32_t stackOffset = 0;
    uint32_t stackSize = 0;
    uint32_t blockSize = 0;
    std::vector<uint8_t> initialData;   ///< [0, stackOffset): initial values

    std::vector<uint8_t> constants;
    std::vector<Insn> code;
    std::vector<PouEntry> pous;
    uint32_t entry = 0;                 ///< index of the PROGRAM in pous

    std::vector<VarEntry> vars;
    std::vector<Binding> bindings;
    std::vector<LineEntry> lines;       ///< sorted by pc
    std::vector<EnumInfo> enums;        ///< enumerations named by VarEntry::typeName

    /// Source line of an instruction (0 when unknown).
    LineEntry lineAt(uint32_t pc) const;
};

/// Checks an image; the message says what is wrong. Must pass before run.
std::expected<void, std::string> verify(const Image& image);

std::vector<uint8_t> serialize(const Image& image);
std::expected<Image, std::string> deserialize(std::string_view bytes);

}  // namespace simpleplc::vm
