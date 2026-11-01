/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterpreterSlowPaths.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClosure.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImageOfCellScope.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectBuilder.h"
#include "AOTObjectBuilderInlines.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "PropertySlot.h"
#include "ScriptExecutable.h"
#include "SourceProvider.h"
#include <bit>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

void willRunInInterpreter(CodeBlock* codeBlock)
{
    VM& vm = codeBlock->vm();
    count(vm, counterFor(codeBlock->codeType()));

    if (!Options::crashIfInterpreterIsUsed()) [[likely]]
        return;
    if (codeBlock->source().provider()->provenance() != SourceProvenance::Static)
        return;
    // The text of an implicit class constructor is the engine's, whichever text declares the class. Only bytecode creates one, and that bytecode came through here.
    if (codeBlock->unlinkedCodeBlock()->isBuiltinDefaultClassConstructor())
        return;
    // Compiled code belongs to the realm of the program. Any other realm has its built-in functions as text, and what creates such a realm is declared to use the interpreter.
    if (vm.hasCompiledCode() && !Instance::of(codeBlock->globalObject()) && codeBlock->unlinkedCodeBlock()->isBuiltinFunction())
        return;

    dataLogLn("crashIfInterpreterIsUsed: ", codeBlock->codeType(), " ", codeBlock->inferredName(), " at ", codeBlock->ownerExecutable()->sourceURL(), ":", codeBlock->ownerExecutable()->firstLine());
    CRASH();
}

static JSValue getById(Instance&, JSCell* base, const Identifier&);

// Code that is not compiled is of no image. What it asks of a cell that is no compiled object it asks the tables of the first image of the realm, whichever image has called it.
// Whoever asks them runs that image meanwhile: to it a closure or a namespace of another image is a stranger's, as to its own code.
static Instance* instanceOfInterpreter(JSGlobalObject* realm)
{
    Instance* instance = realm->aotInstance();
    return instance && instance->state ? instance : nullptr;
}

// The tables of the image of a compiled object say of it all that they say where there is one image. Whoever asks them runs that image meanwhile.
static Instance* instanceOfCompiledObject(JSGlobalObject* realm, JSCell* object)
{
    ASSERT(object->type() == CompiledObjectType);
    Instance* instance = realm->aotInstance();
    if (!instance || !instance->state)
        return nullptr;
    if (!instance->above) [[likely]]
        return instance;
    return object->structure()->realm() == realm ? Instance::owning(object) : nullptr;
}

// Null where the field lies outside the cell and has no storage yet.
static void* storageOfField(const Slot& slot, Object& object)
{
    uint32_t offset = slot.fieldSlot().offset;
    if (!slot.has(Slot::isOutOfLine))
        return &object.at<uint8_t>(offset);
    Extension* extension = object.extension();
    return extension ? extension->slotOrNull(offset) : nullptr;
}

static JSValue readField(Instance& instance, const Slot& slot, Object& object)
{
    SlotRepresentation representation = slot.fieldSlot().representation();
    const void* field = storageOfField(slot, object);
    if (!field)
        return { };
    // Only the object makes the closure that its slot still defers.
    if (representation == SlotRepresentation::DeferredClosure && *static_cast<const uint32_t*>(field) < minCell32)
        return { };
    JSValue value = readSlot(representation, field, *instance.state->strings);
    if (!value && slot.has(Slot::absentIsUndefined))
        return jsUndefined();
    return value;
}

static JSValue valueInStorage(const Slot& slot, JSCell* base)
{
    return valueAtOffset(asObject(slot.has(Slot::isOwn) ? base : slot.holder), slot.offsetInStorage());
}

// callCustomGetterAndReturn() in the stubs.
static JSValue callCustomGetter(Instance& instance, const Slot& slot, JSCell* base, const Identifier& ident)
{
    const CustomAccessorRecord& record = instance.state->propertyCaches->tables().customAccessors().at(slot.customAccessor.index);
    if (record.offsetOfCachedValue) {
        if (JSValue cachedValue = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(reinterpret_cast<const uint8_t*>(base) + record.offsetOfCachedValue)))
            return cachedValue;
    }
    return Interpreter::callCustomGetter(record, base, ident);
}

