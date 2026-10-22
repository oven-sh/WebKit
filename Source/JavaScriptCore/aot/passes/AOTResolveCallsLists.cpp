/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinPristine.h"
#include "AOTExpandIteration.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTSinkAllocations.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
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
    ValueID callee = site.opcode == Opcode::SuperConstructList ? call.parentOperand() : call.calleeOperand();
    CalleeCandidates candidates = calleeCandidates(m_program, m_function, callee);
    bool areCalleesKnown = candidates.isProven() && !candidates.hasOtherCallees;

    ListCallPlan plan;
    if (areCalleesKnown) {
        for (FunctionID function : candidates.proven) {
            const Function& target = m_program.at(function);
            if (canCountArguments(target))
                return std::unexpected(CallDeclineReason::None);
            plan.numberOfArguments = std::max(plan.numberOfArguments, target.numberOfPositionalParameters());
        }
    } else if (!reportsDeclinesIn(site.position.block))
        return std::unexpected(CallDeclineReason::None);

    // Where the callees are not known, only a report reads the answer.
    PristineQuery query { m_program, m_function, &m_context.analyses };
    PristineRequirement requirement { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
    if (pristineProofOf(query, call.listOperand(), requirement, site.position.point()) != PristineProof::Proven)
        return std::unexpected(CallDeclineReason::None);
    if (!areCalleesKnown)
        return std::unexpected(CallDeclineReason::UnknownCallee);
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

static bool isGenericSpread(const Program& program, const Value& value, uint32_t operandIndex)
{
    if (value.opcode != Opcode::CallDirect || operandIndex)
        return false;
    const Function& callee = program.at(value.function());
    return isOutlinedIteration(callee) && callee.derivation.node == static_cast<uint32_t>(OutlinedIteration::AppendIterable);
}

// Whoever leaves the first point arrives at the second, and nobody else does. With a list of the operations that go away: what else stands between the points could as well run in front of code of the program.
bool CallResolver::leadsInLineTo(ProgramPoint from, ProgramPoint to, std::optional<std::span<const ValueID>> retired)
{
    constexpr unsigned maxBlocksInLine = 8;
    const CFG& cfg = m_context.analyses.cfg();
    for (unsigned step = 0; step < maxBlocksInLine; ++step) {
        const Block& block = m_function.at(from.block);
        uint32_t end = from.block == to.block ? to.index : block.values.size();
        if (from.index > end)
            return false;
        for (uint32_t index = from.index; retired && index < end; ++index) {
            ValueID id = block.values[index];
            if (!std::ranges::contains(*retired, id) && m_context.analyses.effectsOf(id).onAnyEdge().interferesWith(Effects::unknownCode()))
                return false;
        }
        if (from.block == to.block)
            return true;
        if (block.terminator.kind != TerminatorKind::Jump || !block.terminator.targets[0].arguments.isEmpty())
            return false;
        from = ProgramPoint::atHeadOf(block.terminator.targets[0].block);
        if (from.block != to.block && cfg.incomingEdges(from.block).size() != 1)
            return false;
    }
    return false;
}

// Every name of the literal, and what is done with each: the rest array is appended once, by one operation or by one of two that a test of the array chooses between, and then the call takes the list.
bool CallResolver::findSpreadOfRest(const CallSite& site, RestHandoverPlan& plan)
{
    const UseLists& useLists = m_context.analyses.useLists();
    const CFG& cfg = m_context.analyses.cfg();
    Vector<ValueID, 2> appends;
    Vector<ValueID, 4> names { plan.list };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : useLists.usesOf(names[index])) {
            if (use.kind != Use::Kind::Operand)
                return false;
            if (use.user == site.position.value) {
                if (use.operandIndex != OperandIndex::CallList::list)
                    return false;
                continue;
            }
            const Value& user = m_function.at(use.user);
            bool appendsToList = (user.opcode == Opcode::AppendSpread && use.operandIndex == OperandIndex::AppendSpread::array)
                || (user.opcode == Opcode::AppendElements && use.operandIndex == OperandIndex::AppendElements::array)
                || isGenericSpread(m_program, user, use.operandIndex);
            bool isOtherName = isCast(user.opcode) && user.opcode != Opcode::Check;
            if (!appendsToList && !isOtherName && user.opcode != Opcode::ValidateType)
                return false;
            if (appendsToList)
                appends.append(use.user);
            if (isOtherName)
                names.append(use.user);
            plan.operationsOnList.append(use.user);
        }
    }

    auto pointOf = [&](ValueID id) {
        ValueLocation location = useLists.locationOf(id);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        return ProgramPoint { location.block, location.index };
    };
    auto sourceOf = [&](ValueID append) {
        return sameObjectThroughCasts(m_function, m_function.at(append).operand(1));
    };
    static_assert(OperandIndex::AppendSpread::iterable == 1 && OperandIndex::AppendElements::source == 1);
    std::span<const ValueID> retired = plan.operationsOnList.span();

    if (appends.size() == 1) {
        if (m_function.at(appends[0]).opcode == Opcode::CallDirect)
            return false;
        plan.append = appends[0];
        plan.rest = sourceOf(appends[0]);
        plan.spreadName = m_function.at(appends[0]).operand(1);
        return leadsInLineTo(pointOf(plan.list), pointOf(appends[0]), std::nullopt) && leadsInLineTo(pointOf(appends[0]), site.position.point(), retired);
    }
    if (appends.size() != 2)
        return false;

    if (m_function.at(appends[0]).opcode != Opcode::AppendElements)
        std::swap(appends[0], appends[1]);
    if (m_function.at(appends[0]).opcode != Opcode::AppendElements || m_function.at(appends[1]).opcode != Opcode::CallDirect)
        return false;
    plan.rest = sourceOf(appends[0]);
    if (sourceOf(appends[1]) != plan.rest)
        return false;
    ProgramPoint whereArrayPasses = pointOf(appends[0]);
    ProgramPoint whereArrayFails = pointOf(appends[1]);
    std::optional<Edge> passing = cfg.singleIncomingEdge(whereArrayPasses.block);
    std::optional<Edge> failing = cfg.singleIncomingEdge(whereArrayFails.block);
    if (!passing || !failing || passing->from != failing->from || passing->isHandlerEdge() || failing->isHandlerEdge() || whereArrayPasses.block == whereArrayFails.block)
        return false;
    const Block& tests = m_function.at(passing->from);
    if (tests.terminator.kind != TerminatorKind::Branch || tests.terminator.targets[0].block != whereArrayPasses.block || tests.terminator.targets[1].block != whereArrayFails.block)
        return false;
    const Value& test = m_function.at(tests.terminator.conditionOperand());
    if (test.opcode != Opcode::IsPristine || sameObjectThroughCasts(m_function, test.valueOperand()) != plan.rest)
        return false;
    plan.spreadName = test.valueOperand();
    const Terminator& afterPassing = m_function.at(whereArrayPasses.block).terminator;
    const Terminator& afterFailing = m_function.at(whereArrayFails.block).terminator;
    if (afterPassing.kind != TerminatorKind::Jump || afterFailing.kind != TerminatorKind::Jump || afterPassing.targets[0].block != afterFailing.targets[0].block)
        return false;
    BlockID join = afterPassing.targets[0].block;
    if (cfg.incomingEdges(join).size() != 2 || !afterPassing.targets[0].arguments.isEmpty() || !afterFailing.targets[0].arguments.isEmpty())
        return false;
    ProgramPoint endOfTests { passing->from, static_cast<uint32_t>(tests.values.size()) };
    auto endOf = [&](BlockID block) {
        return ProgramPoint { block, static_cast<uint32_t>(m_function.at(block).values.size()) };
    };
    return leadsInLineTo(pointOf(plan.list), endOfTests, std::nullopt)
        && leadsInLineTo(ProgramPoint::atHeadOf(whereArrayPasses.block), endOf(whereArrayPasses.block), retired)
        && leadsInLineTo(ProgramPoint::atHeadOf(whereArrayFails.block), endOf(whereArrayFails.block), retired)
        && leadsInLineTo(ProgramPoint::atHeadOf(join), site.position.point(), retired);
}

