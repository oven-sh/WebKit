/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementCheck.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTFlowResults.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTSlotPlanSteps.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BitSetUse {
    uint32_t firstWord { 0 };
    uint32_t numberOfWords { 0 };

    friend auto operator<=>(const BitSetUse&, const BitSetUse&) = default;
};

} // anonymous namespace

static constexpr uint32_t sizeOfStaticsWord = sizeof(uint64_t);

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
    if (context.placement.layout(id).keysAreNeverEnumerated && !shapes.shape(range.first).keysAreNeverEnumerated)
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
    std::span<const uint64_t> table = context.placement.shapes().shapeSetWords();
    uint32_t firstWord = PagedBitSet::firstWordAt(test.bitSetOffset);
    PagedBitSet::Entries entries = PagedBitSet::entriesAt(test.bitSetOffset);
    if (entries == PagedBitSet::Entries::Hashed) {
        if (firstWord >= table.size() || !PagedBitSet::shiftIn(table[firstWord]) || PagedBitSet::shiftIn(table[firstWord]) > 31 || firstWord + PagedBitSet::numberOfWordsOfHashedSet(PagedBitSet::shiftIn(table[firstWord])) > table.size()) {
            context.failures.fail(PlacementRule::BitSetsOverlap, subject);
            return;
        }
        uint64_t head = table[firstWord];
        uint64_t numberOfWords = PagedBitSet::numberOfWordsOfHashedSet(PagedBitSet::shiftIn(head));
        std::span<const uint64_t> words = table.subspan(firstWord, numberOfWords);
        std::span<const PagedBitSet::Slot> slots = spanReinterpretCast<const PagedBitSet::Slot>(words.subspan(1)).first(PagedBitSet::numberOfSlots(PagedBitSet::shiftIn(head)));
        size_t numberOfMembers = 0;
        for (uint32_t slot = 0; slot < slots.size(); ++slot)
            numberOfMembers += PagedBitSet::slotOf(slots[slot], PagedBitSet::multiplierIn(head), PagedBitSet::shiftIn(head)) == slot;
        if (!std::ranges::all_of(passing, [&](uint32_t shape) { return PagedBitSet::contains(words.data(), entries, test.last() - shape); }))
            context.failures.fail(PlacementRule::MemberFailsTest, subject);
        if (numberOfMembers != passing.size())
            context.failures.fail(PlacementRule::TestPassesOthers, subject);
        bitSets.append(BitSetUse { firstWord, static_cast<uint32_t>(numberOfWords) });
        return;
    }
    uint64_t numberOfPages = numberOfPagesInShapeSet(test.count);
    uint64_t numberOfWords = PagedBitSet::numberOfEntryWords(numberOfPages, entries);
    if (firstWord + numberOfWords > table.size()) {
        context.failures.fail(PlacementRule::BitSetsOverlap, subject);
        return;
    }
    const uint64_t* words = table.subspan(firstWord).data();
    size_t numberOfBits = 0;
    for (uint64_t page = 0; page < numberOfPages; ++page) {
        uint64_t word = PagedBitSet::wordOfPage(words, entries, page);
        if (firstWord + word >= table.size()) {
            context.failures.fail(PlacementRule::BitSetsOverlap, subject);
            return;
        }
        numberOfWords = std::max<uint64_t>(numberOfWords, word + 1);
        numberOfBits += std::popcount(words[word]);
    }
    if (!std::ranges::all_of(passing, [&](uint32_t shape) { return PagedBitSet::contains(words, entries, test.last() - shape); }))
        context.failures.fail(PlacementRule::MemberFailsTest, subject);
    if (numberOfBits != passing.size())
        context.failures.fail(PlacementRule::TestPassesOthers, subject);
    bitSets.append(BitSetUse { firstWord, static_cast<uint32_t>(numberOfWords) });
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
    AOT::sort(expectedShapes);
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
        if (i && static_cast<uint64_t>(uses[i - 1].firstWord) + uses[i - 1].numberOfWords > uses[i].firstWord)
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

