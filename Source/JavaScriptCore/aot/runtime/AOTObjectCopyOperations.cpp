/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTFieldMoves.h"
#include "AOTInstance.h"
#include "AOTMappedCopyInlines.h"
#include "AOTModuleHost.h"
#include "AOTModuleNamespace.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "ButterflyInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

class ExcludedKeys {
    WTF_MAKE_NONCOPYABLE(ExcludedKeys);
public:
    ExcludedKeys() = default;

    void collect(Instance* instance, JSGlobalObject* globalObject, ExcludedRun run, std::span<const EncodedJSValue> keys)
    {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (run.numberPlusOne) {
            m_listOfRun = &instance->state->objects->caches->assignmentPlans.excludedNames(run);
            m_names = m_listOfRun->names();
        }
        if (keys.empty())
            return;
        m_namesWithOperands.append(m_names);
        m_listOfRun = nullptr;
        for (EncodedJSValue encodedKey : keys) {
            JSValue key = JSValue::decode(encodedKey);
            if (key.isInt32()) {
                m_namesWithOperands.append(identifierAt(instance, key.asInt32()).impl());
                continue;
            }
            auto identifier = key.toPropertyKey(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            m_namesWithOperands.append(identifier.impl());
            m_identifiers.append(WTF::move(identifier));
        }
        m_names = m_namesWithOperands.span();
    }

    bool isEmpty() const { return m_names.empty(); }
    std::span<UniquedStringImpl* const> names() const { return m_names; }
    bool contains(UniquedStringImpl* name) const { return m_listOfRun ? m_listOfRun->contains(name) : std::ranges::contains(m_names, name); }

private:
    const ExcludedNameList* m_listOfRun { nullptr };
    std::span<UniquedStringImpl* const> m_names;
    Vector<UniquedStringImpl*, 8> m_namesWithOperands;
    Vector<Identifier, 8> m_identifiers;
};

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
    size_t cellSize = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(names.size()));
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), std::min<unsigned>((cellSize - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity));
    JSFinalObject* copy = JSFinalObject::create(vm, profile.structure());
    copy->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
    return copy;
}

static bool tryCollectProperties(VM& vm, JSObject* source, const ExcludedKeys& excluded, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    if (isCompiledObject(source)) {
        auto walkResult = PropertyWalk::forEachOwnPropertyAsCreatedInline(vm, source, [&](const PropertyWalk::OwnProperty& property) {
            if ((property.attributes & PropertyAttribute::DontEnum) || excluded.contains(property.name))
                return IterationStatus::Continue;
            if (property.name == vm.propertyNames->underscoreProto.impl())
                return IterationStatus::Done;
            names.append(property.name);
            values.appendWithCrashOnOverflow(property.value);
            return IterationStatus::Continue;
        }, PropertyWalk::Elements::NeedGenericPath, PropertyWalk::Preparations::AreDone);
        return walkResult == PropertyWalk::Result::Completed;
    }
    bool isMaterializedError = source->structure()->classInfoForCells() == ErrorInstance::info() && uncheckedDowncast<ErrorInstance>(source)->hasMaterializedErrorInfo();
    if (isMaterializedError)
        uncheckedDowncast<ErrorInstance>(source)->materializeErrorInfoIfNeeded(vm);
    Structure* structure = source->structure();
    TypeInfo info = structure->typeInfo();
    bool overridesLookups = !isMaterializedError && (info.overridesGetOwnPropertySlot() || info.overridesAnyFormOfGetOwnPropertyNames());
    if (source->hasNonReifiedStaticProperties() || overridesLookups || structure->hasUnderscoreProtoPropertyExcludingOriginalProto() || hasIndexedProperties(structure->indexingType()))
        return false;
    bool symbolsComeLast = !structure->canPerformFastPropertyEnumerationCommon();
    Vector<std::pair<UniquedStringImpl*, PropertyOffset>, 4> symbols;
    bool everyPropertyIsPlain = true;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if ((entry.attributes() & PropertyAttribute::DontEnum) || PropertyName(entry.key()).isPrivateName() || excluded.contains(entry.key()))
            return true;
        everyPropertyIsPlain = !(entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue);
        if (!everyPropertyIsPlain)
            return false;
        if (symbolsComeLast && entry.key()->isSymbol()) {
            symbols.append({ entry.key(), entry.offset() });
            return true;
        }
        names.append(entry.key());
        values.appendWithCrashOnOverflow(source->getDirect(entry.offset()));
        return true;
    });
    if (!everyPropertyIsPlain) {
        names.clear();
        values.clear();
        return false;
    }
    for (auto [symbol, offset] : symbols) {
        names.append(symbol);
        values.appendWithCrashOnOverflow(source->getDirect(offset));
    }
    return true;
}

