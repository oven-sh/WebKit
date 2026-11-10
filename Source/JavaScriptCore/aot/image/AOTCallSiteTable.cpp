/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallSiteTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallingConventions.h"
#include "AOTCompiledCode.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTPositionEncoder.h"
#include "AOTSort.h"
#include "AOTStubKey.h"
#include "AOTStubsPolls.h"
#include "AOTThreads.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct SiteWithCode {
    uint32_t returnOffset;
    uint32_t site;
    bool isFirstRead;
    uint32_t firstReadColumnDelta { 0 };
};

struct FirstReadDetails {
    static FirstReadDetails decode(uint32_t bits) { return { !!(bits & 1), bits >> 1 }; }
    uint32_t encode() const { return value << 1 | hasNoSite; }

    bool hasNoSite;
    uint32_t value;
};

ALWAYS_INLINE void updateFrame(const CallSiteDetails& site, unsigned& frame)
{
    if (site.kind != CallSiteKind::FirstRead) [[likely]] {
        frame = site.frame;
        return;
    }
    FirstReadDetails details = FirstReadDetails::decode(site.frame);
    if (!details.hasNoSite)
        frame = details.value;
}

struct PrologueCheckOffsets {
    uint32_t callOffset;
    uint32_t resumeOffset;
};

struct StreamHead {
    SideTableReader readerAt(const uint8_t* begin) const { return reader.at(begin); }
    const uint8_t* firstBlock() const { return checkpoints + SiteCheckpoint::numberFor(numberOfSites) * sizeof(SiteCheckpoint); }

    SideTableReader reader;
    uint64_t numberOfSites { 0 };
    uint64_t numberOfSitesWithoutCode { 0 };
    uint64_t numberOfInlineFrames { 0 };
    Vector<PrologueCheckOffsets, 1> prologueChecks;
    const uint8_t* handlers { nullptr };
    const uint8_t* inlineFrameParents { nullptr };
    const uint8_t* inlineFunctions { nullptr };
    const uint8_t* checkpoints { nullptr };
};

struct CallSiteScratch {
    Vector<SiteWithCode, 32> returnSites;
    Vector<SiteWithCode, 32> laterRunOfReturnSites;
    Vector<PrefixSymbol> head;
    Vector<PrefixSymbol> parents;
    Vector<PrefixSymbol> functions;
    Vector<PrefixSymbol> sites;
    Vector<CallSiteStreamHead::Block> blocks;
};

struct InlineFrameSymbols {
    ALWAYS_INLINE friend void appendVarint(InlineFrameSymbols& row, uint64_t value)
    {
        if (std::exchange(row.writesFunction, !row.writesFunction))
            row.functions.appendVarint(SideTableByte::InlineFunction, SideTableByte::InlineFunctionContinuation, value);
        else
            row.parents.appendVarint(SideTableByte::InlineFrameParent, value);
    }

    SideTableSymbolWriter parents;
    SideTableSymbolWriter functions;
    bool writesFunction { false };
};

struct SiteDetailsSymbols {
    ALWAYS_INLINE friend void appendVarint(SiteDetailsSymbols& details, uint64_t value)
    {
        if (std::exchange(details.writesBuiltin, false)) {
            details.symbols.appendVarint(SideTableByte::CalledBuiltin, SideTableByte::CalledBuiltinContinuation, value);
            return;
        }
        if (details.followsInlined)
            details.symbols.appendVarint(SideTableByte::SiteDetailsAfterInlined, SideTableByte::SiteDetailsAfterInlinedContinuation, value);
        else
            details.symbols.appendVarint(SideTableByte::SiteDetails, SideTableByte::SiteDetailsContinuation, value);
        SiteDetailsWord word { value };
        details.followsInlined = word.isInlined();
        details.writesBuiltin = namesBuiltinOf(word.kind());
    }

    SideTableSymbolWriter& symbols;
    bool followsInlined { false };
    bool writesBuiltin { false };
};

} // anonymous namespace

std::optional<uint32_t> endOfLastReturnSite(const CompiledCode& code)
{
    bool hasReturnSite = false;
    uint32_t end = 0;
    forEachReturnAddress(code, [&](uint32_t returnOffset, OptionalIndex) ALWAYS_INLINE_LAMBDA {
        hasReturnSite = true;
        end = std::max(end, returnOffset);
    });
    return hasReturnSite ? std::optional(end) : std::nullopt;
}

static void mergeRunsOfReturnSites(SiteWithCode* first, SiteWithCode* middle, SiteWithCode* last, Vector<SiteWithCode, 32>& laterRun)
{
    laterRun.shrink(0);
    laterRun.append(std::span<const SiteWithCode>(middle, last));
    const SiteWithCode* firstOfLaterRun = laterRun.span().data();
    const SiteWithCode* endOfLaterRun = firstOfLaterRun + laterRun.size();
    SiteWithCode* endOfEarlierRun = middle;
    SiteWithCode* place = last;
    while (endOfLaterRun != firstOfLaterRun) {
        if (endOfEarlierRun != first && endOfLaterRun[-1].returnOffset < endOfEarlierRun[-1].returnOffset)
            *--place = *--endOfEarlierRun;
        else
            *--place = *--endOfLaterRun;
    }
}

static void sortReturnSitesKeepingEqualOffsetsInOrder(CallSiteScratch& scratch)
{
    SiteWithCode* first = scratch.returnSites.mutableSpan().data();
    SiteWithCode* last = first + scratch.returnSites.size();
    SiteWithCode* endOfFirstRun = std::ranges::is_sorted_until(first, last, { }, &SiteWithCode::returnOffset);
    if (endOfFirstRun == last)
        return;
    SiteWithCode* endOfSecondRun = std::ranges::is_sorted_until(endOfFirstRun, last, { }, &SiteWithCode::returnOffset);
    if (std::ranges::is_sorted_until(endOfSecondRun, last, { }, &SiteWithCode::returnOffset) != last) {
        AOT::stableSortInLine(scratch.returnSites, { }, &SiteWithCode::returnOffset);
        return;
    }
    mergeRunsOfReturnSites(first, endOfFirstRun, endOfSecondRun, scratch.laterRunOfReturnSites);
    if (endOfSecondRun != last)
        mergeRunsOfReturnSites(first, endOfSecondRun, last, scratch.laterRunOfReturnSites);
}

static void collectReturnSites(CallSiteScratch& scratch, const CompiledCode& code)
{
    Vector<SiteWithCode, 32>& returnSites = scratch.returnSites;
    returnSites.shrink(0);
    returnSites.reserveCapacity(code.body.returnSites.size() + code.body.firstReadSites.size() + code.sites.faultingAccesses.size() + code.sites.rareLists->pollSites.size());
    std::span<const FirstReadSite> firstReadSites = code.body.firstReadSites.span();
    forEachReturnAddress(code, [&](uint32_t returnOffset, OptionalIndex site) ALWAYS_INLINE_LAMBDA {
        bool hasFirstRead = !firstReadSites.empty() && firstReadSites.front().returnOffset == returnOffset;
        if (!site) {
            RELEASE_ASSERT(!hasFirstRead);
            return;
        }
        uint32_t firstReadColumnDelta = 0;
        if (hasFirstRead) {
            if (firstReadSites.front().site)
                returnSites.append({ returnOffset, firstReadSites.front().site, true });
            else
                firstReadColumnDelta = firstReadSites.front().columnDelta;
            firstReadSites = firstReadSites.subspan(1);
        }
        returnSites.append({ returnOffset, *site, false, firstReadColumnDelta });
    });
    RELEASE_ASSERT(firstReadSites.empty());
    sortReturnSitesKeepingEqualOffsetsInOrder(scratch);
    const SiteWithCode* inOrder = returnSites.span().data();
    bool repeatsSite = false;
    for (size_t i = 1; i < returnSites.size(); ++i) {
        if (inOrder[i].site != inOrder[i - 1].site)
            continue;
        RELEASE_ASSERT(!std::ranges::binary_search(code.sites.flaggedSites, inOrder[i].site, { }, &FlaggedSite::site));
        repeatsSite = true;
    }
    if (repeatsSite)
        returnSites.shrink(std::ranges::unique(returnSites, { }, &SiteWithCode::site).begin() - returnSites.begin());
}

