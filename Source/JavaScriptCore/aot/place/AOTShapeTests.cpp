/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShapeTest.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTImageFormat.h"
#include "AOTPlacementSteps.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Hasher.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxShapeSetBits, Architecture::ARM64) <= noBitSet);
static_assert(defaultLimitValue(LimitID::MaxShapeSetBits, Architecture::X86_64) <= noBitSet);

static ShapeTest createTest(ShapeTestForm form, uint32_t first, uint32_t last)
{
    return { .form = form, .first = first, .count = last - first + 1 };
}

static ShapeTest createImmediateBitsTest(std::span<const uint32_t> shapes)
{
    ShapeTest test = createTest(ShapeTestForm::ImmediateBits, shapes.front(), shapes.back());
    for (uint32_t shape : shapes)
        test.bits |= 1ull << (shapes.back() - shape);
    return test;
}

static std::optional<ShapeTest> tryCreateBlockTest(std::span<const uint32_t> shapes, const AssignedShapes& assigned)
{
    uint32_t first = shapes.front();
    uint32_t last = shapes.back();
    uint32_t block = (last - 1) / shapesPerBlock;
    if (block >= maxTestableBlocks || shapes.size() < 2 || assigned.numberOfShapes > maxShapesForBlockTests)
        return std::nullopt;
    uint32_t blockLast = std::min((block + 1) * shapesPerBlock, assigned.numberOfShapes);
    ShapeTestForm form = ShapeTestForm::BlockOrEarlier;
    if (assigned.countInRange(1, blockLast) != shapes.size()) {
        if (first <= block * shapesPerBlock || assigned.countInRange(block * shapesPerBlock + 1, blockLast) != shapes.size())
            return std::nullopt;
        form = ShapeTestForm::Block;
    }
    ShapeTest test = createTest(form, first, last);
    test.blockByte = static_cast<uint8_t>(shapeWordOf(last) >> 8);
    return test;
}

static uint64_t numberOfPagesOf(std::span<const uint32_t> ascendingShapes)
{
    return numberOfPagesInShapeSet(ascendingShapes.back() - ascendingShapes.front() + 1);
}

// No set has more distinct pages than pages, or than members and an empty page.
bool canEncodeShapeSet(std::span<const uint32_t> ascendingShapes)
{
    uint64_t numberOfPages = numberOfPagesOf(ascendingShapes);
    uint64_t maxDistinctPages = std::min<uint64_t>(numberOfPages, ascendingShapes.size() + 1);
    return PagedBitSet::numberOfEntryWords(numberOfPages, PagedBitSet::Entries::Wide) + maxDistinctPages <= PagedBitSet::maxNumberOfWords(PagedBitSet::Entries::Wide);
}

// The same members give the same words in every build: the multipliers are tried in one order.
static std::optional<EncodedShapeSet> tryHashShapeSet(std::span<const uint32_t> ascendingShapes, size_t maxNumberOfWords)
{
    AOT_THRESHOLD(unsigned, maxNumberOfTrials, 256);
    constexpr unsigned maxLogNumberOfSlots = 10;
    size_t numberOfMembers = ascendingShapes.size();
    uint32_t last = ascendingShapes.back();
    if (numberOfMembers < 2 || last - ascendingShapes.front() > PagedBitSet::maxNumberInSlot)
        return std::nullopt;
    WTF::BitSet<1u << maxLogNumberOfSlots> taken;
    for (unsigned logNumberOfSlots = std::max(1u, WTF::fastLog2(static_cast<unsigned>(numberOfMembers))); logNumberOfSlots <= maxLogNumberOfSlots; ++logNumberOfSlots) {
        unsigned shift = 32 - logNumberOfSlots;
        if (PagedBitSet::numberOfWordsOfHashedSet(shift) > maxNumberOfWords)
            return std::nullopt;
        uint32_t multiplier = 0x9e3779b1u;
        for (unsigned trial = 0; trial < maxNumberOfTrials; ++trial, multiplier = multiplier * 0x2c1b3c6du + 0x297a2d3au) {
            ASSERT(multiplier & 1);
            taken.clearAll();
            bool isPerfect = std::ranges::none_of(ascendingShapes, [&](uint32_t shape) {
                return taken.testAndSet(PagedBitSet::slotOf(last - shape, multiplier, shift));
            });
            if (!isPerfect)
                continue;
            EncodedShapeSet result;
            result.entries = PagedBitSet::Entries::Hashed;
            result.words.fill(0, PagedBitSet::numberOfWordsOfHashedSet(shift));
            result.words[0] = PagedBitSet::headOfHashedSet(multiplier, shift);
            std::span<PagedBitSet::Slot> slots = spanReinterpretCast<PagedBitSet::Slot>(result.words.mutableSpan().subspan(1)).first(PagedBitSet::numberOfSlots(shift));
            for (uint32_t slot = 0; slot < slots.size(); ++slot) {
                uint32_t numberOfAnotherSlot = PagedBitSet::maxNumberInSlot;
                while (PagedBitSet::slotOf(numberOfAnotherSlot, multiplier, shift) == slot)
                    --numberOfAnotherSlot;
                slots[slot] = static_cast<PagedBitSet::Slot>(numberOfAnotherSlot);
            }
            for (uint32_t shape : ascendingShapes)
                slots[PagedBitSet::slotOf(last - shape, multiplier, shift)] = static_cast<PagedBitSet::Slot>(last - shape);
            return result;
        }
    }
    return std::nullopt;
}

