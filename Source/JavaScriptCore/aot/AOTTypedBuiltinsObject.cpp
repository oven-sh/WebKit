/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "AOTOperationsObjects.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "B3StackmapGenerationParams.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "ObjectPrototype.h"
#include "ReflectObject.h"
#include "StructureRareData.h"

namespace JSC { namespace AOT {

enum class OwnKeys : uint32_t { ObjectKeys, ObjectGetOwnPropertyNames, ObjectGetOwnPropertySymbols, ReflectOwnKeys };

enum class FunctionInEngine : uint32_t { Keys, GetOwnPropertyNames, GetOwnPropertySymbols, OwnKeys, Values, GetOwnPropertyDescriptor, Entries, GetOwnPropertyDescriptors, DefineProperties };
static_assert(static_cast<uint32_t>(FunctionInEngine::Keys) == static_cast<uint32_t>(OwnKeys::ObjectKeys) && static_cast<uint32_t>(FunctionInEngine::GetOwnPropertyNames) == static_cast<uint32_t>(OwnKeys::ObjectGetOwnPropertyNames));
static_assert(static_cast<uint32_t>(FunctionInEngine::GetOwnPropertySymbols) == static_cast<uint32_t>(OwnKeys::ObjectGetOwnPropertySymbols) && static_cast<uint32_t>(FunctionInEngine::OwnKeys) == static_cast<uint32_t>(OwnKeys::ReflectOwnKeys));

static NEVER_INLINE JSValue callInEngine(Instance* instance, FunctionInEngine which, JSValue first, JSValue second = JSValue())
{
    static constexpr unsigned count = static_cast<unsigned>(FunctionInEngine::DefineProperties) + 1;
    static constexpr std::array<std::array<ASCIILiteral, 2>, count> names { {
        { "Object"_s, "keys"_s }, { "Object"_s, "getOwnPropertyNames"_s }, { "Object"_s, "getOwnPropertySymbols"_s }, { "Reflect"_s, "ownKeys"_s }, { "Object"_s, "values"_s }, { "Object"_s, "getOwnPropertyDescriptor"_s }, { "Object"_s, "entries"_s }, { "Object"_s, "getOwnPropertyDescriptors"_s }, { "Object"_s, "defineProperties"_s },
    } };
    static const std::array<std::array<unsigned, 2>, count> cells = [] {
        auto& builtins = Typed::Builtins::singleton();
        std::array<std::array<unsigned, 2>, count> result;
        for (unsigned i = 0; i < count; ++i)
            result[i] = { builtins.globalCell(names[i][0]), builtins.staticFunctionCell(names[i][0], names[i][1]) };
        return result;
    }();
    unsigned index = static_cast<unsigned>(which);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(*instance->vm).countOperation("called in the engine", names[index][1].characters());
    MarkedArgumentBuffer list;
    list.append(first);
    if (second)
        list.append(second);
    return callEngineBuiltin(instance, cells[index][1], typedBuiltinCell(instance, cells[index][0]), list);
}

static void cacheKeyList(Instance* instance, VM& vm, JSObject* object)
{
    if (!object->structure()->compiledLayout())
        return;
    auto* ofProgram = uncheckedDowncast<Typed::Object>(object);
    if (&ofProgram->state() != instance->typed)
        return;
    if (JSCellButterfly* list = ofProgram->keys(vm))
        instance->typedKeyLists[Instance::indexOfTypedKeyList(Typed::shapeWordOf(ofProgram->shape()), ofProgram->orderWord())] = { ofProgram->orderWord(), Typed::shapeWordOf(ofProgram->shape()), list };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedOwnPropertyKeys, JSCell*, (Instance* instance, EncodedJSValue encodedValue, uint32_t which))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    OwnKeys keys = static_cast<OwnKeys>(which);
    JSObject* object;
    if (value.isObject()) [[likely]]
        object = asObject(value);
    else {
        if (keys == OwnKeys::ReflectOwnKeys || value.isUndefinedOrNull()) {
            JSValue list = callInEngine(instance, static_cast<FunctionInEngine>(which), value);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            OPERATION_RETURN(scope, list.asCell());
        }
        object = value.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    if (keys != OwnKeys::ObjectGetOwnPropertySymbols)
        cacheKeyList(instance, vm, object);
    switch (keys) {
    case OwnKeys::ObjectKeys:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Exclude));
    case OwnKeys::ObjectGetOwnPropertyNames:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Include));
    case OwnKeys::ObjectGetOwnPropertySymbols:
        if (const Typed::LayoutData* layout = object->structure()->compiledLayout(); layout && Typed::isObjectLayout(layout->kind) && !uncheckedDowncast<Typed::Object>(object)->extraProperties())
            OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr));
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Symbols, DontEnumPropertiesMode::Include));
    case OwnKeys::ReflectOwnKeys:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::StringsAndSymbols, DontEnumPropertiesMode::Include));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSArray* tryListCompiledObjectValues(JSGlobalObject*, VM&, JSObject*);

JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectValues, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        JSValue list = callInEngine(instance, FunctionInEngine::Values, value);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        OPERATION_RETURN(scope, list.asCell());
    }
    if (value.isObject() && asObject(value)->structure()->compiledLayout() && !globalObject->isHavingABadTime()) {
        if (JSArray* list = tryListCompiledObjectValues(globalObject, vm, asObject(value)))
            OPERATION_RETURN(scope, list);
    }
    JSValue values = objectValues(vm, globalObject, value);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, values.asCell());
}

struct CompiledObjectKeysAndFields {
    Typed::Object* object { nullptr };
    JSCellButterfly* names { nullptr };
    std::span<const Typed::State::FieldAsProperty> fields;
    ALWAYS_INLINE JSValue valueAt(unsigned index) const
    {
        const Typed::FieldData& field = *fields.data()[index].field;
        JSValue value = object->readInline(field);
        return value ? value : object->read(field);
    }
};

static std::optional<CompiledObjectKeysAndFields> compiledObjectKeysAndFields(VM& vm, JSObject* target)
{
    const Typed::LayoutData* layout = target->structure()->compiledLayout();
    if (!layout || !Typed::isObjectLayout(layout->kind))
        return std::nullopt;
    auto* object = uncheckedDowncast<Typed::Object>(target);
    JSCellButterfly* names = object->keys(vm);
    if (!names)
        return std::nullopt;
    auto fields = object->state().propertiesInOrder(vm, object->layout(), object->orderWord() & Typed::orderAndCountMask);
    if (!fields || fields->size() != names->length())
        return std::nullopt;
    return CompiledObjectKeysAndFields { object, names, *fields };
}

static NEVER_INLINE JSArray* tryListCompiledObjectValues(JSGlobalObject* globalObject, VM& vm, JSObject* target)
{
    auto compiled = compiledObjectKeysAndFields(vm, target);
    if (!compiled)
        return nullptr;
    Structure* arrayStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (!hasContiguous(arrayStructure->indexingType()))
        return nullptr;
    unsigned count = compiled->names->length();
    JSArray* list = JSArray::tryCreate(vm, arrayStructure, count);
    if (!list)
        return nullptr;
    auto elements = list->butterfly()->contiguous();
    for (unsigned i = 0; i < count; ++i) {
        JSValue value = compiled->valueAt(i);
        if (!value)
            return nullptr;
        elements.atUnsafe(i).setWithoutWriteBarrier(value);
    }
    ensureStillAliveHere(target);
    return list;
}

static NEVER_INLINE JSArray* tryCreateCompiledObjectEntries(JSGlobalObject* globalObject, VM& vm, JSObject* target)
{
    auto compiled = compiledObjectKeysAndFields(vm, target);
    if (!compiled)
        return nullptr;
    Structure* arrayStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (!hasContiguous(arrayStructure->indexingType()) || arrayStructure->outOfLineCapacity())
        return nullptr;
    unsigned count = compiled->names->length();
    JSArray* entries = JSArray::tryCreate(vm, arrayStructure, count);
    if (!entries)
        return nullptr;
    constexpr unsigned vectorLength = arrayLiteralVectorLength(2);
    for (unsigned i = 0; i < count; ++i) {
        void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
        JSValue value = compiled->valueAt(i);
        if (!storage || !value)
            return nullptr;
        Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(2);
        butterfly->contiguous().atUnsafe(0).setWithoutWriteBarrier(compiled->names->get(i));
        butterfly->contiguous().atUnsafe(1).setWithoutWriteBarrier(value);
        for (unsigned k = 2; k < vectorLength; ++k)
            butterfly->contiguous().atUnsafe(k).clear();
        entries->butterfly()->contiguous().at(entries, i).set(vm, entries, JSArray::createWithButterfly(vm, nullptr, arrayStructure, butterfly));
    }
    ensureStillAliveHere(compiled->names);
    ensureStillAliveHere(target);
    return entries;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedObjectEntries, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isObject() || globalObject->isHavingABadTime())
        return nullptr;
    JSObject* target = asObject(value);
    Structure* structure = target->structure();
    if (structure->compiledLayout())
        return tryCreateCompiledObjectEntries(globalObject, vm, target);
    if (!target->staticPropertiesReified() || target->canHaveExistingOwnIndexedProperties() || target->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumerationCommon())
        return nullptr;
    JSCellButterfly* names = structure->cachedPropertyNames(CachedPropertyNamesKind::EnumerableStrings);
    if (!names)
        return nullptr;
    Vector<std::pair<UniquedStringImpl*, PropertyOffset>, 16> properties;
    unsigned count = names->length();
    unsigned inlineCapacity = structure->inlineCapacity();
    bool isInOrder = structure->canAccessPropertiesQuicklyForEnumeration() && count == static_cast<unsigned>(numberOfSlotsForMaxOffset(structure->maxOffset(), inlineCapacity));
    if (!isInOrder) {
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
            if (!(entry.attributes() & PropertyAttribute::DontEnum) && !entry.key()->isSymbol())
                properties.append({ entry.key(), entry.offset() });
            return true;
        });
        if (properties.size() != count)
            return nullptr;
        for (unsigned i = 0; i < count; ++i) {
            if (asString(names->get(i))->tryGetValueImpl() != properties[i].first)
                return nullptr;
        }
    }
    Structure* arrayStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (!hasContiguous(arrayStructure->indexingType()) || arrayStructure->outOfLineCapacity())
        return nullptr;
    JSArray* entries = JSArray::tryCreate(vm, arrayStructure, count);
    if (!entries)
        return nullptr;
    constexpr unsigned vectorLength = arrayLiteralVectorLength(2);
    for (unsigned i = 0; i < count; ++i) {
        void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
        if (!storage)
            return nullptr;
        Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(2);
        butterfly->contiguous().atUnsafe(0).setWithoutWriteBarrier(names->get(i));
        butterfly->contiguous().atUnsafe(1).setWithoutWriteBarrier(target->getDirect(isInOrder ? offsetForPropertyNumber(i, inlineCapacity) : properties[i].second));
        for (unsigned k = 2; k < vectorLength; ++k)
            butterfly->contiguous().atUnsafe(k).clear();
        entries->butterfly()->contiguous().at(entries, i).set(vm, entries, JSArray::createWithButterfly(vm, nullptr, arrayStructure, butterfly));
    }
    ensureStillAliveHere(names);
    return entries;
}

