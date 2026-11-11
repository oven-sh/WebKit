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
#include "AOTPositionEncoder.h"
#include "AOTPrefixCode.h"
#include "AOTSectionWriter.h"
#include "AOTSideTableCodes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVarint.h"
#include <algorithm>
#include <array>
#include <cstring>
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
    Vector<PrefixSymbol> symbols;
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
    for (const PollSite& poll : code.sites.rareLists->pollSites)
        addReturnOffset(poll.returnOffset());
    if (!std::ranges::is_sorted(offsets))
        std::ranges::sort(offsets);
}

static bool eachListOfOffsetsAskedForFrameIsInOrder(const CompiledCode& code)
{
    return std::ranges::is_sorted(code.body.returnSites, { }, &ReturnSite::offset)
        && std::ranges::is_sorted(code.sites.faultingAccesses, { }, &FaultingAccess::codeOffset)
        && std::ranges::is_sorted(code.sites.rareLists->pollSites, { }, &PollSite::returnOffset);
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
    const PollSite* poll = code.sites.rareLists->pollSites.begin();
    const PollSite* endOfPolls = code.sites.rareLists->pollSites.end();
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

static void appendFrameChange(Vector<uint8_t>& bytes, uint32_t previousChange, uint32_t change)
{
    RELEASE_ASSERT(previousChange <= change && !(previousChange % codeOffsetUnit) && !(change % codeOffsetUnit));
    appendVarint(bytes, (change - previousChange) / codeOffsetUnit);
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
            appendFrameChange(bytes, previousChange, endOfLastUnitAsked);
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
        std::span<const uint32_t> words = functions[position].sites.rareLists->frameMapWords.span();
        if (!words.empty())
            found.append({ hashFrameMapWords(words), position });
    });

    FrameMapPlaces places;
    places.firstWordOf.fill(0, functions.size());
    places.storesOwnWords.ensureSize(functions.size());
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> firstWithHash;
    for (auto [hash, position] : hashed) {
        std::span<const uint32_t> words = functions[position].sites.rareLists->frameMapWords.span();
        uint32_t first = firstWithHash.add(hash, position).iterator->value;
        if (first != position && equalSpans(functions[first].sites.rareLists->frameMapWords.span(), words)) {
            places.firstWordOf[position] = places.firstWordOf[first];
            continue;
        }
        places.firstWordOf[position] = static_cast<uint32_t>(places.numberOfWords);
        places.storesOwnWords.quickSet(position);
        places.numberOfWords += words.size();
    }
    return places;
}

static Vector<uint8_t> joinPositionsOfChunks(const ChunksOfIndices& chunks, std::span<PositionEncoder> encoders, BuildContext& context)
{
    RELEASE_ASSERT(encoders.size() == chunks.size());
    PositionCodes::Counts counts { };
    for (const PositionEncoder& encoder : encoders)
        PositionCodes::add(counts, encoder.counts());
    PositionCodes codes(counts);
    const Vector<uint8_t>& bytesOfCodes = codes.bytes();

    Vector<Vector<uint8_t>> positionsOfChunks(chunks.size());
    Vector<uint32_t> placesInChunks;
    placesInChunks.fill(0, PositionIndex::numberOfEntriesFor(chunks.firstIndexOf.last()));
    forEachIndexInParallel(context.parallelLoops(), chunks.size(), [&](unsigned chunk) {
        Vector<uint8_t>& positions = positionsOfChunks[chunk];
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position) {
            if (!(position % PositionIndex::functionsBetween))
                placesInChunks[position / PositionIndex::functionsBetween] = static_cast<uint32_t>(positions.size());
            encoders[chunk].appendNextCoded(positions, codes);
        }
    });

    size_t sizeOfIndex = placesInChunks.size() * sizeof(uint32_t);
    uint64_t sizeOfEntries = 0;
    Vector<uint32_t> placeOfChunk(positionsOfChunks.size());
    for (size_t chunk = 0; chunk < positionsOfChunks.size(); ++chunk) {
        placeOfChunk[chunk] = static_cast<uint32_t>(sizeOfEntries);
        sizeOfEntries += positionsOfChunks[chunk].size();
    }
    if (!checkSectionSize(bytesOfCodes.size() + sizeOfIndex + sizeOfEntries, context))
        return { };

    Vector<uint8_t> result(bytesOfCodes.size() + sizeOfIndex + sizeOfEntries);
    memcpySpan(result.mutableSpan(), bytesOfCodes.span());
    std::span<uint8_t> index = result.mutableSpan().subspan(bytesOfCodes.size(), sizeOfIndex);
    std::span<uint8_t> entries = result.mutableSpan().subspan(bytesOfCodes.size() + sizeOfIndex);
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

