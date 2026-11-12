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
#include "AOTRuntimeReporter.h"
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
#include <wtf/text/StringCommon.h>

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
    if (!current.extraPropertiesSlot()) {
        structures.growFutureCells(current.layout());
        return structures.holdingExtraProperties(vm, current, extra);
    }
    CompiledStructure* structure = namesStructure ? structures.withExtraProperties(vm, current, extra->structure()) : structures.existingWithExtraProperties(current, *extra->structure());
    return structure ? structure : structures.withExtraProperties(vm, current, nullptr);
}

static JSObject* createExtraPropertiesWithStructure(VM& vm, Structure* structure, std::span<const Object::ExtraProperty> inOrder)
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
        return createExtraPropertiesWithStructure(vm, structure, inOrder);

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

using NamesOfStarTarget = UncheckedKeyHashSet<UniquedStringImpl*>;

static constexpr uint32_t starRuleSourceIsMarkedAsESModule = 1 << 2;
static_assert(!(starRuleSourceIsMarkedAsESModule & (starRuleKeyCaptureSlot | starRuleReusesDescriptors)));
static constexpr uint32_t starRulesOfReuse = starRuleReusesDescriptors | starRuleSourceIsMarkedAsESModule;

namespace {

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

struct BindingOfStars {
    Object* holderOfField { nullptr };
    const FieldData* field { nullptr };
};

} // anonymous namespace

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

static void makeForSourcesFirst(Star last, const LayoutData& layoutOfTarget, const Invocable<bool(Star)> auto& isMade, const Invocable<void(Star, const LayoutData&)> auto& make)
{
    struct Pending {
        Star last;
        const LayoutData* layoutOfTarget;
        bool hasListedSources { false };
    };
    Vector<Pending, 8> pending { Pending { last, &layoutOfTarget } };
    while (!pending.isEmpty()) {
        Pending current = pending.last();
        if (!current.last || isMade(current.last) || current.hasListedSources) {
            pending.removeLast();
            if (current.last && !isMade(current.last))
                make(current.last, *current.layoutOfTarget);
            continue;
        }
        pending.last().hasListedSources = true;
        for (Star star = current.last; star && !isMade(star); star = star.previous())
            pending.append(Pending { star.lastStarOfSource(), &star.source().layout() });
    }
}

static UniquedStringImpl* nameOfBindingKey(JSString* key)
{
    return static_cast<UniquedStringImpl*>(key->tryGetValueImpl());
}

static void forEachBindingOfStar(JSCellButterfly* bindings, const Invocable<void(JSString*, GetterSetter*)> auto& functor)
{
    for (unsigned i = 0; i < bindings->length() && bindings->get(i); i += 2)
        functor(asString(bindings->get(i)), uncheckedDowncast<GetterSetter>(bindings->get(i + 1)));
}

static GetterSetter* createAccessorOfStar(VM& vm, State& state, JSGlobalObject* realm, Star star, JSString* key)
{
    unsigned keySlot = star.rule() & starRuleKeyCaptureSlot;
    std::array<uint64_t, 2> captures;
    captures[keySlot] = JSValue::encode(key);
    captures[!keySlot] = JSValue::encode(&star.source());
    JSObject* getter = state.objects->closures.createClosure(vm, star.getter(), captures, 2);
    JSObject* setter = nullptr;
    return GetterSetter::create(vm, realm, getter, setter);
}

static JSCellButterfly* bindStar(VM& vm, State& state, JSGlobalObject* realm, Star star, const NamesOfStarTarget& namesOfTarget)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = star.source().layout();
    Vector<std::pair<JSString*, GetterSetter*>, 32> bindingsOfSource;
    for (Star starOfSource : starsInOrderUpTo(star.lastStarOfSource())) {
        forEachBindingOfStar(starOfSource.bindings(), [&](JSString* key, GetterSetter* accessor) {
            bindingsOfSource.append({ key, accessor });
        });
    }

    uint32_t orderAndCount = star.orderAndCountOfSource();
    std::span<const uint16_t> entriesOfSource = state.objects->keyOrders.entriesListedBy(layout, orderAndCount);
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

static void bindStarsWhoseSourcesAreBound(VM& vm, State& state, JSGlobalObject* realm, Star last, const LayoutData& layoutOfTarget)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    NamesOfStarTarget namesOfTarget;
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
        forEachBindingOfStar(star.bindings(), [&](JSString* key, GetterSetter*) {
            namesOfTarget.add(nameOfBindingKey(key));
        });
    }
}

