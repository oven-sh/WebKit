/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
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
        });
        return walkResult == PropertyWalk::Result::Completed;
    }
    Structure* structure = source->structure();
    TypeInfo info = structure->typeInfo();
    if (source->hasNonReifiedStaticProperties() || info.overridesGetOwnPropertySlot() || info.overridesAnyFormOfGetOwnPropertyNames() || structure->hasUnderscoreProtoPropertyExcludingOriginalProto() || hasIndexedProperties(structure->indexingType()))
        return false;
    // As in the engine's own copy, which takes the keys of such a source names first.
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

// Nothing sees the target before the copy is complete, so what the source runs cannot tell when the properties are defined.
static NEVER_INLINE ASCIILiteral defineCollectedProperties(JSGlobalObject* globalObject, VM& vm, JSObject* target, Object* record, Vector<UniquedStringImpl*, 8>& copiedNames, MarkedArgumentBuffer& copiedValues)
{
    constexpr ASCIILiteral threw = "threw"_s;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (record && record->tryDefineFields(vm, copiedNames.span(), { copiedValues.data(), copiedValues.size() }))
        return "read by lookups, all are fields"_s;
    if (record && tryAssignCollectedProperties(globalObject, vm, *record, copiedNames, copiedValues))
        return "read by lookups, then by a plan"_s;
    // An element is no property that a definition by name knows.
    bool hasIndex = std::ranges::any_of(copiedNames, [](UniquedStringImpl* name) {
        return !!parseIndex(PropertyName(name));
    });
    if (!record && !hasIndex && takesPlainPropertiesDirectly(target->structure()) && !copiedNames.contains(vm.propertyNames->underscoreProto.impl()) && tryStoreCopiedProperties(vm, target, copiedNames, copiedValues))
        return "read by lookups, put as a batch"_s;
    if (record && !hasIndex && !(record->orderWord() & orderWordIntegrityBits)) {
        definePropertiesInOrder(vm, *record, copiedNames.mutableSpan(), { copiedValues.data(), copiedValues.size() });
        RETURN_IF_EXCEPTION(scope, threw);
        return "read by lookups, defined name by name"_s;
    }
    for (size_t i = 0; i < copiedNames.size(); ++i) {
        defineCopiedProperty(globalObject, target, copiedNames[i], copiedValues.at(i));
        RETURN_IF_EXCEPTION(scope, threw);
    }
    return "read by lookups, defined name by name"_s;
}

// None of the ways that run no code takes a getter, and each would walk the source to find that out.
static NEVER_INLINE std::optional<ASCIILiteral> tryCopyCallingGetters(JSGlobalObject* globalObject, VM& vm, JSObject* target, JSObject* from, const ExcludedKeys& excluded)
{
    if (!PropertyWalk::hasGetterToCall(from))
        return std::nullopt;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<RefPtr<UniquedStringImpl>, 32> heldNames;
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    auto isExcluded = [&](UniquedStringImpl* name) {
        return excluded.contains(name);
    };
    ScopedLambda<bool(UniquedStringImpl*)> isLeftOut = isExcluded;
    PropertyWalk::Result result = PropertyWalk::readOwnPropertiesCallingGetters(globalObject, from, heldNames, names, values, { .isLeftOut = excluded.isEmpty() ? nullptr : &isLeftOut });
    RETURN_IF_EXCEPTION(scope, "threw"_s);
    if (result != PropertyWalk::Result::Completed)
        return std::nullopt;
    // The walk hands out no element.
    if (!isCompiledObject(target) && takesPlainPropertiesDirectly(target->structure()) && !names.contains(vm.propertyNames->underscoreProto.impl()) && tryStoreCopiedProperties(vm, target, names, values))
        return "getters called, put as a batch"_s;
    RELEASE_AND_RETURN(scope, defineCollectedProperties(globalObject, vm, target, compiledObjectOrNull(target), names, values));
}

