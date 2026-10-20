/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTFlowObjectState.h"
#include "AOTFlowParts.h"
#include "AOTFlowSymbols.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class SymbolResult : uint8_t {
    None,
    Created,
    Registered,
};

constexpr std::array validationFunctions {
    Builtins::CompilerHelpersValidateByPlan,
    Builtins::CompilerHelpersInterpretNode,
    Builtins::CompilerHelpersHoldsRule,
    Builtins::CompilerHelpersCopyOwnProperties,
};

} // anonymous namespace

static_assert(BuiltinOverloadID::absentIndex <= UINT16_MAX);
static_assert(BuiltinTypeID::absentIndex <= UINT16_MAX);
static_assert(BuiltinClassID::absentIndex <= UINT16_MAX);

static void noteRepeatedAllocation(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowObjectID object)
{
    if (object != unknownFlowObject && mayRunRepeatedly(overloadCall.call.request))
        solver.noteEvent(object, FlowObjectEvent::AllocatedRepeatedly, witnessOf(solver, overloadCall.call));
}

static FlowObjectID allocateForCall(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowSiteDescription& description)
{
    FlowObjectID object = allocateAtCallSite(solver, { overloadCall.call.request.site, description });
    noteRepeatedAllocation(solver, overloadCall, object);
    return object;
}

static BuiltinClassID constructedClassOf(const BuiltinTable& builtins, BuiltinTypeID type)
{
    for (const BuiltinTypePart& part : builtins.at(type).parts) {
        if (part.kind == BuiltinTypePartKind::Instance)
            return part.builtinClass;
    }
    return { };
}

static SymbolResult symbolResultOf(const BuiltinTable& builtins, const FlowOverloadCall& overloadCall)
{
    const BuiltinType& type = builtins.at(overloadCall.overload.result.type);
    bool isOnlySymbol = type.flags.isEmpty() && type.parts.empty() && type.stringLiterals.empty()
        && type.primitives == OptionSet<BuiltinPrimitive> { BuiltinPrimitive::Symbol };
    if (!isOnlySymbol)
        return SymbolResult::None;
    if (type.freshness == BuiltinFreshness::Yes)
        return SymbolResult::Created;
    if (overloadCall.call.builtin == Builtins::SymbolFor)
        return SymbolResult::Registered;
    return SymbolResult::None;
}

// A range rule counts on each of the arguments it looks at: the largest of fewer arguments is smaller, and of none it is no integer.
static bool passesEachArgument(const FlowPassedArguments& arguments)
{
    return !arguments.rest && !arguments.positions.contains(FlowNodeID()) && arguments.count.minimum >= static_cast<int64_t>(arguments.positions.size());
}

static Vector<Range, 8> operandIntegerRanges(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinResult& result = overloadCall.overload.result;
    Vector<Range, 8> ranges;
    if (result.rangeRule == BuiltinRangeRule::None || !result.rangeOperand)
        return ranges;

    bool hasOnlyIntegers = passesEachArgument(overloadCall.call.arguments);
    forEachOperandNode(overloadCall.call, *result.rangeOperand, [&](FlowNodeID node) {
        solver.waitOn(node, overloadCall.call.request.site);
        const FlowValue& value = solver.valueOf(node);
        if (value.mayBeObject() || value.primitives != Primitive::Integer)
            hasOnlyIntegers = false;
        else
            ranges.append(solver.values().rangeOf(value.integers));
    });
    if (!hasOnlyIntegers)
        ranges.clear();
    return ranges;
}

static bool rangeRuleApplies(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    return !operandIntegerRanges(solver, overloadCall).isEmpty();
}

static bool waitsForRangeOperand(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinResult& result = overloadCall.overload.result;
    if (result.rangeRule == BuiltinRangeRule::None || !result.rangeOperand)
        return false;

    bool hasUnreachedOperand = false;
    forEachOperandNode(overloadCall.call, *result.rangeOperand, [&](FlowNodeID node) {
        solver.waitOn(node, overloadCall.call.request.site);
        hasUnreachedOperand |= solver.valueOf(node).isEmpty();
    });
    return hasUnreachedOperand;
}

