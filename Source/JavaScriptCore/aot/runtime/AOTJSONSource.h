/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONHooks.h"
#include "AOTJSONWords.h"
#include <span>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/StackPointer.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

struct Instance;

static constexpr size_t minJSONSourceCapacity = 1 * KB;
static constexpr size_t maxRetainedJSONSourceCapacity = 64 * KB;
inline constexpr uint8_t saturatedJSONCharacter = 0xff;

class JSONSourceScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(JSONSourceScope);
public:
    ALWAYS_INLINE JSONSourceScope(Instance& instance, StringView text, JSONTextMemory memory)
        : m_instance(instance)
        , m_isOpen(open(text, memory, currentStackPointer()))
    {
    }

    ~JSONSourceScope();

    explicit operator bool() const { return m_isOpen; }
    const uint8_t* start() const;

private:
    bool open(StringView, JSONTextMemory, void* stackPointer);

    Instance& m_instance;
    bool m_isOpen;
};

bool isLatin1AfterDecoding(const JSONWords&, std::span<const uint8_t> characters);

ALWAYS_INLINE bool isLatin1(const JSONWords& words, std::span<const uint8_t> characters)
{
    return words.sourceEncoding == JSONSourceEncoding::Latin1 || isLatin1AfterDecoding(words, characters);
}

std::span<const char16_t> original16BitCharacters(const JSONWords&, std::span<const uint8_t> characters);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