static void bindStarsUpTo(VM& vm, State& state, JSGlobalObject* realm, Star last, const LayoutData& layoutOfTarget)
{
    makeForSourcesFirst(last, layoutOfTarget, [](Star star) {
        return !!star.bindings();
    }, [&](Star upTo, const LayoutData& layout) {
        bindStarsWhoseSourcesAreBound(vm, state, realm, upTo, layout);
    });
}

static InternalFieldTuple* pendingStarsOf(const Object& object)
{
    if (!object.hasExtraProperties())
        return nullptr;
    JSObject* cell = cellInPlaceOfExtraProperties(object);
    return cell->type() == InternalFieldTupleType ? uncheckedDowncast<InternalFieldTuple>(cell) : nullptr;
}

static void listFieldsBoundByStars(VM& vm, State& state, Object& target, std::span<const Object::ExtraProperty> properties)
{
    const LayoutData& layout = target.layout();
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
    size_t next = Object::forEachPositionedKey(target.keyOrder(state), layout.numberOfFields, properties.size(), listField, listKeyOfStars);
    for (; next < properties.size(); ++next)
        listKeyOfStars(next);
    target.setKeyOrder(vm, state, keys.span());
}

static JSObject* makePendingStars(Object& target, InternalFieldTuple* pendingStars)
{
    VM& vm = target.vm();
    DeferGCForAWhile deferGC(vm);
    JSGlobalObject* realm = target.structure()->realm();
    State& state = target.state();
    Star last = Star::lastOf(pendingStars);
    bindStarsUpTo(vm, state, realm, last, target.layout());

    Vector<Object::ExtraProperty, 32> properties;
    for (Star star : starsInOrderUpTo(last)) {
        countEvent(vm, RuntimeCounterID::StarsCarriedOutLate);
        forEachBindingOfStar(star.bindings(), [&](JSString* key, GetterSetter* accessor) {
            properties.append(Object::ExtraProperty { nameOfBindingKey(key), accessor });
        });
    }
    bool bindsFields = target.orderWord() & orderWordHasFieldOutsideSlot;
    auto fieldBoundBy = [&](const Object::ExtraProperty& property) {
        return bindsFields ? target.fieldNamed(state, property.name) : nullptr;
    };
    if (bindsFields)
        listFieldsBoundByStars(vm, state, target, properties.span());
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

static bool hasEnumerableStringKeysOfItsOwn(VM& vm, JSObject* object)
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
    return !(object.orderWord() & orderWordIntegrityBits) && !object.hasOutOfLineKeyOrder() && !object.isInFrame();
}

static bool hasEveryFieldOfLayout(const Object& object)
{
    const LayoutData& layout = object.layout();
    if (!object.hasExtraProperties())
        return countOf(object.orderWord()) == layout.numberOfFields;
    return std::ranges::all_of(layout.fields(), [&](const FieldData& field) {
        return object.isInSlot(field);
    });
}

static bool canGetPendingStars(const Object& target)
{
    const LayoutData& layout = target.layout();
    if (!hasKeyOrderThatStaysKnown(target) || target.structure()->mayBePrototype())
        return false;
    if (layout.isClass() || !layout.extraPropertiesSlot() || layout.impossibleMutations().contains(ObjectMutation::AddProperty))
        return false;
    return !target.hasExtraProperties() || pendingStarsOf(target);
}

static bool orderListsField(State& state, const LayoutData& layout, uint32_t orderAndCount, const FieldData& field)
{
    std::span<const uint16_t> listed = state.objects->keyOrders.entriesListedBy(layout, orderAndCount);
    uint16_t index = layout.indexOf(field);
    if (index < listed.size() && listed[index] == index)
        return true;
    return WTF::find16(listed.data(), index, listed.size());
}

static PropertyTable* listNamesOfStarsUpTo(VM&, State&, Star last, const LayoutData& layoutOfTarget);

static Star starThatBindsName(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget, UniquedStringImpl* name)
{
    if (!last)
        return { };
    unsigned entry = listNamesOfStarsUpTo(vm, state, last, layoutOfTarget)->find(name).entryIndex;
    if (entry == PropertyTable::EmptyEntryIndex || entry > last.numberOfNamesUpToHere())
        return { };
    Star star = last;
    for (Star previous = star.previous(); previous && entry <= previous.numberOfNamesUpToHere(); previous = star.previous())
        star = previous;
    return star;
}

