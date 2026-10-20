/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPrototypes.h"
#include "AOTFlowSinkEquations.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CalledSource {
    FlowCallTemporaryRole role;
    uint32_t index;
    ModuleID module;
    FlowNodeID receivers;
};

// One request, for any number of receivers. What a source leads to depends on the receiver only through the node that collects the receivers,
// so the second receiver that finds a source joins that node and nothing else.
struct MethodLookup {
    static constexpr size_t maxCalledSources = 16;

    MethodLookup(const FlowCallRequest& request, FlowPassedArguments&& arguments)
        : request(request)
        , arguments(WTF::move(arguments))
    {
    }

    const FlowCallRequest& request;
    FlowPassedArguments arguments;
    Vector<CalledSource, 8> calledSources;
    bool hasCalledUnknownMethod { false };
    bool hasEscapingReceiver { false };
    // The receivers come from a cursor, so a wait brings one of them back and not the whole statement.
    bool waitsPerReceiver { false };
};

struct FoundMethod {
    const FlowCallRequest& request;
    MethodLookup& lookup;
    FlowObjectID receiver;
    uint16_t primitiveReceiver;
    const FlowPropertySource& source;
};

struct ReceiverHub {
    FlowNodeID node;
    bool isCalled;
};

} // namespace

// No value is inside never. The edge only says, to whoever follows edges, that the operand is used as the target is.
static void connectUses(FlowSolver& solver, FlowNodeID operand, FlowNodeID target)
{
    solver.connect(operand, target, TypeTable::never);
}

// A run without a new receiver still follows the operands: it expands new argument lists, and turns the site generic once an operand holds the unknown object.
// A protocol that passes the unknown value by construction is generic from its first callee on, and says nothing about the other calls of its statement before that.
static FlowPassedArguments checkOperands(FlowSolver& solver, const FlowCallRequest& request)
{
    FlowPassedArguments arguments = expandArguments(solver, request);
    if (!request.operands.argumentList || request.operands.argumentList != unknownValueNode(solver))
        contextOf(solver, request, arguments, FlowObjectSet());
    return arguments;
}

static void joinReceiver(FlowSolver& solver, FlowNodeID hub, const FoundMethod& method)
{
    if (method.receiver) {
        solver.joinObject(hub, method.receiver);
        return;
    }
    solver.joinPrimitives(hub, method.primitiveReceiver);
    if (method.primitiveReceiver & Primitive::Integer)
        solver.join(hub, solver.values().valueOfIntegers(Range::full()));
}

static ReceiverHub receiverHub(FlowSolver& solver, const FoundMethod& method, FlowCallTemporaryRole role, uint32_t index, ModuleID module = { })
{
    Vector<CalledSource, 8>& calledSources = method.lookup.calledSources;
    for (const CalledSource& called : calledSources) {
        if (called.role == role && called.index == index && called.module == module) {
            joinReceiver(solver, called.receivers, method);
            return { called.receivers, true };
        }
    }

    FlowNodeID hub = solver.temporaryFor(callTemporaryKey(method.request.site, role, index));
    joinReceiver(solver, hub, method);
    if (calledSources.size() < MethodLookup::maxCalledSources)
        calledSources.append(CalledSource { role, index, module, hub });

    FlowNodeID operand = method.request.operands.thisValue;
    if (!operand)
        return { hub, false };

    auto binding = solver.callState().operandFlows.operandOfReceiverHub.add(hub, operand);
    if (binding.isNewEntry)
        connectUses(solver, operand, hub);
    else if (binding.iterator->value != operand)
        solver.noteDirectUses(operand, OperandUses::all());
    return { hub, false };
}

static FlowCallRequest withThis(const FlowCallRequest& request, FlowNodeID hub)
{
    FlowCallRequest call = request;
    call.operands.thisValue = hub;
    call.operands.thisObject = { };
    return call;
}

static FlowCallRequest getterRequestOf(const FlowCallRequest& request, FlowNodeID hub, FlowNodeID result)
{
    return {
        request.site,
        request.origin,
        FlowCallLinkKind::Accessor,
        BuiltinCallbackTimes::Once,
        mayRunRepeatedly(request),
        { .thisValue = hub, .result = result, .thrown = request.operands.thrown },
    };
}

