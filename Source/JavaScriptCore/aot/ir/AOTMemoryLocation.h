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
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

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

uint64_t maskOf(const Program&, const MemoryLocation&);
uint64_t maskOfRead(const Program&, const MemoryLocation&);

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

    static constexpr LocationMasks top();

    constexpr bool isEmpty() const { return !m_heaps; }

    constexpr bool isTop() const
    {
        if (m_heaps != bitsOf(allHeaps))
            return false;
        for (uint64_t mask : m_masks) {
            if (mask != entireHeap)
                return false;
        }
        return true;
    }

    constexpr AbstractHeapSet heaps() const { return AbstractHeapSet::fromRaw(m_heaps); }
    constexpr bool contains(AbstractHeap heap) const { return m_heaps & bitsOf(heap); }
    constexpr bool containsAny(AbstractHeapSet heaps) const { return m_heaps & bitsOf(heaps); }
    constexpr bool containsOnly(AbstractHeapSet heaps) const { return !(m_heaps & ~bitsOf(heaps)); }

    constexpr uint64_t maskOf(AbstractHeap heap) const
    {
        if (!contains(heap))
            return 0;
        return indexedHeaps.contains(heap) ? m_masks[maskIndexOf(heap)] : entireHeap;
    }

    constexpr bool containsEntire(AbstractHeap heap) const { return maskOf(heap) == entireHeap; }

    constexpr void add(AbstractHeap heap, uint64_t mask = entireHeap)
    {
        if (!mask)
            return;
        m_heaps |= bitsOf(heap);
        if (indexedHeaps.contains(heap))
            m_masks[maskIndexOf(heap)] |= mask;
    }

    constexpr void add(AbstractHeapSet heaps)
    {
        m_heaps |= bitsOf(heaps);
        if (!heaps.containsAny(indexedHeaps))
            return;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (heaps.contains(indexedHeapsInOrder[index]))
                m_masks[index] = entireHeap;
        }
    }

    ALWAYS_INLINE void add(const Program& program, const MemoryLocation& location) { add(location.heap(), location.isEntireHeap() ? entireHeap : Typed::maskOf(program, location)); }

    void add(const MemoryLocation&);
    ALWAYS_INLINE void addRead(const Program& program, const MemoryLocation& location)
    {
        if (uint64_t mask = location.isEntireHeap() ? entireHeap : maskOfRead(program, location))
            add(location.heap(), mask);
    }

    constexpr bool merge(const LocationMasks& other)
    {
        bool changed = other.m_heaps & ~m_heaps;
        m_heaps |= other.m_heaps;
        if (!other.containsAny(indexedHeaps))
            return changed;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            uint64_t merged = m_masks[index] | other.m_masks[index];
            changed |= merged != m_masks[index];
            m_masks[index] = merged;
        }
        return changed;
    }

    constexpr void remove(AbstractHeapSet heaps)
    {
        m_heaps &= ~bitsOf(heaps);
        if (!heaps.containsAny(indexedHeaps))
            return;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (heaps.contains(indexedHeapsInOrder[index]))
                m_masks[index] = 0;
        }
    }

    constexpr LocationMasks without(AbstractHeapSet heaps) const
    {
        LocationMasks result = *this;
        result.remove(heaps);
        return result;
    }

    constexpr LocationMasks restrictedTo(AbstractHeapSet heaps) const { return without(allHeaps - heaps); }

    constexpr void intersect(const LocationMasks& other)
    {
        bool hadIndexedHeaps = containsAny(indexedHeaps);
        m_heaps &= other.m_heaps & ~bitsOf(indexedHeaps);
        if (!hadIndexedHeaps)
            return;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            m_masks[index] &= other.m_masks[index];
            if (m_masks[index])
                m_heaps |= bitsOf(indexedHeapsInOrder[index]);
        }
    }

    constexpr LocationMasks intersectedWith(const LocationMasks& other) const
    {
        LocationMasks result;
        uint32_t commonHeaps = m_heaps & other.m_heaps;
        result.m_heaps = commonHeaps & ~bitsOf(indexedHeaps);
        if (!(commonHeaps & bitsOf(indexedHeaps)))
            return result;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            uint64_t common = m_masks[index] & other.m_masks[index];
            result.m_masks[index] = common;
            if (common)
                result.m_heaps |= bitsOf(indexedHeapsInOrder[index]);
        }
        return result;
    }

    constexpr bool overlaps(const LocationMasks& other) const { return overlapsWithin(allHeaps, other); }

    constexpr bool overlapsWithin(AbstractHeapSet heaps, const LocationMasks& other) const
    {
        uint32_t common = m_heaps & other.m_heaps & bitsOf(heaps);
        if (!common)
            return false;
        if (common & ~bitsOf(indexedHeaps))
            return true;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (common & bitsOf(indexedHeapsInOrder[index]) && m_masks[index] & other.m_masks[index])
                return true;
        }
        return false;
    }

    constexpr bool isSubsetOf(const LocationMasks& other) const
    {
        if (m_heaps & ~other.m_heaps)
            return false;
        if (!containsAny(indexedHeaps))
            return true;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (m_masks[index] & ~other.m_masks[index])
                return false;
        }
        return true;
    }

    constexpr bool masksAgreeWithHeaps() const
    {
        uint32_t heapsWithMasks = 0;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index) {
            if (m_masks[index])
                heapsWithMasks |= bitsOf(indexedHeapsInOrder[index]);
        }
        return heapsWithMasks == (m_heaps & bitsOf(indexedHeaps));
    }

    void forEach(NOESCAPE const Invocable<void(AbstractHeap, uint64_t mask)> auto& functor) const
    {
        for (AbstractHeap heap : heaps())
            functor(heap, maskOf(heap));
    }

    friend constexpr bool operator==(const LocationMasks& first, const LocationMasks& second)
    {
        return first.m_heaps == second.m_heaps && (!first.containsAny(indexedHeaps) || first.hasMasksOf(second));
    }

