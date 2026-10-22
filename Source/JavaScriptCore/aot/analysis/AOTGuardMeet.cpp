/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTSchemaOperationColumns.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// Whichever of several origins it was, the subject has one of their layouts, one of their key orders and the slots all of them fill. The others say the same for every origin.
static constexpr ProofSet proofsUnitedOverOrigins {
    Proof::Shape,
    Proof::OwnInstance,
    Proof::SlotFilled,
    Proof::NoExtraProperties,
    Proof::NoFieldOutsideSlot,
    Proof::BirthFieldsFilled,
    Proof::NotFrozen,
    Proof::KeyOrder,
};

// On the way into a block a parameter takes the place of the value passed for it: what held for that value holds for the parameter,
// and what held for the value a parameter had before says nothing about the value it has now.
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

// Nothing when no fact from before the edge is about the subject: the edge fills the parameter with a result of its own.
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

static bool haveSameCondition(FunctionAnalyses& analyses, const Value& a, const Value& b, uint32_t firstComparedOperand)
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

bool haveSameCondition(FunctionAnalyses& analyses, const Value& a, const Value& b)
{
    return haveSameCondition(analyses, a, b, 0);
}

// The subject of a fact may be a parameter that took the place of what its test names, so whoever compares origins compares the subjects of the facts.
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

bool GuardAvailability::haveSameOrigins(const GuardFact& a, const GuardFact& b) const
{
    auto hasOriginsOf = [&](const GuardFact& fact, const GuardFact& other) {
        return std::ranges::all_of(other.origins, [&](const GuardOrigin& origin) {
            return std::ranges::any_of(fact.origins, [&](const GuardOrigin& candidate) {
                return haveSameOrigin(candidate, origin);
            });
        });
    };
    return hasOriginsOf(a, b) && hasOriginsOf(b, a);
}

bool GuardAvailability::areSameFact(const GuardFact& a, const GuardFact& b) const
{
    return a.proofs == b.proofs && a.subject == b.subject && a.dependsOn == b.dependsOn && haveSameOrigins(a, b);
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

static std::span<const uint32_t> fieldsListedBy(const Value& allocation)
{
    return Value::opcodesWithFields.contains(allocation.opcode) ? allocation.fields() : std::span<const uint32_t>();
}

// What an allocation proves depends on the shape words of its own layout only, so it lasts as long for an object of that layout and no other.
static bool allocationImplies(const Program& program, const Value& allocation, const Value& impliedAllocation, Proof proof)
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
        return program.at(allocation.type).singleBuiltinClass()->arrayStorage == program.at(impliedAllocation.type).singleBuiltinClass()->arrayStorage;
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
    case Proof::PlainData:
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
            return !proofs.contains(proof) || allocationImplies(program, m_function.at(origin.test), m_function.at(implied.test), proof);
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

// Whether the fact says about its subject all that the other one says about its own.
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

bool GuardAvailability::implies(const GuardFact& fact, const GuardFact& implied) const
{
    return fact.subject == implied.subject && isAsStrongAs(fact, implied);
}

std::optional<uint32_t> GuardAvailability::findSameFact(const GuardFacts& facts, const GuardFact& fact) const
{
    for (uint32_t index = 0; index < facts.facts.size(); ++index) {
        if (areSameFact(facts.facts[index], fact))
            return index;
    }
    return std::nullopt;
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
    // What holds on both sides is the weaker of two facts, and what the weaker one read decides how long it holds.
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

// No use of an unspecified value is ever reached, so an edge that passes one for a parameter says nothing against a fact about the parameter.
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

ProofSet GuardAvailability::proofsToUnite(const GuardFact& a, const GuardFact& b) const
{
    ProofSet proofs;
    for (Proof proof : allMembersOf<Proof>) {
        if (!proofsUnitedOverOrigins.contains(proof) || !a.proves(proof) || !b.proves(proof))
            continue;
        if (proof == Proof::SlotFilled && !fillSameSlot(m_analyses.program(), m_function, a, b))
            continue;
        proofs.add(proof);
    }
    return proofs;
}

// The fact becomes what holds whether it or a fact of the other way is the one that was proven, and it lasts only while both would.
bool GuardAvailability::tryWiden(GuardFact& fact, const GuardFacts& factsOnOtherWay) const
{
    const GuardFact* partner = nullptr;
    ProofSet proofs;
    for (const GuardFact& candidate : factsOnOtherWay.facts) {
        if (candidate.subject != fact.subject)
            continue;
        ProofSet united = proofsToUnite(fact, candidate);
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
    std::ranges::sort(fact.dependsOn);
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
            if (!findSameFact(result, fact))
                result.facts.append(fact);
        }
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
