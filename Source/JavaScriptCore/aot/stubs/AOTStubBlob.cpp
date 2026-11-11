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
#include <utility>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

using EntrySet = WTF::BitSet<numberOfEntries>;

static_assert(std::ranges::all_of(operationFrontEnds(), [](const OperationFrontEnd& frontEnd) {
    return exists(frontEnd.stub) && bodyOf(frontEnd.stub) == frontEnd.stub && descriptorOf(frontEnd.stub.family()).linkage == StubLinkage::FrontEnd;
}));

struct ReachedStubs {
    Vector<StubKey> bodies;
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

static ReachedStubs findReachedStubs(const StubCatalog& catalog, std::span<const StubKey> calledKeys, const EntrySet& entriesUsedByFunctions, bool checksAnswers)
{
    ReachedStubs reached;
    EntrySet used = entriesUsedByFunctions;
    HashSet<uint64_t> seenBodies;
    Vector<StubKey> worklist;
    Vector<std::unique_ptr<StubRecording>> ownDryRuns;
    auto reach = [&](StubKey key) {
        StubKey body = bodyOf(key);
        if (seenBodies.add(body.bits()).isNewEntry)
            worklist.append(body);
    };
    auto dryRunOf = [&](StubKey body) -> const StubRecording& {
        if (catalog.hasRecordingOf(body))
            return catalog.recordingOf(body);
        ownDryRuns.append(makeUniqueWithoutFastMallocCheck<StubRecording>(catalog.dryRun(body)));
        return *ownDryRuns.last();
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
            if (descriptorOf(body.family()).linkage == StubLinkage::FrontEnd)
                RELEASE_ASSERT(registersClobberedByOperations().subsumes(catalog.summaryOf(body).clobbered));
            reached.bodies.append(body);
            used.merge(dryRun.entriesUsed);
            for (auto& reference : dryRun.references)
                reach(reference.target);
        }
        for (const OperationFrontEnd& frontEnd : operationFrontEnds()) {
            if (!used.get(static_cast<size_t>(frontEnd.operation)))
                continue;
            used.set(static_cast<size_t>(frontEnd.raw));
            reach(frontEnd.stub);
        }
    } while (!worklist.isEmpty());
    reached.entriesUsed = used;
    if (checksAnswers) {
        for (const std::unique_ptr<StubRecording>& dryRun : ownDryRuns)
            RELEASE_ASSERT(catalog.answersCover(*dryRun));
    }
    AOT::sortInLine(reached.bodies, { }, [](StubKey body) {
        return std::pair { body.immediate() && descriptorOf(body.family()).binding == ImmediateBinding::Every, body };
    });
    return reached;
}

static Vector<StubEntryRange> findEntriesInFrontOfSingleReturnSites(const StubBlob& blob)
{
    Vector<StubPlacement> byOffset = blob.placements;
    AOT::sortInLine(byOffset, { }, &StubPlacement::offset);
    auto isSingleReturnSite = [&](uint32_t offset) {
        auto site = std::ranges::lower_bound(blob.returnSites, offset, { }, &StubReturnSite::offset);
        return site != blob.returnSites.end() && site->offset == offset && hasSingleReturnSite(site->kind);
    };

    Vector<StubEntryRange> result;
    for (const StubReturnSite& site : blob.returnSites) {
        if (!hasSingleReturnSite(site.kind))
            continue;
        auto nextStub = std::ranges::lower_bound(byOffset, site.offset, { }, &StubPlacement::offset);
        RELEASE_ASSERT(nextStub != byOffset.begin());
        StubKey body = bodyOf(std::prev(nextStub)->key);
        for (auto entry = byOffset.begin(); entry != nextStub; ++entry) {
            if (bodyOf(entry->key) != body)
                continue;
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
    for (StubKey body : reached.bodies)
        jit.generate(body);
    jit.linkJumpsBetweenStubs();

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    if (buffer.didFailToAllocate())
        return std::unexpected(LinkFailure::OutOfMemory);
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    if (!jit.linkCallsAndAddresses(linkBuffer))
        return std::unexpected(LinkFailure::OutOfPCRelativeReach);
    buffer.finalize(false, "AOT stubs"_s);
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
    AOT::sortInLine(blob.placements, { }, &StubPlacement::key);
    for (auto& returnLabel : jit.boundReturnLabels())
        blob.returnSites.append(StubReturnSite { linkBuffer.offsetOf(returnLabel.label), returnLabel.kind });
    AOT::sortInLine(blob.returnSites, { }, &StubReturnSite::offset);
    RELEASE_ASSERT(std::ranges::adjacent_find(blob.returnSites, { }, &StubReturnSite::offset) == blob.returnSites.end());
    blob.entriesInFrontOfSingleReturnSites = findEntriesInFrontOfSingleReturnSites(blob);
    blob.entriesUsed = reached.entriesUsed;
    return blob;
}

std::expected<StubBlob, LinkFailure> buildStubBlob(BuildContext& context, const StubCatalog& catalog, std::span<const StubKey> calledKeys, const EntrySet& entriesUsedByFunctions)
{
    const bool checksAnswers = ASSERT_ENABLED || ASAN_ENABLED || context.options().verifiesMachineCode;
    ReachedStubs reached = findReachedStubs(catalog, calledKeys, entriesUsedByFunctions, checksAnswers);

    auto blob = buildStubBlob(context, catalog, reached, PCRelativeReach::Near);
    if (!blob && blob.error() == LinkFailure::OutOfPCRelativeReach)
        blob = buildStubBlob(context, catalog, reached, PCRelativeReach::Far);
    return blob;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
