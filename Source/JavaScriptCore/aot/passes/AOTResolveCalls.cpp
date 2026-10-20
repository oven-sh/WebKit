/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTResultRules.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <ranges>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet siteOpcodes {
    Opcode::Call,
    Opcode::CallList,
    Opcode::Construct,
    Opcode::ConstructList,
    Opcode::SuperConstruct,
    Opcode::SuperConstructList,
    Opcode::PropGet,
    Opcode::PropSet,
    Opcode::SuperGet,
    Opcode::SuperSet,
    Opcode::InstanceOf,
    Opcode::CallBuiltin,
};

static bool canSinkToCall(FunctionPassContext& context, std::span<const ValueSite> links, const ValueSite& call)
{
    for (const ValueSite& link : links) {
        if (link.block != call.block)
            return false;
    }

    const BlockValues& values = context.function.at(call.block).values;
    for (uint32_t index = links.front().index + 1; index < call.index; ++index) {
        ValueID id = values[index];
        bool isLink = std::ranges::any_of(links, [&](const ValueSite& link) {
            return link.value == id;
        });
        if (!isLink && context.analyses.effectsOf(id).normal.interferesWith(Effects::unknownCode()))
            return false;
    }
    return true;
}

MemberRead findMemberRead(FunctionPassContext& context, const ValueSite& call)
{
    static_assert(!OperandIndex::Call::callee, "findSingleUseCastChain starts at operand 0 of its user.");
    const Function& function = context.function;
    RELEASE_ASSERT(function.at(call.value).opcode == Opcode::Call);

    PropertyKeyID key = function.at(call.value).calleeKey();
    if (!key)
        return { };

    const UseLists& useLists = context.analyses.useLists();
    Vector<ValueID, 2> chain = findSingleUseCastChain(function, useLists, call.value);
    if (chain.isEmpty())
        return { };
    const Value& read = function.at(chain.first());
    if ((read.opcode != Opcode::PropGet && read.opcode != Opcode::SuperGet) || read.key() != key)
        return { };

    MemberRead memberRead;
    for (ValueID link : chain) {
        ValueLocation location = useLists.locationOf(link);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        memberRead.links.append(ValueSite { location.block, location.index, link });
    }
    memberRead.canSinkToCall = canSinkToCall(context, memberRead.links.span(), call);
    return memberRead;
}

CallArm createDirectCallArm(const Program& program, const FlowResults& flow, FunctionID closureFunction, CalleeSetID stamp)
{
    FunctionID callee = versionToEnter(program, flow, closureFunction, stamp);
    return {
        .kind = CallArmKind::DirectCall,
        .closureFunction = closureFunction,
        .callee = callee,
        .resultCheck = resultCheckOf(flow, callee),
    };
}

PassOutcome resolveCalls(FunctionPassContext& context)
{
    return CallResolver(context).run();
}

CallResolver::CallResolver(FunctionPassContext& context)
    : m_context(context)
    , m_program(context.program)
    , m_function(context.function)
    , m_flow(context.inputs.flow)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    , m_expansion(context, m_builder)
{
}

PassOutcome CallResolver::run()
{
    Vector<CallSite> sites = collectSites();
    Vector<PlannedCall> plans = planSites(sites.span());
    if (plans.isEmpty())
        return PassOutcome::Unchanged;

    for (const PlannedCall& planned : plans | std::views::reverse) {
        rewrite(planned);
        RELEASE_ASSERT(!m_expansion.isOpen() && !m_builder.currentBlock());
    }

    m_context.build.statistics().count(StatisticID::CallsResolved, m_numberOfResolvedSites);
    return PassOutcome::Changed;
}

Vector<CallSite> CallResolver::collectSites()
{
    Vector<CallSite> sites;
    BitVector isMemberRead(m_function.values.size());
    forEachValueSite(m_context.analyses, m_function, [&](const ValueSite& position) {
        const Value& value = m_function.at(position.value);
        if (!siteOpcodes.contains(value.opcode))
            return;

        CallSite site { position, value.opcode, { } };
        if (value.opcode == Opcode::Call && value.calleeKey()) {
            site.memberRead = findMemberRead(m_context, position);
            if (!site.memberRead.isEmpty())
                isMemberRead.quickSet(site.memberRead.read().value.index());
        }
        sites.append(WTF::move(site));
    });

    sites.removeAllMatching([&](const CallSite& site) {
        return isMemberRead.quickGet(site.position.value.index());
    });
    return sites;
}

Vector<PlannedCall> CallResolver::planSites(std::span<const CallSite> sites)
{
    Vector<PlannedCall> plans;
    for (const CallSite& site : sites) {
        PlanOrDecline<CallPlan> planned = plan(site);
        if (planned)
            plans.append(PlannedCall { site, WTF::move(*planned) });
        else
            reportDecline(site, planned.error());
    }
    return plans;
}

