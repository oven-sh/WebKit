/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsCache.h"

#if ENABLE(AOT)

#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

EffectsCache::EffectsCache(const EffectContext& context, const Function& function)
    : m_context(context)
    , m_function(function)
    , m_entryOfValue(FillWith { }, function.values.size(), EntryID())
    , m_entryOfTerminator(FillWith { }, function.blocks.size(), EntryID())
{
}

const EffectsOnEdges& EffectsCache::of(ValueID id)
{
    RELEASE_ASSERT(id.index() < m_entryOfValue.size());
    EntryID& entry = m_entryOfValue[id.index()];
    if (!entry)
        entry = intern(Typed::effectsOf(m_context, m_function, m_function.at(id)));
    return m_entries.at(entry);
}

const EffectsOnEdges& EffectsCache::ofTerminator(BlockID block)
{
    RELEASE_ASSERT(block.index() < m_entryOfTerminator.size());
    EntryID& entry = m_entryOfTerminator[block.index()];
    if (!entry)
        entry = intern(Typed::effectsOf(m_context, m_function, m_function.at(block).terminator));
    return m_entries.at(entry);
}

uint32_t EffectsCache::hashOf(const EffectsOnEdges& effectsOnEdges)
{
    Hasher hasher;
    for (const Effects* effects : { &effectsOnEdges.normal, &effectsOnEdges.handler }) {
        for (const LocationMasks* masks : { &effects->reads, &effects->writes }) {
            add(hasher, static_cast<uint64_t>(masks->heaps().toRaw()));
            masks->forEach([&](AbstractHeap, uint64_t mask) {
                add(hasher, mask);
            });
        }
        add(hasher, WTF::packBools(effects->mayThrow, effects->mayCollect, effects->mayCallUnknownCode, effects->maySuspend, effects->mayBeTerminated));
    }
    return hasher.hash();
}

EffectsCache::EntryID EffectsCache::intern(EffectsOnEdges&& effects)
{
    uint32_t hash = hashOf(effects);
    return m_entries.findOrAdd(WTF::move(effects), hash);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
