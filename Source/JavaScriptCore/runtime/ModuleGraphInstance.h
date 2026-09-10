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

#include "JSCell.h"
#include "WriteBarrier.h"
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC {

class AbstractModuleRecord;
class JSModuleRecord;
class JSPromise;
class JSScope;

// A further instantiation of already-linked modules in the same global object
// (Options::useModuleGraphInstances()).
//
// An instance maps a *template* record (the one the module loader registry
// holds) to the record that stands for that module inside this instance:
//   - a Source Text Module Record gets a fresh JSModuleRecord (ModuleGraphInstance::instantiate)
//     with its own executable, environment, namespace and evaluation state (the
//     unlinked code comes from the code cache, so nothing is parsed or generated again);
//   - any other record (synthetic / host modules) is shared with the template
//     graph unless the embedder registers a replacement first (add()).
// Everything else -- Link(), Evaluate(), namespaces, top-level await -- is the
// ordinary machinery running on the instance's records.
class ModuleGraphInstance final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleGraphInstanceSpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    JS_EXPORT_PRIVATE static ModuleGraphInstance* create(VM&, JSGlobalObject*, JSScope* parentScope = nullptr);

    JSGlobalObject* globalObject() const { return m_globalObject.get(); }
    // Scope the instance's module environments are created under (default: the
    // one the global object's own module environments use). Shared CodeBlocks
    // were linked against one scope chain shape, so this must mirror it.
    JSScope* parentScope() const { return m_parentScope.get(); }
    void setParentScope(VM& vm, JSScope* scope) { m_parentScope.setMayBeNull(vm, this, scope); }
    // For the embedder: whatever object represents this instance to it (kept alive by the instance).
    JSValue embedderData() const { return m_embedderData.get(); }
    void setEmbedderData(VM& vm, JSValue value) { m_embedderData.set(vm, this, value); }

    // The record standing for templateRecord in this instance, or null.
    JS_EXPORT_PRIVATE AbstractModuleRecord* recordFor(AbstractModuleRecord* templateRecord) const;
    JSModuleRecord* sourceTextRecordFor(AbstractModuleRecord* templateRecord) const;
    // Register the record that stands for templateRecord (an embedder may
    // pre-register a fresh synthetic record, or the template itself to force sharing).
    JS_EXPORT_PRIVATE void add(VM&, AbstractModuleRecord* templateRecord, AbstractModuleRecord* instanceRecord);
    JS_EXPORT_PRIVATE bool remove(AbstractModuleRecord* templateRecord);

    // Create (or find) this instance's records for templateRecord and every
    // Source Text Module Record it depends on, and Link() them. templateRecord
    // must itself be linked. Returns this instance's record for it.
    JS_EXPORT_PRIVATE JSModuleRecord* instantiate(JSGlobalObject*, JSModuleRecord* templateRecord);
    // instantiate() + Evaluate(); the promise is the top-level capability.
    JS_EXPORT_PRIVATE JSPromise* evaluate(JSGlobalObject*, JSModuleRecord* templateRecord);
    // Same, for callers that cannot wait: throws if evaluation does not settle synchronously.
    JS_EXPORT_PRIVATE JSModuleRecord* evaluateSync(JSGlobalObject*, JSModuleRecord* templateRecord);

    // The embedder is done with the instance: drop every record (code of the
    // instance that still runs keeps what it closes over). Later instantiate()
    // calls throw.
    JS_EXPORT_PRIVATE void clear();
    bool isCleared() const { return m_cleared; }
    bool isDisposed() const { return m_cleared; }

    template<typename Functor> void forEachRecord(const Functor&) const;
    unsigned size() const { return m_records.size(); }

private:
    ModuleGraphInstance(VM&, Structure*);
    void finishCreation(VM&, JSGlobalObject*, JSScope*);
    JSModuleRecord* cloneSubgraph(JSGlobalObject*, JSModuleRecord* templateRecord, Vector<AbstractModuleRecord*>& created);

    struct Entry {
        WriteBarrier<AbstractModuleRecord> templateRecord;
        WriteBarrier<AbstractModuleRecord> instanceRecord;
    };
    UncheckedKeyHashMap<AbstractModuleRecord*, Entry> m_records;
    WriteBarrier<JSGlobalObject> m_globalObject;
    WriteBarrier<JSScope> m_parentScope;
    WriteBarrier<Unknown> m_embedderData;
    bool m_cleared { false };
};

template<typename Functor>
void ModuleGraphInstance::forEachRecord(const Functor& functor) const
{
    for (auto& [key, entry] : m_records)
        functor(*entry.templateRecord.get(), *entry.instanceRecord.get());
}

} // namespace JSC
