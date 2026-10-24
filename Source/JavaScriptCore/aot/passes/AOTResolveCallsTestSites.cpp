/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessCache.h"
#include "AOTResolveAccesses.h"
#include "AOTResolveCalls.h"
#include "AOTResultRules.h"
#include "AOTStatistics.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestedMemberUse {
    NamedAccessSite site;
    AccessOperands operands;
    SlotStamp stamp;
    AccessTests tests;
};

using TestedMemberUsePlanner = std::optional<TestedMemberUse> (*)(FunctionPassContext&, const ValueSite&, const ReceiverCandidates*);

enum class SuperAccessForm : uint8_t { ByName, MethodClosure, AccessorCall, Undefined };

struct TestedSuperAccess {
    SuperAccessForm form { SuperAccessForm::ByName };
    FunctionID function;
    MemberConditions conditions;
};

// A use of a member for which the plan without a failed test leads to no site, whoever uses what is read.
struct RefusedMemberUse {
    RefusedMemberUse(TestSiteReporter& reporter, ValueID receiver, NOESCAPE const Invocable<std::optional<SiteContent>()> auto& contentOfUse)
        : cache(reporter.program().propertyAccessCache())
    {
        if (!cache || reporter.mayHaveFailedTestOf(receiver))
            return;
        content = contentOfUse();
        isCached = content && cache->refusedContents.find(*content);
    }

    bool isKnown() const { return isCached && !cache->checksAnswers; }
    void didRefuse(bool hasFailedTest)
    {
        if (content && !isCached && !hasFailedTest)
            cache->refusedContents.add(*content);
    }
    void willReport() const { RELEASE_ASSERT(!isCached); }

    PropertyAccessCache* cache;
    std::optional<SiteContent> content;
    bool isCached { false };
};

} // anonymous namespace

// A test in front of a member call also answers for the member read before it, so what it proves may never come back once it is lost. The bits of a class never come back either.
static constexpr ProofSet monotoneObjectProofs { Proof::NotNullish, Proof::OwnInstance, Proof::PrototypeLinkUnchanged, Proof::NoExtraProperties, Proof::NoFieldOutsideSlot };

static std::optional<TestedMemberUse> planTestedAccess(FunctionPassContext& context, const ValueSite& site, const ReceiverCandidates* established)
{
    const Value& access = context.function.at(site.value);
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access, ExpectedClassObjects::Tested);
    if (!query)
        return std::nullopt;
    query->isOnlyCalled = true;
    query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    if (established)
        query->receivers = *established;

    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
    if (!plan.isResolved())
        return std::nullopt;

    AccessOperands operands = operandsOfNamedAccess(access);
    SlotStamp stamp = createSlotStamp(context, access, plan);
    AccessTests tests = createAccessTests(context, plan, { operands.receiver, operands.stored, stamp });
    return TestedMemberUse { { site, WTF::move(*query), WTF::move(plan) }, operands, stamp, WTF::move(tests) };
}

static std::optional<TestedMemberUse> planTestedMemberCall(FunctionPassContext& context, const ValueSite& site, const ReceiverCandidates* established)
{
    const Value& call = context.function.at(site.value);
    ValueID thisValue = call.thisValueOperand();

    PropertyAccessQuery query;
    query.access = Access::Read;
    query.key = call.calleeKey();
    query.receivers = established ? *established : receiverCandidates(context.program, context.function, thisValue);
    query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    query.isOnlyCalled = true;
    query.module = context.function.module;

    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, query);
    if (plan.form != AccessForm::Method)
        return std::nullopt;

    SlotStamp stamp { query.key, context.types.completeLayoutSet(plan.receivers.span()), KnowledgeLevel::Expected, call.accessSite() };
    AccessTests tests = createAccessTests(context, plan, { thisValue, { }, stamp });
    return TestedMemberUse { { site, WTF::move(query), WTF::move(plan) }, { thisValue, { }, { } }, stamp, WTF::move(tests) };
}

