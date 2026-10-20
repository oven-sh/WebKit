/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <type_traits>
#include <wtf/Assertions.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class SlotVisitor;

class RememberedSlotSet {
    WTF_MAKE_NONCOPYABLE(RememberedSlotSet);
public:
    static constexpr uint8_t neverWrittenCard = 0;
    static constexpr uint8_t writtenCard = 1;
    static constexpr uint8_t scannedCard = 2;
    static constexpr size_t cardsPerWord = sizeof(uint64_t);

    JS_EXPORT_PRIVATE RememberedSlotSet();
    JS_EXPORT_PRIVATE RememberedSlotSet(std::span<const uint8_t> table, unsigned regionShift);
    JS_EXPORT_PRIVATE RememberedSlotSet(std::span<const uint8_t> table, unsigned regionShift, std::span<uint8_t> zeroedCards);
    JS_EXPORT_PRIVATE ~RememberedSlotSet();

    static constexpr size_t numberOfCardsFor(size_t tableSize, unsigned regionShift)
    {
        size_t regions = (tableSize + (static_cast<size_t>(1) << regionShift) - 1) >> regionShift;
        return std::max<size_t>((regions + cardsPerWord - 1) / cardsPerWord * cardsPerWord, cardsPerWord);
    }

    JS_EXPORT_PRIVATE void didMoveTable(std::span<const uint8_t> table);
    JS_EXPORT_PRIVATE void recordAll();

    ALWAYS_INLINE void record(const void* slot)
    {
        size_t offset = std::bit_cast<uintptr_t>(slot) - m_tableStart;
        ASSERT(!m_tableSize || offset < m_tableSize);
        m_cards[offset >> m_regionShift] = writtenCard;
    }

    uint8_t* cards() { return m_cards; }
    unsigned regionShift() const { return m_regionShift; }
    bool hasWrittenRegions() const;

    template<typename Visitor, typename Functor> void forEachRegionToVisit(Visitor&, const Functor&);
    template<typename Visitor> bool tryDeferFullVisit(Visitor&, void (*visit)(SlotVisitor&, void* owner, size_t beginOffset, size_t endOffset), void* owner);
    template<typename Functor> void forEachRegionToSweep(CollectionScope, const Functor&);
    template<typename Functor> void forEachRegionToClear(const Functor&);

    template<typename Entry, typename Functor>
    static void forEachEntryInRegion(std::span<Entry> entries, size_t beginOffset, size_t endOffset, const Functor& functor)
    {
        size_t first = beginOffset / sizeof(Entry);
        size_t end = std::min((endOffset + sizeof(Entry) - 1) / sizeof(Entry), entries.size());
        for (size_t index = first; index < end; ++index)
            functor(entries[index]);
    }

private:
    static constexpr unsigned shiftOfSingleRegion = 63;
    static constexpr uint64_t writtenInEveryCard = 0x0101010101010101ull;

    enum class Selection : uint8_t { Written, WrittenOrScanned };

    template<typename Functor> void forEachSelectedCard(Selection, const Functor&) const;
    template<typename Functor> void callWithRegionOf(size_t card, const Functor&) const;

    uintptr_t m_tableStart { 0 };
    size_t m_tableSize { 0 };
    unsigned m_regionShift { shiftOfSingleRegion };
    uint32_t m_lastFullVisitVersion { 0 };
    size_t m_numberOfCards { cardsPerWord };
    uint8_t* m_cards { nullptr };
    Vector<uint8_t, cardsPerWord> m_ownedCards;
};

template<typename T>
class RememberedSlot {
public:
    RememberedSlot() = default;

    const T& get() const { return m_value; }
    explicit operator bool() const { return !!m_value; }

    ALWAYS_INLINE void set(RememberedSlotSet& set, const T& value)
    {
        m_value = value;
        set.record(this);
    }

    void clear() { m_value = T { }; }
    void setWithoutRecordingForTesting(const T& value) { m_value = value; }

    static constexpr ptrdiff_t offsetOfValue() { return 0; }

private:
    T m_value;
};

static_assert(sizeof(RememberedSlot<void*>) == sizeof(void*) && std::is_trivially_default_constructible_v<RememberedSlot<void*>>);
static_assert(sizeof(RememberedSlot<uint32_t>) == sizeof(uint32_t));

