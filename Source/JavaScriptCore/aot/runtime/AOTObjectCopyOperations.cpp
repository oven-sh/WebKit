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
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTThreshold.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "ButterflyInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

// The keys that a copy leaves out.
class ExcludedKeys {
    WTF_MAKE_NONCOPYABLE(ExcludedKeys);
public:
    ExcludedKeys() = default;

    void collect(Instance* instance, JSGlobalObject* globalObject, ExcludedRun run, std::span<const EncodedJSValue> keys)
    {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (run.numberPlusOne) {
            m_listOfRun = &instance->state->objects->caches->assignmentPlans.excludedNames(vm, run);
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
    TypeInfo info = structure->typeInfo();
    if (source->hasNonReifiedStaticProperties() || info.overridesGetOwnPropertySlot() || info.overridesAnyFormOfGetOwnPropertyNames() || structure->hasUnderscoreProtoPropertyExcludingOriginalProto() || hasIndexedProperties(structure->indexingType()))
        return false;
    bool everyPropertyIsPlain = true;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if ((entry.attributes() & PropertyAttribute::DontEnum) || PropertyName(entry.key()).isPrivateName() || excluded.contains(entry.key()))
            return true;
        everyPropertyIsPlain = !(entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue);
        if (!everyPropertyIsPlain)
            return false;
        names.append(entry.key());
        values.appendWithCrashOnOverflow(source->getDirect(entry.offset()));
        return true;
    });
    if (!everyPropertyIsPlain) {
        names.clear();
        values.clear();
    }
    return everyPropertyIsPlain;
}

