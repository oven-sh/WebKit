/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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
#include "JSModuleNamespaceObject.h"

#include "AbstractModuleRecord.h"
#include "CyclicModuleRecord.h"
#include "JSCInlines.h"
#include "JSModuleEnvironment.h"
#include "ModuleGraphInstance.h"
#include "JSModuleRecord.h"
#if USE(BUN_JSC_ADDITIONS)
#include "SyntheticModuleRecord.h"
#endif

namespace JSC {

const ClassInfo JSModuleNamespaceObject::s_info = { "ModuleNamespaceObject"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSModuleNamespaceObject) };

JSModuleNamespaceObject::JSModuleNamespaceObject(VM& vm, Structure* structure, AbstractModuleRecord* moduleRecord, Vector<std::pair<Identifier, AbstractModuleRecord::Resolution>>&& resolutions, bool isDeferred)
    : Base(vm, structure)
    , m_exports()
    , m_moduleRecord(moduleRecord, WriteBarrierEarlyInit)
    , m_isDeferred(isDeferred)
{
    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects
    // Quoted from the spec:
    //     A List containing the String values of the exported names exposed as own properties of this object.
    //     The list is ordered as if an Array of those String values had been sorted using Array.prototype.sort using SortCompare as comparator.
    //
    // Sort the exported names by the code point order.
    std::ranges::sort(resolutions, WTF::codePointCompareLessThan, [](const auto& resolution) {
        return StringView(resolution.first.impl());
    });

    for (const auto& pair : resolutions) {
        m_exports.add(pair.first.impl(), ExportEntry {
            pair.second.localName,
            WriteBarrier { pair.second.moduleRecord, WriteBarrierEarlyInit },
        });
    }
}

void JSModuleNamespaceObject::finishCreation(JSGlobalObject* globalObject, bool shouldPreventExtensions)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, m_isDeferred ? "Deferred Module"_s : "Module"_s), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);

#if USE(BUN_JSC_ADDITIONS)
    if (shouldPreventExtensions)
#else
    UNUSED_PARAM(shouldPreventExtensions);
#endif
    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects-getprototypeof
    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects-setprototypeof-v
    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects-isextensible
    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects-preventextensions
    methodTable()->preventExtensions(this, globalObject);

    scope.assertNoExceptionExceptTermination();
}

void JSModuleNamespaceObject::destroy(JSCell* cell)
{
    JSModuleNamespaceObject* thisObject = static_cast<JSModuleNamespaceObject*>(cell);
    thisObject->JSModuleNamespaceObject::~JSModuleNamespaceObject();
}

template<typename Visitor>
void JSModuleNamespaceObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_moduleRecord);
    visitor.append(thisObject->m_graphInstance);
    visitor.append(thisObject->m_instanceEnvironment);
    for (auto& entry : thisObject->m_exports.values())
        visitor.appendHidden(entry.moduleRecord);
}

DEFINE_VISIT_CHILDREN(JSModuleNamespaceObject);

void JSModuleNamespaceObject::setGraphInstance(VM& vm, ModuleGraphInstance* instance, JSModuleEnvironment* environment)
{
    m_graphInstance.setMayBeNull(vm, this, instance);
    m_instanceEnvironment.setMayBeNull(vm, this, environment);
}

JSModuleEnvironment* JSModuleNamespaceObject::environmentFor(JSGlobalObject* globalObject, AbstractModuleRecord* record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (ModuleGraphInstance* instance = m_graphInstance.get()) {
        // Own exports: this instance's environment; re-exports: through its
        // import slots; then the instance. A namespace stays bound to the
        // instance it was created for.
        if (JSModuleEnvironment* own = m_instanceEnvironment.get()) {
            if (record == m_moduleRecord.get())
                return own;
            RELEASE_AND_RETURN(scope, own->importedEnvironmentFor(globalObject, record));
        }
        if (JSModuleEnvironment* found = instance->environment(record))
            return found;
        if (instance->isCleared()) {
            throwTypeError(globalObject, scope, "Module namespace belongs to a module graph instance that was disposed"_s);
            return nullptr;
        }
        // Otherwise the record is one the instance shares with the primary graph.
    }
    return record->moduleEnvironment();
}

// https://tc39.es/proposal-defer-import-eval/#sec-IsSymbolLikeNamespaceKey
ALWAYS_INLINE bool JSModuleNamespaceObject::isSymbolLikeNamespaceKey(VM& vm, PropertyName propertyName)
{
    return propertyName.isSymbol() || (m_isDeferred && propertyName == vm.propertyNames->then);
}

