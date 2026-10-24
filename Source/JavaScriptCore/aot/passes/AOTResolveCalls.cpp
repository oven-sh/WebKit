/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTPropertyAccessCache.h"
#include "AOTResultRules.h"
#include "AOTThreshold.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet validationsOfValues { Opcode::ValidateType, Opcode::ValidateCallee };

static bool canSinkToCall(FunctionPassContext& context, std::span<const ValueSite> links, const ValueSite& call)
{
    for (const ValueSite& link : links) {
        if (link.block != call.block)
            return false;
    }

    // A read that is proven to find methods walks the chain and runs nobody's code. Any other may run a getter.
    Effects effectsOfRead = context.analyses.effectsOf(links.front().value).normal;
    const BlockValues& values = context.function.at(call.block).values;
    for (uint32_t index = links.front().index + 1; index < call.index; ++index) {
        ValueID id = values[index];
        bool isLink = std::ranges::any_of(links, [&](const ValueSite& link) {
            return link.value == id;
        });
        if (!isLink && !validationsOfValues.contains(context.function.at(id).opcode) && context.analyses.effectsOf(id).normal.interferesWith(effectsOfRead))
            return false;
    }
    return true;
}

// findSingleUseCastChain(), but for the operations of a validating build: they use a value and keep nothing of it, so that such a build resolves the calls that any other build resolves.
static Vector<ValueID, 2> findCastChainUsedByCallAlone(const Function& function, const UseLists& useLists, ValueID user, Vector<ValueID, 2>& validations)
{
    Vector<ValueID, 2> chain;
    for (ValueID id : CastChain(function, function.at(user).operand(0))) {
        bool isUsedByUser = false;
        for (const Use& use : useLists.usesOf(id)) {
            if (use.kind == Use::Kind::Operand && validationsOfValues.contains(function.at(use.user).opcode)) {
                validations.append(use.user);
                continue;
            }
            if (isUsedByUser || use.kind != Use::Kind::Operand || use.user != user || use.operandIndex)
                return { };
            isUsedByUser = true;
        }
        if (!isUsedByUser)
            return { };
        chain.append(id);
        user = id;
    }
    chain.reverse();
    return chain;
}

// All but whether the read can sink to the call, which is asked of the effects of what stands between the two.
static MemberRead findLinksOfMemberRead(FunctionPassContext& context, const ValueSite& call)
{
    static_assert(!OperandIndex::Call::callee, "findCastChainUsedByCallAlone starts at operand 0 of its user.");
    const Function& function = context.function;
    RELEASE_ASSERT(function.at(call.value).opcode == Opcode::Call);

    const Value& source = function.at(CastChain(function, function.at(call.value).operand(OperandIndex::Call::callee)).last());
    // A call names the key under which its callee was read from the receiver. `super.run()` reads above the home object, so the call names none and the read says it.
    PropertyKeyID key = source.opcode == Opcode::SuperGet ? source.key() : function.at(call.value).calleeKey();
    if (!key)
        return { };
    if ((source.opcode != Opcode::PropGet && source.opcode != Opcode::SuperGet) || source.key() != key)
        return { };

    const UseLists& useLists = context.analyses.useLists();
    MemberRead memberRead;
    Vector<ValueID, 2> chain = findCastChainUsedByCallAlone(function, useLists, call.value, memberRead.validations);
    if (chain.isEmpty())
        return { };
    const Value& read = function.at(chain.first());
    if ((read.opcode != Opcode::PropGet && read.opcode != Opcode::SuperGet) || read.key() != key)
        return { };

    for (ValueID link : chain) {
        ValueLocation location = useLists.locationOf(link);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        memberRead.links.append(ValueSite { location.block, location.index, link });
    }
    return memberRead;
}

MemberRead findMemberRead(FunctionPassContext& context, const ValueSite& call)
{
    MemberRead memberRead = findLinksOfMemberRead(context, call);
    if (!memberRead.isEmpty())
        memberRead.canSinkToCall = canSinkToCall(context, memberRead.links.span(), call);
    return memberRead;
}

CallArm createDirectCallArm(FunctionID closureFunction)
{
    return {
        .kind = CallArmKind::DirectCall,
        .closureFunction = closureFunction,
        .callee = closureFunction,
    };
}

