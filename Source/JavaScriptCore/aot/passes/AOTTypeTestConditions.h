/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTTypeTable.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using TestConditionID = ID<struct TestConditionIDTag>;

struct TestCondition {
    Value test;
    Proof proof { Proof::None };
    ValueID subject;
    Vector<MemoryLocation, 2> dependsOn;

    bool lastsForever() const { return provesForever(proof); }
    bool lastsForStretch() const { return provesForStretch(proof); }
};

enum class TestPlaceability : uint8_t { Unknown, CanBePlaced };

TestPlaceability placeabilityOf(FunctionAnalyses&, const Value& test);

class TestConditions {
    WTF_MAKE_NONCOPYABLE(TestConditions);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TestConditions);
public:
    explicit TestConditions(FunctionAnalyses&);

    TestConditionID add(const Value& test);
    std::optional<TestConditionID> find(const Value& test) const;

    const TestCondition& at(TestConditionID id) const LIFETIME_BOUND { return m_conditions[id.index()]; }
    unsigned size() const { return m_conditions.size(); }

    const BitVector& stretchConditions() const LIFETIME_BOUND { return m_stretchConditions; }
    AbstractHeapSet heapsDependedOn() const { return m_heapsDependedOn; }

    void dump(PrintStream&, const Program&, TestConditionID) const;

private:
    Value canonicalize(const Value& test) const;
    std::optional<TestConditionID> findCanonical(const Value& canonicalTest, uint32_t hash) const;
    static uint32_t hashOf(const Value& canonicalTest);
    static bool areEqual(const Value& canonicalTest, const Value& otherCanonicalTest);

    const ValueIdentities& m_identities;
    Vector<TestCondition> m_conditions;
    HashedIndex m_index;
    BitVector m_stretchConditions;
    AbstractHeapSet m_heapsDependedOn;
};

// The conditions that a test found false on some way into a block that has not passed through normal code since.
class FailedConditions {
    WTF_MAKE_NONCOPYABLE(FailedConditions);
public:
    explicit FailedConditions(FunctionAnalyses&);

    bool mayHaveFailed(const Value& test, BlockID at) const;
    bool mayHaveFailedTestOf(ValueID subject, BlockID at) const;

private:
    TestConditions m_conditions;
    Vector<std::optional<BitVector>> m_failedAtHead;
};

enum class AllocationProof : uint8_t { Unknown, Proves };

AllocationProof allocationProofOf(const Program&, const Function&, const Value& allocation, const TestCondition&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
