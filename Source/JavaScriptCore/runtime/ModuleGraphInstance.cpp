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

#include "config.h"
#include "ModuleGraphInstance.h"

#include "DeferTermination.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "JSPromise.h"
#include "SyntheticModuleRecord.h"
#include "ModuleGraphInstanceInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC {

const ClassInfo ModuleGraphInstance::s_info = { "ModuleGraphInstance"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleGraphInstance) };

ModuleGraphInstance::ModuleGraphInstance(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void ModuleGraphInstance::destroy(JSCell* cell)
{
    SUPPRESS_MEMORY_UNSAFE_CAST auto* thisObject = static_cast<ModuleGraphInstance*>(cell);
    thisObject->~ModuleGraphInstance();
}

ModuleGraphInstance* ModuleGraphInstance::create(VM& vm, JSGlobalObject* globalObject, JSScope* parentScope)
{
    ModuleGraphInstance* instance = new (NotNull, allocateCell<ModuleGraphInstance>(vm)) ModuleGraphInstance(vm, vm.moduleGraphInstanceStructure.get());
    instance->finishCreation(vm, globalObject, parentScope);
    return instance;
}

void ModuleGraphInstance::finishCreation(VM& vm, JSGlobalObject* globalObject, JSScope* parentScope)
{
    Base::finishCreation(vm);
    m_globalObject.set(vm, this, globalObject);
    m_parentScope.setMayBeNull(vm, this, parentScope);
}

template<typename Visitor>
void ModuleGraphInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleGraphInstance>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_globalObject);
    visitor.append(thisObject->m_parentScope);
    visitor.append(thisObject->m_embedderData);
    Locker locker { thisObject->cellLock() };
    for (auto& [key, entry] : thisObject->m_records) {
        visitor.append(entry.templateRecord);
        visitor.append(entry.instanceRecord);
    }
}

DEFINE_VISIT_CHILDREN(ModuleGraphInstance);

AbstractModuleRecord* ModuleGraphInstance::recordFor(AbstractModuleRecord* templateRecord) const
{
    auto iterator = m_records.find(templateRecord);
    return iterator == m_records.end() ? nullptr : iterator->value.instanceRecord.get();
}

JSModuleRecord* ModuleGraphInstance::sourceTextRecordFor(AbstractModuleRecord* templateRecord) const
{
    return dynamicDowncast<JSModuleRecord>(recordFor(templateRecord));
}

void ModuleGraphInstance::add(VM& vm, AbstractModuleRecord* templateRecord, AbstractModuleRecord* instanceRecord)
{
    Entry entry;
    entry.templateRecord.setWithoutWriteBarrier(templateRecord);
    entry.instanceRecord.setWithoutWriteBarrier(instanceRecord);
    {
        Locker locker { cellLock() };
        m_records.set(templateRecord, WTF::move(entry));
    }
    vm.writeBarrier(this, templateRecord);
    vm.writeBarrier(this, instanceRecord);
}

bool ModuleGraphInstance::remove(AbstractModuleRecord* templateRecord)
{
    Locker locker { cellLock() };
    return m_records.remove(templateRecord);
}

void ModuleGraphInstance::clear()
{
    if (m_cleared)
        return;
    JSGlobalObject* globalObject = m_globalObject.get();
    VM& vm = globalObject->vm();
    // Top-level evaluations of this instance that are still pending settle now:
    // their continuations (JSMicrotask.cpp) drop once the instance is cleared, so
    // nobody else would ever settle the promises import() handed out.
    MarkedArgumentBuffer pending;
    {
        Locker locker { cellLock() };
        m_cleared = true;
        for (auto& [key, entry] : m_records) {
            auto* cyclic = dynamicDowncast<CyclicModuleRecord>(entry.instanceRecord.get());
            JSPromise* capability = cyclic ? cyclic->topLevelCapability() : nullptr;
            if (capability && capability->status() == JSPromise::Status::Pending)
                pending.append(capability);
        }
        m_records.clear();
    }
    RELEASE_ASSERT(!pending.hasOverflowed());
    if (pending.isEmpty())
        return;
    // May run while an evaluation step unwinds with an exception pending:
    // rejecting is bookkeeping, not a new throw.
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendException(vm);
    JSObject* error = createTypeError(globalObject, "Module graph instance was disposed during evaluation"_s);
    for (unsigned i = 0; i < pending.size(); ++i)
        uncheckedDowncast<JSPromise>(pending.at(i))->reject(vm, JSValue(error));
}

