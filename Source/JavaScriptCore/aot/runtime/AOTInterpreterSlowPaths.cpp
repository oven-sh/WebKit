/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterpreterSlowPaths.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTChainDependents.h"
#include "AOTClosure.h"
#include "AOTCompiledStructure.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTObjectWords.h"
#include "AOTPendingStars.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeReporter.h"
#include "AOTState.h"
#include "BytecodeStructs.h"
#include "ClonedArguments.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "PropertySlot.h"
#include "ScriptExecutable.h"
#include "SourceProvider.h"
#include <algorithm>
#include <bit>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

void willRunInInterpreter(CodeBlock* codeBlock)
{
    VM& vm = codeBlock->vm();
    countEvent(vm, counterFor(codeBlock->codeType()));
    if (!Options::crashIfInterpreterIsUsed()) [[likely]]
        return;
    if (codeBlock->source().provider()->provenance() != SourceProvenance::Static)
        return;
    if (codeBlock->unlinkedCodeBlock()->isBuiltinDefaultClassConstructor())
        return;
    if (vm.hasCompiledCode() && !Instance::of(codeBlock->globalObject()) && codeBlock->unlinkedCodeBlock()->isBuiltinFunction())
        return;

    dataLogLn("crashIfInterpreterIsUsed: ", codeBlock->codeType(), " ", codeBlock->inferredName(), " at ", codeBlock->ownerExecutable()->sourceURL(), ":", codeBlock->ownerExecutable()->firstLine());
    CRASH();
}

static JSValue getById(Instance&, JSCell* base, const Identifier&);

static Instance* interpreterInstance(JSGlobalObject* realm)
{
    Instance* instance = realm->aotInstance();
    return instance && instance->state ? instance : nullptr;
}

static Instance* instanceOfTables(JSGlobalObject* realm)
{
    Instance* instance = realm->aotTableInstance();
    return instance && instance->state ? instance : nullptr;
}

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
    if (representation == SlotRepresentation::DeferredClosure && *static_cast<const uint32_t*>(field) < minCell32)
        return { };
    JSValue value = readSlot(representation, field, instance);
    if (!value && slot.has(Slot::absentIsUndefined))
        return jsUndefined();
    return value;
}

static JSValue valueInStorage(const Slot& slot, JSCell* base)
{
    return valueAtOffset(asObject(slot.has(Slot::isOwn) ? base : slot.holder), slot.offsetInStorage());
}

static GetterSetter* accessorPairInStorage(const Slot& slot, JSCell* base)
{
    JSValue word = valueInStorage(slot, base);
    return word && word.isCell() ? dynamicDowncast<GetterSetter>(word.asCell()) : nullptr;
}

static JSValue callCustomGetter(Instance& instance, const Slot& slot, JSCell* base, const Identifier& ident)
{
    const CustomAccessorRecord& record = instance.state->propertyCaches->tables().customAccessors().at(slot.customAccessor.index);
    if (record.offsetOfCachedValue) {
        if (JSValue cachedValue = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(reinterpret_cast<const uint8_t*>(base) + record.offsetOfCachedValue)))
            return cachedValue;
    }
    return Interpreter::callCustomGetter(record, base, ident);
}