// One bit says of every object of a class that it finds the member where the class definitions put it, and one object that hides the member or gets another prototype clears it for all of them.
static std::optional<TestedMemberUse> planWithTestsOfEachObject(FunctionPassContext& context, TestedMemberUse&& use)
{
    const Program& program = context.program;
    const PropertyAccessQuery& query = use.site.query;
    PropertyAccessPlan& plan = use.site.plan;
    if (query.start != MemberStart::Instance || (plan.member.kind != MemberKind::Method && plan.member.kind != MemberKind::Accessor))
        return std::nullopt;

    MemberConditions conditions;
    ProofSet proofs = plan.requiredProofs;
    for (LayoutID layout : plan.receivers) {
        MemberAnswer inherited = findPrototypeMember({ program, &context.inputs.flow }, { MemberStart::ClassPrototype, declaredLayoutOf(program, layout), query.key, query.access, false });
        bool isSameMember = inherited.kind == plan.member.kind && inherited.method == plan.member.method && inherited.getter == plan.member.getter && inherited.setter == plan.member.setter;
        if (!isSameMember || !inherited.receiverProofs.isEmpty())
            return std::nullopt;
        addConditions(conditions, inherited.conditions.span());
        if (ownPropertyGainOf(program, layout, query.key) != OwnPropertyGain::Impossible)
            proofs.add(Proof::NoExtraProperties);
        if (!receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype))
            proofs.add(Proof::PrototypeLinkUnchanged);
    }
    if (conditions == plan.conditions)
        return std::nullopt;

    plan.conditions = WTF::move(conditions);
    plan.requiredProofs = proofs;
    use.tests = createAccessTests(context, plan, { use.operands.receiver, use.operands.stored, use.stamp });
    return WTF::move(use);
}

// hasFailedTest: the plan is another than where no test has failed.
static std::optional<TestedMemberUse> planWithoutFailedTests(TestSiteReporter& reporter, TestedMemberUsePlanner plan, const ValueSite& site, bool& hasFailedTest)
{
    std::optional<TestedMemberUse> use = plan(reporter.context(), site, nullptr);
    while (use && !use->tests.tests.isEmpty()) {
        const Value* receiverTest = use->tests.findReceiverLayoutTest();
        if (!receiverTest || reporter.findEarlierTest(*receiverTest) != EarlierTest::Failed)
            break;
        hasFailedTest = true;

        ReceiverCandidates remaining = use->site.query.receivers;
        remaining.predicted.subtract(use->site.plan.receivers);
        RELEASE_ASSERT(remaining.predicted.size() < use->site.query.receivers.predicted.size());
        use = plan(reporter.context(), site, &remaining);
    }
    if (!use)
        return std::nullopt;

    auto conditionsTest = std::ranges::find(use->tests.tests, Opcode::AreMembersUnchanged, &Value::opcode);
    if (conditionsTest == use->tests.tests.end() || reporter.findEarlierTest(*conditionsTest) != EarlierTest::Failed)
        return use;
    hasFailedTest = true;
    return planWithTestsOfEachObject(reporter.context(), WTF::move(*use));
}

static bool needsPlaceableTests(const TestedMemberUse& use)
{
    return !use.site.plan.needsNoTest() && !use.tests.tests.isEmpty();
}

// The callee tests its own arguments and runs without its types where one of them fails, so the call has the effects of both bodies.
static Effects effectsOfDirectCall(FunctionPassContext& context, FunctionID callee)
{
    return effectsOfCalling({ context.program, context.inputs.effects }, callee).normal;
}

static bool canRecreateMethodClosure(const Program& program, const PropertyAccessPlan& plan)
{
    const Function& method = program.at(plan.member.method);
    return !method.has(FunctionFlags::TakesCallee) && method.captures.size() == (plan.capturesReceiver ? 1u : 0u);
}