static NEVER_INLINE JSArray* tryListCompiledObjectEntries(JSGlobalObject* globalObject, VM& vm, JSObject* target)
{
    auto compiled = compiledObjectKeysAndFields(vm, target);
    if (!compiled)
        return nullptr;
    Structure* arrayStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (!hasContiguous(arrayStructure->indexingType()))
        return nullptr;
    unsigned count = compiled->names->length();
    JSArray* list = JSArray::tryCreate(vm, arrayStructure, 2 * count);
    if (!list)
        return nullptr;
    auto elements = list->butterfly()->contiguous();
    for (unsigned i = 0; i < count; ++i) {
        JSValue value = compiled->valueAt(i);
        if (!value)
            return nullptr;
        elements.atUnsafe(2 * i).setWithoutWriteBarrier(compiled->names->get(i));
        elements.atUnsafe(2 * i + 1).setWithoutWriteBarrier(value);
    }
    ensureStillAliveHere(compiled->names);
    ensureStillAliveHere(target);
    return list;
}

static JSArray* tryListOrdinaryObjectEntries(JSGlobalObject* globalObject, VM& vm, JSValue value)
{
    if (!value.isObject() || globalObject->isHavingABadTime())
        return nullptr;
    JSObject* target = asObject(value);
    Structure* structure = target->structure();
    if (structure->compiledLayout())
        return tryListCompiledObjectEntries(globalObject, vm, target);
    if (!target->staticPropertiesReified() || target->canHaveExistingOwnIndexedProperties() || target->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumerationCommon())
        return nullptr;
    JSCellButterfly* names = structure->cachedPropertyNames(CachedPropertyNamesKind::EnumerableStrings);
    if (!names)
        return nullptr;
    Vector<PropertyOffset, 16> offsets;
    unsigned count = names->length();
    unsigned inlineCapacity = structure->inlineCapacity();
    bool isInIndexOrder = structure->canAccessPropertiesQuicklyForEnumeration() && count == static_cast<unsigned>(numberOfSlotsForMaxOffset(structure->maxOffset(), inlineCapacity));
    if (!isInIndexOrder) {
        bool isInNameOrder = true;
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
            if ((entry.attributes() & PropertyAttribute::DontEnum) || entry.key()->isSymbol())
                return true;
            isInNameOrder = offsets.size() < count && asString(names->get(offsets.size()))->tryGetValueImpl() == entry.key();
            offsets.append(entry.offset());
            return isInNameOrder;
        });
        if (!isInNameOrder || offsets.size() != count)
            return nullptr;
    }
    Structure* arrayStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (!hasContiguous(arrayStructure->indexingType()))
        return nullptr;
    JSArray* list = JSArray::tryCreate(vm, arrayStructure, 2 * count);
    if (!list)
        return nullptr;
    auto elements = list->butterfly()->contiguous();
    for (unsigned i = 0; i < count; ++i) {
        elements.atUnsafe(2 * i).setWithoutWriteBarrier(names->get(i));
        elements.atUnsafe(2 * i + 1).setWithoutWriteBarrier(target->getDirect(isInIndexOrder ? offsetForPropertyNumber(i, inlineCapacity) : offsets[i]));
    }
    ensureStillAliveHere(names);
    return list;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectEntriesFlat, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (JSArray* list = tryListOrdinaryObjectEntries(globalObject, vm, value))
        OPERATION_RETURN(scope, list);
    JSValue entries = callInEngine(instance, FunctionInEngine::Entries, value);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* pairs = asArray(entries);
    unsigned count = pairs->length();
    MarkedArgumentBuffer elements;
    for (unsigned i = 0; i < count; ++i) {
        JSArray* pair = asArray(pairs->getIndexQuickly(i));
        elements.append(pair->getIndexQuickly(0));
        elements.append(pair->getIndexQuickly(1));
    }
    if (elements.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    JSArray* list = constructArray(globalObject, static_cast<ArrayAllocationProfile*>(nullptr), elements);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, list);
}

static bool isOrdinaryForCopyingDescriptors(JSObject* object)
{
    Structure* structure = object->structure();
    return object->type() == FinalObjectType && !structure->compiledLayout() && !structure->hasImmutableProperties() && !structure->hasImmutableShape() && !structure->typeInfo().wouldHaveFixedProperties();
}

static bool tryCopyOwnPropertyDescriptors(Instance* instance, JSGlobalObject* globalObject, JSObject* target, JSObject* source)
{
    VM& vm = globalObject->vm();
    if (target == source || !isOrdinaryForCopyingDescriptors(target) || !isOrdinaryForCopyingDescriptors(source) || hasIndexedProperties(source->indexingType()))
        return false;
    if (!target->structure()->isStructureExtensible() || target->structure()->inheritorsMayOverrideReadOnlyProperties())
        return false;
    if (globalObject->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        globalObject->tryInstallPropertyDescriptorFastPathWatchpoint();
    if (!globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid() || !globalObject->objectPrototypeChainIsSane())
        return false;
    struct Property {
        UniquedStringImpl* name;
        unsigned attributes;
        bool isNew { true };
    };
    Vector<Property, 32> properties;
    MarkedArgumentBuffer values;
    constexpr unsigned plainAttributes = PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::Accessor;
    bool hasOnlyPlainAttributes = true;
    bool hasSymbol = false;
    auto collect = [&](bool symbols) {
        source->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (entry.key()->isSymbol() != symbols) {
                hasSymbol |= entry.key()->isSymbol();
                return true;
            }
            if (PropertyName(entry.key()).isPrivateName())
                return true;
            hasOnlyPlainAttributes &= !(entry.attributes() & ~plainAttributes);
            properties.append({ entry.key(), entry.attributes() });
            values.appendWithCrashOnOverflow(source->getDirect(entry.offset()));
            return true;
        });
    };
    collect(false);
    if (hasSymbol)
        collect(true);
    if (!hasOnlyPlainAttributes)
        return false;
    Structure* targetStructure = target->structure();
    if (isValidOffset(targetStructure->maxOffset())) {
        for (auto& property : properties) {
            unsigned attributes;
            property.isNew = !isValidOffset(targetStructure->get(vm, property.name, attributes));
            if (!property.isNew && attributes & (PropertyAttribute::DontDelete | PropertyAttribute::CustomAccessorOrValue | PropertyAttribute::WouldBeFixed))
                return false;
        }
    }
    bool canAddRuns = globalObject == instance->globalObject && !targetStructure->mayBePrototype() && !targetStructure->hasPolyProto() && !source->structure()->isDictionary() && properties.size() <= 64;
    if (canAddRuns)
        makePrototypeChainWatchable(vm, target);
    for (size_t i = 0; i < properties.size(); ++i) {
        Structure* before = target->structure();
        Vector<UniquedStringImpl*, 32> run;
        for (size_t k = i; canAddRuns && k < properties.size() && properties[k].isNew && !properties[k].attributes; ++k)
            run.append(properties[k].name);
        bool mayAddRun = run.size() > 1 && !before->isDictionary() && before->transitionCountEstimate() <= Structure::s_maxTransitionLength;
        if (mayAddRun && !instance->hasPropertyRunTarget(before, run.span()))
            mayAddRun = instance->reservePropertyRunTarget(Instance::PropertyRunBudget::CopiedNames);
        Structure* newStructure = mayAddRun ? structureAfterPropertyRun(instance, vm, before, run.span()) : nullptr;
        if (newStructure) {
            if (before->outOfLineCapacity() != newStructure->outOfLineCapacity())
                target->nukeStructureAndSetButterfly(vm, before->id(), target->allocateMoreOutOfLineStorage(vm, before->outOfLineCapacity(), newStructure->outOfLineCapacity()));
            PropertyOffset offset = before->maxOffset();
            for (size_t k = 0; k < run.size(); ++k) {
                offset = Instance::offsetAfter(offset, before->inlineCapacity());
                target->putDirectOffset(vm, offset, values.at(i + k));
            }
            target->setStructure(vm, newStructure);
            i += run.size() - 1;
            continue;
        }
        canAddRuns &= run.size() < 2;
        auto [name, attributes, isNew] = properties[i];
        if (!(attributes & PropertyAttribute::Accessor)) {
            target->putDirect(vm, name, values.at(i), attributes);
            continue;
        }
        auto* accessor = uncheckedDowncast<GetterSetter>(values.at(i));
        target->putDirectNonIndexAccessor(vm, name, GetterSetter::create(vm, globalObject, accessor->isGetterNull() ? nullptr : accessor->getter(), accessor->isSetterNull() ? nullptr : accessor->setter()), attributes);
    }
    ensureStillAliveHere(source);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedOwnPropertyDescriptorsUnlessCopied, EncodedJSValue, (Instance* instance, EncodedJSValue encodedSource))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue source = JSValue::decode(encodedSource);
    if (source.isObject() && isOrdinaryForCopyingDescriptors(asObject(source)))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(callInEngine(instance, FunctionInEngine::GetOwnPropertyDescriptors, source)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCopyOwnPropertyDescriptors, void, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedSource, EncodedJSValue encodedDescriptors))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue target = JSValue::decode(encodedTarget);
    JSValue source = JSValue::decode(encodedSource);
    JSValue descriptors = JSValue::decode(encodedDescriptors);
    if (descriptors.isUndefined()) {
        if (target.isObject() && tryCopyOwnPropertyDescriptors(instance, globalObject, asObject(target), asObject(source)))
            OPERATION_RETURN(scope);
        descriptors = callInEngine(instance, FunctionInEngine::GetOwnPropertyDescriptors, source);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    callInEngine(instance, FunctionInEngine::DefineProperties, target, descriptors);
    OPERATION_RETURN(scope);
}

