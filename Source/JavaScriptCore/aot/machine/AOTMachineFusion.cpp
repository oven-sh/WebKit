/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineLiveness.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineFolding.h"
#include "AOTSlotRepresentation.h"
#include <algorithm>
#include <limits>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static_assert(Operation::maxInlineChildren + 1 <= maxLeaves, "a Select has room for both operands of the comparison that it tests");
static_assert(maxOperationsBetweenLoadAndUser + 1 <= UINT8_MAX, "ValueFacts::distanceToTrappingLoad has room for the way from a trapping load to the access that takes its trap over");

static constexpr bool fusionIsComparison(Opcode opcode) { return opcode >= Equal && opcode <= EqualOrUnordered; }
static constexpr bool fusionIsLoad(Opcode opcode) { return opcode >= Load8Z && opcode <= Load; }
static constexpr bool fusionIsStore(Opcode opcode) { return opcode >= Store8 && opcode <= Store; }
static_assert([] {
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        if (fusionIsComparison(opcode) != isComparison(opcode) || fusionIsLoad(opcode) != isLoad(opcode) || fusionIsStore(opcode) != isStore(opcode))
            return false;
    }
    return true;
}());

template<Opcode... opcodes>
constexpr bool fusionOpcodesHaveChildren(unsigned numberOfChildren)
{
    return ((descriptionOf(opcodes).numberOfChildren == numberOfChildren) && ...);
}
static_assert(fusionOpcodesHaveChildren<Neg, Clz, PopCount, Abs, Ceil, Floor, FTrunc, Sqrt, BitwiseCast, SExt8, SExt16, SExt32, ZExt32, Trunc, IToD, FloatToDouble, DoubleToFloat>(1));
static_assert(fusionOpcodesHaveChildren<Load8Z, Load8S, Load16Z, Load16S, Load, Branch>(1));
static_assert(fusionOpcodesHaveChildren<Add, Sub, Mul, MulHigh, UMulHigh, Div, UDiv, Mod, UMod, BitAnd, BitOr, BitXor, Shl, SShr, ZShr, RotR, FMin, FMax>(2));
static_assert(fusionOpcodesHaveChildren<Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual, EqualOrUnordered, Store8, Store16, Store>(2));

static ALWAYS_INLINE Operation* const* fusionChildrenOf(const Operation* operation)
{
    return operation->children().data();
}

static ALWAYS_INLINE void fusionMark(ValueFacts* values, const Operation* operation, ValueFlag flag)
{
    values[operation->index()].flags |= static_cast<uint16_t>(flag);
}

static constexpr uint16_t fusionFlagsOfSettledLeaves = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::UsesOverflowFlag);

namespace FusionTrait {
enum : uint8_t {
    IsLoad = 1 << 0,
    IsBitAnd = 1 << 1,
    MayBelongToSum = 1 << 2,
    IsPatch = 1 << 3,
    IsComparison = 1 << 4,
    StopsLoad = 1 << 5,
    IsTruncation = 1 << 6,
};
}

static constexpr std::array<uint8_t, numberOfOpcodes> fusionTraitsOfOpcodes = [] {
    std::array<uint8_t, numberOfOpcodes> traits { };
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        if (isX86_64() && isLoad(opcode))
            traits[index] |= FusionTrait::IsLoad;
        if (isComparison(opcode))
            traits[index] |= FusionTrait::IsComparison;
        if (opcode == Patch || hasTrait(opcode, OpcodeTrait::HasEffects))
            traits[index] |= FusionTrait::StopsLoad;
    }
    traits[BitAnd] |= FusionTrait::IsBitAnd;
    for (Opcode opcode : { Add, Sub, SExt32 })
        traits[opcode] |= FusionTrait::MayBelongToSum;
    traits[Patch] |= FusionTrait::IsPatch;
    traits[Trunc] |= FusionTrait::IsTruncation;
    return traits;
}();

static ALWAYS_INLINE unsigned fusionTraitsOf(const Operation* operation)
{
    return fusionTraitsOfOpcodes.data()[operation->opcode()];
}

static ALWAYS_INLINE bool fusionCanAbsorb(const ValueFacts* values, const Block* block, const Operation* candidate)
{
    return candidate->useCount() == 1 && candidate->owner() == block && values[candidate->index()].lastUse == noPosition;
}

static NEVER_INLINE bool fusionTestMayStandInLaterBlock(const ValueFacts* values, const Operation* candidate)
{
    unsigned numberOfRangesThatGrow = 0;
    for (const Operation* child : candidate->children()) {
        if (child->hasNoPosition() || values[child->index()].lastUse != noPosition)
            continue;
        if (++numberOfRangesThatGrow > 1 || !child->type().isInt() || child->isNarrowReference() || child->has(OperationFlag::IsInnerPointer))
            return false;
    }
    return true;
}

static ALWAYS_INLINE bool fusionCanAbsorbIntoTest(const ValueFacts* values, const Block* block, const Operation* candidate)
{
    if (candidate->useCount() != 1 || values[candidate->index()].lastUse != noPosition)
        return false;
    return candidate->owner() == block || fusionTestMayStandInLaterBlock(values, candidate);
}

[[maybe_unused]] static unsigned fusionLeavesInStepOf(const BackwardScan& scan, const Operation* root)
{
    if (root->opcode() != Check)
        return scan.numberOfLeavesOf(root);
    const Operation* predicate = root->child(0);
    return scan.m_liveness.isFused(predicate) ? scan.numberOfLeavesOf(predicate) : 1;
}

static IndexExtension fusionExtensionOf(const Operation* extensionOrNull)
{
    if (!extensionOrNull)
        return IndexExtension::None;
    return extensionOrNull->opcode() == ZExt32 ? IndexExtension::ZeroExtend32 : IndexExtension::SignExtend32;
}

static unsigned fuseAddressBelow(ValueFacts* values, unsigned numberOfLeaves, const Operation* access, Operation* sum)
{
    ASSERT(sum == access->lastChild());
    const Block* block = access->owner();
    if (sum->opcode() != Add || !fusionCanAbsorb(values, block, sum))
        return 0;

    Operation* const* operandsOfSum = fusionChildrenOf(sum);
    const Operation* base = operandsOfSum[0];
    Operation* index = operandsOfSum[1];
    if (base->hasNoPosition() && base->opcode() != ArgumentReg && base->opcode() != AddressOf)
        return 0;

    Operation* shift = nullptr;
    Operation* shifted = index;
    unsigned amountOfShift = 0;
    if (index->opcode() == Shl) {
        Operation* const* operandsOfShift = fusionChildrenOf(index);
        if (operandsOfShift[1]->hasInt32() && fusionCanAbsorb(values, block, index)) {
            shift = index;
            shifted = operandsOfShift[0];
            amountOfShift = static_cast<unsigned>(operandsOfShift[1]->asInt32()) & 63;
        }
    }
    Operation* extension = nullptr;
    const Operation* extended = nullptr;
    if ((shifted->opcode() == ZExt32 || shifted->opcode() == SExt32) && fusionCanAbsorb(values, block, shifted)) {
        extension = shifted;
        extended = fusionChildrenOf(shifted)[0];
    }

    Opcode opcodeOfAccess = access->opcode();
    Width width = accessWidthOf(opcodeOfAccess, fusionIsStore(opcodeOfAccess) ? fusionChildrenOf(access)[0]->type() : access->type());
    int32_t offset = access->offset();
    bool recordNamesReference = access->traps() || values[access->index()].has(ValueFlag::TakesOverTrap);

    Operation* fusedShift = shift;
    Operation* fusedExtension = extension;
    for (;;) {
        const Operation* leaf = fusedExtension ? extended : fusedShift ? shifted : index;
        unsigned amount = fusedShift ? amountOfShift : 0;
        IndexExtension kind = fusionExtensionOf(fusedExtension);
        if (kind == IndexExtension::ZeroExtend32 && clearsUpperHalf(leaf))
            kind = IndexExtension::None;
        unsigned numberOfLeavesAdded = fusedShift ? 2 : 1;
        bool hasRoomForLeaves = !leaf->hasNoPosition() && numberOfLeaves + numberOfLeavesAdded <= maxLeaves;
        bool recordFindsReference = !recordNamesReference || (base->isArgumentReg(cellCageBaseGPR) && fusedShift && amount == cell32Shift && kind != IndexExtension::SignExtend32);
        if (hasRoomForLeaves && recordFindsReference && isEncodableIndexedAddress(width, amount, kind, offset)) {
            fusionMark(values, sum, ValueFlag::IsFused);
            if (fusedShift)
                fusionMark(values, fusedShift, ValueFlag::IsFused);
            if (fusedExtension)
                fusionMark(values, fusedExtension, ValueFlag::IsFused);
            return numberOfLeavesAdded;
        }

        if (fusedExtension)
            fusedExtension = nullptr;
        else if (fusedShift)
            fusedShift = nullptr;
        else
            return 0;
    }
}

