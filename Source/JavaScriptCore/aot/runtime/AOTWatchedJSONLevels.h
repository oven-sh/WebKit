/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <memory>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class WatchedJSONLevels {
public:
    bool isWatching() const { return m_numberOfLevelsLeft; }
    bool isWatchingAfterGrowthTo(size_t numberOfCharacters)
    {
        uint32_t numberOfLevels = static_cast<uint32_t>(numberOfCharacters / charactersPerWatchedLevel);
        if (numberOfLevels > m_numberOfLevelsLeft) {
            if (!m_numberOfLevelsLeft)
                forgetLevels();
            m_numberOfLevelsLeft = numberOfLevels;
        }
        return m_numberOfLevelsLeft;
    }
    void stop() { m_numberOfLevelsLeft = 0; }
    void forgetLevels()
    {
        if (!m_levels)
            return;
        m_levels->path.shrink(0);
        m_levels->holders.clear();
    }
    bool enter(EncodedJSValue holder, unsigned depth)
    {
        ASSERT(isWatching());
        if (!m_levels)
            m_levels = makeUnique<Levels>();
        while (!m_levels->path.isEmpty() && m_levels->path.last().depth >= depth)
            m_levels->holders.remove(m_levels->path.takeLast().holder);
        if (!m_levels->holders.add(holder).isNewEntry) {
            stop();
            return false;
        }
        m_levels->path.append(Level { depth, holder });
        --m_numberOfLevelsLeft;
        return true;
    }

private:
    static constexpr size_t charactersPerWatchedLevel = 64 * KB;

    struct Level {
        unsigned depth;
        EncodedJSValue holder;
    };

    struct Levels {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Levels);
        Vector<Level, 16> path;
        UncheckedKeyHashSet<EncodedJSValue> holders;
    };

    uint32_t m_numberOfLevelsLeft { 0 };
    std::unique_ptr<Levels> m_levels;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
