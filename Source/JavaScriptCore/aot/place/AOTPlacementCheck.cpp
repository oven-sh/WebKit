/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementCheck.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTFlowResults.h"
#include "AOTSlotPlanSteps.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BitSetUse {
    uint32_t offset { 0 };
    uint32_t first { 0 };
    uint32_t count { 0 };

    friend auto operator<=>(const BitSetUse&, const BitSetUse&) = default;
};

} // anonymous namespace

static constexpr uint32_t sizeOfStaticsWord = sizeof(uint64_t);
static constexpr uint32_t bitSetAlignment = 32;

ASCIILiteral nameOf(PlacementRule rule)
{
    switch (rule) {
    case PlacementRule::None:
        return "None"_s;
#define AOT_PLACEMENT_RULE_NAME(name) \
    case PlacementRule::name: \
        return #name ""_s;
    FOR_EACH_AOT_PLACEMENT_RULE(AOT_PLACEMENT_RULE_NAME)
#undef AOT_PLACEMENT_RULE_NAME
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isInterned(const Program& program, LayoutSetID set)
{
    return set && set.index() < program.types.numberOfLayoutSets();
}

static LayoutID firstMemberOf(const Program& program, LayoutSetID set)
{
    auto& members = program.at(set).members;
    return members.isEmpty() ? LayoutID() : members.first();
}

static uint32_t maxShapesOf(const PlacementCheckContext& context, LayoutID id)
{
    switch (context.inputs.slots.storageOf(id)) {
    case LayoutStorage::CompiledObject:
        return 2;
    case LayoutStorage::Environment:
        return 1;
    case LayoutStorage::None:
    case LayoutStorage::OrdinaryObject:
        return id == context.placement.closureLayout() ? 1 : 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool tryMarkShapeRange(const PlacementCheckContext& context, LayoutID id, BitVector& numbersInRanges)
{
    const ShapeTable& shapes = context.placement.shapes();
    ShapeRange range = shapes.shapesOf(id);
    uint32_t maxShapes = maxShapesOf(context, id);
    if (range.isEmpty())
        return !maxShapes;
    if (range.count > maxShapes || !range.first || static_cast<uint64_t>(range.first) + range.count - 1 > shapes.numberOfShapes())
        return false;
    bool isCompiledObject = context.inputs.slots.storageOf(id) == LayoutStorage::CompiledObject;
    bool isValid = isCompiledObject ? range.end() <= shapes.firstEnvironmentShape() : range.first >= shapes.firstEnvironmentShape();
    for (uint32_t number = range.first; number < range.end(); ++number) {
        if (shapes.shape(number).layout != id || numbersInRanges.quickSet(number))
            isValid = false;
    }
    return isValid;
}

static bool checkShapeNumbers(const PlacementCheckContext& context)
{
    const ShapeTable& shapes = context.placement.shapes();
    uint32_t numberOfShapes = shapes.numberOfShapes();
    bool areValid = true;
    auto fail = [&](LayoutID layout) {
        context.failures.fail(PlacementRule::ShapeNumbers, layout);
        areValid = false;
    };
    BitVector numbersInRanges;
    numbersInRanges.ensureSize(static_cast<size_t>(numberOfShapes) + 1);
    for (unsigned i = 0; i < context.program.layouts.size(); ++i) {
        if (!tryMarkShapeRange(context, LayoutID(i), numbersInRanges))
            fail(LayoutID(i));
    }
    for (uint32_t number = 1; number <= numberOfShapes; ++number) {
        bool isAssigned = shapes.isAssigned(number);
        if (isAssigned != numbersInRanges.quickGet(number) || shapes.shapeCountInRange(number, number) != static_cast<uint32_t>(isAssigned))
            fail(shapes.shape(number).layout);
    }
    return areValid;
}

static void checkShapeVariants(const PlacementCheckContext& context, LayoutID id)
{
    const ShapeTable& shapes = context.placement.shapes();
    ShapeRange range = shapes.shapesOf(id);
    if (range.isEmpty())
        return;
    if (range.count == 2 && (shapes.shape(range.first).keysAreNeverEnumerated || !shapes.shape(range.first + 1).keysAreNeverEnumerated))
        context.failures.fail(PlacementRule::ShapeVariantWrong, id);
    for (AllocationClassID allocationClassID : context.program.at(id).flow.allocationClasses) {
        uint32_t number = shapes.shapeOf(id, allocationClassID);
        if (number < range.first || number >= range.end()) {
            context.failures.fail(PlacementRule::ShapeVariantWrong, id);
            continue;
        }
        const PlacedShape& shape = shapes.shape(number);
        const AllocationClass& allocationClass = context.inputs.flow.at(allocationClassID);
        bool mayEnumerateKeys = allocationClass.keys != KeyEnumeration::NeverEnumerated;
        if ((shape.keysAreNeverEnumerated && mayEnumerateKeys) || !allocationClass.impossibleMutations.containsAll(shape.impossibleMutations))
            context.failures.fail(PlacementRule::ShapeVariantWrong, id);
    }
}

static bool hasExtentOf(const ShapeTable& shapes, const ShapeTest& test, std::span<const uint32_t> ascendingShapes)
{
    auto spansShapes = [&] {
        if (ascendingShapes.empty() || !ascendingShapes.front() || ascendingShapes.back() > shapes.numberOfShapes())
            return false;
        return test.first == ascendingShapes.front() && test.count == ascendingShapes.back() - ascendingShapes.front() + 1;
    };
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
    case ShapeTestForm::AlwaysPasses:
        return true;
    case ShapeTestForm::OneShape:
        return spansShapes() && test.count == 1;
    case ShapeTestForm::UpTo:
    case ShapeTestForm::From:
    case ShapeTestForm::Range:
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
    case ShapeTestForm::BitSet:
        return spansShapes();
    case ShapeTestForm::ImmediateBits:
        return spansShapes() && test.count <= immediateBitsWindow;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<ShapeRange> findTestedBlock(const ShapeTable& shapes, const ShapeTest& test)
{
    uint32_t block = (test.last() - 1) / shapesPerBlock;
    bool isTestable = block < maxTestableBlocks && shapes.numberOfShapes() <= maxShapesForBlockTests;
    if (!isTestable || test.blockByte != static_cast<uint8_t>(shapeWordOf(test.last()) >> 8))
        return std::nullopt;
    uint32_t first = block * shapesPerBlock + 1;
    uint32_t last = std::min((block + 1) * shapesPerBlock, shapes.numberOfShapes());
    return ShapeRange { first, last - first + 1 };
}

static std::optional<size_t> numberOfPassingShapes(const ShapeTable& shapes, const ShapeTest& test, std::span<const uint32_t> testedShapes)
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return 0;
    case ShapeTestForm::AlwaysPasses:
        return shapes.shapeCountInRange(1, shapes.numberOfShapes());
    case ShapeTestForm::OneShape:
    case ShapeTestForm::Range:
        return shapes.shapeCountInRange(test.first, test.last());
    case ShapeTestForm::UpTo:
        return shapes.shapeCountInRange(1, test.last());
    case ShapeTestForm::From:
        return shapes.shapeCountInRange(test.first, shapes.numberOfShapes());
    case ShapeTestForm::Block: {
        auto block = findTestedBlock(shapes, test);
        if (!block)
            return std::nullopt;
        return shapes.shapeCountInRange(block->first, block->end() - 1);
    }
    case ShapeTestForm::BlockOrEarlier: {
        auto block = findTestedBlock(shapes, test);
        if (!block)
            return std::nullopt;
        return shapes.shapeCountInRange(1, block->end() - 1);
    }
    case ShapeTestForm::ImmediateBits: {
        size_t numberPassing = 0;
        for (uint32_t distanceFromLast = 0; distanceFromLast < test.count; ++distanceFromLast)
            numberPassing += (test.bits >> distanceFromLast & 1) && shapes.isAssigned(test.last() - distanceFromLast);
        return numberPassing;
    }
    case ShapeTestForm::BitSet:
        return testedShapes.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkBitsInTable(const PlacementCheckContext& context, const ShapeTest& test, std::span<const uint32_t> passing, LayoutID subject, Vector<BitSetUse>& bitSets)
{
    const BitVector& bits = context.placement.shapes().shapeSetBits();
    size_t begin = test.bitSetOffset;
    size_t end = std::min(begin + test.count, bits.size());
    if (!std::ranges::all_of(passing, [&](uint32_t shape) { return bits.get(begin + test.last() - shape); }))
        context.failures.fail(PlacementRule::MemberFailsTest, subject);
    size_t numberOfBits = 0;
    for (size_t bit = bits.findBit(std::min(begin, end), true); bit < end; bit = bits.findBit(bit + 1, true))
        ++numberOfBits;
    if (numberOfBits != passing.size())
        context.failures.fail(PlacementRule::TestPassesOthers, subject);
    bitSets.append(BitSetUse { test.bitSetOffset, test.first, test.count });
}

static void checkTest(const PlacementCheckContext& context, const ShapeTest& test, std::span<const uint32_t> passing, LayoutID subject, Vector<BitSetUse>& bitSets)
{
    const ShapeTable& shapes = context.placement.shapes();
    if (!holdsForAnyCell(test.form))
        context.failures.fail(PlacementRule::TestFormNeedsKnownShapes, subject);
    if (!hasExtentOf(shapes, test, passing)) {
        context.failures.fail(PlacementRule::TestPassesOthers, subject);
        return;
    }
    if (!std::ranges::all_of(passing, [&](uint32_t shape) { return passes(test, shape, passing); }))
        context.failures.fail(PlacementRule::MemberFailsTest, subject);
    if (numberOfPassingShapes(shapes, test, passing) != passing.size())
        context.failures.fail(PlacementRule::TestPassesOthers, subject);
    if (test.bitSetOffset != noBitSet)
        checkBitsInTable(context, test, passing, subject, bitSets);
}

static void forEachKnownShape(std::span<const uint32_t> passing, std::span<const uint32_t> known, NOESCAPE const Invocable<void(uint32_t shape, bool shouldPass)> auto& functor)
{
    size_t next = 0;
    for (uint32_t shape : known) {
        while (next < passing.size() && passing[next] < shape)
            ++next;
        functor(shape, next < passing.size() && passing[next] == shape);
    }
}

static PlacementRule findKnownShapeFailure(const ShapeTable& shapes, const ShapeTest& test, std::span<const uint32_t> passing, std::span<const uint32_t> known)
{
    Vector<uint32_t, 16> passingKnownShapes;
    forEachKnownShape(passing, known, [&](uint32_t shape, bool shouldPass) {
        if (shouldPass)
            passingKnownShapes.append(shape);
    });
    if (!hasExtentOf(shapes, test, passingKnownShapes.span()) && !hasExtentOf(shapes, test, passing))
        return PlacementRule::TestPassesOthers;
    PlacementRule failure = PlacementRule::None;
    forEachKnownShape(passing, known, [&](uint32_t shape, bool shouldPass) {
        if (passes(test, shape, passing) != shouldPass)
            failure = shouldPass ? PlacementRule::MemberFailsTest : PlacementRule::TestPassesOthers;
    });
    return failure;
}

// An object of a member that exists without a shape, such as a module namespace object, is none of the set's member shapes.
static bool listsEveryObjectByShape(const PlacementCheckContext& context, LayoutSetID id)
{
    const LayoutSet& set = context.program.at(id);
    return set.isComplete && std::ranges::all_of(set.members, [&](LayoutID member) {
        return context.inputs.slots.storageOf(member) == LayoutStorage::CompiledObject || context.program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
}

static PlacementRule findKnownSetFailure(const PlacementCheckContext& context, const ShapeTest& test, LayoutSetID tested, LayoutSetID known)
{
    const ShapeTable& shapes = context.placement.shapes();
    if (!listsEveryObjectByShape(context, known))
        return test == shapes.testOf(tested) ? PlacementRule::None : PlacementRule::TestFormNeedsKnownShapes;
    return findKnownShapeFailure(shapes, test, shapes.memberShapes(tested), shapes.memberShapes(known));
}

static void checkSetTest(const PlacementCheckContext& context, LayoutSetID set, Vector<BitSetUse>& bitSets)
{
    const ShapeTable& shapes = context.placement.shapes();
    LayoutID subject = firstMemberOf(context.program, set);
    if (!shapes.hasTestOf(set)) {
        context.failures.fail(PlacementRule::MemberShapesWrong, subject);
        return;
    }
    Vector<uint32_t, 16> expectedShapes;
    for (LayoutID member : context.program.at(set).members) {
        if (context.inputs.slots.storageOf(member) != LayoutStorage::CompiledObject)
            continue;
        ShapeRange range = shapes.shapesOf(member);
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            expectedShapes.append(shape);
    }
    std::ranges::sort(expectedShapes);
    auto memberShapes = shapes.memberShapes(set);
    if (std::ranges::adjacent_find(expectedShapes) != expectedShapes.end() || !std::ranges::equal(memberShapes, expectedShapes))
        context.failures.fail(PlacementRule::MemberShapesWrong, subject);
    checkTest(context, shapes.testOf(set), memberShapes, subject, bitSets);
}

static void checkTestWithinKnownSet(const PlacementCheckContext& context, std::span<const ShapeTestWithinKnownSet> entries, size_t index)
{
    auto& entry = entries[index];
    if (!isInterned(context.program, entry.tested) || !isInterned(context.program, entry.known)) {
        context.failures.fail(PlacementRule::MemberShapesWrong);
        return;
    }
    LayoutID subject = firstMemberOf(context.program, entry.tested);
    if (index && std::pair { entries[index - 1].tested, entries[index - 1].known } >= std::pair { entry.tested, entry.known })
        context.failures.fail(PlacementRule::MembersNotSorted, subject);
    PlacementRule failure = findKnownSetFailure(context, entry.test, entry.tested, entry.known);
    if (failure != PlacementRule::None)
        context.failures.fail(failure, subject);
}

static void checkBitSets(const PlacementCheckContext& context, Vector<BitSetUse>&& uses)
{
    sortAndRemoveDuplicates(context.build.parallelLoops(), uses);
    for (size_t i = 0; i < uses.size(); ++i) {
        bool overlapsPrevious = i && static_cast<uint64_t>(uses[i - 1].offset) + uses[i - 1].count > uses[i].offset;
        if (uses[i].offset % bitSetAlignment || overlapsPrevious)
            context.failures.fail(PlacementRule::BitSetsOverlap);
    }
}

void checkShapes(const PlacementCheckContext& context)
{
    if (!checkShapeNumbers(context))
        return;
    auto& loops = context.build.parallelLoops();
    forEachIndexInParallel(loops, context.program.layouts.size(), [&](unsigned i) {
        checkShapeVariants(context, LayoutID(i));
    });
    checkBitSets(context, collectInParallel<BitSetUse>(loops, context.program.types.numberOfLayoutSets(), [&](unsigned i, Vector<BitSetUse>& bitSets) {
        checkSetTest(context, LayoutSetID(i), bitSets);
    }));
    auto entries = context.placement.shapes().testsWithinKnownSets();
    forEachIndexInParallel(loops, entries.size(), [&](unsigned i) {
        checkTestWithinKnownSet(context, entries, i);
    });
}

static SlotAccessKey keyOf(const FieldAccess& access)
{
    return { access.key, access.receivers, access.isStore };
}

static bool isCoverageValid(const Program& program, const FieldAccess& access, const SlotAgreement& agreement)
{
    if (agreement.groups.isEmpty())
        return access.coverage == FieldAccess::Coverage::NoMember && !access.slot.hasStorage();
    if (!isInterned(program, access.membersWithField) || !std::ranges::equal(program.at(access.membersWithField).members, agreement.groups[0].members))
        return false;
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        return false;
    case FieldAccess::Coverage::SomeMembers:
        return !access.isStore && !agreement.coversEveryMember();
    case FieldAccess::Coverage::EveryMemberHasSlot:
        return !access.isStore && agreement.membersWithoutSlotKeepBytesZero;
    case FieldAccess::Coverage::EveryMember:
        return agreement.coversEveryMember();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAccessTestValid(const PlacementCheckContext& context, const FieldAccess& access)
{
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        return access.membersWithFieldTest.form == ShapeTestForm::NeverPasses;
    case FieldAccess::Coverage::EveryMemberHasSlot:
    case FieldAccess::Coverage::EveryMember:
        return access.membersWithFieldTest.form == ShapeTestForm::AlwaysPasses;
    case FieldAccess::Coverage::SomeMembers:
        if (!isInterned(context.program, access.membersWithField))
            return false;
        return findKnownSetFailure(context, access.membersWithFieldTest, access.membersWithField, access.receivers) == PlacementRule::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool areContentsValid(const Program& program, const FieldAccess& access, std::span<const LayoutID> membersWithSlot)
{
    if (!access.contents)
        return false;
    const Type& contents = program.at(access.contents);
    bool maySeeEmptySlot = access.coverage != FieldAccess::Coverage::EveryMember || access.slot.isOutOfLine();
    for (LayoutID member : membersWithSlot) {
        FieldReference field { member, *program.at(member).findField(access.key) };
        const Type& declared = program.at(fieldAt(program, declarationOf(program, field)).declared.type);
        if (!(access.isStore ? contents.isSubsetOf(declared, program) : declared.isSubsetOf(contents, program)))
            return false;
        if (flowOf(program, field).occupancy != Occupancy::AlwaysFilled && canLeaveSlot(access.slot.representation))
            maySeeEmptySlot = true;
    }
    return access.isStore || !maySeeEmptySlot || contents.primitives & Primitive::Absent;
}

static void checkFieldAccess(const PlacementCheckContext& context, FieldAccessID id)
{
    const Program& program = context.program;
    const FieldAccess& access = context.placement.fieldAccess(id);
    if (!isInterned(program, access.receivers)) {
        context.failures.fail(PlacementRule::AccessKey);
        return;
    }
    auto fail = [&, subject = firstMemberOf(program, access.receivers)](PlacementRule rule) {
        context.failures.fail(rule, subject);
    };
    if (id.index() && keyOf(context.placement.fieldAccess(FieldAccessID(id.index() - 1))) >= keyOf(access))
        fail(PlacementRule::AccessKey);

    SlotAgreement agreement = context.inputs.slots.agreementOf(program, access.key, access.receivers);
    if (agreement.groups.size() > 1) {
        fail(PlacementRule::AccessLocationDiffers);
        return;
    }
    std::span<const LayoutID> membersWithSlot;
    if (agreement.hasOneLocation()) {
        const SlotGroup& group = agreement.groups[0];
        membersWithSlot = group.members.span();
        bool extensionWordsAgree = !group.slot.needsExtension() || access.extensionWord.hasLocationOf(group.extensionWord);
        if (!access.slot.hasLocationOf(group.slot) || !extensionWordsAgree)
            fail(PlacementRule::AccessLocationDiffers);
    }
    if (!isCoverageValid(program, access, agreement))
        fail(PlacementRule::CoverageWrong);
    if (!isAccessTestValid(context, access))
        fail(PlacementRule::AccessTestWrong);
    if (!areContentsValid(program, access, membersWithSlot))
        fail(PlacementRule::ContentsTooNarrow);
}

void checkFieldAccesses(const PlacementCheckContext& context)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.placement.numberOfFieldAccesses(), [&](unsigned i) {
        checkFieldAccess(context, FieldAccessID(i));
    });
}

static bool isOrderWordValid(const Placement& placement, const PlannedLayout& planned, const PlacedAllocation& allocation, std::span<const FieldIndex> storedFields)
{
    uint32_t word = allocation.orderWord;
    if (planned.storage != LayoutStorage::CompiledObject)
        return !word;
    if (placement.shapes().shape(allocation.shape).keysAreNeverEnumerated)
        return word == unorderedOrderWord;
    if (storedFields.empty())
        return !word;
    auto orders = placement.keyOrders().of(allocation.layout);
    if (word & ~orderAndCountMask || countOf(word) != storedFields.size() || orderOf(word) >= orders.size())
        return false;
    return spanHasPrefix(orders[orderOf(word)].span(), storedFields);
}

// A store cannot tell whether it creates a field whose slot has no absent state, so only the allocation can record the key.
static bool listsFieldsWithoutAbsentState(const PlacementCheckContext& context, const PlannedLayout& planned, const PlacedAllocation& allocation)
{
    if (planned.storage != LayoutStorage::CompiledObject || !context.program.at(allocation.layout).constructor)
        return true;
    if (context.placement.shapes().shape(allocation.shape).keysAreNeverEnumerated)
        return true;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.hasStorage() && !canLeaveSlot(slot.representation) && !allocation.initialFields.contains(field))
            return false;
    }
    return true;
}

static void checkAllocation(const PlacementCheckContext& context, AllocationID id)
{
    const Placement& placement = context.placement;
    const ShapeTable& shapes = placement.shapes();
    const PlacedAllocation& allocation = placement.allocation(id);
    const PlannedLayout& planned = context.inputs.slots.layout(allocation.layout);
    auto fail = [&](PlacementRule rule) {
        context.failures.fail(rule, allocation.layout);
    };
    ShapeRange range = shapes.shapesOf(allocation.layout);
    if (range.isEmpty()) {
        fail(PlacementRule::AllocationOfUnplacedLayout);
        return;
    }
    bool isClosure = allocation.layout == placement.closureLayout();
    bool isSized = isClosure ? allocation.size >= Closure::allocationSize(allocation.numberOfCaptures) && allocation.initialFields.isEmpty() : allocation.size == planned.size;
    if (!isSized || allocation.size % 16)
        fail(PlacementRule::AllocationSize);
    Vector<FieldIndex, 16> storedFields;
    for (FieldIndex field : allocation.initialFields) {
        if (field >= planned.numberOfFields()) {
            fail(PlacementRule::InitialFieldWithoutStorage);
            continue;
        }
        const PlacedSlot& slot = planned.slots[field];
        if (!slot.hasStorage())
            continue;
        storedFields.append(field);
        if (slot.isOutOfLine() && !slot.mayExistWithoutStorage)
            fail(PlacementRule::InitialFieldWithoutStorage);
    }
    if (allocation.shape < range.first || allocation.shape >= range.end() || !shapes.isAssigned(allocation.shape)) {
        fail(PlacementRule::AllocationShape);
        return;
    }
    if (!isOrderWordValid(placement, planned, allocation, storedFields.span()))
        fail(PlacementRule::AllocationOrderWord);
    if (!listsFieldsWithoutAbsentState(context, planned, allocation))
        fail(PlacementRule::AbsentStateMissing);
}

static void checkKeyOrders(const PlacementCheckContext& context, LayoutID id)
{
    const PlannedLayout& planned = context.inputs.slots.layout(id);
    auto orders = context.placement.keyOrders().of(id);
    if (orders.size() > maxKeyOrdersPerLayout || (planned.keysAreNeverEnumerated && !orders.empty()))
        context.failures.fail(PlacementRule::KeyOrderInvalid, id);
    for (size_t i = 0; i < orders.size(); ++i) {
        auto order = orders[i].span();
        Vector<FieldIndex, 16> sorted(order);
        std::ranges::sort(sorted);
        bool everyFieldHasStorage = std::ranges::all_of(order, [&](FieldIndex field) { return field < planned.numberOfFields() && planned.slots[field].hasStorage(); });
        if (std::ranges::adjacent_find(sorted) != sorted.end() || !everyFieldHasStorage)
            context.failures.fail(PlacementRule::KeyOrderInvalid, id);
        if (i + 1 < orders.size() && (!std::ranges::lexicographical_compare(order, orders[i + 1]) || spanHasPrefix(orders[i + 1].span(), order)))
            context.failures.fail(PlacementRule::KeyOrderInvalid, id);
    }
}

// The keys behind the second order word are the keys behind the first one and then the created fields.
static bool isContinuationValid(const KeyOrders& keyOrders, const PlacedFieldCreation& creation, KeyOrderContinuation continuation)
{
    auto orders = keyOrders.of(creation.layout);
    auto keysBehind = [&](uint32_t orderWord) -> std::optional<std::span<const FieldIndex>> {
        if (orderWord & ~orderAndCountMask)
            return std::nullopt;
        if (!countOf(orderWord))
            return orderOf(orderWord) ? std::nullopt : std::optional(std::span<const FieldIndex> { });
        if (orderOf(orderWord) >= orders.size() || countOf(orderWord) > orders[orderOf(orderWord)].size())
            return std::nullopt;
        return orders[orderOf(orderWord)].span().first(countOf(orderWord));
    };
    auto before = keysBehind(continuation.orderWordBefore);
    auto after = keysBehind(continuation.orderWordAfter);
    if (!before || !after || after->size() != before->size() + creation.fields.size())
        return false;
    if (keyOrders.findOrderWord(creation.layout, *before) != continuation.orderWordBefore || keyOrders.findOrderWord(creation.layout, *after) != continuation.orderWordAfter)
        return false;
    return spanHasPrefix(*after, *before) && std::ranges::equal(after->subspan(before->size()), creation.fields);
}

static void checkFieldCreation(const PlacementCheckContext& context, const PlacedFieldCreation& creation)
{
    const KeyOrders& keyOrders = context.placement.keyOrders();
    bool isValid = !creation.fields.isEmpty() && std::ranges::all_of(creation.continuations, [&](KeyOrderContinuation continuation) {
        return isContinuationValid(keyOrders, creation, continuation);
    });
    if (!isValid)
        context.failures.fail(PlacementRule::KeyOrderInvalid, creation.layout);
}

void checkAllocations(const PlacementCheckContext& context)
{
    auto& loops = context.build.parallelLoops();
    forEachIndexInParallel(loops, context.placement.numberOfAllocations(), [&](unsigned i) {
        checkAllocation(context, AllocationID(i));
    });
    forEachIndexInParallel(loops, context.program.layouts.size(), [&](unsigned i) {
        checkKeyOrders(context, LayoutID(i));
    });
    for (const PlacedFieldCreation& creation : context.placement.keyOrders().fieldCreations)
        checkFieldCreation(context, creation);
}

static bool isEncodingAllowed(const Variable& variable, const PlacedVariable& placed, bool isDescribed)
{
    bool readsAsDeclaredFunction = variable.initialization == VariableInitialization::Function && placed.function == variable.function;
    switch (placed.storage) {
    case VariableStorage::None:
        return true;
    case VariableStorage::ClosureTable:
        return placed.encoding == VariableEncoding::FunctionAsZero && readsAsDeclaredFunction && variable.flow.stores == StoreCount::Never;
    case VariableStorage::Statics:
        break;
    }
    switch (placed.encoding) {
    case VariableEncoding::Plain:
        return variable.initialization != VariableInitialization::Function;
    case VariableEncoding::UndefinedAsZero:
        return variable.initialization == VariableInitialization::Undefined && placed.representation == SlotRepresentation::JSValue && !isDescribed;
    case VariableEncoding::FunctionAsZero:
        return readsAsDeclaredFunction;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasValidInitialWord(const Variable& variable, const PlacedVariable& placed)
{
    SlotTraits traits = traitsOf(placed.representation);
    bool isStaticField = !!variable.owner;
    bool isStoredBeforeEveryRead = variable.flow.initialization != InitializationState::Unknown;
    if (isStaticField && isStoredBeforeEveryRead && placed.encoding == VariableEncoding::Plain)
        return traits.hasAbsent && placed.initialWord == traits.absentBits;
    bool mayBeReadBeforeStore = placed.encoding == VariableEncoding::Plain && !isStoredBeforeEveryRead;
    if (!placed.initialWord && !mayBeReadBeforeStore)
        return true;
    switch (variable.initialization) {
    case VariableInitialization::Uninitialized:
        return traits.hasAbsent && placed.initialWord == traits.absentBits;
    case VariableInitialization::Undefined:
        return traits.hasUndefined && placed.initialWord == traits.undefinedBits;
    case VariableInitialization::Function:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkVariable(const PlacementCheckContext& context, VariableID id, bool isDescribed)
{
    const Program& program = context.program;
    const Variable& variable = program.at(id);
    const PlacedVariable& placed = context.placement.variable(id);
    auto fail = [&](PlacementRule rule) {
        context.failures.fail(rule, variable.owner);
    };
    if (!isEncodingAllowed(variable, placed, isDescribed))
        fail(PlacementRule::EncodingNotAllowed);
    if (!placed.hasWordInStatics())
        return;
    bool isFilledAtProgramEntry = placed.region == StaticsRegion::FilledAtProgramEntry;
    bool mayBeFilledAtProgramEntry = placed.initialWord && !context.inputs.slots.variable(id).isUsedBeforeProgramEntry;
    if (isFilledAtProgramEntry != (placed.offset < context.placement.statics().sizeFilledAtProgramEntry) || (isFilledAtProgramEntry && !mayBeFilledAtProgramEntry))
        fail(PlacementRule::VariableRegion);
    if (!hasValidInitialWord(variable, placed))
        fail(PlacementRule::VariableLacksInitialValue);
    bool isStaticField = !!variable.owner;
    if (!canHold(program, placed.representation, program.at(variable.declared.type)) || (isStaticField && !canLeaveSlot(placed.representation)))
        fail(PlacementRule::RepresentationCannotHoldType);
}

static Vector<uint32_t> collectGuardWordOffsets(const PlacementCheckContext& context)
{
    Vector<uint32_t> offsets;
    for (auto& bit : context.placement.guardBits().bits) {
        if (offsets.isEmpty() || offsets.last() != bit.wordOffset)
            offsets.append(bit.wordOffset);
    }
    sortAndRemoveDuplicates(context.build.parallelLoops(), offsets);
    return offsets;
}

static void checkStaticsExtents(const PlacementCheckContext& context)
{
    const StaticsPlan& statics = context.placement.statics();
    Vector<Extent> extents;
    bool areAligned = true;
    auto add = [&](uint32_t offset, uint32_t width) {
        extents.append(extentAt(offset, width));
        if (offset % width)
            areAligned = false;
    };
    for (auto& variable : statics.variables) {
        if (variable.hasWordInStatics())
            add(variable.offset, widthOf(variable.representation));
    }
    for (uint32_t offset : statics.moduleValueOffsets)
        add(offset, sizeOfStaticsWord);
    for (auto& cell : statics.moduleBuiltinCells)
        add(cell.offset, sizeOfStaticsWord);
    for (auto offset : statics.freeCellOffsets) {
        if (offset)
            add(*offset, sizeOfStaticsWord);
    }
    if (!areAligned || !areDisjoint(extents.mutableSpan(), 0, statics.size)) {
        context.failures.fail(PlacementRule::VariableOverlap);
        return;
    }
    for (uint32_t offset : collectGuardWordOffsets(context))
        add(offset, sizeOfStaticsWord);
    if (!areAligned || !areDisjoint(extents.mutableSpan(), 0, statics.size))
        context.failures.fail(PlacementRule::GuardWordOverlap);
}

static void checkRoots(const PlacementCheckContext& context)
{
    const StaticsPlan& statics = context.placement.statics();
    auto roots = statics.roots.span();
    for (size_t i = 0; i < roots.size(); ++i) {
        bool isAscending = !i || roots[i - 1] < roots[i];
        if (!isAscending || roots[i] % sizeOfStaticsWord || static_cast<uint64_t>(roots[i]) + sizeOfStaticsWord > statics.size)
            context.failures.fail(PlacementRule::RootMissing);
    }
    auto checkRoot = [&](uint32_t offset) {
        if (!std::ranges::binary_search(roots, offset))
            context.failures.fail(PlacementRule::RootMissing);
    };
    for (auto& variable : statics.variables) {
        if (variable.hasWordInStatics() && traitsOf(variable.representation).holdsCell)
            checkRoot(variable.offset);
    }
    for (uint32_t offset : statics.moduleValueOffsets)
        checkRoot(offset);
    for (auto& cell : statics.moduleBuiltinCells)
        checkRoot(cell.offset);
}

static void checkClearedRanges(const PlacementCheckContext& context)
{
    const StaticsPlan& statics = context.placement.statics();
    auto endOf = [](const StaticsRange& range) {
        return static_cast<uint64_t>(range.offset) + range.size;
    };
    for (auto& range : statics.clearedEachCollection) {
        auto firstRoot = std::ranges::lower_bound(statics.roots, range.offset);
        bool containsRoot = firstRoot != statics.roots.end() && *firstRoot < endOf(range);
        if (range.offset < statics.sizeFilledAtProgramEntry || endOf(range) > statics.size || containsRoot)
            context.failures.fail(PlacementRule::ClearedRangeIsFilledAtProgramEntry);
    }
    for (unsigned i = 0; i < statics.freeCellOffsets.size(); ++i) {
        auto offset = statics.freeCellOffsets[i];
        if (!offset)
            continue;
        bool isCleared = std::ranges::any_of(statics.clearedEachCollection, [&](const StaticsRange& range) {
            return range.offset <= *offset && static_cast<uint64_t>(*offset) + sizeOfStaticsWord <= endOf(range);
        });
        if (!isCleared)
            context.failures.fail(PlacementRule::ClearedRangeIsFilledAtProgramEntry, LayoutID(i));
    }
}

void checkStatics(const PlacementCheckContext& context)
{
    const Program& program = context.program;
    const StaticsPlan& statics = context.placement.statics();
    if (statics.variables.size() != program.variables.size() || statics.moduleValueOffsets.size() != program.modules.size()) {
        context.failures.fail(PlacementRule::VariableWithoutStorage);
        return;
    }
    const BitVector described = findVariablesDescribedToRunTime(program, context.inputs.flow);
    forEachIndexInParallel(context.build.parallelLoops(), program.variables.size(), [&](unsigned i) {
        checkVariable(context, VariableID(i), described.quickGet(i));
    });
    checkStaticsExtents(context);
    checkRoots(context);
    checkClearedRanges(context);
}

static std::tuple<LayoutID, ClassSide, MemberGuardKind, PropertyKeyID> keyOf(const PlacedGuardBit& bit)
{
    return { bit.classLayout, bit.side, bit.kind, bit.key };
}

void checkGuardBits(const PlacementCheckContext& context)
{
    auto bits = context.placement.guardBits().bits.span();
    Vector<uint32_t> classSideWordOffsets;
    uint64_t usedBits = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        auto& bit = bits[i];
        bool startsClassSide = !i || bits[i - 1].classLayout != bit.classLayout || bits[i - 1].side != bit.side;
        if (startsClassSide) {
            classSideWordOffsets.append(bit.wordOffset);
            usedBits = 0;
        }
        bool isAscending = !i || keyOf(bits[i - 1]) < keyOf(bit);
        bool hasOwnBit = bit.bit < MemberGuardData::bitsPerWord && !(usedBits & bit.mask());
        bool hasWordOfClassSide = bit.wordOffset && bit.wordOffset == classSideWordOffsets.last();
        bool hasKey = !!bit.key;
        if (!isAscending || !hasOwnBit || !hasWordOfClassSide || hasKey != (bit.kind == MemberGuardKind::Member))
            context.failures.fail(PlacementRule::GuardBitsNotDistinct, bit.classLayout);
        usedBits |= bit.mask();
    }
    std::ranges::sort(classSideWordOffsets);
    if (std::ranges::adjacent_find(classSideWordOffsets) != classSideWordOffsets.end())
        context.failures.fail(PlacementRule::GuardBitsNotDistinct);
}

void checkPlacement(const Program& program, const PlacementInputs& inputs, const Placement& placement, const SiteCaches& siteCaches, BuildContext& build)
{
    if (build.diagnostics().hasErrors())
        return;
    PlacementFailureSink failures(build);
    PlacementCheckContext context { program, inputs, placement, siteCaches, build, failures };
    checkSlotPlan(context);
    checkClassCreation(context);
    checkClosureForms(context);
    checkShapes(context);
    checkFieldAccesses(context);
    checkAllocations(context);
    checkStatics(context);
    checkGuardBits(context);
    checkSites(context);
    checkSiteCaches(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
