/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "AOTProgram.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "BaselineJITRegisters.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

// A call of what is probably a function that the compiler knows (Graph::knownCallee()). Finding a callee's code takes looking at the
// callee, its executable and its CodeBlock, one after the other, and then a jump to wherever that says. Here the site remembers one
// callee, which the runtime has found to be a closure of that function (operationAOTLinkCall()), and its CodeBlock. For that callee
// the call is an instruction, to an address that is known when the image is put together, past the check of the number of
// arguments if there are enough. Any other callee is called the way any callee is.
bool Lowering::lowerCallToKnownFunction(Node* node, LValue callee, unsigned argc, unsigned argv, bool isConstruct, bool hasResult)
{
    if (!usesStubs)
        return false;
    const KnownFunction* known = m_graph.knownCallee(node);
    if (!known)
        return false;
    UnlinkedFunctionCodeBlock* target = isConstruct ? known->forConstruct : known->forCall;
    if (!target)
        return false;
    unsigned index = m_graph.indexOfKnownCallee(known->keyFor(isConstruct));
    if (!Site::fits(index, isConstruct))
        return false;
    bool skipsArityCheck = argc >= target->numParameters();
    unsigned slot = siteOfKnownCall(node, index, isConstruct);

    Vector<ConstrainedValue> inFrame;
    int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
    for (unsigned i = 0; i < argc; ++i) {
        Value::OffsetType offsetFromSP = (virtualRegisterForArgumentIncludingThis(i).offset() - CallerFrameAndPC::sizeInRegisters) * sizeof(EncodedJSValue);
        inFrame.append(ConstrainedValue(lowJSValue(node->use(VirtualRegister(firstArgument + i))), ValueRep::stackArgument(offsetFromSP)));
    }
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    static_assert(BaselineJITRegisters::Call::calleeGPR == GPRInfo::argumentGPR0);
    patchpoint->append(ConstrainedValue(callee, ValueRep::reg(GPRInfo::argumentGPR0)));
    patchpoint->append(ConstrainedValue(m_data, ValueRep::reg(GPRInfo::argumentGPR1)));
    patchpoint->appendVector(inFrame);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
    uint32_t callSiteBits = CallSiteIndex(node->bytecodeIndex).bits();
    patchpoint->setGenerator([stubCalls = &m_graph.stubCalls, argc, callSiteBits, isConstruct, slot, index, skipsArityCheck](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        constexpr GPRReg callee = GPRInfo::argumentGPR0;
        constexpr GPRReg data = GPRInfo::argumentGPR1;
        auto slotOfNewFrame = [](CallFrameSlot slot, ptrdiff_t offset = 0) {
            return CCallHelpers::Address(CCallHelpers::stackPointerRegister, (static_cast<int>(slot) - CallerFrameAndPC::sizeInRegisters) * static_cast<int>(sizeof(Register)) + offset);
        };
        ptrdiff_t offsetOfSlot = Data::offsetOfSlots() + slot * sizeof(Slot);
        CCallHelpers::JumpList slow;
        jit.loadPtr(CCallHelpers::Address(data, offsetOfSlot + OBJECT_OFFSETOF(Slot, pointer)), GPRInfo::regT9);
        slow.append(jit.branchPtr(CCallHelpers::NotEqual, GPRInfo::regT9, callee));
        // The collector lets go of either without a thought for the other.
        jit.loadPtr(CCallHelpers::Address(data, offsetOfSlot + sizeof(Slot) + OBJECT_OFFSETOF(Slot, pointer)), GPRInfo::regT9);
        slow.append(jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::regT9));
        static_assert(static_cast<int>(CallFrameSlot::callee) == static_cast<int>(CallFrameSlot::codeBlock) + 1);
        jit.storePair64(GPRInfo::regT9, callee, CCallHelpers::stackPointerRegister, CCallHelpers::TrustedImm32(slotOfNewFrame(CallFrameSlot::codeBlock).offset));
        jit.store32(CCallHelpers::TrustedImm32(argc), slotOfNewFrame(CallFrameSlot::argumentCountIncludingThis, LowWordOffset));
        jit.store32(CCallHelpers::TrustedImm32(callSiteBits), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));
        stubCalls->callFunction(jit, isConstruct ? Stub::ConstructFarFunction : Stub::CallFarFunction, index, skipsArityCheck);
        CCallHelpers::Label done = jit.label();
        jit.addPtr(CCallHelpers::TrustedImm32(-params.proc().frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);

        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            slow.link(&jit);
            jit.addPtr(CCallHelpers::TrustedImm32(offsetOfSlot), data);
            jit.move(CCallHelpers::TrustedImm32(argc), GPRInfo::regT9);
            jit.move(CCallHelpers::TrustedImm32(callSiteBits), GPRInfo::regT10);
            stubCalls->call(jit, isConstruct ? Stub::ConstructAndLink : Stub::CallAndLink);
            jit.jump().linkTo(done, &jit);
        });
    });
    if (hasResult)
        setJSValue(node, patchpoint);
    return true;
}

