/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleNamespace.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleData.h"
#include "AOTModuleStarExports.h"
#include "AOTModules.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "JSCInlines.h"
#include "SlotVisitorInlines.h"

namespace JSC { namespace AOT {

const ClassInfo ModuleNamespace::s_info = { "Module"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespace) };

Structure* ModuleNamespace::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::preventExtensionsTransition(vm, Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info()));
}

ModuleNamespace::ModuleNamespace(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
    : Base(vm, structure)
    , m_dynamicExportNames(dynamicExports.names, WriteBarrierEarlyInit)
    , m_capturedValues(dynamicExports.capturedValues, WriteBarrierEarlyInit)
    , m_liveSources(dynamicExports.liveSources, WriteBarrierEarlyInit)
    , m_module(module)
    , m_isDeferred(phase == ModulePhase::Defer)
    , m_mayNeedEvaluation(m_isDeferred)
{
}

ModuleNamespace* ModuleNamespace::create(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
{
    ModuleNamespace* object = new (NotNull, allocateCell<ModuleNamespace>(vm)) ModuleNamespace(vm, structure, module, phase, dynamicExports);
    if (!object->modules().data(module).hasExportTable())
        object->m_mayNeedEvaluation = false;
    object->finishCreation(vm);
    return object;
}

Modules& ModuleNamespace::modules() const
{
    return *modulesOf(realm());
}

std::span<const ModuleExportData> ModuleNamespace::exports() const
{
    return modules().table().exports(m_module);
}

bool ModuleNamespace::isAmbiguousExport(unsigned index) const
{
    return exports()[index].isFromStarExport && modules().isExportAmbiguous(m_module, index);
}

void ModuleNamespace::ensureOwnStructure(VM& vm)
{
    if (hasOwnStructure())
        return;
    setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype()));
    m_hasOwnStructure = true;
}

template<typename Visitor>
void ModuleNamespace::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_names);
    visitor.append(thisObject->m_dynamicExportNames);
    visitor.append(thisObject->m_capturedValues);
    visitor.append(thisObject->m_liveSources);
}

DEFINE_VISIT_CHILDREN(ModuleNamespace);

static std::optional<unsigned> indexIn(JSCellButterfly* names, UniquedStringImpl* name)
{
    if (name->isSymbol())
        return std::nullopt;
    unsigned from = 0;
    unsigned to = names->length();
    while (from < to) {
        unsigned middle = from + (to - from) / 2;
        const StringImpl* candidate = asString(names->get(middle))->tryGetValueImpl();
        if (candidate == name)
            return middle;
        if (codePointCompare(StringView(*candidate), StringView(*name)) < 0)
            from = middle + 1;
        else
            to = middle;
    }
    return std::nullopt;
}

std::optional<unsigned> ModuleNamespace::find(UniquedStringImpl* name) const
{
    if (isSymbolLikeKey(name))
        return std::nullopt;
    std::optional<unsigned> index = modules().table().findExport(m_module, name);
    if (!index || isAmbiguousExport(*index))
        return std::nullopt;
    return index;
}

bool ModuleNamespace::tryReadExports(JSGlobalObject* globalObject, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    if (m_isDeferred || m_mayNeedEvaluation || m_dynamicExportNames)
        return false;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    std::span<const ModuleExportData> entries = exports();
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (isAmbiguousExport(index))
            continue;
        JSValue value = modules.readExport(m_module, index);
        RETURN_IF_EXCEPTION(scope, true);
        names.append(modules.identifier(entries[index].name).impl());
        values.appendWithCrashOnOverflow(value);
    }
    return true;
}

JSCellButterfly* ModuleNamespace::ensureNames(JSGlobalObject* globalObject)
{
    if (JSCellButterfly* names = m_names.get())
        return names;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance& instance = modules().instance();
    const ProgramTables& tables = instance.tables();
    ProgramStrings& strings = *instance.state->strings;
    std::span<const ModuleExportData> entries = exports();
    Vector<unsigned> indices;
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (!isAmbiguousExport(index))
            indices.append(index);
    }
    auto* names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, indices.size());
    for (unsigned i = 0; i < indices.size(); ++i) {
        JSString* name = strings.string(vm, ProgramStrings::identifierString(tables, entries[indices[i]].name));
        name->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        names->setIndex(vm, i, name);
    }
    m_names.set(vm, this, names);
    return names;
}

bool ModuleNamespace::isSymbolLikeKey(UniquedStringImpl* name) const
{
    return name->isSymbol() || (m_isDeferred && name == vm().propertyNames->then.impl());
}

bool ModuleNamespace::evaluateIfNeeded(JSGlobalObject* globalObject)
{
    if (!m_mayNeedEvaluation)
        return true;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    modules().evaluateSynchronously(m_module, SynchronousEvaluationReason::DeferredNamespace);
    RETURN_IF_EXCEPTION(scope, false);
    m_mayNeedEvaluation = false;
    return true;
}

bool ModuleNamespace::isDynamicExport(UniquedStringImpl* name) const
{
    return m_dynamicExportNames && !isSymbolLikeKey(name) && indexIn(m_dynamicExportNames.get(), name);
}

JSValue ModuleNamespace::readDynamicExport(JSGlobalObject* globalObject, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSObject* liveSources = m_liveSources.get()) {
        JSValue source = StarExportResolver::capturedValue(globalObject, liveSources, Identifier::fromUid(vm, name.uid()));
        RETURN_IF_EXCEPTION(scope, { });
        if (source)
            RELEASE_AND_RETURN(scope, asObject(source)->get(globalObject, name));
    }
    RELEASE_AND_RETURN(scope, m_capturedValues->get(globalObject, name));
}