template<typename Plan>
static std::optional<CallPlan> tryPlan(PlanOrDecline<Plan>&& result, CallDeclineReason& decline)
{
    if (result)
        return CallPlan(WTF::move(*result));
    if (result.error() != CallDeclineReason::None)
        decline = result.error();
    return std::nullopt;
}

PlanOrDecline<CallPlan> CallResolver::plan(const CallSite& site)
{
    CallDeclineReason decline = CallDeclineReason::None;
    std::optional<CallPlan> found;
    auto tryNext = [&](auto&& result) {
        found = tryPlan(WTF::move(result), decline);
        return found.has_value();
    };

    switch (site.opcode) {
    case Opcode::Call:
        if (!site.memberRead.isEmpty() && (tryNext(planMemberCall(site)) || tryNext(planClassObjectMember(site)) || tryNext(planSuperMember(site))))
            break;
        tryNext(planCalleeDispatch(site));
        break;
    case Opcode::PropGet:
        if (!tryNext(planReceiverDispatch(site, MethodKind::Getter)))
            tryNext(planClassObjectMember(site));
        break;
    case Opcode::PropSet:
        if (!tryNext(planReceiverDispatch(site, MethodKind::Setter)))
            tryNext(planClassObjectMember(site));
        break;
    case Opcode::SuperGet:
    case Opcode::SuperSet:
        tryNext(planSuperMember(site));
        break;
    case Opcode::Construct:
    case Opcode::SuperConstruct:
        tryNext(planConstruct(site));
        break;
    case Opcode::SuperConstructList:
        if (!tryNext(planConstruct(site)))
            tryNext(planListCall(site));
        break;
    case Opcode::CallList:
    case Opcode::ConstructList:
        tryNext(planListCall(site));
        break;
    case Opcode::InstanceOf:
        tryNext(planInstanceOf(site));
        break;
    case Opcode::CallBuiltin:
        tryNext(planFunctionMethod(site));
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (found)
        return WTF::move(*found);
    return std::unexpected(decline);
}

void CallResolver::rewrite(const PlannedCall& planned)
{
    const CallSite& site = planned.site;
    WTF::switchOn(planned.plan,
        [&](const ReceiverDispatchPlan& plan) {
            rewriteReceiverDispatch(site, plan);
        },
        [&](const CalleeDispatchPlan& plan) {
            rewriteCalleeDispatch(site, plan);
        },
        [&](const HolderMemberPlan& plan) {
            rewriteHolderMember(site, plan);
        },
        [&](const ConstructPlan& plan) {
            rewriteConstruct(site, plan);
        },
        [&](const InstanceOfPlan& plan) {
            rewriteInstanceOf(site, plan);
        },
        [&](const ForwardedCallPlan& plan) {
            rewriteForwardedCall(site, plan);
        },
        [&](const BindPlan& plan) {
            rewriteBind(site, plan);
        },
        [&](const MethodPredictionPlan& plan) {
            rewriteMethodPrediction(site, plan);
        },
        [&](const ListCallPlan& plan) {
            rewriteListCall(site, plan);
        });
    ++m_numberOfResolvedSites;
}

void CallResolver::reportDecline(const CallSite& site, CallDeclineReason decline)
{
    if (decline == CallDeclineReason::None || decline == CallDeclineReason::NeedsTest)
        return;
    if (!m_context.build.options().collectsDeclines() || !isHot(heatClassOf(m_context, site.position.block)))
        return;
    LimitID limit = decline == CallDeclineReason::TooManyCallees ? LimitID::MaxDispatchCases : LimitID::None;
    m_context.addDecline(Wire::DeclineKind::HotCallNotResolved, site.position.value, decline, limit);
}

CallDeclineReason CallResolver::declineOfUnprovenCallee(const CalleeCandidates& candidates) const
{
    bool hasTestablePrediction = !candidates.predicted.isEmpty() && candidates.predicted.size() <= m_context.limit(LimitID::MaxDispatchCases);
    return hasTestablePrediction ? CallDeclineReason::NeedsTest : CallDeclineReason::UnknownCallee;
}

bool CallResolver::areSameObject(ValueID a, ValueID b) const
{
    return m_context.analyses.identities().areSameValue(a, b);
}

const Value* CallResolver::findMethodRead(const CallSite& site, Opcode readOpcode) const
{
    RELEASE_ASSERT(site.opcode == Opcode::Call && (readOpcode == Opcode::PropGet || readOpcode == Opcode::SuperGet));
    if (site.memberRead.isEmpty())
        return nullptr;
    const Value& read = m_function.at(site.memberRead.read().value);
    ValueID thisValue = m_function.at(site.position.value).operand(OperandIndex::Call::thisValue);
    if (read.opcode != readOpcode || !thisValue)
        return nullptr;
    ValueID receiver = read.operand(readOpcode == Opcode::SuperGet ? OperandIndex::SuperGet::receiver : OperandIndex::PropGet::object);
    return areSameObject(receiver, thisValue) ? &read : nullptr;
}

bool CallResolver::canRetireMemberRead(const CallSite& site, FunctionID callee) const
{
    Type calleeType = Type::forFunction(callee);
    return std::ranges::all_of(site.memberRead.links, [&](const ValueSite& link) {
        return typeOf(link.value).overlaps(calleeType, m_program);
    });
}

static Value makeObjectCoercibleCheck(FunctionPassContext& context, const Value& read, PropertyKeyID key)
{
    Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { read.operand(*receiverOf(read.opcode)) });
    check.setNullishAccess(NullishAccess::Read);
    const PropertyKey& name = context.program.at(key);
    if (name.kind == PropertyKeyKind::Text)
        check.setName(name.text);
    check.setOrigin(read.origin());
    check.type = resultTypeOf(context.program, context.function, check, context.types);
    check.setPrediction(predictionOf(context.program, context.function, check, context.types));
    return check;
}

