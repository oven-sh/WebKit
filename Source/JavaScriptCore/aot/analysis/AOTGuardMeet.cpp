/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTSchemaOperationColumns.h"
#include "AOTSort.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ProofSet proofsUnitedOverOrigins { Proof::Shape, Proof::OwnInstance, Proof::SlotFilled, Proof::NoExtraProperties, Proof::NoFieldOutsideSlot, Proof::BirthFieldsFilled, Proof::NotFrozen, Proof::KeyOrder };

GuardFacts GuardAvailability::factsAfterEdge(const GuardFacts& factsBeforeEdge, Edge edge) const
{
    if (factsBeforeEdge.facts.isEmpty())
        return factsBeforeEdge;

    BlockID target = targetOf(m_function, edge).block;
    const UseLists& useLists = m_analyses.useLists();
    GuardFacts result { GuardKnowledge::Known, { } };
    for (const GuardFact& fact : factsBeforeEdge.facts) {
        ValueLocation location = fact.subject ? useLists.locationOf(fact.subject) : ValueLocation();
        if (location.placement != ValuePlacement::Parameter || location.block != target)
            result.facts.append(fact);
    }

    auto& parameters = m_function.at(target).parameters;
    for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
        ValueID parameter = parameters[parameterIndex];
        if (isGuardValue(m_function, parameter) || m_analyses.identityOf(parameter) != parameter)
            continue;
        ValueID passed = valuePassedOn(m_function, edge, parameterIndex);
        if (!passed)
            continue;
        ValueID subject = m_analyses.identityOf(passed);
        for (const GuardFact& fact : factsBeforeEdge.facts) {
            if (fact.subject != subject)
                continue;
            result.facts.append(fact);
            result.facts.last().subject = parameter;
        }
    }
    return result;
}

std::optional<ValueID> GuardAvailability::subjectBeforeEdge(ValueID subjectAfterEdge, Edge edge) const
{
    if (!subjectAfterEdge)
        return subjectAfterEdge;
    ValueLocation location = m_analyses.useLists().locationOf(subjectAfterEdge);
    if (location.placement != ValuePlacement::Parameter || location.block != targetOf(m_function, edge).block)
        return subjectAfterEdge;
    ValueID passed = valuePassedOn(m_function, edge, location.index);
    if (!passed)
        return std::nullopt;
    return m_analyses.identityOf(passed);
}

bool haveSameCondition(FunctionAnalyses& analyses, const Value& a, const Value& b, uint32_t firstComparedOperand)
{
    if (a.opcode != b.opcode || !a.hasSamePayloadAs(b) || a.numberOfOperands() != b.numberOfOperands())
        return false;
    for (uint32_t operandIndex = firstComparedOperand; operandIndex < a.numberOfOperands(); ++operandIndex) {
        if (a.operandKind(operandIndex) == OperandKind::Guard)
            continue;
        ValueID left = a.operand(operandIndex);
        ValueID right = b.operand(operandIndex);
        if (!left || !right) {
            if (left != right)
                return false;
            continue;
        }
        if (analyses.identityOf(left) != analyses.identityOf(right))
            return false;
    }
    return true;
}