static JSValue fixedProperty(VM& vm, const ModuleNamespace& object, PropertyName name)
{
    if (name == vm.propertyNames->toStringTagSymbol)
        return object.isDeferred() ? jsNontrivialString(vm, "Deferred Module"_s) : jsNontrivialString(vm, "Module"_s);
    return { };
}

bool ModuleNamespace::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    slot.setIsTaintedByOpaqueObject();
    if (slot.internalMethodType() != PropertySlot::InternalMethodType::VMInquiry && !thisObject->isSymbolLikeKey(name.uid())) {
        thisObject->evaluateIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    std::optional<unsigned> index = thisObject->find(name.uid());
    bool isDynamicExport = !index && thisObject->isDynamicExport(name.uid());
    if (!index && !isDynamicExport) {
        JSValue fixed = fixedProperty(vm, *thisObject, name);
        if (!fixed)
            return false;
        slot.setValue(thisObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete, fixed);
        return true;
    }
    switch (slot.internalMethodType()) {
    case PropertySlot::InternalMethodType::GetOwnProperty:
    case PropertySlot::InternalMethodType::Get: {
        if (isDynamicExport) {
            JSValue value = thisObject->readDynamicExport(globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
            return true;
        }
        JSValue value = thisObject->modules().readExport(thisObject->m_module, *index);
        RETURN_IF_EXCEPTION(scope, false);
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
        return true;
    }
    case PropertySlot::InternalMethodType::HasProperty:
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), jsUndefined());
        return true;
    case PropertySlot::InternalMethodType::VMInquiry:
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

bool ModuleNamespace::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    return getOwnPropertySlot(object, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

void ModuleNamespace::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    thisObject->evaluateIfNeeded(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    JSCellButterfly* ownNames = thisObject->ensureNames(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    JSCellButterfly* dynamicNames = thisObject->m_dynamicExportNames.get();
    unsigned ownNameCount = ownNames->length();
    unsigned dynamicNameCount = dynamicNames ? dynamicNames->length() : 0;
    for (unsigned ownIndex = 0, dynamicIndex = 0; ownIndex < ownNameCount || dynamicIndex < dynamicNameCount;) {
        bool isOwn = dynamicIndex == dynamicNameCount || (ownIndex < ownNameCount && codePointCompare(StringView(*asString(ownNames->get(ownIndex))->tryGetValueImpl()), StringView(*asString(dynamicNames->get(dynamicIndex))->tryGetValueImpl())) < 0);
        Identifier name = asString(isOwn ? ownNames->get(ownIndex++) : dynamicNames->get(dynamicIndex++))->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (thisObject->isSymbolLikeKey(name.impl()))
            continue;
        if (mode == DontEnumPropertiesMode::Exclude) {
            PropertySlot slot(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
            getOwnPropertySlot(thisObject, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, void());
        }
        names.add(name);
    }
    if (names.includeSymbolProperties() && mode == DontEnumPropertiesMode::Include)
        names.add(vm.propertyNames->toStringTagSymbol);
}

bool ModuleNamespace::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (name == vm.propertyNames->__esModule && !uncheckedDowncast<ModuleNamespace>(cell)->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, Base::put(cell, globalObject, name, value, slot));
    if (slot.isStrictMode())
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool ModuleNamespace::putByIndex(JSCell*, JSGlobalObject* globalObject, unsigned, JSValue, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (shouldThrow)
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool ModuleNamespace::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    VM& vm = globalObject->vm();
    if (!thisObject->isSymbolLikeKey(name.uid()) && !thisObject->evaluateIfNeeded(globalObject))
        return false;
    return !thisObject->hasExport(name.uid()) && !fixedProperty(vm, *thisObject, name);
}

bool ModuleNamespace::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Identifier name = Identifier::from(globalObject->vm(), index);
    if (!thisObject->evaluateIfNeeded(globalObject))
        return false;
    return !thisObject->hasExport(name.impl());
}

bool ModuleNamespace::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    if (name == vm.propertyNames->__esModule && descriptor.value().isTrue() && !thisObject->hasExport(name.uid())) {
        thisObject->setESModuleMarker(true);
        return true;
    }
    PropertyDescriptor current;
    bool isCurrentDefined = object->getOwnPropertyDescriptor(globalObject, name, current);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isCurrentDefined || !thisObject->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, validateAndApplyPropertyDescriptor(globalObject, nullptr, name, false, descriptor, isCurrentDefined, current, shouldThrow));

    // The steps and the messages of JSModuleNamespaceObject::defineOwnProperty.
    auto reject = [&](ASCIILiteral message) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, message);
        return false;
    };
    if (descriptor.isAccessorDescriptor())
        return reject("Cannot change module namespace object's binding to accessor"_s);
    if (descriptor.writablePresent() && !descriptor.writable())
        return reject("Cannot change module namespace object's binding to non-writable attribute"_s);
    if (descriptor.enumerablePresent() && !descriptor.enumerable())
        return reject("Cannot replace module namespace object's binding with non-enumerable attribute"_s);
    if (descriptor.configurablePresent() && descriptor.configurable())
        return reject("Cannot replace module namespace object's binding with configurable attribute"_s);
    if (descriptor.value()) {
        bool isSame = sameValue(globalObject, descriptor.value(), current.value());
        RETURN_IF_EXCEPTION(scope, false);
        if (!isSame)
            return reject("Cannot replace module namespace object's binding's value"_s);
    }
    return true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