// A record with plain elements copied into a record of another layout: the names at once, then the elements. No code runs, so the order of the two does not show.
static NEVER_INLINE std::optional<ASCIILiteral> tryCopyNamesThenElements(Instance* instance, JSGlobalObject* globalObject, VM& vm, Object& target, JSObject* from, JSObject* holder)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    IndexingType indexingType = holder->indexingType();
    if (!hasInt32(indexingType) && !hasDouble(indexingType) && !hasContiguous(indexingType))
        return std::nullopt;
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
        }, PropertyWalk::Elements::AreLeftOut);
        if (walked != PropertyWalk::Result::Completed)
            return std::nullopt;
        if (!names.isEmpty() && !target.tryDefineFields(vm, names.span(), { values.data(), values.size() }) && !tryAssignCollectedProperties(globalObject, vm, target, names, values)) {
            definePropertiesInOrder(vm, target, names.mutableSpan(), { values.data(), values.size() });
            RETURN_IF_EXCEPTION(scope, "threw"_s);
        }
    }
    if (!target.hasExtraProperties() && holder->structure() == instance->realm.extraPropertiesWithElementsStructure && target.layout().extraPropertiesSlot() && !target.structure()->mayBePrototype() && !target.layout().isClass() && instance->state->tables.addedNames().empty()) {
        if (JSFinalObject* elements = tryCopyWithElements(vm, holder)) {
            target.setExtraProperties(vm, elements, instance->state->objects->structures.withExtraProperties(vm, target.compiledStructure(), nullptr));
            return "names at once, then a copy of the elements"_s;
        }
    }
    holder->forEachOwnElement<>([&](unsigned index, JSValue value) {
        if (!tryDefineElementOfCompiledObject(instance, &target, index, value))
            defineCopiedProperty(globalObject, &target, Identifier::from(vm, index), value);
        RETURN_IF_EXCEPTION(scope, IterationStatus::Done);
        return IterationStatus::Continue;
    });
    RETURN_IF_EXCEPTION(scope, "threw"_s);
    return "names at once, then elements"_s;
}

