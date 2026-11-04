/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackCheckEmitter.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTMachineFrame.h"
#include "AOTSort.h"
#include "AOTStubRegisters.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPolls.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "LinkBuffer.h"
#include "MinimumReservedZoneSize.h"
#include "StackManager.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(Machine::maxBytesAllocatedBeforeStackCheck + maxStackUsedWithoutCheck < minimumReservedZoneSize, "the stack pointer of a frame that is allocated and then found not to fit still lies in the reserved zone");

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
    for (GPRReg reg : { thisGPR, countGPR, calleeGPR, valueEntryGPR, instanceGPR, cellCageBaseGPR }) {
        if (isMacroScratchGPR(reg))
            return false;
    }
    return true;
}(), "a frame may be set up at the entry of the function, where the arguments are in their registers, and its check writes the macro scratch registers");

static CCallHelpers::Address wordOfStackLimit(StackLimit limit)
{
    switch (limit) {
    case StackLimit::TrapAwareSoft:
        return CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit());
    case StackLimit::Soft:
        return CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitSmallFrameCheck(CCallHelpers& jit, PendingStackCheck& check)
{
    check.limit = StackLimit::TrapAwareSoft;
    // The stack pointer is the left operand: ARM64 compares it only there, and copies it into the scratch register otherwise.
    check.doesNotFit.append(jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, wordOfStackLimit(check.limit)));
}

static void emitLargeFrameCheck(CCallHelpers& jit, PendingStackCheck& check, unsigned bytesBelowFrameRecord)
{
    GPRReg endOfFrame = jit.scratchRegister();
    jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(bytesBelowFrameRecord)), GPRInfo::callFrameRegister, endOfFrame);
    check.limit = StackLimit::Soft;
    check.doesNotFit.append(jit.branchPtr(CCallHelpers::Below, GPRInfo::callFrameRegister, endOfFrame));
    check.doesNotFit.append(jit.branchPtr(CCallHelpers::Below, endOfFrame, wordOfStackLimit(check.limit)));
}

static void generateStackCheckAtFrameSetup(void* context, CCallHelpers& jit, const Machine::Frame& frame, const Machine::StackCheckSite& site)
{
    StackCheckState& state = *static_cast<StackCheckState*>(context);
    unsigned frameSize = frame.frameSize();
    // A small frame goes untested where the callers have counted it, and where the function calls nothing and jumps to nothing: checkUncheckedStackUsage() counts both.
    bool fitsByProof = (state.stackCheck == StackCheck::CoveredByCallers || site.nothingCountsOnCheck) && frameSize <= maxFrameSizeWithoutStackCheck;
    state.isProvenToFitOnStack |= fitsByProof;
    if (fitsByProof && !state.frame->needsStackCheckForTailCalls)
        return;

    RELEASE_ASSERT(site.isAllocated == (site.bytesBelowFrameRecord <= Machine::maxBytesAllocatedBeforeStackCheck));
    state.isEnteredWithoutStackCheck = false;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    PendingStackCheck check { .isAtEntry = frame.coverage() == Machine::FrameCoverage::WholeFunction };
    if (site.isAllocated)
        emitSmallFrameCheck(jit, check);
    else
        emitLargeFrameCheck(jit, check, site.bytesBelowFrameRecord);
    check.resume = jit.label();
    state.pending.append(WTF::move(check));
}

void setStackCheckGenerator(Machine::Procedure& proc, StackCheck stackCheck, const FrameRequirements& frame, StackCheckState& state)
{
    state.stackCheck = stackCheck;
    state.frame = &frame;
    proc.setStackCheckGenerator(generateStackCheckAtFrameSetup, &state);
}

void emitStackOverflowPaths(CCallHelpers& jit, CodeRecords& records, StackCheckState& state)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    // Each of these stands behind the push of a frame record, and the way out takes the stack pointer from the frame pointer, so one serves them all.
    CCallHelpers::JumpList doesNotFit;
    for (const PendingStackCheck& check : state.pending) {
        RELEASE_ASSERT(!check.doesNotFit.empty() && check.resume.isSet());
        switch (check.limit) {
        case StackLimit::Soft:
            doesNotFit.append(check.doesNotFit);
            break;
        case StackLimit::TrapAwareSoft: {
            check.doesNotFit.link(&jit);
            // The allocator takes no frame setup for a safepoint. Behind the entry, narrow references are live without their pointers.
            bool mayCollect = state.functionMayCollect && check.isAtEntry;
            // The stub returns to the resume offset of the PrologueCheck, never to the address after the call.
            CCallHelpers::Label call = jit.label();
            records.stubCalls.call(jit, pollAtPrologue(mayCollect), OptionalIndex { });
            records.traps.stackChecks.append({ call, check.resume });
            break;
        }
        }
    }
    if (doesNotFit.empty())
        return;

    doesNotFit.link(&jit);
    // A caller without a frame record is found by the stack pointer alone, so the stub has to find the stack as that caller left it.
    if (isX86_64() && records.frame.spillsArgumentRegisters) {
        jit.emitFunctionEpilogue();
        emitDropSpilledArgumentRegisters(jit);
        records.stubCalls.tailCall(jit, throwStackOverflow(false));
        return;
    }
    records.stubCalls.tailCall(jit, throwStackOverflow(true));
}

Vector<PrologueCheck, 1> linkStackChecks(LinkBuffer& linkBuffer, const CodeRecords& records)
{
    Vector<PrologueCheck, 1> checks;
    for (const StackCheckLabels& labels : records.traps.stackChecks) {
        RELEASE_ASSERT(labels.call.isSet() && labels.resume.isSet());
        checks.append(PrologueCheck { .callOffset = linkBuffer.offsetOf(labels.call), .resumeOffset = linkBuffer.offsetOf(labels.resume) });
    }
    AOT::sortInLine(checks, { }, &PrologueCheck::callOffset);
    return checks;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
