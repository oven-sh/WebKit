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
    // What the table held lies at the same offsets as before, and the rest of it was never written.
    JS_EXPORT_PRIVATE void didMoveAndGrowTable(std::span<const uint8_t> table);
    JS_EXPORT_PRIVATE void recordAll();

    ALWAYS_INLINE void record(const void* slot)
    {
        size_t offset = std::bit_cast<uintptr_t>(slot) - m_tableStart;
        ASSERT(!m_tableSize || offset < m_tableSize);
        size_t card = offset >> m_regionShift;
        if (m_cards[card] == writtenCard)
            return;
        m_cards[card] = writtenCard;
        if (m_writtenCards.size() < m_maxNumberOfListedCards) [[likely]]
            m_writtenCards.append(static_cast<uint32_t>(card));
        else
            m_listsEveryWrittenCard = false;
    }

    // Compiled code stores into the cards of such a set itself, so only the cards say what was written.
    void setCardsAreWrittenWithoutRecord()
    {
        m_cardsAreWrittenWithoutRecord = true;
        m_listsEveryWrittenCard = false;
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

    using ListOfCards = Vector<uint32_t, 16>;

    // The roots are visited at every stop of the mutator, and little or nothing was written to most tables since the stop before.
    // So the cards that record() writes are listed, and whoever asks for the written ones reads the list and not all the cards.
    // A list of an eighth of the cards is no shorter than the cards, which are read eight at a time: from there on nothing is listed.
    static size_t maxNumberOfListedCardsFor(size_t numberOfCards) { return std::max<size_t>(numberOfCards / cardsPerWord, 16); }
    ListOfCards takeListOfWrittenCards(bool& listsEveryWrittenCard);

    template<typename Functor> void forEachSelectedCard(Selection, const Functor&);
    template<typename Functor> void forEachCardInState(Selection, const Functor&) const;
    template<typename Functor> void callWithRegionOf(size_t card, const Functor&) const;

    uintptr_t m_tableStart { 0 };
    size_t m_tableSize { 0 };
    unsigned m_regionShift { shiftOfSingleRegion };
    uint32_t m_lastFullVisitVersion { 0 };
    bool m_listsEveryWrittenCard { true };
    bool m_cardsAreWrittenWithoutRecord { false };
    size_t m_numberOfCards { cardsPerWord };
    size_t m_maxNumberOfListedCards { maxNumberOfListedCardsFor(cardsPerWord) };
    ListOfCards m_writtenCards;
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
    if (m_listsEveryWrittenCard) {
        for (uint32_t card : m_writtenCards) {
            if (m_cards[card] == writtenCard)
                return true;
        }
        return false;
    }
    for (size_t first = 0; first < m_numberOfCards; first += cardsPerWord) {
        uint64_t states;
        memcpy(&states, m_cards + first, sizeof(states));
        if (states & writtenInEveryCard)
            return true;
    }
    return false;
}

inline auto RememberedSlotSet::takeListOfWrittenCards(bool& listsEveryWrittenCard) -> ListOfCards
{
    listsEveryWrittenCard = std::exchange(m_listsEveryWrittenCard, !m_cardsAreWrittenWithoutRecord);
    return std::exchange(m_writtenCards, { });
}

// Whoever calls this takes every written card out of that state. What the functor records is listed anew.
template<typename Functor>
ALWAYS_INLINE void RememberedSlotSet::forEachSelectedCard(Selection selection, const Functor& functor)
{
    bool listsEveryWrittenCard;
    ListOfCards writtenCards = takeListOfWrittenCards(listsEveryWrittenCard);
    if (selection != Selection::Written || !listsEveryWrittenCard) {
        forEachCardInState(selection, functor);
        return;
    }
    std::ranges::sort(writtenCards);
    for (uint32_t card : writtenCards) {
        if (m_cards[card] == writtenCard)
            functor(card);
    }
}

template<typename Functor>
ALWAYS_INLINE void RememberedSlotSet::forEachCardInState(Selection selection, const Functor& functor) const
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
        forEachCardInState(Selection::WrittenOrScanned, [&](size_t card) {
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