// What has been read out of a source of any kind is put into an ordinary object, which gets the same Structure each time the names are the same.
static bool tryAssignCollectedProperties(JSGlobalObject* globalObject, VM& vm, Object& target, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    AOT_THRESHOLD(size_t, minNumberOfNames, 4);
    // With more names than a chain of Structures holds the object would be a dictionary, which has no plan.
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

static std::optional<ASCIILiteral> tryCopyPropertiesExcluding(JSGlobalObject* globalObject, VM& vm, JSObject* target, JSObject* source, const ExcludedKeys& excluded)
{
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    EnsureStillAliveScope sourceScope(source);
    if (!tryCollectProperties(vm, source, excluded, names, values))
        return std::nullopt;
    count(vm, RuntimeCounterID::CopiesCollectedByName);
    Object* record = compiledObjectOrNull(target);
    if (!record) {
        if (names.size())
            target->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
        return "collected, put as a batch"_s;
    }
    if (record->tryDefineFields(vm, names.span(), { values.data(), values.size() }))
        return "collected, all are fields"_s;
    if (tryAssignCollectedProperties(globalObject, vm, *record, names, values))
        return "collected, then by the plan of an ordinary object"_s;
    definePropertiesInOrder(vm, *record, names.mutableSpan(), { values.data(), values.size() });
    return "collected, put name by name"_s;
}

// An ordinary object with such a Structure gets a plain property by a store and, for a new name, the next Structure. Nobody has to be told.
static bool takesPlainPropertiesDirectly(Structure* target)
{
    return target->typeInfo().type() == FinalObjectType
        && !target->isCompiledStructure()
        && !target->mayHaveImmutableProperties()
        && !target->mayBePrototype()
        && !target->isWatchingReplacement()
        && !target->isDictionary()
        && target->isStructureExtensible()
        && !target->hasPolyProto();
}

// Of the primitives only a string has own properties. Options that nobody has given are an empty literal.
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

// Answers which way the copy went, for a run that counts.
static ASCIILiteral copyDataPropertiesByAnyWay(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* target, JSValue source, const ExcludedKeys& excluded)
{
    constexpr ASCIILiteral threw = "threw"_s;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasOwnPropertiesToCopy(source))
        return "nothing to copy"_s;
    JSObject* from = source.isObject() ? asObject(source) : source.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, threw);
    Object* record = compiledObjectOrNull(target);
    if (record) {
        Object* other = excluded.isEmpty() ? compiledObjectOrNull(from) : nullptr;
        // An object of the target's own layout is copied word by word.
        if (other && record->shapeWord() == other->shapeWord() && record->tryCopyFieldsOf(vm, *other))
            return "fields by words or moves"_s;
        if (tryDefinePropertiesOf(vm, *record, from, excluded.names()))
            return "by a plan"_s;
    }
    if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType) {
        // An ordinary object without properties takes the Structure of an ordinary source and its values word by word, as in the engine's own copy.
        bool mayTakeStructure = !target->structure()->isCompiledStructure() && !from->structure()->isCompiledStructure() && !from->hasNonReifiedStaticProperties() && from->structure()->canPerformFastPropertyEnumerationCommon();
        if (mayTakeStructure && objectCloneFast(vm, uncheckedDowncast<JSFinalObject>(target), from))
            return "ordinary objects, Structure and words"_s;
    }
    if (takesPlainPropertiesDirectly(target->structure()) && tryDefinePropertiesOnOrdinaryObject(vm, *instance->state, target, from, excluded.names()))
        return "into an ordinary object, by a plan"_s;
    auto* ordinary = dynamicDowncast<JSFinalObject>(target);
    bool canPutDirectly = ordinary && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible();
    if (canPutDirectly && excluded.isEmpty()) {
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer propertyValues;
        bool isDone = objectAssignFast(globalObject, ordinary, from, properties, propertyValues);
        RETURN_IF_EXCEPTION(scope, threw);
        if (isDone)
            return "into an ordinary object, objectAssignFast"_s;
    }
    bool canDefineDirectly = record ? !(record->orderWord() & orderWordIntegrityBits) : canPutDirectly;
    if (canDefineDirectly) {
        std::optional<ASCIILiteral> way = tryCopyPropertiesExcluding(globalObject, vm, target, from, excluded);
        RETURN_IF_EXCEPTION(scope, threw);
        if (way)
            return *way;
    }
    // Nothing sees the target before the copy is complete, so what the source runs cannot tell when the properties are defined.
    Vector<UniquedStringImpl*, 8> copiedNames;
    MarkedArgumentBuffer copiedValues;
    auto defineCopiedProperties = [&]() -> ASCIILiteral {
        if (record && record->tryDefineFields(vm, copiedNames.span(), { copiedValues.data(), copiedValues.size() }))
            return "read by lookups, all are fields"_s;
        if (record && tryAssignCollectedProperties(globalObject, vm, *record, copiedNames, copiedValues))
            return "read by lookups, then by a plan"_s;
        // An element is no property that a definition by name knows.
        bool hasIndex = std::ranges::any_of(copiedNames, [](UniquedStringImpl* name) {
            return !!parseIndex(PropertyName(name));
        });
        if (record && !hasIndex && !(record->orderWord() & orderWordIntegrityBits)) {
            definePropertiesInOrder(vm, *record, copiedNames.mutableSpan(), { copiedValues.data(), copiedValues.size() });
            RETURN_IF_EXCEPTION(scope, threw);
            return "read by lookups, defined name by name"_s;
        }
        for (size_t i = 0; i < copiedNames.size(); ++i) {
            target->createDataProperty(globalObject, copiedNames[i], copiedValues.at(i), true);
            RETURN_IF_EXCEPTION(scope, threw);
        }
        return "read by lookups, defined name by name"_s;
    };
    if (auto* moduleNamespace = excluded.isEmpty() ? dynamicDowncast<ModuleNamespace>(from) : nullptr) {
        bool didReadExports = moduleNamespace->tryReadExports(globalObject, copiedNames, copiedValues);
        RETURN_IF_EXCEPTION(scope, threw);
        if (didReadExports)
            RELEASE_AND_RETURN(scope, defineCopiedProperties());
    }
    count(vm, RuntimeCounterID::CopiesByOrdinaryLookups);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    from->methodTable()->getOwnPropertyNames(from, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, threw);
    for (auto& name : names) {
        if (excluded.contains(name.impl()))
            continue;
        PropertySlot slot(from, PropertySlot::InternalMethodType::GetOwnProperty);
        bool has = from->methodTable()->getOwnPropertySlot(from, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, threw);
        if (!has || slot.attributes() & PropertyAttribute::DontEnum)
            continue;
        bool needsReread = slot.isTaintedByOpaqueObject() && !isModuleNamespace(from);
        JSValue value = needsReread ? from->get(globalObject, name) : slot.getValue(globalObject, name);
        RETURN_IF_EXCEPTION(scope, threw);
        copiedNames.append(name.impl());
        copiedValues.appendWithCrashOnOverflow(value);
    }
    RELEASE_AND_RETURN(scope, defineCopiedProperties());
}

static ALWAYS_INLINE void copyDataPropertiesExcluding(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* target, JSValue source, const ExcludedKeys& excluded)
{
    AssignmentPlanCache& plans = instance->state->objects->caches->assignmentPlans;
    plans.noteCopy(copyDataPropertiesByAnyWay(instance, globalObject, vm, target, source, excluded));
}

