/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSModuleEnvironment.h"

#include "AbstractModuleRecord.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironmentInlines.h"
#include "ModuleGraphInstance.h"
#include "JSModuleRecord.h"
#include "SyntheticModuleRecord.h"

namespace JSC {

const ClassInfo JSModuleEnvironment::s_info = { "JSModuleEnvironment"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSModuleEnvironment) };

JSModuleEnvironment* JSModuleEnvironment::create(
    VM& vm, Structure* structure, JSScope* currentScope, SymbolTable* symbolTable, JSValue initialValue, AbstractModuleRecord* moduleRecord)
{
    // JSLexicalEnvironment has the storage to store the variable slots after the its class storage.
    // Because the offset of the variable slots are fixed in the JSLexicalEnvironment, inheritting these class and adding new member field is not allowed,
    // the new member will overlap the variable slots.
    // To keep the JSModuleEnvironment compatible to the JSLexicalEnvironment but add the new member to store the AbstractModuleRecord, we additionally allocate
    // the storage after the variable slots.
    //
    // JSLexicalEnvironment:
    //     [ JSLexicalEnvironment ][ variable slots ]
    //
    // JSModuleEnvironment:
    //     [ JSLexicalEnvironment ][ variable slots ][ additional slots for JSModuleEnvironment ]
    //     ... [ module record ][ graph instance ][ import slots (importSlotCount) ]
    unsigned importSlotCount = moduleRecord ? moduleRecord->importSlotCount() : 0;
    JSModuleEnvironment* result =
        new (
            NotNull,
            allocateCell<JSModuleEnvironment>(vm, JSModuleEnvironment::allocationSize(symbolTable, importSlotCount)))
        JSModuleEnvironment(vm, structure, currentScope, symbolTable, initialValue, moduleRecord);
    result->importSlotCountSlot() = importSlotCount;
    for (unsigned i = 0; i < importSlotCount; ++i)
        result->importSlot(i).setStartingValue(JSValue());
    result->finishCreation(vm);
    return result;
}

template<typename Visitor>
void JSModuleEnvironment::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSModuleEnvironment* thisObject = uncheckedDowncast<JSModuleEnvironment>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.appendValues(thisObject->variables(), thisObject->symbolTable()->scopeSize());
    visitor.append(thisObject->moduleRecordSlot());
    visitor.append(thisObject->graphInstanceSlot());
    if (unsigned count = thisObject->importSlotCount())
        visitor.appendValues(std::bit_cast<WriteBarrierBase<Unknown>*>(std::bit_cast<char*>(thisObject) + offsetOfImportSlot(thisObject->symbolTable(), 0)), count);
}

void JSModuleEnvironment::fillImportSlots(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    AbstractModuleRecord* record = moduleRecord();
    if (!record)
        return;
    UNUSED_PARAM(globalObject);
    ModuleGraphInstance* instance = graphInstance();
    ASSERT(importSlotCount() == record->importSlotCount());
    unsigned count = std::min(importSlotCount(), record->importSlotCount());
    for (unsigned i = 0; i < count; ++i) {
        if (importSlot(i).get())
            continue;
        AbstractModuleRecord* exporter = record->importedRecordAt(i);
        JSModuleEnvironment* target = nullptr;
        if (instance) {
            if (JSModuleEnvironment* found = instance->environment(exporter))
                target = found;
            else if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(exporter); synthetic && !synthetic->hasPerGraphInstanceState())
                target = exporter->moduleEnvironmentMayBeNull(); // stateless synthetic exporters are shared with the primary graph
            else if (!dynamicDowncast<JSModuleRecord>(exporter) && !dynamicDowncast<SyntheticModuleRecord>(exporter))
                target = exporter->moduleEnvironmentMayBeNull();
        } else
            target = exporter->moduleEnvironmentMayBeNull();
        if (target)
            importSlot(i).set(vm, this, target);
    }
}

JSModuleEnvironment* JSModuleEnvironment::importedEnvironmentFor(JSGlobalObject* globalObject, AbstractModuleRecord* exporter)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ModuleGraphInstance* instance = graphInstance();
    if (!instance)
        return exporter->moduleEnvironment();
    // A binding of this module itself resolves to this environment, and an
    // import whose slot has been filled resolves through the slot: code that
    // runs from an instance keeps resolving within that instance whatever the
    // instance map holds by then (an embedder may clear it once the instance
    // is no longer wanted for new imports).
    AbstractModuleRecord* record = moduleRecord();
    if (record == exporter)
        return this;
    std::optional<unsigned> slotIndex = record ? record->importSlotIndexFor(exporter) : std::nullopt;
    if (slotIndex) {
        if (JSValue filled = importSlot(*slotIndex).get(); filled && filled.isCell())
            return uncheckedDowncast<JSModuleEnvironment>(filled);
    }
    JSModuleEnvironment* environment = exporter->graphInstanceEnvironment(globalObject, instance, true);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!environment)
        environment = exporter->moduleEnvironment(); // shared with the primary graph
    // Fill the slot so the interpreter and JIT fast paths take over from here.
    if (slotIndex && environment)
        importSlot(*slotIndex).set(vm, this, environment);
    return environment;
}

