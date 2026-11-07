/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Machine {

using HeapIndex = uint16_t;
inline constexpr HeapIndex rootHeapIndex = 0;

enum class Mutability : uint8_t { Mutable, Immutable };

enum class HeapTrait : uint8_t {
    Immutable = 1 << 0,
    HoldsInnerPointer = 1 << 1,
    HoldsPermanentCell = 1 << 2,
    IsOutsideDeclaredEffects = 1 << 3,
    IsFilledOnce = 1 << 4,
};

class HeapTree {
    WTF_MAKE_NONCOPYABLE(HeapTree);
public:
    JS_EXPORT_PRIVATE HeapTree();

    JS_EXPORT_PRIVATE HeapIndex add(HeapIndex parent, ASCIILiteral name);

    unsigned size() const { return m_parents.size(); }
    HeapIndex parentOf(HeapIndex heap) const { return m_parents[heap]; }
    std::span<const HeapIndex> parents() const LIFETIME_BOUND { return m_parents.span(); }
    ASCIILiteral nameOf(HeapIndex heap) const { return m_names[heap]; }

    bool isSameOrAbove(HeapIndex above, HeapIndex heap) const
    {
        while (heap > above)
            heap = m_parents[heap];
        return heap == above;
    }
    bool overlap(HeapIndex a, HeapIndex b) const { return a <= b ? isSameOrAbove(a, b) : isSameOrAbove(b, a); }

private:
    Vector<HeapIndex> m_parents;
    Vector<ASCIILiteral> m_names;
};

class AbstractHeap {
public:
    constexpr AbstractHeap() = default;
    constexpr AbstractHeap(HeapIndex index, ptrdiff_t offset = 0, OptionSet<HeapTrait> traits = { })
        : m_offset(offset)
        , m_index(index)
        , m_traits(traits)
        , m_isInitialized(true)
    {
    }

    constexpr bool isInitialized() const { return m_isInitialized; }
    constexpr HeapIndex index() const { return m_index; }
    constexpr ptrdiff_t offset() const { return m_offset; }
    constexpr OptionSet<HeapTrait> traits() const { return m_traits; }
    constexpr Mutability mutability() const { return m_traits.contains(HeapTrait::Immutable) ? Mutability::Immutable : Mutability::Mutable; }

private:
    ptrdiff_t m_offset { 0 };
    HeapIndex m_index { rootHeapIndex };
    OptionSet<HeapTrait> m_traits;
    bool m_isInitialized { false };
};
static_assert(sizeof(AbstractHeap) == 16);

class IndexedAbstractHeap {
public:
    static constexpr unsigned numberOfIndexesWithOwnHeap = 16;

    IndexedAbstractHeap() = default;
    JS_EXPORT_PRIVATE IndexedAbstractHeap(HeapTree&, HeapIndex parent, ASCIILiteral name, ptrdiff_t offset, size_t elementSize, OptionSet<HeapTrait> = { });

    const AbstractHeap& atAnyIndex() const LIFETIME_BOUND { return m_anyIndex; }

    AbstractHeap at(ptrdiff_t index) const
    {
        unsigned child = static_cast<size_t>(index) < numberOfIndexesWithOwnHeap ? static_cast<unsigned>(index) : numberOfIndexesWithOwnHeap;
        return AbstractHeap(static_cast<HeapIndex>(m_firstChild + child), m_offset + index * static_cast<ptrdiff_t>(m_elementSize), m_anyIndex.traits());
    }
    AbstractHeap operator[](ptrdiff_t index) const { return at(index); }

    ptrdiff_t offset() const { return m_offset; }
    size_t elementSize() const { return m_elementSize; }

private:
    AbstractHeap m_anyIndex;
    ptrdiff_t m_offset { 0 };
    size_t m_elementSize { 0 };
    HeapIndex m_firstChild { rootHeapIndex };
};

class NumberedAbstractHeap {
public:
    NumberedAbstractHeap() = default;
    NumberedAbstractHeap(HeapTree& tree, HeapIndex parent, ASCIILiteral name)
        : m_heaps(tree, parent, name, 0, 0)
    {
    }

    const AbstractHeap& atAnyNumber() const LIFETIME_BOUND { return m_heaps.atAnyIndex(); }
    AbstractHeap at(ptrdiff_t number) const { return m_heaps.at(number); }
    AbstractHeap operator[](ptrdiff_t number) const { return at(number); }

private:
    IndexedAbstractHeap m_heaps;
};

class HeapSet {
public:
    constexpr HeapSet() = default;
    static constexpr HeapSet top()
    {
        HeapSet result;
        result.m_isTop = true;
        return result;
    }
    static HeapSet of(std::span<const HeapIndex> heaps)
    {
        HeapSet result;
        result.m_heaps = heaps.data();
        result.m_numberOfHeaps = static_cast<uint32_t>(heaps.size());
        return result;
    }

    bool isEmpty() const { return !m_isTop && !m_numberOfHeaps; }
    bool isTop() const { return m_isTop; }
    std::span<const HeapIndex> heaps() const { return unsafeMakeSpan(m_heaps, m_numberOfHeaps); }

private:
    const HeapIndex* m_heaps { nullptr };
    uint32_t m_numberOfHeaps { 0 };
    bool m_isTop { false };
};
static_assert(sizeof(HeapSet) == 16);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
