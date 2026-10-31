/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <concepts>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/Hasher.h>
#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC { namespace AOT {

// A sorted set whose copies share their nodes: a copy costs nothing, and a value added to a copy costs the path that leads to it.
// It is a treap whose priorities are hashes of the values, so the values alone decide the shape, and two sets are compared only where they differ.
template<typename T>
class PersistentSortedSet {
    static_assert(std::is_trivially_copyable_v<T> && std::totally_ordered<T>);
public:
    size_t size() const { return m_root ? m_root->size : 0; }
    bool isEmpty() const { return !m_root; }

    bool contains(const T& value) const
    {
        const Node* node = m_root.get();
        while (node && node->value != value)
            node = (value < node->value ? node->left : node->right).get();
        return node;
    }

    bool overlaps(const PersistentSortedSet& other) const
    {
        if (m_root == other.m_root)
            return !isEmpty();
        const PersistentSortedSet& smaller = size() < other.size() ? *this : other;
        const PersistentSortedSet& larger = &smaller == this ? other : *this;
        return !larger.isEmpty() && !allOf(smaller.m_root.get(), [&](const T& value) {
            return !larger.contains(value);
        });
    }

    bool add(const T& value)
    {
        if (contains(value))
            return false;
        insert(m_root, value, computeHash(value));
        return true;
    }

    bool unite(const PersistentSortedSet& other)
    {
        size_t sizeBefore = size();
        m_root = united(m_root, other.m_root);
        return size() != sizeBefore;
    }

    void intersect(const PersistentSortedSet& other) { m_root = common(m_root, other.m_root); }
    void subtract(const PersistentSortedSet& other) { m_root = without(m_root, other.m_root); }

    template<typename Functor>
    void forEach(const Functor& functor) const
    {
        allOf(m_root.get(), [&](const T& value) {
            functor(value);
            return true;
        });
    }

    friend bool operator==(const PersistentSortedSet& a, const PersistentSortedSet& b) { return areEqual(a.m_root.get(), b.m_root.get()); }

private:
    struct Node : ThreadSafeRefCounted<Node> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Node);

        Node(const T& value, unsigned priority, RefPtr<Node>&& left, RefPtr<Node>&& right)
            : value(value)
            , priority(priority)
            , size(1 + (left ? left->size : 0) + (right ? right->size : 0))
            , left(WTF::move(left))
            , right(WTF::move(right))
        {
        }

        T value;
        unsigned priority;
        uint32_t size;
        RefPtr<Node> left;
        RefPtr<Node> right;
    };

    struct Parts {
        RefPtr<Node> below;
        RefPtr<Node> above;
        bool holdsValue { false };
    };

    static bool standsAbove(unsigned priority, const T& value, const Node& other) { return priority > other.priority || (priority == other.priority && value < other.value); }
    static bool standsAbove(const Node& node, const Node& other) { return standsAbove(node.priority, node.value, other); }
    static RefPtr<Node> create(const T& value, unsigned priority, RefPtr<Node> left, RefPtr<Node> right) { return adoptRef(*new Node(value, priority, WTF::move(left), WTF::move(right))); }

    static RefPtr<Node> withChildren(const RefPtr<Node>& node, RefPtr<Node>&& left, RefPtr<Node>&& right)
    {
        if (left == node->left && right == node->right)
            return node;
        return create(node->value, node->priority, WTF::move(left), WTF::move(right));
    }

    static Parts split(const RefPtr<Node>& node, const T& value)
    {
        if (!node)
            return { };
        if (node->value == value)
            return { node->left, node->right, true };
        if (node->value < value) {
            Parts parts = split(node->right, value);
            return { withChildren(node, RefPtr { node->left }, WTF::move(parts.below)), WTF::move(parts.above), parts.holdsValue };
        }
        Parts parts = split(node->left, value);
        return { WTF::move(parts.below), withChildren(node, WTF::move(parts.above), RefPtr { node->right }), parts.holdsValue };
    }

    static RefPtr<Node> join(RefPtr<Node>&& below, RefPtr<Node>&& above)
    {
        if (!below)
            return WTF::move(above);
        if (!above)
            return WTF::move(below);
        if (standsAbove(*below, *above))
            return withChildren(below, RefPtr { below->left }, join(RefPtr { below->right }, WTF::move(above)));
        return withChildren(above, join(WTF::move(below), RefPtr { above->left }), RefPtr { above->right });
    }

    static RefPtr<Node> united(RefPtr<Node> a, RefPtr<Node> b)
    {
        if (!a || a == b)
            return b;
        if (!b)
            return a;
        if (!standsAbove(*a, *b))
            std::swap(a, b);
        Parts parts = split(b, a->value);
        RefPtr<Node> left = united(a->left, WTF::move(parts.below));
        RefPtr<Node> right = united(a->right, WTF::move(parts.above));
        if (b->value == a->value && left == b->left && right == b->right)
            return b;
        return withChildren(a, WTF::move(left), WTF::move(right));
    }

    static RefPtr<Node> common(RefPtr<Node> a, RefPtr<Node> b)
    {
        if (!a || !b)
            return nullptr;
        if (a == b)
            return a;
        if (!standsAbove(*a, *b))
            std::swap(a, b);
        Parts parts = split(b, a->value);
        RefPtr<Node> left = common(a->left, WTF::move(parts.below));
        RefPtr<Node> right = common(a->right, WTF::move(parts.above));
        if (!parts.holdsValue)
            return join(WTF::move(left), WTF::move(right));
        return withChildren(a, WTF::move(left), WTF::move(right));
    }

    static RefPtr<Node> without(const RefPtr<Node>& a, const RefPtr<Node>& b)
    {
        if (!a || a == b)
            return nullptr;
        if (!b)
            return a;
        Parts parts = split(b, a->value);
        RefPtr<Node> left = without(a->left, parts.below);
        RefPtr<Node> right = without(a->right, parts.above);
        if (parts.holdsValue)
            return join(WTF::move(left), WTF::move(right));
        return withChildren(a, WTF::move(left), WTF::move(right));
    }

    static void insert(RefPtr<Node>& place, const T& value, unsigned priority)
    {
        if (!place || standsAbove(priority, value, *place)) {
            Parts parts = split(place, value);
            place = create(value, priority, WTF::move(parts.below), WTF::move(parts.above));
            return;
        }
        if (!place->hasOneRef())
            place = create(place->value, place->priority, place->left, place->right);
        ++place->size;
        insert(value < place->value ? place->left : place->right, value, priority);
    }

    template<typename Predicate>
    static bool allOf(const Node* node, const Predicate& predicate)
    {
        return !node || (allOf(node->left.get(), predicate) && predicate(node->value) && allOf(node->right.get(), predicate));
    }

    static bool areEqual(const Node* a, const Node* b)
    {
        if (a == b)
            return true;
        return a && b && a->size == b->size && a->value == b->value && areEqual(a->left.get(), b->left.get()) && areEqual(a->right.get(), b->right.get());
    }

    RefPtr<Node> m_root;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
