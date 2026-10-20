/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowPropertyOperations.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SlotAccessContext {
    FlowSolver& solver;
    FlowPartAccess part;
    FlowNodeID receiver;
    Witness witness;

    bool isLoad() const { return part.kind == FlowAccessKind::Load; }
};

} // anonymous namespace

static bool hasInternalSlots(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return true;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Range elementIndexesOf(const FlowSlotAccess& access)
{
    return access.position ? Range::one(*access.position) : arrayIndexRange;
}

static bool selectsElementSlot(const BuiltinTable& builtins, const FlowSlotAccess& access)
{
    std::optional<ContentsSlot> contentsSlot = access.slot ? builtins.at(access.slot).contentsSlot : access.contentsSlot;
    return contentsSlot == ContentsSlot::Element;
}

static void accessUnknown(const SlotAccessContext& context)
{
    FlowSolver& solver = context.solver;
    if (context.isLoad())
        solver.joinUnknown(context.part.value, context.witness);
    else
        handToUnknownRecipient(solver, context.part.value, context.witness);
}

// The statement does not say whether it is in a cycle, so each of its stores counts as repeated.
static void accessSlotPart(const SlotAccessContext& context, FlowNodeID part)
{
    if (!part)
        return;
    accessPart(context.solver, context.part, part);
    if (!context.isLoad())
        context.solver.noteLocationEvent(part, FlowLocationEvent::StoredByManyOperations);
}

static void accessIndexedProperties(const SlotAccessContext& context, FlowObjectID object, Range indexes)
{
    const FlowPartAccess& part = context.part;
    FlowAccessKind kind = context.isLoad() ? FlowAccessKind::Load : FlowAccessKind::Define;
    FlowAccess access { part.site, part.origin, kind, FlowReadUse::Value, FlowAccessFlag::IsInCycle, context.receiver, part.value, { }, { } };
    accessProperties(context.solver, access, FlowObjectSet(object), FlowKeySet::index(indexes));
}

static void accessUnknownReceiver(const SlotAccessContext& context, const FlowSlotAccess& access)
{
    accessUnknown(context);
    if (context.isLoad())
        return;

    FlowSolver& solver = context.solver;
    if (selectsElementSlot(solver.builtins(), access)) {
        accessIndexedProperties(context, unknownFlowObject, elementIndexesOf(access));
        return;
    }
    accessSlotPart(context, solver.graph().undeclaredPart(unknownFlowObject));
}

// An instance of a subclass has no node for what the constructor of the built-in class keeps. Only a class with indexed elements keeps them where a property under an index is.
static bool keepsElementsOutsideProperties(const FlowSolver& solver, FlowObjectID object)
{
    const FlowObjectTable& objects = solver.objects();
    if (!objects.isMadeByBuiltinConstructor(object))
        return false;
    return !indexedElementSlotOf(solver.builtins(), objects.at(objects.at(object).site).description.builtinClass);
}

static void accessElements(const SlotAccessContext& context, FlowObjectID object, Range indexes)
{
    FlowSolver& solver = context.solver;

    if (std::optional<uint32_t> filledElements = numberOfFilledElements(solver, object)) {
        forEachElementPart(solver.graph(), object, indexes, [&](FlowNodeID part) {
            accessSlotPart(context, part);
        });
        if (context.isLoad() && indexes.maximum >= *filledElements)
            solver.joinPrimitives(context.part.value, Primitive::Undefined);
        return;
    }

    if (FlowNodeID contents = contentsNode(solver.graph(), object, ContentsSlot::Element)) {
        accessSlotPart(context, contents);
        return;
    }
    if (keepsElementsOutsideProperties(solver, object)) {
        accessUnknown(context);
        return;
    }
    accessIndexedProperties(context, object, indexes);
}

static void accessContentsSlot(const SlotAccessContext& context, FlowObjectID object, ContentsSlot contentsSlot)
{
    FlowNodeID contents = contentsNode(context.solver.graph(), object, contentsSlot);
    if (!contents) {
        accessUnknown(context);
        return;
    }
    accessSlotPart(context, contents);
}

static void accessSlot(const SlotAccessContext& context, FlowObjectID object, BuiltinSlotID slot)
{
    FlowSolver& solver = context.solver;
    if (slot == indexedElementSlotOf(solver.builtins(), solver.objects().at(object).builtinClass)) {
        accessElements(context, object, arrayIndexRange);
        return;
    }

    FlowNodeID node = solver.graph().slot(object, slot);
    if (!node) {
        accessUnknown(context);
        return;
    }
    accessSlotPart(context, node);
    accessSlotPart(context, solver.graph().undeclaredPart(object));
}

static void accessInternalFields(const SlotAccessContext& context, FlowObjectID object)
{
    FlowSolver& solver = context.solver;
    FlowObject record = solver.objects().at(object);
    if (!hasInternalSlots(record.kind) || (!record.numberOfDeclaredParts && !record.hasUndeclaredPart)) {
        accessUnknown(context);
        return;
    }

    for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index)
        accessSlotPart(context, solver.graph().declaredPart(object, index));
    accessSlotPart(context, solver.graph().undeclaredPart(object));
    if (context.isLoad())
        solver.join(context.part.value, solver.values().unknownValue().withoutObjects());
}

static void accessSelectedSlot(const SlotAccessContext& context, const FlowSlotAccess& access, FlowObjectID object)
{
    if (!context.isLoad())
        noteContentsStore(context.solver, object, context.witness);
    if (access.mayLeaveHole)
        context.solver.noteEvent(object, FlowObjectEvent::HoleMade, context.witness);

    if (object == unknownFlowObject) {
        accessUnknownReceiver(context, access);
        return;
    }
    if (access.internalFieldIndex) {
        accessInternalFields(context, object);
        return;
    }
    if (access.slot) {
        accessSlot(context, object, access.slot);
        return;
    }

    switch (*access.contentsSlot) {
    case ContentsSlot::Element:
        accessElements(context, object, elementIndexesOf(access));
        return;
    case ContentsSlot::Key:
    case ContentsSlot::Value:
        accessContentsSlot(context, object, *access.contentsSlot);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runSlotAccess(FlowSolver& solver, FlowStatementSite site, const FlowSlotAccess& access)
{
    RELEASE_ASSERT(access.kind == FlowAccessKind::Load || access.kind == FlowAccessKind::Store);
    RELEASE_ASSERT(access.contentsSlot.has_value() + !!access.slot + access.internalFieldIndex.has_value() == 1);

    bool isLoad = access.kind == FlowAccessKind::Load;
    FlowNodeID receiver = solver.node(site, access.receiver);
    FlowNodeID value = solver.node(site, access.value);

    solver.noteDirectUses(receiver, { OperandUse::Inspects, isLoad ? OperandUse::ReadsContents : OperandUse::WritesContents });

    SlotAccessContext context {
        solver,
        { site, access.origin, access.kind, FlowReadUse::Value, isLoad ? FlowLocalID() : access.value, value },
        receiver,
        solver.witnessAt(site, access.origin),
    };
    solver.takeNewObjects(site, 0, receiver).forEach([&](FlowObjectID object) {
        accessSelectedSlot(context, access, object);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
