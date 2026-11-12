/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineArena.h"
#include "AOTMachineHeap.h"
#include "AOTMachineOpcode.h"
#include "AOTRelocation.h"
#include "CCallHelpers.h"
#include "CPUFeatureSet.h"
#include "Reg.h"
#include "RegisterAtOffsetList.h"
#include "RegisterSet.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Machine {

class Allocation;
class Allocator;
class Block;
class Builder;
class Encoder;
class Frame;
class Liveness;
class Numbering;
class Operation;
class Order;
class PatchOperation;
class PatchParameters;
class Procedure;
class SlotAssignment;
class StackSlot;
struct MachineCoreListsOfHeapsKept;

Frame* planFrame(Procedure&);

using SpillIndex = uint32_t;
inline constexpr SpillIndex noSpill = UINT32_MAX;

using RegisterMask = uint64_t;
static_assert(Reg::maxIndex() < 64);

constexpr RegisterMask maskOf(Reg reg) { return RegisterMask { 1 } << reg.index(); }
inline RegisterMask maskOf(const RegisterSet& set)
{
    static_assert(sizeof(RegisterBitSet) == sizeof(RegisterBitSet::WordType));
    return set.toScalarRegisterSet().bitsForDebugging();
}

template<typename Functor>
void forEachRegister(RegisterMask mask, const Functor& functor)
{
    for (; mask; mask &= mask - 1)
        functor(Reg::fromIndex(static_cast<unsigned>(std::countr_zero(mask))));
}

inline RegisterSet registerSetOf(RegisterMask mask)
{
    [[maybe_unused]] constexpr RegisterMask everyRegister = ~RegisterMask { 0 } >> (63 - Reg::maxIndex());
    ASSERT(!(mask & ~everyRegister));

    RegisterSet result;
    forEachRegister(mask, [&](Reg reg) {
        result.add(reg, IgnoreVectors);
    });
    return result;
}

class Origin {
public:
    constexpr Origin() = default;
    constexpr explicit Origin(uint32_t bits)
        : m_bits(bits)
    {
    }

    constexpr uint32_t bits() const { return m_bits; }
    constexpr explicit operator bool() const { return m_bits; }
    friend constexpr bool operator==(Origin, Origin) = default;

private:
    uint32_t m_bits { 0 };
};

class ValueRep {
public:
    using OffsetType = int32_t;

    enum Kind : uint8_t {
        WarmAny,
        ColdAny,
        LateColdAny,
        SomeRegister,
        SomeEarlyRegister,
        SomeLateRegister,
        Register,
        StackArgument,
        Stack,
        Constant,
    };

    constexpr ValueRep() = default;
    constexpr ValueRep(Kind kind)
        : m_kind(kind)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == WarmAny || kind == ColdAny || kind == LateColdAny || kind == SomeRegister || kind == SomeEarlyRegister || kind == SomeLateRegister);
    }
    constexpr explicit ValueRep(Reg reg)
        : m_kind(Register)
        , m_reg(reg)
    {
    }

    static constexpr ValueRep reg(Reg reg) { return ValueRep(reg); }
    static constexpr ValueRep stackArgument(OffsetType offsetFromSP) { return withValue(StackArgument, offsetFromSP); }
    static constexpr ValueRep stack(OffsetType offsetFromFP) { return withValue(Stack, offsetFromFP); }
    static constexpr ValueRep constant(int64_t value) { return withValue(Constant, value); }

    constexpr Kind kind() const { return m_kind; }
    constexpr bool isAny() const { return m_kind == WarmAny || m_kind == ColdAny || m_kind == LateColdAny; }
    constexpr bool isLate() const { return m_kind == LateColdAny || m_kind == SomeLateRegister; }
    constexpr bool isReg() const { return m_kind == Register; }
    constexpr bool isGPR() const { return isReg() && m_reg.isGPR(); }
    constexpr bool isFPR() const { return isReg() && m_reg.isFPR(); }
    constexpr bool isStack() const { return m_kind == Stack; }
    constexpr bool isStackArgument() const { return m_kind == StackArgument; }
    constexpr bool isConstant() const { return m_kind == Constant; }

    Reg reg() const
    {
        ASSERT(isReg());
        return m_reg;
    }
    GPRReg gpr() const { return reg().gpr(); }
    FPRReg fpr() const { return reg().fpr(); }
    OffsetType offsetFromFP() const
    {
        ASSERT(isStack());
        return static_cast<OffsetType>(m_value);
    }
    OffsetType offsetFromSP() const
    {
        ASSERT(isStackArgument());
        return static_cast<OffsetType>(m_value);
    }
    int64_t value() const
    {
        ASSERT(isConstant());
        return m_value;
    }

    JS_EXPORT_PRIVATE void emitRestore(CCallHelpers&, Reg destination) const;

    friend constexpr bool operator==(const ValueRep&, const ValueRep&) = default;
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    static constexpr ValueRep withValue(Kind kind, int64_t value)
    {
        ValueRep result;
        result.m_kind = kind;
        result.m_value = value;
        return result;
    }

    Kind m_kind { WarmAny };
    Reg m_reg;
    int64_t m_value { 0 };
};
static_assert(sizeof(ValueRep) == 16);

class ConstrainedValue {
public:
    ConstrainedValue() = default;
    ConstrainedValue(Operation* value, const ValueRep& rep = ValueRep::WarmAny)
        : m_value(value)
        , m_rep(rep)
    {
    }

    explicit operator bool() const { return m_value; }
    Operation* value() const { return m_value; }
    const ValueRep& rep() const { return m_rep; }

private:
    Operation* m_value { nullptr };
    ValueRep m_rep;
};

struct PureFunction {
    static constexpr uint64_t firstIDOfBackEnd = uint64_t { 1 } << 63;

    uint64_t id;
    bool dependsOnChildren { false };
    bool truncatesDoubleToInt64 { false };
    bool dependsOnMemory { false };

    friend bool operator==(const PureFunction&, const PureFunction&) = default;
};

struct Effects {
    static Effects none() { return { }; }
    static Effects forCall()
    {
        Effects result;
        result.exitsSideways = true;
        result.controlDependent = true;
        result.mayCollect = true;
        result.reads = HeapSet::top();
        result.writes = HeapSet::top();
        return result;
    }

    bool mustStay() const { return terminal || exitsSideways || writesLocalState || mayCollect || !writes.isEmpty(); }
    bool maySink() const { return !mustStay() && reads.isEmpty(); }

    bool terminal { false };
    bool exitsSideways { false };
    bool controlDependent { false };
    bool writesLocalState { false };
    // The collector may run in it: a narrow reference that lives across it has its pointer beside it.
    bool mayCollect { false };
    HeapSet reads;
    HeapSet writes;
};

enum class EdgeClass : uint8_t {
    Normal,
    Unlikely,
    Rare,
};

class Edge {
public:
    Edge() = default;
    Edge(Block* block, EdgeClass edgeClass = EdgeClass::Normal)
        : m_block(block)
        , m_class(edgeClass)
    {
    }

    explicit operator bool() const { return m_block; }
    Block* block() const { return m_block; }
    EdgeClass edgeClass() const { return m_class; }
    bool isRare() const { return m_class == EdgeClass::Rare; }
    friend bool operator==(const Edge&, const Edge&) = default;

private:
    friend class Order;

    Block* m_block { nullptr };
    EdgeClass m_class { EdgeClass::Normal };
};