static EncodedShapeSet encodeShapeSetAsBits(std::span<const uint32_t> ascendingShapes)
{
    Vector<uint64_t, 64> pages(FillWith { }, numberOfPagesOf(ascendingShapes), 0);
    for (uint32_t shape : ascendingShapes) {
        uint32_t distanceFromLast = ascendingShapes.back() - shape;
        pages[distanceFromLast >> PagedBitSet::logBitsPerPage] |= 1ull << distanceFromLast % PagedBitSet::bitsPerPage;
    }
    Vector<uint64_t, 64> distinctPages = pages;
    AOT::sort(distinctPages);
    distinctPages.shrink(std::ranges::unique(distinctPages).begin() - distinctPages.begin());

    using enum PagedBitSet::Entries;
    EncodedShapeSet result;
    result.entries = PagedBitSet::numberOfEntryWords(pages.size(), Narrow) + distinctPages.size() > PagedBitSet::maxNumberOfWords(Narrow) ? Wide : Narrow;
    size_t numberOfEntryWords = PagedBitSet::numberOfEntryWords(pages.size(), result.entries);
    if (pages.size() <= maxPagesInShortShapeSet || pages.size() <= numberOfEntryWords + distinctPages.size()) {
        result.entries = None;
        result.words = Vector<uint64_t>(pages.span());
        return result;
    }
    result.words.fill(0, numberOfEntryWords);
    result.words.appendVector(distinctPages);
    std::span<uint8_t> entries = asMutableByteSpan(result.words.mutableSpan().first(numberOfEntryWords));
    for (size_t page = 0; page < pages.size(); ++page) {
        size_t word = numberOfEntryWords + (std::ranges::lower_bound(distinctPages, pages[page]) - distinctPages.begin());
        if (result.entries == Wide)
            spanReinterpretCast<uint16_t>(entries)[page] = static_cast<uint16_t>(word);
        else
            entries[page] = static_cast<uint8_t>(word);
    }
    return result;
}

// The test of a hashed set is the shortest of all, so it is taken wherever it needs no more room.
EncodedShapeSet encodeShapeSet(std::span<const uint32_t> ascendingShapes)
{
    RELEASE_ASSERT(!ascendingShapes.empty() && canEncodeShapeSet(ascendingShapes));
    EncodedShapeSet bits = encodeShapeSetAsBits(ascendingShapes);
    if (std::optional<EncodedShapeSet> hashed = tryHashShapeSet(ascendingShapes, bits.words.size()))
        return WTF::move(*hashed);
    return bits;
}

ShapeTest chooseShapeTestForm(std::span<const uint32_t> shapes, const AssignedShapes& assigned)
{
    if (shapes.empty())
        return { };
    uint32_t first = shapes.front();
    uint32_t last = shapes.back();
    if (assigned.countInRange(first, last) != shapes.size()) {
        if (last - first < immediateBitsWindow)
            return createImmediateBitsTest(shapes);
        return createTest(ShapeTestForm::BitSet, first, last);
    }

    Vector<ShapeTest, 4> candidates;
    if (shapes.size() == 1)
        candidates.append(createTest(ShapeTestForm::OneShape, first, last));
    if (auto blockTest = tryCreateBlockTest(shapes, assigned))
        candidates.append(*blockTest);
    if (!assigned.countInRange(1, first - 1))
        candidates.append(createTest(ShapeTestForm::UpTo, first, last));
    candidates.append(createTest(ShapeTestForm::Range, first, last));
    return *std::ranges::min_element(candidates, { }, [](const ShapeTest& test) {
        return instructionsFor(test);
    });
}

