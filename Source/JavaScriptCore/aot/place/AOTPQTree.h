/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