static bool tryAssignCollectedProperties(JSGlobalObject* globalObject, VM& vm, Object& target, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    static constexpr size_t minNumberOfNames = 4;
    if (names.size() < minNumberOfNames || names.size() > static_cast<size_t>(Structure::s_maxTransitionLength))
        return false;
    for (UniquedStringImpl* name : names) {
        if (parseIndex(PropertyName(name)) || name == vm.propertyNames->underscoreProto.impl())
            return false;
    }
    JSFinalObject* collected = constructEmptyObject(vm, globalObject->objectStructureForObjectConstructor());
    collected->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
    return tryDefinePropertiesOf(vm, target, collected, { });
}

static bool tryCopyPropertiesExcluding(JSGlobalObject* globalObject, VM& vm, JSObject* target, JSObject* source, const ExcludedKeys& excluded)
{
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    EnsureStillAliveScope sourceScope(source);
    if (!tryCollectProperties(vm, source, excluded, names, values))
        return false;
    countEvent(vm, RuntimeCounterID::CopiesCollectedByName);
    Object* record = compiledObjectOrNull(target);
    if (!record) {
        if (names.size())
            target->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
        return true;
    }
    if (record->tryDefineFields(vm, names.span(), { values.data(), values.size() }))
        return true;
    if (tryAssignCollectedProperties(globalObject, vm, *record, names, values))
        return true;
    definePropertiesInOrder(vm, *record, names.mutableSpan(), { values.data(), values.size() });
    return true;
}

namespace {

struct CopiedProperties {
    bool tryAppend(VM& vm, UniquedStringImpl* name, JSValue value)
    {
        if (name == vm.propertyNames->underscoreProto.impl())
            return false;
        names.append(name);
        values.appendWithCrashOnOverflow(value);
        return true;
    }

    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
};

}

static bool tryStoreCopiedProperties(VM& vm, JSObject* target, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    Structure* structure = target->structure();
    if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->hasNonEnumerableProperties() || structure->hasNonConfigurableProperties()) {
        for (UniquedStringImpl* name : names) {
            unsigned attributes;
            if (isValidOffset(structure->get(vm, name, attributes)) && attributes)
                return false;
        }
    }
    if (!names.isEmpty())
        target->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
    return true;
}

static bool tryStoreCopiedProperties(VM& vm, JSObject* target, CopiedProperties& copied)
{
    return tryStoreCopiedProperties(vm, target, copied.names, copied.values);
}

static bool tryCopyDataProperties(VM&, JSObject* target, JSObject* source);

static NEVER_INLINE bool tryCopyDataPropertiesOfDictionary(VM& vm, JSObject* target, JSObject* source)
{
    return flattenOnce(vm, source) && tryCopyDataProperties(vm, target, source);
}

static bool tryCopyDataProperties(VM& vm, JSObject* target, JSObject* source)
{
    Structure* sourceStructure = source->structure();
    if (!takesPlainPropertiesDirectly(target->structure()))
        return false;
    if (sourceStructure->isDictionary()) [[unlikely]]
        return tryCopyDataPropertiesOfDictionary(vm, target, source);
    if (!sourceStructure->canPerformFastPropertyEnumerationCommon() || hasIndexedProperties(sourceStructure->indexingType()))
        return false;
    if (sourceStructure->hasNonReifiedStaticProperties())
        return false;

    CopiedProperties copied;
    bool canCopy = true;
    EnsureStillAliveScope sourceStructureScope(sourceStructure);
    sourceStructure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if ((entry.attributes() & PropertyAttribute::DontEnum) || PropertyName(entry.key()).isPrivateName())
            return true;
        canCopy = !(entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue) && copied.tryAppend(vm, entry.key(), source->getDirect(entry.offset()));
        return canCopy;
    });
    return canCopy && tryStoreCopiedProperties(vm, target, copied);
}

static bool tryCopyRecordDataProperties(VM& vm, JSObject* target, JSObject* object)
{
    Object* record = compiledObjectOrNull(object);
    if (!record || !takesPlainPropertiesDirectly(target->structure()))
        return false;

    CopiedProperties copied;
    bool canCopy = true;
    bool hasOnlyFields = record->forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
        canCopy = copied.tryAppend(vm, &name, value);
        return canCopy;
    }, Object::SymbolKeys::Include);
    return hasOnlyFields && canCopy && tryStoreCopiedProperties(vm, target, copied);
}