namespace {

struct PassingShapes {
    bool areConsecutive() const { return shapes.size() == lastPosition - firstPosition + 1; }

    Vector<uint32_t, 16> shapes;
    size_t firstPosition { 0 };
    size_t lastPosition { 0 };
};

} // anonymous namespace

static PassingShapes findPassingShapes(std::span<const uint32_t> tested, std::span<const uint32_t> known)
{
    PassingShapes passing;
    size_t next = 0;
    for (size_t position = 0; position < known.size(); ++position) {
        while (next < tested.size() && tested[next] < known[position])
            ++next;
        if (next == tested.size())
            break;
        if (tested[next] != known[position])
            continue;
        if (passing.shapes.isEmpty())
            passing.firstPosition = position;
        passing.lastPosition = position;
        passing.shapes.append(known[position]);
    }
    return passing;
}

static ShapeTestForm formOfConsecutiveShapes(const PassingShapes& passing, size_t numberOfKnownShapes)
{
    if (!passing.firstPosition)
        return ShapeTestForm::UpTo;
    if (passing.lastPosition + 1 == numberOfKnownShapes)
        return ShapeTestForm::From;
    if (passing.shapes.size() == 1)
        return ShapeTestForm::OneShape;
    return ShapeTestForm::Range;
}

static ShapeTest shortenForKnownShapes(const ShapeTest& general, std::span<const uint32_t> shapes, std::span<const uint32_t> known)
{
    PassingShapes passing = findPassingShapes(shapes, known);
    if (passing.shapes.isEmpty())
        return { };
    if (passing.shapes.size() == known.size())
        return { .form = ShapeTestForm::AlwaysPasses };
    uint32_t first = passing.shapes.first();
    uint32_t last = passing.shapes.last();
    std::optional<ShapeTest> shortened;
    if (passing.areConsecutive())
        shortened = createTest(formOfConsecutiveShapes(passing, known.size()), first, last);
    else if (last - first < immediateBitsWindow)
        shortened = createImmediateBitsTest(passing.shapes.span());
    return shortened && instructionsFor(*shortened) < instructionsFor(general) ? *shortened : general;
}

ShapeTest chooseShapeTestForm(std::span<const uint32_t> shapes, std::span<const uint32_t> known, const AssignedShapes& assigned)
{
    return shortenForKnownShapes(chooseShapeTestForm(shapes, assigned), shapes, known);
}

static std::pair<LayoutSetID, LayoutSetID> shapeTestKeyOf(const ShapeTestWithinKnownSet& entry)
{
    return { entry.tested, entry.known };
}

static const ShapeTest* findTest(std::span<const ShapeTestWithinKnownSet> tests, LayoutSetID tested, LayoutSetID known)
{
    std::pair key { tested, known };
    auto found = std::ranges::lower_bound(tests, key, { }, shapeTestKeyOf);
    return found != tests.end() && shapeTestKeyOf(*found) == key ? &found->test : nullptr;
}

namespace {

class ShapeTestBuilder {
public:
    ShapeTestBuilder(const PlacementContext& context, const PlacedSites& sites, const ShapeNumbering& numbering, const ShapeVariants& variants)
        : m_context(context)
        , m_sites(sites)
        , m_numbering(numbering)
        , m_variants(variants)
    {
    }

    ShapeTable run(const BitVector& typesNeededAtRunTime, Vector<FieldAccess>& fieldAccesses)
    {
        describeShapes();
        computeShapeCountPrefixSums();
        reserveMemberShapes();
        deriveSetTests();
        placeBitSets(typesNeededAtRunTime);
        deriveKnownSetTests(fieldAccesses.span());
        fillFieldAccessTests(fieldAccesses);
        return ShapeTable(WTF::move(m_parts));
    }

private:
    struct StoredShapeSet {
        uint32_t firstWord;
        uint32_t numberOfWords;
        PagedBitSet::Entries entries;
    };

    const ParallelLoops& loops() const { return m_context.build.parallelLoops(); }
    AssignedShapes assignedShapes() const { return { m_parts.shapeCountPrefixSums.span(), m_numbering.numberOfShapes }; }
    ShapeRange objectShapesOf(LayoutID id) const { return m_context.isCompiledObject(id) ? m_numbering.shapesOfLayout[id.index()] : ShapeRange { }; }