static bool returnsAccessorsOfOperand(const BuiltinOverload& overload)
{
    return std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        bool readsAccessor = action.kind == BuiltinPropertyActionKind::ReadsGetter || action.kind == BuiltinPropertyActionKind::ReadsSetter;
        return readsAccessor && action.value.kind == BuiltinPlaceKind::Result;
    });
}

static bool returnsObjectsOfNativeCell(const BuiltinOverload& overload)
{
    return std::ranges::any_of(overload.flow.transfers, [](const BuiltinTransfer& transfer) {
        return transfer.from.kind == BuiltinPlaceKind::NativeCell && transfer.to.kind == BuiltinPlaceKind::Result;
    });
}

static void joinDeclaredResult(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallRequest& request = call.request;
    FlowNodeID target = request.operands.result;
    Witness witness = witnessOf(solver, call);

    switch (symbolResultOf(solver.builtins(), overloadCall)) {
    case SymbolResult::None:
        break;
    case SymbolResult::Created:
        solver.join(target, valueOfCreatedSymbol(solver, { request.site, request.origin, mayRunRepeatedly(request) }), witness);
        return;
    case SymbolResult::Registered: {
        FlowNodeID key = call.arguments.at(0);
        solver.waitOn(key, request.site);
        if (key)
            solver.join(target, valueOfRegisteredSymbols(solver, solver.valueOf(key)), witness);
        if (!key || !call.arguments.alwaysPasses(0))
            solver.join(target, valueOfRegisteredSymbols(solver, solver.values().valueOfPrimitives(Primitive::Undefined)), witness);
        return;
    }
    }

    if (rangeRuleApplies(solver, overloadCall) || waitsForRangeOperand(solver, overloadCall))
        return;
    // The action joins the functions, and the transfer the objects.
    if (returnsAccessorsOfOperand(overloadCall.overload) || returnsObjectsOfNativeCell(overloadCall.overload)) {
        const BuiltinType& type = solver.builtins().at(overloadCall.overload.result.type);
        if (type.isAnything())
            solver.joinPrimitives(target, Primitive::All, witness);
        else
            joinPrimitivesOfType(solver, type, target, witness);
        return;
    }
    FlowBuiltinTypePath path;
    joinValueOfType(solver, overloadCall, { overloadCall.overload.result.type, target, FlowSiteOrigin::BuiltinResult }, path);
}

static ExpectedResultID expectedResultOf(const FlowSolver& solver, const FlowCallRequest& request)
{
    const Value& operation = solver.operationAt(request.site, request.origin);
    return operation.opcode == Opcode::Call ? operation.expectedResult() : ExpectedResultID();
}

static FlowNodeID unvalidatedValueOf(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinResult& result = overloadCall.overload.result;
    switch (result.typedResultForm) {
    case TypedResultForm::ParsedText:
    case TypedResultForm::ParsedBody:
    case TypedResultForm::Rows:
        return sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::NewValue, .newValueType = result.type });
    case TypedResultForm::ValidatedValue:
        if (FlowNodeID source = sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Operand, .operand = result.typedResultSource }))
            return source;
        [[fallthrough]];
    case TypedResultForm::None:
        return sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Result });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void enterValidationFunctions(FlowSolver& solver, const FlowCallRequest& request, FlowNodeID value)
{
    Witness witness = solver.witnessAt(request.site, request.origin);

    FlowNodeID text = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::MemberValue, value.index()));
    solver.joinPrimitives(text, Primitive::String);

    FlowNodeID operands[] { value, text };
    FlowCallRequest inner {
        request.site,
        request.origin,
        FlowCallLinkKind::BuiltinCallback,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .arguments = operands, .result = request.operands.result, .thrown = request.operands.thrown },
    };
    FlowPassedArguments arguments = expandArguments(solver, inner);
    FlowContext context = contextOf(solver, inner, arguments, FlowObjectSet());

    for (BuiltinID builtin : validationFunctions) {
        noteBuiltinUse(solver, builtin, BuiltinUse::Called, witness);
        for (const BuiltinOverload& overload : solver.builtins().overloadsOf(builtin)) {
            RuntimeFunctionID id = analyzedFunctionOf(overload);
            if (!id)
                continue;
            if (FunctionID function = functionOf(solver, id, builtin, witness))
                linkFunctionCall(solver, { inner, arguments, function, FlowObjectID(), context, FlowInstancePolicy::AlwaysAtCallSite });
        }
    }
}

