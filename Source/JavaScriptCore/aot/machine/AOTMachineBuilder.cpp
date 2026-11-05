/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineBuilder.h"

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTMachineFolding.h"
#include "AOTMachineTarget.h"
#include <algorithm>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Machine {

enum class MachineBuilderFoldFamily : uint8_t {
    // Of integers or of numbers, by the type.
    Arithmetic,
    IntegerArithmetic,
    Bitwise,
    FloatingPoint,
    Conversion,
    Comparison,
    None,
};

static constexpr MachineBuilderFoldFamily machineBuilderFoldFamilyOf(Opcode opcode)
{
    switch (opcode) {
    case Add:
    case Sub:
    case Mul:
    case Div:
    case Neg:
        return MachineBuilderFoldFamily::Arithmetic;
    case MulHigh:
    case UMulHigh:
    case UDiv:
    case Mod:
    case UMod:
        return MachineBuilderFoldFamily::IntegerArithmetic;
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case Clz:
    case PopCount:
        return MachineBuilderFoldFamily::Bitwise;
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
        return MachineBuilderFoldFamily::FloatingPoint;
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
        return MachineBuilderFoldFamily::Conversion;
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
        return MachineBuilderFoldFamily::Comparison;
    default:
        return MachineBuilderFoldFamily::None;
    }
    return MachineBuilderFoldFamily::None;
}

static constexpr TypeKind machineBuilderKindOfFirstChild = Void;
static constexpr TypeKind machineBuilderKindThatOpcodeDoesNotSay = Tuple;

static constexpr TypeKind machineBuilderKindOfResult(Opcode opcode)
{
    switch (opcode) {
    case Opaque:
    case Add:
    case Sub:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case SExt8:
    case SExt16:
        return machineBuilderKindOfFirstChild;
    case SExt32:
    case ZExt32:
        return Int64;
    case Trunc:
        return Int32;
    case IToD:
    case FloatToDouble:
        return Double;
    case DoubleToFloat:
        return Float;
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
        return Int32;
    default:
        break;
    }
    return machineBuilderKindThatOpcodeDoesNotSay;
}

enum class MachineBuilderLowering : uint8_t {
    Never,
    UnlessRightIsConst32,
    Always,
};

static constexpr MachineBuilderLowering machineBuilderLoweringOf(Opcode opcode)
{
#if CPU(ARM64)
    return opcode == Mod || opcode == UMod ? MachineBuilderLowering::Always : MachineBuilderLowering::Never;
#else
    if (opcode == Shl || opcode == SShr || opcode == ZShr)
        return MachineBuilderLowering::UnlessRightIsConst32;
    if (opcode == Div || opcode == UDiv || opcode == Mod || opcode == UMod || opcode == MulHigh || opcode == UMulHigh)
        return MachineBuilderLowering::Always;
    return MachineBuilderLowering::Never;
#endif
}

struct MachineBuilderRulesOfOpcode {
    MachineBuilderFoldFamily foldFamily;
    TypeKind kindOfResult;
    MachineBuilderLowering lowering;
};

static constexpr std::array<MachineBuilderRulesOfOpcode, numberOfOpcodes> machineBuilderRulesOfOpcodes = [] {
    std::array<MachineBuilderRulesOfOpcode, numberOfOpcodes> rules { };
    for (unsigned opcode = 0; opcode < numberOfOpcodes; ++opcode)
        rules[opcode] = { machineBuilderFoldFamilyOf(static_cast<Opcode>(opcode)), machineBuilderKindOfResult(static_cast<Opcode>(opcode)), machineBuilderLoweringOf(static_cast<Opcode>(opcode)) };
    return rules;
}();

