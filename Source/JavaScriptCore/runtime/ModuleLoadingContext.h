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

#include "AbstractModuleRecord.h"
#include "JSCell.h"
#include "JSModuleLoader.h"
#include <wtf/RefPtr.h>

namespace JSC {

class ModuleRegistryEntry;
class ScriptFetcher;

class ModuleLoadingContext final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleLoadingContextSpace<mode>();
    }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    enum class Step : uint8_t {
        Main, // modulePromise settled -> loadRequestedModules
        Requested, // loadRequestedModules settled -> finishLoading + setEntryRecord
        Cached, // cached loadPromise settled -> finishLoading or evaluationError
    };

    // For a request that a load makes for one of its modules. It takes the async context of that load, from the payload.
    static ModuleLoadingContext* create(VM&, JSModuleLoader*, Step, const JSModuleLoader::ModuleReferrer&, const AbstractModuleRecord::ModuleRequest&, JSCell* payload, ModuleRegistryEntry*, RefPtr<ScriptFetcher>);
    // For a load. asyncContext: of whoever starts the load, or of the load that this one is a step of.
    static ModuleLoadingContext* create(VM&, JSModuleLoader*, const AbstractModuleRecord::ModuleRequest&, RefPtr<ScriptFetcher>, OptionSet<ModuleLoadFlag>, int64_t referrerAsyncOrder = -1, JSValue asyncContext = { });

    Step step() const { return m_step; }
    void setStep(Step s) { m_step = s; }

    JSModuleLoader::ModuleReferrer referrer() const;
    const AbstractModuleRecord::ModuleRequest& moduleRequest() const { return m_moduleRequest; }
    JSCell* payload() const { return m_payload.get(); }
    ModuleRegistryEntry* entry() const { return m_entry.get(); }
    ScriptFetcher* scriptFetcher() const { return m_scriptFetcher.get(); }
    AbstractModuleRecord* module() const { return m_module.get(); }
    void module(VM& vm, AbstractModuleRecord* mod) { m_module.set(vm, this, mod); }
    JSModuleLoader* loader() const { return m_loader.get(); }
#if USE(BUN_JSC_ADDITIONS)
    // The async context of the load that this request belongs to: what was current where import() was called. The jobs
    // that this context goes to (ModuleLoadStep, ModuleLoadTopSettled, ModuleLoadLinkEvaluateSettled) run under it where
    // it is a store. Empty for a synchronous load, whose jobs all run inside the call of require().
    JSValue asyncContext() const { return m_asyncContext.get(); }
#endif

    bool evaluate() const { return m_flags.contains(ModuleLoadFlag::Evaluate); }
    bool dynamic() const { return m_flags.contains(ModuleLoadFlag::Dynamic); }
    bool useImportMap() const { return m_flags.contains(ModuleLoadFlag::UseImportMap); }
    bool deferred() const { return m_flags.contains(ModuleLoadFlag::Deferred); }
#if USE(BUN_JSC_ADDITIONS)
    int64_t referrerAsyncOrder() const { return m_referrerAsyncOrder; }
#endif

private:
    ModuleLoadingContext(VM&, Structure*, JSModuleLoader*, Step, const JSModuleLoader::ModuleReferrer&, AbstractModuleRecord::ModuleRequest&&, JSCell* payload, ModuleRegistryEntry*, RefPtr<ScriptFetcher>, JSValue asyncContext);
    ModuleLoadingContext(VM&, Structure*, JSModuleLoader*, AbstractModuleRecord::ModuleRequest&&, RefPtr<ScriptFetcher>, OptionSet<ModuleLoadFlag>, int64_t referrerAsyncOrder, JSValue asyncContext);

    Step m_step { Step::Main };
    AbstractModuleRecord::ModuleRequest m_moduleRequest;
    const RefPtr<ScriptFetcher> m_scriptFetcher;
    WriteBarrier<JSCell> m_payload;
    WriteBarrier<ModuleRegistryEntry> m_entry;
    WriteBarrier<Unknown> m_referrer;
    WriteBarrier<AbstractModuleRecord> m_module;
    WriteBarrier<JSModuleLoader> m_loader;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<Unknown> m_asyncContext;
    int64_t m_referrerAsyncOrder { -1 };
#endif
    OptionSet<ModuleLoadFlag> m_flags;
};

} // namespace JSC