static size_t inlineFrameCount(const SiteDescription& sites)
{
    return sites.inlineFrames.isEmpty() ? 0 : sites.inlineFrames.size() - 1;
}

static void encodeSites(CallSiteScratch& scratch, const CompiledCode& code)
{
    constexpr size_t maxSymbolsOfDistance = 5;
    constexpr size_t maxSymbolsOfSite = 1 + CallSiteDetails::maxSize + maxSymbolsOfDistance;
    const SiteDescription& sites = code.sites;
    std::span<const SiteWithCode> returnSites = scratch.returnSites.span();
    size_t numberOfInlineFrames = inlineFrameCount(sites);
    std::span<const FlaggedSite> flaggedSites = sites.flaggedSites.span();
    const SitePosition* positions = sites.positions.begin();
    uint32_t previousOffset = 0;
    unsigned previousFrame = 0;
    bool previousIsFirstRead = false;
    uint32_t number = sites.numberOfSitesWithoutCode;
    scratch.blocks.shrink(0);
    scratch.sites.shrink(0);
    scratch.sites.grow(returnSites.size() * maxSymbolsOfSite + SiteCheckpoint::numberFor(returnSites.size()) + 1);
    PrefixSymbol* firstSymbol = scratch.sites.mutableSpan().data();
    SideTableSymbolWriter symbols(firstSymbol);
    std::array<PrefixSymbol, SiteCheckpoint::sitesBetween * maxSymbolsOfDistance> distances;
    while (!returnSites.empty()) {
        std::span<const SiteWithCode> block = returnSites.first(std::min<size_t>(returnSites.size(), SiteCheckpoint::sitesBetween));
        returnSites = returnSites.subspan(block.size());
        CallSiteStreamHead::Block head { 0, static_cast<uint32_t>(previousOffset / codeOffsetUnit), previousFrame };
        PrefixSymbol* firstSymbolOfBlock = symbols.end();
        symbols.append(SideTableByte::SitesWithDetails, 0);
        SideTableSymbolWriter distanceSymbols(distances.data());
        SiteDetailsSymbols detailsSymbols { symbols };
        unsigned sitesWithDetails = 0;
        unsigned sitesSkipped = 0;
        bool followsDetails = false;
        for (auto [offset, site, isFirstRead, firstReadColumnDelta] : block) {
            ++number;
            RELEASE_ASSERT(site == number);
            RELEASE_ASSERT(previousIsFirstRead ? offset == previousOffset : offset > previousOffset);
            unsigned frame = positions[number].inlineFrame;
            RELEASE_ASSERT(frame <= numberOfInlineFrames);
            while (!flaggedSites.empty() && flaggedSites.front().site < number)
                flaggedSites = flaggedSites.subspan(1);
            uint32_t distance = offset - previousOffset;
            RELEASE_ASSERT(!(distance % codeOffsetUnit));
            if (followsDetails)
                distanceSymbols.appendVarint(SideTableByte::DistanceAfterDetails, SideTableByte::DistanceAfterDetailsContinuation, distance / codeOffsetUnit);
            else
                distanceSymbols.appendVarint(SideTableByte::Distance, SideTableByte::DistanceContinuation, distance / codeOffsetUnit);
            CallSiteDetails details;
            bool isFlagged = !flaggedSites.empty() && flaggedSites.front().site == number;
            if (isFirstRead || firstReadColumnDelta) {
                RELEASE_ASSERT(isFirstRead || (!isFlagged && frame == previousFrame));
                details.kind = CallSiteKind::FirstRead;
            } else if (isFlagged) {
                details.kind = flaggedSites.front().kind;
                details.calledBuiltin = flaggedSites.front().calledBuiltin;
            }
            followsDetails = frame != previousFrame || details.kind != CallSiteKind::Plain;
            if (followsDetails) {
                details.frame = details.kind == CallSiteKind::FirstRead ? FirstReadDetails { !isFirstRead, isFirstRead ? frame : firstReadColumnDelta }.encode() : frame;
                symbols.append(SideTableByte::SitesSkipped, static_cast<uint8_t>(sitesSkipped));
                details.append(detailsSymbols);
                ++sitesWithDetails;
                sitesSkipped = 0;
            } else
                ++sitesSkipped;
            previousOffset = offset;
            previousFrame = frame;
            previousIsFirstRead = isFirstRead;
        }
        *firstSymbolOfBlock |= static_cast<PrefixSymbol>(sitesWithDetails);
        symbols.append(std::span<const PrefixSymbol>(distances.data(), distanceSymbols.end()));
        head.numberOfSymbols = static_cast<uint32_t>(symbols.end() - firstSymbolOfBlock);
        scratch.blocks.append(head);
    }
    scratch.sites.shrink(symbols.end() - firstSymbol);
}