static bool isCoverageValid(const Program& program, const FieldAccess& access, const SlotAgreement& agreement, std::span<const LayoutID> membersWithSlot)
{
    if (agreement.groups.isEmpty())
        return access.coverage == FieldAccess::Coverage::NoMember && !access.slot.hasStorage();
    if (!isInterned(program, access.membersWithField) || !std::ranges::equal(program.at(access.membersWithField).members, membersWithSlot))
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
        const Type& held = program.at(slotTypeOf(program, fieldAt(program, declarationOf(program, field))));
        if (!(access.isStore ? contents.isSubsetOf(held, program) : held.isSubsetOf(contents, program)))
            return false;
        if (flowOf(program, field).occupancy != Occupancy::AlwaysFilled && canLeaveSlot(access.slot.representation))
            maySeeEmptySlot = true;
    }
    if (access.isStore)
        return true;
    // A field that the object lists without storage for it holds undefined.
    if (access.slot.mayExistWithoutStorage && !(contents.primitives & Primitive::Undefined))
        return false;
    return !maySeeEmptySlot || contents.primitives & Primitive::Absent;
}

// A read asks the key order where some member may list the field without storage. A store of undefined asks for no storage only where every member may.
static bool hasStorageRuleOfMembers(const PlacementCheckContext& context, const FieldAccess& access, std::span<const LayoutID> membersWithSlot)
{
    if (membersWithSlot.empty())
        return !access.slot.mayExistWithoutStorage;
    auto mayExistWithoutStorage = [&](LayoutID member) {
        return context.inputs.slots.slot(member, *context.program.at(member).findField(access.key)).mayExistWithoutStorage;
    };
    bool expected = access.isStore ? std::ranges::all_of(membersWithSlot, mayExistWithoutStorage) : std::ranges::any_of(membersWithSlot, mayExistWithoutStorage);
    return access.slot.mayExistWithoutStorage == expected;
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

    const SlotAgreement& agreement = context.inputs.slots.agreementOf(program, access.key, access.receivers);
    if (access.findsOffsetByShape != agreement.hasOffsetByShape(access.isStore) || (agreement.groups.size() > 1 && !access.findsOffsetByShape)) {
        fail(PlacementRule::AccessLocationDiffers);
        return;
    }
    std::span<const LayoutID> membersWithSlot;
    SlotGroup group;
    if (!agreement.groups.isEmpty()) {
        group = access.findsOffsetByShape ? agreement.groupOfAllOffsets() : agreement.groups[0];
        membersWithSlot = group.members.span();
        bool extensionWordsAgree = !group.slot.needsExtension() || access.extensionWord.hasLocationOf(group.extensionWord);
        if (!access.slot.hasLocationOf(group.slot) || !extensionWordsAgree)
            fail(PlacementRule::AccessLocationDiffers);
    }
    if (!hasStorageRuleOfMembers(context, access, membersWithSlot))
        fail(PlacementRule::OutOfLineNotAllowed);
    if (!isCoverageValid(program, access, agreement, membersWithSlot))
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

// A store cannot tell whether it creates a field whose slot has no absent state.
static bool isListedByAllocationOnly(const PlacedSlot& slot)
{
    return slot.hasStorage() && !canLeaveSlot(slot.representation);
}

static bool listsFieldsThatNoStoreLists(const PlacementCheckContext& context, const PlannedLayout& planned, const PlacedAllocation& allocation)
{
    if (planned.storage != LayoutStorage::CompiledObject || !context.program.at(allocation.layout).constructor)
        return true;
    if (context.placement.shapes().shape(allocation.shape).keysAreNeverEnumerated)
        return true;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (isListedByAllocationOnly(planned.slots[field]) && !allocation.initialFields.contains(field))
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
    bool isSized = isClosure ? allocation.size >= Closure::allocationSize(allocation.numberOfCaptures) && allocation.initialFields.isEmpty() : allocation.size == (allocation.holdsInnerObjects ? planned.sizeWithInnerObjects : planned.size);
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
    if (!listsFieldsThatNoStoreLists(context, planned, allocation))
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
        AOT::sort(sorted);
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
    // Nothing checks what arrives at an expected position.
    const Type& stored = program.at(variable.declared.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : variable.declared.type);
    if (!canHold(program, placed.representation, stored) || (isStaticField && !canLeaveSlot(placed.representation)))
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
    for (auto offset : statics.moduleValueOffsets) {
        if (offset)
            add(*offset, sizeOfStaticsWord);
    }
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
        if (!variable.hasWordInStatics())
            continue;
        SlotTraits traits = traitsOf(variable.representation);
        if (traits.holdsCell && !traits.isNarrowReference)
            checkRoot(variable.offset);
        // A root is visited as a word of eight bytes, and a narrow reference is half of one.
        if (traits.isNarrowReference && std::ranges::binary_search(roots, variable.offset & ~(sizeOfStaticsWord - 1)))
            context.failures.fail(PlacementRule::RootMissing);
    }
    for (auto offset : statics.moduleValueOffsets) {
        if (offset)
            checkRoot(*offset);
    }
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

static bool isValidBitOfClass(std::span<const PlacedGuardBit> bits, size_t index)
{
    const PlacedGuardBit& bit = bits[index];
    bool isAscending = !index || keyOf(bits[index - 1]) < keyOf(bit);
    return bit.classLayout && isAscending && !!bit.key == (bit.kind == MemberGuardKind::Member || bit.kind == MemberGuardKind::NotHidden || bit.kind == MemberGuardKind::PassedByStores);
}

// Whoever clears the bit of a member finds the sets to clear under that member.
static bool isValidBitOfSet(const Program& program, const GuardBitTable& table, size_t index)
{
    const PlacedGuardBit& bit = table.bits[index];
    GuardBitID set(index);
    std::span<const GuardBitID> members = table.membersOf(set);
    if (bit.classLayout || bit.key || members.size() < 2)
        return false;
    if (std::ranges::any_of(members, [&](GuardBitID member) { return member.index() >= table.numberOfClassBits; }) || bit.holdsWhileClear != setHoldsWhileClear(program, table, members))
        return false;
    bool areAscending = std::ranges::adjacent_find(members, [](GuardBitID a, GuardBitID b) { return !(a < b); }) == members.end();
    bool isListedUnderMembers = std::ranges::all_of(members, [&](GuardBitID member) {
        return member.index() < table.numberOfClassBits && std::ranges::binary_search(table.setsContaining(member), set);
    });
    bool comesBehindPreviousSet = index == table.numberOfClassBits || std::ranges::lexicographical_compare(table.membersOf(GuardBitID(index - 1)), members);
    return areAscending && isListedUnderMembers && comesBehindPreviousSet;
}

static bool dividesIntoGroups(std::span<const uint32_t> firstOfGroups, size_t numberOfGroups, size_t numberOfEntries)
{
    return firstOfGroups.size() == numberOfGroups + 1 && !firstOfGroups.front() && firstOfGroups.back() == numberOfEntries && std::ranges::is_sorted(firstOfGroups);
}

static bool hasListsOfSets(const GuardBitTable& table)
{
    if (table.numberOfClassBits > table.bits.size() || table.membersOfSets.size() != table.setsOfClassBits.size())
        return false;
    return dividesIntoGroups(table.firstMemberOfSet.span(), table.bits.size() - table.numberOfClassBits, table.membersOfSets.size())
        && dividesIntoGroups(table.firstSetOfClassBit.span(), table.numberOfClassBits, table.setsOfClassBits.size());
}

void checkGuardBits(const PlacementCheckContext& context)
{
    const GuardBitTable& table = context.placement.guardBits();
    if (!hasListsOfSets(table)) {
        context.failures.fail(PlacementRule::GuardBitsNotDistinct);
        return;
    }
    auto bits = table.bits.span();
    Vector<uint32_t> wordOffsets;
    uint64_t usedBits = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        auto& bit = bits[i];
        // The bits of one side of a class fill words that follow each other: the run time finds them from the first. The bits of sets fill word after word.
        bool isOnSideOfPrevious = i && bits[i - 1].classLayout == bit.classLayout && bits[i - 1].side == bit.side;
        bool startsWord = !isOnSideOfPrevious || !(bit.bit % MemberGuardData::bitsPerWord);
        bool followsWordsOfSide = !bit.classLayout || !bit.bit || (isOnSideOfPrevious && bit.bit == bits[i - 1].bit + 1 && (!startsWord || bit.wordOffset == bits[i - 1].wordOffset + sizeof(uint64_t)));
        if (startsWord) {
            wordOffsets.append(bit.wordOffset);
            usedBits = 0;
        }
        bool hasOwnBit = (bit.classLayout || bit.bit < MemberGuardData::bitsPerWord) && followsWordsOfSide && !(usedBits & bit.mask());
        bool hasWordOfNeighbors = bit.wordOffset && bit.wordOffset == wordOffsets.last();
        bool hasSenseOfClass = !bit.classLayout || bit.holdsWhileClear == context.inputs.slots.classCreation(bit.classLayout).guardsHoldWhileClear;
        bool isValid = hasSenseOfClass && (i < table.numberOfClassBits ? isValidBitOfClass(bits, i) : isValidBitOfSet(context.program, table, i));
        if (!hasOwnBit || !hasWordOfNeighbors || !isValid)
            context.failures.fail(PlacementRule::GuardBitsNotDistinct, bit.classLayout);
        usedBits |= bit.mask();
    }
    AOT::sort(wordOffsets);
    if (std::ranges::adjacent_find(wordOffsets) != wordOffsets.end())
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
