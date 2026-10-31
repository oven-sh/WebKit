/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBlockCycles.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTCFG.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCreateBoundFunctions.h"
#include "AOTDominators.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfRules.h"
#include "AOTFlowApply.h"
#include "AOTFlowPrototypes.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTInlineCopy.h"
#include "AOTLayout.h"
#include "AOTMethodDispatch.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTResolveAccesses.h"
#include "AOTResultRules.h"
#include "AOTSinkAllocations.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestSite.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTWrittenFunction.h"
#include "Interpreter.h"
#include <algorithm>
#include <array>
#include <expected>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/Function.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::TypeTest::value == testedOperandIndex);

bool admitsUnlistedFunctions(const Type& type)
{
    return type.anyFunction || type.anyObject;
}

static ValueID findStrongestLink(const Program& program, const Function& function, ValueID callee)
{
    ValueID strongest;
    for (ValueID id : CastChain(function, callee)) {
        const Type& type = program.typeOf(function, id);
        if (type.functions.isEmpty() || type.anyFunction || type.anyObject || type.mayBeUnspecified())
            continue;
        const Type* current = strongest ? &program.typeOf(function, strongest) : nullptr;
        if (!current || (type.functions.size() != current->functions.size() ? type.functions.size() < current->functions.size() : type != *current && type.isSubsetOf(*current, program)))
            strongest = id;
    }
    return strongest ? strongest : callee;
}

CalleeCandidates calleeCandidates(const Program& program, const Function& function, ValueID callee)
{
    CalleeCandidates result;
    result.value = findStrongestLink(program, function, callee);

    const Type& type = program.typeOf(function, result.value);
    result.mayBeNullish = type.mayBeUndefinedOrNull();
    if (!type.functions.isEmpty()) {
        result.listByType(type.functions.span(), (type.primitives & ~Primitive::UndefinedOrNull) || type.anyFunction || type.anyObject || type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty());
        return result;
    }
    if (!admitsUnlistedFunctions(type))
        return result;

    for (ValueID id : CastChain(function, callee)) {
        TypeID expected = function.at(id).expected().predicted();
        if (expected && !program.at(expected).functions.isEmpty()) {
            result.expectListOfProgram(program.at(expected).functions.span());
            break;
        }
    }
    return result;
}

static CalleeCandidates calleeCandidatesOf(const Program& program, const Function& function, const Value& operation, ValueID callee)
{
    CalleeCandidates result = calleeCandidates(program, function, callee);
    if (result.isProven() || !admitsUnlistedFunctions(program.typeOf(function, result.value)))
        return result;
    if (!Value::opcodesWithCallees.contains(operation.opcode) || !operation.callees())
        return result;
    // An incomplete stamp lists the functions that flow saw arrive at the operation, among whatever else arrives there.
    std::span<const FunctionID> seen = program.at(operation.callees()).seen().predicted().functions;
    if (result.proof() == CalleeProof::Unknown) {
        result.expectListOfProgram(seen);
        return result;
    }
    // The declared type of a variable names its initializer. A constructor that is swapped later (`Object.assign(allocator, other)`) is only in flow's list.
    if (operation.opcode == Opcode::Construct) {
        FunctionList seenOrDeclared(seen);
        for (FunctionID expected : result.expected().predicted())
            seenOrDeclared.add(expected);
        result.expect(WTF::move(seenOrDeclared));
    }
    return result;
}

// Flow counts a function among the callers of whatever it has followed to one of the calls in it. What flow says is stored ahead of every caller of a function holds in those, and a test may pass in another.
// The function with the arms for many places lists what its places have shown, by flow or by a type. Flow has followed nothing to the call in it: isCallWithSharedArms().
static bool isFollowedTo(const Program& program, const Value& operation, FunctionID callee)
{
    if (!Value::opcodesWithCallees.contains(operation.opcode))
        return true;
    return operation.callees() && std::ranges::binary_search(program.at(operation.callees()).followed().value(), callee);
}

// `object.method(..)` where some objects have a property of their own under the name: the ways meet in front of the arguments, one with the closure of the method, the other with what the property holds.
static FunctionList findClosuresArrivingOnWaysOfTheirOwn(FunctionPassContext& context, ValueID callee)
{
    const Function& function = context.function;
    FunctionList closures;
    ValueID joined = sameObjectThroughCasts(function, callee).valueInFrontOfFacts();
    if (function.at(joined).opcode != Opcode::Parameter)
        return closures;
    ValueLocation location = context.analyses.useLists().locationOf(joined);
    if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
        return closures;
    for (Edge edge : context.analyses.cfg().incomingEdges(location.block)) {
        const Target& target = targetOf(function, edge);
        if (edge.isHandlerEdge() || location.index >= target.arguments.size())
            return { };
        const Value& arriving = function.at(sameObjectThroughCasts(function, target.arguments[location.index]));
        if (arriving.opcode == Opcode::NewClosure && function.at(edge.from).frequency.predicted() == BlockFrequency::Normal)
            closures.add(arriving.function());
    }
    return closures;
}

static PropertyKeyID keyOfRead(FunctionPassContext& context, ValueID value)
{
    const Value& read = context.function.at(context.analyses.identityOf(value));
    if (read.opcode == Opcode::PropGet && !read.isFreeName())
        return read.key();
    if (read.opcode != Opcode::ElemGet)
        return { };
    ConstantKey key = constantKeyOf(context.program, context.program.typeOf(context.function, read.operand(OperandIndex::ElemGet::property)));
    if (key.kind != ConstantKeyKind::Text)
        return key.symbol;
    // Flow has made a key of every text that it has followed.
    return context.program.types.find({ .kind = PropertyKeyKind::Text, .text = key.text }).value_or(PropertyKeyID());
}

// `table[type]` in a copy of a function whose caller spells the type. Where the table itself is found on a likely and on a rare way, each way has a read of its own, and they meet in front of the call.
static PropertyKeyID keyThatCalleeWasReadUnder(FunctionPassContext& context, ValueID callee)
{
    const Function& function = context.function;
    ValueID joined = context.analyses.identityOf(callee);
    if (function.at(joined).opcode != Opcode::Parameter)
        return keyOfRead(context, joined);
    ValueLocation location = context.analyses.useLists().locationOf(joined);
    if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
        return { };
    PropertyKeyID key;
    for (Edge edge : context.analyses.cfg().incomingEdges(location.block)) {
        const Target& target = targetOf(function, edge);
        if (edge.isHandlerEdge() || location.index >= target.arguments.size())
            return { };
        PropertyKeyID keyOnThisWay = keyOfRead(context, target.arguments[location.index]);
        if (!keyOnThisWay || (key && key != keyOnThisWay))
            return { };
        key = keyOnThisWay;
    }
    return key;
}

// Also expects the bound function where flow saw the result of one kind of bind reach the call.
// It stops expecting the bind's target and every function that an earlier test has ruled out: `this.m = this.m.bind(this)` hides the method behind the result.
static CalleeCandidates calleeCandidatesOf(FunctionPassContext& context, const Value& operation, ValueID callee, BlockID block)
{
    const Program& program = context.program;
    const FlowResults& flow = context.inputs.flow;
    CalleeCandidates result = calleeCandidatesOf(program, context.function, operation, callee);
    if (operation.opcode != Opcode::Call || !operation.accessSite() || !flow.hasResults())
        return result;
    if (result.isProven() || !admitsUnlistedFunctions(program.typeOf(context.function, result.value)))
        return result;
    if (context.function.at(block).frequency.predicted() == BlockFrequency::Normal && !context.function.runsOnlyBehindFailedTest()) {
        FunctionList closures = findClosuresArrivingOnWaysOfTheirOwn(context, result.value);
        // The arms cost bytes. Code that seldom runs keeps the one call of the value.
        if (closures.size() == 1 && !isCold(heatClassOf(context, block)).predicted()) {
            context.planAsksAboutPlace = true;
            result.expect(WTF::move(closures));
            result.arriveOnWaysOfTheirOwn = true;
            return result;
        }
    }
    // What was stored under this one key says more than a declared type, which lists what lies under any key.
    if (PropertyKeyID key = keyThatCalleeWasReadUnder(context, result.value)) {
        std::span<const FlowClosureStoredUnderKey> stored = flow.closuresSeenStoredUnder(key).predicted();
        if (!stored.empty()) {
            context.planAsksAboutPlace = true;
            FunctionList callees;
            for (const FlowClosureStoredUnderKey& entry : stored)
                callees.add(entry.function);
            result.expect(WTF::move(callees));
            return result;
        }
    }
    std::span<const FlowBoundFunctionAtSite> seen = flow.boundFunctionsSeenAt(operation.accessSite(), FlowBoundFunctionUseAtSite::Called).predicted();
    if (seen.size() != 1 || !canCreateBoundFunction(program, seen[0].target))
        return result;
    FunctionID bound = context.derivedFunction(derivedFunctionKey(FunctionRole::BoundFunction, seen[0].target, seen[0].numberOfBoundArguments));

    context.planAsksAboutPlace = true;
    FunctionList ruledOut;
    ruledOut.add(seen[0].target);
    FunctionList expected(result.expected().predicted());
    for (FunctionID member : expected) {
        if (context.analyses.findEarlierTest(makeTypeTest(result.value, context.types.intern(Type::forFunction(member))), block) == EarlierTest::Failed)
            ruledOut.add(member);
    }
    expected.subtract(ruledOut);
    expected.add(bound);
    result.expect(WTF::move(expected));
    return result;
}

static ValueID findClosureCreation(const Function& function, ValueID closure, FunctionID closureFunction)
{
    ValueID definition = sameObjectThroughCasts(function, closure).valueInFrontOfFacts();
    const Value& value = function.at(definition);
    return value.opcode == Opcode::NewClosure && value.function() == closureFunction ? definition : ValueID();
}

static bool isValueInside(const ValueInsertion& insertion, ValueID value, TypeID type)
{
    const Program& program = insertion.context.program;
    return program.typeOf(insertion.function, value).isSubsetOf(program.at(type), program);
}

static TypeID requiredTypeOf(const ValueInsertion& insertion, const DeclaredType& target)
{
    // The callee may be a function that another task has made, with types of that task.
    return insertion.types.adopt(TypingContext { insertion.context.program, insertion.function, insertion.types }.typeIDOf(target));
}

static ValueID insertWithOrigin(const ValueInsertion& insertion, Value&& value)
{
    ValueID id = insertion.insert(WTF::move(value));
    insertion.function.edit(id, ValueEdit::Definition).setOrigin(insertion.origin);
    return id;
}

static auto withInsertionAtEnd(IRBuilder& builder, const auto& functor)
{
    auto append = [&](auto&& value) { return builder.append(WTF::move(value)); };
    InsertValue insert(append);
    return functor(ValueInsertion { builder.context(), builder.function(), builder.types(), builder.origin(), insert });
}

// Whoever asks knows that the value has the type, where the types of the operations do not show it: flow narrows a parameter from every call that reaches it, and a pass gives what the operation it replaces gave.
ValueID convertTo(const ValueInsertion& insertion, ValueID value, const DeclaredType& target)
{
    TypeID required = requiredTypeOf(insertion, target);
    if (isValueInside(insertion, value, required))
        return value;
    if (insertion.context.build.validates())
        insertWithOrigin(insertion, makeValidateType(value, required));
    return insertWithOrigin(insertion, makeAssumeType(value, Proven::byRulesWithoutChecks(required)));
}

// Flow narrows a parameter from every call that it has followed to the function, so what such a call hands over has the type wherever the value is at hand. A callee that flow has not followed here says
// nothing of the value: it has the type where the call stands, behind the test that has found the callee.
static ValueID convertWhereCallStands(const ValueInsertion& insertion, ValueID value, const DeclaredType& target, ValueID& callStandsHere)
{
    TypeID required = requiredTypeOf(insertion, target);
    if (isValueInside(insertion, value, required))
        return value;
    if (insertion.context.build.validates())
        insertWithOrigin(insertion, makeValidateType(value, required));
    if (!callStandsHere)
        callStandsHere = insertWithOrigin(insertion, IRBuilder::make(Opcode::Specified));
    return insertWithOrigin(insertion, makeAssumeType(value, callStandsHere, Proven::behindFact(required)));
}

ValueID convertTo(IRBuilder& builder, ValueID value, const DeclaredType& target)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) { return convertTo(insertion, value, target); });
}

// The list has no place among the declared parameters: what flow has proven of it is assumed at the head of the callee. Who makes an array predicts its storage from its elements as its readers do.
static TypeID provenElementTypeOfHiddenRest(const Program& program, const Function& callee)
{
    std::optional<unsigned> index = callee.hiddenRestParameterIndex();
    if (!index || !callee.hasBody())
        return TypeTable::anything;
    const Block& entry = callee.at(entryBlock);
    for (ValueID id : entry.values) {
        const Value& value = callee.at(id);
        if (value.opcode != Opcode::AssumeType || value.valueOperand() != entry.parameters[*index])
            continue;
        if (const ClassPart* part = program.at(value.provenType()).findClassPart(BuiltinClasses::Array); part && part->contentsAreProven)
            return *part->provenContentsAt(program.builtins(), ContentsSlot::Element);
    }
    return TypeTable::anything;
}

Value makeDirectCall(const ValueInsertion& insertion, const DirectCallRequest& request)
{
    const Program& program = insertion.context.program;
    const Function& callee = program.at(request.callee);
    RELEASE_ASSERT(request.captures.size() == callee.captures.size());
    RELEASE_ASSERT(request.calleeValue || !callee.has(FunctionFlags::TakesCallee));
    RELEASE_ASSERT(request.calleeKeptInFrame || !callee.capabilities.contains(FunctionCapabilities::RecordsCallee) || callee.capabilities.contains(FunctionCapabilities::RecordsCalleeOnceRenamed) || insertion.function.role == FunctionRole::GenericEntrypoint || request.callsOwnFunctionObject);
    RELEASE_ASSERT(!request.restArray || callee.has(FunctionFlags::HasRestParameter));

    auto insert = [&](Value&& value) { return insertWithOrigin(insertion, WTF::move(value)); };
    ValueID callStandsHere;
    auto convert = [&](ValueID value, const DeclaredType& target) {
        if (!value)
            value = insert(IRBuilder::make(Opcode::Undefined));
        return request.calleeIsPredicted ? convertWhereCallStands(insertion, value, target, callStandsHere) : convertTo(insertion, value, target);
    };
    size_t numberOfPositionalParameters = callee.numberOfPositionalParameters();
    auto insertArrayOfOtherArguments = [&](TypeID contents, bool convertsElements) {
        RELEASE_ASSERT(!request.restArray);
        Vector<ValueID, 8> elements;
        for (ValueID element : request.arguments.subspan(std::min(numberOfPositionalParameters, request.arguments.size())))
            elements.append(convertsElements ? convert(element, DeclaredType(Proven::byPosition(contents))) : element);
        Value array = IRBuilder::make(Opcode::NewArray);
        array.setContents(contents);
        array.setElementsOperands(elements.span());
        return insert(WTF::move(array));
    };
    auto restArgument = [&] {
        TypeID required = requiredTypeOf(insertion, callee.parameters.last());
        ValueID rest = request.restArray;
        if (!rest) {
            const ClassPart* part = program.at(required).findClassPart(BuiltinClasses::Array);
            rest = insertArrayOfOtherArguments(part ? part->writtenContentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything, part && part->contentsAreProven);
        }
        if (!isValueInside(insertion, rest, required))
            return convert(rest, callee.parameters.last());
        if (insertion.function.at(rest).type == required)
            return rest;
        Value upcast = IRBuilder::make(Opcode::Upcast, { rest });
        upcast.setCastType(required);
        return insert(WTF::move(upcast));
    };
    auto hiddenRest = [&] {
        TypeID contents = provenElementTypeOfHiddenRest(program, callee);
        return insertArrayOfOtherArguments(contents, contents != TypeTable::anything);
    };
    auto argumentCount = [&] {
        RELEASE_ASSERT(!request.restArray);
        Value count = IRBuilder::make(Opcode::Integer);
        count.setInteger(static_cast<int64_t>(request.arguments.size()));
        return insert(WTF::move(count));
    };

    Arguments all;
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    all.reserveInitialCapacity(numberOfEntryParameters);
    size_t nextCapture = 0;
    size_t nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        switch (callee.entryParameterKind(index)) {
        case EntryParameter::Capture:
            all.append(convert(request.captures[nextCapture], callee.captures[nextCapture]));
            ++nextCapture;
            break;
        case EntryParameter::This:
            all.append(convert(request.thisValue, callee.thisType));
            break;
        case EntryParameter::Callee:
            all.append(request.calleeValue);
            break;
        case EntryParameter::NewTarget:
            all.append(request.newTarget ? request.newTarget : insert(IRBuilder::make(Opcode::Undefined)));
            break;
        case EntryParameter::ArgumentCount:
            all.append(request.argumentCount ? request.argumentCount : argumentCount());
            break;
        case EntryParameter::Parameter:
            if (nextParameter < numberOfPositionalParameters)
                all.append(convert(nextParameter < request.arguments.size() ? request.arguments[nextParameter] : ValueID(), callee.parameters[nextParameter]));
            else
                all.append(restArgument());
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            all.append(request.hiddenRest ? request.hiddenRest : hiddenRest());
            break;
        }
    }

    if (request.calleeKeptInFrame)
        insert(IRBuilder::make(Opcode::KeepCalleeInFrame, { request.calleeKeptInFrame }));
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(request.callee);
    call.setArgumentsOperands(all.span());
    if (request.accessSite)
        call.setAccessSite(request.accessSite);
    call.setCalledBuiltin(request.calledBuiltin);
    call.setIsConstruction(request.isConstruction);
    call.setIsNeverInlined(request.isNeverInlined);
    call.setIsAlwaysInlined(request.isAlwaysInlined);
    call.setCallsOwnFunctionObject(request.callsOwnFunctionObject);
    call.setCalleeIsKeptInFrame(!!request.calleeKeptInFrame);
    call.setCalleeIsPredicted(request.calleeIsPredicted);
    call.setOrigin(insertion.origin);
    call.isTailCall |= request.isTailCall;
    return call;
}

ValueID appendDirectCall(IRBuilder& builder, const DirectCallRequest& request)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) { return builder.append(makeDirectCall(insertion, request)); });
}

static void appendCalleeValidation(IRBuilder& builder, ValueID callee)
{
    const Function& function = builder.function();
    if (builder.context().build.validates())
        builder.append(makeValidateCallee(sameObjectBehindFacts(function, callee), function.at(callee).type));
}

static Vector<ValueID, 8> insertCaptures(const ValueInsertion& insertion, FunctionID function, ValueID closure)
{
    Vector<ValueID, 8> captures;
    uint32_t numberOfCaptures = insertion.context.program.at(function).captures.size();
    for (uint32_t index = 0; index < numberOfCaptures; ++index) {
        Value capture = IRBuilder::make(Opcode::GetCapture, { closure });
        capture.setFunction(function);
        capture.setIndex(index);
        captures.append(insertWithOrigin(insertion, WTF::move(capture)));
    }
    return captures;
}

Vector<ValueID, 8> appendCaptures(IRBuilder& builder, FunctionID function, ValueID closure)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) { return insertCaptures(insertion, function, closure); });
}

void emitReturnOfCall(IRBuilder& builder, ValueID result)
{
    if (builder.context().program.typeOf(builder.function(), result).isNever())
        builder.emitUnreachable();
    else
        builder.emitReturn(result);
}

TailCallRequirement tailCallRequirementOf(const Function& body)
{
    bool hasRequiredTailCall = std::ranges::any_of(body.blocks, [](const Block& block) {
        return block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.requirement() == TailCallRequirement::Required;
    });
    return hasRequiredTailCall ? TailCallRequirement::Required : TailCallRequirement::Allowed;
}

static bool canBeConstructTarget(const Function& constructor)
{
    return constructor.hasBody() && !constructor.capabilities.contains(FunctionCapabilities::RecordsCallee) && (constructor.role == FunctionRole::Source || constructor.role == FunctionRole::Library);
}

static bool canPassRestArguments(const Program& program, const Function& callee)
{
    if (!callee.has(FunctionFlags::HasRestParameter))
        return true;
    if (callee.parameters.isEmpty())
        return false;
    const DeclaredType& declared = callee.parameters.last();
    if (declared.isExpected())
        return true;
    const Type& rest = program.at(*declared.established());
    return rest.anyObject || rest.findClassPart(BuiltinClasses::Array);
}

bool keepsCalleeOfAccessor(const Program& program, FunctionID accessor)
{
    return !mayBeCalledDirectly(program, accessor) || recordsCalleeOnlyOnceRenamed(program, accessor);
}

bool mayBeCalledDirectly(const Program& program, FunctionID id)
{
    return !program.at(id).capabilities.contains(FunctionCapabilities::RecordsCallee) && mayBeCalledDirectlyOrThroughEntrypoint(program, id);
}

bool mayBeCalledDirectlyWhileFunctionNamesAreUnchanged(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    if (!callee.capabilities.contains(FunctionCapabilities::RecordsCallee))
        return mayBeCalledDirectlyOrThroughEntrypoint(program, id);
    // The tests at the head of a function stay where the entrypoint alone enters them.
    return callee.capabilities.contains(FunctionCapabilities::RecordsCalleeOnceRenamed) && callee.role != FunctionRole::EntryChecks && mayBeCalledDirectlyOrThroughEntrypoint(program, id);
}

// A function that can leave by a tail call may have another of its function objects run above the call: the function object that the caller keeps in its own frame is not the one that runs there.
static bool canLeaveByTailCall(const Program& program, FunctionID id)
{
    FunctionID source = id;
    while (FunctionID next = program.at(source).derivation.source)
        source = next;
    return !program.at(source).has(FunctionCapabilities::MakesNoTailCall);
}

bool mayBeCalledDirectlyWhereCalleeIsKept(const Program& program, FunctionID id)
{
    if (mayBeCalledDirectlyWhileFunctionNamesAreUnchanged(program, id))
        return true;
    return !canLeaveByTailCall(program, id) && program.at(id).role != FunctionRole::EntryChecks && mayBeCalledDirectlyOrThroughEntrypoint(program, id);
}

bool mayBeCalledDirectlyOrThroughEntrypoint(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    FunctionKind kind = callee.kind;
    if (!callee.hasBody() || !canBeCalledByValueOf(callee.role) || !(kind == FunctionKind::Plain || kind == FunctionKind::Arrow || kind == FunctionKind::Method || kind == FunctionKind::Getter || kind == FunctionKind::Setter))
        return false;
    return !callee.isUnsplitCoroutine() && callee.results.size() == 1 && canPassRestArguments(program, callee);
}

bool canCallWithoutClosure(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    // Its frames are named by its function object.
    if (isAccessor(callee) && recordsCalleeOnlyOnceRenamed(program, id))
        return false;
    return callee.captures.isEmpty() && !callee.has(FunctionFlags::TakesCallee) && mayBeCalledDirectly(program, id);
}

static bool canCallAccessorOfMember(const Program& program, const MemberAnswer& answer, FunctionID accessor)
{
    if (canCallWithoutClosure(program, accessor))
        return true;
    return answer.holder && isEvaluatedOnce(program, program.at(answer.holder)) && mayBeCalledDirectlyWhereCalleeIsKept(program, accessor);
}

static bool paysToReadClosureOfMember(FunctionPassContext& context, BlockID block)
{
    return context.function.at(block).frequency.predicted() == BlockFrequency::Normal && !context.function.runsOnlyBehindFailedTest() && isHot(heatClassOf(context, block)).predicted();
}

static Value makeReadOfClosureOfMember(TypeArena& types, ValueID receiver, PropertyKeyID key, FunctionID accessor, bool isSetter)
{
    Value read = IRBuilder::make(Opcode::GetAccessorOfMember, { receiver });
    read.setKey(key);
    read.setValueType(types.intern(Type::forFunction(accessor)));
    read.setIsSetter(isSetter);
    return read;
}

bool canConstructDirectly(const Program& program, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!canBeConstructTarget(constructor) || constructor.constructorForm() == ConstructorForm::NotConstructor)
        return false;
    return constructor.results.size() == 1 && constructor.constructedClass && canPassRestArguments(program, constructor);
}

bool canConstructWithoutClosure(const Program& program, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!canBeConstructTarget(constructor) || constructor.kind != FunctionKind::BaseConstructor)
        return false;
    if (!constructor.captures.isEmpty() || constructor.hasAny({ FunctionFlags::TakesCallee, FunctionFlags::HasThis }))
        return false;
    return constructor.results.size() == 1 && canPassRestArguments(program, constructor);
}

static constexpr OpcodeSet validationsOfValues { Opcode::ValidateType, Opcode::ValidateCallee };
static constexpr OpcodeSet readsOfMembers { Opcode::PropGet, Opcode::SuperGet };

static bool canSinkToCall(FunctionPassContext& context, std::span<const ValueSite> links, const ValueSite& call)
{
    if (std::ranges::any_of(links, [&](const ValueSite& link) { return link.block != call.block; }))
        return false;

    // A read that is proven to find methods walks the chain and runs nobody's code. Any other may run a getter.
    const BlockValues& values = context.function.at(call.block).values;
    for (const ValueSite& read : links) {
        if (!readsOfMembers.contains(context.function.at(read.value).opcode))
            continue;
        Effects effectsOfRead = context.analyses.effectsOf(read.value).normal;
        for (uint32_t index = read.index + 1; index < call.index; ++index) {
            ValueID id = values[index];
            if (!std::ranges::contains(links, id, &ValueSite::value) && !validationsOfValues.contains(context.function.at(id).opcode) && context.analyses.effectsOf(id).normal.interferesWith(effectsOfRead) && !context.build.isSabotaged(Sabotage::ReadsOfCalleesSinkPastWrites))
                return false;
        }
    }
    return true;
}

// findSingleUseCastChain(), but for the operations of a validating build: they use a value and keep nothing of it, so that such a build resolves the calls that any other build resolves.
// The chain goes in front of the links that the read has.
static bool findCastChainUsedByCallAlone(const Function& function, const UseLists& useLists, ValueID user, MemberRead& read)
{
    for (ValueID id : CastChain(function, function.at(user).operand(0))) {
        bool isUsedByUser = false;
        for (const Use& use : useLists.usesOf(id)) {
            if (use.kind == Use::Kind::Operand && validationsOfValues.contains(function.at(use.user).opcode)) {
                read.validations.append(use.user);
                continue;
            }
            if (isUsedByUser || use.kind != Use::Kind::Operand || use.user != user || use.operandIndex)
                return false;
            isUsedByUser = true;
        }
        if (!isUsedByUser)
            return false;
        ValueLocation location = useLists.locationOf(id);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        read.links.insert(0, ValueSite { location.block, location.index, id });
        user = id;
    }
    return true;
}

static MemberRead findLinksOfMemberRead(FunctionPassContext& context, const ValueSite& call)
{
    static_assert(!OperandIndex::Call::callee, "findCastChainUsedByCallAlone starts at operand 0 of its user.");
    const Function& function = context.function;
    RELEASE_ASSERT(function.at(call.value).opcode == Opcode::Call);

    const Value& source = function.at(CastChain(function, function.at(call.value).calleeOperand()).last());
    // A call names the key under which its callee was read from the receiver. `super.run()` reads above the home object, so the call names none and the read says it.
    PropertyKeyID key = source.opcode == Opcode::SuperGet ? source.key() : function.at(call.value).calleeKey();
    if (!key || !readsOfMembers.contains(source.opcode) || source.key() != key)
        return { };

    MemberRead memberRead;
    if (!findCastChainUsedByCallAlone(function, context.analyses.useLists(), call.value, memberRead))
        return { };
    return memberRead;
}

MemberRead findMemberRead(FunctionPassContext& context, const ValueSite& call)
{
    MemberRead memberRead = findLinksOfMemberRead(context, call);
    if (!memberRead.isEmpty())
        memberRead.canSinkToCall = canSinkToCall(context, memberRead.links.span(), call);
    return memberRead;
}

static bool readsMemberOfPrototypeProperty(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::PropGet || value.isFreeName())
        return false;
    const Value& holder = function.at(CastChain(function, value.objectOperand()).last());
    return holder.opcode == Opcode::PropGet && !holder.isFreeName() && holder.key() == program.keyOf(BuiltinNames::prototype);
}

namespace {

struct PrototypeMemberRead {
    LayoutID classLayout;
    ValueID classObject;
    PropertyKeyID key;
    MemberRead reads;

    explicit operator bool() const { return !!classLayout; }
};

enum class ChainEnd : uint8_t { Unknown, MeetsClass, MissesClass };

// What says that an object still inherits from the prototype object that its layout names: a bit of its class, which the run time clears when any object of the class gets another prototype, or a test of the object itself.
enum class PrototypeKnownBy : uint8_t { BitOfClass, TestOfObject };

struct ChainAnswers {
    LayoutList decided;
    LayoutList instances;
    MemberConditions conditions;
    bool leavesOutLayouts { false };
    bool testsObjects { false };
};

struct TestedInstanceOf {
    bool walksChain { false };
    AccessTests tests;
};

} // anonymous namespace

static PrototypeMemberRead findPrototypeMemberRead(FunctionPassContext& context, const ValueSite& site)
{
    static_assert(!OperandIndex::PropGet::object, "findCastChainUsedByCallAlone starts at operand 0 of its user.");
    const Program& program = context.program;
    const Function& function = context.function;
    bool isCall = function.at(site.value).opcode == Opcode::Call;
    if (!readsMemberOfPrototypeProperty(program, function, isCall ? function.at(CastChain(function, function.at(site.value).calleeOperand()).last()) : function.at(site.value)))
        return { };

    const UseLists& useLists = context.analyses.useLists();
    PrototypeMemberRead found;
    if (isCall && !findCastChainUsedByCallAlone(function, useLists, site.value, found.reads))
        return { };
    ValueID memberRead = isCall ? found.reads.read().value : site.value;
    if (!findCastChainUsedByCallAlone(function, useLists, memberRead, found.reads))
        return { };

    found.classObject = function.at(found.reads.read().value).objectOperand();
    const Type& classObjectType = program.typeOf(function, found.classObject);
    if (classObjectType.mayBeUndefinedOrNull())
        return { };
    found.classLayout = classOfClassObject(program, classObjectType);
    found.key = function.at(memberRead).key();
    return found;
}

