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

struct SiteCheckpoint {
    static constexpr unsigned sitesBetween = 64;
    static uint64_t numberFor(uint64_t numberOfSites) { return numberOfSites ? (numberOfSites - 1) / sitesBetween : 0; }
    static SiteCheckpoint at(const uint8_t* first, uint64_t index)
    {
        SiteCheckpoint result;
        memcpy(&result, first + index * sizeof(SiteCheckpoint), sizeof(SiteCheckpoint));
        return result;
    }

    uint32_t offset;
    uint32_t position;
    uint32_t frame;
};
static_assert(sizeof(SiteCheckpoint) == 12 && sizeof(SiteCheckpoint::position) == 4 && sizeof(SiteCheckpoint::frame) == 4);

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

ALWAYS_INLINE void updateFrame(const CallSiteRow& row, unsigned& frame)
{
    if (!row.hasDetails)
        return;
    if (row.details.kind != CallSiteKind::FirstRead) [[likely]] {
        frame = row.details.frame;
        return;
    }
    FirstReadDetails details = FirstReadDetails::decode(row.details.frame);
    if (!details.hasNoSite)
        frame = details.value;
}

struct StreamHead : CallSiteHead {
    const uint8_t* sites() const { return checkpoints + SiteCheckpoint::numberFor(numberOfSites) * sizeof(SiteCheckpoint); }

    const uint8_t* checkpoints { nullptr };
};

struct CallSiteScratch {
    Vector<SiteWithCode, 32> returnSites;
    Vector<SiteWithCode, 32> laterRunOfReturnSites;
    Vector<uint8_t> head;
    Vector<uint8_t> frames;
    Vector<uint8_t> checkpoints;
    Vector<uint8_t> sites;
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
    const SiteDescription& sites = code.sites;
    std::span<const SiteWithCode> returnSites = scratch.returnSites.span();
    size_t numberOfInlineFrames = inlineFrameCount(sites);
    std::span<const FlaggedSite> flaggedSites = sites.flaggedSites.span();
    const SitePosition* positions = sites.positions.begin();
    uint32_t previousOffset = 0;
    unsigned previousFrame = 0;
    bool previousIsFirstRead = false;
    uint32_t number = sites.numberOfSitesWithoutCode;
    scratch.checkpoints.shrink(0);
    scratch.sites.shrink(0);
    scratch.sites.grow(returnSites.size() * CallSiteRow::maxSize);
    uint8_t* firstSiteByte = scratch.sites.mutableSpan().data();
    WriterOfReservedBytes siteBytes(firstSiteByte);
    unsigned writtenCount = 0;
    for (auto [offset, site, isFirstRead, firstReadColumnDelta] : returnSites) {
        ++number;
        RELEASE_ASSERT(site == number);
        RELEASE_ASSERT(previousIsFirstRead ? offset == previousOffset : offset > previousOffset);
        if (writtenCount && !(writtenCount % SiteCheckpoint::sitesBetween)) {
            SiteCheckpoint checkpoint { static_cast<uint32_t>(previousOffset / codeOffsetUnit), static_cast<uint32_t>(siteBytes.end() - firstSiteByte), previousFrame };
            scratch.checkpoints.append(asByteSpan(checkpoint));
        }
        ++writtenCount;
        unsigned frame = positions[number].inlineFrame;
        RELEASE_ASSERT(frame <= numberOfInlineFrames);
        while (!flaggedSites.empty() && flaggedSites.front().site < number)
            flaggedSites = flaggedSites.subspan(1);
        CallSiteRow row { .distance = offset - previousOffset };
        bool isFlagged = !flaggedSites.empty() && flaggedSites.front().site == number;
        if (isFirstRead || firstReadColumnDelta) {
            RELEASE_ASSERT(isFirstRead || (!isFlagged && frame == previousFrame));
            row.details.kind = CallSiteKind::FirstRead;
        } else if (isFlagged) {
            row.details.kind = flaggedSites.front().kind;
            row.details.calledBuiltin = flaggedSites.front().calledBuiltin;
        }
        row.hasDetails = frame != previousFrame || row.details.kind != CallSiteKind::Plain;
        if (row.details.kind == CallSiteKind::FirstRead)
            row.details.frame = FirstReadDetails { !isFirstRead, isFirstRead ? frame : firstReadColumnDelta }.encode();
        else if (row.hasDetails)
            row.details.frame = frame;
        row.append(siteBytes);
        previousOffset = offset;
        previousFrame = frame;
        previousIsFirstRead = isFirstRead;
    }
    scratch.sites.shrink(siteBytes.end() - firstSiteByte);
}

