/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <bit>
#include <memory>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

// Detects a cycle for a writer of JSON that keeps no list of the objects it is currently inside.
// The text of a cycle never ends, so the writer's buffer keeps growing. After each growth the writer reports the next few objects it visits: few enough that checking them costs little next to copying the buffer.
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
    // Returns false if the holder is already being written. Nothing reports that a holder is finished: a later call from a frame at the same address or above it means that it is.
    bool enter(EncodedJSValue holder, void* frame)
    {
        ASSERT(isWatching());
        if (!m_levels)
            m_levels = makeUnique<Levels>();
        while (!m_levels->path.isEmpty() && std::bit_cast<uintptr_t>(m_levels->path.last().frame) <= std::bit_cast<uintptr_t>(frame))
            m_levels->holders.remove(m_levels->path.takeLast().holder);
        if (!m_levels->holders.add(holder).isNewEntry) {
            stop();
            return false;
        }
        m_levels->path.append(Level { frame, holder });
        --m_numberOfLevelsLeft;
        return true;
    }

private:
    static constexpr size_t charactersPerWatchedLevel = 64 * KB;

    struct Level {
        void* frame;
        EncodedJSValue holder;
    };

    // The holders reported since the watch began that are still being written, outermost first.
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