static ALWAYS_INLINE Operation* machineBuilderFold(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* rightOrNull)
{
    switch (machineBuilderRulesOfOpcodes[opcode].foldFamily) {
    case MachineBuilderFoldFamily::Arithmetic:
        if (type.isInt())
            return foldIntegerArithmetic(builder, opcode, type, left, rightOrNull, false);
        return foldFloatingPoint(builder, opcode, type, left, rightOrNull);
    case MachineBuilderFoldFamily::IntegerArithmetic:
        return foldIntegerArithmetic(builder, opcode, type, left, rightOrNull, false);
    case MachineBuilderFoldFamily::Bitwise:
        return foldBitwise(builder, opcode, type, left, rightOrNull);
    case MachineBuilderFoldFamily::FloatingPoint:
        return foldFloatingPoint(builder, opcode, type, left, rightOrNull);
    case MachineBuilderFoldFamily::Conversion:
        return foldConversion(builder, opcode, type, left);
    case MachineBuilderFoldFamily::Comparison:
        return foldComparison(builder, opcode, left, rightOrNull);
    case MachineBuilderFoldFamily::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#if CPU(ARM64)
// A chill Div and a plain one of the same children have different flags, so they are two values.
static constexpr uint64_t machineBuilderPayloadOfChill = 1;
#endif

// A ZExt32 with OperationFlag::ExtendsWordThatIsNotNegative and a plain one of the same word are two values.
static constexpr uint64_t machineBuilderPayloadOfWordThatIsNotNegative = 1;

static ALWAYS_INLINE NumberingKey machineBuilderKeyOf(Opcode opcode, Type type, Operation* first, Operation* second = nullptr, Operation* third = nullptr, uint64_t payload = 0)
{
    return { opcode, type, { first, second, third }, payload };
}

static NumberingKeyOfPurePatch machineBuilderKeyOfPurePatch(const PureFunction& function, Type type, std::span<Operation* const> operands)
{
    return { type, function.id, function.dependsOnChildren ? operands : std::span<Operation* const>() };
}

static uint64_t machineBuilderPayloadOfAccess(int32_t offset, HeapIndex heap)
{
    return static_cast<uint32_t>(offset) | static_cast<uint64_t>(heap) << 32;
}

static NumberingKey machineBuilderKeyOfLoad(const Operation* load, Operation* base)
{
    return machineBuilderKeyOf(load->opcode(), load->type(), base, nullptr, nullptr, machineBuilderPayloadOfAccess(load->offset(), load->heap()));
}

// Rules 1 to 4, 6 and 7 of AOTMachineLiveness.h, which hold while one operation reads it. A second reader in a later block would cost it the fusion and a register up to there.
static bool machineBuilderMayBeFusedIntoItsUser(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (isLoad(opcode)) {
        if (operation->has(OperationFlag::ReadsWordFilledOnce))
            return false;
        return operation->child(0)->hasNoPosition() || (isX86_64() && operation->type() != Int64 && operation->useCount() < 2);
    }
    if (opcode == ZExt32 || opcode == SExt32)
        return !isLoad(operation->child(0)->opcode());
    if (opcode == BitOr)
        return isComparison(operation->child(0)->opcode()) && isComparison(operation->child(1)->opcode());
    return isComparison(opcode) || opcode == BitAnd || opcode == Add || opcode == Shl || opcode == Trunc;
}

static uint64_t machineBuilderPayloadOfSymbol(CodeSymbol symbol)
{
    return static_cast<uint64_t>(symbol.space) << 48 | static_cast<uint64_t>(symbol.entry) << 32 | symbol.index;
}

static bool machineBuilderIsBaseOfAddWithoutPosition(const Operation* operation)
{
    return operation->opcode() == FramePointer || operation->opcode() == SlotBase || (operation->opcode() == ArgumentReg && operation->hasNoPosition());
}

// The collector's rules take the first child of a sum for the owner, and an address takes it for the base, whichever operand was made first.
static unsigned machineBuilderRankAmongOperands(const Operation* operation)
{
    if (operation->has(OperationFlag::IsInnerPointer))
        return 0;
    if (operation->hasNoPosition())
        return 1;
    return 2;
}

static bool machineBuilderShouldSwapOperands(const Operation* left, const Operation* right)
{
    if (right->isConstant())
        return false;
    if (left->isConstant())
        return true;
    unsigned leftRank = machineBuilderRankAmongOperands(left);
    unsigned rightRank = machineBuilderRankAmongOperands(right);
    if (leftRank != rightRank)
        return leftRank > rightRank;
    return left->index() > right->index();
}

static bool machineBuilderFitsInt32(int64_t value)
{
    return static_cast<int32_t>(value) == value;
}

struct MachineBuilderAddress {
    Operation* base;
    int32_t offset;
};

static MachineBuilderAddress machineBuilderAddressOf(Builder& builder, Operation* base, ptrdiff_t offset, bool mayChangeBase)
{
    int64_t distance = offset;
    while (mayChangeBase && base->opcode() == Add && base->child(1)->hasInt64()) {
        int64_t constant = base->child(1)->asInt64();
        if (sumOverflows<int64_t>(distance, constant) || !machineBuilderFitsInt32(distance + constant))
            break;
        distance += constant;
        base = base->child(0);
    }
    // `base + ((index + constant) << amount)`: the constant goes into the distance too, and what is left of the sum is the same whatever the constant.
    if (mayChangeBase && base->opcode() == Add) {
        Operation* index = base->child(1);
        Operation* amount = index->opcode() == Shl && index->child(1)->hasInt32() ? index->child(1) : nullptr;
        Operation* stepped = amount ? index->child(0) : index;
        if (stepped->opcode() == Add && stepped->child(1)->hasInt64() && !stepped->hasNoPosition()) {
            unsigned shift = amount ? static_cast<unsigned>(amount->asInt32()) & 63 : 0;
            int64_t constant = stepped->child(1)->asInt64();
            int64_t scaled = static_cast<int64_t>(static_cast<uint64_t>(constant) << shift);
            if (scaled >> shift == constant && !sumOverflows<int64_t>(distance, scaled) && machineBuilderFitsInt32(distance + scaled)) {
                Operation* unstepped = stepped->child(0);
                if (amount)
                    unstepped = builder.binary(Shl, unstepped, amount);
                base = builder.binary(Add, base->child(0), unstepped);
                distance += scaled;
            }
        }
    }
    if (!machineBuilderFitsInt32(distance)) {
        RELEASE_ASSERT(mayChangeBase);
        base = builder.binary(Add, base, builder.const64(distance));
        distance = 0;
    }
    return { base, static_cast<int32_t>(distance) };
}

static uint16_t machineBuilderFlagsOfLoadFrom(const AbstractHeap& heap)
{
    uint16_t flags = 0;
    if (heap.traits().contains(HeapTrait::Immutable))
        flags |= static_cast<uint16_t>(OperationFlag::ReadsImmutableHeap);
    if (heap.traits().contains(HeapTrait::HoldsInnerPointer))
        flags |= static_cast<uint16_t>(OperationFlag::IsInnerPointer);
    if (heap.traits().contains(HeapTrait::HoldsPermanentCell))
        flags |= static_cast<uint16_t>(OperationFlag::NamesPermanentCell);
    if (heap.traits().contains(HeapTrait::IsFilledOnce))
        flags |= static_cast<uint16_t>(OperationFlag::ReadsWordFilledOnce);
    return flags;
}

static bool machineBuilderCovers(const HeapTree& tree, const HeapSet& declared, HeapIndex heap)
{
    if (declared.isTop())
        return true;
    for (HeapIndex candidate : declared.heaps()) {
        if (tree.isSameOrAbove(candidate, heap))
            return true;
    }
    return false;
}

// Of a Branch or a Switch with one way left. The block takes it whenever it runs, so the class that set it apart from the other ways says nothing now.
// Kept rare, it would make one rare region of everything behind the block.
static Edge machineBuilderUnconditionalEdge(Edge onlyWayLeft)
{
    return Edge(onlyWayLeft.block(), EdgeClass::Normal);
}

#if ASSERT_ENABLED
static const Edge* machineBuilderEdgeBetween(const Block* from, const Block* to)
{
    for (const Edge& edge : from->successors()) {
        if (edge.block() == to)
            return &edge;
    }
    return nullptr;
}
#endif

Builder::Builder(Procedure& procedure)
    : m_proc(procedure)
    , m_numbering(procedure)
{
}

ALWAYS_INLINE void Builder::append(Operation* operation, bool mayEndBlock)
{
    // Whether a Patch ends its block is asked of the Patch each time: emitters say so after they have left the block and come back.
    Block* block = m_block;
    RELEASE_ASSERT(block && block->mayBeAppendedTo());
    block->m_lastOperationMayEndBlock = mayEndBlock;
    operation->m_owner = block;
    operation->m_indexInBlock = block->m_operations.size();
    block->m_operations.append(m_proc.arena(), operation);
}

template<unsigned numberOfChildren, bool isComputation>
ALWAYS_INLINE Operation* Builder::appendNewWithChildren(Opcode opcode, Type type, Operation* first, Operation* second, Operation* third)
{
    static_assert(numberOfChildren <= Operation::maxInlineChildren);
    Operation* result = m_proc.arena().create<Operation>(opcode, type, m_origin);
    result->m_auxiliary = numberOfChildren;
    result->m_children[0] = first;
    result->m_children[1] = second;
    result->m_children[2] = third;
    m_proc.didCreate(result);
    if constexpr (isComputation) {
        ASSERT(countsChildrenAtFirstUse(opcode));
        append(result, false);
    } else {
        append(result, opcode >= Patch);
        if (!countsChildrenAtFirstUse(opcode)) {
            if constexpr (numberOfChildren > 0)
                first->gainUse();
            if constexpr (numberOfChildren > 1)
                second->gainUse();
            if constexpr (numberOfChildren > 2)
                third->gainUse();
        }
    }
    return result;
}

ALWAYS_INLINE Operation* Builder::appendNew(Opcode opcode, Type type)
{
    return appendNewWithChildren<0, false>(opcode, type, nullptr, nullptr, nullptr);
}

ALWAYS_INLINE Operation* Builder::appendNew(Opcode opcode, Type type, Operation* child)
{
    return appendNewWithChildren<1, false>(opcode, type, child, nullptr, nullptr);
}

ALWAYS_INLINE Operation* Builder::appendNew(Opcode opcode, Type type, Operation* first, Operation* second)
{
    return appendNewWithChildren<2, false>(opcode, type, first, second, nullptr);
}

ALWAYS_INLINE Operation* Builder::appendNewComputation(Opcode opcode, Type type, Operation* child)
{
    return appendNewWithChildren<1, true>(opcode, type, child, nullptr, nullptr);
}

ALWAYS_INLINE Operation* Builder::appendNewComputation(Opcode opcode, Type type, Operation* first, Operation* second)
{
    return appendNewWithChildren<2, true>(opcode, type, first, second, nullptr);
}

ALWAYS_INLINE Operation* Builder::appendNewComputation(Opcode opcode, Type type, Operation* first, Operation* second, Operation* third)
{
    return appendNewWithChildren<3, true>(opcode, type, first, second, third);
}

// It is shared by every origin that asks for it, so it has none.
Operation* Builder::createNewWithoutPosition(Opcode opcode, Type type, uint64_t payload, uint32_t hash)
{
    Operation* result = m_proc.arena().create<Operation>(opcode, type, Origin());
    result->m_payload.bits = static_cast<int64_t>(payload);
    m_proc.didCreate(result);
    m_numbering.addWithoutPosition(result, payload, hash);
    return result;
}

ALWAYS_INLINE Operation* Builder::createWithoutPosition(Opcode opcode, Type type, uint64_t payload)
{
    uint32_t hash = Numbering::hashWithoutPosition(opcode, payload);
    if (Operation* existing = m_numbering.findWithoutPosition(opcode, payload, hash))
        return existing;
    return createNewWithoutPosition(opcode, type, payload, hash);
}

void Builder::noteInheritedFind(const Operation* operation)
{
    uint32_t index = operation->index();
    if (index >= m_epochOfLastInheritedFind.size())
        m_epochOfLastInheritedFind.grow(m_proc.arena(), std::max(m_proc.numberOfOperations(), m_epochOfLastInheritedFind.size() * 2));
    m_epochOfLastInheritedFind[index] = m_scopeEpoch;
}

ALWAYS_INLINE Operation* Builder::noteIfInherited(const Numbering::Found& found)
{
    if (found.isInherited) [[unlikely]]
        noteInheritedFind(found.operation);
    return found.operation;
}

ALWAYS_INLINE Operation* Builder::findNumbered(const NumberingKey& key, uint32_t hash)
{
    return noteIfInherited(m_numbering.lookUp(key, hash, m_isDuplicating ? Numbering::Reach::OwnEntries : Numbering::Reach::WholeScope));
}

ALWAYS_INLINE Operation* Builder::findNumberedLoad(const NumberingKey& key, uint32_t hash)
{
    return noteIfInherited(m_numbering.lookUpLoad(key, hash, m_isDuplicating ? Numbering::Reach::OwnEntries : Numbering::Reach::WholeScope));
}

ALWAYS_INLINE Type Builder::typeOfResult(Opcode opcode, Operation* first)
{
    TypeKind kind = machineBuilderRulesOfOpcodes[opcode].kindOfResult;
    RELEASE_ASSERT(kind != machineBuilderKindThatOpcodeDoesNotSay);
    return kind == machineBuilderKindOfFirstChild ? first->type() : Type(kind);
}

void Builder::appendTo(Block* block)
{
    ASSERT(block);
    sealPendingPatch();
    if (block == m_block)
        return;
    if (m_block)
        m_numbering.switchBlocks(m_block, block);
    m_block = block;
}

ScopeFork Builder::forkScope()
{
    sealPendingPatch();
    return { m_numbering.mark(), m_block };
}

// A branch on a constant is a jump, so a block that the emitters made as a continuation or as an arm may have no edge at all. Nothing reaches it then, and it is not marked.
void Builder::appendToContinuation(Block* block)
{
    sealPendingPatch();
    ASSERT(m_block->isTerminated());
    ASSERT(block->numberOfIncomingEdges() <= 1);
    if (block->numberOfIncomingEdges()) {
        ASSERT(machineBuilderEdgeBetween(m_block, block));
        block->m_hasOnlyOnePredecessor = true;
        m_numbering.continueIn(m_block, block);
    }
    m_block = block;
}

void Builder::appendToArm(Block* block, const ScopeFork& fork)
{
    sealPendingPatch();
    ASSERT(block->numberOfIncomingEdges() <= 1);
    if (block->numberOfIncomingEdges()) {
        ASSERT(machineBuilderEdgeBetween(fork.block, block));
        block->m_hasOnlyOnePredecessor = true;
    }
    m_numbering.restore(fork.mark);
    m_block = block;
}

void Builder::appendToJoin(Block* block, const ScopeFork& fork)
{
    sealPendingPatch();
    m_numbering.restore(fork.mark);
    m_block = block;
}

void Builder::appendToSuccessor(Block* block, const ScopeFork& fork)
{
    sealPendingPatch();
    bool comesOutOfFork = fork.block && block->singlePredecessor() == fork.block && block->classOfSingleIncomingEdge() != EdgeClass::Rare;
    if (!comesOutOfFork || block->isLandingPad() || !m_numbering.canRestore(fork.mark)) {
        appendTo(block);
        return;
    }
    block->m_hasOnlyOnePredecessor = true;
    m_numbering.inheritScope(fork.mark);
    ++m_scopeEpoch;
    m_block = block;
}

// An operand that the emitters were handed as a value of Code has a register of its own, so only what a request has found is copied.
Operation* Builder::duplicateInheritedFindIfFusable(Operation* operand)
{
    ASSERT(wasFoundInInheritedScope(operand));
    if (!machineBuilderMayBeFusedIntoItsUser(operand))
        return operand;

    Operation* copy;
    if (isLoad(operand->opcode())) {
        // Where a write since the request has ended the load, the operand is what the word held.
        if (!m_numbering.isStillValid(machineBuilderKeyOfLoad(operand, operand->child(0)), operand))
            return operand;
        copy = reload(operand);
    } else {
        Operation* first = duplicateIfFusable(operand->child(0));
        Operation* second = operand->numChildren() > 1 ? duplicateIfFusable(operand->child(1)) : nullptr;
        SetForScope<bool> isDuplicating(m_isDuplicating, true);
        if (second)
            copy = binary(operand->opcode(), first, second);
        else
            copy = operand->has(OperationFlag::ExtendsWordThatIsNotNegative) ? extendWordThatIsNotNegative(first) : unary(operand->opcode(), first);
    }
    // The emitters have marked the operation that their request found.
    if (operand->isNarrowReference() && !copy->isConstant())
        copy->markAsNarrowReference();
    return copy;
}

// The requests for an operation without position leave a pending Patch alone: emitters make constants while they set a Patch up, and nothing here reads or ends a numbered load.
Operation* Builder::const32(int32_t value)
{
    return createWithoutPosition(Const32, Int32, static_cast<uint64_t>(static_cast<int64_t>(value)));
}

Operation* Builder::const64(int64_t value)
{
    return createWithoutPosition(Const64, Int64, static_cast<uint64_t>(value));
}

Operation* Builder::constFloat(float value)
{
    return createWithoutPosition(ConstFloat, Float, std::bit_cast<uint32_t>(value));
}

Operation* Builder::constDouble(double value)
{
    return createWithoutPosition(ConstDouble, Double, std::bit_cast<uint64_t>(value));
}

Operation* Builder::unary(Opcode opcode, Operation* value)
{
    ASSERT(descriptionOf(opcode).numberOfChildren == 1);
    sealPendingPatch();

    Type type = typeOfResult(opcode, value);
    if (Operation* folded = machineBuilderFold(*this, opcode, type, value, nullptr))
        return folded;

    NumberingKey key = machineBuilderKeyOf(opcode, type, value);
    uint32_t hash = Numbering::hashOf(key);
    if (Operation* existing = findNumbered(key, hash))
        return existing;
    // One that was asked for with proof that the word is not negative gives the same.
    if (opcode == ZExt32) {
        NumberingKey keyWithProof = machineBuilderKeyOf(opcode, type, value, nullptr, nullptr, machineBuilderPayloadOfWordThatIsNotNegative);
        if (Operation* existing = findNumbered(keyWithProof, Numbering::hashOf(keyWithProof)))
            return existing;
    }
    if (Operation* ownValue = duplicateIfFusable(value); ownValue != value)
        return unary(opcode, ownValue);
    Operation* result = appendNewComputation(opcode, type, value);
    m_numbering.add(key, hash, result);
    return result;
}

Operation* Builder::extendWordThatIsNotNegative(Operation* word)
{
    ASSERT(word->type() == Int32);
    // What the operations say by themselves needs no mark.
    if (rangeOf(word).min >= 0)
        return unary(ZExt32, word);
    sealPendingPatch();

    if (Operation* folded = machineBuilderFold(*this, ZExt32, Int64, word, nullptr))
        return folded;

    NumberingKey key = machineBuilderKeyOf(ZExt32, Int64, word, nullptr, nullptr, machineBuilderPayloadOfWordThatIsNotNegative);
    uint32_t hash = Numbering::hashOf(key);
    if (Operation* existing = findNumbered(key, hash))
        return existing;
    if (Operation* ownWord = duplicateIfFusable(word); ownWord != word)
        return extendWordThatIsNotNegative(ownWord);
    Operation* result = appendNewComputation(ZExt32, Int64, word);
    result->m_flags |= static_cast<uint16_t>(OperationFlag::ExtendsWordThatIsNotNegative);
    m_numbering.add(key, hash, result);
    return result;
}

Operation* Builder::binary(Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(descriptionOf(opcode).numberOfChildren == 2);
    ASSERT(right->type() == (opcode == Shl || opcode == SShr || opcode == ZShr ? Type(Int32) : left->type()));
    sealPendingPatch();

    if (isCommutative(opcode) && machineBuilderShouldSwapOperands(left, right))
        std::swap(left, right);

    Type type = typeOfResult(opcode, left);
    if (Operation* folded = machineBuilderFold(*this, opcode, type, left, right))
        return folded;

    if ((opcode == Add || opcode == Sub) && left->hasNoPosition() && right->hasInt64()) {
        uint64_t constant = static_cast<uint64_t>(right->asInt64());
        if (opcode == Sub)
            constant = 0 - constant;
        Operation* base = left;
        if (base->opcode() == Add) {
            constant += static_cast<uint64_t>(base->child(1)->asInt64());
            base = base->child(0);
        }
        if (machineBuilderIsBaseOfAddWithoutPosition(base))
            return constant ? addWithoutPosition(base, static_cast<int64_t>(constant)) : base;
    }

    NumberingKey key = machineBuilderKeyOf(opcode, type, left, right);
    uint32_t hash = Numbering::hashOf(key);
    if (Operation* existing = findNumbered(key, hash))
        return existing;
    Operation* ownLeft = duplicateIfFusable(left);
    Operation* ownRight = duplicateIfFusable(right);
    if (ownLeft != left || ownRight != right)
        return binary(opcode, ownLeft, ownRight);
    MachineBuilderLowering lowering = machineBuilderRulesOfOpcodes[opcode].lowering;
    if (lowering != MachineBuilderLowering::Never && (lowering == MachineBuilderLowering::Always || !right->hasInt32())) [[unlikely]] {
        if (Operation* lowered = lowerForTarget(opcode, type, left, right))
            return lowered;
    }

    Operation* result = appendNewComputation(opcode, type, left, right);
    if ((opcode == Add || opcode == Sub) && left->has(OperationFlag::IsInnerPointer))
        result->m_flags |= static_cast<uint16_t>(OperationFlag::IsInnerPointer);
    m_numbering.add(key, hash, result);
    return result;
}

Operation* Builder::lowWord(Operation* value)
{
    ASSERT(value->type() == Int64);
    sealPendingPatch();

    if (Operation* madeOfLowWords = foldLowWord(*this, value))
        return madeOfLowWords;
    return unary(Trunc, value);
}

Operation* Builder::lowerForTarget(Opcode opcode, Type type, Operation* left, Operation* right)
{
    if (!type.isInt())
        return nullptr;

#if CPU(ARM64)
    if (opcode != Mod && opcode != UMod)
        return nullptr;
    Operation* quotient = binary(opcode == Mod ? Div : UDiv, left, right);
    return binary(Sub, left, binary(Mul, quotient, right));
#elif CPU(X86_64)
    bool isShift = opcode == Shl || opcode == SShr || opcode == ZShr;
    bool namesRegisters = opcode == Div || opcode == UDiv || opcode == Mod || opcode == UMod || opcode == MulHigh || opcode == UMulHigh || (isShift && !right->hasInt32());
    if (!namesRegisters)
        return nullptr;
    PureFunction function { PureFunction::firstIDOfBackEnd | opcode, true };
    Operation* operands[] = { left, right };
    if (Operation* existing = findPurePatch(function, type, operands))
        return existing;
    return appendWithFixedRegisters(*this, opcode, type, left, right);
#endif
}

Operation* Builder::chill(Opcode opcode, Operation* numerator, Operation* denominator)
{
    ASSERT(opcode == Div || opcode == Mod);
    Type type = numerator->type();
    ASSERT(type.isInt() && denominator->type() == type);
    sealPendingPatch();

    if (Operation* folded = foldIntegerArithmetic(*this, opcode, type, numerator, denominator, true))
        return folded;

    Operation* zero = constantOfType(type, 0);
    if (denominator->isInt(0))
        return zero;
    if (denominator->isInt(-1))
        return opcode == Div ? unary(Neg, numerator) : zero;
    IntegerRange range = rangeOf(denominator);
    if (denominator->hasInt() || range.min > 0 || range.max < -1)
        return binary(opcode, numerator, denominator);

#if CPU(ARM64)
    // The instruction answers as a chill Div does. None of the folds of a plain Div, which may lean on its promises, has seen this one.
    if (opcode == Div) {
        NumberingKey key = machineBuilderKeyOf(Div, type, numerator, denominator, nullptr, machineBuilderPayloadOfChill);
        uint32_t hash = Numbering::hashOf(key);
        if (Operation* existing = findNumbered(key, hash))
            return existing;
        Operation* quotient = appendNewComputation(Div, type, numerator, denominator);
        quotient->m_flags |= static_cast<uint16_t>(OperationFlag::IsChill);
        m_numbering.add(key, hash, quotient);
        return quotient;
    }
    // Zero alone needs an answer: the least integer by -1 is itself, which times -1 is itself again, so the remainder is zero.
    Operation* denominatorIsSafe = denominator;
#else
    // Neither 0 nor -1.
    Operation* one = constantOfType(type, 1);
    Operation* denominatorIsSafe = binary(Above, binary(Add, denominator, one), one);
#endif

    Block* safeCase = newBlock();
    Block* unsafeCase = newBlock();
    Block* continuation = newBlock();
    Operation* result = phiIn(continuation, type);

    ScopeFork fork = forkScope();
    branch(denominatorIsSafe, Edge(safeCase), Edge(unsafeCase, EdgeClass::Rare));

    appendToArm(safeCase, fork);
#if CPU(ARM64)
    Operation* safeResult = binary(Sub, numerator, binary(Mul, chill(Div, numerator, denominator), denominator));
#else
    Operation* safeResult = binary(opcode, numerator, denominator);
#endif
    upsilon(safeResult, result);
    jump(continuation);

    appendToArm(unsafeCase, fork);
    Operation* unsafeResult = zero;
#if !CPU(ARM64)
    // By -1 the quotient is the negation, which leaves the least integer as it is.
    if (opcode == Div)
        unsafeResult = select(denominator, unary(Neg, numerator), zero);
#endif
    upsilon(unsafeResult, result);
    jump(continuation);

    appendToJoin(continuation, fork);
    return result;
}

Operation* Builder::bitwiseCast(Operation* value, Type type)
{
    ASSERT(widthOf(value->type()) == widthOf(type));
    sealPendingPatch();

    if (value->type() == type)
        return value;
    if (Operation* folded = foldConversion(*this, BitwiseCast, type, value))
        return folded;

    NumberingKey key = machineBuilderKeyOf(BitwiseCast, type, value);
    uint32_t hash = Numbering::hashOf(key);
    if (Operation* existing = findNumbered(key, hash))
        return existing;
    Operation* result = appendNewComputation(BitwiseCast, type, value);
    m_numbering.add(key, hash, result);
    return result;
}

Operation* Builder::select(Operation* predicate, Operation* whenNotZero, Operation* whenZero)
{
    ASSERT(predicate->type().isInt());
    ASSERT(whenNotZero->type() == whenZero->type());
    sealPendingPatch();

    // Ahead of the peeling: Select(NotEqual(x, 0), 1, 0) is its predicate, and x is no boolean.
    if (Operation* folded = foldSelect(*this, predicate, whenNotZero, whenZero))
        return folded;

    SimplifiedPredicate simplified = simplifyPredicate(predicate);
    if (simplified.isInverted)
        std::swap(whenNotZero, whenZero);

    Type type = whenNotZero->type();
    NumberingKey key = machineBuilderKeyOf(Select, type, simplified.predicate, whenNotZero, whenZero);
    uint32_t hash = Numbering::hashOf(key);
    if (Operation* existing = findNumbered(key, hash))
        return existing;
    if (Operation* ownPredicate = duplicateIfFusable(simplified.predicate); ownPredicate != simplified.predicate)
        return select(ownPredicate, whenNotZero, whenZero);
    Operation* result = appendNewComputation(Select, type, simplified.predicate, whenNotZero, whenZero);
    m_numbering.add(key, hash, result);
    return result;
}

Operation* Builder::opaque(Operation* value)
{
    sealPendingPatch();
    if (value->opcode() == Opaque)
        return value;
    return appendNew(Opaque, typeOfResult(Opaque, value), value);
}

Operation* Builder::extract(Operation* tuple, unsigned index)
{
    ASSERT(tuple->type().isTuple() && index < m_proc.resultCount(tuple->type()));
    sealPendingPatch();

    // The allocator binds an Extract to the place of the member, so one member has one Extract where the emitters ask twice in a row.
    for (unsigned place = m_block->size(); place--;) {
        Operation* previous = m_block->at(place);
        if (previous->opcode() != Extract || previous->child(0) != tuple)
            break;
        if (previous->extractedIndex() == index)
            return previous;
    }

    Operation* result = appendNew(Extract, m_proc.resultType(tuple->type(), index), tuple);
    result->m_payload.bits = index;
    return result;
}

Operation* Builder::argumentRegister(Reg reg)
{
    Type type = reg.isGPR() ? Int64 : Double;
    if (isPinned(reg))
        return createWithoutPosition(ArgumentReg, type, reg.index());

    sealPendingPatch();
    ASSERT(m_block->isEntry());
    for (Operation* operation : m_block->operations()) {
        if (operation->isArgumentReg(reg))
            return operation;
    }
    Operation* result = appendNew(ArgumentReg, type);
    result->m_payload.bits = reg.index();
    return result;
}

Operation* Builder::framePointer()
{
    m_proc.m_namesFramePointer = true;
    return createWithoutPosition(FramePointer, pointerType(), 0);
}

Operation* Builder::slotBase(StackSlot* slot)
{
    Operation* result = createWithoutPosition(SlotBase, pointerType(), slot->index());
    result->m_payload.slot = slot;
    return result;
}

Operation* Builder::addressOf(CodeSymbol symbol)
{
    ASSERT(symbol.space != SymbolSpace::Number && symbol.space != SymbolSpace::InstanceWord);
    Operation* result = createWithoutPosition(AddressOf, pointerType(), machineBuilderPayloadOfSymbol(symbol));
    result->m_auxiliary = static_cast<uint8_t>(symbol.space);
    return result;
}

Operation* Builder::numberOf(CodeSymbol symbol)
{
    ASSERT(symbol.space == SymbolSpace::Number);
    Operation* result = createWithoutPosition(NumberOf, Int32, machineBuilderPayloadOfSymbol(symbol));
    result->m_auxiliary = static_cast<uint8_t>(symbol.space);
    return result;
}

Operation* Builder::instanceAddressOf(CodeSymbol symbol)
{
    ASSERT(symbol.space == SymbolSpace::InstanceWord);
    Operation* result = createWithoutPosition(InstanceAddressOf, pointerType(), machineBuilderPayloadOfSymbol(symbol));
    result->m_auxiliary = static_cast<uint8_t>(symbol.space);
    return result;
}

Operation* Builder::load(Opcode opcode, Type type, Operation* base, ptrdiff_t offset, const AbstractHeap& heap, Trapping trapping)
{
    ASSERT(isLoad(opcode) && heap.isInitialized());
    ASSERT(opcode == Load ? type.isNumeric() : type == Int32);
    ASSERT(base->type() == pointerType());
    sealPendingPatch();
    checkAccess(heap, false);

    bool traps = trapping == Trapping::Yes;
    if (traps) {
        RELEASE_ASSERT(!valueKnownToBeReadThrough(base));
        RELEASE_ASSERT(offset >= 0 && static_cast<uint64_t>(offset) + bytesForWidth(accessWidthOf(opcode, type)) <= trapReach());
    }

    // The handler of a fault reads the tested word out of the base register, so a load that traps keeps its base.
    MachineBuilderAddress address = machineBuilderAddressOf(*this, base, offset, !traps);
    NumberingKey key = machineBuilderKeyOf(opcode, type, address.base, nullptr, nullptr, machineBuilderPayloadOfAccess(address.offset, heap.index()));
    uint32_t hash = 0;
    if (!traps) {
        hash = Numbering::hashOf(key);
        if (Operation* existing = findNumberedLoad(key, hash)) {
            if (m_verifierOfInheritedLoads) [[unlikely]]
                verifyInheritedLoad(existing);
            return existing;
        }
    }
    if (Operation* ownBase = duplicateIfFusable(address.base); ownBase != address.base)
        return load(opcode, type, ownBase, address.offset, heap, trapping);

    Operation* result = appendNew(opcode, type, address.base);
    result->m_payload.memory.offset = address.offset;
    result->m_payload.memory.heap = heap.index();
    result->m_flags |= machineBuilderFlagsOfLoadFrom(heap);
    if (traps)
        result->m_flags |= static_cast<uint16_t>(OperationFlag::Traps);
    else
        m_numbering.addLoad(key, hash, result);
    return result;
}

Operation* Builder::load32OffWhatMayBeNoCell(Operation* base, ptrdiff_t offset, const AbstractHeap& heap)
{
    RELEASE_ASSERT(readsOffNoCellFault && heap.isInitialized() && base->type() == pointerType() && isRepresentableAs<int8_t>(static_cast<int64_t>(offset)));
    sealPendingPatch();
    checkAccess(heap, false);

    // The handler of the fault reads the tested word out of the base register, so the load keeps its base. It is made anew nowhere, so it says nothing of its heap, and no other load stands for it.
    Operation* result = appendNew(Load, Int32, base);
    result->m_payload.memory.offset = static_cast<int32_t>(offset);
    result->m_payload.memory.heap = heap.index();
    result->m_flags |= static_cast<uint16_t>(OperationFlag::ReadsZeroOffNoCell);
    return result;
}

Operation* Builder::store(Opcode opcode, Operation* value, Operation* base, ptrdiff_t offset, const AbstractHeap& heap)
{
    ASSERT(isStore(opcode) && heap.isInitialized());
    ASSERT(opcode == Store ? value->type().isNumeric() : value->type() == Int32);
    ASSERT(base->type() == pointerType());
    sealPendingPatch();
    checkAccess(heap, true);

    if (value->hasDouble())
        value = const64(value->constantBits());
    else if (value->hasFloat())
        value = const32(static_cast<int32_t>(value->constantBits()));

    MachineBuilderAddress address = machineBuilderAddressOf(*this, base, offset, true);
    Operation* ownValue = duplicateIfFusable(value);
    Operation* ownBase = duplicateIfFusable(address.base);
    Operation* result = appendNew(opcode, Void, ownValue, ownBase);
    result->m_payload.memory.offset = address.offset;
    result->m_payload.memory.heap = heap.index();
    m_numbering.didWrite(heap.index());
    return result;
}

Operation* Builder::reload(Operation* load)
{
    ASSERT(isLoad(load->opcode()) && !load->traps());
    sealPendingPatch();

    RELEASE_ASSERT(!load->has(OperationFlag::ReadsWordFilledOnce));
    RELEASE_ASSERT(m_numbering.isStillValid(machineBuilderKeyOfLoad(load, load->child(0)), load));

    Operation* base = duplicateIfFusable(load->child(0));
    NumberingKey key = machineBuilderKeyOfLoad(load, base);
    uint32_t hash = Numbering::hashOf(key);
    if (Operation* madeBefore = m_numbering.lookUpLoad(key, hash, Numbering::Reach::OwnEntries).operation; madeBefore && madeBefore != load)
        return madeBefore;

    Operation* result = appendLoadLike(load, base);
    m_numbering.addLoad(key, hash, result);
    return result;
}

Operation* Builder::appendLoadLike(const Operation* load, Operation* base)
{
    constexpr uint16_t flagsOfHeap = static_cast<uint16_t>(OperationFlag::ReadsImmutableHeap) | static_cast<uint16_t>(OperationFlag::IsInnerPointer) | static_cast<uint16_t>(OperationFlag::NamesPermanentCell);
    Operation* result = appendNew(load->opcode(), load->type(), base);
    result->m_payload.memory.offset = load->offset();
    result->m_payload.memory.heap = load->heap();
    result->m_flags |= load->m_flags & flagsOfHeap;
    return result;
}

void Builder::verifyInheritedLoads(HeapIndex writtenByOtherThreads, Function<void(Operation* readSameBits)>&& verifier)
{
    m_heapWrittenByOtherThreads = writtenByOtherThreads;
    m_verifierOfInheritedLoads = WTF::move(verifier);
}

// Nothing here is folded, numbered or copied: the comparison reads the very operation that the request was handed.
void Builder::verifyInheritedLoad(Operation* load)
{
    if (m_isVerifyingInheritedLoad || !wasFoundInInheritedScope(load) || load->has(OperationFlag::ReadsImmutableHeap))
        return;
    if (m_proc.heapTree().isSameOrAbove(m_heapWrittenByOtherThreads, load->heap()))
        return;
    SetForScope<bool> isVerifying(m_isVerifyingInheritedLoad, true);

    Operation* readBefore = load;
    Operation* readNow = appendLoadLike(load, load->child(0));
    if (!load->type().isInt()) {
        Type typeOfBits = load->type() == Double ? Int64 : Int32;
        readBefore = appendNew(BitwiseCast, typeOfBits, readBefore);
        readNow = appendNew(BitwiseCast, typeOfBits, readNow);
    }
    m_verifierOfInheritedLoads(appendNew(Equal, Int32, readBefore, readNow));
}

void Builder::noteIntegerAddedTo(Operation* load)
{
    RELEASE_ASSERT(load->opcode() == Load && load->type() == Int64);
    load->m_flags |= static_cast<uint16_t>(OperationFlag::IsIntegerAddedTo);
    m_proc.m_holdsIntegersAddedTo = true;
}

void Builder::fence()
{
    sealPendingPatch();
    appendNew(Fence, Void);
}

std::optional<int64_t> Builder::valueKnownToBeReadThrough(const Operation* base)
{
    if (base->hasInt64())
        return base->asInt64();
    if (base->isArgumentReg(cellCageBaseGPR))
        return 0;
    if (base->opcode() == Add && base->child(0)->isArgumentReg(cellCageBaseGPR) && base->child(1)->hasInt64())
        return base->child(1)->asInt64();
    return std::nullopt;
}

void Builder::setDeclaredAccesses(const HeapSet& reads, const HeapSet& writes)
{
    ASSERT(!m_suspensionsOfDeclaredAccesses);
    m_hasDeclaredAccesses = true;
    m_declaredReads = reads;
    m_declaredWrites = writes;
}

void Builder::clearDeclaredAccesses()
{
    ASSERT(!m_suspensionsOfDeclaredAccesses);
    m_hasDeclaredAccesses = false;
    m_declaredReads = HeapSet();
    m_declaredWrites = HeapSet();
}

void Builder::checkDeclaredAccess(const AbstractHeap& heap, bool isStore)
{
    if (heap.traits().containsAny({ HeapTrait::Immutable, HeapTrait::IsOutsideDeclaredEffects }))
        return;
    const HeapTree& tree = m_proc.heapTree();
    if (machineBuilderCovers(tree, m_declaredWrites, heap.index()))
        return;
    if (!isStore && machineBuilderCovers(tree, m_declaredReads, heap.index()))
        return;
    // A failure takes its origin from an operation, and a load that is found by its number has the origin of an earlier request.
    m_proc.fail(FailureKind::AccessOutsideDeclaredEffects, appendNew(Nop, Void));
}

Operation* Builder::phiIn(Block* block, Type type)
{
    ASSERT(block && type.isNumeric());
    Operation* result = m_proc.arena().create<Operation>(Phi, type, m_origin);
    m_proc.didCreate(result);
    result->m_owner = block;
    result->m_indexInBlock = block->m_phis.size();
    block->m_phis.append(m_proc.arena(), result);
    return result;
}

Operation* Builder::upsilon(Operation* value, Operation* phi)
{
    sealPendingPatch();
    Block* block = m_block;
    RELEASE_ASSERT(block && block->mayBeAppendedTo());

    Operation* result = m_proc.arena().create<Operation>(Upsilon, Void, m_origin);
    result->m_auxiliary = 1;
    result->m_children[0] = value;
    result->m_payload.phi = nullptr;
    m_proc.didCreate(result);
    result->m_owner = block;
    result->m_indexInBlock = block->m_upsilons.size();
    block->m_upsilons.append(m_proc.arena(), result);
    value->gainUse();

    if (phi) {
        RELEASE_ASSERT(phi->opcode() == Phi && value->type() == phi->type());
        result->m_payload.phi = phi;
        if (!handsOnClearUpperHalf(value, phi))
            phi->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfMayBeSet);
    }
    return result;
}

Operation* Builder::upsilonOfCompletePhi(Operation* value, Operation* phi)
{
    RELEASE_ASSERT(value->opcode() == Phi && phi->opcode() == Phi && value->type() == phi->type());
    Operation* result = upsilon(value);
    result->m_payload.phi = phi;
    if (!clearsUpperHalfOfRegister(value))
        phi->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfMayBeSet);
    return result;
}

