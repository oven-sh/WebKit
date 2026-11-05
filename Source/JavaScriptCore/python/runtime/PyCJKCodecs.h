/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyBytes.h"
#include "PyStr.h"
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace CJK {

// What the codecs for Chinese, Japanese and Korean (PyCJKCodecs.cpp) are to the objects that use them (PyMultibyteCodec.cpp).

// What a codec remembers from one call to the next. getstate() shows the bytes, so what they mean is as in CPython.
struct State {
    uint8_t c[8] { };
};

// The code points of a str.
class Text {
    WTF_MAKE_NONCOPYABLE(Text);
public:
    explicit Text(const String&);

    // There was no memory for it.
    bool hasFailed() const { return m_hasFailed; }
    size_t length() const { return m_length; }
    ALWAYS_INLINE char32_t operator[](size_t index) const
    {
        ASSERT(index < m_length);
        if (m_units)
            return m_units[index];
        if (m_narrow)
            return m_narrow[index];
        return m_wide[index];
    }
    // Null: they are not all below U+0100.
    const Latin1Character* narrowCharacters() const { return m_narrow; }

private:
    String m_string;
    const char16_t* m_units { nullptr }; // If there are no surrogate pairs.
    const Latin1Character* m_narrow { nullptr };
    WideText m_wide;
    size_t m_length;
    bool m_hasFailed { false };
};

// The bytes that an encoder writes. If the memory runs out, what is written goes nowhere, and makeBytes() raises a MemoryError.
// (They are char8_t because a compiler has to assume that writing a uint8_t may change anything at all, the position in the text
// and the pointers here included, and would load all of that again for every character.)
class Output {
    WTF_MAKE_NONCOPYABLE(Output);
public:
    Output() = default;
    ~Output();

    // Room for a few bytes, for the caller to fill.
    ALWAYS_INLINE char8_t* room(size_t count)
    {
        ASSERT(count <= sizeof(m_nowhere));
        if (static_cast<size_t>(m_end - m_cursor) < count) [[unlikely]]
            grow(count);
        char8_t* result = m_cursor;
        m_cursor += count;
        return result;
    }
    void append(std::span<const uint8_t>);
    // There will be about so many more.
    void expect(size_t count);

    size_t size() const { return m_hasFailed ? 0 : m_cursor - m_start; }
    // Null: a MemoryError.
    PyBytes* makeBytes(JSGlobalObject*) const;

private:
    bool tryGrow(size_t count);
    bool grow(size_t count);

    char8_t* m_start { nullptr };
    char8_t* m_cursor { nullptr };
    char8_t* m_end { nullptr };
    bool m_hasFailed { false };
    char8_t m_nowhere[16];
};

// What encode() and decode() return: 0 when they have come to the end of the input, or how many characters or bytes from the
// position there is no way to convert, or one of these.
constexpr int needsMoreInput = -2;
constexpr int internalError = -3;

// How a codec gets from its module to _multibytecodec.__create_codec().
constexpr auto nameOfCodecCapsule = "multibytecodec.codec"_s;

struct Codec {
    ASCIILiteral name;
    // They move the position past what they have converted. An encoder that is not to `flush` may stop before a character
    // whose code depends on what follows.
    int (*encode)(const Codec&, State&, const Text&, size_t& position, Output&, bool flush);
    int (*decode)(const Codec&, State&, std::span<const uint8_t>, size_t& position, StrBuilder&);
    // Null for codecs without a state.
    void (*startEncoding)(State&);
    void (*finishEncoding)(State&, Output&);
    void (*startDecoding)(State&);
    void (*resetDecoder)(State&);
    const void* configuration;
};

} // namespace CJK

// A PyCapsule, and what is in one of that name: null if the value is anything else. (PyTypesModule.cpp)
JSValue makeCapsule(JSGlobalObject*, ASCIILiteral name, const void* pointer);
const void* pointerOfCapsule(JSGlobalObject*, JSValue, ASCIILiteral name);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
