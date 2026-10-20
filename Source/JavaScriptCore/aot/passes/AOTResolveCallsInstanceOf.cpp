/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTResultRules.h"
#include "AOTTypeTestSite.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::TypeTest::value == testedOperandIndex);
static_assert(OperandIndex::ShapeTest::object == testedOperandIndex);

static bool usesOrdinaryHasInstance(const PropertyAccessContext& context, LayoutID tested)
{
    PropertyKeyID key = context.program.keyOf(BuiltinNames::hasInstanceSymbol);
    MemberAnswer answer = findMember(context, { MemberStart::ClassObject, tested, key, Access::Read });
    return answer.kind == MemberKind::Builtin && answer.needsNoTest() && answer.builtin == Builtins::FunctionPrototypeSymbolHasInstance;
}

static MemberMutability classPrototypeLinkOf(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Instance:
        return layout.constructor ? layout.classFlow.prototypeLink : MemberMutability::Unknown;
    case LayoutKind::BuiltinResult: {
        bool inheritsFromObjectPrototype = layout.builtinLayout && !program.builtins().at(layout.builtinLayout).isTuple;
        return inheritsFromObjectPrototype ? MemberMutability::Fixed : MemberMutability::Unknown;
    }
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::ModuleNamespace:
        return MemberMutability::Fixed;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return MemberMutability::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasDefinedChain(const Program& program, LayoutID layout, LayoutID tested)
{
    if (!receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype))
        return false;

    LayoutID current = layout;
    while (current != tested) {
        const Layout& passed = program.at(current);
        if (classPrototypeLinkOf(program, passed) != MemberMutability::Fixed)
            return false;
        switch (passed.flow.baseState) {
        case BaseState::Layout:
            current = passed.flow.base;
            break;
        case BaseState::None:
            current = passed.classFlow.prototypeBase;
            if (!current)
                return true;
            break;
        // No record says that the prototype object of a built-in class keeps its own prototype.
        case BaseState::BuiltinClass:
        case BaseState::Unknown:
            return false;
        }
    }
    return true;
}

static bool hasProvenAncestry(const Program& program, LayoutID layout, LayoutID tested, Ancestry ancestry)
{
    RELEASE_ASSERT(ancestry != Ancestry::Unknown);
    return ancestryOf(program, layout, tested) == ancestry && hasDefinedChain(program, layout, tested);
}

static LayoutList layoutsToConsider(const Program& program, const ReceiverCandidates& subject, const Type& subjectType, LayoutID tested)
{
    if (subject.isProven())
        return subject.proven;
    if (!subjectType.anyObject)
        return { };
    LayoutList considered(program.at(tested).flow.members.span());
    considered.add(tested);
    return considered;
}

static LayoutList findInstanceLayouts(const Program& program, const LayoutList& considered, LayoutID tested)
{
    LayoutList instances;
    for (LayoutID layout : considered) {
        if (program.at(layout).flow.allocation == AllocationState::NeverAllocated)
            continue;
        if (hasProvenAncestry(program, layout, tested, Ancestry::Descendant))
            instances.add(layout);
    }
    return instances;
}

