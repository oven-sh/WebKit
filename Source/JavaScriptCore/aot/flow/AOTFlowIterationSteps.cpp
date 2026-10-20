/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowIteration.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowParts.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct IteratedIndexes {
    uint32_t first { 0 };
    bool isSingleIndex { false };
    bool mayBePastEnd { false };

    Range upTo(int64_t lastIndex) const { return { first, isSingleIndex ? first : lastIndex }; }
    Range parts() const { return upTo(arrayIndexRange.maximum); }
};

enum class ValueWhenDone : uint8_t {
    NotTaken,
    Undefined,
    ReadFromResult,
};

struct IteratorStep {
    ValueWhenDone valueWhenDone { ValueWhenDone::NotTaken };
    ValueID patternOpen;
    IteratedIndexes patternIndexes;
};

enum class ContainerView : uint8_t {
    Any,
    Collection,
    List,
};

enum class IteratedContents : uint8_t {
    Unknown,
    MapEntries,
    SetElements,
    Elements,
    TypedArrayElements,
    IndexedProperties,
    Characters,
};

struct IteratedContainer {
    FlowObjectID object;
    IteratedContents contents { IteratedContents::Unknown };
    uint32_t numberOfFilledParts { 0 };
    int64_t lastIndex { 0 };
};

} // anonymous namespace

static Witness witnessOf(const FlowSolver& solver, const FlowIterationRequest& request)
{
    return solver.witnessAt(request.site, request.origin);
}

static FlowNewObjects takeNewObjectsOf(FlowSolver& solver, const FlowIterationRequest& request, FlowCallTemporaryRole role, FlowNodeID node)
{
    return solver.takeNewObjects(callTemporaryKey(request.site, role, node.index()), node);
}

static FlowNodeID iteratorResultsOf(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state)
{
    return solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::IteratorResults, state.index()));
}

static IteratorStep iteratorStepOf(const FlowSolver& solver, const FlowIterationRequest& request)
{
    if (!request.origin)
        return { };
    const Value& resume = solver.operationAt(request.site, request.origin);
    if (resume.opcode != Opcode::IteratorResume)
        return { };

    IteratorStep step { resume.readsValueWhenDone() ? ValueWhenDone::ReadFromResult : ValueWhenDone::Undefined, { }, { } };
    std::optional<uint32_t> stepIndex = resume.stepIndex();
    if (!stepIndex)
        return step;

    const Value& open = solver.operationAt(request.site, resume.iteratorOperand());
    if (open.opcode != Opcode::IteratorOpen)
        return step;

    std::optional<uint32_t> elementCount = open.elementCount();
    step.patternOpen = resume.iteratorOperand();
    step.patternIndexes = { *stepIndex, elementCount && *stepIndex < *elementCount };
    return step;
}

static bool isAllocatedAt(const FlowSolver& solver, const FlowIterationRequest& request, ValueID origin, FlowObjectID object)
{
    const FlowObjectTable& objects = solver.objects();
    AllocationSiteID allocationSite = objects.at(object).site;
    if (!origin || !allocationSite)
        return false;

    const FlowSite& site = objects.at(allocationSite);
    return site.function == solver.graph().at(request.site.instance).function && site.description.value == origin;
}

static IteratedIndexes iteratedIndexesOf(const FlowSolver& solver, const FlowIterationRequest& request, const IteratorStep& step, FlowObjectID iterator)
{
    // The position of a step in its pattern is an index only for the iterator that the open of the pattern created.
    IteratedIndexes indexes = isAllocatedAt(solver, request, step.patternOpen, iterator) ? step.patternIndexes : IteratedIndexes { };
    indexes.mayBePastEnd = step.valueWhenDone != ValueWhenDone::NotTaken;
    return indexes;
}

