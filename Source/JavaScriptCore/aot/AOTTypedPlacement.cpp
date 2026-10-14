/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedObject.h"
#include "AOTTypedThreads.h"
#include "MarkedSpace.h"
#include "Options.h"
#include <wtf/DataLog.h>
#include <numeric>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT { namespace Typed {

Rep resultRepOf(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    // Whoever calls a general entry knows nothing about it.
    if (function.isGenericEntrypoint() || !function.resultType || function.returnsBoxedValue)
        return Rep::JSValue;
    auto& type = program.at(function.resultType);
    if (type.isNever() || type.hasOnlyPrimitives(Primitive::Undefined))
        return Rep::Void;
    return repOf(type);
}

namespace {

// Booth and Lueker's PQ tree: the orders of the leaves in which every set reduced so far is consecutive. A reduction is linear in the set.
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

    // False: no order is left in which these are consecutive too, and the tree stays as it is. No leaf is listed twice.
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

    // One of the orders: where the tree leaves a choice, the subtree with the least key among its leaves comes first.
    Vector<uint32_t> frontier(std::span<const uint64_t> keyOfLeaf)
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
                keys[node] = keyOfLeaf[node];
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
        // Its children may be in any order.
        P,
        // Its children are in this order or the reverse.
        Q,
        // What was a Q node. Its children are now among those of the Q node that `parent` leads to.
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
        // Under a P node.
        uint32_t indexInParent { 0 };
        // Under a Q node, in either order, so that nothing has to be done to reverse a row of them.
        std::array<uint32_t, 2> siblings { none, none };
        // Of a P node.
        Vector<uint32_t> children;
        // Of a Q node.
        std::array<uint32_t, 2> endmostChildren { none, none };

        // The rest is about the set being reduced, and holds if `epoch` is that of the tree.
        uint32_t epoch { 0 };
        uint32_t pertinentChildCount { 0 };
        uint32_t pertinentLeafCount { 0 };
        uint32_t fullChildCount { 0 };
        uint32_t firstFullChild { none };
        uint32_t nextFullSibling { none };
        uint32_t partialChildCount { 0 };
        std::array<uint32_t, 2> partialChildren { none, none };
        // Of a Q node: the first and the last of the full children, and the sibling beyond each.
        std::array<uint32_t, 2> endmostFullChildren { none, none };
        std::array<uint32_t, 2> siblingsOfFullChildren { none, none };
    };

    // A row of nodes on their way under a Q node.
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

    void addToRow(uint32_t q, uint32_t node)
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

    // Labels every node with some of the leaves below it, up to the lowest that has them all, which it yields. Nothing: it cannot be reduced.
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
        // Upwards from all leaves at once, until the paths have met: it does not go on to the root of the tree.
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
        // Children before parents.
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

    // Below the root, the full leaves have to end up at one end of the node. In the root they may be in the middle.
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
            node.siblingsOfFullChildren[side] = next;
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

    // Takes the full children of a P node out of it. Yields one node that stands for them, if there are any.
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

    // Dissolves a partial node, which is in no list of children any more, into a row for the Q node: what is empty, then what is full.
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
                    addToRow(q, empty);
                    extend(&Row::emptyEnd, empty, empty);
                }
                if (full != none) {
                    addToRow(q, full);
                    extend(&Row::fullEnd, full, full);
                }
            } else {
                // From one end: the full children, the partial child, the empty children.
                uint32_t firstPertinentChild = partialChild;
                uint32_t lastFullChild = none;
                uint32_t firstEmptyChild = none;
                if (at(node).fullChildCount) {
                    unsigned end = at(node).siblingsOfFullChildren[0] != none;
                    firstPertinentChild = at(node).endmostFullChildren[end];
                    lastFullChild = at(node).endmostFullChildren[!end];
                    firstEmptyChild = at(node).siblingsOfFullChildren[!end];
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
            addToRow(q, full);
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
        // What a partial child has on the side where its full leaves have to go.
        uint32_t pertinentSibling = at(root).partialChildren[1];
        for (unsigned i = 0; i < at(root).partialChildCount; ++i) {
            uint32_t child = at(root).partialChildren[i];
            if (at(root).fullChildCount)
                pertinentSibling = at(root).endmostFullChildren[at(root).siblingsOfFullChildren[0] != child];
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

// A property that nothing stores does not exist: its field has no slot, and the run time is not told of it.
bool exists(const Field& field)
{
    return !(field.flags & fieldIsNeverStored);
}

// The order of an allocation, and how many of it are born, counting what exists.
std::pair<Vector<uint16_t>, uint16_t> orderOfWhatExists(const Layout& layout, const Allocation& allocation)
{
    std::pair<Vector<uint16_t>, uint16_t> result { { }, 0 };
    for (unsigned i = 0; i < allocation.order.size(); ++i) {
        if (!exists(layout.fields[allocation.order[i]]))
            continue;
        result.first.append(allocation.order[i]);
        result.second += i < allocation.born;
    }
    return result;
}

bool startsWith(const Vector<uint16_t>& order, const Vector<uint16_t>& prefix)
{
    return order.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), order.begin());
}

// Every thread makes a state of its own. What is done for one index writes nothing that is read or written for another, so the outcome does not depend on the threads.
template<typename State>
void forEachIndexOnAllThreads(unsigned count, uint64_t numberOfSteps, const Invocable<State()> auto& makeState, const Invocable<void(State&, unsigned)> auto& functor)
{
    std::atomic<unsigned> next { 0 };
    auto work = [&] {
        State state = makeState();
        for (unsigned i = next++; i < count; i = next++)
            functor(state, i);
    };
    // Below this, starting the threads takes longer than the work.
    static constexpr uint64_t fewestStepsForThreads = 1 << 16;
    Vector<Ref<Thread>> threads;
    if (numberOfSteps >= fewestStepsForThreads) {
        for (unsigned i = 1; i < std::min<unsigned>(count, WTF::numberOfProcessorCores()); ++i)
            threads.append(Thread::create("AOT placement"_s, [&work] { work(); }));
    }
    work();
    for (auto& thread : threads)
        thread->waitForCompletion();
}

// Three-way radix quicksort of lists, by their numbers. The key is 0 past the end of a list. What lists have in common is read once, not once per comparison.
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
            uint32_t numberLess = 0;
            uint32_t firstGreater = count;
            for (uint32_t i = 0; i < firstGreater;) {
                uint32_t key = keyAt(range[i], position);
                if (key < pivot)
                    std::swap(range[numberLess++], range[i++]);
                else if (key > pivot)
                    std::swap(range[i], range[--firstGreater]);
                else
                    ++i;
            }
            if (numberLess > 1)
                stack.append(Range { first, numberLess, position });
            if (count - firstGreater > 1)
                stack.append(Range { first + firstGreater, count - firstGreater, position });
            // These have all ended: they are the same list.
            if (!pivot)
                break;
            first += numberLess;
            count = firstGreater - numberLess;
            ++position;
        }
    }
}