ModuleGraphInstance* JSModuleEnvironment::graphInstance()
{
    JSValue value = graphInstanceSlot().get();
    return value && value.isCell() ? uncheckedDowncast<ModuleGraphInstance>(value.asCell()) : nullptr;
}

void JSModuleEnvironment::setGraphInstance(VM& vm, ModuleGraphInstance* instance)
{
    graphInstanceSlot().set(vm, this, instance ? JSValue(instance) : JSValue());
}

DEFINE_VISIT_CHILDREN(JSModuleEnvironment);

bool JSModuleEnvironment::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSModuleEnvironment* thisObject = uncheckedDowncast<JSModuleEnvironment>(cell);
    AbstractModuleRecord::Resolution resolution = thisObject->moduleRecord()->resolveImport(globalObject, Identifier::fromUid(vm, propertyName.uid()));
    RETURN_IF_EXCEPTION(scope, false);
    if (resolution.type == AbstractModuleRecord::Resolution::Type::Resolved) {
        // When resolveImport resolves the resolution, the imported module environment must have the binding.
        JSModuleEnvironment* importedModuleEnvironment = thisObject->importedEnvironmentFor(globalObject, resolution.moduleRecord);
        RETURN_IF_EXCEPTION(scope, false);
        PropertySlot redirectSlot(importedModuleEnvironment, PropertySlot::InternalMethodType::Get);
        bool result = importedModuleEnvironment->methodTable()->getOwnPropertySlot(importedModuleEnvironment, globalObject, resolution.localName, redirectSlot);
        ASSERT_UNUSED(result, result);
        ASSERT(redirectSlot.isValue());
        JSValue value = redirectSlot.getValue(globalObject, resolution.localName);
        scope.assertNoException();
        slot.setValue(thisObject, redirectSlot.attributes(), value);
        return true;
    }
    return Base::getOwnPropertySlot(thisObject, globalObject, propertyName, slot);
}

void JSModuleEnvironment::getOwnSpecialPropertyNames(JSObject* cell, JSGlobalObject*, PropertyNameArrayBuilder& propertyNamesArray, DontEnumPropertiesMode)
{
    JSModuleEnvironment* thisObject = uncheckedDowncast<JSModuleEnvironment>(cell);
    if (propertyNamesArray.includeStringProperties()) {
        for (const auto& pair : thisObject->moduleRecord()->importEntries()) {
            const AbstractModuleRecord::ImportEntry& importEntry = pair.value;
            if (importEntry.type == AbstractModuleRecord::ImportEntryType::Single
#if USE(BUN_JSC_ADDITIONS)
                || importEntry.type == AbstractModuleRecord::ImportEntryType::SingleTypeScript
#endif
            )
                propertyNamesArray.add(importEntry.localName);
        }
    }
}

bool JSModuleEnvironment::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSModuleEnvironment* thisObject = uncheckedDowncast<JSModuleEnvironment>(cell);
    // All imported bindings are immutable.
    AbstractModuleRecord::Resolution resolution = thisObject->moduleRecord()->resolveImport(globalObject, Identifier::fromUid(vm, propertyName.uid()));
    RETURN_IF_EXCEPTION(scope, false);
    if (resolution.type == AbstractModuleRecord::Resolution::Type::Resolved) {
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
        return false;
    }
    RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, propertyName, value, slot));
}

bool JSModuleEnvironment::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSModuleEnvironment* thisObject = uncheckedDowncast<JSModuleEnvironment>(cell);
    // All imported bindings are immutable.
    AbstractModuleRecord::Resolution resolution = thisObject->moduleRecord()->resolveImport(globalObject, Identifier::fromUid(vm, propertyName.uid()));
    RETURN_IF_EXCEPTION(scope, false);
    if (resolution.type == AbstractModuleRecord::Resolution::Type::Resolved)
        return false;
    return Base::deleteProperty(thisObject, globalObject, propertyName, slot);
}

} // namespace JSC
