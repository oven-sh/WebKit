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

#include "JSCJSValue.h"
#include "PageCount.h"
#include <wtf/FastMalloc.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

// The bytes of something that is being made, which come to as many as a program makes them. It can hold as many as a typed array can, which is more than a Vector can, and where a Vector brings everything down
// when there is no room for more, this remembers, as TextBuilder does, and takes no more. What makes a bytes or a bytearray of it raises MemoryError then, and anything else that is to have what is in it asks
// hasOverflowed() first.
class ByteVector final {
    WTF_MAKE_NONCOPYABLE(ByteVector);
public:
    // Explicit so that `{ }`, where bytes are wanted, is a span of none and not one of these.
    explicit ByteVector() = default;
    ByteVector(ByteVector&& other) { take(other); }
    ByteVector& operator=(ByteVector&& other)
    {
        if (this != &other) {
            release();
            take(other);
        }
        return *this;
    }
    ~ByteVector() { release(); }

    bool hasOverflowed() const { return m_hasOverflowed; }
    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }

    // Not all of what was put in it is there if there was no room, so it is not to be had.
    std::span<const uint8_t> span() const
    {
        RELEASE_ASSERT(!m_hasOverflowed);
        return { m_data, m_size };
    }
    std::span<uint8_t> mutableSpan()
    {
        RELEASE_ASSERT(!m_hasOverflowed);
        return { m_data, m_size };
    }
    uint8_t& operator[](size_t index) { return mutableSpan()[index]; }
    uint8_t operator[](size_t index) const { return span()[index]; }
    uint8_t last() const { return span().back(); }
    auto begin() const { return span().begin(); }
    auto end() const { return span().end(); }

    // False if there is no room for so many in all. It is not remembered: what asked can raise what it likes.
    bool tryReserveCapacity(size_t capacity)
    {
        if (capacity <= m_capacity)
            return true;
        if (capacity > MAX_ARRAY_BUFFER_SIZE)
            return false;
        uint8_t* data;
        if (isInline()) {
            if (!tryFastMalloc(capacity).getValue(data))
                return false;
            memcpy(data, m_inline, m_size);
        } else if (!WTF::tryFastRealloc(m_data, capacity).getValue(data))
            return false;
        m_data = data;
        m_capacity = capacity;
        return true;
    }

    void append(uint8_t byte)
    {
        if (m_size == m_capacity && !makeRoomFor(1)) [[unlikely]]
            return;
        m_data[m_size++] = byte;
    }
    void append(std::span<const uint8_t> bytes)
    {
        if (bytes.empty() || !makeRoomFor(bytes.size()))
            return;
        memcpy(m_data + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }
    void appendList(std::initializer_list<uint8_t> bytes) { append(std::span<const uint8_t> { bytes.begin(), bytes.size() }); }
    // So many of the one byte.
    void appendFill(uint8_t byte, size_t count)
    {
        if (!count || !makeRoomFor(count))
            return;
        memset(m_data + m_size, byte, count);
        m_size += count;
    }
    void removeLast()
    {
        RELEASE_ASSERT(m_size);
        --m_size;
    }
    void reverse() { std::ranges::reverse(mutableSpan()); }

private:
    bool isInline() const { return m_data == m_inline; }

    bool makeRoomFor(size_t more)
    {
        if (m_hasOverflowed)
            return false;
        if (more <= m_capacity - m_size)
            return true;
        // Twice as much, so that adding one at a time does not copy them all each time, or failing that only what is needed.
        if (more <= MAX_ARRAY_BUFFER_SIZE - m_size) {
            size_t needed = m_size + more;
            if (tryReserveCapacity(std::min<size_t>(std::max(needed, m_capacity * 2), MAX_ARRAY_BUFFER_SIZE)) || tryReserveCapacity(needed))
                return true;
        }
        m_hasOverflowed = true;
        return false;
    }

    void release()
    {
        if (!isInline())
            fastFree(m_data);
    }

    // What `other` has, leaving it empty. There is nothing here to let go of.
    void take(ByteVector& other)
    {
        m_size = other.m_size;
        m_hasOverflowed = other.m_hasOverflowed;
        if (other.isInline()) {
            memcpy(m_inline, other.m_inline, other.m_size);
            m_data = m_inline;
            m_capacity = sizeof(m_inline);
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
        }
        other.m_data = other.m_inline;
        other.m_capacity = sizeof(m_inline);
        other.m_size = 0;
        other.m_hasOverflowed = false;
    }

    uint8_t m_inline[64]; // Most of what is made is short.
    uint8_t* m_data { m_inline };
    size_t m_size { 0 };
    size_t m_capacity { sizeof(m_inline) };
    bool m_hasOverflowed { false };
};

// str.encode() and bytes.decode(). A null String for `encoding` or `errors` is the default: "utf-8", and "strict".
// Nothing, or a null String, if they raised.
std::optional<ByteVector> encodeString(JSGlobalObject*, JSValue string, const String& encoding, const String& errors);
String decodeBytes(JSGlobalObject*, JSValue object, std::span<const uint8_t>, const String& encoding, const String& errors);

} } // namespace JSC::Python