static bool fuseTruncationsBelow(ValueFacts* values, const Operation* user, Operation* const* children, unsigned numberOfChildren)
{
    if ((!user->type().isInt() && user->opcode() != IToD) || fusionIsComparison(user->opcode()))
        return false;
    const Block* block = user->owner();
    bool hasFused = false;
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        const Operation* child = children[index];
        if (child->opcode() == Trunc && !child->isNarrowReference() && !fusionChildrenOf(child)[0]->hasNoPosition() && fusionCanAbsorb(values, block, child)) {
            fusionMark(values, child, ValueFlag::IsFused);
            hasFused = true;
        }
    }
    return hasFused;
}

static NEVER_INLINE bool fuseTruncationsBelowTest(ValueFacts* values, const Block* block, Operation* const* operands)
{
    for (unsigned index = 0; index < 2; ++index) {
        const Operation* operand = operands[index];
        if (operand->opcode() == Trunc && !operand->isNarrowReference() && !fusionChildrenOf(operand)[0]->hasNoPosition() && fusionCanAbsorb(values, block, operand))
            fusionMark(values, operand, ValueFlag::IsFused);
    }
    return true;
}

static ALWAYS_INLINE bool fusionMayFuseAddress(const Operation* access, const Operation* address)
{
    return mayBeFusedAsAddress(address->opcode()) && address->useCount() == 1 && address->owner() == access->owner();
}

static ALWAYS_INLINE bool fusedLoadCanMoveBehind(const Operation* between)
{
    if (!(fusionTraitsOf(between) & FusionTrait::StopsLoad)) [[likely]]
        return !between->traps();
    if (between->opcode() != Patch)
        return false;
    // No instruction, and no write. What it keeps for the load is kept as far as the last place in front of the load where the collector may look: all that the load moves behind is asked this, so none lies between.
    const PatchOperation* patch = static_cast<const PatchOperation*>(between);
    return patch->onlyKeepsChildrenAlive && patch->effects.writes.isEmpty();
}

static ALWAYS_INLINE bool fusedLoadCanMoveTo(const Operation* load, const Operation* reader)
{
    const Block* block = reader->owner();
    unsigned begin = load->indexInBlock() + 1;
    unsigned end = reader->indexInBlock();
    if (end - begin > maxOperationsBetweenLoadAndUser)
        return false;
    for (unsigned index = begin; index < end; ++index) {
        if (!fusedLoadCanMoveBehind(block->at(index)))
            return false;
    }
    return true;
}

static ALWAYS_INLINE bool fusionCanTestLoadInMemory(const Operation* tested, const Operation* load, const Operation* other)
{
    if (!load->type().isInt())
        return false;
    Opcode opcode = load->opcode();
    if (opcode == Load || !other)
        return true;
    if (!other->hasInt())
        return false;

    int64_t constant = other->asInt();
    if (opcode == Load8Z)
        return tested->opcode() == BitAnd || static_cast<uint64_t>(constant) <= 255;

    bool isByte = opcode == Load8Z || opcode == Load8S;
    bool extendsSign = opcode == Load8S || opcode == Load16S;
    if (tested->opcode() != BitAnd)
        return constant >= (extendsSign ? -128 : 0) && constant <= (isByte && !extendsSign ? 255 : 127);

    uint64_t bits = static_cast<uint32_t>(constant);
    uint64_t bitsOfWord = isByte ? 0xff : 0xffff;
    if (extendsSign && (bits & ~bitsOfWord))
        return false;
    bits &= bitsOfWord;
    return !(bits & ~uint64_t { 0xff }) || !(bits & 0xff);
}

static ALWAYS_INLINE bool fusionTestCanReadLoad(const ValueFacts* values, const Operation* root, const Operation* tested, unsigned index, const Operation* load, const Operation* other)
{
    if (!fusionIsLoad(load->opcode()) || load->traps() || load->has(OperationFlag::IsInnerPointer))
        return false;
    if (!fusionCanAbsorb(values, root->owner(), load) || values[load->index()].has(ValueFlag::TakesOverTrap))
        return false;
    if (!fusionCanTestLoadInMemory(tested, load, other))
        return false;
    if (!acceptsMemoryOperand(*tested, index))
        return false;
    if (other && other->hasNoPosition() && !acceptsImmediate(*tested, 1 - index, *other))
        return false;
    return fusedLoadCanMoveTo(load, root);
}

static std::optional<unsigned> fuseLoadBelowTest(ValueFacts* values, const Operation* root, const Operation* tested, unsigned numberOfLeaves)
{
    if (!isX86_64() || root->opcode() == Check)
        return std::nullopt;

    Operation* const* operands = fusionChildrenOf(tested);
    unsigned numberOfOperands = tested == root ? 1 : 2;
    for (unsigned index = 0; index < numberOfOperands; ++index) {
        Operation* load = operands[index];
        if (!fusionTestCanReadLoad(values, root, tested, index, load, tested == root ? nullptr : operands[1 - index]))
            continue;

        Operation* address = fusionChildrenOf(load)[0];
        fusionMark(values, load, ValueFlag::IsFused);
        return fusionMayFuseAddress(load, address) ? fuseAddressBelow(values, numberOfLeaves, load, address) : 0;
    }
    return std::nullopt;
}

static NEVER_INLINE bool fuseAddressBelowFusedLoad(ValueFacts* values, unsigned numberOfLeaves, const Operation* load, Operation* sum)
{
    fuseAddressBelow(values, numberOfLeaves, load, sum);
    return true;
}

static ALWAYS_INLINE bool fuseLoadThatTestCanRead(ValueFacts* values, const Operation* load, unsigned numberOfLeaves)
{
    Operation* address = fusionChildrenOf(load)[0];
    fusionMark(values, load, ValueFlag::IsFused);
    if (!fusionMayFuseAddress(load, address))
        return true;
    return fuseAddressBelowFusedLoad(values, numberOfLeaves, load, address);
}

static NEVER_INLINE bool fuseEitherLoadBelowOneTestOfBranch(ValueFacts* values, const Operation* branch, const Operation* tested, Operation* const* operands)
{
    if (fusionTestCanReadLoad(values, branch, tested, 0, operands[0], operands[1]))
        return fuseLoadThatTestCanRead(values, operands[0], 2);
    if (fusionTestCanReadLoad(values, branch, tested, 1, operands[1], operands[0]))
        return fuseLoadThatTestCanRead(values, operands[1], 2);
    return true;
}

static NEVER_INLINE bool fuseLoadBelowOneTestOfBranch(ValueFacts* values, const Operation* branch, const Operation* tested, Operation* const* operands)
{
    ASSERT(isX86_64() && branch->opcode() == Branch && operands == fusionChildrenOf(tested));
    if (fusionTraitsOf(operands[1]) & FusionTrait::IsLoad) [[unlikely]]
        return fuseEitherLoadBelowOneTestOfBranch(values, branch, tested, operands);
    if (!fusionTestCanReadLoad(values, branch, tested, 0, operands[0], operands[1]))
        return true;
    return fuseLoadThatTestCanRead(values, operands[0], 2);
}

static NEVER_INLINE bool fuseLoadTestedByBranch(ValueFacts* values, const Operation* branch, const Operation* load)
{
    ASSERT(isX86_64() && branch->opcode() == Branch && fusionChildrenOf(branch)[0] == load);
    if (!fusionTestCanReadLoad(values, branch, branch, 0, load, nullptr))
        return false;
    return fuseLoadThatTestCanRead(values, load, 1);
}

