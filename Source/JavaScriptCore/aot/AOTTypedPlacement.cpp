/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedObject.h"
#include "Options.h"
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT { namespace Typed {

Rep resultRepOf(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    // Whoever calls a general entry knows nothing about it.
    if (function.isGenericEntrypoint() || !function.resultType)
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
        closure.size = WTF::roundUpToMultipleOf<16>(Closure::allocationSize());

        runStep("checkProgram"_s, &Placement::checkProgram);
        runStep("numberFields"_s, &Placement::numberFields);
        runStep("indexViews"_s, &Placement::indexViews);
        runStep("shareSlots"_s, &Placement::shareSlots);
        runStep("findLackedFields"_s, &Placement::findLackedFields);
        runStep("dropSpecializationsThatLackFields"_s, &Placement::dropSpecializationsThatLackFields);
        runStep("collectTypes"_s, &Placement::collectTypes);
        runStep("scanFunctions"_s, &Placement::scanFunctions);
        runStep("followSpecializations"_s, &Placement::followSpecializations);
        runStep("findSlotsThatMayBeAbsent"_s, &Placement::findSlotsThatMayBeAbsent);
        runStep("typeSlots"_s, &Placement::typeSlots);
        runStep("placeObjects"_s, &Placement::placeObjects);
        runStep("placeEnvironments"_s, &Placement::placeEnvironments);
        runStep("makeRoomToSpecialize"_s, &Placement::makeRoomToSpecialize);
        runStep("collectOrders"_s, &Placement::collectOrders);
        runStep("numberShapes"_s, &Placement::numberShapes);
        runStep("placeVariables"_s, &Placement::placeVariables);
        runStep("checkPlacement"_s, &Placement::checkPlacement);
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

    void runStep(ASCIILiteral name, void (Placement::*step)())
    {
        MonotonicTime start = MonotonicTime::now();
        (this->*step)();
        report(name, " took ", (MonotonicTime::now() - start).milliseconds(), " ms");
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
                ++firstFieldNamed[field.name.index() + 1];
        }
        m_firstField.append(next);
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
            for (unsigned k = 0; k < layouts[i].fields.size(); ++k)
                m_fieldsByName[firstFieldNamed[layouts[i].fields[k].name.index()]++] = NamedField { i, m_firstField[i] + k };
        }
    }

    Field& fieldAt(NamedField field) { return m_program.layouts[field.layout].fields[field.field - m_firstField[field.layout]]; }

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
        for (unsigned i = 0; i < fields.size(); ++i)
            fieldNamed[fields[i].name.index()] = i;
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

    // The members of the view that have a field of the name, in their order, by its place. Whichever is shorter is gone through: the members, or the fields of the name.
    void forEachMemberWithField(const FieldsOfOneName& table, std::span<const NamedField> fields, const Layout& view, const Invocable<void(uint32_t place)> auto& functor)
    {
        static constexpr unsigned costOfSearch = 16;
        if (fields.size() * costOfSearch < view.members.size()) {
            for (uint32_t place = 0; place < fields.size(); ++place) {
                if (std::ranges::binary_search(view.members, LayoutID(fields[place].layout)))
                    functor(place);
            }
            return;
        }
        for (LayoutID member : view.members) {
            if (uint32_t place = table.placeOfLayout[member.index()]; place != none)
                functor(place);
        }
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
                Fingerprint membersWithIt;
                forEachMemberWithField(table, fields, layouts[fields[place].layout], [&](uint32_t other) {
                    membersWithIt.add(m_hashOfLayout[fields[other].layout]);
                    other = representativeOf(other);
                    if (other == own)
                        return;
                    representative[std::max(own, other)] = std::min(own, other);
                    own = std::min(own, other);
                });
                m_membersWith[fields[place].field] = membersWithIt;
            }
            for (uint32_t place = 0; place < fields.size(); ++place)
                representativeOfField[fields[place].field] = fields[representativeOf(place)].field;
        });
        Vector<uint32_t> slotOfRepresentative(FillWith { }, numberOfFields, none);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& fields = layouts[i].fields;
            for (unsigned k = 0; k < fields.size(); ++k) {
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
        m_listedMembers.grow(numberOfMembers);
        forEachName([&](FieldsOfOneName& table, std::span<const NamedField> fields) {
            for (auto& field : fields) {
                uint32_t lacked = m_lackedFieldOfField[field.field];
                if (lacked == none || m_setsOfMembers[m_lackedFields[lacked].membersWithIt].listedFor != lacked)
                    continue;
                uint32_t next = m_setsOfMembers[m_lackedFields[lacked].membersWithIt].firstMember;
                forEachMemberWithField(table, fields, layouts[field.layout], [&](uint32_t place) {
                    m_listedMembers[next++] = fields[place].layout;
                });
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
                if (m_program.at(member).kind != LayoutKind::Record)
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
            withFieldsByName(fieldNamed, specialized[index], [&] {
                layout.specializations.removeAllMatching([&](LayoutID id) {
                    unsigned numberOfSharedFields = 0;
                    for (auto& field : m_program.at(id).fields) {
                        uint32_t found = fieldNamed[field.name.index()];
                        if (found == none)
                            continue;
                        RELEASE_ASSERT(field.slot == layout.fields[found].slot);
                        ++numberOfSharedFields;
                    }
                    return numberOfSharedFields != layout.fields.size();
                });
            });
        });
        uint64_t numberLeft = 0;
        for (uint32_t index : specialized)
            numberLeft += layouts[index].specializations.size();
        report(numberOfSpecializations, " specializations of ", specialized.size(), " layouts, of which ", numberOfSpecializations - numberLeft, " lack a field of the layout and are none");
    }

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
                    if (!m_coercionsToView[view] || !isRecord || (layouts[view].isAllocated && view != specialization.index()))
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
        report("casts turn objects of other layouts into members of ", numberOfViews, " views, of ", m_retagSources.size(), " layouts in all");
    }

    void collectTypes()
    {
        for (auto& layout : m_program.layouts) {
            for (auto& field : layout.fields) {
                TypeID type = field.type ? field.type : m_program.types.anything;
                auto& slot = m_program.at(field.slot);
                if (!slot.type)
                    slot.type = type;
                else if (slot.type != type) {
                    m_otherTypes.append({ field.slot.index(), type.index() });
                    m_holdsSeveralTypes.quickSet(field.slot.index());
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
        m_numberOfBirths.fill(0, layouts.size());
        m_numberOfBirthsInitializing.fill(0, m_firstField.last());
        Vector<uint32_t> lastBirthInitializing(FillWith { }, m_firstField.last(), none);
        uint32_t birth = 0;
        for (auto& function : m_program.functions) {
            for (auto& block : function.blocks) {
                for (size_t index = 0; index < block.values.size(); ++index) {
                    auto& value = function.at(block.values[index]);
                    switch (value.opcode) {
                    case Opcode::NewObjectWithShape: {
                        uint32_t layout = value.layout().index();
                        if (layouts[layout].kind != LayoutKind::Environment)
                            break;
                        ++m_numberOfBirths[layout];
                        ++birth;
                        for (size_t next = index + 1; next < block.values.size() && isInitializingStore(m_program, function, block, next); ++next) {
                            auto& store = function.at(block.values[next]);
                            if (store.opcode != Opcode::PutField || store.layout() != value.layout())
                                continue;
                            uint32_t field = m_firstField[layout] + store.field();
                            if (std::exchange(lastBirthInitializing[field], birth) != birth)
                                ++m_numberOfBirthsInitializing[field];
                        }
                        break;
                    }
                    case Opcode::GetById:
                    case Opcode::PutById:
                    case Opcode::InById:
                    case Opcode::CallMethod: {
                        const Field* field = nullptr;
                        if (LayoutID view = m_program.typeOf(function, value.child(0)).onlyView()) {
                            if (auto found = m_program.at(view).fieldNamed(value.string())) {
                                field = &m_program.at(view).fields[*found];
                                if (field->isLackedBySomeMember)
                                    ++m_lackedFields[m_lackedFieldOfField[m_firstField[view.index()] + *found]].numberOfAccesses;
                            }
                        }
                        if (value.opcode == Opcode::CallMethod || (value.opcode == Opcode::GetById && (!field || m_holdsSeveralTypes.quickGet(field->slot.index()))))
                            ++m_testsOfType[value.type.index()];
                        break;
                    }
                    case Opcode::Cast:
                        // It tests for what it can make something of, not for what comes of it.
                        if (value.castReason() == CastReason::Coerce) {
                            for (LayoutID view : m_program.at(value.coercedType()).views)
                                ++m_coercionsToView[view.index()];
                        } else if (value.castReason() != CastReason::Upcast)
                            ++m_testsOfType[value.type.index()];
                        break;
                    case Opcode::HasShapeOf:
                        // Otherwise for the one shape of the layout itself.
                        if (!value.immediates[1])
                            ++m_testsOfView[value.layout().index()];
                        break;
                    case Opcode::IsInstanceOfClass:
                        ++m_testsOfClass[value.layout().index()];
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
                        ++m_testsOfType[value.type.index()];
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }

    void findSlotsThatMayBeAbsent()
    {
        auto& layouts = m_program.layouts;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                auto& field = layout.fields[k];
                bool mayBeAbsent = field.flags & fieldIsOptional;
                // A captured variable holds nothing until it is initialized.
                if (layout.kind == LayoutKind::Environment)
                    mayBeAbsent = !layout.isCoroutineFrame && !(field.flags & (fieldIsImmutable | fieldIsNeverSeenUninitialized)) && (!m_numberOfBirths[i] || m_numberOfBirthsInitializing[m_firstField[i] + k] != m_numberOfBirths[i]);
                if (mayBeAbsent)
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
                if (lastAllocationBearing[k] != i)
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
                        if (m_fieldNamed[field.name.index()] == none)
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

    void typeSlots()
    {
        auto& slots = m_program.slots;
        std::ranges::sort(m_otherTypes);
        size_t next = 0;
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
            slot.store = storeOf(m_program.at(slot.type));
        }
        RELEASE_ASSERT(next == m_otherTypes.size());
        report(m_program.layouts.size(), " layouts, ", m_views.size(), " memberships; ", m_firstField.last(), " fields in ", slots.size(), " slots, of which ", m_holdsSeveralTypes.bitCount(), " hold what fields of several types say and ", m_mayBeAbsent.bitCount(), " may be absent; ", m_lackedFields.size(), " fields of views are lacked by some member, ", m_numberOfRequiredFieldsLacked, " of them required");
    }

    struct SlotUse {
        // The allocated layouts of objects that have the slot.
        uint32_t numberOfLayouts { 0 };
        uint32_t firstLayout { 0 };
        // The bytes the smallest of them needs. Placed at or beyond that, the slot makes its objects larger than they have to be.
        uint32_t deadline { UINT32_MAX };
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

    // Slots are placed greedily at the lowest offset free in every layout that has them, those first that the smallest objects have.
    void placeObjects()
    {
        auto& layouts = m_program.layouts;
        auto& slots = m_program.slots;
        auto hasObjects = [](const Layout& layout) {
            return isObjectLayout(layout.kind) && layout.isAllocated;
        };
        auto bytesNeededBy = [&](const Layout& layout) {
            uint32_t needed = offsetOfFirstField;
            for (auto& field : layout.fields)
                needed += sizeOf(m_program.at(field.slot).store);
            return needed;
        };

        Vector<SlotUse> uses(slots.size());
        Vector<Occupancy> occupancies(layouts.size());
        uint32_t numberOfUses = 0;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (!hasObjects(layout))
                continue;
            occupancies[i].words.append((1ull << offsetOfFirstField) - 1);
            uint32_t needed = bytesNeededBy(layout);
            for (auto& field : layout.fields) {
                auto& use = uses[field.slot.index()];
                ++use.numberOfLayouts;
                use.deadline = std::min(use.deadline, needed);
            }
            numberOfUses += layout.fields.size();
        }
        Vector<uint32_t> layoutsOfSlots(numberOfUses);
        uint32_t next = 0;
        for (auto& use : uses)
            use.firstLayout = std::exchange(next, next + use.numberOfLayouts);
        for (unsigned i = 0; i < layouts.size(); ++i) {
            if (!hasObjects(layouts[i]))
                continue;
            for (auto& field : layouts[i].fields)
                layoutsOfSlots[uses[field.slot.index()].firstLayout++] = i;
        }
        for (auto& use : uses)
            use.firstLayout -= use.numberOfLayouts;

        // A slot no object has is never accessed: no object passes for one of a view without members.
        Vector<uint32_t> order;
        for (unsigned i = 0; i < slots.size(); ++i) {
            slots[i].offset = offsetOfFirstField;
            if (uses[i].numberOfLayouts)
                order.append(i);
        }
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            if (uses[a].deadline != uses[b].deadline)
                return uses[a].deadline < uses[b].deadline;
            if (uses[a].numberOfLayouts != uses[b].numberOfLayouts)
                return uses[a].numberOfLayouts > uses[b].numberOfLayouts;
            if (sizeOf(slots[a].store) != sizeOf(slots[b].store))
                return sizeOf(slots[a].store) > sizeOf(slots[b].store);
            return a < b;
        });

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
            uint64_t room = 0;
            for (;; ++word) {
                uint64_t occupied = 0;
                for (uint32_t layout : layoutsOfSlot) {
                    auto& words = occupancies[layout].words;
                    if (word < words.size())
                        occupied |= words[word];
                }
                room = roomIn(occupied, logSize);
                if (room)
                    break;
            }
            unsigned bit = WTF::ctz(room);
            slot.offset = word * 64 + bit;
            for (uint32_t layout : layoutsOfSlot) {
                auto& words = occupancies[layout].words;
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
            uint32_t end = offsetOfFirstField;
            for (auto& field : layout.fields) {
                auto& slot = m_program.at(field.slot);
                field.stored = slot.type;
                field.store = slot.store;
                field.offset = slot.offset;
                end = std::max(end, slot.offset + sizeOf(slot.store));
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

    // An object goes by the first so many entries of its order. So an order stands in for every order it starts with.
    void collectOrders()
    {
        for (auto& allocation : m_program.allocations) {
            auto& layout = m_program.at(allocation.layout);
            if (!isObjectLayout(layout.kind) || allocation.born > maxFieldsInOrder)
                continue;
            bool isKnown = false;
            for (auto& order : layout.orders) {
                isKnown = startsWith(order, allocation.order);
                if (!isKnown && startsWith(allocation.order, order)) {
                    order = allocation.order;
                    isKnown = true;
                }
                if (isKnown)
                    break;
            }
            if (!isKnown)
                layout.orders.append(allocation.order);
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
            if (variable.isNeverSeenUninitialized)
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
        }
        Vector<unsigned> order(variables.size(), [](size_t index) {
            return static_cast<unsigned>(index);
        });
        std::ranges::stable_sort(order, [&](unsigned a, unsigned b) {
            return groupOf(variables[a]) < groupOf(variables[b]);
        });
        uint32_t offset = 0;
        for (unsigned i : order) {
            variables[i].offset = offset;
            offset += sizeOf(variables[i].store);
        }
        m_program.sizeOfStatics = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(offset);
        report(variables.size(), " variables in ", offset, " bytes");
    }

    // On what the lowering and the run time read, however it has come about.
    void checkPlacement()
    {
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
                uint32_t numberOfMembersWithIt = 0;
                forEachMemberWithField(table, fields, layouts[named.layout], [&](uint32_t place) {
                    auto& own = fieldAt(fields[place]);
                    RELEASE_ASSERT(own.slot == field.slot && own.offset == field.offset && own.store == field.store && own.stored == field.stored);
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
            for (unsigned k = 0; k < layout.fields.size(); ++k) {
                auto& field = layout.fields[k];
                auto& slot = m_program.at(field.slot);
                RELEASE_ASSERT(field.stored == slot.type && field.store == slot.store && field.offset == slot.offset && field.store == storeOf(m_program.at(field.stored)));
                RELEASE_ASSERT(!(field.offset % sizeOf(field.store)));
                RELEASE_ASSERT(m_program.at(field.type ? field.type : m_program.types.anything).isWithin(m_program.at(field.stored), m_program));
                if (field.isLackedBySomeMember)
                    checkNumberPassing(field.membersWithIt, m_membersWith[m_firstField[i] + k].count);
                extents.append({ field.offset, field.offset + sizeOf(field.store) });
            }
            if (!layout.isAllocated)
                continue;
            RELEASE_ASSERT(layout.shape && layout.shape <= m_program.numberOfShapes);
            std::ranges::sort(extents);
            uint32_t end = isObjectLayout(layout.kind) ? offsetOfFirstField : static_cast<uint32_t>(sizeof(JSCell));
            for (auto [first, last] : extents) {
                RELEASE_ASSERT(first >= end);
                end = last;
            }
            RELEASE_ASSERT(end <= layout.size && layout.size <= layout.sizeWithRoomToSpecialize);
            for (LayoutID specialization : layout.specializations)
                RELEASE_ASSERT(m_program.at(specialization).size <= layout.sizeWithRoomToSpecialize);
        }
        for (unsigned i = 0; i < layouts.size(); ++i) {
            checkNumberPassing(layouts[i].instances, numberOfInstanceLayouts[i]);
            checkNumberPassing(layouts[i].retagSources, retagSourcesOf(i).size());
            for (uint32_t source : retagSourcesOf(i))
                RELEASE_ASSERT(m_program.passes(layouts[i].retagSources, layouts[source].shape) && !m_program.passes(layouts[i].test, layouts[source].shape));
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
    // A slot, and the type of one of its fields that is not the type of the first.
    Vector<std::pair<uint32_t, uint32_t>> m_otherTypes;
    Vector<TypeID> m_typesWithAbsent;
    Vector<uint64_t> m_testsOfType;
    // By layout.
    Vector<uint64_t> m_testsOfView;
    Vector<uint64_t> m_testsOfClass;
    Vector<uint64_t> m_coercionsToView;
    // And one more.
    Vector<uint32_t> m_firstRetagSource;
    Vector<uint32_t> m_retagSources;
    Vector<uint32_t> m_numberOfBirths;
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
        Vector<uint32_t> pairs;
        for (auto string : m_program.strings) {
            pairs.append(append(string));
            pairs.append(string.size());
        }
        header.stringsOffset = append(pairs);

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
        Vector<uint8_t> extendedClasses;
        for (auto& engineClass : Builtins::singleton().classes())
            extendedClasses.append(m_program.isExtended(engineClass.name));
        header.extendedClassesOffset = append(extendedClasses);

        Vector<AllocationData> allocations;
        for (auto& allocation : m_program.allocations) {
            auto& layout = m_program.at(allocation.layout);
            uint32_t orderWord = 0;
            if (allocation.born > maxFieldsInOrder)
                orderWord = orderWordOf(keysAreInLayoutOrder, 0);
            else if (isObjectLayout(layout.kind)) {
                size_t order = layout.orders.findIf([&](auto& candidate) {
                    return startsWith(candidate, allocation.order);
                });
                RELEASE_ASSERT(order != notFound);
                orderWord = orderWordOf(static_cast<uint32_t>(order), allocation.born);
            }
            allocations.append(AllocationData { layout.shape, orderWord, layout.sizeWithRoomToSpecialize, entrypointOf(allocation.constructedBy) });
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
        Vector<uint32_t> roots;
        for (auto& variable : m_program.variables) {
            if (holdsCells(variable.store))
                roots.append(variable.offset);
        }
        header.numberOfRoots = roots.size();
        header.rootsOffset = append(roots);

        header.numberOfFunctions = m_program.functions.size();
        Vector<uint32_t> names;
        Vector<uint8_t> kinds;
        Vector<uint16_t> lengths;
        for (auto& function : m_program.functions) {
            names.append(function.name.index());
            static_assert(static_cast<uint8_t>(FunctionKind::Module) <= functionKindMask);
            kinds.append(static_cast<uint8_t>(function.kind) | function.declaredAs);
            lengths.append(static_cast<uint16_t>(function.length));
        }
        header.functionsOffset = append(names);
        header.functionKindsOffset = append(kinds);
        header.functionLengthsOffset = append(lengths);
        // One that is never a value has no closure to ask for its `prototype`.
        Vector<std::pair<uint32_t, uint32_t>> generatorFunctions;
        for (unsigned i = 0; i < m_program.functions.size(); ++i) {
            auto& function = m_program.functions[i];
            if (function.declaredAs & functionIsGenerator && function.resumeFunction && m_program.genericEntrypoints.contains(i))
                generatorFunctions.append({ entrypointOf(FunctionID(i)), entrypointOf(function.resumeFunction) });
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
        at<Data>(0) = header;
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT data: ", m_result.size(), " bytes, of which ", sizeBeforeLayouts - sizeBeforeTypes, " for ", m_offsetOfType.size(), " types, ", sizeAfterLayouts - sizeBeforeLayouts, " for ", m_program.numberOfShapes, " layouts and ", allocations.size(), " allocations, ", shapeSets.sizeInBytes(), " for shape sets");
        return WTF::move(m_result);
    }

private:
    // Cleared, so that the bytes a structure leaves unused between its members are the same in every build.
    uint32_t allocate(size_t size)
    {
        uint32_t offset = WTF::roundUpToMultipleOf<8>(m_result.size());
        m_result.insertFill(m_result.size(), 0, offset + size - m_result.size());
        return offset;
    }

    template<typename T>
    T& at(uint32_t offset) { return *reinterpret_cast<T*>(m_result.mutableSpan().subspan(offset, sizeof(T)).data()); }

    uint32_t append(std::span<const uint8_t> bytes)
    {
        uint32_t offset = allocate(bytes.size());
        memcpySpan(m_result.mutableSpan().subspan(offset), bytes);
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
        auto& classes = Builtins::singleton().classes();
        for (unsigned i = 0; i < m_program.types.size(); ++i) {
            auto& type = m_program.at(TypeID(i));
            // What an array holds is interned before the array.
            RELEASE_ASSERT(!type.elements || type.elements.index() < i);
            uint32_t offset = allocate(sizeof(RuntimeType) + type.views.size() * sizeof(RuntimeShapeTest) + type.engineClasses.size() * sizeof(uint32_t));
            auto& runtimeType = at<RuntimeType>(offset);
            runtimeType.primitives = type.primitives;
            runtimeType.allowsAnyObject = type.anyObject;
            runtimeType.allowsAnyFunction = type.anyFunction;
            runtimeType.admitsForeignObjects = type.admitsForeignObjects(m_program);
            runtimeType.minInteger = type.integers.least;
            runtimeType.maxInteger = type.integers.most;
            runtimeType.numberOfViews = type.views.size();
            runtimeType.numberOfJSTypes = type.engineClasses.size();
            runtimeType.elements = type.elements ? m_offsetOfType[type.elements.index()] : 0;
            uint32_t next = offset + sizeof(RuntimeType);
            for (LayoutID id : type.views) {
                auto& view = m_program.at(id);
                at<RuntimeShapeTest>(next) = RuntimeShapeTest { view.test.first, view.test.count, view.test.set, isObjectLayout(view.kind) ? view.shape : 0u };
                next += sizeof(RuntimeShapeTest);
            }
            for (uint16_t index : type.engineClasses) {
                at<uint32_t>(next) = classes[index].type;
                next += sizeof(uint32_t);
            }
            m_offsetOfType.append(offset);
        }
    }

    uint32_t appendLayout(LayoutID id)
    {
        auto& layout = m_program.at(id);
        Vector<uint32_t> orders;
        for (auto& order : layout.orders) {
            Vector<uint16_t> list;
            list.append(static_cast<uint16_t>(order.size()));
            list.appendVector(order);
            orders.append(append(list));
        }
        uint32_t ordersOffset = append(orders);
        Vector<uint32_t> specializations;
        for (LayoutID specialization : layout.specializations)
            specializations.append(m_program.at(specialization).shape);
        std::ranges::sort(specializations);
        uint32_t specializationsOffset = append(specializations);
        Vector<uint32_t> literalsOffsets;
        for (auto& field : layout.fields) {
            Vector<LiteralData> literals;
            for (auto& literal : field.literals)
                literals.append(LiteralData { !literal.string, literal.string ? literal.string.index() : 0, literal.number });
            literalsOffsets.append(literals.isEmpty() ? 0 : append(literals));
        }
        // The variables of a class that the run time has to do with: the static fields, then the computed names.
        Vector<std::pair<StringID, uint32_t>> variables;
        for (auto& field : layout.staticFields)
            variables.append({ field.name, field.variable });
        Vector<MethodData> methods;
        // What a Record lists is for the compiler alone: see Layout::methods.
        bool isClass = layout.kind == LayoutKind::Instance;
        for (auto* list : { &layout.methods, &layout.staticMethods }) {
            if (!isClass)
                break;
            for (auto& method : *list) {
                uint32_t flags = m_program.at(method.function).environment ? methodTakesEnvironment : 0;
                if (method.mayBeReplaced)
                    flags |= methodMayBeReplaced;
                uint32_t name = method.name.index();
                if (!method.name) {
                    flags |= methodHasComputedName;
                    name = variables.size();
                    variables.append({ m_program.emptyString, method.key });
                }
                methods.append(MethodData { name, entrypointOf(method.function), static_cast<uint32_t>(method.kind), flags });
            }
        }

        uint32_t offset = allocate(sizeof(LayoutData) + (layout.fields.size() + variables.size()) * sizeof(FieldData) + methods.sizeInBytes());
        auto& data = at<LayoutData>(offset);
        data.kind = layout.kind;
        data.index = id.index();
        data.shape = layout.shape;
        data.name = (layout.name ? layout.name : m_program.emptyString).index();
        data.size = layout.size;
        data.base = layout.base ? layout.base.index() : LayoutData::noBase;
        data.mayBeFrozen = layout.mayBeFrozen;
        data.isLaidOut = layout.isLaidOut;
        data.isEvaluatedOnce = layout.isEvaluatedOnce;
        data.hasForeignRoot = layout.hasForeignRoot;
        data.hasFixedMembers = layout.hasFixedMembers;
        data.mayHaveOtherPrototype = layout.mayHaveOtherPrototype;
        data.constructorTakesNewTarget = layout.constructor && m_program.at(layout.constructor).has(Format::FunctionHasNewTarget);
        data.numberOfOrders = orders.size();
        data.ordersOffset = ordersOffset;
        data.numberOfSpecializations = specializations.size();
        data.specializationsOffset = specializationsOffset;
        data.classConstructor = entrypointOf(layout.constructor);
        data.environment = layout.environment ? layout.environment.index() : LayoutData::noBase;
        data.numberOfFields = layout.fields.size();
        data.numberOfMethods = isClass ? layout.methods.size() : 0;
        data.numberOfStaticMethods = layout.staticMethods.size();
        data.numberOfStaticFields = layout.staticFields.size();
        data.numberOfVariables = variables.size();
        uint32_t next = offset + sizeof(LayoutData);
        auto appendField = [&](StringID name, uint32_t offsetOfField, Store store, uint32_t flags, TypeID stored, TypeID own) {
            uint16_t primitives = m_program.at(stored).primitives;
            auto& fieldData = at<FieldData>(next);
            fieldData.name = (name ? name : m_program.emptyString).index();
            fieldData.offset = offsetOfField;
            fieldData.store = store;
            fieldData.flags = flags;
            if (primitives & Primitive::Undefined)
                fieldData.flags |= fieldAllowsUndefined;
            if (primitives & Primitive::Null)
                fieldData.flags |= fieldAllowsNull;
            fieldData.type = m_offsetOfType[stored.index()];
            fieldData.ownType = m_offsetOfType[(own ? own : m_program.types.anything).index()];
            next += sizeof(FieldData);
        };
        for (unsigned i = 0; i < layout.fields.size(); ++i) {
            auto& field = layout.fields[i];
            auto& fieldData = at<FieldData>(next);
            fieldData.numberOfLiterals = field.literals.size();
            fieldData.literalsOffset = literalsOffsets[i];
            appendField(field.name, field.offset, field.store, field.flags, field.stored, field.type);
        }
        memcpySpan(m_result.mutableSpan().subspan(next), asBytes(methods.span()));
        next += methods.sizeInBytes();
        for (unsigned i = 0; i < variables.size(); ++i) {
            auto [name, number] = variables[i];
            if (layout.environment) {
                auto& field = m_program.at(layout.environment).fields[number];
                appendField(name, field.offset, field.store, field.flags, field.stored, field.type);
            } else {
                auto& variable = m_program.variables[number];
                bool isAnnotated = i < layout.staticFields.size() && layout.staticFields[i].isAnnotated;
                appendField(name, variable.offset, variable.store, isAnnotated ? fieldIsAnnotated : 0, variable.type ? variable.type : m_program.types.anything, variable.type);
            }
        }
        return offset;
    }

    const Program& m_program;
    Vector<uint8_t> m_result;
    // By TypeID.
    Vector<uint32_t> m_offsetOfType;
};

} // anonymous namespace

void place(Program& program)
{
    Placement(program).run();
}

Vector<uint8_t> encodeData(const Program& program)
{
    return DataEncoder(program).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
