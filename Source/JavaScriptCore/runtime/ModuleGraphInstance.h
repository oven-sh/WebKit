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
#include "CyclicModuleRecord.h"
#include "JSInternalFieldObjectImpl.h"
#include "JSDestructibleObject.h"
#include <wtf/HashMap.h>

namespace JSC {

class JSModuleEnvironment;
class JSModuleNamespaceObject;
class JSPromise;
class JSScope;
class ModuleGraphInstance;

// The state of one module record within one ModuleGraphInstance: its module
// environment there and the record's evaluation state for that instance (the
// fields the module evaluation algorithm keeps on a Cyclic Module Record). Also
// the context object of that instance's asynchronous evaluation steps.
// It is also the generator object of the instance's module body (internal
// fields State and Frame, as on AbstractModuleRecord) and therefore the driver
// a top-level `for await` in that body resumes.
class ModuleRecordInstance final : public JSInternalFieldObjectImpl<2> {
public:
    using Base = JSInternalFieldObjectImpl<2>;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    using Field = AbstractModuleRecord::Field;
    static_assert(numberOfInternalFields == AbstractModuleRecord::numberOfInternalFields);
    WriteBarrier<Unknown>& internalField(Field field) { return Base::internalField(static_cast<uint32_t>(field)); }
    const WriteBarrier<Unknown>& internalField(Field field) const { return Base::internalField(static_cast<uint32_t>(field)); }
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleRecordInstanceSpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static ModuleRecordInstance* create(VM&, ModuleGraphInstance*, AbstractModuleRecord*, JSModuleEnvironment*);

    AbstractModuleRecord* record() const { return m_record.get(); }
    ModuleGraphInstance* graphInstance() const { return m_graphInstance.get(); }
    JSModuleEnvironment* environment() const { return m_environment.get(); }

    CyclicModuleRecord::Status status() const { return m_status; }
    void setStatus(CyclicModuleRecord::Status status) { m_status = status; }
    JSValue evaluationError() const { return m_evaluationError.get(); }
    void setEvaluationError(VM& vm, JSValue error) { m_evaluationError.set(vm, this, error); }
    unsigned dfsAncestorIndex() const { return m_dfsAncestorIndex; }
    void setDFSAncestorIndex(unsigned index) { m_dfsAncestorIndex = index; }
    CyclicModuleRecord* cycleRoot() const { return m_cycleRoot.get(); }
    void setCycleRoot(VM& vm, CyclicModuleRecord* root) { m_cycleRoot.setMayBeNull(vm, this, root); }
    AbstractModuleRecord::AsyncEvaluationOrder asyncEvaluationOrder() const { return m_asyncEvaluationOrder; }
    void setAsyncEvaluationOrder(AbstractModuleRecord::AsyncEvaluationOrder order) { m_asyncEvaluationOrder = order; }
    std::optional<int> pendingAsyncDependencies() const { return m_pendingAsyncDependencies; }
    void setPendingAsyncDependencies(std::optional<int> value) { m_pendingAsyncDependencies = value; }
    const Vector<WriteBarrier<AbstractModuleRecord>>& asyncParentModules() const LIFETIME_BOUND { return m_asyncParentModules; }
    void appendAsyncParentModule(VM&, AbstractModuleRecord*);
    JSPromise* topLevelCapability() const { return m_topLevelCapability.get(); }
    void setTopLevelCapability(VM& vm, JSPromise* capability) { m_topLevelCapability.setMayBeNull(vm, this, capability); }
    JSPromise* asyncCapability() const { return m_asyncCapability.get(); }
    void setAsyncCapability(VM& vm, JSPromise* capability) { m_asyncCapability.setMayBeNull(vm, this, capability); }
    // Generator state of a module body with top-level await (Field::State).
    bool isExecutionFinished() const;
    JSModuleNamespaceObject* deferredNamespaceObject() const { return m_deferredNamespaceObject.get(); }
    void setDeferredNamespaceObject(VM&, JSModuleNamespaceObject*);

private:
    ModuleRecordInstance(VM&, Structure*);
    void finishCreation(VM&, ModuleGraphInstance*, AbstractModuleRecord*, JSModuleEnvironment*);

