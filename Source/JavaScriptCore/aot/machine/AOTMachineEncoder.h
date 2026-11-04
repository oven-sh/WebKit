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

// The sources of a step, taken in the order of forEachLeaf(). Whoever encodes an operation walks its children in rising order, goes down into a fused child, and takes one location for each leaf.
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

// The address of a load or a store, with everything that was fused into it.
struct AddressForm {
    enum Kind : uint8_t {
        // [base + offset]
        BaseOffset,
        // [base + (extended index << shift) + offset], which isEncodableIndexedAddress() has allowed.
        BaseIndex,
        // The word of the instance that `symbol` names. RelocationRecorder writes the access.
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

// What a Branch, a Select or a Check tests, with everything that was fused into it.
struct Test {
    enum Kind : uint8_t {
        // left `relation` right, as integers of `width`.
        CompareIntegers,
        // As numbers of `width`.
        CompareNumbers,
        // (left & right) is zero, or is not: relation is Equal or NotEqual.
        TestBits,
    };

    Kind kind;
    Opcode relation;
    Width width;
    Location left;
    // An immediate is the constant `rightConstant`.
    Location right;
    int64_t rightConstant { 0 };
    // x86-64: the left operand is what this fused load reads at `addressOnLeft`, and `left` is no location. The instruction reads it where it lies.
    const Operation* loadOnLeft { nullptr };
    AddressForm addressOnLeft { };
    // x86-64, TestBits: one bit of `left` is tested, and `right` holds its number, modulo `width`. Both are registers.
    bool rightIsNumberOfBit { false };
};

// A jump takes four bytes on ARM64, and two or five on x86-64.
inline constexpr unsigned minBytesOfTeardownSharedByReturns = isARM64() ? 12 : 8;

// Step 6: one pass over the steps of the allocation, in the order of the layout, into one assembler. Each method is defined in the file named above it.
// The files with ARM64 or X86_64 in their names hold the same methods, each within #if CPU().
// Everything is public: a helper that this header does not declare is a static function of the file that needs it, takes Encoder&, and has a name that no other file of aot/ uses.
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

    // ---- AOTMachineEncoder.cpp: blocks, labels, frames, late paths.

    void encodeBlock(Block*);
    void encodeSteps(uint32_t begin, uint32_t end);
    void encodeExecute(const Step&);
    ALWAYS_INLINE void encodeExecuteInline(const Step&);
    // Behind the definition of a spilled value: the one store of it. For the Phis of a block and the results of a Patch that ended the block before: at the head.
    void storeToSpillIfSpilled(const Operation* value)
    {
        if (m_allocation.isSpilled(value)) [[unlikely]]
            storeToSpill(value);
    }
    void storeToSpill(const Operation*);
    void storeSpilledValuesDefinedAtHead(Block*, const BlockPlace&);
    // Sets up or tears down where the state that the block needs differs from the state it is entered in, and keeps Procedure::frameRanges() in step with the code as it is laid out.
    void setUpFrame(const Block*);
    void leaveFrameIfSetUp();
    void noteFrameState(bool isSetUp)
    {
        m_frameIsSetUp = isSetUp;
        if (m_frameRangeIsOpen != isSetUp) [[unlikely]]
            startOrEndFrameRange(isSetUp);
    }
    void startOrEndFrameRange(bool isSetUp);
    // What is written from here on was appended for this origin, or for no operation of the emitters where it is null. Keeps Procedure::rangesOfOrigins() in step with the code. Writes no byte.
    void noteOrigin(Origin origin, uint32_t operation = UINT32_MAX)
    {
        if (m_notesRangesOfOrigins) [[unlikely]]
            noteOriginInRanges(origin, operation);
    }
    void noteOriginInRanges(Origin, uint32_t operation);
    bool blockRunsInFrame(const Block*) const;
    void runLatePaths();
    void linkPendingJumps();
    // A block that only jumps on, with nothing to move on the way, has no code: whoever names it enters the block that it leads to.
    // It reads the steps of each block once, and fills m_blocksThatOnlyReturnWithoutFrame on the way.
    void findBlocksWithoutCode();
    const Block* blockEnteredThrough(const Block* block) const { return m_blocksEntered[block->index()]; }
    // On x86-64 a jump that reaches no further than 127 bytes takes two bytes for five or six, and most jumps that reach further lead to rare or unlikely blocks, or back from them.
    // The layout is a row of runs, each of which ends with a block that nothing falls out of. A rare region, or a run of unlikely blocks, may stand between any two of them: no instruction more is run for it.
    // A small one is written in front of or behind the run that jumps into it first, whichever is nearer. Sizes are guessed from the steps.
    // Fills m_blocksAsWritten, or leaves it empty where the layout stands.
    void placeColdCodeNearItsJumps();
    bool hasCode(const Block* block) const { return blockEnteredThrough(block) == block; }
    MacroAssembler::Label* labelOf(const Block* block) { return &m_labels[blockEnteredThrough(block)->index()]; }
    // Where no frame is set up and the block holds a Return alone, it writes the return.
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

