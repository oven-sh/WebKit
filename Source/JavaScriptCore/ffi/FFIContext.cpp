/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#include "config.h"
#include "FFIContext.h"

#if USE(BUN_JSC_ADDITIONS)

#include "JSCJSValueInlines.h"
#include "JSFFICallback.h"
#include "SlotVisitorInlines.h"
#include <algorithm>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace FFI {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FFIContext);

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadsafeInvocation);

FFIContext::ThreadsafeDispatchFunction FFIContext::s_threadsafeDispatch { nullptr };

FFIContext::FFIContext(VM& vm)
    : m_vm(vm)
{
    m_vm.heap.addObserver(this);
}

FFIContext::~FFIContext()
{
    m_vm.heap.removeObserver(this);
}

void FFIContext::setThreadsafeDispatch(ThreadsafeDispatchFunction fn)
{
    s_threadsafeDispatch = fn;
}

void FFIContext::addLiveCallback(VM& vm, JSGlobalObject& owner, JSFFICallback* callback)
{
    Locker locker { owner.cellLock() };
    m_liveCallbacks.append(WriteBarrier<JSFFICallback>(vm, &owner, callback));
}

void FFIContext::removeLiveCallback(JSGlobalObject& owner, JSFFICallback* callback)
{
    Locker locker { owner.cellLock() };
    m_liveCallbacks.removeFirstMatching([&](const WriteBarrier<JSFFICallback>& entry) {
        return entry.get() == callback;
    });
}

template<typename Visitor>
void FFIContext::visitLiveCallbacks(JSGlobalObject& owner, Visitor& visitor)
{
    Locker locker { owner.cellLock() };
    for (auto& callback : m_liveCallbacks)
        visitor.append(callback);
}

template void FFIContext::visitLiveCallbacks(JSGlobalObject&, AbstractSlotVisitor&);
template void FFIContext::visitLiveCallbacks(JSGlobalObject&, SlotVisitor&);

const CString* FFIContext::cachedUTF8(StringImpl& impl)
{
    for (auto& entry : m_utf8Cache) {
        if (entry.key.get() == &impl) {
            entry.lastUse = ++m_utf8CacheClock;
            return &entry.utf8;
        }
    }
    return nullptr;
}

const CString& FFIContext::cacheUTF8(StringImpl& impl, CString&& utf8)
{
    if (m_utf8Cache.size() < utf8CacheCapacity) {
        UTF8CacheEntry entry;
        entry.key = &impl;
        entry.utf8 = WTF::move(utf8);
        entry.lastUse = ++m_utf8CacheClock;
        m_utf8Cache.append(WTF::move(entry));
        return m_utf8Cache.last().utf8;
    }

    UTF8CacheEntry* victim = &m_utf8Cache[0];
    for (auto& entry : m_utf8Cache) {
        if (entry.lastUse < victim->lastUse)
            victim = &entry;
    }
    victim->key = &impl;
    victim->utf8 = WTF::move(utf8);
    victim->lastUse = ++m_utf8CacheClock;
    return victim->utf8;
}

void StringArena::enter()
{
    ++m_depth;
}

void StringArena::reset()
{
    ASSERT(!m_depth);
    m_offsetInLastChunk = 0;
    if (m_chunks.isEmpty())
        return;
    if (m_chunks[0].sizeInBytes() > maximumRetainedChunkBytes) {
        m_chunks.clear();
        return;
    }
    m_chunks.shrink(1);
}

std::span<char> StringArena::allocate(size_t bytes)
{
    ASSERT(m_depth);

    if (!m_chunks.isEmpty()) {
        auto lastChunk = m_chunks.last().mutableSpan();
        if (lastChunk.size() - m_offsetInLastChunk >= bytes) {
            auto result = lastChunk.subspan(m_offsetInLastChunk, bytes);
            m_offsetInLastChunk += bytes;
            return result;
        }
    }

    size_t capacity = std::max(defaultChunkBytes, bytes);
    auto chunk = MallocSpan<char>::tryMalloc(capacity);
    if (!chunk)
        return { };
    m_chunks.append(WTF::move(chunk));
    m_offsetInLastChunk = bytes;
    return m_chunks.last().mutableSpan().first(bytes);
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
