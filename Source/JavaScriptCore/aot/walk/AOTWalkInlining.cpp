/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLimits.h"
#include "AOTLinkTables.h"
#include "AOTSweep.h"
#include "AOTThinSummary.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkKnown.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t inlineLimitOutsideLoops = 6;
static constexpr uint32_t inlineLimitInLoops = 40;
static constexpr uint32_t inlineLimitInLoopsWithoutObjects = 24;
static constexpr uint32_t inlineWindowOfWrittenCall = 40;
static constexpr uint32_t inlineWindowOfWrittenCallInLoops = 120;
static constexpr uint32_t inlineLimitOfLeaves = 12;
static constexpr uint32_t inlineLimitInRecursion = 10;
static constexpr uint32_t inlineLimitOfHelpers = 40;
static constexpr uint32_t inlineWindowOfHelpers = 120;

static constexpr uint64_t mostInlineFramesAtInlining = defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture());

static constexpr OpcodeSet opcodesOfInlining { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallDirect };
static constexpr OpcodeSet constructionsOfInlining { Opcode::Construct, Opcode::SuperConstruct };
static constexpr OpcodeSet callsOfListsAtInlining { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };

struct CallOfInlining {
    FunctionID callee;
    Reference closure;
    Reference thisValue;
    Reference newTarget;
    std::span<const Reference> arguments;
};

static Reference closureCalledAtInlining(const Draft& site)
{
    return site.opcode == Opcode::SuperConstruct ? site.parentOperand() : site.calleeOperand();
}

static bool endsInReturnAtInlining(BodyTerminator end)
{
    return end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall;
}

static bool isNoLargerThanCallWithItsOwnCallsAtInlining(const ThinSummary& summary, size_t numberOfOperands)
{
    size_t size = summary.numberOfOperations;
    for (const SummaryCall& call : summary.calls) {
        if (call.callee != SummaryCall::Callee::Builtin && !call.isMadeByProtocol)
            size += inlineLimitOutsideLoops + call.numberOfArguments;
    }
    return size <= inlineLimitOutsideLoops + numberOfOperands;
}

static bool copyMayLetObjectFallAwayAtInlining(const Walker& walker, FunctionID callee, const std::span<const Reference>* handedOver)
{
    if (!handedOver)
        return true;
    auto isMadeHere = [](const SummaryNode& node) {
        return node.kind == SummaryNode::Kind::Allocation || node.kind == SummaryNode::Kind::ClassInstance || node.kind == SummaryNode::Kind::Closure;
    };
    if (std::ranges::any_of(walker.facts().summaries().at(callee).nodes, isMadeHere))
        return true;
    return std::ranges::any_of(*handedOver, [&](Reference operand) {
        if (!operand || !operand.isValue())
            return false;
        ValueID cell = rootOfCellInSweptCode(walker.code(), operand.id());
        return !walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell));
    });
}

static bool isOnlyCalledAtInlining(const BodyIndex& body, ValueID parameter)
{
    uint32_t calls = 0;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::Call && operation.calleeOperand() == parameter)
                ++calls;
        }
    }
    return calls && calls == body.useCount(parameter);
}

static bool onlyHandsFunctionToHelperThatCallsItAtInlining(const Walker& walker, FunctionID forwarder)
{
    const ThinSummary& summary = walker.facts().summaries().at(forwarder);
    if (summary.calls.size() != 1 || summary.has(SummaryTrait::HasLoops))
        return false;
    const SummaryCall& call = summary.calls[0];
    if (!call.isInTailPosition || call.isConstruction || call.passesList || call.isMadeByProtocol || summary.numberOfOperations > inlineLimitOutsideLoops + call.numberOfArguments)
        return false;
    const LinkTables& link = walker.facts().linked();
    FunctionID helper = link.calleeAt(forwarder, call.site);
    if (!helper || helper == forwarder)
        return false;
    const ThinSummary& summaryOfHelper = walker.facts().summaries().at(helper);
    if (summaryOfHelper.has(SummaryTrait::HasLoops) || summaryOfHelper.numberOfOperations > inlineLimitInLoops)
        return false;
    const BodyIndex* body = walker.facts().bodies().find(forwarder);
    const BodyIndex* bodyOfHelper = walker.facts().bodies().find(helper);
    if (!body || !bodyOfHelper || body->opcodeOf(call.site) != Opcode::Call)
        return false;

    const Function& function = walker.program().at(helper);
    BodyBlock entry = bodyOfHelper->block(entryBlock);
    std::span<const ValueID> written = body->value(call.site).as<Opcode::Call>().argumentsOperands();
    for (unsigned index = function.firstParameterIndex(); index < function.numberOfEntryParameters() && index < entry.numberOfParameters() && index - function.firstParameterIndex() < written.size(); ++index) {
        if (function.entryParameterKind(index) != EntryParameter::Parameter)
            continue;
        ValueID argument = body->valueBehindCasts(written[index - function.firstParameterIndex()]);
        while (argument && !body->isParameter(argument) && body->opcodeOf(argument) == Opcode::CheckInitialized)
            argument = body->valueBehindCasts(body->value(argument).operand(0));
        if (!argument || body->isParameter(argument))
            continue;
        bool isOneFunction = body->opcodeOf(argument) == Opcode::NewClosure;
        if (body->opcodeOf(argument) == Opcode::GetModuleVariable) {
            TypeID proven = link.variable(body->value(argument).as<Opcode::GetModuleVariable>().variable()).proven;
            Type type = walker.types().at(proven ? proven : TypeID(TypeTable::anything));
            isOneFunction = type.isObject() && type.isJavaScriptValue() && singleFunctionOf(walker.types(), type);
        }
        if (isOneFunction && isOnlyCalledAtInlining(*bodyOfHelper, entry.parameter(index)))
            return true;
    }
    return false;
}