static bool isMemberCall(FunctionAnalyses& analyses, const Function& function, const Value& call)
{
    ValueID thisValue = call.thisValueOperand();
    if (!call.calleeKey() || !thisValue)
        return false;
    const Value& read = function.at(sameObjectThroughCasts(function, call.calleeOperand()));
    if (read.opcode != Opcode::PropGet || read.isFreeName() || read.key() != call.calleeKey())
        return false;
    return analyses.identityOf(read.objectOperand()) == analyses.identityOf(thisValue);
}

static void reportClosureTest(TestSiteReporter& reporter, ValueID callee, FunctionID function)
{
    bool isCall = reporter.useValue().opcode == Opcode::Call;
    reporter.report({ makeClosureTest(callee, function) }, isCall ? effectsOfDirectCall(reporter.context(), function) : reporter.analyses().effectsOf(reporter.use()).normal);
}

static bool reportMemberCallTests(TestSiteReporter& reporter, const ValueSite& site)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    if (!isMemberCall(context.analyses, context.function, call))
        return false;

    // All that planTestedMemberCall() takes from the call and its function.
    RefusedMemberUse refused(reporter, call.thisValueOperand(), [&]() -> std::optional<SiteContent> {
        SiteContent content(static_cast<uint32_t>(TestSitePlanner::MemberCall) << 24);
        content.add(&context.inputs.flow);
        content.add(call.calleeKey());
        content.add(context.function.module.index());
        content.addTypesOf(context.function, call.thisValueOperand());
        return content.holdsOnlyIDsOfProgram() ? std::optional(content) : std::nullopt;
    });
    if (refused.isKnown()) [[likely]]
        return false;

    bool hasFailedTest = false;
    std::optional<TestedMemberUse> use = planWithoutFailedTests(reporter, planTestedMemberCall, site, hasFailedTest);
    if (!use || use->tests.tests.isEmpty()) {
        refused.didRefuse(hasFailedTest);
        return false;
    }
    if (!needsPlaceableTests(*use))
        return false;

    const PropertyAccessPlan& plan = use->site.plan;
    if ((plan.requiredProofs | monotoneObjectProofs) != monotoneObjectProofs) {
        refused.willReport();
        if (!mayBeCalledDirectly(context.program, plan.member.method))
            return false;
        // What may come back says nothing about the time of the read, so the function that was read is tested itself: a closure kept in a field whose slot may be emptied and filled again.
        reportClosureTest(reporter, call.calleeOperand(), plan.member.method);
        return reporter.didReport();
    }
    if (!canRecreateMethodClosure(context.program, plan) || !mayBeCalledDirectly(context.program, plan.member.method))
        return false;

    refused.willReport();
    reporter.report(use->tests.tests.span(), effectsOfDirectCall(context, plan.member.method));
    return reporter.didReport();
}

// The function that a call enters is read before its arguments are computed, so which function it is is decided where the read stands, whatever the arguments do.
static bool canBecomeMethodClosure(FunctionPassContext& context, ValueID read)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();

    bool isCalled = false;
    BitVector isSeen(function.values.size());
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            ValueID next;
            if (use.kind == Use::Kind::Operand) {
                Opcode user = function.at(use.user).opcode;
                if (isCast(user) && !use.operandIndex)
                    next = use.user;
                else if ((user == Opcode::Call && use.operandIndex == OperandIndex::Call::callee) || (user == Opcode::CallList && use.operandIndex == OperandIndex::CallList::callee)) {
                    isCalled = true;
                    continue;
                } else
                    return false;
            } else if (use.kind == Use::Kind::EdgeArgument || use.kind == Use::Kind::HandlerArgument)
                next = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
            else
                return false;
            if (!isSeen.quickSet(next.index()))
                worklist.append(next);
        }
    }
    return isCalled;
}

