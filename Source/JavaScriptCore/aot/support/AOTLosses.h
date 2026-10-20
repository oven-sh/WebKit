/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTLimits.h"
#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <optional>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

using Wire::AllocationEscapeReason;
using Wire::AllocationHeapCause;
using Wire::AllocationOutcome;
using Wire::DeclineKind;

constexpr Wire::Witness witnessOf(const Subject& subject)
{
    return { .function = subject.function.index(), .value = subject.value.index(), .location = subject.location };
}

// The kind of a decline says which alternative its reason word holds.
using DeclineReason = Variant<std::monostate, Wire::CallDeclineReason, Wire::GeneratorFusionDeclineReason, Wire::ArrayChainDeclineReason,
    Wire::StringBuilderDeclineReason, OptionSet<AllocationEscapeReason>, Wire::TypedResultDeclineReason, Wire::LoopDeclineReason, Wire::InterpretedCause>;

JS_EXPORT_PRIVATE DeclineReason decodeDeclineReason(DeclineKind, uint32_t word);
JS_EXPORT_PRIVATE uint32_t encodeDeclineReason(DeclineKind, const DeclineReason&);

// " reason=<member>", flags joined by '+', and nothing for None, for no flag and for a kind without reasons.
struct DeclineReasonField {
    const Wire::DeclineRecord& decline;

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;
};

struct AllocationRemark {
    Typed::AllocationSiteID site { };
    SourceLocation location { noSourceLocation };
    AllocationOutcome outcome { AllocationOutcome::Unknown };
    AllocationHeapCause cause { AllocationHeapCause::None };
    SourceLocation forcedBy { noSourceLocation };
    std::optional<Typed::Opcode> forcingOpcode { };
    AllocationEscapeReason reasons { };
    LimitID limit { LimitID::None };
    uint32_t copies { 1 };
};

class Losses {
    WTF_MAKE_NONCOPYABLE(Losses);
public:
    struct Collected {
        bool declines { false };
        bool allocationRemarks { false };
    };

    explicit Losses(Collected);

    void addLimitHit(LimitID, const Subject&);
    void addDecline(const Wire::DeclineRecord&);
    void addAllocationRemark(const AllocationRemark&);

    JS_EXPORT_PRIVATE Vector<Wire::LossRecord> takeLosses(const Typed::Program*);
    JS_EXPORT_PRIVATE Vector<Wire::DeclineRecord> takeDeclines(const Typed::Program*);
    JS_EXPORT_PRIVATE Vector<AllocationRemark> takeAllocationRemarks();

private:
    static constexpr unsigned numberOfStripes = 16;

    struct Stripe {
        Lock lock;
        Vector<Wire::LossRecord> losses WTF_GUARDED_BY_LOCK(lock);
        Vector<Wire::DeclineRecord> declines WTF_GUARDED_BY_LOCK(lock);
        Vector<AllocationRemark> allocationRemarks WTF_GUARDED_BY_LOCK(lock);
    };

    Stripe& stripeOfCurrentThread();
    template<typename Record> Vector<Record> takeRecords(Vector<Record> Stripe::*);

    const Collected m_collected;
    std::array<Stripe, numberOfStripes> m_stripes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
