/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTFunctionTable.h"
#include "AOTPhaseTimes.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct SideTableScratch {
    Vector<uint32_t, 32> offsetsAskedForFrame;
    Vector<CodeRange, 8> rangesWithFrame;
    Vector<uint8_t> frameRanges;
    Vector<uint8_t> handlers;
    Vector<std::pair<uint32_t, uint32_t>, 8> constructedCallees;
    Vector<uint8_t> stream;
    Vector<uint8_t> roomForPositions;
};

struct HashedFrameMaps {
    uint64_t hash;
    uint32_t position;
};

struct FrameMapPlaces {
    Vector<uint32_t> firstWordOf;
    BitVector storesOwnWords;
    uint64_t numberOfWords { 0 };
};

} // anonymous namespace

static void collectOffsetsAskedForFrame(Vector<uint32_t, 32>& offsets, const CompiledCode& code)
{
    offsets.shrink(0);
    auto addReturnOffset = [&](uint32_t returnOffset) {
        RELEASE_ASSERT(returnOffset);
        offsets.append(returnOffset - 1);
    };
    for (const ReturnSite& site : code.body.returnSites)
        addReturnOffset(site.offset);
    for (const FaultingAccess& access : code.sites.faultingAccesses)
        offsets.append(access.codeOffset);
    for (const PollSite& poll : code.sites.pollSites)
        addReturnOffset(poll.returnOffset());
    if (!std::ranges::is_sorted(offsets))
        std::ranges::sort(offsets);
}

static bool eachListOfOffsetsAskedForFrameIsInOrder(const CompiledCode& code)
{
    return std::ranges::is_sorted(code.body.returnSites, { }, &ReturnSite::offset)
        && std::ranges::is_sorted(code.sites.faultingAccesses, { }, &FaultingAccess::codeOffset)
        && std::ranges::is_sorted(code.sites.pollSites, { }, &PollSite::returnOffset);
}

static void forEachOffsetAskedForFrameInOrder(Vector<uint32_t, 32>& offsets, const CompiledCode& code, const Invocable<void(uint32_t)> auto& functor)
{
    if (!eachListOfOffsetsAskedForFrameIsInOrder(code)) {
        collectOffsetsAskedForFrame(offsets, code);
        for (uint32_t offset : offsets)
            functor(offset);
        return;
    }
    constexpr uint64_t none = std::numeric_limits<uint64_t>::max();
    auto offsetInFrontOf = [](uint32_t returnOffset) -> uint64_t {
        RELEASE_ASSERT(returnOffset);
        return returnOffset - 1;
    };
    const ReturnSite* site = code.body.returnSites.begin();
    const ReturnSite* endOfSites = code.body.returnSites.end();
    const FaultingAccess* access = code.sites.faultingAccesses.begin();
    const FaultingAccess* endOfAccesses = code.sites.faultingAccesses.end();
    const PollSite* poll = code.sites.pollSites.begin();
    const PollSite* endOfPolls = code.sites.pollSites.end();
    uint64_t offsetOfSite = site != endOfSites ? offsetInFrontOf(site->offset) : none;
    uint64_t offsetOfAccess = access != endOfAccesses ? access->codeOffset : none;
    uint64_t offsetOfPoll = poll != endOfPolls ? offsetInFrontOf(poll->returnOffset()) : none;
    for (;;) {
        uint64_t offset = std::min(std::min(offsetOfSite, offsetOfAccess), offsetOfPoll);
        if (offset == none)
            return;
        functor(static_cast<uint32_t>(offset));
        if (offset == offsetOfSite)
            offsetOfSite = ++site != endOfSites ? offsetInFrontOf(site->offset) : none;
        else if (offset == offsetOfAccess)
            offsetOfAccess = ++access != endOfAccesses ? access->codeOffset : none;
        else
            offsetOfPoll = ++poll != endOfPolls ? offsetInFrontOf(poll->returnOffset()) : none;
    }
}