static bool mayHaveIndexedSetterOrReadOnly(JSObject* target)
{
    return (hasAnyArrayStorage(target->indexingType()) && target->butterfly()->arrayStorage()->m_sparseMap) || target->structure()->holesMustForwardToPrototype(target);
}

static void assignFrom(Instance* instance, JSGlobalObject* globalObject, JSObject* target, JSValue sourceValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (sourceValue.isUndefinedOrNull())
        return;
    JSObject* source = sourceValue.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto* targetObject = dynamicDowncast<JSFinalObject>(target); targetObject && targetObject->canPerformFastPutInlineExcludingProto() && targetObject->isStructureExtensible() && !(source->canHaveExistingOwnIndexedProperties() && mayHaveIndexedSetterOrReadOnly(targetObject))) {
        if (!source->staticPropertiesReified()) {
            source->reifyAllStaticProperties(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (Structure* structure = targetObject->structure(); !structure->mayBePrototype() && !structure->isWatchingReplacement() && (source->type() == FinalObjectType ? tryCopyDataProperties(instance, vm, targetObject, source, nullptr, "operationAOTTypedObjectAssignFrom") : tryCopyRecordDataProperties(instance, vm, targetObject, source, "operationAOTTypedObjectAssignFrom")))
            return;
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer values;
        bool isDone = objectAssignFast(globalObject, targetObject, source, properties, values);
        RETURN_IF_EXCEPTION(scope, void());
        if (isDone)
            return;
    }
    RELEASE_AND_RETURN(scope, objectAssignGeneric(globalObject, vm, target, source));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectAssignFrom, void, (Instance* instance, JSCell* target, EncodedJSValue source))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    scope.release();
    assignFrom(instance, globalObject, asObject(target), JSValue::decode(source));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectAssignToNew, JSCell*, (Instance* instance, EncodedJSValue encodedSource))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue sourceValue = JSValue::decode(encodedSource);
    if (sourceValue.isObject()) {
        JSObject* source = asObject(sourceValue);
        Structure* structure = source->structure();
        if (!source->hasNonReifiedStaticProperties() && structure->canPerformFastPropertyEnumerationCommon()) {
            if (JSObject* copy = tryCreateObjectViaCloning(vm, globalObject, source))
                OPERATION_RETURN(scope, copy);
        } else if (const Typed::LayoutData* layout = structure->compiledLayout(); layout && Typed::isObjectLayout(layout->kind)) {
            if (JSObject* copy = uncheckedDowncast<Typed::Object>(source)->tryCopyAsPlainObject(globalObject))
                OPERATION_RETURN(scope, copy);
        }
    }
    JSFinalObject* target = constructEmptyObject(globalObject);
    assignFrom(instance, globalObject, target, sourceValue);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, target);
}

static bool appliesToAllObjectsWithStructure(const PropertySlot& slot, JSObject* object, PropertyName name)
{
    if (parseIndex(name) || (!slot.isCacheable() && !slot.isUnset()) || object->type() == GlobalProxyType)
        return false;
    Structure* structure = object->structure();
    return !structure->typeInfo().prohibitsPropertyCaching() && structure->propertyAccessesAreCacheable() && (!slot.isUnset() || (structure->propertyAccessesAreCacheableForAbsence() && !structure->isDictionary()));
}

static std::optional<bool> cacheCompiledObjectLocation(Instance* instance, JSObject* object, PropertyName name)
{
    using Place = Instance::OwnPropertyLocation;
    using Typed::Store;
    Structure* structure = object->structure();
    const Typed::LayoutData* layout = structure->compiledLayout();
    if (!layout || !Typed::isObjectLayout(layout->kind) || parseIndex(name) || name.isPrivateName())
        return std::nullopt;
    auto* compiled = uncheckedDowncast<Typed::Object>(object);
    const Typed::FieldData* field = name.isSymbol() ? nullptr : compiled->fieldNamed(compiled->state(), name);
    if (!field) {
        if (!Typed::Object::structureGuaranteesAbsence(structure, name.uid()))
            return std::nullopt;
        instance->cacheOwnPropertyLocation(structure->id().bits(), name.uid(), 0);
        return false;
    }
    if (field->flags & Typed::fieldIsOutOfLine)
        return std::nullopt;
    unsigned width = 0;
    unsigned marker = 0;
    switch (field->store()) {
    case Store::JSValue:
    case Store::Cell:
    case Store::LiteralString:
        width = 8;
        break;
    case Store::Cell32:
    case Store::OptionalCell32:
    case Store::LiteralString32:
        static_assert(!Typed::absentCell32);
        width = 4;
        break;
    case Store::OptionalBoolean:
        width = 1;
        break;
    case Store::Int32OrNumberBox:
        width = 4;
        marker = 1;
        break;
    case Store::OptionalDouble:
        width = 8;
        marker = 2;
        break;
    case Store::OptionalInt64:
        width = 8;
        marker = 3;
        break;
    default:
        instance->cacheOwnPropertyLocation(structure->id().bits(), name.uid(), Place::isPresent | Place::isNotValue);
        return true;
    }
    if (field->offset >> 3 >= 1u << (32 - Place::wordShift) || field->offset % width)
        return std::nullopt;
    instance->cacheOwnPropertyLocation(structure->id().bits(), name.uid(), Place::isCompiledField | Place::isNotValue | (field->offset & 7) << Place::byteShift | (64 - 8 * width) << Place::maskShiftShift | marker << Place::markerShift | field->offset >> 3 << Place::wordShift);
    return std::nullopt;
}

static void cacheOwnPropertyLocationFromSlot(Instance* instance, const PropertySlot& slot, JSObject* object, PropertyName name)
{
    using Place = Instance::OwnPropertyLocation;
    static_assert(static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete) == Place::attributes);
    cacheCompiledObjectLocation(instance, object, name);
    if (!appliesToAllObjectsWithStructure(slot, object, name))
        return;
    uint32_t place = 0;
    if (!slot.isUnset()) {
        place = Place::isPresent | Place::isNotValue;
        if (slot.isCacheableValue() && slot.slotBase() == object) {
            PropertyOffset offset = slot.cachedOffset();
            int32_t displacement = isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue) : offsetInButterfly(offset) * static_cast<int32_t>(sizeof(EncodedJSValue));
            if (displacement != static_cast<int32_t>(static_cast<uint32_t>(displacement) << Place::displacementShift) >> Place::displacementShift)
                return;
            place = Place::isPresent | (slot.attributes() & Place::attributes) | (isInlineOffset(offset) ? 0 : Place::isOutOfLine) | static_cast<uint32_t>(displacement) << Place::displacementShift;
        }
    }
    instance->cacheOwnPropertyLocation(object->structureID().bits(), name.uid(), place);
}

static void cacheOwnPropertyLocationFromSlot(Instance* instance, Structure* structure, UniquedStringImpl* uid, PropertyOffset offset, unsigned attributes)
{
    using Place = Instance::OwnPropertyLocation;
    uint32_t place = 0;
    if (isValidOffset(offset)) {
        place = Place::isPresent | Place::isNotValue;
        if (!(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))) {
            int32_t displacement = isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue) : offsetInButterfly(offset) * static_cast<int32_t>(sizeof(EncodedJSValue));
            if (displacement != static_cast<int32_t>(static_cast<uint32_t>(displacement) << Place::displacementShift) >> Place::displacementShift)
                return;
            place = Place::isPresent | (attributes & Place::attributes) | (isInlineOffset(offset) ? 0 : Place::isOutOfLine) | static_cast<uint32_t>(displacement) << Place::displacementShift;
        }
    }
    instance->cacheOwnPropertyLocation(structure->id().bits(), uid, place);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasOwnProperty, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        instance->validateKeyedTables();
    if (auto name = findNameEqualToLongString(JSValue::decode(encodedBase), JSValue::decode(encodedKey), false)) [[unlikely]]
        OPERATION_RETURN(scope, !!*name);
    {
        UniquedStringImpl* uid;
        PropertyOffset offset;
        unsigned attributes;
        if (findOwnPropertyByKeyGeneric(instance, JSValue::decode(encodedBase), JSValue::decode(encodedKey), uid, offset, attributes)) {
            if (uid)
                cacheOwnPropertyLocationFromSlot(instance, JSValue::decode(encodedBase).asCell()->structure(), uid, offset, attributes);
            OPERATION_RETURN(scope, isValidOffset(offset));
        }
    }
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool result = object->hasOwnProperty(globalObject, name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    cacheOwnPropertyLocationFromSlot(instance, slot, object, name);
    OPERATION_RETURN(scope, result);
}

