/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <algorithm>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesWithRecords {
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::RetainsContents,
    OperandUse::Calls,
    OperandUse::CallsPerElement,
    OperandUse::CallsRepeatedly,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity,
    OperandUse::ChangesPrototype,
    OperandUse::BecomesPrototype,
    OperandUse::PassesToUserCode,
};

static constexpr OperandUses usesShownByEdges { OperandUse::Retains, OperandUse::Returns };

// These say on which objects the protocols run, so each part of a behavior keeps them.
static constexpr OperandUses usesChoosingReceivers { OperandUse::AppliesTransitively, OperandUse::AppliesToContents };

// No built-in returns what it awaits: a transfer says where the value goes.
static constexpr Protocols resultProtocols { Protocol::Species };

namespace {

enum class BehaviorPart : uint8_t {
    Main,
    ResultProtocols,
    MemberProtocols,
};

struct ParameterBehavior {
    BuiltinOperand operand;
    BehaviorPart part { BehaviorPart::Main };
    OperandBehavior behavior;
    FlowNodeID result;
};

} // anonymous namespace

static uint32_t packedParameter(BuiltinOverloadID overload, const ParameterBehavior& parameter)
{
    static_assert(BuiltinOverloadID::absentIndex <= UINT16_MAX);
    uint32_t operand = packedOperand(parameter.operand);
    ASSERT(operand < 1u << 14);
    return overload.index() << 16 | static_cast<uint32_t>(parameter.part) << 14 | operand;
}

static FlowOperandApplication applicationOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowNodeID operand, FlowNodeID result)
{
    const FlowCallRequest& request = overloadCall.call.request;
    return { request.site, request.origin, operand, { }, result, operandCodeThrownNodeOf(solver, overloadCall) };
}

static bool isOnlyInspected(const OperandBehavior& behavior)
{
    return behavior.protocols.isEmpty() && behavior.uses.containsOnly(OperandUse::Inspects);
}

static bool mayHoldObject(const FlowSolver& solver, FlowNodeID node)
{
    TypeID type = solver.graph().typeAsRead(node);
    return !type || mayBeReference(solver.program().at(type));
}

static bool isCalledLater(const BuiltinOverload& overload, BuiltinOperand operand)
{
    return std::ranges::any_of(overload.flow.callbacks, [&](const BuiltinCallback& callback) {
        return callback.callee == operand && isCalledAfterReturn(callback.times);
    });
}

static bool isNamedOutsideTransfers(const BuiltinFlow& flow, BuiltinOperand operand)
{
    auto namesOperand = [&](const BuiltinPlace& place) {
        return place.operand == operand || place.kind == BuiltinPlaceKind::NewValue;
    };
    return std::ranges::any_of(flow.transfers, [](const BuiltinTransfer& transfer) {
        return transfer.from.kind == BuiltinPlaceKind::NewValue;
    }) || std::ranges::any_of(flow.propertyActions, [&](const BuiltinPropertyAction& action) {
        return action.object == operand || namesOperand(action.value);
    }) || std::ranges::any_of(flow.callbacks, [&](const BuiltinCallback& callback) {
        return namesOperand(callback.newTarget) || namesOperand(callback.thisValue) || namesOperand(callback.spreadArguments) || std::ranges::any_of(callback.arguments, namesOperand);
    }) || std::ranges::any_of(flow.handlerCalls, [&](const BuiltinHandlerCall& handler) {
        return namesOperand(handler.thisValue) || std::ranges::any_of(handler.arguments, namesOperand);
    });
}

// A transfer or an action leaves an edge at a retained or returned operand, which says where it goes. A callback kept for later leaves none.
// A transfer that reads parts of an operand leaves a record of the node that takes them.
static void noteUsesWithoutEdges(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand operand, OperandUses declaredUses)
{
    OperandUses uses = declaredUses - usesShownByEdges;
    if (!isNamedOutsideTransfers(overloadCall.overload.flow, operand))
        uses.remove(OperandUse::RetainsContents);
    if (isCalledLater(overloadCall.overload, operand))
        uses.add(OperandUse::Retains);
    forEachOperandNode(overloadCall.call, operand, [&](FlowNodeID node) {
        solver.noteDirectUses(node, uses);
    });
}