bool GuardAvailability::haveSameOrigin(const GuardOrigin& a, const GuardOrigin& b) const
{
    if (a.source != b.source)
        return false;
    switch (a.source) {
    case GuardSource::Test:
        return a.test == b.test || haveSameCondition(m_analyses, m_function.at(a.test), m_function.at(b.test), subjectOperandIndex + 1);
    case GuardSource::SwitchCase:
        return m_function.at(a.switchBlock).terminator.layoutSets()[a.caseIndex] == m_function.at(b.switchBlock).terminator.layoutSets()[b.caseIndex];
    case GuardSource::Allocation:
        return a.test == b.test;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool GuardAvailability::areSameFact(const GuardFact& a, const GuardFact& b) const
{
    auto hasOriginsOf = [&](const GuardFact& fact, const GuardFact& other) {
        return std::ranges::all_of(other.origins, [&](const GuardOrigin& origin) {
            return std::ranges::any_of(fact.origins, [&](const GuardOrigin& candidate) {
                return haveSameOrigin(candidate, origin);
            });
        });
    };
    return a.proofs == b.proofs && a.subject == b.subject && a.dependsOn == b.dependsOn && hasOriginsOf(a, b) && hasOriginsOf(b, a);
}

static bool testImplies(const Value& test, const Value& impliedTest)
{
    if (test.opcode != impliedTest.opcode)
        return false;
    Proof proof = provesOf(test.opcode);
    if (proof == Proof::SlotFilled)
        return test.key() == impliedTest.key() && (test.holdsDeclaredType() || !impliedTest.holdsDeclaredType());
    if (proof == Proof::Pristine && Value::opcodesWithAspects.contains(test.opcode))
        return test.builtinClass() == impliedTest.builtinClass() && test.aspects().containsAll(impliedTest.aspects());
    if (proof == Proof::ArrayStorage)
        return test.arrayStorage() == impliedTest.arrayStorage() && test.isWritable();
    return false;
}

static std::optional<EdgeResult> findOnlyProofBehind(const Function& function, ValueID guard)
{
    std::optional<Edge> onlyEdge;
    uint32_t parameterIndex = 0;
    for (uint32_t from = 0; from < function.blocks.size(); ++from) {
        bool entersTwice = false;
        forEachOutgoingEdge(function, BlockID(from), [&](Edge edge, const Target& target) {
            size_t index = function.at(target.block).parameters.find(guard);
            if (index == notFound)
                return;
            entersTwice |= !!onlyEdge;
            onlyEdge = edge;
            parameterIndex = static_cast<uint32_t>(index);
        });
        if (entersTwice)
            return std::nullopt;
    }
    if (!onlyEdge)
        return std::nullopt;
    IncomingValue incoming = incomingValueOf(function, *onlyEdge, parameterIndex);
    if (incoming.kind != IncomingValue::Kind::EdgeResult)
        return std::nullopt;
    std::optional<EdgeResult> result = tryGetEdgeResult(function, incoming.result);
    return result && result->kind == EdgeResultKind::Guard && result->test ? result : std::nullopt;
}

static bool asksSameOfSubject(const Function& function, const Value& test, const Value& other)
{
    if (!test.hasSamePayloadAs(other) || test.numberOfOperands() != other.numberOfOperands())
        return false;
    for (uint32_t operandIndex = subjectOperandIndex + 1; operandIndex < test.numberOfOperands(); ++operandIndex) {
        if (test.operandKind(operandIndex) == OperandKind::Guard)
            continue;
        ValueID operand = test.operand(operandIndex);
        ValueID operandOfOther = other.operand(operandIndex);
        if (!operand != !operandOfOther || (operand && definitionThroughUpcasts(function, operand) != definitionThroughUpcasts(function, operandOfOther)))
            return false;
    }
    return true;
}

// Each access names the kind of storage that it relies on, and is held to its guard. What the kind says of the elements answers questions about them, and of an answered question nothing is left.
static bool saysAsMuchOfElements(const Program& program, const Type& array, ArrayStorage storage, ArrayStorage other)
{
    Type heldInOther = typeOfElementRead(other);
    if (typeOfElementRead(storage).isSubsetOf(heldInOther, program))
        return true;
    const ClassPart* part = array.singleBuiltinClass();
    std::optional<ProvenValue<TypeID>> elements = part ? part->provenContentsAt(program.builtins(), ContentsSlot::Element) : std::nullopt;
    return elements && program.at(*elements).isSubsetOf(heldInOther, program);
}

bool provesAsMuchAs(const Program& program, const Function& function, ValueID guard, ValueID other)
{
    if (guard == other)
        return true;
    std::optional<EdgeResult> proof = findOnlyProofBehind(function, guard);
    std::optional<EdgeResult> proofOfOther = findOnlyProofBehind(function, other);
    if (!proof || !proofOfOther || proof->proof != proofOfOther->proof || proof->side != proofOfOther->side || !proof->subject || !proofOfOther->subject)
        return false;
    if (definitionThroughUpcasts(function, proof->subject) != definitionThroughUpcasts(function, proofOfOther->subject) || !function.at(proof->subject).type)
        return false;
    const Value& test = function.at(proof->test);
    const Value& testOfOther = function.at(proofOfOther->test);
    if (test.opcode != testOfOther.opcode)
        return false;
    if (asksSameOfSubject(function, test, testOfOther) || testImplies(test, testOfOther))
        return true;
    return proof->proof == Proof::ArrayStorage && proof->side == EdgeSide::Taken && saysAsMuchOfElements(program, program.typeOf(function, proof->subject), test.arrayStorage(), testOfOther.arrayStorage());
}

static std::span<const uint32_t> fieldsListedBy(const Value& allocation)
{
    return Value::opcodesWithFields.contains(allocation.opcode) ? allocation.fields() : std::span<const uint32_t>();
}

static bool allocationImplies(const Program& program, const Function& function, const Value& allocation, const Value& impliedAllocation, Proof proof)
{
    switch (proof) {
    case Proof::Shape:
    case Proof::OwnInstance:
    case Proof::NoExtraProperties:
    case Proof::NoFieldOutsideSlot:
    case Proof::BirthFieldsFilled:
    case Proof::NotFrozen:
        return allocation.layout() == impliedAllocation.layout();
    case Proof::SlotFilled:
        return allocation.layout() == impliedAllocation.layout() && std::ranges::all_of(fieldsListedBy(impliedAllocation), [&](uint32_t field) {
            return std::ranges::contains(fieldsListedBy(allocation), field);
        });
    case Proof::KeyOrder:
        return allocation.layout() == impliedAllocation.layout() && std::ranges::equal(fieldsListedBy(allocation), fieldsListedBy(impliedAllocation));
    case Proof::Pristine:
        return program.at(allocation.type).singleBuiltinClass()->builtinClass == program.at(impliedAllocation.type).singleBuiltinClass()->builtinClass;
    case Proof::ArrayStorage:
        return arrayStorageOfNewArray(program, function, allocation) == arrayStorageOfNewArray(program, function, impliedAllocation) && (allocatesWritableStorage(function, allocation) || !allocatesWritableStorage(function, impliedAllocation));
    case Proof::None:
    case Proof::TestedType:
    case Proof::Truthiness:
    case Proof::Equality:
    case Proof::IntegerBound:
    case Proof::ExactFunction:
    case Proof::CaseValue:
    case Proof::NotNullish:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::PrototypeLinkUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::SourceOpen:
    case Proof::TextMatches:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool GuardAvailability::originImplies(const GuardOrigin& origin, const GuardOrigin& implied, ProofSet proofs) const
{
    if (haveSameOrigin(origin, implied))
        return true;

    const Program& program = m_analyses.program();
    switch (implied.source) {
    case GuardSource::Test:
        break;
    case GuardSource::SwitchCase:
        return false;
    case GuardSource::Allocation:
        return origin.source == GuardSource::Allocation && std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
            return !proofs.contains(proof) || allocationImplies(program, m_function, m_function.at(origin.test), m_function.at(implied.test), proof);
        });
    }

    const Value& impliedTest = m_function.at(implied.test);
    switch (origin.source) {
    case GuardSource::Test:
        return testImplies(m_function.at(origin.test), impliedTest);
    case GuardSource::SwitchCase:
        return false;
    case GuardSource::Allocation:
        return allocationProves(program, m_function, m_function.at(origin.test), impliedTest);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool GuardAvailability::isAsStrongAs(const GuardFact& fact, const GuardFact& other) const
{
    if ((fact.proofs | other.proofs) != fact.proofs)
        return false;

    if (other.proofs == ProofSet { Proof::Shape })
        return provenLayoutsAreSubsetOf(m_analyses.program(), m_function, fact, other);

    return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
        return std::ranges::any_of(other.origins, [&](const GuardOrigin& otherOrigin) {
            return originImplies(origin, otherOrigin, other.proofs);
        });
    });
}

bool GuardAvailability::anyImplies(const GuardFacts& facts, const GuardFact& implied) const
{
    return std::ranges::any_of(facts.facts, [&](const GuardFact& fact) {
        return implies(fact, implied);
    });
}

bool GuardAvailability::hasEquivalentFact(const GuardFacts& facts, const GuardFact& fact) const
{
    return std::ranges::any_of(facts.facts, [&](const GuardFact& other) {
        return implies(other, fact) && implies(fact, other);
    });
}

GuardFacts GuardAvailability::meetFacts(const GuardFacts& a, const GuardFacts& b) const
{
    if (!a.isKnown() || !b.isKnown())
        return { };
    GuardFacts result { GuardKnowledge::Known, { } };
    auto keepFactsImpliedBy = [&](const GuardFacts& other, const GuardFacts& candidates) {
        for (const GuardFact& candidate : candidates.facts) {
            if (anyImplies(other, candidate) && !hasEquivalentFact(result, candidate))
                result.facts.append(candidate);
        }
    };
    keepFactsImpliedBy(b, a);
    keepFactsImpliedBy(a, b);
    return result;
}

bool GuardAvailability::leavesUnspecified(Edge edge, ValueID subjectAfterEdge) const
{
    std::optional<ValueID> subject = subjectBeforeEdge(subjectAfterEdge, edge);
    return subject && *subject && m_function.at(*subject).opcode == Opcode::Unspecified;
}

static bool fillSameSlot(const Program& program, const Function& function, const GuardFact& a, const GuardFact& b)
{
    auto isFilledInBoth = [&](PropertyKeyID key) {
        return provesSlotFilled(program, function, a, key) && provesSlotFilled(program, function, b, key);
    };
    const GuardOrigin& origin = a.origins.first();
    switch (origin.source) {
    case GuardSource::Test:
        return isFilledInBoth(function.at(origin.test).key());
    case GuardSource::SwitchCase:
        return false;
    case GuardSource::Allocation: {
        const Value& allocation = function.at(origin.test);
        return std::ranges::any_of(fieldsListedBy(allocation), [&](uint32_t field) {
            return isFilledInBoth(fieldAt(program, { allocation.layout(), field }).key);
        });
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool GuardAvailability::tryWiden(GuardFact& fact, const GuardFacts& factsOnOtherWay) const
{
    const GuardFact* partner = nullptr;
    ProofSet proofs;
    for (const GuardFact& candidate : factsOnOtherWay.facts) {
        if (candidate.subject != fact.subject)
            continue;
        ProofSet united;
        for (Proof proof : allMembersOf<Proof>) {
            if (!proofsUnitedOverOrigins.contains(proof) || !fact.proves(proof) || !candidate.proves(proof))
                continue;
            if (proof != Proof::SlotFilled || fillSameSlot(m_analyses.program(), m_function, fact, candidate))
                united.add(proof);
        }
        if (united.isEmpty() || (partner && united != fact.proofs))
            continue;
        partner = &candidate;
        proofs = united;
        if (proofs == fact.proofs)
            break;
    }
    if (!partner)
        return false;

    fact.proofs = proofs;
    for (const GuardOrigin& origin : partner->origins) {
        bool isCovered = std::ranges::any_of(fact.origins, [&](const GuardOrigin& existing) {
            return originImplies(origin, existing, proofs);
        });
        if (isCovered)
            continue;
        fact.origins.removeAllMatching([&](const GuardOrigin& existing) {
            return originImplies(existing, origin, proofs);
        });
        fact.origins.append(origin);
    }
    for (const MemoryLocation& location : partner->dependsOn) {
        if (!fact.dependsOn.contains(location))
            fact.dependsOn.append(location);
    }
    AOT::sort(fact.dependsOn);
    return true;
}

GuardFacts GuardAvailability::meetFactsAfterEdges(std::span<const Edge> edges, std::span<const GuardFacts> factsAfterEdges) const
{
    if (!std::ranges::all_of(factsAfterEdges, &GuardFacts::isKnown))
        return { };

    auto holdsAfterEveryEdge = [&](const GuardFact& fact) {
        for (size_t index = 0; index < edges.size(); ++index) {
            if (!anyImplies(factsAfterEdges[index], fact) && !leavesUnspecified(edges[index], fact.subject))
                return false;
        }
        return true;
    };

    GuardFacts result { GuardKnowledge::Known, { } };
    for (const GuardFacts& candidates : factsAfterEdges) {
        for (const GuardFact& candidate : candidates.facts) {
            if (!hasEquivalentFact(result, candidate) && holdsAfterEveryEdge(candidate))
                result.facts.append(candidate);
        }
    }

    for (const GuardFacts& candidates : factsAfterEdges) {
        for (const GuardFact& candidate : candidates.facts) {
            if (anyImplies(result, candidate))
                continue;
            GuardFact widened = candidate;
            bool canWiden = true;
            for (size_t index = 0; index < edges.size() && canWiden; ++index) {
                if (!anyImplies(factsAfterEdges[index], widened) && !leavesUnspecified(edges[index], widened.subject))
                    canWiden = tryWiden(widened, factsAfterEdges[index]);
            }
            if (canWiden && !anyImplies(result, widened) && holdsAfterEveryEdge(widened))
                result.facts.append(WTF::move(widened));
        }
    }
    return result;
}

GuardFacts GuardAvailability::uniteFacts(std::span<const ValueID> guards) const
{
    GuardFacts result { GuardKnowledge::Known, { } };
    for (ValueID guard : guards) {
        const GuardFacts& facts = factsOf(guard);
        if (!facts.isKnown())
            return { };
        for (const GuardFact& fact : facts.facts) {
            bool isListed = std::ranges::any_of(result.facts, [&](const GuardFact& listed) {
                return areSameFact(listed, fact);
            });
            if (!isListed)
                result.facts.append(fact);
        }
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
