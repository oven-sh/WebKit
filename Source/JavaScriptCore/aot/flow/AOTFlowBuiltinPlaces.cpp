/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTFlowParts.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned argumentIndexBits = 8;
static constexpr unsigned packedOperandBits = 10;
static constexpr unsigned packedIDBits = 16;

static_assert(BuiltinOperand::maxArgumentIndex < 1u << argumentIndexBits);
static_assert(static_cast<uint32_t>(BuiltinOperand::Kind::ArgumentsFrom) < 1u << (packedOperandBits - argumentIndexBits));
static_assert(static_cast<uint32_t>(BuiltinPlaceKind::NativeCell) < 1u << (32 - packedOperandBits - packedIDBits));
static_assert(BuiltinSlotID::absentIndex < 1u << packedIDBits);
static_assert(BuiltinTypeID::absentIndex < 1u << packedIDBits);

uint32_t packedOperand(BuiltinOperand operand)
{
    uint32_t argumentIndex = operand.isReceiver() ? 0 : operand.argumentIndex();
    return static_cast<uint32_t>(operand.kind()) << argumentIndexBits | argumentIndex;
}

static uint32_t packedIDOf(const BuiltinPlace& place)
{
    switch (place.kind) {
    case BuiltinPlaceKind::Slot:
    case BuiltinPlaceKind::ResultSlot:
        return place.slot.index();
    case BuiltinPlaceKind::ResultField:
        return place.fieldIndex;
    case BuiltinPlaceKind::NewValue:
        return place.newValueType.index();
    case BuiltinPlaceKind::IteratedValues:
        return place.isSettled;
    case BuiltinPlaceKind::Member:
        return place.firstMemberStep;
    case BuiltinPlaceKind::NativeCell:
        return place.nativeCell.index();
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::Prototype:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t packedPlace(const BuiltinPlace& place)
{
    uint32_t operand = place.operand ? packedOperand(*place.operand) : 0;
    return static_cast<uint32_t>(place.kind) << (packedOperandBits + packedIDBits) | operand << packedIDBits | packedIDOf(place);
}

static FlowNodeID hubOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowCallTemporaryRole role, const BuiltinPlace& place)
{
    return solver.temporaryFor(callTemporaryKey(overloadCall.call.request.site, role, packedPlace(place)));
}

static bool isSink(const FlowGraph& graph, FlowNodeID node)
{
    return node == graph.nativeCode() || node == graph.unknownJavaScript();
}

static FlowNodeID resultNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    if (FlowNodeID result = overloadCall.call.request.operands.result)
        return result;
    return hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceTarget, place);
}

static FlowNodeID resultFieldOf(const FlowSolver& solver, FlowObjectID object, uint32_t fieldIndex)
{
    const FlowObjectTable& objects = solver.objects();
    if (!objects.hasLayout(object) || fieldIndex >= objects.at(object).numberOfDeclaredParts)
        return { };
    return solver.graph().field(object, fieldIndex);
}

static bool isIndexedElementSlot(const FlowSolver& solver, FlowObjectID object, BuiltinSlotID slot)
{
    return slot == indexedElementSlotOf(solver.builtins(), solver.objects().at(object).builtinClass);
}

static bool mayReturnOtherObjects(const FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallOperands& operands = call.request.operands;
    if (call.mode == BuiltinCallMode::Construct && (operands.newTarget || operands.newTargetObject))
        return true;

    const BuiltinType& type = solver.builtins().at(overloadCall.overload.result.type);
    switch (type.freshness) {
    case BuiltinFreshness::No:
    case BuiltinFreshness::UnlessSpecies:
        return true;
    case BuiltinFreshness::Unaliased:
    case BuiltinFreshness::Yes:
        break;
    }
    return std::ranges::any_of(type.parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::InstanceOfReceiverClass || part.kind == BuiltinTypePartKind::TypedArrayOfReceiverElementType;
    });
}

static FlowNewObjects takeOtherResultObjects(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowCallTemporaryRole cursorRole, const BuiltinPlace& place, const FlowObjectSet& allocatedObjects)
{
    const FlowCallRequest& request = overloadCall.call.request;
    FlowNewObjects objects = solver.takeNewObjects(callTemporaryKey(request.site, cursorRole, packedPlace(place)), request.operands.result);
    FlowObjectSet otherObjects = objects;
    otherObjects.subtract(allocatedObjects);
    return objects.withObjects(WTF::move(otherObjects));
}