static bool copyPaysAtInlining(const Walker& walker, FunctionID callee, size_t numberOfOperands, const PlaceOfCall& place, const std::span<const Reference>* handedOver)
{
    if (!place.isInLoop && onlyHandsFunctionToHelperThatCallsItAtInlining(walker, callee))
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    uint32_t window = place.isInLoop ? inlineWindowOfWrittenCallInLoops : inlineWindowOfWrittenCall;
    if (summary.numberOfOperations <= inlineLimitOutsideLoops + numberOfOperands
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= window || isNoLargerThanCallWithItsOwnCallsAtInlining(summary, numberOfOperands)))
        return true;
    if (uint32_t weight = walker.facts().linked().function(callee).weightAsLeaf; weight <= inlineLimitOfLeaves + numberOfOperands
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + weight <= window)
        && (place.isInLoop || walker.facts().linked().function(walker.unit().function).isNamedInItsCycle))
        return true;
    if (walker.program().at(callee).kind == FunctionKind::Initializer)
        return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
    if (place.caller == walker.unit().function && !place.isInLoop && walker.facts().linked().function(place.caller).isNamedInItsCycle)
        return summary.numberOfOperations <= inlineLimitInRecursion && !summary.has(SummaryTrait::HasLoops);
    if (place.caller != walker.unit().function && place.isInLoop && walker.standsInTurnThatLooksCountOn())
        return walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= window && !summary.has(SummaryTrait::HasLoops);
    if (place.caller != walker.unit().function || !place.isInLoop || summary.has(SummaryTrait::HasLoops))
        return false;
    return summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects || (summary.numberOfOperations <= inlineLimitInLoops && copyMayLetObjectFallAwayAtInlining(walker, callee, handedOver));
}

static bool copyFillsObjectMadeForItAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::CallDirect || !site.isConstruction() || site.isNeverInlined())
        return false;
    const Function& function = walker.program().at(callee);
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfWrittenCall)
        return false;
    return function.takesThisFromConstruction() && function.constructedClass && summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

static uint64_t entryParametersCalledInLoopsAtInlining(const ThinSummary& summary)
{
    return summary.entryParametersCalledInLoops;
}

static bool isClosureOfOneFunctionAtInlining(const Walker& walker, Reference value)
{
    TypeID proven = value ? walker.typeOf(value) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    return type.isObject() && type.isJavaScriptValue() && singleFunctionOf(walker.types(), type);
}

static bool isSecondLoopOverWhatCopyCallsAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, std::span<const Reference> handedOver)
{
    if (!walker.depthOfCopies() || place.caller != walker.currentFunction())
        return false;
    uint64_t calledByCallee = entryParametersCalledInLoopsAtInlining(walker.facts().summaries().at(callee));
    uint64_t calledByCopy = entryParametersCalledInLoopsAtInlining(walker.facts().summaries().at(place.caller));
    if (!calledByCallee || !calledByCopy)
        return false;
    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    for (size_t index = 0; index < std::min<size_t>(handedOver.size(), 64); ++index) {
        if (!(calledByCallee >> index & 1) || !handedOver[index])
            continue;
        for (uint32_t parameter = 0; parameter < std::min<uint32_t>(entry.numberOfParameters(), 64); ++parameter) {
            if ((calledByCopy >> parameter & 1) && walker.referenceOf(entry.parameter(parameter)) == handedOver[index])
                return true;
        }
    }
    return false;
}

static bool copyPaysForClosureHandedOverAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* handedOver)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitOfHelpers)
        return false;
    if (!place.isInLoop && walker.facts().linked().function(walker.unit().function).runsAtMostOnce)
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    uint64_t called = entryParametersCalledInLoopsAtInlining(summary);
    if (!called || !handedOver)
        return called;
    for (size_t index = 0; index < std::min<size_t>(handedOver->size(), 64); ++index) {
        if ((called >> index & 1) && isClosureOfOneFunctionAtInlining(walker, (*handedOver)[index]))
            return true;
    }
    return false;
}

static bool copyPaysForClosureThatCalleeOnlyCallsAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* handedOver)
{
    if (!handedOver)
        return false;
    if (!place.isInLoop && (walker.depthOfCopies() || place.caller != walker.unit().function || !onlyHandsFunctionToHelperThatCallsItAtInlining(walker, place.caller)))
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitInLoops)
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return false;
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t position = 0; position < std::min<uint32_t>(static_cast<uint32_t>(handedOver->size()), entry.numberOfParameters()); ++position) {
        if (!isClosureOfOneFunctionAtInlining(walker, (*handedOver)[position]))
            continue;
        if (isOnlyCalledAtInlining(*body, entry.parameter(position)))
            return true;
    }
    return false;
}

static bool copyOfClosureHandedOverPaysAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place)
{
    if (!place.isInLoop || !walker.depthOfCopies() || place.caller != walker.currentFunction())
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!walker.currentBodyIndex().hasLoops() && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

static bool mayBeWalkedAtInlining(const Walker& walker, FunctionID callee)
{
    const Program& program = walker.program();
    const Function& function = program.at(callee);
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (walker.plan().promotedEnvironments().isMaker(callee))
        return false;
    if (function.isCoroutine() || function.results.size() != 1 || walker.facts().isTakenToRunOnce(callee))
        return false;
    FlagsOfSummaryForInlining flags {
        .suspends = summary.has(SummaryTrait::Suspends),
        .hasDirectEval = summary.has(SummaryTrait::HasDirectEval),
        .holdsOperationsBoundToFrame = body && body->holdsAnyOf(opcodesThatBindBodyToFrame),
    };
    if (whyNoCallInlines(program, callee, PartOfCallee::WholeBody, &flags, body) != NotInlinedCause::None)
        return false;

    if (!walker.plan().conventions().exists({ callee, CodeRole::Body }))
        return false;
    if (walker.code().inlineFrames().size() >= mostInlineFramesAtInlining)
        return false;

    BodyBlock entry = body->block(entryBlock);
    if (body->numberOfIncomingEdges(entryBlock) || entry.hasHandler() || entry.numberOfParameters() != function.numberOfEntryParameters())
        return false;

    bool returns = false;
    for (uint32_t block = body->numberOfBlocks(); block && !returns; --block)
        returns = body->isReachable(BlockID(block - 1)) && endsInReturnAtInlining(body->terminator(BlockID(block - 1)));
    const LinkTables& link = walker.facts().linked();
    return returns && (link.function(callee).namesOnlyWhatEveryFunctionLeadsTo || link.leadsTo(walker.unit(), callee));
}

static bool isEnteredAtInlining(const Function& function, bool isConstruction)
{
    if (!canBeCalledByValueOf(function.role) || function.has(FunctionCapabilities::RecordsCallee))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return !isConstruction;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return isConstruction && !function.has(FunctionFlags::HasThis) && function.constructedClass;
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool takesListAtInlining(const Function& callee, unsigned entryParameter)
{
    EntryParameter kind = callee.entryParameterKind(entryParameter);
    return kind == EntryParameter::HiddenRest || (kind == EntryParameter::Parameter && entryParameter - callee.firstParameterIndex() >= callee.numberOfPositionalParameters());
}

static bool tellsAbsentFromUndefinedAtInlining(Walker& walker, const Function& callee, unsigned entryParameter)
{
    if (callee.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return false;
    TypeID written = callee.parameters[entryParameter - callee.firstParameterIndex()].written();
    return written && (walker.types().at(written).primitives & Primitive::Absent);
}

static Reference operandHandedOverAtInlining(const CallOfInlining& call, const Function& callee, unsigned entryParameter)
{
    switch (callee.entryParameterKind(entryParameter)) {
    case EntryParameter::This:
        return call.thisValue;
    case EntryParameter::Callee:
        return call.closure;
    case EntryParameter::NewTarget:
        return call.newTarget;
    case EntryParameter::Parameter: {
        size_t parameter = entryParameter - callee.firstParameterIndex();
        return parameter < callee.numberOfPositionalParameters() && parameter < call.arguments.size() ? call.arguments[parameter] : Reference();
    }
    case EntryParameter::Capture:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool fitsParameterAtInlining(Walker& walker, const Function& callee, unsigned entryParameter, TypeID arriving)
{
    const DeclaredType* declared = callee.declaredEntryParameter(entryParameter);
    if (!declared || !declared->isEstablished())
        return true;
    TypeID established = walker.program().provenTypeOf(*declared);
    return walker.types().isSubset(arriving, established);
}

static bool operandsFitAtInlining(Walker& walker, const CallOfInlining& call, const Function& callee, std::span<const Reference> handedOver)
{
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    if (handedOver.size() != numberOfEntryParameters)
        return false;
    bool takesList = callee.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    if (takesList && call.arguments.size() > callee.numberOfPositionalParameters() + maxComputedElementsOfArrayLiteral)
        return false;

    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        if (walker.positions().entryParameterIsLeftOutOfCalls(call.callee, index))
            continue;
        if (handedOver[index]) {
            TypeID arriving = walker.typeOf(handedOver[index]);
            if (!arriving || !fitsParameterAtInlining(walker, callee, index, arriving))
                return false;
            continue;
        }

        EntryParameter kind = callee.entryParameterKind(index);
        if (!call.closure || kind == EntryParameter::Callee)
            return false;
        if (kind == EntryParameter::Capture || kind == EntryParameter::ArgumentCount)
            continue;
        if (takesListAtInlining(callee, index)) {
            if (!fitsParameterAtInlining(walker, callee, index, walker.types().builtinClass(BuiltinClasses::Array)))
                return false;
            continue;
        }
        if (!fitsParameterAtInlining(walker, callee, index, tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? TypeTable::absent : TypeTable::undefined))
            return false;
    }
    return true;
}

static bool mayPassGuardAtInlining(Walker& walker, const Function& callee, FunctionID id, std::span<const Reference> handedOver)
{
    const CallingConventions& conventions = walker.plan().conventions();
    if (!conventions.hasParameterGuard(id))
        return true;
    for (unsigned index = 0; index < handedOver.size(); ++index) {
        TypeID tested = conventions.typeBehindGuard(id, index);
        if (!tested)
            continue;
        TypeID arriving = handedOver[index] ? walker.typeOf(handedOver[index]) : tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? TypeID(TypeTable::absent) : TypeID(TypeTable::undefined);
        if (!walker.types().overlap(arriving, tested))
            return false;
    }
    return true;
}

static Reference buildCaptureAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, size_t numberOfCaptures, uint32_t index)
{
    const Code& code = walker.code();
    if (call.closure.isValue()) {
        const Operation& creation = code.at(call.closure.id());
        if (creation.opcode == Opcode::NewClosure && creation.function() == call.callee && creation.numberOfVariadicOperands() == numberOfCaptures) {
            ValueID captured = creation.variadicOperands()[index];
            return code.isConstant(captured) ? Reference::constant(captured) : Reference::value(captured);
        }
    }
    if (Reference scope = findScopeOfClosureReadOffObjectInSight(walker, call.callee, call.closure))
        return scope;
    if (Reference atHand = findCaptureAtHand(walker, call.callee, index, call.closure))
        return atHand;
    Draft capture(Opcode::GetCapture, origin);
    walker.chooseConservativeFields(capture);
    capture.setFunction(call.callee);
    capture.setIndex(index);
    capture.setClosureOperand(call.closure);
    return walker.build(WTF::move(capture));
}

static Reference buildListOfOtherArgumentsAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, const Function& callee)
{
    Draft list(Opcode::NewArray, origin);
    walker.chooseConservativeFields(list);
    list.standsForNoValueOfInput = true;
    list.setContents(TypeTable::anything);
    std::span<const Reference> listed = call.arguments.subspan(std::min<size_t>(callee.numberOfPositionalParameters(), call.arguments.size()));
    list.setVariadicOperands(listed);
    Reference built = walker.build(WTF::move(list));
    noteFreshList(walker, built);
    return built;
}

static Vector<Reference, 8> buildEntryArgumentsAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, const Function& callee, std::span<const Reference> handedOver)
{
    Vector<Reference, 8> operands;
    uint32_t numberOfCapturesPassed = 0;
    for (unsigned index = 0; index < handedOver.size(); ++index) {
        EntryParameter kind = callee.entryParameterKind(index);
        uint32_t capture = kind == EntryParameter::Capture ? numberOfCapturesPassed++ : 0;
        if (kind == EntryParameter::Capture && walker.plan().promotedEnvironments().isRemoved(call.callee, capture)) {
            bool isInSight = handedOver[index] && !walker.types().at(walker.typeOf(handedOver[index])).mayBeUnspecified();
            operands.append(isInSight ? handedOver[index] : valueNoLongerCaptured(walker, call.callee, capture, origin));
            continue;
        }
        if (handedOver[index]) {
            operands.append(handedOver[index]);
            continue;
        }
        switch (kind) {
        case EntryParameter::Capture:
            operands.append(buildCaptureAtInlining(walker, origin, call, callee.captures.size(), capture));
            break;
        case EntryParameter::This:
        case EntryParameter::NewTarget:
            operands.append(walker.constant(Draft(Opcode::Undefined, origin)));
            break;
        case EntryParameter::ArgumentCount: {
            Draft count(Opcode::Integer, origin);
            count.setInteger(static_cast<int64_t>(call.arguments.size()));
            operands.append(walker.constant(WTF::move(count)));
            break;
        }
        case EntryParameter::Parameter:
            if (takesListAtInlining(callee, index))
                operands.append(buildListOfOtherArgumentsAtInlining(walker, origin, call, callee));
            else
                operands.append(walker.constant(Draft(tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? Opcode::Absent : Opcode::Undefined, origin)));
            break;
        case EntryParameter::HiddenRest:
            operands.append(buildListOfOtherArgumentsAtInlining(walker, origin, call, callee));
            break;
        case EntryParameter::Callee:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return operands;
}

namespace {

struct OperandsInSightAtInlining {
    std::span<const Reference> handedOver;
    bool callsParameterOfCopy { false };
    bool calleeStandsBehindBuiltin { false };
};

} // anonymous namespace

std::optional<TurnsOfCopyWrittenOut> turnsOfCopyWrittenOutAt(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::Call || !walker.has(Mechanism::EscapeAnalysis) || !walker.has(Mechanism::Folding) || !walker.has(Mechanism::LoopInvariants))
        return std::nullopt;
    if (!walker.facts().summaries().at(callee).has(SummaryTrait::HasLoops))
        return std::nullopt;
    const Function& function = walker.program().at(callee);
    Vector<std::optional<int64_t>, 8> integers;
    Vector<std::optional<uint32_t>, 8> lengths;
    integers.fill(std::nullopt, function.numberOfEntryParameters());
    lengths.fill(std::nullopt, function.numberOfEntryParameters());
    size_t numberOfArguments = site.argumentsOperands().size();
    std::span<const ValueID> written;
    if (place.body && place.call && place.body->opcodeOf(place.call) == Opcode::Call)
        written = place.body->value(place.call).as<Opcode::Call>().argumentsOperands();
    if (written.size() != numberOfArguments)
        written = { };
    TurnsOfCopyWrittenOut answer;
    answer.firstArgumentListed = static_cast<uint32_t>(std::min<size_t>(function.numberOfPositionalParameters(), numberOfArguments));
    unsigned numberOfLists = 0;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::ArgumentCount)
            integers[index] = static_cast<int64_t>(numberOfArguments);
        if (takesListAtInlining(function, index)) {
            ++numberOfLists;
            answer.entryParameterOfList = index;
            lengths[index] = static_cast<uint32_t>(numberOfArguments) - answer.firstArgumentListed;
            continue;
        }
        if (function.entryParameterKind(index) != EntryParameter::Parameter || index - function.firstParameterIndex() >= written.size())
            continue;
        ValueID argument = place.body->valueBehindCasts(written[index - function.firstParameterIndex()]);
        if (argument && !place.body->isParameter(argument) && place.body->opcodeOf(argument) == Opcode::Integer)
            integers[index] = place.body->value(argument).integer();
    }
    bool saysSomething = numberOfLists || std::ranges::any_of(integers, [](const std::optional<int64_t>& integer) { return integer.has_value(); });
    if (numberOfLists > 1 || !saysSomething)
        return std::nullopt;
    Vector<std::optional<int64_t>, 8> noIntegers;
    Vector<std::optional<uint32_t>, 8> noLengths;
    noIntegers.fill(std::nullopt, function.numberOfEntryParameters());
    noLengths.fill(std::nullopt, function.numberOfEntryParameters());
    if (turnsOfLoopsOfCopyWrittenOut(walker, callee, noIntegers.span(), noLengths.span()))
        return std::nullopt;
    std::optional<TextOfCopyRead> text = turnsOfLoopsOfCopyWrittenOut(walker, callee, integers.span(), lengths.span());
    if (!text)
        return std::nullopt;
    answer.text = WTF::move(*text);
    return answer;
}

