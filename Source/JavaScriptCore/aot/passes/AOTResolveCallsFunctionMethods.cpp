/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinPristine.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCreateBoundFunctions.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

template<typename Plan>
static PlanOrDecline<CallPlan> toCallPlan(PlanOrDecline<Plan>&& planned)
{
    if (!planned)
        return std::unexpected(planned.error());
    return CallPlan(WTF::move(*planned));
}

static unsigned numberOfBoundArguments(const BuiltinCall& bind)
{
    return bind.arguments.empty() ? 0 : static_cast<unsigned>(bind.arguments.size() - 1);
}

static unsigned numberOfCapturedArguments(const Function& target, unsigned numberOfBound)
{
    return std::min(numberOfBound, target.numberOfPositionalParameters());
}

static bool canBind(FunctionPassContext& context, const BuiltinCall& bind, FunctionID targetID, ValueID site)
{
    const Program& program = context.program;
    const Function& target = program.at(targetID);
    if (!canCreateBoundFunction(program, targetID))
        return false;

    unsigned capturedArgumentCount = numberOfCapturedArguments(target, numberOfBoundArguments(bind));
    TypingContext typing { program, context.function, context.types };
    auto isWithin = [&](TypeID type, const DeclaredType& bound) {
        return program.at(type).isSubsetOf(typing.typeOf(bound), program);
    };
    if (target.has(FunctionFlags::HasThis)) {
        TypeID boundThis = bind.arguments.empty() ? TypeTable::undefined : context.function.at(bind.arguments[0]).type;
        if (!isWithin(boundThis, target.thisType))
            return false;
    }
    for (unsigned index = 0; index < capturedArgumentCount; ++index) {
        if (!isWithin(context.function.at(bind.arguments[index + 1]).type, target.parameters[index]))
            return false;
        // `new` on the bound function is answered by the run time, which hands the bound arguments on as values.
        if (target.constructorForm() != ConstructorForm::NotConstructor && !holdsCells(declaredRepresentationOf(program, target.parameters[index])))
            return false;
    }
    return !context.exceeds(LimitID::MaxCaptures, target.captures.size() + numberOfBoundThisAndTargetCaptures + capturedArgumentCount, site);
}

// A closure of a bound function answers with the name and the length its target was written with.
static bool findsMembersAsCreated(const Program& program, const FlowResults& flow, const BuiltinParameter& receiver, FunctionID target)
{
    Vector<PropertyKeyID, 2> keys(receiver.namedMembers.size(), [&](size_t index) {
        return program.keyOf(receiver.namedMembers[index]);
    });
    return flow.areClosuresAsCreated(target, keys.span());
}

namespace {

enum class PristineTestInFront : uint8_t { None, Passed, Failed };

} // anonymous namespace

// The test of the function at the end of the only block that leads to the call, if nothing has run since that could change the function.
static PristineTestInFront findPristineTestInFrontOf(FunctionPassContext& context, const ValueSite& site, ValueID receiver)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    std::span<const BlockID> predecessors = analyses.cfg().predecessors(site.block);
    if (predecessors.size() != 1)
        return PristineTestInFront::None;
    const Block& tests = function.at(predecessors[0]);
    if (tests.terminator.kind != TerminatorKind::Branch || tests.terminator.taken().block == tests.terminator.notTaken().block)
        return PristineTestInFront::None;
    ValueID testID = tests.terminator.conditionOperand();
    const Value& test = function.at(testID);
    if (test.opcode != Opcode::IsPristine || test.builtinClass() != BuiltinClasses::Function || !test.aspects().contains(PristineAspects::Names) || analyses.identityOf(test.operand(0)) != analyses.identityOf(receiver))
        return PristineTestInFront::None;
    // The engine binds what has failed the test, whatever has run since.
    if (tests.terminator.notTaken().block == site.block)
        return PristineTestInFront::Failed;
    auto isPure = [&](ValueID id) {
        return analyses.hasNoEffects(id);
    };
    size_t indexOfTest = tests.values.find(testID);
    if (indexOfTest == notFound || !std::ranges::all_of(tests.values.span().subspan(indexOfTest + 1), isPure))
        return PristineTestInFront::None;
    return std::ranges::all_of(function.at(site.block).values.span().first(site.index), isPure) ? PristineTestInFront::Passed : PristineTestInFront::None;
}

PlanOrDecline<CallPlan> CallResolver::planFunctionMethod(const CallSite& site)
{
    const Value& value = m_function.at(site.position.value);
    if (value.builtinCallForm() != BuiltinCallForm::Whole)
        return std::unexpected(CallDeclineReason::None);

    const BuiltinOverload& overload = m_program.builtins().at(value.overload());
    if (overload.builtin == Builtins::FunctionPrototypeBind)
        return toCallPlan(planBind(site, overload));
    return toCallPlan(planForwardedCall(site));
}

PlanOrDecline<ForwardedCallPlan> CallResolver::planForwardedCall(const CallSite& site)
{
    if (!typeOf(site.position.value).isAnything())
        return std::unexpected(CallDeclineReason::None);
    std::optional<ForwardedCallPlan> plan = Typed::planForwardedCall(m_program, m_function, builtinCallOf(m_program, m_function.at(site.position.value)));
    if (!plan || (plan->list && !isAlwaysArray(typeOf(plan->list))))
        return std::unexpected(CallDeclineReason::None);
    return *plan;
}

