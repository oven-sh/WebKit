/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedObject.h"
#include "AOTTypedThreads.h"
#include "JSCJSValueInlines.h"
#include "MarkedSpace.h"
#include "Options.h"
#include <wtf/DataLog.h>
#include <numeric>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

Rep resultRepOf(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    if (function.isGenericEntrypoint() || !function.resultType || function.returnsBoxedValue)
        return Rep::JSValue;
    auto& type = program.at(function.resultType);
    if (type.isNever() || type.hasOnlyPrimitives(Primitive::Undefined))
        return Rep::Void;
    return repOf(type);
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
        Vector<uint32_t> preorder;
        Vector<uint32_t> stack { m_root };
        while (!stack.isEmpty()) {
            uint32_t node = stack.takeLast();
            preorder.append(node);
            forEachChild(node, [&](uint32_t child) {
                stack.append(child);
            });
        }
        for (size_t i = preorder.size(); i--;) {
            uint32_t node = preorder[i];
            if (at(node).kind == Kind::Leaf)
                keys[node] = leafKey[node];
            if (uint32_t parent = parentOf(node); parent != none)
                keys[parent] = std::min(keys[parent], keys[node]);
        }
        Vector<uint32_t> result;
        Vector<uint32_t> children;
        stack.append(m_root);
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
                std::ranges::sort(children, [&](uint32_t a, uint32_t b) {
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

    enum class Kind : uint8_t {
        Leaf,
        P,
        Q,
        MergedQ,
    };

    enum class Label : uint8_t {
        Empty,
        Partial,
        Full,
    };

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

    template<typename Functor>
    void forEachChild(uint32_t node, const Functor& functor)
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

bool exists(const Field& field)
{
    return !(field.flags & fieldIsNeverStored);
}

StringID slotSharingName(const Field& field)
{
    return field.placeholderFor ? field.placeholderFor : field.name;
}

std::pair<Vector<uint16_t>, uint16_t> existingFieldOrder(const Layout& layout, const Allocation& allocation)
{
    std::pair<Vector<uint16_t>, uint16_t> result { { }, 0 };
    for (unsigned i = 0; i < allocation.order.size(); ++i) {
        if (!exists(layout.fields[allocation.order[i]]))
            continue;
        result.first.append(allocation.order[i]);
        result.second += i < allocation.initialFieldCount;
    }
    return result;
}

bool startsWith(const Vector<uint16_t>& order, const Vector<uint16_t>& prefix)
{
    return order.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), order.begin());
}

template<typename State>
void forEachIndexOnAllThreads(unsigned count, uint64_t numberOfSteps, const Invocable<State()> auto& makeState, const Invocable<void(State&, unsigned)> auto& functor)
{
    std::atomic<unsigned> next { 0 };
    auto work = [&] {
        State state = makeState();
        for (unsigned i = next++; i < count; i = next++)
            functor(state, i);
    };
    static constexpr uint64_t minStepsForThreads = 1 << 16;
    Vector<Ref<Thread>> threads;
    std::optional<ParallelLoopScope> scope;
    if (numberOfSteps >= minStepsForThreads) {
        scope.emplace();
        for (unsigned i = 1; i < std::min<unsigned>(count, WTF::numberOfProcessorCores()); ++i)
            threads.append(Thread::create("AOT placement"_s, [&work] { work(); }));
    }
    work();
    for (auto& thread : threads)
        thread->waitForCompletion();
}

void sortLexicographically(std::span<uint32_t> lists, const Invocable<uint32_t(uint32_t list, uint32_t position)> auto& keyAt)
{
    struct Range {
        uint32_t first;
        uint32_t count;
        uint32_t position;
    };
    Vector<Range, 64> stack { Range { 0, static_cast<uint32_t>(lists.size()), 0 } };
    while (!stack.isEmpty()) {
        auto [first, count, position] = stack.takeLast();
        while (count > 1) {
            auto range = lists.subspan(first, count);
            uint32_t pivot = keyAt(range[count / 2], position);
            uint32_t smallerCount = 0;
            uint32_t firstGreater = count;
            for (uint32_t i = 0; i < firstGreater;) {
                uint32_t key = keyAt(range[i], position);
                if (key < pivot)
                    std::swap(range[smallerCount++], range[i++]);
                else if (key > pivot)
                    std::swap(range[i], range[--firstGreater]);
                else
                    ++i;
            }
            if (smallerCount > 1)
                stack.append(Range { first, smallerCount, position });
            if (count - firstGreater > 1)
                stack.append(Range { first + firstGreater, count - firstGreater, position });
            if (!pivot)
                break;
            first += smallerCount;
            count = firstGreater - smallerCount;
            ++position;
        }
    }
}

class Placement {
public:
    explicit Placement(Program& program)
        : m_program(program)
        , m_fieldNamed(FillWith { }, program.strings.size(), none)
    {
    }

    void run()
    {
        RELEASE_ASSERT(m_program.slots.isEmpty());
        m_program.types.thaw();
        auto& closure = m_program.at(m_program.closureLayout);
        closure.isAllocated = true;
        closure.size = WTF::roundUpToMultipleOf<16>(Closure::allocationSize(0));

        runStep("checkProgram"_s, &Placement::checkProgram);
        runStep("numberFields"_s, &Placement::numberFields);
        runStep("indexSupertypes"_s, &Placement::indexSupertypes);
        runStep("removeSpecializationsMissingFields"_s, &Placement::removeSpecializationsMissingFields);
        runStep("findFamilies"_s, &Placement::findFamilies);
        runStep("expandAllocations"_s, &Placement::expandAllocations);
        runStep("findInitialStores"_s, &Placement::findInitialStores);
        runStep("findAccessedFields"_s, &Placement::findAccessedFields);
        runStep("shareSlots"_s, &Placement::shareSlots);
        runStep("findMissingFields"_s, &Placement::findMissingFields);
        runStep("countInitialStores"_s, &Placement::countInitialStores);
        runStep("clearInvalidFlags"_s, &Placement::clearInvalidFlags);
        runStep("collectTypes"_s, &Placement::collectTypes);
        runStep("scanFunctions"_s, &Placement::scanFunctions);
        runStep("groupLayouts"_s, &Placement::groupLayouts);
        runStep("followSpecializations"_s, &Placement::followSpecializations);
        runStep("findPossiblyAbsentSlots"_s, &Placement::findPossiblyAbsentSlots);
        runStep("findLargeSubtypes"_s, &Placement::findLargeSubtypes);
        runStep("findTopLevelActivations"_s, &Placement::findTopLevelActivations);
        runStep("typeSlots"_s, &Placement::typeSlots);
        m_inlineCap.fill(noCap, m_program.layouts.size());
        runStep("placeObjects"_s, &Placement::placeObjects);
        runStep("placeEnvironments"_s, &Placement::placeEnvironments);
        runStep("reserveSpecializationCapacity"_s, &Placement::reserveSpecializationCapacity);
        runStep("capLayouts"_s, &Placement::capLayouts);
        runStep("widenNumberFieldsIntoPadding"_s, &Placement::widenNumberFieldsIntoPadding);
        runStep("pinLiterals"_s, &Placement::pinLiterals);
        runStep("reserveExtraPropertiesSlot"_s, &Placement::reserveExtraPropertiesSlot);
        runStep("widenReferenceFieldsIntoPadding"_s, &Placement::widenReferenceFieldsIntoPadding);
        runStep("sizeAllocations"_s, &Placement::sizeAllocations);
        runStep("collectOrders"_s, &Placement::collectOrders);
        runStep("numberShapes"_s, &Placement::numberShapes);
        runStep("collectSubtypeShapes"_s, &Placement::collectSubtypeShapes);
        runStep("placeVariables"_s, &Placement::placeVariables);
        runStep("placeArraySites"_s, &Placement::placeArraySites);
        runStep("placeRegExpLiterals"_s, &Placement::placeRegExpLiterals);
        runStep("findFieldsWithSlotInEverySubtype"_s, &Placement::findFieldsWithSlotInEverySubtype);
        runStep("findPossiblyUnlistedFields"_s, &Placement::findPossiblyUnlistedFields);
        runStep("findLazyClosures"_s, &Placement::findLazyClosures);
        runStep("findLazyClosuresCreatedWithPlainThis"_s, &Placement::findLazyClosuresCreatedWithPlainThis);
        runStep("checkPlacement"_s, &Placement::checkPlacement);
        runStep("findFieldsInitializedInEverySubtype"_s, &Placement::findFieldsInitializedInEverySubtype);
        m_program.placedStaticsSize = m_program.sizeOfStatics;
        m_program.types.freeze();
    }

private:
    static constexpr uint32_t none = UINT32_MAX;

    std::span<const char> nameOf(StringID name) const { return byteCast<char>(m_program.string(name ? name : m_program.emptyString)); }
    std::span<const char> nameOf(const Layout& layout) const { return nameOf(layout.name); }

    template<typename... Arguments>
    void report(const Arguments&... arguments) const
    {
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT placement: ", arguments...);
    }

    template<typename Found>
    static bool isEmpty(const Found& found) { return found.isEmpty(); }

    template<typename... Lists>
    static bool isEmpty(const std::tuple<Lists...>& found)
    {
        return std::apply([](auto&... lists) {
            return (lists.isEmpty() && ...);
        }, found);
    }

    size_t indexOf(const Function& function) const { return &function - std::as_const(m_program).functions.begin(); }

    template<typename Found>
    void forEachFunctionInOrder(const Invocable<Found(Function&)> auto& find, const Invocable<void(Found&&)> auto& take, uint8_t held = 0)
    {
        auto found = collectInParallel<Found>(m_program.functions.size(), [&](unsigned i, Vector<Found>& collected) {
            if (held && !(m_referencedByFunction[i] & held))
                return;
            Found inFunction = find(m_program.functions.atKeepingValueIndex(i));
            if (!isEmpty(inFunction))
                collected.append(WTF::move(inFunction));
        });
        for (auto& inFunction : found)
            take(WTF::move(inFunction));
    }

    void findFieldsInitializedInEverySubtype()
    {
        for (auto& layout : m_program.layouts) {
            for (auto& field : layout.fields)
                field.storedInSlot = field.stored;
        }
        Vector<TypeID> typesWithoutAbsent;
        unsigned numberOfFields = 0;
        for (auto& layout : m_program.layouts) {
            if (!isObjectLayout(layout.kind) || layout.subtypes.isEmpty())
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field) || !field.storedInSlot || field.isOutOfLine || field.isMissingInSomeSubtype || field.hasOtherOffsetInSomeSubtype)
                    continue;
                if (!(m_program.at(field.storedInSlot).primitives & Primitive::Absent))
                    continue;
                bool isInEverySubtype = std::ranges::all_of(layout.subtypes, [&](LayoutID subtype) {
                    auto& fields = std::as_const(m_program).at(subtype).fields;
                    auto own = std::as_const(m_program).at(subtype).fieldNamed(field.name);
                    return own && fields[*own].flags & fieldIsNeverObservedAbsent && fields[*own].slot == field.slot;
                });
                if (!isInEverySubtype)
                    continue;
                uint32_t index = field.storedInSlot.index();
                if (typesWithoutAbsent.size() <= index)
                    typesWithoutAbsent.grow(index + 1);
                if (!typesWithoutAbsent[index])
                    typesWithoutAbsent[index] = m_program.types.intern(m_program.at(field.storedInSlot).withoutPrimitives(Primitive::Absent));
                field.stored = typesWithoutAbsent[index];
                ++numberOfFields;
            }
        }
        report(numberOfFields, " supertype fields share a slot that may be empty, but no subtype is ever observed without them");
    }

    void findLazyClosures()
    {
        if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
            return;
        struct ClosureStore {
            uint32_t function;
            ValueID store;
            uint32_t slot;
            FunctionID closure;
            LayoutID ofClass;
        };
        Vector<ClosureStore> stores = collectInParallel<ClosureStore>(m_program.functions.size(), [&](unsigned number, Vector<ClosureStore>& collected) {
            auto& function = std::as_const(m_program).functions[number];
            if (function.kind != FunctionKind::Constructor || !function.has(Format::FunctionHasThis) || function.isGenericEntrypoint() || function.blocks.isEmpty() || function.blocks[0].parameters.size() <= function.captureTypes.size())
                return;
            ValueID self = function.blocks[0].parameters[function.captureTypes.size()];
            LayoutID ofClass = m_program.typeOf(function, self).singleLayout();
            if (!ofClass)
                return;
            auto& layout = m_program.at(ofClass);
            if (layout.kind != LayoutKind::Instance || layout.base || !layout.hasFixedLayout || layout.acceptsUntypedObjects || layout.mayDespecialize || layout.constructor != FunctionID(number))
                return;
            auto isSelf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                    id = function.at(id).child(0);
                return id == self;
            };
            Vector<uint8_t> uses;
            auto useCountOf = [&](ValueID id) {
                if (uses.isEmpty()) {
                    uses.fill(0, function.values.size());
                    auto note = [&](ValueID used) {
                        if (used && uses[used.index()] < 2)
                            ++uses[used.index()];
                    };
                    for (auto& block : function.blocks) {
                        for (ValueID user : block.values)
                            function.at(user).forEachChild(note);
                        note(block.terminator.value);
                        for (auto& target : block.terminator.targets) {
                            for (ValueID argument : target.arguments)
                                note(argument);
                        }
                    }
                }
                return uses[id.index()];
            };
            for (ValueID id : function.blocks[0].values) {
                auto& value = function.at(id);
                bool runsNoCode = !value.inlineFrame;
                switch (value.opcode) {
                case Opcode::Undefined:
                case Opcode::Null:
                case Opcode::Boolean:
                case Opcode::Integer:
                case Opcode::Double:
                case Opcode::String:
                case Opcode::FunctionReference:
                case Opcode::BuiltinFunction:
                case Opcode::NewObject:
                case Opcode::NewArray:
                case Opcode::NewClosure:
                    break;
                case Opcode::Cast:
                    runsNoCode &= value.castReason() == CastReason::Upcast;
                    break;
                case Opcode::PutById: {
                    auto named = isSelf(value.child(0)) ? layout.fieldNamed(value.string()) : std::nullopt;
                    runsNoCode &= !!named;
                    if (!runsNoCode)
                        break;
                    auto& field = layout.fields[*named];
                    auto& stored = function.at(value.child(1));
                    if (stored.opcode != Opcode::NewClosure || stored.inlineFrame || stored.arguments.size() > 1 || (stored.arguments.size() && !isSelf(stored.arguments[0])) || useCountOf(value.child(1)) != 1)
                        break;
                    auto& callee = std::as_const(m_program).at(stored.function());
                    unsigned captures = stored.arguments.size();
                    if (callee.kind == FunctionKind::Constructor || m_program.isResumeFunction(callee) || !m_program.usesSlimClosures(callee) || m_program.closureReferenceAndCaptureCounts(callee) != std::pair(captures, captures) || !m_program.genericEntrypoints.contains(stored.function().index()))
                        break;
                    if (!m_program.lazyClosureNames.get(field.name.index()) || !exists(field) || field.isOutOfLine || field.store != Store::OptionalCell32 || field.isPinned || field.flags & (fieldIsAbsentAtCreation | fieldIsImmutable | fieldIsPlaceholder | fieldMayBeUnlisted) || m_program.at(field.slot).isDiscriminant || isPrivateName(m_program.string(field.name)))
                        break;
                    collected.append({ number, id, field.slot.index(), stored.function(), ofClass });
                    break;
                }
                default:
                    runsNoCode = false;
                    break;
                }
                if (!runsNoCode)
                    break;
            }
        });
        forEachFunctionInOrder<Vector<ClosureStore>>([&](Function& function) {
            Vector<ClosureStore> found;
            Vector<uint8_t> uses;
            auto isUsedOnce = [&](ValueID id) {
                if (uses.isEmpty()) {
                    uses.fill(0, function.values.size());
                    auto note = [&](ValueID used) {
                        if (used && uses[used.index()] < 2)
                            ++uses[used.index()];
                    };
                    for (auto& block : function.blocks) {
                        for (ValueID user : block.values)
                            std::as_const(function).at(user).forEachChild(note);
                        note(block.terminator.value);
                        for (auto& target : block.terminator.targets) {
                            for (ValueID argument : target.arguments)
                                note(argument);
                        }
                    }
                }
                return uses[id.index()] == 1;
            };
            std::optional<BitVector> copied;
            auto isCopiedHere = [&](ValueID object) {
                if (!copied) {
                    copied.emplace();
                    for (auto& value : std::as_const(function).values) {
                        if (value.opcode != Opcode::CallBuiltin && value.opcode != Opcode::Operation)
                            continue;
                        auto operation = languageOperationOf(m_program, value);
                        if (operation != LanguageOperation::CopyDataProperties && operation != LanguageOperation::CopyDataPropertiesExcluding && operation != LanguageOperation::CloneObject && operation != LanguageOperation::CloneObjectWith && operation != LanguageOperation::CloneObjectAfter)
                            continue;
                        for (ValueID argument : value.arguments) {
                            while (function.at(argument).opcode == Opcode::Cast)
                                argument = function.at(argument).child(0);
                            copied->set(argument.index());
                        }
                    }
                }
                return copied->get(object.index());
            };
            for (auto& block : function.blocks) {
                for (size_t at = 0; at < block.values.size(); ++at) {
                    ValueID object = block.values[at];
                    if (function.at(object).opcode != Opcode::NewObjectWithShape)
                        continue;
                    LayoutID created = function.at(object).layout();
                    auto& layout = m_program.at(created);
                    auto& allocation = m_program.allocations[function.at(object).allocation()];
                    if (layout.kind != LayoutKind::Record || allocation.layout != created || allocation.numberOfLiterals || allocation.discriminant != UINT16_MAX || !layout.specializations.isEmpty() || layout.mayDespecialize)
                        continue;
                    for (size_t next = at + 1; next < block.values.size() && (function.at(block.values[next]).opcode == Opcode::NewClosure || isInitializingStore(m_program, function, block, next)); ++next) {
                        auto& store = function.at(block.values[next]);
                        auto named = store.opcode == Opcode::PutById ? layout.fieldNamed(store.string()) : std::nullopt;
                        if (!named && (store.opcode != Opcode::PutField || store.layout() != created))
                            continue;
                        uint32_t index = named ? *named : store.field();
                        auto& field = layout.fields[index];
                        auto& stored = function.at(store.child(1));
                        unsigned captures = stored.arguments.size();
                        if (stored.opcode != Opcode::NewClosure || captures > 1 || (captures && stored.arguments[0] != object) || !isUsedOnce(store.child(1)))
                            continue;
                        auto& callee = std::as_const(m_program).at(stored.function());
                        if (callee.kind == FunctionKind::Constructor || m_program.isResumeFunction(callee) || !m_program.usesSlimClosures(callee) || m_program.closureReferenceAndCaptureCounts(callee) != std::pair(captures, captures) || !m_program.genericEntrypoints.contains(stored.function().index()))
                            continue;
                        if (!m_program.lazyClosureNames.get(field.name.index()) || !exists(field) || field.isOutOfLine || field.store != Store::OptionalCell32 || field.isPinned || field.flags & (fieldIsImmutable | fieldIsPlaceholder | fieldMayBeUnlisted) || m_program.at(field.slot).isDiscriminant || isPrivateName(m_program.string(field.name)))
                            continue;
                        auto initialized = allocation.order.span().first(allocation.initialFieldCount);
                        if (std::ranges::find(initialized, static_cast<uint16_t>(index)) == initialized.end() || isCopiedHere(object))
                            continue;
                        found.append({ static_cast<uint32_t>(&function - &std::as_const(m_program.functions)[0]), block.values[next], field.slot.index(), stored.function(), created });
                    }
                }
            }
            return found;
        }, [&](Vector<ClosureStore>&& found) {
            stores.appendVector(found);
        }, containsNewObjectWithShape);
        if (stores.isEmpty())
            return;
        Vector<FunctionID> slotClosure(m_program.slots.size());
        BitVector isExcluded;
        isExcluded.ensureSize(m_program.slots.size());
        for (auto& store : stores) {
            if (slotClosure[store.slot] && slotClosure[store.slot] != store.closure)
                isExcluded.quickSet(store.slot);
            slotClosure[store.slot] = store.closure;
        }
        for (auto& layout : m_program.layouts) {
            if (!hasObjects(layout))
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field) || !slotClosure[field.slot.index()])
                    continue;
                if ((layout.kind == LayoutKind::Instance && (layout.acceptsUntypedObjects || layout.mayDespecialize)) || field.isOutOfLine || field.store != Store::OptionalCell32 || field.flags & fieldMayBeUnlisted)
                    isExcluded.quickSet(field.slot.index());
            }
        }
        forEachFunctionInOrder<Vector<uint32_t>>([&](Function& function) {
            Vector<uint32_t> read;
            static constexpr OpcodeSet fieldReads { Opcode::GetField };
            std::as_const(m_program).functions.forEachValue(indexOf(function), fieldReads, [&](ValueID id) {
                auto& value = std::as_const(function).at(id);
                if (!isObjectLayout(m_program.at(value.layout()).kind))
                    return;
                if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && slotClosure[field.slot.index()])
                    read.append(field.slot.index());
            });
            return read;
        }, [&](Vector<uint32_t>&& read) {
            for (uint32_t slot : read)
                isExcluded.quickSet(slot);
        });
        unsigned numberOfFields = 0;
        for (auto& layout : m_program.layouts) {
            for (auto& field : layout.fields) {
                if (!isObjectLayout(layout.kind) || !exists(field) || !slotClosure[field.slot.index()] || isExcluded.quickGet(field.slot.index()))
                    continue;
                field.flags |= fieldHoldsLazyClosure;
                field.lazyClosure = slotClosure[field.slot.index()];
                ++numberOfFields;
            }
        }
        unsigned numberOfStores = 0;
        for (auto& store : stores) {
            if (isExcluded.quickGet(store.slot))
                continue;
            m_program.functions.atKeepingValueIndex(store.function).at(store.store).storesLazyClosure = true;
            ++numberOfStores;
        }
        report(numberOfStores, " closures are created lazily when their fields are read, not when their objects are constructed; ", numberOfFields, " fields of layouts and supertypes hold them");
    }

    void findLazyClosuresCreatedWithPlainThis()
    {
        if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions() || m_program.mayRunInMultipleInstances)
            return;
        struct ClosureStore {
            uint32_t function;
            ValueID store;
            FunctionID closure;
            LayoutID ofClass;
        };
        auto isDefinedAtCreation = [](const Field& field) {
            return exists(field) && !field.isPinned && !(field.isPinnedToFunction && field.slot) && !(field.flags & (fieldIsAbsentAtCreation | fieldIsPlaceholder | fieldMayBeUnlisted));
        };
        Vector<ClosureStore> stores = collectInParallel<ClosureStore>(m_program.functions.size(), [&](unsigned number, Vector<ClosureStore>& collected) {
            auto& function = std::as_const(m_program).functions[number];
            if (function.kind != FunctionKind::Constructor || !function.has(Format::FunctionHasThis) || function.isGenericEntrypoint() || function.blocks.isEmpty() || function.blocks[0].parameters.size() <= function.captureTypes.size())
                return;
            ValueID self = function.blocks[0].parameters[function.captureTypes.size()];
            LayoutID ofClass = m_program.typeOf(function, self).singleLayout();
            if (!ofClass)
                return;
            auto& layout = m_program.at(ofClass);
            if (layout.kind != LayoutKind::Instance || layout.base || layout.hasUntypedBase || !layout.hasFixedLayout || !layout.acceptsUntypedObjects || !layout.test.neverPasses() || layout.constructor != FunctionID(number))
                return;
            auto isSelf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
                    id = function.at(id).child(0);
                return id == self;
            };
            auto storesToOwnField = [&](const Value& value) {
                if (value.opcode != Opcode::PutById || !isSelf(value.child(0)) || isSelf(value.child(1)))
                    return false;
                auto named = layout.fieldNamed(value.string());
                return named && isDefinedAtCreation(layout.fields[*named]);
            };
            Vector<uint8_t> uses;
            uses.fill(0, function.values.size());
            auto note = [&](ValueID used) {
                if (used && uses[used.index()] < 2)
                    ++uses[used.index()];
            };
            Vector<StringID, 64> storedNames;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    function.at(id).forEachChild(note);
                    if (function.at(id).opcode == Opcode::PutById && isSelf(function.at(id).child(0)))
                        storedNames.append(function.at(id).string());
                }
                note(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        note(argument);
                }
            }
            BitVector isHeldOnlyBySelf;
            Vector<ValueID, 32> candidates;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (!storesToOwnField(value) || function.at(value.child(1)).opcode != Opcode::NewClosure || uses[value.child(1).index()] != 1)
                        continue;
                    isHeldOnlyBySelf.set(value.child(1).index());
                    auto& stored = function.at(value.child(1));
                    if (value.inlineFrame || stored.inlineFrame || value.isInitialStore() || value.isAtKnownSlot() || stored.arguments.size() != 1 || !isSelf(stored.arguments[0]))
                        continue;
                    if ((block.terminator.kind == TerminatorKind::Invoke && block.values.last() == id) || std::ranges::count(storedNames, value.string()) != 1 || isPrivateName(m_program.string(value.string())))
                        continue;
                    auto& callee = std::as_const(m_program).at(stored.function());
                    if (callee.kind == FunctionKind::Constructor || m_program.isResumeFunction(callee) || m_program.closureReferenceAndCaptureCounts(callee) != std::pair(1u, 1u) || !m_program.genericEntrypoints.contains(stored.function().index()))
                        continue;
                    candidates.append(id);
                }
            }
            if (candidates.isEmpty())
                return;
            auto showsSelf = [&](ValueID id) {
                auto& value = function.at(id);
                if (storesToOwnField(value) || isHeldOnlyBySelf.get(id.index()) || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast))
                    return false;
                bool shows = false;
                value.forEachChild([&](ValueID child) {
                    shows |= isSelf(child);
                });
                return shows;
            };
            struct Stored {
                bool isReached { false };
                BitVector surely;
                BitVector possibly;
            };
            Vector<Stored> atEntry(function.blocks.size());
            atEntry[0].isReached = true;
            BitVector isStoredBeforeSelfIsSeen;
            for (size_t candidate = 0; candidate < candidates.size(); ++candidate)
                isStoredBeforeSelfIsSeen.set(candidate);
            auto flowThrough = [&](size_t index, bool judges) {
                Stored now = atEntry[index];
                auto& block = function.blocks[index];
                for (ValueID id : block.values) {
                    if (size_t candidate = candidates.find(id); candidate != notFound) {
                        if (judges && now.possibly.get(candidate))
                            isStoredBeforeSelfIsSeen.clear(candidate);
                        now.surely.set(candidate);
                        now.possibly.set(candidate);
                    } else if (judges && showsSelf(id))
                        isStoredBeforeSelfIsSeen.filter(now.surely);
                }
                bool shows = block.terminator.kind == TerminatorKind::Return || (block.terminator.value && isSelf(block.terminator.value));
                for (auto& target : block.terminator.targets)
                    shows |= std::ranges::any_of(target.arguments, isSelf);
                if (judges && shows)
                    isStoredBeforeSelfIsSeen.filter(now.surely);
                return now;
            };
            Vector<size_t, 16> worklist { 0 };
            while (!worklist.isEmpty()) {
                size_t index = worklist.takeLast();
                Stored atEnd = flowThrough(index, false);
                for (auto& target : function.blocks[index].terminator.targets) {
                    Stored& next = atEntry[target.block.index()];
                    Stored before = next;
                    if (next.isReached) {
                        next.surely.filter(atEnd.surely);
                        next.possibly.merge(atEnd.possibly);
                    } else
                        next = atEnd;
                    if (!before.isReached || before.surely != next.surely || before.possibly != next.possibly)
                        worklist.append(target.block.index());
                }
            }
            for (size_t index = 0; index < function.blocks.size(); ++index) {
                if (atEntry[index].isReached)
                    flowThrough(index, true);
            }
            for (size_t candidate = 0; candidate < candidates.size(); ++candidate) {
                if (isStoredBeforeSelfIsSeen.get(candidate))
                    collected.append({ number, candidates[candidate], function.at(function.at(candidates[candidate]).child(1)).function(), ofClass });
            }
        });
        unsigned numberOfStores = 0;
        for (auto& store : stores) {
            StringID name = std::as_const(m_program).functions[store.function].at(store.store).string();
            Vector<Field*, 4> fields;
            bool isDefinedInEveryHeir = true;
            for (auto& layout : m_program.layouts) {
                if (layout.kind != LayoutKind::Instance)
                    continue;
                const Layout* root = &layout;
                while (root->base)
                    root = &m_program.at(root->base);
                if (root != &m_program.at(store.ofClass))
                    continue;
                auto named = layout.fieldNamed(name);
                isDefinedInEveryHeir &= named && isDefinedAtCreation(layout.fields[*named]);
                if (named)
                    fields.append(&layout.fields[*named]);
            }
            if (!isDefinedInEveryHeir)
                continue;
            for (Field* field : fields) {
                field->isCreatedWithLazyClosure = true;
                field->lazyClosure = store.closure;
            }
            m_program.functions.atKeepingValueIndex(store.function).at(store.store).storesLazyClosure = true;
            ++numberOfStores;
        }
        report(numberOfStores, " closures over a plain `this` lie in it from its creation: the constructors do not store them");
    }

    void findPossiblyUnlistedFields()
    {
        auto& builtins = Builtins::singleton();
        BitVector mayBeUnlisted;
        mayBeUnlisted.ensureSize(m_program.slots.size());
        static constexpr uint32_t isClassWithoutInstances = 1u << 31;
        Vector<std::pair<LayoutID, StringID>> firstUnlistedFields;
        forEachFunctionInOrder<Vector<uint32_t>>([&](Function& function) {
            Vector<uint32_t> slots;
            static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
            std::as_const(m_program).functions.forEachValue(indexOf(function), builtinCalls, [&](ValueID id) {
                auto& value = function.at(id);
                auto& implementation = builtins.at(value.builtin()).implementation;
                if (implementation.kind != BuiltinImplementation::Operation || (implementation.operation != LanguageOperation::UnlistFields && implementation.operation != LanguageOperation::ListFields))
                    return;
                auto& name = function.at(value.arguments[1]);
                for (LayoutID supertype : m_program.typeOf(function, value.arguments[0]).objectLayouts) {
                    if (m_program.at(supertype).kind == LayoutKind::Instance && !m_program.at(supertype).isAllocated && (name.opcode == Opcode::String || name.opcode == Opcode::ConstantSymbol)) {
                        slots.append(isClassWithoutInstances | supertype.index());
                        slots.append(name.string().index());
                    }
                    for (LayoutID subtype : m_program.at(supertype).subtypes) {
                        auto& layout = m_program.at(subtype);
                        auto named = name.opcode == Opcode::String || name.opcode == Opcode::ConstantSymbol ? layout.fieldNamed(name.string()) : std::nullopt;
                        for (unsigned index = named.value_or(0); layout.kind == LayoutKind::Instance && index < layout.fields.size(); ++index) {
                            auto& field = layout.fields[index];
                            if (exists(field) && !(field.flags & (fieldIsAbsentAtCreation | fieldIsPlaceholder)))
                                slots.append(field.slot.index());
                        }
                    }
                }
            });
            return slots;
        }, [&](Vector<uint32_t>&& slots) {
            for (size_t i = 0; i < slots.size(); ++i) {
                if (slots[i] & isClassWithoutInstances) {
                    firstUnlistedFields.append({ LayoutID(slots[i] & ~isClassWithoutInstances), StringID(slots[i + 1]) });
                    ++i;
                } else
                    mayBeUnlisted.quickSet(slots[i]);
            }
        }, containsCallBuiltin);
        for (auto& layout : m_program.layouts) {
            if (firstUnlistedFields.isEmpty() || layout.kind != LayoutKind::Instance || layout.isAllocated)
                continue;
            for (const Layout* declaring = &layout; declaring; declaring = declaring->base ? &m_program.at(declaring->base) : nullptr) {
                for (auto& [ofClass, name] : firstUnlistedFields) {
                    if (&m_program.at(ofClass) != declaring)
                        continue;
                    for (unsigned index = layout.fieldNamed(name).value_or(layout.fields.size()); index < layout.fields.size(); ++index)
                        layout.fields[index].flags |= fieldMayBeUnlisted;
                }
            }
        }
        if (mayBeUnlisted.isEmpty())
            return;
        for (auto& layout : m_program.layouts) {
            for (auto& field : layout.fields) {
                if (isObjectLayout(layout.kind) && exists(field) && mayBeUnlisted.quickGet(field.slot.index()))
                    field.flags |= fieldMayBeUnlisted;
            }
        }
    }

    static constexpr uint8_t containsNewObjectWithShape = 1;
    static constexpr uint8_t containsNewArray = 2;
    static constexpr uint8_t containsFieldStore = 4;
    static constexpr uint8_t containsCallBuiltin = 8;
    static constexpr uint8_t contentFlagFor(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::NewObjectWithShape:
            return containsNewObjectWithShape;
        case Opcode::NewArray:
            return containsNewArray;
        case Opcode::PutField:
        case Opcode::PutById:
            return containsFieldStore;
        case Opcode::CallBuiltin:
            return containsCallBuiltin;
        default:
            return 0;
        }
    }

    void runStep(ASCIILiteral name, void (Placement::*step)())
    {
        MonotonicTime start = MonotonicTime::now();
        (this->*step)();
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            m_program.functions.verifyValueIndex();
        dataLogLnIf(shouldPrintTimings(), "AOT placement: ", name, " took ", (MonotonicTime::now() - start).milliseconds(), " ms");
        m_phaseTimes.didFinish(name);
    }

    template<typename... Arguments>
    void fail(const Arguments&... arguments)
    {
        static constexpr unsigned maxPrinted = 100;
        if (m_numberOfFailures++ < maxPrinted)
            dataLogLn("AOT placement: ", arguments...);
    }

    void crashIfFailed()
    {
        if (!m_numberOfFailures)
            return;
        dataLogLn("AOT placement: ", m_numberOfFailures.load(), " errors in the front end's output");
        RELEASE_ASSERT_NOT_REACHED();
    }

    void checkProgram()
    {
        auto& layouts = m_program.layouts;
        auto checkList = [&](unsigned index, const Vector<LayoutID>& list, ASCIILiteral what) {
            for (unsigned i = 0; i < list.size(); ++i) {
                if (list[i].index() >= layouts.size() || !isObjectLayout(m_program.at(list[i]).kind) || !m_program.at(list[i]).isAllocated)
                    fail("layout ", index, " ", nameOf(layouts[index]), " has among its ", what, " layout ", list[i].index(), ", which is not an allocated layout");
                if (i && list[i - 1] >= list[i])
                    fail("the ", what, " of layout ", index, " ", nameOf(layouts[index]), " are not sorted");
            }
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            m_maxFieldCount = std::max<uint32_t>(m_maxFieldCount, layout.fields.size());
            if (!isObjectLayout(layout.kind)) {
                if (!layout.subtypes.isEmpty() || !layout.specializations.isEmpty())
                    fail("layout ", i, " is not an object layout but has subtypes or specializations");
                if (!std::ranges::all_of(layout.fields, exists))
                    fail("layout ", i, " is not an object layout but has a field that is never stored");
                continue;
            }
            if (layout.base && layout.base.index() >= i)
                fail("layout ", i, " ", nameOf(layout), " comes before the layout it extends");
            if (layout.fields.size() > UINT16_MAX)
                fail("layout ", i, " ", nameOf(layout), " has ", layout.fields.size(), " fields, more than a field order can hold");
            for (auto& field : layout.fields) {
                if (!field.name)
                    fail("layout ", i, " ", nameOf(layout), " has a field without a name");
                else if (std::exchange(m_fieldNamed[field.name.index()], 0u) != none)
                    fail("layout ", i, " ", nameOf(layout), " has two fields named ", nameOf(field.name));
            }
            for (auto& field : layout.fields) {
                if (field.name)
                    m_fieldNamed[field.name.index()] = none;
            }
            checkList(i, layout.subtypes, "members"_s);
            checkList(i, layout.specializations, "specializations"_s);
            if (layout.isAllocated && !std::ranges::binary_search(layout.subtypes, LayoutID(i)))
                fail("layout ", i, " ", nameOf(layout), " is allocated but is not its own subtype");
            for (LayoutID specialization : layout.specializations) {
                if (specialization == LayoutID(i) || !std::ranges::binary_search(layout.subtypes, specialization))
                    fail("layout ", i, " ", nameOf(layout), " has the specialization ", specialization.index(), ", which is not one of its other subtypes");
            }
        }

        auto& allocations = m_program.allocations;
        RELEASE_ASSERT(!allocations.isEmpty() && allocations[0].layout == m_program.closureLayout);
        Vector<uint32_t> lastAllocationListingField(FillWith { }, m_maxFieldCount, none);
        for (unsigned i = 0; i < allocations.size(); ++i) {
            auto& allocation = allocations[i];
            if (allocation.layout.index() >= layouts.size() || !m_program.at(allocation.layout).isAllocated) {
                fail("allocation ", i, " is of layout ", allocation.layout.index(), ", which is not allocated");
                continue;
            }
            auto& layout = m_program.at(allocation.layout);
            if (allocation.initialFieldCount > allocation.order.size())
                fail("allocation ", i, " of ", nameOf(layout), " initializes more fields than it lists");
            for (uint16_t field : allocation.order) {
                if (field >= layout.fields.size() || std::exchange(lastAllocationListingField[field], i) == i)
                    fail("allocation ", i, " of ", nameOf(layout), " lists field ", field, ", which does not exist or is listed twice");
            }
        }

        for (auto& variable : m_program.variables) {
            if (variable.startsUndefined && variable.type && !(m_program.at(variable.type).primitives & Primitive::Undefined))
                fail("variable ", nameOf(variable.name), " starts as undefined, which its type does not include");
            if (variable.startsZero) {
                Type type = variable.type ? m_program.at(variable.type) : Type::anything();
                Rep rep = repOf(type);
                if (rep != Rep::Int32 && rep != Rep::Int64 && rep != Rep::Double)
                    fail("variable ", nameOf(variable.name), " starts as zero, but is not stored as a number or a boolean");
                else if (rep == Rep::Int32 ? !(type.primitives & Primitive::False) : rep == Rep::Int64 && !type.integers.contains(0))
                    fail("variable ", nameOf(variable.name), " starts as zero, which its type does not include");
            }
        }
        crashIfFailed();
    }

    struct NamedField {
        uint32_t layout;
        uint32_t field;
    };

    struct NamedFieldRange {
        uint32_t first;
        uint32_t count;
    };

    struct Fingerprint {
        uint64_t low { 0 };
        uint64_t high { 0 };
        uint32_t count { 0 };

        void add(const std::array<uint64_t, 2>& layoutHash)
        {
            low += layoutHash[0];
            high += layoutHash[1];
            ++count;
        }
        friend auto operator<=>(const Fingerprint&, const Fingerprint&) = default;
    };

    static uint64_t hashOf(uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ value >> 30) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ value >> 27) * 0x94d049bb133111ebull;
        return value ^ value >> 31;
    }

    void numberFields()
    {
        auto& layouts = m_program.layouts;
        uint32_t next = 0;
        Vector<uint32_t> firstFieldNamed(FillWith { }, m_program.strings.size() + 1, 0);
        for (auto& layout : layouts) {
            m_firstField.append(next);
            next += layout.fields.size();
            if (!isObjectLayout(layout.kind))
                continue;
            for (auto& field : layout.fields)
                firstFieldNamed[slotSharingName(field).index() + 1] += exists(field);
        }
        m_firstField.append(next);
        BitVector isNeverStoredSomewhere(m_program.strings.size());
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!exists(field) && firstFieldNamed[field.name.index() + 1])
                    isNeverStoredSomewhere.quickSet(field.name.index());
            }
        }
        for (auto& layout : layouts) {
            if (!isObjectLayout(layout.kind))
                continue;
            for (auto& field : layout.fields) {
                if (!isNeverStoredSomewhere.quickGet(field.name.index()))
                    continue;
                for (LayoutID subtype : layout.subtypes) {
                    for (auto& inSubtype : m_program.at(subtype).fields) {
                        if (inSubtype.name == field.name && exists(inSubtype) != exists(field))
                            fail("the property ", nameOf(field.name), " is never stored in one of ", nameOf(layout), " and its subtype ", nameOf(m_program.at(subtype)), " and is stored in the other");
                    }
                }
            }
        }
        crashIfFailed();
        for (unsigned i = 0; i < m_program.strings.size(); ++i) {
            if (firstFieldNamed[i + 1])
                m_names.append(NamedFieldRange { firstFieldNamed[i], firstFieldNamed[i + 1] });
            firstFieldNamed[i + 1] += firstFieldNamed[i];
        }
        m_fieldsByName.grow(firstFieldNamed.last());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            m_layoutHashes.append({ hashOf(2 * i), hashOf(2 * i + 1) });
            if (!isObjectLayout(layouts[i].kind))
                continue;
            for (unsigned k = 0; k < layouts[i].fields.size(); ++k) {
                if (exists(layouts[i].fields[k]))
                    m_fieldsByName[firstFieldNamed[slotSharingName(layouts[i].fields[k]).index()]++] = NamedField { i, m_firstField[i] + k };
            }
        }
    }

    Field& fieldAt(NamedField field) { return m_program.layouts[field.layout].fields[field.field - m_firstField[field.layout]]; }

    size_t cellSizeFor(size_t size) const
    {
        auto sizeClass = std::ranges::lower_bound(m_sizeClasses, size);
        return sizeClass == m_sizeClasses.end() ? WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(size) : *sizeClass;
    }

    bool mayHaveExtension(uint32_t layout) const
    {
        auto& candidate = m_program.layouts[layout];
        return isObjectLayout(candidate.kind) && candidate.isAllocated && candidate.discriminant == UINT16_MAX;
    }

    void groupLayouts()
    {
        auto& layouts = m_program.layouts;
        m_group.resize(layouts.size());
        std::iota(m_group.begin(), m_group.end(), 0);
        auto find = [&](uint32_t layout) {
            while (m_group[layout] != layout)
                layout = m_group[layout] = m_group[m_group[layout]];
            return layout;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            for (LayoutID target : layouts[i].specializations) {
                if (!mayHaveExtension(i))
                    break;
                auto [minimum, other] = std::minmax({ find(i), find(target.index()) });
                m_group[other] = minimum;
            }
        }
        for (unsigned i = 0; i < layouts.size(); ++i)
            m_group[i] = find(i);
        m_isAllocatedOrCastTarget.ensureSize(layouts.size());
        for (auto& allocation : m_program.allocations)
            m_isAllocatedOrCastTarget.quickSet(allocation.layout.index());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (m_castsOfInlineReads[i] || layouts[i].kind == LayoutKind::Instance)
                m_isAllocatedOrCastTarget.quickSet(i);
        }
    }

    std::span<const uint32_t> supertypesOf(uint32_t layout) const { return m_supertypes.span().subspan(m_firstSupertype[layout], m_firstSupertype[layout + 1] - m_firstSupertype[layout]); }

    void indexSupertypes()
    {
        auto& layouts = m_program.layouts;
        m_firstSupertype.fill(0, layouts.size() + 1);
        for (auto& supertype : layouts) {
            for (LayoutID subtype : supertype.subtypes)
                ++m_firstSupertype[subtype.index() + 1];
        }
        for (unsigned i = 0; i < layouts.size(); ++i)
            m_firstSupertype[i + 1] += m_firstSupertype[i];
        m_supertypes.grow(m_firstSupertype.last());
        Vector<uint32_t> next = m_firstSupertype;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            for (LayoutID subtype : layouts[i].subtypes)
                m_supertypes[next[subtype.index()]++] = i;
        }
    }

    void withFieldsByName(Vector<uint32_t>& fieldNamed, uint32_t layout, const Invocable<void()> auto& functor)
    {
        auto& fields = m_program.layouts[layout].fields;
        for (unsigned i = 0; i < fields.size(); ++i) {
            if (exists(fields[i]))
                fieldNamed[fields[i].name.index()] = i;
        }
        functor();
        for (auto& field : fields)
            fieldNamed[field.name.index()] = none;
    }

    struct NamedFieldTable {
        Vector<uint32_t> layoutIndex;
        Vector<uint32_t> representative;
    };

    void forEachName(const Invocable<void(NamedFieldTable&, std::span<const NamedField>)> auto& functor)
    {
        forEachIndexOnAllThreads<NamedFieldTable>(m_names.size(), m_supertypes.size(), [&] {
            return NamedFieldTable { Vector<uint32_t>(FillWith { }, m_program.layouts.size(), none), { } };
        }, [&](NamedFieldTable& table, unsigned index) {
            auto fields = m_fieldsByName.span().subspan(m_names[index].first, m_names[index].count);
            for (uint32_t place = 0; place < fields.size(); ++place)
                table.layoutIndex[fields[place].layout] = place;
            functor(table, fields);
            for (auto& field : fields)
                table.layoutIndex[field.layout] = none;
        });
    }

    void forEachWithField(const NamedFieldTable& table, std::span<const NamedField> fields, const Vector<LayoutID>& layouts, const Invocable<void(uint32_t place)> auto& functor)
    {
        static constexpr unsigned searchCost = 16;
        if (fields.size() * searchCost < layouts.size()) {
            for (uint32_t place = 0; place < fields.size(); ++place) {
                if (std::ranges::binary_search(layouts, LayoutID(fields[place].layout)))
                    functor(place);
            }
            return;
        }
        for (LayoutID layout : layouts) {
            if (uint32_t place = table.layoutIndex[layout.index()]; place != none)
                functor(place);
        }
    }

    void forEachSubtypeWithField(const NamedFieldTable& table, std::span<const NamedField> fields, const Layout& supertype, const Invocable<void(uint32_t place)> auto& functor)
    {
        forEachWithField(table, fields, supertype.subtypes, functor);
    }

    struct InitialStores {
        uint32_t function;
        LayoutID supertype;
        Vector<StringID> names;
        uint16_t count { 0 };
    };

    void findInitialStores()
    {
        static constexpr OpcodeSet storesByName { Opcode::PutById };
        m_initialStores = collectInParallel<InitialStores>(m_program.functions.size(), [&](unsigned i, Vector<InitialStores>& collected) {
            if (!m_program.functions.mayContain(i, storesByName))
                return;
            auto& function = std::as_const(m_program).functions[i];
            auto stores = initialStoresOf(function);
            if (stores.empty())
                return;
            InitialStores found { i, m_program.typeOf(function, function.at(stores[0]).child(0)).singleLayout(), { }, 0 };
            for (ValueID store : stores) {
                RELEASE_ASSERT(function.at(store).opcode == Opcode::PutById && function.at(store).child(0) == function.at(stores[0]).child(0) && !found.names.contains(function.at(store).string()));
                found.names.append(function.at(store).string());
            }
            if (found.supertype && m_program.at(found.supertype).kind == LayoutKind::Record)
                collected.append(WTF::move(found));
        });
    }

    std::optional<unsigned> fieldOf(const InitialStores& initial, unsigned index) const
    {
        auto found = m_program.at(initial.supertype).fieldNamed(initial.names[index]);
        return found && exists(m_program.at(initial.supertype).fields[*found]) ? found : std::nullopt;
    }

    void forEachLayoutInitializedBy(const InitialStores& initial, const Invocable<void(uint32_t)> auto& functor) const
    {
        for (LayoutID subtype : m_program.at(initial.supertype).subtypes) {
            if (m_program.at(subtype).isAllocated && isObjectLayout(m_program.at(subtype).kind))
                functor(subtype.index());
        }
    }

    void countInitialStores()
    {
        for (auto& initial : m_initialStores) {
            while (initial.count < std::min<size_t>(initial.names.size(), maxFieldsInOrder)) {
                auto field = fieldOf(initial, initial.count);
                if (!field || m_missingFieldIndex[m_firstField[initial.supertype.index()] + *field] != none)
                    break;
                ++initial.count;
            }
        }
    }

    static bool passesObjectToConstructor(const Value& value)
    {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Operation && (implementation.operation == LanguageOperation::ConstructWith || implementation.operation == LanguageOperation::ConstructWithSpread);
    }

    template<typename T>
    static bool contains(std::span<const T> list, T value) { return std::ranges::find(list, value) != list.end(); }

    std::span<const StringID> initialStoreNamesOf(uint32_t function) const
    {
        uint32_t index = m_functionInitialStores[function];
        return index == none ? std::span<const StringID>() : m_initialStores[index].names.span().first(m_initialStores[index].count);
    }

    bool initializesFieldBeforeEscape(uint32_t index, StringID name) const
    {
        auto& allocation = m_program.allocations[index];
        if (allocation.initialFieldCount)
            return false;
        if (allocation.constructedBy)
            return contains(initialStoreNamesOf(allocation.constructedBy.index()), name);
        if (m_hasOtherUses.quickGet(index))
            return false;
        uint32_t layout = allocation.layout.index();
        return !m_isThisLayout.quickGet(layout) || m_commonInitialStoreNames[layout].contains(name);
    }

    void forEachUninitializedField(const Invocable<void(uint32_t allocation, Layout&, Field&)> auto& functor)
    {
        for (uint32_t i = 0; i < m_program.allocations.size(); ++i) {
            auto& allocation = m_program.allocations[i];
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind))
                continue;
            BitVector wasAllocated;
            for (uint16_t field : allocation.order.span().first(allocation.initialFieldCount))
                wasAllocated.set(field);
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                if (!wasAllocated.get(k))
                    functor(i, layout, layout.fields[k]);
            }
        }
    }

    void clearInvalidFlags()
    {
        auto& layouts = m_program.layouts;
        auto& allocations = m_program.allocations;
        m_functionInitialStores.fill(none, m_program.functions.size());
        for (uint32_t i = 0; i < m_initialStores.size(); ++i)
            m_functionInitialStores[m_initialStores[i].function] = i;

        m_isThisLayout.ensureSize(layouts.size());
        m_commonInitialStoreNames.grow(layouts.size());
        auto functionsAndThisLayouts = collectInParallel<std::pair<uint32_t, LayoutID>>(m_program.functions.size(), [&](unsigned i, auto& collected) {
            auto& function = std::as_const(m_program).functions[i];
            LayoutID supertype = function.kind == FunctionKind::Plain && function.thisType && function.allocatesForUnknownCallers ? m_program.at(function.thisType).singleLayout() : LayoutID();
            if (supertype && layouts[supertype.index()].kind == LayoutKind::Record && layouts[supertype.index()].isAllocated)
                collected.append({ i, supertype });
        });
        for (auto [i, supertype] : functionsAndThisLayouts) {
            auto names = initialStoreNamesOf(i);
            auto& byAll = m_commonInitialStoreNames[supertype.index()];
            if (!m_isThisLayout.quickSet(supertype.index())) {
                for (StringID name : names)
                    byAll.append(name);
            } else {
                byAll.removeAllMatching([&](StringID name) {
                    return !contains(names, name);
                });
            }
        }

        m_hasOtherUses.ensureSize(allocations.size());
        static constexpr uint8_t countMask = 3;
        static constexpr uint8_t isForwarded = 4;
        forEachFunctionInOrder<Vector<uint32_t>>([&](Function& function) {
            Vector<uint32_t> otherUses;
            Vector<uint8_t> uses(FillWith { }, function.values.size(), 0);
            auto use = [&](ValueID value) {
                if (value && (uses[value.index()] & countMask) < 2)
                    ++uses[value.index()];
            };
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    for (ValueID child : value.children)
                        use(child);
                    for (ValueID argument : value.arguments)
                        use(argument);
                    if (passesObjectToConstructor(value) && value.arguments.size() > 1)
                        uses[value.arguments[1].index()] |= isForwarded;
                }
                block.terminator.forEachChild(use);
            }
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::NewObjectWithShape && isObjectLayout(m_program.at(value.layout()).kind) && uses[id.index()] != (1 | isForwarded))
                        otherUses.append(value.allocation());
                }
            }
            return otherUses;
        }, [&](Vector<uint32_t>&& otherUses) {
            for (uint32_t allocation : otherUses)
                m_hasOtherUses.quickSet(allocation);
        }, containsNewObjectWithShape);
        for (uint32_t i = 0; i < allocations.size(); ++i) {
            for (uint32_t k = 0; allocations[i].numberOfLiterals && k <= allocations[i].numberOfLiterals && m_hasOtherUses.quickGet(i); ++k)
                m_hasOtherUses.quickSet(allocations[i].firstLiteralAllocation + k);
        }

        unsigned numberOfFlags = 0;
        for (auto& layout : layouts) {
            for (auto& field : layout.fields)
                numberOfFlags += !!(field.flags & fieldIsNeverObservedAbsent);
        }
        if (!numberOfFlags)
            return;
        unsigned uninitializedCount = 0;
        unsigned notInitiallyStoredCount = 0;
        forEachUninitializedField([&](uint32_t index, Layout& layout, Field& field) {
            if (!(field.flags & fieldIsNeverObservedAbsent) || initializesFieldBeforeEscape(index, field.name))
                return;
            field.flags &= ~fieldIsNeverObservedAbsent;
            auto& allocation = allocations[index];
            bool isForFunction = !allocation.initialFieldCount && (allocation.constructedBy || !m_hasOtherUses.quickGet(index));
            ++(isForFunction ? notInitiallyStoredCount : uninitializedCount);
            if (allocation.constructedBy && isForFunction)
                report("  ", nameOf(layout), ".", nameOf(field.name), " may be observed absent: allocation ", index, " is for `new` of ", nameOf(std::as_const(m_program).at(allocation.constructedBy).name), ", which does not store it initially");
            else if (isForFunction)
                report("  ", nameOf(layout), ".", nameOf(field.name), " may be observed absent: allocation ", index, " is for `new`, and some function whose `this` has the layout does not store it initially");
            else
                report("  ", nameOf(layout), ".", nameOf(field.name), " may be observed absent: allocation ", index, ", initialized with ", allocation.initialFieldCount, " of ", layout.fields.size(), " fields, does not initialize it");
        });
        unsigned acrossTransitionsCount = 0;
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (unsigned i = 0; i < layouts.size(); ++i) {
                if (layouts[i].specializations.isEmpty())
                    continue;
                withFieldsByName(m_fieldNamed, i, [&] {
                    for (LayoutID specialization : layouts[i].specializations) {
                        for (auto& field : m_program.at(specialization).fields) {
                            uint32_t own = m_fieldNamed[field.name.index()];
                            bool isFlaggedBefore = own != none && layouts[i].fields[own].flags & fieldIsNeverObservedAbsent;
                            if (isFlaggedBefore == !!(field.flags & fieldIsNeverObservedAbsent))
                                continue;
                            field.flags &= ~fieldIsNeverObservedAbsent;
                            if (own != none)
                                layouts[i].fields[own].flags &= ~fieldIsNeverObservedAbsent;
                            ++acrossTransitionsCount;
                            didChange = true;
                        }
                    }
                });
            }
        }
        report(numberOfFlags, " fields are declared never observed absent. Refuted: ", uninitializedCount, " that an allocation does not initialize, ", notInitiallyStoredCount, " that a possible constructor does not store initially, ", acrossTransitionsCount, " more across a transition");
    }

    void findAccessedFields()
    {
        m_isAccessed.ensureSize(m_firstField.last());
        m_isStoredThrough.ensureSize(m_firstField.last());
        static constexpr uint32_t isStoredThrough = 1u << 31;
        static constexpr uint32_t isNotAccessed = 1u << 30;
        m_referencedByFunction.fill(0, m_program.functions.size());
        forEachFunctionInOrder<Vector<uint32_t>>([&](Function& function) {
            ImmutableFunctionScope immutableFunctionScope;
            Vector<uint32_t> accessed;
            uint8_t held = 0;
            static constexpr OpcodeSet flaggedOpcodes = [] {
                OpcodeSet result;
                for (unsigned opcode = 0; opcode <= std::numeric_limits<uint8_t>::max(); ++opcode) {
                    if (contentFlagFor(static_cast<Opcode>(opcode)))
                        result.add(static_cast<Opcode>(opcode));
                }
                return result;
            }();
            static constexpr OpcodeSet accessesAndFlaggedOpcodes = flaggedOpcodes | OpcodeSet { Opcode::PutById, Opcode::GetById, Opcode::InById, Opcode::DeleteById, Opcode::CallMethod, Opcode::GetField, Opcode::PutField };
            std::as_const(m_program).functions.forEachIndexedValueWithBlock(indexOf(function), accessesAndFlaggedOpcodes, [&](unsigned blockIndex, unsigned index, ValueID id) {
                auto& block = function.blocks[blockIndex];
                auto& value = function.at(id);
                held |= contentFlagFor(value.opcode);
                switch (value.opcode) {
                case Opcode::PutById:
                    if (isInitializingStore(m_program, function, block, index))
                        break;
                    [[fallthrough]];
                case Opcode::GetById:
                case Opcode::InById:
                case Opcode::DeleteById:
                case Opcode::CallMethod: {
                    auto isAccessedThrough = [&](const Type& type) {
                        LayoutID supertype = type.singleLayout();
                        auto found = supertype ? m_program.at(supertype).fieldNamed(value.string()) : std::nullopt;
                        if (!found)
                            return;
                        uint32_t field = m_firstField[supertype.index()] + *found;
                        if (value.opcode == Opcode::DeleteById)
                            field |= isStoredThrough | isNotAccessed;
                        else if (value.opcode == Opcode::PutById || value.opcode == Opcode::CallMethod)
                            field |= isStoredThrough;
                        accessed.append(field);
                    };
                    isAccessedThrough(m_program.typeOf(function, value.child(0)));
                    if (TypeID prediction = function.at(value.child(0)).prediction())
                        isAccessedThrough(m_program.at(prediction));
                    break;
                }
                case Opcode::GetField:
                case Opcode::PutField: {
                    auto& object = function.at(value.child(0));
                    if (isObjectLayout(m_program.at(value.layout()).kind) && (object.opcode != Opcode::NewObjectWithShape || object.layout() != value.layout()))
                        accessed.append((m_firstField[value.layout().index()] + value.field()) | (value.opcode == Opcode::PutField ? isStoredThrough : 0));
                    break;
                }
                default:
                    break;
                }
            });
            m_referencedByFunction[&function - &std::as_const(m_program.functions)[0]] = held;
            return accessed;
        }, [&](Vector<uint32_t>&& accessed) {
            for (uint32_t field : accessed) {
                if (!(field & isNotAccessed))
                    m_isAccessed.quickSet(field & ~(isStoredThrough | isNotAccessed));
                if (field & isStoredThrough)
                    m_isStoredThrough.quickSet(field & ~(isStoredThrough | isNotAccessed));
            }
        });
    }

    void shareSlots()
    {
        auto& layouts = m_program.layouts;
        uint32_t numberOfFields = m_firstField.last();
        Vector<uint32_t> fieldRepresentative(numberOfFields, [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        m_subtypesWith.fill(Fingerprint { }, numberOfFields);
        forEachName([&](NamedFieldTable& table, std::span<const NamedField> fields) {
            auto& representative = table.representative;
            representative.shrink(0);
            for (uint32_t place = 0; place < fields.size(); ++place)
                representative.append(place);
            auto representativeOf = [&](uint32_t place) {
                while (representative[place] != place) {
                    representative[place] = representative[representative[place]];
                    place = representative[place];
                }
                return place;
            };
            for (uint32_t place = 0; place < fields.size(); ++place) {
                uint32_t own = representativeOf(place);
                auto share = [&](uint32_t other) {
                    other = representativeOf(other);
                    if (other == own)
                        return;
                    representative[std::max(own, other)] = std::min(own, other);
                    own = std::min(own, other);
                };
                if (m_isAccessed.quickGet(fields[place].field)) {
                    auto& supertype = layouts[fields[place].layout];
                    bool mayPart = !supertype.structuralSubtypes.isEmpty() && !m_isStoredThrough.quickGet(fields[place].field);
                    forEachSubtypeWithField(table, fields, supertype, [&](uint32_t other) {
                        auto& forSupertype = fieldAt(fields[place]);
                        auto& own = fieldAt(fields[other]);
                        bool isEquivalent = own.type == forSupertype.type && !((own.flags ^ forSupertype.flags) & fieldIsOptional);
                        if (!mayPart || isEquivalent || !std::ranges::binary_search(supertype.structuralSubtypes, LayoutID(fields[other].layout)))
                            share(other);
                    });
                }
                forEachWithField(table, fields, layouts[fields[place].layout].specializations, share);
            }
            for (uint32_t place = 0; place < fields.size(); ++place) {
                Fingerprint subtypesWithField;
                if (m_isAccessed.quickGet(fields[place].field)) {
                    forEachSubtypeWithField(table, fields, layouts[fields[place].layout], [&](uint32_t other) {
                        if (fieldAt(fields[other]).placeholderFor)
                            return;
                        if (representativeOf(other) == representativeOf(place))
                            subtypesWithField.add(m_layoutHashes[fields[other].layout]);
                        else
                            fieldAt(fields[place]).hasOtherOffsetInSomeSubtype = true;
                    });
                }
                m_subtypesWith[fields[place].field] = subtypesWithField;
            }
            for (uint32_t place = 0; place < fields.size(); ++place)
                fieldRepresentative[fields[place].field] = fields[representativeOf(place)].field;
        });
        Vector<uint32_t> representativeSlot(FillWith { }, numberOfFields, none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& fields = layouts[i].fields;
            for (unsigned k = 0; k < fields.size(); ++k) {
                if (!exists(fields[k]))
                    continue;
                uint32_t& slot = representativeSlot[fieldRepresentative[m_firstField[i] + k]];
                if (slot == none) {
                    slot = m_program.slots.size();
                    m_program.slots.append(Slot { });
                }
                fields[k].slot = SlotID(slot);
            }
        }
        m_holdsSeveralTypes.ensureSize(m_program.slots.size());
        m_mayBeAbsent.ensureSize(m_program.slots.size());
        for (auto& layout : layouts) {
            if (layout.discriminant != UINT16_MAX)
                m_program.at(layout.fields[layout.discriminant].slot).isDiscriminant = true;
        }
    }

    struct MissingField {
        uint32_t layout;
        uint32_t field;
        uint32_t subtypesWithField { none };
        uint64_t numberOfAccesses { 0 };
    };

    struct SubtypeSet {
        uint32_t listedFor;
        uint32_t firstMember;
        uint32_t subtypeCount;
    };

    std::span<const uint32_t> subtypesIn(const SubtypeSet& set) const { return m_listedSubtypes.span().subspan(set.firstMember, set.subtypeCount); }

    void findMissingFields()
    {
        auto& layouts = m_program.layouts;
        m_missingFieldIndex.fill(none, m_firstField.last());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind))
                continue;
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                if (!exists(layout.fields[k])) {
                    layout.fields[k].isMissingInSomeSubtype = true;
                    layout.fields[k].offset = none;
                    continue;
                }
                if (!m_isAccessed.quickGet(m_firstField[i] + k)) {
                    layout.fields[k].isMissingInSomeSubtype = true;
                    continue;
                }
                if (m_subtypesWith[m_firstField[i] + k].count == layout.subtypes.size())
                    continue;
                layout.fields[k].isMissingInSomeSubtype = true;
                m_missingFieldIndex[m_firstField[i] + k] = m_missingFields.size();
                m_missingFields.append(MissingField { i, k });
            }
        }
        auto subtypesWith = [&](uint32_t missingFields) -> const Fingerprint& {
            return m_subtypesWith[m_firstField[m_missingFields[missingFields].layout] + m_missingFields[missingFields].field];
        };
        Vector<uint32_t> sortedMissingFields(m_missingFields.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(sortedMissingFields, [&](uint32_t a, uint32_t b) {
            return subtypesWith(a) != subtypesWith(b) ? subtypesWith(a) < subtypesWith(b) : a < b;
        });
        uint32_t subtypeCount = 0;
        for (uint32_t index : sortedMissingFields) {
            if (m_subtypeSets.isEmpty() || subtypesWith(m_subtypeSets.last().listedFor) != subtypesWith(index)) {
                m_subtypeSets.append(SubtypeSet { index, subtypeCount, subtypesWith(index).count });
                subtypeCount += subtypesWith(index).count;
            }
            m_missingFields[index].subtypesWithField = m_subtypeSets.size() - 1;
        }
        m_listedSubtypes.fill(none, subtypeCount);
        forEachName([&](NamedFieldTable& table, std::span<const NamedField> fields) {
            for (auto& field : fields) {
                uint32_t missingFields = m_missingFieldIndex[field.field];
                if (missingFields == none || m_subtypeSets[m_missingFields[missingFields].subtypesWithField].listedFor != missingFields)
                    continue;
                uint32_t next = m_subtypeSets[m_missingFields[missingFields].subtypesWithField].firstMember;
                forEachSubtypeWithField(table, fields, layouts[field.layout], [&](uint32_t place) {
                    if (!fieldAt(fields[place]).placeholderFor && fieldAt(fields[place]).slot == fieldAt(field).slot)
                        m_listedSubtypes[next++] = fields[place].layout;
                });
                auto& listed = m_subtypeSets[m_missingFields[missingFields].subtypesWithField];
                RELEASE_ASSERT(next == listed.firstMember + listed.subtypeCount);
            }
        });
        for (auto& missingFields : m_missingFields) {
            auto& supertype = layouts[missingFields.layout];
            auto& field = supertype.fields[missingFields.field];
            if (field.flags & fieldIsOptional)
                continue;
            ++m_missingRequiredFieldCount;
            if (!Options::verboseAOTCompilation())
                continue;
            auto subtypesWithField = subtypesIn(m_subtypeSets[missingFields.subtypesWithField]);
            size_t next = 0;
            for (LayoutID subtype : supertype.subtypes) {
                if (next < subtypesWithField.size() && subtypesWithField[next] == subtype.index()) {
                    ++next;
                    continue;
                }
                if (m_program.at(subtype).kind != LayoutKind::Record || std::ranges::binary_search(supertype.structuralSubtypes, subtype))
                    continue;
                report("supertype ", missingFields.layout, " ", nameOf(supertype), " requires ", nameOf(field.name), ", for which its subtype ", subtype.index(), " ", nameOf(m_program.at(subtype)), ", a record, has no field");
                break;
            }
        }
    }

    void removeSpecializationsMissingFields()
    {
        auto& layouts = m_program.layouts;
        Vector<uint32_t> specialized;
        uint64_t numberOfSpecializations = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].specializations.isEmpty())
                continue;
            specialized.append(i);
            numberOfSpecializations += layouts[i].specializations.size();
        }
        forEachIndexOnAllThreads<Vector<uint32_t>>(specialized.size(), numberOfSpecializations, [&] {
            return Vector<uint32_t>(FillWith { }, m_program.strings.size(), none);
        }, [&](Vector<uint32_t>& fieldNamed, unsigned index) {
            auto& layout = layouts[specialized[index]];
            size_t numberOfFields = std::ranges::count_if(layout.fields, [](auto& field) {
                return exists(field) && !field.placeholderFor;
            });
            withFieldsByName(fieldNamed, specialized[index], [&] {
                layout.specializations.removeAllMatching([&](LayoutID id) {
                    unsigned numberOfSharedFields = 0;
                    for (auto& field : m_program.at(id).fields) {
                        uint32_t found = fieldNamed[field.name.index()];
                        if (found == none || field.placeholderFor)
                            continue;
                        ++numberOfSharedFields;
                    }
                    return numberOfSharedFields != numberOfFields;
                });
            });
        });
        for (auto& table : m_program.storeMayTransitionCache)
            table.answers.clear();
        uint64_t unchangedCount = 0;
        for (uint32_t index : specialized)
            unchangedCount += layouts[index].specializations.size();
        report(numberOfSpecializations, " specializations of ", specialized.size(), " layouts, of which ", numberOfSpecializations - unchangedCount, " lack a field of the layout and are dropped");
    }

    std::span<const LayoutID> retaggedSubtypesOf(uint32_t supertype) const { return m_retaggedSubtypes.span().subspan(m_firstRetaggedSubtype[supertype], m_firstRetaggedSubtype[supertype + 1] - m_firstRetaggedSubtype[supertype]); }
    std::span<const uint32_t> retagSourcesOf(uint32_t supertype) const { return m_retagSources.span().subspan(m_firstRetagSource[supertype], m_firstRetagSource[supertype + 1] - m_firstRetagSource[supertype]); }

    void followSpecializations()
    {
        auto& layouts = m_program.layouts;
        BitVector hasPrototypeMembers;
        hasPrototypeMembers.ensureSize(layouts.size());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].kind == LayoutKind::Instance && (!layouts[i].methods.isEmpty() || (layouts[i].base && hasPrototypeMembers.quickGet(layouts[i].base.index()))))
                hasPrototypeMembers.quickSet(i);
        }
        Vector<std::pair<uint64_t, uint32_t>> specialized;
        uint64_t numberOfSteps = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].specializations.isEmpty())
                continue;
            uint64_t hash = 14695981039346656037ull;
            for (LayoutID specialization : layouts[i].specializations)
                hash = (hash ^ specialization.index()) * 1099511628211ull;
            specialized.append({ hash, i });
        }
        std::ranges::sort(specialized);
        Vector<std::pair<uint32_t, uint32_t>> groups;
        for (unsigned i = 0; i < specialized.size(); ++i) {
            if (i && layouts[specialized[i].second].specializations == layouts[specialized[groups.last().first].second].specializations) {
                ++groups.last().second;
                continue;
            }
            groups.append({ i, 1 });
            for (LayoutID specialization : layouts[specialized[i].second].specializations)
                numberOfSteps += supertypesOf(specialization.index()).size();
        }
        struct Counts {
            Vector<uint32_t> group;
            Vector<uint32_t> count;
            Vector<uint32_t> incomingGroup;
            Vector<uint32_t> targetSupertypes;
        };
        Vector<Vector<std::pair<uint32_t, uint32_t>>> groupRetagSources(groups.size());
        Vector<Vector<std::pair<uint32_t, uint32_t>>> retaggedSubtypesOfGroups(groups.size());
        forEachIndexOnAllThreads<Counts>(groups.size(), numberOfSteps, [&] {
            return Counts { Vector<uint32_t>(FillWith { }, layouts.size(), none), Vector<uint32_t>(FillWith { }, layouts.size(), 0), Vector<uint32_t>(FillWith { }, layouts.size(), none), { } };
        }, [&](Counts& counts, unsigned group) {
            auto& specializations = layouts[specialized[groups[group].first].second].specializations;
            counts.targetSupertypes.shrink(0);
            for (LayoutID specialization : specializations) {
                bool isRecord = m_program.at(specialization).kind == LayoutKind::Record;
                for (uint32_t supertype : supertypesOf(specialization.index())) {
                    if (std::exchange(counts.group[supertype], group) != group)
                        counts.count[supertype] = 0;
                    ++counts.count[supertype];
                    if (!m_coercionsToLayout[supertype] || !isRecord || m_program.at(specialization).isAbstract || (layouts[supertype].isAllocated && supertype != specialization.index()))
                        continue;
                    if (std::exchange(counts.incomingGroup[supertype], group) != group)
                        counts.targetSupertypes.append(supertype);
                }
            }
            std::ranges::sort(counts.targetSupertypes);
            for (uint32_t i = 0; i < groups[group].second; ++i) {
                uint32_t layout = specialized[groups[group].first + i].second;
                if (layouts[layout].isAllocated && !hasPrototypeMembers.quickGet(layout)) {
                    auto objectLayouts = supertypesOf(layout);
                    size_t next = 0;
                    for (uint32_t supertype : counts.targetSupertypes) {
                        while (next < objectLayouts.size() && objectLayouts[next] < supertype)
                            ++next;
                        if (next == objectLayouts.size() || objectLayouts[next] != supertype)
                            groupRetagSources[group].append({ supertype, layout });
                    }
                    for (LayoutID specialization : specializations) {
                        if (m_program.at(specialization).kind != LayoutKind::Record || m_program.at(specialization).isAbstract || !std::ranges::binary_search(objectLayouts, specialization.index()))
                            continue;
                        retaggedSubtypesOfGroups[group].append({ specialization.index(), layout });
                        if (m_coercionsToLayout[specialization.index()])
                            groupRetagSources[group].append({ specialization.index(), layout });
                    }
                }
                for (uint32_t supertype : supertypesOf(layout)) {
                    if (counts.group[supertype] == group && counts.count[supertype] == specializations.size())
                        continue;
                    for (LayoutID specialization : specializations) {
                        if (!std::ranges::binary_search(layouts[supertype].subtypes, specialization))
                            fail("layout ", layout, " ", nameOf(layouts[layout]), " is a subtype of supertype ", supertype, " ", nameOf(layouts[supertype]), ", its specialization ", specialization.index(), " ", nameOf(m_program.at(specialization)), " is not");
                    }
                }
            }
        });
        crashIfFailed();

        Vector<std::pair<uint32_t, uint32_t>> retagSources;
        for (auto& perGroup : groupRetagSources)
            retagSources.appendVector(perGroup);
        std::ranges::sort(retagSources);
        m_firstRetagSource.fill(0, layouts.size() + 1);
        for (auto [supertype, layout] : retagSources) {
            ++m_firstRetagSource[supertype + 1];
            m_retagSources.append(layout);
        }
        unsigned numberOfSupertypes = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            numberOfSupertypes += !!m_firstRetagSource[i + 1];
            m_firstRetagSource[i + 1] += m_firstRetagSource[i];
        }
        report("casts transition objects into ", numberOfSupertypes, " supertypes, from ", m_retagSources.size(), " layouts in total");

        Vector<std::pair<uint32_t, uint32_t>> retaggedSubtypes;
        for (auto& perGroup : retaggedSubtypesOfGroups)
            retaggedSubtypes.appendVector(perGroup);
        std::ranges::sort(retaggedSubtypes);
        m_firstRetaggedSubtype.fill(0, layouts.size() + 1);
        for (auto [supertype, layout] : retaggedSubtypes) {
            ++m_firstRetaggedSubtype[supertype + 1];
            m_retaggedSubtypes.append(LayoutID(layout));
        }
        for (unsigned i = 0; i < layouts.size(); ++i)
            m_firstRetaggedSubtype[i + 1] += m_firstRetaggedSubtype[i];
        report(m_retaggedSubtypes.size(), " subtypes can be retagged to their supertype's layout");
    }

    static bool isInstantiated(const Layout& layout)
    {
        return layout.isAllocated || !isObjectLayout(layout.kind);
    }

    void holds(SlotID id, TypeID type)
    {
        auto& slot = m_program.at(id);
        if (!slot.type)
            slot.type = type;
        else if (slot.type != type) {
            m_otherTypes.append({ id.index(), type.index() });
            m_holdsSeveralTypes.quickSet(id.index());
        }
    }

    void collectTypes()
    {
        m_isPredictedInt32.ensureSize(m_program.slots.size());
        m_hasIntegerField.ensureSize(m_program.slots.size());
        m_isNeverObservedAbsent.ensureSize(m_program.slots.size());
        m_mayBeObservedAbsent.ensureSize(m_program.slots.size());
        for (bool created : { true, false }) {
            for (auto& layout : m_program.layouts) {
                if (isInstantiated(layout) != created)
                    continue;
                for (auto& field : layout.fields) {
                    if (!exists(field) || (!created && m_isInAllocatedLayout.quickGet(field.slot.index())))
                        continue;
                    holds(field.slot, field.type ? field.type : m_program.types.anything);
                    if (field.flags & fieldIsPredictedInt32)
                        m_isPredictedInt32.quickSet(field.slot.index());
                    if (field.type && m_program.at(field.type).isInteger())
                        m_hasIntegerField.quickSet(field.slot.index());
                    (field.flags & fieldIsNeverObservedAbsent ? m_isNeverObservedAbsent : m_mayBeObservedAbsent).quickSet(field.slot.index());
                }
            }
            if (!created)
                break;
            m_isInAllocatedLayout.ensureSize(m_program.slots.size());
            for (unsigned i = 0; i < m_program.slots.size(); ++i) {
                if (m_program.slots[i].type)
                    m_isInAllocatedLayout.quickSet(i);
            }
        }
    }

    static constexpr uint64_t weightInLoop = 8;

    static void findBlocksInLoops(const Function& function, BitVector& isInLoop)
    {
        unsigned count = function.blocks.size();
        isInLoop = BitVector(count);
        Vector<uint32_t> index(FillWith { }, count, none);
        Vector<uint32_t> low(FillWith { }, count, 0);
        BitVector isOnStack;
        isOnStack.ensureSize(count);
        Vector<uint32_t> stack;
        Vector<std::pair<uint32_t, uint32_t>> work;
        uint32_t next = 0;
        for (unsigned root = 0; root < count; ++root) {
            if (index[root] != none)
                continue;
            work.append({ root, 0 });
            while (!work.isEmpty()) {
                auto [block, successor] = work.last();
                if (!successor) {
                    index[block] = low[block] = next++;
                    stack.append(block);
                    isOnStack.quickSet(block);
                }
                auto& targets = function.blocks[block].terminator.targets;
                if (successor < targets.size()) {
                    ++work.last().second;
                    uint32_t target = targets[successor].block.index();
                    if (target == block)
                        isInLoop.quickSet(block);
                    if (index[target] == none)
                        work.append({ target, 0 });
                    else if (isOnStack.quickGet(target))
                        low[block] = std::min(low[block], index[target]);
                    continue;
                }
                work.removeLast();
                if (!work.isEmpty())
                    low[work.last().first] = std::min(low[work.last().first], low[block]);
                if (low[block] != index[block])
                    continue;
                bool isCycle = stack.last() != block;
                for (;;) {
                    uint32_t member = stack.takeLast();
                    isOnStack.quickClear(member);
                    if (isCycle)
                        isInLoop.quickSet(member);
                    if (member == block)
                        break;
                }
            }
        }
    }

    enum class NumberUse : uint8_t { AsDouble, AsInteger, PassedOn };

    using SlotsReadFrom = UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    NumberUse numberUseBy(const Function& function, const Value& user, ValueID used, const SlotsReadFrom& slotsReadFrom) const
    {
        auto isFraction = [&](const Type& type) {
            return type.isNumber() && !type.isInteger();
        };
        auto withOther = [&](NumberUse ifInteger) {
            ValueID other = user.child(0) == used ? user.child(1) : user.child(0);
            if (slotsReadFrom.contains(other.index()) || m_program.typeOf(function, other).isInteger())
                return ifInteger;
            return isFraction(m_program.typeOf(function, other)) ? NumberUse::AsDouble : NumberUse::AsInteger;
        };
        auto heldBy = [&](const Field& field) {
            return exists(field) && !m_isPredictedInt32.quickGet(field.slot.index()) && field.type && isFraction(m_program.at(field.type)) ? NumberUse::AsDouble : NumberUse::AsInteger;
        };
        switch (user.opcode) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
            return isFraction(m_program.at(user.type)) ? withOther(NumberUse::PassedOn) : NumberUse::AsInteger;
        case Opcode::Div:
        case Opcode::Pow:
            return m_program.typeOf(function, user.child(0)).isNumber() && m_program.typeOf(function, user.child(1)).isNumber() ? NumberUse::AsDouble : NumberUse::AsInteger;
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
        case Opcode::StrictEq:
            return withOther(NumberUse::AsInteger);
        case Opcode::Negate:
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
        case Opcode::Cast:
            return isFraction(m_program.at(user.type)) ? NumberUse::PassedOn : NumberUse::AsInteger;
        case Opcode::PutModuleVariable:
            return m_program.variables[user.variable().index()].type && isFraction(m_program.at(m_program.variables[user.variable().index()].type)) ? NumberUse::AsDouble : NumberUse::AsInteger;
        case Opcode::PutField:
            return user.child(1) == used ? heldBy(m_program.at(user.layout()).fields[user.field()]) : NumberUse::AsInteger;
        case Opcode::PutById:
            if (LayoutID supertype = m_program.typeOf(function, user.child(0)).singleLayout(); supertype && user.child(1) == used) {
                if (auto found = m_program.at(supertype).fieldNamed(user.string()))
                    return heldBy(m_program.at(supertype).fields[*found]);
            }
            return NumberUse::AsInteger;
        case Opcode::CallDirect: {
            auto& callee = std::as_const(m_program).at(user.function());
            if (callee.blocks.isEmpty() || callee.blocks[0].parameters.size() != user.arguments.size())
                return NumberUse::AsInteger;
            for (unsigned i = 0; i < user.arguments.size(); ++i) {
                if (user.arguments[i] == used && !isFraction(m_program.typeOf(callee, callee.blocks[0].parameters[i])))
                    return NumberUse::AsInteger;
            }
            return std::ranges::find(user.children, used) != user.children.end() ? NumberUse::AsInteger : NumberUse::AsDouble;
        }
        default:
            return NumberUse::AsInteger;
        }
    }

    template<typename Add>
    void countUsesOfPredictedInt32Fields(const Function& function, SlotsReadFrom& slotsReadFrom, const Add& add)
    {
        static constexpr unsigned maxRounds = 4;
        auto passOn = [&](ValueID from, ValueID to) {
            Vector<uint32_t, 1> slots = slotsReadFrom.get(from.index());
            auto& known = slotsReadFrom.add(to.index(), Vector<uint32_t, 1> { }).iterator->value;
            bool changed = false;
            for (uint32_t slot : slots) {
                if (!known.contains(slot)) {
                    known.append(slot);
                    changed = true;
                }
            }
            return changed;
        };
        auto isFraction = [&](TypeID type) {
            return type && m_program.at(type).isNumber() && !m_program.at(type).isInteger();
        };
        auto forEachUse = [&](const auto& functor) {
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& user = function.at(id);
                    user.forEachChild([&](ValueID used) {
                        if (slotsReadFrom.contains(used.index()))
                            functor(used, numberUseBy(function, user, used, slotsReadFrom), id);
                    });
                }
                auto& terminator = block.terminator;
                if (terminator.value && slotsReadFrom.contains(terminator.value.index()))
                    functor(terminator.value, terminator.kind == TerminatorKind::Return && isFraction(function.resultType) ? NumberUse::AsDouble : NumberUse::AsInteger, ValueID());
                for (auto& target : terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (slotsReadFrom.contains(target.arguments[i].index()))
                            functor(target.arguments[i], isFraction(function.at(parameters[i]).type) ? NumberUse::PassedOn : NumberUse::AsInteger, parameters[i]);
                    }
                }
            }
        };
        bool changed = true;
        for (unsigned round = 0; changed; ++round) {
            changed = false;
            forEachUse([&](ValueID used, NumberUse use, ValueID user) {
                if (use == NumberUse::PassedOn)
                    changed |= passOn(used, user);
            });
            if (changed && round == maxRounds) {
                for (auto& slots : slotsReadFrom.values()) {
                    for (uint32_t slot : slots)
                        add(m_integerUseCounts[slot], 1);
                }
                return;
            }
        }
        forEachUse([&](ValueID used, NumberUse use, ValueID) {
            if (use == NumberUse::PassedOn)
                return;
            for (uint32_t slot : slotsReadFrom.get(used.index()))
                add(use == NumberUse::AsDouble ? m_doubleUseCounts[slot] : m_integerUseCounts[slot], 1);
        });
    }

    void scanFunctions()
    {
        auto& layouts = m_program.layouts;
        m_typeTests.fill(0, m_program.types.size());
        m_supertypeTests.fill(0, layouts.size());
        m_classTests.fill(0, layouts.size());
        m_coercionsToLayout.fill(0, layouts.size());
        m_coercionsToType.fill(0, m_program.types.size());
        m_castsOfInlineReads.fill(0, layouts.size());
        m_allocationCounts.fill(0, layouts.size());
        m_isAllocatedOften.ensureSize(layouts.size());
        Vector<std::pair<SlotID, TypeID>> allStoredThroughSupertypes;
        Vector<uint32_t> typesToStringify;
        m_initializingAllocationCounts.fill(0, m_firstField.last());
        m_environmentFieldUseCounts.fill(0, m_firstField.last());
        m_accessWeight.fill(0, m_program.slots.size());
        m_doubleStoreCounts.fill(0, m_program.slots.size());
        m_rangeTestedStoreCounts.fill(0, m_program.slots.size());
        m_integerUseCounts.fill(0, m_program.slots.size());
        m_doubleUseCounts.fill(0, m_program.slots.size());
        auto isStoredAfterRangeTest = [&](Function& function, ValueID id) {
            auto& type = m_program.typeOf(function, id);
            return type.isInteger() && !type.integers.isSubsetOf({ minInlineInt32, INT32_MAX });
        };
        auto isStoredAsDoubleValue = [&](Function& function, ValueID id) {
            for (;;) {
                auto& value = function.at(id);
                if (m_program.at(value.type).isInteger())
                    return false;
                if (value.opcode != Opcode::Cast || (value.castReason() != CastReason::Upcast && value.castReason() != CastReason::Store && value.castReason() != CastReason::Flow))
                    return !!(m_program.at(value.type).primitives & Primitive::Number);
                id = value.child(0);
            }
        };
        m_isInCopiedRecord.ensureSize(m_program.slots.size());
        auto recordCopiedRecords = [&](Function& function, const Value& value, Vector<uint32_t>& copied) {
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            if (implementation.kind != BuiltinImplementation::Operation)
                return;
            switch (implementation.operation) {
            case LanguageOperation::CopyDataProperties:
            case LanguageOperation::CopyDataPropertiesExcluding:
            case LanguageOperation::CloneObject:
            case LanguageOperation::CloneObjectWith:
            case LanguageOperation::CloneObjectAfter:
                break;
            default:
                return;
            }
            auto note = [&](const Type& type) {
                for (LayoutID supertype : type.objectLayouts) {
                    copied.append(supertype.index());
                    for (LayoutID subtype : m_program.at(supertype).subtypes)
                        copied.append(subtype.index());
                }
            };
            for (ValueID argument : value.arguments)
                note(m_program.typeOf(function, argument));
            note(m_program.at(value.type));
            if (value.prediction())
                note(m_program.at(value.prediction()));
        };
        struct Found {
            Vector<std::pair<SlotID, TypeID>> storedThroughSupertypes;
            Vector<uint32_t> allocatedOften;
            Vector<uint32_t> copied;
            Vector<uint32_t> firstStoredWithValue;
            Vector<uint32_t> stringifiedTypes;

            bool isEmpty() const { return storedThroughSupertypes.isEmpty() && allocatedOften.isEmpty() && copied.isEmpty() && firstStoredWithValue.isEmpty() && stringifiedTypes.isEmpty(); }
        };
        m_isFirstStoredWithValue.ensureSize(m_program.slots.size());
        auto isFirstValueStore = [&](Function& function, const Block& block, size_t index) {
            auto& store = function.at(block.values[index]);
            if (!(store.opcode == Opcode::PutById && store.isInitialStore()) && !isInitializingStore(m_program, function, block, index))
                return false;
            ValueID constant = m_program.constantOf(function, store.child(1));
            return !constant || function.at(constant).opcode != Opcode::Undefined;
        };
        auto add = [](auto& sum, uint64_t more) {
            WTF::atomicExchangeAdd(&sum, more, std::memory_order_relaxed);
        };
        forEachFunctionInOrder<Found>([&](Function& function) {
            ImmutableFunctionScope immutableFunctionScope;
            auto [storedThroughSupertypes, allocatedOften, copied, firstStoredWithValue, stringifiedTypes] = Found { };
            Vector<uint32_t, 16> initializedAtThisAllocation;
            SlotsReadFrom slotsReadFrom;
            BitVector isInLoop;
            bool didFindBlocksInLoops = false;
            static constexpr OpcodeSet scanned { Opcode::NewObjectWithShape, Opcode::GetById, Opcode::PutById, Opcode::InById, Opcode::CallMethod, Opcode::GetField, Opcode::PutField, Opcode::Cast, Opcode::HasShapeOf, Opcode::IsInstanceOfClass, Opcode::GetByVal, Opcode::Call, Opcode::Construct, Opcode::CallBuiltin, Opcode::Operation, Opcode::IteratorValue, Opcode::CatchValue, Opcode::GetGlobal, Opcode::GetExternalModule, Opcode::GetArgument };
            std::as_const(m_program).functions.forEachIndexedValueWithBlock(indexOf(function), scanned, [&](unsigned blockIndex, unsigned index, ValueID id) {
                if (!std::exchange(didFindBlocksInLoops, true))
                    findBlocksInLoops(function, isInLoop);
                auto& block = function.blocks[blockIndex];
                uint64_t weight = isInLoop.get(blockIndex) ? weightInLoop : 1;
                auto& value = function.at(id);
                if (value.opcode == Opcode::CallBuiltin)
                    recordCopiedRecords(function, value, copied);
                if (value.opcode == Opcode::CallBuiltin && !value.arguments.isEmpty()) {
                    auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
                    if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::JSONStringify && function.at(value.arguments[0]).type)
                        stringifiedTypes.append(function.at(value.arguments[0]).type.index());
                }
                switch (value.opcode) {
                case Opcode::NewObjectWithShape: {
                    uint32_t layout = value.layout().index();
                    if (layouts[layout].kind != LayoutKind::Environment)
                        break;
                    add(m_allocationCounts[layout], 1);
                    if (isInLoop.get(&block - &function.blocks[0]) || !(value.inlineFrame ? std::as_const(m_program).at(function.inlineFrames[value.inlineFrame].callee) : function).captureTypes.isEmpty())
                        allocatedOften.append(layout);
                    initializedAtThisAllocation.shrink(0);
                    for (size_t next = index + 1; next < block.values.size() && isInitializingStore(m_program, function, block, next); ++next) {
                        auto& store = function.at(block.values[next]);
                        if (store.opcode != Opcode::PutField || store.layout() != value.layout())
                            continue;
                        uint32_t field = m_firstField[layout] + store.field();
                        if (!initializedAtThisAllocation.contains(field)) {
                            initializedAtThisAllocation.append(field);
                            add(m_initializingAllocationCounts[field], 1);
                        }
                    }
                    break;
                }
                case Opcode::GetById:
                case Opcode::PutById:
                case Opcode::InById:
                case Opcode::CallMethod: {
                    const Field* field = nullptr;
                    if (LayoutID supertype = m_program.typeOf(function, value.child(0)).singleLayout()) {
                        if (auto found = m_program.at(supertype).fieldNamed(value.string()); found && exists(m_program.at(supertype).fields[*found])) {
                            field = &m_program.at(supertype).fields[*found];
                            if (uint32_t missingFields = m_missingFieldIndex[m_firstField[supertype.index()] + *found]; missingFields != none)
                                add(m_missingFields[missingFields].numberOfAccesses, 1);
                            else if (m_program.at(supertype).acceptsUntypedObjects)
                                add(m_supertypeTests[supertype.index()], 1);
                            add(m_accessWeight[field->slot.index()], weight);
                            if (value.opcode == Opcode::PutById && isStoredAsDoubleValue(function, value.child(1)))
                                add(m_doubleStoreCounts[field->slot.index()], 1);
                            if (value.opcode == Opcode::PutById && isStoredAfterRangeTest(function, value.child(1)))
                                add(m_rangeTestedStoreCounts[field->slot.index()], 1);
                            if (value.opcode == Opcode::GetById && m_isPredictedInt32.quickGet(field->slot.index()))
                                slotsReadFrom.add(id.index(), Vector<uint32_t, 1> { }).iterator->value.append(field->slot.index());
                            if (value.opcode == Opcode::PutById && !isInstantiated(m_program.at(supertype)))
                                storedThroughSupertypes.append({ field->slot, function.at(value.child(1)).type });
                            if (value.opcode == Opcode::PutById && isFirstValueStore(function, block, index))
                                firstStoredWithValue.append(field->slot.index());
                        }
                    }
                    if (value.opcode == Opcode::GetById && !field) {
                        for (LayoutID supertype : m_program.typeOf(function, value.child(0)).objectLayouts) {
                            auto found = m_program.at(supertype).fieldNamed(value.string());
                            if (found && exists(m_program.at(supertype).fields[*found]) && m_isPredictedInt32.quickGet(m_program.at(supertype).fields[*found].slot.index()))
                                add(m_integerUseCounts[m_program.at(supertype).fields[*found].slot.index()], 1);
                        }
                    }
                    if (value.opcode == Opcode::CallMethod || (value.opcode == Opcode::GetById && (!field || m_holdsSeveralTypes.quickGet(field->slot.index()))))
                        add(m_typeTests[value.type.index()], 1);
                    break;
                }
                case Opcode::GetField:
                    if (layouts[value.layout().index()].kind == LayoutKind::Environment)
                        add(m_environmentFieldUseCounts[m_firstField[value.layout().index()] + value.field()], 1);
                    if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && m_isPredictedInt32.quickGet(field.slot.index()))
                        slotsReadFrom.add(id.index(), Vector<uint32_t, 1> { }).iterator->value.append(field.slot.index());
                    break;
                case Opcode::PutField:
                    if (layouts[value.layout().index()].kind == LayoutKind::Environment)
                        add(m_environmentFieldUseCounts[m_firstField[value.layout().index()] + value.field()], 1);
                    if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && isStoredAsDoubleValue(function, value.child(1)))
                        add(m_doubleStoreCounts[field.slot.index()], 1);
                    if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && isStoredAfterRangeTest(function, value.child(1)))
                        add(m_rangeTestedStoreCounts[field.slot.index()], 1);
                    if (auto& layout = m_program.at(value.layout()); !isInstantiated(layout))
                        storedThroughSupertypes.append({ layout.fields[value.field()].slot, function.at(value.child(1)).type });
                    if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && isObjectLayout(m_program.at(value.layout()).kind) && isFirstValueStore(function, block, index))
                        firstStoredWithValue.append(field.slot.index());
                    break;
                case Opcode::Cast:
                    if (value.castReason() == CastReason::Coerce) {
                        for (LayoutID supertype : m_program.at(value.coercedType()).objectLayouts)
                            add(m_coercionsToLayout[supertype.index()], 1);
                        add(m_coercionsToType[value.coercedType().index()], 1);
                    } else if (value.castReason() != CastReason::Upcast)
                        add(m_typeTests[value.type.index()], 1);
                    if (value.castReason() == CastReason::As || value.castReason() == CastReason::Coerce) {
                        ValueID origin = value.child(0);
                        while (function.at(origin).opcode == Opcode::Cast)
                            origin = function.at(origin).child(0);
                        Opcode returnedBy = function.at(origin).opcode;
                        if (returnedBy == Opcode::GetById || returnedBy == Opcode::GetField || returnedBy == Opcode::GetByVal)
                            break;
                        for (LayoutID supertype : m_program.at(value.castReason() == CastReason::Coerce ? value.coercedType() : value.type).objectLayouts)
                            add(m_castsOfInlineReads[supertype.index()], 1);
                    }
                    break;
                case Opcode::HasShapeOf:
                    if (value.immediates[1] != shapeTestIsExact)
                        add(m_supertypeTests[value.layout().index()], 1);
                    break;
                case Opcode::IsInstanceOfClass:
                    add(m_classTests[value.layout().index()], 1);
                    break;
                case Opcode::GetByVal:
                case Opcode::Call:
                case Opcode::Construct:
                case Opcode::CallBuiltin:
                case Opcode::Operation:
                case Opcode::IteratorValue:
                case Opcode::CatchValue:
                case Opcode::GetGlobal:
                case Opcode::GetExternalModule:
                case Opcode::GetArgument:
                    if (value.opcode == Opcode::CallBuiltin && value.prediction() && Builtins::singleton().at(value.builtin()).implementation.kind == BuiltinImplementation::Inline && Builtins::singleton().at(value.builtin()).implementation.inlined == InlineBuiltin::SetExpectedRowType) {
                        for (LayoutID layout : m_program.at(value.prediction()).objectLayouts)
                            add(m_castsOfInlineReads[layout.index()], 1);
                    }
                    add(m_typeTests[value.type.index()], 1);
                    break;
                default:
                    break;
                }
            });
            if (!slotsReadFrom.isEmpty())
                countUsesOfPredictedInt32Fields(function, slotsReadFrom, add);
            return Found { WTF::move(storedThroughSupertypes), WTF::move(allocatedOften), WTF::move(copied), WTF::move(firstStoredWithValue), WTF::move(stringifiedTypes) };
        }, [&](Found&& found) {
            allStoredThroughSupertypes.appendVector(found.storedThroughSupertypes);
            for (uint32_t layout : found.copied) {
                for (auto& field : layouts[layout].fields) {
                    if (exists(field))
                        m_isInCopiedRecord.quickSet(field.slot.index());
                }
            }
            for (uint32_t layout : found.allocatedOften)
                m_isAllocatedOften.quickSet(layout);
            for (uint32_t slot : found.firstStoredWithValue)
                m_isFirstStoredWithValue.quickSet(slot);
            typesToStringify.appendVector(found.stringifiedTypes);
        });
        BitVector isTypeStringified;
        BitVector isLayoutStringified;
        while (!typesToStringify.isEmpty()) {
            uint32_t index = typesToStringify.takeLast();
            if (isTypeStringified.set(index))
                continue;
            auto& type = m_program.at(TypeID(index));
            if (type.elements)
                typesToStringify.append(type.elements.index());
            auto visit = [&](LayoutID id) {
                if (isLayoutStringified.set(id.index()))
                    return;
                for (auto& field : m_program.at(id).fields) {
                    if (!exists(field))
                        continue;
                    ++m_integerUseCounts[field.slot.index()];
                    if (field.type)
                        typesToStringify.append(field.type.index());
                }
            };
            for (LayoutID supertype : type.objectLayouts) {
                visit(supertype);
                for (LayoutID subtype : m_program.at(supertype).subtypes)
                    visit(subtype);
            }
        }
        for (auto [slot, type] : allStoredThroughSupertypes)
            holds(slot, type);
    }

    void findPossiblyAbsentSlots()
    {
        auto& layouts = m_program.layouts;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                auto& field = layout.fields[k];
                bool mayBeAbsent = field.flags & fieldIsOptional && isInstantiated(layout);
                if (layout.kind == LayoutKind::Environment)
                    mayBeAbsent = !layout.isCoroutineFrame && !(field.flags & (fieldIsImmutable | fieldIsNeverReadUninitialized)) && (!m_allocationCounts[i] || m_initializingAllocationCounts[m_firstField[i] + k] != m_allocationCounts[i]);
                if (mayBeAbsent && exists(field))
                    m_mayBeAbsent.quickSet(field.slot.index());
            }
        }

        Vector<uint32_t> lastAllocationWithField(FillWith { }, m_maxFieldCount, none);
        for (unsigned i = 0; i < m_program.allocations.size(); ++i) {
            auto& allocation = m_program.allocations[i];
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind) || allocation.initialFieldCount == layout.fields.size())
                continue;
            for (unsigned k = 0; k < allocation.initialFieldCount; ++k)
                lastAllocationWithField[allocation.order[k]] = i;
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                if (lastAllocationWithField[k] != i && exists(layout.fields[k]) && !(layout.fields[k].flags & fieldIsNeverObservedAbsent))
                    m_mayBeAbsent.quickSet(layout.fields[k].slot.index());
            }
        }

        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].specializations.isEmpty())
                continue;
            withFieldsByName(m_fieldNamed, i, [&] {
                for (LayoutID specialization : layouts[i].specializations) {
                    for (auto& field : m_program.at(specialization).fields) {
                        if (m_fieldNamed[field.name.index()] == none && exists(field))
                            m_mayBeAbsent.quickSet(field.slot.index());
                    }
                }
            });
        }
    }

    void include(Type& type, const Type& other)
    {
        uint16_t arrayClass = Builtins::singleton().arrayClass().index;
        if (other.engineClasses.contains(arrayClass)) {
            if (!type.engineClasses.contains(arrayClass))
                type.elements = other.elements;
            else if (type.elements != other.elements)
                type.elements = type.elements && other.elements ? unionOfTypes(type.elements, other.elements) : TypeID();
        }
        type.primitives |= other.primitives;
        type.integers = type.integers.unionWith(other.integers);
        type.anyObject |= other.anyObject;
        type.anyFunction |= other.anyFunction;
        type.objectLayouts.appendVector(other.objectLayouts);
        type.engineClasses.appendVector(other.engineClasses);
    }

    void normalize(Type& type)
    {
        type.normalizeLiteralString();
        if (type.anyObject) {
            type.anyFunction = true;
            type.objectLayouts.clear();
            type.engineClasses.clear();
            type.elements = { };
            return;
        }
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        std::ranges::sort(type.objectLayouts);
        type.objectLayouts.shrink(std::unique(type.objectLayouts.begin(), type.objectLayouts.end()) - type.objectLayouts.begin());
        if (type.objectLayouts.size() < 2)
            return;
        std::ranges::stable_sort(type.objectLayouts, [&](LayoutID a, LayoutID b) {
            return m_program.at(a).subtypes.size() > m_program.at(b).subtypes.size();
        });
        Vector<LayoutID, 1> kept;
        for (LayoutID supertype : type.objectLayouts) {
            bool isCovered = false;
            for (unsigned i = 0; i < kept.size() && !isCovered; ++i)
                isCovered = m_program.isUpcast(supertype, kept[i]);
            if (!isCovered)
                kept.append(supertype);
        }
        std::ranges::sort(kept);
        type.objectLayouts = WTF::move(kept);
    }

    TypeID unionOfTypes(TypeID a, TypeID b)
    {
        Type type = m_program.at(a);
        include(type, m_program.at(b));
        normalize(type);
        return m_program.types.intern(type);
    }

    TypeID withAbsent(TypeID id)
    {
        if (m_typesWithAbsent.size() <= id.index())
            m_typesWithAbsent.grow(id.index() + 1);
        if (!m_typesWithAbsent[id.index()]) {
            Type type = m_program.at(id);
            type.primitives |= Primitive::Absent;
            m_typesWithAbsent[id.index()] = m_program.types.intern(type);
        }
        return m_typesWithAbsent[id.index()];
    }

    bool isCompressible(const Type& type) const
    {
        if (!type.hasCells() || type.anyObject || type.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::String | Primitive::Symbol | Primitive::LiteralString))
            return false;
        return std::ranges::all_of(type.objectLayouts, [&](LayoutID supertype) {
            return !m_program.at(supertype).acceptsUntypedObjects && !m_hasLargeSubtype.quickGet(supertype.index());
        });
    }

    void findLargeSubtypes()
    {
        auto& layouts = m_program.layouts;
        m_hasLargeSubtype.ensureSize(layouts.size());
        auto mayBeLarge = [&](const Layout& layout) {
            return offsetOfFirstField + sizeof(void*) * (layout.fields.size() + 1) > MarkedSpace::largeCutoff;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (mayBeLarge(layouts[i]) || std::ranges::any_of(layouts[i].subtypes, [&](LayoutID subtype) { return mayBeLarge(m_program.at(subtype)); }))
                m_hasLargeSubtype.quickSet(i);
        }
    }

    void findTopLevelActivations()
    {
        auto& layouts = m_program.layouts;
        m_hasWideReferences.ensureSize(m_program.slots.size());
        unsigned numberOfActivations = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            RELEASE_ASSERT(!layout.mayBeStackAllocated || m_allocationCounts[i]);
            if (layout.kind != LayoutKind::Environment || (layout.isCoroutineFrame && !layout.mayBeStackAllocated) || !m_allocationCounts[i] || (m_isAllocatedOften.quickGet(i) && !layout.mayBeStackAllocated))
                continue;
            ++numberOfActivations;
            for (auto& field : layout.fields) {
                if (exists(field))
                    m_hasWideReferences.quickSet(field.slot.index());
                if (exists(field) && layout.mayBeStackAllocated)
                    m_isPredictedInt32.quickClear(field.slot.index());
            }
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (exists(field) && field.flags & fieldHasWideReference)
                    m_hasWideReferences.quickSet(field.slot.index());
            }
        }
        report(numberOfActivations, " activations of top-level functions keep wide references");
    }

    Store storeFor(const Type& type, bool isPredictedInt32, bool hasWideReferences) const
    {
        Store store = storeOf(type);
        if (store == Store::JSValue && type.hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Boolean))
            return Store::OptionalBoolean;
        if (isPredictedInt32 && sizeOf(store) == 8 && type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent))
            return Store::Int32OrNumberBox;
        if (!hasWideReferences && !Options::useAOTWideReferences() && isCompressible(type)) {
            if (store == Store::Cell)
                return Store::Cell32;
            if (store == Store::JSValue)
                return Store::OptionalCell32;
            if (store == Store::LiteralString)
                return Store::LiteralString32;
        }
        return store;
    }

    Store outOfLineStoreFor(uint32_t slot, const Type& type) const
    {
        Store store = storeFor(type, false, m_hasWideReferences.quickGet(slot));
        return representsAbsentAsZero(store) && (repOf(store) == Rep::JSValue || store == Store::LiteralString32) ? store : Store::JSValue;
    }

    void typeSlots()
    {
        auto& slots = m_program.slots;
        std::ranges::sort(m_otherTypes);
        size_t next = 0;
        unsigned referenceHoldingCount = 0;
        unsigned numberOfNarrowReferences = 0;
        for (unsigned i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (m_holdsSeveralTypes.quickGet(i)) {
                Type type = m_program.at(slot.type);
                for (uint32_t previous = none; next < m_otherTypes.size() && m_otherTypes[next].first == i; ++next) {
                    if (std::exchange(previous, m_otherTypes[next].second) != m_otherTypes[next].second)
                        include(type, m_program.at(TypeID(m_otherTypes[next].second)));
                }
                normalize(type);
                slot.type = m_program.types.intern(type);
            }
            if (m_mayBeAbsent.quickGet(i))
                slot.type = withAbsent(slot.type);
            bool isOnlyUsedAsDouble = !m_integerUseCounts[i] && m_doubleUseCounts[i];
            if (m_isNeverObservedAbsent.quickGet(i) && !m_hasIntegerField.quickGet(i) && m_program.at(slot.type).isNumber() && (m_doubleStoreCounts[i] || m_isInCopiedRecord.quickGet(i) || isOnlyUsedAsDouble))
                m_isPredictedInt32.quickClear(i);
            slot.store = storeFor(m_program.at(slot.type), m_isPredictedInt32.quickGet(i), m_hasWideReferences.quickGet(i));
            referenceHoldingCount += holdsCells(slot.store);
            numberOfNarrowReferences += slot.store == Store::Cell32 || slot.store == Store::OptionalCell32 || slot.store == Store::LiteralString32;
        }
        RELEASE_ASSERT(next == m_otherTypes.size());
        report(numberOfNarrowReferences, " of ", referenceHoldingCount, " slots that hold references take 32 bits");
        report(m_program.layouts.size(), " layouts, ", m_supertypes.size(), " memberships; ", m_firstField.last(), " fields in ", slots.size(), " slots, of which ", m_holdsSeveralTypes.bitCount(), " hold what fields of several types say and ", m_mayBeAbsent.bitCount(), " may be absent; ", m_missingFields.size(), " supertype fields are missing in some subtype, ", m_missingRequiredFieldCount, " of them required");
    }

    struct SlotUse {
        uint32_t numberOfLayouts { 0 };
        uint32_t firstLayout { 0 };
        uint32_t deadline { UINT32_MAX };
        uint64_t layoutBytesNeeded { 0 };
    };

    struct Occupancy {
        Vector<uint64_t, 2> words;
        std::array<uint32_t, 4> firstWordWithRoom { };
    };

    static uint64_t roomIn(uint64_t occupied, unsigned logSize)
    {
        static constexpr uint64_t aligned[] = { UINT64_MAX, 0x5555555555555555ull, 0x1111111111111111ull, 0x0101010101010101ull };
        uint64_t room = ~occupied;
        for (unsigned i = 0; i < logSize; ++i)
            room &= room >> (1u << i);
        return room & aligned[logSize];
    }

    void placeObjects()
    {
        auto prepared = prepareToPlaceObjects();
        static constexpr std::array<unsigned, 3> layoutCountFactor { 0, 2, 4 };
        std::array<PlacedObjects, layoutCountFactor.size()> placed;
        forEachIndexInParallel(placed.size(), [&](unsigned i) {
            placed[i] = placeObjectsInOrder(prepared, layoutCountFactor[i]);
        });
        unsigned best = 0;
        for (unsigned i = 1; i < placed.size(); ++i) {
            if (placed[i].bytes <= placed[best].bytes)
                best = i;
        }
        for (auto& inOrder : placed) {
            for (auto [slot, store] : inOrder.outOfLineSlots)
                withAbsent(m_program.slots[slot].type);
        }
        applyPlacement(prepared, placed[best]);
    }

    struct PlacedObjects {
        Vector<uint32_t> offsets;
        Vector<std::pair<uint32_t, Store>> outOfLineSlots;
        uint64_t bytes { 0 };
    };

    struct ObjectsToPlace {
        Vector<SlotUse> uses;
        Vector<Occupancy> occupancies;
        Vector<Occupancy> extensions;
        Vector<uint32_t> slotLayouts;
        Vector<uint32_t> order;
        BitVector mayBeOutOfLine;
        BitVector isOnlyReachedByTransition;
        uint32_t numberOfUses { 0 };
    };

    static bool hasObjects(const Layout& layout)
    {
        return isObjectLayout(layout.kind) && layout.isAllocated;
    }

    uint32_t bytesNeededBy(const Layout& layout) const
    {
        uint32_t needed = offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0);
        for (auto& field : layout.fields) {
            if (exists(field) && !m_program.at(field.slot).isOutOfLine)
                needed += sizeOf(m_program.at(field.slot).store);
        }
        return needed;
    }

    void restoreOutOfLineSlots()
    {
        auto& slots = m_program.slots;
        for (auto [index, store, type] : std::exchange(m_outOfLineSlots, { })) {
            slots[index].isOutOfLine = false;
            slots[index].store = store;
            slots[index].type = type;
        }
    }

    ObjectsToPlace prepareToPlaceObjects()
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;

        restoreOutOfLineSlots();

        Vector<SlotUse> uses(slots.size());
        Vector<Occupancy> occupancies(layouts.size());
        uint32_t numberOfUses = 0;
        BitVector isImmutable;
        isImmutable.ensureSize(slots.size());
        BitVector mayBeNullish;
        mayBeNullish.ensureSize(slots.size());
        BitVector cannotHoldUndefinedAtCreation;
        cannotHoldUndefinedAtCreation.ensureSize(slots.size());
        BitVector mayBeSetAtCreation;
        mayBeSetAtCreation.ensureSize(slots.size());
        BitVector isSetAtCreation;
        isSetAtCreation.ensureSize(slots.size());
        m_existsAtCreation.ensureSize(slots.size());
        Vector<uint32_t> bytesNeeded(FillWith { }, layouts.size(), 0u);
        forEachIndexInParallel(layouts.size(), [&](unsigned i) {
            if (hasObjects(layouts[i]))
                bytesNeeded[i] = bytesNeededBy(layouts[i]);
        });
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind))
                continue;
            if (!layout.isAllocated) {
                for (auto& field : layout.fields) {
                    if (exists(field) && field.flags & fieldIsImmutable)
                        isImmutable.quickSet(field.slot.index());
                }
                continue;
            }
            occupancies[i].words.append((1ull << (offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0))) - 1);
            uint32_t needed = bytesNeeded[i];
            for (auto& field : layout.fields) {
                if (!exists(field))
                    continue;
                uint32_t slot = field.slot.index();
                auto& use = uses[slot];
                ++use.numberOfLayouts;
                use.deadline = std::min(use.deadline, needed);
                use.layoutBytesNeeded += needed;
                ++numberOfUses;
                if (field.flags & fieldIsImmutable)
                    isImmutable.quickSet(slot);
                if (field.flags & fieldIsOptional || !(field.flags & fieldDisallowsUndefined))
                    mayBeNullish.quickSet(slot);
                if (field.flags & fieldIsSetAtCreation)
                    isSetAtCreation.quickSet(slot);
                if (field.flags & fieldMayBeSetAtCreation)
                    mayBeSetAtCreation.quickSet(slot);
                if (layout.kind != LayoutKind::Instance || field.flags & fieldIsAbsentAtCreation)
                    continue;
                if (!(m_program.at(slots[slot].type).primitives & Primitive::Undefined))
                    cannotHoldUndefinedAtCreation.quickSet(slot);
                m_existsAtCreation.quickSet(slot);
                if (field.flags & fieldIsSetAtCreation)
                    m_isFirstStoredWithValue.quickSet(slot);
            }
        }
        Vector<uint32_t> slotLayouts(numberOfUses);
        uint32_t next = 0;
        for (auto& use : uses)
            use.firstLayout = std::exchange(next, next + use.numberOfLayouts);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!hasObjects(layouts[i]))
                continue;
            for (auto& field : layouts[i].fields) {
                if (exists(field))
                    slotLayouts[uses[field.slot.index()].firstLayout++] = i;
            }
        }
        for (auto& use : uses)
            use.firstLayout -= use.numberOfLayouts;

        Vector<uint32_t> order;
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (uses[i].numberOfLayouts)
                order.append(i);
        }
        BitVector mayBeOutOfLine;
        mayBeOutOfLine.ensureSize(slots.size());
        for (uint32_t index : order) {
            auto layoutsWithSlot = slotLayouts.span().subspan(uses[index].firstLayout, uses[index].numberOfLayouts);
            if (!slots[index].isDiscriminant && std::ranges::all_of(layoutsWithSlot, [&](uint32_t layout) { return layouts[layout].hasExtension; }))
                mayBeOutOfLine.quickSet(index);
        }
        mayBeOutOfLine.exclude(isImmutable);
        BitVector isOnlyReachedByTransition;
        isOnlyReachedByTransition.ensureSize(slots.size());
        for (uint32_t index : order) {
            auto layoutsWithSlot = slotLayouts.span().subspan(uses[index].firstLayout, uses[index].numberOfLayouts);
            if (std::ranges::any_of(layoutsWithSlot, [&](uint32_t layout) { return m_isAllocatedOrCastTarget.quickGet(layout); }))
                continue;
            isOnlyReachedByTransition.quickSet(index);
            mayBeNullish.quickSet(index);
        }
        mayBeOutOfLine.filter(mayBeNullish);
        mayBeOutOfLine.exclude(cannotHoldUndefinedAtCreation);
        if (!std::exchange(m_hasFoundSlotsInitializedByAllocations, true)) {
            for (auto& allocation : m_program.allocations) {
                for (uint16_t field : allocation.order.span().first(allocation.initialFieldCount)) {
                    if (auto& initialized = m_program.at(allocation.layout).fields[field]; exists(initialized)) {
                        m_existsAtCreation.quickSet(initialized.slot.index());
                        if (allocation.initialFieldCount > maxFieldsInOrder)
                            m_isFirstStoredWithValue.quickSet(initialized.slot.index());
                    }
                }
            }
            for (auto& initial : m_initialStores) {
                for (unsigned k = 0; k < initial.count; ++k) {
                    uint32_t slot = m_program.at(initial.supertype).fields[*fieldOf(initial, k)].slot.index();
                    m_existsAtCreation.quickSet(slot);
                    if (initial.count > maxFieldsInOrder)
                        m_isFirstStoredWithValue.quickSet(slot);
                }
            }
        }
        for (size_t slot : m_existsAtCreation) {
            if (m_isFirstStoredWithValue.quickGet(slot))
                mayBeOutOfLine.quickClear(slot);
        }
        auto scoreOf = [&](uint32_t slot) {
            return std::tuple(isSetAtCreation.quickGet(slot), isSetAtCreation.quickGet(slot) ? uses[slot].numberOfLayouts : 0u, mayBeSetAtCreation.quickGet(slot), m_accessWeight[slot]);
        };
        Vector<Occupancy> extensions(layouts.size());
        for (auto& extension : extensions)
            extension.words.append((1ull << offsetOfFirstExtensionSlot) - 1);
        struct PlacementOrderKey {
            decltype(scoreOf(0)) score;
            uint32_t numberOfLayouts;
            uint32_t deadline;
            uint32_t slot;
            uint8_t size;
            bool mayBeOutOfLine;
            bool isOnlyReachedByTransition;
        };
        Vector<PlacementOrderKey> ordered(order.size(), [&](size_t index) {
            uint32_t slot = order[index];
            return PlacementOrderKey { scoreOf(slot), uses[slot].numberOfLayouts, uses[slot].deadline, slot, static_cast<uint8_t>(sizeOf(slots[slot].store)), mayBeOutOfLine.quickGet(slot), mayBeOutOfLine.quickGet(slot) && isOnlyReachedByTransition.quickGet(slot) };
        });
        sortOnPool(ordered.mutableSpan(), [](const PlacementOrderKey& a, const PlacementOrderKey& b) {
            if (a.mayBeOutOfLine != b.mayBeOutOfLine)
                return !a.mayBeOutOfLine;
            if (a.isOnlyReachedByTransition != b.isOnlyReachedByTransition)
                return a.isOnlyReachedByTransition;
            if (a.mayBeOutOfLine && a.score != b.score)
                return a.score > b.score;
            if (a.numberOfLayouts != b.numberOfLayouts)
                return a.numberOfLayouts > b.numberOfLayouts;
            if (a.deadline != b.deadline)
                return a.deadline < b.deadline;
            if (a.size != b.size)
                return a.size > b.size;
            return a.slot < b.slot;
        });
        for (size_t index = 0; index < order.size(); ++index)
            order[index] = ordered[index].slot;
        return { WTF::move(uses), WTF::move(occupancies), WTF::move(extensions), WTF::move(slotLayouts), WTF::move(order), WTF::move(mayBeOutOfLine), WTF::move(isOnlyReachedByTransition), numberOfUses };
    }

    PlacedObjects placeObjectsInOrder(const ObjectsToPlace& prepared, unsigned smallLayoutsFirst) const
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;
        auto& uses = prepared.uses;
        auto& slotLayouts = prepared.slotLayouts;
        auto& mayBeOutOfLine = prepared.mayBeOutOfLine;
        Vector<Occupancy> occupancies = prepared.occupancies;
        Vector<Occupancy> extensions = prepared.extensions;
        Vector<uint32_t> order = prepared.order;
        PlacedObjects placed;
        placed.offsets.fill(offsetOfFirstField, slots.size());
        if (smallLayoutsFirst) {
            size_t inlineCount = std::ranges::partition_point(order, [&](uint32_t slot) {
                return !mayBeOutOfLine.quickGet(slot);
            }) - order.begin();
            Vector<Vector<uint32_t>> waiting(slots.size());
            Vector<uint32_t> layoutSlots;
            for (auto& layout : layouts) {
                if (!hasObjects(layout))
                    continue;
                layoutSlots.shrink(0);
                for (auto& field : layout.fields) {
                    if (exists(field) && !mayBeOutOfLine.quickGet(field.slot.index()))
                        layoutSlots.append(field.slot.index());
                }
                std::ranges::sort(layoutSlots, [&](uint32_t a, uint32_t b) {
                    return uses[a].numberOfLayouts > uses[b].numberOfLayouts;
                });
                for (size_t later = 1; later < layoutSlots.size(); ++later) {
                    for (size_t earlier = 0; earlier < later && uses[layoutSlots[earlier]].numberOfLayouts >= smallLayoutsFirst * uses[layoutSlots[later]].numberOfLayouts; ++earlier)
                        waiting[layoutSlots[earlier]].append(layoutSlots[later]);
                }
            }
            Vector<uint32_t> pendingCount(FillWith { }, slots.size(), 0);
            for (auto& list : waiting) {
                std::ranges::sort(list);
                list.shrink(std::ranges::unique(list).begin() - list.begin());
                for (uint32_t slot : list)
                    ++pendingCount[slot];
            }
            auto ordersAfter = [&](uint32_t a, uint32_t b) {
                uint64_t left = uses[a].layoutBytesNeeded * uses[b].numberOfLayouts;
                uint64_t right = uses[b].layoutBytesNeeded * uses[a].numberOfLayouts;
                if (left != right)
                    return left > right;
                if (sizeOf(slots[a].store) != sizeOf(slots[b].store))
                    return sizeOf(slots[a].store) < sizeOf(slots[b].store);
                return a > b;
            };
            Vector<uint32_t> ready;
            for (uint32_t slot : order.span().first(inlineCount)) {
                if (!pendingCount[slot])
                    ready.append(slot);
            }
            std::ranges::make_heap(ready, ordersAfter);
            size_t next = 0;
            while (!ready.isEmpty()) {
                std::ranges::pop_heap(ready, ordersAfter);
                uint32_t slot = ready.takeLast();
                order[next++] = slot;
                for (uint32_t other : waiting[slot]) {
                    if (--pendingCount[other])
                        continue;
                    ready.append(other);
                    std::ranges::push_heap(ready, ordersAfter);
                }
            }
            RELEASE_ASSERT(next == inlineCount);
        }

        for (uint32_t index : order) {
            auto& slot = slots[index];
            bool isOutOfLine = false;
            unsigned size = sizeOf(slot.store);
            unsigned logSize = WTF::ctz(size);
            auto layoutsWithSlot = slotLayouts.span().subspan(uses[index].firstLayout, uses[index].numberOfLayouts);
            auto findFirstWordWithRoom = [&](Vector<Occupancy>& space) {
                uint32_t word = 0;
                for (uint32_t layout : layoutsWithSlot) {
                    auto& occupancy = space[layout];
                    uint32_t& first = occupancy.firstWordWithRoom[logSize];
                    while (first < occupancy.words.size() && !roomIn(occupancy.words[first], logSize))
                        ++first;
                    word = std::max(word, first);
                }
                return word;
            };
            uint32_t word = findFirstWordWithRoom(occupancies);
            uint32_t cap = UINT32_MAX;
            for (uint32_t layout : layoutsWithSlot)
                cap = std::min(cap, m_inlineCap[layout]);
            uint64_t room = 0;
            auto* space = &occupancies;
            for (;; ++word) {
                uint64_t occupied = 0;
                for (uint32_t layout : layoutsWithSlot) {
                    auto& words = (*space)[layout].words;
                    if (word < words.size())
                        occupied |= words[word];
                }
                room = roomIn(occupied, logSize);
                if (!room)
                    continue;
                if (isOutOfLine || !mayBeOutOfLine.quickGet(index) || (!prepared.isOnlyReachedByTransition.quickGet(index) && word * 64 + WTF::ctz(room) + size <= cap))
                    break;
                isOutOfLine = true;
                Type type = m_program.at(slot.type);
                type.primitives |= Primitive::Absent;
                Store store = outOfLineStoreFor(index, type);
                placed.outOfLineSlots.append({ index, store });
                size = sizeOf(store);
                logSize = WTF::ctz(size);
                space = &extensions;
                word = findFirstWordWithRoom(extensions) - 1;
            }
            unsigned bit = WTF::ctz(room);
            placed.offsets[index] = word * 64 + bit;
            for (uint32_t layout : layoutsWithSlot) {
                auto& words = (*space)[layout].words;
                while (words.size() <= word)
                    words.append(0);
                words[word] |= ((1ull << size) - 1) << bit;
            }
        }
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!hasObjects(layouts[i]))
                continue;
            auto& words = occupancies[i].words;
            placed.bytes += WTF::roundUpToMultipleOf<16>(words.size() * 64 - WTF::clz(words.last()));
        }
        return placed;
    }

    void applyPlacement(const ObjectsToPlace& prepared, const PlacedObjects& placed)
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;
        for (unsigned i = 0; i < slots.size(); ++i)
            slots[i].offset = placed.offsets[i];
        for (auto [index, store] : placed.outOfLineSlots) {
            auto& slot = slots[index];
            m_outOfLineSlots.append({ index, slot.store, slot.type });
            slot.isOutOfLine = true;
            slot.type = withAbsent(slot.type);
            slot.store = store;
        }
        forEachIndexInParallel(layouts.size(), [&](unsigned i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind))
                return;
            uint32_t end = offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0);
            layout.extensionSize = 0;
            for (auto& field : layout.fields) {
                if (!exists(field))
                    continue;
                auto& slot = m_program.at(field.slot);
                field.stored = slot.type;
                field.store = slot.store;
                field.offset = slot.offset;
                field.isOutOfLine = slot.isOutOfLine;
                field.flags = slot.isOutOfLine && m_existsAtCreation.quickGet(field.slot.index()) ? field.flags | fieldMayExistWithoutStorage : field.flags & ~fieldMayExistWithoutStorage;
                (slot.isOutOfLine ? layout.extensionSize : end) = std::max(slot.isOutOfLine ? layout.extensionSize : end, slot.offset + sizeOf(slot.store));
            }
            if (layout.isAllocated)
                layout.size = WTF::roundUpToMultipleOf<16>(end);
        });
        if (!Options::verboseAOTCompilation())
            return;
        uint64_t numberOfLayouts = 0;
        uint64_t bytes = 0;
        uint64_t holeBytes = 0;
        uint64_t numberOfLayoutsWithHoles = 0;
        uint32_t maxHoleBytes = 0;
        const Layout* layoutWithMaxHoles = nullptr;
        for (auto& layout : layouts) {
            if (!hasObjects(layout))
                continue;
            uint32_t holes = layout.size - WTF::roundUpToMultipleOf<16>(bytesNeededBy(layout));
            ++numberOfLayouts;
            bytes += layout.size;
            holeBytes += holes;
            numberOfLayoutsWithHoles += !!holes;
            if (holes > maxHoleBytes) {
                maxHoleBytes = holes;
                layoutWithMaxHoles = &layout;
            }
        }
        report(numberOfLayouts, " allocated layouts of objects with ", prepared.numberOfUses, " fields in ", prepared.order.size(), " slots; an object of each takes ", bytes, " bytes in all, of which ", holeBytes, " are lost to holes in ", numberOfLayoutsWithHoles, " layouts");
        if (layoutWithMaxHoles)
            report("most is lost in ", nameOf(*layoutWithMaxHoles), ": ", maxHoleBytes, " of ", layoutWithMaxHoles->size, " bytes");
    }

    void capLayouts()
    {
        if (Options::useAOTWideReferences())
            return;
        auto& layouts = m_program.layouts;
        auto largestCells = [&](const Invocable<uint32_t(const Layout&)> auto& bytesOf) {
            Vector<uint32_t> cells(FillWith { }, layouts.size(), 0);
            for (unsigned i = 0; i < layouts.size(); ++i) {
                if (mayHaveExtension(i))
                    cells[m_group[i]] = std::max<uint32_t>(cells[m_group[i]], cellSizeFor(bytesOf(layouts[i])));
            }
            return cells;
        };
        auto bytesAllocated = [](const Layout& layout) {
            return layout.sizeWithSpecializationCapacity;
        };
        auto placeWith = [&](const Invocable<uint32_t(uint32_t group)> auto& capOf) {
            for (unsigned i = 0; i < layouts.size(); ++i) {
                m_inlineCap[i] = mayHaveExtension(i) ? capOf(m_group[i]) : noCap;
                layouts[i].hasExtension = m_inlineCap[i] != noCap;
            }
            placeObjects();
            reserveSpecializationCapacity();
        };

        static constexpr uint32_t extensionPointerEnd = offsetOfExtension + sizeOfExtensionWord;
        auto withEverything = largestCells(bytesAllocated);
        auto atCreation = largestCells([&](const Layout& layout) {
            uint32_t bytes = extensionPointerEnd;
            for (auto& field : layout.fields) {
                if (exists(field) && field.flags & fieldIsSetAtCreation && m_isAllocatedOrCastTarget.quickGet(&layout - &layouts[0]) && (m_isFirstStoredWithValue.quickGet(field.slot.index()) || !m_existsAtCreation.quickGet(field.slot.index())))
                    bytes += sizeOf(field.store);
            }
            return bytes;
        });
        placeWith([](uint32_t) {
            return extensionPointerEnd;
        });
        auto withMinimum = largestCells(bytesAllocated);
        Vector<uint32_t> requiredInlineEnd(FillWith { }, layouts.size(), 0);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!mayHaveExtension(i))
                continue;
            for (auto& field : layouts[i].fields) {
                if (exists(field) && !field.isOutOfLine)
                    requiredInlineEnd[m_group[i]] = std::max<uint32_t>(requiredInlineEnd[m_group[i]], field.offset + sizeOf(field.store));
            }
        }

        uint32_t bytesBeyondCreation = referencesBeyondCreation * sizeOf(Store::Cell32);
        Vector<uint32_t> caps(FillWith { }, layouts.size(), noCap);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            uint32_t cap = std::max<uint32_t>({ withMinimum[i], std::min<uint32_t>(cellSizeFor(std::max(atCreation[i], requiredInlineEnd[i]) + bytesBeyondCreation), maxInlineCapacity), std::min<uint32_t>(cellSizeFor(atCreation[i]), maxInlineCapacityAtCreation) });
            if (withEverything[i] > cap)
                caps[i] = cap;
        }
        for (bool fits = false; !fits;) {
            placeWith([&](uint32_t group) {
                return caps[group];
            });
            fits = true;
            for (unsigned i = 0; i < layouts.size(); ++i) {
                if (layouts[i].extensionSize <= MarkedSpace::largeCutoff || caps[m_group[i]] == noCap)
                    continue;
                caps[m_group[i]] = noCap;
                fits = false;
            }
        }
        placeEnvironments();
        reserveSpecializationCapacity();

        unsigned numberOfLayouts = 0;
        for (auto& layout : layouts)
            numberOfLayouts += layout.hasExtension;
        report(numberOfLayouts, " layouts have an extension; ", m_outOfLineSlots.size(), " slots are out of line");
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (caps[i] != noCap)
                report(nameOf(layouts[i]), " and what it becomes have ", caps[i], " bytes inline");
        }
    }

    void placeEnvironments()
    {
        for (auto& layout : m_program.layouts) {
            if (layout.kind == LayoutKind::Environment)
                placeEnvironment(layout);
        }
    }

    void placeEnvironment(Layout& layout)
    {
        Vector<unsigned> order(layout.fields.size(), [](size_t index) {
            return static_cast<unsigned>(index);
        });
        std::ranges::stable_sort(order, [&](unsigned a, unsigned b) {
            return sizeOf(m_program.at(layout.fields[a].slot).store) > sizeOf(m_program.at(layout.fields[b].slot).store);
        });
        uint32_t offset = sizeof(JSCell);
        for (unsigned i : order) {
            auto& field = layout.fields[i];
            auto& slot = m_program.at(field.slot);
            slot.offset = offset;
            field.stored = slot.type;
            field.store = slot.store;
            field.offset = offset;
            offset += sizeOf(slot.store);
        }
        layout.size = WTF::roundUpToMultipleOf<16>(offset);
        constexpr uint32_t oneByteRange = 128;
        if (!isX86() || layout.size <= oneByteRange)
            return;
        uint32_t firstField = m_firstField[&layout - &m_program.layouts[0]];
        std::ranges::stable_sort(order, [&](unsigned a, unsigned b) {
            return m_environmentFieldUseCounts[firstField + a] > m_environmentFieldUseCounts[firstField + b];
        });
        Vector<std::pair<uint32_t, uint32_t>> holes;
        Vector<uint32_t> offsets(FillWith { }, layout.fields.size(), 0u);
        uint32_t end = sizeof(JSCell);
        for (unsigned i : order) {
            uint32_t size = sizeOf(m_program.at(layout.fields[i].slot).store);
            if (!size) {
                offsets[i] = sizeof(JSCell);
                continue;
            }
            RELEASE_ASSERT(hasOneBitSet(size));
            size_t best = notFound;
            for (size_t k = 0; k < holes.size(); ++k) {
                if (holes[k].second >= size && (best == notFound || holes[k].first < holes[best].first))
                    best = k;
            }
            if (best != notFound) {
                auto [at, room] = holes[best];
                holes.removeAt(best);
                offsets[i] = at;
                for (uint32_t piece = size; piece < room; piece *= 2)
                    holes.append({ at + piece, piece });
                continue;
            }
            while (end % size) {
                uint32_t piece = end & -end;
                holes.append({ end, piece });
                end += piece;
            }
            offsets[i] = end;
            end += size;
        }
        if (WTF::roundUpToMultipleOf<16>(end) > layout.size)
            return;
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            layout.fields[i].offset = offsets[i];
            m_program.at(layout.fields[i].slot).offset = offsets[i];
        }
    }

    void widenNumberFieldsIntoPadding() { widenFieldsIntoPadding(false); }
    void widenReferenceFieldsIntoPadding() { widenFieldsIntoPadding(true); }

    void widenFieldsIntoPadding(bool widensReferences)
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;
        auto isNarrow = [&](Store store) {
            return widensReferences ? store == Store::Cell32 || store == Store::OptionalCell32 : store == Store::Int32OrNumberBox;
        };
        BitVector mustStayNarrow;
        mustStayNarrow.ensureSize(slots.size());
        if (widensReferences) {
            for (auto& layout : layouts) {
                for (auto& field : layout.fields) {
                    if (exists(field) && m_program.lazyClosureNames.get(field.name.index()))
                        mustStayNarrow.quickSet(field.slot.index());
                }
            }
        }
        Vector<uint32_t> candidates;
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (widensReferences && m_program.at(slots[i].type).primitives & (Primitive::String | Primitive::Symbol | Primitive::LiteralString))
                mustStayNarrow.quickSet(i);
            if (isNarrow(slots[i].store) && !slots[i].isOutOfLine && !mustStayNarrow.quickGet(i))
                candidates.append(i);
        }
        if (candidates.isEmpty())
            return;

        auto bytesAt = [](uint32_t offset, uint32_t size) {
            return ((1ull << size) - 1) << (offset % 64);
        };
        Vector<Vector<uint64_t, 4>> occupied(layouts.size());
        Vector<std::pair<uint32_t, uint32_t>> slotLayouts;
        Vector<std::pair<uint32_t, uint32_t>> layoutsBySmallSlot;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind) || !layout.isAllocated)
                continue;
            auto& words = occupied[i];
            words.fill(0, layout.size / 64 + 1);
            words[0] = bytesAt(0, offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0));
            if (layout.offsetOfExtraProperties && layout.offsetOfExtraProperties < layout.size)
                words[layout.offsetOfExtraProperties / 64] |= bytesAt(layout.offsetOfExtraProperties, sizeOfExtraPropertiesWord);
            for (auto& field : layout.fields) {
                if (!exists(field) || field.isOutOfLine)
                    continue;
                words[field.offset / 64] |= bytesAt(field.offset, sizeOf(field.store));
                if (isNarrow(field.store) && !mustStayNarrow.quickGet(field.slot.index()))
                    slotLayouts.append({ field.slot.index(), i });
                if (!widensReferences && sizeOf(field.store) == sizeOf(Store::Int32OrNumberBox))
                    layoutsBySmallSlot.append({ field.slot.index(), i });
            }
        }
        std::ranges::sort(slotLayouts);
        std::ranges::sort(layoutsBySmallSlot);

        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> nameGroup;
        Vector<uint32_t> slotGroup;
        slotGroup.fill(UINT32_MAX, slots.size());
        Vector<Vector<uint32_t, 1>> groups;
        BitVector isDeferred;
        for (auto& layout : layouts) {
            if (layout.kind != LayoutKind::Record)
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field) || !isNarrow(field.store) || mustStayNarrow.quickGet(field.slot.index()))
                    continue;
                uint32_t group = nameGroup.ensure(field.name.index(), [&] {
                    groups.append({ });
                    return groups.size() - 1;
                }).iterator->value;
                if (field.isOutOfLine)
                    isDeferred.set(group);
                uint32_t& thisGroup = slotGroup[field.slot.index()];
                if (thisGroup == UINT32_MAX) {
                    thisGroup = group;
                    groups[group].append(field.slot.index());
                } else if (thisGroup != group) {
                    isDeferred.set(group);
                    isDeferred.set(thisGroup);
                }
            }
        }
        for (uint32_t index : candidates) {
            if (slotGroup[index] != UINT32_MAX)
                continue;
            slotGroup[index] = groups.size();
            groups.append({ index });
        }
        Vector<uint32_t> order;
        Vector<uint64_t> groupWeight;
        groupWeight.fill(0, groups.size());
        for (uint32_t group = 0; group < groups.size(); ++group) {
            if (isDeferred.get(group))
                continue;
            order.append(group);
            for (uint32_t index : groups[group])
                groupWeight[group] = std::max<uint64_t>(groupWeight[group], m_accessWeight[index]);
        }
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            return std::tuple(groupWeight[b], groups[a][0]) < std::tuple(groupWeight[a], groups[b][0]);
        });

        uint32_t narrow = sizeOf(Store::Int32OrNumberBox);
        uint32_t wide = sizeOf(Store::Double);
        auto layoutsOf = [&](uint32_t index) {
            return std::ranges::equal_range(slotLayouts, index, { }, &std::pair<uint32_t, uint32_t>::first);
        };
        uint32_t minOffset = WTF::roundUpToMultipleOf(wide, offsetOfFirstField);
        Vector<uint32_t> firstPossiblyFreeOffset(FillWith { }, layouts.size(), minOffset);
        auto move = [&](uint32_t index, uint32_t from, uint32_t sizeAtOffset, uint32_t to, uint32_t sizeHere) {
            for (auto& use : layoutsOf(index)) {
                occupied[use.second][from / 64] &= ~bytesAt(from, sizeAtOffset);
                occupied[use.second][to / 64] |= bytesAt(to, sizeHere);
                firstPossiblyFreeOffset[use.second] = std::min(firstPossiblyFreeOffset[use.second], from / wide * wide);
            }
        };
        auto roomFor = [&](uint32_t index) -> std::optional<uint32_t> {
            auto& slot = slots[index];
            auto layoutsWithSlot = layoutsOf(index);
            uint32_t end = UINT32_MAX;
            for (auto& use : layoutsWithSlot)
                end = std::min({ end, layouts[use.second].size, m_inlineCap[use.second] });
            auto isFree = [&](uint32_t offset) {
                return std::ranges::all_of(layoutsWithSlot, [&](auto& use) {
                    uint64_t word = occupied[use.second][offset / 64];
                    if (slot.offset / 64 == offset / 64)
                        word &= ~bytesAt(slot.offset, narrow);
                    return !(word & bytesAt(offset, wide));
                });
            };
            uint32_t start = minOffset;
            for (auto& use : layoutsWithSlot) {
                uint32_t& first = firstPossiblyFreeOffset[use.second];
                while (first + wide <= layouts[use.second].size && occupied[use.second][first / 64] & bytesAt(first, wide))
                    first += wide;
                start = std::max(start, first);
            }
            if (uint32_t own = slot.offset / wide * wide; own >= minOffset && own < start && own + wide <= end && isFree(own))
                return own;
            for (uint32_t offset = start; offset + wide <= end; offset += wide) {
                if (isFree(offset))
                    return offset;
            }
            return std::nullopt;
        };
        struct MovedNeighbor {
            uint32_t slot;
            uint32_t from;
            uint32_t to;
        };
        auto moveNeighbor = [&](uint32_t index, uint32_t from, uint32_t to) {
            for (auto& use : std::ranges::equal_range(layoutsBySmallSlot, index, { }, &std::pair<uint32_t, uint32_t>::first)) {
                RELEASE_ASSERT(!(occupied[use.second][to / 64] & bytesAt(to, narrow)) && (occupied[use.second][from / 64] & bytesAt(from, narrow)) == bytesAt(from, narrow));
                occupied[use.second][from / 64] &= ~bytesAt(from, narrow);
                occupied[use.second][to / 64] |= bytesAt(to, narrow);
                firstPossiblyFreeOffset[use.second] = std::min(firstPossiblyFreeOffset[use.second], from / wide * wide);
            }
            slots[index].offset = to;
        };
        auto roomAfterMovingNeighbor = [&](uint32_t index, const Vector<std::pair<uint32_t, uint32_t>, 4>& alreadyMoved, Vector<MovedNeighbor, 2>& movedNeighbors) -> std::optional<uint32_t> {
            uint32_t own = slots[index].offset / wide * wide;
            uint32_t beside = slots[index].offset ^ narrow;
            if (own < minOffset)
                return std::nullopt;
            uint32_t neighbor = none;
            for (auto& use : layoutsOf(index)) {
                if (own + wide > std::min(layouts[use.second].size, m_inlineCap[use.second]))
                    return std::nullopt;
                if (!(occupied[use.second][beside / 64] & bytesAt(beside, narrow)))
                    continue;
                uint32_t held = none;
                for (auto& field : layouts[use.second].fields) {
                    bool isElsewhereNow = alreadyMoved.containsIf([&](auto& moved) {
                        return moved.first == field.slot.index();
                    });
                    if (exists(field) && !field.isOutOfLine && !isElsewhereNow && slots[field.slot.index()].offset == beside && sizeOf(slots[field.slot.index()].store) == narrow)
                        held = field.slot.index();
                }
                if (held == none || (neighbor != none && neighbor != held))
                    return std::nullopt;
                neighbor = held;
            }
            if (neighbor == none)
                return std::nullopt;
            auto layoutsWithNeighbor = std::ranges::equal_range(layoutsBySmallSlot, neighbor, { }, &std::pair<uint32_t, uint32_t>::first);
            uint32_t end = UINT32_MAX;
            for (auto& use : layoutsWithNeighbor)
                end = std::min({ end, layouts[use.second].size, m_inlineCap[use.second] });
            for (uint32_t offset = WTF::roundUpToMultipleOf(narrow, offsetOfFirstField); offset + narrow <= end; offset += narrow) {
                if (offset / wide * wide == own)
                    continue;
                bool isFree = std::ranges::all_of(layoutsWithNeighbor, [&](auto& use) {
                    return !(occupied[use.second][offset / 64] & bytesAt(offset, narrow));
                });
                if (!isFree)
                    continue;
                moveNeighbor(neighbor, beside, offset);
                movedNeighbors.append({ neighbor, beside, offset });
                return own;
            }
            return std::nullopt;
        };
        unsigned widenedCount = 0;
        BitVector isWidened;
        auto widen = [&](uint32_t group, bool mayMoveNeighbors) {
            Vector<std::pair<uint32_t, uint32_t>, 4> found;
            Vector<MovedNeighbor, 2> movedNeighbors;
            bool hasRoom = true;
            for (uint32_t index : groups[group]) {
                if (layoutsOf(index).empty())
                    continue;
                auto offset = roomFor(index);
                if (!offset && mayMoveNeighbors)
                    offset = roomAfterMovingNeighbor(index, found, movedNeighbors);
                if (!offset) {
                    hasRoom = false;
                    break;
                }
                move(index, slots[index].offset, narrow, *offset, wide);
                found.append({ index, *offset });
            }
            if (!hasRoom || found.isEmpty()) {
                for (auto [index, offset] : found | std::views::reverse)
                    move(index, offset, wide, slots[index].offset, narrow);
                for (auto [slot, from, to] : movedNeighbors | std::views::reverse)
                    moveNeighbor(slot, to, from);
                return false;
            }
            for (auto [index, offset] : found) {
                auto& slot = slots[index];
                if (widensReferences)
                    m_hasWideReferences.quickSet(index);
                else
                    m_isPredictedInt32.quickClear(index);
                slot.store = storeFor(m_program.at(slot.type), m_isPredictedInt32.quickGet(index), m_hasWideReferences.quickGet(index));
                RELEASE_ASSERT(sizeOf(slot.store) == wide);
                slot.offset = offset;
                ++widenedCount;
            }
            isWidened.set(group);
            return true;
        };
        for (bool didWiden = true; didWiden;) {
            didWiden = false;
            for (uint32_t group : order) {
                if (!isWidened.get(group) && widen(group, false))
                    didWiden = true;
            }
        }
        if (!widensReferences) {
            for (uint32_t group : order) {
                bool isWantedAsInteger = std::ranges::any_of(groups[group], [&](uint32_t index) {
                    return m_integerUseCounts[index];
                });
                bool isBetterWide = std::ranges::any_of(groups[group], [&](uint32_t index) {
                    return m_doubleUseCounts[index] || m_rangeTestedStoreCounts[index];
                });
                if (!isWidened.get(group) && !isWantedAsInteger && isBetterWide)
                    widen(group, true);
            }
        }
        for (auto& layout : layouts) {
            if (!isObjectLayout(layout.kind))
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field))
                    continue;
                auto& slot = m_program.at(field.slot);
                field.store = slot.store;
                field.offset = slot.offset;
            }
        }
        report(candidates.size() - widenedCount, " of ", candidates.size(), widensReferences ? " slots of references" : " slots of numbers that are expected to be int32s", " take 32 bits: the others have found 64 that were free");
    }

    void reserveExtraPropertiesSlot()
    {
        if (Options::useAOTWideReferences())
            return;
        auto& layouts = m_program.layouts;
        Vector<uint32_t> group(layouts.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        auto find = [&](uint32_t layout) {
            while (group[layout] != layout)
                layout = group[layout] = group[group[layout]];
            return layout;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            for (LayoutID target : layouts[i].specializations) {
                auto [minimum, other] = std::minmax({ find(i), find(target.index()) });
                group[other] = minimum;
            }
        }
        struct Room {
            uint32_t end { LayoutData::extraPropertiesOffsetLimit };
            std::array<uint64_t, LayoutData::extraPropertiesOffsetLimit / 64> occupied { };
        };
        Vector<Room> rooms(layouts.size());
        auto occupy = [](Room& room, uint32_t first, uint32_t end) {
            for (uint32_t byte = first; byte < std::min(end, LayoutData::extraPropertiesOffsetLimit); ++byte)
                room.occupied[byte / 64] |= 1ull << byte % 64;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!hasObjects(layout))
                continue;
            Room& room = rooms[find(i)];
            room.end = std::min<uint32_t>(room.end, cellSizeFor(layout.size));
            occupy(room, 0, offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0));
            for (auto& field : layout.fields) {
                if (exists(field) && !field.isOutOfLine)
                    occupy(room, field.offset, field.offset + sizeOf(field.store));
            }
        }
        unsigned numberOfLayouts = 0;
        unsigned numberOfLayoutsWithRoom = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!hasObjects(layout))
                continue;
            ++numberOfLayouts;
            Room& room = rooms[find(i)];
            for (uint32_t offset = offsetOfFirstField; offset + sizeOfExtraPropertiesWord <= room.end && !layout.offsetOfExtraProperties; offset += sizeOfExtraPropertiesWord) {
                if (!(room.occupied[offset / 64] >> offset % 64 & ((1ull << sizeOfExtraPropertiesWord) - 1)))
                    layout.offsetOfExtraProperties = offset;
            }
            numberOfLayoutsWithRoom += !!layout.offsetOfExtraProperties;
        }
        report(numberOfLayoutsWithRoom, " of ", numberOfLayouts, " allocated object layouts have four free bytes for the extra properties reference");
    }

    void reserveSpecializationCapacity()
    {
        Vector<Layout*> specialized;
        for (auto& layout : m_program.layouts) {
            layout.sizeWithSpecializationCapacity = layout.size;
            if (!layout.specializations.isEmpty())
                specialized.append(&layout);
        }
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (Layout* layout : specialized) {
                for (LayoutID specialization : layout->specializations) {
                    uint32_t room = m_program.at(specialization).sizeWithSpecializationCapacity;
                    if (room <= layout->sizeWithSpecializationCapacity)
                        continue;
                    layout->sizeWithSpecializationCapacity = room;
                    didChange = true;
                }
            }
        }
    }

    Vector<uint32_t> transitionClosure(uint32_t layout, BitVector& seen) const
    {
        Vector<uint32_t> closure { layout };
        seen.clearAll();
        seen.set(layout);
        for (unsigned next = 0; next < closure.size(); ++next) {
            for (LayoutID target : m_program.layouts[closure[next]].specializations) {
                if (!seen.set(target.index()))
                    closure.append(target.index());
            }
        }
        return closure;
    }

    static std::optional<int32_t> integerOf(const Literal& literal)
    {
        if (literal.string || literal.number != static_cast<int32_t>(literal.number))
            return std::nullopt;
        return static_cast<int32_t>(literal.number);
    }

    std::span<const Literal> constantsOf(uint32_t layout, StringID name) const
    {
        auto field = m_program.layouts[layout].fieldNamed(name);
        return field ? m_program.layouts[layout].fields[*field].literals.span() : std::span<const Literal>();
    }

    std::optional<int32_t> constantOf(uint32_t layout, StringID name) const
    {
        auto constants = constantsOf(layout, name);
        return constants.size() == 1 ? integerOf(constants[0]) : std::nullopt;
    }

    struct Family {
        uint32_t base;
        StringID discriminant;
        int32_t first;
        Vector<LayoutID> finalLayouts;
        Vector<uint32_t> baseSizes;
    };

    Family* familyOf(const Allocation& allocation)
    {
        if (allocation.discriminant == UINT16_MAX)
            return nullptr;
        StringID name = m_program.at(allocation.layout).fields[allocation.discriminant].name;
        for (auto& family : m_families) {
            if (family.base == allocation.layout.index() && family.discriminant == name)
                return &family;
        }
        return nullptr;
    }

    void findFamilies()
    {
        static constexpr int64_t maxConstants = 4096;
        auto& layouts = m_program.layouts;
        BitVector seen;
        Vector<Vector<uint32_t>> closures;
        for (auto& allocation : m_program.allocations) {
            if (allocation.discriminant == UINT16_MAX || familyOf(allocation))
                continue;
            uint32_t base = allocation.layout.index();
            RELEASE_ASSERT(isObjectLayout(layouts[base].kind) && exists(layouts[base].fields[allocation.discriminant]));
            StringID name = layouts[base].fields[allocation.discriminant].name;
            Vector<uint32_t> closure = transitionClosure(base, seen);
            int64_t first = std::numeric_limits<int32_t>::max();
            int64_t last = std::numeric_limits<int32_t>::min();
            bool overlaps = false;
            for (uint32_t layout : closure) {
                overlaps |= layouts[layout].discriminant != UINT16_MAX && layouts[layout].fields[layouts[layout].discriminant].name != name;
                for (auto& constant : constantsOf(layout, name)) {
                    if (auto integer = integerOf(constant)) {
                        first = std::min<int64_t>(first, *integer);
                        last = std::max<int64_t>(last, *integer);
                    }
                }
            }
            if (overlaps || first > last || last - first >= maxConstants)
                continue;
            for (uint32_t layout : closure) {
                layouts[layout].discriminant = static_cast<uint16_t>(*layouts[layout].fieldNamed(name));
                layouts[layout].isAbstract = !constantOf(layout, name);
            }
            m_families.append(Family { base, name, static_cast<int32_t>(first), Vector<LayoutID>(FillWith { }, last - first + 1, LayoutID()), { } });
            closures.append(WTF::move(closure));
        }
        uint64_t pruned = 0;
        for (unsigned i = 0; i < m_families.size(); ++i) {
            for (uint32_t layout : closures[i]) {
                auto constant = constantOf(layout, m_families[i].discriminant);
                if (!constant)
                    continue;
                pruned += layouts[layout].specializations.removeAllMatching([&](LayoutID target) {
                    return constantOf(target.index(), m_families[i].discriminant) != constant;
                });
            }
        }
        for (auto& layout : layouts) {
            if (layout.discriminant != UINT16_MAX)
                continue;
            pruned += layout.specializations.removeAllMatching([&](LayoutID target) {
                return m_program.at(target).discriminant != UINT16_MAX;
            });
        }
        for (unsigned i = 0; i < m_families.size(); ++i) {
            auto& family = m_families[i];
            Vector<Vector<uint32_t>> candidates(family.finalLayouts.size());
            closures[i] = transitionClosure(family.base, seen);
            for (uint32_t layout : closures[i]) {
                if (auto constant = constantOf(layout, family.discriminant))
                    candidates[*constant - family.first].append(layout);
            }
            unsigned numberOfFinalLayouts = 0;
            for (unsigned k = 0; k < candidates.size(); ++k) {
                for (uint32_t candidate : candidates[k]) {
                    transitionClosure(candidate, seen);
                    if (!std::ranges::all_of(candidates[k], [&](uint32_t other) { return seen.get(other) || std::ranges::binary_search(supertypesOf(candidate), other); }))
                        continue;
                    family.finalLayouts[k] = LayoutID(candidate);
                    ++numberOfFinalLayouts;
                    break;
                }
            }
            if (closures[i].size() > 1)
                report("family of ", nameOf(layouts[family.base]), " by ", nameOf(family.discriminant), ": ", closures[i].size(), " layouts, ", family.finalLayouts.size(), " constants from ", family.first, ", ", numberOfFinalLayouts, " with a final layout");
        }
        report(m_families.size(), " families; ", pruned, " transitions pruned because they would change a discriminant");
    }

    void expandAllocations()
    {
        auto& allocations = m_program.allocations;
        for (unsigned i = 0, end = allocations.size(); i < end; ++i) {
            Family* family = familyOf(allocations[i]);
            if (!family || m_program.layouts[family->base].specializations.isEmpty())
                continue;
            allocations[i].firstLiteral = family->first;
            allocations[i].numberOfLiterals = family->finalLayouts.size();
            allocations[i].firstLiteralAllocation = allocations.size();
            auto& base = m_program.layouts[family->base];
            for (unsigned k = 0; k <= family->finalLayouts.size(); ++k) {
                Allocation entry = allocations[i];
                entry.numberOfLiterals = 0;
                if (LayoutID chosen = k < family->finalLayouts.size() ? family->finalLayouts[k] : LayoutID()) {
                    auto remap = [&](uint16_t& field) {
                        field = static_cast<uint16_t>(*m_program.at(chosen).fieldNamed(base.fields[field].name));
                    };
                    entry.layout = chosen;
                    remap(entry.discriminant);
                    for (uint16_t& field : entry.order)
                        remap(field);
                }
                allocations.append(WTF::move(entry));
            }
        }
    }

    void sizeBaseAllocations(Family& family)
    {
        auto& layouts = m_program.layouts;
        BitVector seen;
        Vector<uint32_t> closure = transitionClosure(family.base, seen);
        unsigned count = family.finalLayouts.size();
        family.baseSizes.fill(layouts[family.base].size, count + 1);
        uint32_t abstractSize = layouts[family.base].size;
        uint32_t lastAbstractSize = 0;
        BitVector isListed;
        auto countsFor = [&](uint32_t layout, std::span<const Literal> constants, bool lists) {
            for (auto& constant : constants) {
                auto integer = integerOf(constant);
                if (!integer || *integer < family.first || static_cast<int64_t>(*integer) - family.first >= count)
                    continue;
                family.baseSizes[*integer - family.first] = std::max(family.baseSizes[*integer - family.first], layouts[layout].size);
                if (lists)
                    isListed.set(*integer - family.first);
            }
        };
        for (uint32_t layout : closure) {
            auto constants = constantsOf(layout, family.discriminant);
            if (!constants.empty()) {
                countsFor(layout, constants, true);
                continue;
            }
            abstractSize = std::max(abstractSize, layouts[layout].size);
            if (layouts[layout].specializations.isEmpty())
                lastAbstractSize = std::max(lastAbstractSize, layouts[layout].size);
            for (LayoutID subtype : layouts[layout].subtypes)
                countsFor(layout, constantsOf(subtype.index(), family.discriminant), false);
        }
        for (unsigned k = 0; k <= count; ++k) {
            family.baseSizes[k] = isListed.get(k) ? std::max(family.baseSizes[k], lastAbstractSize) : abstractSize;
        }
    }

    void sizeAllocations()
    {
        auto& allocations = m_program.allocations;
        for (auto& allocation : allocations) {
            allocation.size = m_program.at(allocation.layout).sizeWithSpecializationCapacity;
            if (allocation.layout == m_program.closureLayout)
                allocation.size = WTF::roundUpToMultipleOf<16>(Closure::allocationSize(allocation.numberOfCaptures));
        }
        for (auto& family : m_families)
            sizeBaseAllocations(family);
        for (unsigned i = 0; i < allocations.size(); ++i) {
            if (!allocations[i].numberOfLiterals)
                continue;
            Family& family = *familyOf(allocations[i]);
            uint64_t sum = 0;
            for (unsigned k = 0; k <= allocations[i].numberOfLiterals; ++k) {
                auto& entry = allocations[allocations[i].firstLiteralAllocation + k];
                if (entry.layout == allocations[i].layout)
                    entry.size = family.baseSizes[k];
                sum += entry.size;
            }
            report("allocation ", i, " of ", nameOf(m_program.at(allocations[i].layout)), " by ", nameOf(family.discriminant), ": mean size ", sum / (allocations[i].numberOfLiterals + 1), ", ", family.baseSizes.last(), " for any other integer, ", allocations[i].size, " if it is not an integer");
        }
    }

    void collectOrders()
    {
        Vector<BitVector> usedNumbers(m_initialStores.isEmpty() ? 0 : m_program.layouts.size());
        for (auto& initial : m_initialStores) {
            if (!initial.count)
                continue;
            BitVector taken;
            forEachLayoutInitializedBy(initial, [&](uint32_t layout) {
                taken.merge(usedNumbers[layout]);
            });
            uint32_t number = 0;
            while (taken.get(number))
                ++number;
            forEachLayoutInitializedBy(initial, [&](uint32_t index) {
                auto& layout = m_program.layouts[index];
                usedNumbers[index].set(number);
                if (layout.orders.size() <= number)
                    layout.orders.grow(number + 1);
                for (unsigned k = 0; k < initial.count; ++k)
                    layout.orders[number].append(static_cast<uint16_t>(*layout.fieldNamed(initial.names[k])));
                RELEASE_ASSERT(layout.orders.size() < keysAreInLayoutOrder);
            });
            RELEASE_ASSERT(initial.count <= maxFieldsInOrder);
            m_program.functions.atKeepingValueIndex(initial.function).numberOfInitialStores = initial.count;
            m_program.functions.atKeepingValueIndex(initial.function).orderWordAfterInitialStores = orderWordOf(number, initial.count);
            report(nameOf(std::as_const(m_program).functions[initial.function].name), " stores ", initial.count, " of its first ", initial.names.size(), " properties without a check, to a fresh ", nameOf(m_program.at(initial.supertype)), "; order ", number);
        }
        for (auto& allocation : m_program.allocations) {
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind))
                continue;
            auto [expected, initialized] = existingFieldOrder(layout, allocation);
            if (initialized > maxFieldsInOrder)
                continue;
            bool isKnown = false;
            for (auto& order : layout.orders) {
                isKnown = startsWith(order, expected);
                if (!isKnown && startsWith(expected, order)) {
                    order = expected;
                    isKnown = true;
                }
                if (isKnown)
                    break;
            }
            if (!isKnown)
                layout.orders.append(WTF::move(expected));
            RELEASE_ASSERT(layout.orders.size() < keysAreInLayoutOrder);
        }
        for (auto& layout : m_program.layouts) {
            if (layout.kind != LayoutKind::Record || !layout.isAllocated || !layout.orders.isEmpty())
                continue;
            Vector<uint16_t> declared;
            for (unsigned i = 0; i < layout.fields.size(); ++i) {
                if (exists(layout.fields[i]))
                    declared.append(static_cast<uint16_t>(i));
            }
            if (!declared.isEmpty() && declared.size() <= maxFieldsInOrder)
                layout.orders.append(WTF::move(declared));
        }
        static constexpr OpcodeSet operations { Opcode::CallBuiltin, Opcode::Operation };
        auto spreads = collectInParallel<std::pair<uint32_t, uint32_t>>(m_program.functions.size(), [&](unsigned i, auto& collected) {
            if (!m_program.functions.mayContain(i, operations))
                return;
            auto& values = std::as_const(m_program).functions[i].values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                auto& value = values[index];
                if (value.opcode != Opcode::CallBuiltin && value.opcode != Opcode::Operation)
                    continue;
                if (languageOperationOf(m_program, value) == LanguageOperation::CloneObjectWith && value.prediction())
                    collected.append({ i, index });
            }
        });
        for (auto [i, index] : spreads)
            collectSpreadOrders(std::as_const(m_program).functions[i], std::as_const(m_program).functions[i].values[index]);
    }

    void collectSpreadOrders(const Function& function, const Value& spread)
    {
        static constexpr unsigned maxOrdersPerLayout = 4;
        LayoutID targetID = m_program.at(spread.prediction()).withoutPrimitives(Primitive::UndefinedOrNull).singleLayout();
        if (!targetID || m_program.at(targetID).kind != LayoutKind::Record)
            return;
        auto& target = m_program.at(targetID);
        auto targetFieldNamed = [&](StringID name) -> std::optional<uint16_t> {
            auto field = target.fieldNamed(name);
            if (!field || !exists(target.fields[*field]))
                return std::nullopt;
            return static_cast<uint16_t>(*field);
        };
        Vector<uint16_t, 4> appendedFields;
        for (size_t i = 1; i < spread.arguments.size(); ++i) {
            auto& name = function.at(spread.arguments[i]);
            auto field = name.opcode == Opcode::String ? targetFieldNamed(name.string()) : std::nullopt;
            if (!field)
                return;
            appendedFields.append(*field);
        }
        Vector<Vector<uint16_t>> spreadOrders;
        for (LayoutID supertype : m_program.typeOf(function, spread.arguments[0]).objectLayouts) {
            for (LayoutID subtype : m_program.at(supertype).subtypes) {
                auto& source = m_program.at(subtype);
                if (source.kind != LayoutKind::Record)
                    continue;
                for (auto& sourceOrder : source.orders) {
                    Vector<uint16_t> order;
                    bool hasAllFields = true;
                    for (uint16_t index : sourceOrder) {
                        auto field = targetFieldNamed(source.fields[index].name);
                        hasAllFields = !!field;
                        if (!hasAllFields)
                            break;
                        order.append(*field);
                    }
                    if (!hasAllFields)
                        continue;
                    for (uint16_t field : appendedFields) {
                        if (!order.contains(field))
                            order.append(field);
                    }
                    if (order.size() <= maxFieldsInOrder)
                        spreadOrders.append(WTF::move(order));
                }
            }
        }
        for (auto& order : spreadOrders) {
            bool isKnown = target.orders.containsIf([&](auto& known) {
                return startsWith(known, order);
            });
            if (!isKnown && target.orders.size() < maxOrdersPerLayout)
                target.orders.append(WTF::move(order));
        }
    }

    struct Candidate {
        enum Kind : uint8_t {
            TypeSubtypes,
            SupertypeSubtypes,
            SubtypesWithField,
            ClassInstances,
            RetagSources,
            SubtypesKeptByCast,
        };

        Kind kind;
        uint32_t index;
        Fingerprint layouts;
        uint64_t weight;
        uint32_t test { none };
    };

    static constexpr uint32_t shapesPerBlock = 256;
    static constexpr uint32_t maxTestableBlocks = 128;
    static constexpr uint64_t bytesSavedPerBlockTest = 6;
    static constexpr uint32_t numberOfShapesWithShortConstants = 128;
    static constexpr uint32_t firstShapeWithLongConstant = 4096;

    struct Test {
        uint32_t firstLayout;
        uint32_t numberOfLayouts;
        uint64_t weight { 0 };
        bool hasResult { false };
        ShapeTest result { };
    };

    void collectSubtypeShapes()
    {
        for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
            auto& supertype = m_program.layouts[index];
            if (!isObjectLayout(supertype.kind) || supertype.acceptsUntypedObjects || mayDespecialize(m_program, LayoutID(index)))
                continue;
            for (LayoutID subtype : supertype.subtypes) {
                if (uint32_t shape = m_program.at(subtype).shape)
                    supertype.subtypeShapes.append(shape);
            }
            std::ranges::sort(supertype.subtypeShapes);
        }
    }

    void numberShapes()
    {
        auto& layouts = m_program.layouts;
        Vector<Candidate> candidates;
        auto addCandidate = [&](Candidate::Kind kind, uint32_t index, uint64_t weight, const auto& listed, const auto& indexOf) {
            Fingerprint fingerprint;
            for (auto layout : listed)
                fingerprint.add(m_layoutHashes[indexOf(layout)]);
            candidates.append(Candidate { kind, index, fingerprint, weight });
        };
        auto idIndex = [](LayoutID layout) {
            return layout.index();
        };
        Vector<LayoutID> supertypeSubtypes;
        Vector<LayoutID> earlierSupertypeSubtypes;
        auto unionSubtypesOfType = [&](uint32_t type) {
            supertypeSubtypes.shrink(0);
            for (LayoutID supertype : m_program.at(TypeID(type)).objectLayouts) {
                auto& subtypes = m_program.at(supertype).subtypes;
                std::swap(supertypeSubtypes, earlierSupertypeSubtypes);
                supertypeSubtypes.resize(earlierSupertypeSubtypes.size() + subtypes.size());
                supertypeSubtypes.shrink(std::ranges::set_union(earlierSupertypeSubtypes, subtypes, supertypeSubtypes.begin()).out - supertypeSubtypes.begin());
            }
        };

        for (unsigned i = 0; i < m_typeTests.size(); ++i) {
            auto& objectLayouts = m_program.at(TypeID(i)).objectLayouts;
            if (!m_typeTests[i])
                continue;
            for (LayoutID supertype : objectLayouts)
                m_supertypeTests[supertype.index()] += m_typeTests[i];
            if (objectLayouts.size() < 2)
                continue;
            unionSubtypesOfType(i);
            addCandidate(Candidate::TypeSubtypes, i, m_typeTests[i], supertypeSubtypes, idIndex);
        }
        Vector<uint32_t> supertypeCandidate(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!isObjectLayout(layouts[i].kind))
                continue;
            supertypeCandidate[i] = candidates.size();
            addCandidate(Candidate::SupertypeSubtypes, i, m_supertypeTests[i], layouts[i].subtypes, idIndex);
        }
        uint32_t firstSetCandidate = candidates.size();
        for (unsigned i = 0; i < m_subtypeSets.size(); ++i) {
            auto& missingFields = m_missingFields[m_subtypeSets[i].listedFor];
            candidates.append(Candidate { Candidate::SubtypesWithField, i, m_subtypesWith[m_firstField[missingFields.layout] + missingFields.field], 0 });
        }
        for (auto& missingFields : m_missingFields)
            candidates[firstSetCandidate + missingFields.subtypesWithField].weight += missingFields.numberOfAccesses;
        Vector<Vector<uint32_t>> classInstances(layouts.size());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].kind != LayoutKind::Instance || !layouts[i].isAllocated)
                continue;
            for (LayoutID ancestor(i); ancestor; ancestor = m_program.at(ancestor).base)
                classInstances[ancestor.index()].append(i);
        }
        Vector<uint32_t> classCandidate(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].kind != LayoutKind::Instance)
                continue;
            classCandidate[i] = candidates.size();
            addCandidate(Candidate::ClassInstances, i, m_classTests[i], classInstances[i], std::identity { });
        }
        Vector<uint32_t> retagSourcesCandidate(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (retagSourcesOf(i).empty())
                continue;
            retagSourcesCandidate[i] = candidates.size();
            addCandidate(Candidate::RetagSources, i, m_coercionsToLayout[i], retagSourcesOf(i), std::identity { });
        }
        Vector<LayoutID> keptSubtypes;
        auto findSubtypesKeptByCastTo = [&](uint32_t supertype) {
            keptSubtypes.resize(layouts[supertype].subtypes.size());
            keptSubtypes.shrink(std::ranges::set_difference(layouts[supertype].subtypes, retaggedSubtypesOf(supertype), keptSubtypes.begin()).out - keptSubtypes.begin());
        };
        Vector<uint32_t> castTestCandidate(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (retaggedSubtypesOf(i).empty())
                continue;
            castTestCandidate[i] = candidates.size();
            findSubtypesKeptByCastTo(i);
            addCandidate(Candidate::SubtypesKeptByCast, i, m_supertypeTests[i], keptSubtypes, idIndex);
        }

        Vector<uint32_t> layoutsWithOtherPrototype;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (isObjectLayout(layouts[i].kind) && layouts[i].isAllocated && layouts[i].mayHaveOtherPrototype)
                layoutsWithOtherPrototype.append(i);
        }

        Vector<uint32_t> sortedCandidates(candidates.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(sortedCandidates, [&](uint32_t a, uint32_t b) {
            return candidates[a].layouts != candidates[b].layouts ? candidates[a].layouts < candidates[b].layouts : a < b;
        });
        Vector<uint32_t> listedLayouts;
        auto layoutsOf = [&](const Test& test) {
            return listedLayouts.span().subspan(test.firstLayout, test.numberOfLayouts);
        };
        Vector<Test> unsortedTests;
        const Candidate* listedCandidate = nullptr;
        for (uint32_t index : sortedCandidates) {
            auto& candidate = candidates[index];
            if (!listedCandidate || listedCandidate->layouts != candidate.layouts) {
                listedCandidate = &candidate;
                unsortedTests.append(Test { static_cast<uint32_t>(listedLayouts.size()), candidate.layouts.count });
                switch (candidate.kind) {
                case Candidate::TypeSubtypes:
                    unionSubtypesOfType(candidate.index);
                    for (LayoutID subtype : supertypeSubtypes)
                        listedLayouts.append(subtype.index());
                    break;
                case Candidate::SupertypeSubtypes:
                    for (LayoutID subtype : layouts[candidate.index].subtypes)
                        listedLayouts.append(subtype.index());
                    break;
                case Candidate::SubtypesWithField:
                    listedLayouts.append(subtypesIn(m_subtypeSets[candidate.index]));
                    break;
                case Candidate::ClassInstances:
                    listedLayouts.appendVector(classInstances[candidate.index]);
                    break;
                case Candidate::RetagSources:
                    listedLayouts.append(retagSourcesOf(candidate.index));
                    break;
                case Candidate::SubtypesKeptByCast:
                    findSubtypesKeptByCastTo(candidate.index);
                    for (LayoutID subtype : keptSubtypes)
                        listedLayouts.append(subtype.index());
                    break;
                }
                RELEASE_ASSERT(listedLayouts.size() - unsortedTests.last().firstLayout == candidate.layouts.count);
            }
            unsortedTests.last().weight += candidate.weight;
            unsortedTests.last().hasResult |= candidate.kind != Candidate::TypeSubtypes;
            candidate.test = unsortedTests.size() - 1;
        }
        Vector<uint32_t> sortedTests(unsortedTests.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        sortLexicographically(sortedTests.mutableSpan(), [&](uint32_t test, uint32_t position) {
            return position < unsortedTests[test].numberOfLayouts ? listedLayouts[unsortedTests[test].firstLayout + position] + 1 : 0;
        });
        Vector<Test> tests;
        Vector<uint32_t> testIndex(unsortedTests.size());
        for (uint32_t test : sortedTests) {
            testIndex[test] = tests.size();
            tests.append(unsortedTests[test]);
        }
        report(candidates.size(), " layout sets are tested, ", tests.size(), " of them distinct, listing ", listedLayouts.size(), " layouts");
        Vector<uint32_t> order;
        for (unsigned i = 0; i < tests.size(); ++i) {
            if (tests[i].numberOfLayouts > 1)
                order.append(i);
        }
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            if (tests[a].weight != tests[b].weight)
                return tests[a].weight > tests[b].weight;
            if (tests[a].numberOfLayouts != tests[b].numberOfLayouts)
                return tests[a].numberOfLayouts < tests[b].numberOfLayouts;
            return a < b;
        });

        Vector<uint32_t> layoutLeaf(FillWith { }, layouts.size(), none);
        Vector<uint32_t> leafLayout;
        for (uint32_t test : order) {
            for (uint32_t layout : layoutsOf(tests[test])) {
                if (layoutLeaf[layout] != none)
                    continue;
                layoutLeaf[layout] = leafLayout.size();
                leafLayout.append(layout);
            }
        }

        Vector<uint64_t> singleLayoutTestWeight(FillWith { }, layouts.size(), 0);
        for (auto& test : tests) {
            if (test.numberOfLayouts == 1)
                singleLayoutTestWeight[layoutsOf(test)[0]] += test.weight;
        }
        Vector<uint32_t> layoutsTestedAlone;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (isObjectLayout(layouts[i].kind) && layoutLeaf[i] == none)
                layoutsTestedAlone.append(i);
        }
        std::ranges::stable_sort(layoutsTestedAlone, [&](uint32_t a, uint32_t b) {
            return singleLayoutTestWeight[a] > singleLayoutTestWeight[b];
        });

        uint32_t nextShape = 1;
        auto number = [&](Layout& layout) {
            if (layout.isAllocated)
                layout.shape = nextShape++;
        };
        size_t numberOfLayoutsTestedAloneInFrontier = 0;
        if (!leafLayout.isEmpty()) {
            PQTree tree(leafLayout.size());
            Vector<uint64_t> leafKey(leafLayout.size(), [](size_t leaf) {
                return static_cast<uint64_t>(none) << 32 | leaf;
            });
            uint64_t rejectedCount = 0;
            Vector<uint32_t> leaves;
            for (uint32_t test : order) {
                leaves.shrink(0);
                for (uint32_t layout : layoutsOf(tests[test]))
                    leaves.append(layoutLeaf[layout]);
                if (tree.reduce(leaves.span()))
                    continue;
                for (uint32_t leaf : leaves)
                    leafKey[leaf] = std::min(leafKey[leaf], rejectedCount << 32 | leaf);
                ++rejectedCount;
            }
            Vector<uint32_t> frontier;
            for (uint32_t leaf : tree.frontier(leafKey.span()))
                frontier.append(leafLayout[leaf]);

            Vector<uint32_t> groupOf(FillWith { }, layouts.size(), none);
            {
                struct Group {
                    uint32_t first;
                    uint32_t count { 0 };
                    uint64_t weight { 0 };
                    uint64_t numberOfShapes { 0 };
                };
                Vector<uint32_t> placeOf(FillWith { }, layouts.size(), none);
                for (uint32_t place = 0; place < frontier.size(); ++place)
                    placeOf[frontier[place]] = place;
                Vector<int32_t> numberOfTestsSpanning(FillWith { }, frontier.size() + 1, 0);
                Vector<uint64_t> weightOfTestsBeginningAt(FillWith { }, frontier.size(), 0);
                for (uint32_t test : order) {
                    uint32_t first = none;
                    uint32_t last = 0;
                    for (uint32_t layout : layoutsOf(tests[test])) {
                        first = std::min(first, placeOf[layout]);
                        last = std::max(last, placeOf[layout]);
                    }
                    ++numberOfTestsSpanning[first + 1];
                    --numberOfTestsSpanning[last + 1];
                    weightOfTestsBeginningAt[first] += tests[test].weight;
                }
                Vector<Group> groups;
                int32_t spanning = 0;
                for (uint32_t place = 0; place < frontier.size(); ++place) {
                    spanning += numberOfTestsSpanning[place];
                    if (!spanning)
                        groups.append(Group { place });
                    auto& group = groups.last();
                    ++group.count;
                    group.weight += weightOfTestsBeginningAt[place] + singleLayoutTestWeight[frontier[place]];
                    group.numberOfShapes += layouts[frontier[place]].isAllocated;
                }
                while (numberOfLayoutsTestedAloneInFrontier < layoutsTestedAlone.size() && singleLayoutTestWeight[layoutsTestedAlone[numberOfLayoutsTestedAloneInFrontier]]) {
                    uint32_t layout = layoutsTestedAlone[numberOfLayoutsTestedAloneInFrontier++];
                    groups.append(Group { static_cast<uint32_t>(frontier.size()), 1, singleLayoutTestWeight[layout], layouts[layout].isAllocated });
                    frontier.append(layout);
                }
                std::ranges::stable_sort(groups, [](const Group& a, const Group& b) {
                    return a.weight * std::max<uint64_t>(b.numberOfShapes, 1) > b.weight * std::max<uint64_t>(a.numberOfShapes, 1);
                });
                Vector<uint32_t> sorted;
                for (uint32_t index = 0; index < groups.size(); ++index) {
                    for (uint32_t layout : frontier.subspan(groups[index].first, groups[index].count)) {
                        groupOf[layout] = index;
                        sorted.append(layout);
                    }
                }
                frontier = WTF::move(sorted);
            }
            const Vector<uint32_t> frontierByTestsPerShape = frontier;

            struct Numbering {
                Vector<uint32_t> layoutShape;
                uint32_t nextShape { 1 };
                uint64_t estimatedInstructions { 0 };
            };
            auto blockStartAtOrAfter = [](uint32_t shape) {
                return WTF::roundUpToMultipleOf<shapesPerBlock>(shape - 1) + 1;
            };
            auto numberSequence = [&](const Vector<uint32_t>& sequence, uint32_t headLength, bool usesBlocks) {
                Vector<uint32_t> layoutIndex(FillWith { }, layouts.size(), none);
                for (uint32_t place = 0; place < sequence.size(); ++place)
                    layoutIndex[sequence[place]] = place;
                auto extentOf = [&](const Test& test) {
                    uint32_t first = none;
                    uint32_t last = 0;
                    for (uint32_t layout : layoutsOf(test)) {
                        first = std::min(first, layoutIndex[layout]);
                        last = std::max(last, layoutIndex[layout]);
                    }
                    return std::pair { first, last };
                };
                BitVector isInBlock(sequence.size());
                BitVector beginsBlock(sequence.size());
                BitVector endsBlock(sequence.size());
                if (usesBlocks) {
                    Vector<int32_t> numberOfSetsSpanning(FillWith { }, sequence.size() + 2, 0);
                    for (uint32_t test : order) {
                        auto [first, last] = extentOf(tests[test]);
                        if (!tests[test].hasResult || last - first + 1 == tests[test].numberOfLayouts)
                            continue;
                        ++numberOfSetsSpanning[first + 1];
                        --numberOfSetsSpanning[last + 1];
                    }
                    for (uint32_t place = 1; place < numberOfSetsSpanning.size(); ++place)
                        numberOfSetsSpanning[place] += numberOfSetsSpanning[place - 1];
                    for (uint32_t test : order) {
                        if (!tests[test].hasResult || tests[test].numberOfLayouts > shapesPerBlock)
                            continue;
                        auto [first, last] = extentOf(tests[test]);
                        if (last - first + 1 != tests[test].numberOfLayouts || first < headLength)
                            continue;
                        bool isFree = true;
                        for (uint32_t place = first; place <= last; ++place)
                            isFree &= layouts[sequence[place]].isAllocated && isObjectLayout(layouts[sequence[place]].kind) && !isInBlock.quickGet(place);
                        uint64_t unusedNumbers = shapesPerBlock / 2 + shapesPerBlock - tests[test].numberOfLayouts;
                        uint64_t dataBytes = unusedNumbers * sizeof(uint32_t) + (shapesPerBlock / 2 * numberOfSetsSpanning[first] + (shapesPerBlock - tests[test].numberOfLayouts) * numberOfSetsSpanning[last + 1]) / 8;
                        if (!isFree || tests[test].weight * bytesSavedPerBlockTest < dataBytes)
                            continue;
                        for (uint32_t place = first; place <= last; ++place)
                            isInBlock.quickSet(place);
                        beginsBlock.quickSet(first);
                        endsBlock.quickSet(last);
                    }
                }
                Numbering result;
                result.layoutShape.fill(0, layouts.size());
                bool isAligned = false;
                for (uint32_t place = 0; place < sequence.size(); ++place) {
                    if (headLength && place == headLength)
                        result.nextShape = blockStartAtOrAfter(result.nextShape);
                    if (beginsBlock.quickGet(place) && result.nextShape > numberOfShapesWithShortConstants && blockStartAtOrAfter(result.nextShape) / shapesPerBlock < maxTestableBlocks) {
                        result.nextShape = blockStartAtOrAfter(result.nextShape);
                        isAligned = true;
                    }
                    if (layouts[sequence[place]].isAllocated)
                        result.layoutShape[sequence[place]] = result.nextShape++;
                    if (endsBlock.quickGet(place) && std::exchange(isAligned, false))
                        result.nextShape = blockStartAtOrAfter(result.nextShape);
                }
                if (headLength == sequence.size())
                    result.nextShape = blockStartAtOrAfter(result.nextShape);

                Vector<uint32_t> numberUpTo(FillWith { }, result.nextShape, 0);
                for (uint32_t layout : sequence) {
                    if (result.layoutShape[layout])
                        numberUpTo[result.layoutShape[layout]] = 1;
                }
                for (uint32_t shape = 1; shape < result.nextShape; ++shape)
                    numberUpTo[shape] += numberUpTo[shape - 1];
                for (uint32_t index : order) {
                    auto& test = tests[index];
                    uint32_t first = none;
                    uint32_t last = 0;
                    for (uint32_t layout : layoutsOf(test)) {
                        first = std::min(first, result.layoutShape[layout]);
                        last = std::max(last, result.layoutShape[layout]);
                    }
                    if (!first)
                        continue;
                    bool isRange = numberUpTo[last] - numberUpTo[first - 1] == test.numberOfLayouts;
                    if (!test.hasResult) {
                        result.estimatedInstructions += test.weight * (isRange ? 4 : 8);
                        continue;
                    }
                    if (!isRange) {
                        result.estimatedInstructions += test.weight * (last - first < 64 ? 7 : 9);
                        continue;
                    }
                    uint32_t blockFirst = (last - 1) / shapesPerBlock * shapesPerBlock + 1;
                    uint32_t blockLast = std::min(blockFirst + shapesPerBlock - 1, result.nextShape - 1);
                    bool hasBlockTest = (last - 1) / shapesPerBlock < maxTestableBlocks && (numberUpTo[blockLast] == test.numberOfLayouts || (first >= blockFirst && numberUpTo[blockLast] - numberUpTo[blockFirst - 1] == test.numberOfLayouts));
                    result.estimatedInstructions += test.weight * (hasBlockTest || first == 1 ? 2 : 4);
                    if (!hasBlockTest && last >= firstShapeWithLongConstant)
                        result.estimatedInstructions += test.weight;
                }
                for (uint32_t layout : sequence) {
                    if (result.layoutShape[layout] >= firstShapeWithLongConstant)
                        result.estimatedInstructions += singleLayoutTestWeight[layout];
                }
                return result;
            };

            Numbering best = numberSequence(frontier, 0, false);
            auto consider = [&](Numbering&& numbering) {
                if (numbering.estimatedInstructions < best.estimatedInstructions)
                    best = WTF::move(numbering);
            };
            consider(numberSequence(frontier, 0, true));
            for (bool isReversed : { false, true }) {
                if (isReversed)
                    frontier.reverse();
                Vector<uint32_t> frontierIndex(FillWith { }, layouts.size(), none);
                for (uint32_t place = 0; place < frontier.size(); ++place)
                    frontierIndex[frontier[place]] = place;
                for (uint32_t test : order) {
                    if (!tests[test].hasResult)
                        continue;
                    uint32_t first = none;
                    uint32_t last = 0;
                    bool mayBeHead = true;
                    for (uint32_t layout : layoutsOf(tests[test])) {
                        mayBeHead &= layouts[layout].isAllocated && isObjectLayout(layouts[layout].kind);
                        first = std::min(first, frontierIndex[layout]);
                        last = std::max(last, frontierIndex[layout]);
                    }
                    uint32_t length = tests[test].numberOfLayouts;
                    if (!mayBeHead || last - first + 1 != length || length > shapesPerBlock * maxTestableBlocks / 2)
                        continue;
                    if (tests[test].weight * bytesSavedPerBlockTest < (WTF::roundUpToMultipleOf<shapesPerBlock>(length) - length) * sizeof(uint32_t))
                        break;
                    uint32_t group = groupOf[frontier[first]];
                    uint32_t groupBegin = first;
                    while (groupBegin && groupOf[frontier[groupBegin - 1]] == group)
                        --groupBegin;
                    uint32_t groupEnd = last + 1;
                    while (groupEnd < frontier.size() && groupOf[frontier[groupEnd]] == group)
                        ++groupEnd;
                    Vector<uint32_t> before(frontier.subspan(groupBegin, first - groupBegin));
                    std::ranges::reverse(before);
                    Vector<uint32_t> sequence(frontier.subspan(first, length));
                    sequence.append(frontier.subspan(last + 1, groupEnd - last - 1));
                    sequence.appendVector(before);
                    for (uint32_t layout : frontierByTestsPerShape) {
                        if (groupOf[layout] != group)
                            sequence.append(layout);
                    }
                    consider(numberSequence(sequence, length, true));
                    break;
                }
            }
            for (uint32_t layout : frontier)
                layouts[layout].shape = best.layoutShape[layout];
            nextShape = best.nextShape;
        }
        for (uint32_t layout : layoutsTestedAlone.span().subspan(numberOfLayoutsTestedAloneInFrontier))
            number(layouts[layout]);
        uint64_t singleLayoutTestSites = 0;
        uint64_t singleLayoutTestSitesWithShortConstant = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!layouts[i].shape)
                continue;
            singleLayoutTestSites += singleLayoutTestWeight[i];
            if (layouts[i].shape < firstShapeWithLongConstant)
                singleLayoutTestSitesWithShortConstant += singleLayoutTestWeight[i];
        }
        uint64_t multipleLayoutTestSites = 0;
        uint64_t multipleLayoutTestSitesWithShortConstant = 0;
        for (uint32_t test : order) {
            uint32_t last = 0;
            for (uint32_t layout : layoutsOf(tests[test]))
                last = std::max(last, layouts[layout].shape);
            multipleLayoutTestSites += tests[test].weight;
            if (last < firstShapeWithLongConstant)
                multipleLayoutTestSitesWithShortConstant += tests[test].weight;
        }
        report(singleLayoutTestSites, " sites test for a single shape, ", singleLayoutTestSitesWithShortConstant, " of them for a shape number below 4096; ", multipleLayoutTestSites, " sites test for several shapes, ", multipleLayoutTestSitesWithShortConstant, " of them end below 4096; ", leafLayout.size(), " layouts are in tests of several shapes, ", layoutsTestedAlone.size(), " are not");
        for (auto& layout : layouts) {
            if (layout.kind == LayoutKind::Environment)
                number(layout);
        }
        number(m_program.at(m_program.closureLayout));
        m_program.numberOfShapes = nextShape - 1;
        m_program.shapeCountPrefixSums.fill(0, nextShape);
        for (auto& layout : layouts) {
            if (layout.shape)
                m_program.shapeCountPrefixSums[layout.shape] = 1;
        }
        for (uint32_t shape = 1; shape < nextShape; ++shape)
            m_program.shapeCountPrefixSums[shape] += m_program.shapeCountPrefixSums[shape - 1];
        auto chooseBlockTest = [&](ShapeTest& test, uint32_t numberOfLayouts) {
            uint32_t block = (test.last() - 1) / shapesPerBlock;
            if (!test.first || block >= maxTestableBlocks || numberOfLayouts < 2)
                return;
            uint32_t blockLast = std::min((block + 1) * shapesPerBlock, m_program.numberOfShapes);
            if (m_program.shapeCountInRange(1, blockLast) == numberOfLayouts)
                test.passesEarlierBlocks = true;
            else if (test.first <= block * shapesPerBlock || m_program.shapeCountInRange(block * shapesPerBlock + 1, blockLast) != numberOfLayouts)
                return;
            test.blockByte = static_cast<uint8_t>(Typed::shapeWordOf(test.last()) >> 8);
        };

        uint64_t weight = 0;
        uint64_t rangesWeight = 0;
        unsigned numberOfRanges = 0;
        unsigned numberOfSets = 0;
        uint32_t numberOfBits = 0;
        Vector<uint32_t> bits;
        for (auto& test : tests) {
            if (!test.numberOfLayouts)
                continue;
            uint32_t first = none;
            uint32_t last = 0;
            for (uint32_t layout : layoutsOf(test)) {
                first = std::min(first, layouts[layout].shape);
                last = std::max(last, layouts[layout].shape);
            }
            if (test.numberOfLayouts > 1)
                weight += test.weight;
            if (first ? m_program.shapeCountInRange(first, last) == test.numberOfLayouts : last + 1 == test.numberOfLayouts) {
                test.result = ShapeTest { first, last - first + 1, none };
                chooseBlockTest(test.result, test.numberOfLayouts);
                if (test.numberOfLayouts > 1) {
                    ++numberOfRanges;
                    rangesWeight += test.weight;
                }
                continue;
            }
            if (!test.hasResult)
                continue;
            test.result = ShapeTest { first, last - first + 1, numberOfBits };
            for (uint32_t layout : layoutsOf(test))
                bits.append(numberOfBits + last - layouts[layout].shape);
            numberOfBits += WTF::roundUpToMultipleOf<32>(test.result.count);
            ++numberOfSets;
        }
        m_program.shapeSetBits.ensureSize(numberOfBits);
        for (uint32_t bit : bits)
            m_program.shapeSetBits.quickSet(bit);
        auto resultOf = [&](uint32_t candidate) {
            return tests[testIndex[candidates[candidate].test]].result;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (supertypeCandidate[i] != none)
                layouts[i].test = resultOf(supertypeCandidate[i]);
            if (classCandidate[i] != none)
                layouts[i].instances = resultOf(classCandidate[i]);
            if (retagSourcesCandidate[i] != none)
                layouts[i].retagSources = resultOf(retagSourcesCandidate[i]);
            layouts[i].castTest = castTestCandidate[i] != none ? resultOf(castTestCandidate[i]) : layouts[i].test;
        }
        for (auto& missingFields : m_missingFields)
            layouts[missingFields.layout].fields[missingFields.field].subtypesWithField = resultOf(firstSetCandidate + missingFields.subtypesWithField);
        for (uint32_t index : layoutsWithOtherPrototype) {
            if (layouts[index].kind != LayoutKind::Instance)
                continue;
            for (LayoutID ancestor(index); ancestor; ancestor = m_program.at(ancestor).base)
                layouts[ancestor.index()].hasInstancesWithOtherPrototype = true;
        }
        auto instructionsOf = [](const ShapeTest& test) -> unsigned {
            if (test.isRange())
                return test.count == 1 || test.blockByte ? 2 : 4;
            return test.count <= 64 ? 7 : 9;
        };
        constexpr uint32_t maxShapesPerSet = 4096;
        Vector<uint32_t> passing;
        using ShapeTestKind = Program::ShapeTestKind;
        for (unsigned i = 0; i < m_typeTests.size(); ++i) {
            auto& objectLayouts = m_program.at(TypeID(i)).objectLayouts;
            if (objectLayouts.size() < 2)
                continue;
            for (ShapeTestKind asked : { ShapeTestKind::Members, ShapeTestKind::SubtypesKeptByCast, ShapeTestKind::RetagSources }) {
                if (!(asked == ShapeTestKind::RetagSources ? m_coercionsToType[i] : m_typeTests[i]))
                    continue;
                uint64_t key = Program::multiLayoutShapeTestKey(objectLayouts, asked);
                if (m_program.multiLayoutShapeTests.contains(key))
                    continue;
                unsigned oneByOne = 0;
                bool hasSubtypeWithoutShape = false;
                passing.shrink(0);
                for (LayoutID supertype : objectLayouts) {
                    auto& layout = layouts[supertype.index()];
                    auto& test = asked == ShapeTestKind::Members ? layout.test : asked == ShapeTestKind::SubtypesKeptByCast ? layout.castTest : layout.retagSources;
                    if (test.neverPasses())
                        continue;
                    oneByOne += instructionsOf(test);
                    hasSubtypeWithoutShape |= !test.first;
                    if (asked == ShapeTestKind::RetagSources) {
                        for (uint32_t source : retagSourcesOf(supertype.index())) {
                            if (uint32_t shape = layouts[source].shape)
                                passing.append(shape);
                        }
                        continue;
                    }
                    for (LayoutID subtype : layout.subtypes) {
                        if (uint32_t shape = m_program.at(subtype).shape; shape && m_program.passes(test, shape))
                            passing.append(shape);
                    }
                }
                if (hasSubtypeWithoutShape || passing.isEmpty())
                    continue;
                std::ranges::sort(passing);
                passing.shrink(std::ranges::unique(passing).begin() - passing.begin());
                uint32_t first = passing.first();
                uint32_t last = passing.last();
                bool isRange = m_program.shapeCountInRange(first, last) == passing.size();
                ShapeTest result { first, last - first + 1, isRange ? none : numberOfBits };
                if (isRange)
                    chooseBlockTest(result, passing.size());
                if (oneByOne <= instructionsOf(result) || (!isRange && result.count > maxShapesPerSet))
                    continue;
                if (!isRange) {
                    numberOfBits += WTF::roundUpToMultipleOf<32>(result.count);
                    m_program.shapeSetBits.ensureSize(numberOfBits);
                    for (uint32_t shape : passing)
                        m_program.shapeSetBits.quickSet(result.set + last - shape);
                }
                m_program.multiLayoutShapeTests.add(key, Program::MultiLayoutShapeTest { objectLayouts, result, static_cast<uint32_t>(passing.size()) });
            }
        }
        report(m_program.numberOfShapes, " shapes; ", order.size(), " multi-layout shape tests, of weight ", weight, ": ", numberOfRanges, " are a range, of weight ", rangesWeight, "; ", numberOfSets, " shape sets in ", numberOfBits / 8, " bytes");
    }

    void rejectVariablesWithUncheckedReads(const Function& function, Vector<uint8_t>& isCandidate)
    {
        auto candidateReadBy = [&](ValueID id) -> std::optional<uint32_t> {
            auto& value = function.at(id);
            if (value.opcode != Opcode::GetModuleVariable || !WTF::atomicLoad(&isCandidate[value.variable().index()], std::memory_order_relaxed))
                return std::nullopt;
            return value.variable().index();
        };
        auto reject = [&](uint32_t variable) {
            WTF::atomicStore(&isCandidate[variable], static_cast<uint8_t>(0), std::memory_order_relaxed);
        };
        bool hasCandidateRead = false;
        static constexpr OpcodeSet variableReads { Opcode::GetModuleVariable };
        std::as_const(m_program).functions.forEachIndexedValue(indexOf(function), variableReads, [&](ValueID id) {
            auto variable = candidateReadBy(id);
            if (!variable)
                return;
            if (function.at(id).rep == Rep::JSValue)
                hasCandidateRead = true;
            else
                reject(*variable);
        });
        if (!hasCandidateRead)
            return;
        auto readComparedBitwise = [&](ValueID id, bool provesDefinedWhenFalse) -> ValueID {
            auto& comparison = function.at(id);
            if (comparison.opcode == Opcode::IsUndefinedOrNull)
                return candidateReadBy(comparison.child(0)) ? comparison.child(0) : ValueID { };
            if (comparison.opcode != Opcode::StrictEq)
                return { };
            for (auto [read, constant] : { std::pair { comparison.child(0), comparison.child(1) }, std::pair { comparison.child(1), comparison.child(0) } }) {
                Opcode opcode = function.at(constant).opcode;
                if ((opcode == Opcode::Undefined || (!provesDefinedWhenFalse && (opcode == Opcode::Null || opcode == Opcode::Boolean))) && candidateReadBy(read))
                    return read;
            }
            return { };
        };
        Vector<unsigned> numberOfPredecessors(FillWith { }, function.blocks.size(), 0u);
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets)
                ++numberOfPredecessors[target.block.index()];
        }
        auto readCheckedBy = [&](const Terminator& terminator) -> ValueID {
            if (terminator.kind != TerminatorKind::Branch || terminator.targets[0].block == terminator.targets[1].block)
                return { };
            return readComparedBitwise(terminator.value, true);
        };
        UncheckedKeyHashMap<uint32_t, Vector<unsigned, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> blocksWhereDefined;
        for (auto& block : function.blocks) {
            ValueID read = readCheckedBy(block.terminator);
            unsigned target = read ? block.terminator.targets[1].block.index() : 0;
            if (read && numberOfPredecessors[target] == 1)
                blocksWhereDefined.add(read.index(), Vector<unsigned, 1> { }).iterator->value.append(target);
        }
        Vector<unsigned> dominators;
        auto isKnownDefinedIn = [&](ValueID read, unsigned block) {
            auto found = blocksWhereDefined.find(read.index());
            if (found == blocksWhereDefined.end())
                return false;
            if (dominators.isEmpty())
                dominators = immediateDominatorsOf(function);
            for (;; block = dominators[block]) {
                if (found->value.contains(block))
                    return true;
                if (!block || dominators[block] == UINT_MAX)
                    return false;
            }
        };
        for (unsigned index = 0; index < function.blocks.size(); ++index) {
            auto& block = function.blocks[index];
            bool isCold = block.terminator.kind == TerminatorKind::Unreachable || block.terminator.kind == TerminatorKind::Throw;
            auto use = [&](ValueID id) {
                if (auto variable = candidateReadBy(id); variable && !isCold && !isKnownDefinedIn(id, index))
                    reject(*variable);
            };
            for (ValueID id : block.values) {
                if (!readComparedBitwise(id, false))
                    function.at(id).forEachChild(use);
            }
            auto& terminator = block.terminator;
            if (terminator.value)
                use(terminator.value);
            ValueID checked = readCheckedBy(terminator);
            for (unsigned target = 0; target < terminator.targets.size(); ++target) {
                for (ValueID argument : terminator.targets[target].arguments) {
                    if (target != 1 || argument != checked)
                        use(argument);
                }
            }
        }
    }

    void findVariablesStoringUndefinedAsZero()
    {
        if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
            return;
        auto& variables = m_program.variables;
        unsigned numberOfCandidates = 0;
        Vector<uint8_t> isCandidate(variables.size(), [&](size_t index) {
            auto& variable = variables[index];
            bool result = variable.startsUndefined && !variable.startsZero && !variable.isNeverReadUninitialized && variable.store == Store::JSValue;
            numberOfCandidates += result;
            return static_cast<uint8_t>(result);
        });
        for (auto& evaluation : m_program.moduleEvaluations) {
            isCandidate[evaluation.isEvaluated.index()] = 0;
            if (evaluation.error)
                isCandidate[evaluation.error.index()] = 0;
        }
        for (auto& layout : m_program.layouts) {
            if (layout.environment)
                continue;
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods) {
                    if (!method.name && method.key < variables.size())
                        isCandidate[method.key] = 0;
                }
            }
        }
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned index) {
            rejectVariablesWithUncheckedReads(std::as_const(m_program).functions[index], isCandidate);
        });
        unsigned numberFound = 0;
        for (size_t index = 0; index < variables.size(); ++index) {
            variables[index].storesUndefinedAsZero = isCandidate[index];
            numberFound += isCandidate[index];
        }
        report(numberFound, " of ", numberOfCandidates, " boxed variables that start as undefined encode undefined as zero");
    }

    void placeVariables()
    {
        auto& variables = m_program.variables;
        auto groupOf = [](const Variable& variable) -> unsigned {
            if (variable.isNeverReadUninitialized || variable.startsZero || variable.storesUndefinedAsZero)
                return 5 + sizeof(uint64_t) / sizeOf(variable.store);
            switch (variable.store) {
            case Store::JSValue:
            case Store::LiteralString:
                return variable.startsUndefined ? 0 : 5;
            case Store::OptionalInt64:
                return variable.startsUndefined ? 1 : 3;
            case Store::OptionalDouble:
                return variable.startsUndefined ? 2 : 4;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        };
        for (auto& variable : variables) {
            Type type = variable.type ? m_program.at(variable.type) : Type::anything();
            if (variable.mayBeReadUninitialized())
                type.primitives |= Primitive::Absent;
            variable.store = storeOf(type);
            variable.hasCard = true;
        }
        findVariablesStoringUndefinedAsZero();
        constexpr unsigned filledGroupCount = 5;
        BitVector isHostModule(m_program.modules.size());
        auto addModuleOf = [&](FunctionID function) {
            if (ModuleID module = function ? std::as_const(m_program).at(function).module : ModuleID())
                isHostModule.set(module.index());
        };
        for (auto& module : m_program.builtinModules)
            addModuleOf(module.second);
        for (auto& builtin : m_program.builtinFunctions)
            addModuleOf(builtin.function);
        for (FunctionID function : m_program.preludeFunctions)
            addModuleOf(function);
        BitVector mayRunBeforeProgram(m_program.functions.size());
        Vector<unsigned> functionsToVisit;
        auto reach = [&](FunctionID function) {
            if (function && !mayRunBeforeProgram.set(function.index()))
                functionsToVisit.append(function.index());
        };
        auto hostFunctions = collectInParallel<uint32_t>(m_program.functions.size(), [&](unsigned index, Vector<uint32_t>& collected) {
            if (ModuleID module = std::as_const(m_program).functions[index].module; module && isHostModule.get(module.index()))
                collected.append(index);
        });
        for (uint32_t index : hostFunctions)
            reach(FunctionID(index));
        BitVector isInitializedAtCreation(variables.size());
        for (unsigned index = 0; index < variables.size(); ++index) {
            if (!variables[index].module || isHostModule.get(variables[index].module.index()))
                isInitializedAtCreation.set(index);
        }
        unsigned allocationsAndReleasesBeforeProgram = 0;
        unsigned variablesInitializedAtCreationCount = 0;
        auto initializeReusedEnvironmentVariables = [&](LayoutID layout) {
            if (VariableID reused = layout ? m_program.at(layout).reuseVariable : VariableID()) {
                ++allocationsAndReleasesBeforeProgram;
                variablesInitializedAtCreationCount += !isInitializedAtCreation.set(reused.index());
            }
        };
        while (!functionsToVisit.isEmpty()) {
            FunctionID id(functionsToVisit.takeLast());
            auto& function = std::as_const(m_program).at(id);
            for (FunctionID other : { m_program.genericEntrypoints.get(id.index()), function.rampForClosures, function.resumeFunction, function.startFunction, function.worker })
                reach(other);
            if (auto row = m_program.moduleEvaluationIndexByFunction.find(id.index()); row != m_program.moduleEvaluationIndexByFunction.end()) {
                auto& evaluation = m_program.moduleEvaluations[row->value];
                isInitializedAtCreation.set(evaluation.isEvaluated.index());
                if (evaluation.error)
                    isInitializedAtCreation.set(evaluation.error.index());
                for (FunctionID callee : evaluation.callees)
                    reach(callee);
            }
            for (auto& value : function.values) {
                switch (value.opcode) {
                case Opcode::CallDirect:
                case Opcode::FunctionReference:
                case Opcode::NewClosure:
                case Opcode::NewClosureOnce:
                    reach(value.function());
                    break;
                case Opcode::Construct:
                    reach(m_program.constructBody(value));
                    break;
                case Opcode::Cast:
                    reach(m_program.castBody(value));
                    break;
                case Opcode::GetModuleVariable:
                case Opcode::PutModuleVariable:
                    isInitializedAtCreation.set(value.variable().index());
                    break;
                case Opcode::NewObjectWithShape:
                    if (value.allocation() < m_program.allocations.size())
                        initializeReusedEnvironmentVariables(m_program.allocations[value.allocation()].layout);
                    [[fallthrough]];
                case Opcode::PutField:
                    initializeReusedEnvironmentVariables(value.layout());
                    break;
                default:
                    break;
                }
            }
        }
        constexpr unsigned moduleEvaluationFlagsOffset = 32;
        constexpr unsigned moduleEvaluationErrorsOffset = 33;
        BitVector isModuleEvaluationFlag(variables.size());
        BitVector isModuleEvaluationError(variables.size());
        auto startsAsZero = [&](VariableID variable) {
            return variable && variables[variable.index()].store == Store::JSValue && groupOf(variables[variable.index()]) >= filledGroupCount;
        };
        for (auto& evaluation : m_program.moduleEvaluations) {
            if (startsAsZero(evaluation.isEvaluated)) {
                isModuleEvaluationFlag.set(evaluation.isEvaluated.index());
                variables[evaluation.isEvaluated.index()].store = Store::OptionalBoolean;
                variables[evaluation.isEvaluated.index()].startsZero = true;
            }
            if (startsAsZero(evaluation.error))
                isModuleEvaluationError.set(evaluation.error.index());
        }
        auto placeOf = [&](unsigned index) {
            if (isModuleEvaluationFlag.get(index))
                return moduleEvaluationFlagsOffset;
            if (isModuleEvaluationError.get(index))
                return moduleEvaluationErrorsOffset;
            unsigned group = groupOf(variables[index]);
            return group < filledGroupCount && !isInitializedAtCreation.get(index) ? group : group + filledGroupCount;
        };
        constexpr unsigned numberOfPlaces = moduleEvaluationErrorsOffset + 1;
        std::array<unsigned, numberOfPlaces> nextInChain { };
        Vector<uint8_t> places(variables.size(), [&](size_t index) {
            unsigned place = placeOf(index);
            RELEASE_ASSERT(place < numberOfPlaces);
            ++nextInChain[place];
            return static_cast<uint8_t>(place);
        });
        unsigned precedingCount = 0;
        for (unsigned& next : nextInChain)
            precedingCount += std::exchange(next, precedingCount);
        Vector<unsigned> order(FillWith { }, variables.size(), 0u);
        for (unsigned index = 0; index < variables.size(); ++index)
            order[nextInChain[places[index]]++] = index;
        uint32_t offset = 0;
        auto place = [&](bool isLateInitialized) {
            for (unsigned i : order) {
                if ((places[i] < filledGroupCount) != isLateInitialized)
                    continue;
                offset = static_cast<uint32_t>(WTF::roundUpToMultipleOf(sizeOf(variables[i].store), offset));
                variables[i].offset = offset;
                offset += sizeOf(variables[i].store);
            }
        };
        place(true);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            offset = WTF::roundUpToMultipleOf<16 * KB>(offset);
        m_program.sizeOfLazilyInitializedStatics = offset;
        place(false);
        report(variables.size(), " variables in ", offset, " bytes");
        report(m_program.sizeOfLazilyInitializedStatics, " bytes of them are filled when the program is first entered");
        report(allocationsAndReleasesBeforeProgram, " births and releases of an environment that is used again stand in code that may run ahead of the program; ", variablesInitializedAtCreationCount, " variables are filled at birth for that alone");
        auto& builtins = Builtins::singleton();
        m_program.globalPropertyCachesOffset = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        for (auto& numbers : m_program.globalPropertyCacheNumbers)
            numbers.fill(0, m_program.strings.size());
        m_program.globalThisPropertyCacheNumbers.fill(0, m_program.strings.size());
        Vector<Vector<std::pair<uint8_t, uint32_t>>> functionFreeNames(m_program.functions.size());
        Vector<Vector<uint64_t>> functionGlobalMembers(m_program.functions.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            static constexpr OpcodeSet globalAccesses { Opcode::GetGlobal, Opcode::GetGlobalForTypeOf, Opcode::PutGlobal };
            if (!m_program.functions.mayContain(i, globalAccesses))
                return;
            auto& function = std::as_const(m_program).functions[i];
            static constexpr OpcodeSet globalAccessesAndReadsByName = globalAccesses | OpcodeSet { Opcode::GetById };
            std::as_const(m_program).functions.forEachIndexedValue(i, globalAccessesAndReadsByName, [&](ValueID id) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::GetById) {
                    if (auto& base = function.at(value.child(0)); base.opcode == Opcode::GetGlobal && m_program.isGlobalThisName(base.string()))
                        functionFreeNames[i].append({ static_cast<uint8_t>(2), value.string().index() });
                    else if (base.opcode == Opcode::GetGlobal && (function.readsHostObjectMember || m_program.isHostObjectFunction(base.string(), value.string())))
                        functionGlobalMembers[i].append(static_cast<uint64_t>(base.string().index()) << 32 | value.string().index());
                    return;
                }
                if (BuiltinName name = m_program.builtinName(value.string()); builtins.isGlobal(name) && !m_program.isExtended(name))
                    return;
                auto& sourceFunction = value.inlineFrame ? std::as_const(m_program).at(function.inlineFrames[value.inlineFrame].callee) : function;
                bool readsGlobalObject = value.opcode != Opcode::PutGlobal && sourceFunction.has(Format::FunctionIsInBuiltinModule);
                functionFreeNames[i].append({ static_cast<uint8_t>(readsGlobalObject ? 2 : value.opcode == Opcode::PutGlobal), value.string().index() });
            });
        });
        for (auto& freeNames : functionFreeNames) {
            for (auto [table, name] : freeNames) {
                if (uint32_t& number = table == 2 ? m_program.globalThisPropertyCacheNumbers[name] : m_program.globalPropertyCacheNumbers[table][name]; !number)
                    number = ++m_program.numberOfGlobalPropertyCaches;
            }
        }
        for (auto& members : functionGlobalMembers) {
            for (uint64_t member : members) {
                if (uint32_t& number = m_program.globalMemberCacheNumbers.add(member, 0).iterator->value; !number)
                    number = ++m_program.numberOfGlobalPropertyCaches;
            }
        }
        m_program.staticFieldSlotsOffset = m_program.globalPropertyCachesOffset + m_program.numberOfGlobalPropertyCaches * sizeof(GlobalPropertyCache);
        m_program.sizeOfStatics = m_program.staticFieldSlotsOffset + m_program.numberOfStaticFieldSlots * sizeof(int32_t);
        report(m_program.numberOfGlobalPropertyCaches, " caches of free names");
    }

    uint64_t encodedConstant(const Value& constant, bool isShared) const
    {
        switch (constant.opcode) {
        case Opcode::Undefined:
            return JSValue::encode(jsUndefined());
        case Opcode::Null:
            return JSValue::encode(jsNull());
        case Opcode::Boolean:
            return JSValue::encode(jsBoolean(constant.boolean()));
        case Opcode::Integer:
            return JSValue::encode(jsNumber(static_cast<double>(constant.integer())));
        case Opcode::Double:
            return JSValue::encode(jsDoubleNumber(purifyNaN(std::bit_cast<double>(constant.bits()))));
        case Opcode::String: {
            auto bytes = m_program.string(constant.string());
            if (bytes.empty() || (bytes.size() == 1 && bytes[0] && isASCII(bytes[0])))
                return Data::encodedSmallString(bytes.empty() ? 0 : bytes[0]);
            bool isCreatedOnce = isShared && m_program.stringsCreatedOnce.get(constant.string().index());
            return Data::encodedStringConstant(constant.string().index()) | (isCreatedOnce ? Data::encodedStringIsCached : 0);
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void placeArraySites()
    {
        auto& builtins = Builtins::singleton();
        Vector<Value*> allConstantLiterals;
        Vector<Value*> allPushedLiterals;
        Vector<Value*> allUnsharedLiterals;
        Vector<Vector<uint64_t>> allUnsharedLiteralElements;
        using Literals = std::tuple<Vector<Value*>, Vector<Value*>, Vector<Vector<uint64_t>>, Vector<Value*>, Vector<Vector<uint64_t>>>;
        forEachFunctionInOrder<Literals>([&](Function& function) {
            auto [constantLiterals, pushedLiterals, literalElements, unsharedLiterals, unsharedLiteralElements] = Literals { };
            BitVector isStoredTo;
            BitVector isPushedTo;
            auto originOf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                return id;
            };
            static constexpr OpcodeSet writers { Opcode::PutByVal, Opcode::CallBuiltin };
            std::as_const(m_program).functions.forEachIndexedValue(indexOf(function), writers, [&](ValueID id) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::PutByVal) {
                    isStoredTo.set(originOf(value.child(0)).index());
                    return;
                }
                auto& builtin = builtins.at(value.builtin());
                if (builtin.effects & Effect::WritesArguments) {
                    for (ValueID argument : value.arguments)
                        isStoredTo.set(originOf(argument).index());
                }
                if (!builtin.hasReceiver() || !value.child(0))
                    return;
                if (builtin.effects & Effect::WritesReceiver)
                    isStoredTo.set(originOf(value.child(0)).index());
                if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::ArrayPush)
                    isPushedTo.set(originOf(value.child(0)).index());
            });
            for (bool changed = !isPushedTo.isEmpty(); changed;) {
                changed = false;
                for (auto& block : function.blocks) {
                    for (auto& target : block.terminator.targets) {
                        auto& parameters = function.at(target.block).parameters;
                        for (size_t i = 0; i < parameters.size() && i < target.arguments.size(); ++i) {
                            if (isPushedTo.get(parameters[i].index()))
                                changed |= !isPushedTo.set(originOf(target.arguments[i]).index());
                        }
                    }
                }
            }
            static constexpr OpcodeSet arrayLiterals { Opcode::NewArray };
            std::as_const(m_program).functions.forEachIndexedValue(indexOf(function), arrayLiterals, [&](ValueID id) {
                auto& value = function.at(id);
                value.immediates = { Format::none, Format::none };
                if (value.arguments.isEmpty()) {
                    if (isPushedTo.get(id.index()))
                        pushedLiterals.append(&value);
                    return;
                }
                bool areConstants = std::ranges::all_of(value.arguments, [&](ValueID element) {
                    switch (function.at(element).opcode) {
                    case Opcode::Undefined:
                    case Opcode::Null:
                    case Opcode::Boolean:
                    case Opcode::Integer:
                    case Opcode::Double:
                    case Opcode::String:
                        return true;
                    default:
                        return false;
                    }
                });
                bool isShared = !isStoredTo.get(id.index()) && !isPushedTo.get(id.index());
                if (areConstants && (isShared || value.arguments.size() > Program::maxInlineArrayLiteralLength)) {
                    (isShared ? constantLiterals : unsharedLiterals).append(&value);
                    Vector<uint64_t> elements;
                    for (ValueID element : value.arguments)
                        elements.append(encodedConstant(function.at(element), isShared));
                    (isShared ? literalElements : unsharedLiteralElements).append(WTF::move(elements));
                }
            });
            return Literals { WTF::move(constantLiterals), WTF::move(pushedLiterals), WTF::move(literalElements), WTF::move(unsharedLiterals), WTF::move(unsharedLiteralElements) };
        }, [&](Literals&& literals) {
            allConstantLiterals.appendVector(std::get<0>(literals));
            allPushedLiterals.appendVector(std::get<1>(literals));
            m_program.arrayLiteralElements.appendVector(WTF::move(std::get<2>(literals)));
            allUnsharedLiterals.appendVector(std::get<3>(literals));
            allUnsharedLiteralElements.appendVector(WTF::move(std::get<4>(literals)));
        }, containsNewArray);
        auto& constantLiterals = allConstantLiterals;
        auto& pushedLiterals = allPushedLiterals;
        uint32_t offset = WTF::roundUpToMultipleOf<sizeof(JSCell*)>(m_program.sizeOfStatics);
        m_program.copyOnWriteButterfliesOffset = offset;
        m_program.numberOfCopyOnWriteButterflies = constantLiterals.size();
        for (Value* literal : constantLiterals) {
            literal->immediates[1] = offset;
            offset += sizeof(JSCell*);
        }
        uint32_t literalNumber = constantLiterals.size();
        for (Value* literal : allUnsharedLiterals)
            literal->immediates[1] = literalNumber++ << 1 | 1;
        m_program.arrayLiteralElements.appendVector(WTF::move(allUnsharedLiteralElements));
        for (Value* literal : pushedLiterals) {
            literal->immediates[0] = offset;
            offset += 2 * sizeof(uint32_t);
        }
        m_program.sizeOfStatics = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        report(constantLiterals.size(), " constant array literals, ", pushedLiterals.size(), " empty ones that are pushed to");
    }

    void placeRegExpLiterals()
    {
        uint32_t offset = WTF::roundUpToMultipleOf<sizeof(JSCell*)>(m_program.sizeOfStatics);
        m_program.sharedRegExpObjectsOffset = offset;
        forEachFunctionInOrder<Vector<uint64_t>>([&](Function& function) {
            Vector<uint64_t> keys;
            static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
            std::as_const(m_program).functions.forEachIndexedValue(indexOf(function), builtinCalls, [&](ValueID id) {
                if (auto key = regExpLiteralKey(m_program, function, function.at(id)))
                    keys.append(*key);
            });
            return keys;
        }, [&](Vector<uint64_t>&& keys) {
            for (uint64_t key : keys) {
                if (m_program.sharedRegExpObjectOffsets.add(key, offset).isNewEntry)
                    offset += sizeof(JSCell*);
            }
        }, containsCallBuiltin);
        m_program.sizeOfStatics = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        report(m_program.sharedRegExpObjectOffsets.size(), " RegExp literals");
    }

    void checkAllocations()
    {
        auto& allocations = m_program.allocations;
        BitVector seen;
        for (auto& allocation : allocations) {
            auto& layout = m_program.at(allocation.layout);
            if (allocation.layout == m_program.closureLayout) {
                RELEASE_ASSERT(allocation.size >= Closure::allocationSize(allocation.numberOfCaptures) && allocation.order.isEmpty() && !allocation.numberOfLiterals);
                continue;
            }
            RELEASE_ASSERT(allocation.size >= layout.size && allocation.size <= layout.sizeWithSpecializationCapacity && !(allocation.size % 8));
            for (uint16_t field : allocation.order.span().first(allocation.initialFieldCount))
                RELEASE_ASSERT((!layout.fields[field].isOutOfLine || layout.fields[field].flags & fieldMayExistWithoutStorage) && !(layout.fields[field].flags & fieldIsAbsentAtCreation));
            if (!allocation.numberOfLiterals)
                continue;
            RELEASE_ASSERT(allocation.size == layout.sizeWithSpecializationCapacity);
            RELEASE_ASSERT(allocation.firstLiteralAllocation + allocation.numberOfLiterals < allocations.size());
            StringID name = layout.fields[allocation.discriminant].name;
            Vector<uint32_t> closure = transitionClosure(allocation.layout.index(), seen);
            BitVector isInClosure = seen;
            for (unsigned k = 0; k <= allocation.numberOfLiterals; ++k) {
                auto& entry = allocations[allocation.firstLiteralAllocation + k];
                auto& chosen = m_program.at(entry.layout);
                RELEASE_ASSERT(entry.constructedBy == allocation.constructedBy && entry.initialFieldCount == allocation.initialFieldCount && entry.order.size() == allocation.order.size() && !entry.numberOfLiterals);
                for (unsigned n = 0; n < entry.order.size(); ++n)
                    RELEASE_ASSERT(chosen.fields[entry.order[n]].name == layout.fields[allocation.order[n]].name);
                RELEASE_ASSERT(!chosen.hasExtension);
                if (entry.layout == allocation.layout)
                    continue;
                RELEASE_ASSERT(k < allocation.numberOfLiterals && chosen.isAllocated && isInClosure.get(entry.layout.index()));
                RELEASE_ASSERT(constantOf(entry.layout.index(), name) == allocation.firstLiteral + static_cast<int32_t>(k));
                RELEASE_ASSERT(entry.size == chosen.sizeWithSpecializationCapacity);
                transitionClosure(entry.layout.index(), seen);
                for (uint32_t other : closure)
                    RELEASE_ASSERT(constantOf(other, name) != constantOf(entry.layout.index(), name) || seen.get(other) || std::ranges::binary_search(m_program.layouts[other].subtypes, entry.layout));
            }
            for (uint32_t target : closure) {
                for (auto& constant : constantsOf(target, name)) {
                    auto integer = integerOf(constant);
                    if (!integer)
                        continue;
                    int64_t k = static_cast<int64_t>(*integer) - allocation.firstLiteral;
                    RELEASE_ASSERT(k >= 0 && k < allocation.numberOfLiterals);
                    auto& entry = allocations[allocation.firstLiteralAllocation + k];
                    RELEASE_ASSERT(entry.layout != allocation.layout || entry.size >= m_program.layouts[target].size);
                }
            }
        }
        for (auto& layout : m_program.layouts) {
            if (layout.discriminant == UINT16_MAX) {
                RELEASE_ASSERT(!layout.isAbstract);
                for (LayoutID target : layout.specializations)
                    RELEASE_ASSERT(m_program.at(target).discriminant == UINT16_MAX);
                continue;
            }
            StringID name = layout.fields[layout.discriminant].name;
            auto constant = constantOf(&layout - &m_program.layouts[0], name);
            RELEASE_ASSERT(layout.isAbstract == !constant);
            RELEASE_ASSERT(m_program.at(layout.fields[layout.discriminant].slot).isDiscriminant);
            for (LayoutID target : layout.specializations)
                RELEASE_ASSERT(m_program.at(target).discriminant != UINT16_MAX && (!constant || constantOf(target.index(), name) == constant));
        }
    }

    void checkStores()
    {
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            if (!(m_referencedByFunction[i] & containsFieldStore))
                return;
            auto& function = std::as_const(m_program).functions[i];
            static constexpr OpcodeSet fieldStores { Opcode::PutField, Opcode::PutById };
            std::as_const(m_program).functions.forEachIndexedValue(i, fieldStores, [&](ValueID id) {
                auto& value = function.at(id);
                const Field* field = nullptr;
                if (value.opcode == Opcode::PutField)
                    field = &m_program.at(value.layout()).fields[value.field()];
                else if (LayoutID supertype = m_program.typeOf(function, value.child(0)).singleLayout()) {
                    if (auto found = m_program.at(supertype).fieldNamed(value.string()))
                        field = &m_program.at(supertype).fields[*found];
                }
                RELEASE_ASSERT(!field || m_program.typeOf(function, value.child(1)).isSubsetOf(m_program.at(field->stored), m_program));
            });
        });
    }

    void checkFieldsNeverObservedAbsent()
    {
        auto& layouts = m_program.layouts;
        auto isNeverObservedAbsent = [](const Field& field) -> bool {
            return field.flags & fieldIsNeverObservedAbsent;
        };
        auto require = [&](bool condition, const Layout& layout, const Field& field, ASCIILiteral what) {
            if (condition) [[likely]]
                return;
            dataLogLn("AOT: placement: ", nameOf(layout), " (layout ", &layout - &layouts[0], ") field ", nameOf(field.name), " is assumed never observed absent, but ", what);
            RELEASE_ASSERT_NOT_REACHED();
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].specializations.isEmpty())
                continue;
            withFieldsByName(m_fieldNamed, i, [&] {
                for (LayoutID specialization : layouts[i].specializations) {
                    for (auto& field : m_program.at(specialization).fields) {
                        uint32_t own = m_fieldNamed[field.name.index()];
                        require(isNeverObservedAbsent(field) == (own != none && isNeverObservedAbsent(layouts[i].fields[own])), m_program.at(specialization), field, "that does not hold across a transition"_s);
                    }
                }
            });
        }
        for (auto& initial : m_initialStores) {
            auto& function = std::as_const(m_program).functions[initial.function];
            auto& supertype = m_program.at(initial.supertype);
            RELEASE_ASSERT(function.numberOfInitialStores == initial.count && countOf(function.orderWordAfterInitialStores) == initial.count && !(function.orderWordAfterInitialStores >> 28));
            auto names = initial.names.span().first(initial.count);
            forEachLayoutInitializedBy(initial, [&](uint32_t index) {
                auto& layout = layouts[index];
                if (!initial.count)
                    return;
                auto& order = layout.orders[orderOf(function.orderWordAfterInitialStores)];
                RELEASE_ASSERT(order.size() >= initial.count);
                for (unsigned k = 0; k < initial.count; ++k) {
                    auto& field = layout.fields[order[k]];
                    auto& accessThroughSupertype = supertype.fields[*fieldOf(initial, k)];
                    RELEASE_ASSERT(field.name == names[k] && field.slot == accessThroughSupertype.slot && (!field.isOutOfLine || field.flags & fieldMayExistWithoutStorage) && !accessThroughSupertype.isMissingInSomeSubtype);
                }
            });
        }
        forEachUninitializedField([&](uint32_t index, Layout& layout, Field& field) {
            require(!isNeverObservedAbsent(field) || initializesFieldBeforeEscape(index, field.name), layout, field, "an allocation does not initialize it, and not every function its objects may be passed to stores it initially"_s);
        });
        unsigned numberOfFields = 0;
        unsigned outOfLineCount = 0;
        BitVector isDisqualified;
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!isNeverObservedAbsent(field))
                    continue;
                require(exists(field) && isObjectLayout(layout.kind), layout, field, "it is not a field of an object that is ever stored to"_s);
                ++numberOfFields;
                if (field.isOutOfLine) {
                    ++outOfLineCount;
                    continue;
                }
                if (!(m_program.at(field.stored).primitives & Primitive::Absent))
                    continue;
                require(m_mayBeObservedAbsent.quickGet(field.slot.index()), layout, field, "its slot may be empty, and no other field that shares the slot explains that"_s);
                isDisqualified.set(field.slot.index());
            }
        }
        if (!numberOfFields)
            return;
        report(numberOfFields, " fields are never observed absent, ", outOfLineCount, " of them out of line; ", isDisqualified.bitCount(), " of the other fields' slots may still be empty");
        Vector<std::pair<unsigned, const Layout*>> others(FillWith { }, isDisqualified.size(), std::pair<unsigned, const Layout*> { 0, nullptr });
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!layout.isAllocated || !isObjectLayout(layout.kind) || !exists(field) || isNeverObservedAbsent(field) || !isDisqualified.get(field.slot.index()))
                    continue;
                if (!others[field.slot.index()].first++)
                    others[field.slot.index()].second = &layout;
            }
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!isNeverObservedAbsent(field) || !isDisqualified.get(field.slot.index()) || !isDisqualified.quickClear(field.slot.index()))
                    continue;
                auto [count, first] = others[field.slot.index()];
                report("  ", nameOf(layout), ".", nameOf(field.name), ": because of ", count, " layouts, the first is ", first ? nameOf(*first) : nameOf(StringID()));
            }
        }
    }

    void pinLiterals()
    {
        auto& layouts = m_program.layouts;
        BitVector changesShape(layouts.size());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            for (LayoutID specialization : layouts[i].specializations) {
                changesShape.quickSet(i);
                changesShape.quickSet(specialization.index());
            }
        }
        unsigned numberOfPinnedFields = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind) || !isInstantiated(layout) || changesShape.quickGet(i))
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field) || field.isOutOfLine || field.literals.size() != 1 || !field.literals[0].string || !(field.flags & fieldIsNeverObservedAbsent) || m_program.at(field.slot).isDiscriminant)
                    continue;
                field.isPinned = true;
                m_program.at(field.slot).hasPinnedFields = true;
                ++numberOfPinnedFields;
            }
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (field.isPinnedToFunction && isObjectLayout(layout.kind) && exists(field) && field.slot)
                    m_program.at(field.slot).hasPinnedFields = true;
            }
        }
        unsigned numberOfSupertypeFields = 0;
        unsigned supertypeFieldsAfterUntypedCount = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& supertype = layouts[i];
            if (!isObjectLayout(supertype.kind) || supertype.subtypes.isEmpty())
                continue;
            for (unsigned k = 0; k < supertype.fields.size(); ++k) {
                auto& field = supertype.fields[k];
                if (!exists(field) || field.isMissingInSomeSubtype || !m_program.at(field.slot).hasPinnedFields)
                    continue;
                bool isPinnedInEverySubtype = std::ranges::all_of(supertype.subtypes, [&](LayoutID subtype) {
                    auto own = m_program.at(subtype).fieldNamed(field.name);
                    return own && m_program.at(subtype).fields[*own].slot == field.slot && m_program.at(subtype).fields[*own].isPinned;
                });
                field.isPinnedInEverySubtype = isPinnedInEverySubtype && !supertype.acceptsUntypedObjects;
                numberOfSupertypeFields += field.isPinnedInEverySubtype;
                supertypeFieldsAfterUntypedCount += isPinnedInEverySubtype && supertype.acceptsUntypedObjects;
            }
        }
        report(numberOfPinnedFields, " fields are pinned to their single string; ", numberOfSupertypeFields, " supertype fields are pinned in every subtype, and ", supertypeFieldsAfterUntypedCount, " more belong to supertypes that accept untyped objects and are not pinned");
    }

    void findFieldsWithSlotInEverySubtype()
    {
        auto& layouts = m_program.layouts;
        forEachName([&](NamedFieldTable& table, std::span<const NamedField> fields) {
            for (auto& named : fields) {
                auto& field = fieldAt(named);
                auto& supertype = layouts[named.layout];
                if (!m_isAccessed.quickGet(named.field) || !field.isMissingInSomeSubtype || field.hasOtherOffsetInSomeSubtype || field.isOutOfLine || supertype.acceptsUntypedObjects)
                    continue;
                uint32_t subtypesWithSlotCount = 0;
                forEachSubtypeWithField(table, fields, supertype, [&](uint32_t place) {
                    subtypesWithSlotCount += fieldAt(fields[place]).slot == field.slot;
                });
                field.hasSlotInEverySubtype = subtypesWithSlotCount == supertype.subtypes.size();
            }
        });
    }

    void checkPlacement()
    {
        checkAllocations();
        checkStores();
        checkFieldsNeverObservedAbsent();
        auto& layouts = m_program.layouts;
        forEachIndexOnAllThreads<unsigned>(layouts.size(), m_supertypes.size(), [] {
            return 0u;
        }, [&](unsigned&, unsigned supertype) {
            for (LayoutID subtype : layouts[supertype].subtypes)
                RELEASE_ASSERT(m_program.at(subtype).shape && m_program.passes(layouts[supertype].test, m_program.at(subtype).shape));
        });
        forEachName([&](NamedFieldTable& table, std::span<const NamedField> fields) {
            for (auto& named : fields) {
                auto& field = fieldAt(named);
                forEachWithField(table, fields, layouts[named.layout].specializations, [&](uint32_t place) {
                    auto& kept = fieldAt(fields[place]);
                    RELEASE_ASSERT(kept.slot == field.slot && kept.offset == field.offset && kept.store == field.store && kept.stored == field.stored && kept.isOutOfLine == field.isOutOfLine);
                });
                if (!m_isAccessed.quickGet(named.field)) {
                    RELEASE_ASSERT(field.isMissingInSomeSubtype && field.subtypesWithField.neverPasses() && !m_subtypesWith[named.field].count);
                    continue;
                }
                uint32_t subtypesWithFieldCount = 0;
                forEachSubtypeWithField(table, fields, layouts[named.layout], [&](uint32_t place) {
                    auto& own = fieldAt(fields[place]);
                    if (own.placeholderFor)
                        return;
                    if (own.slot != field.slot) {
                        RELEASE_ASSERT(std::ranges::binary_search(layouts[named.layout].structuralSubtypes, LayoutID(fields[place].layout)) && !m_isStoredThrough.quickGet(named.field));
                        RELEASE_ASSERT(field.isMissingInSomeSubtype && !m_program.passes(field.subtypesWithField, layouts[fields[place].layout].shape));
                        return;
                    }
                    RELEASE_ASSERT(own.slot == field.slot && own.offset == field.offset && own.store == field.store && own.stored == field.stored && own.isOutOfLine == field.isOutOfLine);
                    RELEASE_ASSERT(!field.isMissingInSomeSubtype || m_program.passes(field.subtypesWithField, layouts[fields[place].layout].shape));
                    ++subtypesWithFieldCount;
                });
                RELEASE_ASSERT(subtypesWithFieldCount == m_subtypesWith[named.field].count);
                RELEASE_ASSERT(field.isMissingInSomeSubtype == (subtypesWithFieldCount != layouts[named.layout].subtypes.size()));
            }
        });
        using ShapeSets = Vector<std::pair<uint32_t, uint32_t>>;
        auto checkNumberPassing = [&](ShapeSets& sets, const ShapeTest& test, uint32_t expected) {
            if (test.isRange()) {
                RELEASE_ASSERT(test.first ? m_program.shapeCountInRange(test.first, test.last()) == expected : test.count == expected);
                if (test.blockByte) {
                    uint32_t block = (test.last() - 1) / shapesPerBlock;
                    RELEASE_ASSERT(block < maxTestableBlocks && test.blockByte == 0xff - block);
                    RELEASE_ASSERT(m_program.shapeCountInRange(test.passesEarlierBlocks ? 1 : block * shapesPerBlock + 1, std::min((block + 1) * shapesPerBlock, m_program.numberOfShapes)) == expected);
                }
            } else {
                RELEASE_ASSERT(!(test.set % 32) && expected < test.count && test.last() <= m_program.numberOfShapes);
                sets.append({ test.set, expected });
            }
        };
        Vector<uint32_t> numberOfInstanceLayouts(FillWith { }, layouts.size(), 0);
        ShapeSets sets = collectInParallel<std::pair<uint32_t, uint32_t>>(layouts.size(), [&](unsigned i, ShapeSets& collected) {
            auto& layout = layouts[i];
            checkNumberPassing(collected, layout.test, layout.subtypes.size());
            if (layout.kind == LayoutKind::Instance && layout.isAllocated) {
                for (LayoutID ancestor(i); ancestor; ancestor = m_program.at(ancestor).base) {
                    RELEASE_ASSERT(m_program.passes(m_program.at(ancestor).instances, layout.shape));
                    WTF::atomicExchangeAdd(&numberOfInstanceLayouts[ancestor.index()], 1u, std::memory_order_relaxed);
                }
            }
            Vector<std::pair<uint32_t, uint32_t>, 32> extents;
            Vector<std::pair<uint32_t, uint32_t>, 8> extensionExtents;
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                auto& field = layout.fields[k];
                if (!exists(field)) {
                    RELEASE_ASSERT(!field.slot && !field.stored && field.offset == none && field.isMissingInSomeSubtype && field.subtypesWithField.neverPasses());
                    continue;
                }
                auto& slot = m_program.at(field.slot);
                RELEASE_ASSERT(field.stored == slot.type && field.store == slot.store && field.offset == slot.offset && field.isOutOfLine == slot.isOutOfLine);
                if (field.isOutOfLine) {
                    RELEASE_ASSERT(isObjectLayout(layout.kind) && field.store == outOfLineStoreFor(field.slot.index(), m_program.at(field.stored)) && m_program.at(field.stored).primitives & Primitive::Absent && !slot.isDiscriminant && !(field.flags & fieldIsImmutable));
                    RELEASE_ASSERT(field.offset >= offsetOfFirstExtensionSlot && (!layout.isAllocated || (layout.hasExtension && field.offset + sizeOf(field.store) <= layout.extensionSize)));
                } else
                    RELEASE_ASSERT(field.store == storeFor(m_program.at(field.stored), m_isPredictedInt32.quickGet(field.slot.index()), m_hasWideReferences.quickGet(field.slot.index())));
                RELEASE_ASSERT(!(field.offset % sizeOf(field.store)));
                RELEASE_ASSERT(!isInstantiated(layout) || m_program.at(field.type ? field.type : m_program.types.anything).isSubsetOf(m_program.at(field.stored), m_program));
                if (field.isMissingInSomeSubtype)
                    checkNumberPassing(collected, field.subtypesWithField, m_subtypesWith[m_firstField[i] + k].count);
                if (field.isOutOfLine)
                    extensionExtents.append({ field.offset, field.offset + sizeOf(field.store) });
                else
                    extents.append({ field.offset, field.offset + sizeOf(field.store) });
            }
            if (!layout.isAllocated)
                return;
            RELEASE_ASSERT(layout.shape && layout.shape <= m_program.numberOfShapes);
            std::ranges::sort(extents);
            uint32_t end = isObjectLayout(layout.kind) ? offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0) : static_cast<uint32_t>(sizeof(JSCell));
            for (auto [first, last] : extents) {
                RELEASE_ASSERT(first >= end);
                end = last;
            }
            std::ranges::sort(extensionExtents);
            uint32_t extensionEnd = offsetOfFirstExtensionSlot;
            for (auto [first, last] : extensionExtents) {
                RELEASE_ASSERT(first >= extensionEnd);
                extensionEnd = last;
            }
            RELEASE_ASSERT(extensionExtents.isEmpty() ? !layout.extensionSize : layout.extensionSize == extensionEnd);
            for (LayoutID target : layout.specializations)
                RELEASE_ASSERT(m_program.at(target).hasExtension == layout.hasExtension && (!m_program.at(target).isAllocated || m_program.at(target).offsetOfExtraProperties == layout.offsetOfExtraProperties));
            if (uint32_t offset = layout.offsetOfExtraProperties) {
                RELEASE_ASSERT(isObjectLayout(layout.kind) && !(offset % sizeOfExtraPropertiesWord) && offset + sizeOfExtraPropertiesWord <= cellSizeFor(layout.size) && offset < LayoutData::extraPropertiesOffsetLimit);
                for (auto [first, last] : extents)
                    RELEASE_ASSERT(last <= offset || first >= offset + sizeOfExtraPropertiesWord);
            }
            RELEASE_ASSERT(end <= layout.size && layout.size <= layout.sizeWithSpecializationCapacity);
            for (LayoutID specialization : layout.specializations)
                RELEASE_ASSERT(m_program.at(specialization).size <= layout.sizeWithSpecializationCapacity);
        });
        for (auto& entry : m_program.multiLayoutShapeTests.values())
            checkNumberPassing(sets, entry.test, entry.numberOfShapes);
        sets.appendVector(collectInParallel<std::pair<uint32_t, uint32_t>>(layouts.size(), [&](unsigned i, ShapeSets& collected) {
            checkNumberPassing(collected, layouts[i].instances, numberOfInstanceLayouts[i]);
            checkNumberPassing(collected, layouts[i].retagSources, retagSourcesOf(i).size());
            for (uint32_t source : retagSourcesOf(i))
                RELEASE_ASSERT(m_program.passes(layouts[i].retagSources, layouts[source].shape) && !m_program.passes(layouts[i].castTest, layouts[source].shape));
            if (isObjectLayout(layouts[i].kind))
                checkNumberPassing(collected, layouts[i].castTest, layouts[i].subtypes.size() - retaggedSubtypesOf(i).size());
            for (LayoutID subtype : layouts[i].subtypes)
                RELEASE_ASSERT(m_program.passes(layouts[i].castTest, m_program.at(subtype).shape) == !std::ranges::binary_search(retaggedSubtypesOf(i), subtype));
        }));
        std::ranges::sort(sets);
        size_t numberOfBits = 0;
        for (unsigned i = 0; i < sets.size(); ++i) {
            if (i && sets[i].first == sets[i - 1].first)
                RELEASE_ASSERT(sets[i].second == sets[i - 1].second);
            else
                numberOfBits += sets[i].second;
        }
        RELEASE_ASSERT(m_program.shapeSetBits.bitCount() == numberOfBits);
    }

    Program& m_program;
    PhaseTimes m_phaseTimes { "placement"_s };
    Vector<uint32_t> m_fieldNamed;
    std::atomic<unsigned> m_numberOfFailures { 0 };
    uint32_t m_maxFieldCount { 0 };
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_firstSupertype;
    Vector<NamedFieldRange> m_names;
    Vector<NamedField> m_fieldsByName;
    Vector<std::array<uint64_t, 2>> m_layoutHashes;
    BitVector m_isAccessed;
    Vector<uint8_t> m_referencedByFunction;
    BitVector m_isStoredThrough;
    Vector<uint32_t> m_missingFieldIndex;
    Vector<Fingerprint> m_subtypesWith;
    Vector<uint32_t> m_initializingAllocationCounts;
    Vector<uint32_t> m_environmentFieldUseCounts;
    Vector<uint32_t> m_supertypes;
    Vector<MissingField> m_missingFields;
    Vector<SubtypeSet> m_subtypeSets;
    Vector<uint32_t> m_listedSubtypes;
    unsigned m_missingRequiredFieldCount { 0 };
    BitVector m_holdsSeveralTypes;
    BitVector m_mayBeAbsent;
    BitVector m_isInAllocatedLayout;
    Vector<Family> m_families;
    Vector<uint64_t> m_accessWeight;
    Vector<uint64_t> m_doubleStoreCounts;
    Vector<uint64_t> m_rangeTestedStoreCounts;
    Vector<uint64_t> m_integerUseCounts;
    Vector<uint64_t> m_doubleUseCounts;
    BitVector m_isInCopiedRecord;
    const Vector<size_t> m_sizeClasses { MarkedSpace::sizeClassesFor(Options::sizeClassProgressionWithAOT()) };
    Vector<uint32_t> m_group;
    static constexpr uint32_t noCap = UINT32_MAX;
    static constexpr uint32_t referencesBeyondCreation = 4;
    static constexpr uint32_t maxInlineCapacity = 128;
    static constexpr uint32_t maxInlineCapacityAtCreation = 512;
    Vector<uint32_t> m_inlineCap;
    Vector<std::tuple<uint32_t, Store, TypeID>> m_outOfLineSlots;
    BitVector m_isPredictedInt32;
    BitVector m_hasIntegerField;
    BitVector m_isNeverObservedAbsent;
    BitVector m_mayBeObservedAbsent;
    Vector<InitialStores> m_initialStores;
    Vector<uint32_t> m_functionInitialStores;
    BitVector m_isThisLayout;
    Vector<Vector<StringID>> m_commonInitialStoreNames;
    BitVector m_hasOtherUses;
    Vector<std::pair<uint32_t, uint32_t>> m_otherTypes;
    Vector<TypeID> m_typesWithAbsent;
    Vector<uint64_t> m_typeTests;
    Vector<uint64_t> m_supertypeTests;
    Vector<uint64_t> m_classTests;
    Vector<uint64_t> m_coercionsToLayout;
    Vector<uint64_t> m_coercionsToType;
    Vector<uint64_t> m_castsOfInlineReads;
    BitVector m_isAllocatedOrCastTarget;
    BitVector m_isFirstStoredWithValue;
    BitVector m_existsAtCreation;
    bool m_hasFoundSlotsInitializedByAllocations { false };
    Vector<uint32_t> m_firstRetagSource;
    Vector<uint32_t> m_retagSources;
    BitVector m_hasLargeSubtype;
    Vector<uint32_t> m_firstRetaggedSubtype;
    Vector<LayoutID> m_retaggedSubtypes;
    Vector<uint32_t> m_allocationCounts;
    BitVector m_isAllocatedOften;
    BitVector m_hasWideReferences;
};

class DataEncoder {
public:
    explicit DataEncoder(const Program& program)
        : m_program(program)
    {
    }

    Vector<uint8_t> run()
    {
        allocate(sizeof(Data));
        Data header { };
        header.magic = Data::expectedMagic;
        header.numberOfStrings = m_program.strings.size();
        auto& strings = m_program.strings;
        header.stringsOffset = allocateWords(strings.size() * sizeof(uint32_t));
        RELEASE_ASSERT(header.stringsOffset == sizeof(Data));
        Vector<unsigned> constantUseCount;
        constantUseCount.fill(0, strings.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            for (auto& value : m_program.functions[i].values) {
                if (value.opcode == Opcode::String)
                    WTF::atomicExchangeAdd(&constantUseCount[value.string().index()], 1u, std::memory_order_relaxed);
            }
        });
        auto forEachStringInTables = [&](const auto& functor) {
            for (auto& table : m_program.tablesInImage) {
                for (StringID key : table.keys)
                    functor(key);
                for (uint64_t value : table.values) {
                    if (JSValue::decode(value).isCell() && !(value & Data::encodedStringIsSmall))
                        functor(StringID(Data::decodeStringConstant(value)));
                }
            }
        };
        forEachStringInTables([&](StringID string) {
            ++constantUseCount[string.index()];
        });
        BitVector isConstant(strings.size());
        for (uint32_t index = 0; index < strings.size(); ++index) {
            if (constantUseCount[index])
                isConstant.set(index);
        }
        BitVector mayBeLeftOut(strings.size());
        for (auto& variable : m_program.variables) {
            if (variable.name)
                mayBeLeftOut.set(variable.name.index());
        }
        for (auto& [pattern, flags] : m_program.regExpLiterals) {
            mayBeLeftOut.set(pattern.index());
            mayBeLeftOut.set(flags.index());
        }
        BitVector isListedInFields(strings.size());
        for (auto& layout : m_program.layouts) {
            for (auto& field : layout.fields) {
                if (field.typeSource && !keepsTypeSource(field))
                    mayBeLeftOut.set(field.typeSource.index());
                for (auto& literal : field.literals) {
                    if (!literal.string)
                        continue;
                    mayBeLeftOut.set(literal.string.index());
                    isListedInFields.set(literal.string.index());
                }
            }
        }
        Vector<uint32_t> isReferencedElsewhere(FillWith { }, (strings.size() + 31) / 32, 0);
        auto recordNamedString = [&](StringID id) {
            if (id && mayBeLeftOut.get(id.index()))
                WTF::atomicExchangeOr(&isReferencedElsewhere[id.index() / 32], 1u << id.index() % 32, std::memory_order_relaxed);
        };
        auto recordStringsNamedBy = [&](const Value& value) {
            recordNamedString(value.predictedByKey());
            switch (value.opcode) {
            case Opcode::GetExternalModule:
            case Opcode::String:
            case Opcode::BigInt:
            case Opcode::ConstantSymbol:
            case Opcode::GetGlobal:
            case Opcode::GetGlobalForTypeOf:
            case Opcode::PutGlobal:
            case Opcode::GetById:
            case Opcode::PutById:
            case Opcode::InById:
            case Opcode::DeleteById:
            case Opcode::CallMethod:
            case Opcode::Call:
                recordNamedString(value.string());
                break;
            case Opcode::Cast:
                if (value.castReason() != CastReason::Coerce)
                    recordNamedString(value.string());
                break;
            case Opcode::GetModuleVariable:
                if (auto& variable = m_program.variables[value.variable().index()]; variable.mayBeReadUninitialized() && !value.readsInitializedVariable())
                    recordNamedString(variable.name);
                break;
            default:
                break;
            }
        };
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            auto& function = m_program.functions[i];
            recordNamedString(function.name);
            for (auto& block : function.blocks) {
                for (ValueID id : block.values)
                    recordStringsNamedBy(function.at(id));
            }
        });
        forEachStringInTables(recordNamedString);
        for (auto& layout : m_program.layouts) {
            recordNamedString(layout.name);
            recordNamedString(layout.declaredPath);
            for (auto& field : layout.fields) {
                recordNamedString(field.name);
                if (keepsTypeSource(field))
                    recordNamedString(field.typeSource);
            }
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods)
                    recordNamedString(method.name);
            }
            for (auto& field : layout.staticFields)
                recordNamedString(field.name);
        }
        for (auto& module : m_program.modules) {
            recordNamedString(module.path);
            recordNamedString(module.displayPath);
        }
        for (auto& entry : m_program.functionsByKey) {
            recordNamedString(entry.property);
            recordNamedString(entry.key);
        }
        for (auto& row : m_program.initialStructures) {
            for (StringID name : row.names)
                recordNamedString(name);
        }
        for (auto& row : m_program.structureDescriptors) {
            for (StringID name : row.names)
                recordNamedString(name);
        }
        recordNamedString(m_program.emptyString);
        for (size_t index : mayBeLeftOut) {
            StringID id(static_cast<uint32_t>(index));
            if ((index < m_program.identifierNumbers.size() && m_program.identifierNumbers[index]) || m_program.builtinName(id) || m_program.prototypePropertyNames.get(index))
                recordNamedString(id);
        }
        auto isLeftOut = [&](uint32_t index) {
            return mayBeLeftOut.get(index) && !(isReferencedElsewhere[index / 32] >> index % 32 & 1);
        };
        auto hasOnlyDecodedForm = [&](uint32_t index) {
            if (isLeftOut(index))
                return true;
            return isConstant.get(index) && !charactersAreAllASCII(byteCast<Latin1Character>(strings[index])) && !isPrivateName(strings[index]) && !isEnginePrivateName(strings[index]);
        };
        auto isName = [&](uint32_t index) {
            return index < m_program.identifierNumbers.size() && m_program.identifierNumbers[index];
        };
        Vector<uint32_t> byLastCharacters;
        for (uint32_t index = 0; index < strings.size(); ++index) {
            if (!hasOnlyDecodedForm(index))
                byLastCharacters.append(index);
        }
        auto sortByPrefixThenBy = [](Vector<uint32_t>& list, const auto& prefixOf, const auto& isAhead) {
            Vector<std::pair<uint64_t, uint32_t>> withBytes(list.size(), [&](size_t i) {
                return std::pair { prefixOf(list[i]), list[i] };
            });
            std::ranges::sort(withBytes, [&](auto& a, auto& b) {
                return a.first != b.first ? a.first < b.first : isAhead(a.second, b.second);
            });
            for (size_t i = 0; i < list.size(); ++i)
                list[i] = withBytes[i].second;
        };
        auto prefixFrom = [](auto at, auto end) {
            uint64_t result = 0;
            for (unsigned shift = 64; shift && at != end; ++at)
                result |= static_cast<uint64_t>(*at) << (shift -= 8);
            return result;
        };
        sortByPrefixThenBy(byLastCharacters, [&](uint32_t index) {
            return prefixFrom(strings[index].rbegin(), strings[index].rend());
        }, [&](uint32_t a, uint32_t b) {
            auto [endOfA, endOfB] = std::mismatch(strings[a].rbegin(), strings[a].rend(), strings[b].rbegin(), strings[b].rend());
            if (endOfA == strings[a].rend() || endOfB == strings[b].rend())
                return strings[a].size() != strings[b].size() ? strings[a].size() < strings[b].size() : a < b;
            return *endOfA < *endOfB;
        });
        Vector<uint32_t> holders(strings.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        for (size_t i = byLastCharacters.size(); i-- > 1;) {
            auto tail = strings[byLastCharacters[i - 1]];
            auto whole = strings[byLastCharacters[i]];
            if (tail.size() <= whole.size() && equalSpans(tail, whole.last(tail.size())) && (!isName(byLastCharacters[i - 1]) || isName(holders[byLastCharacters[i]])))
                holders[byLastCharacters[i - 1]] = holders[byLastCharacters[i]];
        }
        Vector<uint32_t> gaps(FillWith { }, strings.size(), 0);
        {
            MonotonicTime searchStart = MonotonicTime::now();
            constexpr size_t headSize = sizeof(uint32_t);
            constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
            auto headAt = [](std::span<const uint8_t> text, size_t at) {
                uint32_t head;
                memcpy(&head, text.data() + at, headSize);
                return static_cast<uint64_t>(head) + 1;
            };
            auto beginsWith = [](std::span<const uint8_t> text, std::span<const uint8_t> head) {
                return head.size() <= text.size() && equalSpans(head, text.first(head.size()));
            };
            Vector<uint32_t> inOrder;
            for (uint32_t index : byLastCharacters) {
                if (holders[index] == index && strings[index].size() >= headSize)
                    inOrder.append(index);
            }
            sortByPrefixThenBy(inOrder, [&](uint32_t index) {
                return prefixFrom(strings[index].begin(), strings[index].end());
            }, [&](uint32_t a, uint32_t b) {
                return std::ranges::lexicographical_compare(strings[a], strings[b]);
            });
            struct OrderedString {
                const uint8_t* characters;
                uint32_t size;
                uint32_t index;
                uint64_t afterHead;
            };
            auto prefixAfterHead = [](std::span<const uint8_t> text) {
                if (text.size() >= headSize + sizeof(uint64_t)) {
                    uint64_t bytes;
                    memcpy(&bytes, text.data() + headSize, sizeof(bytes));
                    static_assert(std::endian::native == std::endian::little);
                    return std::byteswap(bytes);
                }
                uint64_t result = 0;
                size_t end = std::min(text.size(), headSize + sizeof(uint64_t));
                for (size_t at = headSize; at < end; ++at)
                    result |= static_cast<uint64_t>(text[at]) << (8 * (headSize + sizeof(uint64_t) - 1 - at));
                return result;
            };
            Vector<OrderedString> orderedStrings(inOrder.size(), [&](size_t place) {
                auto text = strings[inOrder[place]];
                return OrderedString { text.data(), safeCast<uint32_t>(text.size()), inOrder[place], prefixAfterHead(text) };
            });
            Vector<uint32_t> longestHeads(FillWith { }, inOrder.size(), none);
            Vector<uint32_t> open;
            UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, uint32_t>> headRanges;
            constexpr unsigned headSlotBits = 22;
            BitVector mayBeHead(1 << headSlotBits);
            auto headSlot = [](uint64_t head) {
                return static_cast<size_t>(head * 0x9e3779b97f4a7c15ULL >> (64 - headSlotBits));
            };
            for (uint32_t place = 0; place < inOrder.size(); ++place) {
                auto text = strings[inOrder[place]];
                while (!open.isEmpty() && !beginsWith(text, strings[inOrder[open.last()]]))
                    open.removeLast();
                if (!open.isEmpty())
                    longestHeads[place] = open.last();
                open.append(place);
                headRanges.add(headAt(text, 0), std::pair { place, place }).iterator->value.second = place + 1;
                mayBeHead.quickSet(headSlot(headAt(text, 0)));
            }
            Vector<uint64_t> lengthsAndNumbers(inOrder.size(), [&](size_t place) {
                return static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - safeCast<uint32_t>(strings[inOrder[place]].size())) << 32 | inOrder[place];
            });
            std::ranges::sort(lengthsAndNumbers);
            Vector<uint32_t> fromLongest(lengthsAndNumbers.size(), [&](size_t place) {
                return static_cast<uint32_t>(lengthsAndNumbers[place]);
            });
            uint64_t searchedSlots = 0;
            uint64_t stepsOnChains = 0;
            uint64_t heldCount = 0;
            uint64_t occupiedBytes = 0;
            uint64_t charactersCompared = 0;
            auto ordersBefore = [&](std::span<const uint8_t> a, uint64_t afterHeadOfA, const OrderedString& b) {
                constexpr size_t recordedSize = headSize + sizeof(uint64_t);
                size_t shorterSize = std::min<size_t>(a.size(), b.size);
                uint64_t difference = afterHeadOfA ^ b.afterHead;
                size_t same = headSize + (difference ? std::countl_zero(difference) / 8 : sizeof(uint64_t));
                if (same < std::min(shorterSize, recordedSize)) {
                    charactersCompared += same;
                    return afterHeadOfA < b.afterHead;
                }
                if (shorterSize <= recordedSize) {
                    charactersCompared += shorterSize;
                    return b.size > shorterSize;
                }
                auto restOfA = a.subspan(recordedSize);
                std::span restOfB { b.characters + recordedSize, b.size - recordedSize };
                auto [endOfA, endOfB] = std::ranges::mismatch(restOfA, restOfB);
                charactersCompared += recordedSize + (endOfA - restOfA.begin());
                return endOfB != restOfB.end() && (endOfA == restOfA.end() || *endOfA < *endOfB);
            };
            for (uint32_t whole : fromLongest) {
                if (holders[whole] != whole)
                    continue;
                auto text = strings[whole];
                uint64_t maxCharactersCompared = charactersCompared + 1024 * static_cast<uint64_t>(text.size());
                for (size_t at = 0; at + headSize <= text.size() && charactersCompared <= maxCharactersCompared; ++at) {
                    uint64_t head = headAt(text, at);
                    if (!mayBeHead.quickGet(headSlot(head)))
                        continue;
                    auto found = headRanges.find(head);
                    if (found == headRanges.end())
                        continue;
                    ++searchedSlots;
                    auto rest = text.subspan(at);
                    uint64_t afterHeadOfRest = prefixAfterHead(rest);
                    auto [first, afterLast] = found->value;
                    uint32_t low = first;
                    for (uint32_t high = afterLast; low < high;) {
                        uint32_t middle = low + (high - low) / 2;
                        if (ordersBefore(rest, afterHeadOfRest, orderedStrings[middle]))
                            high = middle;
                        else
                            low = middle + 1;
                    }
                    for (uint32_t place = low == first ? none : low - 1; place != none; place = longestHeads[place]) {
                        ++stepsOnChains;
                        uint32_t part = orderedStrings[place].index;
                        std::span inner { orderedStrings[place].characters, orderedStrings[place].size };
                        if (inner.size() >= text.size())
                            continue;
                        charactersCompared += inner.size();
                        if (!beginsWith(rest, inner))
                            continue;
                        if (isName(part) && !isName(whole))
                            continue;
                        if (holders[part] != part)
                            break;
                        holders[part] = whole;
                        gaps[part] = safeCast<uint32_t>(rest.size() - inner.size());
                        ++heldCount;
                        occupiedBytes += inner.size();
                    }
                }
            }
            for (uint32_t index = 0; index < strings.size(); ++index) {
                uint32_t holder = holders[index];
                if (holders[holder] == holder)
                    continue;
                gaps[index] = gaps[holder];
                holders[index] = holders[holder];
            }
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: texts inside longer texts: ", heldCount, " of ", inOrder.size(), " texts, ", occupiedBytes, " bytes; ", searchedSlots, " places searched, ", stepsOnChains, " steps on chains, ", (MonotonicTime::now() - searchStart).milliseconds(), " ms");
        }
        Vector<uint32_t> ends(FillWith { }, strings.size(), 0);
        uint64_t emittedStringCount = 0;
        uint64_t numberOfSeamsBetweenLetters = 0;
        uint64_t numberOfSeparators = 0;
        auto emitString = [&](uint32_t index) {
            if (index >= strings.size() || hasOnlyDecodedForm(index))
                return;
            uint32_t holder = holders[index];
            if (ends[holder] || hasOnlyDecodedForm(holder))
                return;
            if (!strings[holder].empty()) {
                ++emittedStringCount;
                numberOfSeamsBetweenLetters += isASCIIAlpha(m_result.last()) && isASCIIAlpha(strings[holder].front());
                if (isASCIIPrintable(m_result.last()) && isASCIIPrintable(strings[holder].front())) {
                    ++numberOfSeparators;
                    m_result.append(0);
                }
            }
            m_result.append(strings[holder]);
            ends[holder] = safeCast<uint32_t>(m_result.size());
        };
        Vector<std::array<uint32_t, 3>> positionsAndIndices(m_program.functions.size(), [&](size_t index) {
            auto& function = m_program.functions[index];
            return std::array<uint32_t, 3> { function.module ? function.module.index() : UINT32_MAX, function.position, static_cast<uint32_t>(index) };
        });
        std::ranges::sort(positionsAndIndices);
        Vector<uint32_t> functionsInSourceOrder(m_program.functions.size(), [&](size_t index) {
            return positionsAndIndices[index][2];
        });
        static constexpr OpcodeSet stringNamingOpcodes { Opcode::String, Opcode::GetGlobal, Opcode::GetGlobalForTypeOf, Opcode::PutGlobal, Opcode::GetById, Opcode::PutById, Opcode::InById, Opcode::DeleteById, Opcode::CallMethod };
        Vector<Vector<uint32_t>> functionSources(m_program.functions.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            for (auto& value : m_program.functions[i].values) {
                if (stringNamingOpcodes.contains(value.opcode))
                    functionSources[i].append(value.immediates[0]);
                if (value.tableInImage() != Format::none) {
                    for (StringID key : m_program.tablesInImage[value.tableInImage()].keys)
                        functionSources[i].append(key.index());
                }
            }
        });
        size_t sizeBeforeNames = m_result.size();
        for (uint32_t functionIndex : functionsInSourceOrder) {
            for (uint32_t text : functionSources[functionIndex]) {
                if (isName(text))
                    emitString(text);
            }
        }
        for (uint32_t index = 0; index < strings.size(); ++index) {
            if (isName(index))
                emitString(index);
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: name strings: ", m_result.size() - sizeBeforeNames, " bytes");
        for (uint32_t functionIndex : functionsInSourceOrder) {
            auto& function = m_program.functions[functionIndex];
            if (function.name)
                emitString(function.name.index());
            for (uint32_t text : functionSources[functionIndex])
                emitString(text);
        }
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            functionSources[i].clear();
        });
        for (uint32_t index = 0; index < strings.size(); ++index)
            emitString(index);
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: strings emitted: ", emittedStringCount, "; a zero byte precedes ", numberOfSeparators, ", where printable characters meet; letters meet at ", numberOfSeamsBetweenLetters);
        Vector<uint32_t> listedStrings;
        for (uint32_t index = 0; index < strings.size(); ++index) {
            RELEASE_ASSERT(hasOnlyDecodedForm(index) || ends[holders[index]] > gaps[index] + strings[index].size());
            uint32_t offset = hasOnlyDecodedForm(index) ? 0 : ends[holders[index]] - gaps[index] - strings[index].size();
            uint32_t size = safeCast<uint32_t>(strings[index].size());
            uint32_t& word = at<uint32_t>(header.stringsOffset + index * sizeof(uint32_t));
            if (size && !(size >> Data::stringLengthBits) && !(offset >> (32 - Data::stringLengthBits))) {
                word = offset << Data::stringLengthBits | size;
                continue;
            }
            word = safeCast<uint32_t>(listedStrings.size() / 2) << Data::stringLengthBits;
            listedStrings.appendList({ offset, size });
        }
        RELEASE_ASSERT(!(listedStrings.size() / 2 >> (32 - Data::stringLengthBits)));
        header.listedStringsOffset = append(listedStrings);
        Vector<uint32_t> stringFacts;
        for (auto string : m_program.strings) {
            auto characters = byteCast<Latin1Character>(string);
            uint32_t facts = isPrivateName(string) ? Data::stringIsPrivateName : isEnginePrivateName(string) ? Data::stringIsEnginePrivateName : isConstantSymbol(string) ? Data::stringIsConstantSymbol : 0;
            uint32_t identifier = stringFacts.size() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[stringFacts.size()] : 0;
            RELEASE_ASSERT(identifier <= Data::stringHashMask);
            bool isHashed = isListedInFields.get(stringFacts.size()) && isLeftOut(stringFacts.size());
            if (charactersAreAllASCII(characters) && !isHashed)
                facts |= Data::stringIsASCII | (identifier ? 0 : StringHasher::computeHashAndMaskTop8Bits(characters));
            if (identifier)
                facts |= Data::stringHasIdentifierNumber | identifier;
            if (!characters.empty() && isASCIIDigit(characters[0]))
                facts |= Data::stringStartsWithDigit;
            stringFacts.append(facts);
        }
        header.stringFactsOffset = append(stringFacts);
        uint32_t maxIdentifier = 0;
        for (uint32_t identifier : m_program.identifierNumbers)
            maxIdentifier = std::max(maxIdentifier, identifier);
        Vector<uint32_t> identifierStrings(FillWith { }, maxIdentifier + 1, 0);
        Vector<uint32_t> identifierHashes(FillWith { }, maxIdentifier + 1, 0);
        for (uint32_t index = 0; index < m_program.identifierNumbers.size(); ++index) {
            if (uint32_t identifier = m_program.identifierNumbers[index]) {
                identifierStrings[identifier] = index + 1;
                if (auto characters = byteCast<Latin1Character>(m_program.strings[index]); charactersAreAllASCII(characters))
                    identifierHashes[identifier] = StringHasher::computeHashAndMaskTop8Bits(characters);
            }
        }
        header.numberOfIdentifierStrings = identifierStrings.size();
        header.identifierStringsOffset = append(identifierStrings);
        header.identifierHashesOffset = append(identifierHashes);
        Vector<uint32_t> decodedStrings;
        for (uint32_t index = 0; index < m_program.strings.size(); ++index) {
            auto bytes = m_program.strings[index];
            if (!isConstant.get(index) || charactersAreAllASCII(byteCast<Latin1Character>(bytes)) || isLeftOut(index))
                continue;
            Vector<char16_t> units = decodeProgramString(bytes);
            char16_t all = 0;
            for (char16_t unit : units)
                all |= unit;
            RELEASE_ASSERT(units.size() < Data::isDecodedTo16Bits);
            if (all <= 0xff) {
                Vector<uint8_t> narrow(units.size(), [&](size_t i) {
                    return static_cast<uint8_t>(units[i]);
                });
                decodedStrings.appendList({ index, append(narrow), static_cast<uint32_t>(units.size()) });
                m_result.append(0);
            } else {
                decodedStrings.appendList({ index, append(units), static_cast<uint32_t>(units.size()) | Data::isDecodedTo16Bits });
                m_result.appendList({ 0, 0 });
            }
        }
        header.numberOfDecodedStrings = decodedStrings.size() / 3;
        header.decodedStringsOffset = append(decodedStrings);
        Vector<HashedString> allHashedStrings;
        for (size_t index : isListedInFields) {
            if (isLeftOut(index))
                allHashedStrings.append(HashedString::create(index, decodeProgramString(strings[index]).span()));
        }
        header.numberOfHashedStrings = allHashedStrings.size();
        header.hashedStringsOffset = append(allHashedStrings);
        Vector<uint32_t> formatPatterns;
        for (auto& pattern : m_program.formatPatterns)
            formatPatterns.append(append(pattern));
        header.formatPatternsOffset = append(formatPatterns);
        header.icuStampOfFormatPatternsOffset = append(m_program.icuStampOfFormatPatterns);
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: texts that only the types of fields list: ", allHashedStrings.size(), " of ", isListedInFields.bitCount(), " are kept as hashes");
        Vector<uint32_t> atomStrings(FillWith { }, (m_program.strings.size() + 31) / 32, 0);
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            auto& function = m_program.functions[i];
            auto note = [&](ValueID id) {
                while (id && function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                if (id && function.at(id).opcode == Opcode::String)
                    WTF::atomicExchangeOr(&atomStrings[function.at(id).string().index() / 32], 1u << function.at(id).string().index() % 32, std::memory_order_relaxed);
            };
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    switch (value.opcode) {
                    case Opcode::PutByVal:
                        note(value.child(0));
                        note(value.child(1));
                        break;
                    case Opcode::CallBuiltin:
                        if (languageOperationOf(m_program, value) == LanguageOperation::DefineField && value.arguments.size() == 3) {
                            note(value.arguments[1]);
                            break;
                        }
                        [[fallthrough]];
                    case Opcode::StrictEq:
                    case Opcode::Eq:
                    case Opcode::GetByVal:
                    case Opcode::InByVal:
                    case Opcode::DeleteByVal:
                    case Opcode::ToPropertyKey:
                    case Opcode::Operation:
                        for (ValueID child : value.children)
                            note(child);
                        for (ValueID argument : value.arguments)
                            note(argument);
                        break;
                    default:
                        break;
                    }
                }
            }
        });
        header.atomStringsOffset = append(atomStrings);

        size_t sizeBeforeTypes = m_result.size();
        appendTypes();
        header.typesOffset = append(m_typeOffsets);
        header.numberOfTypes = m_typeOffsets.size();
        size_t sizeBeforeLayouts = m_result.size();

        Vector<uint32_t> shapes(FillWith { }, m_program.numberOfShapes + 1, 0);
        Vector<uint32_t> classes(FillWith { }, m_program.layouts.size(), 0);
        m_namesAssumedAbsent.ensureSize(strings.size());
        for (auto& layout : m_program.layouts) {
            for (auto& field : layout.fields) {
                if (field.name)
                    m_namesAssumedAbsent.quickSet(field.name.index());
            }
        }
        for (unsigned i = 0; i < m_program.layouts.size(); ++i) {
            auto& layout = m_program.layouts[i];
            if (!layout.shape && layout.kind != LayoutKind::Instance)
                continue;
            uint32_t offset = appendLayout(LayoutID(i));
            if (layout.shape)
                shapes[layout.shape] = offset;
            if (layout.kind == LayoutKind::Instance)
                classes[i] = offset;
        }
        header.numberOfLayouts = shapes.size();
        header.layoutsOffset = append(shapes);
        header.numberOfLayoutIndices = classes.size();
        Vector<uint32_t> classGroups(FillWith { }, (classes.size() + 31) / 32 * 2, 0);
        uint32_t numberOfClasses = 0;
        for (unsigned i = 0; i < classes.size(); ++i) {
            if (!(i % 32))
                classGroups[i / 32 * 2 + 1] = numberOfClasses;
            if (!classes[i])
                continue;
            classGroups[i / 32 * 2] |= 1u << i % 32;
            ++numberOfClasses;
        }
        for (uint32_t offset : classes) {
            if (offset)
                classGroups.append(offset);
        }
        header.classesOffset = append(classGroups);
        Vector<std::pair<uint32_t, uint32_t>> constructors;
        for (unsigned i = 0; i < m_program.layouts.size(); ++i) {
            if (m_program.layouts[i].constructor)
                constructors.append({ entrypointOf(m_program.layouts[i].constructor), i });
        }
        std::ranges::sort(constructors);
        Vector<uint32_t> constructorsAndClasses;
        for (auto [constructor, index] : constructors) {
            constructorsAndClasses.append(constructor);
            constructorsAndClasses.append(index);
        }
        header.numberOfConstructors = constructors.size();
        header.constructorsOffset = append(constructorsAndClasses);
        Vector<std::array<uint32_t, 3>> boundFunctions;
        for (auto& [key, bound] : m_program.boundFunctions) {
            auto& callee = m_program.at(FunctionID(static_cast<uint32_t>(key >> 2)));
            uint32_t containsTarget = callee.captureTypes.isEmpty() && !callee.hasManyClosures ? 0 : boundFunctionHoldsTarget;
            boundFunctions.append({ entrypointOf(bound), entrypointOf(FunctionID(static_cast<uint32_t>(key >> 2))), static_cast<uint32_t>(key & 3) | containsTarget });
        }
        std::ranges::sort(boundFunctions);
        Vector<uint32_t> boundFunctionsAndTargets;
        for (auto& entry : boundFunctions)
            boundFunctionsAndTargets.append(std::span { entry });
        header.numberOfBoundFunctions = boundFunctions.size();
        header.boundFunctionsOffset = append(boundFunctionsAndTargets);
        Vector<uint32_t> rows;
        uint32_t rowStart = m_program.structureDescriptors.size() + 1;
        for (auto& row : m_program.structureDescriptors) {
            rows.append(rowStart);
            rowStart += 1 + row.names.size();
        }
        rows.append(rowStart);
        for (auto& row : m_program.structureDescriptors) {
            rows.append(row.names.size() | row.inlineCapacity << 8 | row.hasNullPrototype << 16);
            for (StringID name : row.names)
                rows.append(name.index());
        }
        header.numberOfStructureDescriptors = m_program.structureDescriptors.size();
        header.structureDescriptorsOffset = append(rows);
        header.firstThisStructureDescriptor = std::min<uint32_t>(m_program.firstThisStructureDescriptor, m_program.structureDescriptors.size());
        {
            Vector<std::array<uint32_t, 3>> grown;
            for (auto& [from, name, to] : m_program.structureTransitions) {
                if (uint32_t identifier = name < m_program.identifierNumbers.size() ? m_program.identifierNumbers[name] : 0)
                    grown.append({ from, identifier, to });
            }
            std::ranges::sort(grown);
            Vector<uint32_t> words;
            for (size_t i = 0; i < grown.size(); ++i) {
                if (i && grown[i][0] == grown[i - 1][0] && grown[i][1] == grown[i - 1][1])
                    continue;
                words.append(std::span { grown[i] });
            }
            header.numberOfStructureTransitions = words.size() / 3;
            header.structureTransitionsOffset = append(words);
        }
        {
            uint32_t greatest = 0;
            for (uint32_t identifier : m_program.identifierNumbers)
                greatest = std::max(greatest, identifier);
            Vector<Vector<std::pair<uint32_t, uint8_t>, 1>> nameCells;
            nameCells.grow(greatest + 1);
            uint32_t maxCell = 0;
            for (uint32_t number = 0; number < m_program.structureDescriptors.size(); ++number) {
                auto& row = m_program.structureDescriptors[number];
                uint32_t cell = number + 1;
                maxCell = std::max(maxCell, cell);
                for (unsigned place = 0; place < row.names.size(); ++place) {
                    uint32_t index = row.names[place].index();
                    if (uint32_t identifier = index < m_program.identifierNumbers.size() ? m_program.identifierNumbers[index] : 0)
                        nameCells[identifier].append({ cell, static_cast<uint8_t>(place) });
                }
            }
            Vector<uint32_t> order;
            for (uint32_t identifier = 0; identifier <= greatest; ++identifier) {
                if (!nameCells[identifier].isEmpty())
                    order.append(identifier);
            }
            std::ranges::stable_sort(order, [&](uint32_t a, uint32_t b) {
                return nameCells[a].size() > nameCells[b].size();
            });
            Vector<uint32_t> displacements(FillWith { }, greatest + 1, 0);
            Vector<uint32_t> places(FillWith { }, maxCell + 1, 0);
            Vector<uint64_t> isTaken;
            auto bitsFrom = [&](size_t position) -> uint64_t {
                size_t word = position / 64;
                unsigned shift = position % 64;
                uint64_t bits = word < isTaken.size() ? isTaken[word] >> shift : 0;
                if (shift && word + 1 < isTaken.size())
                    bits |= isTaken[word + 1] << (64 - shift);
                return bits;
            };
            uint32_t firstFree = 0;
            for (uint32_t identifier : order) {
                auto& cells = nameCells[identifier];
                while (firstFree < places.size() && places[firstFree])
                    ++firstFree;
                uint32_t displacement = firstFree > cells[0].first ? firstFree - cells[0].first : 0;
                for (;; displacement += 64) {
                    uint64_t collides = 0;
                    for (auto& [cell, place] : cells) {
                        collides |= bitsFrom(static_cast<size_t>(displacement) + cell);
                        if (!~collides)
                            break;
                    }
                    if (~collides) {
                        displacement += std::countr_one(collides);
                        break;
                    }
                }
                while (places.size() < displacement + maxCell + 1)
                    places.append(0);
                while (isTaken.size() * 64 < places.size())
                    isTaken.append(0);
                for (auto& [cell, place] : cells) {
                    places[displacement + cell] = identifier << 8 | place;
                    isTaken[(displacement + cell) / 64] |= 1ULL << (displacement + cell) % 64;
                }
                RELEASE_ASSERT(identifier && identifier < 1u << 24);
                displacements[identifier] = displacement;
            }
            static constexpr ASCIILiteral namesOnObjectPrototype[] = { "constructor"_s, "toString"_s, "toLocaleString"_s, "valueOf"_s, "hasOwnProperty"_s, "propertyIsEnumerable"_s, "isPrototypeOf"_s, "__proto__"_s, "__defineGetter__"_s, "__defineSetter__"_s, "__lookupGetter__"_s, "__lookupSetter__"_s };
            for (uint32_t index = 0; index < m_program.identifierNumbers.size(); ++index) {
                uint32_t identifier = m_program.identifierNumbers[index];
                if (!identifier)
                    continue;
                for (auto name : namesOnObjectPrototype) {
                    if (equalSpans(m_program.strings[index], name.span8()))
                        displacements[identifier] |= Data::nameIsOnObjectPrototype;
                }
                if (isPrivateName(m_program.strings[index]) || isEnginePrivateName(m_program.strings[index]))
                    displacements[identifier] |= Data::nameIsOnObjectPrototype;
            }
            for (uint32_t& entry : places) {
                if (!entry)
                    entry = UINT32_MAX;
            }
            header.nameDisplacementsOffset = append(displacements);
            header.offsetsByNameAndStructureOffset = append(places);
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT image: slots by name and structure: ", order.size(), " names in structures, ", places.size(), " entries, ", displacements.size(), " displacements");
        }
        Vector<uint32_t> eagerBuiltinCells;
        for (size_t cell : m_program.comparedBuiltinCells)
            eagerBuiltinCells.append(static_cast<uint32_t>(cell));
        header.numberOfEagerBuiltinCells = eagerBuiltinCells.size();
        header.eagerBuiltinCellsOffset = append(eagerBuiltinCells);

        BitVector usesFieldTemplate;
        for (size_t i = 0; i < m_program.allocations.size(); ++i) {
            auto& site = m_program.allocations[i];
            if (!site.numberOfLiterals)
                continue;
            usesFieldTemplate.set(i);
            for (uint32_t literal = 0; literal <= site.numberOfLiterals; ++literal)
                usesFieldTemplate.set(site.firstLiteralAllocation + literal);
        }
        Vector<AllocationData> allocations;
        for (auto& allocation : m_program.allocations) {
            auto& layout = m_program.at(allocation.layout);
            uint32_t orderWord = 0;
            auto [expected, initialized] = existingFieldOrder(layout, allocation);
            if (initialized > maxFieldsInOrder)
                orderWord = orderWordOf(keysAreInLayoutOrder, 0);
            else if (isObjectLayout(layout.kind)) {
                size_t order = layout.orders.findIf([&](auto& candidate) {
                    return startsWith(candidate, expected);
                });
                RELEASE_ASSERT(order != notFound);
                orderWord = orderWordOf(static_cast<uint32_t>(order), initialized);
            }
            RELEASE_ASSERT(initialized || !orderWord);
            RELEASE_ASSERT(!(layout.shape >> 31));
            allocations.append(AllocationData { layout.shape, usesFieldTemplate.get(allocations.size()), orderWord, allocation.size, entrypointOf(allocation.constructedBy) });
        }
        header.numberOfAllocations = allocations.size();
        header.allocationsOffset = append(allocations);
        size_t sizeAfterLayouts = m_result.size();

        Vector<uint32_t> shapeSets(FillWith { }, (m_program.shapeSetBits.size() + 31) / 32, 0);
        for (size_t bit : m_program.shapeSetBits)
            shapeSets[bit / 32] |= 1u << bit % 32;
        header.shapeSetsOffset = append(shapeSets);

        header.sizeOfStatics = m_program.sizeOfStatics;
        header.sizeOfLazilyInitializedStatics = m_program.sizeOfLazilyInitializedStatics;
        struct InitialWord {
            uint32_t offset;
            int64_t bits;
        };
        Vector<InitialWord> initialWords;
        for (auto& variable : m_program.variables) {
            if (variable.isNeverReadUninitialized || variable.startsZero || variable.storesUndefinedAsZero)
                continue;
            int64_t bits = 0;
            switch (repOf(variable.store)) {
            case Rep::JSValue:
            case Rep::LiteralString:
                bits = variable.startsUndefined ? JSValue::encode(jsUndefined()) : 0;
                break;
            case Rep::OptionalInt64:
                bits = variable.startsUndefined ? undefinedInteger : absentInteger;
                break;
            case Rep::OptionalDouble:
                bits = static_cast<int64_t>(variable.startsUndefined ? undefinedDouble : absentDouble);
                break;
            default:
                RELEASE_ASSERT(variable.startsUndefined);
                break;
            }
            if (bits) {
                RELEASE_ASSERT(!(variable.offset % sizeof(uint64_t)));
                initialWords.append({ variable.offset, bits });
            }
        }
        std::ranges::sort(initialWords, { }, &InitialWord::offset);
        Vector<uint32_t> staticsRuns;
        for (unsigned first = 0; first < initialWords.size();) {
            unsigned end = first + 1;
            while (end < initialWords.size() && initialWords[end].bits == initialWords[first].bits && initialWords[end].offset == initialWords[end - 1].offset + sizeof(uint64_t) && initialWords[end].offset != m_program.sizeOfLazilyInitializedStatics)
                ++end;
            uint64_t bits = initialWords[first].bits;
            staticsRuns.appendList({ static_cast<uint32_t>(initialWords[first].offset / sizeof(uint64_t)), end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
            first = end;
        }
        header.numberOfStaticsRuns = staticsRuns.size() / 4;
        header.staticsRunsOffset = append(staticsRuns);
        header.numberOfGlobalPropertyCaches = m_program.numberOfGlobalPropertyCaches;
        header.globalPropertyCachesOffset = m_program.globalPropertyCachesOffset;
        header.staticFieldSlotsOffset = m_program.staticFieldSlotsOffset;
        Vector<uint32_t> roots;
        static_assert(sizeof(EncodedJSValue) * 32 == 1 << Data::cardShift);
        Vector<uint32_t> rootMasks;
        rootMasks.fill(0, (m_program.sizeOfStatics + (1 << Data::cardShift) - 1) >> Data::cardShift);
        auto addRootWithCard = [&](uint32_t offset) {
            RELEASE_ASSERT(!(offset % sizeof(EncodedJSValue)) && offset + sizeof(EncodedJSValue) <= m_program.sizeOfStatics);
            rootMasks[offset >> Data::cardShift] |= 1u << (offset / sizeof(EncodedJSValue) % 32);
        };
        for (uint32_t i = 0; i < m_program.numberOfGlobalPropertyCaches; ++i)
            addRootWithCard(m_program.globalPropertyCachesOffset + i * sizeof(GlobalPropertyCache) + OBJECT_OFFSETOF(GlobalPropertyCache, value));
        for (auto& variable : m_program.variables) {
            if (!holdsCells(variable.store))
                continue;
            if (variable.hasCard)
                addRootWithCard(variable.offset);
            else
                roots.append(variable.offset);
        }
        for (uint32_t i = 0; i < m_program.numberOfCopyOnWriteButterflies; ++i)
            addRootWithCard(m_program.copyOnWriteButterfliesOffset + i * sizeof(JSCell*));
        for (uint32_t i = 0; i < m_program.sharedRegExpObjectOffsets.size(); ++i)
            addRootWithCard(m_program.sharedRegExpObjectsOffset + i * sizeof(JSCell*));
        header.numberOfRoots = roots.size();
        header.rootsOffset = append(roots);
        header.numberOfCards = rootMasks.size();
        header.rootMasksOffset = append(rootMasks);
        header.copyOnWriteButterfliesOffset = m_program.copyOnWriteButterfliesOffset;
        Vector<std::array<uint32_t, 2>> arrayLiterals;
        for (auto& elements : m_program.arrayLiteralElements)
            arrayLiterals.append({ append(elements), static_cast<uint32_t>(elements.size()) });
        header.arrayLiteralsOffset = append(arrayLiterals);
        Vector<uint32_t> tablesInImage;
        for (auto& table : m_program.tablesInImage) {
            auto keys = table.keys.map([&](StringID key) {
                return std::pair { key.index(), m_program.string(key) };
            });
            uint32_t offset = append(TableInImage::encode(keys.span(), table.values.span()).span(), 8);
            at<uint32_t>(offset + TableInImage::offsetOfOffsetInData()) = offset;
            tablesInImage.append(offset);
        }
        header.tablesInImageOffset = append(tablesInImage);

        header.numberOfFunctions = m_program.functions.size();
        Vector<uint32_t> names;
        Vector<uint8_t> kinds;
        Vector<uint16_t> lengths;
        Vector<uint8_t> lengthsInBytes;
        for (auto& function : m_program.functions) {
            BuiltinID builtin = m_program.builtinImplementedBy(function);
            StringID builtinNameString = builtin ? m_program.stringOf(Builtins::singleton().at(builtin).nameName) : StringID();
            names.append((builtinNameString ? builtinNameString : function.name).index());
            static_assert(static_cast<uint8_t>(FunctionKind::Bound) < sharedBodyFunctionKind && sharedBodyFunctionKind <= functionKindMask);
            kinds.append(kindByteOf(m_program, function));
            RELEASE_ASSERT(function.length <= (m_program.functionLengthsAreBytes ? std::numeric_limits<uint8_t>::max() : std::numeric_limits<uint16_t>::max()));
            if (m_program.functionLengthsAreBytes)
                lengthsInBytes.append(static_cast<uint8_t>(function.length));
            else
                lengths.append(static_cast<uint16_t>(function.length));
        }
        header.functionsOffset = append(names);
        header.functionKindsOffset = append(kinds);
        header.functionLengthsAreBytes = m_program.functionLengthsAreBytes;
        uint32_t firstClosureFunction = m_program.functions.size();
        uint32_t endOfClosureFunctions = m_program.functions.size();
        bool hasClosureEntrypoint = false;
        auto recordClosureEntrypoint = [&](uint32_t function) {
            firstClosureFunction = std::min(firstClosureFunction, function);
            endOfClosureFunctions = hasClosureEntrypoint ? std::max(endOfClosureFunctions, function + 1) : function + 1;
            hasClosureEntrypoint = true;
        };
        for (auto& [function, entrypoint] : m_program.genericEntrypoints)
            recordClosureEntrypoint(entrypoint.index());
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (m_program.isResumeFunction(m_program.functions[i]))
                recordClosureEntrypoint(i);
        }
        uint32_t indexedFunctionsEnd = 0;
        for (auto& evaluation : m_program.moduleEvaluations) {
            for (FunctionID callee : evaluation.callees) {
                if (!m_program.moduleEvaluationIndexByFunction.contains(callee.index()))
                    indexedFunctionsEnd = std::max(indexedFunctionsEnd, callee.index() + 1);
            }
        }
        uint32_t omittedClosureWordCount = firstClosureFunction > indexedFunctionsEnd ? firstClosureFunction - indexedFunctionsEnd : 0;
        header.firstClosureFunction = firstClosureFunction;
        header.endOfClosureFunctions = endOfClosureFunctions;
        header.omittedClosureWordCount = omittedClosureWordCount;
        auto appendClosureTable = [&](std::span<const uint8_t> rows, size_t rowSize) {
            auto kept = rows.subspan(firstClosureFunction * rowSize, (endOfClosureFunctions - firstClosureFunction) * rowSize);
            if (m_result.size() < firstClosureFunction * rowSize)
                allocate(firstClosureFunction * rowSize - m_result.size());
            uint32_t offset = allocate(kept.size());
            memcpySpan(m_result.mutableSpan().subspan(offset), kept);
            RELEASE_ASSERT(offset >= firstClosureFunction * rowSize);
            return safeCast<uint32_t>(offset - firstClosureFunction * rowSize);
        };
        header.functionLengthsOffset = m_program.functionLengthsAreBytes ? appendClosureTable(asBytes(lengthsInBytes.span()), sizeof(uint8_t)) : appendClosureTable(asBytes(lengths.span()), sizeof(uint16_t));
        uint32_t numberOfModuleEvaluations = m_program.moduleEvaluations.size();
        Vector<uint32_t> moduleEvaluations(2 * numberOfModuleEvaluations);
        auto offsetOf = [&](VariableID variable) {
            return m_program.variables[variable.index()].offset;
        };
        for (uint32_t row = 0; row < numberOfModuleEvaluations; ++row) {
            auto& evaluation = m_program.moduleEvaluations[row];
            moduleEvaluations[row] = offsetOf(evaluation.isEvaluated);
            moduleEvaluations[numberOfModuleEvaluations + row] = moduleEvaluations.size();
            moduleEvaluations.append(evaluation.error ? offsetOf(evaluation.error) : std::numeric_limits<uint32_t>::max());
            moduleEvaluations.append(moduleEvaluations.size() + 1 + 2 * (evaluation.callees.size() + evaluation.fetches.size()));
            size_t precedingCalleeCount = 0;
            size_t nextFetch = 0;
            auto appendFetches = [&] {
                for (; nextFetch < evaluation.fetches.size() && evaluation.fetches[nextFetch].precedingCalleeCount <= precedingCalleeCount; ++nextFetch) {
                    moduleEvaluations.append(Program::ModuleEvaluation::fetchMarker);
                    moduleEvaluations.append(evaluation.fetches[nextFetch].externalModule);
                }
            };
            for (FunctionID callee : evaluation.callees) {
                appendFetches();
                ++precedingCalleeCount;
                auto other = m_program.moduleEvaluationIndexByFunction.find(callee.index());
                bool isRow = other != m_program.moduleEvaluationIndexByFunction.end();
                moduleEvaluations.append(isRow ? offsetOf(m_program.moduleEvaluations[other->value].isEvaluated) : std::numeric_limits<uint32_t>::max());
                moduleEvaluations.append(isRow ? other->value : callee.index() + omittedClosureWordCount);
            }
            appendFetches();
            RELEASE_ASSERT(nextFetch == evaluation.fetches.size());
        }
        header.moduleEvaluationsOffset = append(moduleEvaluations);
        Vector<std::array<uint8_t, 2>> closureCounts;
        closureCounts.fill({ 0, 0 }, m_program.functions.size());
        auto setCounts = [&](FunctionID closureEntrypoint, const Function& function) {
            auto [references, captures] = m_program.closureReferenceAndCaptureCounts(function);
            closureCounts[closureEntrypoint.index()] = { static_cast<uint8_t>(references), static_cast<uint8_t>(captures) };
        };
        for (auto& [function, entrypoint] : m_program.genericEntrypoints)
            setCounts(entrypoint, m_program.functions[function]);
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (m_program.isResumeFunction(m_program.functions[i]))
                setCounts(FunctionID(i), m_program.functions[i]);
        }
        header.closureCountsOffset = appendClosureTable(asBytes(closureCounts.span()), sizeof(closureCounts[0]));
        Vector<uint8_t> slimClosures;
        slimClosures.fill(0, (m_program.functions.size() + 7) / 8);
        for (auto& [function, entrypoint] : m_program.genericEntrypoints) {
            if (m_program.usesSlimClosures(m_program.functions[function]))
                slimClosures[entrypoint.index() / 8] |= 1 << entrypoint.index() % 8;
        }
        header.slimClosuresOffset = append(slimClosures);
        Vector<uint8_t> assignedNames;
        assignedNames.fill(0, (m_program.functions.size() + 7) / 8);
        for (auto& [function, entrypoint] : m_program.genericEntrypoints) {
            if (m_program.functions[function].has(Format::FunctionHasExplicitName))
                assignedNames[entrypoint.index() / 8] |= 1 << entrypoint.index() % 8;
        }
        header.givenNamesOffset = append(assignedNames);
        RELEASE_ASSERT(m_program.entryModules.size() == m_program.entries.size());
        Vector<uint32_t> entryPoints(m_program.entryModules.span());
        header.entryPointsInOrderOffset = append(entryPoints);
        std::ranges::sort(entryPoints);
        header.numberOfEntryPoints = entryPoints.size();
        header.entryPointsOffset = append(entryPoints);
        Vector<uint32_t> builtinModules;
        for (auto& [id, function] : m_program.builtinModules)
            builtinModules.appendList({ id, entrypointOf(function) });
        header.numberOfBuiltinModules = m_program.builtinModules.size();
        header.builtinModulesOffset = append(builtinModules);
        Vector<uint32_t> builtinFunctions;
        for (auto& [id, function, visibility] : m_program.builtinFunctions)
            builtinFunctions.appendList({ id, entrypointOf(function), visibility });
        header.numberOfBuiltinFunctions = m_program.builtinFunctions.size();
        header.builtinFunctionsOffset = append(builtinFunctions);
        Vector<uint32_t> preludeFunctions;
        for (unsigned which = 0; which < numberOfPreludeFunctions; ++which)
            preludeFunctions.append(which < m_program.preludeFunctions.size() && m_program.preludeFunctions[which] ? m_program.preludeFunctions[which].index() : UINT32_MAX);
        header.preludeFunctionsOffset = append(preludeFunctions);
        Vector<std::pair<uint32_t, uint32_t>> generatorFunctions;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.declaredAs & functionIsGenerator && function.resumeFunction && m_program.genericEntrypoints.contains(i))
                generatorFunctions.append({ entrypointOf(FunctionID(i)), function.resumeFunction.index() | (function.takesCalleeClosure || function.rampForClosures ? Data::generatorTakesCalleeClosure : 0) });
        }
        std::ranges::sort(generatorFunctions);
        Vector<uint32_t> generatorAndResumeFunctions;
        for (auto [generatorFunction, resumeFunction] : generatorFunctions) {
            generatorAndResumeFunctions.append(generatorFunction);
            generatorAndResumeFunctions.append(resumeFunction);
        }
        header.numberOfGeneratorFunctions = generatorFunctions.size();
        header.generatorFunctionsOffset = append(generatorAndResumeFunctions);
        Vector<uint32_t> asyncGeneratorBodies;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (m_program.functions[i].capturesExceptions)
                asyncGeneratorBodies.append(i);
        }
        header.numberOfAsyncGeneratorBodies = asyncGeneratorBodies.size();
        header.asyncGeneratorBodiesOffset = append(asyncGeneratorBodies);
        Vector<uint32_t> sloppyPlainFunctions;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.has(Format::FunctionIsSloppy) && function.kind == FunctionKind::Plain && !(function.declaredAs & (functionIsAsync | functionIsGenerator)) && m_program.genericEntrypoints.contains(i))
                sloppyPlainFunctions.append(entrypointOf(FunctionID(i)));
        }
        std::ranges::sort(sloppyPlainFunctions);
        header.numberOfSloppyPlainFunctions = sloppyPlainFunctions.size();
        header.sloppyPlainFunctionsOffset = append(sloppyPlainFunctions);
        Vector<uint32_t> sloppyFunctionBoundaries;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (m_program.functions[i].has(Format::FunctionIsSloppy) != (sloppyFunctionBoundaries.size() & 1))
                sloppyFunctionBoundaries.append(i);
        }
        header.numberOfSloppyFunctionBoundaries = sloppyFunctionBoundaries.size();
        header.sloppyFunctionBoundariesOffset = append(sloppyFunctionBoundaries);
        Vector<std::pair<uint32_t, uint32_t>> constructorFunctions;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (uint32_t allocation = m_program.functions[i].thisAllocation; allocation != Format::none)
                constructorFunctions.append({ entrypointOf(FunctionID(i)), allocation });
        }
        std::ranges::sort(constructorFunctions);
        Vector<uint32_t> constructorFunctionsAndAllocations;
        for (auto [constructorFunction, allocation] : constructorFunctions) {
            constructorFunctionsAndAllocations.append(constructorFunction);
            constructorFunctionsAndAllocations.append(allocation);
        }
        header.numberOfConstructorFunctions = constructorFunctions.size();
        header.constructorFunctionsOffset = append(constructorFunctionsAndAllocations);
        Vector<std::pair<uint32_t, uint32_t>> rejectedFreshObjects;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.thisAllocation == Format::none)
                continue;
            auto stores = initialStoresOf(function).first(function.numberOfInitialStores);
            for (LayoutID subtype : m_program.at(m_program.allocations[function.thisAllocation].layout).subtypes) {
                auto& layout = m_program.at(subtype);
                bool storesAllInitially = std::ranges::all_of(layout.fields, [&](auto& field) {
                    return !(field.flags & fieldIsNeverObservedAbsent) || std::ranges::any_of(stores, [&](ValueID store) {
                        return function.at(store).string() == field.name;
                    });
                });
                if (layout.shape && !storesAllInitially)
                    rejectedFreshObjects.append({ entrypointOf(FunctionID(i)), layout.shape });
            }
        }
        std::ranges::sort(rejectedFreshObjects);
        Vector<uint32_t> rejectedFunctionsAndShapes;
        for (auto [function, shape] : rejectedFreshObjects) {
            rejectedFunctionsAndShapes.append(function);
            rejectedFunctionsAndShapes.append(shape);
        }
        header.numberOfRejectedFreshObjects = rejectedFreshObjects.size();
        header.rejectedFreshObjectsOffset = append(rejectedFunctionsAndShapes);
        Vector<uint32_t> functionsWithoutFieldNamesOnPrototype;
        for (auto& [index, entrypoint] : m_program.genericEntrypoints) {
            if (m_program.functions[index].has(Format::FunctionHasNoFieldNamesOnPrototype))
                functionsWithoutFieldNamesOnPrototype.append(entrypoint.index());
        }
        std::ranges::sort(functionsWithoutFieldNamesOnPrototype);
        header.numberOfFunctionsWithoutFieldNamesOnPrototype = functionsWithoutFieldNamesOnPrototype.size();
        header.functionsWithoutFieldNamesOnPrototypeOffset = append(functionsWithoutFieldNamesOnPrototype);
        Vector<std::pair<uint32_t, uint32_t>> thisPropertyCounts;
        Vector<std::pair<uint32_t, uint32_t>> functionsAndEntrypoints;
        for (auto& [index, entrypoint] : m_program.genericEntrypoints)
            functionsAndEntrypoints.append({ index, entrypoint.index() });
        Vector<uint32_t> thisCounts;
        thisCounts.fill(0, functionsAndEntrypoints.size());
        Vector<Vector<StringID, 4>> namesAssignedByCreators(m_program.functions.size());
        for (auto& maker : m_program.functions) {
            UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> created;
            for (auto& block : maker.blocks) {
                for (ValueID id : block.values) {
                    auto& value = maker.at(id);
                    if (value.opcode == Opcode::Construct || value.opcode == Opcode::CreateThis) {
                        if (FunctionID expected = maker.at(value.child(0)).predictedClosure())
                            created.add(id.index(), expected.index());
                        continue;
                    }
                    if (value.opcode != Opcode::PutById || created.isEmpty())
                        continue;
                    ValueID object = value.child(0);
                    while (maker.at(object).opcode == Opcode::Cast && maker.at(object).castReason() == CastReason::Upcast)
                        object = maker.at(object).child(0);
                    auto found = created.find(object.index());
                    if (found != created.end() && !namesAssignedByCreators[found->value].contains(value.string()))
                        namesAssignedByCreators[found->value].append(value.string());
                }
            }
        }
        forEachIndexInParallel(functionsAndEntrypoints.size(), [&](unsigned i) {
            uint32_t index = functionsAndEntrypoints[i].first;
            auto& function = m_program.functions[index];
            if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Constructor)
                return;
            Vector<StringID, 16> names;
            unsigned numberOfKeys = 0;
            bool coversWholeChain = collectPropertiesAssignedToThis(m_program, FunctionID(index), names, numberOfKeys);
            Vector<StringID, 16> selfAssignedNames;
            unsigned selfAssignedKeyCount = 0;
            collectPropertiesAssignedToThis(m_program, FunctionID(index), selfAssignedNames, selfAssignedKeyCount, false);
            for (StringID name : namesAssignedByCreators[index]) {
                if (!names.contains(name))
                    names.append(name);
                if (!selfAssignedNames.contains(name))
                    selfAssignedNames.append(name);
            }
            auto boundedCount = [](size_t number) {
                return static_cast<uint32_t>(std::min<size_t>(number, Data::propertyCountMask));
            };
            if (names.size() + numberOfKeys)
                thisCounts[i] = boundedCount(names.size() + numberOfKeys) | boundedCount(selfAssignedNames.size() + selfAssignedKeyCount) << Data::propertyCountBits | (coversWholeChain ? Data::propertyCountIncludesPrototypeChain : 0);
        });
        for (unsigned i = 0; i < thisCounts.size(); ++i) {
            if (thisCounts[i])
                thisPropertyCounts.append({ functionsAndEntrypoints[i].second, thisCounts[i] });
        }
        std::ranges::sort(thisPropertyCounts);
        Vector<uint32_t> functionsAndThisPropertyCounts;
        for (auto [function, count] : thisPropertyCounts) {
            functionsAndThisPropertyCounts.append(function);
            functionsAndThisPropertyCounts.append(count);
        }
        header.numberOfThisPropertyCounts = thisPropertyCounts.size();
        header.thisPropertyCountsOffset = append(functionsAndThisPropertyCounts);
        BitVector isReservedFieldName;
        for (auto& layout : m_program.layouts) {
            if (!isObjectLayout(layout.kind))
                continue;
            for (auto& field : layout.fields) {
                if (field.name && !isPrivateName(m_program.string(field.name)) && !m_program.prototypePropertyNames.get(field.name.index()))
                    isReservedFieldName.set(field.name.index());
            }
        }
        Vector<uint32_t> reservedFieldNames;
        for (size_t index : isReservedFieldName)
            reservedFieldNames.append(static_cast<uint32_t>(index));
        header.numberOfReservedFieldNames = reservedFieldNames.size();
        header.reservedFieldNamesOffset = append(reservedFieldNames);
        BitVector isFieldName;
        for (auto& layout : m_program.layouts) {
            if (!isObjectLayout(layout.kind))
                continue;
            for (auto& field : layout.fields) {
                if (field.name && !m_program.string(field.name).empty() && !isPrivateName(m_program.string(field.name)) && !isEnginePrivateName(m_program.string(field.name)))
                    isFieldName.set(field.name.index());
            }
        }
        Vector<std::array<uint32_t, 2>> fieldNames;
        for (size_t index : isFieldName) {
            String text = String::fromUTF8(m_program.string(StringID(static_cast<uint32_t>(index))));
            fieldNames.append({ text.isNull() ? UINT32_MAX : text.impl()->hash(), static_cast<uint32_t>(index) });
        }
        std::ranges::sort(fieldNames);
        header.numberOfFieldNames = fieldNames.size();
        header.fieldNamesOffset = append(fieldNames);
        header.isCompiledForAudit = Options::auditAOTTypedAssertions();
        header.logsTypeErrors = m_program.logsTypeErrors;
        header.mayRunInMultipleInstances = m_program.mayRunInMultipleInstances;
        header.numberOfSiteWords = m_program.sites.size();
        header.sitesOffset = append(m_program.sites);
        header.countsAllocations = Options::countAOTTypedAllocations();
        header.usesWideReferences = Options::useAOTWideReferences();
        header.numberOfLiteralStrings = m_program.literalStrings.size();
        header.literalStringsOffset = append(Vector<uint32_t>(m_program.literalStrings.size(), [&](size_t number) {
            return m_program.literalStrings[number].index() + 1;
        }));
        {
            uint32_t mask = WTF::roundUpToPowerOfTwo(std::max<uint32_t>(m_program.literalStrings.size(), 4) * 2) - 1;
            Vector<uint32_t> table(FillWith { }, mask + 1, 0);
            uint32_t maxLength = 0;
            for (uint32_t index = 0; index < m_program.literalStrings.size(); ++index) {
                auto bytes = m_program.string(m_program.literalStrings[index]);
                uint32_t hash;
                if (charactersAreAllASCII(byteCast<Latin1Character>(bytes))) {
                    hash = StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(byteCast<Latin1Character>(bytes));
                    maxLength = std::max<uint32_t>(maxLength, bytes.size());
                } else {
                    Vector<char16_t> units = decodeProgramString(bytes);
                    hash = StringHasher::computeHashAndMaskTop8Bits<char16_t>(units.span());
                    maxLength = std::max<uint32_t>(maxLength, units.size());
                }
                uint32_t place = hash & mask;
                while (table[place])
                    place = (place + 1) & mask;
                table[place] = Data::literalStringTableEntry(index, hash);
            }
            header.literalStringTableOffset = append(table);
            header.literalStringTableMask = mask;
            header.maxLiteralStringLength = maxLength;
        }
        {
            Vector<uint32_t> owner(m_program.functions.size(), [](size_t index) {
                return static_cast<uint32_t>(index);
            });
            for (unsigned i = 0; i < m_program.functions.size(); ++i) {
                if (FunctionID resume = m_program.functions[i].resumeFunction; resume && !m_program.functions[i].isRampForClosures)
                    owner[resume.index()] = i;
                if (FunctionID start = m_program.functions[i].startFunction)
                    owner[start.index()] = i;
            }
            Vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> entries;
            for (unsigned i = 0; i < m_program.functions.size(); ++i) {
                const Function& function = m_program.functions[i];
                if (function.has(Format::FunctionIsHiddenInStackTraces) && !function.isGenericEntrypoint())
                    entries.append({ function.module.index(), function.position, function.name.index(), i });
            }
            std::ranges::sort(entries);
            for (unsigned i = 1; i < entries.size(); ++i) {
                auto& [module, position, name, function] = entries[i];
                auto& first = entries[i - 1];
                if (std::get<0>(first) != module || std::get<1>(first) != position || std::get<2>(first) != name)
                    continue;
                if (owner[function] == function)
                    owner[function] = owner[std::get<3>(first)];
            }
            for (unsigned i = 0; i < m_program.functions.size() && !entries.isEmpty(); ++i) {
                const Function& function = m_program.functions[i];
                if (function.has(Format::FunctionIsHiddenInStackTraces) || function.isGenericEntrypoint() || owner[i] != i)
                    continue;
                std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> key { function.module.index(), function.position, function.name.index(), 0 };
                auto found = std::ranges::lower_bound(entries, key);
                if (found != entries.end() && std::get<0>(*found) == std::get<0>(key) && std::get<1>(*found) == std::get<1>(key) && std::get<2>(*found) == std::get<2>(key))
                    owner[i] = owner[std::get<3>(*found)];
            }
            Vector<uint32_t> first(owner.size(), [](size_t index) {
                return static_cast<uint32_t>(index);
            });
            for (unsigned i = 0; i < owner.size(); ++i)
                first[owner[i]] = std::min(first[owner[i]], i);
            for (auto& one : owner)
                one = first[one];
            for (auto& [function, entrypoint] : m_program.genericEntrypoints)
                owner[entrypoint.index()] = owner[function];
            Vector<uint32_t> aliasRuns;
            Vector<uint16_t> ownerLowHalves;
            Vector<uint8_t> ownerHighBytes;
            RELEASE_ASSERT(!(owner.size() >> 24));
            unsigned afterLastAlias = 0;
            for (unsigned i = 0; i < owner.size(); ++i) {
                if (owner[i] == i)
                    continue;
                if (aliasRuns.isEmpty() || i != afterLastAlias || owner[i] < owner[i - 1])
                    aliasRuns.appendList({ static_cast<uint32_t>(i), static_cast<uint32_t>(ownerHighBytes.size()) });
                afterLastAlias = i + 1;
                ownerLowHalves.append(static_cast<uint16_t>(owner[i]));
                ownerHighBytes.append(static_cast<uint8_t>(owner[i] >> 16));
            }
            header.numberOfFunctionAliases = ownerHighBytes.size();
            header.numberOfFunctionAliasRuns = aliasRuns.size() / 2;
            Vector<uint8_t> aliases;
            aliases.append(asBytes(aliasRuns.span()));
            aliases.append(asBytes(ownerLowHalves.span()));
            aliases.appendVector(ownerHighBytes);
            header.functionAliasesOffset = append(aliases);
            header.firstGenericEntrypoint = m_program.firstGenericEntrypoint;
            header.numberOfConsecutiveGenericEntrypoints = m_program.numberOfConsecutiveGenericEntrypoints;
        }
        if (header.isCompiledForAudit) {
            Vector<uint32_t> sources;
            for (auto& function : m_program.functions) {
                StringID path = function.module ? m_program.modules[function.module.index()].displayPath : StringID();
                sources.append((path ? path : m_program.emptyString).index());
            }
            header.functionSourcesOffset = append(sources);
        }
        at<Data>(0) = header;
        Vector<uint32_t> makers;
        Vector<std::array<uint32_t, 2>> allocationsByConstructor;
        {
            const Data& data = at<Data>(0);
            auto* allocations = data.at<AllocationData>(data.allocationsOffset);
            Vector<uint32_t> found;
            auto makes = [&](uint32_t function, const AllocationData& allocation) {
                const LayoutData& layout = *data.at<LayoutData>(data.at<uint32_t>(data.layoutsOffset)[allocation.shape]);
                bool hasDefinitelyInitializedField = std::ranges::any_of(std::span { layout.fields(), layout.numberOfFields }, [](const FieldData& field) {
                    return field.flags & fieldIsNeverObservedAbsentAsDeclared;
                });
                found.append(function << 2 | (layout.mayHaveOtherPrototype ? Data::createsObjectsOfFlaggedLayout : 0) | (hasDefinitelyInitializedField ? Data::createsObjectsWithDefinitelyInitializedFields : 0));
            };
            for (uint32_t i = 0; i < data.numberOfAllocations; ++i) {
                if (allocations[i].constructedBy == LayoutData::noBase)
                    continue;
                makes(allocations[i].constructedBy, allocations[i]);
                allocationsByConstructor.append({ allocations[i].constructedBy, i });
            }
            for (auto& functionAndAllocation : std::span { data.at<std::array<uint32_t, 2>>(data.constructorFunctionsOffset), data.numberOfConstructorFunctions })
                makes(functionAndAllocation[0], allocations[functionAndAllocation[1]]);
            std::ranges::sort(found);
            for (uint32_t maker : found) {
                if (!makers.isEmpty() && makers.last() >> 2 == maker >> 2)
                    makers.last() |= maker;
                else
                    makers.append(maker);
            }
            std::ranges::sort(allocationsByConstructor);
        }
        header.numberOfObjectCreators = makers.size();
        header.objectCreatorsOffset = append(makers);
        header.numberOfAllocationsByConstructor = allocationsByConstructor.size();
        header.allocationsByConstructorOffset = append(allocationsByConstructor);
        header.functionTextsOffset = appendFunctionSources();
        at<Data>(0) = header;
        {
            const Data& data = at<Data>(0);
            auto classWithIndex = [&](uint32_t index) -> LayoutData* {
                uint32_t offset = data.offsetOfClass(index);
                return offset ? &at<LayoutData>(offset) : nullptr;
            };
            auto baseOf = [&](const LayoutData& layout) -> LayoutData* {
                return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base());
            };
            auto hasNoLookupName = [&](auto& method) {
                return method.flags & methodHasComputedName || parseIndex(m_program.string(StringID(method.name)));
            };
            auto isFunctionOwnProperty = [&](auto& method) {
                auto name = m_program.string(StringID(method.name));
                return !(method.flags & methodHasComputedName) && (equalSpans(name, asBytes("name"_s.span8())) || equalSpans(name, asBytes("length"_s.span8())));
            };
            for (uint32_t index = 0; index < data.numberOfLayoutIndices; ++index) {
                LayoutData* ofClass = classWithIndex(index);
                if (!ofClass)
                    continue;
                ofClass->createsMembersLazily = ofClass->isEvaluatedOnce && std::ranges::none_of(ofClass->methods(), hasNoLookupName);
                bool isAhead = ofClass->createsMembersLazily;
                for (const LayoutData* next = ofClass; next; next = baseOf(*next))
                    isAhead &= next->isEvaluatedOnce && !next->hasUntypedBase;
                ofClass->createsPrototypeBeforeClassObject = isAhead;
                bool isCreatedOnChange = ofClass->createsMembersLazily && std::ranges::none_of(ofClass->staticMethods(), hasNoLookupName) && std::ranges::none_of(ofClass->staticMethods(), isFunctionOwnProperty);
                for (const LayoutData* next = baseOf(*ofClass); next; next = baseOf(*next))
                    isCreatedOnChange &= next->isEvaluatedOnce;
                ofClass->createsClassObjectOnMutation = isCreatedOnChange;
            }
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT data: ", m_result.size(), " bytes, of which ", sizeBeforeLayouts - sizeBeforeTypes, " for ", m_typeOffsets.size(), " types, ", sizeAfterLayouts - sizeBeforeLayouts, " for ", m_program.numberOfShapes, " layouts and ", allocations.size(), " allocations, ", shapeSets.sizeInBytes(), " for shape sets");
        return WTF::move(m_result);
    }

private:
    uint32_t appendFunctionSources()
    {
        Vector<std::array<uint32_t, 2>> values;
        for (auto& [function, entrypoint] : m_program.genericEntrypoints)
            values.append({ entrypoint.index(), function });
        std::ranges::sort(values);
        Vector<std::array<uint32_t, 3>> entries;
        Vector<uint32_t> paths;
        Vector<uint8_t> text;
        Vector<uint32_t> moduleSourceOffset(m_program.modules.size(), [](size_t) {
            return UINT32_MAX;
        });
        Vector<uint32_t> without;
        struct RetainedSource {
            uint32_t start;
            uint32_t end;
            uint32_t place;
        };
        Vector<Vector<RetainedSource>> retainedModuleSource(m_program.modules.size());
        for (auto [entrypoint, number] : values) {
            const Function& function = m_program.functions[number];
            if (!function.module || function.kind == FunctionKind::Bound || function.declaredAs & functionIsInBuiltinModule || function.has(Format::FunctionIsBuiltin) || !function.sourceEnd)
                continue;
            retainedModuleSource[function.module.index()].append({ function.sourceStart, function.sourceEnd, 0 });
        }
        for (auto& kept : retainedModuleSource) {
            std::ranges::sort(kept, [](const RetainedSource& a, const RetainedSource& b) {
                return a.start != b.start ? a.start < b.start : a.end > b.end;
            });
            size_t count = 0;
            for (const RetainedSource& next : kept) {
                if (count && next.start < kept[count - 1].end)
                    kept[count - 1].end = std::max(kept[count - 1].end, next.end);
                else
                    kept[count++] = next;
            }
            kept.shrink(count);
        }
        for (auto [entrypoint, number] : values) {
            const Function& function = m_program.functions[number];
            if (!function.module || function.kind == FunctionKind::Bound || function.declaredAs & functionIsInBuiltinModule || function.has(Format::FunctionIsBuiltin))
                continue;
            if (!function.sourceStart && !function.sourceEnd) {
                if (!function.has(Format::FunctionIsHiddenInStackTraces))
                    without.append(number);
                continue;
            }
            const Module& module = m_program.modules[function.module.index()];
            uint32_t& place = moduleSourceOffset[function.module.index()];
            if (function.sourceEnd) {
                auto& kept = retainedModuleSource[function.module.index()];
                if (place == UINT32_MAX) {
                    place = 0;
                    for (RetainedSource& range : kept) {
                        range.place = safeCast<uint32_t>(text.size());
                        text.append(module.printed.subspan(range.start, range.end - range.start));
                    }
                }
                auto behind = std::ranges::upper_bound(kept, function.sourceStart, { }, &RetainedSource::start);
                RELEASE_ASSERT(behind != kept.begin());
                const RetainedSource& around = *std::prev(behind);
                RELEASE_ASSERT(function.sourceEnd <= around.end);
                entries.append({ entrypoint, around.place + function.sourceStart - around.start, around.place + function.sourceEnd - around.start });
                continue;
            }
            if (place == UINT32_MAX) {
                auto path = m_program.string(module.path);
                place = paths.size() / 2;
                paths.appendList({ append(path), safeCast<uint32_t>(path.size()) });
            }
            entries.append({ entrypoint, place, function.sourceStart - 1 });
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT: the text of functions: ", entries.size(), " functions that can be values have one, ", without.size(), " have none; ", text.size(), " bytes of text");
        if (entries.isEmpty())
            return 0;
        if (Options::verboseAOTCompilation()) [[unlikely]] {
            for (uint32_t number : without) {
                const Function& function = m_program.functions[number];
                LineColumn where = m_program.lineColumnFor(function.module, function.position);
                dataLogLn("AOT: no text: ", String::fromUTF8(m_program.string(function.name)), " at ", String::fromUTF8(m_program.string(m_program.modules[function.module.index()].displayPath)), ":", where.line, ":", where.column);
            }
        }
        RELEASE_ASSERT(text.isEmpty() || paths.isEmpty());
        FunctionTexts texts { safeCast<uint32_t>(entries.size()), append(entries), text.isEmpty() ? 0 : append(text), append(paths) };
        return append(asBytes(singleElementSpan(texts)));
    }

    uint32_t allocate(size_t size, size_t alignment = 8)
    {
        uint32_t offset = WTF::roundUpToMultipleOf(alignment, m_result.size());
        m_result.insertFill(m_result.size(), 0, offset + size - m_result.size());
        return offset;
    }

    uint32_t allocateWords(size_t size)
    {
        uint32_t offset = WTF::roundUpToMultipleOf<4>(m_result.size());
        m_result.insertFill(m_result.size(), 0, offset + size - m_result.size());
        return offset;
    }

    template<typename T>
    T& at(uint32_t offset) { return *reinterpret_cast<T*>(m_result.mutableSpan().subspan(offset, sizeof(T)).data()); }

    static uint64_t hashOf(std::span<const uint8_t> bytes)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : bytes)
            hash = (hash ^ byte) * 1099511628211ull;
        return hash | 1;
    }

    uint32_t append(std::span<const uint8_t> bytes, size_t alignment = 8)
    {
        auto& withHash = m_inlinedCopies.add(hashOf(bytes), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
        for (auto [offset, size] : withHash) {
            if (size == bytes.size() && !(offset % alignment) && equalSpans(m_result.span().subspan(offset, size), bytes))
                return offset;
        }
        uint32_t offset = allocate(bytes.size(), alignment);
        memcpySpan(m_result.mutableSpan().subspan(offset), bytes);
        withHash.append({ offset, safeCast<uint32_t>(bytes.size()) });
        return offset;
    }

    template<typename T>
    uint32_t append(const Vector<T>& vector) { return append(asBytes(vector.span()), sizeof(T) == 1 ? 8 : alignof(T)); }

    uint32_t entrypointOf(FunctionID function) const
    {
        if (!function)
            return LayoutData::noBase;
        auto entrypoint = m_program.genericEntrypoints.find(function.index());
        RELEASE_ASSERT(entrypoint != m_program.genericEntrypoints.end());
        return entrypoint->value.index();
    }

    void appendTypes()
    {
        auto classes = Builtins::singleton().classes();
        Vector<std::pair<uint32_t, LayoutID>> supertypesWithoutLayoutData;
        Vector<uint32_t> supertypeOffset(FillWith { }, m_program.layouts.size(), 0);
        UncheckedKeyHashMap<uint64_t, Vector<std::pair<Vector<uint64_t, 16>, uint32_t>, 1>> matchingTypes;
        for (unsigned i = 0; i < m_program.types.size(); ++i) {
            auto& type = m_program.at(TypeID(i));
            if (!m_program.isNeededAtRuntime(TypeID(i))) {
                m_typeOffsets.append(0);
                continue;
            }
            RELEASE_ASSERT(!type.elements || type.elements.index() < i);
            for (LayoutID id : type.objectLayouts) {
                if (supertypeOffset[id.index()])
                    continue;
                auto& supertype = m_program.at(id);
                uint32_t test = supertypeOffset[id.index()] = allocate(sizeof(RuntimeShapeTest), alignof(RuntimeShapeTest));
                at<RuntimeShapeTest>(test) = RuntimeShapeTest { supertype.test.first, supertype.test.count, supertype.test.set, isObjectLayout(supertype.kind) ? supertype.shape : 0u, (supertype.name ? supertype.name : m_program.emptyString).index(), keepsDeclarationLocation(supertype), 0, supertype.declaredPath ? supertype.declaredPath.index() : UINT32_MAX, supertype.declaredLine };
                if (at<RuntimeShapeTest>(test).supertypeShape)
                    m_result.shrink(test + RuntimeShapeTest::sizeWithSupertypeShape);
                else if (!keepsDeclarationLocation(supertype))
                    m_result.shrink(test + RuntimeShapeTest::sizeWithoutDeclaredLocation);
                if (isObjectLayout(supertype.kind) && !supertype.shape)
                    supertypesWithoutLayoutData.append({ test, id });
            }
            Vector<uint64_t, 16> said { type.primitives, type.anyObject, type.anyFunction, type.acceptsUntypedObjects(m_program), static_cast<uint64_t>(type.integers.minimum), static_cast<uint64_t>(type.integers.maximum), type.objectLayouts.size(), type.elements ? m_typeOffsets[type.elements.index()] : 0 };
            for (LayoutID id : type.objectLayouts)
                said.append(supertypeOffset[id.index()]);
            for (uint16_t index : type.engineClasses)
                said.append(classes[index].type);
            auto& typesWithHash = matchingTypes.add(hashOf(asBytes(said.span())), Vector<std::pair<Vector<uint64_t, 16>, uint32_t>, 1> { }).iterator->value;
            if (size_t same = typesWithHash.findIf([&](auto& entry) { return entry.first == said; }); same != notFound) {
                m_typeOffsets.append(typesWithHash[same].second);
                continue;
            }
            int64_t minimum = type.integers.minimum;
            int64_t maximum = type.integers.maximum;
            bool hasNoInteger = minimum == 1 && !maximum;
            bool hasOneInteger = minimum == maximum && minimum == static_cast<int32_t>(minimum);
            Vector<uint32_t, 5> rest;
            if (type.elements)
                rest.append(m_typeOffsets[type.elements.index()]);
            if (hasOneInteger)
                rest.append(static_cast<uint32_t>(minimum));
            else if (!hasNoInteger)
                rest.appendList({ static_cast<uint32_t>(minimum), static_cast<uint32_t>(static_cast<uint64_t>(minimum) >> 32), static_cast<uint32_t>(maximum), static_cast<uint32_t>(static_cast<uint64_t>(maximum) >> 32) });
            RELEASE_ASSERT(type.objectLayouts.size() <= UINT16_MAX && type.engineClasses.size() <= UINT8_MAX);
            uint32_t offset = allocateWords(sizeof(RuntimeType) + type.objectLayouts.size() * sizeof(int32_t) + type.engineClasses.size() * sizeof(uint32_t) + rest.sizeInBytes());
            typesWithHash.append({ said, offset });
            auto& runtimeType = at<RuntimeType>(offset);
            runtimeType.primitives = type.withLiteralStringWidened().primitives;
            runtimeType.bits = (type.anyObject ? RuntimeType::allowsAnyObjectBit : 0) | (type.anyFunction ? RuntimeType::allowsAnyFunctionBit : 0) | (!type.acceptsUntypedObjects(m_program) ? 0 : m_program.isRelaxedForLogging(type) ? RuntimeType::isRelaxedForLoggingBit : type.acceptsUncheckedObjects(m_program) ? RuntimeType::acceptsUntypedObjectsBit : 0)
                | (type.acceptsOnlyOtherLayouts(m_program) ? RuntimeType::acceptsOnlyOtherLayoutsBit : 0)
                | (type.elements ? RuntimeType::hasElementsBit : 0) | (hasOneInteger ? RuntimeType::hasOneIntegerBit : hasNoInteger ? 0 : RuntimeType::hasIntegersBit);
            runtimeType.supertypesCount = type.objectLayouts.size();
            runtimeType.jsTypesCount = type.engineClasses.size();
            uint32_t next = offset + sizeof(RuntimeType);
            for (LayoutID id : type.objectLayouts) {
                at<int32_t>(next) = static_cast<int32_t>(supertypeOffset[id.index()]) - static_cast<int32_t>(offset);
                next += sizeof(int32_t);
            }
            for (uint16_t index : type.engineClasses) {
                at<uint32_t>(next) = classes[index].type;
                next += sizeof(uint32_t);
            }
            for (uint32_t word : rest) {
                at<uint32_t>(next) = word;
                next += sizeof(uint32_t);
            }
            m_typeOffsets.append(offset);
        }
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> fieldsOffset;
        for (auto [test, id] : supertypesWithoutLayoutData) {
            uint32_t offset = fieldsOffset.ensure(id.index(), [&] {
                Vector<uint8_t> bytes;
                auto isListed = [&](auto& field) {
                    return !!field.name;
                };
                SupertypeFields::append(bytes, std::ranges::count_if(m_program.at(id).fields, isListed));
                for (auto& field : m_program.at(id).fields) {
                    if (!isListed(field))
                        continue;
                    RELEASE_ASSERT(!(field.name.index() >> 31));
                    SupertypeFields::append(bytes, field.name.index() << 1 | (field.flags & fieldIsOptional ? 1 : 0));
                    SupertypeFields::append(bytes, (field.type ? field.type : m_program.types.anything).index());
                    SupertypeFields::append(bytes, keepsTypeSource(field) ? field.typeSource.index() + 1 : 0);
                }
                return append(bytes);
            }).iterator->value;
            at<RuntimeShapeTest>(test).fields = offset;
        }
    }

    String printedNameOf(const Layout& layout) const
    {
        if (layout.name && !m_program.string(layout.name).empty()) {
            String name = String::fromUTF8(m_program.string(layout.name));
            return name.isNull() ? "?"_str : name;
        }
        static constexpr size_t maxNamesShown = 4;
        StringBuilder result;
        size_t numberOfProperties = 0;
        for (auto& field : layout.fields) {
            if (layout.shape ? !exists(field) : !field.name)
                continue;
            if (numberOfProperties++ < maxNamesShown)
                result.append(',', String::fromUTF8(m_program.string(field.name ? field.name : m_program.emptyString)));
        }
        result.append(numberOfProperties > maxNamesShown ? '+' : '.');
        return result.toString();
    }

    bool keepsDeclarationLocation(const Layout& layout)
    {
        if (!layout.declaredPath)
            return false;
        if (m_program.logsTypeErrors || Options::auditAOTTypedAssertions())
            return true;
        if (m_namePrintCounts.isEmpty()) {
            for (auto& other : m_program.layouts) {
                if (isObjectLayout(other.kind) && !other.isTypedOnlySupertype)
                    ++m_namePrintCounts.add(printedNameOf(other), 0).iterator->value;
            }
        }
        return m_namePrintCounts.get(printedNameOf(layout)) != 1;
    }

    String runtimeTypeDescription(TypeID id) const
    {
        auto& type = m_program.at(id);
        DescribedType described { type.primitives, type.anyObject, type.anyFunction, type.integers.minimum, type.integers.maximum, { }, { }, { } };
        for (LayoutID supertype : type.objectLayouts) {
            StringID name = m_program.at(supertype).name;
            if (!name || m_program.string(name).empty())
                return { };
            described.views.append(String::fromUTF8(m_program.string(name)));
            if (described.views.last().isNull())
                return { };
        }
        auto classes = Builtins::singleton().classes();
        for (uint16_t index : type.engineClasses)
            described.jsTypes.append(static_cast<uint32_t>(classes[index].type));
        if (type.elements) {
            described.elements = runtimeTypeDescription(type.elements);
            if (described.elements.isNull())
                return { };
        }
        return describe(described, false);
    }

    bool keepsTypeSource(const Field& field)
    {
        if (!field.typeSource)
            return false;
        if (m_program.logsTypeErrors || Options::auditAOTTypedAssertions())
            return true;
        TypeID type = field.type ? field.type : m_program.types.anything;
        return m_keepsTypeSource.ensure(static_cast<uint64_t>(type.index()) << 32 | field.typeSource.index(), [&] {
            String described = runtimeTypeDescription(type);
            return described.isNull() || described != String::fromUTF8(m_program.string(field.typeSource));
        }).iterator->value;
    }

    uint32_t appendLayout(LayoutID id)
    {
        auto& layout = m_program.at(id);
        Vector<uint32_t> fieldNumber;
        uint32_t numberOfFields = 0;
        bool isObject = isObjectLayout(layout.kind);
        auto isDescribed = [&](unsigned i) {
            auto& field = layout.fields[i];
            if (!exists(field))
                return false;
            if (layout.isCoroutineFrame)
                return i < CoroutineFrame::firstArgumentField;
            return isObject || holdsCells(field.store) || !representsAbsentAsZero(field.store);
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i)
            fieldNumber.append(isDescribed(i) ? numberOfFields++ : UINT32_MAX);
        Vector<uint32_t> orders;
        for (auto& order : layout.orders) {
            Vector<uint16_t> list;
            list.append(static_cast<uint16_t>(order.size()));
            for (uint16_t field : order)
                list.append(static_cast<uint16_t>(fieldNumber[field]));
            orders.append(append(list));
        }
        uint32_t ordersOffset = orders.isEmpty() ? 0 : append(orders);
        Vector<uint32_t> specializations;
        for (LayoutID specialization : layout.specializations)
            specializations.append(m_program.at(specialization).shape);
        std::ranges::sort(specializations);
        uint32_t specializationsOffset = specializations.isEmpty() ? 0 : append(specializations);
        Vector<uint32_t> literalsOffsets;
        for (auto& field : layout.fields) {
            Vector<LiteralData> literals;
            for (auto& literal : field.literals)
                literals.append(literal.string ? LiteralData::ofString(literal.string.index()) : LiteralData::ofNumber(literal.number));
            literalsOffsets.append(literals.isEmpty() || !isObjectLayout(layout.kind) ? 0 : append(literals));
        }
        Vector<uint32_t> typeTexts;
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            if (isObject && isDescribed(i))
                typeTexts.append(keepsTypeSource(layout.fields[i]) ? layout.fields[i].typeSource.index() : UINT32_MAX);
        }
        uint32_t typeStringsOffset = std::ranges::all_of(typeTexts, [](uint32_t text) { return text == UINT32_MAX; }) ? 0 : append(typeTexts);
        Vector<uint32_t> literalsWithoutField { 0u };
        for (unsigned i = 0; i < layout.fields.size() && layout.kind == LayoutKind::Record; ++i) {
            auto& field = layout.fields[i];
            if (exists(field) || !field.name || field.literals.isEmpty())
                continue;
            ++literalsWithoutField[0];
            literalsWithoutField.appendList({ field.name.index(), static_cast<uint32_t>(field.literals.size()), literalsOffsets[i] });
        }
        uint32_t literalsWithoutFieldOffset = literalsWithoutField[0] ? append(literalsWithoutField) : 0;
        static constexpr unsigned fewFields = 8;
        Vector<std::pair<uint32_t, uint16_t>> fieldsByName;
        for (unsigned i = 0; i < layout.fields.size() && isObject && numberOfFields > fewFields; ++i) {
            if (isDescribed(i))
                fieldsByName.append({ (layout.fields[i].name ? layout.fields[i].name : m_program.emptyString).index(), static_cast<uint16_t>(fieldNumber[i]) });
        }
        std::ranges::sort(fieldsByName);
        uint32_t fieldsByNameOffset = fieldsByName.isEmpty() ? 0 : append(fieldsByName.map([](auto& entry) {
            return entry.second;
        }));
        struct Variable {
            StringID name;
            uint32_t number;
            TypeID staticFieldType;
        };
        Vector<Variable> variables;
        for (auto& field : layout.staticFields) {
            if (field.type)
                variables.append({ field.name, 0, field.type });
        }
        unsigned numberOfStaticFields = variables.size();
        Vector<MethodData> methods;
        bool isClass = layout.kind == LayoutKind::Instance;
        for (auto* list : { &layout.methods, &layout.staticMethods }) {
            if (!isClass)
                break;
            for (auto& method : *list) {
                uint32_t flags = m_program.at(method.function).captureTypes.isEmpty() ? 0 : methodTakesEnvironment;
                if (method.mayBeReplaced)
                    flags |= methodMayBeReplaced;
                uint32_t name = method.name.index();
                if (!method.name) {
                    flags |= methodHasComputedName;
                    name = variables.size();
                    variables.append({ m_program.emptyString, method.key, { } });
                } else if (auto text = m_program.string(method.name); m_namesAssumedAbsent.get(name) || isOnObjectPrototype(m_program, method.name) || equalSpans(text, asBytes("then"_s.span8())) || equalSpans(text, asBytes("toJSON"_s.span8())) || parseIndex(text))
                    flags |= methodNameIsAssumedAbsent;
                RELEASE_ASSERT(name < 1u << 24 && static_cast<uint32_t>(method.kind) < 16 && flags < 16);
                methods.append(MethodData { name, static_cast<uint32_t>(method.kind), flags, entrypointOf(method.function) });
            }
        }

        Vector<uint32_t> boxedSlots;
        Vector<uint32_t> cellSlots;
        Vector<uint32_t> narrowSlots;
        for (auto& field : layout.fields) {
            if (exists(field) && holdsCells(field.store) && !field.isOutOfLine)
                (sizeOf(field.store) == 4 ? narrowSlots : field.store == Store::JSValue || field.store == Store::LiteralString ? boxedSlots : cellSlots).append(field.offset);
        }
        std::ranges::sort(boxedSlots);
        std::ranges::sort(cellSlots);
        std::ranges::sort(narrowSlots);
        Vector<uint32_t> references { static_cast<uint32_t>(boxedSlots.size()), static_cast<uint32_t>(cellSlots.size()), static_cast<uint32_t>(narrowSlots.size()) };
        references.appendVector(boxedSlots);
        references.appendVector(cellSlots);
        references.appendVector(narrowSlots);
        if (layout.hasExtension) {
            Vector<uint32_t> boxedOutOfLine;
            Vector<uint32_t> narrowOutOfLine;
            for (auto& field : layout.fields) {
                if (exists(field) && holdsCells(field.store) && field.isOutOfLine)
                    (sizeOf(field.store) == 4 ? narrowOutOfLine : boxedOutOfLine).append(field.offset);
            }
            std::ranges::sort(boxedOutOfLine);
            std::ranges::sort(narrowOutOfLine);
            references.appendList({ static_cast<uint32_t>(boxedOutOfLine.size()), static_cast<uint32_t>(narrowOutOfLine.size()) });
            references.appendVector(boxedOutOfLine);
            references.appendVector(narrowOutOfLine);
        }
        uint32_t referencesOffset = references.size() > 3 ? append(references) : 0;

        if (!isObject) {
            uint32_t offset = allocate(sizeof(LayoutData), alignof(LayoutData));
            auto& data = at<LayoutData>(offset);
            data.kind = layout.kind;
            data.index = id.index();
            data.shape = layout.shape;
            data.name = (layout.name ? layout.name : m_program.emptyString).index();
            data.size = layout.size;
            data.numberOfDescribedVariables = numberOfFields;
            data.referencesOffset = referencesOffset ? static_cast<int32_t>(referencesOffset) - static_cast<int32_t>(offset) : 0;
            RELEASE_ASSERT(!layout.hasExtension && layout.sizeWithSpecializationCapacity == layout.size && orders.isEmpty() && variables.isEmpty() && methods.isEmpty());
            m_result.shrink(offset + LayoutData::sizeOfEnvironment);
            for (unsigned i = 0; i < layout.fields.size(); ++i) {
                if (!isDescribed(i))
                    continue;
                auto& field = layout.fields[i];
                RELEASE_ASSERT(!field.isOutOfLine && field.offset < 1u << 24);
                uint32_t next = m_result.size();
                m_result.insertFill(next, 0, sizeof(VariableData));
                auto& variable = at<VariableData>(next);
                variable.name = (field.name ? field.name : m_program.emptyString).index();
                variable.offset = field.offset;
                variable.storeBits = static_cast<uint8_t>(field.store);
            }
            return offset;
        }
        Vector<uint32_t> fieldTypes;
        auto appendFieldTypes = [&](TypeID stored, TypeID own, uint32_t numberOfLiterals, uint32_t literalsOffset) {
            RELEASE_ASSERT(m_typeOffsets[stored.index()] && m_typeOffsets[(own ? own : m_program.types.anything).index()]);
            fieldTypes.append(append(Vector<uint32_t> { m_typeOffsets[stored.index()], m_typeOffsets[(own ? own : m_program.types.anything).index()], numberOfLiterals, literalsOffset }));
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            if (isDescribed(i))
                appendFieldTypes(layout.fields[i].storedInSlot, layout.fields[i].type, layout.fields[i].literals.size(), layout.fields[i].lazyClosure ? entrypointOf(layout.fields[i].lazyClosure) | (m_program.at(layout.fields[i].lazyClosure).captureTypes.isEmpty() ? 0 : lazyClosureCapturesObject) : literalsOffsets[i]);
        }
        for (auto [name, number, staticFieldType] : variables) {
            if (staticFieldType)
                appendFieldTypes(staticFieldType, staticFieldType, 0, 0);
            else if (layout.environment)
                appendFieldTypes(m_program.at(layout.environment).fields[number].storedInSlot, m_program.at(layout.environment).fields[number].type, 0, 0);
            else
                appendFieldTypes(m_program.variables[number].type ? m_program.variables[number].type : m_program.types.anything, m_program.variables[number].type, 0, 0);
        }
        uint32_t discriminant = layout.discriminant == UINT16_MAX ? LayoutData::noBase : fieldNumber[layout.discriminant];
        bool hasFieldsWithSymbolKeys = false;
        for (unsigned i = 0; i < layout.fields.size() && isObject; ++i)
            hasFieldsWithSymbolKeys |= fieldNumber[i] != UINT32_MAX && layout.fields[i].name && isConstantSymbol(m_program.string(layout.fields[i].name));
        bool hasRareData = hasFieldsWithSymbolKeys || layout.sizeWithSpecializationCapacity != layout.size || !specializations.isEmpty() || layout.extensionSize || discriminant != LayoutData::noBase || fieldsByNameOffset || literalsWithoutFieldOffset;
        bool hasMessageData = typeStringsOffset || keepsDeclarationLocation(layout);
        uint32_t sizeBeforeLayoutData = (isClass ? sizeof(ClassData) : 0) + (hasMessageData ? sizeof(MessageLayoutData) : 0) + (hasRareData ? sizeof(RareLayoutData) : 0);
        uint32_t offset = allocate(sizeBeforeLayoutData + sizeof(LayoutData) + (numberOfFields + variables.size()) * sizeof(FieldData) + methods.sizeInBytes(), alignof(LayoutData)) + sizeBeforeLayoutData;
        auto& data = at<LayoutData>(offset);
        data.kind = layout.kind;
        data.index = id.index();
        data.shape = layout.shape;
        data.name = (layout.name ? layout.name : m_program.emptyString).index();
        data.size = layout.size;
        data.hasRareData = hasRareData;
        data.hasMessageData = hasMessageData;
        if (hasMessageData)
            data.messageData() = { typeStringsOffset, keepsDeclarationLocation(layout) ? layout.declaredPath.index() : UINT32_MAX, keepsDeclarationLocation(layout) ? layout.declaredLine : 0 };
        if (hasRareData)
            data.rareData() = { layout.sizeWithSpecializationCapacity, static_cast<uint32_t>(specializations.size()), hasFieldsWithSymbolKeys, specializationsOffset, layout.extensionSize, discriminant, fieldsByNameOffset, literalsWithoutFieldOffset };
        data.mayBeFrozen = layout.mayBeFrozen || layout.isImmutableAfterAllocation;
        data.mayDespecialize = layout.mayDespecialize || (layout.kind == LayoutKind::Instance && layout.hasFixedLayout && !layout.isAllocated && layout.acceptsUntypedObjects && !layout.checksUntypedObjects());
        data.hasFixedLayout = layout.hasFixedLayout;
        data.isEvaluatedOnce = layout.isEvaluatedOnce;
        data.hasUntypedBase = layout.hasUntypedBase;
        data.hasImmutableMembers = layout.hasImmutableMembers;
        data.mayHaveOtherPrototype = layout.mayHaveOtherPrototype;
        data.mayBeConstructed = layout.mayBeConstructed;
        data.constructorMayBeReplaced = layout.constructorMayBeReplaced;
        data.constructorTakesNewTarget = layout.constructor && m_program.at(layout.constructor).has(Format::FunctionHasNewTarget);
        data.numberOfOrders = orders.size();
        data.ordersOffset = ordersOffset;
        data.numberOfFields = numberOfFields;
        data.isAbstract = layout.isAbstract;
        data.referencesOffset = referencesOffset ? static_cast<int32_t>(referencesOffset) - static_cast<int32_t>(offset) : 0;
        data.hasExtension = layout.hasExtension;
        data.extraPropertiesSlotIndex = layout.offsetOfExtraProperties / sizeOfExtraPropertiesWord;
        if (isClass) {
            auto& ofClass = data.classData();
            ofClass.base = layout.base ? layout.base.index() : LayoutData::noBase;
            ofClass.classConstructor = entrypointOf(layout.constructor);
            ofClass.environment = layout.environment ? layout.environment.index() : LayoutData::noBase;
            ofClass.numberOfMethods = layout.methods.size();
            ofClass.numberOfStaticMethods = layout.staticMethods.size();
            ofClass.numberOfStaticFields = numberOfStaticFields;
            ofClass.prototypeMayGainMembers = layout.prototypeMayGainMembers;
            ofClass.numberOfVariables = variables.size();
            static_assert(Format::none == LayoutData::noBase);
            ofClass.firstStaticFieldLocation = layout.classObjectSlot;
        } else
            RELEASE_ASSERT(!layout.base && variables.isEmpty() && methods.isEmpty());
        uint32_t next = offset + sizeof(LayoutData);
        unsigned numberOfFieldsAppended = 0;
        auto appendField = [&](StringID name, uint32_t fieldOffset, Store store, uint32_t flags, TypeID stored) {
            uint16_t primitives = m_program.at(stored).primitives;
            auto& fieldData = at<FieldData>(next);
            RELEASE_ASSERT(fieldOffset < 1u << 24);
            fieldData.name = (name ? name : m_program.emptyString).index();
            fieldData.offset = fieldOffset;
            fieldData.storeBits = static_cast<uint8_t>(store);
            fieldData.flags = flags;
            if (primitives & Primitive::Undefined)
                fieldData.flags |= fieldAllowsUndefined;
            if (primitives & Primitive::Null)
                fieldData.flags |= fieldAllowsNull;
            fieldData.typesDistance = static_cast<int32_t>(fieldTypes[numberOfFieldsAppended++]) - static_cast<int32_t>(next);
            next += sizeof(FieldData);
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            auto& field = layout.fields[i];
            if (!isDescribed(i))
                continue;
            uint32_t pinned = field.isPinned || (field.isPinnedToFunction && field.slot) ? fieldIsPinned | field.slot.index() % numberOfPinnedFieldGuards << pinnedFieldGuardShift : 0;
            RELEASE_ASSERT(!pinned || !(field.isUndeclared || field.isCreatedWithLazyClosure));
            appendField(field.name, field.offset, field.store, field.flags | (field.isOutOfLine ? fieldIsOutOfLine : 0) | pinned | (field.isUndeclared ? unpinnedFieldIsUndeclared : 0) | (field.isCreatedWithLazyClosure ? unpinnedFieldIsCreatedWithLazyClosure : 0), field.storedInSlot);
        }
        memcpySpan(m_result.mutableSpan().subspan(next), asBytes(methods.span()));
        next += methods.sizeInBytes();
        for (unsigned i = 0; i < variables.size(); ++i) {
            auto [name, number, staticFieldType] = variables[i];
            if (staticFieldType)
                appendField(name, 0, Store::JSValue, fieldIsAnnotated, staticFieldType);
            else if (layout.environment) {
                auto& field = m_program.at(layout.environment).fields[number];
                appendField(name, field.offset, field.store, field.flags, field.storedInSlot);
            } else {
                auto& variable = m_program.variables[number];
                appendField(name, variable.offset, variable.store, 0, variable.type ? variable.type : m_program.types.anything);
            }
        }
        return offset;
    }

    const Program& m_program;
    Vector<uint8_t> m_result;
    BitVector m_namesAssumedAbsent;
    Vector<uint32_t> m_typeOffsets;
    UncheckedKeyHashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keepsTypeSource;
    UncheckedKeyHashMap<String, unsigned> m_namePrintCounts;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_inlinedCopies;
};

} // anonymous namespace

static void findRecordsImmutableAfterAllocation(Program& program)
{
    Vector<uint8_t> isStoredTo(FillWith { }, program.layouts.size(), 0);
    forEachIndexInParallel(program.functions.size(), [&](unsigned number) {
        static constexpr OpcodeSet stores { Opcode::PutById, Opcode::PutField, Opcode::PutByVal };
        if (!program.functions.mayContain(number, stores))
            return;
        ImmutableFunctionScope immutableFunctionScope;
        auto& function = std::as_const(program).functions[number];
        std::as_const(program).functions.forEachIndexedValueWithBlock(number, stores, [&](unsigned block, unsigned index, ValueID id) {
            if (isInitializingStore(program, function, function.blocks[block], index))
                return;
            for (LayoutID supertype : program.at(function.at(function.at(id).child(0)).type).objectLayouts) {
                for (LayoutID subtype : program.at(supertype).subtypes) {
                    if (!WTF::atomicLoad(&isStoredTo[subtype.index()], std::memory_order_relaxed))
                        WTF::atomicStore(&isStoredTo[subtype.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
                }
            }
        });
    });
    unsigned count = 0;
    unsigned records = 0;
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind != LayoutKind::Record)
            continue;
        ++records;
        layout.isImmutableAfterAllocation = !isStoredTo[i];
        count += layout.isImmutableAfterAllocation;
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT placement: records immutable after allocation: ", count, " of ", records);
}

static void findLayoutsFrozenByUntypedCode(Program& program)
{
    auto& builtins = Builtins::singleton();
    auto changesIntegrityLevel = [&](BuiltinID id) {
        auto& builtin = builtins.at(id);
        if (builtin.kind != BuiltinKind::StaticFunction)
            return false;
        if (builtin.holder == "Object"_s)
            return builtin.name == "freeze"_s || builtin.name == "seal"_s || builtin.name == "preventExtensions"_s;
        return builtin.holder == "Reflect"_s && builtin.name == "preventExtensions"_s;
    };
    uint8_t takesAnyObject = 0;
    forEachIndexInParallel(program.functions.size(), [&](unsigned number) {
        static constexpr OpcodeSet uses { Opcode::CallBuiltin, Opcode::BuiltinFunction };
        if (WTF::atomicLoad(&takesAnyObject, std::memory_order_relaxed) || !program.functions.mayContain(number, uses))
            return;
        ImmutableFunctionScope immutableFunctionScope;
        auto& function = std::as_const(program).functions[number];
        if (function.has(Format::FunctionIsBuiltin | Format::FunctionIsInBuiltinModule) || (function.module && program.modules[function.module.index()].isBuiltin))
            return;
        std::as_const(program).functions.forEachIndexedValue(number, uses, [&](ValueID id) {
            auto& value = function.at(id);
            if (!changesIntegrityLevel(value.builtin()))
                return;
            if (value.opcode == Opcode::CallBuiltin && (value.arguments.isEmpty() || !program.typeOf(function, value.arguments[0]).anyObject))
                return;
            WTF::atomicStore(&takesAnyObject, static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    if (!takesAnyObject)
        return;
    for (auto& layout : program.layouts) {
        if (layout.mayDespecialize)
            layout.mayBeFrozen = true;
    }
}

static void findTypesNeededAtRuntime(Program& program)
{
    BitVector& named = program.typesNeededAtRuntime;
    named.ensureSize(program.types.size());
    BitVector isNamedTypeLayout(program.layouts.size());
    Vector<TypeID> worklist;
    auto note = [&](TypeID type) {
        if (type && !named.set(type.index()))
            worklist.append(type);
    };
    auto recordFieldsOf = [&](const Layout& layout) {
        for (auto& field : layout.fields) {
            note(field.storedInSlot);
            note(field.type);
        }
        for (auto& field : layout.staticFields)
            note(field.type);
    };
    note(program.types.anything);
    if (Options::auditAOTTypedAssertions()) [[unlikely]] {
        for (uint32_t type = 0; type < program.types.size(); ++type)
            note(TypeID(type));
    }
    Vector<uint8_t> isReferencedByValue(FillWith { }, program.types.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> isDecodedRowType(FillWith { }, program.types.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto mark = [&](TypeID type) {
            if (type && !WTF::atomicLoad(&isReferencedByValue[type.index()], std::memory_order_relaxed))
                WTF::atomicStore(&isReferencedByValue[type.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        for (auto& value : std::as_const(program).functions[i].values) {
            switch (value.opcode) {
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::Boolean:
            case Opcode::String:
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::NewObjectWithShape:
                break;
            case Opcode::Cast:
                if (value.castReason() == CastReason::Member)
                    break;
                if (value.castReason() == CastReason::Coerce)
                    mark(value.coercedType());
                [[fallthrough]];
            default:
                mark(value.type);
                if (value.opcode == Opcode::CallBuiltin && Builtins::singleton().at(value.builtin()).implementation.kind == BuiltinImplementation::Inline && Builtins::singleton().at(value.builtin()).implementation.inlined == InlineBuiltin::SetExpectedRowType && value.prediction()) {
                    mark(value.prediction());
                    WTF::atomicStore(&isDecodedRowType[value.prediction().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
                }
                break;
            }
        }
    });
    for (uint32_t type = 0; type < isReferencedByValue.size(); ++type) {
        if (isReferencedByValue[type])
            note(TypeID(type));
        if (isDecodedRowType[type]) {
            for (LayoutID layout : program.at(TypeID(type)).objectLayouts)
                program.at(layout).isDecodedRow = true;
        }
    }
    for (auto& layout : program.layouts) {
        if (layout.shape || layout.kind == LayoutKind::Instance || layout.kind == LayoutKind::Environment)
            recordFieldsOf(layout);
    }
    for (auto& variable : program.variables)
        note(variable.type);
    while (!worklist.isEmpty()) {
        auto& type = program.at(worklist.takeLast());
        note(type.elements);
        for (LayoutID supertype : type.objectLayouts) {
            if (!isNamedTypeLayout.set(supertype.index()))
                recordFieldsOf(program.at(supertype));
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT placement: types with a runtime description: ", named.bitCount(), " of ", program.types.size());
}

void place(Program& program)
{
    Placement(program).run();
    PhaseTimes phaseTimes("place"_s);
    findRecordsImmutableAfterAllocation(program);
    phaseTimes.didFinish("findRecordsImmutableAfterAllocation"_s);
    findLayoutsFrozenByUntypedCode(program);
    phaseTimes.didFinish("findLayoutsFrozenByUntypedCode"_s);
    assignStructureDescriptors(program);
    phaseTimes.didFinish("assignStructureDescriptors"_s);
    findTypesNeededAtRuntime(program);
    phaseTimes.didFinish("findTypesNeededAtRuntime"_s);
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        program.functions.verifyValueIndex();
}

uint8_t kindByteOf(const Program& program, const Function& function)
{
    return (function.isSharedOutlinedBody || function.isGuardedPreludeEntry ? sharedBodyFunctionKind : function.has(Format::FunctionIsModuleBody) ? static_cast<uint8_t>(FunctionKind::Module) : function.flags & Format::FunctionHasExplicitName && !function.suspensionPositions.isEmpty() ? resumeFunctionNamedByOwnerKind : static_cast<uint8_t>(function.kind)) | function.declaredAs | (program.isHiddenInStackTraces(function) ? functionIsHiddenInStackTraces : 0) | (function.kind == FunctionKind::Module && function.has(Format::FunctionHasExplicitName) ? moduleIsCommonJS : 0);
}

Vector<uint8_t> encodeData(const Program& program)
{
    return DataEncoder(program).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
