/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInterpreterSlowPaths.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectBuilder.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTPropertyCachesNamedAccess.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertySlot.h"
#include <bit>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static JSValue getById(Instance&, JSCell* base, const Identifier&);

static Instance* runningInstanceOf(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance && instance->state ? instance : nullptr;
}

static NamedAccess namedAccess(Instance& instance, const Identifier& ident)
{
    return { &instance, instance.globalObject, *instance.vm, *instance.state->propertyCaches, ident, std::nullopt, nullptr };
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

static JSValue callGetterInField(Instance& instance, const Slot& slot, Object& object)
{
    const void* field = storageOfField(slot, object);
    JSValue accessor = field ? readSlot(slot.fieldSlot().representation(), field, *instance.state->strings) : JSValue();
    if (!accessor || !accessor.isCell())
        return slot.has(Slot::absentIsUndefined) ? jsUndefined() : JSValue();
    return uncheckedDowncast<GetterSetter>(accessor.asCell())->callGetter(instance.globalObject, &object);
}

// The value of a data property does not depend on the receiver, so the holder is asked for it as for a property of its own.
static JSValue readFieldOrInheritedField(Instance& instance, const Slot& slot, Object& object, const Identifier& ident)
{
    const void* field = storageOfField(slot, object);
    if (!field)
        return { };
    if (!slotHoldsAbsent(slot.fieldSlot().representation(), field))
        return readField(instance, slot, object);
    return getById(instance, MarkedBlockCage::decode(instance.cellCageBase, slot.fieldAndHolder.holder), ident);
}

static JSValue readExtraProperty(const Slot& slot, Object& object)
{
    JSObject* extraProperties = object.extraProperties();
    return extraProperties ? extraProperties->getDirect(Slot::offsetFor(slot.extraProperty.wordIndex)) : JSValue();
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
    JSCell* thisCell = record.slotBase ? record.slotBase : base;
    return JSValue::decode(std::bit_cast<GetValueFunc>(record.function)(record.globalObject, JSValue::encode(thisCell), ident));
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
    case Slot::Kind::FieldGetter:
        return callGetterInField(instance, slot, *uncheckedDowncast<Object>(base));
    case Slot::Kind::InheritedField:
        return getById(instance, slot.holder, ident);
    case Slot::Kind::FieldOrInheritedField:
        return readFieldOrInheritedField(instance, slot, *uncheckedDowncast<Object>(base), ident);
    case Slot::Kind::ExtraProperty:
        return readExtraProperty(slot, *uncheckedDowncast<Object>(base));
    case Slot::Kind::Constant:
        return JSValue::decode(slot.constant);
    case Slot::Kind::Empty:
    case Slot::Kind::Setter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::Transition:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Uncacheable:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue getById(Instance& instance, JSCell* base, const Identifier& ident)
{
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        RELEASE_AND_RETURN(scope, getAndCache(namedAccess(instance, ident), base));
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

static bool isHeldByCacheOfInterpreter(VM& vm, JSObject* base, const Identifier& ident, const PropertySlot& slot)
{
    return slot.isCacheableValue() || (isJSArray(base) && ident == vm.propertyNames->length);
}

static std::optional<bool> isFieldPresent(const Slot& slot, Object& object)
{
    const void* field = storageOfField(slot, object);
    if (!field)
        return std::nullopt;
    if (!slotHoldsAbsent(slot.field.slot.representation(), field))
        return true;
    if (slot.has(Slot::absentIsUndefined))
        return false;
    return std::nullopt;
}

static std::optional<bool> isPresentBySlot(const Slot& slot, JSObject* base)
{
    switch (Slot::presenceOf(slot.kind)) {
    case SlotPresence::Unknown:
        return std::nullopt;
    case SlotPresence::Present:
        return true;
    case SlotPresence::Absent:
        return false;
    case SlotPresence::ReadsSlot:
        return isFieldPresent(slot, *uncheckedDowncast<Object>(base));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool inById(Instance& instance, JSObject* base, const Identifier& ident)
{
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Read);
    if (!slot)
        return hasPropertyAndCache(&instance, base, { ident.impl(), std::nullopt });
    if (std::optional<bool> isPresent = isPresentBySlot(*slot, base))
        return *isPresent;
    return base->hasProperty(instance.globalObject, ident);
}

// A marker that the type of the field does not list is left to Object::store(), which may have to set the mark of the cell.
static bool slotAdmits(const RuntimeLayoutTable& layouts, const Slot::FieldPayload& field, JSValue value)
{
    return layouts.valueHasType(layouts.typeAt(field.type), value);
}

static bool tryStoreField(Instance& instance, const Slot& slot, Object& object, JSValue value)
{
    SlotRepresentation representation = slot.field.slot.representation();
    void* field = storageOfField(slot, object);
    if (!field || object.isFrozen() || slotHoldsAbsent(representation, field))
        return false;
    State& state = *instance.state;
    bool takesAnyValue = slot.kind == Slot::Kind::FieldCreation && slot.has(Slot::remembersOrder);
    if ((!takesAnyValue && !slotAdmits(state.objects->layouts, slot.field, value)) || !tryWriteSlot(*state.strings, representation, field, value))
        return false;
    if (traitsOf(representation).holdsCell)
        instance.vm->writeBarrier(&object, value);
    return true;
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

static bool tryReplaceExtraProperty(Instance& instance, const Slot& slot, Object& object, JSValue value)
{
    JSObject* extraProperties = object.extraProperties();
    if (!extraProperties)
        return false;
    extraProperties->putDirectOffset(*instance.vm, Slot::offsetFor(slot.extraProperty.wordIndex), value);
    return true;
}

static void putById(Instance& instance, JSCell* base, const Identifier& ident, JSValue value, bool isStrict)
{
    const Slot* slot = instance.state->propertyCaches->findByKey(base->structure(), ident.impl(), SlotAccess::Store);
    if (!slot) {
        putAndCache(namedAccess(instance, ident), base, value, isStrict ? PutMode::Strict : PutMode::Sloppy);
        return;
    }
    bool isField = slot->kind == Slot::Kind::Field || slot->kind == Slot::Kind::FieldCreation;
    if (isField && tryStoreField(instance, *slot, *uncheckedDowncast<Object>(base), value))
        return;
    if (slot->kind == Slot::Kind::Setter) {
        uncheckedDowncast<GetterSetter>(valueInStorage(*slot, base))->callSetter(instance.globalObject, base, value, isStrict);
        return;
    }
    if (slot->kind == Slot::Kind::ExtraPropertyAddition && tryAddExtraProperty(instance, *slot, *uncheckedDowncast<Object>(base), value))
        return;
    if (slot->kind == Slot::Kind::ExtraProperty && tryReplaceExtraProperty(instance, *slot, *uncheckedDowncast<Object>(base), value))
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

static std::optional<JSValue> tryGetByName(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident, CacheOfOperation cache)
{
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterReadSlowPaths);
    if (base->type() != CompiledObjectType)
        return tryGetFromOrdinaryObject(*instance, base, ident, cache);
    return getById(*instance, base, ident);
}

std::optional<JSValue> tryGetById(JSGlobalObject* globalObject, JSCell* base, const Identifier& ident)
{
    return tryGetByName(globalObject, base, ident, CacheOfOperation::HoldsOwnValues);
}

std::optional<PropertyOffset> placeOfFieldInSiteCache(JSGlobalObject* globalObject, Structure* structure, const Identifier& ident)
{
    Instance* instance = runningInstanceOf(globalObject);
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
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance || structureBefore->isDictionary() || isHeldByCacheOfInterpreter(*instance->vm, base, ident, slot))
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
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return std::nullopt;
    count(*instance, RuntimeCounterID::InterpreterPresenceSlowPaths);
    return inById(*instance, base, ident);
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
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return false;
    count(*instance, RuntimeCounterID::InterpreterStoreSlowPaths);
    putById(*instance, base, ident, value, isStrict);
    return true;
}

bool tryPutByVal(JSGlobalObject* globalObject, JSCell* base, JSValue key, JSValue value, bool isStrict)
{
    UniquedStringImpl* uid = nameOfKey(key);
    return uid && tryPutById(globalObject, base, Identifier::fromUid(globalObject->vm(), uid), value, isStrict);
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
    return word & allocatorTag && word != allocatorTag ? std::bit_cast<const ObjectLiteralPlan*>(word - allocatorTag) : nullptr;
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

std::unique_ptr<ObjectLiteralPlan> ObjectLiteralPlans::createPlanForRecord(Instance& instance, std::span<UniquedStringImpl* const> names)
{
    VM& vm = *instance.vm;
    State& state = *instance.state;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::optional<uint32_t> lastName = vm.aotState().identifiers->numberOf(names.back());
    if (!lastName)
        return nullptr;

    std::unique_ptr<ObjectBuilderPlan> best;
    for (const ParsedRecordFieldData& row : std::ranges::equal_range(state.tables.parsedRecordFields(), *lastName, { }, &ParsedRecordFieldData::name)) {
        if (row.isForValidator())
            continue;
        const LayoutData& layout = layouts.layoutForShape(row.shape);
        if (layout.numberOfFields < names.size() || (best && best->layout().numberOfFields <= layout.numberOfFields) || !ObjectBuilder::canBuildWithoutCompilerKnowledge(layout))
            continue;
        bool hasEveryName = std::ranges::all_of(names, [&](UniquedStringImpl* name) {
            return !!layouts.field(vm, layout, name);
        });
        bool everyFieldAcceptsEveryValue = std::ranges::all_of(layout.fields(), [&](const FieldData& field) {
            return typeAcceptsEveryValue(layouts.typeOf(field));
        });
        if (!hasEveryName || !everyFieldAcceptsEveryValue)
            continue;
        std::unique_ptr<ObjectBuilderPlan> builderPlan = ObjectBuilder::tryCreatePlan(state, layout, names);
        if (!builderPlan || !builderPlan->hasFastPath() || !builderPlan->m_namesEveryFieldOfBirth)
            continue;
        bool storesValuesUnchanged = std::ranges::all_of(builderPlan->m_fields, [&](const ObjectBuilderPlan::PlannedField& planned) {
            const FieldData& field = *planned.field;
            return field.slot.representation() == SlotRepresentation::JSValue && !field.holdsRawValueFromBirth && !field.keepsStateOfBirth;
        });
        bool otherWordsAreZero = std::ranges::all_of(builderPlan->m_initialWords, [](const ObjectBuilderPlan::InitialWord& initial) {
            return initial.offset == offsetOfOrderWord || !initial.word;
        });
        if (storesValuesUnchanged && otherWordsAreZero)
            best = WTF::move(builderPlan);
    }
    if (!best)
        return nullptr;

    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && offsetOfFirstField == 2 * sizeof(uint64_t));
    auto plan = makeUniqueWithoutFastMallocCheck<ObjectLiteralPlan>();
    plan->allocator = best->m_allocator.localAllocator();
    plan->cellHeader = cellHeaderFor(state.objects->structures.objectStructure(vm, best->layout().shape));
    for (const ObjectBuilderPlan::InitialWord& initial : best->m_initialWords) {
        if (initial.offset == offsetOfOrderWord)
            plan->secondWord = initial.word;
    }
    plan->numberOfValues = names.size();
    for (size_t index = 0; index < names.size(); ++index)
        plan->valueOffsets[index] = best->m_fastFields[index].offset;
    plan->setClearedWords((best->m_initialBody.size() + sizeof(JSCell) - offsetOfFirstField + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    return plan;
}

const ObjectLiteralPlan* ObjectLiteralPlans::findOrCreate(Instance& instance, Structure* emptyStructure, std::span<UniquedStringImpl* const> names, bool mayBeRecord)
{
    for (const Entry& entry : m_entries) {
        if (entry.emptyStructure == emptyStructure && entry.mayBeRecord == mayBeRecord && std::ranges::equal(entry.names, names, { }, &RefPtr<UniquedStringImpl>::get))
            return entry.plan.get();
    }
    if (m_entries.size() == maxNumberOfPlans)
        return nullptr;
    instance.state->immortalCells->add(emptyStructure);
    std::unique_ptr<ObjectLiteralPlan> plan = mayBeRecord ? createPlanForRecord(instance, names) : nullptr;
    Structure* structureOfFinalObject = nullptr;
    if (!plan) {
        plan = makeUniqueWithoutFastMallocCheck<ObjectLiteralPlan>();
        structureOfFinalObject = fillPlanForFinalObject(instance, emptyStructure, names, *plan);
        if (!structureOfFinalObject)
            plan = nullptr;
    }
    m_entries.append({ emptyStructure, structureOfFinalObject, mayBeRecord, { }, WTF::move(plan) });
    m_entries.last().names.appendContainerWithMapping(names, [](UniquedStringImpl* name) {
        return RefPtr { name };
    });
    return m_entries.last().plan.get();
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
    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return;
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
    allocatorWord(profile) = ObjectLiteralPlan::allocatorTag;
}

bool planObjectLiteral(JSGlobalObject* globalObject, CodeBlock* codeBlock, const JSInstruction* instruction, ObjectAllocationProfile& profile)
{
    if (allocatorWord(profile) != ObjectLiteralPlan::allocatorTag)
        return false;

    VM& vm = globalObject->vm();
    Structure* emptyStructure = profile.structure();
    allocatorWord(profile) = std::bit_cast<uintptr_t>(subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(emptyStructure->inlineCapacity()), AllocatorForMode::EnsureAllocator).localAllocator());

    Instance* instance = runningInstanceOf(globalObject);
    if (!instance)
        return true;

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
        plans = makeUnique<ObjectLiteralPlans>();
    if (const ObjectLiteralPlan* plan = plans->findOrCreate(*instance, emptyStructure, names.span(), !codeBlock->unlinkedCodeBlock()->isBuiltinFunction()))
        allocatorWord(profile) = std::bit_cast<uintptr_t>(plan) | ObjectLiteralPlan::allocatorTag;
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
