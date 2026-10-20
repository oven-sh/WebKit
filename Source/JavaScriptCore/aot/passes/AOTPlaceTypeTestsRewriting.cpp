/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTSSARepair.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct AdoptedDefinition {
    uint32_t condition { 0 };
    uint32_t definitionIndex { 0 };
    ProgramPoint pointAfter;
};

class GuardWrappers {
    WTF_MAKE_NONCOPYABLE(GuardWrappers);
public:
    GuardWrappers(Function& function, IRBuilder& builder)
        : m_function(function)
        , m_builder(builder)
    {
    }

    ValueID wrap(ValueID guard, ProgramPoint pointAfter);
    void redirectGuardOperandsOf(ValueID user) const;

private:
    ValueID wrapperOf(ValueID guard) const { return guard.index() < m_wrappers.size() ? m_wrappers[guard.index()] : ValueID(); }

    Function& m_function;
    IRBuilder& m_builder;
    Vector<ValueID> m_wrappers;
};

ValueID GuardWrappers::wrap(ValueID guard, ProgramPoint pointAfter)
{
    RELEASE_ASSERT(!wrapperOf(guard));
    m_builder.setOrigin(m_function.at(guard).origin());
    ValueID wrapper = m_builder.insertBefore(pointAfter, IRBuilder::make(Opcode::AllGuards, { guard }));
    if (guard.index() >= m_wrappers.size())
        m_wrappers.grow(guard.index() + 1);
    m_wrappers[guard.index()] = wrapper;
    return wrapper;
}

void GuardWrappers::redirectGuardOperandsOf(ValueID user) const
{
    Value& value = m_function.at(user);
    value.forEachOperand([&](ValueID& operand, uint32_t operandIndex) {
        if (value.operandKind(operandIndex) != OperandKind::Guard)
            return;
        if (ValueID wrapper = wrapperOf(operand))
            operand = wrapper;
    });
}

} // anonymous namespace

static Vector<AdoptedDefinition, 4> findAdoptedDefinitions(const TestConditions& conditions, std::span<const Vector<ValueID, 2>> definitions, std::span<const ValueID> adoptedProofs, const UseLists& useLists)
{
    BitVector isAdopted(useLists.numberOfValues());
    for (ValueID proof : adoptedProofs) {
        RELEASE_ASSERT(proof.index() < useLists.numberOfValues());
        isAdopted.quickSet(proof.index());
    }

    Vector<AdoptedDefinition, 4> result;
    for (size_t condition : conditions.stretchConditions()) {
        const Vector<ValueID, 2>& proofs = definitions[condition];
        if (proofs.size() < 2)
            continue;
        for (uint32_t index = 0; index < proofs.size(); ++index) {
            if (isAdopted.get(proofs[index].index()))
                result.append(AdoptedDefinition { static_cast<uint32_t>(condition), index, useLists.pointAfter(proofs[index]) });
        }
    }
    return result;
}

static bool acceptsNarrowedValue(OperandKind kind)
{
    return !mustBeDefinition(kind) && kind != OperandKind::Guard;
}