// https://tc39.es/proposal-defer-import-eval/#sec-GetModuleExportsList
// The spec returns O.[[Exports]] after triggering evaluation; here we only perform the
// evaluation side effect since callers already have direct access to m_exports.
void JSModuleNamespaceObject::ensureDeferredNamespaceEvaluation(JSGlobalObject* globalObject)
{
    // 1. If O.[[Deferred]] is true, then
    ASSERT(m_isDeferred);
    // A namespace of a module graph instance evaluates its module in that instance.
    ModuleGraphInstance* instance = m_graphInstance.get();
    if (instance && instance->isCleared()) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwTypeError(globalObject, scope, "Module namespace belongs to a module graph instance that was disposed"_s);
        return;
    }
    // Fast path: if the module's cycle has already successfully evaluated, EvaluateModuleSync would
    // observe a fulfilled promise and return without throwing, so we can skip the work entirely.
    // We must consult [[CycleRoot]] here because Evaluate() redirects to it; for a non-root SCC
    // member, status/evaluationError on the module itself may not reflect the cycle's outcome.
    if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(m_moduleRecord.get())) {
        CyclicModuleRecord* root = cyclic->cycleRoot(instance) ? cyclic->cycleRoot(instance) : cyclic;
        if (root->status(instance) == CyclicModuleRecord::Status::Evaluated && !root->evaluationError(instance))
            return;
    }
    //   1.a. Let m be O.[[Module]].
    //   1.b. Perform ? EvaluateModuleSync(m).
    m_moduleRecord->evaluateSync(globalObject, instance);
    // 2. Return O.[[Exports]].
}

static JSValue getValue(JSModuleEnvironment* environment, PropertyName localName, ScopeOffset& scopeOffset)
{
    SymbolTable* symbolTable = environment->symbolTable();
    {
        ConcurrentJSLocker locker(symbolTable->m_lock);
        auto iter = symbolTable->find(locker, localName.uid());
        ASSERT(iter != symbolTable->end(locker));
        SymbolTableEntry& entry = iter->value;
        ASSERT(!entry.isNull());
        scopeOffset = entry.scopeOffset();
    }
    return environment->variableAt(scopeOffset).get();
}

bool JSModuleNamespaceObject::getOwnPropertySlotCommon(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects-getownproperty-p

    // 1. If IsSymbolLikeNamespaceKey(P, O) is true, then return ! Ordinary{Get,GetOwnProperty,HasProperty}(O, P, ...).
    // It may return the descriptor with writable: true, but namespace objects does not allow it in [[Set]] / [[DefineOwnProperty]] side.
    if (isSymbolLikeNamespaceKey(vm, propertyName))
        return Base::getOwnPropertySlot(this, globalObject, propertyName, slot);

    slot.setIsTaintedByOpaqueObject();

    if (m_isDeferred && slot.internalMethodType() != PropertySlot::InternalMethodType::VMInquiry) [[unlikely]] {
        ensureDeferredNamespaceEvaluation(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }

    auto iterator = m_exports.find(propertyName.uid());
    if (iterator == m_exports.end()) {
#if USE(BUN_JSC_ADDITIONS)
        if (propertyName == vm.propertyNames->__esModule) {
            return Base::getOwnPropertySlot(this, globalObject, propertyName, slot);
        }
#endif
        return false;
    }

    ExportEntry& exportEntry = iterator->value;

    switch (slot.internalMethodType()) {
    case PropertySlot::InternalMethodType::GetOwnProperty:
    case PropertySlot::InternalMethodType::Get: {
        if (exportEntry.localName == vm.propertyNames->starNamespacePrivateName) {
            // https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-get-p-receiver
            // 10. If binding.[[BindingName]] is "*namespace*", then
            //     a. Return ? GetModuleNamespace(targetModule).
            // We call getModuleNamespace() to ensure materialization. And after that, looking up the value from the scope to encourage module namespace object IC.
            exportEntry.moduleRecord->getModuleNamespace(globalObject, m_graphInstance.get());
            RETURN_IF_EXCEPTION(scope, false);
        }
        JSModuleEnvironment* environment = environmentFor(globalObject, exportEntry.moduleRecord.get());
        RETURN_IF_EXCEPTION(scope, false);
        ScopeOffset scopeOffset;
        JSValue value = getValue(environment, exportEntry.localName, scopeOffset);
#if USE(BUN_JSC_ADDITIONS)
        if (!value) [[unlikely]] {
            // Same idea as the *namespace* case above: a lazy export of a SyntheticModuleRecord is materialized on
            // first read, then looked up from the scope again so that the module namespace object IC applies to it.
            SyntheticModuleRecord::materializeLazyExport(globalObject, exportEntry.moduleRecord.get(), exportEntry.localName);
            RETURN_IF_EXCEPTION(scope, false);
            value = getValue(environment, exportEntry.localName, scopeOffset);
        }
#endif
        // If the value is filled with TDZ value, throw a reference error.
        if (!value) {
            RefPtr uid = propertyName.uid();
            throwVMError(globalObject, scope, createTDZError(globalObject, *uid));
            return false;
        }

        slot.setValueModuleNamespace(this, static_cast<unsigned>(PropertyAttribute::DontDelete), value, environment, scopeOffset);
        return true;
    }

    case PropertySlot::InternalMethodType::HasProperty: {
        // Do not perform [[Get]] for [[HasProperty]].
        // [[Get]] / [[GetOwnProperty]] onto namespace object could throw an error while [[HasProperty]] just returns true here.
        // https://tc39.github.io/ecma262/#sec-module-namespace-exotic-objects-hasproperty-p
        slot.setValue(this, static_cast<unsigned>(PropertyAttribute::DontDelete), jsUndefined());
        return true;
    }

    case PropertySlot::InternalMethodType::VMInquiry:
        slot.setValue(this, static_cast<unsigned>(JSC::PropertyAttribute::None), jsUndefined());
        return false;
    }

    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

bool JSModuleNamespaceObject::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    return thisObject->getOwnPropertySlotCommon(globalObject, propertyName, slot);
}

bool JSModuleNamespaceObject::getOwnPropertySlotByIndex(JSObject* cell, JSGlobalObject* globalObject, unsigned propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    return thisObject->getOwnPropertySlotCommon(globalObject, Identifier::from(vm, propertyName), slot);
}

bool JSModuleNamespaceObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    auto* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    if (thisObject->m_isOverridingValue) {
        return true;
    } else if (propertyName == vm.propertyNames->__esModule && !thisObject->m_exports.contains(propertyName.uid())) [[unlikely]] {
        RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, propertyName, value, slot));
    }