// Empty where the slot does not say what the value is, and after an exception.
static JSValue readBySlot(Instance& instance, const Slot& slot, JSCell* base, const Identifier& ident)
{
    switch (slot.kind) {
    case Slot::Kind::Value:
        return valueInStorage(slot, base);
    case Slot::Kind::Getter:
        return uncheckedDowncast<GetterSetter>(valueInStorage(slot, base))->callGetter(instance.globalObject, base);
    case Slot::Kind::FarValue:
        return asObject(base)->getDirect(slot.offsetInStorage());
    case Slot::Kind::Absent:
        return jsUndefined();
    case Slot::Kind::CustomGetter:
        return callCustomGetter(instance, slot, base, ident);
    case Slot::Kind::Field:
        return readField(instance, slot, *uncheckedDowncast<Object>(base));
    case Slot::Kind::InheritedField: {
        Instance* instanceOfHolder = instanceOfCompiledObject(instance.globalObject, slot.holder);
        if (!instanceOfHolder)
            return { };
        ImageOfInstanceScope imageOfHolder(*instanceOfHolder);
        return getById(*instanceOfHolder, slot.holder, ident);
    }
    case Slot::Kind::ExtraProperty: {
        JSObject* extraProperties = uncheckedDowncast<Object>(base)->extraProperties();
        return extraProperties ? extraProperties->getDirect(Slot::offsetFor(slot.extraProperty.wordIndex)) : JSValue();
    }
    case Slot::Kind::Constant:
        return JSValue::decode(slot.constant);
    case Slot::Kind::Empty:
    case Slot::Kind::Setter:
    case Slot::Kind::Transition:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::FieldGetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::FieldOrInheritedField:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Uncacheable:
    case Slot::Kind::Removal:
    case Slot::Kind::ExtraPropertyRemoval:
    case Slot::Kind::FieldRemoval:
    case Slot::Kind::NothingToRemove:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue getById(Instance& instance, JSCell* base, const Identifier& ident)
{
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        RELEASE_AND_RETURN(scope, getAndCache({ &instance, ident, std::nullopt, nullptr }, base));
    JSValue value = readBySlot(instance, *slot, base, ident);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    RELEASE_AND_RETURN(scope, JSValue(base).get(instance.globalObject, ident));
}

enum class CacheOfOperation : bool { None, HoldsOwnValues };

// The interpreter looks an ordinary object up itself, so that its own cache gets what it can hold. didGetById() files the rest.
static std::optional<JSValue> tryGetFromOrdinaryObject(Instance& instance, JSCell* base, const Identifier& ident, CacheOfOperation cache)
{
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        return std::nullopt;
    if (cache == CacheOfOperation::HoldsOwnValues && slot->kind == Slot::Kind::Value && slot->has(Slot::isOwn))
        return std::nullopt;
    JSValue value = readBySlot(instance, *slot, base, ident);
    RETURN_IF_EXCEPTION(scope, JSValue());
    if (!value)
        return std::nullopt;
    return value;
}

// addExtraProperty() in the stubs, for a property that needs no new storage: the Structures before and after are the slot's, so nothing is looked up and nobody is told again.
static bool tryAddExtraProperty(Instance& instance, const Slot& slot, Object& object, JSValue value)
{
    if (object.isNotExtensible() || slot.has(Slot::isOutOfLine))
        return false;
    VM& vm = *instance.vm;
    // As in operationAOTAddPropertyBySlot(): this pointer alone keeps the Structure alive across the allocation below.
    auto* newStructure = WTF::opaque(uncheckedDowncast<CompiledStructure>(slot.extraPropertyAddition.newStructureID.decode()));
    PropertyOffset offset = Slot::offsetFor(slot.extraPropertyAddition.wordIndex);
    JSObject* extraProperties = object.extraProperties();
    if (!extraProperties) {
        if (!object.layout().extraPropertiesSlot())
            return false;
        extraProperties = constructEmptyObject(vm, newStructure->extraPropertiesStructure());
        extraProperties->putDirectOffset(vm, offset, value);
        object.setExtraProperties(vm, extraProperties, newStructure);
        return true;
    }
    extraProperties->putDirectOffset(vm, offset, value);
    extraProperties->setStructure(vm, newStructure->extraPropertiesStructure());
    object.JSCell::setStructure(vm, newStructure);
    return true;
}

static void putById(Instance& instance, JSCell* base, const Identifier& ident, JSValue value, bool isStrict)
{
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Store);
    if (!slot) {
        putAndCache({ &instance, ident, std::nullopt, nullptr }, base, value, isStrict ? PutMode::Strict : PutMode::Sloppy);
        return;
    }
    if (slot->kind == Slot::Kind::Setter) {
        uncheckedDowncast<GetterSetter>(valueInStorage(*slot, base))->callSetter(instance.globalObject, base, value, isStrict);
        return;
    }
    if (slot->kind == Slot::Kind::ExtraPropertyAddition && tryAddExtraProperty(instance, *slot, *uncheckedDowncast<Object>(base), value))
        return;
    PutPropertySlot putSlot(base, isStrict, PutPropertySlot::PutById);
    JSValue(base).putInline(instance.globalObject, ident, value, putSlot);
}

static UniquedStringImpl* nameOfKey(JSValue key)
{
    UniquedStringImpl* uid = key.isCell() ? uidOfKey(key.asCell()) : nullptr;
    return uid && !parseIndex(*uid) ? uid : nullptr;
}

namespace Interpreter {

JSValue callCustomGetter(const CustomAccessorRecord& record, JSCell* base, const Identifier& ident)
{
    JSCell* thisCell = record.slotBase ? record.slotBase : base;
    return JSValue::decode(std::bit_cast<GetValueFunc>(record.function)(record.globalObject, JSValue::encode(thisCell), ident));
}

static std::optional<JSValue> tryGetByName(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident, CacheOfOperation cache)
{
    bool isCompiledObject = base->type() == CompiledObjectType;
    Instance* instance = isCompiledObject ? instanceOfCompiledObject(globalObject, base) : instanceOfInterpreter(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterReadSlowPaths);
    ImageOfInstanceScope imageOfTables(*instance);
    if (!isCompiledObject)
        return tryGetFromOrdinaryObject(*instance, base, ident, cache);
    return getById(*instance, base, ident);
}

std::optional<JSValue> tryGetById(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident)
{
    return tryGetByName(globalObject, base, ident, CacheOfOperation::HoldsOwnValues);
}

bool sharesStructureWithClassesThatLackTheirMembers(VM& vm, JSCell* cell)
{
    Closure* closure = uncheckedDowncast<Closure>(cell);
    return closure->kind().isConstructor && closure->classData() && !closure->getDirect(vm, vm.propertyNames->prototype);
}

std::optional<PropertyOffset> placeOfFieldInSiteCache(JSGlobalObject* globalObject, JSCell* object, Structure* structure, const Identifier& ident)
{
    Instance* instance = instanceOfCompiledObject(globalObject, object);
    if (!instance)
        return std::nullopt;
    const Slot* slot = instance->state->propertyCaches->findByKey(structure, ident.impl(), SlotAccess::Read);
    if (!slot)
        return std::nullopt;
    if (slot->kind == Slot::Kind::ExtraProperty) {
        std::optional<SlotData> hiddenWord = uncheckedDowncast<CompiledStructure>(structure)->extraPropertiesSlot();
        int32_t wordIndex = slot->extraProperty.wordIndex;
        if (!hiddenWord || hiddenWord->representation() != SlotRepresentation::Cell32 || hiddenWord->offset > maxHiddenWordOffsetInSiteCache || !Slot::fitsInLocation(wordIndex))
            return invalidOffset;
        return extraPropertyInSiteCache + static_cast<PropertyOffset>(hiddenWord->offset << 16 | static_cast<uint16_t>(wordIndex));
    }
    if (slot->kind != Slot::Kind::Field || slot->has(Slot::isOutOfLine))
        return invalidOffset;
    uint32_t offset = slot->fieldSlot().offset;
    static_assert(SlotData::maxOffset < static_cast<uint32_t>(narrowReferenceInSiteCache));
    switch (slot->fieldSlot().representation()) {
    case SlotRepresentation::JSValue: {
        if (offset < sizeof(JSObjectWithButterfly) || offset % sizeof(EncodedJSValue))
            return invalidOffset;
        uint32_t property = (offset - sizeof(JSObjectWithButterfly)) / sizeof(EncodedJSValue);
        return property < static_cast<uint32_t>(firstOutOfLineOffset) ? static_cast<PropertyOffset>(property) : invalidOffset;
    }
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return narrowReferenceInSiteCache + static_cast<PropertyOffset>(offset);
    case SlotRepresentation::Cell:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalInt32:
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::Float:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::LiteralString:
        return invalidOffset;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void didGetById(JSGlobalObject* globalObject, JSObject* base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot)
{
    Instance* instance = instanceOfInterpreter(globalObject);
    if (!instance || structureBefore->isDictionary() || slot.isCacheableValue() || (isJSArray(base) && ident == instance->vm->propertyNames->length))
        return;
    ImageOfInstanceScope imageOfTables(*instance);
    PropertyCaches& caches = *instance->state->propertyCaches;
    UniquedStringImpl* uid = ident.impl();
    if (caches.findByKey(structureBefore, uid, SlotAccess::Read) || !caches.shouldFile(structureBefore->id(), uid, SlotAccess::Read))
        return;
    caches.cache({ structureBefore, { uid, std::nullopt }, SlotAccess::Read }, classifyRead({ instance->globalObject, base, structureBefore, uid, slot }));
}

std::optional<JSValue> tryGetByVal(JSGlobalObject* globalObject, JSCell* base, JSValue key)
{
    UniquedStringImpl* uid = nameOfKey(key);
    if (!uid)
        return std::nullopt;
    return tryGetByName(globalObject, base, Identifier::fromUid(globalObject->vm(), uid), CacheOfOperation::None);
}

std::optional<bool> tryInById(JSGlobalObject* globalObject, JSObject* base, const Identifier& ident)
{
    bool isCompiledObject = base->type() == CompiledObjectType;
    Instance* instance = isCompiledObject ? instanceOfCompiledObject(globalObject, base) : instanceOfInterpreter(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterPresenceSlowPaths);
    ImageOfInstanceScope imageOfTables(*instance);
    if (!instance->state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read))
        return hasPropertyAndCache(instance, base, { ident.impl(), std::nullopt });
    return base->hasProperty(instance->globalObject, ident);
}

std::optional<bool> tryInByVal(JSGlobalObject* globalObject, JSObject* base, JSValue key)
{
    UniquedStringImpl* uid = nameOfKey(key);
    if (!uid)
        return std::nullopt;
    return tryInById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid));
}

bool tryPutById(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident, JSValue value, bool isStrict)
{
    bool isCompiledObject = base->type() == CompiledObjectType;
    Instance* instance = isCompiledObject ? instanceOfCompiledObject(globalObject, base) : instanceOfInterpreter(globalObject);
    if (!instance)
        return false;
    count(*instance, RuntimeCounterID::InterpreterStoreSlowPaths);
    ImageOfInstanceScope imageOfTables(*instance);
    putById(*instance, base, ident, value, isStrict);
    return true;
}

bool tryPutByVal(JSGlobalObject* globalObject, JSCell* base, JSValue key, JSValue value, bool isStrict)
{
    UniquedStringImpl* uid = nameOfKey(key);
    return uid && tryPutById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid), value, isStrict);
}

