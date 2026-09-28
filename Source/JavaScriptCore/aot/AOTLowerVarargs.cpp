/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "AOTOperationsObjects.h"
#include "B3ValueInlines.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "BaselineJITRegisters.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "FunctionRareData.h"
#include "JSCInlines.h"
#include "SetupVarargsFrame.h"

namespace JSC { namespace AOT {

using namespace B3;

// Every call here is what Lowering::lowerCall() makes: to whatever the callee turns out to be, by way of the thunk that finds
// the callee's code from the callee. What differs is where the arguments come from and what becomes of the caller's frame.

Lowering::Arguments Lowering::lowerArguments(Node* node, unsigned argc, unsigned argv)
{
    Arguments result;
    int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
    for (unsigned i = 0; i < argc; ++i)
        result.append(lowJSValue(node->use(VirtualRegister(firstArgument + i))));
    return result;
}

// A patchpoint goes into the block when it is made: whatever it takes has to have been made before.

static void appendCalleeFrame(Procedure& proc, PatchpointValue* patchpoint, LValue callee, LValue argumentCount, const Vector<LValue, 8>& arguments)
{
    unsigned frameSize = (CallFrame::headerSizeInRegisters + arguments.size()) * sizeof(EncodedJSValue);
    proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(frameSize));

    auto addArgument = [&](LValue value, VirtualRegister reg, int offset) {
        Value::OffsetType offsetFromSP = (reg.offset() - CallerFrameAndPC::sizeInRegisters) * sizeof(EncodedJSValue) + offset;
        patchpoint->append(ConstrainedValue(value, ValueRep::stackArgument(offsetFromSP)));
    };
    addArgument(callee, VirtualRegister(CallFrameSlot::callee), 0);
    addArgument(argumentCount, VirtualRegister(CallFrameSlot::argumentCountIncludingThis), LowWordOffset);
    for (unsigned i = 0; i < arguments.size(); ++i)
        addArgument(arguments[i], virtualRegisterForArgumentIncludingThis(i), 0);
}

// The arguments, in the frame being made. The stubs fill in the rest of it.
static void appendArgumentsOfCalleeFrame(Procedure& proc, PatchpointValue* patchpoint, const Vector<LValue, 8>& arguments)
{
    unsigned frameSize = (CallFrame::headerSizeInRegisters + arguments.size()) * sizeof(EncodedJSValue);
    proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(frameSize));
    for (unsigned i = 0; i < arguments.size(); ++i) {
        Value::OffsetType offsetFromSP = (virtualRegisterForArgumentIncludingThis(i).offset() - CallerFrameAndPC::sizeInRegisters) * sizeof(EncodedJSValue);
        patchpoint->append(ConstrainedValue(arguments[i], ValueRep::stackArgument(offsetFromSP)));
    }
}

LValue Lowering::emitCall(Node* node, LValue callee, const Arguments& arguments, bool isConstruct)
{
    if constexpr (usesStubs) {
        PatchpointValue* patchpoint = m_out.patchpoint(Int64);
        patchpoint->append(ConstrainedValue(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR)));
        appendArgumentsOfCalleeFrame(m_proc, patchpoint, arguments);
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        patchpoint->setGenerator([stubCalls = &m_graph.stubCalls, argc = static_cast<uint32_t>(arguments.size()), callSiteBits = callSiteBitsOf(node), isConstruct](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.move(CCallHelpers::TrustedImm32(callSiteBits), GPRInfo::regT10);
            stubCalls->call(jit, isConstruct ? Stub::Construct : Stub::Call, argc);
            jit.addPtr(CCallHelpers::TrustedImm32(-params.proc().frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        });
        return patchpoint;
    }

    LValue callLinkInfo =entry(isConstruct ? Entry::CallLinkInfoForConstruct : Entry::CallLinkInfoForCall);
    LValue thunk = entry(isConstruct ? Entry::VirtualConstruct : Entry::VirtualCall);
    LValue argumentCount = m_out.constInt32(arguments.size());

    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->append(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR));
    patchpoint->append(callLinkInfo, ValueRep::reg(BaselineJITRegisters::Call::callLinkInfoGPR));
    patchpoint->append(thunk, ValueRep::reg(BaselineJITRegisters::Call::callTargetGPR));
    appendCalleeFrame(m_proc, patchpoint, callee, argumentCount, arguments);
    patchpoint->append(m_notCellMask, ValueRep::reg(GPRInfo::notCellMaskRegister));
    patchpoint->append(m_numberTag, ValueRep::reg(GPRInfo::numberTagRegister));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };

    uint32_t callSiteBits = callSiteBitsOf(node);
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.store32(CCallHelpers::TrustedImm32(callSiteBits), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));
        jit.call(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
        jit.addPtr(CCallHelpers::TrustedImm32(-params.proc().frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
    });
    return patchpoint;
}

