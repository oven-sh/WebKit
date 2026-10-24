/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDirectCodeGenerator.h"

#if ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)

#include "AOTASANShadowChecks.h"
#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTFunctionCodeGenerator.h"
#include "AOTRelocation.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStubRegisters.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirGenerationContext.h"
#include "AirHandleCalleeSaves.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirStackAllocation.h"
#include "AirStackSlot.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3Common.h"
#include "B3ExtractValue.h"
#include "B3FenceValue.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3RelocatableValue.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3SwitchValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr unsigned numberOfRegisters = Reg::maxIndex() + 1;
static_assert(numberOfRegisters <= 64);

inline uint64_t bitOfRegister(Reg reg) { return static_cast<uint64_t>(1) << reg.index(); }
inline uint64_t bitsOf(const RegisterSet& registers) { return registers.toScalarRegisterSet().bitsForDebugging(); }
inline uint8_t encode(Reg reg) { return static_cast<uint8_t>(reg.index() + 1); }
inline Reg decode(uint8_t encoded) { return encoded ? Reg::fromIndex(encoded - 1) : Reg(); }
constexpr uint8_t arrivesInSlot = 0xff;
constexpr uint8_t arrivesInHome = 0xfe;

enum class ValueForm : uint8_t {
    Plain,
    Dead,
    Constant,
    Alias,
    FixedRegister,
    Rematerialized,
    Folded,
};

enum ValueFlag : uint16_t {
    NeedsHome = 1,
    IsZeroExtended = 2,
    IsDouble = 4,
    IsWide = 8,
    IsFullClobber = 16,
    ArrivesInSlot = 32,
    MayNeedHome = 64,
    HasSeveralSources = 128,
    IsReadWide = 256,
    HasSourceWithHighBits = 512,
    WasLetGoUnsaved = 1024,
};

struct ValueState {
    union {
        int64_t constant;
        const B3::Value* target;
    };
    uint32_t position;
    uint32_t lastUse;
    uint32_t stretch;
    uint32_t stretchBefore;
    uint32_t shortStretch;
    uint32_t shortStretchBefore;
    uint32_t memoryEpoch;
    uint32_t block;
    uint32_t nextPhi;
    uint32_t phi;
    uint32_t homeIndex;
    uint32_t lastUseInOwnBlock;
    int32_t home;
    int32_t shadow;
    uint16_t rawUses;
    uint16_t uses;
    uint16_t flags;
    ValueForm form;
    uint8_t reg;
    uint8_t hint;
    uint8_t arrival;
    uint8_t base;
};

struct HomeState {
    uint32_t value;
    uint32_t firstExposedUse;
    uint32_t numberOfExposedUses;
    uint32_t firstOrderedUse;
    uint32_t nextDefinedInBlock;
    uint32_t parent;
    uint32_t nextInWeb;
    uint32_t lastInWeb;
    uint32_t webSize;
    uint32_t firstUpsilon;
    uint32_t phiFed;
    uint32_t numberOfPhisFed;
    uint32_t start;
    uint32_t end;
    uint32_t accesses;
    uint32_t slot;
    uint32_t stretchOfLastUse;
    int32_t gain;
    int32_t gainOfWeb;
    uint8_t gainInStretch;
    uint8_t homeRegister;
    bool holdsArgument;
    bool holdsDouble;
};

struct ExposedUse {
    uint32_t block;
    uint32_t home;
    uint32_t lastPosition;
    uint32_t next;
};

struct OrderedUse {
    uint32_t block;
    uint32_t lastPosition;
};

struct UpsilonRecord {
    const B3::UpsilonValue* upsilon;
    uint32_t block;
    uint32_t nextForPhi;
};

struct KnownLoad {
    const B3::Value* load { nullptr };
    uint32_t memoryEpoch { 0 };
};

struct Loop {
    uint32_t firstBlock;
    uint32_t lastBlock;
    uint32_t parent;
    uint32_t firstPosition;
    uint32_t lastPosition;
};

struct RegisterFile {
    std::array<uint32_t, numberOfRegisters> holder;
    uint64_t occupied;
    uint64_t dirty;
};

struct DirectBlock {
    B3::BasicBlock* block { nullptr };
    uint32_t firstPosition { 0 };
    uint32_t lastPosition { 0 };
    uint32_t stretchBeforeTerminator { 0 };
    uint32_t stretchAtEnd { 0 };
    uint32_t shortStretchBeforeTerminator { 0 };
    uint32_t shortStretchAtEnd { 0 };
    uint32_t numberOfPredecessors { 0 };
    uint32_t predecessor { 0 };
    uint32_t firstPhi { 0 };
    uint32_t loop { 0 };
    uint32_t snapshot { 0 };
    uint32_t emissionIndex { 0 };
    uint32_t storedAtEntry { 0 };
    uint32_t firstDefinition { 0 };
    uint32_t memoryEpochAtEnd { 0 };
    uint32_t forwardsTo { UINT32_MAX };
    uint32_t numberOfGeneralPhis { 0 };
    uint32_t numberOfDoublePhis { 0 };
    uint64_t arrivalRegisters { 0 };
    bool isRare { false };
    bool isReachedNormally { false };
    bool startsEmpty { false };
    bool hasBackEdgePredecessor { false };
    bool branchesToOneBlock { false };
    bool isReachable { false };
    bool usesFrame { false };
    bool runsInFrame { true };
    uint32_t firstSuccessor { 0 };
    bool hasOffer { false };
    bool hasOfferFromCodeAhead { false };
    bool continuesStretch { false };
    uint8_t onlySuccessor { 0 };
    uint32_t offeredStretch { 0 };
    uint32_t offeredShortStretch { 0 };
    uint32_t entry { 0 };
    uint32_t incomingEdges { 0 };
    uint32_t arrivedEdges { 0 };
    double frequency { 1 };
    MacroAssembler::Label label;
    MacroAssembler::JumpList jumps;
    Box<MacroAssembler::Label> sharedLabel;
};

struct User {
    uint32_t position { 0 };
    uint32_t stretch { 0 };
    uint32_t shortStretch { 0 };
    uint32_t memoryEpoch { 0 };
    uint32_t block { 0 };
};

struct AddressPlan {
    const B3::Value* base { nullptr };
    const B3::Value* index { nullptr };
    const B3::Air::StackSlot* slot { nullptr };
    GPRReg fixedBase { InvalidGPRReg };
    uint8_t shift { 0 };
    int32_t offset { 0 };
};

struct MemoryOperand {
    GPRReg base { InvalidGPRReg };
    GPRReg index { InvalidGPRReg };
    uint8_t shift { 0 };
    int32_t offset { 0 };

    bool hasIndex() const { return index != InvalidGPRReg; }
    MacroAssembler::Address address() const { return MacroAssembler::Address(base, offset); }
    MacroAssembler::BaseIndex baseIndex() const { return MacroAssembler::BaseIndex(base, index, static_cast<MacroAssembler::Scale>(shift), offset); }
};

enum class OperandShape : uint8_t {
    Register,
    Immediate,
    PinnedMemory,
    PinnedRegister,
    Load,
    ScaledIndex,
    BitTest,
};

struct OperandPlan {
    OperandShape shape { OperandShape::Register };
    const B3::Value* left { nullptr };
    const B3::Value* right { nullptr };
    const B3::Value* load { nullptr };
    const B3::Value* foldedShift { nullptr };
    int64_t immediate { 0 };
    B3::PinnedConstant pinned { };
    bool isCommuted { false };
    bool loadIsLeft { false };
    uint8_t loadWidth { 0 };
    uint8_t shift { 0 };
};

struct PendingJump {
    MacroAssembler::Jump jump;
    uint32_t target;
    uint32_t offset;
};

struct EdgeStub {
    MacroAssembler::Jump jump;
    uint32_t target;
    uint32_t snapshot;
};

struct Island {
    MacroAssembler::Label label;
    uint32_t target;
    uint32_t offset;
};

struct Transfer {
    Reg destination;
    const B3::Value* value;
    Reg source;
    bool isDone { false };
};

constexpr unsigned maxPhisInRegisters = 5;
constexpr uint8_t bytesOfHomeLoad = 4;
constexpr unsigned numberOfSpareSlots = 4;
AOT_THRESHOLD(unsigned, maxBlocks, 60000);

bool fitsInt32(int64_t value) { return value == static_cast<int32_t>(value); }

Reg shiftAmountRegister()
{
#if CPU(X86_64)
    return Reg(X86Registers::ecx);
#else
    return Reg();
#endif
}

bool isPureOperation(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
    case B3::ZExt32:
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::Trunc:
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::EqualOrUnordered:
        return true;
    default:
        return false;
    }
}

bool isComparison(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::EqualOrUnordered:
        return true;
    default:
        return false;
    }
}

MacroAssembler::RelationalCondition relationalConditionOf(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
        return MacroAssembler::Equal;
    case B3::NotEqual:
        return MacroAssembler::NotEqual;
    case B3::LessThan:
        return MacroAssembler::LessThan;
    case B3::GreaterThan:
        return MacroAssembler::GreaterThan;
    case B3::LessEqual:
        return MacroAssembler::LessThanOrEqual;
    case B3::GreaterEqual:
        return MacroAssembler::GreaterThanOrEqual;
    case B3::Above:
        return MacroAssembler::Above;
    case B3::Below:
        return MacroAssembler::Below;
    case B3::AboveEqual:
        return MacroAssembler::AboveOrEqual;
    case B3::BelowEqual:
        return MacroAssembler::BelowOrEqual;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

MacroAssembler::DoubleCondition doubleConditionOf(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
        return MacroAssembler::DoubleEqualAndOrdered;
    case B3::EqualOrUnordered:
        return MacroAssembler::DoubleEqualOrUnordered;
    case B3::NotEqual:
        return MacroAssembler::DoubleNotEqualOrUnordered;
    case B3::LessThan:
        return MacroAssembler::DoubleLessThanAndOrdered;
    case B3::GreaterThan:
        return MacroAssembler::DoubleGreaterThanAndOrdered;
    case B3::LessEqual:
        return MacroAssembler::DoubleLessThanOrEqualAndOrdered;
    case B3::GreaterEqual:
        return MacroAssembler::DoubleGreaterThanOrEqualAndOrdered;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool comparesWithoutSign(MacroAssembler::RelationalCondition condition)
{
    switch (condition) {
    case MacroAssembler::Equal:
    case MacroAssembler::NotEqual:
    case MacroAssembler::Above:
    case MacroAssembler::AboveOrEqual:
    case MacroAssembler::Below:
    case MacroAssembler::BelowOrEqual:
        return true;
    default:
        return false;
    }
}

bool isCommutative(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Add:
    case B3::Mul:
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        return true;
    default:
        return false;
    }
}

std::optional<int64_t> foldBinary(B3::Opcode opcode, bool isWide, int64_t a, int64_t b)
{
    uint64_t ua = static_cast<uint64_t>(a);
    uint64_t ub = static_cast<uint64_t>(b);
    uint64_t result;
    switch (opcode) {
    case B3::Add:
        result = ua + ub;
        break;
    case B3::Sub:
        result = ua - ub;
        break;
    case B3::Mul:
        result = ua * ub;
        break;
    case B3::BitAnd:
        result = ua & ub;
        break;
    case B3::BitOr:
        result = ua | ub;
        break;
    case B3::BitXor:
        result = ua ^ ub;
        break;
    case B3::Shl:
        result = isWide ? ua << (ub & 63) : static_cast<uint64_t>(static_cast<uint32_t>(ua) << (ub & 31));
        break;
    case B3::SShr:
        result = isWide ? static_cast<uint64_t>(a >> (ub & 63)) : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a) >> (ub & 31)));
        break;
    case B3::ZShr:
        result = isWide ? ua >> (ub & 63) : static_cast<uint64_t>(static_cast<uint32_t>(ua) >> (ub & 31));
        break;
    default:
        return std::nullopt;
    }
    if (!isWide)
        return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(result)));
    return static_cast<int64_t>(result);
}

} // anonymous namespace

class DirectCodeGenerator {
    WTF_MAKE_NONCOPYABLE(DirectCodeGenerator);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DirectCodeGenerator);
public:
    DirectCodeGenerator(B3::Procedure& procedure, DirectCodeAttempt attempt)
        : m_procedure(procedure)
        , m_code(procedure.code())
        , m_reservesSpareSlots(attempt == DirectCodeAttempt::WithSpareSlots)
    {
    }

    DirectCodeAttempt attemptAfterFailure() const { return m_lackedSlot && !m_reservesSpareSlots ? DirectCodeAttempt::WithSpareSlots : DirectCodeAttempt::None; }

    bool plan();
    bool generate(CCallHelpers&, CodeRecords&, const BlockColdnessRule&);

    unsigned numberOfValues() const { return m_states.size(); }

private:
    using Jump = MacroAssembler::Jump;
    using Label = MacroAssembler::Label;
    using TrustedImm32 = MacroAssembler::TrustedImm32;
    using TrustedImm64 = MacroAssembler::TrustedImm64;
    using Address = MacroAssembler::Address;

    bool refuse()
    {
        m_isRefused = true;
        return false;
    }

    void fail() { m_hasFailed = true; }
    void failForLackOfSlot()
    {
        m_lackedSlot |= !m_hasFailed;
        m_hasFailed = true;
    }

    ValueState& stateOf(const B3::Value* value) { return m_states[value->index()]; }
    const ValueState& stateOf(const B3::Value* value) const { return m_states[value->index()]; }

    const B3::Value* resolve(const B3::Value* value) const
    {
        while (stateOf(value).form == ValueForm::Alias)
            value = stateOf(value).target;
        return value;
    }

    bool constantOf(const B3::Value* value, int64_t& result) const
    {
        const ValueState& state = stateOf(resolve(value));
        if (state.form != ValueForm::Constant)
            return false;
        result = state.constant;
        return true;
    }

    bool isConstant(const B3::Value* value) const { return stateOf(resolve(value)).form == ValueForm::Constant; }

    bool isZero(const B3::Value* value) const
    {
        int64_t constant;
        return constantOf(value, constant) && !constant;
    }

    B3::BasicBlock* finalTarget(B3::BasicBlock* block, bool* passesRareEdge = nullptr) const
    {
        for (unsigned hops = 0; hops < 8 && block->last()->opcode() == B3::Jump; ++hops) {
            if (block->size() == 1) {
                block = block->successorBlock(0);
                continue;
            }
            if (!m_hasMergesOnlyBranchedOn || block->size() != 2 || block->at(0)->opcode() != B3::Upsilon)
                break;
            B3::BasicBlock* merge = block->successorBlock(0);
            const B3::Value* source = block->at(0)->child(0);
            if (!m_mergesOnlyBranchedOn.get(merge->index()) || block->at(0)->as<B3::UpsilonValue>()->phi() != merge->at(0) || !source->hasInt())
                break;
            const B3::FrequentedBlock& edge = merge->successor(source->asInt() ? 0 : 1);
            if (passesRareEdge)
                *passesRareEdge |= edge.isRare();
            block = edge.block();
        }
        return block;
    }
    void findMergesOnlyBranchedOn();
    bool everyLoopIsEnteredAtItsTop();
    void chooseWhereToSetUpFrame();
    void emitFrameSetup();
    bool tryReusePureCall(const B3::PatchpointValue*, ValueState&);

    uint32_t successorOrderOf(const DirectBlock& block, unsigned index) const { return m_successorOrders[block.firstSuccessor + index]; }
    DirectBlock& successorOf(const DirectBlock& block, unsigned index) { return m_blocks[successorOrderOf(block, index)]; }
    DirectBlock& targetOf(const DirectBlock& block, unsigned index)
    {
        DirectBlock* target = &successorOf(block, index);
        while (target->forwardsTo != UINT32_MAX)
            target = &m_blocks[target->forwardsTo];
        return *target;
    }

    void setUpRegisters();
    bool orderBlocks();
    void findLoops();
    bool numberValues();
    bool classify(B3::Value*, ValueState&, uint32_t order, uint32_t& stretch, uint32_t& shortStretch, uint32_t& memoryEpoch);
    void classifyIntegerBinary(B3::Value*, ValueState&);
    void makeConstant(ValueState& state, int64_t constant)
    {
        state.form = ValueForm::Constant;
        state.constant = constant;
    }
    void makeAlias(ValueState& state, const B3::Value* target, bool targetIsChild = true)
    {
        state.form = ValueForm::Alias;
        state.target = target;
        uint16_t& rawUses = stateOf(target).rawUses;
        if (targetIsChild && rawUses && rawUses != UINT16_MAX)
            --rawUses;
    }

    void releaseRawUse(const B3::Value*);
    bool isSameValue(const B3::Value*, const B3::Value*, unsigned depth) const;
    std::pair<const B3::Value*, bool> truthSourceOf(const B3::Value* condition) const;
    std::optional<bool> knownTruthOf(uint32_t order, const B3::Value* condition);
    bool isLogicalCondition(const B3::Value*) const;
    void fetchConditionOperands(const B3::Value* comparison);
    void noteUses();
    void noteUsesOf(B3::Value*, ValueState&);
    void noteUse(const B3::Value*, Reg hint = Reg());
    void noteLateUse(const B3::Value*, uint32_t stretchAfter);
    void noteAddress(const AddressPlan&);
    void noteCondition(const B3::Value*);
    void noteConditionOperands(const B3::Value*);
    void noteOperands(const OperandPlan&, Reg hintForLeft = Reg());
    void noteStackmapChildren(B3::StackmapValue*, unsigned first, const ValueState&);
    void layOutFrame();
    uint32_t ensureHome(const B3::Value*);
    void computeLiveness();
    bool isLiveIn(uint32_t home, uint32_t block) const { return m_liveIn[static_cast<size_t>(block) * m_liveWords + home / 64] >> (home % 64) & 1; }
    bool isLiveOut(uint32_t home, const DirectBlock&);
    uint32_t lastUseIn(uint32_t home, uint32_t block);
    bool isLiveBehind(uint32_t home, uint32_t position, uint32_t block) { return lastUseIn(home, block) > position || isLiveOut(home, m_blocks[block]); }
    bool areFedDifferentValuesInOneBlock(uint32_t first, uint32_t second);
    bool writeClobbers(uint32_t written, uint32_t other);
    uint32_t findWeb(uint32_t home);
    bool tryCoalesce(uint32_t, uint32_t);
    void coalescePhis();
    void findBlocksWithoutCode();
    void orderEmission();
    void estimateFrequencies();
    bool isAncestorOrSelf(uint32_t ancestor, uint32_t block) const;
    bool tryReuseLoad(B3::Value*, ValueState&, uint32_t memoryEpoch);

    User userOf(const ValueState& state) const
    {
        return { state.position, state.flags & IsFullClobber ? state.stretchBefore : state.stretch, state.shortStretchBefore ? state.shortStretchBefore : state.shortStretch, state.memoryEpoch, state.block };
    }

    AddressPlan planAddress(const B3::Value* pointer, int64_t offset) const;
    bool isFoldableCondition(const B3::Value*) const;
    bool conditionIsFolded(const B3::Value* condition) const
    {
        return m_isPlanning ? isFoldableCondition(condition) : stateOf(condition).form == ValueForm::Folded;
    }
    bool canFoldLoad(const B3::Value*) const;
    bool loadIsFolded(const B3::Value* load) const
    {
        return m_isPlanning ? canFoldLoad(load) : stateOf(load).form == ValueForm::Folded;
    }
    std::optional<B3::PinnedConstant> pinnedConstantFor(int64_t constant) const
    {
        auto finder = m_procedure.policy().addressing.findPinnedConstant;
        if (!finder)
            return std::nullopt;
        return finder(static_cast<uint64_t>(constant));
    }
    OperandPlan planComparison(B3::Opcode, const B3::Value* left, const B3::Value* right, bool mayFoldLoads) const;
    OperandPlan planTest(const B3::Value* left, const B3::Value* right, bool mayFoldLoads) const;
    OperandPlan planBinary(const B3::Value*) const;
    std::pair<const B3::Value*, uint8_t> scaledOperandOf(const B3::Value*) const;
    bool replacesImmediate(int64_t constant, bool hasOneByteForm, OperandPlan&) const;
    std::optional<MacroAssembler::RelationalCondition> setFlagsFor(const B3::Value* condition, bool whenTrue);
    bool storesImmediate(const B3::Value* stored, B3::Opcode) const;

    bool isLiveAfter(uint32_t id) { return m_states[id].lastUse > m_position && isUsedFrom(id, m_position + 1); }
    bool isNeededNow(uint32_t id) { return m_states[id].lastUse >= m_position && isUsedFrom(id, m_position); }
    bool isUsedFrom(uint32_t id, uint32_t position)
    {
        const ValueState& state = m_states[id];
        if (!m_hasLiveness || !state.homeIndex || m_currentBlock == UINT32_MAX || state.form != ValueForm::Plain)
            return true;
        return lastUseIn(state.homeIndex - 1, m_currentBlock) >= position || isLiveOut(state.homeIndex - 1, m_blocks[m_currentBlock]);
    }
    uint32_t holderOf(Reg reg) const { return m_file.holder[reg.index()]; }

    size_t bytesSoFar() const { return m_jit->m_assembler.buffer().codeSize(); }

    void finishUnit(CodeUnit::Kind kind = CodeUnit::Kind::Plain)
    {
        Label end = m_jit->labelIgnoringWatchpoints();
        m_records->units.didGenerateInBlock(m_unitBegin, end, kind);
        m_unitBegin = end;
    }

    void release(Reg reg)
    {
        uint64_t bit = bitOfRegister(reg);
        if (m_file.dirty & bit) {
            uint32_t id = m_file.holder[reg.index()] - 1;
            m_states[id].flags |= WasLetGoUnsaved;
            m_valuesLetGoUnsaved.append(id);
        }
        m_file.holder[reg.index()] = 0;
        m_file.occupied &= ~bit;
        m_file.dirty &= ~bit;
    }

    void bind(Reg reg, uint32_t id, bool isDirty)
    {
        uint64_t bit = bitOfRegister(reg);
        m_file.holder[reg.index()] = id + 1;
        m_file.occupied |= bit;
        if (isDirty)
            m_file.dirty |= bit;
        else
            m_file.dirty &= ~bit;
        m_states[id].flags &= ~WasLetGoUnsaved;
        m_states[id].reg = encode(reg);
    }

    Reg locate(const B3::Value* value) const
    {
        const ValueState& state = stateOf(value);
        if (state.form == ValueForm::FixedRegister)
            return decode(state.base);
        Reg reg = decode(state.reg);
        if (reg && m_file.holder[reg.index()] == value->index() + 1)
            return reg;
        if (state.home > 0 && state.form == ValueForm::Plain)
            return decode(static_cast<uint8_t>(state.home));
        return Reg();
    }

    void lock(Reg reg) { m_locked |= bitOfRegister(reg); }

    Address addressOfHome(int32_t offset) const
    {
        if (!isAddressedOffStackPointer(offset))
            return Address(MacroAssembler::framePointerRegister, offset);
        return Address(MacroAssembler::stackPointerRegister, offset + m_homeBias + static_cast<int32_t>(m_code.frameSize()));
    }
    bool isAddressedOffStackPointer(int32_t offset) const
    {
        if (!m_hasFrameRecord)
            return true;
        return isX86_64() && offset < -128 && offset + static_cast<int32_t>(m_code.frameSize()) <= 127;
    }
    CodeUnit::Kind kindOfFrameAccess(int32_t offset) const { return isAddressedOffStackPointer(offset) ? CodeUnit::Kind::PlainOffStackPointer : CodeUnit::Kind::Plain; }
    int32_t takeLateHome();
    // Webs of other forms share a slot where their lives do not meet, and a source may die where its phi is born. A value of 32 bits leaves the high half of its slot as it was.
    static bool liesInHomeOf(const ValueState& source, const ValueState& phi)
    {
        if (source.form != ValueForm::Plain || !(source.flags & NeedsHome) || source.home != phi.home || ((source.flags ^ phi.flags) & IsDouble))
            return false;
        return (source.flags & (IsWide | IsReadWide)) || !(phi.flags & (IsWide | IsReadWide));
    }
    void storeToFrame(Reg, const ValueState&, int32_t offset);
    void loadFromFrame(Reg, const ValueState&, int32_t offset);
    void writeBack(Reg);
    void moveRegister(Reg source, Reg destination);
    Reg allocate(uint64_t candidates, Reg preferred = Reg());
    Reg findFree(uint64_t candidates);
    void materialize(const B3::Value*, Reg destination);
    Reg registerFor(const B3::Value*, uint64_t avoid = 0);
    GPRReg gprFor(const B3::Value* value, uint64_t avoid = 0) { return registerFor(value, avoid).gpr(); }
    FPRReg fprFor(const B3::Value* value, uint64_t avoid = 0) { return registerFor(value, avoid).fpr(); }
    Reg define(const B3::Value*, Reg reusable = Reg(), Reg otherReusable = Reg(), uint64_t avoid = 0);
    GPRReg defineGPR(const B3::Value* value, GPRReg reusable = InvalidGPRReg, GPRReg otherReusable = InvalidGPRReg)
    {
        return define(value, reusable == InvalidGPRReg ? Reg() : Reg(reusable), otherReusable == InvalidGPRReg ? Reg() : Reg(otherReusable)).gpr();
    }
    FPRReg defineFPR(const B3::Value* value, FPRReg reusable = InvalidFPRReg, FPRReg otherReusable = InvalidFPRReg)
    {
        return define(value, reusable == InvalidFPRReg ? Reg() : Reg(reusable), otherReusable == InvalidFPRReg ? Reg() : Reg(otherReusable)).fpr();
    }
    void finishDefinition(const B3::Value*, Reg);
    Reg hintFor(const ValueState&) const;
    void preserveValuesIn(uint64_t registers, uint64_t avoid, bool alsoValuesNeededNow);
    void flushDirtyValues();
    void moveIntoRegisters(Vector<Transfer, 8>&);
    MemoryOperand operandFor(const AddressPlan&);
    MemoryOperand operandForLoad(const B3::Value* load)
    {
        const B3::MemoryValue* memory = load->as<B3::MemoryValue>();
        return operandFor(planAddress(memory->lastChild(), memory->offset()));
    }

    template<typename Functor>
    auto withAddress(const MemoryOperand& operand, const Functor& functor)
    {
        if (operand.hasIndex())
            return functor(operand.baseIndex());
        return functor(operand.address());
    }

    void checkASANShadow(const MemoryOperand& operand, unsigned size, bool isStore)
    {
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
        finishUnit();
        withAddress(operand, [&](auto address) {
            emitASANShadowCheck(*m_jit, address, size, isStore);
        });
        finishUnit(CodeUnit::Kind::Pinned);
#else
        UNUSED_PARAM(operand);
        UNUSED_PARAM(size);
        UNUSED_PARAM(isStore);
#endif
    }

    void checkASANShadow(const MemoryOperand& operand, const B3::Value* access)
    {
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
        const B3::MemoryValue* memory = access->as<B3::MemoryValue>();
        checkASANShadow(operand, memory->accessByteSize(), memory->isStore());
#else
        UNUSED_PARAM(operand);
        UNUSED_PARAM(access);
#endif
    }

    void emitBlock(DirectBlock&);
    void enterBlock(DirectBlock&);
    void emitValue(B3::Value*, ValueState&, DirectBlock&);
    void emitIntegerBinary(B3::Value*);
    void emitShift(B3::Value*);
    void emitLoad(B3::Value*);
    void emitStore(B3::Value*);
    void emitComparison(B3::Value*);
    void emitSelect(B3::Value*);
    void emitDoubleBinary(B3::Value*);
    void emitDoubleExtremum(B3::Value*);
    void emitDivision(B3::Value*);
    void emitCheck(B3::CheckValue*);
    void emitCheckedArithmetic(B3::CheckValue*);
    void emitPatchpoint(B3::PatchpointValue*, ValueState&, DirectBlock&);
    void emitTerminator(B3::Value*, ValueState&, DirectBlock&);
    void emitSwitch(B3::SwitchValue*, DirectBlock&);
    void emitReturn(B3::Value*);
    void passPhiArguments(bool terminatorReadsValues);
    void leaveFor(DirectBlock& target);
    void didBranchTo(DirectBlock& target, const DirectBlock& from, bool registersChangeAfterward = false);
    void noteEdgeTo(DirectBlock& target);
    void noteEntryOf(DirectBlock&);
    void checkEdge(const RegisterFile& atJump, uint32_t from, DirectBlock& target);
    void jumpTo(DirectBlock& target, const DirectBlock& from);
    void link(Jump, DirectBlock& target);
    void linkConditional(Jump, DirectBlock& target);
    void linkEdge(Jump, DirectBlock& target);
    bool isLiveInto(uint32_t id, const DirectBlock& target) const;
    void arriveAt(DirectBlock& target);
    bool planEntry(DirectBlock& target, Vector<Transfer, 8>& transfers, uint64_t& registersToWriteBack);
    void conformTo(DirectBlock& target);
    void restoreRegisterFile(const RegisterFile&);
    void emitEdgeStubs();
    void placeIslands(const DirectBlock& next);
    void linkPendingJumps();
    bool isNext(const DirectBlock& target, const DirectBlock& from) const
    {
        if (m_isInGap)
            return false;
        for (uint32_t index = from.emissionIndex + 1; index < m_emissionOrder.size(); ++index) {
            const DirectBlock& next = m_blocks[m_emissionOrder[index]];
            if (next.label.isSet())
                continue;
            return &next == &target;
        }
        return false;
    }
    bool mayBeWrittenInGap(DirectBlock&);
    Jump branchOn(const B3::Value* condition, bool whenTrue);
    Jump branchOnComparison(const B3::Value* comparison, bool whenTrue);
    Jump branchOnTest(const B3::Value* left, const B3::Value* right, bool whenNonZero);
    B3::ValueRep repForStackmapChild(const B3::Value*, const B3::ValueRep& constraint, uint64_t early, uint64_t late, uint64_t& temporaries);
    void noteNewLatePaths();
    void recordOrigin(const B3::Value*);
    Box<Label> sharedLabelOf(DirectBlock& block)
    {
        if (!block.sharedLabel) {
            block.sharedLabel = Box<Label>::create();
            if (block.label.isSet())
                *block.sharedLabel = block.label;
        }
        return block.sharedLabel;
    }

    B3::Procedure& m_procedure;
    B3::Air::Code& m_code;
    CCallHelpers* m_jit { nullptr };
    CodeRecords* m_records { nullptr };
    const BlockColdnessRule* m_coldness { nullptr };
    B3::Air::GenerationContext m_context;

    bool m_isRefused { false };
    bool m_hasFailed { false };
    bool m_lackedSlot { false };
    bool m_codeIsReachable { true };
    uint32_t m_currentBlock { UINT32_MAX };
    std::array<uint32_t, numberOfRegisters> m_valuesLeftBehind { };
    uint64_t m_registersWithValuesLeftBehind { 0 };
    bool m_isInRareCode { false };
    bool m_isInGap { false };
    bool m_hasMergesOnlyBranchedOn { false };
    BitVector m_mergesOnlyBranchedOn;
    Vector<uint32_t, 64> m_successorOrders;
    uint32_t m_usualValuesLeft { 0 };
    Vector<PendingJump, 16> m_pendingJumps;
    Vector<Island, 8> m_islands;
    Vector<EdgeStub, 4> m_edgeStubs;
    bool m_reservesSpareSlots { false };
    bool m_needsFrameRecord { false };
    uint8_t m_bytesOfLoadAtUse { bytesOfHomeLoad };
    unsigned m_numberOfTeardowns { 0 };
    uint64_t m_homeRegisters { 0 };
    bool m_hasFrameRecord { true };
    int32_t m_homeBias { 0 };
    bool m_isPlanning { true };
    bool m_needsFrame { false };
    bool m_hasFrame { false };
    uint32_t m_blockThatSetsUpFrame { 0 };
    bool m_hasStackmaps { false };

    Vector<ValueState> m_states;
    Vector<DirectBlock> m_blocks;
    Vector<uint32_t> m_orderOf;
    Vector<uint32_t> m_emissionOrder;
    Vector<Loop, 4> m_loops;
    Vector<std::pair<uint32_t, uint32_t>, 4> m_backEdges;
    Vector<HomeState> m_homes;
    Vector<ExposedUse> m_exposedUses;
    Vector<OrderedUse> m_orderedUses;
    Vector<UpsilonRecord> m_upsilons;
    Vector<uint64_t> m_liveIn;
    size_t m_liveWords { 0 };
    bool m_hasLiveness { false };
    std::array<KnownLoad, 64> m_knownLoads { };
    std::array<const B3::PatchpointValue*, 64> m_knownPureCalls { };
    Vector<std::pair<Reg, unsigned>, 8> m_pushedArguments;
    Vector<const B3::Value*> m_valuesWithReservedHomes;
    Vector<int32_t, 8> m_variableOffsets;
    Vector<int32_t, 4> m_spareSlots;
    Vector<RegisterFile> m_snapshots;
    Vector<const B3::UpsilonValue*, 8> m_pendingUpsilons;
    Vector<uint32_t, 16> m_valuesLetGoUnsaved;
    std::array<const B3::Value*, numberOfRegisters> m_argumentValues { };

    uint64_t m_generalRegisters { 0 };
    uint64_t m_doubleRegisters { 0 };
    uint64_t m_calleeSaves { 0 };
    uint64_t m_clobberedRegisters { 0 };
    uint32_t m_lastStretch { 0 };
    uint32_t m_lastMemoryEpoch { 0 };
    unsigned m_bytesOfStackArguments { 0 };
    unsigned m_numberOfShadows { 0 };

    User m_user;
    RegisterFile m_file { };
    uint64_t m_locked { 0 };
    uint32_t m_position { 0 };
    uint32_t m_stateIsFor { UINT32_MAX };
    struct UncheckedEdge {
        uint32_t target;
        uint32_t from;
        RegisterFile atJump;
    };
    bool m_checksEdges { false };
    Vector<UncheckedEdge> m_uncheckedEdges;
    Vector<uint32_t> m_snapshotAtEntry;
    unsigned m_numberOfLatePathsNoted { 0 };
    Label m_unitBegin;
};