static BindingOfStars findBindingOfStar(VM& vm, State& state, Star star, UniquedStringImpl* name)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    while (star) {
        Object& source = star.source();
        const LayoutData& layout = source.layout();
        const FieldData* field = layouts.field(vm, layout, name);
        if (field && !field->isPrivate() && orderListsField(state, layout, star.orderAndCountOfSource(), *field)) {
            if (field->flags & fieldIsNotEnumerable || !layouts.hasStringKey(*field))
                return { };
            return { &source, field };
        }
        star = starThatBindsName(vm, state, star.lastStarOfSource(), layout, name);
    }
    return { };
}

static bool starBindsName(VM& vm, State& state, Star star, UniquedStringImpl* name)
{
    return name != vm.propertyNames->defaultKeyword.impl() && findBindingOfStar(vm, state, star, name).field;
}

static BindingOfStars findBindingOfStars(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget, UniquedStringImpl* name)
{
    return findBindingOfStar(vm, state, starThatBindsName(vm, state, last, layoutOfTarget, name), name);
}

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
        if (field.flags & fieldMayExistWithoutStorage && orderListsField(state, layout, target.orderWord() & orderAndCountMask, field))
            continue;
        uint16_t index = layout.indexOf(field);
        if (starThatBindsFieldOfTarget(star.previous(), index) || !starBindsName(vm, state, star, layouts.nameOf(vm, field)))
            continue;
        if (!field.canLeaveSlot())
            return false;
        bound.append(index);
    }
    if (bound.isEmpty())
        return true;
    star.setFieldsOfTarget(vm, bound.span());
    target.at<uint32_t>(offsetOfOrderWord) |= orderWordHasFieldOutsideSlot;
    target.setBearsMark(true);
    return true;
}

static bool canBeSourceOfPendingStar(VM& vm, JSGlobalObject* realm, const Object& source)
{
    if (!hasKeyOrderThatStaysKnown(source) || (source.hasExtraProperties() && !pendingStarsOf(source)))
        return false;
    for (const FieldData& field : source.layout().fields()) {
        if (field.flags & fieldAttributeFlags && !(field.flags & fieldIsNotEnumerable) && source.isInSlot(field))
            return false;
    }
    JSObject* objectPrototype = realm->objectPrototype();
    return source.getPrototypeDirect() == JSValue(objectPrototype) && !hasEnumerableStringKeysOfItsOwn(vm, objectPrototype);
}

static bool descriptorsHaveOnlyOwnMembers(JSGlobalObject* realm)
{
    if (realm->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        realm->tryInstallPropertyDescriptorFastPathWatchpoint();
    return realm->propertyDescriptorFastPathWatchpointSet().isStillValid() && realm->objectPrototypeChainIsSane();
}

static std::optional<bool> sourceOfStarIsMarkedAsESModule(VM& vm, JSGlobalObject* realm, const Object& source)
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
    ASSERT(!(rule & ~(starRuleKeyCaptureSlot | starRuleReusesDescriptors)));
    JSGlobalObject* realm = target.structure()->realm();
    if (&target == &source || source.structure()->realm() != realm)
        return false;
    if (!canGetPendingStars(target) || !canBeSourceOfPendingStar(vm, realm, source) || !descriptorsHaveOnlyOwnMembers(realm))
        return false;

    Star lastStarOfSource;
    if (InternalFieldTuple* pendingStarsOfSource = pendingStarsOf(source)) {
        lastStarOfSource = Star::lastOf(pendingStarsOfSource);
        if (rule & starRuleReusesDescriptors) {
            std::optional<bool> isMarked = sourceOfStarIsMarkedAsESModule(vm, realm, source);
            if (!isMarked)
                return false;
            if (*isMarked)
                rule |= starRuleSourceIsMarkedAsESModule;
        }
    }
    if (!lastStarOfSource && !countOf(source.orderWord()))
        return true;

    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    Star star = Star::create(vm, source, pendingStars ? Star::lastOf(pendingStars) : Star(), lastStarOfSource, getter, rule);
    if (!tryBindFieldsThatTargetLacks(vm, target, star))
        return false;
    countEvent(vm, RuntimeCounterID::StarsRecorded);
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

static constexpr unsigned lastStarInSnapshot = 0;
static constexpr unsigned firstSourceInSnapshot = 1;

using CellsSeen = UncheckedKeyHashSet<JSCell*>;

static void forEachStarUpToAndBehind(Star last, CellsSeen& seen, const Invocable<void(Star)> auto& functor)
{
    Vector<Star, 8> pending { last };
    while (!pending.isEmpty()) {
        for (Star star = pending.takeLast(); star && seen.add(star.record()).isNewEntry; star = star.previous()) {
            functor(star);
            pending.append(star.lastStarOfSource());
        }
    }
}

static JSCellButterfly* valuesInSnapshotOfStars(JSCellButterfly* snapshot, const Object& source)
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
    CellsSeen seen;
    Vector<Object*, 8> sources;
    forEachStarUpToAndBehind(last, seen, [&](Star star) {
        if (seen.add(&star.source()).isNewEntry)
            sources.append(&star.source());
    });
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
    DeferGCForAWhile deferGC(vm);
    Star last = Star::fromRecord(snapshot->get(lastStarInSnapshot));
    BindingOfStars found = findBindingOfStars(vm, target.state(), last, target.layout(), name);
    if (!found.field)
        return { };
    JSValue held = valuesInSnapshotOfStars(snapshot, *found.holderOfField)->get(found.holderOfField->layout().indexOf(*found.field));
    return held ? held : jsUndefined();
}

