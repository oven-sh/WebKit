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
#include <type_traits>

namespace JSC { namespace AOT {

class BuildContext;

enum class LinkFailure : uint8_t {
    ProcessAddressInCode,
    OutOfPCRelativeReach,
    OutOfMemory,
};

enum class SymbolSpace : uint8_t {
    Function,
    Stub,
    ArgumentThunk,
    Constant,
    JumpTable,
    Number,
    InstanceWord,
    RegExp,
    WebAssembly,
    FFI,
};
static constexpr unsigned numberOfSymbolSpaces = 10;
static_assert(static_cast<unsigned>(SymbolSpace::FFI) + 1 == numberOfSymbolSpaces);

enum class ConstantKind : uint8_t {
    Unlabeled,
    Number,
    ShapeSet,
    ShapeBitmap,
    OffsetTable,
    CellTemplate,
    ByteLiteral,
};
static constexpr unsigned numberOfConstantKinds = 7;
static_assert(static_cast<unsigned>(ConstantKind::ByteLiteral) + 1 == numberOfConstantKinds);

static constexpr uint16_t primaryEntry = 0;
static constexpr uint16_t entryInFrontOfStub = 1;
static constexpr uint16_t entryInFrontOfStubThatDoesTwoMoves = 2;
static constexpr unsigned sizeOfMoveInFrontOfStub = isX86_64() ? 3 : 4;

constexpr unsigned sizeOfHeadOfStub(uint16_t entry)
{
    switch (entry) {
    case entryInFrontOfStub:
        return sizeOfMoveInFrontOfStub;
    case entryInFrontOfStubThatDoesTwoMoves:
        return 2 * sizeOfMoveInFrontOfStub;
    default:
        return 0;
    }
}

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
    Vector<uint8_t> takeBytes();
    CCallHelpers::Label end() const { return m_end; }
    bool hasBranchIslands() const { return m_hasBranchIslands; }

private:
    size_t planBranchIslands(CCallHelpers&);

