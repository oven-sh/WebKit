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
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Hasher.h>
#include <wtf/Int128.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

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

ShapeVariants chooseShapeVariants(const PlacementContext& context, const PlacedSites& sites)
{
    ShapeVariants variants { .ordered = BitVector(context.program.layouts.size()), .unordered = BitVector(context.program.layouts.size()) };
    for (unsigned index = 0; index < context.program.layouts.size(); ++index) {
        LayoutID id(index);
        if (context.isEnvironment(id) || id == context.closureLayout) {
            variants.ordered.quickSet(index);
            continue;
        }
        if (!context.isCompiledObject(id))
            continue;
        bool someOrdered = !context.slots().layout(id).keysAreNeverEnumerated;
        bool someUnordered = !someOrdered || (std::ranges::any_of(context.program.at(id).flow.allocationClasses, [&](AllocationClassID allocationClass) {
            return context.inputs.flow.at(allocationClass).keys == KeyEnumeration::NeverEnumerated;
        }) && std::ranges::any_of(std::ranges::equal_range(sites.allocations.span(), id, { }, &AllocationUse::layout), &AllocationUse::keysAreNeverEnumerated));
        variants.ordered.quickSet(index, someOrdered);
        variants.unordered.quickSet(index, someUnordered);
    }
    return variants;
}

ShapeTestCandidates collectShapeTestCandidates(const PlacementContext& context, const PlacedSites& sites, std::span<const FieldAccess> fieldAccesses, const ShapeVariants& variants, const BitVector& typesNeededAtRunTime)
{
    struct TestedSet {
        LayoutSetID set;
        SiteWeight weight { 0 };
        uint32_t sites { 0 };
    };
    RELEASE_ASSERT(fieldAccesses.size() == sites.fieldAccesses.size());
    Vector<TestedSet> testedSets;
    for (auto& use : sites.shapeTests) {
        if (use.tested)
            testedSets.append(TestedSet { use.tested, use.weight, use.sites });
    }
    for (size_t i = 0; i < fieldAccesses.size(); ++i) {
        if (fieldAccesses[i].coverage == FieldAccess::Coverage::SomeMembers)
            testedSets.append(TestedSet { fieldAccesses[i].membersWithField, sites.fieldAccesses[i].weight, sites.fieldAccesses[i].sites });
    }
    // The sets that the run time tests yield to every set that code tests. Where the numbers are still free a run of them saves the bit set in the tables, which is as wide as its members lie apart.
    for (size_t type : typesNeededAtRunTime) {
        if (LayoutSetID set = context.program.at(TypeID(type)).layouts)
            testedSets.append(TestedSet { set, 0, 0 });
    }
    parallelSort(context.build.parallelLoops(), testedSets.mutableSpan(), [](const TestedSet& a, const TestedSet& b) {
        return a.set < b.set;
    });

    ShapeTestCandidates result;
    auto& lists = result.lists;
    for (size_t next = 0; next < testedSets.size();) {
        TestedLayoutList list { .set = testedSets[next].set, .firstLayout = static_cast<uint32_t>(result.listedLayouts.size()) };
        for (; next < testedSets.size() && testedSets[next].set == list.set; ++next) {
            list.weight = saturatedSum(list.weight, testedSets[next].weight);
            list.sites = clampTo<uint32_t>(static_cast<uint64_t>(list.sites) + testedSets[next].sites);
        }
        for (LayoutID member : context.program.at(list.set).completeMembers()) {
            uint32_t numberOfShapes = variants.numberOfShapesOf(member);
            if (!context.isCompiledObject(member) || !numberOfShapes)
                continue;
            result.listedLayouts.append(member.index());
            ++list.numberOfLayouts;
            list.numberOfShapes += numberOfShapes;
        }
        if (list.numberOfLayouts)
            lists.append(list);
    }

    result.singleLayoutTestWeight.fill(0, context.program.layouts.size());
    for (unsigned i = 0; i < lists.size(); ++i) {
        if (lists[i].numberOfLayouts > 1) {
            result.listsByWeight.append(i);
            continue;
        }
        SiteWeight& weight = result.singleLayoutTestWeight[result.listedLayouts[lists[i].firstLayout]];
        weight = saturatedSum(weight, lists[i].weight);
    }
    AOT::sort(result.listsByWeight, [&](uint32_t a, uint32_t b) {
        return std::tuple { lists[b].weight.predicted(), lists[a].numberOfLayouts, a } < std::tuple { lists[a].weight.predicted(), lists[b].numberOfLayouts, b };
    });
    return result;
}

namespace {

class PQTree {
public:
    explicit PQTree(uint32_t numberOfLeaves)
        : m_nodes(numberOfLeaves)
    {
        RELEASE_ASSERT(numberOfLeaves);
        if (numberOfLeaves == 1)
            return;
        m_root = addNode(Kind::P);
        for (uint32_t leaf = 0; leaf < numberOfLeaves; ++leaf)
            addToP(m_root, leaf);
    }

    bool reduce(std::span<const uint32_t> leaves)
    {
        if (leaves.size() < 2)
            return true;
        uint32_t root = labelPertinentSubtree(leaves);
        if (root == none)
            return false;
        if (at(root).label == Label::Full)
            return true;
        if (at(root).kind == Kind::P)
            reducePRoot(root);
        else
            reduceQRoot(root);
        return true;
    }

    Vector<uint32_t> frontier(std::span<const uint64_t> leafKey)
    {
        Vector<uint64_t> keys(FillWith { }, m_nodes.size(), UINT64_MAX);
        Vector<uint32_t> leavesByKey(leafKey.size(), [](size_t leaf) {
            return static_cast<uint32_t>(leaf);
        });
        AOT::sort(leavesByKey, { }, [&](uint32_t leaf) {
            return leafKey[leaf];
        });
        for (uint32_t leaf : leavesByKey) {
            for (uint32_t node = leaf; node != none && keys[node] == UINT64_MAX; node = parentOf(node))
                keys[node] = leafKey[leaf];
        }
        Vector<uint32_t> result;
        Vector<uint32_t> children;
        Vector<uint32_t> stack { m_root };
        while (!stack.isEmpty()) {
            uint32_t node = stack.takeLast();
            if (at(node).kind == Kind::Leaf) {
                result.append(node);
                continue;
            }
            children.shrink(0);
            forEachChild(node, [&](uint32_t child) {
                children.append(child);
            });
            if (at(node).kind == Kind::P) {
                AOT::sort(children, [&](uint32_t a, uint32_t b) {
                    return keys[a] < keys[b];
                });
            } else if (keys[children.last()] < keys[children.first()])
                children.reverse();
            for (size_t i = children.size(); i--;)
                stack.append(children[i]);
        }
        return result;
    }

private:
    static constexpr uint32_t none = UINT32_MAX;

