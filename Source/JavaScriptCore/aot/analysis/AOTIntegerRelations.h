/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTPrimitives.h"
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

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
    void assumeSameValue(ValueID copy, ValueID original);

private:
    enum class AssumedSums : uint8_t { Follow, Stop };

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

    struct UpperBoundGoal {
        ValueID parameter;
        ValueID bound;
        int64_t offset { 0 };
    };

    struct LowerBoundGoal {
        ValueID parameter;
        int64_t minimum { 0 };
    };

    using AssumedWholeParameters = Vector<ValueID, 8>;
    using UpperBoundGoals = Vector<UpperBoundGoal, 8>;
    using LowerBoundGoals = Vector<LowerBoundGoal, 8>;

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    std::optional<Range> assumedRangeOf(ValueID base, std::optional<BlockID> at) const;
    bool hasAssumption(ValueID base, BlockID at) const { return !!assumedRangeOf(base, at); }
    bool isIntegerOrAssumed(ValueID, BlockID at) const;
    bool isIdentityConversion(const Value&) const;
    bool takeStep();

    Term normalize(ValueID id) const { return normalize(id, AssumedSums::Stop, std::nullopt); }
    Term normalize(ValueID, AssumedSums, std::optional<BlockID> at) const;
    std::optional<Range> rangeOf(ValueID base, BlockID at) const;
    static void appendRelationship(Vector<Relationship, 2>&, Term left, Term right, int64_t offset);
    Vector<Relationship, 2> relationshipsOfEdge(const Value& comparison, EdgeSide, ValueID base, BlockID at);
    template<typename Functor> bool forEachDominatingRelationship(ValueID base, BlockID at, const Functor&);

    Answer proveTerms(Term left, Term right, int64_t offset, BlockID at);
    Answer prove(ValueID left, ValueID right, int64_t offset, BlockID at, UpperBoundGoals&);
    Answer proveByInduction(ValueID left, ValueID right, int64_t offset, UpperBoundGoals&);

    Answer proveAtLeast(ValueID base, int64_t minimum, BlockID at, LowerBoundGoals&);
    Answer proveValueAtLeast(ValueID, int64_t minimum, BlockID at, LowerBoundGoals&);
    Answer proveOperationAtLeast(const Value&, int64_t minimum, BlockID at, LowerBoundGoals&);
    Answer proveProductAtLeast(const Value&, int64_t minimum, BlockID at, LowerBoundGoals&);
    Answer proveParameterAtLeast(ValueID, int64_t minimum, LowerBoundGoals&);

    Answer proveWhole(ValueID, BlockID at, AssumedWholeParameters&);
    Answer proveWholeParameter(ValueID, AssumedWholeParameters&);
    bool isKnownWhole(ValueID, BlockID at);

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    Vector<Assumption, 2> m_assumptions;
    Vector<std::pair<ValueID, ValueID>, 4> m_sameValues;
    unsigned m_steps { 0 };
    bool m_didReportLimit { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