std::optional<bool> tryInstanceOf(JSGlobalObject* globalObject, JSValue value, JSValue constructor)
{
    Instance* instance = instanceOfInterpreter(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterReadSlowPaths);
    ImageOfInstanceScope imageOfTables(*instance);
    return instanceOfAndCache(instance, value, constructor);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(ObjectLiteralPlans);

static uintptr_t& allocatorWord(ObjectAllocationProfile& profile)
{
    static_assert(sizeof(Allocator) == sizeof(uintptr_t));
    return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(&profile) + ObjectAllocationProfile::offsetOfAllocator());
}

const ObjectLiteralPlan* ObjectLiteralPlan::of(ObjectAllocationProfile& profile)
{
    uintptr_t word = allocatorWord(profile);
    return word & allocatorTag && word != notPlannedYet ? std::bit_cast<const ObjectLiteralPlan*>(word & ~notPlannedYet) : nullptr;
}

uint8_t* ObjectLiteralPlan::allocateCell(VM& vm) const
{
    auto* words = static_cast<uint64_t*>(Allocator(allocator).allocate(vm.heap, allocator->cellSize(), nullptr, AllocationFailureMode::Assert));
    words[0] = cellHeader;
    words[1] = secondWord;
    std::fill_n(words + offsetOfClearedWords / sizeof(uint64_t), numberOfClearedWords, 0);
    return reinterpret_cast<uint8_t*>(words);
}