static ModuleID moduleOfHolder(const FlowSolver& solver, FlowObjectID holder)
{
    if (!holder || solver.objects().kindOf(holder) != FlowObjectKind::ModuleBuiltin)
        return { };
    return solver.objects().at(holder).module;
}

static void callUnknownMethod(FlowSolver& solver, const FoundMethod& method)
{
    const FlowCallRequest& request = method.request;
    if (method.receiver && canEscapeAsReceiver(solver.objects().kindOf(method.receiver))) {
        // The solver records the mutation on each object that it adds to a sink.
        if (!solver.records().possibleMutations(method.receiver).contains(ObjectMutation::ReachUnknownJavaScript))
            handToUnknownRecipient(solver, method.receiver, solver.witnessAt(request.site, request.origin));
        if (!std::exchange(method.lookup.hasEscapingReceiver, true))
            connectUses(solver, request.operands.thisValue, solver.graph().nativeCode());
    }
    if (std::exchange(method.lookup.hasCalledUnknownMethod, true))
        return;

    FlowCallRequest call = request;
    call.operands.thisValue = { };
    call.operands.thisObject = { };
    callObjects(solver, call, FlowObjectSet(unknownFlowObject));
}

// A location that is only called may hold no closure, so each call that reads it has to become a direct call. Only the call of a member in emitted code can:
// a protocol looks its member up by name at run time, and so does the native code a model stands for. A call of a variable with a second receiver stays a lookup by name.
static FlowLocationEvent readEventOfCallee(FlowSolver& solver, const FoundMethod& method)
{
    const FlowCallRequest& request = method.request;
    if (request.kind != FlowCallLinkKind::Call || solver.functionOf(request.site).role == FunctionRole::Model)
        return FlowLocationEvent::ReadAsValue;
    FlowLocationEvent event = readEventOf(solver, method.source, FlowReadUse::CalleeOnly);
    if (event == FlowLocationEvent::ReadAsValue || solver.graph().kindOf(method.source.node) != FlowNodeKind::Variable)
        return event;

    FlowNodeID receivers = request.operands.thisValue;
    if (!receivers || !method.receiver || method.source.certainty != FlowCertainty::Proven)
        return FlowLocationEvent::ReadAsValue;

    const FlowValue& value = solver.valueOf(receivers);
    bool hasOneReceiver = value.objects.onlyObject() == method.receiver && !(value.primitives & ~Primitive::UndefinedOrNull);
    if (!hasOneReceiver)
        return FlowLocationEvent::ReadAsValue;

    // A second receiver never leaves again, so only this answer can change.
    solver.waitOn(receivers, request.site, FlowWakeUp::Revisit);
    return FlowLocationEvent::ReadAsCallee;
}

static void callValue(FlowSolver& solver, const FoundMethod& method)
{
    FlowNodeID callees = method.source.node;
    solver.noteLocationEvent(callees, readEventOfCallee(solver, method));

    if (holdsValuesOfUnnamedKeys(solver.graph().kindOf(callees)) && saysNoMoreThanUnknownSource(solver, method.source)) {
        callUnknownMethod(solver, method);
        return;
    }

    // Each holder has a node of its own for the keys nobody names, and nearly all of these stay empty: a call of one is made when it holds something to call.
    if (method.lookup.waitsPerReceiver && !method.source.key && !solver.valueOf(callees).mayBeObject()) {
        solver.waitOn(callees, method.request.site, FlowWakeUp::Revisit);
        return;
    }

    ReceiverHub hub = receiverHub(solver, method, FlowCallTemporaryRole::ReceiversOfSource, callees.index());
    if (!hub.isCalled)
        callValuesWith(solver, withThis(method.request, hub.node), method.lookup.arguments, callees);
}

