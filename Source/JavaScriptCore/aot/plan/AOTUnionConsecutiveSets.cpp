/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnionConsecutiveSets.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTSort.h"
#include <algorithm>
#include <array>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct ConsecutiveSets::Tree {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Tree);
    WTF_MAKE_NONCOPYABLE(Tree);

    explicit Tree(uint32_t numberOfLeaves);

    bool tryKeepTogether(std::span<const uint32_t> leaves);
    bool tryKeepNestedSetTogether(std::span<const uint32_t> leaves);
    Vector<uint32_t> order(std::span<const uint64_t> keyOfLeaf) const;

private:
    static constexpr uint32_t none = UINT32_MAX;

    enum class Kind : uint8_t { Leaf, P, Q, AbsorbedQ };
    enum class Label : uint8_t { Empty, Partial, Full };

    struct Node {
        Kind kind { Kind::Leaf };
        uint32_t parent { none };
        std::array<uint32_t, 2> neighbors { none, none };
        std::array<uint32_t, 2> endChildren { none, none };
        uint32_t numberOfChildren { 0 };

        uint32_t epoch { 0 };
        Label label { Label::Empty };
        uint8_t numberOfPartialChildren { 0 };
        std::array<uint32_t, 2> partialChildren { none, none };
        uint32_t numberOfPertinentChildrenToLabel { 0 };
        uint32_t numberOfPertinentLeaves { 0 };
        uint32_t numberOfFullChildren { 0 };
        uint32_t firstFullChild { none };
        uint32_t nextFullSibling { none };
        std::array<uint32_t, 2> endsOfFullRun { none, none };
        std::array<uint32_t, 2> neighborsOfFullRun { none, none };
    };

    struct PartialRow {
        uint32_t emptyEnd { none };
        uint32_t fullEnd { none };
    };

    Node& at(uint32_t node) { return m_nodes[node]; }
    const Node& at(uint32_t node) const { return m_nodes[node]; }

    bool hasLabel(uint32_t node, Label label) const { return at(node).epoch == m_epoch && at(node).label == label; }
    bool isNeighborOf(uint32_t node, uint32_t other) const { return at(node).neighbors[0] == other || at(node).neighbors[1] == other; }
    uint32_t otherNeighbor(uint32_t node, uint32_t neighbor) const { return at(node).neighbors[at(node).neighbors[0] == neighbor]; }
    uint32_t otherEndChild(uint32_t node, uint32_t endChild) const { return at(node).endChildren[at(node).endChildren[0] == endChild]; }

    void forEachChild(uint32_t node, const auto& functor) const
    {
        uint32_t previous = none;
        for (uint32_t child = at(node).endChildren[0]; child != none;) {
            functor(child);
            previous = std::exchange(child, otherNeighbor(child, previous));
        }
    }

    uint32_t addNode(Kind);
    uint32_t parentOf(uint32_t node);

    void replaceNeighbor(uint32_t node, uint32_t from, uint32_t to);
    void replaceEndChild(uint32_t parent, uint32_t from, uint32_t to);
    void join(uint32_t, uint32_t);
    void appendChildToP(uint32_t parent, uint32_t child);
    void removeChildFromP(uint32_t child);
    void adoptUnlinked(uint32_t q, uint32_t child);
    void replaceInParent(uint32_t node, uint32_t replacement);

    void beginVisiting(uint32_t node);
    uint32_t markNodes(std::span<const uint32_t> nodes);
    void countPertinentChildren();
    uint32_t labelPertinentNodes(uint32_t numberOfNodesInSet);
    bool tryLabel(uint32_t node, bool isPertinentRoot);

    uint32_t takeFullChildrenOfP(uint32_t node);
    PartialRow flatten(uint32_t node, uint32_t q);
    void reducePertinentRootP(uint32_t root);
    uint32_t findNestedSetOf(uint32_t leaf);
    void reducePertinentRootQ(uint32_t root);

    struct NestedSet {
        uint32_t parent { none };
        uint32_t node { none };
        uint32_t numberOfLeaves { 1 };
        uint32_t lastEnclosingSet { 0 };
    };

    Vector<Node> m_nodes;
    Vector<uint32_t> m_queue;
    Vector<NestedSet> m_nestedSets;
    Vector<uint32_t> m_nestedSetsInside;
    uint32_t m_numberOfNestedSets { 0 };
    uint32_t m_numberOfLeaves { 0 };
    uint32_t m_root { none };
    uint32_t m_epoch { 0 };
};