// Made for this call of this function, by its generic entry or by whoever calls it directly, and known to nobody else when the function begins.
bool CallResolver::isRestArrayOfThisFrame(ValueID id) const
{
    if (m_function.at(id).opcode == Opcode::CreateRest)
        return true;
    if (!m_function.has(FunctionFlags::HasRestParameter))
        return false;
    const Block& entry = m_function.at(entryBlock);
    unsigned index = m_function.firstParameterIndex() + m_function.numberOfPositionalParameters();
    return index < entry.parameters.size() && entry.parameters[index] == id;
}

// Nothing that can run before the call does more than read the array, and nothing that can run behind it looks at the array again. A test of its header tells nobody anything.
bool CallResolver::isSeenByNobodyElse(const CallSite& site, const RestHandoverPlan& plan)
{
    const UseLists& useLists = m_context.analyses.useLists();
    const CFG& cfg = m_context.analyses.cfg();
    BitVector noBarrier(cfg.numNodes());
    BlockID siteBlock = site.position.block;
    for (BlockID successor : cfg.successors(siteBlock)) {
        if (cfg.blocksReachableFrom(successor, noBarrier).get(siteBlock.index()))
            return false;
    }
    BitVector behindCall = cfg.blocksReachableFrom(siteBlock, noBarrier);

    Vector<ValueID, 4> names { plan.rest };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : useLists.usesOf(names[index])) {
            if (use.kind == Use::Kind::BlockSubject)
                continue;
            if (use.kind == Use::Kind::Operand) {
                if (plan.operationsOnList.contains(use.user))
                    continue;
                const Value& user = m_function.at(use.user);
                if (isCast(user.opcode) && user.opcode != Opcode::Check) {
                    names.append(use.user);
                    continue;
                }
                if (user.operandKind(use.operandIndex) == OperandKind::Guard || user.opcode == Opcode::AllGuards || user.opcode == Opcode::IsPristine || user.opcode == Opcode::HasArrayStorage || user.opcode == Opcode::ValidateType)
                    continue;
                bool readsLength = user.opcode == Opcode::PropGet && user.key() == m_program.keyOf(BuiltinNames::length);
                bool runsBehindCall = use.block == siteBlock ? useLists.locationOf(use.user).index > site.position.index : behindCall.get(use.block.index());
                if (!runsBehindCall && (readsLength || readsArrayWhereItLies(m_program, m_function, user, use.operandIndex)))
                    continue;
            }
            if (behindCall.get(use.block.index()) || cfg.blocksReachableFrom(use.block, noBarrier).get(siteBlock.index()))
                return false;
        }
    }
    return true;
}