static bool isConstructorOfClass(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

static bool allocatesOwnThis(const Function& constructor)
{
    if (constructor.has(FunctionFlags::AllocatesOwnThis))
        return true;

    switch (constructor.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return !constructor.has(FunctionFlags::HasThis);
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Without a new.target to read the prototype from, the object has the prototype object of its class.
static ValueID emitThis(const Program& program, LayoutID allocatedClass, ValueID newTargetToRead, NOESCAPE const Invocable<ValueID(Value&&)> auto& emit)
{
    ValueID prototype;
    if (newTargetToRead) {
        Value read = IRBuilder::make(Opcode::GetConstructPrototype, { newTargetToRead });
        read.setLayout(declaredLayoutOf(program, allocatedClass));
        prototype = emit(WTF::move(read));
    }

    Value object = IRBuilder::make(Opcode::NewObject, { prototype });
    object.setLayout(allocatedClass);
    const Vector<AllocationClassID>& allocationClasses = program.at(allocatedClass).flow.allocationClasses;
    if (allocationClasses.size() == 1)
        object.setAllocationClass(allocationClasses.first());
    return emit(WTF::move(object));
}

static ValueID appendThis(IRBuilder& builder, LayoutID allocatedClass, ValueID newTargetToRead)
{
    return emitThis(builder.context().program, allocatedClass, newTargetToRead, [&](Value&& value) { return builder.append(WTF::move(value)); });
}

ValueID appendThisOfConstructorFunction(IRBuilder& builder, FunctionID constructor, ValueID closure)
{
    const Program& program = builder.context().program;
    LayoutID constructed = program.at(constructor).constructedClass;
    return appendThis(builder, constructed, hasOnePrototypeObject(program, constructed) ? ValueID() : closure);
}

static bool mayBeAtOrBelow(const Program& program, LayoutID layout, LayoutID base)
{
    for (size_t steps = 0; layout && steps <= program.layouts.size(); ++steps) {
        if (layout == base)
            return true;
        const LayoutFlow& flow = program.at(layout).flow;
        if (flow.baseState != BaseState::Layout)
            return program.at(layout).has(LayoutFlags::HasHeritage);
        layout = flow.base;
    }
    return false;
}

// new.target selects what is allocated, and the allocation is handed its prototype object alone. For a class beside the holder of the super call, the function that is constructed decides.
static bool isAlwaysClassAtOrBelow(const Program& program, const Type& newTarget, LayoutID holder)
{
    if (!newTarget.isObject() || !newTarget.hasOnlyKnownCallees() || newTarget.layouts || !newTarget.builtins.isEmpty())
        return false;
    return std::ranges::all_of(newTarget.functions, [&](FunctionID function) {
        return isConstructorOfClass(program.at(function)) && mayBeAtOrBelow(program, program.at(function).constructedClass, holder);
    });
}

// The run time exchanges the object for an instance of the class that new.target selects, so the class that holds the super call stands for all of them.
static LayoutID findClassAllocatedBySuperCall(const Program& program, LayoutID holder)
{
    // The source names a built-in class as the parent. Its constructor makes the instances, so the layout has no placement either, whatever flow finds above the class object.
    if (builtinBaseClassOf(program, holder))
        return { };
    if (program.at(holder).flow.allocation != AllocationState::NeverAllocated)
        return holder;
    std::span<const LayoutID> allocatedBelow = program.layoutsBelow(holder);
    return allocatedBelow.size() == 1 ? allocatedBelow[0] : LayoutID();
}

static Type classObjectsAllocatedAtOrBelow(const Program& program, LayoutID holder)
{
    Type classObjects;
    auto add = [&](LayoutID allocated) {
        if (FunctionID constructor = program.at(declaredLayoutOf(program, allocated)).constructor)
            classObjects.functions.add(constructor);
    };
    if (program.at(holder).flow.allocation != AllocationState::NeverAllocated)
        add(holder);
    std::ranges::for_each(program.layoutsBelow(holder), add);
    return classObjects;
}

static bool countsItsArguments(const Function& function)
{
    return function.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter });
}

static uint32_t constructorOperandIndexOf(Opcode construct)
{
    return construct == Opcode::Construct ? *OperandPosition::callee(construct) : *OperandPosition::parent(construct);
}

bool isConstructorFunction(const Program& program, const Function& function)
{
    if (function.kind != FunctionKind::Plain || function.constructorForm() != ConstructorForm::Base || !function.has(FunctionFlags::HasThis) || !function.constructedClass)
        return false;
    return program.at(declaredLayoutOf(program, function.constructedClass)).has(LayoutFlags::IsConstructorFunction);
}

// A function that takes `this` can have a copy that `new` enters where the function is its own new.target. The copy takes no `this`: it allocates the object and returns what was constructed.
static bool canHaveConstructBody(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || !function.mayBeCloned() || function.results.size() != 1 || !isConstructorFunction(program, function))
        return false;
    if (function.thisType.written() && !function.thisType.isExpected()) {
        const Type& bound = program.at(*function.thisType.established());
        if (!bound.anyObject && !std::ranges::binary_search(membersOf(program, bound), function.constructedClass))
            return false;
    }
    // A tail call in a constructor is none: what it returns decides whether the object is returned in its place. A test that leaves for generic code has to stand where that code was made.
    return std::ranges::none_of(function.blocks, [](const Block& block) {
        return block.terminator.kind == TerminatorKind::ReturnCall && std::ranges::any_of(block.terminator.casts(), [](TailCast cast) { return cast.resolution == CastResolution::Unresolved || cast.resolution == CastResolution::Check; });
    });
}

void emitReturnOfConstruction(IRBuilder& builder, ValueID returned, ValueID object)
{
    const Type* returnedType = returned ? &builder.context().program.typeOf(builder.function(), returned) : nullptr;
    if (returnedType && returnedType->isNever())
        return builder.emitUnreachable();
    if (!returnedType || !returnedType->hasObjects())
        return builder.emitReturn(object);
    if (returnedType->isObject())
        return builder.emitReturn(returned);

    ValueID isObject = builder.appendTypeTest(returned, Type::object());
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isObject, 0, BlockFrequency::Unlikely, BlockFrequency::Normal);
    builder.emitReturn(branch.provenOnTaken);
    builder.moveToEndOf(branch.notTaken);
    builder.emitReturn(object);
}

Function buildConstructBody(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& original = program.at(key.target);
    RELEASE_ASSERT(canHaveConstructBody(program, key.target));
    unsigned thisIndex = *original.thisParameterIndex();
    bool readsPrototypeOfNewTarget = !hasOnePrototypeObject(program, original.constructedClass);

    Vector<ParameterOrigin> parameterMap;
    for (unsigned index = 0; index < original.numberOfEntryParameters(); ++index) {
        if (index != thisIndex)
            parameterMap.append(ParameterOrigin { index });
    }
    Function body = original;
    body.role = FunctionRole::ConstructBody;
    body.creator = CreatorRecord::Version { .provenVersion = key.target };
    body.flags.remove(FunctionFlags::HasThis);
    body.flags.add(FunctionFlags::AllocatesOwnThis);
    body.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    if (readsPrototypeOfNewTarget && !original.has(FunctionFlags::HasNewTarget)) {
        body.flags.add(FunctionFlags::HasNewTarget);
        parameterMap.insert(*body.newTargetParameterIndex(), ParameterOrigin { });
    }
    body.capabilities = initialCapabilitiesOf(body);
    body.derivation = Derivation { .source = key.target, .parameterMap = WTF::move(parameterMap) };

    IRBuilder builder(AnalysisContext { inputs.build, program, &inputs.flow, nullptr, key.target }, body, inputs.types);
    builder.setOrigin({ original.position, 0 });
    if (body.at(entryBlock).handler) {
        builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
        builder.removeHandler(entryBlock);
    }

    ValueID thisParameter = body.at(entryBlock).parameters[thisIndex];
    ValueID newTarget;
    if (std::optional<unsigned> index = original.newTargetParameterIndex())
        newTarget = body.at(entryBlock).parameters[*index];
    else if (body.has(FunctionFlags::HasNewTarget))
        newTarget = builder.addParameter(entryBlock, TypeTable::anything);

    auto insertAtHead = [&](Value&& value) { return builder.insertBefore(ProgramPoint::atHeadOf(entryBlock), WTF::move(value)); };

    // new.target is a closure of the function here, which is what says that its prototype property belongs to the layout.
    std::optional<unsigned> calleeIndex = original.calleeParameterIndex();
    ValueID closure = calleeIndex ? body.at(entryBlock).parameters[*calleeIndex] : newTarget;
    ValueID validation;
    ValueID ownClosure;
    if (closure) {
        FunctionID closureFunction = program.at(declaredLayoutOf(program, original.constructedClass)).constructor;
        TypeID closureType = inputs.types.intern(Type::forFunction(closureFunction));
        if (inputs.build.validates())
            validation = insertAtHead(makeValidateType(closure, closureType));
        ownClosure = insertAtHead(makeAssumeType(closure, Proven::byRulesWithoutChecks(closureType)));
    }
    ValueID object = emitThis(program, original.constructedClass, readsPrototypeOfNewTarget ? ownClosure : ValueID(), insertAtHead);
    builder.applyInsertions();

    ValueSubstitution substitution(body);
    substitution.replace(thisParameter, object);
    if (ownClosure) {
        substitution.replace(closure, ownClosure);
        if (newTarget && newTarget != closure)
            substitution.replace(newTarget, ownClosure);
    }
    substitution.apply(body, nullptr);
    if (validation)
        body.edit(validation, ValueEdit::Definition).setValueOperand(closure);
    if (ownClosure)
        body.edit(ownClosure, ValueEdit::Definition).setValueOperand(closure);

    BitVector removedParameters(body.at(entryBlock).parameters.size());
    removedParameters.set(thisIndex);
    builder.removeParameters(entryBlock, removedParameters);
    if (!original.has(FunctionFlags::HasNewTarget) && newTarget) {
        auto& parameters = body.edit(entryBlock, BlockEdit::Parameters).parameters;
        RELEASE_ASSERT(parameters.last() == newTarget);
        parameters.removeLast();
        parameters.insert(*body.newTargetParameterIndex(), newTarget);
    }

    size_t numberOfBlocks = body.blocks.size();
    for (size_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        if (body.at(block).terminator.kind == TerminatorKind::ReturnCall)
            builder.demoteReturnCall(block);
        if (body.at(block).terminator.kind != TerminatorKind::Return)
            continue;

        ValueID returned = body.at(block).terminator.valueOperand();
        builder.setOrigin(body.at(block).terminator.origin());
        body.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator();
        builder.moveToEndOf(block);
        emitReturnOfConstruction(builder, returned, object);
    }
    Vector<BlockID> blocks(body.blocks.size(), [](size_t index) { return BlockID(index); });
    keepFrameBelowTailCalls(body, blocks.span(), 0);

    TypeID typeOfResult = TypeTable::never;
    for (const Block& block : body.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            typeOfResult = inputs.types.unionOf(typeOfResult, body.at(block.terminator.valueOperand()).type);
    }
    body.results[0] = DeclaredType(Proven::byPosition(typeOfResult == TypeTable::never ? body.at(object).type : typeOfResult));
    return body;
}

