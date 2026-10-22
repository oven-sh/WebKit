/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestConditions.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRPrinter.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct HashedTest {
    const Value& test;

    friend void add(Hasher& hasher, const HashedTest& hashed)
    {
        add(hasher, static_cast<uint32_t>(hashed.test.opcode));
        hashed.test.addPayloadTo(hasher);
        add(hasher, hashed.test.numberOfOperands());
        for (uint32_t operandIndex = 0; operandIndex < hashed.test.numberOfOperands(); ++operandIndex)
            add(hasher, hashed.test.operand(operandIndex).index());
    }
};

} // anonymous namespace

TestPlaceability placeabilityOf(FunctionAnalyses& analyses, const Value& test)
{
    if (provesOf(test.opcode) == Proof::None)
        return TestPlaceability::Unknown;

    Effects bound = declaredBoundOf(test.opcode);
    if (!bound.writes.isEmpty() || bound.mayExitSideways() || bound.mayCollect)
        return TestPlaceability::Unknown;

    std::optional<unsigned> guard = OperandPosition::guard(test.opcode);
    size_t numberOfValues = analyses.function().values.size();
    for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
        ValueID operand = test.operand(operandIndex);
        bool isAsExpected = operandIndex == guard ? !operand : operand && operand.index() < numberOfValues;
        if (!isAsExpected)
            return TestPlaceability::Unknown;
    }
    return TestPlaceability::CanBePlaced;
}

TestConditions::TestConditions(FunctionAnalyses& analyses)
    : m_identities(analyses.identities())
{
}

TestConditionID TestConditions::add(const Value& test)
{
    Value canonical = canonicalize(test);
    uint32_t hash = hashOf(canonical);
    if (std::optional<TestConditionID> existing = findCanonical(canonical, hash))
        return *existing;

    Proof proof = provesOf(canonical.opcode);
    RELEASE_ASSERT(proof != Proof::None);
    ValueID subject = canonical.numberOfOperands() ? canonical.operand(testedOperandIndex) : ValueID();
    uint32_t index = static_cast<uint32_t>(m_conditions.size());

    TestCondition condition { WTF::move(canonical), proof, subject, { } };
    if (condition.lastsForStretch()) {
        condition.dependsOn = locationsReadBy(condition.test);
        for (const MemoryLocation& location : condition.dependsOn)
            m_heapsDependedOn.add(location.heap());
        m_stretchConditions.set(index);
    }

    m_conditions.append(WTF::move(condition));
    m_index.add(hash, index);
    return TestConditionID(index);
}

std::optional<TestConditionID> TestConditions::find(const Value& test) const
{
    Value canonical = canonicalize(test);
    return findCanonical(canonical, hashOf(canonical));
}

void TestConditions::dump(PrintStream& out, const Program& program, TestConditionID id) const
{
    const Value& test = at(id).test;
    IRPrinter printer(out, program);
    out.print(nameOf(test.opcode));
    printFields(printer, test);
}

Value TestConditions::canonicalize(const Value& test) const
{
    Value canonical = test;
    std::optional<unsigned> guard = OperandPosition::guard(test.opcode);
    for (uint32_t operandIndex = 0; operandIndex < canonical.numberOfOperands(); ++operandIndex) {
        if (operandIndex == guard) {
            canonical.setOperand(operandIndex, ValueID());
            continue;
        }
        if (ValueID operand = canonical.operand(operandIndex))
            canonical.setOperand(operandIndex, m_identities.identityOf(operand));
    }
    canonical.type = TypeID();
    canonical.setExpected(TypeID());
    canonical.representation = Representation::Unchosen;
    canonical.setOrigin({ Instruction::noPosition, 0 });
    return canonical;
}

std::optional<TestConditionID> TestConditions::findCanonical(const Value& canonicalTest, uint32_t hash) const
{
    std::optional<uint32_t> position = m_index.find(hash, [&](uint32_t candidate) {
        return areEqual(m_conditions[candidate].test, canonicalTest);
    });
    if (!position)
        return std::nullopt;
    return TestConditionID(*position);
}

uint32_t TestConditions::hashOf(const Value& canonicalTest)
{
    return computeHash(HashedTest { canonicalTest });
}

bool TestConditions::areEqual(const Value& a, const Value& b)
{
    if (a.opcode != b.opcode || !a.hasSamePayloadAs(b) || a.numberOfOperands() != b.numberOfOperands())
        return false;
    for (uint32_t operandIndex = 0; operandIndex < a.numberOfOperands(); ++operandIndex) {
        if (a.operand(operandIndex) != b.operand(operandIndex))
            return false;
    }
    return true;
}

AllocationProof allocationProofOf(const Program& program, const Function& function, const Value& allocation, const TestCondition& condition)
{
    return allocationProves(program, function, allocation, condition.test) ? AllocationProof::Proves : AllocationProof::Unknown;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
