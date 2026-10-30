/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSystemResources.h"
#include <array>
#include <bit>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

// Words named by positions below 2^31. The memory comes in pieces, each twice as large as the one before, and never moves. A page of a piece takes memory when it is first written to.
class ValueFlowWords {
    WTF_MAKE_NONCOPYABLE(ValueFlowWords);
public:
    ValueFlowWords() = default;
    ValueFlowWords(ValueFlowWords&& other) { *this = WTF::move(other); }
    ValueFlowWords& operator=(ValueFlowWords&& other)
    {
        release();
        m_pieces = std::exchange(other.m_pieces, { });
        m_wordAtPositionZero = std::exchange(other.m_wordAtPositionZero, { });
        m_next = std::exchange(other.m_next, 1);
        m_endOfPiece = std::exchange(other.m_endOfPiece, 0);
        m_numberOfWordsTaken = std::exchange(other.m_numberOfWordsTaken, 0);
        return *this;
    }
    ~ValueFlowWords() { release(); }

    ALWAYS_INLINE uint32_t* at(uint32_t position) const { return reinterpret_cast<uint32_t*>(m_wordAtPositionZero[pieceOf(position)] + static_cast<uintptr_t>(position) * sizeof(uint32_t)); }

    // Never position 0. The words are zero unless the caller has written to them before.
    uint32_t take(uint64_t numberOfWords)
    {
        if (m_next + numberOfWords > m_endOfPiece) [[unlikely]]
            takePieceFor(numberOfWords);
        m_numberOfWordsTaken += numberOfWords;
        return static_cast<uint32_t>(std::exchange(m_next, m_next + numberOfWords));
    }

    size_t bytesTaken() const { return m_numberOfWordsTaken * sizeof(uint32_t); }

private:
    static constexpr unsigned shiftOfFirstPiece = 12;
    static constexpr unsigned numberOfPieces = 31 - shiftOfFirstPiece;
    static unsigned pieceOf(uint32_t position) { return std::bit_width((position >> shiftOfFirstPiece) + 1) - 1; }
    static uint64_t firstPositionOf(unsigned piece) { return ((1ull << piece) - 1) << shiftOfFirstPiece; }
    static uint64_t numberOfWordsIn(unsigned piece) { return 1ull << (piece + shiftOfFirstPiece); }

    NEVER_INLINE void takePieceFor(uint64_t numberOfWords)
    {
        unsigned piece = m_endOfPiece ? pieceOf(static_cast<uint32_t>(m_endOfPiece - 1)) + 1 : 0;
        while (piece < numberOfPieces && numberOfWordsIn(piece) - !piece < numberOfWords)
            ++piece;
        if (piece >= numberOfPieces) [[unlikely]]
            exitProgramTooLarge();
        m_pieces[piece] = static_cast<uint32_t*>(allocatePages(roundUpToMultipleOf(pageSize(), numberOfWordsIn(piece) * sizeof(uint32_t))));
        m_wordAtPositionZero[piece] = reinterpret_cast<uintptr_t>(m_pieces[piece]) - firstPositionOf(piece) * sizeof(uint32_t);
        m_next = firstPositionOf(piece) + !piece;
        m_endOfPiece = firstPositionOf(piece) + numberOfWordsIn(piece);
    }

    void release()
    {
        for (unsigned piece = 0; piece < numberOfPieces; ++piece) {
            if (m_pieces[piece])
                releasePages(m_pieces[piece], roundUpToMultipleOf(pageSize(), numberOfWordsIn(piece) * sizeof(uint32_t)));
        }
        m_pieces = { };
    }

    std::array<uint32_t*, numberOfPieces> m_pieces { };
    // Where position 0 would lie if the piece began there.
    std::array<uintptr_t, numberOfPieces> m_wordAtPositionZero { };
    uint64_t m_next { 1 };
    uint64_t m_endOfPiece { 0 };
    uint64_t m_numberOfWordsTaken { 0 };
};

// Many lists of distinct numbers below 2^31. A list is named by 32 bits, 0 for the empty one, and a list of one number is the number itself with the highest bit set.
// Up to sixteen numbers stand side by side behind their count and are compared one by one. More are dealt out over eight lists by three bits of a hash of the number.
// So a number takes about six bytes however long its list is, no list needs more than 17 words in one piece, and what a list has outgrown fits the next list that grows.
// One thread writes at a time, and nobody reads meanwhile.
class ValueFlowNumberLists {
public:
    void prefetch(uint32_t list) const
    {
        if (list && !(list & isOneNumber))
            __builtin_prefetch(m_words.at(list));
    }