static bool stepsForOfAtInlining(const Walker& walker, FunctionID callee)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    for (uint32_t loop = 0; body && loop < body->numberOfLoops(); ++loop) {
        if (findForOfSteppedAtHead(*body, LoopID(loop)))
            return true;
    }
    return false;
}

static bool callAdmitsCopyAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place, const OperandsInSightAtInlining* inSight)
{
    if (walker.program().at(callee).isCoroutine() && (site.opcode != Opcode::CallDirect || site.role() != CodeRole::CoroutineBody))
        return false;
    auto copyPays = [&](size_t numberOfOperands) {
        if (inSight && isSecondLoopOverWhatCopyCallsAtInlining(walker, callee, place, inSight->handedOver))
            return false;
        return ((!inSight || !inSight->calleeStandsBehindBuiltin) && copyPaysAtInlining(walker, callee, numberOfOperands, place, inSight ? &inSight->handedOver : nullptr))
            || copyPaysForClosureHandedOverAtInlining(walker, callee, place, inSight ? &inSight->handedOver : nullptr)
            || copyPaysForClosureThatCalleeOnlyCallsAtInlining(walker, callee, place, inSight ? &inSight->handedOver : nullptr)
            || ((!inSight || inSight->callsParameterOfCopy) && copyOfClosureHandedOverPaysAtInlining(walker, callee, place))
            || ((!inSight || !walker.hasTextsInStringBuilders()) && place.caller == walker.unit().function && walker.facts().summaries().at(callee).has(SummaryTrait::HasLoops) && (place.isInLoop || !stepsForOfAtInlining(walker, callee)) && turnsOfCopyWrittenOutAt(walker, site, callee, place));
    };
    if (copyFillsObjectMadeForItAtInlining(walker, site, callee, place))
        return true;
    if (site.opcode == Opcode::CallDirect)
        return !site.isNeverInlined() && copyPays(site.numberOfVariadicOperands());

    bool isConstruction = constructionsOfInlining.contains(site.opcode);
    if (site.opcode == Opcode::Construct && site.newTargetOperand() && site.newTargetOperand() != site.calleeOperand())
        return false;
    if (site.opcode == Opcode::Call && site.isNeverInlined())
        return false;
    return isEnteredAtInlining(walker.program().at(callee), isConstruction) && copyPays(site.argumentsOperands().size());
}

bool mayCopyCallee(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    return walker.has(Mechanism::Inlining) && opcodesOfInlining.contains(site.opcode) && callAdmitsCopyAtInlining(walker, site, callee, place, nullptr)
        && !walker.isBeingWalked(callee) && mayBeWalkedAtInlining(walker, callee);
}

