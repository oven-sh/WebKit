/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "InternalFieldTuple.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "PropertyTable.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <array>
#include <ranges>
#include <tuple>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT {

static JSObject* cellInPlaceOfExtraProperties(const Object& object)
{
    ASSERT(object.hasExtraProperties());
    const CompiledStructure& structure = object.compiledStructure();
    std::optional<SlotData> slot = structure.extraPropertiesSlot();
    if (!slot) [[unlikely]]
        return structure.heldExtraProperties();
    if (slot->representation() == SlotRepresentation::Cell)
        return object.at<JSObject*>(slot->offset);
    ASSERT(slot->representation() == SlotRepresentation::Cell32);
    return asObject(decodeCell32(&object, object.at<uint32_t>(slot->offset)));
}

static void writeExtraPropertiesWord(Object& object, JSObject* extra)
{
    std::optional<SlotData> slot = object.compiledStructure().extraPropertiesSlot();
    if (!slot)
        return;
    if (slot->representation() == SlotRepresentation::Cell) {
        object.at<JSObject*>(slot->offset) = extra;
        return;
    }
    RELEASE_ASSERT(slot->representation() == SlotRepresentation::Cell32);
    object.at<uint32_t>(slot->offset) = encodeCell32(&object, extra);
}

static CompiledStructure* structureForNewExtraProperties(VM& vm, Object& object, JSObject* extra, bool namesStructure)
{
    StructureTable& structures = object.state().objects->structures;
    const CompiledStructure& current = object.compiledStructure();
    if (!current.extraPropertiesSlot())
        return structures.holdingExtraProperties(vm, current, extra);
    CompiledStructure* structure = namesStructure ? structures.withExtraProperties(vm, current, extra->structure()) : structures.existingWithExtraProperties(current, *extra->structure());
    return structure ? structure : structures.withExtraProperties(vm, current, nullptr);
}

static JSObject* createWithStructure(VM& vm, Structure* structure, std::span<const Object::ExtraProperty> inOrder)
{
    int inlineCapacity = structure->inlineCapacity();
    int numberOfProperties = static_cast<int>(inOrder.size());
    Butterfly* butterfly = structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    for (int i = inlineCapacity; i < numberOfProperties; ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(offsetForPropertyNumber(i, inlineCapacity))].setWithoutWriteBarrier(inOrder[i].value);
    JSFinalObject* extra = JSFinalObject::createWithButterfly(vm, structure, butterfly);
    for (int i = 0; i < std::min(numberOfProperties, inlineCapacity); ++i)
        extra->putDirectOffset(vm, offsetForPropertyNumber(i, inlineCapacity), inOrder[i].value);
    return extra;
}

static JSObject* createExtraPropertiesWithAttributes(JSGlobalObject* realm, std::span<const Object::ExtraProperty> inOrder, unsigned attributes)
{
    VM& vm = realm->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Structure* emptyStructure = Instance::of(realm)->realm.emptyExtraPropertiesStructure;
    Vector<UniquedStringImpl*, 16> names;
    names.reserveInitialCapacity(inOrder.size());
    for (auto& property : inOrder)
        names.append(property.name);
    if (Structure* structure = Structure::createWithProperties(vm, emptyStructure, names.span(), attributes))
        return createWithStructure(vm, structure, inOrder);

    JSObject* extra = constructEmptyObject(vm, emptyStructure);
    if (inOrder.size() > static_cast<size_t>(Structure::s_maxTransitionLength))
        extra->convertToDictionary(vm);
    for (auto& property : inOrder) {
        if (auto index = parseIndex(*property.name)) {
            ASSERT(!attributes);
            extra->putDirectIndex(realm, *index, property.value);
            scope.assertNoException();
        } else
            putProperty(vm, extra, property.name, { property.value, attributes });
    }
    return extra;
}

using NameSet = UncheckedKeyHashSet<UniquedStringImpl*>;

static constexpr uint32_t starRuleSourceIsMarkedAsESModule = 1 << 2;
static_assert(!(starRuleSourceIsMarkedAsESModule & (starRuleKeyCaptureSlot | starRuleReusesDescriptors | starRuleKeyIsLiteralStringWord)));
static constexpr uint32_t starRulesOfReuse = starRuleReusesDescriptors | starRuleSourceIsMarkedAsESModule;

class Star {
public:
    Star() = default;