static void encodeStream(Vector<PrefixSymbol>& stream, CallSiteScratch& scratch, const CompiledCode& code, const FunctionNumbers& numbers, const Typed::CallingConventions& conventions)
{
    const SiteDescription& sites = code.sites;
    std::span<const PrologueCheck> prologueChecks = code.frame.prologueChecks.span();
    for (size_t i = 1; i < prologueChecks.size(); ++i)
        RELEASE_ASSERT(prologueChecks[i - 1].callOffset < prologueChecks[i].callOffset);
    if (std::ranges::any_of(code.body.stubKeys, isPollAtPrologue)) {
        for (const Relocation& relocation : code.body.relocations) {
            if (relocation.space != SymbolSpace::Stub || !isPollAtPrologue(code.body.stubKeys[relocation.symbol]))
                continue;
            RELEASE_ASSERT(std::ranges::binary_search(prologueChecks, relocation.offset, { }, &PrologueCheck::callOffset));
        }
    }
    collectReturnSites(scratch, code);
    const Vector<SiteWithCode, 32>& returnSites = scratch.returnSites;
    RELEASE_ASSERT(sites.positions.size() == 1 + static_cast<size_t>(sites.numberOfSitesWithoutCode) + returnSites.size());
    if (returnSites.isEmpty() && prologueChecks.empty())
        return;

    CallSiteStreamHead head { };
    scratch.parents.shrink(0);
    scratch.functions.shrink(0);
    if (sites.hasEncodedInlineFrames()) {
        std::span<const InlineFrame> inlineFrames = sites.inlineFrames.span();
        RELEASE_ASSERT(inlineFrames.size() - 1 <= sites.numberOfSitesWithoutCode);
        scratch.parents.grow((inlineFrames.size() - 1) * InlineFrameRow::maxSize);
        scratch.functions.grow((inlineFrames.size() - 1) * InlineFrameRow::maxSize);
        PrefixSymbol* firstParent = scratch.parents.mutableSpan().data();
        PrefixSymbol* firstFunction = scratch.functions.mutableSpan().data();
        InlineFrameSymbols rows { SideTableSymbolWriter(firstParent), SideTableSymbolWriter(firstFunction) };
        for (unsigned frame = 1; frame < inlineFrames.size(); ++frame) {
            RELEASE_ASSERT(inlineFrames[frame].parent < frame);
            RELEASE_ASSERT(inlineFrames[frame].callSite == sites.numberOfSitesWithoutCode - (inlineFrames.size() - 1) + frame);
            BuiltinID calledBuiltin = inlineFrames[frame].calledBuiltin;
            Typed::CodeUnit shown = code.body.callees[inlineFrames[frame].callee];
            FunctionID function = calledBuiltin ? functionInFramesOf(calledBuiltin, numbers.numberOfFunctions()) : shown.role == Typed::CodeRole::Body ? shown.function : numbers.idOf(conventions.indexOf(shown));
            RELEASE_ASSERT(function);
            InlineFrameRow { .isTailCall = inlineFrames[frame].isTailCall, .isConstruct = inlineFrames[frame].isConstruct, .parent = inlineFrames[frame].parent, .function = function }.append(rows);
        }
        scratch.parents.shrink(rows.parents.end() - firstParent);
        scratch.functions.shrink(rows.functions.end() - firstFunction);
        head.hasInlineFrames = true;
    }

    constexpr size_t maxSymbolsOfWord = 10;
    scratch.head.shrink(0);
    scratch.head.grow((4 + 2 * prologueChecks.size()) * maxSymbolsOfWord);
    PrefixSymbol* firstHeadSymbol = scratch.head.mutableSpan().data();
    SideTableSymbolWriter headSymbols(firstHeadSymbol);
    headSymbols.appendVarint(SideTableByte::CallSiteHead, SideTableByte::CallSiteHeadContinuation, static_cast<uint64_t>(returnSites.size()) << 2 | static_cast<uint64_t>(!prologueChecks.empty()) << 1 | head.hasInlineFrames);
    if (!prologueChecks.empty()) {
        headSymbols.appendVarint(SideTableByte::PrologueCheckCount, prologueChecks.size());
        for (const PrologueCheck& check : prologueChecks) {
            RELEASE_ASSERT(!(check.callOffset % codeOffsetUnit) && !(check.resumeOffset % codeOffsetUnit));
            headSymbols.appendVarint(SideTableByte::PrologueCall, SideTableByte::PrologueCallContinuation, check.callOffset / codeOffsetUnit);
            headSymbols.appendVarint(SideTableByte::PrologueResume, check.resumeOffset / codeOffsetUnit);
        }
    }
    headSymbols.appendVarint(SideTableByte::SitesWithoutCode, sites.numberOfSitesWithoutCode);
    if (head.hasInlineFrames)
        headSymbols.appendVarint(SideTableByte::InlineFrameCount, inlineFrameCount(sites));
    scratch.head.shrink(headSymbols.end() - firstHeadSymbol);

    encodeSites(scratch, code);
    head.numberOfHeadSymbols = static_cast<uint32_t>(scratch.head.size());
    head.numberOfParentSymbols = static_cast<uint32_t>(scratch.parents.size());
    head.numberOfFunctionSymbols = static_cast<uint32_t>(scratch.functions.size());
    head.numberOfBlocks = static_cast<uint32_t>(scratch.blocks.size());
    auto symbolsOfBytes = [](std::span<const uint8_t> bytes) {
        return spanReinterpretCast<const PrefixSymbol>(bytes);
    };
    stream.reserveInitialCapacity((sizeof(head) + scratch.blocks.sizeInBytes()) / sizeof(PrefixSymbol) + scratch.head.size() + scratch.parents.size() + scratch.functions.size() + scratch.sites.size());
    stream.append(symbolsOfBytes(asByteSpan(head)));
    stream.append(symbolsOfBytes(asBytes(scratch.blocks.span())));
    stream.append(scratch.head.span());
    stream.append(scratch.parents.span());
    stream.append(scratch.functions.span());
    stream.append(scratch.sites.span());
}

Vector<Vector<PrefixSymbol>> encodeCallSites(const LinkedFunctions& functions, const FunctionNumbers& numbers, const Typed::CallingConventions& conventions, BuildContext& context)
{
    Vector<Vector<PrefixSymbol>> streams(functions.size());
    ChunksOfIndices chunks = divideIntoChunks(static_cast<unsigned>(functions.size()));
    forEachIndexInParallel(context.parallelLoops(), chunks.size(), [&](unsigned chunk) {
        CallSiteScratch scratch;
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position)
            encodeStream(streams[position], scratch, functions[position], numbers, conventions);
    });
    uint64_t numberOfInlineFrames = 0;
    for (const CompiledCode& code : functions)
        numberOfInlineFrames += inlineFrameCount(code.sites);
    context.statistics().count(StatisticID::InlineFramesEmitted, numberOfInlineFrames);
    return streams;
}

SideTableParts SideTableParts::read(ImageFunction function, std::span<const uint8_t> section, uint32_t offset)
{
    SideTableParts parts;
    SideTableReader reader(section, section.data() + function.sideTables);
    reader.next(SideTableByte::Flags);
    parts.frame = SideTableFrame::read(reader, offset);
    if (function.has(ImageFunctionFlag::HasFrameMaps)) {
        parts.firstFrameMapWord = static_cast<uint32_t>(reader.nextVarint(SideTableByte::FrameMapStart, SideTableByte::FrameMapStartContinuation));
        parts.numberOfFrameMapWords = static_cast<uint32_t>(reader.nextVarint(SideTableByte::FrameMapSize));
    }
    parts.hasHandlers = function.has(ImageFunctionFlag::HasHandlers);
    if (function.has(ImageFunctionFlag::HasConstructedCallees)) {
        for (uint64_t size = reader.nextVarint(SideTableByte::Continuation); size; --size)
            parts.constructedCallees.append(reader.next(SideTableByte::ConstructedCallee));
    }
    if (function.has(ImageFunctionFlag::HasCallSites))
        parts.callSites = reader;
    return parts;
}

std::optional<uint32_t> SideTableParts::constructedCalleeAt(uint32_t site) const
{
    const uint8_t* cursor = constructedCallees.begin();
    const uint8_t* end = constructedCallees.end();
    uint64_t listed = 0;
    while (cursor < end && listed < site) {
        listed += readVarint(cursor);
        uint32_t function = static_cast<uint32_t>(readVarint(cursor));
        if (listed == site)
            return function;
    }
    return std::nullopt;
}

static StreamHead readHead(const SideTableParts& parts)
{
    SideTableReader reader = *parts.callSites;
    StreamHead head { .reader = reader };
    uint64_t bits = reader.nextVarint(SideTableByte::CallSiteHead, SideTableByte::CallSiteHeadContinuation);
    bool hasInlineFrames = bits & 1;
    bool hasPrologueChecks = bits & 2;
    head.numberOfSites = bits >> 2;
    if (hasPrologueChecks) {
        for (uint64_t count = reader.nextVarint(SideTableByte::PrologueCheckCount); count; --count) {
            uint64_t call = reader.nextVarint(SideTableByte::PrologueCall, SideTableByte::PrologueCallContinuation);
            uint64_t resume = reader.nextVarint(SideTableByte::PrologueResume);
            head.prologueChecks.append({ static_cast<uint32_t>(call * codeOffsetUnit), static_cast<uint32_t>(resume * codeOffsetUnit) });
        }
    }
    head.numberOfSitesWithoutCode = reader.nextVarint(SideTableByte::SitesWithoutCode);
    if (hasInlineFrames)
        head.numberOfInlineFrames = reader.nextVarint(SideTableByte::InlineFrameCount);
    const uint8_t* next = reader.alignToByte();
    auto passPart = [&] {
        size_t size = static_cast<size_t>(readVarint(next));
        return std::exchange(next, next + size);
    };
    if (parts.hasHandlers)
        head.handlers = passPart();
    if (hasInlineFrames) {
        head.inlineFrameParents = passPart();
        head.inlineFunctions = passPart();
    }
    head.checkpoints = next;
    return head;
}

