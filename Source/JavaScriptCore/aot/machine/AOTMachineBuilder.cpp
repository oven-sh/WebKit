/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineBuilder.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineFolding.h"
#include "AOTMachineTarget.h"
#include <algorithm>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Machine {

enum class MachineBuilderFoldFamily : uint8_t {
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
    case RotR:
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
    case RotR:
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

static constexpr std::array<MachineBuilderRulesOfOpcode, sizeOfTableByOpcode> machineBuilderRulesOfOpcodes = [] {
    std::array<MachineBuilderRulesOfOpcode, sizeOfTableByOpcode> rules { };
    for (unsigned opcode = 0; opcode < sizeOfTableByOpcode; ++opcode)
        rules[opcode] = { machineBuilderFoldFamilyOf(static_cast<Opcode>(opcode)), machineBuilderKindOfResult(static_cast<Opcode>(opcode)), machineBuilderLoweringOf(static_cast<Opcode>(opcode)) };
    return rules;
}();

static ALWAYS_INLINE bool machineBuilderSumWithConstantStaysAsItIs(const Operation* left, const Operation* right)
{
    if (!right->hasInt() || !right->asInt())
        return false;
    Opcode opcode = left->opcode();
    return opcode != Const32 && opcode != Const64 && opcode != Add && opcode != Neg;
}

static ALWAYS_INLINE Operation* machineBuilderFold(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* rightOrNull)
{
    switch (machineBuilderRulesOfOpcodes[opcode].foldFamily) {
    case MachineBuilderFoldFamily::Arithmetic:
        if (type.isInt()) {
            if (opcode == Add && machineBuilderSumWithConstantStaysAsItIs(left, rightOrNull))
                return nullptr;
            return foldIntegerArithmetic(builder, opcode, type, left, rightOrNull, false);
        }
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
static constexpr uint64_t machineBuilderPayloadOfChill = 1;
#endif

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

static bool machineBuilderMayBeFusedIntoItsUser(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if (isLoad(opcode)) {
        if (operation->has(OperationFlag::ReadsWordFilledOnce))
            return false;
        return operation->child(0)->hasNoPosition() || (isX86_64() && operation->type() != Int64 && operation->useCount() < 2);
    }
    if (opcode == ZExt32)
        return isARM64() && !clearsUpperHalf(operation->child(0));
    if (opcode == SExt32)
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

static unsigned machineBuilderRankAmongOperands(const Operation* operation)
{
    if (operation->has(OperationFlag::IsInnerPointer))
        return 0;
    if (operation->hasNoPosition())
        return 1;
    return operation->opcode() == Load && operation->type().isFloat() ? 3 : 2;
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

static ALWAYS_INLINE MachineBuilderAddress machineBuilderAddressOf(Builder& builder, Operation* base, ptrdiff_t offset, bool mayChangeBase)
{
    int64_t distance = offset;
    while (mayChangeBase && base->opcode() == Add && base->child(1)->hasInt64()) {
        int64_t constant = base->child(1)->asInt64();
        if (sumOverflows<int64_t>(distance, constant) || !machineBuilderFitsInt32(distance + constant))
            break;
        distance += constant;
        base = base->child(0);
    }
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

static bool accessUsesSharedSum(const MachineBuilderAddress& address)
{
    if (!isARM64() || !address.offset || address.base->opcode() != Add || address.base->hasNoPosition())
        return false;
    const Operation* pointer = address.base->child(0);
    return pointer->isArgumentReg(cellCageBaseGPR) || pointer->has(OperationFlag::IsInnerPointer);
}

#if CPU(ARM64)
struct MachineBuilderIndexOfSum {
    explicit MachineBuilderIndexOfSum(const Operation* sum)
        : word(sum->child(1))
    {
        if (word->opcode() == Shl && word->child(1)->hasInt32()) {
            shift = static_cast<unsigned>(word->child(1)->asInt32()) & 63;
            word = word->child(0);
        }
        if (word->opcode() == ZExt32 || word->opcode() == SExt32) {
            extension = word->opcode();
            word = word->child(0);
        }
    }
    friend bool operator==(const MachineBuilderIndexOfSum&, const MachineBuilderIndexOfSum&) = default;

    const Operation* word;
    unsigned shift { 0 };
    Opcode extension { Nop };
};

Operation* Builder::sharedSumFor(Operation* sum)
{
    ASSERT(sum->opcode() == Add && sum->type() == Int64);
    MachineBuilderIndexOfSum index(sum);
    Operation*& last = m_lastSharedSums[(sum->child(0)->index() * 7 + index.word->index() * 3 + index.shift) % m_lastSharedSums.size()];
    if (last && last->child(0) == sum->child(0) && MachineBuilderIndexOfSum(last) == index) {
        NumberingKey key = machineBuilderKeyOf(Add, Int64, last->child(0), last->child(1));
        if (Operation* visible = findNumbered(key, Numbering::hashOf(key)))
            return visible;
    }
    last = sum;
    return sum;
}
#endif

static constexpr unsigned machineBuilderNumberOfSetsOfHeapTraits = 32;
static_assert(static_cast<unsigned>(HeapTrait::Immutable) < machineBuilderNumberOfSetsOfHeapTraits && static_cast<unsigned>(HeapTrait::HoldsInnerPointer) < machineBuilderNumberOfSetsOfHeapTraits);
static_assert(static_cast<unsigned>(HeapTrait::HoldsPermanentCell) < machineBuilderNumberOfSetsOfHeapTraits && static_cast<unsigned>(HeapTrait::IsFilledOnce) < machineBuilderNumberOfSetsOfHeapTraits);

static constexpr std::array<uint16_t, machineBuilderNumberOfSetsOfHeapTraits> machineBuilderFlagsOfLoadsByHeapTraits = [] {
    std::array<uint16_t, machineBuilderNumberOfSetsOfHeapTraits> flagsOfLoads { };
    for (unsigned traits = 0; traits < machineBuilderNumberOfSetsOfHeapTraits; ++traits) {
        uint16_t flags = 0;
        if (traits & static_cast<unsigned>(HeapTrait::Immutable))
            flags |= static_cast<uint16_t>(OperationFlag::ReadsImmutableHeap);
        if (traits & static_cast<unsigned>(HeapTrait::HoldsInnerPointer))
            flags |= static_cast<uint16_t>(OperationFlag::IsInnerPointer);
        if (traits & static_cast<unsigned>(HeapTrait::HoldsPermanentCell))
            flags |= static_cast<uint16_t>(OperationFlag::NamesPermanentCell);
        if (traits & static_cast<unsigned>(HeapTrait::IsFilledOnce))
            flags |= static_cast<uint16_t>(OperationFlag::ReadsWordFilledOnce);
        flagsOfLoads[traits] = flags;
    }
    return flagsOfLoads;
}();

static uint16_t machineBuilderFlagsOfLoadFrom(const AbstractHeap& heap)
{
    return machineBuilderFlagsOfLoadsByHeapTraits[heap.traits().toRaw() & (machineBuilderNumberOfSetsOfHeapTraits - 1)];
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

static Edge machineBuilderUnconditionalEdge(Edge onlyWayLeft)
{
    return Edge(onlyWayLeft.block(), EdgeClass::Normal);
}

static_assert(alignof(Operation) <= Arena::alignmentOfCursor && alignof(PatchOperation) <= Arena::alignmentOfCursor);

Builder::Builder(Procedure& procedure)
    : m_proc(procedure)
    , m_numbering(procedure)
{
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Builder::hasRoomToCreate(size_t bytes) const
{
    const Procedure& procedure = m_proc;
    return procedure.m_operations.size() != procedure.m_operations.capacity() && procedure.m_arena.hasRoomFor(bytes);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Builder::hasRoomToAppendTo(const Block* block, size_t bytes) const
{
    return block->m_operations.size() != block->m_operations.capacity() && hasRoomToCreate(bytes);
}

template<bool hasRoom>
ALWAYS_INLINE void Builder::didCreate(Operation* operation)
{
    Procedure& procedure = m_proc;
    operation->m_index = procedure.m_operations.size();
    if constexpr (hasRoom)
        procedure.m_operations.appendWithinCapacity(operation);
    else
        procedure.m_operations.append(procedure.m_arena, operation);
}

template<bool hasRoom>
ALWAYS_INLINE void Builder::append(Operation* operation, bool mayEndBlock)
{
    Block* block = m_block;
    RELEASE_ASSERT(block && block->mayBeAppendedTo());
    block->m_lastOperationMayEndBlock = mayEndBlock;
    operation->m_owner = block;
    operation->m_indexInBlock = block->m_operations.size();
    if constexpr (hasRoom)
        block->m_operations.appendWithinCapacity(operation);
    else
        block->m_operations.append(m_proc.arena(), operation);
}

template<unsigned numberOfChildren, KindOfAppendedOperation kind, bool hasRoom>
ALWAYS_INLINE Operation* Builder::appendNewWithChildren(Opcode opcode, Type type, Operation* first, Operation* second, Operation* third)
{
    static_assert(numberOfChildren <= Operation::maxInlineChildren);
    Block* block = m_block;
    RELEASE_ASSERT(block && block->mayBeAppendedTo());
    Procedure& procedure = m_proc;
    Arena& arena = procedure.m_arena;
    void* memory = hasRoom ? arena.allocateWhereThereIsRoom(sizeof(Operation)) : arena.allocate(sizeof(Operation), alignof(Operation));
    Operation* result = new (NotNull, memory) Operation(opcode, type, m_origin, procedure.m_operations.size(), block, block->m_operations.size(), static_cast<uint8_t>(numberOfChildren), first, second, third);
    if constexpr (hasRoom) {
        procedure.m_operations.appendWithinCapacity(result);
        block->m_operations.appendWithinCapacity(result);
    } else {
        procedure.m_operations.append(arena, result);
        block->m_operations.append(arena, result);
    }

    bool readsChildrenAtOnce;
    if constexpr (kind == KindOfAppendedOperation::Computation) {
        ASSERT(countsChildrenAtFirstUse(opcode));
        block->m_lastOperationMayEndBlock = false;
        readsChildrenAtOnce = false;
    } else if constexpr (kind == KindOfAppendedOperation::MemoryAccess) {
        ASSERT(isMemoryAccess(opcode));
        block->m_lastOperationMayEndBlock = false;
        readsChildrenAtOnce = true;
    } else {
        block->m_lastOperationMayEndBlock = opcode >= Patch;
        readsChildrenAtOnce = !countsChildrenAtFirstUse(opcode);
    }
    if (readsChildrenAtOnce) {
        if constexpr (kind == KindOfAppendedOperation::MemoryAccess && numberOfChildren > 1)
            first->gainUse(static_cast<uint16_t>(OperationFlag::UpperHalfIsRead));
        else if constexpr (numberOfChildren > 0)
            first->gainUse();
        if constexpr (numberOfChildren > 1)
            second->gainUse();
        if constexpr (numberOfChildren > 2)
            third->gainUse();
    }
    return result;
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNew(Opcode opcode, Type type)
{
    return appendNewWithChildren<0, KindOfAppendedOperation::Any, false>(opcode, type, nullptr, nullptr, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNew(Opcode opcode, Type type, Operation* child)
{
    return appendNewWithChildren<1, KindOfAppendedOperation::Any, false>(opcode, type, child, nullptr, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNew(Opcode opcode, Type type, Operation* first, Operation* second)
{
    return appendNewWithChildren<2, KindOfAppendedOperation::Any, false>(opcode, type, first, second, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNewMemoryAccess(Opcode opcode, Type type, Operation* child)
{
    return appendNewWithChildren<1, KindOfAppendedOperation::MemoryAccess, false>(opcode, type, child, nullptr, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNewMemoryAccess(Opcode opcode, Type type, Operation* first, Operation* second)
{
    return appendNewWithChildren<2, KindOfAppendedOperation::MemoryAccess, false>(opcode, type, first, second, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNewComputation(Opcode opcode, Type type, Operation* child)
{
    return appendNewWithChildren<1, KindOfAppendedOperation::Computation, false>(opcode, type, child, nullptr, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNewComputation(Opcode opcode, Type type, Operation* first, Operation* second)
{
    return appendNewWithChildren<2, KindOfAppendedOperation::Computation, false>(opcode, type, first, second, nullptr);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::appendNewComputation(Opcode opcode, Type type, Operation* first, Operation* second, Operation* third)
{
    return appendNewWithChildren<3, KindOfAppendedOperation::Computation, false>(opcode, type, first, second, third);
}

template<bool hasRoom>
ALWAYS_INLINE Operation* Builder::createNewWithoutPosition(Opcode opcode, Type type, uint64_t payload, uint32_t hash)
{
    Arena& arena = m_proc.m_arena;
    void* memory = hasRoom ? arena.allocateWhereThereIsRoom(sizeof(Operation)) : arena.allocate(sizeof(Operation), alignof(Operation));
    Operation* result = new (NotNull, memory) Operation(opcode, type, Origin());
    result->m_payload.bits = static_cast<int64_t>(payload);
    didCreate<hasRoom>(result);
    if constexpr (hasRoom)
        m_numbering.addWithoutPositionWhereThereIsRoom(result, payload, hash);
    else
        m_numbering.addWithoutPosition(result, payload, hash);
    return result;
}

NEVER_INLINE Operation* Builder::createNewWithoutPositionWhereRoomMayLack(Opcode opcode, Type type, uint64_t payload, uint32_t hash)
{
    return createNewWithoutPosition<false>(opcode, type, payload, hash);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::createWithoutPosition(Opcode opcode, Type type, uint64_t payload)
{
    uint32_t hash = Numbering::hashWithoutPosition(opcode, payload);
    if (Operation* existing = m_numbering.findWithoutPosition(opcode, payload, hash))
        return existing;
    if (!hasRoomToCreate(sizeof(Operation)) || !m_numbering.hasRoomWithoutPosition()) [[unlikely]]
        return createNewWithoutPositionWhereRoomMayLack(opcode, type, payload, hash);
    return createNewWithoutPosition<true>(opcode, type, payload, hash);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Builder::noteInheritedFind(const Operation* operation)
{
    uint32_t index = operation->index();
    if (index >= m_epochOfLastInheritedFind.size()) {
        m_epochOfLastInheritedFind.grow(m_proc.arena(), std::max(m_proc.numberOfOperations(), m_epochOfLastInheritedFind.size() * 2));
        m_proc.noteListThatBuildingLeaves(Procedure::ListThatBuildingLeaves::EpochsOfInheritedFinds, m_epochOfLastInheritedFind);
    }
    m_epochOfLastInheritedFind[index] = m_scopeEpoch;
    m_hasInheritedFindsInEpoch = true;
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::noteIfInherited(const Numbering::Found& found)
{
    if (found.isInherited) [[unlikely]]
        noteInheritedFind(found.operation);
    return found.operation;
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::findNumbered(const NumberingKey& key, uint32_t hash)
{
    return noteIfInherited(m_numbering.lookUp(key, hash, m_isDuplicating ? Numbering::Reach::OwnEntries : Numbering::Reach::WholeScope));
}

ALWAYS_INLINE_INTO_OTHER_FILES Operation* Builder::findNumberedLoad(const NumberingKey& key, uint32_t hash)
{
    return noteIfInherited(m_numbering.lookUpLoad(key, hash, m_isDuplicating ? Numbering::Reach::OwnEntries : Numbering::Reach::WholeScope));
}

ALWAYS_INLINE_INTO_OTHER_FILES Type Builder::typeOfResult(Opcode opcode, Operation* first)
{
    TypeKind kind = machineBuilderRulesOfOpcodes[opcode].kindOfResult;
    RELEASE_ASSERT(kind != machineBuilderKindThatOpcodeDoesNotSay);
    return kind == machineBuilderKindOfFirstChild ? first->type() : Type(kind);
}

Operation* Builder::duplicateInheritedFindIfFusable(Operation* operand)
{
    ASSERT(wasFoundInInheritedScope(operand));
    if (!machineBuilderMayBeFusedIntoItsUser(operand))
        return operand;

    Operation* copy;
    if (isLoad(operand->opcode())) {
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
    if (operand->isNarrowReference() && !copy->isConstant())
        copy->markAsNarrowReference();
    return copy;
}

Operation* Builder::const32Slow(int32_t value)
{
    Operation** kept = static_cast<uint32_t>(value) < numberOfSmallConstantsKept ? &m_smallConstants32[static_cast<uint32_t>(value)] : nullptr;
    Operation* result = createWithoutPosition(Const32, Int32, static_cast<uint64_t>(static_cast<int64_t>(value)));
    if (kept)
        *kept = result;
    return result;
}

Operation* Builder::const64Slow(int64_t value)
{
    Operation** kept = static_cast<uint64_t>(value) < numberOfSmallConstantsKept ? &m_smallConstants64[static_cast<uint64_t>(value)] : nullptr;
    Operation* result = createWithoutPosition(Const64, Int64, static_cast<uint64_t>(value));
    if (kept)
        *kept = result;
    return result;
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
    if (opcode == ZExt32) {
        NumberingKey keyWithProof = machineBuilderKeyOf(opcode, type, value, nullptr, nullptr, machineBuilderPayloadOfWordThatIsNotNegative);
        if (Operation* existing = findNumbered(keyWithProof, Numbering::hashOf(keyWithProof)))
            return existing;
    }
    if (Operation* ownValue = duplicateIfFusable(value); ownValue != value)
        return unary(opcode, ownValue);
    Operation* result = appendNewComputation(opcode, type, value);
    if (opcode == ZExt32 || opcode == Trunc)
        m_proc.noteWordName(result);
    m_numbering.add(key, hash, result);
    return result;
}

Operation* Builder::extendWordThatIsNotNegative(Operation* word)
{
    ASSERT(word->type() == Int32);
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
    m_proc.noteWordName(result);
    m_numbering.add(key, hash, result);
    return result;
}

bool Builder::findsAgain(Operation* sumOrProduct)
{
    if (sumOrProduct->opcode() != Add && sumOrProduct->opcode() != Mul)
        return false;
    NumberingKey key = machineBuilderKeyOf(sumOrProduct->opcode(), sumOrProduct->type(), sumOrProduct->child(0), sumOrProduct->child(1));
    return findNumbered(key, Numbering::hashOf(key)) == sumOrProduct;
}

Operation* Builder::binary(Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(descriptionOf(opcode).numberOfChildren == 2);
    ASSERT(right->type() == (opcode == Shl || opcode == SShr || opcode == ZShr || opcode == RotR ? Type(Int32) : left->type()));
    ASSERT(opcode != RotR || right->hasInt32());
    sealPendingPatch();

    bool comparesLinkedNumber = (opcode == Equal || opcode == NotEqual) && (left->opcode() == NumberOf) != (right->opcode() == NumberOf) && !left->isConstant() && !right->isConstant();
    if (comparesLinkedNumber ? left->opcode() == NumberOf : isCommutative(opcode) && machineBuilderShouldSwapOperands(left, right))
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
    if (m_hasInheritedFindsInEpoch) [[unlikely]] {
        Operation* ownLeft = duplicateIfFusable(left);
        Operation* ownRight = duplicateIfFusable(right);
        if (ownLeft != left || ownRight != right)
            return binary(opcode, ownLeft, ownRight);
    }
    MachineBuilderLowering lowering = machineBuilderRulesOfOpcodes[opcode].lowering;
    if (lowering != MachineBuilderLowering::Never && (lowering == MachineBuilderLowering::Always || !right->hasInt32())) [[unlikely]] {
        if (Operation* lowered = lowerForTarget(opcode, type, left, right))
            return lowered;
    }

    Operation* ownShift = opcode == Add && right->opcode() == Shl ? makeOwnShiftOfSum(right) : nullptr;
    Operation* result = appendNewComputation(opcode, type, left, right);
    if (ownShift)
        m_proc.noteOwnShiftOfSum(result, ownShift);
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
    Operation* denominatorIsSafe = denominator;
#else
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
    simplified.predicate->m_flags |= static_cast<uint16_t>(OperationFlag::HasOpaqueReader);
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

Operation* Builder::numberOf(CodeSymbol symbol, Type type)
{
    ASSERT(symbol.space == SymbolSpace::Number);
    constexpr uint64_t isWide = uint64_t { 1 } << 63;
    Operation* result = createWithoutPosition(NumberOf, type, machineBuilderPayloadOfSymbol(symbol) | (type == Int64 ? isWide : 0));
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

    MachineBuilderAddress address = machineBuilderAddressOf(*this, base, offset, !traps);
    bool usesSharedSum = !traps && accessUsesSharedSum(address);
    if (usesSharedSum)
        address.base = sharedSumFor(address.base);
    NumberingKey key = machineBuilderKeyOf(opcode, type, address.base, nullptr, nullptr, machineBuilderPayloadOfAccess(address.offset, heap.index()));
    uint32_t hash = 0;
    if (!traps) {
        hash = Numbering::hashOf(key);
        if (Operation* existing = findNumberedLoad(key, hash)) {
            if (m_verifierOfReusedLoads) [[unlikely]]
                verifyReusedLoad(existing, address.base, address.offset);
            return existing;
        }
    }
    if (!usesSharedSum) {
        if (Operation* ownBase = duplicateIfFusable(address.base); ownBase != address.base)
            return load(opcode, type, ownBase, address.offset, heap, trapping);
    }

    Operation* result = appendNewMemoryAccess(opcode, type, address.base);
    result->m_payload.memory.offset = address.offset;
    result->m_payload.memory.heap = heap.index();
    result->m_flags |= machineBuilderFlagsOfLoadFrom(heap);
    if (traps)
        result->m_flags |= static_cast<uint16_t>(OperationFlag::Traps);
    else {
        m_numbering.addLoad(key, hash, result);
        if (m_verifierOfReusedLoads) [[unlikely]]
            noteNumberedLoadForStores(result);
    }
    return result;
}

static bool machineBuilderUpdatesLoadedWord(const Operation* stored, const Operation* address)
{
    const Operation* computation = stored->opcode() == Trunc ? stored->child(0) : stored;
    Opcode opcode = computation->opcode();
    if (opcode != Add && opcode != Sub && opcode != BitAnd && opcode != BitOr && opcode != BitXor)
        return false;
    return std::ranges::any_of(computation->children(), [&](const Operation* operand) {
        if (operand->opcode() == SExt32)
            operand = operand->child(0);
        return operand->opcode() == Load && operand->lastChild() == address;
    });
}

Operation* Builder::makeOwnShiftOfSum(Operation* shift)
{
    if (shift->type() != Int64 || shift->isNarrowReference() || !shift->child(1)->hasInt32() || shift->child(0)->hasNoPosition() || shift->child(0)->isNarrowReference()
        || !isEncodableIndexedAddress(Width64, static_cast<unsigned>(shift->child(1)->asInt32()) & 63, IndexExtension::None, 0))
        return nullptr;
    if (!shift->has(OperationFlag::ScalesIndexOfSum)) {
        shift->m_flags |= static_cast<uint16_t>(OperationFlag::ScalesIndexOfSum);
        return nullptr;
    }
    return appendNewComputation(Shl, Int64, shift->child(0), shift->child(1));
}

Operation* Builder::makeSumOfStore(Operation* sum, const Operation* stored)
{
    if (!isX86_64() || sum->opcode() != Add || sum->hasNoPosition() || sum->isNarrowReference())
        return duplicateIfFusable(sum);
    Operation* base = sum->child(0);
    Operation* index = sum->child(1);
    Operation* amount = index->opcode() == Shl && index->child(1)->hasInt32() ? index->child(1) : nullptr;
    Operation* scaled = amount ? index->child(0) : index;
    if (!wasFoundInInheritedScope(sum) && machineBuilderUpdatesLoadedWord(stored, sum))
        return sum;
    if (base->hasNoPosition() || scaled->hasNoPosition() || scaled->isNarrowReference() || !isEncodableIndexedAddress(Width64, amount ? static_cast<unsigned>(amount->asInt32()) & 63 : 0, IndexExtension::None, 0))
        return duplicateIfFusable(sum);
    Operation* own = appendNewComputation(Add, Int64, base, amount ? appendNewComputation(Shl, Int64, scaled, amount) : scaled);
    own->m_flags |= sum->m_flags & static_cast<uint16_t>(OperationFlag::IsInnerPointer);
    return own;
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
    Operation* ownBase = accessUsesSharedSum(address) ? sharedSumFor(address.base) : makeSumOfStore(address.base, value);
    Operation* result = appendNewMemoryAccess(opcode, Void, ownValue, ownBase);
    result->m_payload.memory.offset = address.offset;
    result->m_payload.memory.heap = heap.index();
    result->m_owner->m_writesMemory = true;
    m_numbering.didWrite(heap.index());
    if (m_verifierOfReusedLoads) [[unlikely]]
        stopIfLoadOfStoredWordStands(result);
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
    if (m_verifierOfReusedLoads) [[unlikely]]
        noteNumberedLoadForStores(result);
    return result;
}

void Builder::forgetLoadsFrom(const AbstractHeap& heap)
{
    sealPendingPatch();
    m_numbering.didWrite(heap.index());
}

Operation* Builder::appendLoadLike(const Operation* load, Operation* base)
{
    constexpr uint16_t flagsOfHeap = static_cast<uint16_t>(OperationFlag::ReadsImmutableHeap) | static_cast<uint16_t>(OperationFlag::IsInnerPointer) | static_cast<uint16_t>(OperationFlag::NamesPermanentCell);
    Operation* result = appendNewMemoryAccess(load->opcode(), load->type(), base);
    result->m_payload.memory.offset = load->offset();
    result->m_payload.memory.heap = load->heap();
    result->m_flags |= load->m_flags & flagsOfHeap;
    return result;
}

void Builder::verifyReusedLoads(std::initializer_list<HeapIndex> writtenByOtherThreads, Function<void(Operation* readSameBits)>&& verifier)
{
    m_heapsWrittenByOtherThreads = writtenByOtherThreads;
    m_verifierOfReusedLoads = WTF::move(verifier);
}

bool Builder::isWrittenByOtherThreads(HeapIndex heap) const
{
    return std::ranges::any_of(m_heapsWrittenByOtherThreads, [&](HeapIndex written) {
        return m_proc.heapTree().isSameOrAbove(written, heap);
    });
}

static uint64_t machineBuilderPlaceOf(const Operation* base, int64_t granule)
{
    return static_cast<uint64_t>(base->index()) << 32 | static_cast<uint32_t>(granule);
}

void Builder::noteNumberedLoadForStores(Operation* load)
{
    if (load->has(OperationFlag::ReadsImmutableHeap) || isWrittenByOtherThreads(load->heap()))
        return;
    int64_t begin = load->offset();
    int64_t end = begin + bytesForWidth(load->accessWidth());
    for (int64_t granule = begin >> 3; granule <= (end - 1) >> 3; ++granule) {
        Vector<Operation*, 2>& loads = m_numberedLoadsByPlace.add(machineBuilderPlaceOf(load->lastChild(), granule), Vector<Operation*, 2>()).iterator->value;
        size_t sameKey = loads.findIf([&](const Operation* other) {
            return other->opcode() == load->opcode() && other->type() == load->type() && other->offset() == load->offset() && other->heap() == load->heap();
        });
        if (sameKey == notFound)
            loads.append(load);
        else
            loads[sameKey] = load;
    }
}

void Builder::stopIfLoadOfStoredWordStands(const Operation* store)
{
    if (isWrittenByOtherThreads(store->heap()))
        return;
    int64_t begin = store->offset();
    int64_t end = begin + bytesForWidth(store->accessWidth());
    for (int64_t granule = begin >> 3; granule <= (end - 1) >> 3; ++granule) {
        auto found = m_numberedLoadsByPlace.find(machineBuilderPlaceOf(store->lastChild(), granule));
        if (found == m_numberedLoadsByPlace.end())
            continue;
        for (Operation* load : found->value) {
            if (load->offset() < end && begin < load->offset() + static_cast<int64_t>(bytesForWidth(load->accessWidth())) && m_numbering.isStillValid(machineBuilderKeyOfLoad(load, load->child(0)), load)) {
                m_proc.stopAtStoreOverLoadThatStands(store);
                return;
            }
        }
    }
}

void Builder::verifyReusedLoad(Operation* load, Operation* baseAsked, int32_t offsetAsked)
{
    if (m_isVerifyingReusedLoad || load->has(OperationFlag::ReadsImmutableHeap) || isWrittenByOtherThreads(load->heap()) || load->type() == Vector128)
        return;
    SetForScope<bool> isVerifying(m_isVerifyingReusedLoad, true);

    Operation* readBefore = load;
    Operation* readNow = appendLoadLike(load, baseAsked);
    readNow->m_payload.memory.offset = offsetAsked;
    if (!load->type().isInt()) {
        Type typeOfBits = load->type() == Double ? Int64 : Int32;
        readBefore = appendNew(BitwiseCast, typeOfBits, readBefore);
        readNow = appendNew(BitwiseCast, typeOfBits, readNow);
    }
    m_verifierOfReusedLoads(appendNew(Equal, Int32, readBefore, readNow));
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
    appendNew(Fence, Void)->m_owner->m_writesMemory = true;
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
    m_proc.fail(FailureKind::AccessOutsideDeclaredEffects, appendNew(Nop, Void));
}

Operation* Builder::phiIn(Block* block, Type type)
{
    ASSERT(block && type.isNumeric());
    Procedure& procedure = m_proc;
    Arena& arena = procedure.m_arena;
    Operation* result = arena.create<Operation>(Phi, type, m_origin, procedure.m_operations.size(), block, block->m_phis.size(), static_cast<uint8_t>(0), nullptr, nullptr, nullptr);
    procedure.m_operations.append(arena, result);
    if (!block->m_phis.capacity())
        block->m_phis.adoptStorage(arena.allocateArray<Operation*>(Block::firstCapacityOfPhis), Block::firstCapacityOfPhis);
    block->m_phis.append(arena, result);
    return result;
}

template<bool hasRoom>
ALWAYS_INLINE Operation* Builder::appendUpsilon(Operation* value, Operation* phi)
{
    Block* block = m_block;
    RELEASE_ASSERT(block && block->mayBeAppendedTo());

    Procedure& procedure = m_proc;
    Arena& arena = procedure.m_arena;
    void* memory = hasRoom ? arena.allocateWhereThereIsRoom(sizeof(Operation)) : arena.allocate(sizeof(Operation), alignof(Operation));
    Operation* result = new (NotNull, memory) Operation(Upsilon, Void, m_origin, procedure.m_operations.size(), block, block->m_upsilons.size(), static_cast<uint8_t>(1), value, nullptr, nullptr);
    if constexpr (hasRoom) {
        procedure.m_operations.appendWithinCapacity(result);
        block->m_upsilons.appendWithinCapacity(result);
    } else {
        procedure.m_operations.append(arena, result);
        block->m_upsilons.append(arena, result);
    }

    if (phi) {
        RELEASE_ASSERT(phi->opcode() == Phi && value->type() == phi->type());
        result->m_payload.phi = phi;
        procedure.noteUpperHalfHandedOn(result);
    }
    if (value->gainUseLeavingChildrenUncounted()) [[unlikely]]
        return countChildrenOfValueHandedOn(value, result);
    return result;
}

NEVER_INLINE Operation* Builder::countChildrenOfValueHandedOn(Operation* value, Operation* upsilon)
{
    value->countChildrenAtFirstUse();
    return upsilon;
}

NEVER_INLINE Operation* Builder::upsilonWhereRoomMayLack(Operation* value, Operation* phi)
{
    sealPendingPatch();
    return appendUpsilon<false>(value, phi);
}

Operation* Builder::upsilon(Operation* value, Operation* phi)
{
    const Block* block = m_block;
    if (m_pendingPatch || !block || block->m_upsilons.size() == block->m_upsilons.capacity() || !hasRoomToCreate(sizeof(Operation))) [[unlikely]]
        return upsilonWhereRoomMayLack(value, phi);
    return appendUpsilon<true>(value, phi);
}

void Builder::setPhi(Operation* upsilon, Operation* phi)
{
    RELEASE_ASSERT(upsilon->opcode() == Upsilon && phi->opcode() == Phi);
    RELEASE_ASSERT(!upsilon->m_payload.phi);
    RELEASE_ASSERT(upsilon->child(0)->type() == phi->type());
#if ASSERT_ENABLED
    const Block* block = upsilon->owner();
    if (block->isTerminated() && (block->last()->opcode() == Branch || block->last()->opcode() == Switch))
        ASSERT(hasEdgeBetween(block, phi->owner()));
#endif
    upsilon->m_payload.phi = phi;
    m_proc.noteUpperHalfHandedOn(upsilon);
}

template<bool hasRoom>
ALWAYS_INLINE PatchOperation* Builder::appendPatch(Type type)
{
    Procedure& procedure = m_proc;
    void* memory = hasRoom ? procedure.m_arena.allocateWhereThereIsRoom(PatchOperation::bytesOfNewPatch()) : procedure.m_arena.allocate(PatchOperation::bytesOfNewPatch(), alignof(PatchOperation));
    PatchOperation* result = PatchOperation::createAt(memory, procedure, Patch, type, m_origin);
    didCreate<hasRoom>(result);
    append<hasRoom>(result, true);
    m_pendingPatch = result;
    return result;
}

NEVER_INLINE PatchOperation* Builder::patchWhereRoomMayLack(Type type)
{
    sealPendingPatch();
    return appendPatch<false>(type);
}

PatchOperation* Builder::patch(Type type)
{
    const Block* block = m_block;
    if (m_pendingPatch || !block || !hasRoomToAppendTo(block, PatchOperation::bytesOfNewPatch())) [[unlikely]]
        return patchWhereRoomMayLack(type);
    return appendPatch<true>(type);
}

PatchOperation* Builder::check(Operation* predicate)
{
    ASSERT(predicate->type().isInt());
    sealPendingPatch();

    SimplifiedPredicate simplified = simplifyPredicate(predicate);
    bool failsWhenPredicateIsZero = simplified.isInverted && isComparison(simplified.predicate->opcode()) && !simplified.predicate->child(0)->type().isInt();
    if (!simplified.isInverted || failsWhenPredicateIsZero)
        predicate = simplified.predicate;
    predicate = duplicateIfFusable(predicate);
    RELEASE_ASSERT(!failsWhenPredicateIsZero || isComparison(predicate->opcode()));

    PatchOperation* result = PatchOperation::create(m_proc, Check, Void, m_origin);
    result->setFailsWhenPredicateIsZero(failsWhenPredicateIsZero);
    result->effects = Effects::none();
    result->effects.exitsSideways = true;
    result->effects.reads = HeapSet::top();

    if (predicate->isInt(0)) {
        result->append(predicate, ValueRep::ColdAny);
        return result;
    }

    didCreate<false>(result);
    append<false>(result, false);
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

void Builder::recordInPlaceOfReadOfMemory(const PureFunction& function, Operation* phi, Operation* operand, const AbstractHeap& heap)
{
    sealPendingPatch();
    RELEASE_ASSERT(function.dependsOnChildren && function.dependsOnMemory && phi->opcode() == Phi && phi->owner() == m_block);
    Operation* operands[] { operand };
    m_numbering.addInPlaceOfReadOfMemory(machineBuilderKeyOfPurePatch(function, phi->type(), operands), phi, heap.index());
}

void Builder::recordInPlaceOfPurePatch(const PureFunction& function, Operation* phi)
{
    sealPendingPatch();
    RELEASE_ASSERT(!function.dependsOnChildren && !function.dependsOnMemory && phi->owner() == m_block);
    m_numbering.addInPlaceOfPurePatch(machineBuilderKeyOfPurePatch(function, phi->type(), { }), phi);
}

NEVER_INLINE void Builder::jumpWhereRoomMayLack(Edge target)
{
    sealPendingPatch();
    appendNew(Jump, Void);
    m_block->setSuccessorsInline(target);
}

void Builder::jump(Edge target)
{
    Block* block = m_block;
    if (m_pendingPatch || !block || !block->m_successors.isEmpty() || !hasRoomToAppendTo(block, sizeof(Operation))) [[unlikely]]
        return jumpWhereRoomMayLack(target);
    appendNewWithChildren<0, KindOfAppendedOperation::Any, true>(Jump, Void, nullptr, nullptr, nullptr);
    block->appendSuccessorWhereRoomIsKnown<true>(target);
}

bool Builder::tryToLeadBranchPastEmptyBlock(Edge target)
{
    Block* block = m_block;
    if (m_pendingPatch || !block || m_proc.m_blocks.last() != block)
        return false;
    if (!block->isEmpty() || !block->m_phis.isEmpty() || !block->m_upsilons.isEmpty() || !block->m_successors.isEmpty() || block->m_isLandingPad || block->m_isEntry)
        return false;
    Block* from = block->singlePredecessor();
    if (!from || from->numSuccessors() != 2 || from->last()->opcode() != Branch)
        return false;
    unsigned index = from->successor(0).block() == block ? 0 : 1;
    Block* newTarget = target.block();
    if (from->successor(index).edgeClass() != EdgeClass::Normal || newTarget == block || from->successor(1 - index).block() == newTarget || from->successor(1 - index).block() == block)
        return false;

    uint32_t numberOfIncomingEdges = newTarget->m_numberOfIncomingEdges;
    RELEASE_ASSERT(!newTarget->m_isEntry && (!newTarget->m_hasOnlyOnePredecessor || !numberOfIncomingEdges));
    from->m_successors[index] = target;
    newTarget->didGainIncomingEdge(from, target.edgeClass());
    m_proc.m_blocks.takeLast();
    m_block = from;
    return true;
}

void Builder::branch(Operation* predicate, Edge whenNotZero, Edge whenZero)
{
    ASSERT(predicate->type().isInt());
    sealPendingPatch();

    SimplifiedPredicate simplified = peelPredicateInline(predicate);
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
    if (patch->isSuspension()) [[unlikely]] {
        patch->m_owner->m_writesMemory = true;
        m_numbering.didWrite(HeapSet::top());
        m_numbering.didSuspend();
        return;
    }
    if (!patch->effects.writes.isEmpty() || (patch->opcode() == Patch && !patch->onlyKeepsChildrenAlive))
        patch->m_owner->m_writesMemory = true;
    m_numbering.didWrite(patch->effects.writes);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