    static Star create(VM& vm, Object& source, Star previous, Star lastStarOfSource, FunctionID getter, uint32_t rule)
    {
        Star star(JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, numberOfSlots));
        star.m_record->setIndex(vm, sourceSlot, &source);
        if (previous)
            star.m_record->setIndex(vm, previousSlot, previous.m_record);
        if (lastStarOfSource)
            star.m_record->setIndex(vm, lastStarOfSourceSlot, lastStarOfSource.m_record);
        star.m_record->setIndex(vm, orderAndCountOfSourceSlot, jsNumber(source.orderWord() & orderAndCountMask));
        star.m_record->setIndex(vm, getterSlot, jsNumber(getter.index()));
        star.m_record->setIndex(vm, ruleSlot, jsNumber(rule));
        return star;
    }

    static Star lastOf(InternalFieldTuple* pendingStars) { return Star(uncheckedDowncast<JSCellButterfly>(pendingStars->getInternalField(0).asCell())); }
    void becomeLastOf(VM& vm, InternalFieldTuple* pendingStars) const { pendingStars->putInternalField(vm, 0, m_record); }
    InternalFieldTuple* createPendingStars(VM& vm, JSGlobalObject* realm) const { return InternalFieldTuple::create(vm, realm->internalFieldTupleStructure(), m_record, jsUndefined()); }

    static Star fromRecord(JSValue record) { return Star(uncheckedDowncast<JSCellButterfly>(record.asCell())); }
    JSCellButterfly* record() const { return m_record; }

    explicit operator bool() const { return m_record; }
    Object& source() const { return *uncheckedDowncast<Object>(m_record->get(sourceSlot).asCell()); }
    Star previous() const { return Star(butterflyIn(previousSlot)); }
    Star lastStarOfSource() const { return Star(butterflyIn(lastStarOfSourceSlot)); }
    uint32_t orderAndCountOfSource() const { return m_record->get(orderAndCountOfSourceSlot).asUInt32(); }
    FunctionID getter() const { return FunctionID(m_record->get(getterSlot).asUInt32()); }
    uint32_t rule() const { return m_record->get(ruleSlot).asUInt32(); }

    JSCellButterfly* bindings() const { return butterflyIn(bindingsSlot); }
    void setBindings(VM& vm, JSCellButterfly* bindings) const { m_record->setIndex(vm, bindingsSlot, bindings); }

    // The fields of the layout of the target that the target lacked at the star and whose names the star binds. No other star binds the name of a field of the target.
    void setFieldsOfTarget(VM& vm, std::span<const uint16_t> fields) const
    {
        JSCellButterfly* indices = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, static_cast<unsigned>(fields.size()));
        for (unsigned i = 0; i < fields.size(); ++i)
            indices->setIndex(vm, i, jsNumber(fields[i]));
        m_record->setIndex(vm, fieldsOfTargetSlot, indices);
    }

    void forEachFieldOfTarget(const Invocable<void(uint16_t)> auto& functor) const
    {
        JSCellButterfly* indices = butterflyIn(fieldsOfTargetSlot);
        for (unsigned i = 0; indices && i < indices->length(); ++i)
            functor(static_cast<uint16_t>(indices->get(i).asUInt32()));
    }

    bool bindsFieldOfTarget(uint16_t index) const
    {
        bool isFound = false;
        forEachFieldOfTarget([&](uint16_t bound) {
            isFound |= bound == index;
        });
        return isFound;
    }

    // The names that the stars of the target bind, in their order, once somebody has asked for them: one table for all its stars. Those of this star and the stars before it come first in it.
    PropertyTable* namesOfStars() const
    {
        JSValue names = m_record->get(namesOfStarsSlot);
        return names ? uncheckedDowncast<PropertyTable>(names.asCell()) : nullptr;
    }
    uint32_t numberOfNamesUpToHere() const { return m_record->get(numberOfNamesUpToHereSlot).asUInt32(); }
    void setNamesOfStars(VM& vm, PropertyTable* names) const
    {
        m_record->setIndex(vm, namesOfStarsSlot, names);
        m_record->setIndex(vm, numberOfNamesUpToHereSlot, jsNumber(names->size()));
    }

private:
    enum Slot : unsigned { sourceSlot, previousSlot, lastStarOfSourceSlot, orderAndCountOfSourceSlot, getterSlot, ruleSlot, bindingsSlot, fieldsOfTargetSlot, namesOfStarsSlot, numberOfNamesUpToHereSlot, numberOfSlots };

    explicit Star(JSCellButterfly* record)
        : m_record(record)
    {
    }

    JSCellButterfly* butterflyIn(Slot slot) const
    {
        JSValue value = m_record->get(slot);
        return value ? uncheckedDowncast<JSCellButterfly>(value.asCell()) : nullptr;
    }

    JSCellButterfly* m_record { nullptr };
};

static Star starThatBindsFieldOfTarget(Star last, uint16_t index)
{
    for (Star star = last; star; star = star.previous()) {
        if (star.bindsFieldOfTarget(index))
            return star;
    }
    return { };
}

static Vector<Star, 8> starsInOrderUpTo(Star last)
{
    Vector<Star, 8> stars;
    for (Star star = last; star; star = star.previous())
        stars.append(star);
    stars.reverse();
    return stars;
}

static UniquedStringImpl* nameOfBindingKey(JSString* key)
{
    return static_cast<UniquedStringImpl*>(key->tryGetValueImpl());
}

static void forEachBinding(JSCellButterfly* bindings, const Invocable<void(JSString*, GetterSetter*)> auto& functor)
{
    for (unsigned i = 0; i < bindings->length() && bindings->get(i); i += 2)
        functor(asString(bindings->get(i)), uncheckedDowncast<GetterSetter>(bindings->get(i + 1)));
}

static void bindStarsUpTo(VM&, State&, JSGlobalObject* realm, Star last, const LayoutData& layoutOfTarget);

static GetterSetter* createAccessorOfStar(VM& vm, State& state, JSGlobalObject* realm, Star star, JSString* key)
{
    unsigned keySlot = star.rule() & starRuleKeyCaptureSlot;
    std::array<uint64_t, 2> captures;
    captures[keySlot] = star.rule() & starRuleKeyIsLiteralStringWord ? state.strings->literalStringWordFor(key) : JSValue::encode(key);
    captures[!keySlot] = JSValue::encode(&star.source());
    JSObject* getter = state.objects->closures.createClosure(vm, star.getter(), captures, 2);
    JSObject* setter = nullptr;
    return GetterSetter::create(vm, realm, getter, setter);
}