void CallResolver::rewriteForwardedCall(const CallSite& site, const ForwardedCallPlan& plan)
{
    becomeForwardedCall(m_function.edit(site.position.value), plan);
    m_context.analyses.didChangeValues();
}

PlanOrDecline<BindPlan> CallResolver::planBind(const CallSite& site, const BuiltinOverload& overload)
{
    BuiltinCall bind = builtinCallOf(m_program, m_function.at(site.position.value));
    if (!bind.receiver)
        return std::unexpected(CallDeclineReason::None);

    CalleeCandidates candidates = calleeCandidates(m_program, m_function, bind.receiver);
    if (!candidates.isProven() || candidates.proven.size() != 1 || candidates.hasOtherCallees || candidates.mayBeNullish)
        return std::unexpected(candidates.expected.isEmpty() ? CallDeclineReason::UnknownCallee : CallDeclineReason::NeedsTest);

    FunctionID targetID = candidates.proven[0];
    if (!canBind(m_context, bind, targetID, site.position.value))
        return std::unexpected(CallDeclineReason::CalleeNotCallable);
    bool testsThatClosureIsAsCreated = false;
    if (!findsMembersAsCreated(m_program, m_flow, *overload.receiver, targetID)) {
        // A bound function takes its name, its length and its prototype at the bind. A closure that has passed the test in front of the call has them as it was created with them.
        // The closure of a bound function is created with another name than its function is written with.
        if (m_program.at(targetID).role == FunctionRole::BoundFunction)
            return std::unexpected(CallDeclineReason::UnknownMember);
        PristineQuery query { m_program, m_function, &m_context.analyses };
        PristineRequirement names { BuiltinClasses::Function, PristineAspects::Names };
        if (pristineProofOf(query, bind.receiver, names, site.position.point()) != PristineProof::Proven) {
            switch (findPristineTestInFrontOf(m_context, site.position, bind.receiver)) {
            case PristineTestInFront::Passed:
                break;
            case PristineTestInFront::Failed:
                return std::unexpected(CallDeclineReason::UnknownMember);
            case PristineTestInFront::None:
                // The test that stood where `bind` was looked up may have met its other side again in front of the call.
                if (m_context.analyses.blockFrequencies().classOf(site.position.block) == BlockFrequency::Rare)
                    return std::unexpected(CallDeclineReason::UnknownMember);
                testsThatClosureIsAsCreated = true;
                break;
            }
        }
    }

    unsigned numberOfBound = numberOfBoundArguments(bind);
    FunctionID boundFunction = m_context.derivedFunction(derivedFunctionKey(FunctionRole::BoundFunction, targetID, numberOfBound));
    const Function& target = m_program.at(targetID);
    const Function& bound = m_program.at(boundFunction);
    const CreatorRecord::BoundFunction& record = bound.creator.as<CreatorRecord::BoundFunction>();
    bool capturesThis = target.has(FunctionFlags::HasThis);
    size_t numberOfCaptures = target.captures.size() + capturesThis + record.numberOfCapturedArguments + record.capturesTargetClosure;
    if (record.numberOfCapturedArguments > numberOfBound || numberOfCaptures != bound.captures.size())
        return std::unexpected(CallDeclineReason::None);

    BindPlan plan;
    plan.arm = createCalleeArm(candidates, targetID);
    plan.closure = candidates.value;
    plan.boundFunction = boundFunction;
    plan.numberOfCapturedArguments = record.numberOfCapturedArguments;
    plan.capturesThis = capturesThis;
    plan.capturesTargetClosure = record.capturesTargetClosure;
    plan.testsThatClosureIsAsCreated = testsThatClosureIsAsCreated;
    return plan;
}

void CallResolver::rewriteBind(const CallSite& site, const BindPlan& plan)
{
    m_expansion.begin(site.position);
    if (plan.testsThatClosureIsAsCreated) {
        Value test = IRBuilder::make(Opcode::IsPristine, { plan.closure });
        test.setBuiltinClass(BuiltinClasses::Function);
        test.setAspects(PristineAspects::Names);
        IRBuilder::BranchBlocks branch = m_builder.emitBranch(m_expansion.append(WTF::move(test)), m_expansion.frequency(), BlockFrequency::Unlikely);
        m_builder.moveToEndOf(branch.notTaken);
        m_expansion.finishArm(m_expansion.appendOriginal());
        m_builder.moveToEndOf(branch.taken);
    }
    std::span<const ValueID> arguments = m_expansion.original().argumentsOperands();

    appendCalleeValidation(m_builder, plan.closure);
    Vector<ValueID, 8> captures = capturesOf(plan.arm, plan.closure, { });
    if (plan.capturesThis)
        captures.append(arguments.empty() ? m_builder.appendUndefined() : arguments[0]);
    for (uint32_t index = 0; index < plan.numberOfCapturedArguments; ++index)
        captures.append(arguments[index + 1]);
    if (plan.capturesTargetClosure)
        captures.append(plan.closure);

    Value creation = IRBuilder::make(Opcode::NewClosure);
    creation.setFunction(plan.boundFunction);
    creation.setCapturesOperands(captures.span());
    m_expansion.finishArm(m_expansion.append(WTF::move(creation)));
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