static void reportMemberAccessTests(TestSiteReporter& reporter, const ValueSite& site)
{
    FunctionPassContext& context = reporter.context();
    const Value& access = reporter.useValue();
    RefusedMemberUse refused(reporter, access.operand(*receiverOf(access.opcode)), [&] {
        return contentOfNamedAccess(context, access, static_cast<uint8_t>(TestSitePlanner::CalledMember));
    });
    if (refused.isKnown()) [[likely]]
        return;

    bool hasFailedTest = false;
    std::optional<TestedMemberUse> use = planWithoutFailedTests(reporter, planTestedAccess, site, hasFailedTest);
    if (!use) {
        refused.didRefuse(hasFailedTest);
        return;
    }
    // The rule of property accesses is next to ask for the plan of an access that gets no site here.
    auto leavePlan = [&] {
        if (hasFailedTest || reporter.didReport())
            return;
        std::optional<bool> isOnlyCalled = use->site.query.planFollowsFromIsOnlyCalled ? std::optional(true) : std::nullopt;
        reporter.keepPlanOfUse({ isOnlyCalled, WTF::move(use->site.plan), use->stamp, WTF::move(use->tests) });
    };
    auto refuse = [&] {
        refused.didRefuse(hasFailedTest);
        leavePlan();
    };

    const PropertyAccessPlan& plan = use->site.plan;
    if (use->tests.tests.isEmpty() || (plan.form != AccessForm::Method && plan.form != AccessForm::Call))
        return refuse();
    if (!needsPlaceableTests(*use))
        return leavePlan();

    if (plan.form == AccessForm::Method) {
        if (!canRecreateMethodClosure(context.program, plan))
            return refuse();
        if (canBecomeMethodClosure(context, site.value)) {
            refused.willReport();
            reporter.report(use->tests.tests.span(), effectsAfterRewrite(context, plan, use->stamp));
        }
        return leavePlan();
    }

    FunctionID accessor = plan.member.accessorFor(use->site.query.access);
    if (!canCallWithoutClosure(context.program, accessor))
        return refuse();
    refused.willReport();
    reporter.report(use->tests.tests.span(), effectsOfDirectCall(context, accessor));
    leavePlan();
}

static TestedSuperAccess planTestedSuperAccess(FunctionPassContext& context, const Value& access)
{
    const Program& program = context.program;
    if (!access.key())
        return { };
    HomeObject home = findHomeObject(program, context.function, access);
    if (!home)
        return { };

    Access kind = access.opcode == Opcode::SuperSet ? Access::Store : Access::Read;
    MemberAnswer answer = findSuperMember({ program, &context.inputs.flow }, home, access.key(), kind);
    if (answer.level != KnowledgeLevel::Expected || !answer.receiverProofs.isEmpty() || answer.conditions.isEmpty())
        return { };

    if (answer.kind == MemberKind::Method && kind == Access::Read && canCallWithoutClosure(program, answer.method))
        return { SuperAccessForm::MethodClosure, answer.method, WTF::move(answer.conditions) };
    if (answer.kind != MemberKind::Accessor)
        return { };
    FunctionID accessor = answer.accessorFor(kind);
    if (!accessor && kind == Access::Read)
        return { SuperAccessForm::Undefined, { }, WTF::move(answer.conditions) };
    if (accessor && canCallWithoutClosure(program, accessor))
        return { SuperAccessForm::AccessorCall, accessor, WTF::move(answer.conditions) };
    return { };
}

static void reportSuperAccessTests(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    TestedSuperAccess access = planTestedSuperAccess(context, reporter.useValue());

    Effects effects = Effects::none();
    if (access.form == SuperAccessForm::ByName || (access.form == SuperAccessForm::MethodClosure && !isOnlyCalled(context.analyses, reporter.use())))
        return;
    if (access.form == SuperAccessForm::MethodClosure)
        effects = declaredBoundOf(Opcode::NewClosure);
    else if (access.form == SuperAccessForm::AccessorCall)
        effects = effectsOfDirectCall(context, access.function);
    reporter.report({ makeConditionsTest(context.types, access.conditions.span()) }, effects);
}