static std::optional<StreamHead> readHead(const Image& image, uint32_t function)
{
    ImageFunction record = image.function(function);
    if (!record.has(ImageFunctionFlag::HasCallSites))
        return std::nullopt;
    return readHead(SideTableParts::read(record, image.section<uint8_t>(ImageSection::SideTables)));
}

void InlineFrameReader::read(const Image& image, uint32_t function, unsigned last)
{
    if (!m_decoded) {
        decode(image, function, last);
        return;
    }
    auto& functions = m_decoded->m_functions;
    bool isOtherFunction = m_image != &image || m_function != function;
    if (functions.isEmpty() && m_function && isOtherFunction) {
        functions.grow(DecodedInlineFrames::numberOfFunctions);
        keep();
    }
    if (isOtherFunction && !functions.isEmpty()) {
        const auto& kept = functions[function % DecodedInlineFrames::numberOfFunctions];
        if (kept.image == &image && kept.function == function) {
            m_image = &image;
            m_function = function;
            m_frames.shrink(0);
            m_frames.append(kept.frames.span());
            m_rest = kept.rest;
            if (last <= m_frames.size())
                return;
        }
    }
    decode(image, function, last);
    if (!functions.isEmpty())
        keep();
}

void InlineFrameReader::keep()
{
    auto& kept = m_decoded->m_functions[*m_function % DecodedInlineFrames::numberOfFunctions];
    if (kept.image != m_image || kept.function != *m_function) {
        kept.image = m_image;
        kept.function = *m_function;
        kept.frames.shrink(0);
    } else if (kept.frames.size() >= m_frames.size())
        return;
    kept.frames.append(m_frames.span().subspan(kept.frames.size()));
    kept.rest = m_rest;
}

void InlineFrameReader::decode(const Image& image, uint32_t function, unsigned last)
{
    uint32_t callSite = 0;
    if (m_image == &image && m_function == function)
        callSite = m_frames.last().callSite.bits();
    else {
        std::optional<StreamHead> head = readHead(image, function);
        RELEASE_ASSERT(head);
        m_image = &image;
        m_function = function;
        m_frames.shrink(0);
        m_rest = { head->readerAt(head->inlineFrameParents), head->readerAt(head->inlineFunctions), head->numberOfInlineFrames };
        callSite = static_cast<uint32_t>(head->numberOfSitesWithoutCode - head->numberOfInlineFrames);
    }
    unsigned numberOfDecodedFrames = static_cast<unsigned>(m_frames.size());
    RELEASE_ASSERT(last > numberOfDecodedFrames && last - numberOfDecodedFrames <= m_rest.count);
    if (last > m_frames.capacity())
        m_frames.reserveCapacity(std::max<uint64_t>(last, std::min<uint64_t>(2 * m_frames.capacity(), numberOfDecodedFrames + m_rest.count)));
    for (unsigned frame = numberOfDecodedFrames + 1; frame <= last; ++frame) {
        InlineFrameCursor cursor { *m_rest.parents, *m_rest.functions };
        InlineFrameRow row = InlineFrameRow::read(cursor);
        RELEASE_ASSERT(row.parent < frame);
        m_frames.unsafeAppendWithoutCapacityCheck(InlineFrameRecord { row, CallSiteIndex(++callSite) });
    }
    m_rest.count -= last - numberOfDecodedFrames;
}

SiteFrames::SiteFrames(const Image& image, uint32_t function)
{
    std::optional<StreamHead> head = readHead(image, function);
    if (!head || !head->numberOfSites)
        return;
    m_remainingInlineFrames = head->numberOfInlineFrames;
    if (m_remainingInlineFrames)
        m_inlineFrameParents = head->readerAt(head->inlineFrameParents);
    RELEASE_ASSERT(m_remainingInlineFrames <= head->numberOfSitesWithoutCode);
    m_remainingSuspensionSites = head->numberOfSitesWithoutCode - m_remainingInlineFrames;
    m_remainingSitesWithCode = head->numberOfSites;
    m_checkpoints = head->checkpoints;
    m_firstBlock = head->firstBlock();
    m_sites = head->readerAt(m_firstBlock);
    startBlock(m_firstBlock);
}

void SiteFrames::startBlock(const uint8_t* begin)
{
    m_sites = m_sites->at(begin);
    m_details.emplace(*m_sites);
    m_siteInBlock = 0;
    ++m_nextBlock;
}

unsigned SiteFrames::next()
{
    if (m_remainingSuspensionSites) {
        --m_remainingSuspensionSites;
        return 0;
    }
    if (m_remainingInlineFrames) {
        --m_remainingInlineFrames;
        return static_cast<unsigned>(m_inlineFrameParents->nextVarint(SideTableByte::InlineFrameParent) >> 2);
    }
    if (!m_remainingSitesWithCode)
        return 0;
    --m_remainingSitesWithCode;
    if (m_siteInBlock == SiteCheckpoint::sitesBetween)
        startBlock(m_firstBlock + SiteCheckpoint::at(m_checkpoints, m_nextBlock - 1).position);
    if (m_details->isAt(m_siteInBlock++))
        updateFrame(m_details->next(), m_frame);
    return m_frame;
}

void SiteFrames::skip(uint64_t numberOfSites)
{
    uint64_t skippedSuspensionSites = std::min(numberOfSites, m_remainingSuspensionSites);
    m_remainingSuspensionSites -= skippedSuspensionSites;
    numberOfSites -= skippedSuspensionSites;
    for (; numberOfSites && m_remainingInlineFrames; --numberOfSites)
        next();
    uint64_t numberOfCheckpoints = SiteCheckpoint::numberFor(m_remainingSitesWithCode);
    if (uint64_t precedingCount = std::min(numberOfSites / SiteCheckpoint::sitesBetween, numberOfCheckpoints)) {
        SiteCheckpoint checkpoint = SiteCheckpoint::at(m_checkpoints, precedingCount - 1);
        m_nextBlock = precedingCount;
        startBlock(m_firstBlock + checkpoint.position);
        m_frame = checkpoint.frame;
        m_remainingSitesWithCode -= precedingCount * SiteCheckpoint::sitesBetween;
        numberOfSites -= precedingCount * SiteCheckpoint::sitesBetween;
    }
    for (numberOfSites = std::min(numberOfSites, m_remainingSitesWithCode); numberOfSites; --numberOfSites)
        next();
}

