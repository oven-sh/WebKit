/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowConstruction.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowThrown.h"
#include "AOTLayout.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class FieldPresence : uint8_t {
    MayBeMissing,
    Present,
};

struct FieldAccessContext {
    FlowSolver& solver;
    FlowPartAccess part;
    FieldReference field;
    PropertyKeyID key;
    Witness witness;
};

} // anonymous namespace

static void accessUnknown(const FieldAccessContext& context)
{
    FlowSolver& solver = context.solver;
    switch (context.part.kind) {
    case FlowAccessKind::Load:
        solver.joinUnknown(context.part.value, context.witness);
        return;
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
        handToUnknownRecipient(solver, context.part.value, context.witness);
        return;
    case FlowAccessKind::Test:
        return;
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
    case FlowAccessKind::Delete:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeIntoLocation(const FieldAccessContext& context, FlowObjectID object, FlowNodeID location, FlowStoreNaming naming)
{
    FlowSolver& solver = context.solver;
    FlowPartAccess part = context.part;
    part.naming = naming;
    accessPart(solver, part, location);

    bool isDeclaredField = solver.graph().kindOf(location) == FlowNodeKind::DeclaredPart || enforcedTypeOf(solver.graph(), location);
    solver.noteMutation(object, isDeclaredField ? ObjectMutation::StoreAfterCreation : ObjectMutation::AddProperty, context.witness);
}

static void accessUnknownReceiver(const FieldAccessContext& context)
{
    accessUnknown(context);
    if (!isWrite(context.part.kind))
        return;

    FlowSolver& solver = context.solver;
    if (context.key) {
        storeIntoLocation(context, unknownFlowObject, solver.ensurePropertyValue(unknownFlowObject, context.key), FlowStoreNaming::Unknown);
        return;
    }
    accessPart(solver, context.part, solver.graph().undeclaredPart(unknownFlowObject));
    noteContentsStore(solver, unknownFlowObject, context.witness);
}

static FieldPresence accessField(const FieldAccessContext& context, FlowObjectID object)
{
    if (object == unknownFlowObject) {
        accessUnknownReceiver(context);
        return FieldPresence::MayBeMissing;
    }

    FlowSolver& solver = context.solver;
    FlowFieldLocation found = findFieldLocation(solver.graph(), object, context.field.layout, context.field.field);
    FlowNodeID location = found.lookup == FlowFieldLookup::Found ? found.node : FlowNodeID();
    if (!location && context.key)
        location = solver.ensurePropertyValue(object, context.key);

    if (!location) {
        accessUnknown(context);
        return FieldPresence::MayBeMissing;
    }

    // The operation names a field of a layout. A property under the key of the field is none, whether this access creates its location or finds it.
    bool isDeclaredField = solver.graph().kindOf(location) == FlowNodeKind::DeclaredPart;
    if (isWrite(context.part.kind))
        storeIntoLocation(context, object, location, isDeclaredField ? FlowStoreNaming::NamesLocation : FlowStoreNaming::Unknown);
    else
        accessPart(solver, context.part, location);

    if (context.part.kind == FlowAccessKind::Define || isDeclaredField)
        return FieldPresence::Present;
    return FieldPresence::MayBeMissing;
}

void runFieldAccess(FlowSolver& solver, FlowStatementSite site, const FlowFieldAccess& access)
{
    RELEASE_ASSERT(access.kind == FlowAccessKind::Load || access.kind == FlowAccessKind::Store || access.kind == FlowAccessKind::Define || access.kind == FlowAccessKind::Test);

    bool isWriteAccess = isWrite(access.kind);
    FlowNodeID receiver = solver.node(site, access.receiver);
    FlowNodeID value = solver.node(site, access.value);
    if (access.kind == FlowAccessKind::Define && definesUnobservedValue(solver, site, access.origin))
        value = { };
    FieldReference reference { access.layout, access.field };
    const Field& field = fieldAt(solver.program(), reference);

    solver.noteDirectUses(receiver, OperandUse::Inspects);

    FieldAccessContext context {
        solver,
        { site, access.origin, access.kind, access.use, isWriteAccess ? access.value : FlowLocalID(), value },
        reference,
        field.key,
        solver.witnessAt(site, access.origin),
    };

    bool mayBeMissing = false;
    solver.takeNewObjects(site, 0, receiver).forEach([&](FlowObjectID object) {
        if (accessField(context, object) == FieldPresence::MayBeMissing)
            mayBeMissing = true;
    });

    if (!field.flags.contains(FieldFlags::IsPrivate))
        return;

    bool receiverMayBePrimitive = receiver && solver.valueOf(receiver).primitives;
    bool mayThrow = receiverMayBePrimitive || (mayBeMissing && access.kind != FlowAccessKind::Test);
    if (mayThrow)
        throwRuntimeError(solver, solver.node(site, access.thrown), context.witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
