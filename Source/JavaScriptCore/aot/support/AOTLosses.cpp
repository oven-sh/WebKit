/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLosses.h"

#if ENABLE(AOT)

#include "AOTProvisionalFunctions.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/SaturatingArithmetic.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

template<typename Reason>
static Reason memberAt(uint32_t word)
{
    RELEASE_ASSERT(word < numberOfEnumerators<Reason>);
    return static_cast<Reason>(word);
}

DeclineReason decodeDeclineReason(DeclineKind kind, uint32_t word)
{
    switch (kind) {
    case DeclineKind::HotCallNotResolved:
        return memberAt<Wire::CallDeclineReason>(word);
    case DeclineKind::GeneratorLoopNotFused:
        return memberAt<Wire::GeneratorFusionDeclineReason>(word);
    case DeclineKind::ArrayChainNotFused:
        return memberAt<Wire::ArrayChainDeclineReason>(word);
    case DeclineKind::StringBuilderNotCreated:
        return memberAt<Wire::StringBuilderDeclineReason>(word);
    case DeclineKind::AllocationNotSunk:
        RELEASE_ASSERT(!(word >> numberOfEnumerators<AllocationEscapeReason>));
        return OptionSet<AllocationEscapeReason>::fromRaw(word);
    case DeclineKind::TypedResultNotSelected:
        return memberAt<Wire::TypedResultDeclineReason>(word);
    case DeclineKind::LoopNotVersioned:
    case DeclineKind::LoopBoundNotClamped:
        return memberAt<Wire::LoopDeclineReason>(word);
    case DeclineKind::ResultNodeInterpreted:
        return memberAt<Wire::InterpretedCause>(word);
    case DeclineKind::Unknown:
    case DeclineKind::HotCallNotInlined:
    case DeclineKind::StringBuildersNotJoined:
    case DeclineKind::IterationNotExpandedByClass:
    case DeclineKind::StretchNotGuarded:
    case DeclineKind::GenericVersionNotCreated:
        RELEASE_ASSERT(!word);
        return std::monostate { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t encodeDeclineReason(DeclineKind kind, const DeclineReason& reason)
{
    RELEASE_ASSERT(reason.index() == decodeDeclineReason(kind, 0).index());
    return WTF::switchOn(reason,
        [](std::monostate) -> uint32_t {
            return 0;
        },
        [](OptionSet<AllocationEscapeReason> flags) -> uint32_t {
            return flags.toRaw();
        },
        [](auto member) -> uint32_t {
            return static_cast<uint32_t>(member);
        });
}

void DeclineReasonField::dump(PrintStream& out) const
{
    WTF::switchOn(decodeDeclineReason(decline.kind, decline.reason),
        [](std::monostate) {
        },
        [&](OptionSet<AllocationEscapeReason> flags) {
            CommaPrinter separator("+"_s, " reason="_s);
            for (AllocationEscapeReason flag : flags)
                out.print(separator, nameOf(flag));
        },
        [&]<typename Reason>(Reason member) {
            if constexpr (requires { Reason::None; }) {
                if (member == Reason::None)
                    return;
            }
            out.print(" reason="_s, nameOf(member));
        });
}

static auto subjectKey(const Wire::LossRecord& loss)
{
    const Wire::Witness& witness = loss.witness;
    return std::tuple { witness.location.module, witness.location.position, witness.function, witness.value, loss.layout };
}

static void mergeLoss(Wire::LossRecord& merged, const Wire::LossRecord& loss)
{
    ASSERT(merged.limit == loss.limit);
    merged.count = saturatingSum<uint32_t>(merged.count, loss.count);
    if (subjectKey(loss) < subjectKey(merged)) {
        merged.layout = loss.layout;
        merged.witness = loss.witness;
    }
}

static void convertToIDsAsRead(Wire::Witness& witness, const Typed::Program* program)
{
    if (!program)
        return;
    Typed::FunctionID function(witness.function);
    Typed::FunctionID functionAsRead = findFunctionAsRead(program, function);
    if (functionAsRead == function)
        return;
    witness.function = functionAsRead.index();
    witness.value = Wire::none;
}

static void resolveSubject(Wire::LossRecord& loss, const Typed::Program* program)
{
    if (!program)
        return;
    Wire::Witness& witness = loss.witness;
    Subject subject {
        .function = Typed::FunctionID(witness.function),
        .value = Typed::ValueID(witness.value),
        .layout = Typed::LayoutID(loss.layout),
        .location = witness.location,
    };
    if (auto location = findLocation(program, subject))
        witness.location = *location;
    convertToIDsAsRead(witness, program);
    loss.layout = findLayoutAsRead(program, subject.layout).index();
}

static auto declineOrderKey(const Wire::DeclineRecord& decline)
{
    const Wire::Witness& witness = decline.witness;
    return std::tuple { witness.location.module, witness.location.position, decline.kind, witness.function, witness.value, decline.reason, decline.limit, decline.count };
}

static_assert(sizeof(Wire::DeclineRecord) == 8 * sizeof(uint32_t), "declineOrderKey has to compare every member");

static unsigned rankOf(AllocationOutcome outcome)
{
    switch (outcome) {
    case AllocationOutcome::Unknown:
        return 0;
    case AllocationOutcome::Removed:
        return 1;
    case AllocationOutcome::Frame:
        return 2;
    case AllocationOutcome::FrameUntilMaterialized:
        return 3;
    case AllocationOutcome::Deferred:
        return 4;
    case AllocationOutcome::Slim:
        return 5;
    case AllocationOutcome::Heap:
        return 6;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static auto remarkOrderKey(const AllocationRemark& remark)
{
    const auto& [site, location, outcome, cause, forcedBy, forcingOpcode, reasons, limit, copies] = remark;
    return std::tuple {
        site, -static_cast<int>(rankOf(outcome)), forcedBy.module, forcedBy.position, forcingOpcode, reasons, limit, cause,
        location.module, location.position, copies,
    };
}

static bool refinesHeapOutcome(AllocationOutcome outcome)
{
    switch (outcome) {
    case AllocationOutcome::Deferred:
    case AllocationOutcome::Slim:
        return true;
    case AllocationOutcome::Unknown:
    case AllocationOutcome::Removed:
    case AllocationOutcome::Frame:
    case AllocationOutcome::FrameUntilMaterialized:
    case AllocationOutcome::Heap:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isOnHeap(AllocationOutcome outcome)
{
    return outcome == AllocationOutcome::Heap || refinesHeapOutcome(outcome);
}

static bool haveSameStorage(AllocationOutcome a, AllocationOutcome b)
{
    return a == b || (isOnHeap(a) && isOnHeap(b));
}

// The count is of the copies that share the outcome reported: a site that is removed from two inlined copies and built in the third is allocated once.
static AllocationRemark mergeCopiesOfSite(std::span<const AllocationRemark> worstFirst)
{
    AllocationRemark merged = worstFirst.front();
    merged.copies = 0;
    size_t numberOfHeapCopies = 0;
    size_t numberOfRefinedCopies = 0;
    std::optional<AllocationOutcome> worstRefinement;
    for (const AllocationRemark& copy : worstFirst) {
        if (haveSameStorage(copy.outcome, worstFirst.front().outcome))
            merged.copies = saturatingSum<uint32_t>(merged.copies, copy.copies);
        if (copy.outcome == AllocationOutcome::Heap)
            ++numberOfHeapCopies;
        if (!refinesHeapOutcome(copy.outcome))
            continue;
        ++numberOfRefinedCopies;
        if (!worstRefinement)
            worstRefinement = copy.outcome;
    }

    // Selection says later which form a closure on the heap has. Why the closure is on the heap stays what the first remark said.
    if (numberOfHeapCopies && numberOfRefinedCopies >= numberOfHeapCopies)
        merged.outcome = *worstRefinement;
    return merged;
}

static void mergeCopies(Vector<AllocationRemark>& remarks)
{
    std::ranges::sort(remarks, { }, remarkOrderKey);

    size_t numberOfSites = 0;
    for (size_t begin = 0; begin < remarks.size();) {
        size_t end = begin + 1;
        while (end < remarks.size() && remarks[end].site == remarks[begin].site)
            ++end;
        remarks[numberOfSites++] = mergeCopiesOfSite(remarks.span().subspan(begin, end - begin));
        begin = end;
    }
    remarks.shrink(numberOfSites);
}

Losses::Losses(Collected collected)
    : m_collected(collected)
{
}

Losses::Stripe& Losses::stripeOfCurrentThread()
{
    return m_stripes[Thread::currentSingleton().uid() % numberOfStripes];
}

template<typename Record>
Vector<Record> Losses::takeRecords(Vector<Record> Stripe::* list)
{
    Vector<Record> records;
    for (Stripe& stripe : m_stripes) {
        Locker locker { stripe.lock };
        records.appendVector(std::exchange(stripe.*list, { }));
    }
    return records;
}

void Losses::addLimitHit(LimitID limit, const Subject& subject)
{
    RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(subject.function));
    Wire::LossRecord loss { .limit = limit, .stage = stageOf(limit), .layout = subject.layout.index(), .count = 1, .witness = witnessOf(subject) };

    Stripe& stripe = stripeOfCurrentThread();
    Locker locker { stripe.lock };
    size_t index = stripe.losses.findIf([&](const Wire::LossRecord& other) {
        return other.limit == limit;
    });
    if (index == notFound) {
        stripe.losses.append(loss);
        return;
    }
    mergeLoss(stripe.losses[index], loss);
}

Vector<Typed::FunctionID> Losses::functionsWithUnresolvedWitnesses()
{
    Vector<Typed::FunctionID> functions;
    for (Stripe& stripe : m_stripes) {
        Locker locker { stripe.lock };
        for (const Wire::LossRecord& loss : stripe.losses) {
            if (Typed::ValueID(loss.witness.value) && !hasModule(loss.witness.location))
                functions.append(Typed::FunctionID(loss.witness.function));
        }
    }
    return functions;
}

Vector<Wire::LossRecord> Losses::takeLosses(const Typed::Program* program)
{
    std::array<std::optional<Wire::LossRecord>, numberOfLimits> lossByLimit;
    for (const Wire::LossRecord& loss : takeRecords(&Stripe::losses)) {
        std::optional<Wire::LossRecord>& merged = lossByLimit[static_cast<size_t>(loss.limit)];
        if (merged)
            mergeLoss(*merged, loss);
        else
            merged = loss;
    }

    Vector<Wire::LossRecord> losses;
    for (std::optional<Wire::LossRecord>& loss : lossByLimit) {
        if (!loss)
            continue;
        resolveSubject(*loss, program);
        losses.append(*loss);
    }
    return losses;
}

void Losses::addDecline(const Wire::DeclineRecord& decline)
{
    if (!m_collected.declines)
        return;
    RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(Typed::FunctionID(decline.witness.function)));

    Stripe& stripe = stripeOfCurrentThread();
    Locker locker { stripe.lock };
    stripe.declines.append(decline);
}

Vector<Wire::DeclineRecord> Losses::takeDeclines(const Typed::Program* program)
{
    Vector<Wire::DeclineRecord> declines = takeRecords(&Stripe::declines);
    for (Wire::DeclineRecord& decline : declines)
        convertToIDsAsRead(decline.witness, program);
    std::ranges::sort(declines, { }, declineOrderKey);
    removeRepeatedElements(declines, [](const Wire::DeclineRecord& a, const Wire::DeclineRecord& b) {
        return a.kind == b.kind && a.reason == b.reason && a.witness == b.witness;
    });
    return declines;
}

void Losses::addAllocationRemark(const AllocationRemark& remark)
{
    if (!m_collected.allocationRemarks)
        return;
    RELEASE_ASSERT(remark.site);

    Stripe& stripe = stripeOfCurrentThread();
    Locker locker { stripe.lock };
    stripe.allocationRemarks.append(remark);
}

Vector<AllocationRemark> Losses::takeAllocationRemarks()
{
    Vector<AllocationRemark> remarks = takeRecords(&Stripe::allocationRemarks);
    mergeCopies(remarks);
    return remarks;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