// ---- Tail calls

// With the callee's frame where a call would have it: gives the registers this function saved back to its caller, moves the
// frame up over this function's own, and goes.
static void emitTailCallSequence(CCallHelpers& jit, const StackmapGenerationParams& params, unsigned numParameters)
{
    jit.emitRestore(params.proc().calleeSaveRegisterAtOffsetList());
    jit.move(CCallHelpers::TrustedImm32(numParameters), GPRInfo::regT9);
    jit.prepareForTailCallSlow(RegisterSet {
        BaselineJITRegisters::Call::calleeGPR,
        BaselineJITRegisters::Call::callLinkInfoGPR,
        BaselineJITRegisters::Call::callTargetGPR,
    }, GPRInfo::regT9);
    jit.farJump(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
}

void Lowering::emitTailCall(Node*, LValue callee, const Arguments& arguments)
{
    LValue callLinkInfo = entry(Entry::CallLinkInfoForTailCall);
    LValue thunk = entry(Entry::VirtualTailCall);
    LValue argumentCount = m_out.constInt32(arguments.size());

    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->append(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR));
    patchpoint->append(callLinkInfo, ValueRep::reg(BaselineJITRegisters::Call::callLinkInfoGPR));
    patchpoint->append(thunk, ValueRep::reg(BaselineJITRegisters::Call::callTargetGPR));
    appendCalleeFrame(m_proc, patchpoint, callee, argumentCount, arguments);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setGenerator([numParameters = m_graph.codeBlock()->numParameters()](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        emitTailCallSequence(jit, params, numParameters);
    });
}

