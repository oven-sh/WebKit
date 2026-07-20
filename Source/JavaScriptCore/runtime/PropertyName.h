/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "CacheableIdentifier.h"
#include "Identifier.h"
#include "JSGlobalObjectFunctions.h"
#include "PrivateName.h"
#include <wtf/dtoa.h>

#if USE(BUN_JSC_ADDITIONS)
#include "InlinePropertyKey.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class PropertyName {
public:
    PropertyName()
        : m_impl(nullptr)
    {
    }

    // FIXME: Make PropertyName const-correct.
    PropertyName(const UniquedStringImpl* propertyName)
        : m_impl(const_cast<UniquedStringImpl*>(propertyName))
    {
    }

    PropertyName(UniquedStringImpl* propertyName)
        : m_impl(propertyName)
    {
    }

    PropertyName(const Identifier& propertyName)
        : PropertyName(propertyName.impl())
    {
    }

    // Defined in PropertyNameInlines.h.
    PropertyName(const CacheableIdentifier&);

    PropertyName(const PrivateName& propertyName)
        : m_impl(&propertyName.uid())
    {
        ASSERT(m_impl);
        ASSERT(m_impl->isSymbol());
    }

    bool isNull() const { return !m_impl; }

    bool isSymbol() const
    {
#if USE(BUN_JSC_ADDITIONS)
        return m_impl && uidIsSymbol(m_impl);
#else
        return m_impl && m_impl->isSymbol();
#endif
    }

    bool isPrivateName() const
    {
#if USE(BUN_JSC_ADDITIONS)
        return m_impl && !isInlinePropertyKey(m_impl) && m_impl->isSymbol() && static_cast<const SymbolImpl*>(m_impl)->isPrivate();
#else
        return isSymbol() && static_cast<const SymbolImpl*>(m_impl)->isPrivate();
#endif
    }

    UniquedStringImpl* uid() const
    {
        return m_impl;
    }

    AtomStringImpl* publicName() const
    {
#if USE(BUN_JSC_ADDITIONS)
        if (!m_impl)
            return nullptr;
        if (isInlinePropertyKey(m_impl)) [[unlikely]] {
            // Callers (Bun bindings, Lookup.cpp, JSCustom*Function) assume
            // "not symbol ⇒ non-null". Materialize the atom; the leaked ref
            // just pins one interned 2..5-char string — pre-fiber behaviour.
            uintptr_t w = reinterpret_cast<uintptr_t>(m_impl);
            unsigned len = inlinePropertyKeyLength(w);
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&w);
            RefPtr<AtomStringImpl> atom = inlinePropertyKeyIs8Bit(w)
                ? AtomStringImpl::add(std::span<const Latin1Character> { bytes + 1, len })
                : AtomStringImpl::add(std::span<const char16_t> { reinterpret_cast<const char16_t*>(bytes + 2), len });
            return atom.leakRef();
        }
        return m_impl->isSymbol() ? nullptr : static_cast<AtomStringImpl*>(m_impl);
#else
        return (!m_impl || m_impl->isSymbol()) ? nullptr : static_cast<AtomStringImpl*>(m_impl);
#endif
    }

    void dump(PrintStream& out) const
    {
#if USE(BUN_JSC_ADDITIONS)
        if (m_impl) {
            if (isInlinePropertyKey(m_impl)) {
                uintptr_t word = reinterpret_cast<uintptr_t>(m_impl);
                unsigned len = inlinePropertyKeyLength(word);
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&word);
                if (inlinePropertyKeyIs8Bit(word))
                    out.print(StringView(std::span<const Latin1Character> { bytes + 1, len }));
                else
                    out.print(StringView(std::span<const char16_t> { reinterpret_cast<const char16_t*>(bytes + 2), len }));
            } else
                out.print(m_impl);
        } else
            out.print("<null property name>");
#else
        if (m_impl)
            out.print(m_impl);
        else
            out.print("<null property name>");
#endif
    }

private:
    UniquedStringImpl* m_impl;
};
static_assert(sizeof(PropertyName) == sizeof(UniquedStringImpl*), "UniquedStringImpl* and PropertyName should be compatible to invoke easily from JIT code.");

inline bool operator==(PropertyName a, const Identifier& b)
{
    return a.uid() == b.impl();
}

inline bool operator==(PropertyName a, PropertyName b)
{
    return a.uid() == b.uid();
}