static JSCellButterfly* bindStar(VM& vm, State& state, JSGlobalObject* realm, Star star, const NameSet& namesOfTarget)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = star.source().layout();
    bindStarsUpTo(vm, state, realm, star.lastStarOfSource(), layout);
    Vector<std::pair<JSString*, GetterSetter*>, 32> bindingsOfSource;
    for (Star starOfSource : starsInOrderUpTo(star.lastStarOfSource())) {
        forEachBinding(starOfSource.bindings(), [&](JSString* key, GetterSetter* accessor) {
            bindingsOfSource.append({ key, accessor });
        });
    }

    uint32_t orderAndCount = star.orderAndCountOfSource();
    std::span<const uint16_t> entriesOfSource = state.objects->keyOrders.entriesInOrder(layout, orderOf(orderAndCount)).first(countOf(orderAndCount));
    JSCellButterfly* bindings = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, static_cast<unsigned>(2 * (entriesOfSource.size() + bindingsOfSource.size())));

    unsigned end = 0;
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();
    auto isReExported = [&](UniquedStringImpl* name) {
        return name != defaultName && !namesOfTarget.contains(name);
    };
    auto append = [&](JSString* key, GetterSetter* accessor) {
        bindings->setIndex(vm, end++, key);
        bindings->setIndex(vm, end++, accessor);
    };

    auto bindField = [&](uint16_t index) {
        const FieldData& field = layout.fields()[index];
        if (field.isPrivate() || field.flags & fieldIsNotEnumerable || !layouts.hasStringKey(field))
            return;
        UniquedStringImpl* name = layouts.nameOf(vm, field);
        if (!isReExported(name))
            return;
        JSString* key = jsOwnedString(vm, String(name));
        append(key, createAccessorOfStar(vm, state, realm, star, key));
    };
    bool reusesAccessors = (star.rule() & starRulesOfReuse) == starRulesOfReuse;
    auto bindBindingOfSource = [&](size_t index) {
        auto [key, accessor] = bindingsOfSource[index];
        if (isReExported(nameOfBindingKey(key)))
            append(key, reusesAccessors ? accessor : createAccessorOfStar(vm, state, realm, star, key));
    };
    size_t next = Object::forEachPositionedKey(entriesOfSource, layout.numberOfFields, bindingsOfSource.size(), bindField, bindBindingOfSource);
    for (; next < bindingsOfSource.size(); ++next)
        bindBindingOfSource(next);
    return bindings;
}

static void bindStarsUpTo(VM& vm, State& state, JSGlobalObject* realm, Star last, const LayoutData& layoutOfTarget)
{
    if (!last || last.bindings())
        return;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    NameSet namesOfTarget;
    for (const FieldData& field : layoutOfTarget.fields()) {
        if (!field.isPrivate())
            namesOfTarget.add(layouts.nameOf(vm, field));
    }
    for (Star star : starsInOrderUpTo(last)) {
        star.forEachFieldOfTarget([&](uint16_t index) {
            namesOfTarget.remove(layouts.nameOf(vm, layoutOfTarget.fields()[index]));
        });
        if (!star.bindings())
            star.setBindings(vm, bindStar(vm, state, realm, star, namesOfTarget));
        forEachBinding(star.bindings(), [&](JSString* key, GetterSetter*) {
            namesOfTarget.add(nameOfBindingKey(key));
        });
    }
}

static InternalFieldTuple* pendingStarsOf(const Object& object)
{
    if (!object.hasExtraProperties())
        return nullptr;
    JSObject* cell = cellInPlaceOfExtraProperties(object);
    return cell->type() == InternalFieldTupleType ? uncheckedDowncast<InternalFieldTuple>(cell) : nullptr;
}

// While stars wait the key order counts each of their keys as an extra key. A key under the name of a field is listed as that field.
static void listFieldsBoundByStars(VM& vm, State& state, Object& target, std::span<const Object::ExtraProperty> properties)
{
    const LayoutData& layout = target.layout();
    if (layout.keysAreNeverEnumerated)
        return;
    Vector<uint16_t, 32> keys;
    ExtraKeyCounter positioned(layout.numberOfFields);
    uint32_t numberOfExtraKeys = 0;
    auto listField = [&](uint16_t index) {
        positioned.appendEntriesCountingTo(numberOfExtraKeys, keys);
        keys.append(index);
    };
    auto listKeyOfStars = [&](size_t index) {
        if (const FieldData* field = target.fieldNamed(state, properties[index].name))
            listField(layout.indexOf(*field));
        else
            ++numberOfExtraKeys;
    };
    Object::KeyOrderStorage storage;
    size_t next = Object::forEachPositionedKey(target.keyOrder(state, storage), layout.numberOfFields, properties.size(), listField, listKeyOfStars);
    for (; next < properties.size(); ++next)
        listKeyOfStars(next);
    target.setKeyOrder(vm, state, keys.span());
}