void Builder::setPhi(Operation* upsilon, Operation* phi)
{
    RELEASE_ASSERT(upsilon->opcode() == Upsilon && phi->opcode() == Phi);
    RELEASE_ASSERT(!upsilon->m_payload.phi);
    RELEASE_ASSERT(upsilon->child(0)->type() == phi->type());
#if ASSERT_ENABLED
    // A Jump may stand for a Branch on a constant, whose other way the Upsilon was made for: it is dead then. A Patch that ends its block may still be given successors.
    const Block* block = upsilon->owner();
    if (block->isTerminated() && (block->last()->opcode() == Branch || block->last()->opcode() == Switch))
        ASSERT(machineBuilderEdgeBetween(block, phi->owner()));
#endif
    upsilon->m_payload.phi = phi;
    if (!handsOnClearUpperHalf(upsilon->child(0), phi))
        phi->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfMayBeSet);
}

PatchOperation* Builder::patch(Type type)
{
    sealPendingPatch();
    PatchOperation* result = PatchOperation::create(m_proc, Patch, type, m_origin);
    m_proc.didCreate(result);
    append(result, true);
    m_pendingPatch = result;
    return result;
}

PatchOperation* Builder::check(Operation* predicate)
{
    ASSERT(predicate->type().isInt());
    sealPendingPatch();

    // A Check has one way out, so it cannot test the inverse.
    SimplifiedPredicate simplified = simplifyPredicate(predicate);
    if (!simplified.isInverted)
        predicate = simplified.predicate;
    predicate = duplicateIfFusable(predicate);

    PatchOperation* result = PatchOperation::create(m_proc, Check, Void, m_origin);
    // The late path leaves the function, so the code behind the Check sees nothing of what it does.
    result->effects = Effects::none();
    result->effects.exitsSideways = true;
    result->effects.reads = HeapSet::top();

    if (predicate->isInt(0)) {
        result->append(predicate, ValueRep::ColdAny);
        return result;
    }

    m_proc.didCreate(result);
    append(result, false);
    result->append(predicate, ValueRep::ColdAny);
    m_pendingPatch = result;
    return result;
}