static void encodeStream(Vector<uint8_t>& stream, CallSiteScratch& scratch, const CompiledCode& code, const FunctionNumbers& numbers, const Typed::CallingConventions& conventions)
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

    CallSiteHead head;
    head.numberOfSites = returnSites.size();
    head.hasPrologueChecks = !prologueChecks.empty();
    head.prologueChecks.reserveInitialCapacity(prologueChecks.size());
    for (const PrologueCheck& check : prologueChecks)
        head.prologueChecks.append({ .callOffset = check.callOffset, .resumeOffset = check.resumeOffset });
    head.numberOfSitesWithoutCode = sites.numberOfSitesWithoutCode;
    if (sites.hasEncodedInlineFrames()) {
        std::span<const InlineFrame> inlineFrames = sites.inlineFrames.span();
        RELEASE_ASSERT(inlineFrames.size() - 1 <= sites.numberOfSitesWithoutCode);
        scratch.frames.shrink(0);
        scratch.frames.grow((inlineFrames.size() - 1) * InlineFrameRow::maxSize);
        uint8_t* firstFrameByte = scratch.frames.mutableSpan().data();
        WriterOfReservedBytes frames(firstFrameByte);
        for (unsigned frame = 1; frame < inlineFrames.size(); ++frame) {
            RELEASE_ASSERT(inlineFrames[frame].parent < frame);
            RELEASE_ASSERT(inlineFrames[frame].callSite == sites.numberOfSitesWithoutCode - (inlineFrames.size() - 1) + frame);
            BuiltinID calledBuiltin = inlineFrames[frame].calledBuiltin;
            Typed::CodeUnit shown = code.body.callees[inlineFrames[frame].callee];
            FunctionID function = calledBuiltin ? functionInFramesOf(calledBuiltin, numbers.numberOfFunctions()) : shown.role == Typed::CodeRole::Body ? shown.function : numbers.idOf(conventions.indexOf(shown));
            RELEASE_ASSERT(function);
            InlineFrameRow { .isTailCall = inlineFrames[frame].isTailCall, .isConstruct = inlineFrames[frame].isConstruct, .parent = inlineFrames[frame].parent, .function = function }.append(frames);
        }
        head.hasInlineFrames = true;
        head.numberOfInlineFrames = inlineFrameCount(sites);
        head.inlineFrames = std::span<const uint8_t>(firstFrameByte, frames.end());
    }
    scratch.head.shrink(0);
    head.append(scratch.head);
    encodeSites(scratch, code);
    stream.reserveInitialCapacity(scratch.head.size() + scratch.checkpoints.size() + scratch.sites.size());
    stream.append(scratch.head.span());
    stream.append(scratch.checkpoints.span());
    stream.append(scratch.sites.span());
}