    enum class Kind : uint8_t { Leaf, P, Q, MergedQ };
    enum class Label : uint8_t { Empty, Partial, Full };

    struct Node {
        Kind kind { Kind::Leaf };
        Label label { Label::Empty };
        uint32_t parent { none };
        uint32_t indexInParent { 0 };
        std::array<uint32_t, 2> siblings { none, none };
        Vector<uint32_t> children;
        std::array<uint32_t, 2> endmostChildren { none, none };

        uint32_t epoch { 0 };
        uint32_t pertinentChildCount { 0 };
        uint32_t pertinentLeafCount { 0 };
        uint32_t fullChildCount { 0 };
        uint32_t firstFullChild { none };
        uint32_t nextFullSibling { none };
        uint32_t partialChildCount { 0 };
        std::array<uint32_t, 2> partialChildren { none, none };
        std::array<uint32_t, 2> endmostFullChildren { none, none };
        std::array<uint32_t, 2> fullChildSiblings { none, none };
    };

    struct Row {
        uint32_t emptyEnd { none };
        uint32_t fullEnd { none };
    };

    Node& at(uint32_t node) { return m_nodes[node]; }

    uint32_t addNode(Kind kind)
    {
        m_nodes.append(Node { });
        m_nodes.last().kind = kind;
        return m_nodes.size() - 1;
    }

    uint32_t parentOf(uint32_t node)
    {
        uint32_t parent = at(node).parent;
        if (parent == none || at(parent).kind != Kind::MergedQ)
            return parent;
        uint32_t found = parent;
        while (at(found).kind == Kind::MergedQ)
            found = at(found).parent;
        while (parent != found)
            parent = std::exchange(at(parent).parent, found);
        at(node).parent = found;
        return found;
    }

    bool has(uint32_t node, Label label) { return at(node).epoch == m_epoch && at(node).label == label; }
    bool isSiblingOf(uint32_t node, uint32_t sibling) { return at(node).siblings[0] == sibling || at(node).siblings[1] == sibling; }
    uint32_t otherSibling(uint32_t node, uint32_t sibling) { return at(node).siblings[at(node).siblings[0] == sibling]; }

    void replaceSibling(uint32_t node, uint32_t from, uint32_t to)
    {
        ASSERT(isSiblingOf(node, from));
        at(node).siblings[at(node).siblings[0] != from] = to;
    }

    void connect(uint32_t a, uint32_t b)
    {
        replaceSibling(a, none, b);
        replaceSibling(b, none, a);
    }

    void addToP(uint32_t parent, uint32_t child)
    {
        at(child).parent = parent;
        at(child).indexInParent = at(parent).children.size();
        at(parent).children.append(child);
    }

    void removeFromP(uint32_t child)
    {
        auto& children = at(at(child).parent).children;
        uint32_t last = children.takeLast();
        if (last == child)
            return;
        children[at(child).indexInParent] = last;
        at(last).indexInParent = at(child).indexInParent;
    }

    void addToQ(uint32_t q, uint32_t node)
    {
        at(node).parent = q;
        at(node).siblings = { none, none };
    }

    void forEachChild(uint32_t node, const auto& functor)
    {
        if (at(node).kind == Kind::P) {
            for (uint32_t child : at(node).children)
                functor(child);
            return;
        }
        if (at(node).kind != Kind::Q)
            return;
        uint32_t previous = none;
        for (uint32_t child = at(node).endmostChildren[0]; child != none;) {
            functor(child);
            previous = std::exchange(child, otherSibling(child, previous));
        }
    }

    void beginVisiting(uint32_t index)
    {
        Node& node = at(index);
        node.epoch = m_epoch;
        node.label = Label::Empty;
        node.pertinentChildCount = 0;
        node.pertinentLeafCount = 0;
        node.fullChildCount = 0;
        node.firstFullChild = none;
        node.partialChildCount = 0;
    }