PassOutcome resolveCalls(FunctionPassContext& context)
{
    return CallResolver(context, CallResolver::Resolution::Everything).run();
}

CallResolver::CallResolver(FunctionPassContext& context, Resolution resolution)
    : m_context(context)
    , m_program(context.program)
    , m_function(context.function)
    , m_flow(context.inputs.flow)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    , m_expansion(context, m_builder)
    , m_resolution(resolution)
{
}

PassOutcome CallResolver::run()
{
    Vector<CallSite> sites = collectSites();
    Vector<PlannedCall> plans = planSites(sites.mutableSpan());
    if (plans.isEmpty())
        return PassOutcome::Unchanged;

    for (const PlannedCall& planned : plans | std::views::reverse) {
        // Whoever cuts a block finds in it what was put behind the cut.
        if (m_builder.hasInsertions() && !std::holds_alternative<OnlyCalleePlan>(planned.plan))
            m_builder.applyInsertions();
        rewrite(planned);
        RELEASE_ASSERT(!m_expansion.isOpen() && !m_builder.currentBlock());
    }
    if (m_builder.hasInsertions())
        m_builder.applyInsertions();

    m_context.build.statistics().count(StatisticID::CallsResolved, m_numberOfResolvedSites);
    // Each rewrite tells the analyses of its edits as it makes them.
    m_context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

// False only where plan() finds nothing and has nothing to report. It asks what the planners of the operation ask first, of the operation and of the types of its operands alone.
bool CallResolver::mayHavePlan(const Value& value) const
{
    switch (value.opcode) {
    case Opcode::PropGet:
    case Opcode::PropSet: {
        const Type& receiver = typeOf(value.objectOperand());
        if (receiver.mayBeUndefinedOrNull())
            return false;
        if (!value.isFreeName() && receiver.layouts && !hasMembersWithoutLayout(m_program, receiver))
            return true;
        return !!classOfClassObject(m_program, receiver);
    }
    case Opcode::CallBuiltin:
        if (value.builtinCallForm() != BuiltinCallForm::Whole)
            return false;
        return m_program.at(value.type).isAnything() || m_program.builtins().at(value.overload()).builtin == Builtins::FunctionPrototypeBind;
    default:
        return true;
    }
}

Vector<CallSite> CallResolver::collectSites()
{
    Vector<CallSite> sites;
    BitVector isMemberRead(m_function.values.size());
    const bool checksScreen = m_context.build.options().verifiesAfterEveryPass;
    m_context.forEachSiteIn(m_resolution == Resolution::MethodTable ? SiteList::Calls : SiteList::CallSites, [&](const ValueSite& position) {
        if (m_resolution == Resolution::MethodTable && !mayCallThroughTable(position))
            return;

        const Value& value = m_function.at(position.value);
        if (!mayHavePlan(value)) {
            if (checksScreen) [[unlikely]] {
                PlanOrDecline<CallPlan> planned = plan(CallSite { position, value.opcode, { } });
                RELEASE_ASSERT(!planned && (planned.error() == CallDeclineReason::None || planned.error() == CallDeclineReason::NeedsTest));
            }
            return;
        }
        CallSite site { position, value.opcode, { } };
        if (value.opcode == Opcode::Call) {
            site.memberRead = findLinksOfMemberRead(m_context, position);
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

// All that plan() takes from a call, a read or a store by name and its function, until it asks about the place of the site. None for the other operations.
std::optional<SiteContent> CallResolver::contentOf(const CallSite& site) const
{
    AOT_THRESHOLD(unsigned, maxLinks, 8);
    const Value& value = m_function.at(site.position.value);
    SiteContent content(static_cast<uint32_t>(RefusalKind::Call) << 24 | static_cast<uint32_t>(value.opcode) << 8 | value.flags);
    content.add(&m_flow);
    auto addAccess = [&](const Value& access) {
        content.add(static_cast<uint32_t>(access.opcode) << 8 | access.flags);
        content.add(access.key());
        content.add(access.receivers());
        content.addTypesOf(m_function, access.objectOperand());
    };

    switch (value.opcode) {
    case Opcode::PropGet:
    case Opcode::PropSet:
        addAccess(value);
        break;
    case Opcode::Call: {
        content.add(value.callees());
        unsigned numberOfLinks = 0;
        for (ValueID link : CastChain(m_function, value.calleeOperand())) {
            if (++numberOfLinks > maxLinks)
                return std::nullopt;
            content.addTypesOf(m_function, link);
        }
        content.add(numberOfLinks);
        content.add(static_cast<uint32_t>(site.memberRead.links.size()) << 1 | site.memberRead.canSinkToCall);
        if (site.memberRead.isEmpty())
            break;
        const Value& read = m_function.at(site.memberRead.read().value);
        if (read.opcode != Opcode::PropGet)
            return std::nullopt;
        addAccess(read);
        content.add(!value.thisValueOperand() ? 0u : value.thisValueOperand() == read.objectOperand() ? 1u : 2u);
        break;
    }
    default:
        return std::nullopt;
    }
    if (!content.holdsOnlyIDsOfProgram())
        return std::nullopt;
    return content;
}

Vector<PlannedCall> CallResolver::planSites(std::span<CallSite> sites)
{
    Vector<PlannedCall> plans;
    RefusalKind kind = m_resolution == Resolution::MethodTable ? RefusalKind::TableCall : RefusalKind::Call;
    PropertyAccessCache* cache = m_resolution == Resolution::Everything ? m_program.propertyAccessCache() : nullptr;
    for (CallSite& site : sites) {
        if (std::optional<OnlyCalleePlan> onlyCallee = planCallOfOnlyCallee(site)) {
            plans.append(PlannedCall { site, WTF::move(*onlyCallee) });
            continue;
        }
        uint32_t state = RefusedSites::stateOf(m_function, site.position.value);
        if (!site.memberRead.isEmpty())
            state = state * 31 + RefusedSites::stateOf(m_function, site.memberRead.read().value);
        if (m_context.refusedSites.contains(kind, site.position.value, state))
            continue;
        if (!site.memberRead.isEmpty())
            site.memberRead.canSinkToCall = canSinkToCall(m_context, site.memberRead.links.span(), site.position);

        std::optional<SiteContent> content = cache ? contentOf(site) : std::nullopt;
        std::optional<uint32_t> reasonOfKnownRefusal = content ? cache->refusedContents.find(*content) : std::nullopt;
        if (reasonOfKnownRefusal && !cache->checksAnswers) [[likely]] {
            reportDecline(site, static_cast<CallDeclineReason>(*reasonOfKnownRefusal));
            m_context.refusedSites.add(kind, m_function, site.position.value, state);
            continue;
        }

        m_context.planAsksAboutPlace = false;
        uint64_t unshareableEventCountBefore = m_context.types.unshareableEventCount();
        PlanOrDecline<CallPlan> planned = plan(site);
        RELEASE_ASSERT(!reasonOfKnownRefusal || (!planned && static_cast<uint32_t>(planned.error()) == *reasonOfKnownRefusal));
        if (planned)
            plans.append(PlannedCall { site, WTF::move(*planned) });
        else {
            reportDecline(site, planned.error());
            m_context.refusedSites.add(kind, m_function, site.position.value, state);
            if (content && !reasonOfKnownRefusal && !m_context.planAsksAboutPlace && m_context.types.unshareableEventCount() == unshareableEventCountBefore)
                cache->refusedContents.add(*content, static_cast<uint32_t>(planned.error()));
        }
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

    if (m_resolution == Resolution::MethodTable) {
        if (site.opcode == Opcode::Call && !site.memberRead.isEmpty() && tryNext(planTableCall(site)))
            return WTF::move(*found);
        return std::unexpected(CallDeclineReason::None);
    }

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
        if (!tryNext(planRestHandover(site)))
            tryNext(planListCall(site));
        break;
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
        [&](const OnlyCalleePlan& plan) {
            rewriteCallOfOnlyCallee(site, plan);
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
        [&](const ListCallPlan& plan) {
            rewriteListCall(site, plan);
        },
        [&](const RestHandoverPlan& plan) {
            rewriteRestHandover(site, plan);
        },
        [&](const TableCallPlan& plan) {
            rewriteTableCall(site, plan);
        });
    ++m_numberOfResolvedSites;
}

void CallResolver::reportDecline(const CallSite& site, CallDeclineReason decline)
{
    if (decline == CallDeclineReason::None || decline == CallDeclineReason::NeedsTest)
        return;
    if (!reportsDeclinesIn(site.position.block))
        return;
    LimitID limit = decline == CallDeclineReason::TooManyCallees ? LimitID::MaxDispatchCases : LimitID::None;
    m_context.addDecline(Wire::DeclineKind::HotCallNotResolved, site.position.value, decline, limit);
}

bool CallResolver::reportsDeclinesIn(BlockID block)
{
    return m_context.build.options().collectsDeclines() && isHot(heatClassOf(m_context, block));
}

CallDeclineReason CallResolver::declineOfUnprovenCallee(const CalleeCandidates& candidates) const
{
    return candidates.hasOneExpectedCallee() ? CallDeclineReason::NeedsTest : CallDeclineReason::UnknownCallee;
}

bool CallResolver::areSameObject(ValueID a, ValueID b) const
{
    if (a == b)
        return true;
    m_context.planAsksAboutPlace = true;
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
    check.setExpected(expectedTypeOf(context.program, context.function, check, context.types));
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
        m_function.edit(read) = makeObjectCoercibleCheck(m_context, copies.first(), key);
    else
        becomeUndefined(m_function, read);
    for (const ValueSite& cast : site.memberRead.links.span().subspan(1))
        becomeUndefined(m_function, cast.value);
    for (ValueID validation : site.memberRead.validations)
        becomeUndefined(m_function, validation);

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

void CallResolver::branchOnConditions(std::span<const MemberCondition> conditions, const OriginalCopy& original)
{
    RELEASE_ASSERT(m_expansion.isOpen());
    if (conditions.empty())
        return;

    BlockFrequency frequency = m_function.at(m_builder.currentBlock()).frequency;
    BlockID failedUnasked = m_builder.beginPrediction();
    ValueID areUnchanged = m_expansion.append(makeConditionsTest(m_context.types, conditions));
    IRBuilder::BranchBlocks branch = m_builder.emitBranch(areUnchanged, frequency, rarerOf(frequency, BlockFrequency::Unlikely));
    m_builder.endPrediction(failedUnasked, branch.notTaken);
    m_builder.moveToEndOf(branch.notTaken);
    m_expansion.finishArm(m_expansion.appendOriginal(original));
    m_builder.moveToEndOf(branch.taken);
}

ValueID CallResolver::appendCall(const CallArm& arm, ValueID thisValue, ValueID closure, std::span<const ValueID> arguments)
{
    RELEASE_ASSERT(m_expansion.isOpen() && arm.kind == CallArmKind::DirectCall);
    // An arm of a dispatch may be for the method of a class that the receiver is known not to have: `this.describe()` of a base constructor, inlined under `new Base()`, has an arm for the method of the subclass.
    const Function& callee = m_program.at(arm.callee);
    if (thisValue && callee.has(FunctionFlags::HasThis) && callee.thisType.type && !callee.thisType.flags.contains(PositionFlags::IsExpected)) {
        const Type& declared = m_program.at(callee.thisType.type);
        if (typeOf(thisValue).hasOnlyLayouts(m_program) && declared.hasOnlyLayouts(m_program) && !typeOf(thisValue).overlaps(declared, m_program))
            return convertTo(m_builder, thisValue, callee.thisType);
    }

    const Value& original = m_expansion.original();
    Vector<ValueID, 8> captures = capturesOf(arm, closure, thisValue);
    return appendDirectCall(m_builder, {
        .callee = arm.callee,
        .captures = captures.span(),
        .thisValue = thisValue,
        .calleeValue = callee.has(FunctionFlags::TakesCallee) ? closure : ValueID(),
        .arguments = arguments,
        .accessSite = original.accessSite(),
        .isNeverInlined = original.opcode == Opcode::Call && original.isNeverInlined(),
        .isDemotedTailCall = m_expansion.isTailCall(),
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