static Vector<FlowNodeID, 2> claimedValueNodes(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    FlowNodeID result = overloadCall.call.request.operands.result;
    switch (overloadCall.overload.result.typedResultForm) {
    case TypedResultForm::None:
    case TypedResultForm::ValidatedValue:
        return { };
    case TypedResultForm::ParsedText:
        return { result };
    case TypedResultForm::ParsedBody:
    case TypedResultForm::Rows:
        break;
    }

    Vector<FlowNodeID, 2> nodes;
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        if (FlowNodeID resolvedValue = solver.graph().slot(object, BuiltinSlots::PromiseValue))
            nodes.append(resolvedValue);
    });
    if (nodes.isEmpty())
        nodes.append(result);
    return nodes;
}

static TypedResultTarget findClaimedResult(FlowSolver& solver, const FlowCallRequest& request)
{
    const Function& function = solver.functionOf(request.site);
    FunctionID id = solver.graph().at(request.site.instance).function;
    const FlowFunctionUses& functionUses = *solver.callState().builtins.usesOfFunctionsWithTypedResults.ensure(id, [&] {
        solver.charge(request.site, function.values.size());
        return makeUnique<FlowFunctionUses>(function);
    }).iterator->value;
    return findTypedResultTarget(solver.program(), function, functionUses.uses, request.origin);
}

// An operation has one typed result, allocated when the first of its callees is looked at. So what is particular to a callee meets in a node of the operation:
// the values that a validator leaves uncopied flow into one, and the result flows out of the other to wherever a callee puts it.
static FlowNodeID nodeOfOperation(FlowSolver& solver, const FlowCallRequest& request, FlowCallTemporaryRole role)
{
    return solver.temporaryFor(callTemporaryKey({ request.site.instance, { } }, role, request.origin.index()));
}

static void applyExpectedResult(FlowSolver& solver, const FlowCallRequest& request, ExpectedResultID expected, FlowNodeID unvalidatedValue)
{
    const ExpectedResult& expectedResult = solver.program().at(expected);
    FlowNodeID uncopiedValues;
    if (!expectedResult.validation.isEmpty()) {
        FlowNodeID validatedValues = transitiveContentsOf(solver, request.site, unvalidatedValue);
        enterValidationFunctions(solver, request, validatedValues);
        uncopiedValues = nodeOfOperation(solver, request, FlowCallTemporaryRole::UncopiedValuesOfTypedResult);
        solver.connect(validatedValues, uncopiedValues);
    }

    FlowNodeID typedResult = nodeOfOperation(solver, request, FlowCallTemporaryRole::TypedResult);
    solver.connect(typedResult, request.operands.result);
    if (solver.callState().builtins.typedResultTypes.add(std::pair { request.site.instance, request.origin }, expectedResult.type).isNewEntry)
        allocateTypedResult(solver, request.site, request.origin, expected, uncopiedValues, typedResult);
}

static void applyTypedResult(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowCallRequest& request = overloadCall.call.request;
    if (!request.origin || request.kind != FlowCallLinkKind::Call)
        return;

    if (ExpectedResultID expected = expectedResultOf(solver, request)) {
        bool isValidated = !solver.program().at(expected).validation.isEmpty();
        if (isValidated || overloadCall.overload.result.typedResultForm != TypedResultForm::None)
            applyExpectedResult(solver, request, expected, isValidated ? unvalidatedValueOf(solver, overloadCall) : FlowNodeID());
        return;
    }

    Vector<FlowNodeID, 2> nodes = claimedValueNodes(solver, overloadCall);
    if (nodes.isEmpty())
        return;
    FlowNodeID typedResult = nodeOfOperation(solver, request, FlowCallTemporaryRole::TypedResult);
    for (FlowNodeID node : nodes)
        solver.connect(typedResult, node);

    auto& recordedTypes = solver.callState().builtins.typedResultTypes;
    std::pair key { request.site.instance, request.origin };
    if (recordedTypes.contains(key))
        return;
    TypedResultTarget target = findClaimedResult(solver, request);
    recordedTypes.add(key, target.type);
    if (target)
        allocateTypedResult(solver, request.site, request.origin, target.type, target.layouts.span(), typedResult);
}

