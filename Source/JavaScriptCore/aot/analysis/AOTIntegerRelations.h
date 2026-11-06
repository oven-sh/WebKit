/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTPrimitives.h"
#include "AOTVector.h"
#include <optional>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class IntegerRelations {
    WTF_MAKE_NONCOPYABLE(IntegerRelations);
public:
    explicit IntegerRelations(FunctionAnalyses&);

    enum class Answer : uint8_t { Unknown, Proven };

    Answer isAtMost(ValueID left, ValueID right, int64_t offset, BlockID at);
    Answer isAtLeast(ValueID, int64_t minimum, BlockID at);
    Answer isAtMost(ValueID, int64_t maximum, BlockID at);
    Answer isWholeNumber(ValueID, BlockID at);

    void assumeRange(ValueID, Range, BlockID scope);

private:
    struct Term {
        ValueID base;
        int64_t offset { 0 };
        bool usedAssumedSum { false };
    };

    struct Relationship {
        ValueID left;
        ValueID right;
        int64_t offset { 0 };
    };

    struct Assumption {
        ValueID value;
        Range range;
        BlockID scope;
    };

    struct LowerBoundGoal {
        ValueID parameter;
        int64_t minimum { 0 };
    };

    using AssumedWholeParameters = Vector<ValueID, 8>;
    using UpperBoundGoals = Vector<Relationship, 8>;
    using LowerBoundGoals = Vector<LowerBoundGoal, 8>;

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    static Answer answerOf(bool isProven) { return isProven ? Answer::Proven : Answer::Unknown; }
    std::optional<Range> assumedRangeOf(ValueID base, BlockID at) const;
    bool hasAssumption(ValueID base, BlockID at) const { return !!assumedRangeOf(base, at); }
    bool isIntegerOrAssumed(ValueID id, BlockID at) const { return typeOf(id).isInteger() || hasAssumption(normalize(id).base, at); }
    bool isNumberOrAssumed(ValueID id, BlockID at) const { return typeOf(id).isNumber() || hasAssumption(normalize(id).base, at); }
    bool isIdentityConversion(const Value&) const;
    bool takeStep();

    Term normalize(ValueID, std::optional<BlockID> placeOfAssumedSums = std::nullopt) const;
    std::optional<Range> rangeOf(ValueID base, BlockID at) const;
    static void appendRelationship(Vector<Relationship, 2>&, Term left, Term right, int64_t offset);
    Vector<Relationship, 2> relationshipsOfEdge(const Value& comparison, EdgeSide, ValueID base, BlockID at);
    template<typename Functor> bool forEachDominatingRelationship(ValueID base, BlockID at, const Functor&);
    bool isEnteredOverEdgeOfComparison(BlockID) const;
    const Value& comparisonOnEdgeInto(BlockID) const;
    BlockID nextBlockBehindComparison(BlockID);
    bool comparisonOnEdgeIntoIsAbout(BlockID, ValueID base);

    bool proveTerms(Term left, Term right, int64_t offset, BlockID at);
    bool prove(ValueID left, ValueID right, int64_t offset, BlockID at, UpperBoundGoals&);
    bool proveByInduction(ValueID left, ValueID right, int64_t offset, UpperBoundGoals&);
    bool proveRoundedSum(ValueID left, ValueID right, int64_t offset, BlockID at, UpperBoundGoals&);

    bool proveAtLeast(ValueID base, int64_t minimum, BlockID at, LowerBoundGoals&);
    bool proveValueAtLeast(ValueID, int64_t minimum, BlockID at, LowerBoundGoals&);
    bool proveProductAtLeast(const Value&, int64_t minimum, BlockID at, LowerBoundGoals&);
    bool proveSumAtLeast(const Value&, int64_t minimum, BlockID at, LowerBoundGoals&);
    bool proveParameterAtLeast(ValueID, int64_t minimum, LowerBoundGoals&);

    bool proveWhole(ValueID, BlockID at, AssumedWholeParameters&);
    bool isKnownWhole(ValueID, BlockID at);

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    Vector<Assumption, 2> m_assumptions;
    Vector<uint32_t> m_nextBlockBehindComparison;
    struct ComparedBases {
        ValueID left;
        ValueID right;
        bool isKnown { false };
    };
    Vector<ComparedBases> m_comparedBases;
    void indexComparisonsByBase();
    HashMap<uint32_t, Vector<BlockID, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksBehindComparisonsAbout;
    Vector<uint32_t> m_depthInDominatorTree;
    size_t m_numberOfBlocksWalked { 0 };
    bool m_hasIndexOfComparisons { false };
    unsigned m_steps { 0 };
    bool m_didReportLimit { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
