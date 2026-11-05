/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineLiveness.h"

#if ENABLE(AOT)

#include "AOTMachineFolding.h"
#include "AOTSlotRepresentation.h"
#include <algorithm>
#include <limits>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static_assert(Operation::maxInlineChildren + 1 <= maxLeaves, "a Select has room for both operands of the comparison that it tests");
static_assert(maxOperationsBetweenLoadAndUser + 1 <= UINT8_MAX, "ValueFacts::distanceToTrappingLoad has room for the way from a trapping load to the access that takes its trap over");

// As isComparison(), isLoad() and isStore() say. The opcodes of each kind stand in a row, so the opcode itself answers and no table is read.
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

// The children of these are read where they lie, as many as the list of opcodes gives them.
template<Opcode... opcodes>
constexpr bool fusionOpcodesHaveChildren(unsigned numberOfChildren)
{
    return ((descriptionOf(opcodes).numberOfChildren == numberOfChildren) && ...);
}
static_assert(fusionOpcodesHaveChildren<Neg, Clz, PopCount, Abs, Ceil, Floor, FTrunc, Sqrt, BitwiseCast, SExt8, SExt16, SExt32, ZExt32, Trunc, IToD, FloatToDouble, DoubleToFloat>(1));
static_assert(fusionOpcodesHaveChildren<Load8Z, Load8S, Load16Z, Load16S, Load, Branch>(1));
static_assert(fusionOpcodesHaveChildren<Add, Sub, Mul, MulHigh, UMulHigh, Div, UDiv, Mod, UMod, BitAnd, BitOr, BitXor, Shl, SShr, ZShr, FMin, FMax>(2));
static_assert(fusionOpcodesHaveChildren<Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual, EqualOrUnordered, Store8, Store16, Store>(2));

// Whoever asks knows by the opcode how many there are.
static ALWAYS_INLINE Operation* const* fusionChildrenOf(const Operation* operation)
{
    return operation->children().data();
}

// Throughout this file `values` is Liveness::m_values as it lies, by Operation::index(). It has its full length before the scan begins, so it never moves.
static ALWAYS_INLINE void fusionMark(ValueFacts* values, const Operation* operation, ValueFlag flag)
{
    values[operation->index()].flags |= static_cast<uint16_t>(flag);
}

// The leaves of an operation with one of these are settled: forEachLeaf().
static constexpr uint16_t fusionFlagsOfSettledLeaves = static_cast<uint16_t>(ValueFlag::WorksInMemory) | static_cast<uint16_t>(ValueFlag::GoesByOverflow);