NEVER_INLINE void ObjectLiteralPlan::setClearedWords(unsigned numberOfWordsBehindSecond)
{
    unsigned numberOfFilledWords = 0;
    while (numberOfFilledWords < numberOfWordsBehindSecond && std::ranges::contains(std::span(valueOffsets).first(numberOfValues), (2 + numberOfFilledWords) * sizeof(uint64_t)))
        ++numberOfFilledWords;
    offsetOfClearedWords = (2 + numberOfFilledWords) * sizeof(uint64_t);
    numberOfClearedWords = numberOfWordsBehindSecond - numberOfFilledWords;
}

// Returns the Structure of the finished object.
template<typename Names>
static Structure* fillPlanForFinalObject(Instance& instance, Structure* emptyStructure, const Names& names, ObjectLiteralPlan& result)
{
    VM& vm = *instance.vm;
    ObjectLiteralPlan plan;
    Structure* structure = emptyStructure;
    for (size_t index = 0; index < names.size(); ++index) {
        PropertyOffset offset = invalidOffset;
        structure = Structure::addPropertyTransition(vm, structure, &*names[index], 0, offset);
        if (structure->isDictionary() || !isInlineOffset(offset))
            return nullptr;
        plan.valueOffsets[index] = JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue);
    }
    unsigned inlineCapacity = structure->inlineCapacity();
    plan.allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(inlineCapacity), AllocatorForMode::EnsureAllocator).localAllocator();
    if (!plan.allocator)
        return nullptr;
    instance.state->immortalCells->add(structure);
    plan.cellHeader = cellHeaderFor(structure);
    plan.numberOfValues = names.size();
    plan.setClearedWords(inlineCapacity);
    result = plan;
    return structure;
}

