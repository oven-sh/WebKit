/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#include <array>
#include <wtf/Atomics.h>

namespace JSC {

class JSString;
class VM;

// Under GIL-off this per-VM cache is shared by every mutator thread, so the
// slots are atomic and make() verifies and returns a single snapshot of the
// slot: a colliding key's miss-store from another thread can replace the slot
// between the hash+equal check and the return, so a re-read would hand back
// the wrong atom. The miss path publishes with a release store. The snapshot
// load is relaxed in production (the same instruction as the pre-threads plain
// load) because every consumer read is address-dependent on the snapshot
// (cell -> m_fiber -> impl -> hash and characters); TSAN cannot model
// dependency ordering, so TSAN builds use acquire instead.
class KeyAtomStringCache {
public:
    static constexpr auto maxStringLengthForCache = 64;
    static constexpr auto capacity = 512;
    using Cache = std::array<WTF::Atomic<JSString*>, capacity>;

#if TSAN_ENABLED
    static constexpr std::memory_order slotLoadOrder = std::memory_order_acquire;
#else
    static constexpr std::memory_order slotLoadOrder = std::memory_order_relaxed;
#endif

    template<typename Buffer, typename Func>
    JSString* make(VM&, Buffer&, const Func&);

    ALWAYS_INLINE void clear()
    {
        // Runs world-stopped (GC finalize); relaxed is sufficient — the STW
        // handshake orders these stores against every mutator's loads.
        for (auto& slot : m_cache)
            slot.store(nullptr, std::memory_order_relaxed);
    }

private:
    Cache m_cache { };
};

} // namespace JSC
