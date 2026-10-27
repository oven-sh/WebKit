/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// The first word, a byte each: how many words list values, narrow references, other live words. Then the flags. Those words follow, in that order.
// Where a number does not fit its byte, the three numbers are the three words behind the first.
class SavedFrameMap {
public:
    static constexpr uint32_t listsEveryLiveWordBit = 1u << 24;
    static constexpr uint32_t hasLongHeaderBit = 1u << 25;
    static constexpr unsigned numberOfHeaderWords = 1;
    static constexpr unsigned numberOfLongHeaderWords = 4;

    static std::optional<SavedFrameMap> tryCreate(std::span<const uint32_t> frameMapWordsOfFunction, uint32_t offset)
    {
        if (offset >= frameMapWordsOfFunction.size())
            return std::nullopt;
        std::span<const uint32_t> words = frameMapWordsOfFunction.subspan(offset);
        uint32_t header = words[0];
        if (header >> 26)
            return std::nullopt;
        bool hasLongHeader = header & hasLongHeaderBit;
        if (hasLongHeader && (words.size() < numberOfLongHeaderWords || header & 0xffffff))
            return std::nullopt;
        uint64_t numberOfValueWords = hasLongHeader ? words[1] : header & 0xff;
        uint64_t numberOfNarrowReferenceWords = hasLongHeader ? words[2] : header >> 8 & 0xff;
        uint64_t numberOfOtherLiveWordWords = hasLongHeader ? words[3] : header >> 16 & 0xff;
        std::span<const uint32_t> bits = words.subspan(hasLongHeader ? numberOfLongHeaderWords : numberOfHeaderWords);
        if (numberOfValueWords + numberOfNarrowReferenceWords + numberOfOtherLiveWordWords > bits.size())
            return std::nullopt;
        return SavedFrameMap(words.data(), bits.first(numberOfValueWords), bits.subspan(numberOfValueWords, numberOfNarrowReferenceWords), bits.subspan(numberOfValueWords + numberOfNarrowReferenceWords, numberOfOtherLiveWordWords), header & listsEveryLiveWordBit);
    }

    // What tryCreate() accepted.
    static SavedFrameMap fromFirstWord(const uint32_t* firstWord) { return *tryCreate(unsafeMakeSpan(firstWord, SIZE_MAX / sizeof(uint32_t)), 0); }
    const uint32_t* firstWord() const { return m_firstWord; }
    size_t numberOfWords() const { return m_otherLiveWordBits.data() + m_otherLiveWordBits.size() - m_firstWord; }

    bool listsValue(uint32_t location) const { return isSet(m_valueBits, location); }
    bool listsNarrowReference(uint32_t narrowLocation) const { return isSet(m_narrowReferenceBits, narrowLocation); }
    bool listsOtherLiveWord(uint32_t location) const { return isSet(m_otherLiveWordBits, location); }
    bool listsEveryLiveWord() const { return m_listsEveryLiveWord; }

    template<typename Functor>
    bool forEachValue(const Functor& functor) const { return forEachSetBit(m_valueBits, functor); }

    template<typename Functor>
    bool forEachNarrowReference(const Functor& functor) const { return forEachSetBit(m_narrowReferenceBits, functor); }

    template<typename Functor>
    bool forEachOtherLiveWord(const Functor& functor) const { return forEachSetBit(m_otherLiveWordBits, functor); }

    // Of location 0, if the frame has it, and of the locations from first to last: those that hold nothing live. None where the map does not list every live word.
    template<typename Functor>
    bool forEachDeadWord(bool hasLocationZero, uint32_t first, uint32_t last, const Functor& functor) const
    {
        if (!m_listsEveryLiveWord)
            return true;
        for (uint32_t index = 0; index <= last / 32; ++index) {
            uint32_t candidates = UINT32_MAX;
            if (first > 32 * index)
                candidates = first - 32 * index < 32 ? UINT32_MAX << (first - 32 * index) : 0;
            if (last - 32 * index < 31)
                candidates &= UINT32_MAX >> (31 - (last - 32 * index));
            if (!index && hasLocationZero)
                candidates |= 1;
            for (uint32_t deadWords = candidates & ~liveWordsAt(index); deadWords; deadWords &= deadWords - 1) {
                if (!functor(32 * index + static_cast<uint32_t>(std::countr_zero(deadWords))))
                    return false;
            }
        }
        return true;
    }

private:
    SavedFrameMap(const uint32_t* firstWord, std::span<const uint32_t> valueBits, std::span<const uint32_t> narrowReferenceBits, std::span<const uint32_t> otherLiveWordBits, bool listsEveryLiveWord)
        : m_firstWord(firstWord)
        , m_valueBits(valueBits)
        , m_narrowReferenceBits(narrowReferenceBits)
        , m_otherLiveWordBits(otherLiveWordBits)
        , m_listsEveryLiveWord(listsEveryLiveWord)
    {
    }