static uint64_t headerWith(Structure* structure)
{
    return static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
}

static void prepareDataPropertyDescriptorAllocation(Instance* instance)
{
    auto& data = instance->inlineAllocations[static_cast<unsigned>(Instance::InlineAllocation::DataPropertyDescriptor)];
    if (data.allocator) [[likely]]
        return;
    Structure* structure = instance->globalObject->dataPropertyDescriptorObjectStructure();
    RELEASE_ASSERT(structure->inlineCapacity() == JSFinalObject::defaultInlineCapacity);
    auto* allocator = subspaceFor<JSFinalObject>(*instance->vm)->allocatorFor(JSFinalObject::defaultSizeInBytes, AllocatorForMode::EnsureAllocator).localAllocator();
    RELEASE_ASSERT(allocator->cellSize() == JSFinalObject::defaultSizeInBytes);
    data.header = headerWith(structure);
    data.allocator = allocator;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedObjectCreate, JSCell*, (Instance* instance, JSCell* prototype))
{
    prepareDataPropertyDescriptorAllocation(instance);
    JSFinalObject* object = constructEmptyObject(instance->globalObject, asObject(prototype));
    if (object->structure()->inlineCapacity() == JSFinalObject::defaultInlineCapacity) {
        instance->objectCreateCache[Instance::objectCreateCacheIndex(std::bit_cast<uintptr_t>(prototype))] = { prototype, headerWith(object->structure()) };
        instance->objectCreateCacheIsDirty = true;
    }
    return object;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetOwnPropertyDescriptor, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    prepareDataPropertyDescriptorAllocation(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSObject* object;
    if (base.isObject()) [[likely]]
        object = asObject(base);
    else {
        if (base.isUndefinedOrNull())
            OPERATION_RETURN(scope, JSValue::encode(callInEngine(instance, FunctionInEngine::GetOwnPropertyDescriptor, base, JSValue::decode(encodedKey))));
        object = base.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!object->structure()->typeInfo().overridesGetOwnPropertySlot()) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
        bool has = object->hasOwnProperty(globalObject, name, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        cacheOwnPropertyLocationFromSlot(instance, slot, object, name);
        PropertyDescriptor descriptor;
        has = has && descriptor.setPropertySlot(globalObject, name, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(has ? JSValue(constructObjectFromPropertyDescriptor(globalObject, descriptor)) : jsUndefined()));
    }
    OPERATION_RETURN(scope, JSValue::encode(objectConstructorGetOwnPropertyDescriptor(globalObject, object, name)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasOwnPropertyByName, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    const Identifier& identifier = instance->typed->name(vm, name);
    if (const Typed::LayoutData* layout = object->structure()->compiledLayout(); layout && Typed::isObjectLayout(layout->kind)) {
        auto* compiled = uncheckedDowncast<Typed::Object>(object);
        if (const Typed::FieldData* field = compiled->fieldNamed(compiled->state(), identifier)) {
            if (!compiled->state().mayBeAbsent(*field)) {
                instance->cacheOwnPropertyResult(object->structureID().bits(), name, true);
                OPERATION_RETURN(scope, true);
            }
            OPERATION_RETURN(scope, !!compiled->read(*field));
        }
        if (Typed::Object::structureGuaranteesAbsence(object->structure(), identifier.impl())) {
            instance->cacheOwnPropertyResult(object->structureID().bits(), name, false);
            OPERATION_RETURN(scope, false);
        }
    }
    {
        PropertyOffset offset;
        unsigned attributes;
        if (!parseIndex(*identifier.impl()) && findOwnPropertyGeneric(object, identifier.impl(), offset, attributes)) {
            instance->cacheOwnPropertyResult(object->structureID().bits(), name, isValidOffset(offset));
            OPERATION_RETURN(scope, isValidOffset(offset));
        }
    }
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool result = object->hasOwnProperty(globalObject, identifier, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (appliesToAllObjectsWithStructure(slot, object, identifier))
        instance->cacheOwnPropertyResult(object->structureID().bits(), name, result);
    OPERATION_RETURN(scope, result);
}

namespace Typed {

using namespace B3;

std::optional<Writes> writesOfInlineObject(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::ObjectHasOwn:
    case InlineBuiltin::ObjectKeys:
    case InlineBuiltin::ObjectGetOwnPropertyNames:
    case InlineBuiltin::ObjectGetOwnPropertySymbols:
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
    case InlineBuiltin::ReflectOwnKeys:
        return Writes::Heap;
    case InlineBuiltin::ObjectAssign:
        return writesIf(!arguments.empty(), Writes::Heap);
    case InlineBuiltin::ObjectGetOwnPropertyDescriptor:
        return writesIf(arguments.size() >= 2, Writes::Heap);
    case InlineBuiltin::ObjectCreate:
        return writesIf(arguments.size() == 1, Writes::Heap);
    case InlineBuiltin::ObjectGetPrototypeOf:
    case InlineBuiltin::ReflectGetPrototypeOf:
        return Writes::Heap;
    case InlineBuiltin::ObjectCall:
        return writesIf(arguments.size() == 1, Writes::RuntimeState);
    case InlineBuiltin::ObjectIs:
        return areNumbers(arguments) ? Writes::Pure : Writes::RuntimeState;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

CellPointer Lowering::objectOrElse(ValueID id, LBasicBlock otherwise)
{
    return narrowOrElse(get(id), typeOf(id), m_program.at(m_program.types.object), otherwise).as<CellPointer>();
}

bool Lowering::hasOnlyProgramObjects(const Type& type) const
{
    return !type.objectLayouts.isEmpty() && !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.acceptsUntypedObjects(m_program);
}

Operand Lowering::convertToDeclaredResult(Operand yielded, const Type& guarantee, const Value& call, const Type& type)
{
    if (guaranteesResultType(m_program, m_function, call))
        return convert(yielded, type, repOf(type));
    return ensure(yielded, guarantee, type);
}

CellPointer Lowering::allocateCell(LValue allocator, size_t cellSize, LBasicBlock slowPath)
{
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* patchpoint = m_out.patchpoint(pointerType());
    applyEffects(patchpoint, { Writes::RuntimeState, false });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(allocator, ValueRep::SomeRegister));
    patchpoint->numGPScratchRegisters = 1;
    patchpoint->resultConstraints = { ValueRep::SomeEarlyRegister };
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(slowPath));
    patchpoint->setGenerator([cellSize](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::JumpList outOfSpace;
        jit.emitAllocateWithNonNullAllocator(params[0].gpr(), JITAllocator::variableNonNullWithConstantCellSize(cellSize), params[1].gpr(), params.gpScratch(0), outOfSpace, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
        CCallHelpers::Jump done;
        if (!params.fallsThroughToSuccessor(0))
            done = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            outOfSpace.linkTo(*labels[1], &jit);
            if (done.isSet())
                done.linkTo(*labels[0], &jit);
        });
    });
    m_out.appendTo(continuation);
    return CellPointer(patchpoint);
}

CellPointer Lowering::newCopyOnWriteArray(LValue list, LBasicBlock slowPath)
{
    static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff && !(sizeof(JSArray) % MarkedSpace::sizeStep));
    LValue header = changing64(Instance::offsetOfNewCopyOnWriteArrayHeader(CopyOnWriteArrayWithContiguous));
    orElse(m_out.notZero64(header), slowPath);
    CellPointer array = allocateCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    storeHeader(array.value(), header);
    m_out.storePtr(m_out.add(list, m_out.constIntPtr(JSCellButterfly::offsetOfData())), array.value(), m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    return array;
}

LValue Lowering::layoutKeyListOrElse(CellPointer object, LBasicBlock otherwise)
{
    static_assert(sizeof(Instance::TypedKeyList) == 16);
    static_assert(hasOneBitSet(Instance::numberOfTypedKeyLists));
    static_assert(Instance::indexOfTypedKeyList(3, Instance::numberOfTypedKeyLists + 4) == 7);
    Word32 shapeWord = shapeWordOf(object);
    Word32 objectOrderWord = orderWordOf(object);
    LValue entry = m_out.add(fixedPointer(Instance::offsetOfTypedKeyLists()), m_out.shl(zeroExtended(bitAnd(Word32(m_out.add(shapeWord.value(), objectOrderWord.value())), word32(Instance::numberOfTypedKeyLists - 1))).value(), m_out.constInt32(4)));
    orElse(equal(Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, entry, OBJECT_OFFSETOF(Instance::TypedKeyList, shapeWord)))), shapeWord), otherwise);
    Word32 orderWord(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, entry, OBJECT_OFFSETOF(Instance::TypedKeyList, orderWord))));
    orElse(equal(orderWord, objectOrderWord), otherwise);
    LValue list = m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, entry, OBJECT_OFFSETOF(Instance::TypedKeyList, list)));
    orElse(m_out.notNull(list), otherwise);
    return list;
}

