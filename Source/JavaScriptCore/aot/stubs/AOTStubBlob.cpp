/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubBlob.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include "AOTStubAssembler.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRecording.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT {

using EntrySet = WTF::BitSet<numberOfEntries>;

static_assert(std::ranges::all_of(operationFrontEnds(), [](const OperationFrontEnd& frontEnd) {
    return exists(frontEnd.stub) && descriptorOf(frontEnd.stub.family()).linkage == StubLinkage::FrontEnd;
}));

struct ReachedBody {
    StubKey key;
    const StubRecording* dryRun;
};

struct ReachedStubs {
    Vector<ReachedBody> bodies;
    Vector<std::unique_ptr<StubRecording>> ownDryRuns;
    EntrySet entriesUsed;
};

template<typename Functor>
static void forEachStubEnteredByRuntime(const Functor& functor)
{
    forEachKeyWithoutImmediate([&](StubKey key) {
        switch (descriptorOf(key.family()).linkage) {
        case StubLinkage::EnteredByRuntime:
        case StubLinkage::Handler:
            functor(key);
            return;
        case StubLinkage::CalledByCode:
        case StubLinkage::FrontEnd:
        case StubLinkage::InternalEntry:
            return;
        }
    });
}

static ReachedStubs findReachedStubs(const StubCatalog& catalog, std::span<const StubKey> calledKeys, const EntrySet& entriesUsedByFunctions)
{
    ReachedStubs reached;
    EntrySet used = entriesUsedByFunctions;
    HashSet<uint64_t> seenBodies;
    Vector<StubKey> worklist;
    auto reach = [&](StubKey key) {
        StubKey body = bodyOf(key);
        if (seenBodies.add(body.bits()).isNewEntry)
            worklist.append(body);
    };
    auto dryRunOf = [&](StubKey body) -> const StubRecording& {
        if (catalog.hasRecordingOf(body))
            return catalog.recordingOf(body);
        reached.ownDryRuns.append(makeUniqueWithoutFastMallocCheck<StubRecording>(catalog.dryRun(body)));
        return *reached.ownDryRuns.last();
    };
    for (StubKey key : calledKeys) {
        RELEASE_ASSERT(exists(key));
        reach(key);
    }
    forEachStubEnteredByRuntime(reach);
    do {
        while (!worklist.isEmpty()) {
            StubKey body = worklist.takeLast();
            const StubRecording& dryRun = dryRunOf(body);
            reached.bodies.append(ReachedBody { body, &dryRun });
            used.merge(dryRun.entriesUsed);
            for (auto& reference : dryRun.references)
                reach(reference.target);
        }
        for (auto& frontEnd : operationFrontEnds()) {
            if (!used.get(static_cast<size_t>(frontEnd.operation)))
                continue;
            used.set(static_cast<size_t>(frontEnd.raw));
            reach(frontEnd.stub);
        }
    } while (!worklist.isEmpty());
    reached.entriesUsed = used;
    return reached;
}

static void orderBodies(Vector<ReachedBody>& bodies, uint32_t layoutSeed)
{
    AOT::sort(bodies, { }, &ReachedBody::key);
    if (!layoutSeed)
        return;
    WeakRandom random(layoutSeed);
    for (size_t remaining = bodies.size(); remaining > 1; --remaining)
        std::swap(bodies[remaining - 1], bodies[random.getUint32(static_cast<unsigned>(remaining))]);
}

static void checkRecording(const StubCatalog& catalog, const StubRecording& recording, const StubRecording& dryRun)
{
    RELEASE_ASSERT(recording.references == dryRun.references);
    RELEASE_ASSERT(recording.entries == dryRun.entries);
    RELEASE_ASSERT(recording.entriesUsed == dryRun.entriesUsed);
    RELEASE_ASSERT(recording.operationsCalled == dryRun.operationsCalled);
    RELEASE_ASSERT(recording.returnLabels == dryRun.returnLabels);
    RELEASE_ASSERT(recording.exits == dryRun.exits);
    RELEASE_ASSERT(recording.symbols == dryRun.symbols);
    RELEASE_ASSERT(recording.hasUnknownEffects == dryRun.hasUnknownEffects);
    RELEASE_ASSERT(recording.leavesStubs == dryRun.leavesStubs);
    RELEASE_ASSERT(recording.clobberedInline == dryRun.clobberedInline);
    RELEASE_ASSERT(recording.clobberedByCallees == dryRun.clobberedByCallees);

    RegisterSet closedClobbers = catalog.closedClobbersOf(recording);
    for (StubKey entry : recording.entries) {
        RELEASE_ASSERT(catalog.summaryOf(entry).clobbered.subsumes(closedClobbers));
        // Callers reach a front end through the run-time table, so they recorded only the clobbers of an operation call.
        if (descriptorOf(entry.family()).linkage == StubLinkage::FrontEnd)
            RELEASE_ASSERT(registersClobberedByOperations().subsumes(closedClobbers));
    }
}

static void checkReturnLabels(const Vector<StubAssembler::BoundReturnLabel>& returnLabels)
{
    WTF::BitSet<numberOfStubFrameKinds> boundKinds;
    for (auto& returnLabel : returnLabels) {
        bool wasBound = boundKinds.testAndSet(static_cast<size_t>(returnLabel.kind));
        RELEASE_ASSERT(!wasBound || !hasSingleReturnSite(returnLabel.kind));
    }
}