// An array that is only read may lie in the frame of whoever made it, and f may be one that keeps its rest array. Then the array is a cell in every call of this function.
// That is the one cell f needs anyway where every likely way through this function comes to the call. Where another way leaves without it, the list stays: a copy on that way alone.
bool CallResolver::liesOnEveryLikelyWay(BlockID siteBlock)
{
    const CFG& cfg = m_context.analyses.cfg();
    const BlockFrequencies& frequencies = m_context.analyses.blockFrequencies();
    if (siteBlock == entryBlock)
        return true;
    BitVector barrier(cfg.numNodes());
    for (BlockID block : cfg.reversePostOrder()) {
        if (block == siteBlock || frequencies.classOf(block) != BlockFrequency::Normal)
            barrier.set(block.index());
    }
    for (size_t index : cfg.blocksReachableFrom(entryBlock, barrier)) {
        if (cfg.successors(BlockID(index)).empty())
            return false;
    }
    return true;
}

// f(a, b, ...rest), where f is one known function that takes as many values by name as stand in front of the spread and the others in a rest parameter, cannot count its arguments in another way,
// and `rest` is the rest array of this frame, which nobody else has seen. The list would be made of a, b and the elements and spread over the stack, and f's entry would make a third array of what it finds there.
// That array would hold what `rest` holds, and nobody could tell them apart: `rest` is handed over in its place, and f may do with it whatever it likes.
// An array that is not as it was made, or too long for the stack (see planListCall()), takes the way it took before.
PlanOrDecline<RestHandoverPlan> CallResolver::planRestHandover(const CallSite& site)
{
    // The call that stays stands in a rare block.
    if (m_context.analyses.blockFrequencies().classOf(site.position.block) == BlockFrequency::Rare)
        return std::unexpected(CallDeclineReason::None);

    const Value& call = m_function.at(site.position.value);
    CalleeCandidates candidates = calleeCandidates(m_program, m_function, call.calleeOperand());
    if (!candidates.isProven() || candidates.hasOtherCallees || candidates.mayBeNullish || candidates.proven.size() != 1)
        return std::unexpected(CallDeclineReason::None);
    FunctionID callee = candidates.proven[0];
    const Function& target = m_program.at(callee);
    if (!target.has(FunctionFlags::HasRestParameter) || target.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return std::unexpected(CallDeclineReason::None);
    if (!mayBeCalledDirectly(m_program, callee) || !canPassRestArguments(m_program, target))
        return std::unexpected(CallDeclineReason::None);

    RestHandoverPlan plan;
    plan.list = sameObjectThroughCasts(m_function, call.listOperand());
    const Value& list = m_function.at(plan.list);
    if (list.opcode != Opcode::NewArray || !list.holes().empty() || list.capacityOperand() || list.elementsOperands().size() != target.numberOfPositionalParameters())
        return std::unexpected(CallDeclineReason::None);
    if (!findSpreadOfRest(site, plan) || !isRestArrayOfThisFrame(plan.rest) || !isSeenByNobodyElse(site, plan) || !liesOnEveryLikelyWay(site.position.block))
        return std::unexpected(CallDeclineReason::None);

    // What f knows of its rest array it knows of this one.
    TypeID required = TypingContext { m_program, m_function, m_context.types }.typeIDOf(target.parameters.last());
    if (!typeOf(plan.spreadName).isSubsetOf(m_program.at(required), m_program))
        return std::unexpected(CallDeclineReason::None);
    // And the other way round: f does with its rest array what it likes, and where f is taken in, its code finds the array under the type that it has here.
    // What is proven of this array, its contents, its storage, that it has no hole, was proven of an array that f never sees.
    if (!m_program.at(required).isSubsetOf(typeOf(plan.spreadName), m_program))
        return std::unexpected(CallDeclineReason::None);

    PristineQuery query { m_program, m_function, &m_context.analyses };
    PristineRequirement requirement { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
    plan.isRestProvenPristine = pristineProofOf(query, plan.spreadName, requirement, site.position.point()) == PristineProof::Proven;
    if (!plan.isRestProvenPristine && pristineTestOf(m_program.builtins(), BuiltinClasses::Array) != PristineTest::MayPass)
        return std::unexpected(CallDeclineReason::None);

    plan.callee = candidates.value;
    plan.proof = candidates.proof;
    plan.arm = createCalleeArm(candidates, callee);
    return plan;
}

void CallResolver::rewriteRestHandover(const CallSite& site, const RestHandoverPlan& plan)
{
    m_expansion.begin(site.position);
    ValueID thisValue = m_expansion.original().operand(OperandIndex::CallList::thisValue);
    Value list = m_function.at(plan.list);
    Vector<ValueID, 8> arguments(list.elementsOperands());
    Value append = plan.append ? m_function.at(plan.append) : IRBuilder::make(Opcode::AppendSpread, { plan.list, plan.spreadName });

    m_expansion.addReplaced(list);
    for (ValueID operation : plan.operationsOnList) {
        m_expansion.addReplaced(m_function.at(operation));
        becomeUndefined(m_function, operation);
    }
    becomeUndefined(m_function, plan.list);
    m_context.analyses.didChangeValues();

    BlockID asBefore = m_builder.addBlock(BlockFrequency::Rare);
    auto goOnIf = [&](ValueID condition) {
        BlockID next = m_builder.addBlock(m_expansion.frequency());
        m_builder.emitBranch(condition, Target { next, { }, { } }, Target { asBefore, { }, { } });
        m_builder.moveToEndOf(next);
    };
    if (!plan.isRestProvenPristine)
        goOnIf(m_builder.append(makePristineTest(plan.spreadName, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names })));
    Value lengthRead = IRBuilder::make(Opcode::PropGet, { plan.spreadName });
    lengthRead.setKey(m_program.keyOf(BuiltinNames::length));
    ValueID length = m_expansion.append(IRBuilder::make(Opcode::ToLength, { m_expansion.append(WTF::move(lengthRead)) }));
    uint64_t limit = m_context.build.limit(LimitID::MaxReservedStackArguments);
    goOnIf(m_expansion.append(IRBuilder::make(Opcode::LessEq, { length, m_builder.appendInteger(limit - std::min<uint64_t>(limit, arguments.size())) })));

    if (plan.proof == CalleeProof::Type)
        appendCalleeValidation(m_builder, plan.callee);
    Vector<ValueID, 8> captures = capturesOf(plan.arm, plan.callee, thisValue);
    m_expansion.finishArm(appendDirectCall(m_builder, {
        .callee = plan.arm.callee,
        .captures = captures.span(),
        .thisValue = thisValue,
        .calleeValue = m_program.at(plan.arm.callee).has(FunctionFlags::TakesCallee) ? plan.callee : ValueID(),
        .arguments = arguments.span(),
        .restArray = plan.spreadName,
        .accessSite = m_expansion.original().accessSite(),
        .isDemotedTailCall = m_expansion.isTailCall(),
    }));

    m_builder.moveToEndOf(asBefore);
    ValueID listAsBefore = m_builder.append(WTF::move(list));
    append.setOperand(0, listAsBefore);
    m_builder.append(WTF::move(append));
    m_expansion.finishArm(m_expansion.appendOriginal({ OperandIndex::CallList::list, listAsBefore }));
    m_expansion.end();
    m_context.build.statistics().count(StatisticID::RestArraysHandedOver);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