CellPointer Lowering::lowerOwnPropertyKeys(InlineBuiltin which, ValueID argument)
{
    OwnKeys keys = OwnKeys::ObjectKeys;
    const AbstractHeap* cachedOnStructure = &m_heaps.StructureRareData_cachedEnumerableStrings;
    switch (which) {
    case InlineBuiltin::ObjectKeys:
        break;
    case InlineBuiltin::ObjectGetOwnPropertyNames:
        keys = OwnKeys::ObjectGetOwnPropertyNames;
        cachedOnStructure = &m_heaps.StructureRareData_cachedStrings;
        break;
    case InlineBuiltin::ObjectGetOwnPropertySymbols:
        keys = OwnKeys::ObjectGetOwnPropertySymbols;
        cachedOnStructure = &m_heaps.StructureRareData_cachedSymbols;
        break;
    case InlineBuiltin::ReflectOwnKeys:
        keys = OwnKeys::ReflectOwnKeys;
        cachedOnStructure = &m_heaps.StructureRareData_cachedStringsAndSymbols;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto& type = typeOf(argument);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock haveList = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> lists;
    Vector<ValueFromBlock, 3> results;
    CellPointer object = objectOrElse(argument, slowPath);
    bool listsSymbols = keys == OwnKeys::ObjectGetOwnPropertySymbols;

    auto findLayoutKeyList = [&] {
        if (listsSymbols) {
            orElse(hasNoBits(orderWordOf(object), orderWordHasExtraProperties), slowPath);
            results.append(m_out.anchor(newArrayOf(std::span<const Boxed> { }).value()));
            m_out.jump(continuation);
            return;
        }
        lists.append(m_out.anchor(layoutKeyListOrElse(object, slowPath)));
        m_out.jump(haveList);
    };
    bool mayBeProgramObject = type.anyObject || !type.objectLayouts.isEmpty();
    if (mayBeProgramObject && !listsSymbols && hasOnlyProgramObjects(type))
        findLayoutKeyList();
    else {
        LBasicBlock isNotRetained = mayBeProgramObject ? newUnlikelyBlock() : slowPath;
        LValue previousOrRareData = m_out.loadPtr(structureOf(object.value()), m_heaps.Structure_previousOrRareData);
        orElse(m_out.notNull(previousOrRareData), isNotRetained);
        orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), isNotRetained);
        LValue list = m_out.loadPtr(previousOrRareData, *cachedOnStructure);
        orElse(m_out.above(list, m_out.constIntPtr(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel()))), isNotRetained);
        lists.append(m_out.anchor(list));
        m_out.jump(haveList);
        if (mayBeProgramObject) {
            m_out.appendTo(isNotRetained);
            orElse(passes(shapeWordOf(object), ShapeTest { 1, static_cast<uint32_t>(m_program.layouts.size()) }), slowPath);
            findLayoutKeyList();
        }
    }

    m_out.appendTo(haveList);
    results.append(m_out.anchor(newCopyOnWriteArray(m_out.phi(pointerType(), lists), slowPath).value()));
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    results.append(m_out.anchor(AOT_TYPED_OPERATION(OwnPropertyKeys, in<Boxed>(argument), word32(static_cast<int32_t>(keys))).value()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(pointerType(), results));
}

CellPointer Lowering::listKeysForIn(Boxed value, const Type& type)
{
    static_assert(!NonArray && ArrayClass == 1 && UndecidedShape == 2 && ArrayWithUndecided == 3);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    orElse(isCell(value, type), slowPath);
    CellPointer object = asCell(value);
    auto cachedEnumeratorFor = [&](CellPointer cell, LBasicBlock otherwise) {
        orElse(belowOrEqual(anyArrayIndexingMode(cell, IndexingTypeMask), word32(ArrayWithUndecided)), otherwise);
        LValue previousOrRareData = m_out.loadPtr(structureOf(cell.value()), m_heaps.Structure_previousOrRareData);
        orElse(m_out.notNull(previousOrRareData), otherwise);
        orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), otherwise);
        LValue enumerator = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_cachedPropertyNameEnumeratorAndFlag);
        orElse(m_out.notNull(enumerator), otherwise);
        orElse(m_out.testIsZeroPtr(enumerator, m_out.constIntPtr(StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag)), otherwise);
        return std::pair { previousOrRareData, enumerator };
    };
    auto listLayoutKeys = [&](LBasicBlock isNotProgramObject) {
        if (isNotProgramObject)
            orElse(passes(shapeWordOf(object), ShapeTest { 1, static_cast<uint32_t>(m_program.layouts.size()) }), isNotProgramObject);
        LValue list = layoutKeyListOrElse(object, slowPath);
        Boxed prototype(m_out.load64(structureOf(object.value()), m_heaps.Structure_prototype));
        orElse(m_out.notZero64(prototype.value()), slowPath);
        orElse(isCell(prototype), slowPath);
        LValue inherited = cachedEnumeratorFor(CellPointer(prototype.value()), slowPath).second;
        orElse(m_out.isZero32(m_out.load32(inherited, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex)), slowPath);
        orElse(m_out.isZero32(m_out.load32(inherited, m_heaps.JSPropertyNameEnumerator_indexLength)), slowPath);
        results.append(m_out.anchor(newCopyOnWriteArray(list, slowPath).value()));
        m_out.jump(continuation);
    };
    bool isProgramObject = hasOnlyProgramObjects(type);
    if (!type.objectLayouts.isEmpty()) {
        LBasicBlock isNotProgramObject = isProgramObject ? nullptr : m_out.newBlock();
        listLayoutKeys(isNotProgramObject);
        if (!isProgramObject)
            m_out.appendTo(isNotProgramObject);
    }
    if (!isProgramObject) {
        LBasicBlock hasNoCachedList = type.objectLayouts.isEmpty() && type.anyObject ? newUnlikelyBlock() : slowPath;
        auto [previousOrRareData, enumerator] = cachedEnumeratorFor(object, hasNoCachedList);
        LValue numberOfNames = m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex);
        orElse(m_out.isZero32(m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength)), slowPath);
        LBasicBlock returnsEmptyList = m_out.newBlock();
        LBasicBlock listsNames = m_out.newBlock();
        m_out.branch(m_out.notZero32(numberOfNames), unsure(listsNames), unsure(returnsEmptyList));
        m_out.appendTo(returnsEmptyList);
        results.append(m_out.anchor(fixedPointer(Instance::offsetOfEmptyKeyList())));
        m_out.jump(continuation);
        m_out.appendTo(listsNames);
        LValue list = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_cachedEnumerableStrings);
        orElse(m_out.above(list, m_out.constIntPtr(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel()))), slowPath);
        orElse(m_out.equal(m_out.load32(m_out.add(list, m_out.constIntPtr(JSCellButterfly::offsetOfData())), m_heaps.Butterfly_publicLength), numberOfNames), slowPath);
        results.append(m_out.anchor(newCopyOnWriteArray(list, slowPath).value()));
        m_out.jump(continuation);
        if (hasNoCachedList != slowPath) {
            m_out.appendTo(hasNoCachedList);
            listLayoutKeys(slowPath);
        }
    }

    m_out.appendTo(slowPath);
    results.append(m_out.anchor(AOT_TYPED_OPERATION(ForInKeys, value).value()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(pointerType(), results));
}

Lowering::OwnField Lowering::ownFieldOfSubtypes(LayoutID view, StringID name) const
{
    auto& members = m_program.at(view).subtypes;
    bool isInAll = !members.isEmpty();
    bool isInNone = !members.isEmpty();
    for (LayoutID member : members) {
        auto& layout = m_program.at(member);
        auto index = layout.fieldNamed(name);
        if (!index) {
            isInAll = false;
            continue;
        }
        isInNone = false;
        TypeID stored = layout.fields[*index].stored;
        if (!stored || m_program.at(stored).primitives & Primitive::Absent)
            isInAll = false;
    }
    return isInAll ? OwnField::InAllMembers : isInNone ? OwnField::InNoMember : OwnField::Varies;
}

