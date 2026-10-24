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
#include "AOTRuntimeCounters.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <tuple>
#include <wtf/PrintStream.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>

#if OS(LINUX)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

struct AssignmentPlanCache::CopyCounts {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CopyCounts);

    struct Count {
        uint32_t copies { 0 };
        uint64_t names { 0 };
        uint64_t instructions { 0 };
    };

    UncheckedKeyHashMap<String, Count> byWay;
    UncheckedKeyHashMap<String, Count> byWayAndObjects;
    UncheckedKeyHashMap<String, Count> byStructuresAndOrderWords;
    UncheckedKeyHashMap<String, Count> byLayouts;
    UncheckedKeyHashMap<String, Count> plansWithoutUse;
    // By the first Structure that a collection found dead, in the order of namesOfDeadStructures. The collector may count in a thread of its own.
    std::array<std::atomic<uint32_t>, 6> plansDroppedByCollections { };
    std::atomic<uint32_t> collectionsThatLookedAtPlans { 0 };
};

static constexpr std::array<ASCIILiteral, 6> namesOfDeadStructures { "of the target"_s, "of the source"_s, "of the source's properties"_s, "of the target's extra properties before"_s, "of the target's extra properties after"_s, "of the target after"_s };

AssignmentPlanCache::AssignmentPlanCache(State& state)
    : m_state(state)
    , m_chainDependentOwner(state.chainDependents->addOwner(*this))
{
    state.caches->add(CacheKind::WeakByKey, "assignment plans"_s, *this);
    if (Options::aotCountsPath())
        m_copyCounts = makeUnique<CopyCounts>();
}

AssignmentPlanCache::~AssignmentPlanCache() = default;

static unsigned numberOfPropertiesOf(JSObject* ordinaryObjectOrNull)
{
    return ordinaryObjectOrNull ? ordinaryObjectOrNull->structure()->inlineSize() + ordinaryObjectOrNull->structure()->outOfLineSize() : 0;
}

static void describeForCopyCounts(StringBuilder& out, JSObject* object, uint64_t& names)
{
    if (Object* compiled = compiledObjectOrNull(object)) {
        // The order word of an object whose keys nobody lists says nothing.
        bool hasNumberedOrder = !compiled->layout().keysAreNeverEnumerated && !compiled->hasOutOfLineKeyOrder() && !isOrderStoredOutOfLine(compiled->orderWord());
        unsigned fields = hasNumberedOrder ? countOf(compiled->orderWord()) : 0;
        unsigned extra = numberOfPropertiesOf(compiled->extraProperties());
        names = fields + extra;
        out.append('L', compiled->layout().index, " with "_s, fields, " of "_s, compiled->layout().numberOfFields, " fields"_s);
        if (compiled->hasExtraProperties())
            out.append(" + "_s, extra, " extra"_s);
        return;
    }
    if (object->type() == FinalObjectType) {
        names = numberOfPropertiesOf(object);
        out.append("an ordinary object with "_s, names);
        return;
    }
    names = 0;
    out.append(object->classInfo()->className);
}

// What a way costs is not how many copies take it.
static uint64_t userInstructionsOfThisThread()
{
#if OS(LINUX)
    static thread_local int counter = -2;
    if (counter == -2) {
        perf_event_attr attributes { };
        attributes.type = PERF_TYPE_HARDWARE;
        attributes.size = sizeof(attributes);
        attributes.config = PERF_COUNT_HW_INSTRUCTIONS;
        attributes.exclude_kernel = 1;
        attributes.exclude_hv = 1;
        counter = static_cast<int>(syscall(SYS_perf_event_open, &attributes, 0, -1, -1, 0));
    }
    uint64_t instructions = 0;
    if (counter >= 0 && read(counter, &instructions, sizeof(instructions)) == sizeof(instructions))
        return instructions;
#endif
    return 0;
}