static ALWAYS_INLINE bool fuseShiftBelowBitTest(ValueFacts* values, const Block* block, Operation* const* operands, unsigned numberOfLeaves)
{
    const Operation* shifted = operands[0];
    if (!isX86_64() || shifted->opcode() != Patch || !operands[1]->isInt(1))
        return false;
    const PatchOperation* shift = static_cast<const PatchOperation*>(shifted);
    if (!shift->pureFunction)
        return false;
    uint64_t function = shift->pureFunction->id;
    if (function != (PureFunction::firstIDOfBackEnd | ZShr) && function != (PureFunction::firstIDOfBackEnd | SShr))
        return false;
    if (!fusionCanAbsorb(values, block, shift) || numberOfLeaves + 1 > maxLeaves)
        return false;
    ASSERT(shift->numChildren() == 2);
    fusionMark(values, shift, ValueFlag::IsFused);
    return true;
}

static ALWAYS_INLINE bool fusionGivesOneOrZero(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    return fusionIsComparison(opcode) || (opcode == BitAnd && fusionChildrenOf(operation)[1]->isInt(1));
}

static bool fusionAccessLiesWithinTrapReach(const Operation* access)
{
    int64_t begin = access->offset();
    int64_t end = begin + static_cast<int64_t>(bytesForWidth(access->accessWidth()));
    return begin >= 0 && end <= static_cast<int64_t>(trapReach());
}

static bool fusionCanRunAheadOfTrap(const Operation* between)
{
    if (fusionIsLoad(between->opcode()))
        return !between->traps() && fusionChildrenOf(between)[0]->hasNoPosition();
    if (between->opcode() == Patch) {
        const PatchOperation* patch = between->as<PatchOperation>();
        return patch->effects.maySink() && !patch->isSuspension() && std::ranges::all_of(patch->children(), [](const Operation* operand) { return operand->type().isFloat(); });
    }
    return !between->mustStay();
}

static ALWAYS_INLINE bool comparisonMayReadSumOfExtendedWords(const Operation* comparison)
{
    ASSERT(isComparison(comparison->opcode()));
    Operation* const* operands = fusionChildrenOf(comparison);
    Opcode left = operands[0]->opcode();
    if (left == Add || left == Sub)
        return true;
    if (left != SExt32)
        return false;
    Opcode right = operands[1]->opcode();
    return right == Add || right == Sub;
}

static ALWAYS_INLINE bool branchMayReadSumOfExtendedWords(const Operation* branch)
{
    if (branch->opcode() != Branch)
        return false;
    const Operation* comparison = fusionChildrenOf(branch)[0];
    return fusionIsComparison(comparison->opcode()) && comparisonMayReadSumOfExtendedWords(comparison);
}

static ALWAYS_INLINE bool fusePredicateBelow(const BackwardScan& scan, ValueFacts* values, const Operation* user, const Operation* predicate)
{
    Opcode opcodeOfUser = user->opcode();
    Opcode opcode = predicate->opcode();
    const Block* block = user->owner();
    unsigned numberOfLeaves = opcodeOfUser == Select ? user->numChildren() : 1;
    ASSERT_UNUSED(scan, numberOfLeaves == fusionLeavesInStepOf(scan, user));
    if (fusionIsLoad(opcode))
        return fuseLoadBelowTest(values, user, user, numberOfLeaves).has_value();

    static_assert(maxLeaves >= 4);
    const Operation* tests[2] { predicate, nullptr };
    unsigned numberOfTests = 0;
    if (opcodeOfUser == Branch && (opcode == BitAnd || opcode == BitOr)) {
        Operation* const* operands = fusionChildrenOf(predicate);
        const Operation* first = operands[0];
        const Operation* second = operands[1];
        if (fusionGivesOneOrZero(second) && fusionGivesOneOrZero(first)
            && fusionCanAbsorb(values, block, predicate) && fusionCanAbsorb(values, block, first) && fusionCanAbsorb(values, block, second)) {
            fusionMark(values, predicate, ValueFlag::IsFused);
            fusionMark(values, first, ValueFlag::IsFused);
            fusionMark(values, second, ValueFlag::IsFused);
            tests[0] = first;
            tests[1] = second;
            numberOfTests = 2;
            numberOfLeaves += 3;
        }
    }
    if (!numberOfTests) {
        if ((!fusionIsComparison(opcode) && opcode != BitAnd) || !fusionCanAbsorbIntoTest(values, block, predicate))
            return false;

        if (numberOfLeaves + 1 > maxLeaves)
            return false;
        fusionMark(values, predicate, ValueFlag::IsFused);
        numberOfTests = 1;
        ++numberOfLeaves;
    }

    bool mayFuseLoad = isX86_64() && opcodeOfUser != Check;
    for (unsigned number = 0; number < numberOfTests; ++number) {
        ASSERT(numberOfLeaves == fusionLeavesInStepOf(scan, user));
        const Operation* tested = tests[number];
        Opcode opcodeOfTested = tested->opcode();
        Operation* const* operands = fusionChildrenOf(tested);
        if ((opcodeOfTested == Equal || opcodeOfTested == NotEqual) && operands[1]->isInt(0)) {
            const Operation* bits = operands[0];
            if (bits->opcode() == BitAnd && numberOfLeaves < maxLeaves && fusionCanAbsorb(values, block, bits)) {
                fusionMark(values, bits, ValueFlag::IsFused);
                tested = bits;
                opcodeOfTested = BitAnd;
                operands = fusionChildrenOf(bits);
                ++numberOfLeaves;
            }
        }
        if (opcodeOfUser == Branch && opcodeOfTested == BitAnd && fuseShiftBelowBitTest(values, block, operands, numberOfLeaves)) {
            ++numberOfLeaves;
            continue;
        }
        if (opcodeOfUser != Select && ((fusionTraitsOf(operands[0]) | fusionTraitsOf(operands[1])) & FusionTrait::IsTruncation))
            fuseTruncationsBelowTest(values, block, operands);
        if (!mayFuseLoad || (!fusionIsLoad(operands[0]->opcode()) && !fusionIsLoad(operands[1]->opcode())))
            continue;
        if (std::optional<unsigned> numberOfLeavesAdded = fuseLoadBelowTest(values, user, tested, numberOfLeaves)) {
            mayFuseLoad = false;
            numberOfLeaves += *numberOfLeavesAdded;
        }
    }
    ASSERT(numberOfLeaves == fusionLeavesInStepOf(scan, user) && numberOfLeaves <= maxLeaves);
    return true;
}

static NEVER_INLINE bool fuseIntoTest(ValueFacts* values, Operation* user, const Operation* predicate, BackwardScan& scan)
{
    if (values[user->index()].flags & fusionFlagsOfSettledLeaves)
        return false;
    if (user->opcode() == Branch && fusionIsComparison(predicate->opcode()) && comparisonMayReadSumOfExtendedWords(predicate) && scan.tryTestSumByOverflow(user))
        return true;
    return fusePredicateBelow(scan, values, user, predicate);
}

static NEVER_INLINE bool fuseShiftOrLoadBelowBitAndOfBranch(ValueFacts* values, const Operation* branch, const Operation* bits, Operation* const* operands)
{
    ASSERT(branch->opcode() == Branch && bits->opcode() == BitAnd && operands == fusionChildrenOf(bits) && operands[0]->opcode() == Patch);
    if (fuseShiftBelowBitTest(values, branch->owner(), operands, 2))
        return true;
    if (!(fusionTraitsOf(operands[1]) & FusionTrait::IsLoad))
        return true;
    return fuseLoadBelowOneTestOfBranch(values, branch, bits, operands);
}

static ALWAYS_INLINE bool fusionComparesBitAndWithZero(const Operation* comparison, Operation* const* operands)
{
    Opcode opcode = comparison->opcode();
    return (opcode == Equal || opcode == NotEqual) && operands[0]->opcode() == BitAnd && operands[1]->isInt(0);
}

