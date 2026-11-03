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

private:
    String m_string;
    const char16_t* m_units { nullptr }; // If there are no surrogate pairs.
    const Latin1Character* m_narrow { nullptr };
    Vector<char32_t> m_wide;
    size_t m_length;
    bool m_hasFailed { false };
};

// What encode() and decode() return: 0 when they have come to the end of the input, or how many characters or bytes from the
// position there is no way to convert, or one of these.
constexpr int needsMoreInput = -2;
constexpr int internalError = -3;

struct Codec {
    ASCIILiteral name;
    // They move the position past what they have converted. An encoder that is not to `flush` may stop before a character
    // whose code depends on what follows.
    int (*encode)(const Codec&, State&, const Text&, size_t& position, ByteBuilder&, bool flush);
    int (*decode)(const Codec&, State&, std::span<const uint8_t>, size_t& position, StrBuilder&);
    // Null for codecs without a state.
    void (*startEncoding)(State&);
    void (*finishEncoding)(State&, ByteBuilder&);
    void (*startDecoding)(State&);
    void (*resetDecoder)(State&);
    const void* configuration;
};

} // namespace CJK

// A MultibyteCodec object. (PyMultibyteCodec.cpp)
JSValue makeMultibyteCodec(JSGlobalObject*, const CJK::Codec&);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
