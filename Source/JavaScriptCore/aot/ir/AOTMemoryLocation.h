/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAbstractHeap.h"
#include "AOTID.h"
#include <array>
#include <compare>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct Function;
struct Value;

template<typename IDType>
    requires requires(IDType id) { id.index(); }
constexpr uint32_t locationIndexOf(IDType id) { return id.index(); }
constexpr uint32_t locationIndexOf(uint32_t index) { return index; }
constexpr uint32_t locationIndexOf(ElementBase base) { return static_cast<uint32_t>(base); }

class MemoryLocation {
public:
    constexpr MemoryLocation(AbstractHeap heap)
        : m_heap(heap)
    {
    }

    static constexpr MemoryLocation at(AbstractHeap heap, uint32_t first, uint32_t second = 0)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(indexedHeaps.contains(heap));
        MemoryLocation location(heap);
        location.m_isEntireHeap = false;
        location.m_first = first;
        location.m_second = second;
        return location;
    }

    static constexpr MemoryLocation moduleVariable(VariableID variable) { return at(AbstractHeap::ModuleVariables, variable.index()); }
    static constexpr MemoryLocation environmentField(LayoutID layout, uint32_t field) { return at(AbstractHeap::EnvironmentFields, layout.index(), field); }
    static constexpr MemoryLocation objectField(PropertyKeyID key, LayoutSetID receivers = { }) { return at(AbstractHeap::ObjectFields, key.index(), receivers ? receivers.index() + 1 : 0); }
    static constexpr MemoryLocation namedProperty(PropertyKeyID key) { return at(AbstractHeap::NamedProperties, key.index()); }
    static constexpr MemoryLocation shapeWords(LayoutSetID layouts) { return at(AbstractHeap::ShapeWords, layouts.index()); }
    static constexpr MemoryLocation memberGuards(LayoutID layout) { return at(AbstractHeap::MemberGuards, layout.index()); }
    static constexpr MemoryLocation elements(ElementBase base) { return at(AbstractHeap::Elements, static_cast<uint32_t>(base)); }

    constexpr AbstractHeap heap() const { return m_heap; }
    constexpr bool isEntireHeap() const { return m_isEntireHeap; }
    constexpr uint32_t firstIndex() const { return indexOf(m_first); }
    constexpr uint32_t secondIndex() const { return indexOf(m_second); }

    constexpr VariableID variable() const { return VariableID(indexIn(AbstractHeap::ModuleVariables, m_first)); }
    constexpr LayoutID environmentLayout() const { return LayoutID(indexIn(AbstractHeap::EnvironmentFields, m_first)); }
    constexpr uint32_t environmentFieldIndex() const { return indexIn(AbstractHeap::EnvironmentFields, m_second); }
    constexpr LayoutSetID layoutSet() const { return LayoutSetID(indexIn(AbstractHeap::ShapeWords, m_first)); }

    constexpr LayoutSetID receivers() const
    {
        uint32_t encodedReceivers = indexIn(AbstractHeap::ObjectFields, m_second);
        return encodedReceivers ? LayoutSetID(encodedReceivers - 1) : LayoutSetID();
    }

    constexpr LayoutID guardedLayout() const { return LayoutID(indexIn(AbstractHeap::MemberGuards, m_first)); }
    constexpr ElementBase elementBase() const { return static_cast<ElementBase>(indexIn(AbstractHeap::Elements, m_first)); }

    constexpr PropertyKeyID key() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(m_heap == AbstractHeap::ObjectFields || m_heap == AbstractHeap::NamedProperties);
        return PropertyKeyID(indexOf(m_first));
    }

    void dump(PrintStream&, const Program&) const;

    friend constexpr auto operator<=>(const MemoryLocation&, const MemoryLocation&) = default;

private:
    constexpr uint32_t indexOf(uint32_t index) const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(!m_isEntireHeap);
        return index;
    }

    constexpr uint32_t indexIn(AbstractHeap heap, uint32_t index) const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(m_heap == heap);
        return indexOf(index);
    }

    AbstractHeap m_heap;
    bool m_isEntireHeap { true };
    uint32_t m_first { 0 };
    uint32_t m_second { 0 };
};

static_assert(sizeof(MemoryLocation) == 12);

enum class LocationOverlap : uint8_t { May, None, Exact };

uint64_t maskOf(const Program&, const MemoryLocation&);
LocationOverlap overlapOf(const Program&, const MemoryLocation&, const MemoryLocation&);

struct MemoryLocationDump {
    const Program& program;
    MemoryLocation location;

    void dump(PrintStream&) const;
};

class LocationMasks {
public:
    static constexpr uint64_t entireHeap = UINT64_MAX;

    constexpr LocationMasks() = default;

    constexpr LocationMasks(AbstractHeapSet heaps)
    {
        add(heaps);
    }

    static constexpr LocationMasks top() { return LocationMasks(allHeaps); }