static JSValue readBySlot(Instance& instance, const Slot& slot, JSCell* base, const Identifier& ident)
{
    switch (slot.kind) {
    case Slot::Kind::Value:
        return valueInStorage(slot, base);
    case Slot::Kind::Getter: {
        GetterSetter* pair = accessorPairInStorage(slot, base);
        return pair ? pair->callGetter(instance.globalObject, base) : JSValue();
    }
    case Slot::Kind::FarValue:
        return asObject(base)->getDirect(slot.offsetInStorage());
    case Slot::Kind::Absent:
        return jsUndefined();
    case Slot::Kind::CustomGetter:
        return callCustomGetter(instance, slot, base, ident);
    case Slot::Kind::Field:
    case Slot::fieldOfGetter:
        return readField(instance, slot, *uncheckedDowncast<Object>(base));
    case Slot::Kind::InheritedField:
        return getById(instance, slot.holder, ident);
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
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedAccessor:
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
        RELEASE_AND_RETURN(scope, getAndCache({ &instance, ident, std::nullopt }, base));
    JSValue value = readBySlot(instance, *slot, base, ident);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    RELEASE_AND_RETURN(scope, JSValue(base).get(instance.globalObject, ident));
}

enum class CacheOfOperation : bool { None, HoldsOwnValues };

static void prepareAccessorForCall(JSObject* accessor)
{
    if (accessor->type() != JSFunctionType)
        return;
    ExecutableBase* executable = uncheckedDowncast<JSFunction>(accessor)->executable();
    if (executable->hasJITCodeForCall())
        executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
}

static std::optional<JSValue> tryGetFromOrdinaryObject(Instance& instance, JSCell* base, const Identifier& ident, CacheOfOperation cache)
{
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        return std::nullopt;
    if (cache == CacheOfOperation::HoldsOwnValues && slot->kind == Slot::Kind::Value && slot->has(Slot::isOwn))
        return std::nullopt;
    GetterSetter* pair = slot->kind == Slot::Kind::Getter ? accessorPairInStorage(*slot, base) : nullptr;
    JSValue value = pair ? pair->callGetter(instance.globalObject, base) : readBySlot(instance, *slot, base, ident);
    RETURN_IF_EXCEPTION(scope, JSValue());
    if (pair)
        prepareAccessorForCall(pair->getter());
    if (!value)
        return std::nullopt;
    return value;
}

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
        if (!object.compiledStructure().extraPropertiesSlot())
            return false;
        extraProperties = constructEmptyObject(vm, newStructure->extraPropertiesStructure());
        extraProperties->putDirectOffset(vm, offset, value);
        object.setExtraProperties(vm, extraProperties, newStructure);
        return true;
    }
    extraProperties->putDirectOffset(vm, offset, value);
    extraProperties->setStructure(vm, newStructure->extraPropertiesStructure());
    object.setVariantOfStructure(vm, newStructure);
    return true;
}

static void putById(Instance& instance, JSCell* base, const Identifier& ident, JSValue value, bool isStrict)
{
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Store);
    if (!slot) {
        putAndCache({ &instance, ident, std::nullopt }, base, value, isStrict ? PutMode::Strict : PutMode::Sloppy);
        return;
    }
    if (GetterSetter* pair = slot->kind == Slot::Kind::Setter ? accessorPairInStorage(*slot, base) : nullptr) {
        pair->callSetter(instance.globalObject, base, value, isStrict);
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
    Instance* instance = interpreterInstance(globalObject);
    if (!instance)
        return std::nullopt;
    if (!isCompiledObject)
        return tryGetFromOrdinaryObject(*instance, base, ident, cache);
    return getById(*instance, base, ident);
}

std::optional<JSValue> tryGetById(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident)
{
    return tryGetByName(globalObject, base, ident, CacheOfOperation::HoldsOwnValues);
}

void didPutById(JSGlobalObject* globalObject, JSObject* base, Structure* structureBefore, const Identifier& ident, const PutPropertySlot& slot)
{
    Instance* instance = instanceOfTables(globalObject);
    UniquedStringImpl* uid = ident.impl();
    if (!instance || uid->isSymbol() || base->type() != FinalObjectType)
        return;
    Structure* structure = base->structure();
    bool isAddition = slot.type() == PutPropertySlot::NewProperty;
    if ((isAddition || structure != structureBefore) && (structureBefore->isDictionary() || structure->isDictionary() || (structureBefore->outOfLineCapacity() && structure->outOfLineCapacity() != structureBefore->outOfLineCapacity())))
        return;
    PropertyCaches& caches = *instance->state->propertyCaches;
    if (!caches.findByKey(structureBefore, uid, SlotAccess::Store) && caches.shouldFile(structureBefore->id(), uid, SlotAccess::Store))
        caches.cache({ structureBefore, { uid, std::nullopt }, SlotAccess::Store }, classifyStore({ instance->globalObject, base, structureBefore, uid, slot, slot.isStrictMode() ? PutMode::Strict : PutMode::Sloppy, std::nullopt }));
    if (!slot.isCacheableSetter())
        return;
    if (const Slot* filed = caches.findByKey(base->structure(), uid, SlotAccess::Store); filed && filed->kind == Slot::Kind::Setter) {
        if (GetterSetter* pair = accessorPairInStorage(*filed, base))
            prepareAccessorForCall(pair->setter());
    }
}

bool isClassWithLazyMembers(VM& vm, JSCell* cell)
{
    Closure* closure = uncheckedDowncast<Closure>(cell);
    return closure->kind().isConstructor && closure->classData() && !closure->getDirect(vm, vm.propertyNames->prototype);
}

