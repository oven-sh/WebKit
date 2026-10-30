/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAssignmentPlans.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTKeyOrderTable.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeCounters.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <algorithm>
#include <wtf/PrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

ExcludedNameList::ExcludedNameList(Vector<UniquedStringImpl*>&& names, uint32_t runNumberPlusOne)
    : m_names(WTF::move(names))
    , m_runNumberPlusOne(runNumberPlusOne)
{
    if (m_names.size() <= maxNumberOfNamesCompared)
        return;
    m_longList.reserveInitialCapacity(static_cast<unsigned>(m_names.size()));
    for (UniquedStringImpl* name : m_names)
        m_longList.add(name);
}

AssignmentPlanCache::AssignmentPlanCache(State& state)
    : m_state(state)
    , m_chainDependentOwner(state.chainDependents->addOwner(*this))
    , m_sets(state.tableArena)
{
    state.caches->add(CacheKind::WeakByKey, "assignment plans"_s, *this);
    if (Options::aotCountsPath())
        m_copyCounts = makeUnique<UncheckedKeyHashMap<String, uint32_t>>();
}

AssignmentPlanCache::~AssignmentPlanCache() = default;

void AssignmentPlanCache::note(ASCIILiteral kind, ASCIILiteral what)
{
    if (m_copyCounts)
        ++m_copyCounts->add(makeString(kind, what), 0).iterator->value;
}

void AssignmentPlanCache::dumpCopies(PrintStream& out) const
{
    if (!m_copyCounts)
        return;
    auto lines = copyToVector(m_copyCounts->keys());
    AOT::sort(lines, [](const String& a, const String& b) {
        return codePointCompareLessThan(a, b);
    });
    for (const String& line : lines)
        out.println(m_copyCounts->get(line), "\t"_s, line);
}

NEVER_INLINE const ExcludedNameList& AssignmentPlanCache::addExcludedNames(VM& vm, const ExcludedRun& run)
{
    ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
    Vector<UniquedStringImpl*> names(run.identifiers.size(), [&](size_t index) {
        return identifiers.identifier(run.identifiers[index]).impl();
    });
    auto list = makeUnique<ExcludedNameList>(WTF::move(names), run.numberPlusOne);
    const ExcludedNameList& added = *list;
    if (!added.names().empty())
        m_excludedListsByAddress.add(added.names().data(), &added);
    m_excludedListsByRun.add(run.numberPlusOne, WTF::move(list));
    return added;
}

const ExcludedNameList* AssignmentPlanCache::excludedListAt(std::span<UniquedStringImpl* const> names) const
{
    if (names.empty())
        return nullptr;
    const ExcludedNameList* list = m_excludedListsByAddress.get(names.data());
    return list && list->names().size() == names.size() ? list : nullptr;
}

bool AssignmentPlanCache::isSecondCopy(uint32_t hash)
{
    if (!m_copiesWithoutPlan)
        m_copiesWithoutPlan = makeUnique<RememberedCopies>();
    if (m_copiesWithoutPlan->testAndSet(hash % numberOfCopiesRemembered))
        return true;
    if (++m_numberOfCopiesWithoutPlan > numberOfCopiesRemembered / 2) {
        m_copiesWithoutPlan->clearAll();
        m_numberOfCopiesWithoutPlan = 0;
    }
    return false;
}

NEVER_INLINE AssignmentPlan* AssignmentPlanCache::makePlan(VM& vm, JSObject* target, JSObject* extraProperties, const AssignmentSource& source, uint32_t hash)
{
    if (!m_sets)
        m_sets.make();
    unsigned setIndex = hash % numberOfSets;
    Set& set = (*m_sets)[setIndex];
    auto way = std::ranges::find_if(set.ways, [](auto& candidate) {
        return !candidate || !candidate->target;
    });
    if (way == set.ways.end())
        way = set.ways.begin() + set.nextVictim++ % waysPerSet;
    if (!*way)
        *way = makeUnique<AssignmentPlan>();
    AssignmentPlan& plan = **way;
    plan = { };
    plan.target = target->structureID();
    plan.source = source.object->structureID();
    plan.sourcePropertyStorage = source.propertyStorageStructureID();
    plan.sourceStructureNamesPropertyStorage = !source.compiledObject || source.compiledObject->compiledStructure().extraPropertiesStructureID() == plan.sourcePropertyStorage;
    plan.extraPropertiesBefore = extraProperties ? extraProperties->structureID() : StructureID();
    plan.sourceOrderWord = source.orderWord;
    plan.servesDefinitionsOnly = source.defines;
    plan.excludedList = excludedListAt(source.excludedNames);
    if (!plan.excludedList)
        plan.excludedNames.appendRange(source.excludedNames.begin(), source.excludedNames.end());
    didWritePlan(plan);
    count(vm, RuntimeCounterID::AssignmentPlansMade);
    fillPlan(vm, plan, setIndex * waysPerSet + static_cast<unsigned>(way - set.ways.begin()), target, extraProperties, source);
    didWritePlan(plan);
    return &plan;
}

