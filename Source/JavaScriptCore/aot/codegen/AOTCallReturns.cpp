/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTCallingConventions.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCodeSites.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTConvention.h"
#include "AOTHintEmitter.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTMachineFrame.h"
#include "AOTStackCheckEmitter.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsValues.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestEmitter.h"
#include "AOTTypeTestPlan.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JSCJSValueInlines.h"
#include "StackAlignment.h"
#include <algorithm>
#include <span>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

static bool returnsNothing(const ArgumentLocations& locations)
{
    return std::ranges::all_of(locations.results, [](const ArgumentLocation& result) {
        return result.kind == ArgumentLocation::Kind::None;
    });
}

static bool hasCompatibleResults(std::span<const ArgumentLocation> callee, std::span<const ArgumentLocation> caller)
{
    return std::ranges::equal(callee, caller, [](const ArgumentLocation& returned, const ArgumentLocation& expected) {
        return returned.kind == expected.kind && returned.reg == expected.reg && returned.stackIndex == expected.stackIndex
            && haveSameRegisterRepresentation(returned.representation, expected.representation);
    });
}

static unsigned spilledBytesOf(const CallingConventions& conventions, CodeUnit unit)
{
    return conventions.spillsArgumentRegisters(unit) ? sizeOfSpilledArgumentRegisters : 0;
}

static void emitStoreToStackWord(CCallHelpers& jit, const Machine::Frame& frame, const Machine::ValueRep& source, GPRReg scratch, uint32_t stackIndex)
{
    CCallHelpers::Address word = frame.addressInFrame(offsetOfStackWordFromFrame(stackIndex));
    if (source.isGPR()) {
        jit.store64(source.gpr(), word);
        return;
    }
    if (source.isFPR()) {
        jit.storeDouble(source.fpr(), word);
        return;
    }
    if (source.isStack())
        jit.load64(frame.addressInFrame(source.offsetFromFP()), scratch);
    else
        source.emitRestore(jit, scratch);
    jit.store64(scratch, word);
}