void DirectCodeGenerator::setUpRegisters()
{
    uint64_t excluded = bitsOf(RegisterSet::macroClobberedGPRs()) | bitsOf(RegisterSet::stackRegisters());
#if CPU(X86_64)
    excluded |= bitOfRegister(Reg(MacroAssembler::fpTempRegister));
#elif CPU(ARM64)
    excluded |= bitOfRegister(Reg(MacroAssembler::fpTempRegister));
#endif
    RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
    calleeSaves.exclude(m_code.unsavedCalleeSaves());
    calleeSaves.merge(m_code.policy().registers.savedByCallee);
    m_calleeSaves = bitsOf(calleeSaves) & bitsOf(m_code.mutableRegs());
    for (Reg reg : m_code.regsInPriorityOrder(B3::GP)) {
        if (!((excluded | m_calleeSaves) & bitOfRegister(reg)))
            m_generalRegisters |= bitOfRegister(reg);
    }
    for (Reg reg : m_code.regsInPriorityOrder(B3::FP)) {
        if (!((excluded | m_calleeSaves) & bitOfRegister(reg)))
            m_doubleRegisters |= bitOfRegister(reg);
    }
}

void DirectCodeGenerator::findMergesOnlyBranchedOn()
{
    BitVector phis;
    Vector<std::pair<const B3::Value*, unsigned>, 8> merges;
    for (B3::BasicBlock* block : m_procedure) {
        if (block->size() != 2 || block->at(0)->opcode() != B3::Phi || block->last()->opcode() != B3::Branch || block->last()->child(0) != block->at(0))
            continue;
        m_hasMergesOnlyBranchedOn = true;
        m_mergesOnlyBranchedOn.set(block->index());
        phis.set(block->at(0)->index());
        merges.append({ block->at(0), block->index() });
    }
    if (!m_hasMergesOnlyBranchedOn)
        return;
    for (B3::BasicBlock* block : m_procedure) {
        for (B3::Value* value : *block) {
            for (B3::Value* child : value->children()) {
                if (child->opcode() != B3::Phi || !phis.get(child->index()) || (value->opcode() == B3::Branch && block->at(0) == child && block->size() == 2))
                    continue;
                for (auto [phi, merge] : merges) {
                    if (phi == child)
                        m_mergesOnlyBranchedOn.clear(merge);
                }
            }
        }
    }
}

bool DirectCodeGenerator::orderBlocks()
{
    unsigned numberOfBlocks = m_procedure.size();
    if (numberOfBlocks > maxBlocks)
        return refuse();
    constexpr uint32_t unseen = UINT32_MAX;
    constexpr uint32_t seen = UINT32_MAX - 1;
    m_orderOf.fill(unseen, numberOfBlocks);

    struct Frame {
        B3::BasicBlock* block;
        unsigned next;
    };
    Vector<Frame, 64> stack;
    Vector<B3::BasicBlock*, 64> postOrder;
    auto rankOf = [](const B3::FrequentedBlock& edge) -> unsigned {
        switch (edge.frequency()) {
        case B3::FrequencyClass::Normal:
            return 0;
        case B3::FrequencyClass::Unlikely:
            return 1;
        case B3::FrequencyClass::Rare:
            return 2;
        }
        return 0;
    };
    auto successorInVisitOrder = [&](B3::BasicBlock* block, unsigned position) -> B3::BasicBlock* {
        unsigned count = block->numSuccessors();
        if (count == 1)
            return block->successorBlock(0);
        if (count == 2) {
            bool firstIsVisitedFirst = rankOf(block->successor(0)) > rankOf(block->successor(1));
            return block->successorBlock(firstIsVisitedFirst ? position : 1 - position);
        }
        if (count == 3) {
            std::array<unsigned, 3> order { 2, 1, 0 };
            AOT::stableSort(order, std::ranges::greater { }, [&](unsigned index) {
                return rankOf(block->successor(index));
            });
            return block->successorBlock(order[position]);
        }
        return block->successorBlock(count - 1 - position);
    };

    findMergesOnlyBranchedOn();
    B3::BasicBlock* root = m_procedure.at(0);
    m_orderOf[root->index()] = seen;
    stack.append({ root, 0 });
    while (!stack.isEmpty()) {
        B3::BasicBlock* block = stack.last().block;
        unsigned next = stack.last().next;
        if (next == block->numSuccessors()) {
            postOrder.append(block);
            stack.removeLast();
            continue;
        }
        ++stack.last().next;
        B3::BasicBlock* successor = finalTarget(successorInVisitOrder(block, next));
        if (m_orderOf[successor->index()] != unseen)
            continue;
        m_orderOf[successor->index()] = seen;
        stack.append({ successor, 0 });
    }

    unsigned count = postOrder.size();
    m_blocks.grow(count);
    for (unsigned order = 0; order < count; ++order) {
        B3::BasicBlock* block = postOrder[count - 1 - order];
        m_blocks[order].block = block;
        m_orderOf[block->index()] = order;
    }

    m_blocks[0].isReachedNormally = true;
    for (unsigned order = 0; order < count; ++order) {
        DirectBlock& state = m_blocks[order];
        state.isRare = !state.isReachedNormally;
        B3::BasicBlock* block = state.block;
        B3::PatchpointValue* terminalPatchpoint = block->last()->as<B3::PatchpointValue>();
        state.firstSuccessor = m_successorOrders.size();
        for (unsigned index = 0; index < block->numSuccessors(); ++index) {
            const B3::FrequentedBlock& edge = block->successor(index);
            bool isRareEdge = edge.isRare();
            uint32_t targetOrder = m_orderOf[finalTarget(edge.block(), &isRareEdge)->index()];
            m_successorOrders.append(targetOrder);
            DirectBlock& target = m_blocks[targetOrder];
            ++target.numberOfPredecessors;
            target.predecessor = order;
            if (!state.isRare && !isRareEdge)
                target.isReachedNormally = true;
            if (targetOrder <= order) {
                m_backEdges.append({ targetOrder, order });
                target.hasBackEdgePredecessor = true;
            }
            if (terminalPatchpoint && index < terminalPatchpoint->numberOfSuccessorClobbers() && !terminalPatchpoint->lateClobberedOnSuccessor(index).isEmpty())
                target.startsEmpty = true;
        }
    }
    if (m_blocks[0].numberOfPredecessors)
        return refuse();
    for (unsigned order = 1; order < count; ++order) {
        if (m_blocks[order].numberOfPredecessors != 1)
            m_blocks[order].startsEmpty = true;
    }

    return true;
}

void DirectCodeGenerator::orderEmission()
{
    unsigned count = m_blocks.size();
    m_emissionOrder.reserveInitialCapacity(count);
    for (unsigned order = 0; order < count; ++order) {
        if (!m_blocks[order].isRare && m_blocks[order].forwardsTo == UINT32_MAX && m_blocks[order].isReachable)
            m_emissionOrder.append(order);
    }
    for (unsigned order = 0; order < count; ++order) {
        if (m_blocks[order].isRare && m_blocks[order].forwardsTo == UINT32_MAX && m_blocks[order].isReachable)
            m_emissionOrder.append(order);
    }
    for (unsigned index = 0; index < m_emissionOrder.size(); ++index) {
        DirectBlock& block = m_blocks[m_emissionOrder[index]];
        block.emissionIndex = index;
        if (!block.isRare)
            m_usualValuesLeft += block.block->size();
        unsigned numberOfSuccessors = block.block->numSuccessors();
        if (block.onlySuccessor)
            ++targetOf(block, block.onlySuccessor - 1).incomingEdges;
        else if (block.block->last()->opcode() == B3::Branch && &targetOf(block, 0) == &targetOf(block, 1))
            ++targetOf(block, 0).incomingEdges;
        else {
            for (unsigned successor = 0; successor < numberOfSuccessors; ++successor)
                ++targetOf(block, successor).incomingEdges;
        }
    }
}

void DirectCodeGenerator::chooseWhereToSetUpFrame()
{
    AOT_THRESHOLD(unsigned, maxBlocksAheadOfFrame, 8);
    if (!m_hasFrame || m_reservesSpareSlots || !m_code.policy().frame.shrinkWraps || m_code.policy().frame.setsUpFrameAtEntry)
        return;
    for (const HomeState& home : m_homes) {
        const ValueState& state = m_states[home.value];
        if (!home.firstUpsilon || state.form != ValueForm::Plain || !(state.flags & (NeedsHome | ArrivesInSlot)))
            continue;
        m_blocks[state.block].usesFrame = true;
        for (uint32_t record = home.firstUpsilon; record; record = m_upsilons[record - 1].nextForPhi)
            m_blocks[m_upsilons[record - 1].block].usesFrame = true;
    }
    {
        DirectBlock* block = &m_blocks[0];
        for (unsigned steps = 0; steps < maxBlocksAheadOfFrame; ++steps) {
            if (block->usesFrame)
                return;
            bool hasOneTarget = block->onlySuccessor || block->block->numSuccessors() == 1;
            if (!hasOneTarget)
                break;
            block = &targetOf(*block, block->onlySuccessor ? block->onlySuccessor - 1 : 0);
            if (block->startsEmpty)
                return;
        }
    }
    uint32_t count = m_blocks.size();
    auto forEachTarget = [&](DirectBlock& block, auto&& function) {
        if (block.onlySuccessor) {
            function(targetOf(block, block.onlySuccessor - 1));
            return;
        }
        for (unsigned index = 0; index < block.block->numSuccessors(); ++index)
            function(targetOf(block, index));
    };
    auto orderOf = [&](const DirectBlock& block) {
        return static_cast<uint32_t>(&block - m_blocks.span().data());
    };
    BitVector leadsToFrameUse;
    for (uint32_t order = count; order--;) {
        DirectBlock& block = m_blocks[order];
        if (!block.isReachable)
            continue;
        bool leads = block.usesFrame;
        forEachTarget(block, [&](DirectBlock& target) {
            leads |= orderOf(target) <= order || leadsToFrameUse.get(orderOf(target));
        });
        if (leads)
            leadsToFrameUse.set(order);
    }
    uint32_t chosen = 0;
    for (unsigned steps = 0; steps < maxBlocksAheadOfFrame && !m_blocks[chosen].usesFrame; ++steps) {
        DirectBlock* next = nullptr;
        unsigned numberThatLead = 0;
        forEachTarget(m_blocks[chosen], [&](DirectBlock& target) {
            if (leadsToFrameUse.get(orderOf(target)) && next != &target) {
                ++numberThatLead;
                next = &target;
            }
        });
        if (numberThatLead != 1 || next->startsEmpty || next->incomingEdges != 1 || m_blocks[chosen].block->last()->opcode() == B3::Patchpoint)
            break;
        chosen = orderOf(*next);
    }
    if (!chosen)
        return;
    BitVector isBehindSetup;
    Vector<uint32_t, 32> worklist;
    auto visitFrom = [&](uint32_t first, BitVector& seen, auto&& mayEnter) {
        worklist.append(first);
        seen.set(first);
        while (!worklist.isEmpty()) {
            forEachTarget(m_blocks[worklist.takeLast()], [&](DirectBlock& target) {
                uint32_t order = orderOf(target);
                if (!seen.get(order) && mayEnter(order)) {
                    seen.set(order);
                    worklist.append(order);
                }
            });
        }
    };
    visitFrom(chosen, isBehindSetup, [](uint32_t) { return true; });
    BitVector isAheadOfSetup;
    visitFrom(0, isAheadOfSetup, [&](uint32_t order) { return order != chosen; });
    for (uint32_t order = 0; order < count; ++order) {
        if (isAheadOfSetup.get(order) && isBehindSetup.get(order))
            return;
    }
    m_blockThatSetsUpFrame = chosen;
    for (uint32_t order = 0; order < count; ++order)
        m_blocks[order].runsInFrame = !isAheadOfSetup.get(order);
    B3::Air::FrameLayout layout = m_code.frameLayout();
    layout.coverage = B3::Air::FrameCoverage::SomePaths;
    m_code.setFrameLayout(WTF::move(layout));
}

void DirectCodeGenerator::emitFrameSetup()
{
    CCallHelpers& jit = *m_jit;
    Vector<B3::Air::Arg, 16> operands;
    uint64_t scratches = bitsOf(RegisterSet::macroClobberedGPRs());
    operands.append(B3::Air::Arg::imm(std::popcount(scratches)));
    for (uint64_t bits = scratches; bits; bits &= bits - 1)
        operands.append(B3::Air::Tmp(Reg::fromIndex(std::countr_zero(bits))));
    for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList())
        operands.append(B3::Air::Tmp(entry.reg()));
    for (auto [reg, word] : m_pushedArguments) {
        if (locate(m_argumentValues[reg.index()]) != reg) {
            failForLackOfSlot();
            return;
        }
        operands.append(B3::Air::Tmp(reg));
        operands.append(B3::Air::Arg::imm(word));
        m_file.dirty &= ~bitOfRegister(reg);
    }
    B3::Air::Inst enterFrame(B3::Air::EnterFrame, nullptr, operands.span());
    B3::Air::emitFrameSetup(jit, m_context, enterFrame);
    m_context.setFrameIsSetUp(jit, true);
    noteNewLatePaths();
    finishUnit(CodeUnit::Kind::Pinned);
}