    // ---- AOTMachineEncoderPushes.cpp

    // Of a block that sets the frame up at its head. x86-64: the stores that stand right behind the setup and fill the first words of what SlotAssignment lays out, without a gap: the Saves of a head of the framed part,
    // and at an entrypoint the stores behind the ArgumentRegs that begin the block. The setup pushes those words from the registers returned, the highest word first, and setupHasPushed() holds of their spills.
    // Nothing where the pushes would make the setup longer by an instruction.
    std::span<const GPRReg> findRegistersThatSetupPushes(const Block*);
    // Of the last setup that was written. A spill has one store behind its definition, and Saves at heads alone, each of which sets the frame up: so what is left over from an earlier block names no store of a later one.
    bool setupHasPushed(SpillIndex spill) const { return !m_spillsPushedAtSetup.isEmpty() && std::ranges::find(m_spillsPushedAtSetup.span(), spill) != m_spillsPushedAtSetup.span().end(); }

    // ---- AOTMachineEncoderAddresses.cpp

    // Takes the leaves of the address from the cursor. A base without position costs no register: a SlotBase and the FramePointer are the stack pointer or the frame pointer with an offset,
    // an Add without position is its base register with an offset, an InstanceAddressOf is a symbol.
    AddressForm resolveAddress(const Operation* access, LeafCursor&);
    // Of an Add of which addsScaledIndex() holds: base + (extended index << shift) + offset, which isEncodableScaledSum() has allowed. The offset is what a base without position adds to its register.
    AddressForm resolveScaledSum(const Operation* sum, LeafCursor&);
    MacroAssembler::Address addressOf(Location memory) const;
    // The register of the address that holds the reference which a fault would be blamed on, and whether it holds the narrow form.
    std::pair<GPRReg, bool> referenceOf(const Operation* access, const AddressForm&) const;
    void recordTrappingAccess(const Operation* access, const AddressForm&, MacroAssembler::Label instruction);

    // ---- AOTMachineEncoderMoves.cpp: every step but Execute.

    void encodeMove(const Step&);
    void encodeReload(const Step&);
    void encodeSave(const Step&);
    void encodeSwap(const Step&);
    void encodeClearSpill(const Step&);
    void encodeKeepPointer(const Step&);
    // Of any operation without position. A constant that findPinnedConstant() knows comes from its register or its word. Any other number comes from immediates where that is no longer than reading it,
    // otherwise from the constants of the function, read pc-relative. May write the scratch GPR, and on x86-64 the flags.
    void encodeMaterialize(const Operation*, Reg destination);
    // ARM64: two Saves or two Reloads of Width64 to neighboring spills.
    bool tryEncodePairOfMoves(const Step& first, const Step& second);

