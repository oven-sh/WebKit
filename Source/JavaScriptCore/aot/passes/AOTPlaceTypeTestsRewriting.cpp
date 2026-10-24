/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
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

// The uses of everything that has the identity of a tested value are listed once for all tests.
class DominatedUseReplacer {
    WTF_MAKE_NONCOPYABLE(DominatedUseReplacer);
public:
    DominatedUseReplacer(FunctionPassContext&, IRBuilder&, Vector<ValueID>& changedValues);

    void addSubject(ValueID);
    void collectUses();
    void replaceUsesDominatedBy(Edge, ValueID subject, ValueID proven);
    void insertCasts();
    const BitVector& provenValuesInUse() const LIFETIME_BOUND { return m_isInUse; }

private:
    struct NarrowableUse {
        ValueID identity;
        ValueID alias;
        Use use;
        bool getsCast { false };
    };

    struct PlannedCast {
        Edge edge;
        ValueID proven;
        TypeID castType;
        uint32_t use { 0 };
    };

    struct Narrowing {
        TypeID replacedType;
        bool holdsProvenType { false };
        std::optional<TypeID> castType;
    };

    Narrowing& narrowingOf(TypeID replacedType, const Type& provenType);
    TypeID castTypeFor(Narrowing&, const Type& provenType);
    bool isReachedByCastIn(BlockID target, const Use&) const;
    void replace(const Use&, ValueID replacement);
    void compareWithReplacedValue(const Use&, ValueID replacement);

    FunctionPassContext& m_context;
    Function& m_function;
    IRBuilder& m_builder;
    Vector<ValueID>& m_changedValues;
    BitVector m_isSubject;
    Vector<NarrowableUse> m_uses;
    Vector<PlannedCast> m_plannedCasts;
    Vector<Narrowing, 4> m_narrowings;
    BitVector m_isInUse;
    bool m_hasComparisons { false };
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
    Value& value = m_function.edit(user, ValueEdit::Definition);
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
    // Where a use stands now follows from where it stood and from the cuts: the lists of all uses of the function are not made for that.
    m_usePoints.reserveInitialCapacity(m_plan.uses.size());
    for (const PlannedUse& planned : m_plan.uses) {
        const TestSite& site = m_plan.sites.sites[planned.site];
        ProgramPoint point = pointBehindCuts(site.point);
        const BlockValues& values = m_function.at(point.block).values;
        if (point.index >= values.size() || values[point.index] != site.use) {
            // Something was put in front of it.
            ValueLocation location = m_context.analyses.useLists().locationOf(site.use);
            RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
            point = { location.block, location.index };
        }
        m_usePoints.append(point);
    }
    Vector<AdoptedDefinition, 4> adoptedDefinitions;
    if (!m_adoptedProofs.isEmpty())
        adoptedDefinitions = findAdoptedDefinitions(*m_plan.conditions, m_definitions.span(), m_adoptedProofs.span(), m_context.analyses.useLists());

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

