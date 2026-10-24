/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPointsToSolver.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTPointsToSolverInternal.h"
#include "AOTResultRules.h"
#include "AOTEffectsCache.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool isRetainedInOperand(const OperandSummary& summary)
{
    return summary.uses.contains(OperandUse::Retains) && summary.retainedIn == RetainedIn::Operand;
}

struct OpcodeObjectFacts {
    uint64_t retainingOperands { 0 };
    bool retainsOperandInResult { false };
    bool allocatesOrReturnsOperand { false };
    bool isSuperConstruct { false };
};

static OpcodeObjectFacts computeObjectFactsOf(Opcode opcode)
{
    OpcodeObjectFacts facts;
    bool returnsOperand = false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t index = 0; index < numberOfDeclaredOperands; ++index) {
        returnsOperand |= operandUsesOf(opcode, index).contains(OperandUse::Returns);
        facts.retainsOperandInResult |= operandUsesOf(opcode, index).contains(OperandUse::Retains) && retainedInOf(opcode, index) == RetainedIn::Result;
        if (retainedInOf(opcode, index) != RetainedIn::Operand)
            continue;
        std::optional<uint8_t> retainingOperand = retainingOperandOf(opcode, index);
        if (!retainingOperand)
            continue;
        RELEASE_ASSERT(*retainingOperand < 64);
        facts.retainingOperands |= 1ull << *retainingOperand;
    }
    // The copy of an environment has the type of the environment, so its row has no result rule of an allocation.
    facts.allocatesOrReturnsOperand = returnsOperand && (resultRuleOf(opcode) == ResultRule::Allocation || opcode == Opcode::MaterializeEnvironment);
    facts.isSuperConstruct = OperandPosition::parent(opcode).has_value();
    return facts;
}

static const OpcodeObjectFacts& objectFactsOf(Opcode opcode)
{
    static const std::array<OpcodeObjectFacts, numberOfOpcodes> factsOfOpcodes = [] {
        std::array<OpcodeObjectFacts, numberOfOpcodes> result { };
        for (Opcode member : allMembersOf<Opcode>)
            result[static_cast<unsigned>(member)] = computeObjectFactsOf(member);
        return result;
    }();
    return factsOfOpcodes[static_cast<unsigned>(opcode)];
}

const OpcodeSet& opcodesThatMayDefineObjects()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode member : allMembersOf<Opcode>) {
            const OpcodeObjectFacts& facts = objectFactsOf(member);
            if (resultMayBeFresh(member) || facts.allocatesOrReturnsOperand || facts.isSuperConstruct)
                result.add(member);
        }
        return result;
    }();
    return opcodes;
}

const OpcodeSet& opcodesThatDefineObjectsThatNeedNotBeFresh()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode member : allMembersOf<Opcode>) {
            const OpcodeObjectFacts& facts = objectFactsOf(member);
            if (facts.allocatesOrReturnsOperand || facts.isSuperConstruct)
                result.add(member);
        }
        return result;
    }();
    return opcodes;
}

static bool isRetainingOperand(Opcode opcode, uint32_t operandIndex)
{
    return operandIndex < 64 && (objectFactsOf(opcode).retainingOperands >> operandIndex) & 1;
}

static EscapeReason reasonForRetainingTerminator(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return EscapeReason::Returned;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        return EscapeReason::Thrown;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return EscapeReason::StoredInUnknownMemory;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ObjectSlot slotInResult(const Value& user, uint32_t operandIndex)
{
    std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
    if (!position)
        return std::nullopt;
    if (!Value::opcodesWithFields.contains(user.opcode))
        return position;
    auto fields = user.fields();
    if (*position >= fields.size())
        return std::nullopt;
    return fields[*position];
}

// An opaque value made from an object refers to it. A class token refers to nothing: it is read out of the object that holds it, as any value is.
static bool refersToOperand(const Type& result)
{
    return result.opaque && !result.isOpaque(OpaqueKind::ClassToken);
}

// What summarizeOperand() says of an operand that may be a reference, where it says so whatever the types are: the operation is described by its row, runs no code of its operands and calls none of them.
struct DeclaredOperandFacts {
    bool isSameForEveryReference { false };
    // What PointsToSolver::isContentsStore() says of the summary.
    bool isContentsStore { false };
    OperandSummary summaryOfReference;
};

static DeclaredOperandFacts computeDeclaredOperandFacts(Opcode opcode, uint32_t declaredIndex)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Declared:
    case EffectRule::Operator:
        break;
    case EffectRule::Access:
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return { };
    }

    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t index = 0; index < numberOfDeclaredOperands; ++index) {
        if (protocolsOf(opcode, index).contains(Protocol::HasInstance))
            return { };
    }
    OperandBehavior behavior = declaredBehaviorOf(opcode, declaredIndex);
    if (behavior.uses.containsAny(callingUses) || !behavior.protocols.isEmpty() || behavior.declarer != OperandDeclarer::Operation)
        return { };

    OperandSummary summary;
    if (!behavior.uses.isEmpty())
        summary = { behavior.uses, behavior.retainedIn, behavior.retainingOperand };
    if (operandKindOf(opcode, declaredIndex) == OperandKind::Results)
        summary.uses.add(OperandUse::Returns);
    if (!behavior.uses.contains(OperandUse::RetainsContents) && declaredBoundOf(opcode).mayCallUnknownCode)
        summary.uses.add(OperandUse::PassesToUserCode);
    OperandUses ownUses = operandUsesOf(opcode, declaredIndex);
    bool isContentsStore = summary.uses.contains(OperandUse::WritesContents) && ownUses.contains(OperandUse::WritesContents) && !ownUses.containsAny({ OperandUse::Retains, OperandUse::Calls });
    return { true, isContentsStore, summary };
}