static ALWAYS_INLINE bool hasOwnPropertiesToCopy(JSValue source)
{
    if (!source.isObject())
        return source.isString() && asString(source)->length();
    JSObject* object = asObject(source);
    if (object->type() != FinalObjectType)
        return true;
    Structure* structure = object->structure();
    return structure->maxOffset() != invalidOffset || structure->isDictionary() || structure->isCompiledStructure() || hasIndexedProperties(structure->indexingType()) || structure->hasNonReifiedStaticProperties();
}

static NEVER_INLINE void defineCollectedProperties(JSGlobalObject* globalObject, VM& vm, JSObject* target, Object* record, Vector<UniquedStringImpl*, 8>& copiedNames, MarkedArgumentBuffer& copiedValues)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (record && record->tryDefineFields(vm, copiedNames.span(), { copiedValues.data(), copiedValues.size() }))
        return;
    if (record && tryAssignCollectedProperties(globalObject, vm, *record, copiedNames, copiedValues))
        return;
    bool hasIndex = std::ranges::any_of(copiedNames, [](UniquedStringImpl* name) {
        return !!parseIndex(PropertyName(name));
    });
    if (!record && !hasIndex && takesPlainPropertiesDirectly(target->structure()) && !copiedNames.contains(vm.propertyNames->underscoreProto.impl()) && tryStoreCopiedProperties(vm, target, copiedNames, copiedValues))
        return;
    if (record && !hasIndex && !(record->orderWord() & orderWordIntegrityBits)) {
        definePropertiesInOrder(vm, *record, copiedNames.mutableSpan(), { copiedValues.data(), copiedValues.size() });
        RETURN_IF_EXCEPTION(scope, void());
        return;
    }
    for (size_t i = 0; i < copiedNames.size(); ++i) {
        defineCopiedProperty(globalObject, target, copiedNames[i], copiedValues.at(i));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static NEVER_INLINE bool tryCopyCallingGetters(JSGlobalObject* globalObject, VM& vm, JSObject* target, JSObject* from, const ExcludedKeys& excluded)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<RefPtr<UniquedStringImpl>, 32> heldNames;
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    auto isExcluded = [&](UniquedStringImpl* name) {
        return excluded.contains(name);
    };
    ScopedLambda<bool(UniquedStringImpl*)> isLeftOut = isExcluded;
    PropertyWalk::Result result = PropertyWalk::readOwnPropertiesCallingGetters(globalObject, from, heldNames, names, values, { .isLeftOut = excluded.isEmpty() ? nullptr : &isLeftOut });
    RETURN_IF_EXCEPTION(scope, false);
    if (result != PropertyWalk::Result::Completed)
        return false;
    if (!isCompiledObject(target) && takesPlainPropertiesDirectly(target->structure()) && !names.contains(vm.propertyNames->underscoreProto.impl()) && tryStoreCopiedProperties(vm, target, names, values))
        return true;
    RELEASE_AND_RETURN(scope, (defineCollectedProperties(globalObject, vm, target, compiledObjectOrNull(target), names, values), true));
}

static NEVER_INLINE bool tryCopyNamesThenElements(Instance* instance, JSGlobalObject* globalObject, VM& vm, Object& target, JSObject* from, JSObject* holder)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    IndexingType indexingType = holder->indexingType();
    if (!hasInt32(indexingType) && !hasDouble(indexingType) && !hasContiguous(indexingType))
        return false;
    bool holdsOnlyElements = holder->structure() == instance->realm.extraPropertiesWithElementsStructure;
    if (!holdsOnlyElements || !tryDefineFieldsLeavingElementsOut(vm, target, *uncheckedDowncast<Object>(from))) {
        Vector<UniquedStringImpl*, 8> names;
        MarkedArgumentBuffer values;
        auto walked = PropertyWalk::forEachOwnPropertyAsCreatedInline(vm, from, [&](const PropertyWalk::OwnProperty& property) {
            if ((property.attributes & PropertyAttribute::DontEnum) || PropertyName(property.name).isPrivateName())
                return IterationStatus::Continue;
            if (property.name == vm.propertyNames->underscoreProto.impl())
                return IterationStatus::Done;
            names.append(property.name);
            values.appendWithCrashOnOverflow(property.value);
            return IterationStatus::Continue;
        }, PropertyWalk::Elements::Skip);
        if (walked != PropertyWalk::Result::Completed)
            return false;
        if (!names.isEmpty() && !target.tryDefineFields(vm, names.span(), { values.data(), values.size() }) && !tryAssignCollectedProperties(globalObject, vm, target, names, values)) {
            definePropertiesInOrder(vm, target, names.mutableSpan(), { values.data(), values.size() });
            RETURN_IF_EXCEPTION(scope, true);
        }
    }
    if (!target.hasExtraProperties() && holder->structure() == instance->realm.extraPropertiesWithElementsStructure && target.layout().extraPropertiesSlot() && !target.structure()->mayBePrototype() && !target.layout().isClass()) {
        if (JSFinalObject* elements = tryCopyWithElements(vm, holder)) {
            target.setExtraProperties(vm, elements, instance->state->objects->structures.withExtraProperties(vm, target.compiledStructure(), nullptr));
            return true;
        }
    }
    holder->forEachOwnElement<>([&](unsigned index, JSValue value) {
        if (!tryDefineElementOfCompiledObject(instance, &target, index, value))
            defineCopiedProperty(globalObject, &target, Identifier::from(vm, index), value);
        RETURN_IF_EXCEPTION(scope, IterationStatus::Done);
        return IterationStatus::Continue;
    });
    RETURN_IF_EXCEPTION(scope, true);
    return true;
}