static void callGetterResults(FlowSolver& solver, const FoundMethod& method)
{
    const FlowCallRequest& request = method.request;
    FlowNodeID getters = method.source.node;
    if (saysNoMoreThanUnknownSource(solver, method.source)) {
        callUnknownMethod(solver, method);
        return;
    }

    ReceiverHub hub = receiverHub(solver, method, FlowCallTemporaryRole::ReceiversOfGetter, getters.index());
    if (hub.isCalled)
        return;

    FlowNodeID result = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::ResultOfGetter, getters.index()));
    callValuesOf(solver, getterRequestOf(request, hub.node, result), getters);
    callValuesWith(solver, withThis(request, hub.node), method.lookup.arguments, result);
}

// A member whose receiver the table does not describe is a method of a host class, and such a method refuses a receiver that is no instance of its class. Native code calls it like any function that reached it.
static bool mayChangeReceiver(const BuiltinOverload& overload)
{
    if (overload.isUndeclared() || (overload.receiver && overload.receiver->uses == OperandUses::all()))
        return false;
    if (overload.receiver && overload.receiver->uses.containsAny(mutatingUses))
        return true;
    bool storesIntoReceiver = std::ranges::any_of(overload.flow.transfers, [](const BuiltinTransfer& transfer) {
        return transfer.to.operand && transfer.to.operand->isReceiver();
    });
    return storesIntoReceiver || std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        return action.object.isReceiver();
    });
}

// A lookup through the unknown object finds a member of a built-in prototype because the receiver inherits it, so the member changes those escaped objects and no others.
// Native code's one statement per member takes the calls of every site: the work grows with the sites plus the receivers, not with their product.
static void callOnEscapedInheritors(FlowSolver& solver, const FoundMethod& method, BuiltinID member, BuiltinCallMode mode)
{
    const FlowCallRequest& request = method.request;
    BuiltinID holder = solver.builtins().at(method.source.builtin).holder;
    FlowNodeID receivers = escapedInheritorsOf(solver, holder);
    if (solver.builtins().at(holder).kind != BuiltinKind::Prototype)
        solver.joinObject(receivers, ensureBuiltinObject(solver, holder));
    connectUses(solver, request.operands.thisValue, receivers);

    FlowCallRequest call {
        nativeCodeSite(solver),
        ValueID(),
        FlowCallLinkKind::Host,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .thisValue = receivers, .result = request.operands.result, .thrown = request.operands.thrown },
    };
    callBuiltinOfModule(solver, call, method.lookup.arguments, member, mode, moduleOfHolder(solver, method.source.holder));
}

// A locale, a module specifier, a string that a line of the entry tests: recordOperandValues() keeps what each call passes there.
static bool hasOperandReadPerCall(const BuiltinTable& builtins, BuiltinID member)
{
    auto hasRole = [](const BuiltinParameter& parameter) {
        return !parameter.roles.isEmpty();
    };
    bool hasOperandWithRole = std::ranges::any_of(builtins.overloadsOf(member), [&](const BuiltinOverload& overload) {
        return (overload.receiver && hasRole(*overload.receiver)) || std::ranges::any_of(overload.parameters, hasRole);
    });
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(member);
    return hasOperandWithRole
        || std::ranges::any_of(builtins.featureUsesOf(member), &BuiltinFeatureUse::condition)
        || std::ranges::any_of(builtins.moduleLoadsOf(member), &BuiltinModuleLoad::condition)
        || (unseenCode && unseenCode->condition);
}

