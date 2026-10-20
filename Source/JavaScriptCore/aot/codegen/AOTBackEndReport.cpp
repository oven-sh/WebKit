/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBackEndReport.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeGenerator.h"
#include "AOTFrameAddressValidation.h"
#include "AOTNarrowReferenceValidation.h"

namespace JSC { namespace AOT {

#define AOT_B3_LIMIT_ASSERTION(name, defaultValue, purpose, degradesTo) \
    static_assert(defaultLimitValue(LimitID::name, Architecture::ARM64) == B3::defaultValueOf(B3::Limit::name)); \
    static_assert(defaultLimitValue(LimitID::name, Architecture::X86_64) == B3::defaultValueOf(B3::Limit::name));
FOR_EACH_B3_LIMIT(AOT_B3_LIMIT_ASSERTION)
#undef AOT_B3_LIMIT_ASSERTION

LimitID limitIDOf(B3::Limit limit)
{
    switch (limit) {
#define AOT_B3_LIMIT_CASE(name, defaultValue, purpose, degradesTo) \
    case B3::Limit::name: \
        return LimitID::name;
    FOR_EACH_B3_LIMIT(AOT_B3_LIMIT_CASE)
#undef AOT_B3_LIMIT_CASE
    }
    RELEASE_ASSERT_NOT_REACHED();
}

B3::LimitValues b3LimitValuesFrom(const BuildContext& build)
{
    B3::LimitValues values;
#define AOT_SET_B3_LIMIT(name, defaultValue, purpose, degradesTo) values.set(B3::Limit::name, build.limit(LimitID::name));
    FOR_EACH_B3_LIMIT(AOT_SET_B3_LIMIT)
#undef AOT_SET_B3_LIMIT
    return values;
}

static InternalErrorKind internalErrorKindOf(B3::FailureKind kind)
{
    switch (kind) {
    case B3::FailureKind::AbsoluteAddress:
        return InternalErrorKind::AbsoluteAddressInProcedure;
    case B3::FailureKind::RelocationNotLowered:
        return InternalErrorKind::RelocationNotLowered;
    case B3::FailureKind::VariableReadBeforeSet:
        return InternalErrorKind::VariableReadBeforeSet;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Wire::DeclineKind> declineKindOf(B3::RemarkKind kind)
{
    switch (kind) {
    case B3::RemarkKind::LoopNotVersioned:
        return Wire::DeclineKind::LoopNotVersioned;
    case B3::RemarkKind::PollNotStripMined:
        return Wire::DeclineKind::LoopPollNotStripMined;
    case B3::RemarkKind::LoopBoundNotClamped:
        return Wire::DeclineKind::LoopBoundNotClamped;
    case B3::RemarkKind::LoopVersioned:
    case B3::RemarkKind::LoopRotated:
    case B3::RemarkKind::PollStripMined:
    case B3::RemarkKind::RangeChecksRemoved:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wire::LoopDeclineReason declineReasonOf(B3::GaveUpReason reason)
{
    switch (reason) {
    case B3::GaveUpReason::None:
        return Wire::LoopDeclineReason::None;
    case B3::GaveUpReason::NotInnermost:
        return Wire::LoopDeclineReason::NotInnermost;
    case B3::GaveUpReason::SeveralEntries:
        return Wire::LoopDeclineReason::SeveralEntries;
    case B3::GaveUpReason::NoSingleLikelyPath:
        return Wire::LoopDeclineReason::NoSingleLikelyPath;
    case B3::GaveUpReason::UncloneableValue:
        return Wire::LoopDeclineReason::UncloneableValue;
    case B3::GaveUpReason::NothingToHoist:
        return Wire::LoopDeclineReason::NothingToHoist;
    case B3::GaveUpReason::NotCounted:
        return Wire::LoopDeclineReason::NotCounted;
    case B3::GaveUpReason::CounterMayWrap:
        return Wire::LoopDeclineReason::CounterMayWrap;
    case B3::GaveUpReason::BoundNotInvariant:
        return Wire::LoopDeclineReason::BoundNotInvariant;
    case B3::GaveUpReason::TestNotOnLikelyPath:
        return Wire::LoopDeclineReason::TestNotOnLikelyPath;
    case B3::GaveUpReason::EffectsBeforeTopTest:
        return Wire::LoopDeclineReason::EffectsBeforeTopTest;
    case B3::GaveUpReason::PollNotOnEveryIteration:
        return Wire::LoopDeclineReason::PollNotOnEveryIteration;
    case B3::GaveUpReason::SeveralPolls:
        return Wire::LoopDeclineReason::SeveralPolls;
    case B3::GaveUpReason::LimitReached:
        return Wire::LoopDeclineReason::LimitReached;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StatisticID statisticOf(B3::Counter counter)
{
    switch (counter) {
    case B3::Counter::LoopsVersioned:
        return StatisticID::LoopsVersioned;
    case B3::Counter::LoopsRotated:
        return StatisticID::LoopsRotated;
    case B3::Counter::PollsStripMined:
        return StatisticID::LoopPollsStripMined;
    case B3::Counter::RangeChecksRemoved:
        return StatisticID::RangeChecksRemoved;
    case B3::Counter::LoopBoundsClamped:
        return StatisticID::LoopBoundsClamped;
    case B3::Counter::KnownInductionsUsed:
        return StatisticID::KnownInductionsUsed;
    case B3::Counter::TrapsFoldedIntoAccesses:
        return StatisticID::NullChecksFoldedIntoAccesses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void collectFindings(const B3::CompilationReport& report, std::span<const FrameAddressViolation> violations, Vector<BackEndFinding, 1>& findings)
{
    if (report.failure)
        findings.append(BackEndFinding { internalErrorKindOf(report.failure->kind), report.failure->origin });
    for (const FrameAddressViolation& violation : violations)
        findings.append(BackEndFinding { InternalErrorKind::FrameAddressAcrossSuspension, violation.address });
}

static void collectDeclines(const B3::CompilationReport& report, Vector<BackEndDecline>& declines)
{
    for (const B3::Remark& remark : report.remarks) {
        std::optional<Wire::DeclineKind> kind = declineKindOf(remark.kind);
        if (!kind || remark.reason == B3::GaveUpReason::NotInnermost)
            continue;
        declines.append(BackEndDecline { *kind, remark.limit ? limitIDOf(*remark.limit) : LimitID::None, declineReasonOf(remark.reason), remark.origin });
    }
}

static void collectLimitHits(const B3::CompilationReport& report, Vector<BackEndLimitHit, 2>& limitHits)
{
    for (unsigned i = 0; i < B3::numberOfLimits; ++i) {
        auto limit = static_cast<B3::Limit>(i);
        if (uint32_t count = report.hitsOf(limit))
            limitHits.append(BackEndLimitHit { limitIDOf(limit), count, report.firstOriginOf(limit) });
    }
}

BackEndReport collectBackEndReport(const B3::CompilationReport& report, std::span<const FrameAddressViolation> violations, bool collectsDeclines)
{
    BackEndReport result;
    collectFindings(report, violations, result.findings);
    if (collectsDeclines)
        collectDeclines(report, result.declines);
    collectLimitHits(report, result.limitHits);
    return result;
}

static InternalErrorKind internalErrorKindOf(NarrowReferenceViolationKind kind)
{
    switch (kind) {
    case NarrowReferenceViolationKind::HasNoPointer:
        return InternalErrorKind::NarrowReferenceWithoutPointer;
    case NarrowReferenceViolationKind::IsDisplaced:
        return InternalErrorKind::DisplacedNarrowReference;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void appendFindings(BackEndReport& report, std::span<const NarrowReferenceViolation> violations)
{
    for (const NarrowReferenceViolation& violation : violations)
        report.findings.append(BackEndFinding { internalErrorKindOf(violation.kind), violation.reference, violation.safepoint, violation.detail });
}

static Subject subjectOf(Typed::FunctionID id, const Typed::Function& function, B3::Origin origin)
{
    Typed::ValueID value = Typed::valueOfOrigin(origin);
    if (value.index() >= function.values.size())
        return { .function = id };
    return { .function = id, .value = value, .location = function.locationOf(value) };
}

static void countStatistics(Statistics& statistics, const B3::CompilationReport& report)
{
    for (unsigned i = 0; i < B3::numberOfCounters; ++i) {
        auto counter = static_cast<B3::Counter>(i);
        if (uint32_t amount = report.countOf(counter))
            statistics.count(statisticOf(counter), amount);
    }
}

bool reportBackEnd(BuildContext& build, Typed::FunctionID id, const Typed::Function& function, const BackEndReport& report, const B3::CompilationReport& raw)
{
    for (const BackEndFinding& finding : report.findings) {
        Subject subject = subjectOf(id, function, finding.origin);
        Typed::ValueID safepoint = Typed::valueOfOrigin(finding.safepoint);
        build.diagnostics().add(InternalError { .kind = finding.kind, .function = subject.function, .value = subject.value, .count = safepoint ? safepoint.index() : 0, .location = subject.location, .detail = finding.detail });
    }
    for (const BackEndDecline& decline : report.declines) {
        build.addDecline({
            .kind = decline.kind,
            .limit = decline.limit,
            .reason = encodeDeclineReason(decline.kind, decline.reason),
            .witness = witnessOf(subjectOf(id, function, decline.origin)),
        });
    }
    for (const BackEndLimitHit& hit : report.limitHits) {
        Subject subject = subjectOf(id, function, hit.firstOrigin);
        for (uint32_t i = 0; i < hit.count; ++i)
            build.gaveUp(hit.limit, subject);
    }
    countStatistics(build.statistics(), raw);
    return !report.findings.isEmpty();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