static void copyDataPropertiesByAnyWay(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* target, JSValue source, const ExcludedKeys& excluded)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(hasOwnPropertiesToCopy(source));
    JSObject* from;
    if (source.isObject()) [[likely]]
        from = asObject(source);
    else {
        from = source.toObject(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (isCompiledObject(from) && uncheckedDowncast<Object>(from)->layout().hasFieldsWithSymbolKeys) [[unlikely]] {
        prepareForWalk(globalObject, from);
        RETURN_IF_EXCEPTION(scope, void());
    }
    Object* record = compiledObjectOrNull(target);
    if (record) {
        Object* other = excluded.isEmpty() ? compiledObjectOrNull(from) : nullptr;
        if (other && record->shapeWord() == other->shapeWord() && record->tryCopyFieldsOf(vm, *other))
            return;
        if (other && other->hasExtraProperties() && !(record->orderWord() & orderWordIntegrityBits)) {
            JSObject* holder = PropertyWalk::objectHoldingIndexedProperties(other);
            if (holder && hasIndexedProperties(holder->indexingType())) {
                bool didCopy = tryCopyNamesThenElements(instance, globalObject, vm, *record, from, holder);
                RETURN_IF_EXCEPTION(scope, void());
                if (didCopy)
                    return;
            }
        }
        if (tryDefinePropertiesOf(vm, *record, from, excluded.names()))
            return;
    }
    if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType) {
        bool mayTakeStructure = !target->structure()->isCompiledStructure() && !from->structure()->isCompiledStructure() && !from->hasNonReifiedStaticProperties() && from->structure()->canPerformFastPropertyEnumerationCommon();
        if (mayTakeStructure && objectCloneFast(vm, uncheckedDowncast<JSFinalObject>(target), from))
            return;
    }
    if (takesPlainPropertiesDirectly(target->structure()) && tryDefinePropertiesOnOrdinaryObject(vm, *instance->state, target, from, excluded.names()))
        return;
    if (isCompiledObject(from) && PropertyWalk::hasGetterToCall(from)) {
        auto isExcluded = [&](UniquedStringImpl* name) {
            return excluded.contains(name);
        };
        ScopedLambda<bool(UniquedStringImpl*)> isLeftOut = isExcluded;
        bool isEmptyOrdinaryObject = !excluded.isEmpty() && target->structure() == globalObject->objectStructureForObjectConstructor();
        JSObject* read = PropertyWalk::tryCopyCallingGetters(globalObject, from, PropertyWalk::NewObjectTakes::Definitions, excluded.isEmpty() ? nullptr : &isLeftOut, isEmptyOrdinaryObject ? target : nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        if (read == target)
            return;
        if (read) {
            if (!hasOwnPropertiesToCopy(read))
                return;
            if (record && tryRepeatAssignment(vm, *instance->state, target, read, 0) != RepeatedCopy::IsNotDone)
                return;
            ExcludedKeys nothing;
            RELEASE_AND_RETURN(scope, copyDataPropertiesByAnyWay(instance, globalObject, vm, target, read, nothing));
        }
        bool didCopy = tryCopyCallingGetters(globalObject, vm, target, from, excluded);
        RETURN_IF_EXCEPTION(scope, void());
        if (didCopy)
            return;
    }
    if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType) {
        if (tryCopyDataProperties(vm, target, from))
            return;
    }
    if (excluded.isEmpty() && target->type() == FinalObjectType && tryCopyRecordDataProperties(vm, target, from))
        return;
    if (from->hasNonReifiedStaticProperties()) {
        from->reifyAllStaticProperties(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }
    auto* ordinary = dynamicDowncast<JSFinalObject>(target);
    bool canPutDirectly = ordinary && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible();
    JSObject* holderOfElements = isCompiledObject(from) ? PropertyWalk::objectHoldingIndexedProperties(from) : from;
    bool hasElements = holderOfElements && hasIndexedProperties(holderOfElements->indexingType());
    if (canPutDirectly && excluded.isEmpty() && !hasElements) {
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer propertyValues;
        bool isDone = objectAssignFast(globalObject, ordinary, from, properties, propertyValues);
        RETURN_IF_EXCEPTION(scope, void());
        if (isDone)
            return;
    }
    bool canDefineDirectly = record ? !(record->orderWord() & orderWordIntegrityBits) : canPutDirectly;
    if (canDefineDirectly) {
        bool didCopy = tryCopyPropertiesExcluding(globalObject, vm, target, from, excluded);
        RETURN_IF_EXCEPTION(scope, void());
        if (didCopy)
            return;
    }
    Vector<UniquedStringImpl*, 8> copiedNames;
    MarkedArgumentBuffer copiedValues;
    auto defineCopiedProperties = [&] {
        return defineCollectedProperties(globalObject, vm, target, record, copiedNames, copiedValues);
    };
    if (auto* moduleNamespace = excluded.isEmpty() ? dynamicDowncast<ModuleNamespace>(from) : nullptr) {
        bool didReadExports = moduleNamespace->tryReadExports(globalObject, copiedNames, copiedValues);
        RETURN_IF_EXCEPTION(scope, void());
        if (didReadExports)
            RELEASE_AND_RETURN(scope, defineCopiedProperties());
    }
    countEvent(vm, RuntimeCounterID::CopiesByOrdinaryLookups);
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

static ExcludedRun excludedRunAt(Instance* instance, uint32_t numberPlusOne)
{
    if (!numberPlusOne)
        return { };
    return { numberPlusOne, instance->tables().propertyRunRow(numberPlusOne - 1).subspan(1) };
}

static ALWAYS_INLINE void copyDataPropertiesAsBeforeOrByAnyWay(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasOwnPropertiesToCopy(JSValue::decode(source)))
        return;
    JSObject* object = cellArgument<JSObject>(target, Entry::operationAOTCopyDataProperties);
    if (!count && isCompiledObject(object) && tryRepeatAssignment(vm, *instance->state, object, JSValue::decode(source), excludedRun) != RepeatedCopy::IsNotDone)
        return;
    ExcludedKeys excluded;
    JSObject* read = nullptr;
    if (Object* from = !count && isCompiledObject(object) ? compiledObjectOrNull(JSValue::decode(source)) : nullptr) {
        ExcludedRun run = excludedRunAt(instance, excludedRun);
        read = PropertyWalk::tryCopyCallingGettersAsBefore(globalObject, from, run.numberPlusOne, run.numberPlusOne ? instance->state->objects->caches->assignmentPlans.excludedNames(run).names() : std::span<UniquedStringImpl* const> { });
        RETURN_IF_EXCEPTION(scope, void());
        if (read && (!hasOwnPropertiesToCopy(read) || tryRepeatAssignment(vm, *instance->state, object, read, 0) != RepeatedCopy::IsNotDone))
            return;
    }
    if (read)
        source = JSValue::encode(read);
    else if (excludedRun || count) {
        excluded.collect(instance, globalObject, excludedRunAt(instance, excludedRun), { excludedKeys, count });
        RETURN_IF_EXCEPTION(scope, void());
    }
    scope.release();
    countEvent(vm, RuntimeCounterID::CopiesNotRepeated);
    copyDataPropertiesByAnyWay(instance, globalObject, vm, object, JSValue::decode(source), excluded);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyDataProperties, void, (Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyDataProperties);
    if (Object* from = compiledObjectOrNull(JSValue::decode(source)); from && from->bearsMark() && !(from->orderWord() & orderWordHasFieldOutsideSlot)) [[unlikely]]
        from->tryFinishBirth();
    scope.release();
    copyDataPropertiesAsBeforeOrByAnyWay(instance, globalObject, vm, target, source, excludedRun, count, excludedKeys);
    OPERATION_RETURN(scope);
}

static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES copyDataPropertiesFollowingPlan(Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys)
{
    if (!count && tryCopyFollowingPlanWithoutFrame(*instance, target, source, excludedRun))
        return { nullptr };
    AOT_MUST_TAIL_CALL return operationAOTCopyDataProperties(instance, target, source, excludedRun, count, excludedKeys);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyDataPropertiesByFieldMap, void, (Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    if (!count && tryCopyByFieldMapWithoutFrame(*instance, target, source, excludedRun)) [[likely]]
        return { nullptr };
    AOT_MUST_TAIL_CALL return copyDataPropertiesFollowingPlan(instance, target, source, excludedRun, count, excludedKeys);
}

static JSObject* tryCopyWithoutNames(VM& vm, JSGlobalObject* globalObject, JSObject* source, ExcludedRun excluded)
{
    bool canHaveStructureOfSource = !excluded.numberPlusOne && source->type() == FinalObjectType && (!source->structure()->isDictionary() || flattenOnceUnlessCopiedAsDictionary(vm, source)) && source->structure()->canPerformFastPropertyEnumerationCommon();
    if (JSObject* copy = canHaveStructureOfSource ? tryCreateObjectViaCloning(vm, globalObject, source) : nullptr)
        return copy;
    Structure* structure = source->structure();
    bool isPlainEmptyObject = canHaveStructureOfSource && !structure->didTransition() && !structure->seenProperties().bits() && !source->butterfly() && checkStructureForClone(structure)
        && structure->realm() == globalObject && structure->storedPrototype() == globalObject->objectPrototype();
    if (isPlainEmptyObject)
        return JSFinalObject::create(vm, structure);
    if (hasIndexedProperties(structure->indexingType()))
        return nullptr;
    return tryCopyAsOrdinaryObject(vm, globalObject, source, excluded);
}

// Out of line: the operation keeps nothing alive across the calls in here.
static NEVER_INLINE JSObject* tryCopyCompiledObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, Object& source)
{
    if (Object* copy = &source.state() == instance->state ? Object::tryCreateCopyOfFields(vm, source) : nullptr)
        return copy;
    return tryCopyAsOrdinaryObject(vm, globalObject, &source, ExcludedRun { });
}

static NEVER_INLINE JSObject* copyObjectByNames(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue source, ExcludedRun run, std::span<const EncodedJSValue> excludedKeys)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (run.numberPlusOne && excludedKeys.empty() && source.isObject() && isCompiledObject(asObject(source))) {
        JSObject* rest = PropertyWalk::tryCopyCallingGettersAsBefore(globalObject, asObject(source), run.numberPlusOne, instance->state->objects->caches->assignmentPlans.excludedNames(run).names());
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (rest)
            return rest;
    }
    if (!run.numberPlusOne && excludedKeys.empty() && source.isObject() && isCompiledObject(asObject(source)) && PropertyWalk::hasGetterToCall(asObject(source))) {
        JSObject* read = PropertyWalk::tryCopyCallingGetters(globalObject, asObject(source), PropertyWalk::NewObjectTakes::Definitions);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (read)
            return read;
    }
    if (!run.numberPlusOne && excludedKeys.empty() && source.isObject()) {
        JSObject* object = asObject(source);
        Structure* structure = object->structure();
        JSFinalObject* copy = structure->realm() == globalObject && structure->storedPrototype() == JSValue(globalObject->objectPrototype()) ? tryCopyWithElements(vm, object) : nullptr;
        if (copy)
            return copy;
    }
    JSObject* copy = constructEmptyObject(globalObject);
    if (!hasOwnPropertiesToCopy(source)) {
        prepareFinalObjectAllocation(instance, InlineAllocation::DefaultFinalObject, globalObject->objectStructureForObjectConstructor());
        return copy;
    }
    ExcludedKeys excluded;
    excluded.collect(instance, globalObject, run, excludedKeys);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, (copyDataPropertiesByAnyWay(instance, globalObject, vm, copy, source, excluded), copy));
}