inline bool operator==(PropertyName a, const char* b)
{
#if USE(BUN_JSC_ADDITIONS)
    auto* uid = a.uid();
    if (isInlinePropertyKey(uid)) {
        if (!b)
            return false;
        uintptr_t w = reinterpret_cast<uintptr_t>(uid);
        unsigned len = inlinePropertyKeyLength(w);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&w);
        if (inlinePropertyKeyIs8Bit(w)) {
            const Latin1Character* chars = bytes + 1;
            for (unsigned i = 0; i < len; ++i) {
                if (!b[i] || static_cast<Latin1Character>(b[i]) != chars[i])
                    return false;
            }
            return !b[len];
        }
        const char16_t* chars = reinterpret_cast<const char16_t*>(bytes + 2);
        for (unsigned i = 0; i < len; ++i) {
            if (!b[i] || static_cast<char16_t>(static_cast<unsigned char>(b[i])) != chars[i])
                return false;
        }
        return !b[len];
    }
    return equal(uid, b);
#else
    return equal(a.uid(), b);
#endif
}

ALWAYS_INLINE std::optional<uint32_t> parseIndex(PropertyName propertyName)
{
#if USE(BUN_JSC_ADDITIONS)
    auto uid = propertyName.uid();
    if (!uid || uidIsSymbol(uid))
        return std::nullopt;
    if (isInlinePropertyKey(uid)) {
        uintptr_t w = reinterpret_cast<uintptr_t>(uid);
        if (inlinePropertyKeyIs8Bit(w))
            return parseIndex(inlinePropertyKeySpan8(w));
        return std::nullopt;
    }
    return parseIndex(*uid);
#else
    auto uid = propertyName.uid();
    if (!uid)
        return std::nullopt;
    if (uid->isSymbol())
        return std::nullopt;
    return parseIndex(*uid);
#endif
}

template<typename CharacterType>
ALWAYS_INLINE std::optional<bool> fastIsCanonicalNumericIndexString(std::span<const CharacterType> characters)
{
    auto* rawCharacters = characters.data();
    auto length = characters.size();
    ASSERT(length >= 1);
    auto first = rawCharacters[0];
    if (length == 1)
        return isASCIIDigit(first);
    auto second = rawCharacters[1];
    if (first == '-') {
        // -Infinity case should go to the slow path. -NaN cannot exist since it becomes NaN.
        if (!isASCIIDigit(second) && (length != strlen("-Infinity") || second != 'I'))
            return false;
        if (length == 2) // Including -0, and it should be accepted.
            return true;
    } else if (!isASCIIDigit(first)) {
        // Infinity and NaN should go to the slow path.
        if (!(length == strlen("Infinity") && first == 'I') && !(length == strlen("NaN") && first == 'N'))
            return false;
    }
    return std::nullopt;
}

// https://www.ecma-international.org/ecma-262/9.0/index.html#sec-canonicalnumericindexstring
ALWAYS_INLINE bool isCanonicalNumericIndexString(UniquedStringImpl* propertyName)
{
    if (!propertyName)
        return false;
#if USE(BUN_JSC_ADDITIONS)
    if (isInlinePropertyKey(propertyName)) {
        uintptr_t w = reinterpret_cast<uintptr_t>(propertyName);
        unsigned len = inlinePropertyKeyLength(w);
        if (!len)
            return false;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&w);
        if (inlinePropertyKeyIs8Bit(w)) {
            std::span<const Latin1Character> chars { bytes + 1, len };
            auto fastResult = fastIsCanonicalNumericIndexString(chars);
            if (fastResult)
                return *fastResult;
            double index = jsToNumber(StringView(chars));
            NumberToStringBuffer buffer;
            auto out = WTF::numberToStringAndSize(index, buffer);
            return WTF::equal(chars, byteCast<Latin1Character>(out));
        }
        std::span<const char16_t> chars { reinterpret_cast<const char16_t*>(bytes + 2), len };
        auto fastResult = fastIsCanonicalNumericIndexString(chars);
        if (fastResult)
            return *fastResult;
        double index = jsToNumber(StringView(chars));
        NumberToStringBuffer buffer;
        auto out = byteCast<Latin1Character>(WTF::numberToStringAndSize(index, buffer));
        if (chars.size() != out.size())
            return false;
        for (size_t i = 0; i < out.size(); ++i) {
            if (chars[i] != static_cast<char16_t>(out[i]))
                return false;
        }
        return true;
    }
#endif
    if (propertyName->isSymbol())
        return false;
    if (!propertyName->length())
        return false;

    auto fastResult = propertyName->is8Bit()
        ? fastIsCanonicalNumericIndexString(propertyName->span8())
        : fastIsCanonicalNumericIndexString(propertyName->span16());
    if (fastResult)
        return *fastResult;

    double index = jsToNumber(propertyName);
    NumberToStringBuffer buffer;
    auto span = WTF::numberToStringAndSize(index, buffer);
    return equal(propertyName, byteCast<Latin1Character>(span));
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