static bool typeAcceptsEveryValue(const RuntimeType& type)
{
    constexpr OptionSet<PrimitiveSet> everyPrimitive {
        PrimitiveSet::Undefined, PrimitiveSet::Null, PrimitiveSet::False, PrimitiveSet::True, PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber,
        PrimitiveSet::String, PrimitiveSet::Symbol, PrimitiveSet::BigInt
    };
    return type.primitiveSet().containsAll(everyPrimitive) && !type.integerRange() && type.acceptsEveryObject();
}

ObjectLiteralPlans::ObjectLiteralPlans(State& state)
{
    state.caches->add(CacheKind::ClearedEachCollection, "object literal plans"_s, *this);
}

void ObjectLiteralPlans::clear()
{
    for (const Entry& entry : m_entries) {
        if (entry.recordBuilder)
            entry.recordBuilder->clearAcceptedStructures();
    }
}

// The builder was written for what a parser makes: it takes every cell but a string for an object, and undefined for null.
JSObject* ObjectLiteralPlans::tryBuildRecord(VM& vm, const ObjectLiteralPlan& plan, const EncodedJSValue* values)
{
    const ObjectBuilderPlan& builder = *plan.recordBuilder;
    ImageOfInstanceScope imageOfRecord(builder.m_state.instance);
    for (unsigned index = 0; index < plan.numberOfValues; ++index) {
        JSValue value = JSValue::decode(values[index]);
        if (value.isCell() ? !value.isString() && !value.isObject() : value.isUndefined() && !builder.m_state.objects->layouts.slotAdmits(*builder.m_fields[index].field, value))
            return nullptr;
    }
    return ObjectBuilder::tryBuild(vm, builder, plan.structureOfRecord, values);
}

bool ObjectLiteralPlans::storesValuesUnchanged(const ObjectBuilderPlan& builder, bool testsValues)
{
    bool slotsHoldValues = std::ranges::all_of(builder.m_fields, [&](const ObjectBuilderPlan::PlannedField& planned) {
        const FieldData& field = *planned.field;
        SlotRepresentation representation = field.slot.representation();
        return (representation == SlotRepresentation::JSValue || (testsValues && representation == SlotRepresentation::Cell)) && !field.holdsRawValueFromBirth && !field.keepsStateOfBirth;
    });
    bool otherWordsAreZero = testsValues || std::ranges::all_of(builder.m_initialWords, [](const ObjectBuilderPlan::InitialWord& initial) {
        return initial.offset == offsetOfOrderWord || !initial.word;
    });
    return builder.m_namesEveryFieldOfBirth && slotsHoldValues && otherWordsAreZero;
}

void ObjectLiteralPlans::fillPlanFromBuilder(Instance& instance, const ObjectBuilderPlan& builder, ObjectLiteralPlan& plan)
{
    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && offsetOfFirstField == 2 * sizeof(uint64_t));
    plan.allocator = builder.m_allocator.localAllocator();
    plan.structureOfRecord = instance.state->objects->structures.objectStructure(*instance.vm, builder.layout().shape);
    plan.cellHeader = cellHeaderFor(plan.structureOfRecord);
    for (const ObjectBuilderPlan::InitialWord& initial : builder.m_initialWords) {
        if (initial.offset == offsetOfOrderWord)
            plan.secondWord = initial.word;
    }
    plan.numberOfValues = builder.m_fastFields.size();
    for (size_t index = 0; index < builder.m_fastFields.size(); ++index)
        plan.valueOffsets[index] = builder.m_fastFields[index].offset;
    plan.numberOfClearedWords = (builder.m_initialBody.size() + sizeof(JSCell) - offsetOfFirstField + sizeof(uint64_t) - 1) / sizeof(uint64_t);
}

