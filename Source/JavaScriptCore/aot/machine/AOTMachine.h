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

// The back end. A machine operation is touched four times: appended (folded and numbered on the way), scanned backward, allocated, encoded.
//
//     Builder (AOTMachineBuilder.h)                    append     emitters, through Output
//     orderBlocks() (AOTMachineOrder.h)                order      blocks only
//     scanBackward() (AOTMachineLiveness.h)            scan
//     allocateRegisters() (AOTMachineAllocation.h)     allocate
//     assignSlots() (AOTMachineSlots.h)                slots      spilled values only
//     planFrame() (AOTMachineFrame.h)
//     Encoder (AOTMachineEncoder.h)                    encode
//
// Operations, blocks and lists are append-only. Order retargets edges and makes one edge of two that a Branch has to one block, and an Upsilon gets its Phi once. Nothing else changes what was appended.

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

// A place in the frame for a value that has lost its register: Allocation::spills().
using SpillIndex = uint32_t;
inline constexpr SpillIndex noSpill = UINT32_MAX;

// By Reg::index().
using RegisterMask = uint64_t;
static_assert(Reg::maxIndex() < 64);

constexpr RegisterMask maskOf(Reg reg) { return RegisterMask { 1 } << reg.index(); }
JS_EXPORT_PRIVATE RegisterMask maskOf(const RegisterSet&);
JS_EXPORT_PRIVATE RegisterSet registerSetOf(RegisterMask);

template<typename Functor>
void forEachRegister(RegisterMask mask, const Functor& functor)
{
    for (; mask; mask &= mask - 1)
        functor(Reg::fromIndex(static_cast<unsigned>(std::countr_zero(mask))));
}

// What the emitters were generating when they appended an operation. The back end hands it on and never looks inside.
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

// As a constraint: where an operand or a result of a Patch has to be. As an answer: where it is.
class ValueRep {
public:
    using OffsetType = int32_t;

    enum Kind : uint8_t {
        // Constraints of operands. Anywhere: a register, a slot or, for a constant, nowhere at all.
        // What else has no position lies nowhere either, and no answer names that. A pinned ArgumentReg is answered with its register.
        // Any other is made in a register for this use, under the two constraints below too.
        WarmAny,
        // As above, and nothing is moved or loaded for the sake of this use.
        ColdAny,
        // As above, and the place still holds the value behind the operation: no result, scratch register or clobbered register.
        LateColdAny,
        // Constraints of operands and results.
        SomeRegister,
        // Of a result: a register that holds no operand.
        SomeEarlyRegister,
        // Of an operand: a register that is neither a result nor clobbered late.
        SomeLateRegister,
        // Constraints and answers.
        Register,
        // A word of the outgoing area, by its offset from the stack pointer at the operation.
        StackArgument,
        // Answers.
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
    constexpr bool isCold() const { return m_kind == ColdAny || m_kind == LateColdAny; }
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

    // Of an answer. The frame is set up if the value lies in it.
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

// Two Patches with equal ones, and equal children where it depends on them, give the same result wherever they stand. They are numbered like any pure operation.
struct PureFunction {
    // The ids from here on are the back end's own: this bit, and the opcode that the Patch stands for. codegen/AOTPureOperations.h gives out the ids below.
    static constexpr uint64_t firstIDOfBackEnd = uint64_t { 1 } << 63;

    uint64_t id;
    bool dependsOnChildren { false };
    // Its one child is a Double, and for a whole number within int64 its result is that number.
    bool truncatesDoubleToInt64 { false };
    // Its result also follows from what lies in Effects::reads of the Patch, so a write there ends its number as it ends that of a load. Two with one id read the same heaps.
    bool dependsOnMemory { false };