Vector<Vector<uint8_t>> encodeCallSites(const LinkedFunctions& functions, const FunctionNumbers& numbers, const Typed::CallingConventions& conventions, BuildContext& context)
{
    Vector<Vector<uint8_t>> streams(functions.size());
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

static StreamHead readHead(const uint8_t* cursor)
{
    StreamHead head { CallSiteHead::read(cursor) };
    head.checkpoints = cursor;
    return head;
}

static const uint8_t* findCallSites(const Image& image, uint32_t function)
{
    const ImageFunction& record = image.function(function);
    if (!record.has(ImageFunctionFlag::HasCallSites))
        return nullptr;
    return SideTableParts::read(record, image.section<uint8_t>(ImageSection::SideTables).data()).callSites;
}

static std::optional<StreamHead> readHead(const Image& image, uint32_t function)
{
    const uint8_t* callSites = findCallSites(image, function);
    if (!callSites)
        return std::nullopt;
    return readHead(callSites);
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
        m_rest = { head->inlineFrames.data(), head->numberOfInlineFrames };
        callSite = static_cast<uint32_t>(head->numberOfSitesWithoutCode - head->numberOfInlineFrames);
    }
    unsigned numberOfDecodedFrames = static_cast<unsigned>(m_frames.size());
    RELEASE_ASSERT(last > numberOfDecodedFrames && last - numberOfDecodedFrames <= m_rest.count);
    if (last > m_frames.capacity())
        m_frames.reserveCapacity(std::max<uint64_t>(last, std::min<uint64_t>(2 * m_frames.capacity(), numberOfDecodedFrames + m_rest.count)));
    for (unsigned frame = numberOfDecodedFrames + 1; frame <= last; ++frame) {
        InlineFrameRow row = InlineFrameRow::read(m_rest.rows);
        RELEASE_ASSERT(row.parent < frame);
        m_frames.unsafeAppendWithoutCapacityCheck(InlineFrameRecord { row, CallSiteIndex(++callSite) });
    }
    m_rest.count -= last - numberOfDecodedFrames;
}

SiteFrames::SiteFrames(const Image& image, uint32_t function)
    : SiteFrames(findCallSites(image, function))
{
}

SiteFrames::SiteFrames(const uint8_t* callSites)
{
    if (!callSites)
        return;
    StreamHead head = readHead(callSites);
    if (!head.numberOfSites)
        return;
    m_remainingInlineFrames = head.numberOfInlineFrames;
    m_inlineFrames = head.inlineFrames.data();
    RELEASE_ASSERT(m_remainingInlineFrames <= head.numberOfSitesWithoutCode);
    m_remainingSuspensionSites = head.numberOfSitesWithoutCode - m_remainingInlineFrames;
    m_remainingSitesWithCode = head.numberOfSites;
    m_sites = head.sites();
}

unsigned SiteFrames::next()
{
    if (m_remainingSuspensionSites) {
        --m_remainingSuspensionSites;
        return 0;
    }
    if (m_remainingInlineFrames) {
        --m_remainingInlineFrames;
        return InlineFrameRow::read(m_inlineFrames).parent;
    }
    if (!m_remainingSitesWithCode)
        return 0;
    --m_remainingSitesWithCode;
    updateFrame(CallSiteRow::read(m_sites), m_frame);
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
        SiteCheckpoint checkpoint = SiteCheckpoint::at(m_sites - numberOfCheckpoints * sizeof(SiteCheckpoint), precedingCount - 1);
        m_sites += checkpoint.position;
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
    const uint8_t* cursor = head->sites();
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
        cursor += checkpoint.position;
        number += precedingCount * SiteCheckpoint::sitesBetween;
        count -= precedingCount * SiteCheckpoint::sitesBetween;
    }
    bool hasSiteInFront = false;
    uint64_t numberInFront = 0;
    unsigned frameInFront = 0;
    std::optional<CallSiteIndex> firstReadSite;
    while (count--) {
        CallSiteRow row = CallSiteRow::read(cursor);
        offset += row.distance;
        updateFrame(row, frame);
        ++number;
        if (offset > returnOffset)
            break;
        if (offset < returnOffset) {
            hasSiteInFront = row.details.kind == CallSiteKind::Plain;
            numberInFront = number;
            frameInFront = frame;
            continue;
        }
        CallSiteIndex callSite(static_cast<uint32_t>(number));
        if (row.details.kind != CallSiteKind::FirstRead)
            return CallSiteRecord { callSite, frame, row.details.kind, row.details.calledBuiltin, firstReadSite };
        if (FirstReadDetails details = FirstReadDetails::decode(row.details.frame); details.hasNoSite)
            return CallSiteRecord { callSite, frame, CallSiteKind::Plain, { }, firstReadSiteInFrontOf(callSite, details.value) };
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
    const ImageFunction& record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasConstructedCallees)) [[likely]]
        return std::nullopt;
    std::optional<uint32_t> callee = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables).data()).constructedCalleeAt(site.bits());
    return callee ? std::optional(FunctionID(*callee)) : std::nullopt;
}

