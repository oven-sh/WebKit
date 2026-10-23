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
#include "AOTSort.h"
#include "AOTStubKey.h"
#include "AOTThreads.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/Atomics.h>
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

struct HeadWord {
    uint64_t encode() const { return numberOfSites << 3 | hasPrologueChecks << 2 | hasInlineFrames << 1 | hasSpreadSites; }
    static HeadWord decode(uint64_t word) { return { word >> 3, !!(word & 4), !!(word & 2), !!(word & 1) }; }

    uint64_t numberOfSites;
    bool hasPrologueChecks;
    bool hasInlineFrames;
    bool hasSpreadSites;
};

struct InlineFrameWord {
    uint64_t encode() const { return static_cast<uint64_t>(parent) << 2 | isConstruct << 1 | isTailCall; }
    static InlineFrameWord decode(uint64_t word) { return { static_cast<unsigned>(word >> 2), !!(word & 2), !!(word & 1) }; }

    unsigned parent;
    bool isConstruct;
    bool isTailCall;
};

// The frame applies until the next site with details. The three flags apply to this site only.
struct SiteDetails {
    uint64_t encode() const { return static_cast<uint64_t>(frame) << 3 | isFirstRead << 2 | isTailCall << 1 | isConstruct; }
    static SiteDetails decode(uint64_t word) { return { static_cast<unsigned>(word >> 3), !!(word & 4), !!(word & 2), !!(word & 1) }; }

    unsigned frame;
    bool isFirstRead;
    bool isTailCall;
    bool isConstruct;
};

struct SiteWithCode {
    uint32_t returnOffset;
    uint32_t site;
    bool isFirstRead;
};

struct StreamHead {
    const uint8_t* sites() const { return checkpoints + SiteCheckpoint::numberFor(numberOfSites) * sizeof(SiteCheckpoint); }

    uint64_t numberOfSites { 0 };
    bool hasSpreadSites { false };
    Vector<std::pair<uint32_t, uint32_t>, 1> prologueChecks;
    uint64_t numberOfSitesWithoutCode { 0 };
    uint64_t numberOfInlineFrames { 0 };
    const uint8_t* inlineFrames { nullptr };
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
        if (hasFirstRead) {
            returnSites.append({ returnOffset, firstReadSites.front().site, true });
            firstReadSites = firstReadSites.subspan(1);
        }
        returnSites.append({ returnOffset, *site, false });
    });
    RELEASE_ASSERT(firstReadSites.empty());
    AOT::stableSort(returnSites, { }, &SiteWithCode::returnOffset);
    // Return addresses in a row may share a site. The first of them stands for all.
    returnSites.shrink(std::ranges::unique(returnSites, { }, &SiteWithCode::site).begin() - returnSites.begin());
    return returnSites;
}

static void checkProloguePollCalls(const CompiledCode& code)
{
    for (const Relocation& relocation : code.body.relocations) {
        if (relocation.space != SymbolSpace::Stub || code.body.stubKeys[relocation.symbol].family() != StubFamily::PollAtPrologue)
            continue;
        RELEASE_ASSERT(code.frame.prologueChecks.containsIf([&](const PrologueCheck& check) {
            return check.callOffset == relocation.offset;
        }));
    }
}

static size_t inlineFrameCount(const SiteDescription& sites)
{
    return sites.inlineFrames.isEmpty() ? 0 : sites.inlineFrames.size() - 1;
}

static void appendPrologueChecks(Vector<uint8_t>& stream, std::span<const PrologueCheck> prologueChecks)
{
    appendVarint(stream, prologueChecks.size());
    std::optional<uint32_t> previousCallOffset;
    for (const PrologueCheck& check : prologueChecks) {
        RELEASE_ASSERT(!(check.callOffset % codeOffsetUnit) && !(check.resumeOffset % codeOffsetUnit));
        RELEASE_ASSERT(!previousCallOffset || *previousCallOffset < check.callOffset);
        appendVarint(stream, check.callOffset / codeOffsetUnit);
        appendVarint(stream, check.resumeOffset / codeOffsetUnit);
        previousCallOffset = check.callOffset;
    }
}