static ClassSide sideOf(MemberStart start)
{
    switch (start) {
    case MemberStart::ClassPrototype:
        return ClassSide::Prototype;
    case MemberStart::ClassObject:
        return ClassSide::Static;
    case MemberStart::Instance:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberAnswer findBuiltinClassMember(const PropertyAccessContext& context, BuiltinClassID builtinClass, ClassSide side, PropertyKeyID key, Access access)
{
    switch (side) {
    case ClassSide::Prototype:
        return findBuiltinInstanceMember(context, builtinClass, key, access);
    case ClassSide::Static: {
        BuiltinID constructor = context.program.builtins().at(builtinClass).constructor;
        if (!constructor)
            return MemberAnswer::unknown(UnprovenCause::UnknownBase);
        return findBuiltinObjectMember(context, constructor, key, access);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The guards of a constructor function begin with the first object that it makes, and they speak of the prototype object that it has then.
// While the guard of its prototype property holds, they have begun, and a class below the function inherits from that object.
static bool startsAtPrototypeThatGuardsMayNotDescribe(const Layout& parent, const MemberAnswer& found)
{
    if (!parent.has(LayoutFlags::IsConstructorFunction))
        return false;
    return parent.classFlow.prototypeProperty != MemberMutability::Fixed || !found.conditions.isEmpty();
}

static std::optional<MemberAnswer> findSuperMember(const PropertyAccessContext& context, const Function& function, const Value& superAccess, Access access)
{
    const Program& program = context.program;
    const Value& superBase = function.at(sameObjectThroughCasts(function, superAccess.baseOperand()));
    if (superBase.opcode != Opcode::GetSuperBase)
        return std::nullopt;
    LayoutID classLayout = classOfClassObject(program, program.typeOf(function, superBase.homeOperand()));
    if (!classLayout)
        return std::nullopt;
    MemberStart start = MemberStart::Instance;
    switch (superBase.homeKind()) {
    case HomeObjectKind::Object:
        start = MemberStart::ClassObject;
        break;
    case HomeObjectKind::ClassPrototype:
        if (FunctionID constructor = program.at(classLayout).constructor; !constructor || !isConstructorOfClass(program.at(constructor)))
            return std::nullopt;
        start = MemberStart::ClassPrototype;
        break;
    }

    PropertyKeyID key = superAccess.key();
    if (keyLookupOf(program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);
    ClassSide side = sideOf(start);
    MemberAnswer walked;
    NextHolder next = nextHolderOf(program, classLayout, side, walked.conditions);

    MemberAnswer found;
    switch (next.step) {
    case ChainStep::Unknown:
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    case ChainStep::CompiledClass:
        found = findMember(context, { start, next.layout, key, access });
        if (side == ClassSide::Prototype && startsAtPrototypeThatGuardsMayNotDescribe(program.at(next.layout), found))
            addCondition(walked.conditions, { MemberCondition::Kind::MemberUnchanged, next.layout, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
        break;
    case ChainStep::BuiltinClass:
        found = findBuiltinClassMember(context, next.builtinClass, side, key, access);
        break;
    case ChainStep::NoBase:
        found = findBuiltinInstanceMember(context, side == ClassSide::Static ? BuiltinClasses::Function : BuiltinClasses::Object, key, access);
        break;
    }
    return finishAtChainEnd(WTF::move(walked), WTF::move(found));
}

static bool inheritsFromObjectPrototype(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return true;
    case LayoutKind::BuiltinResult:
        // A tuple is an array, and no record says that the prototype object of a built-in class keeps its own prototype.
        return layout.builtinLayout && !program.builtins().at(layout.builtinLayout).isTuple;
    case LayoutKind::Instance:
    case LayoutKind::ModuleNamespace:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ChainEnd followChain(const Program& program, LayoutID layout, LayoutID tested, MemberConditions& conditions)
{
    const Layout& start = program.at(layout);
    if (start.kind == LayoutKind::ModuleNamespace)
        return ChainEnd::MissesClass;

    bool mayShareObject = program.at(tested).classFlow.hasAssignedPrototype;
    if (inheritsFromObjectPrototype(program, start))
        return mayShareObject ? ChainEnd::Unknown : ChainEnd::MissesClass;
    if (start.kind != LayoutKind::Instance)
        return ChainEnd::Unknown;

    LayoutID current = declaredLayoutOf(program, layout);
    for (size_t unvisitedClasses = program.layouts.size(); unvisitedClasses; --unvisitedClasses) {
        if (current == tested)
            return ChainEnd::MeetsClass;
        const Layout& passed = program.at(current);
        if (passed.kind != LayoutKind::Instance || !passed.constructor)
            return ChainEnd::Unknown;
        mayShareObject |= passed.classFlow.hasAssignedPrototype;
        NextHolder next = nextHolderOf(program, current, ClassSide::Prototype, conditions);
        switch (next.step) {
        case ChainStep::CompiledClass:
            current = next.layout;
            // `function Meter() {}`, `Meter.prototype = first`, `class Fine extends Meter {}`, `Meter.prototype = second`: the class below is linked to the object that the property held when it was defined.
            if (program.at(current).has(LayoutFlags::IsConstructorFunction) && program.at(current).classFlow.prototypeProperty != MemberMutability::Fixed)
                return ChainEnd::Unknown;
            break;
        case ChainStep::NoBase:
            return mayShareObject ? ChainEnd::Unknown : ChainEnd::MissesClass;
        case ChainStep::BuiltinClass:
        case ChainStep::Unknown:
            return ChainEnd::Unknown;
        }
    }
    return ChainEnd::Unknown;
}

static ChainAnswers followChains(const Program& program, const LayoutList& layouts, LayoutID tested, PrototypeKnownBy knownBy)
{
    ChainAnswers answers;
    for (LayoutID layout : layouts) {
        if (program.at(layout).flow.allocation == AllocationState::NeverAllocated)
            continue;

        MemberConditions conditions;
        ChainEnd end = followChain(program, layout, tested, conditions);
        bool testsObject = false;
        // The run time defines the class of a constructor function at the first construction that is carried out, with whatever the function has as its prototype property then.
        // Where nothing replaces that property it is the one object, and the bits of the tested class, which is the function's own or one above it, say that the class was defined.
        // That the chain misses a class no bit says: another function may have been given the object and been constructed first, and then this one has no class.
        const Layout* maker = program.at(layout).kind == LayoutKind::Instance ? &program.at(declaredLayoutOf(program, layout)) : nullptr;
        bool keepsPrototypeObject = maker && maker->classFlow.prototypeProperty == MemberMutability::Fixed && end == ChainEnd::MeetsClass;
        bool isMadeByConstructorFunction = maker && maker->has(LayoutFlags::IsConstructorFunction) && !keepsPrototypeObject;
        if (end != ChainEnd::Unknown && (isMadeByConstructorFunction || (program.at(layout).kind != LayoutKind::ModuleNamespace && !receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype)))) {
            switch (knownBy) {
            case PrototypeKnownBy::BitOfClass:
                // Only a class has bits, and they follow what happens to compiled objects.
                if (builtinBaseClassOf(program, layout))
                    testsObject = true;
                else if (maker && !isMadeByConstructorFunction)
                    addCondition(conditions, { MemberCondition::Kind::InstancePrototypeUnchanged, declaredLayoutOf(program, layout), ClassSide::Prototype, { } });
                else
                    end = ChainEnd::Unknown;
                break;
            case PrototypeKnownBy::TestOfObject:
                testsObject = true;
                break;
            }
        }
        if (end == ChainEnd::Unknown) {
            answers.leavesOutLayouts = true;
            continue;
        }

        answers.decided.add(layout);
        if (end == ChainEnd::MeetsClass)
            answers.instances.add(layout);
        addConditions(answers.conditions, conditions.span());
        answers.testsObjects |= testsObject;
    }
    return answers;
}

struct ConsideredLayouts {
    LayoutList layouts;
    // No object with another layout arrives.
    bool areAll { false };
};

static ConsideredLayouts layoutsToConsider(const Program& program, const ReceiverCandidates& subject, const Type& subjectType, LayoutID tested)
{
    if (subject.isProven())
        return { subject.proven, true };
    if (!subjectType.anyObject)
        return { { }, true };

    // Nothing is known of the object. The layouts of the class and of what is below it are worth one test.
    LayoutList considered(program.layoutsBelow(tested));
    considered.unite(LayoutList(program.at(program.declaredMembersOf(tested)).completeMembers()));
    considered.add(tested);
    return { WTF::move(considered), false };
}

static bool isObjectOutsideLayouts(UnionPartKind kind)
{
    switch (kind) {
    case UnionPartKind::OtherObjects:
    case UnionPartKind::BuiltinClass:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return true;
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
    case UnionPartKind::Layouts:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A type that lists layouts counts as a proof of the instance where the value has it from its definition. A test of the shape narrows a value to such a type too, and the objects of every instance of the image pass it.
bool mayBelongToOtherInstance(FunctionPassContext& context, ValueID subject, const ReceiverCandidates& candidates)
{
    if (instanceCountOf({ context.program, &context.inputs.flow }) == InstanceCount::AtMostOne)
        return false;
    if (!candidates.isProven())
        return true;
    context.planAsksAboutPlace = true;

    // Where values join, the type says what they have in common, and one of them may have it from a test: each value that arrives is asked.
    const Function& function = context.function;
    BitVector isListed;
    Vector<ValueID, 8> values;
    auto add = [&](ValueID value) {
        // What a declared type says is taken at its word, here as everywhere.
        for (; isCastOrConversion(function.at(value).opcode); value = function.at(value).operand(0)) {
            if (function.at(value).opcode == Opcode::Check && context.program.typeOf(function, value).layouts)
                return;
        }
        if (!isListed.set(value.index()))
            values.append(value);
    };
    add(subject);
    for (size_t position = 0; position < values.size(); ++position) {
        ValueID value = values[position];
        if (!context.program.typeOf(function, value).layouts)
            return true;
        ValueLocation location = context.analyses.useLists().locationOf(value);
        if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
            continue;
        for (Edge edge : context.analyses.cfg().incomingEdges(location.block)) {
            if (edge.isHandlerEdge())
                return true;
            ValueID passed = valuePassedOn(function, edge, location.index);
            if (!passed)
                return true;
            add(passed);
        }
    }
    return false;
}

// An object of another instance stays one, whatever type it has where values join behind the test.
bool hasFailedInstanceTest(FunctionPassContext& context, ValueID subject, BlockID block)
{
    if (instanceCountOf({ context.program, &context.inputs.flow }) == InstanceCount::AtMostOne)
        return false;
    context.planAsksAboutPlace = true;
    return context.analyses.findEarlierTest(makeObjectTest(Proof::OwnInstance, subject, { }), block) == EarlierTest::Failed;
}

void noteWhetherTypeProvesInstance(FunctionPassContext& context, ReceiverCandidates& candidates, ValueID receiver, BlockID block)
{
    candidates.mayBelongToOtherInstance = candidates.proof == ReceiverProof::Type && (mayBelongToOtherInstance(context, receiver, candidates) || hasFailedInstanceTest(context, receiver, block));
}

static LayoutID classTestedBy(const Program& program, const Function& function, const Value& instanceOf)
{
    const Type& constructorType = program.typeOf(function, instanceOf.constructorOperand());
    return constructorType.isObject() ? classOfClassObject(program, constructorType) : LayoutID();
}

// While these hold, C finds the ordinary Symbol.hasInstance and its prototype property holds the prototype object of its class.
static std::optional<MemberConditions> conditionsOfClassObject(const PropertyAccessContext& context, LayoutID tested)
{
    const Program& program = context.program;
    if (guardsAreNeverSet(program, program.at(tested)))
        return std::nullopt;
    MemberAnswer hasInstance = findMember(context, { MemberStart::ClassObject, tested, program.keyOf(BuiltinNames::hasInstanceSymbol), Access::Read });
    if (hasInstance.kind != MemberKind::Builtin || hasInstance.builtin != Builtins::FunctionPrototypeSymbolHasInstance || !hasInstance.receiverProofs.isEmpty())
        return std::nullopt;

    MemberConditions conditions = WTF::move(hasInstance.conditions);
    const ClassFlow& classFlow = program.at(tested).classFlow;
    // `Object.setPrototypeOf(Readable, Stream)`: the function has another parent than Function.prototype from before its first construction on.
    // The guard of the member is armed over the chain that is there, and ends when an object of that chain gets the name or another parent.
    if (program.at(tested).has(LayoutFlags::IsConstructorFunction)) {
        conditions.removeAllMatching([&](const MemberCondition& condition) {
            return condition.kind == MemberCondition::Kind::PrototypeLinkUnchanged && condition.layout == tested && condition.side == ClassSide::Static;
        });
    }
    // No code can change the property of a class.
    if (program.at(tested).has(LayoutFlags::IsConstructorFunction) && classFlow.prototypeProperty != MemberMutability::Fixed && !classFlow.hasAssignedPrototype)
        addCondition(conditions, { MemberCondition::Kind::MemberUnchanged, tested, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    if (!isEvaluatedOnce(program, program.at(tested)))
        addCondition(conditions, { MemberCondition::Kind::ClassEvaluatedOnce, tested, ClassSide::Prototype, { } });
    return conditions;
}

static Vector<TestedInstanceOf, 2> planTestedInstanceOf(FunctionPassContext& context, const Value& instanceOf)
{
    const Program& program = context.program;
    PropertyAccessContext accessContext { program, &context.inputs.flow };
    LayoutID tested = classTestedBy(program, context.function, instanceOf);
    std::optional<MemberConditions> conditionsOfClass = tested ? conditionsOfClassObject(accessContext, tested) : std::nullopt;
    if (!conditionsOfClass)
        return { };

    Vector<TestedInstanceOf, 2> forms;
    ValueID subject = instanceOf.valueOperand();
    ReceiverCandidates candidates = receiverCandidates(program, context.function, subject);
    auto appendFormOfLayouts = [&](PrototypeKnownBy knownBy) {
        ChainAnswers answers = followChains(program, candidates.isProven() ? candidates.proven : candidates.expected.predicted(), tested, knownBy);
        // Where no object has to be asked, the bits have said it all.
        if (answers.decided.isEmpty() || (knownBy == PrototypeKnownBy::TestOfObject && !answers.testsObjects))
            return;

        PropertyAccessPlan plan;
        plan.conditions = *conditionsOfClass;
        addConditions(plan.conditions, answers.conditions.span());
        plan.receivers = answers.decided;
        // undefined and null are instances of nothing, and the tests of an object take an object.
        if (!candidates.isProven() || candidates.mayBeNullish || answers.leavesOutLayouts)
            plan.receiverTest = ReceiverTest::Type;
        if (mayBelongToOtherInstance(context, subject, candidates))
            plan.requiredProofs.add(Proof::OwnInstance);
        if (answers.testsObjects)
            plan.requiredProofs.add(Proof::PrototypeLinkUnchanged);
        AccessTests tests = createAccessTests(context, plan, { subject, { }, { } });
        if (!tests.tests.isEmpty())
            forms.append(TestedInstanceOf { false, WTF::move(tests) });
    };
    if (!candidates.isProven() || !candidates.hasOtherParts) {
        appendFormOfLayouts(PrototypeKnownBy::BitOfClass);
        appendFormOfLayouts(PrototypeKnownBy::TestOfObject);
    }

    if (!conditionsOfClass->isEmpty())
        forms.append(TestedInstanceOf { true, { { makeConditionsTest(context.types, conditionsOfClass->span()) }, { } } });
    return forms;
}

static std::optional<LayoutList> instancesAmongTestedLayouts(const Program& program, const Function& function, const Value& instanceOf, std::span<const Value> tests, LayoutList& testedLayouts)
{
    std::optional<ReceiverCandidates> established = receiversEstablishedBy(program, tests);
    if (!established)
        established = receiverCandidates(program, function, instanceOf.valueOperand());
    if (!established->isProven() || established->hasOtherParts || established->mayBeNullish)
        return std::nullopt;

    bool testsObjects = std::ranges::contains(tests, Opcode::HasLayoutPrototype, &Value::opcode);
    ChainAnswers answers = followChains(program, established->proven, classTestedBy(program, function, instanceOf), testsObjects ? PrototypeKnownBy::TestOfObject : PrototypeKnownBy::BitOfClass);
    // The layouts answer only under the tests that their chains ask for. The tests of the class object are among the tests of every form.
    if (answers.leavesOutLayouts || (answers.testsObjects && !testsObjects) || !testsCoverConditions(tests, answers.conditions.span()))
        return std::nullopt;

    testedLayouts = WTF::move(answers.decided);
    return WTF::move(answers.instances);
}

static bool useNamesGuard(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isTailCall(const Function& function, const ValueSite& call)
{
    const Terminator& terminator = function.at(call.block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call.value;
}

static void appendLookupValidation(IRBuilder& builder, FunctionID method, ValueID firstHolder, PropertyKeyID key, ValueID capturedReceiver = { })
{
    Value closure = IRBuilder::make(Opcode::NewClosure);
    closure.setFunction(method);
    if (capturedReceiver)
        closure.setCapturesOperands(std::array { capturedReceiver });

    Value validation = IRBuilder::make(Opcode::ValidateLookup, { firstHolder, builder.append(WTF::move(closure)) });
    validation.setKey(key);
    builder.append(WTF::move(validation));
}

// Every object has a function object of its own under the field, and a stack that is taken before one of them gets its first name is read behind it.
static bool mayGetNameOfItsOwnOutOfSight(const Program& program, const FlowResults& flow, const PropertyAccessPlan& read)
{
    return read.capturesReceiver && recordsCalleeOnlyOnceRenamed(program, read.member.method) && flow.closuresMayBeNamedOutOfSight(read.member.method);
}

static bool canCallDeferredClosure(const Program& program, TypeArena& types, const PropertyAccessPlan& read)
{
    if (read.form != AccessForm::Method || !read.needsNoTest() || !mayBeCalledDirectly(program, read.member.method) || program.at(read.member.method).has(FunctionFlags::TakesCallee))
        return false;
    const Function& method = program.at(read.member.method);
    if (!read.capturesReceiver)
        return method.captures.isEmpty();
    return method.captures.size() == 1 && Type::forLayouts(read.receivers.span(), types).isSubsetOf(program.at(method.captures[0].written()), program);
}

static bool isGenericSpread(const Program& program, const Value& value, uint32_t operandIndex)
{
    if (value.opcode != Opcode::CallDirect || operandIndex)
        return false;
    const Function& callee = program.at(value.function());
    return isOutlinedIteration(callee) && callee.derivation.node == static_cast<uint32_t>(OutlinedIteration::AppendIterable);
}

bool isCallWithSharedArms(const Function& function)
{
    return function.role == FunctionRole::OutlinedBody && kindOfOutlinedBody(function.derivation.node) == OutlinedIteration::CallWithSharedArms;
}

namespace {

// What tells one function with shared arms from another: the functions that the value it calls may be, and how the places that call it pass `this` and the arguments.
struct SharedArms {
    // No convention has more registers for numbers. An argument behind these is passed as any value.
    static constexpr unsigned numberOfArgumentsPassedAsNumbers = 8;
    static constexpr unsigned passesThisShift = 8;
    static constexpr unsigned namesCalleeSetShift = 9;
    static constexpr unsigned numbersShift = 10;
    static constexpr unsigned numberOfArgumentsShift = numbersShift + numberOfArgumentsPassedAsNumbers;
    static constexpr uint32_t maxNumberOfArguments = (1u << (32 - numberOfArgumentsShift)) - 1;

    DerivedFunctionKey key() const
    {
        uint32_t node = static_cast<uint32_t>(OutlinedIteration::CallWithSharedArms) | static_cast<uint32_t>(passesThis) << passesThisShift | static_cast<uint32_t>(namesCalleeSet) << namesCalleeSetShift
            | static_cast<uint32_t>(numbers) << numbersShift | numberOfArguments << numberOfArgumentsShift;
        return { .role = FunctionRole::OutlinedBody, .target = FunctionID(), .parameter = list, .node = node };
    }
    static SharedArms decode(const DerivedFunctionKey& key)
    {
        RELEASE_ASSERT(kindOfOutlinedBody(key.node) == OutlinedIteration::CallWithSharedArms);
        return { key.parameter, !!(key.node >> namesCalleeSetShift & 1), key.node >> numberOfArgumentsShift, !!(key.node >> passesThisShift & 1), static_cast<uint8_t>(key.node >> numbersShift) };
    }
    std::span<const FunctionID> functions(const Program& program LIFETIME_BOUND) const
    {
        return namesCalleeSet ? program.at(CalleeSetID(list)).written().functions : program.at(TypeID(list)).functions.span();
    }

    // A record of the program that lists the functions: a type, or a callee set.
    uint32_t list { 0 };
    bool namesCalleeSet { false };
    uint32_t numberOfArguments { 0 };
    bool passesThis { false };
    // Bit i: argument i is a number.
    uint8_t numbers { 0 };
};

struct CallSite {
    ValueSite position;
    Opcode opcode { Opcode::Undefined };
    MemberRead memberRead;
    PrototypeMemberRead prototypeMemberRead;
};

enum class CallArmKind : uint8_t { OriginalOperations, DirectCall, Undefined };

struct CallArm {
    CallArmKind kind { CallArmKind::OriginalOperations };
    FunctionID closureFunction;
    FunctionID callee;
    ValueID closureCreation;
    bool capturesReceiver { false };
    bool keepsCalleeInFrame { false };
    bool readsClosureOfMember { false };
    LayoutSetID receivers;
    MemberConditions conditions;
};

struct ConstructArm {
    CallArm call;
    LayoutID allocatedClass;
    bool readsConstructPrototype { false };
    TypeID testedNewTarget;
};

struct SpreadOfRest {
    ValueID rest;
    ValueID spreadName;
    ValueID list;
    Vector<ValueID, 4> operationsOnList;
    ValueID append;
};

struct CallResolver {
    using Rewriting = WTF::Function<void()>;
    using Plan = std::expected<Rewriting, CallDeclineReason>;
    using Planner = Plan (CallResolver::*)(const CallSite&);

    struct PlannedCall {
        Rewriting rewrite;
        bool cutsBlock { true };
    };

    static constexpr std::unexpected<CallDeclineReason> declined { CallDeclineReason::None };

    FunctionPassContext& context;
    bool resolvesOnlyThroughTable;
    const Program& program { context.program };
    Function& function { context.function };
    const FlowResults& flow { context.inputs.flow };
    IRBuilder builder { context.analyses.context(), context.function, context.types, &context.analyses };
    SiteExpansion expansion { context, builder };
    PristineRequirement pristineArray { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };

    PassOutcome run()
    {
        Vector<CallSite> sites = collectSites();
        Vector<PlannedCall> plans = planSites(sites.mutableSpan());
        if (plans.isEmpty())
            return PassOutcome::Unchanged;

        for (const PlannedCall& planned : plans | std::views::reverse) {
            // Whoever cuts a block finds in it what was put behind the cut.
            if (builder.hasInsertions() && planned.cutsBlock)
                builder.applyInsertions();
            planned.rewrite();
            RELEASE_ASSERT(!expansion.isOpen() && !builder.currentBlock());
        }
        if (builder.hasInsertions())
            builder.applyInsertions();

        context.build.statistics().count(StatisticID::CallsResolved, plans.size());
        // Each rewriting tells the analyses of its edits as it makes them.
        context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

    // False only where plan() finds nothing and has nothing to report. It asks what the planners of the operation ask first, of the operation and of the types of its operands alone.
    bool mayHavePlan(const Value& value) const
    {
        if (value.opcode == Opcode::CallBuiltin) {
            const BuiltinOverload& overload = program.builtins().at(value.overload());
            if (value.builtinCallForm() == BuiltinCallForm::AnyReceiver)
                return overload.builtin == Builtins::FunctionPrototypeBind || (program.at(value.type).isAnything() && forwardsCallOfAnyReceiver(overload));
            return value.builtinCallForm() == BuiltinCallForm::Whole && (program.at(value.type).isAnything() || overload.builtin == Builtins::FunctionPrototypeBind);
        }
        if (value.opcode != Opcode::PropGet && value.opcode != Opcode::PropSet)
            return true;
        if (!resolvesOnlyThroughTable && readsMemberOfPrototypeProperty(program, function, value))
            return true;
        const Type& receiver = typeOf(value.objectOperand());
        if (receiver.mayBeUndefinedOrNull())
            return false;
        return (!value.isFreeName() && receiver.layouts) || classOfClassObject(program, receiver);
    }

    Vector<CallSite> collectSites()
    {
        Vector<CallSite> sites;
        BitVector isMemberRead(function.values.size());
        const bool checksScreen = context.build.options().verifiesAfterEveryPass;
        context.forEachSiteIn(resolvesOnlyThroughTable ? SiteList::Calls : SiteList::CallSites, [&](const ValueSite& position) {
            if (resolvesOnlyThroughTable && !findReceiversOfTableCall(position))
                return;

            const Value& value = function.at(position.value);
            if (!mayHavePlan(value)) {
                if (checksScreen) [[unlikely]] {
                    Plan planned = plan(CallSite { position, value.opcode, { }, { } });
                    RELEASE_ASSERT(!planned && (planned.error() == CallDeclineReason::None || planned.error() == CallDeclineReason::NeedsTest));
                }
                return;
            }
            CallSite site { position, value.opcode, { }, { } };
            if (value.opcode == Opcode::Call) {
                site.memberRead = findLinksOfMemberRead(context, position);
                if (!site.memberRead.isEmpty())
                    isMemberRead.quickSet(site.memberRead.read().value.index());
            }
            if (!resolvesOnlyThroughTable && site.memberRead.isEmpty() && (value.opcode == Opcode::Call || value.opcode == Opcode::PropGet)) {
                site.prototypeMemberRead = findPrototypeMemberRead(context, position);
                for (const ValueSite& link : site.prototypeMemberRead.reads.links)
                    isMemberRead.quickSet(link.value.index());
            }
            sites.append(WTF::move(site));
        });

        sites.removeAllMatching([&](const CallSite& site) { return isMemberRead.quickGet(site.position.value.index()); });
        return sites;
    }

    // All that plan() takes from a call, a read or a store by name and its function, until it asks about the place of the site. None for the other operations.
    std::optional<SiteContent> contentOf(const CallSite& site) const
    {
        AOT_THRESHOLD(unsigned, maxLinks, 8);
        const Value& value = function.at(site.position.value);
        SiteContent content(static_cast<uint32_t>(RefusalKind::Call) << 24 | static_cast<uint32_t>(value.opcode) << 8 | value.flags);
        content.add(&flow);
        auto addAccess = [&](const Value& access) {
            content.add(static_cast<uint32_t>(access.opcode) << 8 | access.flags);
            content.add(access.key());
            content.add(access.receivers());
            content.addTypesOf(function, access.objectOperand());
        };

        // The plan follows from what the receiver was read from.
        if (site.prototypeMemberRead)
            return std::nullopt;
        if (value.opcode == Opcode::PropGet || value.opcode == Opcode::PropSet)
            addAccess(value);
        else if (value.opcode == Opcode::Call) {
            content.add(value.callees());
            // A tail call gives up the frame that would keep the callee, so it is planned another way.
            content.add(static_cast<uint32_t>(isTailCall(function, site.position) || value.isTailCall));
            if (value.accessSite() && flow.hasResults()) {
                std::span<const FlowBoundFunctionAtSite> seen = flow.boundFunctionsSeenAt(value.accessSite(), FlowBoundFunctionUseAtSite::Called).predicted();
                content.add(static_cast<uint32_t>(seen.size()));
                for (const FlowBoundFunctionAtSite& bound : seen) {
                    content.add(bound.target.index());
                    content.add(bound.numberOfBoundArguments);
                }
                PropertyKeyID key = keyThatCalleeWasReadUnder(context, value.calleeOperand());
                std::span<const FlowClosureStoredUnderKey> stored = key ? flow.closuresSeenStoredUnder(key).predicted() : std::span<const FlowClosureStoredUnderKey>();
                content.add(static_cast<uint32_t>(stored.size()));
                for (const FlowClosureStoredUnderKey& entry : stored)
                    content.add(entry.function.index());
            }
            // The plan follows from what the ways to the call bring.
            if (!findClosuresArrivingOnWaysOfTheirOwn(context, value.calleeOperand()).isEmpty())
                return std::nullopt;
            unsigned numberOfLinks = 0;
            for (ValueID link : CastChain(function, value.calleeOperand())) {
                if (++numberOfLinks > maxLinks)
                    return std::nullopt;
                content.addTypesOf(function, link);
            }
            content.add(numberOfLinks);
            content.add(static_cast<uint32_t>(site.memberRead.links.size()) << 1 | site.memberRead.canSinkToCall);
            if (!site.memberRead.isEmpty()) {
                const Value& read = function.at(site.memberRead.read().value);
                if (read.opcode != Opcode::PropGet)
                    return std::nullopt;
                addAccess(read);
                content.add(!value.thisValueOperand() ? 0u : value.thisValueOperand() == read.objectOperand() ? 1u : 2u);
            }
        } else
            return std::nullopt;
        return content.holdsOnlyIDsOfProgram() ? std::optional(content) : std::nullopt;
    }

    Vector<PlannedCall> planSites(std::span<CallSite> sites)
    {
        Vector<PlannedCall> plans;
        RefusalKind kind = resolvesOnlyThroughTable ? RefusalKind::TableCall : RefusalKind::Call;
        PropertyAccessCache* cache = resolvesOnlyThroughTable ? nullptr : program.propertyAccessCache();
        for (CallSite& site : sites) {
            if (Rewriting onlyCallee = planCallOfOnlyCallee(site)) {
                plans.append({ WTF::move(onlyCallee), false });
                continue;
            }
            uint32_t state = RefusedSites::stateOf(function, site.position.value);
            if (!site.memberRead.isEmpty())
                state = state * 31 + RefusedSites::stateOf(function, site.memberRead.read().value);
            if (context.refusedSites.contains(kind, site.position.value, state))
                continue;
            if (!site.memberRead.isEmpty())
                site.memberRead.canSinkToCall = canSinkToCall(context, site.memberRead.links.span(), site.position);
            if (site.prototypeMemberRead)
                site.prototypeMemberRead.reads.canSinkToCall = canSinkToCall(context, site.prototypeMemberRead.reads.links.span(), site.position);

            std::optional<SiteContent> content = cache ? contentOf(site) : std::nullopt;
            std::optional<uint32_t> reasonOfKnownRefusal = content ? cache->refusedContents.find(*content) : std::nullopt;
            if (reasonOfKnownRefusal && !cache->checksAnswers) [[likely]] {
                reportDecline(site, static_cast<CallDeclineReason>(*reasonOfKnownRefusal));
                context.refusedSites.add(kind, function, site.position.value, state);
                continue;
            }

            context.planAsksAboutPlace = false;
            uint64_t unshareableEventCountBefore = context.types.unshareableEventCount();
            Plan planned = plan(site);
            RELEASE_ASSERT(!reasonOfKnownRefusal || (!planned && static_cast<uint32_t>(planned.error()) == *reasonOfKnownRefusal));
            if (planned) {
                plans.append({ WTF::move(*planned) });
                continue;
            }
            reportDecline(site, planned.error());
            context.refusedSites.add(kind, function, site.position.value, state);
            if (content && !reasonOfKnownRefusal && !context.planAsksAboutPlace && context.types.unshareableEventCount() == unshareableEventCountBefore)
                cache->refusedContents.add(*content, static_cast<uint32_t>(planned.error()));
        }
        return plans;
    }

    // collectSites() looks for the read of a member of a prototype property only where a call has no member read, so the planner of the one and those of the other take different calls.
    Plan plan(const CallSite& site)
    {
        static constexpr std::pair<OpcodeSet, Planner> planners[] {
            { { Opcode::Call, Opcode::PropGet }, &CallResolver::planPrototypeMember },
            { { Opcode::Call, Opcode::PropGet, Opcode::PropSet }, &CallResolver::planReceiverDispatch },
            { { Opcode::Call, Opcode::PropGet, Opcode::PropSet }, &CallResolver::planClassObjectMember },
            { { Opcode::Call, Opcode::SuperGet, Opcode::SuperSet }, &CallResolver::planSuperMember },
            { { Opcode::Call }, &CallResolver::planCalleeDispatch },
            { { Opcode::Construct, Opcode::SuperConstruct, Opcode::SuperConstructList }, &CallResolver::planConstruct },
            { { Opcode::CallList }, &CallResolver::planRestHandover },
            { { Opcode::CallList }, &CallResolver::planSpreadHandover },
            { { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList }, &CallResolver::planListCall },
            { { Opcode::CallList }, &CallResolver::planCallWithElements },
            { { Opcode::CallList }, &CallResolver::planPushOfList },
            { { Opcode::InstanceOf }, &CallResolver::planInstanceOf },
            { { Opcode::CallBuiltin }, &CallResolver::planFunctionMethod },
        };
        static_assert([] {
            OpcodeSet planned;
            for (auto& [opcodes, planner] : planners)
                planned |= opcodes;
            return planned == callSiteOpcodes;
        }());
        RELEASE_ASSERT(callSiteOpcodes.contains(site.opcode));
        if (resolvesOnlyThroughTable)
            return planTableCall(site);

        Plan result = declined;
        for (auto& [opcodes, planner] : planners) {
            if (!opcodes.contains(site.opcode))
                continue;
            Plan planned = (this->*planner)(site);
            if (planned)
                return planned;
            if (planned.error() != CallDeclineReason::None)
                result = WTF::move(planned);
        }
        return result;
    }

    void reportDecline(const CallSite& site, CallDeclineReason decline)
    {
        if (decline != CallDeclineReason::None && decline != CallDeclineReason::NeedsTest && reportsDeclinesIn(site.position.block))
            context.addDecline(Wire::DeclineKind::HotCallNotResolved, site.position.value, decline, decline == CallDeclineReason::TooManyCallees ? LimitID::MaxDispatchCases : LimitID::None);
    }

    const Type& typeOf(ValueID id) const { return program.typeOf(function, id); }
    PropertyAccessContext accessContext() const { return { program, &flow }; }
    bool reportsDeclinesIn(BlockID block) { return context.build.options().collectsDeclines() && isHot(heatClassOf(context, block)).predicted(); }
    Expected<bool> isRare(const CallSite& site) { return context.analyses.blockFrequencies().classOf(site.position.block) == BlockFrequency::Rare; }
    bool hasFailedEarlier(const Value& test, const CallSite& site) { return context.analyses.findEarlierTest(test, site.position.block) == EarlierTest::Failed; }
    bool isProvenPristine(ValueID value, const PristineRequirement& requirement, const CallSite& site) { return pristineProofOf({ program, function, &context.analyses }, value, requirement, site.position.point()) == PristineProof::Proven; }
    Value makeInstanceTest(ValueID subject, LayoutSetID instances) { return makeTypeTest(subject, context.types.intern(Type::forLayoutSet(instances))); }

    bool areSameObject(ValueID a, ValueID b)
    {
        if (a == b)
            return true;
        context.planAsksAboutPlace = true;
        return context.analyses.identities().areSameValue(a, b);
    }

    const Value* findMethodRead(const CallSite& site, Opcode readOpcode)
    {
        RELEASE_ASSERT(site.opcode == Opcode::Call && readsOfMembers.contains(readOpcode));
        if (site.memberRead.isEmpty())
            return nullptr;
        const Value& read = function.at(site.memberRead.read().value);
        ValueID thisValue = function.at(site.position.value).thisValueOperand();
        if (read.opcode != readOpcode || !thisValue)
            return nullptr;
        return areSameObject(readOpcode == Opcode::SuperGet ? read.receiverOperand() : read.objectOperand(), thisValue) ? &read : nullptr;
    }

    bool canRetireMemberRead(const CallSite& site, FunctionID callee) const
    {
        Type calleeType = Type::forFunction(callee);
        return std::ranges::all_of(site.memberRead.links, [&](const ValueSite& link) { return typeOf(link.value).overlaps(calleeType, program); });
    }

    // An arm of a dispatch may be for the method of a class that the receiver is known not to have: `this.describe()` of a base constructor, inlined under `new Base()`, has an arm for the method of the subclass.
    bool cannotBeThisOf(const Function& callee, ValueID thisValue) const
    {
        if (!thisValue || !callee.has(FunctionFlags::HasThis) || !callee.thisType.written() || callee.thisType.isExpected())
            return false;
        const Type& declared = program.at(*callee.thisType.established());
        return typeOf(thisValue).hasOnlyLayouts() && declared.hasOnlyLayouts() && !typeOf(thisValue).overlaps(declared, program);
    }

    CallArm directCallArm(FunctionID callee, const MemberConditions& conditions = { }, ValueID closure = { }) const
    {
        return { .kind = CallArmKind::DirectCall, .closureFunction = callee, .callee = callee, .closureCreation = closure ? findClosureCreation(function, closure, callee) : ValueID(), .conditions = conditions };
    }

    void retireReads(const MemberRead& reads, bool checksReceiver, PropertyKeyID key, uint32_t operandIndexInOriginal = OperandIndex::Call::callee)
    {
        RELEASE_ASSERT(expansion.isOpen() && !reads.isEmpty());

        Vector<Value, 2> copies;
        for (const ValueSite& link : reads.links) {
            copies.append(function.at(link.value));
            if (readsOfMembers.contains(copies.last().opcode))
                expansion.addReplaced(copies.last());
        }

        if (checksReceiver) {
            const Value& read = copies.first();
            Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { read.operand(*receiverOf(read.opcode)) });
            check.setNullishAccess(NullishAccess::Read);
            if (const PropertyKey& name = program.at(key); name.kind == PropertyKeyKind::Text)
                check.setName(name.text);
            check.setOrigin(read.origin());
            check.type = resultTypeOf(program, function, check, context.types);
            check.setExpected(expectedTypeOf(program, function, check, context.types));
            function.edit(reads.read().value) = WTF::move(check);
        } else
            becomeUndefined(function, reads.read().value);
        for (const ValueSite& link : reads.links.span().subspan(1))
            becomeUndefined(function, link.value);
        for (ValueID validation : reads.validations)
            becomeUndefined(function, validation);

        expansion.setOperationsBeforeOriginal(WTF::move(copies), operandIndexInOriginal);
        context.analyses.didChangeValues();
    }

    Vector<ValueID, 8> capturesOf(const CallArm& arm, ValueID closure, ValueID receiver)
    {
        if (program.at(arm.closureFunction).captures.isEmpty())
            return { };
        if (arm.capturesReceiver)
            return Vector<ValueID, 8> { receiver };
        if (arm.closureCreation)
            return Vector<ValueID, 8>(function.at(arm.closureCreation).variadicOperands());
        return appendCaptures(builder, arm.closureFunction, closure);
    }

    void branchOnConditions(std::span<const MemberCondition> conditions, const OriginalCopy& original, LayoutID classWhoseGuardsBegin = { })
    {
        RELEASE_ASSERT(expansion.isOpen());
        if (conditions.empty())
            return;

        Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
        BlockID failedUnasked = builder.beginPrediction();
        ValueID areUnchanged = expansion.append(makeConditionsTest(context.types, conditions));
        IRBuilder::BranchBlocks branch = builder.emitBranch(areUnchanged, frequency, rarerOf(frequency, BlockFrequency::Unlikely));
        builder.endPrediction(failedUnasked, branch.notTaken);
        builder.moveToEndOf(branch.notTaken);
        if (classWhoseGuardsBegin) {
            // Guards that have begun say so: the one of the property holds until the program puts another object there.
            MemberCondition property { MemberCondition::Kind::MemberUnchanged, classWhoseGuardsBegin, ClassSide::Static, program.keyOf(BuiltinNames::prototype) };
            ValueID haveBegun = expansion.append(makeConditionsTest(context.types, { &property, 1 }));
            IRBuilder::BranchBlocks begun = builder.emitBranch(haveBegun, function.at(builder.currentBlock()).frequency, BlockFrequency::Rare);
            builder.moveToEndOf(begun.notTaken);
            Value begin = IRBuilder::make(Opcode::BeginMemberGuards);
            begin.setLayout(classWhoseGuardsBegin);
            expansion.append(WTF::move(begin));
            builder.emitJump(begun.taken);
            builder.moveToEndOf(begun.taken);
        }
        expansion.finishArm(expansion.appendOriginal(original));
        builder.moveToEndOf(branch.taken);
    }

    void branchOnOwnInstance(ValueID receiver, const OriginalCopy& original)
    {
        RELEASE_ASSERT(expansion.isOpen());
        Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
        ValueID belongs = expansion.append(makeObjectTest(Proof::OwnInstance, receiver, { }));
        IRBuilder::BranchBlocks branch = builder.emitBranch(belongs, frequency, rarerOf(frequency, BlockFrequency::Unlikely));
        builder.moveToEndOf(branch.notTaken);
        expansion.finishArm(expansion.appendOriginal(original));
        builder.moveToEndOf(branch.taken);
    }

    bool seldomRuns(const CallSite& site)
    {
        context.planAsksAboutPlace = true;
        // It is made for places that run often, and nobody has estimated yet how often it is entered.
        if (isCallWithSharedArms(function))
            return false;
        BlockID block = site.position.block;
        return function.at(block).frequency.predicted() != BlockFrequency::Normal || function.runsOnlyBehindFailedTest() || isCold(heatClassOf(context, block)).predicted();
    }

    // The test costs code size, so rarely run code keeps the single indirect call. A renamed function stays renamed, so the call after a failed test is left as is.
    bool paysToTestFunctionNames(const CallSite& site)
    {
        if (seldomRuns(site))
            return false;
        // What a function of the library is handed is mostly the caller's own function object. Where the body is copied into the caller, that one is called without a test.
        if (isLibraryCode(program, function) && function.at(sameObjectThroughCasts(function, function.at(site.position.value).calleeOperand())).opcode == Opcode::Parameter)
            return false;
        return !hasFailedEarlier(IRBuilder::make(Opcode::AreFunctionNamesUnchanged), site);
    }

    void branchOnFunctionNames()
    {
        RELEASE_ASSERT(expansion.isOpen());
        Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
        BlockID failedUnasked = builder.beginPrediction();
        ValueID areUnchanged = expansion.append(IRBuilder::make(Opcode::AreFunctionNamesUnchanged));
        IRBuilder::BranchBlocks branch = builder.emitBranch(areUnchanged, frequency, rarerOf(frequency, BlockFrequency::Unlikely));
        builder.endPrediction(failedUnasked, branch.notTaken);
        builder.moveToEndOf(branch.notTaken);
        expansion.finishArm(expansion.appendOriginal({ }));
        builder.moveToEndOf(branch.taken);
    }

    ValueID appendClosureOfMember(ValueID receiver, PropertyKeyID key, FunctionID accessor, bool isSetter)
    {
        return expansion.append(makeReadOfClosureOfMember(context.types, receiver, key, accessor, isSetter));
    }

    ValueID appendCall(const CallArm& arm, ValueID thisValue, ValueID closure, std::span<const ValueID> arguments)
    {
        RELEASE_ASSERT(expansion.isOpen() && arm.kind == CallArmKind::DirectCall);
        const Function& callee = program.at(arm.callee);
        if (cannotBeThisOf(callee, thisValue))
            return convertTo(builder, thisValue, callee.thisType);
        return appendCallOfArm(arm, thisValue, closure, arguments);
    }

    ValueID appendCallOfArm(const CallArm& arm, ValueID thisValue, ValueID closure, std::span<const ValueID> arguments, ValueID newTarget = { }, ValueID restArray = { })
    {
        const Value& original = expansion.original();
        Vector<ValueID, 8> captures = capturesOf(arm, closure, thisValue);
        return appendDirectCall(builder, {
            .callee = arm.callee,
            .captures = captures.span(),
            .thisValue = thisValue,
            .calleeValue = program.at(arm.callee).has(FunctionFlags::TakesCallee) ? closure : ValueID(),
            .newTarget = newTarget,
            .arguments = arguments,
            .restArray = restArray,
            .accessSite = original.accessSite(),
            .calledBuiltin = original.opcode == Opcode::Call ? original.calledBuiltin() : BuiltinID(),
            .isConstruction = !!newTarget,
            .isNeverInlined = original.opcode == Opcode::Call && original.isNeverInlined(),
            .isAlwaysInlined = newTarget ? program.at(writtenFunctionOf(program, arm.callee)).has(FunctionCapabilities::MakesNoTailCallIfConstructed) : original.opcode == Opcode::Call && original.isAlwaysInlined(),
            .isTailCall = !newTarget && expansion.isTailCall(),
            .calleeKeptInFrame = arm.keepsCalleeInFrame ? closure : ValueID(),
            .calleeIsPredicted = isCallWithSharedArms(function) || !isFollowedTo(program, original, arm.closureFunction),
        });
    }

    ValueID appendTestThatListFits(ValueID list, uint64_t numberOfOtherArguments)
    {
        Value lengthRead = IRBuilder::make(Opcode::PropGet, { list });
        lengthRead.setKey(program.keyOf(BuiltinNames::length));
        ValueID length = expansion.append(IRBuilder::make(Opcode::ToLength, { expansion.append(WTF::move(lengthRead)) }));
        uint64_t limit = context.build.limit(LimitID::MaxReservedStackArguments);
        return expansion.append(IRBuilder::make(Opcode::LessEq, { length, builder.appendInteger(limit - std::min(limit, numberOfOtherArguments)) }));
    }

    // What the class definitions say of a member holds while the bits of its conditions are set, and one test at the site asks for all of them.
    bool canTestConditionsInArm(const CallSite& site, const MemberAnswer& answer)
    {
        if (!answer.receiverProofs.isEmpty())
            return false;
        if (answer.conditions.isEmpty())
            return true;
        // The failing side reads the member again, at the call.
        if (site.opcode == Opcode::Call && !site.memberRead.canSinkToCall)
            return false;
        // A bit that was cleared stays cleared, so the operations behind a failed test stay as they are.
        context.planAsksAboutPlace = true;
        if (holdMemberUnderSymbolOfProgram(program, answer.conditions) && !paysToTestMembersUnderSymbolsOfProgram(context, site.position.block))
            return false;
        return !hasFailedEarlier(makeConditionsTest(context.types, answer.conditions.span()), site);
    }

    std::optional<CallArm> planReceiverArm(const CallSite& site, const MemberArm& arm)
    {
        const MemberAnswer& answer = arm.answer;
        bool isCall = site.opcode == Opcode::Call;
        std::optional<CallArm> originalOperations = isCall && site.memberRead.canSinkToCall ? std::optional(CallArm { }) : std::nullopt;
        switch (answer.kind) {
        case MemberKind::Method:
            if (!isCall || !canTestConditionsInArm(site, answer) || !canCallWithoutClosure(program, answer.method) || !canRetireMemberRead(site, answer.method))
                return std::nullopt;
            if (mayBeNamedOutOfSight(answer.method))
                return originalOperations;
            return directCallArm(answer.method, answer.conditions);
        case MemberKind::Accessor: {
            if (!canTestConditionsInArm(site, answer) || isCall)
                return std::nullopt;
            FunctionID accessor = answer.accessorFor(accessOf(site.opcode));
            if (!accessor && site.opcode == Opcode::PropGet)
                return CallArm { .kind = CallArmKind::Undefined, .conditions = answer.conditions };
            if (!accessor || !canCallAccessorOfMember(program, answer, accessor))
                return std::nullopt;
            CallArm direct = directCallArm(accessor, answer.conditions);
            if (!canCallWithoutClosure(program, accessor)) {
                context.planAsksAboutPlace = true;
                if (!paysToReadClosureOfMember(context, site.position.block))
                    return std::nullopt;
                direct.readsClosureOfMember = true;
                direct.keepsCalleeInFrame = keepsCalleeOfAccessor(program, accessor);
            }
            return direct;
        }
        case MemberKind::Field: {
            if (!isCall)
                return std::nullopt;
            PropertyAccessPlan read = planPropertyAccess(accessContext(), {
                .access = Access::Read,
                .key = function.at(site.memberRead.read().value).key(),
                .receivers = { .proof = ReceiverProof::Type, .level = KnowledgeLevel::Proven, .proven = arm.layouts },
                .maxMembers = context.limit(LimitID::MaxAccessPlanMembers),
                .isOnlyCalled = true,
            });
            if (!canCallDeferredClosure(program, context.types, read) || mayGetNameOfItsOwnOutOfSight(program, flow, read) || !canRetireMemberRead(site, read.member.method))
                return originalOperations;
            CallArm direct = directCallArm(read.member.method);
            direct.capturesReceiver = read.capturesReceiver;
            return direct;
        }
        case MemberKind::Builtin:
        case MemberKind::Variable:
        case MemberKind::Namespace:
            return originalOperations;
        case MemberKind::Absent:
        case MemberKind::Unknown:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Plan planReceiverDispatch(const CallSite& site)
    {
        bool isCall = site.opcode == Opcode::Call;
        RELEASE_ASSERT(isCall || site.opcode == Opcode::PropGet || site.opcode == Opcode::PropSet);
        const Value* found = isCall ? findMethodRead(site, Opcode::PropGet) : &function.at(site.position.value);
        if (!found || found->isFreeName() || (isCall && classOfClassObject(program, typeOf(found->objectOperand()))))
            return declined;
        const Value& access = *found;
        PropertyKeyID key = access.key();
        ValueID receiver = access.objectOperand();

        ReceiverCandidates candidates = receiverCandidates(program, function, receiver, access.receivers());
        if (!candidates.isProven())
            return std::unexpected(!candidates.expected.predicted().isEmpty() ? CallDeclineReason::NeedsTest : isCall ? CallDeclineReason::UnknownMember : CallDeclineReason::None);
        bool checksReceiverAtRead = candidates.mayBeNullish;
        if (checksReceiverAtRead && !isCall)
            return declined;
        // The members, their bits and the variables of what is entered are this instance's. Rare code stays small, and an object of another instance stays one.
        if (hasFailedInstanceTest(context, receiver, site.position.block))
            return declined;
        bool testsOwnInstance = mayBelongToOtherInstance(context, receiver, candidates);
        if (testsOwnInstance && isRare(site).predicted())
            return declined;

        MemberPartition partition = partitionMembers(accessContext(), candidates.proven.span(), key, accessOf(access.opcode));
        auto armsOfPartition = partition.arms.map<Vector<std::optional<CallArm>, 4>>([&](const MemberArm& arm) { return planReceiverArm(site, arm); });
        // `stream.state`, a field of most classes and such an accessor of a few: nothing says that the few come more often, and the arm of the others would be the unlikely one.
        if (!partition.rest.isEmpty() || armsOfPartition.contains(std::nullopt)) {
            for (std::optional<CallArm>& arm : armsOfPartition) {
                if (arm && arm->readsClosureOfMember)
                    arm = std::nullopt;
            }
        }
        if (!armsOfPartition.containsIf([](const std::optional<CallArm>& arm) { return arm && arm->kind != CallArmKind::OriginalOperations; })) {
            if (partition.arms.containsIf([](const MemberArm& arm) { return !arm.answer.needsNoTest(); }))
                return std::unexpected(CallDeclineReason::NeedsTest);
            return std::unexpected(isCall || partition.findArm(MemberKind::Accessor) ? CallDeclineReason::UnknownMember : CallDeclineReason::None);
        }
        context.planAsksAboutPlace = true;

        DispatchPlan dispatch = planDispatch(context, partition, candidates, site.position.value, [&](const MemberArm& arm) { return armsOfPartition[static_cast<size_t>(&arm - partition.arms.begin())].has_value(); });
        if (dispatch.arms.isEmpty())
            return std::unexpected(CallDeclineReason::UnknownMember);

        ValueSplit split = createSplit(context, dispatch, partition, receiver, function.at(site.position.block).frequency);
        if (checksReceiverAtRead) {
            Type covered;
            for (const SplitArm& arm : split.arms)
                covered = covered.unionWith(context.types.at(arm.testedType), context.types);
            split.hasRest = !typeOf(receiver).withoutNullish().isSubsetOf(covered, program);
            split.value = site.memberRead.read().value;
        }

        bool copiesMemberRead = split.hasRest || testsOwnInstance;
        Vector<CallArm, 4> arms;
        for (uint32_t armIndex : dispatch.arms) {
            arms.append(*armsOfPartition[armIndex]);
            arms.last().receivers = context.types.completeLayoutSet(partition.arms[armIndex].layouts.span());
            copiesMemberRead |= arms.last().kind == CallArmKind::OriginalOperations || !arms.last().conditions.isEmpty();
        }
        if (isCall && copiesMemberRead && !site.memberRead.canSinkToCall)
            return std::unexpected(CallDeclineReason::EffectsBeforeCall);
        LayoutSetID restReceivers = dispatch.hasRest() ? context.types.completeLayoutSet(dispatch.rest.span()) : LayoutSetID();

        return [this, &site, isCall, key, checksReceiverAtRead, testsOwnInstance, restReceivers, split = WTF::move(split), arms = WTF::move(arms)] {
            expansion.begin(site.position);
            if (isCall)
                retireReads(site.memberRead, checksReceiverAtRead, key);

            const Value& original = expansion.original();
            uint32_t receiverOperandIndex = isCall ? OperandIndex::Call::thisValue : *receiverOf(site.opcode);
            ValueID stored = site.opcode == Opcode::PropSet ? original.valueOperand() : ValueID();
            std::span<const ValueID> arguments = isCall ? original.variadicOperands() : std::span(&stored, stored ? 1 : 0);
            auto emitArm = [&](uint32_t armIndex, ValueID narrowed) {
                const CallArm& arm = arms[armIndex];
                OriginalCopy copy { receiverOperandIndex, narrowed, arm.receivers, { } };
                if (testsOwnInstance && arm.kind != CallArmKind::OriginalOperations)
                    branchOnOwnInstance(narrowed, copy);
                branchOnConditions(arm.conditions.span(), copy);
                if (arm.kind == CallArmKind::DirectCall && isCall && context.build.validates())
                    appendLookupValidation(builder, arm.closureFunction, narrowed, key, arm.capturesReceiver ? narrowed : ValueID());
                switch (arm.kind) {
                case CallArmKind::DirectCall:
                    return expansion.finishArm(appendCall(arm, narrowed, arm.readsClosureOfMember ? appendClosureOfMember(narrowed, key, arm.closureFunction, !!stored) : ValueID(), arguments));
                case CallArmKind::Undefined:
                    return expansion.finishArm(builder.appendUndefined());
                case CallArmKind::OriginalOperations:
                    return expansion.finishArm(expansion.appendOriginal(copy));
                }
                RELEASE_ASSERT_NOT_REACHED();
            };
            emitSplit(expansion, split, emitArm, [&](ValueID narrowed) { expansion.finishArm(expansion.appendOriginal({ receiverOperandIndex, narrowed, restReceivers, { } })); });
            expansion.end();
            context.build.statistics().count(StatisticID::DispatchArms, arms.size());
        };
    }

    bool canTestConditions(const CallSite& site, const MemberAnswer& answer)
    {
        if (answer.needsNoTest())
            return true;
        if (!isMemberOfClass(answer) || !answer.receiverProofs.isEmpty())
            return false;
        // The failing side reads the member again, at the call.
        if (site.opcode == Opcode::Call && !site.memberRead.canSinkToCall && !site.prototypeMemberRead.reads.canSinkToCall)
            return false;

        // Rare code stays small: there the operations give way only to what takes their place without a branch.
        // A bit that was cleared stays cleared, so the operations behind a failed test stay as they are.
        context.planAsksAboutPlace = true;
        return !isRare(site).predicted() && !hasFailedEarlier(makeConditionsTest(context.types, answer.conditions.span()), site);
    }

    static std::optional<FunctionID> findFunctionEnteredFor(Opcode opcode, const MemberAnswer& answer)
    {
        bool isCall = opcode == Opcode::Call;
        bool isStore = opcode == Opcode::PropSet || opcode == Opcode::SuperSet;
        RELEASE_ASSERT(isCall || isStore || readsOfMembers.contains(opcode));
        switch (answer.kind) {
        case MemberKind::Method:
            return isCall ? std::optional(answer.method) : std::nullopt;
        case MemberKind::Accessor:
            if (isCall || (isStore && !answer.setter))
                return std::nullopt;
            return isStore ? answer.setter : answer.getter;
        case MemberKind::Absent:
            return opcode == Opcode::SuperGet ? std::optional(FunctionID()) : std::nullopt;
        case MemberKind::Unknown:
        case MemberKind::Field:
        case MemberKind::Variable:
        case MemberKind::Namespace:
        case MemberKind::Builtin:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isUsedByCastsAlone(ValueID read) const
    {
        const UseLists& uses = context.analyses.useLists();
        Vector<ValueID, 4> worklist { read };
        while (!worklist.isEmpty()) {
            for (const Use& use : uses.usesOf(worklist.takeLast())) {
                if (use.kind != Use::Kind::Operand || use.operandIndex || !isCast(function.at(use.user).opcode))
                    return false;
                worklist.append(use.user);
            }
        }
        return true;
    }

    // `super.method()` where the method does nothing that shows: the call goes as soon as its callee and its result are known, and leaves the read of the method behind.
    Plan planReadOfMethodThatNobodyTakes(const CallSite& site, const MemberAnswer& answer)
    {
        if (site.opcode != Opcode::SuperGet || answer.kind != MemberKind::Method || !canCallWithoutClosure(program, answer.method) || !isUsedByCastsAlone(site.position.value))
            return declined;
        return [this, &site, method = answer.method, conditions = answer.conditions] {
            expansion.begin(site.position);
            branchOnConditions(conditions.span(), { });
            Value closure = IRBuilder::make(Opcode::NewClosure);
            closure.setFunction(method);
            expansion.finishArm(expansion.append(WTF::move(closure)));
            expansion.end();
        };
    }

    Plan planHolderMember(const CallSite& site, const MemberAnswer& answer, ValueID thisValue)
    {
        if (!answer.isKnown())
            return std::unexpected(CallDeclineReason::UnknownMember);
        if (!canTestConditions(site, answer))
            return std::unexpected(CallDeclineReason::NeedsTest);

        std::optional<FunctionID> entered = findFunctionEnteredFor(site.opcode, answer);
        if (!entered)
            return planReadOfMethodThatNobodyTakes(site, answer);
        FunctionID callee = *entered;
        bool isCall = site.opcode == Opcode::Call;
        bool isStore = site.opcode == Opcode::PropSet || site.opcode == Opcode::SuperSet;
        if (callee && (!canCallWithoutClosure(program, callee) || mayBeNamedOutOfSight(callee)))
            return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
        if (isCall && !canRetireMemberRead(site, callee))
            return declined;
        ValueID stored = callee && isStore ? function.at(site.position.value).valueOperand() : ValueID();

        return [this, &site, isCall, callee, thisValue, stored, conditions = answer.conditions] {
            std::optional<Value> validatedRead = isCall && context.build.validates() ? std::optional(function.at(site.memberRead.read().value)) : std::nullopt;
            RELEASE_ASSERT(!validatedRead || readsOfMembers.contains(validatedRead->opcode));
            expansion.begin(site.position);
            if (isCall)
                retireReads(site.memberRead, false, { });
            branchOnConditions(conditions.span(), { });
            if (validatedRead)
                appendLookupValidation(builder, callee, validatedRead->opcode == Opcode::PropGet ? validatedRead->objectOperand() : validatedRead->baseOperand(), validatedRead->key());
            std::span<const ValueID> arguments = isCall ? expansion.original().argumentsOperands() : std::span(&stored, stored ? 1 : 0);
            expansion.finishArm(callee ? appendCall(directCallArm(callee), thisValue, { }, arguments) : builder.appendUndefined());
            expansion.end();
        };
    }

    Plan planClassObjectMember(const CallSite& site)
    {
        const Value* access = site.opcode == Opcode::Call ? findMethodRead(site, Opcode::PropGet) : &function.at(site.position.value);
        if (!access)
            return declined;
        const Type& classObjectType = typeOf(access->objectOperand());
        LayoutID classLayout = classOfClassObject(program, classObjectType);
        if (!classLayout || classObjectType.mayBeUndefinedOrNull())
            return declined;

        MemberAnswer answer = findMember(accessContext(), { MemberStart::ClassObject, classLayout, access->key(), accessOf(access->opcode) });
        if (answer.kind == MemberKind::Variable)
            return declined;
        return planHolderMember(site, answer, access->objectOperand());
    }

    // The lookup starts at the prototype of an object, not of a class: it finds what a lookup that misses on the object itself goes on to find.
    MemberAnswer findMemberOnPrototypesOfHome(const Value& superAccess, Access access)
    {
        const Value& superBase = function.at(context.analyses.identityOf(superAccess.baseOperand()));
        if (superBase.opcode != Opcode::GetSuperBase || superBase.homeKind() != HomeObjectKind::Object)
            return MemberAnswer::unknown(UnprovenCause::Unknown);

        ReceiverCandidates homes = receiverCandidates(program, function, superBase.homeOperand());
        if (!homes.isProven() || homes.mayBeNullish || homes.proven.size() > context.limit(LimitID::MaxAccessPlanMembers))
            return MemberAnswer::unknown(UnprovenCause::Unknown);

        MemberArm arm;
        for (LayoutID layout : homes.proven) {
            MemberAnswer found = findMemberOnPrototypes(accessContext(), { MemberStart::Instance, layout, superAccess.key(), access });
            if (arm.layouts.isEmpty())
                arm.answer = found;
            else if (!belongToOneArm(arm.answer, found))
                return MemberAnswer::unknown(UnprovenCause::Unknown);
            mergeIntoArm(arm, layout, found);
        }
        return arm.answer;
    }

    Plan planSuperMember(const CallSite& site)
    {
        const Value* access = site.opcode == Opcode::Call ? findMethodRead(site, Opcode::SuperGet) : &function.at(site.position.value);
        if (!access || !access->key())
            return declined;

        Access lookup = access->opcode == Opcode::SuperSet ? Access::Store : Access::Read;
        std::optional<MemberAnswer> aboveClass = findSuperMember(accessContext(), function, *access, lookup);
        MemberAnswer answer = aboveClass ? WTF::move(*aboveClass) : findMemberOnPrototypesOfHome(*access, lookup);
        if (!aboveClass && !answer.isKnown())
            return declined;
        if (answer.kind == MemberKind::Field || answer.kind == MemberKind::Variable)
            return std::unexpected(CallDeclineReason::UnknownMember);
        return planHolderMember(site, answer, access->receiverOperand());
    }

    Plan planPrototypeMember(const CallSite& site)
    {
        const PrototypeMemberRead& read = site.prototypeMemberRead;
        // Both reads go to the side where the conditions fail.
        if (!read || !read.reads.canSinkToCall || !isEvaluatedOnce(program, program.at(read.classLayout)))
            return declined;
        // The body of a module runs once, and it is where the prototype objects are filled: the class is not to be defined by a read in the middle of that.
        AccessSiteID accessSite = function.at(read.reads.read().value).accessSite();
        FunctionID writtenIn = flow.hasResults() && accessSite ? flow.at(accessSite).function : FunctionID();
        if (writtenIn && program.at(writtenIn).isModuleBody())
            return declined;
        MemberAnswer answer = findMember(accessContext(), { MemberStart::ClassPrototype, read.classLayout, read.key, Access::Read });
        if (answer.kind != MemberKind::Method || !program.at(answer.method).captures.isEmpty())
            return declined;
        // While the condition holds, the property holds the prototype object of the class. No code can change the property of a class.
        bool isConstructorFunction = program.at(read.classLayout).has(LayoutFlags::IsConstructorFunction);
        if (isConstructorFunction && program.at(read.classLayout).classFlow.prototypeProperty != MemberMutability::Fixed) {
            addCondition(answer.conditions, { MemberCondition::Kind::MemberUnchanged, read.classLayout, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
            setLevel(answer);
        }
        if (!canTestConditions(site, answer))
            return std::unexpected(CallDeclineReason::NeedsTest);

        bool isCall = site.opcode == Opcode::Call;
        if (isCall && (!function.at(site.position.value).thisValueOperand() || !canCallWithoutClosure(program, answer.method) || mayBeNamedOutOfSight(answer.method)))
            return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
        if (!isCall && (!answer.holder || !isEvaluatedOnce(program, program.at(answer.holder))))
            return declined;

        return [this, &site, &read, isCall, isConstructorFunction, method = answer.method, conditions = WTF::move(answer.conditions)] {
            expansion.begin(site.position);
            retireReads(read.reads, false, { }, isCall ? OperandIndex::Call::callee : OperandIndex::PropGet::object);
            // The guards of a constructor function begin with the first object that it makes, and many a base makes none.
            branchOnConditions(conditions.span(), { }, isConstructorFunction ? read.classLayout : LayoutID());

            if (context.build.validates()) {
                Value prototype = IRBuilder::make(Opcode::GetClassPrototype, { read.classObject });
                prototype.setLayout(read.classLayout);
                appendLookupValidation(builder, method, expansion.append(WTF::move(prototype)), read.key);
            }
            if (isCall)
                expansion.finishArm(appendCall(directCallArm(method), expansion.original().thisValueOperand(), { }, expansion.original().argumentsOperands()));
            else {
                Value closure = IRBuilder::make(Opcode::NewClosure);
                closure.setFunction(method);
                closure.setIsHeldByClosureTable(true);
                expansion.finishArm(expansion.append(WTF::move(closure)));
            }
            expansion.end();
        };
    }

    bool passesNumbersInRegisters(FunctionID id) const
    {
        auto isNumberInRegister = [&](const DeclaredType& declared) {
            Representation representation = declaredRepresentationOf(program, declared);
            return representation == Representation::Int32 || isInt64OrMarker(representation) || isDoubleOrMarker(representation);
        };
        const Function& callee = program.at(id);
        return std::ranges::any_of(callee.results, isNumberInRegister) || std::ranges::any_of(callee.parameters, isNumberInRegister);
    }

    // Proven or only expected: a number that is only expected is passed as any value.
    bool takesOrReturnsNumbers(FunctionID id) const
    {
        auto isNumber = [&](const DeclaredType& declared) {
            Type type = program.at(declared.written()).withoutNullish();
            return !type.isNever() && type.isSubsetOf(program.at(TypeTable::number), program);
        };
        const Function& callee = program.at(id);
        return std::ranges::any_of(callee.results, isNumber) || std::ranges::any_of(callee.parameters, isNumber);
    }

    uint64_t mostArmsWhereNumbersStayInRegisters() const
    {
        AOT_THRESHOLD(uint64_t, armFactorWhenNumbersStayInRegisters, 2);
        return context.limit(LimitID::MaxDispatchCases) * armFactorWhenNumbersStayInRegisters;
    }

    std::expected<Vector<FunctionID, 4>, CallDeclineReason> chooseDispatchedCallees(const CallSite& site, const CalleeCandidates& candidates, bool (*canEnter)(const Program&, FunctionID), uint64_t maxExpectedCallees = 1)
    {
        if (!candidates.isDispatchedAtOperation())
            return std::unexpected(candidates.hasOneExpectedCallee() ? CallDeclineReason::NeedsTest : CallDeclineReason::UnknownCallee);
        bool listsEveryCallee = !!candidates.all();
        // What stays behind the arms for expected callees expects them as well.
        if (!listsEveryCallee) {
            context.planAsksAboutPlace = true;
            if (context.analyses.hasFailedTestOf(candidates.value)) {
                for (FunctionID member : candidates.listed().predicted()) {
                    if (hasFailedEarlier(makeTypeTest(candidates.value, context.types.intern(Type::forFunction(member))), site))
                        return declined;
                }
            }
        }

        // Callees get an arm each or none: arms for some of them would be chosen by id, and the next run would take as many from the rest.
        // An indirect call boxes each number it passes, the callee's entry unboxes it, and the same happens for the result. A few more comparisons cost less than that.
        // The function that has the arms for all places has one for each callee.
        bool hasArmsForAllPlaces = isCallWithSharedArms(function);
        // One member more than any limit below allows decides the answer. Many call sites may list the same thousands of callees.
        uint64_t mostMembersWithArms = hasArmsForAllPlaces ? std::numeric_limits<uint64_t>::max() : std::max<uint64_t>({ 1, maxExpectedCallees, mostArmsWhereNumbersStayInRegisters() });
        Vector<FunctionID, 4> members;
        for (FunctionID member : candidates.listed().predicted()) {
            if (!canEnter(program, member))
                continue;
            // Many arms cost no more than few where each tests one word for a constant. A closure that is not slim takes more to tell: it is called as a value behind the arms.
            if (hasArmsForAllPlaces && closureFormOf(program, flow, member) != ClosureForm::Slim)
                continue;
            members.append(member);
            if (members.size() > mostMembersWithArms)
                break;
        }
        if (members.isEmpty())
            return std::unexpected(CallDeclineReason::CalleeNotCallable);
        // What exceeds a limit is reported, site by site.
        context.planAsksAboutPlace |= listsEveryCallee || members.size() <= 1;
        bool isWithinWiderLimit = hasArmsForAllPlaces || (listsEveryCallee && members.size() <= mostArmsWhereNumbersStayInRegisters() && std::ranges::all_of(members, [&](FunctionID member) { return passesNumbersInRegisters(member); }));
        if ((!listsEveryCallee && !hasArmsForAllPlaces && members.size() > maxExpectedCallees) || (!isWithinWiderLimit && context.exceeds(LimitID::MaxDispatchCases, members.size(), site.position.value)))
            return std::unexpected(CallDeclineReason::TooManyCallees);
        // The value of a provisional id depends on which thread asked for the function first.
        AOT::sort(members, [&](FunctionID a, FunctionID b) { return context.inputs.derivedFunctions->compare(a, b) < 0; });
        return members;
    }

    ValueSplit createCalleeSplit(const CallSite& site, const CalleeCandidates& candidates, std::span<const FunctionID> dispatchedCallees)
    {
        Expected<BlockFrequency> frequency = function.at(site.position.block).frequency;
        ValueSplit split;
        split.value = candidates.value;
        Type covered;
        for (FunctionID member : dispatchedCallees) {
            split.arms.append(SplitArm { context.types.intern(Type::forFunction(member)), frequency });
            covered.functions.add(member);
        }

        split.hasRest = !typeOf(candidates.value).isSubsetOf(covered, program);
        // A function without a body is one that nothing enters.
        bool leavesOutListedCallee = std::ranges::any_of(candidates.listed().predicted(), [&](FunctionID listed) { return program.at(listed).hasBody() && !covered.functions.contains(listed); });
        // What bind returned is no closure of a function that the type lists.
        const Value& operation = function.at(site.position.value);
        bool restWasSeen = operation.opcode == Opcode::Call && operation.accessSite() && flow.hasResults() && !flow.boundFunctionsSeenAt(operation.accessSite(), FlowBoundFunctionUseAtSite::Called).predicted().empty();
        split.restFrequency = leavesOutListedCallee || restWasSeen || candidates.arriveOnWaysOfTheirOwn ? frequency : rarerOf(frequency, BlockFrequency::Unlikely);
        return split;
    }

    void emitCalleeDispatch(CalleeProof proof, const ValueSplit& split, uint32_t calleeOperandIndex, NOESCAPE const EmitSplitArm& emitArm)
    {
        if (proof == CalleeProof::Type)
            appendCalleeValidation(builder, split.value);
        emitSplit(expansion, split, emitArm, [&](ValueID narrowed) { expansion.finishArm(expansion.appendOriginal({ calleeOperandIndex, narrowed })); });
        expansion.end();
    }

    // Many call sites may list the same thousands of callees: a long list of the program is asked once.
    PropertyAccessCache::NamesOfCallees namesOfCallees(const CalleeCandidates& candidates)
    {
        using Names = PropertyAccessCache::NamesOfCallees;
        std::span<const FunctionID> listed = candidates.listed().predicted();
        auto ask = [&] {
            Names names { listed };
            for (FunctionID member : listed) {
                bool whileNamesAreUnchanged = mayBeCalledDirectlyWhileFunctionNamesAreUnchanged(program, member);
                names.someRecordsCalleeOnlyOnceRenamed |= recordsCalleeOnlyOnceRenamed(member);
                names.someIsCalledDirectlyWhileNamesAreUnchanged |= whileNamesAreUnchanged;
                names.someIsCalledDirectlyOnlyWhileNamesAreUnchanged |= whileNamesAreUnchanged && !mayBeCalledDirectly(program, member);
                // A function without a body is one that nothing enters.
                if (!program.at(member).hasBody())
                    continue;
                names.allTakeOrReturnNumbersInDirectCalls &= mayBeCalledDirectlyWhereCalleeIsKept(program, member) && takesOrReturnsNumbers(member);
                names.somePassesNumbersInRegisters |= passesNumbersInRegisters(member) && closureFormOf(program, flow, member) == ClosureForm::Slim;
            }
            names.allComeBack = names.allTakeOrReturnNumbersInDirectCalls && names.somePassesNumbersInRegisters && std::ranges::all_of(listed, [&](FunctionID member) { return !program.at(member).hasBody() || comesBack(member); });
            return names;
        };
        AOT_THRESHOLD(size_t, fewestCalleesAskedOnce, 16);
        PropertyAccessCache* cache = program.propertyAccessCache();
        if (!cache || listed.size() < fewestCalleesAskedOnce || !candidates.listsListOfProgram())
            return ask();
        auto isWanted = [&](const Names& names) {
            return names.callees.data() == listed.data() && names.callees.size() == listed.size();
        };
        uint32_t hash = static_cast<uint32_t>(std::bit_cast<uintptr_t>(listed.data()) * 0x9e3779b97f4a7c15ull >> 32);
        const Names* known = cache->namesOfCallees.find(hash, isWanted);
        if (!known)
            known = &cache->namesOfCallees.add(hash, ask(), isWanted);
        if (cache->checksAnswers) {
            Names fresh = ask();
            RELEASE_ASSERT(known->someRecordsCalleeOnlyOnceRenamed == fresh.someRecordsCalleeOnlyOnceRenamed && known->someIsCalledDirectlyWhileNamesAreUnchanged == fresh.someIsCalledDirectlyWhileNamesAreUnchanged
                && known->someIsCalledDirectlyOnlyWhileNamesAreUnchanged == fresh.someIsCalledDirectlyOnlyWhileNamesAreUnchanged && known->allTakeOrReturnNumbersInDirectCalls == fresh.allTakeOrReturnNumbersInDirectCalls
                && known->somePassesNumbersInRegisters == fresh.somePassesNumbersInRegisters && known->allComeBack == fresh.allComeBack);
        }
        return *known;
    }

    // The type lists more callees than a place gets arms for. Each of them takes or returns a number, and some of them in a register: a call of the value would box every number on the way there and back.
    // One function has the arms, for all the places that call one of these functions: a place calls that function, with the numbers in registers. The arms test one word for constants, which makes a jump table.
    Plan planCallWithSharedArms(const CallSite& site, const CalleeCandidates& candidates)
    {
        const Value& call = function.at(site.position.value);
        if (!candidates.all() || candidates.listed().predicted().size() <= mostArmsWhereNumbersStayInRegisters() || isCallWithSharedArms(function))
            return declined;
        std::span<const ValueID> arguments = call.argumentsOperands();
        // The word of a closure names its function where the program runs once.
        if (call.calledBuiltin() || arguments.size() > SharedArms::maxNumberOfArguments || flow.programInstances() != ProgramInstances::AtMostOne)
            return declined;
        // The function is found under a record of the program that lists the callees: a type that this pass has made means nothing to another function.
        SharedArms arms { 0, false, static_cast<uint32_t>(arguments.size()), !!call.thisValueOperand(), 0 };
        TypeID functions = context.types.intern(typeOf(candidates.value).withoutNullish());
        if (!TypeTable::isProvisional(functions))
            arms.list = functions.index();
        else if (TypeID type = function.at(candidates.value).type; !TypeTable::isProvisional(type))
            arms.list = type.index();
        else if (call.callees() && !TypeTable::isProvisional(call.callees()) && std::ranges::equal(program.at(call.callees()).written().functions, candidates.listed().predicted())) {
            arms.list = call.callees().index();
            arms.namesCalleeSet = true;
        } else
            return declined;
        PropertyAccessCache::NamesOfCallees names = namesOfCallees(candidates);
        if (!names.allTakeOrReturnNumbersInDirectCalls || !names.somePassesNumbersInRegisters || seldomRuns(site))
            return declined;
        // The shared function returns from its call of the value.
        bool hasToComeBack = isTailCall(function, site.position) && function.at(site.position.block).terminator.requirement() == TailCallRequirement::Required;
        if (hasToComeBack && !names.allComeBack)
            return declined;

        for (size_t index = 0; index < std::min<size_t>(arguments.size(), SharedArms::numberOfArgumentsPassedAsNumbers); ++index) {
            if (typeOf(arguments[index]).isSubsetOf(program.at(TypeTable::number), program))
                arms.numbers |= 1u << index;
        }
        // The call of undefined or null throws where it stands. The test is for those two: what a test for the functions leaves of the type takes a look at each pair of them.
        Expected<BlockFrequency> frequency = function.at(site.position.block).frequency;
        ValueSplit split { .value = candidates.value, .arms = { }, .hasRest = true, .restFrequency = frequency };
        split.arms.append(SplitArm { TypeTable::nullish, rarerOf(frequency, BlockFrequency::Rare) });
        return [this, &site, proof = candidates.proof(), mayBeNullish = candidates.mayBeNullish, split = WTF::move(split), shared = context.derivedFunction(arms.key())] {
            expansion.begin(site.position);
            const Value& original = expansion.original();
            if (proof == CalleeProof::Type)
                appendCalleeValidation(builder, split.value);
            auto callSharedArms = [&](ValueID closure) {
                Vector<ValueID, 8> passed { closure };
                if (original.thisValueOperand())
                    passed.append(original.thisValueOperand());
                passed.append(original.argumentsOperands());
                expansion.finishArm(appendDirectCall(builder, { .callee = shared, .arguments = passed.span(), .accessSite = original.accessSite(), .isNeverInlined = true, .isTailCall = expansion.isTailCall() || original.isTailCall }));
            };
            if (mayBeNullish) {
                emitSplit(expansion, split, [&](uint32_t, ValueID nullish) {
                    expansion.finishArm(expansion.appendOriginal({ OperandIndex::Call::callee, nullish }));
                }, [&](ValueID closure) {
                    callSharedArms(closure);
                });
            } else
                callSharedArms(split.value);
            expansion.end();
        };
    }

    Plan planCalleeDispatch(const CallSite& site)
    {
        RELEASE_ASSERT(site.opcode == Opcode::Call);
        const Value& call = function.at(site.position.value);
        CalleeCandidates candidates = calleeCandidatesOf(context, call, call.calleeOperand(), site.position.block);
        if (Plan shared = planCallWithSharedArms(site, candidates))
            return shared;
        // Once a function object gets a name where the compiler cannot see it, a frame is named by the function object that was called. The caller keeps it in its own frame, which a tail call gives up.
        if (!isTailCall(function, site.position) && !call.isTailCall) {
            auto members = chooseDispatchedCallees(site, candidates, mayBeCalledDirectlyWhereCalleeIsKept);
            if (!members)
                return std::unexpected(members.error());
            auto arms = members->map<Vector<CallArm, 4>>([&](FunctionID member) {
                CallArm arm = directCallArm(member, { }, candidates.value);
                arm.keepsCalleeInFrame = isNamedByCallee(site, candidates.value, member);
                return arm;
            });
            // Reading the captures and keeping the callee takes more bytes than the call of the value.
            if (std::ranges::any_of(arms, &CallArm::keepsCalleeInFrame) && seldomRuns(site))
                return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
            // A stack that is taken before the first function object is renamed is read behind it.
            if (std::ranges::any_of(arms, [&](const CallArm& arm) { return arm.keepsCalleeInFrame && canLeaveByTailCall(program, arm.callee); }))
                return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
            return [this, &site, proof = candidates.proof(), arms = WTF::move(arms), split = createCalleeSplit(site, candidates, members->span())] {
                expansion.begin(site.position);
                const Value& original = expansion.original();
                emitCalleeDispatch(proof, split, OperandIndex::Call::callee, [&](uint32_t armIndex, ValueID narrowed) {
                    expansion.finishArm(appendCall(arms[armIndex], original.thisValueOperand(), narrowed, original.variadicOperands()));
                });
            };
        }
        PropertyAccessCache::NamesOfCallees names = namesOfCallees(candidates);
        if (names.someRecordsCalleeOnlyOnceRenamed) {
            context.planAsksAboutPlace = true;
            if (haveFunctionNamesChanged(site))
                return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
            // A stack that is taken before the first name is given is read behind it, and above a frame that was given up only the entrypoint for calls by value says who runs.
            bool mayGetNameOutOfSight = std::ranges::any_of(candidates.listed().predicted(), [&](FunctionID member) {
                return recordsCalleeOnlyOnceRenamed(member) && (flow.closuresMayBeNamedOutOfSight(member) || callsOwnClosureThatEscapes(site, candidates.value, member));
            });
            if (mayGetNameOutOfSight)
                return std::unexpected(CallDeclineReason::CalleeNeedsClosure);
        }
        auto isEnteredByValueOnceRenamed = [&](FunctionID member) { return !mayBeCalledDirectly(program, member) && mayBeCalledDirectlyWhileFunctionNamesAreUnchanged(program, member); };
        bool mayTestNames = names.someIsCalledDirectlyOnlyWhileNamesAreUnchanged && paysToTestFunctionNames(site);
        auto members = chooseDispatchedCallees(site, candidates, mayTestNames ? mayBeCalledDirectlyWhileFunctionNamesAreUnchanged : mayBeCalledDirectly);
        if (!members)
            return std::unexpected(members.error());

        bool testsNames = mayTestNames && std::ranges::any_of(*members, isEnteredByValueOnceRenamed);
        auto arms = members->map<Vector<CallArm, 4>>([&](FunctionID member) {
            return directCallArm(member, { }, candidates.value);
        });
        return [this, &site, testsNames, proof = candidates.proof(), arms = WTF::move(arms), split = createCalleeSplit(site, candidates, members->span())] {
            expansion.begin(site.position);
            if (testsNames)
                branchOnFunctionNames();
            const Value& original = expansion.original();
            emitCalleeDispatch(proof, split, OperandIndex::Call::callee, [&](uint32_t armIndex, ValueID narrowed) {
                expansion.finishArm(appendCall(arms[armIndex], original.thisValueOperand(), narrowed, original.variadicOperands()));
            });
        };
    }

    // The one entry of a cycle stands in front of all of it. So a way round that leaves the creation out lies in a cycle that leaves it out.
    bool liesOnWayRoundWithoutCreation(BlockID block, BlockID made)
    {
        if (block == made)
            return false;
        const BlockCycles& cycles = context.analyses.blockCycles();
        std::optional<unsigned> innermost = cycles.innermostCycleOf(block);
        return innermost && (!cycles.forest()[*innermost].isReducible() || !cycles.forest().contains(*innermost, made));
    }

    // Whether a way leads from the end of one block to the start of the other on which the function object is not made anew. The creation stands in front of the second block on every way to it.
    bool leadsOnward(BlockID from, BlockID to, BlockID made)
    {
        CFG& cfg = context.analyses.cfg();
        if (!cfg.isReachable(from) || !cfg.isReachable(to))
            return cfg.isReachable(from);
        Dominators& dominators = context.analyses.dominators();
        if (!dominators.dominates(made, from))
            return false;

        // Against the edges, as far as the blocks that stand in front of the first block: a way from it to one of those is a way round.
        AOT_THRESHOLD(unsigned, maxEntrances, 32);
        Vector<BlockID, 8> entrances;
        Vector<BlockID, 8> worklist { to };
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (dominators.dominates(block, from)) {
                if (liesOnWayRoundWithoutCreation(block, made))
                    return true;
                continue;
            }
            if (dominators.dominates(from, block))
                return true;
            // Every way from the first block to this one comes by the highest block in front of this one that does not stand in front of the first as well.
            BlockID entrance = *std::ranges::find_if(dominators.children(dominators.nearestCommonDominator(block, from)), [&](BlockID child) { return dominators.dominates(child, block); });
            if (entrances.contains(entrance))
                continue;
            entrances.append(entrance);
            if (entrances.size() > maxEntrances)
                return true;
            for (BlockID predecessor : cfg.predecessors(entrance)) {
                if (predecessor == from)
                    return true;
                if (cfg.isReachable(predecessor) && !dominators.dominates(entrance, predecessor))
                    worklist.append(predecessor);
            }
        }
        return false;
    }

    bool leadsToCall(const Use& use, const CallSite& site, BlockID made)
    {
        // What an edge carries is handed on where the edge leads.
        BlockID from = use.isEdgeUse() ? targetOf(function, use.edge()).block : use.block;
        if (from == site.position.block && (use.isEdgeUse() || (use.kind == Use::Kind::Operand && context.analyses.useLists().locationOf(use.user).index <= site.position.index)))
            return true;
        return leadsOnward(from, site.position.block, made);
    }

    bool isReachedFromCall(const Use& use, const CallSite& site, BlockID made)
    {
        if (use.block == site.position.block && (use.kind != Use::Kind::Operand || context.analyses.useLists().locationOf(use.user).index >= site.position.index))
            return true;
        return leadsOnward(site.position.block, use.block, made);
    }

    // A look stores nothing in the object and runs no code that gets it: no accessor, no trap, no conversion.
    bool onlyLooksAt(const Value& user, uint32_t operandIndex)
    {
        if (!user.operandUses(operandIndex).containsOnly(OperandUse::Inspects) || isWrittenOperand(user.opcode, std::min(operandIndex, numberOfNamedOperandsOf(user.opcode))))
            return false;
        return summarizeOperand(context.analyses.effectContext(), function, user, operandIndex).uses.containsOnly(OperandUse::Inspects);
    }

    // A stack names its frames when it is read, which may be long behind the call. A function object that the function makes itself, and that is only called on every way through the call, has the names it was made with for as long as there is such a stack.
    bool isHandedToNobodyAround(const CallSite& site, ValueID closureCreation)
    {
        if (!closureCreation)
            return false;
        const Value& creation = function.at(closureCreation);
        const Function& callee = program.at(creation.function());
        if (creation.isMadeOnDemand() || namesClosureOfTable(program, creation) || callee.has(FunctionFlags::TakesCallee))
            return false;
        return !escapesAround(site, closureCreation);
    }

    bool escapesAround(const CallSite& site, ValueID closureCreation)
    {
        const UseLists& uses = context.analyses.useLists();
        BlockID made = uses.blockOf(closureCreation);
        Vector<ValueID, 4> sameObject { closureCreation };
        for (size_t position = 0; position < sameObject.size(); ++position) {
            for (const Use& use : uses.usesOf(sameObject[position])) {
                if (use.kind == Use::Kind::Operand) {
                    const Value& user = function.at(use.user);
                    if (isCast(user.opcode) && !use.operandIndex) {
                        sameObject.append(use.user);
                        continue;
                    }
                    // A construction hands the function object to its body as new.target.
                    bool isCalled = (user.opcode == Opcode::Call && use.operandIndex == OperandIndex::Call::callee) || (user.opcode == Opcode::CallList && use.operandIndex == OperandIndex::CallList::callee);
                    if (isCalled || onlyLooksAt(user, use.operandIndex))
                        continue;
                }
                if (leadsToCall(use, site, made) || isReachedFromCall(use, site, made))
                    return true;
            }
        }
        return false;
    }

    bool recordsCalleeOnlyOnceRenamed(FunctionID callee) { return Typed::recordsCalleeOnlyOnceRenamed(program, callee); }

    bool mayBeNamedOutOfSight(FunctionID callee) { return recordsCalleeOnlyOnceRenamed(callee) && flow.closuresMayBeNamedOutOfSight(callee); }

    bool haveFunctionNamesChanged(const CallSite& site) { return hasFailedEarlier(IRBuilder::make(Opcode::AreFunctionNamesUnchanged), site); }

    bool isNamedByCallee(const CallSite& site, ValueID closure, FunctionID callee)
    {
        return !mayBeCalledDirectly(program, callee) || isNamedByCalleeOnceRenamed(site, closure, callee);
    }

    bool isNamedByCalleeOnceRenamed(const CallSite& site, ValueID closure, FunctionID callee)
    {
        if (!recordsCalleeOnlyOnceRenamed(callee))
            return false;
        context.planAsksAboutPlace = true;
        return isAccessor(program.at(callee)) || flow.closuresMayBeNamedOutOfSight(callee) || callsOwnClosureThatEscapes(site, closure, callee);
    }

    bool callsOwnClosureThatEscapes(const CallSite& site, ValueID closure, FunctionID callee)
    {
        ValueID closureCreation = findClosureCreation(function, closure, callee);
        return closureCreation && escapesAround(site, closureCreation);
    }

    // A call of the one function that its callee can be, whose result is what the call gives: no test, no arm, no block.
    // There is a plan only where planCalleeDispatch() plans one arm without a test and that arm ends with the result of the call as it is.
    Rewriting planCallOfOnlyCallee(const CallSite& site)
    {
        if (resolvesOnlyThroughTable || site.opcode != Opcode::Call || !site.memberRead.isEmpty())
            return nullptr;
        const Value& call = function.at(site.position.value);
        // Where calleeCandidates() proves one function and admits nothing else, not even undefined: that function, and the value whose type says it.
        ValueID closure = findStrongestLink(program, function, call.calleeOperand());
        FunctionID only = typeOf(closure).singleFunction();
        if (!only || typeOf(closure).mayBeUndefinedOrNull() || !mayBeCalledDirectlyOrThroughEntrypoint(program, only) || context.limit(LimitID::MaxDispatchCases) < 1 || isTailCall(function, site.position))
            return nullptr;
        // A frame without a record shows the last name given to any function object of the function, unless the call records that it enters the caller's own, or keeps the function object where the caller's frame shows it.
        bool callsOwnFunctionObject = !mayBeCalledDirectly(program, only) || (recordsCalleeOnlyOnceRenamed(only) && (isAccessor(program.at(only)) || flow.closuresMayBeNamedOutOfSight(only)));
        bool keepsCalleeInFrame = false;
        if (callsOwnFunctionObject && (call.isTailCall || call.calledBuiltin() || !isHandedToNobodyAround(site, findClosureCreation(function, closure, only)))) {
            if (call.isTailCall || !mayBeCalledDirectlyWhereCalleeIsKept(program, only))
                return nullptr;
            callsOwnFunctionObject = false;
            keepsCalleeInFrame = true;
        }
        if (!callsOwnFunctionObject && !keepsCalleeInFrame && recordsCalleeOnlyOnceRenamed(only) && callsOwnClosureThatEscapes(site, closure, only)) {
            if (call.isTailCall)
                return nullptr;
            keepsCalleeInFrame = true;
        }
        // A small accessor is inlined, and nothing is left of the call.
        if (keepsCalleeInFrame && !isAccessor(program.at(only)) && seldomRuns(site))
            return nullptr;
        // A stack that is taken before the first function object is renamed is read behind it.
        if (keepsCalleeInFrame && !isAccessor(program.at(only)) && canLeaveByTailCall(program, only))
            return nullptr;
        // appendCall() puts no call where the receiver cannot be what the callee takes.
        if (cannotBeThisOf(program.at(only), call.thisValueOperand()))
            return nullptr;
        TypeID result = TypingContext { program, function, context.types }.typeIDOf(program.at(only).result());
        if (TypeTable::isProvisional(result) || result == TypeTable::never || !program.at(result).isSubsetOf(typeOf(site.position.value), program))
            return nullptr;

        return [this, &site, only, closure, callsOwnFunctionObject, keepsCalleeInFrame, closureCreation = findClosureCreation(function, closure, only)] {
            const Value original = function.at(site.position.value);
            const Function& callee = program.at(only);
            builder.setOrigin(original.origin());
            // ScopedLambda hands its arguments to the functor as lvalues.
            auto insertBeforeCall = [&](auto&& value) { return builder.insertBefore(site.position.point(), WTF::move(value)); };
            InsertValue insert = insertBeforeCall;
            ValueInsertion insertion { builder.context(), function, context.types, original.origin(), insert };

            if (context.build.validates())
                insert(makeValidateCallee(sameObjectBehindFacts(function, closure), function.at(closure).type));
            Vector<ValueID, 8> captures;
            if (!callee.captures.isEmpty())
                captures = closureCreation ? Vector<ValueID, 8>(function.at(closureCreation).variadicOperands()) : insertCaptures(insertion, only, closure);
            ValueID result = insert(makeDirectCall(insertion, {
                .callee = only,
                .captures = captures.span(),
                .thisValue = original.thisValueOperand(),
                .calleeValue = callee.has(FunctionFlags::TakesCallee) ? closure : ValueID(),
                .arguments = original.variadicOperands(),
                .accessSite = original.accessSite(),
                .calledBuiltin = original.calledBuiltin(),
                .isNeverInlined = original.isNeverInlined(),
                .isAlwaysInlined = original.isAlwaysInlined(),
                .isTailCall = !!original.isTailCall,
                .callsOwnFunctionObject = callsOwnFunctionObject,
                .calleeKeptInFrame = keepsCalleeInFrame ? closure : ValueID(),
                .calleeIsPredicted = isCallWithSharedArms(function) || !isFollowedTo(program, original, only),
            }));
            RELEASE_ASSERT(typeOf(result).isSubsetOf(program.at(original.type), program) && !typeOf(result).isNever());
            becomeUpcast(function, site.position.value, result);
            context.analyses.didChangeValues();
        };
    }

    std::expected<ConstructArm, CallDeclineReason> planConstructArm(const CallSite& site, const CalleeCandidates& candidates, FunctionID member)
    {
        const Value& construct = function.at(site.position.value);
        bool isSuperCall = site.opcode != Opcode::Construct;
        ConstructArm arm { directCallArm(member, { }, candidates.value), { } };
        arm.call.keepsCalleeInFrame = isNamedByCalleeOnceRenamed(site, candidates.value, member);

        ValueID newTarget = construct.newTargetOperand();
        bool isOwnNewTarget = !isSuperCall && (!newTarget || areSameObject(newTarget, construct.operand(constructorOperandIndexOf(site.opcode))));
        if (isOwnNewTarget && canHaveConstructBody(program, member)) {
            arm.call.callee = context.derivedFunction(derivedFunctionKey(FunctionRole::ConstructBody, member));
            return arm;
        }

        LayoutID constructed = program.at(member).constructedClass;
        if (!allocatesOwnThis(program.at(member))) {
            if (!(isSuperCall ? construct.layout() : constructed))
                return std::unexpected(CallDeclineReason::CalleeNotCallable);
            if (!isOwnNewTarget && !isSuperCall)
                return declined;
            if (isSuperCall && !isAlwaysClassAtOrBelow(program, typeOf(newTarget), construct.layout())) {
                Type classObjects = classObjectsAllocatedAtOrBelow(program, construct.layout());
                if (!typeOf(newTarget).overlaps(classObjects, program) || classObjects.functions.size() > context.limit(LimitID::MaxDispatchCases) || !isAlwaysClassAtOrBelow(program, classObjects, construct.layout()))
                    return declined;
                arm.testedNewTarget = context.types.intern(classObjects);
                context.planAsksAboutPlace = true;
                if (!isHot(heatClassOf(context, site.position.block)).predicted() || hasFailedEarlier(makeTypeTest(newTarget, arm.testedNewTarget), site))
                    return declined;
            }
            arm.allocatedClass = isSuperCall ? findClassAllocatedBySuperCall(program, construct.layout()) : constructed;
            // Flow may have seen a function arrive here that it follows no construction to. The run time makes the objects of a layout that has none.
            if (!arm.allocatedClass || program.at(arm.allocatedClass).flow.allocation == AllocationState::NeverAllocated)
                return declined;
        } else if (isOwnNewTarget && canTakeAllocatedThis(program, member)) {
            arm.allocatedClass = layoutAllocatedBy(program, member);
            arm.call.callee = context.derivedFunction(derivedFunctionKey(FunctionRole::AllocatedThisVersion, member));
        }
        arm.readsConstructPrototype = arm.allocatedClass && !(isOwnNewTarget && hasOnePrototypeObject(program, arm.allocatedClass));
        return arm;
    }

    void emitConstructArm(const ConstructArm& arm, ValueID closure, std::span<const ValueID> arguments)
    {
        const Value& original = expansion.original();
        ValueID newTarget = original.newTargetOperand() ? original.newTargetOperand() : closure;
        if (!arm.testedNewTarget)
            return emitConstructArm(arm, closure, newTarget, arguments);

        Expected<BlockFrequency> frequency = expansion.frequency();
        ValueSplit split { .value = newTarget, .arms = { SplitArm { arm.testedNewTarget, frequency } }, .hasRest = true, .restFrequency = rarerOf(frequency, BlockFrequency::Unlikely) };
        emitSplit(expansion, split, [&](uint32_t, ValueID classObject) {
            emitConstructArm(arm, closure, classObject, arguments);
        }, [&](ValueID) {
            expansion.finishArm(expansion.appendOriginal({ constructorOperandIndexOf(original.opcode), closure }));
        });
    }

    void emitConstructArm(const ConstructArm& arm, ValueID closure, ValueID newTarget, std::span<const ValueID> arguments)
    {
        ValueID thisValue = arm.allocatedClass ? appendThis(builder, arm.allocatedClass, arm.readsConstructPrototype ? newTarget : ValueID()) : ValueID();
        ValueID returned = appendCallOfArm(arm.call, thisValue, closure, arguments, newTarget);

        if (!arm.allocatedClass || typeOf(returned).isNever())
            return expansion.finishArm(returned);
        if (!typeOf(returned).hasObjects())
            return expansion.finishArm(thisValue);
        Expected<BlockFrequency> frequency = expansion.frequency();
        ValueID isObject = builder.appendTypeTest(returned, Type::object());
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isObject, 0, rarerOf(frequency, BlockFrequency::Unlikely), frequency);
        expansion.finishArm(branch.provenOnTaken);
        builder.moveToEndOf(branch.notTaken);
        expansion.finishArm(thisValue);
    }

    bool takesLiteralListThatNothingElseUses(const Value& construct)
    {
        const UseLists& useLists = context.analyses.useLists();
        ValueID id = construct.listOperand();
        while (useLists.singleUse(id) && isCastThatProvesNothing(function.at(id).opcode))
            id = function.at(id).valueOperand();
        const Value& list = function.at(id);
        return useLists.singleUse(id) && list.opcode == Opcode::NewArray && list.holes().empty() && !list.capacityOperand();
    }

    Plan planConstruct(const CallSite& site)
    {
        const Value& construct = function.at(site.position.value);
        if (site.opcode == Opcode::SuperConstructList && !takesLiteralListThatNothingElseUses(construct))
            return declined;

        CalleeCandidates candidates = calleeCandidatesOf(program, function, construct, construct.operand(constructorOperandIndexOf(site.opcode)));
        // The class of a new object decides every access that follows, so each constructor that flow saw gets an arm.
        auto members = chooseDispatchedCallees(site, candidates, canConstructDirectly, context.limit(LimitID::MaxDispatchCases));
        if (!members)
            return std::unexpected(members.error());

        // A constructor without an arm is constructed by the original operation, with whatever else the value may be.
        Vector<ConstructArm, 2> arms;
        Vector<FunctionID, 4> armConstructors;
        CallDeclineReason decline = CallDeclineReason::None;
        for (FunctionID member : *members) {
            auto arm = planConstructArm(site, candidates, member);
            if (!arm) {
                decline = arm.error();
                continue;
            }
            arms.append(WTF::move(*arm));
            armConstructors.append(member);
        }
        if (arms.isEmpty())
            return std::unexpected(decline);

        return [this, &site, proof = candidates.proof(), arms = WTF::move(arms), split = createCalleeSplit(site, candidates, armConstructors.span())] {
            expansion.begin(site.position);
            const Value& original = expansion.original();
            const Value& list = site.opcode == Opcode::SuperConstructList ? function.at(definitionThroughUpcasts(function, original.listOperand())) : original;
            RELEASE_ASSERT(site.opcode != Opcode::SuperConstructList || list.opcode == Opcode::NewArray);
            Vector<ValueID, 8> arguments(list.variadicOperands());
            emitCalleeDispatch(proof, split, constructorOperandIndexOf(site.opcode), [&](uint32_t armIndex, ValueID narrowed) { emitConstructArm(arms[armIndex], narrowed, arguments.span()); });
            context.build.statistics().count(StatisticID::ConstructsResolved);
        };
    }

    // Both lookups on the class object are answered, so what is left of the operation is the walk up the chain of the value.
    void finishArmWithWalk(ValueID subject)
    {
        const Value& original = expansion.original();
        Value read = IRBuilder::make(Opcode::GetClassPrototype, { original.constructorOperand() });
        read.setLayout(classTestedBy(program, function, original));
        ValueID prototype = expansion.append(WTF::move(read));
        expansion.finishArm(expansion.append(IRBuilder::make(Opcode::HasInPrototypeChain, { subject, prototype })));
    }

    void overwriteValue(ValueID id, Value&& replacement)
    {
        Value& value = function.edit(id);
        replacement.setOrigin(value.origin());
        replacement.type = resultTypeOf(program, function, replacement, context.types);
        value = WTF::move(replacement);
        context.analyses.didChangeValues();
    }

    void passProofOnBranches(ValueID test, ValueID subject)
    {
        builder.setOrigin(function.at(test).origin());

        Vector<ValueID, 2> proofs;
        size_t numberOfBlocks = function.blocks.size();
        for (size_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            BlockID block(blockIndex);
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != test)
                continue;
            for (Edge edge : { Edge { block, 0 }, Edge { block, 1 } }) {
                builder.splitEdge(edge);
                proofs.append(builder.addEdgeResult(edge, testedOperandIndex));
            }
        }
        if (proofs.isEmpty())
            return;

        FunctionAnalyses& analyses = context.analyses;
        for (const Use& use : analyses.useLists().usesOf(subject)) {
            if (useNamesGuard(function, use))
                continue;
            size_t proofIndex = proofs.findIf([&](ValueID proof) { return analyses.dominates(proof, use); });
            if (proofIndex != notFound)
                replaceUse(function, use, proofs[proofIndex]);
        }
        analyses.didChangeValues();
    }

    Plan planInstanceOf(const CallSite& site)
    {
        const Value& instanceOf = function.at(site.position.value);
        LayoutID tested = classTestedBy(program, function, instanceOf);
        if (!tested)
            return declined;
        std::optional<MemberConditions> conditionsOfClass = conditionsOfClassObject(accessContext(), tested);
        if (!conditionsOfClass)
            return std::unexpected(CallDeclineReason::UnknownMember);

        MemberConditions conditions = WTF::move(*conditionsOfClass);
        ValueID subject = instanceOf.valueOperand();
        const Type& subjectType = typeOf(subject);
        ReceiverCandidates candidates = receiverCandidates(program, function, subject);
        bool isLeftToTestsOfExpectedLayouts = !candidates.isProven() && !candidates.expected.predicted().isEmpty() && std::ranges::any_of(planTestedInstanceOf(context, instanceOf), [&](const TestedInstanceOf& form) {
            return !form.walksChain && std::ranges::none_of(form.tests.tests, [&](const Value& test) { return hasFailedEarlier(test, site); });
        });
        if (isLeftToTestsOfExpectedLayouts)
            return std::unexpected(CallDeclineReason::NeedsTest);

        auto [considered, areAllConsidered] = layoutsToConsider(program, candidates, subjectType, tested);
        bool isOverLimit = context.exceeds(LimitID::MaxAccessPlanMembers, considered.size(), site.position.value);
        if (isOverLimit)
            considered.clear();
        ChainAnswers answers = followChains(program, considered, tested, PrototypeKnownBy::BitOfClass);

        bool failedTestMeansFalse = areAllConsidered && !isOverLimit && !answers.leavesOutLayouts && !answers.testsObjects
            && std::ranges::none_of(unionPartsOf(subjectType), [](const UnionPart& part) { return isObjectOutsideLayouts(part.kind); });
        bool isAlwaysFalse = failedTestMeansFalse && answers.instances.isEmpty();
        // A miss walks the chain, so nothing is asked about the layouts that are no instances.
        if (!failedTestMeansFalse)
            answers = followChains(program, answers.instances, tested, PrototypeKnownBy::BitOfClass);
        LayoutSetID instances = answers.instances.isEmpty() ? LayoutSetID() : context.types.completeLayoutSet(answers.instances.span());

        bool hasFailed = instances && !failedTestMeansFalse && (hasFailedEarlier(makeInstanceTest(subject, instances), site)
            || (mayBelongToOtherInstance(context, subject, candidates) && hasFailedEarlier(makeObjectTest(Proof::OwnInstance, subject, { }), site)));
        if (hasFailed)
            instances = { };
        else
            addConditions(conditions, answers.conditions.span());

        // Rare code stays small: there the operation gives way only to what takes its place without a branch.
        bool takesPlaceOfOperation = failedTestMeansFalse && conditions.isEmpty();
        if (!takesPlaceOfOperation && isRare(site).predicted())
            return declined;
        // A bit that was cleared stays cleared. What stands behind its test is left to the tests that ask each object.
        if (!conditions.isEmpty() && hasFailedEarlier(makeConditionsTest(context.types, conditions.span()), site))
            return std::unexpected(CallDeclineReason::NeedsTest);

        // `this instanceof Duplex`, in a program that makes only what is below: nothing has begun the guards of a function that is only a base.
        LayoutID functionWhoseGuardsBegin = conditions.isEmpty() ? LayoutID() : constructorFunctionAskedAbout(program, makeConditionsTest(context.types, conditions.span()));
        return [this, &site, subject, instances, isAlwaysFalse, failedTestMeansFalse, takesPlaceOfOperation, functionWhoseGuardsBegin, testsPrototypeOfInstance = answers.testsObjects, conditions = WTF::move(conditions)] {
            if (takesPlaceOfOperation && isAlwaysFalse) {
                Value answer = IRBuilder::make(Opcode::Boolean);
                answer.setBoolean(false);
                return overwriteValue(site.position.value, WTF::move(answer));
            }
            if (takesPlaceOfOperation) {
                overwriteValue(site.position.value, makeInstanceTest(subject, instances));
                return passProofOnBranches(site.position.value, subject);
            }

            expansion.begin(site.position);
            branchOnConditions(conditions.span(), { }, functionWhoseGuardsBegin);
            if (isAlwaysFalse)
                expansion.finishArm(builder.appendBoolean(false));
            else if (failedTestMeansFalse)
                expansion.finishArm(expansion.append(makeInstanceTest(subject, instances)));
            else if (!instances)
                finishArmWithWalk(subject);
            else {
                Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
                bool testsOwnInstance = mayBelongToOtherInstance(context, subject, receiverCandidates(program, function, subject));

                ValueID test = expansion.append(makeInstanceTest(subject, instances));
                IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequency);
                Vector<BlockID, 2> whereTestsOfObjectFail;
                auto testObject = [&](Proof proof) {
                    ValueID holds = expansion.append(makeObjectTest(proof, branch.provenOnTaken, { }));
                    whereTestsOfObjectFail.append(builder.emitBranch(holds, frequency, rarerOf(frequency, BlockFrequency::Unlikely)).notTaken);
                };
                if (testsOwnInstance)
                    testObject(Proof::OwnInstance);
                if (testsPrototypeOfInstance)
                    testObject(Proof::PrototypeLinkUnchanged);
                expansion.finishArm(builder.appendBoolean(true));
                for (BlockID block : whereTestsOfObjectFail) {
                    builder.moveToEndOf(block);
                    // The walk names the subject as it was before the layout test: a type that lists layouts counts as a proof of the instance.
                    finishArmWithWalk(subject);
                }

                builder.moveToEndOf(branch.notTaken);
                finishArmWithWalk(branch.provenOnNotTaken);
            }
            expansion.end();
        };
    }

    bool isAlwaysConstructor(ValueID value)
    {
        CalleeCandidates candidates = calleeCandidates(program, function, value);
        std::optional<ProvenValue<std::span<const FunctionID>>> all = candidates.all();
        return all && !candidates.mayBeNullish && std::ranges::all_of(all->value(), [&](FunctionID id) {
            return program.at(id).constructorForm() != ConstructorForm::NotConstructor;
        });
    }

    Plan planFunctionMethod(const CallSite& site)
    {
        const Value& value = function.at(site.position.value);
        const BuiltinOverload& overload = program.builtins().at(value.overload());
        bool forwardsAnyReceiver = value.builtinCallForm() == BuiltinCallForm::AnyReceiver && forwardsCallOfAnyReceiver(overload);
        if (!forwardsAnyReceiver && overload.builtin == Builtins::FunctionPrototypeBind && (value.builtinCallForm() == BuiltinCallForm::Whole || value.builtinCallForm() == BuiltinCallForm::AnyReceiver))
            return planBind(site, overload);
        if (!forwardsAnyReceiver && value.builtinCallForm() != BuiltinCallForm::Whole)
            return declined;

        if (!typeOf(site.position.value).isAnything())
            return declined;
        std::optional<ForwardedCallPlan> forwarded = planForwardedCall(program, function, builtinCallOf(program, value));
        if (!forwarded) {
            forwarded = planForwardedConstruct(program, function, builtinCallOf(program, value));
            // Written any other way, the built-in has a frame of its own between the caller and the constructor.
            if (forwarded && !(value.isDoneInPlace() && isAlwaysConstructor(forwarded->callee) && (!forwarded->newTarget || isAlwaysConstructor(forwarded->newTarget))))
                return declined;
        }
        if (!forwarded || (forwarded->list && !isAlwaysArray(typeOf(forwarded->list))))
            return declined;
        return [this, &site, forwarded = *forwarded] {
            becomeForwardedCall(function.edit(site.position.value), forwarded);
            context.analyses.didChangeValues();
        };
    }

    bool canBind(const BuiltinCall& bind, FunctionID targetID, unsigned numberOfBoundArguments, ValueID site)
    {
        const Function& target = program.at(targetID);
        if (!canCreateBoundFunction(program, targetID))
            return false;

        unsigned numberOfCapturedArguments = std::min(numberOfBoundArguments, target.numberOfPositionalParameters());
        TypingContext typing { program, function, context.types };
        auto isWithin = [&](TypeID type, const DeclaredType& bound) { return program.at(type).isSubsetOf(typing.typeOf(bound), program); };
        if (target.has(FunctionFlags::HasThis) && !isWithin(bind.arguments.empty() ? TypeTable::undefined : function.at(bind.arguments[0]).type, target.thisType))
            return false;
        for (unsigned index = 0; index < numberOfCapturedArguments; ++index) {
            if (!isWithin(function.at(bind.arguments[index + 1]).type, target.parameters[index]))
                return false;
        }
        return !context.exceeds(LimitID::MaxCaptures, target.captures.size() + numberOfBoundThisAndTargetCaptures + numberOfCapturedArguments, site);
    }

    std::optional<bool> findPristineTestInFrontOf(const ValueSite& site, ValueID receiver, BuiltinClassID closureClass)
    {
        FunctionAnalyses& analyses = context.analyses;
        std::span<const BlockID> predecessors = analyses.cfg().predecessors(site.block);
        if (predecessors.size() != 1)
            return std::nullopt;
        const Block& tests = function.at(predecessors[0]);
        if (tests.terminator.kind != TerminatorKind::Branch || tests.terminator.taken().block == tests.terminator.notTaken().block)
            return std::nullopt;
        ValueID testID = tests.terminator.conditionOperand();
        const Value& test = function.at(testID);
        if (test.opcode != Opcode::IsPristine || analyses.identityOf(test.operand(0)) != analyses.identityOf(receiver))
            return std::nullopt;
        bool asksClosuresOfClass = test.builtinClass() == closureClass && test.aspects().contains(PristineAspects::Names);
        bool asksClosuresOfBothClasses = test.builtinClass() == BuiltinClasses::Function && test.aspects().contains(PristineAspects::NamesKeptBelow);
        if (!asksClosuresOfClass && !asksClosuresOfBothClasses)
            return std::nullopt;
        // The engine binds what has failed the test, whatever has run since.
        if (tests.terminator.notTaken().block == site.block)
            return false;
        auto isPure = [&](ValueID id) { return analyses.hasNoEffects(id); };
        size_t indexOfTest = tests.values.find(testID);
        if (indexOfTest == notFound || !std::ranges::all_of(tests.values.span().subspan(indexOfTest + 1), isPure) || !std::ranges::all_of(function.at(site.block).values.span().first(site.index), isPure))
            return std::nullopt;
        return true;
    }

    Expected<FunctionID> findExpectedBindTarget(const CallSite& site, const CalleeCandidates& candidates, unsigned numberOfBound)
    {
        AccessSiteID accessSite = function.at(site.position.value).accessSite();
        if (!accessSite || !flow.hasResults())
            return FunctionID();
        std::span<const FlowBoundFunctionAtSite> seen = flow.boundFunctionsSeenAt(accessSite, FlowBoundFunctionUseAtSite::Made).predicted();
        if (seen.size() != 1 || seen[0].numberOfBoundArguments != numberOfBound)
            return FunctionID();
        TypeID closures = context.types.intern(Type::forFunction(seen[0].target));
        if (!typeOf(candidates.value).overlaps(program.at(closures), program))
            return FunctionID();
        context.planAsksAboutPlace = true;
        if (hasFailedEarlier(makeTypeTest(candidates.value, closures), site))
            return FunctionID();
        return seen[0].target;
    }

    Plan planBind(const CallSite& site, const BuiltinOverload& overload)
    {
        BuiltinCall bind = builtinCallOf(program, function.at(site.position.value));
        if (!bind.receiver)
            return declined;
        CalleeCandidates candidates = calleeCandidates(program, function, bind.receiver);
        unsigned numberOfBound = bind.arguments.empty() ? 0 : static_cast<unsigned>(bind.arguments.size() - 1);
        // Where the target is not proven, the bind tests for the one function that flow saw, and the engine binds anything else.
        std::optional<ProvenValue<FunctionID>> provenTarget = candidates.only();
        bool testsTarget = !provenTarget;
        FunctionID targetID = testsTarget ? findExpectedBindTarget(site, candidates, numberOfBound).predicted() : provenTarget->value();
        if (!targetID)
            return std::unexpected(candidates.expected().predicted().empty() ? CallDeclineReason::UnknownCallee : CallDeclineReason::NeedsTest);
        if (!canBind(bind, targetID, numberOfBound, site.position.value))
            return std::unexpected(CallDeclineReason::CalleeNotCallable);

        // A closure of a bound function answers with the name and the length its target was written with.
        // Nobody has said that the closure has its name and its length as it was created with them: the bind asks, and a closure that has not is bound by the engine.
        bool testsThatClosureIsAsCreated = false;
        BuiltinClassID closureClass = classOfFunctionObject(program.at(targetID));
        Vector<PropertyKeyID, 2> namedMembers(overload.receiver->namedMembers.size(), [&](size_t index) { return program.keyOf(overload.receiver->namedMembers[index]); });
        if (!flow.areClosuresAsCreated(targetID, namedMembers.span())) {
            // A bound function takes its name, its length and its prototype at the bind.
            if (program.at(targetID).role == FunctionRole::BoundFunction)
                return std::unexpected(CallDeclineReason::UnknownMember);
            if (!isProvenPristine(bind.receiver, { closureClass, PristineAspects::Names }, site) && !isProvenPristine(bind.receiver, { BuiltinClasses::Function, PristineAspects::NamesKeptBelow }, site)) {
                std::optional<bool> hasPassedTest = findPristineTestInFrontOf(site.position, bind.receiver, closureClass);
                // The test that stood where `bind` was looked up may have met its other side again in front of the call.
                if (hasPassedTest ? !*hasPassedTest : isRare(site).predicted())
                    return std::unexpected(CallDeclineReason::UnknownMember);
                testsThatClosureIsAsCreated = !hasPassedTest;
            }
        }

        FunctionID boundFunction = context.derivedFunction(derivedFunctionKey(FunctionRole::BoundFunction, targetID, numberOfBound));
        CreatorRecord::BoundFunction record = program.at(boundFunction).creator.as<CreatorRecord::BoundFunction>();
        bool capturesThis = program.at(targetID).has(FunctionFlags::HasThis);
        size_t numberOfCaptures = (record.capturesTargetClosure ? 1 : program.at(targetID).captures.size()) + capturesThis + record.numberOfCapturedArguments;
        if (record.numberOfCapturedArguments > numberOfBound || numberOfCaptures != program.at(boundFunction).captures.size())
            return declined;

        return [this, &site, record, boundFunction, capturesThis, testsThatClosureIsAsCreated, closureClass, testsTarget, targetID, receiver = candidates.value, arm = directCallArm(targetID, { }, candidates.value)] {
            auto appendBoundClosure = [&](ValueID closure) {
                if (testsThatClosureIsAsCreated) {
                    IRBuilder::BranchBlocks branch = builder.emitBranch(expansion.append(makePristineTest(closure, closureClass, PristineAspects::Names)), expansion.frequency(), BlockFrequency::Unlikely);
                    builder.moveToEndOf(branch.notTaken);
                    expansion.finishArm(expansion.appendOriginal());
                    builder.moveToEndOf(branch.taken);
                }
                std::span<const ValueID> arguments = expansion.original().argumentsOperands();

                appendCalleeValidation(builder, closure);
                Vector<ValueID, 8> captures;
                if (!record.capturesTargetClosure)
                    captures = capturesOf(arm, closure, { });
                if (capturesThis)
                    captures.append(arguments.empty() ? builder.appendUndefined() : arguments[0]);
                for (uint32_t index = 0; index < record.numberOfCapturedArguments; ++index)
                    captures.append(arguments[index + 1]);
                if (record.capturesTargetClosure)
                    captures.append(closure);

                Value creation = IRBuilder::make(Opcode::NewClosure);
                creation.setFunction(boundFunction);
                creation.setCapturesOperands(captures.span());
                expansion.finishArm(expansion.append(WTF::move(creation)));
            };

            expansion.begin(site.position);
            if (testsTarget) {
                ValueSplit split;
                split.value = receiver;
                split.arms.append(SplitArm { context.types.intern(Type::forFunction(targetID)), expansion.frequency() });
                split.restFrequency = rarerOf(expansion.frequency(), BlockFrequency::Unlikely);
                emitSplit(expansion, split, [&](uint32_t, ValueID narrowed) { appendBoundClosure(narrowed); }, [&](ValueID) { expansion.finishArm(expansion.appendOriginal()); });
            } else
                appendBoundClosure(receiver);
            expansion.end();
        };
    }

    // Every way out of the function is a return, or a tail call of a function that comes back too. Whoever calls it where the language asks for a tail call keeps one frame, however often that happens.
    bool comesBack(FunctionID id, uint64_t depth = 0) const
    {
        const Function& body = program.at(id);
        return body.hasBody() && std::ranges::all_of(body.blocks, [&](const Block& block) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                return true;
            const Value& call = body.at(block.terminator.callOperand());
            return call.opcode == Opcode::CallDirect && depth < context.limit(LimitID::MaxForwardingDepth) && comesBack(call.function(), depth + 1);
        });
    }

    // The block is entered only where the test of the array has just passed, and nothing has run since.
    bool hasPassedPristineTestInFrontOf(const ValueSite& site, ValueID array, const PristineRequirement& requirement)
    {
        context.planAsksAboutPlace = true;
        FunctionAnalyses& analyses = context.analyses;
        std::span<const BlockID> predecessors = analyses.cfg().predecessors(site.block);
        if (predecessors.size() != 1)
            return false;
        const Block& tests = function.at(predecessors[0]);
        if (tests.terminator.kind != TerminatorKind::Branch || tests.terminator.taken().block != site.block || tests.terminator.notTaken().block == site.block)
            return false;
        ValueID testID = tests.terminator.conditionOperand();
        const Value& test = function.at(testID);
        if (test.opcode != Opcode::IsPristine || test.builtinClass() != requirement.builtinClass || !test.aspects().containsAll(requirement.aspects) || analyses.identityOf(test.operand(0)) != analyses.identityOf(array))
            return false;
        auto isPure = [&](ValueID id) { return analyses.hasNoEffects(id); };
        size_t indexOfTest = tests.values.find(testID);
        return indexOfTest != notFound && std::ranges::all_of(tests.values.span().subspan(indexOfTest + 1), isPure) && std::ranges::all_of(function.at(site.block).values.span().first(site.index), isPure);
    }

    // `f(...list)` where f is a declared function that cannot count its arguments, and the list an unchanged array that is made elsewhere. One function for all such places reads what f takes out of the storage of the array
    // and calls f: the place hands it the list, which takes no more code than the stub that calls any value with a list.
    Plan planCallWithElements(const CallSite& site)
    {
        if (isRare(site).predicted() || isOutlinedIteration(function))
            return declined;
        const Value& call = function.at(site.position.value);
        if (call.thisValueOperand() || call.calledBuiltin())
            return declined;
        const Value& read = function.at(CastChain(function, call.calleeOperand()).last());
        if (read.opcode != Opcode::GetModuleVariable)
            return declined;
        const Variable& variable = program.at(read.variable());
        std::optional<ProvenValue<FunctionID>> only = calleeCandidates(program, function, call.calleeOperand()).only();
        if (!only || !variable.function || !variable.flags.contains(VariableFlags::IsImmutable) || variable.function != only->value())
            return declined;
        const Function& target = program.at(variable.function);
        if (target.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter }) || !canCallWithoutClosure(program, variable.function))
            return declined;
        // The shared function returns from its call of f.
        bool hasToComeBack = isTailCall(function, site.position) && function.at(site.position.block).terminator.requirement() == TailCallRequirement::Required;
        if (hasToComeBack && !comesBack(variable.function))
            return declined;
        // A list that is made here may yet go altogether.
        if (resultRuleOf(function.at(sameObjectBehindFacts(function, call.listOperand())).opcode) == ResultRule::Allocation)
            return declined;
        if (!isProvenPristine(call.listOperand(), pristineArray, site) && !hasPassedPristineTestInFrontOf(site.position, call.listOperand(), pristineArray))
            return declined;

        return [this, &site, shared = context.derivedFunction(keyOfCallWithElements(read.variable()))] {
            expansion.begin(site.position);
            ValueID list = expansion.original().listOperand();
            expansion.finishArm(appendDirectCall(builder, { .callee = shared, .arguments = singleElementSpan(list), .accessSite = expansion.original().accessSite(), .isNeverInlined = true, .isTailCall = expansion.isTailCall() }));
            expansion.end();
        };
    }

    // `list.push(...other)`, `list.push.apply(list, other)`: Array.prototype.push appends the elements of the unchanged array to the array and answers with the new length.
    // No store into an array with unchanged storage runs the code of others. One that refuses the elements, or the length, AppendElements hands to Array.prototype.push itself.
    Plan planPushOfList(const CallSite& site)
    {
        if (isRare(site).predicted())
            return declined;
        const Value& call = function.at(site.position.value);
        ValueID receiver = call.thisValueOperand();
        if (!receiver || singleBuiltinCalleeOf(program, function, call) != Builtins::ArrayPrototypePush)
            return declined;
        if (!typeOf(receiver).isSubsetOf(Type::builtinClass(BuiltinClasses::Array), program))
            return declined;
        if (!typeOf(call.listOperand()).isSubsetOf(Type::builtinClass(BuiltinClasses::Array), program))
            return declined;
        PristineRequirement unchangedStorage { BuiltinClasses::Array, PristineAspects::Storage };
        bool isStorageProvenUnchanged = isProvenPristine(receiver, unchangedStorage, site);
        bool isListUnchangedByType = pristineAspectsProvenBy(typeOf(call.listOperand()), BuiltinClasses::Array).containsAll(pristineArray.aspects);
        if ((!isStorageProvenUnchanged || !isListUnchangedByType) && pristineTestOf(program.builtins(), BuiltinClasses::Array) != PristineTest::MayPass)
            return declined;

        return [this, &site, unchangedStorage, isStorageProvenUnchanged, isListUnchangedByType] {
            expansion.begin(site.position);
            ValueID receiver = expansion.original().thisValueOperand();
            ValueID list = expansion.original().listOperand();
            BlockID asBefore = builder.addBlock(BlockFrequency::Rare);
            auto goOnIf = [&](ValueID condition) {
                BlockID next = builder.addBlock(expansion.frequency());
                builder.emitBranch(condition, Target { next, { }, { } }, Target { asBefore, { }, { } });
                builder.moveToEndOf(next);
            };
            // The call refuses a list that does not fit on the stack.
            goOnIf(appendTestThatListFits(list, 0));
            if (!isStorageProvenUnchanged)
                goOnIf(builder.append(makePristineTest(receiver, unchangedStorage.builtinClass, unchangedStorage.aspects)));
            Value append = IRBuilder::make(Opcode::AppendElements, { receiver, list });
            if (!isListUnchangedByType) {
                IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(makePristineTest(list, pristineArray.builtinClass, pristineArray.aspects)), testedOperandIndex, expansion.frequency(), BlockFrequency::Rare, { BlockID(), Target { asBefore, { }, { } } });
                append.setGuardOperand(branch.provenOnTaken);
                builder.moveToEndOf(branch.taken);
            }
            builder.append(WTF::move(append));
            Value length = IRBuilder::make(Opcode::PropGet, { receiver });
            length.setKey(program.keyOf(BuiltinNames::length));
            expansion.finishArm(expansion.append(WTF::move(length)));

            builder.moveToEndOf(asBefore);
            expansion.finishArm(expansion.appendOriginal());
            expansion.end();
        };
    }

    // A function that cannot count its arguments sees no more of a list than its parameters name, and undefined past the end of the list either way.
    Plan planListCall(const CallSite& site)
    {
        // The call that stays for a list that is too long stands in a rare block.
        // The arm that passes arguments ends with another operation than the list call, which SiteExpansion returns from instead of leaving by it.
        if (isRare(site).predicted() || (isTailCall(function, site.position) && function.at(site.position.block).terminator.requirement() == TailCallRequirement::Required))
            return declined;

        const Value& call = function.at(site.position.value);
        CalleeCandidates candidates = calleeCandidates(program, function, site.opcode == Opcode::SuperConstructList ? call.parentOperand() : call.calleeOperand());
        std::optional<ProvenValue<std::span<const FunctionID>>> all = candidates.all();
        bool areCalleesKnown = !!all;
        unsigned numberOfArguments = 0;
        if (areCalleesKnown) {
            for (FunctionID id : all->value()) {
                if (countsItsArguments(program.at(id)))
                    return declined;
                numberOfArguments = std::max(numberOfArguments, program.at(id).numberOfPositionalParameters());
            }
        } else if (!reportsDeclinesIn(site.position.block))
            return declined;

        // Where the callees are not known, only a report reads the answer.
        if (!isProvenPristine(call.listOperand(), pristineArray, site) && !(site.opcode == Opcode::SuperConstructList && hasPassedPristineTestInFrontOf(site.position, call.listOperand(), pristineArray)))
            return declined;
        if (!areCalleesKnown)
            return std::unexpected(CallDeclineReason::UnknownCallee);

        return [this, &site, numberOfArguments] {
            expansion.begin(site.position);
            ValueID list = expansion.original().listOperand();
            IRBuilder::BranchBlocks branch = builder.emitBranch(appendTestThatListFits(list, 0), expansion.frequency(), BlockFrequency::Rare);

            builder.moveToEndOf(branch.notTaken);
            expansion.finishArm(expansion.appendOriginal());

            builder.moveToEndOf(branch.taken);
            Vector<ValueID, 8> arguments;
            for (uint32_t position = 0; position < numberOfArguments; ++position) {
                Value read = IRBuilder::make(Opcode::ElemGet, { list, builder.appendInteger(position) });
                read.setKeyIsIntegral(true);
                arguments.append(expansion.append(WTF::move(read)));
            }
            Value call = expansion.original();
            convertListCall(call, arguments.span());
            call.isTailCall = expansion.isTailCall();
            expansion.finishArm(expansion.append(WTF::move(call)));
            expansion.end();
        };
    }

    // Whoever leaves the first point arrives at the second, and nobody else does. With a list of the operations that go away: what else stands between the points could as well run in front of code of the program.
    bool leadsInLineTo(ProgramPoint from, ProgramPoint to, std::optional<std::span<const ValueID>> retired)
    {
        AOT_THRESHOLD(unsigned, maxBlocksInLine, 8);
        const CFG& cfg = context.analyses.cfg();
        for (unsigned step = 0; step < maxBlocksInLine; ++step) {
            const Block& block = function.at(from.block);
            uint32_t end = from.block == to.block ? to.index : block.values.size();
            if (from.index > end)
                return false;
            for (uint32_t index = from.index; retired && index < end; ++index) {
                ValueID id = block.values[index];
                if (!std::ranges::contains(*retired, id) && context.analyses.effectsOf(id).onAnyEdge().interferesWith(Effects::unknownCode()))
                    return false;
            }
            if (from.block == to.block)
                return true;
            if (block.terminator.kind != TerminatorKind::Jump || !block.terminator.targets[0].arguments.isEmpty())
                return false;
            from = ProgramPoint::atHeadOf(block.terminator.targets[0].block);
            if (from.block != to.block && cfg.incomingEdges(from.block).size() != 1)
                return false;
        }
        return false;
    }

    // Every name of the literal, and what is done with each: the rest array is appended once, by one operation or by one of two that a test of the array chooses between, and then the call takes the list.
    bool findSpreadOfRest(const CallSite& site, SpreadOfRest& spread)
    {
        const UseLists& useLists = context.analyses.useLists();
        const CFG& cfg = context.analyses.cfg();
        Vector<ValueID, 2> appends;
        Vector<ValueID, 4> names { spread.list };
        for (size_t index = 0; index < names.size(); ++index) {
            for (const Use& use : useLists.usesOf(names[index])) {
                if (use.kind != Use::Kind::Operand)
                    return false;
                if (use.user == site.position.value) {
                    if (use.operandIndex != OperandIndex::CallList::list)
                        return false;
                    continue;
                }
                const Value& user = function.at(use.user);
                bool appendsToList = (user.opcode == Opcode::AppendSpread && use.operandIndex == OperandIndex::AppendSpread::array)
                    || (user.opcode == Opcode::AppendElements && use.operandIndex == OperandIndex::AppendElements::array)
                    || isGenericSpread(program, user, use.operandIndex);
                bool isOtherName = isCast(user.opcode) && user.opcode != Opcode::Check;
                if (!appendsToList && !isOtherName && user.opcode != Opcode::ValidateType)
                    return false;
                if (appendsToList)
                    appends.append(use.user);
                if (isOtherName)
                    names.append(use.user);
                spread.operationsOnList.append(use.user);
            }
        }

        auto pointOf = [&](ValueID id) {
            ValueLocation location = useLists.locationOf(id);
            RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
            return ProgramPoint { location.block, location.index };
        };
        auto sourceOf = [&](ValueID append) { return sameObjectBehindFacts(function, function.at(append).operand(1)); };
        static_assert(OperandIndex::AppendSpread::iterable == 1 && OperandIndex::AppendElements::source == 1);
        std::span<const ValueID> retired = spread.operationsOnList.span();

        if (appends.size() == 1) {
            if (function.at(appends[0]).opcode == Opcode::CallDirect)
                return false;
            spread.append = appends[0];
            spread.rest = sourceOf(appends[0]);
            spread.spreadName = function.at(appends[0]).operand(1);
            return leadsInLineTo(pointOf(spread.list), pointOf(appends[0]), std::nullopt) && leadsInLineTo(pointOf(appends[0]), site.position.point(), retired);
        }
        if (appends.size() != 2)
            return false;

        if (function.at(appends[0]).opcode != Opcode::AppendElements)
            std::swap(appends[0], appends[1]);
        if (function.at(appends[0]).opcode != Opcode::AppendElements || function.at(appends[1]).opcode != Opcode::CallDirect)
            return false;
        spread.rest = sourceOf(appends[0]);
        if (sourceOf(appends[1]) != spread.rest)
            return false;
        BlockID whereArrayPasses = pointOf(appends[0]).block;
        BlockID whereArrayFails = pointOf(appends[1]).block;
        std::optional<Edge> passing = cfg.singleIncomingEdge(whereArrayPasses);
        std::optional<Edge> failing = cfg.singleIncomingEdge(whereArrayFails);
        if (!passing || !failing || passing->from != failing->from || passing->isHandlerEdge() || failing->isHandlerEdge() || whereArrayPasses == whereArrayFails)
            return false;
        const Block& tests = function.at(passing->from);
        if (tests.terminator.kind != TerminatorKind::Branch || tests.terminator.targets[0].block != whereArrayPasses || tests.terminator.targets[1].block != whereArrayFails)
            return false;
        const Value& test = function.at(tests.terminator.conditionOperand());
        if (test.opcode != Opcode::IsPristine || sameObjectThroughCasts(function, test.valueOperand()) != spread.rest)
            return false;
        spread.spreadName = test.valueOperand();
        const Terminator& afterPassing = function.at(whereArrayPasses).terminator;
        const Terminator& afterFailing = function.at(whereArrayFails).terminator;
        if (afterPassing.kind != TerminatorKind::Jump || afterFailing.kind != TerminatorKind::Jump || afterPassing.targets[0].block != afterFailing.targets[0].block)
            return false;
        BlockID join = afterPassing.targets[0].block;
        if (cfg.incomingEdges(join).size() != 2 || !afterPassing.targets[0].arguments.isEmpty() || !afterFailing.targets[0].arguments.isEmpty())
            return false;
        auto endOf = [&](BlockID block) { return ProgramPoint { block, static_cast<uint32_t>(function.at(block).values.size()) }; };
        return leadsInLineTo(pointOf(spread.list), endOf(passing->from), std::nullopt)
            && leadsInLineTo(ProgramPoint::atHeadOf(whereArrayPasses), endOf(whereArrayPasses), retired)
            && leadsInLineTo(ProgramPoint::atHeadOf(whereArrayFails), endOf(whereArrayFails), retired)
            && leadsInLineTo(ProgramPoint::atHeadOf(join), site.position.point(), retired);
    }

    // Nothing that can run before the call does more than read the array, and nothing that can run behind it looks at the array again. A test of its header tells nobody anything.
    bool isSeenByNobodyElse(const CallSite& site, const SpreadOfRest& spread)
    {
        const UseLists& useLists = context.analyses.useLists();
        const CFG& cfg = context.analyses.cfg();
        BitVector noBarrier(cfg.numNodes());
        BlockID siteBlock = site.position.block;
        for (BlockID successor : cfg.successors(siteBlock)) {
            if (cfg.blocksReachableFrom(successor, noBarrier).get(siteBlock.index()))
                return false;
        }
        BitVector behindCall = cfg.blocksReachableFrom(siteBlock, noBarrier);

        Vector<ValueID, 4> names { spread.rest };
        for (size_t index = 0; index < names.size(); ++index) {
            for (const Use& use : useLists.usesOf(names[index])) {
                if (use.kind == Use::Kind::BlockSubject)
                    continue;
                if (use.kind == Use::Kind::Operand) {
                    if (spread.operationsOnList.contains(use.user))
                        continue;
                    const Value& user = function.at(use.user);
                    if (isCast(user.opcode) && user.opcode != Opcode::Check) {
                        names.append(use.user);
                        continue;
                    }
                    if (user.operandKind(use.operandIndex) == OperandKind::Guard || user.opcode == Opcode::AllGuards || user.opcode == Opcode::IsPristine || user.opcode == Opcode::HasArrayStorage || user.opcode == Opcode::ValidateType)
                        continue;
                    bool readsLength = user.opcode == Opcode::PropGet && user.key() == program.keyOf(BuiltinNames::length);
                    bool runsBehindCall = use.block == siteBlock ? useLists.locationOf(use.user).index > site.position.index : behindCall.get(use.block.index());
                    if (!runsBehindCall && (readsLength || readsArrayWhereItLies(program, function, user, use.operandIndex)))
                        continue;
                }
                if (behindCall.get(use.block.index()) || cfg.blocksReachableFrom(use.block, noBarrier).get(siteBlock.index()))
                    return false;
            }
        }
        return true;
    }

    // An array that is only read may lie in the frame of whoever made it, and f may be one that keeps its rest array.
    Expected<bool> liesOnEveryLikelyWay(BlockID siteBlock)
    {
        const CFG& cfg = context.analyses.cfg();
        const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
        if (siteBlock == entryBlock)
            return true;
        BitVector barrier(cfg.numNodes());
        for (BlockID block : cfg.reversePostOrder()) {
            if (block == siteBlock || frequencies.classOf(block).predicted() != BlockFrequency::Normal)
                barrier.set(block.index());
        }
        for (size_t index : cfg.blocksReachableFrom(entryBlock, barrier)) {
            if (cfg.successors(BlockID(index)).empty())
                return false;
        }
        return true;
    }

    Plan planRestHandover(const CallSite& site)
    {
        // The call that stays stands in a rare block.
        if (isRare(site).predicted())
            return declined;

        const Value& call = function.at(site.position.value);
        CalleeCandidates candidates = calleeCandidates(program, function, call.calleeOperand());
        std::optional<ProvenValue<FunctionID>> only = candidates.only();
        if (!only)
            return declined;
        FunctionID callee = *only;
        const Function& target = program.at(callee);
        if (!target.has(FunctionFlags::HasRestParameter) || target.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }) || !mayBeCalledDirectly(program, callee) || !canPassRestArguments(program, target))
            return declined;

        SpreadOfRest spread;
        spread.list = sameObjectBehindFacts(function, call.listOperand());
        const Value& list = function.at(spread.list);
        if (list.opcode != Opcode::NewArray || !list.holes().empty() || list.capacityOperand() || list.elementsOperands().size() != target.numberOfPositionalParameters())
            return declined;
        if (!findSpreadOfRest(site, spread))
            return declined;
        // Made for this call of this function, by its generic entry or by whoever calls it directly, and known to nobody else when the function begins.
        const Block& entry = function.at(entryBlock);
        unsigned restIndex = function.firstParameterIndex() + function.numberOfPositionalParameters();
        bool isRestArrayOfThisFrame = function.at(spread.rest).opcode == Opcode::CreateRest || (function.has(FunctionFlags::HasRestParameter) && restIndex < entry.parameters.size() && entry.parameters[restIndex] == spread.rest);
        if (!isRestArrayOfThisFrame || !isSeenByNobodyElse(site, spread) || !liesOnEveryLikelyWay(site.position.block).predicted())
            return declined;

        // What f knows of its rest array it knows of this one.
        // And the other way round: f does with its rest array what it likes, and where f is taken in, its code finds the array under the type that it has here.
        const Type& required = program.at(TypingContext { program, function, context.types }.typeIDOf(target.parameters.last()));
        if (!typeOf(spread.spreadName).isSubsetOf(required, program) || !required.isSubsetOf(typeOf(spread.spreadName), program))
            return declined;
        bool isRestProvenPristine = isProvenPristine(spread.spreadName, pristineArray, site);
        if (!isRestProvenPristine && pristineTestOf(program.builtins(), BuiltinClasses::Array) != PristineTest::MayPass)
            return declined;

        return [this, &site, isRestProvenPristine, closure = candidates.value, arm = directCallArm(callee, { }, candidates.value), spread = WTF::move(spread)] {
            expansion.begin(site.position);
            ValueID thisValue = expansion.original().thisValueOperand();
            Value list = function.at(spread.list);
            Vector<ValueID, 8> arguments(list.elementsOperands());
            Value append = spread.append ? function.at(spread.append) : IRBuilder::make(Opcode::AppendSpread, { spread.list, spread.spreadName });

            expansion.addReplaced(list);
            for (ValueID operation : spread.operationsOnList) {
                expansion.addReplaced(function.at(operation));
                becomeUndefined(function, operation);
            }
            becomeUndefined(function, spread.list);
            context.analyses.didChangeValues();

            BlockID asBefore = builder.addBlock(BlockFrequency::Rare);
            auto goOnIf = [&](ValueID condition) {
                BlockID next = builder.addBlock(expansion.frequency());
                builder.emitBranch(condition, Target { next, { }, { } }, Target { asBefore, { }, { } });
                builder.moveToEndOf(next);
            };
            if (!isRestProvenPristine)
                goOnIf(builder.append(makePristineTest(spread.spreadName, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names })));
            goOnIf(appendTestThatListFits(spread.spreadName, arguments.size()));

            appendCalleeValidation(builder, closure);
            expansion.finishArm(appendCallOfArm(arm, thisValue, closure, arguments.span(), { }, spread.spreadName));

            builder.moveToEndOf(asBefore);
            ValueID listAsBefore = builder.append(WTF::move(list));
            append.setOperand(0, listAsBefore);
            builder.append(WTF::move(append));
            expansion.finishArm(expansion.appendOriginal({ OperandIndex::CallList::list, listAsBefore }));
            expansion.end();
            context.build.statistics().count(StatisticID::RestArraysHandedOver);
        };
    }

    // `f(...list)`: the call reads its arguments out of an array by index, which is what the iterator of an unchanged array yields. Such an array needs no copy for the call.
    Plan planSpreadHandover(const CallSite& site)
    {
        // The expansion leaves the old call in a rare block.
        if (isRare(site).predicted())
            return declined;

        const Value& call = function.at(site.position.value);
        SpreadOfRest spread;
        spread.list = sameObjectBehindFacts(function, call.listOperand());
        const Value& list = function.at(spread.list);
        if (list.opcode != Opcode::NewArray || !list.holes().empty() || list.capacityOperand() || !list.elementsOperands().empty())
            return declined;
        if (!findSpreadOfRest(site, spread) || !spread.append || function.at(spread.append).opcode != Opcode::AppendSpread)
            return declined;

        TypeID arrayType = context.types.intern(Type::builtinClass(BuiltinClasses::Array));
        const Type& arrays = context.types.at(arrayType);
        if (!typeOf(spread.spreadName).overlaps(arrays, program) || !program.expectedOrTypeOf(function, spread.spreadName).predicted().overlaps(arrays, program))
            return declined;
        if (!findMemberOfInstances({ program, context.inputs.flow }, BuiltinClasses::Array, program.keyOf(program.builtins().nameOf(WellKnownSymbol::Iterator))).isConstant())
            return declined;
        bool isProvenArray = typeOf(spread.spreadName).isSubsetOf(arrays, program);
        bool isSourceProvenPristine = isProvenArray && isProvenPristine(spread.spreadName, pristineArray, site);
        if (!isSourceProvenPristine && pristineTestOf(program.builtins(), BuiltinClasses::Array) != PristineTest::MayPass)
            return declined;
        if (context.analyses.findEarlierTest(makeTypeTest(spread.spreadName, arrayType), site.position.block) == EarlierTest::Failed)
            return declined;

        return [this, &site, arrayType, isProvenArray, isSourceProvenPristine, spread = WTF::move(spread)] {
            expansion.begin(site.position);
            Value list = function.at(spread.list);
            Value append = function.at(spread.append);

            expansion.addReplaced(list);
            for (ValueID operation : spread.operationsOnList) {
                expansion.addReplaced(function.at(operation));
                becomeUndefined(function, operation);
            }
            becomeUndefined(function, spread.list);
            context.analyses.didChangeValues();

            BlockID asBefore = isSourceProvenPristine ? BlockID() : builder.addBlock(BlockFrequency::Rare);
            ValueID source = spread.spreadName;
            if (!isProvenArray) {
                IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(source, arrayType), OperandIndex::TypeTest::value, expansion.frequency(), BlockFrequency::Rare);
                builder.moveToEndOf(branch.notTaken);
                builder.emitJump(asBefore, { });
                builder.moveToEndOf(branch.taken);
                source = branch.provenOnTaken;
            }
            if (!isSourceProvenPristine) {
                BlockID next = builder.addBlock(expansion.frequency());
                builder.emitBranch(builder.append(makePristineTest(source, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names })), Target { next, { }, { } }, Target { asBefore, { }, { } });
                builder.moveToEndOf(next);
            }
            expansion.finishArm(expansion.appendOriginal({ OperandIndex::CallList::list, source }));

            if (asBefore) {
                builder.moveToEndOf(asBefore);
                // What the iteration throws names the position of the spread, not that of the call.
                Origin originOfCall = builder.origin();
                builder.setOrigin(list.origin());
                ValueID listAsBefore = builder.append(WTF::move(list));
                append.setOperand(0, listAsBefore);
                builder.setOrigin(append.origin());
                builder.append(WTF::move(append));
                builder.setOrigin(originOfCall);
                expansion.finishArm(expansion.appendOriginal({ OperandIndex::CallList::list, listAsBefore }));
            }
            expansion.end();
        };
    }

    // The type alone says which rows the call can ask for: what flow knows beside it is not in the type of the operand.
    std::optional<ReceiverCandidates> findReceiversOfTableCall(const ValueSite& position) const
    {
        const Value& call = function.at(position.value);
        if (call.opcode != Opcode::Call || !call.calleeKey() || call.expectedResult() || call.argumentsOperands().size() > maxTableCallArguments)
            return std::nullopt;
        const Value& read = function.at(CastChain(function, call.calleeOperand()).last());
        if (read.opcode != Opcode::PropGet || read.key() != call.calleeKey() || read.isFreeName() || classOfClassObject(program, typeOf(read.objectOperand())))
            return std::nullopt;
        ReceiverCandidates candidates = receiverCandidates(program, function, read.objectOperand(), { });
        if (candidates.proof == ReceiverProof::Unknown)
            return findReceiversByNameOfMethod(position, read);
        if (candidates.proof != ReceiverProof::Type || candidates.hasOtherParts || candidates.proven.size() > context.limit(LimitID::MaxAccessPlanMembers))
            return std::nullopt;
        return candidates;
    }

    // `emitter.on(name, listener)` in a loop, and nothing says what `emitter` is. One test tells the objects that find a method of the program under the name from everything else.
    std::optional<ReceiverCandidates> findReceiversByNameOfMethod(const ValueSite& position, const Value& read) const
    {
        PropertyKeyID key = read.key();
        if (!typeOf(read.objectOperand()).anyObject || program.at(key).kind != PropertyKeyKind::Text || program.layoutsWithName(key).empty())
            return std::nullopt;
        if (function.at(position.block).frequency.predicted() != BlockFrequency::Normal || function.runsOnlyBehindFailedTest())
            return std::nullopt;
        // The test and the second way to call are paid for once, in bytes. In a loop that runs often the lookup is paid for at every turn.
        if (!isHot(heatClassByOwnLoops(context, position.block)).predicted() || !isHot(heatClassOf(context, position.block)).predicted())
            return std::nullopt;
        // `list.push(x)`, `text.slice(1)`: the receiver is likelier to be what the language makes.
        if (BuiltinMemberName name = builtinMemberNameOf(program, key)) {
            for (const MemberOfClasses& method : methodsNamed(program, name.name)) {
                if (program.builtins().at(method.member).origin != BuiltinOrigin::Host)
                    return std::nullopt;
            }
        }
        size_t maxLayouts = context.limit(LimitID::MaxAccessPlanMembers);
        ReceiverCandidates candidates = receiverCandidates(program, function, read.objectOperand(), read.receivers());
        if (candidates.proof != ReceiverProof::Unknown)
            return std::nullopt;
        // What was seen to arrive need not be all that arrives: the test costs the same for every object that finds something under the name.
        LayoutList layouts = allocatedLayoutsWith(program, singleElementSpan(key), maxLayouts);
        for (LayoutID layout : candidates.expected.predicted())
            layouts.add(layout);
        if (layouts.isEmpty() || layouts.size() > maxLayouts)
            return std::nullopt;
        candidates.expected = WTF::move(layouts);
        return candidates;
    }

    // The function ends in a call that chooses among more functions than one round of arms tests for, or in a call of a function that does.
    bool leavesByDispatch(FunctionID id, uint64_t depth = 0) const
    {
        const Function& body = program.at(id);
        return std::ranges::any_of(body.blocks, [&](const Block& block) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                return false;
            const Value& call = body.at(block.terminator.callOperand());
            switch (call.opcode) {
            case Opcode::CallTable:
                return true;
            case Opcode::CallDirect:
                return depth < context.limit(LimitID::MaxForwardingDepth) && leavesByDispatch(call.function(), depth + 1);
            case Opcode::Call:
                return call.callees() && program.at(call.callees()).seen().predicted().functions.size() > context.limit(LimitID::MaxDispatchCases);
            default:
                return false;
            }
        });
    }

    Plan planTableCall(const CallSite& site)
    {
        const Value* read = site.opcode == Opcode::Call ? findMethodRead(site, Opcode::PropGet) : nullptr;
        std::optional<ReceiverCandidates> candidates = read ? findReceiversOfTableCall(site.position) : std::nullopt;
        if (!candidates)
            return declined;
        bool isGuess = candidates->proof == ReceiverProof::Unknown;
        bool sinksRead = site.memberRead.canSinkToCall;
        if (!sinksRead && (!isGuess || !findsSameMethodAtCall(site)))
            return declined;
        if (isGuess && isTailCall(function, site.position))
            return declined;
        std::span<const LayoutID> layoutsOfReceiver = isGuess ? candidates->expected.predicted().span() : candidates->proven.span();

        const Value& call = function.at(site.position.value);
        MethodSelector selector { read->key(), static_cast<uint32_t>(call.argumentsOperands().size()), ArgumentForm::Typed };
        Vector<TypeID, 4> parameters = parameterTypesOf(program, context.types, selector);
        for (uint32_t index = 0; index < selector.numberOfArguments; ++index) {
            if (!typeOf(call.argumentsOperands()[index]).isSubsetOf(context.types.at(parameters[index]), program))
                selector.form = ArgumentForm::Boxed;
        }
        // Once for all the methods: it walks every method of the name. A thousand classes with one method name, called at a thousand sites, were the cube of a thousand.
        if (selector.form == ArgumentForm::Boxed)
            parameters = parameterTypesOf(program, context.types, selector);

        // The receivers of a method that flow has seen arrive at the call are told from the others: their call is direct, and can be inlined.
        Vector<LayoutSetID, 2> receiversOfMethodsSeen;
        std::span<const FunctionID> functionsSeen = isGuess && call.callees() ? program.at(call.callees()).seen().predicted().functions : std::span<const FunctionID>();
        using Targets = PropertyAccessCache::TableCallTargets;
        auto findTargets = [&] {
            MemberPartition partition = partitionMembers(accessContext(), layoutsOfReceiver, selector.key, Access::Read);
            Targets targets;
            // The layouts of the family that find something else under the name than a method that the table can enter: a field, an accessor, nothing.
            targets.otherLayouts = LayoutList(partition.rest.span());
            targets.numberOfArms = partition.arms.size();
            for (const MemberArm& arm : partition.arms) {
                const MemberAnswer& answer = arm.answer;
                bool isEnteredThroughTable = answer.kind == MemberKind::Method && answer.receiverProofs.isEmpty() && canRetireMemberRead(site, answer.method)
                    && canHaveTableEntrypoint(program, context.types, answer.method, parameters.span());
                if (answer.kind == MemberKind::Method && answer.receiverProofs.isEmpty() && std::ranges::contains(functionsSeen, answer.method) && canRetireMemberRead(site, answer.method)
                    && receiversOfMethodsSeen.size() + 1 < context.limit(LimitID::MaxDispatchCases) && std::ranges::none_of(arm.layouts, [&](LayoutID layout) { return !!builtinBaseClassOf(program, layout); })) {
                    receiversOfMethodsSeen.append(context.types.completeLayoutSet(arm.layouts.span()));
                    continue;
                }
                // A method that only objects of classes below a built-in class find is entered by no receiver of the table.
                bool hasReceiverOfTable = false;
                for (LayoutID layout : arm.layouts) {
                    bool isOfTable = isEnteredThroughTable && !builtinBaseClassOf(program, layout);
                    (isOfTable ? targets.layoutsOfTable : targets.otherLayouts).add(layout);
                    hasReceiverOfTable |= isOfTable;
                }
                if (!hasReceiverOfTable)
                    continue;
                targets.hasConditions |= !answer.conditions.isEmpty();
                targets.someMethodLeavesByDispatch |= leavesByDispatch(answer.method);
                targets.methods.append(answer.method);
            }
            return targets;
        };
        // A thousand classes with one method name, called at a thousand sites: every site asks the same of every method.
        std::optional<Targets> ownTargets;
        const Targets* found = nullptr;
        PropertyAccessCache* cache = program.propertyAccessCache();
        auto typesOfMemberRead = site.memberRead.links.map<Vector<TypeID, 2>>([&](const ValueSite& link) { return function.at(link.value).type; });
        bool namesOnlyRecordsOfProgram = !TypeTable::isProvisional(selector.key) && !std::ranges::any_of(typesOfMemberRead, [](TypeID type) { return TypeTable::isProvisional(type); });
        if (cache && !isGuess && namesOnlyRecordsOfProgram && candidates->proven.size() >= PropertyAccessCache::fewestLayouts) {
            PropertyAccessCache::TableCallKey key { cache->lists.idOf(candidates->proven.span()), selector.key, selector.numberOfArguments, selector.form == ArgumentForm::Typed, WTF::move(typesOfMemberRead), &flow };
            Hasher hasher;
            add(hasher, key.receivers, key.key, key.numberOfArguments, key.passesTypedArguments, key.flow);
            for (TypeID type : key.typesOfMemberRead)
                add(hasher, type);
            found = &findOrCompute(cache->tableCalls, key, hasher.hash(), [&] {
                return PropertyAccessCache::TableCall { key, findTargets() };
            }).value;
            RELEASE_ASSERT(!cache->checksAnswers || *found == findTargets());
        } else
            found = &ownTargets.emplace(findTargets());
        const Targets& targets = *found;
        // One method: a test of one bit in front of a direct call, which can be inlined, costs less than the load of an entry and an indirect call.
        bool paysToGoThroughTable = targets.methods.size() >= 2 && (targets.hasConditions || targets.numberOfArms > context.limit(LimitID::MaxDispatchCases));
        // A name with one method is left to PlaceTypeTests, which guesses the receiver from the name and whose test serves what else is asked of it.
        if (isGuess ? targets.layoutsOfTable.isEmpty() || targets.methods.size() + receiversOfMethodsSeen.size() < 2 : !paysToGoThroughTable)
            return declined;
        if (isTailCall(function, site.position)) {
            // Once a method is replaced the table enters the lookup, which keeps a frame while it calls what the receiver has under the name.
            if (targets.hasConditions)
                return declined;
            // Arms that are copied into the caller cost less than the load of an entry and a jump. Not where a copy leaves by such a call again: that call gets arms too, and so do theirs.
            if (!targets.someMethodLeavesByDispatch)
                return declined;
        }

        LayoutSetID receivers = context.types.completeLayoutSet(targets.layoutsOfTable.span());
        LayoutSetID otherReceivers = targets.otherLayouts.isEmpty() || isGuess ? LayoutSetID() : context.types.completeLayoutSet(targets.otherLayouts.span());
        // Undefined and null pass no test: they throw at the read of the arm that stays by name, as before.
        bool checksReceiverAtRead = candidates->mayBeNullish && !isGuess;
        ValueID receiver = checksReceiverAtRead ? site.memberRead.read().value : read->objectOperand();

        return [this, &site, selector, receiver, receivers, otherReceivers, checksReceiverAtRead, isGuess, sinksRead, paysToGoThroughTable, methods = targets.methods, receiversOfMethodsSeen = WTF::move(receiversOfMethodsSeen)] {
            expansion.begin(site.position);
            if (sinksRead)
                retireReads(site.memberRead, checksReceiverAtRead, selector.key);
            else
                expansion.setOperationsBeforeOriginal(site.memberRead.links.map<Vector<Value, 2>>([&](const ValueSite& link) { return function.at(link.value); }), OperandIndex::Call::callee);

            auto finishWithTableCall = [&](uint32_t, ValueID narrowed) {
                FunctionID lookup = context.derivedFunction(tableLookupKey(selector));
                // Many calls enter the same entrypoints, so a call names their set.
                CalleeSet entered = CalleeSet::complete(methods.map<Vector<FunctionID>>([&](FunctionID method) { return context.derivedFunction(tableEntrypointKey(method, selector)); }), { });
                canonicalize(entered);
                CalleeSetID entrypoints = context.types.intern(WTF::move(entered));
                const Value& original = expansion.original();
                Vector<ValueID, 8> operands { narrowed };
                operands.append(original.argumentsOperands());

                Value tableCall = IRBuilder::make(Opcode::CallTable);
                tableCall.setFunction(lookup);
                tableCall.setArgumentsOperands(operands.span());
                tableCall.setKey(selector.key);
                tableCall.setReceivers(receivers);
                tableCall.setEntrypoints(entrypoints);
                tableCall.setAccessSite(original.accessSite());
                tableCall.setCallees(original.callees());
                expansion.finishArm(expansion.append(WTF::move(tableCall)));
            };

            if (otherReceivers || isGuess) {
                ValueSplit split { .value = receiver, .arms = { }, .hasRest = true, .restFrequency = isGuess ? Expected<BlockFrequency>(BlockFrequency::Unlikely) : expansion.frequency() };
                for (LayoutSetID receiversOfMethod : receiversOfMethodsSeen)
                    split.arms.append(SplitArm { context.types.intern(Type::forLayoutSet(receiversOfMethod)), expansion.frequency() });
                split.arms.append(SplitArm { context.types.intern(Type::forLayoutSet(receivers)), expansion.frequency() });
                auto finishWithCallByName = [&](ValueID narrowed, LayoutSetID stamp) { expansion.finishArm(expansion.appendOriginal({ OperandIndex::Call::thisValue, narrowed, stamp, { } })); };
                // ResolveCalls, which is next, knows what to make of a receiver that finds few methods.
                emitSplit(expansion, split, [&](uint32_t index, ValueID narrowed) {
                    if (index < receiversOfMethodsSeen.size())
                        finishWithCallByName(narrowed, receiversOfMethodsSeen[index]);
                    else if (paysToGoThroughTable)
                        finishWithTableCall(index, narrowed);
                    else
                        finishWithCallByName(narrowed, receivers);
                }, [&](ValueID narrowed) {
                    // The read has run where it is written.
                    if (!sinksRead)
                        expansion.setOperationsBeforeOriginal({ }, OperandIndex::Call::callee);
                    finishWithCallByName(narrowed, otherReceivers);
                });
            } else
                finishWithTableCall(0, receiver);
            expansion.end();
            if (paysToGoThroughTable)
                context.build.statistics().count(StatisticID::TableCalls);
            if (sinksRead)
                return;

            // An object that passes the test is not asked where the read is written: what it finds under the name is looked for at the call.
            expansion.begin(site.memberRead.read());
            ValueSplit split { .value = receiver, .arms = { }, .hasRest = true, .restFrequency = BlockFrequency::Unlikely };
            Type receiversOfAllArms = Type::forLayoutSet(receivers);
            for (LayoutSetID receiversOfMethod : receiversOfMethodsSeen)
                receiversOfAllArms = receiversOfAllArms.unionWith(Type::forLayoutSet(receiversOfMethod), context.types);
            split.arms.append(SplitArm { context.types.intern(receiversOfAllArms), expansion.frequency() });
            emitSplit(expansion, split, [&](uint32_t, ValueID) { expansion.finishArm(builder.appendUndefined()); }, [&](ValueID narrowed) { expansion.finishArm(expansion.appendOriginal({ *receiverOf(Opcode::PropGet), narrowed })); });
            expansion.end();
        };
    }

    // Between the read and the call nothing changes what an object finds under a name, and nobody else's code runs.
    bool findsSameMethodAtCall(const CallSite& site) const
    {
        const MemberRead& reads = site.memberRead;
        if (!reads.validations.isEmpty() || std::ranges::any_of(reads.links, [&](const ValueSite& link) { return link.block != site.position.block; }))
            return false;
        const BlockValues& values = function.at(site.position.block).values;
        for (uint32_t index = reads.read().index + 1; index < site.position.index; ++index) {
            const Effects& effects = context.analyses.effectsOf(values[index]).normal;
            if (effects.mayRunOtherCode() || !effects.writes.containsOnly(heapsWhoseWritesNobodySees))
                return false;
        }
        return true;
    }
};

struct TestedMemberUse {
    NamedAccessSite site;
    AccessOperands operands;
    SlotStamp stamp;
    AccessTests tests;
};

using TestedMemberUsePlanner = std::optional<TestedMemberUse> (*)(FunctionPassContext&, const ValueSite&, const ReceiverCandidates*);

enum class SuperAccessForm : uint8_t { ByName, MethodClosure, AccessorCall, Undefined };

struct TestedSuperAccess {
    SuperAccessForm form { SuperAccessForm::ByName };
    FunctionID function;
    MemberConditions conditions;
};

struct RefusedMemberUse {
    RefusedMemberUse(TestSiteReporter& reporter, ValueID receiver, NOESCAPE const Invocable<std::optional<SiteContent>()> auto& contentOfUse)
        : cache(reporter.program().propertyAccessCache())
        , types(reporter.context().types)
    {
        if (!cache || reporter.mayHaveFailedTestOf(receiver))
            return;
        content = contentOfUse();
        isCached = content && cache->refusedContents.find(*content);
        unshareableEventCountBefore = types.unshareableEventCount();
        useCountOfRecordsOfPhaseBefore = types.useCountOfRecordsOfPhase();
    }

    bool isKnown() const { return isCached && !cache->checksAnswers; }
    // The memo is one for all threads. A plan that has used a record of this phase has given it its place among the first uses of the task, and whoever finds the refusal
    // in the memo skips that: which thread came first would decide the numbers of new layout sets and types.
    void didRefuse(bool hasFailedTest)
    {
        RELEASE_ASSERT(!isCached || types.useCountOfRecordsOfPhase() == useCountOfRecordsOfPhaseBefore);
        if (content && !isCached && !hasFailedTest && types.unshareableEventCount() == unshareableEventCountBefore)
            cache->refusedContents.add(*content);
    }
    void willReport() const { RELEASE_ASSERT(!isCached); }

    PropertyAccessCache* cache;
    TypeArena& types;
    std::optional<SiteContent> content;
    uint64_t unshareableEventCountBefore { 0 };
    uint64_t useCountOfRecordsOfPhaseBefore { 0 };
    bool isCached { false };
};

} // anonymous namespace

PassOutcome resolveCalls(FunctionPassContext& context)
{
    return CallResolver { context, false }.run();
}

std::span<const FunctionID> calleesOfCallWithSharedArms(const Program& program, const Function& function)
{
    RELEASE_ASSERT(isCallWithSharedArms(function));
    return SharedArms::decode({ .role = function.role, .target = FunctionID(), .parameter = function.derivation.parameter, .node = function.derivation.node }).functions(program);
}

Function buildCallWithSharedArms(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    SharedArms arms = SharedArms::decode(key);

    Function function;
    function.name = inputs.strings.intern(StringView("callWithSharedArms"_s));
    function.kind = FunctionKind::Plain;
    function.role = FunctionRole::OutlinedBody;
    function.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    Type callees;
    for (FunctionID callee : arms.functions(program))
        callees.functions.add(callee);
    // It takes any function and expects these. So a test looks at the word of a closure that is as it was made, and a closure that has got a property or another name since is called as a value behind the arms.
    Vector<TypeID, 8> parameterTypes { TypeTable::function };
    if (arms.passesThis)
        parameterTypes.append(TypeTable::anything);
    for (uint32_t index = 0; index < arms.numberOfArguments; ++index)
        parameterTypes.append(index < SharedArms::numberOfArgumentsPassedAsNumbers && (arms.numbers >> index & 1) ? TypeTable::number : TypeTable::anything);
    function.length = static_cast<uint32_t>(parameterTypes.size());
    for (TypeID type : parameterTypes) {
        function.parameters.append(DeclaredType(Proven::byPosition(type)));
        function.derivation.parameterMap.append(ParameterOrigin { });
    }
    // What a call of the value returns is what one of the functions returns.
    TypingContext typing { program, function, inputs.types };
    Type returned;
    for (FunctionID callee : arms.functions(program))
        returned = returned.unionWith(onlyCalleeOf(program, Type::forFunction(callee)) ? typing.typeOf(program.at(callee).result()) : Type::anything(), inputs.types);
    TypeID resultType = returned.isNever() ? TypeTable::anything : inputs.types.intern(returned);
    function.results.append(DeclaredType(Proven::byPosition(resultType)));
    function.capabilities = initialCapabilitiesOf(function);

    IRBuilder builder({ inputs.build, program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    Vector<ValueID, 8> parameters;
    for (TypeID type : parameterTypes)
        parameters.append(builder.addParameter(entry, type));
    builder.moveToEndOf(entry);
    function.edit(parameters[0], ValueEdit::Type).setExpected(ExpectedType(inputs.types.intern(callees)));

    Value call = IRBuilder::make(Opcode::Call);
    call.setCalleeOperand(parameters[0]);
    if (arms.passesThis)
        call.setThisValueOperand(parameters[1]);
    call.setArgumentsOperands(parameters.span().subspan(1 + arms.passesThis));
    // Each place that calls this function has shown that the value is one of these. So nothing else is entered, behind the arms either, and a place keeps across its call what it kept across the call of the value.
    call.setCallees(inputs.types.intern(CalleeSet::complete(Vector<FunctionID>(arms.functions(program)), { })));
    ValueID result = builder.append(WTF::move(call));
    if (resultType != TypeTable::anything) {
        if (inputs.build.validates())
            builder.append(makeValidateType(result, resultType));
        result = builder.append(makeAssumeType(result, Proven::byPosition(resultType)));
    }
    builder.emitReturn(result);
    return function;
}

DerivedFunctionKey keyOfCallWithElements(VariableID variable)
{
    return { .role = FunctionRole::OutlinedBody, .target = FunctionID(), .parameter = variable.index(), .node = static_cast<uint32_t>(OutlinedIteration::CallWithElements) };
}

Function buildCallWithElements(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    VariableID variable(key.parameter);
    unsigned numberOfArguments = program.at(program.at(variable).function).numberOfPositionalParameters();

    Function function;
    function.name = inputs.strings.intern(StringView("callWithElements"_s));
    function.kind = FunctionKind::Plain;
    function.role = FunctionRole::OutlinedBody;
    function.length = 1;
    function.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    TypeID listType = inputs.types.intern(Type::builtinClass(BuiltinClasses::Array));
    function.parameters.append(DeclaredType(Proven::byPosition(listType)));
    function.derivation.parameterMap.append(ParameterOrigin { });
    function.results.append(DeclaredType(Proven::byPosition(TypeTable::anything)));
    function.capabilities = initialCapabilitiesOf(function);

    IRBuilder builder({ inputs.build, program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID list = builder.addParameter(entry, listType);
    BlockID byValue = builder.addBlock(BlockFrequency::Rare);
    builder.moveToEndOf(entry);

    // Each way reads the variable for itself: where the call is resolved, the closure is not made.
    auto makeListCall = [&] {
        Value read = IRBuilder::make(Opcode::GetModuleVariable);
        read.setVariable(variable);
        Value call = IRBuilder::make(Opcode::CallList);
        call.setCalleeOperand(builder.append(WTF::move(read)));
        call.setListOperand(list);
        return call;
    };
    auto goOnWithProofOf = [&](Value&& test, BlockID otherwise) {
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(WTF::move(test)), testedOperandIndex, BlockFrequency::Normal, BlockFrequency::Rare, { BlockID(), Target { otherwise, { }, { } } });
        builder.moveToEndOf(branch.taken);
        return branch.provenOnTaken;
    };
    auto emitCall = [&](std::span<const ValueID> arguments) {
        Value call = makeListCall();
        convertListCall(call, arguments);
        builder.emitReturn(builder.append(WTF::move(call)));
    };

    // The call refuses a list that does not fit on the stack.
    Value lengthRead = IRBuilder::make(Opcode::PropGet, { list });
    lengthRead.setKey(program.keyOf(BuiltinNames::length));
    ValueID length = builder.append(IRBuilder::make(Opcode::ToLength, { builder.append(WTF::move(lengthRead)) }));
    BlockID fits = builder.addBlock();
    builder.emitBranch(builder.append(IRBuilder::make(Opcode::LessEq, { length, builder.appendInteger(inputs.build.limit(LimitID::MaxReservedStackArguments)) })), Target { fits, { }, { } }, Target { byValue, { }, { } });
    builder.moveToEndOf(fits);

    if (!numberOfArguments)
        emitCall({ });
    else {
        constexpr std::array storages { ArrayStorage::Double, ArrayStorage::Int32, ArrayStorage::Contiguous };
        for (size_t index = 0; index < storages.size(); ++index) {
            ArrayStorage storage = storages[index];
            BlockID next = index + 1 < storages.size() ? builder.addBlock() : byValue;
            ValueID hasStorage = goOnWithProofOf(makeArrayStorageTest(list, storage), next);
            Vector<ValueID, 8> arguments;
            for (uint32_t position = 0; position < numberOfArguments; ++position) {
                ValueID elementKey = builder.appendInteger(position);
                Value test = makePresenceTest(list, elementKey, storage, true);
                test.setGuardOperand(hasStorage);
                ValueID isPresent = goOnWithProofOf(WTF::move(test), byValue);
                Value element = IRBuilder::make(Opcode::GetArrayElement, { list, elementKey });
                element.setGuardOperand(builder.append(IRBuilder::make(Opcode::AllGuards, { hasStorage, isPresent })));
                element.setArrayStorage(storage);
                element.setLevel(KnowledgeLevel::Expected);
                element.setKeyIsIntegral(true);
                element.setIsPresent(true);
                arguments.append(builder.append(WTF::move(element)));
            }
            emitCall(arguments.span());
            if (next != byValue)
                builder.moveToEndOf(next);
        }
    }

    // A short list, a hole, another kind of storage.
    builder.moveToEndOf(byValue);
    builder.emitReturn(builder.append(makeListCall()));
    return function;
}

PassOutcome callThroughMethodTable(FunctionPassContext& context)
{
    // The lookup of a selector is where the table sends a receiver that finds something else than the class definitions say.
    if (context.function.role == FunctionRole::TableLookup)
        return PassOutcome::Unchanged;
    return CallResolver { context, true }.run();
}

// A test in front of a member call also answers for the member read before it, so what it proves may never come back once it is lost. The bits of a class never come back either.
static constexpr ProofSet monotoneObjectProofs { Proof::NotNullish, Proof::OwnInstance, Proof::PrototypeLinkUnchanged, Proof::NoExtraProperties, Proof::NoFieldOutsideSlot };

static std::optional<TestedMemberUse> planTestedAccess(FunctionPassContext& context, const ValueSite& site, const ReceiverCandidates* established)
{
    const Value& access = context.function.at(site.value);
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access, ExpectedClassObjects::Tested);
    if (!query)
        return std::nullopt;
    query->isOnlyCalled = true;
    query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    if (established)
        query->receivers = *established;
    AccessOperands operands = operandsOfNamedAccess(access);
    noteWhetherTypeProvesInstance(context, query->receivers, operands.receiver, site.block);

    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
    if (!plan.isResolved())
        return std::nullopt;

    SlotStamp stamp = createSlotStamp(context, access, plan);
    AccessTests tests = createAccessTests(context, plan, { operands.receiver, operands.stored, stamp });
    return TestedMemberUse { { site, WTF::move(*query), WTF::move(plan) }, operands, stamp, WTF::move(tests) };
}

static std::optional<TestedMemberUse> planTestedMemberCall(FunctionPassContext& context, const ValueSite& site, const ReceiverCandidates* established)
{
    const Value& call = context.function.at(site.value);
    ValueID thisValue = call.thisValueOperand();

    PropertyAccessQuery query;
    query.access = Access::Read;
    query.key = call.calleeKey();
    query.receivers = established ? *established : receiverCandidates(context.program, context.function, thisValue);
    noteWhetherTypeProvesInstance(context, query.receivers, thisValue, site.block);
    query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    query.isOnlyCalled = true;
    query.module = context.function.module;

    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, query);
    if (plan.form != AccessForm::Method)
        return std::nullopt;

    SlotStamp stamp { query.key, context.types.completeLayoutSet(plan.receivers.span()), KnowledgeLevel::Expected, call.accessSite() };
    AccessTests tests = createAccessTests(context, plan, { thisValue, { }, stamp });
    return TestedMemberUse { { site, WTF::move(query), WTF::move(plan) }, { thisValue, { }, { } }, stamp, WTF::move(tests) };
}

// One bit says of every object of a class that it finds the member where the class definitions put it, and one object that hides the member or gets another prototype clears it for all of them.
static std::optional<TestedMemberUse> planWithTestsOfEachObject(FunctionPassContext& context, TestedMemberUse&& use)
{
    const Program& program = context.program;
    const PropertyAccessQuery& query = use.site.query;
    PropertyAccessPlan& plan = use.site.plan;
    if (query.start != MemberStart::Instance || (plan.member.kind != MemberKind::Method && plan.member.kind != MemberKind::Accessor))
        return std::nullopt;

    MemberConditions conditions;
    ProofSet proofs = plan.requiredProofs;
    for (LayoutID layout : plan.receivers) {
        MemberAnswer inherited = findPrototypeMember({ program, &context.inputs.flow }, { MemberStart::ClassPrototype, declaredLayoutOf(program, layout), query.key, query.access, false });
        bool isSameMember = inherited.kind == plan.member.kind && inherited.method == plan.member.method && inherited.getter == plan.member.getter && inherited.setter == plan.member.setter;
        if (!isSameMember || !inherited.receiverProofs.isEmpty() || findField(program, layout, query.key, BaseChain::Proven))
            return std::nullopt;
        addConditions(conditions, inherited.conditions.span());
        if (ownPropertyGainOf(program, layout, query.key) != OwnPropertyGain::Impossible)
            proofs.add(Proof::NoExtraProperties);
        if (!receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype))
            proofs.add(Proof::PrototypeLinkUnchanged);
    }
    if (conditions == plan.conditions)
        return std::nullopt;

    plan.conditions = WTF::move(conditions);
    plan.requiredProofs = proofs;
    use.tests = createAccessTests(context, plan, { use.operands.receiver, use.operands.stored, use.stamp });
    return WTF::move(use);
}

static std::optional<TestedMemberUse> planWithoutFailedTests(TestSiteReporter& reporter, TestedMemberUsePlanner plan, const ValueSite& site, bool& hasFailedTest)
{
    std::optional<TestedMemberUse> use = plan(reporter.context(), site, nullptr);
    if (use && hasFailedInstanceTest(reporter.context(), use->operands.receiver, site.block)) {
        hasFailedTest = true;
        return std::nullopt;
    }
    while (use && !use->tests.tests.isEmpty()) {
        const Value* receiverTest = use->tests.findReceiverLayoutTest();
        if (!receiverTest || reporter.findEarlierTest(*receiverTest) != EarlierTest::Failed)
            break;
        hasFailedTest = true;

        ReceiverCandidates remaining = use->site.query.receivers;
        remaining.stopExpecting(use->site.plan.receivers);
        RELEASE_ASSERT(remaining.expected.predicted().size() < use->site.query.receivers.expected.predicted().size());
        use = plan(reporter.context(), site, &remaining);
    }
    if (!use)
        return std::nullopt;

    auto conditionsTest = std::ranges::find(use->tests.tests, Opcode::AreMembersUnchanged, &Value::opcode);
    if (conditionsTest == use->tests.tests.end() || reporter.findEarlierTest(*conditionsTest) != EarlierTest::Failed)
        return use;
    hasFailedTest = true;
    return planWithTestsOfEachObject(reporter.context(), WTF::move(*use));
}

// The callee tests its own arguments and runs without its types where one of them fails, so the call has the effects of both bodies.
static Effects effectsOfDirectCall(FunctionPassContext& context, FunctionID callee)
{
    return effectsOfCalling({ context.program, context.inputs.effects }, callee).normal;
}

// A frame of a method that records its callee shows the name of the object that was called, so a closure made at the call would hide a new name.
static bool canRecreateMethodClosure(const Program& program, const PropertyAccessPlan& plan)
{
    const Function& method = program.at(plan.member.method);
    return !method.has(FunctionFlags::TakesCallee) && !method.capabilities.contains(FunctionCapabilities::RecordsCallee) && method.captures.size() == (plan.capturesReceiver ? 1u : 0u);
}

static bool mayBeNamedOutOfSight(FunctionPassContext& context, FunctionID function)
{
    return recordsCalleeOnlyOnceRenamed(context.program, function) && context.inputs.flow.closuresMayBeNamedOutOfSight(function);
}

// The caller's frame, which keeps the function object it calls, exists for as long as the callee runs.
static bool keepsFrameOfCaller(TestSiteReporter& reporter)
{
    return !reporter.useValue().isTailCall && !isTailCall(reporter.function(), ValueSite { reporter.point().block, reporter.point().index, reporter.use() });
}

static bool mayBeCalledDirectlyAt(TestSiteReporter& reporter, FunctionID function)
{
    if (keepsFrameOfCaller(reporter)) {
        bool isKept = recordsCalleeOnlyOnceRenamed(reporter.program(), function) && reporter.context().inputs.flow.closuresMayBeNamedOutOfSight(function);
        return !(isKept && canLeaveByTailCall(reporter.program(), function)) && mayBeCalledDirectlyWhereCalleeIsKept(reporter.program(), function);
    }
    // Above a frame that was given up only the entrypoint for calls by value says who runs.
    if (recordsCalleeOnlyOnceRenamed(reporter.program(), function) && reporter.context().inputs.flow.closuresMayBeNamedOutOfSight(function))
        return false;
    return mayBeCalledDirectly(reporter.program(), function);
}

static void reportClosureTest(TestSiteReporter& reporter, ValueID callee, FunctionID function)
{
    bool isCall = reporter.useValue().opcode == Opcode::Call;
    Vector<Value, 2> tests { makeClosureTest(callee, function) };
    reporter.report(tests.span(), isCall ? effectsOfDirectCall(reporter.context(), function) : reporter.analyses().effectsOf(reporter.use()).normal);
}

static bool reportMemberCallTests(TestSiteReporter& reporter, const ValueSite& site)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    ValueID thisValue = call.thisValueOperand();
    if (!call.calleeKey() || !thisValue)
        return false;
    const Value& read = context.function.at(sameObjectThroughCasts(context.function, call.calleeOperand()));
    if (read.opcode != Opcode::PropGet || read.isFreeName() || read.key() != call.calleeKey() || context.analyses.identityOf(read.objectOperand()) != context.analyses.identityOf(thisValue))
        return false;

    // All that planTestedMemberCall() takes from the call and its function.
    RefusedMemberUse refused(reporter, call.thisValueOperand(), [&]() -> std::optional<SiteContent> {
        SiteContent content(static_cast<uint32_t>(TestSitePlanner::MemberCall) << 24);
        content.add(&context.inputs.flow);
        content.add(call.calleeKey());
        content.add(context.function.module.index());
        content.addTypesOf(context.function, call.thisValueOperand());
        return content.holdsOnlyIDsOfProgram() ? std::optional(content) : std::nullopt;
    });
    if (refused.isKnown()) [[likely]]
        return false;

    bool hasFailedTest = false;
    std::optional<TestedMemberUse> use = planWithoutFailedTests(reporter, planTestedMemberCall, site, hasFailedTest);
    if (!use || use->tests.tests.isEmpty()) {
        refused.didRefuse(hasFailedTest);
        return false;
    }
    if (use->site.plan.needsNoTest() || use->tests.tests.isEmpty())
        return false;

    const PropertyAccessPlan& plan = use->site.plan;
    if ((plan.requiredProofs | monotoneObjectProofs) != monotoneObjectProofs || mayGetNameOfItsOwnOutOfSight(context.program, context.inputs.flow, plan)) {
        refused.willReport();
        if (!mayBeCalledDirectlyAt(reporter, plan.member.method))
            return false;
        // What may come back says nothing about the time of the read, so the function that was read is tested itself: a closure kept in a field whose slot may be emptied and filled again.
        reportClosureTest(reporter, call.calleeOperand(), plan.member.method);
        return reporter.didReport();
    }
    if (!canRecreateMethodClosure(context.program, plan) || !mayBeCalledDirectly(context.program, plan.member.method))
        return false;

    refused.willReport();
    if (recordsCalleeOnlyOnceRenamed(context.program, plan.member.method) && context.inputs.flow.closuresMayBeNamedOutOfSight(plan.member.method))
        use->tests.tests.append(IRBuilder::make(Opcode::AreFunctionNamesUnchanged));
    reporter.report(use->tests.tests.span(), effectsOfDirectCall(context, plan.member.method));
    return reporter.didReport();
}

// The function that a call enters is read before its arguments are computed, so which function it is is decided where the read stands, whatever the arguments do.
static bool canBecomeMethodClosure(FunctionPassContext& context, ValueID read)
{
    return isOnlyCalledBehindJoins(context.analyses, read);
}

static void reportMemberAccessTests(TestSiteReporter& reporter, const ValueSite& site)
{
    FunctionPassContext& context = reporter.context();
    const Value& access = reporter.useValue();
    RefusedMemberUse refused(reporter, access.operand(*receiverOf(access.opcode)), [&] {
        return contentOfNamedAccess(context, access, static_cast<uint8_t>(TestSitePlanner::CalledMember));
    });
    if (refused.isKnown()) [[likely]]
        return;

    bool hasFailedTest = false;
    std::optional<TestedMemberUse> use = planWithoutFailedTests(reporter, planTestedAccess, site, hasFailedTest);
    if (!use) {
        refused.didRefuse(hasFailedTest);
        return;
    }
    // The rule of property accesses is next to ask for the plan of an access that gets no site here.
    auto leavePlan = [&] {
        if (hasFailedTest || reporter.didReport())
            return;
        std::optional<bool> isOnlyCalled = use->site.query.planFollowsFromIsOnlyCalled ? std::optional(true) : std::nullopt;
        reporter.keepPlanOfUse({ isOnlyCalled, WTF::move(use->site.plan), use->stamp, WTF::move(use->tests) });
    };
    auto refuse = [&] {
        refused.didRefuse(hasFailedTest);
        leavePlan();
    };

    const PropertyAccessPlan& plan = use->site.plan;
    if (use->tests.tests.isEmpty() || (plan.form != AccessForm::Method && plan.form != AccessForm::Call))
        return refuse();
    if (plan.needsNoTest() || use->tests.tests.isEmpty())
        return leavePlan();

    if (plan.form == AccessForm::Method) {
        if (!canRecreateMethodClosure(context.program, plan) || mayBeNamedOutOfSight(context, plan.member.method))
            return refuse();
        if (canBecomeMethodClosure(context, site.value)) {
            refused.willReport();
            reporter.report(use->tests.tests.span(), effectsAfterRewrite(context, plan, use->stamp));
        }
        return leavePlan();
    }

    FunctionID accessor = plan.member.accessorFor(use->site.query.access);
    if (!canCallAccessorOfMember(context.program, plan.member, accessor))
        return refuse();
    // What is refused for the place it stands at is not refused for all that looks like it.
    if (!canCallWithoutClosure(context.program, accessor) && !paysToReadClosureOfMember(context, reporter.point().block))
        return leavePlan();
    refused.willReport();
    reporter.report(use->tests.tests.span(), effectsOfDirectCall(context, accessor));
    leavePlan();
}

static TestedSuperAccess planTestedSuperAccess(FunctionPassContext& context, const Value& access)
{
    const Program& program = context.program;
    if (!access.key())
        return { };
    Access kind = access.opcode == Opcode::SuperSet ? Access::Store : Access::Read;
    std::optional<MemberAnswer> found = findSuperMember({ program, &context.inputs.flow }, context.function, access, kind);
    if (!found)
        return { };

    MemberAnswer& answer = *found;
    if (answer.level != KnowledgeLevel::Expected || !answer.receiverProofs.isEmpty() || answer.conditions.isEmpty())
        return { };

    if (answer.kind == MemberKind::Method && kind == Access::Read && canCallWithoutClosure(program, answer.method) && !mayBeNamedOutOfSight(context, answer.method))
        return { SuperAccessForm::MethodClosure, answer.method, WTF::move(answer.conditions) };
    if (answer.kind != MemberKind::Accessor)
        return { };
    FunctionID accessor = answer.accessorFor(kind);
    if (!accessor && kind == Access::Read)
        return { SuperAccessForm::Undefined, { }, WTF::move(answer.conditions) };
    if (accessor && canCallWithoutClosure(program, accessor))
        return { SuperAccessForm::AccessorCall, accessor, WTF::move(answer.conditions) };
    return { };
}

static void reportSuperAccessTests(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    TestedSuperAccess access = planTestedSuperAccess(context, reporter.useValue());

    Effects effects = Effects::none();
    if (access.form == SuperAccessForm::ByName || (access.form == SuperAccessForm::MethodClosure && !isOnlyCalled(context.analyses, reporter.use())))
        return;
    if (access.form == SuperAccessForm::MethodClosure)
        effects = declaredBoundOf(Opcode::NewClosure);
    else if (access.form == SuperAccessForm::AccessorCall)
        effects = effectsOfDirectCall(context, access.function);
    reporter.report({ makeConditionsTest(context.types, access.conditions.span()) }, effects);
}

static void reportTestsOfListCall(TestSiteReporter& reporter, ValueID callee, FunctionID function)
{
    ValueID list = reporter.useValue().listOperand();
    PristineRequirement pristineArray { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
    Vector<Value, 2> tests { makeClosureTest(callee, function) };
    if (pristineProofOf({ reporter.program(), reporter.function(), &reporter.analyses() }, list, pristineArray, reporter.point()) != PristineProof::Proven)
        tests.append(makePristineTest(list, pristineArray.builtinClass, pristineArray.aspects));
    reporter.report(tests.span(), reporter.analyses().effectsOf(reporter.use()).normal);
}

// With several expected callees the call of the value stays as it is: chooseDispatchedCallees() says why.
static void reportCalleeTest(TestSiteReporter& reporter)
{
    const Program& program = reporter.program();
    const Value& call = reporter.useValue();
    ValueID callee = OperandPosition::parent(call.opcode) ? call.parentOperand() : call.calleeOperand();

    CalleeCandidates candidates = calleeCandidatesOf(reporter.context(), call, callee, reporter.point().block);
    if (!candidates.hasOneExpectedCallee())
        return;
    FunctionID member = candidates.expected().predicted()[0];
    bool leavesThisToCaller = OperandPosition::parent(call.opcode) && !allocatesOwnThis(program.at(member));
    if (leavesThisToCaller && !isAlwaysClassAtOrBelow(program, reporter.typeOf(call.newTargetOperand()), call.layout()) && !isHot(heatClassOf(reporter.context(), reporter.point().block)).predicted())
        return;
    if (call.opcode == Opcode::SuperConstructList || call.opcode == Opcode::ConstructSuperForwardVarargs) {
        if (!leavesThisToCaller || !canConstructDirectly(program, member) || countsItsArguments(program.at(member)))
            return;
        if (call.opcode == Opcode::SuperConstructList)
            reportTestsOfListCall(reporter, callee, member);
        else if (static_cast<uint64_t>(call.firstArgument()) + program.at(member).numberOfPositionalParameters() <= maxArguments)
            reportClosureTest(reporter, callee, member);
        return;
    }
    if (call.opcode != Opcode::Call ? canConstructDirectly(program, member) : mayBeCalledDirectlyAt(reporter, member))
        reportClosureTest(reporter, callee, member);
}

// f.apply(t, list) is a list call once the list is known to be an array.
static void reportForwardedListTest(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    bool forwardsAnyReceiver = call.builtinCallForm() == BuiltinCallForm::AnyReceiver && forwardsCallOfAnyReceiver(context.program.builtins().at(call.overload()));
    if ((call.builtinCallForm() != BuiltinCallForm::Whole && !forwardsAnyReceiver) || !reporter.typeOf(reporter.use()).isAnything())
        return;
    std::optional<ForwardedCallPlan> plan = planForwardedCall(context.program, context.function, builtinCallOf(context.program, call));
    if (!plan || !plan->list || isAlwaysArray(reporter.typeOf(plan->list)))
        return;

    const Type& expected = context.program.expectedOrTypeOf(context.function, plan->list).predicted();
    if (expected.anyObject || expected.findClassPart(BuiltinClasses::Array))
        reporter.report({ makeTypeTest(plan->list, context.types.intern(Type::builtinClass(BuiltinClasses::Array))) }, Effects::unknownCode());
}

// While nobody has stored to the prototype property of the one class object, it holds the prototype object of the class, and that is what the Structure of an allocation site names.
static void reportTestOfOwnConstructPrototype(TestSiteReporter& reporter, ValueID prototype, NOESCAPE const Invocable<Effects()> auto& effectsWhenPassed)
{
    const Program& program = reporter.program();
    LayoutID allocated = reporter.useValue().layout();
    const Value& read = reporter.function().at(prototype);
    if (read.opcode != Opcode::GetConstructPrototype || read.layout() != allocated)
        return;
    // The read of the class object's own prototype takes a function: new.target of a body that native code may enter is undefined or the function.
    const Type& newTarget = program.typeOf(reporter.function(), read.newTargetOperand());
    if (!newTarget.isObject() || classOfClassObject(program, newTarget) != allocated || guardsAreNeverSet(program, program.at(allocated)))
        return;

    MemberConditions conditions;
    addCondition(conditions, { MemberCondition::Kind::MemberUnchanged, allocated, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    if (!isEvaluatedOnce(program, program.at(allocated)))
        addCondition(conditions, { MemberCondition::Kind::ClassEvaluatedOnce, allocated, ClassSide::Prototype, { } });
    Value test = makeConditionsTest(reporter.types(), conditions.span());
    reporter.report({ test }, effectsWhenPassed());
}

static void reportInstanceOfTests(TestSiteReporter& reporter)
{
    for (const TestedInstanceOf& tested : planTestedInstanceOf(reporter.context(), reporter.useValue())) {
        if (std::ranges::any_of(tested.tests.tests, [&](const Value& test) { return reporter.findEarlierTest(test) == EarlierTest::Failed; }))
            continue;
        // The walk asks each object of the chain for its prototype, and a proxy answers with code of the program: what the row declares is what the walk does by itself.
        reporter.report(tested.tests.tests.span(), tested.walksChain ? reporter.analyses().effectsOf(reporter.use()).normal : Effects::none());
        return;
    }
}

void reportCallTests(TestSiteReporter& reporter)
{
    static constexpr OpcodeSet operations {
        Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::SuperConstructList, Opcode::ConstructSuperForwardVarargs, Opcode::PropGet, Opcode::PropSet, Opcode::SuperGet, Opcode::SuperSet,
        Opcode::CallBuiltin, Opcode::GetConstructPrototype, Opcode::InstanceOf, Opcode::NewObject,
    };
    reporter.forEachCandidate(operations, [&](const Value& value) {
        ValueSite site { reporter.point().block, reporter.point().index, reporter.use() };
        switch (value.opcode) {
        case Opcode::Call:
            if (!reportMemberCallTests(reporter, site))
                reportCalleeTest(reporter);
            break;
        case Opcode::Construct:
        case Opcode::SuperConstruct:
        case Opcode::SuperConstructList:
        case Opcode::ConstructSuperForwardVarargs:
            reportCalleeTest(reporter);
            break;
        case Opcode::PropGet:
        case Opcode::PropSet:
            reportMemberAccessTests(reporter, site);
            break;
        case Opcode::SuperGet:
        case Opcode::SuperSet:
            reportSuperAccessTests(reporter);
            break;
        case Opcode::CallBuiltin:
            reportForwardedListTest(reporter);
            break;
        case Opcode::GetConstructPrototype:
            reportTestOfOwnConstructPrototype(reporter, reporter.use(), [] { return declaredBoundOf(Opcode::GetClassPrototype); });
            break;
        case Opcode::InstanceOf:
            reportInstanceOfTests(reporter);
            break;
        case Opcode::NewObject:
            if (value.prototypeOperand())
                reportTestOfOwnConstructPrototype(reporter, value.prototypeOperand(), [] { return declaredBoundOf(NamedEffectSet::Allocates); });
            break;
        default:
            break;
        }
    });
}

static void rewriteTestedInstanceOf(TestSiteRewriter& rewriter)
{
    Value& use = rewriter.useValue();
    LayoutList testedLayouts;
    std::optional<LayoutList> instances = instancesAmongTestedLayouts(rewriter.program(), rewriter.function(), use, rewriter.tests(), testedLayouts);
    if (!instances) {
        ValueID subject = use.valueOperand();
        Value read = IRBuilder::make(Opcode::GetClassPrototype, { use.constructorOperand() });
        read.setLayout(classTestedBy(rewriter.program(), rewriter.function(), use));
        ValueID prototype = rewriter.insertBeforeUse(WTF::move(read));

        Value& walk = rewriter.useValue();
        walk.become(Opcode::HasInPrototypeChain);
        walk.setValueOperand(subject);
        walk.setPrototypeOperand(prototype);
        return;
    }
    if (instances->isEmpty() || instances->size() == testedLayouts.size()) {
        use.become(Opcode::Boolean);
        use.setBoolean(!instances->isEmpty());
        return;
    }

    Origin origin = use.origin();
    use = makeTypeTest(use.valueOperand(), rewriter.types().intern(Type::forLayouts(instances->span(), rewriter.types())));
    use.setOrigin(origin);
}

void rewriteTestedCall(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeUse = [&](auto&& value) { return rewriter.insertBeforeUse(WTF::move(value)); };
    InsertValue insert = insertBeforeUse;
    ValueInsertion insertion { rewriter.builder().context(), rewriter.function(), rewriter.types(), rewriter.useValue().origin(), insert };

    std::optional<ReceiverCandidates> receivers = receiversEstablishedBy(rewriter.program(), rewriter.tests());
    const ReceiverCandidates* established = receivers ? &*receivers : nullptr;
    ValueSite site { rewriter.point().block, rewriter.point().index, rewriter.use() };
    auto becomeDirectCall = [&](const DirectCallRequest& request) {
        Value call = makeDirectCall(insertion, request);
        Value& use = rewriter.useValue();
        call.setOrigin(use.origin());
        use = WTF::move(call);
    };

    switch (rewriter.useValue().opcode) {
    case Opcode::Call: {
        bool testsCallee = rewriter.tests()[0].opcode == Opcode::IsClosureOf;
        std::optional<TestedMemberUse> use = testsCallee ? std::nullopt : planTestedMemberCall(context, site, established);
        RELEASE_ASSERT(testsCallee || use);

        const Value& call = rewriter.useValue();
        FunctionID function = testsCallee ? rewriter.tests()[0].function() : use->site.plan.member.method;
        ValueID callee = call.calleeOperand();
        // The receiver is the value that passed the tests, so the callee's own test of `this` folds once the callee is inlined.
        ValueID thisValue = testsCallee ? call.thisValueOperand() : use->operands.receiver;
        AccessSiteID accessSite = call.accessSite();
        BuiltinID calledBuiltin = call.calledBuiltin();
        bool isNeverInlined = call.isNeverInlined();
        bool isTailCall = call.isTailCall;
        bool calleeIsPredicted = isCallWithSharedArms(context.function) || !isFollowedTo(context.program, call, function);
        // reportMemberCallTests() checks the names where the function object is not read.
        bool isNamedByCallee = !mayBeCalledDirectly(context.program, function) || (recordsCalleeOnlyOnceRenamed(context.program, function) && context.inputs.flow.closuresMayBeNamedOutOfSight(function));
        bool testsNames = std::ranges::any_of(rewriter.tests(), [](const Value& test) { return test.opcode == Opcode::AreFunctionNamesUnchanged; });
        Vector<ValueID, 8> arguments(call.variadicOperands());

        Vector<ValueID, 8> captures;
        if (testsCallee)
            captures = insertCaptures(insertion, function, callee);
        else if (use->site.plan.capturesReceiver)
            captures.append(thisValue);
        becomeDirectCall({
            .callee = function,
            .captures = captures.span(),
            .thisValue = thisValue,
            .calleeValue = testsCallee && context.program.at(function).has(FunctionFlags::TakesCallee) ? callee : ValueID(),
            .arguments = arguments.span(),
            .accessSite = accessSite,
            .calledBuiltin = calledBuiltin,
            .isNeverInlined = isNeverInlined,
            .isTailCall = isTailCall,
            .calleeKeptInFrame = testsCallee && isNamedByCallee && !testsNames ? callee : ValueID(),
            .calleeIsPredicted = calleeIsPredicted,
        });
        break;
    }
    case Opcode::Construct:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
    case Opcode::CallBuiltin:
        return;
    case Opcode::ConstructSuperForwardVarargs: {
        const Value forwarding = rewriter.useValue();
        Vector<ValueID, 8> arguments;
        for (uint32_t position = 0; position < context.program.at(rewriter.tests()[0].function()).numberOfPositionalParameters(); ++position) {
            Value read = IRBuilder::make(Opcode::GetArgument);
            read.setArgumentIndex(forwarding.firstArgument() + position);
            arguments.append(rewriter.insertBeforeUse(WTF::move(read)));
        }
        Value& construct = rewriter.useValue();
        construct.become(Opcode::SuperConstruct);
        construct.setLayout(forwarding.layout());
        construct.setParentOperand(forwarding.parentOperand());
        construct.setNewTargetOperand(forwarding.newTargetOperand());
        construct.setAccessSite(forwarding.accessSite());
        construct.setCallees(forwarding.callees());
        construct.setArgumentsOperands(arguments.span());
        break;
    }
    case Opcode::PropGet:
    case Opcode::PropSet: {
        std::optional<TestedMemberUse> use = planTestedAccess(context, site, established);
        RELEASE_ASSERT(use);
        if (use->site.plan.form == AccessForm::Method) {
            use->site.plan.level = KnowledgeLevel::Expected;
            AccessRewriter(context, insert).rewrite(use->site, use->operands);
            break;
        }
        FunctionID accessor = use->site.plan.member.accessorFor(use->site.query.access);
        ValueID closure;
        Vector<ValueID, 8> captures;
        if (!canCallWithoutClosure(context.program, accessor)) {
            closure = rewriter.insertBeforeUse(makeReadOfClosureOfMember(rewriter.types(), use->operands.receiver, use->site.query.key, accessor, use->site.query.access == Access::Store));
            captures = insertCaptures(insertion, accessor, closure);
        }
        becomeDirectCall({
            .callee = accessor,
            .captures = captures.span(),
            .thisValue = use->operands.receiver,
            .calleeValue = closure && context.program.at(accessor).has(FunctionFlags::TakesCallee) ? closure : ValueID(),
            .arguments = std::span(&use->operands.stored, use->operands.stored ? 1 : 0),
            .accessSite = use->stamp.accessSite,
            .calleeKeptInFrame = closure && keepsCalleeOfAccessor(context.program, accessor) ? closure : ValueID(),
            .calleeIsPredicted = true,
        });
        break;
    }
    case Opcode::SuperGet:
    case Opcode::SuperSet: {
        Value& use = rewriter.useValue();
        TestedSuperAccess access = planTestedSuperAccess(context, use);
        RELEASE_ASSERT(access.form != SuperAccessForm::ByName);
        if (access.form == SuperAccessForm::MethodClosure) {
            use.become(Opcode::NewClosure);
            use.setFunction(access.function);
        } else if (access.form == SuperAccessForm::Undefined)
            use.become(Opcode::Undefined);
        else {
            ValueID stored = use.opcode == Opcode::SuperSet ? use.valueOperand() : ValueID();
            becomeDirectCall({ .callee = access.function, .thisValue = use.receiverOperand(), .arguments = std::span(&stored, use.opcode == Opcode::SuperSet ? 1 : 0), .accessSite = use.accessSite(), .calleeIsPredicted = true });
        }
        break;
    }
    case Opcode::GetConstructPrototype: {
        Value& read = rewriter.useValue();
        ValueID classObject = read.newTargetOperand();
        LayoutID layout = read.layout();
        read.become(Opcode::GetClassPrototype);
        read.setOperand(OperandIndex::GetClassPrototype::classObject, classObject);
        read.setLayout(layout);
        break;
    }
    case Opcode::NewObject:
        rewriter.useValue().setOperand(*OperandPosition::prototype(Opcode::NewObject), ValueID());
        break;
    case Opcode::InstanceOf:
        rewriteTestedInstanceOf(rewriter);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value& use = rewriter.useValue();
    use.type = resultTypeOf(context.program, context.function, use, context.types, &context.typingMemo);
    context.build.statistics().count(StatisticID::CallsGuarded);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
