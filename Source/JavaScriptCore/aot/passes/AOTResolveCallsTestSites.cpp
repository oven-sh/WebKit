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
#include "AOTResolveAccesses.h"
#include "AOTResolveCalls.h"
#include "AOTResultRules.h"
#include "AOTStatistics.h"
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

struct TestedCallRewrite {
    TestSiteRewriter& rewriter;
    const ValueInsertion& insertion;
    ValueSite site;
    const ReceiverCandidates* established { nullptr };
};

} // anonymous namespace

// A test in front of a member call also answers for the member read before it, so what it proves may never come back once it is lost.
static constexpr ProofSet monotoneObjectProofs { Proof::OwnInstance, Proof::PrototypeLinkUnchanged, Proof::NoExtraProperties };

static std::optional<TestedMemberUse> planTestedAccess(FunctionPassContext& context, const ValueSite& site, const ReceiverCandidates* established)
{
    const Value& access = context.function.at(site.value);
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
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
    query.receivers = established ? *established : receiverCandidates(context.program, context.function, thisValue, { });
    query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    query.isOnlyCalled = true;

    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, query);
    if (plan.form != AccessForm::Method)
        return std::nullopt;

    AccessOperands operands { thisValue, { }, { } };
    SlotStamp stamp { query.key, context.types.completeLayoutSet(plan.receivers.span()), KnowledgeLevel::Predicted, call.accessSite() };
    AccessTests tests = createAccessTests(context, plan, { thisValue, { }, stamp });
    return TestedMemberUse { { site, WTF::move(query), WTF::move(plan) }, operands, stamp, WTF::move(tests) };
}

static std::optional<TestedMemberUse> planWithoutFailedArms(TestSiteReporter& reporter, TestedMemberUsePlanner plan, const ValueSite& site)
{
    std::optional<TestedMemberUse> use = plan(reporter.context(), site, nullptr);
    while (use && !use->tests.tests.isEmpty()) {
        const Value* receiverTest = use->tests.findReceiverLayoutTest();
        if (!receiverTest || reporter.findEarlierTest(*receiverTest) != EarlierTest::Failed)
            break;

        ReceiverCandidates remaining = use->site.query.receivers;
        remaining.predicted.subtract(use->site.plan.receivers);
        RELEASE_ASSERT(remaining.predicted.size() < use->site.query.receivers.predicted.size());
        use = plan(reporter.context(), site, &remaining);
    }
    return use;
}

static bool needsPlaceableTests(const TestedMemberUse& use)
{
    return !use.site.plan.needsNoTest() && !use.tests.tests.isEmpty();
}

static Effects effectsOfDirectCall(FunctionPassContext& context, FunctionID closureFunction, FunctionID version)
{
    EffectContext effectContext { context.program, context.inputs.effects };
    Effects effects = effectsOfCalling(effectContext, version).normal;
    // A call whose arguments need a conversion enters the version with the written types.
    FunctionID writtenVersion = versionToEnter(context.program, context.inputs.flow, closureFunction, { });
    if (writtenVersion != version)
        effects.merge(effectsOfCalling(effectContext, writtenVersion).normal);
    return effects;
}

static bool canRecreateMethodClosure(const Program& program, const PropertyAccessPlan& plan)
{
    const Function& method = program.at(plan.member.method);
    return !method.has(FunctionFlags::TakesCallee) && method.captures.size() == (plan.capturesReceiver ? 1u : 0u);
}

// A use under tests is rewritten in place, so neither a rejection path nor a Claim of the result can follow the call.
static bool canBecomeCallOf(const FunctionPassContext& context, FunctionID version)
{
    return !rejectsOnFailedConversion(context.program, version) && resultCheckOf(context.inputs.flow, version) == ResultCheck::ByCallee;
}

static bool canCallTestedMethod(const FunctionPassContext& context, const PropertyAccessPlan& plan, FunctionID version)
{
    return canRecreateMethodClosure(context.program, plan)
        && mayBeCalledDirectly(context.program, plan.member.method)
        && canBecomeCallOf(context, version);
}