    // For a list whose first words are at hand.
    void prefetchPlaceOf(uint32_t list, uint32_t number) const { prefetch(listThatMayHold(list, number)); }

    bool contains(uint32_t list, uint32_t number) const
    {
        list = listThatMayHold(list, number);
        if (!list || (list & isOneNumber))
            return list == (number | isOneNumber);
        const uint32_t* words = m_words.at(list);
        return holds(words + 1, words[0], number);
    }

    // The empty list and a list of one number take no memory: the name says it all.
    static bool isAllInName(uint32_t list) { return !list || (list & isOneNumber); }
    static bool nameHolds(uint32_t list, uint32_t number) { return list == (number | isOneNumber); }

    // False if the list had the number.
    bool add(uint32_t& list, uint32_t number)
    {
        ASSERT(!(number & isOneNumber));
        if (!addBehind<true>(list, number, firstShift))
            return false;
        ++m_numberOfNumbers;
        return true;
    }

    void addWhatListLacks(uint32_t& list, uint32_t number)
    {
        ASSERT(!contains(list, number));
        addBehind<false>(list, number, firstShift);
        ++m_numberOfNumbers;
    }

    void clear(uint32_t& list) { m_numberOfNumbers -= giveBack(std::exchange(list, 0)); }

    // False if the list lacked the number. The others keep their order.
    bool remove(uint32_t& list, uint32_t number)
    {
        uint32_t* name = &list;
        for (unsigned shift = firstShift; *name && !(*name & isOneNumber); shift -= bitsDealtBy) {
            uint32_t* words = m_words.at(*name);
            if (words[0] == isDealtOut) {
                name = &words[1 + listDealtTo(number, shift)];
                continue;
            }
            uint32_t size = words[0];
            uint32_t position = 1;
            while (position <= size && words[position] != number)
                ++position;
            if (position > size)
                return false;
            // How many numbers a list holds says how much room it has, so a list that needs less room moves.
            uint32_t shrunk = *name;
            if (size == 2)
                shrunk = words[3 - position] | isOneNumber;
            else {
                uint32_t* remaining = words;
                if (roomFor(size - 1) != roomFor(size)) {
                    shrunk = takeList(roomFor(size - 1));
                    remaining = m_words.at(shrunk);
                    for (uint32_t index = 1; index < position; ++index)
                        remaining[index] = words[index];
                }
                for (uint32_t index = position; index < size; ++index)
                    remaining[index] = words[index + 1];
                remaining[0] = size - 1;
            }
            if (shrunk != *name)
                giveBackList(std::exchange(*name, shrunk), roomFor(size));
            --m_numberOfNumbers;
            return true;
        }
        if (*name != (number | isOneNumber))
            return false;
        *name = 0;
        --m_numberOfNumbers;
        return true;
    }

    // Of up to sixteen numbers the latest comes first.
    void forEach(uint32_t list, NOESCAPE const auto& functor) const
    {
        if (!list)
            return;
        if (list & isOneNumber) {
            functor(list & ~isOneNumber);
            return;
        }
        const uint32_t* words = m_words.at(list);
        if (words[0] == isDealtOut) {
            for (unsigned index = 0; index < listsDealtTo; ++index)
                forEach(words[1 + index], functor);
            return;
        }
        for (uint32_t index = words[0]; index; --index)
            functor(words[index]);
    }

    size_t numberOfNumbers() const { return m_numberOfNumbers; }
    size_t bytesInUse() const { return m_numberOfNumbers * sizeof(uint32_t); }
    size_t memoryInBytes() const { return m_words.bytesTaken(); }

private:
    static constexpr uint32_t isOneNumber = 1u << 31;
    static constexpr uint32_t isDealtOut = UINT32_MAX;
    static constexpr uint32_t mostSideBySide = 16;
    static constexpr unsigned bitsDealtBy = 3;
    static constexpr unsigned listsDealtTo = 1 << bitsDealtBy;
    static constexpr unsigned firstShift = 32 - bitsDealtBy;

    static uint32_t roomFor(uint32_t size)
    {
        if (size <= 4)
            return size;
        uint32_t powerOfTwo = std::bit_ceil(size);
        return size <= powerOfTwo / 4 * 3 ? powerOfTwo / 4 * 3 : powerOfTwo;
    }
    // Numbers that differ have hashes that differ. Lists that 30 bits have dealt out hold four numbers at the most.
    static unsigned listDealtTo(uint32_t number, unsigned shift) { return number * 0x9e3779b1u >> shift & (listsDealtTo - 1); }