Operation* Builder::findPurePatch(const PureFunction& function, Type type, std::span<Operation* const> operands)
{
    sealPendingPatch();

    if (function.truncatesDoubleToInt64 && type == Int64 && operands.size() == 1 && operands[0]->opcode() == IToD) {
        Operation* integer = operands[0]->child(0);
        if (integer->type() == Int32)
            return unary(SExt32, integer);
        // Every integer of magnitude up to 2^53 is exactly representable as a double.
        constexpr int64_t limit = int64_t { 1 } << 53;
        IntegerRange range = rangeOf(integer);
        if (range.min >= -limit && range.max <= limit)
            return integer;
    }

    return m_numbering.findPurePatch(machineBuilderKeyOfPurePatch(function, type, operands));
}

void Builder::recordPurePatch(PatchOperation* patch)
{
    sealPendingPatch();
    RELEASE_ASSERT(patch->opcode() == Patch && patch->pureFunction && !patch->effects.terminal);
    ASSERT(patch->owner() == m_block);
    m_numbering.addPurePatch(machineBuilderKeyOfPurePatch(*patch->pureFunction, patch->type(), patch->operands()), patch);
}

void Builder::recordInPlaceOfPurePatch(const PureFunction& function, Operation* phi)
{
    sealPendingPatch();
    RELEASE_ASSERT(!function.dependsOnChildren && !function.dependsOnMemory && phi->owner() == m_block);
    m_numbering.addInPlaceOfPurePatch(machineBuilderKeyOfPurePatch(function, phi->type(), { }), phi);
}