static ExcludedRun excludedRunAt(Instance* instance, uint32_t numberPlusOne)
{
    if (!numberPlusOne)
        return { };
    return { numberPlusOne, instance->tables().propertyRunRow(numberPlusOne - 1).subspan(1) };
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyDataProperties, void, (Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyDataProperties);
    if (!hasOwnPropertiesToCopy(JSValue::decode(source)))
        OPERATION_RETURN(scope);
    JSObject* object = cellArgument<JSObject>(target, Entry::operationAOTCopyDataProperties);
    AssignmentPlanCache& plans = instance->state->objects->caches->assignmentPlans;
    if (RepeatedCopy way = !count && isCompiledObject(object) ? tryRepeatAssignment(vm, *instance->state, object, JSValue::decode(source), excludedRun) : RepeatedCopy::IsNotDone; way != RepeatedCopy::IsNotDone) {
        if (plans.countsCopies()) [[unlikely]] {
            constexpr std::array<ASCIILiteral, 5> names { ""_s, "fields by words or moves"_s, "by the map of two layouts"_s, "by the map of two layouts, the first of its kind"_s, "as the copy before"_s };
            plans.noteCopy(names[static_cast<size_t>(way)]);
        }
        OPERATION_RETURN(scope);
    }
    ExcludedKeys excluded;
    excluded.collect(instance, globalObject, excludedRunAt(instance, excludedRun), { excludedKeys, count });
    OPERATION_RETURN_IF_EXCEPTION(scope);
    scope.release();
    copyDataPropertiesExcluding(instance, globalObject, vm, object, JSValue::decode(source), excluded);
    OPERATION_RETURN(scope);
}

static JSObject* tryCopyWithoutNames(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSObject* source, ExcludedRun excluded)
{
    bool canHaveStructureOfSource = !excluded.numberPlusOne && source->type() == FinalObjectType && source->structure()->canPerformFastPropertyEnumerationCommon();
    if (JSObject* copy = canHaveStructureOfSource ? tryCreateObjectViaCloning(vm, globalObject, source) : nullptr) {
        if (copy->structureID() == source->structureID() && !source->butterfly()) {
            ClonedStructure& cloned = instance->allocation.clonedStructures[AllocationWords::placeOfClonedStructure(copy->structureID().bits())];
            cloned.cellSize = JSFinalObject::allocationSize(copy->structure()->inlineCapacity());
            cloned.header = *std::bit_cast<const uint64_t*>(copy);
        }
        return copy;
    }
    return tryCopyAsOrdinaryObject(vm, globalObject, source, excluded);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyObject, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyObject);
    JSValue source = JSValue::decode(encodedSource);
    ExcludedRun run = excludedRunAt(instance, excludedRun);
    // A copy of a record is a record: the names have the same places in it, and whoever expects the type of the source finds it. Flow counts on nothing in between: the words of the source, or an ordinary object.
    if (!count && !excludedRun && source.isObject()) {
        Object* record = compiledObjectOrNull(source);
        if (Object* copy = record && &record->state() == instance->state ? Object::tryCreateCopyOfFields(vm, *record) : nullptr)
            OPERATION_RETURN(scope, copy);
    }
    if (!count && source.isObject()) {
        if (JSObject* copy = tryCopyWithoutNames(instance, vm, globalObject, asObject(source), run))
            OPERATION_RETURN(scope, copy);
    }
    JSObject* copy = constructEmptyObject(globalObject);
    if (!hasOwnPropertiesToCopy(source))
        OPERATION_RETURN(scope, copy);
    ExcludedKeys excluded;
    excluded.collect(instance, globalObject, run, { excludedKeys, count });
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    copyDataPropertiesExcluding(instance, globalObject, vm, copy, source, excluded);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryCopyTwoObjects, JSCell*, (Instance* instance, EncodedJSValue first, EncodedJSValue second))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryCopyTwoObjects);
    DeferTerminationForAWhile deferTermination(vm);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    AssignmentPlanCache& plans = instance->state->objects->caches->assignmentPlans;
    JSFinalObject* copy = tryCopyTwoObjects(vm, *instance->state, globalObject, JSValue::decode(first), JSValue::decode(second));
    if (copy)
        plans.noteCopy("two at once, by a plan"_s);
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCloneObjectAs, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t requestOffset, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCloneObjectAs);
    DeferTerminationForAWhile deferTermination(vm);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
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
    copyDataPropertiesExcluding(instance, globalObject, vm, copy, source, ExcludedKeys { });
    scope.assertNoException();
    for (uint32_t i = 0; i < request.count; ++i) {
        JSValue value = JSValue::decode(values[i]);
        if (value.isUndefined())
            continue;
        copy->createDataProperty(globalObject, identifierAt(instance, layout.fields()[request.fields[i]].name), value, true);
        scope.assertNoException();
    }
    OPERATION_RETURN(scope, copy);
}