namespace {

struct SideTableInputs {
    OptionSet<ImageFunctionFlag> flags() const;

    OptionSet<ImageFunctionFlag> flagsOfFunction;
    std::span<const uint8_t> frameRanges;
    uint32_t frame { 0 };
    std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
    std::span<const uint8_t> handlers;
    std::span<const std::pair<uint32_t, uint32_t>> constructedCallees;
    std::span<const PrefixSymbol> callSites;
};

OptionSet<ImageFunctionFlag> SideTableInputs::flags() const
{
    OptionSet<ImageFunctionFlag> flags = flagsOfFunction;
    flags.set(ImageFunctionFlag::HasFrameMaps, frameMapRange && frameMapRange->second);
    flags.set(ImageFunctionFlag::HasHandlers, !handlers.empty());
    flags.set(ImageFunctionFlag::HasConstructedCallees, !constructedCallees.empty());
    flags.set(ImageFunctionFlag::HasCallSites, !callSites.empty());
    return flags;
}

struct SideTableStream {
    static SideTableStream read(std::span<const PrefixSymbol> words)
    {
        SideTableStream stream;
        auto take = [&](size_t size) {
            return std::exchange(words, words.subspan(size)).first(size);
        };
        auto takeBytes = [&](std::span<uint8_t> bytes) {
            memcpySpan(bytes, asBytes(take(bytes.size() / sizeof(PrefixSymbol))));
        };
        std::array<uint32_t, 2> numbersOfSymbols;
        takeBytes(asMutableByteSpan(numbersOfSymbols));
        stream.own = take(numbersOfSymbols[0]);
        stream.handlers = take(numbersOfSymbols[1]);
        if (words.empty())
            return stream;
        takeBytes(asMutableByteSpan(stream.head));
        stream.blocks = take(stream.head.numberOfBlocks * sizeof(CallSiteStreamHead::Block) / sizeof(PrefixSymbol));
        stream.callSites = words;
        return stream;
    }

    CallSiteStreamHead::Block block(size_t index) const
    {
        constexpr size_t size = sizeof(CallSiteStreamHead::Block);
        CallSiteStreamHead::Block result;
        memcpySpan(asMutableByteSpan(result), asBytes(blocks).subspan(index * size, size));
        return result;
    }

    std::span<const PrefixSymbol> own;
    std::span<const PrefixSymbol> handlers;
    CallSiteStreamHead head { };
    std::span<const PrefixSymbol> blocks;
    std::span<const PrefixSymbol> callSites;
};

} // anonymous namespace

