/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLayout.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTResultRules.h"
#include "AOTTypeTestSite.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::TypeTest::value == testedOperandIndex);

static constexpr uint32_t numberOfBranchTargets = 2;

namespace {

enum class ChainEnd : uint8_t { Unknown, MeetsClass, MissesClass };

// What says that an object still inherits from the prototype object that its layout names: a bit of its class, which the run time clears when any object of the class gets another prototype, or a test of the object itself.
enum class PrototypeKnownBy : uint8_t { BitOfClass, TestOfObject };

struct ChainAnswers {
    // The layouts whose objects are instances, or are none, while the conditions hold.
    LayoutList decided;
    LayoutList instances;
    MemberConditions conditions;
    bool leavesOutLayouts { false };
    bool testsObjects { false };
};

} // anonymous namespace

static bool inheritsFromObjectPrototype(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return true;
    case LayoutKind::BuiltinResult:
        // A tuple is an array, and no record says that the prototype object of a built-in class keeps its own prototype.
        return layout.builtinLayout && !program.builtins().at(layout.builtinLayout).isTuple;
    case LayoutKind::Instance:
    case LayoutKind::ModuleNamespace:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Collects what has to hold for the prototype objects of the classes above the layout's own to be linked as the classes were defined, up to the tested class or to the end of the chain.
static ChainEnd followChain(const Program& program, LayoutID layout, LayoutID tested, MemberConditions& conditions)
{
    const Layout& start = program.at(layout);
    if (start.kind == LayoutKind::ModuleNamespace)
        return ChainEnd::MissesClass;

    bool mayShareObject = program.at(tested).classFlow.hasAssignedPrototype;
    if (inheritsFromObjectPrototype(program, start))
        return mayShareObject ? ChainEnd::Unknown : ChainEnd::MissesClass;
    if (start.kind != LayoutKind::Instance)
        return ChainEnd::Unknown;

    LayoutID current = declaredLayoutOf(program, layout);
    for (size_t unvisitedClasses = program.layouts.size(); unvisitedClasses; --unvisitedClasses) {
        if (current == tested)
            return ChainEnd::MeetsClass;
        const Layout& passed = program.at(current);
        if (passed.kind != LayoutKind::Instance || !passed.constructor)
            return ChainEnd::Unknown;
        mayShareObject |= passed.classFlow.hasAssignedPrototype;
        NextHolder next = nextHolderOf(program, current, ClassSide::Prototype, conditions);
        switch (next.step) {
        case ChainStep::CompiledClass:
            current = next.layout;
            // `function Meter() {}`, `Meter.prototype = first`, `class Fine extends Meter {}`, `Meter.prototype = second`: the class below is linked to the object that the property held when it was defined.
            if (program.at(current).has(LayoutFlags::IsConstructorFunction) && program.at(current).classFlow.prototypeProperty != MemberMutability::Fixed)
                return ChainEnd::Unknown;
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

static bool neverChangesPrototype(const Program& program, LayoutID layout)
{
    return program.at(layout).kind == LayoutKind::ModuleNamespace || receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype);
}

static ChainAnswers followChains(const Program& program, const LayoutList& layouts, LayoutID tested, PrototypeKnownBy knownBy)
{
    ChainAnswers answers;
    for (LayoutID layout : layouts) {
        if (program.at(layout).flow.allocation == AllocationState::NeverAllocated)
            continue;

        MemberConditions conditions;
        ChainEnd end = followChain(program, layout, tested, conditions);
        bool testsObject = false;
        // The run time defines the class of a constructor function at the first construction that is carried out, with whatever the function has as its prototype property then.
        // Where nothing replaces that property it is the one object, and the bits of the tested class, which is the function's own or one above it, say that the class was defined.
        // That the chain misses a class no bit says: another function may have been given the object and been constructed first, and then this one has no class.
        const Layout* maker = program.at(layout).kind == LayoutKind::Instance ? &program.at(declaredLayoutOf(program, layout)) : nullptr;
        bool keepsPrototypeObject = maker && maker->classFlow.prototypeProperty == MemberMutability::Fixed && end == ChainEnd::MeetsClass;
        bool isMadeByConstructorFunction = maker && maker->has(LayoutFlags::IsConstructorFunction) && !keepsPrototypeObject;
        if (end != ChainEnd::Unknown && (isMadeByConstructorFunction || !neverChangesPrototype(program, layout))) {
            switch (knownBy) {
            case PrototypeKnownBy::BitOfClass:
                // Only a class has bits, and they follow what happens to compiled objects.
                if (builtinBaseClassOf(program, layout))
                    testsObject = true;
                else if (program.at(layout).kind == LayoutKind::Instance && !isMadeByConstructorFunction)
                    addCondition(conditions, { MemberCondition::Kind::InstancePrototypeUnchanged, declaredLayoutOf(program, layout), ClassSide::Prototype, { } });
                else
                    end = ChainEnd::Unknown;
                break;
            case PrototypeKnownBy::TestOfObject:
                testsObject = true;
                break;
            }
        }
        if (end == ChainEnd::Unknown) {
            answers.leavesOutLayouts = true;
            continue;
        }

        answers.decided.add(layout);
        if (end == ChainEnd::MeetsClass)
            answers.instances.add(layout);
        addConditions(answers.conditions, conditions.span());
        answers.testsObjects |= testsObject;
    }
    return answers;
}

static LayoutList layoutsToConsider(const Program& program, const ReceiverCandidates& subject, const Type& subjectType, LayoutID tested)
{
    if (subject.isProven())
        return subject.proven;
    // Objects that no layout describes were converted to the type of the family, so its layouts are only expected.
    if (hasMembersWithoutLayout(program, subjectType))
        return subject.predicted;
    if (!subjectType.anyObject)
        return { };

    // Nothing is known of the object. The layouts of the class and of what is below it are worth one test.
    LayoutList considered(program.layoutsBelow(tested));
    considered.unite(LayoutList(program.at(program.declaredMembersOf(tested)).members.span()));
    considered.add(tested);
    return considered;
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

static bool hasObjectsOutsideLayouts(const Program& program, const Type& type)
{
    return hasMembersWithoutLayout(program, type) || std::ranges::any_of(unionPartsOf(type), [](const UnionPart& part) {
        return isObjectOutsideLayouts(part.kind);
    });
}

static bool mayBelongToOtherInstance(const PropertyAccessContext& context, const ReceiverCandidates& subject)
{
    return !subject.isProven() && instanceCountOf(context) != InstanceCount::AtMostOne;
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
    if (guardsAreNeverSet(program, program.at(tested)))
        return std::nullopt;
    MemberAnswer hasInstance = findMember(context, { MemberStart::ClassObject, tested, program.keyOf(BuiltinNames::hasInstanceSymbol), Access::Read });
    if (hasInstance.kind != MemberKind::Builtin || hasInstance.builtin != Builtins::FunctionPrototypeSymbolHasInstance || !hasInstance.receiverProofs.isEmpty())
        return std::nullopt;

    MemberConditions conditions = WTF::move(hasInstance.conditions);
    const ClassFlow& classFlow = program.at(tested).classFlow;
    // No code can change the property of a class.
    if (program.at(tested).has(LayoutFlags::IsConstructorFunction) && classFlow.prototypeProperty != MemberMutability::Fixed && !classFlow.hasAssignedPrototype)
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
        if (!tests.tests.isEmpty())
            forms.append(TestedInstanceOf { walksChain, WTF::move(tests) });
    };

    ValueID subject = instanceOf.operand(OperandIndex::InstanceOf::value);
    ReceiverCandidates candidates = receiverCandidates(program, context.function, subject);
    auto appendFormOfLayouts = [&](PrototypeKnownBy knownBy) {
        ChainAnswers answers = followChains(program, candidates.isProven() ? candidates.proven : candidates.predicted, tested, knownBy);
        // Where no object has to be asked, the bits have said it all.
        if (answers.decided.isEmpty() || (knownBy == PrototypeKnownBy::TestOfObject && !answers.testsObjects))
            return;

        PropertyAccessPlan plan;
        plan.conditions = *conditionsOfClass;
        addConditions(plan.conditions, answers.conditions.span());
        plan.receivers = answers.decided;
        // undefined and null are instances of nothing, and the tests of an object take an object.
        if (!candidates.isProven() || candidates.mayBeNullish || answers.leavesOutLayouts)
            plan.receiverTest = ReceiverTest::Type;
        if (mayBelongToOtherInstance(accessContext, candidates))
            plan.requiredProofs.add(Proof::OwnInstance);
        if (answers.testsObjects)
            plan.requiredProofs.add(Proof::PrototypeLinkUnchanged);
        appendForm(false, createAccessTests(context, plan, { subject, { }, { } }));
    };
    if (!candidates.isProven() || !candidates.hasOtherParts) {
        appendFormOfLayouts(PrototypeKnownBy::BitOfClass);
        appendFormOfLayouts(PrototypeKnownBy::TestOfObject);
    }

    if (!conditionsOfClass->isEmpty()) {
        AccessTests testsOfClass;
        testsOfClass.tests.append(makeConditionsTest(context.types, conditionsOfClass->span()));
        appendForm(true, WTF::move(testsOfClass));
    }
    return forms;
}

std::optional<LayoutList> instancesAmongTestedLayouts(const Program& program, const Function& function, const Value& instanceOf, std::span<const Value> tests, LayoutList& testedLayouts)
{
    std::optional<ReceiverCandidates> established = receiversEstablishedBy(program, tests);
    if (!established)
        established = receiverCandidates(program, function, instanceOf.operand(OperandIndex::InstanceOf::value));
    if (!established->isProven() || established->hasOtherParts || established->mayBeNullish)
        return std::nullopt;

    bool testsObjects = std::ranges::contains(tests, Opcode::HasLayoutPrototype, &Value::opcode);
    PrototypeKnownBy knownBy = testsObjects ? PrototypeKnownBy::TestOfObject : PrototypeKnownBy::BitOfClass;
    ChainAnswers answers = followChains(program, established->proven, classTestedBy(program, function, instanceOf), knownBy);
    // The layouts answer only under the tests that their chains ask for. The tests of the class object are among the tests of every form.
    if (answers.leavesOutLayouts || (answers.testsObjects && !testsObjects) || !testsCoverConditions(tests, answers.conditions.span()))
        return std::nullopt;

    testedLayouts = WTF::move(answers.decided);
    return WTF::move(answers.instances);
}

static Value makeInstanceTest(TypeArena& types, const InstanceOfPlan& plan)
{
    return makeTypeTest(plan.subject, types.intern(Type::forLayoutSet(plan.instances)));
}

// Both lookups on the class object are answered, so what is left of the operation is the walk up the chain of the value.
static void finishArmWithWalk(SiteExpansion& expansion, ValueID subject)
{
    const Value& original = expansion.original();
    Value read = IRBuilder::make(Opcode::GetClassPrototype, { original.operand(OperandIndex::InstanceOf::constructor) });
    read.setLayout(classTestedBy(expansion.context().program, expansion.function(), original));
    ValueID prototype = expansion.append(WTF::move(read));
    expansion.finishArm(expansion.append(IRBuilder::make(Opcode::HasInPrototypeChain, { subject, prototype })));
}

static bool useNamesGuard(const Function& function, const Use& use)
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
    Value& value = context.function.edit(id);
    replacement.setOrigin(value.origin());
    replacement.type = resultTypeOf(context.program, context.function, replacement, context.types);
    value = WTF::move(replacement);
    context.analyses.didChangeValues();
}

static bool isLeftToTestsOfExpectedLayouts(FunctionPassContext& context, const Value& instanceOf, const ReceiverCandidates& subject, BlockID block)
{
    if (subject.isProven() || subject.predicted.isEmpty())
        return false;
    return std::ranges::any_of(planTestedInstanceOf(context, instanceOf), [&](const TestedInstanceOf& form) {
        return !form.walksChain && std::ranges::none_of(form.tests.tests, [&](const Value& test) {
            return context.analyses.findEarlierTest(test, block) == EarlierTest::Failed;
        });
    });
}

PlanOrDecline<InstanceOfPlan> CallResolver::planInstanceOf(const CallSite& site)
{
    const Value& instanceOf = m_function.at(site.position.value);
    LayoutID tested = classTestedBy(m_program, m_function, instanceOf);
    if (!tested)
        return std::unexpected(CallDeclineReason::None);
    std::optional<MemberConditions> conditionsOfClass = conditionsOfClassObject(accessContext(), tested);
    if (!conditionsOfClass)
        return std::unexpected(CallDeclineReason::UnknownMember);

    InstanceOfPlan plan;
    plan.subject = instanceOf.operand(OperandIndex::InstanceOf::value);
    plan.conditions = WTF::move(*conditionsOfClass);
    const Type& subjectType = typeOf(plan.subject);
    ReceiverCandidates subject = receiverCandidates(m_program, m_function, plan.subject);
    if (isLeftToTestsOfExpectedLayouts(m_context, instanceOf, subject, site.position.block))
        return std::unexpected(CallDeclineReason::NeedsTest);

    LayoutList considered = layoutsToConsider(m_program, subject, subjectType, tested);
    bool isOverLimit = m_context.exceeds(LimitID::MaxAccessPlanMembers, considered.size(), site.position.value);
    if (isOverLimit)
        considered.clear();
    ChainAnswers answers = followChains(m_program, considered, tested, PrototypeKnownBy::BitOfClass);

    FunctionAnalyses& analyses = m_context.analyses;
    BlockID block = site.position.block;
    plan.failedTestMeansFalse = !isOverLimit && !answers.leavesOutLayouts && !answers.testsObjects && !hasObjectsOutsideLayouts(m_program, subjectType);
    plan.isAlwaysFalse = plan.failedTestMeansFalse && answers.instances.isEmpty();
    if (!plan.failedTestMeansFalse) {
        // A miss walks the chain, so nothing is asked about the layouts that are no instances.
        answers = followChains(m_program, answers.instances, tested, PrototypeKnownBy::BitOfClass);
    }
    if (!answers.instances.isEmpty())
        plan.instances = m_context.types.completeLayoutSet(answers.instances.span());
    plan.testsPrototypeOfInstance = answers.testsObjects;

    if (plan.instances && !plan.failedTestMeansFalse) {
        bool hasFailed = analyses.findEarlierTest(makeInstanceTest(m_context.types, plan), block) == EarlierTest::Failed
            || (mayBelongToOtherInstance(accessContext(), subject) && analyses.findEarlierTest(makeObjectTest(Proof::OwnInstance, plan.subject, { }), block) == EarlierTest::Failed);
        if (hasFailed) {
            plan.instances = { };
            answers.conditions.clear();
        }
    }
    addConditions(plan.conditions, answers.conditions.span());

    // Rare code stays small: there the operation gives way only to what takes its place without a branch.
    bool takesPlaceOfOperation = plan.failedTestMeansFalse && plan.conditions.isEmpty();
    if (!takesPlaceOfOperation && analyses.blockFrequencies().classOf(block) == BlockFrequency::Rare)
        return std::unexpected(CallDeclineReason::None);

    // A bit that was cleared stays cleared. What stands behind its test is left to the tests that ask each object.
    if (!plan.conditions.isEmpty() && analyses.findEarlierTest(makeConditionsTest(m_context.types, plan.conditions.span()), block) == EarlierTest::Failed)
        return std::unexpected(CallDeclineReason::NeedsTest);
    return plan;
}

void CallResolver::rewriteInstanceOf(const CallSite& site, const InstanceOfPlan& plan)
{
    if (plan.conditions.isEmpty() && plan.isAlwaysFalse) {
        Value answer = IRBuilder::make(Opcode::Boolean);
        answer.setBoolean(false);
        overwriteValue(m_context, site.position.value, WTF::move(answer));
        return;
    }

    if (plan.conditions.isEmpty() && plan.failedTestMeansFalse) {
        overwriteValue(m_context, site.position.value, makeInstanceTest(m_context.types, plan));
        passProofOnBranches(site.position.value, plan.subject);
        return;
    }

    m_expansion.begin(site.position);
    branchOnConditions(plan.conditions.span(), { });

    if (plan.isAlwaysFalse) {
        m_expansion.finishArm(m_builder.appendBoolean(false));
        m_expansion.end();
        return;
    }
    if (plan.failedTestMeansFalse) {
        m_expansion.finishArm(m_expansion.append(makeInstanceTest(m_context.types, plan)));
        m_expansion.end();
        return;
    }
    if (!plan.instances) {
        finishArmWithWalk(m_expansion, plan.subject);
        m_expansion.end();
        return;
    }

    BlockFrequency frequency = m_function.at(m_builder.currentBlock()).frequency;
    bool testsOwnInstance = mayBelongToOtherInstance(accessContext(), receiverCandidates(m_program, m_function, plan.subject));

    ValueID test = m_expansion.append(makeInstanceTest(m_context.types, plan));
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequency);
    Vector<BlockID, 2> whereTestsOfObjectFail;
    auto testObject = [&](Proof proof) {
        ValueID holds = m_expansion.append(makeObjectTest(proof, branch.provenOnTaken, { }));
        whereTestsOfObjectFail.append(m_builder.emitBranch(holds, frequency, rarerOf(frequency, BlockFrequency::Unlikely)).notTaken);
    };
    if (testsOwnInstance)
        testObject(Proof::OwnInstance);
    if (plan.testsPrototypeOfInstance)
        testObject(Proof::PrototypeLinkUnchanged);
    m_expansion.finishArm(m_builder.appendBoolean(true));
    for (BlockID block : whereTestsOfObjectFail) {
        m_builder.moveToEndOf(block);
        // The walk names the subject as it was before the layout test: a type that lists layouts counts as a proof of the instance.
        finishArmWithWalk(m_expansion, plan.subject);
    }

    m_builder.moveToEndOf(branch.notTaken);
    finishArmWithWalk(m_expansion, branch.provenOnNotTaken);
    m_expansion.end();
}

void CallResolver::passProofOnBranches(ValueID test, ValueID subject)
{
    m_builder.setOrigin(m_function.at(test).origin());

    Vector<ValueID, 2> proofs;
    size_t numberOfBlocks = m_function.blocks.size();
    for (size_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != test)
            continue;
        for (uint32_t targetIndex = 0; targetIndex < numberOfBranchTargets; ++targetIndex) {
            Edge edge { block, targetIndex };
            m_builder.splitEdge(edge);
            proofs.append(m_builder.addEdgeResult(edge, testedOperandIndex));
        }
    }
    if (proofs.isEmpty())
        return;

    FunctionAnalyses& analyses = m_context.analyses;
    for (const Use& use : analyses.useLists().usesOf(subject)) {
        if (useNamesGuard(m_function, use))
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