ReturnSiteInfo Image::returnSiteInfoAt(uint32_t function, uint32_t returnOffset) const
{
    const ImageFunction& record = this->function(function);
    SideTableParts parts = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables).data());
    ReturnSiteInfo result;
    if (parts.callSites) {
        StreamHead head = readHead(parts.callSites);
        result.site = findCallSite(&head, returnOffset);
    }
    if (result.site && !parts.handlers.empty())
        result.handler = findHandler(parts.handlers, result.site->callSite);
    if (hasFrameAtReturnOffset(function, parts, returnOffset))
        result.frame = section<ImageFrame>(ImageSection::Frames)[parts.frame];
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
    uint64_t endOfLast = 0;
    int64_t target = 0;
    while (cursor < end) {
        HandlerRow row = HandlerRow::read(cursor);
        uint64_t start = endOfLast + row.sitesSkipped;
        endOfLast = start + row.furtherSites + 1;
        target += row.targetChange;
        if (site.bits() < start)
            return std::nullopt;
        if (site.bits() < endOfLast)
            return HandlerRecord { static_cast<uint32_t>(target), row.type };
    }
    return std::nullopt;
}

static constexpr size_t maxZerosInFrontOfStart = 3;
static_assert(sitesBetweenPositionCheckpoints * (maxZerosInFrontOfStart + ConstructionStartRow::maxSize + PositionRow::maxSize) <= std::numeric_limits<uint16_t>::max(), "a checkpoint stores the size of a run of positions in 16 bits");

namespace {

struct PositionState {
    int64_t line { 0 };
    int64_t column { 0 };
    uint32_t source { 0 };
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

static void appendConstructionStart(WriterOfReservedBytes& stream, const SitePosition& position, const SiteConstructionStart& constructionStart)
{
    bool startsOnSameLine = constructionStart.line == position.line;
    bool startsInFront = startsOnSameLine ? constructionStart.column < position.column : constructionStart.line < position.line;
    if (constructionStart.isWhereWritten) {
        bool startsBehind = startsOnSameLine ? constructionStart.column > position.column : constructionStart.line > position.line;
        if (startsBehind) {
            for (size_t zeros = 0; zeros < maxZerosInFrontOfStart; ++zeros)
                stream.append(0);
            ConstructionStartRow start { .isOnSameLine = startsOnSameLine };
            if (startsOnSameLine)
                start.columnsBack = constructionStart.column - position.column;
            else {
                start.linesBack = constructionStart.line - position.line;
                start.column = constructionStart.column;
            }
            start.append(stream);
        }
    } else if (constructionStart.line && (startsInFront || constructionStart.isForCallers)) {
        stream.append(0);
        if (constructionStart.isForCallers)
            stream.append(0);
        startsOnSameLine |= !startsInFront;
        ConstructionStartRow start { .isOnSameLine = startsOnSameLine };
        if (startsOnSameLine)
            start.columnsBack = startsInFront ? position.column - constructionStart.column : 0;
        else {
            start.linesBack = position.line - constructionStart.line;
            start.column = constructionStart.column;
        }
        start.append(stream);
    }
}

static void appendPosition(WriterOfReservedBytes& stream, PositionState& last, const SitePosition& position)
{
    uint32_t source = sourceNumber(position);
    PositionRow row { .isOnSameLine = position.line == last.line && source == last.source };
    if (row.isOnSameLine)
        row.columnChange = static_cast<int64_t>(position.column) - last.column;
    else {
        row.isInOtherSource = source != last.source;
        row.lineChange = static_cast<int64_t>(position.line) - last.line;
        row.source = row.isInOtherSource ? source : 0;
        row.column = position.column;
    }
    row.append(stream);
    last.line = position.line;
    last.column = position.column;
    last.source = source;
}

OwnPosition OwnPosition::of(const SiteDescription& sites)
{
    return { sites.positions[0].line, sourceNumber(sites.positions[0]) };
}

void encodePositions(Vector<uint8_t>& stream, const SiteDescription& sites, const uint8_t*, OwnPosition ofFunctionInFront)
{
    Vector<uint8_t> roomForPositions;
    encodePositions(stream, roomForPositions, sites, ofFunctionInFront);
}

void encodePositions(Vector<uint8_t>& stream, Vector<uint8_t>& roomForPositions, const SiteDescription& sites, OwnPosition ofFunctionInFront)
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

