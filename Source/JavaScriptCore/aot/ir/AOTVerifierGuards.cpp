/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTGuardFactDetails.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return reliesOf(kind).isEmpty(); }), "verifyGuards has to visit terminators once one of them relies on a guard");

String FunctionVerifier::describeWhatMayEnd(BlockID blockOfUser, ValueID guard, const Value& user)
{
    // Those that stand nearest in front of the use, in the order in which the blocks are walked.
    constexpr unsigned mostListed = 6;
    Vector<std::pair<BlockID, ValueID>> found;
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (analyses().guards().mayBeEndedAt(guard, user, { block, index }))
                found.append({ block, values[index] });
        }
        if (block == blockOfUser)
            break;
    }
    StringBuilder listed;
    for (size_t index = found.size() > mostListed ? found.size() - mostListed : 0; index < found.size(); ++index)
        listed.append(listed.isEmpty() ? ""_s : ", "_s, "v"_s, found[index].second.index(), " ("_s, nameOf(m_function.at(found[index].second).opcode), ") in b"_s, found[index].first.index());
    return makeString("the guard is v"_s, guard.index(), "; of "_s, found.size(), " operations in front of the use that may end what it proves the nearest are "_s, listed.toString());
}

void FunctionVerifier::verifyGuards()
{
    if (m_form == IRForm::AsRead && !isOptimizing())
        return;

    std::optional<BitVector> readsKeptForValidation;
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        std::optional<GuardAvailability::Cursor> cursor;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& value = m_function.at(id);
            if (reliesOf(value.opcode).isEmpty())
                continue;

            ProofSet neededFromGuard = proofsNeededFromGuard(m_program, m_function, value);
            if (neededFromGuard.isEmpty())
                continue;

            for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                if (value.operandKind(operandIndex) != OperandKind::Guard)
                    continue;
                ValueID guard = value.operand(operandIndex);
                if (!guard) {
                    fail(InternalErrorKind::InvalidGuard, block, id);
                    return;
                }

                if (!cursor)
                    cursor.emplace(analyses().guards(), block);
                cursor->advanceTo(index);
                GuardState state = cursor->stateOf(guard, value);
                if (state == GuardState::Available)
                    continue;

                verifyGuardOperand(block, id, value, neededFromGuard, guard);
                if (hasFailed())
                    return;
                if (state != GuardState::Unavailable || m_inputs.cannotJudgeGuards || analyses().escape().gaveUp())
                    continue;
                if (!readsKeptForValidation)
                    readsKeptForValidation = findReadsKeptForValidation(m_function);
                if (readsKeptForValidation->get(id.index()))
                    continue;
                fail(InternalErrorKind::GuardNotAvailable, block, id, guard.index());
                m_errors.last().detail = describeWhatMayEnd(block, guard, value);
                return;
            }
        }
    }
}

void FunctionVerifier::verifyGuardOperand(BlockID block, ValueID user, const Value& value, ProofSet neededFromGuard, ValueID guard)
{
    const GuardFacts& facts = analyses().guardFacts(guard);
    ValueID subject = guardedOperandOf(value);
    ValueID identity = subject ? analyses().identityOf(subject) : ValueID();
    for (Proof proof : allMembersOf<Proof>) {
        if (!neededFromGuard.contains(proof))
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
