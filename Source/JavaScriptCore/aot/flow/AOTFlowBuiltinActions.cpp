/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTFlowPropertyState.h"
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr size_t maxActionsPerOverload = 1 << 8;
constexpr uint32_t inheritorCursorBit = 1u << 31;

struct ActionApplication {
    FlowSolver& solver;
    const FlowOverloadCall& overloadCall;
    const BuiltinPropertyAction& action;
    uint32_t cursorIndex;
    Witness witness;

    const FlowBuiltinCall& call() const { return overloadCall.call; }
    const FlowCallRequest& request() const { return overloadCall.call.request; }
};

} // namespace

// Most of what arrives in a node names no other key: an object beside an object, a string beside any string. Only another key brings back the objects that were accessed.
static FlowKeySet keysInNode(FlowSolver& solver, FlowStatementSite site, FlowNodeID node)
{
    solver.waitOn(node, site);
    FlowKeySet keys = solver.values().keysOf(solver.valueOf(node));

    Vector<FlowKeysOfNode, 0>& keysOfNodes = statementAt(solver.callState().builtins, site).keysOfNodes;
    size_t index = keysOfNodes.findIf([&](const FlowKeysOfNode& known) {
        return known.node == node;
    });
    if (index == notFound)
        keysOfNodes.append(FlowKeysOfNode { node, keys });
    else if (keysOfNodes[index].keys.unite(keys))
        solver.revisit(site);
    return keys;
}

static FlowKeySet keysInOperand(FlowSolver& solver, const FlowBuiltinCall& call, BuiltinOperand operand)
{
    FlowKeySet keys;
    bool isPassed = false;
    forEachOperandNode(call, operand, [&](FlowNodeID node) {
        isPassed = true;
        keys.unite(keysInNode(solver, call.request.site, node));
    });
    if (!isPassed || (operand.kind() == BuiltinOperand::Kind::Argument && !isAlwaysPassed(call, operand)))
        keys.unite(solver.values().keysOf(solver.values().valueOfPrimitives(Primitive::Undefined)));
    return keys;
}

static FlowKeySet keysInElementsOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand operand)
{
    FlowStatementSite site = overloadCall.call.request.site;
    Witness witness = witnessOf(solver, overloadCall.call);
    FlowNodeID elements = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::KeyListElements, packedOperand(operand)));
    FlowTemporaryKey cursor = callTemporaryKey(site, FlowCallTemporaryRole::KeyListObjects, packedOperand(operand));

    FlowNodeID lists = sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Operand, .operand = operand });
    solver.takeNewObjects(cursor, lists).forEach([&](FlowObjectID object) {
        bool hasElementPart = false;
        forEachElementPart(solver.graph(), object, arrayIndexRange, [&](FlowNodeID part) {
            hasElementPart = true;
            solver.noteLocationEvent(part, FlowLocationEvent::ReadAsValue);
            solver.connect(part, elements);
        });
        if (!hasElementPart)
            solver.joinUnknown(elements, witness);
    });

    return keysInNode(solver, site, elements);
}