    uint32_t labelPertinentSubtree(std::span<const uint32_t> leaves)
    {
        ++m_epoch;
        m_queue.shrink(0);
        for (uint32_t leaf : leaves) {
            beginVisiting(leaf);
            at(leaf).label = Label::Full;
            at(leaf).pertinentLeafCount = 1;
            m_queue.append(leaf);
        }
        bool hasPassedRoot = false;
        for (size_t head = 0; m_queue.size() - head + hasPassedRoot > 1;) {
            uint32_t parent = parentOf(m_queue[head++]);
            if (parent == none) {
                hasPassedRoot = true;
                continue;
            }
            if (at(parent).epoch != m_epoch) {
                beginVisiting(parent);
                m_queue.append(parent);
            }
            ++at(parent).pertinentChildCount;
        }
        m_queue.shrink(leaves.size());
        for (size_t head = 0; head < m_queue.size(); ++head) {
            uint32_t node = m_queue[head];
            bool isRoot = at(node).pertinentLeafCount == leaves.size();
            if (at(node).kind != Kind::Leaf && !label(node, isRoot))
                return none;
            if (isRoot)
                return node;
            Node& parent = at(parentOf(node));
            parent.pertinentLeafCount += at(node).pertinentLeafCount;
            if (at(node).label == Label::Full) {
                at(node).nextFullSibling = std::exchange(parent.firstFullChild, node);
                ++parent.fullChildCount;
            } else {
                if (parent.partialChildCount == 2)
                    return none;
                parent.partialChildren[parent.partialChildCount++] = node;
            }
            if (!--parent.pertinentChildCount)
                m_queue.append(at(node).parent);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool label(uint32_t index, bool isRoot)
    {
        Node& node = at(index);
        if (node.partialChildCount > (isRoot ? 2u : 1u))
            return false;
        node.label = Label::Partial;
        if (node.kind == Kind::P) {
            if (node.fullChildCount == node.children.size())
                node.label = Label::Full;
            return true;
        }
        if (!node.fullChildCount)
            return isSiblingOf(node.partialChildren[0], isRoot ? node.partialChildren[1] : none);
        uint32_t count = 1;
        unsigned numberOfEnds = 0;
        unsigned numberOfPartialSiblings = 0;
        for (unsigned side = 0; side < 2; ++side) {
            uint32_t child = node.firstFullChild;
            uint32_t next = at(child).siblings[side];
            while (next != none && has(next, Label::Full)) {
                child = std::exchange(next, otherSibling(next, child));
                ++count;
            }
            node.endmostFullChildren[side] = child;
            node.fullChildSiblings[side] = next;
            if (next == none)
                ++numberOfEnds;
            else if (has(next, Label::Partial))
                ++numberOfPartialSiblings;
        }
        if (count != node.fullChildCount || numberOfPartialSiblings != node.partialChildCount)
            return false;
        if (numberOfEnds == 2)
            node.label = Label::Full;
        return isRoot || numberOfEnds;
    }

    uint32_t takeFullChildren(uint32_t node)
    {
        uint32_t first = at(node).firstFullChild;
        if (at(node).fullChildCount < 2) {
            if (first != none)
                removeFromP(first);
            return first;
        }
        uint32_t group = addNode(Kind::P);
        for (uint32_t child = first; child != none; child = at(child).nextFullSibling) {
            removeFromP(child);
            addToP(group, child);
        }
        return group;
    }

    Row flatten(uint32_t node, uint32_t q)
    {
        Row outer;
        Row inner;
        auto extend = [&](uint32_t Row::* end, uint32_t first, uint32_t last) {
            if (inner.*end == none)
                outer.*end = first;
            else
                connect(inner.*end, first);
            inner.*end = last;
        };
        while (node != none) {
            uint32_t partialChild = at(node).partialChildCount ? at(node).partialChildren[0] : none;
            if (at(node).kind == Kind::P) {
                uint32_t full = takeFullChildren(node);
                if (partialChild != none)
                    removeFromP(partialChild);
                uint32_t empty = node;
                if (at(node).children.size() < 2)
                    empty = at(node).children.isEmpty() ? none : at(node).children[0];
                if (empty != none) {
                    addToQ(q, empty);
                    extend(&Row::emptyEnd, empty, empty);
                }
                if (full != none) {
                    addToQ(q, full);
                    extend(&Row::fullEnd, full, full);
                }
            } else {
                uint32_t firstPertinentChild = partialChild;
                uint32_t lastFullChild = none;
                uint32_t firstEmptyChild = none;
                if (at(node).fullChildCount) {
                    unsigned end = at(node).fullChildSiblings[0] != none;
                    firstPertinentChild = at(node).endmostFullChildren[end];
                    lastFullChild = at(node).endmostFullChildren[!end];
                    firstEmptyChild = at(node).fullChildSiblings[!end];
                    replaceSibling(lastFullChild, firstEmptyChild, none);
                    extend(&Row::fullEnd, firstPertinentChild, lastFullChild);
                }
                if (partialChild != none)
                    firstEmptyChild = otherSibling(partialChild, lastFullChild);
                if (firstEmptyChild != none) {
                    replaceSibling(firstEmptyChild, partialChild != none ? partialChild : lastFullChild, none);
                    extend(&Row::emptyEnd, at(node).endmostChildren[at(node).endmostChildren[0] == firstPertinentChild], firstEmptyChild);
                }
                at(node).kind = Kind::MergedQ;
                at(node).parent = q;
            }
            node = partialChild;
        }
        connect(inner.emptyEnd, inner.fullEnd);
        return outer;
    }

    void replace(uint32_t node, uint32_t replacement)
    {
        uint32_t parent = parentOf(node);
        at(replacement).parent = parent;
        if (parent == none) {
            m_root = replacement;
            return;
        }
        if (at(parent).kind == Kind::P) {
            at(replacement).indexInParent = at(node).indexInParent;
            at(parent).children[at(node).indexInParent] = replacement;
            return;
        }
        at(replacement).siblings = at(node).siblings;
        for (uint32_t sibling : at(node).siblings) {
            if (sibling != none)
                replaceSibling(sibling, node, replacement);
        }
        for (uint32_t& child : at(parent).endmostChildren) {
            if (child == node)
                child = replacement;
        }
    }

    void reducePRoot(uint32_t root)
    {
        unsigned partialChildCount = at(root).partialChildCount;
        auto partialChildren = at(root).partialChildren;
        uint32_t full = takeFullChildren(root);
        if (!partialChildCount) {
            addToP(root, full);
            return;
        }
        uint32_t q = addNode(Kind::Q);
        removeFromP(partialChildren[0]);
        Row row = flatten(partialChildren[0], q);
        if (full != none) {
            addToQ(q, full);
            connect(row.fullEnd, full);
            row.fullEnd = full;
        }
        if (partialChildCount == 2) {
            removeFromP(partialChildren[1]);
            Row other = flatten(partialChildren[1], q);
            connect(row.fullEnd, other.fullEnd);
            row.fullEnd = other.emptyEnd;
        }
        at(q).endmostChildren = { row.emptyEnd, row.fullEnd };
        if (at(root).children.isEmpty())
            replace(root, q);
        else
            addToP(root, q);
    }

    void reduceQRoot(uint32_t root)
    {
        uint32_t pertinentSibling = at(root).partialChildren[1];
        for (unsigned i = 0; i < at(root).partialChildCount; ++i) {
            uint32_t child = at(root).partialChildren[i];
            if (at(root).fullChildCount)
                pertinentSibling = at(root).endmostFullChildren[at(root).fullChildSiblings[0] != child];
            uint32_t emptySibling = otherSibling(child, pertinentSibling);
            replaceSibling(pertinentSibling, child, none);
            if (emptySibling != none)
                replaceSibling(emptySibling, child, none);
            Row row = flatten(child, root);
            connect(pertinentSibling, row.fullEnd);
            if (emptySibling != none)
                connect(emptySibling, row.emptyEnd);
            else
                at(root).endmostChildren[at(root).endmostChildren[0] != child] = row.emptyEnd;
            pertinentSibling = row.fullEnd;
        }
    }

    Vector<Node> m_nodes;
    Vector<uint32_t> m_queue;
    uint32_t m_root { 0 };
    uint32_t m_epoch { 0 };
};

inline constexpr uint64_t bytesSavedPerBlockTest = 6;
inline constexpr uint32_t numberOfShapesWithShortConstants = 128;

constexpr uint32_t blockStartAtOrAfter(uint32_t shape)
{
    return static_cast<uint32_t>(WTF::roundUpToMultipleOf<shapesPerBlock>(shape - 1) + 1);
}

class ShapeNumberer {
public:
    ShapeNumberer(const PlacementContext& context, const ShapeTestCandidates& candidates, const ShapeVariants& variants)
        : m_context(context)
        , m_candidates(candidates)
        , m_variants(variants)
        , m_numberOfLayouts(context.program.layouts.size())
    {
    }

    ShapeNumbering run()
    {
        m_result.shapesOfLayout.fill(ShapeRange { }, m_numberOfLayouts);
        assignLeaves();
        if (!m_leafLayout.isEmpty()) {
            groupFrontier(computeFrontier());
            chooseNumbering();
        }
        numberRemainingLayouts();
        if (m_context.build.exceeds(LimitID::MaxShapes, m_result.numberOfShapes))
            m_context.build.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyShapes, .limit = LimitID::MaxShapes, .count = m_result.numberOfShapes });
        return WTF::move(m_result);
    }

private:
    struct Group {
        uint32_t first;
        uint32_t count { 0 };
        SiteWeight weight { 0 };
    };

