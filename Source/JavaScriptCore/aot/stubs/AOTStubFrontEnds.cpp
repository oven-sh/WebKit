/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubFrontEnds.h"

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsConstruct.h"
#include "AOTStubsOperations.h"
#include "AOTStubsStrings.h"

namespace JSC { namespace AOT {

StubSignature operationFrontEndSignature(unsigned numberOfOperands)
{
    StubSignature signature;
    for (unsigned i = 0; i < numberOfOperands; ++i)
        signature.addOperand(operationArgumentGPR(i + 1));
    signature.result = StubResult::TwoGPRs;
    signature.firstResult = GPRInfo::returnValueGPR;
    signature.secondResult = GPRInfo::returnValueGPR2;
    return signature;
}

std::span<const OperationPrelude> operationPreludes()
{
    static constexpr OperationPrelude preludes[] = {
        { Entry::operationAOTBarrierSlow, emitBarrierPrelude },
        { Entry::operationAOTBarrierSlowPreservingRegisters, emitBarrierPrelude },
        { Entry::operationAOTCatch, emitCatchPrelude },
        { Entry::operationAOTCatchException, emitCatchPrelude },
        { Entry::operationAOTGetConstructPrototype, emitGetConstructPrototypePrelude },
        { Entry::operationAOTGetSuperBase, emitGetSuperBasePrelude },
        { Entry::operationAOTStringToLowerCase, emitToLowerCasePrelude },
        { Entry::operationAOTStringToUpperCase, emitToUpperCasePrelude },
        { Entry::operationAOTThrow, emitThrowPrelude },
        { Entry::operationAOTThrowAfterCleanup, emitThrowAfterCleanupPrelude },
    };
    return preludes;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
