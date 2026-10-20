/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsConstants.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

// On x86-64 the immediate and the result share a register.
static StubSignature signatureOfConstant()
{
    StubSignature signature;
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    signature.immediate = isARM64() ? gprOf(stubImmediateRegister) : GPRInfo::returnValueGPR;
    return signature;
}

static void createConstantAndReturn(StubAssembler& jit, Entry operation, GPRReg number)
{
    jit.callOperationAndReturn(operation, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = number,
    });
}

static void jumpToUnboundStub(StubAssembler& jit)
{
    jit.move(TrustedImm32(*jit.key().immediate()), *jit.signature().immediate);
    jit.jumpToStub(jit.key().withoutImmediate());
}

static void returnCellFromTable(StubAssembler& jit, int32_t offsetOfTable, Entry createOperation)
{
    const GPRReg number = jit.immediate();
    const GPRReg result = jit.result();
#if CPU(X86_64)
    constexpr GPRReg table = CCallHelpers::s_scratchRegister;
    const GPRReg index = jit.allocateScratch(StubRegister::T9);
#else
    const GPRReg table = jit.allocateScratch(StubRegister::T11);
    const GPRReg index = jit.allocateScratch(StubRegister::T12);
#endif
    RELEASE_ASSERT(noOverlap(result, table, index, instanceGPR, cellCageBaseGPR) && number != table);

    jit.loadPtr(Address(instanceGPR, offsetOfTable), table);
    jit.zeroExtend32ToWord(number, index);
    jit.load64(BaseIndex(table, index, CCallHelpers::TimesEight), result);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    createConstantAndReturn(jit, createOperation, index);
}

static Entry createOperationOf(CreatedConstant constant)
{
    switch (constant) {
    case CreatedConstant::String:
        return Entry::operationAOTStringConstant;
    case CreatedConstant::NewString:
        return Entry::operationAOTNewStringConstant;
    case CreatedConstant::Key:
        return Entry::operationAOTKeyConstant;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfCreateConstant(StubKey)
{
    return signatureOfConstant();
}

void generateCreateConstant(StubAssembler& jit)
{
    createConstantAndReturn(jit, createOperationOf(static_cast<CreatedConstant>(jit.key().form())), jit.immediate());
}

StubSignature signatureOfBuiltinCell(StubKey)
{
    StubSignature signature = signatureOfConstant();
    signature.boundImmediates = StubBoundImmediates::Every;
    return signature;
}

void generateBuiltinCell(StubAssembler& jit)
{
    constexpr int32_t offsetOfTable = AOT_INSTANCE_PREFIX_OFFSET(builtinCells);
    std::optional<uint32_t> index = jit.key().immediate();
    if (!index) {
        returnCellFromTable(jit, offsetOfTable, Entry::operationAOTBuiltinCell);
        return;
    }
    const GPRReg result = jit.result();

    jit.loadPtr(Address(instanceGPR, offsetOfTable), result);
    jit.load64(Address(result, safeCast<int32_t>(*index * sizeof(JSCell*))), result);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    jumpToUnboundStub(jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
