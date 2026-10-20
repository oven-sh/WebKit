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
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <algorithm>

namespace JSC { namespace AOT {

AssignmentPlanCache::AssignmentPlanCache(State& state)
    : m_state(state)
    , m_chainDependentOwner(state.chainDependents->addOwner(*this))
{
    state.caches->add(CacheKind::WeakByKey, "assignment plans"_s, *this);
}

AssignmentPlanCache::~AssignmentPlanCache() = default;

NEVER_INLINE AssignmentPlan* AssignmentPlanCache::makePlan(VM& vm, Object& target, JSObject* extraProperties, const AssignmentSource& source, Object::PlanCreation creation)
{
    StructureID extraPropertiesID = extraProperties ? extraProperties->structureID() : StructureID();
    if (creation == Object::PlanCreation::AtSecondCopy) {
        if (!m_copiesWithoutPlan)
            m_copiesWithoutPlan = makeUniqueWithoutFastMallocCheck<std::array<uint32_t, numberOfCopiesRemembered>>();
        uint32_t hash = hashFor(target.structureID(), extraPropertiesID, source);
        if (std::exchange((*m_copiesWithoutPlan)[hash % numberOfCopiesRemembered], hash) != hash)
            return nullptr;
    }
    if (!m_plans)
        m_plans = makeUniqueWithoutFastMallocCheck<Plans>();
    unsigned index = indexFor(target.structureID(), extraPropertiesID, source);
    AssignmentPlan& plan = (*m_plans)[index];
    plan = { };
    plan.target = target.structureID();
    plan.source = source.object->structureID();
    plan.sourcePropertyStorage = source.propertyStorageStructureID();
    plan.extraPropertiesBefore = extraPropertiesID;
    plan.sourceOrderWord = source.orderWord;
    plan.excludedNames.appendRange(source.excludedNames.begin(), source.excludedNames.end());
    m_writes.recordAll();
    count(vm, RuntimeCounterID::AssignmentPlansMade);
    fillPlan(vm, plan, index, target, extraProperties, source);
    return &plan;
}

void AssignmentPlanCache::dropChainDependent(VM&, uint32_t subject, UniquedStringImpl*)
{
    if (m_plans)
        (*m_plans)[subject] = { };
}

bool AssignmentPlanCache::hasChainDependent(VM&, uint32_t subject, UniquedStringImpl*) const
{
    return m_plans && (*m_plans)[subject].target;
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
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        for (auto& plan : *m_plans) {
            if (!plan.target)
                continue;
            if (isDeadStructure(plan.target) || isDeadStructure(plan.source) || isDeadStructure(plan.sourcePropertyStorage) || isDeadStructure(plan.extraPropertiesBefore) || isDead(plan.extraPropertiesAfter) || isDead(plan.targetAfter)) {
                plan.forgetCells();
                ++removed;
                continue;
            }
            for (auto& store : plan.fieldStores) {
                if (isDeadStructure(store.lastAcceptedStructure))
                    store.lastAcceptedStructure = { };
            }
        }
    });
    return removed;
}

namespace {

struct AssignedProperty {
    UniquedStringImpl* name;
    const FieldData* field;
    PropertyOffset offset;
};

}