static JSObject* makePendingStars(Object& target, InternalFieldTuple* pendingStars)
{
    VM& vm = target.vm();
    DeferGCForAWhile deferGC(vm);
    JSGlobalObject* realm = target.structure()->realm();
    Star last = Star::lastOf(pendingStars);
    // Whoever lists the keys may be code of another image. The getters are closures of the image of the object.
    RunningImageScope imageOfTarget(vm.aotState(), target.state().instance.program);
    bindStarsUpTo(vm, target.state(), realm, last, target.layout());

    Vector<Object::ExtraProperty, 32> properties;
    for (Star star : starsInOrderUpTo(last)) {
        count(target.instance(), RuntimeCounterID::StarsCarriedOutLate);
        forEachBinding(star.bindings(), [&](JSString* key, GetterSetter* accessor) {
            properties.append(Object::ExtraProperty { nameOfBindingKey(key), accessor });
        });
    }
    State& state = target.state();
    bool bindsFields = target.orderWord() & orderWordHasFieldOutsideSlot;
    auto fieldBoundBy = [&](const Object::ExtraProperty& property) {
        return bindsFields ? target.fieldNamed(state, property.name) : nullptr;
    };
    if (bindsFields)
        listFieldsBoundByStars(vm, state, target, properties.span());
    // A field leaves its slot for extra properties that are there, so from the first binding under the name of a field on they are made one by one.
    size_t numberMadeAtOnce = std::ranges::find_if(properties, fieldBoundBy) - properties.begin();
    for (auto& property : properties) {
        if (!fieldBoundBy(property))
            target.willAddExtraProperty(vm, property.name);
    }
    JSObject* extra = createExtraPropertiesWithAttributes(realm, properties.span().first(numberMadeAtOnce), attributesOfStarBinding);
    writeExtraPropertiesWord(target, extra);
    vm.writeBarrier(&target, extra);
    for (auto& property : properties.span().subspan(numberMadeAtOnce)) {
        if (const FieldData* field = fieldBoundBy(property)) {
            target.moveFieldToExtraProperties(vm, *field, { property.value, attributesOfStarBinding }, SlotExitReason::Redefinition);
            continue;
        }
        putProperty(vm, extra, property.name, { property.value, attributesOfStarBinding });
        target.didChangeExtraProperties(vm);
    }
    return extra;
}

static bool hasEnumerableStringKeys(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->typeInfo().overridesAnyFormOfGetOwnPropertyNames() || object->hasNonReifiedStaticProperties() || hasIndexedProperties(structure->indexingType()))
        return true;
    bool isFound = false;
    structure->forEachProperty(vm, [&](const auto& property) {
        isFound = !(property.attributes() & PropertyAttribute::DontEnum) && !property.key()->isSymbol();
        return !isFound;
    });
    return isFound;
}

static bool hasKeyOrderThatStaysKnown(const Object& object)
{
    return !(object.orderWord() & orderWordIntegrityBits) && !object.layout().keysAreNeverEnumerated && !object.hasOutOfLineKeyOrder() && !object.isInFrame();
}

static bool hasEveryFieldOfLayout(const Object& object)
{
    const LayoutData& layout = object.layout();
    // The order of an object with extra properties may also count those.
    if (!layout.keysAreNeverEnumerated && !object.hasExtraProperties())
        return countOf(object.orderWord()) == layout.numberOfFields;
    return std::ranges::all_of(layout.fields(), [&](const FieldData& field) {
        return object.isInSlot(field);
    });
}

static bool canGetPendingStars(const Object& target)
{
    const LayoutData& layout = target.layout();
    bool hasNoOrderToKeep = layout.keysAreNeverEnumerated && !(target.orderWord() & orderWordIntegrityBits) && !target.isInFrame();
    if ((!hasNoOrderToKeep && !hasKeyOrderThatStaysKnown(target)) || target.structure()->mayBePrototype())
        return false;
    if (layout.isClass() || !layout.extraPropertiesSlot() || layout.impossibleMutations().contains(ObjectMutation::AddProperty))
        return false;
    return !target.hasExtraProperties() || pendingStarsOf(target);
}

static bool listsField(State&, const LayoutData&, uint32_t orderAndCount, const FieldData&);
static bool bindsName(VM&, State&, Star, UniquedStringImpl* name);

// A field that the target gets later is listed behind the names of the star: the run time lists a field that an object with extra properties gets, and asks numberOfKeysOfPendingStars() for it.
// The name of a field that the target lacks is bound like any other name that it lacks. Its accessor will lie outside the slot, and the order word says so from here on, so that nothing takes the empty slot for a missing property.
static bool tryBindFieldsThatTargetLacks(VM& vm, Object& target, Star star)
{
    if (hasEveryFieldOfLayout(target))
        return true;
    State& state = target.state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = target.layout();
    Vector<uint16_t, 4> bound;
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate() || !layouts.hasStringKey(field) || target.isInSlot(field))
            continue;
        if (field.flags & fieldMayExistWithoutStorage && !layout.keysAreNeverEnumerated && listsField(state, layout, target.orderWord() & orderAndCountMask, field))
            continue;
        uint16_t index = layout.indexOf(field);
        if (starThatBindsFieldOfTarget(star.previous(), index) || !bindsName(vm, state, star, layouts.nameOf(vm, field)))
            continue;
        if (!field.canLeaveSlot() || field.keepsStateOfBirth)
            return false;
        bound.append(index);
    }
    if (bound.isEmpty())
        return true;
    star.setFieldsOfTarget(vm, bound.span());
    target.at<uint32_t>(offsetOfOrderWord) |= orderWordHasFieldOutsideSlot;
    target.setPerCellBit(true);
    return true;
}

static bool canBeSourceOfPendingStar(VM& vm, JSGlobalObject* realm, const Object& source)
{
    if (!hasKeyOrderThatStaysKnown(source) || (source.hasExtraProperties() && !pendingStarsOf(source)))
        return false;
    for (const FieldData& field : source.layout().fields()) {
        if (field.flags & fieldAttributeFlags && !(field.flags & fieldIsNotEnumerable))
            return false;
    }
    JSObject* objectPrototype = realm->objectPrototype();
    return source.getPrototypeDirect() == JSValue(objectPrototype) && !hasEnumerableStringKeys(vm, objectPrototype);
}

