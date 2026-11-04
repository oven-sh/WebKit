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
#include "AOTMachineFrame.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsValues.h"
#include "AOTTypeArena.h"
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

// CallTarget is private to CallEmitter, so its type is deduced.
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
        // It came in that register.
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A unit whose check its callers cover is entered behind a check. Every other target checks the stack for itself where it sets up a frame, unless it calls nothing and jumps to nothing, so that no cycle leads through it.
template<typename Target>
static bool pollsWithoutFrame(const CallingConventions& conventions, CodeUnit unit, const Target& target)
{
    using TargetKind = decltype(Target::kind);
    if (!hasFaultingLoopPolls || spilledBytesOf(conventions, unit))
        return false;
    // The method table may hold the body of a method itself.
    if (target.kind == TargetKind::Address)
        return false;
    return target.kind != TargetKind::Function || conventions.stackCheckOf(target.calledUnit) != StackCheck::CoveredByCallers;
}

bool CallEmitter::hasStackWordsOfCaller(uint32_t numberOfWords) const
{
    return !numberOfWords || (conventions().entryConventionOf(m_unit) == EntryConvention::Direct && numberOfWords <= conventions().stackWordsReservedByCallersOf(m_program, m_unit));
}

// The check is the poll of a cycle of tail calls, and what a callee counts on whose frames nobody has counted below the unit.
void CallEmitter::didEmitTailCall()
{
    if (m_code.holdsCallThatPlanHasNotCounted() || conventions().testsStackInFrontOfTailCalls(m_unit))
        m_records.frame.needsStackCheckForTailCalls = true;
}