    static uint32_t wordAt(std::span<const uint32_t> bits, size_t index) { return index < bits.size() ? bits[index] : 0; }

    // A bit for each of the locations from 32 * index on.
    uint32_t liveWordsAt(uint32_t index) const
    {
        uint32_t liveWords = wordAt(m_valueBits, index) | wordAt(m_otherLiveWordBits, index);
        if (m_narrowReferenceBits.empty())
            return liveWords;
        // Location l holds the narrow locations 2 * l - 1 and 2 * l.
        uint64_t halves = wordAt(m_narrowReferenceBits, 2 * index) | static_cast<uint64_t>(wordAt(m_narrowReferenceBits, 2 * index + 1)) << 32;
        uint64_t bits = halves | halves << 1 | (index ? wordAt(m_narrowReferenceBits, 2 * index - 1) >> 31 : 0);
        bits &= 0x5555555555555555ULL;
        bits = (bits | bits >> 1) & 0x3333333333333333ULL;
        bits = (bits | bits >> 2) & 0x0f0f0f0f0f0f0f0fULL;
        bits = (bits | bits >> 4) & 0x00ff00ff00ff00ffULL;
        bits = (bits | bits >> 8) & 0x0000ffff0000ffffULL;
        bits = (bits | bits >> 16) & 0x00000000ffffffffULL;
        return liveWords | static_cast<uint32_t>(bits);
    }

    static bool isSet(std::span<const uint32_t> bits, uint32_t location)
    {
        return location / 32 < bits.size() && bits[location / 32] >> location % 32 & 1;
    }

    template<typename Functor>
    static bool forEachSetBit(std::span<const uint32_t> bits, const Functor& functor)
    {
        bool didVisitAll = true;
        WTF::forEachSetBit(bits, [&](size_t location) {
            didVisitAll = functor(static_cast<uint32_t>(location));
            return didVisitAll ? IterationStatus::Continue : IterationStatus::Done;
        });
        return didVisitAll;
    }

    const uint32_t* m_firstWord;
    std::span<const uint32_t> m_valueBits;
    std::span<const uint32_t> m_narrowReferenceBits;
    std::span<const uint32_t> m_otherLiveWordBits;
    bool m_listsEveryLiveWord;
};

class SavedFrameMapBuilder {
public:
    void addValue(uint32_t location) { set(m_valueBits, location); }
    void addNarrowReference(uint32_t narrowLocation) { set(m_narrowReferenceBits, narrowLocation); }
    // A word that holds no reference, and that the function may read before it writes it, if it goes on from here.
    void addOtherLiveWord(uint32_t location) { set(m_otherLiveWordBits, location); }
    void setListsEveryLiveWord() { m_listsEveryLiveWord = true; }
    bool listsReferenceIn(uint32_t location) const { return isSet(m_valueBits, location) || isSet(m_narrowReferenceBits, 2 * location) || (location && isSet(m_narrowReferenceBits, 2 * location - 1)); }

    Vector<uint32_t, 8> finish() const
    {
        Vector<uint32_t, 8> words;
        words.reserveInitialCapacity(SavedFrameMap::numberOfLongHeaderWords + m_valueBits.size() + m_narrowReferenceBits.size() + m_otherLiveWordBits.size());
        uint32_t flags = m_listsEveryLiveWord ? SavedFrameMap::listsEveryLiveWordBit : 0;
        if (std::max({ m_valueBits.size(), m_narrowReferenceBits.size(), m_otherLiveWordBits.size() }) <= 0xff)
            words.append(static_cast<uint32_t>(m_valueBits.size() | m_narrowReferenceBits.size() << 8 | m_otherLiveWordBits.size() << 16 | flags));
        else {
            words.append(flags | SavedFrameMap::hasLongHeaderBit);
            words.append(static_cast<uint32_t>(m_valueBits.size()));
            words.append(static_cast<uint32_t>(m_narrowReferenceBits.size()));
            words.append(static_cast<uint32_t>(m_otherLiveWordBits.size()));
        }
        words.appendVector(m_valueBits);
        words.appendVector(m_narrowReferenceBits);
        words.appendVector(m_otherLiveWordBits);
        return words;
    }

private:
    static bool isSet(const Vector<uint32_t, 4>& bits, uint32_t location)
    {
        return location / 32 < bits.size() && bits[location / 32] >> location % 32 & 1;
    }

    static void set(Vector<uint32_t, 4>& bits, uint32_t location)
    {
        while (bits.size() <= location / 32)
            bits.append(0);
        bits[location / 32] |= 1u << location % 32;
    }

    Vector<uint32_t, 4> m_valueBits;
    Vector<uint32_t, 4> m_narrowReferenceBits;
    Vector<uint32_t, 4> m_otherLiveWordBits;
    bool m_listsEveryLiveWord { false };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