static void connectSlotSource(FlowSolver& solver, FlowObjectID object, BuiltinSlotID slot, FlowNodeID hub, const Witness& witness)
{
    const FlowGraph& graph = solver.graph();
    FlowNodeID node = graph.slot(object, slot);
    if (!node) {
        solver.joinUnknown(hub, witness);
        return;
    }

    auto read = [&](FlowNodeID part) {
        solver.noteLocationEvent(part, FlowLocationEvent::ReadAsValue);
        solver.connect(part, hub);
    };
    if (isIndexedElementSlot(solver, object, slot))
        forEachElementPart(graph, object, arrayIndexRange, read);
    else
        read(node);
    if (FlowNodeID undeclaredPart = graph.undeclaredPart(object))
        read(undeclaredPart);
}

static void storeIntoUnknownObject(FlowSolver& solver, BuiltinSlotID slot, FlowNodeID from, const Witness& witness)
{
    bool storesElement = solver.builtins().at(slot).contentsSlot == ContentsSlot::Element;
    FlowNodeID location = storesElement
        ? solver.ensureOpenPart(unknownFlowObject, FlowOpenPart::IndexedProperties)
        : solver.graph().undeclaredPart(unknownFlowObject);
    solver.connect(from, solver.graph().nativeCode(), { }, witness);
    solver.connect(from, location, { }, witness);
}

static void storeIntoSlotOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowNewObjects& holders, BuiltinSlotID slot, FlowNodeID from)
{
    Witness witness = witnessOf(solver, overloadCall.call);
    solver.forEachMember(holders, [&](FlowObjectID holder) {
        if (holder == unknownFlowObject)
            storeIntoUnknownObject(solver, slot, from, witness);
        else
            connectIntoSlot(solver, overloadCall, from, holder, slot);
        noteContentsStore(solver, holder, witness);
    });
}

static FlowNodeID operandSourceNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    const FlowBuiltinCall& call = overloadCall.call;
    BuiltinOperand operand = *place.operand;
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return operandNodeOf(call, operand);
    case BuiltinOperand::Kind::ArgumentsFrom: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        forEachOperandNode(call, operand, [&](FlowNodeID node) {
            solver.connect(node, hub);
        });
        return hub;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void connectIteratedValueSources(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    const FlowCallRequest& request = overloadCall.call.request;
    if (place.isSettled) {
        FlowNodeID values = sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::IteratedValues, .operand = place.operand });
        FlowNodeID rejected = rejectionNodeOf(solver, overloadCall);
        awaitValuesOf(solver, { request.site, request.origin, hub, rejected ? rejected : request.operands.thrown }, values);
        return;
    }
    forEachOperandNode(overloadCall.call, *place.operand, [&](FlowNodeID node) {
        connectIteratedValues(solver, { request.site, request.origin, IteratorKind::Sync, true, operandCodeThrownNodeOf(solver, overloadCall) }, node, hub);
    });
}

static void connectPrototypeSources(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    FlowStatementSite site = overloadCall.call.request.site;
    FlowNodeID inheritors = operandSourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Operand, .operand = place.operand });
    if (!inheritors)
        return;

    FlowTemporaryKey cursor = callTemporaryKey(site, FlowCallTemporaryRole::PlaceSourceObjects, packedPlace(place));
    loadPrototypes(solver, site, solver.takeNewObjects(cursor, inheritors), hub, witnessOf(solver, overloadCall.call));

    const BuiltinParameter* parameter = solver.builtins().parameterFor(overloadCall.overload, *place.operand);
    if (parameter && parameter->conversion == BuiltinConversion::ToObject)
        loadPrototypes(solver, FlowPrimitiveReceiver { solver.valueOf(inheritors).primitives }, hub);
}

static void connectResultPartSources(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    const FlowGraph& graph = solver.graph();
    Witness witness = witnessOf(solver, overloadCall.call);

    FlowObjectSet allocatedObjects;
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        allocatedObjects.add(object);
        if (object == unknownFlowObject) {
            solver.joinUnknown(hub, witness);
            return;
        }
        if (place.kind == BuiltinPlaceKind::ResultField) {
            solver.connect(resultFieldOf(solver, object, place.fieldIndex), hub);
            return;
        }
        if (graph.slot(object, place.slot))
            connectSlotSource(solver, object, place.slot, hub, witness);
    });
    if (place.kind != BuiltinPlaceKind::ResultSlot || !mayReturnOtherObjects(solver, overloadCall))
        return;

    FlowNodeID result = overloadCall.call.request.operands.result;
    if (!result || isSink(graph, result)) {
        solver.joinUnknown(hub, witness);
        return;
    }
    takeOtherResultObjects(solver, overloadCall, FlowCallTemporaryRole::PlaceSourceObjects, place, allocatedObjects).forEach([&](FlowObjectID object) {
        connectSlotSource(solver, object, place.slot, hub, witness);
    });
}