static void applyDeclaredBehavior(FlowSolver& solver, const FlowOverloadCall& overloadCall, const ParameterBehavior& parameter)
{
    const FlowBuiltinCall& call = overloadCall.call;
    if (isOnlyInspected(parameter.behavior))
        return;

    // The evaluator finds its cursors by site and operand node, so each behavior that looks at objects gets a node of its own.
    FlowNodeID hub;
    forEachOperandNode(call, parameter.operand, [&](FlowNodeID node) {
        if (!mayHoldObject(solver, node)) {
            if (mayActOnPrimitives(parameter.behavior.protocols))
                applyOperandBehavior(solver, applicationOf(solver, overloadCall, node, parameter.result), parameter.behavior);
            return;
        }
        // The behavior moves no value, so an operand that holds nothing to look at needs no node and no edge yet.
        const FlowValue& value = solver.valueOf(node);
        if (value.objects.isEmpty() && !actOnPrimitives(parameter.behavior.protocols, value.primitives)) {
            solver.waitOn(node, call.request.site, wakeUpAtFirstReceiver(parameter.behavior.protocols));
            return;
        }
        if (!hub)
            hub = solver.temporaryFor(callTemporaryKey(call.request.site, FlowCallTemporaryRole::ParameterOperands, packedParameter(overloadCall.id, parameter)));
        solver.connect(node, hub);
    });
    if (hub)
        applyOperandBehavior(solver, applicationOf(solver, overloadCall, hub, parameter.result), parameter.behavior);
}

// A built-in whose result is never what the species constructor returns works on that object itself: it reads it and calls its methods.
static FlowNodeID targetOfConstructedObjects(const FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    if (solver.builtins().at(overloadCall.overload.result.type).freshness == BuiltinFreshness::UnlessSpecies)
        return overloadCall.call.request.operands.result;
    return solver.graph().nativeCode();
}

static bool mayReturnUnknownObject(const BuiltinType& type)
{
    if (type.isAnything() || type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }))
        return true;
    return type.freshness == BuiltinFreshness::No && std::ranges::contains(type.parts, BuiltinTypePartKind::Instance, &BuiltinTypePart::kind);
}

// The built-in may return what a member of the operand holds or returns, and an unknown result stands only for what has reached native code.
// The conversion goes with the members: it also converts what is read under their names.
static void applyMemberProtocols(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand operand, OperandBehavior& behavior)
{
    constexpr Protocols protocolsWithMembers { Protocol::Named, Protocol::CallsNamed };
    constexpr Protocols memberProtocols = protocolsWithMembers | Protocols { Protocol::ToPrimitive };
    if (!behavior.protocols.containsAny(protocolsWithMembers) || !mayReturnUnknownObject(solver.builtins().at(overloadCall.overload.result.type)))
        return;

    OperandBehavior memberBehavior = behavior;
    memberBehavior.uses = behavior.uses & usesChoosingReceivers;
    memberBehavior.protocols = behavior.protocols & memberProtocols;
    behavior.protocols.remove(memberProtocols);
    behavior.namedMembers = { };
    behavior.calledMembers = { };
    applyDeclaredBehavior(solver, overloadCall, { operand, BehaviorPart::MemberProtocols, memberBehavior, solver.graph().nativeCode() });
}

static void applyBehaviorWithoutRecords(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand operand, OperandBehavior behavior)
{
    FlowNodeID result = targetOfConstructedObjects(solver, overloadCall);
    behavior.uses.remove(usesWithRecords);
    applyMemberProtocols(solver, overloadCall, operand, behavior);

    Protocols producing = behavior.protocols & resultProtocols;
    if (producing.isEmpty()) {
        applyDeclaredBehavior(solver, overloadCall, { operand, BehaviorPart::Main, behavior, { } });
        return;
    }
    if (producing == behavior.protocols) {
        applyDeclaredBehavior(solver, overloadCall, { operand, BehaviorPart::Main, behavior, result });
        return;
    }

    OperandBehavior producingBehavior = behavior;
    producingBehavior.uses = behavior.uses & usesChoosingReceivers;
    producingBehavior.protocols = producing;
    producingBehavior.namedMembers = { };
    producingBehavior.calledMembers = { };
    behavior.protocols.remove(producing);
    applyDeclaredBehavior(solver, overloadCall, { operand, BehaviorPart::Main, behavior, { } });
    applyDeclaredBehavior(solver, overloadCall, { operand, BehaviorPart::ResultProtocols, producingBehavior, result });
}