inline bool RememberedSlotSet::hasWrittenRegions() const
{
    for (size_t first = 0; first < m_numberOfCards; first += cardsPerWord) {
        uint64_t states;
        memcpy(&states, m_cards + first, sizeof(states));
        if (states & writtenInEveryCard)
            return true;
    }
    return false;
}

template<typename Functor>
ALWAYS_INLINE void RememberedSlotSet::forEachSelectedCard(Selection selection, const Functor& functor) const
{
    static_assert(writtenCard == 1 && scannedCard == 2 && !neverWrittenCard);
    for (size_t first = 0; first < m_numberOfCards; first += cardsPerWord) {
        uint64_t states;
        memcpy(&states, m_cards + first, sizeof(states));
        if (!(selection == Selection::Written ? states & writtenInEveryCard : states))
            continue;
        for (size_t card = first; card < first + cardsPerWord; ++card) {
            uint8_t state = m_cards[card];
            if (state == writtenCard || (selection == Selection::WrittenOrScanned && state == scannedCard))
                functor(card);
        }
    }
}

template<typename Functor>
ALWAYS_INLINE void RememberedSlotSet::callWithRegionOf(size_t card, const Functor& functor) const
{
    if (m_regionShift == shiftOfSingleRegion) {
        functor(static_cast<size_t>(0), m_tableSize);
        return;
    }
    size_t begin = card << m_regionShift;
    functor(begin, std::min(begin + (static_cast<size_t>(1) << m_regionShift), m_tableSize));
}

template<typename Visitor, typename Functor>
void RememberedSlotSet::forEachRegionToVisit(Visitor& visitor, const Functor& functor)
{
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
#if ASSERT_ENABLED
        visitor.collector().forEachHeap([](auto& heap) {
            ASSERT(heap.worldIsStopped());
        });
#endif
        Selection selection = Selection::Written;
        if (visitor.collectionScope() == CollectionScope::Full && m_lastFullVisitVersion != visitor.markingVersion()) {
            m_lastFullVisitVersion = visitor.markingVersion();
            selection = Selection::WrittenOrScanned;
        }
        forEachSelectedCard(selection, [&](size_t card) {
            m_cards[card] = scannedCard;
            callWithRegionOf(card, functor);
        });
    } else {
        forEachSelectedCard(Selection::WrittenOrScanned, [&](size_t card) {
            callWithRegionOf(card, functor);
        });
    }
}

template<typename Visitor>
bool RememberedSlotSet::tryDeferFullVisit(Visitor& visitor, void (*visit)(SlotVisitor&, void* owner, size_t beginOffset, size_t endOffset), void* owner)
{
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (visitor.collectionScope() != CollectionScope::Full || m_lastFullVisitVersion == visitor.markingVersion() || !visitor.defersRootRegions())
            return false;
        m_lastFullVisitVersion = visitor.markingVersion();

        constexpr size_t maxBytesPerDeferredRegion = 4096;
        size_t deferredBegin = 0;
        size_t deferredEnd = 0;
        auto deferRegion = [&] {
            if (deferredBegin != deferredEnd)
                visitor.deferRootRegion({ visit, owner, deferredBegin, deferredEnd });
        };
        forEachSelectedCard(Selection::WrittenOrScanned, [&](size_t card) {
            m_cards[card] = scannedCard;
            callWithRegionOf(card, [&](size_t beginOffset, size_t endOffset) {
                if (beginOffset != deferredEnd || endOffset - deferredBegin > maxBytesPerDeferredRegion) {
                    deferRegion();
                    deferredBegin = beginOffset;
                }
                deferredEnd = endOffset;
            });
        });
        deferRegion();
        return true;
    } else {
        UNUSED_PARAM(visitor);
        UNUSED_PARAM(visit);
        UNUSED_PARAM(owner);
        return false;
    }
}

template<typename Functor>
void RememberedSlotSet::forEachRegionToSweep(CollectionScope scope, const Functor& functor)
{
    forEachSelectedCard(scope == CollectionScope::Eden ? Selection::Written : Selection::WrittenOrScanned, [&](size_t card) {
        m_cards[card] = scannedCard;
        callWithRegionOf(card, functor);
    });
}

template<typename Functor>
void RememberedSlotSet::forEachRegionToClear(const Functor& functor)
{
    forEachSelectedCard(Selection::WrittenOrScanned, [&](size_t card) {
        m_cards[card] = neverWrittenCard;
        callWithRegionOf(card, functor);
    });
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