    Vector<std::pair<ValueID, TypeID>, 4> typesOfRepresentatives;
    for (const Vector<ValueID, 2>& definitions : m_definitions) {
        if (definitions.size() < 2)
            continue;
        TypeID typeOfFirst = m_function.at(definitions.first()).type;
        if (std::ranges::all_of(definitions, [&](ValueID definition) { return m_function.at(definition).type == typeOfFirst; }))
            continue;
        Type typeOfAll = m_context.types.at(typeOfFirst);
        for (ValueID definition : definitions.subspan(1))
            typeOfAll = typeOfAll.unionWith(m_context.types.at(m_function.at(definition).type), m_context.types);
        typesOfRepresentatives.append({ definitions.first(), typeOfFirst });
        m_function.edit(definitions.first(), ValueEdit::Type).type = m_context.types.intern(typeOfAll);
    }
    for (uint32_t useIndex = 0; useIndex < m_plan.uses.size(); ++useIndex) {
        if (isPlaced(m_plan.uses[useIndex].stretch))
            rewriteUse(m_plan.uses[useIndex], useIndex);
    }
    for (auto [representative, type] : typesOfRepresentatives)
        m_function.edit(representative, ValueEdit::Type).type = type;
    m_builder.applyInsertions();
    // The uses and the guard operands of the tests were written past the builder.
    m_context.analyses.didChangeValues();
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
        if (m_context.build.validates() && replacement != operand)
            m_builder.insertBefore(m_usePoints[useIndex], IRBuilder::make(Opcode::ValidateSameValue, { replacement, operand }));
        m_function.edit(site.use, ValueEdit::Definition).setOperand(narrowed.operandIndex, replacement);
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
            if (!prerequisite.narrowedOperand) {
                prerequisites.appendIfNotContains(proof);
                return;
            }
            if (std::optional<TypeID> castType = findTypeOfOperand(repeated, *prerequisite.narrowedOperand, proof))
                proof = m_builder.insertBefore(m_usePoints[useIndex], makeAssumeType(proof, *castType));
            repeated.setOperand(*prerequisite.narrowedOperand, proof);
        });
        keepOnlyWhatOperandsProve(repeated);
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
    TypeID expected = useAsPlanned.expected();
    ruleOf(site.rule).rewrite(rewriter);
    Value& use = m_function.edit(site.use, ValueEdit::Type);
    use.type = type;
    use.setExpected(expected);
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

    // An adopted proof in a copied range stands for its condition at uses it does not reach, so one repair has to know it, its copy and the other proofs as one variable.
    if (m_copy)
        m_copy->addVariablesTo(repair);
    repair.run();
}

DominatedUseReplacer::DominatedUseReplacer(FunctionPassContext& context, IRBuilder& builder, Vector<ValueID>& changedValues)
    : m_context(context)
    , m_function(context.function)
    , m_builder(builder)
    , m_changedValues(changedValues)
    , m_isSubject(context.function.values.size())
{
}

void DominatedUseReplacer::addSubject(ValueID subject)
{
    m_isSubject.set(m_context.analyses.identityOf(subject).index());
}

void DominatedUseReplacer::collectUses()
{
    const ValueIdentities& identities = m_context.analyses.identities();
    const UseLists& useLists = m_context.analyses.useLists();
    for (size_t index = 0; index < m_function.values.size(); ++index) {
        ValueID alias(index);
        ValueID identity = identities.identityOf(alias);
        if (!m_isSubject.get(identity.index()))
            continue;
        for (const Use& use : useLists.usesOf(alias)) {
            if (acceptsNarrowedValue(m_function, use))
                m_uses.append(NarrowableUse { identity, alias, use });
        }
    }
    AOT::stableSort(m_uses, { }, &NarrowableUse::identity);
}

auto DominatedUseReplacer::narrowingOf(TypeID replacedType, const Type& provenType) -> Narrowing&
{
    size_t found = m_narrowings.findIf([&](const Narrowing& known) {
        return known.replacedType == replacedType;
    });
    if (found == notFound) {
        found = m_narrowings.size();
        m_narrowings.append(Narrowing { replacedType, provenType.isSubsetOf(m_context.types.at(replacedType), m_context.program), std::nullopt });
    }
    return m_narrowings[found];
}

// The type of the value may say what no test finds out, such as what flow proved about a class.
TypeID DominatedUseReplacer::castTypeFor(Narrowing& narrowing, const Type& provenType)
{
    if (!narrowing.castType) {
        TypeArena& types = m_context.types;
        Type replacedType = types.at(narrowing.replacedType);
        Type both = provenType.intersectWith(replacedType, types);
        bool saysMore = !both.isNever() && both != replacedType && both.isSubsetOf(replacedType, m_context.program);
        narrowing.castType = saysMore ? types.intern(both) : TypeID();
    }
    return *narrowing.castType;
}