void Builder::jump(Edge target)
{
    sealPendingPatch();
    appendNew(Jump, Void);
    m_block->setSuccessorsInline(target);
}

void Builder::branch(Operation* predicate, Edge whenNotZero, Edge whenZero)
{
    ASSERT(predicate->type().isInt());
    sealPendingPatch();

    SimplifiedPredicate simplified = simplifyPredicate(predicate);
    predicate = simplified.predicate;
    if (simplified.isInverted)
        std::swap(whenNotZero, whenZero);

    if (predicate->hasInt()) {
        jump(machineBuilderUnconditionalEdge(predicate->asInt() ? whenNotZero : whenZero));
        return;
    }
    if (whenNotZero.block() == whenZero.block()) {
        jump(machineBuilderUnconditionalEdge(whenNotZero));
        return;
    }

    predicate = duplicateIfFusable(predicate);
    appendNew(Branch, Void, predicate);
    m_block->setSuccessorsInline(whenNotZero, whenZero);
}

void Builder::switchOn(Operation* value, std::span<const SwitchCase> cases, Edge otherwise)
{
    ASSERT(value->type().isInt());
    sealPendingPatch();

    bool isInt32 = value->type() == Int32;
    auto caseValueOf = [&](const SwitchCase& switchCase) -> int64_t {
        return isInt32 ? static_cast<int32_t>(switchCase.caseValue) : switchCase.caseValue;
    };

    if (value->hasInt()) {
        Edge target = otherwise;
        for (const SwitchCase& switchCase : cases) {
            if (caseValueOf(switchCase) == value->asInt()) {
                target = switchCase.target;
                break;
            }
        }
        jump(machineBuilderUnconditionalEdge(target));
        return;
    }
    if (cases.empty()) {
        jump(machineBuilderUnconditionalEdge(otherwise));
        return;
    }

    Arena& arena = m_proc.arena();
    std::span<SwitchCase> sorted = arena.copySpan<SwitchCase>(cases);
    for (SwitchCase& switchCase : sorted)
        switchCase.caseValue = caseValueOf(switchCase);
    std::sort(sorted.begin(), sorted.end(), [](const SwitchCase& a, const SwitchCase& b) {
        return a.caseValue < b.caseValue;
    });
    std::span<int64_t> caseValues = arena.createSpan<int64_t>(sorted.size());
    for (size_t index = 0; index < sorted.size(); ++index) {
        RELEASE_ASSERT(!index || sorted[index - 1].caseValue < sorted[index].caseValue);
        caseValues[index] = sorted[index].caseValue;
    }

    Operation* result = appendNew(Switch, Void, value);
    result->m_payload.switchData = arena.create<SwitchData>(SwitchData { caseValues });
    for (const SwitchCase& switchCase : sorted)
        m_block->appendSuccessor(switchCase.target);
    m_block->appendSuccessor(otherwise);
}