    std::span<const uint32_t> memberShapesOf(LayoutSetID set) const
    {
        uint32_t first = m_parts.firstMemberShape[set.index()];
        return m_parts.memberShapes.subspan(first, m_parts.firstMemberShape[set.index() + 1] - first);
    }

    bool areMemberShapesComplete(LayoutSetID id) const
    {
        const LayoutSet& set = m_context.program.at(id);
        return set.isComplete && std::ranges::all_of(set.members, [&](LayoutID member) {
            return m_context.isCompiledObject(member) || m_context.program.at(member).flow.allocation == AllocationState::NeverAllocated;
        });
    }

    void describeShapes()
    {
        m_parts.shapes.grow(m_numbering.numberOfShapes + 1);
        m_parts.shapesOfLayout = m_numbering.shapesOfLayout;
        m_parts.firstEnvironmentShape = m_numbering.firstEnvironmentShape;
        m_parts.shapeOfAllocationClass.fill(0, m_context.inputs.flow.numberOfAllocationClasses());
        for (unsigned index = 0; index < m_numbering.shapesOfLayout.size(); ++index) {
            if (!m_numbering.shapesOfLayout[index].isEmpty())
                describeShapesOf(LayoutID(index), m_numbering.shapesOfLayout[index]);
        }
    }

    void describeShapesOf(LayoutID layout, ShapeRange range)
    {
        uint32_t next = range.first;
        if (m_variants.hasOrderedShape(layout))
            m_parts.shapes[next++] = PlacedShape { .layout = layout, .keysAreNeverEnumerated = false };
        if (m_variants.hasUnorderedShape(layout))
            m_parts.shapes[next++] = PlacedShape { .layout = layout, .keysAreNeverEnumerated = true };
        RELEASE_ASSERT(next == range.end());

        const FlowResults& flow = m_context.inputs.flow;
        std::array<bool, 2> hasClass { };
        for (AllocationClassID id : m_context.program.at(layout).flow.allocationClasses) {
            const AllocationClass& allocationClass = flow.at(id);
            unsigned variant = range.count == 2 && allocationClass.keys == KeyEnumeration::NeverEnumerated;
            PlacedShape& shape = m_parts.shapes[range.first + variant];
            shape.impossibleMutations = hasClass[variant] ? shape.impossibleMutations & allocationClass.impossibleMutations : allocationClass.impossibleMutations;
            hasClass[variant] = true;
            if (id.index() < m_parts.shapeOfAllocationClass.size())
                m_parts.shapeOfAllocationClass[id.index()] = range.first + variant;
        }
        // The passes ask the layout, so the run time holds every object to what the layout promises as well.
        const LayoutFlow& layoutFlow = m_context.program.at(layout).flow;
        if (layoutFlow.allocation == AllocationState::NeverAllocated)
            return;
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            m_parts.shapes[shape].impossibleMutations.add(layoutFlow.impossibleMutations);
    }

    void computeShapeCountPrefixSums()
    {
        auto& sums = m_parts.shapeCountPrefixSums;
        sums.fill(0, m_numbering.numberOfShapes + 1);
        for (ShapeRange range : m_numbering.shapesOfLayout) {
            for (uint32_t shape = range.first; shape < range.end(); ++shape)
                sums[shape] = 1;
        }
        for (uint32_t shape = 1; shape <= m_numbering.numberOfShapes; ++shape)
            sums[shape] += sums[shape - 1];
    }

    void reserveMemberShapes()
    {
        const Program& program = m_context.program;
        unsigned numberOfSets = program.types.numberOfLayoutSets();
        m_parts.firstMemberShape.fill(0, numberOfSets + 1);
        forEachIndexInParallel(loops(), numberOfSets, [&](unsigned index) {
            CheckedUint32 numberOfShapes = 0;
            for (LayoutID member : program.at(LayoutSetID(index)).members)
                numberOfShapes += objectShapesOf(member).count;
            m_parts.firstMemberShape[index + 1] = numberOfShapes.value();
        });
        CheckedUint32 numberOfShapes = 0;
        for (unsigned index = 1; index <= numberOfSets; ++index) {
            numberOfShapes += m_parts.firstMemberShape[index];
            m_parts.firstMemberShape[index] = numberOfShapes.value();
        }
        m_parts.memberShapes.grow(numberOfShapes.value());
    }