// A callee that nobody knows may be the built-in that the checker saw, and a call gets a typed parser only where every instance models what the parser allocates.
// A result that is validated is validated whatever was called.
void allocateExpectedResult(FlowSolver& solver, const FlowCallRequest& request)
{
    if (!request.origin || request.kind != FlowCallLinkKind::Call)
        return;

    if (ExpectedResultID expected = expectedResultOf(solver, request))
        applyExpectedResult(solver, request, expected, request.operands.result);
}

static void retainIteratedOperand(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinResult& result = overloadCall.overload.result;
    if (!result.iteratedOperand)
        return;

    const FlowGraph& graph = solver.graph();
    Witness witness = witnessOf(solver, overloadCall.call);
    forEachResultObject(solver, overloadCall, [&](FlowObjectID iterator) {
        BuiltinClassID builtinClass = solver.objects().at(iterator).builtinClass;
        FlowNodeID iterated = builtinClass ? graph.slot(iterator, solver.builtins().findSlot(builtinClass, BuiltinNames::iterated)) : FlowNodeID();
        if (iterated)
            noteIteratorOf(solver, iterator, result.iterationKind);
        forEachOperandNode(overloadCall.call, *result.iteratedOperand, [&](FlowNodeID node) {
            solver.connect(node, iterated ? iterated : graph.nativeCode(), { }, witness);
        });
    });
}

static void connectPrototypeOfResult(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinResult& result = overloadCall.overload.result;
    if (!result.prototypeSource)
        return;

    FlowNodeID prototypes = sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Prototype, .operand = result.prototypeSource });
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        if (object != unknownFlowObject)
            connectCreationPrototype(solver, object, prototypes);
    });
}

static void rejectResultPromises(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    Witness witness = witnessOf(solver, overloadCall.call);
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        if (FlowNodeID reason = solver.graph().slot(object, BuiltinSlots::PromiseReason))
            solver.joinObject(reason, runtimeErrorObject(solver), witness);
    });
}

static void recordCopiedHoles(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    auto recordHolesOf = [&](const BuiltinParameter& parameter) {
        if (!parameter.roles.contains(BuiltinOperandRole::CopiesHolesToResult) || !mayBePassed(call, parameter.operand))
            return;
        forEachOperandNode(call, parameter.operand, [&](FlowNodeID source) {
            forEachResultObject(solver, overloadCall, [&](FlowObjectID result) {
                solver.records().addHoleCopy(source, result);
            });
        });
    };
    if (overloadCall.overload.receiver)
        recordHolesOf(*overloadCall.overload.receiver);
    for (const BuiltinParameter& parameter : overloadCall.overload.parameters)
        recordHolesOf(parameter);
}

void applyResult(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallOperands& operands = call.request.operands;
    const BuiltinOverload& overload = overloadCall.overload;

    if (call.mode == BuiltinCallMode::Construct && (operands.newTarget || operands.newTargetObject))
        allocateForNewTargets(solver, overloadCall, constructedClassOf(solver.builtins(), overload.result.type));
    else {
        joinDeclaredResult(solver, overloadCall);
        applyTypedResult(solver, overloadCall);
    }

    applyRangeRule(solver, overloadCall);
    retainIteratedOperand(solver, overloadCall);
    recordCopiedHoles(solver, overloadCall);
    connectPrototypeOfResult(solver, overloadCall);
    if (overload.throws())
        rejectResultPromises(solver, overloadCall);
}

void applyRangeRule(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    Vector<Range, 8> ranges = operandIntegerRanges(solver, overloadCall);
    if (ranges.isEmpty())
        return;
    Range range = computeResultRange(overloadCall.overload.result.rangeRule, ranges.span());
    solver.join(overloadCall.call.request.operands.result, solver.values().valueOfIntegers(range), witnessOf(solver, overloadCall.call));
}