    // ---- AOTMachineEncoderPatches.cpp: Patch and Check, and the methods of PatchParameters and ValueRep.

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
    // The late path of an earlier Check with the same generator, the same reps and the same frame state, if this one shares.
    struct SharedLatePath {
        PatchGenerator* generator;
        std::span<const ValueRep> reps;
        bool frameIsSetUp;
        MacroAssembler::Label* entry;
    };
    SharedLatePath* findSharedLatePath(PatchOperation*, std::span<const ValueRep>);

    // ---- AOTMachineEncoderControl.cpp: what ends a block.

    // Where Operation::isUnconditionalJump() holds: a jump, or nothing where the block falls through.
    void encodeTerminator(Block*, const Step&);
    void encodeBranch(Block*, const Step&);
    // A Branch with ValueFlag::GoesByOverflow. What widens the sum or takes it back stands on a late path, in front of a jump to the failing successor.
    void encodeBranchOnOverflow(Block*, const Step&);
    // Dense values from minCasesForJumpTable on: bounds test, then a jump through a table of 32-bit distances from the table to the targets. Otherwise a binary search.
    void encodeSwitch(Block*, const Step&);
    void encodeSwitchThroughTable(Block*, GPRReg value, int64_t firstValue, uint64_t numberOfEntries);
    void encodeBinarySearch(Block*, GPRReg value, Width, std::span<const int64_t> values, unsigned firstSuccessor);
    void encodeReturn(const Step&);
    // Reads the predicate of a Branch, Select or Check: child 0, through simplifyPredicate()'s shapes and what the scan fused.
    // A fused load and a predicate without position go into the scratch register of their bank, so it may emit code. Nothing else writes those registers until the test is encoded.
    Test resolveTest(const Operation* user, LeafCursor&);

    // ---- AOTMachineEncoderComparisonsARM64.cpp, AOTMachineEncoderComparisonsX86_64.cpp

    // Jumps where the test holds, or where it does not.
    MacroAssembler::JumpList branchOn(const Test&, bool whenItHolds);
#if CPU(X86_64)
    MacroAssembler::Jump jumpOnIntegers(const Test&, bool whenItHolds);
    std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfPredicate(const Step&);
#endif
    // The eleven comparisons with their result in a register.
    void encodeComparison(const Step&);
    void encodeSelect(const Step&);

    // ---- AOTMachineEncoderIntegersARM64.cpp, AOTMachineEncoderIntegersX86_64.cpp

    // Add, Sub, Mul, Neg, BitAnd, BitOr, BitXor, Shl, SShr, ZShr, Clz, PopCount, SExt8, SExt16, SExt32, ZExt32, Trunc of integers, and Opaque.
    // ARM64 alone: MulHigh, UMulHigh, Div, UDiv, chill or not, and a shift by a register.
    // What it writes last for an operation of which clearsUpperHalfOfRegister() holds writes 32 bits of the destination. It writes nothing where isFreeInRegisterOfChild() holds and the registers agree.
    // An Add of which addsScaledIndex() holds is one instruction, and its destination may be the register of any leaf.
    void encodeInteger(const Step&);
    // x86-64: an Int32 Add of two registers that the Add of a constant behind it alone reads. One lea computes both, and the register of the first Add is not written.
    bool tryEncodeSumAndConstant(const Step& sum, const Step& sumWithConstant);
    // An Add or a Sub with ValueFlag::GoesByOverflow. The last instruction it writes that sets the flags is the addition or subtraction of 32 bits, and what it writes last clears the upper half of the destination.
    void encodeSumThatSetsFlags(const Step&);
    // Behind that, with nothing in between that writes the flags. Jumps where it overflowed, or where it did not.
    MacroAssembler::Jump jumpOnOverflow(bool whenItOverflowed);

    // ---- AOTMachineEncoderNumbersARM64.cpp, AOTMachineEncoderNumbersX86_64.cpp

    // Add, Sub, Mul, Div, Neg of numbers, Abs, Ceil, Floor, FTrunc, Sqrt, FMin, FMax, IToD, FloatToDouble, DoubleToFloat, BitwiseCast.
    void encodeNumber(const Step&);