enum class OperationFlag : uint16_t {
    Traps = 1 << 0,
    IsNarrowReference = 1 << 1,
    IsInnerPointer = 1 << 2,
    IsChill = 1 << 3,
    ReadsImmutableHeap = 1 << 4,
    NamesPermanentCell = 1 << 5,
    UpperHalfIsRead = 1 << 6,
    IsIntegerAddedTo = 1 << 7,
    ReadsWordFilledOnce = 1 << 8,
    ExtendsWordThatIsNotNegative = 1 << 9,
    UpperHalfMayBeSet = 1 << 10,
    HasOpaqueReader = 1 << 11,
    ScalesIndexOfSum = 1 << 12,
};

struct SwitchData {
    std::span<const int64_t> caseValues;
};

class Operation {
    WTF_MAKE_NONCOPYABLE(Operation);
public:
    static constexpr unsigned maxInlineChildren = 3;

    Opcode opcode() const { return m_opcode; }
    Type type() const { return m_type; }
    uint32_t index() const { return m_index; }
    Origin origin() const { return m_origin; }
    Block* owner() const { return m_owner; }
    uint32_t indexInBlock() const { return m_indexInBlock; }
    bool hasNoPosition() const { return !m_owner; }

    inline unsigned numChildren() const;
    inline Operation* child(unsigned index) const;
    Operation* lastChild() const { return child(numChildren() - 1); }
    inline std::span<Operation* const> children() const;
    Operation* const* inlineChildren() const
    {
        ASSERT(m_opcode != Patch && m_opcode != Check && !namesSymbol(m_opcode));
        return m_children;
    }
    unsigned numberOfInlineChildren() const
    {
        ASSERT(m_opcode != Patch && m_opcode != Check && !namesSymbol(m_opcode));
        return m_auxiliary;
    }

    bool has(OperationFlag flag) const { return m_flags & static_cast<uint16_t>(flag); }
    bool traps() const { return has(OperationFlag::Traps); }
    bool isChill() const { return has(OperationFlag::IsChill); }
    bool isNarrowReference() const { return has(OperationFlag::IsNarrowReference); }
    void markAsNarrowReference() { m_flags |= static_cast<uint16_t>(OperationFlag::IsNarrowReference); }

    unsigned useCount() const { return m_useCount; }
    static constexpr uint16_t opaqueReadFlags = static_cast<uint16_t>(OperationFlag::UpperHalfIsRead) | static_cast<uint16_t>(OperationFlag::HasOpaqueReader);

    ALWAYS_INLINE void gainUse(uint16_t flagsOfRead = opaqueReadFlags)
    {
        if (gainUseLeavingChildrenUncounted(flagsOfRead))
            countChildrenAtFirstUse();
    }
    JS_EXPORT_PRIVATE void loseUse();

    bool isConstant() const { return Machine::isConstant(m_opcode); }
    bool hasInt32() const { return m_opcode == Const32; }
    int32_t asInt32() const
    {
        ASSERT(hasInt32());
        return static_cast<int32_t>(m_payload.bits);
    }
    bool isInt32(int32_t value) const { return hasInt32() && asInt32() == value; }
    bool hasInt64() const { return m_opcode == Const64; }
    int64_t asInt64() const
    {
        ASSERT(hasInt64());
        return m_payload.bits;
    }
    bool isInt64(int64_t value) const { return hasInt64() && asInt64() == value; }
    bool hasInt() const { return hasInt32() || hasInt64(); }
    int64_t asInt() const
    {
        ASSERT(hasInt() && (!hasInt32() || m_payload.bits == asInt32()));
        return m_payload.bits;
    }
    bool isInt(int64_t value) const { return hasInt() && asInt() == value; }
    bool hasDouble() const { return m_opcode == ConstDouble; }
    double asDouble() const
    {
        ASSERT(hasDouble());
        return std::bit_cast<double>(m_payload.bits);
    }
    bool hasFloat() const { return m_opcode == ConstFloat; }
    float asFloat() const
    {
        ASSERT(hasFloat());
        return std::bit_cast<float>(static_cast<int32_t>(m_payload.bits));
    }
    int64_t constantBits() const
    {
        ASSERT(isConstant());
        return m_payload.bits;
    }

    int32_t offset() const
    {
        ASSERT(isMemoryAccess(m_opcode));
        return m_payload.memory.offset;
    }
    HeapIndex heap() const
    {
        ASSERT(isMemoryAccess(m_opcode));
        return m_payload.memory.heap;
    }
    Width accessWidth() const { return accessWidthOf(m_opcode, isStore(m_opcode) ? child(0)->type() : m_type); }

    Reg argumentReg() const
    {
        ASSERT(m_opcode == ArgumentReg);
        return Reg::fromIndex(static_cast<unsigned>(m_payload.bits));
    }
    bool isArgumentReg(Reg reg) const { return m_opcode == ArgumentReg && argumentReg() == reg; }
    StackSlot* slot() const
    {
        ASSERT(m_opcode == SlotBase);
        return m_payload.slot;
    }
    CodeSymbol symbol() const
    {
        ASSERT(namesSymbol(m_opcode));
        return { static_cast<SymbolSpace>(m_auxiliary), static_cast<uint32_t>(m_payload.bits), static_cast<uint16_t>(m_payload.bits >> 32) };
    }
    Operation* phi() const
    {
        ASSERT(m_opcode == Upsilon);
        return m_payload.phi;
    }
    Operation* valueHandedOn() const
    {
        ASSERT(m_opcode == Upsilon);
        return m_children[0];
    }
    unsigned extractedIndex() const
    {
        ASSERT(m_opcode == Extract);
        return static_cast<unsigned>(m_payload.bits);
    }
    const SwitchData& switchData() const
    {
        ASSERT(m_opcode == Switch);
        return *m_payload.switchData;
    }

    template<typename T> T* as() { return T::accepts(m_opcode) ? static_cast<T*>(this) : nullptr; }
    template<typename T> const T* as() const { return T::accepts(m_opcode) ? static_cast<const T*>(this) : nullptr; }

    inline bool mustStay() const;
    inline bool endsBlock() const;
    inline bool isUnconditionalJump() const;

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

protected:
    friend class Arena;
    friend class Builder;
    friend class Numbering;
    friend class Order;
    friend class PatchOperation;
    friend class Procedure;

    Operation(Opcode opcode, Type type, Origin origin)
        : m_opcode(opcode)
        , m_type(type)
        , m_origin(origin)
    {
    }

    Operation(Opcode opcode, Type type, Origin origin, uint32_t index, Block* owner, uint32_t indexInBlock, uint8_t numberOfChildren, Operation* first, Operation* second, Operation* third)
        : m_opcode(opcode)
        , m_auxiliary(numberOfChildren)
        , m_type(type)
        , m_index(index)
        , m_origin(origin)
        , m_owner(owner)
        , m_indexInBlock(indexInBlock)
        , m_children { first, second, third }
    {
    }

    ALWAYS_INLINE bool gainUseLeavingChildrenUncounted(uint16_t flagsOfRead = opaqueReadFlags)
    {
        m_flags |= flagsOfRead;
        if (m_useCount) {
            if (m_useCount != std::numeric_limits<uint16_t>::max())
                ++m_useCount;
            return false;
        }
        m_useCount = 1;
        return m_owner && countsChildrenAtFirstUse(m_opcode);
    }
    JS_EXPORT_PRIVATE void countChildrenAtFirstUse();
    void countChildrenAtFirstUseWithWorklist();
    static void countChildrenOfSeveralAtFirstUse(Operation* first, Operation* second, Operation* thirdOrNull);

    union Payload {
        int64_t bits;
        struct {
            int32_t offset;
            HeapIndex heap;
        } memory;
        StackSlot* slot;
        Operation* phi;
        const SwitchData* switchData;
    };