static bool canCallTestedAccessor(const FunctionPassContext& context, FunctionID accessor, FunctionID version)
{
    return canCallWithoutClosure(context.program, accessor) && canBecomeCallOf(context, version);
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

static Effects effectsOfTestedCallee(TestSiteReporter&, FunctionID member);

// What may come back says nothing about the time of the read, so the function that was read is tested itself. Where the read stands under its own tests in the same stretch, it is the closure of the method, and this test folds.
static bool reportTestOfMethodRead(TestSiteReporter& reporter, FunctionID method)
{
    if (!mayBeCalledDirectly(reporter.program(), method))
        return false;
    Value test = makeClosureTest(reporter.useValue().calleeOperand(), method);
    if (reporter.findEarlierTest(test) == EarlierTest::Failed)
        return false;
    reporter.report({ test }, effectsOfTestedCallee(reporter, method));
    return true;
}

static bool reportMemberCallTests(TestSiteReporter& reporter, const ValueSite& site)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    if (!isMemberCall(context.analyses, context.function, call))
        return false;

    std::optional<TestedMemberUse> use = planWithoutFailedArms(reporter, planTestedMemberCall, site);
    if (!use || !needsPlaceableTests(*use))
        return false;

    const PropertyAccessPlan& plan = use->site.plan;
    if ((plan.requiredProofs | monotoneObjectProofs) != monotoneObjectProofs)
        return reportTestOfMethodRead(reporter, plan.member.method);

    FunctionID version = versionToEnter(context.program, context.inputs.flow, plan.member.method, call.callees());
    if (!canCallTestedMethod(context, plan, version))
        return false;

    reporter.report(use->tests.tests.span(), effectsOfDirectCall(context, plan.member.method, version));
    return true;
}

static bool canBecomeMethodClosure(FunctionPassContext& context, ValueID read)
{
    if (!isOnlyCalled(context.analyses, read))
        return false;

    const UseLists& useLists = context.analyses.useLists();
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        ValueID current = worklist.takeLast();
        for (const Use& use : useLists.usesOf(current)) {
            const Value& user = context.function.at(use.user);
            if (isCast(user.opcode)) {
                worklist.append(use.user);
                continue;
            }
            if (user.opcode != Opcode::Call)
                continue;
            ValueLocation location = useLists.locationOf(use.user);
            MemberRead memberRead = findMemberRead(context, { location.block, location.index, use.user });
            if (!memberRead.isEmpty() && memberRead.read().value == read && !memberRead.canSinkToCall)
                return false;
        }
    }
    return true;
}