void AssignmentPlanCache::dropChainDependent(VM&, uint32_t subject, UniquedStringImpl*)
{
    if (AssignmentPlan* plan = planAt(subject))
        *plan = { };
}

bool AssignmentPlanCache::hasChainDependent(VM&, uint32_t subject, UniquedStringImpl*) const
{
    const AssignmentPlan* plan = planAt(subject);
    return plan && plan->target;
}

size_t AssignmentPlanCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t removed = 0;
    auto isDead = [&](JSCell* cellOrNull) {
        return cellOrNull && !vm.heap.isMarked(cellOrNull);
    };
    auto isDeadStructure = [&](StructureID idOrEmpty) {
        return idOrEmpty && isDead(idOrEmpty.decode());
    };
    auto removeIfDead = [&](AssignmentPlan& plan) {
        if (isDeadStructure(plan.target) || isDeadStructure(plan.source) || isDeadStructure(plan.sourcePropertyStorage) || isDeadStructure(plan.extraPropertiesBefore) || isDead(plan.extraPropertiesAfter) || isDead(plan.targetAfter)) {
            plan.forgetCells();
            ++removed;
            return;
        }
        for (auto& store : plan.fieldStores) {
            if (isDeadStructure(store.lastAcceptedStructure))
                store.lastAcceptedStructure = { };
        }
    };
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        for (std::unique_ptr<CopyOfTwoPlan>& plan : m_copiesOfTwo) {
            if (!plan)
                continue;
            bool hasDeadSource = std::ranges::any_of(plan->sources, [&](const CopyOfTwoPlan::Source& source) {
                return isDeadStructure(source.structure) || isDeadStructure(source.propertyStorage);
            });
            if (hasDeadSource || isDead(plan->structure)) {
                plan = nullptr;
                ++removed;
            }
        }
        if (!m_sets)
            return;
        if (scope == CollectionScope::Eden && m_listsEveryWrittenPlan) {
            for (AssignmentPlan* plan : m_writtenPlans) {
                if (plan->target)
                    removeIfDead(*plan);
            }
            return;
        }
        for (Set& set : *m_sets) {
            for (auto& plan : set.ways) {
                if (plan && plan->target)
                    removeIfDead(*plan);
            }
        }
    });
    m_writtenPlans.shrink(0);
    m_listsEveryWrittenPlan = true;
    return removed;
}

namespace {

struct AssignedProperty {
    UniquedStringImpl* name;
    const FieldData* field;
    PropertyOffset offset;
};

}

static bool hasForm(const FieldData* fieldOrNull)
{
    return fieldOrNull && fieldOrNull->flags & fieldAttributeFlags;
}