    PositionState start { positionsOfSites[0].line, positionsOfSites[0].column, sourceNumber(positionsOfSites[0]) };
    Vector<uint8_t> checkpoints;
    constexpr size_t maxSizeOfPosition = maxZerosInFrontOfStart + ConstructionStartRow::maxSize + PositionRow::maxSize;
    roomForPositions.shrink(0);
    roomForPositions.grow((count - 1) * maxSizeOfPosition);
    uint8_t* firstPositionByte = roomForPositions.mutableSpan().data();
    WriterOfReservedBytes positions(firstPositionByte);
    const uint8_t* lastRunEnd = firstPositionByte;
    Vector<PositionState, 8> states;
    states.fill(start, followsInlineFrames ? numberOfInlineFrames + 1 : 1);
    Vector<uint32_t, 8> checkpointsOfStates;
    checkpointsOfStates.fill(0, states.size());
    uint32_t numberOfCheckpoints = 0;
    for (size_t site = 1; site < count; ++site) {
        if (!(site % sitesBetweenPositionCheckpoints)) {
            size_t bytesSinceLast = positions.end() - lastRunEnd;
            ASSERT(bytesSinceLast <= sitesBetweenPositionCheckpoints * maxSizeOfPosition);
            checkpoints.append(static_cast<uint8_t>(bytesSinceLast));
            checkpoints.append(static_cast<uint8_t>(bytesSinceLast >> 8));
            lastRunEnd = positions.end();
            ++numberOfCheckpoints;
        }
        unsigned frame = frameOf(site);
        if (std::exchange(checkpointsOfStates[frame], numberOfCheckpoints) != numberOfCheckpoints)
            states[frame] = start;
        if (!constructionStarts.empty() && constructionStarts.front().site == site) {
            appendConstructionStart(positions, positionsOfSites[site], constructionStarts.front());
            constructionStarts = constructionStarts.subspan(1);
        }
        appendPosition(positions, states[frame], positionsOfSites[site]);
    }
    RELEASE_ASSERT(constructionStarts.empty());