    struct Blocks {
        BitVector begins;
        BitVector ends;
    };

    struct SequenceNumbering {
        Vector<uint32_t> firstShapeOfLayout;
        uint32_t nextShape { 1 };
        SiteWeight estimatedInstructions { 0 };
    };

    uint32_t numberOfShapesOf(uint32_t layout) const
    {
        uint32_t numberOfShapes = m_variants.numberOfShapesOf(LayoutID(layout));
        RELEASE_ASSERT(numberOfShapes);
        return numberOfShapes;
    }

    Vector<uint32_t> positionsIn(const Vector<uint32_t>& sequence) const
    {
        Vector<uint32_t> positionOf(FillWith { }, m_numberOfLayouts, 0);
        for (uint32_t position = 0; position < sequence.size(); ++position)
            positionOf[sequence[position]] = position;
        return positionOf;
    }

    std::pair<uint32_t, uint32_t> extentOf(const TestedLayoutList& list, const Vector<uint32_t>& positionOf) const
    {
        uint32_t first = std::numeric_limits<uint32_t>::max();
        uint32_t last = 0;
        for (uint32_t layout : m_candidates.layoutsOf(list)) {
            first = std::min(first, positionOf[layout]);
            last = std::max(last, positionOf[layout]);
        }
        return std::pair { first, last };
    }

    void assignLeaves()
    {
        m_leafOfLayout.fill(std::nullopt, m_numberOfLayouts);
        for (uint32_t index : m_candidates.listsByWeight) {
            for (uint32_t layout : m_candidates.layoutsOf(m_candidates.lists[index])) {
                if (m_leafOfLayout[layout])
                    continue;
                m_leafOfLayout[layout] = static_cast<uint32_t>(m_leafLayout.size());
                m_leafLayout.append(layout);
            }
        }
        for (unsigned i = 0; i < m_numberOfLayouts; ++i) {
            if (m_context.isCompiledObject(LayoutID(i)) && !m_leafOfLayout[i])
                m_layoutsTestedAlone.append(i);
        }
        AOT::stableSort(m_layoutsTestedAlone, [&](uint32_t a, uint32_t b) {
            return (m_candidates.singleLayoutTestWeight[a] > m_candidates.singleLayoutTestWeight[b]).predicted();
        });
    }

    Vector<uint32_t> computeFrontier() const
    {
        PQTree tree(m_leafLayout.size());
        Vector<uint64_t> leafKey(m_leafLayout.size(), [](size_t leaf) {
            return static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) << 32 | leaf;
        });
        uint64_t rejectedCount = 0;
        Vector<uint32_t> leaves;
        for (uint32_t index : m_candidates.listsByWeight) {
            leaves.shrink(0);
            for (uint32_t layout : m_candidates.layoutsOf(m_candidates.lists[index]))
                leaves.append(*m_leafOfLayout[layout]);
            if (tree.reduce(leaves.span()))
                continue;
            for (uint32_t leaf : leaves)
                leafKey[leaf] = std::min(leafKey[leaf], rejectedCount << 32 | leaf);
            ++rejectedCount;
        }
        Vector<uint32_t> frontier;
        for (uint32_t leaf : tree.frontier(leafKey.span()))
            frontier.append(m_leafLayout[leaf]);
        return frontier;
    }