static Vector<uint8_t> encodeInlineFrames(const CompiledCode& code)
{
    const Vector<InlineFrame>& inlineFrames = code.sites.inlineFrames;
    uint32_t numberOfSitesWithoutCode = code.sites.numberOfSitesWithoutCode;
    RELEASE_ASSERT(inlineFrames.size() - 1 <= numberOfSitesWithoutCode);
    Vector<uint8_t> frames;
    for (unsigned frame = 1; frame < inlineFrames.size(); ++frame) {
        RELEASE_ASSERT(inlineFrames[frame].parent < frame);
        RELEASE_ASSERT(inlineFrames[frame].callSite == numberOfSitesWithoutCode - (inlineFrames.size() - 1) + frame);
        appendVarint(frames, InlineFrameWord { inlineFrames[frame].parent, inlineFrames[frame].isConstruct, inlineFrames[frame].isTailCall }.encode());
        appendVarint(frames, code.body.callees[inlineFrames[frame].callee].index());
    }
    return frames;
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
    for (auto [offset, site, isFirstRead] : returnSites) {
        ++number;
        RELEASE_ASSERT(site == number);
        RELEASE_ASSERT(!(offset % codeOffsetUnit));
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
        OptionSet<SiteFlag> flags;
        if (!flaggedSites.empty() && flaggedSites.front().site == number)
            flags = flaggedSites.front().flags;
        bool hasDetails = frame != previousFrame || isFirstRead || !flags.isEmpty();
        appendVarint(siteBytes, static_cast<uint64_t>((offset - previousOffset) / codeOffsetUnit) << 1 | hasDetails);
        if (hasDetails)
            appendVarint(siteBytes, SiteDetails { frame, isFirstRead, flags.contains(SiteFlag::IsTailCall), flags.contains(SiteFlag::Constructs) }.encode());
        previousOffset = offset;
        previousFrame = frame;
        previousIsFirstRead = isFirstRead;
    }
    stream.appendVector(siteBytes);
}

static void encodeStream(Vector<uint8_t>& stream, const CompiledCode& code)
{
    const SiteDescription& sites = code.sites;
    std::span<const PrologueCheck> prologueChecks = code.frame.prologueChecks.span();
    checkProloguePollCalls(code);
    Vector<SiteWithCode, 32> returnSites = collectReturnSites(code);
    RELEASE_ASSERT(sites.positions.size() == 1 + static_cast<size_t>(sites.numberOfSitesWithoutCode) + returnSites.size() + sites.spreadSites.size());
    if (returnSites.isEmpty() && prologueChecks.empty())
        return;

    bool hasInlineFrames = sites.hasEncodedInlineFrames();
    bool hasSpreadSites = !returnSites.isEmpty() && !sites.spreadSites.isEmpty();
    appendVarint(stream, HeadWord { returnSites.size(), !prologueChecks.empty(), hasInlineFrames, hasSpreadSites }.encode());
    if (!prologueChecks.empty())
        appendPrologueChecks(stream, prologueChecks);
    appendVarint(stream, sites.numberOfSitesWithoutCode);
    if (hasInlineFrames) {
        Vector<uint8_t> frames = encodeInlineFrames(code);
        appendVarint(stream, inlineFrameCount(sites));
        appendVarint(stream, frames.size());
        stream.appendVector(frames);
    }
    appendSites(stream, code, returnSites.span());
    if (hasSpreadSites) {
        appendVarint(stream, sites.spreadSites.size());
        for (auto& entry : sites.spreadSites) {
            appendVarint(stream, entry.callSite);
            appendVarint(stream, entry.item);
            appendVarint(stream, entry.site);
        }
    }
}

static void updateMaximum(Atomic<uint64_t>& maximum, uint64_t value)
{
    maximum.transactionRelaxed([&](uint64_t& current) {
        if (current >= value)
            return false;
        current = value;
        return true;
    });
}