template<Opcode opcodeOfUser, bool predicateIsBitAnd>
static ALWAYS_INLINE bool fuseOneTestInto(ValueFacts* values, Operation* user, const Operation* predicate, BackwardScan& scan)
{
    static_assert(opcodeOfUser == Branch || opcodeOfUser == Check);
    static_assert(maxLeaves >= 4);
    ASSERT(user->opcode() == opcodeOfUser && (predicateIsBitAnd ? predicate->opcode() == BitAnd : isComparison(predicate->opcode())));
    constexpr uint16_t isFused = static_cast<uint16_t>(ValueFlag::IsFused);

    if (predicate->useCount() != 1)
        return false;
    ValueFacts& factsOfPredicate = values[predicate->index()];
    if (factsOfPredicate.lastUse != noPosition || predicate->owner() != user->owner()) [[unlikely]]
        return fuseIntoTest(values, user, predicate, scan);

    Operation* const* operands = fusionChildrenOf(predicate);
    unsigned traitsOfLeft = fusionTraitsOf(operands[0]);
    unsigned traitsOfRight = fusionTraitsOf(operands[1]);
    unsigned traits = traitsOfLeft | traitsOfRight;
    if constexpr (opcodeOfUser == Check) {
        if constexpr (!predicateIsBitAnd) {
            if (fusionComparesBitAndWithZero(predicate, operands)) [[unlikely]]
                return fuseIntoTest(values, user, predicate, scan);
        }
    } else {
        if constexpr (predicateIsBitAnd) {
            if (traits & (FusionTrait::IsComparison | FusionTrait::IsBitAnd | FusionTrait::IsPatch | FusionTrait::IsLoad)) {
                if (traitsOfRight & (FusionTrait::IsComparison | FusionTrait::IsBitAnd)) [[unlikely]]
                    return fuseIntoTest(values, user, predicate, scan);
                if (traitsOfLeft & FusionTrait::IsPatch) {
                    factsOfPredicate.flags |= isFused;
                    return fuseShiftOrLoadBelowBitAndOfBranch(values, user, predicate, operands);
                }
                if (traits & FusionTrait::IsLoad) {
                    factsOfPredicate.flags |= isFused;
                    return fuseLoadBelowOneTestOfBranch(values, user, predicate, operands);
                }
            }
        } else if (traits & (FusionTrait::MayBelongToSum | FusionTrait::IsBitAnd | FusionTrait::IsLoad)) {
            if ((traits & FusionTrait::MayBelongToSum) || ((traitsOfLeft & FusionTrait::IsBitAnd) && fusionComparesBitAndWithZero(predicate, operands))) [[unlikely]]
                return fuseIntoTest(values, user, predicate, scan);
            if (traits & FusionTrait::IsLoad) {
                factsOfPredicate.flags |= isFused;
                return fuseLoadBelowOneTestOfBranch(values, user, predicate, operands);
            }
        }
    }

    factsOfPredicate.flags |= isFused;
    ASSERT(fusionLeavesInStepOf(scan, user) == 2);
    if (traits & FusionTrait::IsTruncation)
        return fuseTruncationsBelowTest(values, user->owner(), operands);
    return true;
}

static NEVER_INLINE bool fuseIntoBranch(ValueFacts* values, Operation* branch, const Operation* predicate, BackwardScan& scan)
{
    ASSERT(branch->opcode() == Branch && fusionChildrenOf(branch)[0] == predicate);
    if (values[branch->index()].flags & fusionFlagsOfSettledLeaves) [[unlikely]]
        return false;
    Opcode opcode = predicate->opcode();
    if (fusionIsComparison(opcode)) [[likely]]
        return fuseOneTestInto<Branch, false>(values, branch, predicate, scan);
    if (opcode == BitAnd)
        return fuseOneTestInto<Branch, true>(values, branch, predicate, scan);
    if (fusionIsLoad(opcode))
        return isX86_64() && fuseLoadTestedByBranch(values, branch, predicate);
    if (opcode == BitOr)
        return fuseIntoTest(values, branch, predicate, scan);
    return false;
}

static NEVER_INLINE bool fuseIntoCheck(ValueFacts* values, Operation* check, const Operation* predicate, BackwardScan& scan)
{
    ASSERT(check->opcode() == Check && fusionChildrenOf(check)[0] == predicate);
    if (values[check->index()].flags & fusionFlagsOfSettledLeaves) [[unlikely]]
        return false;
    Opcode opcode = predicate->opcode();
    if (fusionIsComparison(opcode))
        return fuseOneTestInto<Check, false>(values, check, predicate, scan);
    if (opcode == BitAnd)
        return fuseOneTestInto<Check, true>(values, check, predicate, scan);
    return false;
}

static NEVER_INLINE bool fuseAddressOfAccess(ValueFacts* values, const Operation* access, Operation* sum, unsigned numberOfChildren)
{
    ASSERT(isMemoryAccess(access->opcode()) && !values[access->index()].has(ValueFlag::IsFused));
    if (values[access->index()].flags & fusionFlagsOfSettledLeaves)
        return false;
    return !!fuseAddressBelow(values, numberOfChildren, access, sum);
}

static ALWAYS_INLINE bool fuseIntoAccess(ValueFacts* values, const Operation* access, Operation* address, unsigned numberOfChildren)
{
    if (!fusionMayFuseAddress(access, address))
        return false;
    return fuseAddressOfAccess(values, access, address, numberOfChildren);
}

static ALWAYS_INLINE bool fuseScaledIndexBelow(ValueFacts* values, const Operation* sum)
{
    ASSERT(!values[sum->index()].has(ValueFlag::IsFused));
    if (sum->opcode() != Add || sum->type() != Int64)
        return false;

    static_assert(maxLeaves >= 3);
    bool sumSinks = values[sum->index()].has(ValueFlag::IsSunk);
    const Block* blockOfSum = sum->owner();
    auto canAbsorb = [&](const Operation* candidate) ALWAYS_INLINE_LAMBDA {
        return fusionCanAbsorb(values, sumSinks ? candidate->owner() : blockOfSum, candidate);
    };
    Operation* const* operandsOfSum = fusionChildrenOf(sum);
    for (unsigned shiftChildIndex : { 1u, 0u }) {
        const Operation* shift = operandsOfSum[shiftChildIndex];
        const Operation* base = operandsOfSum[1 - shiftChildIndex];
        if (shift->opcode() != Shl)
            continue;
        Operation* const* operandsOfShift = fusionChildrenOf(shift);
        if (!operandsOfShift[1]->hasInt32() || shift->isNarrowReference() || !canAbsorb(shift))
            continue;
        if (base->isConstant() || (!shiftChildIndex && sum->has(OperationFlag::IsInnerPointer)))
            continue;

        unsigned amount = static_cast<unsigned>(operandsOfShift[1]->asInt32()) & 63;
        const Operation* shifted = operandsOfShift[0];
        bool isExtension = shifted->opcode() == ZExt32 || shifted->opcode() == SExt32;
        const Operation* extension = isExtension && !shifted->isNarrowReference() && canAbsorb(shifted) ? shifted : nullptr;

        auto tryForm = [&](const Operation* fusedExtension) ALWAYS_INLINE_LAMBDA {
            const Operation* leaf = fusedExtension ? fusionChildrenOf(fusedExtension)[0] : shifted;
            IndexExtension kind = fusionExtensionOf(fusedExtension);
            if (kind == IndexExtension::ZeroExtend32 && clearsUpperHalf(leaf))
                kind = IndexExtension::None;
            if (leaf->hasNoPosition() || !isEncodableScaledSum(amount, kind))
                return false;
            fusionMark(values, shift, ValueFlag::IsFused);
            if (fusedExtension)
                fusionMark(values, fusedExtension, ValueFlag::IsFused);
            return true;
        };
        if ((extension && tryForm(extension)) || tryForm(nullptr))
            return true;
    }
    return false;
}

static bool fuseLoadBelowComputation(ValueFacts* values, const Operation* user, unsigned childIndex, Operation* load, unsigned numberOfLeaves)
{
    if (!isX86_64() || load->opcode() != Load || load->traps() || load->has(OperationFlag::IsInnerPointer))
        return false;
    if (!fusionCanAbsorb(values, user->owner(), load) || values[load->index()].has(ValueFlag::TakesOverTrap))
        return false;
    if (!fusedLoadCanMoveTo(load, user) || !acceptsMemoryOperand(*user, childIndex))
        return false;

    Operation* address = fusionChildrenOf(load)[0];
    fusionMark(values, load, ValueFlag::IsFused);
    if (fusionMayFuseAddress(load, address))
        fuseAddressBelow(values, numberOfLeaves, load, address);
    return true;
}