CopyDescription AssignmentPlanCache::describeCopy(JSObject* target, JSValue source) const
{
    uint64_t namesInTarget = 0;
    uint64_t names = 0;
    StringBuilder objects;
    objects.append(" | into "_s);
    describeForCopyCounts(objects, target, namesInTarget);
    objects.append(" | from "_s);
    if (source.isObject())
        describeForCopyCounts(objects, asObject(source), names);
    else
        objects.append("a primitive"_s);
    CopyDescription description { objects.toString(), names, { }, { } };
    Object* to = compiledObjectOrNull(target);
    Object* from = compiledObjectOrNull(source);
    if (to && from && to->shapeWord() != from->shapeWord()) {
        JSObject* extraOfTarget = to->extraProperties();
        JSObject* extraOfSource = from->extraProperties();
        description.structuresAndOrderWords = makeString(to->structureID().bits(), ' ', extraOfTarget ? extraOfTarget->structureID().bits() : 0, ' ', from->structureID().bits(), ' ', extraOfSource ? extraOfSource->structureID().bits() : 0, ' ', from->orderWord());
        description.layouts = makeString(to->layout().index, ' ', from->layout().index);
    }
    description.instructionsBefore = userInstructionsOfThisThread();
    return description;
}

void AssignmentPlanCache::notePlanWithoutUse(ASCIILiteral reason)
{
    if (m_copyCounts)
        ++m_copyCounts->plansWithoutUse.add(String(reason), CopyCounts::Count { }).iterator->value.copies;
}

void AssignmentPlanCache::noteCopy(ASCIILiteral way, CopyDescription&& description)
{
    uint64_t instructions = userInstructionsOfThisThread() - description.instructionsBefore;
    auto add = [&](CopyCounts::Count& count) {
        ++count.copies;
        count.names += description.names;
        count.instructions += instructions;
    };
    add(m_copyCounts->byWay.add(String(way), CopyCounts::Count { }).iterator->value);
    add(m_copyCounts->byWayAndObjects.add(makeString(way, description.objects), CopyCounts::Count { }).iterator->value);
    if (!description.layouts.isNull()) {
        add(m_copyCounts->byStructuresAndOrderWords.add(description.structuresAndOrderWords, CopyCounts::Count { }).iterator->value);
        add(m_copyCounts->byLayouts.add(description.layouts, CopyCounts::Count { }).iterator->value);
    }
}

void AssignmentPlanCache::dumpCopies(PrintStream& out) const
{
    if (!m_copyCounts)
        return;
    auto dump = [&](const UncheckedKeyHashMap<String, CopyCounts::Count>& counts, ASCIILiteral prefix, size_t limit) {
        Vector<std::tuple<uint32_t, String, uint64_t, uint64_t>> lines;
        for (auto& [kind, count] : counts)
            lines.append({ count.copies, kind, count.names, count.instructions });
        AOT::sort(lines, [](const auto& a, const auto& b) {
            return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) > std::get<0>(b) : codePointCompareLessThan(std::get<1>(a), std::get<1>(b));
        });
        for (auto& [copies, kind, names, instructions] : lines.span().first(std::min(lines.size(), limit)))
            out.println(copies, "\t"_s, prefix, kind, " | names in all "_s, names, " | instructions in all "_s, instructions);
    };
    dump(m_copyCounts->byWay, "copies:"_s, std::numeric_limits<size_t>::max());
    dump(m_copyCounts->byWayAndObjects, "copies of one kind:"_s, 120);
    dump(m_copyCounts->plansWithoutUse, "plans made without use:"_s, std::numeric_limits<size_t>::max());
    for (size_t index = 0; index < namesOfDeadStructures.size(); ++index)
        out.println(m_copyCounts->plansDroppedByCollections[index].load(), "\tplans dropped by collections:the Structure "_s, namesOfDeadStructures[index], " was dead"_s);
    out.println(m_copyCounts->collectionsThatLookedAtPlans.load(), "\tplans dropped by collections:collections that looked"_s);
    // A few dear copies cost more than many cheap ones.
    Vector<std::tuple<uint64_t, String, uint32_t, uint64_t>> kindsByInstructions;
    for (auto& [kind, count] : m_copyCounts->byWayAndObjects)
        kindsByInstructions.append({ count.instructions, kind, count.copies, count.names });
    AOT::sort(kindsByInstructions, [](const auto& a, const auto& b) {
        return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) > std::get<0>(b) : codePointCompareLessThan(std::get<1>(a), std::get<1>(b));
    });
    for (auto& [instructions, kind, copies, names] : kindsByInstructions.span().first(std::min<size_t>(kindsByInstructions.size(), 100)))
        out.println(instructions, "\tinstructions of one kind:"_s, kind, " | copies "_s, copies, " | names in all "_s, names);
    // Between compiled objects of two layouts: how many keys were copied how often.
    auto dumpHistogram = [&](const UncheckedKeyHashMap<String, CopyCounts::Count>& counts, ASCIILiteral prefix) {
        constexpr std::array<uint32_t, 8> upTo { 1, 2, 3, 4, 8, 16, 64, UINT32_MAX };
        std::array<CopyCounts::Count, upTo.size()> classes { };
        for (auto& count : counts.values()) {
            size_t index = std::ranges::find_if(upTo, [&](uint32_t limit) { return count.copies <= limit; }) - upTo.begin();
            ++classes[index].names;
            classes[index].copies += count.copies;
        }
        for (size_t index = 0; index < upTo.size(); ++index)
            out.println(classes[index].names, "\t"_s, prefix, "copied up to "_s, upTo[index], " times | copies in all "_s, classes[index].copies);
    };
    dumpHistogram(m_copyCounts->byStructuresAndOrderWords, "keys of plans:"_s);
    dumpHistogram(m_copyCounts->byLayouts, "pairs of layouts:"_s);
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
    // Where half of the bits are set, every other copy between shapes that never meet again would get a plan.
    if (++m_numberOfCopiesWithoutPlan > numberOfCopiesRemembered / 2) {
        m_copiesWithoutPlan->clearAll();
        m_numberOfCopiesWithoutPlan = 0;
    }
    return false;
}