static void reportMemberAccessTests(TestSiteReporter& reporter, const ValueSite& site)
{
    FunctionPassContext& context = reporter.context();
    std::optional<TestedMemberUse> use = planWithoutFailedArms(reporter, planTestedAccess, site);
    if (!use || !needsPlaceableTests(*use))
        return;

    const PropertyAccessPlan& plan = use->site.plan;
    if (plan.form == AccessForm::Method) {
        if (canRecreateMethodClosure(context.program, plan) && canBecomeMethodClosure(context, site.value))
            reporter.report(use->tests.tests.span(), effectsAfterRewrite(context, plan, use->stamp));
        return;
    }
    if (plan.form != AccessForm::Call)
        return;

    FunctionID accessor = plan.member.accessorFor(use->site.query.access);
    FunctionID version = versionToEnter(context.program, context.inputs.flow, accessor, { });
    if (canCallTestedAccessor(context, accessor, version))
        reporter.report(use->tests.tests.span(), effectsOfDirectCall(context, accessor, version));
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
    if (answer.level != KnowledgeLevel::Predicted || !answer.receiverProofs.isEmpty() || answer.conditions.isEmpty())
        return { };

    switch (answer.kind) {
    case MemberKind::Method:
        if (kind == Access::Read && canCallWithoutClosure(program, answer.method))
            return { SuperAccessForm::MethodClosure, answer.method, WTF::move(answer.conditions) };
        return { };
    case MemberKind::Accessor: {
        FunctionID accessor = answer.accessorFor(kind);
        if (!accessor) {
            if (kind == Access::Read)
                return { SuperAccessForm::Undefined, { }, WTF::move(answer.conditions) };
            return { };
        }
        if (canCallTestedAccessor(context, accessor, versionToEnter(program, context.inputs.flow, accessor, { })))
            return { SuperAccessForm::AccessorCall, accessor, WTF::move(answer.conditions) };
        return { };
    }
    case MemberKind::Unknown:
    case MemberKind::Absent:
    case MemberKind::Field:
    case MemberKind::Variable:
    case MemberKind::Namespace:
    case MemberKind::Builtin:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportSuperAccessTests(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    TestedSuperAccess access = planTestedSuperAccess(context, reporter.useValue());

    Effects effects;
    switch (access.form) {
    case SuperAccessForm::ByName:
        return;
    case SuperAccessForm::MethodClosure:
        if (!isOnlyCalled(context.analyses, reporter.use()))
            return;
        effects = declaredBoundOf(Opcode::NewClosure);
        break;
    case SuperAccessForm::AccessorCall:
        effects = effectsOfDirectCall(context, access.function, versionToEnter(context.program, context.inputs.flow, access.function, { }));
        break;
    case SuperAccessForm::Undefined:
        effects = Effects::none();
        break;
    }

    Vector<Value, 2> tests;
    for (const MemberCondition& condition : access.conditions)
        tests.append(makeConditionTest(condition));
    reporter.report(tests.span(), effects);
}

static Effects effectsOfTestedCallee(TestSiteReporter& reporter, FunctionID member)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    const Effects& effectsOfGenericForm = context.analyses.effectsOf(reporter.use()).normal;
    if (call.opcode != Opcode::Call)
        return effectsOfGenericForm;

    FunctionID version = versionToEnter(context.program, context.inputs.flow, member, call.callees());
    if (!canBecomeCallOf(context, version))
        return effectsOfGenericForm;
    return effectsOfDirectCall(context, member, version);
}

static void reportCalleeTest(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    const Program& program = context.program;
    const Value& call = reporter.useValue();
    ValueID callee = call.opcode == Opcode::SuperConstruct ? call.parentOperand() : call.calleeOperand();

    CalleeCandidates candidates = calleeCandidates(program, context.inputs.flow, context.function, callee, call.callees(), call.accessSite());
    if (candidates.isProven())
        return;
    if (context.exceeds(LimitID::MaxDispatchCases, candidates.predicted.size(), reporter.use()))
        return;

    bool constructs = call.opcode != Opcode::Call;
    for (FunctionID member : candidates.predicted) {
        if (!(constructs ? canConstructDirectly(program, member) : mayBeCalledDirectly(program, member)))
            continue;
        Value test = makeClosureTest(callee, member);
        if (reporter.findEarlierTest(test) == EarlierTest::Failed)
            continue;
        reporter.report({ test }, effectsOfTestedCallee(reporter, member));
        return;
    }
}

// f.apply(t, list) is a list call once the list is known to be an array.
static void reportForwardedListTest(TestSiteReporter& reporter)
{
    FunctionPassContext& context = reporter.context();
    const Value& call = reporter.useValue();
    if (call.builtinCallForm() != BuiltinCallForm::Whole || !reporter.typeOf(reporter.use()).isAnything())
        return;
    std::optional<ForwardedCallPlan> plan = planForwardedCall(context.program, context.function, builtinCallOf(context.program, call));
    if (!plan || !plan->list || isAlwaysArray(reporter.typeOf(plan->list)))
        return;

    Type expected = reporter.predictedPartOf(plan->list).value_or(reporter.typeOf(plan->list));
    if (!expected.anyObject && !expected.findClassPart(BuiltinClasses::Array))
        return;
    reporter.report({ makeTypeTest(plan->list, context.types.intern(Type::builtinClass(BuiltinClasses::Array))) }, Effects::unknownCode());
}

// While nobody has stored to the prototype property of the one class object, it holds the prototype object of the class, and that is what the Structure of an allocation site names.
static Vector<Value, 2> testsOfOwnConstructPrototype(const FunctionPassContext& context, ValueID prototype, LayoutID allocated)
{
    const Program& program = context.program;
    const Value& read = context.function.at(prototype);
    if (read.opcode != Opcode::GetConstructPrototype || read.layout() != allocated)
        return { };
    // The read of the class object's own prototype takes a function: new.target of a body that native code may enter is undefined or the function.
    const Type& newTarget = program.typeOf(context.function, read.newTargetOperand());
    if (!newTarget.isObject() || classOfClassObject(program, newTarget) != allocated)
        return { };

    Vector<Value, 2> tests;
    tests.append(makeConditionTest({ MemberCondition::Kind::MemberUnchanged, allocated, ClassSide::Static, program.keyOf(BuiltinNames::prototype) }));
    if (!isEvaluatedOnce(program, program.at(allocated)))
        tests.append(makeConditionTest({ MemberCondition::Kind::ClassEvaluatedOnce, allocated, ClassSide::Prototype, { } }));
    return tests;
}

static void reportConstructPrototypeTests(TestSiteReporter& reporter)
{
    Vector<Value, 2> tests = testsOfOwnConstructPrototype(reporter.context(), reporter.use(), reporter.useValue().layout());
    if (!tests.isEmpty())
        reporter.report(tests.span(), declaredBoundOf(Opcode::GetClassPrototype));
}

static void reportAllocationUnderConstructPrototypeTests(TestSiteReporter& reporter)
{
    const Value& allocation = reporter.useValue();
    if (!allocation.prototypeOperand())
        return;
    Vector<Value, 2> tests = testsOfOwnConstructPrototype(reporter.context(), allocation.prototypeOperand(), allocation.layout());
    if (!tests.isEmpty())
        reporter.report(tests.span(), reporter.context().analyses.effectsOf(reporter.use()).normal);
}

static void reportInstanceOfTests(TestSiteReporter& reporter)
{
    for (const TestedInstanceOf& tested : planTestedInstanceOf(reporter.context(), reporter.useValue())) {
        bool hasFailed = std::ranges::any_of(tested.tests.tests, [&](const Value& test) {
            return reporter.findEarlierTest(test) == EarlierTest::Failed;
        });
        if (hasFailed)
            continue;
        reporter.report(tested.tests.tests.span(), tested.walksChain ? declaredBoundOf(Opcode::HasInPrototypeChain) : Effects::none());
        return;
    }
}

static void becomeWalkOfChain(TestSiteRewriter& rewriter)
{
    const Value& instanceOf = rewriter.useValue();
    ValueID subject = instanceOf.valueOperand();
    Value read = IRBuilder::make(Opcode::GetClassPrototype, { instanceOf.constructorOperand() });
    read.setLayout(classTestedBy(rewriter.program(), rewriter.function(), instanceOf));
    ValueID prototype = rewriter.insertBeforeUse(WTF::move(read));

    Value& walk = rewriter.useValue();
    walk.become(Opcode::HasInPrototypeChain);
    walk.setValueOperand(subject);
    walk.setPrototypeOperand(prototype);
}

static void rewriteTestedInstanceOf(TestSiteRewriter& rewriter)
{
    const Program& program = rewriter.program();
    Value& use = rewriter.useValue();
    LayoutList testedLayouts;
    std::optional<LayoutList> instances = instancesAmongTestedLayouts(program, rewriter.function(), use, rewriter.tests(), testedLayouts);
    if (!instances) {
        becomeWalkOfChain(rewriter);
        return;
    }
    if (instances->isEmpty() || instances->size() == testedLayouts.size()) {
        use.become(Opcode::Boolean);
        use.setBoolean(!instances->isEmpty());
        return;
    }

    ValueID subject = use.valueOperand();
    Origin origin = use.origin();
    switch (layoutTestKindFor(program, instances->span())) {
    case LayoutTestKind::Type:
        use = makeTypeTest(subject, rewriter.types().intern(Type::forLayouts(instances->span(), rewriter.types())));
        break;
    case LayoutTestKind::Shape:
        use = makeShapeTest(subject, rewriter.types().completeLayoutSet(instances->span()));
        break;
    }
    use.setOrigin(origin);
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
            reportConstructPrototypeTests(reporter);
            break;
        case Opcode::InstanceOf:
            reportInstanceOfTests(reporter);
            break;
        case Opcode::NewObject:
            reportAllocationUnderConstructPrototypeTests(reporter);
            break;
        default:
            break;
        }
    });
}

