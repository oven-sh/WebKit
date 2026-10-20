/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSolver.h"

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowFixpointCheck.h"
#include "AOTFlowPropertyOperations.h"
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

Witness FlowSolver::witnessOfNode(FlowNodeID node) const
{
    if (!node)
        return noWitness;
    // What comes into a location of an object that unseen code holds gets into its hands for the reason the object did.
    if (FlowObjectID holder = m_graph.objectOf(node); holder && holder != unknownFlowObject)
        return m_records.witnessOfReach(holder);
    if (m_graph.kindOf(node) != FlowNodeKind::Local)
        return noWitness;
    FlowInstanceID instance = m_graph.instanceOf(node);
    return witnessAt(instance, m_graph.summaryOf(instance).originOf(m_graph.localOf(node)));
}

Vector<FlowNodeID, 8> FlowSolver::nodesOf(FlowStatementSite site, FlowLocalList list) const
{
    std::span<const FlowLocalID> locals = summaryOf(site).statements.list(list);
    Vector<FlowNodeID, 8> nodes;
    nodes.reserveInitialCapacity(locals.size());
    for (FlowLocalID local : locals)
        nodes.append(node(site, local));
    return nodes;
}

void FlowSolver::noteMutation(FlowObjectID object, OptionSet<ObjectMutation> mutations, const Witness& witness)
{
    OptionSet<ObjectMutation> added = m_records.addMutations(object, mutations, witness);
    if (added.isEmpty())
        return;
    ++m_counters.changes;
    forgetAnswersOfLookupsWaitingOnMutations(*this, object);
    if (m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->didAddMutations(object, added);

    m_waiters.onMutations.forEachWaiterOn(object, [&](const MutationWaiter& waiter) {
        if (waiter.mutationsToRevisitFor.containsAny(added))
            revisit(waiter);
        else if (waiter.mutationsToContinueFor.containsAny(added))
            queueStatement(waiter.site);
    });
}

void FlowSolver::revisitMutationWaiters(FlowObjectID object)
{
    forgetAnswersOfLookupsWaitingOnMutations(*this, object);
    m_waiters.onMutations.forEachWaiterOn(object, [&](const MutationWaiter& waiter) {
        if (waiter.mutationsToRevisitFor.isEmpty())
            queueStatement(waiter.site);
        else
            revisit(waiter);
    });
}

void FlowSolver::revisitPropertyWaiters(FlowObjectID object, PropertyKeyID key)
{
    forgetAnswersOfLookupsWaitingOnProperty(*this, object, key);
    m_waiters.onNamedProperties.forEachWaiterOn({ object, key }, [&](const Waiter& waiter) {
        revisit(waiter);
    });
}

void FlowSolver::noteMutationOf(const FlowObjectSet& objects, OptionSet<ObjectMutation> mutations, const Witness& witness)
{
    objects.forEach([&](FlowObjectID object) {
        noteMutation(object, mutations, witness);
    });
}

void FlowSolver::noteUse(FlowObjectID object, FlowObjectUse use)
{
    if (m_records.addUse(object, use) && m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->didAddUse(object, use);
}

void FlowSolver::noteUseOf(const FlowObjectSet& objects, FlowObjectUse use)
{
    objects.forEach([&](FlowObjectID object) {
        noteUse(object, use);
    });
}

bool FlowSolver::noteEvent(FlowObjectID object, FlowObjectEvent event, const Witness& witness)
{
    bool isNew = m_records.addEvent(object, event, witness);
    if (isNew && m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->didAddEvent(object, event);
    return isNew;
}

void FlowSolver::noteLocationEvent(FlowNodeID location, OptionSet<FlowLocationEvent> events)
{
    if (!location)
        return;

    OptionSet<FlowLocationEvent>& knownEvents = m_records.ensureLocationRecord(location).events;
    OptionSet<FlowLocationEvent> addedEvents = events - knownEvents;
    if (m_fixpointCheck && !addedEvents.isEmpty()) [[unlikely]]
        m_fixpointCheck->didAddLocationEvents(location, addedEvents);
    knownEvents.add(events);
    // A lookup asks for some of these events by name: a location that was redefined still shows that the property is present until it is found missing at the allocation.
    if (addedEvents.containsAny(emptyingLocationEvents))
        revisitLookupsEndingAt(location);
}

void FlowSolver::revisitLookupsEndingAt(FlowNodeID location)
{
    FlowNodeDescription description = m_graph.describe(location);
    switch (description.kind) {
    case FlowNodeKind::Property:
        revisitPropertyWaiters(description.object, description.key);
        return;
    case FlowNodeKind::DeclaredPart:
        if (m_objects.hasLayout(description.object)) {
            if (PropertyKeyID key = m_program.at(m_objects.at(description.object).layout).fields[description.index].key)
                revisitPropertyWaiters(description.object, key);
        }
        return;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSolver::noteDirectUses(FlowNodeID node, OperandUses uses)
{
    if (!node)
        return;
    if (m_fixpointCheck && !m_records.directUsesOf(node).containsAll(uses)) [[unlikely]]
        m_fixpointCheck->didAddDirectUses(node, uses - m_records.directUsesOf(node));
    m_records.addDirectUses(node, uses);
}

FlowObjectID FlowSolver::appendObject(const FlowObject& record)
{
    FlowObjectID object = m_objects.append(record);
    if (object == unknownFlowObject)
        return object;
    m_graph.didAppendObject(object);
    return object;
}

FlowNodeID FlowSolver::ensureOpenPart(FlowObjectID object, FlowOpenPart part)
{
    if (FlowNodeID existing = m_graph.find(object, part))
        return existing;
    FlowNodeID node = m_graph.ensure(object, part);
    didCreateNode(object, PropertyKeyID(), node);
    return node;
}

FlowNodeID FlowSolver::ensurePropertyValue(FlowObjectID object, PropertyKeyID key)
{
    bool isNew = false;
    FlowNodeID node = m_graph.ensurePropertyValue(object, key, isNew);
    if (isNew)
        didCreateNode(object, key, node);
    return node;
}

FlowNodeID FlowSolver::ensureGetter(FlowObjectID object, PropertyKeyID key)
{
    bool isNew = false;
    FlowNodeID node = m_graph.ensureGetter(object, key, isNew);
    if (isNew)
        didCreateNode(object, key, node);
    return node;
}

FlowNodeID FlowSolver::ensureSetter(FlowObjectID object, PropertyKeyID key)
{
    bool isNew = false;
    FlowNodeID node = m_graph.ensureSetter(object, key, isNew);
    if (isNew)
        didCreateNode(object, key, node);
    return node;
}

void FlowSolver::didCreateNode(FlowObjectID object, PropertyKeyID key, FlowNodeID node)
{
    forgetAnswersOfLookupsThrough(*this, object);
    didCreateLocation(*this, object, key, node);
    if (hasReachedNativeCode(object))
        exposeLocation(*this, object, node);
    if (key)
        revisitPropertyWaiters(object, key);
    m_waiters.onProperties.forEachWaiterOn(object, [&](const Waiter& waiter) {
        revisit(waiter);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