static void appendStream(Vector<PrefixSymbol>& stream, Vector<PrefixSymbol>& room, const SideTableInputs& inputs)
{
    constexpr size_t maxSymbolsOfWord = 10;
    OptionSet<ImageFunctionFlag> flags = inputs.flags();
    room.shrink(0);
    room.grow(8 * maxSymbolsOfWord + inputs.frameRanges.size() + inputs.handlers.size() + inputs.constructedCallees.size() * 2 * maxSymbolsOfWord);
    PrefixSymbol* firstSymbol = room.mutableSpan().data();
    SideTableSymbolWriter symbols(firstSymbol);
    symbols.append(SideTableByte::Flags, static_cast<uint8_t>(flags.toRaw()));
    symbols.appendVarint(SideTableByte::FrameChangesSize, inputs.frameRanges.size());
    if (!inputs.frameRanges.empty()) {
        SideTableByte kind = SideTableByte::FirstFrameChange;
        for (uint8_t byte : inputs.frameRanges)
            symbols.append(std::exchange(kind, SideTableByte::LaterFrameChange), byte);
        symbols.appendVarint(SideTableByte::Frame, inputs.frame);
    }
    if (flags.contains(ImageFunctionFlag::HasFrameMaps)) {
        symbols.appendVarint(SideTableByte::FrameMapStart, SideTableByte::FrameMapStartContinuation, inputs.frameMapRange->first);
        symbols.appendVarint(SideTableByte::FrameMapSize, inputs.frameMapRange->second);
    }
    if (flags.contains(ImageFunctionFlag::HasConstructedCallees)) {
        Vector<uint8_t> rows;
        uint32_t previous = 0;
        for (auto [site, function] : inputs.constructedCallees) {
            RELEASE_ASSERT(site > previous);
            appendVarint(rows, site - std::exchange(previous, site));
            appendVarint(rows, function);
        }
        symbols.appendVarint(SideTableByte::Continuation, rows.size());
        for (uint8_t byte : rows)
            symbols.append(SideTableByte::ConstructedCallee, byte);
    }
    PrefixSymbol* endOfOwn = symbols.end();
    if (flags.contains(ImageFunctionFlag::HasHandlers)) {
        Vector<uint64_t, 16> words;
        for (const uint8_t* cursor = inputs.handlers.data(); cursor < inputs.handlers.data() + inputs.handlers.size();)
            words.append(readVarint(cursor));
        constexpr size_t wordsOfRow = 3;
        RELEASE_ASSERT(!(words.size() % wordsOfRow));
        symbols.appendVarint(SideTableByte::HandlerRows, words.size() / wordsOfRow);
        for (size_t word = 0; word < words.size(); word += wordsOfRow) {
            symbols.appendVarint(SideTableByte::HandlerStart, words[word]);
            symbols.appendVarint(SideTableByte::HandlerSites, words[word + 1]);
            symbols.appendVarint(SideTableByte::HandlerTarget, SideTableByte::HandlerTargetContinuation, words[word + 2]);
        }
    }
    std::array<uint32_t, 2> numbersOfSymbols { static_cast<uint32_t>(endOfOwn - firstSymbol), static_cast<uint32_t>(symbols.end() - endOfOwn) };
    stream.reserveInitialCapacity(sizeof(numbersOfSymbols) / sizeof(PrefixSymbol) + (symbols.end() - firstSymbol) + inputs.callSites.size());
    stream.append(spanReinterpretCast<const PrefixSymbol>(asByteSpan(numbersOfSymbols)));
    stream.append(std::span<const PrefixSymbol>(firstSymbol, symbols.end()));
    stream.append(inputs.callSites);
}

static uint8_t* writeStream(uint8_t* to, const SideTableStream& stream, const SideTableCodes& codes)
{
    std::span<const PrefixSymbol> left = stream.callSites;
    auto take = [&](size_t size) {
        return std::exchange(left, left.subspan(size)).first(size);
    };
    auto writePart = [&](std::span<const PrefixSymbol> symbols) {
        constexpr size_t maxSizeOfVarint = 10;
        uint8_t* part = to + maxSizeOfVarint;
        uint8_t* endOfPart = codes.write(part, symbols);
        WriterOfReservedBytes sizeOfPart(to);
        appendVarint(sizeOfPart, endOfPart - part);
        memmove(sizeOfPart.end(), part, endOfPart - part);
        to = sizeOfPart.end() + (endOfPart - part);
    };
    to = codes.write(to, stream.own, take(stream.head.numberOfHeadSymbols));
    if (!stream.handlers.empty())
        writePart(stream.handlers);
    if (stream.callSites.empty())
        return to;
    if (stream.head.hasInlineFrames) {
        writePart(take(stream.head.numberOfParentSymbols));
        writePart(take(stream.head.numberOfFunctionSymbols));
    }
    uint32_t numberOfBlocks = stream.head.numberOfBlocks;
    uint8_t* checkpoints = to;
    uint8_t* firstBlock = to += numberOfBlocks ? (numberOfBlocks - 1) * sizeof(SiteCheckpoint) : 0;
    for (uint32_t number = 0; number < numberOfBlocks; ++number) {
        CallSiteStreamHead::Block block = stream.block(number);
        if (number) {
            SiteCheckpoint checkpoint { block.offset, static_cast<uint32_t>(to - firstBlock), block.frame };
            memcpy(checkpoints + (number - 1) * sizeof(SiteCheckpoint), &checkpoint, sizeof(SiteCheckpoint));
        }
        to = codes.write(to, take(block.numberOfSymbols));
    }
    RELEASE_ASSERT(left.empty());
    return to;
}

