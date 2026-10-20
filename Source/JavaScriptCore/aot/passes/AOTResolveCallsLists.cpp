/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinPristine.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSinkAllocations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool canCountArguments(const Function& function)
{
    return function.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter });
}

// The arm that passes arguments ends with another operation than the list call, which SiteExpansion returns from instead of leaving by it.
static bool isRequiredTailCall(const Function& function, const ValueSite& site)
{
    const Terminator& terminator = function.at(site.block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == site.value && terminator.requirement() == TailCallRequirement::Required;
}

// A function that cannot count its arguments sees no more of a list than its parameters name, and undefined past the end of the list either way.
// What it can see is whether the call happens: a list that does not fit the stack throws a RangeError in its place. A list no longer than the arguments a call can name takes no more of the stack than such a call.
PlanOrDecline<ListCallPlan> CallResolver::planListCall(const CallSite& site)
{
    // The call that stays for a list that is too long stands in a rare block.
    if (m_context.analyses.blockFrequencies().classOf(site.position.block) == BlockFrequency::Rare || isRequiredTailCall(m_function, site.position))
        return std::unexpected(CallDeclineReason::None);

    const Value& call = m_function.at(site.position.value);
    PristineQuery query { m_program, m_function, &m_context.analyses };
    PristineRequirement requirement { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
    if (pristineProofOf(query, call.listOperand(), requirement, site.position.point()) != PristineProof::Proven)
        return std::unexpected(CallDeclineReason::None);

    ValueID callee = site.opcode == Opcode::SuperConstructList ? call.parentOperand() : call.calleeOperand();
    CalleeCandidates candidates = calleeCandidates(m_program, m_flow, m_function, callee, call.callees(), call.accessSite());
    if (!candidates.isProven() || candidates.hasOtherCallees)
        return std::unexpected(CallDeclineReason::UnknownCallee);

    ListCallPlan plan;
    for (FunctionID function : candidates.proven) {
        const Function& target = m_program.at(function);
        if (canCountArguments(target))
            return std::unexpected(CallDeclineReason::None);
        plan.numberOfArguments = std::max(plan.numberOfArguments, target.numberOfPositionalParameters());
    }
    return plan;
}

void CallResolver::rewriteListCall(const CallSite& site, const ListCallPlan& plan)
{
    m_expansion.begin(site.position);
    ValueID list = m_expansion.original().listOperand();

    Value lengthRead = IRBuilder::make(Opcode::PropGet, { list });
    lengthRead.setKey(m_program.keyOf(BuiltinNames::length));
    ValueID length = m_expansion.append(IRBuilder::make(Opcode::ToLength, { m_expansion.append(WTF::move(lengthRead)) }));
    ValueID fits = m_expansion.append(IRBuilder::make(Opcode::LessEq, { length, m_builder.appendInteger(m_context.build.limit(LimitID::MaxReservedStackArguments)) }));
    IRBuilder::BranchBlocks branch = m_builder.emitBranch(fits, m_expansion.frequency(), BlockFrequency::Rare);

    m_builder.moveToEndOf(branch.notTaken);
    m_expansion.finishArm(m_expansion.appendOriginal());

    m_builder.moveToEndOf(branch.taken);
    Vector<ValueID, 8> arguments;
    for (uint32_t position = 0; position < plan.numberOfArguments; ++position) {
        Value read = IRBuilder::make(Opcode::ElemGet, { list, m_builder.appendInteger(position) });
        read.setKeyIsIntegral(true);
        arguments.append(m_expansion.append(WTF::move(read)));
    }
    Value call = m_expansion.original();
    convertListCall(call, arguments.span());
    if (FlagMask::isDemotedTailCall(call.opcode))
        call.setIsDemotedTailCall(m_expansion.isTailCall());
    m_expansion.finishArm(m_expansion.append(WTF::move(call)));
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