void Builder::ret(Operation* value)
{
    sealPendingPatch();
    appendNew(Return, Void, value);
}

void Builder::retVoid()
{
    sealPendingPatch();
    appendNew(Return, Void);
}

void Builder::oops()
{
    sealPendingPatch();
    appendNew(Oops, Void);
}

Operation* Builder::addWithoutPosition(Operation* base, int64_t constant)
{
    ASSERT(machineBuilderIsBaseOfAddWithoutPosition(base) && base->type() == pointerType() && constant);
    uint64_t payload = static_cast<uint64_t>(constant);
    uint32_t hash = Numbering::hashWithoutPosition(Add, payload, base->index());
    if (Operation* existing = m_numbering.findWithoutPosition(Add, payload, hash, base->index()))
        return existing;
    Operation* addend = const64(constant);
    Operation* result = m_proc.arena().create<Operation>(Add, pointerType(), Origin());
    result->m_auxiliary = 2;
    result->m_children[0] = base;
    result->m_children[1] = addend;
    m_proc.didCreate(result);
    base->gainUse();
    addend->gainUse();
    m_numbering.addWithoutPosition(result, payload, hash);
    return result;
}

void Builder::sealPatch()
{
    PatchOperation* patch = std::exchange(m_pendingPatch, nullptr);
    ASSERT(!patch->effects.terminal || patch->owner()->last() == patch);
    // Other code runs during a suspension, and the frame comes back without the slots that are dead there, whatever the effects say.
    m_numbering.didWrite(patch->isSuspension() ? HeapSet::top() : patch->effects.writes);
    if (patch->isSuspension())
        m_numbering.didSuspend();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