    friend bool operator==(const PureFunction&, const PureFunction&) = default;
};

// Of a Patch. Nothing here moves an operation ahead of another, so only these questions are asked:
// may it be dropped, may it sink into a rare region, which numbered loads does it end, is it a safepoint.
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
    // Recorded for the printer. Nothing is hoisted.
    bool controlDependent { false };
    bool writesLocalState { false };
    // The collector may run in it: a narrow reference that lives across it has its pointer beside it.
    bool mayCollect { false };
    // Only asked whether empty.
    HeapSet reads;
    HeapSet writes;
};

enum class EdgeClass : uint8_t {
    Normal,
    // Laid out behind every block that normal edges alone lead to, ahead of the rare regions. Allocated like its normal siblings.
    Unlikely,
    // What only such edges lead to is a rare region: its code lies at the end of the function, and the code around it does not pay for it.
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
    // A load through null, undefined or what their narrow words decode to faults, and the handler of the fault throws. It is never dropped, and the encoder records it.
    Traps = 1 << 0,
    // A 32-bit word that names a cell. The wrapper NarrowReference sets it.
    IsNarrowReference = 1 << 1,
    // Its first child, or the base of its address, has to outlive it.
    IsInnerPointer = 1 << 2,
    // Div and Mod: x / 0 is 0, and the least integer by -1 is itself.
    IsChill = 1 << 3,
    ReadsImmutableHeap = 1 << 4,
    NamesPermanentCell = 1 << 5,
    // Something other than a Trunc reads it, so the upper half of an Int64 counts. It rises where the read is counted.
    UpperHalfIsRead = 1 << 6,
    // A Load of 64 bits that reads an integer from minIntegerAddedTo to maxIntegerAddedTo. Builder::noteIntegerAddedTo() sets it.
    IsIntegerAddedTo = 1 << 7,
    // HeapTrait::IsFilledOnce. Whoever tests the word and then reads it reads what it tested, so the load is never made a second time for one of its readers.
    ReadsWordFilledOnce = 1 << 8,
    // A ZExt32 of a word that is no less than zero as an int32, so it is the sign extension too. Builder::extendWordThatIsNotNegative() sets it.
    ExtendsWordThatIsNotNegative = 1 << 9,
    // A Phi of 32 bits that some Upsilon hands a word above which anything may lie: clearsUpperHalfOfRegister(). Builder::upsilon() and setPhi() set it, and whoever else makes an Upsilon copies one.
    UpperHalfMayBeSet = 1 << 10,
    // A Load of 32 bits whose base may be a boxed value that is no cell. It then reads zero: readsOffNoCellFault, image/AOTCodeSites.h. Builder::load32OffWhatMayBeNoCell() makes it.
    // The encoder writes it in one of the two forms that the handler of the fault knows: compared with a constant where it lies, or moved into a register.
    ReadsZeroOffNoCell = 1 << 11,
};

struct SwitchData {
    // Rising. Successor i of the block belongs to value i, and the last successor to every other value.
    std::span<const int64_t> caseValues;
};

class Operation {
    WTF_MAKE_NONCOPYABLE(Operation);
public:
    static constexpr unsigned maxInlineChildren = 3;

    Opcode opcode() const { return m_opcode; }
    Type type() const { return m_type; }
    // Dense within the procedure, in the order of appending.
    uint32_t index() const { return m_index; }
    Origin origin() const { return m_origin; }
    // Null for an operation without position.
    Block* owner() const { return m_owner; }
    uint32_t indexInBlock() const { return m_indexInBlock; }
    bool hasNoPosition() const { return !m_owner; }

    inline unsigned numChildren() const;
    inline Operation* child(unsigned index) const;
    Operation* lastChild() const { return child(numChildren() - 1); }
    inline std::span<Operation* const> children() const;
    // Both of an operation that is no Patch or Check and names no symbol.
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
    bool readsZeroOffNoCell() const { return has(OperationFlag::ReadsZeroOffNoCell); }
    bool isChill() const { return has(OperationFlag::IsChill); }
    bool isNarrowReference() const { return has(OperationFlag::IsNarrowReference); }
    // Flags only rise.
    void markAsNarrowReference() { m_flags |= static_cast<uint16_t>(OperationFlag::IsNarrowReference); }

    // Saturates. Final once the emitters are done. An operation that was folded away or found by its number has counted nothing, nor has a pure operation that nothing reads.
    unsigned useCount() const { return m_useCount; }
    // One more read of this operation. Saturates. At the first read of an operation that stands in a block and for which countsChildrenAtFirstUse() holds, each of its children gains a use first.
    // An Add without position has counted its children when it was made.
    // The reader is no Trunc: a Trunc counts its child at its own first read, in here, and that read alone leaves UpperHalfIsRead as it is.
    ALWAYS_INLINE void gainUse()
    {
        m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfIsRead);
        if (m_useCount) {
            if (m_useCount != std::numeric_limits<uint16_t>::max())
                ++m_useCount;
            return;
        }
        m_useCount = 1;
        if (m_owner && countsChildrenAtFirstUse(m_opcode))
            countChildrenAtFirstUse();
    }
    // One read fewer: Order takes back the read of a Branch that tests nothing. An operation that counted its children at its first read and is left without a read takes those back too.
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
    int64_t asInt() const { return hasInt32() ? asInt32() : asInt64(); }
    bool isInt(int64_t value) const { return hasInt() && asInt() == value; }
    bool hasIntPtr() const { return hasInt64(); }
    intptr_t asIntPtr() const { return asInt64(); }
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
    // Of any constant, as the register would hold it.
    int64_t constantBits() const
    {
        ASSERT(isConstant());
        return m_payload.bits;
    }

    // Of a load or a store. The address is the last child plus the offset.
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
    // child(0) of an Upsilon.
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

    // Whether it stays though nothing uses its result.
    inline bool mustStay() const;
    inline bool endsBlock() const;
    // A Jump, or a Branch whose two edges led to one block, of which Order has left the block one. Such a Branch tests nothing and reads nothing: forEachLeaf() lists no leaf of it.
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

    JS_EXPORT_PRIVATE void countChildrenAtFirstUse();
    void countChildrenAtFirstUseWithWorklist();

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
    // The number of inline children, or the space of the symbol where namesSymbol().
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

private:
    static void runFunctor(const PatchGenerator& generator, CCallHelpers& jit, const PatchParameters& parameters)
    {
        static_cast<const PatchGeneratorOf&>(generator).m_functor(jit, parameters);
    }

    Functor m_functor;
};

// The constraints of the results of a Patch: none for Void, one for a number, one for each member of a tuple.
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

    JS_EXPORT_PRIVATE void assign(std::span<const ValueRep>);

    Arena* m_arena;
    ArenaVector<ValueRep> m_reps;
};