void DirectCodeGenerator::estimateFrequencies()
{
    constexpr double rareEdgeShare = 1.0 / 1024;
    constexpr double unlikelyEdgeShare = 1.0 / 16;
    constexpr double returningBlockWeight = 0.25;
    constexpr double iterationsPerLoop = 10;
    constexpr double minFrequency = 1e-6;
    constexpr double maxFrequency = 1e6;
    unsigned count = m_blocks.size();
    Vector<double, 64> weights;
    Vector<double, 64> incoming;
    weights.fill(1, count);
    incoming.fill(0, count);
    auto orderOfSuccessor = [&](const DirectBlock& block, unsigned index) {
        return successorOrderOf(block, index);
    };
    auto edgeWeight = [&](const DirectBlock& block, unsigned index) {
        double weight = weights[orderOfSuccessor(block, index)];
        switch (block.block->successor(index).frequency()) {
        case B3::FrequencyClass::Normal:
            return weight;
        case B3::FrequencyClass::Unlikely:
            return std::max(rareEdgeShare, weight * unlikelyEdgeShare);
        case B3::FrequencyClass::Rare:
            return rareEdgeShare;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (uint32_t order = count; order--;) {
        const DirectBlock& block = m_blocks[order];
        double weight = block.block->last()->opcode() == B3::Oops ? rareEdgeShare : 1;
        if (unsigned numberOfSuccessors = block.block->numSuccessors()) {
            double likeliest = 0;
            for (unsigned index = 0; index < numberOfSuccessors; ++index)
                likeliest = std::max(likeliest, edgeWeight(block, index));
            weight = std::max(rareEdgeShare, std::min(weight, likeliest));
        }
        weights[order] = weight;
    }
    Vector<bool, 64> leadsNowhere;
    leadsNowhere.fill(false, count);
    for (uint32_t order = 0; order < count; ++order)
        leadsNowhere[order] = weights[order] <= rareEdgeShare;
    if (m_procedure.policy().optimization.returnsAreUnlikely) {
        for (uint32_t order = 0; order < count; ++order) {
            if (m_blocks[order].block->last()->opcode() == B3::Return)
                weights[order] = std::max(rareEdgeShare, std::min(weights[order], returningBlockWeight));
        }
    }
    incoming[0] = 1;
    for (uint32_t order = 0; order < count; ++order) {
        DirectBlock& block = m_blocks[order];
        double flow = incoming[order];
        double frequency = flow;
        for (uint32_t loop = block.loop; loop; loop = m_loops[loop - 1].parent)
            frequency *= iterationsPerLoop;
        block.frequency = std::clamp(frequency, minFrequency, maxFrequency);
        unsigned numberOfSuccessors = block.block->numSuccessors();
        if (block.onlySuccessor) {
            uint32_t successor = orderOfSuccessor(block, block.onlySuccessor - 1);
            if (successor > order)
                incoming[successor] += flow;
            continue;
        }
        double weightOfOthers = 0;
        double rare = 0;
        for (unsigned index = 0; index < numberOfSuccessors; ++index) {
            uint32_t successor = orderOfSuccessor(block, index);
            if (successor <= order)
                continue;
            bool isRare = block.block->successor(index).frequency() == B3::FrequencyClass::Rare || (leadsNowhere[successor] && !leadsNowhere[order]);
            if (isRare)
                rare += rareEdgeShare;
            else
                weightOfOthers += edgeWeight(block, index);
        }
        rare = std::min(rare, 0.5);
        for (unsigned index = 0; index < numberOfSuccessors; ++index) {
            uint32_t successor = orderOfSuccessor(block, index);
            if (successor <= order)
                continue;
            bool isRare = block.block->successor(index).frequency() == B3::FrequencyClass::Rare || (leadsNowhere[successor] && !leadsNowhere[order]);
            incoming[successor] += flow * (isRare ? rareEdgeShare : (1 - rare) * edgeWeight(block, index) / weightOfOthers);
        }
    }
}

void DirectCodeGenerator::findLoops()
{
    if (m_backEdges.isEmpty())
        return;
    AOT::sort(m_backEdges, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    });
    Vector<uint32_t, 8> open;
    for (auto [first, last] : m_backEdges) {
        while (!open.isEmpty() && m_loops[open.last() - 1].lastBlock < first)
            open.removeLast();
        if (!open.isEmpty() && m_loops[open.last() - 1].firstBlock == first)
            continue;
        for (uint32_t enclosing : open) {
            if (m_loops[enclosing - 1].lastBlock < last)
                m_loops[enclosing - 1].lastBlock = last;
        }
        m_loops.append(Loop { first, last, open.isEmpty() ? 0 : open.last(), 0, 0 });
        open.append(m_loops.size());
    }
    for (uint32_t index = 0; index < m_loops.size(); ++index) {
        Loop& loop = m_loops[index];
        loop.firstPosition = m_blocks[loop.firstBlock].firstPosition;
        loop.lastPosition = m_blocks[loop.lastBlock].lastPosition;
        for (uint32_t order = loop.firstBlock; order <= loop.lastBlock; ++order)
            m_blocks[order].loop = index + 1;
    }
}

void DirectCodeGenerator::classifyIntegerBinary(B3::Value* value, ValueState& state)
{
    bool isWide = state.flags & IsWide;
    B3::Opcode opcode = value->opcode();
    const B3::Value* left = resolve(value->child(0));
    const B3::Value* right = resolve(value->child(1));
    const ValueState& leftState = stateOf(left);
    const ValueState& rightState = stateOf(right);
    bool leftIsConstant = leftState.form == ValueForm::Constant;
    bool rightIsConstant = rightState.form == ValueForm::Constant;
    if (leftIsConstant && rightIsConstant) {
        if (std::optional<int64_t> folded = foldBinary(opcode, isWide, leftState.constant, rightState.constant)) {
            makeConstant(state, *folded);
            return;
        }
    }
    if (left == right && !leftIsConstant) {
        auto dropRawUse = [&] {
            uint16_t& rawUses = stateOf(left).rawUses;
            if (rawUses && rawUses != UINT16_MAX)
                --rawUses;
        };
        switch (opcode) {
        case B3::Sub:
        case B3::BitXor:
            dropRawUse();
            dropRawUse();
            makeConstant(state, 0);
            return;
        case B3::BitAnd:
        case B3::BitOr:
            dropRawUse();
            makeAlias(state, left);
            return;
        default:
            break;
        }
    }
    auto isIdentity = [&](int64_t constant) {
        switch (opcode) {
        case B3::Add:
        case B3::Sub:
        case B3::BitOr:
        case B3::BitXor:
        case B3::Shl:
        case B3::SShr:
        case B3::ZShr:
            return !constant;
        case B3::Mul:
            return constant == 1;
        case B3::BitAnd:
            return constant == -1;
        default:
            return false;
        }
    };
    if (rightIsConstant && isIdentity(rightState.constant)) {
        makeAlias(state, left);
        return;
    }
    if (leftIsConstant && isCommutative(opcode) && isIdentity(leftState.constant)) {
        makeAlias(state, right);
        return;
    }
    if (opcode == B3::Add && isWide && (leftIsConstant || rightIsConstant)) {
        const B3::Value* base = rightIsConstant ? left : right;
        const ValueState& baseState = stateOf(base);
        int64_t constant = rightIsConstant ? rightState.constant : leftState.constant;
        bool isSumOffFixedRegister = baseState.form == ValueForm::Rematerialized && base->opcode() == B3::Add;
        if (baseState.form == ValueForm::FixedRegister || isSumOffFixedRegister) {
            int64_t offset = constant + (isSumOffFixedRegister ? baseState.constant : 0);
            if (fitsInt32(offset)) {
                state.form = ValueForm::Rematerialized;
                state.constant = offset;
                state.base = baseState.base;
                return;
            }
        }
    }
    if (!isWide)
        state.flags |= IsZeroExtended;
}

bool DirectCodeGenerator::classify(B3::Value* value, ValueState& state, uint32_t order, uint32_t& stretch, uint32_t& shortStretch, uint32_t& memoryEpoch)
{
    B3::Opcode opcode = value->opcode();
    switch (value->type().kind()) {
    case B3::Void:
    case B3::Int32:
        break;
    case B3::Int64:
        state.flags |= IsWide;
        break;
    case B3::Double:
        state.flags |= IsDouble;
        break;
    case B3::Tuple:
        if (opcode != B3::Patchpoint)
            return refuse();
        break;
    default:
        return refuse();
    }
    bool isDouble = state.flags & IsDouble;

    switch (opcode) {
    case B3::Nop:
        state.form = ValueForm::Dead;
        return true;
    case B3::Identity:
    case B3::Opaque:
        makeAlias(state, resolve(value->child(0)));
        return true;
    case B3::Const32:
        makeConstant(state, value->asInt32());
        return true;
    case B3::Const64:
        makeConstant(state, value->asInt64());
        return true;
    case B3::ConstDouble:
        makeConstant(state, std::bit_cast<int64_t>(value->asDouble()));
        return true;
    case B3::Set:
    case B3::Get:
        return true;
    case B3::SlotBase:
        state.form = ValueForm::Rematerialized;
        return true;
    case B3::Relocatable: {
        auto space = static_cast<RelocationSpace>(value->as<B3::RelocatableValue>()->symbol().space);
        if (space != RelocationSpace::SiteCaches && space != RelocationSpace::Constants && space != RelocationSpace::JumpTables)
            return refuse();
        state.form = ValueForm::Rematerialized;
        return true;
    }
    case B3::FramePointer:
        state.form = ValueForm::FixedRegister;
        state.base = encode(Reg(MacroAssembler::framePointerRegister));
        return true;
    case B3::ArgumentReg: {
        Reg reg = value->as<B3::ArgumentRegValue>()->argumentReg();
        if (m_code.isPinned(reg)) {
            state.form = ValueForm::FixedRegister;
            state.base = encode(reg);
            return true;
        }
        if (order)
            return refuse();
        if (!((m_generalRegisters | m_doubleRegisters) & bitOfRegister(reg)))
            return refuse();
        if (const B3::Value* first = m_argumentValues[reg.index()]) {
            makeAlias(state, first, false);
            return true;
        }
        m_argumentValues[reg.index()] = value;
        return true;
    }
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
        if (isDouble)
            return true;
        classifyIntegerBinary(value, state);
        return true;
    case B3::Div:
        if (isDouble)
            return true;
        [[fallthrough]];
    case B3::Mod:
    case B3::UDiv:
    case B3::UMod:
    case B3::UMulHigh:
    case B3::MulHigh:
        if (isDouble || !isX86_64())
            return refuse();
        if (!(state.flags & IsWide))
            state.flags |= IsZeroExtended;
        return true;
    case B3::FMax:
    case B3::FMin:
    case B3::Ceil:
    case B3::Floor:
    case B3::FTrunc:
        if (!isDouble)
            return refuse();
        return true;
    case B3::PopCount:
        if (!(state.flags & IsWide))
            state.flags |= IsZeroExtended;
        return true;
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        if (isDouble)
            return refuse();
        classifyIntegerBinary(value, state);
        return true;
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
        classifyIntegerBinary(value, state);
        return true;
    case B3::Neg:
        if (isDouble)
            return isX86_64() && !pinnedConstantFor(std::numeric_limits<int64_t>::min()) ? refuse() : true;
        if (!(state.flags & IsWide))
            state.flags |= IsZeroExtended;
        return true;
    case B3::Clz:
        if (!(state.flags & IsWide))
            state.flags |= IsZeroExtended;
        return true;
    case B3::Sqrt:
    case B3::Abs:
        if (!isDouble)
            return refuse();
        return true;
    case B3::BitwiseCast:
        if (value->child(0)->type() != B3::Int64 && value->child(0)->type() != B3::Double)
            return refuse();
        return true;
    case B3::IToD:
        return true;
    case B3::SExt8:
    case B3::SExt16:
        state.flags |= IsZeroExtended;
        return true;
    case B3::SExt32: {
        int64_t constant;
        if (constantOf(value->child(0), constant))
            makeConstant(state, static_cast<int32_t>(constant));
        return true;
    }
    case B3::ZExt32: {
        const B3::Value* child = resolve(value->child(0));
        ValueState& childState = stateOf(child);
        if (childState.form == ValueForm::Constant)
            makeConstant(state, static_cast<int64_t>(static_cast<uint32_t>(childState.constant)));
        else if (childState.form == ValueForm::Plain && (childState.flags & IsZeroExtended)) {
            childState.flags |= IsReadWide;
            makeAlias(state, child);
        }
        return true;
    }
    case B3::Trunc: {
        const B3::Value* child = resolve(value->child(0));
        if (child->type() != B3::Int64 && child->type() != B3::Int32)
            return refuse();
        const ValueState& childState = stateOf(child);
        if (childState.form == ValueForm::Constant)
            makeConstant(state, static_cast<int32_t>(childState.constant));
        else if (childState.form == ValueForm::Plain)
            makeAlias(state, child);
        return true;
    }
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
        state.flags |= IsZeroExtended;
        return true;
    case B3::EqualOrUnordered:
        if (value->child(0)->type() != B3::Double)
            return refuse();
        state.flags |= IsZeroExtended;
        return true;
    case B3::Select:
        {
            std::optional<bool> truth;
            int64_t constant;
            if (constantOf(value->child(0), constant))
                truth = !!constant;
            else
                truth = knownTruthOf(order, value->child(0));
            if (truth) {
                releaseRawUse(value->child(0));
                releaseRawUse(value->child(*truth ? 2 : 1));
                makeAlias(state, resolve(value->child(*truth ? 1 : 2)));
            }
        }
        return true;
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load:
        if (value->as<B3::MemoryValue>()->hasFence())
            return refuse();
        if (value->type() == B3::Int32)
            state.flags |= IsZeroExtended;
        tryReuseLoad(value, state, memoryEpoch);
        return true;
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
        if (value->as<B3::MemoryValue>()->hasFence())
            return refuse();
        if (value->child(0)->type() != B3::Int32 && value->child(0)->type() != B3::Int64 && value->child(0)->type() != B3::Double)
            return refuse();
        memoryEpoch = ++m_lastMemoryEpoch;
        return true;
    case B3::Fence:
        memoryEpoch = ++m_lastMemoryEpoch;
        return true;
    case B3::Patchpoint: {
        B3::PatchpointValue* patchpoint = value->as<B3::PatchpointValue>();
        if (patchpoint->pureFunction && tryReusePureCall(patchpoint, state))
            return true;
        m_hasStackmaps = true;
        memoryEpoch = ++m_lastMemoryEpoch;
        uint64_t clobbered = bitsOf(patchpoint->earlyClobbered()) | bitsOf(patchpoint->lateClobbered());
        m_clobberedRegisters |= clobbered;
        unsigned numberKept = std::popcount(m_generalRegisters & ~clobbered);
        if (numberKept <= 2) {
            state.flags |= IsFullClobber;
            state.stretchBefore = stretch;
            stretch = ++m_lastStretch;
            state.stretch = stretch;
        }
        if (clobbered & (m_generalRegisters | m_doubleRegisters)) {
            state.shortStretchBefore = shortStretch;
            shortStretch = ++m_lastStretch;
            state.shortStretch = shortStretch;
        }
        for (const B3::ValueRep& constraint : patchpoint->resultConstraints) {
            switch (constraint.kind()) {
            case B3::ValueRep::WarmAny:
            case B3::ValueRep::SomeRegister:
            case B3::ValueRep::SomeEarlyRegister:
                break;
            case B3::ValueRep::Register:
                m_clobberedRegisters |= bitOfRegister(constraint.reg());
                break;
            default:
                return refuse();
            }
        }
        return true;
    }
    case B3::Extract: {
        const B3::Value* tuple = value->child(0);
        if (tuple->opcode() != B3::Patchpoint)
            return refuse();
        ValueState& tupleState = stateOf(tuple);
        int32_t index = value->as<B3::ExtractValue>()->index();
        for (const B3::Value* other = tupleState.target; other; other = stateOf(other).target) {
            if (other->as<B3::ExtractValue>()->index() == index) {
                makeAlias(state, other, false);
                return true;
            }
        }
        state.target = tupleState.target;
        tupleState.target = value;
        state.stretch = tupleState.stretch;
        state.shortStretch = tupleState.shortStretch;
        state.position = tupleState.position;
        return true;
    }
    case B3::Check:
        m_hasStackmaps = true;
        return true;
    case B3::CheckAdd:
    case B3::CheckSub:
    case B3::CheckMul:
        m_hasStackmaps = true;
        if (!(state.flags & IsWide))
            state.flags |= IsZeroExtended;
        return true;
    case B3::Upsilon: {
        ValueState& phiState = stateOf(value->as<B3::UpsilonValue>()->phi());
        const B3::Value* source = resolve(value->child(0));
        if (!phiState.shadow)
            phiState.target = source;
        else if (phiState.target != source)
            phiState.flags |= HasSeveralSources;
        ++phiState.shadow;
        if (stateOf(source).form != ValueForm::Constant && !(stateOf(source).flags & IsZeroExtended))
            phiState.flags |= HasSourceWithHighBits;
        return true;
    }
    case B3::Phi: {
        DirectBlock& block = m_blocks[order];
        int32_t numberOfUpsilons = std::exchange(state.shadow, 0);
        if (numberOfUpsilons && !block.hasBackEdgePredecessor && !(state.flags & HasSeveralSources)) {
            const B3::Value* source = state.target;
            makeAlias(state, source, false);
            uint16_t& rawUses = stateOf(source).rawUses;
            if (rawUses != UINT16_MAX)
                rawUses -= std::min<int32_t>(rawUses, numberOfUpsilons);
            return true;
        }
        if (numberOfUpsilons && !block.hasBackEdgePredecessor && value->type() == B3::Int32 && !(state.flags & HasSourceWithHighBits))
            state.flags |= IsZeroExtended;
        state.target = nullptr;
        state.nextPhi = block.firstPhi;
        block.firstPhi = value->index() + 1;
        uint32_t& count = isDouble ? block.numberOfDoublePhis : block.numberOfGeneralPhis;
        if (++count > maxPhisInRegisters)
            state.flags |= ArrivesInSlot;
        return true;
    }
    case B3::Jump:
    case B3::Branch:
    case B3::Switch:
    case B3::Return:
    case B3::Oops:
        return true;
    default:
        return refuse();
    }
}

bool DirectCodeGenerator::numberValues()
{
    unsigned numberOfValues = m_procedure.values().size();
    m_states.grow(numberOfValues);
    std::memset(static_cast<void*>(m_states.mutableSpan().data()), 0, numberOfValues * sizeof(ValueState));

    uint32_t position = 0;
    m_blocks[0].isReachable = true;
    for (uint32_t order = 0; order < m_blocks.size(); ++order) {
        DirectBlock& block = m_blocks[order];
        block.isReachable |= block.hasBackEdgePredecessor;
        if (!block.isReachable) {
            block.firstPosition = position + 1;
            for (B3::Value* value : *block.block) {
                ValueState& state = stateOf(value);
                state.position = ++position;
                state.block = order;
                state.form = ValueForm::Dead;
            }
            block.lastPosition = position;
            continue;
        }
        uint32_t stretch = order && !block.startsEmpty ? m_blocks[block.predecessor].stretchAtEnd : ++m_lastStretch;
        uint32_t shortStretch = order && !block.startsEmpty ? m_blocks[block.predecessor].shortStretchAtEnd : ++m_lastStretch;
        if (order && block.startsEmpty && !block.hasBackEdgePredecessor && block.offeredStretch && block.hasOfferFromCodeAhead && !m_reservesSpareSlots) {
            block.continuesStretch = true;
            stretch = block.offeredStretch;
            if (block.offeredShortStretch)
                shortStretch = block.offeredShortStretch;
        }
        uint32_t memoryEpoch = order && !block.startsEmpty ? m_blocks[block.predecessor].memoryEpochAtEnd : ++m_lastMemoryEpoch;
        block.firstPosition = position + 1;
        unsigned size = block.block->size();
        for (unsigned index = 0; index < size; ++index) {
            B3::Value* value = block.block->at(index);
            ValueState& state = stateOf(value);
            state.position = ++position;
            state.block = order;
            state.stretch = stretch;
            state.shortStretch = shortStretch;
            state.memoryEpoch = memoryEpoch;
            if (index + 1 == size) {
                block.stretchBeforeTerminator = stretch;
                block.shortStretchBeforeTerminator = shortStretch;
            }
            for (B3::Value* child : value->children()) {
                uint16_t& rawUses = stateOf(resolve(child)).rawUses;
                if (rawUses != UINT16_MAX)
                    ++rawUses;
            }
            if (!classify(value, state, order, stretch, shortStretch, memoryEpoch))
                return false;
        }
        block.lastPosition = position;
        block.stretchAtEnd = stretch;
        block.shortStretchAtEnd = shortStretch;
        block.memoryEpochAtEnd = memoryEpoch;

        B3::Value* last = block.block->last();
        auto reach = [&](DirectBlock& successor) {
            successor.isReachable = true;
            uint32_t offeredStretch = last->opcode() == B3::Patchpoint ? 0 : stretch;
            if (!successor.hasOffer) {
                successor.hasOffer = true;
                successor.offeredStretch = offeredStretch;
                successor.offeredShortStretch = shortStretch;
            } else {
                if (successor.offeredStretch != offeredStretch)
                    successor.offeredStretch = 0;
                if (successor.offeredShortStretch != shortStretch)
                    successor.offeredShortStretch = 0;
            }
            successor.hasOfferFromCodeAhead |= successor.isRare || !block.isRare;
        };
        if (last->opcode() == B3::Branch) {
            std::optional<bool> truth;
            int64_t constant;
            if (constantOf(last->child(0), constant))
                truth = !!constant;
            else
                truth = knownTruthOf(order, last->child(0));
            if (truth) {
                block.onlySuccessor = *truth ? 1 : 2;
                releaseRawUse(last->child(0));
                reach(successorOf(block, *truth ? 0 : 1));
                continue;
            }
        }
        for (unsigned index = 0; index < block.block->numSuccessors(); ++index)
            reach(successorOf(block, index));
    }
    return true;
}

void DirectCodeGenerator::releaseRawUse(const B3::Value* value)
{
    value = resolve(value);
    ValueState& state = stateOf(value);
    if (state.form != ValueForm::Plain || !state.rawUses || state.rawUses == UINT16_MAX)
        return;
    if (--state.rawUses)
        return;
    if (!isPureOperation(value->opcode()) && !(B3::isLoad(value->opcode()) && !value->traps()))
        return;
    for (const B3::Value* child : value->children())
        releaseRawUse(child);
}

bool DirectCodeGenerator::isSameValue(const B3::Value* a, const B3::Value* b, unsigned depth) const
{
    if (a->type() != b->type())
        return false;
    a = resolve(a);
    b = resolve(b);
    if (a == b)
        return true;
    if (a->type() != b->type())
        return false;
    int64_t first;
    int64_t second;
    if (constantOf(a, first))
        return constantOf(b, second) && first == second;
    if (depth >= 3 || a->opcode() != b->opcode() || !isPureOperation(a->opcode()) || a->numChildren() != b->numChildren())
        return false;
    for (unsigned index = 0; index < a->numChildren(); ++index) {
        if (!isSameValue(a->child(index), b->child(index), depth + 1))
            return false;
    }
    return true;
}

std::pair<const B3::Value*, bool> DirectCodeGenerator::truthSourceOf(const B3::Value* condition) const
{
    bool isNegated = false;
    for (;;) {
        const B3::Value* resolved = resolve(condition);
        if (resolved->type() != condition->type())
            return { condition, isNegated };
        condition = resolved;
        int64_t constant;
        if (condition->opcode() == B3::BitXor && constantOf(condition->child(1), constant) && constant == 1 && isComparison(resolve(condition->child(0))->opcode())) {
            isNegated = !isNegated;
            condition = condition->child(0);
            continue;
        }
        if ((condition->opcode() == B3::Equal || condition->opcode() == B3::NotEqual) && condition->child(0)->type().isInt() && isZero(condition->child(1))) {
            if (condition->opcode() == B3::Equal)
                isNegated = !isNegated;
            condition = condition->child(0);
            continue;
        }
        return { condition, isNegated };
    }
}

std::optional<bool> DirectCodeGenerator::knownTruthOf(uint32_t order, const B3::Value* condition)
{
    AOT_THRESHOLD(unsigned, maxBlocksLookedAt, 12);
    auto [wanted, wantedIsNegated] = truthSourceOf(condition);
    for (unsigned steps = 0; order && !m_blocks[order].startsEmpty && steps < maxBlocksLookedAt; ++steps) {
        DirectBlock& parent = m_blocks[m_blocks[order].predecessor];
        const B3::Value* last = parent.block->last();
        if (last->opcode() == B3::Branch && !parent.onlySuccessor) {
            bool isTaken = &successorOf(parent, 0) == &m_blocks[order];
            bool isNotTaken = &successorOf(parent, 1) == &m_blocks[order];
            if (isTaken != isNotTaken) {
                auto [known, knownIsNegated] = truthSourceOf(last->child(0));
                if (isSameValue(known, wanted, 0))
                    return (isTaken != knownIsNegated) != wantedIsNegated;
            }
        }
        order = m_blocks[order].predecessor;
    }
    return std::nullopt;
}

std::pair<const B3::Value*, uint8_t> DirectCodeGenerator::scaledOperandOf(const B3::Value* candidate) const
{
    candidate = resolve(candidate);
    const ValueState& state = stateOf(candidate);
    if ((state.form != ValueForm::Plain && state.form != ValueForm::Dead) || state.stretch != m_user.stretch)
        return { nullptr, 0 };
    int64_t constant;
    if (candidate->opcode() == B3::Shl && candidate->type() == B3::Int64 && constantOf(candidate->child(1), constant) && constant >= 1 && constant <= 3 && !isConstant(candidate->child(0)))
        return { resolve(candidate->child(0)), static_cast<uint8_t>(constant) };
    if (candidate->opcode() == B3::Mul && candidate->type() == B3::Int64) {
        for (unsigned index = 0; index < 2; ++index) {
            if (constantOf(candidate->child(index), constant) && (constant == 2 || constant == 4 || constant == 8) && !isConstant(candidate->child(1 - index)))
                return { resolve(candidate->child(1 - index)), static_cast<uint8_t>(std::countr_zero(static_cast<uint64_t>(constant))) };
        }
    }
    return { nullptr, 0 };
}

AddressPlan DirectCodeGenerator::planAddress(const B3::Value* pointer, int64_t offset) const
{
    AddressPlan plan;
    for (;;) {
        pointer = resolve(pointer);
        const ValueState& state = stateOf(pointer);
        if (state.form == ValueForm::FixedRegister) {
            plan.fixedBase = decode(state.base).gpr();
            break;
        }
        if (state.form == ValueForm::Rematerialized) {
            if (pointer->opcode() == B3::SlotBase) {
                plan.fixedBase = MacroAssembler::framePointerRegister;
                plan.slot = pointer->as<B3::SlotBaseValue>()->slot();
                break;
            }
            if (pointer->opcode() == B3::Add && fitsInt32(offset + state.constant)) {
                plan.fixedBase = decode(state.base).gpr();
                offset += state.constant;
                break;
            }
            plan.base = pointer;
            break;
        }
        if ((state.form == ValueForm::Plain || state.form == ValueForm::Dead) && pointer->opcode() == B3::Add && pointer->type() == B3::Int64) {
            const B3::Value* left = resolve(pointer->child(0));
            const B3::Value* right = resolve(pointer->child(1));
            int64_t constant;
            if (constantOf(right, constant) && fitsInt32(offset + constant)) {
                offset += constant;
                pointer = left;
                continue;
            }
            if (constantOf(left, constant) && fitsInt32(offset + constant)) {
                offset += constant;
                pointer = right;
                continue;
            }
            if (!plan.index && state.stretch == m_user.stretch && !isConstant(left) && !isConstant(right)) {
                if (auto [operand, shift] = scaledOperandOf(right); operand) {
                    plan.index = operand;
                    plan.shift = shift;
                    pointer = left;
                    continue;
                }
                if (auto [operand, shift] = scaledOperandOf(left); operand) {
                    plan.index = operand;
                    plan.shift = shift;
                    pointer = right;
                    continue;
                }
                plan.index = right;
                pointer = left;
                continue;
            }
        }
        plan.base = pointer;
        break;
    }
    plan.offset = static_cast<int32_t>(offset);
    return plan;
}

bool DirectCodeGenerator::isFoldableCondition(const B3::Value* condition) const
{
    const ValueState& state = stateOf(condition);
    if (state.form != ValueForm::Plain || state.rawUses != 1 || state.block != m_user.block)
        return false;
    if (condition->opcode() == B3::BitXor) {
        int64_t constant;
        const B3::Value* negated = condition->child(0);
        return constantOf(condition->child(1), constant) && constant == 1 && (isComparison(negated->opcode()) || negated->opcode() == B3::BitXor) && isFoldableCondition(negated);
    }
    if (B3::isLoad(condition->opcode()))
        return condition->opcode() != B3::Load8S && condition->opcode() != B3::Load16S && condition->type().isInt() && canFoldLoad(condition);
    return isComparison(condition->opcode()) || condition->opcode() == B3::BitAnd;
}

bool DirectCodeGenerator::isLogicalCondition(const B3::Value* condition) const
{
    if ((condition->opcode() != B3::BitOr && condition->opcode() != B3::BitAnd))
        return false;
    const B3::Value* first = condition->child(0);
    const B3::Value* second = condition->child(1);
    if (!isComparison(first->opcode()) || !isComparison(second->opcode()) || first == second)
        return false;
    if (first->child(0)->opcode() == B3::BitXor || second->child(0)->opcode() == B3::BitXor)
        return false;
    if (!m_isPlanning)
        return stateOf(condition).form == ValueForm::Folded && stateOf(first).form == ValueForm::Folded && stateOf(second).form == ValueForm::Folded;
    const ValueState& state = stateOf(condition);
    return state.form == ValueForm::Plain && state.rawUses == 1 && state.block == m_user.block && isFoldableCondition(first) && isFoldableCondition(second);
}

void DirectCodeGenerator::fetchConditionOperands(const B3::Value* comparison)
{
    const B3::Value* left = comparison->child(0);
    const B3::Value* right = comparison->child(1);
    if (left->type() == B3::Double) {
        fprFor(left);
        fprFor(right);
        return;
    }
    if ((comparison->opcode() == B3::Equal || comparison->opcode() == B3::NotEqual) && isZero(right) && stateOf(left).form == ValueForm::Folded && !B3::isLoad(left->opcode())) {
        if (isComparison(left->opcode()))
            fetchConditionOperands(left);
        else if (left->opcode() == B3::BitAnd) {
            OperandPlan plan = planTest(left->child(0), left->child(1), true);
            if (plan.load)
                operandForLoad(plan.load);
            else {
                gprFor(plan.left);
                if (plan.shape == OperandShape::Register || plan.shape == OperandShape::BitTest)
                    gprFor(plan.right);
            }
        }
        return;
    }
    OperandPlan plan = planComparison(comparison->opcode(), left, right, true);
    if (plan.load)
        operandForLoad(plan.load);
    if (plan.left != plan.load)
        gprFor(plan.left);
    if (plan.right != plan.load && plan.shape != OperandShape::Immediate && plan.shape != OperandShape::PinnedMemory && plan.shape != OperandShape::PinnedRegister)
        gprFor(plan.right);
}

bool DirectCodeGenerator::canFoldLoad(const B3::Value* load) const
{
    if constexpr (!isX86_64())
        return false;
    const ValueState& state = stateOf(load);
    if (state.form != ValueForm::Plain || state.rawUses != 1 || state.memoryEpoch != m_user.memoryEpoch || state.block != m_user.block)
        return false;
    return B3::isLoad(load->opcode()) && !load->traps();
}

bool DirectCodeGenerator::replacesImmediate(int64_t constant, bool hasOneByteForm, OperandPlan& plan) const
{
    if ((hasOneByteForm && constant == static_cast<int8_t>(constant)))
        return false;
    std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(constant);
    if (!pinned || !pinned->replacesImmediates || (pinned->offset && (!isX86_64() || *pinned->offset != static_cast<int8_t>(*pinned->offset))))
        return false;
    plan.pinned = *pinned;
    plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
    return true;
}

OperandPlan DirectCodeGenerator::planComparison(B3::Opcode opcode, const B3::Value* left, const B3::Value* right, bool mayFoldLoads) const
{
    OperandPlan plan;
    bool isWide = left->type() == B3::Int64;
    if (isConstant(left) && !isConstant(right)) {
        std::swap(left, right);
        plan.isCommuted = true;
    }
    plan.left = left;
    plan.right = right;
    MacroAssembler::RelationalCondition condition = relationalConditionOf(opcode);
    auto isWholeLoad = [&](const B3::Value* candidate) {
        return mayFoldLoads && candidate->opcode() == B3::Load && candidate->type() == left->type() && loadIsFolded(candidate);
    };
    int64_t constant;
    if (constantOf(right, constant)) {
        if (!isWide || fitsInt32(constant)) {
            plan.shape = OperandShape::Immediate;
            plan.immediate = constant;
            if (!mayFoldLoads)
                return plan;
            if (isWide && !isWholeLoad(left) && replacesImmediate(constant, true, plan))
                return plan;
            if (left->opcode() == B3::Load8Z && comparesWithoutSign(condition) && constant >= 0 && constant <= 255 && loadIsFolded(left)) {
                plan.load = left;
                plan.loadWidth = 8;
            } else if (left->opcode() == B3::Load16Z && comparesWithoutSign(condition) && constant >= 0 && constant <= 65535 && loadIsFolded(left)) {
                plan.load = left;
                plan.loadWidth = 16;
            } else if (isWholeLoad(left)) {
                plan.load = left;
                plan.loadWidth = isWide ? 64 : 32;
            }
            plan.loadIsLeft = !!plan.load;
            return plan;
        }
        if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(constant)) {
            plan.pinned = *pinned;
            plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
            if (plan.shape == OperandShape::PinnedRegister || isX86_64())
                return plan;
        }
        plan.shape = OperandShape::Register;
        return plan;
    }
    if (isWholeLoad(right)) {
        plan.shape = OperandShape::Load;
        plan.load = right;
        plan.loadWidth = isWide ? 64 : 32;
        return plan;
    }
    if (isWholeLoad(left)) {
        plan.shape = OperandShape::Load;
        plan.load = left;
        plan.loadIsLeft = true;
        plan.loadWidth = isWide ? 64 : 32;
        return plan;
    }
    return plan;
}

OperandPlan DirectCodeGenerator::planTest(const B3::Value* left, const B3::Value* right, bool mayFoldLoads) const
{
    OperandPlan plan;
    bool isWide = left->type() == B3::Int64;
    if (isConstant(left) && !isConstant(right))
        std::swap(left, right);
    plan.left = left;
    plan.right = right;
    int64_t constant;
    if (!constantOf(right, constant))
        return plan;
    if (isX86_64() && mayFoldLoads && constant == 1 && left->opcode() == B3::ZShr && !isConstant(left->child(1))) {
        const ValueState& shiftState = stateOf(left);
        if (m_isPlanning ? shiftState.form == ValueForm::Plain && shiftState.rawUses == 1 && shiftState.block == m_user.block : shiftState.form == ValueForm::Folded) {
            plan.shape = OperandShape::BitTest;
            plan.foldedShift = left;
            plan.left = left->child(0);
            plan.right = left->child(1);
            return plan;
        }
    }
    if (!isWide || fitsInt32(constant)) {
        plan.shape = OperandShape::Immediate;
        plan.immediate = constant;
        if (!mayFoldLoads)
            return plan;
        if (isWide && !(left->opcode() == B3::Load && loadIsFolded(left)) && (constant < 0 || constant > 127) && replacesImmediate(constant, false, plan))
            return plan;
        if (left->opcode() == B3::Load8Z && constant >= 0 && constant <= 255 && loadIsFolded(left)) {
            plan.load = left;
            plan.loadWidth = 8;
        } else if (left->opcode() == B3::Load16Z && constant >= 0 && constant <= 65535 && loadIsFolded(left)) {
            plan.load = left;
            plan.loadWidth = 16;
        } else if (left->opcode() == B3::Load && left->type().isInt() && loadIsFolded(left)) {
            plan.load = left;
            plan.loadWidth = isWide ? 64 : 32;
        }
        plan.loadIsLeft = !!plan.load;
        return plan;
    }
    if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(constant)) {
        plan.pinned = *pinned;
        plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
        if (plan.shape == OperandShape::PinnedRegister || isX86_64())
            return plan;
    }
    plan.shape = OperandShape::Register;
    return plan;
}

OperandPlan DirectCodeGenerator::planBinary(const B3::Value* value) const
{
    OperandPlan plan;
    B3::Opcode opcode = value->opcode();
    const B3::Value* left = value->child(0);
    const B3::Value* right = value->child(1);
    bool isWide = value->type() == B3::Int64;
    if (isCommutative(opcode) && isConstant(left) && !isConstant(right))
        std::swap(left, right);
    plan.left = left;
    plan.right = right;
    int64_t constant;
    if (constantOf(right, constant)) {
        std::optional<B3::PinnedConstant> pinned;
        if (isWide && opcode != B3::Mul)
            pinned = pinnedConstantFor(constant);
        bool isImmediate = !isWide || fitsInt32(constant);
        if (pinned && (!isImmediate || pinned->replacesImmediates) && (!pinned->offset || isX86_64())) {
            plan.pinned = *pinned;
            plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
            return plan;
        }
        if (isImmediate) {
            plan.shape = OperandShape::Immediate;
            plan.immediate = constant;
        }
        return plan;
    }
    bool mayBeAddress = opcode == B3::Add && isWide;
    if (mayBeAddress) {
        if (auto [operand, shift] = scaledOperandOf(right); operand) {
            plan.shape = OperandShape::ScaledIndex;
            plan.right = operand;
            plan.shift = shift;
        } else if (auto [operand, shift] = scaledOperandOf(left); operand) {
            plan.shape = OperandShape::ScaledIndex;
            plan.left = right;
            plan.right = operand;
            plan.shift = shift;
        }
        return plan;
    }
    if (opcode != B3::Mul && right->opcode() == B3::Load && right->type() == value->type() && loadIsFolded(right)) {
        plan.shape = OperandShape::Load;
        plan.load = right;
        return plan;
    }
    if (isCommutative(opcode) && opcode != B3::Mul && left->opcode() == B3::Load && left->type() == value->type() && loadIsFolded(left)) {
        plan.shape = OperandShape::Load;
        plan.load = left;
        plan.left = right;
        plan.right = left;
        return plan;
    }
    return plan;
}

bool DirectCodeGenerator::storesImmediate(const B3::Value* stored, B3::Opcode opcode) const
{
    int64_t constant;
    if (!constantOf(stored, constant) || stored->type() == B3::Double)
        return false;
    return opcode != B3::Store || stored->type() == B3::Int32 || fitsInt32(constant);
}

void DirectCodeGenerator::noteUse(const B3::Value* child, Reg hint)
{
    child = resolve(child);
    ValueState& state = stateOf(child);
    switch (state.form) {
    case ValueForm::FixedRegister:
        if (decode(state.base) == Reg(MacroAssembler::framePointerRegister))
            m_needsFrame = m_needsFrameRecord = m_blocks[m_user.block].usesFrame = true;
        return;
    case ValueForm::Constant:
    case ValueForm::Rematerialized:
        state.lastUse = std::max(state.lastUse, m_user.position);
        if (state.form == ValueForm::Rematerialized && (child->opcode() == B3::SlotBase || decode(state.base) == Reg(MacroAssembler::framePointerRegister)))
            m_needsFrame = m_needsFrameRecord = m_blocks[m_user.block].usesFrame = true;
        return;
    case ValueForm::Plain:
        break;
    case ValueForm::Dead:
    case ValueForm::Alias:
    case ValueForm::Folded:
        refuse();
        return;
    }
    if (state.uses != UINT16_MAX)
        ++state.uses;
    uint32_t end = m_user.position;
    for (uint32_t loop = m_blocks[m_user.block].loop; loop; loop = m_loops[loop - 1].parent) {
        if (state.position < m_loops[loop - 1].firstPosition)
            end = std::max(end, m_loops[loop - 1].lastPosition);
    }
    state.lastUse = std::max(state.lastUse, end);
    if (state.block == m_user.block)
        state.lastUseInOwnBlock = std::max(state.lastUseInOwnBlock, m_user.position);
    else {
        uint32_t home = ensureHome(child);
        uint32_t first = m_homes[home].firstExposedUse;
        if (!first || m_exposedUses[first - 1].block != m_user.block) {
            m_exposedUses.append(ExposedUse { m_user.block, home, m_user.position, first });
            m_homes[home].firstExposedUse = m_exposedUses.size();
            ++m_homes[home].numberOfExposedUses;
        } else
            m_exposedUses[first - 1].lastPosition = std::max(m_exposedUses[first - 1].lastPosition, m_user.position);
    }
    if (state.stretch != m_user.stretch) {
        state.flags |= NeedsHome;
        HomeState& home = m_homes[ensureHome(child)];
        if (home.stretchOfLastUse != m_user.stretch) {
            home.gain += home.gainInStretch;
            home.stretchOfLastUse = m_user.stretch;
            home.gainInStretch = 0;
        }
        if (m_bytesOfLoadAtUse)
            home.gainInStretch = hint ? 1 : m_bytesOfLoadAtUse;
    } else if (state.shortStretch != m_user.shortStretch)
        state.flags |= MayNeedHome;
    if (hint && !(state.hint && m_blocks[m_user.block].isRare && !m_blocks[state.block].isRare))
        state.hint = encode(hint);
}

void DirectCodeGenerator::noteLateUse(const B3::Value* child, uint32_t stretchAfter)
{
    uint32_t stretch = std::exchange(m_user.stretch, stretchAfter);
    uint32_t shortStretch = std::exchange(m_user.shortStretch, 0);
    noteUse(child);
    m_user.stretch = stretch;
    m_user.shortStretch = shortStretch;
}

void DirectCodeGenerator::noteAddress(const AddressPlan& plan)
{
    if (plan.base)
        noteUse(plan.base);
    if (plan.index)
        noteUse(plan.index);
    if (plan.fixedBase == MacroAssembler::framePointerRegister)
        m_needsFrame = m_needsFrameRecord = m_blocks[m_user.block].usesFrame = true;
}

void DirectCodeGenerator::noteOperands(const OperandPlan& plan, Reg hintForLeft)
{
    if (plan.foldedShift)
        stateOf(plan.foldedShift).form = ValueForm::Folded;
    if (plan.load) {
        stateOf(plan.load).form = ValueForm::Folded;
        const B3::MemoryValue* memory = plan.load->as<B3::MemoryValue>();
        noteAddress(planAddress(memory->lastChild(), memory->offset()));
    }
    if (plan.left != plan.load)
        noteUse(plan.left, hintForLeft);
    if (plan.right != plan.load && plan.shape != OperandShape::Immediate && plan.shape != OperandShape::PinnedMemory && plan.shape != OperandShape::PinnedRegister)
        noteUse(plan.right);
}

void DirectCodeGenerator::noteConditionOperands(const B3::Value* condition)
{
    if (B3::isLoad(condition->opcode())) {
        const B3::MemoryValue* memory = condition->as<B3::MemoryValue>();
        noteAddress(planAddress(memory->lastChild(), memory->offset()));
        return;
    }
    const B3::Value* left = condition->child(0);
    const B3::Value* right = condition->child(1);
    if (condition->opcode() == B3::BitAnd) {
        noteOperands(planTest(left, right, true));
        return;
    }
    if (condition->opcode() == B3::BitXor) {
        stateOf(left).form = ValueForm::Folded;
        noteConditionOperands(left);
        return;
    }
    if (left->type() == B3::Double) {
        noteUse(left);
        noteUse(right);
        return;
    }
    if ((condition->opcode() == B3::Equal || condition->opcode() == B3::NotEqual) && isZero(right) && !B3::isLoad(left->opcode()) && isFoldableCondition(left)) {
        stateOf(left).form = ValueForm::Folded;
        noteConditionOperands(left);
        return;
    }
    noteOperands(planComparison(condition->opcode(), left, right, true));
}

void DirectCodeGenerator::noteCondition(const B3::Value* condition)
{
    if (isFoldableCondition(condition)) {
        stateOf(condition).form = ValueForm::Folded;
        noteConditionOperands(condition);
        return;
    }
    noteUse(condition);
}

void DirectCodeGenerator::noteStackmapChildren(B3::StackmapValue* stackmap, unsigned first, const ValueState& state)
{
    if (!stackmap->runsWithoutFrame() || stackmap->followsFrameSetup() || stackmap->needsFrameRecord())
        m_needsFrame = m_blocks[m_user.block].usesFrame = true;
    if ((bitsOf(stackmap->earlyClobbered()) | bitsOf(stackmap->lateClobbered())) & (m_generalRegisters | m_doubleRegisters))
        m_blocks[m_user.block].usesFrame = true;
    if (stackmap->needsFrameRecord())
        m_needsFrameRecord = true;
    for (unsigned index = first; index < stackmap->numChildren(); ++index) {
        B3::ConstrainedValue child = stackmap->constrainedChild(index);
        switch (child.rep().kind()) {
        case B3::ValueRep::WarmAny:
        case B3::ValueRep::ColdAny:
            m_bytesOfLoadAtUse = 0;
            noteUse(child.value());
            m_bytesOfLoadAtUse = bytesOfHomeLoad;
            break;
        case B3::ValueRep::SomeRegister:
        case B3::ValueRep::SomeRegisterWithClobber:
            noteUse(child.value());
            break;
        case B3::ValueRep::LateColdAny:
            m_bytesOfLoadAtUse = 0;
            noteLateUse(child.value(), state.stretch);
            m_bytesOfLoadAtUse = bytesOfHomeLoad;
            break;
        case B3::ValueRep::SomeLateRegister:
            noteLateUse(child.value(), state.stretch);
            break;
        case B3::ValueRep::Register:
            noteUse(child.value(), child.rep().reg());
            m_clobberedRegisters |= bitOfRegister(child.rep().reg());
            break;
        case B3::ValueRep::StackArgument:
            noteUse(child.value());
            m_bytesOfStackArguments = std::max<unsigned>(m_bytesOfStackArguments, child.rep().offsetFromSP() + sizeof(CPURegister));
            m_needsFrame = m_needsFrameRecord = m_blocks[m_user.block].usesFrame = true;
            break;
        default:
            refuse();
            return;
        }
    }
}