std::unique_ptr<ObjectLiteralPlan> ObjectLiteralPlans::createPlanThatTestsValues(Instance& instance, const ObjectBuilderPlan& builder, const ObjectLiteralPlan& planForOrdinaryObject, Vector<uint64_t>& initialWords)
{
    if (!storesValuesUnchanged(builder, true))
        return nullptr;
    using Builder = ObjectBuilderPlan;
    static_assert(ObjectLiteralPlan::acceptsInt32 == Builder::AcceptsInt32 && ObjectLiteralPlan::acceptsDouble == Builder::AcceptsDouble && ObjectLiteralPlan::acceptsString == Builder::AcceptsString);
    static_assert(ObjectLiteralPlan::acceptsObject == Builder::AcceptsObject && ObjectLiteralPlan::acceptsBoolean == Builder::AcceptsBoolean && ObjectLiteralPlan::acceptsNull == Builder::AcceptsNull);
    auto plan = makeUniqueWithoutFastMallocCheck<ObjectLiteralPlan>();
    fillPlanFromBuilder(instance, builder, *plan);
    plan->planForOrdinaryObject = &planForOrdinaryObject;
    plan->recordBuilder = &builder;
    initialWords.fill(0, WTF::roundUpToMultipleOf<sizeof(uint64_t)>(builder.m_initialBody.size()) / sizeof(uint64_t));
    memcpySpan(asMutableByteSpan(initialWords.mutableSpan()), builder.m_initialBody.span());
    plan->initialWords = initialWords.span().data();
    plan->numberOfInitialWords = initialWords.size();
    plan->acceptedStructureIDs = builder.m_acceptedStructureIDs.span().data();
    for (size_t index = 0; index < builder.m_fastFields.size(); ++index) {
        constexpr uint8_t cells = ObjectBuilderPlan::AcceptsString | ObjectBuilderPlan::AcceptsObject;
        constexpr uint8_t testedInline = cells | ObjectBuilderPlan::AcceptsInt32 | ObjectBuilderPlan::AcceptsDouble | ObjectBuilderPlan::AcceptsBoolean | ObjectBuilderPlan::AcceptsNull;
        bool holdsCellsAlone = builder.m_fastFields[index].representation == SlotRepresentation::Cell;
        plan->acceptedValues[index] = builder.m_fastFields[index].acceptedValues & (holdsCellsAlone ? cells : testedInline);
        RuntimeLayoutTable& layouts = instance.state->objects->layouts;
        const FieldData& field = *builder.m_fields[index].field;
        if (!holdsCellsAlone && layouts.slotAdmits(field, jsUndefined()))
            plan->acceptedValues[index] |= ObjectLiteralPlan::acceptsUndefined;
        for (const RuntimeCellTest& test : layouts.typeOf(field).cellTests()) {
            if (test.kind != CellTestKind::CellTypeIs || test.cellTypes().first < ObjectType)
                continue;
            plan->firstAcceptedCellTypes[index] = test.cellTypes().first;
            plan->lastAcceptedCellTypes[index] = test.cellTypes().last;
            break;
        }
    }
    return plan;
}

std::unique_ptr<ObjectLiteralPlan> ObjectLiteralPlans::createPlanForRecord(Instance& instance, std::span<UniquedStringImpl* const> names, std::unique_ptr<ObjectBuilderPlan>& builderThatTestsValues)
{
    VM& vm = *instance.vm;
    State& state = *instance.state;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::optional<uint32_t> lastName = identifiersOf(instance).numberOf(names.back());
    if (!lastName)
        return nullptr;

    std::unique_ptr<ObjectBuilderPlan> best;
    for (const ParsedRecordFieldData& row : std::ranges::equal_range(state.tables.parsedRecordFields(), *lastName, { }, &ParsedRecordFieldData::nameNumber)) {
        const LayoutData& layout = layouts.layoutForShape(row.shape);
        if (layout.numberOfFields < names.size() || (best && best->layout().numberOfFields <= layout.numberOfFields) || !ObjectBuilder::canBuildWithoutCompilerKnowledge(layout))
            continue;
        bool hasEveryName = std::ranges::all_of(names, [&](UniquedStringImpl* name) {
            return !!layouts.field(vm, layout, name);
        });
        if (!hasEveryName)
            continue;
        std::unique_ptr<ObjectBuilderPlan> builderPlan = ObjectBuilder::tryCreatePlan(state, layout, names);
        if (!builderPlan || !builderPlan->hasFastPath())
            continue;
        bool everyFieldAcceptsEveryValue = std::ranges::all_of(layout.fields(), [&](const FieldData& field) {
            return typeAcceptsEveryValue(layouts.typeOf(field));
        });
        if (everyFieldAcceptsEveryValue && storesValuesUnchanged(*builderPlan, false))
            best = WTF::move(builderPlan);
        else if (!builderThatTestsValues || layout.numberOfFields < builderThatTestsValues->layout().numberOfFields)
            builderThatTestsValues = WTF::move(builderPlan);
    }
    if (!best)
        return nullptr;
    builderThatTestsValues = nullptr;

    auto plan = makeUniqueWithoutFastMallocCheck<ObjectLiteralPlan>();
    fillPlanFromBuilder(instance, *best, *plan);
    plan->setClearedWords(plan->numberOfClearedWords);
    return plan;
}