// An operation that writes its own instructions: Patch, and Check, whose generator writes the late path that a predicate other than zero leads to.
// The parameters list the results first and then the operands, in the order of appending. A Check's predicate is child 0 and is no parameter.
class PatchOperation final : public Operation {
public:
    static bool accepts(Opcode opcode) { return opcode == Patch || opcode == Check; }

    Effects effects { Effects::forCall() };
    // SomeRegister unless set. Those of a tuple have to be set.
    ResultConstraints resultConstraints;
    // A result under SomeRegister or SomeEarlyRegister gets one of these. Any register unless set.
    void holdResultsTo(const RegisterSet& set) { m_registersOfResults = maskOf(set); }
    RegisterMask registersOfResults() const { return m_registersOfResults; }
    // Different from every operand, result and clobbered register.
    uint8_t numGPScratchRegisters { 0 };
    uint8_t numFPScratchRegisters { 0 };
    // Emits no code and has no result. It only keeps its children alive up to this point.
    bool onlyKeepsChildrenAlive { false };
    std::optional<PureFunction> pureFunction;

    void append(const ConstrainedValue& value) { append(value.value(), value.rep()); }
    JS_EXPORT_PRIVATE void append(Operation*, const ValueRep&);
    void appendSomeRegister(Operation* value) { append(value, ValueRep::SomeRegister); }
    template<typename VectorType>
    void appendVector(const VectorType& vector)
    {
        for (const auto& value : vector)
            append(value);
    }

    // effects.reads and effects.writes, from heaps.
    JS_EXPORT_PRIVATE void setReads(std::span<const AbstractHeap* const>);
    JS_EXPORT_PRIVATE void setWrites(std::span<const AbstractHeap* const>);

    unsigned numberOfOperands() const { return m_operands.size(); }
    std::span<Operation* const> operands() const { return m_operands.span(); }
    // By the index of the child.
    const ValueRep& constraintOf(unsigned child) const { return m_constraints[child]; }
    ConstrainedValue constrainedChild(unsigned child) const { return ConstrainedValue(m_operands[child], m_constraints[child]); }

    // An early clobbered register holds no operand, a late clobbered one no result. Behind a Patch neither holds a value that is read again.
    // A Check writes them on its late path alone: on the passing side they keep what they hold.
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

    // Of a Patch that ends its block: what is clobbered beside lateClobbered() on the way to that successor, which has been appended to the block.
    // The successor is a landing pad from here on: it is entered with every value in its slot.
    JS_EXPORT_PRIVATE void clobberLateOnSuccessor(unsigned successorIndex, const RegisterSet&);
    JS_EXPORT_PRIVATE RegisterMask lateClobberedOnSuccessor(unsigned successorIndex) const;

    // The functor is copied into the arena. It is called once, while the function is encoded, as functor(CCallHelpers&, const PatchParameters&).
    template<typename Functor> inline void setGenerator(const Functor&);
    // Several operations may share one: Procedure::createGenerator().
    void setGenerator(PatchGenerator* generator) { m_generator = generator; }
    PatchGenerator* generator() const { return m_generator; }

    // Its code needs no frame of this function: no return address is overwritten, nobody walks the stack from it, and it names no slot.
    void setRunsWithoutFrame(bool value) { setBit(RunsWithoutFrame, value); }
    bool runsWithoutFrame() const { return bit(RunsWithoutFrame); }
    // Of one that runs without a frame: it relies on the stack check of a frame setup on every path to it. The function sets its frame up at entry.
    void setFollowsFrameSetup(bool value) { setBit(FollowsFrameSetup, value); }
    bool followsFrameSetup() const { return bit(FollowsFrameSetup); }
    // Of one that ends its block and has no successor: it returns to the caller and goes on to no other code, so nothing behind it counts on a stack check of this function.
    void setLeavesByReturning(bool value) { setBit(LeavesByReturning, value); }
    bool leavesByReturning() const { return bit(LeavesByReturning); }
    // What it emits or calls takes the frame pointer register for the frame of this function.
    void setNeedsFrameRecord(bool value) { setBit(NeedsFrameRecord, value); }
    bool needsFrameRecord() const { return bit(NeedsFrameRecord); }
    // The function may go on behind it in another frame, which holds only the slots that are live across it.
    void setIsSuspension(bool value) { setBit(IsSuspension, value); }
    bool isSuspension() const { return bit(IsSuspension); }
    // Of a suspension: an immediate in its code holds the index of the frame word of that child, counted down from the frame pointer. A small index makes a short instruction, so assignSlots() gives the child one of the first words.
    void setOperandEncodedAsFrameWordIndex(unsigned child) { m_operandEncodedAsFrameWordIndex = child; }
    std::optional<unsigned> operandEncodedAsFrameWordIndex() const { return m_operandEncodedAsFrameWordIndex == UINT32_MAX ? std::nullopt : std::optional<unsigned>(m_operandEncodedAsFrameWordIndex); }
    // The program lets go of its one child here. If nothing reads the child again, the allocator clears its slot, and the generator clears those of registersToRelease() that are not in use.
    void setReleasesDeadValues(bool value) { setBit(ReleasesDeadValues, value); }
    bool releasesDeadValues() const { return bit(ReleasesDeadValues); }
    // Of a call: the callee lets go of that child while it runs, so the value is the callee's from here on. If nothing reads the child again, the allocator clears its slot in front of the call,
    // and the generator clears those of registersToRelease() that are not in use before it calls.
    JS_EXPORT_PRIVATE void markChildAsLetGoByCallee(unsigned child);
    std::span<const unsigned> childrenLetGoByCallee() const { return m_childrenLetGoByCallee.span(); }
    // Of a Check: those with one generator, the same operand locations and the same frame state get one late path. The code that the generator emits may depend on nothing else.
    void setSharesLatePath(bool value) { setBit(SharesLatePath, value); }
    bool sharesLatePath() const { return bit(SharesLatePath); }
    // Its generator can bring an operand to the general register that the operand is held to from wherever it lies, in code that sites share: shorter, and slower.
    // So outside loops, in a rare block, and where the function does not go on behind the Patch, nothing is moved, loaded or made for such an operand: leaveOperandsWhereTheyLie().
    void setBringsOperandsToTheirRegisters(bool value) { setBit(BringsOperandsToTheirRegisters, value); }
    bool bringsOperandsToTheirRegisters() const { return bit(BringsOperandsToTheirRegisters); }
    // Each operand with a position that is held to a general register may lie anywhere from here on, as under ColdAny.
    // The allocator adds the register to those that are clobbered late, unless the operand lies there or a result is held to it.
    // Nothing changes where the Patch takes a scratch register, or has an operand under SomeRegister, SomeLateRegister or WarmAny: that might be given a register which the generator writes first.
    JS_EXPORT_PRIVATE void leaveOperandsWhereTheyLie();