    size_t sizeOfPositions = positions.end() - firstPositionByte;
    bool hasCheckpoints = !checkpoints.isEmpty();
    bool isInOtherSource = start.source != ofFunctionInFront.source;
    std::array<uint8_t, PositionsHead::maxSize> roomForHead;
    WriterOfReservedBytes head(roomForHead.data());
    PositionsHead {
        .followsInlineFrames = followsInlineFrames,
        .hasCheckpoints = hasCheckpoints,
        .size = checkpoints.size() + sizeOfPositions,
        .isInOtherSource = isInOtherSource,
        .lineChange = start.line - (isInOtherSource ? 0 : static_cast<int64_t>(ofFunctionInFront.line)),
        .sourceChange = static_cast<int64_t>(start.source) - ofFunctionInFront.source,
        .column = positionsOfSites[0].column,
        .count = hasCheckpoints ? count : 0,
    }.append(head);
    stream.append(std::span<const uint8_t>(roomForHead.data(), head.end()));
    stream.appendVector(checkpoints);
    stream.append(std::span<const uint8_t>(firstPositionByte, sizeOfPositions));
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

static SourcePosition sourcePositionFor(const PositionState& state)
{
    return { { static_cast<unsigned>(state.line), static_cast<unsigned>(state.column) }, state.source ? ModuleID(state.source - 1) : ModuleID(), state.constructionStart, state.startIsForCallers, state.startIsWhereWritten };
}

std::optional<SourcePosition> decodePosition(const Image& image, uint32_t function, const uint8_t* entryInSection, OwnPosition ofFunctionInFront, CallSiteIndex site)
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
    unsigned frame = 0;
    for (uint64_t i = first; i <= number; ++i) {
        if (cursor == entry.end)
            return std::nullopt;
        if (frames)
            frame = frames->next();
        while (frame >= states.size())
            states.append(start);
        readPosition(cursor, states[frame]);
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
    const uint8_t* table = image.section<uint8_t>(ImageSection::Positions).data();
    uint32_t placeOfFirstEntry;
    memcpy(&placeOfFirstEntry, table + function / PositionIndex::functionsBetween * sizeof(uint32_t), sizeof(uint32_t));
    const uint8_t* cursor = table + PositionIndex::numberOfEntriesFor(image.section<ImageFunction>(ImageSection::Functions).size()) * sizeof(uint32_t) + placeOfFirstEntry;
    OwnPosition ofFunctionInFront;
    for (unsigned entriesInFront = function % PositionIndex::functionsBetween; entriesInFront; --entriesInFront)
        readEntry(cursor, ofFunctionInFront);
    return decodePosition(image, function, cursor, ofFunctionInFront, site);
}

std::optional<SourcePosition> Image::positionOf(uint32_t function, CallSiteIndex site) const
{
    size_t place = (function * 31 + site.bits()) % numberOfRememberedPositions;
    {
        Locker locker { rememberedPositionsLock };
        // The first position that is asked for may be that of an error for memory that is used up. Without memory for them the positions are not remembered.
        if (!rememberedPositions) {
            void* memory = nullptr;
            if (tryFastZeroedMalloc(numberOfRememberedPositions * sizeof(RememberedPosition)).getValue(memory))
                rememberedPositions = static_cast<RememberedPosition*>(memory);
        }
        if (rememberedPositions) {
            const RememberedPosition& remembered = rememberedPositions[place];
            if (remembered.image == this && remembered.function == function && remembered.site == site.bits())
                return SourcePosition { { remembered.line, remembered.column }, remembered.sourcePlusOne ? ModuleID(remembered.sourcePlusOne - 1) : ModuleID(), remembered.constructionStart, remembered.startIsForCallers, remembered.startIsWhereWritten };
        }
    }
    unsigned firstReadColumnDelta = site.bits() >> siteNumberBits;
    std::optional<SourcePosition> position = findPosition(*this, function, CallSiteIndex(site.bits() & ((1u << siteNumberBits) - 1)));
    if (position && firstReadColumnDelta)
        position = SourcePosition { { position->lineColumn.line, position->lineColumn.column - firstReadColumnDelta }, position->source };
    if (position) {
        Locker locker { rememberedPositionsLock };
        if (rememberedPositions)
            rememberedPositions[place] = { this, function, site.bits(), position->lineColumn.line, position->lineColumn.column, position->source ? position->source.index() + 1 : 0, position->constructionStart, position->startIsForCallers, position->startIsWhereWritten };
    }
    return position;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