static ALWAYS_INLINE JSObject* copyObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys)
{
    Object* compiled = !count && !excludedRun && source.isObject() ? compiledObjectOrNull(source) : nullptr;
    if (compiled) {
        if (JSObject* copy = tryCopyCompiledObject(instance, globalObject, vm, *compiled)) [[likely]]
            return copy;
    }
    ExcludedRun run = excludedRunAt(instance, excludedRun);
    if (!compiled && !count && source.isObject()) {
        if (JSObject* copy = tryCopyWithoutNames(vm, globalObject, asObject(source), run))
            return copy;
    }
    return copyObjectByNames(instance, globalObject, vm, source, run, { excludedKeys, count });
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyObject, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyObject);
    JSObject* copy = copyObject(instance, globalObject, vm, JSValue::decode(encodedSource), excludedRun, count, excludedKeys);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, copy);
}

static bool hasNoGetterAndNoProto(JSObject* ordinary)
{
    Structure* structure = ordinary->structure();
    return ordinary->type() == FinalObjectType && !structure->hasAnyKindOfGetterSetterProperties() && !structure->hasUnderscoreProtoPropertyExcludingOriginalProto()
        && !ordinary->canHaveExistingOwnIndexedGetterSetterProperties() && !ordinary->hasNonReifiedStaticProperties();
}