static void codeStreams(SideTables& result, std::span<const Vector<PrefixSymbol>> streams, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<uint32_t> firstEqualStream = findFirstEqualStreams(streams, loops);
    Vector<uint32_t> distinctStreams;
    for (uint32_t position = 0; position < streams.size(); ++position) {
        if (firstEqualStream[position] == position)
            distinctStreams.append(position);
    }

    ChunksOfIndices chunks = divideIntoChunks(static_cast<unsigned>(distinctStreams.size()));
    auto forEachStreamOf = [&](unsigned chunk, const auto& functor) {
        for (unsigned number = chunks.firstIndexOf[chunk]; number < chunks.firstIndexOf[chunk + 1]; ++number)
            functor(distinctStreams[number], SideTableStream::read(streams[distinctStreams[number]].span()));
    };
    Vector<SideTableCodes::Counts> countsOfChunks(chunks.size());
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        countsOfChunks[chunk].fill(0);
        forEachStreamOf(chunk, [&](uint32_t, const SideTableStream& stream) {
            SideTableCodes::count(countsOfChunks[chunk], stream.own);
            SideTableCodes::count(countsOfChunks[chunk], stream.handlers);
            SideTableCodes::count(countsOfChunks[chunk], stream.callSites);
        });
    });
    SideTableCodes::Counts counts { };
    for (const SideTableCodes::Counts& countsOfChunk : countsOfChunks)
        SideTableCodes::add(counts, countsOfChunk);
    SideTableCodes codes(counts);

    Vector<Vector<uint8_t>> bytesOfChunks(chunks.size());
    result.offsets.grow(streams.size());
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        size_t maxSize = 0;
        forEachStreamOf(chunk, [&](uint32_t position, const SideTableStream& stream) {
            constexpr size_t maxSizeOfPartEnd = 16;
            maxSize += streams[position].size() * sizeof(PrefixSymbol) + (4 + stream.head.numberOfBlocks) * maxSizeOfPartEnd;
        });
        Vector<uint8_t>& bytes = bytesOfChunks[chunk];
        bytes.grow(maxSize);
        uint8_t* begin = bytes.mutableSpan().data();
        uint8_t* end = begin;
        forEachStreamOf(chunk, [&](uint32_t position, const SideTableStream& stream) {
            result.offsets[position] = static_cast<uint32_t>(end - begin);
            end = writeStream(end, stream, codes);
        });
        RELEASE_ASSERT(end <= begin + maxSize);
        bytes.shrink(end - begin);
    });

    uint64_t size = codes.bytes().size();
    Vector<uint32_t> firstByteOfChunk(chunks.size());
    for (unsigned chunk = 0; chunk < chunks.size(); ++chunk) {
        firstByteOfChunk[chunk] = static_cast<uint32_t>(size);
        size += bytesOfChunks[chunk].size();
    }
    size += sideTablePadding;
    if (!checkSectionSize(size, context)) {
        result.offsets.fill(0);
        return;
    }
    result.bytes.grow(static_cast<size_t>(size));
    std::span<uint8_t> section = result.bytes.mutableSpan();
    memcpySpan(section, codes.bytes().span());
    memsetSpan(section.last(sideTablePadding), std::numeric_limits<uint8_t>::max());
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        memcpySpan(section.subspan(firstByteOfChunk[chunk]), bytesOfChunks[chunk].span());
        for (unsigned number = chunks.firstIndexOf[chunk]; number < chunks.firstIndexOf[chunk + 1]; ++number)
            result.offsets[distinctStreams[number]] += firstByteOfChunk[chunk];
    });
    for (uint32_t position = 0; position < streams.size(); ++position)
        result.offsets[position] = result.offsets[firstEqualStream[position]];
}