// Whether the thunk will find code to jump to without asking the runtime, which is in no position to help once the caller's
// frame is gone (see operationAOTPrepareTailCall()).
LValue Lowering::canTailCall(Node* node, LValue callee, Type calleeType)
{
    if (Options::aotDisableFastPaths() & 128)
        return m_out.booleanFalse;

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock functionCase = m_out.newBlock();
    LBasicBlock hasRareData = m_out.newBlock();
    LBasicBlock hasExecutable = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(calleeType, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(callee), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, functionCase);
    m_out.branch(isCellOfType(callee, JSFunctionType), usually(functionCase), rarely(slowCase));

    m_out.appendTo(functionCase, hasRareData);
    LValue executableOrRareData = m_out.loadPtr(callee, m_heaps.JSFunction_executableOrRareData);
    ValueFromBlock direct = m_out.anchor(executableOrRareData);
    m_out.branch(m_out.testNonZeroPtr(executableOrRareData, m_out.constIntPtr(JSFunction::rareDataTag)), unsure(hasRareData), unsure(hasExecutable));

    m_out.appendTo(hasRareData, hasExecutable);
    ValueFromBlock indirect = m_out.anchor(m_out.loadPtr(m_out.address(m_heaps.FunctionRareData_executable, executableOrRareData, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag)));
    m_out.jump(hasExecutable);

    m_out.appendTo(hasExecutable, slowCase);
    LValue executable = m_out.phi(pointerType(), direct, indirect);
    LValue code = m_out.loadPtr(m_out.address(m_heaps.root, executable, ExecutableBase::offsetOfJITCodeWithArityCheckFor(CodeSpecializationKind::CodeForCall)));
    ValueFromBlock fastResult = m_out.anchor(m_out.booleanTrue);
    m_out.branch(m_out.notNull(code), usually(continuation), rarely(slowCase));

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(m_out.notZero64(vmCall(node, Int64, Entry::operationAOTPrepareTailCall, m_globalObject, callee)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int32, fastResult, slowResult);
}

void Lowering::lowerTailCall(Node* node)
{
    auto bytecode = node->as<OpTailCall>();
    Node* calleeNode = node->use(bytecode.m_callee);
    LValue callee = lowJSValue(calleeNode);
    Arguments arguments = lowerArguments(node, bytecode.m_argc, bytecode.m_argv);

    if (usesStubs && !(Options::aotDisableFastPaths() & 128)) {
        // What the callee was found as says what it may well be, and if it is that, there is nothing to call. If it is not, it is
        // called as if this had not been tried: it may be counting on its caller's frame being gone.
        LBasicBlock continuation = nullptr;
        ValueFromBlock resultOfStub;
        StubIntrinsic intrinsic = StubIntrinsic::None;
        if (calleeNode->isBytecode(op_get_by_id))
            intrinsic = stubIntrinsicFor(m_graph.codeBlock()->identifier(calleeNode->as<OpGetById>().m_property).impl(), arguments.size(), true);
        if (intrinsic != StubIntrinsic::None) {
            PatchpointValue* attempt = m_out.patchpoint(m_proc.addTuple({ Int64, Int32 }));
            attempt->append(ConstrainedValue(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR)));
            appendArgumentsOfCalleeFrame(m_proc, attempt, arguments);
            attempt->clobber(RegisterSet::macroClobberedGPRs());
            attempt->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
            attempt->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR), ValueRep::reg(GPRInfo::regT12) };
            attempt->setGenerator([stubCalls = &m_graph.stubCalls, intrinsic, callSiteBits = callSiteBitsOf(node)](CCallHelpers& jit, const StackmapGenerationParams&) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                jit.move(CCallHelpers::TrustedImm32(callSiteBits), GPRInfo::regT10);
                stubCalls->call(jit, Stub::TryCallIntrinsic, static_cast<uint32_t>(intrinsic));
                // (Each of the two is one instruction.)
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::regT12);
                CCallHelpers::Jump done = jit.jump();
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::regT12);
                done.link(&jit);
            });
            LBasicBlock wasThat = m_out.newBlock();
            LBasicBlock wasNotThat = m_out.newBlock();
            continuation = m_out.newBlock();
            m_out.branch(m_out.notZero32(m_out.extract(attempt, 1)), usually(wasThat), rarely(wasNotThat));
            m_out.appendTo(wasThat, wasNotThat);
            resultOfStub = m_out.anchor(m_out.extract(attempt, 0));
            m_out.jump(continuation);
            m_out.appendTo(wasNotThat);
        }

        // The frame is made as for any call. If the callee turns out to be something that can be jumped to, the way out is the same
        // for every tail call of the function: put back what it saved, and let the stub move the frame.
        PatchpointValue* prepare = m_out.patchpoint(Int64);
        prepare->append(ConstrainedValue(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR)));
        appendArgumentsOfCalleeFrame(m_proc, prepare, arguments);
        prepare->clobber(RegisterSet::macroClobberedGPRs());
        prepare->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
        prepare->resultConstraints = { ValueRep::reg(GPRInfo::regT12) };
        prepare->setGenerator([stubCalls = &m_graph.stubCalls, argc = static_cast<uint32_t>(arguments.size()), callSiteBits = callSiteBitsOf(node)](CCallHelpers& jit, const StackmapGenerationParams&) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.move(CCallHelpers::TrustedImm32(argc), GPRInfo::regT9);
            jit.move(CCallHelpers::TrustedImm32(callSiteBits), GPRInfo::regT10);
            stubCalls->call(jit, Stub::TailCallPrepare);
        });

        LBasicBlock tailCase = m_out.newBlock();
        LBasicBlock ordinaryCase = m_out.newBlock();
        m_out.branch(m_out.notZero64(prepare), usually(tailCase), rarely(ordinaryCase));

        m_out.appendTo(tailCase, ordinaryCase);
        if (!m_tailCallBlock)
            m_tailCallBlock = m_out.newBlock();
        m_tailCallCallees.append(m_out.anchor(callee));
        m_tailCallTargets.append(m_out.anchor(prepare));
        m_out.jump(m_tailCallBlock);

        m_out.appendTo(ordinaryCase);
        LValue result = emitCall(node, callee, arguments);
        if (continuation) {
            ValueFromBlock resultOfCall = m_out.anchor(result);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            result = m_out.phi(Int64, resultOfStub, resultOfCall);
        }
        setJSValue(node, result);
        return;
    }

    LBasicBlock tailCase = m_out.newBlock();
    LBasicBlock ordinaryCase = m_out.newBlock();
    m_out.branch(canTailCall(node, callee, calleeNode->type), usually(tailCase), rarely(ordinaryCase));

    m_out.appendTo(tailCase, ordinaryCase);
    emitTailCall(node, callee, arguments);

    // The op_ret that follows returns the result.
    m_out.appendTo(ordinaryCase);
    setJSValue(node, emitCall(node, callee, arguments));
}