AssignmentPlanCache::ClaimedPlan AssignmentPlanCache::claimPlan(VM& vm, StructureID target, JSObject* extraProperties, const AssignmentSource& source, uint32_t hash, Object::PlanCreation creation)
{
    if (creation == Object::PlanCreation::AtSecondCopy && !isSecondCopy(hash))
        return { };
    if (!m_sets)
        m_sets = makeUniqueWithoutFastMallocCheck<Sets>();
    unsigned setIndex = hash % numberOfSets;
    Set& set = (*m_sets)[setIndex];
    auto way = std::ranges::find_if(set.ways, [](auto& candidate) {
        return !candidate || !candidate->target;
    });
    if (way == set.ways.end())
        way = set.ways.begin() + set.nextVictim++ % waysPerSet;
    if (!*way)
        *way = makeUnique<AssignmentPlan>();
    unsigned index = setIndex * waysPerSet + static_cast<unsigned>(way - set.ways.begin());
    AssignmentPlan& plan = **way;
    plan = { };
    plan.target = target;
    plan.source = source.object->structureID();
    plan.sourcePropertyStorage = source.propertyStorageStructureID();
    plan.extraPropertiesBefore = extraProperties ? extraProperties->structureID() : StructureID();
    plan.sourceOrderWord = source.orderWord;
    plan.servesDefinitionsOnly = source.defines;
    plan.excludedList = excludedListAt(source.excludedNames);
    if (!plan.excludedList)
        plan.excludedNames.appendRange(source.excludedNames.begin(), source.excludedNames.end());
    didWritePlan(plan);
    count(vm, RuntimeCounterID::AssignmentPlansMade);
    return { &plan, index };
}

NEVER_INLINE AssignmentPlan* AssignmentPlanCache::makePlan(VM& vm, Object& target, JSObject* extraProperties, const AssignmentSource& source, uint32_t hash, Object::PlanCreation creation)
{
    ClaimedPlan claimed = claimPlan(vm, target.structureID(), extraProperties, source, hash, creation);
    if (claimed.plan) {
        fillPlan(vm, *claimed.plan, claimed.index, target, extraProperties, source);
        didWritePlan(*claimed.plan);
    }
    return claimed.plan;
}