// An operation on one own property runs the traps of a proxy, as a walk of the prototype chain does, and shows nobody the order of the keys.
static OperandBehavior behaviorOf(const BuiltinOverload& overload, const BuiltinParameter& parameter)
{
    OperandBehavior behavior = declaredBehaviorOf(overload, parameter.operand);
    if (parameter.roles.contains(BuiltinOperandRole::KeysAreNotListed) && behavior.protocols.contains(Protocol::OwnKeys)) {
        behavior.protocols.remove(Protocol::OwnKeys);
        behavior.protocols.add(Protocol::PrototypeChain);
    }
    return behavior;
}

void applyParameters(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    if (overloadCall.overload.receiver)
        applyParameter(solver, overloadCall, *overloadCall.overload.receiver);
    for (const BuiltinParameter& parameter : overloadCall.overload.parameters)
        applyParameter(solver, overloadCall, parameter);
}

void runProtocolsOfParameters(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    auto runProtocols = [&](const BuiltinParameter& parameter) {
        OperandBehavior behavior = behaviorOf(overloadCall.overload, parameter);
        if (claimsEveryUse(behavior)) {
            applyParameter(solver, overloadCall, parameter);
            return;
        }
        behavior.uses = behavior.uses & (usesChoosingReceivers | OperandUses { OperandUse::Inspects });
        applyBehaviorWithoutRecords(solver, overloadCall, parameter.operand, behavior);
    };
    if (overloadCall.overload.receiver)
        runProtocols(*overloadCall.overload.receiver);
    for (const BuiltinParameter& parameter : overloadCall.overload.parameters)
        runProtocols(parameter);
}

// Every use is also what an operand gets that nobody has described: in an overload without a declaration, and in one that a generator made from a signature.
static bool describesEveryUse(const BuiltinOverload& overload)
{
    return !overload.flags.containsAny({ BuiltinOverloadFlag::IsUndeclared, BuiltinOverloadFlag::IsProvenByConstruction });
}

// A statement of native code's own stands for a call of seen code whose callee nobody knows, and the operands of such a call reach native code and keep their shape.
static void handToUndescribedNativeCode(FlowSolver& solver, const FlowOperandApplication& application)
{
    Witness witness = solver.witnessAt(application.site, application.origin);
    solver.noteDirectUses(application.operand, OperandUses::all());
    handToUnknownRecipient(solver, application.operand, witness);
    solver.joinUnknown(application.result, witness);
    throwUnknownValue(solver, application.thrown, witness);
}

void applyParameter(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinParameter& parameter)
{
    const FlowBuiltinCall& call = overloadCall.call;
    OperandBehavior behavior = behaviorOf(overloadCall.overload, parameter);
    if (claimsEveryUse(behavior)) {
        forEachOperandNode(call, parameter.operand, [&](FlowNodeID node) {
            FlowOperandApplication application = applicationOf(solver, overloadCall, node, call.request.operands.result);
            if (describesEveryUse(overloadCall.overload)) {
                applyOperandBehavior(solver, application, behavior);
                applyEveryUseToUnknownObject(solver, application, parameter);
            } else if (isInSeenCode(solver, call.request.site))
                applyOperandBehavior(solver, application, behavior);
            else
                handToUndescribedNativeCode(solver, application);
        });
    } else {
        noteUsesWithoutEdges(solver, overloadCall, parameter.operand, behavior.uses);
        applyBehaviorWithoutRecords(solver, overloadCall, parameter.operand, behavior);
    }
    applyOperandRoles(solver, overloadCall, parameter);
}