static void encodeFrameRanges(SideTableScratch& scratch, const CompiledCode& code)
{
    Vector<uint8_t>& bytes = scratch.frameRanges;
    bytes.shrink(0);
    if (!code.frame.hasFrame())
        return;
    Vector<CodeRange, 8>& ranges = scratch.rangesWithFrame;
    ranges.shrink(0);
    ranges.append(code.frame.frameRanges.span());
    AOT::sortInLine(ranges, { }, &CodeRange::begin);
    const CodeRange* nextRange = ranges.span().data();
    const CodeRange* endOfRanges = nextRange + ranges.size();
    uint32_t endOfRangesPassed = 0;
    auto hasFrameAt = [&](uint32_t offset) {
        for (; nextRange != endOfRanges && nextRange->begin <= offset; ++nextRange)
            endOfRangesPassed = std::max(endOfRangesPassed, nextRange->end);
        return offset < endOfRangesPassed;
    };
    bool hasFrame = false;
    uint32_t previousChange = 0;
    uint32_t endOfLastUnitAsked = 0;
    forEachOffsetAskedForFrameInOrder(scratch.offsetsAskedForFrame, code, [&](uint32_t offset) ALWAYS_INLINE_LAMBDA {
        if (hasFrameAt(offset) != hasFrame) {
            SideTableParts::appendFrameChange(bytes, previousChange, endOfLastUnitAsked);
            previousChange = endOfLastUnitAsked;
            hasFrame = !hasFrame;
        }
        endOfLastUnitAsked = (offset / codeOffsetUnit + 1) * codeOffsetUnit;
    });
}

static uint64_t hashFrameMapWords(std::span<const uint32_t> words)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t word : words)
        hash = (hash ^ word) * 1099511628211ull;
    return hash >> 1;
}

static FrameMapPlaces findPlacesOfFrameMaps(const LinkedFunctions& functions, const ParallelLoops& loops)
{
    Vector<HashedFrameMaps> hashed = collectInParallel<HashedFrameMaps>(loops, static_cast<unsigned>(functions.size()), [&](unsigned position, Vector<HashedFrameMaps>& found) {
        std::span<const uint32_t> words = functions[position].sites.frameMapWords.span();
        if (!words.empty())
            found.append({ hashFrameMapWords(words), position });
    });

    FrameMapPlaces places;
    places.firstWordOf.fill(0, functions.size());
    places.storesOwnWords.ensureSize(functions.size());
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> firstWithHash;
    for (auto [hash, position] : hashed) {
        std::span<const uint32_t> words = functions[position].sites.frameMapWords.span();
        uint32_t first = firstWithHash.add(hash, position).iterator->value;
        if (first != position && equalSpans(functions[first].sites.frameMapWords.span(), words)) {
            places.firstWordOf[position] = places.firstWordOf[first];
            continue;
        }
        places.firstWordOf[position] = static_cast<uint32_t>(places.numberOfWords);
        places.storesOwnWords.quickSet(position);
        places.numberOfWords += words.size();
    }
    return places;
}

static Vector<uint8_t> joinPositionsOfChunks(const ChunksOfIndices& chunks, std::span<const Vector<uint8_t>> positionsOfChunks, std::span<const uint32_t> placesInChunks, BuildContext& context)
{
    RELEASE_ASSERT(positionsOfChunks.size() == chunks.size());
    size_t sizeOfIndex = placesInChunks.size() * sizeof(uint32_t);
    uint64_t sizeOfEntries = 0;
    Vector<uint32_t> placeOfChunk(positionsOfChunks.size());
    for (size_t chunk = 0; chunk < positionsOfChunks.size(); ++chunk) {
        placeOfChunk[chunk] = static_cast<uint32_t>(sizeOfEntries);
        sizeOfEntries += positionsOfChunks[chunk].size();
    }
    if (!checkSectionSize(sizeOfIndex + sizeOfEntries, context))
        return { };

    Vector<uint8_t> result(sizeOfIndex + sizeOfEntries);
    std::span<uint8_t> index = result.mutableSpan().first(sizeOfIndex);
    std::span<uint8_t> entries = result.mutableSpan().subspan(sizeOfIndex);
    forEachIndexInParallel(context.parallelLoops(), chunks.size(), [&](unsigned chunk) {
        memcpySpan(entries.subspan(placeOfChunk[chunk]), positionsOfChunks[chunk].span());
        size_t endOfGroups = PositionIndex::numberOfEntriesFor(chunks.firstIndexOf[chunk + 1]);
        for (size_t group = PositionIndex::numberOfEntriesFor(chunks.firstIndexOf[chunk]); group < endOfGroups; ++group) {
            uint32_t place = placeOfChunk[chunk] + placesInChunks[group];
            memcpySpan(index.subspan(group * sizeof(uint32_t)), asByteSpan(place));
        }
    });
    return result;
}