static std::optional<CallSiteRecord> findCallSite(const StreamHead* head, uint32_t returnOffset)
{
    for (auto [callOffset, resumeOffset] : head->prologueChecks) {
        if (callOffset + sizeOfNearCall == returnOffset)
            return std::nullopt;
    }
    const uint8_t* block = head->firstBlock();
    uint32_t offset = 0;
    uint64_t number = head->numberOfSitesWithoutCode;
    unsigned frame = 0;
    uint64_t count = head->numberOfSites;
    uint64_t precedingCount = 0;
    for (uint64_t end = SiteCheckpoint::numberFor(count); precedingCount < end;) {
        uint64_t middle = precedingCount + (end - precedingCount) / 2;
        if (SiteCheckpoint::at(head->checkpoints, middle).offset * codeOffsetUnit < returnOffset)
            precedingCount = middle + 1;
        else
            end = middle;
    }
    if (precedingCount)
        --precedingCount;
    if (precedingCount) {
        SiteCheckpoint checkpoint = SiteCheckpoint::at(head->checkpoints, precedingCount - 1);
        offset = checkpoint.offset * codeOffsetUnit;
        frame = checkpoint.frame;
        block += checkpoint.position;
        number += precedingCount * SiteCheckpoint::sitesBetween;
        count -= precedingCount * SiteCheckpoint::sitesBetween;
    }
    bool hasSiteInFront = false;
    uint64_t numberInFront = 0;
    unsigned frameInFront = 0;
    std::optional<CallSiteIndex> firstReadSite;
    struct DetailedSite {
        unsigned site;
        CallSiteDetails details;
    };
    Vector<DetailedSite, 16> detailedSites;
    SideTableReader reader = head->readerAt(block);
    const SideTableReader::DistanceCodes codes = reader.distanceCodes();
    const DetailedSite* nextDetailed = nullptr;
    unsigned siteInBlock = SiteCheckpoint::sitesBetween;
    bool followsDetails = false;
    while (count--) {
        if (siteInBlock == SiteCheckpoint::sitesBetween) {
            reader.alignToByte();
            detailedSites.shrink(0);
            for (SiteDetailsReader details(reader); !details.isAtEnd();) {
                unsigned site = details.site();
                detailedSites.append({ site, details.next() });
            }
            nextDetailed = detailedSites.begin();
            siteInBlock = 0;
            followsDetails = false;
        }
        offset += static_cast<uint32_t>(reader.nextDistance(codes, followsDetails) * codeOffsetUnit);
        followsDetails = nextDetailed != detailedSites.end() && nextDetailed->site == siteInBlock;
        ++siteInBlock;
        CallSiteDetails details;
        if (followsDetails) {
            details = nextDetailed++->details;
            updateFrame(details, frame);
        }
        ++number;
        if (offset > returnOffset)
            break;
        if (offset < returnOffset) {
            hasSiteInFront = details.kind == CallSiteKind::Plain;
            numberInFront = number;
            frameInFront = frame;
            continue;
        }
        CallSiteIndex callSite(static_cast<uint32_t>(number));
        if (details.kind != CallSiteKind::FirstRead)
            return CallSiteRecord { callSite, frame, details.kind, details.calledBuiltin, firstReadSite };
        if (FirstReadDetails firstRead = FirstReadDetails::decode(details.frame); firstRead.hasNoSite)
            return CallSiteRecord { callSite, frame, CallSiteKind::Plain, { }, firstReadSiteInFrontOf(callSite, firstRead.value) };
        firstReadSite = callSite;
    }
    if (!hasSiteInFront)
        return std::nullopt;
    return CallSiteRecord { CallSiteIndex(static_cast<uint32_t>(numberInFront)), frameInFront };
}

std::optional<CallSiteRecord> Image::callSiteAt(uint32_t function, uint32_t returnOffset) const
{
    std::optional<StreamHead> head = readHead(*this, function);
    if (!head)
        return std::nullopt;
    return findCallSite(&*head, returnOffset);
}

std::optional<FunctionID> Image::constructedCalleeAt(uint32_t function, CallSiteIndex site) const
{
    ImageFunction record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasConstructedCallees)) [[likely]]
        return std::nullopt;
    std::optional<uint32_t> callee = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables)).constructedCalleeAt(site.bits());
    return callee ? std::optional(FunctionID(*callee)) : std::nullopt;
}

static std::optional<HandlerRecord> findHandlerAmong(const Invocable<std::optional<HandlerRow>()> auto& nextRow, CallSiteIndex site)
{
    uint64_t endOfLast = 0;
    int64_t target = 0;
    while (std::optional<HandlerRow> row = nextRow()) {
        uint64_t start = endOfLast + row->sitesSkipped;
        endOfLast = start + row->furtherSites + 1;
        target += row->targetChange;
        if (site.bits() < start)
            return std::nullopt;
        if (site.bits() < endOfLast)
            return HandlerRecord { static_cast<uint32_t>(target), row->type };
    }
    return std::nullopt;
}

static std::optional<HandlerRecord> findHandler(const StreamHead& head, CallSiteIndex site)
{
    if (!head.handlers)
        return std::nullopt;
    SideTableReader reader = head.readerAt(head.handlers);
    uint64_t rowsLeft = reader.nextVarint(SideTableByte::HandlerRows);
    return findHandlerAmong([&]() -> std::optional<HandlerRow> {
        if (!rowsLeft)
            return std::nullopt;
        --rowsLeft;
        HandlerCursor cursor { reader };
        return HandlerRow::read(cursor);
    }, site);
}

std::optional<HandlerRecord> Image::handlerAt(uint32_t function, CallSiteIndex site) const
{
    std::optional<StreamHead> head = readHead(*this, function);
    return head ? findHandler(*head, site) : std::nullopt;
}

ReturnSiteInfo Image::returnSiteInfoAt(uint32_t function, uint32_t returnOffset) const
{
    ImageFunction record = this->function(function);
    SideTableParts parts = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables), returnOffset ? returnOffset - 1 : 0);
    ReturnSiteInfo result;
    result.hasConstructedCallees = record.has(ImageFunctionFlag::HasConstructedCallees);
    if (parts.callSites) {
        StreamHead head = readHead(parts);
        result.site = findCallSite(&head, returnOffset);
        if (result.site)
            result.handler = findHandler(head, result.site->callSite);
    }
    if (hasFrameAtReturnOffset(function, parts.frame, returnOffset))
        result.frame = section<ImageFrame>(ImageSection::Frames)[parts.frame.number];
    if (record.has(ImageFunctionFlag::SpillsArgumentRegisters))
        result.frame.calleeSavesStart |= ImageFrame::liesBelowSpilledArgumentsBit;
    return result;
}

std::optional<PrologueCheck> Image::prologueCheckAt(const void* returnAddress) const
{
    CodeLocation location = locate(static_cast<const uint8_t*>(returnAddress) - 1);
    if (location.kind != CodeKind::Function)
        return std::nullopt;
    std::optional<StreamHead> head = readHead(*this, location.function);
    if (!head)
        return std::nullopt;
    for (auto [callOffset, resumeOffset] : head->prologueChecks) {
        if (callOffset + sizeOfNearCall == static_cast<size_t>(location.offset) + 1)
            return PrologueCheck { location.function, callOffset, resumeOffset };
    }
    return std::nullopt;
}

void* resumeAddressAfterStackCheck(const void* returnAddress)
{
    const Image* image = Image::current();
    std::optional<PrologueCheck> check = image->prologueCheckAt(returnAddress);
    RELEASE_ASSERT(check);
    return const_cast<uint8_t*>(image->codeOf(check->function)) + check->resumeOffset;
}

static_assert(!static_cast<uint8_t>(HandlerType::Catch) && static_cast<uint8_t>(HandlerType::Finally) == 1 && static_cast<uint8_t>(HandlerType::SynthesizedCatch) == 2 && static_cast<uint8_t>(HandlerType::SynthesizedFinally) == 3);
static_assert(std::endian::native == std::endian::little);

static constexpr uint8_t takesExceptionBit = 1 << 0;
static constexpr uint8_t ignoresValueBit = 1 << 1;

static_assert(static_cast<uint8_t>(HandlerType::Finally) == takesExceptionBit && static_cast<uint8_t>(HandlerType::SynthesizedCatch) == ignoresValueBit);
static_assert(HandlerRecord { 0, HandlerType::SynthesizedCatch }.ignoresValue() && !HandlerRecord { 0, HandlerType::Catch }.ignoresValue() && !HandlerRecord { 0, HandlerType::Finally }.ignoresValue());