bool copyOfCalleeCalledInLoopPays(const Walker& walker, FunctionID callee, size_t numberOfArguments)
{
    PlaceOfCall place { walker.currentFunction(), true };
    return callee && walker.has(Mechanism::Inlining) && isEnteredAtInlining(walker.program().at(callee), false) && copyPaysAtInlining(walker, callee, numberOfArguments, place, nullptr)
        && !walker.isBeingWalked(callee) && mayBeWalkedAtInlining(walker, callee);
}

bool shouldCopyCallee(const Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> handedOver, bool callsParameterOfCopy)
{
    OperandsInSightAtInlining inSight { handedOver, callsParameterOfCopy, false };
    PlaceOfCall place { walker.currentFunction(), walker.isInLoop() };
    return walker.has(Mechanism::Inlining) && opcodesOfInlining.contains(site.opcode) && callAdmitsCopyAtInlining(walker, site, callee, place, &inSight)
        && !walker.isBeingWalked(callee) && mayBeWalkedAtInlining(walker, callee);
}

Vector<Reference, 8> operandsHandedToCopy(const Draft& site, const Function& callee)
{
    if (site.opcode == Opcode::CallDirect)
        return Vector<Reference, 8>(site.variadicOperands());

    CallOfInlining request;
    request.closure = closureCalledAtInlining(site);
    request.arguments = site.argumentsOperands();
    if (site.opcode == Opcode::Construct)
        request.newTarget = request.closure;
    else if (site.opcode == Opcode::SuperConstruct)
        request.newTarget = site.newTargetOperand();
    else
        request.thisValue = site.thisValueOperand();

    Vector<Reference, 8> handedOver;
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    for (unsigned index = 0; index < numberOfEntryParameters; ++index)
        handedOver.append(operandHandedOverAtInlining(request, callee, index));
    return handedOver;
}

static_assert(opcodesThatInlineCalleeTakes == (opcodesOfInlining | callsOfListsAtInlining));

static bool inlineCalleeOfListWrittenOut(Walker& walker, const BodyValue& call)
{
    Reference closure = walker.referenceOf(call.opcode() == Opcode::SuperConstructList ? call.parentOperand() : call.calleeOperand());
    FunctionID callee = closure ? findOnlyCallee(walker, call, closure) : FunctionID();
    if (!callee)
        return false;
    std::optional<Draft> site = draftOfCallWithListWrittenOut(walker, call);
    if (!site)
        return false;
    Vector<Reference, 8> handedOver = operandsHandedToCopy(*site, walker.program().at(callee));
    OperandsInSightAtInlining inSight { handedOver.span(), false, false };
    if (!callAdmitsCopyAtInlining(walker, *site, callee, { walker.currentFunction(), walker.isInLoop() }, &inSight))
        return false;
    return walker.walkInline(call, callee, handedOver.span(), &*site);
}

namespace {

struct HeadThatReturnsAtOnce {
    uint32_t entryParameter { 0 };
    bool returnsWhereTrue { false };
    ValueID returned;
};

} // anonymous namespace

static std::optional<HeadThatReturnsAtOnce> findHeadThatReturnsAtOnceAtInlining(const BodyIndex& body)
{
    BodyBlock entry = body.block(entryBlock);
    BodyTerminator end = entry.terminator();
    if (!end.isConditionalBranch() || entry.hasHandler() || body.isLoopHead(entryBlock))
        return std::nullopt;
    ValueID asked = end.operand(0);
    bool isNegated = false;
    uint32_t numberOfTests = 0;
    while (!body.isParameter(asked)) {
        Opcode opcode = body.opcodeOf(asked);
        if (opcode != Opcode::ToBoolean && opcode != Opcode::LogicalNot)
            return std::nullopt;
        isNegated ^= opcode == Opcode::LogicalNot;
        ++numberOfTests;
        asked = body.value(asked).operand(0);
    }
    std::optional<uint32_t> entryParameter = entry.indexOfParameter(asked);
    uint32_t numberOfConstants = 0;
    for (uint32_t place = 0; place < entry.numberOfValues(); ++place)
        numberOfConstants += primitiveConstants.contains(body.opcodeOf(ValueID(entry.firstValue().index() + place)));
    if (!entryParameter || numberOfTests + numberOfConstants != entry.numberOfValues())
        return std::nullopt;
    bool isTaken = true;
    for (BodyTarget target : end.targets()) {
        bool holdsWhereTrue = std::exchange(isTaken, false) != isNegated;
        BodyBlock block = body.block(target.block());
        BodyTerminator endOfBlock = block.terminator();
        if (!target.arguments().empty() || block.hasHandler() || endOfBlock.kind() != TerminatorKind::Return)
            continue;
        bool holdsConstantsAlone = true;
        for (uint32_t position = 0; position < block.numberOfParameters(); ++position)
            holdsConstantsAlone &= body.isUnused(block.parameter(position));
        for (uint32_t place = 0; place < block.numberOfValues(); ++place) {
            Opcode opcode = body.opcodeOf(ValueID(block.firstValue().index() + place));
            holdsConstantsAlone &= primitiveConstants.contains(opcode) || opcode == Opcode::Convert;
        }
        ValueID returned = endOfBlock.operand(0) ? body.valueBehindCasts(endOfBlock.operand(0)) : ValueID();
        if (holdsConstantsAlone && returned && !body.isParameter(returned) && primitiveConstants.contains(body.opcodeOf(returned)))
            return HeadThatReturnsAtOnce { *entryParameter, holdsWhereTrue, returned };
    }
    return std::nullopt;
}