// Compiled code accesses a field through a view without a test. So the field is where the view says, kept as the view says, in every member.
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
        // The run time makes a closure of a function whether or not the program does.
        auto& closure = m_program.at(m_program.closureLayout);
        closure.isAllocated = true;
        closure.size = WTF::roundUpToMultipleOf<16>(Closure::allocationSize(0));

        runStep("checkProgram"_s, &Placement::checkProgram);
        runStep("numberFields"_s, &Placement::numberFields);
        runStep("indexViews"_s, &Placement::indexViews);
        runStep("dropSpecializationsThatLackFields"_s, &Placement::dropSpecializationsThatLackFields);
        runStep("findFamilies"_s, &Placement::findFamilies);
        runStep("expandAllocations"_s, &Placement::expandAllocations);
        runStep("findInitialStores"_s, &Placement::findInitialStores);
        runStep("findAccessedFields"_s, &Placement::findAccessedFields);
        runStep("shareSlots"_s, &Placement::shareSlots);
        runStep("findLackedFields"_s, &Placement::findLackedFields);
        runStep("countInitialStores"_s, &Placement::countInitialStores);
        runStep("dropFlagsThatDoNotHold"_s, &Placement::dropFlagsThatDoNotHold);
        runStep("collectTypes"_s, &Placement::collectTypes);
        runStep("scanFunctions"_s, &Placement::scanFunctions);
        runStep("groupLayouts"_s, &Placement::groupLayouts);
        runStep("followSpecializations"_s, &Placement::followSpecializations);
        runStep("findSlotsThatMayBeAbsent"_s, &Placement::findSlotsThatMayBeAbsent);
        runStep("findLargeMembers"_s, &Placement::findLargeMembers);
        runStep("findTopLevelActivations"_s, &Placement::findTopLevelActivations);
        runStep("typeSlots"_s, &Placement::typeSlots);
        m_inlineCap.fill(noCap, m_program.layouts.size());
        runStep("placeObjects"_s, &Placement::placeObjects);
        runStep("placeEnvironments"_s, &Placement::placeEnvironments);
        runStep("makeRoomToSpecialize"_s, &Placement::makeRoomToSpecialize);
        runStep("capLayouts"_s, &Placement::capLayouts);
        runStep("widenNumbersWhereBytesAreFree"_s, &Placement::widenNumbersWhereBytesAreFree);
        runStep("pinLiterals"_s, &Placement::pinLiterals);
        runStep("sizeAllocations"_s, &Placement::sizeAllocations);
        runStep("collectOrders"_s, &Placement::collectOrders);
        runStep("numberShapes"_s, &Placement::numberShapes);
        runStep("placeVariables"_s, &Placement::placeVariables);
        runStep("placeArraySites"_s, &Placement::placeArraySites);
        runStep("placeRegExpLiterals"_s, &Placement::placeRegExpLiterals);
        runStep("checkPlacement"_s, &Placement::checkPlacement);
        m_program.sizeOfStaticsAsPlaced = m_program.sizeOfStatics;
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

    // What is found in one function asks nothing of another: it is found on all threads, and taken in the order of the functions.
    template<typename Found>
    void forEachFunctionInOrder(const Invocable<Found(Function&)> auto& find, const Invocable<void(Found&&)> auto& take, uint8_t held = 0)
    {
        Vector<Found> found(m_program.functions.size());
        forEachIndexInParallel(found.size(), [&](unsigned i) {
            if (held && !(m_heldByFunction[i] & held))
                return;
            found[i] = find(m_program.functions[i]);
        });
        for (auto& ofFunction : found)
            take(WTF::move(ofFunction));
    }

    static constexpr uint8_t holdsNewObjectWithShape = 1;
    static constexpr uint8_t holdsNewArray = 2;
    static constexpr uint8_t holdsStoreToField = 4;
    static constexpr uint8_t holdsCallBuiltin = 8;
    static uint8_t heldWith(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::NewObjectWithShape:
            return holdsNewObjectWithShape;
        case Opcode::NewArray:
            return holdsNewArray;
        case Opcode::PutField:
        case Opcode::PutById:
            return holdsStoreToField;
        case Opcode::CallBuiltin:
            return holdsCallBuiltin;
        default:
            return 0;
        }
    }

    void runStep(ASCIILiteral name, void (Placement::*step)())
    {
        MonotonicTime start = MonotonicTime::now();
        (this->*step)();
        dataLogLnIf(saysClocks(), "AOT placement: ", name, " took ", (MonotonicTime::now() - start).milliseconds(), " ms");
    }

    // What the front end has handed over cannot be placed.
    template<typename... Arguments>
    void fail(const Arguments&... arguments)
    {
        static constexpr unsigned mostPrinted = 100;
        if (m_numberOfFailures++ < mostPrinted)
            dataLogLn("AOT placement: ", arguments...);
    }

    void crashIfFailed()
    {
        if (!m_numberOfFailures)
            return;
        dataLogLn("AOT placement: ", m_numberOfFailures.load(), " faults in what the front end has handed over");
        RELEASE_ASSERT_NOT_REACHED();
    }

    void checkProgram()
    {
        auto& layouts = m_program.layouts;
        auto checkList = [&](unsigned index, const Vector<LayoutID>& list, ASCIILiteral what) {
            for (unsigned i = 0; i < list.size(); ++i) {
                if (list[i].index() >= layouts.size() || !isObjectLayout(m_program.at(list[i]).kind) || !m_program.at(list[i]).isAllocated)
                    fail("layout ", index, " ", nameOf(layouts[index]), " has among its ", what, " layout ", list[i].index(), ", which is not one objects are allocated with");
                if (i && list[i - 1] >= list[i])
                    fail("the ", what, " of layout ", index, " ", nameOf(layouts[index]), " are not sorted");
            }
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            m_mostFields = std::max<uint32_t>(m_mostFields, layout.fields.size());
            if (!isObjectLayout(layout.kind)) {
                if (!layout.members.isEmpty() || !layout.specializations.isEmpty())
                    fail("layout ", i, " is not of objects and has members or specializations");
                if (!std::ranges::all_of(layout.fields, exists))
                    fail("layout ", i, " is not of objects and has a field that is never stored");
                continue;
            }
            if (layout.base && layout.base.index() >= i)
                fail("layout ", i, " ", nameOf(layout), " comes before the layout it extends");
            if (layout.fields.size() > UINT16_MAX)
                fail("layout ", i, " ", nameOf(layout), " has ", layout.fields.size(), " fields, more than an order can list");
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
            checkList(i, layout.members, "members"_s);
            checkList(i, layout.specializations, "specializations"_s);
            // With no members it would pass for an object of any view.
            if (layout.isAllocated && !std::ranges::binary_search(layout.members, LayoutID(i)))
                fail("layout ", i, " ", nameOf(layout), " is allocated and is not a member of itself");
            for (LayoutID specialization : layout.specializations) {
                if (specialization == LayoutID(i) || !std::ranges::binary_search(layout.members, specialization))
                    fail("layout ", i, " ", nameOf(layout), " has the specialization ", specialization.index(), ", which is not another of its members");
            }
        }

        auto& allocations = m_program.allocations;
        RELEASE_ASSERT(!allocations.isEmpty() && allocations[0].layout == m_program.closureLayout);
        Vector<uint32_t> lastAllocationListing(FillWith { }, m_mostFields, none);
        for (unsigned i = 0; i < allocations.size(); ++i) {
            auto& allocation = allocations[i];
            if (allocation.layout.index() >= layouts.size() || !m_program.at(allocation.layout).isAllocated) {
                fail("allocation ", i, " is of layout ", allocation.layout.index(), ", which is not allocated");
                continue;
            }
            auto& layout = m_program.at(allocation.layout);
            if (allocation.born > allocation.order.size())
                fail("allocation ", i, " of ", nameOf(layout), " has more fields born than it lists");
            for (uint16_t field : allocation.order) {
                if (field >= layout.fields.size() || std::exchange(lastAllocationListing[field], i) == i)
                    fail("allocation ", i, " of ", nameOf(layout), " lists field ", field, ", which is not there or is listed twice");
            }
        }

        for (auto& variable : m_program.variables) {
            if (variable.startsUndefined && variable.type && !(m_program.at(variable.type).primitives & Primitive::Undefined))
                fail("variable ", nameOf(variable.name), " starts out as undefined, which is not of its type");
            if (variable.startsZero) {
                Type type = variable.type ? m_program.at(variable.type) : Type::anything();
                Rep rep = repOf(type);
                if (rep != Rep::Int32 && rep != Rep::Int64 && rep != Rep::Double)
                    fail("variable ", nameOf(variable.name), " starts out as zero, and is not kept as a number or a boolean");
                else if (rep == Rep::Int32 ? !(type.primitives & Primitive::False) : rep == Rep::Int64 && !type.integers.contains(0))
                    fail("variable ", nameOf(variable.name), " starts out as zero, which is not of its type");
            }
        }
        crashIfFailed();
    }

    struct NamedField {
        uint32_t layout;
        // Its number among the fields of all layouts.
        uint32_t field;
    };

    struct FieldsOfName {
        uint32_t first;
        uint32_t count;
    };

    // Stands for a set of layouts: sets with one fingerprint are taken for one set, and listed once. checkPlacement() does not take that on trust.
    struct Fingerprint {
        uint64_t low { 0 };
        uint64_t high { 0 };
        uint32_t count { 0 };

        void add(const std::array<uint64_t, 2>& hashOfLayout)
        {
            low += hashOfLayout[0];
            high += hashOfLayout[1];
            ++count;
        }
        friend auto operator<=>(const Fingerprint&, const Fingerprint&) = default;
    };

    // SplitMix64.
    static uint64_t hashOf(uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ value >> 30) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ value >> 27) * 0x94d049bb133111ebull;
        return value ^ value >> 31;
    }

    // The fields of all layouts are numbered through, those of one layout in a row. Those of objects are also listed by name, in that order: only fields of one name share a slot.
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
                firstFieldNamed[field.name.index() + 1] += exists(field);
        }
        m_firstField.append(next);
        // A view with a slot for what a member has no bytes for would read what is not there.
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!exists(field) && firstFieldNamed[field.name.index() + 1])
                    fail("the property ", nameOf(field.name), " is never stored in ", nameOf(layout), " and is stored elsewhere");
            }
        }
        crashIfFailed();
        for (unsigned i = 0; i < m_program.strings.size(); ++i) {
            if (firstFieldNamed[i + 1])
                m_names.append(FieldsOfName { firstFieldNamed[i], firstFieldNamed[i + 1] });
            firstFieldNamed[i + 1] += firstFieldNamed[i];
        }
        m_fieldsByName.grow(firstFieldNamed.last());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            m_hashOfLayout.append({ hashOf(2 * i), hashOf(2 * i + 1) });
            if (!isObjectLayout(layouts[i].kind))
                continue;
            for (unsigned k = 0; k < layouts[i].fields.size(); ++k) {
                if (exists(layouts[i].fields[k]))
                    m_fieldsByName[firstFieldNamed[layouts[i].fields[k].name.index()]++] = NamedField { i, m_firstField[i] + k };
            }
        }
    }

    Field& fieldAt(NamedField field) { return m_program.layouts[field.layout].fields[field.field - m_firstField[field.layout]]; }

    // What a cell takes in the process that runs the program.
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

    // Layouts that transitions lead between are a group. The least of them stands for it.
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
                auto [least, other] = std::minmax({ find(i), find(target.index()) });
                m_group[other] = least;
            }
        }
        for (unsigned i = 0; i < layouts.size(); ++i)
            m_group[i] = find(i);
        m_isBornOrNamedByCast.ensureSize(layouts.size());
        for (auto& allocation : m_program.allocations)
            m_isBornOrNamedByCast.quickSet(allocation.layout.index());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (m_castsOfWhatIsNotReadOutOfPlace[i] || layouts[i].kind == LayoutKind::Instance)
                m_isBornOrNamedByCast.quickSet(i);
        }
    }

    std::span<const uint32_t> viewsOf(uint32_t layout) const { return m_views.span().subspan(m_firstView[layout], m_firstView[layout + 1] - m_firstView[layout]); }

    void indexViews()
    {
        auto& layouts = m_program.layouts;
        m_firstView.fill(0, layouts.size() + 1);
        for (auto& view : layouts) {
            for (LayoutID member : view.members)
                ++m_firstView[member.index() + 1];
        }
        for (unsigned i = 0; i < layouts.size(); ++i)
            m_firstView[i + 1] += m_firstView[i];
        m_views.grow(m_firstView.last());
        Vector<uint32_t> next = m_firstView;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            for (LayoutID member : layouts[i].members)
                m_views[next[member.index()]++] = i;
        }
    }

    // While the functor runs, the table leads from a name to the index of the field of the layout.
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

    struct FieldsOfOneName {
        // By layout: the place of its field among those of the name.
        Vector<uint32_t> placeOfLayout;
        // By place.
        Vector<uint32_t> representative;
    };

    // One name after the other, on all threads. The functor is handed the fields of the name.
    void forEachName(const Invocable<void(FieldsOfOneName&, std::span<const NamedField>)> auto& functor)
    {
        forEachIndexOnAllThreads<FieldsOfOneName>(m_names.size(), m_views.size(), [&] {
            return FieldsOfOneName { Vector<uint32_t>(FillWith { }, m_program.layouts.size(), none), { } };
        }, [&](FieldsOfOneName& table, unsigned index) {
            auto fields = m_fieldsByName.span().subspan(m_names[index].first, m_names[index].count);
            for (uint32_t place = 0; place < fields.size(); ++place)
                table.placeOfLayout[fields[place].layout] = place;
            functor(table, fields);
            for (auto& field : fields)
                table.placeOfLayout[field.layout] = none;
        });
    }

    // Those of the layouts, which are sorted, that have a field of the name, in their order, by its place. Whichever is shorter is gone through: the layouts, or the fields of the name.
    void forEachWithField(const FieldsOfOneName& table, std::span<const NamedField> fields, const Vector<LayoutID>& layouts, const Invocable<void(uint32_t place)> auto& functor)
    {
        static constexpr unsigned costOfSearch = 16;
        if (fields.size() * costOfSearch < layouts.size()) {
            for (uint32_t place = 0; place < fields.size(); ++place) {
                if (std::ranges::binary_search(layouts, LayoutID(fields[place].layout)))
                    functor(place);
            }
            return;
        }
        for (LayoutID layout : layouts) {
            if (uint32_t place = table.placeOfLayout[layout.index()]; place != none)
                functor(place);
        }
    }

    void forEachMemberWithField(const FieldsOfOneName& table, std::span<const NamedField> fields, const Layout& view, const Invocable<void(uint32_t place)> auto& functor)
    {
        forEachWithField(table, fields, view.members, functor);
    }

    // The fields that compiled code gets at by the offset they have in a view, whatever member the object is of: as the lowering comes by
    // them, see fieldOf() and Lowering::fieldExpectedFor(). The first stores to an object go by the layout it has just been made with.
    // initialStoresOf() of a function.
    struct InitialStores {
        uint32_t function;
        // The type of `this`.
        LayoutID view;
        Vector<StringID> names;
        // Function::numberOfInitialStores.
        uint16_t count { 0 };
    };

    void findInitialStores()
    {
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            auto stores = initialStoresOf(function);
            if (stores.empty())
                continue;
            InitialStores found { i, m_program.typeOf(function, function.at(stores[0]).child(0)).onlyView(), { }, 0 };
            for (ValueID store : stores) {
                RELEASE_ASSERT(function.at(store).opcode == Opcode::PutById && function.at(store).child(0) == function.at(stores[0]).child(0) && !found.names.contains(function.at(store).string()));
                found.names.append(function.at(store).string());
            }
            if (found.view && m_program.at(found.view).kind == LayoutKind::Record)
                m_initialStores.append(WTF::move(found));
        }
    }

    // Which field of the type of `this` the store goes to, if it has one.
    std::optional<unsigned> fieldOf(const InitialStores& initial, unsigned index) const
    {
        auto found = m_program.at(initial.view).fieldNamed(initial.names[index]);
        return found && exists(m_program.at(initial.view).fields[*found]) ? found : std::nullopt;
    }

    void forEachLayoutInitializedBy(const InitialStores& initial, const Invocable<void(uint32_t)> auto& functor) const
    {
        for (LayoutID member : m_program.at(initial.view).members) {
            if (m_program.at(member).isAllocated && isObjectLayout(m_program.at(member).kind))
                functor(member.index());
        }
    }

    // Code that stores without a test stores to one offset, whatever the object: nobody may lack the field. It is a prefix, not a
    // selection, so that the keys come in the order of the stores.
    void countInitialStores()
    {
        for (auto& initial : m_initialStores) {
            while (initial.count < std::min<size_t>(initial.names.size(), maxFieldsInOrder)) {
                auto field = fieldOf(initial, initial.count);
                if (!field || m_lackedFieldOfField[m_firstField[initial.view.index()] + *field] != none)
                    break;
                ++initial.count;
            }
        }
    }

    // `new callee(..)`: the operation that is handed the object the front end has made for it, as its second argument.
    static bool handsObjectToConstructor(const Value& value)
    {
        if (value.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Operation && (implementation.operation == LanguageOperation::ConstructWith || implementation.operation == LanguageOperation::ConstructWithSpread);
    }

    template<typename T>
    static bool contains(std::span<const T> list, T value) { return std::ranges::find(list, value) != list.end(); }

    // The names that the function stores to a fresh `this` without a test, and counts among its keys.
    std::span<const StringID> namesStoredFirstBy(uint32_t function) const
    {
        uint32_t index = m_initialStoresOfFunction[function];
        return index == none ? std::span<const StringID>() : m_initialStores[index].names.span().first(m_initialStores[index].count);
    }

    // An object of the allocation is not born with the field. Whether it gets it, counted among its keys, before anybody looks: it is
    // born with nothing, and goes nowhere but to functions that have the field among their initial stores: the one that compiled code
    // calls, those that declare `this` to be of the layout, for which the run time makes it, and of all others that `new` turns out to
    // call only those that store first all that is never seen absent: Data::refusedFreshObjectsOffset.
    bool getsFieldBeforeAnyoneLooks(uint32_t index, StringID name) const
    {
        auto& allocation = m_program.allocations[index];
        if (allocation.born)
            return false;
        if (allocation.constructedBy)
            return contains(namesStoredFirstBy(allocation.constructedBy.index()), name);
        if (m_isUsedOtherwise.quickGet(index))
            return false;
        uint32_t layout = allocation.layout.index();
        return !m_isLayoutOfThis.quickGet(layout) || m_namesStoredFirstByAll[layout].contains(name);
    }

    void forEachFieldThatIsNotBorn(const Invocable<void(uint32_t allocation, Layout&, Field&)> auto& functor)
    {
        for (uint32_t i = 0; i < m_program.allocations.size(); ++i) {
            auto& allocation = m_program.allocations[i];
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind))
                continue;
            auto born = allocation.order.span().first(allocation.born);
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                if (!contains(born, static_cast<uint16_t>(k)))
                    functor(i, layout, layout.fields[k]);
            }
        }
    }

    // fieldIsNeverSeenAbsent is true of the program as it was written. A slot that cannot say "absent" takes more: see
    // getsFieldBeforeAnyoneLooks(). The passes may have parted an allocation from the stores that followed it, or found out that `new`
    // calls a function that stores nothing. Where that is so the flag goes, and the slot gets its marker.
    void dropFlagsThatDoNotHold()
    {
        auto& layouts = m_program.layouts;
        auto& allocations = m_program.allocations;
        m_initialStoresOfFunction.fill(none, m_program.functions.size());
        for (uint32_t i = 0; i < m_initialStores.size(); ++i)
            m_initialStoresOfFunction[m_initialStores[i].function] = i;

        // Every function that declares `this`, with initial stores or without.
        m_isLayoutOfThis.ensureSize(layouts.size());
        m_namesStoredFirstByAll.grow(layouts.size());
        for (uint32_t i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            LayoutID view = function.kind == FunctionKind::Plain && function.thisType ? m_program.at(function.thisType).onlyView() : LayoutID();
            if (!view || layouts[view.index()].kind != LayoutKind::Record || !layouts[view.index()].isAllocated)
                continue;
            auto names = namesStoredFirstBy(i);
            auto& byAll = m_namesStoredFirstByAll[view.index()];
            if (!m_isLayoutOfThis.quickSet(view.index())) {
                for (StringID name : names)
                    byAll.append(name);
            } else {
                byAll.removeAllMatching([&](StringID name) {
                    return !contains(names, name);
                });
            }
        }

        // Whether all that becomes of an object is that `new` hands it on.
        m_isUsedOtherwise.ensureSize(allocations.size());
        static constexpr uint8_t countMask = 3;
        static constexpr uint8_t isHandedOn = 4;
        forEachFunctionInOrder<Vector<uint32_t>>([&](Function& function) {
            Vector<uint32_t> usedOtherwise;
            // By value: how often it is used, up to two, and whether `new` hands it on.
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
                    if (handsObjectToConstructor(value) && value.arguments.size() > 1)
                        uses[value.arguments[1].index()] |= isHandedOn;
                }
                block.terminator.forEachChild(use);
            }
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::NewObjectWithShape && isObjectLayout(m_program.at(value.layout()).kind) && uses[id.index()] != (1 | isHandedOn))
                        usedOtherwise.append(value.allocation());
                }
            }
            return usedOtherwise;
        }, [&](Vector<uint32_t>&& usedOtherwise) {
            for (uint32_t allocation : usedOtherwise)
                m_isUsedOtherwise.quickSet(allocation);
        }, holdsNewObjectWithShape);
        for (uint32_t i = 0; i < allocations.size(); ++i) {
            for (uint32_t k = 0; allocations[i].numberOfLiterals && k <= allocations[i].numberOfLiterals && m_isUsedOtherwise.quickGet(i); ++k)
                m_isUsedOtherwise.quickSet(allocations[i].allocationOfFirstLiteral + k);
        }

        unsigned numberOfFlags = 0;
        for (auto& layout : layouts) {
            for (auto& field : layout.fields)
                numberOfFlags += !!(field.flags & fieldIsNeverSeenAbsent);
        }
        if (!numberOfFlags)
            return;
        unsigned numberNotBorn = 0;
        unsigned numberNotStoredFirst = 0;
        forEachFieldThatIsNotBorn([&](uint32_t index, Layout& layout, Field& field) {
            if (!(field.flags & fieldIsNeverSeenAbsent) || getsFieldBeforeAnyoneLooks(index, field.name))
                return;
            field.flags &= ~fieldIsNeverSeenAbsent;
            auto& allocation = allocations[index];
            bool isForFunction = !allocation.born && (allocation.constructedBy || !m_isUsedOtherwise.quickGet(index));
            ++(isForFunction ? numberNotStoredFirst : numberNotBorn);
            if (allocation.constructedBy && isForFunction)
                report("  ", nameOf(layout), ".", nameOf(field.name), " may be seen absent: allocation ", index, " is for `new` of ", nameOf(m_program.at(allocation.constructedBy).name), ", which does not store it first");
            else if (isForFunction)
                report("  ", nameOf(layout), ".", nameOf(field.name), " may be seen absent: allocation ", index, " is for `new`, and some function that declares `this` to be of the layout does not store it first");
            else
                report("  ", nameOf(layout), ".", nameOf(field.name), " may be seen absent: allocation ", index, ", born with ", allocation.born, " of ", layout.fields.size(), " fields, does not bear it");
        });
        // A transition neither adds such a field nor changes what is known of one.
        unsigned numberAcrossTransitions = 0;
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (unsigned i = 0; i < layouts.size(); ++i) {
                if (layouts[i].specializations.isEmpty())
                    continue;
                withFieldsByName(m_fieldNamed, i, [&] {
                    for (LayoutID specialization : layouts[i].specializations) {
                        for (auto& field : m_program.at(specialization).fields) {
                            uint32_t own = m_fieldNamed[field.name.index()];
                            bool isFlaggedBefore = own != none && layouts[i].fields[own].flags & fieldIsNeverSeenAbsent;
                            if (isFlaggedBefore == !!(field.flags & fieldIsNeverSeenAbsent))
                                continue;
                            field.flags &= ~fieldIsNeverSeenAbsent;
                            if (own != none)
                                layouts[i].fields[own].flags &= ~fieldIsNeverSeenAbsent;
                            ++numberAcrossTransitions;
                            didChange = true;
                        }
                    }
                });
            }
        }
        report(numberOfFlags, " fields are said never to be seen absent. Not so: ", numberNotBorn, " that an allocation does not bear, ", numberNotStoredFirst, " that a function `new` may call does not store first, ", numberAcrossTransitions, " more on the other side of a transition");
    }

    void findAccessedFields()
    {
        m_isAccessed.ensureSize(m_firstField.last());
        m_isStoredThrough.ensureSize(m_firstField.last());
        static constexpr uint32_t isStoredThrough = 1u << 31;
        static constexpr uint32_t isNotAccessed = 1u << 30;
        m_heldByFunction.fill(0, m_program.functions.size());
        forEachFunctionInOrder<Vector<uint32_t>>([&](Function& function) {
            Vector<uint32_t> accessed;
            uint8_t held = 0;
            for (auto& block : function.blocks) {
                for (size_t index = 0; index < block.values.size(); ++index) {
                    auto& value = function.at(block.values[index]);
                    held |= heldWith(value.opcode);
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
                            LayoutID view = type.onlyView();
                            auto found = view ? m_program.at(view).fieldNamed(value.string()) : std::nullopt;
                            if (!found)
                                return;
                            uint32_t field = m_firstField[view.index()] + *found;
                            if (value.opcode == Opcode::DeleteById)
                                field |= isStoredThrough | isNotAccessed;
                            else if (value.opcode == Opcode::PutById || value.opcode == Opcode::CallMethod)
                                field |= isStoredThrough;
                            accessed.append(field);
                        };
                        isAccessedThrough(m_program.typeOf(function, value.child(0)));
                        if (TypeID prediction = function.at(value.child(0)).prediction)
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
                }
            }
            m_heldByFunction[&function - &std::as_const(m_program.functions)[0]] = held;
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

    // The field that comes first stands for all that share its slot, whatever the order they are brought together in.
    void shareSlots()
    {
        auto& layouts = m_program.layouts;
        uint32_t numberOfFields = m_firstField.last();
        Vector<uint32_t> representativeOfField(numberOfFields, [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        m_membersWith.fill(Fingerprint { }, numberOfFields);
        forEachName([&](FieldsOfOneName& table, std::span<const NamedField> fields) {
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
                    auto& view = layouts[fields[place].layout];
                    bool mayPart = !view.membersByFit.isEmpty() && !m_isStoredThrough.quickGet(fields[place].field);
                    forEachMemberWithField(table, fields, view, [&](uint32_t other) {
                        auto& ofView = fieldAt(fields[place]);
                        auto& own = fieldAt(fields[other]);
                        bool saysTheSame = own.type == ofView.type && !((own.flags ^ ofView.flags) & fieldIsOptional);
                        if (!mayPart || saysTheSame || !std::ranges::binary_search(view.membersByFit, LayoutID(fields[other].layout)))
                            share(other);
                    });
                }
                // What an object is turned into has its fields where they are.
                forEachWithField(table, fields, layouts[fields[place].layout].specializations, share);
            }
            for (uint32_t place = 0; place < fields.size(); ++place) {
                Fingerprint membersWithIt;
                if (m_isAccessed.quickGet(fields[place].field)) {
                    forEachMemberWithField(table, fields, layouts[fields[place].layout], [&](uint32_t other) {
                        if (representativeOf(other) == representativeOf(place))
                            membersWithIt.add(m_hashOfLayout[fields[other].layout]);
                        else
                            fieldAt(fields[place]).isElsewhereInSomeMember = true;
                    });
                }
                m_membersWith[fields[place].field] = membersWithIt;
            }
            for (uint32_t place = 0; place < fields.size(); ++place)
                representativeOfField[fields[place].field] = fields[representativeOf(place)].field;
        });
        Vector<uint32_t> slotOfRepresentative(FillWith { }, numberOfFields, none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& fields = layouts[i].fields;
            for (unsigned k = 0; k < fields.size(); ++k) {
                if (!exists(fields[k]))
                    continue;
                uint32_t& slot = slotOfRepresentative[representativeOfField[m_firstField[i] + k]];
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

    struct LackedField {
        uint32_t layout;
        uint32_t field;
        // Among m_setsOfMembers.
        uint32_t membersWithIt { none };
        uint64_t numberOfAccesses { 0 };
    };

    struct SetOfMembers {
        // The lacked field that it was first found for.
        uint32_t listedFor;
        // In m_listedMembers.
        uint32_t firstMember;
        uint32_t numberOfMembers;
    };

    std::span<const uint32_t> membersIn(const SetOfMembers& set) const { return m_listedMembers.span().subspan(set.firstMember, set.numberOfMembers); }

    // No room is kept in a member for a field it has not. An access through the view tests for the members that have it.
    void findLackedFields()
    {
        auto& layouts = m_program.layouts;
        m_lackedFieldOfField.fill(none, m_firstField.last());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind))
                continue;
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                // Whoever comes by it all the same finds a field that no member has, at an offset no object reaches.
                if (!exists(layout.fields[k])) {
                    layout.fields[k].isLackedBySomeMember = true;
                    layout.fields[k].offset = none;
                    continue;
                }
                // Whoever comes by it all the same tests the shape, in vain, and asks the object.
                if (!m_isAccessed.quickGet(m_firstField[i] + k)) {
                    layout.fields[k].isLackedBySomeMember = true;
                    continue;
                }
                if (m_membersWith[m_firstField[i] + k].count == layout.members.size())
                    continue;
                layout.fields[k].isLackedBySomeMember = true;
                m_lackedFieldOfField[m_firstField[i] + k] = m_lackedFields.size();
                m_lackedFields.append(LackedField { i, k });
            }
        }
        // Many fields are had by the same members: those of a type, by all that are really of the type.
        auto membersWith = [&](uint32_t lacked) -> const Fingerprint& {
            return m_membersWith[m_firstField[m_lackedFields[lacked].layout] + m_lackedFields[lacked].field];
        };
        Vector<uint32_t> sortedLackedFields(m_lackedFields.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        std::ranges::sort(sortedLackedFields, [&](uint32_t a, uint32_t b) {
            return membersWith(a) != membersWith(b) ? membersWith(a) < membersWith(b) : a < b;
        });
        uint32_t numberOfMembers = 0;
        for (uint32_t index : sortedLackedFields) {
            if (m_setsOfMembers.isEmpty() || membersWith(m_setsOfMembers.last().listedFor) != membersWith(index)) {
                m_setsOfMembers.append(SetOfMembers { index, numberOfMembers, membersWith(index).count });
                numberOfMembers += membersWith(index).count;
            }
            m_lackedFields[index].membersWithIt = m_setsOfMembers.size() - 1;
        }
        m_listedMembers.fill(none, numberOfMembers);
        forEachName([&](FieldsOfOneName& table, std::span<const NamedField> fields) {
            for (auto& field : fields) {
                uint32_t lacked = m_lackedFieldOfField[field.field];
                if (lacked == none || m_setsOfMembers[m_lackedFields[lacked].membersWithIt].listedFor != lacked)
                    continue;
                uint32_t next = m_setsOfMembers[m_lackedFields[lacked].membersWithIt].firstMember;
                forEachMemberWithField(table, fields, layouts[field.layout], [&](uint32_t place) {
                    if (fieldAt(fields[place]).slot == fieldAt(field).slot)
                        m_listedMembers[next++] = fields[place].layout;
                });
                auto& listed = m_setsOfMembers[m_lackedFields[lacked].membersWithIt];
                RELEASE_ASSERT(next == listed.firstMember + listed.numberOfMembers);
            }
        });
        // An object of a class has methods and accessors on its prototype, and any object has those of Object.prototype. Anything else is a fault of the front end.
        for (auto& lacked : m_lackedFields) {
            auto& view = layouts[lacked.layout];
            auto& field = view.fields[lacked.field];
            if (field.flags & fieldIsOptional)
                continue;
            ++m_numberOfRequiredFieldsLacked;
            if (!Options::verboseAOTCompilation())
                continue;
            auto membersWithIt = membersIn(m_setsOfMembers[lacked.membersWithIt]);
            size_t next = 0;
            for (LayoutID member : view.members) {
                if (next < membersWithIt.size() && membersWithIt[next] == member.index()) {
                    ++next;
                    continue;
                }
                if (m_program.at(member).kind != LayoutKind::Record || std::ranges::binary_search(view.membersByFit, member))
                    continue;
                report("view ", lacked.layout, " ", nameOf(view), " requires ", nameOf(field.name), ", for which its member ", member.index(), " ", nameOf(m_program.at(member)), ", a record, has no field");
                break;
            }
        }
    }

    // A member of a layout need not have its optional fields, and may use their bytes for something else: no object can be turned into one of it in place.
    void dropSpecializationsThatLackFields()
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
            size_t numberOfFields = std::ranges::count_if(layout.fields, exists);
            withFieldsByName(fieldNamed, specialized[index], [&] {
                layout.specializations.removeAllMatching([&](LayoutID id) {
                    unsigned numberOfSharedFields = 0;
                    for (auto& field : m_program.at(id).fields) {
                        uint32_t found = fieldNamed[field.name.index()];
                        if (found == none)
                            continue;
                        ++numberOfSharedFields;
                    }
                    return numberOfSharedFields != numberOfFields;
                });
            });
        });
        uint64_t numberLeft = 0;
        for (uint32_t index : specialized)
            numberLeft += layouts[index].specializations.size();
        report(numberOfSpecializations, " specializations of ", specialized.size(), " layouts, of which ", numberOfSpecializations - numberLeft, " lack a field of the layout and are none");
    }

    std::span<const LayoutID> retaggedMembersOf(uint32_t view) const { return m_retaggedMembers.span().subspan(m_firstRetaggedMember[view], m_firstRetaggedMember[view + 1] - m_firstRetaggedMember[view]); }
    std::span<const uint32_t> retagSourcesOf(uint32_t view) const { return m_retagSources.span().subspan(m_firstRetagSource[view], m_firstRetagSource[view + 1] - m_firstRetagSource[view]); }

    // An object that a cast turns into one of a specialization stays in the places it is in, and gets into views it was no member of: Layout::retagSources.
    void followSpecializations()
    {
        auto& layouts = m_program.layouts;
        // The run time leaves an object of such a class as it is: what it finds on its prototype compiled code goes by the shape for.
        BitVector hasPrototypeMembers;
        hasPrototypeMembers.ensureSize(layouts.size());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].kind == LayoutKind::Instance && (!layouts[i].methods.isEmpty() || (layouts[i].base && hasPrototypeMembers.quickGet(layouts[i].base.index()))))
                hasPrototypeMembers.quickSet(i);
        }
        // Layouts with the same specializations are seen to together.
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
                numberOfSteps += viewsOf(specialization.index()).size();
        }
        // By view: how many of the specializations of the group at hand are members of it.
        struct Counts {
            Vector<uint32_t> group;
            Vector<uint32_t> count;
            // The views that casts coerce to and that one of the specializations makes an object a member of.
            Vector<uint32_t> groupThatGetsInto;
            Vector<uint32_t> viewsToGetInto;
        };
        // By group: a view, and a layout that is no member of it.
        Vector<Vector<std::pair<uint32_t, uint32_t>>> retagSourcesOfGroups(groups.size());
        // By group: a view, and a member of it that can become the layout of the view.
        Vector<Vector<std::pair<uint32_t, uint32_t>>> retaggedMembersOfGroups(groups.size());
        forEachIndexOnAllThreads<Counts>(groups.size(), numberOfSteps, [&] {
            return Counts { Vector<uint32_t>(FillWith { }, layouts.size(), none), Vector<uint32_t>(FillWith { }, layouts.size(), 0), Vector<uint32_t>(FillWith { }, layouts.size(), none), { } };
        }, [&](Counts& counts, unsigned group) {
            auto& specializations = layouts[specialized[groups[group].first].second].specializations;
            counts.viewsToGetInto.shrink(0);
            for (LayoutID specialization : specializations) {
                bool isRecord = m_program.at(specialization).kind == LayoutKind::Record;
                for (uint32_t view : viewsOf(specialization.index())) {
                    if (std::exchange(counts.group[view], group) != group)
                        counts.count[view] = 0;
                    ++counts.count[view];
                    // For a view the run time makes an object one of the layout of the view, or of a member where nothing is allocated with that. Only of records.
                    if (!m_coercionsToView[view] || !isRecord || m_program.at(specialization).isAbstract || (layouts[view].isAllocated && view != specialization.index()))
                        continue;
                    if (std::exchange(counts.groupThatGetsInto[view], group) != group)
                        counts.viewsToGetInto.append(view);
                }
            }
            std::ranges::sort(counts.viewsToGetInto);
            for (uint32_t i = 0; i < groups[group].second; ++i) {
                uint32_t layout = specialized[groups[group].first + i].second;
                if (layouts[layout].isAllocated && !hasPrototypeMembers.quickGet(layout)) {
                    auto views = viewsOf(layout);
                    size_t next = 0;
                    for (uint32_t view : counts.viewsToGetInto) {
                        while (next < views.size() && views[next] < view)
                            ++next;
                        if (next == views.size() || views[next] != view)
                            retagSourcesOfGroups[group].append({ view, layout });
                    }
                    for (LayoutID specialization : specializations) {
                        if (m_program.at(specialization).kind != LayoutKind::Record || m_program.at(specialization).isAbstract || !std::ranges::binary_search(views, specialization.index()))
                            continue;
                        retaggedMembersOfGroups[group].append({ specialization.index(), layout });
                        if (m_coercionsToView[specialization.index()])
                            retagSourcesOfGroups[group].append({ specialization.index(), layout });
                    }
                }
                for (uint32_t view : viewsOf(layout)) {
                    if (counts.group[view] == group && counts.count[view] == specializations.size())
                        continue;
                    for (LayoutID specialization : specializations) {
                        if (!std::ranges::binary_search(layouts[view].members, specialization))
                            fail("layout ", layout, " ", nameOf(layouts[layout]), " is a member of view ", view, " ", nameOf(layouts[view]), ", its specialization ", specialization.index(), " ", nameOf(m_program.at(specialization)), " is not");
                    }
                }
            }
        });
        crashIfFailed();

        Vector<std::pair<uint32_t, uint32_t>> retagSources;
        for (auto& ofGroup : retagSourcesOfGroups)
            retagSources.appendVector(ofGroup);
        std::ranges::sort(retagSources);
        m_firstRetagSource.fill(0, layouts.size() + 1);
        for (auto [view, layout] : retagSources) {
            ++m_firstRetagSource[view + 1];
            m_retagSources.append(layout);
        }
        unsigned numberOfViews = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            numberOfViews += !!m_firstRetagSource[i + 1];
            m_firstRetagSource[i + 1] += m_firstRetagSource[i];
        }
        report("casts make transitions to get objects into ", numberOfViews, " views, from ", m_retagSources.size(), " layouts in all");

        Vector<std::pair<uint32_t, uint32_t>> retaggedMembers;
        for (auto& ofGroup : retaggedMembersOfGroups)
            retaggedMembers.appendVector(ofGroup);
        std::ranges::sort(retaggedMembers);
        m_firstRetaggedMember.fill(0, layouts.size() + 1);
        for (auto [view, layout] : retaggedMembers) {
            ++m_firstRetaggedMember[view + 1];
            m_retaggedMembers.append(LayoutID(layout));
        }
        for (unsigned i = 0; i < layouts.size(); ++i)
            m_firstRetaggedMember[i + 1] += m_firstRetaggedMember[i];
        report(m_retaggedMembers.size(), " members of views can become the layout of the view");
    }

    // Whether there are objects, environments or frames of the layout itself.
    static bool isMade(const Layout& layout)
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

    // A slot holds what the layouts say that objects are made as: the run time holds what it stores to the type of the object's own
    // field. What a view says that nothing is made as is what a read may find there, in some member or other. What compiled code
    // stores through such a view is added by scanFunctions(). Only a slot that nothing made has goes by its views.
    void collectTypes()
    {
        m_isExpectedInt32.ensureSize(m_program.slots.size());
        m_hasIntegerField.ensureSize(m_program.slots.size());
        m_isNeverSeenAbsent.ensureSize(m_program.slots.size());
        m_mayBeSeenAbsent.ensureSize(m_program.slots.size());
        for (bool made : { true, false }) {
            for (auto& layout : m_program.layouts) {
                if (isMade(layout) != made)
                    continue;
                for (auto& field : layout.fields) {
                    if (!exists(field) || (!made && m_isInMadeLayout.quickGet(field.slot.index())))
                        continue;
                    holds(field.slot, field.type ? field.type : m_program.types.anything);
                    if (field.flags & fieldIsExpectedInt32)
                        m_isExpectedInt32.quickSet(field.slot.index());
                    if (field.type && m_program.at(field.type).isInteger())
                        m_hasIntegerField.quickSet(field.slot.index());
                    (field.flags & fieldIsNeverSeenAbsent ? m_isNeverSeenAbsent : m_mayBeSeenAbsent).quickSet(field.slot.index());
                }
            }
            if (!made)
                break;
            m_isInMadeLayout.ensureSize(m_program.slots.size());
            for (unsigned i = 0; i < m_program.slots.size(); ++i) {
                if (m_program.slots[i].type)
                    m_isInMadeLayout.quickSet(i);
            }
        }
    }

    static constexpr uint64_t weightInLoop = 8;

    // Blocks in a cycle of the control flow graph: strongly connected components, iteratively.
    static void findBlocksInLoops(const Function& function, BitVector& isInLoop)
    {
        unsigned count = function.blocks.size();
        isInLoop = BitVector(count);
        Vector<uint32_t> index(FillWith { }, count, none);
        Vector<uint32_t> low(FillWith { }, count, 0);
        BitVector isOnStack;
        isOnStack.ensureSize(count);
        Vector<uint32_t> stack;
        // A block, and the next of its successors to look at.
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

    // How often compiled code makes which test, going by the operations. And which fields of environments every allocation initializes.
    void scanFunctions()
    {
        auto& layouts = m_program.layouts;
        m_testsOfType.fill(0, m_program.types.size());
        m_testsOfView.fill(0, layouts.size());
        m_testsOfClass.fill(0, layouts.size());
        m_coercionsToView.fill(0, layouts.size());
        m_castsOfWhatIsNotReadOutOfPlace.fill(0, layouts.size());
        m_numberOfBirths.fill(0, layouts.size());
        m_isMadeOften.ensureSize(layouts.size());
        Vector<std::pair<SlotID, TypeID>> allStoredThroughViews;
        m_numberOfBirthsInitializing.fill(0, m_firstField.last());
        m_accessWeight.fill(0, m_program.slots.size());
        m_numberOfStoresOfDoubles.fill(0, m_program.slots.size());
        auto isDoubleAsStored = [&](Function& function, ValueID id) {
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
        auto noteRecordsCopied = [&](Function& function, const Value& value, Vector<uint32_t>& copied) {
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
                for (LayoutID view : type.views) {
                    copied.append(view.index());
                    for (LayoutID member : m_program.at(view).members)
                        copied.append(member.index());
                }
            };
            for (ValueID argument : value.arguments)
                note(m_program.typeOf(function, argument));
            note(m_program.at(value.type));
            if (value.prediction)
                note(m_program.at(value.prediction));
        };
        struct Found {
            Vector<std::pair<SlotID, TypeID>> storedThroughViews;
            Vector<uint32_t> madeOften;
            Vector<uint32_t> copied;
            Vector<uint32_t> firstStoredWithValue;
        };
        m_isFirstStoredWithValue.ensureSize(m_program.slots.size());
        auto isFirstStoreOfValue = [&](Function& function, const Block& block, size_t index) {
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
            auto [storedThroughViews, madeOften, copied, firstStoredWithValue] = Found { };
            Vector<uint32_t, 16> initializedAtThisBirth;
            BitVector isInLoop;
            findBlocksInLoops(function, isInLoop);
            for (auto& block : function.blocks) {
                uint64_t weight = isInLoop.get(&block - &function.blocks[0]) ? weightInLoop : 1;
                for (size_t index = 0; index < block.values.size(); ++index) {
                    auto& value = function.at(block.values[index]);
                    if (value.opcode == Opcode::CallBuiltin)
                        noteRecordsCopied(function, value, copied);
                    switch (value.opcode) {
                    case Opcode::NewObjectWithShape: {
                        uint32_t layout = value.layout().index();
                        if (layouts[layout].kind != LayoutKind::Environment)
                            break;
                        add(m_numberOfBirths[layout], 1);
                        if (isInLoop.get(&block - &function.blocks[0]) || !(value.inlineFrame ? m_program.at(function.inlineFrames[value.inlineFrame].callee) : function).captureTypes.isEmpty())
                            madeOften.append(layout);
                        initializedAtThisBirth.shrink(0);
                        for (size_t next = index + 1; next < block.values.size() && isInitializingStore(m_program, function, block, next); ++next) {
                            auto& store = function.at(block.values[next]);
                            if (store.opcode != Opcode::PutField || store.layout() != value.layout())
                                continue;
                            uint32_t field = m_firstField[layout] + store.field();
                            if (!initializedAtThisBirth.contains(field)) {
                                initializedAtThisBirth.append(field);
                                add(m_numberOfBirthsInitializing[field], 1);
                            }
                        }
                        break;
                    }
                    case Opcode::GetById:
                    case Opcode::PutById:
                    case Opcode::InById:
                    case Opcode::CallMethod: {
                        const Field* field = nullptr;
                        if (LayoutID view = m_program.typeOf(function, value.child(0)).onlyView()) {
                            if (auto found = m_program.at(view).fieldNamed(value.string()); found && exists(m_program.at(view).fields[*found])) {
                                field = &m_program.at(view).fields[*found];
                                if (uint32_t lacked = m_lackedFieldOfField[m_firstField[view.index()] + *found]; lacked != none)
                                    add(m_lackedFields[lacked].numberOfAccesses, 1);
                                add(m_accessWeight[field->slot.index()], weight);
                                if (value.opcode == Opcode::PutById && isDoubleAsStored(function, value.child(1)))
                                    add(m_numberOfStoresOfDoubles[field->slot.index()], 1);
                                if (value.opcode == Opcode::PutById && !isMade(m_program.at(view)))
                                    storedThroughViews.append({ field->slot, function.at(value.child(1)).type });
                                if (value.opcode == Opcode::PutById && isFirstStoreOfValue(function, block, index))
                                    firstStoredWithValue.append(field->slot.index());
                            }
                        }
                        if (value.opcode == Opcode::CallMethod || (value.opcode == Opcode::GetById && (!field || m_holdsSeveralTypes.quickGet(field->slot.index()))))
                            add(m_testsOfType[value.type.index()], 1);
                        break;
                    }
                    case Opcode::PutField:
                        if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && isDoubleAsStored(function, value.child(1)))
                            add(m_numberOfStoresOfDoubles[field.slot.index()], 1);
                        if (auto& layout = m_program.at(value.layout()); !isMade(layout))
                            storedThroughViews.append({ layout.fields[value.field()].slot, function.at(value.child(1)).type });
                        if (auto& field = m_program.at(value.layout()).fields[value.field()]; exists(field) && isObjectLayout(m_program.at(value.layout()).kind) && isFirstStoreOfValue(function, block, index))
                            firstStoredWithValue.append(field.slot.index());
                        break;
                    case Opcode::Cast:
                        // It tests for what it can make something of, not for what comes of it.
                        if (value.castReason() == CastReason::Coerce) {
                            for (LayoutID view : m_program.at(value.coercedType()).views)
                                add(m_coercionsToView[view.index()], 1);
                        } else if (value.castReason() != CastReason::Upcast)
                            add(m_testsOfType[value.type.index()], 1);
                        if (value.castReason() == CastReason::As || value.castReason() == CastReason::Coerce) {
                            ValueID origin = value.child(0);
                            while (function.at(origin).opcode == Opcode::Cast)
                                origin = function.at(origin).child(0);
                            Opcode yieldedBy = function.at(origin).opcode;
                            if (yieldedBy == Opcode::GetById || yieldedBy == Opcode::GetField || yieldedBy == Opcode::GetByVal)
                                break;
                            for (LayoutID view : m_program.at(value.castReason() == CastReason::Coerce ? value.coercedType() : value.type).views)
                                add(m_castsOfWhatIsNotReadOutOfPlace[view.index()], 1);
                        }
                        break;
                    case Opcode::HasShapeOf:
                        // Otherwise for the one shape of the layout itself.
                        if (!value.immediates[1])
                            add(m_testsOfView[value.layout().index()], 1);
                        break;
                    case Opcode::IsInstanceOfClass:
                        add(m_testsOfClass[value.layout().index()], 1);
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
                        add(m_testsOfType[value.type.index()], 1);
                        break;
                    default:
                        break;
                    }
                }
            }
            return Found { WTF::move(storedThroughViews), WTF::move(madeOften), WTF::move(copied), WTF::move(firstStoredWithValue) };
        }, [&](Found&& found) {
            allStoredThroughViews.appendVector(found.storedThroughViews);
            for (uint32_t layout : found.copied) {
                for (auto& field : layouts[layout].fields) {
                    if (exists(field))
                        m_isInCopiedRecord.quickSet(field.slot.index());
                }
            }
            for (uint32_t layout : found.madeOften)
                m_isMadeOften.quickSet(layout);
            for (uint32_t slot : found.firstStoredWithValue)
                m_isFirstStoredWithValue.quickSet(slot);
        });
        for (auto [slot, type] : allStoredThroughViews)
            holds(slot, type);
    }

    void findSlotsThatMayBeAbsent()
    {
        auto& layouts = m_program.layouts;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                auto& field = layout.fields[k];
                // Only the object's own layout says whether a property of it may be deleted or left out.
                bool mayBeAbsent = field.flags & fieldIsOptional && isMade(layout);
                // A captured variable holds nothing until it is initialized.
                if (layout.kind == LayoutKind::Environment)
                    mayBeAbsent = !layout.isCoroutineFrame && !(field.flags & (fieldIsImmutable | fieldIsNeverSeenUninitialized)) && (!m_numberOfBirths[i] || m_numberOfBirthsInitializing[m_firstField[i] + k] != m_numberOfBirths[i]);
                if (mayBeAbsent && exists(field))
                    m_mayBeAbsent.quickSet(field.slot.index());
            }
        }

        Vector<uint32_t> lastAllocationBearing(FillWith { }, m_mostFields, none);
        for (unsigned i = 0; i < m_program.allocations.size(); ++i) {
            auto& allocation = m_program.allocations[i];
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind) || allocation.born == layout.fields.size())
                continue;
            for (unsigned k = 0; k < allocation.born; ++k)
                lastAllocationBearing[allocation.order[k]] = i;
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                // What is never seen absent is stored to before anybody looks.
                if (lastAllocationBearing[k] != i && exists(layout.fields[k]) && !(layout.fields[k].flags & fieldIsNeverSeenAbsent))
                    m_mayBeAbsent.quickSet(layout.fields[k].slot.index());
            }
        }

        // An object that is turned into one of a specialization comes by the fields its layout has not without anything in them.
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

    // Either: what a slot holds that fields of both types share. To be normalized.
    void include(Type& type, const Type& other)
    {
        uint16_t arrayClass = Builtins::singleton().arrayClass().index;
        if (other.engineClasses.contains(arrayClass)) {
            if (!type.engineClasses.contains(arrayClass))
                type.elements = other.elements;
            else if (type.elements != other.elements)
                type.elements = type.elements && other.elements ? eitherOf(type.elements, other.elements) : TypeID();
        }
        type.primitives |= other.primitives;
        type.integers = type.integers.hull(other.integers);
        type.anyObject |= other.anyObject;
        type.anyFunction |= other.anyFunction;
        type.views.appendVector(other.views);
        type.engineClasses.appendVector(other.engineClasses);
    }

    void normalize(Type& type)
    {
        if (type.anyObject) {
            type.anyFunction = true;
            type.views.clear();
            type.engineClasses.clear();
            type.elements = { };
            return;
        }
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        std::ranges::sort(type.views);
        type.views.shrink(std::unique(type.views.begin(), type.views.end()) - type.views.begin());
        if (type.views.size() < 2)
            return;
        // A view whose members are all members of another one adds nothing. So the type of the widest field is the type of the slot.
        std::ranges::stable_sort(type.views, [&](LayoutID a, LayoutID b) {
            return m_program.at(a).members.size() > m_program.at(b).members.size();
        });
        Vector<LayoutID, 1> kept;
        for (LayoutID view : type.views) {
            bool isCovered = false;
            for (unsigned i = 0; i < kept.size() && !isCovered; ++i)
                isCovered = m_program.isUpcast(view, kept[i]);
            if (!isCovered)
                kept.append(view);
        }
        std::ranges::sort(kept);
        type.views = WTF::move(kept);
    }

    TypeID eitherOf(TypeID a, TypeID b)
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

    // Whether every cell of the type is in a block, where addresses are multiples of 16, so that 32 bits say which it is. A cell above
    // MarkedSpace::largeCutoff is allocated by itself. So is a bigint, and whatever is no cell of the program may be.
    bool isCompressible(const Type& type) const
    {
        if (!type.hasCells() || type.anyObject || type.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::String | Primitive::Symbol))
            return false;
        return std::ranges::all_of(type.views, [&](LayoutID view) {
            return !m_program.at(view).admitsForeignObjects && !m_hasLargeMember.quickGet(view.index());
        });
    }

    // Before anything is placed: a field takes eight bytes at most.
    void findLargeMembers()
    {
        auto& layouts = m_program.layouts;
        m_hasLargeMember.ensureSize(layouts.size());
        auto mayBeLarge = [&](const Layout& layout) {
            return offsetOfFirstField + sizeof(void*) * (layout.fields.size() + 1) > MarkedSpace::largeCutoff;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (mayBeLarge(layouts[i]) || std::ranges::any_of(layouts[i].members, [&](LayoutID member) { return mayBeLarge(m_program.at(member)); }))
                m_hasLargeMember.quickSet(i);
        }
    }

    // An environment that is made when a function at the top level of a module is called: a scope that many functions share and few calls make.
    void findTopLevelActivations()
    {
        auto& layouts = m_program.layouts;
        m_keepsWideReferences.ensureSize(m_program.slots.size());
        unsigned numberOfActivations = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            RELEASE_ASSERT(!layout.mayBeInFrame || m_numberOfBirths[i]);
            if (layout.kind != LayoutKind::Environment || (layout.isCoroutineFrame && !layout.mayBeInFrame) || !m_numberOfBirths[i] || (m_isMadeOften.quickGet(i) && !layout.mayBeInFrame))
                continue;
            ++numberOfActivations;
            for (auto& field : layout.fields) {
                if (exists(field))
                    m_keepsWideReferences.quickSet(field.slot.index());
                if (exists(field) && layout.mayBeInFrame)
                    m_isExpectedInt32.quickClear(field.slot.index());
            }
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (exists(field) && field.flags & fieldKeepsWideReference)
                    m_keepsWideReferences.quickSet(field.slot.index());
            }
        }
        report(numberOfActivations, " activations of top-level functions keep wide references");
    }

    // What is narrow by its type stays as it is: a load of that tests nothing.
    Store storeFor(const Type& type, bool isExpectedInt32, bool keepsWideReferences) const
    {
        Store store = storeOf(type);
        // Neither a cell nor a number.
        if (store == Store::JSValue && type.hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Boolean))
            return Store::OptionalBoolean;
        if (isExpectedInt32 && sizeOf(store) == 8 && type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent))
            return Store::Int32OrElsewhere;
        if (!keepsWideReferences && !Options::useAOTWideReferences() && isCompressible(type)) {
            if (store == Store::Cell)
                return Store::Cell32;
            if (store == Store::JSValue)
                return Store::OptionalCell32;
        }
        return store;
    }

    Store storeOutOfLineFor(uint32_t slot, TypeID type) const
    {
        Store store = storeFor(m_program.at(type), false, m_keepsWideReferences.quickGet(slot));
        return isAbsentAsZero(store) && repOf(store) == Rep::JSValue ? store : Store::JSValue;
    }

    void typeSlots()
    {
        auto& slots = m_program.slots;
        std::ranges::sort(m_otherTypes);
        size_t next = 0;
        unsigned numberThatHoldReferences = 0;
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
            // A number that is never seen absent is kept as it is, whatever is expected of it: a load of it tests nothing. Unless some field
            // says that it is an integer: what reads that would have to convert the number, and test.
            if (m_isNeverSeenAbsent.quickGet(i) && !m_hasIntegerField.quickGet(i) && m_program.at(slot.type).isNumber() && (m_numberOfStoresOfDoubles[i] || m_isInCopiedRecord.quickGet(i)))
                m_isExpectedInt32.quickClear(i);
            slot.store = storeFor(m_program.at(slot.type), m_isExpectedInt32.quickGet(i), m_keepsWideReferences.quickGet(i));
            numberThatHoldReferences += holdsCells(slot.store);
            numberOfNarrowReferences += slot.store == Store::Cell32 || slot.store == Store::OptionalCell32;
        }
        RELEASE_ASSERT(next == m_otherTypes.size());
        report(numberOfNarrowReferences, " of ", numberThatHoldReferences, " slots that hold references take 32 bits");
        report(m_program.layouts.size(), " layouts, ", m_views.size(), " memberships; ", m_firstField.last(), " fields in ", slots.size(), " slots, of which ", m_holdsSeveralTypes.bitCount(), " hold what fields of several types say and ", m_mayBeAbsent.bitCount(), " may be absent; ", m_lackedFields.size(), " fields of views are lacked by some member, ", m_numberOfRequiredFieldsLacked, " of them required");
    }

    struct SlotUse {
        // The allocated layouts of objects that have the slot.
        uint32_t numberOfLayouts { 0 };
        uint32_t firstLayout { 0 };
        // The bytes the smallest of them needs. Placed at or beyond that, the slot makes its objects larger than they have to be.
        uint32_t deadline { UINT32_MAX };
        // The bytes all of them need, added up.
        uint64_t bytesNeededByLayouts { 0 };
    };

    struct Occupancy {
        // A bit per byte of the object.
        Vector<uint64_t, 2> words;
        // By the binary logarithm of a size: no word before this one has room for a field of that size.
        std::array<uint32_t, 4> firstWordWithRoom { };
    };

    // The bytes of a word of Occupancy at which an aligned field has room.
    static uint64_t roomIn(uint64_t occupied, unsigned logSize)
    {
        static constexpr uint64_t aligned[] = { UINT64_MAX, 0x5555555555555555ull, 0x1111111111111111ull, 0x0101010101010101ull };
        uint64_t room = ~occupied;
        for (unsigned i = 0; i < logSize; ++i)
            room &= room >> (1u << i);
        return room & aligned[logSize];
    }

    // Of two orders the one by which an object of each layout takes fewer bytes in all.
    void placeObjects()
    {
        auto prepared = prepareToPlaceObjects();
        unsigned best = 0;
        uint64_t leastBytes = placeObjectsInOrder(prepared, 0);
        for (unsigned timesAsManyLayouts : { 2u, 4u }) {
            uint64_t bytes = placeObjectsInOrder(prepared, timesAsManyLayouts);
            if (bytes > leastBytes)
                continue;
            leastBytes = bytes;
            best = timesAsManyLayouts;
        }
        if (best != 4)
            placeObjectsInOrder(prepared, best);
    }

    struct ObjectsToPlace {
        Vector<SlotUse> uses;
        Vector<Occupancy> occupancies;
        Vector<Occupancy> extensions;
        Vector<uint32_t> layoutsOfSlots;
        Vector<uint32_t> order;
        BitVector mayBeOutOfLine;
        BitVector comesAboutByTransitionAlone;
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

    void takeBackWhatWasPutOutOfLine()
    {
        auto& slots = m_program.slots;
        for (auto [index, store, type] : std::exchange(m_slotsPutOutOfLine, { })) {
            slots[index].isOutOfLine = false;
            slots[index].store = store;
            slots[index].type = type;
        }
    }

    // Slots are placed greedily at the lowest offset free in every layout that has them, those first that most layouts have: placed far out, they make all of these large.
    // smallLayoutsFirst: that holds between slots that share a layout where the one is in twice as many layouts as the other. Of the rest, the slot first whose layouts are small: a small layout comes out
    // without holes, and a large one takes them, where they weigh less.
    ObjectsToPlace prepareToPlaceObjects()
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;

        // What an earlier run has put out of line is as typeSlots() left it.
        takeBackWhatWasPutOutOfLine();

        Vector<SlotUse> uses(slots.size());
        Vector<Occupancy> occupancies(layouts.size());
        uint32_t numberOfUses = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!hasObjects(layout))
                continue;
            occupancies[i].words.append((1ull << (offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0))) - 1);
            uint32_t needed = bytesNeededBy(layout);
            for (auto& field : layout.fields) {
                if (!exists(field))
                    continue;
                auto& use = uses[field.slot.index()];
                ++use.numberOfLayouts;
                use.deadline = std::min(use.deadline, needed);
                use.bytesNeededByLayouts += needed;
                ++numberOfUses;
            }
        }
        Vector<uint32_t> layoutsOfSlots(numberOfUses);
        uint32_t next = 0;
        for (auto& use : uses)
            use.firstLayout = std::exchange(next, next + use.numberOfLayouts);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!hasObjects(layouts[i]))
                continue;
            for (auto& field : layouts[i].fields) {
                if (exists(field))
                    layoutsOfSlots[uses[field.slot.index()].firstLayout++] = i;
            }
        }
        for (auto& use : uses)
            use.firstLayout -= use.numberOfLayouts;

        // A slot no object has is never accessed: no object passes for one of a view without members.
        Vector<uint32_t> order;
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (uses[i].numberOfLayouts)
                order.append(i);
        }
        // A slot may go out of line if every layout that has it has an extension, and it is neither born at an allocation nor a discriminant.
        BitVector mayBeOutOfLine;
        mayBeOutOfLine.ensureSize(slots.size());
        for (uint32_t index : order) {
            auto layoutsOfSlot = layoutsOfSlots.span().subspan(uses[index].firstLayout, uses[index].numberOfLayouts);
            if (!slots[index].isDiscriminant && std::ranges::all_of(layoutsOfSlot, [&](uint32_t layout) { return layouts[layout].hasExtension; }))
                mayBeOutOfLine.quickSet(index);
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (isObjectLayout(layout.kind) && exists(field) && field.flags & fieldIsImmutable)
                    mayBeOutOfLine.quickClear(field.slot.index());
            }
        }
        BitVector mayBeNil;
        mayBeNil.ensureSize(slots.size());
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (hasObjects(layout) && exists(field) && (field.flags & fieldIsOptional || !(field.flags & fieldDoesNotAdmitUndefined)))
                    mayBeNil.quickSet(field.slot.index());
            }
        }
        BitVector comesAboutByTransitionAlone;
        comesAboutByTransitionAlone.ensureSize(slots.size());
        for (uint32_t index : order) {
            auto layoutsOfSlot = layoutsOfSlots.span().subspan(uses[index].firstLayout, uses[index].numberOfLayouts);
            if (std::ranges::any_of(layoutsOfSlot, [&](uint32_t layout) { return m_isBornOrNamedByCast.quickGet(layout); }))
                continue;
            comesAboutByTransitionAlone.quickSet(index);
            mayBeNil.quickSet(index);
        }
        mayBeOutOfLine.filter(mayBeNil);
        for (auto& layout : layouts) {
            if (layout.kind != LayoutKind::Instance || !hasObjects(layout))
                continue;
            for (auto& field : layout.fields) {
                if (exists(field) && !(field.flags & fieldIsAbsentAtBirth) && !(m_program.at(slots[field.slot.index()].type).primitives & Primitive::Undefined))
                    mayBeOutOfLine.quickClear(field.slot.index());
            }
        }
        m_existsFromBirth.ensureSize(slots.size());
        for (auto& allocation : m_program.allocations) {
            for (uint16_t field : allocation.order.span().first(allocation.born)) {
                if (auto& born = m_program.at(allocation.layout).fields[field]; exists(born)) {
                    m_existsFromBirth.quickSet(born.slot.index());
                    if (allocation.born > maxFieldsInOrder)
                        m_isFirstStoredWithValue.quickSet(born.slot.index());
                }
            }
        }
        for (auto& initial : m_initialStores) {
            for (unsigned k = 0; k < initial.count; ++k) {
                uint32_t slot = m_program.at(initial.view).fields[*fieldOf(initial, k)].slot.index();
                m_existsFromBirth.quickSet(slot);
                if (initial.count > maxFieldsInOrder)
                    m_isFirstStoredWithValue.quickSet(slot);
            }
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (layout.kind == LayoutKind::Instance && hasObjects(layout) && exists(field) && !(field.flags & fieldIsAbsentAtBirth))
                    m_existsFromBirth.quickSet(field.slot.index());
            }
        }
        for (size_t slot : m_existsFromBirth) {
            if (m_isFirstStoredWithValue.quickGet(slot))
                mayBeOutOfLine.quickClear(slot);
        }
        // What every object of some layout gets where it is made is inline too. Of the rest, what some get there comes first, then what most
        // code gets at.
        BitVector mayBeSetAtCreation;
        mayBeSetAtCreation.ensureSize(slots.size());
        BitVector isSetAtCreation;
        isSetAtCreation.ensureSize(slots.size());
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!hasObjects(layout) || !exists(field))
                    continue;
                if (field.flags & fieldIsSetAtCreation)
                    isSetAtCreation.quickSet(field.slot.index());
                if (field.flags & fieldMayBeSetAtCreation)
                    mayBeSetAtCreation.quickSet(field.slot.index());
            }
        }
        auto scoreOf = [&](uint32_t slot) {
            return std::tuple(isSetAtCreation.quickGet(slot), isSetAtCreation.quickGet(slot) ? uses[slot].numberOfLayouts : 0u, mayBeSetAtCreation.quickGet(slot), m_accessWeight[slot]);
        };
        Vector<Occupancy> extensions(layouts.size());
        for (auto& extension : extensions)
            extension.words.append((1ull << offsetOfFirstExtensionSlot) - 1);
        struct WhatTheOrderGoesBy {
            decltype(scoreOf(0)) score;
            uint32_t numberOfLayouts;
            uint32_t deadline;
            uint32_t slot;
            uint8_t size;
            bool mayBeOutOfLine;
            bool comesAboutByTransitionAlone;
        };
        Vector<WhatTheOrderGoesBy> ordered(order.size(), [&](size_t index) {
            uint32_t slot = order[index];
            return WhatTheOrderGoesBy { scoreOf(slot), uses[slot].numberOfLayouts, uses[slot].deadline, slot, static_cast<uint8_t>(sizeOf(slots[slot].store)), mayBeOutOfLine.quickGet(slot), mayBeOutOfLine.quickGet(slot) && comesAboutByTransitionAlone.quickGet(slot) };
        });
        std::ranges::sort(ordered, [](const WhatTheOrderGoesBy& a, const WhatTheOrderGoesBy& b) {
            if (a.mayBeOutOfLine != b.mayBeOutOfLine)
                return !a.mayBeOutOfLine;
            if (a.comesAboutByTransitionAlone != b.comesAboutByTransitionAlone)
                return a.comesAboutByTransitionAlone;
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
        return { WTF::move(uses), WTF::move(occupancies), WTF::move(extensions), WTF::move(layoutsOfSlots), WTF::move(order), WTF::move(mayBeOutOfLine), WTF::move(comesAboutByTransitionAlone), numberOfUses };
    }

    uint64_t placeObjectsInOrder(const ObjectsToPlace& prepared, unsigned smallLayoutsFirst)
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;
        auto& uses = prepared.uses;
        auto& layoutsOfSlots = prepared.layoutsOfSlots;
        auto& mayBeOutOfLine = prepared.mayBeOutOfLine;
        uint32_t numberOfUses = prepared.numberOfUses;
        Vector<Occupancy> occupancies = prepared.occupancies;
        Vector<Occupancy> extensions = prepared.extensions;
        Vector<uint32_t> order = prepared.order;
        takeBackWhatWasPutOutOfLine();
        for (auto& slot : slots)
            slot.offset = offsetOfFirstField;
        if (smallLayoutsFirst) {
            size_t numberInLine = std::ranges::partition_point(order, [&](uint32_t slot) {
                return !mayBeOutOfLine.quickGet(slot);
            }) - order.begin();
            Vector<Vector<uint32_t>> waiting(slots.size());
            Vector<uint32_t> slotsOfLayout;
            for (auto& layout : layouts) {
                if (!hasObjects(layout))
                    continue;
                slotsOfLayout.shrink(0);
                for (auto& field : layout.fields) {
                    if (exists(field) && !mayBeOutOfLine.quickGet(field.slot.index()))
                        slotsOfLayout.append(field.slot.index());
                }
                std::ranges::sort(slotsOfLayout, [&](uint32_t a, uint32_t b) {
                    return uses[a].numberOfLayouts > uses[b].numberOfLayouts;
                });
                for (size_t later = 1; later < slotsOfLayout.size(); ++later) {
                    for (size_t earlier = 0; earlier < later && uses[slotsOfLayout[earlier]].numberOfLayouts >= smallLayoutsFirst * uses[slotsOfLayout[later]].numberOfLayouts; ++earlier)
                        waiting[slotsOfLayout[earlier]].append(slotsOfLayout[later]);
                }
            }
            Vector<uint32_t> numberWaitedFor(FillWith { }, slots.size(), 0);
            for (auto& list : waiting) {
                std::ranges::sort(list);
                list.shrink(std::ranges::unique(list).begin() - list.begin());
                for (uint32_t slot : list)
                    ++numberWaitedFor[slot];
            }
            // A heap has what compares greatest on top.
            auto comesLater = [&](uint32_t a, uint32_t b) {
                uint64_t left = uses[a].bytesNeededByLayouts * uses[b].numberOfLayouts;
                uint64_t right = uses[b].bytesNeededByLayouts * uses[a].numberOfLayouts;
                if (left != right)
                    return left > right;
                if (sizeOf(slots[a].store) != sizeOf(slots[b].store))
                    return sizeOf(slots[a].store) < sizeOf(slots[b].store);
                return a > b;
            };
            Vector<uint32_t> ready;
            for (uint32_t slot : order.span().first(numberInLine)) {
                if (!numberWaitedFor[slot])
                    ready.append(slot);
            }
            std::ranges::make_heap(ready, comesLater);
            size_t next = 0;
            while (!ready.isEmpty()) {
                std::ranges::pop_heap(ready, comesLater);
                uint32_t slot = ready.takeLast();
                order[next++] = slot;
                for (uint32_t other : waiting[slot]) {
                    if (--numberWaitedFor[other])
                        continue;
                    ready.append(other);
                    std::ranges::push_heap(ready, comesLater);
                }
            }
            RELEASE_ASSERT(next == numberInLine);
        }

        for (uint32_t index : order) {
            auto& slot = slots[index];
            unsigned size = sizeOf(slot.store);
            unsigned logSize = WTF::ctz(size);
            auto layoutsOfSlot = layoutsOfSlots.span().subspan(uses[index].firstLayout, uses[index].numberOfLayouts);
            uint32_t word = 0;
            for (uint32_t layout : layoutsOfSlot) {
                auto& occupancy = occupancies[layout];
                uint32_t& first = occupancy.firstWordWithRoom[logSize];
                while (first < occupancy.words.size() && !roomIn(occupancy.words[first], logSize))
                    ++first;
                word = std::max(word, first);
            }
            uint32_t cap = UINT32_MAX;
            for (uint32_t layout : layoutsOfSlot)
                cap = std::min(cap, m_inlineCap[layout]);
            uint64_t room = 0;
            auto* space = &occupancies;
            for (;; ++word) {
                uint64_t occupied = 0;
                for (uint32_t layout : layoutsOfSlot) {
                    auto& words = (*space)[layout].words;
                    if (word < words.size())
                        occupied |= words[word];
                }
                room = roomIn(occupied, logSize);
                if (!room)
                    continue;
                if (slot.isOutOfLine || !mayBeOutOfLine.quickGet(index) || (!prepared.comesAboutByTransitionAlone.quickGet(index) && word * 64 + WTF::ctz(room) + size <= cap))
                    break;
                m_slotsPutOutOfLine.append({ index, slot.store, slot.type });
                slot.isOutOfLine = true;
                slot.type = withAbsent(slot.type);
                slot.store = storeOutOfLineFor(index, slot.type);
                size = sizeOf(slot.store);
                logSize = WTF::ctz(size);
                space = &extensions;
                word = static_cast<uint32_t>(-1);
            }
            unsigned bit = WTF::ctz(room);
            slot.offset = word * 64 + bit;
            for (uint32_t layout : layoutsOfSlot) {
                auto& words = (*space)[layout].words;
                while (words.size() <= word)
                    words.append(0);
                words[word] |= ((1ull << size) - 1) << bit;
            }
        }

        uint64_t numberOfLayouts = 0;
        uint64_t bytes = 0;
        uint64_t bytesOfHoles = 0;
        uint64_t numberOfLayoutsWithHoles = 0;
        uint32_t mostBytesOfHoles = 0;
        const Layout* layoutWithMostHoles = nullptr;
        for (auto& layout : layouts) {
            if (!isObjectLayout(layout.kind))
                continue;
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
                field.flags = slot.isOutOfLine && m_existsFromBirth.quickGet(field.slot.index()) ? field.flags | fieldMayExistWithoutStorage : field.flags & ~fieldMayExistWithoutStorage;
                (slot.isOutOfLine ? layout.extensionSize : end) = std::max(slot.isOutOfLine ? layout.extensionSize : end, slot.offset + sizeOf(slot.store));
            }
            if (!layout.isAllocated)
                continue;
            layout.size = WTF::roundUpToMultipleOf<16>(end);
            // In cells of 16 bytes, which is what an object takes.
            uint32_t holes = layout.size - WTF::roundUpToMultipleOf<16>(bytesNeededBy(layout));
            ++numberOfLayouts;
            bytes += layout.size;
            bytesOfHoles += holes;
            numberOfLayoutsWithHoles += !!holes;
            if (holes > mostBytesOfHoles) {
                mostBytesOfHoles = holes;
                layoutWithMostHoles = &layout;
            }
        }
        report(numberOfLayouts, " allocated layouts of objects with ", numberOfUses, " fields in ", order.size(), " slots; an object of each takes ", bytes, " bytes in all, of which ", bytesOfHoles, " are lost to holes in ", numberOfLayoutsWithHoles, " layouts");
        if (layoutWithMostHoles)
            report("most is lost in ", nameOf(*layoutWithMostHoles), ": ", mostBytesOfHoles, " of ", layoutWithMostHoles->size, " bytes");
        return bytes;
    }

    // Which layouts have an extension, and how many bytes of their fields are inline. One answer for all layouts that transitions lead
    // between, because a transition neither adds nor removes the pointer. A family with a discriminant has none: its objects are
    // allocated at their final size. Then the slots are allocated again.
    //
    // How often a field is there that is not set at creation, nothing known here says. So nothing goes by it. Besides what has to be inline
    // there is room for referencesBeyondCreation more than what every object gets at creation, for the slots that come first in the order
    // of placeObjects(). That bounds what an object loses to fields it never gets. What it gets beyond them costs it an extension.
    void capLayouts()
    {
        if (Options::useAOTWideReferences())
            return;
        auto& layouts = m_program.layouts;
        // By group: the cell of its largest layout.
        auto largestCells = [&](const Invocable<uint32_t(const Layout&)> auto& bytesOf) {
            Vector<uint32_t> cells(FillWith { }, layouts.size(), 0);
            for (unsigned i = 0; i < layouts.size(); ++i) {
                if (mayHaveExtension(i))
                    cells[m_group[i]] = std::max<uint32_t>(cells[m_group[i]], cellSizeFor(bytesOf(layouts[i])));
            }
            return cells;
        };
        auto bytesAllocated = [](const Layout& layout) {
            return layout.sizeWithRoomToSpecialize;
        };
        auto placeWith = [&](const Invocable<uint32_t(uint32_t group)> auto& capOf) {
            for (unsigned i = 0; i < layouts.size(); ++i) {
                m_inlineCap[i] = mayHaveExtension(i) ? capOf(m_group[i]) : noCap;
                layouts[i].hasExtension = m_inlineCap[i] != noCap;
            }
            placeObjects();
            makeRoomToSpecialize();
        };

        static constexpr uint32_t endOfExtensionPointer = offsetOfExtension + sizeOfExtensionWord;
        // As yet everything is inline.
        auto withEverything = largestCells(bytesAllocated);
        auto atCreation = largestCells([&](const Layout& layout) {
            uint32_t bytes = endOfExtensionPointer;
            for (auto& field : layout.fields) {
                if (exists(field) && field.flags & fieldIsSetAtCreation && m_isBornOrNamedByCast.quickGet(&layout - &layouts[0]) && (m_isFirstStoredWithValue.quickGet(field.slot.index()) || !m_existsFromBirth.quickGet(field.slot.index())))
                    bytes += sizeOf(field.store);
            }
            return bytes;
        });
        placeWith([](uint32_t) {
            return endOfExtensionPointer;
        });
        auto withTheLeast = largestCells(bytesAllocated);
        Vector<uint32_t> endOfWhatHasToBeInLine(FillWith { }, layouts.size(), 0);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!mayHaveExtension(i))
                continue;
            for (auto& field : layouts[i].fields) {
                if (exists(field) && !field.isOutOfLine)
                    endOfWhatHasToBeInLine[m_group[i]] = std::max<uint32_t>(endOfWhatHasToBeInLine[m_group[i]], field.offset + sizeOf(field.store));
            }
        }

        uint32_t bytesBeyondCreation = referencesBeyondCreation * sizeOf(Store::Cell32);
        Vector<uint32_t> caps(FillWith { }, layouts.size(), noCap);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            uint32_t cap = std::max<uint32_t>(withTheLeast[i], std::min<uint32_t>(cellSizeFor(std::max(atCreation[i], endOfWhatHasToBeInLine[i]) + bytesBeyondCreation), mostRoomInLine));
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
        makeRoomToSpecialize();

        unsigned numberOfLayouts = 0;
        for (auto& layout : layouts)
            numberOfLayouts += layout.hasExtension;
        report(numberOfLayouts, " layouts have an extension; ", m_slotsPutOutOfLine.size(), " slots are out of line");
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

    // It has neither of the two words, and nothing looks at it through a view.
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
    }

    // What has been turned into one of a specialization may be turned into one of a specialization of that.
    void widenNumbersWhereBytesAreFree()
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;
        Vector<uint32_t> candidates;
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (slots[i].store == Store::Int32OrElsewhere && !slots[i].isOutOfLine)
                candidates.append(i);
        }
        if (candidates.isEmpty())
            return;

        auto bytesAt = [](uint32_t offset, uint32_t size) {
            return ((1ull << size) - 1) << (offset % 64);
        };
        Vector<Vector<uint64_t, 4>> occupied(layouts.size());
        Vector<std::pair<uint32_t, uint32_t>> layoutsOfSlots;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!isObjectLayout(layout.kind) || !layout.isAllocated)
                continue;
            auto& words = occupied[i];
            words.fill(0, layout.size / 64 + 1);
            words[0] = bytesAt(0, offsetOfFirstField + (layout.hasExtension ? sizeOfExtensionWord : 0));
            for (auto& field : layout.fields) {
                if (!exists(field) || field.isOutOfLine)
                    continue;
                words[field.offset / 64] |= bytesAt(field.offset, sizeOf(field.store));
                if (field.store == Store::Int32OrElsewhere)
                    layoutsOfSlots.append({ field.slot.index(), i });
            }
        }
        std::ranges::sort(layoutsOfSlots);

        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> groupOfName;
        Vector<uint32_t> groupOfSlot;
        groupOfSlot.fill(UINT32_MAX, slots.size());
        Vector<Vector<uint32_t, 1>> groups;
        BitVector isHeldBack;
        for (auto& layout : layouts) {
            if (layout.kind != LayoutKind::Record)
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field) || field.store != Store::Int32OrElsewhere)
                    continue;
                uint32_t group = groupOfName.ensure(field.name.index(), [&] {
                    groups.append({ });
                    return groups.size() - 1;
                }).iterator->value;
                if (field.isOutOfLine)
                    isHeldBack.set(group);
                uint32_t& groupOfThis = groupOfSlot[field.slot.index()];
                if (groupOfThis == UINT32_MAX) {
                    groupOfThis = group;
                    groups[group].append(field.slot.index());
                } else if (groupOfThis != group) {
                    isHeldBack.set(group);
                    isHeldBack.set(groupOfThis);
                }
            }
        }
        for (uint32_t index : candidates) {
            if (groupOfSlot[index] != UINT32_MAX)
                continue;
            groupOfSlot[index] = groups.size();
            groups.append({ index });
        }
        Vector<uint32_t> order;
        Vector<uint64_t> weightOfGroup;
        weightOfGroup.fill(0, groups.size());
        for (uint32_t group = 0; group < groups.size(); ++group) {
            if (isHeldBack.get(group))
                continue;
            order.append(group);
            for (uint32_t index : groups[group])
                weightOfGroup[group] = std::max<uint64_t>(weightOfGroup[group], m_accessWeight[index]);
        }
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            return std::tuple(weightOfGroup[b], groups[a][0]) < std::tuple(weightOfGroup[a], groups[b][0]);
        });

        uint32_t narrow = sizeOf(Store::Int32OrElsewhere);
        uint32_t wide = sizeOf(Store::Double);
        auto layoutsOf = [&](uint32_t index) {
            return std::ranges::equal_range(layoutsOfSlots, index, { }, &std::pair<uint32_t, uint32_t>::first);
        };
        auto move = [&](uint32_t index, uint32_t from, uint32_t sizeThere, uint32_t to, uint32_t sizeHere) {
            for (auto& use : layoutsOf(index)) {
                occupied[use.second][from / 64] &= ~bytesAt(from, sizeThere);
                occupied[use.second][to / 64] |= bytesAt(to, sizeHere);
            }
        };
        auto roomFor = [&](uint32_t index) -> std::optional<uint32_t> {
            auto& slot = slots[index];
            auto layoutsOfSlot = layoutsOf(index);
            uint32_t end = UINT32_MAX;
            for (auto& use : layoutsOfSlot)
                end = std::min({ end, layouts[use.second].size, m_inlineCap[use.second] });
            for (uint32_t offset = WTF::roundUpToMultipleOf(wide, offsetOfFirstField); offset + wide <= end; offset += wide) {
                bool isFree = std::ranges::all_of(layoutsOfSlot, [&](auto& use) {
                    uint64_t word = occupied[use.second][offset / 64];
                    if (slot.offset / 64 == offset / 64)
                        word &= ~bytesAt(slot.offset, narrow);
                    return !(word & bytesAt(offset, wide));
                });
                if (isFree)
                    return offset;
            }
            return std::nullopt;
        };
        unsigned numberWidened = 0;
        BitVector isWidened;
        for (bool didWiden = true; didWiden;) {
            didWiden = false;
            for (uint32_t group : order) {
                if (isWidened.get(group))
                    continue;
                Vector<std::pair<uint32_t, uint32_t>, 4> found;
                bool hasRoom = true;
                for (uint32_t index : groups[group]) {
                    if (layoutsOf(index).empty())
                        continue;
                    auto offset = roomFor(index);
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
                    continue;
                }
                for (auto [index, offset] : found) {
                    auto& slot = slots[index];
                    m_isExpectedInt32.quickClear(index);
                    slot.store = storeFor(m_program.at(slot.type), false, m_keepsWideReferences.quickGet(index));
                    RELEASE_ASSERT(sizeOf(slot.store) == wide);
                    slot.offset = offset;
                    ++numberWidened;
                }
                isWidened.set(group);
                didWiden = true;
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
        report(candidates.size() - numberWidened, " of ", candidates.size(), " slots of numbers that are expected to be int32s take 32 bits: the others have found 64 that were free");
    }

    void makeRoomToSpecialize()
    {
        Vector<Layout*> specialized;
        for (auto& layout : m_program.layouts) {
            layout.sizeWithRoomToSpecialize = layout.size;
            if (!layout.specializations.isEmpty())
                specialized.append(&layout);
        }
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (Layout* layout : specialized) {
                for (LayoutID specialization : layout->specializations) {
                    uint32_t room = m_program.at(specialization).sizeWithRoomToSpecialize;
                    if (room <= layout->sizeWithRoomToSpecialize)
                        continue;
                    layout->sizeWithRoomToSpecialize = room;
                    didChange = true;
                }
            }
        }
    }

    // Transitive closure of shape transitions, the layout itself first.
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

    // Set if the layout lists exactly one constant for the field, an integer.
    std::optional<int32_t> constantOf(uint32_t layout, StringID name) const
    {
        auto constants = constantsOf(layout, name);
        return constants.size() == 1 ? integerOf(constants[0]) : std::nullopt;
    }

    // The transition closure of an allocation-site layout whose objects have an integer discriminant that never changes after allocation.
    struct Family {
        uint32_t base;
        StringID discriminant;
        int32_t first;
        // By constant - first. None: no final layout, the object is allocated in the base layout.
        Vector<LayoutID> finalLayouts;
        // Allocation size in the base layout, by constant - first, and one more for any other integer. Filled by sizeAllocations().
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

    // Marks the layouts of each family, prunes transitions that would change the discriminant, and picks the final layout per constant: the
    // one single-constant layout, or the least of several (all others reachable from it).
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
        // Nothing stores the discriminant after allocation, so an object allocated without one never gets one.
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
                    // A cast to any other layout of the constant finds the object to be a member, or makes it one.
                    if (!std::ranges::all_of(candidates[k], [&](uint32_t other) { return seen.get(other) || std::ranges::binary_search(viewsOf(candidate), other); }))
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

    // One allocation per constant behind each allocation that has a discriminant, and one for any other integer. See Allocation::numberOfLiterals.
    void expandAllocations()
    {
        auto& allocations = m_program.allocations;
        for (unsigned i = 0, end = allocations.size(); i < end; ++i) {
            Family* family = familyOf(allocations[i]);
            if (!family || m_program.layouts[family->base].specializations.isEmpty())
                continue;
            allocations[i].firstLiteral = family->first;
            allocations[i].numberOfLiterals = family->finalLayouts.size();
            allocations[i].allocationOfFirstLiteral = allocations.size();
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

    // Room an object needs if it stays in the base layout at allocation. A layout counts for the constants it lists. One that lists none is
    // an abstract supertype: it counts for the constants of its subtypes, which have its fields at the same offsets.
    void sizeBaseAllocations(Family& family)
    {
        auto& layouts = m_program.layouts;
        BitVector seen;
        Vector<uint32_t> closure = transitionClosure(family.base, seen);
        unsigned count = family.finalLayouts.size();
        family.baseSizes.fill(layouts[family.base].size, count + 1);
        uint32_t sizeOfAbstract = layouts[family.base].size;
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
            sizeOfAbstract = std::max(sizeOfAbstract, layouts[layout].size);
            for (LayoutID subtype : layouts[layout].members)
                countsFor(layout, constantsOf(subtype.index(), family.discriminant), false);
        }
        for (unsigned k = 0; k <= count; ++k) {
            if (!isListed.get(k))
                family.baseSizes[k] = sizeOfAbstract;
        }
    }

    void sizeAllocations()
    {
        auto& allocations = m_program.allocations;
        for (auto& allocation : allocations) {
            allocation.size = m_program.at(allocation.layout).sizeWithRoomToSpecialize;
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
                auto& entry = allocations[allocations[i].allocationOfFirstLiteral + k];
                if (entry.layout == allocations[i].layout)
                    entry.size = family.baseSizes[k];
                sum += entry.size;
            }
            report("allocation ", i, " of ", nameOf(m_program.at(allocations[i].layout)), " by ", nameOf(family.discriminant), ": mean size ", sum / (allocations[i].numberOfLiterals + 1), ", ", family.baseSizes.last(), " for any other integer, ", allocations[i].size, " if it is no integer");
        }
    }

    // An object goes by the first so many entries of its order. So an order stands in for every order it starts with.
    void collectOrders()
    {
        // The order in an order word is a number among those of the object's own layout. Functions that may be handed objects of one layout
        // get different numbers, and each layout has the order of a function at the number of the function. So the order word that a
        // function leaves does not depend on the layout.
        Vector<BitVector> numbersTaken(m_initialStores.isEmpty() ? 0 : m_program.layouts.size());
        for (auto& initial : m_initialStores) {
            if (!initial.count)
                continue;
            BitVector taken;
            forEachLayoutInitializedBy(initial, [&](uint32_t layout) {
                taken.merge(numbersTaken[layout]);
            });
            uint32_t number = 0;
            while (taken.get(number))
                ++number;
            forEachLayoutInitializedBy(initial, [&](uint32_t index) {
                auto& layout = m_program.layouts[index];
                numbersTaken[index].set(number);
                if (layout.orders.size() <= number)
                    layout.orders.grow(number + 1);
                for (unsigned k = 0; k < initial.count; ++k)
                    layout.orders[number].append(static_cast<uint16_t>(*layout.fieldNamed(initial.names[k])));
                RELEASE_ASSERT(layout.orders.size() < keysAreInLayoutOrder);
            });
            RELEASE_ASSERT(initial.count <= maxFieldsInOrder);
            m_program.functions[initial.function].numberOfInitialStores = initial.count;
            m_program.functions[initial.function].orderWordAfterInitialStores = orderWordOf(number, initial.count);
            report(nameOf(m_program.functions[initial.function].name), " stores ", initial.count, " of its first ", initial.names.size(), " properties without a test, to a fresh ", nameOf(m_program.at(initial.view)), "; order ", number);
        }
        for (auto& allocation : m_program.allocations) {
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind))
                continue;
            auto [expected, born] = orderOfWhatExists(layout, allocation);
            if (born > maxFieldsInOrder)
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
    }

    // The layouts whose shapes compiled code tests for together.
    struct Candidate {
        enum Kind : uint8_t {
            // The lowering puts the test together from those of the views, and merges their ranges if they are adjacent.
            MembersOfViewsOfType,
            MembersOfView,
            MembersWithField,
            InstancesOfClass,
            RetagSources,
            MembersKeptByCast,
        };

        Kind kind;
        // The type, the layout, or the number among m_setsOfMembers.
        uint32_t index;
        Fingerprint layouts;
        uint64_t weight;
        uint32_t test { none };
    };

    struct Test {
        uint32_t firstLayout;
        uint32_t numberOfLayouts;
        uint64_t weight { 0 };
        bool hasResult { false };
        ShapeTest result { };
    };

    // A test is one comparison if its shapes are consecutive numbers. Tests get that in the order of their weight, unless those before rule it out.
    void numberShapes()
    {
        auto& layouts = m_program.layouts;
        Vector<Candidate> candidates;
        auto addCandidate = [&](Candidate::Kind kind, uint32_t index, uint64_t weight, const auto& listed, const auto& indexOf) {
            Fingerprint fingerprint;
            for (auto layout : listed)
                fingerprint.add(m_hashOfLayout[indexOf(layout)]);
            candidates.append(Candidate { kind, index, fingerprint, weight });
        };
        auto indexOfID = [](LayoutID layout) {
            return layout.index();
        };
        Vector<LayoutID> membersOfViews;
        Vector<LayoutID> membersOfEarlierViews;
        auto uniteMembersOfViewsOf = [&](uint32_t type) {
            membersOfViews.shrink(0);
            for (LayoutID view : m_program.at(TypeID(type)).views) {
                auto& members = m_program.at(view).members;
                std::swap(membersOfViews, membersOfEarlierViews);
                membersOfViews.resize(membersOfEarlierViews.size() + members.size());
                membersOfViews.shrink(std::ranges::set_union(membersOfEarlierViews, members, membersOfViews.begin()).out - membersOfViews.begin());
            }
        };

        for (unsigned i = 0; i < m_testsOfType.size(); ++i) {
            auto& views = m_program.at(TypeID(i)).views;
            if (!m_testsOfType[i])
                continue;
            for (LayoutID view : views)
                m_testsOfView[view.index()] += m_testsOfType[i];
            if (views.size() < 2)
                continue;
            uniteMembersOfViewsOf(i);
            addCandidate(Candidate::MembersOfViewsOfType, i, m_testsOfType[i], membersOfViews, indexOfID);
        }
        Vector<uint32_t> candidateOfView(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!isObjectLayout(layouts[i].kind))
                continue;
            candidateOfView[i] = candidates.size();
            addCandidate(Candidate::MembersOfView, i, m_testsOfView[i], layouts[i].members, indexOfID);
        }
        uint32_t firstCandidateOfSets = candidates.size();
        for (unsigned i = 0; i < m_setsOfMembers.size(); ++i) {
            auto& lacked = m_lackedFields[m_setsOfMembers[i].listedFor];
            candidates.append(Candidate { Candidate::MembersWithField, i, m_membersWith[m_firstField[lacked.layout] + lacked.field], 0 });
        }
        for (auto& lacked : m_lackedFields)
            candidates[firstCandidateOfSets + lacked.membersWithIt].weight += lacked.numberOfAccesses;
        Vector<Vector<uint32_t>> instancesOfClass(layouts.size());
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].kind != LayoutKind::Instance || !layouts[i].isAllocated)
                continue;
            for (LayoutID ancestor(i); ancestor; ancestor = m_program.at(ancestor).base)
                instancesOfClass[ancestor.index()].append(i);
        }
        Vector<uint32_t> candidateOfClass(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].kind != LayoutKind::Instance)
                continue;
            candidateOfClass[i] = candidates.size();
            addCandidate(Candidate::InstancesOfClass, i, m_testsOfClass[i], instancesOfClass[i], std::identity { });
        }
        Vector<uint32_t> candidateOfRetagSources(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (retagSourcesOf(i).empty())
                continue;
            candidateOfRetagSources[i] = candidates.size();
            addCandidate(Candidate::RetagSources, i, m_coercionsToView[i], retagSourcesOf(i), std::identity { });
        }
        Vector<LayoutID> membersKept;
        auto findMembersKeptByCastTo = [&](uint32_t view) {
            membersKept.resize(layouts[view].members.size());
            membersKept.shrink(std::ranges::set_difference(layouts[view].members, retaggedMembersOf(view), membersKept.begin()).out - membersKept.begin());
        };
        Vector<uint32_t> candidateOfCastTest(FillWith { }, layouts.size(), none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (retaggedMembersOf(i).empty())
                continue;
            candidateOfCastTest[i] = candidates.size();
            findMembersKeptByCastTo(i);
            addCandidate(Candidate::MembersKeptByCast, i, m_testsOfView[i], membersKept, indexOfID);
        }

        // Candidates for the same layouts are one test, and only that is listed.
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
                case Candidate::MembersOfViewsOfType:
                    uniteMembersOfViewsOf(candidate.index);
                    for (LayoutID member : membersOfViews)
                        listedLayouts.append(member.index());
                    break;
                case Candidate::MembersOfView:
                    for (LayoutID member : layouts[candidate.index].members)
                        listedLayouts.append(member.index());
                    break;
                case Candidate::MembersWithField:
                    listedLayouts.append(membersIn(m_setsOfMembers[candidate.index]));
                    break;
                case Candidate::InstancesOfClass:
                    listedLayouts.appendVector(instancesOfClass[candidate.index]);
                    break;
                case Candidate::RetagSources:
                    listedLayouts.append(retagSourcesOf(candidate.index));
                    break;
                case Candidate::MembersKeptByCast:
                    findMembersKeptByCastTo(candidate.index);
                    for (LayoutID member : membersKept)
                        listedLayouts.append(member.index());
                    break;
                }
                RELEASE_ASSERT(listedLayouts.size() - unsortedTests.last().firstLayout == candidate.layouts.count);
            }
            unsortedTests.last().weight += candidate.weight;
            unsortedTests.last().hasResult |= candidate.kind != Candidate::MembersOfViewsOfType;
            candidate.test = unsortedTests.size() - 1;
        }
        // The tests are in the lexicographic order of their layouts. Among tests of one weight and size that decides which comes first.
        Vector<uint32_t> sortedTests(unsortedTests.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
        sortLexicographically(sortedTests.mutableSpan(), [&](uint32_t test, uint32_t position) {
            return position < unsortedTests[test].numberOfLayouts ? listedLayouts[unsortedTests[test].firstLayout + position] + 1 : 0;
        });
        Vector<Test> tests;
        Vector<uint32_t> placeOfTest(unsortedTests.size());
        for (uint32_t test : sortedTests) {
            placeOfTest[test] = tests.size();
            tests.append(unsortedTests[test]);
        }
        report(candidates.size(), " sets of layouts are tested for, ", tests.size(), " of them different, which list ", listedLayouts.size(), " layouts");
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

        // The leaves are numbered as the tests come to them, so a low number means much tested.
        Vector<uint32_t> leafOfLayout(FillWith { }, layouts.size(), none);
        Vector<uint32_t> layoutOfLeaf;
        for (uint32_t test : order) {
            for (uint32_t layout : layoutsOf(tests[test])) {
                if (leafOfLayout[layout] != none)
                    continue;
                leafOfLayout[layout] = layoutOfLeaf.size();
                layoutOfLeaf.append(layout);
            }
        }

        uint32_t nextShape = 1;
        auto number = [&](Layout& layout) {
            if (layout.isAllocated)
                layout.shape = nextShape++;
        };
        if (!layoutOfLeaf.isEmpty()) {
            PQTree tree(layoutOfLeaf.size());
            // Where the tree leaves a choice, what the weightiest test that was turned down is after is kept together.
            Vector<uint64_t> keyOfLeaf(layoutOfLeaf.size(), [](size_t leaf) {
                return static_cast<uint64_t>(none) << 32 | leaf;
            });
            uint64_t numberTurnedDown = 0;
            Vector<uint32_t> leaves;
            for (uint32_t test : order) {
                leaves.shrink(0);
                for (uint32_t layout : layoutsOf(tests[test]))
                    leaves.append(leafOfLayout[layout]);
                if (tree.reduce(leaves.span()))
                    continue;
                for (uint32_t leaf : leaves)
                    keyOfLeaf[leaf] = std::min(keyOfLeaf[leaf], numberTurnedDown << 32 | leaf);
                ++numberTurnedDown;
            }
            for (uint32_t leaf : tree.frontier(keyOfLeaf.span()))
                number(layouts[layoutOfLeaf[leaf]]);
        }
        // Objects have the lowest numbers, which are the ones compared with.
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (isObjectLayout(layouts[i].kind) && leafOfLayout[i] == none)
                number(layouts[i]);
        }
        for (auto& layout : layouts) {
            if (layout.kind == LayoutKind::Environment)
                number(layout);
        }
        number(m_program.at(m_program.closureLayout));
        m_program.numberOfShapes = nextShape - 1;

        uint64_t weight = 0;
        uint64_t weightOfRanges = 0;
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
            // No two layouts have one shape.
            if (last - first + 1 == test.numberOfLayouts) {
                test.result = ShapeTest { first, test.numberOfLayouts, none };
                if (test.numberOfLayouts > 1) {
                    ++numberOfRanges;
                    weightOfRanges += test.weight;
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
            return tests[placeOfTest[candidates[candidate].test]].result;
        };
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (candidateOfView[i] != none)
                layouts[i].test = resultOf(candidateOfView[i]);
            if (candidateOfClass[i] != none)
                layouts[i].instances = resultOf(candidateOfClass[i]);
            if (candidateOfRetagSources[i] != none)
                layouts[i].retagSources = resultOf(candidateOfRetagSources[i]);
            layouts[i].castTest = candidateOfCastTest[i] != none ? resultOf(candidateOfCastTest[i]) : layouts[i].test;
        }
        for (auto& lacked : m_lackedFields)
            layouts[lacked.layout].fields[lacked.field].membersWithIt = resultOf(firstCandidateOfSets + lacked.membersWithIt);
        report(m_program.numberOfShapes, " shapes; ", order.size(), " tests for the shapes of several layouts, of weight ", weight, ": ", numberOfRanges, " are a range, of weight ", weightOfRanges, "; ", numberOfSets, " shape sets in ", numberOfBits / 8, " bytes");
    }

    // Grouped by what they hold at first, which the entry function stores: those for which that is all bits clear come last. Among them
    // are those that are never seen uninitialized, whatever they are kept as: the widest first, so that each is aligned.
    void placeVariables()
    {
        auto& variables = m_program.variables;
        auto groupOf = [](const Variable& variable) -> unsigned {
            if (variable.isNeverSeenUninitialized || variable.startsZero)
                return 5 + sizeof(uint64_t) / sizeOf(variable.store);
            switch (variable.store) {
            case Store::JSValue:
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
            if (variable.mayBeSeenUninitialized())
                type.primitives |= Primitive::Absent;
            variable.store = storeOf(type);
            variable.hasCard = true;
        }
        Vector<unsigned> timesStoredLater;
        timesStoredLater.fill(0, variables.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned index) {
            auto& function = m_program.functions[index];
            if ((function.kind == FunctionKind::Module && function.parameterTypes.isEmpty()) || (function.module && m_program.modules[function.module.index()].initializer == FunctionID(index)))
                return;
            for (auto& value : function.values) {
                if (value.opcode == Opcode::PutModuleVariable)
                    WTF::atomicExchangeAdd(&timesStoredLater[value.variable().index()], 1u, std::memory_order_relaxed);
            }
        });
        for (unsigned index = 0; index < variables.size(); ++index) {
            if (timesStoredLater[index])
                variables[index].hasCard = false;
        }
        constexpr unsigned numberOfGroupsThatAreFilled = 5;
        BitVector isModuleOfHost(m_program.modules.size());
        auto addModuleOf = [&](FunctionID function) {
            if (ModuleID module = function ? m_program.at(function).module : ModuleID())
                isModuleOfHost.set(module.index());
        };
        for (auto& module : m_program.builtinModules)
            addModuleOf(module.second);
        for (auto& builtin : m_program.builtinFunctions)
            addModuleOf(builtin.function);
        for (FunctionID function : m_program.preludeFunctions)
            addModuleOf(function);
        BitVector mayRunAheadOfProgram(m_program.functions.size());
        Vector<unsigned> functionsToLookAt;
        auto reach = [&](FunctionID function) {
            if (function && !mayRunAheadOfProgram.set(function.index()))
                functionsToLookAt.append(function.index());
        };
        for (unsigned index = 0; index < m_program.functions.size(); ++index) {
            if (ModuleID module = m_program.functions[index].module; module && isModuleOfHost.get(module.index()))
                reach(FunctionID(index));
        }
        BitVector isFilledAtBirth(variables.size());
        for (unsigned index = 0; index < variables.size(); ++index) {
            if (!variables[index].module || isModuleOfHost.get(variables[index].module.index()))
                isFilledAtBirth.set(index);
        }
        unsigned numberOfBirthsAndReleasesAheadOfProgram = 0;
        unsigned numberOfVariablesFilledAtBirthForThem = 0;
        auto fillAtBirthWhatKeeps = [&](LayoutID layout) {
            if (VariableID reused = layout ? m_program.at(layout).reuseVariable : VariableID()) {
                ++numberOfBirthsAndReleasesAheadOfProgram;
                numberOfVariablesFilledAtBirthForThem += !isFilledAtBirth.set(reused.index());
            }
        };
        while (!functionsToLookAt.isEmpty()) {
            FunctionID id(functionsToLookAt.takeLast());
            auto& function = m_program.at(id);
            for (FunctionID other : { m_program.genericEntrypoints.get(id.index()), function.rampForClosures, function.resumeFunction, function.startFunction, function.worker })
                reach(other);
            if (auto row = m_program.moduleEvaluationOfFunction.find(id.index()); row != m_program.moduleEvaluationOfFunction.end()) {
                auto& evaluation = m_program.moduleEvaluations[row->value];
                isFilledAtBirth.set(evaluation.isEvaluated.index());
                if (evaluation.error)
                    isFilledAtBirth.set(evaluation.error.index());
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
                    reach(m_program.bodyOfConstruction(value));
                    break;
                case Opcode::Cast:
                    reach(m_program.bodyOfCast(value));
                    break;
                case Opcode::GetModuleVariable:
                case Opcode::PutModuleVariable:
                    isFilledAtBirth.set(value.variable().index());
                    break;
                case Opcode::NewObjectWithShape:
                    if (value.allocation() < m_program.allocations.size())
                        fillAtBirthWhatKeeps(m_program.allocations[value.allocation()].layout);
                    [[fallthrough]];
                case Opcode::PutField:
                    fillAtBirthWhatKeeps(value.layout());
                    break;
                default:
                    break;
                }
            }
        }
        auto placeOf = [&](unsigned index) {
            unsigned group = groupOf(variables[index]);
            return group < numberOfGroupsThatAreFilled && !isFilledAtBirth.get(index) ? group : group + numberOfGroupsThatAreFilled;
        };
        Vector<unsigned> order(variables.size(), [](size_t index) {
            return static_cast<unsigned>(index);
        });
        std::ranges::stable_sort(order, [&](unsigned a, unsigned b) {
            return placeOf(a) < placeOf(b);
        });
        uint32_t offset = 0;
        auto place = [&](bool isFilledLate) {
            for (unsigned i : order) {
                if ((placeOf(i) < numberOfGroupsThatAreFilled) != isFilledLate)
                    continue;
                variables[i].offset = offset;
                offset += sizeOf(variables[i].store);
            }
        };
        place(true);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            offset = WTF::roundUpToMultipleOf<16 * KB>(offset);
        m_program.sizeOfStaticsFilledLate = offset;
        place(false);
        report(variables.size(), " variables in ", offset, " bytes");
        report(m_program.sizeOfStaticsFilledLate, " bytes of them are filled when the program is first entered");
        report(numberOfBirthsAndReleasesAheadOfProgram, " births and releases of an environment that is used again stand in code that may run ahead of the program; ", numberOfVariablesFilledAtBirthForThem, " variables are filled at birth for that alone");
        auto& builtins = Builtins::singleton();
        m_program.globalPropertyCachesOffset = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        for (auto& numbers : m_program.globalPropertyCacheNumbers)
            numbers.fill(0, m_program.strings.size());
        m_program.propertyOfGlobalThisCacheNumbers.fill(0, m_program.strings.size());
        Vector<Vector<std::pair<uint8_t, uint32_t>>> freeNamesOfFunctions(m_program.functions.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            auto& function = m_program.functions[i];
            for (auto& value : function.values) {
                if (value.opcode == Opcode::GetById) {
                    if (auto& base = function.at(value.child(0)); base.opcode == Opcode::GetGlobal && m_program.isNameOfGlobalThis(base.string()))
                        freeNamesOfFunctions[i].append({ static_cast<uint8_t>(2), value.string().index() });
                    continue;
                }
                if (value.opcode != Opcode::GetGlobal && value.opcode != Opcode::GetGlobalForTypeOf && value.opcode != Opcode::PutGlobal)
                    continue;
                if (BuiltinName name = m_program.builtinName(value.string()); builtins.isGlobal(name) && !m_program.isExtended(name))
                    continue;
                freeNamesOfFunctions[i].append({ static_cast<uint8_t>(value.opcode == Opcode::PutGlobal), value.string().index() });
            }
        });
        for (auto& freeNames : freeNamesOfFunctions) {
            for (auto [table, name] : freeNames) {
                if (uint32_t& number = table == 2 ? m_program.propertyOfGlobalThisCacheNumbers[name] : m_program.globalPropertyCacheNumbers[table][name]; !number)
                    number = ++m_program.numberOfGlobalPropertyCaches;
            }
        }
        m_program.staticFieldPlacesOffset = m_program.globalPropertyCachesOffset + m_program.numberOfGlobalPropertyCaches * sizeof(GlobalPropertyCache);
        m_program.sizeOfStatics = m_program.staticFieldPlacesOffset + m_program.numberOfStaticFieldPlaces * sizeof(int32_t);
        report(m_program.numberOfGlobalPropertyCaches, " caches of free names");
    }

    // What a NewArray keeps from one evaluation to the next, in the statics behind what came before. All of it is zero when the program starts, and none of it can be seen.
    // Of constants: a JSCellButterfly, as for the engine's NewArrayBuffer: the arrays made there are copy-on-write.
    // Empty: eight bytes, for what ArrayAllocationProfile::vectorLengthHint() is for: see ArraySite.
    // Which sites get one goes by what the function itself does with the array, which decides how fast the program is and nothing else: an array that is stored to right
    // away is better not made copy-on-write, and only of an array that is pushed to here is it known where it was made when it has to grow.
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
            bool isMadeOnce = isShared && m_program.stringsMadeOnce.get(constant.string().index());
            return Data::encodedStringConstant(constant.string().index()) | (isMadeOnce ? Data::encodedStringIsMadeOnce : 0);
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void placeArraySites()
    {
        auto& builtins = Builtins::singleton();
        Vector<Value*> allLiteralsOfConstants;
        Vector<Value*> allLiteralsPushedTo;
        Vector<Value*> allUnsharedLiterals;
        Vector<Vector<uint64_t>> allElementsOfUnsharedLiterals;
        using Literals = std::tuple<Vector<Value*>, Vector<Value*>, Vector<Vector<uint64_t>>, Vector<Value*>, Vector<Vector<uint64_t>>>;
        forEachFunctionInOrder<Literals>([&](Function& function) {
            auto [literalsOfConstants, literalsPushedTo, elementsOfLiterals, unsharedLiterals, elementsOfUnsharedLiterals] = Literals { };
            BitVector isStoredTo;
            BitVector isPushedTo;
            auto originOf = [&](ValueID id) {
                while (function.at(id).opcode == Opcode::Cast)
                    id = function.at(id).child(0);
                return id;
            };
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode == Opcode::PutByVal)
                        isStoredTo.set(originOf(value.child(0)).index());
                    if (value.opcode != Opcode::CallBuiltin)
                        continue;
                    auto& builtin = builtins.at(value.builtin());
                    if (builtin.effects & Effect::WritesArguments) {
                        for (ValueID argument : value.arguments)
                            isStoredTo.set(originOf(argument).index());
                    }
                    if (!builtin.takesReceiver() || !value.child(0))
                        continue;
                    if (builtin.effects & Effect::WritesReceiver)
                        isStoredTo.set(originOf(value.child(0)).index());
                    if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::ArrayPush)
                        isPushedTo.set(originOf(value.child(0)).index());
                }
            }
            // `const result = c ? [] : a.slice(0, i); result.push(x)`: what is pushed to where ways meet is whatever comes along any of them.
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
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    if (value.opcode != Opcode::NewArray)
                        continue;
                    value.immediates = { Format::none, Format::none };
                    if (value.arguments.isEmpty()) {
                        if (isPushedTo.get(id.index()))
                            literalsPushedTo.append(&value);
                        continue;
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
                    bool isFromData = Options::useAOTArrayLiteralsFromData();
                    if (areConstants && (isShared || (isFromData && value.arguments.size() > Program::maxLengthOfArrayLiteralMadeInline))) {
                        (isShared ? literalsOfConstants : unsharedLiterals).append(&value);
                        if (isFromData) {
                            Vector<uint64_t> elements;
                            for (ValueID element : value.arguments)
                                elements.append(encodedConstant(function.at(element), isShared));
                            (isShared ? elementsOfLiterals : elementsOfUnsharedLiterals).append(WTF::move(elements));
                        }
                    }
                }
            }
            return Literals { WTF::move(literalsOfConstants), WTF::move(literalsPushedTo), WTF::move(elementsOfLiterals), WTF::move(unsharedLiterals), WTF::move(elementsOfUnsharedLiterals) };
        }, [&](Literals&& literals) {
            allLiteralsOfConstants.appendVector(std::get<0>(literals));
            allLiteralsPushedTo.appendVector(std::get<1>(literals));
            m_program.elementsOfArrayLiterals.appendVector(WTF::move(std::get<2>(literals)));
            allUnsharedLiterals.appendVector(std::get<3>(literals));
            allElementsOfUnsharedLiterals.appendVector(WTF::move(std::get<4>(literals)));
        }, holdsNewArray);
        auto& literalsOfConstants = allLiteralsOfConstants;
        auto& literalsPushedTo = allLiteralsPushedTo;
        uint32_t offset = WTF::roundUpToMultipleOf<sizeof(JSCell*)>(m_program.sizeOfStatics);
        m_program.copyOnWriteButterfliesOffset = offset;
        m_program.numberOfCopyOnWriteButterflies = literalsOfConstants.size();
        for (Value* literal : literalsOfConstants) {
            literal->immediates[1] = offset;
            offset += sizeof(JSCell*);
        }
        uint32_t numberOfLiteral = literalsOfConstants.size();
        for (Value* literal : allUnsharedLiterals)
            literal->immediates[1] = numberOfLiteral++ << 1 | 1;
        m_program.elementsOfArrayLiterals.appendVector(WTF::move(allElementsOfUnsharedLiterals));
        for (Value* literal : literalsPushedTo) {
            literal->immediates[0] = offset;
            offset += 2 * sizeof(uint32_t);
        }
        m_program.sizeOfStatics = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        report(literalsOfConstants.size(), " array literals of constants, ", literalsPushedTo.size(), " empty ones that are pushed to");
    }

    // A word for each RegExp literal of the program, by source and flags, zero when the program starts: the object that is made the first time a site is come to where nothing can
    // get hold of it, and used again at every such site. Which sites those are the lowering sees, in what it lowers: Lowering::isKeptByNothing().
    void placeRegExpLiterals()
    {
        uint32_t offset = WTF::roundUpToMultipleOf<sizeof(JSCell*)>(m_program.sizeOfStatics);
        m_program.sharedRegExpObjectsOffset = offset;
        forEachFunctionInOrder<Vector<uint64_t>>([&](Function& function) {
            Vector<uint64_t> keys;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    if (auto key = keyOfRegExpLiteral(m_program, function, function.at(id)))
                        keys.append(*key);
                }
            }
            return keys;
        }, [&](Vector<uint64_t>&& keys) {
            for (uint64_t key : keys) {
                if (m_program.offsetsOfSharedRegExpObjects.add(key, offset).isNewEntry)
                    offset += sizeof(JSCell*);
            }
        }, holdsCallBuiltin);
        m_program.sizeOfStatics = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        report(m_program.offsetsOfSharedRegExpObjects.size(), " RegExp literals");
    }

    // On what the lowering and the run time read, however it has come about.
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
            RELEASE_ASSERT(allocation.size >= layout.size && allocation.size <= layout.sizeWithRoomToSpecialize && !(allocation.size % 8));
            for (uint16_t field : allocation.order.span().first(allocation.born))
                RELEASE_ASSERT((!layout.fields[field].isOutOfLine || layout.fields[field].flags & fieldMayExistWithoutStorage) && !(layout.fields[field].flags & fieldIsAbsentAtBirth));
            if (!allocation.numberOfLiterals)
                continue;
            RELEASE_ASSERT(allocation.size == layout.sizeWithRoomToSpecialize);
            RELEASE_ASSERT(allocation.allocationOfFirstLiteral + allocation.numberOfLiterals < allocations.size());
            StringID name = layout.fields[allocation.discriminant].name;
            Vector<uint32_t> closure = transitionClosure(allocation.layout.index(), seen);
            BitVector isInClosure = seen;
            for (unsigned k = 0; k <= allocation.numberOfLiterals; ++k) {
                auto& entry = allocations[allocation.allocationOfFirstLiteral + k];
                auto& chosen = m_program.at(entry.layout);
                RELEASE_ASSERT(entry.constructedBy == allocation.constructedBy && entry.born == allocation.born && entry.order.size() == allocation.order.size() && !entry.numberOfLiterals);
                for (unsigned n = 0; n < entry.order.size(); ++n)
                    RELEASE_ASSERT(chosen.fields[entry.order[n]].name == layout.fields[allocation.order[n]].name);
                RELEASE_ASSERT(!chosen.hasExtension);
                if (entry.layout == allocation.layout)
                    continue;
                // A final layout: a transition target of the base that lists this constant alone, with room for every layout of the constant
                // that it is no member of.
                RELEASE_ASSERT(k < allocation.numberOfLiterals && chosen.isAllocated && isInClosure.get(entry.layout.index()));
                RELEASE_ASSERT(constantOf(entry.layout.index(), name) == allocation.firstLiteral + static_cast<int32_t>(k));
                RELEASE_ASSERT(entry.size == chosen.sizeWithRoomToSpecialize);
                transitionClosure(entry.layout.index(), seen);
                for (uint32_t other : closure)
                    RELEASE_ASSERT(constantOf(other, name) != constantOf(entry.layout.index(), name) || seen.get(other) || std::ranges::binary_search(m_program.layouts[other].members, entry.layout));
            }
            // In the base layout: whatever lists a constant has room in what is allocated for it.
            for (uint32_t target : closure) {
                for (auto& constant : constantsOf(target, name)) {
                    auto integer = integerOf(constant);
                    if (!integer)
                        continue;
                    int64_t k = static_cast<int64_t>(*integer) - allocation.firstLiteral;
                    RELEASE_ASSERT(k >= 0 && k < allocation.numberOfLiterals);
                    auto& entry = allocations[allocation.allocationOfFirstLiteral + k];
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

    // Compiled code converts what it stores to the way the slot keeps it, and tests nothing.
    void checkStores()
    {
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            if (!(m_heldByFunction[i] & holdsStoreToField))
                return;
            auto& function = m_program.functions[i];
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = function.at(id);
                    const Field* field = nullptr;
                    if (value.opcode == Opcode::PutField)
                        field = &m_program.at(value.layout()).fields[value.field()];
                    else if (value.opcode == Opcode::PutById) {
                        if (LayoutID view = m_program.typeOf(function, value.child(0)).onlyView()) {
                            if (auto found = m_program.at(view).fieldNamed(value.string()))
                                field = &m_program.at(view).fields[*found];
                        }
                    }
                    RELEASE_ASSERT(!field || m_program.typeOf(function, value.child(1)).isWithin(m_program.at(field->stored), m_program));
                }
            }
        });
    }

    void checkFieldsNeverSeenAbsent()
    {
        auto& layouts = m_program.layouts;
        auto isNeverSeenAbsent = [](const Field& field) -> bool {
            return field.flags & fieldIsNeverSeenAbsent;
        };
        auto require = [&](bool condition, const Layout& layout, const Field& field, ASCIILiteral what) {
            if (condition) [[likely]]
                return;
            dataLogLn("AOT: placement: ", nameOf(layout), " (layout ", &layout - &layouts[0], ") field ", nameOf(field.name), " is taken never to be seen absent, but ", what);
            RELEASE_ASSERT_NOT_REACHED();
        };
        // A transition neither adds such a field nor changes what is known of one.
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (layouts[i].specializations.isEmpty())
                continue;
            withFieldsByName(m_fieldNamed, i, [&] {
                for (LayoutID specialization : layouts[i].specializations) {
                    for (auto& field : m_program.at(specialization).fields) {
                        uint32_t own = m_fieldNamed[field.name.index()];
                        require(isNeverSeenAbsent(field) == (own != none && isNeverSeenAbsent(layouts[i].fields[own])), m_program.at(specialization), field, "that is not so on the other side of a transition"_s);
                    }
                }
            });
        }
        // What compiled code goes by where it carries out the initial stores of a function.
        for (auto& initial : m_initialStores) {
            auto& function = m_program.functions[initial.function];
            auto& view = m_program.at(initial.view);
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
                    auto& throughView = view.fields[*fieldOf(initial, k)];
                    RELEASE_ASSERT(field.name == names[k] && field.slot == throughView.slot && (!field.isOutOfLine || field.flags & fieldMayExistWithoutStorage) && !throughView.isLackedBySomeMember);
                }
            });
        }
        // Somebody stores to it, and counts it among the keys, before anybody looks.
        forEachFieldThatIsNotBorn([&](uint32_t index, Layout& layout, Field& field) {
            require(!isNeverSeenAbsent(field) || getsFieldBeforeAnyoneLooks(index, field.name), layout, field, "an allocation does not bear it, and not all that its objects may be handed to store it first"_s);
        });
        // What it is for: a slot that cannot say "absent", unless a field without the flag shares it.
        unsigned numberOfFields = 0;
        unsigned numberOutOfLine = 0;
        BitVector isSpoiled;
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!isNeverSeenAbsent(field))
                    continue;
                require(exists(field) && isObjectLayout(layout.kind), layout, field, "it is no field of an object that something stores to"_s);
                ++numberOfFields;
                // Every out-of-line slot can say "absent". The flag gains nothing there, and nothing goes by it.
                if (field.isOutOfLine) {
                    ++numberOutOfLine;
                    continue;
                }
                if (!(m_program.at(field.stored).primitives & Primitive::Absent))
                    continue;
                require(m_mayBeSeenAbsent.quickGet(field.slot.index()), layout, field, "its slot may hold nothing, and no other field shares the slot that could be the reason"_s);
                isSpoiled.set(field.slot.index());
            }
        }
        if (!numberOfFields)
            return;
        report(numberOfFields, " fields are never seen absent, ", numberOutOfLine, " of them out of line; ", isSpoiled.bitCount(), " of the slots of the others may hold nothing all the same");
        // By slot: how many fields of objects are without the flag, and the first of them.
        Vector<std::pair<unsigned, const Layout*>> others(FillWith { }, isSpoiled.size(), std::pair<unsigned, const Layout*> { 0, nullptr });
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!layout.isAllocated || !isObjectLayout(layout.kind) || !exists(field) || isNeverSeenAbsent(field) || !isSpoiled.get(field.slot.index()))
                    continue;
                if (!others[field.slot.index()].first++)
                    others[field.slot.index()].second = &layout;
            }
        }
        for (auto& layout : layouts) {
            for (auto& field : layout.fields) {
                if (!isNeverSeenAbsent(field) || !isSpoiled.get(field.slot.index()) || !isSpoiled.quickClear(field.slot.index()))
                    continue;
                auto [count, first] = others[field.slot.index()];
                report("  ", nameOf(layout), ".", nameOf(field.name), ": because of ", count, " layouts, the first is ", first ? nameOf(*first) : nameOf(StringID()));
            }
        }
    }

    // See fieldIsPinned in AOTTypedData.h. What is decided here is what the layouts say: who stores what is seen to where it is stored.
    void pinLiterals()
    {
        auto& layouts = m_program.layouts;
        // The shape of such an object changes, and what it was born with stays.
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
            if (!isObjectLayout(layout.kind) || !isMade(layout) || changesShape.quickGet(i))
                continue;
            for (auto& field : layout.fields) {
                if (!exists(field) || field.isOutOfLine || field.literals.size() != 1 || !field.literals[0].string || !(field.flags & fieldIsNeverSeenAbsent) || m_program.at(field.slot).isDiscriminant)
                    continue;
                field.isPinned = true;
                m_program.at(field.slot).hasPinnedFields = true;
                ++numberOfPinnedFields;
            }
        }
        unsigned numberOfViewFields = 0;
        unsigned numberOfViewFieldsBehindStrangers = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& view = layouts[i];
            if (!isObjectLayout(view.kind) || view.members.isEmpty())
                continue;
            for (unsigned k = 0; k < view.fields.size(); ++k) {
                auto& field = view.fields[k];
                if (!exists(field) || field.isLackedBySomeMember || !m_program.at(field.slot).hasPinnedFields)
                    continue;
                bool isPinnedInEveryMember = std::ranges::all_of(view.members, [&](LayoutID member) {
                    auto own = m_program.at(member).fieldNamed(field.name);
                    return own && m_program.at(member).fields[*own].slot == field.slot && m_program.at(member).fields[*own].isPinned;
                });
                // Any object may get to such a view, and is asked.
                field.isPinnedInEveryMember = isPinnedInEveryMember && !view.admitsForeignObjects;
                numberOfViewFields += field.isPinnedInEveryMember;
                numberOfViewFieldsBehindStrangers += isPinnedInEveryMember && view.admitsForeignObjects;
            }
        }
        report(numberOfPinnedFields, " fields are pinned to the one string they list; ", numberOfViewFields, " fields of views are pinned in every member, and ", numberOfViewFieldsBehindStrangers, " more of views that admit foreign objects, which go without");
    }

    void checkPlacement()
    {
        checkAllocations();
        checkStores();
        checkFieldsNeverSeenAbsent();
        auto& layouts = m_program.layouts;
        forEachIndexOnAllThreads<unsigned>(layouts.size(), m_views.size(), [] {
            return 0u;
        }, [&](unsigned&, unsigned view) {
            for (LayoutID member : layouts[view].members)
                RELEASE_ASSERT(m_program.at(member).shape && m_program.passes(layouts[view].test, m_program.at(member).shape));
        });
        // Whoever has the field passes the test for it, and below it is made sure that no more pass than have it: so whoever has it not does not pass.
        forEachName([&](FieldsOfOneName& table, std::span<const NamedField> fields) {
            for (auto& named : fields) {
                auto& field = fieldAt(named);
                forEachWithField(table, fields, layouts[named.layout].specializations, [&](uint32_t place) {
                    auto& kept = fieldAt(fields[place]);
                    RELEASE_ASSERT(kept.slot == field.slot && kept.offset == field.offset && kept.store == field.store && kept.stored == field.stored && kept.isOutOfLine == field.isOutOfLine);
                });
                if (!m_isAccessed.quickGet(named.field)) {
                    RELEASE_ASSERT(field.isLackedBySomeMember && field.membersWithIt.passesNothing() && !m_membersWith[named.field].count);
                    continue;
                }
                uint32_t numberOfMembersWithIt = 0;
                forEachMemberWithField(table, fields, layouts[named.layout], [&](uint32_t place) {
                    auto& own = fieldAt(fields[place]);
                    if (own.slot != field.slot) {
                        RELEASE_ASSERT(std::ranges::binary_search(layouts[named.layout].membersByFit, LayoutID(fields[place].layout)) && !m_isStoredThrough.quickGet(named.field));
                        RELEASE_ASSERT(field.isLackedBySomeMember && !m_program.passes(field.membersWithIt, layouts[fields[place].layout].shape));
                        return;
                    }
                    RELEASE_ASSERT(own.slot == field.slot && own.offset == field.offset && own.store == field.store && own.stored == field.stored && own.isOutOfLine == field.isOutOfLine);
                    RELEASE_ASSERT(!field.isLackedBySomeMember || m_program.passes(field.membersWithIt, layouts[fields[place].layout].shape));
                    ++numberOfMembersWithIt;
                });
                RELEASE_ASSERT(numberOfMembersWithIt == m_membersWith[named.field].count);
                RELEASE_ASSERT(field.isLackedBySomeMember == (numberOfMembersWithIt != layouts[named.layout].members.size()));
            }
        });
        // All that are to pass do. That no more pass: a range has their number, and the sets have as many bits as they have shapes.
        Vector<std::pair<uint32_t, uint32_t>> sets;
        auto checkNumberPassing = [&](const ShapeTest& test, uint32_t expected) {
            if (test.isRange())
                RELEASE_ASSERT(test.count == expected);
            else {
                RELEASE_ASSERT(!(test.set % 32) && expected < test.count && test.last() <= m_program.numberOfShapes);
                sets.append({ test.set, expected });
            }
        };
        Vector<uint32_t> numberOfInstanceLayouts(FillWith { }, layouts.size(), 0);
        Vector<std::pair<uint32_t, uint32_t>> extents;
        Vector<std::pair<uint32_t, uint32_t>> extensionExtents;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            checkNumberPassing(layout.test, layout.members.size());
            if (layout.kind == LayoutKind::Instance && layout.isAllocated) {
                for (LayoutID ancestor(i); ancestor; ancestor = m_program.at(ancestor).base) {
                    RELEASE_ASSERT(m_program.passes(m_program.at(ancestor).instances, layout.shape));
                    ++numberOfInstanceLayouts[ancestor.index()];
                }
            }
            extents.shrink(0);
            extensionExtents.shrink(0);
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                auto& field = layout.fields[k];
                if (!exists(field)) {
                    RELEASE_ASSERT(!field.slot && !field.stored && field.offset == none && field.isLackedBySomeMember && field.membersWithIt.passesNothing());
                    continue;
                }
                auto& slot = m_program.at(field.slot);
                RELEASE_ASSERT(field.stored == slot.type && field.store == slot.store && field.offset == slot.offset && field.isOutOfLine == slot.isOutOfLine);
                if (field.isOutOfLine) {
                    RELEASE_ASSERT(isObjectLayout(layout.kind) && field.store == storeOutOfLineFor(field.slot.index(), field.stored) && m_program.at(field.stored).primitives & Primitive::Absent && !slot.isDiscriminant && !(field.flags & fieldIsImmutable));
                    RELEASE_ASSERT(field.offset >= offsetOfFirstExtensionSlot && (!layout.isAllocated || (layout.hasExtension && field.offset + sizeOf(field.store) <= layout.extensionSize)));
                } else
                    RELEASE_ASSERT(field.store == storeFor(m_program.at(field.stored), m_isExpectedInt32.quickGet(field.slot.index()), m_keepsWideReferences.quickGet(field.slot.index())));
                RELEASE_ASSERT(!(field.offset % sizeOf(field.store)));
                RELEASE_ASSERT(!isMade(layout) || m_program.at(field.type ? field.type : m_program.types.anything).isWithin(m_program.at(field.stored), m_program));
                if (field.isLackedBySomeMember)
                    checkNumberPassing(field.membersWithIt, m_membersWith[m_firstField[i] + k].count);
                (field.isOutOfLine ? extensionExtents : extents).append({ field.offset, field.offset + sizeOf(field.store) });
            }
            if (!layout.isAllocated)
                continue;
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
                RELEASE_ASSERT(m_program.at(target).hasExtension == layout.hasExtension);
            RELEASE_ASSERT(end <= layout.size && layout.size <= layout.sizeWithRoomToSpecialize);
            for (LayoutID specialization : layout.specializations)
                RELEASE_ASSERT(m_program.at(specialization).size <= layout.sizeWithRoomToSpecialize);
        }
        for (unsigned i = 0; i < layouts.size(); ++i) {
            checkNumberPassing(layouts[i].instances, numberOfInstanceLayouts[i]);
            checkNumberPassing(layouts[i].retagSources, retagSourcesOf(i).size());
            for (uint32_t source : retagSourcesOf(i))
                RELEASE_ASSERT(m_program.passes(layouts[i].retagSources, layouts[source].shape) && !m_program.passes(layouts[i].castTest, layouts[source].shape));
            if (isObjectLayout(layouts[i].kind))
                checkNumberPassing(layouts[i].castTest, layouts[i].members.size() - retaggedMembersOf(i).size());
            for (LayoutID member : layouts[i].members)
                RELEASE_ASSERT(m_program.passes(layouts[i].castTest, m_program.at(member).shape) == !std::ranges::binary_search(retaggedMembersOf(i), member));
        }
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
    // By string. See withFieldsByName().
    Vector<uint32_t> m_fieldNamed;
    std::atomic<unsigned> m_numberOfFailures { 0 };
    uint32_t m_mostFields { 0 };
    // By layout, and one more.
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_firstView;
    Vector<FieldsOfName> m_names;
    Vector<NamedField> m_fieldsByName;
    Vector<std::array<uint64_t, 2>> m_hashOfLayout;
    // By field.
    BitVector m_isAccessed;
    Vector<uint8_t> m_heldByFunction;
    BitVector m_isStoredThrough;
    Vector<uint32_t> m_lackedFieldOfField;
    Vector<Fingerprint> m_membersWith;
    Vector<uint32_t> m_numberOfBirthsInitializing;
    Vector<uint32_t> m_views;
    Vector<LackedField> m_lackedFields;
    Vector<SetOfMembers> m_setsOfMembers;
    Vector<uint32_t> m_listedMembers;
    unsigned m_numberOfRequiredFieldsLacked { 0 };
    // By slot.
    BitVector m_holdsSeveralTypes;
    BitVector m_mayBeAbsent;
    BitVector m_isInMadeLayout;
    Vector<Family> m_families;
    // By slot: access sites, those in loops weighted.
    Vector<uint64_t> m_accessWeight;
    Vector<uint64_t> m_numberOfStoresOfDoubles;
    BitVector m_isInCopiedRecord;
    const Vector<size_t> m_sizeClasses { MarkedSpace::sizeClassesFor(Options::sizeClassProgressionWithAOT()) };
    // By layout.
    Vector<uint32_t> m_group;
    static constexpr uint32_t noCap = UINT32_MAX;
    static constexpr uint32_t referencesBeyondCreation = 4;
    static constexpr uint32_t mostRoomInLine = 128;
    // By layout: where the inline slots that may as well be out of line end. noCap: it has no extension.
    Vector<uint32_t> m_inlineCap;
    // With what typeSlots() made of them.
    Vector<std::tuple<uint32_t, Store, TypeID>> m_slotsPutOutOfLine;
    BitVector m_isExpectedInt32;
    BitVector m_hasIntegerField;
    // By slot: some field in it has fieldIsNeverSeenAbsent, some has not.
    BitVector m_isNeverSeenAbsent;
    BitVector m_mayBeSeenAbsent;
    Vector<InitialStores> m_initialStores;
    // By function: which of them, or none.
    Vector<uint32_t> m_initialStoresOfFunction;
    // By layout: some function declares `this` to be of it, so the run time makes a fresh object of it for the function. And the names that
    // all of those have among namesStoredFirstBy().
    BitVector m_isLayoutOfThis;
    Vector<Vector<StringID>> m_namesStoredFirstByAll;
    // By allocation: more becomes of some object of it than that `new` hands it on to whatever it calls.
    BitVector m_isUsedOtherwise;
    // A slot, and the type of one of its fields that is not the type of the first.
    Vector<std::pair<uint32_t, uint32_t>> m_otherTypes;
    Vector<TypeID> m_typesWithAbsent;
    Vector<uint64_t> m_testsOfType;
    // By layout.
    Vector<uint64_t> m_testsOfView;
    Vector<uint64_t> m_testsOfClass;
    Vector<uint64_t> m_coercionsToView;
    Vector<uint64_t> m_castsOfWhatIsNotReadOutOfPlace;
    BitVector m_isBornOrNamedByCast;
    BitVector m_isFirstStoredWithValue;
    BitVector m_existsFromBirth;
    // And one more.
    Vector<uint32_t> m_firstRetagSource;
    Vector<uint32_t> m_retagSources;
    BitVector m_hasLargeMember;
    Vector<uint32_t> m_firstRetaggedMember;
    Vector<LayoutID> m_retaggedMembers;
    Vector<uint32_t> m_numberOfBirths;
    // By layout, of environments: some allocation is in a loop or belongs to a function that captures something.
    BitVector m_isMadeOften;
    // By slot.
    BitVector m_keepsWideReferences;
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
        Vector<unsigned> timesConstant;
        timesConstant.fill(0, strings.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            for (auto& value : m_program.functions[i].values) {
                if (value.opcode == Opcode::String)
                    WTF::atomicExchangeAdd(&timesConstant[value.string().index()], 1u, std::memory_order_relaxed);
            }
        });
        BitVector isConstant(strings.size());
        for (uint32_t index = 0; index < strings.size(); ++index) {
            if (timesConstant[index])
                isConstant.set(index);
        }
        auto hasOnlyDecodedForm = [&](uint32_t index) {
            return isConstant.get(index) && !charactersAreAllASCII(byteCast<Latin1Character>(strings[index])) && !isPrivateName(strings[index]) && !isEnginePrivateName(strings[index]);
        };
        Vector<uint32_t> byLastCharacters;
        for (uint32_t index = 0; index < strings.size(); ++index) {
            if (!hasOnlyDecodedForm(index))
                byLastCharacters.append(index);
        }
        std::ranges::sort(byLastCharacters, [&](uint32_t a, uint32_t b) {
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
            if (tail.size() <= whole.size() && equalSpans(tail, whole.last(tail.size())))
                holders[byLastCharacters[i - 1]] = holders[byLastCharacters[i]];
        }
        Vector<uint32_t> gaps(FillWith { }, strings.size(), 0);
        {
            MonotonicTime startOfSearch = MonotonicTime::now();
            constexpr size_t sizeOfHead = sizeof(uint32_t);
            constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
            auto headAt = [](std::span<const uint8_t> text, size_t at) {
                uint32_t head;
                memcpy(&head, text.data() + at, sizeOfHead);
                return static_cast<uint64_t>(head) + 1;
            };
            auto beginsWith = [](std::span<const uint8_t> text, std::span<const uint8_t> head) {
                return head.size() <= text.size() && equalSpans(head, text.first(head.size()));
            };
            Vector<uint32_t> inOrder;
            for (uint32_t index : byLastCharacters) {
                if (holders[index] == index && strings[index].size() >= sizeOfHead)
                    inOrder.append(index);
            }
            std::ranges::sort(inOrder, [&](uint32_t a, uint32_t b) {
                return std::ranges::lexicographical_compare(strings[a], strings[b]);
            });
            Vector<uint32_t> longestHeads(FillWith { }, inOrder.size(), none);
            Vector<uint32_t> open;
            UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, uint32_t>> rangesOfHeads;
            for (uint32_t place = 0; place < inOrder.size(); ++place) {
                auto text = strings[inOrder[place]];
                while (!open.isEmpty() && !beginsWith(text, strings[inOrder[open.last()]]))
                    open.removeLast();
                if (!open.isEmpty())
                    longestHeads[place] = open.last();
                open.append(place);
                rangesOfHeads.add(headAt(text, 0), std::pair { place, place }).iterator->value.second = place + 1;
            }
            Vector<uint32_t> fromLongest = inOrder;
            std::ranges::sort(fromLongest, [&](uint32_t a, uint32_t b) {
                return strings[a].size() != strings[b].size() ? strings[a].size() > strings[b].size() : a < b;
            });
            uint64_t placesSearched = 0;
            uint64_t stepsOnChains = 0;
            uint64_t numberHeld = 0;
            uint64_t bytesHeld = 0;
            for (uint32_t whole : fromLongest) {
                if (holders[whole] != whole)
                    continue;
                auto text = strings[whole];
                for (size_t at = 0; at + sizeOfHead <= text.size(); ++at) {
                    auto found = rangesOfHeads.find(headAt(text, at));
                    if (found == rangesOfHeads.end())
                        continue;
                    ++placesSearched;
                    auto rest = text.subspan(at);
                    auto [first, behindLast] = found->value;
                    uint32_t low = first;
                    for (uint32_t high = behindLast; low < high;) {
                        uint32_t middle = low + (high - low) / 2;
                        if (std::ranges::lexicographical_compare(rest, strings[inOrder[middle]]))
                            high = middle;
                        else
                            low = middle + 1;
                    }
                    for (uint32_t place = low == first ? none : low - 1; place != none; place = longestHeads[place]) {
                        ++stepsOnChains;
                        uint32_t part = inOrder[place];
                        auto inner = strings[part];
                        if (inner.size() >= text.size() || !beginsWith(rest, inner))
                            continue;
                        if (holders[part] != part)
                            break;
                        holders[part] = whole;
                        gaps[part] = safeCast<uint32_t>(rest.size() - inner.size());
                        ++numberHeld;
                        bytesHeld += inner.size();
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
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: texts inside longer texts: ", numberHeld, " of ", inOrder.size(), " texts, ", bytesHeld, " bytes; ", placesSearched, " places searched, ", stepsOnChains, " steps on chains, ", (MonotonicTime::now() - startOfSearch).milliseconds(), " ms");
        }
        Vector<uint32_t> ends(FillWith { }, strings.size(), 0);
        auto layText = [&](uint32_t index) {
            if (index >= strings.size() || hasOnlyDecodedForm(index))
                return;
            uint32_t holder = holders[index];
            if (ends[holder] || hasOnlyDecodedForm(holder))
                return;
            m_result.append(strings[holder]);
            ends[holder] = safeCast<uint32_t>(m_result.size());
        };
        Vector<std::array<uint32_t, 3>> placesAndNumbers(m_program.functions.size(), [&](size_t index) {
            auto& function = m_program.functions[index];
            return std::array<uint32_t, 3> { function.module ? function.module.index() : UINT32_MAX, function.position, static_cast<uint32_t>(index) };
        });
        std::ranges::sort(placesAndNumbers);
        Vector<uint32_t> functionsInSourceOrder(m_program.functions.size(), [&](size_t index) {
            return placesAndNumbers[index][2];
        });
        std::array<std::atomic<int8_t>, 256> namesString { };
        Vector<Vector<uint32_t>> textsOfFunctions(m_program.functions.size());
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            for (auto& value : m_program.functions[i].values) {
                int8_t names = namesString[static_cast<uint8_t>(value.opcode)].load(std::memory_order_relaxed);
                if (!names) {
                    StringView name { nameOf(value.opcode) };
                    names = value.opcode == Opcode::String || name.find("ById"_s) != notFound || name.find("Method"_s) != notFound || name.find("Global"_s) != notFound ? 1 : -1;
                    namesString[static_cast<uint8_t>(value.opcode)].store(names, std::memory_order_relaxed);
                }
                if (names > 0)
                    textsOfFunctions[i].append(value.immediates[0]);
            }
        });
        for (uint32_t functionIndex : functionsInSourceOrder) {
            auto& function = m_program.functions[functionIndex];
            if (function.name)
                layText(function.name.index());
            for (uint32_t text : textsOfFunctions[functionIndex])
                layText(text);
        }
        forEachIndexInParallel(m_program.functions.size(), [&](unsigned i) {
            textsOfFunctions[i].clear();
        });
        for (uint32_t index = 0; index < strings.size(); ++index)
            layText(index);
        Vector<uint32_t> pairs;
        for (uint32_t index = 0; index < strings.size(); ++index) {
            RELEASE_ASSERT(hasOnlyDecodedForm(index) || ends[holders[index]] > gaps[index] + strings[index].size());
            pairs.append(hasOnlyDecodedForm(index) ? 0 : ends[holders[index]] - gaps[index] - strings[index].size());
            pairs.append(strings[index].size());
        }
        header.stringsOffset = append(pairs);
        Vector<uint32_t> stringFacts;
        for (auto string : m_program.strings) {
            auto characters = byteCast<Latin1Character>(string);
            uint32_t facts = isPrivateName(string) ? Data::stringIsPrivateName : isEnginePrivateName(string) ? Data::stringIsEnginePrivateName : 0;
            uint32_t identifier = stringFacts.size() < m_program.identifierNumbers.size() ? m_program.identifierNumbers[stringFacts.size()] : 0;
            RELEASE_ASSERT(identifier <= Data::stringHashMask);
            if (charactersAreAllASCII(characters))
                facts |= Data::stringIsASCII | (identifier ? 0 : StringHasher::computeHashAndMaskTop8Bits(characters));
            if (identifier)
                facts |= Data::stringHasIdentifierNumber | identifier;
            stringFacts.append(facts);
        }
        header.stringFactsOffset = append(stringFacts);
        uint32_t greatestIdentifier = 0;
        for (uint32_t identifier : m_program.identifierNumbers)
            greatestIdentifier = std::max(greatestIdentifier, identifier);
        Vector<uint32_t> stringsOfIdentifiers(FillWith { }, greatestIdentifier + 1, 0);
        Vector<uint32_t> hashesOfIdentifiers(FillWith { }, greatestIdentifier + 1, 0);
        for (uint32_t index = 0; index < m_program.identifierNumbers.size(); ++index) {
            if (uint32_t identifier = m_program.identifierNumbers[index]) {
                stringsOfIdentifiers[identifier] = index + 1;
                if (auto characters = byteCast<Latin1Character>(m_program.strings[index]); charactersAreAllASCII(characters))
                    hashesOfIdentifiers[identifier] = StringHasher::computeHashAndMaskTop8Bits(characters);
            }
        }
        header.numberOfStringsOfIdentifiers = stringsOfIdentifiers.size();
        header.stringsOfIdentifiersOffset = append(stringsOfIdentifiers);
        header.hashesOfIdentifiersOffset = append(hashesOfIdentifiers);
        // What is among the strings and is no constant of the program need not be a text.
        Vector<uint32_t> decodedStrings;
        for (uint32_t index = 0; index < m_program.strings.size(); ++index) {
            auto bytes = m_program.strings[index];
            if (!isConstant.get(index) || charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
                continue;
            // UTF-8, but that half of a surrogate pair on its own is encoded like any other code unit.
            Vector<char16_t> units;
            char16_t all = 0;
            for (size_t i = 0; i < bytes.size();) {
                uint8_t lead = bytes[i];
                unsigned length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                RELEASE_ASSERT(i + length <= bytes.size());
                char32_t character = length == 1 ? lead : lead & (0x7f >> length);
                for (unsigned k = 1; k < length; ++k)
                    character = character << 6 | (bytes[i + k] & 0x3f);
                i += length;
                if (U_IS_BMP(character))
                    units.append(static_cast<char16_t>(character));
                else {
                    units.append(U16_LEAD(character));
                    units.append(U16_TRAIL(character));
                }
            }
            for (char16_t unit : units)
                all |= unit;
            RELEASE_ASSERT(units.size() < Data::isDecodedTo16Bits);
            if (all <= 0xff) {
                Vector<uint8_t> narrow(units.size(), [&](size_t i) {
                    return static_cast<uint8_t>(units[i]);
                });
                decodedStrings.appendList({ index, append(narrow), static_cast<uint32_t>(units.size()) });
            } else
                decodedStrings.appendList({ index, append(units), static_cast<uint32_t>(units.size()) | Data::isDecodedTo16Bits });
        }
        header.numberOfDecodedStrings = decodedStrings.size() / 3;
        header.decodedStringsOffset = append(decodedStrings);
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
                    case Opcode::StrictEq:
                    case Opcode::Eq:
                    case Opcode::GetByVal:
                    case Opcode::PutByVal:
                    case Opcode::InByVal:
                    case Opcode::DeleteByVal:
                    case Opcode::ToPropertyKey:
                    case Opcode::Operation:
                    case Opcode::CallBuiltin:
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
        header.numberOfTypes = m_offsetOfType.size();
        header.typesOffset = append(m_offsetOfType);
        size_t sizeBeforeLayouts = m_result.size();

        Vector<uint32_t> shapes(FillWith { }, m_program.numberOfShapes + 1, 0);
        Vector<uint32_t> classes(FillWith { }, m_program.layouts.size(), 0);
        for (unsigned i = 0; i < m_program.layouts.size(); ++i) {
            auto& layout = m_program.layouts[i];
            // The prototype of a class nothing allocates is on the chain of the classes that extend it.
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
        header.classesOffset = append(classes);
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
            uint32_t holdsItsTarget = callee.captureTypes.isEmpty() && !callee.hasManyClosures ? 0 : boundFunctionHoldsItsTarget;
            boundFunctions.append({ entrypointOf(bound), entrypointOf(FunctionID(static_cast<uint32_t>(key >> 2))), static_cast<uint32_t>(key & 3) | holdsItsTarget });
        }
        std::ranges::sort(boundFunctions);
        Vector<uint32_t> boundFunctionsAndWhatTheyBind;
        for (auto& entry : boundFunctions)
            boundFunctionsAndWhatTheyBind.append(std::span { entry });
        header.numberOfBoundFunctions = boundFunctions.size();
        header.boundFunctionsOffset = append(boundFunctionsAndWhatTheyBind);
        Vector<uint32_t> rows;
        uint32_t startOfRow = m_program.numberedRows.size() + 1;
        for (auto& row : m_program.numberedRows) {
            rows.append(startOfRow);
            startOfRow += 1 + row.names.size();
        }
        rows.append(startOfRow);
        for (auto& row : m_program.numberedRows) {
            rows.append(row.names.size() | row.inlineCapacity << 8 | row.hasNullPrototype << 16);
            for (StringID name : row.names)
                rows.append(name.index());
        }
        header.numberOfRows = m_program.numberedRows.size();
        header.rowsOffset = append(rows);
        header.firstNumberOfRowsOfThis = std::min<uint32_t>(m_program.firstNumberOfRowsOfThis, m_program.numberedRows.size());
        {
            Vector<std::array<uint32_t, 3>> grown;
            for (auto& [from, name, to] : m_program.rowsGrownInto) {
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
            header.numberOfRowsGrownInto = words.size() / 3;
            header.rowsGrownIntoOffset = append(words);
        }
        {
            uint32_t greatest = 0;
            for (uint32_t identifier : m_program.identifierNumbers)
                greatest = std::max(greatest, identifier);
            Vector<Vector<std::pair<uint32_t, uint8_t>, 1>> cellsOfNames;
            cellsOfNames.grow(greatest + 1);
            uint32_t greatestCell = 0;
            for (uint32_t number = 0; number < m_program.numberedRows.size(); ++number) {
                auto& row = m_program.numberedRows[number];
                uint32_t cell = number + 1;
                greatestCell = std::max(greatestCell, cell);
                for (unsigned place = 0; place < row.names.size(); ++place) {
                    uint32_t index = row.names[place].index();
                    if (uint32_t identifier = index < m_program.identifierNumbers.size() ? m_program.identifierNumbers[index] : 0)
                        cellsOfNames[identifier].append({ cell, static_cast<uint8_t>(place) });
                }
            }
            Vector<uint32_t> order;
            for (uint32_t identifier = 0; identifier <= greatest; ++identifier) {
                if (!cellsOfNames[identifier].isEmpty())
                    order.append(identifier);
            }
            std::ranges::stable_sort(order, [&](uint32_t a, uint32_t b) {
                return cellsOfNames[a].size() > cellsOfNames[b].size();
            });
            Vector<uint32_t> displacements(FillWith { }, greatest + 1, 0);
            Vector<uint32_t> places(FillWith { }, greatestCell + 1, 0);
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
                auto& cells = cellsOfNames[identifier];
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
                while (places.size() < displacement + greatestCell + 1)
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
            header.displacementsOfNamesOffset = append(displacements);
            header.placesByNameAndRowOffset = append(places);
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT image: places by name and row: ", order.size(), " names in rows, ", places.size(), " entries, ", displacements.size(), " displacements");
        }
        Vector<uint8_t> extendedClasses;
        for (auto& engineClass : Builtins::singleton().classes())
            extendedClasses.append(m_program.isExtended(engineClass.name));
        header.extendedClassesOffset = append(extendedClasses);

        BitVector usesFieldTemplate;
        for (size_t i = 0; i < m_program.allocations.size(); ++i) {
            auto& site = m_program.allocations[i];
            if (!site.numberOfLiterals)
                continue;
            usesFieldTemplate.set(i);
            for (uint32_t literal = 0; literal <= site.numberOfLiterals; ++literal)
                usesFieldTemplate.set(site.allocationOfFirstLiteral + literal);
        }
        Vector<AllocationData> allocations;
        for (auto& allocation : m_program.allocations) {
            auto& layout = m_program.at(allocation.layout);
            uint32_t orderWord = 0;
            auto [expected, born] = orderOfWhatExists(layout, allocation);
            if (born > maxFieldsInOrder)
                orderWord = orderWordOf(keysAreInLayoutOrder, 0);
            else if (isObjectLayout(layout.kind)) {
                size_t order = layout.orders.findIf([&](auto& candidate) {
                    return startsWith(candidate, expected);
                });
                RELEASE_ASSERT(order != notFound);
                orderWord = orderWordOf(static_cast<uint32_t>(order), born);
            }
            // By this compiled code knows an object that nothing has been stored to: see Function::numberOfInitialStores.
            RELEASE_ASSERT(born || !orderWord);
            allocations.append(AllocationData { layout.shape, orderWord, allocation.size, entrypointOf(allocation.constructedBy), usesFieldTemplate.get(allocations.size()) });
        }
        header.numberOfAllocations = allocations.size();
        header.allocationsOffset = append(allocations);
        size_t sizeAfterLayouts = m_result.size();

        Vector<uint32_t> shapeSets(FillWith { }, (m_program.shapeSetBits.size() + 31) / 32, 0);
        for (size_t bit : m_program.shapeSetBits)
            shapeSets[bit / 32] |= 1u << bit % 32;
        header.numberOfShapeSetWords = shapeSets.size();
        header.shapeSetsOffset = append(shapeSets);

        header.sizeOfStatics = m_program.sizeOfStatics;
        header.sizeOfStaticsFilledLate = m_program.sizeOfStaticsFilledLate;
        // The memory is all zero bits at first, which in a boxed value stands for nothing.
        struct InitialWord {
            uint32_t offset;
            int64_t bits;
        };
        Vector<InitialWord> initialWords;
        for (auto& variable : m_program.variables) {
            // Only the collector looks at it before the first store, and to that no bits are nothing. Or no bits are what it starts out as.
            if (variable.isNeverSeenUninitialized || variable.startsZero)
                continue;
            int64_t bits = 0;
            switch (repOf(variable.store)) {
            case Rep::JSValue:
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
            while (end < initialWords.size() && initialWords[end].bits == initialWords[first].bits && initialWords[end].offset == initialWords[end - 1].offset + sizeof(uint64_t) && initialWords[end].offset != m_program.sizeOfStaticsFilledLate)
                ++end;
            uint64_t bits = initialWords[first].bits;
            staticsRuns.appendList({ static_cast<uint32_t>(initialWords[first].offset / sizeof(uint64_t)), end - first, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) });
            first = end;
        }
        header.numberOfStaticsRuns = staticsRuns.size() / 4;
        header.staticsRunsOffset = append(staticsRuns);
        header.numberOfGlobalPropertyCaches = m_program.numberOfGlobalPropertyCaches;
        header.globalPropertyCachesOffset = m_program.globalPropertyCachesOffset;
        header.staticFieldPlacesOffset = m_program.staticFieldPlacesOffset;
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
        for (uint32_t i = 0; i < m_program.offsetsOfSharedRegExpObjects.size(); ++i)
            addRootWithCard(m_program.sharedRegExpObjectsOffset + i * sizeof(JSCell*));
        header.numberOfRoots = roots.size();
        header.rootsOffset = append(roots);
        header.numberOfCards = rootMasks.size();
        header.rootMasksOffset = append(rootMasks);
        header.copyOnWriteButterfliesOffset = m_program.copyOnWriteButterfliesOffset;
        Vector<std::array<uint32_t, 2>> arrayLiterals;
        for (auto& elements : m_program.elementsOfArrayLiterals)
            arrayLiterals.append({ append(elements), static_cast<uint32_t>(elements.size()) });
        header.arrayLiteralsOffset = append(arrayLiterals);

        header.numberOfFunctions = m_program.functions.size();
        Vector<uint32_t> names;
        Vector<uint8_t> kinds;
        Vector<uint16_t> lengths;
        Vector<uint8_t> lengthsInBytes;
        for (auto& function : m_program.functions) {
            BuiltinID builtin = m_program.builtinCarriedOutBy(function);
            StringID nameOfBuiltin = builtin ? m_program.stringOf(Builtins::singleton().at(builtin).nameName) : StringID();
            names.append((nameOfBuiltin ? nameOfBuiltin : function.name).index());
            static_assert(static_cast<uint8_t>(FunctionKind::Bound) < functionKindOfBodiesOfSeveralSites && functionKindOfBodiesOfSeveralSites <= functionKindMask);
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
        uint32_t firstFunctionClosuresGoBy = m_program.functions.size();
        uint32_t endOfFunctionsClosuresGoBy = m_program.functions.size();
        bool closuresGoBySome = false;
        auto noteClosuresGoBy = [&](uint32_t function) {
            firstFunctionClosuresGoBy = std::min(firstFunctionClosuresGoBy, function);
            endOfFunctionsClosuresGoBy = closuresGoBySome ? std::max(endOfFunctionsClosuresGoBy, function + 1) : function + 1;
            closuresGoBySome = true;
        };
        for (auto& [function, entrypoint] : m_program.genericEntrypoints)
            noteClosuresGoBy(entrypoint.index());
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (m_program.isResumeFunction(m_program.functions[i]))
                noteClosuresGoBy(i);
        }
        uint32_t endOfFunctionsCalledByNumber = 0;
        for (auto& evaluation : m_program.moduleEvaluations) {
            for (FunctionID callee : evaluation.callees) {
                if (!m_program.moduleEvaluationOfFunction.contains(callee.index()))
                    endOfFunctionsCalledByNumber = std::max(endOfFunctionsCalledByNumber, callee.index() + 1);
            }
        }
        uint32_t rowsLeftOutAheadOfClosures = firstFunctionClosuresGoBy > endOfFunctionsCalledByNumber ? firstFunctionClosuresGoBy - endOfFunctionsCalledByNumber : 0;
        header.firstFunctionClosuresGoBy = firstFunctionClosuresGoBy;
        header.endOfFunctionsClosuresGoBy = endOfFunctionsClosuresGoBy;
        header.rowsLeftOutAheadOfClosures = rowsLeftOutAheadOfClosures;
        auto appendRowsOfClosures = [&](std::span<const uint8_t> rows, size_t sizeOfRow) {
            auto kept = rows.subspan(firstFunctionClosuresGoBy * sizeOfRow, (endOfFunctionsClosuresGoBy - firstFunctionClosuresGoBy) * sizeOfRow);
            if (m_result.size() < firstFunctionClosuresGoBy * sizeOfRow)
                allocate(firstFunctionClosuresGoBy * sizeOfRow - m_result.size());
            uint32_t offset = allocate(kept.size());
            memcpySpan(m_result.mutableSpan().subspan(offset), kept);
            RELEASE_ASSERT(offset >= firstFunctionClosuresGoBy * sizeOfRow);
            return safeCast<uint32_t>(offset - firstFunctionClosuresGoBy * sizeOfRow);
        };
        header.functionLengthsOffset = m_program.functionLengthsAreBytes ? appendRowsOfClosures(asBytes(lengthsInBytes.span()), sizeof(uint8_t)) : appendRowsOfClosures(asBytes(lengths.span()), sizeof(uint16_t));
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
            moduleEvaluations.append(moduleEvaluations.size() + 1 + 2 * evaluation.callees.size());
            for (FunctionID callee : evaluation.callees) {
                auto other = m_program.moduleEvaluationOfFunction.find(callee.index());
                bool isRow = other != m_program.moduleEvaluationOfFunction.end();
                moduleEvaluations.append(isRow ? offsetOf(m_program.moduleEvaluations[other->value].isEvaluated) : std::numeric_limits<uint32_t>::max());
                moduleEvaluations.append(isRow ? other->value : callee.index() + rowsLeftOutAheadOfClosures);
            }
        }
        header.moduleEvaluationsOffset = append(moduleEvaluations);
        Vector<std::array<uint8_t, 2>> closureCounts;
        closureCounts.fill({ 0, 0 }, m_program.functions.size());
        auto setCounts = [&](FunctionID closuresGoBy, const Function& function) {
            auto [references, captures] = m_program.referencesAndCapturesOfClosures(function);
            closureCounts[closuresGoBy.index()] = { static_cast<uint8_t>(references), static_cast<uint8_t>(captures) };
        };
        for (auto& [function, entrypoint] : m_program.genericEntrypoints)
            setCounts(entrypoint, m_program.functions[function]);
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (m_program.isResumeFunction(m_program.functions[i]))
                setCounts(FunctionID(i), m_program.functions[i]);
        }
        header.closureCountsOffset = appendRowsOfClosures(asBytes(closureCounts.span()), sizeof(closureCounts[0]));
        Vector<uint8_t> slimClosures;
        slimClosures.fill(0, (m_program.functions.size() + 7) / 8);
        for (auto& [function, entrypoint] : m_program.genericEntrypoints) {
            if (m_program.makesSlimClosures(m_program.functions[function]))
                slimClosures[entrypoint.index() / 8] |= 1 << entrypoint.index() % 8;
        }
        header.slimClosuresOffset = append(slimClosures);
        RELEASE_ASSERT(m_program.modulesOfEntries.size() == m_program.entries.size());
        Vector<uint32_t> entryPoints(m_program.modulesOfEntries.span());
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
        // One that is never a value has no closure to ask for its `prototype`.
        Vector<std::pair<uint32_t, uint32_t>> generatorFunctions;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.declaredAs & functionIsGenerator && function.resumeFunction && m_program.genericEntrypoints.contains(i))
                generatorFunctions.append({ entrypointOf(FunctionID(i)), function.resumeFunction.index() | (function.takesItsClosure || function.rampForClosures ? Data::generatorTakesItsClosure : 0) });
        }
        std::ranges::sort(generatorFunctions);
        Vector<uint32_t> generatorAndResumeFunctions;
        for (auto [generatorFunction, resumeFunction] : generatorFunctions) {
            generatorAndResumeFunctions.append(generatorFunction);
            generatorAndResumeFunctions.append(resumeFunction);
        }
        header.numberOfGeneratorFunctions = generatorFunctions.size();
        header.generatorFunctionsOffset = append(generatorAndResumeFunctions);
        Vector<std::pair<uint32_t, uint32_t>> constructorFunctions;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            if (uint32_t allocation = m_program.functions[i].allocationOfThis; allocation != Format::none)
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
        Vector<std::pair<uint32_t, uint32_t>> refusedFreshObjects;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.allocationOfThis == Format::none)
                continue;
            auto stores = initialStoresOf(function).first(function.numberOfInitialStores);
            for (LayoutID member : m_program.at(m_program.allocations[function.allocationOfThis].layout).members) {
                auto& layout = m_program.at(member);
                bool storesAllFirst = std::ranges::all_of(layout.fields, [&](auto& field) {
                    return !(field.flags & fieldIsNeverSeenAbsent) || std::ranges::any_of(stores, [&](ValueID store) {
                        return function.at(store).string() == field.name;
                    });
                });
                if (layout.shape && !storesAllFirst)
                    refusedFreshObjects.append({ entrypointOf(FunctionID(i)), layout.shape });
            }
        }
        std::ranges::sort(refusedFreshObjects);
        Vector<uint32_t> refusedFunctionsAndShapes;
        for (auto [function, shape] : refusedFreshObjects) {
            refusedFunctionsAndShapes.append(function);
            refusedFunctionsAndShapes.append(shape);
        }
        header.numberOfRefusedFreshObjects = refusedFreshObjects.size();
        header.refusedFreshObjectsOffset = append(refusedFunctionsAndShapes);
        Vector<uint32_t> functionsThatKeepFieldNamesOffTheirPrototypes;
        for (auto& [index, entrypoint] : m_program.genericEntrypoints) {
            if (m_program.functions[index].has(Format::FunctionKeepsFieldNamesOffItsPrototype))
                functionsThatKeepFieldNamesOffTheirPrototypes.append(entrypoint.index());
        }
        std::ranges::sort(functionsThatKeepFieldNamesOffTheirPrototypes);
        header.numberOfFunctionsThatKeepFieldNamesOffTheirPrototypes = functionsThatKeepFieldNamesOffTheirPrototypes.size();
        header.functionsThatKeepFieldNamesOffTheirPrototypesOffset = append(functionsThatKeepFieldNamesOffTheirPrototypes);
        Vector<std::pair<uint32_t, uint32_t>> propertyCountsOfThis;
        Vector<std::pair<uint32_t, uint32_t>> functionsAndEntrypoints;
        for (auto& [index, entrypoint] : m_program.genericEntrypoints)
            functionsAndEntrypoints.append({ index, entrypoint.index() });
        Vector<uint32_t> countsOfThis;
        countsOfThis.fill(0, functionsAndEntrypoints.size());
        Vector<Vector<StringID, 4>> namesGivenByMakers(m_program.functions.size());
        for (auto& maker : m_program.functions) {
            UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> made;
            for (auto& block : maker.blocks) {
                for (ValueID id : block.values) {
                    auto& value = maker.at(id);
                    if (value.opcode == Opcode::Construct || value.opcode == Opcode::CreateThis) {
                        if (FunctionID expected = maker.at(value.child(0)).predictedClosure)
                            made.add(id.index(), expected.index());
                        continue;
                    }
                    if (value.opcode != Opcode::PutById || made.isEmpty())
                        continue;
                    ValueID object = value.child(0);
                    while (maker.at(object).opcode == Opcode::Cast && maker.at(object).castReason() == CastReason::Upcast)
                        object = maker.at(object).child(0);
                    auto found = made.find(object.index());
                    if (found != made.end() && !namesGivenByMakers[found->value].contains(value.string()))
                        namesGivenByMakers[found->value].append(value.string());
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
            bool isOfWholeChain = collectPropertiesGivenToThis(m_program, FunctionID(index), names, numberOfKeys);
            for (StringID name : namesGivenByMakers[index]) {
                if (!names.contains(name))
                    names.append(name);
            }
            if (names.size() + numberOfKeys)
                countsOfThis[i] = (names.size() + numberOfKeys) | (isOfWholeChain ? Data::propertyCountIsOfWholeChain : 0);
        });
        for (unsigned i = 0; i < countsOfThis.size(); ++i) {
            if (countsOfThis[i])
                propertyCountsOfThis.append({ functionsAndEntrypoints[i].second, countsOfThis[i] });
        }
        std::ranges::sort(propertyCountsOfThis);
        Vector<uint32_t> functionsAndPropertyCountsOfThis;
        for (auto [function, count] : propertyCountsOfThis) {
            functionsAndPropertyCountsOfThis.append(function);
            functionsAndPropertyCountsOfThis.append(count);
        }
        header.numberOfPropertyCountsOfThis = propertyCountsOfThis.size();
        header.propertyCountsOfThisOffset = append(functionsAndPropertyCountsOfThis);
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
        header.numberOfWordsOfSites = m_program.sites.size();
        header.sitesOffset = append(m_program.sites);
        header.countsAllocations = Options::countAOTTypedAllocations();
        header.usesWideReferences = Options::useAOTWideReferences();
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
            for (auto& [function, entrypoint] : m_program.genericEntrypoints)
                owner[entrypoint.index()] = owner[function];
            Vector<uint32_t> runsOfAliases;
            Vector<uint16_t> lowHalvesOfOwners;
            Vector<uint8_t> highBytesOfOwners;
            RELEASE_ASSERT(!(owner.size() >> 24));
            unsigned behindLastAlias = 0;
            for (unsigned i = 0; i < owner.size(); ++i) {
                if (owner[i] == i)
                    continue;
                if (runsOfAliases.isEmpty() || i != behindLastAlias)
                    runsOfAliases.appendList({ static_cast<uint32_t>(i), static_cast<uint32_t>(highBytesOfOwners.size()) });
                behindLastAlias = i + 1;
                lowHalvesOfOwners.append(static_cast<uint16_t>(owner[i]));
                highBytesOfOwners.append(static_cast<uint8_t>(owner[i] >> 16));
            }
            header.numberOfFunctionAliases = highBytesOfOwners.size();
            header.numberOfRunsOfFunctionAliases = runsOfAliases.size() / 2;
            Vector<uint8_t> aliases;
            aliases.append(asBytes(runsOfAliases.span()));
            aliases.append(asBytes(lowHalvesOfOwners.span()));
            aliases.appendVector(highBytesOfOwners);
            header.functionAliasesOffset = append(aliases);
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
                    return field.flags & fieldIsNeverSeenAbsentAsWritten;
                });
                found.append(function << 2 | (layout.mayHaveOtherPrototype ? Data::makesObjectsOfFlaggedLayout : 0) | (hasDefinitelyInitializedField ? Data::makesObjectsWithDefinitelyInitializedFields : 0));
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
        header.numberOfMakers = makers.size();
        header.makersOffset = append(makers);
        header.numberOfAllocationsByConstructor = allocationsByConstructor.size();
        header.allocationsByConstructorOffset = append(allocationsByConstructor);
        header.functionTextsOffset = appendFunctionTexts();
        at<Data>(0) = header;
        {
            const Data& data = at<Data>(0);
            auto classWithIndex = [&](uint32_t index) -> LayoutData* {
                uint32_t offset = data.at<uint32_t>(data.classesOffset)[index];
                return offset ? &at<LayoutData>(offset) : nullptr;
            };
            auto baseOf = [&](const LayoutData& layout) -> LayoutData* {
                return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base());
            };
            auto isFoundByNoName = [&](auto& method) {
                return method.flags & methodHasComputedName || parseIndex(m_program.string(StringID(method.name)));
            };
            auto isAskedOfTheFunction = [&](auto& method) {
                auto name = m_program.string(StringID(method.name));
                return !(method.flags & methodHasComputedName) && (equalSpans(name, "name"_s.span8()) || equalSpans(name, "length"_s.span8()));
            };
            for (uint32_t index = 0; index < data.numberOfLayoutIndices; ++index) {
                LayoutData* ofClass = classWithIndex(index);
                if (!ofClass)
                    continue;
                ofClass->makesMembersWhenLookedAt = ofClass->isEvaluatedOnce && std::ranges::none_of(ofClass->methods(), isFoundByNoName);
                bool isAhead = ofClass->makesMembersWhenLookedAt;
                for (const LayoutData* next = ofClass; next; next = baseOf(*next))
                    isAhead &= next->isEvaluatedOnce && !next->hasForeignRoot;
                ofClass->hasPrototypeAheadOfClassObject = isAhead;
                bool isMadeWhenChanged = ofClass->makesMembersWhenLookedAt && std::ranges::none_of(ofClass->staticMethods(), isFoundByNoName) && std::ranges::none_of(ofClass->staticMethods(), isAskedOfTheFunction);
                for (const LayoutData* next = baseOf(*ofClass); next; next = baseOf(*next))
                    isMadeWhenChanged &= next->isEvaluatedOnce;
                ofClass->makesClassObjectWhenChanged = isMadeWhenChanged;
            }
        }
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT data: ", m_result.size(), " bytes, of which ", sizeBeforeLayouts - sizeBeforeTypes, " for ", m_offsetOfType.size(), " types, ", sizeAfterLayouts - sizeBeforeLayouts, " for ", m_program.numberOfShapes, " layouts and ", allocations.size(), " allocations, ", shapeSets.sizeInBytes(), " for shape sets");
        return WTF::move(m_result);
    }