#endif

    // http://www.ecma-international.org/ecma-262/6.0/#sec-module-namespace-exotic-objects-set-p-v-receiver
    if (slot.isStrictMode())
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool JSModuleNamespaceObject::putByIndex(JSCell*, JSGlobalObject* globalObject, unsigned, JSValue, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (shouldThrow)
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool JSModuleNamespaceObject::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    // https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-delete-p
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    if (thisObject->isSymbolLikeNamespaceKey(vm, propertyName))
        RELEASE_AND_RETURN(scope, Base::deleteProperty(thisObject, globalObject, propertyName, slot));

    if (thisObject->m_isDeferred) [[unlikely]] {
        thisObject->ensureDeferredNamespaceEvaluation(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }

    return !thisObject->m_exports.contains(propertyName.uid());
}

bool JSModuleNamespaceObject::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned propertyName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    if (thisObject->m_isDeferred) [[unlikely]] {
        thisObject->ensureDeferredNamespaceEvaluation(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return !thisObject->m_exports.contains(Identifier::from(vm, propertyName).impl());
}

void JSModuleNamespaceObject::getOwnPropertyNames(JSObject* cell, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-ownpropertykeys
    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);
    if (thisObject->m_isDeferred) [[unlikely]] {
        thisObject->ensureDeferredNamespaceEvaluation(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (const auto& name : thisObject->m_exports.keys()) {
        if (mode == DontEnumPropertiesMode::Exclude) {
            // Perform [[GetOwnProperty]] to throw ReferenceError if binding is uninitialized.
            PropertySlot slot(cell, PropertySlot::InternalMethodType::GetOwnProperty);
            bool hasProperty = thisObject->getOwnPropertySlotCommon(globalObject, name.get(), slot);
            RETURN_IF_EXCEPTION(scope, void());
            if (!hasProperty)
                continue;
        }
        propertyNames.add(name);
    }
    if (propertyNames.includeSymbolProperties()) {
        scope.release();
        thisObject->getOwnNonIndexPropertyNames(globalObject, propertyNames, mode);
    }
}

bool JSModuleNamespaceObject::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-defineownproperty-p-desc

    JSModuleNamespaceObject* thisObject = uncheckedDowncast<JSModuleNamespaceObject>(cell);

    // 1. If IsSymbolLikeNamespaceKey(P, O) is true, return ! OrdinaryDefineOwnProperty(O, P, Desc).
    if (thisObject->isSymbolLikeNamespaceKey(vm, propertyName))
        RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow));

    // 2. Let current be ? O.[[GetOwnProperty]](P).
    PropertyDescriptor current;
    bool isCurrentDefined = thisObject->getOwnPropertyDescriptor(globalObject, propertyName, current);
    RETURN_IF_EXCEPTION(scope, false);

#if USE(BUN_JSC_ADDITIONS)
    if (thisObject->m_isOverridingValue) {
        return true;
    } else if (propertyName == vm.propertyNames->__esModule && !thisObject->m_exports.contains(propertyName.uid())) {
        RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow));
    }