Vector<Vector<uint8_t>> encodeCallSites(const LinkedFunctions& functions, BuildContext& context)
{
    Vector<Vector<uint8_t>> streams(functions.size());
    Atomic<uint64_t> largestSiteCount { 0 };
    Atomic<uint64_t> largestInlineFrameCount { 0 };
    Atomic<uint64_t> numberOfSites { 0 };
    Atomic<uint64_t> numberOfInlineFrames { 0 };
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& code = functions[position];
        encodeStream(streams[position], code);
        updateMaximum(largestSiteCount, code.sites.positions.size() - 1);
        updateMaximum(largestInlineFrameCount, inlineFrameCount(code.sites));
        numberOfSites.exchangeAdd(code.sites.positions.size() - 1, std::memory_order_relaxed);
        numberOfInlineFrames.exchangeAdd(inlineFrameCount(code.sites), std::memory_order_relaxed);
    });
    context.statistics().count(StatisticID::CallSitesEmitted, numberOfSites.loadRelaxed());
    context.statistics().count(StatisticID::InlineFramesEmitted, numberOfInlineFrames.loadRelaxed());
    context.statistics().count(StatisticID::LargestSiteCountInFunction, largestSiteCount.loadRelaxed());
    context.statistics().count(StatisticID::LargestInlineFrameCountInFunction, largestInlineFrameCount.loadRelaxed());

    return streams;
}

static StreamHead readHead(const uint8_t* cursor);

static std::optional<StreamHead> readHead(const Image& image, uint32_t function)
{
    const ImageFunction& record = image.function(function);
    if (!record.has(ImageFunctionFlag::HasCallSites))
        return std::nullopt;
    return readHead(SideTableParts::read(record, image.section<uint8_t>(ImageSection::SideTables).data()).callSites);
}

static StreamHead readHead(const uint8_t* cursor)
{
    HeadWord word = HeadWord::decode(readVarint(cursor));
    StreamHead head;
    head.numberOfSites = word.numberOfSites;
    head.hasSpreadSites = word.hasSpreadSites;
    if (word.hasPrologueChecks) {
        for (uint64_t count = readVarint(cursor); count--;) {
            uint32_t callOffset = static_cast<uint32_t>(readVarint(cursor) * codeOffsetUnit);
            uint32_t resumeOffset = static_cast<uint32_t>(readVarint(cursor) * codeOffsetUnit);
            head.prologueChecks.append(std::pair { callOffset, resumeOffset });
        }
    }
    head.numberOfSitesWithoutCode = readVarint(cursor);
    if (word.hasInlineFrames) {
        head.numberOfInlineFrames = readVarint(cursor);
        uint64_t size = readVarint(cursor);
        head.inlineFrames = cursor;
        cursor += size;
    }
    head.checkpoints = cursor;
    return head;
}

void InlineFrameReader::read(const Image& image, uint32_t function, unsigned last)
{
    std::optional<StreamHead> head = readHead(image, function);
    RELEASE_ASSERT(head && last && last <= head->numberOfInlineFrames);
    const uint8_t* cursor = head->inlineFrames;
    m_image = &image;
    m_function = function;
    m_frames.shrink(0);
    m_frames.reserveCapacity(last);
    for (unsigned frame = 1; frame <= last; ++frame) {
        InlineFrameWord word = InlineFrameWord::decode(readVarint(cursor));
        uint32_t callSite = static_cast<uint32_t>(head->numberOfSitesWithoutCode - head->numberOfInlineFrames + frame);
        uint32_t callee = static_cast<uint32_t>(readVarint(cursor));
        RELEASE_ASSERT(word.parent < frame);
        m_frames.unsafeAppendWithoutCapacityCheck(InlineFrameRecord { word.parent, CallSiteIndex(callSite), FunctionID(callee), word.isConstruct, word.isTailCall });
    }
}