    // ---- AOTMachineEncoderMemoryARM64.cpp, AOTMachineEncoderMemoryX86_64.cpp

    void encodeLoad(const Step&);
    void encodeStore(const Step&);
    void encodeFence();
    // ARM64: two Stores of one width through one base register to neighboring words.
    bool tryEncodePairOfStores(const Step& first, const Step& second);
    // A Branch with ValueFlag::WorksInMemory. On x86-64 what takes an update back stands on a late path, in front of a jump to the successor that does not store.
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

    // By Block::index(). It does not move: generators and records keep pointers into it.
    UncheckedSpan<MacroAssembler::Label> m_labels;
    // By Block::index(). The block itself where it has code.
    UncheckedSpan<const Block*> m_blocksEntered;
    // By Block::index(): plannedBlockOnlyReturns() holds of it, it runs without a frame, and it is neither an entrypoint nor a landing pad.
    BitSpan m_blocksThatOnlyReturnWithoutFrame;
    BitSpan m_blocksThatStoreResultOfPatchAtHead;
    // By BlockPlace::layoutIndex, and one more for the end: where the first block from there on that has code lies in the layout.
    UncheckedSpan<uint32_t> m_nextLayoutIndexWithCode;
    uint32_t m_numberOfBlocksWithCode { 0 };
    struct PendingJump {
        MacroAssembler::Jump jump;
        const MacroAssembler::Label* label;
    };
    ArenaVector<PendingJump> m_pendingJumps;
    // A table of Procedure::jumpTables() whose targets wait for the labels of their blocks. linkPendingJumps() copies them.
    struct PendingJumpTable {
        // In Procedure::jumpTables().
        uint32_t index;
        // One for each entry of the table, holes too: JumpTableRecord::targets has the same size.
        std::span<const Block* const> targets;
    };
    ArenaVector<PendingJumpTable> m_pendingJumpTables;
    struct PendingLatePath {
        LatePath* path;
        bool frameIsSetUp;
        // m_origin where it was added: of the Patch or Check whose generator added it, or of the late path that did.
        Origin origin;
        uint32_t operation;
    };
    ArenaVector<PendingLatePath> m_latePaths;
    ArenaVector<SharedLatePath> m_sharedLatePaths;
    // Finds a late path in m_sharedLatePaths by its generator. Open addressing: an index into that list, or UINT32_MAX where empty. A power of two in size, at least half empty.
    std::span<uint32_t> m_sharedLatePathSlots;
    // The blocks that have code, in the order in which they are written. Empty where that is the layout.
    ArenaVector<Block*> m_blocksAsWritten;
    // By Block::index(), for the blocks of m_blocksAsWritten.
    UncheckedSpan<uint32_t> m_indexAsWritten;
    // In m_blocksAsWritten: from here on stands the rare code that has found no place among the other.
    uint32_t m_beginOfRareCodeAsWritten { 0 };
    bool m_isAmongOtherCode { false };
    Block* m_block { nullptr };
    const Block* m_blockWrittenNext { nullptr };
    bool m_blockIsLeftInFrame { false };
    bool m_frameIsSetUp { false };
    bool m_frameRangeIsOpen { false };
    // By the word, counted down from the top of what SlotAssignment lays out.
    ArenaVector<GPRReg> m_registersPushedAtSetup;
    ArenaVector<SpillIndex> m_spillsPushedAtSetup;
    // In front of the teardown and the return that the first Return in a frame has written. Not set where they are shorter than minBytesOfTeardownSharedByReturns.
    // The Returns of rare regions jump there. Any other writes its own.
    MacroAssembler::Label m_teardownSharedByReturns;
    // Of the code that is being written.
    Origin m_origin;
    uint32_t m_operationOfOrigin { UINT32_MAX };
    // x86-64: the scratch register for numbers was cleared for a store of that block, which ends at that offset: tryEncodePairOfStores().
    const Block* m_blockWithClearedScratchFPR { nullptr };
    unsigned m_offsetBehindClearedScratchFPR { 0 };
};

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