    Opcode m_opcode;
    uint8_t m_auxiliary { 0 };
    uint16_t m_flags { 0 };
    Type m_type;
    uint32_t m_index { 0 };
    Origin m_origin;
    Block* m_owner { nullptr };
    uint32_t m_indexInBlock { 0 };
    uint16_t m_useCount { 0 };
    Operation* m_children[maxInlineChildren] { nullptr, nullptr, nullptr };
    Payload m_payload { 0 };
};
static_assert(sizeof(Operation) == 64);

class PatchGenerator {
public:
    void run(CCallHelpers& jit, const PatchParameters& parameters) const { m_run(*this, jit, parameters); }

protected:
    using Function = void (*)(const PatchGenerator&, CCallHelpers&, const PatchParameters&);

    explicit PatchGenerator(Function run)
        : m_run(run)
    {
    }

private:
    Function m_run;
};

template<typename Functor>
class PatchGeneratorOf final : public PatchGenerator {
public:
    explicit PatchGeneratorOf(const Functor& functor)
        : PatchGenerator(runFunctor)
        , m_functor(functor)
    {
    }

    template<typename... Fields>
    explicit PatchGeneratorOf(std::in_place_t, Fields&&... fields)
        : PatchGenerator(runFunctor)
        , m_functor { std::forward<Fields>(fields)... }
    {
    }

private:
    static void runFunctor(const PatchGenerator& generator, CCallHelpers& jit, const PatchParameters& parameters)
    {
        static_cast<const PatchGeneratorOf&>(generator).m_functor(jit, parameters);
    }

    Functor m_functor;
};

class ResultConstraints {
public:
    unsigned size() const { return m_reps.size(); }
    bool isEmpty() const { return m_reps.isEmpty(); }
    const ValueRep& operator[](unsigned index) const { return m_reps[index]; }
    std::span<const ValueRep> span() const { return m_reps.span(); }

    void append(const ValueRep& rep) { m_reps.append(*m_arena, rep); }
    ResultConstraints& operator=(std::initializer_list<ValueRep> reps)
    {
        assign(std::span<const ValueRep>(reps.begin(), reps.size()));
        return *this;
    }
    template<typename VectorType>
    requires requires(const VectorType& vector) { vector.span(); }
    ResultConstraints& operator=(const VectorType& reps)
    {
        assign(reps.span());
        return *this;
    }

private:
    friend class PatchOperation;

    explicit ResultConstraints(Arena& arena)
        : m_arena(&arena)
    {
    }

    void assign(std::span<const ValueRep> reps)
    {
        m_reps.clear();
        if (reps.size() == 1) [[likely]] {
            m_reps.appendWithinCapacity(reps.front());
            return;
        }
        appendNoneOrSeveral(reps);
    }
    JS_EXPORT_PRIVATE void appendNoneOrSeveral(std::span<const ValueRep>);

    Arena* m_arena;
    ArenaVector<ValueRep> m_reps;
};

struct FactsOfSuspension {
    std::optional<BitSpan> frameWordsDeadAcrossSuspension;
    uint32_t operandEncodedAsFrameWordIndex { UINT32_MAX };
};
inline constexpr FactsOfSuspension factsOfPatchThatIsToldNothingOfSuspensions { };

class PatchOperation final : public Operation {
public:
    static bool accepts(Opcode opcode) { return opcode == Patch || opcode == Check; }

    Effects effects { Effects::forCall() };
    ResultConstraints resultConstraints;
    void holdResultsTo(const RegisterSet& set) { m_registersOfResults = maskOf(set); }
    RegisterMask registersOfResults() const { return m_registersOfResults; }
    uint8_t numGPScratchRegisters { 0 };
    uint8_t numFPScratchRegisters { 0 };
    bool onlyKeepsChildrenAlive { false };

private:
    uint16_t m_bits { 0 };

public:
    std::optional<PureFunction> pureFunction;

    void append(const ConstrainedValue& value) { append(value.value(), value.rep()); }
    inline void append(Operation*, const ValueRep&);
    void appendSomeRegister(Operation* value) { append(value, ValueRep::SomeRegister); }
    template<typename VectorType>
    void appendVector(const VectorType& vector)
    {
        for (const auto& value : vector)
            append(value);
    }

    JS_EXPORT_PRIVATE void setReads(std::span<const AbstractHeap* const>);
    JS_EXPORT_PRIVATE void setWrites(std::span<const AbstractHeap* const>);

    unsigned numberOfOperands() const { return m_operands.size(); }
    std::span<Operation* const> operands() const { return m_operands.span(); }
    const ValueRep& constraintOf(unsigned child) const { return m_constraints[child]; }

    void clobberEarly(const RegisterSet& set) { m_earlyClobbered |= maskOf(set); }
    void clobberLate(const RegisterSet& set) { m_lateClobbered |= maskOf(set); }
    void clobber(const RegisterSet& set)
    {
        RegisterMask mask = maskOf(set);
        m_earlyClobbered |= mask;
        m_lateClobbered |= mask;
    }
    void clobberEarly(RegisterMask mask) { m_earlyClobbered |= mask; }
    void clobberLate(RegisterMask mask) { m_lateClobbered |= mask; }
    RegisterSet earlyClobbered() const { return registerSetOf(m_earlyClobbered); }
    RegisterSet lateClobbered() const { return registerSetOf(m_lateClobbered); }
    RegisterMask earlyClobberedMask() const { return m_earlyClobbered; }
    RegisterMask lateClobberedMask() const { return m_lateClobbered; }

    JS_EXPORT_PRIVATE void clobberLateOnSuccessor(unsigned successorIndex, const RegisterSet&);
    JS_EXPORT_PRIVATE RegisterMask lateClobberedOnSuccessor(unsigned successorIndex) const;

    template<typename Functor> inline void setGenerator(const Functor&);
    template<typename Functor, typename... Fields> inline void makeGenerator(Fields&&...);
    void setGenerator(PatchGenerator* generator) { m_generator = generator; }
    PatchGenerator* generator() const { return m_generator; }

