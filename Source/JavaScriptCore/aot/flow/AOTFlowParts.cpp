/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowObjectsInternal.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTLayout.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static bool standsForOneLocation(const FlowGraph& graph, FlowNodeID part)
{
    switch (graph.kindOf(part)) {
    case FlowNodeKind::Position:
    case FlowNodeKind::Property:
        return true;
    case FlowNodeKind::DeclaredPart: {
        FlowObjectKind kind = graph.objects().kindOf(graph.objectOf(part));
        return kind == FlowObjectKind::Allocation || kind == FlowObjectKind::Closure;
    }
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        return false;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void noteElementMoved(FlowSolver& solver, FlowNodeID element, const Witness& witness)
{
    solver.joinPrimitives(element, Primitive::Undefined, witness);
    solver.noteLocationEvent(element, overwrittenLocationEvents);
}

static void joinEscapedPositions(FlowSolver& solver, const Witness& witness)
{
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    FlowObjectSet escapedObjects = solver.valueOf(solver.graph().nativeCode()).objects;
    escapedObjects.forEach([&](FlowObjectID object) {
        // didEscape() leaves the same objects out when they escape after the store.
        if (object != unknownFlowObject && mutabilityOf(inputs, holderOf(solver.objects(), object)) != FlowHolderMutability::Immutable)
            joinPositions(solver, object, witness);
    });
}

BuiltinSlotID indexedElementSlotOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    if (!builtinClass)
        return { };

    BuiltinSlotID slot = builtins.findSlot(builtinClass, ContentsSlot::Element);
    if (!slot)
        return { };

    AbstractHeap heap = builtins.at(slot).heap;
    if (heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements)
        return slot;
    return { };
}

FlowNodeID contentsNode(const FlowGraph& graph, FlowObjectID object, ContentsSlot contentsSlot)
{
    BuiltinClassID builtinClass = graph.objects().at(object).builtinClass;
    if (!builtinClass)
        return { };
    return graph.slot(object, graph.program().builtins().findSlot(builtinClass, contentsSlot));
}

void forEachElementPart(const FlowGraph& graph, FlowObjectID object, Range indexes, NOESCAPE const ScopedLambda<void(FlowNodeID)>& functor)
{
    if (indexes.isEmpty())
        return;

    const Program& program = graph.program();
    const FlowObject& record = graph.objects().at(object);
    switch (record.kind) {
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque: {
        BuiltinSlotID slot = indexedElementSlotOf(program.builtins(), record.builtinClass);
        if (!slot)
            return;

        int64_t numberOfPositions = record.numberOfPositions;
        int64_t lastPosition = std::min(indexes.maximum, numberOfPositions - 1);
        for (int64_t index = std::max<int64_t>(indexes.minimum, 0); index <= lastPosition; ++index)
            functor(graph.position(object, static_cast<uint32_t>(index)));
        if (indexes.maximum >= numberOfPositions)
            functor(graph.slot(object, slot));
        return;
    }
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void joinPositions(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    if (!solver.noteEvent(object, FlowObjectEvent::ElementsMoved, witness))
        return;

    if (object == unknownFlowObject) {
        joinEscapedPositions(solver, witness);
        return;
    }

    const FlowGraph& graph = solver.graph();
    uint32_t numberOfPositions = solver.objects().at(object).numberOfPositions;
    if (!numberOfPositions)
        return;

    FlowNodeID rest = contentsNode(graph, object, ContentsSlot::Element);
    RELEASE_ASSERT(rest);
    for (uint32_t index = 0; index < numberOfPositions; ++index) {
        FlowNodeID position = graph.position(object, index);
        solver.connect(position, rest, { }, witness);
        solver.connect(rest, position, { }, witness);
        noteElementMoved(solver, position, witness);
    }
}

void noteContentsStore(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    solver.noteMutation(object, ObjectMutation::StoreAfterCreation, witness);
    solver.noteEvent(object, FlowObjectEvent::ContentsStored, witness);
}

FlowFieldLocation findFieldLocation(const FlowGraph& graph, FlowObjectID object, PropertyKeyID key)
{
    if (!graph.objects().hasLayout(object))
        return { };

    const FlowObject& record = graph.objects().at(object);
    std::optional<uint32_t> index = graph.program().at(record.layout).findField(key);
    if (index && *index < record.numberOfDeclaredParts)
        return { FlowFieldLookup::Found, graph.field(object, *index), *index };
    return { FlowFieldLookup::Absent, { }, 0 };
}

FlowFieldLocation findFieldLocation(const FlowGraph& graph, FlowObjectID object, LayoutID declaringLayout, uint32_t fieldIndex)
{
    const Program& program = graph.program();
    bool hasLayout = graph.objects().hasLayout(object);
    if (hasLayout) {
        const FlowObject& record = graph.objects().at(object);
        if (record.layout == declaringLayout) {
            RELEASE_ASSERT(fieldIndex < record.numberOfDeclaredParts);
            return { FlowFieldLookup::Found, graph.field(object, fieldIndex), fieldIndex };
        }

        const Vector<Field>& fields = program.at(record.layout).fields;
        for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index) {
            if (fields[index].originLayout == declaringLayout && fields[index].originField == fieldIndex)
                return { FlowFieldLookup::Found, graph.field(object, index), index };
        }
    }

    if (PropertyKeyID key = program.at(declaringLayout).fields[fieldIndex].key) {
        FlowFieldLocation declared = findFieldLocation(graph, object, key);
        if (declared.lookup == FlowFieldLookup::Found)
            return declared;

        const FlowProperty* property = graph.findProperty(object, key);
        if (property && property->value)
            return { FlowFieldLookup::Found, property->value, 0 };
    }
    return { hasLayout ? FlowFieldLookup::Absent : FlowFieldLookup::Unknown, { }, 0 };
}

void recordStoredValue(FlowLocationRecord& record, const FlowClosureCreation* closure)
{
    if (!closure || closure->use != FlowClosureUse::StoredOnce || closure->capture == FlowClosureCapture::Other) {
        record.events.add(FlowLocationEvent::StoredOtherValue);
        record.isGivenOtherValueByName = true;
        return;
    }

    ClosureDeferral capture = closure->capture == FlowClosureCapture::Nothing ? ClosureDeferral::CapturesNothing : ClosureDeferral::CapturesObject;
    if (record.deferrableFunction && (record.deferrableFunction != closure->function || record.deferrableCapture != capture)) {
        record.events.add(FlowLocationEvent::StoredOtherValue);
        record.isGivenOtherValueByName = true;
        return;
    }
    record.events.add(FlowLocationEvent::StoredDeferrableClosure);
    record.deferrableFunction = closure->function;
    record.deferrableCapture = capture;
}

void recordStore(FlowSolver& solver, const FlowRecordedStore& store, FlowNodeID location)
{
    if (!location)
        return;

    Witness witness = solver.witnessAt(store.site, store.origin);
    FlowLocationRecord& record = solver.records().ensureLocationRecord(location);

    // The host's statements share noWitness, so a store without a function is never known to be one operation.
    bool isFirstStore = hasFunction(witness) && !record.events.contains(FlowLocationEvent::StoredByOperation);
    bool isSameOperation = hasFunction(witness) && record.firstStore.function == witness.function && record.firstStore.value == witness.value;
    if (!isFirstStore && !isSameOperation)
        record.events.add(FlowLocationEvent::StoredByManyOperations);
    record.events.add(FlowLocationEvent::StoredByOperation);
    if (store.naming == FlowStoreNaming::Unknown)
        record.events.add(FlowLocationEvent::StoredWithoutNamingLocation);
    keepLowerWitness(record.firstStore, witness);
    recordStoredValue(record, store.storedLocal ? solver.summaryOf(store.site).closureCreationAt(store.storedLocal) : nullptr);
}

void accessPart(FlowSolver& solver, const FlowPartAccess& access, FlowNodeID part)
{
    if (!part)
        return;

    switch (access.kind) {
    case FlowAccessKind::Load:
        solver.connect(part, access.value);
        solver.noteLocationEvent(part, readEventOf(access.use));
        return;
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
        solver.connect(access.value, part, enforcedTypeOf(solver.graph(), part), solver.witnessAt(access.site, access.origin));
        if (standsForOneLocation(solver.graph(), part))
            recordStore(solver, { access.site, access.origin, access.storedLocal, access.naming }, part);
        else
            solver.noteLocationEvent(part, overwrittenLocationEvents);
        return;
    case FlowAccessKind::Delete:
        solver.noteLocationEvent(part, FlowLocationEvent::Deleted);
        keepLowerWitness(solver.records().ensureLocationRecord(part).firstLoss, solver.witnessAt(access.site, access.origin));
        return;
    case FlowAccessKind::Test:
        solver.noteLocationEvent(part, FlowLocationEvent::PresenceObserved);
        return;
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> numberOfFilledElements(const FlowSolver& solver, FlowObjectID object)
{
    const FlowObject& record = solver.objects().at(object);
    switch (record.kind) {
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        if (!indexedElementSlotOf(solver.builtins(), record.builtinClass))
            return std::nullopt;
        return record.numberOfPositions;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void loadElementParts(FlowSolver& solver, const FlowPartAccess& load, FlowObjectID list, Range indexes)
{
    RELEASE_ASSERT(load.kind == FlowAccessKind::Load);
    std::optional<uint32_t> filledElements = numberOfFilledElements(solver, list);
    if (!filledElements || indexes.isEmpty())
        return;

    forEachElementPart(solver.graph(), list, indexes, [&](FlowNodeID part) {
        accessPart(solver, load, part);
    });
    if (load.value && indexes.maximum >= *filledElements)
        solver.joinPrimitives(load.value, Primitive::Undefined);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