static bool acceptsNarrowedValue(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return acceptsNarrowedValue(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand:
        return acceptsNarrowedValue(function.at(use.block).terminator.operandKind(use.operandIndex));
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestPlacer::rewriteUses()
{
    const UseLists& useLists = m_context.analyses.useLists();
    m_usePoints.reserveInitialCapacity(m_plan.uses.size());
    for (const PlannedUse& planned : m_plan.uses) {
        ValueLocation location = useLists.locationOf(m_plan.sites.sites[planned.site].use);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        m_usePoints.append(ProgramPoint { location.block, location.index });
    }
    Vector<AdoptedDefinition, 4> adoptedDefinitions = findAdoptedDefinitions(*m_plan.conditions,m_definitions.span(), m_adoptedProofs.span(), useLists);

    // SSARepair redirects every use of a definition, and an adopted proof has other uses: an allocation is also the object.
    GuardWrappers wrappers(m_function, m_builder);
    for (const AdoptedDefinition& adopted : adoptedDefinitions) {
        ValueID& definition = m_definitions[adopted.condition][adopted.definitionIndex];
        definition = wrappers.wrap(definition, adopted.pointAfter);
        m_isDefinition.set(definition.index());
    }
    if (!adoptedDefinitions.isEmpty()) {
        for (const EmittedTest& emitted : m_emittedTests) {
            ValueID test = m_function.at(emitted.block).terminator.conditionOperand();
            const Value& value = m_function.at(test);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Guard && !m_isDefinition.get(operand.index()))
                    wrappers.redirectGuardOperandsOf(operand);
            });
            wrappers.redirectGuardOperandsOf(test);
        }
    }

    for (uint32_t useIndex = 0; useIndex < m_plan.uses.size(); ++useIndex) {
        if (isPlaced(m_plan.uses[useIndex].stretch))
            rewriteUse(m_plan.uses[useIndex], useIndex);
    }
    m_builder.applyInsertions();
}

// One value stands for everything that has the identity of the tested value. The site was planned with the type of its own operand, which a branch or a cast may have narrowed in another way than the test.
ValueID TestPlacer::narrowToOperand(ValueID passed, ValueID operand, uint32_t useIndex)
{
    TypeArena& types = m_context.types;
    Type passedType = types.at(m_function.at(passed).type);
    Type operandType = types.at(m_function.at(operand).type);
    if (passedType.isSubsetOf(operandType, m_context.program))
        return passed;
    Type both = passedType.intersectWith(operandType, types);
    if (both.isNever())
        return passed;

    TypeID castType = types.intern(both);
    if (m_context.build.validates())
        m_builder.insertBefore(m_usePoints[useIndex], makeValidateType(passed, castType));
    return m_builder.insertBefore(m_usePoints[useIndex], makeAssumeType(passed, castType));
}