template<typename Target>
static void emitJumpTo(CCallHelpers& jit, CodeRecords& records, const Target& target)
{
    using TargetKind = decltype(Target::kind);
    switch (target.kind) {
    case TargetKind::Function:
        records.stubCalls.jumpToFunction(jit, target.calledUnit, target.entry);
        return;
    case TargetKind::Stub:
        if (target.immediate)
            records.stubCalls.tailCall(jit, target.calledStub, *target.immediate);
        else
            records.stubCalls.tailCall(jit, target.calledStub);
        return;
    case TargetKind::Address:
        jit.farJump(valueEntryGPR, JSEntryPtrTag);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Target>
static void emitMoveAddressOf(CCallHelpers& jit, CodeRecords& records, const Target& target)
{
    using TargetKind = decltype(Target::kind);
    switch (target.kind) {
    case TargetKind::Function:
        records.stubCalls.moveAddressOfFunction(target.calledUnit, target.entry, valueEntryGPR);
        return;
    case TargetKind::Stub:
        records.stubCalls.moveAddressOfStub(jit, target.calledStub, target.immediate, valueEntryGPR);
        return;
    case TargetKind::Address:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Target>
static bool pollsWithoutFrame(const CallingConventions& conventions, CodeUnit unit, const Target& target)
{
    using TargetKind = decltype(Target::kind);
    if (!hasFaultingLoopPolls || spilledBytesOf(conventions, unit))
        return false;
    if (target.kind == TargetKind::Address)
        return false;
    return target.kind != TargetKind::Function || conventions.stackCheckOf(target.calledUnit) != StackCheck::CoveredByCallers;
}

bool CallEmitter::hasStackWordsOfCaller(uint32_t numberOfWords) const
{
    return !numberOfWords || (conventions().entryConventionOf(m_unit) == EntryConvention::Direct && numberOfWords <= conventions().stackWordsReservedByCallersOf(m_program, m_unit));
}

void CallEmitter::didEmitTailCall()
{
    if (m_code.holdsCallThatPlanHasNotCounted() || conventions().testsStackInFrontOfTailCalls(m_unit))
        m_records.frame.needsStackCheckForTailCalls = true;
}

ALWAYS_INLINE_INTO_OTHER_FILES const ArgumentLocations& CallEmitter::ownLocations()
{
    if (!m_ownLocations)
        m_ownLocations = conventions().locationsOf(m_program, m_unit);
    return *m_ownLocations;
}

void CallEmitter::emitTailCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed, const ArgumentLocations* locationsOfCalledUnit)
{
    RELEASE_ASSERT(placed.onStack.size() <= placed.stackWordsToReserve);
    const ArgumentLocations& own = ownLocations();
    if (!returnsNothing(own)) {
        bool returnsWhereCallerReturns = false;
        switch (target.kind) {
        case TargetKind::Function:
        case TargetKind::Address:
            if (locationsOfCalledUnit)
                returnsWhereCallerReturns = hasCompatibleResults(locationsOfCalledUnit->results.span(), own.results.span());
            else
                returnsWhereCallerReturns = hasCompatibleResults(conventions().locationsOf(m_program, target.calledUnit).results.span(), own.results.span());
            break;
        case TargetKind::Stub: {
            const ArgumentLocation boxedResult { ArgumentLocation::Kind::GPR, GPRInfo::returnValueGPR, 0, Representation::JSValue };
            returnsWhereCallerReturns = hasCompatibleResults(singleElementSpan(boxedResult), own.results.span());
            break;
        }
        }
        if (!returnsWhereCallerReturns) {
            fail(context, InternalErrorKind::InvalidReturnCall);
            m_out.unreachable();
            return;
        }
    }

    bool fitsInIncomingArea = hasStackWordsOfCaller(placed.stackWordsToReserve);
    if (!fitsInIncomingArea && (!m_records.frame.readsCallerFrame || own.numberOfStackResults)) {
        fail(context, InternalErrorKind::InvalidReturnCall);
        m_out.unreachable();
        return;
    }

    didEmitTailCall();
    if (fitsInIncomingArea) {
        if (m_records.frame.needsStackCheckForTailCalls && pollsWithoutFrame(conventions(), m_unit, target))
            emitter<HintEmitter>().pollWhereNoFrameIsSetUp(context);
        jumpAfterFrameTeardown(target, placed);
    } else
        callFromArgumentListFrame(target, placed);
}

Machine::PatchOperation* CallEmitter::leaveAfterFrameTeardown(std::span<const Machine::ConstrainedValue> inRegisters, std::span<const StackWord> stackWords, const Invocable<void(CCallHelpers&)> auto& leave)
{
    static_assert(std::is_trivially_copyable_v<std::remove_cvref_t<decltype(leave)>>);

    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    for (const Machine::ConstrainedValue& value : inRegisters)
        patchpoint->append(value);
    std::span<uint32_t> stackIndices = m_proc.arena().createSpan<uint32_t>(stackWords.size());
    for (size_t i = 0; i < stackWords.size(); ++i) {
        patchpoint->append(stackWords[i].value, Machine::ValueRep::ColdAny);
        stackIndices[i] = stackWords[i].index;
    }
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setRunsWithoutFrame(stackIndices.empty());
    patchpoint->setGenerator([numberInRegisters = static_cast<unsigned>(inRegisters.size()), stackIndices = std::span<const uint32_t>(stackIndices), leave](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg scratch = jit.scratchRegister();
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreToStackWord(jit, params.frame(), params[numberInRegisters + i], scratch, stackIndices[i]);
        if (params.frameIsSetUp())
            Machine::emitFrameTeardown(jit, params.frame());
        else if (params.value()->followsFrameSetup()) {
            RELEASE_ASSERT(params.value()->testsStackAheadOfFramedPart());
            emitStackTestAheadOfFramedPart(jit, params);
        }
        leave(jit);
    });
    return patchpoint;
}

void CallEmitter::jumpAfterFrameTeardown(const CallTarget& target, const PlacedArguments& placed)
{
    Vector<StackWord, 4> stackWords;
    for (uint32_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            stackWords.append({ stackIndex, argument });
    }
    unsigned spilledBytes = spilledBytesOf(conventions(), m_unit);
    RELEASE_ASSERT(stackWords.isEmpty() || !spilledBytes);
    Vector<Machine::ConstrainedValue, 8> inRegisters(placed.inRegisters);
    if (target.kind == TargetKind::Address)
        inRegisters.append(Machine::ConstrainedValue(target.calledAddress, Machine::ValueRep::reg(valueEntryGPR)));

    Machine::PatchOperation* patchpoint = leaveAfterFrameTeardown(inRegisters.span(), stackWords.span(), [records = &m_records, destination = target, spilledBytes](CCallHelpers& jit) {
        if (spilledBytes)
            emitDropSpilledArgumentRegisters(jit);
        emitJumpTo(jit, *records, destination);
    });
    patchpoint->setFollowsFrameSetup(m_records.frame.needsStackCheckForTailCalls && !pollsWithoutFrame(conventions(), m_unit, target));
    patchpoint->setTestsStackAheadOfFramedPart(patchpoint->followsFrameSetup() && isX86_64() && !spilledBytes && stackWords.isEmpty());
}

void CallEmitter::callFromArgumentListFrame(const CallTarget& target, const PlacedArguments& placed)
{
    RELEASE_ASSERT(m_records.frame.readsCallerFrame);
    m_records.frame.dropsArgumentListFrames = true;

    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    for (const Machine::ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    if (target.kind == TargetKind::Address)
        patchpoint->append(Machine::ConstrainedValue(target.calledAddress, Machine::ValueRep::reg(valueEntryGPR)));
    for (size_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            patchpoint->append(argument, Machine::ValueRep::stackArgument(safeCast<Machine::ValueRep::OffsetType>(stackIndex * sizeOfStackWord)));
    }
    m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(placed.stackWordsToReserve * sizeOfStackWord));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(registersClobberedByCompiledCalls());
    patchpoint->effects.terminal = true;
    patchpoint->setNeedsFrameRecord(true);

    patchpoint->setGenerator([records = &m_records, destination = target, numberOfWords = placed.stackWordsToReserve, spilledBytes = spilledBytesOf(conventions(), m_unit)](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        RELEASE_ASSERT(params.frameIsSetUp());
        Machine::emitCalleeSaveRestore(jit, params.frame());
        emitMoveAddressOf(jit, *records, destination);
        if (numberOfWords && numberOfWords <= StubKey::maxCount) {
            records->stubCalls.tailCall(jit, AOT::callFromArgumentListFrameWithWords(numberOfWords, spilledBytes));
            return;
        }
        emitMoveStackWordsToArgumentListFrame(jit, numberOfWords, spilledBytes);
        records->stubCalls.tailCall(jit, AOT::callFromArgumentListFrame());
    });
}

void CallEmitter::tailCallByConvention(const OperationContext& context, const ConventionCall& call)
{
    emitTailCall(context, CallTarget::stub(call.stub.withForm(static_cast<uint8_t>(CallPosition::Tail)), std::nullopt), placeConventionArguments(call));
}

static Origin originAtTailCall(const OperationContext& context)
{
    Origin origin = context.origin();
    origin.isTailCall = true;
    return origin;
}

void CallEmitter::requireCallable(const OperationContext& context, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee, bool isAtTailCall, std::optional<Boxed> listOfCall)
{
    ClosureTestEmitter& closures = emitter<ClosureTestEmitter>();
    LBasicBlock mayNotBeCallable = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isCell(callee), mayNotBeCallable);
    Condition isKnownToBeCallable = closures.isSlimClosureAsCreated(unboxCell(callee));
    m_out.branch(isKnownToBeCallable.value(), normalEdge(continuation), rareEdge(mayNotBeCallable));

    m_out.appendTo(mayNotBeCallable);
    if (listOfCall) {
        LBasicBlock cannotBeCalled = newBlock();
        branchUnless(isCell(callee), cannotBeCalled);
        m_out.branch(emitter<TypeTestEmitter>().isCallable(unboxCell(callee)).value(), normalEdge(continuation), rareEdge(cannotBeCalled));

        m_out.appendTo(cannotBeCalled);
        callOperation<CellPointer>(context, RuntimeOperation::NewArgumentList, { *listOfCall }, { .origin = isAtTailCall ? originAtTailCall(context) : context.origin(), .isOnRarePath = true });
    }
    callStub(context, throwUnlessCallable(), { callee.value() }, { .origin = isAtTailCall ? originAtTailCall(context) : context.origin(), .throwSite = siteThatNamesCallee, .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

bool CallEmitter::mayForwardCallToReceiver(const Operation& call)
{
    if (!call.calleeKey() || !call.calleeName() || !call.thisValueOperand())
        return false;
    BuiltinID member = findOwnMember(m_program, Builtins::FunctionPrototype, call.calleeKey());
    if (member != Builtins::FunctionPrototypeCall && member != Builtins::FunctionPrototypeApply)
        return false;
    if (!types().overlap(typeIDOf(call.calleeOperand()), types().forBuiltin(member)))
        return false;
    if (!types().isSubset(typeIDOf(call.thisValueOperand()), TypeTable::function))
        return true;
    ValueID list = listThatApplyWouldRead(call);
    return list && (typeOf(list).primitives & ~Primitive::UndefinedOrNull);
}

ValueID CallEmitter::listThatApplyWouldRead(const Operation& call)
{
    if (call.opcode != Opcode::Call || call.argumentsOperands().size() < 2)
        return { };
    return findOwnMember(m_program, Builtins::FunctionPrototype, call.calleeKey()) == Builtins::FunctionPrototypeApply ? call.argumentsOperands()[1] : ValueID();
}

void CallEmitter::requireCallableBeforeTailCall(const ValueContext& value, Boxed callee, Boxed thisValue, std::optional<Boxed> listOfCall)
{
    if (mayForwardCallToReceiver(value.value())) {
        requireForwardedCallCanBeMade(value, callee, thisValue, true);
        return;
    }
    if (!types().isSubset(typeIDOf(value->calleeOperand()), TypeTable::function))
        requireCallableBeforeTailCall(value, callee, siteThatNamesCallee(value), listOfCall);
}

void CallEmitter::requireForwardedCallCanBeMade(const ValueContext& value, Boxed callee, Boxed thisValue, bool isAtTailCall)
{
    ValueID listOperand = listThatApplyWouldRead(value.value());
    Boxed list = listOperand ? in<Boxed>(listOperand) : boxedConstant(jsUndefined());
    CallOptions options { .origin = isAtTailCall ? originAtTailCall(value) : value.origin(), .throwSite = siteThatNamesCallee(value), .isOnRarePath = true };
    if (singleBuiltinOf(types(), typeOf(value->calleeOperand()))) {
        callStub(value, throwUnlessForwardedCallCanBeMade(), { callee.value(), thisValue.value(), list.value() }, options);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isCell(callee), slowPath);
    m_out.branch(emitter<ClosureTestEmitter>().isSlimClosureAsCreated(unboxCell(callee)).value(), normalEdge(continuation), rareEdge(slowPath));

    m_out.appendTo(slowPath);
    callStub(value, throwUnlessForwardedCallCanBeMade(), { callee.value(), thisValue.value(), list.value() }, options);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void CallEmitter::tailCallValue(const ValueContext& value, const ValueCall& call)
{
    RELEASE_ASSERT(!call.lentArguments);
    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());
    if (call.methodMayBeUnread) {
        RELEASE_ASSERT(call.methodOfBuiltinClass && !mayForwardCallToReceiver(value.value()));
        LBasicBlock wasRead = newBlock();
        LBasicBlock leaves = newBlock();
        branch(isNotEmpty(call.callee), { rareEdge(wasRead), normalEdge(leaves) });
        m_out.appendTo(wasRead);
        requireCallableBeforeTailCall(value, call.callee, call.thisValue);
        m_out.jump(leaves);
        m_out.appendTo(leaves);
        ConventionCall unread { .stub = callMethodThatMayBeUnreadInTailPosition(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size())), .callee = call.callee.value(), .thisValue = call.thisValue.value(), .arguments = values.span() };
        emitTailCall(value, CallTarget::stub(unread.stub, std::nullopt), placeConventionArguments(unread));
        return;
    }
    requireCallableBeforeTailCall(value, call.callee, call.thisValue);
    bool calleeIsCell = call.calleeIsCell || !mayForwardCallToReceiver(value.value());

    ConventionCall conventionCall { .callee = call.callee.value(), .thisValue = call.thisValue.value(), .arguments = values.span() };
    LValue countOperand = nullptr;
    chooseStubThatCallsValue(conventionCall, countOperand, calleeIsCell);
    tailCallByConvention(value, conventionCall);
}

void ControlEmitter::generateReturn(const TerminatorContext& terminator)
{
    ValueID value = terminator->valueOperand();
    unsigned numberOfResults = conventions().numberOfResults(m_unit);
    Vector<Operand, 4> operands;
    if (!value) {
        RELEASE_ASSERT(numberOfResults == 1);
        operands.append(emitter<ConstantEmitter>().constantOf(jsUndefined(), conventions().resultRepresentation(m_unit)));
    } else if (opcodeOf(value) == Opcode::Results) {
        std::span<const ValueID> listed = operationAt(value).valuesOperands();
        RELEASE_ASSERT(listed.size() == numberOfResults);
        for (unsigned index = 0; index < numberOfResults; ++index) {
            Representation form = conventions().resultRepresentation(m_unit, index);
            operands.append(form == Representation::Void ? Operand { } : generator().getIn(listed[index], form));
        }
    } else if (numberOfResults == 1)
        operands.append(generator().getIn(value, conventions().resultRepresentation(m_unit)));
    else {
        for (unsigned index = 0; index < numberOfResults; ++index)
            operands.append(generator().resultAt(value, index));
    }
    emitter<CallEmitter>().returnValues(terminator, operands.span());
}

void ControlEmitter::generateReturnCall(const TerminatorContext& terminator)
{
    generator().generateReturnedCall(terminator, terminator->callOperand());
}

void CallEmitter::returnValues(const TerminatorContext&, std::span<const Operand> operands)
{
    returnValues(operands);
}

void CallEmitter::returnValues(std::span<const Operand> operands)
{
    std::span<const ArgumentLocation> results = ownLocations().results.span();
    bool spillsArgumentRegisters = conventions().spillsArgumentRegisters(m_unit);
    RELEASE_ASSERT(operands.size() == results.size());
    for (size_t i = 0; i < results.size(); ++i)
        RELEASE_ASSERT(!operands[i].value || isPassedAs(operands[i].representation, results[i].representation));

    if (!spillsArgumentRegisters && results.size() <= 1) {
        if (results.empty() || results[0].kind == ArgumentLocation::Kind::None) {
            m_out.retVoid();
            return;
        }
        RELEASE_ASSERT(results[0].reg == (isPassedInFPR(results[0].representation) ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR)));
        m_out.ret(operands[0].value);
        return;
    }

    Vector<Machine::ConstrainedValue, 8> inRegisters;
    Vector<StackWord, 4> stackWords;
    for (size_t i = 0; i < results.size(); ++i) {
        LValue value = operands[i].value;
        if (!value)
            continue;
        switch (results[i].kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            inRegisters.append(Machine::ConstrainedValue(value, Machine::ValueRep::reg(results[i].reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            stackWords.append({ results[i].stackIndex, value->type() == Machine::Int32 ? m_out.zeroExt(value, Machine::Int64) : value });
            break;
        }
    }
    RELEASE_ASSERT(stackWords.isEmpty() || !spillsArgumentRegisters);
    Machine::PatchOperation* patchpoint = leaveAfterFrameTeardown(inRegisters.span(), stackWords.span(), [spillsArgumentRegisters](CCallHelpers& jit) {
        if (spillsArgumentRegisters)
            emitReturnDroppingSpilledArgumentRegisters(jit);
        else
            jit.ret();
    });
    patchpoint->setLeavesByReturning(true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