// With several expected callees the call of the value stays as it is: chooseDispatchedCallees() says why.
static void reportCalleeTest(TestSiteReporter& reporter)
{
    const Program& program = reporter.program();
    const Value& call = reporter.useValue();
    ValueID callee = call.opcode == Opcode::SuperConstruct ? call.parentOperand() : call.calleeOperand();

    CalleeCandidates candidates = calleeCandidatesOf(program, reporter.function(), call, callee);
    if (!candidates.hasOneExpectedCallee())
        return;
    FunctionID member = candidates.expected[0];
    if (call.opcode != Opcode::Call ? canConstructDirectly(program, member) : mayBeCalledDirectly(program, member))
        reportClosureTest(reporter, callee, member);
}

// f.apply(t, list) is a list call once the list is known to be an array.
static void reportForwardedListTest(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    bool forwardsAnyReceiver = call.builtinCallForm() == BuiltinCallForm::AnyReceiver && forwardsCallOfAnyReceiver(context.program.builtins().at(call.overload()));
    if ((call.builtinCallForm() != BuiltinCallForm::Whole && !forwardsAnyReceiver) || !reporter.typeOf(reporter.use()).isAnything())
        return;
    std::optional<ForwardedCallPlan> plan = planForwardedCall(context.program, context.function, builtinCallOf(context.program, call));
    if (!plan || !plan->list || isAlwaysArray(reporter.typeOf(plan->list)))
        return;

    const Type& expected = context.program.expectedOrTypeOf(context.function, plan->list);
    if (expected.anyObject || expected.findClassPart(BuiltinClasses::Array))
        reporter.report({ makeTypeTest(plan->list, context.types.intern(Type::builtinClass(BuiltinClasses::Array))) }, Effects::unknownCode());
}