    constexpr bool isEmpty() const { return m_heaps.isEmpty(); }
    constexpr bool isTop() const { return *this == top(); }
    constexpr AbstractHeapSet heaps() const { return m_heaps; }
    constexpr bool contains(AbstractHeap heap) const { return m_heaps.contains(heap); }
    constexpr bool containsAny(AbstractHeapSet heaps) const { return m_heaps.containsAny(heaps); }
    constexpr bool containsOnly(AbstractHeapSet heaps) const { return m_heaps.containsOnly(heaps); }

    constexpr uint64_t maskOf(AbstractHeap heap) const
    {
        if (!m_heaps.contains(heap))
            return 0;
        return indexedHeaps.contains(heap) ? m_masks[maskIndexOf(heap)] : entireHeap;
    }

    constexpr bool containsEntire(AbstractHeap heap) const { return maskOf(heap) == entireHeap; }

    constexpr void add(AbstractHeap heap, uint64_t mask = entireHeap)
    {
        if (!mask)
            return;
        m_heaps.add(heap);
        if (indexedHeaps.contains(heap))
            m_masks[maskIndexOf(heap)] |= mask;
    }

    constexpr void add(AbstractHeapSet heaps)
    {
        for (unsigned index = 0; index < numberOfHeaps; ++index) {
            auto heap = static_cast<AbstractHeap>(index);
            if (heaps.contains(heap))
                add(heap);
        }
    }

    void add(const Program& program, const MemoryLocation& location) { add(location.heap(), Typed::maskOf(program, location)); }

    // Without a program, a layout set counts as all of ShapeWords.
    void add(const MemoryLocation&);

    constexpr bool merge(const LocationMasks& other)
    {
        bool changed = !m_heaps.containsAll(other.m_heaps);
        m_heaps.add(other.m_heaps);
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            uint64_t merged = m_masks[index] | other.m_masks[index];
            changed |= merged != m_masks[index];
            m_masks[index] = merged;
        }
        return changed;
    }

    constexpr void remove(AbstractHeapSet heaps)
    {
        m_heaps.remove(heaps);
        for (unsigned index = 0; index < numberOfHeaps; ++index) {
            auto heap = static_cast<AbstractHeap>(index);
            if (heaps.contains(heap) && indexedHeaps.contains(heap))
                m_masks[maskIndexOf(heap)] = 0;
        }
    }

    constexpr LocationMasks without(AbstractHeapSet heaps) const
    {
        LocationMasks result = *this;
        result.remove(heaps);
        return result;
    }

    constexpr LocationMasks restrictedTo(AbstractHeapSet heaps) const { return without(allHeaps - heaps); }

    constexpr LocationMasks intersectedWith(const LocationMasks& other) const
    {
        LocationMasks result;
        AbstractHeapSet common = m_heaps & other.m_heaps;
        for (unsigned index = 0; index < numberOfHeaps; ++index) {
            auto heap = static_cast<AbstractHeap>(index);
            if (common.contains(heap))
                result.add(heap, maskOf(heap) & other.maskOf(heap));
        }
        return result;
    }

    constexpr bool overlaps(AbstractHeap heap, uint64_t mask) const { return maskOf(heap) & mask; }

    constexpr bool overlaps(const LocationMasks& other) const
    {
        if (!((m_heaps & other.m_heaps) - indexedHeaps).isEmpty())
            return true;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (m_masks[index] & other.m_masks[index])
                return true;
        }
        return false;
    }

    bool overlaps(const Program& program, const MemoryLocation& location) const { return overlaps(location.heap(), Typed::maskOf(program, location)); }

    constexpr bool isSubsetOf(const LocationMasks& other) const
    {
        if (!other.m_heaps.containsAll(m_heaps))
            return false;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (m_masks[index] & ~other.m_masks[index])
                return false;
        }
        return true;
    }

    void forEach(NOESCAPE const Invocable<void(AbstractHeap, uint64_t mask)> auto& functor) const
    {
        for (AbstractHeap heap : m_heaps)
            functor(heap, maskOf(heap));
    }

    void dump(PrintStream&) const;

    friend constexpr bool operator==(const LocationMasks&, const LocationMasks&) = default;

private:
    AbstractHeapSet m_heaps;
    std::array<uint64_t, numberOfIndexedHeaps> m_masks { };
};

struct MemoryAccess {
    enum class Kind : uint8_t { NotExact, Read, Write };

    Kind kind { Kind::NotExact };
    MemoryLocation location { AbstractHeap::RuntimeState };
    ValueID base;
    ValueID stored;

    explicit operator bool() const { return kind != Kind::NotExact; }
};

MemoryAccess memoryAccessOf(const Function&, const Value&);

enum class LocationStability : uint8_t { Unknown, ChangedByOwnStores, UnchangedDuringActivation };

LocationStability stabilityOf(const Program&, const Value& access, const MemoryAccess&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
