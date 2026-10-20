/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTGuardFactDetails.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return reliesOf(kind).isEmpty(); }), "verifyGuards has to visit terminators once one of them relies on a guard");

void FunctionVerifier::verifyGuards()
{
    if (m_form == IRForm::AsRead && !isOptimizing())
        return;

    for (BlockID block : analyses().cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        std::optional<GuardAvailability::Cursor> cursor;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& value = m_function.at(id);
            if (reliesOf(value.opcode).isEmpty())
                continue;

            for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                if (value.operandKind(operandIndex) != OperandKind::Guard)
                    continue;
                ValueID guard = value.operand(operandIndex);
                if (!guard) {
                    if (relyingOnTypesIsEnough(value))
                        continue;
                    fail(InternalErrorKind::InvalidGuard, block, id);
                    return;
                }

                verifyGuardOperand(block, id, value, operandIndex, guard);
                if (hasFailed())
                    return;

                if (!cursor)
                    cursor.emplace(analyses().guards(), block);
                cursor->advanceTo(index);
                if (cursor->stateOf(guard, value) != GuardState::Unavailable)
                    continue;
                fail(InternalErrorKind::GuardNotAvailable, block, id, guard.index());
                return;
            }
        }
    }
}

void FunctionVerifier::verifyGuardOperand(BlockID block, ValueID user, const Value& value, uint32_t, ValueID guard)
{
    const GuardFacts& facts = analyses().guardFacts(guard);
    ProofSet needed = proofsNeededFromGuard(m_program, m_function, value);
    ValueID subject = guardedOperandOf(value);
    ValueID identity = subject ? analyses().identityOf(subject) : ValueID();
    for (Proof proof : allMembersOf<Proof>) {
        if (!needed.contains(proof))
            continue;
        bool isProven = std::ranges::any_of(facts.facts, [&](const GuardFact& fact) {
            return (!identity || fact.subject == identity) && provesForUser(analyses(), fact, proof, value);
        });
        if (isProven)
            continue;
        fail(InternalErrorKind::InvalidGuard, block, user, guard.index());
        return;
    }
}

bool FunctionVerifier::relyingOnTypesIsEnough(const Value& access) const
{
    return proofsNeededFromGuard(m_program, m_function, access).isEmpty();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