static std::optional<Vector<AssignedProperty, 16>> listOwnProperties(VM& vm, const AssignmentSource& source)
{
    Vector<AssignedProperty, 16> properties;
    if (const Object* compiledSource = source.compiledObject; compiledSource && source.propertyStorage) {
        const Vector<PropertyListEntry>* listed = compiledSource->fieldsAndExtraPropertiesInKeyOrder(vm);
        if (!listed)
            return std::nullopt;
        for (const PropertyListEntry& property : *listed)
            properties.append({ property.name, property.field, property.offsetInExtraProperties });
        return properties;
    }
    if (const Object* compiledSource = source.compiledObject) {
        auto listed = compiledSource->state().objects->keyOrders.propertiesInOrder(vm, compiledSource->layout(), source.orderWord, Object::SymbolKeys::Include);
        if (!listed)
            return std::nullopt;
        for (auto& property : *listed)
            properties.append({ property.name, property.field, invalidOffset });
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

static std::optional<Vector<AssignedProperty, 16>> listAssignedProperties(VM& vm, const AssignmentSource& source)
{
    auto properties = listOwnProperties(vm, source);
    if (properties && !source.excludedNames.empty()) {
        properties->removeAllMatching([&](const AssignedProperty& property) {
            return std::ranges::contains(source.excludedNames, property.name);
        });
    }
    return properties;
}

void AssignmentPlanCache::fillPlan(VM& vm, AssignmentPlan& plan, unsigned index, Object& target, JSObject* extraProperties, const AssignmentSource& source)
{
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    auto properties = listAssignedProperties(vm, source);
    if (!properties)
        return;
    bool sharesTypes = source.compiledObject && &source.compiledObject->state() == &m_state;
    const LayoutData& layout = target.layout();
    JSGlobalObject* realm = target.structure()->realm();
    Structure* extraPropertiesStructure = extraProperties ? extraProperties->structure() : Instance::of(realm)->realm.emptyExtraPropertiesStructure;
    if (extraProperties && (extraProperties->type() != FinalObjectType || extraPropertiesStructure->isDictionary()))
        return;
    Vector<AssignmentPlan::FieldStore> fieldStores;
    Vector<AssignmentPlan::ExtraPropertyStore> extraPropertyStores;
    Vector<UniquedStringImpl*, 8> addedNames;
    Vector<UniquedStringImpl*, 8> creatableFieldNames;
    bool mayCreateFieldAfterAddedName = false;
    bool mayCreateFieldBetweenAddedNames = false;
    for (auto& property : *properties) {
        const FieldData* field = layouts.field(vm, layout, property.name);
        if (!field) {
            if (!layout.extraPropertiesSlot())
                return;
            unsigned attributes = 0;
            PropertyOffset offset = extraPropertiesStructure->get(vm, property.name, attributes);
            if (isValidOffset(offset)) {
                if (attributes)
                    return;
            } else {
                if (parseIndex(PropertyName(property.name)) || property.name == vm.propertyNames->underscoreProto.impl())
                    return;
                extraPropertiesStructure = Structure::addPropertyTransition(vm, extraPropertiesStructure, PropertyName(property.name), 0, offset);
                if (extraPropertiesStructure->isDictionary())
                    return;
                if (addedNames.isEmpty())
                    plan.numberOfFieldStoresBeforeAddedNames = fieldStores.size();
                mayCreateFieldBetweenAddedNames |= mayCreateFieldAfterAddedName;
                addedNames.append(property.name);
            }
            extraPropertyStores.append(AssignmentPlan::ExtraPropertyStore { .from = property.field, .sourceOffset = property.offset, .offset = offset });
            continue;
        }
        if (field->flags & fieldIsStoredOnce || field->slot.representation() == SlotRepresentation::DeferredClosure)
            return;
        if (extraProperties && isValidOffset(extraProperties->structure()->get(vm, property.name)))
            return;
        const SlotTraits traits = traitsOf(field->slot.representation());
        fieldStores.append(AssignmentPlan::FieldStore {
            .from = property.field,
            .sourceOffset = property.offset,
            .to = field,
            .acceptedKinds = kindsAlwaysAccepted(layouts.typeOf(*field)),
            .sourceAlwaysFits = sharesTypes && property.field && alwaysFits(layouts, *property.field, *field),
            .isAbsentWhenEmpty = !(field->flags & fieldMayExistWithoutStorage),
            .isDirect = !(field->isOutOfLine() && traits.absentBits),
        });
        if (field->isOutOfLine())
            plan.granuleBitsFilled |= granuleBitOf(field->slot.offset);
        if (layouts.mayBeAbsent(*field)) {
            mayCreateFieldAfterAddedName |= !addedNames.isEmpty();
            creatableFieldNames.append(property.name);
        }
    }
    auto isIntercepted = [&](UniquedStringImpl* name) {
        return !storeCreatesOwnProperty(realm, target.structure(), &target, name, ChainDependent { name, index, m_chainDependentOwner });
    };
    if (std::ranges::any_of(addedNames.span(), isIntercepted))
        return;
    if (!extraPropertyStores.isEmpty()) {
        plan.extraPropertiesAfter = extraPropertiesStructure;
        plan.targetAfter = m_state.objects->structures.withExtraProperties(vm, target.compiledStructure(), extraPropertiesStructure);
    }
    plan.addsExtraProperties = !addedNames.isEmpty();
    plan.mayCreateFields = !mayCreateFieldBetweenAddedNames && !creatableFieldNames.isEmpty() && std::ranges::none_of(creatableFieldNames.span(), isIntercepted);
    if (addedNames.isEmpty() || mayCreateFieldBetweenAddedNames)
        plan.numberOfFieldStoresBeforeAddedNames = fieldStores.size();
    plan.fieldStores = WTF::move(fieldStores);
    plan.extraPropertyStores = WTF::move(extraPropertyStores);
    plan.isUsable = true;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
