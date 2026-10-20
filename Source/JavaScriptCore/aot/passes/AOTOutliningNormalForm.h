/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOutliningCandidates.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class OutliningNormalForm {
    WTF_MAKE_NONCOPYABLE(OutliningNormalForm);
public:
    OutliningNormalForm(const Function&, const OutliningSite&, uint64_t maxLiveIns);

    const Vector<ValueID, 6>& liveIns() const LIFETIME_BOUND { return m_liveIns; }
    bool hasRequiredDefinitionOutside() const { return m_hasRequiredDefinitionOutside; }
    unsigned hash() const;
    bool isEqualTo(const OutliningNormalForm&) const;

private:
    enum class Mark : uint32_t { Value, Parameter, Terminator, InteriorTarget, ExitTarget, Proof, Result, Absent, InSite, LiveIn, Constant };
    enum class Reference : uint8_t { Absent, InSite, LiveIn, Constant };

    void appendMark(Mark mark) { m_words.append(static_cast<uint32_t>(mark)); }
    void appendWord(uint32_t word) { m_words.append(word); }
    void appendCount(size_t count) { m_words.append(static_cast<uint32_t>(count)); }
    void appendValue(ValueID);
    void appendParameter(ValueID);
    void appendTerminator(const Terminator&);
    void appendTarget(const Target&);
    Reference appendReference(ValueID);
    size_t liveInIndex(ValueID);

    const Function& m_function;
    const OutliningSite& m_site;
    uint64_t m_maxLiveIns;
    bool m_hasRequiredDefinitionOutside { false };
    Vector<ValueID, 24> m_definitions;
    Vector<ValueID, 6> m_liveIns;
    Vector<uint32_t, 128> m_words;
    Vector<const Value*, 24> m_payloads;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