// ---- Calls with an array of arguments

// The callee's frame goes below everything this function has on the stack, and is as large as it takes.
LValue Lowering::emitCallVarargs(Node* node, LValue callee, LValue thisValue, LValue arguments, LValue length, int firstVarArg, bool isConstruct, bool isTail)
{
    LValue setupOperation = entry(Entry::operationAOTSetupVarargsFrame);
    LValue callLinkInfo = entry(isTail ? Entry::CallLinkInfoForTailCall : isConstruct ? Entry::CallLinkInfoForConstruct : Entry::CallLinkInfoForCall);
    LValue thunk = entry(isTail ? Entry::VirtualTailCall : isConstruct ? Entry::VirtualConstruct : Entry::VirtualCall);
    LValue handleException = entry(Entry::HandleException);

    PatchpointValue* patchpoint = m_out.patchpoint(isTail ? Void : Int64);
    // For the operation that fills the frame.
    patchpoint->append(m_globalObject, ValueRep::reg(GPRInfo::argumentGPR0));
    patchpoint->append(arguments, ValueRep::reg(GPRInfo::argumentGPR2));
    patchpoint->append(length, ValueRep::reg(GPRInfo::argumentGPR4));
    patchpoint->append(setupOperation, ValueRep::reg(GPRInfo::nonArgGPR0));
    // For after it: these are wherever a call leaves them alone.
    unsigned firstLate = (isTail ? 0 : 1) + 4;
    patchpoint->append(callee, ValueRep::LateColdAny);
    patchpoint->append(thisValue, ValueRep::LateColdAny);
    patchpoint->append(callLinkInfo, ValueRep::LateColdAny);
    patchpoint->append(thunk, ValueRep::LateColdAny);
    patchpoint->append(handleException, ValueRep::LateColdAny);
    patchpoint->append(m_notCellMask, ValueRep::reg(GPRInfo::notCellMaskRegister));
    patchpoint->append(m_numberTag, ValueRep::reg(GPRInfo::numberTagRegister));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
    if (isTail)
        patchpoint->effects.terminal = true;
    else
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };

    uint32_t callSiteBits = callSiteBitsOf(node);
    unsigned numParameters = m_graph.codeBlock()->numParameters();
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        constexpr GPRReg frameGPR = GPRInfo::regT3;
        constexpr GPRReg scratchGPR = GPRInfo::regT1;
        static_assert(noOverlap(frameGPR, scratchGPR, BaselineJITRegisters::Call::calleeGPR, BaselineJITRegisters::Call::callLinkInfoGPR, BaselineJITRegisters::Call::callTargetGPR));
        int32_t frameSize = params.proc().frameSize();
        auto resetStackPointer = [&] {
            jit.addPtr(CCallHelpers::TrustedImm32(-frameSize), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        };

        jit.store32(CCallHelpers::TrustedImm32(callSiteBits), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));

        // The operation writes to the new frame, so its own has to be below that.
        jit.move(CCallHelpers::TrustedImm32(frameSize / sizeof(Register)), GPRInfo::argumentGPR1);
        emitSetVarargsFrame(jit, GPRInfo::argumentGPR4, false, GPRInfo::argumentGPR1, GPRInfo::argumentGPR1);
        jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(sizeof(CallerFrameAndPC) + WTF::roundUpToMultipleOf<stackAlignmentBytes()>(5 * sizeof(void*)))), GPRInfo::argumentGPR1, CCallHelpers::stackPointerRegister);
        jit.move(CCallHelpers::TrustedImm32(firstVarArg), GPRInfo::argumentGPR3);
        jit.call(GPRInfo::nonArgGPR0, OperationPtrTag);

        auto noException = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR2);
        params[firstLate + 4].emitRestore(jit, scratchGPR);
        resetStackPointer();
        jit.farJump(scratchGPR, JITThunkPtrTag);
        noException.link(&jit);

        jit.move(GPRInfo::returnValueGPR, frameGPR);
        params[firstLate + 1].emitRestore(jit, scratchGPR);
        jit.store64(scratchGPR, CCallHelpers::Address(frameGPR, CallFrame::thisArgumentOffset() * static_cast<int>(sizeof(Register))));
        params[firstLate].emitRestore(jit, BaselineJITRegisters::Call::calleeGPR);
        jit.store64(BaselineJITRegisters::Call::calleeGPR, CCallHelpers::Address(frameGPR, CallFrameSlot::callee * static_cast<int>(sizeof(Register))));
        params[firstLate + 2].emitRestore(jit, BaselineJITRegisters::Call::callLinkInfoGPR);
        params[firstLate + 3].emitRestore(jit, BaselineJITRegisters::Call::callTargetGPR);
        jit.addPtr(CCallHelpers::TrustedImm32(sizeof(CallerFrameAndPC)), frameGPR, CCallHelpers::stackPointerRegister);

        if (isTail) {
            emitTailCallSequence(jit, params, numParameters);
            return;
        }
        jit.call(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
        resetStackPointer();
    });
    return patchpoint;
}

