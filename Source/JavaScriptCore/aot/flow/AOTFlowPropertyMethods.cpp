/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct MethodDefinitionContext {
    FlowSolver& solver;
    FlowStatementSite site;
    const FlowMethodDefinition& statement;
    const FlowKeySet& keys;
    const FlowKeySet& unnamedKeys;
    FlowNodeID function;
    const FlowObjectSet& functionObjects;
    Witness witness;
};

static FlowAccessKind accessKindOf(MethodKind kind)
{
    switch (kind) {
    case MethodKind::Method:
        return FlowAccessKind::Define;
    case MethodKind::Getter:
        return FlowAccessKind::DefineGetter;
    case MethodKind::Setter:
        return FlowAccessKind::DefineSetter;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowObjectID ensureMemberHolder(FlowSolver& solver, ClassSide side, FlowObjectID classObject)
{
    switch (side) {
    case ClassSide::Static:
        return classObject;
    case ClassSide::Prototype:
        return ensurePrototypeObject(solver, classObject);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowNodeID ensureMemberNode(FlowSolver& solver, MethodKind kind, FlowObjectID holder, PropertyKeyID name)
{
    switch (kind) {
    case MethodKind::Method:
        return ensureOwnValue(solver, holder, name);
    case MethodKind::Getter:
    case MethodKind::Setter:
        return ensureOwnAccessor(solver, holder, name, kind);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void noteMemberMayBeMissing(FlowSolver& solver, FlowNodeID node)
{
    solver.noteLocationEvent(node, FlowLocationEvent::MissingAtAllocation);
    solver.noteLocationEvent(node, overwrittenLocationEvents);
}

static void noteMemberPresence(const MethodDefinitionContext& context, FlowNodeID node)
{
    FlowSolver& solver = context.solver;
    const FlowMethodDefinition& statement = context.statement;

    // The value of a computed key may grow until this name is one of many or is no longer listed. A lookup without a waiter and a store that runs only the setter are not told.
    bool mayGetOtherName = !statement.key;
    if (mayGetOtherName && statement.methodKind == MethodKind::Method)
        solver.joinPrimitives(node, Primitive::Undefined);

    if (!context.keys.onlyName()) {
        noteMemberMayBeMissing(solver, node);
        return;
    }

    if (!mayGetOtherName || statement.methodKind != MethodKind::Setter)
        solver.noteLocationEvent(node, FlowLocationEvent::InitializedAtAllocation);
}

static bool isNamedLocationOf(const FlowGraph& graph, FlowObjectID holder, FlowNodeID node)
{
    switch (graph.kindOf(node)) {
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
        return graph.describe(node).object == holder;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A name that the key held alone on an earlier run may have left the listed names. The edges out of the function lead to each location that it was defined in.
static void withdrawMembersUnderEarlierNames(const MethodDefinitionContext& context, FlowObjectID holder)
{
    FlowSolver& solver = context.solver;
    const FlowGraph& graph = solver.graph();

    Vector<FlowNodeID, 4> members;
    graph.forEachEdge(context.function, [&](const FlowEdge& edge) {
        if (isNamedLocationOf(graph, holder, edge.target))
            members.append(edge.target);
    });
    for (FlowNodeID member : members)
        noteMemberMayBeMissing(solver, member);
}

static void noteRedefinedMember(const MethodDefinitionContext& context, FlowObjectID holder, PropertyKeyID name)
{
    FlowSolver& solver = context.solver;
    // The other location under the name may be created after this run.
    solver.waitOnProperty(holder, name, context.site);
    FlowOwnProperty own = findOwnProperty(FlowWalkInputs::of(solver), holderOf(solver.objects(), holder), name);
    if (!own.value || (!own.getter && !own.setter))
        return;
    for (FlowNodeID node : { own.value, own.getter, own.setter }) {
        if (!node)
            continue;
        solver.noteLocationEvent(node, FlowLocationEvent::Redefined);
        keepLowerWitness(solver.records().ensureLocationRecord(node).firstLoss, context.witness);
    }
}

// A null name stands for a key nobody can name.
static void recordMemberDefinitions(const MethodDefinitionContext& context, FlowObjectID holder, PropertyKeyID name)
{
    FlowSolver& solver = context.solver;
    const FlowMethodDefinition& statement = context.statement;
    context.functionObjects.forEach([&](FlowObjectID functionObject) {
        const FlowObject& record = solver.objects().at(functionObject);
        if (record.kind != FlowObjectKind::Closure)
            return;
        solver.records().addMemberDefinition(FlowMemberDefinition { statement.layout, statement.side, statement.methodKind, statement.index, statement.key, name, record.function, holder, context.witness });
    });
}

static void defineNamedMember(const MethodDefinitionContext& context, FlowObjectID holder, PropertyKeyID name)
{
    FlowSolver& solver = context.solver;
    const FlowMethodDefinition& statement = context.statement;

    FlowNodeID node = ensureMemberNode(solver, statement.methodKind, holder, name);
    if (!node)
        return;

    solver.connect(context.function, node);
    noteMemberPresence(context, node);
    noteRedefinedMember(context, holder, name);
    recordMemberDefinitions(context, holder, name);
}

static void defineUnnamedMembers(const MethodDefinitionContext& context, FlowObjectID holder)
{
    if (context.unnamedKeys.isEmpty())
        return;

    FlowSolver& solver = context.solver;
    const FlowMethodDefinition& statement = context.statement;
    if (context.unnamedKeys.hasUnnamedKeys())
        recordMemberDefinitions(context, holder, PropertyKeyID());

    FlowAccess access { context.site, statement.origin, accessKindOf(statement.methodKind), FlowReadUse::Value, { }, solver.node(context.site, statement.classObject), context.function, { }, { } };
    accessProperties(solver, access, FlowObjectSet(holder), context.unnamedKeys);
}

static void defineMembers(const MethodDefinitionContext& context, FlowObjectID classObject)
{
    FlowSolver& solver = context.solver;
    if (solver.objects().kindOf(classObject) != FlowObjectKind::Closure) {
        handToUnknownRecipient(solver, context.function, context.witness);
        return;
    }

    FlowObjectID holder = ensureMemberHolder(solver, context.statement.side, classObject);
    if (!holder)
        return;

    for (PropertyKeyID name : context.keys.names)
        defineNamedMember(context, holder, name);
    defineUnnamedMembers(context, holder);
    if (!context.keys.onlyName())
        withdrawMembersUnderEarlierNames(context, holder);
}

void runMethodDefinition(FlowSolver& solver, FlowStatementSite site, const FlowMethodDefinition& statement)
{
    noteDeclaredOperandUses(solver, site, statement.origin);

    FlowKeySet keys = statement.key ? keySetOf(solver.program(), statement.key) : solver.values().keysOf(solver.valueAt(site, statement.computedKey));
    FlowKeySet unnamedKeys = keys;
    unnamedKeys.names.clear();

    FlowNodeID function = solver.node(site, statement.function);
    FlowObjectSet functionObjects = solver.valueOf(function).objects;
    MethodDefinitionContext context { solver, site, statement, keys, unnamedKeys, function, functionObjects, solver.witnessAt(site, statement.origin) };

    FlowObjectSet classObjects = solver.valueAt(site, statement.classObject).objects;
    classObjects.forEach([&](FlowObjectID classObject) {
        defineMembers(context, classObject);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