bool DominatedUseReplacer::isReachedByCastIn(BlockID target, const Use& use) const
{
    if (use.block != target)
        return m_context.analyses.dominators().terminatorDominates(target, use.block);

    switch (use.kind) {
    case Use::Kind::Operand:
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return true;
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// In a validating build: what takes the place of a value is that value.
void DominatedUseReplacer::compareWithReplacedValue(const Use& use, ValueID replacement)
{
    if (!m_context.build.validates() || use.kind == Use::Kind::HandlerArgument || use.kind == Use::Kind::BlockSubject)
        return;
    ValueID replaced = usedValueOf(m_function, use);
    if (!m_context.program.at(m_function.at(replaced).type).isJavaScriptValue())
        return;
    ProgramPoint point = pointBeforeTerminatorOf(m_function, use.block);
    if (use.kind == Use::Kind::Operand)
        point.index = m_context.analyses.useLists().locationOf(use.user).index;
    m_builder.insertBefore(point, IRBuilder::make(Opcode::ValidateSameValue, { replacement, replaced }));
    m_hasComparisons = true;
}

void DominatedUseReplacer::replace(const Use& use, ValueID replacement)
{
    compareWithReplacedValue(use, replacement);
    replaceUse(m_function, use, replacement);
    if (use.kind == Use::Kind::Operand)
        m_changedValues.append(use.user);
}

void DominatedUseReplacer::replaceUsesDominatedBy(Edge edge, ValueID subject, ValueID proven)
{
    FunctionAnalyses& analyses = m_context.analyses;
    TypeID provenTypeID = m_function.at(proven).type;
    Type provenType = m_context.types.at(provenTypeID);
    ProgramPoint pointOfTest = pointBeforeTerminatorOf(m_function, edge.from);
    BlockID target = targetOf(m_function, edge).block;
    m_narrowings.shrink(0);

    auto uses = std::ranges::equal_range(m_uses, analyses.identityOf(subject), { }, &NarrowableUse::identity);
    uint32_t firstUse = static_cast<uint32_t>(uses.begin() - m_uses.begin());
    for (uint32_t index = firstUse; index < firstUse + uses.size(); ++index) {
        NarrowableUse& narrowable = m_uses[index];
        // A use that gets a cast will name none of the values that were listed.
        if (narrowable.alias == proven || narrowable.getsCast)
            continue;
        const Use& use = narrowable.use;
        if (use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == Opcode::ValidateSameValue)
            continue;
        ValueID current = usedValueOf(m_function, use);
        if (current == proven || !analyses.dominates(edge, use.block))
            continue;
        TypeID currentTypeID = m_function.at(current).type;
        if (currentTypeID == provenTypeID)
            continue;
        Narrowing& narrowing = narrowingOf(currentTypeID, provenType);
        if (narrowing.holdsProvenType) {
            replace(use, proven);
            m_isInUse.set(proven.index());
            continue;
        }
        // What a value defined behind the edge says holds from its definition on.
        if (!analyses.dominates(current, pointOfTest) || !isReachedByCastIn(target, use))
            continue;
        if (TypeID castType = castTypeFor(narrowing, provenType)) {
            narrowable.getsCast = true;
            m_plannedCasts.append(PlannedCast { edge, proven, castType, index });
            m_isInUse.set(proven.index());
        }
    }
}

void DominatedUseReplacer::insertCasts()
{
    Vector<std::pair<TypeID, ValueID>, 2> castsOfProvenValue;
    ValueID proven;
    for (const PlannedCast& planned : m_plannedCasts) {
        if (planned.proven != proven) {
            proven = planned.proven;
            castsOfProvenValue.shrink(0);
            m_builder.setOrigin(m_function.at(planned.edge.from).terminator.origin());
        }
        size_t found = castsOfProvenValue.findIf([&](const auto& known) {
            return known.first == planned.castType;
        });
        if (found == notFound) {
            found = castsOfProvenValue.size();
            BlockID target = targetOf(m_function, planned.edge).block;
            ProgramPoint head { target, indexAfterCaughtValues(m_function, m_function.at(target)) };
            if (m_context.build.validates())
                m_builder.insertBefore(head, makeValidateType(proven, planned.castType));
            castsOfProvenValue.append({ planned.castType, m_builder.insertBefore(head, makeAssumeType(proven, planned.castType)) });
        }
        replace(m_uses[planned.use].use, castsOfProvenValue[found].second);
    }
    if (!m_plannedCasts.isEmpty() || m_hasComparisons)
        m_builder.applyInsertions();
}

void TestPlacer::replaceDominatedUses()
{
    Vector<uint32_t, 8> narrowingTests;
    for (uint32_t index = 0; index < m_emittedTests.size(); ++index) {
        const PlannedTest& planned = m_plan.tests[m_emittedTests[index].plannedTest];
        if (conditionOf(planned.condition).lastsForever())
            narrowingTests.append(index);
    }
    if (narrowingTests.isEmpty()) {
        leaveOutBlocksOfUnusedProofs({ });
        return;
    }

    FunctionAnalyses& analyses = m_context.analyses;
    const CFG& cfg = analyses.cfg();
    auto edgeInto = [&](BlockID block) {
        std::optional<Edge> edge = cfg.singleIncomingEdge(block);
        RELEASE_ASSERT(edge);
        return *edge;
    };

    // Where the copies meet, the repair of SSA has given the test the join of what each of them passes on.
    auto subjectOf = [&](const EmittedTest& emitted) {
        ValueID test = m_function.at(edgeInto(emitted.passed).from).terminator.conditionOperand();
        return m_function.at(test).operand(testedOperandIndex);
    };

    DominatedUseReplacer replacer(m_context, m_builder, m_changedValues);
    for (uint32_t index : narrowingTests)
        replacer.addSubject(subjectOf(m_emittedTests[index]));
    replacer.collectUses();
    for (uint32_t index : narrowingTests) {
        const EmittedTest& emitted = m_emittedTests[index];
        ValueID subject = subjectOf(emitted);
        replacer.replaceUsesDominatedBy(edgeInto(emitted.passed), subject, emitted.provenWhenPassed);
        if (emitted.provenWhenFailed)
            replacer.replaceUsesDominatedBy(edgeInto(emitted.failed), subject, emitted.provenWhenFailed);
    }
    replacer.insertCasts();
    analyses.didChangeValues();
    leaveOutBlocksOfUnusedProofs(replacer.provenValuesInUse());
}

// What a test proves where it fails is a parameter of a block on the failing edge.
void TestPlacer::leaveOutBlocksOfUnusedProofs(const BitVector& isUsed)
{
    bool changed = false;
    for (const EmittedTest& emitted : m_emittedTests) {
        if (!emitted.provenWhenFailed || isUsed.get(emitted.provenWhenFailed.index()))
            continue;
        // A rewritten use may have split the block of the test, so the branch is looked for from its target.
        std::optional<Edge> edge = m_context.analyses.cfg().singleIncomingEdge(emitted.failed);
        RELEASE_ASSERT(edge && m_function.at(emitted.failed).terminator.kind == TerminatorKind::Jump);
        // The repair of SSA may have led a value through the block, and a cast may stand in it.
        if (!m_function.at(emitted.failed).values.isEmpty() || m_function.at(emitted.failed).parameters.size() != 1)
            continue;
        Block& failed = m_function.edit(emitted.failed, { BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
        Target& notTaken = editTarget(m_function, *edge);
        notTaken = Target { failed.terminator.next().block, WTF::move(failed.terminator.next().arguments), { } };
        failed.parameters.clear();
        failed.terminator = Terminator { };
        changed = true;
    }
    if (changed)
        m_context.analyses.didChangeControlFlow();
}

void TestPlacer::retypeChangedValues()
{
    narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build, &m_context.typingMemo }, m_changedValues.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