void CallResolver::retireMemberRead(const CallSite& site, bool checksReceiver, PropertyKeyID key)
{
    RELEASE_ASSERT(m_expansion.isOpen() && !site.memberRead.isEmpty());

    Vector<Value, 2> copies;
    for (const ValueSite& link : site.memberRead.links)
        copies.append(m_function.at(link.value));
    m_expansion.addReplaced(copies.first());

    ValueID read = site.memberRead.read().value;
    if (checksReceiver)
        m_function.at(read) = makeObjectCoercibleCheck(m_context, copies.first(), key);
    else
        becomeUndefined(m_function, read);
    for (const ValueSite& cast : site.memberRead.links.span().subspan(1))
        becomeUndefined(m_function, cast.value);

    m_expansion.setOperationsBeforeOriginal(WTF::move(copies), OperandIndex::Call::callee);
    m_context.analyses.didChangeValues();
}

Vector<ValueID, 8> CallResolver::capturesOf(const CallArm& arm, ValueID closure, ValueID receiver)
{
    if (m_program.at(arm.closureFunction).captures.isEmpty())
        return { };
    if (arm.capturesReceiver)
        return Vector<ValueID, 8> { receiver };
    if (arm.closureCreation)
        return Vector<ValueID, 8>(m_function.at(arm.closureCreation).variadicOperands());
    return appendCaptures(m_builder, arm.closureFunction, closure);
}

ValueID CallResolver::appendCall(const CallArm& arm, ValueID thisValue, ValueID closure, std::span<const ValueID> arguments)
{
    RELEASE_ASSERT(m_expansion.isOpen() && arm.kind == CallArmKind::DirectCall);
    // An arm of a dispatch may be for the method of a class that the receiver is known not to have: `this.describe()` of a base constructor, inlined under `new Base()`, has an arm for the method of the subclass.
    // The conversion of the receiver says that the arm is never reached, and nothing may follow it. Only layouts are asked, whose sets are exact.
    const Function& callee = m_program.at(arm.callee);
    if (thisValue && callee.has(FunctionFlags::HasThis) && callee.thisType.type) {
        const Type& expected = m_program.at(callee.thisType.type);
        if (typeOf(thisValue).hasOnlyLayouts() && expected.hasOnlyLayouts() && !typeOf(thisValue).overlaps(expected, m_program))
            return convertTo(m_builder, thisValue, callee.thisType);
    }

    const Value& original = m_expansion.original();
    Vector<ValueID, 8> captures = capturesOf(arm, closure, thisValue);
    ValueID result = appendDirectCall(m_builder, {
        .callee = arm.callee,
        .captures = captures.span(),
        .thisValue = thisValue,
        .calleeValue = m_program.at(arm.callee).has(FunctionFlags::TakesCallee) ? closure : ValueID(),
        .arguments = arguments,
        .accessSite = original.accessSite(),
        .resultCheck = arm.resultCheck,
        .isNeverInlined = original.opcode == Opcode::Call && original.isNeverInlined(),
        .isDemotedTailCall = m_expansion.isTailCall(),
    });
    // The result of a required tail call is tested by the caller that leaves the cycle.
    if (m_expansion.isRequiredTailCall())
        return result;
    return appendResultCheck(m_builder, result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
