/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <memory>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Lock.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class FlowResults;

enum class InlineBarrier : uint8_t {
    Unknown,
    None,
    NoBody,
    NotInlinable,
    NeverInlined,
};

ASCIILiteral nameOf(InlineBarrier);

// The blocks that one edge dominates, as a range of preorder indices in the dominator tree.
struct DominatedRange {
    bool contains(const DominatedRange& other) const { return begin <= other.begin && other.begin < end; }

    uint32_t begin { 0 };
    uint32_t end { 0 };
};

struct ParameterCondition {
    ValueID condition;
    uint32_t entryParameter { 0 };
    uint32_t bytesOnlyIfTrue { 0 };
    uint32_t bytesOnlyIfFalse { 0 };
    DominatedRange rangeIfTrue;
    DominatedRange rangeIfFalse;
    // Where it is false, control enters InliningSummary::bytesOnlyIfSomeTestFails.
    bool isTestInFrontOfSharedBytes { false };
};

// A switch on a parameter. Where the site passes a constant, one arm stays. The key of a case is its integer, or the index of its string.
struct ParameterSwitch {
    uint32_t entryParameter { 0 };
    bool isOnStrings { false };
    Vector<std::pair<int64_t, uint32_t>, 4> bytesOnlyInCase;
    uint32_t bytesOnlyOtherwise { 0 };
    Vector<DominatedRange, 4> caseRanges;
    DominatedRange rangeOtherwise;
};

// A part of InliningSummary::bytesOnlyIfSomeTestFails, and the innermost side of a condition or a switch that it lies on, which counts it among its own. No range: it lies on none.
struct BytesBehindFailedTests {
    DominatedRange rangeAround;
    uint32_t bytes { 0 };
};

struct ReceiverParameter {
    uint32_t entryParameter { 0 };
    uint32_t accessesByName { 0 };
    uint32_t stores { 0 };
};

struct InliningSummary {
    bool canInlineWholeBody() const { return barrier == InlineBarrier::None && blocksBoundToFrame.isEmpty(); }
    bool canInlineHead() const { return barrier == InlineBarrier::None && !headBlocks.isEmpty(); }

    InlineBarrier barrier { InlineBarrier::Unknown };
    uint32_t codeBytes { 0 };
    uint32_t headBytes { 0 };
    // The generic version of a function is entered where one of several tests of the parameters fails, so no single test owns its bytes. They go where every one of the tests is decided.
    uint32_t bytesOnlyIfSomeTestFails { 0 };
    BitVector headBlocks;
    BitVector noReturnBlocks;
    BitVector blocksBoundToFrame;
    BitVector calledParameters;
    BitVector parametersCalledInCycles;
    BitVector iteratedParameters;
    Vector<ParameterCondition, 2> conditions;
    Vector<ParameterSwitch, 0> switches;
    Vector<BytesBehindFailedTests, 0> partsOfBytesOnlyIfSomeTestFails;
    Vector<ReceiverParameter, 1> receiverParameters;
    bool neverReturns { false };
    bool hasBareReturn { false };
    bool isWrapper { false };
    enum class Answer : uint8_t { NotAsked, No, Yes };
    // The answer of returnsFreshAggregate() for the function, once it is made. Threads that share the summary read and write it as an atomic: each that finds none makes it, and all make the same. InliningSummaries keeps the answers for its own.
    mutable Answer returnsFreshAggregate { Answer::NotAsked };
};

class InliningSummaries {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(InliningSummaries);
public:
    InliningSummaries() = default;

    bool contains(FunctionID id) const { return id.index() < m_summaries.size(); }
    const InliningSummary& of(const Program&, FunctionID id) const LIFETIME_BOUND { return contains(id) ? m_summaries[id.index()] : m_unknown; }
    bool neverReturns(const Program& program, FunctionID id) const { return of(program, id).neverReturns; }
    // What returnsFreshAggregate() says of a function that can be inlined.
    bool returnsFreshAggregate(const Program&, FunctionID) const;

    void dump(PrintStream&, const Program&) const;

private:
    friend InliningSummaries summarizeForInlining(const Program&, const CallGraph&, const Heat&, const FlowResults&, BuildContext&);

    using Answer = InliningSummary::Answer;
    static constexpr unsigned numberOfLocks = 256;

    ParallelArray<InliningSummary> m_summaries;
    InliningSummary m_unknown;
    BuildContext* m_build { nullptr };
    const FlowResults* m_flow { nullptr };
    mutable Vector<Answer> m_returnsFreshAggregate;
    // Whoever makes the answer for a function holds the lock of its index modulo their number.
    std::unique_ptr<std::array<Lock, numberOfLocks>> m_locksOfAnswers;
};

InliningSummaries summarizeForInlining(const Program&, const CallGraph&, const Heat&, const FlowResults&, BuildContext&);

InliningSummary summarizeFunctionForInlining(const AnalysisContext&, const Function&, HeatClass);

bool isBoundToFrame(const Function&, const Block&, uint32_t indexInBlock);
bool isWrapperCreatedByEngine(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