const ObjectLiteralPlan* ObjectLiteralPlans::findOrCreate(Instance& instance, Structure* emptyStructure, std::span<UniquedStringImpl* const> names, bool mayBeRecord)
{
    for (const Entry& entry : m_entries) {
        if (entry.emptyStructure == emptyStructure && entry.mayBeRecord == mayBeRecord && std::ranges::equal(entry.names, names, { }, &RefPtr<UniquedStringImpl>::get))
            return entry.planForSites();
    }
    if (m_entries.size() == maxNumberOfPlans)
        return nullptr;
    instance.state->immortalCells->add(emptyStructure);
    std::unique_ptr<ObjectBuilderPlan> recordBuilder;
    std::unique_ptr<ObjectLiteralPlan> plan = mayBeRecord ? createPlanForRecord(instance, names, recordBuilder) : nullptr;
    Structure* structureOfFinalObject = nullptr;
    if (!plan) {
        plan = makeUniqueWithoutFastMallocCheck<ObjectLiteralPlan>();
        structureOfFinalObject = fillPlanForFinalObject(instance, emptyStructure, names, *plan);
        if (!structureOfFinalObject)
            plan = nullptr;
    }
    Vector<uint64_t> initialWordsOfRecord;
    std::unique_ptr<ObjectLiteralPlan> planThatTestsValues = plan && recordBuilder ? createPlanThatTestsValues(instance, *recordBuilder, *plan, initialWordsOfRecord) : nullptr;
    m_entries.append({ emptyStructure, structureOfFinalObject, mayBeRecord, { }, WTF::move(plan), WTF::move(recordBuilder), WTF::move(planThatTestsValues), WTF::move(initialWordsOfRecord) });
    m_entries.last().names.appendContainerWithMapping(names, [](UniquedStringImpl* name) {
        return RefPtr { name };
    });
    return m_entries.last().planForSites();
}

void ObjectLiteralPlans::didOutgrowCell(Instance& instance, Structure* beforeAddition)
{
    unsigned inlineCapacity = beforeAddition->inlineCapacity();
    for (Entry& entry : m_entries) {
        if (!entry.structureOfFinalObject || entry.structureOfFinalObject->inlineCapacity() != inlineCapacity || inlineCapacity >= entry.names.size() + maxPropertiesAddedInCell)
            continue;
        Structure* ancestor = beforeAddition;
        while (ancestor && ancestor != entry.structureOfFinalObject)
            ancestor = ancestor->previousID();
        if (!ancestor)
            continue;
        JSGlobalObject* globalObject = entry.emptyStructure->realm();
        Structure* roomier = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, asObject(entry.emptyStructure->storedPrototype()), inlineCapacity + 2);
        instance.state->immortalCells->add(roomier);
        // The sites hold the address of the plan.
        if (Structure* structure = fillPlanForFinalObject(instance, roomier, entry.names, *entry.plan))
            entry.structureOfFinalObject = structure;
        return;
    }
}

void didAllocateFirstPropertyOutsideCell(JSGlobalObject* globalObject, Structure* beforeAddition)
{
    didLeaveInlineAllocation(globalObject->vm());
    Instance* instance = instanceOfInterpreter(globalObject);
    if (!instance)
        return;
    ImageOfInstanceScope imageOfTables(*instance);
    if (ObjectLiteralPlans* plans = instance->state->objects->caches->objectLiteralPlans.get())
        plans->didOutgrowCell(*instance, beforeAddition);
}