ConsecutiveSets::Tree::Tree(uint32_t numberOfLeaves)
    : m_nodes(numberOfLeaves)
    , m_numberOfLeaves(numberOfLeaves)
{
    if (!numberOfLeaves)
        return;
    m_root = 0;
    if (numberOfLeaves == 1)
        return;
    m_root = addNode(Kind::P);
    for (uint32_t leaf = 0; leaf < numberOfLeaves; ++leaf)
        appendChildToP(m_root, leaf);
}

uint32_t ConsecutiveSets::Tree::addNode(Kind kind)
{
    uint32_t node = static_cast<uint32_t>(m_nodes.size());
    RELEASE_ASSERT(node != none);
    m_nodes.append(Node { .kind = kind });
    return node;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t ConsecutiveSets::Tree::parentOf(uint32_t node)
{
    uint32_t parent = at(node).parent;
    if (parent == none || at(parent).kind != Kind::AbsorbedQ)
        return parent;
    uint32_t survivor = parent;
    while (at(survivor).kind == Kind::AbsorbedQ)
        survivor = at(survivor).parent;
    while (parent != survivor)
        parent = std::exchange(at(parent).parent, survivor);
    at(node).parent = survivor;
    return survivor;
}

void ConsecutiveSets::Tree::replaceNeighbor(uint32_t node, uint32_t from, uint32_t to)
{
    ASSERT(isNeighborOf(node, from));
    at(node).neighbors[at(node).neighbors[0] != from] = to;
}

void ConsecutiveSets::Tree::replaceEndChild(uint32_t parent, uint32_t from, uint32_t to)
{
    ASSERT(at(parent).endChildren[0] == from || at(parent).endChildren[1] == from);
    at(parent).endChildren[at(parent).endChildren[0] != from] = to;
}

void ConsecutiveSets::Tree::join(uint32_t a, uint32_t b)
{
    replaceNeighbor(a, none, b);
    replaceNeighbor(b, none, a);
}

void ConsecutiveSets::Tree::appendChildToP(uint32_t parent, uint32_t child)
{
    ASSERT(at(parent).kind == Kind::P);
    at(child).parent = parent;
    at(child).neighbors = { none, none };
    uint32_t last = at(parent).endChildren[1];
    if (last == none)
        at(parent).endChildren[0] = child;
    else
        join(last, child);
    at(parent).endChildren[1] = child;
    ++at(parent).numberOfChildren;
}

void ConsecutiveSets::Tree::removeChildFromP(uint32_t child)
{
    uint32_t parent = at(child).parent;
    ASSERT(at(parent).kind == Kind::P);
    std::array<uint32_t, 2> neighbors = at(child).neighbors;
    for (unsigned side = 0; side < 2; ++side) {
        if (neighbors[side] != none)
            replaceNeighbor(neighbors[side], child, neighbors[!side]);
        else
            replaceEndChild(parent, child, neighbors[!side]);
    }
    --at(parent).numberOfChildren;
}

void ConsecutiveSets::Tree::adoptUnlinked(uint32_t q, uint32_t child)
{
    at(child).parent = q;
    at(child).neighbors = { none, none };
}

void ConsecutiveSets::Tree::replaceInParent(uint32_t node, uint32_t replacement)
{
    uint32_t parent = parentOf(node);
    std::array<uint32_t, 2> neighbors = at(node).neighbors;
    at(replacement).parent = parent;
    at(replacement).neighbors = neighbors;
    if (parent == none) {
        m_root = replacement;
        return;
    }
    for (uint32_t neighbor : neighbors) {
        if (neighbor != none)
            replaceNeighbor(neighbor, node, replacement);
        else
            replaceEndChild(parent, node, replacement);
    }
}

void ConsecutiveSets::Tree::beginVisiting(uint32_t index)
{
    Node& node = at(index);
    node.epoch = m_epoch;
    node.label = Label::Empty;
    node.numberOfPartialChildren = 0;
    node.numberOfPertinentChildrenToLabel = 0;
    node.numberOfPertinentLeaves = 0;
    node.numberOfFullChildren = 0;
    node.firstFullChild = none;
}

uint32_t ConsecutiveSets::Tree::markNodes(std::span<const uint32_t> nodes)
{
    ++m_epoch;
    RELEASE_ASSERT(m_epoch);
    m_queue.shrink(0);
    for (uint32_t node : nodes) {
        RELEASE_ASSERT(node < m_nodes.size());
        if (at(node).epoch == m_epoch)
            continue;
        beginVisiting(node);
        at(node).label = Label::Full;
        at(node).numberOfPertinentLeaves = 1;
        m_queue.append(node);
    }
    return static_cast<uint32_t>(m_queue.size());
}

void ConsecutiveSets::Tree::countPertinentChildren()
{
    bool hasPassedRoot = false;
    for (size_t head = 0; m_queue.size() - head + hasPassedRoot > 1; ++head) {
        uint32_t parent = parentOf(m_queue[head]);
        if (parent == none) {
            hasPassedRoot = true;
            continue;
        }
        if (at(parent).epoch != m_epoch) {
            beginVisiting(parent);
            m_queue.append(parent);
        }
        ++at(parent).numberOfPertinentChildrenToLabel;
    }
}

uint32_t ConsecutiveSets::Tree::labelPertinentNodes(uint32_t numberOfNodesInSet)
{
    m_queue.shrink(numberOfNodesInSet);
    for (size_t head = 0; head < m_queue.size(); ++head) {
        uint32_t node = m_queue[head];
        bool isPertinentRoot = at(node).numberOfPertinentLeaves == numberOfNodesInSet;
        if (head >= numberOfNodesInSet && !tryLabel(node, isPertinentRoot))
            return none;
        if (isPertinentRoot)
            return node;
        uint32_t parentIndex = parentOf(node);
        Node& parent = at(parentIndex);
        ASSERT(parent.epoch == m_epoch);
        parent.numberOfPertinentLeaves += at(node).numberOfPertinentLeaves;
        if (at(node).label == Label::Full) {
            at(node).nextFullSibling = std::exchange(parent.firstFullChild, node);
            ++parent.numberOfFullChildren;
        } else {
            if (parent.numberOfPartialChildren == 2)
                return none;
            parent.partialChildren[parent.numberOfPartialChildren++] = node;
        }
        if (!--parent.numberOfPertinentChildrenToLabel)
            m_queue.append(parentIndex);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ConsecutiveSets::Tree::tryLabel(uint32_t index, bool isPertinentRoot)
{
    Node& node = at(index);
    if (node.numberOfPartialChildren > (isPertinentRoot ? 2 : 1))
        return false;
    node.label = Label::Partial;
    if (node.kind == Kind::P) {
        if (node.numberOfFullChildren == node.numberOfChildren)
            node.label = Label::Full;
        return true;
    }
    ASSERT(node.kind == Kind::Q);
    if (!node.numberOfFullChildren) {
        ASSERT(node.numberOfPartialChildren == (isPertinentRoot ? 2 : 1));
        return isNeighborOf(node.partialChildren[0], isPertinentRoot ? node.partialChildren[1] : none);
    }
    uint32_t lengthOfRun = 1;
    unsigned numberOfRowEnds = 0;
    unsigned numberOfPartialNeighbors = 0;
    for (unsigned side = 0; side < 2; ++side) {
        uint32_t child = node.firstFullChild;
        uint32_t next = at(child).neighbors[side];
        while (next != none && hasLabel(next, Label::Full)) {
            child = std::exchange(next, otherNeighbor(next, child));
            ++lengthOfRun;
        }
        node.endsOfFullRun[side] = child;
        node.neighborsOfFullRun[side] = next;
        if (next == none)
            ++numberOfRowEnds;
        else if (hasLabel(next, Label::Partial))
            ++numberOfPartialNeighbors;
    }
    if (lengthOfRun != node.numberOfFullChildren || numberOfPartialNeighbors != node.numberOfPartialChildren)
        return false;
    if (numberOfRowEnds == 2)
        node.label = Label::Full;
    return isPertinentRoot || numberOfRowEnds;
}

uint32_t ConsecutiveSets::Tree::takeFullChildrenOfP(uint32_t node)
{
    uint32_t first = at(node).firstFullChild;
    if (at(node).numberOfFullChildren < 2) {
        if (first != none)
            removeChildFromP(first);
        return first;
    }
    uint32_t group = addNode(Kind::P);
    for (uint32_t child = first; child != none; child = at(child).nextFullSibling) {
        removeChildFromP(child);
        appendChildToP(group, child);
    }
    return group;
}

auto ConsecutiveSets::Tree::flatten(uint32_t node, uint32_t q) -> PartialRow
{
    PartialRow outer;
    PartialRow inner;
    auto extend = [&](uint32_t PartialRow::* end, uint32_t outermost, uint32_t innermost) {
        if (inner.*end == none)
            outer.*end = outermost;
        else
            join(inner.*end, outermost);
        inner.*end = innermost;
    };
    while (node != none) {
        uint32_t partialChild = at(node).numberOfPartialChildren ? at(node).partialChildren[0] : none;
        if (at(node).kind == Kind::P) {
            uint32_t full = takeFullChildrenOfP(node);
            if (partialChild != none)
                removeChildFromP(partialChild);
            uint32_t empty = at(node).numberOfChildren < 2 ? at(node).endChildren[0] : node;
            if (empty != none) {
                adoptUnlinked(q, empty);
                extend(&PartialRow::emptyEnd, empty, empty);
            }
            if (full != none) {
                adoptUnlinked(q, full);
                extend(&PartialRow::fullEnd, full, full);
            }
        } else {
            ASSERT(at(node).kind == Kind::Q);
            uint32_t outermostPertinent = partialChild;
            uint32_t innermostFull = none;
            uint32_t innermostEmpty = none;
            if (at(node).numberOfFullChildren) {
                unsigned sideAtRowEnd = at(node).neighborsOfFullRun[0] != none;
                ASSERT(at(node).neighborsOfFullRun[sideAtRowEnd] == none);
                outermostPertinent = at(node).endsOfFullRun[sideAtRowEnd];
                innermostFull = at(node).endsOfFullRun[!sideAtRowEnd];
                innermostEmpty = at(node).neighborsOfFullRun[!sideAtRowEnd];
                replaceNeighbor(innermostFull, innermostEmpty, none);
                extend(&PartialRow::fullEnd, outermostPertinent, innermostFull);
            }
            if (partialChild != none)
                innermostEmpty = otherNeighbor(partialChild, innermostFull);
            if (innermostEmpty != none) {
                replaceNeighbor(innermostEmpty, partialChild != none ? partialChild : innermostFull, none);
                extend(&PartialRow::emptyEnd, otherEndChild(node, outermostPertinent), innermostEmpty);
            }
            at(node).kind = Kind::AbsorbedQ;
            at(node).parent = q;
        }
        node = partialChild;
    }
    ASSERT(inner.emptyEnd != none && inner.fullEnd != none);
    join(inner.emptyEnd, inner.fullEnd);
    return outer;
}

void ConsecutiveSets::Tree::reducePertinentRootP(uint32_t root)
{
    unsigned numberOfPartialChildren = at(root).numberOfPartialChildren;
    std::array<uint32_t, 2> partialChildren = at(root).partialChildren;
    uint32_t full = takeFullChildrenOfP(root);
    if (!numberOfPartialChildren) {
        ASSERT(full != none && at(root).numberOfChildren);
        appendChildToP(root, full);
        return;
    }
    uint32_t q = addNode(Kind::Q);
    removeChildFromP(partialChildren[0]);
    PartialRow row = flatten(partialChildren[0], q);
    if (full != none) {
        adoptUnlinked(q, full);
        join(row.fullEnd, full);
        row.fullEnd = full;
    }
    if (numberOfPartialChildren == 2) {
        removeChildFromP(partialChildren[1]);
        PartialRow other = flatten(partialChildren[1], q);
        join(row.fullEnd, other.fullEnd);
        row.fullEnd = other.emptyEnd;
    }
    at(q).endChildren = { row.emptyEnd, row.fullEnd };
    if (at(root).numberOfChildren)
        appendChildToP(root, q);
    else
        replaceInParent(root, q);
}

void ConsecutiveSets::Tree::reducePertinentRootQ(uint32_t root)
{
    uint32_t pertinentNeighbor = at(root).partialChildren[1];
    for (unsigned i = 0; i < at(root).numberOfPartialChildren; ++i) {
        uint32_t child = at(root).partialChildren[i];
        if (at(root).numberOfFullChildren)
            pertinentNeighbor = at(root).endsOfFullRun[at(root).neighborsOfFullRun[0] != child];
        uint32_t emptyNeighbor = otherNeighbor(child, pertinentNeighbor);
        replaceNeighbor(pertinentNeighbor, child, none);
        if (emptyNeighbor != none)
            replaceNeighbor(emptyNeighbor, child, none);
        PartialRow row = flatten(child, root);
        join(pertinentNeighbor, row.fullEnd);
        if (emptyNeighbor != none)
            join(emptyNeighbor, row.emptyEnd);
        else
            replaceEndChild(root, child, row.emptyEnd);
        pertinentNeighbor = row.fullEnd;
    }
}

bool ConsecutiveSets::Tree::tryKeepTogether(std::span<const uint32_t> leaves)
{
    if (leaves.size() < 2)
        return true;
    uint32_t numberOfLeavesInSet = markNodes(leaves);
    if (numberOfLeavesInSet < 2)
        return true;
    countPertinentChildren();
    uint32_t pertinentRoot = labelPertinentNodes(numberOfLeavesInSet);
    if (pertinentRoot == none)
        return false;
    if (at(pertinentRoot).label == Label::Full)
        return true;
    if (at(pertinentRoot).kind == Kind::P)
        reducePertinentRootP(pertinentRoot);
    else
        reducePertinentRootQ(pertinentRoot);
    return true;
}

uint32_t ConsecutiveSets::Tree::findNestedSetOf(uint32_t leaf)
{
    uint32_t set = leaf;
    while (m_nestedSets[set].parent != none)
        set = m_nestedSets[set].parent;
    while (leaf != set)
        leaf = std::exchange(m_nestedSets[leaf].parent, set);
    return set;
}

bool ConsecutiveSets::Tree::tryKeepNestedSetTogether(std::span<const uint32_t> leaves)
{
    if (leaves.size() < 2)
        return true;
    if (m_nestedSets.isEmpty()) {
        m_nestedSets.grow(m_numberOfLeaves);
        for (uint32_t leaf = 0; leaf < m_numberOfLeaves; ++leaf)
            m_nestedSets[leaf] = NestedSet { .node = leaf };
    }
    ++m_numberOfNestedSets;
    m_nestedSetsInside.shrink(0);
    size_t numberOfLeavesInside = 0;
    for (uint32_t leaf : leaves) {
        uint32_t set = findNestedSetOf(leaf);
        if (std::exchange(m_nestedSets[set].lastEnclosingSet, m_numberOfNestedSets) == m_numberOfNestedSets)
            continue;
        m_nestedSetsInside.append(set);
        numberOfLeavesInside += m_nestedSets[set].numberOfLeaves;
    }
    if (numberOfLeavesInside != leaves.size())
        return false;
    if (m_nestedSetsInside.size() == 1)
        return true;

    Vector<uint32_t, 16> nodes;
    for (uint32_t set : m_nestedSetsInside)
        nodes.append(m_nestedSets[set].node);
    uint32_t numberOfNodes = markNodes(nodes.span());
    RELEASE_ASSERT(numberOfNodes == nodes.size());
    countPertinentChildren();
    uint32_t root = labelPertinentNodes(numberOfNodes);
    RELEASE_ASSERT(root != none && at(root).kind == Kind::P && !at(root).numberOfPartialChildren);
    uint32_t node = root;
    if (at(root).label != Label::Full) {
        reducePertinentRootP(root);
        node = at(root).endChildren[1];
    }
    uint32_t united = m_nestedSetsInside[0];
    for (uint32_t set : m_nestedSetsInside.span().subspan(1))
        m_nestedSets[set].parent = united;
    m_nestedSets[united].node = node;
    m_nestedSets[united].numberOfLeaves = static_cast<uint32_t>(numberOfLeavesInside);
    return true;
}

Vector<uint32_t> ConsecutiveSets::Tree::order(std::span<const uint64_t> keyOfLeaf) const
{
    RELEASE_ASSERT(keyOfLeaf.size() == m_numberOfLeaves);
    Vector<uint32_t> result;
    if (m_root == none)
        return result;
    result.reserveInitialCapacity(m_numberOfLeaves);

    Vector<uint32_t> parents(FillWith { }, m_nodes.size(), none);
    Vector<uint32_t> parentsBeforeChildren { m_root };
    for (size_t next = 0; next < parentsBeforeChildren.size(); ++next) {
        uint32_t node = parentsBeforeChildren[next];
        forEachChild(node, [&](uint32_t child) {
            parents[child] = node;
            parentsBeforeChildren.append(child);
        });
    }
    Vector<uint64_t> lowestKeyBelow(FillWith { }, m_nodes.size(), UINT64_MAX);
    for (size_t next = parentsBeforeChildren.size(); next--;) {
        uint32_t node = parentsBeforeChildren[next];
        if (at(node).kind == Kind::Leaf)
            lowestKeyBelow[node] = keyOfLeaf[node];
        if (uint32_t parent = parents[node]; parent != none)
            lowestKeyBelow[parent] = std::min(lowestKeyBelow[parent], lowestKeyBelow[node]);
    }

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
            AOT::sortInLine(children, { }, [&](uint32_t child) {
                return lowestKeyBelow[child];
            });
        } else if (lowestKeyBelow[children.last()] < lowestKeyBelow[children.first()])
            children.reverse();
        for (size_t next = children.size(); next--;)
            stack.append(children[next]);
    }
    RELEASE_ASSERT(result.size() == m_numberOfLeaves);
    return result;
}

ConsecutiveSets::ConsecutiveSets(uint32_t numberOfLeaves)
    : m_tree(makeUnique<Tree>(numberOfLeaves))
{
}

ConsecutiveSets::~ConsecutiveSets() = default;

bool ConsecutiveSets::tryKeepTogether(std::span<const uint32_t> leaves)
{
    return m_tree->tryKeepTogether(leaves);
}

bool ConsecutiveSets::tryKeepNestedSetTogether(std::span<const uint32_t> leaves)
{
    return m_tree->tryKeepNestedSetTogether(leaves);
}

Vector<uint32_t> ConsecutiveSets::order(std::span<const uint64_t> keyOfLeaf) const
{
    return m_tree->order(keyOfLeaf);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