static Vector<StubEntryRange> findEntriesInFrontOfSingleReturnSites(const StubBlob& blob)
{
    Vector<StubPlacement> byOffset = blob.placements;
    AOT::sort(byOffset, { }, &StubPlacement::offset);
    auto isSingleReturnSite = [&](uint32_t offset) {
        auto site = std::ranges::lower_bound(blob.returnSites, offset, { }, &StubReturnSite::offset);
        return site != blob.returnSites.end() && site->offset == offset && hasSingleReturnSite(site->kind);
    };

    Vector<StubEntryRange> result;
    for (const StubReturnSite& site : blob.returnSites) {
        if (!hasSingleReturnSite(site.kind))
            continue;
        auto behindCall = std::ranges::lower_bound(byOffset, site.offset, { }, &StubPlacement::offset);
        RELEASE_ASSERT(behindCall != byOffset.begin());
        StubKey body = bodyOf(std::prev(behindCall)->key);
        for (auto entry = byOffset.begin(); entry != behindCall; ++entry) {
            if (bodyOf(entry->key) != body)
                continue;
            // Only a return comes to the site itself, and it comes to the copy that made the call.
            auto next = std::find_if(std::next(entry), byOffset.end(), [&](const StubPlacement& placement) {
                return placement.offset != entry->offset && !isSingleReturnSite(placement.offset);
            });
            result.append({ entry->offset, next == byOffset.end() ? static_cast<uint32_t>(blob.code.bytes.size()) : next->offset });
        }
    }
    AOT::sort(result, { }, &StubEntryRange::offset);
    removeRepeatedElements(result, [](const StubEntryRange& a, const StubEntryRange& b) {
        return a.offset == b.offset;
    });
    return result;
}

std::optional<uint32_t> StubBlob::offsetOf(StubKey key) const
{
    auto placement = std::ranges::lower_bound(placements, key, { }, &StubPlacement::key);
    if (placement == placements.end() || placement->key != key)
        return std::nullopt;
    return placement->offset;
}

static std::expected<StubBlob, LinkFailure> buildStubBlob(BuildContext& context, const StubCatalog& catalog, const ReachedStubs& reached, PCRelativeReach addressReach)
{
    StubAssembler jit(catalog.options(), catalog, addressReach);
    RELEASE_ASSERT(!jit.m_assembler.codeSize());
    for (auto& body : reached.bodies)
        checkRecording(catalog, jit.generate(body.key), *body.dryRun);
    checkReturnLabels(jit.boundReturnLabels());
    jit.linkJumpsBetweenStubs();

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    if (buffer.didFailToAllocate())
        return std::unexpected(LinkFailure::OutOfMemory);
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    if (!jit.linkCallsAndAddresses(linkBuffer))
        return std::unexpected(LinkFailure::OutOfPCRelativeReach);
    buffer.finalize(context.options().logging.dumpsDisassembly, "AOT stubs"_s);
    auto relocations = jit.relocations().link(buffer, &context);
    if (!relocations)
        return std::unexpected(relocations.error());

    StubBlob blob;
    blob.code.bytes.append(buffer.bytes());
    blob.code.relocations = WTF::move(*relocations);
    RELEASE_ASSERT(std::ranges::all_of(blob.code.relocations, [](const Relocation& relocation) {
        return relocation.space == SymbolSpace::RegExp;
    }));
    for (auto& entry : jit.boundEntries())
        blob.placements.append(StubPlacement { entry.key, linkBuffer.offsetOf(entry.label) });
    AOT::sort(blob.placements, { }, &StubPlacement::key);
    for (auto& returnLabel : jit.boundReturnLabels())
        blob.returnSites.append(StubReturnSite { linkBuffer.offsetOf(returnLabel.label), returnLabel.kind });
    AOT::sort(blob.returnSites, { }, &StubReturnSite::offset);
    RELEASE_ASSERT(std::ranges::adjacent_find(blob.returnSites, { }, &StubReturnSite::offset) == blob.returnSites.end());
    blob.entriesInFrontOfSingleReturnSites = findEntriesInFrontOfSingleReturnSites(blob);
    blob.entriesUsed = reached.entriesUsed;

    Statistics& statistics = context.statistics();
    statistics.count(StatisticID::StubsGenerated, reached.bodies.size());
    statistics.count(StatisticID::StubBytes, blob.code.bytes.size());
    return blob;
}

std::expected<StubBlob, LinkFailure> buildStubBlob(BuildContext& context, const StubCatalog& catalog, std::span<const StubKey> calledKeys, const EntrySet& entriesUsedByFunctions)
{
    ReachedStubs reached = findReachedStubs(catalog, calledKeys, entriesUsedByFunctions);
    orderBodies(reached.bodies, catalog.options().layoutSeed);

    // A near address reaches 1 MB on ARM64. A large program has more stubs than that, because each name it uses often gets stubs of its own.
    auto blob = buildStubBlob(context, catalog, reached, PCRelativeReach::Near);
    if (!blob && blob.error() == LinkFailure::OutOfPCRelativeReach)
        blob = buildStubBlob(context, catalog, reached, PCRelativeReach::Far);
    return blob;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