// This instance's record for templateRecord and, recursively, for the Source
// Text Module Records it requests; [[LoadedModules]] of each new record is filled
// so GetImportedModule works on it like on any record. Records made here are
// appended to 'created' so a failure can take them out again.
JSModuleRecord* ModuleGraphInstance::cloneSubgraph(JSGlobalObject* globalObject, JSModuleRecord* templateRecord, Vector<AbstractModuleRecord*>& created)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (AbstractModuleRecord* existing = recordFor(templateRecord)) {
        auto* record = dynamicDowncast<JSModuleRecord>(existing);
        if (!record) [[unlikely]] {
            throwTypeError(globalObject, scope, makeString("Module '"_s, templateRecord->moduleKey().string(), "' is registered in this module graph instance as a non-source-text record"_s));
            return nullptr;
        }
        return record;
    }
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return nullptr;
    }
    using Status = CyclicModuleRecord::Status;
    if (templateRecord->status() == Status::New || templateRecord->status() == Status::Unlinked || templateRecord->status() == Status::Linking) {
        throwTypeError(globalObject, scope, makeString("Module '"_s, templateRecord->moduleKey().string(), "' must be linked before it can be instantiated into a module graph instance"_s));
        return nullptr;
    }

    JSModuleRecord* record = JSModuleRecord::createForGraphInstance(globalObject, vm, templateRecord, this);
    RETURN_IF_EXCEPTION(scope, nullptr);
    add(vm, templateRecord, record);
    created.append(templateRecord);

    const auto& requests = templateRecord->requestedModules();
    for (unsigned i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        AbstractModuleRecord* dependency = templateRecord->hostResolveImportedModule(globalObject, request.m_specifier, request.type());
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!dependency) [[unlikely]] {
            throwTypeError(globalObject, scope, makeString("Module '"_s, templateRecord->moduleKey().string(), "' has an unresolved dependency '"_s, request.m_specifier.string(), "' and cannot be instantiated into a module graph instance"_s));
            return nullptr;
        }
        // GetImportedModule on the new record answers through the template and
        // this map (AbstractModuleRecord::hostResolveImportedModule), so all that
        // is needed here is that every dependency with state of its own has its
        // record in the map.
        if (auto* sourceText = dynamicDowncast<JSModuleRecord>(dependency)) {
            cloneSubgraph(globalObject, sourceText, created);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } else if (recordFor(dependency)) {
            // registered already (an earlier instantiate, or the embedder)
        } else if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(dependency); synthetic && synthetic->regeneratesPerGraphInstance()) {
            // A host or data module with state of its own: the instance gets a
            // fresh record from the same provider / source.
            JSGlobalObject::GraphInstanceLoadingScope loading(globalObject, this);
            SyntheticModuleRecord* fresh = SyntheticModuleRecord::createForGraphInstance(globalObject, synthetic);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!fresh) [[unlikely]] {
                throwTypeError(globalObject, scope, makeString("Module '"_s, dependency->moduleKey().string(), "' could not be regenerated for a module graph instance"_s));
                return nullptr;
            }
            add(vm, dependency, fresh);
            created.append(dependency);
        }
        // else: shared with the template graph
    }
    return record;
}

JSModuleRecord* ModuleGraphInstance::instantiate(JSGlobalObject* globalObject, JSModuleRecord* templateRecord)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (m_cleared) {
        throwTypeError(globalObject, scope, "Module graph instance has been disposed"_s);
        return nullptr;
    }

    Vector<AbstractModuleRecord*> created;
    JSModuleRecord* record = cloneSubgraph(globalObject, templateRecord, created);
    auto rollBack = [&] {
        for (AbstractModuleRecord* templateOfCreated : created)
            remove(templateOfCreated);
    };
    if (scope.exception()) [[unlikely]] {
        rollBack();
        return nullptr;
    }

    // Link(): the ordinary algorithm over this instance's records (their
    // dependencies outside the instance are already linked/evaluated).
    record->link(globalObject, nullptr);
    if (scope.exception()) [[unlikely]] {
        // Link() reset the records it touched to Unlinked; a later instantiate()
        // starts over with fresh ones.
        rollBack();
        return nullptr;
    }

    return record;
}

JSPromise* ModuleGraphInstance::evaluate(JSGlobalObject* globalObject, JSModuleRecord* templateRecord)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSModuleRecord* record = instantiate(globalObject, templateRecord);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, record->evaluate(globalObject));
}

JSModuleRecord* ModuleGraphInstance::evaluateSync(JSGlobalObject* globalObject, JSModuleRecord* templateRecord)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSModuleRecord* record = instantiate(globalObject, templateRecord);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSPromise* promise = record->evaluate(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    switch (promise->status()) {
    case JSPromise::Status::Fulfilled:
        return record;
    case JSPromise::Status::Rejected:
        promise->markAsHandled();
        scope.throwException(globalObject, promise->result());
        return nullptr;
    case JSPromise::Status::Pending:
        throwTypeError(globalObject, scope, makeString("Module '"_s, record->moduleKey().string(), "' or one of its dependencies uses top-level await and cannot be evaluated synchronously"_s));
        return nullptr;
    }
    return record;
}

} // namespace JSC