void Lowering::lowerCall(Node* node, VirtualRegister calleeRegister, unsigned argc, unsigned argv, bool isConstruct, bool hasResult)
{
    // A call to whatever the callee turns out to be. There is nothing at the call site to link: the thunk finds the callee's
    // code from the callee, every time.
    LValue callee = lowJSValue(node->use(calleeRegister));

    unsigned frameSize = (CallFrame::headerSizeInRegisters + argc) * sizeof(EncodedJSValue);
    m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(frameSize));

    if (lowerCallToKnownFunction(node, callee, argc, argv, isConstruct, hasResult))
        return;

    if constexpr (usesStubs) {
        // The arguments go in the frame being made; the stub does the rest of what every call does.
        Vector<ConstrainedValue> inFrame;
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        for (unsigned i = 0; i < argc; ++i) {
            Value::OffsetType offsetFromSP = (virtualRegisterForArgumentIncludingThis(i).offset() - CallerFrameAndPC::sizeInRegisters) * sizeof(EncodedJSValue);
            inFrame.append(ConstrainedValue(lowJSValue(node->use(VirtualRegister(firstArgument + i))), ValueRep::stackArgument(offsetFromSP)));
        }
        PatchpointValue* patchpoint = m_out.patchpoint(Int64);
        patchpoint->append(ConstrainedValue(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR)));
        patchpoint->appendVector(inFrame);
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        uint32_t callSiteBits = CallSiteIndex(node->bytecodeIndex).bits();
        patchpoint->setGenerator([stubCalls = &m_graph.stubCalls, argc, callSiteBits, isConstruct](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.move(CCallHelpers::TrustedImm32(argc), GPRInfo::regT9);
            jit.move(CCallHelpers::TrustedImm32(callSiteBits), GPRInfo::regT10);
            stubCalls->call(jit, isConstruct ? Stub::Construct : Stub::Call);
            jit.addPtr(CCallHelpers::TrustedImm32(-params.proc().frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        });
        if (hasResult)
            setJSValue(node, patchpoint);
        return;
    }

    Vector<ConstrainedValue> arguments;
    arguments.append(ConstrainedValue(callee, ValueRep::reg(BaselineJITRegisters::Call::calleeGPR)));
    arguments.append(ConstrainedValue(entry(isConstruct ? Entry::CallLinkInfoForConstruct : Entry::CallLinkInfoForCall), ValueRep::reg(BaselineJITRegisters::Call::callLinkInfoGPR)));
    arguments.append(ConstrainedValue(entry(isConstruct ? Entry::VirtualConstruct : Entry::VirtualCall), ValueRep::reg(BaselineJITRegisters::Call::callTargetGPR)));

    auto addArgument = [&](LValue value, VirtualRegister reg, int offset) {
        Value::OffsetType offsetFromSP = (reg.offset() - CallerFrameAndPC::sizeInRegisters) * sizeof(EncodedJSValue) + offset;
        arguments.append(ConstrainedValue(value, ValueRep::stackArgument(offsetFromSP)));
    };
    addArgument(callee, VirtualRegister(CallFrameSlot::callee), 0);
    addArgument(m_out.constInt32(argc), VirtualRegister(CallFrameSlot::argumentCountIncludingThis), LowWordOffset);
    int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
    for (unsigned i = 0; i < argc; ++i)
        addArgument(lowJSValue(node->use(VirtualRegister(firstArgument + i))), virtualRegisterForArgumentIncludingThis(i), 0);

    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    patchpoint->appendVector(arguments);
    patchpoint->append(m_notCellMask, ValueRep::reg(GPRInfo::notCellMaskRegister));
    patchpoint->append(m_numberTag, ValueRep::reg(GPRInfo::numberTagRegister));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet::registersToSaveForJSCall(RegisterSet::allScalarRegisters()));
    patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };

    uint32_t callSiteBits = CallSiteIndex(node->bytecodeIndex).bits();
    patchpoint->setGenerator([=](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.store32(CCallHelpers::TrustedImm32(callSiteBits), CCallHelpers::highWordFor(CallFrameSlot::argumentCountIncludingThis));
        jit.call(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
        jit.addPtr(CCallHelpers::TrustedImm32(-params.proc().frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
    });

    if (hasResult)
        setJSValue(node, patchpoint);
}

bool Lowering::tryLowerCall(Node* node)
{
    switch (node->opcode) {
    case op_call: {
        auto bytecode = node->as<OpCall>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, true);
        return true;
    }
    case op_call_ignore_result: {
        auto bytecode = node->as<OpCallIgnoreResult>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, false);
        return true;
    }
    case op_tail_call: {
        // As an ordinary call: the op_ret that follows returns the result. The frame is not reused.
        auto bytecode = node->as<OpTailCall>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, false, true);
        return true;
    }
    case op_construct: {
        auto bytecode = node->as<OpConstruct>();
        lowerCall(node, bytecode.m_callee, bytecode.m_argc, bytecode.m_argv, true, true);
        return true;
    }
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
