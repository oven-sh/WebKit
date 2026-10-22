/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFreshResults.h"
#include "AOTFunctionEditing.h"
#include "AOTInlineCopy.h"
#include "AOTPassRunner.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct SplitCandidate {
    FunctionID id;
    std::optional<Function> expandedBody;
};

struct WrapperCallSite {
    ValueID call;
    FunctionID wrapper;
};

class FreshResultSplitter {
    WTF_MAKE_NONCOPYABLE(FreshResultSplitter);
public:
    FreshResultSplitter(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

    void run();

private:
    const Program& program() const { return m_editor.program(); }
    BuildContext& build() const { return m_runner.build(); }
    AnalysisContext contextFor(FunctionID) const;
    bool shouldConsider(FunctionID) const;
    bool isWrapper(FunctionID id) const { return m_wrappers.get(id.index()); }
    bool isWrapperOfLastRound(FunctionID id) const { return m_wrappersOfLastRound.get(id.index()); }
    bool isWrapperThatNoCodeNames(FunctionID) const;

    void collectExistingWrappers();
    Vector<SplitCandidate> findCandidates();
    Vector<SplitCandidate> findForwarders();
    Vector<WrapperCallSite, 2> findForwardedCalls(FunctionID) const;
    unsigned split(Vector<SplitCandidate>&&);
    void moveInlineFramesToWorkers(std::span<const SplitCandidate>, std::span<const FunctionID> workers);

    Vector<WrapperCallSite, 4> findWrapperCalls(FunctionID, const Function& caller) const;
    bool expandCalls(FunctionPassContext&, std::span<const WrapperCallSite>) const;
    void expandCallSites();
    unsigned expandProfitableCalls(FunctionID caller, Function&, std::span<const WrapperCallSite>, TaskArenas&) const;
    Vector<WrapperCallSite, 4> chooseProfitableCalls(FunctionID caller, const Function&, std::span<const WrapperCallSite>, TaskArenas&) const;

    void cloneWorkersByReadResults();

    ProgramEditor& m_editor;
    const OptimizationInputs& m_inputs;
    const PassRunner& m_runner;
    BitVector m_wrappers;
    BitVector m_wrappersOfLastRound;
    unsigned m_numberOfSplits { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