    // Answers, for the generator.
    // The register that leaveOperandsWhereTheyLie() has released the operand from. None for any other operand.
    Reg registerToBringOperandTo(unsigned child) const { return child < m_registersToBringOperandsTo.size() ? m_registersToBringOperandsTo[child] : Reg(); }
    RegisterSet registersToRelease() const { return registerSetOf(m_registersToRelease); }
    // Of a suspension: the words of the frame, counted down from the frame pointer, first word 1, that nothing reads behind it before they are written.
    const std::optional<BitSpan>& frameWordsDeadAcrossSuspension() const { return m_frameWordsDeadAcrossSuspension; }
    // Of a suspension: how many of the lowest words that suspensions keep have to be cleared, if the function is suspended here for the first time. Nothing holds them here, and not every path to it has stored to them.
    unsigned numberOfFrameWordsToClearAtFirstSuspension() const { return m_numberOfFrameWordsToClearAtFirstSuspension; }
    // Of a suspension: the other such words, counted like the dead ones. The encoder clears them in front of it.
    const BitSpan& frameWordsClearedInFrontOfSuspension() const { return m_frameWordsClearedInFrontOfSuspension; }

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
        ReleasesDeadValues = 1 << 4,
        SharesLatePath = 1 << 5,
        LeavesByReturning = 1 << 6,
        BringsOperandsToTheirRegisters = 1 << 7,
    };

    struct SuccessorClobber {
        unsigned successorIndex;
        RegisterMask mask;
    };

    static constexpr uint32_t firstCapacityOfOperands = 4;
    static constexpr size_t bytesOfFirstConstraints = firstCapacityOfOperands * sizeof(ValueRep);
    static constexpr size_t bytesOfFirstOperands = firstCapacityOfOperands * sizeof(Operation*);
    static constexpr size_t bytesOfFirstStorage = bytesOfFirstConstraints + bytesOfFirstOperands + sizeof(ValueRep);

    static inline PatchOperation* create(Procedure&, Opcode, Type, Origin);
    inline PatchOperation(Procedure&, Opcode, Type, Origin);

    void growStorageOfOperands();

    bool bit(Bit bit) const { return m_bits & bit; }
    void setBit(Bit bit, bool value) { m_bits = value ? m_bits | bit : m_bits & ~bit; }

    Procedure& m_procedure;
    ArenaVector<Operation*> m_operands;
    ArenaVector<ValueRep> m_constraints;
    ArenaVector<SuccessorClobber> m_successorClobbers;
    ArenaVector<unsigned> m_childrenLetGoByCallee;
    // By the index of the child. Empty unless some operand was released.
    ArenaVector<Reg> m_registersToBringOperandsTo;
    RegisterMask m_earlyClobbered { 0 };
    RegisterMask m_lateClobbered { 0 };
    RegisterMask m_registersToRelease { 0 };
    RegisterMask m_registersOfResults { ~static_cast<RegisterMask>(0) };
    PatchGenerator* m_generator { nullptr };
    std::optional<BitSpan> m_frameWordsDeadAcrossSuspension;
    BitSpan m_frameWordsClearedInFrontOfSuspension;
    uint32_t m_operandEncodedAsFrameWordIndex { UINT32_MAX };
    uint8_t m_numberOfFrameWordsToClearAtFirstSuspension { 0 };
    uint16_t m_bits { 0 };
};

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

