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
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "InternalFieldTuple.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <array>
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
static constexpr uint32_t starRulesOfReuse = starRuleReusesDescriptors | starRuleSourceIsMarkedAsESModule;
static constexpr unsigned attributesOfStarBinding = PropertyAttribute::Accessor | PropertyAttribute::DontDelete;

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

private:
    enum Slot : unsigned { sourceSlot, previousSlot, lastStarOfSourceSlot, orderAndCountOfSourceSlot, getterSlot, ruleSlot, bindingsSlot, numberOfSlots };

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
    captures[keySlot] = JSValue::encode(key);
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
    Vector<Star, 8> starsOfSource = starsInOrderUpTo(star.lastStarOfSource());

    uint32_t orderAndCount = star.orderAndCountOfSource();
    std::span<const uint16_t> fieldsOfSource = state.objects->keyOrders.entriesInOrder(layout, orderOf(orderAndCount)).first(countOf(orderAndCount));
    size_t maxNumberOfBindings = fieldsOfSource.size();
    for (Star starOfSource : starsOfSource)
        maxNumberOfBindings += starOfSource.bindings()->length() / 2;
    JSCellButterfly* bindings = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, static_cast<unsigned>(2 * maxNumberOfBindings));

    unsigned end = 0;
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();
    auto isReExported = [&](UniquedStringImpl* name) {
        return name != defaultName && !namesOfTarget.contains(name);
    };
    auto append = [&](JSString* key, GetterSetter* accessor) {
        bindings->setIndex(vm, end++, key);
        bindings->setIndex(vm, end++, accessor);
    };

    for (uint16_t index : fieldsOfSource) {
        ASSERT(index < layout.numberOfFields);
        const FieldData& field = layout.fields()[index];
        if (field.isPrivate() || field.flags & fieldIsNotEnumerable || !layouts.hasStringKey(field))
            continue;
        UniquedStringImpl* name = layouts.nameOf(vm, field);
        if (!isReExported(name))
            continue;
        JSString* key = jsOwnedString(vm, String(name));
        append(key, createAccessorOfStar(vm, state, realm, star, key));
    }
    bool reusesAccessors = (star.rule() & starRulesOfReuse) == starRulesOfReuse;
    for (Star starOfSource : starsOfSource) {
        forEachBinding(starOfSource.bindings(), [&](JSString* key, GetterSetter* accessor) {
            if (isReExported(nameOfBindingKey(key)))
                append(key, reusesAccessors ? accessor : createAccessorOfStar(vm, state, realm, star, key));
        });
    }
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

static JSObject* makePendingStars(Object& target, InternalFieldTuple* pendingStars)
{
    VM& vm = target.vm();
    DeferGCForAWhile deferGC(vm);
    JSGlobalObject* realm = target.structure()->realm();
    Star last = Star::lastOf(pendingStars);
    bindStarsUpTo(vm, target.state(), realm, last, target.layout());

    Vector<Object::ExtraProperty, 32> properties;
    for (Star star : starsInOrderUpTo(last)) {
        count(target.instance(), RuntimeCounterID::StarsCarriedOutLate);
        forEachBinding(star.bindings(), [&](JSString* key, GetterSetter* accessor) {
            properties.append(Object::ExtraProperty { nameOfBindingKey(key), accessor });
        });
    }
    for (auto& property : properties)
        target.willAddExtraProperty(vm, property.name);
    JSObject* extra = createExtraPropertiesWithAttributes(realm, properties.span(), attributesOfStarBinding);
    writeExtraPropertiesWord(target, extra);
    vm.writeBarrier(&target, extra);
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
    if (!layout.keysAreNeverEnumerated)
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
    if (!hasEveryFieldOfLayout(target))
        return false;
    return !target.hasExtraProperties() || pendingStarsOf(target);
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

    count(target.instance(), RuntimeCounterID::StarsRecorded);
    InternalFieldTuple* pendingStars = pendingStarsOf(target);
    Star star = Star::create(vm, source, pendingStars ? Star::lastOf(pendingStars) : Star(), lastStarOfSource, getter, rule);
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

static BindingOfStars findBindingOfStars(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget, UniquedStringImpl* name)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const FieldData* fieldOfTarget = layouts.field(vm, layoutOfTarget, name);
    if (fieldOfTarget && !fieldOfTarget->isPrivate())
        return { };
    for (Star star : starsInOrderUpTo(last)) {
        Object& source = star.source();
        const LayoutData& layout = source.layout();
        const FieldData* field = layouts.field(vm, layout, name);
        if (field && !field->isPrivate()) {
            if (!(field->flags & fieldIsNotEnumerable) && layouts.hasStringKey(*field) && listsField(state, layout, star.orderAndCountOfSource(), *field))
                return { &source, &source, field };
            continue;
        }
        BindingOfStars found = findBindingOfStars(vm, state, star.lastStarOfSource(), layout, name);
        if (!found.field)
            continue;
        if ((star.rule() & starRulesOfReuse) != starRulesOfReuse)
            found.sourceOfGetter = &source;
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
        if (source->orderWord() & orderWordHasFieldOutsideSlot || (source->hasExtraProperties() && !pendingStarsOf(*source)))
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

static void forEachNameOfStars(VM& vm, State& state, Star last, const LayoutData& layoutOfTarget, NameSet& seen, const ScopedLambda<void(UniquedStringImpl*)>& functor)
{
    RuntimeLayoutTable& layouts = state.objects->layouts;
    for (const FieldData& field : layoutOfTarget.fields()) {
        if (!field.isPrivate())
            seen.add(layouts.nameOf(vm, field));
    }
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();
    for (Star star : starsInOrderUpTo(last)) {
        const LayoutData& layout = star.source().layout();
        uint32_t orderAndCount = star.orderAndCountOfSource();
        for (uint16_t index : state.objects->keyOrders.entriesInOrder(layout, orderOf(orderAndCount)).first(countOf(orderAndCount))) {
            const FieldData& field = layout.fields()[index];
            if (field.isPrivate() || field.flags & fieldIsNotEnumerable || !layouts.hasStringKey(field))
                continue;
            UniquedStringImpl* name = layouts.nameOf(vm, field);
            if (name != defaultName && seen.add(name).isNewEntry)
                functor(name);
        }
        NameSet seenBySource;
        forEachNameOfStars(vm, state, star.lastStarOfSource(), layout, seenBySource, [&](UniquedStringImpl* name) {
            if (name != defaultName && seen.add(name).isNewEntry)
                functor(name);
        });
    }
}

void forEachBindingInSnapshotOfPendingStars(VM& vm, const Object& target, JSCellButterfly* snapshot, const ScopedLambda<void(UniquedStringImpl*, JSValue)>& functor)
{
    NameSet seen;
    Star last = Star::fromRecord(snapshot->get(lastStarInSnapshot));
    forEachNameOfStars(vm, target.state(), last, target.layout(), seen, [&](UniquedStringImpl* name) {
        functor(name, valueInSnapshotOfPendingStars(vm, target, snapshot, name));
    });
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
    if (!pendingStars || name->isSymbol() || parseIndex(*name))
        return std::nullopt;
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