void DirectCodeGenerator::noteUsesOf(B3::Value* value, ValueState& state)
{
    if (state.form != ValueForm::Plain)
        return;
    m_user = userOf(state);
    B3::Opcode opcode = value->opcode();

    bool mustBeGenerated = false;
    switch (opcode) {
    case B3::Set:
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
    case B3::Check:
    case B3::CheckAdd:
    case B3::CheckSub:
    case B3::CheckMul:
    case B3::Jump:
    case B3::Branch:
    case B3::Switch:
    case B3::Return:
    case B3::Oops:
    case B3::Upsilon:
    case B3::Fence:
        mustBeGenerated = true;
        break;
    case B3::Patchpoint:
        mustBeGenerated = value->as<B3::PatchpointValue>()->effects.mustExecute();
        if (!mustBeGenerated) {
            for (const B3::Value* extract = value->type().isTuple() ? state.target : nullptr; extract; extract = stateOf(extract).target)
                mustBeGenerated |= stateOf(extract).form == ValueForm::Plain;
        }
        break;
    default:
        mustBeGenerated = value->traps();
        break;
    }
    if (!mustBeGenerated && !state.uses) {
        state.form = ValueForm::Dead;
        return;
    }
    if (state.flags & NeedsHome)
        ensureHome(value);
    else if (state.flags & MayNeedHome)
        m_valuesWithReservedHomes.append(value);
    if ((state.flags & (NeedsHome | MayNeedHome)) && opcode != B3::ArgumentReg)
        m_blocks[state.block].usesFrame = true;

    switch (opcode) {
    case B3::Set:
        noteUse(value->child(0));
        m_needsFrame = m_needsFrameRecord = m_blocks[state.block].usesFrame = true;
        return;
    case B3::Get:
        m_needsFrame = m_needsFrameRecord = m_blocks[state.block].usesFrame = true;
        return;
    case B3::ArgumentReg:
    case B3::Extract:
    case B3::Phi:
    case B3::Jump:
    case B3::Oops:
    case B3::Fence:
        return;
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::Div:
        if (state.flags & IsDouble) {
            noteUse(value->child(0));
            noteUse(value->child(1));
            return;
        }
        if (opcode == B3::Div) {
            noteUse(value->child(0), Reg(GPRInfo::returnValueGPR));
            noteUse(value->child(1));
            return;
        }
        noteOperands(planBinary(value), decode(state.hint));
        return;
    case B3::Mod:
    case B3::UDiv:
    case B3::UMod:
    case B3::UMulHigh:
    case B3::MulHigh:
        noteUse(value->child(0), Reg(GPRInfo::returnValueGPR));
        noteUse(value->child(1));
        return;
    case B3::FMax:
    case B3::FMin:
        noteUse(value->child(0));
        noteUse(value->child(1));
        return;
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        noteOperands(planBinary(value), decode(state.hint));
        return;
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
        noteUse(value->child(0), decode(state.hint));
        if (!isConstant(value->child(1)))
            noteUse(value->child(1), shiftAmountRegister());
        return;
    case B3::Neg:
    case B3::Clz:
    case B3::PopCount:
    case B3::Ceil:
    case B3::Floor:
    case B3::FTrunc:
    case B3::Sqrt:
    case B3::Abs:
    case B3::BitwiseCast:
    case B3::IToD:
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::ZExt32:
    case B3::Trunc:
        noteUse(value->child(0));
        return;
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::EqualOrUnordered:
        if (value->child(0)->type() == B3::Double) {
            noteUse(value->child(0));
            noteUse(value->child(1));
            return;
        }
        noteOperands(planComparison(opcode, value->child(0), value->child(1), false));
        return;
    case B3::Select:
        noteCondition(value->child(0));
        noteUse(value->child(1));
        noteUse(value->child(2), decode(state.hint));
        return;
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load: {
        AddressPlan plan = planAddress(value->lastChild(), value->as<B3::MemoryValue>()->offset());
        if (value->traps()) {
            bool isNarrow = plan.index && plan.fixedBase == cellCageBaseGPR && plan.shift == cell32Shift;
            if ((plan.index && !isNarrow) || (!plan.index && (!plan.base || stateOf(plan.base).form != ValueForm::Plain))) {
                refuse();
                return;
            }
        }
        noteAddress(plan);
        return;
    }
    case B3::Store8:
    case B3::Store16:
    case B3::Store: {
        if (value->traps()) {
            refuse();
            return;
        }
        noteUse(value->child(0));
        noteAddress(planAddress(value->lastChild(), value->as<B3::MemoryValue>()->offset()));
        return;
    }
    case B3::Patchpoint: {
        B3::PatchpointValue* patchpoint = value->as<B3::PatchpointValue>();
        if (patchpoint->effects.terminal) {
            DirectBlock& block = m_blocks[state.block];
            if (patchpoint->runsWithoutFrame())
                ++m_numberOfTeardowns;
            bool hasLiveResult = state.uses;
            for (const B3::Value* extract = value->type().isTuple() ? state.target : nullptr; extract; extract = stateOf(extract).target)
                hasLiveResult |= stateOf(extract).form == ValueForm::Plain;
            if (hasLiveResult && (!block.block->numSuccessors() || successorOf(block, 0).startsEmpty)) {
                refuse();
                return;
            }
            if (block.block->numSuccessors() > 31) {
                refuse();
                return;
            }
        }
        noteStackmapChildren(patchpoint, 0, state);
        return;
    }
    case B3::Check: {
        int64_t constant;
        if (constantOf(value->child(0), constant) && !constant) {
            state.form = ValueForm::Dead;
            return;
        }
        noteStackmapChildren(value->as<B3::StackmapValue>(), 1, state);
        if (!isConstant(value->child(0)))
            noteCondition(value->child(0));
        return;
    }
    case B3::CheckAdd:
    case B3::CheckSub:
    case B3::CheckMul:
        noteStackmapChildren(value->as<B3::StackmapValue>(), 2, state);
        noteUse(value->child(0));
        if (!isConstant(value->child(1)) || (state.flags & IsWide) || opcode == B3::CheckMul)
            noteUse(value->child(1));
        return;
    case B3::Upsilon: {
        const B3::Value* phi = value->as<B3::UpsilonValue>()->phi();
        ValueState& phiState = stateOf(phi);
        DirectBlock& block = m_blocks[state.block];
        bool isBackEdge = phiState.position <= state.position;
        if (phiState.form == ValueForm::Alias || (!isBackEdge && phiState.form == ValueForm::Dead)) {
            state.form = ValueForm::Dead;
            return;
        }
        if (isBackEdge && phiState.uses != UINT16_MAX)
            ++phiState.uses;
        bool leadsToPhi = false;
        for (unsigned index = 0; index < block.block->numSuccessors(); ++index)
            leadsToPhi |= &successorOf(block, index) == &m_blocks[phiState.block];
        if (!leadsToPhi || block.block->last()->opcode() != B3::Jump)
            phiState.flags |= ArrivesInSlot;
        m_user.position = block.lastPosition;
        m_user.stretch = block.stretchBeforeTerminator;
        m_user.shortStretch = block.shortStretchBeforeTerminator;
        m_bytesOfLoadAtUse = 0;
        noteUse(value->child(0));
        m_bytesOfLoadAtUse = bytesOfHomeLoad;
        {
            uint32_t home = ensureHome(phi);
            m_upsilons.append(UpsilonRecord { value->as<B3::UpsilonValue>(), state.block, m_homes[home].firstUpsilon });
            m_homes[home].firstUpsilon = m_upsilons.size();
        }
        const B3::Value* source = resolve(value->child(0));
        ValueState& sourceState = stateOf(source);
        if (sourceState.form == ValueForm::Plain) {
            sourceState.phi = phi->index() + 1;
            if (!phiState.hint && source->opcode() == B3::Patchpoint) {
                const auto& constraints = source->as<B3::PatchpointValue>()->resultConstraints;
                if (constraints.size() == 1 && constraints[0].kind() == B3::ValueRep::Register)
                    phiState.hint = encode(constraints[0].reg());
            }
        }
        return;
    }
    case B3::Branch: {
        DirectBlock& block = m_blocks[state.block];
        if (block.onlySuccessor)
            return;
        if (&targetOf(block, 0) == &targetOf(block, 1)) {
            block.branchesToOneBlock = true;
            return;
        }
        if (isLogicalCondition(value->child(0))) {
            const B3::Value* condition = value->child(0);
            stateOf(condition).form = ValueForm::Folded;
            for (unsigned index = 0; index < 2; ++index) {
                stateOf(condition->child(index)).form = ValueForm::Folded;
                noteConditionOperands(condition->child(index));
            }
            return;
        }
        if (!isConstant(value->child(0)))
            noteCondition(value->child(0));
        return;
    }
    case B3::Switch:
        noteUse(value->child(0));
        return;
    case B3::Return:
        ++m_numberOfTeardowns;
        if (value->numChildren())
            noteUse(value->child(0), value->child(0)->type() == B3::Double ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR));
        return;
    default:
        refuse();
        return;
    }
}

void DirectCodeGenerator::noteUses()
{
    for (uint32_t order = m_blocks.size(); order--;) {
        DirectBlock& state = m_blocks[order];
        B3::BasicBlock* block = state.block;
        bool hasCode = false;
        for (unsigned index = block->size(); index--;) {
            B3::Value* value = block->at(index);
            noteUsesOf(value, stateOf(value));
            if (m_isRefused)
                return;
            hasCode |= index + 1 < block->size() && stateOf(value).form == ValueForm::Plain;
        }
        if (hasCode || !order || !state.isReachable || (block->last()->opcode() != B3::Jump && !state.branchesToOneBlock && !state.onlySuccessor))
            continue;
        if (!state.startsEmpty && m_blocks[state.predecessor].block->last()->opcode() == B3::Patchpoint && m_blocks[state.predecessor].block->last()->type() != B3::Void)
            continue;
        uint32_t target = static_cast<uint32_t>(&targetOf(state, state.onlySuccessor == 2 ? 1 : 0) - m_blocks.span().data());
        if (target != order)
            state.forwardsTo = target;
    }
}

bool DirectCodeGenerator::isAncestorOrSelf(uint32_t ancestor, uint32_t block) const
{
    for (;;) {
        if (block == ancestor)
            return true;
        if (block < ancestor || !block || m_blocks[block].startsEmpty)
            return false;
        block = m_blocks[block].predecessor;
    }
}

bool DirectCodeGenerator::tryReuseLoad(B3::Value* value, ValueState& state, uint32_t memoryEpoch)
{
    const B3::MemoryValue* memory = value->as<B3::MemoryValue>();
    const B3::Value* base = resolve(memory->lastChild());
    size_t hash = (static_cast<size_t>(base->index()) * 31 + static_cast<size_t>(memory->offset()) * 7 + static_cast<size_t>(value->opcode())) % m_knownLoads.size();
    KnownLoad& known = m_knownLoads[hash];
    if (known.load && !value->traps()) {
        const B3::MemoryValue* earlier = known.load->as<B3::MemoryValue>();
        const ValueState& earlierState = stateOf(known.load);
        bool isSameAccess = earlier->opcode() == value->opcode() && earlier->type() == value->type() && earlier->offset() == memory->offset() && resolve(earlier->lastChild()) == base;
        bool isUnchanged = known.memoryEpoch == memoryEpoch || (memory->readsMutability() == B3::Mutability::Immutable && earlierState.stretch == state.stretch);
        if (isSameAccess && isUnchanged && isAncestorOrSelf(earlierState.block, state.block)) {
            makeAlias(state, known.load, false);
            uint16_t& rawUses = stateOf(base).rawUses;
            if (rawUses && rawUses != UINT16_MAX)
                --rawUses;
            return true;
        }
    }
    known.load = value;
    known.memoryEpoch = memoryEpoch;
    return false;
}

bool DirectCodeGenerator::tryReusePureCall(const B3::PatchpointValue* patchpoint, ValueState& state)
{
    if (patchpoint->type().isTuple() || patchpoint->type() == B3::Void || patchpoint->effects.terminal)
        return false;
    size_t hash = static_cast<size_t>(patchpoint->pureFunction->id * 0x9e3779b97f4a7c15ULL >> 32);
    if (patchpoint->pureFunction->dependsOnChildren) {
        for (const B3::Value* child : patchpoint->children())
            hash = hash * 31 + resolve(child)->index();
    }
    const B3::PatchpointValue*& known = m_knownPureCalls[hash % m_knownPureCalls.size()];
    auto canonicalChild = [&](const B3::Value* child) {
        return resolve(child);
    };
    if (known && B3::isSamePureCall(known, patchpoint, canonicalChild) && stateOf(known).form == ValueForm::Plain && isAncestorOrSelf(stateOf(known).block, state.block)) {
        for (const B3::Value* child : patchpoint->children()) {
            uint16_t& rawUses = stateOf(resolve(child)).rawUses;
            if (rawUses && rawUses != UINT16_MAX)
                --rawUses;
        }
        makeAlias(state, known, false);
        return true;
    }
    known = patchpoint;
    return false;
}

uint32_t DirectCodeGenerator::ensureHome(const B3::Value* value)
{
    ValueState& state = stateOf(value);
    if (state.homeIndex)
        return state.homeIndex - 1;
    uint32_t index = m_homes.size();
    DirectBlock& block = m_blocks[state.block];
    HomeState home { };
    home.value = value->index();
    home.nextDefinedInBlock = block.firstDefinition;
    home.parent = index;
    home.lastInWeb = index;
    home.webSize = 1;
    m_homes.append(home);
    block.firstDefinition = index + 1;
    state.homeIndex = index + 1;
    return index;
}

bool DirectCodeGenerator::everyLoopIsEnteredAtItsTop()
{
    if (m_backEdges.isEmpty())
        return true;
    uint32_t count = m_blocks.size();
    Vector<uint32_t> firstPredecessor;
    firstPredecessor.fill(0, count + 2);
    auto forEachEdge = [&](auto&& function) {
        for (uint32_t order = 0; order < count; ++order) {
            const DirectBlock& block = m_blocks[order];
            for (unsigned index = 0; index < block.block->numSuccessors(); ++index)
                function(order, successorOrderOf(block, index));
        }
    };
    forEachEdge([&](uint32_t, uint32_t target) {
        ++firstPredecessor[target + 2];
    });
    for (uint32_t order = 0; order < count; ++order)
        firstPredecessor[order + 2] += firstPredecessor[order + 1];
    Vector<uint32_t> predecessors;
    predecessors.fill(0, firstPredecessor[count + 1]);
    forEachEdge([&](uint32_t source, uint32_t target) {
        predecessors[firstPredecessor[target + 1]++] = source;
    });

    constexpr uint32_t unknown = UINT32_MAX;
    Vector<uint32_t> dominator;
    dominator.fill(unknown, count);
    dominator[0] = 0;
    auto intersect = [&](uint32_t a, uint32_t b) {
        while (a != b) {
            while (a > b)
                a = dominator[a];
            while (b > a)
                b = dominator[b];
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t order = 1; order < count; ++order) {
            uint32_t best = unknown;
            for (uint32_t index = firstPredecessor[order]; index < firstPredecessor[order + 1]; ++index) {
                uint32_t predecessor = predecessors[index];
                if (dominator[predecessor] != unknown)
                    best = best == unknown ? predecessor : intersect(predecessor, best);
            }
            if (best != unknown && dominator[order] != best) {
                dominator[order] = best;
                changed = true;
            }
        }
    }
    for (auto [first, last] : m_backEdges) {
        uint32_t walk = last;
        while (walk > first && dominator[walk] != unknown)
            walk = dominator[walk];
        if (walk != first)
            return false;
    }
    return true;
}

void DirectCodeGenerator::computeLiveness()
{
    AOT_THRESHOLD(size_t, maxWords, 1 << 20);
    m_liveWords = (m_homes.size() + 63) / 64;
    size_t total = m_liveWords * m_blocks.size();
    if (total > maxWords && !everyLoopIsEnteredAtItsTop())
        refuse();
    if (!total || total > maxWords)
        return;
    m_hasLiveness = true;
    m_liveIn.fill(0, total);
    size_t cursor = 0;
    for (uint32_t order = m_blocks.size(); order--;) {
        DirectBlock& block = m_blocks[order];
        uint64_t* live = m_liveIn.mutableSpan().data() + static_cast<size_t>(order) * m_liveWords;
        for (unsigned index = 0; index < block.block->numSuccessors(); ++index) {
            uint32_t successor = successorOrderOf(block, index);
            if (successor <= order)
                continue;
            const uint64_t* other = m_liveIn.span().data() + static_cast<size_t>(successor) * m_liveWords;
            for (size_t word = 0; word < m_liveWords; ++word)
                live[word] |= other[word];
        }
        for (uint32_t home = block.firstDefinition; home; home = m_homes[home - 1].nextDefinedInBlock)
            live[(home - 1) / 64] &= ~(static_cast<uint64_t>(1) << ((home - 1) % 64));
        for (; cursor < m_exposedUses.size() && m_exposedUses[cursor].block == order; ++cursor)
            live[m_exposedUses[cursor].home / 64] |= static_cast<uint64_t>(1) << (m_exposedUses[cursor].home % 64);
    }
    for (const Loop& loop : m_loops) {
        const uint64_t* header = m_liveIn.span().data() + static_cast<size_t>(loop.firstBlock) * m_liveWords;
        for (uint32_t order = loop.firstBlock + 1; order <= loop.lastBlock; ++order) {
            uint64_t* live = m_liveIn.mutableSpan().data() + static_cast<size_t>(order) * m_liveWords;
            for (size_t word = 0; word < m_liveWords; ++word)
                live[word] |= header[word];
        }
    }
    if (!m_backEdges.isEmpty()) {
        Vector<uint64_t, 16> live;
        live.fill(0, m_liveWords);
        bool hasIrregularLoops = false;
        for (bool changed = true; changed;) {
            changed = false;
            size_t use = 0;
            for (uint32_t order = m_blocks.size(); order--;) {
                DirectBlock& block = m_blocks[order];
                for (size_t word = 0; word < m_liveWords; ++word)
                    live[word] = 0;
                for (unsigned index = 0; index < block.block->numSuccessors(); ++index) {
                    const uint64_t* other = m_liveIn.span().data() + static_cast<size_t>(successorOrderOf(block, index)) * m_liveWords;
                    for (size_t word = 0; word < m_liveWords; ++word)
                        live[word] |= other[word];
                }
                for (uint32_t home = block.firstDefinition; home; home = m_homes[home - 1].nextDefinedInBlock)
                    live[(home - 1) / 64] &= ~(static_cast<uint64_t>(1) << ((home - 1) % 64));
                for (; use < m_exposedUses.size() && m_exposedUses[use].block == order; ++use)
                    live[m_exposedUses[use].home / 64] |= static_cast<uint64_t>(1) << (m_exposedUses[use].home % 64);
                uint64_t* known = m_liveIn.mutableSpan().data() + static_cast<size_t>(order) * m_liveWords;
                for (size_t word = 0; word < m_liveWords; ++word) {
                    if (live[word] & ~known[word]) {
                        known[word] |= live[word];
                        changed = true;
                    }
                }
            }
            hasIrregularLoops |= changed;
        }
        if (hasIrregularLoops) {
            for (uint32_t order = 0; order < m_blocks.size(); ++order) {
                const uint64_t* known = m_liveIn.span().data() + static_cast<size_t>(order) * m_liveWords;
                for (size_t word = 0; word < m_liveWords; ++word) {
                    for (uint64_t bits = known[word]; bits; bits &= bits - 1) {
                        ValueState& state = m_states[m_homes[word * 64 + std::countr_zero(bits)].value];
                        state.lastUse = std::max(state.lastUse, m_blocks[order].lastPosition);
                    }
                }
            }
        }
    }
}

bool DirectCodeGenerator::isLiveOut(uint32_t home, const DirectBlock& block)
{
    for (unsigned index = 0; index < block.block->numSuccessors(); ++index) {
        if (isLiveIn(home, successorOrderOf(block, index)))
            return true;
    }
    return false;
}

uint32_t DirectCodeGenerator::lastUseIn(uint32_t home, uint32_t block)
{
    AOT_THRESHOLD(uint32_t, maxUsesWalked, 12);
    const ValueState& state = m_states[m_homes[home].value];
    if (state.block == block)
        return state.lastUseInOwnBlock;
    if (uint32_t count = m_homes[home].numberOfExposedUses; count > maxUsesWalked) {
        if (!m_homes[home].firstOrderedUse) {
            m_homes[home].firstOrderedUse = m_orderedUses.size() + 1;
            for (uint32_t use = m_homes[home].firstExposedUse; use; use = m_exposedUses[use - 1].next)
                m_orderedUses.append(OrderedUse { m_exposedUses[use - 1].block, m_exposedUses[use - 1].lastPosition });
        }
        auto uses = m_orderedUses.span().subspan(m_homes[home].firstOrderedUse - 1, count);
        auto found = std::ranges::lower_bound(uses, block, { }, &OrderedUse::block);
        return found != uses.end() && found->block == block ? found->lastPosition : 0;
    }
    for (uint32_t use = m_homes[home].firstExposedUse; use; use = m_exposedUses[use - 1].next) {
        if (m_exposedUses[use - 1].block == block)
            return m_exposedUses[use - 1].lastPosition;
    }
    return 0;
}

// A phi is not live into its own block, yet its home is written at the end of each block that feeds it. A block that ends in a branch may feed the phis of both targets.
// noteUses() meets the blocks from the last to the first, so the records of a phi run from its first block to its last.
bool DirectCodeGenerator::areFedDifferentValuesInOneBlock(uint32_t first, uint32_t second)
{
    uint32_t a = m_homes[first].firstUpsilon;
    uint32_t b = m_homes[second].firstUpsilon;
    while (a && b) {
        const UpsilonRecord& recordOfFirst = m_upsilons[a - 1];
        const UpsilonRecord& recordOfSecond = m_upsilons[b - 1];
        if (recordOfFirst.block == recordOfSecond.block && resolve(recordOfFirst.upsilon->child(0)) != resolve(recordOfSecond.upsilon->child(0)))
            return true;
        if (recordOfFirst.block <= recordOfSecond.block)
            a = recordOfFirst.nextForPhi;
        else
            b = recordOfSecond.nextForPhi;
    }
    return false;
}

bool DirectCodeGenerator::writeClobbers(uint32_t written, uint32_t other)
{
    const ValueState& writtenState = m_states[m_homes[written].value];
    const ValueState& otherState = m_states[m_homes[other].value];
    if (m_homes[written].firstUpsilon || m_procedure.values().at(m_homes[written].value)->opcode() == B3::Phi) {
        if (isLiveIn(other, writtenState.block))
            return true;
        if (written != other && areFedDifferentValuesInOneBlock(written, other))
            return true;
        if (otherState.block == writtenState.block && written != other && (otherState.position < writtenState.position || m_procedure.values().at(m_homes[other].value)->opcode() == B3::Phi))
            return true;
        for (uint32_t record = m_homes[written].firstUpsilon; record; record = m_upsilons[record - 1].nextForPhi) {
            const UpsilonRecord& upsilon = m_upsilons[record - 1];
            bool leadsToPhi = false;
            for (unsigned index = 0; index < m_blocks[upsilon.block].block->numSuccessors(); ++index)
                leadsToPhi |= successorOrderOf(m_blocks[upsilon.block], index) == writtenState.block;
            if (!leadsToPhi)
                return true;
            if (resolve(upsilon.upsilon->child(0))->index() == m_homes[other].value && written != other) {
                if (isLiveOut(other, m_blocks[upsilon.block]))
                    return true;
                continue;
            }
            if (isLiveOut(other, m_blocks[upsilon.block]))
                return true;
            if (written != other && lastUseIn(other, upsilon.block) >= m_blocks[upsilon.block].lastPosition)
                return true;
        }
        return false;
    }
    if (otherState.position > writtenState.position)
        return false;
    if (otherState.block != writtenState.block && !isLiveIn(other, writtenState.block))
        return false;
    return isLiveBehind(other, writtenState.position, writtenState.block);
}

uint32_t DirectCodeGenerator::findWeb(uint32_t home)
{
    while (m_homes[home].parent != home) {
        m_homes[home].parent = m_homes[m_homes[home].parent].parent;
        home = m_homes[home].parent;
    }
    return home;
}

bool DirectCodeGenerator::tryCoalesce(uint32_t first, uint32_t second)
{
    AOT_THRESHOLD(uint32_t, maxWebSize, 24);
    first = findWeb(first);
    second = findWeb(second);
    if (first == second)
        return true;
    if (m_homes[first].webSize + m_homes[second].webSize > maxWebSize)
        return false;
    for (uint32_t a = first + 1; a; a = m_homes[a - 1].nextInWeb) {
        for (uint32_t b = second + 1; b; b = m_homes[b - 1].nextInWeb) {
            if (writeClobbers(a - 1, b - 1) || writeClobbers(b - 1, a - 1))
                return false;
        }
    }
    m_homes[second].parent = first;
    m_homes[m_homes[first].lastInWeb].nextInWeb = second + 1;
    m_homes[first].lastInWeb = m_homes[second].lastInWeb;
    m_homes[first].webSize += m_homes[second].webSize;
    return true;
}

void DirectCodeGenerator::coalescePhis()
{
    if (m_hasLiveness) {
        for (const UpsilonRecord& record : m_upsilons) {
            const ValueState& sourceState = stateOf(resolve(record.upsilon->child(0)));
            if (sourceState.form != ValueForm::Plain || !sourceState.homeIndex || !m_homes[sourceState.homeIndex - 1].firstUpsilon || stateOf(record.upsilon->phi()).form != ValueForm::Plain)
                continue;
            HomeState& source = m_homes[sourceState.homeIndex - 1];
            ++source.numberOfPhisFed;
            source.phiFed = record.upsilon->phi()->index() + 1;
        }
    }
    auto choosesHome = [&](HomeState& home, ValueState& state) {
        unsigned numberOfSources = 0;
        unsigned numberInHomes = 0;
        for (uint32_t record = home.firstUpsilon; record; record = m_upsilons[record - 1].nextForPhi) {
            const ValueState& sourceState = stateOf(resolve(m_upsilons[record - 1].upsilon->child(0)));
            ++numberOfSources;
            numberInHomes += sourceState.form == ValueForm::Plain && (sourceState.flags & NeedsHome);
        }
        if (home.numberOfPhisFed == 1 && state.uses == 1 && (m_states[home.phiFed - 1].flags & NeedsHome))
            ++numberInHomes;
        return 11 * numberInHomes > 8 + 4 * numberOfSources;
    };
    if (m_hasLiveness) {
        for (unsigned round = 0; round < 2; ++round) {
            for (HomeState& home : m_homes) {
                ValueState& state = m_states[home.value];
                if (home.firstUpsilon && state.form == ValueForm::Plain && !(state.flags & (NeedsHome | ArrivesInSlot)) && choosesHome(home, state))
                    state.flags |= NeedsHome;
            }
        }
    }
    for (HomeState& home : m_homes) {
        ValueState& state = m_states[home.value];
        if (!home.firstUpsilon || state.form != ValueForm::Plain)
            continue;
        if (state.flags & ArrivesInSlot)
            state.flags |= NeedsHome;
        if (!(state.flags & NeedsHome))
            continue;
        uint32_t index = static_cast<uint32_t>(&home - m_homes.span().data());
        if (!m_hasLiveness || writeClobbers(index, index))
            state.flags |= ArrivesInSlot;
        else
            state.flags &= ~ArrivesInSlot;
    }
    if (!m_hasLiveness)
        return;
    for (const UpsilonRecord& record : m_upsilons) {
        const ValueState& phiState = stateOf(record.upsilon->phi());
        if (phiState.form != ValueForm::Plain || !(phiState.flags & NeedsHome) || (phiState.flags & ArrivesInSlot))
            continue;
        const ValueState& sourceState = stateOf(resolve(record.upsilon->child(0)));
        if (sourceState.form != ValueForm::Plain || !(sourceState.flags & NeedsHome) || !sourceState.homeIndex)
            continue;
        if ((sourceState.flags & (IsDouble | IsWide | IsReadWide)) != (phiState.flags & (IsDouble | IsWide | IsReadWide)))
            continue;
        tryCoalesce(sourceState.homeIndex - 1, phiState.homeIndex - 1);
    }
}

void DirectCodeGenerator::findBlocksWithoutCode()
{
    AOT_THRESHOLD(unsigned, maxValuesLookedAt, 24);
    for (uint32_t order = 1; order < m_blocks.size(); ++order) {
        DirectBlock& block = m_blocks[order];
        if (block.block->size() > maxValuesLookedAt || block.block->last()->opcode() != B3::Jump)
            continue;
        if (!block.startsEmpty && m_blocks[block.predecessor].block->last()->opcode() == B3::Patchpoint && m_blocks[block.predecessor].block->last()->type() != B3::Void)
            continue;
        bool hasCode = false;
        for (B3::Value* value : *block.block) {
            const ValueState& state = stateOf(value);
            if (state.form != ValueForm::Plain || value->opcode() == B3::Jump)
                continue;
            if (value->opcode() == B3::Upsilon) {
                const ValueState& phiState = stateOf(value->as<B3::UpsilonValue>()->phi());
                const ValueState& sourceState = stateOf(resolve(value->child(0)));
                if (phiState.form != ValueForm::Plain)
                    continue;
                if (phiState.arrival == arrivesInHome && liesInHomeOf(sourceState, phiState))
                    continue;
            }
            hasCode = true;
            break;
        }
        uint32_t target = successorOrderOf(block, 0);
        if (!hasCode && target != order && block.forwardsTo == UINT32_MAX)
            block.forwardsTo = target;
    }
    for (DirectBlock& block : m_blocks) {
        unsigned hops = 0;
        for (uint32_t target = block.forwardsTo; target != UINT32_MAX; target = m_blocks[target].forwardsTo) {
            if (++hops > m_blocks.size()) {
                block.forwardsTo = UINT32_MAX;
                break;
            }
        }
    }
    for (DirectBlock& block : m_blocks) {
        if (!block.startsEmpty)
            continue;
        for (uint32_t target = block.forwardsTo; target != UINT32_MAX; target = m_blocks[target].forwardsTo)
            m_blocks[target].startsEmpty = true;
    }
}