    void setRunsWithoutFrame(bool value) { setBit(RunsWithoutFrame, value); }
    bool runsWithoutFrame() const { return bit(RunsWithoutFrame); }
    void setShowsRegistersToCollector(bool value) { setBit(ShowsRegistersToCollector, value); }
    bool showsRegistersToCollector() const { return bit(ShowsRegistersToCollector); }
    void setIsLoopPoll(bool value) { setBit(IsLoopPoll, value); }
    bool isLoopPoll() const { return bit(IsLoopPoll); }
    void setFollowsFrameSetup(bool value) { setBit(FollowsFrameSetup, value); }
    bool followsFrameSetup() const { return bit(FollowsFrameSetup); }
    void setTestsStackAheadOfFramedPart(bool value) { setBit(TestsStackAheadOfFramedPart, value); }
    bool testsStackAheadOfFramedPart() const { return bit(TestsStackAheadOfFramedPart); }
    bool mustStandBehindFrameSetup() const { return followsFrameSetup() && !testsStackAheadOfFramedPart(); }
    void setLeavesByReturning(bool value) { setBit(LeavesByReturning, value); }
    bool leavesByReturning() const { return bit(LeavesByReturning); }
    void setNeedsFrameRecord(bool value) { setBit(NeedsFrameRecord, value); }
    void describeNewCall(bool needsFrameRecord, bool runsWithoutFrame)
    {
        ASSERT(!m_bits);
        m_bits = static_cast<uint16_t>(FindsOperandsInFrameWithoutRecord | needsFrameRecord * NeedsFrameRecord | runsWithoutFrame * RunsWithoutFrame);
    }
    bool needsFrameRecord() const { return bit(NeedsFrameRecord); }
    void setFindsOperandsInFrameWithoutRecord(bool value) { setBit(FindsOperandsInFrameWithoutRecord, value); }
    bool findsOperandsInFrameWithoutRecord() const { return bit(FindsOperandsInFrameWithoutRecord); }
    void setIsSuspension(bool value) { setBit(IsSuspension, value); }
    bool isSuspension() const { return bit(IsSuspension); }
    void setOperandEncodedAsFrameWordIndex(unsigned child) { ensureFactsOfSuspension().operandEncodedAsFrameWordIndex = child; }
    std::optional<unsigned> operandEncodedAsFrameWordIndex() const
    {
        uint32_t child = factsOfSuspension().operandEncodedAsFrameWordIndex;
        return child == UINT32_MAX ? std::nullopt : std::optional<unsigned>(child);
    }
    void setSharesLatePath(bool value) { setBit(SharesLatePath, value); }
    bool sharesLatePath() const { return bit(SharesLatePath); }
    bool failsWhenPredicateIsZero() const { return bit(FailsWhenPredicateIsZero); }
    void setClearsUpperHalf(bool value) { setBit(ClearsUpperHalf, value); }
    bool clearsUpperHalf() const { return bit(ClearsUpperHalf); }
    void setBringsOperandsToTheirRegisters(bool value) { setBit(BringsOperandsToTheirRegisters, value); }
    bool bringsOperandsToTheirRegisters() const { return bit(BringsOperandsToTheirRegisters); }
    JS_EXPORT_PRIVATE void leaveOperandsWhereTheyLie();
    void setRegisterThatCalleeFillsOutOfResult(Reg reg) { m_registerThatCalleeFillsOutOfResult = reg; }
    Reg registerThatCalleeFillsOutOfResult() const { return m_registersToBringOperandsTo.isEmpty() ? m_registerThatCalleeFillsOutOfResult : Reg(); }
    void setRegisterThatCalleeMovesIntoResult(Reg reg) { m_registerThatCalleeMovesIntoResult = reg; }
    Reg registerThatCalleeMovesIntoResult() const { return m_registerThatCalleeMovesIntoResult; }
    void setLeavesMoveOutOfResultToCallee(bool value) { setBit(LeavesMoveOutOfResultToCallee, value); }
    bool leavesMoveOutOfResultToCallee() const { return bit(LeavesMoveOutOfResultToCallee); }

    Reg registerToBringOperandTo(unsigned child) const { return child < m_registersToBringOperandsTo.size() ? m_registersToBringOperandsTo[child] : Reg(); }
    const std::optional<BitSpan>& frameWordsDeadAcrossSuspension() const { return factsOfSuspension().frameWordsDeadAcrossSuspension; }

private:
    friend class Allocator;
    friend class Arena;
    friend class Builder;
    friend class Operation;
    friend class SlotAssigner;

    enum Bit : uint16_t {
        RunsWithoutFrame = 1 << 0,
        FollowsFrameSetup = 1 << 1,
        NeedsFrameRecord = 1 << 2,
        IsSuspension = 1 << 3,
        IsLoopPoll = 1 << 4,
        SharesLatePath = 1 << 5,
        LeavesByReturning = 1 << 6,
        BringsOperandsToTheirRegisters = 1 << 7,
        FindsOperandsInFrameWithoutRecord = 1 << 8,
        LeavesMoveOutOfResultToCallee = 1 << 9,
        TestsStackAheadOfFramedPart = 1 << 10,
        FailsWhenPredicateIsZero = 1 << 11,
        ShowsRegistersToCollector = 1 << 12,
        ClearsUpperHalf = 1 << 13,
    };

    struct SuccessorClobber {
        unsigned successorIndex;
        RegisterMask mask;
    };

    static constexpr uint32_t firstCapacityOfOperands = 4;
    static constexpr size_t bytesOfFirstConstraints = firstCapacityOfOperands * sizeof(ValueRep);
    static constexpr size_t bytesOfFirstOperands = firstCapacityOfOperands * sizeof(Operation*);
    static constexpr size_t bytesOfFirstStorage = bytesOfFirstConstraints + bytesOfFirstOperands + sizeof(ValueRep);

    static constexpr size_t bytesOfNewPatch() { return sizeof(PatchOperation) + bytesOfFirstStorage; }
    static inline PatchOperation* createAt(void* memory, Procedure&, Opcode, Type, Origin);
    static inline PatchOperation* create(Procedure&, Opcode, Type, Origin);
    inline PatchOperation(Procedure&, Opcode, Type, Origin);

    JS_EXPORT_PRIVATE void growStorageOfOperands();
    JS_EXPORT_PRIVATE FactsOfSuspension& ensureFactsOfSuspension();
    const FactsOfSuspension& factsOfSuspension() const { return m_factsOfSuspension ? *m_factsOfSuspension : factsOfPatchThatIsToldNothingOfSuspensions; }

    bool bit(Bit bit) const { return m_bits & bit; }
    void setBit(Bit bit, bool value) { m_bits = value ? m_bits | bit : m_bits & ~bit; }
    void setFailsWhenPredicateIsZero(bool value) { setBit(FailsWhenPredicateIsZero, value); }

    Procedure& m_procedure;
    ArenaVector<Operation*> m_operands;
    ArenaVector<ValueRep> m_constraints;
    ArenaVector<SuccessorClobber> m_successorClobbers;
    ArenaVector<Reg> m_registersToBringOperandsTo;
    Reg m_registerThatCalleeFillsOutOfResult;
    Reg m_registerThatCalleeMovesIntoResult;
    RegisterMask m_earlyClobbered { 0 };
    RegisterMask m_lateClobbered { 0 };
    RegisterMask m_registersOfResults { ~static_cast<RegisterMask>(0) };
    PatchGenerator* m_generator { nullptr };
    FactsOfSuspension* m_factsOfSuspension { nullptr };
};

ALWAYS_INLINE void PatchOperation::append(Operation* value, const ValueRep& rep)
{
    ASSERT(value->type().isNumeric());
    ASSERT(rep.isAny() || rep.kind() == ValueRep::SomeRegister || rep.kind() == ValueRep::SomeLateRegister || rep.kind() == ValueRep::Register || rep.kind() == ValueRep::StackArgument);
    ASSERT(m_opcode != Check || !m_operands.isEmpty() || rep.kind() == ValueRep::ColdAny);
    RELEASE_ASSERT(value->hasNoPosition() || value->owner() != m_owner || value->opcode() == Phi || value->indexInBlock() < m_indexInBlock);

    if (m_operands.size() == m_operands.capacity()) [[unlikely]]
        growStorageOfOperands();
    m_operands.appendWithinCapacity(value);
    m_constraints.appendWithinCapacity(rep);

    if (m_owner)
        value->gainUse();
}

inline unsigned Operation::numChildren() const
{
    if (const PatchOperation* patch = as<PatchOperation>())
        return patch->m_operands.size();
    return namesSymbol(m_opcode) ? 0 : m_auxiliary;
}

inline std::span<Operation* const> Operation::children() const
{
    if (const PatchOperation* patch = as<PatchOperation>())
        return patch->m_operands.span();
    return std::span<Operation* const>(m_children, numChildren());
}

inline Operation* Operation::child(unsigned index) const
{
    if (const PatchOperation* patch = as<PatchOperation>())
        return patch->m_operands[index];
    ASSERT(index < numChildren());
    return std::span<Operation* const, maxInlineChildren>(m_children)[index];
}

