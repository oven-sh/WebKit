/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONKeyOrder.h"
#include "AOTJSONSource.h"
#include "JSCJSValue.h"
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

class CacheRegistry;
struct Instance;

class JSONRuntime {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(JSONRuntime);
    WTF_MAKE_NONCOPYABLE(JSONRuntime);
public:
    JSONRuntime(Instance&, CacheRegistry&);
    ~JSONRuntime();

    bool open(JSValue textOrBytes, void* stackPointer);
    bool open(StringView text, void* stackPointer);
    bool open(std::span<const uint8_t> utf8, void* stackPointer);
    bool close(const uint8_t* cursor);
    void abandon();
    void dropSourcesBelow(void* stackPointer);

    void clear();

    Instance& instance() const { return m_instance; }
    const StringImpl* wideText() const { return m_wideText.get(); }
    JSONKeyOrderLists& keyOrderLists() { return m_keyOrderLists; }
    size_t numberOfSuspendedSources() const { return m_suspendedSources.size(); }
    size_t sourceCapacity() const { return m_sourceCapacity; }

private:
    uint8_t* begin(void* stackPointer, size_t length);
    uint8_t* beginBesideOpenSource(void* stackPointer, size_t length);
    uint8_t* beginIn(uint8_t* start, void* stackPointer, size_t length, bool isNested);
    void end();
    void suspend();
    void resume();
    uint8_t* reserve(size_t size);
    uint8_t* replace(size_t size);
    void freeCurrent();
    void freeAll();

    bool openLatin1(std::span<const Latin1Character>, void* stackPointer);
    bool open16Bit(std::span<const char16_t>, void* stackPointer);
    bool openUTF8(std::span<const uint8_t>, void* stackPointer);

    Instance& m_instance;
    Vector<JSONSource, 2> m_suspendedSources;
    RefPtr<StringImpl> m_wideText;
    size_t m_sourceCapacity { 0 };
    JSONKeyOrderLists m_keyOrderLists;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