// While nobody has stored to the prototype property of the one class object, it holds the prototype object of the class, and that is what the Structure of an allocation site names.
static void reportTestOfOwnConstructPrototype(TestSiteReporter& reporter, ValueID prototype, NOESCAPE const Invocable<Effects()> auto& effectsWhenPassed)
{
    const Program& program = reporter.program();
    LayoutID allocated = reporter.useValue().layout();
    const Value& read = reporter.function().at(prototype);
    if (read.opcode != Opcode::GetConstructPrototype || read.layout() != allocated)
        return;
    // The read of the class object's own prototype takes a function: new.target of a body that native code may enter is undefined or the function.
    const Type& newTarget = program.typeOf(reporter.function(), read.newTargetOperand());
    if (!newTarget.isObject() || classOfClassObject(program, newTarget) != allocated || guardsAreNeverSet(program, program.at(allocated)))
        return;

    MemberConditions conditions;
    addCondition(conditions, { MemberCondition::Kind::MemberUnchanged, allocated, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    if (!isEvaluatedOnce(program, program.at(allocated)))
        addCondition(conditions, { MemberCondition::Kind::ClassEvaluatedOnce, allocated, ClassSide::Prototype, { } });
    Value test = makeConditionsTest(reporter.types(), conditions.span());
    reporter.report({ test }, effectsWhenPassed());
}

static void reportInstanceOfTests(TestSiteReporter& reporter)
{
    for (const TestedInstanceOf& tested : planTestedInstanceOf(reporter.context(), reporter.useValue())) {
        if (std::ranges::any_of(tested.tests.tests, [&](const Value& test) { return reporter.findEarlierTest(test) == EarlierTest::Failed; }))
            continue;
        // The walk asks each object of the chain for its prototype, and a proxy answers with code of the program: what the row declares is what the walk does by itself.
        reporter.report(tested.tests.tests.span(), tested.walksChain ? reporter.analyses().effectsOf(reporter.use()).normal : Effects::none());
        return;
    }
}

void reportCallTests(TestSiteReporter& reporter)
{
    reporter.forEachCandidate([&](const Value& value) {
        ValueSite site { reporter.point().block, reporter.point().index, reporter.use() };
        switch (value.opcode) {
        case Opcode::Call:
            if (!reportMemberCallTests(reporter, site))
                reportCalleeTest(reporter);
            break;
        case Opcode::Construct:
        case Opcode::SuperConstruct:
            reportCalleeTest(reporter);
            break;
        case Opcode::PropGet:
        case Opcode::PropSet:
            reportMemberAccessTests(reporter, site);
            break;
        case Opcode::SuperGet:
        case Opcode::SuperSet:
            reportSuperAccessTests(reporter);
            break;
        case Opcode::CallBuiltin:
            reportForwardedListTest(reporter);
            break;
        case Opcode::GetConstructPrototype:
            reportTestOfOwnConstructPrototype(reporter, reporter.use(), [] { return declaredBoundOf(Opcode::GetClassPrototype); });
            break;
        case Opcode::InstanceOf:
            reportInstanceOfTests(reporter);
            break;
        case Opcode::NewObject:
            if (value.prototypeOperand())
                reportTestOfOwnConstructPrototype(reporter, value.prototypeOperand(), [&] { return reporter.analyses().effectsOf(reporter.use()).normal; });
            break;
        default:
            break;
        }
    });
}

static void rewriteTestedInstanceOf(TestSiteRewriter& rewriter)
{
    Value& use = rewriter.useValue();
    LayoutList testedLayouts;
    std::optional<LayoutList> instances = instancesAmongTestedLayouts(rewriter.program(), rewriter.function(), use, rewriter.tests(), testedLayouts);
    if (!instances) {
        const Value& instanceOf = rewriter.useValue();
        ValueID subject = instanceOf.valueOperand();
        Value read = IRBuilder::make(Opcode::GetClassPrototype, { instanceOf.constructorOperand() });
        read.setLayout(classTestedBy(rewriter.program(), rewriter.function(), instanceOf));
        ValueID prototype = rewriter.insertBeforeUse(WTF::move(read));

        Value& walk = rewriter.useValue();
        walk.become(Opcode::HasInPrototypeChain);
        walk.setValueOperand(subject);
        walk.setPrototypeOperand(prototype);
        return;
    }
    if (instances->isEmpty() || instances->size() == testedLayouts.size()) {
        use.become(Opcode::Boolean);
        use.setBoolean(!instances->isEmpty());
        return;
    }

    Origin origin = use.origin();
    use = makeTypeTest(use.valueOperand(), rewriter.types().intern(Type::forLayouts(instances->span(), rewriter.types())));
    use.setOrigin(origin);
}

void rewriteTestedCall(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeUse = [&](auto&& value) { return rewriter.insertBeforeUse(WTF::move(value)); };
    InsertValue insert = insertBeforeUse;
    ValueInsertion insertion { rewriter.builder().context(), rewriter.function(), rewriter.types(), rewriter.useValue().origin(), insert };

    std::optional<ReceiverCandidates> receivers = receiversEstablishedBy(rewriter.program(), rewriter.tests());
    const ReceiverCandidates* established = receivers ? &*receivers : nullptr;
    ValueSite site { rewriter.point().block, rewriter.point().index, rewriter.use() };
    auto becomeDirectCall = [&](const DirectCallRequest& request) {
        Value call = makeDirectCall(insertion, request);
        Value& use = rewriter.useValue();
        call.setOrigin(use.origin());
        use = WTF::move(call);
    };

    switch (rewriter.useValue().opcode) {
    case Opcode::Call: {
        bool testsCallee = rewriter.tests()[0].opcode == Opcode::IsClosureOf;
        std::optional<TestedMemberUse> use = testsCallee ? std::nullopt : planTestedMemberCall(context, site, established);
        RELEASE_ASSERT(testsCallee || use);

        const Value& call = rewriter.useValue();
        FunctionID function = testsCallee ? rewriter.tests()[0].function() : use->site.plan.member.method;
        ValueID callee = call.calleeOperand();
        // The receiver is the value that passed the tests, so the callee's own test of `this` folds once the callee is inlined.
        ValueID thisValue = testsCallee ? call.thisValueOperand() : use->operands.receiver;
        AccessSiteID accessSite = call.accessSite();
        bool isNeverInlined = call.isNeverInlined();
        bool isDemotedTailCall = call.isDemotedTailCall();
        Vector<ValueID, 8> arguments(call.variadicOperands());

        Vector<ValueID, 8> captures;
        if (testsCallee)
            captures = insertCaptures(insertion, function, callee);
        else if (use->site.plan.capturesReceiver)
            captures.append(thisValue);
        becomeDirectCall({
            .callee = function,
            .captures = captures.span(),
            .thisValue = thisValue,
            .calleeValue = testsCallee && context.program.at(function).has(FunctionFlags::TakesCallee) ? callee : ValueID(),
            .arguments = arguments.span(),
            .accessSite = accessSite,
            .isNeverInlined = isNeverInlined,
            .isDemotedTailCall = isDemotedTailCall,
        });
        break;
    }
    case Opcode::Construct:
    case Opcode::SuperConstruct:
    case Opcode::CallBuiltin:
        return;
    case Opcode::PropGet:
    case Opcode::PropSet: {
        std::optional<TestedMemberUse> use = planTestedAccess(context, site, established);
        RELEASE_ASSERT(use);
        if (use->site.plan.form == AccessForm::Method) {
            use->site.plan.level = KnowledgeLevel::Expected;
            AccessRewriter(context, insert).rewrite(use->site, use->operands);
            break;
        }
        Vector<ValueID, 1> arguments;
        if (use->operands.stored)
            arguments.append(use->operands.stored);
        becomeDirectCall({ .callee = use->site.plan.member.accessorFor(use->site.query.access), .thisValue = use->operands.receiver, .arguments = arguments.span(), .accessSite = use->stamp.accessSite });
        break;
    }
    case Opcode::SuperGet:
    case Opcode::SuperSet: {
        Value& use = rewriter.useValue();
        TestedSuperAccess access = planTestedSuperAccess(context, use);
        RELEASE_ASSERT(access.form != SuperAccessForm::ByName);
        if (access.form == SuperAccessForm::MethodClosure) {
            use.become(Opcode::NewClosure);
            use.setFunction(access.function);
        } else if (access.form == SuperAccessForm::Undefined)
            use.become(Opcode::Undefined);
        else {
            Vector<ValueID, 1> arguments;
            if (use.opcode == Opcode::SuperSet)
                arguments.append(use.valueOperand());
            becomeDirectCall({ .callee = access.function, .thisValue = use.receiverOperand(), .arguments = arguments.span(), .accessSite = use.accessSite() });
        }
        break;
    }
    case Opcode::GetConstructPrototype: {
        Value& read = rewriter.useValue();
        ValueID classObject = read.newTargetOperand();
        LayoutID layout = read.layout();
        read.become(Opcode::GetClassPrototype);
        read.setOperand(OperandIndex::GetClassPrototype::classObject, classObject);
        read.setLayout(layout);
        break;
    }
    case Opcode::NewObject:
        rewriter.useValue().setOperand(*OperandPosition::prototype(Opcode::NewObject), ValueID());
        break;
    case Opcode::InstanceOf:
        rewriteTestedInstanceOf(rewriter);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value& use = rewriter.useValue();
    use.type = resultTypeOf(context.program, context.function, use, context.types, &context.typingMemo);
    context.build.statistics().count(StatisticID::CallsGuarded);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