static bool isAssignedAsCopied(VM& vm, JSGlobalObject* globalObject, JSValue source)
{
    Structure* above = globalObject->objectPrototype()->structure();
    if (above->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || above->mayInterceptIndexedAccesses())
        return false;
    if (!source.isObject())
        return true;
    JSObject* holderOfElements = isCompiledObject(asObject(source)) ? PropertyWalk::objectHoldingIndexedProperties(asObject(source)) : asObject(source);
    if (holderOfElements && hasIndexedProperties(holderOfElements->indexingType()))
        return false;
    Object* compiled = compiledObjectOrNull(source);
    if (!compiled)
        return hasNoGetterAndNoProto(asObject(source));
    if (std::ranges::any_of(compiled->layout().fields(), &FieldData::isAccessor) || PropertyWalk::hasOwnProperty(compiled, vm.propertyNames->underscoreProto.impl()) != PropertyWalk::Presence::Absent)
        return false;
    JSObject* extra = compiled->extraProperties();
    return !extra || hasNoGetterAndNoProto(extra);
}

static NEVER_INLINE JSObject* assignToNewObject(JSGlobalObject* globalObject, VM& vm, JSValue source)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (source.isObject() && isCompiledObject(asObject(source)) && PropertyWalk::hasGetterToCall(asObject(source))) {
        JSObject* copy = PropertyWalk::tryCopyCallingGetters(globalObject, asObject(source), PropertyWalk::NewObjectTakes::Stores);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (copy)
            return copy;
    }
    JSFinalObject* target = constructEmptyObject(globalObject);
    if (source.isUndefinedOrNull())
        return target;
    JSObject* from = source.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool didAssign = PropertyWalk::tryAssignCallingGetters(globalObject, target, from);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (didAssign)
        return target;
    if (target->canPerformFastPutInlineExcludingProto()) {
        if (!from->staticPropertiesReified()) {
            from->reifyAllStaticProperties(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        Vector<UniquedStringImpl*, 8> names;
        MarkedArgumentBuffer values;
        didAssign = objectAssignFast(globalObject, target, from, names, values);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (didAssign)
            return target;
    }
    objectAssignGeneric(globalObject, vm, target, from);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return target;
}

JSC_DEFINE_JIT_OPERATION(operationAOTAssignToNewObject, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAssignToNewObject);
    JSValue source = JSValue::decode(encodedSource);
    JSObject* made = isAssignedAsCopied(vm, globalObject, source) ? copyObject(instance, globalObject, vm, source, excludedRun, count, excludedKeys) : assignToNewObject(globalObject, vm, source);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, made);
}

