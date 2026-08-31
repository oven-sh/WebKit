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
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "JSInternalFieldObjectImplInlines.h"
#include "JSModuleEnvironment.h"
#include "JSModuleNamespaceObject.h"
#include "JSPromise.h"
#include "ModuleGraphInstanceInlines.h"

namespace JSC {

const ClassInfo ModuleRecordInstance::s_info = { "ModuleRecordInstance"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleRecordInstance) };
const ClassInfo ModuleGraphInstance::s_info = { "ModuleGraphInstance"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleGraphInstance) };

ModuleRecordInstance::ModuleRecordInstance(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void ModuleRecordInstance::destroy(JSCell* cell)
{
    SUPPRESS_MEMORY_UNSAFE_CAST auto* thisObject = static_cast<ModuleRecordInstance*>(cell);
    thisObject->~ModuleRecordInstance();
}

ModuleRecordInstance* ModuleRecordInstance::create(VM& vm, ModuleGraphInstance* graphInstance, AbstractModuleRecord* record, JSModuleEnvironment* environment)
{
    Structure* structure = vm.moduleRecordInstanceStructure.get();
    ModuleRecordInstance* instance = new (NotNull, allocateCell<ModuleRecordInstance>(vm)) ModuleRecordInstance(vm, structure);
    instance->finishCreation(vm, graphInstance, record, environment);
    return instance;
}

void ModuleRecordInstance::finishCreation(VM& vm, ModuleGraphInstance* graphInstance, AbstractModuleRecord* record, JSModuleEnvironment* environment)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    auto initialValues = AbstractModuleRecord::initialValues();
    for (unsigned index = 0; index < numberOfInternalFields; ++index)
        internalField(static_cast<Field>(index)).set(vm, this, initialValues[index]);
    m_graphInstance.set(vm, this, graphInstance);
    m_record.set(vm, this, record);
    m_environment.setMayBeNull(vm, this, environment);
}

template<typename Visitor>
void ModuleRecordInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleRecordInstance>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_record);
    visitor.append(thisObject->m_graphInstance);
    visitor.append(thisObject->m_environment);
    visitor.append(thisObject->m_evaluationError);
    visitor.append(thisObject->m_cycleRoot);
    visitor.append(thisObject->m_topLevelCapability);
    visitor.append(thisObject->m_asyncCapability);
    visitor.append(thisObject->m_deferredNamespaceObject);
    Locker locker { thisObject->cellLock() };
    visitor.append(thisObject->m_asyncParentModules.begin(), thisObject->m_asyncParentModules.end());
}

DEFINE_VISIT_CHILDREN(ModuleRecordInstance);

void ModuleRecordInstance::appendAsyncParentModule(VM& vm, AbstractModuleRecord* record)
{
    Locker locker { cellLock() };
    m_asyncParentModules.append(WriteBarrier<AbstractModuleRecord>(vm, this, record));
}

// Same encoding as JSModuleRecord::isTopLevelExecutionFinished(): Field::State is
// the module body generator's resume point — a body that ran to completion leaves
// it at Executing (nothing to resume), a body suspended at a top-level await
// stores its resume label instead.
bool ModuleRecordInstance::isTopLevelExecutionFinished() const
{
    JSValue state = internalField(Field::State).get();
    return !state.isNumber() || state.asInt32AsAnyInt() == std::to_underlying(AbstractModuleRecord::State::Executing);
}

void ModuleRecordInstance::setDeferredNamespaceObject(VM& vm, JSModuleNamespaceObject* namespaceObject)
{
    m_deferredNamespaceObject.setMayBeNull(vm, this, namespaceObject);
}

ModuleGraphInstance::ModuleGraphInstance(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

ModuleGraphInstance* ModuleGraphInstance::create(VM& vm, JSGlobalObject* globalObject, JSScope* parentScope)
{
    Structure* structure = globalObject->moduleGraphInstanceStructure();
    ModuleGraphInstance* instance = new (NotNull, allocateCell<ModuleGraphInstance>(vm)) ModuleGraphInstance(vm, structure);
    instance->finishCreation(vm, parentScope);
    return instance;
}

void ModuleGraphInstance::finishCreation(VM& vm, JSScope* parentScope)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    m_parentScope.setMayBeNull(vm, this, parentScope);
}

template<typename Visitor>
void ModuleGraphInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleGraphInstance>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_parentScope);
    Locker locker { thisObject->cellLock() };
    for (auto& [record, instance] : thisObject->m_records)
        visitor.append(instance);
}

DEFINE_VISIT_CHILDREN(ModuleGraphInstance);

ModuleRecordInstance* ModuleGraphInstance::recordInstance(AbstractModuleRecord* record) const
{
    auto iterator = m_records.find(record);
    return iterator == m_records.end() ? nullptr : iterator->value.get();
}

ModuleRecordInstance* ModuleGraphInstance::add(VM& vm, AbstractModuleRecord* record, JSModuleEnvironment* environment)
{
    RELEASE_ASSERT(!m_cleared); // callers check isCleared() and throw first
    // Idempotent: host code that runs while a record is instantiated (a
    // synthetic module's generator) may have instantiated it re-entrantly.
    if (ModuleRecordInstance* existing = recordInstance(record))
        return existing;
    ModuleRecordInstance* instance = ModuleRecordInstance::create(vm, this, record, environment);
    environment->setGraphInstance(vm, this);
    Locker locker { cellLock() };
    m_records.add(record, WriteBarrier<ModuleRecordInstance>(vm, this, instance));
    return instance;
}

bool ModuleGraphInstance::remove(AbstractModuleRecord* record)
{
    Locker locker { cellLock() };
    return m_records.remove(record);
}

void ModuleGraphInstance::destroy(JSCell* cell)
{
    SUPPRESS_MEMORY_UNSAFE_CAST auto* thisObject = static_cast<ModuleGraphInstance*>(cell);
    thisObject->~ModuleGraphInstance();
}

void ModuleGraphInstance::clear(JSGlobalObject* globalObject)
{
    if (m_busy) {
        // An evaluation step is running against this instance (its module code
        // asked for the disposal): finish that step coherently, clear after it.
        m_clearPending = true;
        return;
    }
    m_clearPending = false;
    VM& vm = globalObject->vm();
    // Pending top-level evaluation promises are rejected below; keep them alive
    // (they were reachable only through the records' state) across the
    // allocation of the error.
    MarkedArgumentBuffer pending;
    {
        Locker locker { cellLock() };
        m_cleared = true;
        for (auto& entry : m_records) {
            JSPromise* capability = entry.value->topLevelCapability();
            if (capability && capability->status() == JSPromise::Status::Pending)
                pending.append(capability);
        }
        m_records.clear();
    }
    RELEASE_ASSERT(!pending.hasOverflowed());
    if (pending.isEmpty())
        return;
    // May run as a deferred clear when an evaluation step unwinds with an
    // exception pending (BusyScope): rejecting is bookkeeping, not a new throw.
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendException(vm);
    JSObject* error = createTypeError(globalObject, "Module graph instance was disposed during evaluation"_s);
    for (unsigned i = 0; i < pending.size(); ++i)
        uncheckedDowncast<JSPromise>(pending.at(i))->reject(vm, JSValue(error));
}

} // namespace JSC
