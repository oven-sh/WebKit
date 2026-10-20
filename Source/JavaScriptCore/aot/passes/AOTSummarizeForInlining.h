/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include "AOTPerFunction.h"
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>
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

struct ParameterCondition {
    ValueID condition;
    uint32_t entryParameter { 0 };
    uint32_t bytesOnlyIfTrue { 0 };
    uint32_t bytesOnlyIfFalse { 0 };
};

struct ReceiverParameter {
    uint32_t entryParameter { 0 };
    uint32_t accessesByName { 0 };
};

struct InliningSummary {
    bool canInlineWholeBody() const { return barrier == InlineBarrier::None && blocksBoundToFrame.isEmpty(); }
    bool canInlineHead() const { return barrier == InlineBarrier::None && !headBlocks.isEmpty(); }

    InlineBarrier barrier { InlineBarrier::Unknown };
    uint32_t codeBytes { 0 };
    uint32_t headBytes { 0 };
    BitVector headBlocks;
    BitVector noReturnBlocks;
    BitVector blocksBoundToFrame;
    BitVector blocksWithHandlers;
    BitVector calledParameters;
    BitVector parametersCalledInCycles;
    Vector<ParameterCondition, 2> conditions;
    Vector<ReceiverParameter, 1> receiverParameters;
    bool neverReturns { false };
    bool hasBareReturn { false };
    bool isWrapper { false };
    bool returnsFreshAggregate { false };
};

class InliningSummaries {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(InliningSummaries);
public:
    InliningSummaries() = default;

    bool contains(FunctionID id) const { return id.index() < m_summaries.size(); }
    const InliningSummary& of(const Program& program, FunctionID id) const LIFETIME_BOUND { return contains(id) ? m_summaries.of(program, id) : m_unknown; }
    bool neverReturns(const Program& program, FunctionID id) const { return of(program, id).neverReturns; }

    void dump(PrintStream&, const Program&) const;

private:
    friend InliningSummaries summarizeForInlining(const Program&, const CallGraph&, const Heat&, const FlowResults&, BuildContext&);

    PerFunction<InliningSummary> m_summaries;
    InliningSummary m_unknown;
};

InliningSummaries summarizeForInlining(const Program&, const CallGraph&, const Heat&, const FlowResults&, BuildContext&);

InliningSummary summarizeFunctionForInlining(const AnalysisContext&, const Function&, HeatClass);

bool isBoundToFrame(const Function&, const Block&, uint32_t indexInBlock);
bool isWrapperCreatedByEngine(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
