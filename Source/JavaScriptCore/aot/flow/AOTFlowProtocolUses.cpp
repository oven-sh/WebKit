/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesOfOperandNode {
    OperandUse::Inspects,
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

static constexpr OperandUses contentUses {
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
    OperandUse::RetainsContents,
    OperandUse::MovesElements,
};

static constexpr OperandUses objectModelUses {
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity,
    OperandUse::ChangesPrototype,
    OperandUse::BecomesPrototype,
};

static constexpr OperandUses usesOfOperandObjects = callingUses | contentUses | objectModelUses;

static constexpr OperandUses operandUsesOfNativeCode = callingUses | OperandUses {
    OperandUse::Inspects,
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::PassesToUserCode,
    OperandUse::ReadsContents,
    OperandUse::RetainsContents,
};

static_assert((usesOfOperandNode | usesOfOperandObjects) == everyFlagOf<OperandUse>());

namespace {

enum class ContentsListing : uint8_t {
    Unknown,
    Complete,
};

} // anonymous namespace

// connectOperand applies a transitive behavior to the reachable objects, and those of the operand are among them.
static bool appliesToOperandObjects(const OperandBehavior& behavior)
{
    return !claimsEveryUse(behavior) && !behavior.uses.contains(OperandUse::AppliesTransitively) && behavior.uses.containsAny(usesOfOperandObjects);
}

static ContentsListing contentsListingOf(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return ContentsListing::Unknown;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
        return ContentsListing::Complete;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool holdsContents(FlowNodeKind kind)
{
    switch (kind) {
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Property:
        return true;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The declared parts of a closure are its captures, which only its own body reads.
static Vector<FlowNodeID, 8> contentNodesOf(const FlowGraph& graph, FlowObjectID object)
{
    bool isClosure = graph.objects().kindOf(object) == FlowObjectKind::Closure;
    Vector<FlowNodeID, 8> nodes;
    graph.forEachNodeOf(object, [&](FlowNodeID node) {
        FlowNodeKind kind = graph.kindOf(node);
        if (holdsContents(kind) && !(isClosure && kind == FlowNodeKind::DeclaredPart))
            nodes.append(node);
    });
    return nodes;
}

static void connectContentsOf(FlowSolver& solver, FlowStatementSite site, FlowObjectID object, FlowNodeID target, const Witness& witness)
{
    FlowObjectKind kind = solver.objects().kindOf(object);
    if (contentsListingOf(kind) == ContentsListing::Unknown)
        solver.joinUnknown(target, witness);
    if (kind == FlowObjectKind::Unknown)
        return;

    // The unknown object stands only for what has reached native code, so the exports have to reach it.
    if (kind == FlowObjectKind::ModuleNamespace)
        solver.joinObject(solver.graph().nativeCode(), object, witness);

    solver.waitOnProperties(object, site);
    for (FlowNodeID node : contentNodesOf(solver.graph(), object)) {
        solver.noteLocationEvent(node, FlowLocationEvent::ReadAsValue);
        solver.connect(node, target, { }, witness);
    }
}

static void storeIntoRetainer(FlowSolver& solver, FlowNodeID retained, FlowNodeID part, const Witness& witness)
{
    solver.noteLocationEvent(part, repeatedUnnamedStoreEvents);
    solver.connect(retained, part, { }, witness);
}

static void retainInObject(FlowSolver& solver, FlowNodeID retained, FlowObjectID retainer, const Witness& witness)
{
    FlowGraph& graph = solver.graph();
    if (retainer == unknownFlowObject) {
        solver.connect(retained, graph.nativeCode(), { }, witness);
        storeIntoRetainer(solver, retained, graph.undeclaredPart(retainer), witness);
        storeIntoRetainer(solver, retained, solver.ensureOpenPart(retainer, FlowOpenPart::IndexedProperties), witness);
        return;
    }
    if (FlowNodeID part = graph.undeclaredPart(retainer)) {
        storeIntoRetainer(solver, retained, part, witness);
        return;
    }

    bool hasElementPart = false;
    forEachElementPart(graph, retainer, arrayIndexRange, [&](FlowNodeID part) {
        hasElementPart = true;
        storeIntoRetainer(solver, retained, part, witness);
    });
    if (hasElementPart)
        return;

    if (FlowNodeID contents = contentsNode(graph, retainer, ContentsSlot::Element)) {
        storeIntoRetainer(solver, retained, contents, witness);
        return;
    }
    solver.connect(retained, graph.nativeCode(), { }, witness);
}

static void retainOperand(FlowSolver& solver, const FlowOperandApplication& application, RetainedIn retainedIn, const Witness& witness)
{
    switch (retainedIn) {
    case RetainedIn::UnknownMemory:
        solver.connect(application.operand, solver.graph().nativeCode(), { }, witness);
        return;
    case RetainedIn::Result:
    case RetainedIn::Operand:
        retainInObjects(solver, application, application.operand);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void applyToReachableObjects(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior)
{
    OperandBehavior rest = behavior;
    rest.uses.remove(OperandUse::AppliesTransitively);
    if (rest.protocols.isEmpty() && rest.uses.containsOnly(OperandUse::Inspects))
        return;

    // Nothing is reachable before the first object arrives.
    const FlowValue& value = solver.valueOf(application.operand);
    bool actsOnPrimitives = rest.uses.containsAny({ OperandUse::Retains, OperandUse::Returns }) || actOnPrimitives(rest.protocols, value.primitives);
    if (!actsOnPrimitives && value.objects.isEmpty()) {
        solver.waitOn(application.operand, application.site, wakeUpAtFirstReceiver(rest.protocols));
        return;
    }

    bool resultIsRead = !application.resultStaysInOperand && isReadByProgram(solver, application.result);
    bool isDoneByNativeCode = rest.uses.containsOnly(operandUsesOfNativeCode) && areRunByNativeCode(rest.protocols, resultIsRead);

    // What a protocol reads from a reachable object, or is handed by its code, is reachable too.
    FlowOperandApplication nested = application;
    nested.operand = transitiveContentsOf(solver, application.site, application.operand, isDoneByNativeCode ? FlowEscapedObjects::AreUnknown : FlowEscapedObjects::AreFollowed);
    nested.result = solver.temporaryFor(callTemporaryKey(application.site, FlowCallTemporaryRole::TransitiveContents, nested.operand.index()));
    nested.resultStaysInOperand = !resultIsRead;
    solver.connect(nested.result, nested.operand);
    solver.connect(nested.result, application.result);
    applyOperandBehavior(solver, nested, rest);
}

static FlowNodeID contentsOf(FlowSolver& solver, FlowStatementSite site, FlowNodeID operand)
{
    FlowNodeID contents = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::ContentsOfOperand, operand.index()));
    Witness witness = solver.witnessAt(site, ValueID());
    solver.forEachMember(solver.takeNewObjects(callTemporaryKey(site, FlowCallTemporaryRole::HoldersOfContents, operand.index()), operand), [&](FlowObjectID holder) {
        connectContentsOf(solver, site, holder, contents, witness);
    });
    return contents;
}

// An object of the operand is among the receivers only where something that the operand holds leads back to it.
static void runProtocolsOnContents(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior)
{
    if (behavior.protocols.isEmpty())
        return;

    OperandBehavior behaviorOfContents = behavior;
    behaviorOfContents.uses = { OperandUse::Inspects, OperandUse::AppliesTransitively };
    FlowOperandApplication nested = application;
    nested.operand = contentsOf(solver, application.site, application.operand);
    applyOperandBehavior(solver, nested, behaviorOfContents);
}

void runOperandUse(FlowSolver& solver, FlowStatementSite site, const FlowOperandUse& use)
{
    if (!use.operand) {
        runOperationWithoutOperand(solver, site, use);
        return;
    }

    FlowNodeID operand = solver.node(site, use.operand);
    bool hasOperation = use.origin && !use.isTerminatorOperand;
    OperandBehavior behavior = hasOperation ? declaredBehaviorOf(solver.operationAt(site, use.origin).opcode, use.operandIndex) : OperandBehavior();
    if (!use.thrown) {
        solver.noteDirectUses(operand, hasOperation ? behavior.uses : OperandUses { OperandUse::Inspects });
        return;
    }

    applyOperandBehavior(solver, { site, use.origin, operand, solver.node(site, use.retainer), solver.node(site, use.result), solver.node(site, use.thrown) }, behavior);
}

void runOperationWithoutOperand(FlowSolver& solver, FlowStatementSite site, const FlowOperandUse& use)
{
    Witness witness = solver.witnessAt(site, use.origin);
    if (use.thrown)
        throwRuntimeError(solver, solver.node(site, use.thrown), witness);
    if (!use.origin)
        return;

    const Value& operation = solver.operationAt(site, use.origin);
    if (operation.opcode == Opcode::NewRuntimeError) {
        solver.joinObject(solver.node(site, use.result), runtimeErrorObject(solver), witness);
        return;
    }
    if (operation.opcode == Opcode::NativeBinding) {
        noteBuiltinUse(solver, operation.builtin(), { BuiltinUse::Read, BuiltinUse::Called, BuiltinUse::Escaped }, witness);
        solver.joinUnknown(solver.node(site, use.result), witness);
        // Creating the binding runs nothing. A call of it, or of what is read from it, does.
        solver.joinObject(solver.node(site, use.result), ensureBuiltinObject(solver, operation.builtin()), witness);
    }
}

void applyOperandBehavior(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior)
{
    if (!application.operand)
        return;
    connectOperand(solver, application, behavior);
    if (!appliesToOperandObjects(behavior))
        return;
    FlowTemporaryKey cursor = callTemporaryKey(application.site, FlowCallTemporaryRole::OperandObjects, application.operand.index());
    applyOperandBehaviorTo(solver, application, behavior, solver.takeNewObjects(cursor, application.operand));
}

void connectOperand(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior)
{
    FlowGraph& graph = solver.graph();
    FlowNodeID operand = application.operand;
    Witness witness = solver.witnessAt(application.site, application.origin);

    solver.noteDirectUses(operand, behavior.uses);
    if (claimsEveryUse(behavior)) {
        solver.connect(operand, graph.unknownJavaScript(), { }, witness);
        solver.joinUnknown(application.result, witness);
        throwUnknownValue(solver, application.thrown, witness);
        return;
    }
    if (behavior.uses.contains(OperandUse::AppliesTransitively)) {
        applyToReachableObjects(solver, application, behavior);
        return;
    }

    if (behavior.uses.contains(OperandUse::Returns))
        solver.connect(operand, application.result, { }, witness);
    if (behavior.uses.contains(OperandUse::Retains))
        retainOperand(solver, application, behavior.retainedIn, witness);
    if (behavior.uses.contains(OperandUse::PassesToUserCode))
        solver.connect(operand, graph.nativeCode(), { }, witness);
    // A retainer object that arrives after the operand's objects gets their contents too.
    if (behavior.uses.contains(OperandUse::RetainsContents))
        retainedContentsTargetOf(solver, application, behavior);
    if (behavior.uses.contains(OperandUse::AppliesToContents)) {
        runProtocolsOnContents(solver, application, behavior);
        return;
    }
    runProtocols(solver, { application.site, application.origin, operand, behavior.protocols, behavior.namedMembers, application.result, application.thrown, behavior.namedMemberUse, behavior.calledMembers, application.resultStaysInOperand, false, behavior.functionsAreConvertedUnderNamesOnly, behavior.convertedMembers });
}

void retainInObjects(FlowSolver& solver, const FlowOperandApplication& application, FlowNodeID retained)
{
    if (!retained)
        return;

    Witness witness = solver.witnessAt(application.site, application.origin);
    if (!application.retainer) {
        solver.connect(retained, solver.graph().nativeCode(), { }, witness);
        return;
    }

    FlowTemporaryKey cursor = callTemporaryKey(application.site, FlowCallTemporaryRole::RetainerObjects, retained.index());
    solver.takeNewObjects(cursor, application.retainer).forEach([&](FlowObjectID retainer) {
        retainInObject(solver, retained, retainer, witness);
    });
}

void applyOperandBehaviorTo(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior, const FlowNewObjects& objects)
{
    if (objects.isEmpty() || !appliesToOperandObjects(behavior))
        return;

    if (behavior.uses.containsAny(callingUses))
        callOperandObjects(solver, application, behavior.uses, objects);
    if (behavior.uses.containsAny(contentUses)) {
        solver.forEachMember(objects, [&](FlowObjectID object) {
            applyContentUses(solver, application, behavior, object);
        });
    }
    applyMutatingUses(solver, application, behavior.uses, objects);
}

BuiltinCallbackTimes callbackTimesOf(OperandUses uses)
{
    if (uses.contains(OperandUse::CallsRepeatedly))
        return BuiltinCallbackTimes::Repeatedly;
    if (uses.contains(OperandUse::CallsPerElement))
        return BuiltinCallbackTimes::PerElement;
    return BuiltinCallbackTimes::Once;
}

void callOperandObjects(FlowSolver& solver, const FlowOperandApplication& application, OperandUses uses, const FlowNewObjects& objects)
{
    FlowNodeID unknownValue = unknownValueNode(solver);
    FlowCallRequest request {
        application.site,
        application.origin,
        FlowCallLinkKind::BuiltinCallback,
        callbackTimesOf(uses),
        true,
        { .thisValue = unknownValue, .argumentList = unknownValue, .result = solver.graph().nativeCode(), .thrown = application.thrown },
    };
    callObjects(solver, request, objects);
}

void applyContentUses(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior, FlowObjectID object)
{
    Witness witness = solver.witnessAt(application.site, application.origin);
    OperandUses uses = behavior.uses;

    if (uses.contains(OperandUse::MovesElements))
        joinPositions(solver, object, witness);
    if (uses.containsAny({ OperandUse::WritesContents, OperandUse::MovesElements }))
        noteContentsStore(solver, object, witness);
    if (uses.contains(OperandUse::RetainsContents))
        connectContentsOf(solver, application.site, object, retainedContentsTargetOf(solver, application, behavior), witness);
    // Native code reads every location of what has reached it, also one that is made later.
    if (!uses.contains(OperandUse::ReadsContents) || object == unknownFlowObject || solver.hasReachedNativeCode(object))
        return;

    solver.noteUse(object, FlowObjectUse::ReadByUnknownKey);
    solver.waitOnProperties(object, application.site);
    for (FlowNodeID node : contentNodesOf(solver.graph(), object))
        solver.noteLocationEvent(node, FlowLocationEvent::ReadAsValue);
}

FlowNodeID retainedContentsTargetOf(FlowSolver& solver, const FlowOperandApplication& application, const OperandBehavior& behavior)
{
    switch (behavior.retainedIn) {
    case RetainedIn::UnknownMemory:
        return solver.graph().nativeCode();
    case RetainedIn::Result:
    case RetainedIn::Operand: {
        FlowNodeID contents = solver.temporaryFor(callTemporaryKey(application.site, FlowCallTemporaryRole::TransitiveContents, application.operand.index() ^ 0x80000000));
        retainInObjects(solver, application, contents);
        return contents;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// unknownJavaScript opens each object it is handed. The unknown object is none: at an operand it stands for the objects in native code that the operand takes.
// Where that is every object, what the entry does is done through the unknown object's own locations, which all of them alias. Where the entry throws for anything but
// the objects it accepts, those are the objects of the operand, and each is opened like one that flows in.
void applyEveryUseThroughUnknownObject(FlowSolver& solver, const FlowOperandApplication& application)
{
    if (!std::exchange(solver.callState().unseenCode.everyUseIsAppliedToUnknownObject, true))
        applyMutatingUses(solver, application, objectModelUses, FlowObjectSet(unknownFlowObject));
}

void applyEveryUseToUnknownObject(FlowSolver& solver, const FlowOperandApplication& application, const BuiltinParameter& parameter)
{
    solver.waitOn(application.operand, application.site);
    if (!solver.valueOf(application.operand).hasUnknownObject())
        return;

    const BuiltinType& accepted = solver.builtins().at(parameter.accepted);
    if (!takesOnlyAcceptedObjects(solver.builtins(), parameter) || matchObject(solver, unknownFlowObject, accepted) == FlowTypeMatch::Always) {
        applyEveryUseThroughUnknownObject(solver, application);
        return;
    }

    FlowGraph& graph = solver.graph();
    Witness witness = solver.witnessAt(application.site, application.origin);
    FlowNewObjects escaped = solver.takeNewObjects(callTemporaryKey(application.site, FlowCallTemporaryRole::EscapedObjectsOfOperand, parameter.accepted.index()), graph.nativeCode());
    solver.charge(application.site, escaped.size());
    escaped.forEach([&](FlowObjectID object) {
        if (object != unknownFlowObject && matchObject(solver, object, accepted) != FlowTypeMatch::Never)
            solver.joinObject(graph.unknownJavaScript(), object, witness);
    });
}

void noteIntegrityChange(FlowSolver& solver, const FlowObjectSet& objects, const Witness& witness)
{
    solver.noteMutationOf(objects, ObjectMutation::ChangeIntegrity, witness);
    // The unknown object may be a proxy, which seals and freezes its target by redefining one property after the other.
    if (objects.containsUnknownObject())
        solver.noteMutation(unknownFlowObject, ObjectMutation::RedefineProperty, witness);
}

void applyMutatingUses(FlowSolver& solver, const FlowOperandApplication& application, OperandUses uses, const FlowNewObjects& objects)
{
    if (!uses.containsAny(objectModelUses))
        return;

    Witness witness = solver.witnessAt(application.site, application.origin);
    FlowNodeID unknownValue = unknownValueNode(solver);
    FlowKeySet everyKey = FlowKeySet::everyKey();
    FlowAccess access { .site = application.site, .origin = application.origin, .flags = FlowAccessFlag::IsInCycle, .receiver = application.operand, .value = unknownValue, .thrown = application.thrown };

    if (uses.containsAny({ OperandUse::StoresByKey, OperandUse::AssignsFrom })) {
        access.kind = FlowAccessKind::Store;
        accessProperties(solver, access, objects, everyKey);
    }
    if (uses.containsAny({ OperandUse::DefinesName, OperandUse::TakesDescriptor })) {
        access.kind = FlowAccessKind::Define;
        accessProperties(solver, access, objects, everyKey);
        noteAttributeChanges(solver, access, objects, everyKey);
        solver.noteMutationOf(objects, ObjectMutation::RedefineProperty, witness);
    }
    if (uses.contains(OperandUse::DeletesByKey)) {
        access.kind = FlowAccessKind::Delete;
        access.value = FlowNodeID();
        accessProperties(solver, access, objects, everyKey);
    }
    if (uses.contains(OperandUse::ChangesIntegrity))
        noteIntegrityChange(solver, objects, witness);
    if (uses.contains(OperandUse::ChangesPrototype))
        changePrototype(solver, objects, unknownValue, witness);
    if (uses.contains(OperandUse::BecomesPrototype))
        solver.noteMutationOf(objects, ObjectMutation::BecomePrototype, witness);
}

FlowNodeID transitiveContentsOf(FlowSolver& solver, FlowStatementSite site, FlowNodeID operand, FlowEscapedObjects escapedObjects)
{
    if (!operand)
        return { };

    bool followsEscapedObjects = escapedObjects == FlowEscapedObjects::AreFollowed;
    FlowCallTemporaryRole contentsRole = followsEscapedObjects ? FlowCallTemporaryRole::TransitiveContents : FlowCallTemporaryRole::ContentsOutsideNativeCode;
    FlowCallTemporaryRole objectsRole = followsEscapedObjects ? FlowCallTemporaryRole::TransitiveObjects : FlowCallTemporaryRole::ObjectsOutsideNativeCode;

    FlowNodeID reachable = solver.temporaryFor(callTemporaryKey(site, contentsRole, operand.index()));
    solver.connect(operand, reachable);

    Witness witness = solver.witnessAt(site, ValueID());
    FlowTemporaryKey cursor = callTemporaryKey(site, objectsRole, reachable.index());
    for (;;) {
        FlowNewObjects fresh = solver.takeNewObjects(cursor, reachable);
        if (fresh.isEmpty() || solver.isAboutToBeAbandoned(site))
            break;
        // The fixpoint check hands out every object again. Where none was left to the unknown value, each was followed before it escaped, and is followed again.
        bool leavesEscapedObjects = !solver.isCheckingFixpoint() || solver.valueOf(reachable).hasUnknownObject();
        solver.forEachMember(fresh, [&](FlowObjectID object) {
            // Whatever an object in the hands of unknown JavaScript holds has reached unknown JavaScript too, and the unknown object stands for all of it.
            // The use of a member of a built-in object is recorded where it is read. So such an object is followed, and an object of the program is left out
            // only where the members of escaped built-in objects follow their holders: that covers the built-in objects which only it leads to.
            bool hasEscaped = followsEscapedObjects
                ? solver.callState().unseenCode.membersFollowHolders && canEscapeAsReceiver(solver.objects().kindOf(object)) && solver.hasReachedUnknownJavaScript(object)
                : solver.hasReachedNativeCode(object);
            if (object == unknownFlowObject || (leavesEscapedObjects && hasEscaped)) {
                solver.joinUnknown(reachable, witness);
                return;
            }
            solver.charge(site, 1);
            connectContentsOf(solver, site, object, reachable, witness);
        });
    }
    return reachable;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
