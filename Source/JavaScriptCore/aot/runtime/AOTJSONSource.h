/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONWords.h"
#include "AOTThreshold.h"
#include "JSCJSValue.h"
#include <bit>
#include <span>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/StackPointer.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

struct Instance;

AOT_THRESHOLD(size_t, minJSONSourceCapacity, 1 * KB);
AOT_THRESHOLD(size_t, maxRetainedJSONSourceCapacity, 64 * KB);
inline constexpr uint8_t saturatedJSONCharacter = 0xff;

struct JSONSource {
    uint8_t* start { nullptr };
    const uint8_t* end { nullptr };
    size_t capacity { 0 };
    void* ownerStackPointer { nullptr };
    RefPtr<StringImpl> wideText;
    uint32_t numberOfKeyOrderLists { 0 };
    JSONSourceEncoding encoding { JSONSourceEncoding::Latin1 };
    bool canEndInline { false };
    bool valueParserFailed { false };
};

class JSONSourceScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(JSONSourceScope);
public:
    ALWAYS_INLINE JSONSourceScope(Instance& instance, StringView text)
        : m_instance(instance)
        , m_isOpen(open(text, currentStackPointer()))
    {
    }

    ALWAYS_INLINE JSONSourceScope(Instance& instance, std::span<const uint8_t> utf8)
        : m_instance(instance)
        , m_isOpen(open(utf8, currentStackPointer()))
    {
    }

    ~JSONSourceScope();

    explicit operator bool() const { return m_isOpen; }
    const uint8_t* start() const;

private:
    bool open(StringView, void* stackPointer);
    bool open(std::span<const uint8_t> utf8, void* stackPointer);

    Instance& m_instance;
    bool m_isOpen;
};

ALWAYS_INLINE bool mayHaveOpenSource(const JSONWords& words, void* stackPointer)
{
    return std::bit_cast<uintptr_t>(words.sourceOwnerStackPointer) > std::bit_cast<uintptr_t>(stackPointer);
}

inline std::span<const uint8_t> withoutByteOrderMark(std::span<const uint8_t> bytes)
{
    bool hasMark = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
    return bytes.subspan(hasMark ? 3 : 0);
}

// The json() of a body looks for a mark in three bytes or more.
inline bool hasUTF16LittleEndianMark(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xfe;
}

// The text JSON.parse gets in place of the bytes of a body. A body without bytes throws what json() rejects with. A value that is no Uint8Array is returned as it is.
JSValue decodeJSONBody(JSGlobalObject*, JSValue bytes);

bool isLatin1AfterDecoding(const JSONWords&, std::span<const uint8_t> characters);

ALWAYS_INLINE bool isLatin1(const JSONWords& words, std::span<const uint8_t> characters)
{
    return words.sourceEncoding == JSONSourceEncoding::Latin1 || isLatin1AfterDecoding(words, characters);
}

std::span<const char16_t> original16BitCharacters(const JSONWords&, std::span<const uint8_t> characters);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
