/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEndOfThread.h"
#include <cstddef>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/Platform.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

enum class KindOfBlocks : uint8_t {
    HeldByKnownAnswers,
    HeldByWalkState,
};
static constexpr unsigned numberOfKindsOfBlocks = static_cast<unsigned>(KindOfBlocks::HeldByWalkState) + 1;

class BlocksOfThread {
protected:
    static constexpr unsigned mostBlocks = 64;

    struct Block {
        Block* next;
    };

    struct List {
        Block* first;
        unsigned numberOfBlocks;
    };

    struct Lists {
        List ofKinds[numberOfKindsOfBlocks];
    };

    static ALWAYS_INLINE List& listOf(KindOfBlocks kind) { return s_lists->ofKinds[static_cast<unsigned>(kind)]; }

    static ALWAYS_INLINE void push(List& list, void* freed)
    {
        Block* block = static_cast<Block*>(freed);
        block->next = list.first;
        list.first = block;
        ++list.numberOfBlocks;
    }

    static NEVER_INLINE void freeWhereListTakesNone(KindOfBlocks kind, void* freed)
    {
        if (s_lists != &s_listsOfNewThread) {
            fastFree(freed);
            return;
        }
        s_lists = new (NotNull, fastMalloc(sizeof(Lists))) Lists { };
        runAtEndOfThread(end);
        push(listOf(kind), freed);
    }

private:
    static void end()
    {
        Lists* lists = std::exchange(s_lists, &s_listsOfEndedThread);
        for (List& list : lists->ofKinds) {
            while (Block* block = list.first) {
                list.first = block->next;
                fastFree(block);
            }
        }
        fastFree(lists);
    }

    static inline constinit Lists s_listsOfNewThread { { { nullptr, mostBlocks }, { nullptr, mostBlocks } } };
    static inline constinit Lists s_listsOfEndedThread { { { nullptr, mostBlocks }, { nullptr, mostBlocks } } };

    static inline constinit thread_local Lists* s_lists = ASAN_ENABLED ? &s_listsOfEndedThread : &s_listsOfNewThread;
};

template<typename T, KindOfBlocks kind>
class ThreadBlockCache : BlocksOfThread {
public:
    static ALWAYS_INLINE void* allocate()
    {
        List& list = listOf(kind);
        Block* block = list.first;
        if (!block) [[unlikely]]
            return fastMalloc(sizeof(T));
        list.first = block->next;
        --list.numberOfBlocks;
        return block;
    }

    static ALWAYS_INLINE void free(void* freed)
    {
        List& list = listOf(kind);
        if (list.numberOfBlocks >= mostBlocks) [[unlikely]] {
            freeWhereListTakesNone(kind, freed);
            return;
        }
        push(list, freed);
    }

private:
    static_assert(sizeof(T) >= sizeof(Block) && alignof(T) <= alignof(std::max_align_t));
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