Word32 Lowering::lowerHasOwnProperty(ValueID object, ValueID key)
{
    auto text = constantASCIIString(key);
    if (!text || text->empty() || isASCIIDigit(text->front()) || m_function.at(originOf(key)).string().index() > Instance::maxNameWithOwnPropertyResult) {
        if (!(predictedTypeOf(key).primitives & Primitive::String))
            return AOT_TYPED_OPERATION(HasOwnProperty, in<Boxed>(object), in<Boxed>(key));
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        if (auto* stamp = forInStampOf(object, key)) {
            LBasicBlock hasChanged = newColdBlock();
            isAsListedOrElse(*stamp, in<Boxed>(object), hasChanged);
            join.add(boolean(true));
            m_out.appendTo(hasChanged);
        }
        CellPointer cell = objectOrElse(object, slowPath);
        join.add(hasOwnPropertyAt(cell, ownPropertyLocationOrElse(cell, key, slowPath)));
        m_out.appendTo(slowPath);
        join.add(AOT_TYPED_OPERATION(HasOwnProperty, in<Boxed>(object), in<Boxed>(key)));
        return join.finish().as<Word32>();
    }
    StringID name = m_function.at(originOf(key)).string();
    auto genericPath = [&](Boxed value) {
        return AOT_TYPED_OPERATION(HasOwnPropertyByName, value, word32(name.index()));
    };
    auto& receiver = typeOf(object);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    CellPointer cell = objectOrElse(object, slowPath);

    auto checkStructure = [&]() -> Operand {
        static_assert(Instance::ownPropertyResultIndex(0x1230, 7) == (((0x1230u >> 4) ^ Instance::ownPropertyResultSalt(7)) & (Instance::numberOfOwnPropertyResults - 1)));
        Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID);
        Word32 index = bitAnd(bitXor(shiftRightUnsigned(structureID, word32(4)), word32(Instance::ownPropertyResultSalt(name.index()))), word32(Instance::numberOfOwnPropertyResults - 1));
        Pointer answers(m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfOwnPropertyResults())));
        Integer pair = bitOr(zeroExtended(structureID), integer(static_cast<int64_t>(name.index()) << 32));
        auto differenceAt = [&](Word32 place) {
            return bitXor(Integer(m_out.load64(TypedPointer(m_heaps.AOTTyped_collectorState, offsetBy(answers, zeroExtended(place), 3).value()))), pair);
        };
        auto matchesPair = [&](Integer difference) {
            return isZero(Integer(m_out.shl(difference.value(), m_out.constInt32(1))));
        };
        LBasicBlock probesNeighbor = newUnlikelyBlock();
        LBasicBlock notInNeighbor = newUnlikelyBlock();
        LBasicBlock isFound = m_out.newBlock();
        Vector<ValueFromBlock, 2> found;
        Integer difference = differenceAt(index);
        ValueFromBlock firstStep = m_out.anchor(m_out.constInt32(1));
        orElse(matchesPair(difference), probesNeighbor);
        found.append(m_out.anchor(isNotZero(difference).value()));
        m_out.jump(isFound);

        m_out.appendTo(probesNeighbor);
        LValue step = m_out.phi(Int32, firstStep);
        Integer neighborDifference = differenceAt(bitXor(index, Word32(step)));
        orElse(matchesPair(neighborDifference), notInNeighbor);
        found.append(m_out.anchor(isNotZero(neighborDifference).value()));
        m_out.jump(isFound);

        m_out.appendTo(notInNeighbor);
        LValue nextStep = m_out.add(step, m_out.constInt32(1));
        m_out.addIncomingToPhi(step, m_out.anchor(nextStep));
        m_out.branch(m_out.below(nextStep, m_out.constInt32(Instance::ownPropertyResultWays)), unsure(probesNeighbor), unsure(slowPath));

        m_out.appendTo(isFound);
        return Word32(m_out.phi(Int32, found));
    };

    LayoutID view = receiver.singleLayout();
    if (!view)
        join.add(checkStructure());
    else {
        join.add(ifSubtype(cell, object, view, Rep::Int32, [&]() -> Operand {
            switch (ownFieldOfSubtypes(view, name)) {
            case OwnField::InAllMembers:
                return boolean(true);
            case OwnField::InNoMember:
                return either<Word32>(hasNoBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
                    return boolean(false);
                }, [&] {
                    return genericPath(box(cell));
                });
            case OwnField::Varies:
                break;
            }
            const Field* field = fieldFor(receiver, name);
            if (!field || (field->isMissingInSomeSubtype && field->subtypesWithField.neverPasses()))
                return genericPath(box(cell));
            return accessThroughSupertype(cell, object, view, *field, Rep::Int32, [&]() -> Operand {
                return hasOwnField(cell, *field);
            }, [&]() -> Operand {
                return genericPath(box(cell));
            });
        }, checkStructure));
    }

    m_out.appendTo(slowPath);
    join.add(genericPath(in<Boxed>(object)));
    return join.finish().as<Word32>();
}

Word32 Lowering::ownPropertyLocationOrElse(CellPointer object, ValueID key, LBasicBlock otherwise)
{
    using Place = Instance::OwnPropertyLocation;
    static_assert(sizeof(Place) == 16 && !OBJECT_OFFSETOF(Place, uid) && OBJECT_OFFSETOF(Place, structureID) == 8 && OBJECT_OFFSETOF(Place, place) == 12);
    static_assert(Instance::ownPropertyLocationIndex(0x1230, 0x7f40) == (((0x7f40u + 0x1230u + (0x1230u << 2)) >> 4) & (Instance::numberOfOwnPropertyLocations - 1)));
    CellPointer string = narrowOrElse(get(key), typeOf(key), m_program.at(m_program.types.string), otherwise).as<CellPointer>();
    Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
    orElse(hasNoBits(impl, JSString::isRopeInPointer), otherwise);
    orElse(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIsAtom()), otherwise);
    Integer structureID = zeroExtended(loadField<Word32>(object, m_heaps.JSCell_structureID));
    Integer uid(impl.value());
    Integer offset = bitAnd(add(uid, add(structureID, Integer(m_out.shl(structureID.value(), m_out.constInt32(2))))), integer((Instance::numberOfOwnPropertyLocations - 1) << 4));
    LValue table = m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfOwnPropertyLocations()));
    LBasicBlock probesNeighbor = newUnlikelyBlock();
    LBasicBlock notInNeighbor = newUnlikelyBlock();
    LBasicBlock isFound = m_out.newBlock();
    Vector<ValueFromBlock, 2> words;
    auto ask = [&](Integer offsetOfEntry, LBasicBlock isMissing) {
        LValue entry = m_out.add(table, offsetOfEntry.value());
        orElse(equal(Integer(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, entry, OBJECT_OFFSETOF(Place, uid)))), uid), isMissing);
        Integer found(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, entry, OBJECT_OFFSETOF(Place, structureID))));
        orElse(equal(low32(found), low32(structureID)), isMissing);
        words.append(m_out.anchor(found.value()));
        m_out.jump(isFound);
    };
    ValueFromBlock firstStep = m_out.anchor(m_out.constInt64(sizeof(Place)));
    ask(offset, probesNeighbor);
    m_out.appendTo(probesNeighbor);
    LValue step = m_out.phi(Int64, firstStep);
    ask(bitXor(offset, Integer(step)), notInNeighbor);
    m_out.appendTo(notInNeighbor);
    LValue nextStep = m_out.add(step, m_out.constInt64(sizeof(Place)));
    m_out.addIncomingToPhi(step, m_out.anchor(nextStep));
    m_out.branch(m_out.below(nextStep, m_out.constInt64(Instance::ownPropertyLocationWays * sizeof(Place))), unsure(probesNeighbor), unsure(otherwise));
    m_out.appendTo(isFound);
    return low32(Integer(m_out.lShr(m_out.phi(Int64, words), m_out.constInt32(32))));
}

Word32 Lowering::hasOwnPropertyAt(CellPointer object, Word32 place)
{
    using Place = Instance::OwnPropertyLocation;
    LBasicBlock isField = m_out.newBlock();
    LBasicBlock isByStructure = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(hasBits(place, Place::isCompiledField).value(), unsure(isField), unsure(isByStructure));
    m_out.appendTo(isByStructure);
    join.add(bitAnd(place, word32(Place::isPresent)));
    m_out.appendTo(isField);
    auto part = [&](unsigned shift, uint32_t mask) {
        return m_out.bitAnd(m_out.lShr(place.value(), m_out.constInt32(shift)), m_out.constInt32(mask));
    };
    static_assert(Place::wordShift + 13 == 32);
    LValue address = m_out.add(object.value(), m_out.shl(m_out.zeroExtPtr(m_out.lShr(place.value(), m_out.constInt32(Place::wordShift))), m_out.constInt32(3)));
    LValue held = m_out.lShr(m_out.load64(TypedPointer(m_heaps.root, address)), m_out.shl(part(Place::byteShift, 7), m_out.constInt32(3)));
    held = m_out.bitAnd(held, m_out.lShr(m_out.constInt64(-1), part(Place::maskShiftShift, 63)));
    LValue which = part(Place::markerShift, 3);
    auto isMarker = [&](unsigned number) {
        return m_out.equal(which, m_out.constInt32(number));
    };
    LValue absent = m_out.select(isMarker(1), integer(static_cast<uint32_t>(absentInt32)).value(), m_out.select(isMarker(2), integer(static_cast<int64_t>(absentDouble)).value(), m_out.select(isMarker(3), integer(absentInteger).value(), m_out.int64Zero)));
    join.add(Word32(m_out.notEqual(held, absent)));
    return join.finish().as<Word32>();
}

TypedPointer Lowering::inlineProperty(CellPointer object, PropertyOffset offset)
{
    return m_out.address(m_heaps.properties.atAnyNumber(), object.value(), JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue));
}

Operand Lowering::lowerGetOwnPropertyDescriptor(ValueID result, const Value& value)
{
    using Place = Instance::OwnPropertyLocation;
    constexpr auto kind = Instance::InlineAllocation::DataPropertyDescriptor;
    static_assert(JSFinalObject::defaultInlineCapacity == 6 && !dataPropertyDescriptorValuePropertyOffset && dataPropertyDescriptorWritablePropertyOffset == 1 && dataPropertyDescriptorEnumerablePropertyOffset == 2 && dataPropertyDescriptorConfigurablePropertyOffset == 3);
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
    auto& type = typeOf(result);
    auto& anything = m_program.at(m_program.types.anything);
    ValueID key = value.arguments[1];
    auto genericPath = [&] {
        return convertToDeclaredResult(AOT_TYPED_OPERATION(GetOwnPropertyDescriptor, in<Boxed>(value.arguments[0]), in<Boxed>(key)), anything, value, type);
    };
    if (!(predictedTypeOf(key).primitives & Primitive::String))
        return genericPath();
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock isPresent = m_out.newBlock();
    LBasicBlock isAbsent = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer object = objectOrElse(value.arguments[0], slowPath);
    Word32 place = ownPropertyLocationOrElse(object, key, slowPath);
    orElse(hasNoBits(place, Place::isNotValue), slowPath);
    m_out.branch(hasBits(place, Place::isPresent).value(), unsure(isPresent), unsure(isAbsent));

    m_out.appendTo(isPresent);
    LValue allocator = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(kind)));
    orElse(m_out.notNull(allocator), slowPath);
    CellPointer descriptor = allocateCell(allocator, JSFinalObject::defaultSizeInBytes, slowPath);
    Pointer base = select(hasBits(place, Place::isOutOfLine), butterflyOf(object), Pointer(object.value()));
    LValue displacement = m_out.signExt32ToPtr(m_out.aShr(place.value(), m_out.constInt32(Place::displacementShift)));
    LValue held = m_out.load64(TypedPointer(m_heaps.root, m_out.add(base.value(), displacement)));
    auto isMissingAttribute = [&](PropertyAttribute attribute) {
        return m_out.bitOr(m_out.constInt64(JSValue::ValueFalse), zeroExtended(hasNoBits(place, static_cast<unsigned>(attribute))).value());
    };
    m_out.store64(m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfHeaderFor(kind))), descriptor.value(), m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, descriptor.value(), m_heaps.JSObject_butterfly);
    m_out.store64(held, inlineProperty(descriptor, dataPropertyDescriptorValuePropertyOffset));
    m_out.store64(isMissingAttribute(PropertyAttribute::ReadOnly), inlineProperty(descriptor, dataPropertyDescriptorWritablePropertyOffset));
    m_out.store64(isMissingAttribute(PropertyAttribute::DontEnum), inlineProperty(descriptor, dataPropertyDescriptorEnumerablePropertyOffset));
    m_out.store64(isMissingAttribute(PropertyAttribute::DontDelete), inlineProperty(descriptor, dataPropertyDescriptorConfigurablePropertyOffset));
    for (PropertyOffset offset = dataPropertyDescriptorConfigurablePropertyOffset + 1; offset < static_cast<PropertyOffset>(JSFinalObject::defaultInlineCapacity); ++offset)
        m_out.store64(m_out.int64Zero, inlineProperty(descriptor, offset));
    fenceAfterInitialization();
    join.add(convertToDeclaredResult(descriptor, m_program.at(m_program.types.object), value, type));

    m_out.appendTo(isAbsent);
    join.add(convertToDeclaredResult(undefined(), m_program.at(m_program.types.undefined), value, type));

    m_out.appendTo(slowPath);
    join.add(genericPath());
    return join.finish();
}

