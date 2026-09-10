/*
 * Copyright (C) 2012, 2013 Apple Inc. All rights reserved.
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
#include "ArrayAllocationProfile.h"

#include "ConcurrentButterfly.h"
#include "JSCConfig.h"
#include "JSThreadsCounters.h"

#include "JSCellInlines.h"
#include <algorithm>

namespace JSC {

// GIL off (SPEC-objectmodel history §28): an Int32 array that meets a double
// is relabelled Contiguous, not Double (T4-O), so the profile of its
// allocation site would learn Contiguous and every later array from the site
// would keep its doubles boxed. If the array that demoted the recommendation
// holds nothing but numbers, and at least one that is not an int32, the site
// is a numeric one and its arrays should be born Double instead: no
// conversion then ever happens to them.
static bool lastArrayLooksLikeDoubles(JSArray* array)
{
    // Sample only an array this thread owns (its butterfly carries this
    // thread's tag): a shared profile's last array can belong to a thread that
    // is writing it right now, and the decision can wait for that thread's own
    // profile update instead of reading its lanes from here.
    if (!butterflyWordOwnedByCurrentThread(array->taggedButterflyWord()))
        return false;
    unsigned length = array->length();
    if (!length)
        return false;
    bool sawNonInt32 = false;
    auto numeric = [&](unsigned i) {
        JSValue v = array->tryGetIndexQuickly(i);
        if (!v)
            return true; // hole
        if (!v.isNumber())
            return false;
        sawNonInt32 |= !v.isInt32();
        return true;
    };
    unsigned prefix = std::min(length, 24u);
    for (unsigned i = 0; i < prefix; ++i) {
        if (!numeric(i))
            return false;
    }
    for (unsigned k = 1; k <= 8 && prefix < length; ++k) {
        if (!numeric(prefix + (length - prefix) * k / 9))
            return false;
    }
    return sawNonInt32;
}

void ArrayAllocationProfile::updateProfile()
{
    // This is awkwardly racy but totally sound even when executed concurrently. The
    // worst cases go something like this:
    //
    // - Two threads race to execute this code; one of them succeeds in updating the
    //   m_currentIndexingType and the other either updates it again, or sees a null
    //   m_lastArray; if it updates it again then at worst it will cause the profile
    //   to "forget" some array. That's still sound, since we don't promise that
    //   this profile is a reflection of any kind of truth.
    //
    // - A concurrent thread reads m_lastArray, but that array is now dead. While
    //   it's possible for that array to no longer be reachable, it cannot actually
    //   be freed, since we require the GC to wait until all concurrent JITing
    //   finishes.
    //
    // But one exception is vector length. We access vector length to get the vector
    // length hint. However vector length can be accessible only from the main
    // thread because large butterfly can be realloced in the main thread.
    // So for now, we update the allocation profile only from the main thread.
    
    ASSERT(!isCompilationThread());
    // Under useJSThreads multiple mutators can race here on a shared
    // CodeBlock. The whole profile word is accessed via relaxed atomics (see
    // the class comment in ArrayAllocationProfile.h): the exchange below
    // atomically claims lastArray (so at most one racing thread sees a given
    // array), and the trailing setTypeRelaxed can lose a racing update, which
    // at worst "forgets" a profile observation — sound, per the comment above
    // and SPEC-ungil §5.7.
    Storage storage = m_storage.exchangeTupleRelaxed(Storage(nullptr, m_storage.typeRelaxed()));
    JSArray* lastArray = storage.pointer();
    IndexingTypeAndVectorLength current = storage.type();
    if (!lastArray)
        return;
    if (Options::useArrayAllocationProfiling()) [[likely]] {
        // The basic model here is that we will upgrade ourselves to whatever the CoW version of lastArray is except ArrayStorage since we don't have CoW ArrayStorage.
        IndexingType indexingType = leastUpperBoundOfIndexingTypes(current.indexingType() & IndexingTypeMask, lastArray->indexingType());
        if (g_jscConfig.gilOffProcess && hasContiguous(indexingType) && !hasContiguous(current.indexingType()) && !hasDouble(current.indexingType())
            && m_gilOffDoubleDemotionSet.isStillValid() && lastArrayLooksLikeDoubles(lastArray)) [[unlikely]] {
            // Once only per site: if Double turns out wrong, the demotion below
            // fires the set and this branch is never taken again.
            JSTHREADS_COUNT(arrayAllocationProfilePromotedToDoubleGILOff);
            indexingType = (indexingType & ~IndexingShapeMask) | DoubleShape;
        }
        if (isCopyOnWrite(current.indexingType())) {
            if (indexingType > ArrayWithContiguous)
                indexingType = ArrayWithContiguous;
            indexingType |= CopyOnWrite;
        }
        unsigned largestSeenVectorLength = std::min(std::max(current.vectorLength(), lastArray->getVectorLength()), BASE_CONTIGUOUS_VECTOR_LEN_MAX);
        m_storage.setTypeRelaxed(IndexingTypeAndVectorLength(indexingType, largestSeenVectorLength));
        // T4-P: the recommendation left Double (this site's arrays get
        // converted after allocation). GIL off that conversion is a stop per
        // array unless the optimized allocation follows; tell the code that
        // baked Double. Published after the type store so the recompile reads
        // the new recommendation.
        if (g_jscConfig.gilOffProcess && hasDouble(current.indexingType()) && !hasDouble(indexingType) && !hasUndecided(indexingType) && m_gilOffDoubleDemotionSet.isStillValid()) [[unlikely]] {
            JSTHREADS_COUNT(arrayAllocationProfileLeftDoubleGILOff);
            m_gilOffDoubleDemotionSet.fireAll(lastArray->vm(), "GIL off: array allocation profile left Double");
        }
    }
}

} // namespace JSC

