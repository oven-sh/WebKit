/*
 * Copyright (C) 2026 Peter Steinberger. All rights reserved.
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
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSCellInlines.h>
#include <JavaScriptCore/MarkedBlockInlines.h>
#include <JavaScriptCore/VM.h>
#include <array>


namespace TestWebKitAPI {

struct FinalizationState {
    unsigned count { 0 };
    bool shuttingDown { false };
    bool inactiveMarks { false };
    bool inactiveNewlyAllocated { false };
};
static void finalize(JSObjectRef object)
{
    auto& state = *static_cast<FinalizationState*>(JSObjectGetPrivate(object));
    // The Structure may already be swept when this finalizer runs.
    auto* cell = uncheckedToJS(object);
    auto& vm = cell->vm();
    ++state.count;
    state.shuttingDown = vm.heap.isShuttingDown();
    if (cell->isPreciseAllocation()) {
        auto* allocation = JSC::PreciseAllocation::fromCell(cell);
        state.inactiveMarks = !allocation->isMarked();
        state.inactiveNewlyAllocated = !allocation->isNewlyAllocated();
        return;
    }
    auto* block = JSC::MarkedBlock::blockFor(cell);
    state.inactiveMarks = block->areMarksStale(vm.heap.objectSpace().markingVersion());
    state.inactiveNewlyAllocated = !block->hasAnyNewlyAllocated();
}
static void finalizeHeap(bool collectBeforeRelease)
{
    std::array<FinalizationState, 512> states;
    auto group = JSContextGroupCreate();
    auto context = JSGlobalContextCreateInGroup(group, nullptr);
    auto definition = kJSClassDefinitionEmpty;
    definition.attributes = kJSClassAttributeNoAutomaticPrototype;
    definition.finalize = finalize;
    auto klass = JSClassCreate(&definition);
    {
        JSC::JSLockHolder locker(::toJS(group));
        auto global = JSContextGetGlobalObject(context);
        for (unsigned i = 0; i < states.size(); ++i) {
            auto object = JSObjectMake(context, klass, &states[i]);
            JSObjectSetPropertyAtIndex(context, global, i, object, nullptr);
        }
        if (collectBeforeRelease)
            ::toJS(group)->heap.collectNow(JSC::Sync, JSC::CollectionScope::Full);
        for (const auto& state : states)
            EXPECT_EQ(state.count, 0u);
        JSGlobalContextRelease(context);
    }
    JSClassRelease(klass);
    JSContextGroupRelease(group);
    for (const auto& state : states) {
        EXPECT_EQ(state.count, 1u);
        EXPECT_TRUE(state.shuttingDown);
        EXPECT_TRUE(state.inactiveMarks);
        EXPECT_TRUE(state.inactiveNewlyAllocated);
    }
}

TEST(JavaScriptCore_HeapTeardown, FinalizesNewCellsWithoutLiveBitmaps)
{
    finalizeHeap(false);
}

TEST(JavaScriptCore_HeapTeardown, FinalizesMarkedCellsWithoutLiveBitmaps)
{
    finalizeHeap(true);
}

} // namespace TestWebKitAPI