static const DeclaredOperandFacts& declaredOperandFactsOf(Opcode opcode, uint32_t operandIndex)
{
    static constexpr uint32_t maxDeclaredOperands = 8;
    static const DeclaredOperandFacts unknown;
    static const std::array<std::array<DeclaredOperandFacts, maxDeclaredOperands>, numberOfOpcodes> factsOfOpcodes = [] {
        std::array<std::array<DeclaredOperandFacts, maxDeclaredOperands>, numberOfOpcodes> result { };
        for (Opcode member : allMembersOf<Opcode>) {
            uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(member) + (hasVariadicOperand(member) ? 1 : 0);
            for (uint32_t index = 0; index < std::min(numberOfDeclaredOperands, maxDeclaredOperands); ++index)
                result[static_cast<unsigned>(member)][index] = computeDeclaredOperandFacts(member, index);
        }
        return result;
    }();
    uint32_t declaredIndex = std::min(operandIndex, numberOfNamedOperandsOf(opcode));
    return declaredIndex < maxDeclaredOperands ? factsOfOpcodes[static_cast<unsigned>(opcode)][declaredIndex] : unknown;
}

// The uses that solveOperandUse() does something for.
static constexpr OperandUses usesWithConsequences {
    OperandUse::Returns,
    OperandUse::PassesToUserCode,
    OperandUse::Retains,
    OperandUse::Calls,
    OperandUse::CallsPerElement,
    OperandUse::CallsRepeatedly,
    OperandUse::RetainsContents,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
    OperandUse::BecomesPrototype,
};