static bool isObjectOutsideLayouts(UnionPartKind kind)
{
    switch (kind) {
    case UnionPartKind::OtherObjects:
    case UnionPartKind::BuiltinClass:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return true;
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
    case UnionPartKind::Layouts:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasObjectsOutsideLayouts(const Type& type)
{
    return std::ranges::any_of(unionPartsOf(type), [](const UnionPart& part) {
        return isObjectOutsideLayouts(part.kind);
    });
}

static bool mayBelongToOtherInstance(const PropertyAccessContext& context, const ReceiverCandidates& subject)
{
    return !subject.isProven() && instanceCountOf(context) != InstanceCount::AtMostOne;
}

enum class ChainEnd : uint8_t { Unknown, MeetsClass, MissesClass };

// Collects what has to hold for the prototype objects of the classes above the layout's own to be linked as the classes were defined, up to the tested class or to the end of the chain.
// Object.prototype, where a class without a base ends, inherits from nothing for good. An object that was assigned as the prototype of a function may be the prototype of another one too,
// so a chain with such an object in it, or a tested function with one, is not known to miss.
static ChainEnd followChain(const Program& program, LayoutID layout, LayoutID tested, MemberConditions& conditions)
{
    if (program.at(layout).kind != LayoutKind::Instance)
        return ChainEnd::Unknown;
    bool mayShareObject = program.at(tested).classFlow.hasAssignedPrototype;
    LayoutID current = declaredLayoutOf(program, layout);
    for (size_t unvisitedClasses = program.layouts.size(); unvisitedClasses; --unvisitedClasses) {
        if (current == tested)
            return ChainEnd::MeetsClass;
        mayShareObject |= program.at(current).classFlow.hasAssignedPrototype;
        NextHolder next = nextHolderOf(program, current, ClassSide::Prototype, conditions);
        switch (next.step) {
        case ChainStep::CompiledClass:
            current = next.layout;
            break;
        case ChainStep::NoBase:
            return mayShareObject ? ChainEnd::Unknown : ChainEnd::MissesClass;
        case ChainStep::BuiltinClass:
        case ChainStep::Unknown:
            return ChainEnd::Unknown;
        }
    }
    return ChainEnd::Unknown;
}

LayoutID classTestedBy(const Program& program, const Function& function, const Value& instanceOf)
{
    const Type& constructorType = program.typeOf(function, instanceOf.operand(OperandIndex::InstanceOf::constructor));
    return constructorType.isObject() ? classOfClassObject(program, constructorType) : LayoutID();
}

// While these hold, C finds the ordinary Symbol.hasInstance and its prototype property holds the prototype object of its class.
static std::optional<MemberConditions> conditionsOfClassObject(const PropertyAccessContext& context, LayoutID tested)
{
    const Program& program = context.program;
    MemberAnswer hasInstance = findMember(context, { MemberStart::ClassObject, tested, program.keyOf(BuiltinNames::hasInstanceSymbol), Access::Read });
    if (hasInstance.kind != MemberKind::Builtin || hasInstance.builtin != Builtins::FunctionPrototypeSymbolHasInstance || !hasInstance.receiverProofs.isEmpty())
        return std::nullopt;

    MemberConditions conditions = WTF::move(hasInstance.conditions);
    const ClassFlow& classFlow = program.at(tested).classFlow;
    if (classFlow.prototypeProperty != MemberMutability::Fixed && !classFlow.hasAssignedPrototype)
        addCondition(conditions, { MemberCondition::Kind::MemberUnchanged, tested, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    if (!isEvaluatedOnce(program, program.at(tested)))
        addCondition(conditions, { MemberCondition::Kind::ClassEvaluatedOnce, tested, ClassSide::Prototype, { } });
    return conditions;
}

Vector<TestedInstanceOf, 2> planTestedInstanceOf(FunctionPassContext& context, const Value& instanceOf)
{
    const Program& program = context.program;
    PropertyAccessContext accessContext { program, &context.inputs.flow };
    LayoutID tested = classTestedBy(program, context.function, instanceOf);
    if (!tested)
        return { };
    std::optional<MemberConditions> conditionsOfClass = conditionsOfClassObject(accessContext, tested);
    if (!conditionsOfClass)
        return { };

    Vector<TestedInstanceOf, 2> forms;
    auto appendForm = [&](bool walksChain, AccessTests&& tests) {
        if (!walksChain && !tests.tests.isEmpty())
            forms.append(TestedInstanceOf { walksChain, WTF::move(tests) });
    };

    ValueID subject = instanceOf.operand(OperandIndex::InstanceOf::value);
    ReceiverCandidates candidates = receiverCandidates(program, context.function, subject, { });
    PropertyAccessPlan plan;
    plan.conditions = *conditionsOfClass;
    bool leavesOutLayouts = false;
    if (!candidates.isProven() || !candidates.hasOtherParts) {
        for (LayoutID layout : candidates.isProven() ? candidates.proven : candidates.predicted) {
            MemberConditions conditionsOfChain;
            if (followChain(program, layout, tested, conditionsOfChain) == ChainEnd::Unknown) {
                leavesOutLayouts = true;
                continue;
            }
            plan.receivers.add(layout);
            for (const MemberCondition& condition : conditionsOfChain)
                addCondition(plan.conditions, condition);
            if (!receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype))
                plan.requiredProofs.add(Proof::PrototypeLinkUnchanged);
        }
    }
    if (!plan.receivers.isEmpty()) {
        // undefined and null are instances of nothing, and the tests of an object take an object.
        if (!candidates.isProven() || candidates.mayBeNullish || leavesOutLayouts)
            plan.receiverTest = layoutTestKindFor(program, plan.receivers.span()) == LayoutTestKind::Type ? ReceiverTest::Type : ReceiverTest::Shape;
        if (mayBelongToOtherInstance(accessContext, candidates))
            plan.requiredProofs.add(Proof::OwnInstance);
        appendForm(false, createAccessTests(context, plan, { subject, { }, { } }));
    }

    AccessTests testsOfClass;
    for (const MemberCondition& condition : *conditionsOfClass)
        testsOfClass.tests.append(makeConditionTest(condition));
    appendForm(true, WTF::move(testsOfClass));
    return forms;
}

std::optional<LayoutList> instancesAmongTestedLayouts(const Program& program, const Function& function, const Value& instanceOf, std::span<const Value> tests, LayoutList& testedLayouts)
{
    // The tests of the class object are about no value.
    bool testsValue = std::ranges::any_of(tests, [](const Value& test) {
        return test.numberOfOperands();
    });
    if (!testsValue)
        return std::nullopt;

    std::optional<ReceiverCandidates> established = receiversEstablishedBy(program, tests);
    if (!established)
        established = receiverCandidates(program, function, instanceOf.operand(OperandIndex::InstanceOf::value), { });
    RELEASE_ASSERT(established->isProven());
    testedLayouts = established->proven;

    LayoutID tested = classTestedBy(program, function, instanceOf);
    LayoutList instances;
    for (LayoutID layout : testedLayouts) {
        MemberConditions conditions;
        switch (followChain(program, layout, tested, conditions)) {
        case ChainEnd::MeetsClass:
            instances.add(layout);
            break;
        case ChainEnd::MissesClass:
            break;
        case ChainEnd::Unknown:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return instances;
}

static Value makeInstanceTest(TypeArena& types, const InstanceOfPlan& plan, ValueID subject)
{
    switch (plan.test) {
    case LayoutTestKind::Type:
        return makeTypeTest(subject, types.intern(Type::forLayoutSet(plan.instances)));
    case LayoutTestKind::Shape:
        return makeShapeTest(subject, plan.instances);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID emitObjectTest(SiteExpansion& expansion, ValueID subject)
{
    if (!expansion.context().program.typeOf(expansion.function(), subject).primitives)
        return subject;

    IRBuilder& builder = expansion.builder();
    BlockFrequency frequency = expansion.frequency();
    ValueID isObject = builder.appendTypeTest(subject, Type::object());
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isObject, testedOperandIndex, frequency, frequency);
    builder.moveToEndOf(branch.notTaken);
    expansion.finishArm(builder.appendBoolean(false));
    builder.moveToEndOf(branch.taken);
    return branch.provenOnTaken;
}

static void finishArmWithOriginal(SiteExpansion& expansion, ValueID subject)
{
    expansion.finishArm(expansion.appendOriginal({ OperandIndex::InstanceOf::value, subject, { }, { } }));
}

static bool namesGuard(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void overwriteValue(FunctionPassContext& context, ValueID id, Value&& replacement)
{
    Value& value = context.function.at(id);
    replacement.setOrigin(value.origin());
    replacement.type = resultTypeOf(context.program, context.function, replacement, context.types);
    value = WTF::move(replacement);
    context.analyses.didChangeValues();
}

PlanOrDecline<InstanceOfPlan> CallResolver::planInstanceOf(const CallSite& site)
{
    const Value& instanceOf = m_function.at(site.position.value);
    const Type& constructorType = typeOf(instanceOf.operand(OperandIndex::InstanceOf::constructor));
    LayoutID tested = classOfClassObject(m_program, constructorType);
    if (!tested || constructorType.mayBeUndefinedOrNull())
        return std::unexpected(CallDeclineReason::None);

    const ClassFlow& classFlow = m_program.at(tested).classFlow;
    bool hasOnePrototypeObject = classFlow.prototypeProperty == MemberMutability::Fixed || classFlow.hasAssignedPrototype;
    if (classFlow.evaluations != ExecutionCount::AtMostOnce || !hasOnePrototypeObject)
        return std::unexpected(CallDeclineReason::UnknownMember);
    if (!usesOrdinaryHasInstance(accessContext(), tested))
        return std::unexpected(CallDeclineReason::UnknownMember);

    InstanceOfPlan plan;
    plan.subject = instanceOf.operand(OperandIndex::InstanceOf::value);
    const Type& subjectType = typeOf(plan.subject);
    ReceiverCandidates subject = receiverCandidates(m_program, m_function, plan.subject, { });
    LayoutList instances = findInstanceLayouts(m_program, layoutsToConsider(m_program, subject, subjectType, tested), tested);

    plan.failedTestMeansFalse = subject.isProven() && !hasObjectsOutsideLayouts(subjectType) && std::ranges::all_of(subject.proven, [&](LayoutID layout) {
        return instances.contains(layout) || hasProvenAncestry(m_program, layout, tested, Ancestry::Unrelated);
    });
    plan.isAlwaysFalse = plan.failedTestMeansFalse && instances.isEmpty();
    if (plan.isAlwaysFalse)
        return plan;
    if (instances.isEmpty())
        return std::unexpected(CallDeclineReason::UnknownMember);

    plan.test = subject.isProven() ? LayoutTestKind::Type : layoutTestKindFor(m_program, instances.span());
    plan.instances = m_context.types.completeLayoutSet(instances.span());
    if (plan.failedTestMeansFalse)
        return plan;

    FunctionAnalyses& analyses = m_context.analyses;
    BlockID block = site.position.block;
    if (analyses.findEarlierTest(makeInstanceTest(m_context.types, plan, plan.subject), block) == EarlierTest::Failed)
        return std::unexpected(CallDeclineReason::None);
    if (mayBelongToOtherInstance(accessContext(), subject) && analyses.findEarlierTest(makeObjectTest(Proof::OwnInstance, plan.subject, { }), block) == EarlierTest::Failed)
        return std::unexpected(CallDeclineReason::None);
    return plan;
}

void CallResolver::rewriteInstanceOf(const CallSite& site, const InstanceOfPlan& plan)
{
    if (plan.isAlwaysFalse) {
        Value answer = IRBuilder::make(Opcode::Boolean);
        answer.setBoolean(false);
        overwriteValue(m_context, site.position.value, WTF::move(answer));
        return;
    }

    if (plan.failedTestMeansFalse) {
        overwriteValue(m_context, site.position.value, makeInstanceTest(m_context.types, plan, plan.subject));
        passProofOnBranches(site.position.value, plan.subject);
        return;
    }

    bool isTypeTest = plan.test == LayoutTestKind::Type;
    bool testsOwnInstance = mayBelongToOtherInstance(accessContext(), receiverCandidates(m_program, m_function, plan.subject, { }));

    m_expansion.begin(site.position);
    BlockFrequency frequency = m_expansion.frequency();
    ValueID subject = testsOwnInstance && !isTypeTest ? emitObjectTest(m_expansion, plan.subject) : plan.subject;

    ValueID test = m_expansion.append(makeInstanceTest(m_context.types, plan, subject));
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequency);
    if (testsOwnInstance) {
        // The copy below names the subject as it was before the layout test: a type that lists layouts counts as a proof of the instance.
        ValueID isOwnObject = m_expansion.append(makeObjectTest(Proof::OwnInstance, isTypeTest ? branch.provenOnTaken : subject, { }));
        IRBuilder::BranchBlocks ownership = m_builder.emitBranch(isOwnObject, frequency, BlockFrequency::Unlikely);
        m_expansion.finishArm(m_builder.appendBoolean(true));
        m_builder.moveToEndOf(ownership.notTaken);
        finishArmWithOriginal(m_expansion, subject);
    } else
        m_expansion.finishArm(m_builder.appendBoolean(true));

    m_builder.moveToEndOf(branch.notTaken);
    finishArmWithOriginal(m_expansion, isTypeTest ? branch.provenOnNotTaken : subject);
    m_expansion.end();
}

void CallResolver::passProofOnBranches(ValueID test, ValueID subject)
{
    bool narrowsSubject = provesForever(provesOf(m_function.at(test).opcode));
    uint32_t numberOfProvingTargets = narrowsSubject ? 2 : 1;
    m_builder.setOrigin(m_function.at(test).origin());

    Vector<ValueID, 2> proofs;
    size_t numberOfBlocks = m_function.blocks.size();
    for (size_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != test)
            continue;
        for (uint32_t targetIndex = 0; targetIndex < numberOfProvingTargets; ++targetIndex) {
            Edge edge { block, targetIndex };
            m_builder.splitEdge(edge);
            proofs.append(m_builder.addEdgeResult(edge, testedOperandIndex));
        }
    }
    if (!narrowsSubject || proofs.isEmpty())
        return;

    FunctionAnalyses& analyses = m_context.analyses;
    for (const Use& use : analyses.useLists().usesOf(subject)) {
        if (namesGuard(m_function, use))
            continue;
        size_t proofIndex = proofs.findIf([&](ValueID proof) {
            return analyses.dominates(proof, use);
        });
        if (proofIndex != notFound)
            replaceUse(m_function, use, proofs[proofIndex]);
    }
    analyses.didChangeValues();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