void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler> handlers)
{
    uint32_t endOfLast = 0;
    int64_t targetOfLast = 0;
    while (!handlers.empty()) {
        auto [start, target, type] = handlers.front();
        uint32_t end = start;
        for (; !handlers.empty() && handlers.front().site == end && handlers.front().codeOffset == target && handlers.front().type == type; handlers = handlers.subspan(1))
            ++end;
        RELEASE_ASSERT(start >= endOfLast && end > start);
        HandlerRow { .sitesSkipped = start - endOfLast, .furtherSites = end - start - 1, .type = type, .targetChange = target - targetOfLast }.append(bytes);
        endOfLast = end;
        targetOfLast = target;
    }
}

std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex site)
{
    const uint8_t* cursor = handlers.data();
    const uint8_t* end = cursor + handlers.size();
    return findHandlerAmong([&]() -> std::optional<HandlerRow> {
        if (cursor >= end)
            return std::nullopt;
        return HandlerRow::read(cursor);
    }, site);
}

static constexpr size_t maxZerosInFrontOfStart = 3;
static constexpr size_t maxSizeOfPosition = maxZerosInFrontOfStart + ConstructionStartRow::maxSize + PositionRow::maxSize;
static_assert(sitesBetweenPositionCheckpoints * maxSizeOfPosition * PrefixCode::maxLength / 8 < std::numeric_limits<uint16_t>::max(), "a checkpoint stores the size of a run of positions in 16 bits");

namespace {

enum class PreviousRow : uint8_t { None, OnOtherLine, OnSameLine };

enum RowCode : unsigned {
    ColumnDeltaContinuation = 9,
    LineDeltaContinuation,
    SourceStart,
    SourceContinuation,
    ColumnStart,
    OtherByte,
    NumberOfRowCodes
};
static_assert(NumberOfRowCodes == PositionIndex::numberOfCodes);

struct PositionState {
    int64_t line { 0 };
    int64_t column { 0 };
    uint32_t source { 0 };
    PreviousRow previousRow { PreviousRow::None };
    LineColumn constructionStart { 0, 0 };
    bool startIsForCallers { false };
    bool startIsWhereWritten { false };
};

struct PositionsEntry {
    PositionState start;
    bool followsInlineFrames { false };
    std::optional<uint64_t> count;
    const uint8_t* sites { nullptr };
    const uint8_t* end { nullptr };
};

struct RememberedPosition {
    const Image* image;
    uint32_t function;
    uint32_t site;
    uint32_t line;
    uint32_t column;
    uint32_t sourcePlusOne;
    LineColumn constructionStart;
    bool startIsForCallers;
    bool startIsWhereWritten;
};

} // anonymous namespace

static uint32_t sourceNumber(const SitePosition& position)
{
    return position.source ? position.source.index() + 1 : 0;
}

static unsigned firstByteCode(bool followsInlineFrames, bool isSameFrame, PreviousRow previousRow)
{
    unsigned kindOfFrame = !followsInlineFrames ? 0 : isSameFrame ? 1 : 2;
    return kindOfFrame * 3 + std::to_underlying(previousRow);
}

template<typename NextByte>
static void forEachRemainingByte(uint8_t firstByte, unsigned codeOfFirst, const NextByte& nextByte)
{
    uint8_t byte = firstByte;
    auto finishVarint = [&](unsigned code) {
        while (byte & 0x80)
            byte = nextByte(code);
    };
    if (!byte) {
        for (size_t zeros = 0; !byte && zeros < maxZerosInFrontOfStart; ++zeros)
            byte = nextByte(OtherByte);
        bool startsOnSameLine = byte & 1;
        finishVarint(OtherByte);
        if (!startsOnSameLine) {
            byte = nextByte(OtherByte);
            finishVarint(OtherByte);
        }
        byte = nextByte(codeOfFirst);
    }
    bool isOnSameLine = byte & 1;
    bool isInOtherSource = !isOnSameLine && byte & 2;
    finishVarint(isOnSameLine ? ColumnDeltaContinuation : LineDeltaContinuation);
    if (isOnSameLine)
        return;
    if (isInOtherSource) {
        byte = nextByte(SourceStart);
        finishVarint(SourceContinuation);
    }
    byte = nextByte(ColumnStart);
    finishVarint(OtherByte);
}

namespace {

class RowSymbols {
public:
    explicit RowSymbols(PrefixSymbol* firstReservedSymbol)
        : m_symbols(firstReservedSymbol)
    {
    }

    PrefixSymbol* end() const { return m_symbols.end(); }
    void appendStartMarker(unsigned codeOfFirst, size_t number)
    {
        for (size_t zeros = 0; zeros < number; ++zeros)
            m_symbols.append(zeros ? static_cast<unsigned>(OtherByte) : codeOfFirst, 0);
    }
    ALWAYS_INLINE void appendColumnChange(unsigned codeOfFirst, int64_t change)
    {
        m_symbols.appendVarint(codeOfFirst, ColumnDeltaContinuation, zigZagEncode(change) << 1 | 1);
    }
    ALWAYS_INLINE void appendLineChange(unsigned codeOfFirst, int64_t change, std::optional<uint32_t> otherSource, uint32_t column)
    {
        m_symbols.appendVarint(codeOfFirst, LineDeltaContinuation, zigZagEncode(change) << 2 | static_cast<uint64_t>(otherSource.has_value()) << 1);
        if (otherSource)
            m_symbols.appendVarint(SourceStart, SourceContinuation, *otherSource);
        m_symbols.appendVarint(ColumnStart, OtherByte, column);
    }
    friend void appendVarint(RowSymbols& rows, uint64_t value) { rows.m_symbols.appendVarint(OtherByte, OtherByte, value); }

private:
    PrefixSymbolWriter m_symbols;
};

struct PositionInFront {
    uint32_t line;
    uint32_t column;
    uint32_t source;
    uint32_t numberOfRun;
    PreviousRow previousRow;
};

} // anonymous namespace

static void appendConstructionStart(RowSymbols& rows, unsigned codeOfFirst, const SitePosition& position, const SiteConstructionStart& constructionStart)
{
    bool startsOnSameLine = constructionStart.line == position.line;
    bool startsInFront = startsOnSameLine ? constructionStart.column < position.column : constructionStart.line < position.line;
    if (constructionStart.isWhereWritten) {
        bool startsBehind = startsOnSameLine ? constructionStart.column > position.column : constructionStart.line > position.line;
        if (startsBehind) {
            rows.appendStartMarker(codeOfFirst, maxZerosInFrontOfStart);
            ConstructionStartRow start { .isOnSameLine = startsOnSameLine };
            if (startsOnSameLine)
                start.columnsBack = constructionStart.column - position.column;
            else {
                start.linesBack = constructionStart.line - position.line;
                start.column = constructionStart.column;
            }
            start.append(rows);
        }
    } else if (constructionStart.line && (startsInFront || constructionStart.isForCallers)) {
        rows.appendStartMarker(codeOfFirst, constructionStart.isForCallers ? 2 : 1);
        startsOnSameLine |= !startsInFront;
        ConstructionStartRow start { .isOnSameLine = startsOnSameLine };
        if (startsOnSameLine)
            start.columnsBack = startsInFront ? position.column - constructionStart.column : 0;
        else {
            start.linesBack = position.line - constructionStart.line;
            start.column = constructionStart.column;
        }
        start.append(rows);
    }
}

static ALWAYS_INLINE void appendPosition(RowSymbols& rows, unsigned codeOfFirst, PositionInFront& last, const SitePosition& position)
{
    uint32_t source = sourceNumber(position);
    if (position.line == last.line && source == last.source) {
        rows.appendColumnChange(codeOfFirst, static_cast<int64_t>(position.column) - last.column);
        last.previousRow = PreviousRow::OnSameLine;
    } else {
        rows.appendLineChange(codeOfFirst, static_cast<int64_t>(position.line) - last.line, source == last.source ? std::nullopt : std::optional(source), position.column);
        last.line = position.line;
        last.source = source;
        last.previousRow = PreviousRow::OnOtherLine;
    }
    last.column = position.column;
}

