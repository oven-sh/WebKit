/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackCheckEmitter.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTElideStackChecks.h"
#include "AOTInstance.h"
#include "AOTStubBodyConvention.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPolls.h"
#include "AirGenerationContext.h"
#include "B3Procedure.h"
#include "LinkBuffer.h"
#include "StackManager.h"
#include <algorithm>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT { namespace Typed {

static void emitSmallFrameCheck(CCallHelpers& jit, PendingStackCheck& check)
{
    check.limit = StackLimit::TrapAwareSoft;
    check.doesNotFit.append(jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit())));
}

static constexpr bool isMacroScratchGPR(GPRReg reg)
{
#if CPU(X86_64)
    return reg == CCallHelpers::s_scratchRegister;
#else
    return reg == CCallHelpers::dataTempRegister || reg == CCallHelpers::memoryTempRegister;
#endif
}

static_assert([] {
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (isMacroScratchGPR(directArgumentGPR(index)))
            return false;
    }
    for (unsigned index = 0; index < StubBodyConvention::maxNumberOfOperandGPRs; ++index) {
        if (isMacroScratchGPR(StubBodyConvention::operandGPR(index)))
            return false;
    }
    for (GPRReg reg : { thisGPR, countGPR, calleeGPR, valueEntryGPR, instanceGPR, cellCageBaseGPR }) {
        if (isMacroScratchGPR(reg))
            return false;
    }
    return true;
}(), "a frame that is checked before it is allocated is set up at the entry of the function, and its check writes the macro scratch registers");

static void emitLargeFrameCheck(CCallHelpers& jit, PendingStackCheck& check)
{
    // The assembler hands out its scratch register only where the EnterFrame names it as a scratch operand.
    GPRReg newStackPointer = jit.scratchRegister();
    jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(check.bytesBelowFrameRecord), newStackPointer);
    check.limit = StackLimit::Soft;
    check.doesNotFit.append(jit.branchPtr(CCallHelpers::Above, newStackPointer, GPRInfo::callFrameRegister));
    check.doesNotFit.append(jit.branchPtr(CCallHelpers::Below, newStackPointer, CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit())));
}

void setStackCheckGenerator(B3::Procedure& proc, StackCheck stackCheck, const FrameRequirements& frame, StackCheckState& state)
{
    proc.code().setStackCheckGenerator(createSharedTask<B3::Air::StackCheckGeneratorFunction>([stackCheck, &frame, &state](CCallHelpers& jit, B3::Air::GenerationContext& context, const B3::Air::StackCheckSite& site) {
        const B3::Air::Code& code = *context.code;
        unsigned frameSize = code.frameSize();
        bool fitsByProof = stackCheck == StackCheck::CoveredByCallers && frameSize <= maxFrameSizeWithoutStackCheck;
        state.isProvenToFitOnStack |= fitsByProof;
        bool checksStack = (stackCheck != StackCheck::DeferredToCallee && !fitsByProof) || frame.needsStackCheckForTailCalls || frameSize > maxUncheckedFrameSize;
        if (!checksStack)
            return;

        state.isEnteredWithoutStackCheck = false;
        PendingStackCheck check { .bytesBelowFrameRecord = site.bytesBelowFrameRecord };
        if (site.bytesBelowFrameRecord <= code.policy().frame.maxBytesAllocatedBeforeStackCheck)
            emitSmallFrameCheck(jit, check);
        else
            emitLargeFrameCheck(jit, check);
        check.resume = jit.label();
        state.pending.append(WTF::move(check));
    }));
}

void emitStackOverflowPaths(CCallHelpers& jit, CodeRecords& records, StackCheckState& state)
{
    for (const PendingStackCheck& check : state.pending) {
        RELEASE_ASSERT(!check.doesNotFit.empty() && check.resume.isSet());
        check.doesNotFit.link(&jit);
        switch (check.limit) {
        case StackLimit::Soft:
            records.stubCalls.tailCall(jit, throwStackOverflow(true));
            break;
        case StackLimit::TrapAwareSoft: {
            // The stub returns to the resume offset of the PrologueCheck, never to the address after the call.
            CCallHelpers::Label call = jit.label();
            records.stubCalls.call(jit, pollAtPrologue(), OptionalIndex { });
            records.units.pin(call, jit.label());
            records.traps.stackChecks.append({ call, check.resume });
            break;
        }
        }
    }
}

Vector<PrologueCheck, 1> linkStackChecks(LinkBuffer& linkBuffer, FunctionID id, const CodeRecords& records)
{
    Vector<PrologueCheck, 1> checks;
    for (const StackCheckLabels& labels : records.traps.stackChecks) {
        RELEASE_ASSERT(labels.call.isSet() && labels.resume.isSet());
        checks.append({ id.index(), linkBuffer.offsetOf(labels.call), linkBuffer.offsetOf(labels.resume) });
    }
    std::ranges::sort(checks, { }, &PrologueCheck::callOffset);
    return checks;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