// What an operation is to the rules of tests, by its opcode. One byte answers for all of them, and the bytes of two operands together say whether either asks for a closer look.
namespace FusionTrait {
enum : uint8_t {
    // A load, on a target whose tests read an operand where it lies.
    IsLoad = 1 << 0,
    IsBitAnd = 1 << 1,
    // An Add, a Sub or a SExt32. A comparison that has none of them for an operand reads no sum of extended words: comparisonMayReadSumOfExtendedWords().
    MayBelongToSum = 1 << 2,
    IsPatch = 1 << 3,
    IsComparison = 1 << 4,
    // A Patch, or what has effects. No load moves behind it: fusedLoadCanMoveBehind().
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

// `candidate` is a child of an operation in `block`, whose own read of it is not on record yet.
// A read that is on record comes from the rules of the collector and stands behind that operation, so the candidate needs a place of its own.
static ALWAYS_INLINE bool fusionCanAbsorb(const ValueFacts* values, const Block* block, const Operation* candidate)
{
    return candidate->useCount() == 1 && candidate->owner() == block && values[candidate->index()].lastUse == noPosition;
}

// The range of the candidate goes. In its place the range of one integer may grow as far as the test: a register for a register.
// A narrow reference or an inner pointer would take a second value along, and a float would take a register of the other bank.
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

// A comparison or a BitAnd gives the same wherever it stands behind its children, so the test that alone reads it may stand in a later block.
static ALWAYS_INLINE bool fusionCanAbsorbIntoTest(const ValueFacts* values, const Block* block, const Operation* candidate)
{
    if (candidate->useCount() != 1 || values[candidate->index()].lastUse != noPosition)
        return false;
    return candidate->owner() == block || fusionTestMayStandInLaterBlock(values, candidate);
}

// Step::sources of a Check holds the leaves of its predicate alone.
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

// `sum` is the last child of the access. `numberOfLeaves` is what fusionLeavesInStepOf() says of the operation whose step lists the leaves: the access, or the user that the access is fused into.
// Returns how many leaves that step gains: none where nothing is fused.
static unsigned fuseAddressBelow(ValueFacts* values, unsigned numberOfLeaves, const Operation* access, Operation* sum)
{
    ASSERT(sum == access->lastChild());
    const Block* block = access->owner();
    if (sum->opcode() != Add || !fusionCanAbsorb(values, block, sum))
        return 0;

    // Whoever reads the address takes the first child for the base and looks for a shift or an extension in the second alone.
    Operation* const* operandsOfSum = fusionChildrenOf(sum);
    const Operation* base = operandsOfSum[0];
    Operation* index = operandsOfSum[1];
    // Among the operations without position a pinned register is a register as it stands, and the allocator puts the address of a table of the image in one.
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
    // The record of the fault names one register that holds the reference. With the sum gone that is the index beside the cage base, which then has to be the narrow word.
    bool recordNamesReference = access->traps() || values[access->index()].has(ValueFlag::TakesOverTrap);

    // The forms, from the one that takes in the most: with the extension, then without it, then without the shift either.
    Operation* fusedShift = shift;
    Operation* fusedExtension = extension;
    for (;;) {
        const Operation* leaf = fusedExtension ? extended : fusedShift ? shifted : index;
        unsigned amount = fusedShift ? amountOfShift : 0;
        IndexExtension kind = fusionExtensionOf(fusedExtension);
        // The register of such a word holds the extended word already: Encoder::resolveAddress() says the same.
        if (kind == IndexExtension::ZeroExtend32 && clearsUpperHalfOfRegister(leaf))
            kind = IndexExtension::None;
        // Two leaves stand in the place of the sum, and two in the place of the shift: its amount is one.
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

        // What a form leaves out stays a value and is the index, or what is shifted, as it stands.
        if (fusedExtension)
            fusedExtension = nullptr;
        else if (fusedShift)
            fusedShift = nullptr;
        else
            return 0;
    }
}

// `user` stays and is written by Encoder::encodeInteger(), or is an IToD: their instructions read the low half of a register whatever lies above it.
// A narrow reference has a range of its own, which the rules of the collector go by.
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

// `operands` are the two children of a comparison or a BitAnd that has just been fused into a Branch or a Check in `block`: the test reads the low half of a register whatever lies above it.
static NEVER_INLINE bool fuseTruncationsBelowTest(ValueFacts* values, const Block* block, Operation* const* operands)
{
    for (unsigned index = 0; index < 2; ++index) {
        const Operation* operand = operands[index];
        if (operand->opcode() == Trunc && !operand->isNarrowReference() && !fusionChildrenOf(operand)[0]->hasNoPosition() && fusionCanAbsorb(values, block, operand))
            fusionMark(values, operand, ValueFlag::IsFused);
    }
    return true;
}

// fuseAddressBelow() refuses every other address, so whoever asks this first need not call it.
static ALWAYS_INLINE bool fusionMayFuseAddress(const Operation* access, const Operation* address)
{
    return address->opcode() == Add && address->useCount() == 1 && address->owner() == access->owner() && !access->readsZeroOffNoCell();
}

// It is no Patch and need not stay. What has to stay, a Patch aside, has effects or traps: Operation::mustStay().
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

// Both stand in one block. The scan has not been at any of the operations between them, so it goes by what they are and not by whether they stay.
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

// Whether one instruction tests what the load reads where it lies, as wide as it lies there. `other` is the operand beside it, null where the word itself is tested.
// Where the encoder finds no such instruction after all, the word goes through the scratch register.
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
    // What stands below comes to this for a byte that is extended with zeros, which most tests of a narrow word read: its bounds are these, and every bit that counts lies in the one byte.
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

// On x86-64. Whether `tested` can read its child `index`, which is `load`, where the word lies. It marks nothing.
// `tested` is a comparison or a BitAnd that is fused into `root`, or `root` itself, a Branch or a Select, which then tests its first child as it stands.
// `other` is the operand beside the load, null where the word itself is tested.
static ALWAYS_INLINE bool fusionTestCanReadLoad(const ValueFacts* values, const Operation* root, const Operation* tested, unsigned index, const Operation* load, const Operation* other)
{
    if (!fusionIsLoad(load->opcode()) || load->traps() || load->has(OperationFlag::IsInnerPointer))
        return false;
    // The handler of its fault knows the comparison of the whole word with a constant.
    if (load->readsZeroOffNoCell() && (index || !other || !other->hasInt32() || !fusionIsComparison(tested->opcode())))
        return false;
    if (!fusionCanAbsorb(values, root->owner(), load) || values[load->index()].has(ValueFlag::TakesOverTrap))
        return false;
    if (!fusionCanTestLoadInMemory(tested, load, other))
        return false;
    if (!acceptsMemoryOperand(*tested, index))
        return false;
    // It would be read from a word of the instance, and an instruction reads memory once.
    if (other && other->hasNoPosition() && !acceptsImmediate(*tested, 1 - index, *other))
        return false;
    // None of these questions changes anything. This one goes through the block, so it comes last, when nothing is left to ask of the operand beside the load.
    return fusedLoadCanMoveTo(load, root);
}

// `tested` is a comparison or a BitAnd that is fused into `root`, or `root` itself, which then tests its first child as it stands. `numberOfLeaves`: as for fuseAddressBelow().
// Returns how many leaves `root` gains, or nothing where no load is fused.
static std::optional<unsigned> fuseLoadBelowTest(ValueFacts* values, const Operation* root, const Operation* tested, unsigned numberOfLeaves)
{
    // The allocator asks a Check for its operands in memory by other rules.
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

// fusionTestCanReadLoad() holds of the load. `numberOfLeaves`: as for fuseAddressBelow(). The address is read ahead of the mark, while the opcode still says where the child of a load lies.
static ALWAYS_INLINE bool fuseLoadThatTestCanRead(ValueFacts* values, const Operation* load, unsigned numberOfLeaves)
{
    Operation* address = fusionChildrenOf(load)[0];
    fusionMark(values, load, ValueFlag::IsFused);
    if (!fusionMayFuseAddress(load, address))
        return true;
    return fuseAddressBelowFusedLoad(values, numberOfLeaves, load, address);
}

// As fuseLoadBelowOneTestOfBranch(), where the second operand is a load.
static NEVER_INLINE bool fuseEitherLoadBelowOneTestOfBranch(ValueFacts* values, const Operation* branch, const Operation* tested, Operation* const* operands)
{
    if (fusionTestCanReadLoad(values, branch, tested, 0, operands[0], operands[1]))
        return fuseLoadThatTestCanRead(values, operands[0], 2);
    if (fusionTestCanReadLoad(values, branch, tested, 1, operands[1], operands[0]))
        return fuseLoadThatTestCanRead(values, operands[1], 2);
    return true;
}

// `tested` is the one comparison or BitAnd that is fused into the Branch. `operands` are its children, which are the two leaves of the Branch. It has been marked, so the answer is true.
// It calls nothing that comes back, and so saves no register: what is left to do is jumped to.
static NEVER_INLINE bool fuseLoadBelowOneTestOfBranch(ValueFacts* values, const Operation* branch, const Operation* tested, Operation* const* operands)
{
    ASSERT(isX86_64() && branch->opcode() == Branch && operands == fusionChildrenOf(tested));
    // A load mostly stands on the left, beside a constant. With no second try to come, nothing but the load is kept for later.
    if (fusionTraitsOf(operands[1]) & FusionTrait::IsLoad) [[unlikely]]
        return fuseEitherLoadBelowOneTestOfBranch(values, branch, tested, operands);
    if (!fusionTestCanReadLoad(values, branch, tested, 0, operands[0], operands[1]))
        return true;
    return fuseLoadThatTestCanRead(values, operands[0], 2);
}

// The Branch tests the load as it stands.
static NEVER_INLINE bool fuseLoadTestedByBranch(ValueFacts* values, const Operation* branch, const Operation* load)
{
    ASSERT(isX86_64() && branch->opcode() == Branch && fusionChildrenOf(branch)[0] == load);
    if (!fusionTestCanReadLoad(values, branch, branch, 0, load, nullptr))
        return false;
    return fuseLoadThatTestCanRead(values, load, 1);
}

// `operands` are the children of a BitAnd that is fused into a Branch in `block`, which has `numberOfLeaves`. (value >> amount) & 1 is the bit of the value that the amount names, whichever way the sign goes.
// One instruction of x86-64 tests it, with the amount in any register. There the builder has made a Patch of the shift: appendWithFixedRegisters().
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
    // The value and the amount stand in the place of the shift.
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

// It used to run behind the test of the reference. A load through a value may lean on that test, and its fault would have no record.
// A load through the frame, the instance or the code leans on nothing.
static bool fusionCanRunAheadOfTrap(const Operation* between)
{
    if (fusionIsLoad(between->opcode()))
        return !between->traps() && fusionChildrenOf(between)[0]->hasNoPosition();
    return between->opcode() != Patch && !between->mustStay();
}

// False only where parseSumTestedByOverflow() is: what that takes for the sum is the left operand, the first child of a left operand that is an Add, or the right operand beside a SExt32, and it is an Add or a Sub.
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

// `user` is a Branch, a Select or a Check that stays, and `predicate` its first child.
static ALWAYS_INLINE bool fusePredicateBelow(const BackwardScan& scan, ValueFacts* values, const Operation* user, const Operation* predicate)
{
    Opcode opcodeOfUser = user->opcode();
    Opcode opcode = predicate->opcode();
    const Block* block = user->owner();
    // What fusionLeavesInStepOf() says of the user, kept up as operations are fused.
    // What has one use is fused by its user alone, and the scan has been neither at a child of the user nor at anything below one. So none of them is fused so far, and the children of what is fused here stand in its place as leaves.
    unsigned numberOfLeaves = opcodeOfUser == Select ? user->numChildren() : 1;
    ASSERT_UNUSED(scan, numberOfLeaves == fusionLeavesInStepOf(scan, user));
    if (fusionIsLoad(opcode))
        return fuseLoadBelowTest(values, user, user, numberOfLeaves).has_value();

    // Of two operations that give one or zero, both hold where their BitAnd is not zero, and one does where their BitOr is not. So a Branch on either tests twice, with a jump behind each test.
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
            // The two operands of each test stand in the place of the predicate.
            numberOfLeaves += 3;
        }
    }
    if (!numberOfTests) {
        if ((!fusionIsComparison(opcode) && opcode != BitAnd) || !fusionCanAbsorbIntoTest(values, block, predicate))
            return false;

        // It puts two leaves in the place of one. A constant is a leaf too: whether it stands in the instruction is asked of acceptsImmediate() when the leaf is placed.
        if (numberOfLeaves + 1 > maxLeaves)
            return false;
        fusionMark(values, predicate, ValueFlag::IsFused);
        numberOfTests = 1;
        ++numberOfLeaves;
    }