OwnPosition OwnPosition::of(const SiteDescription& sites)
{
    return { sites.positions[0].line, sourceNumber(sites.positions[0]) };
}

void PositionEncoder::encode(const SiteDescription& sites, OwnPosition ofFunctionInFront)
{
    RELEASE_ASSERT(!sites.positions.isEmpty());
    const SitePosition* positionsOfSites = sites.positions.begin();
    size_t count = sites.positions.size();
    size_t numberOfInlineFrames = inlineFrameCount(sites);
    size_t numberOfSitesWithoutCode = sites.numberOfSitesWithoutCode;
    RELEASE_ASSERT(numberOfInlineFrames <= numberOfSitesWithoutCode && numberOfSitesWithoutCode < count);
    bool followsInlineFrames = sites.hasEncodedInlineFrames();
    const InlineFrame* inlineFrames = sites.inlineFrames.begin();
    std::span<const SiteConstructionStart> constructionStarts = sites.rareLists->constructionStarts.span();
    size_t numberOfSuspensionSites = numberOfSitesWithoutCode - numberOfInlineFrames;
    auto frameOf = [&](size_t site) -> unsigned {
        if (!followsInlineFrames)
            return 0;
        if (site > numberOfSitesWithoutCode)
            return positionsOfSites[site].inlineFrame;
        return site > numberOfSuspensionSites ? inlineFrames[site - numberOfSuspensionSites].parent : 0;
    };

    PositionInFront start { positionsOfSites[0].line, positionsOfSites[0].column, sourceNumber(positionsOfSites[0]), 0, PreviousRow::None };
    Vector<uint8_t> checkpoints;
    m_roomForSymbols.shrink(0);
    m_roomForSymbols.grow((count - 1) * maxSizeOfPosition);
    PrefixSymbol* firstSymbol = m_roomForSymbols.mutableSpan().data();
    RowSymbols rows(firstSymbol);
    const PrefixSymbol* lastRunEnd = firstSymbol;
    Vector<PositionInFront, 8> positionsInFront;
    positionsInFront.fill(start, followsInlineFrames ? numberOfInlineFrames + 1 : 1);
    uint32_t numberOfRun = 0;
    unsigned frameInFront = 0;
    for (size_t site = 1; site < count; ++site) {
        if (!(site % sitesBetweenPositionCheckpoints)) {
            size_t symbolsSinceLast = rows.end() - lastRunEnd;
            ASSERT(symbolsSinceLast <= sitesBetweenPositionCheckpoints * maxSizeOfPosition);
            checkpoints.append(static_cast<uint8_t>(symbolsSinceLast));
            checkpoints.append(static_cast<uint8_t>(symbolsSinceLast >> 8));
            lastRunEnd = rows.end();
            start.numberOfRun = ++numberOfRun;
            frameInFront = 0;
        }
        unsigned frame = frameOf(site);
        PositionInFront& inFront = positionsInFront[frame];
        if (inFront.numberOfRun != numberOfRun)
            inFront = start;
        unsigned codeOfFirst = firstByteCode(followsInlineFrames, frame == std::exchange(frameInFront, frame), inFront.previousRow);
        if (!constructionStarts.empty() && constructionStarts.front().site == site) {
            appendConstructionStart(rows, codeOfFirst, positionsOfSites[site], constructionStarts.front());
            constructionStarts = constructionStarts.subspan(1);
        }
        appendPosition(rows, codeOfFirst, inFront, positionsOfSites[site]);
    }
    RELEASE_ASSERT(constructionStarts.empty());

    std::span<const PrefixSymbol> symbols(firstSymbol, rows.end());
    PositionCodes::count(m_counts, symbols);
    m_symbols.append(symbols);
    bool hasCheckpoints = !checkpoints.isEmpty();
    bool isInOtherSource = start.source != ofFunctionInFront.source;
    std::array<uint8_t, PositionsHead::maxSize> roomForHead;
    WriterOfReservedBytes head(roomForHead.data());
    PositionsHead {
        .followsInlineFrames = followsInlineFrames,
        .hasCheckpoints = hasCheckpoints,
        .size = checkpoints.size() + symbols.size(),
        .isInOtherSource = isInOtherSource,
        .lineChange = static_cast<int64_t>(start.line) - (isInOtherSource ? 0 : ofFunctionInFront.line),
        .sourceChange = static_cast<int64_t>(start.source) - ofFunctionInFront.source,
        .column = positionsOfSites[0].column,
        .count = hasCheckpoints ? count : 0,
    }.append(head);
    m_heads.append(std::span<const uint8_t>(roomForHead.data(), head.end()));
    m_heads.appendVector(checkpoints);
}

void PositionEncoder::appendNextCoded(Vector<uint8_t>& stream, const PositionCodes& codes)
{
    const uint8_t* begin = m_heads.span().data();
    const uint8_t* cursor = begin + m_nextHead;
    PositionsHead head = PositionsHead::read(cursor);
    size_t numberOfCheckpoints = head.hasCheckpoints ? (head.count - 1) / sitesBetweenPositionCheckpoints : 0;
    const uint8_t* checkpoints = cursor;
    cursor += numberOfCheckpoints * sizeof(uint16_t);
    m_nextHead = cursor - begin;
    std::span<const PrefixSymbol> symbols = m_symbols.subspan(m_nextSymbol, head.size - numberOfCheckpoints * sizeof(uint16_t));
    m_nextSymbol += symbols.size();

    m_roomForBytes.shrink(0);
    m_roomForBytes.grow(symbols.size() * PrefixCode::maxLength / 8 + numberOfCheckpoints + 1);
    uint8_t* firstCodedByte = m_roomForBytes.mutableSpan().data();
    uint8_t* lastRunEnd = firstCodedByte;
    Vector<uint8_t> codedCheckpoints;
    for (size_t run = 0; run <= numberOfCheckpoints; ++run) {
        bool isLast = run == numberOfCheckpoints;
        size_t sizeOfRun = isLast ? symbols.size() : checkpoints[2 * run] | checkpoints[2 * run + 1] << 8;
        uint8_t* endOfCodedRun = codes.write(lastRunEnd, symbols.first(sizeOfRun));
        symbols = symbols.subspan(sizeOfRun);
        if (!isLast) {
            size_t size = endOfCodedRun - lastRunEnd;
            codedCheckpoints.append(static_cast<uint8_t>(size));
            codedCheckpoints.append(static_cast<uint8_t>(size >> 8));
        }
        lastRunEnd = endOfCodedRun;
    }

    std::span<const uint8_t> codedRuns(firstCodedByte, lastRunEnd);
    head.size = codedCheckpoints.size() + codedRuns.size();
    std::array<uint8_t, PositionsHead::maxSize> roomForHead;
    WriterOfReservedBytes headBytes(roomForHead.data());
    head.append(headBytes);
    stream.append(std::span<const uint8_t>(roomForHead.data(), headBytes.end()));
    stream.appendVector(codedCheckpoints);
    stream.append(codedRuns);
}

static PositionsEntry readEntry(const uint8_t*& cursor, OwnPosition& ofFunctionInFront)
{
    PositionsEntry entry;
    PositionsHead head = PositionsHead::read(cursor);
    entry.start.source = static_cast<uint32_t>(ofFunctionInFront.source + head.sourceChange);
    entry.start.line = (head.isInOtherSource ? 0 : ofFunctionInFront.line) + head.lineChange;
    entry.start.column = head.column;
    entry.followsInlineFrames = head.followsInlineFrames;
    if (head.hasCheckpoints)
        entry.count = head.count;
    entry.sites = cursor;
    entry.end = cursor + head.size;
    cursor = entry.end;
    ofFunctionInFront = { static_cast<uint32_t>(entry.start.line), entry.start.source };
    return entry;
}