static bool copyHeadThatReturnsAtOnceAtInlining(Walker& walker, const BodyValue& call, const Draft& site, FunctionID callee, std::span<const Reference> handedOver)
{
    if ((call.opcode() != Opcode::Call && call.opcode() != Opcode::CallDirect) || !walker.opensGuesses() || site.resultIsReturnedAtOnce || site.isTailCall)
        return false;
    const Function& function = walker.program().at(callee);
    if (function.isCoroutine() || (call.opcode() == Opcode::CallDirect && (site.isConstruction() || site.role() != CodeRole::Body)))
        return false;
    const BodyIndex& body = walker.facts().bodies().at(callee);
    std::optional<HeadThatReturnsAtOnce> head = findHeadThatReturnsAtOnceAtInlining(body);
    if (!head || head->entryParameter >= handedOver.size() || function.entryParameterKind(head->entryParameter) != EntryParameter::Parameter
        || takesListAtInlining(function, head->entryParameter) || tellsAbsentFromUndefinedAtInlining(walker, function, head->entryParameter))
        return false;

    TypeArena& types = walker.types();
    Reference argument = handedOver[head->entryParameter];
    TypeID type = argument ? walker.typeOf(argument) : TypeID(TypeTable::undefined);
    if (!type || isNeverType(type) || !types.at(type).isJavaScriptValue())
        return false;
    Origin origin = site.origin();
    origin.isTailCall = false;
    auto returned = [&] {
        Draft constant = walker.draftOf(body.value(head->returned));
        constant.setOrigin(origin);
        return walker.constant(WTF::move(constant));
    };
    Reference constant = returned();
    if (!constant)
        return false;
    TypeID shown = argument ? walker.typeShown(argument) : TypeID(TypeTable::undefined);
    bool isNeverTrue = isNeverType(truthyPartOf(types, shown));
    bool isAlwaysTrue = isNeverType(falsyPartOf(types, shown));
    if (isNeverTrue || isAlwaysTrue) {
        if (isAlwaysTrue != head->returnsWhereTrue)
            return false;
        walker.bind(call.id(), constant);
        return true;
    }

    if (!isBooleanType(type) || walker.representationOf(argument) != Representation::Boolean)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();
    BlockID whereHeadReturns = walker.addBlock();
    BlockID whereCalleeIsEntered = walker.addBlock();
    BlockID behindCall = walker.addBlock();
    walker.branch(argument, head->returnsWhereTrue ? whereHeadReturns : whereCalleeIsEntered, head->returnsWhereTrue ? whereCalleeIsEntered : whereHeadReturns);
    Reference answer;
    walker.continueIn(whereCalleeIsEntered);
    {
        Walker::ClosedArm calls(walker);
        Draft known(Opcode::Boolean, origin);
        known.setBoolean(!head->returnsWhereTrue);
        Reference handedOn = walker.constant(WTF::move(known));
        call.forEachOperand([&](ValueID operand, uint32_t) {
            if (handedOn && walker.referenceOf(operand) == argument)
                walker.bind(operand, handedOn);
        });
        walker.walkStretch(block.id(), place, place + 1);
        if (walker.hasFailed())
            return true;
        Reference called = walker.isUnreachable() ? Reference() : walker.referenceOf(call.id());
        bool keepsForm = called && types.isSubset(walker.typeOf(constant), walker.typeOf(called));
        answer = walker.addParameter(behindCall, keepsForm ? walker.typeOf(called) : TypeID(TypeTable::anything), keepsForm ? walker.representationOf(called) : Representation::JSValue);
        if (called)
            walker.jumpTo(behindCall, singleElementSpan(called));
    }
    walker.continueIn(whereHeadReturns);
    walker.jumpTo(behindCall, singleElementSpan(constant));
    walker.continueIn(behindCall);
    walker.bind(call.id(), answer);
    return true;
}

static bool copyLearnsOfThisAtInlining(Walker& walker, const BodyValue& call, FunctionID callee)
{
    ValueID written = call.as<Opcode::Call>().thisValueOperand();
    TypeID handed = written ? walker.typeOf(walker.referenceOf(written)) : TypeID();
    TypeID own = walker.program().at(callee).thisType.written();
    return !handed || !own || !walker.types().isSubset(walker.types().withoutNullish(own), handed);
}

bool inlineCalleeOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (callsOfListsAtInlining.contains(call.opcode()))
        return inlineCalleeOfListWrittenOut(walker, call);
    if (!opcodesOfInlining.contains(call.opcode()))
        return false;
    if (call.opcode() == Opcode::Call && walker.nothingIsProvenOf(walker.referenceOf(call.calleeOperand())))
        return false;

    Reference closure = call.opcode() == Opcode::CallDirect ? Reference() : walker.referenceOf(call.opcode() == Opcode::SuperConstruct ? call.parentOperand() : call.calleeOperand());
    FunctionID callee = call.opcode() == Opcode::CallDirect ? call.function() : findOnlyCallee(walker, call, closure);
    bool calleeStandsBehindBuiltin = !callee && call.opcode() == Opcode::Call && !call.isDoneInPlace() && closure;
    if (calleeStandsBehindBuiltin)
        callee = findLibraryFunctionBehindBuiltin(walker, walker.typeOf(closure));
    if (!callee)
        return false;

    if (call.opcode() == Opcode::Call && walker.standsAtFrameOfRowOfShapeTests() && !copyLearnsOfThisAtInlining(walker, call, callee))
        return false;
    Draft site = walker.draftOf(call);
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<Reference, 8> handedOver = operandsHandedToCopy(site, walker.program().at(callee));
    bool callsParameterOfCopy = call.opcode() == Opcode::Call && walker.depthOfCopies() && index.block(entryBlock).indexOfParameter(index.valueBehindCasts(call.as<Opcode::Call>().calleeOperand()));
    OperandsInSightAtInlining inSight { handedOver.span(), callsParameterOfCopy, calleeStandsBehindBuiltin };
    if (callAdmitsCopyAtInlining(walker, site, callee, { walker.currentFunction(), walker.isInLoop(), &index, call.id() }, &inSight) && walker.walkInline(call, callee, handedOver.span(), &site))
        return true;
    return !calleeStandsBehindBuiltin && !walker.hasFailed() && copyHeadThatReturnsAtOnceAtInlining(walker, call, site, callee, handedOver.span());
}

struct ClosureMadeForCallAtInlining {
    Reference made;
    ValueID written;
};

static Vector<ClosureMadeForCallAtInlining, 2> collectClosuresMadeForCallAtInlining(Walker& walker, const BodyValue& call)
{
    Vector<ClosureMadeForCallAtInlining, 2> closures;
    const BodyIndex& index = walker.currentBodyIndex();
    call.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID written = operand;
        while (written && !index.isParameter(written) && index.hasOneUse(written) && inputOperationsThatOnlyHandOn.contains(index.opcodeOf(written)))
            written = index.value(written).operand(0);
        if (!written || index.isParameter(written) || !index.hasOneUse(written) || index.opcodeOf(written) != Opcode::NewClosure || index.blockOf(written) != index.blockOf(call.id()))
            return;
        Reference made = walker.referenceOf(operand);
        if (made && made.isValue() && walker.code().at(made.id()).opcode == Opcode::NewClosure)
            closures.append({ made, written });
    });
    return closures;
}

