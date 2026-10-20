/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTFlowParts.h"
#include "AOTLayout.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static TypeID declaredTypeOfInheritedField(const FlowGraph& graph, FlowNodeID property)
{
    const FlowObjectTable& objects = graph.objects();
    FlowObjectID object = graph.objectOf(property);
    if (!objects.hasLayout(object))
        return { };

    const Program& program = graph.program();
    LayoutID layout = objects.at(object).layout;
    if (program.at(layout).isEnvironment())
        return { };

    std::optional<FieldReference> reference = findField(program, layout, graph.describe(property).key, BaseChain::Predicted);
    if (!reference)
        return { };
    return fieldAt(program, *reference).declared.type;
}

TypeID enforcedTypeOf(const FlowGraph& graph, FlowNodeID location)
{
    if (!location)
        return { };

    switch (graph.kindOf(location)) {
    case FlowNodeKind::Variable:
    case FlowNodeKind::DeclaredPart:
        return graph.typeAsRead(location);
    case FlowNodeKind::Property:
        return declaredTypeOfInheritedField(graph, location);
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
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
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void noteStore(FlowSolver& solver, const FlowLocationStore& store)
{
    const FlowGraph& graph = solver.graph();
    FlowLocalID storedLocal;
    if (store.value && graph.kindOf(store.value) == FlowNodeKind::Local && graph.instanceOf(store.value) == store.site.instance)
        storedLocal = graph.localOf(store.value);

    recordStore(solver, { store.site, store.origin, storedLocal, store.naming }, store.location);
    if (store.isInCycle)
        solver.noteLocationEvent(store.location, FlowLocationEvent::StoredByManyOperations);
}

FlowStoredType checkStoredType(FlowSolver& solver, const FlowLocationStore& store)
{
    if (!store.value)
        return FlowStoredType::Inside;

    const FlowGraph& graph = solver.graph();
    TypeID declared = enforcedTypeOf(graph, store.location);
    if (!declared)
        return FlowStoredType::Inside;

    const Program& program = solver.program();
    TypeID stored = graph.typeAsRead(store.value);
    if (stored && program.at(stored).isSubsetOf(program.at(declared), program))
        return FlowStoredType::Inside;

    if (!solver.values().filterOut(solver.valueOf(store.value), program.at(declared)).isEmpty())
        return FlowStoredType::MayBeOutside;

    // What is outside stays outside, so only this answer can change.
    solver.waitOn(store.value, store.site, FlowWakeUp::Revisit);
    return FlowStoredType::Inside;
}

static void recordLocationStore(FlowSolver& solver, const FlowLocationStore& store)
{
    noteStore(solver, store);
    if (checkStoredType(solver, store) == FlowStoredType::Inside)
        return;

    Witness witness = solver.witnessAt(store.site, store.origin);
    FlowLocationRecord& record = solver.records().ensureLocationRecord(store.location);
    record.events.add(FlowLocationEvent::StoredOutsideType);
    keepLowerWitness(record.firstLoss, witness);
    if (FlowObjectID owner = solver.graph().objectOf(store.location))
        solver.noteMutation(owner, ObjectMutation::StoreOfOtherType, witness);
}

void runLocationAccess(FlowSolver& solver, FlowStatementSite site, const FlowLocationAccess& statement)
{
    const FlowGraph& graph = solver.graph();
    FlowNodeID location = solver.node(site, statement.location);
    FlowNodeID value = solver.node(site, statement.value);

    switch (statement.kind) {
    case FlowAccessKind::Load: {
        bool isReadByUnseenCode = value == graph.nativeCode() || value == graph.unknownJavaScript();
        solver.noteLocationEvent(location, isReadByUnseenCode ? FlowLocationEvent::ReadByUnknownCode : readEventOf(statement.use));
        return;
    }
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
        recordLocationStore(solver, { site, statement.origin, value, location, statement.isInCycle, FlowStoreNaming::NamesLocation });
        return;
    case FlowAccessKind::Test:
        solver.noteLocationEvent(location, FlowLocationEvent::PresenceObserved);
        return;
    case FlowAccessKind::Delete:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
