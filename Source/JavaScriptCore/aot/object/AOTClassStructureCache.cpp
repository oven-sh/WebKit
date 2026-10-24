/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassStructureCache.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTState.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

ClassStructureCache::ClassStructureCache(State& state)
{
    state.caches->add(CacheKind::WeakByKey, "class structures"_s, *this);
}

ClassStructures ClassStructureCache::find(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull) const
{
    return m_structures.get(keyFor(classIndex, classObjectParentOrNull, prototypeParentOrNull));
}

void ClassStructureCache::add(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull, ClassStructures structures)
{
    ASSERT(structures.classObjectStructure && structures.prototypeStructure);
    m_structures.set(keyFor(classIndex, classObjectParentOrNull, prototypeParentOrNull), structures);
    m_writes.recordAll();
}

size_t ClassStructureCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t sizeBefore = m_structures.size();
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_structures.removeIf([&](auto& entry) {
            return !vm.heap.isMarked(entry.value.classObjectStructure) || !vm.heap.isMarked(entry.value.prototypeStructure);
        });
    });
    return sizeBefore - m_structures.size();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
