/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FlowCopyContext {
    FlowSolver& solver;
    const FlowPropertyCopyRequest& request;
    Witness witness;
};

} // namespace

// A copy is a loop over sources and keys, so one operation may call an accessor many times.
static constexpr OptionSet<FlowAccessFlag> copyAccessFlags { FlowAccessFlag::IsInCycle };

struct FlowCopiedKeys {
    const FlowKeySet& keys;
    OptionSet<FlowAccessFlag> flags { copyAccessFlags };
    FlowStoreCompletion completion { FlowStoreCompletion::MayLeaveStoreOut };
};

static FlowAccessKind accessKindOf(FlowCopyOperation operation)
{
    switch (operation) {
    case FlowCopyOperation::CopyDataProperties:
        return FlowAccessKind::Define;
    case FlowCopyOperation::Assign:
        return FlowAccessKind::Store;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowKeySet keysOfUnnamedSource(const FlowGraph& graph, FlowNodeID node)
{
    FlowNodeKind kind = graph.kindOf(node);
    switch (kind) {
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        return keysHeldBy(*unnamedKeyPartOf(kind));
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
        return FlowKeySet::index(arrayIndexRange);
    case FlowNodeKind::Variable:
    case FlowNodeKind::UndeclaredPart:
        return FlowKeySet::everyKey();
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void put(const FlowCopyContext& context, FlowNodeID value, const FlowCopiedKeys& copied, const FlowNewObjects& targets)
{
    const FlowPropertyCopyRequest& request = context.request;
    FlowAccess access { request.site, request.origin, accessKindOf(request.operation), FlowReadUse::Value, copied.flags, request.targets, value, { }, request.thrown };
    access.completion = copied.completion;
    accessProperties(context.solver, access, targets, copied.keys);
}

static constexpr OptionSet<ObjectMutation> mutationsHidingProperties = reachMutations | OptionSet<ObjectMutation> { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty };

// The object has the field from its creation on, as an enumerable data property, whatever runs before the copy.
static bool alwaysHasEnumerableField(const FlowCopyContext& context, FlowObjectID object, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    if (solver.graph().kindOf(source.node) != FlowNodeKind::DeclaredPart || solver.objects().kindOf(object) != FlowObjectKind::Allocation)
        return false;
    solver.waitOnMutations(object, context.request.site, mutationsHidingProperties);
    if (solver.records().possibleMutations(object).containsAny(mutationsHidingProperties))
        return false;
    OptionSet<FlowLocationEvent> events = solver.records().recordOf(source.node).events;
    return events.contains(FlowLocationEvent::InitializedAtAllocation) && !events.containsAny(emptyingLocationEvents);
}

static bool isOfferedByOtherSource(const FlowCopyContext& context, FlowObjectID object, PropertyKeyID key)
{
    FlowSolver& solver = context.solver;
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    bool isOffered = false;
    solver.valueOf(context.request.sources).objects.forEach([&](FlowObjectID other) {
        if (other == object)
            return;
        // A source that gets the key after this run brings the object back.
        solver.waitOnProperty(other, key, context.request.site);
        isOffered |= other == unknownFlowObject || findOwnProperty(inputs, holderOf(solver.objects(), other), key).lookup != FlowOwnLookup::Absent;
    });
    return isOffered;
}

// One run stores once under a key that one source alone can have, and has stored when it completes where that source always has the key. The sources are as few as the arguments then.
static void putField(const FlowCopyContext& context, FlowObjectID object, const FlowPropertySource& source, const FlowNewObjects& targets)
{
    const FlowPropertyCopyRequest& request = context.request;
    FlowKeySet keys = keySetOf(context.solver.program(), source.key);
    FlowCopiedKeys copied { keys };
    if (!request.mayRunRepeatedly && request.copiesFromEverySource && !isOfferedByOtherSource(context, object, source.key)) {
        copied.flags = { };
        if (alwaysHasEnumerableField(context, object, source))
            copied.completion = FlowStoreCompletion::HasStored;
    }
    put(context, source.node, copied, targets);
}

static void copyLoadedValue(const FlowCopyContext& context, FlowObjectID object, const FlowPropertySource& source, const FlowTemporaryKey& temporary, const FlowNewObjects& targets)
{
    FlowSolver& solver = context.solver;
    const FlowPropertyCopyRequest& request = context.request;
    FlowKeySet keys = source.key ? keySetOf(solver.program(), source.key) : FlowKeySet::everyKey();
    FlowNodeID result = solver.temporaryFor(temporary);
    FlowAccess load { request.site, request.origin, FlowAccessKind::Load, FlowReadUse::Value, copyAccessFlags, request.sources, result, { }, request.thrown };
    loadFromSource(FlowAccessContext { solver, load, keys, context.witness }, object, source);
    put(context, result, { keys }, targets);
}

static void copySource(const FlowCopyContext& context, FlowObjectID object, const FlowPropertySource& source, const FlowNewObjects& targets)
{
    FlowSolver& solver = context.solver;
    const Program& program = solver.program();
    const FlowPropertyCopyRequest& request = context.request;
    if (source.key && request.excludedKeys.mayName(source.key, program))
        return;

    switch (source.kind) {
    case FlowPropertySourceKind::Value:
        // A read creates the node of an unnamed key, so only a node that holds a value makes this a store under keys nobody can name.
        if (!source.key && solver.valueOf(source.node).isEmpty()) {
            solver.waitOn(source.node, request.site, FlowWakeUp::Revisit);
            return;
        }
        solver.noteLocationEvent(source.node, valueReadEvents);
        if (source.key)
            putField(context, object, source, targets);
        else
            put(context, source.node, { keysOfUnnamedSource(solver.graph(), source.node) }, targets);
        return;
    case FlowPropertySourceKind::Getter:
        copyLoadedValue(context, object, source, propertyTemporaryKey(request.site, FlowPropertyRole::CopiedGetterResult, source.node.index()), targets);
        return;
    case FlowPropertySourceKind::BuiltinMember:
    case FlowPropertySourceKind::BuiltinGetter:
        copyLoadedValue(context, object, source, propertyTemporaryKey(request.site, FlowPropertyRole::CopiedUnknownValue, 1 + source.builtin.index()), targets);
        return;
    case FlowPropertySourceKind::Unknown:
        copyLoadedValue(context, object, source, propertyTemporaryKey(request.site, FlowPropertyRole::CopiedUnknownValue), targets);
        return;
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void copyFrom(const FlowCopyContext& context, FlowObjectID object, const FlowNewObjects& targets)
{
    if (targets.isEmpty())
        return;

    FlowSolver& solver = context.solver;
    solver.noteUse(object, FlowObjectUse::Enumerated);
    FlowPropertyWalk walk { .operation = FlowWalkOperation::CopyDataProperties, .waiter = context.request.site, .isEnumerableOnly = true };
    forEachPropertySource(solver, object, FlowKeySet::everyKey(), walk, [&](const FlowPropertySource& source) {
        copySource(context, object, source, targets);
    });
}

void copyProperties(FlowSolver& solver, const FlowPropertyCopyRequest& request)
{
    if (!request.sources || !request.targets)
        return;

    // A cursor belongs to the other node (the sources these targets have received), so requests of one site that share a node miss no pair of objects.
    FlowNewObjects newSources = solver.takeNewObjects(propertyTemporaryKey(request.site, FlowPropertyRole::CopySources, request.targets.index()), request.sources);
    FlowNewObjects newTargets = solver.takeNewObjects(propertyTemporaryKey(request.site, FlowPropertyRole::CopyTargets, request.sources.index()), request.targets);
    FlowNewObjects allTargets = solver.valueOf(request.targets).objects;
    FlowObjectSet oldSources = solver.valueOf(request.sources).objects;
    oldSources.subtract(newSources);

    FlowCopyContext context { solver, request, solver.witnessAt(request.site, request.origin) };
    solver.forEachMember(newSources, [&](FlowObjectID source) {
        copyFrom(context, source, allTargets);
    });
    if (!newTargets.isEmpty()) {
        // What such a copy waits on brings the source back as a new one, for all targets.
        solver.forEachMember(newSources.withObjects(WTF::move(oldSources)), [&](FlowObjectID source) {
            copyFrom(context, source, newTargets);
        });
    }

    if (solver.valueOf(request.sources).primitives & Primitive::AnyString) {
        FlowNodeID characters = solver.temporaryFor(propertyTemporaryKey(request.site, FlowPropertyRole::CopiedCharacters));
        solver.joinPrimitives(characters, Primitive::String);
        put(context, characters, { FlowKeySet::index(arrayIndexRange) }, allTargets);
    }
}

static bool hasExactUnion(Range first, Range second)
{
    return first.isEmpty() || second.isEmpty() || (first.minimum <= second.maximum + 1 && second.minimum <= first.maximum + 1);
}

// Answers nothing while a computed key has not arrived: until then nothing says which key the copy leaves out, and what it has copied stays copied.
std::optional<FlowKeySet> certainlyExcludedKeys(FlowSolver& solver, FlowStatementSite site, const FlowPropertyCopy& copy)
{
    const Program& program = solver.program();
    FlowKeySet excluded;
    for (PropertyKeyID key : solver.operationAt(site, copy.origin).excludedKeys()) {
        std::optional<uint32_t> index = arrayIndexOf(program, key);
        if (!index) {
            excluded.names.add(key);
            continue;
        }
        // A key set has one range of indexes, and an index in the gap between two excluded ones is copied.
        FlowKeySet keys = FlowKeySet::index(Range::one(*index));
        if (hasExactUnion(excluded.indexes, keys.indexes))
            excluded.unite(keys);
    }

    for (FlowLocalID local : solver.summaryOf(site).statements.list(copy.excludedKeys)) {
        FlowNodeID node = solver.node(site, local);
        if (!node)
            continue;
        solver.waitOn(node, site, FlowWakeUp::Revisit);
        FlowKeySet keys = solver.values().keysOf(solver.valueOf(node));
        if (keys.isEmpty())
            return std::nullopt;
        if (PropertyKeyID name = keys.onlyName())
            excluded.names.add(name);
    }
    return excluded;
}

void runPropertyCopy(FlowSolver& solver, FlowStatementSite site, const FlowPropertyCopy& copy)
{
    noteDeclaredOperandUses(solver, site, copy.origin);
    std::optional<FlowKeySet> excludedKeys = certainlyExcludedKeys(solver, site, copy);
    if (!excludedKeys)
        return;
    copyProperties(solver, FlowPropertyCopyRequest { site, copy.origin, FlowCopyOperation::CopyDataProperties, solver.node(site, copy.source), solver.node(site, copy.target), solver.node(site, copy.thrown), WTF::move(*excludedKeys) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