static bool descriptorsHaveOnlyOwnMembers(JSGlobalObject* realm)
{
    if (realm->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        realm->tryInstallPropertyDescriptorFastPathWatchpoint();
    return realm->propertyDescriptorFastPathWatchpointSet().isStillValid() && realm->objectPrototypeChainIsSane();
}

static std::optional<bool> isMarkedAsESModule(VM& vm, JSGlobalObject* realm, const Object& source)
{
    const FieldData* mark = source.fieldNamed(source.state(), vm.propertyNames->__esModule);
    if (!mark || mark->isAccessor() || mark->slot.representation() == SlotRepresentation::DeferredClosure)
        return std::nullopt;
    JSValue value = source.read(*mark);
    if (!value)
        return std::nullopt;
    return value.toBoolean(realm);
}

bool tryAddPendingStar(VM& vm, Object& target, Object& source, FunctionID getter, uint32_t rule)
{
    ASSERT(!(rule & ~(starRuleKeyCaptureSlot | starRuleReusesDescriptors | starRuleKeyIsLiteralStringWord)));
    JSGlobalObject* realm = target.structure()->realm();
    if (&target == &source || source.structure()->realm() != realm)
        return false;
    if (vm.aotState().hasSeveralImages && &source.state() != &target.state()) [[unlikely]]
        return false;
    if (!canGetPendingStars(target) || !canBeSourceOfPendingStar(vm, realm, source) || !descriptorsHaveOnlyOwnMembers(realm))
        return false;

    Star lastStarOfSource;
    if (InternalFieldTuple* pendingStarsOfSource = pendingStarsOf(source)) {
        lastStarOfSource = Star::lastOf(pendingStarsOfSource);
        if (rule & starRuleReusesDescriptors) {
            std::optional<bool> isMarked = isMarkedAsESModule(vm, realm, source);
            if (!isMarked)
                return false;
            if (*isMarked)
                rule |= starRuleSourceIsMarkedAsESModule;
        }
    }
    validateKeyListing(vm, source);
    if (!lastStarOfSource && !countOf(source.orderWord()))
        return true;

    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    Star star = Star::create(vm, source, pendingStars ? Star::lastOf(pendingStars) : Star(), lastStarOfSource, getter, rule);
    if (!tryBindFieldsThatTargetLacks(vm, target, star))
        return false;
    count(target.instance(), RuntimeCounterID::StarsRecorded);
    if (pendingStars) {
        star.becomeLastOf(vm, pendingStars);
        return true;
    }
    target.setExtraProperties(vm, star.createPendingStars(vm, realm), target.state().objects->structures.withExtraProperties(vm, target.compiledStructure(), nullptr));
    return true;
}

bool hasPendingStars(const Object& object)
{
    return pendingStarsOf(object);
}

static bool listsField(State& state, const LayoutData& layout, uint32_t orderAndCount, const FieldData& field)
{
    std::span<const uint16_t> listed = state.objects->keyOrders.entriesInOrder(layout, orderOf(orderAndCount)).first(countOf(orderAndCount));
    uint16_t index = layout.indexOf(field);
    if (index < listed.size() && listed[index] == index)
        return true;
    return WTF::find16(listed.data(), index, listed.size());
}

struct BindingOfStars {
    Object* sourceOfGetter { nullptr };
    Object* holderOfField { nullptr };
    const FieldData* field { nullptr };
};

static BindingOfStars findBindingOfStars(VM&, State&, Star last, const LayoutData& layoutOfTarget, UniquedStringImpl* name);

// What the star binds the name to if the target lacks the name.
static BindingOfStars findBindingOfStar(VM& vm, State& state, Star star, UniquedStringImpl* name)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    Object& source = star.source();
    const LayoutData& layout = source.layout();
    const FieldData* field = layouts.field(vm, layout, name);
    if (field && !field->isPrivate() && listsField(state, layout, star.orderAndCountOfSource(), *field)) {
        if (field->flags & fieldIsNotEnumerable || !layouts.hasStringKey(*field))
            return { };
        return { &source, &source, field };
    }
    BindingOfStars found = findBindingOfStars(vm, state, star.lastStarOfSource(), layout, name);
    if (found.field && (star.rule() & starRulesOfReuse) != starRulesOfReuse)
        found.sourceOfGetter = &source;
    return found;
}

static bool bindsName(VM& vm, State& state, Star star, UniquedStringImpl* name)
{
    return name != vm.propertyNames->defaultKeyword.impl() && findBindingOfStar(vm, state, star, name).field;
}

static BindingOfStars findBindingOfStars(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget, UniquedStringImpl* name)
{
    const FieldData* fieldOfTarget = state.objects->layouts.field(vm, layoutOfTarget, name);
    if (fieldOfTarget && !fieldOfTarget->isPrivate()) {
        Star star = starThatBindsFieldOfTarget(last, layoutOfTarget.indexOf(*fieldOfTarget));
        return star ? findBindingOfStar(vm, state, star, name) : BindingOfStars { };
    }
    for (Star star : starsInOrderUpTo(last)) {
        if (BindingOfStars found = findBindingOfStar(vm, state, star, name); found.field)
            return found;
    }
    return { };
}

static constexpr unsigned lastStarInSnapshot = 0;
static constexpr unsigned firstSourceInSnapshot = 1;

static void collectSources(Star last, Vector<Object*, 8>& sources)
{
    for (Star star = last; star; star = star.previous()) {
        if (!sources.contains(&star.source()))
            sources.append(&star.source());
        collectSources(star.lastStarOfSource(), sources);
    }
}