    void deriveSetTests()
    {
        const Program& program = m_context.program;
        unsigned numberOfSets = program.types.numberOfLayoutSets();
        AssignedShapes assigned = assignedShapes();
        m_parts.testOfSet.grow(numberOfSets);
        forEachIndexInParallel(loops(), numberOfSets, [&](unsigned index) {
            uint32_t first = m_parts.firstMemberShape[index];
            auto shapes = m_parts.memberShapes.mutableSubspan(first, m_parts.firstMemberShape[index + 1] - first);
            size_t next = 0;
            for (LayoutID member : program.at(LayoutSetID(index)).members) {
                ShapeRange range = objectShapesOf(member);
                for (uint32_t shape = range.first; shape < range.end(); ++shape)
                    shapes[next++] = shape;
            }
            RELEASE_ASSERT(next == shapes.size());
            AOT::sort(shapes);
            m_parts.testOfSet[index] = chooseShapeTestForm(shapes, assigned);
        });
    }

    void placeBitSets(const BitVector& typesNeededAtRunTime)
    {
        unsigned numberOfSets = m_parts.testOfSet.size();
        BitVector setsNamedByRecords(numberOfSets);
        for (size_t type : typesNeededAtRunTime) {
            if (LayoutSetID set = m_context.program.at(TypeID(type)).layouts)
                setsNamedByRecords.quickSet(set.index());
        }
        m_parts.setsWithTest.ensureSize(numberOfSets);
        Vector<uint32_t> setsToEncode;
        std::optional<uint32_t> unencodableSetSize;
        for (unsigned index = 0; index < numberOfSets; ++index) {
            const ShapeTest& test = m_parts.testOfSet[index];
            m_parts.setsWithTest.quickSet(index);
            if (!hasBits(test.form))
                continue;
            if (!canEncodeShapeSet(memberShapesOf(LayoutSetID(index)))) {
                unencodableSetSize = test.count;
                break;
            }
            if (setsNamedByRecords.quickGet(index))
                setsToEncode.append(index);
        }
        Vector<EncodedShapeSet> encodedSets(setsToEncode.size());
        forEachIndexInParallel(loops(), setsToEncode.size(), [&](unsigned position) {
            encodedSets[position] = encodeShapeSet(memberShapesOf(LayoutSetID(setsToEncode[position])));
        });
        for (size_t position = 0; position < setsToEncode.size(); ++position)
            m_parts.testOfSet[setsToEncode[position]].bitSetOffset = bitSetOffsetFor(encodedSets[position]);
        if (unencodableSetSize) {
            m_context.build.reportLimitExceeded(LimitID::MaxShapeSetBits, *unencodableSetSize);
            return;
        }
        uint64_t numberOfBits = static_cast<uint64_t>(m_parts.shapeSetWords.size()) * PagedBitSet::bitsPerPage;
        if (numberOfBits > m_context.build.limit(LimitID::MaxShapeSetBits))
            m_context.build.reportLimitExceeded(LimitID::MaxShapeSetBits, numberOfBits);
    }

    // Sets whose members lie at the same distances from one another have the same words.
    uint32_t bitSetOffsetFor(const EncodedShapeSet& encoded)
    {
        auto wordsOf = [&](const StoredShapeSet& stored) {
            return m_parts.shapeSetWords.subspan(stored.firstWord, stored.numberOfWords);
        };
        uint32_t hash = computeHash(encoded.words.span());
        auto found = m_shapeSetIndex.find(hash, [&](uint32_t position) {
            return m_shapeSets[position].entries == encoded.entries && std::ranges::equal(wordsOf(m_shapeSets[position]), encoded.words);
        });
        if (!found) {
            found = static_cast<uint32_t>(m_shapeSets.size());
            m_shapeSetIndex.add(hash, m_shapeSets.size());
            // The words become the start of the pool of constants, where compiled code finds the set at the alignment of a constant of its size.
            while (m_parts.shapeSetWords.sizeInBytes() % alignmentOfConstant(encoded.words.sizeInBytes()))
                m_parts.shapeSetWords.append(0);
            m_parts.shapeSetRanges.append({ static_cast<uint32_t>(m_parts.shapeSetWords.size()), static_cast<uint32_t>(encoded.words.size()) });
            m_shapeSets.append(StoredShapeSet { static_cast<uint32_t>(m_parts.shapeSetWords.size()), static_cast<uint32_t>(encoded.words.size()), encoded.entries });
            m_parts.shapeSetWords.appendVector(encoded.words);
        }
        return PagedBitSet::locationOf(m_shapeSets[*found].firstWord, encoded.entries);
    }