    static ALWAYS_INLINE bool holds(const uint32_t* numbers, uint32_t size, uint32_t number)
    {
        for (uint32_t index = 0; index < size; ++index) {
            if (numbers[index] == number)
                return true;
        }
        return false;
    }

    ALWAYS_INLINE uint32_t listThatMayHold(uint32_t list, uint32_t number) const
    {
        for (unsigned shift = firstShift; list && !(list & isOneNumber); shift -= bitsDealtBy) {
            const uint32_t* words = m_words.at(list);
            if (words[0] != isDealtOut) [[likely]]
                break;
            list = words[1 + listDealtTo(number, shift)];
        }
        return list;
    }

    template<bool mayHold>
    bool addBehind(uint32_t& list, uint32_t number, unsigned shift)
    {
        if (!list) {
            list = number | isOneNumber;
            return true;
        }
        if (list & isOneNumber) {
            if (mayHold && list == (number | isOneNumber))
                return false;
            uint32_t two = takeList(2);
            uint32_t* words = m_words.at(two);
            words[0] = 2;
            words[1] = list & ~isOneNumber;
            words[2] = number;
            list = two;
            return true;
        }
        uint32_t* words = m_words.at(list);
        uint32_t size = words[0];
        if (size == isDealtOut)
            return addBehind<mayHold>(words[1 + listDealtTo(number, shift)], number, shift - bitsDealtBy);
        if (mayHold && holds(words + 1, size, number))
            return false;
        uint32_t room = roomFor(size);
        if (size < room) {
            words[1 + size] = number;
            words[0] = size + 1;
            return true;
        }
        if (size < mostSideBySide) {
            uint32_t larger = takeList(roomFor(size + 1));
            uint32_t* wordsOfLarger = m_words.at(larger);
            wordsOfLarger[0] = size + 1;
            for (uint32_t index = 1; index <= size; ++index)
                wordsOfLarger[index] = words[index];
            wordsOfLarger[1 + size] = number;
            giveBackList(list, room);
            list = larger;
            return true;
        }
        uint32_t dealtOut = takeList(listsDealtTo);
        uint32_t* wordsDealtOut = m_words.at(dealtOut);
        wordsDealtOut[0] = isDealtOut;
        for (unsigned index = 0; index < listsDealtTo; ++index)
            wordsDealtOut[1 + index] = 0;
        for (uint32_t index = 1; index <= size; ++index)
            addBehind<false>(wordsDealtOut[1 + listDealtTo(words[index], shift)], words[index], shift - bitsDealtBy);
        addBehind<false>(wordsDealtOut[1 + listDealtTo(number, shift)], number, shift - bitsDealtBy);
        giveBackList(list, room);
        list = dealtOut;
        return true;
    }

    uint32_t takeList(uint32_t room)
    {
        uint32_t& firstFree = m_firstFree[room];
        if (firstFree)
            return std::exchange(firstFree, *m_words.at(firstFree));
        return m_words.take(1 + room);
    }

    void giveBackList(uint32_t list, uint32_t room)
    {
        uint32_t& firstFree = m_firstFree[room];
        *m_words.at(list) = firstFree;
        firstFree = list;
    }

    size_t giveBack(uint32_t list)
    {
        if (isAllInName(list))
            return !!list;
        const uint32_t* words = m_words.at(list);
        if (uint32_t size = words[0]; size != isDealtOut) {
            giveBackList(list, roomFor(size));
            return size;
        }
        size_t numberOfNumbers = 0;
        for (unsigned index = 0; index < listsDealtTo; ++index)
            numberOfNumbers += giveBack(words[1 + index]);
        giveBackList(list, listsDealtTo);
        return numberOfNumbers;
    }

    ValueFlowWords m_words;
    std::array<uint32_t, mostSideBySide + 1> m_firstFree { };
    size_t m_numberOfNumbers { 0 };
};

// Many maps from 32-bit numbers to 32-bit values, laid out as the lists above: a map is named by 32 bits, 0 for the empty one. Up to sixteen keys stand side by side behind their count, their values behind them;
// more are dealt out over eight maps by three bits of a hash of the key. A key with its value takes about eleven bytes. One thread writes at a time, and nobody reads meanwhile.
class ValueFlowNumberMaps {
public:
    const uint32_t* find(uint32_t map, uint32_t key) const
    {
        for (unsigned shift = firstShift; map; shift -= bitsDealtBy) {
            const uint32_t* words = m_words.at(map);
            if (words[0] == isDealtOut) {
                map = words[1 + mapDealtTo(key, shift)];
                continue;
            }
            for (uint32_t index = 0; index < words[0]; ++index) {
                if (words[1 + index] == key)
                    return words + 1 + roomFor(words[0]) + index;
            }
            return nullptr;
        }
        return nullptr;
    }