AssignmentPlan* AssignmentPlanCache::planForOrdinaryTarget(VM& vm, JSObject* target, const AssignmentSource& source, Object::PlanCreation creation)
{
    ASSERT(source.defines && !target->structure()->isCompiledStructure());
    StructureID targetID = target->structureID();
    uint32_t hash = hashFor(targetID, { }, source);
    if (AssignmentPlan* plan = find(targetID, { }, source, hash)) [[likely]]
        return plan;
    ClaimedPlan claimed = claimPlan(vm, targetID, nullptr, source, hash, creation);
    if (claimed.plan) {
        fillPlanForOrdinaryTarget(vm, *claimed.plan, target, source);
        didWritePlan(*claimed.plan);
    }
    return claimed.plan;
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
        std::array<bool, 6> areDead { isDeadStructure(plan.target), isDeadStructure(plan.source), isDeadStructure(plan.sourcePropertyStorage), isDeadStructure(plan.extraPropertiesBefore), isDead(plan.extraPropertiesAfter), isDead(plan.targetAfter) };
        auto firstDead = std::ranges::find(areDead, true);
        if (firstDead != areDead.end()) {
            if (m_copyCounts) [[unlikely]]
                ++m_copyCounts->plansDroppedByCollections[firstDead - areDead.begin()];
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
        if (m_copyCounts) [[unlikely]]
            ++m_copyCounts->collectionsThatLookedAtPlans;
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

// A field whose layout gives it a form is not copied like a plain property: one that is not enumerable is left out, and a getter is called.
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
            if (hasForm(property.field))
                return std::nullopt;
            properties.append({ property.name, property.field, property.offsetInExtraProperties });
        }
        return properties;
    }
    if (const Object* compiledSource = source.compiledObject) {
        // As KeyOrderTable::propertiesInOrder() lists them. That list is kept for ever, with the text of its keys, and many an order is copied once.
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
            if (objects.layouts.hasStringKey(field) || objects.layouts.hasSymbolKey(field))
                properties.append({ objects.layouts.nameOf(vm, field), &field, invalidOffset });
        }
        return properties;
    }
    Structure* sourceStructure = source.object->structure();
    if (sourceStructure->isDictionary() || source.object->hasNonReifiedStaticProperties() || !sourceStructure->canPerformFastPropertyEnumerationCommon())
        return std::nullopt;
    sourceStructure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (!(entry.attributes() & PropertyAttribute::DontEnum) && !PropertyName(entry.key()).isPrivateName())
            properties.append({ entry.key(), nullptr, entry.offset() });
        return true;
    });
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

// Where stores by name have made the Structures that lead from name to name, the last of them is the one that other objects with these names have.
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
    size_t size = before.isEmpty() ? minNumberOfPlacesForMappedCopies : before.size() * 2;
    m_mappedCopies.grow(size);
    m_shiftForMappedCopies = 64 - WTF::fastLog2(static_cast<unsigned>(size));
    m_numberOfMappedCopiesTaken = 0;
    for (std::unique_ptr<MappedCopy>& copy : before) {
        if (!copy || copy->state == MappedCopy::State::IsUnknown)
            continue;
        uint64_t hash = hashOfMappedCopy(copy->targetWords, copy->sourceWords, copy->excludedRunNumberPlusOne);
        // One that finds no place is described again when it comes again.
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
        // Whoever asked for it has left it as it was.
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
    map->entries.fill(FieldMap::isUnknown, source.numberOfFields);
    maps.append(WTF::move(map));
    return maps.last().get();
}

// What fillPlan() asks of a store that a run does without a test of the value, whatever the target holds.
uint16_t AssignmentPlanCache::describeMappedField(VM& vm, const LayoutData& target, const FieldData& from, uint32_t excludedRunNumberPlusOne)
{
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    if (from.attributes() || hasForm(&from))
        return FieldMap::isNotServed;
    if (!layouts.hasStringKey(from) && !layouts.hasSymbolKey(from))
        return FieldMap::isLeftOut;
    UniquedStringImpl* name = layouts.nameOf(vm, from);
    if (excludedRunNumberPlusOne && m_excludedListsByRun.get(excludedRunNumberPlusOne)->contains(name))
        return FieldMap::isLeftOut;
    const FieldData* to = layouts.field(vm, target, name);
    if (!to || to->flags & (fieldIsStoredOnce | fieldAttributeFlags | fieldMayExistWithoutStorage))
        return FieldMap::isNotServed;
    if (to->holdsRawValueFromBirth || from.holdsRawValueFromBirth)
        return FieldMap::isNotServed;
    SlotRepresentation representation = to->slot.representation();
    if (from.slot.representation() == SlotRepresentation::DeferredClosure || representation == SlotRepresentation::DeferredClosure)
        return FieldMap::isNotServed;
    // A granule starts as zeros, which have to say that the fields in it are absent. The copy fills a granule that holds one field.
    const SlotTraits traits = traitsOf(representation);
    if (to->isOutOfLine() && traits.absentBits && traits.width != extensionGranuleSize)
        return FieldMap::isNotServed;
    uint16_t entry = FieldMap::firstField + target.indexOf(*to);
    static_assert(FieldMap::firstField + maxFieldsInOrder < FieldMap::isTested);
    return alwaysFits(layouts, from, *to) ? entry : entry | FieldMap::isTested;
}