    void deriveKnownSetTests(std::span<const FieldAccess> fieldAccesses)
    {
        auto& tests = m_parts.testsWithinKnownSets;
        for (auto& use : m_sites.shapeTests) {
            if (use.tested && use.known)
                tests.append(ShapeTestWithinKnownSet { use.tested, use.known, { } });
        }
        for (auto& access : fieldAccesses) {
            if (access.coverage == FieldAccess::Coverage::SomeMembers)
                tests.append(ShapeTestWithinKnownSet { access.membersWithField, access.receivers, { } });
        }
        sortAndRemoveDuplicates(loops(), tests, [](const ShapeTestWithinKnownSet& a, const ShapeTestWithinKnownSet& b) {
            return shapeTestKeyOf(a) < shapeTestKeyOf(b);
        }, [](const ShapeTestWithinKnownSet& a, const ShapeTestWithinKnownSet& b) {
            return shapeTestKeyOf(a) == shapeTestKeyOf(b);
        });
        forEachIndexInParallel(loops(), tests.size(), [&](unsigned index) {
            ShapeTestWithinKnownSet& entry = tests[index];
            const ShapeTest& general = m_parts.testOfSet[entry.tested.index()];
            entry.test = areMemberShapesComplete(entry.known) ? shortenForKnownShapes(general, memberShapesOf(entry.tested), memberShapesOf(entry.known)) : general;
        });
    }

    void fillFieldAccessTests(Vector<FieldAccess>& fieldAccesses) const
    {
        forEachIndexInParallel(loops(), fieldAccesses.size(), [&](unsigned index) {
            FieldAccess& access = fieldAccesses[index];
            switch (access.coverage) {
            case FieldAccess::Coverage::NoMember:
                access.membersWithFieldTest = { };
                return;
            case FieldAccess::Coverage::EveryMemberHasSlot:
            case FieldAccess::Coverage::EveryMember:
                access.membersWithFieldTest = { .form = ShapeTestForm::AlwaysPasses };
                return;
            case FieldAccess::Coverage::SomeMembers: {
                const ShapeTest* test = findTest(m_parts.testsWithinKnownSets.span(), access.membersWithField, access.receivers);
                RELEASE_ASSERT(test);
                access.membersWithFieldTest = *test;
                return;
            }
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    }

    const PlacementContext& m_context;
    const PlacedSites& m_sites;
    const ShapeNumbering& m_numbering;
    const ShapeVariants& m_variants;
    Vector<StoredShapeSet> m_shapeSets;
    HashedIndex m_shapeSetIndex;
    ShapeTableParts m_parts;
};

} // anonymous namespace

ShapeTable deriveShapeTests(const PlacementContext& context, const PlacedSites& sites, const ShapeNumbering& numbering, const ShapeVariants& variants, const BitVector& typesNeededAtRunTime, Vector<FieldAccess>& fieldAccesses)
{
    return ShapeTestBuilder(context, sites, numbering, variants).run(typesNeededAtRunTime, fieldAccesses);
}

const ShapeTest& ShapeTable::testOf(LayoutSetID tested) const
{
    if (!tested || tested.index() >= m_parts.testOfSet.size())
        return m_neverPasses;
    return m_parts.testOfSet[tested.index()];
}

const ShapeTest& ShapeTable::testOf(LayoutSetID tested, LayoutSetID known) const
{
    if (!known)
        return testOf(tested);
    const ShapeTest* test = findTest(m_parts.testsWithinKnownSets.span(), tested, known);
    return test ? *test : testOf(tested);
}

std::span<const uint32_t> ShapeTable::memberShapes(LayoutSetID set) const
{
    if (!set || set.index() + 1 >= m_parts.firstMemberShape.size())
        return { };
    uint32_t first = m_parts.firstMemberShape[set.index()];
    return m_parts.memberShapes.subspan(first, m_parts.firstMemberShape[set.index() + 1] - first);
}

ShapeTestData ShapeTable::dataOf(const ShapeTest& test) const
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        return { 0, 0, noBitSet };
    case ShapeTestForm::OneShape:
    case ShapeTestForm::UpTo:
    case ShapeTestForm::Range:
    case ShapeTestForm::Block:
    case ShapeTestForm::BlockOrEarlier:
        return { test.last(), test.count, noBitSet };
    case ShapeTestForm::ImmediateBits:
    case ShapeTestForm::BitSet:
        RELEASE_ASSERT(test.bitSetOffset != noBitSet);
        return { test.last(), test.count, test.bitSetOffset };
    case ShapeTestForm::AlwaysPasses:
    case ShapeTestForm::From:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