    WriteBarrier<AbstractModuleRecord> m_record;
    WriteBarrier<ModuleGraphInstance> m_graphInstance;
    WriteBarrier<JSModuleEnvironment> m_environment;
    WriteBarrier<Unknown> m_evaluationError;
    WriteBarrier<CyclicModuleRecord> m_cycleRoot;
    WriteBarrier<JSPromise> m_topLevelCapability;
    WriteBarrier<JSPromise> m_asyncCapability;
    WriteBarrier<JSModuleNamespaceObject> m_deferredNamespaceObject;
    Vector<WriteBarrier<AbstractModuleRecord>> m_asyncParentModules;
    AbstractModuleRecord::AsyncEvaluationOrder m_asyncEvaluationOrder { };
    std::optional<int> m_pendingAsyncDependencies;
    unsigned m_dfsAncestorIndex { 0 };
    CyclicModuleRecord::Status m_status { CyclicModuleRecord::Status::Linked };
};

// One instantiation of a module graph in a global object beyond the primary
// one: maps each module record instantiated for it to its ModuleRecordInstance
// (environment + evaluation state). Records not in the map are shared with the
// primary graph (their own environment and state apply).
class ModuleGraphInstance final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleGraphInstanceSpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    JS_EXPORT_PRIVATE static ModuleGraphInstance* create(VM&, JSGlobalObject*, JSScope* parentScope);

    // The scope module environments of this instance are created under: an
    // embedder-provided scope (e.g. a scope overlay) or the global object's
    // module environment parent scope.
    JSScope* parentScope() const { return m_parentScope.get(); }
    void setParentScope(VM& vm, JSScope* scope) { m_parentScope.setMayBeNull(vm, this, scope); }

    ModuleRecordInstance* recordInstance(AbstractModuleRecord*) const;
    JSModuleEnvironment* environment(AbstractModuleRecord* record) const
    {
        ModuleRecordInstance* instance = recordInstance(record);
        return instance ? instance->environment() : nullptr;
    }
    ModuleRecordInstance* add(VM&, AbstractModuleRecord*, JSModuleEnvironment*);
    bool remove(AbstractModuleRecord*);
    // Drops every record's environment and state (the embedder is done with the
    // instance; code of the instance that still runs keeps what it closes over).
    // Releases every record's state; pending top-level evaluation promises of
    // this instance are rejected and later asynchronous completions of its
    // modules are dropped.
    JS_EXPORT_PRIVATE void clear(JSGlobalObject*);
    bool isCleared() const { return m_cleared; }
    // Disposed by its owner: cleared, or to be cleared as soon as the evaluation
    // step currently running against it returns (see BusyScope).
    bool isDisposed() const { return m_cleared || m_clearPending; }

    // Brackets an evaluation step that runs against this instance (Evaluate(),
    // an asynchronous completion or resumption); a clear() requested meanwhile
    // is performed when the outermost step returns.
    class BusyScope {
    public:
        BusyScope(JSGlobalObject* globalObject, ModuleGraphInstance* instance)
            : m_globalObject(globalObject), m_instance(instance)
        {
            if (m_instance)
                ++m_instance->m_busy;
        }
        ~BusyScope()
        {
            if (m_instance && !--m_instance->m_busy && m_instance->m_clearPending)
                m_instance->clear(m_globalObject);
        }
    private:
        JSGlobalObject* m_globalObject;
        ModuleGraphInstance* m_instance;
    };
    template<typename Functor> void forEachRecord(const Functor&) const;

private:
    ModuleGraphInstance(VM&, Structure*);
    void finishCreation(VM&, JSScope* parentScope);

    WriteBarrier<JSScope> m_parentScope;
    UncheckedKeyHashMap<AbstractModuleRecord*, WriteBarrier<ModuleRecordInstance>> m_records;
    bool m_cleared { false };
    bool m_clearPending { false };
    unsigned m_busy { 0 };
};

template<typename Functor>
void ModuleGraphInstance::forEachRecord(const Functor& functor) const
{
    for (auto& [record, instance] : m_records)
        functor(*record, *instance.get());
}

} // namespace JSC