void checkPlacesInSymbolTable(SymbolTable& table)
{
    auto* bytes = reinterpret_cast<uint8_t*>(&table);
    RELEASE_ASSERT(reinterpret_cast<uint8_t*>(&table.singleton()) == bytes + PlacesInSymbolTable::offsetOfSingleton);
    RELEASE_ASSERT(*reinterpret_cast<uint32_t*>(bytes + PlacesInSymbolTable::offsetOfMaxScopeOffset) + 1 == table.scopeSize());
    uintptr_t singleton = *reinterpret_cast<uintptr_t*>(bytes + PlacesInSymbolTable::offsetOfSingleton);
    bool isThin = singleton & 1;
    RELEASE_ASSERT(table.singleton().isStillValid() || !isThin || singleton == PlacesInSymbolTable::invalidatedSingleton);
}

void fuseReadsOfClosureVariables(CodeBlock& codeBlock)
{
    static_assert(ClosureVar < 1u << FusedClosureVariableRead::typeBits);
    for (const auto& instruction : codeBlock.instructions()) {
        if (instruction->width() != OpcodeSize::Narrow || !instruction->is<OpResolveScope>())
            continue;
        auto resolution = instruction->as<OpResolveScope>();
        auto& metadata = resolution.metadata(&codeBlock);
        const JSInstruction* next = instruction.ptr() + instruction->size();
        if (metadata.m_resolveType != ClosureVar || next->width() != OpcodeSize::Narrow || !next->is<OpGetFromScope>())
            continue;
        auto read = next->as<OpGetFromScope>();
        auto& metadataOfRead = read.metadata(&codeBlock);
        if (read.m_scope != resolution.m_dst || metadataOfRead.m_getPutInfo.resolveType() != ClosureVar || metadataOfRead.m_operand > FusedClosureVariableRead::maxOffset)
            continue;
        size_t bytesToSkip = next->size();
        const JSInstruction* afterRead = next + next->size();
        if (afterRead->width() == OpcodeSize::Narrow && afterRead->is<OpCheckTdz>() && afterRead->as<OpCheckTdz>().m_targetVirtualRegister == read.m_dst)
            bytesToSkip += afterRead->size();
        metadata.m_resolveType = static_cast<ResolveType>(ClosureVar | bytesToSkip << FusedClosureVariableRead::typeBits | metadataOfRead.m_operand << FusedClosureVariableRead::offsetShift);
    }
}

void didLinkNewObject(ObjectAllocationProfile& profile)
{
    allocatorWord(profile) = ObjectLiteralPlan::notPlannedYet;
}

bool planObjectLiteral(JSGlobalObject* globalObject, CodeBlock* codeBlock, const JSInstruction* instruction, ObjectAllocationProfile& profile)
{
    if (allocatorWord(profile) != ObjectLiteralPlan::notPlannedYet)
        return false;

    VM& vm = globalObject->vm();
    Structure* emptyStructure = profile.structure();
    allocatorWord(profile) = std::bit_cast<uintptr_t>(subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(emptyStructure->inlineCapacity()), AllocatorForMode::EnsureAllocator).localAllocator());

    Instance* instance = instanceOfInterpreter(globalObject);
    if (!instance)
        return true;
    ImageOfInstanceScope imageOfTables(*instance);

    VirtualRegister object = instruction->as<OpNewObject>().m_dst;
    Vector<UniquedStringImpl*, ObjectLiteralPlan::maxNumberOfValues> names;
    for (const JSInstruction* next = instruction + instruction->size(); names.size() < ObjectLiteralPlan::maxNumberOfValues && next->width() != OpcodeSize::Wide32 && next->is<OpPutById>(); next += next->size()) {
        auto store = next->as<OpPutById>();
        UniquedStringImpl* name = codeBlock->identifier(store.m_property).impl();
        if (store.m_base != object || store.m_value == object || !store.m_flags.isDirect() || names.contains(name))
            break;
        names.append(name);
    }
    if (names.isEmpty())
        return true;

    std::unique_ptr<ObjectLiteralPlans>& plans = instance->state->objects->caches->objectLiteralPlans;
    if (!plans)
        plans = makeUnique<ObjectLiteralPlans>(*instance->state);
    if (const ObjectLiteralPlan* plan = plans->findOrCreate(*instance, emptyStructure, names.span(), !codeBlock->unlinkedCodeBlock()->isBuiltinFunction()))
        allocatorWord(profile) = std::bit_cast<uintptr_t>(plan) | ObjectLiteralPlan::allocatorTag | (plan->recordBuilder ? ObjectLiteralPlan::slowPathTag : 0);
    return true;
}

void didLeaveInlineAllocation(VM& vm)
{
    count(vm, RuntimeCounterID::InterpreterAllocationSlowPaths);
}

void didLeaveInlineBranch(VM& vm)
{
    count(vm, RuntimeCounterID::InterpreterBranchSlowPaths);
}

} // namespace Interpreter

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
