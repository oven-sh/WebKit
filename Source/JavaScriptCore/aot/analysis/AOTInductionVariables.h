/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTNaturalLoops.h"
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

inline constexpr OpcodeSet relationalOperators { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

Opcode mirroredComparison(Opcode relationalOperator);
Opcode negatedComparison(Opcode relationalOperator);

enum class InductionStepKind : uint8_t { Unknown, Constant, Invariant };
enum class InductionDirection : uint8_t { Unknown, Increasing, Decreasing };
enum class LoopBoundKind : uint8_t { Unknown, ComputedInLoop, Invariant };
enum class TestedValue : uint8_t { BeforeStep, AfterStep };
enum class EntryRelation : uint8_t { Unknown, Holds };
enum class LoopExits : uint8_t { Unknown, AlsoElsewhere, OnlyAtTest };

bool isBoundingRelation(Opcode relationalOperator, InductionDirection);

struct InductionStep {
    InductionStepKind kind { InductionStepKind::Unknown };
    int64_t constant { 0 };
    ValueID invariant;
    bool isSubtracted { false };

    friend bool operator==(const InductionStep&, const InductionStep&) = default;
};

struct InductionTest {
    BlockID block;
    ValueID comparison;
    Opcode relation { Opcode::Less };
    TestedValue tested { TestedValue::BeforeStep };
    ValueID bound;
    LoopBoundKind boundKind { LoopBoundKind::Unknown };
    Edge stayingEdge;
    Edge leavingEdge;
    EntryRelation entryRelation { EntryRelation::Unknown };
    LoopExits exits { LoopExits::Unknown };

    friend bool operator==(const InductionTest&, const InductionTest&) = default;
};

struct InductionVariable {
    ValueID parameter;
    uint32_t parameterIndex { 0 };
    ValueID start;
    InductionStep step;
    InductionDirection direction { InductionDirection::Unknown };
    Vector<ValueID, 1> steppedValues;
    Vector<InductionTest, 1> tests;

    const InductionTest* controllingTest() const LIFETIME_BOUND;
    const InductionTest* boundingTest() const LIFETIME_BOUND;

    friend bool operator==(const InductionVariable&, const InductionVariable&) = default;
};

struct LoopInduction {
    BlockID header;
    unsigned loopIndex { 0 };
    Vector<InductionVariable, 2> variables;

    const InductionVariable* find(ValueID parameter) const LIFETIME_BOUND;
    const InductionVariable* counter() const LIFETIME_BOUND;

    friend bool operator==(const LoopInduction&, const LoopInduction&) = default;
};

class InductionVariables {
    WTF_MAKE_NONCOPYABLE(InductionVariables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(InductionVariables);
public:
    explicit InductionVariables(FunctionAnalyses&);

    std::span<const LoopInduction> loops() const LIFETIME_BOUND { return m_loops.span(); }
    const LoopInduction& of(const NaturalLoop&) const LIFETIME_BOUND;
    const LoopInduction* ofHeader(BlockID) const LIFETIME_BOUND;
    const InductionVariable* find(ValueID headerParameter) const LIFETIME_BOUND;

    void dump(PrintStream&) const;

private:
    Vector<LoopInduction> m_loops;
};

LoopExits exitsOf(FunctionAnalyses&, const NaturalLoop&, BlockID testingBlock);
bool isInvariantIn(FunctionAnalyses&, const NaturalLoop&, ValueID);
void dumpInductionVariable(PrintStream&, const InductionVariable&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