AnswerFromTypes answerFromTypes(const EscapeAnalysisInputs& inputs, AnswerFromTypes::Question question, uint32_t subject, uint32_t operandIndex)
{
    using Question = AnswerFromTypes::Question;

    const Program& program = inputs.context.program;
    const Function& function = inputs.function;
    AnswerFromTypes answer { .question = question, .subject = subject, .operandIndex = operandIndex };
    switch (question) {
    case Question::IsReference:
        answer.isTrue = mayBeReference(program.typeOf(function, ValueID(subject)));
        return answer;
    case Question::MayHoldReference:
        answer.isTrue = mayBeReference(program.typeOf(function, ValueID(subject))) || numberOfResultsOf(program, function, function.at(ValueID(subject))).has_value();
        return answer;
    case Question::RefersToOperand:
        answer.isTrue = refersToOperand(program.typeOf(function, ValueID(subject)));
        return answer;
    case Question::HasFreshResult: {
        auto keptEffects = [&] {
            return inputs.effectsCache.of(ValueID(subject));
        };
        answer.isTrue = freshnessOf(inputs.effects, function, function.at(ValueID(subject)), ScopedLambda<EffectsOnEdges()>(keptEffects)) != ResultFreshness::Unknown;
        return answer;
    }
    case Question::MayRunOtherCode: {
        const EffectsOnEdges& effects = inputs.effectsCache.of(ValueID(subject));
        answer.isTrue = effects.normal.mayRunOtherCode() || effects.handler.mayRunOtherCode();
        return answer;
    }
    case Question::SummaryOfOperand:
        if (const Value& user = function.at(ValueID(subject)); declaredOperandFactsOf(user.opcode, operandIndex).isSameForEveryReference && mayBeReference(program.typeOf(function, user.operand(operandIndex))))
            answer.summary = declaredOperandFactsOf(user.opcode, operandIndex).summaryOfReference;
        else if (effectRuleOf(user.opcode) == EffectRule::Call) {
            // Read out of the summaries of the callees: to look it up by what it is computed from takes longer.
            answer.summary = summarizeOperand(inputs.effects, function, user, operandIndex);
        } else
            answer.summary = inputs.effectsCache.summaryOfOperand(ValueID(subject), operandIndex);
        if (inputs.context.build.options().verifiesAfterEveryPass)
            RELEASE_ASSERT(answer.summary == summarizeOperand(inputs.effects, function, function.at(ValueID(subject)), operandIndex));
        return answer;
    case Question::MakesObjectOnEdge:
        answer.isTrue = freshnessOf(inputs.effects, function, function.at(BlockID(subject)).terminator) != ResultFreshness::Unknown;
        return answer;
    case Question::SummaryOfTerminatorOperand:
        answer.summary = summarizeOperand(inputs.effects, function, function.at(BlockID(subject)).terminator, operandIndex);
        return answer;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PointsToSolver::PointsToSolver(const EscapeAnalysisInputs& inputs, Sweep sweep)
    : m_inputs(inputs)
    , m_program(inputs.context.program)
    , m_function(inputs.function)
    , m_cfg(inputs.cfg)
    , m_useLists(inputs.useLists)
    , m_maxWork(inputs.context.build.limit(LimitID::MaxEscapeAnalysisWork))
    , m_maxWordsOfSets(std::max<uint64_t>(m_maxWork, inputs.function.values.size()))
    , m_sweep(sweep)
{
    m_results.setOfValue.fill(ValueSetID(), m_function.values.size());
    m_valueFlags.fill({ }, m_function.values.size());
    m_firstSummaryOfUser.fill(0, m_function.values.size());
    m_valuesWithArrivals.ensureSize(m_function.values.size());
}

PointsToResults PointsToSolver::solve()
{
    findNarrowedCopies();
    findObjects();
    Vector<ObjectID, 16> arrivedObjects;
    WordsOfSet arrivedWords(FillWith { }, m_wordsPerSet, ObjectWord { 0 });
    WordRange rangeOfArrivedWords { 0, 1 };
    size_t numberOfValues = m_function.values.size();
    for (size_t next = 0; !m_results.gaveUp;) {
        size_t position = m_valuesWithArrivals.findBit(next, true);
        if (position >= numberOfValues) {
            if (!next)
                break;
            next = 0;
            continue;
        }
        next = position + 1;
        m_valuesWithArrivals.quickClear(position);

        ValueID value(mirroredWhenBackward(position));
        ValueSetID set = m_results.setOfValue[value.index()];
        std::span<ObjectWord> arrivals = arrivalsOf(set);
        if (m_wordsPerSet > 1) {
            for (uint32_t index = rangeOfArrivedWords.first; index < rangeOfArrivedWords.end; ++index)
                arrivedWords[index] = 0;
            rangeOfArrivedWords = std::exchange(m_arrivedWordsOfSets[set.index()], WordRange { });
        }
        arrivedObjects.shrink(0);
        for (uint32_t index = rangeOfArrivedWords.first; index < rangeOfArrivedWords.end; ++index) {
            arrivedWords[index] = std::exchange(arrivals[index], 0);
            for (ObjectWord word = arrivedWords[index]; word; word &= word - 1)
                arrivedObjects.append(ObjectID(index * objectsPerWord + std::countr_zero(word)));
        }
        if (!takeWork(arrivedObjects.size()))
            break;
        ArrivedObjects arrived { arrivedObjects.span(), arrivedWords.span() };
        for (const Use& use : m_useLists.usesOf(value))
            solveUse(arrived, value, use);
        for (const NarrowedCopy& copy : narrowedCopiesOf(value))
            addAll(copy.parameter, arrived);
        addOperandsRetainedInResult(value, arrived);
        applyQueuedUses();
    }
    if (m_results.gaveUp)
        forgetAllButObjects();
    else {
        computeMayBeUntracked();
        collectContents();
    }
    takeSets();
    m_results.work = m_work;
    m_results.answersFromTypes = WTF::move(m_answersFromTypes);
    return WTF::move(m_results);
}

bool PointsToSolver::takeWork(uint64_t amount)
{
    m_work += amount;
    if (m_work <= m_maxWork)
        return true;
    giveUp();
    return false;
}

void PointsToSolver::giveUp()
{
    if (std::exchange(m_results.gaveUp, true))
        return;
    if (m_sweep == Sweep::Forward)
        m_inputs.context.build.gaveUp(LimitID::MaxEscapeAnalysisWork, m_inputs.subject);
    forgetAllButObjects();
}

// Once more when the loop has ended: the uses of the value at which it gave up are looked at to the last one.
void PointsToSolver::forgetAllButObjects()
{
    m_valuesWithArrivals.clearAll();
    m_applicationWorklist.clear();
    m_results.uses.clear();
    m_results.contents.clear();
    for (size_t index = 0; index < m_results.setOfValue.size(); ++index) {
        if (m_results.setOfValue[index] && !definesObject(ValueID(index)))
            m_results.setOfValue[index] = ValueSetID();
    }
    for (SolvedObject& object : m_results.objects) {
        object.reasons = { };
        object.appliedUses = { };
    }
}

bool PointsToSolver::askOnce(ValueID value, AnswerFromTypes::Question question, ValueFlag yes, ValueFlag no)
{
    OptionSet<ValueFlag>& flags = m_valueFlags[value.index()];
    if (flags.containsAny({ yes, no }))
        return flags.contains(yes);
    bool result = ask(question, value.index()).isTrue;
    flags.add(result ? yes : no);
    return result;
}

bool PointsToSolver::mayRunOtherCode(ValueID value)
{
    // Whatever the types are.
    const EffectTraitBounds& bounds = effectFactsOf(m_function.at(value).opcode).traits;
    bool isSameForAllTypes = bounds.atLeast.mayRunOtherCode() == bounds.atMost.mayRunOtherCode();
    if (isSameForAllTypes && !m_inputs.context.build.options().verifiesAfterEveryPass)
        return bounds.atLeast.mayRunOtherCode();
    bool result = askOnce(value, AnswerFromTypes::Question::MayRunOtherCode, ValueFlag::MayRunOtherCode, ValueFlag::RunsNoOtherCode);
    RELEASE_ASSERT(!isSameForAllTypes || result == bounds.atLeast.mayRunOtherCode());
    return result;
}

PointsToSolver::WordsOfSet PointsToSolver::objectsOf(ValueID value) const
{
    ValueSetID set = m_results.setOfValue[value.index()];
    if (!set)
        return { };
    return WordsOfSet(m_wordsOfSets.subspan(size_t { set.index() } * m_wordsPerSet, m_wordsPerSet));
}

AnswerFromTypes PointsToSolver::ask(AnswerFromTypes::Question question, uint32_t subject, uint32_t operandIndex) const
{
    AnswerFromTypes answer = answerFromTypes(m_inputs, question, subject, operandIndex);
    if (!m_inputs.remembersAnswersFromTypes)
        return answer;
    if (m_answersFromTypes.size() == m_answersFromTypes.capacity())
        m_answersFromTypes.reserveCapacity(std::max<size_t>(16, m_answersFromTypes.capacity() * 2));
    m_answersFromTypes.append(answer);
    return answer;
}

static void appendZeros(Vector<ObjectWord>& words, unsigned count)
{
    size_t size = words.size();
    if (size + count > words.capacity())
        words.reserveCapacity(std::max<size_t>(64, std::max<size_t>(size + count, words.capacity() * 2)));
    if (count == 1) {
        words.append(0);
        return;
    }
    words.grow(size + count);
    zeroSpan(words.mutableSpan().subspan(size));
}

ValueSetID PointsToSolver::ensureSet(ValueID value)
{
    ValueSetID& id = m_results.setOfValue[value.index()];
    if (id)
        return id;
    if (m_wordsOfSets.size() + m_wordsPerSet > m_maxWordsOfSets) {
        giveUp();
        return { };
    }
    id = ValueSetID(m_setsThatMayBeUntracked.size());
    m_setsThatMayBeUntracked.append(true);
    m_valueOfSet.append(value);
    appendZeros(m_wordsOfSets, m_wordsPerSet);
    appendZeros(m_arrivalsOfSets, m_wordsPerSet);
    if (m_wordsPerSet > 1)
        m_arrivedWordsOfSets.append(WordRange { });
    return id;
}

void PointsToSolver::takeSets()
{
    size_t numberOfSets = m_setsThatMayBeUntracked.size();
    m_results.sets.reserveInitialCapacity(numberOfSets);
    for (size_t index = 0; index < numberOfSets; ++index) {
        ValueSet set;
        set.objects.ensureSize(m_results.objects.size());
        forEachObjectIn(wordsOf(ValueSetID(index)), [&](ObjectID object) {
            set.objects.quickSet(object.index());
        });
        set.mayBeUntracked = m_setsThatMayBeUntracked[index];
        m_results.sets.append(WTF::move(set));
    }
}

ValueID PointsToSolver::parameterFor(const Use& edgeUse) const
{
    const Target& target = targetOf(m_function, edgeUse.edge());
    auto& parameters = m_function.at(target.block).parameters;
    if (edgeUse.operandIndex >= parameters.size())
        return { };
    return parameters[edgeUse.operandIndex];
}

std::span<const NarrowedCopy> PointsToSolver::narrowedCopiesOf(ValueID subject) const
{
    if (!m_valueFlags[subject.index()].contains(ValueFlag::HasNarrowedCopies))
        return { };
    auto copies = std::ranges::equal_range(m_narrowedCopies, subject, { }, &NarrowedCopy::subject);
    return m_narrowedCopies.subspan(copies.begin() - m_narrowedCopies.begin(), copies.size());
}

void PointsToSolver::findNarrowedCopies()
{
    for (BlockID from : m_cfg.reversePostOrder()) {
        bool passesProofs = std::ranges::any_of(m_function.at(from).terminator.targets, [](const Target& target) {
            return !target.proofs.isEmpty();
        });
        if (!passesProofs)
            continue;
        // A proof that lasts for a stretch is handed on as a guard. It narrows nothing.
        std::optional<Proof> proof = proofOfTerminator(m_function, m_function.at(from));
        if (!proof || provesForStretch(*proof))
            continue;
        // What forEachEdgeResult() finds, with one look at the test for all the edges.
        const Terminator& terminator = m_function.at(from).terminator;
        auto addCopiesOfOperandsOf = [&](const auto& test) {
            uint32_t numberOfTargets = terminator.targets.size();
            for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
                const Target& target = terminator.targets[targetIndex];
                if (target.proofs.isEmpty() || targetResultsOf(terminator.kind, targetIndex, numberOfTargets) != TargetResults::Proofs)
                    continue;
                const auto& parameters = m_function.at(target.block).parameters;
                size_t firstParameter = target.arguments.size();
                for (size_t resultIndex = 0; resultIndex < target.proofs.size() && firstParameter + resultIndex < parameters.size(); ++resultIndex) {
                    uint32_t operandIndex = target.proofs[resultIndex];
                    if (operandIndex >= test.numberOfOperands() || !test.operand(operandIndex) || test.operandKind(operandIndex) == OperandKind::Guard)
                        continue;
                    ValueID subject = test.operand(operandIndex);
                    ValueID copy = parameters[firstParameter + resultIndex];
                    if (!ask(AnswerFromTypes::Question::IsReference, copy.index()).isTrue)
                        continue;
                    m_narrowedCopies.append(NarrowedCopy { subject, copy });
                    m_valueFlags[subject.index()].add(ValueFlag::HasNarrowedCopies);
                }
            }
        };
        if (terminator.isConditionalBranch())
            addCopiesOfOperandsOf(m_function.at(terminator.conditionOperand()));
        else
            addCopiesOfOperandsOf(terminator);
    }
    AOT::sort(m_narrowedCopies);
    removeRepeatedElements(m_narrowedCopies);
}

void PointsToSolver::findObjects()
{
    // How many words a set has is known when all objects are.
    struct FoundObject {
        ValueID value;
        ObjectKind kind { ObjectKind::Allocation };
        bool sharesItsValue { false };
    };
    Vector<FoundObject, 16> found;
    auto addObject = [&](ValueID definition, ObjectKind kind) {
        found.append({ definition, kind, false });
    };
    auto addObjectSharingItsValue = [&](ValueID value) {
        found.append({ value, ObjectKind::Allocation, true });
    };

    for (ValueID parameter : m_function.at(m_cfg.root()).parameters) {
        if (ask(AnswerFromTypes::Question::IsReference, parameter.index()).isTrue)
            addObject(parameter, ObjectKind::EntryParameter);
    }
    const OpcodeSet& definingOpcodes = opcodesThatMayDefineObjects();
    for (BlockID block : m_cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (!definingOpcodes.contains(value.opcode))
                continue;
            if (resultMayBeFresh(value.opcode) && ask(AnswerFromTypes::Question::HasFreshResult, id.index()).isTrue)
                addObject(id, ObjectKind::Allocation);
            else if (objectFactsOf(value.opcode).allocatesOrReturnsOperand)
                addObjectSharingItsValue(id);
            else if (objectFactsOf(value.opcode).isSuperConstruct && ask(AnswerFromTypes::Question::IsReference, id.index()).isTrue)
                addObject(id, ObjectKind::EntryParameter);
        }
        if (!makesObjectOnEdge(block))
            continue;
        forEachEdgeResult(m_function, block, [&](const EdgeResult& result, ValueID parameter) {
            if (result.kind == EdgeResultKind::DeclaredResult)
                addObjectSharingItsValue(parameter);
        });
    }

    m_wordsPerSet = std::max<size_t>(1, (found.size() + objectsPerWord - 1) / objectsPerWord);
    // Each object has a set of its own, also when the rest is given up. So none is tracked.
    if (uint64_t { found.size() } * m_wordsPerSet > m_maxWordsOfSets) {
        found.clear();
        m_wordsPerSet = 1;
        giveUp();
    }
    m_newContents.fill(0, m_wordsPerSet);
    m_results.objects.reserveInitialCapacity(found.size());
    for (const FoundObject& object : found) {
        if (object.sharesItsValue)
            this->addObjectSharingItsValue(object.value);
        else
            this->addObject(object.value, object.kind);
    }
    m_containerOfObject.fill(noContainer, m_results.objects.size());
    m_containers.reserveInitialCapacity(m_results.objects.size());
    m_lastApplicationsToAll.fill({ }, m_results.objects.size());
    m_calledObjects.ensureSize(m_results.objects.size());
    if (!m_results.objects.isEmpty())
        m_results.uses.reserveInitialCapacity(m_function.values.size());
}

