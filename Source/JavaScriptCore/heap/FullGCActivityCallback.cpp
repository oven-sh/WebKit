/*
 * Copyright (C) 2014-2026 Apple Inc. All rights reserved.
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

#include "config.h"
#include "FullGCActivityCallback.h"
#include <wtf/MemoryPressureHandler.h>

#include "VM.h"

namespace JSC {

FullGCActivityCallback::FullGCActivityCallback(JSC::Heap& heap, Synchronousness synchronousness)
    : GCActivityCallback(heap, synchronousness)
{
}

FullGCActivityCallback::~FullGCActivityCallback() = default;

void FullGCActivityCallback::doCollection(VM& vm)
{
    // T4(c): once the server heap is shared this timer fires again (the
    // §5.4/I15 blanket dispatch disable was lifted): it runs on the main
    // client's run-loop thread with the API lock and heap access held.
    // heap.collect() below routes through collectAsync/collectSync, whose
    // ISS arms reroute to §10B.1 ticketing / the §10.2 election — no
    // fire-and-forget collection can result.
    JSC::Heap& heap = vm.heap;
    setDidGCRecently(false);

#if !PLATFORM(IOS_FAMILY) || PLATFORM(MACCATALYST)
    // T4(c) amendment (audit blocker): the isPagedOut() pressure bail must
    // NOT run mutator-side once shared. MarkedSpace::isPagedOut() carries
    // ASSERT(!isSharedServer() || worldIsStoppedForAllClients()) — written
    // under the pre-T4(c) "never fired once shared" assumption — so a debug
    // build asserts on the first pressure-gated fire. And the walk is
    // genuinely racy: BlockDirectory::updatePercentageOfPagedOutPages
    // iterates the m_blocks Vector lock-free while a sibling client's
    // addBlock (under MSPL, which this timer thread does not hold) can
    // append and reallocate the vector spine. Holding heap access only
    // excludes stop WINDOWS (block frees); it does not exclude MSPL appends
    // — the earlier comment's frees-are-world-stopped argument did not
    // cover the resize race the original T8 audit cited when disabling
    // this path. When shared, skip the bail and collect unconditionally:
    // worst case is a redundant full GC under memory pressure (and on this
    // branch's ~13.9GB-RSS profile a full GC under pressure is the desired
    // outcome anyway). Flag-off this branch is byte-identical.
    if (!heap.isSharedServer()) [[likely]] {
        MonotonicTime startTime = MonotonicTime::now();
        if (MemoryPressureHandler::singleton().isUnderMemoryPressure() && heap.isPagedOut()) {
            cancel();
            heap.increaseLastFullGCLength(MonotonicTime::now() - startTime);
            return;
        }
    }
#endif

    heap.collect(m_synchronousness, CollectionScope::Full);
}

Seconds FullGCActivityCallback::lastGCLength(JSC::Heap& heap)
{
    return heap.lastFullGCLength();
}

double FullGCActivityCallback::deathRate(JSC::Heap& heap)
{
    size_t sizeBefore = heap.sizeBeforeLastFullCollection();
    size_t sizeAfter = heap.sizeAfterLastFullCollection();
    return GCActivityCallback::deathRate(sizeBefore, sizeAfter);
}

double FullGCActivityCallback::gcTimeSlice(size_t bytes)
{
    return std::min((static_cast<double>(bytes) / MB) * Options::percentCPUPerMBForFullTimer(), Options::collectionTimerMaxPercentCPU());
}

} // namespace JSC
