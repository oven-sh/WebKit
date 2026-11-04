/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "AssemblerPolicy.h"
#include "CCallHelpers.h"
#include "LinkBuffer.h"
#include "RelocationKind.h"

namespace JSC { namespace AOT {

class BuildContext;

enum class LinkFailure : uint8_t {
    ProcessAddressInCode,
    OutOfPCRelativeReach,
    OutOfMemory,
};

// What the index of a symbol counts, in the code of a function | in shared code, which is made once everything has its number:
enum class SymbolSpace : uint8_t {
    // CodeBody::callees | never
    Function,
    // CodeBody::stubKeys | ArgumentThunks::stubKeys. A stub reaches another stub without a relocation.
    Stub,
    // CodeBody::argumentMoves | never
    ArgumentThunk,
    // CodeBody::constants | never
    Constant,
    // CodeBody::jumpTables | never
    JumpTable,
    // CodeBody::mentions: the symbol stands for the 32 bits that ImageNumbers::valueOf() answers | never
    Number,
    // CodeBody::mentions: the symbol stands for the word of the instance that lies valueOf() bytes from the instance register, a signed distance | never
    InstanceWord,
    // CodeBody::mentions, of NumberSpace::RegExp | the number of the record
    // Either way the symbol stands for the code of the expression, at the entry that CodeSymbol::entry names.
    // A function takes that address, through Output::relocatable(). Shared code calls it, through StubAssembler::callCode(). Only a call or jump from shared code gets a veneer.
    RegExp,
    WebAssembly,
    FFI,
};
static constexpr unsigned numberOfSymbolSpaces = 10;
static_assert(static_cast<unsigned>(SymbolSpace::FFI) + 1 == numberOfSymbolSpaces);

constexpr bool namesMention(SymbolSpace space)
{
    switch (space) {
    case SymbolSpace::Number:
    case SymbolSpace::InstanceWord:
    case SymbolSpace::RegExp:
        return true;
    case SymbolSpace::Function:
    case SymbolSpace::Stub:
    case SymbolSpace::ArgumentThunk:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// What a constant of CodeBody::constants is, as its maker says. No code follows from it: the pool counts its bytes by it.
enum class ConstantKind : uint8_t {
    Unlabeled,
    Number,
    ShapeSet,
    BitSetOverKnownShapes,
    OffsetTable,
    CellTemplate,
};
static constexpr unsigned numberOfConstantKinds = 6;
static_assert(static_cast<unsigned>(ConstantKind::CellTemplate) + 1 == numberOfConstantKinds);

static constexpr uint16_t primaryEntry = 0;

struct CodeSymbol {
    friend constexpr bool operator==(const CodeSymbol&, const CodeSymbol&) = default;

    SymbolSpace space;
    uint32_t index;
    uint16_t entry { primaryEntry };
};

struct Relocation {
    constexpr CodeSymbol codeSymbol() const { return { space, symbol, entry }; }

    friend constexpr bool operator==(const Relocation&, const Relocation&) = default;

    uint32_t offset;
    RelocationKind kind;
    SymbolSpace space;
    uint16_t entry;
    uint32_t symbol;
};
static_assert(sizeof(Relocation) == 12);

struct RelocatableCode {
    Vector<uint8_t> bytes;
    Vector<Relocation> relocations;
};

class JS_EXPORT_PRIVATE ImageLinkBuffer {
    WTF_MAKE_NONCOPYABLE(ImageLinkBuffer);
public:
    ImageLinkBuffer(CCallHelpers&, LinkBuffer::Profile);

    LinkBuffer& linkBuffer() { return m_linkBuffer; }
    bool didFailToAllocate() const { return m_linkBuffer.didFailToAllocate(); }

    void finalize(bool dumpsDisassembly, ASCIILiteral name);
    std::span<uint8_t> bytes();
    CCallHelpers::Label end() const { return m_end; }

private:
    CCallHelpers::Label m_end;
    Vector<uint32_t> m_storage;
    LinkBuffer m_linkBuffer;
    uint32_t m_sizeOfCode { 0 };
    bool m_didFinalize { false };
};

// One unit of what lies where no code does. It faults, and not with a signal that programs listen for.
JS_EXPORT_PRIVATE void emitPadding(CCallHelpers&);

enum class InstanceWordWidth : uint8_t { Byte, Word32, Word64 };

// A Number symbol resolves to a 32-bit number, an InstanceWord symbol to a 32-bit distance from the instance register, and every other symbol to an offset in the code. The kind says how the site is patched with that value, so it has to agree with the space.
constexpr bool relocationMayName(RelocationKind kind, SymbolSpace space)
{
    switch (kind) {
    case RelocationKind::Number32:
    case RelocationKind::NumberWord:
        return space == SymbolSpace::Number;
    case RelocationKind::InstanceOffset:
    case RelocationKind::InstanceAccess:
        return space == SymbolSpace::InstanceWord;
    case RelocationKind::Call:
    case RelocationKind::Jump:
    case RelocationKind::ShortJump:
    case RelocationKind::ConditionalJump:
    case RelocationKind::ShortConditionalJump:
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
    case RelocationKind::InvertedJumpThenJump:
    case RelocationKind::AddressNear:
    case RelocationKind::AddressFar:
    case RelocationKind::Load64:
    case RelocationKind::Distance32:
        return space != SymbolSpace::Number && space != SymbolSpace::InstanceWord;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// Every sequence has one length whatever the number turns out to be, so the bytes of a function do not depend on the rest of the program.
class JS_EXPORT_PRIVATE RelocationRecorder {
    WTF_MAKE_NONCOPYABLE(RelocationRecorder);
public:
    RelocationRecorder(CCallHelpers&, const AssemblerPolicy&);

    void moveAddress(CodeSymbol, GPRReg dest, PCRelativeReach);
    void load64(CodeSymbol, GPRReg dest);
    ALWAYS_INLINE uint32_t call(CodeSymbol symbol)
    {
        CCallHelpers::Label site = m_jit.label();
        m_jit.nearCall();
        return append(site, RelocationKind::Call, symbol);
    }
    uint32_t jump(CodeSymbol symbol)
    {
        CCallHelpers::Label site = m_jit.label();
        m_jit.nearTailCall();
        return append(site, RelocationKind::Jump, symbol);
    }
    void emitDistance32(CodeSymbol, int32_t addend);

    // SymbolSpace::Number, RelocationKind::Number32. The upper half of dest is zero. ARM64: movz, movk. x86-64: mov r32, imm32.
    void moveNumber(CodeSymbol symbol, GPRReg dest) { append(m_jit.moveNumber32WithPatch(dest), RelocationKind::Number32, symbol); }
    // SymbolSpace::Number, RelocationKind::NumberWord. Four bytes of data in the code that hold the number itself: a word after a call that says what to throw.
    void emitNumber32(CodeSymbol);

    // SymbolSpace::InstanceWord, at any signed 32-bit distance. What follows is what the assembler writes, and on ARM64 that holds a distance from 16 MB - 4 KB below the instance to less than 16 MB above it.
    // For a word beyond that applyRelocation() answers false and writes nothing: isFarInstanceWord() below.
    // RelocationKind::InstanceOffset. ARM64: add or sub of the upper twelve bits, then add of the lower twelve. x86-64: lea with a 32-bit displacement.
    void moveInstanceAddress(CodeSymbol, GPRReg instance, GPRReg dest);
    // RelocationKind::InstanceAccess. ARM64: add or sub of the upper twelve bits, into dest or, for an FPR, into the assembler's memory scratch register,
    // then a load with the lower twelve bits as its offset. x86-64: one load with a 32-bit displacement. A Byte and a Word32 are zero-extended.
    // ARM64 counts that offset in units of the width, so the distance is a multiple of the width.
    void loadInstanceWord(CodeSymbol, InstanceWordWidth, GPRReg instance, GPRReg dest);
    void loadInstanceDouble(CodeSymbol, GPRReg instance, FPRReg dest);
    // ARM64 writes the assembler's memory scratch register.
    void storeInstanceWord(CodeSymbol, InstanceWordWidth, GPRReg source, GPRReg instance);
    void storeInstanceDouble(CodeSymbol, FPRReg source, GPRReg instance);
#if CPU(X86_64)
    // RelocationKind::InstanceAccess. The functor is given the word as a memory operand and emits ONE instruction that reads or writes it there, or nothing. An immediate may follow the displacement.
    // In front of a displacement stand prefixes, the opcode and the ModRM byte alone, so the first four bytes of the instruction that hold the placeholder are the displacement.
    template<typename Functor>
    auto withInstanceWordAsOperand(CodeSymbol symbol, GPRReg instance, const Functor& functor)
    {
        auto& buffer = m_jit.m_assembler.buffer();
        size_t sizeBefore = buffer.codeSize();
        auto result = functor(CCallHelpers::Address(instance, placeholderOfInstanceWordDistance));
        if (buffer.codeSize() == sizeBefore)
            return result;
        std::span<uint8_t> instruction = WTF::unsafeMakeSpan(static_cast<uint8_t*>(buffer.data()), buffer.codeSize()).subspan(sizeBefore);
        size_t index = 0;
        while (true) {
            RELEASE_ASSERT(index + sizeof(int32_t) <= instruction.size());
            if (WTF::unalignedLoad<int32_t>(instruction.subspan(index).data()) == placeholderOfInstanceWordDistance)
                break;
            ++index;
        }
        WTF::unalignedStore<int32_t>(instruction.subspan(index).data(), 0);
        append(CCallHelpers::Jump(buffer.label().labelAtOffset(-static_cast<int>(instruction.size() - index))).label(), RelocationKind::InstanceAccess, symbol);
        return result;
    }
#endif

    uint32_t numberOfRelocations() const { return m_pending.size(); }

    std::expected<RelocatableCode, LinkFailure> finalize(LinkBuffer::Profile, BuildContext*);
    std::expected<Vector<Relocation>, LinkFailure> link(ImageLinkBuffer&, BuildContext*);

private:
#if CPU(X86_64)
    // Too large for a displacement of one byte, so the assembler writes four.
    static constexpr int32_t placeholderOfInstanceWordDistance = 0x12345678;
#endif

    struct PendingRelocation {
        CCallHelpers::Label site;
        uint32_t symbol;
        uint16_t entry;
        RelocationKind kind;
        SymbolSpace space;
    };

    ALWAYS_INLINE uint32_t append(CCallHelpers::Label site, RelocationKind kind, CodeSymbol symbol)
    {
        RELEASE_ASSERT(relocationMayName(kind, symbol.space));
        uint32_t index = numberOfRelocations();
        if (index == m_pending.capacity()) [[unlikely]]
            m_pending.reserveCapacity(2 * static_cast<size_t>(index));
        m_pending.unsafeAppendWithoutCapacityCheck({ site, symbol.index, symbol.entry, kind, symbol.space });
        return index;
    }

    CCallHelpers& m_jit;
    Vector<PendingRelocation, 32> m_pending;
};

// AddressFar is page relative on ARM64: code must be loaded at a multiple of 4 KB.
// For Number32, NumberWord, InstanceOffset and InstanceAccess the target is not an offset into the code. It is the 32-bit number, zero-extended.
[[nodiscard]] ALWAYS_INLINE bool applyRelocation(std::span<uint8_t> code, size_t bodyStart, const Relocation& relocation, size_t target)
{
    return MacroAssembler::applyRelocation(relocation.kind, code, bodyStart + relocation.offset, target);
}

// A word whose distance from the instance the instructions of InstanceOffset and InstanceAccess do not hold. There is none on x86-64.
// Image, which alone knows the distance, writes the two instructions of ARM64 anew, so the length stays: ldr w, <the distance>, then the add, load or store with [instance, w, sxtw].
// The distance lies in a word that Image puts behind the body. A site that lies further in front of that word than the ldr reaches jumps to a detour behind the body in its place: movz, movk, b back.
ALWAYS_INLINE bool isFarInstanceWord([[maybe_unused]] int32_t distance)
{
#if CPU(ARM64)
    return !MacroAssembler::holdsInstanceWordDistanceInImmediates(distance);
#else
    return false;
#endif
}

#if CPU(ARM64)
static constexpr size_t reachOfReadOfFarInstanceWord = MacroAssembler::instanceWordDistanceReadReach;
static constexpr size_t sizeOfDetourToFarInstanceWord = MacroAssembler::instanceWordDetourSize;
#else
static constexpr size_t reachOfReadOfFarInstanceWord = 0;
static constexpr size_t sizeOfDetourToFarInstanceWord = 0;
#endif

// `placeOfDistance` and `detour` are offsets into the code. The caller writes the word at the one. The other is written here.
[[nodiscard]] ALWAYS_INLINE bool applyRelocationReadingDistance([[maybe_unused]] std::span<uint8_t> code, [[maybe_unused]] size_t bodyStart, [[maybe_unused]] const Relocation& relocation, [[maybe_unused]] size_t placeOfDistance)
{
#if CPU(ARM64)
    return MacroAssembler::readInstanceWordDistance(relocation.kind, code, bodyStart + relocation.offset, placeOfDistance);
#else
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

[[nodiscard]] ALWAYS_INLINE bool applyRelocationThroughDetour([[maybe_unused]] std::span<uint8_t> code, [[maybe_unused]] size_t bodyStart, [[maybe_unused]] const Relocation& relocation, [[maybe_unused]] size_t detour, [[maybe_unused]] int32_t distance)
{
#if CPU(ARM64)
    return MacroAssembler::makeInstanceWordDistanceInDetour(relocation.kind, code, bodyStart + relocation.offset, detour, distance);
#else
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

template<typename Resolver, typename Reporter>
uint64_t applyRelocations(std::span<uint8_t> code, size_t bodyStart, std::span<const Relocation> relocations, const Resolver& resolve, const Reporter& reportOutOfReach)
{
    uint64_t numberApplied = 0;
    for (const Relocation& relocation : relocations) {
        std::optional<size_t> target = resolve(relocation);
        if (!target)
            continue;
        if (!applyRelocation(code, bodyStart, relocation, *target)) {
            reportOutOfReach(relocation);
            continue;
        }
        ++numberApplied;
    }
    return numberApplied;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