static std::optional<Vector<AssignedProperty, 16>> listOwnProperties(VM& vm, const AssignmentSource& source)
{
    Vector<AssignedProperty, 16> properties;
    if (const Object* compiledSource = source.compiledObject; compiledSource && source.propertyStorage) {
        const Vector<PropertyListEntry>* listed = compiledSource->fieldsAndExtraPropertiesInKeyOrder(vm);
        if (!listed)
            return std::nullopt;
        for (const PropertyListEntry& property : *listed) {
            if (hasForm(property.field) || (property.field && isPreparedBeforeLookups(vm, property.name)))
                return std::nullopt;
            properties.append({ property.name, property.field, property.offsetInExtraProperties });
        }
        return properties;
    }
    if (const Object* compiledSource = source.compiledObject) {
        ObjectModel& objects = *compiledSource->state().objects;
        const LayoutData& layout = compiledSource->layout();
        auto entries = objects.keyOrders.entriesInOrder(layout, orderOf(source.orderWord)).first(countOf(source.orderWord));
        properties.reserveInitialCapacity(entries.size());
        for (uint16_t index : entries) {
            if (index >= layout.numberOfFields)
                return std::nullopt;
            const FieldData& field = layout.fields()[index];
            if (field.attributes() || hasForm(&field))
                return std::nullopt;
            if (objects.layouts.hasSymbolKey(field) && isPreparedBeforeLookups(vm, objects.layouts.nameOf(vm, field)))
                return std::nullopt;
            if (objects.layouts.hasStringKey(field) || objects.layouts.hasSymbolKey(field))
                properties.append({ objects.layouts.nameOf(vm, field), &field, invalidOffset });
        }
        return properties;
    }
    Structure* sourceStructure = source.object->structure();
    if (!flattenOnce(vm, source.object) || source.object->hasNonReifiedStaticProperties() || !sourceStructure->canPerformFastPropertyEnumerationCommon() || hasIndexedProperties(sourceStructure->indexingType()))
        return std::nullopt;
    bool hasOnlyDataProperties = true;
    sourceStructure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (entry.attributes() & PropertyAttribute::DontEnum || PropertyName(entry.key()).isPrivateName())
            return true;
        hasOnlyDataProperties = !(entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue);
        properties.append({ entry.key(), nullptr, entry.offset() });
        return hasOnlyDataProperties;
    });
    if (!hasOnlyDataProperties)
        return std::nullopt;
    return properties;
}

static std::optional<Vector<AssignedProperty, 16>> listAssignedProperties(VM& vm, const AssignmentSource& source, const ExcludedNameList* excludedListOrNull)
{
    auto properties = listOwnProperties(vm, source);
    if (properties && !source.excludedNames.empty()) {
        properties->removeAllMatching([&](const AssignedProperty& property) {
            return excludedListOrNull ? excludedListOrNull->contains(property.name) : std::ranges::contains(source.excludedNames, property.name);
        });
    }
    return properties;
}

static Structure* structureWithAddedNames(VM& vm, Structure* structure, std::span<UniquedStringImpl* const> names, bool addsAllAtOnce)
{
    PropertyOffset offset = invalidOffset;
    Structure* existing = structure;
    for (UniquedStringImpl* name : names) {
        existing = Structure::addPropertyTransitionToExistingStructure(existing, PropertyName(name), 0, offset);
        if (!existing)
            break;
    }
    if (existing)
        return existing;
    if (addsAllAtOnce && names.size() > 1) {
        if (Structure* created = Structure::createWithProperties(vm, structure, names, 0))
            return created->isDictionary() ? nullptr : created;
    }
    for (UniquedStringImpl* name : names) {
        structure = Structure::addPropertyTransition(vm, structure, PropertyName(name), 0, offset);
        if (structure->isDictionary())
            return nullptr;
    }
    return structure;
}

void AssignmentPlanCache::growMappedCopies()
{
    Vector<std::unique_ptr<MappedCopy>> before = std::exchange(m_mappedCopies, { });
    zeroSpan(m_state.instance.copiesWithoutFrame());
    size_t size = before.isEmpty() ? minNumberOfPlacesForMappedCopies : before.size() * 2;
    m_mappedCopies.grow(size);
    m_shiftForMappedCopies = 64 - WTF::fastLog2(static_cast<unsigned>(size));
    m_numberOfMappedCopiesTaken = 0;
    for (std::unique_ptr<MappedCopy>& copy : before) {
        if (!copy || copy->state == MappedCopy::State::IsUnknown)
            continue;
        uint64_t hash = hashOfMappedCopy(copy->targetWords, copy->sourceWords, copy->excludedRunNumberPlusOne);
        for (unsigned probe = 0; probe < maxProbesForMappedCopy; ++probe) {
            std::unique_ptr<MappedCopy>& place = placeForMappedCopy(hash, probe);
            if (place)
                continue;
            place = WTF::move(copy);
            ++m_numberOfMappedCopiesTaken;
            break;
        }
    }
}

