/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTFieldMoves.h"
#include "AOTObject.h"
#include "CollectionScope.h"
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include "StructureID.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct State;

struct AssignmentSource {
    ALWAYS_INLINE JSValue read(const auto& store) const
    {
        return store.from ? compiledObject->readInline(*store.from) : propertyStorage->getDirect(store.sourceOffset);
    }

    StructureID propertyStorageStructureID() const { return propertyStorage ? propertyStorage->structureID() : StructureID(); }

    JSObject* object { nullptr };
    const Object* compiledObject { nullptr };
    // An ordinary object itself, or the extra properties of a compiled object.
    JSObject* propertyStorage { nullptr };
    uint32_t orderWord { 0 };
    std::span<UniquedStringImpl* const> excludedNames { };
};

struct AssignmentPlan {
    struct FieldStore {
        const FieldData* from { nullptr };
        PropertyOffset sourceOffset { invalidOffset };
        const FieldData* to { nullptr };
        OptionSet<ValueKind> acceptedKinds;
        bool sourceAlwaysFits { false };
        bool isAbsentWhenEmpty { false };
        bool isDirect { false };
        StructureID lastAcceptedStructure { };
        std::optional<uint32_t> orderWordBeforeCreation { };
        uint32_t orderWordAfterCreation { 0 };
    };

    struct ExtraPropertyStore {
        const FieldData* from { nullptr };
        PropertyOffset sourceOffset { invalidOffset };
        PropertyOffset offset { invalidOffset };
    };

    bool isFor(StructureID targetID, StructureID extraPropertiesID, const AssignmentSource& from) const
    {
        return target == targetID && extraPropertiesBefore == extraPropertiesID && source == from.object->structureID()
            && sourcePropertyStorage == from.propertyStorageStructureID() && sourceOrderWord == from.orderWord
            && std::ranges::equal(excludedNames, from.excludedNames, { }, &RefPtr<UniquedStringImpl>::get);
    }

    // For the collector, which may run in a thread of its own: a name is released by the thread whose table of names holds it, when the plan is made anew.
    void forgetCells()
    {
        Vector<RefPtr<UniquedStringImpl>, 2> names = WTF::move(excludedNames);
        *this = { };
        excludedNames = WTF::move(names);
    }

    StructureID target;
    StructureID source;
    StructureID sourcePropertyStorage;
    StructureID extraPropertiesBefore;
    uint32_t sourceOrderWord { 0 };
    Vector<RefPtr<UniquedStringImpl>, 2> excludedNames;
    bool isUsable { false };
    bool addsExtraProperties { false };
    bool mayCreateFields { false };
    uint64_t granuleBitsFilled { 0 };
    Structure* extraPropertiesAfter { nullptr };
    CompiledStructure* targetAfter { nullptr };
    // A field that the run creates is listed behind the names that the run has added by then, so the stores behind the first added name run after the extra properties.
    Vector<FieldStore> fieldStores;
    unsigned numberOfFieldStoresBeforeAddedNames { 0 };
    Vector<ExtraPropertyStore> extraPropertyStores;
};

class AssignmentPlanCache final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(AssignmentPlanCache);
public:
    explicit AssignmentPlanCache(State&);
    ~AssignmentPlanCache() final;

    static constexpr unsigned numberOfPlans = 64;

    ALWAYS_INLINE AssignmentPlan* planFor(VM&, Object& target, JSObject* extraProperties, const AssignmentSource&, Object::PlanCreation);
    // A run that writes a Structure into its plan says so, or a collection of the young cells alone would not look at the plan.
    void didWritePlan() { m_writes.recordAll(); }
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using Plans = std::array<AssignmentPlan, numberOfPlans>;
    static constexpr unsigned numberOfCopiesRemembered = 256;

    static uint32_t hashFor(StructureID target, StructureID extraProperties, const AssignmentSource& source)
    {
        uintptr_t firstExcludedName = source.excludedNames.empty() ? 0 : std::bit_cast<uintptr_t>(source.excludedNames.front());
        return static_cast<uint32_t>((target.bits() >> 4) + (source.object->structureID().bits() >> 4) * 31 + (source.propertyStorageStructureID().bits() >> 4) * 13 + (extraProperties.bits() >> 4) * 7 + source.orderWord + (firstExcludedName >> 4) * 5);
    }
    static unsigned indexFor(StructureID target, StructureID extraProperties, const AssignmentSource& source) { return hashFor(target, extraProperties, source) % numberOfPlans; }
    AssignmentPlan* makePlan(VM&, Object& target, JSObject* extraProperties, const AssignmentSource&, Object::PlanCreation);
    void fillPlan(VM&, AssignmentPlan&, unsigned index, Object& target, JSObject* extraProperties, const AssignmentSource&);

    void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const final;

    State& m_state;
    ChainDependentOwnerID m_chainDependentOwner;
    std::unique_ptr<Plans> m_plans;
    // Hashes only: a hash that two copies share by chance makes a plan one copy early.
    std::unique_ptr<std::array<uint32_t, numberOfCopiesRemembered>> m_copiesWithoutPlan;
    RememberedSlotSet m_writes;
};

ALWAYS_INLINE AssignmentPlan* AssignmentPlanCache::planFor(VM& vm, Object& target, JSObject* extraProperties, const AssignmentSource& source, Object::PlanCreation creation)
{
    if (m_plans) [[likely]] {
        StructureID targetID = target.structureID();
        StructureID extraPropertiesID = extraProperties ? extraProperties->structureID() : StructureID();
        AssignmentPlan& plan = (*m_plans)[indexFor(targetID, extraPropertiesID, source)];
        if (plan.isFor(targetID, extraPropertiesID, source)) [[likely]]
            return &plan;
    }
    return makePlan(vm, target, extraProperties, source, creation);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
