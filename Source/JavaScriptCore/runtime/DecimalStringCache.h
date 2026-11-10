/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/Decimal128.h>
#include <JavaScriptCore/JSString.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

// The strings of Decimals that were converted lately, as NumericStrings has those of Numbers.
class DecimalStringCache {
    WTF_MAKE_TZONE_ALLOCATED(DecimalStringCache);
public:
    enum class Notation : uint8_t { Standard, Exponential, Fixed, Precision };

    template<typename Function>
    ALWAYS_INLINE JSString* get(VM& vm, const Decimal128& value, Notation notation, unsigned digits, Decimal128::RoundingMode mode, const Function& convert)
    {
        ASSERT(digits <= maximumDigits);
        uint32_t format = static_cast<uint32_t>(notation) | static_cast<uint32_t>(mode) << 2 | digits << 5;
        auto& entry = m_entries[(value.hash() ^ format * 0x9e3779b1u >> 16) % size];
        if (entry.string && entry.format == format && entry.value.isSameValueAs(value)) [[likely]]
            return entry.string;
        String text = convert();
        ASSERT(!text.isNull());
        JSString* string = jsString(vm, WTF::move(text));
        entry = { value, string, format };
        return string;
    }

    JSString* toString(VM& vm, const Decimal128& value)
    {
        return get(vm, value, Notation::Standard, 0, { }, [&] { return value.toString(); });
    }

    JSString* toExponential(VM& vm, const Decimal128& value)
    {
        return get(vm, value, Notation::Exponential, 0, { }, [&] { return value.toExponentialString(); });
    }

    static constexpr unsigned maximumDigits = 100;

    void clearOnGarbageCollection() { m_entries.fill({ }); }

    template<typename Visitor>
    void visitAggregate(Visitor& visitor)
    {
        for (auto& entry : m_entries)
            visitor.appendUnbarriered(entry.string);
    }

private:
    static constexpr unsigned size = 512;

    struct Entry {
        Decimal128 value;
        JSString* string { nullptr };
        uint32_t format { 0 };
    };

    std::array<Entry, size> m_entries { };
};

} // namespace JSC