void DirectCodeGenerator::layOutFrame()
{
    unsigned extent = 0;
    auto takeSlot = [&] {
        extent += sizeof(CPURegister);
        return -static_cast<int32_t>(extent);
    };

    Vector<uint32_t, 32> webs;
    for (uint32_t index = 0; index < m_homes.size(); ++index) {
        const ValueState& state = m_states[m_homes[index].value];
        if (state.form != ValueForm::Plain || !(state.flags & NeedsHome))
            continue;
        uint32_t web = findWeb(index);
        HomeState& root = m_homes[web];
        if (!root.accesses) {
            webs.append(web);
            root.start = UINT32_MAX;
        }
        root.accesses += 1 + state.uses;
        root.holdsDouble |= !!(state.flags & IsDouble);
        root.gainOfWeb += m_homes[index].gain + m_homes[index].gainInStretch;
        switch (m_procedure.values().at(m_homes[index].value)->opcode()) {
        case B3::ArgumentReg:
            root.gainOfWeb -= 2;
            break;
        case B3::Patchpoint:
        case B3::Extract:
            root.gainOfWeb += 1;
            break;
        case B3::Phi:
            break;
        default:
            root.gainOfWeb += 4;
            break;
        }
        root.start = std::min(root.start, state.position);
        root.end = std::max(root.end, std::max(state.lastUse, state.position));
        if (!(state.flags & ArrivesInSlot)) {
            for (uint32_t record = m_homes[index].firstUpsilon; record; record = m_upsilons[record - 1].nextForPhi) {
                uint32_t position = m_blocks[m_upsilons[record - 1].block].lastPosition;
                root.start = std::min(root.start, position);
                root.end = std::max(root.end, position);
            }
        }
        if (m_procedure.values().at(m_homes[index].value)->opcode() == B3::ArgumentReg) {
            root.start = 0;
            if (!(state.flags & IsDouble))
                root.holdsArgument = true;
        }
    }
    AOT::sort(webs, { }, [&](uint32_t web) {
        return m_homes[web].start;
    });

    uint64_t clobberedAtSuspensions = bitsOf(m_code.calleeSavesClobberedAtSuspensions());
    uint64_t registersForHomes = m_calleeSaves & ~m_clobberedRegisters & ~clobberedAtSuspensions & bitsOf(RegisterSet::allGPRs());
    if (registersForHomes) {
        AOT_THRESHOLD(unsigned, maxCandidates, 32);
        Vector<uint32_t, 32> candidates;
        for (uint32_t web : webs) {
            if (!m_homes[web].holdsDouble && m_homes[web].gainOfWeb > 0)
                candidates.append(web);
        }
        AOT::sort(candidates, std::ranges::greater { }, [&](uint32_t web) {
            return m_homes[web].gainOfWeb;
        });
        if (candidates.size() > maxCandidates)
            candidates.shrink(maxCandidates);
        for (uint64_t bits = registersForHomes; bits && !candidates.isEmpty(); bits &= bits - 1) {
            Reg reg = Reg::fromIndex(std::countr_zero(bits));
            Vector<uint32_t, 8> taken;
            int32_t gain = 0;
            for (uint32_t web : candidates) {
                const HomeState& root = m_homes[web];
                bool overlaps = false;
                for (uint32_t other : taken)
                    overlaps |= root.start <= m_homes[other].end && m_homes[other].start <= root.end;
                if (overlaps)
                    continue;
                taken.append(web);
                gain += root.gainOfWeb;
            }
            int32_t bytesOfPush = isX86_64() && reg.gpr() < 8 ? 1 : isX86_64() ? 2 : 4;
            if (gain <= bytesOfPush * static_cast<int32_t>(1 + m_numberOfTeardowns))
                break;
            m_homeRegisters |= bitOfRegister(reg);
            for (uint32_t web : taken) {
                m_homes[web].homeRegister = encode(reg);
                candidates.removeFirst(web);
            }
        }
    }
    uint64_t savedRegisters = (m_clobberedRegisters & m_calleeSaves & ~clobberedAtSuspensions) | m_homeRegisters;
    unsigned bytesOfSaves = 0;
    bool savesAreGeneral = true;
    if (savedRegisters) {
        RegisterSet used;
        for (uint64_t bits = savedRegisters; bits; bits &= bits - 1) {
            used.add(Reg::fromIndex(std::countr_zero(bits)), IgnoreVectors);
            savesAreGeneral &= Reg::fromIndex(std::countr_zero(bits)).isGPR();
        }
        B3::Air::handleCalleeSaves(m_code, used);
        m_needsFrame = true;
        if (B3::Air::StackSlot* slot = m_code.calleeSaveStackSlot())
            bytesOfSaves = WTF::roundUpToMultipleOf<sizeof(CPURegister)>(slot->byteSize());
    }
    unsigned bytesOfArguments = std::max(m_code.callArgAreaSizeInBytes(), m_bytesOfStackArguments);
    size_t numberOfShadows = 0;
    for (const HomeState& home : m_homes)
        numberOfShadows += m_states[home.value].form == ValueForm::Plain && (m_states[home.value].flags & ArrivesInSlot);
    size_t boundOfExtent = sizeof(CPURegister) * (webs.size() + numberOfShadows + m_valuesWithReservedHomes.size() + m_procedure.variables().size() + numberOfSpareSlots + 2) + bytesOfSaves + bytesOfArguments;
    for (B3::Air::StackSlot* slot : m_code.stackSlots())
        boundOfExtent += slot->byteSize() + stackAlignmentBytes();
    bool savesLieOnTop = bytesOfSaves && (boundOfExtent > B3::AirLimits::maxFrameSizeWithPushedRegisters || clobberedAtSuspensions);
    if (savesLieOnTop) {
        extent = bytesOfSaves;
        m_code.calleeSaveStackSlot()->setOffsetFromFP(-static_cast<intptr_t>(bytesOfSaves));
    }

    AOT_THRESHOLD(unsigned, maxSlotsSearched, 48);
    struct Slot {
        uint32_t freeAt;
        uint32_t accesses;
        uint32_t number;
        bool holdsArgument;
    };
    Vector<Slot, 32> slots;
    for (uint32_t web : webs) {
        HomeState& root = m_homes[web];
        if (root.homeRegister)
            continue;
        unsigned slot = 0;
        unsigned limit = std::min<unsigned>(slots.size(), maxSlotsSearched);
        while (slot < limit && slots[slot].freeAt > root.start)
            ++slot;
        if (slot == limit) {
            slot = slots.size();
            slots.append(Slot { 0, 0, slot, false });
        }
        slots[slot].freeAt = std::max(slots[slot].freeAt, root.end + 1);
        slots[slot].accesses += root.accesses;
        slots[slot].holdsArgument |= root.holdsArgument;
        root.slot = slot;
    }
    Vector<Slot, 32> ranked = slots;
    AOT::sort(ranked, [](const Slot& a, const Slot& b) {
        if (a.holdsArgument != b.holdsArgument)
            return a.holdsArgument;
        return a.accesses > b.accesses;
    });
    Vector<int32_t, 32> offsetOfSlot;
    offsetOfSlot.fill(0, slots.size());
    for (const Slot& slot : ranked)
        offsetOfSlot[slot.number] = takeSlot();
    for (uint32_t index = 0; index < m_homes.size(); ++index) {
        ValueState& state = m_states[m_homes[index].value];
        if (state.form != ValueForm::Plain || !(state.flags & NeedsHome))
            continue;
        const HomeState& root = m_homes[findWeb(index)];
        state.home = root.homeRegister ? static_cast<int32_t>(root.homeRegister) : offsetOfSlot[root.slot];
        if (m_homes[index].firstUpsilon || m_procedure.values().at(m_homes[index].value)->opcode() == B3::Phi)
            state.arrival = state.flags & ArrivesInSlot ? arrivesInSlot : arrivesInHome;
    }
    bool needsFrameForHomes = extent;
    unsigned bytesOfSavesAtBottom = savesLieOnTop ? 0 : bytesOfSaves;

    bool mayDoWithoutRecord = isX86_64() && m_code.policy().frame.mayOmitFrameRecord && !m_needsFrameRecord && !savedRegisters && !bytesOfArguments && !m_procedure.variables().size() && !numberOfShadows && !m_code.stackSlots().size();
    if (!mayDoWithoutRecord || m_reservesSpareSlots || extent) {
        unsigned firstHome = extent;
        Vector<uint32_t, 32> freeAt;
        for (unsigned index = m_valuesWithReservedHomes.size(); index--;) {
            ValueState& state = stateOf(m_valuesWithReservedHomes[index]);
            if (state.flags & NeedsHome)
                continue;
            unsigned slot = 0;
            unsigned limit = std::min<unsigned>(freeAt.size(), maxSlotsSearched);
            while (slot < limit && freeAt[slot] > state.position)
                ++slot;
            if (slot == limit) {
                slot = freeAt.size();
                freeAt.append(0);
            }
            freeAt[slot] = state.lastUse + 1;
            state.home = -static_cast<int32_t>(firstHome + (slot + 1) * sizeof(CPURegister));
        }
        extent += freeAt.size() * sizeof(CPURegister);
    }
    unsigned extentOfHomes = extent;

    m_variableOffsets.fill(0, m_procedure.variables().size());
    for (B3::Variable* variable : m_procedure.variables())
        m_variableOffsets[variable->index()] = takeSlot();
    for (DirectBlock& block : m_blocks) {
        for (uint32_t phi = block.firstPhi; phi; phi = m_states[phi - 1].nextPhi) {
            ValueState& state = m_states[phi - 1];
            if (state.form == ValueForm::Plain && (state.flags & ArrivesInSlot)) {
                state.shadow = takeSlot();
                state.arrival = arrivesInSlot;
            }
        }
    }
    for (B3::Air::StackSlot* slot : m_code.stackSlots()) {
        if (slot == m_code.calleeSaveStackSlot())
            continue;
        extent = WTF::roundUpToMultipleOf(slot->alignment(), extent + slot->byteSize());
        slot->setOffsetFromFP(-static_cast<intptr_t>(extent));
    }
    extent = WTF::roundUpToMultipleOf<sizeof(CPURegister)>(extent);

    if (needsFrameForHomes || extent > extentOfHomes || bytesOfArguments)
        m_needsFrame = true;
    if (m_reservesSpareSlots)
        m_needsFrame = true;
    m_hasFrame = m_needsFrame;
    if (m_reservesSpareSlots) {
        for (unsigned index = 0; index < numberOfSpareSlots; ++index)
            m_spareSlots.append(takeSlot());
    }

    bool hasOtherSlots = m_code.stackSlots().size() > (m_code.calleeSaveStackSlot() ? 1u : 0u);
    bool mayOmitRecord = isX86_64() && m_hasFrame && m_code.policy().frame.mayOmitFrameRecord && !m_needsFrameRecord && !hasOtherSlots && !bytesOfArguments;
    bool omitsRecord = mayOmitRecord && !savedRegisters && extent <= sizeof(CPURegister);
    AOT_THRESHOLD(unsigned, maxBytesWithoutRecord, 120);
    if (mayOmitRecord && !omitsRecord && !savesLieOnTop && savesAreGeneral && m_code.policy().frame.mayPushCalleeSaves && m_code.policy().frame.mayPushSpillsAtFrameSetup && !stackAdjustmentForAlignment() && extent + bytesOfSaves <= maxBytesWithoutRecord) {
        auto bytesToMoveStackPointer = [](unsigned words) {
            return !words ? 0 : words == 1 ? 1 : 4;
        };
        unsigned numberOfPushes = 0;
        for (unsigned index = 0; index < numberOfRegisters; ++index) {
            const B3::Value* argument = m_argumentValues[index];
            if (argument && Reg::fromIndex(index).isGPR() && stateOf(argument).form == ValueForm::Plain && (stateOf(argument).flags & NeedsHome) && stateOf(argument).home < 0)
                ++numberOfPushes;
        }
        int accesses = static_cast<int>(m_valuesWithReservedHomes.size() + 3 * numberOfShadows + 2 * m_procedure.variables().size()) - static_cast<int>(numberOfPushes);
        for (const Slot& slot : slots)
            accesses += slot.accesses;
        unsigned words = (extent + bytesOfSaves) / sizeof(CPURegister);
        unsigned wordsOfSlots = extent / sizeof(CPURegister);
        unsigned wordsWithRecord = wordsOfSlots + words % 2;
        unsigned wordsWithoutRecord = wordsOfSlots + !(words % 2);
        constexpr int bytesOfRecordSetup = 4;
        constexpr int bytesOfRecordTeardown = 1;
        int gain = bytesOfRecordSetup + bytesToMoveStackPointer(wordsWithRecord - std::min(numberOfPushes, wordsWithRecord)) - bytesToMoveStackPointer(wordsWithoutRecord - std::min(numberOfPushes, wordsWithoutRecord));
        gain += static_cast<int>(m_numberOfTeardowns) * (bytesOfRecordTeardown - bytesToMoveStackPointer(wordsWithoutRecord));
        omitsRecord = gain > accesses;
    }
    m_hasFrameRecord = !omitsRecord;
    if (!m_hasFrameRecord)
        m_homeBias = sizeof(CPURegister);
    unsigned bytesAboveFramePointer = static_cast<unsigned>(m_homeBias);
    unsigned bytesBelowSlots = bytesOfSavesAtBottom + bytesOfArguments;
    unsigned frameSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(std::max(extent + bytesBelowSlots, bytesAboveFramePointer) - bytesAboveFramePointer) + stackAdjustmentForAlignment();
    while (m_hasFrame && extent + bytesBelowSlots + sizeof(CPURegister) <= frameSize + bytesAboveFramePointer - stackAdjustmentForAlignment())
        m_spareSlots.append(takeSlot());
    if (bytesOfSavesAtBottom)
        m_code.calleeSaveStackSlot()->setOffsetFromFP(m_homeBias - static_cast<intptr_t>(extent + bytesOfSaves));
    if (m_hasFrame)
        m_code.setFrameSize(frameSize);

    B3::Air::FrameLayout layout;
    layout.isPlanned = true;
    layout.hasFrame = m_hasFrame;
    layout.hasFrameRecord = m_hasFrameRecord;
    layout.coverage = m_hasFrame ? B3::Air::FrameCoverage::EveryPathNeedsFrame : B3::Air::FrameCoverage::NoPath;
    layout.slotArea = B3::Air::SlotArea::BelowFrameRecord;
    layout.calleeSaveForm = B3::Air::CalleeSaveForm::Stored;
    layout.numberOfFrameSetups = m_hasFrame;
#if CPU(X86_64)
    if (m_hasFrame && frameSize <= B3::AirLimits::maxFrameSizeWithPushedRegisters) {
        if (bytesOfSavesAtBottom && savesAreGeneral && !bytesOfArguments && !stackAdjustmentForAlignment() && m_code.policy().frame.mayPushCalleeSaves)
            layout.calleeSaveForm = B3::Air::CalleeSaveForm::PushedAtFrameBottom;
        else if (savesLieOnTop && savesAreGeneral && m_code.policy().frame.mayPushCalleeSaves)
            layout.calleeSaveForm = B3::Air::CalleeSaveForm::PushedBelowFrameRecord;
        for (unsigned index = 0; index < numberOfRegisters; ++index) {
            const B3::Value* argument = m_argumentValues[index];
            if (!argument || !Reg::fromIndex(index).isGPR() || !m_code.policy().frame.mayPushSpillsAtFrameSetup)
                continue;
            const ValueState& state = stateOf(argument);
            if (state.form == ValueForm::Plain && (state.flags & NeedsHome) && state.home < 0)
                m_pushedArguments.append({ Reg::fromIndex(index), static_cast<unsigned>(-state.home) / static_cast<unsigned>(sizeof(CPURegister)) - 1 });
        }
        layout.numberOfSpillsPushedAtFrameSetup = m_pushedArguments.size();
    }
#endif
    m_code.setFrameLayout(WTF::move(layout));
}

bool DirectCodeGenerator::plan()
{
    if (m_procedure.numEntrypoints() != 1)
        return refuse();
    for (B3::Variable* variable : m_procedure.variables()) {
        if (variable->type() != B3::Int32 && variable->type() != B3::Int64 && variable->type() != B3::Double)
            return refuse();
    }
    setUpRegisters();
    if (!orderBlocks())
        return false;
    if (!numberValues())
        return false;
    findLoops();
    noteUses();
    if (m_isRefused)
        return false;
    computeLiveness();
    if (m_isRefused)
        return false;
    coalescePhis();
    return true;
}

int32_t DirectCodeGenerator::takeLateHome()
{
    if (m_spareSlots.isEmpty()) {
        failForLackOfSlot();
        return -static_cast<int32_t>(sizeof(CPURegister));
    }
    return m_spareSlots.takeLast();
}

void DirectCodeGenerator::storeToFrame(Reg reg, const ValueState& state, int32_t offset)
{
    if (!m_context.frameIsSetUp) {
        failForLackOfSlot();
        return;
    }
    if (offset > 0) {
        moveRegister(reg, decode(static_cast<uint8_t>(offset)));
        return;
    }
    if (state.flags & IsDouble)
        m_jit->storeDouble(reg.fpr(), addressOfHome(offset));
    else if (state.flags & (IsWide | IsReadWide))
        m_jit->store64(reg.gpr(), addressOfHome(offset));
    else
        m_jit->store32(reg.gpr(), addressOfHome(offset));
    finishUnit(kindOfFrameAccess(offset));
}

void DirectCodeGenerator::loadFromFrame(Reg reg, const ValueState& state, int32_t offset)
{
    if (!m_context.frameIsSetUp) {
        failForLackOfSlot();
        return;
    }
    if (offset > 0) {
        moveRegister(decode(static_cast<uint8_t>(offset)), reg);
        return;
    }
    if (state.flags & IsDouble)
        m_jit->loadDouble(addressOfHome(offset), reg.fpr());
    else if (state.flags & IsWide)
        m_jit->load64(addressOfHome(offset), reg.gpr());
    else
        m_jit->load32(addressOfHome(offset), reg.gpr());
    finishUnit(kindOfFrameAccess(offset));
}

void DirectCodeGenerator::writeBack(Reg reg)
{
    uint32_t id = holderOf(reg) - 1;
    ValueState& state = m_states[id];
    if (!m_context.frameIsSetUp) {
        failForLackOfSlot();
        return;
    }
    if (!state.home)
        state.home = takeLateHome();
    storeToFrame(reg, state, state.home);
    m_file.dirty &= ~bitOfRegister(reg);
}

void DirectCodeGenerator::moveRegister(Reg source, Reg destination)
{
    if (source == destination)
        return;
    if (source.isGPR())
        m_jit->move(source.gpr(), destination.gpr());
    else
        m_jit->moveDouble(source.fpr(), destination.fpr());
    finishUnit();
}

Reg DirectCodeGenerator::findFree(uint64_t candidates)
{
    candidates &= ~m_locked;
    for (uint64_t bits = candidates; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        if (!(m_file.occupied & bitOfRegister(reg)))
            return reg;
        if (!isNeededNow(holderOf(reg) - 1)) {
            release(reg);
            return reg;
        }
    }
    return Reg();
}

Reg DirectCodeGenerator::allocate(uint64_t candidates, Reg preferred)
{
    bool wantsGeneralRegister = m_generalRegisters & candidates;
    candidates &= ~m_locked;
    if (preferred && (candidates & bitOfRegister(preferred))) {
        uint32_t holder = holderOf(preferred);
        if (!holder || !isNeededNow(holder - 1)) {
            release(preferred);
            return preferred;
        }
    }
    if (Reg free = findFree(candidates))
        return free;

    Reg victim;
    uint32_t farthest = 0;
    bool victimIsDirty = true;
    for (uint64_t bits = candidates; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        bool isDirty = m_file.dirty & bitOfRegister(reg);
        uint32_t lastUse = m_states[holderOf(reg) - 1].lastUse;
        if (!victim || (victimIsDirty && !isDirty) || (victimIsDirty == isDirty && lastUse > farthest)) {
            victim = reg;
            farthest = lastUse;
            victimIsDirty = isDirty;
        }
    }
    if (!victim) {
        fail();
        return Reg::fromIndex(std::countr_zero(wantsGeneralRegister ? m_generalRegisters : m_doubleRegisters));
    }
    if (victimIsDirty)
        writeBack(victim);
    release(victim);
    return victim;
}