// Bytes of the frame that the emitters name. A spilled value gets its place from assignSlots() and has none of these.
class StackSlot {
    WTF_MAKE_NONCOPYABLE(StackSlot);
public:
    unsigned index() const { return m_index; }
    unsigned byteSize() const { return m_byteSize; }
    void ensureSize(unsigned byteSize) { m_byteSize = std::max(m_byteSize, byteSize); }
    // No suspension keeps its bytes.
    void setIsDeadAtSuspensions(bool value) { m_isDeadAtSuspensions = value; }
    bool isDeadAtSuspensions() const { return m_isDeadAtSuspensions; }
    // From planFrame() on.
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

    // Without the Phis and Upsilons.
    unsigned size() const { return m_operations.size(); }
    bool isEmpty() const { return m_operations.isEmpty(); }
    Operation* at(unsigned index) const { return m_operations[index]; }
    Operation* last() const { return m_operations.last(); }
    std::span<Operation* const> operations() const { return m_operations.span(); }
    bool isTerminated() const { return !isEmpty() && last()->endsBlock(); }

    // A Phi stands at the head whenever it was made, also after the block was filled.
    std::span<Operation* const> phis() const { return m_phis.span(); }
    // An Upsilon hands its child to its Phi on the edge to the block of that Phi. Its child is read at the end of this block. Where no edge leads there, because a Branch or a Switch on a constant was appended as a Jump, nothing reads the Upsilon.
    std::span<Operation* const> upsilons() const { return m_upsilons.span(); }

    unsigned numSuccessors() const { return m_successors.size(); }
    const Edge& successor(unsigned index) const { return m_successors[index]; }
    std::span<const Edge> successors() const { return m_successors.span(); }
    JS_EXPORT_PRIVATE void appendSuccessor(Edge);
    JS_EXPORT_PRIVATE void setSuccessors(Edge);
    JS_EXPORT_PRIVATE void setSuccessors(Edge taken, Edge notTaken);
    JS_EXPORT_PRIVATE void clearSuccessors();

    // Edges that name it so far.
    unsigned numberOfIncomingEdges() const { return m_numberOfIncomingEdges; }
    // The block that its one incoming edge comes out of, if that edge was appended. Null with any other number of edges, and once Order has retargeted an edge to it.
    Block* singlePredecessor() const { return m_numberOfIncomingEdges == 1 ? m_singlePredecessor : nullptr; }
    // While singlePredecessor() names a block.
    EdgeClass classOfSingleIncomingEdge() const { return m_classOfSingleIncomingEdge; }
    // It continues the numbering scope of the one block that leads to it. A second edge that is appended to it asserts. One that Order retargets to it takes the mark away.
    bool hasOnlyOnePredecessor() const { return m_hasOnlyOnePredecessor; }
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
    static constexpr uint32_t firstCapacityOfUpsilons = 2;
    static constexpr uint32_t firstCapacityOfSuccessors = 2;

    inline void appendSuccessorInline(Edge);
    inline void setSuccessorsInline(Edge);
    inline void setSuccessorsInline(Edge taken, Edge notTaken);
    bool mayBeAppendedTo() const { return !m_lastOperationMayEndBlock || !isTerminated(); }

    // Order::retarget() calls it too. An edge that Order moves skips only blocks that compute nothing, or goes to a block with the content of its old target. So it may be the second edge into a block
    // that continues the scope of its predecessor: the arm of a Branch on a join of constants, one of two arms that do the same. The block then has several predecessors, and says so.
    void didGainIncomingEdge()
    {
        RELEASE_ASSERT(!m_isEntry);
        if (m_numberOfIncomingEdges)
            m_hasOnlyOnePredecessor = false;
        // An edge may have been taken away since it was set, so the count alone does not say whether it still names the source of the edge that is left.
        m_singlePredecessor = nullptr;
        ++m_numberOfIncomingEdges;
    }

    Procedure& m_procedure;
    uint32_t m_index;
    uint32_t m_numberOfIncomingEdges { 0 };
    Block* m_singlePredecessor { nullptr };
    EdgeClass m_classOfSingleIncomingEdge { EdgeClass::Normal };
    bool m_hasOnlyOnePredecessor { false };
    bool m_isLandingPad { false };
    bool m_isEntry { false };
    bool m_lastOperationMayEndBlock { false };
    ArenaVector<Operation*> m_operations;
    ArenaVector<Operation*> m_phis;
    ArenaVector<Operation*> m_upsilons;
    ArenaVector<Edge> m_successors;
};

inline bool Operation::isUnconditionalJump() const
{
    return m_opcode == Jump || (m_opcode == Branch && m_owner->numSuccessors() == 1);
}

