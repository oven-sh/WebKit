/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#pragma once

#include "CyclicModuleRecord.h"
#include "JSCell.h"
#include "JSPromise.h"
#include <wtf/RefPtr.h>

namespace JSC {

class ScriptFetcher;

// Table 45
// https://tc39.es/ecma262/#graphloadingstate-record
class ModuleGraphLoadingState final : public JSCell {
    friend class LLIntOffsetsExtractor;
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleGraphLoadingStateSpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static ModuleGraphLoadingState* create(VM&, JSPromise*, RefPtr<ScriptFetcher>, JSValue asyncContext = { });

    JSPromise* promise() const { return m_promise.get(); }
    unsigned pendingModulesCount() const { return m_pendingModulesCount; }
    bool isLoading() const { return m_isLoading; }
    ScriptFetcher* scriptFetcher() const { return m_scriptFetcher.get(); }
#if USE(BUN_JSC_ADDITIONS)
    // The async context of the load that this state is for. The requests that the load makes for its modules inherit it:
    // JSModuleLoader::asyncContextOfLoad().
    JSValue asyncContext() const { return m_asyncContext.get(); }
#endif

    void setPendingModulesCount(unsigned count) { m_pendingModulesCount = count; }
    void setIsLoading(bool loading) { m_isLoading = loading; }

    void appendVisited(VM&, CyclicModuleRecord*);
#if ENABLE(AOT)
    // Of the module that was appended last.
    void forgetVisited(CyclicModuleRecord* cyclic)
    {
        if (m_wholeLoad)
            m_wholeLoad->forgetVisited(cyclic);
        Locker locker { cellLock() };
        bool wasVisited = m_visited.removeLastMatching([&](const auto& visited) { return visited.get() == cyclic; });
        ASSERT_UNUSED(wasVisited, wasVisited);
        m_visitedSet.remove(cyclic);
    }

    // Each module gets a state of its own (moduleLoadStep()), and each state walks through every record that is still New: on a cycle, through all of it.
    // The state of a compiled record that was asked for by a graph load knows that load: a compiled record is walked once a load, by the first state that comes to it.
    // Whatever such a state walks, compiled or not, lies in the [[Visited]] of the load's first state too.
    void setWholeLoad(VM& vm, ModuleGraphLoadingState* askedBy) { m_wholeLoad.set(vm, this, askedBy->m_wholeLoad ? askedBy->m_wholeLoad.get() : askedBy); }
    bool isPartOfWholeLoad() const { return !!m_wholeLoad; }
    // Whether this state is the first of its load that comes to the record. If so the record counts as walked from here on.
    bool beginWalkForWholeLoad(VM& vm, CyclicModuleRecord* cyclic)
    {
        if (m_wholeLoad->containsVisited(cyclic))
            return false;
        m_wholeLoad->appendVisited(vm, cyclic);
        return true;
    }
#endif
    bool containsVisited(const AbstractModuleRecord* record) const { return m_visitedSet.contains(record); }
    void iterateVisited(NOESCAPE const auto& function) const
    {
        for (const auto& barrier : m_visited)
            function(barrier.get());
    }

    // innerModuleLoading() drains this queue iteratively instead of recursing
    // through hostLoadImportedModule -> continueModuleLoading. Entries are only
    // present while a drain is on the stack and are otherwise reachable via the
    // module registry / referrer.loadedModules, so raw pointers need no GC visit.
    void enqueueInnerLoad(AbstractModuleRecord* record) { m_innerLoadQueue.append(record); }
    AbstractModuleRecord* takeInnerLoad() { return m_innerLoadQueue.isEmpty() ? nullptr : m_innerLoadQueue.takeLast(); }
    bool drainingInnerLoad() const { return m_drainingInnerLoad; }
    void setDrainingInnerLoad(bool draining) { m_drainingInnerLoad = draining; }

    // Combined-promise AND-join state used by top-level loadModule (mirrors ModuleLoaderPayload's same-named fields).
    bool decrementRemaining()
    {
        ASSERT(m_remainingFulfillments > 0);
        return !--m_remainingFulfillments;
    }
    JSValue fulfillment() const { return m_fulfillment.get(); }
    void setFulfillment(VM& vm, JSValue value) { m_fulfillment.set(vm, this, value); }


private:
    ModuleGraphLoadingState(VM&, Structure*, JSPromise*, RefPtr<ScriptFetcher>, JSValue asyncContext);

    void finishCreation(VM&);

    // [[PromiseCapability]]
    WriteBarrier<JSPromise> m_promise;
    // [[Visited]]
    Vector<WriteBarrier<CyclicModuleRecord>, 8> m_visited;
    // Contains the same contents as m_visited, so no write barriers needed. Typed
    // as AbstractModuleRecord* so the hot containsVisited() check can run before
    // dynamicDowncast on the per-edge fast path.
    UncheckedKeyHashSet<const AbstractModuleRecord*> m_visitedSet;
    Vector<AbstractModuleRecord*, 8> m_innerLoadQueue;
#if ENABLE(AOT)
    WriteBarrier<ModuleGraphLoadingState> m_wholeLoad;
#endif
    // Slot used only at top-level loadModule to AND-join loadPromise and statePromise into combinedPromise.
    WriteBarrier<Unknown> m_fulfillment;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<Unknown> m_asyncContext;
#endif
    // [[PendingModulesCount]]
    unsigned m_pendingModulesCount { 1 };
    uint8_t m_remainingFulfillments { 2 };
    // [[IsLoading]]
    bool m_isLoading { true };
    bool m_drainingInnerLoad { false };
    // [[HostDefined]]
    const RefPtr<ScriptFetcher> m_scriptFetcher;
};

} // namespace JSC
