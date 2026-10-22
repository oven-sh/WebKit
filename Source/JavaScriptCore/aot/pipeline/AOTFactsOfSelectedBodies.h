/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTPipelineInternal.h"
#include "AOTPlacement.h"
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// What the steps between selection and the choice of representations take from one body by itself: what it names, for the call graph, and what has to be numbered.
class FactsOfSelectedBodies {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FactsOfSelectedBodies);
    WTF_MAKE_NONCOPYABLE(FactsOfSelectedBodies);
public:
    explicit FactsOfSelectedBodies(Pipeline& pipeline)
        : m_program(pipeline.program())
        , m_flow(pipeline.flow())
        , m_build(pipeline.runner().build())
        , m_inputsOfScans(pipeline.createPlacementInputs(m_noCallGraph))
        , m_scansForPlacement(m_program, m_inputsOfScans, m_build)
        , m_entriesThatRunNoCode(findEntriesThatRunNoCode())
        , m_references(m_program.functions.size())
    {
    }

    // From any thread, no two for one function at a time, while the caches of the program and an interning phase of its types are open. The body holds no provisional id.
    void read(FunctionID id)
    {
        readReferences(id);
        m_scansForPlacement.scan(id);
    }

    // The two halves of read(), for a caller that takes many functions through one and then through the other.
    void readReferences(FunctionID id)
    {
        m_references[id.index()] = makeUniqueWithoutFastMallocCheck<ReferencesOfBody>(collectReferencesOf(m_program, m_flow, id, m_entriesThatRunNoCode));
    }

    // Null for a function that was not read.
    const ReferencesOfBody* referencesOf(FunctionID id) const { return id.index() < m_references.size() ? m_references[id.index()].get() : nullptr; }

    // By one thread, while nobody reads.
    void didAddFunctions()
    {
        m_references.grow(m_program.functions.size());
        m_scansForPlacement.didAddFunctions();
    }

    void giveReferencesTo(ProgramAnalyses& analyses)
    {
        analyses.takeReferencesOfBodies(m_references.span());
        m_build.destroyLater(WTF::move(m_references));
    }

    BodyScansForPlacement& scansForPlacement() { return m_scansForPlacement; }

private:
    const Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    // The scans ask nothing of it.
    CallGraph m_noCallGraph;
    PlacementInputs m_inputsOfScans;
    BodyScansForPlacement m_scansForPlacement;
    EntriesThatRunNoCode m_entriesThatRunNoCode;
    Vector<std::unique_ptr<ReferencesOfBody>> m_references;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
