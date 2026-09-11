/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/CyclicModuleRecord.h>
#include <JavaScriptCore/ErrorInstance.h>
#include <JavaScriptCore/ParserModes.h>
#include <JavaScriptCore/SourceCode.h>

namespace JSC {

class ModuleProgramExecutable;

// Based on the Source Text Module Record
// http://www.ecma-international.org/ecma-262/6.0/#sec-source-text-module-records
class JSModuleRecord final : public CyclicModuleRecord {
    friend class LLIntOffsetsExtractor;
public:
    using Base = CyclicModuleRecord;

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.jsModuleRecordSpace<mode>();
    }

    static size_t estimatedSize(JSCell*, VM&);

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static JSModuleRecord* create(JSGlobalObject*, VM&, Structure*, JSModuleLoader*, const Identifier&, const SourceCode&, CodeFeatures);
#if USE(BUN_JSC_ADDITIONS)
    // Module `moduleIndex` of the graph: features, TLA/TypeScript flags and requested modules come from the graph; no
    // import/export entries are added (AbstractModuleRecord::isPrelinked()). With Options::usePrelinkedModuleInfo() off
    // the result is instead an ordinary record whose entries were copied out of the graph.
    JS_EXPORT_PRIVATE static JSModuleRecord* createPrelinked(JSGlobalObject*, VM&, Structure*, JSModuleLoader*, const Identifier& moduleKey, const SourceCode&, Ref<PrelinkedModuleGraph>&&, uint32_t moduleIndex);
#endif

    JS_EXPORT_PRIVATE JSValue evaluate(JSGlobalObject*, JSValue sentValue, JSValue resumeMode);

    bool isTopLevelExecutionFinished() const;

    void execute(JSGlobalObject*, JSPromise* = nullptr);
    void executeAsync(JSGlobalObject*);

    const SourceCode& sourceCode() const LIFETIME_BOUND { return m_sourceCode; }
    CodeFeatures features() const { return m_features; }

    ModuleProgramExecutable* getOrMakeExecutable(JSGlobalObject*);

    // Import slots of this record's environment (JSModuleEnvironment::importSlot):
    // one per import entry, in entry order.
    unsigned importSlotCount() const;
    unsigned importSlotIndex(UniquedStringImpl* localName);
    // The environment's import slots get the environments this record's import
    // entries resolve to. Every module the entries resolve to has its environment.
    void fillImportSlots(JSGlobalObject*);
    // For each import slot, the ScopeOffset the entry's binding has in the
    // environment it resolves to (UINT_MAX for namespace imports and unresolved
    // entries); nullopt if some exporting environment's layout is not known yet.
    std::optional<Vector<unsigned>> importSlotLayout(JSGlobalObject*);

private:
    JSModuleRecord(VM&, Structure*, JSModuleLoader*, const Identifier&, const SourceCode&, CodeFeatures);

    void finishCreation(JSGlobalObject*, VM&);

    SourceCode m_sourceCode;
    WriteBarrier<ModuleProgramExecutable> m_moduleProgramExecutable;
    CodeFeatures m_features;
};

} // namespace JSC