static void connectResultSlotTargets(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    const FlowCallRequest& request = overloadCall.call.request;
    const FlowGraph& graph = solver.graph();

    FlowObjectSet allocatedObjects;
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        allocatedObjects.add(object);
        if (object == unknownFlowObject || graph.slot(object, place.slot))
            connectIntoSlot(solver, overloadCall, hub, object, place.slot);
    });
    if (!mayReturnOtherObjects(solver, overloadCall)) {
        if (allocatedObjects.isEmpty())
            solver.connect(hub, graph.nativeCode(), { }, witnessOf(solver, overloadCall.call));
        return;
    }

    FlowNodeID result = request.operands.result;
    if (!result || isSink(graph, result)) {
        solver.connect(hub, graph.nativeCode(), { }, witnessOf(solver, overloadCall.call));
        return;
    }

    storeIntoSlotOf(solver, overloadCall, takeOtherResultObjects(solver, overloadCall, FlowCallTemporaryRole::PlaceTargetObjects, place, allocatedObjects), place.slot, hub);
}

static void connectResultFieldTargets(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    Witness witness = witnessOf(solver, overloadCall.call);
    bool hasField = false;
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        FlowNodeID field = object == unknownFlowObject ? solver.graph().nativeCode() : resultFieldOf(solver, object, place.fieldIndex);
        solver.connect(hub, field, { }, witness);
        hasField |= !!field;
    });
    if (!hasField)
        solver.connect(hub, solver.graph().nativeCode(), { }, witness);
}

// One node for the whole program. What is stored there lives on, and only an entry that names the cell takes it out again.
FlowNodeID nativeCellNode(FlowSolver& solver, BuiltinName cell)
{
    FlowNodeID node = solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::NativeCell, cell.index()));
    solver.noteDirectUses(node, OperandUse::Retains);
    return node;
}

static FlowNodeID nativeCellNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinName cell)
{
    FlowNodeID node = nativeCellNode(solver, cell);

    const BuiltinTable& builtins = solver.builtins();
    if (BuiltinID holder = builtins.at(overloadCall.call.builtin).holder) {
        if (BuiltinClassID receiverClass = builtins.at(holder).instanceClass)
            noteNativeCellReachedThrough(solver, receiverClass, node);
    }
    for (const BuiltinTypePart& part : builtins.at(overloadCall.overload.result.type).parts) {
        if (part.kind == BuiltinTypePartKind::Instance && part.builtinClass)
            noteNativeCellReachedThrough(solver, part.builtinClass, node);
    }
    return node;
}

