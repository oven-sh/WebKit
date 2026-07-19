/*
 *  Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "JSString.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringConcatenate.h>

namespace JSC {

// A StringBuilder drop-in for sites that end with jsString(vm, builder.toString()).
// Short all-Latin-1 results (≤ maxBigInlineLength8) accumulate in a stack buffer
// and emit an inline JSString cell directly from toJS(), touching no malloc.
class JSStringBuilder {
    WTF_MAKE_NONCOPYABLE(JSStringBuilder);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    static constexpr unsigned inlineCapacity = JSString::maxBigInlineLength8;

    JSStringBuilder() = default;
    explicit JSStringBuilder(OverflowPolicy policy)
        : m_builder(policy)
    {
    }

    ALWAYS_INLINE void append(Latin1Character c)
    {
        if (m_spilled) {
            m_builder.append(c);
            return;
        }
        if (m_inlineLength < inlineCapacity) [[likely]] {
            m_inline[m_inlineLength++] = c;
            return;
        }
        spill();
        m_builder.append(c);
    }

    ALWAYS_INLINE void append(char c) { append(byteCast<Latin1Character>(c)); }

    ALWAYS_INLINE void append(char16_t c)
    {
        if (m_spilled) {
            m_builder.append(c);
            return;
        }
        if (isLatin1(c)) {
            append(static_cast<Latin1Character>(c));
            return;
        }
        spill();
        m_builder.append(c);
    }

    void append(std::span<const Latin1Character> chars)
    {
        if (m_spilled) {
            m_builder.append(chars);
            return;
        }
        if (m_inlineLength + chars.size() <= inlineCapacity) {
            if (!chars.empty())
                memcpy(m_inline + m_inlineLength, chars.data(), chars.size());
            m_inlineLength += chars.size();
            return;
        }
        spill();
        m_builder.append(chars);
    }

    void append(std::span<const char16_t> chars)
    {
        if (!m_spilled)
            spill();
        m_builder.append(chars);
    }

    ALWAYS_INLINE void append(StringView s)
    {
        if (s.is8Bit())
            append(s.span8());
        else
            append(s.span16());
    }

    ALWAYS_INLINE void append(const String& s) { append(StringView { s }); }
    ALWAYS_INLINE void append(const StringBuilder& other) { append(StringView { other }); }
    ALWAYS_INLINE void append(ASCIILiteral s) { append(s.span8()); }

    // Variadic: hex(), numbers, makeString()-style args.
    template<WTF::StringTypeAdaptable... StringTypes>
    void append(const StringTypes&... strings)
    {
        appendFromAdapters(WTF::StringTypeAdapter<StringTypes>(strings)...);
    }

    void reserveCapacity(unsigned capacity)
    {
        if (capacity <= inlineCapacity)
            return;
        if (!m_spilled)
            spill();
        m_builder.reserveCapacity(capacity);
    }

    bool hasOverflowed() const { return m_spilled && m_builder.hasOverflowed(); }
    bool isEmpty() const { return !length(); }
    unsigned length() const { return m_spilled ? m_builder.length() : m_inlineLength; }
    bool is8Bit() const { return !m_spilled || m_builder.is8Bit(); }

    JSString* toJS(VM& vm)
    {
        if (!m_spilled) {
            if (!m_inlineLength)
                return vm.smallStrings.emptyString();
            if (m_inlineLength == 1)
                return vm.smallStrings.singleCharacterString(m_inline[0]);
            if (m_inlineLength <= JSString::maxInlineLength8)
                return JSString::createInline8(vm, { m_inline, m_inlineLength });
            return JSBigInlineString::create8(vm, { m_inline, m_inlineLength });
        }
        // Spilled: still check for inline-eligible short results (e.g. a char16_t
        // forced spill). Long results reuse the builder's StringImpl via toString().
        unsigned len = m_builder.length();
        if (len <= JSString::maxBigInlineLength8 && (m_builder.is8Bit() || len <= JSString::maxBigInlineLength16))
            return jsString(vm, StringView { m_builder });
        return jsString(vm, m_builder.toString());
    }

    // Escape hatch for callers that also need the WTF::String.
    String toString()
    {
        if (!m_spilled)
            spill();
        return m_builder.toString();
    }

private:
    template<typename... Adapters>
    ALWAYS_INLINE void appendFromAdapters(const Adapters&... adapters)
    {
        if (m_spilled) {
            m_builder.appendFromAdapters(adapters...);
            return;
        }
        if constexpr (!(... || WTF::stringBuilderSlowPathRequiredForAdapter<Adapters>)) {
            if (WTF::are8Bit(adapters...)) {
                auto required = WTF::saturatingSum<uint32_t>(static_cast<uint32_t>(m_inlineLength), adapters.length()...);
                if (required <= inlineCapacity) {
                    std::span<Latin1Character> dest { m_inline + m_inlineLength, static_cast<size_t>(required - m_inlineLength) };
                    WTF::stringTypeAdapterAccumulator(dest, adapters...);
                    m_inlineLength = static_cast<uint8_t>(required);
                    return;
                }
            }
        }
        spill();
        m_builder.appendFromAdapters(adapters...);
    }

    ALWAYS_INLINE void spill()
    {
        ASSERT(!m_spilled);
        m_spilled = true;
        if (m_inlineLength)
            m_builder.append(std::span<const Latin1Character> { m_inline, m_inlineLength });
    }

    StringBuilder m_builder;
    Latin1Character m_inline[inlineCapacity];
    uint8_t m_inlineLength { 0 };
    bool m_spilled { false };
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