void AssignmentPlanCache::fillPlanForOrdinaryTarget(VM& vm, AssignmentPlan& plan, JSObject* target, const AssignmentSource& source)
{
    auto properties = listAssignedProperties(vm, source, plan.excludedList);
    if (!properties)
        return notePlanWithoutUse("the source has no list of its properties"_s);
    Structure* before = target->structure();
    Vector<AssignmentPlan::ExtraPropertyStore> stores;
    Vector<UniquedStringImpl*, 8> addedNames;
    Vector<size_t, 8> storesOfAddedNames;
    for (auto& property : *properties) {
        unsigned attributes = 0;
        PropertyOffset offset = before->get(vm, property.name, attributes);
        if (isValidOffset(offset)) {
            if (attributes)
                return notePlanWithoutUse("a property of the ordinary target has attributes"_s);
        } else {
            if (parseIndex(PropertyName(property.name)) || property.name == vm.propertyNames->underscoreProto.impl())
                return notePlanWithoutUse("a name is an index or __proto__"_s);
            addedNames.append(property.name);
            storesOfAddedNames.append(stores.size());
        }
        stores.append(AssignmentPlan::ExtraPropertyStore { .from = property.field, .sourceOffset = property.offset, .offset = offset, .name = property.name });
    }
    Structure* after = structureWithAddedNames(vm, before, addedNames.span(), true);
    if (!after)
        return notePlanWithoutUse("the added names make the ordinary target a dictionary"_s);
    for (size_t i = 0; i < addedNames.size(); ++i)
        stores[storesOfAddedNames[i]].offset = after->get(vm, addedNames[i]);
    plan.extraPropertiesAfter = after;
    plan.extraPropertyStores = WTF::move(stores);
    plan.isUsable = true;
}

void AssignmentPlanCache::fillPlanForCopyOfTwo(VM& vm, CopyOfTwoPlan& plan, JSGlobalObject* realm, const AssignmentSource* first, const AssignmentSource* second)
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
            // A name keeps the place that the first source gave it and gets the value of the second.
            if (size_t place = names.find(property.name); place != notFound) {
                copied[place] = { property, index };
                continue;
            }
            names.append(property.name);
            copied.append({ property, index });
        }
    }
    // A literal that begins with copies mostly goes on with a name or two.
    size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(names.size() + 1));
    unsigned inlineCapacity = static_cast<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue));
    if (names.isEmpty() || inlineCapacity > JSFinalObject::maxInlineCapacity)
        return notePlanWithoutUse("the properties of two sources do not fit a cell"_s);
    Structure* structure = structureWithAddedNames(vm, realm->structureCache().emptyObjectStructureForPrototype(realm, realm->objectPrototype(), inlineCapacity), names.span(), true);
    if (!structure)
        return notePlanWithoutUse("the names of two sources make a dictionary"_s);

    const RuntimeLayoutTable& layouts = m_state.objects->layouts;
    for (size_t place = 0; place < copied.size(); ++place) {
        PropertyOffset offset = structure->get(vm, names[place]);
        if (!isInlineOffset(offset))
            return notePlanWithoutUse("the properties of two sources do not fit a cell"_s);
        auto& [property, source] = copied[place];
        if (property.field)
            plan.sources[source].fields.appendFieldToInlineProperty(layouts, *property.field, offset);
        else
            plan.sources[source].properties.appendPropertyToInlineProperty(property.offset, offset);
    }
    for (CopyOfTwoPlan::Source& source : plan.sources) {
        if (source.fields.namesFieldWithAttributes)
            return notePlanWithoutUse("a field of two sources has attributes"_s);
        source.fields.finish(structure, 0, 0);
        source.properties.finish(structure, 0, 0);
    }
    plan.structure = structure;
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
    // Every plan here has been asked for of late: this pair is copied one behind the other.
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
    fillPlanForCopyOfTwo(vm, plan, realm, first, second);
    m_writes.recordAll();
    count(vm, RuntimeCounterID::AssignmentPlansMade);
    return &plan;
}