static void resumeWithoutMethod(FlowSolver& solver, const FlowIterationRequest& request, const FlowCallRequest& call, FlowObjectID iterator, const FlowIteratorResumption& resumption)
{
    Witness witness = witnessOf(solver, request);
    switch (resumption.mode) {
    case ResumeMode::Next:
        return;
    case ResumeMode::Throw: {
        const Program& program = solver.program();
        FlowCallRequest close = call;
        close.operands.arguments = { };
        callMethods(solver, close, FlowObjectSet(iterator), keySetOf(program, methodKeyOf(program, ResumeMode::Return)));
        connectThrown(solver, resumption.sent, request.thrown, witness);
        return;
    }
    case ResumeMode::Return: {
        FlowNodeID target = elementTargetOf(solver, request, resumption.value);
        if (resumption.sent)
            solver.connect(resumption.sent, target, { }, witness);
        else
            solver.joinPrimitives(target, Primitive::Undefined, witness);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static IteratedContainer iteratedCharacters()
{
    return { { }, IteratedContents::Characters, 0, Range::length().maximum };
}

static IteratedContainer iteratedContainerOf(const FlowSolver& solver, FlowObjectID object, ContainerView view)
{
    if (object == unknownFlowObject)
        return { object };

    const FlowGraph& graph = solver.graph();
    const FlowObjectTable& objects = solver.objects();
    const BuiltinTable& builtins = solver.builtins();
    const FlowObject& record = objects.at(object);
    FlowNodeID element = contentsNode(graph, object, ContentsSlot::Element);
    int64_t lastArrayIndex = Range::uint32().maximum;

    if (view != ContainerView::List) {
        if (contentsNode(graph, object, ContentsSlot::Key))
            return { object, IteratedContents::MapEntries };
        if (element && objects.isInstanceOf(object, BuiltinClasses::Set))
            return { object, IteratedContents::SetElements };
    }
    if (view == ContainerView::Collection)
        return { object };

    if (BuiltinSlotID slot = indexedElementSlotOf(builtins, record.builtinClass); element && slot) {
        bool isTypedArray = builtins.at(slot).heap == AbstractHeap::TypedArrayElements;
        return { object, isTypedArray ? IteratedContents::TypedArrayElements : IteratedContents::Elements, record.numberOfPositions, lastArrayIndex };
    }
    if (view == ContainerView::List)
        return { object, IteratedContents::IndexedProperties, 0, maxExactInteger };
    return { object };
}

static FlowPartAccess partLoadOf(const FlowIterationRequest& request, FlowNodeID target)
{
    return { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load, .value = target };
}

static FlowAccess propertyLoadOf(const FlowIterationRequest& request, FlowNodeID receiver, FlowNodeID target)
{
    FlowAccess load { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load, .receiver = receiver, .value = target, .thrown = request.thrown };
    if (request.isInCycle)
        load.flags.add(FlowAccessFlag::IsInCycle);
    return load;
}

static void loadProperties(FlowSolver& solver, const FlowIterationRequest& request, FlowObjectID object, const FlowKeySet& keys, FlowNodeID target)
{
    accessProperties(solver, propertyLoadOf(request, { }, target), FlowObjectSet(object), keys);
}

static void loadKeys(FlowSolver& solver, const FlowIterationRequest& request, const IteratedContainer& container, const IteratedIndexes& indexes, FlowNodeID target)
{
    const FlowGraph& graph = solver.graph();
    switch (container.contents) {
    case IteratedContents::MapEntries:
        accessPart(solver, partLoadOf(request, target), contentsNode(graph, container.object, ContentsSlot::Key));
        return;
    case IteratedContents::SetElements:
        accessPart(solver, partLoadOf(request, target), contentsNode(graph, container.object, ContentsSlot::Element));
        return;
    case IteratedContents::Elements:
    case IteratedContents::TypedArrayElements:
    case IteratedContents::IndexedProperties:
    case IteratedContents::Characters:
        solver.join(target, solver.values().valueOfIntegers(indexes.upTo(container.lastIndex)));
        return;
    case IteratedContents::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void loadValues(FlowSolver& solver, const FlowIterationRequest& request, const IteratedContainer& container, const IteratedIndexes& indexes, FlowNodeID target)
{
    const FlowGraph& graph = solver.graph();
    FlowObjectID object = container.object;
    Range parts = indexes.parts();

    switch (container.contents) {
    case IteratedContents::MapEntries:
        accessPart(solver, partLoadOf(request, target), contentsNode(graph, object, ContentsSlot::Value));
        return;
    case IteratedContents::SetElements:
        accessPart(solver, partLoadOf(request, target), contentsNode(graph, object, ContentsSlot::Element));
        return;
    case IteratedContents::Elements:
        loadElementParts(solver, partLoadOf(request, target), object, parts);
        return;
    case IteratedContents::TypedArrayElements: {
        // A typed array converts what is stored into it, so only the read is recorded on its parts.
        loadElementParts(solver, partLoadOf(request, { }), object, parts);
        TypedArrayType typedArrayType = solver.builtins().at(solver.objects().at(object).builtinClass).typedArrayType;
        solver.join(target, solver.values().valueOfType(elementTypeOf(typedArrayType)), witnessOf(solver, request));
        return;
    }
    case IteratedContents::IndexedProperties:
        loadProperties(solver, request, object, FlowKeySet::index(parts), target);
        return;
    case IteratedContents::Characters:
        solver.joinPrimitives(target, Primitive::String);
        return;
    case IteratedContents::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void connectContents(FlowSolver& solver, const FlowIterationRequest& request, const IteratedContainer& container, IterationKind kind, const IteratedIndexes& indexes, FlowNodeID values)
{
    Witness witness = witnessOf(solver, request);
    if (container.contents == IteratedContents::Unknown) {
        solver.joinUnknown(values, witness);
        return;
    }
    if (container.contents == IteratedContents::IndexedProperties) {
        const Program& program = solver.program();
        loadProperties(solver, request, container.object, keySetOf(program, program.keyOf(BuiltinNames::length)), { });
    }

    switch (kind) {
    case IterationKind::Keys:
        loadKeys(solver, request, container, indexes, values);
        break;
    case IterationKind::Values:
        loadValues(solver, request, container, indexes, values);
        break;
    case IterationKind::Entries: {
        FlowObjectID pair = allocateEntriesPair(solver, request, container.object);
        const FlowGraph& graph = solver.graph();
        bool isTracked = pair != unknownFlowObject;
        loadKeys(solver, request, container, indexes, isTracked ? graph.position(pair, 0) : graph.nativeCode());
        loadValues(solver, request, container, indexes, isTracked ? graph.position(pair, 1) : graph.nativeCode());
        solver.joinObject(values, pair, witness);
        break;
    }
    }

    if (indexes.mayBePastEnd && indexes.parts().maximum >= container.numberOfFilledParts)
        solver.joinPrimitives(values, Primitive::Undefined);
}

static void stepBuiltinIterator(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, FlowObjectID iterator, const IteratedIndexes& indexes, FlowNodeID target)
{
    std::optional<IterationKind> kind = iterationKindOf(solver.callState(), iterator);
    RELEASE_ASSERT(kind);

    if (solver.objects().isInstanceOf(iterator, BuiltinClasses::StringIterator)) {
        connectContents(solver, request, iteratedCharacters(), *kind, indexes, target);
        return;
    }

    BuiltinSlotID slot = solver.builtins().findSlot(solver.objects().at(iterator).builtinClass, BuiltinNames::iterated);
    FlowNodeID iterated = solver.graph().slot(iterator, slot);
    if (!iterated) {
        solver.joinUnknown(target, witnessOf(solver, request));
        return;
    }

    bool stepsOverList = solver.objects().isInstanceOf(iterator, BuiltinClasses::ArrayIterator);
    ContainerView view = stepsOverList ? ContainerView::List : ContainerView::Collection;
    // One iterator object stands for every iteration at its site, and each of those has a target of its own, so each has a cursor of its own.
    FlowTemporaryKey cursor = callTemporaryKey(request.site, FlowCallTemporaryRole::IteratedContainers, state.index());
    solver.forEachMember(solver.takeNewObjects(cursor, iterated), [&](FlowObjectID container) {
        connectContents(solver, request, iteratedContainerOf(solver, container, view), *kind, indexes, target);
    });
    if (stepsOverList && (solver.valueOf(iterated).primitives & Primitive::AnyString))
        connectContents(solver, request, iteratedCharacters(), *kind, indexes, target);
}

// The containers of an iterator arrive one by one. Every run comes here, outside the loop over the iterators, so each container is read once.
static void stepBuiltinIterators(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, const FlowIteratorResumption& resumption)
{
    FlowObjectSet iterators = solver.callState().iteration.steppedIterators.get({ request.site, state });
    if (iterators.isEmpty())
        return;

    FlowNodeID target = elementTargetOf(solver, request, resumption.value);
    IteratorStep step = iteratorStepOf(solver, request);
    iterators.forEach([&](FlowObjectID iterator) {
        stepBuiltinIterator(solver, request, state, iterator, iteratedIndexesOf(solver, request, step, iterator), target);
    });
}

void resumeIterators(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, const FlowNewObjects& iterators, const FlowIteratorResumption& resumption)
{
    if (!state)
        return;

    solver.forEachMember(iterators, [&](FlowObjectID iterator) {
        resumeIterator(solver, request, state, iterator, resumption);
    });
    stepBuiltinIterators(solver, request, state, resumption);

    FlowNodeID results = iteratorResultsOf(solver, request, state);
    switch (request.kind) {
    case IteratorKind::Sync:
        readIteratorResults(solver, request, results, resumption.value);
        return;
    case IteratorKind::Async: {
        FlowNodeID awaited = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::AwaitedIteratorResults, state.index()));
        FlowNodeID values = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::AwaitedElements, awaited.index()));
        awaitValuesOf(solver, { request.site, request.origin, awaited, request.thrown }, results);
        readIteratorResults(solver, request, awaited, values);

        // An async-from-sync iterator awaits the value of each result. Any other iterator hands it on as it is.
        solver.connect(values, resumption.value);
        awaitValuesOf(solver, { request.site, request.origin, resumption.value, request.thrown }, values);
        awaitValuesOf(solver, { request.site, request.origin, resumption.value, request.thrown }, elementTargetOf(solver, request, resumption.value));
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The operation is the method of the prototype: nothing is looked up, and the library has tested the class of the iterator.
void stepArrayIterators(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, const FlowNewObjects& iterators, const FlowIteratorResumption& resumption)
{
    const FlowObjectTable& objects = solver.objects();
    Witness witness = witnessOf(solver, request);
    solver.forEachMember(iterators, [&](FlowObjectID iterator) {
        bool isUntracked = iterator == unknownFlowObject || objects.kindOf(iterator) == FlowObjectKind::Opaque;
        if (!isUntracked && !objects.isInstanceOf(iterator, BuiltinClasses::ArrayIterator))
            return;
        if (!isUntracked && iterationKindOf(solver.callState(), iterator)) {
            resumeDirectly(solver, request, state, iterator, FlowDirectIteration::BuiltinIterator, resumption);
            return;
        }
        solver.joinUnknown(resumption.value, witness);
        throwUnknownValue(solver, request.thrown, witness);
    });
    stepBuiltinIterators(solver, request, state, resumption);
    solver.joinPrimitives(resumption.value, Primitive::Undefined);
}

FlowDirectIteration directIterationOf(FlowSolver& solver, FlowObjectID iterator)
{
    const FlowCallState& state = solver.callState();
    if (iterationKindOf(state, iterator))
        return FlowDirectIteration::BuiltinIterator;

    FlowInstanceID body = coroutineBodyOf(state, iterator);
    if (body && solver.graph().summaryOf(body).coroutine.yielded)
        return FlowDirectIteration::Generator;
    return FlowDirectIteration::Unknown;
}

// What the step becomes may look the method up by name at run time. Such a call reaches a method of the library through its function object, as native code does.
static void enterLibraryFunctionsOf(FlowSolver& solver, BuiltinID method, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    method = builtins.canonicalID(method);
    for (const BuiltinOverload& overload : builtins.overloadsOf(method)) {
        RuntimeFunctionID id = overload.libraryFunction();
        if (!id)
            continue;
        if (FunctionID function = functionOf(solver, id, method, witness))
            enterFromNativeCode(solver, function, FlowObjectID(), witness);
    }
}

// Whoever holds a built-in iterator steps it through the method of its prototype, which is locked.
void exposeBuiltinIterator(FlowSolver& solver, FlowObjectID iterator, const Witness& witness)
{
    if (!iterationKindOf(solver.callState(), iterator))
        return;
    const BuiltinTable& builtins = solver.builtins();
    BuiltinID prototype = builtins.canonicalID(builtins.at(solver.objects().at(iterator).builtinClass).prototype);
    if (BuiltinID next = builtins.findOwnMember(prototype, BuiltinNames::next))
        enterLibraryFunctionsOf(solver, next, witness);
}

void resumeIterator(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, FlowObjectID iterator, const FlowIteratorResumption& resumption)
{
    // The unknown object stands for built-in iterators too: the walk finds their methods in the table.
    if (iterator == unknownFlowObject) {
        Witness witness = witnessOf(solver, request);
        solver.joinUnknown(resumption.value, witness);
        throwUnknownValue(solver, request.thrown, witness);
        handToUnknownRecipient(solver, resumption.sent, witness);
    }

    FlowDirectIteration direct = directIterationOf(solver, iterator);
    if (direct == FlowDirectIteration::BuiltinIterator && resumption.mode != ResumeMode::Next)
        direct = FlowDirectIteration::Unknown;

    std::array<FlowNodeID, 1> sentValues { resumption.sent };
    FlowCallRequest call {
        request.site,
        request.origin,
        FlowCallLinkKind::Protocol,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .thisValue = state, .arguments = std::span(sentValues).first(resumption.sent ? 1 : 0), .result = iteratorResultsOf(solver, request, state), .thrown = request.thrown },
    };

    const Program& program = solver.program();
    FlowKeySet keys = keySetOf(program, methodKeyOf(program, resumption.mode));
    forEachPropertySource(solver, iterator, keys, { FlowWalkOperation::Get, false, request.site }, [&](const FlowPropertySource& source) {
        switch (source.kind) {
        case FlowPropertySourceKind::Absent:
            resumeWithoutMethod(solver, request, call, iterator, resumption);
            return;
        case FlowPropertySourceKind::BuiltinMember:
            switch (direct) {
            case FlowDirectIteration::Unknown:
                break;
            case FlowDirectIteration::BuiltinIterator:
            case FlowDirectIteration::Generator:
                enterLibraryFunctionsOf(solver, source.builtin, witnessOf(solver, request));
                resumeDirectly(solver, request, state, iterator, direct, resumption);
                return;
            }
            break;
        case FlowPropertySourceKind::Unknown:
        case FlowPropertySourceKind::Value:
        case FlowPropertySourceKind::Getter:
        case FlowPropertySourceKind::Setter:
        case FlowPropertySourceKind::BuiltinGetter:
        case FlowPropertySourceKind::BuiltinSetter:
            break;
        }

        callMethodSource(solver, call, iterator, 0, source);
    });
}

void resumeDirectly(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, FlowObjectID iterator, FlowDirectIteration direct, const FlowIteratorResumption& resumption)
{
    switch (direct) {
    case FlowDirectIteration::BuiltinIterator:
        solver.callState().iteration.steppedIterators.add({ request.site, state }, FlowObjectSet()).iterator->value.add(iterator);
        return;
    case FlowDirectIteration::Generator: {
        FlowNodeID target = elementTargetOf(solver, request, resumption.value);
        FlowGeneratorBody body = resumeGenerator(solver, { request.site, request.origin, resumption.mode, resumption.sent, target, request.thrown }, iterator);
        RELEASE_ASSERT(body == FlowGeneratorBody::Resumed);
        if (iteratorStepOf(solver, request).valueWhenDone == ValueWhenDone::Undefined)
            solver.joinPrimitives(resumption.value, Primitive::Undefined);
        return;
    }
    case FlowDirectIteration::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void connectContentsByKind(FlowSolver& solver, const FlowIterationRequest& request, FlowObjectID container, IterationKind kind, FlowNodeID values)
{
    connectContents(solver, request, iteratedContainerOf(solver, container, ContainerView::Any), kind, { }, values);
}

FlowObjectID allocateEntriesPair(FlowSolver& solver, const FlowIterationRequest& request, FlowObjectID)
{
    FlowObjectID pair = allocateAtCallSite(solver, {
        request.site,
        { .kind = FlowSiteKind::Instance, .origin = FlowSiteOrigin::BuiltinElement, .numberOfPositions = 2, .isCreatedDense = true, .value = request.origin, .builtinClass = BuiltinClasses::Array },
    });
    if (pair != unknownFlowObject && request.isInCycle)
        solver.noteEvent(pair, FlowObjectEvent::AllocatedRepeatedly, witnessOf(solver, request));
    return pair;
}

FlowNodeID elementTargetOf(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID value)
{
    if (!value)
        return { };

    switch (request.kind) {
    case IteratorKind::Sync:
        return value;
    case IteratorKind::Async:
        // resumeIterators() awaits what arrives here.
        return solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::AwaitedElements, value.index()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void readIteratorResults(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID results, FlowNodeID value)
{
    FlowNewObjects objects = takeNewObjectsOf(solver, request, FlowCallTemporaryRole::IteratorResultObjects, results);
    if (objects.isEmpty())
        return;

    if (iteratorStepOf(solver, request).valueWhenDone == ValueWhenDone::Undefined)
        solver.joinPrimitives(value, Primitive::Undefined);

    const Program& program = solver.program();
    accessProperties(solver, propertyLoadOf(request, results, value), objects, keySetOf(program, program.keyOf(BuiltinNames::value)));
    accessProperties(solver, propertyLoadOf(request, results, { }), objects, keySetOf(program, program.keyOf(BuiltinNames::done)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
