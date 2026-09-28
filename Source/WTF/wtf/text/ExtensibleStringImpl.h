/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/text/StringImpl.h>

namespace WTF {

// A buffer of characters with room to spare, and the strings that are prefixes of it.
//
// Appending to a string and then looking at the result, over and over, copies the whole string each time if every result has a buffer of
// its own, so building a string of n characters that way takes time proportional to the square of n. Here the results share one buffer.
// Each is an ordinary immutable StringImpl that refers to the first so many characters. If a string ends where the used part of the
// buffer ends, what is appended to it can be written after it, where no existing string can see it, and the result is a longer prefix.
// If something has been written there already, because the same string was appended to twice, that cannot be done, and the caller copies.
//
// The buffer is itself a StringImpl, as the base of a substring has to be, whose length is its capacity. It is never used as a string:
// nothing but its prefixes refers to it, and what is past usedLength() is uninitialized.
class ExtensibleStringImpl final : public StringImpl {
public:
    // Null if there is no memory for it, or if a string cannot be so long.
    template<typename CharacterType>
    static RefPtr<ExtensibleStringImpl> tryCreate(unsigned capacity)
    {
        ASSERT(capacity);
        if (!isValidLength<CharacterType>(capacity))
            return nullptr;
        void* memory = StringImplMalloc::tryMalloc(sizeof(ExtensibleStringImpl) + static_cast<size_t>(capacity) * sizeof(CharacterType));
        if (!memory)
            return nullptr;
        auto* characters = std::bit_cast<CharacterType*>(static_cast<uint8_t*>(memory) + sizeof(ExtensibleStringImpl));
        return adoptRef(*new (NotNull, memory) ExtensibleStringImpl(unsafeMakeSpan<const CharacterType>(characters, capacity)));
    }

    // Of a string for which isPrefixOfExtensibleBuffer() is true.
    static ExtensibleStringImpl& bufferOf(const StringImpl& prefix)
    {
        ASSERT(prefix.isPrefixOfExtensibleBuffer());
        return static_cast<ExtensibleStringImpl&>(*prefix.substringBuffer());
    }

    unsigned capacity() const { return length(); }
    unsigned usedLength() const { return m_usedLength; }

    // Where to write what comes after the used part.
    template<typename CharacterType>
    std::span<CharacterType> unusedCharacters()
    {
        ASSERT(is8Bit() == (sizeof(CharacterType) == 1));
        return unsafeMakeSpan(const_cast<CharacterType*>(span<CharacterType>().data()), capacity()).subspan(m_usedLength);
    }

    // The string that is the first `length` characters, which have all been written. They are used from now on.
    Ref<StringImpl> createPrefix(unsigned length)
    {
        ASSERT(length && length >= m_usedLength && length <= capacity());
        m_usedLength = length;
        auto* memory = static_cast<StringImpl*>(StringImplMalloc::malloc(allocationSize<StringImpl*>(1)));
        Ref prefix = is8Bit() ? adoptRef(*new (NotNull, memory) StringImpl(span8().first(length), Ref<StringImpl> { *this })) : adoptRef(*new (NotNull, memory) StringImpl(span16().first(length), Ref<StringImpl> { *this }));
        *prefix->tailPointer<uintptr_t>() |= s_substringBufferIsExtensible;
        return prefix;
    }

private:
    template<typename CharacterType>
    explicit ExtensibleStringImpl(std::span<const CharacterType> characters)
        : StringImpl(characters, ConstructWithoutCopying)
    {
    }

    unsigned m_usedLength { 0 };
};

} // namespace WTF

using WTF::ExtensibleStringImpl;