static JSCellButterfly* valuesInSnapshot(JSCellButterfly* snapshot, const Object& source)
{
    for (unsigned i = firstSourceInSnapshot; i < snapshot->length(); i += 2) {
        if (snapshot->get(i).asCell() == &source)
            return uncheckedDowncast<JSCellButterfly>(snapshot->get(i + 1).asCell());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSCellButterfly* snapshotOfPendingStars(VM& vm, const Object& target)
{
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    if (!pendingStars)
        return nullptr;
    DeferGCForAWhile deferGC(vm);
    Star last = Star::lastOf(pendingStars);
    Vector<Object*, 8> sources;
    collectSources(last, sources);
    JSCellButterfly* snapshot = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, firstSourceInSnapshot + 2 * sources.size());
    snapshot->setIndex(vm, lastStarInSnapshot, last.record());
    unsigned end = firstSourceInSnapshot;
    for (Object* source : sources) {
        if (source->hasExtraProperties() && !pendingStarsOf(*source))
            return nullptr;
        const LayoutData& layout = source->layout();
        JSCellButterfly* values = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, layout.numberOfFields);
        for (const FieldData& field : layout.fields()) {
            if (field.isPrivate() || field.isAccessor())
                continue;
            JSValue held = source->readInline(field);
            if (!held)
                held = source->read(field);
            if (held)
                values->setIndex(vm, layout.indexOf(field), held);
        }
        snapshot->setIndex(vm, end++, source);
        snapshot->setIndex(vm, end++, values);
    }
    return snapshot;
}

JSValue valueInSnapshotOfPendingStars(VM& vm, const Object& target, JSCellButterfly* snapshot, UniquedStringImpl* name)
{
    if (name->isSymbol() || name == vm.propertyNames->defaultKeyword.impl())
        return { };
    Star last = Star::fromRecord(snapshot->get(lastStarInSnapshot));
    BindingOfStars found = findBindingOfStars(vm, target.state(), last, target.layout(), name);
    if (!found.field)
        return { };
    JSValue held = valuesInSnapshot(snapshot, *found.holderOfField)->get(found.holderOfField->layout().indexOf(*found.field));
    return held ? held : jsUndefined();
}

static PropertyTable* listNamesOfStarsUpTo(VM&, State&, Star last, const LayoutData& layoutOfTarget);

// The names that a listing of the source showed at the star, in its order.
static void forEachNameOfSource(VM& vm, State& state, Star star, const Invocable<void(UniquedStringImpl*)> auto& functor)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = star.source().layout();
    Star lastStarOfSource = star.lastStarOfSource();
    PropertyTable* namesOfStars = lastStarOfSource ? listNamesOfStarsUpTo(vm, state, lastStarOfSource, layout) : nullptr;
    size_t numberOfNamesOfStars = lastStarOfSource ? lastStarOfSource.numberOfNamesUpToHere() : 0;
    auto nameOfStars = [&](size_t index) {
        functor(namesOfStars->keyOfEntry(static_cast<unsigned>(index) + 1));
    };
    uint32_t orderAndCount = star.orderAndCountOfSource();
    std::span<const uint16_t> entries = state.objects->keyOrders.entriesInOrder(layout, orderOf(orderAndCount)).first(countOf(orderAndCount));
    size_t next = Object::forEachPositionedKey(entries, layout.numberOfFields, numberOfNamesOfStars, [&](uint16_t index) {
        const FieldData& field = layout.fields()[index];
        if (!field.isPrivate() && !(field.flags & fieldIsNotEnumerable) && layouts.hasStringKey(field))
            functor(layouts.nameOf(vm, field));
    }, nameOfStars);
    for (; next < numberOfNamesOfStars; ++next)
        nameOfStars(next);
}

// The names that bindStarsUpTo() binds, in its order. What a star binds is settled at the star, so each star is listed once, behind the stars before it.
static PropertyTable* listNamesOfStarsUpTo(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget)
{
    if (PropertyTable* names = last.namesOfStars())
        return names;
    Vector<Star, 8> starsToList;
    Star lastListed = last;
    for (; lastListed && !lastListed.namesOfStars(); lastListed = lastListed.previous())
        starsToList.append(lastListed);
    PropertyTable* names = lastListed ? lastListed.namesOfStars() : PropertyTable::create(vm, 0);
    ASSERT(!lastListed || names->size() == lastListed.numberOfNamesUpToHere());
    RuntimeLayoutTable& layouts = state.objects->layouts;
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();
    for (Star star : starsToList | std::views::reverse) {
        forEachNameOfSource(vm, state, star, [&](UniquedStringImpl* name) {
            if (name == defaultName)
                return;
            const FieldData* field = layouts.field(vm, layoutOfTarget, name);
            if (field && !field->isPrivate() && !star.bindsFieldOfTarget(layoutOfTarget.indexOf(*field)))
                return;
            std::ignore = names->add(vm, PropertyTableEntry(name, 0, 0));
        });
        star.setNamesOfStars(vm, names);
    }
    return names;
}

std::optional<uint32_t> numberOfKeysOfPendingStars(VM& vm, const Object& target)
{
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    if (!pendingStars)
        return std::nullopt;
    DeferGCForAWhile deferGC(vm);
    Star last = Star::lastOf(pendingStars);
    listNamesOfStarsUpTo(vm, target.state(), last, target.layout());
    return last.numberOfNamesUpToHere();
}

void forEachKeyOfPendingStars(VM& vm, const Object& target, const ScopedLambda<void(UniquedStringImpl*, unsigned attributes)>& functor)
{
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    if (!pendingStars)
        return;
    DeferGCForAWhile deferGC(vm);
    Star last = Star::lastOf(pendingStars);
    PropertyTable* names = listNamesOfStarsUpTo(vm, target.state(), last, target.layout());
    for (unsigned entry = 1; entry <= last.numberOfNamesUpToHere(); ++entry)
        functor(names->keyOfEntry(entry), attributesOfStarBinding);
}

