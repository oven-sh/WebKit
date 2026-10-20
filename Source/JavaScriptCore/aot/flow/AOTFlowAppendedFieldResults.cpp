/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowAppendedFields.h"

#if ENABLE(AOT)

#include "AOTFlowKeys.h"
#include <algorithm>
#include <utility>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static void forEachProvenAncestor(const FlowHierarchy& hierarchy, LayoutID layout, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    for (const FlowBaseResult* link = &hierarchy.baseOf(layout); link->state == BaseState::Layout; link = &hierarchy.baseOf(link->base))
        functor(link->base);
}

bool canAppendFieldsTo(const FlowFamilyInputs& inputs, LayoutID layout)
{
    switch (inputs.program().at(layout).kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
        return inputs.isEnabled(FlowResultKind::AppendedFields, layout.index());
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canNameField(const Program& program, std::span<const PropertyKeyID> symbolsCreatedOnce, PropertyKeyID key)
{
    switch (classOf(program, key)) {
    case FlowKeyClass::String:
        return !program.isIndexLike(key);
    case FlowKeyClass::Symbol:
        // The key of a call that may run again stands for several symbols, and a field has one key.
        return program.at(key).kind != PropertyKeyKind::CreatedSymbol || std::ranges::binary_search(symbolsCreatedOnce, key);
    case FlowKeyClass::Index:
    case FlowKeyClass::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isStoredDataProperty(const FlowFamilyInputs& inputs, const FlowProperty& property)
{
    if (!property.value || property.getter || property.setter)
        return false;
    if (inputs.graph().kindOf(property.value) != FlowNodeKind::Property)
        return false;
    return inputs.records().recordOf(property.value).events.containsAny({ FlowLocationEvent::StoredByOperation, FlowLocationEvent::StoredByManyOperations });
}

void appendStoredKeys(const FlowFamilyInputs& inputs, FlowObjectID object, Vector<PropertyKeyID>& keys)
{
    for (const FlowProperty& property : inputs.graph().propertiesOf(object)) {
        if (isStoredDataProperty(inputs, property))
            keys.append(property.key);
    }
}

static bool isFieldOfTaggedLayout(const FlowFamilyInputs& inputs, LayoutID layout, PropertyKeyID key)
{
    const Program& program = inputs.program();
    return std::ranges::any_of(inputs.hierarchy.layoutsBelow(declaredLayoutOf(program, layout)), [&](LayoutID below) {
        if (!program.at(below).isDerived())
            return false;
        std::optional<uint32_t> field = program.at(below).findField(key);
        return field && !program.at(below).fields[*field].flags.contains(FieldFlags::IsInherited);
    });
}

void keepCandidateKeys(const FlowFamilyInputs& inputs, std::span<const PropertyKeyID> symbolsCreatedOnce, LayoutID layout, Vector<PropertyKeyID>& keys)
{
    std::ranges::sort(keys);
    removeRepeatedElements(keys);
    // A store through a receiver that nobody knows reaches the objects of every layout. Where the program declares types for the objects, a name that none of them has gets no room.
    Vector<LayoutSetID, 2> families = declaredFamiliesOf(inputs.program(), layout);
    keys.removeAllMatching([&](PropertyKeyID key) {
        if (!canNameField(inputs.program(), symbolsCreatedOnce, key) || inputs.hierarchy.findDeclaredField(layout, key).has_value())
            return true;
        if (isFieldOfTaggedLayout(inputs, layout, key))
            return true;
        return !families.isEmpty() && !isDeclaredIn(inputs.program(), families.span(), key);
    });
}

// What is stored on an object that is created once gets no room in the objects of the layout that are created again and again: `Child.prototype = new Parent()` with the methods of Child.
static Vector<PropertyKeyID> collectCandidateKeys(const FlowFamilyInputs& inputs, std::span<const PropertyKeyID> symbolsCreatedOnce, LayoutID layout)
{
    auto objects = inputs.hierarchy.objectsOf(layout);
    bool hasObjectCreatedAgain = std::ranges::any_of(objects, [&](FlowObjectID object) {
        return inputs.factsOf(object).events.contains(FlowObjectEvent::Allocated) && !isCreatedOnce(inputs, object);
    });

    Vector<PropertyKeyID> keys;
    for (FlowObjectID object : objects) {
        if (!hasObjectCreatedAgain || !isCreatedOnce(inputs, object))
            appendStoredKeys(inputs, object, keys);
    }
    keepCandidateKeys(inputs, symbolsCreatedOnce, layout, keys);
    return keys;
}

static uint64_t countFieldsOnChain(const FlowFamilyInputs& inputs, LayoutID layout)
{
    uint64_t count = inputs.program().at(layout).fields.size();
    forEachProvenAncestor(inputs.hierarchy, layout, [&](LayoutID ancestor) {
        count += inputs.program().at(ancestor).fields.size();
    });
    return count;
}

// A layout below inherits the appended fields on top of the fields it has.
static uint64_t largestFieldCountBelow(const FlowHierarchy& hierarchy, LayoutID layout, std::span<const uint64_t> fieldCountsAsAnalyzed)
{
    uint64_t largest = 0;
    for (LayoutID below : hierarchy.layoutsBelow(layout))
        largest = std::max(largest, fieldCountsAsAnalyzed[below.index()]);
    return largest;
}

static uint64_t removeInheritedKeys(const FlowHierarchy& hierarchy, LayoutID layout, FlowAppendedKeys& keysByLayout)
{
    Vector<PropertyKeyID>& keys = keysByLayout[layout.index()];
    uint64_t numberOfInheritedKeys = 0;
    forEachProvenAncestor(hierarchy, layout, [&](LayoutID ancestor) {
        const Vector<PropertyKeyID>& inheritedKeys = keysByLayout[ancestor.index()];
        if (inheritedKeys.isEmpty())
            return;
        numberOfInheritedKeys += inheritedKeys.size();
        keys.removeAllMatching([&](PropertyKeyID key) {
            return std::ranges::binary_search(inheritedKeys, key);
        });
    });
    return numberOfInheritedKeys;
}

namespace {

// How many operations of reached functions name each key. Counted when the first layout has more names than it may keep.
class AccessCounts {
public:
    explicit AccessCounts(const FlowFamilyInputs& inputs)
        : m_inputs(inputs)
    {
    }

    uint32_t of(PropertyKeyID key)
    {
        if (!m_isCounted)
            count();
        return m_counts.get(key);
    }

private:
    void count()
    {
        m_isCounted = true;
        for (const FlowSummary& summary : m_inputs.solution.summaries) {
            if (!summary.hasBody() || m_inputs.graph().isHost(summary.function) || !m_inputs.graph().isReached(summary.function))
                continue;
            for (const FlowPropertyAccess& access : summary.statements.propertyAccesses) {
                if (access.key)
                    ++m_counts.add(access.key, 0u).iterator->value;
            }
        }
    }

    const FlowFamilyInputs& m_inputs;
    HashMap<PropertyKeyID, uint32_t> m_counts;
    bool m_isCounted { false };
};

} // anonymous namespace

// The names that the program mentions least go to the extra properties.
static void keepMostAccessedKeys(const FlowFamilyInputs& inputs, AccessCounts& counts, LimitID limit, LayoutID layout, uint64_t numberOfOtherFields, Vector<PropertyKeyID>& keys)
{
    if (keys.isEmpty() || !inputs.build().exceeds(limit, numberOfOtherFields + keys.size(), Subject { .layout = layout }))
        return;
    uint64_t allowed = inputs.build().limit(limit);
    std::ranges::sort(keys, [&](PropertyKeyID a, PropertyKeyID b) {
        return std::pair { counts.of(b), a } < std::pair { counts.of(a), b };
    });
    keys.shrink(allowed > numberOfOtherFields ? allowed - numberOfOtherFields : 0);
    std::ranges::sort(keys);
}

FlowAppendedKeys computeAppendedFieldKeys(const FlowFamilyInputs& inputs, std::span<const PropertyKeyID> symbolsCreatedOnce)
{
    const FlowHierarchy& hierarchy = inputs.hierarchy;
    unsigned numberOfLayouts = inputs.program().layouts.size();

    FlowAppendedKeys keysByLayout(numberOfLayouts);
    Vector<uint64_t> fieldCountsAsAnalyzed(numberOfLayouts);
    forEachIndexInParallel(inputs.loops(), numberOfLayouts, [&](unsigned index) {
        LayoutID layout(index);
        fieldCountsAsAnalyzed[index] = countFieldsOnChain(inputs, layout);
        if (canAppendFieldsTo(inputs, layout))
            keysByLayout[index] = collectCandidateKeys(inputs, symbolsCreatedOnce, layout);
    });

    AccessCounts counts(inputs);
    for (LayoutID layout : hierarchy.layoutsFromBasesDown()) {
        Vector<PropertyKeyID>& keys = keysByLayout[layout.index()];
        if (keys.isEmpty())
            continue;
        uint64_t numberOfInheritedKeys = removeInheritedKeys(hierarchy, layout, keysByLayout);
        keepMostAccessedKeys(inputs, counts, LimitID::MaxAppendedFieldsPerLayout, layout, 0, keys);
        keepMostAccessedKeys(inputs, counts, LimitID::FieldsInKeyOrder, layout, largestFieldCountBelow(hierarchy, layout, fieldCountsAsAnalyzed.span()) + numberOfInheritedKeys, keys);
    }
    return keysByLayout;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
