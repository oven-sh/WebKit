/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubFrontEnds.h"

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubRecording.h"
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
    using namespace StubFamilyTokens;
    static constexpr OperationPrelude preludes[] = {
        { Entry::operationAOTAddressOfThrow, emitAddressOfThrowPrelude, NoEffects },
        { Entry::operationAOTCatch, emitCatchPrelude, writes(RuntimeState) },
        { Entry::operationAOTCatchException, emitCatchExceptionPrelude, writes(RuntimeState) },
        { Entry::operationAOTCatchValueAndException, emitCatchValueAndExceptionPrelude, writes(RuntimeState) },
        { Entry::operationAOTCopyDataProperties, emitCopyDataPropertiesPrelude, AsOperation },
        { Entry::operationAOTCreatePromise, emitCreatePromisePrelude, writes(PromiseState, AllocatorState) },
        { Entry::operationAOTGetClassPrototype, emitGetClassPrototypePrelude, NoEffects },
        { Entry::operationAOTGetConstructPrototype, emitGetConstructPrototypePrelude, NoEffects },
        { Entry::operationAOTGetSuperBase, emitGetSuperBasePrelude, NoEffects },
        { Entry::operationAOTPrivateDefine, emitPrivateDefinePrelude, writes(ObjectFields, NamedProperties, ShapeWords, OrderWords, ArrayStorage, AllocatorState) },
        { Entry::operationAOTPrivateHas, emitPrivateHasPrelude, NoEffects },
        { Entry::operationAOTPrivateSet, emitPrivateSetPrelude, writes(NamedProperties) },
        { Entry::operationAOTStringLocaleCompare, emitStringLocaleComparePrelude, NoEffects },
        { Entry::operationAOTStringToLowerCase, emitToLowerCasePrelude, NoEffects },
        { Entry::operationAOTStringToUpperCase, emitToUpperCasePrelude, NoEffects },
        { Entry::operationAOTThrow, emitThrowPrelude, writes(RuntimeState) },
        { Entry::operationAOTThrowAfterCleanup, emitThrowAfterCleanupPrelude, writes(RuntimeState) },
    };
    static_assert(std::ranges::all_of(preludes, [](const OperationPrelude& prelude) {
        return prelude.ownEffects.toEffects().writes.isSubsetOf(effectsOf(prelude.operation).writes);
    }));
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