void PointsToSolver::addObject(ValueID definition, ObjectKind kind)
{
    ObjectID object(m_results.objects.size());
    m_results.objects.append(SolvedObject { definition, kind, { }, { } });
    RELEASE_ASSERT(!hasSet(definition));
    add(definition, object);
    m_setsThatMayBeUntracked[m_results.setOfValue[definition.index()].index()] = false;
    m_valueFlags[definition.index()].add(ValueFlag::DefinesObject);
}

bool PointsToSolver::makesObjectOnEdge(BlockID from) const
{
    return resultMayBeFresh(m_function.at(from).terminator.kind) && ask(AnswerFromTypes::Question::MakesObjectOnEdge, from.index()).isTrue;
}

// No value stands for the object alone: another edge may pass another object to the parameter, and the operation may hand back its operand.
void PointsToSolver::addObjectSharingItsValue(ValueID value)
{
    ObjectID object(m_results.objects.size());
    m_results.objects.append(SolvedObject { value, ObjectKind::Allocation, { }, { } });
    m_valueFlags[value.index()].add(ValueFlag::IsSharedByObjects);
    add(value, object);
}

void PointsToSolver::add(ValueID value, ObjectID object)
{
    if (definesObject(value))
        return;

    ValueSetID set = ensureSet(value);
    if (!set)
        return;
    uint32_t wordInSet = object.index() / objectsPerWord;
    size_t word = size_t { set.index() } * m_wordsPerSet + wordInSet;
    ObjectWord bit = ObjectWord { 1 } << (object.index() % objectsPerWord);
    if (m_wordsOfSets[word] & bit)
        return;
    m_wordsOfSets[word] |= bit;
    m_arrivalsOfSets[word] |= bit;
    if (m_wordsPerSet > 1) {
        WordRange& range = m_arrivedWordsOfSets[set.index()];
        range.first = std::min(range.first, wordInSet);
        range.end = std::max(range.end, wordInSet + 1);
    }
    m_valuesWithArrivals.quickSet(mirroredWhenBackward(value.index()));
}