static void callBuiltinMember(FlowSolver& solver, const FoundMethod& method)
{
    BuiltinID member = solver.builtins().canonicalID(method.source.builtin);
    BuiltinCallMode mode = builtinCallModeOf(method.request.kind);
    bool isEvent = letsInUnseenCode(solver.builtins(), member);
    // The custom inspect method of a class of the host calls a function of a start-up module itself.
    bool isCalledByHost = solver.builtins().at(member).origin == BuiltinOrigin::Host && passesOperandsOfHost(solver, method.request.site, method.request);
    if (isCalledByHost && isInSeenCode(solver, method.request.site))
        exposeStartupModules(solver, solver.witnessAt(method.request.site, method.request.origin));
    if (!standsForWhatItHolds(solver.builtins(), member) && !solver.builtins().dispatchOf(member, mode)) {
        callUnknownMethod(solver, method);
        return;
    }

    ModuleID module = moduleOfHolder(solver, method.source.holder);
    if (!isEvent && (method.receiver == unknownFlowObject || method.source.isFoundThroughUnknownObject)) {
        // Once unseen JavaScript can run, everything can happen to every escaped object anyway.
        bool mayChangeReceivers = !solver.records().mayRunUnknownJavaScript() && std::ranges::any_of(solver.builtins().overloadsOf(member), mayChangeReceiver);
        if (method.receiver == unknownFlowObject && mayChangeReceivers) {
            callOnEscapedInheritors(solver, method, member, mode);
            return;
        }
        if (method.source.isFoundThroughUnknownObject && !mayChangeReceivers && !hasOperandReadPerCall(solver.builtins(), member)) {
            // What the member does to a receiver nobody knows is the same at every site. The unknown source that the walk reports beside the member hands
            // the operands of this call to native code, which calls whatever has reached it with unknown operands, once for the program.
            FlowObjectID function = module ? ensureModuleBuiltinObject(solver, member, module) : ensureBuiltinObject(solver, member);
            solver.joinObject(solver.graph().nativeCode(), function, solver.witnessAt(method.request.site, method.request.origin));
            return;
        }
    }

    ReceiverHub hub = receiverHub(solver, method, FlowCallTemporaryRole::ReceiversOfBuiltin, member.index(), module);
    if (!hub.isCalled)
        callBuiltinOfModule(solver, withThis(method.request, hub.node), method.lookup.arguments, member, mode, module);
}

static void callBuiltinGetterResults(FlowSolver& solver, const FoundMethod& method)
{
    const FlowCallRequest& request = method.request;
    BuiltinID getter = method.source.builtin;
    ReceiverHub hub = receiverHub(solver, method, FlowCallTemporaryRole::ReceiversOfBuiltinGetter, getter.index());
    FlowNodeID result = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::ResultOfBuiltinGetter, getter.index()));
    // A data property answers for the object that holds it, whoever the lookup started at. One node stands for `this` of the getter and of the function it answers.
    const FlowPropertySource& source = method.source;
    if (method.receiver && source.holder && source.key && solver.builtins().at(getter).kind == BuiltinKind::DataProperty) {
        forEachHolderOfInstanceProperty(solver, method.receiver, source.key, source.holder, request.site, [&](FlowObjectID holder) {
            solver.joinObject(hub.node, holder);
        });
    }

    if (!hub.isCalled) {
        callBuiltin(solver, getterRequestOf(request, hub.node, result), getter, BuiltinCallMode::Get);

        FlowNodeID knownResult = solver.gateFor(callTemporaryKey(request.site, FlowCallTemporaryRole::KnownResultOfBuiltinGetter, getter.index()));
        solver.connect(result, knownResult);
        callValuesWith(solver, withThis(request, hub.node), method.lookup.arguments, knownResult);
    }

    // A function of the host that the table does not describe treats its receiver like a member that the table does not list.
    if (solver.valueOf(result).hasUnknownObject())
        callUnknownMethod(solver, method);
    else
        solver.waitOn(result, request.site, FlowWakeUp::Revisit);
}

static constexpr OperandUses usesOfCallee = callingUses | OperandUses { OperandUse::Inspects };

static bool onlyCallsReceiver(const BuiltinTable& builtins, BuiltinID member)
{
    std::span<const BuiltinOverload> overloads = builtins.overloadsOf(builtins.canonicalID(member));
    return !overloads.empty() && std::ranges::all_of(overloads, [](const BuiltinOverload& overload) {
        const BuiltinParameter* receiver = overload.receiver;
        return !overload.isUndeclared() && receiver && receiver->uses.contains(OperandUse::Calls) && receiver->uses.containsOnly(usesOfCallee) && receiver->protocols.isEmpty();
    });
}

// f.call(x) and f.apply(x, list) use a closure as a call of it does.
static bool isCallThroughBuiltin(const FlowSolver& solver, FlowObjectID receiver, const FlowPropertySource& source)
{
    return solver.objects().kindOf(receiver) == FlowObjectKind::Closure
        && source.kind == FlowPropertySourceKind::BuiltinMember
        && source.certainty == FlowCertainty::Proven
        && onlyCallsReceiver(solver.builtins(), source.builtin);
}