FlowNodeID sourceNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    switch (place.kind) {
    case BuiltinPlaceKind::None:
        return { };
    case BuiltinPlaceKind::NativeCell:
        return nativeCellNodeOf(solver, overloadCall, place.nativeCell);
    case BuiltinPlaceKind::Member: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        connectMemberSources(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::Operand:
        return operandSourceNodeOf(solver, overloadCall, place);
    case BuiltinPlaceKind::Result: {
        FlowNodeID result = resultNodeOf(solver, overloadCall, place);
        return isSink(solver.graph(), result) ? unknownValueNode(solver) : result;
    }
    case BuiltinPlaceKind::NativeMemory:
        return unknownValueNode(solver);
    case BuiltinPlaceKind::Slot: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        connectSlotSources(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::IteratedValues: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        connectIteratedValueSources(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::OwnValues: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        connectOwnValueSources(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::Prototype: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        connectPrototypeSources(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        connectResultPartSources(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::NewValue: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceSource, place);
        FlowBuiltinTypePath path;
        joinValueOfType(solver, overloadCall, { place.newValueType, hub, FlowSiteOrigin::BuiltinElement, FlowOperandParts::AreConnected }, path);
        return hub;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowNodeID targetNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    switch (place.kind) {
    case BuiltinPlaceKind::Result:
        return resultNodeOf(solver, overloadCall, place);
    case BuiltinPlaceKind::Slot: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceTarget, place);
        connectSlotTargets(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::ResultSlot: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceTarget, place);
        connectResultSlotTargets(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::ResultField: {
        FlowNodeID hub = hubOf(solver, overloadCall, FlowCallTemporaryRole::PlaceTarget, place);
        connectResultFieldTargets(solver, overloadCall, place, hub);
        return hub;
    }
    case BuiltinPlaceKind::NativeMemory:
        return solver.graph().nativeCode();
    case BuiltinPlaceKind::NativeCell:
        return nativeCellNodeOf(solver, overloadCall, place.nativeCell);
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A cursor reads one node, so the rest arguments are read through the node that collects them.
static FlowNodeID holdersNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    return operandSourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Operand, .operand = place.operand });
}

void connectSlotSources(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    const FlowBuiltinCall& call = overloadCall.call;
    FlowTemporaryKey cursor = callTemporaryKey(call.request.site, FlowCallTemporaryRole::PlaceSourceObjects, packedPlace(place));
    Witness witness = witnessOf(solver, call);
    solver.takeNewObjects(cursor, holdersNodeOf(solver, overloadCall, place)).forEach([&](FlowObjectID object) {
        connectSlotSource(solver, object, place.slot, hub, witness);
    });
}

void connectSlotTargets(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    FlowTemporaryKey cursor = callTemporaryKey(overloadCall.call.request.site, FlowCallTemporaryRole::PlaceTargetObjects, packedPlace(place));
    storeIntoSlotOf(solver, overloadCall, solver.takeNewObjects(cursor, holdersNodeOf(solver, overloadCall, place)), place.slot, hub);
}

void connectOwnValueSources(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    const FlowCallRequest& request = overloadCall.call.request;
    FlowNodeID holders = holdersNodeOf(solver, overloadCall, place);
    if (!holders)
        return;
    if (declaredBehaviorOf(overloadCall.overload, *place.operand).uses.contains(OperandUse::AppliesTransitively))
        holders = transitiveContentsOf(solver, request.site, holders);

    FlowTemporaryKey cursor = callTemporaryKey(request.site, FlowCallTemporaryRole::PlaceSourceObjects, packedPlace(place));
    runOwnValues(solver, { request.site, request.origin, holders, { }, { }, hub, request.operands.thrown }, solver.takeNewObjects(cursor, holders));
    // The own values of a string are its characters.
    if (solver.valueOf(holders).primitives & Primitive::AnyString)
        solver.joinPrimitives(hub, Primitive::String);
}

void connectMemberSources(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place, FlowNodeID hub)
{
    const FlowCallRequest& request = overloadCall.call.request;
    uint32_t operand = packedOperand(*place.operand) << packedIDBits;
    FlowNodeID holders = holdersNodeOf(solver, overloadCall, place);
    for (size_t step = 0; holders && step < place.memberPath.size(); ++step) {
        uint32_t index = operand | (place.firstMemberStep + step);
        bool isLastStep = step + 1 == place.memberPath.size();
        FlowNodeID values = isLastStep ? hub : solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::MemberPathValues, index));
        FlowNewObjects objects = solver.takeNewObjects(callTemporaryKey(request.site, FlowCallTemporaryRole::MemberPathHolders, index), holders);
        if (place.memberPath[step]) {
            FlowAccess read { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load, .receiver = holders, .value = values, .thrown = request.operands.thrown };
            accessProperties(solver, read, objects, keysOfNames(solver.program(), place.memberPath.subspan(step, 1)));
        } else
            runOwnValues(solver, { request.site, request.origin, holders, { }, { }, values, request.operands.thrown }, objects);
        holders = values;
    }
}

void connectIntoSlot(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowNodeID from, FlowObjectID object, BuiltinSlotID slot)
{
    const FlowCallRequest& request = overloadCall.call.request;
    const FlowGraph& graph = solver.graph();
    Witness witness = witnessOf(solver, overloadCall.call);

    FlowNodeID node = graph.slot(object, slot);
    if (!node) {
        solver.connect(from, graph.nativeCode(), { }, witness);
        return;
    }
    if (holdsResolvedValues(solver.builtins(), slot)) {
        resolvePromiseWith(solver, request.site, request.origin, object, from);
        solver.noteLocationEvent(node, repeatedUnnamedStoreEvents);
        return;
    }
    if (slot == BuiltinSlots::PromiseReason) {
        rejectPromiseWith(solver, object, from, witness);
        solver.noteLocationEvent(node, repeatedUnnamedStoreEvents);
        return;
    }

    auto store = [&](FlowNodeID part) {
        solver.connect(from, part, { }, witness);
        solver.noteLocationEvent(part, repeatedUnnamedStoreEvents);
    };
    if (isIndexedElementSlot(solver, object, slot))
        forEachElementPart(graph, object, arrayIndexRange, store);
    else
        store(node);
}

bool holdsResolvedValues(const BuiltinTable& builtins, BuiltinSlotID slot)
{
    const BuiltinSlot& record = builtins.at(slot);
    return record.owner == BuiltinClasses::Promise && record.contentsSlot == ContentsSlot::Value;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