// The words lie outside the table of all sets, which may move here.
void PointsToSolver::addAll(ValueID value, std::span<const ObjectWord> objects)
{
    if (definesObject(value))
        return;
    // No value has an empty set.
    if (std::ranges::all_of(objects, [](ObjectWord word) { return !word; }))
        return;

    ValueSetID set = ensureSet(value);
    if (!set)
        return;
    std::span<ObjectWord> words = wordsOf(set);
    std::span<ObjectWord> arrivals = arrivalsOf(set);
    if (m_wordsPerSet == 1) {
        ObjectWord newObjects = objects[0] & ~words[0];
        if (!newObjects)
            return;
        words[0] |= newObjects;
        arrivals[0] |= newObjects;
        m_valuesWithArrivals.quickSet(mirroredWhenBackward(value.index()));
        return;
    }

    WordRange& range = m_arrivedWordsOfSets[set.index()];
    ObjectWord arrived = 0;
    for (uint32_t index = 0; index < objects.size(); ++index) {
        ObjectWord newObjects = objects[index] & ~words[index];
        if (!newObjects)
            continue;
        words[index] |= newObjects;
        arrivals[index] |= newObjects;
        arrived |= newObjects;
        range.first = std::min(range.first, index);
        range.end = std::max(range.end, index + 1);
    }
    if (arrived)
        m_valuesWithArrivals.quickSet(mirroredWhenBackward(value.index()));
}

void PointsToSolver::addAll(ValueID value, const ArrivedObjects& arrived)
{
    AOT_THRESHOLD(size_t, fewObjects, 4);
    if (m_wordsPerSet == 1 || arrived.objects.size() > fewObjects) {
        addAll(value, arrived.words);
        return;
    }
    for (ObjectID object : arrived.objects)
        add(value, object);
}