FlowKeySet keysOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinKey& key)
{
    switch (key.kind) {
    case BuiltinKeyKind::None:
        return FlowKeySet::none();
    case BuiltinKeyKind::Any:
        return FlowKeySet::everyKey();
    case BuiltinKeyKind::Indexes:
        return FlowKeySet::index(arrayIndexRange);
    case BuiltinKeyKind::Names:
        return keysOfNames(solver.program(), key.names);
    case BuiltinKeyKind::Operand:
        return keysInOperand(solver, overloadCall.call, *key.operand);
    case BuiltinKeyKind::ElementsOf:
        return keysInElementsOf(solver, overloadCall, *key.operand);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An operation on one own property runs the traps of a proxy. One that goes on to the prototypes declares the walk.
static bool readsOnlyOwnProperties(const ActionApplication& application)
{
    const BuiltinOverload& overload = application.overloadCall.overload;
    const BuiltinParameter* parameter = application.solver.builtins().parameterFor(overload, application.action.object);
    if (!parameter || !parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed))
        return false;
    return !declaredBehaviorOf(overload, application.action.object).protocols.contains(Protocol::PrototypeChain);
}

static FlowAccess accessOf(const ActionApplication& application, FlowAccessKind kind, FlowNodeID receiver, FlowNodeID value)
{
    const FlowCallRequest& request = application.request();
    FlowAccess access { .site = request.site, .origin = request.origin, .kind = kind, .receiver = receiver, .value = value, .thrown = request.operands.thrown };
    if (mayRunRepeatedly(request))
        access.flags.add(FlowAccessFlag::IsInCycle);
    if ((kind == FlowAccessKind::Load || kind == FlowAccessKind::Test) && readsOnlyOwnProperties(application))
        access.flags.add(FlowAccessFlag::IsOwnOnly);
    return access;
}

static FlowNodeID sourceNodeOfOperand(const ActionApplication& application, BuiltinOperand operand)
{
    return sourceNodeOf(application.solver, application.overloadCall, { .kind = BuiltinPlaceKind::Operand, .operand = operand });
}

// A cursor reads one node, so the rest arguments are read through the node that collects them.
static void visitObjectOperand(const ActionApplication& application, NOESCAPE const Invocable<void(FlowNodeID, const FlowNewObjects&)> auto& functor)
{
    FlowTemporaryKey cursor = callTemporaryKey(application.request().site, FlowCallTemporaryRole::ActionObjects, application.cursorIndex);
    if (FlowNodeID node = sourceNodeOfOperand(application, application.action.object))
        functor(node, application.solver.takeNewObjects(cursor, node));
}

static FlowNodeID storedValueNodeOf(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    const BuiltinPlace& value = application.action.value;
    FlowNodeID node = sourceNodeOf(solver, application.overloadCall, value);
    if (node && !mayBeMissing(application.call(), value))
        return node;

    FlowNodeID valueOrUndefined = solver.temporaryFor(callTemporaryKey(application.request().site, FlowCallTemporaryRole::PlaceSource, packedPlace(value)));
    solver.joinPrimitives(valueOrUndefined, Primitive::Undefined);
    if (node && node != valueOrUndefined)
        solver.connect(node, valueOrUndefined);
    return valueOrUndefined;
}

static void accessByKey(const ActionApplication& application, FlowAccessKind kind, FlowNodeID value)
{
    FlowSolver& solver = application.solver;
    FlowKeySet keys = keysOf(solver, application.overloadCall, application.action.key);
    visitObjectOperand(application, [&](FlowNodeID node, const FlowNewObjects& objects) {
        FlowAccess access = accessOf(application, kind, node, value);
        accessProperties(solver, access, objects, keys);
        // The table does not say which attributes native code gives the property.
        if (kind == FlowAccessKind::Define)
            noteAttributeChanges(solver, access, objects, keys);
        if (uint16_t primitives = solver.valueOf(node).primitives)
            accessProperties(solver, access, FlowPrimitiveReceiver { primitives }, keys);
    });
}

// Whether each argument from the operand on is always passed and is one object that flow follows. No answer while an argument has not arrived: the call does not run before it has.
static std::optional<bool> passesOneObjectPerArgument(const ActionApplication& application, BuiltinOperand operand)
{
    FlowSolver& solver = application.solver;
    const auto& arguments = application.call().arguments;
    if (arguments.rest)
        return false;

    bool passesOneObject = true;
    for (size_t position = operand.argumentIndex(); position < arguments.positions.size(); ++position) {
        FlowNodeID node = arguments.positions[position];
        if (!node || !arguments.alwaysPasses(position))
            return false;
        solver.waitOn(node, application.request().site);
        const FlowValue& value = solver.valueOf(node);
        if (value.isEmpty())
            return std::nullopt;
        passesOneObject &= !value.primitives && !value.hasUnknownObject() && value.objects.size() == 1;
    }
    return passesOneObject;
}

// The value of the action names the own values of the sources, and copyProperties() takes the node that holds the source objects.
static void assignProperties(const ActionApplication& application)
{
    const FlowCallRequest& request = application.request();
    FlowNodeID sources = sourceNodeOfOperand(application, *application.action.value.operand);
    FlowNodeID targets = sourceNodeOfOperand(application, application.action.object);
    if (!sources || !targets)
        return;

    std::optional<bool> copiesFromEverySource = passesOneObjectPerArgument(application, *application.action.value.operand);
    if (!copiesFromEverySource)
        return;
    // An earlier run may have found one object per argument. What it has copied stays listed as stored, and the sources that have come out are copied again as they are copied after this answer.
    if (!*copiesFromEverySource)
        noteUncertainMemberStore(application.solver, request.site);
    auto knownAnswer = application.solver.propertyState().copiesWithOneObjectPerArgument.add(request.site, *copiesFromEverySource);
    if (!knownAnswer.isNewEntry && knownAnswer.iterator->value != *copiesFromEverySource) {
        knownAnswer.iterator->value = *copiesFromEverySource;
        application.solver.revisit(request.site);
    }
    copyProperties(application.solver, { request.site, request.origin, FlowCopyOperation::Assign, sources, targets, request.operands.thrown, FlowKeySet::none(), mayRunRepeatedly(request), *copiesFromEverySource });
}

static void defineFromDescriptor(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowNodeID descriptor = sourceNodeOf(solver, application.overloadCall, application.action.value);
    if (!descriptor)
        return;
    FlowKeySet keys = keysOf(solver, application.overloadCall, application.action.key);
    visitObjectOperand(application, [&](FlowNodeID node, const FlowNewObjects& objects) {
        FlowAccess access = accessOf(application, FlowAccessKind::Define, node, { });
        access.completion = FlowStoreCompletion::HasStored;
        definePropertiesFromDescriptor(solver, access, objects, keys, descriptor);
    });
}

static void defineFromDescriptorMap(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowNodeID descriptorMap = sourceNodeOf(solver, application.overloadCall, application.action.value);
    if (!descriptorMap)
        return;
    visitObjectOperand(application, [&](FlowNodeID node, const FlowNewObjects& objects) {
        definePropertiesFromDescriptors(solver, accessOf(application, FlowAccessKind::Define, node, { }), objects, descriptorMap);
    });
}

// This runs before the call answers, so whoever finds the map in a node finds what it describes.
static void rememberDescribedObjects(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowNodeID described = sourceNodeOfOperand(application, application.action.object);
    if (!described)
        return;
    forEachResultObject(solver, application.overloadCall, [&](FlowObjectID map) {
        if (map == unknownFlowObject)
            return;
        auto known = solver.propertyState().objectsDescribedByMap.add({ map, application.request().site.instance }, described);
        RELEASE_ASSERT(known.iterator->value == described);
    });
}

// The function behind a built-in accessor is an object that nobody tracks, so whoever holds it calls it as native code does: with any receiver and any value that has escaped.
static void readBuiltinAccessor(const ActionApplication& application, BuiltinID builtin, FlowPropertySourceKind accessor, FlowNodeID target)
{
    FlowSolver& solver = application.solver;
    BuiltinCallMode mode = accessor == FlowPropertySourceKind::Getter ? BuiltinCallMode::Get : BuiltinCallMode::Set;
    if (!solver.builtins().dispatchOf(builtin, mode))
        return;
    solver.joinUnknownObject(target, application.witness);
    if (solver.builtins().at(builtin).kind == BuiltinKind::Accessor)
        callBuiltinAccessorFromNativeCode(solver, builtin, mode);
}

template<typename Receiver>
static void readAccessorsOf(const ActionApplication& application, Receiver receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk, FlowPropertySourceKind accessor, FlowNodeID target)
{
    FlowSolver& solver = application.solver;
    forEachPropertySource(solver, receiver, keys, walk, [&](const FlowPropertySource& source) {
        switch (source.kind) {
        case FlowPropertySourceKind::Getter:
        case FlowPropertySourceKind::Setter:
            if (source.kind == accessor)
                solver.connect(source.node, target, { }, application.witness);
            return;
        case FlowPropertySourceKind::Unknown:
            solver.joinUnknownObject(target, application.witness);
            return;
        // The walk of a deletion reports an accessor of the table as a member.
        case FlowPropertySourceKind::BuiltinMember:
        case FlowPropertySourceKind::BuiltinGetter:
        case FlowPropertySourceKind::BuiltinSetter:
            readBuiltinAccessor(application, source.builtin, accessor, target);
            return;
        case FlowPropertySourceKind::Absent:
        case FlowPropertySourceKind::Value:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static void readGettersByKey(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowNodeID target = targetNodeOf(solver, application.overloadCall, application.action.value);
    FlowKeySet keys = keysOf(solver, application.overloadCall, application.action.key);
    FlowPropertyWalk walk { FlowWalkOperation::Get, false, application.request().site, readsOnlyOwnProperties(application) };
    visitObjectOperand(application, [&](FlowNodeID node, const FlowNewObjects& objects) {
        solver.forEachMember(objects, [&](FlowObjectID object) {
            readAccessorsOf(application, object, keys, walk, FlowPropertySourceKind::Getter, target);
        });
        if (uint16_t primitives = solver.valueOf(node).primitives)
            readAccessorsOf(application, FlowPrimitiveReceiver { primitives }, keys, walk, FlowPropertySourceKind::Getter, target);
    });
}

// The walk of a store makes a location for the stored value on the receiver, so the receiver's own setters come from the walk of a deletion,
// which does not show what unknown code defined on an object it can reach.
static void readSettersByKey(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowStatementSite site = application.request().site;
    FlowNodeID target = targetNodeOf(solver, application.overloadCall, application.action.value);
    FlowKeySet keys = keysOf(solver, application.overloadCall, application.action.key);
    bool isOwnOnly = readsOnlyOwnProperties(application);
    visitObjectOperand(application, [&](FlowNodeID node, const FlowNewObjects& objects) {
        solver.forEachMember(objects, [&](FlowObjectID object) {
            readAccessorsOf(application, object, keys, FlowPropertyWalk { FlowWalkOperation::Delete, false, site }, FlowPropertySourceKind::Setter, target);
            if (!isOwnOnly)
                readAccessorsOf(application, object, keys, FlowPropertyWalk { FlowWalkOperation::Set, true, site }, FlowPropertySourceKind::Setter, target);
            // The walks wait for nothing on a holder that nobody can change.
            solver.waitOnMutations(object, site, ObjectMutation::ReachNativeCode);
            if (solver.hasReachedNativeCode(object))
                solver.joinUnknownObject(target, application.witness);
        });
        if (uint16_t primitives = solver.valueOf(node).primitives)
            readAccessorsOf(application, FlowPrimitiveReceiver { primitives }, keys, FlowPropertyWalk { FlowWalkOperation::Set, false, site }, FlowPropertySourceKind::Setter, target);
    });
}

static void changeIntegrity(const ActionApplication& application)
{
    visitObjectOperand(application, [&](FlowNodeID, const FlowNewObjects& objects) {
        noteIntegrityChange(application.solver, objects, application.witness);
    });
}

// The order of execution follows the prototype link of an object as the member "__proto__" of that object.
static void noteStoresOfPrototypeLink(const ActionApplication& application, const FlowObjectSet& inheritors, FlowNodeID prototypes)
{
    FlowSolver& solver = application.solver;
    FlowStatementSite site = application.request().site;
    PropertyKeyID link = solver.program().keyOf(BuiltinNames::__proto__);
    inheritors.forEach([&](FlowObjectID object) {
        if (memberHolderKindOf(solver.objects(), object) == FlowMemberHolder::No) {
            noteUncertainMemberStore(solver, site);
            return;
        }
        noteMemberStore(solver, site, object, link);
        auto known = solver.propertyState().prototypesGivenByBuiltin.add(object, prototypes);
        if (known.iterator->value != prototypes)
            known.iterator->value = FlowNodeID();
    });
}

static void changePrototypeOf(const ActionApplication& application, const FlowObjectSet& inheritors, FlowNodeID prototypes)
{
    if (inheritors.isEmpty())
        return;

    FlowSolver& solver = application.solver;
    noteStoresOfPrototypeLink(application, inheritors, prototypes);
    changePrototype(solver, inheritors, prototypes, application.witness);
    // Whoever holds an object reads its prototype. Native code holds an object that nobody tracks.
    if (inheritors.contains(unknownFlowObject)) {
        handToUnknownRecipient(solver, prototypes, application.witness);
        return;
    }
    // A built-in object of the realm is tracked: its link follows it to native code if it gets there. Unseen code alone finds it on its own, from the global object.
    bool isFoundByUnseenCode = inheritors.anyOf([&](FlowObjectID inheritor) {
        return solver.objects().kindOf(inheritor) == FlowObjectKind::Builtin;
    });
    if (isFoundByUnseenCode)
        handToUnseenCode(solver, prototypes, application.witness);
}

static void changeOperandPrototype(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowNodeID prototypes = sourceNodeOf(solver, application.overloadCall, application.action.value);
    if (!prototypes)
        return;

    // Nothing but an object or null becomes a prototype, so the objects are taken once such a value has arrived.
    solver.waitOn(prototypes, application.request().site);
    const FlowValue& value = solver.valueOf(prototypes);
    if (!value.mayBeObject() && !(value.primitives & Primitive::Null))
        return;

    visitObjectOperand(application, [&](FlowNodeID, const FlowNewObjects& objects) {
        changePrototypeOf(application, objects, prototypes);
    });
}

static void linkUnknownInheritor(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    visitObjectOperand(application, [&](FlowNodeID node, const FlowNewObjects& objects) {
        solver.connect(node, solver.graph().nativeCode(), { }, application.witness);
        solver.noteMutationOf(objects, ObjectMutation::BecomePrototype, application.witness);
    });
}

static void linkResultObjects(const ActionApplication& application)
{
    FlowSolver& solver = application.solver;
    FlowObjectSet inheritors;
    forEachResultObject(solver, application.overloadCall, [&](FlowObjectID object) {
        inheritors.add(object);
    });
    if (inheritors.isEmpty() || inheritors.remove(unknownFlowObject))
        linkUnknownInheritor(application);

    inheritors.forEach([&](FlowObjectID object) {
        forEachOperandNode(application.call(), application.action.object, [&](FlowNodeID prototypes) {
            connectCreationPrototype(solver, object, prototypes);
        });
    });
}

static void linkOperandObjects(const ActionApplication& application, BuiltinOperand operand)
{
    FlowSolver& solver = application.solver;
    FlowTemporaryKey cursor = callTemporaryKey(application.request().site, FlowCallTemporaryRole::ActionObjects, application.cursorIndex | inheritorCursorBit);
    FlowObjectSet inheritors = solver.takeNewObjects(cursor, sourceNodeOfOperand(application, operand));
    forEachOperandNode(application.call(), application.action.object, [&](FlowNodeID prototypes) {
        changePrototypeOf(application, inheritors, prototypes);
    });
}

static void linkInheritors(const ActionApplication& application)
{
    const BuiltinPlace& inheritor = application.action.value;
    switch (inheritor.kind) {
    case BuiltinPlaceKind::Result:
        linkResultObjects(application);
        return;
    case BuiltinPlaceKind::Operand:
        linkOperandObjects(application, *inheritor.operand);
        return;
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Slot:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
    case BuiltinPlaceKind::NativeCell:
        linkUnknownInheritor(application);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void applyPropertyAction(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPropertyAction& action, uint32_t actionIndex)
{
    ActionApplication application { solver, overloadCall, action, overloadCall.id.index() << 8 | actionIndex, witnessOf(solver, overloadCall.call) };
    switch (action.kind) {
    case BuiltinPropertyActionKind::ReadsByKey:
        accessByKey(application, FlowAccessKind::Load, targetNodeOf(solver, overloadCall, action.value));
        return;
    case BuiltinPropertyActionKind::StoresByKey:
        accessByKey(application, FlowAccessKind::Store, storedValueNodeOf(application));
        return;
    case BuiltinPropertyActionKind::DefinesName:
        accessByKey(application, FlowAccessKind::Define, storedValueNodeOf(application));
        return;
    case BuiltinPropertyActionKind::DeletesByKey:
        accessByKey(application, FlowAccessKind::Delete, { });
        return;
    case BuiltinPropertyActionKind::AssignsFrom:
        assignProperties(application);
        return;
    case BuiltinPropertyActionKind::TakesDescriptor:
        defineFromDescriptor(application);
        return;
    case BuiltinPropertyActionKind::TakesDescriptorMap:
        defineFromDescriptorMap(application);
        return;
    case BuiltinPropertyActionKind::ChangesIntegrity:
        changeIntegrity(application);
        return;
    case BuiltinPropertyActionKind::ChangesPrototype:
        changeOperandPrototype(application);
        return;
    case BuiltinPropertyActionKind::BecomesPrototype:
        linkInheritors(application);
        return;
    case BuiltinPropertyActionKind::DefinesGetter:
        accessByKey(application, FlowAccessKind::DefineGetter, storedValueNodeOf(application));
        return;
    case BuiltinPropertyActionKind::DefinesSetter:
        accessByKey(application, FlowAccessKind::DefineSetter, storedValueNodeOf(application));
        return;
    case BuiltinPropertyActionKind::ReadsGetter:
        readGettersByKey(application);
        return;
    case BuiltinPropertyActionKind::ReadsSetter:
        readSettersByKey(application);
        return;
    case BuiltinPropertyActionKind::TestsByKey:
        accessByKey(application, FlowAccessKind::Test, { });
        return;
    case BuiltinPropertyActionKind::DescribesOwnProperties:
        rememberDescribedObjects(application);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void applyPropertyActions(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    std::span<const BuiltinPropertyAction> actions = overloadCall.overload.flow.propertyActions;
    RELEASE_ASSERT(actions.size() <= maxActionsPerOverload);
    for (uint32_t index = 0; index < actions.size(); ++index)
        applyPropertyAction(solver, overloadCall, actions[index], index);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