    void groupFrontier(Vector<uint32_t>&& frontier)
    {
        Vector<uint32_t> positionOf = positionsIn(frontier);
        Vector<int32_t> numberOfListsSpanning(FillWith { }, frontier.size() + 1, 0);
        Vector<SiteWeight> weightOfListsBeginningAt(FillWith { }, frontier.size(), 0);
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            // A list without weight ties no groups together: it would move tested shapes behind untested ones.
            if (!list.weight.predicted())
                continue;
            auto [first, last] = extentOf(list, positionOf);
            ++numberOfListsSpanning[first + 1];
            --numberOfListsSpanning[last + 1];
            weightOfListsBeginningAt[first] = saturatedSum(weightOfListsBeginningAt[first], list.weight);
        }
        Vector<Group> groups;
        int32_t spanning = 0;
        for (uint32_t position = 0; position < frontier.size(); ++position) {
            spanning += numberOfListsSpanning[position];
            if (!spanning)
                groups.append(Group { position });
            auto& group = groups.last();
            ++group.count;
            group.weight = saturatedSum(group.weight, saturatedSum(weightOfListsBeginningAt[position], m_candidates.singleLayoutTestWeight[frontier[position]]));
        }
        while (m_numberOfLayoutsTestedAloneInFrontier < m_layoutsTestedAlone.size() && m_candidates.singleLayoutTestWeight[m_layoutsTestedAlone[m_numberOfLayoutsTestedAloneInFrontier]].predicted()) {
            uint32_t layout = m_layoutsTestedAlone[m_numberOfLayoutsTestedAloneInFrontier++];
            groups.append(Group { static_cast<uint32_t>(frontier.size()), 1, m_candidates.singleLayoutTestWeight[layout] });
            frontier.append(layout);
        }
        AOT::stableSort(groups, [](const Group& a, const Group& b) {
            return static_cast<UInt128>(a.weight.predicted()) * static_cast<UInt128>(b.count) > static_cast<UInt128>(b.weight.predicted()) * static_cast<UInt128>(a.count);
        });
        m_groupOf.fill(0, m_numberOfLayouts);
        for (uint32_t index = 0; index < groups.size(); ++index) {
            for (uint32_t layout : frontier.subspan(groups[index].first, groups[index].count)) {
                m_groupOf[layout] = index;
                m_frontierByTestsPerShape.append(layout);
            }
        }
    }

    Blocks chooseBlocks(const Vector<uint32_t>& sequence, uint32_t headLength) const
    {
        Vector<uint32_t> positionOf = positionsIn(sequence);
        BitVector isInBlock(sequence.size());
        Blocks blocks { BitVector(sequence.size()), BitVector(sequence.size()) };
        Vector<int32_t> numberOfBitSetsSpanning(FillWith { }, sequence.size() + 2, 0);
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            auto [first, last] = extentOf(list, positionOf);
            if (last - first + 1 == list.numberOfLayouts)
                continue;
            ++numberOfBitSetsSpanning[first + 1];
            --numberOfBitSetsSpanning[last + 1];
        }
        for (uint32_t position = 1; position < numberOfBitSetsSpanning.size(); ++position)
            numberOfBitSetsSpanning[position] += numberOfBitSetsSpanning[position - 1];
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            if (list.numberOfShapes > shapesPerBlock)
                continue;
            auto [first, last] = extentOf(list, positionOf);
            if (last - first + 1 != list.numberOfLayouts || first < headLength)
                continue;
            bool isFree = true;
            for (uint32_t position = first; position <= last; ++position)
                isFree &= !isInBlock.quickGet(position);
            uint64_t unusedNumbersBefore = shapesPerBlock / 2;
            uint64_t unusedNumbersAfter = shapesPerBlock - list.numberOfShapes;
            uint64_t dataBytes = (unusedNumbersBefore + unusedNumbersAfter) * sizeof(uint32_t) + (unusedNumbersBefore * numberOfBitSetsSpanning[first] + unusedNumbersAfter * numberOfBitSetsSpanning[last + 1]) / 8;
            if (!isFree || static_cast<uint64_t>(list.sites) * bytesSavedPerBlockTest < dataBytes)
                continue;
            for (uint32_t position = first; position <= last; ++position)
                isInBlock.quickSet(position);
            blocks.begins.quickSet(first);
            blocks.ends.quickSet(last);
        }
        return blocks;
    }

    SequenceNumbering assignShapes(const Vector<uint32_t>& sequence, uint32_t headLength, const Blocks& blocks) const
    {
        SequenceNumbering result;
        result.firstShapeOfLayout.fill(0, m_numberOfLayouts);
        bool isAligned = false;
        for (uint32_t position = 0; position < sequence.size(); ++position) {
            if (headLength && position == headLength)
                result.nextShape = blockStartAtOrAfter(result.nextShape);
            if (blocks.begins.quickGet(position) && result.nextShape > numberOfShapesWithShortConstants && blockStartAtOrAfter(result.nextShape) / shapesPerBlock < maxTestableBlocks) {
                result.nextShape = blockStartAtOrAfter(result.nextShape);
                isAligned = true;
            }
            result.firstShapeOfLayout[sequence[position]] = result.nextShape;
            result.nextShape += numberOfShapesOf(sequence[position]);
            if (blocks.ends.quickGet(position) && std::exchange(isAligned, false))
                result.nextShape = blockStartAtOrAfter(result.nextShape);
        }
        if (headLength == sequence.size())
            result.nextShape = blockStartAtOrAfter(result.nextShape);
        return result;
    }

    SiteWeight estimateInstructions(const Vector<uint32_t>& sequence, const SequenceNumbering& numbering) const
    {
        Vector<uint32_t> countPrefixSums(FillWith { }, numbering.nextShape + m_numberOfShapesAfterFrontier, 0);
        for (uint32_t layout : sequence)
            std::ranges::fill(countPrefixSums.mutableSubspan(numbering.firstShapeOfLayout[layout], numberOfShapesOf(layout)), 1);
        std::ranges::fill(countPrefixSums.mutableSubspan(numbering.nextShape), 1);
        for (uint32_t shape = 1; shape < countPrefixSums.size(); ++shape)
            countPrefixSums[shape] += countPrefixSums[shape - 1];
        AssignedShapes assigned { countPrefixSums.span(), static_cast<uint32_t>(countPrefixSums.size() - 1) };
        SiteWeight estimatedInstructions = 0;
        Vector<uint32_t> shapes;
        auto addTest = [&](std::span<const uint32_t> layouts, SiteWeight weight) {
            shapes.shrink(0);
            for (uint32_t layout : layouts) {
                uint32_t first = numbering.firstShapeOfLayout[layout];
                for (uint32_t shape = first; shape < first + numberOfShapesOf(layout); ++shape)
                    shapes.append(shape);
            }
            AOT::sort(shapes);
            estimatedInstructions = saturatedSum(estimatedInstructions, saturatedProduct(weight, instructionsFor(chooseShapeTestForm(shapes.span(), assigned))));
        };
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            addTest(m_candidates.layoutsOf(list), list.weight);
        }
        for (const uint32_t& layout : sequence) {
            if (SiteWeight weight = m_candidates.singleLayoutTestWeight[layout]; weight.predicted())
                addTest(singleElementSpan(layout), weight);
        }
        return estimatedInstructions;
    }

    SequenceNumbering numberSequence(const Vector<uint32_t>& sequence, uint32_t headLength, bool usesBlocks) const
    {
        RELEASE_ASSERT(sequence.size() == m_frontierByTestsPerShape.size());
        Blocks blocks = usesBlocks ? chooseBlocks(sequence, headLength) : Blocks { BitVector(sequence.size()), BitVector(sequence.size()) };
        SequenceNumbering result = assignShapes(sequence, headLength, blocks);
        result.estimatedInstructions = estimateInstructions(sequence, result);
        return result;
    }

    std::optional<SequenceNumbering> numberSequenceWithHead(const Vector<uint32_t>& frontier) const
    {
        Vector<uint32_t> positionOf = positionsIn(frontier);
        for (uint32_t index : m_candidates.listsByWeight) {
            auto& list = m_candidates.lists[index];
            auto [first, last] = extentOf(list, positionOf);
            uint32_t length = list.numberOfLayouts;
            // A list without weight may lie in several groups.
            if (!list.weight.predicted() || last - first + 1 != length || list.numberOfShapes > shapesPerBlock * maxTestableBlocks / 2)
                continue;
            if (static_cast<uint64_t>(list.sites) * bytesSavedPerBlockTest < (WTF::roundUpToMultipleOf<shapesPerBlock>(list.numberOfShapes) - list.numberOfShapes) * sizeof(uint32_t))
                continue;
            uint32_t group = m_groupOf[frontier[first]];
            uint32_t groupBegin = first;
            while (groupBegin && m_groupOf[frontier[groupBegin - 1]] == group)
                --groupBegin;
            uint32_t groupEnd = last + 1;
            while (groupEnd < frontier.size() && m_groupOf[frontier[groupEnd]] == group)
                ++groupEnd;
            Vector<uint32_t> before(frontier.subspan(groupBegin, first - groupBegin));
            std::ranges::reverse(before);
            Vector<uint32_t> sequence(frontier.subspan(first, length));
            sequence.append(frontier.subspan(last + 1, groupEnd - last - 1));
            sequence.appendVector(before);
            for (uint32_t layout : m_frontierByTestsPerShape) {
                if (m_groupOf[layout] != group)
                    sequence.append(layout);
            }
            return numberSequence(sequence, length, true);
        }
        return std::nullopt;
    }

    void chooseNumbering()
    {
        m_numberOfShapesAfterFrontier = static_cast<uint32_t>(m_variants.ordered.bitCount() + m_variants.unordered.bitCount());
        for (uint32_t layout : m_frontierByTestsPerShape)
            m_numberOfShapesAfterFrontier -= numberOfShapesOf(layout);
        Vector<uint32_t> reversedFrontier = m_frontierByTestsPerShape;
        reversedFrontier.reverse();
        std::array<std::optional<SequenceNumbering>, 4> numberings;
        runTasksInParallel(m_context.build.parallelLoops(), {
            [&] { numberings[0] = numberSequence(m_frontierByTestsPerShape, 0, false); },
            [&] { numberings[1] = numberSequence(m_frontierByTestsPerShape, 0, true); },
            [&] { numberings[2] = numberSequenceWithHead(m_frontierByTestsPerShape); },
            [&] { numberings[3] = numberSequenceWithHead(reversedFrontier); },
        });
        SequenceNumbering best = WTF::move(*numberings[0]);
        for (std::optional<SequenceNumbering>& numbering : std::span(numberings).subspan(1)) {
            if (numbering && (numbering->estimatedInstructions < best.estimatedInstructions).predicted())
                best = WTF::move(*numbering);
        }
        for (uint32_t layout : m_frontierByTestsPerShape)
            m_result.shapesOfLayout[layout] = ShapeRange { best.firstShapeOfLayout[layout], numberOfShapesOf(layout) };
        m_nextShape = best.nextShape;
    }

    void numberRemainingLayouts()
    {
        auto number = [&](uint32_t layout) {
            m_result.shapesOfLayout[layout] = ShapeRange { m_nextShape, numberOfShapesOf(layout) };
            m_nextShape += numberOfShapesOf(layout);
        };
        for (uint32_t layout : m_layoutsTestedAlone.span().subspan(m_numberOfLayoutsTestedAloneInFrontier))
            number(layout);
        m_result.firstEnvironmentShape = m_nextShape;
        for (unsigned i = 0; i < m_numberOfLayouts; ++i) {
            if (m_context.isEnvironment(LayoutID(i)))
                number(i);
        }
        number(m_context.closureLayout.index());
        m_result.numberOfShapes = m_nextShape - 1;
    }

    const PlacementContext& m_context;
    const ShapeTestCandidates& m_candidates;
    const ShapeVariants& m_variants;
    unsigned m_numberOfLayouts;
    Vector<std::optional<uint32_t>> m_leafOfLayout;
    Vector<uint32_t> m_leafLayout;
    Vector<uint32_t> m_layoutsTestedAlone;
    size_t m_numberOfLayoutsTestedAloneInFrontier { 0 };
    Vector<uint32_t> m_frontierByTestsPerShape;
    Vector<uint32_t> m_groupOf;
    uint32_t m_numberOfShapesAfterFrontier { 0 };
    uint32_t m_nextShape { 1 };
    ShapeNumbering m_result;
};

} // anonymous namespace

