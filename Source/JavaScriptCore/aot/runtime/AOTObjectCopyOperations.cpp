/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModuleNamespace.h"
#include "AOTObject.h"
#include "AOTObjectCopy.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCopy.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "ButterflyInlines.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using ExcludedNames = Vector<UniquedStringImpl*, 8>;

static Structure* emptyLiteralStructureForCopy(JSGlobalObject* globalObject, VM& vm, unsigned inlineCapacity)
{
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    return profile.structure();
}

static unsigned inlineCapacityForSizeClass(size_t propertyCount)
{
    size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(propertyCount));
    return std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
}

static void defineCopiedProperty(JSGlobalObject* globalObject, JSObject* target, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    Structure* structure = target->structure();
    unsigned attributes = 0;
    if (target->type() == FinalObjectType && structure->isStructureExtensible() && !parseIndex(name) && (structure->get(vm, name, attributes) == invalidOffset || !attributes))
        target->putDirect(vm, name, value);
    else
        target->createDataProperty(globalObject, name, value, true);
}

JSObject* withAllPropertiesInline(JSGlobalObject* globalObject, JSObject* object)
{
    if (!Instance::of(globalObject))
        return object;
    Structure* structure = object->structure();
    if (!structure->outOfLineSize() || !structure->canPerformFastPropertyEnumerationCommon() || !checkStructureForClone(structure, false))
        return object;
    if (structure->realm() != globalObject || structure->storedPrototype() != JSValue(globalObject->objectPrototype()))
        return object;
    VM& vm = globalObject->vm();
    Vector<UniquedStringImpl*, 16> names;
    MarkedArgumentBuffer values;
    bool fitsInline = true;
    EnsureStillAliveScope structureScope(structure);
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        fitsInline = !entry.attributes() && names.size() < JSFinalObject::maxInlineCapacity;
        if (!fitsInline)
            return false;
        names.append(entry.key());
        values.appendWithCrashOnOverflow(object->getDirect(entry.offset()));
        return true;
    });
    if (!fitsInline)
        return object;
    JSFinalObject* copy = JSFinalObject::create(vm, emptyLiteralStructureForCopy(globalObject, vm, inlineCapacityForSizeClass(names.size())));
    copy->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
    return copy;
}

static bool tryCollectProperties(VM& vm, JSObject* source, const ExcludedNames& excluded, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    if (isCompiledObject(source)) {
        auto walkResult = PropertyWalk::forEachOwnPropertyInline(vm, source, PropertyWalk::Filter::StringsAndSymbols, [&](const PropertyWalk::OwnProperty& property) {
            if ((property.attributes & PropertyAttribute::DontEnum) || excluded.contains(property.name))
                return IterationStatus::Continue;
            if (property.name == vm.propertyNames->underscoreProto.impl())
                return IterationStatus::Done;
            names.append(property.name);
            values.appendWithCrashOnOverflow(property.value);
            return IterationStatus::Continue;
        });
        return walkResult == PropertyWalk::Result::Completed;
    }
    Structure* structure = source->structure();
    if (source->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumeration())
        return false;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if ((entry.attributes() & PropertyAttribute::DontEnum) || PropertyName(entry.key()).isPrivateName() || excluded.contains(entry.key()))
            return true;
        names.append(entry.key());
        values.appendWithCrashOnOverflow(source->getDirect(entry.offset()));
        return true;
    });
    return true;
}

static bool tryCopyPropertiesExcluding(VM& vm, JSObject* target, JSObject* source, const ExcludedNames& excluded)
{
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    EnsureStillAliveScope sourceScope(source);
    if (!tryCollectProperties(vm, source, excluded, names, values))
        return false;
    count(vm, RuntimeCounterID::CopiesCollectedByName);
    if (!isCompiledObject(target)) {
        if (names.size())
            target->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
        return true;
    }
    if (uncheckedDowncast<Object>(target)->tryDefineFields(vm, names.span(), { values.data(), values.size() }))
        return true;
    for (size_t i = 0; i < names.size(); ++i) {
        if (isCompiledObject(target))
            PropertyWalk::putDirect(vm, target, names[i], values.at(i), 0);
        else
            target->putDirect(vm, names[i], values.at(i));
    }
    return true;
}

