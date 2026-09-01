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

// UNGIL V7 (Race C, LANDED): this per-VM 512-slot open-address cache is hit
// by every lite under GIL-off (JSStringInlines.h toIdentifier /
// jsSubstringOfResolved key paths), so the slots are Atomic<JSString*> and
// make() operates on a single SNAPSHOT of the slot. The snapshot is
// load-bearing for correctness, not just for TSAN: the slot is verified by
// hash+equal and then RETURNED, and a concurrent miss-store from another
// lite between verification and return swaps in a different atom that hashes
// to the same bucket (observed in vivo: "p8"/"p17" both map to slot
// hash%512==357; returning a post-verification re-load of the slot made
// o["p"+17] resolve key "p8" — the butterfly-stress silent value corruption).
// Therefore make() MUST return the verified snapshot, never re-read the slot
// after verification. tryGetValueImpl() on the snapshot is null-checked
// (rope-bit fiber => nullptr). Publication is store-release / load-acquire so
// a consumer that wins the snapshot sees the producer's fully-initialized
// JSString and atom StringImpl. clear() uses relaxed stores: it runs only at
// GC-finalize stop-the-world, which already orders it against all mutators.
class KeyAtomStringCache {
public:
    static constexpr auto maxStringLengthForCache = 64;
    static constexpr auto capacity = 512;
    using Cache = std::array<WTF::Atomic<JSString*>, capacity>;

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