    // Each test is a comparison or a BitAnd that has just been fused. It takes in what its instruction can read, as far as the user has room for the leaves.
    // The allocator asks a Check for its operands in memory by other rules.
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
        // One leaf in the place of one.
        if (opcodeOfUser != Select && ((fusionTraitsOf(operands[0]) | fusionTraitsOf(operands[1])) & FusionTrait::IsTruncation))
            fuseTruncationsBelowTest(values, block, operands);
        if (!mayFuseLoad || (!fusionIsLoad(operands[0]->opcode()) && !fusionIsLoad(operands[1]->opcode())))
            continue;
        if (std::optional<unsigned> numberOfLeavesAdded = fuseLoadBelowTest(values, user, tested, numberOfLeaves)) {
            // The allocator counts one operand in memory for a step, though a step that tests twice is two instructions.
            mayFuseLoad = false;
            numberOfLeaves += *numberOfLeavesAdded;
        }
    }
    ASSERT(numberOfLeaves == fusionLeavesInStepOf(scan, user) && numberOfLeaves <= maxLeaves);
    return true;
}

// `predicate` is a comparison, a BitAnd, a BitOr or a load. This way is right for every test. Most Branches and Checks go a shorter one: fuseOneTestInto().
// The functions that a test is handed on among take their arguments in one order, so that each is jumped to with the registers as they are.
static NEVER_INLINE bool fuseIntoTest(ValueFacts* values, Operation* user, const Operation* predicate, BackwardScan& scan)
{
    if (values[user->index()].flags & fusionFlagsOfSettledLeaves)
        return false;
    if (user->opcode() == Branch && fusionIsComparison(predicate->opcode()) && comparisonMayReadSumOfExtendedWords(predicate) && scan.tryTestSumByOverflow(user))
        return true;
    return fusePredicateBelow(scan, values, user, predicate);
}