void TestPlacer::rewriteUse(const PlannedUse& planned, uint32_t useIndex)
{
    const TestSite& site = m_plan.sites.sites[planned.site];

    m_builder.setOrigin(m_function.at(site.use).origin());
    for (const SubjectOperand& narrowed : std::ranges::equal_range(m_subjectOperands, useIndex, { }, &SubjectOperand::use)) {
        ValueID passed = representativeOf(requirementAt(narrowed.requirement).condition);
        ValueID operand = m_function.at(site.use).operand(narrowed.operandIndex);
        ValueID replacement = narrowToOperand(passed, operand, useIndex);
        m_function.at(site.use).setOperand(narrowed.operandIndex, replacement);
    }

    Vector<ValueID, 4> proofs;
    Vector<ValueID, 4> guards;
    for (uint32_t test = 0; test < site.numberOfTests; ++test) {
        TestConditionID condition = requirementAt(planned.firstRequirement + test).condition;
        ValueID proof = representativeOf(condition);
        proofs.append(proof);
        if (conditionOf(condition).lastsForStretch())
            guards.appendIfNotContains(proof);
    }

    ValueID guard;
    if (guards.size() == 1)
        guard = guards.first();
    else if (!guards.isEmpty()) {
        Value allGuards = IRBuilder::make(Opcode::AllGuards);
        allGuards.setGuardsOperands(guards.span());
        m_builder.setOrigin(m_function.at(site.use).origin());
        guard = m_builder.insertBefore(m_usePoints[useIndex], WTF::move(allGuards));
    }

    // One test stands for every use of its stretch. In a validating build each use makes the tests that last for a stretch again, and their answers are compared with the one it relies on.
    for (uint32_t test = 0; m_context.build.validates() && test < site.numberOfTests; ++test) {
        uint32_t requirement = planned.firstRequirement + test;
        if (!conditionOf(requirementAt(requirement).condition).lastsForStretch())
            continue;
        Value repeated = m_plan.sites.tests[requirementAt(requirement).test];
        Vector<ValueID, 4> prerequisites;
        forEachTestPrerequisite(m_plan.requirements.span(), *m_plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
            ValueID proof = representativeOf(prerequisite.condition);
            if (prerequisite.narrowedOperand)
                repeated.setOperand(*prerequisite.narrowedOperand, proof);
            else
                prerequisites.appendIfNotContains(proof);
        });
        if (!prerequisites.isEmpty()) {
            Value allPrerequisites = IRBuilder::make(Opcode::AllGuards);
            allPrerequisites.setGuardsOperands(prerequisites.span());
            repeated.setGuardOperand(m_builder.insertBefore(m_usePoints[useIndex], WTF::move(allPrerequisites)));
        }
        Value expected = IRBuilder::make(Opcode::Boolean);
        expected.setBoolean(true);
        ValueID answer = m_builder.insertBefore(m_usePoints[useIndex], WTF::move(repeated));
        ValueID expectedAnswer = m_builder.insertBefore(m_usePoints[useIndex], WTF::move(expected));
        m_builder.insertBefore(m_usePoints[useIndex], IRBuilder::make(Opcode::ValidateSameValue, { answer, expectedAnswer }));
    }

    m_changedValues.append(site.use);
    TestSiteRewriter rewriter(TestSiteRewriter::Inputs { m_context, m_builder, site, m_plan.sites.testsOf(site), proofs.span(), guard, m_usePoints[useIndex], m_changedValues });

    // Until SSA is repaired a site behind the join names the value of the fast copy. Its rule has to see the type the site was planned with, which is the type of the join.
    const Value& useAsPlanned = m_function.at(site.use);
    TypeID type = useAsPlanned.type;
    TypeID written = useAsPlanned.written();
    TypeID prediction = useAsPlanned.prediction();
    ruleOf(site.rule).rewrite(rewriter);
    Value& use = m_function.at(site.use);
    use.type = type;
    use.setWritten(written);
    use.setPrediction(prediction);
}

void TestPlacer::repairProofs()
{
    FunctionAnalyses& analyses = m_context.analyses;
    SSARepair repair(m_builder, analyses);
    auto addVariable = [&](std::span<const ValueID> definitions) {
        if (definitions.size() < 2)
            return;
        SSARepair::Variable variable = repair.addVariable(definitions.front());
        for (ValueID definition : definitions.subspan(1))
            repair.addDefinition(variable, definition);
    };
    for (const Vector<ValueID, 2>& definitions : m_definitions)
        addVariable(definitions.span());
    addVariable(m_epochReads.span());

    // An adopted proof in a copied range stands for its condition at uses it does not reach, so one repair has to know it, its copy and the other proofs as one variable.
    if (m_cloner)
        m_cloner->addVariablesTo(repair, analyses);
    repair.run();
}

void TestPlacer::replaceDominatedUses()
{
    Vector<uint32_t, 8> narrowingTests;
    for (uint32_t index = 0; index < m_emittedTests.size(); ++index) {
        const PlannedTest& planned = m_plan.tests[m_emittedTests[index].plannedTest];
        if (planned.kind == PlannedTestKind::Test && conditionOf(planned.condition).lastsForever())
            narrowingTests.append(index);
    }
    if (narrowingTests.isEmpty())
        return;

    FunctionAnalyses& analyses = m_context.analyses;
    const CFG& cfg = analyses.cfg();
    const ValueIdentities& identities = analyses.identities();

    BitVector isSubject(m_function.values.size());
    for (uint32_t index : narrowingTests)
        isSubject.set(identities.identityOf(m_emittedTests[index].subject).index());
    for (size_t index = 0; index < m_function.values.size(); ++index) {
        ValueID identity = identities.identityOf(ValueID(index));
        if (isSubject.get(identity.index()))
            m_aliasesByIdentity.append({ identity, ValueID(index) });
    }
    std::ranges::sort(m_aliasesByIdentity);

    auto edgeInto = [&](BlockID block) {
        std::optional<Edge> edge = cfg.singleIncomingEdge(block);
        RELEASE_ASSERT(edge);
        return *edge;
    };
    for (uint32_t index : narrowingTests) {
        const EmittedTest& emitted = m_emittedTests[index];
        replaceUsesDominatedBy(edgeInto(emitted.passed), emitted.subject, emitted.provenWhenPassed);
        if (emitted.provenWhenFailed)
            replaceUsesDominatedBy(edgeInto(emitted.failed), emitted.subject, emitted.provenWhenFailed);
    }
    analyses.didChangeValues();
}

