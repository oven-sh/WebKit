/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTFlowOrder.h"
#include <bit>

namespace JSC { namespace AOT { namespace Typed {

FlowWalkOperation walkOperationOf(FlowAccessKind kind)
{
    switch (kind) {
    case FlowAccessKind::Load:
        return FlowWalkOperation::Get;
    case FlowAccessKind::Store:
        return FlowWalkOperation::Set;
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        return FlowWalkOperation::DefineOwnProperty;
    case FlowAccessKind::Delete:
        return FlowWalkOperation::Delete;
    case FlowAccessKind::Test:
        return FlowWalkOperation::HasProperty;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowPropertyWalk walkOf(const FlowAccess& access)
{
    return {
        .operation = walkOperationOf(access.kind),
        .startsAtPrototype = access.flags.contains(FlowAccessFlag::StartsAtPrototype),
        .waiter = access.site,
        .isOwnOnly = access.flags.contains(FlowAccessFlag::IsOwnOnly),
        .builtinMembers = access.builtinMembers,
        .receivers = access.receiver,
        .startsAboveReceiver = !!access.thisValue,
    };
}

OperandUses receiverUsesOf(FlowAccessKind kind, const FlowKeySet& keys)
{
    OperandUses uses { OperandUse::Inspects };
    switch (kind) {
    case FlowAccessKind::Load:
    case FlowAccessKind::Test:
        if (keys.mayNameIndex())
            uses.add(OperandUse::ReadsContents);
        return uses;
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
        uses.add(OperandUse::StoresByKey);
        if (keys.mayNameIndex())
            uses.add(OperandUse::WritesContents);
        return uses;
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        uses.add(OperandUse::DefinesName);
        return uses;
    case FlowAccessKind::Delete:
        uses.add(OperandUse::DeletesByKey);
        return uses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void noteDeclaredOperandUses(FlowSolver& solver, FlowStatementSite site, ValueID origin)
{
    if (!origin)
        return;

    const Value& operation = solver.operationAt(site, origin);
    operation.forEachOperand([&](ValueID operand, uint32_t index) {
        // The edge into a part of the retaining object says for how long the operand is retained. As a direct use it would mean unknown memory.
        OperandUses uses = operation.operandUses(index);
        if (retainedInOf(operation.opcode, index) == RetainedIn::Operand)
            uses.remove(OperandUse::Retains);
        solver.noteDirectUses(solver.graph().nodeOf(site.instance, operand), uses);
    });
}

static void accessReceiver(const FlowAccessContext& context, FlowObjectID receiver)
{
    switch (context.access.kind) {
    case FlowAccessKind::Load:
        loadProperties(context, receiver);
        return;
    case FlowAccessKind::Store:
        storeProperties(context, receiver);
        return;
    case FlowAccessKind::Define:
        defineProperties(context, receiver);
        return;
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        defineAccessors(context, receiver);
        return;
    case FlowAccessKind::Delete:
        deleteProperties(context, receiver);
        return;
    case FlowAccessKind::Test:
        testProperties(context, receiver);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<FlowNewObjects> receiversBesideUnknownObject(FlowSolver& solver, const FlowNewObjects& receivers, FlowNodeID receiverNode)
{
    if (receivers.isEmpty())
        return std::nullopt;

    bool comeFromNode = receivers.cursor() && receiverNode;
    const FlowObjectSet& allReceivers = comeFromNode ? solver.valueOf(receiverNode).objects : receivers;
    if (!allReceivers.containsUnknownObject())
        return std::nullopt;

    // The receivers that have escaped are predictions. The unknown object, which is among the receivers, is none.
    const FlowObjectBitmap& escapedObjects = solver.escapedObjects();
    size_t maxEscapedReceivers = solver.build().limit(LimitID::MaxDispatchCases) + (escapedObjects.contains(unknownFlowObject) ? 1 : 0);
    // Nobody waits for a prediction, so running every rule again makes none.
    if (!solver.isCheckingFixpoint() && !allReceivers.hasMoreCommonObjectsThan(escapedObjects, maxEscapedReceivers))
        return std::nullopt;

    FlowObjectSet remaining = receivers;
    if (!comeFromNode || solver.nodeReadBy(receivers.cursor()) == receiverNode)
        remaining.subtract(escapedObjects);
    else
        remaining.subtractCommonObjectsOf(allReceivers, escapedObjects);
    if (receivers.containsUnknownObject())
        remaining.add(unknownFlowObject);
    return receivers.withObjects(WTF::move(remaining));
}

// Reading a member of a built-in object, or an export, is what records its use: what is kept in the image goes by the record of each member, not by that of its holder.
std::optional<FlowNewObjects> holdersBesideUnknownObject(FlowSolver& solver, const FlowNewObjects& holders, FlowNodeID holderNode)
{
    std::optional<FlowNewObjects> remaining = receiversBesideUnknownObject(solver, holders, holderNode);
    if (!remaining)
        return std::nullopt;
    holders.forEach([&](FlowObjectID holder) {
        if (!canEscapeAsReceiver(solver.objects().kindOf(holder)))
            remaining->add(holder);
    });
    return remaining;
}

static bool writesProperty(FlowAccessKind kind)
{
    switch (kind) {
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        return true;
    case FlowAccessKind::Load:
    case FlowAccessKind::Delete:
    case FlowAccessKind::Test:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool looksUpProperty(FlowAccessKind kind)
{
    switch (kind) {
    case FlowAccessKind::Load:
    case FlowAccessKind::Test:
        return true;
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
    case FlowAccessKind::Delete:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool standsForOneKey(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool storesUnderOneKey(const Program& program, const FlowAccess& access, const FlowKeySet& keys)
{
    bool isOperationOfProgram = access.site.statement.kind == FlowStatementKind::PropertyAccess || access.site.statement.kind == FlowStatementKind::ElementAccess;
    if (!isOperationOfProgram && access.completion == FlowStoreCompletion::MayLeaveStoreOut)
        return false;
    PropertyKeyID name = keys.onlyName();
    return !access.thisValue && name && standsForOneKey(program, name);
}

void accessProperties(FlowSolver& solver, const FlowAccess& access, const FlowNewObjects& receivers, const FlowKeySet& keys)
{
    if (keys.isEmpty() || receivers.isEmpty())
        return;

    FlowAccessContext context { solver, access, keys, solver.witnessAt(access.site, access.origin) };
    bool mayMissMember = writesProperty(access.kind) && !storesUnderOneKey(solver.program(), access, keys);
    std::optional<FlowNewObjects> remaining = receiversBesideUnknownObject(solver, receivers, access.receiver);
    solver.forEachMember(remaining ? *remaining : receivers, [&](FlowObjectID receiver) {
        if (access.site.instance && solver.isAboutToBeAbandoned(access.site))
            return;
        context.holderOfWrittenMember = { };
        context.storeWaitsForQuietQueues = false;
        accessReceiver(context, receiver);
        mayMissMember |= writesProperty(access.kind) && context.holderOfWrittenMember != receiver && !context.storeWaitsForQuietQueues;
        if (receiver != unknownFlowObject || !looksUpProperty(access.kind))
            return;
        for (PropertyKeyID name : keys.names)
            noteMemberRead(solver, access.site, unknownFlowObject, name);
        if (access.kind == FlowAccessKind::Load && keys.names.contains(solver.propertyState().keys.constructor))
            letConstructorsShareLocationsWithUnknownObject(solver);
    });
    if (mayMissMember)
        noteUncertainMemberStore(solver, access.site);
}

void accessProperties(FlowSolver& solver, const FlowAccess& access, FlowPrimitiveReceiver receiver, const FlowKeySet& keys)
{
    constexpr uint16_t primitivesWithoutWrapper = Primitive::UndefinedOrNull | Primitive::Absent;
    if (!(receiver.primitives & ~primitivesWithoutWrapper) || keys.isEmpty())
        return;

    FlowAccessContext context { solver, access, keys, solver.witnessAt(access.site, access.origin) };
    switch (access.kind) {
    case FlowAccessKind::Load:
        loadProperties(context, receiver);
        return;
    case FlowAccessKind::Store:
        storeProperties(context, receiver);
        noteUncertainMemberStore(solver, access.site);
        return;
    case FlowAccessKind::Test:
        testProperties(context, receiver);
        return;
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
    case FlowAccessKind::Delete:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Statement>
static FlowAccess accessOf(FlowSolver& solver, FlowStatementSite site, const Statement& statement)
{
    return {
        site,
        statement.origin,
        statement.kind,
        statement.use,
        statement.flags,
        solver.node(site, statement.receiver),
        solver.node(site, statement.value),
        solver.node(site, statement.thisValue),
        solver.node(site, statement.thrown),
    };
}

static void runAccess(FlowSolver& solver, const FlowAccess& access, const FlowKeySet& keys)
{
    RELEASE_ASSERT(access.receiver);

    FlowNewObjects receivers = solver.takeNewObjects(access.site, 0, access.receiver);
    bool hasNewPrimitives = solver.takeNewPrimitives(access.site, 0, access.receiver);
    if (receivers.isEmpty() && !hasNewPrimitives)
        return;

    if (access.origin)
        noteDeclaredOperandUses(solver, access.site, access.origin);
    else
        solver.noteDirectUses(access.receiver, receiverUsesOf(access.kind, keys));

    accessProperties(solver, access, receivers, keys);
    if (hasNewPrimitives)
        accessProperties(solver, access, FlowPrimitiveReceiver { solver.valueOf(access.receiver).primitives }, keys);
}

void runPropertyAccess(FlowSolver& solver, FlowStatementSite site, const FlowPropertyAccess& statement)
{
    FlowAccess access = accessOf(solver, site, statement);
    if (statement.kind == FlowAccessKind::Load) {
        switch (lookupStartOf(solver, site, statement.origin)) {
        case FlowLookupStart::Undecided:
            return;
        case FlowLookupStart::Prototype:
            access.flags.add(FlowAccessFlag::StartsAtPrototype);
            break;
        case FlowLookupStart::Object:
            break;
        }
    }
    if (statement.kind == FlowAccessKind::Define && definesUnobservedValue(solver, site, statement.origin))
        access.flags.add(FlowAccessFlag::StoresUnobservedValue);
    runAccess(solver, access, keySetOf(solver.program(), statement.key));
}

// The range of a loop counter grows step by step, and each other range is another key. A list has a part of its own for its first positions and one part for the rest,
// so the end of a range is rounded up: to the last index before a power of two, and past those positions to the last index there is.
static Range roundedIndexes(Range indexes, uint64_t maxPositionalParts)
{
    if (indexes.isEmpty() || indexes.isSingleValue())
        return indexes;
    uint64_t numberOfIndexes = std::bit_ceil(static_cast<uint64_t>(indexes.maximum) + 1);
    indexes.maximum = numberOfIndexes > maxPositionalParts ? arrayIndexRange.maximum : static_cast<int64_t>(numberOfIndexes) - 1;
    return indexes;
}

void runElementAccess(FlowSolver& solver, FlowStatementSite site, const FlowElementAccess& statement)
{
    FlowNodeID keyNode = solver.node(site, statement.key);
    RELEASE_ASSERT(keyNode);

    // A local wakes the statements that the summary lists for it.
    if (solver.summaryOf(site).isExternal(statement.key))
        solver.waitOn(keyNode, site);

    // Most of what arrives in the key names no other key: an object beside an object, a string beside any string. Only another key brings the receivers back.
    FlowKeySet keys = solver.values().keysOf(solver.valueOf(keyNode));
    keys.indexes = roundedIndexes(keys.indexes, solver.build().limit(LimitID::MaxPositionalParts));
    auto knownKeys = solver.propertyState().keysOfElementAccesses.add(site, keys);
    if (!knownKeys.isNewEntry && knownKeys.iterator->value.unite(keys))
        solver.revisit(site);

    if (solver.valueOf(keyNode).mayBeObject())
        runProtocols(solver, FlowProtocolRequest { site, statement.origin, keyNode, Protocols { Protocol::ToPrimitive }, { }, FlowNodeID(), solver.node(site, statement.thrown) });

    runAccess(solver, accessOf(solver, site, statement), keys);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