enum FusibleChild : uint8_t {
    FusibleTruncation = 1 << 0,
    FusibleShift = 1 << 1,
    FusibleLoad = 1 << 2,
};

static constexpr std::array<uint8_t, numberOfOpcodes> fusibleChildrenByOpcode = [] {
    std::array<uint8_t, numberOfOpcodes> kinds { };
    kinds[Trunc] = FusibleTruncation;
    kinds[Shl] = FusibleShift;
    kinds[Load] = isX86_64() ? FusibleLoad : 0;
    return kinds;
}();

static ALWAYS_INLINE unsigned fusibleKindOf(const Operation* child)
{
    return fusibleChildrenByOpcode.data()[child->opcode()];
}

static NEVER_INLINE bool fuseIntoComputation(ValueFacts* values, const Operation* user, Operation* const* children, unsigned numberOfChildren, unsigned kinds)
{
    ASSERT(!values[user->index()].has(ValueFlag::IsFused));
    if (values[user->index()].flags & fusionFlagsOfSettledLeaves)
        return false;

    bool hasFused = (kinds & FusibleTruncation) && fuseTruncationsBelow(values, user, children, numberOfChildren);
    if ((kinds & FusibleShift) && fuseScaledIndexBelow(values, user))
        return true;
    if (!isX86_64() || !(kinds & FusibleLoad))
        return hasFused;
    for (unsigned index = numberOfChildren; index--;) {
        if (fuseLoadBelowComputation(values, user, index, children[index], numberOfChildren))
            return true;
    }
    return hasFused;
}