Operand Lowering::lowerCreate(ValueID result, const Value& value)
{
    using CacheEntry = Instance::ObjectCreateCacheEntry;
    static_assert(sizeof(CacheEntry) == 16 && !OBJECT_OFFSETOF(CacheEntry, prototype) && Instance::objectCreateCacheIndex(0x7f40) == ((0x7f40 >> 4) & (Instance::objectCreateCacheSize - 1)));
    auto& type = typeOf(result);
    auto& objectType = m_program.at(m_program.types.object);
    Rep rep = repOf(type);
    LBasicBlock noObject = newBlockForGenericCall(value);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer prototype = objectOrElse(value.arguments[0], noObject);
    LValue offset = m_out.bitAnd(prototype.value(), m_out.constIntPtr((Instance::objectCreateCacheSize - 1) << 4));
    LValue entry = m_out.add(m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfObjectCreateCache())), offset);
    orElse(m_out.equal(m_out.loadPtr(m_out.address(m_heaps.AOTTyped_collectorState, entry, OBJECT_OFFSETOF(CacheEntry, prototype))), prototype.value()), slowPath);
    LValue header = m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, entry, OBJECT_OFFSETOF(CacheEntry, header)));
    LValue allocator = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(Instance::InlineAllocation::DataPropertyDescriptor)));
    CellPointer object = allocateCell(allocator, JSFinalObject::defaultSizeInBytes, slowPath);
    m_out.store64(header, object.value(), m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, object.value(), m_heaps.JSObject_butterfly);
    for (PropertyOffset slot = 0; slot < static_cast<PropertyOffset>(JSFinalObject::defaultInlineCapacity); ++slot)
        m_out.store64(m_out.int64Zero, inlineProperty(object, slot));
    fenceAfterInitialization();
    join.add(convertToDeclaredResult(object, objectType, value, type));

    m_out.appendTo(slowPath);
    join.add(convertToDeclaredResult(AOT_TYPED_OPERATION(ObjectCreate, prototype), objectType, value, type));

    if (!isSharedGenericPath(noObject)) {
        m_out.appendTo(noObject);
        join.add(convertToDeclaredResult(callBuiltinFunction(value, builtinCallEffects(value)), m_program.at(m_program.types.anything), value, type));
    }
    return join.finish();
}

Operand Lowering::lowerGetPrototypeOf(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    LBasicBlock slowPath = newBlockForGenericCall(value);
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer object = objectOrElse(value.arguments[0], slowPath);
    orElse(hasNoBits(loadByte(object, m_heaps.JSCell_typeInfoFlags), OverridesGetPrototype), slowPath);
    Boxed prototype(m_out.load64(structureOf(object.value()), m_heaps.Structure_prototype));
    orElse(isNotEmpty(prototype), slowPath);
    Type objectOrNull = Type::object();
    objectOrNull.primitives = Primitive::Null;
    join.add(convertToDeclaredResult(prototype, objectOrNull, value, type));

    if (!isSharedGenericPath(slowPath)) {
        m_out.appendTo(slowPath);
        join.add(convertToDeclaredResult(callBuiltinFunction(value, builtinCallEffects(value)), m_program.at(m_program.types.anything), value, type));
    }
    return join.finish();
}

Vector<Lowering::KeyOrder, 2> Lowering::keyOrdersExpectedOf(ValueID id) const
{
    static constexpr unsigned maxOrders = 2;
    static constexpr unsigned maxFields = 8;
    LayoutID view = predictedTypeOf(id).withoutPrimitives(Primitive::UndefinedOrNull).singleLayout();
    if (!view || m_program.at(view).subtypes.size() > maxOrders)
        return { };
    Vector<KeyOrder, 2> result;
    bool hasTooMany = false;
    auto add = [&](LayoutID member, uint32_t order, uint32_t count) {
        uint32_t orderWord = Typed::orderWordOf(order, count);
        if (hasTooMany || result.containsIf([&](const KeyOrder& known) { return known.layout == member && known.orderWord == orderWord; }))
            return;
        auto& layout = m_program.at(member);
        KeyOrder keys { member, orderWord, { } };
        for (uint16_t index : layout.orders[order].span().first(count)) {
            auto name = m_program.string(layout.fields[index].name);
            if (isPrivateName(name))
                continue;
            hasTooMany |= name.empty() || isASCIIDigit(name.front()) || equalSpans(name, asBytes("__proto__"_s.span8()));
            keys.fields.append(index);
        }
        hasTooMany |= result.size() == maxOrders || keys.fields.size() > maxFields;
        if (!hasTooMany)
            result.append(WTF::move(keys));
    };
    for (LayoutID member : m_program.at(view).subtypes) {
        auto& layout = m_program.at(member);
        if (!isObjectLayout(layout.kind))
            return { };
        for (auto& allocation : m_program.allocations) {
            if (allocation.layout != member)
                continue;
            Vector<uint16_t, 16> expected;
            uint32_t born = 0;
            for (unsigned i = 0; i < allocation.order.size(); ++i) {
                if (layout.fields[allocation.order[i]].flags & fieldIsNeverStored)
                    continue;
                expected.append(allocation.order[i]);
                born += i < allocation.initialFieldCount;
            }
            size_t order = layout.orders.findIf([&](auto& candidate) {
                return candidate.size() >= expected.size() && std::equal(expected.begin(), expected.end(), candidate.begin());
            });
            if (order != notFound && born <= maxFieldsInOrder)
                add(member, order, born);
        }
        for (unsigned order = 0; order < layout.orders.size(); ++order)
            add(member, order, layout.orders[order].size());
    }
    return hasTooMany ? Vector<KeyOrder, 2> { } : result;
}

template<typename InlinePath, typename GenericPath>
void Lowering::byKeyOrder(ValueID object, const Vector<KeyOrder, 2>& orders, const InlinePath& inlinePath, const GenericPath& genericPath)
{
    if (orders.isEmpty()) {
        genericPath();
        return;
    }
    LBasicBlock otherwise = newColdBlock();
    CellPointer cell = objectOrElse(object, otherwise);
    Word32 shapeWord = shapeWordOf(cell);
    Word32 orderWord = orderWordOf(cell);
    for (auto& order : orders) {
        LBasicBlock matches = m_out.newBlock();
        LBasicBlock next = &order == &orders.last() ? otherwise : m_out.newBlock();
        m_out.branch(logicalAnd(equal(shapeWord, word32(Typed::shapeWordOf(m_program.at(order.layout).shape))), equal(orderWord, word32(order.orderWord))).value(), usually(matches), rarely(next));
        m_out.appendTo(matches);
        inlinePath(cell, order);
        m_out.appendTo(next);
    }
    genericPath();
}

