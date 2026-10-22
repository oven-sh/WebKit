/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FunctionCheck {
    const Program& program;
    const FlowResults& results;
    BuildContext& build;
    FunctionID id;
    const Function& function;
    Vector<InternalError>& errors;

    void report(InternalErrorKind kind, BlockID block, ValueID value) const
    {
        errors.append(InternalError {
            .kind = kind,
            .function = id,
            .block = block,
            .value = value,
            .location = function.locationOf(value),
        });
    }

    bool isOutside(ValueID value, TypeID required) const
    {
        return required && !program.typeOf(function, value).isSubsetOf(program.at(required), program);
    }
};

} // anonymous namespace

static bool isValidMember(const Program& program, LayoutID member)
{
    if (member.index() >= program.layouts.size())
        return false;
    const Layout& layout = program.at(member);
    return !layout.isEnvironment() && layout.flow.allocation != AllocationState::NeverAllocated;
}

static bool hasValidMembers(const Program& program, LayoutID id)
{
    const Vector<LayoutID>& members = program.at(id).flow.members;
    if (std::ranges::adjacent_find(members, std::ranges::greater_equal { }) != members.end())
        return false;
    return std::ranges::all_of(members, [&](LayoutID member) { return isValidMember(program, member); });
}

static void checkOperands(const FunctionCheck& check, const TypingContext& typing, BlockID block, ValueID user)
{
    const Value& value = check.function.at(user);
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (check.isOutside(operand, requiredTypeOfOperand(typing, value, operandIndex)))
            check.report(InternalErrorKind::FlowWriterOutsideDeclaredType, block, user);
    });
}

static void checkReturn(const FunctionCheck& check, const TypingContext& typing, BlockID block, const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Return || !terminator.valueOperand())
        return;
    if (check.isOutside(terminator.valueOperand(), requiredTypeOfOperand(typing, terminator, OperandIndex::Return::value)))
        check.report(InternalErrorKind::FlowWriterOutsideDeclaredType, block, terminator.valueOperand());
}

static void checkWriters(const FunctionCheck& check, const TypingContext& typing, BlockID id, const Block& block)
{
    for (ValueID value : block.values)
        checkOperands(check, typing, id, value);
    checkReturn(check, typing, id, block.terminator);
}

static bool mayHaveFlowProof(const FunctionCheck& check)
{
    return check.results.hasResults()
        && check.build.isEnabled(FlowResultKind::NarrowedTypes, findFunctionAsRead(&check.program, check.id).index());
}

static void checkAssumedTailCasts(const FunctionCheck& check, const TypingContext& typing, BlockID block, const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return;
    ValueID call = terminator.callOperand();
    Type returned = check.program.typeOf(check.function, call);
    for (TailCast cast : terminator.casts()) {
        const Type& castType = check.program.at(cast.declared.type);
        if (cast.resolution == CastResolution::Assume && !returned.isSubsetOf(castType, check.program))
            check.report(InternalErrorKind::FlowRemovedCheckNotProven, block, call);
        returned = returned.castTo(castType, typing.types);
    }
}

static bool endsAtCastToNever(const FunctionCheck& check, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Unreachable || block.values.isEmpty())
        return false;
    const Value& last = check.function.at(block.values.last());
    return last.opcode == Opcode::AssumeType && check.program.at(last.type).isNever();
}

// Narrowing is switched on or off per position, so a function with narrowing off still casts what it stores into a narrowed position.
static BitVector findCastsIntoPositions(const FunctionCheck& check, const TypingContext& typing, const CFG& cfg)
{
    const Function& function = check.function;
    BitVector casts(function.values.size());
    for (BlockID id : cfg.reversePostOrder()) {
        if (endsAtCastToNever(check, function.at(id)))
            casts.quickSet(function.at(id).values.last().index());
        for (ValueID user : function.at(id).values) {
            const Value& value = function.at(user);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                const Value& cast = function.at(operand);
                if (cast.opcode == Opcode::AssumeType && cast.castType() == requiredTypeOfOperand(typing, value, operandIndex))
                    casts.quickSet(operand.index());
            });
        }
    }
    return casts;
}

static void checkAssumedTypes(const FunctionCheck& check, const TypingContext& typing, const CFG& cfg)
{
    BitVector castsIntoPositions;
    if (check.results.hasResults())
        castsIntoPositions = findCastsIntoPositions(check, typing, cfg);

    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = check.function.at(id);
        for (ValueID valueID : block.values) {
            const Value& value = check.function.at(valueID);
            if (value.opcode != Opcode::AssumeType || castsIntoPositions.get(valueID.index()))
                continue;
            if (check.isOutside(value.valueOperand(), value.castType()))
                check.report(InternalErrorKind::FlowRemovedCheckNotProven, id, valueID);
        }
        checkAssumedTailCasts(check, typing, id, block.terminator);
    }
}

static void checkFunction(const FunctionCheck& check)
{
    if (!check.function.hasBody())
        return;

    ScratchTypeArena types(check.program.types);
    TypingContext typing { check.program, check.function, types.arena(), TypeLevel::Proven };
    CFG cfg(check.function);
    if (check.results.hasResults()) {
        for (BlockID id : cfg.reversePostOrder())
            checkWriters(check, typing, id, check.function.at(id));
    }
    if (!mayHaveFlowProof(check))
        checkAssumedTypes(check, typing, cfg);
}

void validateFlowResults(const Program& program, const FlowResults& results, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<InternalError> errors;
    if (results.hasResults()) {
        errors = collectInParallel<InternalError>(loops, program.layouts.size(), [&](unsigned index, Vector<InternalError>& found) {
            if (!hasValidMembers(program, LayoutID(index)))
                found.append(InternalError { .kind = InternalErrorKind::FlowMembersInvalid, .layout = LayoutID(index) });
        });
    }

    std::optional<TypeInterningPhase> scratch;
    if (!program.types.hasOpenPhase())
        scratch.emplace(program.types, 0u, TypeInterningPhase::Scratch);
    errors.appendVector(collectInParallel<InternalError>(loops, program.functions.size(), [&](unsigned index, Vector<InternalError>& found) {
        checkFunction(FunctionCheck { program, results, build, FunctionID(index), program.functions[index], found });
    }));
    if (scratch)
        scratch->discard();

    build.diagnostics().append(WTF::move(errors));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