// What the back end has to know of a function before any operation is appended.
struct ProcedureTraits {
    // It has suspensions, or is the body of a coroutine: every register but SavedFrame::preservedRegister dies at a suspension, the callee-saved FPRs are not used,
    // and the saved registers are those that a saved frame can describe.
    bool savesFrameOnSuspend { false };
    bool hasHandlers { false };
    // The stub that rejects the promise of an async function returns from the frame through its record.
    bool rejectsOnThrow { false };
    // StubBodyConvention: the function saves every register that it writes but those that a call of it clobbers, and writes none of those that unwinding restores. Never beside savesFrameOnSuspend.
    bool savesRegistersOfCallers { false };
    bool usesNarrowReferences { true };
    // The likely way through the function ends in a jump to the code that lies behind it, which whoever lays the code out takes away. So all that is not likely stands in front of the entry.
    bool fallsIntoWhatLiesBehind { false };
    // A callee-saved register that the function does not save yet is taken only for a value that is worth its store and its load: Allocator::calleeSavesWorthTakingFor().
    // The product asks for it on x86-64 alone, which has two such registers for integers and pushes each by itself. ARM64 has ten and saves them in pairs: there it made 102,000 stores and loads of the application out of 89,000 moves, and the setups stayed as they were.
    bool chargesForCalleeSaves { false };
    // Whoever walks the stack finds the frame by its size, and nothing takes the frame pointer register for the frame but a Patch that says so: PatchOperation::needsFrameRecord(). On x86-64 such a frame may go without a record: AOTMachineFrameForms.h.
    bool mayOmitFrameRecord { false };
    // In checking builds, under --aot-validate and with Options::verifyAOTMachineCode(): the checks of AOTMachineChecker.h run, and CodeGenerator holds emitted loads and stores to the declared effects.
    bool checksAllocation { false };
    // The encoder notes which bytes it writes for which origin: describeRangesOfOrigins(). The bytes are the same with and without it.
    bool notesRangesOfOrigins { false };
    // Of the machine that the code will run on: AssemblerPolicy::features of the assembler that generate() is given.
    CPUFeatureSet features;

    bool setsUpFrameAtEntry() const { return savesFrameOnSuspend || hasHandlers || rejectsOnThrow; }
};

// A frame setup that allocates no more than this moves the stack pointer in front of the stack check, so that the check compares the stack pointer itself.
inline constexpr unsigned maxBytesAllocatedBeforeStackCheck = 4 * KB;

struct StackCheckSite {
    // What the frame setup that is being written allocates. The frame record is pushed.
    unsigned bytesBelowFrameRecord { 0 };
    // bytesBelowFrameRecord <= maxBytesAllocatedBeforeStackCheck. The stack pointer already stands below those bytes, and none of them is written but the callee saves that the setup has pushed.
    // Otherwise it has not moved since the record was pushed.
    bool isAllocated { false };
    // The function does not set its frame up at entry, and no Patch of it counts on a check: Allocation::holdsPatchThatCountsOnStackCheck().
    // So it calls nothing and jumps to nothing, and uses the stack as a function without a frame does, and these bytes and the record more. Whether that goes untested is the generator's to say.
    bool nothingCountsOnCheck { false };
};

// Called where a frame has been given its record and before anything else of it is written, once for each frame setup.
using StackCheckGenerator = void (*)(void* context, CCallHelpers&, const Frame&, const StackCheckSite&);

struct ConstantDataRecord {
    std::span<const uint8_t> bytes;
    uint32_t alignment;
    ConstantKind kind;
};

struct Entrypoint {
    uint16_t id;
    Block* block;
    // From encode() on.
    MacroAssembler::Label label;
};

struct JumpTableRecord {
    // Of the targets, in the order of the table. Set while the function is encoded.
    std::span<MacroAssembler::Label> targets;
};

struct TrappingAccess {
    MacroAssembler::Label instruction;
    Origin origin;
    // The register that held the tested word: the base of the address or, where the reference was never made a pointer, its index.
    GPRReg reference;
    bool referenceIsNarrow;
};

struct FrameRange {
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
    // Operation::index() of the operation that was encoded, late paths included. UINT32_MAX: the code is the back end's own.
    uint32_t operation { UINT32_MAX };
};

// Each is a fault of the compiler. The first one stops the steps that follow, and whoever drives the back end reports it with the function and the origin.
enum class FailureKind : uint8_t {
    // validate(): a block without an end, a child that does not come first, a type that does not fit, an Upsilon without its Phi.
    MalformedProcedure,
    // The constraints of one Patch ask for more registers than there are.
    OutOfRegisters,
    // Checking builds from here on.
    AccessOutsideDeclaredEffects,
    RegisterLacksItsValue,
    SpillSlotLacksItsValue,
    FrameAddressAcrossSuspension,
    NarrowReferenceWithoutPointer,
    InnerPointerWithoutOwner,
    NeedsFrameWhereNoneIsSetUp,
};

struct Failure {
    FailureKind kind;
    Origin origin;
    // Operation::index(), or UINT32_MAX.
    uint32_t operation { UINT32_MAX };
};

class Procedure {
    WTF_MAKE_NONCOPYABLE(Procedure);
public:
    JS_EXPORT_PRIVATE Procedure(Arena&, const HeapTree&, const ProcedureTraits&);

    // Keeps the first.
    JS_EXPORT_PRIVATE void fail(FailureKind, const Operation* = nullptr);
    const std::optional<Failure>& failure() const { return m_failure; }

    Arena& arena() { return m_arena; }
    const HeapTree& heapTree() const { return m_heapTree; }
    const ProcedureTraits& traits() const { return m_traits; }

    JS_EXPORT_PRIVATE Block* addBlock();
    unsigned numberOfBlocks() const { return m_blocks.size(); }
    Block* blockAt(unsigned index) const { return m_blocks[index]; }
    std::span<Block* const> blocks() const { return m_blocks.span(); }

    // Code is entered at the head of these and nowhere else. No edge leads to one. The first has the primary entry.
    JS_EXPORT_PRIVATE Block* addEntrypoint(uint16_t id);
    std::span<const Entrypoint> entrypoints() const { return m_entrypoints.span(); }
    MacroAssembler::Label entrypointLabel(unsigned index) const { return m_entrypoints[index].label; }