private:
    static_assert(numberOfHeaps <= 32);
    static constexpr uint32_t bitsOf(AbstractHeapSet heaps) { return static_cast<uint32_t>(heaps.toRaw()); }

    constexpr bool hasMasksOf(const LocationMasks& other) const
    {
        uint64_t difference = 0;
        for (unsigned index = 0; index < numberOfIndexedHeaps; ++index)
            difference |= m_masks[index] ^ other.m_masks[index];
        return !difference;
    }

    std::array<uint64_t, numberOfIndexedHeaps> m_masks { };
    uint32_t m_heaps { 0 };
};

constexpr LocationMasks LocationMasks::top()
{
    constexpr LocationMasks everyHeap(allHeaps);
    return everyHeap;
}

constexpr uint64_t locationMaskOfBit(std::optional<uint8_t> bit, uint32_t offset = 0)
{
    if (!bit)
        return LocationMasks::entireHeap;
    return uint64_t { 1 } << ((*bit + offset) & 63);
}

constexpr uint64_t cellHoldingElementsMask = uint64_t { 1 } << 63;

constexpr uint64_t locationMaskOfElementBase(ElementBase base)
{
    switch (base) {
    case ElementBase::Unknown:
        return LocationMasks::entireHeap;
    case ElementBase::Array:
        return (uint64_t { 1 } << static_cast<unsigned>(base)) | cellHoldingElementsMask;
    case ElementBase::Arguments:
    case ElementBase::Object:
        return uint64_t { 1 } << static_cast<unsigned>(base);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline constexpr LocationMasks placesUnderLengthsOfLists { AbstractHeapSet { AbstractHeap::ArrayStorage, AbstractHeap::ArrayLengths } };

template<typename ProgramWithLocationBits>
ALWAYS_INLINE uint64_t locationMaskIn(const ProgramWithLocationBits& program, const MemoryLocation& location)
{
    if (location.isEntireHeap())
        return LocationMasks::entireHeap;

    switch (location.heap()) {
    case AbstractHeap::ModuleVariables:
        return locationMaskOfBit(program.locationBitOf(location.variable()));
    case AbstractHeap::EnvironmentFields:
        return locationMaskOfBit(program.locationBitOf(location.environmentLayout()), location.environmentFieldIndex());
    case AbstractHeap::MemberGuards:
        return locationMaskOfBit(program.locationBitOf(location.guardedLayout()));
    case AbstractHeap::ObjectFields:
    case AbstractHeap::NamedProperties:
        return locationMaskOfBit(program.types.locationBitOf(location.key()));
    case AbstractHeap::Elements:
        return locationMaskOfElementBase(location.elementBase());
    case AbstractHeap::ShapeWords:
        return LocationMasks::entireHeap;
    case AbstractHeap::OrderWords:
    case AbstractHeap::PrototypeLinks:
    case AbstractHeap::ArrayStorage:
    case AbstractHeap::TypedArrayStorage:
    case AbstractHeap::TypedArrayElements:
    case AbstractHeap::CollectionStorage:
    case AbstractHeap::CollectionEntries:
    case AbstractHeap::InternalFields:
    case AbstractHeap::RegExpState:
    case AbstractHeap::PromiseState:
    case AbstractHeap::StringStorage:
    case AbstractHeap::StringBuilderContents:
    case AbstractHeap::ParserState:
    case AbstractHeap::ExternalState:
    case AbstractHeap::AllocatorState:
    case AbstractHeap::RuntimeState:
    case AbstractHeap::CalleesInFrames:
    case AbstractHeap::ArrayLengths:
    case AbstractHeap::ArrayCapacity:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