static bool tryBecomeDirectCall(const TestedCallRewrite& rewrite, const DirectCallRequest& request)
{
    std::optional<Value> call = tryMakeDirectCall(rewrite.insertion, request);
    if (!call)
        return false;
    Value& use = rewrite.rewriter.useValue();
    call->setOrigin(use.origin());
    use = WTF::move(*call);
    return true;
}

static void rewriteTestedMemberCall(const TestedCallRewrite& rewrite)
{
    FunctionPassContext& context = rewrite.rewriter.context();
    std::optional<TestedMemberUse> use = planTestedMemberCall(context, rewrite.site, rewrite.established);
    RELEASE_ASSERT(use);

    const Value& call = rewrite.rewriter.useValue();
    CallArm arm = createDirectCallArm(context.program, context.inputs.flow, use->site.plan.member.method, call.callees());
    ValueID thisValue = use->operands.receiver;
    Vector<ValueID, 1> captures;
    if (use->site.plan.capturesReceiver)
        captures.append(thisValue);
    Vector<ValueID, 8> arguments(call.variadicOperands());

    bool didBecomeDirectCall = tryBecomeDirectCall(rewrite, {
        .callee = arm.callee,
        .captures = captures.span(),
        .thisValue = thisValue,
        .arguments = arguments.span(),
        .accessSite = call.accessSite(),
        .resultCheck = arm.resultCheck,
        .isNeverInlined = call.isNeverInlined(),
        .isDemotedTailCall = call.isDemotedTailCall(),
    });
    RELEASE_ASSERT(didBecomeDirectCall);
}

