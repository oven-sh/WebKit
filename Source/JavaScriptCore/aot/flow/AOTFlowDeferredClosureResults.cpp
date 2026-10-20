/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool canHaveDeferredClosures(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
        return true;
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

// Code generation emits nothing for a store into a deferred slot, so no later store may replace a closure that a read has already created.
static bool isFixedAtCreation(const LocationFlow& location)
{
    return location.initialization == InitializationState::SetAtCreation && location.stores == StoreCount::AtMostOnce;
}

static bool storesOnlyDeferrableClosures(const FlowLocationFacts& facts)
{
    return facts.events.contains(FlowLocationEvent::StoredDeferrableClosure)
        && !facts.events.contains(FlowLocationEvent::StoredOtherValue);
}

static bool hasSingleDeferrableFunction(const FlowLocationFacts& facts)
{
    return facts.hasOneDeferrableFunction && facts.deferrableFunction && facts.deferrableCapture != ClosureDeferral::Unknown;
}

static bool containsOnlyFunction(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject, FunctionID function)
{
    const FlowObjectTable& objects = inputs.objects();
    bool hasOtherValue = false;
    forEachFieldNode(inputs, subject, [&](FlowObjectID, FlowNodeID node) {
        const FlowValue& value = inputs.graph().valueOf(node);
        hasOtherValue |= !value.hasOnlyObjects() || value.objects.anyOf([&](FlowObjectID object) {
            return objects.kindOf(object) != FlowObjectKind::Closure || objects.at(object).function != function;
        });
    });
    return !hasOtherValue;
}

// A slot that defers its closure is never left: a deletion, a redefinition or a store of something that is no cell turns the object into an ordinary one, and a stored cell goes into the slot.
// So what unknown JavaScript does to an object that reaches it is no obstacle, and neither is a store of seen code under an unknown key, which goes by name at run time: such an object
// is not proven to keep its shape anyway. A store that names the field is an obstacle: nothing is emitted for a store into the slot.
static std::optional<FlowLocationFacts> factsOfSeenCodeAboutOpenedObjects(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject)
{
    if (!subject.declaredIndex || !subject.key)
        return std::nullopt;

    FlowLocationFacts facts;
    bool isDeferrable = true;
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout)) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout))
            isDeferrable &= inputs.factsOf(object).mutations.contains(ObjectMutation::ReachUnknownJavaScript);
    }
    size_t numberOfNodes = 0;
    forEachFieldNode(inputs, subject, [&](FlowObjectID object, FlowNodeID node) {
        ++numberOfNodes;
        const FlowLocationRecord& record = inputs.records().recordOf(node);
        facts.add(record);
        isDeferrable &= record.events.containsAll({ FlowLocationEvent::InitializedAtAllocation, FlowLocationEvent::StoredDeferrableClosure })
            && !record.events.containsAny({ FlowLocationEvent::MissingAtAllocation, FlowLocationEvent::StoredByOperation })
            && !record.isGivenOtherValueByName
            && !aliasedEventsOf(inputs.solution, object, subject.key).contains(FlowLocationEvent::StoredByOperation);
    });
    size_t numberOfObjects = 0;
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout))
        numberOfObjects += inputs.hierarchy.objectsOf(layout).size();
    if (!isDeferrable || !numberOfNodes || numberOfNodes != numberOfObjects)
        return std::nullopt;
    return facts;
}

static void computeDeferral(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject, LocationFlow& location)
{
    if (!isFixedAtCreation(location)) {
        std::optional<FlowLocationFacts> facts = factsOfSeenCodeAboutOpenedObjects(inputs, subject);
        if (!facts || !hasSingleDeferrableFunction(*facts))
            return;
        location.deferral = facts->deferrableCapture;
        location.deferredFunction = facts->deferrableFunction;
        return;
    }

    FlowLocationFacts facts = collectLocationFacts(inputs, subject);
    if (!storesOnlyDeferrableClosures(facts) || !hasSingleDeferrableFunction(facts))
        return;
    if (!containsOnlyFunction(inputs, subject, facts.deferrableFunction))
        return;

    location.deferral = facts.deferrableCapture;
    location.deferredFunction = facts.deferrableFunction;
}

static void computeDeferralsOfLayout(const FlowFamilyInputs& inputs, LayoutID id, std::span<const PropertyKeyID> appendedKeys, FlowFieldResults& fields)
{
    const Layout& layout = inputs.program().at(id);
    Vector<FieldFlow>& declared = fields.declared[id.index()];
    Vector<FieldFlow>& appended = fields.appended[id.index()];
    RELEASE_ASSERT(declared.size() == layout.fields.size());
    RELEASE_ASSERT(appended.size() == appendedKeys.size());

    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        if (PropertyKeyID key = layout.fields[index].key)
            computeDeferral(inputs, FlowFieldSubject { id, index, key }, declared[index].location);
    }
    for (size_t index = 0; index < appendedKeys.size(); ++index)
        computeDeferral(inputs, FlowFieldSubject { id, std::nullopt, appendedKeys[index] }, appended[index].location);
}

void computeDeferredClosureResults(const FlowFamilyInputs& inputs, const FlowAppendedKeys& appendedKeys, FlowFieldResults& fields)
{
    size_t layoutCount = inputs.program().layouts.size();
    RELEASE_ASSERT(appendedKeys.size() == layoutCount && fields.declared.size() == layoutCount && fields.appended.size() == layoutCount);

    forEachIndexInParallel(inputs.loops(), layoutCount, [&](unsigned index) {
        LayoutID id(index);
        if (canHaveDeferredClosures(inputs.program().at(id).kind) && inputs.isEnabled(FlowResultKind::DeferredClosures, index))
            computeDeferralsOfLayout(inputs, id, appendedKeys[index].span(), fields);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