std::optional<bool> isBoundByPendingStar(const Object& target, const FieldData& field)
{
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    if (!pendingStars)
        return std::nullopt;
    return target.orderWord() & orderWordHasFieldOutsideSlot && starThatBindsFieldOfTarget(Star::lastOf(pendingStars), target.layout().indexOf(field));
}

void forEachBindingInSnapshotOfPendingStars(VM& vm, const Object& target, JSCellButterfly* snapshot, const ScopedLambda<void(UniquedStringImpl*, JSValue)>& functor)
{
    Star last = Star::fromRecord(snapshot->get(lastStarInSnapshot));
    PropertyTable* names = listNamesOfStarsUpTo(vm, target.state(), last, target.layout());
    for (unsigned entry = 1; entry <= last.numberOfNamesUpToHere(); ++entry) {
        UniquedStringImpl* name = names->keyOfEntry(entry);
        functor(name, valueInSnapshotOfPendingStars(vm, target, snapshot, name));
    }
}

static uint32_t countNamesOfStars(Star last)
{
    uint32_t numberOfNames = 0;
    for (Star star = last; star; star = star.previous())
        numberOfNames += countOf(star.orderAndCountOfSource()) + countNamesOfStars(star.lastStarOfSource());
    return numberOfNames;
}

std::optional<Object*> sourceOfPendingBinding(VM& vm, const Object& target, UniquedStringImpl* name)
{
    constexpr uint32_t searchesForOneBinding = 8;
    constexpr unsigned searchesLeftField = 1;
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    if (!pendingStars)
        return std::nullopt;
    // A star binds names of fields that are strings, and no field is an element.
    if (name->isSymbol() || parseIndex(*name))
        return nullptr;
    Star last = Star::lastOf(pendingStars);
    JSValue stored = pendingStars->getInternalField(searchesLeftField);
    uint32_t searchesLeft = stored.isUInt32() ? stored.asUInt32() : std::min<uint32_t>(countNamesOfStars(last), INT32_MAX / searchesForOneBinding) * searchesForOneBinding;
    if (!searchesLeft)
        return std::nullopt;
    pendingStars->putInternalField(vm, searchesLeftField, jsNumber(searchesLeft - 1));
    if (name == vm.propertyNames->defaultKeyword.impl())
        return nullptr;
    return findBindingOfStars(vm, target.state(), last, target.layout(), name).sourceOfGetter;
}

JSObject* extraPropertiesIfMade(const Object& object)
{
    if (!object.hasExtraProperties())
        return nullptr;
    JSObject* cell = cellInPlaceOfExtraProperties(object);
    return cell->type() == InternalFieldTupleType ? nullptr : cell;
}

JSObject* Object::extraPropertiesSlow() const
{
    JSObject* cell = cellInPlaceOfExtraProperties(*this);
    if (cell->type() == InternalFieldTupleType) [[unlikely]]
        return makePendingStars(const_cast<Object&>(*this), uncheckedDowncast<InternalFieldTuple>(cell));
    return cell;
}

JSObject* Object::ensureExtraProperties(VM& vm)
{
    if (JSObject* existing = extraProperties())
        return existing;
    JSGlobalObject* realm = structure()->realm();
    JSObject* extra = constructEmptyObject(vm, Instance::of(realm)->realm.emptyExtraPropertiesStructure);
    if (isNotExtensible())
        extra->methodTable()->preventExtensions(extra, realm);
    setExtraProperties(vm, extra, structureForNewExtraProperties(vm, *this, extra, false));
    return extra;
}

// What the first property does to the Structure of the extra properties is done before the object gets them, so the Structure of the object is looked for once.
void Object::adoptExtraProperties(VM& vm, JSObject* filled)
{
    ASSERT(!isNotExtensible());
    setExtraProperties(vm, filled, structureForNewExtraProperties(vm, *this, filled, false));
}

void Object::setExtraProperties(VM& vm, JSObject* extra, CompiledStructure* withExtraProperties)
{
    ASSERT(!hasExtraProperties() && withExtraProperties->hasExtraProperties());
    ASSERT(layout().extraPropertiesSlot() || withExtraProperties->heldExtraProperties() == extra);
    writeExtraPropertiesWord(*this, extra);
    vm.mutatorFence();
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties;
    vm.mutatorFence();
    JSCell::setStructure(vm, withExtraProperties);
    vm.writeBarrier(this, extra);
}

JSObject* Object::createExtraProperties(JSGlobalObject* realm, std::span<const ExtraProperty> inOrder)
{
    return createExtraPropertiesWithAttributes(realm, inOrder, static_cast<unsigned>(PropertyAttribute::None));
}

bool Object::tryCreateExtraPropertiesOfNewObject(VM& vm, std::span<const ExtraProperty> inOrder)
{
    ASSERT(!hasExtraProperties() && !hasFlagsInOrderWord());
    const LayoutData& layout = this->layout();
    if (!layout.extraPropertiesSlot() || layout.impossibleMutations().contains(ObjectMutation::AddProperty) || layout.numberOfFields + inOrder.size() > maxKeyOrderEntry)
        return false;
    for (auto& property : inOrder)
        willAddExtraProperty(vm, property.name);
    JSObject* extra = createExtraProperties(structure()->realm(), inOrder);
    setExtraProperties(vm, extra, structureForNewExtraProperties(vm, *this, extra, false));
    return true;
}

