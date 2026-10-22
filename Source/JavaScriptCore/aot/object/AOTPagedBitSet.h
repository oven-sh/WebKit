/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <limits>
#include <wtf/Assertions.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// A set of numbers, cut into pages of 64. A short or dense set is its pages, one word each.
// Where most of the numbers lie far apart the words begin with an entry for each page: the index, among these same words,
// of the word that holds the bits of the page. Pages with equal bits share a word, so an empty page costs its entry and nothing else.
// A few numbers that lie far apart are no bits at all: a table with a slot for each, found by a multiplication that sends no two of them to one slot.
// The first word has the multiplier in its lower half and the shift above it, and the slots follow, 32 bits each. A slot without a member holds a number that belongs to another slot,
// so whatever number is asked for, in reach or not, it is a member if and only if its slot holds it.
struct PagedBitSet {
    enum class Entries : uint8_t { None, Narrow, Wide, Hashed };

    static constexpr unsigned logBitsPerPage = 6;
    static constexpr unsigned bitsPerPage = 1u << logBitsPerPage;
    static constexpr unsigned logBytesPerWord = 3;
    static constexpr unsigned logBytesPerNarrowEntry = 0;
    static constexpr unsigned logBytesPerWideEntry = 1;

    // A location is the offset in bytes of the first word in the table that holds the sets, with one of these bits set where the set has entries.
    static constexpr uint32_t narrowEntriesInLocation = static_cast<uint32_t>(Entries::Narrow);
    static constexpr uint32_t wideEntriesInLocation = static_cast<uint32_t>(Entries::Wide);
    static constexpr uint32_t hashedInLocation = static_cast<uint32_t>(Entries::Hashed);
    static constexpr uint32_t entriesMaskInLocation = narrowEntriesInLocation | wideEntriesInLocation;
    static constexpr uint32_t locationOf(uint64_t firstWord, Entries entries) { return static_cast<uint32_t>(firstWord << logBytesPerWord) | static_cast<uint32_t>(entries); }
    static constexpr uint32_t firstWordAt(uint32_t location) { return location >> logBytesPerWord; }
    static constexpr Entries entriesAt(uint32_t location) { return static_cast<Entries>(location & entriesMaskInLocation); }

    static constexpr uint64_t numberOfPages(uint64_t endOfNumbers) { return (endOfNumbers + bitsPerPage - 1) >> logBitsPerPage; }
    static constexpr uint64_t numberOfEntryWords(uint64_t numberOfPages, Entries entries)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(entries != Entries::Hashed);
        if (entries == Entries::None)
            return 0;
        unsigned logEntriesPerWord = logBytesPerWord - (entries == Entries::Wide ? logBytesPerWideEntry : logBytesPerNarrowEntry);
        return (numberOfPages + (1u << logEntriesPerWord) - 1) >> logEntriesPerWord;
    }
    // What an entry can name.
    static constexpr uint64_t maxNumberOfWords(Entries entries) { return 1ull << (8u << (entries == Entries::Wide ? logBytesPerWideEntry : logBytesPerNarrowEntry)); }

    static uint64_t wordOfPage(const uint64_t* words, Entries entries, uint64_t page)
    {
        switch (entries) {
        case Entries::None:
            return page;
        case Entries::Narrow:
            return reinterpret_cast<const uint8_t*>(words)[page];
        case Entries::Wide:
            return reinterpret_cast<const uint16_t*>(words)[page];
        case Entries::Hashed:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // A slot holds a member: its distance from the last one. A set whose first member lies further from its last than a slot can say is not hashed.
    using Slot = uint16_t;
    static constexpr unsigned logBytesPerSlot = 1;
    static constexpr uint32_t maxNumberInSlot = std::numeric_limits<Slot>::max();
    static constexpr ptrdiff_t offsetOfSlots = sizeof(uint64_t);
    static constexpr uint64_t headOfHashedSet(uint32_t multiplier, unsigned shift) { return static_cast<uint64_t>(shift) << 32 | multiplier; }
    static constexpr uint32_t multiplierIn(uint64_t head) { return static_cast<uint32_t>(head); }
    static constexpr unsigned shiftIn(uint64_t head) { return static_cast<unsigned>(head >> 32); }
    static constexpr uint32_t slotOf(uint32_t number, uint32_t multiplier, unsigned shift) { return number * multiplier >> shift; }
    static constexpr uint64_t numberOfSlots(unsigned shift) { return 1ull << (32 - shift); }
    static constexpr uint64_t numberOfWordsOfHashedSet(unsigned shift) { return 1 + ((numberOfSlots(shift) * sizeof(Slot) + sizeof(uint64_t) - 1) >> logBytesPerWord); }

    static bool contains(const uint64_t* words, Entries entries, uint32_t number)
    {
        if (entries == Entries::Hashed)
            return reinterpret_cast<const Slot*>(words + 1)[slotOf(number, multiplierIn(words[0]), shiftIn(words[0]))] == number;
        return words[wordOfPage(words, entries, number >> logBitsPerPage)] >> number % bitsPerPage & 1;
    }
};
static_assert(PagedBitSet::numberOfEntryWords(9, PagedBitSet::Entries::Narrow) == 2 && PagedBitSet::numberOfEntryWords(9, PagedBitSet::Entries::Wide) == 3 && !PagedBitSet::numberOfEntryWords(9, PagedBitSet::Entries::None));
static_assert(PagedBitSet::maxNumberOfWords(PagedBitSet::Entries::Narrow) == 256 && PagedBitSet::maxNumberOfWords(PagedBitSet::Entries::Wide) == 65536 && PagedBitSet::numberOfPages(65) == 2);
static_assert(PagedBitSet::firstWordAt(PagedBitSet::locationOf(5, PagedBitSet::Entries::Wide)) == 5 && PagedBitSet::entriesAt(PagedBitSet::locationOf(5, PagedBitSet::Entries::Wide)) == PagedBitSet::Entries::Wide);
static_assert(PagedBitSet::locationOf(5, PagedBitSet::Entries::None) == 5 << PagedBitSet::logBytesPerWord && PagedBitSet::entriesMaskInLocation < 1u << PagedBitSet::logBytesPerWord);
static_assert(PagedBitSet::narrowEntriesInLocation == 1 && PagedBitSet::wideEntriesInLocation == 2, "each kind of entries has a bit of its own in a location");
static_assert(PagedBitSet::hashedInLocation == (PagedBitSet::narrowEntriesInLocation | PagedBitSet::wideEntriesInLocation), "a hashed set has both");
static_assert(PagedBitSet::numberOfWordsOfHashedSet(31) == 2 && PagedBitSet::numberOfWordsOfHashedSet(30) == 2 && PagedBitSet::numberOfWordsOfHashedSet(28) == 5 && sizeof(PagedBitSet::Slot) == 1u << PagedBitSet::logBytesPerSlot && PagedBitSet::shiftIn(PagedBitSet::headOfHashedSet(0xffffffffu, 31)) == 31);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