std::optional<PropertyOffset> fieldOffsetForSiteCache(JSGlobalObject* globalObject, Structure* structure, const Identifier& ident)
{
    Instance* instance = interpreterInstance(globalObject);
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
    static_assert(SlotData::maxOffset < static_cast<uint32_t>(fieldInSiteCache));
    switch (slot->fieldSlot().representation()) {
    case SlotRepresentation::JSValue: {
        if (offset < sizeof(JSObjectWithButterfly) || offset % sizeof(EncodedJSValue))
            return invalidOffset;
        uint32_t property = (offset - sizeof(JSObjectWithButterfly)) / sizeof(EncodedJSValue);
        return fieldInSiteCache + static_cast<PropertyOffset>(property);
    }
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
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
        return invalidOffset;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void didGetById(JSGlobalObject* globalObject, JSCell* base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, bool isStoredInTable)
{
    Instance* instance = base->isObject() ? instanceOfTables(globalObject) : interpreterInstance(globalObject);
    if (!instance || (!isStoredInTable && ((structureBefore->isDictionary() && !slot.isCacheableGetter() && !slot.isUnset()) || slot.isCacheableValue())) || (isJSArray(base) && ident == instance->vm->propertyNames->length))
        return;
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
    Instance* instance = interpreterInstance(globalObject);
    if (!instance)
        return std::nullopt;
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
    Instance* instance = interpreterInstance(globalObject);
    if (!instance)
        return false;
    putById(*instance, base, ident, value, isStrict);
    return true;
}

std::optional<JSValue> tryGetFromContext(JSGlobalObject* realm, const Identifier& ident)
{
    JSObject* contextified = realm->contextifiedObject();
    if (!contextified || (contextified->type() != CompiledObjectType && contextified->type() != FinalObjectType))
        return std::nullopt;
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance* tables = instanceOfTables(realm);
    if (!tables)
        return std::nullopt;
    Instance& instance = *tables;
    UniquedStringImpl* uid = ident.impl();
    PropertyCaches& caches = *instance.state->propertyCaches;
    const Slot* slot = caches.findByKey(contextified->structure(), uid, SlotAccess::Read);
    if (!slot) {
        if (!caches.shouldFile(contextified->structureID(), uid, SlotAccess::Read) || !hasOnlyOrdinaryLookupsOnChain(contextified))
            return std::nullopt;
        hasPropertyAndCache(&instance, contextified, { uid, std::nullopt });
        RETURN_IF_EXCEPTION(scope, JSValue());
        slot = caches.findByKey(contextified->structure(), uid, SlotAccess::Read);
        if (!slot)
            return std::nullopt;
    }
    if (slot->kind == Slot::Kind::Absent) {
        unsigned attributes = 0;
        PropertyOffset offset = realm->structure()->get(vm, uid, attributes);
        if (!isValidOffset(offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return std::nullopt;
        return realm->getDirect(offset);
    }
    if (slot->kind != Slot::Kind::Value && slot->kind != Slot::Kind::FarValue && slot->kind != Slot::Kind::Field && slot->kind != Slot::Kind::ExtraProperty && slot->kind != Slot::Kind::Constant)
        return std::nullopt;
    JSValue value = readBySlot(instance, *slot, contextified, ident);
    RETURN_IF_EXCEPTION(scope, JSValue());
    if (!value || value.isUndefined())
        return std::nullopt;
    return value == contextified ? JSValue(realm->globalThis()) : value;
}

bool tryPutToContext(JSGlobalObject* realm, const Identifier& ident, JSValue value, PutPropertySlot& slot)
{
    JSObject* contextified = realm->contextifiedObject();
    if (!contextified || contextified->structure()->mayBePrototype())
        return false;
    JSObject* holder = contextified;
    if (Object* compiled = compiledObjectOrNull(contextified))
        holder = extraPropertiesIfCreated(*compiled);
    else if (contextified->type() != FinalObjectType)
        return false;
    if (!holder)
        return false;
    VM& vm = realm->vm();
    unsigned attributes = 0;
    Structure* structure = holder->structure();
    PropertyOffset offset = structure->get(vm, ident.impl(), attributes);
    if (!isValidOffset(offset) || attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue) || structure->isImmutableProperty(ident.impl()) || structure->mayBePrototype())
        return false;
    structure->didReplaceProperty(offset);
    holder->putDirectOffset(vm, offset, value);
    JSGlobalObject::put(realm, realm, ident, value, slot);
    return true;
}

bool tryPutByVal(JSGlobalObject* globalObject, JSCell* base, JSValue key, JSValue value, bool isStrict)
{
    UniquedStringImpl* uid = nameOfKey(key);
    if (!uid || isPreparedBeforeLookups(globalObject->vm(), uid))
        return false;
    return tryPutById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid), value, isStrict);
}

std::optional<bool> tryInstanceOf(JSGlobalObject* globalObject, JSValue value, JSValue constructor)
{
    Instance* instance = interpreterInstance(globalObject);
    if (!instance)
        return std::nullopt;
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
    return word & allocatorTag && word != notPlannedYet ? std::bit_cast<const ObjectLiteralPlan*>(word & ~allocatorTag) : nullptr;
}

uint8_t* ObjectLiteralPlan::allocateCell(VM& vm) const
{
    auto* words = static_cast<uint64_t*>(Allocator(allocator).allocate(vm.heap, allocator->cellSize(), nullptr, AllocationFailureMode::Assert));
    words[0] = cellHeader;
    words[1] = secondWord;
    std::fill_n(words + offsetOfClearedWords / sizeof(uint64_t), numberOfClearedWords, 0);
    return reinterpret_cast<uint8_t*>(words);
}

std::unique_ptr<ObjectLiteralPlan> ObjectLiteralPlan::create(unsigned numberOfValues)
{
    std::unique_ptr<ObjectLiteralPlan> plan { new (NotNull, fastMalloc(sizeof(ObjectLiteralPlan) + numberOfValues * sizeof(uint16_t))) ObjectLiteralPlan };
    plan->numberOfValues = numberOfValues;
    return plan;
}

std::span<uint16_t> ObjectLiteralPlan::valueOffsets()
{
    return unsafeMakeSpan(reinterpret_cast<uint16_t*>(this + 1), numberOfValues);
}

std::span<const uint16_t> ObjectLiteralPlan::valueOffsets() const
{
    return unsafeMakeSpan(reinterpret_cast<const uint16_t*>(this + 1), numberOfValues);
}

void ObjectLiteralPlan::setClearedWords(unsigned numberOfWordsBehindSecond)
{
    unsigned numberOfFilledWords = 0;
    while (numberOfFilledWords < numberOfWordsBehindSecond && std::ranges::contains(valueOffsets(), (2 + numberOfFilledWords) * sizeof(uint64_t)))
        ++numberOfFilledWords;
    offsetOfClearedWords = (2 + numberOfFilledWords) * sizeof(uint64_t);
    numberOfClearedWords = numberOfWordsBehindSecond - numberOfFilledWords;
}

bool ObjectLiteralPlans::storesValuesUnchanged(const ObjectBuilderPlan& builder)
{
    RuntimeLayoutTable& layouts = builder.m_state.objects->layouts;
    bool slotsHoldEveryValue = std::ranges::all_of(builder.m_fields, [&](const ObjectBuilderPlan::PlannedField& planned) {
        const FieldData& field = *planned.field;
        return field.slot.representation() == SlotRepresentation::JSValue && !field.holdsRawValueFromBirth && layouts.typeOf(field).takesEveryValue();
    });
    bool otherWordsAreZero = std::ranges::all_of(builder.m_initialWords, [](const ObjectBuilderPlan::InitialWord& initial) {
        return initial.offset == offsetOfOrderWord || !initial.word;
    });
    return builder.m_namesEveryFieldOfBirth && slotsHoldEveryValue && otherWordsAreZero;
}

std::unique_ptr<ObjectLiteralPlan> ObjectLiteralPlans::createPlan(Instance& instance, std::span<UniquedStringImpl* const> names)
{
    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && offsetOfFirstField == 2 * sizeof(uint64_t));
    State& state = *instance.state;
    std::unique_ptr<ObjectBuilderPlan> builder = ObjectBuilder::tryCreatePlanOfSmallestRecord(state, names, storesValuesUnchanged);
    if (!builder)
        return nullptr;

    Structure* structure = state.objects->structures.objectStructure(*instance.vm, builder->layout().shape);
    state.immortalCells->add(structure);
    std::unique_ptr<ObjectLiteralPlan> plan = ObjectLiteralPlan::create(builder->m_fastFields.size());
    plan->allocator = builder->m_allocator.localAllocator();
    plan->cellHeader = cellHeaderFor(structure);
    for (const ObjectBuilderPlan::InitialWord& initial : builder->m_initialWords) {
        if (initial.offset == offsetOfOrderWord)
            plan->secondWord = initial.word;
    }
    std::ranges::transform(builder->m_fastFields, plan->valueOffsets().begin(), &ObjectBuilderPlan::FastField::offset);
    plan->setClearedWords((builder->m_initialBody.size() + sizeof(JSCell) - offsetOfFirstField + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    return plan;
}

const ObjectLiteralPlan* ObjectLiteralPlans::findOrCreate(Instance& instance, std::span<UniquedStringImpl* const> names)
{
    for (const Entry& entry : m_entries) {
        if (std::ranges::equal(entry.names, names, { }, &RefPtr<UniquedStringImpl>::get))
            return entry.plan.get();
    }
    std::unique_ptr<ObjectLiteralPlan> plan = createPlan(instance, names);
    if (!plan)
        return nullptr;
    m_entries.append({ { }, WTF::move(plan) });
    m_entries.last().names.appendContainerWithMapping(names, [](UniquedStringImpl* name) {
        return RefPtr { name };
    });
    return m_entries.last().plan.get();
}

void didLinkNewObject(ObjectAllocationProfile& profile)
{
    allocatorWord(profile) = ObjectLiteralPlan::notPlannedYet;
}

JSArray* createEmptyArray(JSGlobalObject* globalObject, JSC::ArrayAllocationProfile& profile)
{
    constexpr unsigned vectorLength = BASE_CONTIGUOUS_VECTOR_LEN_EMPTY;
    Instance* instance = interpreterInstance(globalObject);
    IndexingType indexingType = profile.selectIndexingType();
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(hasUndecided(indexingType) ? ArrayWithInt32 : indexingType);
    bool hasStorageInCell = hasInt32(structure->indexingType()) || hasContiguous(structure->indexingType());
    JSArray* array = instance && hasStorageInCell && !structure->outOfLineCapacity() ? tryCreateArrayWithStorage(globalObject->vm(), structure, 0, vectorLength) : nullptr;
    if (!array)
        return constructEmptyArray(globalObject, &profile);
    instance->findMissingShortArrayAllocator(vectorLength);
    return profile.updateLastAllocation(array);
}

void planObjectLiteral(JSGlobalObject* globalObject, CodeBlock* codeBlock, const JSInstruction* instruction, ObjectAllocationProfile& profile)
{
    if (allocatorWord(profile) != ObjectLiteralPlan::notPlannedYet)
        return;

    VM& vm = globalObject->vm();
    allocatorWord(profile) = std::bit_cast<uintptr_t>(subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(profile.structure()->inlineCapacity()), AllocatorForMode::EnsureAllocator).localAllocator());

    Instance* instance = interpreterInstance(globalObject);
    if (!instance || codeBlock->unlinkedCodeBlock()->isBuiltinFunction())
        return;

    VirtualRegister object = instruction->as<OpNewObject>().m_dst;
    Vector<UniquedStringImpl*, 16> names;
    for (const JSInstruction* next = instruction + instruction->size(); next->width() != OpcodeSize::Wide32 && next->is<OpPutById>(); next += next->size()) {
        auto store = next->as<OpPutById>();
        if (store.m_base != object || store.m_value == object || !store.m_flags.isDirect())
            break;
        names.append(codeBlock->identifier(store.m_property).impl());
    }
    if (names.isEmpty())
        return;

    std::unique_ptr<ObjectLiteralPlans>& plans = instance->state->objects->caches->objectLiteralPlans;
    if (!plans)
        plans = makeUnique<ObjectLiteralPlans>();
    if (const ObjectLiteralPlan* plan = plans->findOrCreate(*instance, names.span()))
        allocatorWord(profile) = std::bit_cast<uintptr_t>(plan) | ObjectLiteralPlan::allocatorTag;
}

void checkSymbolTableOffsets(SymbolTable& table)
{
    auto* bytes = reinterpret_cast<uint8_t*>(&table);
    RELEASE_ASSERT(reinterpret_cast<uint8_t*>(&table.singleton()) == bytes + SymbolTableOffsets::offsetOfSingleton);
    RELEASE_ASSERT(*reinterpret_cast<uint32_t*>(bytes + SymbolTableOffsets::offsetOfMaxScopeOffset) + 1 == table.scopeSize());
    uintptr_t singleton = *reinterpret_cast<uintptr_t*>(bytes + SymbolTableOffsets::offsetOfSingleton);
    bool isThin = singleton & 1;
    RELEASE_ASSERT(table.singleton().isStillValid() || !isThin || singleton == SymbolTableOffsets::invalidatedSingleton);
}

void didCreateFunction(JSGlobalObject* globalObject, FunctionExecutable* executable, JSFunction* function)
{
    if (!executable->singleton().hasBeenInvalidated())
        return;
    Instance* instance = instanceOfTables(globalObject);
    VM& vm = globalObject->vm();
    if (!instance || function->structure()->realm() != globalObject)
        return;
    Allocator allocator = allocatorForConcurrently<JSFunction>(vm, sizeof(JSFunction), AllocatorForMode::EnsureAllocator);
    if (!allocator)
        return;
    uint64_t header = function->structureID().bits() | static_cast<uint64_t>(std::bit_cast<uint32_t>(function->structure()->typeInfoBlob())) << 32;
    instance->state->propertyCaches->remember(FunctionAllocationEntry { executable, header, allocator.localAllocator() });
}

void didCreateClonedArguments(JSGlobalObject* globalObject)
{
    if (Instance* instance = instanceOfTables(globalObject))
        instance->propertyCaches.clonedArgumentsAllocator = allocatorForConcurrently<ClonedArguments>(globalObject->vm(), sizeof(ClonedArguments), AllocatorForMode::EnsureAllocator).localAllocator();
}

void rememberSwitchTarget(JSGlobalObject* globalObject, const JSInstruction* instruction, const StringImpl& atom, int32_t offset)
{
    ASSERT(atom.isAtom());
    if (Instance* instance = instanceOfTables(globalObject))
        instance->state->propertyCaches->remember(StringSwitchTarget { &atom, instruction, offset });
}

void fuseVariableReads(CodeBlock& codeBlock, std::span<const JSInstruction* const> resolutions)
{
    static_assert(ClosureVar < 1u << FusedClosureVariableRead::typeBits);
    for (const JSInstruction* instruction : resolutions) {
        if (instruction->width() != OpcodeSize::Narrow)
            continue;
        auto resolution = instruction->as<OpResolveScope>();
        auto& metadata = resolution.metadata(&codeBlock);
        const JSInstruction* next = instruction + instruction->size();
        if (next->width() != OpcodeSize::Narrow || !next->is<OpGetFromScope>())
            continue;
        auto read = next->as<OpGetFromScope>();
        auto& metadataOfRead = read.metadata(&codeBlock);
        if (read.m_scope != resolution.m_dst || metadataOfRead.m_getPutInfo.resolveType() != metadata.m_resolveType)
            continue;
        using Kind = FusedClosureVariableRead::Kind;
        if (metadata.m_resolveType != ClosureVar) {
            ASSERT(metadata.m_resolveType == GlobalVar || metadata.m_resolveType == GlobalLexicalVar);
            uint64_t address = metadataOfRead.m_operand;
            if (!address || address >> (64 - FusedClosureVariableRead::offsetShift))
                continue;
            Kind kind = read.m_dst == resolution.m_dst ? Kind::ReadGlobalIntoScopeRegister : Kind::ReadGlobal;
            static_assert(OBJECT_OFFSETOF(OpResolveScope::Metadata, m_localScopeDepth) == OBJECT_OFFSETOF(OpResolveScope::Metadata, m_resolveType) + sizeof(uint32_t));
            metadata.m_resolveType = static_cast<ResolveType>(metadata.m_resolveType | kind << FusedClosureVariableRead::typeBits | static_cast<uint32_t>(address << FusedClosureVariableRead::offsetShift));
            metadata.m_localScopeDepth = static_cast<uint32_t>(address >> (32 - FusedClosureVariableRead::offsetShift));
            continue;
        }
        if (metadataOfRead.m_operand > FusedClosureVariableRead::maxOffset)
            continue;
        const JSInstruction* afterRead = next + next->size();
        bool isChecked = afterRead->width() == OpcodeSize::Narrow && afterRead->is<OpCheckTdz>() && afterRead->as<OpCheckTdz>().m_targetVirtualRegister == read.m_dst;
        if (isChecked)
            afterRead += afterRead->size();
        Kind kind = isChecked ? Kind::ReadAndCheck : Kind::Read;
        if (read.m_dst == resolution.m_dst)
            kind = isChecked ? Kind::ReadIntoScopeRegisterAndCheck : Kind::ReadIntoScopeRegister;
        if (metadata.m_localScopeDepth)
            kind = static_cast<Kind>(kind - Kind::Read + Kind::ReadOuter);
        metadata.m_resolveType = static_cast<ResolveType>(ClosureVar | kind << FusedClosureVariableRead::typeBits | metadataOfRead.m_operand << FusedClosureVariableRead::offsetShift);
    }
}

} // namespace Interpreter

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