static bool isCopiedWithItsStructure(JSGlobalObject* globalObject, Structure* structure)
{
    return checkStructureForClone(structure) && structure->canPerformFastPropertyEnumerationCommon() && structure->realm() == globalObject && structure->storedPrototype() == JSValue(globalObject->objectPrototype());
}

static void rememberCopyWithNames(Instance* instance, VM& vm, Structure* structureOfSource, JSObject* copy, std::span<const EncodedJSValue> names)
{
    static_assert(MarkedSpace::largeCutoff <= UINT16_MAX);
    Structure* structure = copy->structure();
    if (structure->typeInfo().type() != FinalObjectType || structure->isDictionary() || hasIndexedProperties(structure->indexingType()) || structure->inlineCapacity() != structureOfSource->inlineCapacity())
        return;
    size_t cellSize = roundUpToMultipleOf<MarkedSpace::sizeStep>(JSFinalObject::allocationSize(structure->inlineCapacity()));
    size_t storageSizeOfSource = structureOfSource->outOfLineCapacity() * sizeof(EncodedJSValue);
    size_t storageSize = structure->outOfLineCapacity() * sizeof(EncodedJSValue);
    if (cellSize > MarkedSpace::largeCutoff || storageSize > MarkedSpace::largeCutoff || storageSize < storageSizeOfSource || names.size() > UINT16_MAX)
        return;

    std::span<uint8_t> table { instance->objects.copiesWithNames, sizeOfCopiesWithNames };
    auto wordAt = [&](uint32_t offset) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(table.subspan(offset, sizeof(uint32_t)).data());
    };
    auto recordAt = [&](uint32_t offset) -> CopyWithNames& {
        return *reinterpret_cast<CopyWithNames*>(table.subspan(offset, sizeof(CopyWithNames)).data());
    };
    uint32_t offsetOfPlace = placeOfCopyWithNames(structureOfSource->id().bits(), static_cast<uint32_t>(names[0])) * sizeof(uint32_t);
    if (wordAt(offsetOfPlace) && (++instance->objects.copiesThatFoundTheirPlaceOccupied & copiesThatTakeAnOccupiedPlaceMask))
        return;

    Vector<NameInCopy, 8> namesInCopy;
    for (EncodedJSValue name : names) {
        uint32_t number = static_cast<uint32_t>(name);
        UniquedStringImpl* uid = identifierAt(instance, number).impl();
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, uid, attributes);
        if (!isValidOffset(offset) || attributes)
            return;
        if (isValidOffset(structureOfSource->get(vm, uid)))
            structure->didCachePropertyReplacement(vm, offset);
        namesInCopy.append({ number, offsetRelativeToBase(offset) });
    }

    uint32_t size = sizeof(CopyWithNames) + namesInCopy.size() * sizeof(NameInCopy);
    if (offsetOfFirstCopyWithNames + size > sizeOfCopiesWithNames)
        return;
    uint32_t offset = wordAt(offsetOfPlace);
    if (!offset || recordAt(offset).count != namesInCopy.size()) {
        if (offsetOfFirstCopyWithNames + wordAt(offsetOfTakenBytesOfCopiesWithNames) + size > sizeOfCopiesWithNames)
            zeroSpan(table);
        offset = offsetOfFirstCopyWithNames + wordAt(offsetOfTakenBytesOfCopiesWithNames);
        wordAt(offsetOfTakenBytesOfCopiesWithNames) += size;
    }
    recordAt(offset) = {
        .structureOfSource = structureOfSource->id().bits(),
        .firstName = namesInCopy[0].name,
        .header = cellHeaderFor(structure),
        .count = static_cast<uint16_t>(namesInCopy.size()),
        .cellSize = static_cast<uint16_t>(cellSize),
        .storageSizeOfSource = static_cast<uint16_t>(storageSizeOfSource),
        .storageSize = static_cast<uint16_t>(storageSize),
    };
    memcpySpan(table.subspan(offset + sizeof(CopyWithNames), namesInCopy.size() * sizeof(NameInCopy)), asBytes(namesInCopy.span()));
    wordAt(offsetOfPlace) = offset;
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyObjectAndDefine, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t, uint32_t count, const EncodedJSValue* list))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyObjectAndDefine);
    JSValue source = JSValue::decode(encodedSource);
    JSObject* copy = nullptr;
    Object* compiled = source.isObject() ? compiledObjectOrNull(source) : nullptr;
    if (compiled)
        copy = tryCopyCompiledObject(instance, globalObject, vm, *compiled);
    else if (source.isObject())
        copy = tryCopyWithoutNames(vm, globalObject, asObject(source), ExcludedRun { });
    if (!copy) {
        copy = copyObjectByNames(instance, globalObject, vm, source, ExcludedRun { }, { });
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }

    std::span<const EncodedJSValue> names { list + 1, count };
    std::span<const EncodedJSValue> values { list + 1 + count, count };
    Structure* structureOfCopy = copy->structure();
    bool isCopiedByStub = !compiled && source.isObject() && asObject(source)->structure() == structureOfCopy && isCopiedWithItsStructure(globalObject, structureOfCopy);
    Vector<UniquedStringImpl*, 8> definedNames;
    MarkedArgumentBuffer definedValues;
    for (size_t index = 0; index < count; ++index) {
        definedNames.append(identifierAt(instance, static_cast<uint32_t>(names[index])).impl());
        definedValues.appendWithCrashOnOverflow(JSValue::decode(values[index]));
    }
    defineCollectedProperties(globalObject, vm, copy, compiledObjectOrNull(copy), definedNames, definedValues);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (isCopiedByStub)
        rememberCopyWithNames(instance, vm, structureOfCopy, copy, names);
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryCopyTwoObjects, JSCell*, (Instance* instance, EncodedJSValue first, EncodedJSValue second))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryCopyTwoObjects);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, tryCopyTwoObjects(vm, *instance->state, globalObject, JSValue::decode(first), JSValue::decode(second)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