// Answers which way the copy went, for a run that counts.
static ASCIILiteral copyDataPropertiesByAnyWay(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* target, JSValue source, const ExcludedKeys& excluded)
{
    constexpr ASCIILiteral threw = "threw"_s;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(hasOwnPropertiesToCopy(source));
    JSObject* from;
    if (source.isObject()) [[likely]]
        from = asObject(source);
    else {
        from = source.toObject(globalObject);
        RETURN_IF_EXCEPTION(scope, threw);
    }
    if (isCompiledObject(from) && uncheckedDowncast<Object>(from)->layout().hasFieldsWithSymbolKeys) [[unlikely]] {
        prepareForWalk(globalObject, from);
        RETURN_IF_EXCEPTION(scope, threw);
    }
    Object* record = compiledObjectOrNull(target);
    if (record) {
        Object* other = excluded.isEmpty() ? compiledObjectOrNull(from) : nullptr;
        // An object of the target's own layout is copied word by word.
        if (other && record->shapeWord() == other->shapeWord() && record->tryCopyFieldsOf(vm, *other))
            return "fields by words or moves"_s;
        // A plan and the other ways that run no code do not read elements.
        if (other && other->hasExtraProperties() && !(record->orderWord() & orderWordIntegrityBits)) {
            JSObject* holder = PropertyWalk::objectHoldingIndexedProperties(other);
            if (holder && hasIndexedProperties(holder->indexingType())) {
                std::optional<ASCIILiteral> way = tryCopyNamesThenElements(instance, globalObject, vm, *record, from, holder);
                RETURN_IF_EXCEPTION(scope, threw);
                if (way)
                    return *way;
            }
        }
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
    if (isCompiledObject(from)) {
        std::optional<ASCIILiteral> way = tryCopyCallingGetters(globalObject, vm, target, from, excluded);
        RETURN_IF_EXCEPTION(scope, threw);
        if (way)
            return *way;
    }
    if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType) {
        if (tryCopyDataProperties(vm, target, from))
            return "final objects, tryCopyDataProperties"_s;
    }
    if (excluded.isEmpty() && target->type() == FinalObjectType && tryCopyRecordDataProperties(vm, target, from))
        return "final objects, tryCopyRecordDataProperties"_s;
    // As in the engine's own copy, which decides the order of the source's keys from here on.
    if (from->hasNonReifiedStaticProperties()) {
        from->reifyAllStaticProperties(globalObject);
        RETURN_IF_EXCEPTION(scope, threw);
    }
    auto* ordinary = dynamicDowncast<JSFinalObject>(target);
    bool canPutDirectly = ordinary && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible();
    // The engine's own copy takes the keys of a source that has elements names first.
    JSObject* holderOfElements = isCompiledObject(from) ? PropertyWalk::objectHoldingIndexedProperties(from) : from;
    bool hasElements = holderOfElements && hasIndexedProperties(holderOfElements->indexingType());
    if (canPutDirectly && excluded.isEmpty() && !hasElements) {
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
    Vector<UniquedStringImpl*, 8> copiedNames;
    MarkedArgumentBuffer copiedValues;
    auto defineCopiedProperties = [&] {
        return defineCollectedProperties(globalObject, vm, target, record, copiedNames, copiedValues);
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
    ASCIILiteral way = copyDataPropertiesByAnyWay(instance, globalObject, vm, target, source, excluded);
    if (Options::aotCountsPath()) [[unlikely]]
        instance->state->objects->caches->assignmentPlans.noteCopy(way);
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
    if (RepeatedCopy way = !count && isCompiledObject(object) ? tryRepeatAssignment(vm, *instance->state, object, JSValue::decode(source), excludedRun) : RepeatedCopy::IsNotDone; way != RepeatedCopy::IsNotDone) {
        if (Options::aotCountsPath()) [[unlikely]] {
            constexpr std::array<ASCIILiteral, 5> names { ""_s, "fields by words or moves"_s, "by the map of two layouts"_s, "by the map of two layouts, the first of its kind"_s, "as the copy before"_s };
            instance->state->objects->caches->assignmentPlans.noteCopy(names[static_cast<size_t>(way)]);
        }
        return;
    }
    ExcludedKeys excluded;
    if (excludedRun || count) {
        excluded.collect(instance, globalObject, excludedRunAt(instance, excludedRun), { excludedKeys, count });
        RETURN_IF_EXCEPTION(scope, void());
    }
    scope.release();
    copyDataPropertiesExcluding(instance, globalObject, vm, object, JSValue::decode(source), excluded);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCopyDataProperties, void, (Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCopyDataProperties);
    scope.release();
    copyDataPropertiesAsBeforeOrByAnyWay(instance, globalObject, vm, target, source, excludedRun, count, excludedKeys);
    OPERATION_RETURN(scope);
}

// The stub in front calls it where a copy that takes no frame is kept at the place of this one.
JSC_DEFINE_JIT_OPERATION(operationAOTCopyDataPropertiesByFieldMap, void, (Instance* instance, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    if (!count && tryCopyByFieldMapWithoutFrame(*instance, target, source, excludedRun)) [[likely]]
        return { nullptr };
    AOT_MUST_TAIL_CALL return operationAOTCopyDataProperties(instance, target, source, excludedRun, count, excludedKeys);
}

static JSObject* tryCopyWithoutNames(VM& vm, JSGlobalObject* globalObject, JSObject* source, ExcludedRun excluded)
{
    bool canHaveStructureOfSource = !excluded.numberPlusOne && source->type() == FinalObjectType && (!source->structure()->isDictionary() || flattenOnceUnlessCopiedAsDictionary(vm, source)) && source->structure()->canPerformFastPropertyEnumerationCommon();
    if (JSObject* copy = canHaveStructureOfSource ? tryCreateObjectViaCloning(vm, globalObject, source) : nullptr)
        return copy;
    // The engine's clone leaves out a source that never got a property. Its copy is as large as it is: what is copied to be added to keeps its room.
    Structure* structure = source->structure();
    bool isPlainEmptyObject = canHaveStructureOfSource && !structure->didTransition() && !structure->seenProperties().bits() && !source->butterfly() && checkStructureForClone(structure)
        && structure->realm() == globalObject && structure->storedPrototype() == globalObject->objectPrototype();
    if (isPlainEmptyObject)
        return JSFinalObject::create(vm, structure);
    return tryCopyAsOrdinaryObject(vm, globalObject, source, excluded);
}

// A copy of a record is a record: the names have the same places in it, and whoever expects the type of the source finds it. Flow counts on nothing in between: the words of the source, or an ordinary object.
// Out of line: the operation keeps nothing alive across the calls in here.
static NEVER_INLINE JSObject* tryCopyCompiledObject(Instance* instance, JSGlobalObject* globalObject, VM& vm, Object& source)
{
    if (Object* copy = &source.state() == instance->state ? Object::tryCreateCopyOfFields(vm, source) : nullptr)
        return copy;
    return tryCopyAsOrdinaryObject(vm, globalObject, &source, ExcludedRun { });
}

// Out of line: the list of excluded keys takes a large frame, which the copies that need no names would pay for.
static NEVER_INLINE JSObject* copyObjectByNames(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSValue source, ExcludedRun run, std::span<const EncodedJSValue> excludedKeys)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    // None of the ways that run no code takes a getter.
    if (source.isObject() && isCompiledObject(asObject(source)) && PropertyWalk::hasGetterToCall(asObject(source))) {
        JSObject* copy = constructEmptyObject(globalObject);
        ExcludedKeys excluded;
        excluded.collect(instance, globalObject, run, excludedKeys);
        RETURN_IF_EXCEPTION(scope, nullptr);
        std::optional<ASCIILiteral> way = tryCopyCallingGetters(globalObject, vm, copy, asObject(source), excluded);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (way) {
            if (Options::aotCountsPath()) [[unlikely]]
                instance->state->objects->caches->assignmentPlans.noteCopy(*way);
            return copy;
        }
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
    RELEASE_AND_RETURN(scope, (copyDataPropertiesExcluding(instance, globalObject, vm, copy, source, excluded), copy));
}

static bool hasNoGetterAndNoProto(JSObject* ordinary)
{
    Structure* structure = ordinary->structure();
    return ordinary->type() == FinalObjectType && !structure->hasAnyKindOfGetterSetterProperties() && !structure->hasUnderscoreProtoPropertyExcludingOriginalProto()
        && !ordinary->canHaveExistingOwnIndexedGetterSetterProperties() && !ordinary->hasNonReifiedStaticProperties();
}

// Object.assign({}, source) makes what { ...source } makes where a store to the new object is a definition all the way: nothing on Object.prototype intercepts one, the source has no name __proto__, and it runs no code that could change that.
static bool isAssignedAsCopied(VM& vm, JSGlobalObject* globalObject, JSValue source)
{
    Structure* above = globalObject->objectPrototype()->structure();
    if (above->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || above->mayInterceptIndexedAccesses())
        return false;
    if (!source.isObject())
        return true;
    Object* compiled = compiledObjectOrNull(source);
    if (!compiled)
        return hasNoGetterAndNoProto(asObject(source));
    if (std::ranges::any_of(compiled->layout().fields(), &FieldData::isAccessor) || compiled->fieldNamed(compiled->state(), vm.propertyNames->underscoreProto.impl()))
        return false;
    JSObject* extra = compiled->extraProperties();
    return !extra || hasNoGetterAndNoProto(extra);
}

static NEVER_INLINE JSObject* assignToNewObject(JSGlobalObject* globalObject, VM& vm, JSValue source)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
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
    JSValue source = JSValue::decode(encodedSource);
    JSObject* copy = nullptr;
    Object* compiled = !count && !excludedRun && source.isObject() ? compiledObjectOrNull(source) : nullptr;
    if (compiled) {
        copy = tryCopyCompiledObject(instance, globalObject, vm, *compiled);
        if (copy) [[likely]]
            OPERATION_RETURN(scope, copy);
    }
    ExcludedRun run = excludedRunAt(instance, excludedRun);
    if (!compiled && !count && source.isObject())
        copy = tryCopyWithoutNames(vm, globalObject, asObject(source), run);
    if (!copy) {
        copy = copyObjectByNames(instance, globalObject, vm, source, run, { excludedKeys, count });
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAssignToNewObject, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAssignToNewObject);
    JSValue source = JSValue::decode(encodedSource);
    JSObject* made = isAssignedAsCopied(vm, globalObject, source) ? copyObject(instance, globalObject, vm, source, excludedRun, count, excludedKeys) : assignToNewObject(globalObject, vm, source);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, made);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryCopyTwoObjects, JSCell*, (Instance* instance, EncodedJSValue first, EncodedJSValue second))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryCopyTwoObjects);
    DeferTerminationForAWhile deferTermination(vm);
    AssignmentPlanCache& plans = instance->state->objects->caches->assignmentPlans;
    JSFinalObject* copy = tryCopyTwoObjects(vm, *instance->state, globalObject, JSValue::decode(first), JSValue::decode(second));
    if (copy)
        plans.noteCopy("two at once, by a plan"_s);
    OPERATION_RETURN(scope, copy);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