NEVER_INLINE MappedCopy& AssignmentPlanCache::placeForNewMappedCopy(uint64_t hash)
{
    if (m_numberOfMappedCopiesTaken * 2 >= m_mappedCopies.size() && m_mappedCopies.size() < maxNumberOfPlacesForMappedCopies)
        growMappedCopies();
    for (unsigned probe = 0; probe < maxProbesForMappedCopy; ++probe) {
        std::unique_ptr<MappedCopy>& place = placeForMappedCopy(hash, probe);
        if (!place) {
            place = makeUniqueWithoutFastMallocCheck<MappedCopy>();
            ++m_numberOfMappedCopiesTaken;
            return *place;
        }
        if (place->state == MappedCopy::State::IsUnknown)
            return *place;
    }
    return *placeForMappedCopy(hash, m_nextMappedCopyReplaced++ % maxProbesForMappedCopy);
}

FieldMap* AssignmentPlanCache::fieldMapFor(const LayoutData& target, const LayoutData& source, uint32_t excludedRunNumberPlusOne)
{
    if (excludedRunNumberPlusOne && !m_excludedListsByRun.contains(excludedRunNumberPlusOne))
        return nullptr;
    uint64_t key = (static_cast<uint64_t>(target.index) + 1) << 32 | source.index;
    auto& maps = m_fieldMaps.add(key, Vector<std::unique_ptr<FieldMap>, 1>()).iterator->value;
    for (auto& map : maps) {
        if (map->excludedRunNumberPlusOne == excludedRunNumberPlusOne)
            return map.get();
    }
    auto map = makeUnique<FieldMap>();
    map->excludedRunNumberPlusOne = excludedRunNumberPlusOne;
    map->entries.fill(FieldMap::Entry { }, source.numberOfFields);
    maps.append(WTF::move(map));
    return maps.last().get();
}

FieldMap::Entry AssignmentPlanCache::describeMappedField(VM& vm, const LayoutData& target, const FieldData& from, uint32_t excludedRunNumberPlusOne)
{
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    if (from.attributes() || hasForm(&from))
        return { FieldMap::isNotServed };
    if (!layouts.hasStringKey(from) && !layouts.hasSymbolKey(from))
        return { FieldMap::isLeftOut };
    UniquedStringImpl* name = layouts.nameOf(vm, from);
    if (isPreparedBeforeLookups(vm, name))
        return { FieldMap::isNotServed };
    if (excludedRunNumberPlusOne && m_excludedListsByRun.get(excludedRunNumberPlusOne)->contains(name))
        return { FieldMap::isLeftOut };
    const FieldData* to = layouts.field(vm, target, name);
    if (!to || to->flags & (fieldIsStoredOnce | fieldAttributeFlags | fieldMayExistWithoutStorage))
        return { FieldMap::isNotServed };
    if (to->holdsRawValueFromBirth)
        return { FieldMap::isNotServed };
    SlotRepresentation representation = to->slot.representation();
    if (from.slot.representation() == SlotRepresentation::DeferredClosure || representation == SlotRepresentation::DeferredClosure)
        return { FieldMap::isNotServed };
    // A granule starts as zeros, which have to say that the fields in it are absent. The copy fills a granule that holds one field.
    const SlotTraits traits = traitsOf(representation);
    if (to->isOutOfLine() && traits.absentBits && traits.width != extensionGranuleSize)
        return { FieldMap::isNotServed };
    static_assert(FieldMap::firstField + maxFieldsInOrder < FieldMap::testsUndefinedAndNull);
    SlotFit fit = fitOf(layouts, from, *to);
    uint16_t state = FieldMap::firstField + target.indexOf(*to);
    if (fit == SlotFit::UnlessUndefinedOrNull)
        state |= FieldMap::testsUndefinedAndNull;
    uint8_t acceptedKinds = fit == SlotFit::IsTested ? kindsAlwaysAccepted(layouts.typeOf(*to)).toRaw() : 0;
    return { state, moveKindFor(from.slot.representation(), representation, fit), acceptedKinds };
}