static void forEachNameOfSourceOfStar(VM& vm, State& state, Star star, const Invocable<void(UniquedStringImpl*)> auto& functor)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = star.source().layout();
    Star lastStarOfSource = star.lastStarOfSource();
    PropertyTable* namesOfStars = lastStarOfSource ? lastStarOfSource.namesOfStars() : nullptr;
    size_t numberOfNamesOfStars = lastStarOfSource ? lastStarOfSource.numberOfNamesUpToHere() : 0;
    auto nameOfStars = [&](size_t index) {
        functor(namesOfStars->keyOfEntry(static_cast<unsigned>(index) + 1));
    };
    uint32_t orderAndCount = star.orderAndCountOfSource();
    std::span<const uint16_t> entries = state.objects->keyOrders.entriesListedBy(layout, orderAndCount);
    size_t next = Object::forEachPositionedKey(entries, layout.numberOfFields, numberOfNamesOfStars, [&](uint16_t index) {
        const FieldData& field = layout.fields()[index];
        if (!field.isPrivate() && !(field.flags & fieldIsNotEnumerable) && layouts.hasStringKey(field))
            functor(layouts.nameOf(vm, field));
    }, nameOfStars);
    for (; next < numberOfNamesOfStars; ++next)
        nameOfStars(next);
}

static void listNamesOfStarsWhoseSourcesAreListed(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget)
{
    Vector<Star, 8> starsToList;
    Star lastListed = last;
    for (; lastListed && !lastListed.namesOfStars(); lastListed = lastListed.previous())
        starsToList.append(lastListed);
    PropertyTable* names = lastListed ? lastListed.namesOfStars() : PropertyTable::create(vm, 0);
    ASSERT(!lastListed || names->size() == lastListed.numberOfNamesUpToHere());
    RuntimeLayoutTable& layouts = state.objects->layouts;
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();
    for (Star star : starsToList | std::views::reverse) {
        forEachNameOfSourceOfStar(vm, state, star, [&](UniquedStringImpl* name) {
            if (name == defaultName)
                return;
            const FieldData* field = layouts.field(vm, layoutOfTarget, name);
            if (field && !field->isPrivate() && !star.bindsFieldOfTarget(layoutOfTarget.indexOf(*field)))
                return;
            std::ignore = names->add(vm, PropertyTableEntry(name, 0, 0));
        });
        star.setNamesOfStars(vm, names);
    }
}

static PropertyTable* listNamesOfStarsUpTo(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget)
{
    if (PropertyTable* names = last.namesOfStars())
        return names;
    makeForSourcesFirst(last, layoutOfTarget, [](Star star) {
        return !!star.namesOfStars();
    }, [&](Star upTo, const LayoutData& layout) {
        listNamesOfStarsWhoseSourcesAreListed(vm, state, upTo, layout);
    });
    return last.namesOfStars();
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
    CellsSeen seen;
    uint32_t numberOfNames = 0;
    forEachStarUpToAndBehind(last, seen, [&](Star star) {
        numberOfNames += countOf(star.orderAndCountOfSource());
    });
    return numberOfNames;
}