inline bool Operation::mustStay() const
{
    if (m_opcode == Patch)
        return static_cast<const PatchOperation*>(this)->effects.mustStay();
    return hasTrait(m_opcode, OpcodeTrait::HasEffects) || traps();
}

inline bool Operation::endsBlock() const
{
    if (m_opcode == Patch)
        return static_cast<const PatchOperation*>(this)->effects.terminal;
    return isTerminal(m_opcode);
}

class StackSlot {
    WTF_MAKE_NONCOPYABLE(StackSlot);
public:
    unsigned index() const { return m_index; }
    unsigned byteSize() const { return m_byteSize; }
    void ensureSize(unsigned byteSize) { m_byteSize = std::max(m_byteSize, byteSize); }
    void setIsDeadAtSuspensions(bool value) { m_isDeadAtSuspensions = value; }
    bool isDeadAtSuspensions() const { return m_isDeadAtSuspensions; }
    intptr_t offsetFromFP() const
    {
        ASSERT(m_offsetFromFP);
        return m_offsetFromFP;
    }

private:
    friend class Arena;
    friend class Procedure;
    friend Frame* planFrame(Procedure&);

    StackSlot(unsigned index, unsigned byteSize)
        : m_index(index)
        , m_byteSize(byteSize)
    {
    }

    unsigned m_index;
    unsigned m_byteSize;
    bool m_isDeadAtSuspensions { false };
    intptr_t m_offsetFromFP { 0 };
};

class Block {
    WTF_MAKE_NONCOPYABLE(Block);
public:
    static constexpr uint32_t notOrdered = UINT32_MAX;

    uint32_t index() const { return m_index; }
    Procedure& procedure() const { return m_procedure; }

    unsigned size() const { return m_operations.size(); }
    bool isEmpty() const { return m_operations.isEmpty(); }
    Operation* at(unsigned index) const { return m_operations[index]; }
    Operation* last() const { return m_operations.last(); }
    std::span<Operation* const> operations() const { return m_operations.span(); }
    bool isTerminated() const { return !isEmpty() && last()->endsBlock(); }

    std::span<Operation* const> phis() const { return m_phis.span(); }
    std::span<Operation* const> upsilons() const { return m_upsilons.span(); }

    unsigned numSuccessors() const { return m_successors.size(); }
    const Edge& successor(unsigned index) const { return m_successors[index]; }
    std::span<const Edge> successors() const { return m_successors.span(); }
    inline void appendSuccessor(Edge);
    JS_EXPORT_PRIVATE void setSuccessors(Edge);
    JS_EXPORT_PRIVATE void setSuccessors(Edge taken, Edge notTaken);
    JS_EXPORT_PRIVATE void clearSuccessors();

    unsigned numberOfIncomingEdges() const { return m_numberOfIncomingEdges; }
    Block* singlePredecessor() const { return m_numberOfIncomingEdges == 1 ? std::bit_cast<Block*>(m_sourcesOfIncomingEdgesFolded) : nullptr; }
    Block* otherPredecessor(const Block* one) const { return m_numberOfIncomingEdges == 2 ? std::bit_cast<Block*>(m_sourcesOfIncomingEdgesFolded ^ std::bit_cast<uintptr_t>(one)) : nullptr; }
    EdgeClass classOfSingleIncomingEdge() const
    {
        ASSERT(m_numberOfIncomingEdges == 1);
        return static_cast<EdgeClass>(m_classesOfIncomingEdgesFolded);
    }
    bool hasOnlyOnePredecessor() const { return m_hasOnlyOnePredecessor; }
    bool writesMemory() const { return m_writesMemory; }
    bool isLandingPad() const { return m_isLandingPad; }
    bool isEntry() const { return m_isEntry; }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    friend class Arena;
    friend class Builder;
    friend class Order;
    friend class PatchOperation;
    friend class Procedure;

    Block(Procedure& procedure, uint32_t index)
        : m_procedure(procedure)
        , m_index(index)
    {
    }

    static constexpr uint32_t firstCapacityOfOperations = 8;
    static constexpr uint32_t firstCapacityOfPhis = 4;
    static constexpr uint32_t firstCapacityOfUpsilons = 2;
    static constexpr uint32_t firstCapacityOfSuccessors = 2;

    template<bool hasRoom> inline void appendSuccessorWhereRoomIsKnown(Edge);
    inline void appendSuccessorInline(Edge);
    inline void setSuccessorsInline(Edge);
    inline void setSuccessorsInline(Edge taken, Edge notTaken);
    bool mayBeAppendedTo() const { return !m_lastOperationMayEndBlock || !isTerminated(); }

    void didGainIncomingEdge(const Block* from, EdgeClass edgeClass)
    {
        RELEASE_ASSERT(!m_isEntry);
        if (m_numberOfIncomingEdges)
            m_hasOnlyOnePredecessor = false;
        m_sourcesOfIncomingEdgesFolded ^= std::bit_cast<uintptr_t>(from);
        m_classesOfIncomingEdgesFolded ^= static_cast<uint8_t>(edgeClass);
        ++m_numberOfIncomingEdges;
    }

    void didLoseIncomingEdge(const Block* from, EdgeClass edgeClass)
    {
        ASSERT(m_numberOfIncomingEdges);
        m_sourcesOfIncomingEdgesFolded ^= std::bit_cast<uintptr_t>(from);
        m_classesOfIncomingEdgesFolded ^= static_cast<uint8_t>(edgeClass);
        --m_numberOfIncomingEdges;
    }

    void didChangeClassOfIncomingEdge(EdgeClass before, EdgeClass behind)
    {
        m_classesOfIncomingEdgesFolded ^= static_cast<uint8_t>(before) ^ static_cast<uint8_t>(behind);
    }

    Procedure& m_procedure;
    uint32_t m_index;
    uint32_t m_numberOfIncomingEdges { 0 };
    uintptr_t m_sourcesOfIncomingEdgesFolded { 0 };
    uint8_t m_classesOfIncomingEdgesFolded { 0 };
    bool m_hasOnlyOnePredecessor { false };
    bool m_isLandingPad { false };
    bool m_isEntry { false };
    bool m_lastOperationMayEndBlock { false };
    bool m_writesMemory { false };
    ArenaVector<Operation*> m_operations;
    ArenaVector<Operation*> m_phis;
    ArenaVector<Operation*> m_upsilons;
    ArenaVector<Edge> m_successors;
};

inline bool Operation::isUnconditionalJump() const
{
    return m_opcode == Jump || (m_opcode == Branch && m_owner->numSuccessors() == 1);
}

struct ProcedureTraits {
    bool savesFrameOnSuspend { false };
    bool hasHandlers { false };
    bool rejectsOnThrow { false };
    bool savesRegistersOfCallers { false };
    bool usesNarrowReferences { true };
    bool fallsIntoWhatLiesBehind { false };
    bool chargesForCalleeSaves { false };
    bool mayOmitFrameRecord { false };
    bool mayJumpOverUnlikelySides { false };
    bool checksAllocation { false };
    bool shouldUseFallbackBlockOrder { false };
    bool shouldDropPatchOnlySaves { false };
    bool shouldAssumeSpillSlotsDead { false };
    bool shouldAssumeClearUpperHalves { false };
    bool shouldIgnoreCollectorSpillReads { false };
    bool notesRangesOfOrigins { false };
    CPUFeatureSet features;

    bool setsUpFrameAtEntry() const { return savesFrameOnSuspend || hasHandlers || rejectsOnThrow; }
};

inline constexpr unsigned maxBytesAllocatedBeforeStackCheck = 4 * KB;

struct StackCheckSite {
    unsigned bytesBelowFrameRecord { 0 };
    bool isAllocated { false };
    bool nothingCountsOnCheck { false };
};

