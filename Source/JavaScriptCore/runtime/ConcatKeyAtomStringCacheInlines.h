/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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

#include <wtf/ThreadSanitizerSupport.h>

#include "CodeBlock.h"
#include "ConcatKeyAtomStringCache.h"
#include "Identifier.h"
#include "SmallStrings.h"
#include "VM.h"

namespace JSC {

template<typename Func>
inline JSString* ConcatKeyAtomStringCache::getOrInsert(VM& vm, JSString* s0, JSString* s1, JSString* s2, const Func& func)
{
    // THREADS/TSAN: pairs with the TSAN_ANNOTATE_HAPPENS_BEFORE in
    // DFG::Graph::tryAddConcatKeyAtomStringCache (the pointer arrives baked in
    // JIT'd code; the real ordering is code installation). No-op outside TSAN.
    TSAN_ANNOTATE_HAPPENS_AFTER(this);
    JSString* variable = nullptr;
    switch (m_mode) {
    case Mode::Variable0: {
        variable = s0;
        break;
    }
    case Mode::Variable1: {
        variable = s1;
        break;
    }
    case Mode::Variable2: {
        variable = s2;
        break;
    }
    case Mode::Megamorphic: {
        return nullptr;
    }
    }

    auto value = variable->tryGetValue();
    SUPPRESS_UNCOUNTED_LOCAL auto* impl = value->impl();

    if (!impl)
        return nullptr;

    if (!impl->isAtom())
        return nullptr;

    SUPPRESS_UNCOUNTED_LOCAL auto& atomStringImpl = *static_cast<AtomStringImpl*>(impl);
    if (atomStringImpl.length() > maxStringLengthForCache)
        return nullptr;


    // With useJSThreads, N mutators race this cache through the shared DFG/FTL
    // CodeBlock (the cache is graph-owned, one instance baked per MakeAtomString
    // site). Today's lock-free shape has three holes under that sharing:
    //   (1) m_cache.get() races a locked add()'s rehash (torn HashMap read);
    //   (2) the size read below is unlocked, so two first-inserters can both
    //       see size==0 and interleave their m_quickCache[0] key/value stores
    //       — leaving {key:A, value:B's result}, a PERSISTENT wrong-atom pair
    //       that the inline fast path then returns forever (observed as
    //       spawned-thread-butterfly-stress "named property corrupt": single
    //       digit p's only, because their toString results are the shared
    //       immortal singleCharacterStrings, so only those keys pointer-match
    //       a foreign thread's quick-cache key);
    //   (3) key was published BEFORE value, so a pointer-matching reader could
    //       load a still-null value (observed as the empty-JSValue subscript
    //       SEGV in operationGetByValOptimize/putByValOptimize).
    // Flag-on the whole map step is serialized on m_lock (the same leaf lock
    // visitAggregate already takes), each quick slot is written EXACTLY once
    // (its index is the locked map size), and value is published before key.
    // The flag-on JIT no longer reads the quick entries (DFG/FTL defer to
    // operationMakeAtomString*WithCache, see compileMakeAtomString), so the
    // ordering is defensive. Flag-off: today's lock-free code, bit-identical.
    if (Options::useJSThreads()) [[unlikely]] {
        {
            Locker locker { m_lock };
            if (auto* result = m_cache.get(atomStringImpl))
                return result;
        }
        // func() allocates (jsAtomString) — never under m_lock (GC's
        // visitAggregate takes it; no safepoint/alloc under a leaf lock).
        auto* result = func(vm);
        if (!result) [[unlikely]]
            return nullptr;
        {
            Locker locker { m_lock };
            if (m_mode == Mode::Megamorphic)
                return result;
            size_t size = m_cache.size();
            if (size == maxCapacity) [[unlikely]] {
                m_cache.clear();
                m_mode = Mode::Megamorphic;
                return result;
            }
            auto addResult = m_cache.add(atomStringImpl, result);
            if (!addResult.isNewEntry)
                return addResult.iterator->value;
            if (size < 2) {
                auto& entry = m_quickCache[size];
                entry.m_value.set(vm, m_owner, result);
                WTF::storeStoreFence();
                entry.m_key.set(vm, m_owner, variable);
            }
        }
        vm.writeBarrier(m_owner, result);
        return result;
    }

    if (auto* result = m_cache.get(atomStringImpl))
        return result;

    if (auto* result = func(vm)) [[likely]] {
        size_t size = m_cache.size();
        if (size == maxCapacity) [[unlikely]] {
            {
                Locker locker { m_lock };
                m_cache.clear();
            }
            m_mode = Mode::Megamorphic;
        } else {
            {
                Locker locker { m_lock };
                m_cache.add(atomStringImpl, result);
            }
            vm.writeBarrier(m_owner, result);
            if (size < 2) {
                auto& entry = m_quickCache[size];
                entry.m_key.set(vm, m_owner, variable);
                entry.m_value.set(vm, m_owner, result);
            }
        }
        return result;
    }

    return nullptr;
}

} // namespace JSC