static void rewriteTestedAccess(const TestedCallRewrite& rewrite)
{
    FunctionPassContext& context = rewrite.rewriter.context();
    std::optional<TestedMemberUse> use = planTestedAccess(context, rewrite.site, rewrite.established);
    RELEASE_ASSERT(use);

    if (use->site.plan.form == AccessForm::Method) {
        use->site.plan.level = KnowledgeLevel::Predicted;
        AccessRewriter(context, rewrite.insertion.insert).rewrite(use->site, use->operands);
        return;
    }

    FunctionID accessor = use->site.plan.member.accessorFor(use->site.query.access);
    CallArm arm = createDirectCallArm(context.program, context.inputs.flow, accessor, { });
    Vector<ValueID, 1> arguments;
    if (use->operands.stored)
        arguments.append(use->operands.stored);

    bool didBecomeDirectCall = tryBecomeDirectCall(rewrite, {
        .callee = arm.callee,
        .thisValue = use->operands.receiver,
        .arguments = arguments.span(),
        .accessSite = use->stamp.accessSite,
        .resultCheck = arm.resultCheck,
    });
    RELEASE_ASSERT(didBecomeDirectCall);
}

static void rewriteTestedSuperAccess(const TestedCallRewrite& rewrite)
{
    FunctionPassContext& context = rewrite.rewriter.context();
    Value& use = rewrite.rewriter.useValue();
    TestedSuperAccess access = planTestedSuperAccess(context, use);

    switch (access.form) {
    case SuperAccessForm::ByName:
        RELEASE_ASSERT_NOT_REACHED();
    case SuperAccessForm::MethodClosure:
        use.become(Opcode::NewClosure);
        use.setFunction(access.function);
        return;
    case SuperAccessForm::AccessorCall: {
        CallArm arm = createDirectCallArm(context.program, context.inputs.flow, access.function, { });
        Vector<ValueID, 1> arguments;
        if (use.opcode == Opcode::SuperSet)
            arguments.append(use.valueOperand());

        bool didBecomeDirectCall = tryBecomeDirectCall(rewrite, {
            .callee = arm.callee,
            .thisValue = use.receiverOperand(),
            .arguments = arguments.span(),
            .accessSite = use.accessSite(),
            .resultCheck = arm.resultCheck,
        });
        RELEASE_ASSERT(didBecomeDirectCall);
        return;
    }
    case SuperAccessForm::Undefined:
        use.become(Opcode::Undefined);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool tryCallTestedCallee(const TestedCallRewrite& rewrite)
{
    FunctionPassContext& context = rewrite.rewriter.context();
    const Value& call = rewrite.rewriter.useValue();
    CallArm arm = createDirectCallArm(context.program, context.inputs.flow, rewrite.rewriter.tests()[0].function(), call.callees());
    if (arm.resultCheck == ResultCheck::ByCallers)
        return false;
    ValueID callee = call.calleeOperand();
    ValueID thisValue = call.thisValueOperand();
    AccessSiteID accessSite = call.accessSite();
    bool isNeverInlined = call.isNeverInlined();
    bool isDemotedTailCall = call.isDemotedTailCall();
    Vector<ValueID, 8> arguments(call.variadicOperands());

    Vector<ValueID, 8> captures = insertCaptures(rewrite.insertion, arm.closureFunction, callee);
    return tryBecomeDirectCall(rewrite, {
        .callee = arm.callee,
        .captures = captures.span(),
        .thisValue = thisValue,
        .calleeValue = context.program.at(arm.callee).has(FunctionFlags::TakesCallee) ? callee : ValueID(),
        .arguments = arguments.span(),
        .accessSite = accessSite,
        .resultCheck = arm.resultCheck,
        .isNeverInlined = isNeverInlined,
        .isDemotedTailCall = isDemotedTailCall,
    });
}

static void finishTestedCallRewrite(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    Value& use = rewriter.useValue();
    use.type = resultTypeOf(context.program, context.function, use, context.types);
    use.setWritten(writtenTypeOf(context.program, context.function, use, context.types));
    use.setPrediction(predictionOf(context.program, context.function, use, context.types));

    context.build.statistics().count(StatisticID::CallsGuarded);
}

void rewriteTestedCall(TestSiteRewriter& rewriter)
{
    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeUse = [&](auto&& value) {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertValue insert = insertBeforeUse;
    ValueInsertion insertion { rewriter.builder().context(), rewriter.function(), rewriter.types(), rewriter.useValue().origin(), insert };

    std::optional<ReceiverCandidates> established = receiversEstablishedBy(rewriter.program(), rewriter.tests());
    ProgramPoint point = rewriter.point();
    TestedCallRewrite rewrite { rewriter, insertion, { point.block, point.index, rewriter.use() }, established ? &*established : nullptr };

    switch (rewriter.useValue().opcode) {
    case Opcode::Call:
        if (rewriter.tests()[0].opcode != Opcode::IsClosureOf)
            rewriteTestedMemberCall(rewrite);
        else if (!tryCallTestedCallee(rewrite))
            return;
        break;
    case Opcode::Construct:
    case Opcode::SuperConstruct:
    case Opcode::CallBuiltin:
        return;
    case Opcode::PropGet:
    case Opcode::PropSet:
        rewriteTestedAccess(rewrite);
        break;
    case Opcode::SuperGet:
    case Opcode::SuperSet:
        rewriteTestedSuperAccess(rewrite);
        break;
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
    finishTestedCallRewrite(rewriter);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