ShapeNumbering numberShapes(const PlacementContext& context, const ShapeTestCandidates& candidates, const ShapeVariants& variants)
{
    return ShapeNumberer(context, candidates, variants).run();
}

static ShapeTest shortenForKnownShapes(const ShapeTest& general, std::span<const uint32_t> tested, std::span<const uint32_t> known)
{
    Vector<uint32_t, 16> passing;
    size_t firstPosition = 0;
    size_t lastPosition = 0;
    size_t next = 0;
    for (size_t position = 0; position < known.size(); ++position) {
        while (next < tested.size() && tested[next] < known[position])
            ++next;
        if (next == tested.size())
            break;
        if (tested[next] != known[position])
            continue;
        if (passing.isEmpty())
            firstPosition = position;
        lastPosition = position;
        passing.append(known[position]);
    }
    if (passing.isEmpty())
        return { };
    if (passing.size() == known.size())
        return { .form = ShapeTestForm::AlwaysPasses };

    std::optional<ShapeTest> shortened;
    if (passing.size() == lastPosition - firstPosition + 1) {
        ShapeTestForm form = !firstPosition ? ShapeTestForm::UpTo : lastPosition + 1 == known.size() ? ShapeTestForm::From : passing.size() == 1 ? ShapeTestForm::OneShape : ShapeTestForm::Range;
        shortened = createTest(form, passing.first(), passing.last());
    } else if (passing.last() - passing.first() < immediateBitsWindow)
        shortened = createImmediateBitsTest(passing.span());
    return shortened && instructionsFor(*shortened) < instructionsFor(general) ? *shortened : general;
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

static std::span<const uint32_t> memberShapesOf(const ShapeTableParts& parts, LayoutSetID set)
{
    uint32_t first = parts.firstMemberShape[set.index()];
    return parts.memberShapes.subspan(first, parts.firstMemberShape[set.index() + 1] - first);
}

static void describeShapes(const PlacementContext& context, const ShapeVariants& variants, LayoutID layout, ShapeRange range, ShapeTableParts& parts)
{
    uint32_t next = range.first;
    if (variants.hasOrderedShape(layout))
        parts.shapes[next++] = PlacedShape { .layout = layout, .keysAreNeverEnumerated = false };
    if (variants.hasUnorderedShape(layout))
        parts.shapes[next++] = PlacedShape { .layout = layout, .keysAreNeverEnumerated = true };
    RELEASE_ASSERT(next == range.end());

    const LayoutFlow& layoutFlow = context.program.at(layout).flow;
    std::array<bool, 2> hasClass { };
    for (AllocationClassID id : layoutFlow.allocationClasses) {
        const AllocationClass& allocationClass = context.inputs.flow.at(id);
        unsigned variant = range.count == 2 && allocationClass.keys == KeyEnumeration::NeverEnumerated;
        PlacedShape& shape = parts.shapes[range.first + variant];
        shape.impossibleMutations = hasClass[variant] ? shape.impossibleMutations & allocationClass.impossibleMutations : allocationClass.impossibleMutations;
        hasClass[variant] = true;
        if (id.index() < parts.shapeOfAllocationClass.size())
            parts.shapeOfAllocationClass[id.index()] = range.first + variant;
    }
    for (uint32_t shape = range.first; shape < range.end(); ++shape) {
        parts.shapeCountPrefixSums[shape] = 1;
        // The passes ask the layout, so the run time holds every object to what the layout promises as well.
        if (layoutFlow.allocation != AllocationState::NeverAllocated)
            parts.shapes[shape].impossibleMutations.add(layoutFlow.impossibleMutations);
    }
}

static void deriveSetTests(const PlacementContext& context, const ShapeNumbering& numbering, ShapeTableParts& parts)
{
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned numberOfSets = context.program.types.numberOfLayoutSets();
    auto objectShapesOf = [&](LayoutID id) {
        return context.isCompiledObject(id) ? numbering.shapesOfLayout[id.index()] : ShapeRange { };
    };
    parts.firstMemberShape.fill(0, numberOfSets + 1);
    forEachIndexInParallel(loops, numberOfSets, [&](unsigned index) {
        CheckedUint32 numberOfShapes = 0;
        for (LayoutID member : context.program.at(LayoutSetID(index)).written())
            numberOfShapes += objectShapesOf(member).count;
        parts.firstMemberShape[index + 1] = numberOfShapes.value();
    });
    CheckedUint32 numberOfShapes = 0;
    for (unsigned index = 1; index <= numberOfSets; ++index) {
        numberOfShapes += parts.firstMemberShape[index];
        parts.firstMemberShape[index] = numberOfShapes.value();
    }
    parts.memberShapes.grow(numberOfShapes.value());

    AssignedShapes assigned { parts.shapeCountPrefixSums.span(), numbering.numberOfShapes };
    parts.testOfSet.grow(numberOfSets);
    forEachIndexInParallel(loops, numberOfSets, [&](unsigned index) {
        uint32_t first = parts.firstMemberShape[index];
        auto shapes = parts.memberShapes.mutableSubspan(first, parts.firstMemberShape[index + 1] - first);
        size_t next = 0;
        for (LayoutID member : context.program.at(LayoutSetID(index)).written()) {
            ShapeRange range = objectShapesOf(member);
            for (uint32_t shape = range.first; shape < range.end(); ++shape)
                shapes[next++] = shape;
        }
        RELEASE_ASSERT(next == shapes.size());
        AOT::sort(shapes);
        parts.testOfSet[index] = chooseShapeTestForm(shapes, assigned);
    });
}

static void placeBitSets(const PlacementContext& context, const BitVector& typesNeededAtRunTime, ShapeTableParts& parts)
{
    unsigned numberOfSets = parts.testOfSet.size();
    BitVector setsNamedByRecords(numberOfSets);
    for (size_t type : typesNeededAtRunTime) {
        if (LayoutSetID set = context.program.at(TypeID(type)).layouts)
            setsNamedByRecords.quickSet(set.index());
    }
    parts.setsWithTest.ensureSize(numberOfSets);
    Vector<uint32_t> setsToEncode;
    std::optional<uint32_t> unencodableSetSize;
    for (unsigned index = 0; index < numberOfSets; ++index) {
        const ShapeTest& test = parts.testOfSet[index];
        parts.setsWithTest.quickSet(index);
        if (!hasBits(test.form))
            continue;
        if (!canEncodeShapeSet(memberShapesOf(parts, LayoutSetID(index)))) {
            unencodableSetSize = test.count;
            break;
        }
        if (setsNamedByRecords.quickGet(index))
            setsToEncode.append(index);
    }
    Vector<EncodedShapeSet> encodedSets(setsToEncode.size());
    forEachIndexInParallel(context.build.parallelLoops(), setsToEncode.size(), [&](unsigned position) {
        encodedSets[position] = encodeShapeSet(memberShapesOf(parts, LayoutSetID(setsToEncode[position])));
    });

    Vector<PagedBitSet::Entries> entriesOfStoredSets;
    HashedIndex storedSets;
    for (size_t position = 0; position < setsToEncode.size(); ++position) {
        const EncodedShapeSet& encoded = encodedSets[position];
        uint32_t hash = computeHash(encoded.words.span());
        auto found = storedSets.find(hash, [&](uint32_t stored) {
            return entriesOfStoredSets[stored] == encoded.entries && std::ranges::equal(parts.shapeSetWords.subspan(parts.shapeSetRanges[stored].firstWord, parts.shapeSetRanges[stored].numberOfWords), encoded.words);
        });
        if (!found) {
            found = static_cast<uint32_t>(entriesOfStoredSets.size());
            storedSets.add(hash, *found);
            // The words become the start of the pool of constants, where compiled code finds the set at the alignment of a constant of its size.
            while (parts.shapeSetWords.sizeInBytes() % alignmentOfConstant(encoded.words.sizeInBytes()))
                parts.shapeSetWords.append(0);
            parts.shapeSetRanges.append({ static_cast<uint32_t>(parts.shapeSetWords.size()), static_cast<uint32_t>(encoded.words.size()) });
            entriesOfStoredSets.append(encoded.entries);
            parts.shapeSetWords.appendVector(encoded.words);
        }
        parts.testOfSet[setsToEncode[position]].bitSetOffset = PagedBitSet::locationOf(parts.shapeSetRanges[*found].firstWord, encoded.entries);
    }
    uint64_t numberOfBits = unencodableSetSize ? *unencodableSetSize : static_cast<uint64_t>(parts.shapeSetWords.size()) * PagedBitSet::bitsPerPage;
    if (unencodableSetSize || numberOfBits > context.build.limit(LimitID::MaxShapeSetBits))
        context.build.reportLimitExceeded(LimitID::MaxShapeSetBits, numberOfBits);
}

static void deriveKnownSetTests(const PlacementContext& context, const PlacedSites& sites, Vector<FieldAccess>& fieldAccesses, ShapeTableParts& parts)
{
    const ParallelLoops& loops = context.build.parallelLoops();
    auto& tests = parts.testsWithinKnownSets;
    for (auto& use : sites.shapeTests) {
        if (use.tested && use.known)
            tests.append(ShapeTestWithinKnownSet { use.tested, use.known, { } });
    }
    for (auto& access : fieldAccesses) {
        if (access.coverage == FieldAccess::Coverage::SomeMembers)
            tests.append(ShapeTestWithinKnownSet { access.membersWithField, access.receivers, { } });
    }
    sortAndRemoveDuplicates(loops, tests, [](const ShapeTestWithinKnownSet& a, const ShapeTestWithinKnownSet& b) {
        return shapeTestKeyOf(a) < shapeTestKeyOf(b);
    }, [](const ShapeTestWithinKnownSet& a, const ShapeTestWithinKnownSet& b) {
        return shapeTestKeyOf(a) == shapeTestKeyOf(b);
    });
    forEachIndexInParallel(loops, tests.size(), [&](unsigned index) {
        ShapeTestWithinKnownSet& entry = tests[index];
        auto known = context.program.at(entry.known).all();
        bool areMemberShapesComplete = known && std::ranges::all_of(*known, [&](LayoutID member) {
            return context.holdsCompiledObjects(member) || context.program.at(member).flow.allocation == AllocationState::NeverAllocated;
        });
        const ShapeTest& general = parts.testOfSet[entry.tested.index()];
        entry.test = areMemberShapesComplete ? shortenForKnownShapes(general, memberShapesOf(parts, entry.tested), memberShapesOf(parts, entry.known)) : general;
    });
    forEachIndexInParallel(loops, fieldAccesses.size(), [&](unsigned index) {
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
            const ShapeTest* test = findTest(tests.span(), access.membersWithField, access.receivers);
            RELEASE_ASSERT(test);
            access.membersWithFieldTest = *test;
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

ShapeTable deriveShapeTests(const PlacementContext& context, const PlacedSites& sites, const ShapeNumbering& numbering, const ShapeVariants& variants, const BitVector& typesNeededAtRunTime, Vector<FieldAccess>& fieldAccesses)
{
    ShapeTableParts parts;
    parts.shapes.grow(numbering.numberOfShapes + 1);
    parts.shapesOfLayout = numbering.shapesOfLayout;
    parts.firstEnvironmentShape = numbering.firstEnvironmentShape;
    parts.shapeOfAllocationClass.fill(0, context.inputs.flow.numberOfAllocationClasses());
    parts.shapeCountPrefixSums.fill(0, numbering.numberOfShapes + 1);
    for (unsigned index = 0; index < numbering.shapesOfLayout.size(); ++index) {
        if (!numbering.shapesOfLayout[index].isEmpty())
            describeShapes(context, variants, LayoutID(index), numbering.shapesOfLayout[index], parts);
    }
    for (uint32_t shape = 1; shape <= numbering.numberOfShapes; ++shape)
        parts.shapeCountPrefixSums[shape] += parts.shapeCountPrefixSums[shape - 1];
    deriveSetTests(context, numbering, parts);
    placeBitSets(context, typesNeededAtRunTime, parts);
    deriveKnownSetTests(context, sites, fieldAccesses, parts);
    return ShapeTable(WTF::move(parts));
}

const ShapeTest& ShapeTable::testOf(LayoutSetID tested) const
{
    return tested && tested.index() < m_parts.testOfSet.size() ? m_parts.testOfSet[tested.index()] : m_neverPasses;
}

const ShapeTest& ShapeTable::testOf(LayoutSetID tested, LayoutSetID known) const
{
    const ShapeTest* test = known ? findTest(m_parts.testsWithinKnownSets.span(), tested, known) : nullptr;
    return test ? *test : testOf(tested);
}

std::span<const uint32_t> ShapeTable::memberShapes(LayoutSetID set) const
{
    if (!set || set.index() + 1 >= m_parts.firstMemberShape.size())
        return { };
    return memberShapesOf(m_parts, set);
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
