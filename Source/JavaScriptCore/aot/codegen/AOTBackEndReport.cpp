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
#include "AOTInnerPointerValidation.h"
#include "AOTNarrowReferenceValidation.h"
#include "AOTVector.h"

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
    case B3::FailureKind::SpillSlotLacksItsValue:
        return InternalErrorKind::SpillSlotLacksItsValue;
    case B3::FailureKind::RegisterLacksItsValue:
        return InternalErrorKind::RegisterLacksItsValue;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Wire::DeclineKind> declineKindOf(B3::RemarkKind kind)
{
    switch (kind) {
    case B3::RemarkKind::LoopNotVersioned:
        return Wire::DeclineKind::LoopNotVersioned;
    case B3::RemarkKind::LoopBoundNotClamped:
        return Wire::DeclineKind::LoopBoundNotClamped;
    case B3::RemarkKind::LoopVersioned:
    case B3::RemarkKind::LoopRotated:
    case B3::RemarkKind::LoopVectorized:
    case B3::RemarkKind::LoopUnrolled:
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
    case B3::Counter::LoopsVectorized:
        return StatisticID::LoopsVectorized;
    case B3::Counter::LoopsUnrolled:
        return StatisticID::LoopsUnrolled;
    case B3::Counter::RangeChecksRemoved:
        return StatisticID::RangeChecksRemoved;
    case B3::Counter::LoopBoundsClamped:
        return StatisticID::LoopBoundsClamped;
    case B3::Counter::LoopPollsRemoved:
        return StatisticID::LoopPollsRemoved;
    case B3::Counter::SumsKeptAsIntegers:
        return StatisticID::SumsKeptAsIntegers;
    case B3::Counter::KnownInductionsUsed:
        return StatisticID::KnownInductionsUsed;
    case B3::Counter::TrapsFoldedIntoAccesses:
        return StatisticID::NullChecksFoldedIntoAccesses;
    case B3::Counter::ColdUsesKeptThroughSecondSplit:
        return StatisticID::ColdUsesKeptThroughSecondSplit;
    case B3::Counter::ColdUsesKeptThroughSecondSplitBetweenDefs:
        return StatisticID::ColdUsesKeptThroughSecondSplitBetweenDefs;
    case B3::Counter::MovesReadForWidths:
        return StatisticID::MovesReadForWidths;
    case B3::Counter::PredecessorsReadForRareBlocks:
        return StatisticID::PredecessorsReadForRareBlocks;
    case B3::Counter::StepsToReleaseDeadValues:
        return StatisticID::StepsToReleaseDeadValues;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BackEndReport collectBackEndReport(const B3::CompilationReport& report, std::span<const FrameAddressViolation> violations, bool collectsDeclines)
{
    BackEndReport result;
    if (report.failure)
        result.findings.append(BackEndFinding { internalErrorKindOf(report.failure->kind), report.failure->origin, B3::Origin(), report.failure->detail });
    for (const FrameAddressViolation& violation : violations)
        result.findings.append(BackEndFinding { InternalErrorKind::FrameAddressAcrossSuspension, violation.address });

    if (collectsDeclines) {
        for (const B3::Remark& remark : report.remarks) {
            std::optional<Wire::DeclineKind> kind = declineKindOf(remark.kind);
            if (!kind || remark.reason == B3::GaveUpReason::NotInnermost)
                continue;
            result.declines.append(BackEndDecline { *kind, remark.limit ? limitIDOf(*remark.limit) : LimitID::None, declineReasonOf(remark.reason), remark.origin });
        }
    }

    for (unsigned i = 0; i < B3::numberOfLimits; ++i) {
        auto limit = static_cast<B3::Limit>(i);
        if (uint32_t count = report.hitsOf(limit))
            result.limitHits.append(BackEndLimitHit { limitIDOf(limit), count, report.firstOriginOf(limit) });
    }
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

void appendFindings(BackEndReport& report, std::span<const InnerPointerViolation> violations)
{
    for (const InnerPointerViolation& violation : violations)
        report.findings.append(BackEndFinding { InternalErrorKind::InnerPointerWithoutOwner, violation.pointer, violation.safepoint, violation.detail });
}

static Subject subjectOf(Typed::FunctionID id, const Typed::Function& function, B3::Origin origin)
{
    Typed::ValueID value = Typed::valueOfOrigin(origin);
    if (value.index() >= function.values.size())
        return { .function = id };
    return { .function = id, .value = value, .location = function.locationOf(value) };
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
    for (unsigned i = 0; i < B3::numberOfCounters; ++i) {
        auto counter = static_cast<B3::Counter>(i);
        if (uint32_t amount = raw.countOf(counter))
            build.statistics().count(statisticOf(counter), amount);
    }
    if (raw.countOf(B3::Counter::ColdUsesKeptThroughSecondSplit))
        build.statistics().count(StatisticID::FunctionsWithColdUsesKeptThroughSecondSplit);
    if (raw.countOf(B3::Counter::ColdUsesKeptThroughSecondSplitBetweenDefs))
        build.statistics().count(StatisticID::FunctionsWithColdUsesKeptThroughSecondSplitBetweenDefs);
    return !report.findings.isEmpty();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