    unsigned numberOfOperations() const { return m_operations.size(); }
    Operation* operationAt(uint32_t index) const { return m_operations[index]; }
    // Some Load has OperationFlag::IsIntegerAddedTo.
    bool holdsIntegersAddedTo() const { return m_holdsIntegersAddedTo; }
    // Somebody has asked the builder for the FramePointer, whether or not anything reads it.
    bool namesFramePointer() const { return m_namesFramePointer; }

    JS_EXPORT_PRIVATE Type addTuple(std::span<const Type>);
    Type addTuple(std::initializer_list<Type> types) { return addTuple(std::span<const Type>(types.begin(), types.size())); }
    std::span<const Type> tupleOf(Type type) const { return m_tuples[type.tupleIndex()]; }
    unsigned numberOfTuples() const { return m_tuples.size(); }
    unsigned resultCount(Type type) const { return type.isTuple() ? static_cast<unsigned>(tupleOf(type).size()) : type == Void ? 0 : 1; }
    Type resultType(Type type, unsigned index) const { return type.isTuple() ? tupleOf(type)[index] : type; }

    JS_EXPORT_PRIVATE StackSlot* addStackSlot(unsigned byteSize);
    std::span<StackSlot* const> stackSlots() const { return m_stackSlots.span(); }

    // The outgoing area at the bottom of the frame is at least this large.
    void requestCallArgAreaSizeInBytes(unsigned size) { m_callArgAreaSize = std::max(m_callArgAreaSize, size); }
    unsigned callArgAreaSizeInBytes() const { return m_callArgAreaSize; }

    // Returns the index of the constant: the symbol { SymbolSpace::Constant, index }. The bytes are copied. Equal bytes are one constant whatever their makers call them, and it keeps the kind of the first.
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

    void setStackCheckGenerator(StackCheckGenerator generator, void* context)
    {
        m_stackCheckGenerator = generator;
        m_stackCheckContext = context;
    }
    StackCheckGenerator stackCheckGenerator() const { return m_stackCheckGenerator; }
    void* stackCheckContext() const { return m_stackCheckContext; }
    // The generator makes no check for a frame of at most so many bytes, so the setup of such a frame cannot stop the function. Zero: every setup may.
    void setLargestFrameSetUpWithoutStackCheck(unsigned bytes) { m_largestFrameSetUpWithoutStackCheck = bytes; }
    unsigned largestFrameSetUpWithoutStackCheck() const { return m_largestFrameSetUpWithoutStackCheck; }

    // Each from the step that makes it on.
    const Order& order() const { return *m_order; }
    const Liveness& liveness() const { return *m_liveness; }
    const Allocation& allocation() const { return *m_allocation; }
    const SlotAssignment& slots() const { return *m_slots; }
    const Frame& frame() const { return *m_frame; }
    Allocation& allocationForTesting() { return *m_allocation; }
    Frame& frameForTesting() { return *m_frame; }

    // From encode() on.
    std::span<const JumpTableRecord> jumpTables() const { return m_jumpTables.span(); }
    std::span<const TrappingAccess> trappingAccesses() const { return m_trappingAccesses.span(); }
    std::span<const FrameRange> frameRanges() const { return m_frameRanges.span(); }
    // In the order of the code. Empty unless ProcedureTraits::notesRangesOfOrigins.
    std::span<const RangeOfOrigin> rangesOfOrigins() const { return m_rangesOfOrigins.span(); }
    // How often the encoder has written an operation: each Execute that it goes over, both of a pair of stores.
    uint32_t numberOfOperationsEncoded() const { return m_numberOfOperationsEncoded; }
    // Where the code of the rare blocks and the late paths begins, but for the rare regions that stand among the other code: Encoder::placeColdCodeNearItsJumps().
    MacroAssembler::Label rareCodeStart() const { return m_rareCodeStart; }
    // Those regions, in the order of the code. Neighbors are one range.
    std::span<const RangeOfRareCode> rangesOfRareCodeAmongOtherCode() const { return m_rangesOfRareCodeAmongOtherCode.span(); }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    friend class Builder;
    friend class Encoder;
    friend class Order;
    friend void prepareForGeneration(Procedure&);

    // Gives the operation its index.
    void didCreate(Operation* operation)
    {
        operation->m_index = m_operations.size();
        m_operations.append(m_arena, operation);
    }

    // One call after another lists the same heaps.
    struct ListOfHeapsWithItsSet {
        std::span<const AbstractHeap* const> list;
        HeapSet set;
    };

    std::span<const HeapIndex> heapsKeptOf(std::span<const AbstractHeap* const>);