    // The value of a new key is 0. The reference holds until the next key is added.
    uint32_t& ensure(uint32_t& map, uint32_t key) { return ensureBehind(map, key, firstShift); }

    size_t bytesInUse() const { return m_numberOfKeys * 2 * sizeof(uint32_t); }
    size_t memoryInBytes() const { return m_words.bytesTaken(); }

private:
    static constexpr uint32_t isDealtOut = UINT32_MAX;
    static constexpr uint32_t mostSideBySide = 16;
    static constexpr unsigned bitsDealtBy = 3;
    static constexpr unsigned mapsDealtTo = 1 << bitsDealtBy;
    static constexpr unsigned firstShift = 32 - bitsDealtBy;

    static uint32_t roomFor(uint32_t size)
    {
        if (size <= 4)
            return size;
        uint32_t powerOfTwo = std::bit_ceil(size);
        return size <= powerOfTwo / 4 * 3 ? powerOfTwo / 4 * 3 : powerOfTwo;
    }
    // Keys that differ have hashes that differ. Maps that 30 bits have dealt out hold four keys at the most.
    static unsigned mapDealtTo(uint32_t key, unsigned shift) { return key * 0x9e3779b1u >> shift & (mapsDealtTo - 1); }

    uint32_t& ensureBehind(uint32_t& map, uint32_t key, unsigned shift)
    {
        if (!map) {
            map = takeWords(3);
            uint32_t* words = m_words.at(map);
            words[0] = 1;
            words[1] = key;
            words[2] = 0;
            ++m_numberOfKeys;
            return words[2];
        }
        uint32_t* words = m_words.at(map);
        uint32_t size = words[0];
        if (size == isDealtOut)
            return ensureBehind(words[1 + mapDealtTo(key, shift)], key, shift - bitsDealtBy);
        uint32_t room = roomFor(size);
        for (uint32_t index = 0; index < size; ++index) {
            if (words[1 + index] == key)
                return words[1 + room + index];
        }
        if (size == mostSideBySide) {
            uint32_t dealtOut = takeWords(1 + mapsDealtTo);
            uint32_t* wordsDealtOut = m_words.at(dealtOut);
            wordsDealtOut[0] = isDealtOut;
            for (unsigned index = 0; index < mapsDealtTo; ++index)
                wordsDealtOut[1 + index] = 0;
            m_numberOfKeys -= size;
            for (uint32_t index = 0; index < size; ++index)
                ensureBehind(wordsDealtOut[1 + mapDealtTo(words[1 + index], shift)], words[1 + index], shift - bitsDealtBy) = words[1 + room + index];
            giveBackWords(map, 1 + 2 * room);
            map = dealtOut;
            return ensureBehind(wordsDealtOut[1 + mapDealtTo(key, shift)], key, shift - bitsDealtBy);
        }
        if (size == room) {
            uint32_t roomOfLarger = roomFor(size + 1);
            uint32_t larger = takeWords(1 + 2 * roomOfLarger);
            uint32_t* wordsOfLarger = m_words.at(larger);
            for (uint32_t index = 0; index < size; ++index) {
                wordsOfLarger[1 + index] = words[1 + index];
                wordsOfLarger[1 + roomOfLarger + index] = words[1 + room + index];
            }
            giveBackWords(map, 1 + 2 * room);
            map = larger;
            words = wordsOfLarger;
            room = roomOfLarger;
        }
        words[0] = size + 1;
        words[1 + size] = key;
        words[1 + room + size] = 0;
        ++m_numberOfKeys;
        return words[1 + room + size];
    }

    uint32_t takeWords(uint32_t numberOfWords)
    {
        uint32_t& firstFree = m_firstFree[numberOfWords];
        if (firstFree)
            return std::exchange(firstFree, *m_words.at(firstFree));
        return m_words.take(numberOfWords);
    }

    void giveBackWords(uint32_t position, uint32_t numberOfWords)
    {
        uint32_t& firstFree = m_firstFree[numberOfWords];
        *m_words.at(position) = firstFree;
        firstFree = position;
    }

    ValueFlowWords m_words;
    std::array<uint32_t, 2 + 2 * mostSideBySide> m_firstFree { };
    size_t m_numberOfKeys { 0 };
};

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