using StackCheckGenerator = void (*)(void* context, CCallHelpers&, const Frame&, const StackCheckSite&);

struct ConstantDataRecord {
    std::span<const uint8_t> bytes;
    uint32_t alignment;
    ConstantKind kind;
};

struct Entrypoint {
    uint16_t id;
    Block* block;
    MacroAssembler::Label label;
};

struct JumpTableRecord {
    std::span<MacroAssembler::Label> targets;
};

struct TrappingAccess {
    MacroAssembler::Label instruction;
    Origin origin;
    GPRReg reference;
    bool referenceIsNarrow;
};

struct FrameRange {
    MacroAssembler::Label begin;
    MacroAssembler::Label end;
};

struct ShortLoop {
    MacroAssembler::Label begin;
    MacroAssembler::Label end;
};

struct RangeOfRareCode {
    MacroAssembler::Label begin;
    MacroAssembler::Label end;
};

struct RangeOfOrigin {
    MacroAssembler::Label begin;
    MacroAssembler::Label end;
    Origin origin;
    uint32_t operation { UINT32_MAX };
};

enum class FailureKind : uint8_t {
    MalformedProcedure,
    OutOfRegisters,
    AccessOutsideDeclaredEffects,
    WrongValueInRegister,
    WrongValueInSpillSlot,
    FrameAddressAcrossSuspension,
    NarrowReferenceWithoutPointer,
    InnerPointerWithoutOwner,
    MissingFrameSetup,
    UpperHalfIsNotClear,
};

struct PlaceOfCheck {
    static constexpr PlaceOfCheck here(const char* file = __builtin_FILE_NAME(), const char* function = __builtin_FUNCTION(), uint32_t line = __builtin_LINE()) { return { file, function, line }; }

    const char* file { nullptr };
    const char* function { nullptr };
    uint32_t line { 0 };
};

struct Failure {
    FailureKind kind;
    Origin origin;
    uint32_t operation { UINT32_MAX };
    PlaceOfCheck check;
};

class Procedure {
    WTF_MAKE_NONCOPYABLE(Procedure);
public:
    JS_EXPORT_PRIVATE Procedure(Arena&, const HeapTree&, const ProcedureTraits&);

    COLD JS_EXPORT_PRIVATE void fail(FailureKind, const Operation* = nullptr, PlaceOfCheck = PlaceOfCheck::here());
    const std::optional<Failure>& failure() const { return m_failure; }

    Arena& arena() { return m_arena; }
    const HeapTree& heapTree() const { return m_heapTree; }
    const ProcedureTraits& traits() const { return m_traits; }
    bool checksAllocation() const { return m_traits.checksAllocation || m_orderWasMadeAgain; }
    void noteThatOrderWasMadeAgain() { m_orderWasMadeAgain = true; }
    void noteVectors() { m_holdsVectors = true; }
    void noteLoopPoll(PatchOperation* poll)
    {
        poll->setIsLoopPoll(true);
        m_loopPolls.append(m_arena, poll);
    }
    std::span<PatchOperation* const> loopPolls() const { return m_loopPolls.span(); }
    bool holdsVectors() const { return m_holdsVectors; }

    JS_EXPORT_PRIVATE Block* addBlock();
    Block* addBlockThatOnlyJumps();
    unsigned numberOfBlocks() const { return m_blocks.size(); }
    Block* blockAt(unsigned index) const { return m_blocks[index]; }
    std::span<Block* const> blocks() const { return m_blocks.span(); }
    std::span<Block* const> blocksBegunBeforeAnythingLedToThem() const { return m_blocksBegunBeforeAnythingLedToThem.span(); }
    COLD void stopAtStoreOverLoadThatStands(const Operation* store);
    COLD void stopAtStoresThatWayLeadsToAfterAll();
    bool keepsStoresToLookAtAgain() const { return !m_storesOverLoadsWhereNoWayLed.isEmpty(); }

    ALWAYS_INLINE void noteWordName(Operation* extension);
    std::span<Operation* const> namesOfWords() const { return m_namesOfWords.span(); }
    void noteOwnShiftOfSum(Operation* sum, Operation* shift) { m_ownShiftsOfSums.append(m_arena, { sum, shift }); }
    void separateShiftsOfSums();
    ALWAYS_INLINE void noteUpperHalfHandedOn(Operation* upsilon);
    void settleUpperHalvesOfPhis();

    JS_EXPORT_PRIVATE Block* addEntrypoint(uint16_t id);
    std::span<const Entrypoint> entrypoints() const { return m_entrypoints.span(); }
    MacroAssembler::Label entrypointLabel(unsigned index) const { return m_entrypoints[index].label; }

    unsigned numberOfOperations() const { return m_operations.size(); }
    Operation* operationAt(uint32_t index) const { return m_operations[index]; }
    bool holdsIntegersAddedTo() const { return m_holdsIntegersAddedTo; }
    bool namesFramePointer() const { return m_namesFramePointer; }

    JS_EXPORT_PRIVATE Type addTuple(std::span<const Type>);
    Type addTuple(std::initializer_list<Type> types) { return addTuple(std::span<const Type>(types.begin(), types.size())); }
    std::span<const Type> tupleOf(Type type) const { return m_tuples[type.tupleIndex()]; }
    unsigned numberOfTuples() const { return m_tuples.size(); }
    unsigned resultCount(Type type) const { return type.isTuple() ? static_cast<unsigned>(tupleOf(type).size()) : type == Void ? 0 : 1; }
    Type resultType(Type type, unsigned index) const { return type.isTuple() ? tupleOf(type)[index] : type; }

    JS_EXPORT_PRIVATE StackSlot* addStackSlot(unsigned byteSize);
    std::span<StackSlot* const> stackSlots() const { return m_stackSlots.span(); }

    void requestCallArgAreaSizeInBytes(unsigned size) { m_callArgAreaSize = std::max(m_callArgAreaSize, size); }
    unsigned callArgAreaSizeInBytes() const { return m_callArgAreaSize; }

    JS_EXPORT_PRIVATE uint32_t addConstantData(std::span<const uint8_t>, uint32_t alignment, ConstantKind);
    std::span<const ConstantDataRecord> constantData() const { return m_constantData.span(); }

    JS_EXPORT_PRIVATE HeapSet heapSet(std::span<const AbstractHeap* const>);
    HeapSet heapSet(const AbstractHeap& heap)
    {
        const AbstractHeap* pointer = &heap;
        return heapSet(std::span<const AbstractHeap* const>(&pointer, 1));
    }

    template<typename Functor>
    PatchGenerator* createGenerator(const Functor& functor) { return m_arena.create<PatchGeneratorOf<Functor>>(functor); }
    template<typename Functor, typename... Fields>
    PatchGenerator* makeGenerator(Fields&&... fields) { return m_arena.create<PatchGeneratorOf<Functor>>(std::in_place, std::forward<Fields>(fields)...); }

    void setStackCheckGenerator(StackCheckGenerator generator, void* context)
    {
        m_stackCheckGenerator = generator;
        m_stackCheckContext = context;
    }
    StackCheckGenerator stackCheckGenerator() const { return m_stackCheckGenerator; }
    void* stackCheckContext() const { return m_stackCheckContext; }
    void setLargestFrameSetUpWithoutStackCheck(unsigned bytes) { m_largestFrameSetUpWithoutStackCheck = bytes; }
    unsigned largestFrameSetUpWithoutStackCheck() const { return m_largestFrameSetUpWithoutStackCheck; }