static void readPosition(const uint8_t*& cursor, PositionState& state)
{
    std::optional<ConstructionStartRow> start;
    state.startIsForCallers = false;
    state.startIsWhereWritten = false;
    if (!*cursor) {
        ++cursor;
        if (!*cursor) {
            ++cursor;
            state.startIsWhereWritten = !*cursor;
            state.startIsForCallers = !state.startIsWhereWritten;
            cursor += state.startIsWhereWritten;
        }
        start = ConstructionStartRow::read(cursor);
    }
    PositionRow row = PositionRow::read(cursor);
    if (row.isOnSameLine)
        state.column += row.columnChange;
    else {
        state.line += row.lineChange;
        if (row.isInOtherSource)
            state.source = row.source;
        state.column = row.column;
    }
    state.previousRow = row.isOnSameLine ? PreviousRow::OnSameLine : PreviousRow::OnOtherLine;
    state.constructionStart = { 0, 0 };
    if (!start)
        return;
    if (state.startIsWhereWritten)
        state.constructionStart = start->isOnSameLine ? LineColumn { static_cast<unsigned>(state.line), static_cast<unsigned>(state.column + start->columnsBack) } : LineColumn { static_cast<unsigned>(state.line + start->linesBack), start->column };
    else if (start->isOnSameLine)
        state.constructionStart = { static_cast<unsigned>(state.line), static_cast<unsigned>(state.column - start->columnsBack) };
    else
        state.constructionStart = { static_cast<unsigned>(state.line - start->linesBack), start->column };
}

static bool readCodedPosition(PrefixCodeReader& reader, const uint8_t* section, unsigned codeOfFirst, PositionState& state)
{
    unsigned firstByte = reader.read(PositionCodes::code(section, codeOfFirst));
    if (firstByte == PrefixCode::endOfStream)
        return false;
    std::array<uint8_t, maxSizeOfPosition> bytes;
    size_t size = 0;
    bytes[size++] = static_cast<uint8_t>(firstByte);
    forEachRemainingByte(bytes[0], codeOfFirst, [&](unsigned code) {
        RELEASE_ASSERT(size < bytes.size());
        return bytes[size++] = static_cast<uint8_t>(reader.read(PositionCodes::code(section, code)));
    });
    const uint8_t* cursor = bytes.data();
    readPosition(cursor, state);
    return true;
}

static SourcePosition sourcePositionFor(const PositionState& state)
{
    return { { static_cast<unsigned>(state.line), static_cast<unsigned>(state.column) }, state.source ? ModuleID(state.source - 1) : ModuleID(), state.constructionStart, state.startIsForCallers, state.startIsWhereWritten };
}

std::optional<SourcePosition> decodePosition(const Image& image, uint32_t function, const uint8_t* section, const uint8_t* entryInSection, OwnPosition ofFunctionInFront, CallSiteIndex site)
{
    PositionsEntry entry = readEntry(entryInSection, ofFunctionInFront);
    const PositionState& start = entry.start;
    const uint8_t* cursor = entry.sites;

    uint64_t number = site.bits();
    if (entry.count && number >= *entry.count)
        return std::nullopt;
    if (!number)
        return sourcePositionFor(start);

    uint64_t first = 1;
    if (entry.count) {
        constexpr uint64_t sitesBetween = sitesBetweenPositionCheckpoints;
        uint64_t numberOfCheckpoints = (*entry.count - 1) / sitesBetween;
        const uint8_t* checkpoints = cursor;
        cursor += numberOfCheckpoints * sizeof(uint16_t);
        uint64_t precedingCount = number / sitesBetween;
        for (uint64_t i = 0; i < precedingCount; ++i)
            cursor += checkpoints[2 * i] | checkpoints[2 * i + 1] << 8;
        if (precedingCount)
            first = precedingCount * sitesBetween;
    }

    std::optional<SiteFrames> frames;
    if (entry.followsInlineFrames) {
        frames.emplace(image, function);
        frames->skip(first - 1);
    }
    Vector<PositionState, 8> states { start };
    PrefixCodeReader reader(cursor, entry.end);
    unsigned frame = 0;
    for (uint64_t i = first; i <= number; ++i) {
        unsigned frameInFront = frame;
        if (frames)
            frame = frames->next();
        while (frame >= states.size())
            states.append(start);
        unsigned codeOfFirst = firstByteCode(entry.followsInlineFrames, frame == frameInFront, states[frame].previousRow);
        if (!readCodedPosition(reader, section, codeOfFirst, states[frame]))
            return std::nullopt;
    }
    return sourcePositionFor(states[frame]);
}

static Lock rememberedPositionsLock;
static constexpr size_t numberOfRememberedPositions = 1024;
static RememberedPosition* rememberedPositions WTF_GUARDED_BY_LOCK(rememberedPositionsLock);

void forgetRememberedPositions()
{
    Locker locker { rememberedPositionsLock };
    if (rememberedPositions)
        std::fill_n(rememberedPositions, numberOfRememberedPositions, RememberedPosition { });
}

static std::optional<SourcePosition> findPosition(const Image& image, uint32_t function, CallSiteIndex site)
{
    const ImageFunction& record = image.function(function);
    if (!record.has(ImageFunctionFlag::HasPositions))
        return std::nullopt;
    const uint8_t* section = image.section<uint8_t>(ImageSection::Positions).data();
    const uint8_t* cursor = PositionIndex::firstEntry(section, function / PositionIndex::functionsBetween, image.header().counts.functions);
    OwnPosition ofFunctionInFront;
    for (unsigned entriesInFront = function % PositionIndex::functionsBetween; entriesInFront; --entriesInFront)
        readEntry(cursor, ofFunctionInFront);
    return decodePosition(image, function, section, cursor, ofFunctionInFront, site);
}

std::optional<SourcePosition> Image::positionOf(uint32_t function, CallSiteIndex site) const
{
    size_t place = (function * 31 + site.bits()) % numberOfRememberedPositions;
    {
        Locker locker { rememberedPositionsLock };
        if (!rememberedPositions)
            rememberedPositions = static_cast<RememberedPosition*>(fastZeroedMalloc(numberOfRememberedPositions * sizeof(RememberedPosition)));
        const RememberedPosition& remembered = rememberedPositions[place];
        if (remembered.image == this && remembered.function == function && remembered.site == site.bits())
            return SourcePosition { { remembered.line, remembered.column }, remembered.sourcePlusOne ? ModuleID(remembered.sourcePlusOne - 1) : ModuleID(), remembered.constructionStart, remembered.startIsForCallers, remembered.startIsWhereWritten };
    }
    unsigned firstReadColumnDelta = site.bits() >> siteNumberBits;
    std::optional<SourcePosition> position = findPosition(*this, function, CallSiteIndex(site.bits() & ((1u << siteNumberBits) - 1)));
    if (position && firstReadColumnDelta)
        position = SourcePosition { { position->lineColumn.line, position->lineColumn.column - firstReadColumnDelta }, position->source };
    if (position) {
        Locker locker { rememberedPositionsLock };
        rememberedPositions[place] = { this, function, site.bits(), position->lineColumn.line, position->lineColumn.column, position->source ? position->source.index() + 1 : 0, position->constructionStart, position->startIsForCallers, position->startIsWhereWritten };
    }
    return position;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
