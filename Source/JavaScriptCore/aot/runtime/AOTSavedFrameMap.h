/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class SavedFrameMap {
public:
    static constexpr unsigned numberOfHeaderWords = 2;

    static std::optional<SavedFrameMap> tryCreate(std::span<const uint32_t> frameMapWordsOfFunction, uint32_t offset)
    {
        if (offset >= frameMapWordsOfFunction.size())
            return std::nullopt;
        std::span<const uint32_t> words = frameMapWordsOfFunction.subspan(offset);
        if (words.size() < numberOfHeaderWords || words[0] > words.size() - 1 || !words[0] || words[1] > words[0] - 1)
            return std::nullopt;
        std::span<const uint32_t> bits = words.subspan(numberOfHeaderWords, words[0] - 1);
        return SavedFrameMap(bits.first(words[1]), bits.subspan(words[1]));
    }

    bool listsValue(uint32_t location) const { return isSet(m_valueBits, location); }
    bool listsNarrowReference(uint32_t narrowLocation) const { return isSet(m_narrowReferenceBits, narrowLocation); }

    template<typename Functor>
    bool forEachValue(const Functor& functor) const { return forEachSetBit(m_valueBits, functor); }

    template<typename Functor>
    bool forEachNarrowReference(const Functor& functor) const { return forEachSetBit(m_narrowReferenceBits, functor); }

private:
    SavedFrameMap(std::span<const uint32_t> valueBits, std::span<const uint32_t> narrowReferenceBits)
        : m_valueBits(valueBits)
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

    std::span<const uint32_t> m_valueBits;
    std::span<const uint32_t> m_narrowReferenceBits;
};

class SavedFrameMapBuilder {
public:
    void addValue(uint32_t location) { set(m_valueBits, location); }
    void addNarrowReference(uint32_t narrowLocation) { set(m_narrowReferenceBits, narrowLocation); }

    Vector<uint32_t, 8> finish() const
    {
        Vector<uint32_t, 8> words;
        words.reserveInitialCapacity(SavedFrameMap::numberOfHeaderWords + m_valueBits.size() + m_narrowReferenceBits.size());
        words.append(static_cast<uint32_t>(1 + m_valueBits.size() + m_narrowReferenceBits.size()));
        words.append(static_cast<uint32_t>(m_valueBits.size()));
        words.appendVector(m_valueBits);
        words.appendVector(m_narrowReferenceBits);
        return words;
    }

private:
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