    const Order& order() const { return *m_order; }
    const Liveness& liveness() const { return *m_liveness; }
    const Allocation& allocation() const { return *m_allocation; }
    const SlotAssignment& slots() const { return *m_slots; }
    const Frame& frame() const { return *m_frame; }
    Allocation& allocationForTesting() { return *m_allocation; }
    Frame& frameForTesting() { return *m_frame; }

    std::span<const JumpTableRecord> jumpTables() const { return m_jumpTables.span(); }
    std::span<const TrappingAccess> trappingAccesses() const { return m_trappingAccesses.span(); }
    std::span<const FrameRange> frameRanges() const { return m_frameRanges.span(); }
    std::span<const RangeOfOrigin> rangesOfOrigins() const { return m_rangesOfOrigins.span(); }
    uint32_t numberOfOperationsEncoded() const { return m_numberOfOperationsEncoded; }
    uint32_t switchBitTestCount() const { return m_switchBitTestCount; }
    uint32_t numberOfInnerPointersAcrossSafepoints() const { return m_numberOfInnerPointersAcrossSafepoints; }
    void noteInnerPointerAcrossSafepoint() { ++m_numberOfInnerPointersAcrossSafepoints; }
    uint32_t numberOfTrapsTakenOver() const { return m_numberOfTrapsTakenOver; }
    MacroAssembler::Label rareCodeStart() const { return m_rareCodeStart; }
    std::span<const RangeOfRareCode> rangesOfRareCodeAmongOtherCode() const { return m_rangesOfRareCodeAmongOtherCode.span(); }
    std::span<const ShortLoop> shortLoops() const { return m_shortLoops.span(); }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    friend class Builder;
    friend class Encoder;
    friend class Numbering;
    friend class Order;
    friend void prepareForGeneration(Procedure&);

    void didCreate(Operation* operation)
    {
        operation->m_index = m_operations.size();
        m_operations.append(m_arena, operation);
    }
    template<uint32_t capacityOfOperations> inline Block* addBlockWithRoomForOperations();
    inline Block* addBlockInline();

    enum class ListThatBuildingLeaves : uint8_t {
        NumberedOperations,
        BucketsOfNumberedOperations,
        OperationsWithoutPosition,
        BucketsWithoutPosition,
        EpochsOfInheritedFinds,
    };
    static constexpr unsigned numberOfListsThatBuildingLeaves = 5;

    struct RoomThatBuildingLeaves {
        void* room { nullptr };
        size_t bytes { 0 };
    };
    using RoomsThatBuildingLeaves = std::array<RoomThatBuildingLeaves, numberOfListsThatBuildingLeaves>;

    template<typename T>
    void noteListThatBuildingLeaves(ListThatBuildingLeaves list, ArenaVector<T>& held)
    {
        size_t bytes = static_cast<size_t>(held.capacity()) * sizeof(T);
        if (bytes >= Arena::smallestRoomLetGo)
            noteRoomThatBuildingLeaves(list, held.begin(), bytes);
    }
    void noteRoomThatBuildingLeaves(ListThatBuildingLeaves, void* room, size_t bytes);
    void letGoOfWhatBuildingLeaves();

    struct ListOfHeapsWithItsSet {
        std::span<const AbstractHeap* const> list;
        HeapSet set;
    };

    std::span<const HeapIndex> heapsKeptOf(std::span<const AbstractHeap* const>);

    Arena& m_arena;
    const HeapTree& m_heapTree;
    const ProcedureTraits m_traits;
    bool m_orderWasMadeAgain { false };
    bool m_holdsVectors { false };
    std::array<ListOfHeapsWithItsSet, 4> m_lastListsOfHeaps { };
    MachineCoreListsOfHeapsKept* m_listsOfHeapsKept { nullptr };
    unsigned m_numberOfListsOfHeapsKept { 0 };
    ArenaVector<Block*> m_blocks;
    ArenaVector<Block*> m_blocksBegunBeforeAnythingLedToThem;
    ArenaVector<PatchOperation*> m_loopPolls;
    ArenaVector<const Operation*> m_storesOverLoadsWhereNoWayLed;
    ArenaVector<Operation*> m_operations;
    ArenaVector<Operation*> m_upsilonsOfWordsBetweenPhis;
    ArenaVector<Operation*> m_namesOfWords;
    ArenaVector<std::pair<Operation*, Operation*>> m_ownShiftsOfSums;
    ArenaVector<Entrypoint> m_entrypoints;
    ArenaVector<std::span<const Type>> m_tuples;
    ArenaVector<StackSlot*> m_stackSlots;
    ArenaVector<ConstantDataRecord> m_constantData;
    ArenaVector<uint32_t> m_constantDataBuckets;
    RoomsThatBuildingLeaves* m_roomsThatBuildingLeaves { nullptr };
    unsigned m_callArgAreaSize { 0 };
    bool m_holdsIntegersAddedTo { false };
    bool m_namesFramePointer { false };
    StackCheckGenerator m_stackCheckGenerator { nullptr };
    void* m_stackCheckContext { nullptr };
    unsigned m_largestFrameSetUpWithoutStackCheck { 0 };

    Order* m_order { nullptr };
    Liveness* m_liveness { nullptr };
    Allocation* m_allocation { nullptr };
    SlotAssignment* m_slots { nullptr };
    Frame* m_frame { nullptr };

    ArenaVector<JumpTableRecord> m_jumpTables;
    ArenaVector<TrappingAccess> m_trappingAccesses;
    ArenaVector<FrameRange> m_frameRanges;
    ArenaVector<RangeOfOrigin> m_rangesOfOrigins;
    uint32_t m_numberOfOperationsEncoded { 0 };
    uint32_t m_switchBitTestCount { 0 };
    uint32_t m_numberOfInnerPointersAcrossSafepoints { 0 };
    uint32_t m_numberOfTrapsTakenOver { 0 };
    MacroAssembler::Label m_rareCodeStart;
    ArenaVector<RangeOfRareCode> m_rangesOfRareCodeAmongOtherCode;
    ArenaVector<ShortLoop> m_shortLoops;
    std::optional<Failure> m_failure;
};

template<uint32_t capacityOfOperations>
ALWAYS_INLINE Block* Procedure::addBlockWithRoomForOperations()
{
    static_assert(std::is_trivially_destructible_v<Block>);
    static_assert(alignof(Block) >= alignof(Edge) && alignof(Edge) >= alignof(Operation*) && !(sizeof(Block) % alignof(Edge)));
    constexpr size_t bytesOfSuccessors = Block::firstCapacityOfSuccessors * sizeof(Edge);
    constexpr size_t bytesOfOperations = capacityOfOperations * sizeof(Operation*);
    constexpr size_t bytesOfUpsilons = Block::firstCapacityOfUpsilons * sizeof(Operation*);
    uintptr_t memory = reinterpret_cast<uintptr_t>(m_arena.allocate(sizeof(Block) + bytesOfSuccessors + bytesOfOperations + bytesOfUpsilons, alignof(Block)));
    Block* block = new (NotNull, reinterpret_cast<void*>(memory)) Block(*this, m_blocks.size());
    block->m_successors.adoptStorage(reinterpret_cast<Edge*>(memory + sizeof(Block)), Block::firstCapacityOfSuccessors);
    block->m_operations.adoptStorage(reinterpret_cast<Operation**>(memory + sizeof(Block) + bytesOfSuccessors), capacityOfOperations);
    block->m_upsilons.adoptStorage(reinterpret_cast<Operation**>(memory + sizeof(Block) + bytesOfSuccessors + bytesOfOperations), Block::firstCapacityOfUpsilons);
    m_blocks.append(m_arena, block);
    return block;
}

ALWAYS_INLINE Block* Procedure::addBlockInline()
{
    return addBlockWithRoomForOperations<Block::firstCapacityOfOperations>();
}