    Arena& m_arena;
    const HeapTree& m_heapTree;
    const ProcedureTraits m_traits;
    std::array<ListOfHeapsWithItsSet, 4> m_lastListsOfHeaps { };
    MachineCoreListsOfHeapsKept* m_listsOfHeapsKept { nullptr };
    unsigned m_numberOfListsOfHeapsKept { 0 };
    ArenaVector<Block*> m_blocks;
    ArenaVector<Operation*> m_operations;
    ArenaVector<Entrypoint> m_entrypoints;
    ArenaVector<std::span<const Type>> m_tuples;
    ArenaVector<StackSlot*> m_stackSlots;
    ArenaVector<ConstantDataRecord> m_constantData;
    // Open addressing by the hash of bytes and alignment: one more than the index in m_constantData, or zero. The size is a power of two.
    ArenaVector<uint32_t> m_constantDataBuckets;
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
    MacroAssembler::Label m_rareCodeStart;
    ArenaVector<RangeOfRareCode> m_rangesOfRareCodeAmongOtherCode;
    std::optional<Failure> m_failure;
};

template<typename Functor>
inline void PatchOperation::setGenerator(const Functor& functor)
{
    m_generator = m_procedure.createGenerator(functor);
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

inline PatchOperation* PatchOperation::create(Procedure& procedure, Opcode opcode, Type type, Origin origin)
{
    static_assert(std::is_trivially_destructible_v<PatchOperation>);
    static_assert(alignof(PatchOperation) >= alignof(ValueRep) && alignof(ValueRep) >= alignof(Operation*));
    static_assert(!(sizeof(PatchOperation) % alignof(ValueRep)) && !((bytesOfFirstConstraints + bytesOfFirstOperands) % alignof(ValueRep)));
    return new (NotNull, procedure.arena().allocate(sizeof(PatchOperation) + bytesOfFirstStorage, alignof(PatchOperation))) PatchOperation(procedure, opcode, type, origin);
}

ALWAYS_INLINE void Block::appendSuccessorInline(Edge edge)
{
    Block* target = edge.block();
    RELEASE_ASSERT(target);
    ASSERT(&target->m_procedure == &m_procedure);
    uint32_t numberOfIncomingEdges = target->m_numberOfIncomingEdges;
    // The target reads values that were numbered in its one predecessor. On a second path into it they would not have been computed.
    RELEASE_ASSERT(!target->m_isEntry && (!target->m_hasOnlyOnePredecessor || !numberOfIncomingEdges));
    m_successors.append(m_procedure.arena(), edge);
    target->m_numberOfIncomingEdges = numberOfIncomingEdges + 1;
    if (numberOfIncomingEdges) {
        target->m_singlePredecessor = nullptr;
        return;
    }
    target->m_singlePredecessor = this;
    target->m_classOfSingleIncomingEdge = edge.edgeClass();
}

ALWAYS_INLINE void Block::setSuccessorsInline(Edge target)
{
    if (!m_successors.isEmpty()) [[unlikely]]
        clearSuccessors();
    appendSuccessorInline(target);
}

ALWAYS_INLINE void Block::setSuccessorsInline(Edge taken, Edge notTaken)
{
    if (!m_successors.isEmpty()) [[unlikely]]
        clearSuccessors();
    appendSuccessorInline(taken);
    appendSuccessorInline(notTaken);
}

// What a generator is told. It holds for the one call of the generator and may not be kept, but for what addLatePath() copies.
class PatchParameters {
    WTF_MAKE_NONCOPYABLE(PatchParameters);
public:
    PatchOperation* value() const { return m_value; }

    // The results, then the operands. A Check has its operands behind the predicate alone.
    unsigned size() const { return static_cast<unsigned>(m_reps.size()); }
    const ValueRep& at(unsigned index) const { return m_reps[index]; }
    const ValueRep& operator[](unsigned index) const { return at(index); }
    const ValueRep* begin() const { return m_reps.data(); }
    const ValueRep* end() const { return m_reps.data() + m_reps.size(); }

    // The registers that hold a value which is read behind the operation, and those of its results.
    RegisterSet usedRegisters() const { return registerSetOf(m_usedRegisters); }
    RegisterMask usedRegistersMask() const { return m_usedRegisters; }
    // usedRegisters(), the pinned registers and the callee-saved registers that the function does not save, less the scratch registers.
    JS_EXPORT_PRIVATE RegisterSet unavailableRegisters() const;

    GPRReg gpScratch(unsigned index) const { return m_gpScratch[index]; }
    FPRReg fpScratch(unsigned index) const { return m_fpScratch[index]; }

    // Of a Patch that ends its block. A label is set before any late path runs, and stays where it is until the function is linked.
    std::span<MacroAssembler::Label* const> successorLabels() const { return m_successorLabels; }
    JS_EXPORT_PRIVATE bool fallsThroughToSuccessor(unsigned successorIndex) const;

    bool frameIsSetUp() const { return m_frameIsSetUp; }
    JS_EXPORT_PRIVATE const Frame& frame() const;
    JS_EXPORT_PRIVATE Procedure& proc() const;
    Origin origin() const { return m_value->origin(); }

    // The functor is copied into the arena and called as functor(CCallHelpers&) behind the last block, in the order of adding. The frame state there is that of the operation.
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

// AOTMachineGenerate.cpp.
// Order, scan, allocate, slots, frame, each with its check in checking builds. After it the frame is known, and nothing has been written. Stops at the first failure.
JS_EXPORT_PRIVATE void prepareForGeneration(Procedure&);
// Encode. Writes every block, the late paths behind them, and links all of it within the assembler. Not after a failure.
JS_EXPORT_PRIVATE void generate(Procedure&, CCallHelpers&, RelocationRecorder&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