bool Walker::copyHandsBack(ValueID input)
{
    Frame& frame = m_frames.last();
    const BodyIndex& body = *frame.index;
    if (!frame.hasLookedForValueHandedBack) {
        frame.hasLookedForValueHandedBack = true;
        ValueID only;
        bool isOne = true;
        for (uint32_t number = 0; number < body.numberOfBlocks() && isOne; ++number) {
            if (!body.isReachable(BlockID(number)) || !endsInReturnAtInlining(body.terminator(BlockID(number))))
                continue;
            ValueID returned = body.terminator(BlockID(number)).operand(0);
            if (returned)
                returned = body.valueBehindCasts(returned);
            isOne = returned && (!only || only == returned);
            only = returned;
        }
        frame.valueHandedBack = isOne ? only : ValueID();
    }
    return frame.valueHandedBack && frame.valueHandedBack == body.valueBehindCasts(input);
}

bool Walker::walkInline(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft* writtenOut)
{
    if (!m_opensGuesses || isBeingWalked(callee))
        return false;

    std::optional<Draft> drafted;
    if (!writtenOut)
        drafted.emplace(draftOf(call));
    const Draft& site = writtenOut ? *writtenOut : *drafted;
    Vector<Reference, 2> heldByCallAlone = collectWhatCopyAloneHolds(*this, call);
    Vector<ClosureMadeForCallAtInlining, 2> closuresMadeForCall;
    const Function& function = program().at(callee);
    bool isConstruction = constructionsOfInlining.contains(site.opcode);
    CallOfInlining written;
    written.callee = callee;
    if (isConstruction || site.opcode == Opcode::Call) {
        written.closure = closureCalledAtInlining(site);
        written.arguments = site.argumentsOperands();
    }
    if (!mayBeWalkedAtInlining(*this, callee) || !operandsFitAtInlining(*this, written, function, arguments))
        return false;
    if (!depthOfCopies())
        m_operationsCopiedAtWrittenCall = 0;
    m_operationsCopiedAtWrittenCall += facts().summaries().at(callee).numberOfOperations;
    if (!mayPassGuardAtInlining(*this, function, callee, arguments))
        return false;

    Origin originOfCall = site.origin();
    originOfCall.isTailCall = false;
    m_environmentCellsOfUnit.mayNameSome |= isHandedEnvironmentWithByte(*this, callee);
    Vector<Reference, 8> entryArguments = buildEntryArgumentsAtInlining(*this, originOfCall, written, function, arguments);
    if (isUnreachable())
        return true;

    Vector<Reference, 8> argumentsAsTheyCame;
    BlockID whereGuardFails;
    const CallingConventions& conventions = plan().conventions();
    for (uint32_t index = 0; index < entryArguments.size() && !isUnreachable(); ++index) {
        TypeID tested = conventions.typeBehindGuard(callee, index);
        if (!tested || m_types.isSubset(ownTypeOf(entryArguments[index]), tested))
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        BlockID whereTestPasses = addBlock();
        if (!whereGuardFails) {
            whereGuardFails = addBlock(BlockFrequency::Rare);
            noteGuessOpened();
            argumentsAsTheyCame = entryArguments;
            closuresMadeForCall = collectClosuresMadeForCallAtInlining(*this, call);
        }
        branch(passes, whereTestPasses, whereGuardFails);
        continueIn(whereTestPasses);

        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(entryArguments[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
    }
    const LinkTables& link = facts().linked();
    const LinkedFunction& linked = link.function(callee);
    for (uint32_t index = 0; index < entryArguments.size() && checksProofsOf(ProofSource::EntryParameter) && linked.everyCallerIsKnown && !isUnreachable(); ++index) {
        if (TypeID proven = index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID())
            checkProofOfCopy(entryArguments[index], proven, ProofSource::EntryParameter, originOfCall);
    }
    for (uint32_t index = 0; index < entryArguments.size() && has(Mechanism::Arrivals) && linked.everyCallerIsKnown && !isUnreachable(); ++index) {
        TypeID proven = index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID();
        bool isOneLook = proven && (!m_types.at(proven).hasObjects() || singleFunctionOf(m_types, m_types.at(proven)) || (m_types.at(proven).hasOnlyLayouts() && holdsOneMember(m_types.at(proven).layouts)));
        if (proven && !isOneLook)
            continue;
        TypeID tested = proven && entryArguments[index].isValue() ? m_types.testablePartOf(proven) : TypeID();
        TypeID arriving = tested ? ownTypeOf(entryArguments[index]) : TypeID();
        if (!arriving || !m_types.at(arriving).isJavaScriptValue() || m_types.at(tested).isAnything() || m_types.isSubset(arriving, tested) || !m_types.overlap(arriving, tested))
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        BlockID whereClaimHolds = addBlock();
        BlockID whereClaimFails = addBlock(BlockFrequency::Rare);
        branch(passes, whereClaimHolds, whereClaimFails);
        continueIn(whereClaimFails);
        buildCheckOfProof(entryArguments[index], TypeTable::never, TypeTable::anything, ProofSource::EntryParameter, originOfCall);
        endAsUnreachable(originOfCall);
        continueIn(whereClaimHolds);

        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(entryArguments[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
    }
    if (m_hasFailed)
        return true;
    bool copyIsReached = !isUnreachable();
    bool returnsInPlaceOfUnit = unitReturnsAtOnce(site);

    const BodyIndex& body = facts().bodies().at(callee);
    const BodyIndex& bodyOfCaller = currentBodyIndex();

    uint32_t loopDepthAtCall = m_frames.last().loopDepthAtCall + bodyOfCaller.loopDepthOf(bodyOfCaller.blockOf(call.id()));
    bool callsAreCountedByPlan = callsOfCopyAreCountedForStackTests(call, callee);
    ExpectationOfValue ofAnswer = expectationOfAnswerOfCallBeingCopied(*this, call);
    uint32_t numberOfValuesBuiltBefore = m_code.numberOfValues();
    bool copyIsMadeByLinkOfChain = isCopiedByLinkOfChain(call);
    uint32_t inlineFrame = m_code.addInlineFrame({
        .callee = callee,
        .callPosition = site.position,
        .parent = m_frames.last().inlineFrame,
        .isTailCall = site.isTailCall,
        .isConstruction = isConstruction,
        .callIsPartOfConstruction = site.isPartOfConstruction,
        .module = function.module,
        .calledBuiltin = !isConstruction && function.isHiddenInStackTraces() ? site.calledBuiltin() : BuiltinID(),
    });

    Frame& frame = m_frames.appendNew();
    frame.function = callee;
    frame.body = &body.body();
    frame.index = &body;
    if (function.isModuleBody())
        frame.variablesStoredFirst = facts().linked().variablesStoredFirstIn(callee);
    frame.references.fill(Reference(), body.numberOfValues());
    frame.loopDepthAtCall = loopDepthAtCall;
    frame.returnsInPlaceOfUnit = returnsInPlaceOfUnit;
    frame.callsAreCountedByPlan = callsAreCountedByPlan;
    if (ofAnswer.type) {
        frame.typeExpectedOfAnswer = ofAnswer.type;
        frame.answerIsDeclared = ofAnswer.isDeclared;
    }
    frame.callCopied = call.id();
    frame.numberOfValuesBuiltBefore = numberOfValuesBuiltBefore;
    frame.isCopiedByLinkOfChain = copyIsMadeByLinkOfChain;
    frame.inlineFrame = inlineFrame;

    using ReturnOfCopy = Frame::ReturnOfCopy;
    Vector<ReturnOfCopy, 2> returns;
    m_frames.last().returnsOfCopy = &returns;

    BodyBlock entry = body.block(entryBlock);
    for (uint32_t index = 0; index < entryArguments.size() && copyIsReached; ++index)
        bind(entry.parameter(index), entryArguments[index]);
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFailed && copyIsReached; ++number)
        walkBlock(BlockID(number));
    m_block = { };
    m_state.clear();
    bool wasCopiedByLinkOfChain = m_frames.last().isCopiedByLinkOfChain;

    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_frames.removeLast();
    for (ReturnOfCopy& returned : returns) {
        if (!returned.value)
            returned.value = constant(Draft(Opcode::Undefined, originOfCall));
    }
    size_t numberOfReturnsOfCopy = returns.size();
    m_origin = originOfCall;
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;

    if (whereGuardFails && !m_hasFailed) {
        m_block = { };
        m_state.clear();
        continueIn(whereGuardFails);
        for (Reference& argument : argumentsAsTheyCame) {
            for (const ClosureMadeForCallAtInlining& closure : closuresMadeForCall) {
                if (argument != closure.made || isUnreachable())
                    continue;
                Draft again = draftOf(currentBodyIndex().value(closure.written));
                chooseConservativeFields(again);
                if (Reference madeHere = build(WTF::move(again)))
                    argument = madeHere;
            }
        }
        Reference answer = callSlowVersion(*this, site, callee, argumentsAsTheyCame.span(), isConstruction || (call.opcode() == Opcode::CallDirect && site.isConstruction()));
        if (answer && returnsInPlaceOfUnit)
            returnAtOnce(answer);
        else if (answer)
            returns.append(ReturnOfCopy { m_block, answer, WTF::move(m_state), false });
        m_block = { };
        m_state.clear();
    }

    for (size_t index = numberOfReturnsOfCopy; index < returns.size(); ++index)
        returns[index].isLedByClosedArm = true;

    if (m_hasFailed || returns.isEmpty()) {
        m_block = { };
        m_state.clear();
        return true;
    }

    Vector<ReturnOfCopy*, 4> passing;
    Vector<ReturnOfCopy*, 4> closed;
    for (ReturnOfCopy& returned : returns)
        (returned.isLedByClosedArm ? closed : passing).append(&returned);
    if (wasCopiedByLinkOfChain && !passing.isEmpty() && !closed.isEmpty() && letReturnsOfCopyWalkEndOfBlockApart(call.id(), passing.span(), closed.span())) {
        auto walkEndOfBlockBehind = [&](std::span<ReturnOfCopy* const> ways) {
            Vector<ReturnOfCopy, 2> returnsOfOneKind;
            for (ReturnOfCopy* returned : ways)
                returnsOfOneKind.append(WTF::move(*returned));
            goOnBehindReturnsOfCopy(call, function, linked, heldByCallAlone, originOfCall, returnsOfOneKind);
            walkEndOfBlockBehindReturnsOfCopy(call.id());
        };
        {
            ClosedArm closedArm(*this);
            ++m_numberOfCopiesBuiltForSize;
            walkEndOfBlockBehind(closed.span());
            --m_numberOfCopiesBuiltForSize;
        }
        if (!m_hasFailed)
            walkEndOfBlockBehind(passing.span());
        m_frames.last().references[call.id().index()] = Reference();
        m_block = { };
        m_state.clear();
        m_callIsAccountedFor = true;
        m_calleeAccountedFor = callee;
        return true;
    }
    return goOnBehindReturnsOfCopy(call, function, linked, heldByCallAlone, originOfCall, returns);
}

bool Walker::goOnBehindReturnsOfCopy(const BodyValue& call, const Function& function, const LinkedFunction& linked, const Vector<Reference, 2>& heldByCallAlone, Origin originOfCall, Vector<Frame::ReturnOfCopy, 2>& returns)
{
    using ReturnOfCopy = Frame::ReturnOfCopy;

    if (returns.size() == 1 && !returns[0].isUnderHandlerOfCallee) {
        m_block = returns[0].block;
        m_state = WTF::move(returns[0].state);
        if (checksProofsOf(ProofSource::ResultOfCall) && !function.isCoroutine())
            checkProofOfResultOfCopy(returns[0].value, linked.result, originOfCall);
        letGoWhatCopyHeld(*this, heldByCallAlone.span(), originOfCall);
        bind(call.id(), returns[0].value);
        return true;
    }

    TypeID type;
    Vector<Representation, 2> forms;
    for (const ReturnOfCopy& returned : returns) {
        TypeID arriving = m_code.at(returned.value.id()).type;
        type = type ? joinTypes(type, arriving) : arriving;
        forms.append(representationOf(returned.value));
    }
    BlockID behindCall = addBlock();
    Reference result = addParameter(behindCall, type, formOfParameter(type, forms.span()));
    WalkState stateBehindCall;
    for (size_t index = 0; index < returns.size() && !m_hasFailed; ++index) {
        m_block = returns[index].block;
        m_state = WTF::move(returns[index].state);
        jumpTo(behindCall, singleElementSpan(returns[index].value));
        if (index)
            stateBehindCall.meetWith(m_state);
        else
            stateBehindCall = WTF::move(m_state);
    }
    m_state = WTF::move(stateBehindCall);
    if (checksProofsOf(ProofSource::ResultOfCall) && !function.isCoroutine())
        checkProofOfResultOfCopy(result, linked.result, originOfCall);
    letGoWhatCopyHeld(*this, heldByCallAlone.span(), originOfCall);
    bind(call.id(), result);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