// `bits` is the BitAnd that alone is fused into the Branch, `operands` are its children, and the first of them is a Patch.
static NEVER_INLINE bool fuseShiftOrLoadBelowBitAndOfBranch(ValueFacts* values, const Operation* branch, const Operation* bits, Operation* const* operands)
{
    ASSERT(branch->opcode() == Branch && bits->opcode() == BitAnd && operands == fusionChildrenOf(bits) && operands[0]->opcode() == Patch);
    if (fuseShiftBelowBitTest(values, branch->owner(), operands, 2))
        return true;
    if (!(fusionTraitsOf(operands[1]) & FusionTrait::IsLoad))
        return true;
    return fuseLoadBelowOneTestOfBranch(values, branch, bits, operands);
}

// fusePredicateBelow() looks for a BitAnd to fuse below such a comparison.
static ALWAYS_INLINE bool fusionComparesBitAndWithZero(const Operation* comparison, Operation* const* operands)
{
    Opcode opcode = comparison->opcode();
    return (opcode == Equal || opcode == NotEqual) && operands[0]->opcode() == BitAnd && operands[1]->isInt(0);
}

// The short way of a Branch or a Check whose leaves are not settled, on a comparison or a BitAnd. It marks what fuseIntoTest() would.
// One test in the block of its user, with operands that are values, constants or loads, is nearly all there is. What may be tested twice or go by overflow, what stands in an earlier block
// and what compares a BitAnd with zero is handed to fuseIntoTest() with nothing marked so far, which begins anew.
// One test never has too many leaves: two of its own, and one more for a shift or two for an address below them.
template<Opcode opcodeOfUser, bool predicateIsBitAnd>
static ALWAYS_INLINE bool fuseOneTestInto(ValueFacts* values, Operation* user, const Operation* predicate, BackwardScan& scan)
{
    static_assert(opcodeOfUser == Branch || opcodeOfUser == Check);
    static_assert(maxLeaves >= 4);
    ASSERT(user->opcode() == opcodeOfUser && (predicateIsBitAnd ? predicate->opcode() == BitAnd : isComparison(predicate->opcode())));
    constexpr uint16_t isFused = static_cast<uint16_t>(ValueFlag::IsFused);

    // However it is tested, the test alone reads it.
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
        // The allocator asks a Check for its operands in memory by other rules, and a Check neither tests twice nor goes by overflow.
        if constexpr (!predicateIsBitAnd) {
            if (fusionComparesBitAndWithZero(predicate, operands)) [[unlikely]]
                return fuseIntoTest(values, user, predicate, scan);
        }
    } else {
        if constexpr (predicateIsBitAnd) {
            if (traits & (FusionTrait::IsComparison | FusionTrait::IsBitAnd | FusionTrait::IsPatch | FusionTrait::IsLoad)) {
                // Each of two tests gives one or zero: fusionGivesOneOrZero().
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

// Like what it jumps to, it calls nothing that comes back and saves no register.
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

// A Check takes in a comparison or a BitAnd, and nothing else.
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

// Nothing is fused into an access but its address, so it has as many leaves as children.
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

static bool fuseScaledIndexBelow(ValueFacts* values, const Operation* sum)
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
        // A narrow reference has a range of its own, which the rules of the collector go by. So has the owner of an inner pointer, which is the first child.
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
            if (kind == IndexExtension::ZeroExtend32 && clearsUpperHalfOfRegister(leaf))
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

// `load` is that child of the user. `numberOfLeaves`: as for fuseAddressBelow().
static bool fuseLoadBelowComputation(ValueFacts* values, const Operation* user, unsigned childIndex, Operation* load, unsigned numberOfLeaves)
{
    // The instruction reads as many bytes as the type of the child has, so a load that extends a byte or a half is none for it.
    // What is computed from an inner pointer has the load for its owner, and a fused load has no range that could hand that on to its base.
    if (!isX86_64() || load->opcode() != Load || load->traps() || load->has(OperationFlag::IsInnerPointer) || load->readsZeroOffNoCell())
        return false;
    if (!fusionCanAbsorb(values, user->owner(), load) || values[load->index()].has(ValueFlag::TakesOverTrap))
        return false;
    // Neither question changes anything, so the target is asked last.
    if (!fusedLoadCanMoveTo(load, user) || !acceptsMemoryOperand(*user, childIndex))
        return false;

    Operation* address = fusionChildrenOf(load)[0];
    fusionMark(values, load, ValueFlag::IsFused);
    if (fusionMayFuseAddress(load, address))
        fuseAddressBelow(values, numberOfLeaves, load, address);
    return true;
}

// What a computation may take in, by the opcode of the child: rules 6, 7 and 4.
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

// `kinds` holds what fusibleKindOf() says of the children that have one use. A rule that would find no such child of its kind marks nothing, so it is not tried.
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
    // At most one child is read from memory. A fused Trunc has one child, as a fused load has, so the user still has as many leaves as children.
    for (unsigned index = numberOfChildren; index--;) {
        if (fuseLoadBelowComputation(values, user, index, children[index], numberOfChildren))
            return true;
    }
    return hasFused;
}

// Like fuseIntoBranch(), it returns whether it has marked anything. Where it has not, and the leaves of the user were not settled before, they are its children: whatever is fused has one use, and its user alone fuses it.
// It goes by the opcode of the user and then by those of its children, and looks at nothing else until they leave something to fuse.
static NEVER_INLINE bool fuseIntoOtherThanBranch(ValueFacts* values, Operation* user, BackwardScan& scan)
{
    switch (user->opcode()) {
    // Goes by whether its first child is zero.
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

    // Its last child is an address.
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

    // One instruction from values to a value.
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
        // Each of the rules takes in what has one use.
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

// It is kept this small so that it is copied into the scan wherever the two are compiled together, and a Branch is then one call away.
void BackwardScan::fuseInto(Operation* user)
{
    ValueFacts* values = m_liveness.m_values.begin();
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
    // Both read it, so it has two uses, is fused into neither address, and lies in the base register of the access that takes over.
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
        // The address is the last child: the one child of a load, the second of a store.
        Opcode opcode = candidate->opcode();
        bool isStoreBehind = fusionIsStore(opcode);
        if ((!isStoreBehind && !fusionIsLoad(opcode)) || fusionChildrenOf(candidate)[isStoreBehind ? 1 : 0] != base) {
            if (!fusionCanRunAheadOfTrap(candidate))
                return false;
            continue;
        }

        // It faults where the load would have. If it cannot answer for that fault, the load stays in front of it.
        // An access has one record, and one that traps or has taken a trap over owes it to another origin.
        if (scanned.has(ValueFlag::IsFused) || scanned.has(ValueFlag::TakesOverTrap) || candidate->traps() || candidate->readsZeroOffNoCell() || !fusionAccessLiesWithinTrapReach(candidate))
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

// Where the low half neither carries nor borrows, the sum has the high half of the old word. So it has not left these.
static_assert((maxIntegerAddedTo | memoryUpdateBitsOfLowHalf) == maxIntegerAddedTo && !(minIntegerAddedTo & memoryUpdateBitsOfLowHalf));

// The sums with which a test lets the Store be reached.
struct MemoryUpdatePassingSums {
    std::optional<int64_t> min;
    std::optional<int64_t> max;
};

// Sums of a few of them do not overflow.
static bool memoryUpdateCanReckonWith(int64_t bound, bool wordIsWide)
{
    constexpr int64_t limit = std::numeric_limits<int64_t>::max() / 4;
    return wordIsWide ? bound > -limit && bound < limit : isRepresentableAs<int32_t>(bound >> 2);
}

static bool memoryUpdateIsSignedOrder(Opcode relation)
{
    return relation == LessThan || relation == LessEqual || relation == GreaterThan || relation == GreaterEqual;
}

// The Load of the word that the Store writes, as the child stands or below the extension of a word of 32 bits.
static const Operation* memoryUpdateLoadBelow(const Operation* child, const Operation* store)
{
    Type typeOfWord = store->child(0)->type();
    if (child->type() != typeOfWord) {
        if (child->opcode() != SExt32)
            return nullptr;
        child = child->child(0);
    }
    // A load from an immutable heap may sink behind the Branch that stores.
    if (child->opcode() != Load || child->type() != typeOfWord || child->traps() || child->readsZeroOffNoCell() || child->has(OperationFlag::ReadsImmutableHeap))
        return nullptr;
    return child->lastChild() == store->lastChild() && child->offset() == store->offset() ? child : nullptr;
}

static bool memoryUpdateIsExtension(const Operation* operand)
{
    return operand->opcode() == SExt32 || operand->opcode() == ZExt32;
}

// False only where parseMemoryUpdate() is. What works in memory is one of five computations on either target, and memoryUpdateLoadBelow() finds the word in a Load or below a SExt32.
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

// Everything but who else reads the result, and what stands between the Load and the Store.
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
    // The instruction reads 32 bits, which the extension has left as they were.
    if (load->type() == Int32 && operand->type() == Int64 && memoryUpdateIsExtension(operand)) {
        update.userOfOperand = operand;
        update.indexOfOperand = 0;
    }
    return true;
}

// The sums for which sum + bias `relation` bound. Neither the sum nor what is added to it wraps: the caller knows how far the sum reaches.
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

// Whether a sum from `minSum` to `maxSum` passes just where the addition or subtraction of 32 bits does not overflow and gives no less than `leastResult`.
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

// The old word may be any int32.
static bool memoryUpdateIsTestedByOverflow(const MemoryUpdatePassingSums& passing, const IntegerRange& operands, bool subtracts, std::optional<int32_t>& leastStored)
{
    int64_t minSum = memoryUpdateMinInt32 + (subtracts ? -operands.max : operands.min);
    int64_t maxSum = memoryUpdateMaxInt32 + (subtracts ? -operands.min : operands.max);
    return sumIsTestedByOverflow(passing, minSum, maxSum, leastStored);
}

// How a comparison reads a sum: sum + bias `relation` bound, or sum == SExt32(Trunc(sum)), which says that the sum is an int32.
struct TestOfSum {
    // Add(sum, bias). Null where the sum is compared as it stands.
    const Operation* biasedSum { nullptr };
    // SExt32(lowHalf) and Trunc(sum). Null where the sum is compared with a constant.
    const Operation* extensionOfLowHalf { nullptr };
    const Operation* lowHalf { nullptr };
    int64_t bias { 0 };
    int64_t bound { 0 };
};

// Everything but the relation, and who else reads these.
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

// `update` is what parseMemoryUpdate() says of the Store, and the Branch ends the one block that leads to the block of the Store.
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

    // The sum wraps as the instruction does, and the comparison reads what the instruction has left in the word.
    if (!truncation && !wordIsWide) {
        if (isBiased || !block->successor(!update.successorThatStores).isRare())
            return false;
        update.kind = MemoryUpdate::TestedBehind;
        update.comparesWord = true;
        return true;
    }

    // Where the extension was taken off, it is the user.
    const Operation* operand = update.userOfOperand == sum ? update.operand() : update.userOfOperand;
    bool operandIsConstant = operand->hasInt();
    if (operandIsConstant && !isRepresentableAs<int32_t>(update.constant))
        return false;
    IntegerRange operands = rangeOf(operand);

    // The sum does not wrap, so sum `relation` bound holds just where word `relation` bound - constant does.
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
        // Where the word can be compared in memory, a constant is taken off the bound instead, which saves the second test.
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

    // Of any other word of 64 bits the sum may wrap.
    if (!update.load->has(OperationFlag::IsIntegerAddedTo))
        return false;
    bool adds = update.opcode == Add;
    IntegerRange amounts = operands;
    // A constant below zero goes the other way. A register would hold the constant as it stands, and what the least int32 is taken off by is no immediate.
    if (operandIsConstant && update.constant < 0 && update.constant > memoryUpdateMinInt32 && acceptsImmediate(*sum, update.indexOfOperand, *operand)) {
        adds = !adds;
        amounts = { -update.constant, -update.constant };
    }
    // The way back from a carry needs the scratch register for the sum, so none is left for the address of a word that a symbol names.
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
    // No more than 32 bits of the amount are set, so its low half extended with zeros is the amount.
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

// `storeThatMovesAhead` is null where the Store stays behind the operation.
static bool memoryUpdateMayStandBetween(const HeapTree& heaps, const Operation* between, const Operation* storeThatMovesAhead)
{
    Opcode opcode = between->opcode();
    if (opcode == Jump || opcode == Branch || opcode == Switch)
        return true;
    if (opcode == Patch) {
        const PatchOperation* patch = static_cast<const PatchOperation*>(between);
        if (patch->onlyKeepsChildrenAlive)
            return true;
        // Of what a Patch reads only this much is known: whether it reads at all.
        return !patch->isSuspension() && (storeThatMovesAhead ? patch->effects.maySink() : !patch->effects.mustStay());
    }
    if (between->mustStay())
        return false;
    return !storeThatMovesAhead || !isLoad(opcode) || !heaps.overlap(between->heap(), storeThatMovesAhead->heap());
}

// The scan has not been at any of these, so it goes by what they are and not by whether they stay.
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
        // The Branch ends this block.
        storeThatMovesAhead = nullptr;
    }
}

// `storesBehindBranches` is Liveness::m_storesBehindBranches.
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
        // Outside loops what reads the word soon after stands behind the Store, and the scan has been there.
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

// The instruction reads 32 bits, which an extension has left as they were, and so has a Trunc below it.
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

// Everything but who reads the upper half of the sum, and what the successors of the block are.
static bool parseSumTestedByOverflow(const Operation* branch, SumTestedByOverflow& result)
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

    // So each is the sign extension of its low half, and the sum in 64 bits is the sum of two int32.
    IntegerRange int32s { memoryUpdateMinInt32, memoryUpdateMaxInt32 };
    IntegerRange lefts = rangeOf(sum->child(0));
    IntegerRange rights = rangeOf(sum->child(1));
    if (!int32s.contains(lefts) || !int32s.contains(rights))
        return false;
    bool subtracts = sum->opcode() == Sub;
    int64_t minSum = lefts.min + (subtracts ? -rights.max : rights.min);
    int64_t maxSum = lefts.max + (subtracts ? -rights.min : rights.max);

    bool isTestedByOverflow = false;
    for (uint8_t successorThatPasses : { 0, 1 }) {
        Opcode relationThatPasses = successorThatPasses ? invertedComparison(relation) : relation;
        std::optional<MemoryUpdatePassingSums> passing;
        if (!test.lowHalf)
            passing = memoryUpdateSumsThatPass(relationThatPasses, test.bias, test.bound, false);
        else if (relationThatPasses == Equal)
            passing = MemoryUpdatePassingSums { memoryUpdateMinInt32, memoryUpdateMaxInt32 };
        std::optional<int32_t> leastResult;
        if (!passing || !sumIsTestedByOverflow(*passing, minSum, maxSum, leastResult))
            continue;
        result.successorThatPasses = successorThatPasses;
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

bool mayTestSumByOverflow(const Operation* branch)
{
    if (!branchMayReadSumOfExtendedWords(branch))
        return false;
    SumTestedByOverflow result;
    return parseSumTestedByOverflow(branch, result);
}

SumTestedByOverflow sumTestedByOverflowOf(const Liveness& liveness, const Operation* sumOrBranch)
{
    ASSERT(liveness.goesByOverflow(sumOrBranch));
    SumTestedByOverflow result;
    const Operation* branch = sumOrBranch->opcode() == Branch ? sumOrBranch : sumOrBranch->owner()->last();
    bool isAsScanned = parseSumTestedByOverflow(branch, result) && (branch == sumOrBranch || result.sum == sumOrBranch);
    RELEASE_ASSERT(isAsScanned);
    result.isWidenedWhereTestFails = !result.registerHoldsWholeSum && liveness.of(result.sum).has(ValueFlag::UpperHalfIsRead);
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
    SumTestedByOverflow test;
    if (!parseSumTestedByOverflow(branch, test))
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
        WayOutOfBranch failingSide = wayOutOfBranchThrough(m_order, block->successor(!test.successorThatPasses).block());
        for (uint32_t use = sum.firstUse; use; use = m_liveness.use(use).next) {
            if (m_liveness.isUseOfUpperHalf(use) && !failingSide.holds(m_liveness.use(use).position))
                return false;
        }
    }

    set(test.sum, ValueFlag::GoesByOverflow);
    set(branch, ValueFlag::GoesByOverflow);
    return true;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
