/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTIR.h"
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class EffectsCache {
    WTF_MAKE_NONCOPYABLE(EffectsCache);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(EffectsCache);
public:
    EffectsCache(const EffectContext&, const Function&);

    const EffectsOnEdges& of(ValueID) LIFETIME_BOUND;
    const EffectsOnEdges& ofTerminator(BlockID) LIFETIME_BOUND;

    unsigned numberOfBlocks() const { return m_entryOfTerminator.size(); }

    void forEachCachedValue(NOESCAPE const Invocable<void(ValueID, const EffectsOnEdges&)> auto& functor) const
    {
        for (size_t index = 0; index < m_entryOfValue.size(); ++index) {
            if (EntryID entry = m_entryOfValue[index])
                functor(ValueID(index), m_entries.at(entry));
        }
    }

    void forEachCachedTerminator(NOESCAPE const Invocable<void(BlockID, const EffectsOnEdges&)> auto& functor) const
    {
        for (size_t index = 0; index < m_entryOfTerminator.size(); ++index) {
            if (EntryID entry = m_entryOfTerminator[index])
                functor(BlockID(index), m_entries.at(entry));
        }
    }

private:
    using EntryID = ID<struct EffectsCacheEntryIDTag>;

    static uint32_t hashOf(const EffectsOnEdges&);
    EntryID intern(EffectsOnEdges&&);

    const EffectContext m_context;
    const Function& m_function;
    Vector<EntryID> m_entryOfValue;
    Vector<EntryID> m_entryOfTerminator;
    InternedRecords<EffectsOnEdges, EntryID, 16> m_entries;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
