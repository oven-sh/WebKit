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
#include <wtf/TZoneMalloc.h>

namespace JSC {

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
#if USE(BUN_JSC_ADDITIONS)
    JSPromise* evaluate(JSGlobalObject*, int64_t referrerAsyncOrder = -1, JSPromise* dynamicImportPromise = nullptr);
#else
    JSPromise* evaluate(JSGlobalObject*);
#endif
    void execute(JSGlobalObject*, JSPromise* = nullptr);
    void executeAsync(JSGlobalObject*);
    void asyncExecutionFulfilled(JSGlobalObject*);
    void asyncExecutionRejected(JSGlobalObject*, JSValue);

    Status status() const { return m_status; }
    JSValue evaluationError() const { return m_evaluationError.get(); }
    unsigned dfsAncestorIndex() const { return m_dfsAncestorIndex; }

    // https://tc39.es/proposal-defer-import-eval/#sec-IsModuleSCCEvaluated
    // A module in an import cycle reaches EVALUATED once its own body has run, so only its cycle
    // root reaching EVALUATED tells you the whole cycle is done.
    bool isSCCEvaluated() const
    {
        // 1. If module.[[CycleRoot]] is not EMPTY, then
        //   1.a. If module.[[CycleRoot]].[[Status]] is EVALUATED, return true.
        //   1.b. Return false.
        if (CyclicModuleRecord* root = cycleRoot())
            return root->status() == Status::Evaluated;
        // 2. If module.[[Status]] is EVALUATED, return true.
        // 3. Return false.
        return status() == Status::Evaluated;
    }

    void setStatus(Status newStatus) { m_status = newStatus; }
    void setEvaluationError(VM&, JSValue);
    void setDFSAncestorIndex(unsigned newIndex) { m_dfsAncestorIndex = newIndex; }

#if USE(BUN_JSC_ADDITIONS)
    // Link leaves an import entry or an indirect export whose resolution is Resolution::Type::Pending for later. What it
    // waits for is known once the record that hasPendingExports() has evaluated. That is before this module's body
    // runs, unless the two are in an import cycle: JSModuleRecord::execute calls this right before the body, and an
    // entry that still waits then reads as undefined until it can be bound (JSModuleRecord::fillImportSlot).
    void linkPendingImports(JSGlobalObject*);

    // What code of this module that was linked while an import still waited embeds for that import: the scope offset in
    // the environment that the import slot will hold. Every later link of the import uses the same offset.
    struct EarlyImportLink {
        AbstractModuleRecord* exporter { nullptr }; // The one record that could export the name, or null.
        unsigned offset { 0 };
    };
#endif

protected:
    CyclicModuleRecord(VM&, Structure*, JSModuleLoader*, const Identifier&, SourceProviderSourceType);
    void finishCreation(JSGlobalObject*, VM&);

    bool resolveIndirectExports(JSGlobalObject*);
    bool linkImportEntry(JSGlobalObject*, JSModuleEnvironment*, const ImportEntry&, AbstractModuleRecord* importedModule);

#if USE(BUN_JSC_ADDITIONS)
    struct PendingImports {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(PendingImports);
        Vector<ImportEntry> entries;
        bool hasIndirectExports { false };
        UncheckedKeyHashMap<unsigned, EarlyImportLink, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> earlyLinks; // by import slot
    };
    PendingImports& ensurePendingImports()
    {
        if (!m_pendingImports)
            m_pendingImports = makeUnique<PendingImports>();
        return *m_pendingImports;
    }
    std::unique_ptr<PendingImports> m_pendingImports;
#endif

    WriteBarrier<Unknown> m_evaluationError;
    unsigned m_dfsAncestorIndex { 0 };
    Status m_status { Status::New };
    bool m_initialized { false };
};

} // namespace JSC