void CallEmitter::emitTailCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed)
{
    RELEASE_ASSERT(placed.onStack.size() <= placed.stackWordsToReserve);
    ArgumentLocations own = conventions().locationsOf(m_program, m_unit);
    if (!returnsNothing(own)) {
        bool returnsWhereCallerReturns = false;
        switch (target.kind) {
        case TargetKind::Function:
        case TargetKind::Address:
            returnsWhereCallerReturns = hasCompatibleResults(conventions().locationsOf(m_program, target.calledUnit).results.span(), own.results.span());
            break;
        case TargetKind::Stub: {
            // Whatever a stub of the call convention enters returns one boxed value.
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
    // Whether a unit leaves from argument list frames is settled before its code is generated. Such a frame is gone once the callee has returned, so no result may lie in it.
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
    // The generator holds a copy in the arena.
    static_assert(std::is_trivially_copyable_v<std::remove_cvref_t<decltype(leave)>>);

    Machine::PatchOperation* patchpoint = m_out.patchpoint(Machine::Void);
    for (const Machine::ConstrainedValue& value : inRegisters)
        patchpoint->append(value);
    std::span<uint32_t> stackIndices = m_proc.arena().createSpan<uint32_t>(stackWords.size());
    for (size_t i = 0; i < stackWords.size(); ++i) {
        patchpoint->append(stackWords[i].value, Machine::ValueRep::ColdAny);
        stackIndices[i] = stackWords[i].index;
    }
    // No scratch register is asked of the allocator: an operand that lies in a register keeps it, and on x86-64 those and the fixed ones may be all there are.
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setRunsWithoutFrame(stackIndices.empty());
    patchpoint->setGenerator([numberInRegisters = static_cast<unsigned>(inRegisters.size()), stackIndices = std::span<const uint32_t>(stackIndices), leave](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        // The frame may be gone already: the registers that it saved are the caller's again.
        emitClearOfReleasedRegisters(jit, params, registersClobberedByCompiledCalls());
        GPRReg scratch = jit.scratchRegister();
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreToStackWord(jit, params.frame(), params[numberInRegisters + i], scratch, stackIndices[i]);
        if (params.frameIsSetUp())
            Machine::emitFrameTeardown(jit, params.frame());
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
    // The frame of the callee lies where this one was.
    // The stack check is part of the frame setup.
    patchpoint->setFollowsFrameSetup(m_records.frame.needsStackCheckForTailCalls && !pollsWithoutFrame(conventions(), m_unit, target));
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
        emitMoveStackWordsToArgumentListFrame(jit, numberOfWords, spilledBytes);
        emitMoveAddressOf(jit, *records, destination);
        records->stubCalls.tailCall(jit, AOT::callFromArgumentListFrame());
    });
}

void CallEmitter::tailCallByConvention(const OperationContext& context, const ConventionCall& call)
{
    emitTailCall(context, CallTarget::stub(call.stub.withForm(static_cast<uint8_t>(CallPosition::Tail)), std::nullopt), placeConventionArguments(call));
}

// A stack trace tells a frame at a failed tail call from one at an ordinary call: TraceFrame::isAtFailedTailCall.
static Origin originAtTailCall(const OperationContext& context)
{
    Origin origin = context.origin();
    origin.isTailCall = true;
    return origin;
}

// The stub that calls finds out too late that there is nothing to call: the frame that the error names is gone by then.
void CallEmitter::requireCallable(const OperationContext& context, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee, bool isAtTailCall)
{
    ClosureTestEmitter& closures = emitter<ClosureTestEmitter>();
    LBasicBlock mayNotBeCallable = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isCell(callee), mayNotBeCallable);
    Condition isKnownToBeCallable = closures.hasFunctionCellType(unboxCell(callee));
    m_out.branch(isKnownToBeCallable.value(), normalEdge(continuation), rareEdge(mayNotBeCallable));

    m_out.appendTo(mayNotBeCallable);
    callStub(context, throwUnlessCallable(), { callee.value() }, { .origin = isAtTailCall ? originAtTailCall(context) : context.origin(), .throwSite = siteThatNamesCallee, .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

bool CallEmitter::mayForwardCallToReceiver(const ValueContext& value)
{
    if (!value->calleeKey() || !value->calleeName() || !value->thisValueOperand())
        return false;
    BuiltinID member = findOwnMember(m_program, Builtins::FunctionPrototype, value->calleeKey());
    if (member != Builtins::FunctionPrototypeCall && member != Builtins::FunctionPrototypeApply)
        return false;
    if (!types().overlap(typeIDOf(value->calleeOperand()), types().forBuiltin(member)))
        return false;
    return !types().isSubset(typeIDOf(value->thisValueOperand()), TypeTable::function);
}

void CallEmitter::requireCallableBeforeTailCall(const ValueContext& value, Boxed callee, Boxed thisValue)
{
    if (!mayForwardCallToReceiver(value)) {
        if (!types().isSubset(typeIDOf(value->calleeOperand()), TypeTable::function))
            requireCallableBeforeTailCall(value, callee, siteThatNamesCallee(value));
        return;
    }

    CallOptions options { .origin = originAtTailCall(value), .throwSite = siteThatNamesCallee(value), .isOnRarePath = true };
    if (singleBuiltinOf(types(), typeOf(value->calleeOperand()))) {
        callStub(value, throwUnlessForwardedCalleeIsCallable(), { callee.value(), thisValue.value() }, options);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isCell(callee), slowPath);
    m_out.branch(emitter<ClosureTestEmitter>().isSlimClosureAsCreated(unboxCell(callee)).value(), normalEdge(continuation), rareEdge(slowPath));

    m_out.appendTo(slowPath);
    callStub(value, throwUnlessForwardedCalleeIsCallable(), { callee.value(), thisValue.value() }, options);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// The frame holds closures that the call lends. It stays until a callee returns that cannot come round to this call again by tail calls alone. What follows stands where the callee is any other.
void CallEmitter::callAndReturnIfCalleeEndsItsTailCalls(const ValueContext& value, const ValueCall& call)
{
    ArgumentLocations own = conventions().locationsOf(m_program, m_unit);
    if (own.results.size() != 1 || own.results[0].representation != Representation::JSValue)
        return;

    LBasicBlock keepsFrame = newBlock();
    LBasicBlock givesUpFrame = newBlock();
    emitter<ClosureTestEmitter>().branchOnEndsItsTailCalls(call.callee, { normalEdge(keepsFrame), normalEdge(givesUpFrame) });

    m_out.appendTo(keepsFrame);
    Boxed result = callValue(value, call, { .isTailCall = true });
    Operand operand = result;
    returnValues(*value.returnCall(), singleElementSpan(operand));

    m_out.appendTo(givesUpFrame);
}

void CallEmitter::tailCallValue(const ValueContext& value, const ValueCall& call)
{
    requireCallableBeforeTailCall(value, call.callee, call.thisValue);

    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());
    if (call.lentArguments) {
        callAndReturnIfCalleeEndsItsTailCalls(value, call);
        for (unsigned index = 0; index < values.size(); ++index) {
            if (call.lentArguments >> index & 1)
                values[index] = boxCell(callOperation<CellPointer>(value, RuntimeOperation::MoveClosureToHeap, { unboxCell(call.arguments[index]) })).value();
        }
    }

    ConventionCall conventionCall { .callee = call.callee.value(), .thisValue = call.thisValue.value(), .arguments = values.span() };
    LValue countOperand = nullptr;
    chooseStubThatCallsValue(conventionCall, countOperand, call.calleeIsCell);
    tailCallByConvention(value, conventionCall);
}

void ControlEmitter::generateReturn(const TerminatorContext& terminator)
{
    ValueID value = terminator->valueOperand();
    unsigned numberOfResults = conventions().numberOfResults(m_unit);
    // A constant has no form of its own, and a cell and its boxed form are one word: each value is asked for in the form of its place.
    Vector<Operand, 4> operands;
    if (!value) {
        RELEASE_ASSERT(numberOfResults == 1);
        operands.append(emitter<ConstantEmitter>().constantOf(jsUndefined(), conventions().resultRepresentation(m_unit)));
    } else if (opcodeOf(value) == Opcode::Results) {
        std::span<const ValueID> listed = operationAt(value).valuesOperands();
        RELEASE_ASSERT(listed.size() == numberOfResults);
        // A word that says nothing is handed back all the same, as zero: the caller keeps it where the collector reads until it has asked the word that tells,
        // and what the register held last may be the very object.
        for (unsigned index = 0; index < numberOfResults; ++index) {
            // A result that nobody takes has no place.
            Representation form = conventions().resultRepresentation(m_unit, index);
            operands.append(form == Representation::Void ? Operand { } : generator().getIn(listed[index], form));
        }
    } else if (numberOfResults == 1)
        operands.append(generator().getIn(value, conventions().resultRepresentation(m_unit)));
    else {
        // The results of a call, in the forms in which its callee returns them.
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
    ArgumentLocations locations = conventions().locationsOf(m_program, m_unit);
    std::span<const ArgumentLocation> results = locations.results.span();
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