void AssignmentPlanCache::fillPlanForCopyOfTwo(VM& vm, CopyOfTwoPlan& plan, JSGlobalObject* realm, const AssignmentSource* first, const AssignmentSource* second, bool isOrdinaryCopy)
{
    struct CopiedProperty {
        AssignedProperty property;
        unsigned source;
    };
    Vector<CopiedProperty, 32> copied;
    Vector<UniquedStringImpl*, 32> names;
    std::array<const AssignmentSource*, 2> sources { first, second };
    for (unsigned index = 0; index < sources.size(); ++index) {
        if (!sources[index])
            continue;
        auto properties = listAssignedProperties(vm, *sources[index], nullptr);
        if (!properties)
            return notePlanWithoutUse("a source of two has no list of its properties"_s);
        for (const AssignedProperty& property : *properties) {
            if (parseIndex(PropertyName(property.name)) || property.name == vm.propertyNames->underscoreProto.impl())
                return notePlanWithoutUse("a name is an index or __proto__"_s);
            if (size_t place = names.find(property.name); place != notFound) {
                copied[place] = { property, index };
                continue;
            }
            names.append(property.name);
            copied.append({ property, index });
            // Each name is looked for among those before it.
            if (names.size() > JSFinalObject::maxInlineCapacity)
                return notePlanWithoutUse("the properties of two sources do not fit a cell"_s);
        }
    }
    size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(names.size() + 1));
    unsigned inlineCapacity = static_cast<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue));
    bool fitsCell = isOrdinaryCopy ? names.size() < JSFinalObject::maxInlineCapacity : !names.isEmpty() && inlineCapacity <= JSFinalObject::maxInlineCapacity;
    if (!fitsCell)
        return notePlanWithoutUse("the properties of two sources do not fit a cell"_s);
    Structure* empty = realm->structureCache().emptyObjectStructureForPrototype(realm, realm->objectPrototype(), std::min<unsigned>(inlineCapacity, JSFinalObject::maxInlineCapacity));
    Structure* structure = structureWithAddedNames(vm, empty, names.span(), !isOrdinaryCopy);
    if (!structure)
        return notePlanWithoutUse("the names of two sources make a dictionary"_s);

    for (size_t place = 0; place < copied.size(); ++place) {
        PropertyOffset offset = structure->get(vm, names[place]);
        if (!isInlineOffset(offset))
            return notePlanWithoutUse("the properties of two sources do not fit a cell"_s);
        auto& [property, source] = copied[place];
        if (property.field)
            plan.sources[source].fields.appendField(*property.field, offset);
        else
            plan.sources[source].properties.appendProperty(property.offset, offset);
    }
    for (CopyOfTwoPlan::Source& source : plan.sources) {
        source.fields.structure = structure;
        source.properties.structure = structure;
    }
    plan.structure = structure;
}

std::unique_ptr<FieldMovePlan> AssignmentPlanCache::createPlanForOrdinaryCopy(VM& vm, JSGlobalObject* realm, JSObject* source, const ExcludedRun& excluded)
{
    const Object* compiled = compiledObjectOrNull(source);
    AssignmentSource from { source, compiled, compiled ? nullptr : source, compiled ? compiled->orderWord() & orderAndCountMask : 0 };
    if (excluded.numberPlusOne)
        from.excludedNames = excludedNames(vm, excluded).names();
    CopyOfTwoPlan plan;
    fillPlanForCopyOfTwo(vm, plan, realm, &from, nullptr, true);
    return makeUnique<FieldMovePlan>(WTF::move(compiled ? plan.sources[0].fields : plan.sources[0].properties));
}

CopyOfTwoPlan* AssignmentPlanCache::planForCopyOfTwo(VM& vm, JSGlobalObject* realm, const AssignmentSource* first, const AssignmentSource* second)
{
    auto wordsOf = [](const AssignmentSource* source) -> uint64_t {
        if (!source)
            return 0;
        return (static_cast<uint64_t>(source->object->structureID().bits()) << 32 | source->propertyStorageStructureID().bits()) ^ static_cast<uint64_t>(source->orderWord) * 0x9e3779b97f4a7c15ull;
    };
    uint32_t hash = WTF::intHash(static_cast<uint64_t>(wordsOf(first) ^ std::rotl(wordsOf(second), 29) * 0xc2b2ae3d27d4eb4full));
    auto placeAt = [&](unsigned step) -> std::unique_ptr<CopyOfTwoPlan>& {
        return m_copiesOfTwo[(hash + step) % numberOfCopiesOfTwo];
    };
    for (unsigned step = 0; step < numberOfPlacesForCopyOfTwo; ++step) {
        CopyOfTwoPlan* plan = placeAt(step).get();
        if (plan && plan->isFor(first, second)) [[likely]] {
            plan->visitsInVainLeft = CopyOfTwoPlan::maxVisitsInVain;
            return plan;
        }
    }
    if (!isSecondCopy(hash))
        return nullptr;

    std::unique_ptr<CopyOfTwoPlan>* place = nullptr;
    for (unsigned step = 0; step < numberOfPlacesForCopyOfTwo && !place; ++step) {
        if (!placeAt(step))
            place = &placeAt(step);
    }
    for (unsigned step = 0; step < numberOfPlacesForCopyOfTwo && !place; ++step) {
        if (!placeAt(step)->visitsInVainLeft)
            place = &placeAt(step);
        else
            --placeAt(step)->visitsInVainLeft;
    }
    if (!place)
        return nullptr;

    *place = makeUnique<CopyOfTwoPlan>();
    CopyOfTwoPlan& plan = **place;
    std::array<const AssignmentSource*, 2> sources { first, second };
    for (unsigned index = 0; index < sources.size(); ++index) {
        if (!sources[index])
            continue;
        plan.sources[index].structure = sources[index]->object->structureID();
        plan.sources[index].propertyStorage = sources[index]->propertyStorageStructureID();
        plan.sources[index].orderWord = sources[index]->orderWord;
    }
    fillPlanForCopyOfTwo(vm, plan, realm, first, second, false);
    m_writes.recordAll();
    count(vm, RuntimeCounterID::AssignmentPlansMade);
    return &plan;
}

