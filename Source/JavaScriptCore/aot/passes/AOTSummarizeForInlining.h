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
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Lock.h>

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

// The blocks that one edge dominates, as a range of preorder indices in the dominator tree.
struct DominatedRange {
    bool contains(const DominatedRange& other) const { return begin <= other.begin && other.begin < end; }
    bool isEmpty() const { return begin == end; }

    uint32_t begin { 0 };
    uint32_t end { 0 };
};

// Code, and the innermost side of a condition or a switch that it lies on, which counts it among its own. No range: it lies on none.
struct BytesInRange {
    DominatedRange range;
    uint32_t bytes { 0 };
};

struct ParameterCondition {
    BlockID block;
    ValueID condition;
    uint32_t entryParameter { 0 };
    BytesInRange onlyIfTrue;
    BytesInRange onlyIfFalse;
    // The test and its branch, which go where the site decides the condition.
    BytesInRange test;
    // Where it is false, control enters InliningSummary::bytesOnlyIfSomeTestFails.
    bool isTestInFrontOfSharedBytes { false };
};

// A switch on a parameter, or on the length of the list that it holds. Where the site passes a constant, or makes the list, one arm stays. The key of a case is its integer, or the index of its string.
struct ParameterSwitch {
    BlockID block;
    uint32_t entryParameter { 0 };
    bool isOnStrings { false };
    bool isOnLength { false };
    Vector<std::pair<int64_t, BytesInRange>, 4> onlyInCase;
    BytesInRange onlyOtherwise;
    BytesInRange test;
};

// `g(a, ...list)`: what it takes to make a list and to spread into it the list that a parameter holds. Where the site makes the list of the parameter, the copy passes its elements one by one.
struct SpreadOfParameter {
    uint32_t entryParameter { 0 };
    BytesInRange code;
};

struct ReceiverParameter {
    uint32_t entryParameter { 0 };
    uint32_t accessesByName { 0 };
    uint32_t stores { 0 };
};

// A parameter under which objects are read: `events[type]`.
struct KeyParameter {
    uint32_t entryParameter { 0 };
    uint32_t accesses { 0 };
};

struct InliningSummary {
    bool canInlineWholeBody() const { return barrier == InlineBarrier::None && blocksBoundToFrame.isEmpty(); }
    bool canInlineHead() const { return barrier == InlineBarrier::None && !headBlocks.isEmpty(); }
    void shrinkToFit();

    bool testsParentOfClass { false };
    InlineBarrier barrier { InlineBarrier::Unknown };
    bool neverReturns { false };
    // Nothing in it loops: it does not spin, it ends by throwing.
    bool onlyThrows { false };
    bool hasCycle { false };
    bool hasBareReturn { false };
    bool isWrapper { false };
    // Its rest parameter is only spread into its calls, and it is short.
    bool onlyForwardsRestParameter { false };
    // `(a, b) => a - b`: handed two numbers, the function answers with their difference and does nothing else.
    enum class Difference : uint8_t { None, FirstMinusSecond, SecondMinusFirst };
    Difference difference { Difference::None };
    enum class Answer : uint8_t { NotAsked, No, Yes };
    // The answer of returnsFreshAggregate() for the function, once it is made. Threads that share the summary read and write it as an atomic: each that finds none makes it, and all make the same.
    mutable Answer returnsFreshAggregate { Answer::NotAsked };
    uint32_t codeBytes { 0 };
    // The larger of two parts: the code that is expected to run, without the tests of declared types, and the code that is not.
    uint32_t likelyCodeBytes { 0 };
    uint32_t headBytes { 0 };
    // The generic version of a function is entered where one of several tests of the parameters fails, so no single test owns its bytes. They go where every one of the tests is decided.
    uint32_t bytesOnlyIfSomeTestFails { 0 };
    BitVector headBlocks;
    BitVector noReturnBlocks;
    BitVector blocksBoundToFrame;
    BitVector calledParameters;
    BitVector parametersCalledInCycles;
    // In a loop that stands in a loop.
    BitVector parametersCalledInNestedLoops;
    // sort() calls no comparator: it hands the comparator to the function that compares. The parameters of library code that reach a called parameter
    // by calls that are made whenever the function runs, and where each is handed on.
    struct ForwardedParameter {
        uint32_t entryParameter { 0 };
        FunctionID callee;
        uint32_t calleeParameter { 0 };
    };
    BitVector forwardedParameters;
    BitVector iteratedParameters;
    CompactList<ForwardedParameter> forwards;
    CompactList<ParameterCondition> conditions;
    Vector<ParameterSwitch, 0> switches;
    CompactList<BytesInRange> partsOfBytesOnlyIfSomeTestFails;
    CompactList<SpreadOfParameter> spreadsOfParameters;
    CompactList<ReceiverParameter> receiverParameters;
    CompactList<KeyParameter> keyParameters;
    // It returns what these answer when handed its two parameters: the versions behind the tests of the declared types.
    struct DifferenceCall {
        FunctionID callee;
        bool swapsParameters { false };
    };
    CompactList<DifferenceCall> differenceCalls;
};
static_assert(sizeof(InliningSummary) <= 168);

class InliningSummaries {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(InliningSummaries);
public:
    InliningSummaries() = default;

    bool contains(FunctionID id) const { return id.index() < m_positions.size(); }
    const InliningSummary& of(const Program&, FunctionID id) const LIFETIME_BOUND { return contains(id) ? m_summaries[m_positions[id.index()]] : m_unknown; }
    bool neverReturns(const Program& program, FunctionID id) const { return of(program, id).neverReturns; }
    bool onlyThrows(const Program& program, FunctionID id) const { return of(program, id).onlyThrows; }
    // What returnsFreshAggregate() says of a function that can be inlined.
    bool returnsFreshAggregate(const Program&, FunctionID) const;

    void dump(PrintStream&, const Program&) const;

private:
    friend InliningSummaries summarizeForInlining(const Program&, const CallGraph&, const Heat&, const FlowResults&, BuildContext&);

    static constexpr unsigned numberOfLocks = 256;
    static constexpr uint32_t positionWithoutBody = 0;
    static constexpr uint32_t positionOfUnknown = 1;
    static constexpr uint32_t firstOwnPosition = 2;

    Vector<uint32_t> m_positions;
    ParallelArray<InliningSummary> m_summaries;
    InliningSummary m_unknown;
    BuildContext* m_build { nullptr };
    const FlowResults* m_flow { nullptr };
    // Whoever makes the answer for a function holds the lock of its index modulo their number.
    std::unique_ptr<std::array<Lock, numberOfLocks>> m_locksOfAnswers;
};

InliningSummaries summarizeForInlining(const Program&, const CallGraph&, const Heat&, const FlowResults&, BuildContext&);

InliningSummary summarizeFunctionForInlining(const AnalysisContext&, const Function&, Expected<HeatClass>);

bool isBoundToFrame(const Function&, const Block&, uint32_t indexInBlock);
bool isWrapperCreatedByEngine(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
