/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTPredictedValues.h"
#include "AOTTypePropagation.h"

namespace JSC { namespace AOT { namespace Typed {

// A type that holds behind checks is written down only where a check stands in front of the value. Whoever takes the check away takes the type along.
void FunctionVerifier::verifyProofs()
{
    bool hasProofBehindChecks = std::ranges::any_of(m_function.values, [](const Value& value) {
        return value.opcode == Opcode::AssumeType && value.isBehindChecks();
    });
    if (!hasProofBehindChecks)
        return;

    PredictedValues predicted(m_function, analyses().cfg());
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::AssumeType && value.isBehindChecks() && !predicted.isBehindCheck(value.valueOperand()))
                fail(InternalErrorKind::ProofWithoutCheck, block, id);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