void AssignmentPlanCache::describeMove(VM& vm, AssignmentPlan::FieldStore& store, const LayoutData& target, bool sharesTypes)
{
    using MovesFrom = AssignmentPlan::FieldStore::MovesFrom;
    const FieldData& to = *store.to;
    SlotRepresentation representation = to.slot.representation();
    MappedCopy::Move move { 0, 0, MappedCopy::MoveKind::OtherForm, static_cast<uint8_t>(widthOf(representation)), false, to.isOutOfLine(), SlotRepresentation::JSValue, representation, store.acceptedKinds.toRaw(), target.indexOf(to) };
    move.leastInt32 = store.leastInt32;
    move.greatestInt32 = store.greatestInt32;
    if (!store.from) {
        std::optional<MappedCopy::MoveKind> kind = moveKindOutOfOrdinaryObject(m_state.objects->layouts, store);
        if (!kind)
            return;
        move.kind = *kind;
        bool isInline = isInlineOffset(store.sourceOffset);
        store.movesFrom = isInline ? MovesFrom::InlineProperties : MovesFrom::OutsideProperties;
        store.bytesInSource = static_cast<int32_t>((isInline ? store.sourceOffset : offsetInOutOfLineStorage(store.sourceOffset)) * sizeof(EncodedJSValue));
        store.move = move;
        return;
    }
    // Where a field lies in an Extension goes by the granules that the Extension has, which no plan knows.
    if (!sharesTypes || store.from->isOutOfLine() || !store.isDirect)
        return;
    FieldMap::Entry entry = describeMappedField(vm, target, *store.from, 0);
    if (entry.state < FieldMap::firstField || entry.kind == MappedCopy::MoveKind::OtherForm || entry.kind == MappedCopy::MoveKind::Tested)
        return;
    ASSERT(entry.fieldOfTarget() == target.indexOf(to));
    move.kind = entry.kind;
    move.representationOfSource = store.from->slot.representation();
    move.acceptedKinds = entry.acceptedKinds;
    move.testsUndefinedAndNull = !!(entry.state & FieldMap::testsUndefinedAndNull);
    store.movesFrom = MovesFrom::Cell;
    store.bytesInSource = static_cast<int32_t>(store.from->slot.offset);
    store.move = move;
}