std::optional<Object*> sourceOfPendingBinding(VM& vm, const Object& target, UniquedStringImpl* name)
{
    constexpr uint32_t searchesForOneBinding = 8;
    constexpr unsigned searchesLeftField = 1;
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    if (!pendingStars)
        return std::nullopt;
    if (name->isSymbol() || parseIndex(*name))
        return nullptr;
    DeferGCForAWhile deferGC(vm);
    Star last = Star::lastOf(pendingStars);
    JSValue stored = pendingStars->getInternalField(searchesLeftField);
    uint32_t searchesLeft = stored.isUInt32() ? stored.asUInt32() : std::min<uint32_t>(countNamesOfStars(last), INT32_MAX / searchesForOneBinding) * searchesForOneBinding;
    if (!searchesLeft)
        return std::nullopt;
    pendingStars->putInternalField(vm, searchesLeftField, jsNumber(searchesLeft - 1));
    if (name == vm.propertyNames->defaultKeyword.impl())
        return nullptr;
    return findBindingOfStars(vm, target.state(), last, target.layout(), name).holderOfField;
}

JSObject* extraPropertiesIfCreated(const Object& object)
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

void Object::adoptExtraProperties(VM& vm, JSObject* filled)
{
    ASSERT(!isNotExtensible());
    setExtraProperties(vm, filled, structureForNewExtraProperties(vm, *this, filled, false));
}

void Object::setExtraProperties(VM& vm, JSObject* extra, CompiledStructure* withExtraProperties)
{
    ASSERT(!hasExtraProperties() && withExtraProperties->hasExtraProperties());
    ASSERT(withExtraProperties->extraPropertiesSlot() || withExtraProperties->heldExtraProperties() == extra);
    vm.mutatorFence();
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
    if (!compiledStructure().extraPropertiesSlot() || layout.impossibleMutations().contains(ObjectMutation::AddProperty))
        return false;
    for (auto& property : inOrder)
        willAddExtraProperty(vm, property.name);
    JSObject* extra = createExtraProperties(structure()->realm(), inOrder);
    setExtraProperties(vm, extra, structureForNewExtraProperties(vm, *this, extra, false));
    return true;
}

void Object::didChangeExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return;
    const CompiledStructure& current = compiledStructure();
    if (Structure* listed = current.heldStructure()) [[unlikely]] {
        if (listed != extra->structure())
            JSCell::setStructure(vm, state().objects->structures.withoutHeldStructure(vm, current));
        return;
    }
    StructureID namedStructureID = current.extraPropertiesStructureID();
    if (namedStructureID == extra->structureID())
        return;
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
    if (current.heldExtraProperties()) [[unlikely]] {
        didChangeExtraProperties(vm);
        tryNameHeldStructure(vm);
        return;
    }
    if (current.extraPropertiesStructureID() == extra->structureID())
        return;
    CompiledStructure* updatedStructure = state().objects->structures.withExtraProperties(vm, current, extra->structure());
    if (updatedStructure == &current)
        return;
    JSCell::setStructure(vm, updatedStructure);
    state().propertyCaches->forgetFieldExits();
    if (!wasDictionary)
        state().propertyCaches->forgetAddedExtraProperties();
}

bool Object::tryNameHeldStructure(VM& vm)
{
    const CompiledStructure& current = compiledStructure();
    JSObject* held = current.heldExtraProperties();
    if (!held || held->type() != FinalObjectType)
        return false;
    Structure* structure = held->structure();
    if (Structure* named = current.heldStructure())
        return named == structure;
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence())
        return false;
    current.setHeldStructure(vm, structure);
    return true;
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
        if (structure()->mayBePrototype() && compiledStructure().reportsReplacements()) [[unlikely]] {
            countEvent(vm, RuntimeCounterID::ValuesReplacedOnCompiledPrototypes);
            didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Change, name.uid());
        }
        return { ExtraPropertyStore::Replaced, offset };
    }
    if (isNotExtensible())
        return { };
    JSGlobalObject* realm = structure()->realm();
    if (!isDefinition && !storeCreatesOwnProperty(realm, structure(), this, name.uid(), isMadeAtRunTime ? StoredName::IsNotSpelledByProgram : StoredName::MayBeSpelledByProgram))
        return { };
    if (!mayAddExtraProperty(vm, name, !isMadeAtRunTime))
        return { };
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

bool Object::mayAddExtraProperty(VM& vm, PropertyName name, bool isSpelledByProgram)
{
    if (!name.isPrivateName() && !admitsMutation(vm, *this, ObjectMutation::AddProperty))
        return false;
    if (isSpelledByProgram)
        state().objects->classes.didShadowMember(layout(), name.uid());
    return true;
}

void Object::willAddExtraProperty(VM& vm, PropertyName name, bool isSpelledByProgram)
{
    if (!name.isPrivateName())
        validateMutation(vm, *this, name, ObjectMutation::AddProperty);
    if (isSpelledByProgram)
        state().objects->classes.didShadowMember(layout(), name.uid());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