CellPointer Lowering::cloneAsPlainObject(ValueID source, const CloneDefinitions& defined)
{
    auto& anything = m_program.at(m_program.types.anything);
    Join made(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
    byKeyOrder(source, keyOrdersExpectedOf(source), [&](CellPointer cell, const KeyOrder& order) {
        auto& fields = m_program.at(order.layout).fields;
        Vector<StringID, 8> names;
        Vector<Boxed, 8> values;
        for (uint16_t index : order.fields) {
            names.append(fields[index].name);
            values.append(readField(cell, fields[index], anything).as<Boxed>());
        }
        made.add(newObjectLiteral(names.span(), values.span()));
    }, [&] {
        if (defined.more)
            made.add(AOT_TYPED_OPERATION(CloneObjectAndExtend, in<Boxed>(source), integer(static_cast<int64_t>(defined.more)), defined.values.size() > 0 ? in<Boxed>(defined.values[0]) : undefined(), defined.values.size() > 1 ? in<Boxed>(defined.values[1]) : undefined()));
        else
            made.add(AOT_TYPED_OPERATION(CloneObject, in<Boxed>(source), word32(false)));
    });
    return made.finish().as<CellPointer>();
}

CellPointer Lowering::newArrayOf(std::span<const Boxed> elements)
{
    unsigned length = elements.size();
    if (length <= maxArrayElementsInRegisters) {
        Vector<std::pair<LValue, GPRReg>, 4> operands;
        for (unsigned i = 0; i < length; ++i)
            operands.append({ elements[i].value(), GPRInfo::toArgumentRegister(1 + i) });
        return CellPointer(callAllocationStub(arrayLiteralStub(length), operands, effectsForOperation<operationAOTTypedNewArray>(), true));
    }
    unsigned vectorLength = std::max<unsigned>(length, BASE_CONTIGUOUS_VECTOR_LEN) | 1;
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto [array, butterfly] = allocateArray(word32(length), word32(vectorLength), slowPath);
    for (unsigned i = 0; i < length; ++i)
        m_out.store64(elements[i].value(), butterfly.value(), m_heaps.indexedContiguousProperties[i]);
    fenceAfterInitialization();
    ValueFromBlock fast = m_out.anchor(array.value());
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    LValue buffer = transientBuffer(length * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < length; ++i)
        m_out.store64(elements[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArray, word32(length), Pointer(buffer)).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

CellPointer Lowering::lowerValuesOrEntries(InlineBuiltin which, const Value& value)
{
    ValueID object = value.arguments[0];
    auto& anything = m_program.at(m_program.types.anything);
    Join join(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
    byKeyOrder(object, keyOrdersExpectedOf(object), [&](CellPointer cell, const KeyOrder& order) {
        auto& fields = m_program.at(order.layout).fields;
        Vector<Boxed, 8> elements;
        for (uint16_t index : order.fields)
            elements.append(readField(cell, fields[index], anything).as<Boxed>());
        if (which == InlineBuiltin::ObjectEntries && value.interleavesKeysAndValues()) {
            Vector<Boxed, 16> alternating;
            for (unsigned i = 0; i < elements.size(); ++i) {
                alternating.append(box(stringConstant(fields[order.fields[i]].name)));
                alternating.append(elements[i]);
            }
            join.add(newArrayOf(alternating.span()));
            return;
        }
        if (which == InlineBuiltin::ObjectEntries) {
            for (unsigned i = 0; i < elements.size(); ++i) {
                Boxed pair[] = { box(stringConstant(fields[order.fields[i]].name)), elements[i] };
                elements[i] = box(newArrayOf(pair));
            }
        }
        join.add(newArrayOf(elements.span()));
    }, [&] {
        if (which == InlineBuiltin::ObjectValues)
            join.add(AOT_TYPED_OPERATION(ObjectValues, in<Boxed>(object)));
        else if (value.interleavesKeysAndValues())
            join.add(AOT_TYPED_OPERATION(ObjectEntriesFlat, in<Boxed>(object)));
        else {
            CellPointer made = AOT_TYPED_OPERATION(ObjectEntries, in<Boxed>(object));
            LBasicBlock isMade = m_out.newBlock();
            LBasicBlock isNotCreated = newBlockForGenericCall(value);
            m_out.branch(m_out.notNull(made.value()), usually(isMade), rarely(isNotCreated));
            m_out.appendTo(isMade);
            join.add(made);
            if (!isSharedGenericPath(isNotCreated)) {
                m_out.appendTo(isNotCreated);
                join.add(convert(callBuiltinFunction(value, builtinCallEffects(value)), Type::engineClass(m_builtins.arrayClass()), Rep::Cell));
            }
        }
    });
    return join.finish().as<CellPointer>();
}

bool Lowering::isNewPlainObject(ValueID id) const
{
    auto& values = m_currentBlock->values;
    for (size_t index = m_currentIndexInBlock; index--;) {
        if (values[index] == id)
            return m_function.at(id).opcode == Opcode::NewObject;
        auto& value = m_function.at(values[index]);
        if (isDefineField(value) && value.arguments[0] == id && value.arguments[2] != id && m_function.at(value.arguments[1]).opcode == Opcode::String)
            continue;
        bool usesObject = false;
        value.forEachChild([&](ValueID child) {
            usesObject |= child == id;
        });
        if (usesObject)
            return false;
    }
    return false;
}

Operand Lowering::lowerAssign(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    auto& anything = m_program.at(m_program.types.anything);
    Rep rep = repOf(type);
    LBasicBlock noObject = nullptr;
    Join join(m_out, rep, b3TypeOf(rep));
    ValueID into = value.arguments[0];
    bool isNew = isNewPlainObject(into);
    unsigned firstSource = 1;
    CellPointer target;
    if (isAllocatedHere(into)) {
        ValueID source = value.arguments[firstSource++];
        Join made(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
        byKeyOrder(source, keyOrdersExpectedOf(source), [&](CellPointer cell, const KeyOrder& order) {
            auto& fields = m_program.at(order.layout).fields;
            Vector<StringID, 8> names;
            Vector<Boxed, 8> values;
            for (uint16_t index : order.fields) {
                names.append(fields[index].name);
                values.append(readField(cell, fields[index], anything).as<Boxed>());
            }
            made.add(newObjectLiteral(names.span(), values.span()));
        }, [&] {
            made.add(AOT_TYPED_OPERATION(ObjectAssignToNew, in<Boxed>(source)));
        });
        target = made.finish().as<CellPointer>();
        defineDeferredAllocation(into, target);
        RELEASE_ASSERT(isNew);
    } else {
        noObject = newBlockForGenericCall(value);
        target = objectOrElse(into, noObject);
    }
    for (unsigned i = firstSource; i < value.arguments.size(); ++i) {
        ValueID source = value.arguments[i];
        LBasicBlock continuation = m_out.newBlock();
        byKeyOrder(source, isNew ? keyOrdersExpectedOf(source) : Vector<KeyOrder, 2> { }, [&](CellPointer cell, const KeyOrder& order) {
            auto& fields = m_program.at(order.layout).fields;
            for (uint16_t index : order.fields)
                emitPutById(box(target), fields[index].name, readField(cell, fields[index], anything).as<Boxed>());
            m_out.jump(continuation);
        }, [&] {
            AOT_TYPED_OPERATION(ObjectAssignFrom, target, in<Boxed>(source));
            m_out.jump(continuation);
        });
        m_out.appendTo(continuation);
    }
    join.add(convertToDeclaredResult(target, m_program.at(m_program.types.object), value, type));
    if (noObject && !isSharedGenericPath(noObject)) {
        m_out.appendTo(noObject);
        join.add(convertToDeclaredResult(callBuiltinFunction(value, builtinCallEffects(value)), anything, value, type));
    }
    return join.finish();
}

Word32 Lowering::lowerSameValue(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    auto areSameNumber = [&](Number x, Number y) {
        return logicalOr(equal(bitsOf(x), bitsOf(y)), logicalAnd(notEqualOrUnordered(x, x), notEqualOrUnordered(y, y)));
    };
    if (left.isInteger() && right.isInteger())
        return equal(get(a).as<Integer>(), get(b).as<Integer>());
    if (left.isNumber() && right.isNumber())
        return areSameNumber(in<Number>(a), in<Number>(b));
    if (!(left.primitives & right.primitives & Primitive::Number))
        return lowerStrictEq(a, b);
    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    LBasicBlock numbers = m_out.newBlock();
    LBasicBlock others = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(logicalAnd(isNumber(x), isNumber(y)).value(), unsure(numbers), unsure(others));
    m_out.appendTo(numbers);
    join.add(areSameNumber(Number(numberToDouble(x.value())), Number(numberToDouble(y.value()))));
    m_out.appendTo(others);
    join.add(lowerStrictEq(a, b));
    return join.finish().as<Word32>();
}

std::optional<Operand> Lowering::lowerInlineObject(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    auto& type = typeOf(result);
    switch (which) {
    case InlineBuiltin::ObjectHasOwn: {
        Word32 answer = lowerHasOwnProperty(value.arguments[0], value.arguments[1]);
        Operand yielded = booleanResult(answer, type);
        m_truthinessPhis.add(result.index(), Truthiness { yielded.value, answer.value() });
        return yielded;
    }
    case InlineBuiltin::ObjectKeys:
    case InlineBuiltin::ObjectGetOwnPropertyNames:
    case InlineBuiltin::ObjectGetOwnPropertySymbols:
    case InlineBuiltin::ReflectOwnKeys:
        return ensure(lowerOwnPropertyKeys(which, value.arguments[0]), Type::engineClass(m_builtins.arrayClass()), type);
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
        return ensure(lowerValuesOrEntries(which, value), Type::engineClass(m_builtins.arrayClass()), type);
    case InlineBuiltin::ObjectAssign:
        return lowerAssign(result, value);
    case InlineBuiltin::ObjectGetOwnPropertyDescriptor:
        return lowerGetOwnPropertyDescriptor(result, value);
    case InlineBuiltin::ObjectCreate:
        return lowerCreate(result, value);
    case InlineBuiltin::ObjectGetPrototypeOf:
    case InlineBuiltin::ReflectGetPrototypeOf:
        return lowerGetPrototypeOf(result, value);
    case InlineBuiltin::ObjectIs:
        return booleanResult(lowerSameValue(value.arguments[0], value.arguments[1]), type);
    case InlineBuiltin::ObjectCall: {
        Rep rep = repOf(type);
        LBasicBlock slowPath = newBlockForGenericCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(convertToDeclaredResult(objectOrElse(value.arguments[0], slowPath), m_program.at(m_program.types.object), value, type));
        if (!isSharedGenericPath(slowPath)) {
            m_out.appendTo(slowPath);
            join.add(convertToDeclaredResult(callBuiltinFunction(value, builtinCallEffects(value)), m_program.at(m_program.types.anything), value, type));
        }
        return join.finish();
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