void AssignmentPlanCache::fillPlan(VM& vm, AssignmentPlan& plan, unsigned index, JSObject* target, JSObject* extraProperties, const AssignmentSource& source)
{
    Object* record = compiledObjectOrNull(target);
    ASSERT(record || (source.defines && !extraProperties));
    if (!record)
        extraProperties = target;
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    auto properties = listAssignedProperties(vm, source, plan.excludedList);
    if (!properties)
        return notePlanWithoutUse("the source has no list of its properties"_s);
    bool sharesTypes = source.compiledObject && &source.compiledObject->state() == &m_state;
    const LayoutData* layout = record ? &record->layout() : nullptr;
    JSGlobalObject* realm = target->structure()->realm();
    Structure* extraPropertiesBefore = extraProperties ? extraProperties->structure() : Instance::of(realm)->realm.emptyExtraPropertiesStructure;
    if (extraProperties && (extraProperties->type() != FinalObjectType || extraPropertiesBefore->isDictionary()))
        return notePlanWithoutUse("the extra properties of the target are a dictionary or no plain object"_s);
    Vector<AssignmentPlan::FieldStore> fieldStores;
    Vector<AssignmentPlan::ExtraPropertyStore> extraPropertyStores;
    Vector<UniquedStringImpl*, 8> addedNames;
    Vector<size_t, 8> storesOfAddedNames;
    Vector<UniquedStringImpl*, 8> creatableFieldNames;
    Vector<uint16_t> storesInSourceOrder;
    if (properties->size() >= AssignmentPlan::isExtraPropertyStore)
        return notePlanWithoutUse("there are too many stores to number"_s);
    if (record) {
        fieldStores.reserveInitialCapacity(properties->size());
        storesInSourceOrder.reserveInitialCapacity(properties->size());
    }
    bool hasStoredSymbol = false;
    for (auto& property : *properties) {
        plan.storesNameBehindSymbol |= hasStoredSymbol && !property.name->isSymbol();
        hasStoredSymbol |= property.name->isSymbol();
        const FieldData* field = record ? layouts.field(vm, *layout, property.name) : nullptr;
        if (!field) {
            if (record && !layout->extraPropertiesSlot())
                return notePlanWithoutUse("the layout of the target has no word for extra properties"_s);
            unsigned attributes = 0;
            PropertyOffset offset = extraPropertiesBefore->get(vm, property.name, attributes);
            if (isValidOffset(offset)) {
                if (attributes)
                    return notePlanWithoutUse(record ? "an extra property of the target has attributes"_s : "a property of the ordinary target has attributes"_s);
            } else {
                if (parseIndex(PropertyName(property.name)) || property.name == vm.propertyNames->underscoreProto.impl())
                    return notePlanWithoutUse("a name is an index or __proto__"_s);
                ASSERT(!addedNames.contains(property.name));
                addedNames.append(property.name);
                storesOfAddedNames.append(extraPropertyStores.size());
            }
            if (record)
                storesInSourceOrder.append(static_cast<uint16_t>(extraPropertyStores.size() | AssignmentPlan::isExtraPropertyStore));
            extraPropertyStores.append(AssignmentPlan::ExtraPropertyStore { .from = property.field, .sourceOffset = property.offset, .offset = offset, .name = property.name, .addsName = !isValidOffset(offset) });
            continue;
        }
        if (field->flags & (fieldIsStoredOnce | fieldAttributeFlags))
            return notePlanWithoutUse("a field of the target is stored once or has a form"_s);
        if (isPreparedBeforeLookups(vm, property.name))
            return notePlanWithoutUse("the target is asked to prepare itself before a store to a field"_s);
        if (extraProperties && isValidOffset(extraPropertiesBefore->get(vm, property.name)))
            return notePlanWithoutUse("a field of the target lies among its extra properties"_s);
        const SlotTraits traits = traitsOf(field->slot.representation());
        SlotFit fit = sharesTypes && property.field ? fitOf(layouts, *property.field, *field) : SlotFit::IsTested;
        storesInSourceOrder.append(static_cast<uint16_t>(fieldStores.size()));
        fieldStores.append(AssignmentPlan::FieldStore {
            .from = property.field,
            .to = field,
            .sourceOffset = property.offset,
            .acceptedKinds = kindsAlwaysAccepted(layouts.typeOf(*field)),
            .sourceAlwaysFits = fit == SlotFit::Always,
            .sourceFitsUnlessUndefinedOrNull = fit == SlotFit::UnlessUndefinedOrNull,
            .isDirect = !(field->isOutOfLine() && traits.absentBits && traits.width != extensionGranuleSize),
            .copiesBits = property.field && property.field->slot.representation() == field->slot.representation(),
            .holdsCell = traits.holdsCell,
            .hasAbsent = traits.hasAbsent,
            .leavesUndefinedAndNullToOtherWay = !!field->holdsRawValueFromBirth,
            .width = traits.width,
            .absentBits = traits.absentBits,
        });
        AssignmentPlan::FieldStore& store = fieldStores.last();
        if (const RuntimeType& type = layouts.typeOf(*field); type.primitiveSet().contains(PrimitiveSet::Integer)) {
            auto range = type.integerRange();
            int64_t least = range ? std::max<int64_t>(range->first, INT32_MIN) : INT32_MIN;
            int64_t greatest = range ? std::min<int64_t>(range->second, INT32_MAX) : INT32_MAX;
            if (least <= greatest) {
                store.leastInt32 = static_cast<int32_t>(least);
                store.greatestInt32 = static_cast<int32_t>(greatest);
                store.writesInt32AsItIs = holdsIntegersBetween(field->slot.representation(), least, greatest);
            }
        }
        store.copiesBitsUntested = store.sourceAlwaysFits && store.copiesBits && store.isDirect && !(field->flags & fieldMayExistWithoutStorage) && field->slot.representation() != SlotRepresentation::DeferredClosure
            && !(store.leavesUndefinedAndNullToOtherWay && property.field->slotHoldsUndefinedAndNull);
        if (!store.copiesBitsUntested)
            describeMove(vm, store, *layout, sharesTypes);
        plan.mayStoreCells |= traits.holdsCell;
        plan.readsOutOfLineFields |= property.field && property.field->isOutOfLine();
        if (field->isOutOfLine())
            plan.granuleBitsFilled |= granuleBitOf(field->slot.offset);
        if (layouts.mayBeAbsent(*field))
            creatableFieldNames.append(property.name);
    }
    auto isIntercepted = [&](UniquedStringImpl* name) {
        return !source.defines && !storeCreatesOwnProperty(realm, target->structure(), target, name, ChainDependent { name, index, m_chainDependentOwner });
    };
    if (std::ranges::any_of(addedNames.span(), isIntercepted) || std::ranges::any_of(creatableFieldNames.span(), isIntercepted))
        return notePlanWithoutUse("a prototype intercepts a name that a store may create"_s);
    if (!record || !extraPropertyStores.isEmpty()) {
        Structure* extraPropertiesAfter = structureWithAddedNames(vm, extraPropertiesBefore, addedNames.span(), true);
        if (!extraPropertiesAfter)
            return notePlanWithoutUse(record ? "the added names make the extra properties a dictionary"_s : "the added names make the ordinary target a dictionary"_s);
        for (size_t i = 0; i < addedNames.size(); ++i)
            extraPropertyStores[storesOfAddedNames[i]].offset = extraPropertiesAfter->get(vm, addedNames[i]);
        plan.extraPropertiesAfter = extraPropertiesAfter;
        for (auto& store : extraPropertyStores) {
            auto bytesOf = [](PropertyOffset offset) {
                return static_cast<int32_t>((isInlineOffset(offset) ? offset : offsetInOutOfLineStorage(offset)) * sizeof(EncodedJSValue));
            };
            store.isOutsideInSource = isValidOffset(store.sourceOffset) && !isInlineOffset(store.sourceOffset);
            store.isOutsideInTarget = !isInlineOffset(store.offset);
            store.bytesInSource = isValidOffset(store.sourceOffset) ? bytesOf(store.sourceOffset) : 0;
            store.bytesInTarget = bytesOf(store.offset);
        }
        if (record)
            plan.targetAfter = m_state.objects->structures.withExtraProperties(vm, record->compiledStructure(), extraPropertiesAfter);
    }
    // A run adds its names by the Structure, so what a store by name reports for each object is reported here for all of them. A guard that a name has cleared stays cleared.
    for (UniquedStringImpl* name : record ? addedNames.span() : std::span<UniquedStringImpl* const>()) {
        layouts.willAddProperty(vm, *layout, name);
        m_state.objects->classes.didShadowMember(*layout, name);
    }
    plan.addsExtraProperties = !addedNames.isEmpty();
    plan.storesOnlyFieldsThatFit = !creatableFieldNames.isEmpty() && extraPropertyStores.isEmpty() && !extraProperties && std::ranges::all_of(fieldStores, [](const AssignmentPlan::FieldStore& store) {
        return store.sourceAlwaysFits && store.isDirect && store.from->slot.representation() != SlotRepresentation::DeferredClosure;
    });
    plan.fieldStores = WTF::move(fieldStores);
    plan.extraPropertyStores = WTF::move(extraPropertyStores);
    plan.storesInSourceOrder = WTF::move(storesInSourceOrder);
    plan.isUsable = true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