void forEachResultObject(FlowSolver& solver, const FlowOverloadCall& overloadCall, NOESCAPE const ScopedLambda<void(FlowObjectID)>& functor)
{
    FlowBuiltinValue value { overloadCall.overload.result.type, { }, FlowSiteOrigin::BuiltinResult };
    const BuiltinType& type = solver.builtins().at(value.type);
    for (const BuiltinTypePart& part : type.parts) {
        bool isAllocated = part.kind == BuiltinTypePartKind::Layout || (part.kind == BuiltinTypePartKind::Instance && type.freshness != BuiltinFreshness::No);
        if (isAllocated)
            functor(allocatePart(solver, overloadCall, value, part));
    }
}

void joinValueOfType(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowBuiltinValue& value, FlowBuiltinTypePath& path)
{
    if (!value.target && value.origin == FlowSiteOrigin::BuiltinElement)
        return;

    const FlowCallRequest& request = overloadCall.call.request;
    const BuiltinType& type = solver.builtins().at(value.type);
    Witness witness = witnessOf(solver, overloadCall.call);
    if (type.isAnything()) {
        solver.joinUnknown(value.target, witness);
        return;
    }

    path.append(value.type);
    joinPrimitivesOfType(solver, type, value.target, witness);
    if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction }))
        solver.joinUnknownObject(value.target, witness);
    if (type.flags.contains(BuiltinTypeFlag::HasPlainData)) {
        FlowPlainDataObjects plainData = allocatePlainData(solver, request.site, request.origin);
        for (FlowObjectID object : { plainData.object, plainData.array }) {
            noteRepeatedAllocation(solver, overloadCall, object);
            solver.joinObject(value.target, object, witness);
        }
    }
    for (const BuiltinTypePart& part : type.parts)
        joinPart(solver, overloadCall, value, part, path);
    path.removeLast();
}

void joinPrimitivesOfType(FlowSolver& solver, const BuiltinType& type, FlowNodeID target, const Witness& witness)
{
    FlowValueTable& values = solver.values();
    uint16_t bits = primitivesOf(type);

    bool hasOnlyIntegers = type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || !type.integers.isEmpty();
    if ((bits & Primitive::Integer) && hasOnlyIntegers) {
        bits &= ~Primitive::Number;
        Range range = type.integers.isEmpty() ? Range::full() : Range { type.integers.minimum, type.integers.maximum };
        solver.join(target, values.valueOfIntegers(range), witness);
    }
    if (!type.primitives.contains(BuiltinPrimitive::String)) {
        bits &= ~Primitive::LiteralString;
        for (BuiltinName literal : type.stringLiterals)
            solver.join(target, values.valueOfString(solver.program().stringOf(literal)), witness);
    }
    solver.joinPrimitives(target, bits, witness);
}

