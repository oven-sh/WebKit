/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

#include <array>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static FlowCallRequest accessorRequestOf(const FlowAccess& access, FlowObjectID receiver)
{
    FlowCallRequest request { access.site, access.origin, FlowCallLinkKind::Accessor, BuiltinCallbackTimes::Once, access.flags.contains(FlowAccessFlag::IsInCycle), { } };
    request.operands.thisValue = access.thisValue ? access.thisValue : access.receiver;
    request.operands.thisObject = access.thisValue ? FlowObjectID() : receiver;
    request.operands.thrown = access.thrown;
    return request;
}

static void passAccessorValue(FlowCallRequest& request, MethodKind kind, const std::array<FlowNodeID, 1>& value)
{
    if (kind == MethodKind::Setter)
        request.operands.arguments = value;
    else
        request.operands.result = value[0];
}

static void callAccessorFunctions(const FlowAccessContext& context, FlowObjectID receiver, FlowNodeID accessors, MethodKind kind)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;

    std::array<FlowNodeID, 1> value { access.value };
    FlowCallRequest request = accessorRequestOf(access, receiver);
    passAccessorValue(request, kind, value);
    callValuesOf(solver, request, accessors);
}

static FlowNodeID connectBuiltinAccessorValue(FlowSolver& solver, const FlowAccess& access, MethodKind kind, uint32_t accessorIndex)
{
    FlowNodeID value = solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::BuiltinAccessorValue, accessorIndex));
    if (kind == MethodKind::Getter) {
        solver.connect(value, access.value);
        return value;
    }

    if (access.value)
        solver.connect(access.value, value);
    else
        solver.joinPrimitives(value, Primitive::Undefined);
    return value;
}

static void callBuiltinAccessor(const FlowAccessContext& context, FlowObjectID receiver, FlowCallRequest request, BuiltinID accessor, MethodKind kind)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    BuiltinCallMode mode = kind == MethodKind::Setter ? BuiltinCallMode::Set : BuiltinCallMode::Get;

    if (!access.site.instance || !receiver) {
        std::array<FlowNodeID, 1> value { access.value };
        passAccessorValue(request, kind, value);
        callBuiltin(solver, request, accessor, mode);
        return;
    }

    uint32_t accessorIndex = accessor.index() * 2 + (kind == MethodKind::Setter);
    std::array<FlowNodeID, 1> value { connectBuiltinAccessorValue(solver, access, kind, accessorIndex) };
    passAccessorValue(request, kind, value);
    callBuiltin(solver, request, accessor, mode);
}

// The table describes a data property by a getter that is typed on `this`. A data property answers for the object that holds it, whoever the lookup started at. A primitive holds its own.
static bool readsDataPropertyOfObject(const FlowSolver& solver, FlowObjectID receiver, const FlowPropertySource& source, MethodKind kind)
{
    return kind == MethodKind::Getter && receiver && source.holder && source.key && solver.builtins().at(source.builtin).kind == BuiltinKind::DataProperty;
}

static void callBuiltinAccessor(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source, MethodKind kind)
{
    FlowCallRequest request = accessorRequestOf(context.access, receiver);
    if (!readsDataPropertyOfObject(context.solver, receiver, source, kind)) {
        callBuiltinAccessor(context, receiver, request, source.builtin, kind);
        return;
    }
    forEachHolderOfInstanceProperty(context.solver, receiver, source.key, source.holder, context.access.site, [&](FlowObjectID holder) {
        FlowCallRequest requestOfHolder = request;
        if (requestOfHolder.operands.thisObject != holder) {
            requestOfHolder.operands.thisValue = { };
            requestOfHolder.operands.thisObject = holder;
        }
        callBuiltinAccessor(context, receiver, requestOfHolder, source.builtin, kind);
    });
}

static FlowObjectID ensureMemberObject(FlowSolver& solver, const FlowPropertySource& source, const Builtin& member)
{
    if (member.scope == BuiltinScope::Module && source.holder && solver.objects().kindOf(source.holder) == FlowObjectKind::ModuleBuiltin)
        return ensureModuleBuiltinObject(solver, source.builtin, solver.objects().at(source.holder).module);
    return ensureBuiltinObject(solver, source.builtin);
}