private:
    uint32_t appendFunctionTexts()
    {
        Vector<std::array<uint32_t, 2>> values;
        for (auto& [function, entrypoint] : m_program.genericEntrypoints)
            values.append({ entrypoint.index(), function });
        std::ranges::sort(values);
        Vector<std::array<uint32_t, 3>> entries;
        Vector<uint32_t> paths;
        Vector<uint8_t> text;
        Vector<uint32_t> placeOfModule(m_program.modules.size(), [](size_t) {
            return UINT32_MAX;
        });
        Vector<uint32_t> without;
        for (auto [entrypoint, number] : values) {
            const Function& function = m_program.functions[number];
            if (!function.module || function.kind == FunctionKind::Bound || function.declaredAs & functionIsOfBuiltinModule || function.has(Format::FunctionIsBuiltin))
                continue;
            if (!function.placeOfText && !function.endOfText) {
                if (!function.has(Format::FunctionIsHiddenInStackTraces))
                    without.append(number);
                continue;
            }
            const Module& module = m_program.modules[function.module.index()];
            uint32_t& place = placeOfModule[function.module.index()];
            if (function.endOfText) {
                if (place == UINT32_MAX) {
                    place = safeCast<uint32_t>(text.size());
                    text.append(module.printed);
                }
                entries.append({ entrypoint, place + function.placeOfText, place + function.endOfText });
                continue;
            }
            if (place == UINT32_MAX) {
                auto path = m_program.string(module.path);
                place = paths.size() / 2;
                paths.appendList({ append(path), safeCast<uint32_t>(path.size()) });
            }
            entries.append({ entrypoint, place, function.placeOfText - 1 });
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

    // Cleared, so that the bytes a structure leaves unused between its members are the same in every build.
    uint32_t allocate(size_t size)
    {
        uint32_t offset = WTF::roundUpToMultipleOf<8>(m_result.size());
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

    uint32_t append(std::span<const uint8_t> bytes)
    {
        auto& withHash = m_copies.add(hashOf(bytes), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
        for (auto [offset, size] : withHash) {
            if (size == bytes.size() && equalSpans(m_result.span().subspan(offset, size), bytes))
                return offset;
        }
        uint32_t offset = allocate(bytes.size());
        memcpySpan(m_result.mutableSpan().subspan(offset), bytes);
        withHash.append({ offset, safeCast<uint32_t>(bytes.size()) });
        return offset;
    }

    template<typename T>
    uint32_t append(const Vector<T>& vector) { return append(asBytes(vector.span())); }

    // A closure goes by the number of the general entry of its function.
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
        Vector<std::pair<uint32_t, LayoutID>> viewsWithoutLayoutData;
        Vector<uint32_t> offsetOfView(FillWith { }, m_program.layouts.size(), 0);
        UncheckedKeyHashMap<uint64_t, Vector<std::pair<Vector<uint64_t, 16>, uint32_t>, 1>> typesThatSay;
        for (unsigned i = 0; i < m_program.types.size(); ++i) {
            auto& type = m_program.at(TypeID(i));
            // What an array holds is interned before the array.
            RELEASE_ASSERT(!type.elements || type.elements.index() < i);
            for (LayoutID id : type.views) {
                if (offsetOfView[id.index()])
                    continue;
                auto& view = m_program.at(id);
                uint32_t test = offsetOfView[id.index()] = allocate(sizeof(RuntimeShapeTest));
                at<RuntimeShapeTest>(test) = RuntimeShapeTest { view.test.first, view.test.count, view.test.set, isObjectLayout(view.kind) ? view.shape : 0u, (view.name ? view.name : m_program.emptyString).index(), 0, view.declaredPath ? view.declaredPath.index() : UINT32_MAX, view.declaredLine };
                if (isObjectLayout(view.kind) && !view.shape)
                    viewsWithoutLayoutData.append({ test, id });
            }
            Vector<uint64_t, 16> said { type.primitives, type.anyObject, type.anyFunction, type.admitsForeignObjects(m_program), static_cast<uint64_t>(type.integers.least), static_cast<uint64_t>(type.integers.most), type.views.size(), type.elements ? m_offsetOfType[type.elements.index()] : 0 };
            for (LayoutID id : type.views)
                said.append(offsetOfView[id.index()]);
            for (uint16_t index : type.engineClasses)
                said.append(classes[index].type);
            auto& typesWithHash = typesThatSay.add(hashOf(asBytes(said.span())), Vector<std::pair<Vector<uint64_t, 16>, uint32_t>, 1> { }).iterator->value;
            if (size_t same = typesWithHash.findIf([&](auto& entry) { return entry.first == said; }); same != notFound) {
                m_offsetOfType.append(typesWithHash[same].second);
                continue;
            }
            int64_t least = type.integers.least;
            int64_t most = type.integers.most;
            bool hasNoInteger = least == 1 && !most;
            bool hasOneInteger = least == most && least == static_cast<int32_t>(least);
            Vector<uint32_t, 5> rest;
            if (type.elements)
                rest.append(m_offsetOfType[type.elements.index()]);
            if (hasOneInteger)
                rest.append(static_cast<uint32_t>(least));
            else if (!hasNoInteger)
                rest.appendList({ static_cast<uint32_t>(least), static_cast<uint32_t>(static_cast<uint64_t>(least) >> 32), static_cast<uint32_t>(most), static_cast<uint32_t>(static_cast<uint64_t>(most) >> 32) });
            RELEASE_ASSERT(type.views.size() <= UINT16_MAX && type.engineClasses.size() <= UINT8_MAX);
            uint32_t offset = allocateWords(sizeof(RuntimeType) + type.views.size() * sizeof(int32_t) + type.engineClasses.size() * sizeof(uint32_t) + rest.sizeInBytes());
            typesWithHash.append({ said, offset });
            auto& runtimeType = at<RuntimeType>(offset);
            runtimeType.primitives = type.primitives;
            runtimeType.bits = (type.anyObject ? RuntimeType::allowsAnyObjectBit : 0) | (type.anyFunction ? RuntimeType::allowsAnyFunctionBit : 0) | (type.admitsForeignObjects(m_program) ? RuntimeType::admitsForeignObjectsBit : 0)
                | (type.elements ? RuntimeType::hasElementsBit : 0) | (hasOneInteger ? RuntimeType::hasOneIntegerBit : hasNoInteger ? 0 : RuntimeType::hasIntegersBit);
            runtimeType.viewsCount = type.views.size();
            runtimeType.jsTypesCount = type.engineClasses.size();
            uint32_t next = offset + sizeof(RuntimeType);
            for (LayoutID id : type.views) {
                at<int32_t>(next) = static_cast<int32_t>(offsetOfView[id.index()]) - static_cast<int32_t>(offset);
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
            m_offsetOfType.append(offset);
        }
        // ViewFields, once per view. They name types, so they come after all types.
        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> offsetOfFields;
        for (auto [test, id] : viewsWithoutLayoutData) {
            uint32_t offset = offsetOfFields.ensure(id.index(), [&] {
                Vector<uint8_t> bytes;
                auto isListed = [&](auto& field) {
                    return exists(field) && field.name;
                };
                ViewFields::append(bytes, std::ranges::count_if(m_program.at(id).fields, isListed));
                for (auto& field : m_program.at(id).fields) {
                    if (!isListed(field))
                        continue;
                    RELEASE_ASSERT(!(field.name.index() >> 31));
                    ViewFields::append(bytes, field.name.index() << 1 | (field.flags & fieldIsOptional ? 1 : 0));
                    ViewFields::append(bytes, (field.type ? field.type : m_program.types.anything).index());
                    ViewFields::append(bytes, field.typeText.index() + 1);
                }
                return append(bytes);
            }).iterator->value;
            at<RuntimeShapeTest>(test).fields = offset;
        }
    }

    uint32_t appendLayout(LayoutID id)
    {
        auto& layout = m_program.at(id);
        // By field: its number among those that exist, which are the fields the run time knows.
        Vector<uint32_t> numberOfField;
        uint32_t numberOfFields = 0;
        bool isObject = isObjectLayout(layout.kind);
        auto isDescribed = [&](unsigned i) {
            auto& field = layout.fields[i];
            if (!exists(field))
                return false;
            if (layout.isCoroutineFrame)
                return i < CoroutineFrame::firstArgumentField;
            return isObject || holdsCells(field.store) || !isAbsentAsZero(field.store);
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i)
            numberOfField.append(isDescribed(i) ? numberOfFields++ : UINT32_MAX);
        Vector<uint32_t> orders;
        for (auto& order : layout.orders) {
            Vector<uint16_t> list;
            list.append(static_cast<uint16_t>(order.size()));
            for (uint16_t field : order)
                list.append(static_cast<uint16_t>(numberOfField[field]));
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
                literals.append(LiteralData { !literal.string, literal.string ? literal.string.index() : 0, literal.number });
            literalsOffsets.append(literals.isEmpty() || !isObjectLayout(layout.kind) ? 0 : append(literals));
        }
        Vector<uint32_t> typeTexts;
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            if (isObject && isDescribed(i))
                typeTexts.append(layout.fields[i].typeText.index());
        }
        uint32_t typeTextsOffset = typeTexts.isEmpty() ? 0 : append(typeTexts);
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
                fieldsByName.append({ (layout.fields[i].name ? layout.fields[i].name : m_program.emptyString).index(), static_cast<uint16_t>(numberOfField[i]) });
        }
        std::ranges::sort(fieldsByName);
        uint32_t fieldsByNameOffset = fieldsByName.isEmpty() ? 0 : append(fieldsByName.map([](auto& entry) {
            return entry.second;
        }));
        struct Variable {
            StringID name;
            uint32_t number;
            TypeID typeOfStaticField;
        };
        Vector<Variable> variables;
        for (auto& field : layout.staticFields) {
            if (field.type)
                variables.append({ field.name, 0, field.type });
        }
        unsigned numberOfStaticFields = variables.size();
        Vector<MethodData> methods;
        // What a Record lists is for the compiler alone: see Layout::methods.
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
                }
                methods.append(MethodData { name, entrypointOf(method.function), static_cast<uint32_t>(method.kind), flags });
            }
        }

        // See LayoutData::referencesOffset.
        Vector<uint32_t> boxedSlots;
        Vector<uint32_t> cellSlots;
        Vector<uint32_t> narrowSlots;
        for (auto& field : layout.fields) {
            if (exists(field) && holdsCells(field.store) && !field.isOutOfLine)
                (sizeOf(field.store) == 4 ? narrowSlots : field.store == Store::JSValue ? boxedSlots : cellSlots).append(field.offset);
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
            uint32_t offset = allocate(sizeof(LayoutData));
            auto& data = at<LayoutData>(offset);
            data.kind = layout.kind;
            data.index = id.index();
            data.shape = layout.shape;
            data.name = (layout.name ? layout.name : m_program.emptyString).index();
            data.size = layout.size;
            data.numberOfDescribedVariables = numberOfFields;
            data.referencesOffset = referencesOffset ? static_cast<int32_t>(referencesOffset) - static_cast<int32_t>(offset) : 0;
            RELEASE_ASSERT(!layout.hasExtension && layout.sizeWithRoomToSpecialize == layout.size && orders.isEmpty() && variables.isEmpty() && methods.isEmpty());
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
        Vector<uint32_t> typesOfFields;
        auto appendTypesOfField = [&](TypeID stored, TypeID own, uint32_t numberOfLiterals, uint32_t literalsOffset) {
            typesOfFields.append(append(Vector<uint32_t> { m_offsetOfType[stored.index()], m_offsetOfType[(own ? own : m_program.types.anything).index()], numberOfLiterals, literalsOffset }));
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            if (isDescribed(i))
                appendTypesOfField(layout.fields[i].stored, layout.fields[i].type, layout.fields[i].literals.size(), literalsOffsets[i]);
        }
        for (auto [name, number, typeOfStaticField] : variables) {
            if (typeOfStaticField)
                appendTypesOfField(typeOfStaticField, typeOfStaticField, 0, 0);
            else if (layout.environment)
                appendTypesOfField(m_program.at(layout.environment).fields[number].stored, m_program.at(layout.environment).fields[number].type, 0, 0);
            else
                appendTypesOfField(m_program.variables[number].type ? m_program.variables[number].type : m_program.types.anything, m_program.variables[number].type, 0, 0);
        }
        uint32_t offset = allocate((isClass ? sizeof(ClassData) : 0) + sizeof(LayoutData) + (numberOfFields + variables.size()) * sizeof(FieldData) + methods.sizeInBytes()) + (isClass ? sizeof(ClassData) : 0);
        auto& data = at<LayoutData>(offset);
        data.kind = layout.kind;
        data.index = id.index();
        data.shape = layout.shape;
        data.name = (layout.name ? layout.name : m_program.emptyString).index();
        data.declaredPath = layout.declaredPath ? layout.declaredPath.index() : UINT32_MAX;
        data.declaredLine = layout.declaredLine;
        data.size = layout.size;
        data.sizeWithRoomToSpecialize = layout.sizeWithRoomToSpecialize;
        data.mayBeFrozen = layout.mayBeFrozen;
        data.mayGiveUp = layout.mayGiveUp;
        data.isLaidOut = layout.isLaidOut;
        data.isEvaluatedOnce = layout.isEvaluatedOnce;
        data.hasForeignRoot = layout.hasForeignRoot;
        data.hasFixedMembers = layout.hasFixedMembers;
        data.mayHaveOtherPrototype = layout.mayHaveOtherPrototype;
        data.mayBeMadeByNew = layout.mayBeMadeByNew;
        data.constructorTakesNewTarget = layout.constructor && m_program.at(layout.constructor).has(Format::FunctionHasNewTarget);
        data.numberOfOrders = orders.size();
        data.ordersOffset = ordersOffset;
        data.numberOfSpecializations = specializations.size();
        data.specializationsOffset = specializationsOffset;
        data.numberOfFields = numberOfFields;
        data.typeTextsOffset = typeTextsOffset;
        data.literalsWithoutFieldOffset = literalsWithoutFieldOffset;
        data.fieldsByNameOffset = fieldsByNameOffset;
        data.discriminant = layout.discriminant == UINT16_MAX ? LayoutData::noBase : numberOfField[layout.discriminant];
        data.isAbstract = layout.isAbstract;
        data.referencesOffset = referencesOffset ? static_cast<int32_t>(referencesOffset) - static_cast<int32_t>(offset) : 0;
        data.hasExtension = layout.hasExtension;
        data.extensionSize = layout.extensionSize;
        if (isClass) {
            auto& ofClass = data.classData();
            ofClass.base = layout.base ? layout.base.index() : LayoutData::noBase;
            ofClass.classConstructor = entrypointOf(layout.constructor);
            ofClass.environment = layout.environment ? layout.environment.index() : LayoutData::noBase;
            ofClass.numberOfMethods = layout.methods.size();
            ofClass.numberOfStaticMethods = layout.staticMethods.size();
            ofClass.numberOfStaticFields = numberOfStaticFields;
            ofClass.numberOfVariables = variables.size();
            static_assert(Format::none == LayoutData::noBase);
            ofClass.firstStaticFieldPlace = layout.classObjectPlace;
        } else
            RELEASE_ASSERT(!layout.base && variables.isEmpty() && methods.isEmpty());
        uint32_t next = offset + sizeof(LayoutData);
        unsigned numberOfFieldsAppended = 0;
        auto appendField = [&](StringID name, uint32_t offsetOfField, Store store, uint32_t flags, TypeID stored) {
            uint16_t primitives = m_program.at(stored).primitives;
            auto& fieldData = at<FieldData>(next);
            RELEASE_ASSERT(offsetOfField < 1u << 24);
            fieldData.name = (name ? name : m_program.emptyString).index();
            fieldData.offset = offsetOfField;
            fieldData.storeBits = static_cast<uint8_t>(store);
            fieldData.flags = flags;
            if (primitives & Primitive::Undefined)
                fieldData.flags |= fieldAllowsUndefined;
            if (primitives & Primitive::Null)
                fieldData.flags |= fieldAllowsNull;
            fieldData.distanceOfTypes = static_cast<int32_t>(typesOfFields[numberOfFieldsAppended++]) - static_cast<int32_t>(next);
            next += sizeof(FieldData);
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            auto& field = layout.fields[i];
            if (!isDescribed(i))
                continue;
            uint32_t pinned = field.isPinned ? fieldIsPinned | field.slot.index() % numberOfGuardsOfPinnedFields << guardOfPinnedFieldShift : 0;
            appendField(field.name, field.offset, field.store, field.flags | (field.isOutOfLine ? fieldIsOutOfLine : 0) | pinned, field.stored);
        }
        memcpySpan(m_result.mutableSpan().subspan(next), asBytes(methods.span()));
        next += methods.sizeInBytes();
        for (unsigned i = 0; i < variables.size(); ++i) {
            auto [name, number, typeOfStaticField] = variables[i];
            if (typeOfStaticField)
                appendField(name, 0, Store::JSValue, fieldIsAnnotated, typeOfStaticField);
            else if (layout.environment) {
                auto& field = m_program.at(layout.environment).fields[number];
                appendField(name, field.offset, field.store, field.flags, field.stored);
            } else {
                auto& variable = m_program.variables[number];
                appendField(name, variable.offset, variable.store, 0, variable.type ? variable.type : m_program.types.anything);
            }
        }
        return offset;
    }

    const Program& m_program;
    Vector<uint8_t> m_result;
    // By TypeID.
    Vector<uint32_t> m_offsetOfType;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_copies;
};

} // anonymous namespace

void place(Program& program)
{
    Placement(program).run();
    numberRowsOfBirths(program);
}

uint8_t kindByteOf(const Program& program, const Function& function)
{
    return (function.isBodyOfSeveralSites ? functionKindOfBodiesOfSeveralSites : function.has(Format::FunctionIsCodeOfModule) ? static_cast<uint8_t>(FunctionKind::Module) : function.flags & Format::FunctionNameIsGiven && !function.suspensionPositions.isEmpty() ? functionKindOfResumeFunctionsNamedByOthers : static_cast<uint8_t>(function.kind)) | function.declaredAs | (program.isHiddenInStackTraces(function) ? functionIsHiddenInStackTraces : 0);
}

Vector<uint8_t> encodeData(const Program& program)
{
    return DataEncoder(program).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
