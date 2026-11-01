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
#include "AOTStubsField.h"
#include "AOTStubsObjectCopy.h"
#include "AOTStubsOperations.h"
#include "AOTStubsStrings.h"
#include <algorithm>

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
        { Entry::operationAOTAddressOfThrow, emitAddressOfThrowPrelude },
        { Entry::operationAOTCatch, emitCatchPrelude },
        { Entry::operationAOTCatchException, emitCatchExceptionPrelude },
        { Entry::operationAOTCatchValueAndException, emitCatchValueAndExceptionPrelude },
        { Entry::operationAOTCopyDataProperties, emitCopyDataPropertiesPrelude },
        { Entry::operationAOTCreatePromise, emitCreatePromisePrelude },
        { Entry::operationAOTGetConstructPrototype, emitGetConstructPrototypePrelude },
        { Entry::operationAOTGetSuperBase, emitGetSuperBasePrelude },
        { Entry::operationAOTJSONAppendValue, emitJSONAppendValuePrelude },
        { Entry::operationAOTPrivateDefine, emitPrivateDefinePrelude },
        { Entry::operationAOTPrivateSet, emitPrivateSetPrelude },
        { Entry::operationAOTStringToLowerCase, emitToLowerCasePrelude },
        { Entry::operationAOTStringToUpperCase, emitToUpperCasePrelude },
        { Entry::operationAOTThrow, emitThrowPrelude },
        { Entry::operationAOTThrowAfterCleanup, emitThrowAfterCleanupPrelude },
    };
    return preludes;
}

bool hasPrelude(Entry entry)
{
    return std::ranges::any_of(operationPreludes(), [&](const OperationPrelude& prelude) {
        return prelude.operation == entry;
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
