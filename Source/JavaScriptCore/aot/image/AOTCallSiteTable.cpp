/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallSiteTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompiledCode.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTLinkedFunctions.h"
#include "AOTProgramTables.h"
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

// What CallSiteDetails::frame holds in a row of the kind FirstRead: the frame of a read that has a site, or the columns from a read without one to the site of the row, which is plain and in the frame of the site in front.
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

} // anonymous namespace

std::optional<uint32_t> endOfLastReturnSite(const CompiledCode& code)
{
    std::optional<uint32_t> end;
    forEachReturnAddress(code, [&](uint32_t returnOffset, OptionalIndex) {
        end = end ? std::max(*end, returnOffset) : returnOffset;
    });
    return end;
}

static Vector<SiteWithCode, 32> collectReturnSites(const CompiledCode& code)
{
    Vector<SiteWithCode, 32> returnSites;
    std::span<const FirstReadSite> firstReadSites = code.body.firstReadSites.span();
    forEachReturnAddress(code, [&](uint32_t returnOffset, OptionalIndex site) {
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
    AOT::stableSortInLine(returnSites, { }, &SiteWithCode::returnOffset);
    // findCallSite() hands a return address the site in front of it only if that site says nothing but where it is.
    for (size_t i = 1; i < returnSites.size(); ++i)
        RELEASE_ASSERT(returnSites[i].site != returnSites[i - 1].site || !std::ranges::binary_search(code.sites.flaggedSites, returnSites[i].site, { }, &FlaggedSite::site));
    returnSites.shrink(std::ranges::unique(returnSites, { }, &SiteWithCode::site).begin() - returnSites.begin());
    return returnSites;
}

static size_t inlineFrameCount(const SiteDescription& sites)
{
    return sites.inlineFrames.isEmpty() ? 0 : sites.inlineFrames.size() - 1;
}

static void appendSites(Vector<uint8_t>& stream, const CompiledCode& code, std::span<const SiteWithCode> returnSites)
{
    const SiteDescription& sites = code.sites;
    size_t numberOfInlineFrames = inlineFrameCount(sites);
    std::span<const FlaggedSite> flaggedSites = sites.flaggedSites.span();
    uint32_t previousOffset = 0;
    unsigned previousFrame = 0;
    bool previousIsFirstRead = false;
    uint32_t number = sites.numberOfSitesWithoutCode;
    Vector<uint8_t> siteBytes;
    siteBytes.reserveInitialCapacity(2 * returnSites.size());
    unsigned writtenCount = 0;
    for (auto [offset, site, isFirstRead, firstReadColumnDelta] : returnSites) {
        ++number;
        RELEASE_ASSERT(site == number);
        RELEASE_ASSERT(previousIsFirstRead ? offset == previousOffset : offset > previousOffset);
        if (writtenCount && !(writtenCount % SiteCheckpoint::sitesBetween)) {
            SiteCheckpoint checkpoint { static_cast<uint32_t>(previousOffset / codeOffsetUnit), static_cast<uint32_t>(siteBytes.size()), previousFrame };
            stream.append(asByteSpan(checkpoint));
        }
        ++writtenCount;
        unsigned frame = sites.positions[number].inlineFrame;
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
    stream.appendVector(siteBytes);
}

static void encodeStream(Vector<uint8_t>& stream, const CompiledCode& code, size_t numberOfFunctions)
{
    const SiteDescription& sites = code.sites;
    std::span<const PrologueCheck> prologueChecks = code.frame.prologueChecks.span();
    for (const Relocation& relocation : code.body.relocations) {
        if (relocation.space != SymbolSpace::Stub || !isPollAtPrologue(code.body.stubKeys[relocation.symbol]))
            continue;
        RELEASE_ASSERT(code.frame.prologueChecks.containsIf([&](const PrologueCheck& check) {
            return check.callOffset == relocation.offset;
        }));
    }
    Vector<SiteWithCode, 32> returnSites = collectReturnSites(code);
    RELEASE_ASSERT(sites.positions.size() == 1 + static_cast<size_t>(sites.numberOfSitesWithoutCode) + returnSites.size());
    if (returnSites.isEmpty() && prologueChecks.empty())
        return;

    CallSiteHead head;
    head.numberOfSites = returnSites.size();
    head.hasPrologueChecks = !prologueChecks.empty();
    for (const PrologueCheck& check : prologueChecks) {
        RELEASE_ASSERT(head.prologueChecks.isEmpty() || head.prologueChecks.last().callOffset < check.callOffset);
        head.prologueChecks.append({ .callOffset = check.callOffset, .resumeOffset = check.resumeOffset });
    }
    head.numberOfSitesWithoutCode = sites.numberOfSitesWithoutCode;
    Vector<uint8_t> frames;
    if (sites.hasEncodedInlineFrames()) {
        const Vector<InlineFrame>& inlineFrames = sites.inlineFrames;
        RELEASE_ASSERT(inlineFrames.size() - 1 <= sites.numberOfSitesWithoutCode);
        for (unsigned frame = 1; frame < inlineFrames.size(); ++frame) {
            RELEASE_ASSERT(inlineFrames[frame].parent < frame);
            RELEASE_ASSERT(inlineFrames[frame].callSite == sites.numberOfSitesWithoutCode - (inlineFrames.size() - 1) + frame);
            BuiltinID calledBuiltin = inlineFrames[frame].calledBuiltin;
            FunctionID function = calledBuiltin ? functionInFramesOf(calledBuiltin, numberOfFunctions) : code.body.callees[inlineFrames[frame].callee];
            InlineFrameRow { .isTailCall = inlineFrames[frame].isTailCall, .isConstruct = inlineFrames[frame].isConstruct, .parent = inlineFrames[frame].parent, .function = function }.append(frames);
        }
        head.hasInlineFrames = true;
        head.numberOfInlineFrames = inlineFrameCount(sites);
        head.inlineFrames = frames.span();
    }
    head.append(stream);
    appendSites(stream, code, returnSites.span());
}

Vector<Vector<uint8_t>> encodeCallSites(const LinkedFunctions& functions, size_t numberOfFunctions, BuildContext& context)
{
    Vector<Vector<uint8_t>> streams(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        encodeStream(streams[position], functions[position], numberOfFunctions);
    });
    uint64_t largestSiteCount = 0;
    uint64_t largestInlineFrameCount = 0;
    uint64_t numberOfSites = 0;
    uint64_t numberOfInlineFrames = 0;
    for (const CompiledCode& code : functions) {
        largestSiteCount = std::max<uint64_t>(largestSiteCount, code.sites.positions.size() - 1);
        largestInlineFrameCount = std::max<uint64_t>(largestInlineFrameCount, inlineFrameCount(code.sites));
        numberOfSites += code.sites.positions.size() - 1;
        numberOfInlineFrames += inlineFrameCount(code.sites);
    }
    context.statistics().count(StatisticID::CallSitesEmitted, numberOfSites);
    context.statistics().count(StatisticID::InlineFramesEmitted, numberOfInlineFrames);
    context.statistics().count(StatisticID::LargestSiteCountInFunction, largestSiteCount);
    context.statistics().count(StatisticID::LargestInlineFrameCountInFunction, largestInlineFrameCount);
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
    if (functions.isEmpty() && m_function && (m_image != &image || *m_function != function)) {
        functions.grow(DecodedInlineFrames::numberOfFunctions);
        keep();
    }
    if (!functions.isEmpty()) {
        const auto& kept = functions[function % DecodedInlineFrames::numberOfFunctions];
        if (kept.image == &image && kept.function == function && last <= kept.frames.size()) {
            m_image = &image;
            m_function = function;
            m_frames.shrink(0);
            m_frames.append(kept.frames.span());
            return;
        }
    }
    ++m_decoded->m_numberOfTimesDecoded;
    decode(image, function, last);
    if (!functions.isEmpty())
        keep();
}

void InlineFrameReader::keep()
{
    auto& kept = m_decoded->m_functions[*m_function % DecodedInlineFrames::numberOfFunctions];
    kept.image = m_image;
    kept.function = *m_function;
    kept.frames.shrink(0);
    kept.frames.append(m_frames.span());
}

void InlineFrameReader::decode(const Image& image, uint32_t function, unsigned last)
{
    std::optional<StreamHead> head = readHead(image, function);
    RELEASE_ASSERT(head && last && last <= head->numberOfInlineFrames);
    const uint8_t* cursor = head->inlineFrames.data();
    m_image = &image;
    m_function = function;
    m_frames.shrink(0);
    m_frames.reserveCapacity(last);
    for (unsigned frame = 1; frame <= last; ++frame) {
        InlineFrameRow row = InlineFrameRow::read(cursor);
        uint32_t callSite = static_cast<uint32_t>(head->numberOfSitesWithoutCode - head->numberOfInlineFrames + frame);
        RELEASE_ASSERT(row.parent < frame);
        m_frames.unsafeAppendWithoutCapacityCheck(InlineFrameRecord { row, CallSiteIndex(callSite) });
    }
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
    // The comparison is strict so that the scan starts before the first of two reads at one offset, even when a checkpoint separates them.
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
    // A return address without a site of its own has the site in front of it, unless that one says more than where it is.
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

std::optional<CalleeOfSite> Image::calleeOfSiteAt(uint32_t function, CallSiteIndex site) const
{
    const ImageFunction& record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasCalleesOfSites)) [[likely]]
        return std::nullopt;
    return SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables).data()).calleeOfSiteAt(site.bits());
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
    // Looks up a byte of the call itself: the call may be the last instruction of its body, so the return address may belong to the next body.
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

