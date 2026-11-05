/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineSlots.h"

namespace JSC { namespace AOT { namespace Machine {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<typename T>
class UncheckedSpan {
public:
    UncheckedSpan() = default;
    template<typename U>
    UncheckedSpan(std::span<U> span)
        : m_data(span.data())
        , m_size(span.size())
    {
    }
    template<typename U>
    UncheckedSpan(UncheckedSpan<U> other)
        : m_data(other.data())
        , m_size(other.size())
    {
    }

    T& operator[](size_t index) const
    {
        ASSERT(index < m_size);
        return m_data[index];
    }
    T* data() const { return m_data; }
    size_t size() const { return m_size; }
    bool empty() const { return !m_size; }

private:
    T* m_data { nullptr };
    size_t m_size { 0 };
};

inline Operation* childWithoutCheck(const Operation* operation, unsigned index)
{
    ASSERT(index < operation->numChildren());
    return operation->children().data()[index];
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

class LeafCursor {
public:
    explicit LeafCursor(const Step& step)
        : m_step(step)
    {
    }

    Location take()
    {
        ASSERT(m_next < maxLeaves);
        return m_step.sources[m_next++];
    }
    unsigned numberTaken() const { return m_next; }

private:
    const Step& m_step;
    unsigned m_next { 0 };
};

struct AddressForm {
    enum Kind : uint8_t {
        BaseOffset,
        BaseIndex,
        InstanceSymbol,
    };

    MacroAssembler::Address address() const
    {
        ASSERT(kind == BaseOffset);
        return MacroAssembler::Address(base, offset);
    }

    Kind kind { BaseOffset };
    uint8_t shift { 0 };
    IndexExtension extension { IndexExtension::None };
    GPRReg base { InvalidGPRReg };
    GPRReg index { InvalidGPRReg };
    int32_t offset { 0 };
    CodeSymbol symbol { SymbolSpace::InstanceWord, 0 };
};

struct Test {
    enum Kind : uint8_t {
        CompareIntegers,
        CompareNumbers,
        TestBits,
    };

    Kind kind;
    Opcode relation;
    Width width;
    Location left;
    Location right;
    int64_t rightConstant { 0 };
    const Operation* loadOnLeft { nullptr };
    AddressForm addressOnLeft { };
    bool rightIsNumberOfBit { false };
};

inline constexpr unsigned minBytesOfTeardownSharedByReturns = isARM64() ? 12 : 8;

class Encoder {
    WTF_MAKE_NONCOPYABLE(Encoder);
public:
    Encoder(Procedure&, CCallHelpers&, RelocationRecorder&);
    void run();

    Procedure& procedure() { return m_proc; }
    const Frame& frame() const { return m_frame; }
    void addLatePath(LatePath* path) { m_latePaths.append(m_arena, PendingLatePath { path, m_frameIsSetUp, m_origin, m_operationOfOrigin }); }
    bool fallsThroughTo(const Block* from, const Block* target) const
    {
        if (from == m_block && blockEnteredThrough(target) != m_blockWrittenNext)
            return false;
        return fallsThroughToSlowCase(from, target);
    }
    bool fallsThroughToSlowCase(const Block* from, const Block* target) const;

    void encodeBlock(Block*);
    void encodeSteps(uint32_t begin, uint32_t end);
    void encodeExecute(const Step&);
    ALWAYS_INLINE void encodeExecuteInline(const Step&);
    void storeToSpillIfSpilled(const Operation* value)
    {
        if (m_allocation.isSpilled(value)) [[unlikely]]
            storeToSpill(value);
    }
    void storeToSpill(const Operation*);
    void storeSpilledValuesDefinedAtHead(Block*, const BlockPlace&);
    void setUpFrame(const Block*);
    void leaveFrameIfSetUp();
    void noteFrameState(bool isSetUp)
    {
        m_frameIsSetUp = isSetUp;
        if (m_frameRangeIsOpen != isSetUp) [[unlikely]]
            startOrEndFrameRange(isSetUp);
    }
    void startOrEndFrameRange(bool isSetUp);
    void noteOrigin(Origin origin, uint32_t operation = UINT32_MAX)
    {
        if (m_notesRangesOfOrigins) [[unlikely]]
            noteOriginInRanges(origin, operation);
    }
    void noteOriginInRanges(Origin, uint32_t operation);
    bool blockRunsInFrame(const Block*) const;
    void runLatePaths();
    void linkPendingJumps();
    void findBlocksWithoutCode();
    const Block* blockEnteredThrough(const Block* block) const { return m_blocksEntered[block->index()]; }
    void placeColdCodeNearItsJumps();
    bool hasCode(const Block* block) const { return blockEnteredThrough(block) == block; }
    MacroAssembler::Label* labelOf(const Block* block) { return &m_labels[blockEnteredThrough(block)->index()]; }
    void jumpTo(const Block*);
    void addPendingJump(MacroAssembler::Jump jump, const Block* target)
    {
        if (!jump.isSet())
            return;

        const MacroAssembler::Label* label = labelOf(target);
        if (label->isSet()) {
            jump.linkTo(*label, &m_jit);
            return;
        }
        m_pendingJumps.append(m_arena, PendingJump { jump, label });
    }
    void addPendingJumps(const MacroAssembler::JumpList&, const Block*);

    std::span<const GPRReg> findRegistersThatSetupPushes(const Block*);
    bool setupHasPushed(SpillIndex spill) const { return !m_spillsPushedAtSetup.isEmpty() && std::ranges::find(m_spillsPushedAtSetup.span(), spill) != m_spillsPushedAtSetup.span().end(); }

    AddressForm resolveAddress(const Operation* access, LeafCursor&);
    AddressForm resolveScaledSum(const Operation* sum, LeafCursor&);
    MacroAssembler::Address addressOf(Location memory) const;
    std::pair<GPRReg, bool> referenceOf(const Operation* access, const AddressForm&) const;
    void recordTrappingAccess(const Operation* access, const AddressForm&, MacroAssembler::Label instruction);

    void encodeMove(const Step&);
    void encodeReload(const Step&);
    void encodeSave(const Step&);
    void encodeSwap(const Step&);
    void encodeClearSpill(const Step&);
    void encodeKeepPointer(const Step&);
    void encodeMaterialize(const Operation*, Reg destination);
    bool tryEncodePairOfMoves(const Step& first, const Step& second);

    void encodePatch(const Step&, PatchOperation*);
    void encodeCheck(const Step&, PatchOperation*);
    ALWAYS_INLINE ValueRep repOf(Location location, const Operation* value) const
    {
        switch (location.kind()) {
        case Location::Register:
            return ValueRep::reg(location.reg());
        case Location::Spill:
            RELEASE_ASSERT(m_frameIsSetUp);
            RELEASE_ASSERT(m_frame.hasFrameRecord());
            return ValueRep::stack(m_frame.offsetFromFPOf(location.spillIndex()));
        case Location::Outgoing:
            RELEASE_ASSERT(m_frameIsSetUp);
            return ValueRep::stackArgument(static_cast<ValueRep::OffsetType>(location.offsetFromSP()));
        case Location::Immediate:
            if (value->isConstant())
                return ValueRep::constant(value->constantBits());
            RELEASE_ASSERT(value->opcode() == ArgumentReg && value->hasNoPosition() && isPinned(value->argumentReg()));
            return ValueRep::reg(value->argumentReg());
        case Location::None:
        case Location::SlotBytes:
        case Location::InstanceWord:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    void fillParameters(PatchParameters&, const Step&, PatchOperation*);
    struct SharedLatePath {
        PatchGenerator* generator;
        std::span<const ValueRep> reps;
        bool frameIsSetUp;
        MacroAssembler::Label* entry;
    };
    SharedLatePath* findSharedLatePath(PatchOperation*, std::span<const ValueRep>);

    void encodeTerminator(Block*, const Step&);
    void encodeBranch(Block*, const Step&);
    void encodeBranchOnOverflow(Block*, const Step&);
    void encodeSwitch(Block*, const Step&);
    void encodeSwitchThroughTable(Block*, GPRReg value, int64_t firstValue, uint64_t numberOfEntries);
    void encodeBinarySearch(Block*, GPRReg value, Width, std::span<const int64_t> values, unsigned firstSuccessor);
    void encodeReturn(const Step&);
    Test resolveTest(const Operation* user, LeafCursor&);

    MacroAssembler::JumpList branchOn(const Test&, bool whenItHolds);
#if CPU(X86_64)
    MacroAssembler::Jump jumpOnIntegers(const Test&, bool whenItHolds);
    std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfPredicate(const Step&);
#endif
    void encodeComparison(const Step&);
    void encodeSelect(const Step&);

    void encodeInteger(const Step&);
    bool tryEncodeSumAndConstant(const Step& sum, const Step& sumWithConstant);
    void encodeSumThatSetsFlags(const Step&);
    MacroAssembler::Jump jumpOnOverflow(bool whenItOverflowed);

    void encodeNumber(const Step&);

    void encodeLoad(const Step&);
    void encodeStore(const Step&);
    void encodeFence();
    bool tryEncodePairOfStores(const Step& first, const Step& second);
    void encodeBranchInMemory(Block*, const Step&);

    Procedure& m_proc;
    Arena& m_arena;
    CCallHelpers& m_jit;
    RelocationRecorder& m_relocations;
    const Order& m_order;
    const Liveness& m_liveness;
    const Allocation& m_allocation;
    const Frame& m_frame;
    const FrameCoverage m_coverage;
    const bool m_notesRangesOfOrigins;
    const bool m_hasSpills;
    const bool m_hasFramedPart;

    UncheckedSpan<MacroAssembler::Label> m_labels;
    UncheckedSpan<const Block*> m_blocksEntered;
    BitSpan m_blocksThatOnlyReturnWithoutFrame;
    BitSpan m_blocksThatStoreResultOfPatchAtHead;
    UncheckedSpan<uint32_t> m_nextLayoutIndexWithCode;
    uint32_t m_numberOfBlocksWithCode { 0 };
    struct PendingJump {
        MacroAssembler::Jump jump;
        const MacroAssembler::Label* label;
    };
    ArenaVector<PendingJump> m_pendingJumps;
    struct PendingJumpTable {
        uint32_t index;
        std::span<const Block* const> targets;
    };
    ArenaVector<PendingJumpTable> m_pendingJumpTables;
    struct PendingLatePath {
        LatePath* path;
        bool frameIsSetUp;
        Origin origin;
        uint32_t operation;
    };
    ArenaVector<PendingLatePath> m_latePaths;
    ArenaVector<SharedLatePath> m_sharedLatePaths;
    std::span<uint32_t> m_sharedLatePathSlots;
    ArenaVector<Block*> m_blocksAsWritten;
    UncheckedSpan<uint32_t> m_indexAsWritten;
    uint32_t m_beginOfRareCodeAsWritten { 0 };
    bool m_isAmongOtherCode { false };
    Block* m_block { nullptr };
    const Block* m_blockWrittenNext { nullptr };
    bool m_blockIsLeftInFrame { false };
    bool m_frameIsSetUp { false };
    bool m_frameRangeIsOpen { false };
    ArenaVector<GPRReg> m_registersPushedAtSetup;
    ArenaVector<SpillIndex> m_spillsPushedAtSetup;
    MacroAssembler::Label m_teardownSharedByReturns;
    Origin m_origin;
    uint32_t m_operationOfOrigin { UINT32_MAX };
    const Block* m_blockWithClearedScratchFPR { nullptr };
    unsigned m_offsetBehindClearedScratchFPR { 0 };
};

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
