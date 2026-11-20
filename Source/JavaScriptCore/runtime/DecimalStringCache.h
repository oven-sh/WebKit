/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/Decimal128.h>
#include <JavaScriptCore/JSString.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UnalignedAccess.h>

namespace JSC {

// The strings of Decimals that were converted lately, as NumericStrings has those of Numbers.
class DecimalStringCache {
    WTF_MAKE_TZONE_ALLOCATED(DecimalStringCache);
public:
    ALWAYS_INLINE JSString* toString(VM& vm, const Decimal128& value) { return get(vm, value, formatOf(Notation::Standard)); }
    ALWAYS_INLINE JSString* toExponential(VM& vm, const Decimal128& value) { return get(vm, value, formatOf(Notation::Exponential)); }
    ALWAYS_INLINE JSString* toFixed(VM& vm, const Decimal128& value, unsigned digits, Decimal128::RoundingMode mode) { return get(vm, value, formatOf(Notation::Fixed, digits, mode)); }
    ALWAYS_INLINE JSString* toPrecision(VM& vm, const Decimal128& value, unsigned digits, Decimal128::RoundingMode mode) { return get(vm, value, formatOf(Notation::Precision, digits, mode)); }

    static constexpr unsigned maximumDigits = 100;

    void clearOnGarbageCollection() { m_sets.fill({ }); }

    template<typename Visitor>
    void visitAggregate(Visitor& visitor)
    {
        for (auto& set : m_sets) {
            for (auto& entry : set)
                visitor.appendUnbarriered(entry.string);
        }
    }

private:
    enum class Notation : uint8_t { Standard, Exponential, Fixed, Precision };

    // The notation in 2 bits, the mode in 3 and the digits.
    static constexpr uint32_t formatOf(Notation notation, unsigned digits = 0, Decimal128::RoundingMode mode = { })
    {
        ASSERT(digits <= maximumDigits);
        ASSERT(static_cast<unsigned>(mode) < 8);
        return static_cast<uint32_t>(notation) | static_cast<uint32_t>(mode) << 2 | digits << 5;
    }

    ALWAYS_INLINE JSString* get(VM& vm, const Decimal128& value, uint32_t format)
    {
        // Two entries in a set: two values of one set that take turns would miss each time otherwise.
        auto& set = m_sets[value.hash(static_cast<uint64_t>(format) << 32) % m_sets.size()];
        if (set[0].is(value, format)) [[likely]]
            return set[0].string;
        if (set[1].is(value, format))
            return set[1].string;
        return add(vm, set, value, format);
    }

    struct Entry {
        Decimal128 value;
        JSString* string { nullptr };
        uint32_t format { 0 };

        bool is(const Decimal128& otherValue, uint32_t otherFormat) const { return string && format == otherFormat && value.isSameValueAs(otherValue); }
    };
    // The one added last is the first.
    using Set = std::array<Entry, 2>;

    // Out of line, so that finding a string is inline.
    static JSString* add(VM&, Set&, const Decimal128&, uint32_t format);

    std::array<Set, 512> m_sets { };
};

// The values of short literals, for code that evaluates one each time. By the characters, so the collector has no part in it.
class DecimalLiteralCache {
    WTF_MAKE_TZONE_ALLOCATED(DecimalLiteralCache);
public:
    ALWAYS_INLINE Decimal128 get(StringView text)
    {
        if (!text.is8Bit() || text.isEmpty() || text.length() > sizeof(uint64_t)) [[unlikely]]
            return parse(text);
        uint64_t characters = pack(text.span8());
        auto& set = m_sets[(characters * 0x9e3779b97f4a7c15ull) >> (64 - log2OfNumberOfSets)];
        if (set[0].is(characters, text.length())) [[likely]]
            return set[0].value;
        if (set[1].is(characters, text.length()))
            return set[1].value;
        return add(set, characters, text);
    }

private:
    struct Entry {
        uint64_t characters { 0 };
        unsigned length { 0 };
        Decimal128 value;

        bool is(uint64_t otherCharacters, unsigned otherLength) const { return characters == otherCharacters && length == otherLength; }
    };
    // The one added last is the first.
    using Set = std::array<Entry, 2>;

    // Each at its place. The loads overlap where there are fewer than they cover.
    static ALWAYS_INLINE uint64_t pack(std::span<const Latin1Character> text)
    {
        size_t size = text.size();
        if (size >= 4)
            return WTF::unalignedLoad<uint32_t>(text.data()) | static_cast<uint64_t>(WTF::unalignedLoad<uint32_t>(text.last(4).data())) << 8 * (size - 4);
        return text.front() | text[size / 2] << 8 * (size / 2) | text.back() << 8 * (size - 1);
    }

    // NaN if it is no number.
    static Decimal128 parse(StringView);
    static Decimal128 add(Set&, uint64_t characters, StringView);

    static constexpr unsigned log2OfNumberOfSets = 5;
    std::array<Set, 1 << log2OfNumberOfSets> m_sets { };
};

} // namespace JSC