void DirectCodeGenerator::materialize(const B3::Value* value, Reg destination)
{
    const ValueState& state = stateOf(value);
    switch (state.form) {
    case ValueForm::Constant:
        if (state.flags & IsDouble) {
            if (!state.constant)
                m_jit->moveZeroToDouble(destination.fpr());
            else if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(state.constant); pinned && pinned->offset)
                m_jit->loadDouble(Address(pinned->reg.gpr(), *pinned->offset), destination.fpr());
            else {
                m_jit->move(TrustedImm64(state.constant), m_jit->scratchRegister());
                m_jit->move64ToDouble(m_jit->scratchRegister(), destination.fpr());
            }
        } else if (!(state.flags & IsWide))
            m_jit->move(TrustedImm32(static_cast<int32_t>(state.constant)), destination.gpr());
        else if (std::optional<B3::PinnedConstant> pinned = fitsInt32(state.constant) || isRepresentableAs<uint32_t>(state.constant) ? std::nullopt : pinnedConstantFor(state.constant)) {
            if (pinned->offset)
                m_jit->load64(Address(pinned->reg.gpr(), *pinned->offset), destination.gpr());
            else
                m_jit->move(pinned->reg.gpr(), destination.gpr());
        } else
            m_jit->move(TrustedImm64(state.constant), destination.gpr());
        finishUnit();
        return;
    case ValueForm::FixedRegister:
        moveRegister(decode(state.base), destination);
        return;
    case ValueForm::Rematerialized:
        switch (value->opcode()) {
        case B3::SlotBase:
            m_jit->addPtr(TrustedImm32(static_cast<int32_t>(value->as<B3::SlotBaseValue>()->slot()->offsetFromFP())), MacroAssembler::framePointerRegister, destination.gpr());
            finishUnit();
            return;
        case B3::Add:
            m_jit->addPtr(TrustedImm32(static_cast<int32_t>(state.constant)), decode(state.base).gpr(), destination.gpr());
            finishUnit();
            return;
        case B3::Relocatable: {
            const B3::RelocatableValue* relocatable = value->as<B3::RelocatableValue>();
            uint32_t index = relocatable->symbol().index;
            switch (static_cast<RelocationSpace>(relocatable->symbol().space)) {
            case RelocationSpace::SiteCaches:
                m_records->relocations.moveInstanceAddress({ SymbolSpace::SiteCache, index }, instanceGPR, destination.gpr());
                break;
            case RelocationSpace::Constants:
                m_records->relocations.moveAddress({ SymbolSpace::Constant, index }, destination.gpr(), PCRelativeReach::Far);
                break;
            case RelocationSpace::JumpTables:
                m_records->relocations.moveAddress({ SymbolSpace::JumpTable, index }, destination.gpr(), PCRelativeReach::Far);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            finishUnit(CodeUnit::Kind::Pinned);
            if (relocatable->addend()) {
                m_jit->add64(TrustedImm64(relocatable->addend()), destination.gpr());
                finishUnit();
            }
            return;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    case ValueForm::Plain:
        if (!state.home || ((state.flags & WasLetGoUnsaved) && state.lastUse >= m_position)) {
            fail();
            return;
        }
        loadFromFrame(destination, state, state.home);
        return;
    case ValueForm::Dead:
    case ValueForm::Alias:
    case ValueForm::Folded:
        fail();
        return;
    }
}

Reg DirectCodeGenerator::registerFor(const B3::Value* value, uint64_t avoid)
{
    value = resolve(value);
    const ValueState& state = stateOf(value);
    uint64_t bank = state.flags & IsDouble ? m_doubleRegisters : m_generalRegisters;
    Reg reg = locate(value);
    if (reg && !(bitOfRegister(reg) & avoid)) {
        lock(reg);
        return reg;
    }
    Reg destination = allocate(bank & ~avoid, state.form == ValueForm::Plain ? hintFor(state) : Reg());
    if (reg && state.form != ValueForm::FixedRegister) {
        bool isDirty = m_file.dirty & bitOfRegister(reg);
        moveRegister(reg, destination);
        release(reg);
        bind(destination, value->index(), isDirty);
    } else {
        materialize(value, destination);
        bind(destination, value->index(), false);
    }
    lock(destination);
    return destination;
}

Reg DirectCodeGenerator::hintFor(const ValueState& state) const
{
    if (state.hint)
        return decode(state.hint);
    if (state.phi) {
        const ValueState& phi = m_states[state.phi - 1];
        if (phi.arrival && phi.arrival < arrivesInHome)
            return decode(phi.arrival);
        if (phi.hint)
            return decode(phi.hint);
    }
    return Reg();
}

Reg DirectCodeGenerator::define(const B3::Value* value, Reg reusable, Reg otherReusable, uint64_t avoid)
{
    const ValueState& state = stateOf(value);
    uint64_t bank = (state.flags & IsDouble ? m_doubleRegisters : m_generalRegisters) & ~avoid;
    auto dies = [&](Reg reg) {
        if (!reg || !(bank & bitOfRegister(reg)))
            return false;
        uint32_t holder = holderOf(reg);
        return !holder || !isLiveAfter(holder - 1);
    };
    if (state.home > 0) {
        Reg home = decode(static_cast<uint8_t>(state.home));
        if (!((m_locked | avoid) & bitOfRegister(home)) || (!(avoid & bitOfRegister(home)) && (home == reusable || home == otherReusable))) {
            lock(home);
            return home;
        }
    }
    Reg hint = hintFor(state);
    Reg result;
    if (hint && (bank & bitOfRegister(hint))) {
        if ((hint == reusable || hint == otherReusable) && dies(hint))
            result = hint;
        else if (!(m_locked & bitOfRegister(hint)) && (!holderOf(hint) || !isNeededNow(holderOf(hint) - 1)))
            result = hint;
        else if (!(m_locked & bitOfRegister(hint))) {
            if (!(m_file.dirty & bitOfRegister(hint)))
                result = hint;
            else {
                uint32_t holder = holderOf(hint) - 1;
                Reg other = hintFor(m_states[holder]);
                if (other && other != hint && (bank & bitOfRegister(other)) && !(m_locked & bitOfRegister(other)) && (!holderOf(other) || !isNeededNow(holderOf(other) - 1))) {
                    release(other);
                    moveRegister(hint, other);
                    release(hint);
                    bind(other, holder, true);
                    result = hint;
                }
            }
        }
    }
    if (!result && dies(reusable))
        result = reusable;
    if (!result && dies(otherReusable))
        result = otherReusable;
    if (!result)
        result = allocate(bank);
    release(result);
    lock(result);
    return result;
}

void DirectCodeGenerator::finishDefinition(const B3::Value* value, Reg reg)
{
    ValueState& state = stateOf(value);
    if (state.home > 0 && encode(reg) == state.home)
        return;
    bind(reg, value->index(), true);
    if (state.flags & NeedsHome)
        writeBack(reg);
}

void DirectCodeGenerator::preserveValuesIn(uint64_t registers, uint64_t avoid, bool alsoValuesNeededNow)
{
    m_registersWithValuesLeftBehind = 0;
    for (uint64_t bits = registers & m_file.dirty; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        uint32_t id = holderOf(reg) - 1;
        if (!(alsoValuesNeededNow ? isNeededNow(id) : isLiveAfter(id)))
            continue;
        uint64_t bank = reg.isGPR() ? m_generalRegisters : m_doubleRegisters;
        if (Reg free = m_states[id].flags & NeedsHome ? Reg() : findFree(bank & ~avoid)) {
            moveRegister(reg, free);
            release(reg);
            bind(free, id, true);
            m_valuesLeftBehind[reg.index()] = id + 1;
            m_registersWithValuesLeftBehind |= bitOfRegister(reg);
            continue;
        }
        writeBack(reg);
    }
}

void DirectCodeGenerator::flushDirtyValues()
{
    for (uint64_t bits = m_file.dirty; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        uint32_t id = holderOf(reg) - 1;
        if ((m_states[id].flags & NeedsHome) && isLiveAfter(id))
            writeBack(reg);
    }
}

void DirectCodeGenerator::moveIntoRegisters(Vector<Transfer, 8>& transfers)
{
    for (Transfer& transfer : transfers) {
        transfer.value = resolve(transfer.value);
        transfer.source = locate(transfer.value);
        transfer.isDone = transfer.source == transfer.destination;
        if ((m_registersWithValuesLeftBehind & bitOfRegister(transfer.destination)) && m_valuesLeftBehind[transfer.destination.index()] == transfer.value->index() + 1)
            transfer.isDone = true;
    }
    m_registersWithValuesLeftBehind = 0;
    for (;;) {
        bool madeProgress = false;
        bool hasPendingMove = false;
        for (Transfer& transfer : transfers) {
            if (transfer.isDone)
                continue;
            bool destinationIsRead = false;
            for (const Transfer& other : transfers)
                destinationIsRead |= !other.isDone && &other != &transfer && other.source == transfer.destination;
            if (destinationIsRead) {
                hasPendingMove = true;
                continue;
            }
            if (transfer.source)
                moveRegister(transfer.source, transfer.destination);
            else
                materialize(transfer.value, transfer.destination);
            transfer.isDone = true;
            madeProgress = true;
        }
        if (!hasPendingMove)
            break;
        if (madeProgress)
            continue;
        for (Transfer& transfer : transfers) {
            if (transfer.isDone || !transfer.source)
                continue;
            Reg source = transfer.source;
            Reg destination = transfer.destination;
            if (source.isGPR())
                m_jit->swap(source.gpr(), destination.gpr());
            else
                m_jit->swapDouble(source.fpr(), destination.fpr());
            finishUnit();
            for (Transfer& other : transfers) {
                if (other.isDone)
                    continue;
                if (other.source == destination)
                    other.source = source;
                else if (other.source == source)
                    other.source = destination;
                other.isDone = other.source == other.destination;
            }
            break;
        }
    }
    for (Transfer& transfer : transfers) {
        if (transfer.isDone)
            continue;
        materialize(transfer.value, transfer.destination);
        transfer.isDone = true;
    }
}

MemoryOperand DirectCodeGenerator::operandFor(const AddressPlan& plan)
{
    MemoryOperand operand;
    operand.offset = plan.offset;
    if (plan.slot)
        operand.offset += static_cast<int32_t>(plan.slot->offsetFromFP());
    operand.base = plan.base ? gprFor(plan.base) : plan.fixedBase;
    if (plan.index) {
        operand.index = gprFor(plan.index);
        operand.shift = plan.shift;
    }
    return operand;
}

void DirectCodeGenerator::recordOrigin(const B3::Value* value)
{
    if (m_procedure.needsPCToOriginMap()) [[unlikely]]
        m_procedure.pcToOriginMap().appendItem(m_jit->labelIgnoringWatchpoints(), value->origin());
}

void DirectCodeGenerator::noteNewLatePaths()
{
    for (; m_numberOfLatePathsNoted < m_context.latePaths.size(); ++m_numberOfLatePathsNoted) {
        if (!m_context.frameIsSetUp)
            m_context.latePathsWithoutFrame.set(m_numberOfLatePathsNoted);
    }
}

void DirectCodeGenerator::emitIntegerBinary(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    B3::Opcode opcode = value->opcode();
    bool isWide = value->type() == B3::Int64;
    OperandPlan plan = planBinary(value);

    if (plan.shape == OperandShape::Load || plan.shape == OperandShape::PinnedMemory) {
#if CPU(X86_64)
        MemoryOperand operand;
        if (plan.shape == OperandShape::Load)
            operand = operandForLoad(plan.load);
        else {
            operand.base = plan.pinned.reg.gpr();
            operand.offset = *plan.pinned.offset;
        }
        GPRReg left = gprFor(plan.left);
        GPRReg result = defineGPR(value, left);
        moveRegister(Reg(left), Reg(result));
        if (plan.shape == OperandShape::Load)
            checkASANShadow(operand, isWide ? 8 : 4, false);
        withAddress(operand, [&](auto address) {
            switch (opcode) {
            case B3::Add:
                isWide ? jit.add64(address, result) : jit.add32(address, result);
                break;
            case B3::Sub:
                isWide ? jit.sub64(address, result) : jit.sub32(address, result);
                break;
            case B3::BitAnd:
                isWide ? jit.and64(address, result) : jit.and32(address, result);
                break;
            case B3::BitOr:
                isWide ? jit.or64(address, result) : jit.or32(address, result);
                break;
            case B3::BitXor:
                isWide ? jit.xor64(address, result) : jit.xor32(address, result);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        });
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }

    GPRReg left = gprFor(plan.left);
    if (plan.shape == OperandShape::Immediate) {
        TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
        GPRReg result = defineGPR(value, left);
        switch (opcode) {
        case B3::Add:
            isWide ? jit.add64(immediate, left, result) : jit.add32(immediate, left, result);
            break;
        case B3::Sub:
            isWide ? jit.sub64(left, immediate, result) : jit.sub32(left, immediate, result);
            break;
        case B3::Mul:
            if (plan.immediate > 1 && !(plan.immediate & (plan.immediate - 1))) {
                TrustedImm32 amount(std::countr_zero(static_cast<uint64_t>(plan.immediate)));
                isWide ? jit.lshift64(left, amount, result) : jit.lshift32(left, amount, result);
                break;
            }
            if (!isWide) {
                jit.mul32(immediate, left, result);
                break;
            }
#if CPU(X86_64)
            jit.mul64(immediate, left, result);
#else
            jit.move(TrustedImm64(static_cast<int64_t>(plan.immediate)), jit.scratchRegister());
            jit.mul64(left, jit.scratchRegister(), result);
#endif
            break;
        case B3::BitAnd:
            isWide ? jit.and64(immediate, left, result) : jit.and32(immediate, left, result);
            break;
        case B3::BitOr:
            isWide ? jit.or64(immediate, left, result) : jit.or32(immediate, left, result);
            break;
        case B3::BitXor:
            isWide ? jit.xor64(immediate, left, result) : jit.xor32(immediate, left, result);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }

    GPRReg right = plan.shape == OperandShape::PinnedRegister ? plan.pinned.reg.gpr() : gprFor(plan.right);
    GPRReg result = defineGPR(value, left, isCommutative(opcode) ? right : InvalidGPRReg);
    if (plan.shape == OperandShape::ScaledIndex) {
        jit.getEffectiveAddress(MacroAssembler::BaseIndex(left, right, static_cast<MacroAssembler::Scale>(plan.shift)), result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    switch (opcode) {
    case B3::Add:
        isWide ? jit.add64(left, right, result) : jit.add32(left, right, result);
        break;
    case B3::Sub:
        isWide ? jit.sub64(left, right, result) : jit.sub32(left, right, result);
        break;
    case B3::Mul:
        isWide ? jit.mul64(left, right, result) : jit.mul32(left, right, result);
        break;
    case B3::BitAnd:
        isWide ? jit.and64(left, right, result) : jit.and32(left, right, result);
        break;
    case B3::BitOr:
        isWide ? jit.or64(left, right, result) : jit.or32(left, right, result);
        break;
    case B3::BitXor:
        isWide ? jit.xor64(left, right, result) : jit.xor32(left, right, result);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    finishUnit();
    finishDefinition(value, Reg(result));
}

void DirectCodeGenerator::emitShift(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    B3::Opcode opcode = value->opcode();
    bool isWide = value->type() == B3::Int64;
    GPRReg left = gprFor(value->child(0));
    int64_t constant;
    if (constantOf(value->child(1), constant)) {
        TrustedImm32 amount(static_cast<int32_t>(constant & (isWide ? 63 : 31)));
        GPRReg result = defineGPR(value, left);
        switch (opcode) {
        case B3::Shl:
            isWide ? jit.lshift64(left, amount, result) : jit.lshift32(left, amount, result);
            break;
        case B3::SShr:
            isWide ? jit.rshift64(left, amount, result) : jit.rshift32(left, amount, result);
            break;
        case B3::ZShr:
            isWide ? jit.urshift64(left, amount, result) : jit.urshift32(left, amount, result);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    GPRReg amount = gprFor(value->child(1));
#if CPU(X86_64)
    Reg counter(X86Registers::ecx);
    if (amount != counter.gpr() && left != counter.gpr() && !(m_locked & bitOfRegister(counter)) && (!holderOf(counter) || !isLiveAfter(holderOf(counter) - 1) || !(m_file.dirty & bitOfRegister(counter)))) {
        release(counter);
        moveRegister(Reg(amount), counter);
        lock(counter);
        amount = counter.gpr();
    }
#endif
    GPRReg result = define(value, Reg(), Reg(), bitOfRegister(Reg(left)) | bitOfRegister(Reg(amount))).gpr();
    switch (opcode) {
    case B3::Shl:
        isWide ? jit.lshift64(left, amount, result) : jit.lshift32(left, amount, result);
        break;
    case B3::SShr:
        isWide ? jit.rshift64(left, amount, result) : jit.rshift32(left, amount, result);
        break;
    case B3::ZShr:
        isWide ? jit.urshift64(left, amount, result) : jit.urshift32(left, amount, result);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    finishUnit();
    finishDefinition(value, Reg(result));
}

void DirectCodeGenerator::emitLoad(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    MemoryOperand operand = operandForLoad(value);
    bool isDouble = value->type() == B3::Double;
    Reg result = isDouble ? define(value) : define(value, Reg(operand.base), operand.hasIndex() ? Reg(operand.index) : Reg());
    GPRReg reference = operand.hasIndex() ? operand.index : operand.base;
    bool referenceIsNarrow = operand.hasIndex();
#if CPU(ARM64)
    auto isOneInstruction = [&] {
        if (operand.hasIndex())
            return false;
        if (ARM64Assembler::canEncodeSImmOffset(operand.offset))
            return true;
        switch (value->opcode()) {
        case B3::Load8Z:
        case B3::Load8S:
            return ARM64Assembler::canEncodePImmOffset<8>(operand.offset);
        case B3::Load16Z:
        case B3::Load16S:
            return ARM64Assembler::canEncodePImmOffset<16>(operand.offset);
        default:
            return value->type() == B3::Int32 ? ARM64Assembler::canEncodePImmOffset<32>(operand.offset) : ARM64Assembler::canEncodePImmOffset<64>(operand.offset);
        }
    };
    if (value->traps() && !isOneInstruction()) {
        if (operand.hasIndex() && !isUInt12(operand.offset)) {
            fail();
            return;
        }
        GPRReg address = jit.scratchRegister();
        if (operand.hasIndex())
            jit.getEffectiveAddress(operand.baseIndex(), address);
        else
            jit.add64(TrustedImm32(operand.offset), operand.base, address);
        finishUnit();
        operand = MemoryOperand { address };
    }
#endif
    checkASANShadow(operand, value);
    Label begin = jit.labelIgnoringWatchpoints();
    withAddress(operand, [&](auto address) {
        switch (value->opcode()) {
        case B3::Load8Z:
            jit.load8(address, result.gpr());
            break;
        case B3::Load8S:
            jit.load8SignedExtendTo32(address, result.gpr());
            break;
        case B3::Load16Z:
            jit.load16(address, result.gpr());
            break;
        case B3::Load16S:
            jit.load16SignedExtendTo32(address, result.gpr());
            break;
        case B3::Load:
            if (isDouble)
                jit.loadDouble(address, result.fpr());
            else if (value->type() == B3::Int64)
                jit.load64(address, result.gpr());
            else
                jit.load32(address, result.gpr());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    });
    if (value->traps()) {
        m_records->traps.trappingAccesses.append(TrappingAccess { begin, valueOfOrigin(value->origin()), reference, referenceIsNarrow });
        m_records->units.didGenerateTrappingAccess(begin, value->origin());
        finishUnit(CodeUnit::Kind::Pinned);
    } else
        finishUnit();
    finishDefinition(value, result);
}

void DirectCodeGenerator::emitStore(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    const B3::Value* stored = value->child(0);
    B3::Opcode opcode = value->opcode();
    const B3::MemoryValue* memory = value->as<B3::MemoryValue>();
    MemoryOperand operand = operandFor(planAddress(memory->lastChild(), memory->offset()));
    bool isStoredFromRegister = !storesImmediate(stored, opcode);
    if (!isStoredFromRegister && isX86_64() && opcode == B3::Store) {
        const B3::Value* constantValue = resolve(stored);
        int64_t constant = 0;
        constantOf(stored, constant);
        if (locate(constantValue))
            isStoredFromRegister = true;
        else if (!constant || stateOf(constantValue).lastUse > m_position) {
            if (Reg free = findFree(m_generalRegisters)) {
                materialize(constantValue, free);
                bind(free, constantValue->index(), false);
                isStoredFromRegister = true;
            }
        }
    }
    if (!isStoredFromRegister) {
        int64_t constant = 0;
        constantOf(stored, constant);
        TrustedImm32 immediate(static_cast<int32_t>(constant));
        checkASANShadow(operand, value);
        withAddress(operand, [&](auto address) {
            switch (opcode) {
            case B3::Store8:
                jit.store8(immediate, address);
                break;
            case B3::Store16:
                jit.store16(immediate, address);
                break;
            default:
                if (stored->type() == B3::Int64)
                    jit.store64(immediate, address);
                else
                    jit.store32(immediate, address);
                break;
            }
        });
        finishUnit();
        return;
    }
    Reg source = registerFor(stored);
    checkASANShadow(operand, value);
    withAddress(operand, [&](auto address) {
        switch (opcode) {
        case B3::Store8:
            jit.store8(source.gpr(), address);
            break;
        case B3::Store16:
            jit.store16(source.gpr(), address);
            break;
        default:
            if (stored->type() == B3::Double)
                jit.storeDouble(source.fpr(), address);
            else if (stored->type() == B3::Int64)
                jit.store64(source.gpr(), address);
            else
                jit.store32(source.gpr(), address);
            break;
        }
    });
    finishUnit();
}

void DirectCodeGenerator::emitComparison(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    const B3::Value* left = value->child(0);
    const B3::Value* right = value->child(1);
    if (left->type() == B3::Double) {
        FPRReg leftRegister = fprFor(left);
        FPRReg rightRegister = fprFor(right);
        GPRReg result = defineGPR(value);
        jit.compareDouble(doubleConditionOf(value->opcode()), leftRegister, rightRegister, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    bool isWide = left->type() == B3::Int64;
    OperandPlan plan = planComparison(value->opcode(), left, right, false);
    MacroAssembler::RelationalCondition condition = relationalConditionOf(value->opcode());
    if (plan.isCommuted)
        condition = MacroAssembler::commute(condition);
    GPRReg leftRegister = gprFor(plan.left);
    if (plan.shape == OperandShape::Immediate) {
        GPRReg result = defineGPR(value, leftRegister);
        TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
        isWide ? jit.compare64(condition, leftRegister, immediate, result) : jit.compare32(condition, leftRegister, immediate, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    GPRReg rightRegister;
    if (plan.shape == OperandShape::PinnedRegister)
        rightRegister = plan.pinned.reg.gpr();
    else if (plan.shape == OperandShape::PinnedMemory) {
        rightRegister = jit.scratchRegister();
        jit.load64(Address(plan.pinned.reg.gpr(), *plan.pinned.offset), rightRegister);
    } else
        rightRegister = gprFor(plan.right);
    GPRReg result = defineGPR(value, leftRegister);
    isWide ? jit.compare64(condition, leftRegister, rightRegister, result) : jit.compare32(condition, leftRegister, rightRegister, result);
    finishUnit();
    finishDefinition(value, Reg(result));
}

MacroAssembler::Jump DirectCodeGenerator::branchOnTest(const B3::Value* left, const B3::Value* right, bool whenNonZero)
{
    CCallHelpers& jit = *m_jit;
    MacroAssembler::ResultCondition condition = whenNonZero ? MacroAssembler::NonZero : MacroAssembler::Zero;
    bool isWide = left->type() == B3::Int64;
    OperandPlan plan = planTest(left, right, true);
    if (plan.load) {
#if CPU(X86_64)
        MemoryOperand operand = operandForLoad(plan.load);
        TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
        checkASANShadow(operand, plan.loadWidth / 8, false);
        return withAddress(operand, [&](auto address) {
            switch (plan.loadWidth) {
            case 8:
                return jit.branchTest8(condition, address, mask);
            case 16:
                return jit.branchTest16(condition, address, mask);
            case 32:
                return jit.branchTest32(condition, address, mask);
            default:
                return jit.branchTest64(condition, address, mask);
            }
        });
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }
    GPRReg leftRegister = gprFor(plan.left);
    switch (plan.shape) {
    case OperandShape::Immediate: {
        TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
        return isWide ? jit.branchTest64(condition, leftRegister, mask) : jit.branchTest32(condition, leftRegister, mask);
    }
    case OperandShape::BitTest:
#if CPU(X86_64)
        return isWide ? jit.branchTestBit64(condition, leftRegister, gprFor(plan.right)) : jit.branchTestBit32(condition, leftRegister, gprFor(plan.right));
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    case OperandShape::PinnedRegister:
        return jit.branchTest64(condition, leftRegister, plan.pinned.reg.gpr());
    case OperandShape::PinnedMemory:
#if CPU(X86_64)
        return jit.branchTest64(condition, leftRegister, Address(plan.pinned.reg.gpr(), *plan.pinned.offset));
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    case OperandShape::Register:
    case OperandShape::Load:
    case OperandShape::ScaledIndex:
        break;
    }
    GPRReg rightRegister = gprFor(plan.right);
    return isWide ? jit.branchTest64(condition, leftRegister, rightRegister) : jit.branchTest32(condition, leftRegister, rightRegister);
}

MacroAssembler::Jump DirectCodeGenerator::branchOnComparison(const B3::Value* comparison, bool whenTrue)
{
    CCallHelpers& jit = *m_jit;
    const B3::Value* left = comparison->child(0);
    const B3::Value* right = comparison->child(1);
    B3::Opcode opcode = comparison->opcode();
    if (left->type() == B3::Double) {
        FPRReg leftRegister = fprFor(left);
        FPRReg rightRegister = fprFor(right);
        MacroAssembler::DoubleCondition condition = doubleConditionOf(opcode);
        return jit.branchDouble(whenTrue ? condition : MacroAssembler::invert(condition), leftRegister, rightRegister);
    }
    if ((opcode == B3::Equal || opcode == B3::NotEqual) && isZero(right) && stateOf(left).form == ValueForm::Folded && !B3::isLoad(left->opcode()))
        return branchOn(left, opcode == B3::NotEqual ? whenTrue : !whenTrue);

    bool isWide = left->type() == B3::Int64;
    OperandPlan plan = planComparison(opcode, left, right, true);
    MacroAssembler::RelationalCondition condition = relationalConditionOf(opcode);
    if (plan.isCommuted)
        condition = MacroAssembler::commute(condition);
    if (!whenTrue)
        condition = MacroAssembler::invert(condition);

    if (plan.load) {
#if CPU(X86_64)
        MemoryOperand operand = operandForLoad(plan.load);
        if (plan.shape == OperandShape::Immediate) {
            TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
            checkASANShadow(operand, plan.loadWidth / 8, false);
            if (operand.hasIndex() && plan.loadWidth == 64) {
                GPRReg scratch = jit.scratchRegister();
                jit.load64(operand.baseIndex(), scratch);
                return jit.branch64(condition, scratch, immediate);
            }
            switch (plan.loadWidth) {
            case 8:
                return withAddress(operand, [&](auto address) { return jit.branch8(condition, address, immediate); });
            case 16:
                return withAddress(operand, [&](auto address) { return jit.branch16(condition, address, immediate); });
            case 32:
                return withAddress(operand, [&](auto address) { return jit.branch32(condition, address, immediate); });
            default:
                return jit.branch64(condition, operand.address(), immediate);
            }
        }
        GPRReg other = gprFor(plan.loadIsLeft ? plan.right : plan.left);
        if (!plan.loadIsLeft)
            condition = MacroAssembler::commute(condition);
        checkASANShadow(operand, isWide ? 8 : 4, false);
        return withAddress(operand, [&](auto address) {
            return isWide ? jit.branch64(condition, address, other) : jit.branch32(condition, address, other);
        });
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }

    GPRReg leftRegister = gprFor(plan.left);
    switch (plan.shape) {
    case OperandShape::Immediate: {
        TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
        return isWide ? jit.branch64(condition, leftRegister, immediate) : jit.branch32(condition, leftRegister, immediate);
    }
    case OperandShape::PinnedRegister:
        return jit.branch64(condition, leftRegister, plan.pinned.reg.gpr());
    case OperandShape::PinnedMemory:
        return jit.branch64(condition, leftRegister, Address(plan.pinned.reg.gpr(), *plan.pinned.offset));
    case OperandShape::Register:
    case OperandShape::Load:
    case OperandShape::ScaledIndex:
    case OperandShape::BitTest:
        break;
    }
    GPRReg rightRegister = gprFor(plan.right);
    return isWide ? jit.branch64(condition, leftRegister, rightRegister) : jit.branch32(condition, leftRegister, rightRegister);
}

MacroAssembler::Jump DirectCodeGenerator::branchOn(const B3::Value* condition, bool whenTrue)
{
    if (stateOf(condition).form == ValueForm::Folded) {
        if (condition->opcode() == B3::BitAnd)
            return branchOnTest(condition->child(0), condition->child(1), whenTrue);
        if (condition->opcode() == B3::BitXor)
            return branchOn(condition->child(0), !whenTrue);
        if (B3::isLoad(condition->opcode())) {
#if CPU(X86_64)
            MacroAssembler::ResultCondition result = whenTrue ? MacroAssembler::NonZero : MacroAssembler::Zero;
            MemoryOperand operand = operandForLoad(condition);
            checkASANShadow(operand, condition);
            return withAddress(operand, [&](auto address) {
                switch (condition->opcode()) {
                case B3::Load8Z:
                    return m_jit->branchTest8(result, address);
                case B3::Load16Z:
                    return m_jit->branchTest16(result, address);
                default:
                    return condition->type() == B3::Int64 ? m_jit->branchTest64(result, address) : m_jit->branchTest32(result, address);
                }
            });
#else
            RELEASE_ASSERT_NOT_REACHED();
#endif
        }
        return branchOnComparison(condition, whenTrue);
    }
    GPRReg reg = gprFor(condition);
    MacroAssembler::ResultCondition result = whenTrue ? MacroAssembler::NonZero : MacroAssembler::Zero;
    return condition->type() == B3::Int64 ? m_jit->branchTest64(result, reg) : m_jit->branchTest32(result, reg);
}

std::optional<MacroAssembler::RelationalCondition> DirectCodeGenerator::setFlagsFor(const B3::Value* condition, bool whenTrue)
{
    CCallHelpers& jit = *m_jit;
    auto byZero = [&](bool whenNonZero) {
        return whenNonZero ? MacroAssembler::NotEqual : MacroAssembler::Equal;
    };
    if (stateOf(condition).form != ValueForm::Folded) {
        GPRReg reg = gprFor(condition);
        condition->type() == B3::Int64 ? jit.testOnFlags64(reg, reg) : jit.testOnFlags32(reg, reg);
        return byZero(whenTrue);
    }
    const B3::Value* left = condition->child(0);
    if (condition->opcode() == B3::BitXor)
        return setFlagsFor(left, !whenTrue);
    if (B3::isLoad(condition->opcode()))
        return std::nullopt;
    const B3::Value* right = condition->child(1);
    bool isWide = left->type() == B3::Int64;
    if (condition->opcode() == B3::BitAnd) {
        OperandPlan plan = planTest(left, right, true);
        if (plan.load || plan.shape == OperandShape::BitTest)
            return std::nullopt;
        GPRReg leftRegister = gprFor(plan.left);
        switch (plan.shape) {
        case OperandShape::Immediate: {
            TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
            isWide ? jit.testOnFlags64(leftRegister, mask) : jit.testOnFlags32(leftRegister, mask);
            break;
        }
        case OperandShape::PinnedRegister:
            jit.testOnFlags64(leftRegister, plan.pinned.reg.gpr());
            break;
        case OperandShape::PinnedMemory:
#if CPU(X86_64)
            jit.testOnFlags64(Address(plan.pinned.reg.gpr(), *plan.pinned.offset), leftRegister);
            break;
#else
            RELEASE_ASSERT_NOT_REACHED();
#endif
        case OperandShape::Register:
        case OperandShape::Load:
        case OperandShape::ScaledIndex:
        case OperandShape::BitTest: {
            GPRReg rightRegister = gprFor(plan.right);
            isWide ? jit.testOnFlags64(leftRegister, rightRegister) : jit.testOnFlags32(leftRegister, rightRegister);
            break;
        }
        }
        return byZero(whenTrue);
    }
    if (left->type() == B3::Double)
        return std::nullopt;
    B3::Opcode opcode = condition->opcode();
    if ((opcode == B3::Equal || opcode == B3::NotEqual) && isZero(right) && stateOf(left).form == ValueForm::Folded && !B3::isLoad(left->opcode()))
        return setFlagsFor(left, opcode == B3::NotEqual ? whenTrue : !whenTrue);
    OperandPlan plan = planComparison(opcode, left, right, true);
    if (plan.load)
        return std::nullopt;
    MacroAssembler::RelationalCondition relation = relationalConditionOf(opcode);
    if (plan.isCommuted)
        relation = MacroAssembler::commute(relation);
    if (!whenTrue)
        relation = MacroAssembler::invert(relation);
    GPRReg leftRegister = gprFor(plan.left);
    switch (plan.shape) {
    case OperandShape::Immediate: {
        TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
        isWide ? jit.compareOnFlags64(leftRegister, immediate) : jit.compareOnFlags32(leftRegister, immediate);
        break;
    }
    case OperandShape::PinnedRegister:
        jit.compareOnFlags64(leftRegister, plan.pinned.reg.gpr());
        break;
    case OperandShape::PinnedMemory:
#if CPU(X86_64)
        jit.compareOnFlags64(leftRegister, Address(plan.pinned.reg.gpr(), *plan.pinned.offset));
        break;
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    case OperandShape::Register:
    case OperandShape::Load:
    case OperandShape::ScaledIndex:
    case OperandShape::BitTest: {
        GPRReg rightRegister = gprFor(plan.right);
        isWide ? jit.compareOnFlags64(leftRegister, rightRegister) : jit.compareOnFlags32(leftRegister, rightRegister);
        break;
    }
    }
    return relation;
}

void DirectCodeGenerator::emitSelect(B3::Value* value)
{
    const B3::Value* condition = value->child(0);
    int64_t constant;
    if (constantOf(condition, constant)) {
        Reg source = registerFor(value->child(constant ? 1 : 2));
        Reg result = define(value, source);
        moveRegister(source, result);
        finishDefinition(value, result);
        return;
    }
    Reg whenTrue = registerFor(value->child(1));
    Reg whenFalse = registerFor(value->child(2));
    if (whenTrue.isGPR()) {
        if (std::optional<MacroAssembler::RelationalCondition> flags = setFlagsFor(condition, true)) {
            Reg result = define(value, whenFalse, whenTrue);
            m_jit->moveConditionallyOnFlags64(*flags, whenTrue.gpr(), whenFalse.gpr(), result.gpr());
            finishUnit();
            finishDefinition(value, result);
            return;
        }
    }
    Reg result = define(value, Reg(), Reg(), bitOfRegister(whenTrue) | bitOfRegister(whenFalse));
    moveRegister(whenFalse, result);
    Jump skip = branchOn(condition, false);
    finishUnit();
    moveRegister(whenTrue, result);
    skip.link(m_jit);
    finishDefinition(value, result);
}

void DirectCodeGenerator::emitDoubleBinary(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    FPRReg left = fprFor(value->child(0));
    FPRReg right = fprFor(value->child(1));
    bool commutes = value->opcode() == B3::Add || value->opcode() == B3::Mul;
    FPRReg result = defineFPR(value, left, commutes ? right : InvalidFPRReg);
    switch (value->opcode()) {
    case B3::Add:
        jit.addDouble(left, right, result);
        break;
    case B3::Sub:
        jit.subDouble(left, right, result);
        break;
    case B3::Mul:
        jit.mulDouble(left, right, result);
        break;
    case B3::Div:
        jit.divDouble(left, right, result);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    finishUnit();
    finishDefinition(value, Reg(result));
}

void DirectCodeGenerator::emitDoubleExtremum(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    bool isMax = value->opcode() == B3::FMax;
    FPRReg left = fprFor(value->child(0));
    FPRReg right = fprFor(value->child(1));
#if CPU(ARM64)
    FPRReg result = defineFPR(value, left, right);
    isMax ? jit.doubleMax(left, right, result) : jit.doubleMin(left, right, result);
#else
    FPRReg result = define(value, Reg(), Reg(), bitOfRegister(Reg(left)) | bitOfRegister(Reg(right))).fpr();
    MacroAssembler::JumpList done;
    Jump notEqual = jit.branchDouble(MacroAssembler::DoubleNotEqualOrUnordered, left, right);
    isMax ? jit.andDouble(left, right, result) : jit.orDouble(left, right, result);
    done.append(jit.jump());
    notEqual.link(&jit);
    Jump notLess = jit.branchDouble(MacroAssembler::DoubleGreaterThanOrEqualOrUnordered, left, right);
    jit.moveDouble(isMax ? right : left, result);
    done.append(jit.jump());
    notLess.link(&jit);
    Jump isUnordered = jit.branchDouble(MacroAssembler::DoubleLessThanOrEqualOrUnordered, left, right);
    jit.moveDouble(isMax ? left : right, result);
    done.append(jit.jump());
    isUnordered.link(&jit);
    jit.addDouble(left, right, result);
    done.link(&jit);
#endif
    finishUnit();
    finishDefinition(value, Reg(result));
}

void DirectCodeGenerator::emitDivision(B3::Value* value)
{
#if CPU(X86_64)
    CCallHelpers& jit = *m_jit;
    B3::Opcode opcode = value->opcode();
    bool isWide = value->type() == B3::Int64;
    bool isMultiplication = opcode == B3::UMulHigh || opcode == B3::MulHigh;
    bool isSigned = opcode == B3::Div || opcode == B3::Mod;
    bool takesHighHalf = opcode == B3::Mod || opcode == B3::UMod || isMultiplication;
    Reg low(X86Registers::eax);
    Reg high(X86Registers::edx);
    uint64_t fixed = bitOfRegister(low) | bitOfRegister(high);
    preserveValuesIn(fixed, fixed, true);
    uint64_t left = m_registersWithValuesLeftBehind;
    GPRReg right = registerFor(value->child(1), fixed).gpr();
    m_registersWithValuesLeftBehind = left;
    Vector<Transfer, 8> transfers;
    transfers.append(Transfer { low, value->child(0), Reg() });
    moveIntoRegisters(transfers);
    release(low);
    release(high);
    lock(low);
    lock(high);
    Reg result = takesHighHalf ? high : low;
    MacroAssembler::JumpList done;
    if (value->isChill()) {
        GPRReg scratch = jit.scratchRegister();
        MacroAssembler::JumpList isUsual;
        if (isWide) {
            jit.add64(TrustedImm32(1), right, scratch);
            isUsual.append(jit.branch64(MacroAssembler::Above, scratch, TrustedImm32(1)));
        } else {
            jit.add32(TrustedImm32(1), right, scratch);
            isUsual.append(jit.branch32(MacroAssembler::Above, scratch, TrustedImm32(1)));
        }
        Jump isNotZero = isWide ? jit.branchTest64(MacroAssembler::NonZero, right) : jit.branchTest32(MacroAssembler::NonZero, right);
        jit.move(TrustedImm32(0), result.gpr());
        done.append(jit.jump());
        isNotZero.link(&jit);
        if (isWide) {
            jit.move(TrustedImm64(std::numeric_limits<int64_t>::min()), scratch);
            isUsual.append(jit.branch64(MacroAssembler::NotEqual, low.gpr(), scratch));
        } else
            isUsual.append(jit.branch32(MacroAssembler::NotEqual, low.gpr(), TrustedImm32(std::numeric_limits<int32_t>::min())));
        if (opcode == B3::Mod)
            jit.move(TrustedImm32(0), result.gpr());
        else if (!isWide)
            jit.zeroExtend32ToWord(low.gpr(), low.gpr());
        done.append(jit.jump());
        isUsual.link(&jit);
    }
    if (isMultiplication) {
        if (opcode == B3::UMulHigh)
            isWide ? jit.x86UMulHigh64(right, low.gpr(), high.gpr()) : jit.x86UMulHigh32(right, low.gpr(), high.gpr());
        else
            isWide ? jit.x86MulHigh64(right, low.gpr(), high.gpr()) : jit.x86MulHigh32(right, low.gpr(), high.gpr());
    } else if (isSigned) {
        if (isWide) {
            jit.x86ConvertToQuadWord64();
            jit.x86Div64(right);
        } else {
            jit.x86ConvertToDoubleWord32();
            jit.x86Div32(right);
        }
    } else {
        jit.xor32(high.gpr(), high.gpr());
        isWide ? jit.x86UDiv64(right) : jit.x86UDiv32(right);
    }
    done.link(&jit);
    finishUnit();
    finishDefinition(value, result);
#else
    UNUSED_PARAM(value);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

B3::ValueRep DirectCodeGenerator::repForStackmapChild(const B3::Value* child, const B3::ValueRep& constraint, uint64_t early, uint64_t late, uint64_t& temporaries)
{
    child = resolve(child);
    const ValueState& state = stateOf(child);
    uint64_t bank = state.flags & IsDouble ? m_doubleRegisters : m_generalRegisters;
    switch (constraint.kind()) {
    case B3::ValueRep::SomeRegister:
        return B3::ValueRep::reg(registerFor(child, early));
    case B3::ValueRep::SomeLateRegister:
        return B3::ValueRep::reg(registerFor(child, early | late));
    case B3::ValueRep::SomeRegisterWithClobber: {
        Reg source = registerFor(child, early);
        Reg copy = allocate(bank & ~(early | late));
        lock(copy);
        moveRegister(source, copy);
        temporaries |= bitOfRegister(copy);
        return B3::ValueRep::reg(copy);
    }
    case B3::ValueRep::WarmAny:
    case B3::ValueRep::ColdAny:
    case B3::ValueRep::LateColdAny: {
        uint64_t avoid = constraint.kind() == B3::ValueRep::LateColdAny ? early | late : early;
        if (state.form == ValueForm::Constant)
            return B3::ValueRep::constant(state.flags & (IsWide | IsDouble) ? state.constant : static_cast<int64_t>(static_cast<int32_t>(state.constant)));
        if (state.form != ValueForm::Plain)
            return B3::ValueRep::reg(registerFor(child, avoid));
        Reg reg = locate(child);
        if (reg && !(bitOfRegister(reg) & avoid)) {
            lock(reg);
            return B3::ValueRep::reg(reg);
        }
        if (reg && (m_file.dirty & bitOfRegister(reg))) {
            if (!(state.flags & NeedsHome)) {
                if (Reg free = findFree(bank & ~avoid)) {
                    moveRegister(reg, free);
                    release(reg);
                    bind(free, child->index(), true);
                    lock(free);
                    return B3::ValueRep::reg(free);
                }
            }
            writeBack(reg);
        }
        if (!state.home) {
            fail();
            return B3::ValueRep::constant(0);
        }
        if (state.home > 0) {
            Reg home = decode(static_cast<uint8_t>(state.home));
            if (bitOfRegister(home) & avoid)
                fail();
            return B3::ValueRep::reg(home);
        }
        return B3::ValueRep::stack(state.home + m_homeBias);
    }
    default:
        fail();
        return B3::ValueRep::constant(0);
    }
}

void DirectCodeGenerator::emitCheck(B3::CheckValue* check)
{
    CCallHelpers& jit = *m_jit;
    uint64_t early = bitsOf(check->earlyClobbered());
    uint64_t temporaries = 0;
    WTF::Vector<B3::ValueRep> reps;
    for (unsigned index = 1; index < check->numChildren(); ++index) {
        B3::ConstrainedValue child = check->constrainedChild(index);
        if (child.rep().kind() == B3::ValueRep::Register || child.rep().kind() == B3::ValueRep::StackArgument) {
            fail();
            return;
        }
        reps.append(repForStackmapChild(child.value(), child.rep(), early, 0, temporaries));
    }
    Jump failure;
    int64_t constant;
    if (constantOf(check->child(0), constant))
        failure = jit.jump();
    else
        failure = branchOn(check->child(0), true);
    finishUnit();

    Box<MacroAssembler::JumpList> sharedFailures;
    if (check->sharesLatePath()) {
        auto& latePaths = m_context.sharedLatePathsByGenerator.add(check->generator().get(), Vector<B3::Air::GenerationContext::SharedLatePath, 1>()).iterator->value;
        for (auto& latePath : latePaths) {
            if (latePath.frameIsSetUp == m_context.frameIsSetUp && latePath.reps == reps) {
                latePath.jumps->append(failure);
                return;
            }
        }
        sharedFailures = Box<MacroAssembler::JumpList>::create();
        sharedFailures->append(failure);
        latePaths.append({ reps, m_context.frameIsSetUp, sharedFailures });
    }
    m_context.latePaths.append(std::tuple {
        check->origin(),
        createSharedTask<B3::Air::GenerationContext::LatePathFunction>([=](CCallHelpers& jit, B3::Air::GenerationContext& context) {
            if (sharedFailures)
                sharedFailures->link(&jit);
            else
                failure.link(&jit);
            check->generator()->run(jit, B3::StackmapGenerationParams(check, reps, context));
        })
    });
    noteNewLatePaths();
}

void DirectCodeGenerator::emitCheckedArithmetic(B3::CheckValue* check)
{
    CCallHelpers& jit = *m_jit;
    bool isWide = check->type() == B3::Int64;
    uint64_t early = bitsOf(check->earlyClobbered());
    uint64_t temporaries = 0;
    WTF::Vector<B3::ValueRep> reps;
    for (unsigned index = 2; index < check->numChildren(); ++index) {
        B3::ConstrainedValue child = check->constrainedChild(index);
        if (child.rep().kind() == B3::ValueRep::Register || child.rep().kind() == B3::ValueRep::StackArgument) {
            fail();
            return;
        }
        reps.append(repForStackmapChild(child.value(), child.rep(), early, 0, temporaries));
    }
    GPRReg left = gprFor(check->child(0));
    int64_t constant;
    bool hasImmediate = constantOf(check->child(1), constant) && !isWide && check->opcode() != B3::CheckMul;
    GPRReg right = hasImmediate ? InvalidGPRReg : gprFor(check->child(1));
    GPRReg result = define(check, Reg(), Reg(), m_locked).gpr();
    Jump failure;
    switch (check->opcode()) {
    case B3::CheckAdd:
        if (hasImmediate)
            failure = jit.branchAdd32(MacroAssembler::Overflow, left, TrustedImm32(static_cast<int32_t>(constant)), result);
        else
            failure = isWide ? jit.branchAdd64(MacroAssembler::Overflow, left, right, result) : jit.branchAdd32(MacroAssembler::Overflow, left, right, result);
        break;
    case B3::CheckSub:
        if (hasImmediate)
            failure = jit.branchSub32(MacroAssembler::Overflow, left, TrustedImm32(static_cast<int32_t>(constant)), result);
        else if (isWide) {
            jit.move(left, result);
            failure = jit.branchSub64(MacroAssembler::Overflow, right, result);
        } else
            failure = jit.branchSub32(MacroAssembler::Overflow, left, right, result);
        break;
    case B3::CheckMul:
        failure = isWide ? jit.branchMul64(MacroAssembler::Overflow, left, right, result) : jit.branchMul32(MacroAssembler::Overflow, left, right, result);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    finishUnit();
    m_context.latePaths.append(std::tuple {
        check->origin(),
        createSharedTask<B3::Air::GenerationContext::LatePathFunction>([=](CCallHelpers& jit, B3::Air::GenerationContext& context) {
            failure.link(&jit);
            check->generator()->run(jit, B3::StackmapGenerationParams(check, reps, context));
        })
    });
    noteNewLatePaths();
    finishDefinition(check, Reg(result));
}

void DirectCodeGenerator::emitPatchpoint(B3::PatchpointValue* patchpoint, ValueState& state, DirectBlock& block)
{
    CCallHelpers& jit = *m_jit;
    bool isTerminal = patchpoint->effects.terminal;
    if (isTerminal) {
        passPhiArguments(true);
        bool leadsToEmptyState = false;
        for (unsigned index = 0; index < block.block->numSuccessors(); ++index)
            leadsToEmptyState |= targetOf(block, index).startsEmpty;
        if (leadsToEmptyState)
            flushDirtyValues();
    }

    uint64_t early = bitsOf(patchpoint->earlyClobbered());
    uint64_t late = bitsOf(patchpoint->lateClobbered());
    unsigned numberOfChildren = patchpoint->numChildren();
    unsigned numberOfResults = m_procedure.resultCount(patchpoint->type());
    uint64_t fixedResults = 0;
    for (unsigned index = 0; index < numberOfResults; ++index) {
        const B3::ValueRep& constraint = patchpoint->resultConstraints[index];
        if (constraint.kind() == B3::ValueRep::Register)
            fixedResults |= bitOfRegister(constraint.reg());
    }
    late &= ~fixedResults;
    uint64_t fixedArguments = 0;
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        B3::ValueRep constraint = patchpoint->constrainedChild(index).rep();
        if (constraint.kind() == B3::ValueRep::Register)
            fixedArguments |= bitOfRegister(constraint.reg());
    }
    uint64_t overwritten = fixedArguments | early | late | fixedResults;

    preserveValuesIn(overwritten, overwritten, false);

    B3::StackmapGenerationParams::Reps reps;
    reps.grow(numberOfResults + numberOfChildren);
    uint64_t temporaries = 0;
    uint64_t lockedBefore = m_locked;
    m_locked |= fixedArguments;
    Vector<Transfer, 8> transfers;
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        B3::ConstrainedValue child = patchpoint->constrainedChild(index);
        switch (child.rep().kind()) {
        case B3::ValueRep::Register:
            transfers.append(Transfer { child.rep().reg(), child.value(), Reg() });
            reps[numberOfResults + index] = B3::ValueRep::reg(child.rep().reg());
            break;
        case B3::ValueRep::StackArgument: {
            int32_t offset = child.rep().offsetFromSP() - static_cast<int32_t>(m_code.frameSize());
            const B3::Value* argument = resolve(child.value());
            Reg source = locate(argument);
            if (!source) {
                source = stateOf(argument).flags & IsDouble ? Reg(MacroAssembler::fpTempRegister) : Reg(jit.scratchRegister());
                materialize(argument, source);
            }
            ValueState width = stateOf(argument);
            if (!(width.flags & IsDouble))
                width.flags |= IsWide;
            storeToFrame(source, width, offset);
            reps[numberOfResults + index] = B3::ValueRep::stack(offset);
            break;
        }
        default:
            reps[numberOfResults + index] = repForStackmapChild(child.value(), child.rep(), fixedArguments | early, late | fixedResults, temporaries);
            break;
        }
    }

    Vector<GPRReg, 4> generalScratches;
    Vector<FPRReg, 4> doubleScratches;
    for (unsigned index = 0; index < patchpoint->numGPScratchRegisters; ++index) {
        Reg scratch = allocate(m_generalRegisters & ~overwritten);
        if (m_hasFailed)
            return;
        lock(scratch);
        temporaries |= bitOfRegister(scratch);
        generalScratches.append(scratch.gpr());
    }
    for (unsigned index = 0; index < patchpoint->numFPScratchRegisters; ++index) {
        Reg scratch = allocate(m_doubleRegisters & ~overwritten);
        if (m_hasFailed)
            return;
        lock(scratch);
        temporaries |= bitOfRegister(scratch);
        doubleScratches.append(scratch.fpr());
    }

    std::array<Reg, 8> resultRegisters { };
    if (numberOfResults > resultRegisters.size()) {
        fail();
        return;
    }
    for (unsigned index = 0; index < numberOfResults; ++index) {
        const B3::ValueRep& constraint = patchpoint->resultConstraints[index];
        if (constraint.kind() == B3::ValueRep::Register)
            resultRegisters[index] = constraint.reg();
        else {
            bool isDouble = m_procedure.typeAtOffset(patchpoint->type(), index) == B3::Double;
            Reg hint = numberOfResults == 1 ? hintFor(state) : Reg();
            resultRegisters[index] = allocate((isDouble ? m_doubleRegisters : m_generalRegisters) & ~overwritten, hint);
            lock(resultRegisters[index]);
        }
        reps[index] = B3::ValueRep::reg(resultRegisters[index]);
    }

    moveIntoRegisters(transfers);

    uint64_t resultBits = 0;
    for (unsigned index = 0; index < numberOfResults; ++index)
        resultBits |= bitOfRegister(resultRegisters[index]);
    uint64_t lost = early | late | resultBits | temporaries;
    for (const Transfer& transfer : transfers) {
        if (holderOf(transfer.destination) != transfer.value->index() + 1)
            lost |= bitOfRegister(transfer.destination);
    }

    RegisterSet usedRegisters;
    for (uint64_t bits = m_file.occupied & ~lost; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        if (isLiveAfter(holderOf(reg) - 1))
            usedRegisters.add(reg, IgnoreVectors);
    }
    for (uint64_t bits = m_homeRegisters; bits; bits &= bits - 1)
        usedRegisters.add(Reg::fromIndex(std::countr_zero(bits)), IgnoreVectors);
    patchpoint->m_usedRegisters = usedRegisters;

    B3::StackmapGenerationParams params(patchpoint, WTF::move(reps), m_context);
    for (GPRReg scratch : generalScratches)
        params.m_gpScratch.append(scratch);
    for (FPRReg scratch : doubleScratches)
        params.m_fpScratch.append(scratch);
    WTF::Vector<Box<Label>> labels;
    if (isTerminal) {
        for (unsigned index = 0; index < block.block->numSuccessors(); ++index) {
            DirectBlock& successor = targetOf(block, index);
            labels.append(sharedLabelOf(successor));
            if (isNext(successor, block))
                params.m_successorsFallenThroughTo |= 1u << index;
        }
        params.m_labelsOfSuccessors = &labels;
    }

    m_unitBegin = jit.labelIgnoringWatchpoints();
    patchpoint->generator()->run(jit, params);
    noteNewLatePaths();
    finishUnit(CodeUnit::Kind::Pinned);

    for (uint64_t bits = lost & m_file.occupied; bits; bits &= bits - 1)
        release(Reg::fromIndex(std::countr_zero(bits)));
    m_locked = lockedBefore;

    auto defineResult = [&](const B3::Value* result, Reg reg) {
        if (stateOf(result).form != ValueForm::Plain)
            return;
        if (!isTerminal) {
            finishDefinition(result, reg);
            return;
        }
        bind(reg, result->index(), true);
        if (stateOf(result).flags & NeedsHome)
            targetOf(block, 0).storedAtEntry = 1;
    };
    if (patchpoint->type().isTuple()) {
        for (const B3::Value* extract = state.target; extract; extract = stateOf(extract).target)
            defineResult(extract, resultRegisters[extract->as<B3::ExtractValue>()->index()]);
    } else if (numberOfResults && state.uses)
        defineResult(patchpoint, resultRegisters[0]);

    if (!isTerminal)
        return;
    for (unsigned index = 0; index < block.block->numSuccessors(); ++index) {
        DirectBlock& successor = targetOf(block, index);
        Vector<Transfer, 8> transfers;
        uint64_t registersToWriteBack;
        if (planEntry(successor, transfers, registersToWriteBack)) {
            fail();
            return;
        }
        didBranchTo(successor, block);
    }
}

void DirectCodeGenerator::passPhiArguments(bool terminatorReadsValues)
{
    if (m_pendingUpsilons.isEmpty())
        return;
    struct Copy {
        const B3::Value* source;
        const ValueState* phi;
        int32_t destination;
    };
    Vector<Copy, 8> copies;
    Vector<Transfer, 8> transfers;
    Vector<const B3::Value*, 8> phisInRegisters;
    uint64_t destinations = 0;
    for (const B3::UpsilonValue* upsilon : m_pendingUpsilons) {
        const B3::Value* phi = upsilon->phi();
        ValueState& phiState = stateOf(phi);
        if (phiState.form != ValueForm::Plain)
            continue;
        const B3::Value* source = resolve(upsilon->child(0));
        const ValueState& sourceState = stateOf(source);
        if (phiState.arrival == arrivesInHome) {
            if (liesInHomeOf(sourceState, phiState))
                continue;
            if (phiState.home > 0) {
                Reg home = decode(static_cast<uint8_t>(phiState.home));
                destinations |= bitOfRegister(home);
                transfers.append(Transfer { home, source, Reg() });
                phisInRegisters.append(nullptr);
                continue;
            }
            copies.append(Copy { source, &phiState, phiState.home });
            continue;
        }
        if (phiState.arrival == arrivesInSlot) {
            copies.append(Copy { source, &phiState, phiState.shadow });
            continue;
        }
        if (!phiState.arrival) {
            DirectBlock& target = m_blocks[phiState.block];
            uint64_t candidates = (phiState.flags & IsDouble ? m_doubleRegisters : m_generalRegisters) & ~target.arrivalRegisters;
            Reg chosen;
            Reg hint = decode(phiState.hint);
            Reg current = locate(source);
            if (hint && (candidates & bitOfRegister(hint)))
                chosen = hint;
            else if (current && (candidates & bitOfRegister(current)))
                chosen = current;
            else {
                uint64_t free = candidates & ~m_file.occupied;
                chosen = Reg::fromIndex(std::countr_zero(free ? free : candidates));
            }
            phiState.arrival = encode(chosen);
            target.arrivalRegisters |= bitOfRegister(chosen);
        }
        Reg destination = decode(phiState.arrival);
        destinations |= bitOfRegister(destination);
        transfers.append(Transfer { destination, source, Reg() });
        phisInRegisters.append(phi);
    }

    for (const Copy& copy : copies) {
        for (const Copy& other : copies)
            RELEASE_ASSERT(copy.destination != other.destination || copy.source == other.source);
    }
    for (const Transfer& transfer : transfers) {
        for (const Transfer& other : transfers)
            RELEASE_ASSERT(transfer.destination != other.destination || transfer.value == other.value);
    }

    // The copies of a block happen at once: none reads a home behind the store of another into it.
    auto homeRead = [&](const B3::Value* source) -> int32_t {
        const ValueState& state = stateOf(source);
        return state.form == ValueForm::Plain && !locate(source) ? state.home : 0;
    };
    auto makeCopy = [&](const Copy& copy) {
        const ValueState& phiState = *copy.phi;
        int64_t constant;
        bool mustClearHighBits = (phiState.flags & IsReadWide) && !(phiState.flags & IsWide);
        if (constantOf(copy.source, constant) && !(phiState.flags & IsDouble) && fitsInt32(constant) && !(mustClearHighBits && constant < 0)) {
            if (phiState.flags & (IsWide | IsReadWide))
                m_jit->store64(TrustedImm32(static_cast<int32_t>(constant)), addressOfHome(copy.destination));
            else
                m_jit->store32(TrustedImm32(static_cast<int32_t>(constant)), addressOfHome(copy.destination));
            finishUnit(kindOfFrameAccess(copy.destination));
            return;
        }
        Reg reg = locate(copy.source);
        if (!reg) {
            reg = phiState.flags & IsDouble ? Reg(MacroAssembler::fpTempRegister) : Reg(m_jit->scratchRegister());
            materialize(copy.source, reg);
        }
        storeToFrame(reg, phiState, copy.destination);
    };
    uint64_t lockedBefore = m_locked;
    Vector<const Copy*, 8> pending;
    for (const Copy& copy : copies) {
        pending.append(&copy);
        if (Reg reg = locate(copy.source))
            lock(reg);
    }
    for (const Transfer& transfer : transfers) {
        int32_t home = homeRead(transfer.value);
        if (home && copies.containsIf([&](const Copy& copy) { return copy.destination == home; }))
            lock(registerFor(transfer.value, destinations));
        else if (Reg reg = locate(transfer.value))
            lock(reg);
    }
    while (!pending.isEmpty() && !m_hasFailed) {
        auto isStillRead = [&](const Copy* copy) {
            return pending.containsIf([&](const Copy* other) { return other != copy && homeRead(other->source) == copy->destination; });
        };
        size_t next = pending.findIf([&](const Copy* copy) { return !isStillRead(copy); });
        if (next == notFound) {
            for (const Copy* other : pending) {
                if (other != pending[0] && homeRead(other->source) == pending[0]->destination)
                    lock(registerFor(other->source, destinations));
            }
            continue;
        }
        makeCopy(*pending[next]);
        pending.removeAt(next);
    }
    m_locked = lockedBefore;

    if (!transfers.isEmpty()) {
        preserveValuesIn(destinations, destinations, terminatorReadsValues);
        moveIntoRegisters(transfers);
        for (unsigned index = 0; index < transfers.size(); ++index) {
            if (!phisInRegisters[index])
                continue;
            Reg destination = transfers[index].destination;
            release(destination);
            bind(destination, phisInRegisters[index]->index(), true);
            lock(destination);
        }
    }
    m_pendingUpsilons.shrink(0);
}

// Where B3 validates: on every way into a block the registers hold, at the jump, what the block's code takes them to hold.
void DirectCodeGenerator::checkEdge(const RegisterFile& atJump, uint32_t from, DirectBlock& target)
{
    uint32_t order = static_cast<uint32_t>(&target - m_blocks.span().data());
    const RegisterFile& atEntry = m_snapshots[m_snapshotAtEntry[order] - 1];
    auto stop = [&](const char* what, unsigned index) {
        dataLogLn("AOT: code made without optimizing: on the way from block ", from, " to block ", order, " (#", target.block->index(), ") ", what, ": ", Reg::fromIndex(index));
        dataLogLn(m_procedure);
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (uint64_t bits = atEntry.occupied; bits; bits &= bits - 1) {
        unsigned index = std::countr_zero(bits);
        uint64_t bit = static_cast<uint64_t>(1) << index;
        if (!(atJump.occupied & bit) || atJump.holder[index] != atEntry.holder[index])
            stop("a register does not hold the value that the block takes it to hold", index);
        else if (!(atEntry.dirty & bit) && (atJump.dirty & bit) && !target.storedAtEntry)
            stop("a value is not saved that the block takes for saved", index);
    }
    for (uint64_t bits = atJump.dirty & atJump.occupied; bits; bits &= bits - 1) {
        unsigned index = std::countr_zero(bits);
        uint32_t holder = atJump.holder[index];
        if (!isLiveInto(holder - 1, target))
            continue;
        bool isBound = false;
        for (uint64_t others = atEntry.occupied; others && !isBound; others &= others - 1)
            isBound = atEntry.holder[std::countr_zero(others)] == holder;
        if (!isBound)
            stop("a value that the block needs is neither saved nor in a register there", index);
    }
}

void DirectCodeGenerator::noteEdgeTo(DirectBlock& target)
{
    if (!m_checksEdges || m_hasFailed)
        return;
    uint32_t order = static_cast<uint32_t>(&target - m_blocks.span().data());
    if (m_snapshotAtEntry[order]) {
        checkEdge(m_file, m_currentBlock, target);
        return;
    }
    m_uncheckedEdges.append(UncheckedEdge { order, m_currentBlock, m_file });
}

void DirectCodeGenerator::noteEntryOf(DirectBlock& block)
{
    if (!m_checksEdges || m_hasFailed)
        return;
    uint32_t order = static_cast<uint32_t>(&block - m_blocks.span().data());
    m_snapshots.append(m_file);
    m_snapshotAtEntry[order] = m_snapshots.size();
    m_uncheckedEdges.removeAllMatching([&](const UncheckedEdge& edge) {
        if (edge.target != order)
            return false;
        checkEdge(edge.atJump, edge.from, block);
        return true;
    });
}

void DirectCodeGenerator::link(Jump jump, DirectBlock& target)
{
    noteEdgeTo(target);
    if (target.label.isSet())
        jump.linkTo(target.label, m_jit);
    else
        target.jumps.append(jump);
}

void DirectCodeGenerator::linkConditional(Jump jump, DirectBlock& target)
{
    constexpr uint32_t reachBackward = 120;
    if (!isX86_64() || target.label.isSet() || !target.isRare || m_isInRareCode) {
        link(jump, target);
        return;
    }
    noteEdgeTo(target);
    uint32_t order = static_cast<uint32_t>(&target - m_blocks.span().data());
    uint32_t now = static_cast<uint32_t>(bytesSoFar());
    for (unsigned index = m_islands.size(); index--;) {
        const Island& island = m_islands[index];
        if (now - island.offset > reachBackward)
            break;
        if (island.target == order) {
            jump.linkTo(island.label, m_jit);
            return;
        }
    }
    m_pendingJumps.append(PendingJump { jump, order, now });
}

void DirectCodeGenerator::linkPendingJumps()
{
    for (PendingJump& pending : m_pendingJumps)
        m_blocks[pending.target].jumps.append(pending.jump);
    m_pendingJumps.shrink(0);
}

bool DirectCodeGenerator::mayBeWrittenInGap(DirectBlock& block)
{
    AOT_THRESHOLD(unsigned, maxValues, 10);
    if (!block.isRare || block.label.isSet() || block.block->size() > maxValues)
        return false;
    if (block.startsEmpty ? !block.entry : !block.snapshot)
        return false;
    unsigned numberOfSuccessors = block.block->numSuccessors();
    if (block.onlySuccessor)
        return targetOf(block, block.onlySuccessor - 1).label.isSet();
    for (unsigned index = 0; index < numberOfSuccessors; ++index) {
        if (!targetOf(block, index).label.isSet())
            return false;
    }
    return true;
}

void DirectCodeGenerator::placeIslands(const DirectBlock& next)
{
    constexpr uint32_t reachForward = 127;
    constexpr uint32_t distanceToActAt = 64;
    constexpr uint32_t bytesOfShortJump = 2;
    constexpr uint32_t bytesOfNearJump = 5;
    constexpr int bytesSavedAtJump = 4;
    if (m_pendingJumps.isEmpty())
        return;
    uint32_t now = static_cast<uint32_t>(bytesSoFar());
    uint32_t first = now + (m_codeIsReachable ? bytesOfNearJump : 0);
    m_pendingJumps.removeAllMatching([&](PendingJump& pending) {
        if (first - pending.offset <= reachForward)
            return false;
        m_blocks[pending.target].jumps.append(pending.jump);
        return true;
    });
    constexpr uint32_t bytesExpectedOfValue = 3;
    constexpr uint32_t distanceTakenForNear = 100;
    if (m_pendingJumps.isEmpty())
        return;
    bool wouldBeOutOfReachLater = now - m_pendingJumps[0].offset + next.block->size() * bytesExpectedOfValue > distanceTakenForNear;
    if (m_codeIsReachable && now - m_pendingJumps[0].offset < distanceToActAt && !wouldBeOutOfReachLater && !m_code.policy().layout.placesRareBlocksAfterGuards)
        return;
    if (now - m_pendingJumps[0].offset + m_usualValuesLeft * bytesExpectedOfValue < distanceTakenForNear)
        return;

    Vector<std::pair<uint32_t, unsigned>, 8> targets;
    for (const PendingJump& pending : m_pendingJumps) {
        bool isKnown = false;
        for (auto& [target, count] : targets) {
            if (target == pending.target) {
                ++count;
                isKnown = true;
            }
        }
        if (!isKnown)
            targets.append({ pending.target, 1 });
    }
    const B3::Air::LayoutPolicy& layout = m_code.policy().layout;
    if (m_codeIsReachable && ((!layout.makesGapsForRareBlocks && !layout.placesRareBlocksAfterGuards) || (next.loop)))
        return;
    int gain = m_codeIsReachable ? -static_cast<int>(bytesOfShortJump) : 0;
    for (auto [target, count] : targets) {
        if (mayBeWrittenInGap(m_blocks[target]))
            gain += bytesSavedAtJump * static_cast<int>(count);
        else if (count >= 2)
            gain += bytesSavedAtJump * static_cast<int>(count) - static_cast<int>(bytesOfNearJump);
    }
    if (gain <= 0)
        return;

    bool wasReachable = m_codeIsReachable;
    Jump over;
    if (m_codeIsReachable)
        over = m_jit->jump();
    finishUnit();
    for (auto [target, numberOfJumps] : targets) {
        DirectBlock& block = m_blocks[target];
        if (!mayBeWrittenInGap(block))
            continue;
        uint32_t offset = static_cast<uint32_t>(bytesSoFar());
        bool isWithinReach = true;
        for (const PendingJump& pending : m_pendingJumps)
            isWithinReach &= pending.target != target || offset - pending.offset <= reachForward;
        if (!isWithinReach)
            continue;
        m_pendingJumps.removeAllMatching([&](PendingJump& pending) {
            if (pending.target != target)
                return false;
            block.jumps.append(pending.jump);
            return true;
        });
        RegisterFile file = m_file;
        uint32_t stateIsFor = m_stateIsFor;
        m_isInGap = true;
        m_isInRareCode = true;
        emitBlock(block);
        m_isInGap = false;
        m_isInRareCode = false;
        restoreRegisterFile(file);
        m_stateIsFor = stateIsFor;
        if (m_hasFailed)
            return;
    }
    m_unitBegin = m_jit->labelIgnoringWatchpoints();
    for (auto [target, count] : targets) {
        if (count < 2 || m_blocks[target].label.isSet())
            continue;
        Label label = m_jit->label();
        uint32_t offset = static_cast<uint32_t>(bytesSoFar());
        unsigned numberLinked = 0;
        m_pendingJumps.removeAllMatching([&](PendingJump& pending) {
            if (pending.target != target)
                return false;
            if (offset - pending.offset > reachForward)
                m_blocks[target].jumps.append(pending.jump);
            else {
                pending.jump.linkTo(label, m_jit);
                ++numberLinked;
            }
            return true;
        });
        if (!numberLinked)
            continue;
        m_blocks[target].jumps.append(m_jit->jump());
        m_islands.append(Island { label, target, offset });
    }
    if (wasReachable)
        over.link(m_jit);
    m_codeIsReachable = wasReachable;
    finishUnit();
}

void DirectCodeGenerator::leaveFor(DirectBlock& target)
{
    if (target.startsEmpty)
        flushDirtyValues();
}

bool DirectCodeGenerator::isLiveInto(uint32_t id, const DirectBlock& target) const
{
    const ValueState& state = m_states[id];
    uint32_t order = static_cast<uint32_t>(&target - m_blocks.span().data());
    if (state.form != ValueForm::Plain)
        return false;
    if (state.block == order)
        return m_procedure.values().at(id)->opcode() == B3::Phi;
    if (m_hasLiveness)
        return state.homeIndex && isLiveIn(state.homeIndex - 1, order);
    return state.position < target.firstPosition && state.lastUse >= target.firstPosition;
}

void DirectCodeGenerator::restoreRegisterFile(const RegisterFile& file)
{
    m_file = file;
    for (uint64_t bits = m_file.occupied; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        m_states[holderOf(reg) - 1].reg = encode(reg);
    }
}

void DirectCodeGenerator::arriveAt(DirectBlock& target)
{
    ++target.arrivedEdges;
    if (target.entry || target.label.isSet())
        return;
    RegisterFile entry = m_file;
    for (uint64_t bits = entry.occupied; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        bool isDirty = entry.dirty & bitOfRegister(reg);
        if (isLiveInto(entry.holder[reg.index()] - 1, target))
            continue;
        entry.holder[reg.index()] = 0;
        entry.occupied &= ~bitOfRegister(reg);
        entry.dirty &= ~bitOfRegister(reg);
    }
    m_snapshots.append(entry);
    target.entry = m_snapshots.size();
}

bool DirectCodeGenerator::planEntry(DirectBlock& target, Vector<Transfer, 8>& transfers, uint64_t& registersToWriteBack)
{
    registersToWriteBack = 0;
    if (!target.startsEmpty || !target.entry)
        return false;
    RegisterFile& entry = m_snapshots[target.entry - 1];
    bool mayChangeEntry = !target.label.isSet();
    for (uint64_t bits = entry.occupied; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        uint64_t bit = bitOfRegister(reg);
        uint32_t holder = entry.holder[reg.index()];
        bool isHeld = m_file.holder[reg.index()] == holder;
        if (isHeld) {
            if ((entry.dirty & bit) || !(m_file.dirty & bit))
                continue;
            if (mayChangeEntry)
                entry.dirty |= bit;
            else
                registersToWriteBack |= bit;
            continue;
        }
        if (!(entry.dirty & bit) && mayChangeEntry) {
            entry.holder[reg.index()] = 0;
            entry.occupied &= ~bit;
            continue;
        }
        transfers.append(Transfer { reg, m_procedure.values().at(holder - 1), Reg() });
    }
    for (uint64_t bits = m_file.dirty; bits; bits &= bits - 1) {
        Reg reg = Reg::fromIndex(std::countr_zero(bits));
        uint32_t holder = holderOf(reg);
        if (!isLiveInto(holder - 1, target))
            continue;
        bool isBound = false;
        for (uint64_t others = entry.occupied; others && !isBound; others &= others - 1)
            isBound = entry.holder[std::countr_zero(others)] == holder;
        if (!isBound)
            registersToWriteBack |= bitOfRegister(reg);
    }
    return !transfers.isEmpty() || registersToWriteBack;
}

void DirectCodeGenerator::conformTo(DirectBlock& target)
{
    Vector<Transfer, 8> transfers;
    uint64_t registersToWriteBack;
    if (!planEntry(target, transfers, registersToWriteBack))
        return;
    for (uint64_t bits = registersToWriteBack; bits; bits &= bits - 1)
        writeBack(Reg::fromIndex(std::countr_zero(bits)));
    for (const Transfer& transfer : transfers) {
        if (!locate(transfer.value) && !stateOf(transfer.value).home) {
            fail();
            return;
        }
    }
    moveIntoRegisters(transfers);
    for (const Transfer& transfer : transfers)
        release(transfer.destination);
    for (const Transfer& transfer : transfers) {
        if (Reg old = locate(transfer.value); old && stateOf(transfer.value).home <= 0)
            release(old);
        bind(transfer.destination, transfer.value->index(), true);
    }
}

void DirectCodeGenerator::linkEdge(Jump jump, DirectBlock& target)
{
    Vector<Transfer, 8> transfers;
    uint64_t registersToWriteBack;
    if (!planEntry(target, transfers, registersToWriteBack)) {
        linkConditional(jump, target);
        return;
    }
    m_snapshots.append(m_file);
    m_edgeStubs.append(EdgeStub { jump, static_cast<uint32_t>(&target - m_blocks.span().data()), static_cast<uint32_t>(m_snapshots.size()) });
}

void DirectCodeGenerator::emitEdgeStubs()
{
    for (unsigned index = 0; index < m_edgeStubs.size() && !m_hasFailed; ++index) {
        EdgeStub stub = m_edgeStubs[index];
        DirectBlock& target = m_blocks[stub.target];
        BlockColdness coldness = m_coldness->of(0.001, true);
        m_records->units.willGenerateBlock(coldness.isCold, coldness.isRarelyRun);
        m_unitBegin = m_jit->labelIgnoringWatchpoints();
        m_context.setFrameIsSetUp(*m_jit, m_hasFrame && target.runsInFrame);
        stub.jump.link(m_jit);
        RegisterFile file = m_snapshots[stub.snapshot - 1];
        restoreRegisterFile(file);
        m_locked = 0;
        m_position = target.firstPosition;
        m_currentBlock = UINT32_MAX;
        conformTo(target);
        link(m_jit->jump(), target);
        finishUnit();
    }
}

void DirectCodeGenerator::didBranchTo(DirectBlock& target, const DirectBlock& from, bool registersChangeAfterward)
{
    if (target.startsEmpty) {
        arriveAt(target);
        return;
    }
    if (isNext(target, from) && !registersChangeAfterward) {
        m_stateIsFor = target.emissionIndex;
        return;
    }
    m_snapshots.append(m_file);
    target.snapshot = m_snapshots.size();
}

void DirectCodeGenerator::jumpTo(DirectBlock& target, const DirectBlock& from)
{
    m_codeIsReachable = isNext(target, from);
    if (m_codeIsReachable)
        return;
    Jump jump = m_jit->jump();
    bool isLinked = false;
    if (target.isRare && !m_isInRareCode && !target.label.isSet()) {
        uint32_t order = static_cast<uint32_t>(&target - m_blocks.span().data());
        for (unsigned index = m_islands.size(); index-- && !isLinked && bytesSoFar() - m_islands[index].offset <= 120;) {
            if (m_islands[index].target == order) {
                noteEdgeTo(target);
                jump.linkTo(m_islands[index].label, m_jit);
                isLinked = true;
            }
        }
    }
    if (!isLinked)
        link(jump, target);
    finishUnit();
}

void DirectCodeGenerator::emitReturn(B3::Value* value)
{
    CCallHelpers& jit = *m_jit;
    if (value->numChildren()) {
        Vector<Transfer, 8> transfers;
        transfers.append(Transfer { value->child(0)->type() == B3::Double ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR), value->child(0), Reg() });
        moveIntoRegisters(transfers);
    }
    if (m_context.frameIsSetUp) {
        m_context.setFrameIsSetUp(jit, false);
        B3::Air::emitFrameTeardown(jit, m_code, B3::Air::registerDeadAtReturns);
        finishUnit(CodeUnit::Kind::Pinned);
    }
    jit.ret();
    m_codeIsReachable = false;
    finishUnit(CodeUnit::Kind::End);
}

void DirectCodeGenerator::emitSwitch(B3::SwitchValue* value, DirectBlock& block)
{
    CCallHelpers& jit = *m_jit;
    bool isWide = value->child(0)->type() == B3::Int64;
    passPhiArguments(true);
    unsigned numberOfSuccessors = block.block->numSuccessors();
    for (unsigned index = 0; index < numberOfSuccessors; ++index)
        leaveFor(targetOf(block, index));
    GPRReg reg = gprFor(value->child(0));

    struct Case {
        int64_t value;
        unsigned successor;
    };
    Vector<Case, 16> cases;
    for (unsigned index = 0; index < value->numCaseValues(); ++index)
        cases.append(Case { value->caseValue(index), index });
    AOT::sort(cases, { }, &Case::value);
    DirectBlock& otherwise = targetOf(block, numberOfSuccessors - 1);
    auto finish = [&](bool jumpsToOtherwise) {
        for (unsigned index = 0; index + 1 < numberOfSuccessors; ++index) {
            DirectBlock& successor = targetOf(block, index);
            if (!successor.snapshot)
                didBranchTo(successor, block, jumpsToOtherwise);
        }
        if (jumpsToOtherwise) {
            conformTo(otherwise);
            jumpTo(otherwise, block);
        }
        if (!otherwise.snapshot)
            didBranchTo(otherwise, block);
    };
    uint64_t span = cases.isEmpty() ? 0 : static_cast<uint64_t>(cases.last().value) - static_cast<uint64_t>(cases[0].value);
    bool firstIsSmall = !cases.isEmpty() && fitsInt32(cases[0].value) && cases[0].value != std::numeric_limits<int32_t>::min();
    auto subtractFirst = [&]() -> GPRReg {
        if (!cases[0].value)
            return reg;
        GPRReg index = allocate(m_generalRegisters & ~bitOfRegister(Reg(reg))).gpr();
        lock(Reg(index));
        TrustedImm32 negated(-static_cast<int32_t>(cases[0].value));
        isWide ? jit.add64(negated, reg, index) : jit.add32(negated, reg, index);
        return index;
    };

    AOT_THRESHOLD(uint64_t, densityLimit, 4);
    if (firstIsSmall && cases.size() >= m_procedure.policy().switchLowering.minCasesForJumpTable && span < (1u << 16) && (span + 1) / cases.size() < densityLimit && m_procedure.policy().addressing.isPositionIndependent) {
        bool everyEdgeIsFree = true;
        for (unsigned index = 0; index < numberOfSuccessors; ++index) {
            Vector<Transfer, 8> transfers;
            uint64_t registersToWriteBack;
            everyEdgeIsFree &= !planEntry(targetOf(block, index), transfers, registersToWriteBack);
        }
        if (everyEdgeIsFree) {
            GPRReg index = allocate(m_generalRegisters & ~bitOfRegister(Reg(reg))).gpr();
            lock(Reg(index));
            GPRReg table = allocate(m_generalRegisters & ~bitOfRegister(Reg(reg))).gpr();
            lock(Reg(table));
            TrustedImm32 negated(-static_cast<int32_t>(cases[0].value));
            isWide ? jit.add64(negated, reg, index) : jit.add32(negated, reg, index);
            TrustedImm32 numberOfEntries(static_cast<int32_t>(span + 1));
            linkConditional(isWide ? jit.branch64(MacroAssembler::AboveOrEqual, index, numberOfEntries) : jit.branch32(MacroAssembler::AboveOrEqual, index, numberOfEntries), otherwise);
            finishUnit();
            unsigned jumpTable = m_procedure.addJumpTable(static_cast<unsigned>(span + 1));
            m_records->relocations.moveAddress({ SymbolSpace::JumpTable, jumpTable }, table, PCRelativeReach::Far);
            finishUnit(CodeUnit::Kind::Pinned);
            jit.load32SignedExtendTo64(MacroAssembler::BaseIndex(table, index, MacroAssembler::TimesFour), index);
            jit.add64(table, index);
            for (unsigned successor = 0; successor < numberOfSuccessors; ++successor)
                noteEdgeTo(targetOf(block, successor));
            jit.farJump(index, NoPtrTag);
            finishUnit(CodeUnit::Kind::Pinned);
            m_codeIsReachable = false;
            Vector<std::pair<uint32_t, Box<Label>>> entries;
            for (const Case& entry : cases)
                entries.append({ static_cast<uint32_t>(entry.value - cases[0].value), sharedLabelOf(targetOf(block, entry.successor)) });
            Box<Label> labelOfOtherwise = sharedLabelOf(otherwise);
            B3::Procedure::JumpTableRecord* record = &m_procedure.jumpTable(jumpTable);
            jit.addLinkTask([record, entries = WTF::move(entries), labelOfOtherwise](LinkBuffer& linkBuffer) {
                record->targetOffsets.fill(linkBuffer.offsetOf(*labelOfOtherwise));
                for (const auto& [entry, label] : entries)
                    record->targetOffsets[entry] = linkBuffer.offsetOf(*label);
            });
            finish(false);
            return;
        }
    }

#if CPU(X86_64)
    constexpr unsigned minCasesForMask = 3;
    if (firstIsSmall && cases.size() > minCasesForMask && span < 64) {
        struct Group {
            DirectBlock* target;
            uint64_t mask;
            unsigned size;
        };
        Vector<Group, 8> groups;
        bool hasLargeGroup = false;
        for (const Case& entry : cases) {
            DirectBlock* target = &targetOf(block, entry.successor);
            if (target == &otherwise)
                continue;
            uint64_t bit = static_cast<uint64_t>(1) << (entry.value - cases[0].value);
            size_t position = groups.findIf([&](const Group& group) { return group.target == target; });
            if (position == notFound)
                groups.append(Group { target, bit, 1 });
            else {
                groups[position].mask |= bit;
                hasLargeGroup |= ++groups[position].size >= minCasesForMask;
            }
        }
        if (hasLargeGroup) {
            GPRReg index = subtractFirst();
            GPRReg mask = allocate(m_generalRegisters & ~(bitOfRegister(Reg(reg)) | bitOfRegister(Reg(index)))).gpr();
            lock(Reg(mask));
            TrustedImm32 last(static_cast<int32_t>(span));
            linkEdge(isWide ? jit.branch64(MacroAssembler::Above, index, last) : jit.branch32(MacroAssembler::Above, index, last), otherwise);
            finishUnit();
            for (const Group& group : groups) {
                if (group.size >= minCasesForMask) {
                    jit.move(TrustedImm64(static_cast<int64_t>(group.mask)), mask);
                    linkEdge(jit.branchTestBit64(MacroAssembler::NonZero, mask, index), *group.target);
                    finishUnit();
                    continue;
                }
                for (uint64_t bits = group.mask; bits; bits &= bits - 1) {
                    TrustedImm32 position(std::countr_zero(bits));
                    linkEdge(isWide ? jit.branch64(MacroAssembler::Equal, index, position) : jit.branch32(MacroAssembler::Equal, index, position), *group.target);
                    finishUnit();
                }
            }
            finish(true);
            return;
        }
    }
#endif

    for (unsigned index = 0; index < value->numCaseValues(); ++index) {
        int64_t caseValue = value->caseValue(index);
        Jump jump;
        if (!isWide)
            jump = jit.branch32(MacroAssembler::Equal, reg, TrustedImm32(static_cast<int32_t>(caseValue)));
        else if (fitsInt32(caseValue))
            jump = jit.branch64(MacroAssembler::Equal, reg, TrustedImm32(static_cast<int32_t>(caseValue)));
        else
            jump = jit.branch64(MacroAssembler::Equal, reg, TrustedImm64(caseValue));
        linkEdge(jump, targetOf(block, index));
        finishUnit();
    }
    for (unsigned index = 0; index + 1 < numberOfSuccessors; ++index) {
        DirectBlock& successor = targetOf(block, index);
        if (!successor.snapshot)
            didBranchTo(successor, block, true);
    }
    DirectBlock& fallThrough = targetOf(block, numberOfSuccessors - 1);
    conformTo(fallThrough);
    jumpTo(fallThrough, block);
    if (!fallThrough.snapshot)
        didBranchTo(fallThrough, block);
}

void DirectCodeGenerator::emitTerminator(B3::Value* value, ValueState& state, DirectBlock& block)
{
    CCallHelpers& jit = *m_jit;
    switch (value->opcode()) {
    case B3::Jump: {
        DirectBlock& target = targetOf(block, 0);
        leaveFor(target);
        passPhiArguments(false);
        conformTo(target);
        jumpTo(target, block);
        didBranchTo(target, block);
        return;
    }
    case B3::Branch: {
        DirectBlock& taken = targetOf(block, 0);
        DirectBlock& notTaken = targetOf(block, 1);
        if (block.onlySuccessor) {
            DirectBlock& target = block.onlySuccessor == 1 ? taken : notTaken;
            leaveFor(target);
            passPhiArguments(false);
            conformTo(target);
            jumpTo(target, block);
            didBranchTo(target, block);
            return;
        }
        leaveFor(taken);
        leaveFor(notTaken);
        passPhiArguments(true);
        int64_t constant;
        if (constantOf(value->child(0), constant)) {
            DirectBlock& target = constant ? taken : notTaken;
            conformTo(target);
            jumpTo(target, block);
            didBranchTo(taken, block);
            if (&taken != &notTaken)
                didBranchTo(notTaken, block);
            return;
        }
        if (&taken == &notTaken) {
            conformTo(taken);
            jumpTo(taken, block);
            didBranchTo(taken, block);
            return;
        }
        if (isLogicalCondition(value->child(0))) {
            const B3::Value* first = value->child(0)->child(0);
            const B3::Value* second = value->child(0)->child(1);
            bool isOr = value->child(0)->opcode() == B3::BitOr;
            fetchConditionOperands(first);
            fetchConditionOperands(second);
            linkEdge(branchOn(first, isOr), isOr ? taken : notTaken);
            finishUnit();
            if (isNext(taken, block)) {
                linkEdge(branchOn(second, false), notTaken);
                finishUnit();
                m_codeIsReachable = true;
                didBranchTo(notTaken, block);
                conformTo(taken);
                didBranchTo(taken, block);
            } else {
                linkEdge(branchOn(second, true), taken);
                finishUnit();
                didBranchTo(taken, block);
                conformTo(notTaken);
                jumpTo(notTaken, block);
                didBranchTo(notTaken, block);
            }
            return;
        }
        if (isNext(taken, block)) {
            linkEdge(branchOn(value->child(0), false), notTaken);
            finishUnit();
            m_codeIsReachable = true;
            didBranchTo(notTaken, block);
            conformTo(taken);
            didBranchTo(taken, block);
        } else {
            linkEdge(branchOn(value->child(0), true), taken);
            finishUnit();
            didBranchTo(taken, block);
            conformTo(notTaken);
            jumpTo(notTaken, block);
            didBranchTo(notTaken, block);
        }
        return;
    }
    case B3::Switch:
        emitSwitch(value->as<B3::SwitchValue>(), block);
        return;
    case B3::Return:
        emitReturn(value);
        return;
    case B3::Oops:
        jit.breakpoint();
        m_codeIsReachable = false;
        finishUnit(CodeUnit::Kind::End);
        return;
    case B3::Patchpoint:
        m_codeIsReachable = true;
        emitPatchpoint(value->as<B3::PatchpointValue>(), state, block);
        return;
    default:
        fail();
        return;
    }
}

void DirectCodeGenerator::emitValue(B3::Value* value, ValueState& state, DirectBlock& block)
{
    CCallHelpers& jit = *m_jit;
    switch (value->opcode()) {
    case B3::ArgumentReg:
        if (state.flags & NeedsHome) {
            Reg reg = locate(value);
            if (!reg) {
                fail();
                return;
            }
            if ((m_file.dirty & bitOfRegister(reg)) && !m_blockThatSetsUpFrame)
                writeBack(reg);
        }
        return;
    case B3::Extract:
        return;
    case B3::Set: {
        const B3::Variable* variable = value->as<B3::VariableValue>()->variable();
        ValueState width { };
        width.flags = variable->type() == B3::Double ? IsDouble : variable->type() == B3::Int64 ? IsWide : 0;
        storeToFrame(registerFor(value->child(0)), width, m_variableOffsets[variable->index()]);
        return;
    }
    case B3::Get: {
        Reg result = define(value);
        loadFromFrame(result, state, m_variableOffsets[value->as<B3::VariableValue>()->variable()->index()]);
        finishDefinition(value, result);
        return;
    }
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::Div:
        if (state.flags & IsDouble)
            emitDoubleBinary(value);
        else if (value->opcode() == B3::Div)
            emitDivision(value);
        else
            emitIntegerBinary(value);
        return;
    case B3::Mod:
    case B3::UDiv:
    case B3::UMod:
    case B3::UMulHigh:
    case B3::MulHigh:
        emitDivision(value);
        return;
    case B3::FMax:
    case B3::FMin:
        emitDoubleExtremum(value);
        return;
    case B3::PopCount: {
        GPRReg source = gprFor(value->child(0));
        GPRReg result = defineGPR(value, source);
        state.flags & IsWide ? jit.countPopulation64(source, result, MacroAssembler::fpTempRegister) : jit.countPopulation32(source, result, MacroAssembler::fpTempRegister);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::Ceil:
    case B3::Floor:
    case B3::FTrunc: {
        FPRReg source = fprFor(value->child(0));
        FPRReg result = defineFPR(value, source);
        if (value->opcode() == B3::Ceil)
            jit.ceilDouble(source, result);
        else if (value->opcode() == B3::Floor)
            jit.floorDouble(source, result);
        else
            jit.truncDouble(source, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        emitIntegerBinary(value);
        return;
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
        emitShift(value);
        return;
    case B3::Neg: {
        if (state.flags & IsDouble) {
            FPRReg source = fprFor(value->child(0));
            FPRReg result = defineFPR(value, source);
#if CPU(X86_64)
            B3::PinnedConstant signBit = *pinnedConstantFor(std::numeric_limits<int64_t>::min());
            jit.loadDouble(Address(signBit.reg.gpr(), *signBit.offset), MacroAssembler::fpTempRegister);
            jit.xorDouble(source, MacroAssembler::fpTempRegister, result);
#else
            jit.negateDouble(source, result);
#endif
            finishUnit();
            finishDefinition(value, Reg(result));
            return;
        }
        GPRReg source = gprFor(value->child(0));
        GPRReg result = defineGPR(value, source);
        state.flags & IsWide ? jit.neg64(source, result) : jit.neg32(source, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::Clz: {
        GPRReg source = gprFor(value->child(0));
        GPRReg result = defineGPR(value, source);
        state.flags & IsWide ? jit.countLeadingZeros64(source, result) : jit.countLeadingZeros32(source, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::Sqrt:
    case B3::Abs: {
        FPRReg source = fprFor(value->child(0));
        FPRReg result = defineFPR(value, source);
        value->opcode() == B3::Sqrt ? jit.sqrtDouble(source, result) : jit.absDouble(source, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::BitwiseCast: {
        Reg source = registerFor(value->child(0));
        Reg result = define(value);
        if (state.flags & IsDouble)
            jit.move64ToDouble(source.gpr(), result.fpr());
        else
            jit.moveDoubleTo64(source.fpr(), result.gpr());
        finishUnit();
        finishDefinition(value, result);
        return;
    }
    case B3::IToD: {
        GPRReg source = gprFor(value->child(0));
        FPRReg result = defineFPR(value);
        value->child(0)->type() == B3::Int64 ? jit.convertInt64ToDouble(source, result) : jit.convertInt32ToDouble(source, result);
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::ZExt32:
    case B3::Trunc: {
        GPRReg source = gprFor(value->child(0));
        GPRReg result = defineGPR(value, source);
        switch (value->opcode()) {
        case B3::SExt8:
            jit.signExtend8To32(source, result);
            break;
        case B3::SExt16:
            jit.signExtend16To32(source, result);
            break;
        case B3::SExt32:
            jit.signExtend32To64(source, result);
            break;
        case B3::ZExt32:
            jit.zeroExtend32ToWord(source, result);
            break;
        default:
            jit.move(source, result);
            break;
        }
        finishUnit();
        finishDefinition(value, Reg(result));
        return;
    }
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::EqualOrUnordered:
        emitComparison(value);
        return;
    case B3::Select:
        emitSelect(value);
        return;
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load:
        emitLoad(value);
        return;
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
        emitStore(value);
        return;
    case B3::Fence: {
        const B3::FenceValue* fence = value->as<B3::FenceValue>();
        if (!fence->write && !fence->read)
            return;
        if (!fence->write)
            jit.storeFence();
        else if (!fence->read)
            jit.loadFence();
        else
            jit.memoryFence();
        finishUnit(CodeUnit::Kind::Pinned);
        return;
    }
    case B3::Check:
        emitCheck(value->as<B3::CheckValue>());
        return;
    case B3::CheckAdd:
    case B3::CheckSub:
    case B3::CheckMul:
        emitCheckedArithmetic(value->as<B3::CheckValue>());
        return;
    case B3::Upsilon:
        m_pendingUpsilons.removeAllMatching([&](const B3::UpsilonValue* earlier) {
            return earlier->phi() == value->as<B3::UpsilonValue>()->phi();
        });
        m_pendingUpsilons.append(value->as<B3::UpsilonValue>());
        return;
    case B3::Phi: {
        if (state.arrival == arrivesInHome)
            return;
        if (state.arrival == arrivesInSlot) {
            Reg result = define(value);
            loadFromFrame(result, state, state.shadow);
            finishDefinition(value, result);
            return;
        }
        Reg reg = locate(value);
        if (!reg) {
            fail();
            return;
        }
        if (state.flags & NeedsHome)
            writeBack(reg);
        return;
    }
    case B3::Patchpoint:
        if (!value->as<B3::PatchpointValue>()->effects.terminal) {
            emitPatchpoint(value->as<B3::PatchpointValue>(), state, block);
            return;
        }
        [[fallthrough]];
    default:
        emitTerminator(value, state, block);
        return;
    }
}

void DirectCodeGenerator::enterBlock(DirectBlock& block)
{
    if (!block.emissionIndex)
        return;
    if (!block.startsEmpty) {
        if (m_stateIsFor != block.emissionIndex) {
            if (!block.snapshot) {
                fail();
                return;
            }
            m_file = m_snapshots[block.snapshot - 1];
            for (uint64_t bits = m_file.occupied; bits; bits &= bits - 1) {
                Reg reg = Reg::fromIndex(std::countr_zero(bits));
                m_states[holderOf(reg) - 1].reg = encode(reg);
            }
        }
        if (block.storedAtEntry)
            flushDirtyValues();
        return;
    }
    if (block.entry) {
        RegisterFile& entry = m_snapshots[block.entry - 1];
        if (block.arrivedEdges < block.incomingEdges) {
            for (uint64_t bits = entry.occupied & ~entry.dirty; bits; bits &= bits - 1) {
                entry.holder[std::countr_zero(bits)] = 0;
                entry.occupied &= ~(static_cast<uint64_t>(1) << std::countr_zero(bits));
            }
        }
        RegisterFile file = entry;
        restoreRegisterFile(file);
        return;
    }
    if (block.continuesStretch) {
        fail();
        return;
    }
    m_file.holder.fill(0);
    m_file.occupied = 0;
    m_file.dirty = 0;
    for (uint32_t phi = block.firstPhi; phi; phi = m_states[phi - 1].nextPhi) {
        const ValueState& state = m_states[phi - 1];
        if (state.form == ValueForm::Plain && state.arrival && state.arrival < arrivesInHome)
            bind(decode(state.arrival), phi - 1, true);
    }
    m_snapshots.append(m_file);
    block.entry = m_snapshots.size();
}

void DirectCodeGenerator::emitBlock(DirectBlock& block)
{
    CCallHelpers& jit = *m_jit;
    if (block.isRare && !m_isInRareCode) {
        m_isInRareCode = true;
        linkPendingJumps();
    }
    if (!m_isInRareCode)
        placeIslands(block);
    if (!block.isRare)
        m_usualValuesLeft -= block.block->size();
    block.jumps.link(&jit);
    block.label = jit.label();
    if (block.sharedLabel)
        *block.sharedLabel = block.label;
    bool setsUpFrame = m_blockThatSetsUpFrame && &block == &m_blocks[m_blockThatSetsUpFrame];
    m_context.setFrameIsSetUp(jit, m_hasFrame && block.runsInFrame && !setsUpFrame);
    BlockColdness coldness = m_coldness->of(block.frequency, block.isRare);
    m_records->units.willGenerateBlock(coldness.isCold, coldness.isRarelyRun);
    m_unitBegin = jit.labelIgnoringWatchpoints();

    m_locked = 0;
    m_position = block.firstPosition;
    if (m_codeIsReachable && !m_isInGap && block.emissionIndex && m_currentBlock < m_blocks.size()) {
        bool isSuccessor = false;
        for (unsigned successor = 0; successor < m_blocks[m_currentBlock].block->numSuccessors(); ++successor)
            isSuccessor |= &targetOf(m_blocks[m_currentBlock], successor) == &block;
        if (isSuccessor)
            noteEdgeTo(block);
    }
    m_currentBlock = static_cast<uint32_t>(&block - m_blocks.span().data());
    enterBlock(block);
    noteEntryOf(block);
    m_stateIsFor = UINT32_MAX;
    m_pendingUpsilons.shrink(0);
    for (uint32_t id : m_valuesLetGoUnsaved)
        m_states[id].flags &= ~WasLetGoUnsaved;
    m_valuesLetGoUnsaved.shrink(0);
    if (setsUpFrame && !m_hasFailed) {
        emitFrameSetup();
        flushDirtyValues();
    }

    for (B3::Value* value : *block.block) {
        ValueState& state = stateOf(value);
        if (state.form != ValueForm::Plain)
            continue;
        m_position = state.position;
        m_user = userOf(state);
        m_locked = 0;
        recordOrigin(value);
        emitValue(value, state, block);
        if (m_hasFailed)
            return;
    }
}

bool DirectCodeGenerator::generate(CCallHelpers& jit, CodeRecords& records, const BlockColdnessRule& coldness)
{
    layOutFrame();
    findBlocksWithoutCode();
    orderEmission();
    chooseWhereToSetUpFrame();
    estimateFrequencies();
    m_isPlanning = false;
    m_checksEdges = B3::shouldValidateIR(m_procedure);
    if (m_checksEdges)
        m_snapshotAtEntry.fill(0, m_blocks.size());
    m_jit = &jit;
    m_records = &records;
    m_coldness = &coldness;
    AllowMacroScratchRegisterUsage allowScratch(jit);

    m_context.code = &m_code;
    m_context.frameIsSetUp = false;

    m_file.holder.fill(0);
    m_file.occupied = 0;
    m_file.dirty = 0;
    for (unsigned index = 0; index < numberOfRegisters; ++index) {
        const B3::Value* argument = m_argumentValues[index];
        if (argument && stateOf(argument).form == ValueForm::Plain)
            bind(Reg::fromIndex(index), argument->index(), true);
    }

    Label entry = jit.label();
    BlockColdness entryColdness = coldness.of(1, false);
    records.units.willGenerateBlock(entryColdness.isCold, entryColdness.isRarelyRun);
    m_unitBegin = jit.labelIgnoringWatchpoints();
    if (m_hasFrame && !m_blockThatSetsUpFrame)
        emitFrameSetup();

    for (uint32_t order : m_emissionOrder) {
        if (m_blocks[order].label.isSet())
            continue;
        emitBlock(m_blocks[order]);
        if (m_hasFailed)
            return false;
    }

    emitEdgeStubs();
    if (m_hasFailed)
        return false;
    bool hasJumpToNowhere = false;
    for (const DirectBlock& block : m_blocks)
        hasJumpToNowhere |= !block.label.isSet() && !block.jumps.empty();
    if (!m_pendingJumps.isEmpty() || hasJumpToNowhere) {
        fail();
        return false;
    }
    m_code.setNumEntrypoints(1);
    m_code.setEntrypointLabels(WTF::Vector<Label> { entry });
    if (m_procedure.needsPCToOriginMap()) [[unlikely]]
        m_procedure.pcToOriginMap().appendItem(jit.labelIgnoringWatchpoints(), B3::Origin());

    for (unsigned index = 0; index < m_context.latePaths.size(); ++index) {
        Ref<B3::Air::GenerationContext::LatePath> latePath = std::get<1>(m_context.latePaths[index]);
        m_context.setFrameIsSetUp(jit, m_hasFrame && !m_context.latePathsWithoutFrame.get(index));
        latePath->run(jit, m_context);
        noteNewLatePaths();
    }
    m_context.setFrameIsSetUp(jit, false);
    m_code.setFrameRanges(WTF::move(m_context.frameRanges));
    if (m_procedure.needsPCToOriginMap()) [[unlikely]]
        m_procedure.pcToOriginMap().appendItem(jit.labelIgnoringWatchpoints(), B3::Origin());

    return true;
}

void DirectCodeGeneratorDeleter::operator()(DirectCodeGenerator* generator) const
{
    delete generator;
}

DirectCodeAttempt attemptAfterFailure(const DirectCodeGenerator& generator)
{
    return generator.attemptAfterFailure();
}

DirectCodeGeneratorPointer planDirectCode(B3::Procedure& procedure, DirectCodeAttempt attempt)
{
    DirectCodeGeneratorPointer generator(new DirectCodeGenerator(procedure, attempt));
    if (!generator->plan())
        return nullptr;
    return generator;
}

bool generateDirectCode(DirectCodeGenerator& generator, CCallHelpers& jit, CodeRecords& records, const BlockColdnessRule& coldness)
{
    return generator.generate(jit, records, coldness);
}

} } } // namespace JSC::AOT::Typed

#elif ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class DirectCodeGenerator { };

void DirectCodeGeneratorDeleter::operator()(DirectCodeGenerator* generator) const { delete generator; }
DirectCodeGeneratorPointer planDirectCode(B3::Procedure&, DirectCodeAttempt) { return nullptr; }
DirectCodeAttempt attemptAfterFailure(const DirectCodeGenerator&) { return DirectCodeAttempt::None; }
bool generateDirectCode(DirectCodeGenerator&, CCallHelpers&, CodeRecords&, const BlockColdnessRule&) { return false; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