SideTables encodeSideTables(const LinkedFunctions& functions, std::span<const Vector<PrefixSymbol>> callSites, BuildContext& context)
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
    result.sizes.grow(numberOfFunctions);
    std::span<uint32_t> frameMapWords = result.frameMapWords.mutableSpan();
    Vector<Vector<PrefixSymbol>> streams(numberOfFunctions);

    ChunksOfIndices chunks = divideIntoChunks(numberOfFunctions);
    Vector<PositionEncoder> encoders(hasPositions ? chunks.size() : 0);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        SideTableScratch scratch;
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position) {
            const CompiledCode& code = functions[position];
            const SiteDescription& sites = code.sites;
            const RareListsOfSites& rareLists = *sites.rareLists;
            encodeFrameRanges(scratch, code);
            std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
            if (!rareLists.frameMapWords.isEmpty()) {
                frameMapRange.emplace(frameMaps.firstWordOf[position], static_cast<uint32_t>(rareLists.frameMapWords.size()));
                if (frameMaps.storesOwnWords.quickGet(position))
                    memcpySpan(frameMapWords.subspan(frameMapRange->first), rareLists.frameMapWords.span());
            }
            scratch.handlers.shrink(0);
            encodeHandlers(scratch.handlers, rareLists.handlers.span());
            scratch.constructedCallees.shrink(0);
            for (const ConstructedCallee& row : rareLists.constructedCallees) {
                CodeUnit callee = code.body.callees[row.callee];
                RELEASE_ASSERT(callee);
                scratch.constructedCallees.append({ row.site, callee.function.index() });
            }
            OptionSet<ImageFunctionFlag> flagsOfFunction;
            flagsOfFunction.set(ImageFunctionFlag::HasInlineFrames, sites.hasEncodedInlineFrames());
            flagsOfFunction.set(ImageFunctionFlag::HasPositions, hasPositions && !sites.positions.isEmpty());
            flagsOfFunction.set(ImageFunctionFlag::SpillsArgumentRegisters, code.frame.spillsArgumentRegisters);
            SideTableInputs inputs {
                .flagsOfFunction = flagsOfFunction,
                .frameRanges = scratch.frameRanges.span(),
                .frame = frameOf[position],
                .frameMapRange = frameMapRange,
                .handlers = scratch.handlers.span(),
                .constructedCallees = scratch.constructedCallees.span(),
                .callSites = callSites[position].span(),
            };
            appendStream(streams[position], scratch.symbols, inputs);
            result.sizes[position] = static_cast<uint32_t>(scratch.frameRanges.size() + scratch.handlers.size() + callSites[position].size());
            if (!hasPositions)
                continue;
            bool beginsGroup = !(position % PositionIndex::functionsBetween);
            encoders[chunk].encode(sites, beginsGroup ? OwnPosition { } : OwnPosition::of(functions[position - 1].sites));
        }
    });

    times.didFinish("encode"_s);

    codeStreams(result, streams.span(), context);
    times.didFinish("code streams, hold equal ones once"_s);
    if (hasPositions)
        result.positions = joinPositionsOfChunks(chunks, encoders.mutableSpan(), context);
    times.didFinish("code and join positions"_s);
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position)
            streams[position].clear();
        if (hasPositions)
            encoders[chunk] = { };
    });
    times.didFinish("free the streams"_s);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