static NEVER_INLINE bool fuseIntoOtherThanBranch(ValueFacts* values, Operation* user, BackwardScan& scan)
{
    switch (user->opcode()) {
    case Branch:
        return fuseIntoBranch(values, user, fusionChildrenOf(user)[0], scan);
    case Check:
        return fuseIntoCheck(values, user, fusionChildrenOf(user)[0], scan);
    case Select: {
        const Operation* predicate = fusionChildrenOf(user)[0];
        Opcode opcode = predicate->opcode();
        if (!fusionIsComparison(opcode) && !fusionIsLoad(opcode) && opcode != BitAnd && opcode != BitOr)
            return false;
        return fuseIntoTest(values, user, predicate, scan);
    }

    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        return fuseIntoAccess(values, user, fusionChildrenOf(user)[0], 1);
    case Store8:
    case Store16:
    case Store:
        return fuseIntoAccess(values, user, fusionChildrenOf(user)[1], 2);

    case Neg:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat: {
        Operation* const* children = fusionChildrenOf(user);
        unsigned kinds = fusibleKindOf(children[0]);
        return kinds && children[0]->useCount() == 1 && fuseIntoComputation(values, user, children, 1, kinds);
    }
    case Add:
    case Sub:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case RotR:
    case FMin:
    case FMax:
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
    case EqualOrUnordered: {
        Operation* const* children = fusionChildrenOf(user);
        unsigned kindOfLeft = fusibleKindOf(children[0]);
        unsigned kindOfRight = fusibleKindOf(children[1]);
        if (!(kindOfLeft | kindOfRight))
            return false;
        if (children[0]->useCount() != 1)
            kindOfLeft = 0;
        if (children[1]->useCount() != 1)
            kindOfRight = 0;
        unsigned kinds = kindOfLeft | kindOfRight;
        return kinds && fuseIntoComputation(values, user, children, 2, kinds);
    }

    default:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BackwardScan::fuseInto(Operation* user)
{
    ValueFacts* values = m_facts;
    if (user->opcode() == Branch)
        fuseIntoBranch(values, user, fusionChildrenOf(user)[0], *this);
    else
        fuseIntoOtherThanBranch(values, user, *this);
}

bool BackwardScan::tryFuseScaledIndex(Operation* sum)
{
    return fuseScaledIndexBelow(m_liveness.m_values.begin(), sum);
}

bool BackwardScan::tryHandOverTrap(Operation* trappingLoad)
{
    ASSERT(isLoad(trappingLoad->opcode()) && trappingLoad->traps() && facts(trappingLoad).lastUse == noPosition);
    const Operation* base = fusionChildrenOf(trappingLoad)[0];
    if (base->hasNoPosition())
        return false;

    ValueFacts* values = m_liveness.m_values.begin();
    const Block* block = trappingLoad->owner();
    unsigned begin = trappingLoad->indexInBlock() + 1;
    unsigned end = std::min(block->size(), begin + maxOperationsBetweenLoadAndUser + 1);
    for (unsigned index = begin; index < end; ++index) {
        Operation* candidate = block->at(index);
        ValueFacts& scanned = values[candidate->index()];
        if (scanned.has(ValueFlag::IsDropped) || scanned.has(ValueFlag::IsSunk))
            continue;
        Opcode opcode = candidate->opcode();
        bool isStoreBehind = fusionIsStore(opcode);
        if ((!isStoreBehind && !fusionIsLoad(opcode)) || fusionChildrenOf(candidate)[isStoreBehind ? 1 : 0] != base) {
            if (!fusionCanRunAheadOfTrap(candidate))
                return false;
            continue;
        }

        if (scanned.has(ValueFlag::IsFused) || scanned.has(ValueFlag::TakesOverTrap) || candidate->traps() || !fusionAccessLiesWithinTrapReach(candidate))
            return false;
        scanned.flags |= static_cast<uint16_t>(ValueFlag::TakesOverTrap);
        scanned.distanceToTrappingLoad = static_cast<uint8_t>(index + 1 - begin);
        return true;
    }
    return false;
}

unsigned BackwardScan::numberOfLeavesOf(const Operation* operation) const
{
    unsigned result = 0;
    forEachLeaf(m_liveness, operation, [&](const Operation*, unsigned, Operation*) {
        ++result;
    });
    return result;
}

static constexpr int64_t memoryUpdateMinInt32 = std::numeric_limits<int32_t>::min();
static constexpr int64_t memoryUpdateMaxInt32 = std::numeric_limits<int32_t>::max();
static constexpr int64_t memoryUpdateBitsOfLowHalf = std::numeric_limits<uint32_t>::max();

static_assert((maxIntegerAddedTo | memoryUpdateBitsOfLowHalf) == maxIntegerAddedTo && !(minIntegerAddedTo & memoryUpdateBitsOfLowHalf));

struct MemoryUpdatePassingSums {
    std::optional<int64_t> min;
    std::optional<int64_t> max;
};

static bool memoryUpdateCanReckonWith(int64_t bound, bool wordIsWide)
{
    constexpr int64_t limit = std::numeric_limits<int64_t>::max() / 4;
    return wordIsWide ? bound > -limit && bound < limit : isRepresentableAs<int32_t>(bound >> 2);
}

static bool memoryUpdateIsSignedOrder(Opcode relation)
{
    return relation == LessThan || relation == LessEqual || relation == GreaterThan || relation == GreaterEqual;
}

static const Operation* memoryUpdateLoadBelow(const Operation* child, const Operation* store)
{
    Type typeOfWord = store->child(0)->type();
    if (child->type() != typeOfWord) {
        if (child->opcode() != SExt32)
            return nullptr;
        child = child->child(0);
    }
    if (child->opcode() != Load || child->type() != typeOfWord || child->traps() || child->has(OperationFlag::ReadsImmutableHeap))
        return nullptr;
    return child->lastChild() == store->lastChild() && child->offset() == store->offset() ? child : nullptr;
}

static bool memoryUpdateIsExtension(const Operation* operand)
{
    return operand->opcode() == SExt32 || operand->opcode() == ZExt32;
}

static ALWAYS_INLINE bool storeMayUpdateWordInMemory(const Operation* store)
{
    if (store->opcode() != Store)
        return false;
    const Operation* computation = fusionChildrenOf(store)[0];
    if (computation->opcode() == Trunc)
        computation = fusionChildrenOf(computation)[0];
    Opcode opcode = computation->opcode();
    if (opcode != Add && opcode != Sub && opcode != BitAnd && opcode != BitOr && opcode != BitXor)
        return false;
    Operation* const* operands = fusionChildrenOf(computation);
    auto mayBeWord = [](const Operation* operand) {
        return operand->opcode() == Load || operand->opcode() == SExt32;
    };
    return mayBeWord(operands[0]) || mayBeWord(operands[1]);
}

static bool parseMemoryUpdate(const Operation* store, MemoryUpdate& update)
{
    if (store->opcode() != Store)
        return false;
    const Operation* stored = store->child(0);
    if (!stored->type().isInt())
        return false;
    const Operation* computation = stored->opcode() == Trunc ? stored->child(0) : stored;
    bool isSum = computation->opcode() == Add || computation->opcode() == Sub;
    if (computation->hasNoPosition() || !(acceptsMemoryDestination(*computation) || (updatesMemoryThroughScratchRegister() && isSum && computation->type().isInt())))
        return false;

    unsigned indexOfWord = 0;
    const Operation* load = memoryUpdateLoadBelow(computation->child(0), store);
    if (!load && isCommutative(computation->opcode())) {
        indexOfWord = 1;
        load = memoryUpdateLoadBelow(computation->child(1), store);
    }
    if (!load)
        return false;
    ASSERT(storeMayUpdateWordInMemory(store));

    update.kind = MemoryUpdate::InPlace;
    update.opcode = computation->opcode();
    update.computation = computation;
    update.load = load;
    update.userOfOperand = computation;
    update.indexOfOperand = 1 - indexOfWord;

    const Operation* operand = update.operand();
    if (operand->hasInt())
        update.constant = operand->asInt();
    if (load->type() == Int32 && operand->type() == Int64 && memoryUpdateIsExtension(operand)) {
        update.userOfOperand = operand;
        update.indexOfOperand = 0;
    }
    return true;
}

static std::optional<MemoryUpdatePassingSums> memoryUpdateSumsThatPass(Opcode relation, int64_t bias, int64_t bound, bool wordIsWide)
{
    if (!memoryUpdateCanReckonWith(bias, wordIsWide) || !memoryUpdateCanReckonWith(bound, wordIsWide))
        return std::nullopt;
    if (relation == LessThan)
        return MemoryUpdatePassingSums { std::nullopt, bound - 1 - bias };
    if (relation == LessEqual)
        return MemoryUpdatePassingSums { std::nullopt, bound - bias };
    if (relation == GreaterThan)
        return MemoryUpdatePassingSums { bound + 1 - bias, std::nullopt };
    if (relation == GreaterEqual)
        return MemoryUpdatePassingSums { bound - bias, std::nullopt };
    if (relation != Below && relation != BelowEqual)
        return std::nullopt;
    int64_t limit = relation == Below ? bound - 1 : bound;
    if (limit < 0)
        return std::nullopt;
    return MemoryUpdatePassingSums { -bias, limit - bias };
}

static bool sumIsTestedByOverflow(const MemoryUpdatePassingSums& passing, int64_t minSum, int64_t maxSum, std::optional<int32_t>& leastResult)
{
    bool cannotOverflow = maxSum <= memoryUpdateMaxInt32;
    if (passing.max ? *passing.max != memoryUpdateMaxInt32 && (*passing.max < maxSum || !cannotOverflow) : !cannotOverflow)
        return false;
    bool cannotUnderflow = minSum >= memoryUpdateMinInt32;
    if ((!passing.min || *passing.min < memoryUpdateMinInt32) && !cannotUnderflow)
        return false;
    if (passing.min && *passing.min > memoryUpdateMaxInt32)
        return false;
    if (passing.min && *passing.min > memoryUpdateMinInt32 && minSum < *passing.min)
        leastResult = static_cast<int32_t>(*passing.min);
    return true;
}

static bool memoryUpdateIsTestedByOverflow(const MemoryUpdatePassingSums& passing, const IntegerRange& operands, bool subtracts, std::optional<int32_t>& leastStored)
{
    int64_t minSum = memoryUpdateMinInt32 + (subtracts ? -operands.max : operands.min);
    int64_t maxSum = memoryUpdateMaxInt32 + (subtracts ? -operands.min : operands.max);
    return sumIsTestedByOverflow(passing, minSum, maxSum, leastStored);
}

struct TestOfSum {
    const Operation* biasedSum { nullptr };
    const Operation* extensionOfLowHalf { nullptr };
    const Operation* lowHalf { nullptr };
    int64_t bias { 0 };
    int64_t bound { 0 };
};

static bool parseTestOfSum(const Operation* comparison, const Operation* sum, TestOfSum& test)
{
    const Operation* left = comparison->child(0);
    const Operation* right = comparison->child(1);
    if (right->hasInt()) {
        test.bound = right->asInt();
        if (left == sum)
            return true;
        if (left->opcode() != Add || left->child(0) != sum || !left->child(1)->hasInt())
            return false;
        test.biasedSum = left;
        test.bias = left->child(1)->asInt();
        return true;
    }

    if (left != sum && right != sum)
        return false;
    const Operation* extension = left == sum ? right : left;
    if (extension->opcode() != SExt32)
        return false;
    const Operation* lowHalf = extension->child(0);
    if (lowHalf->opcode() != Trunc || lowHalf->child(0) != sum)
        return false;
    test.extensionOfLowHalf = extension;
    test.lowHalf = lowHalf;
    return true;
}

static bool parseTestOfMemoryUpdate(const Operation* store, const Operation* branch, MemoryUpdate& update)
{
    const Operation* sum = update.computation;
    if (update.opcode != Add && update.opcode != Sub)
        return false;

    const Block* block = branch->owner();
    if (block->numSuccessors() != 2 || block->successor(0).block() == block->successor(1).block())
        return false;
    update.successorThatStores = block->successor(1).block() == store->owner();

    const Operation* comparison = branch->child(0);
    Opcode relation = comparison->opcode();
    if (!isComparison(relation) || relation == EqualOrUnordered || !comparison->child(0)->type().isInt() || comparison->useCount() != 1)
        return false;
    if (update.successorThatStores)
        relation = invertedComparison(relation);

    const Operation* truncation = store->child(0) == sum ? nullptr : store->child(0);
    TestOfSum test;
    if (!parseTestOfSum(comparison, sum, test))
        return false;
    unsigned usesOfSum = 2;
    unsigned usesOfTruncation = 1;
    bool isBiased = !!test.biasedSum;
    bool comparesWithOwnLowHalf = !!test.lowHalf;
    int64_t bias = test.bias;
    int64_t bound = test.bound;
    if (isBiased && test.biasedSum->useCount() != 1)
        return false;
    if (comparesWithOwnLowHalf) {
        if (relation != Equal || !truncation || test.extensionOfLowHalf->useCount() != 1)
            return false;
        if (test.lowHalf == truncation)
            usesOfTruncation = 2;
        else {
            if (test.lowHalf->useCount() != 1)
                return false;
            usesOfSum = 3;
        }
    }
    if (sum->useCount() != usesOfSum || (truncation && truncation->useCount() != usesOfTruncation))
        return false;

    bool wordIsWide = update.load->type() == Int64;
    update.relation = relation;
    update.bias = bias;
    update.bound = bound;

    if (!truncation && !wordIsWide) {
        if (isBiased || !block->successor(!update.successorThatStores).isRare())
            return false;
        update.kind = MemoryUpdate::TestedBehind;
        update.comparesWord = true;
        return true;
    }

    const Operation* operand = update.userOfOperand == sum ? update.operand() : update.userOfOperand;
    bool operandIsConstant = operand->hasInt();
    if (operandIsConstant && !isRepresentableAs<int32_t>(update.constant))
        return false;
    IntegerRange operands = rangeOf(operand);

    auto compareAhead = [&] {
        if (!operandIsConstant || update.opcode != Add || comparesWithOwnLowHalf || isBiased || !memoryUpdateIsSignedOrder(relation))
            return false;
        if (!memoryUpdateCanReckonWith(bound, false) || !isRepresentableAs<int32_t>(bound - update.constant))
            return false;
        update.kind = MemoryUpdate::ComparedAhead;
        update.bound = bound - update.constant;
        return true;
    };

    if (!wordIsWide) {
        if (!isRepresentableAs<int32_t>(operands.min) || !isRepresentableAs<int32_t>(operands.max))
            return false;
        std::optional<MemoryUpdatePassingSums> passing = MemoryUpdatePassingSums { memoryUpdateMinInt32, memoryUpdateMaxInt32 };
        if (!comparesWithOwnLowHalf)
            passing = memoryUpdateSumsThatPass(relation, bias, bound, false);
        std::optional<int32_t> leastStored;
        bool prefersToCompareAhead = operandIsConstant && !updatesMemoryThroughScratchRegister();
        if (passing && memoryUpdateIsTestedByOverflow(*passing, operands, update.opcode == Sub, leastStored) && !(leastStored && prefersToCompareAhead)) {
            update.kind = MemoryUpdate::TestedBehind;
            update.testsOverflow = true;
            update.comparesWord = !!leastStored;
            update.relation = GreaterEqual;
            update.bias = 0;
            update.bound = leastStored.value_or(0);
            return true;
        }
        return compareAhead();
    }

    if (!update.load->has(OperationFlag::IsIntegerAddedTo))
        return false;
    bool adds = update.opcode == Add;
    IntegerRange amounts = operands;
    if (operandIsConstant && update.constant < 0 && update.constant > memoryUpdateMinInt32 && acceptsImmediate(*sum, update.indexOfOperand, *operand)) {
        adds = !adds;
        amounts = { -update.constant, -update.constant };
    }
    if (amounts.min < 0 || amounts.max > memoryUpdateBitsOfLowHalf || update.load->lastChild()->opcode() == InstanceAddressOf)
        return compareAhead();

    std::optional<MemoryUpdatePassingSums> passing = memoryUpdateSumsThatPass(relation, bias, bound, true);
    if (!passing)
        return false;
    int64_t minSum = adds ? minIntegerAddedTo + amounts.min : minIntegerAddedTo;
    int64_t maxSum = adds ? maxIntegerAddedTo : maxIntegerAddedTo - amounts.min;
    if ((passing->min && minSum < *passing->min) || (passing->max && maxSum > *passing->max))
        return compareAhead();
    if (!isEncodableArithmeticConstant(bias) || !isEncodableArithmeticConstant(bound))
        return false;

    update.kind = MemoryUpdate::LowHalf;
    update.opcode = adds ? Add : Sub;
    if (operandIsConstant)
        update.constant = amounts.min;
    if (memoryUpdateIsExtension(operand)) {
        update.userOfOperand = operand;
        update.indexOfOperand = 0;
    }
    return true;
}

MemoryUpdate memoryUpdateOf(const Liveness& liveness, const Operation* storeOrBranch)
{
    ASSERT(liveness.worksInMemory(storeOrBranch));
    MemoryUpdate update;
    const Operation* store = storeOrBranch->opcode() == Store ? storeOrBranch : liveness.storeBehind(storeOrBranch);
    bool isAsScanned = store && parseMemoryUpdate(store, update);
    if (isAsScanned && store != storeOrBranch)
        isAsScanned = storeOrBranch->opcode() == Branch && parseTestOfMemoryUpdate(store, storeOrBranch, update);
    RELEASE_ASSERT(isAsScanned);
    return update;
}

static bool memoryUpdateMayStandBetween(const HeapTree& heaps, const Operation* between, const Operation* storeThatMovesAhead)
{
    Opcode opcode = between->opcode();
    if (opcode == Jump || opcode == Branch || opcode == Switch)
        return true;
    if (opcode == Patch) {
        const PatchOperation* patch = static_cast<const PatchOperation*>(between);
        if (patch->onlyKeepsChildrenAlive)
            return true;
        return !patch->isSuspension() && (storeThatMovesAhead ? patch->effects.maySink() : !patch->effects.mustStay());
    }
    if (between->mustStay())
        return false;
    return !storeThatMovesAhead || !isLoad(opcode) || !heaps.overlap(between->heap(), storeThatMovesAhead->heap());
}

static bool memoryUpdateFindsWordAsLoaded(const BackwardScan& scan, const MemoryUpdate& update, const Operation* store)
{
    const HeapTree& heaps = scan.m_proc.heapTree();
    bool branchStores = update.kind == MemoryUpdate::TestedBehind || update.kind == MemoryUpdate::LowHalf;
    const Operation* storeThatMovesAhead = branchStores ? store : nullptr;
    const Block* blockOfLoad = update.load->owner();
    const Block* block = store->owner();
    unsigned end = store->indexInBlock();
    unsigned numberLeft = maxOperationsBetweenLoadAndUpdate;
    for (;;) {
        unsigned begin = block == blockOfLoad ? update.load->indexInBlock() + 1 : 0;
        if (begin > end || end - begin > numberLeft)
            return false;
        numberLeft -= end - begin;
        for (unsigned index = begin; index < end; ++index) {
            if (!memoryUpdateMayStandBetween(heaps, block->at(index), storeThatMovesAhead))
                return false;
        }
        if (block == blockOfLoad)
            return true;

        std::span<Block* const> predecessors = scan.m_order.of(block).predecessors;
        if (predecessors.size() != 1 || block->isLandingPad())
            return false;
        block = predecessors.front();
        end = block->size();
        storeThatMovesAhead = nullptr;
    }
}

static NEVER_INLINE bool updateWordInMemory(BackwardScan& scan, ArenaVector<const Operation*>& storesBehindBranches, Operation* store)
{
    MemoryUpdate update;
    if (!parseMemoryUpdate(store, update))
        return false;

    Operation* branch = nullptr;
    if (store->child(0)->useCount() != 1 || update.computation->useCount() != 1) {
        const Block* block = store->owner();
        std::span<Block* const> predecessors = scan.m_order.of(block).predecessors;
        if (predecessors.size() != 1 || block->isLandingPad())
            return false;
        branch = predecessors.front()->last();
        // The Branch gets other leaves, so the scan must not have read the ones it has.
        if (branch->opcode() != Branch || branch->isUnconditionalJump() || scan.facts(branch).definition != noPosition || !parseTestOfMemoryUpdate(store, branch, update))
            return false;
    }
    if (updatesMemoryThroughScratchRegister()) {
        if (update.kind != MemoryUpdate::TestedBehind || !update.testsOverflow)
            return false;
        if (update.comparesWord && update.bound - memoryUpdateMinInt32 > maxConstantAddedInScratchRegister)
            return false;
        bool isTooLargeForImmediate = update.constant > maxConstantAddedInScratchRegister || update.constant < -maxConstantAddedInScratchRegister;
        if (isTooLargeForImmediate && acceptsImmediate(*update.userOfOperand, update.indexOfOperand, *update.operand()))
            return false;
    }
    if (!memoryUpdateFindsWordAsLoaded(scan, update, store))
        return false;

    if (branch) {
        ASSERT(!scan.m_liveness.worksInMemory(branch));
        if (storesBehindBranches.isEmpty())
            storesBehindBranches.fill(scan.m_proc.arena(), scan.m_order.mostBlocks(), nullptr);
        storesBehindBranches[branch->owner()->index()] = store;
        scan.set(branch, ValueFlag::WorksInMemory);
    }

    switch (update.kind) {
    case MemoryUpdate::InPlace:
    case MemoryUpdate::ComparedAhead:
        scan.set(store, ValueFlag::WorksInMemory);
        return false;
    case MemoryUpdate::TestedBehind:
        return true;
    case MemoryUpdate::LowHalf: {
        uint32_t loop = scan.m_order.of(store->owner()).loop;
        scan.m_lowHalfUpdatesInLoops.append(scan.m_proc.arena(), BackwardScan::LowHalfUpdate { branch, update.load->lastChild()->index(), loop, BackwardScan::bitOfWord(update.load->offset()) });
        if (loop == noLoop)
            scan.settleLowHalfUpdatesOf(noLoop);
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool BackwardScan::tryUpdateInMemory(Operation* store)
{
    if (!storeMayUpdateWordInMemory(store))
        return false;
    return updateWordInMemory(*this, m_liveness.m_storesBehindBranches, store);
}

void BackwardScan::settleLowHalfUpdatesOf(uint32_t loop)
{
    while (!m_lowHalfUpdatesInLoops.isEmpty() && m_lowHalfUpdatesInLoops.last().loop == loop) {
        LowHalfUpdate update = m_lowHalfUpdatesInLoops.takeLast();
        if (m_wordsReadWhole[update.base] & update.bit)
            set(update.branch, ValueFlag::AddsToWholeWord);
    }
}

static bool isSumOfExtendedWords(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    if ((opcode != Add && opcode != Sub) || operation->type() != Int64 || operation->hasNoPosition())
        return false;
    Operation* const* operands = fusionChildrenOf(operation);
    const Operation* left = operands[0];
    const Operation* right = operands[1];
    if (memoryUpdateIsExtension(left))
        return memoryUpdateIsExtension(right) || right->hasInt64();
    return left->hasInt64() && memoryUpdateIsExtension(right);
}

static void findOperandOfSumOfExtendedWords(const Operation* sum, unsigned index, const Operation*& user, unsigned& childIndex)
{
    user = sum;
    childIndex = index;
    const Operation* extension = sum->child(index);
    if (extension->hasInt64())
        return;
    user = extension;
    childIndex = 0;
    const Operation* word = extension->child(0);
    if (word->opcode() == Trunc && !word->isNarrowReference() && !word->child(0)->hasNoPosition())
        user = word;
}

static bool parseOverflowCheckedSum(const Operation* branch, OverflowCheckedSum& result)
{
    if (branch->opcode() != Branch || branch->isUnconditionalJump())
        return false;
    const Block* block = branch->owner();
    if (block->numSuccessors() != 2 || block->successor(0).block() == block->successor(1).block())
        return false;

    const Operation* comparison = fusionChildrenOf(branch)[0];
    Opcode relation = comparison->opcode();
    if (!fusionIsComparison(relation) || relation == EqualOrUnordered || comparison->owner() != block || comparison->useCount() != 1)
        return false;

    Operation* const* operands = fusionChildrenOf(comparison);
    const Operation* left = operands[0];
    const Operation* right = operands[1];
    if (left->type() != Int64)
        return false;
    const Operation* sum = left;
    if (!right->hasInt())
        sum = left->opcode() == SExt32 ? right : left;
    else if (!isSumOfExtendedWords(left) && left->opcode() == Add)
        sum = fusionChildrenOf(left)[0];
    if (!isSumOfExtendedWords(sum) || sum->owner() != block || sum->isNarrowReference() || sum->has(OperationFlag::IsInnerPointer))
        return false;
    ASSERT(branchMayReadSumOfExtendedWords(branch));

    TestOfSum test;
    if (!parseTestOfSum(comparison, sum, test))
        return false;
    for (const Operation* readByTestAlone : { test.biasedSum, test.extensionOfLowHalf }) {
        if (readByTestAlone && (readByTestAlone->useCount() != 1 || readByTestAlone->owner() != block))
            return false;
    }
    for (unsigned index = sum->indexInBlock() + 1; index < branch->indexInBlock(); ++index) {
        const Operation* between = block->at(index);
        if (between == comparison || between == test.biasedSum || between == test.extensionOfLowHalf || between->opcode() == Nop)
            continue;
        if (between->opcode() != Trunc || between->child(0) != sum || between->isNarrowReference())
            return false;
    }

    IntegerRange int32s { memoryUpdateMinInt32, memoryUpdateMaxInt32 };
    IntegerRange lefts = rangeOf(sum->child(0));
    IntegerRange rights = rangeOf(sum->child(1));
    if (!int32s.contains(lefts) || !int32s.contains(rights))
        return false;
    bool subtracts = sum->opcode() == Sub;
    int64_t minSum = lefts.min + (subtracts ? -rights.max : rights.min);
    int64_t maxSum = lefts.max + (subtracts ? -rights.min : rights.max);

    bool isTestedByOverflow = false;
    for (uint8_t passingSuccessor : { 0, 1 }) {
        Opcode passingRelation = passingSuccessor ? invertedComparison(relation) : relation;
        std::optional<MemoryUpdatePassingSums> passing;
        if (!test.lowHalf)
            passing = memoryUpdateSumsThatPass(passingRelation, test.bias, test.bound, false);
        else if (passingRelation == Equal)
            passing = MemoryUpdatePassingSums { memoryUpdateMinInt32, memoryUpdateMaxInt32 };
        std::optional<int32_t> leastResult;
        if (!passing || !sumIsTestedByOverflow(*passing, minSum, maxSum, leastResult))
            continue;
        result.passingSuccessor = passingSuccessor;
        result.leastResult = leastResult;
        isTestedByOverflow = true;
        break;
    }
    if (!isTestedByOverflow)
        return false;

    result.sum = sum;
    findOperandOfSumOfExtendedWords(sum, 0, result.userOfLeft, result.indexOfLeft);
    findOperandOfSumOfExtendedWords(sum, 1, result.userOfRight, result.indexOfRight);
    result.userOfSum = test.biasedSum ? test.biasedSum : comparison;
    result.indexOfSum = result.userOfSum->child(0) == sum ? 0 : 1;
    result.registerHoldsWholeSum = minSum >= 0;
    result.mayFallBelowInt32 = minSum < memoryUpdateMinInt32;
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool mayTestSumByOverflow(const Operation* branch)
{
    if (!branchMayReadSumOfExtendedWords(branch))
        return false;
    OverflowCheckedSum result;
    return parseOverflowCheckedSum(branch, result);
}

OverflowCheckedSum overflowCheckedSumOf(const Liveness& liveness, const Operation* sumOrBranch)
{
    ASSERT(liveness.usesOverflowFlag(sumOrBranch));
    OverflowCheckedSum result;
    const Operation* branch = sumOrBranch->opcode() == Branch ? sumOrBranch : sumOrBranch->owner()->last();
    bool isAsScanned = parseOverflowCheckedSum(branch, result) && (branch == sumOrBranch || result.sum == sumOrBranch);
    RELEASE_ASSERT(isAsScanned);
    result.isWidenedOnFailure = !result.registerHoldsWholeSum && liveness.of(result.sum).has(ValueFlag::UpperHalfIsRead);
    return result;
}

WayOutOfBranch wayOutOfBranchThrough(const Order& order, const Block* successor)
{
    const BlockPlace& place = order.of(successor);
    ASSERT(place.predecessors.size() == 1);
    uint32_t region = place.rareRegion;
    if (!place.entersRareRegion || order.rareRegions()[region].onlyEntry != successor)
        return { place.head, order.endOf(successor), !!successor->numSuccessors() };

    std::span<Block* const> blocks = order.blocksInOrder();
    uint32_t first = place.orderIndex;
    while (first && order.of(blocks[first - 1]).rareRegion == region)
        --first;
    uint32_t end = place.orderIndex + 1;
    while (end < blocks.size() && order.of(blocks[end]).rareRegion == region)
        ++end;

    WayOutOfBranch way { order.headOf(blocks[first]), order.endOf(blocks[end - 1]), false };
    for (uint32_t index = first; index < end; ++index) {
        for (const Edge& edge : blocks[index]->successors())
            way.leadsOn |= order.of(edge.block()).rareRegion != region;
    }
    return way;
}

bool BackwardScan::tryTestSumByOverflow(Operation* branch)
{
    ASSERT(branch->opcode() == Branch && !m_liveness.worksInMemory(branch));
    if (!branchMayReadSumOfExtendedWords(branch))
        return false;
    OverflowCheckedSum test;
    if (!parseOverflowCheckedSum(branch, test))
        return false;

    const Block* block = branch->owner();
    for (const Edge& edge : block->successors()) {
        const Block* successor = edge.block();
        if (successor->isLandingPad() || !successor->phis().empty() || m_order.of(successor).predecessors.size() != 1)
            return false;
    }

    const ValueFacts& sum = facts(test.sum);
    ASSERT(sum.definition == noPosition && !sum.has(ValueFlag::IsFused));
    if (!test.registerHoldsWholeSum && sum.has(ValueFlag::UpperHalfIsRead)) {
        WayOutOfBranch failingSide = wayOutOfBranchThrough(m_order, block->successor(!test.passingSuccessor).block());
        for (uint32_t use = sum.firstUse; use; use = m_liveness.use(use).next) {
            if (m_liveness.isUseOfUpperHalf(use) && !failingSide.holds(m_liveness.use(use).position))
                return false;
        }
    }

    set(test.sum, ValueFlag::UsesOverflowFlag);
    set(branch, ValueFlag::UsesOverflowFlag);
    return true;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
