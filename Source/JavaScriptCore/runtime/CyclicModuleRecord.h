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

#include <JavaScriptCore/AbstractModuleRecord.h>

namespace JSC {

class ModuleGraphInstance;

class ErrorInstance;

class CyclicModuleRecord : public AbstractModuleRecord {
    friend class LLIntOffsetsExtractor;
public:
    using Base = AbstractModuleRecord;

    enum class Status : uint8_t {
        New,
        Unlinked,
        Linking,
        Linked,
        Evaluating,
        EvaluatingAsync,
        Evaluated,
    };

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    template<typename CellType, SubspaceAccess>
    static void subspaceFor(VM&)
    {
        RELEASE_ASSERT_NOT_REACHED();
    }

    void initializeEnvironment(JSGlobalObject*, RefPtr<ScriptFetcher>);
    void link(JSGlobalObject*, RefPtr<ScriptFetcher>);
    // Every evaluation entry point takes the module graph instance being
    // evaluated (null: the primary instantiation, whose state lives on the
    // record itself). See ModuleGraphInstance.
#if USE(BUN_JSC_ADDITIONS)
    JSPromise* evaluate(JSGlobalObject*, int64_t referrerAsyncOrder = -1, JSPromise* dynamicImportPromise = nullptr, ModuleGraphInstance* = nullptr);
#else
    JSPromise* evaluate(JSGlobalObject*, ModuleGraphInstance* = nullptr);
#endif
    void execute(JSGlobalObject*, JSPromise* = nullptr, ModuleGraphInstance* = nullptr);
    void executeAsync(JSGlobalObject*, ModuleGraphInstance* = nullptr);
    void asyncExecutionFulfilled(JSGlobalObject*, ModuleGraphInstance* = nullptr);
    void asyncExecutionRejected(JSGlobalObject*, JSValue, ModuleGraphInstance* = nullptr);

    Status status() const { return m_status; }
    JSValue evaluationError() const { return m_evaluationError.get(); }
    unsigned dfsAncestorIndex() const { return m_dfsAncestorIndex; }
    // Evaluation state of this record in `instance` (null, or a record the
    // instance shares with the primary graph: the record's own state).
    using AbstractModuleRecord::cycleRoot;
    using AbstractModuleRecord::asyncEvaluationOrder;
    using AbstractModuleRecord::pendingAsyncDependencies;
    using AbstractModuleRecord::topLevelCapability;
    using AbstractModuleRecord::setCycleRoot;
    using AbstractModuleRecord::setAsyncEvaluationOrder;
    using AbstractModuleRecord::setPendingAsyncDependencies;
    using AbstractModuleRecord::appendAsyncParentModule;
    using AbstractModuleRecord::setTopLevelCapability;
    Status status(ModuleGraphInstance*) const;
    JSValue evaluationError(ModuleGraphInstance*) const;
    unsigned dfsAncestorIndex(ModuleGraphInstance*) const;
    CyclicModuleRecord* cycleRoot(ModuleGraphInstance*) const;
    AsyncEvaluationOrder asyncEvaluationOrder(ModuleGraphInstance*) const;
    std::optional<int> pendingAsyncDependencies(ModuleGraphInstance*) const;
    JSPromise* topLevelCapability(ModuleGraphInstance*) const;
    void setStatus(ModuleGraphInstance*, Status);
    void setEvaluationError(VM&, ModuleGraphInstance*, JSValue);
    void setDFSAncestorIndex(ModuleGraphInstance*, unsigned);
    void setCycleRoot(VM&, ModuleGraphInstance*, CyclicModuleRecord*);
    void setAsyncEvaluationOrder(ModuleGraphInstance*, AsyncEvaluationOrder);
    void setPendingAsyncDependencies(ModuleGraphInstance*, std::optional<int>);
    void appendAsyncParentModule(VM&, ModuleGraphInstance*, AbstractModuleRecord*);
    void setTopLevelCapability(VM&, ModuleGraphInstance*, JSPromise*);
    template<typename Functor> void forEachAsyncParentModule(ModuleGraphInstance*, const Functor&) const;

    // https://tc39.es/proposal-defer-import-eval/#sec-IsModuleSCCEvaluated
    // A module in an import cycle reaches EVALUATED once its own body has run, so only its cycle
    // root reaching EVALUATED tells you the whole cycle is done.
    bool isSCCEvaluated(ModuleGraphInstance* instance = nullptr) const
    {
        // 1. If module.[[CycleRoot]] is not EMPTY, then
        //   1.a. If module.[[CycleRoot]].[[Status]] is EVALUATED, return true.
        //   1.b. Return false.
        if (CyclicModuleRecord* root = cycleRoot(instance))
            return root->status(instance) == Status::Evaluated;
        // 2. If module.[[Status]] is EVALUATED, return true.
        // 3. Return false.
        return status(instance) == Status::Evaluated;
    }

    void setStatus(Status newStatus) { m_status = newStatus; }
    void setEvaluationError(VM& vm, JSValue error) { m_evaluationError.set(vm, this, error); }
    void setDFSAncestorIndex(unsigned newIndex) { m_dfsAncestorIndex = newIndex; }

protected:
    CyclicModuleRecord(VM&, Structure*, const Identifier&, SourceProviderSourceType);
    void finishCreation(JSGlobalObject*, VM&);

    WriteBarrier<Unknown> m_evaluationError;
    unsigned m_dfsAncestorIndex { 0 };
    Status m_status { Status::New };
    bool m_initialized { false };
};

} // namespace JSC
