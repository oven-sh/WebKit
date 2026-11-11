/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class SavedFrameMap {
public:
    static constexpr uint32_t hasLongHeaderBit = 1u << 16;
    static constexpr unsigned numberOfHeaderWords = 1;
    static constexpr unsigned numberOfLongHeaderWords = 3;

    static std::optional<SavedFrameMap> tryCreate(std::span<const uint32_t> frameMapWordsOfFunction, uint32_t offset)
    {
        if (offset >= frameMapWordsOfFunction.size())
            return std::nullopt;
        std::span<const uint32_t> words = frameMapWordsOfFunction.subspan(offset);
        uint32_t header = words[0];
        if (header >> 17)
            return std::nullopt;
        bool hasLongHeader = header & hasLongHeaderBit;
        if (hasLongHeader && (words.size() < numberOfLongHeaderWords || header & 0xffff))
            return std::nullopt;
        uint64_t numberOfValueWords = hasLongHeader ? words[1] : header & 0xff;
        uint64_t numberOfNarrowReferenceWords = hasLongHeader ? words[2] : header >> 8 & 0xff;
        std::span<const uint32_t> bits = words.subspan(hasLongHeader ? numberOfLongHeaderWords : numberOfHeaderWords);
        if (numberOfValueWords + numberOfNarrowReferenceWords > bits.size())
            return std::nullopt;
        return SavedFrameMap(words.data(), bits.first(numberOfValueWords), bits.subspan(numberOfValueWords, numberOfNarrowReferenceWords));
    }

    static SavedFrameMap fromFirstWord(const uint32_t* firstWord) { return *tryCreate(unsafeMakeSpan(firstWord, SIZE_MAX / sizeof(uint32_t)), 0); }
    size_t numberOfWords() const { return m_narrowReferenceBits.data() + m_narrowReferenceBits.size() - m_firstWord; }

    bool listsValue(uint32_t location) const { return isSet(m_valueBits, location); }
    bool listsNarrowReference(uint32_t narrowLocation) const { return isSet(m_narrowReferenceBits, narrowLocation); }

    template<typename Functor>
    bool forEachValue(const Functor& functor) const { return forEachSetBit(m_valueBits, functor); }

    template<typename Functor>
    bool forEachNarrowReference(const Functor& functor) const { return forEachSetBit(m_narrowReferenceBits, functor); }

private:
    SavedFrameMap(const uint32_t* firstWord, std::span<const uint32_t> valueBits, std::span<const uint32_t> narrowReferenceBits)
        : m_firstWord(firstWord)
        , m_valueBits(valueBits)
        , m_narrowReferenceBits(narrowReferenceBits)
    {
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
};

class SavedFrameMapBuilder {
public:
    void addValue(uint32_t location) { set(m_valueBits, location); }
    void addNarrowReference(uint32_t narrowLocation) { set(m_narrowReferenceBits, narrowLocation); }
    bool listsReferenceIn(uint32_t location) const { return isSet(m_valueBits, location) || isSet(m_narrowReferenceBits, 2 * location) || (location && isSet(m_narrowReferenceBits, 2 * location - 1)); }

    Vector<uint32_t, 8> finish() const
    {
        Vector<uint32_t, 8> words;
        words.reserveInitialCapacity(SavedFrameMap::numberOfLongHeaderWords + m_valueBits.size() + m_narrowReferenceBits.size());
        if (std::max(m_valueBits.size(), m_narrowReferenceBits.size()) <= 0xff)
            words.append(static_cast<uint32_t>(m_valueBits.size() | m_narrowReferenceBits.size() << 8));
        else {
            words.append(SavedFrameMap::hasLongHeaderBit);
            words.append(static_cast<uint32_t>(m_valueBits.size()));
            words.append(static_cast<uint32_t>(m_narrowReferenceBits.size()));
        }
        words.appendVector(m_valueBits);
        words.appendVector(m_narrowReferenceBits);
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
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