// Of the primitives only a string has own properties.
static ALWAYS_INLINE bool hasOwnPropertiesToCopy(JSValue source)
{
    return source.isObject() || source.isString();
}

static void copyDataPropertiesExcluding(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* target, JSValue source, const ExcludedNames& excluded)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasOwnPropertiesToCopy(source))
        return;
    JSObject* from = source.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType && tryCopyDataProperties(instance, vm, target, from, TargetPrototypes::Ignored))
        return;
    if (excluded.isEmpty() && target->type() == FinalObjectType && tryCopyRecordDataProperties(instance, vm, target, from))
        return;
    auto* ordinary = dynamicDowncast<JSFinalObject>(target);
    bool canPutDirectly = ordinary && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible();
    if (canPutDirectly && excluded.isEmpty()) {
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer propertyValues;
        bool isDone = objectAssignFast(globalObject, ordinary, from, properties, propertyValues);
        RETURN_IF_EXCEPTION(scope, void());
        if (isDone)
            return;
    }
    Object* record = compiledObjectOrNull(target);
    if (record) {
        if (Object* other = compiledObjectOrNull(from); other && excluded.isEmpty() && record->tryCopyFieldsOf(vm, *other))
            return;
        if (record->tryAssignPropertiesOf(vm, from, excluded.span(), Object::PlanCreation::AtSecondCopy))
            return;
    }
    bool canDefineDirectly = record ? !(record->orderWord() & orderWordIntegrityBits) : canPutDirectly && !excluded.isEmpty();
    if (canDefineDirectly && tryCopyPropertiesExcluding(vm, target, from, excluded))
        return;
    // Nothing sees the target before the copy is complete, so what the source runs cannot tell when the properties are defined.
    Vector<UniquedStringImpl*, 8> copiedNames;
    MarkedArgumentBuffer copiedValues;
    auto defineCopiedProperties = [&] {
        if (record && record->tryDefineFields(vm, copiedNames.span(), { copiedValues.data(), copiedValues.size() }))
            return;
        for (size_t i = 0; i < copiedNames.size(); ++i) {
            defineCopiedProperty(globalObject, target, copiedNames[i], copiedValues.at(i));
            RETURN_IF_EXCEPTION(scope, void());
        }
    };
    if (auto* moduleNamespace = excluded.isEmpty() ? dynamicDowncast<ModuleNamespace>(from) : nullptr) {
        bool didReadExports = moduleNamespace->tryReadExports(globalObject, copiedNames, copiedValues);
        RETURN_IF_EXCEPTION(scope, void());
        if (didReadExports)
            RELEASE_AND_RETURN(scope, defineCopiedProperties());
    }
    count(vm, RuntimeCounterID::CopiesByOrdinaryLookups);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    from->methodTable()->getOwnPropertyNames(from, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& name : names) {
        if (excluded.contains(name.impl()))
            continue;
        PropertySlot slot(from, PropertySlot::InternalMethodType::GetOwnProperty);
        bool has = from->methodTable()->getOwnPropertySlot(from, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (!has || slot.attributes() & PropertyAttribute::DontEnum)
            continue;
        bool needsReread = slot.isTaintedByOpaqueObject() && !isModuleNamespace(from);
        JSValue value = needsReread ? from->get(globalObject, name) : slot.getValue(globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        copiedNames.append(name.impl());
        copiedValues.appendWithCrashOnOverflow(value);
    }
    RELEASE_AND_RETURN(scope, defineCopiedProperties());
}

void copyDataProperties(Instance* instance, JSGlobalObject* globalObject, JSObject* target, JSValue source)
{
    copyDataPropertiesExcluding(instance, globalObject, globalObject->vm(), target, source, { });
}

static ExcludedRun excludedRunAt(Instance* instance, uint32_t numberPlusOne)
{
    if (!numberPlusOne)
        return { };
    return { numberPlusOne, instance->tables().propertyRunRow(numberPlusOne - 1).subspan(1) };
}

namespace {

// A key among the operands is a string or a symbol. A number stands for the identifier with that number.
class ExcludedKeys {
public:
    bool tryCollect(Instance* instance, JSGlobalObject* globalObject, ExcludedRun run, std::span<const EncodedJSValue> keys)
    {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        for (uint32_t identifier : run.identifiers)
            m_names.append(identifierAt(instance, identifier).impl());
        for (EncodedJSValue encodedKey : keys) {
            JSValue key = JSValue::decode(encodedKey);
            if (key.isInt32()) {
                m_names.append(identifierAt(instance, key.asInt32()).impl());
                continue;
            }
            auto identifier = key.toPropertyKey(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            m_names.append(identifier.impl());
            m_identifiers.append(WTF::move(identifier));
        }
        return true;
    }

    const ExcludedNames& names() const { return m_names; }

private:
    ExcludedNames m_names;
    Vector<Identifier, 8> m_identifiers;
};

}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyDataProperties, void, (Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyDataProperties);
    if (!hasOwnPropertiesToCopy(JSValue::decode(source)))
        OPERATION_RETURN(scope);
    JSObject* object = cellArgument<JSObject>(target, Entry::operationAOTCopyDataProperties);
    ExcludedKeys excluded;
    bool didCollect = excluded.tryCollect(instance, globalObject, excludedRunAt(instance, excludedRun), { excludedKeys, count });
    OPERATION_RETURN_IF_EXCEPTION(scope);
    ASSERT_UNUSED(didCollect, didCollect);
    scope.release();
    copyDataPropertiesExcluding(instance, globalObject, vm, object, JSValue::decode(source), excluded.names());
    OPERATION_RETURN(scope);
}

static JSObject* tryCopyWithoutNames(VM& vm, JSGlobalObject* globalObject, JSObject* source, ExcludedRun excluded)
{
    bool canHaveStructureOfSource = !excluded.numberPlusOne && source->type() == FinalObjectType && source->structure()->canPerformFastPropertyEnumerationCommon();
    if (JSObject* copy = canHaveStructureOfSource ? tryCreateObjectViaCloning(vm, globalObject, source) : nullptr)
        return copy;
    return tryCopyAsOrdinaryObject(vm, globalObject, source, excluded);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyObject, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyObject);
    JSValue source = JSValue::decode(encodedSource);
    ExcludedRun run = excludedRunAt(instance, excludedRun);
    if (!count && source.isObject()) {
        if (JSObject* copy = tryCopyWithoutNames(vm, globalObject, asObject(source), run))
            OPERATION_RETURN(scope, copy);
    }
    JSObject* copy = constructEmptyObject(globalObject);
    if (!hasOwnPropertiesToCopy(source))
        OPERATION_RETURN(scope, copy);
    ExcludedKeys excluded;
    bool didCollect = excluded.tryCollect(instance, globalObject, run, { excludedKeys, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(didCollect, didCollect);
    copyDataPropertiesExcluding(instance, globalObject, vm, copy, source, excluded.names());
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCloneObjectAs, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t requestOffset, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCloneObjectAs);
    const CloneAsRequest& request = requestAt<CloneAsRequest>(instance, requestOffset);
    const LayoutData& layout = instance->state->objects->layouts.layoutForShape(request.shape);
    JSObject* source = objectArgument(encodedSource, Entry::operationAOTCloneObjectAs);
    Vector<EncodedJSValue, 16> replacedFields(FillWith { }, layout.numberOfFields, JSValue::encode(JSValue()));
    for (uint32_t i = 0; i < request.count; ++i) {
        if (!JSValue::decode(values[i]).isUndefined())
            replacedFields[request.fields[i]] = values[i];
    }
    if (Object* copy = tryCloneAs(vm, source, layout, replacedFields.span()))
        OPERATION_RETURN(scope, copy);
    count(*instance, RuntimeCounterID::ValidationsFellBackPerValue);
    JSObject* copy = constructEmptyObject(globalObject);
    copyDataProperties(instance, globalObject, copy, source);
    scope.assertNoException();
    for (uint32_t i = 0; i < request.count; ++i) {
        JSValue value = JSValue::decode(values[i]);
        if (value.isUndefined())
            continue;
        defineCopiedProperty(globalObject, copy, identifierAt(instance, layout.fields()[request.fields[i]].name), value);
        scope.assertNoException();
    }
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsPlainDataObject, size_t, (Instance*, EncodedJSValue value))
{
    return isPlainDataObject(JSValue::decode(value));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