bool catchesExceptions(const Image& image, uint32_t function)
{
    const ImageFunction& record = image.function(function);
    if (!record.has(ImageFunctionFlag::HasHandlers))
        return false;
    std::span<const uint8_t> handlers = SideTableParts::read(record, image.section<uint8_t>(ImageSection::SideTables).data()).handlers;
    const uint8_t* cursor = handlers.data();
    const uint8_t* end = cursor + handlers.size();
    while (cursor < end) {
        if (HandlerRecord { 0, HandlerRow::read(cursor).type }.catchesExceptions())
            return true;
    }
    return false;
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

static void appendPosition(Vector<uint8_t>& stream, PositionState& last, const SitePosition& position)
{
    uint32_t source = sourceNumber(position);
    bool startsOnSameLine = position.constructionStartLine == position.line;
    bool startsInFront = startsOnSameLine ? position.constructionStartColumn < position.column : position.constructionStartLine < position.line;
    if (position.startIsWhereWritten) {
        // The same row behind three zeros, counted forward from the site.
        bool startsBehind = startsOnSameLine ? position.constructionStartColumn > position.column : position.constructionStartLine > position.line;
        if (startsBehind) {
            for (size_t zeros = 0; zeros < maxZerosInFrontOfStart; ++zeros)
                stream.append(0);
            ConstructionStartRow start { .isOnSameLine = startsOnSameLine };
            if (startsOnSameLine)
                start.columnsBack = position.constructionStartColumn - position.column;
            else {
                start.linesBack = position.constructionStartLine - position.line;
                start.column = position.constructionStartColumn;
            }
            start.append(stream);
        }
    } else if (position.constructionStartLine && (startsInFront || position.startIsForCallers)) {
        // That a site has a place for callers says what the site is, so it is written wherever that place lies.
        stream.append(0);
        // No row begins with a zero: a start on another line is at least one line back.
        if (position.startIsForCallers)
            stream.append(0);
        startsOnSameLine |= !startsInFront;
        ConstructionStartRow start { .isOnSameLine = startsOnSameLine };
        if (startsOnSameLine)
            start.columnsBack = startsInFront ? position.column - position.constructionStartColumn : 0;
        else {
            start.linesBack = position.line - position.constructionStartLine;
            start.column = position.constructionStartColumn;
        }
        start.append(stream);
    }
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
    last = { position.line, position.column, source };
}

OwnPosition OwnPosition::of(const SiteDescription& sites)
{
    return { sites.positions[0].line, sourceNumber(sites.positions[0]) };
}

void encodePositions(Vector<uint8_t>& stream, const SiteDescription& sites, const uint8_t* callSites, OwnPosition ofFunctionInFront)
{
    RELEASE_ASSERT(!sites.positions.isEmpty());
    size_t count = sites.positions.size();
    size_t numberOfInlineFrames = inlineFrameCount(sites);
    size_t numberOfSitesWithoutCode = sites.numberOfSitesWithoutCode;
    RELEASE_ASSERT(numberOfInlineFrames <= numberOfSitesWithoutCode && numberOfSitesWithoutCode < count);
    bool followsInlineFrames = sites.hasEncodedInlineFrames();
    // The decoder's only way to learn the frame of a site.
    SiteFrames frames(followsInlineFrames ? callSites : nullptr);

    PositionState start { sites.positions[0].line, sites.positions[0].column, sourceNumber(sites.positions[0]) };
    Vector<uint8_t> checkpoints;
    Vector<uint8_t> positions;
    size_t lastRunEnd = 0;
    Vector<PositionState, 8> states;
    states.fill(start, followsInlineFrames ? numberOfInlineFrames + 1 : 1);
    for (size_t site = 1; site < count; ++site) {
        if (!(site % sitesBetweenPositionCheckpoints)) {
            size_t bytesSinceLast = positions.size() - lastRunEnd;
            ASSERT(bytesSinceLast <= sitesBetweenPositionCheckpoints * (maxZerosInFrontOfStart + ConstructionStartRow::maxSize + PositionRow::maxSize));
            checkpoints.append(static_cast<uint8_t>(bytesSinceLast));
            checkpoints.append(static_cast<uint8_t>(bytesSinceLast >> 8));
            lastRunEnd = positions.size();
            states.fill(start);
        }
        appendPosition(positions, states[frames.next()], sites.positions[site]);
    }

    bool hasCheckpoints = !checkpoints.isEmpty();
    bool isInOtherSource = start.source != ofFunctionInFront.source;
    PositionsHead {
        .followsInlineFrames = followsInlineFrames,
        .hasCheckpoints = hasCheckpoints,
        .size = checkpoints.size() + positions.size(),
        .isInOtherSource = isInOtherSource,
        .lineChange = start.line - (isInOtherSource ? 0 : static_cast<int64_t>(ofFunctionInFront.line)),
        .sourceChange = static_cast<int64_t>(start.source) - ofFunctionInFront.source,
        .column = sites.positions[0].column,
        .count = hasCheckpoints ? count : 0,
    }.append(stream);
    stream.appendVector(checkpoints);
    stream.appendVector(positions);
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
static constinit std::array<RememberedPosition, 64> rememberedPositions WTF_GUARDED_BY_LOCK(rememberedPositionsLock) { };

void forgetRememberedPositions()
{
    Locker locker { rememberedPositionsLock };
    rememberedPositions.fill({ });
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
    if (function >= header().counts.functions) [[unlikely]] {
        std::optional<SourcePosition> position;
        uint32_t placesInFront = function - header().counts.functions;
        forEachTailCallPlace(programTables().placesOfTailCalls(), [&](const TailCallPlace& place) {
            if (placesInFront--)
                return IterationStatus::Continue;
            position = SourcePosition { { place.line, place.column }, place.sourcePlusOne ? ModuleID(place.sourcePlusOne - 1) : ModuleID() };
            return IterationStatus::Done;
        });
        return position;
    }
    RememberedPosition& remembered = rememberedPositions[(function * 31 + site.bits()) % rememberedPositions.size()];
    {
        Locker locker { rememberedPositionsLock };
        if (remembered.image == this && remembered.function == function && remembered.site == site.bits())
            return SourcePosition { { remembered.line, remembered.column }, remembered.sourcePlusOne ? ModuleID(remembered.sourcePlusOne - 1) : ModuleID(), remembered.constructionStart, remembered.startIsForCallers, remembered.startIsWhereWritten };
    }
    unsigned firstReadColumnDelta = site.bits() >> siteNumberBits;
    std::optional<SourcePosition> position = findPosition(*this, function, CallSiteIndex(site.bits() & ((1u << siteNumberBits) - 1)));
    if (position && firstReadColumnDelta)
        position = SourcePosition { { position->lineColumn.line, position->lineColumn.column - firstReadColumnDelta }, position->source };
    if (position) {
        Locker locker { rememberedPositionsLock };
        remembered = { this, function, site.bits(), position->lineColumn.line, position->lineColumn.column, position->source ? position->source.index() + 1 : 0, position->constructionStart, position->startIsForCallers, position->startIsWhereWritten };
    }
    return position;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