static FlowBuiltinMemberLookup builtinMembersFoundBy(const FlowCallRequest& request, const FlowKeySet& keys)
{
    return request.kind == FlowCallLinkKind::Protocol ? builtinMembersFoundByProtocol(keys) : FlowBuiltinMemberLookup::Every;
}

static void callFoundMethod(FlowSolver& solver, const FoundMethod& method)
{
    switch (method.source.kind) {
    case FlowPropertySourceKind::Unknown:
        callUnknownMethod(solver, method);
        return;
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        return;
    case FlowPropertySourceKind::Value:
        callValue(solver, method);
        return;
    case FlowPropertySourceKind::Getter:
        callGetterResults(solver, method);
        return;
    case FlowPropertySourceKind::BuiltinMember:
        callBuiltinMember(solver, method);
        return;
    case FlowPropertySourceKind::BuiltinGetter:
        callBuiltinGetterResults(solver, method);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What the program stored under a name through a receiver nobody knows is what a call of that name through such a receiver is predicted to reach, beside the unknown callee.
static void callMethodsStoredThroughUnknownReceivers(FlowSolver& solver, const FlowCallRequest& request, MethodLookup& lookup, const FlowKeySet& keys)
{
    for (PropertyKeyID name : keys.names) {
        solver.waitOnProperty(unknownFlowObject, name, request.site);
        const FlowProperty* entry = solver.graph().findProperty(unknownFlowObject, name);
        if (!entry || !entry->value)
            continue;
        FlowPropertySource source { FlowPropertySourceKind::Value, FlowCertainty::Predicted, true, unknownFlowObject, name, entry->value };
        callFoundMethod(solver, { request, lookup, unknownFlowObject, 0, source });
    }
}

void callMethods(FlowSolver& solver, const FlowCallRequest& request, const FlowNewObjects& receivers, const FlowKeySet& keys)
{
    MethodLookup lookup(request, checkOperands(solver, request));
    lookup.waitsPerReceiver = !!receivers.cursor();
    solver.noteDirectUses(request.operands.thisValue, OperandUse::Inspects);

    FlowPropertyWalk walk { .waiter = request.site, .builtinMembers = builtinMembersFoundBy(request, keys), .receivers = receivers.cursor() ? request.operands.thisValue : FlowNodeID() };
    std::optional<FlowNewObjects> remaining = receiversBesideUnknownObject(solver, receivers, request.operands.thisValue);
    solver.forEachMember(remaining ? *remaining : receivers, [&](FlowObjectID receiver) {
        if (solver.isAboutToBeAbandoned(request.site))
            return;
        bool hasSource = false;
        bool hasOtherSource = false;
        forEachPropertySource(solver, receiver, keys, walk, [&](const FlowPropertySource& source) {
            hasSource = true;
            hasOtherSource |= !isCallThroughBuiltin(solver, receiver, source);
            callFoundMethod(solver, { request, lookup, receiver, 0, source });
        });
        if (receiver == unknownFlowObject && request.kind == FlowCallLinkKind::Call)
            callMethodsStoredThroughUnknownReceivers(solver, request, lookup, keys);
        if (!hasSource || hasOtherSource)
            solver.noteUse(receiver, lookupUseOf(keys));
    });
}

void callMethodsOfPrimitives(FlowSolver& solver, const FlowCallRequest& request, FlowPrimitiveReceiver receiver, const FlowKeySet& keys)
{
    MethodLookup lookup(request, expandArguments(solver, request));
    FlowPropertyWalk walk { .waiter = request.site, .builtinMembers = builtinMembersFoundBy(request, keys) };
    forEachPropertySource(solver, receiver, keys, walk, [&](const FlowPropertySource& source) {
        callFoundMethod(solver, { request, lookup, FlowObjectID(), receiver.primitives, source });
    });
}

void callMethodSource(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID receiver, uint16_t primitiveReceiver, const FlowPropertySource& source)
{
    MethodLookup lookup(request, expandArguments(solver, request));
    callFoundMethod(solver, { request, lookup, receiver, primitiveReceiver, source });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