void PointsToSolver::solveUse(const ArrivedObjects& arrived, ValueID usedValue, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        solveEdgeUse(arrived, use);
        return;
    case Use::Kind::TerminatorOperand:
        solveTerminatorOperand(arrived, use);
        return;
    case Use::Kind::Operand:
        solveOperand(arrived, usedValue, use);
        return;
    case Use::Kind::BlockSubject: {
        SolvedUse solved { .use = use, .uses = OperandUse::Inspects };
        record(solved, arrived);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::record(SolvedUse& solved, const ArrivedObjects& arrived)
{
    bool onlyCreatesFollowedAlias = solved.flowsTo && !solved.retainedBy && solved.reasons.isEmpty() && solved.uses.containsOnly({ OperandUse::Inspects, OperandUse::Returns });
    if (onlyCreatesFollowedAlias)
        return;
    size_t size = m_results.uses.size() + arrived.objects.size();
    if (size > m_results.uses.capacity())
        m_results.uses.reserveCapacity(std::max<size_t>({ 16, size, m_results.uses.capacity() * 2 }));
    for (ObjectID object : arrived.objects) {
        solved.object = object;
        m_results.uses.append(solved);
    }
}

void PointsToSolver::solveEdgeUse(const ArrivedObjects& arrived, const Use& use)
{
    if (ValueID parameter = parameterFor(use)) {
        addAll(parameter, arrived);
        return;
    }
    SolvedUse solved { .use = use, .uses = OperandUses::all(), .reasons = EscapeReason::StoredInUnknownMemory, .isContentsStore = true };
    record(solved, arrived);
}

void PointsToSolver::solveTerminatorOperand(const ArrivedObjects& arrived, const Use& use)
{
    const Terminator& terminator = m_function.at(use.block).terminator;
    OperandUses uses = ask(AnswerFromTypes::Question::SummaryOfTerminatorOperand, use.block.index(), use.operandIndex).summary.uses;
    SolvedUse solved { .use = use, .uses = uses, .isContentsStore = uses.contains(OperandUse::WritesContents) };
    for (OperandUse operandUse : uses & declaredOperandUses)
        solveTerminatorOperandUse(solved, arrived, terminator.kind, operandUse);
    record(solved, arrived);
}

void PointsToSolver::solveTerminatorOperandUse(SolvedUse& solved, const ArrivedObjects& arrived, TerminatorKind kind, OperandUse use)
{
    switch (use) {
    case OperandUse::Inspects:
    case OperandUse::ReadsContents:
    case OperandUse::WritesContents:
    case OperandUse::MovesElements:
    case OperandUse::StoresByKey:
    case OperandUse::DeletesByKey:
    case OperandUse::AssignsFrom:
    case OperandUse::DefinesName:
    case OperandUse::TakesDescriptor:
    case OperandUse::ChangesIntegrity:
    case OperandUse::ChangesPrototype:
        return;
    case OperandUse::Returns:
        if (returnsToCaller(kind))
            solved.reasons.add(EscapeReason::Returned);
        else
            addToDeclaredResults(solved, arrived);
        return;
    case OperandUse::Retains:
    case OperandUse::BecomesPrototype:
        solved.reasons.add(reasonForRetainingTerminator(kind));
        return;
    case OperandUse::PassesToUserCode:
        solved.reasons.add(EscapeReason::PassedToUnknownCode);
        return;
    case OperandUse::Calls:
    case OperandUse::CallsPerElement:
    case OperandUse::CallsRepeatedly:
        for (ObjectID object : arrived.objects)
            callObject(object);
        return;
    case OperandUse::RetainsContents:
        for (ObjectID object : arrived.objects)
            applyToContents(object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
        return;
    case OperandUse::AppliesTransitively:
    case OperandUse::AppliesToContents:
        for (ObjectID object : arrived.objects)
            applyToContents(object, std::nullopt, solved.uses, EscapeReason::RetainedByCallee);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::addToDeclaredResults(SolvedUse& solved, const ArrivedObjects& arrived)
{
    forEachEdgeResult(m_function, solved.use.block, [&](const EdgeResult& result, ValueID parameter) {
        if (result.kind != EdgeResultKind::DeclaredResult || !mayHoldReference(parameter))
            return;
        solved.flowsTo = parameter;
        addAll(parameter, arrived);
    });
}

OperandSummary PointsToSolver::summaryOf(ValueID user, uint32_t operandIndex)
{
    uint32_t& first = m_firstSummaryOfUser[user.index()];
    if (!first) {
        first = m_summaries.size() + 1;
        m_summaries.grow(m_summaries.size() + m_function.at(user).numberOfOperands());
    }
    KeptSummary& kept = m_summaries[first - 1 + operandIndex];
    if (!kept.isKnown) {
        kept.summary = ask(AnswerFromTypes::Question::SummaryOfOperand, user.index(), operandIndex).summary;
        kept.isKnown = true;
    }
    return kept.summary;
}

void PointsToSolver::solveOperand(const ArrivedObjects& arrived, ValueID usedValue, const Use& use)
{
    const Value& user = m_function.at(use.user);
    const DeclaredOperandFacts& facts = declaredOperandFactsOf(user.opcode, use.operandIndex);
    // What answerFromTypes() says then. That the used value is a reference is asked and written down once for all its uses.
    if (!facts.isSameForEveryReference || !isReference(usedValue)) {
        solveOperandWith(arrived, use, summaryOf(use.user, use.operandIndex));
        return;
    }
    if (m_inputs.context.build.options().verifiesAfterEveryPass) [[unlikely]]
        RELEASE_ASSERT(facts.summaryOfReference == summarizeOperand(m_inputs.effects, m_function, user, use.operandIndex));
    if (facts.summaryOfReference.uses.containsAny(usesWithConsequences) || isRetainingOperand(user.opcode, use.operandIndex)) {
        solveOperandWith(arrived, use, facts.summaryOfReference);
        return;
    }

    // Most uses: a test, a cast to a number, a read or a store of a slot. The object goes nowhere.
    SolvedUse solved { .use = use, .uses = facts.summaryOfReference.uses, .isContentsStore = facts.isContentsStore };
    solved.readsContents = mayHoldReference(use.user);
    if (solved.readsContents)
        solveContentsRead(solved, arrived, UserOperand { user, use.operandIndex, facts.summaryOfReference });
    record(solved, arrived);
}

void PointsToSolver::solveOperandWith(const ArrivedObjects& arrived, const Use& use, const OperandSummary& summary)
{
    const Value& user = m_function.at(use.user);
    UserOperand operand { user, use.operandIndex, summary };
    bool userMayHoldReference = mayHoldReference(use.user);
    if (!userMayHoldReference)
        operand.summary.uses.remove(OperandUse::Returns);
    bool storesContents = isContentsStore(operand);
    Vector<ValueID, 4> storedOperands = operandsStoredIn(use.user, operand);
    SolvedUse solved { .use = use, .uses = operand.summary.uses, .isContentsStore = storesContents };
    for (OperandUse operandUse : solved.uses & declaredOperandUses)
        solveOperandUse(solved, arrived, operand, operandUse);
    if (!storedOperands.isEmpty()) {
        for (ObjectID object : arrived.objects)
            addStoredOperands(object, user, storedOperands.span());
    }
    solved.readsContents = userMayHoldReference && mayReadContentsIntoReference(solved, operand);
    if (solved.readsContents)
        solveContentsRead(solved, arrived, operand);
    record(solved, arrived);
}

void PointsToSolver::solveOperandUse(SolvedUse& solved, const ArrivedObjects& arrived, const UserOperand& operand, OperandUse use)
{
    switch (use) {
    case OperandUse::Inspects:
    case OperandUse::ReadsContents:
    case OperandUse::WritesContents:
    case OperandUse::MovesElements:
    case OperandUse::StoresByKey:
    case OperandUse::DeletesByKey:
    case OperandUse::AssignsFrom:
    case OperandUse::DefinesName:
    case OperandUse::TakesDescriptor:
    case OperandUse::ChangesIntegrity:
    case OperandUse::ChangesPrototype:
        return;
    case OperandUse::Returns:
        if (definesObject(solved.use.user) || (isSummarizedCall(operand.user) && m_inputs.context.callResults == CallResults::NotFollowed)) {
            solved.reasons.add(EscapeReason::RetainedByCallee);
            return;
        }
        solved.flowsTo = solved.use.user;
        addAll(solved.flowsTo, arrived);
        return;
    case OperandUse::PassesToUserCode:
        if (isSummarizedCall(operand.user) || !isRetainedInOperand(operand.summary))
            solved.reasons.add(EscapeReason::PassedToUnknownCode);
        return;
    case OperandUse::Retains:
        solveRetainedOperand(solved, arrived, operand);
        return;
    case OperandUse::Calls:
    case OperandUse::CallsPerElement:
    case OperandUse::CallsRepeatedly:
        for (ObjectID object : arrived.objects)
            callObject(object);
        return;
    case OperandUse::RetainsContents:
        for (ObjectID object : arrived.objects)
            applyToContents(object, std::nullopt, retainedContentsUses, EscapeReason::LeakedByContainer);
        return;
    case OperandUse::AppliesTransitively:
    case OperandUse::AppliesToContents:
        for (ObjectID object : arrived.objects)
            applyToContents(object, std::nullopt, operand.summary.uses, EscapeReason::RetainedByCallee);
        return;
    case OperandUse::BecomesPrototype:
        if (!operand.summary.uses.contains(OperandUse::Retains) || operand.summary.retainedIn == RetainedIn::UnknownMemory)
            solved.reasons.add(EscapeReason::StoredInUnknownMemory);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PointsToSolver::solveRetainedOperand(SolvedUse& solved, const ArrivedObjects& arrived, const UserOperand& operand)
{
    switch (operand.summary.retainedIn) {
    case RetainedIn::UnknownMemory:
        solved.reasons.add(isSummarizedCall(operand.user) ? EscapeReason::RetainedByCallee : EscapeReason::StoredInUnknownMemory);
        return;
    case RetainedIn::Result:
        solved.retainedBy = solved.use.user;
        forEachObjectIn(objectsOf(solved.retainedBy).span(), [&](ObjectID container) {
            addContents(container, slotInResult(operand.user, operand.index), arrived.words);
        });
        return;
    case RetainedIn::Operand:
        if (operand.summary.retainingOperand < operand.user.numberOfOperands())
            solved.retainedBy = operand.user.operand(operand.summary.retainingOperand);
        if (!solved.retainedBy) {
            solved.reasons.add(EscapeReason::StoredInUnknownMemory);
            return;
        }
        forEachObjectIn(objectsOf(solved.retainedBy).span(), [&](ObjectID container) {
            addContents(container, slotInContainer(container, operand.user), arrived.words);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ValueID, 4> PointsToSolver::operandsStoredIn(ValueID userID, const UserOperand& operand)
{
    Vector<ValueID, 4> storedOperands;
    const Value& user = operand.user;
    if (!isSummarizedCall(user) && !isRetainingOperand(user.opcode, operand.index))
        return storedOperands;
    user.forEachOperand([&](ValueID stored, uint32_t index) {
        if (index == operand.index || !hasSet(stored))
            return;
        OperandSummary summary = summaryOf(userID, index);
        if (isRetainedInOperand(summary) && summary.retainingOperand == operand.index)
            storedOperands.append(stored);
    });
    return storedOperands;
}

void PointsToSolver::addStoredOperands(ObjectID container, const Value& user, std::span<const ValueID> storedOperands)
{
    if (storedOperands.empty())
        return;
    ObjectSlot slot = slotInContainer(container, user);
    for (ValueID stored : storedOperands)
        addContents(container, slot, objectsOf(stored).span());
}

void PointsToSolver::addOperandsRetainedInResult(ValueID result, const ArrivedObjects& containers)
{
    const Value& user = m_function.at(result);
    if (!isSummarizedCall(user) && !objectFactsOf(user.opcode).retainsOperandInResult)
        return;
    user.forEachOperand([&](ValueID stored, uint32_t index) {
        if (!hasSet(stored))
            return;
        OperandSummary summary = summaryOf(result, index);
        if (!summary.uses.contains(OperandUse::Retains) || summary.retainedIn != RetainedIn::Result)
            return;
        ObjectSlot slot = slotInResult(user, index);
        WordsOfSet objects = objectsOf(stored);
        for (ObjectID container : containers.objects)
            addContents(container, slot, objects.span());
    });
}

bool PointsToSolver::isContentsStore(const UserOperand& operand) const
{
    if (!operand.summary.uses.contains(OperandUse::WritesContents) || isSummarizedCall(operand.user))
        return false;
    Opcode opcode = operand.user.opcode;
    OperandUses ownUses = operandUsesOf(opcode, std::min(operand.index, numberOfNamedOperandsOf(opcode)));
    return ownUses.contains(OperandUse::WritesContents) && !ownUses.containsAny({ OperandUse::Retains, OperandUse::Calls });
}

// An object gets to a value that makes none and is no parameter of a block from an operand: the value may be the operand, refer to it, or be read out of it.
std::optional<Vector<ValueID, 8>> findMakersOfObjectsBehind(FunctionAnalyses& analyses, ValueID value)
{
    constexpr size_t maxDefinitions = 64;
    AOT_THRESHOLD(size_t, maxMakers, 8);

    const Function& function = analyses.function();
    const OpcodeSet& definingOpcodes = opcodesThatMayDefineObjects();
    Vector<ValueID, 8> makers;
    Vector<ValueID, maxDefinitions> definitions { value };
    auto addOperandsOf = [&](const Instruction& instruction) {
        instruction.forEachOperand([&](ValueID operand, uint32_t) {
            definitions.appendIfNotContains(operand);
        });
    };
    for (size_t next = 0; next < definitions.size(); ++next) {
        ValueID id = definitions[next];
        const Value& definition = function.at(id);
        if (definition.opcode != Opcode::Parameter) {
            // As findObjects() tells them apart. What stands in a new object from the start is found in the object.
            if (definingOpcodes.contains(definition.opcode)) {
                if (resultMayBeFresh(definition.opcode) && freshnessOf(analyses.effectContext(), function, definition) != ResultFreshness::Unknown) {
                    makers.append(id);
                    continue;
                }
                if (objectFactsOf(definition.opcode).allocatesOrReturnsOperand)
                    makers.append(id);
                else if (objectFactsOf(definition.opcode).isSuperConstruct && mayBeReference(analyses.program().typeOf(function, id)))
                    continue;
            }
            addOperandsOf(definition);
        } else {
            ValueLocation location = analyses.useLists().locationOf(id);
            if (location.placement != ValuePlacement::Parameter)
                return std::nullopt;
            if (location.block == analyses.cfg().root())
                continue;
            for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
                IncomingValue incoming = incomingValueOf(function, edge, location.index);
                if (incoming.kind == IncomingValue::Kind::Argument) {
                    if (!incoming.argument)
                        return std::nullopt;
                    definitions.appendIfNotContains(incoming.argument);
                    continue;
                }
                std::optional<EdgeResult> result = tryGetEdgeResult(function, incoming.result);
                if (!result || result->kind == EdgeResultKind::DeclaredResult)
                    return std::nullopt;
                addOperandsOf(function.at(edge.from).terminator);
                if (result->subject)
                    definitions.appendIfNotContains(result->subject);
                if (result->test)
                    definitions.appendIfNotContains(result->test);
            }
        }
        if (definitions.size() > maxDefinitions || makers.size() > maxMakers)
            return std::nullopt;
    }
    return makers;
}

// The reasons that solveOperandUse() and solveTerminatorOperandUse() add whatever the sets hold, and what record() and EscapeAnalysis::computeUses() leave out.
UseSeenAlone classifyUseAlone(FunctionAnalyses& analyses, const Use& use)
{
    static constexpr OperandUses usesOfFollowedAlias { OperandUse::Inspects, OperandUse::Returns };

    const Function& function = analyses.function();
    const Program& program = analyses.program();
    switch (use.kind) {
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return UseSeenAlone::Unknown;
    case Use::Kind::BlockSubject:
        return UseSeenAlone::IsListed;
    case Use::Kind::TerminatorOperand: {
        const Terminator& terminator = function.at(use.block).terminator;
        OperandUses uses = summarizeOperand(analyses.effectContext(), function, terminator, use.operandIndex).uses;
        if (uses.containsAny({ OperandUse::Retains, OperandUse::BecomesPrototype, OperandUse::PassesToUserCode }) || (uses.contains(OperandUse::Returns) && returnsToCaller(terminator.kind)))
            return UseSeenAlone::IsListedAsEscape;
        return uses.contains(OperandUse::Returns) && uses.containsOnly(usesOfFollowedAlias) ? UseSeenAlone::Unknown : UseSeenAlone::IsListed;
    }
    case Use::Kind::Operand:
        break;
    }

    const Value& user = function.at(use.user);
    OperandSummary summary = summarizeOperand(analyses.effectContext(), function, user, use.operandIndex);
    bool userMayHoldReference = mayBeReference(program.typeOf(function, use.user)) || numberOfResultsOf(program, function, user).has_value();
    if (!userMayHoldReference)
        summary.uses.remove(OperandUse::Returns);
    bool isRetainedInUnknownMemory = summary.uses.contains(OperandUse::Retains) && summary.retainedIn == RetainedIn::UnknownMemory;
    bool escapes = isRetainedInUnknownMemory
        || (summary.uses.contains(OperandUse::PassesToUserCode) && (isSummarizedCall(user) || !isRetainedInOperand(summary)))
        || (summary.uses.contains(OperandUse::BecomesPrototype) && (!summary.uses.contains(OperandUse::Retains) || isRetainedInUnknownMemory))
        || (summary.uses.contains(OperandUse::Returns) && isSummarizedCall(user) && analyses.context().callResults == CallResults::NotFollowed);
    if (escapes)
        return UseSeenAlone::IsListedAsEscape;
    // Whatever the holder may be is open to others: it is no object of the function, an entry parameter, or was read out of an object that is open.
    if (isRetainedInOperand(summary)) {
        ValueID holder = summary.retainingOperand < user.numberOfOperands() ? user.operand(summary.retainingOperand) : ValueID();
        std::optional<Vector<ValueID, 8>> makers;
        if (holder)
            makers = findMakersOfObjectsBehind(analyses, holder);
        if (!holder || (makers && makers->isEmpty()))
            return UseSeenAlone::IsListedAsEscape;
    }
    if (userMayHoldReference && summary.uses.containsOnly(usesOfFollowedAlias)) {
        bool handsOn = summary.uses.contains(OperandUse::Returns) && !opcodesThatMayDefineObjects().contains(user.opcode);
        // Else it may refer to the operand.
        return handsOn ? UseSeenAlone::HandsOn : UseSeenAlone::Unknown;
    }
    return UseSeenAlone::IsListed;
}

static Vector<SolvedUse> sortedByObjectAndUse(const Vector<SolvedUse>& uses)
{
    Vector<SolvedUse> sorted = uses;
    AOT::sort(sorted, { }, [](const SolvedUse& solved) {
        const Use& use = solved.use;
        return std::tuple { solved.object, use.kind, use.block, use.user, use.operandIndex, use.targetIndex };
    });
    return sorted;
}

static void verifySameResults(const PointsToResults& results, const PointsToResults& repeated)
{
    RELEASE_ASSERT(results.gaveUp == repeated.gaveUp);
    RELEASE_ASSERT(results.objects.size() == repeated.objects.size());
    for (size_t index = 0; index < results.objects.size(); ++index) {
        const SolvedObject& object = results.objects[index];
        const SolvedObject& other = repeated.objects[index];
        RELEASE_ASSERT(object.definition == other.definition && object.kind == other.kind);
        RELEASE_ASSERT(object.reasons == other.reasons && object.appliedUses == other.appliedUses);
    }

    RELEASE_ASSERT(results.setOfValue.size() == repeated.setOfValue.size());
    for (size_t index = 0; index < results.setOfValue.size(); ++index) {
        const ValueSet* set = results.setOf(ValueID(index));
        const ValueSet* other = repeated.setOf(ValueID(index));
        RELEASE_ASSERT(!set == !other);
        if (set)
            RELEASE_ASSERT(set->mayBeUntracked == other->mayBeUntracked && set->objects == other->objects);
    }

    RELEASE_ASSERT(results.uses.size() == repeated.uses.size());
    Vector<SolvedUse> uses = sortedByObjectAndUse(results.uses);
    Vector<SolvedUse> otherUses = sortedByObjectAndUse(repeated.uses);
    for (size_t index = 0; index < uses.size(); ++index) {
        const SolvedUse& solved = uses[index];
        const SolvedUse& other = otherUses[index];
        RELEASE_ASSERT(solved.object == other.object && solved.use == other.use && solved.uses == other.uses && solved.reasons == other.reasons);
        RELEASE_ASSERT(solved.flowsTo == other.flowsTo && solved.retainedBy == other.retainedBy);
        RELEASE_ASSERT(solved.isContentsStore == other.isContentsStore && solved.readsContents == other.readsContents);
    }

    RELEASE_ASSERT(results.contents.size() == repeated.contents.size());
    for (size_t index = 0; index < results.contents.size(); ++index) {
        const SolvedContents& entry = results.contents[index];
        const SolvedContents& other = repeated.contents[index];
        RELEASE_ASSERT(entry.container == other.container && entry.slot == other.slot && entry.object == other.object && entry.isLeaked == other.isLeaked);
    }
}

PointsToResults solvePointsTo(const EscapeAnalysisInputs& inputs)
{
    PointsToResults results = PointsToSolver(inputs, Sweep::Forward).solve();
    if (inputs.context.build.options().verifiesAfterEveryPass) [[unlikely]]
        verifySameResults(results, PointsToSolver(inputs, Sweep::BackwardToVerify).solve());
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
