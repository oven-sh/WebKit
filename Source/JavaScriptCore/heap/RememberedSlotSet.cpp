/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "RememberedSlot.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

RememberedSlotSet::RememberedSlotSet()
{
    m_ownedCards.fill(neverWrittenCard, cardsPerWord);
    m_cards = m_ownedCards.mutableSpan().data();
}

RememberedSlotSet::RememberedSlotSet(std::span<const uint8_t> table, unsigned regionShift)
{
    RELEASE_ASSERT(regionShift < shiftOfSingleRegion, regionShift);
    m_tableStart = std::bit_cast<uintptr_t>(table.data());
    m_tableSize = table.size();
    m_regionShift = regionShift;
    m_numberOfCards = numberOfCardsFor(m_tableSize, regionShift);
    m_maxNumberOfListedCards = maxNumberOfListedCardsFor(m_numberOfCards);
    m_ownedCards.fill(neverWrittenCard, m_numberOfCards);
    m_cards = m_ownedCards.mutableSpan().data();
}

RememberedSlotSet::RememberedSlotSet(std::span<const uint8_t> table, unsigned regionShift, std::span<uint8_t> zeroedCards)
{
    RELEASE_ASSERT(regionShift < shiftOfSingleRegion, regionShift);
    m_tableStart = std::bit_cast<uintptr_t>(table.data());
    m_tableSize = table.size();
    m_regionShift = regionShift;
    m_numberOfCards = numberOfCardsFor(m_tableSize, regionShift);
    m_maxNumberOfListedCards = maxNumberOfListedCardsFor(m_numberOfCards);
    RELEASE_ASSERT(zeroedCards.size() >= m_numberOfCards && !(std::bit_cast<uintptr_t>(zeroedCards.data()) % cardsPerWord), zeroedCards.size(), m_numberOfCards);
    m_cards = zeroedCards.data();
}

RememberedSlotSet::~RememberedSlotSet() = default;

void RememberedSlotSet::didMoveTable(std::span<const uint8_t> table)
{
    RELEASE_ASSERT(m_cards == m_ownedCards.span().data() && m_regionShift != shiftOfSingleRegion);
    m_tableStart = std::bit_cast<uintptr_t>(table.data());
    m_tableSize = table.size();
    m_numberOfCards = numberOfCardsFor(m_tableSize, m_regionShift);
    m_maxNumberOfListedCards = maxNumberOfListedCardsFor(m_numberOfCards);
    m_writtenCards.shrink(0);
    m_ownedCards.fill(neverWrittenCard, m_numberOfCards);
    m_cards = m_ownedCards.mutableSpan().data();
    recordAll();
}

void RememberedSlotSet::didMoveAndGrowTable(std::span<const uint8_t> table)
{
    RELEASE_ASSERT(m_cards == m_ownedCards.span().data() && m_regionShift != shiftOfSingleRegion && table.size() >= m_tableSize);
    m_tableStart = std::bit_cast<uintptr_t>(table.data());
    m_tableSize = table.size();
    size_t previousNumberOfCards = std::exchange(m_numberOfCards, numberOfCardsFor(m_tableSize, m_regionShift));
    m_maxNumberOfListedCards = maxNumberOfListedCardsFor(m_numberOfCards);
    m_ownedCards.grow(m_numberOfCards);
    m_cards = m_ownedCards.mutableSpan().data();
    memset(m_cards + previousNumberOfCards, neverWrittenCard, m_numberOfCards - previousNumberOfCards);
}

void RememberedSlotSet::recordAll()
{
    m_listsEveryWrittenCard = false;
    if (m_regionShift == shiftOfSingleRegion) {
        m_cards[0] = writtenCard;
        return;
    }
    memset(m_cards, writtenCard, (m_tableSize + (static_cast<size_t>(1) << m_regionShift) - 1) >> m_regionShift);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
