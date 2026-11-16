/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSExportMacros.h"
#include <atomic>
#include <wtf/ScopedLambda.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class Collector;
class Heap;

// Empties sparse MarkedBlocks of a compiled program by moving their cells into the holes of other blocks.
class HeapCompaction {
public:
    struct Result {
        ASCIILiteral skipped;
        bool didCollect { false };
        size_t candidateBlocks { 0 };
        size_t pinnedBlocks { 0 };
        size_t emptiedBlocks { 0 };
        size_t movedCells { 0 };
        Seconds duration { };
    };

    using ScanSpan = ScopedLambda<void(const void* begin, const void* end)>;
    using ForEachNativeSpan = ScopedLambda<void(const ScanSpan&)>;

    // A synchronous full collection that compacts. `forEachNativeSpan` hands over the allocations outside of the heap: a cell
    // that a word there points into stays where it is.
    JS_EXPORT_PRIVATE static Result collectAndCompact(Heap&, NOESCAPE const ForEachNativeSpan&);

    // What the heap could give back if its live cells were dense, as of the last collection.
    JS_EXPORT_PRIVATE static size_t bytesInHoles(Heap&);

    static bool isActive() { return s_active.load(std::memory_order_relaxed); }
    JS_EXPORT_PRIVATE static void didAskWhetherMarked(const void* cell);
    static void willPruneDeadReferences(Collector&, Heap&, bool mutatorConducts);
    static void didPruneDeadReferences(Collector&, Heap&);

private:
    class Pass;
    JS_EXPORT_PRIVATE static std::atomic<bool> s_active;
    static thread_local Pass* t_pass;
};

} // namespace JSC