static FlowSiteKind siteKindOf(BuiltinFreshness freshness)
{
    switch (freshness) {
    case BuiltinFreshness::Unaliased:
        return FlowSiteKind::Opaque;
    case BuiltinFreshness::UnlessSpecies:
    case BuiltinFreshness::Yes:
        return FlowSiteKind::Instance;
    case BuiltinFreshness::No:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void joinAllocatedPart(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowBuiltinValue& value, const BuiltinTypePart& part, FlowBuiltinTypePath& path)
{
    FlowObjectID object = allocatePart(solver, overloadCall, value, part);
    solver.joinObject(value.target, object, witnessOf(solver, overloadCall.call));
    fillAllocatedPart(solver, overloadCall, value, part, object, path);
}

void joinPart(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowBuiltinValue& value, const BuiltinTypePart& part, FlowBuiltinTypePath& path)
{
    Witness witness = witnessOf(solver, overloadCall.call);
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        if (solver.builtins().at(value.type).freshness == BuiltinFreshness::No) {
            noteInstanceOf(solver, part.builtinClass, witness);
            solver.joinUnknownObject(value.target, witness);
            return;
        }
        joinAllocatedPart(solver, overloadCall, value, part, path);
        return;
    case BuiltinTypePartKind::Layout:
        joinAllocatedPart(solver, overloadCall, value, part, path);
        return;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        joinInstancesOfReceiverClass(solver, overloadCall, value, part);
        return;
    case BuiltinTypePartKind::Builtin:
        solver.joinObject(value.target, ensureBuiltinObject(solver, part.builtin), witness);
        // Each CommonJS module has a module object of its own, and no record says which of them a built-in answers.
        if (solver.builtins().canonicalID(part.builtin) == Builtins::ModuleObject)
            solver.connect(commonJSModuleObjectsNode(solver), value.target, { }, witness);
        return;
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        if (value.operandParts == FlowOperandParts::AreConnected)
            connectOperandPart(solver, overloadCall, part, value.target);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowObjectID allocatePart(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowBuiltinValue& value, const BuiltinTypePart& part)
{
    FlowSiteDescription description;
    description.origin = value.origin;
    description.value = overloadCall.call.request.origin;
    description.ordinal = overloadCall.id.index() << 16 | value.type.index();

    switch (part.kind) {
    case BuiltinTypePartKind::Layout:
        // A tuple is an array with a position for each of its elements, like an array literal.
        if (const BuiltinLayout& layout = solver.builtins().at(part.layout); layout.isTuple) {
            description.kind = FlowSiteKind::Instance;
            description.numberOfPositions = static_cast<uint16_t>(layout.fields.size());
            description.isCreatedDense = true;
            description.builtinClass = BuiltinClasses::Array;
            return allocateForCall(solver, overloadCall, description);
        }
        description.kind = FlowSiteKind::Layout;
        description.layout = solver.program().layoutOf(part.layout);
        return allocateForCall(solver, overloadCall, description);
    case BuiltinTypePartKind::Instance:
        description.kind = siteKindOf(solver.builtins().at(value.type).freshness);
        description.isCreatedDense = part.pristine.contains(PristineAspects::Dense);
        description.builtinClass = part.builtinClass;
        return allocateForCall(solver, overloadCall, description);
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Builtin:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<FlowNodeID, 8> nodesOfSlot(const FlowSolver& solver, FlowObjectID object, BuiltinSlotID slot)
{
    const FlowGraph& graph = solver.graph();
    Vector<FlowNodeID, 8> nodes;
    if (slot == indexedElementSlotOf(solver.builtins(), solver.objects().at(object).builtinClass)) {
        forEachElementPart(graph, object, arrayIndexRange, [&](FlowNodeID part) {
            nodes.append(part);
        });
    } else if (FlowNodeID node = graph.slot(object, slot))
        nodes.append(node);
    return nodes;
}

// Only the first type of a path can stand for the result itself, which is another object than one the result holds.
static bool isBeingFilled(const FlowBuiltinTypePath& path, BuiltinTypeID type)
{
    ASSERT(path.last() == type);
    if (path.size() < 3)
        return false;
    return std::ranges::contains(path.subspan(1, path.size() - 2), type);
}

// A typed array converts what is stored into it, so its class says what its elements are.
static std::optional<Type> findConvertedElements(const FlowSolver& solver, FlowObjectID object, const BuiltinSlot& slot)
{
    if (slot.heap != AbstractHeap::TypedArrayElements)
        return std::nullopt;
    const BuiltinTable& builtins = solver.builtins();
    BuiltinClassID typedArrayClass = builtins.typedArrayClassOf(solver.objects().at(object).builtinClass);
    TypedArrayType typedArrayType = typedArrayClass ? builtins.at(typedArrayClass).typedArrayType : NotTypedArray;
    if (!isTypedView(typedArrayType))
        return std::nullopt;
    return elementTypeOf(typedArrayType);
}

static void joinAnyContents(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowObjectID object, const BuiltinSlot& slot, FlowBuiltinTypePath& path)
{
    std::optional<Type> convertedElements = findConvertedElements(solver, object, slot);
    for (FlowNodeID node : nodesOfSlot(solver, object, slot.id)) {
        if (convertedElements)
            solver.join(node, solver.values().valueOfType(*convertedElements), witnessOf(solver, overloadCall.call));
        else if (slot.holds)
            joinValueOfType(solver, overloadCall, { slot.holds, node, FlowSiteOrigin::BuiltinElement }, path);
        else
            solver.joinUnknown(node, witnessOf(solver, overloadCall.call));
    }
}

// A type that lists no contents for a slot says nothing about them. Only never says that the slot is empty.
static void fillUnlistedContents(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinTypePart& part, FlowObjectID object, FlowBuiltinTypePath& path)
{
    const BuiltinTable& builtins = solver.builtins();
    for (BuiltinSlotID id : builtins.at(part.builtinClass).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.indexInType && !std::ranges::contains(part.contents, id, &BuiltinSlotContents::slot))
            joinAnyContents(solver, overloadCall, object, slot, path);
    }
}

// A typed array is filled with zeros when it is created and converts what is stored, so no transfer says what its elements hold.
static void fillTypedArrayElements(FlowSolver& solver, const FlowOverloadCall& overloadCall, FlowObjectID object)
{
    if (object == unknownFlowObject)
        return;

    const BuiltinTable& builtins = solver.builtins();
    BuiltinSlotID slot = indexedElementSlotOf(builtins, solver.objects().at(object).builtinClass);
    if (!slot || builtins.at(slot).heap != AbstractHeap::TypedArrayElements)
        return;
    FlowBuiltinTypePath path;
    joinAnyContents(solver, overloadCall, object, builtins.at(slot), path);
}

void fillAllocatedPart(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowBuiltinValue& value, const BuiltinTypePart& part, FlowObjectID object, FlowBuiltinTypePath& path)
{
    if (object == unknownFlowObject || isBeingFilled(path, value.type))
        return;

    FlowOperandParts operandPartsOfContents = value.origin == FlowSiteOrigin::BuiltinResult ? value.operandParts : FlowOperandParts::AreConnected;
    for (const BuiltinSlotContents& contents : part.contents) {
        for (FlowNodeID node : nodesOfSlot(solver, object, contents.slot))
            joinValueOfType(solver, overloadCall, { contents.type, node, FlowSiteOrigin::BuiltinElement, operandPartsOfContents }, path);
    }
    if (part.kind != BuiltinTypePartKind::Layout) {
        fillUnlistedContents(solver, overloadCall, part, object, path);
        return;
    }

    const BuiltinLayout& layout = solver.builtins().at(part.layout);
    std::span<const BuiltinLayoutField> fields = layout.fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        FlowNodeID field = layout.isTuple ? solver.graph().position(object, index) : solver.graph().field(object, index);
        joinValueOfType(solver, overloadCall, { fields[index].type, field, FlowSiteOrigin::BuiltinElement, value.operandParts }, path);
        if (fields[index].isOptional)
            solver.joinPrimitives(field, Primitive::Absent);
    }
}

static BuiltinClassID allocatedClassFor(const FlowSolver& solver, FlowObjectID receiver, BuiltinTypePartKind kind)
{
    const FlowObject& record = solver.objects().at(receiver);
    if (record.kind != FlowObjectKind::Instance)
        return { };
    if (kind == BuiltinTypePartKind::TypedArrayOfReceiverElementType)
        return solver.builtins().typedArrayClassOf(record.builtinClass);
    return record.builtinClass;
}

void joinInstancesOfReceiverClass(FlowSolver& solver, const FlowOverloadCall& overloadCall, const FlowBuiltinValue& value, const BuiltinTypePart& part)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallRequest& request = call.request;
    Witness witness = witnessOf(solver, call);

    FlowTemporaryKey cursor = callTemporaryKey(request.site, FlowCallTemporaryRole::ReceiverClassObjects, overloadCall.id.index());
    solver.takeNewObjects(cursor, call.receiver).forEach([&](FlowObjectID receiver) {
        BuiltinClassID builtinClass = allocatedClassFor(solver, receiver, part.kind);
        if (!builtinClass) {
            solver.joinUnknownObject(value.target, witness);
            return;
        }

        FlowSiteDescription description;
        description.kind = FlowSiteKind::Instance;
        description.origin = value.origin;
        description.value = request.origin;
        description.builtinClass = builtinClass;
        description.ordinal = overloadCall.id.index() << 16 | builtinClass.index();
        FlowObjectID instance = allocateForCall(solver, overloadCall, description);
        fillTypedArrayElements(solver, overloadCall, instance);
        solver.joinObject(value.target, instance, witness);
    });
}

static BuiltinID prototypeOfClass(const FlowSolver& solver, BuiltinClassID builtinClass)
{
    return builtinClass ? solver.builtins().at(builtinClass).prototype : BuiltinID();
}

static FlowObjectID allocateSubclassInstance(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinClassID builtinClass, FlowObjectID newTarget, LayoutID layout)
{
    FlowSiteDescription description;
    description.kind = FlowSiteKind::Layout;
    description.origin = FlowSiteOrigin::BuiltinConstructor;
    description.value = overloadCall.call.request.origin;
    description.layout = layout;
    description.builtinClass = builtinClass;

    FlowObjectID instance = allocateForCall(solver, overloadCall, description);
    // The object that stands for every site of a layout does not say who made it.
    if (instance == unknownFlowObject || solver.objects().at(instance).standsForLayout)
        return unknownFlowObject;

    connectPrototypeFromConstructor(solver, overloadCall.call.request, instance, newTarget, prototypeOfClass(solver, builtinClass));
    if (indexedElementSlotOf(solver.builtins(), builtinClass))
        solver.joinUnknown(solver.ensureOpenPart(instance, FlowOpenPart::IndexedProperties), witnessOf(solver, overloadCall.call));
    return instance;
}

// Every test of a built-in class takes an allocation whose layout has no heritage for an object outside the class, and no allocation for a function.
static bool canStandForInstance(const FlowSolver& solver, LayoutID layout, BuiltinClassID builtinClass)
{
    if (!layout || !solver.program().at(layout).has(LayoutFlags::HasHeritage))
        return false;
    return !builtinClass || !isFunctionClass(solver.builtins(), builtinClass);
}

static bool isCalledBuiltin(const FlowSolver& solver, const FlowBuiltinCall& call, FlowObjectID object)
{
    const FlowObject& record = solver.objects().at(object);
    bool isBuiltin = record.kind == FlowObjectKind::Builtin || record.kind == FlowObjectKind::ModuleBuiltin;
    return isBuiltin && record.builtin == call.builtin;
}

static void allocateForOtherNewTarget(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinClassID builtinClass, FlowObjectID newTarget)
{
    const FlowBuiltinCall& call = overloadCall.call;
    FlowNodeID target = call.request.operands.result;
    Witness witness = witnessOf(solver, call);
    FlowObject record = solver.objects().at(newTarget);

    if (record.kind == FlowObjectKind::Closure) {
        LayoutID layout = solver.program().at(record.function).constructedClass;
        FlowObjectID instance = canStandForInstance(solver, layout, builtinClass) ? allocateSubclassInstance(solver, overloadCall, builtinClass, newTarget, layout) : unknownFlowObject;
        if (instance != unknownFlowObject) {
            solver.joinObject(target, instance, witness);
            return;
        }
    }

    // new.target is any constructor: a closure, a bound function, a proxy, another built-in constructor. The new object inherits from what it holds as its prototype.
    connectPrototypeFromConstructor(solver, call.request, unknownFlowObject, newTarget, prototypeOfClass(solver, builtinClass));
    solver.joinUnknownObject(target, witness);
}

void allocateForNewTargets(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinClassID builtinClass)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallRequest& request = call.request;
    const FlowCallOperands& operands = request.operands;
    auto isCalled = [&](FlowObjectID newTarget) {
        return isCalledBuiltin(solver, call, newTarget);
    };

    // The declared result takes from cursors of its own, so every run reaches it.
    bool constructsCalledBuiltin = operands.newTargetObject ? isCalled(operands.newTargetObject) : solver.valueOf(operands.newTarget).objects.anyOf(isCalled);
    if (constructsCalledBuiltin)
        joinDeclaredResult(solver, overloadCall);

    if (operands.newTargetObject) {
        if (!constructsCalledBuiltin)
            allocateForOtherNewTarget(solver, overloadCall, builtinClass, operands.newTargetObject);
        return;
    }

    // An overload that is selected late allocates for the new targets that came before.
    FlowTemporaryKey cursor = callTemporaryKey(request.site, FlowCallTemporaryRole::NewTargets, overloadCall.id.index());
    solver.forEachMember(solver.takeNewObjects(cursor, operands.newTarget), [&](FlowObjectID newTarget) {
        if (!isCalled(newTarget))
            allocateForOtherNewTarget(solver, overloadCall, builtinClass, newTarget);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