template<typename Functor>
inline void PatchOperation::setGenerator(const Functor& functor)
{
    m_generator = m_procedure.createGenerator(functor);
}

template<typename Functor, typename... Fields>
inline void PatchOperation::makeGenerator(Fields&&... fields)
{
    m_generator = m_procedure.makeGenerator<Functor>(std::forward<Fields>(fields)...);
}

inline PatchOperation::PatchOperation(Procedure& procedure, Opcode opcode, Type type, Origin origin)
    : Operation(opcode, type, origin)
    , resultConstraints(procedure.arena())
    , m_procedure(procedure)
{
    ASSERT(accepts(opcode));
    ASSERT(opcode == Patch || type == Void);

    uintptr_t storage = reinterpret_cast<uintptr_t>(this) + sizeof(PatchOperation);
    m_constraints.adoptStorage(reinterpret_cast<ValueRep*>(storage), firstCapacityOfOperands);
    m_operands.adoptStorage(reinterpret_cast<Operation**>(storage + bytesOfFirstConstraints), firstCapacityOfOperands);
    resultConstraints.m_reps.adoptStorage(reinterpret_cast<ValueRep*>(storage + bytesOfFirstConstraints + bytesOfFirstOperands), 1);
    if (type.isNumeric())
        resultConstraints.m_reps.appendWithinCapacity(ValueRep::SomeRegister);
}

inline PatchOperation* PatchOperation::createAt(void* memory, Procedure& procedure, Opcode opcode, Type type, Origin origin)
{
    static_assert(std::is_trivially_destructible_v<PatchOperation>);
    static_assert(alignof(PatchOperation) >= alignof(ValueRep) && alignof(ValueRep) >= alignof(Operation*));
    static_assert(!(sizeof(PatchOperation) % alignof(ValueRep)) && !((bytesOfFirstConstraints + bytesOfFirstOperands) % alignof(ValueRep)));
    return new (NotNull, memory) PatchOperation(procedure, opcode, type, origin);
}

inline PatchOperation* PatchOperation::create(Procedure& procedure, Opcode opcode, Type type, Origin origin)
{
    return createAt(procedure.arena().allocate(bytesOfNewPatch(), alignof(PatchOperation)), procedure, opcode, type, origin);
}

template<bool hasRoom>
ALWAYS_INLINE void Block::appendSuccessorWhereRoomIsKnown(Edge edge)
{
    Block* target = edge.block();
    RELEASE_ASSERT(target);
    ASSERT(&target->m_procedure == &m_procedure);
    uint32_t numberOfIncomingEdges = target->m_numberOfIncomingEdges;
    RELEASE_ASSERT(!target->m_isEntry && (!target->m_hasOnlyOnePredecessor || !numberOfIncomingEdges));
    if constexpr (hasRoom)
        m_successors.appendWithinCapacity(edge);
    else
        m_successors.append(m_procedure.arena(), edge);
    target->m_numberOfIncomingEdges = numberOfIncomingEdges + 1;
    target->m_sourcesOfIncomingEdgesFolded ^= std::bit_cast<uintptr_t>(this);
    target->m_classesOfIncomingEdgesFolded ^= static_cast<uint8_t>(edge.edgeClass());
}

ALWAYS_INLINE void Block::appendSuccessorInline(Edge edge)
{
    appendSuccessorWhereRoomIsKnown<false>(edge);
}

ALWAYS_INLINE void Block::appendSuccessor(Edge edge)
{
    appendSuccessorInline(edge);
}

ALWAYS_INLINE void Block::setSuccessorsInline(Edge target)
{
    static_assert(firstCapacityOfSuccessors >= 1);
    if (!m_successors.isEmpty()) [[unlikely]]
        clearSuccessors();
    appendSuccessorWhereRoomIsKnown<true>(target);
}

ALWAYS_INLINE void Block::setSuccessorsInline(Edge taken, Edge notTaken)
{
    static_assert(firstCapacityOfSuccessors >= 2);
    if (!m_successors.isEmpty()) [[unlikely]]
        clearSuccessors();
    appendSuccessorWhereRoomIsKnown<true>(taken);
    appendSuccessorWhereRoomIsKnown<true>(notTaken);
}

class PatchParameters {
    WTF_MAKE_NONCOPYABLE(PatchParameters);
public:
    PatchOperation* value() const { return m_value; }

    unsigned size() const { return static_cast<unsigned>(m_reps.size()); }
    const ValueRep& at(unsigned index) const { return m_reps[index]; }
    const ValueRep& operator[](unsigned index) const { return at(index); }
    const ValueRep* begin() const { return m_reps.data(); }
    const ValueRep* end() const { return m_reps.data() + m_reps.size(); }

    RegisterSet usedRegisters() const { return registerSetOf(m_usedRegisters); }
    RegisterMask usedRegistersMask() const { return m_usedRegisters; }
    JS_EXPORT_PRIVATE RegisterSet unavailableRegisters() const;

    GPRReg gpScratch(unsigned index) const { return m_gpScratch[index]; }
    FPRReg fpScratch(unsigned index) const { return m_fpScratch[index]; }

    std::span<MacroAssembler::Label* const> successorLabels() const { return m_successorLabels; }
    JS_EXPORT_PRIVATE bool fallsThroughToSuccessor(unsigned successorIndex) const;

    bool frameIsSetUp() const { return m_frameIsSetUp; }
    JS_EXPORT_PRIVATE const Frame& frame() const;
    JS_EXPORT_PRIVATE Procedure& proc() const;
    Origin origin() const { return m_value->origin(); }

    template<typename Functor> inline void addLatePath(const Functor&) const;

private:
    friend class Encoder;

    PatchParameters(Encoder& encoder, PatchOperation* value)
        : m_encoder(encoder)
        , m_value(value)
    {
    }

    static constexpr unsigned maxScratchRegisters = 4;

    Encoder& m_encoder;
    PatchOperation* m_value;
    std::span<const ValueRep> m_reps;
    std::span<MacroAssembler::Label* const> m_successorLabels;
    RegisterMask m_usedRegisters { 0 };
    GPRReg m_gpScratch[maxScratchRegisters] { InvalidGPRReg, InvalidGPRReg, InvalidGPRReg, InvalidGPRReg };
    FPRReg m_fpScratch[maxScratchRegisters] { InvalidFPRReg, InvalidFPRReg, InvalidFPRReg, InvalidFPRReg };
    bool m_frameIsSetUp { false };
};

class LatePath {
public:
    void run(CCallHelpers& jit) const { m_run(*this, jit); }

protected:
    using Function = void (*)(const LatePath&, CCallHelpers&);

    explicit LatePath(Function run)
        : m_run(run)
    {
    }

private:
    Function m_run;
};

template<typename Functor>
class LatePathOf final : public LatePath {
public:
    explicit LatePathOf(const Functor& functor)
        : LatePath(runFunctor)
        , m_functor(functor)
    {
    }

private:
    static void runFunctor(const LatePath& path, CCallHelpers& jit) { static_cast<const LatePathOf&>(path).m_functor(jit); }

    Functor m_functor;
};

JS_EXPORT_PRIVATE void addLatePath(Encoder&, LatePath*);

template<typename Functor>
inline void PatchParameters::addLatePath(const Functor& functor) const
{
    Machine::addLatePath(m_encoder, proc().arena().create<LatePathOf<Functor>>(functor));
}

JS_EXPORT_PRIVATE void prepareForGeneration(Procedure&);
JS_EXPORT_PRIVATE void generate(Procedure&, CCallHelpers&, RelocationRecorder&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