void Lowering::lowerCallVarargs(Node* node, VirtualRegister calleeRegister, VirtualRegister thisRegister, VirtualRegister argumentsRegister, int firstVarArg, bool isConstruct, bool isTail)
{
    Node* calleeNode = node->use(calleeRegister);
    LValue callee = lowJSValue(calleeNode);
    LValue thisValue = lowJSValue(node->use(thisRegister));
    LValue arguments = lowJSValue(node->use(argumentsRegister));

    // How much stack this function uses is not known until its code is generated.
    PatchpointValue* numUsedStackSlots = m_out.patchpoint(Int32);
    numUsedStackSlots->effects = Effects::none();
    numUsedStackSlots->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(CCallHelpers::TrustedImm32(params.proc().frameSize() / sizeof(Register)), params[0].gpr());
    });
    LValue length = vmCall(node, Int64, Entry::operationAOTSizeFrameForVarargs, m_globalObject, arguments, numUsedStackSlots, m_out.constInt32(firstVarArg));

    if (!isTail) {
        setJSValue(node, emitCallVarargs(node, callee, thisValue, arguments, length, firstVarArg, isConstruct, false));
        return;
    }

    LBasicBlock tailCase = m_out.newBlock();
    LBasicBlock ordinaryCase = m_out.newBlock();
    m_out.branch(canTailCall(node, callee, calleeNode->type), usually(tailCase), rarely(ordinaryCase));

    m_out.appendTo(tailCase, ordinaryCase);
    emitCallVarargs(node, callee, thisValue, arguments, length, firstVarArg, false, true);

    m_out.appendTo(ordinaryCase);
    setJSValue(node, emitCallVarargs(node, callee, thisValue, arguments, length, firstVarArg, false, false));
}