#endif

    // 3. If current is undefined, return false.
    if (!isCurrentDefined) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, NonExtensibleObjectPropertyDefineError);
        return false;
    }

    // 4. If IsAccessorDescriptor(Desc) is true, return false.
    if (descriptor.isAccessorDescriptor()) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, "Cannot change module namespace object's binding to accessor"_s);
        return false;
    }

    // 5. If Desc.[[Writable]] is present and has value false, return false.
    if (descriptor.writablePresent() && !descriptor.writable()) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, "Cannot change module namespace object's binding to non-writable attribute"_s);
        return false;
    }

    // 6. If Desc.[[Enumerable]] is present and has value false, return false.
    if (descriptor.enumerablePresent() && !descriptor.enumerable()) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, "Cannot replace module namespace object's binding with non-enumerable attribute"_s);
        return false;
    }

    // 7. If Desc.[[Configurable]] is present and has value true, return false.
    if (descriptor.configurablePresent() && descriptor.configurable()) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, "Cannot replace module namespace object's binding with configurable attribute"_s);
        return false;
    }

    // 8. If Desc.[[Value]] is present, return SameValue(Desc.[[Value]], current.[[Value]]).
    if (descriptor.value()) {
        bool result = sameValue(globalObject, descriptor.value(), current.value());
        RETURN_IF_EXCEPTION(scope, false);
        if (!result) {
            if (shouldThrow)
                throwTypeError(globalObject, scope, "Cannot replace module namespace object's binding's value"_s);
            return false;
        }
    }

    // 9. Return true.
    return true;
}

#if USE(BUN_JSC_ADDITIONS)

void JSModuleNamespaceObject::overrideExports(JSGlobalObject* globalObject, const WTF::Function<bool(JSGlobalObject* globalObject, const Identifier& exportName, JSC::JSValue& result)>& iter)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    for (auto& pair : m_exports) {
        JSValue value = jsUndefined();
        const Identifier& name = pair.value.localName;

        if (iter(globalObject, name, value)) {
            auto* moduleNamespaceObject = pair.value.moduleRecord->getModuleNamespace(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            bool putResult = false;
            moduleNamespaceObject->m_isOverridingValue = true;
            if (JSModuleEnvironment* moduleEnvironment = pair.value.moduleRecord->moduleEnvironmentMayBeNull()) {
                symbolTablePutTouchWatchpointSet(moduleEnvironment, globalObject, name, value, false, true, putResult);
            }
            JSC::PutPropertySlot putter = JSC::PutPropertySlot(moduleNamespaceObject, false);
            moduleNamespaceObject->put(moduleNamespaceObject, globalObject, name, value, putter);
            moduleNamespaceObject->m_isOverridingValue = false;
        }
    }
}

bool JSModuleNamespaceObject::overrideExportValue(JSGlobalObject* globalObject, PropertyName name, JSValue value)
{
    auto& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSC::Identifier identifier = Identifier::fromUid(vm, name.uid());
    auto resolution = moduleRecord()->resolveExport(globalObject, identifier);
    RETURN_IF_EXCEPTION(scope, false);

    // FIXME: figure out how to do this
    // Support setting a default export value when it wasn't already exported.
    // if (resolution.type == AbstractModuleRecord::Resolution::Type::Error && name == "default"_s) {
    //     resolution.type = AbstractModuleRecord::Resolution::Type::Resolved;
    //     resolution.localName = identifier;
    //     resolution.moduleRecord = moduleRecord();
    // }

    if (resolution.type != AbstractModuleRecord::Resolution::Type::Resolved) {
        return false;
    }

    auto* record = resolution.moduleRecord;
    auto* moduleNamespaceObject = record->getModuleNamespace(globalObject);
    RETURN_IF_EXCEPTION(scope, false);

    bool putResult = false;
    moduleNamespaceObject->m_isOverridingValue = true;
    if (JSModuleEnvironment* moduleEnvironment = record->moduleEnvironmentMayBeNull()) {
        symbolTablePutTouchWatchpointSet(moduleEnvironment, globalObject, resolution.localName, value, false, true, putResult);
        RETURN_IF_EXCEPTION(scope, {});
    }
    JSC::PutPropertySlot putter = JSC::PutPropertySlot(moduleNamespaceObject, false);
    putResult = moduleNamespaceObject->put(moduleNamespaceObject, globalObject, name, value, putter);
    RETURN_IF_EXCEPTION(scope, {});
    moduleNamespaceObject->m_isOverridingValue = false;
    return putResult;
}

#endif

} // namespace JSC