void AssignmentPlanCache::fillPlan(VM& vm, AssignmentPlan& plan, unsigned index, Object& target, JSObject* extraProperties, const AssignmentSource& source)
{
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    auto properties = listAssignedProperties(vm, source, plan.excludedList);
    if (!properties)
        return notePlanWithoutUse("the source has no list of its properties"_s);
    bool sharesTypes = source.compiledObject && &source.compiledObject->state() == &m_state;
    const LayoutData& layout = target.layout();
    JSGlobalObject* realm = target.structure()->realm();
    Structure* extraPropertiesBefore = extraProperties ? extraProperties->structure() : Instance::of(realm)->realm.emptyExtraPropertiesStructure;
    if (extraProperties && (extraProperties->type() != FinalObjectType || extraPropertiesBefore->isDictionary()))
        return notePlanWithoutUse("the extra properties of the target are a dictionary or no plain object"_s);
    Vector<AssignmentPlan::FieldStore> fieldStores;
    Vector<AssignmentPlan::ExtraPropertyStore> extraPropertyStores;
    Vector<UniquedStringImpl*, 8> addedNames;
    Vector<size_t, 8> storesOfAddedNames;
    Vector<UniquedStringImpl*, 8> creatableFieldNames;
    Vector<uint16_t> storesInSourceOrder;
    bool canNumberStores = properties->size() < AssignmentPlan::isExtraPropertyStore;
    fieldStores.reserveInitialCapacity(properties->size());
    if (canNumberStores)
        storesInSourceOrder.reserveInitialCapacity(properties->size());
    bool mayCreateFieldAfterAddedName = false;
    bool mayCreateFieldBetweenAddedNames = false;
    for (auto& property : *properties) {
        const FieldData* field = layouts.field(vm, layout, property.name);
        if (!field) {
            if (!layout.extraPropertiesSlot())
                return notePlanWithoutUse("the layout of the target has no word for extra properties"_s);
            unsigned attributes = 0;
            PropertyOffset offset = extraPropertiesBefore->get(vm, property.name, attributes);
            if (isValidOffset(offset)) {
                if (attributes)
                    return notePlanWithoutUse("an extra property of the target has attributes"_s);
            } else {
                if (parseIndex(PropertyName(property.name)) || property.name == vm.propertyNames->underscoreProto.impl())
                    return notePlanWithoutUse("a name is an index or __proto__"_s);
                ASSERT(!addedNames.contains(property.name));
                if (addedNames.isEmpty())
                    plan.numberOfFieldStoresBeforeAddedNames = fieldStores.size();
                mayCreateFieldBetweenAddedNames |= mayCreateFieldAfterAddedName;
                addedNames.append(property.name);
                storesOfAddedNames.append(extraPropertyStores.size());
            }
            if (canNumberStores)
                storesInSourceOrder.append(static_cast<uint16_t>(extraPropertyStores.size() | AssignmentPlan::isExtraPropertyStore));
            extraPropertyStores.append(AssignmentPlan::ExtraPropertyStore { .from = property.field, .sourceOffset = property.offset, .offset = offset, .name = property.name, .addsName = !isValidOffset(offset) });
            continue;
        }
        // An assignment to a field with a form throws, calls a setter, or creates a plain property where the field is absent.
        if (field->flags & (fieldIsStoredOnce | fieldAttributeFlags))
            return notePlanWithoutUse("a field of the target is stored once or has a form"_s);
        // A run writes slots by itself, and a marker in such a slot has to set the mark of the cell.
        if (field->holdsRawValueFromBirth)
            return notePlanWithoutUse("the mark of the target's cell answers for a field"_s);
        // A slot for a closure that may not be made yet takes a function object, undefined or null like the slot of a narrow reference, and what it held is gone with the store.
        constexpr bool isDefinedByName = false;
        if (extraProperties && isValidOffset(extraPropertiesBefore->get(vm, property.name)))
            return notePlanWithoutUse("a field of the target lies among its extra properties"_s);
        const SlotTraits traits = traitsOf(field->slot.representation());
        if (canNumberStores)
            storesInSourceOrder.append(static_cast<uint16_t>(fieldStores.size()));
        fieldStores.append(AssignmentPlan::FieldStore {
            .from = property.field,
            .to = field,
            .sourceOffset = property.offset,
            .acceptedKinds = kindsAlwaysAccepted(layouts.typeOf(*field)),
            .sourceAlwaysFits = !isDefinedByName && sharesTypes && property.field && alwaysFits(layouts, *property.field, *field),
            .isDefinedByName = isDefinedByName,
            .isAbsentWhenEmpty = !(field->flags & fieldMayExistWithoutStorage),
            .isDirect = !(field->isOutOfLine() && traits.absentBits),
            .copiesBits = property.field && property.field->slot.representation() == field->slot.representation(),
            .holdsCell = traits.holdsCell,
            .hasAbsent = traits.hasAbsent,
            .width = traits.width,
            .absentBits = traits.absentBits,
        });
        AssignmentPlan::FieldStore& store = fieldStores.last();
        store.copiesBitsUntested = store.sourceAlwaysFits && store.copiesBits && store.isDirect && store.isAbsentWhenEmpty && field->slot.representation() != SlotRepresentation::DeferredClosure;
        plan.mayStoreCells |= traits.holdsCell;
        plan.readsOutOfLineFields |= property.field && property.field->isOutOfLine();
        if (field->isOutOfLine())
            plan.granuleBitsFilled |= granuleBitOf(field->slot.offset);
        if (layouts.mayBeAbsent(*field)) {
            mayCreateFieldAfterAddedName |= !addedNames.isEmpty();
            creatableFieldNames.append(property.name);
        }
    }
    auto isIntercepted = [&](UniquedStringImpl* name) {
        return !source.defines && !storeCreatesOwnProperty(realm, target.structure(), &target, name, ChainDependent { name, index, m_chainDependentOwner });
    };
    if (std::ranges::any_of(addedNames.span(), isIntercepted))
        return notePlanWithoutUse("a prototype intercepts an added name"_s);
    if (!extraPropertyStores.isEmpty()) {
        Structure* extraPropertiesAfter = structureWithAddedNames(vm, extraPropertiesBefore, addedNames.span(), true);
        if (!extraPropertiesAfter)
            return notePlanWithoutUse("the added names make the extra properties a dictionary"_s);
        for (size_t i = 0; i < addedNames.size(); ++i)
            extraPropertyStores[storesOfAddedNames[i]].offset = extraPropertiesAfter->get(vm, addedNames[i]);
        plan.extraPropertiesAfter = extraPropertiesAfter;
        plan.targetAfter = m_state.objects->structures.withExtraProperties(vm, target.compiledStructure(), extraPropertiesAfter);
    }
    // A run adds its names by the Structure, so what a store by name reports for each object is reported here for all of them. A guard that a name has cleared stays cleared.
    for (UniquedStringImpl* name : addedNames) {
        layouts.willAddProperty(vm, layout, name);
        m_state.objects->classes.didShadowMember(layout, name);
    }
    plan.addsExtraProperties = !addedNames.isEmpty();
    plan.storesAreDefinitions = std::ranges::none_of(creatableFieldNames.span(), isIntercepted);
    plan.mayCreateFields = !mayCreateFieldBetweenAddedNames && !creatableFieldNames.isEmpty() && plan.storesAreDefinitions;
    if (addedNames.isEmpty() || mayCreateFieldBetweenAddedNames)
        plan.numberOfFieldStoresBeforeAddedNames = fieldStores.size();
    plan.storesOnlyFieldsThatFit = plan.mayCreateFields && extraPropertyStores.isEmpty() && !extraProperties && std::ranges::all_of(fieldStores, [](const AssignmentPlan::FieldStore& store) {
        return store.sourceAlwaysFits && store.isDirect && store.from->slot.representation() != SlotRepresentation::DeferredClosure;
    });
    auto firstDefinedByName = std::ranges::find_if(fieldStores, [](const AssignmentPlan::FieldStore& store) {
        return store.isDefinedByName;
    });
    // As mayFinishByName() asks of a run that has met a value its field does not take.
    bool mayDefineByName = plan.storesAreDefinitions && canNumberStores;
    if (firstDefinedByName != fieldStores.end() && !mayDefineByName)
        return notePlanWithoutUse("a field of the target holds a closure not made yet, and the rest cannot be defined by name"_s);
    plan.numberOfFieldStoresInRun = firstDefinedByName - fieldStores.begin();
    plan.fieldStores = WTF::move(fieldStores);
    plan.extraPropertyStores = WTF::move(extraPropertyStores);
    plan.storesInSourceOrder = WTF::move(storesInSourceOrder);
    plan.isUsable = true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
