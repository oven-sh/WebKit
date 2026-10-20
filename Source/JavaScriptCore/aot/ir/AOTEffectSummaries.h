/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTEffectsOfOperands.h"
#include "AOTThreads.h"
#include <memory>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BodySummaries;
class FlowResults;
class FunctionAnalyses;

struct ParameterSummary {
    OperandSummary operand;
    OperandMemory memory;

    friend bool operator==(const ParameterSummary&, const ParameterSummary&) = default;
};

struct UnknownCodeWitness {
    FunctionID function;
    ValueID value;

    explicit operator bool() const { return !!function; }
    friend bool operator==(const UnknownCodeWitness&, const UnknownCodeWitness&) = default;
};

struct FunctionSummary {
    EffectsOnEdges elsewhere { EffectsOnEdges::unknownCode() };
    OperandMemory throughParameters;
    Vector<ParameterSummary> parameters;
    UnknownCodeWitness unknownCodeWitness;

    const ParameterSummary& parameter(uint32_t entryParameter) const LIFETIME_BOUND;
    EffectsOnEdges onCall() const;

    friend bool operator==(const FunctionSummary&, const FunctionSummary&) = default;
};

class EffectSummaries {
    WTF_MAKE_TZONE_ALLOCATED(EffectSummaries);
    WTF_MAKE_NONCOPYABLE(EffectSummaries);
public:
    EffectSummaries() = default;
    EffectSummaries(EffectSummaries&&) = default;
    EffectSummaries& operator=(EffectSummaries&&) = default;

    EffectSummaries(ParallelArray<FunctionSummary>&&, const Effects& unknownCode);

    const FunctionSummary& of(FunctionID) const LIFETIME_BOUND;
    const Effects& unknownCode() const LIFETIME_BOUND { return m_unknownCode; }
    unsigned size() const { return m_summaries.size(); }
    bool contains(FunctionID id) const { return id && id.index() < size(); }

    void dump(PrintStream&, const Program&) const;

private:
    ParallelArray<FunctionSummary> m_summaries;
    FunctionSummary m_summaryOfUnknownFunction;
    Effects m_unknownCode { Effects::unknownCode() };
};

// The summary of a function that is called. Of a function that was made after the summaries nothing is known, unless its role says what it does:
// a serializer does no more than JSON.stringify does with an object of its layout, a parser no more than JSON.parse does with a string,
// and the step of an array chain calls the traversal it was made from.
class CalleeSummary {
public:
    CalleeSummary(const EffectContext&, FunctionID);

    const FunctionSummary& operator*() const LIFETIME_BOUND { return m_summaryByRole ? *m_summaryByRole : *m_computed; }
    const FunctionSummary* operator->() const LIFETIME_BOUND { return &**this; }

private:
    const FunctionSummary* m_computed;
    std::optional<FunctionSummary> m_summaryByRole;
};

EffectSummaries createSummariesFromFlow(const Program&, const FlowResults&, BuildContext&);
EffectSummaries computeEffectSummaries(const Program&, const FlowResults&, BuildContext&, BodySummaries* kept = nullptr);
FunctionSummary summarizeBody(const EffectContext&, FunctionID, FunctionAnalyses&);

struct BaseObject {
    enum class Kind : uint8_t { Untracked, EntryParameter, OwnAllocation };

    Kind kind { Kind::Untracked };
    uint32_t entryParameter { 0 };
};

std::unique_ptr<FunctionAnalyses> createAnalysesForSummary(BuildContext&, const EffectContext&, const FlowResults&, FunctionID, const Function&);
void forEachBaseObject(FunctionAnalyses&, ValueID, NOESCAPE const ScopedLambda<void(const BaseObject&)>&);
OperandSummary summarizeEntryParameter(FunctionAnalyses&, uint32_t entryParameter);
OperandSummary publishedParameterSummary(const FlowResults&, FunctionID, uint32_t entryParameter);
CallerState callerStateOf(const FlowResults&, FunctionID);
// The callbacks, handlers, accessors and protocol methods that flow linked. Their callers may all be known to flow, and the operation that enters one may
// still read as unknown code here: the type of its operand does not name the function, or a built-in that runs code of no operand calls a handler that
// native code holds (one drains the jobs, another closes a socket).
BitVector functionsCalledIndirectly(const FlowResults&, size_t numberOfFunctions);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