// A length above the one the array has leaves holes in between, and 0 alone is above none.
static void noteHolesBelowNewLength(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand newLength)
{
    const FlowBuiltinCall& call = overloadCall.call;
    FlowStatementSite site = call.request.site;

    bool mayBeAboveLength = false;
    forEachOperandNode(call, newLength, [&](FlowNodeID node) {
        solver.waitOn(node, site);
        const FlowValue& value = solver.valueOf(node);
        bool isZero = !value.mayBeObject() && value.primitives == Primitive::Integer && solver.values().rangeOf(value.integers) == Range::one(0);
        mayBeAboveLength |= !value.isEmpty() && !isZero;
    });
    if (!mayBeAboveLength)
        return;

    Witness witness = witnessOf(solver, call);
    forEachOperandNode(call, BuiltinOperand::receiver(), [&](FlowNodeID receiver) {
        solver.waitOn(receiver, site);
        FlowObjectSet arrays = solver.valueOf(receiver).objects;
        arrays.forEach([&](FlowObjectID array) {
            solver.noteEvent(array, FlowObjectEvent::HoleMade, witness);
        });
    });
}

void applyOperandRoles(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinParameter& parameter)
{
    const FlowBuiltinCall& call = overloadCall.call;
    if (!mayBePassed(call, parameter.operand))
        return;

    for (BuiltinOperandRole role : parameter.roles) {
        switch (role) {
        case BuiltinOperandRole::EvaluatedAsText:
            if (isAppliedBySeenCode(solver, call))
                raiseUnseenCode(solver, UnseenCodeCause::EvaluatesText, call.builtin, witnessOf(solver, call));
            break;
        case BuiltinOperandRole::ModuleSpecifier:
            if (!isAppliedBySeenCode(solver, call))
                break;
            forEachOperandNode(call, parameter.operand, [&](FlowNodeID node) {
                loadModulesNamedBy(solver, overloadCall, node);
            });
            break;
        case BuiltinOperandRole::BecomesLengthOfReceiver:
            noteHolesBelowNewLength(solver, overloadCall, parameter.operand);
            break;
        case BuiltinOperandRole::ReadAsCharacters:
        case BuiltinOperandRole::Locale:
        case BuiltinOperandRole::ContentKey:
        case BuiltinOperandRole::ReplacesContents:
        case BuiltinOperandRole::MembersAreOnlyInspected:
        case BuiltinOperandRole::KeysAreNotListed:
        case BuiltinOperandRole::MembersAreOfObjectsOnly:
        case BuiltinOperandRole::FunctionsAreConvertedUnderNamesOnly:
        case BuiltinOperandRole::CopiesHolesToResult:
            break;
        }
    }
}

static ModuleID findModuleNamed(const Program& program, StringID name)
{
    for (size_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.path == name || module.displayPath == name)
            return ModuleID(index);
    }
    return { };
}

void loadModulesNamedBy(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowNodeID specifier)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallRequest& request = call.request;
    solver.waitOn(specifier, request.site);
    const FlowValue& value = solver.valueOf(specifier);
    if (value.isEmpty())
        return;

    Witness witness = witnessOf(solver, call);
    FlowModuleLoad load { request.site, request.origin, request.operands.result, request.operands.thrown };
    bool startsInstance = solver.builtins().at(call.builtin).flags.contains(BuiltinFlag::StartsInstance);
    bool isKnown = !value.mayBeObject() && !(value.primitives & ~Primitive::LiteralString);

    bool givesNameToHost = false;
    Vector<StringID, 4> names(solver.values().stringsOf(value.strings));
    for (StringID name : names) {
        ModuleID module = findModuleNamed(solver.program(), name);
        if (!module)
            isKnown = false;
        else if (startsInstance) {
            didRequestModule(solver, request.site.instance, module);
            evaluateModule(solver, module, witness);
        } else {
            givesNameToHost = true;
            solver.records().addModuleLoad({ request.site.instance, request.origin, module });
            requireModule(solver, load, module);
        }
    }
    // A realm starts with an entry point of the build, and each of those is a root. The run time refuses every other name, and nothing of this realm is handed over but copies of data.
    if (startsInstance)
        return;
    if (!isKnown)
        loadByUnknownName(solver, load, call.builtin, { FlowModuleAnswer::RequiredValue, FlowModuleAnswer::Namespace });
    // The call stays a call, so the host gets the name. The run time takes names from the whole program or from nobody.
    else if (givesNameToHost)
        raiseUnseenCode(solver, UnseenCodeCause::LoadsModuleByName, call.builtin, witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