// An extra-properties object that became empty stays, and bit 28 stays set: compiled code tests the bit at a member call on behalf of the member read before it.
void Object::didChangeExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return;
    const CompiledStructure& current = compiledStructure();
    StructureID namedStructureID = current.extraPropertiesStructureID();
    if (namedStructureID == extra->structureID())
        return;
    // A dictionary keeps its Structure while it grows, so a Structure that names none goes on saying all that is known.
    if (!namedStructureID && extra->structure()->isDictionary())
        return;
    StructureTable& structures = state().objects->structures;
    CompiledStructure* updatedStructure = structures.existingWithExtraProperties(current, *extra->structure());
    if (!updatedStructure && !namedStructureID)
        return;
    JSCell::setStructure(vm, updatedStructure ? updatedStructure : structures.withExtraProperties(vm, current, nullptr));
}

void Object::ensureStructureForExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return;
    bool wasDictionary = extra->structure()->isDictionary();
    flattenOnce(vm, extra);
    if (wasDictionary && !extra->structure()->isDictionary())
        didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, nullptr);
    const CompiledStructure& current = compiledStructure();
    if (current.extraPropertiesStructureID() == extra->structureID())
        return;
    CompiledStructure* updatedStructure = state().objects->structures.withExtraProperties(vm, current, extra->structure());
    if (updatedStructure == &current)
        return;
    JSCell::setStructure(vm, updatedStructure);
    // The next exit finds the Structure that names these extra properties.
    state().propertyCaches->forgetFieldExits();
}

bool Object::ensureStructureForExtraPropertiesIfNeeded(VM& vm, JSCell* cell)
{
    const CompiledStructure* structure = compiledObjectStructureOrNull(cell->structure());
    if (!structure || !structure->hasExtraProperties() || structure->extraPropertiesStructureID()) [[likely]]
        return false;
    uncheckedDowncast<Object>(cell)->ensureStructureForExtraProperties(vm);
    return cell->structure() != structure;
}

Object::ExtraPropertyStoreResult Object::tryStoreExtraProperty(VM& vm, PropertyName name, JSValue value, OptionSet<ExtraPropertyStoreFlag> flags)
{
    bool isDefinition = flags.contains(ExtraPropertyStoreFlag::IsDefinition);
    bool namesStructure = flags.contains(ExtraPropertyStoreFlag::NamesExtraPropertiesStructure);
    bool isMadeAtRunTime = flags.contains(ExtraPropertyStoreFlag::IsMadeAtRunTime);
    if (!isMadeAtRunTime && !flags.contains(ExtraPropertyStoreFlag::IsKnownNotToBeField) && fieldNamed(state(), name))
        return { };
    unsigned attributes = 0;
    JSObject* extra = extraProperties();
    ASSERT(!flags.contains(ExtraPropertyStoreFlag::IsKnownToBeAbsent) || !extra || !isValidOffset(extra->structure()->get(vm, name)));
    PropertyOffset offset = extra && !flags.contains(ExtraPropertyStoreFlag::IsKnownToBeAbsent) ? extra->structure()->get(vm, name, attributes) : invalidOffset;
    if (!isValidOffset(offset) && !isMadeAtRunTime && (parseIndex(name) || name.isPrivateName() || name == vm.propertyNames->underscoreProto))
        return { };
    if (isValidOffset(offset)) {
        if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue) || (isDefinition && attributes))
            return { };
        extra->putDirectOffset(vm, offset, value);
        return { ExtraPropertyStore::Replaced, offset };
    }
    if (isNotExtensible())
        return { };
    JSGlobalObject* realm = structure()->realm();
    if (!isDefinition && !storeCreatesOwnProperty(realm, structure(), this, name.uid(), isMadeAtRunTime ? StoredName::IsNotSpelledByProgram : StoredName::MayBeSpelledByProgram))
        return { };
    willAddExtraProperty(vm, name, !isMadeAtRunTime);
    bool hadNone = !extra;
    if (hadNone)
        extra = constructEmptyObject(vm, Instance::of(realm)->realm.emptyExtraPropertiesStructure);
    StructureID structureBefore = extra->structureID();
    PutPropertySlot slot(extra);
    extra->putDirect(vm, name, value, slot);
    if (hadNone)
        setExtraProperties(vm, extra, structureForNewExtraProperties(vm, *this, extra, namesStructure));
    else if (namesStructure)
        ensureStructureForExtraProperties(vm);
    else if (extra->structureID() != structureBefore)
        didChangeExtraProperties(vm);
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, name.uid());
    return { ExtraPropertyStore::Added, slot.cachedOffset() };
}

bool Object::repeatsExtraPropertyStore(VM& vm, PropertyName name) const
{
    JSObject* extra = extraProperties();
    Structure* structure = extra ? extra->structure() : Instance::of(this->structure()->realm())->realm.emptyExtraPropertiesStructure;
    if (structure->isDictionary() || isValidOffset(structure->get(vm, name)))
        return true;
    PropertyOffset offset = invalidOffset;
    return Structure::addPropertyTransitionToExistingStructure(structure, name, 0, offset);
}

void Object::willAddExtraProperty(VM& vm, PropertyName name, bool isSpelledByProgram)
{
    validateMutation(vm, *this, name, ObjectMutation::AddProperty);
    ObjectModel& objects = *state().objects;
    objects.layouts.willAddProperty(vm, layout(), name.uid());
    if (isSpelledByProgram)
        objects.classes.didShadowMember(layout(), name.uid());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
