/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineSlots.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "MarkedBlockCage.h"
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Machine {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

inline Operation* childWithoutCheck(const Operation* operation, unsigned index)
{
    ASSERT(index < operation->numChildren());
    return operation->children().data()[index];
}

inline Operation* inlineChildWithoutCheck(const Operation* operation, unsigned index)
{
    ASSERT(index < operation->numberOfInlineChildren());
    return operation->inlineChildren()[index];
}

inline Operation* addressChildWithoutCheck(const Operation* access)
{
    ASSERT(isMemoryAccess(access->opcode()) && access->numberOfInlineChildren());
    return access->inlineChildren()[access->numberOfInlineChildren() - 1];
}

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
    Location peek() const
    {
        ASSERT(m_next < maxLeaves);
        return m_step.sources[m_next];
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
    IndexExtension extensionOfRight { IndexExtension::None };
    const Operation* linkedNumberOnRight { nullptr };
};

struct FrameAtBlock {
    bool isEnteredInFrame() const { return runsInFrame && !isSetUpAtHead; }
    bool isLeftInFrame() const { return runsInFrame && !isTornDownAtEnd; }

    bool runsInFrame;
    bool isSetUpAtHead;
    bool isTornDownAtEnd;
    bool returnsInPlaceOfJump;
};

enum class MemoryOfEncodedMove : bool { IsRead, IsWritten };

inline constexpr unsigned minBytesOfTeardownSharedByReturns = isARM64() ? 12 : 8;

inline bool tryMoveSmallConstant(CCallHelpers& jit, int32_t constant, GPRReg destination)
{
#if CPU(X86_64)
    static_assert(!static_cast<uint32_t>(MarkedBlockCage::maxSize));
    if (!isRepresentableAs<int8_t>(constant))
        return false;
    jit.m_assembler.leal_mr(constant, cellCageBaseGPR, destination);
    return true;
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(constant);
    UNUSED_PARAM(destination);
    return false;
#endif
}

class Encoder {
    WTF_MAKE_NONCOPYABLE(Encoder);
public:
    Encoder(Procedure&, CCallHelpers&, RelocationRecorder&);
    void run();

    Procedure& procedure() { return m_proc; }
    const Frame& frame() const { return m_frame; }
    void addLatePath(LatePath* path) { m_latePaths.append(m_arena, PendingLatePath { path, m_frameIsSetUp, m_origin, m_operationOfOrigin }); }

    const BlockPlace& placeOf(const Block* block) const { return m_places[block->index()]; }
    const BlockSteps& stepsOf(const Block* block) const { return m_stepsOfBlocks[block->index()]; }
    const ValueFacts& factsOf(const Operation* operation) const { return m_valueFacts[operation->index()]; }
    bool isFused(const Operation* operation) const { return factsOf(operation).has(ValueFlag::IsFused); }
    bool isSpilled(const Operation* value) const { return m_spillsOfValues[value->index()] != noSpill; }

    template<FrameCoverage coverage>
    ALWAYS_INLINE FrameAtBlock frameAtBlockUnder(const Block* block, const BlockPlace& place) const
    {
        if constexpr (coverage == FrameCoverage::None)
            return { false, false, false, false };
        else if constexpr (coverage == FrameCoverage::WholeFunction)
            return { true, block->isEntry(), false, false };
        else if constexpr (coverage == FrameCoverage::RareRegions) {
            bool runsInFrame = place.isRare() && m_allocation.needsFrameIn(place.rareRegion);
            return { runsInFrame, runsInFrame && (block->isEntry() || place.entersRareRegion), runsInFrame && place.leavesRareRegion, false };
        } else
            return { m_frame.holdsStepsOf(block), m_frame.isSetUpAtHeadOf(block), m_frame.isTornDownAtEndOf(block), m_frame.returnsInPlaceOfJumpAtEndOf(block) };
    }
    ALWAYS_INLINE FrameAtBlock frameAtBlock(const Block* block, const BlockPlace& place) const
    {
        switch (m_coverage) {
        case FrameCoverage::None:
            return frameAtBlockUnder<FrameCoverage::None>(block, place);
        case FrameCoverage::WholeFunction:
            return frameAtBlockUnder<FrameCoverage::WholeFunction>(block, place);
        case FrameCoverage::RareRegions:
            return frameAtBlockUnder<FrameCoverage::RareRegions>(block, place);
        case FrameCoverage::FramedPart:
            return frameAtBlockUnder<FrameCoverage::FramedPart>(block, place);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    ALWAYS_INLINE FrameAtBlock frameAtBlock(const Block* block) const { return frameAtBlock(block, placeOf(block)); }

    ALWAYS_INLINE bool fallsThroughTo(const Block* from, const Block* target) const
    {
        if (from != m_block) [[unlikely]]
            return fallsThroughToSlowCase(from, target);
        const Block* entered = blockEnteredThrough(target);
        if (entered != m_blockWrittenNext || !entered)
            return false;
        return m_blockIsLeftInFrame == frameAtBlock(entered).isEnteredInFrame();
    }
    bool fallsThroughToSlowCase(const Block* from, const Block* target) const;
    void findIndexesAsWritten() const;

    template<FrameCoverage> void encodeBlocksUnder();
    template<FrameCoverage> ALWAYS_INLINE void encodeBlockUnder(Block*);
    void encodeSteps(uint32_t begin, uint32_t end);
    void encodeExecute(const Step&);
    ALWAYS_INLINE void encodeExecuteInline(const Step&);
    void storeToSpillIfSpilled(const Operation* value)
    {
        if (isSpilled(value)) [[unlikely]]
            storeToSpill(value);
    }
    void storeToSpill(const Operation*);
    void storeSpilledValuesDefinedAtHead(Block*, const BlockPlace&);
    void setUpFrame(const Block*);
    void leaveFrameIfSetUp(std::span<const GPRReg> registersPoppedOutOfWords = { });
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
    void removeRepeatedMoveBlocks(std::span<const uint32_t> layoutIndexesOfMoveBlocks, std::span<const uint32_t> layoutIndexesOfBlocksThatOnlyJumpOn);
    const Block* blockEnteredThrough(const Block* block) const { return m_blocksEntered[block->index()]; }
    void placeColdCodeNearItsJumps();
    bool hasCode(const Block* block) const { return blockEnteredThrough(block) == block; }
    MacroAssembler::Label* labelOf(const Block* block) { return &m_labels[blockEnteredThrough(block)->index()]; }
    ALWAYS_INLINE void jumpTo(const Block* target)
    {
        if (!m_frameIsSetUp && m_blocksThatOnlyReturnWithoutFrame.get(blockEnteredThrough(target)->index())) {
            m_jit.ret();
            return;
        }
        addPendingJump(m_jit.jump(), target);
    }
    ALWAYS_INLINE void addPendingJump(MacroAssembler::Jump jump, const Block* target)
    {
        if (!jump.isSet())
            return;

        const MacroAssembler::Label* label = labelOf(target);
        if (label->isSet()) {
            jump.linkTo(*label, &m_jit);
            return;
        }
        if (m_pendingJumps.size() < m_pendingJumps.capacity()) [[likely]] {
            m_pendingJumps.appendWithinCapacity(PendingJump { jump, label });
            return;
        }
        m_pendingJumps.append(m_arena, PendingJump { jump, label });
    }
    void addPendingJumps(const MacroAssembler::JumpList&, const Block*);

    std::span<const GPRReg> findRegistersThatSetupPushes(const Block*);
    bool setupPushesSavesOfRareRegion(const Block* block) const { return !m_spillsPushedAtSetup.isEmpty() && !block->isEntry() && !m_allocation.isHeadOfFramedPart(block); }
    std::span<const GPRReg> findRegistersThatTeardownPops(uint32_t beginOfSteps, uint32_t endOfSteps, uint32_t& endOfStepsAheadOfPops);
    bool setupHasPushed(SpillIndex spill) const { return !m_spillsPushedAtSetup.isEmpty() && std::ranges::find(m_spillsPushedAtSetup.span(), spill) != m_spillsPushedAtSetup.span().end(); }

    ALWAYS_INLINE AddressForm resolveAddress(const Operation* access, LeafCursor& leaves)
    {
        RELEASE_ASSERT(isMemoryAccess(access->opcode()));
        if (isFused(addressChildWithoutCheck(access)) || !leaves.peek().isRegister()) [[unlikely]]
            return resolveAddressSlowCase(access, leaves);

        Reg base = leaves.take().reg();
        RELEASE_ASSERT(base.isGPR());
        AddressForm form;
        form.base = base.gpr();
        RELEASE_ASSERT(form.base != InvalidGPRReg);
        form.offset = access->offset();
        return form;
    }
    AddressForm resolveAddressSlowCase(const Operation* access, LeafCursor&);
    AddressForm resolveScaledSum(const Operation* sum, LeafCursor&);
    NEVER_INLINE void reportDeadSlotRead() const;
    ALWAYS_INLINE int32_t offsetOfSlot(SpillIndex spill, MemoryOfEncodedMove use) const
    {
        int32_t offset = m_frame.taggedOffsetOf(spill);
        if (use == MemoryOfEncodedMove::IsRead)
            m_readSlotTags |= offset;
        return offset & ~Frame::deadSlotTag;
    }
    ALWAYS_INLINE MacroAssembler::Address addressOf(Location memory, MemoryOfEncodedMove use = MemoryOfEncodedMove::IsRead) const
    {
        switch (memory.kind()) {
        case Location::Spill:
            RELEASE_ASSERT(m_frameIsSetUp);
            return m_frame.addressInFrame(offsetOfSlot(memory.spillIndex(), use));

        case Location::Outgoing:
            RELEASE_ASSERT(m_frameIsSetUp);
            ASSERT(memory.offsetFromSP() < m_frame.bytesBelowFrameRecord());
            return MacroAssembler::Address(MacroAssembler::stackPointerRegister, safeCast<int32_t>(memory.offsetFromSP()));

        case Location::InstanceWord:
            return MacroAssembler::Address(instanceGPR, memory.offsetInInstance());

        case Location::None:
        case Location::Register:
        case Location::SlotBytes:
        case Location::Immediate:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    std::pair<GPRReg, bool> referenceOf(const Operation* access, const AddressForm&) const;
    void recordTrappingAccess(const Operation* access, const AddressForm&, MacroAssembler::Label instruction);

    ALWAYS_INLINE void encodeMove(const Step&);
    ALWAYS_INLINE void encodeReload(const Step&);
    ALWAYS_INLINE void encodeSave(const Step&);
    void encodeSwap(const Step&);
    void encodeKeepPointer(const Step&);
    void encodeMaterialize(const Operation*, Reg destination);
    bool tryEncodePairOfMoves(const Step& first, const Step& second);

    ALWAYS_INLINE void encodePatch(const Step&, PatchOperation*);
    void encodeCheck(const Step&, PatchOperation*);
    ALWAYS_INLINE ValueRep repOf(const PatchOperation* patch, Location location, const Operation* value, MemoryOfEncodedMove access = MemoryOfEncodedMove::IsRead) const
    {
        switch (location.kind()) {
        case Location::Register:
            return ValueRep::reg(location.reg());
        case Location::Spill:
            RELEASE_ASSERT(m_frameIsSetUp);
            RELEASE_ASSERT(m_frame.hasFrameRecord() || patch->findsOperandsInFrameWithoutRecord());
            return ValueRep::stack(offsetOfSlot(location.spillIndex(), access));
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
    template<bool isCheck> ALWAYS_INLINE void fillParameters(PatchParameters&, const Step&, PatchOperation*);
    struct SharedLatePath {
        PatchGenerator* generator;
        std::span<const ValueRep> reps;
        bool frameIsSetUp;
        MacroAssembler::Label* entry;
    };
    SharedLatePath* findSharedLatePath(PatchOperation*, std::span<const ValueRep>);

    ALWAYS_INLINE void encodeTerminatorInline(Block* block, const Step& step)
    {
        ASSERT(step.kind == StepKind::Execute && step.operation == block->last());

        if (step.operation->isUnconditionalJump()) {
            const Block* target = block->successor(0).block();
            if (!fallsThroughTo(block, target))
                jumpTo(target);
            return;
        }
        if (step.operation->opcode() == Branch) {
            encodeBranch(block, step);
            return;
        }
        encodeRareTerminator(block, step);
    }
    void encodeTerminator(Block*, const Step&);
    void encodeRareTerminator(Block*, const Step&);
    void encodeBranch(Block*, const Step&);
    void encodeBranchOnTest(Block*, const Step&, const Operation* predicate);
    void encodeBranchOnOverflow(Block*, const Step&);
    void encodeSwitch(Block*, const Step&);
    void encodeSwitchByBits(Block*, GPRReg value, Width, unsigned begin, unsigned end);
    void encodeSwitchThroughTable(Block*, GPRReg value, int64_t firstValue, uint64_t numberOfEntries);
    void encodeBinarySearch(Block*, GPRReg value, Width, std::span<const int64_t> values, unsigned firstSuccessor);
    void encodeReturn(const Step&);
    void noteReturnThatJumpsMayCopy();
    Test resolveTest(const Operation* user, LeafCursor&);

    MacroAssembler::JumpList branchOn(const Test&, bool whenItHolds);
#if CPU(X86_64)
    MacroAssembler::Jump jumpOnIntegers(const Test&, bool whenItHolds);
    std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfPredicate(const Step&, const Operation* predicate);
#endif
    void encodeComparison(const Step&);
    void encodeSelect(const Step&);

    void encodeInteger(const Step&);
    bool tryEncodeSumAndConstant(const Step& sum, const Step& sumWithConstant);
    void encodeSumThatSetsFlags(const Step&);
    MacroAssembler::Jump jumpOnOverflow(bool whenItOverflowed);

    void encodeNumber(const Step&);
    void materializeBitsInFPR(uint64_t bits, Width, FPRReg destination);
    bool blockHasWrittenNumberRegisterInFrontOf(const Step&, FPRReg);
    bool writesNumberRegister(const Step&, FPRReg) const;
    bool wasNumberRegisterWrittenRecently(const Step&, FPRReg) const;

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
    const BlockPlace* const m_places;
    const BlockSteps* const m_stepsOfBlocks;
    const Step* const m_steps;
    const SpillIndex* const m_spillsOfValues;
    const ValueFacts* const m_valueFacts;
    mutable int32_t m_readSlotTags { 0 };

    UncheckedSpan<MacroAssembler::Label> m_labels;
    UncheckedSpan<const Block*> m_blocksEntered;
    BitSpan m_blocksThatOnlyReturnWithoutFrame;
    BitSpan m_blocksThatStoreResultOfPatchAtHead;
    UncheckedSpan<uint32_t> m_nextLayoutIndexWithCode;
    uint32_t m_numberOfBlocksWithCode { 0 };
    uint32_t m_numberOfOperationsEncoded { 0 };
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
    mutable UncheckedSpan<uint32_t> m_indexAsWritten;
    uint32_t m_beginOfRareCodeAsWritten { 0 };
    bool m_isAmongOtherCode { false };
    Block* m_block { nullptr };
    const Block* m_blockWrittenNext { nullptr };
    bool m_blockIsLeftInFrame { false };
    bool m_frameIsSetUp { false };
    bool m_frameRangeIsOpen { false };
    ArenaVector<GPRReg> m_registersPushedAtSetup;
    ArenaVector<SpillIndex> m_spillsPushedAtSetup;
    ArenaVector<GPRReg> m_registersPoppedAtTeardown;
    MacroAssembler::Label m_teardownSharedByReturns;
    Origin m_origin;
    uint32_t m_operationOfOrigin { UINT32_MAX };
    const Block* m_blockWithClearedScratchFPR { nullptr };
    unsigned m_offsetBehindClearedScratchFPR { 0 };
    const Block* m_blockWhoseWrittenNumberRegistersAreNoted { nullptr };
    const Step* m_firstStepNotLookedAtForNumberRegisters { nullptr };
    RegisterMask m_numberRegistersWrittenInBlock { 0 };
};

inline bool encodedMoveIsWide(Width width)
{
    RELEASE_ASSERT(width == Width32 || width == Width64);
    return width == Width64;
}

inline void checkRegisterOfEncodedMove(Reg reg)
{
    RELEASE_ASSERT(reg);
    RELEASE_ASSERT(!reg.isGPR() || reg.gpr() != secondScratchGPR);
}

inline void willWriteRegisterOfEncodedMove(CCallHelpers& jit, Reg destination)
{
    checkRegisterOfEncodedMove(destination);
    if (destination.isGPR() && destination.gpr() == scratchGPR)
        jit.scratchRegister();
}

ALWAYS_INLINE void checkMemoryOfEncodedMove(const Encoder& encoder, Location memory, Width width, MemoryOfEncodedMove access)
{
    switch (memory.kind()) {
    case Location::Spill:
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        RELEASE_ASSERT(bytesForWidth(width) <= bytesForWidth(encoder.m_allocation.spills()[memory.spillIndex()].width));
        return;
    case Location::Outgoing:
        RELEASE_ASSERT(encoder.m_frameIsSetUp);
        return;
    case Location::InstanceWord:
        RELEASE_ASSERT(access == MemoryOfEncodedMove::IsRead);
        return;
    case Location::None:
    case Location::Register:
    case Location::SlotBytes:
    case Location::Immediate:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE void loadForEncodedMove(CCallHelpers& jit, MacroAssembler::Address source, Width width, Reg destination)
{
    bool isWide = encodedMoveIsWide(width);
    willWriteRegisterOfEncodedMove(jit, destination);
    if (destination.isGPR()) {
        if (isWide)
            jit.load64(source, destination.gpr());
        else
            jit.load32(source, destination.gpr());
        return;
    }
    if (isWide)
        jit.loadDouble(source, destination.fpr());
    else
        jit.loadFloat(source, destination.fpr());
}

ALWAYS_INLINE void storeForEncodedMove(CCallHelpers& jit, Reg source, Width width, MacroAssembler::Address destination)
{
    bool isWide = encodedMoveIsWide(width);
    checkRegisterOfEncodedMove(source);
    if (source.isGPR()) {
        if (isWide)
            jit.store64(source.gpr(), destination);
        else
            jit.store32(source.gpr(), destination);
        return;
    }
    if (isWide)
        jit.storeDouble(source.fpr(), destination);
    else
        jit.storeFloat(source.fpr(), destination);
}

ALWAYS_INLINE void Encoder::encodeMove(const Step& step)
{
    ASSERT(step.kind == StepKind::Move);
    RELEASE_ASSERT(step.destination.isRegister() && step.sources[0].isRegister());
    Reg source = step.sources[0].reg();
    Reg destination = step.destination.reg();
    checkRegisterOfEncodedMove(source);
    RELEASE_ASSERT(bankOf(source) == bankOf(destination));
    bool isWide = encodedMoveIsWide(step.width);
    if (source == destination)
        return;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    willWriteRegisterOfEncodedMove(m_jit, destination);
    if (destination.isFPR()) {
        m_jit.moveDouble(source.fpr(), destination.fpr());
        return;
    }
    if (isWide)
        m_jit.move(source.gpr(), destination.gpr());
    else
        m_jit.zeroExtend32ToWord(source.gpr(), destination.gpr());
}

ALWAYS_INLINE void Encoder::encodeReload(const Step& step)
{
    ASSERT(step.kind == StepKind::Reload);
    RELEASE_ASSERT(step.destination.isRegister());
    checkMemoryOfEncodedMove(*this, step.sources[0], step.width, MemoryOfEncodedMove::IsRead);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    loadForEncodedMove(m_jit, addressOf(step.sources[0]), step.width, step.destination.reg());
}

ALWAYS_INLINE void Encoder::encodeSave(const Step& step)
{
    ASSERT(step.kind == StepKind::Save);
    RELEASE_ASSERT(step.sources[0].isRegister());
    checkMemoryOfEncodedMove(*this, step.destination, step.width, MemoryOfEncodedMove::IsWritten);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    storeForEncodedMove(m_jit, step.sources[0].reg(), step.width, addressOf(step.destination, MemoryOfEncodedMove::IsWritten));
}

ALWAYS_INLINE bool patchRepAnswersConstraint(const ValueRep& answer, const ValueRep& constraint)
{
    switch (constraint.kind()) {
    case ValueRep::WarmAny:
    case ValueRep::ColdAny:
    case ValueRep::LateColdAny:
        return true;
    case ValueRep::SomeRegister:
    case ValueRep::SomeEarlyRegister:
    case ValueRep::SomeLateRegister:
        return answer.isReg();
    case ValueRep::Register:
    case ValueRep::StackArgument:
        return answer == constraint;
    case ValueRep::Stack:
    case ValueRep::Constant:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE bool patchRegisterAnswersConstraint(const ValueRep& answer, const ValueRep& constraint)
{
    static_assert(ValueRep::WarmAny < ValueRep::Register && ValueRep::ColdAny < ValueRep::Register && ValueRep::LateColdAny < ValueRep::Register);
    static_assert(ValueRep::SomeRegister < ValueRep::Register && ValueRep::SomeEarlyRegister < ValueRep::Register && ValueRep::SomeLateRegister < ValueRep::Register);
    static_assert(ValueRep::StackArgument > ValueRep::Register && ValueRep::Stack > ValueRep::Register && ValueRep::Constant > ValueRep::Register);
    ASSERT(answer.isReg());
    return constraint.kind() < ValueRep::Register || answer == constraint;
}

template<bool isCheck>
ALWAYS_INLINE void Encoder::fillParameters(PatchParameters& parameters, const Step& step, PatchOperation* patch)
{
    ASSERT(patch->opcode() == (isCheck ? Check : Patch));
    RELEASE_ASSERT(step.patchAssignment != UINT32_MAX);
    const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
    std::span<const Location> locations = assignment.locations;
    std::span<Operation* const> operands = patch->operands();

    unsigned numberOfResults = m_proc.resultCount(patch->type());
    constexpr unsigned firstOperand = isCheck ? 1 : 0;
    unsigned numberOfOperands = static_cast<unsigned>(operands.size()) - firstOperand;
    RELEASE_ASSERT(locations.size() == numberOfResults + numberOfOperands);

    if (size_t numberOfReps = locations.size()) {
        static_assert(alignof(ValueRep) <= Arena::alignmentOfCursor);
        const size_t bytesOfReps = numberOfReps * sizeof(ValueRep);
        ValueRep* reps = static_cast<ValueRep*>(m_arena.hasRoomFor(bytesOfReps) ? m_arena.allocateWhereThereIsRoom(bytesOfReps) : m_arena.allocate(bytesOfReps, alignof(ValueRep)));
        const Location* location = locations.data();
        ValueRep* rep = reps;
        static constexpr ValueRep someRegister { ValueRep::SomeRegister };
        for (unsigned index = 0; index < numberOfResults; ++index, ++location, ++rep) {
            const ValueRep& constraint = index < patch->resultConstraints.size() ? patch->resultConstraints[index] : someRegister;
            if (location->isRegister()) {
                new (NotNull, rep) ValueRep(location->reg());
                RELEASE_ASSERT(patchRegisterAnswersConstraint(*rep, constraint));
                continue;
            }
            new (NotNull, rep) ValueRep(repOf(patch, *location, patch, MemoryOfEncodedMove::IsWritten));
            RELEASE_ASSERT(patchRepAnswersConstraint(*rep, constraint));
        }
        Operation* const* operand = operands.data() + firstOperand;
        for (unsigned child = firstOperand; child < firstOperand + numberOfOperands; ++child, ++location, ++rep, ++operand) {
            const ValueRep& constraint = patch->constraintOf(child);
            if (location->isRegister()) {
                new (NotNull, rep) ValueRep(location->reg());
                RELEASE_ASSERT(patchRegisterAnswersConstraint(*rep, constraint));
                continue;
            }
            new (NotNull, rep) ValueRep(repOf(patch, *location, *operand));
            RELEASE_ASSERT(patchRepAnswersConstraint(*rep, constraint));
        }
        parameters.m_reps = std::span<const ValueRep>(reps, numberOfReps);
    }

    std::span<GPRReg> gpScratch(parameters.m_gpScratch);
    unsigned numberOfGPScratch = 0;
    if (assignment.gpScratch) [[unlikely]] {
        forEachRegister(assignment.gpScratch, [&](Reg reg) {
            RELEASE_ASSERT(numberOfGPScratch < gpScratch.size());
            gpScratch[numberOfGPScratch++] = reg.gpr();
        });
    }
    RELEASE_ASSERT(numberOfGPScratch == patch->numGPScratchRegisters);

    std::span<FPRReg> fpScratch(parameters.m_fpScratch);
    unsigned numberOfFPScratch = 0;
    if (assignment.fpScratch) [[unlikely]] {
        forEachRegister(assignment.fpScratch, [&](Reg reg) {
            RELEASE_ASSERT(numberOfFPScratch < fpScratch.size());
            fpScratch[numberOfFPScratch++] = reg.fpr();
        });
    }
    RELEASE_ASSERT(numberOfFPScratch == patch->numFPScratchRegisters);

    parameters.m_usedRegisters = assignment.usedRegisters;
    parameters.m_frameIsSetUp = m_frameIsSetUp;

    if constexpr (!isCheck) {
        if (!patch->effects.terminal)
            return;
        const Block* block = patch->owner();
        ASSERT(block == m_block);
        std::span<MacroAssembler::Label*> labels = m_arena.createSpan<MacroAssembler::Label*>(block->numSuccessors());
        for (unsigned index = 0; index < block->numSuccessors(); ++index)
            labels[index] = labelOf(block->successor(index).block());
        parameters.m_successorLabels = labels;
    }
}

ALWAYS_INLINE void Encoder::encodePatch(const Step& step, PatchOperation* patch)
{
    ASSERT(patch->opcode() == Patch);
    ASSERT(!patch->onlyKeepsChildrenAlive);
    RELEASE_ASSERT(patch->generator());
    RELEASE_ASSERT(m_frameIsSetUp || patch->runsWithoutFrame());

    PatchParameters parameters(*this, patch);
    fillParameters<false>(parameters, step, patch);
    patch->generator()->run(m_jit, parameters);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