SideTables encodeSideTables(const LinkedFunctions& functions, std::span<const Vector<uint8_t>> callSites, BuildContext& context)
{
    RELEASE_ASSERT(callSites.size() == functions.size());
    const ParallelLoops& loops = context.parallelLoops();
    unsigned numberOfFunctions = static_cast<unsigned>(functions.size());
    StepTimes times(context, "image: side tables"_s);

    FrameMapPlaces frameMaps = findPlacesOfFrameMaps(functions, loops);
    static_assert(WTF::isValidCapacityForVector<uint32_t>(defaultLimitValue(LimitID::VectorBytes, hostArchitecture()) / sizeof(uint32_t)));
    if (!checkSectionSize(frameMaps.numberOfWords * sizeof(uint32_t), context))
        return { };

    std::optional<FrameTable> frameTable = collectFrames(functions, context);
    if (!frameTable)
        return { };
    const Vector<uint32_t>& frameOf = frameTable->frameOf;
    times.didFinish("place frame maps, collect frames"_s);

    bool hasPositions = !context.options().obfuscates();
    SideTables result;
    result.frames = WTF::move(frameTable->frames);
    result.frameMapWords.grow(frameMaps.numberOfWords);
    result.flags.grow(numberOfFunctions);
    std::span<uint32_t> frameMapWords = result.frameMapWords.mutableSpan();
    Vector<Vector<uint8_t>> streams(numberOfFunctions);

    ChunksOfIndices chunks = divideIntoChunks(numberOfFunctions);
    Vector<Vector<uint8_t>> positionsOfChunks(chunks.size());
    Vector<uint32_t> placesInChunks;
    placesInChunks.fill(0, PositionIndex::numberOfEntriesFor(numberOfFunctions));
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        SideTableScratch scratch;
        Vector<uint8_t>& positions = positionsOfChunks[chunk];
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position) {
            const CompiledCode& code = functions[position];
            const SiteDescription& sites = code.sites;
            encodeFrameRanges(scratch, code);
            std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
            if (!sites.frameMapWords.isEmpty()) {
                frameMapRange.emplace(frameMaps.firstWordOf[position], static_cast<uint32_t>(sites.frameMapWords.size()));
                if (frameMaps.storesOwnWords.quickGet(position))
                    memcpySpan(frameMapWords.subspan(frameMapRange->first), sites.frameMapWords.span());
            }
            scratch.handlers.shrink(0);
            encodeHandlers(scratch.handlers, sites.handlers.span());
            scratch.constructedCallees.shrink(0);
            for (const ConstructedCallee& row : sites.constructedCallees) {
                CodeUnit callee = code.body.callees[row.callee];
                RELEASE_ASSERT(callee);
                scratch.constructedCallees.append({ row.site, callee.function.index() });
            }
            SideTableInputs inputs {
                .frameRanges = scratch.frameRanges.span(),
                .frame = frameOf[position],
                .frameMapRange = frameMapRange,
                .handlers = scratch.handlers.span(),
                .calleesOfSites = sites.calleesOfSites.span(),
                .constructedCallees = scratch.constructedCallees.span(),
                .callSites = callSites[position].span(),
            };
            scratch.stream.shrink(0);
            SideTableParts::append(scratch.stream, inputs);
            streams[position].reserveInitialCapacity(scratch.stream.size());
            streams[position].append(scratch.stream.span());
            result.flags[position] = inputs.flags();
            if (!hasPositions)
                continue;
            bool beginsGroup = !(position % PositionIndex::functionsBetween);
            if (beginsGroup)
                placesInChunks[position / PositionIndex::functionsBetween] = static_cast<uint32_t>(positions.size());
            encodePositions(positions, scratch.roomForPositions, sites, beginsGroup ? OwnPosition { } : OwnPosition::of(functions[position - 1].sites));
        }
    });

    times.didFinish("encode"_s);

    SectionWriter writer(context);
    result.offsets = writer.appendAll(streams.span());
    result.bytes = writer.takeBytes();
    times.didFinish("hold equal streams once"_s);
    result.positions = joinPositionsOfChunks(chunks, positionsOfChunks.span(), placesInChunks.span(), context);
    times.didFinish("join positions"_s);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position)
            streams[position].clear();
        positionsOfChunks[chunk].clear();
    });
    times.didFinish("free the streams"_s);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