    CCallHelpers::Label m_end;
#if CPU(ARM64)
    ARM64BranchIslands m_branchIslands;
#endif
    bool m_hasBranchIslands { false };
    Vector<uint8_t> m_storage;
    LinkBuffer m_linkBuffer;
    uint32_t m_sizeOfCode { 0 };
    bool m_didFinalize { false };
};

JS_EXPORT_PRIVATE void emitPadding(CCallHelpers&);
JS_EXPORT_PRIVATE void emitUnreachable(CCallHelpers&);

enum class InstanceWordWidth : uint8_t { Byte, Word32, Word64 };

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

template<RelocationKind kind>
using ConstantRelocationKind = std::integral_constant<RelocationKind, kind>;

template<typename Functor>
ALWAYS_INLINE auto withKindAsConstant(RelocationKind kind, const Functor& functor)
{
    switch (kind) {
    case RelocationKind::Call:
        return functor(ConstantRelocationKind<RelocationKind::Call> { });
    case RelocationKind::Jump:
        return functor(ConstantRelocationKind<RelocationKind::Jump> { });
    case RelocationKind::ShortJump:
        return functor(ConstantRelocationKind<RelocationKind::ShortJump> { });
    case RelocationKind::ConditionalJump:
        return functor(ConstantRelocationKind<RelocationKind::ConditionalJump> { });
    case RelocationKind::ShortConditionalJump:
        return functor(ConstantRelocationKind<RelocationKind::ShortConditionalJump> { });
    case RelocationKind::ConditionalJumpThenNop:
        return functor(ConstantRelocationKind<RelocationKind::ConditionalJumpThenNop> { });
    case RelocationKind::TestBitJump:
        return functor(ConstantRelocationKind<RelocationKind::TestBitJump> { });
    case RelocationKind::TestBitJumpThenNop:
        return functor(ConstantRelocationKind<RelocationKind::TestBitJumpThenNop> { });
    case RelocationKind::InvertedJumpThenJump:
        return functor(ConstantRelocationKind<RelocationKind::InvertedJumpThenJump> { });
    case RelocationKind::AddressNear:
        return functor(ConstantRelocationKind<RelocationKind::AddressNear> { });
    case RelocationKind::AddressFar:
        return functor(ConstantRelocationKind<RelocationKind::AddressFar> { });
    case RelocationKind::Load64:
        return functor(ConstantRelocationKind<RelocationKind::Load64> { });
    case RelocationKind::Distance32:
        return functor(ConstantRelocationKind<RelocationKind::Distance32> { });
    case RelocationKind::InstanceOffset:
        return functor(ConstantRelocationKind<RelocationKind::InstanceOffset> { });
    case RelocationKind::Number32:
        return functor(ConstantRelocationKind<RelocationKind::Number32> { });
    case RelocationKind::InstanceAccess:
        return functor(ConstantRelocationKind<RelocationKind::InstanceAccess> { });
    case RelocationKind::NumberWord:
        return functor(ConstantRelocationKind<RelocationKind::NumberWord> { });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class JS_EXPORT_PRIVATE RelocationRecorder {
    WTF_MAKE_NONCOPYABLE(RelocationRecorder);
public:
    RelocationRecorder(CCallHelpers&, const AssemblerPolicy&);

    void moveAddress(CodeSymbol, GPRReg dest, PCRelativeReach);
    void load64(CodeSymbol, GPRReg dest);
#if CPU(X86_64)
    void loadDouble(CodeSymbol, FPRReg dest);
    void addDouble(CodeSymbol, FPRReg dest);
    void subDouble(CodeSymbol, FPRReg dest);
    void mulDouble(CodeSymbol, FPRReg dest);
    void divDouble(CodeSymbol, FPRReg dest);
    void compare64(GPRReg left, CodeSymbol);
#endif
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

    bool exceedsBranchReach() const;
    CCallHelpers::Label returnAddressOf(uint32_t call) const;
    void routeTransfersThroughEnd();

    void moveNumber(CodeSymbol symbol, GPRReg dest) { append(m_jit.moveNumber32WithPatch(dest), RelocationKind::Number32, symbol); }
    void emitNumber32(CodeSymbol);

    void moveInstanceAddress(CodeSymbol, GPRReg instance, GPRReg dest);
    void loadInstanceWord(CodeSymbol, InstanceWordWidth, GPRReg instance, GPRReg dest);
    void loadInstanceDouble(CodeSymbol, GPRReg instance, FPRReg dest);
    void storeInstanceWord(CodeSymbol, InstanceWordWidth, GPRReg source, GPRReg instance);
    void storeInstanceDouble(CodeSymbol, FPRReg source, GPRReg instance);
#if CPU(X86_64)
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

#if CPU(X86_64)
    template<typename Functor>
    auto withNumberAsImmediate(CodeSymbol symbol, const Functor& functor)
    {
        auto& buffer = m_jit.m_assembler.buffer();
        auto result = functor(placeholderOfNumber);
        std::span<uint8_t> immediate = WTF::unsafeMakeSpan(static_cast<uint8_t*>(buffer.data()), buffer.codeSize()).last(sizeof(int32_t));
        RELEASE_ASSERT(WTF::unalignedLoad<int32_t>(immediate.data()) == placeholderOfNumber);
        WTF::unalignedStore<int32_t>(immediate.data(), 0);
        append(CCallHelpers::Jump(buffer.label().labelAtOffset(-static_cast<int>(sizeof(int32_t)))).label(), RelocationKind::Number32, symbol);
        return result;
    }
#endif

    uint32_t numberOfRelocations() const { return m_pending.size(); }
    void reserveRoomFor(size_t number) { m_pending.reserveCapacity(number); }

    std::expected<RelocatableCode, LinkFailure> finalize(LinkBuffer::Profile, BuildContext*);
    std::expected<Vector<Relocation>, LinkFailure> link(ImageLinkBuffer&, BuildContext*);

private:
#if CPU(X86_64)
    static constexpr int32_t placeholderOfInstanceWordDistance = 0x12345678;
    static constexpr int32_t placeholderOfNumber = 0x23456789;
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

[[nodiscard]] ALWAYS_INLINE bool applyRelocation(std::span<uint8_t> code, size_t bodyStart, const Relocation& relocation, size_t target)
{
    return withKindAsConstant(relocation.kind, [&](auto kind) ALWAYS_INLINE_LAMBDA {
        return MacroAssembler::applyRelocation(kind, code, bodyStart + relocation.offset, target);
    });
}

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
