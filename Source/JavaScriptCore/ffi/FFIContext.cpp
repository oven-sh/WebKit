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

#if ENABLE(FFI)

#include "FFIConversions.h"
#include "JSCJSValueInlines.h"
#include "JSFFICallback.h"
#include "SlotVisitorInlines.h"
#include <algorithm>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace FFI {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FFIContext);

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadsafeInvocation);

void ThreadsafeInvocation::decodeCStrings(const Signature& signature)
{
    static_assert(Signature::maxArguments <= std::numeric_limits<decltype(m_stringSlots)>::digits);
    auto types = signature.arguments();
    for (unsigned i = 0; i < types.size(); ++i) {
        if (types[i] != Type::CString || !m_slots[i])
            continue;
        String string = stringFromCString(reinterpret_cast<const char*>(static_cast<uintptr_t>(m_slots[i])));
        if (string.isNull()) [[unlikely]]
            string = emptyString();
        m_slots[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(string.releaseImpl().leakRef()));
        m_stringSlots |= 1u << i;
    }
}

ThreadsafeInvocation::~ThreadsafeInvocation()
{
    // One that did not run.
    for (unsigned i = 0; m_stringSlots; ++i) {
        if (hasString(i))
            takeString(i);
    }
}

FFIContext::ThreadsafeDispatchFunction FFIContext::s_threadsafeDispatch { nullptr };

FFIContext::FFIContext(VM& vm, JSGlobalObject& owner)
    : m_vm(vm)
    , m_owner(owner)
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

const UTF8CString* FFIContext::cachedUTF8(StringImpl& impl)
{
    for (auto& entry : m_utf8Cache) {
        if (entry.key.get() == &impl) {
            entry.lastUse = ++m_utf8CacheClock;
            return &entry.utf8;
        }
    }
    return nullptr;
}

const UTF8CString& FFIContext::cacheUTF8(StringImpl& impl, UTF8CString&& utf8)
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

void FFIContext::didGarbageCollect(CollectionScope)
{
    m_arena.shrinkWhenIdle();
    // The callbacks can be swept before the owner, with which this goes.
    if (!m_liveCallbacks.isEmpty() && !m_vm.heap.isMarked(&m_owner))
        m_liveCallbacks.clear();
}

// A call that a threadsafe callback has accepted is made, though nothing else may refer to its global object any more.
void FFIContext::visitRoots(AbstractSlotVisitor& visitor)
{
    Locker locker { m_owner.cellLock() };
    for (auto& callback : m_liveCallbacks) {
        if (callback->hasPendingInvocations())
            visitor.append(callback);
    }
}

// Not in didGarbageCollect(): the collector's thread may not drop the last reference to a string.
void FFIContext::clearCachesInEpilogue()
{
    m_utf8Cache.removeAllMatching([&](auto& entry) {
        return entry.lastUse <= m_utf8CacheClockAtLastCollection;
    });
    m_utf8CacheClockAtLastCollection = m_utf8CacheClock;
}

void StringArena::releaseUnusedChunks()
{
    if (m_chunks.size() <= m_position.usedChunks)
        return;
    bool keepsSpareChunk = m_chunks[m_position.usedChunks].sizeInBytes() <= maximumRetainedChunkBytes;
    m_chunks.shrink(m_position.usedChunks + keepsSpareChunk);
}

void StringArena::reset()
{
    ASSERT(!m_depth);
    m_position = { };
    releaseUnusedChunks();
}

void StringArena::goBackToLastMark()
{
    m_position = m_marks.takeLast().position;
    m_depthOfLastMark = m_marks.isEmpty() ? 0 : m_marks.last().depth;
    releaseUnusedChunks();
}

std::span<char> StringArena::allocate(size_t bytes)
{
    ASSERT(m_depth);

    if (m_depth > 1 && m_depth != m_depthOfLastMark) [[unlikely]] {
        m_marks.append({ m_depth, m_position });
        m_depthOfLastMark = m_depth;
    }

    if (m_position.usedChunks) {
        auto lastUsedChunk = m_chunks[m_position.usedChunks - 1].mutableSpan();
        if (lastUsedChunk.size() - m_position.offsetInLastUsedChunk >= bytes) {
            auto result = lastUsedChunk.subspan(m_position.offsetInLastUsedChunk, bytes);
            m_position.offsetInLastUsedChunk += bytes;
            return result;
        }
    }

    if (m_chunks.size() == m_position.usedChunks || m_chunks[m_position.usedChunks].sizeInBytes() < bytes) {
        auto chunk = MallocSpan<char>::tryMalloc(std::max(defaultChunkBytes, bytes));
        if (!chunk)
            return { };
        m_chunks.shrink(m_position.usedChunks);
        m_chunks.append(WTF::move(chunk));
    }
    m_position.offsetInLastUsedChunk = bytes;
    return m_chunks[m_position.usedChunks++].mutableSpan().first(bytes);
}

} } // namespace JSC::FFI

#endif // ENABLE(FFI)