static int32_t placeOf(PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return static_cast<int32_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue));
    return static_cast<int32_t>(offsetInButterfly(offset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)));
}

static SourcePlans::Words createSourcePlan(VM& vm, Instance& instance, JSCell* source, const CloneAsRequest& request)
{
    SourcePlans::Words plan = SourcePlans::createUnusable(request.count);
    Structure* structure = source->structure();
    if (!isPlainDataObject(source) || structure->realm() != instance.globalObject)
        return plan;
    ObjectModel& objects = *instance.state->objects;
    const LayoutData& layout = objects.layouts.layoutForShape(request.shape);
    if (layout.kind() != LayoutKind::Record)
        return plan;

    constexpr uint32_t notRequested = UINT32_MAX;
    Vector<uint32_t, 16> memberOfField(FillWith { }, layout.numberOfFields, notRequested);
    for (uint32_t member = 0; member < request.count; ++member)
        memberOfField[request.fields[member]] = member;

    uint32_t order = unorderedOrderWord;
    bool everyKeyIsRequested = true;
    structure->forEachProperty(vm, [&](const auto& entry) {
        const FieldData* field = objects.layouts.field(vm, layout, entry.key());
        uint32_t member = field && field->slot.representation() != SlotRepresentation::DeferredClosure ? memberOfField[layout.indexOf(*field)] : notRequested;
        std::optional<uint32_t> next = order;
        if (member != notRequested && !layout.keysAreNeverEnumerated)
            next = objects.keyOrders.orderAfterCreating(layout, order, layout.indexOf(*field));
        if (member == notRequested || !next) {
            everyKeyIsRequested = false;
            return false;
        }
        order = *next;
        plan[SourcePlan::firstPlaceWord + member] = static_cast<uint32_t>(placeOf(entry.offset()));
        return true;
    });
    if (!everyKeyIsRequested)
        return plan;
    for (uint32_t member = 0; member < request.count; ++member) {
        bool isAbsent = plan[SourcePlan::firstPlaceWord + member] == static_cast<uint32_t>(SourcePlan::absent);
        if (isAbsent && !(layout.fields()[request.fields[member]].flags & fieldIsOptional))
            return plan;
    }
    plan[SourcePlan::orderWord] = order;
    plan[SourcePlan::isUsableWord] = 1;
    return plan;
}

// The plan, and the object that it is a plan of: the source, or what a spread of a compiled source gives where a spread runs no code.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindSourcePlan, UGPRPair, (Instance* instance, JSCell* source, uint32_t requestOffset))
{
    SourcePlans& plans = instance->state->objects->caches->sourcePlans;
    auto planOf = [](const uint32_t* plan, JSCell* object) {
        return makeUGPRPair(std::bit_cast<UCPURegister>(plan), std::bit_cast<UCPURegister>(object));
    };
    if (const uint32_t* plan = plans.find(source->structure(), requestOffset)) [[likely]]
        return planOf(plan, source);

    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFindSourcePlan);
    DeferTerminationForAWhile deferTermination(vm);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    if (compiledObjectOrNull(source)) {
        if (JSFinalObject* copy = tryCopyAsOrdinaryObject(vm, globalObject, asObject(source), { }))
            source = copy;
        if (const uint32_t* plan = plans.find(source->structure(), requestOffset))
            return planOf(plan, source);
    }
    DeferGCForAWhile deferGC(vm);
    const CloneAsRequest& request = requestAt<CloneAsRequest>(instance, requestOffset);
    // Of a compiled object that no spread copies so, nothing is remembered: one Structure stands for objects with other keys.
    if (compiledObjectOrNull(source))
        return planOf(plans.planThatNeverApplies(request.count), source);
    return planOf(plans.add(source->structure(), requestOffset, request.count, createSourcePlan(vm, *instance, source, request)), source);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsPlainDataObject, size_t, (Instance*, EncodedJSValue value))
{
    return isPlainDataObject(JSValue::decode(value));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