// ---- eval(...)

// A call whose callee is written "eval". If that is what it is, the code runs in the scope of the caller, and the runtime gets
// the frame that a call would have been made with, which is what shows up in stack traces. If not, this is a call like any other.
void Lowering::lowerCallDirectEval(Node* node)
{
    auto bytecode = node->as<OpCallDirectEval>();
    LValue callee = lowJSValue(node->use(bytecode.m_callee));
    Arguments arguments = lowerArguments(node, bytecode.m_argc, bytecode.m_argv);

    LValue scope = lowCell(node->use(bytecode.m_scope));
    LValue thisValue = lowJSValue(node->use(bytecode.m_thisValue));
    LValue operation = entry(Entry::operationAOTCallDirectEval);
    LValue argumentCount = m_out.constInt32(arguments.size());

    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->append(scope, ValueRep::reg(GPRInfo::argumentGPR1));
    patchpoint->append(thisValue, ValueRep::reg(GPRInfo::argumentGPR2));
    patchpoint->append(operation, ValueRep::reg(GPRInfo::nonArgGPR0));
    appendCalleeFrame(m_proc, patchpoint, callee, argumentCount, arguments);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };

    uint32_t callSiteBits = callSiteBitsOf(node);
    uint32_t bytecodeIndexBits = node->bytecodeIndex.asBits();
    uint32_t lexicallyScopedFeatures = bytecode.m_lexicallyScopedFeatures;
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.store32(CCallHelpers::TrustedImm32(callSiteBits), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));
        // The part of a frame that a callee makes for itself.
        jit.subPtr(CCallHelpers::TrustedImm32(sizeof(CallerFrameAndPC)), CCallHelpers::stackPointerRegister);
        jit.storePtr(GPRInfo::callFrameRegister, CCallHelpers::Address(CCallHelpers::stackPointerRegister, CallFrame::callerFrameOffset()));
        jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::Address(CCallHelpers::stackPointerRegister, CallFrame::returnPCOffset()));
        jit.move(CCallHelpers::stackPointerRegister, GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImm32(bytecodeIndexBits), GPRInfo::argumentGPR3);
        jit.move(CCallHelpers::TrustedImm32(lexicallyScopedFeatures), GPRInfo::argumentGPR4);
        jit.call(GPRInfo::nonArgGPR0, OperationPtrTag);
        jit.addPtr(CCallHelpers::TrustedImm32(-params.proc().frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
    });
    checkException();

    LBasicBlock notEval = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock evalResult = m_out.anchor(patchpoint);
    m_out.branch(m_out.isZero64(patchpoint), rarely(notEval), usually(continuation));

    m_out.appendTo(notEval, continuation);
    ValueFromBlock callResult = m_out.anchor(emitCall(node, callee, arguments));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, evalResult, callResult));
}

bool Lowering::tryLowerCallVariant(Node* node)
{
    switch (node->opcode) {
    case op_tail_call:
        lowerTailCall(node);
        return true;
    case op_super_construct: {
        // What sets it apart from op_construct is what the other tiers learn from it.
        auto bytecode = node->as<OpSuperConstruct>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, true, true);
        return true;
    }
    case op_call_varargs: {
        auto bytecode = node->as<OpCallVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, false, false);
        return true;
    }
    case op_tail_call_varargs: {
        auto bytecode = node->as<OpTailCallVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, false, true);
        return true;
    }
    case op_construct_varargs: {
        auto bytecode = node->as<OpConstructVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, true, false);
        return true;
    }
    case op_super_construct_varargs: {
        auto bytecode = node->as<OpSuperConstructVarargs>();
        lowerCallVarargs(node, bytecode.m_callee, bytecode.m_thisValue, bytecode.m_arguments, bytecode.m_firstVarArg, true, false);
        return true;
    }
    case op_call_direct_eval:
        lowerCallDirectEval(node);
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