static std::optional<FlowValue> valueOfConstant(FlowSolver& solver, BuiltinID constant)
{
    const BuiltinTable& builtins = solver.builtins();
    if (BuiltinName symbol = builtins.nameOfSymbol(constant))
        return solver.values().valueOfSymbol(solver.program().keyOf(symbol));

    std::optional<double> number = builtins.numberOf(constant);
    if (!number)
        return std::nullopt;
    if (isTrackedInteger(*number))
        return solver.values().valueOfIntegers(Range::one(static_cast<int64_t>(*number)));
    return solver.values().valueOfPrimitives(static_cast<uint16_t>(primitiveOfNumber(*number)));
}

static void testSource(const FlowAccessContext& context, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::Value:
        context.solver.noteLocationEvent(source.node, FlowLocationEvent::PresenceObserved);
        return;
    case FlowPropertySourceKind::BuiltinMember:
    case FlowPropertySourceKind::BuiltinGetter:
        noteBuiltinUse(context.solver, source.builtin, BuiltinUse::Tested, context.witness);
        return;
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Getter:
        return;
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void loadProperties(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;
    solver.noteUse(receiver, lookupUseOf(context.keys));

    forEachPropertySource(solver, receiver, context.keys, walkOf(context.access), [&](const FlowPropertySource& source) {
        loadFromSource(context, receiver, source);
    });
}

void loadProperties(const FlowAccessContext& context, FlowPrimitiveReceiver receiver)
{
    FlowSolver& solver = context.solver;
    if ((receiver.primitives & Primitive::AnyString) && context.keys.mayNameIndex())
        solver.joinPrimitives(context.access.value, Primitive::String | Primitive::Undefined);

    forEachPropertySource(solver, receiver, context.keys, walkOf(context.access), [&](const FlowPropertySource& source) {
        loadFromSource(context, FlowObjectID(), source);
    });
}

void testProperties(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;
    solver.noteUse(receiver, lookupUseOf(context.keys));

    forEachPropertySource(solver, receiver, context.keys, walkOf(context.access), [&](const FlowPropertySource& source) {
        testSource(context, source);
    });
}

void testProperties(const FlowAccessContext& context, FlowPrimitiveReceiver receiver)
{
    forEachPropertySource(context.solver, receiver, context.keys, walkOf(context.access), [&](const FlowPropertySource& source) {
        testSource(context, source);
    });
}

static bool holdsFieldsOrVariables(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Allocation:
    case FlowObjectKind::ModuleNamespace:
        return true;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A location that is only read as a callee loses its value once the operations that read it are gone, so a pass has to find each of them from the location.
// The receivers of an access are the objects the lookup starts at: they name neither the layout of a prototype object on which it ends nor the module of a namespace object.
FlowLocationEvent readEventOf(const FlowSolver& solver, const FlowPropertySource& source, FlowReadUse use)
{
    bool isInherited = !source.isOwn && source.holder && holdsFieldsOrVariables(solver.objects().kindOf(source.holder));
    return isInherited ? FlowLocationEvent::ReadAsValue : readEventOf(use);
}

static void loadUnknownValue(const FlowAccessContext& context)
{
    if (!std::exchange(context.hasLoadedUnknownValue, true))
        context.solver.joinUnknown(context.access.value, context.witness);
}

void loadFromSource(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;

    switch (source.kind) {
    case FlowPropertySourceKind::Unknown:
        loadUnknownValue(context);
        passToUnknownAccessor(context, receiver, source);
        return;
    case FlowPropertySourceKind::Absent:
        solver.joinPrimitives(access.value, Primitive::Undefined);
        return;
    case FlowPropertySourceKind::Value:
        solver.noteLocationEvent(source.node, readEventOf(solver, source, context.keys.hasUnnamedKeys() ? FlowReadUse::Value : access.use));
        if (holdsValuesOfUnnamedKeys(solver.graph().kindOf(source.node)) && saysNoMoreThanUnknownSource(solver, source))
            loadUnknownValue(context);
        else
            solver.connect(source.node, access.value);
        return;
    case FlowPropertySourceKind::Getter:
    case FlowPropertySourceKind::BuiltinGetter:
        callAccessor(context, receiver, source);
        return;
    case FlowPropertySourceKind::BuiltinMember:
        joinBuiltinMember(context, source);
        return;
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void joinBuiltinMember(const FlowAccessContext& context, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    const Builtin& member = solver.builtins().at(source.builtin);

    noteBuiltinUse(solver, source.builtin, access.flags.contains(FlowAccessFlag::IsForTypeOf) ? BuiltinUse::Tested : BuiltinUse::Read, context.witness);

    // The holder stands for what it holds and the table does not describe.
    if (!source.key) {
        solver.joinObject(access.value, source.holder);
        return;
    }
    if (isObjectKind(member.kind)) {
        solver.joinObject(access.value, ensureMemberObject(solver, source, member));
        return;
    }

    RELEASE_ASSERT(member.kind == BuiltinKind::Constant);
    if (std::optional<FlowValue> value = valueOfConstant(solver, source.builtin)) {
        solver.join(access.value, *value);
        return;
    }

    FlowPropertySource getter = source;
    getter.kind = FlowPropertySourceKind::BuiltinGetter;
    callAccessor(context, FlowObjectID(), getter);
}

// A store under a key nobody names finds each setter on the chain of each receiver. Native code calls the setters of a holder that it holds with unknown operands,
// which stand for a receiver that it holds and for a value that the store puts into a location of that receiver.
static bool isSetterLeftToNativeCode(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    if (context.access.thisValue || !receiver || !source.holder || context.keys.names.contains(source.key))
        return false;
    const FlowRecords& records = context.solver.records();
    return records.possibleMutations(receiver).contains(ObjectMutation::ReachNativeCode) && records.possibleMutations(source.holder).contains(ObjectMutation::ReachNativeCode);
}

// Whoever defines an accessor through the unknown object hands the function to native code.
static void callAccessorsIn(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source, MethodKind kind)
{
    bool isUnknownAccessor = source.holder == unknownFlowObject
        || saysNoMoreThanUnknownSource(context.solver, source)
        || (kind == MethodKind::Setter && isSetterLeftToNativeCode(context, receiver, source));
    if (!isUnknownAccessor) {
        callAccessorFunctions(context, receiver, source.node, kind);
        return;
    }
    if (kind == MethodKind::Getter)
        loadUnknownValue(context);
    passToUnknownAccessor(context, receiver, source);
}

void callAccessor(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::Getter:
        callAccessorsIn(context, receiver, source, MethodKind::Getter);
        return;
    case FlowPropertySourceKind::Setter:
        callAccessorsIn(context, receiver, source, MethodKind::Setter);
        return;
    case FlowPropertySourceKind::BuiltinGetter:
        callBuiltinAccessor(context, receiver, source, MethodKind::Getter);
        return;
    case FlowPropertySourceKind::BuiltinSetter:
        callBuiltinAccessor(context, receiver, source, MethodKind::Setter);
        return;
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Value:
    case FlowPropertySourceKind::BuiltinMember:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void passToUnknownAccessor(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    const FlowGraph& graph = solver.graph();

    bool isOpenedHolder = source.holder && source.holder != unknownFlowObject && solver.records().possibleMutations(source.holder).contains(ObjectMutation::ReachUnknownJavaScript);
    FlowNodeID sink = isOpenedHolder ? graph.unknownJavaScript() : graph.nativeCode();
    FlowPassedToUnknownAccessor& passed = isOpenedHolder ? context.passedToUnknownJavaScript : context.passedToNativeCode;

    if (!access.thisValue && receiver && canEscapeAsReceiver(solver.objects().kindOf(receiver))) {
        // The solver records the mutation on each object that it adds to the sink, and what has reached unknown JavaScript has reached native code.
        if (!solver.records().possibleMutations(receiver).contains(ObjectMutation::ReachUnknownJavaScript)) {
            if (isOpenedHolder)
                solver.joinObject(sink, receiver, context.witness);
            else
                handToUnknownRecipient(solver, receiver, context.witness);
        }
        // No value is inside never. The edge only says, to whoever follows edges, that the receiver's node is used as the sink is.
        if (!std::exchange(passed.usesOfReceiverNode, true))
            solver.connect(access.receiver, sink, TypeTable::never);
    }
    if (std::exchange(passed.operands, true))
        return;

    if (access.site.instance && access.origin)
        noteUnknownCallee(solver, access.site, access.origin, FlowCallLinkKind::Accessor);
    auto pass = [&](FlowNodeID operand) {
        if (isOpenedHolder)
            solver.connect(operand, sink, { }, context.witness);
        else
            handToUnknownRecipient(solver, operand, context.witness);
    };
    pass(access.thisValue);
    if (isWrite(access.kind))
        pass(access.value);
    solver.joinUnknown(access.thrown, context.witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
