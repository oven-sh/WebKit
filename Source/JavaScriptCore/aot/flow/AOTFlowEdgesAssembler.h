/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSummary.h"
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowSummaryShape {
    FunctionID function;
    uint32_t numberOfEntryParameters { 0 };
    uint32_t numberOfResults { 0 };
    uint32_t numberOfValues { 0 };
    // An instance per call site keeps apart the objects that different callers pass. A function is not given one for an entry parameter that holds no object.
    BitVector entryParametersWithoutObjects;
};

class FlowSummaryAssembler {
    WTF_MAKE_NONCOPYABLE(FlowSummaryAssembler);
public:
    explicit FlowSummaryAssembler(const FlowSummaryShape&);

    FlowLocalID entryParameter(uint32_t index) const { return m_summary.entryParameter(index); }
    FlowLocalID result(uint32_t index) const { return m_summary.result(index); }
    FlowLocalID thrown() const { return m_summary.thrown(); }

    FlowLocalID addLocalFor(ValueID);
    FlowLocalID addTemporary();
    void bind(ValueID, FlowLocalID);
    FlowLocalID localOf(ValueID value) const { return m_summary.localOfValue[value.index()]; }

    FlowLocalID external(const FlowExternalNode&);
    static bool isProvisionalExternal(FlowLocalID local) { return local && local.index() >= firstProvisionalExternal; }
    std::span<const FlowExternalNode> externals() const LIFETIME_BOUND { return m_summary.externals.span(); }

    uint32_t addSite(const FlowSiteDescription&);
    const FlowSiteDescription& siteAt(uint32_t site) const LIFETIME_BOUND { return m_summary.sites[site]; }
    uint32_t addRange(Range);

    void seed(const FlowSeed&);
    bool hasSeed(FlowLocalID local) const { return m_seededLocals.get(local.index()); }
    void copy(FlowLocalID from, FlowLocalID to);
    FlowLocalList addList(std::span<const FlowLocalID> locals) { return m_summary.statements.addList(locals); }

#define DECLARE_FLOW_STATEMENT_ADDER(kind, type, member, cursors) \
    void add(const type& statement) { m_summary.statements.member.append(statement); }
    FOR_EACH_FLOW_STATEMENT_KIND(DECLARE_FLOW_STATEMENT_ADDER)
#undef DECLARE_FLOW_STATEMENT_ADDER

    void addClosureCreation(const FlowClosureCreation& creation) { m_summary.closureCreations.append(creation); }
    void setCoroutine(const FlowCoroutineSummary& coroutine) { m_summary.coroutine = coroutine; }
    const FlowCoroutineSummary& coroutine() const LIFETIME_BOUND { return m_summary.coroutine; }
    void setRestSite(uint32_t site) { m_summary.restSite = site; }

    FlowSummary finish();

private:
    static constexpr uint32_t firstProvisionalExternal = 1u << 30;

    BitVector localsKnownOutside() const;
    BitVector localsFilledByMoreThanCopies() const;
    Vector<FlowLocalID> findEqualLocals() const;
    void renumberLocals(const Vector<FlowLocalID>& representativeOf);
    void buildCopyEdges();
    void buildWaiters();
    void sortClosureCreations();
    void computeFlags();
    BitVector localsReachedFromEntryParameters() const;
    BitVector localsReachedFrom(Vector<FlowLocalID, 16>&& sources) const;
    bool handsUpNewObject() const;
    void flagCallsByUseOfResult();

    FlowSummary m_summary;
    HashMap<FlowExternalNode, FlowLocalID> m_externals;
    Vector<std::pair<FlowLocalID, FlowLocalID>> m_copies;
    BitVector m_seededLocals;
    BitVector m_entryParametersWithoutObjects;
};

void forEachLocalReadBy(const FlowStatements&, FlowStatementReference, NOESCAPE const ScopedLambda<void(FlowLocalID)>&);
void forEachLocalThatWakes(const FlowStatements&, FlowStatementReference, NOESCAPE const ScopedLambda<void(FlowLocalID)>&);
void forEachLocalPassedOnBy(const FlowStatements&, FlowStatementReference, NOESCAPE const ScopedLambda<void(FlowLocalID)>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