SiteFrames::SiteFrames(const Image& image, uint32_t function)
{
    std::optional<StreamHead> head = readHead(image, function);
    if (!head || !head->numberOfSites)
        return;
    m_remainingInlineFrames = head->numberOfInlineFrames;
    m_inlineFrames = head->inlineFrames;
    RELEASE_ASSERT(m_remainingInlineFrames <= head->numberOfSitesWithoutCode);
    m_remainingSuspensionSites = head->numberOfSitesWithoutCode - m_remainingInlineFrames;
    m_remainingSitesWithCode = head->numberOfSites;
    m_sites = head->sites();
}

unsigned SiteFrames::next()
{
    if (m_remainingSuspensionSites) {
        --m_remainingSuspensionSites;
        return 0;
    }
    if (m_remainingInlineFrames) {
        --m_remainingInlineFrames;
        unsigned parent = InlineFrameWord::decode(readVarint(m_inlineFrames)).parent;
        readVarint(m_inlineFrames);
        return parent;
    }
    if (!m_remainingSitesWithCode)
        return 0;
    --m_remainingSitesWithCode;
    if (readVarint(m_sites) & 1)
        m_frame = SiteDetails::decode(readVarint(m_sites)).frame;
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

static std::optional<CallSiteRecord> findCallSite(const StreamHead*, uint32_t returnOffset);

std::optional<CallSiteRecord> Image::callSiteAt(uint32_t function, uint32_t returnOffset) const
{
    std::optional<StreamHead> head = readHead(*this, function);
    if (!head)
        return std::nullopt;
    return findCallSite(&*head, returnOffset);
}

// Everything that a walk over the stack asks about a return address, out of one run of bytes that is read once.
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

static std::optional<CallSiteRecord> findCallSite(const StreamHead* head, uint32_t returnOffset)
{
    // What a prologue check throws comes from in front of everything that the function does: no site of the body speaks for it.
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
    // The site in front of the return address may be the last one in front of a checkpoint.
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
        uint64_t word = readVarint(cursor);
        offset += (word >> 1) * codeOffsetUnit;
        SiteDetails details { frame, false, false, false };
        if (word & 1)
            details = SiteDetails::decode(readVarint(cursor));
        frame = details.frame;
        ++number;
        if (offset > returnOffset)
            break;
        if (offset < returnOffset) {
            hasSiteInFront = !details.isFirstRead && !details.isConstruct && !details.isTailCall;
            numberInFront = number;
            frameInFront = frame;
            continue;
        }
        CallSiteIndex callSite(static_cast<uint32_t>(number));
        if (!details.isFirstRead)
            return CallSiteRecord { callSite, frame, details.isConstruct, details.isTailCall, firstReadSite };
        firstReadSite = callSite;
    }
    if (!hasSiteInFront)
        return std::nullopt;
    return CallSiteRecord { CallSiteIndex(static_cast<uint32_t>(numberInFront)), frameInFront, false, false, std::nullopt };
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

std::optional<CallSiteIndex> Image::spreadSiteAt(uint32_t function, CallSiteIndex callSite, unsigned item) const
{
    std::optional<StreamHead> head = readHead(*this, function);
    if (!head || !head->hasSpreadSites)
        return std::nullopt;
    const uint8_t* cursor = head->sites();
    uint64_t numberOfSites = head->numberOfSites;
    if (uint64_t numberOfCheckpoints = SiteCheckpoint::numberFor(numberOfSites)) {
        cursor += SiteCheckpoint::at(head->checkpoints, numberOfCheckpoints - 1).position;
        numberOfSites -= numberOfCheckpoints * SiteCheckpoint::sitesBetween;
    }
    for (uint64_t count = numberOfSites; count--;) {
        uint64_t word = readVarint(cursor);
        if (word & 1)
            readVarint(cursor);
    }
    for (uint64_t count = readVarint(cursor); count--;) {
        uint64_t spreadCallSite = readVarint(cursor);
        uint64_t spreadItem = readVarint(cursor);
        uint64_t site = readVarint(cursor);
        if (spreadCallSite == callSite.bits() && spreadItem == item)
            return CallSiteIndex(static_cast<uint32_t>(site));
    }
    return std::nullopt;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