void TestPlacer::replaceUsesDominatedBy(Edge edge, ValueID subject, ValueID proven)
{
    FunctionAnalyses& analyses = m_context.analyses;
    const UseLists& useLists = analyses.useLists();
    TypeArena& types = m_context.types;
    TypeID provenTypeID = m_function.at(proven).type;
    Type provenType = types.at(provenTypeID);
    Vector<Use, 4> usesNarrowedAnotherWay;

    for (const auto& [identity, alias] : std::ranges::equal_range(m_aliasesByIdentity, analyses.identityOf(subject), { }, &std::pair<ValueID, ValueID>::first)) {
        if (alias == proven)
            continue;
        for (const Use& use : useLists.usesOf(alias)) {
            ValueID current = usedValueOf(m_function, use);
            if (current == proven || !analyses.dominates(edge, use.block) || !acceptsNarrowedValue(m_function, use))
                continue;
            TypeID currentTypeID = m_function.at(current).type;
            if (currentTypeID == provenTypeID)
                continue;
            if (!provenType.isSubsetOf(types.at(currentTypeID), m_context.program)) {
                // What a value defined behind the edge says holds from its definition on.
                if (analyses.dominates(current, pointBeforeTerminatorOf(m_function, edge.from)))
                    usesNarrowedAnotherWay.append(use);
                continue;
            }
            replaceUse(m_function, use, proven);
            if (use.kind == Use::Kind::Operand)
                m_changedValues.append(use.user);
        }
    }

    // The type of the value may say what no test finds out, such as what flow proved about a class. The first insertion ends the life of the use lists.
    BlockID target = targetOf(m_function, edge).block;
    Vector<std::pair<TypeID, ValueID>, 2> castsByReplacedType;
    m_builder.setOrigin(m_function.at(edge.from).terminator.origin());
    for (const Use& use : usesNarrowedAnotherWay) {
        TypeID replacedTypeID = m_function.at(usedValueOf(m_function, use)).type;
        size_t found = castsByReplacedType.findIf([&](const auto& known) {
            return known.first == replacedTypeID;
        });
        if (found == notFound) {
            found = castsByReplacedType.size();
            Type replacedType = types.at(replacedTypeID);
            Type both = provenType.intersectWith(replacedType, types);
            ValueID cast;
            if (!both.isNever() && both != replacedType && both.isSubsetOf(replacedType, m_context.program)) {
                TypeID castType = types.intern(both);
                ProgramPoint head { target, indexAfterCaughtValues(m_function, m_function.at(target)) };
                if (m_context.build.validates())
                    m_builder.insertBefore(head, makeValidateType(proven, castType));
                cast = m_builder.insertBefore(head, makeAssumeType(proven, castType));
            }
            castsByReplacedType.append({ replacedTypeID, cast });
        }
        ValueID cast = castsByReplacedType[found].second;
        if (!cast)
            continue;
        replaceUse(m_function, use, cast);
        if (use.kind == Use::Kind::Operand)
            m_changedValues.append(use.user);
    }
    m_builder.applyInsertions();
}

void TestPlacer::retypeChangedValues()
{
    narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build }, m_changedValues.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
