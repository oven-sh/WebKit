/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFaultTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTImage.h"
#include "AOTLinkedFunctions.h"
#include "AOTSectionWriter.h"
#include "AOTThreads.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

constexpr uint32_t faultSitesBetweenCheckpoints = 64;

struct FaultTableHeader {
    uint32_t numberOfSites;
    uint32_t numberOfNullChecks;
    uint32_t numberOfFirstWords;
    uint32_t numberOfSecondWords;
    uint32_t defaultSecondWord;
    uint32_t numberOfThirdWords;
};

struct FaultCheckpoint {
    uint32_t previousSiteOffset;
    uint32_t stepsPosition;
    uint32_t firstNullCheck;
};

struct FaultSiteWord {
    uint32_t nullCheck;
    uint32_t word;
};

static_assert(sizeof(FaultTableHeader) == 24 && sizeof(FaultCheckpoint) == 12 && sizeof(FaultSiteWord) == 8);

constexpr uint64_t framelessStepBit = 1 << 0;
constexpr uint64_t loopPollStepBit = 1 << 1;
constexpr unsigned stepDistanceShift = 2;
constexpr uint64_t maxStepSize = sizeOfVarint(static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) << stepDistanceShift | loopPollStepBit | framelessStepBit);

struct FaultTableLayout {
    FaultTableLayout(uint64_t numberOfSites, uint64_t numberOfNullChecks, uint64_t numberOfFirstWords, uint64_t numberOfSecondWords, uint64_t numberOfThirdWords)
        : numberOfCheckpoints((numberOfSites + faultSitesBetweenCheckpoints - 1) / faultSitesBetweenCheckpoints)
        , hasNarrowWordNumbers(numberOfFirstWords <= std::numeric_limits<uint16_t>::max() + 1u)
        , firstWordsOffset(sizeof(FaultTableHeader) + numberOfCheckpoints * sizeof(FaultCheckpoint))
        , secondWordsOffset(firstWordsOffset + numberOfFirstWords * sizeof(uint32_t))
        , thirdWordsOffset(secondWordsOffset + numberOfSecondWords * sizeof(FaultSiteWord))
        , wordNumbersOffset(thirdWordsOffset + numberOfThirdWords * sizeof(FaultSiteWord))
        , stepsOffset(wordNumbersOffset + numberOfNullChecks * (hasNarrowWordNumbers ? sizeof(uint16_t) : sizeof(uint32_t)))
    {
    }

    explicit FaultTableLayout(const FaultTableHeader& header)
        : FaultTableLayout(header.numberOfSites, header.numberOfNullChecks, header.numberOfFirstWords, header.numberOfSecondWords, header.numberOfThirdWords)
    {
    }

    uint64_t sizeWithSteps(uint64_t sizeOfSteps) const { return roundUpToMultipleOf<sizeof(uint32_t)>(stepsOffset + sizeOfSteps); }

    uint64_t numberOfCheckpoints;
    bool hasNarrowWordNumbers;
    uint64_t firstWordsOffset;
    uint64_t secondWordsOffset;
    uint64_t thirdWordsOffset;
    uint64_t wordNumbersOffset;
    uint64_t stepsOffset;
};

struct CollectedSite {
    bool isNullCheck() const { return !(stepBits & loopPollStepBit); }
    uint64_t stepFrom(uint32_t previousOffset) const { return static_cast<uint64_t>(offset - previousOffset) << stepDistanceShift | stepBits; }

    uint32_t offset;
    uint8_t stepBits;
    uint8_t numberOfWords;
    std::array<uint32_t, FaultSite::maxThrowSiteWords> words;
};

class FrameRangeCursor {
public:
    explicit FrameRangeCursor(const FrameDescription& frame)
        : m_ranges(frame.frameRanges.span())
    {
    }

    bool hasFrameAt(uint32_t ascendingOffset)
    {
        while (!m_ranges.empty() && m_ranges.front().end <= ascendingOffset)
            m_ranges = m_ranges.subspan(1);
        return !m_ranges.empty() && m_ranges.front().begin <= ascendingOffset;
    }

private:
    std::span<const CodeRange> m_ranges;
};

CollectedSite createSite(uint32_t bodyStart, uint32_t offsetInBody, uint64_t stepBits, std::span<const uint32_t> words)
{
    uint64_t offset = static_cast<uint64_t>(bodyStart) + offsetInBody;
    RELEASE_ASSERT(offset <= std::numeric_limits<uint32_t>::max() && words.size() <= FaultSite::maxThrowSiteWords);
    CollectedSite site { static_cast<uint32_t>(offset), static_cast<uint8_t>(stepBits), static_cast<uint8_t>(words.size()), { } };
    std::ranges::copy(words, site.words.begin());
    return site;
}

void addFaultSites(Vector<CollectedSite>& sites, const CompiledCode& code, uint32_t bodyStart)
{
    RELEASE_ASSERT(code.frame.hasFrame() || !code.frame.calleeSaveRegisters.registerCount());
    FrameRangeCursor frame(code.frame);
    std::span accesses = code.sites.faultingAccesses.span();
    std::span polls = code.sites.pollSites.span();
    while (!accesses.empty() || !polls.empty()) {
        if (polls.empty() || (!accesses.empty() && accesses.front().codeOffset < polls.front().offset)) {
            const FaultingAccess& access = accesses.front();
            RELEASE_ASSERT(!access.throwSite.isEmpty());
            sites.append(createSite(bodyStart, access.codeOffset, frame.hasFrameAt(access.codeOffset) ? 0 : framelessStepBit, access.throwSite.span()));
            accesses = accesses.subspan(1);
            continue;
        }
        const PollSite& poll = polls.front();
        RELEASE_ASSERT(poll.callerIsFrameless == !frame.hasFrameAt(poll.offset));
        sites.append(createSite(bodyStart, poll.offset, loopPollStepBit | (poll.callerIsFrameless ? framelessStepBit : 0), { }));
        polls = polls.subspan(1);
    }
}

class FaultTableEncoder {
public:
    FaultTableEncoder(std::span<const CollectedSite>, const ParallelLoops&);

    Vector<uint8_t> encode() const;

private:
    struct Block {
        Vector<uint8_t> steps;
        uint32_t numberOfNullChecks { 0 };
    };

    std::span<const CollectedSite> sitesOf(size_t block) const;
    uint32_t siteOffsetBefore(size_t block) const;
    Block encodeBlock(size_t block) const;
    Vector<uint32_t> collectWords(unsigned index) const;
    Vector<FaultSiteWord> collectSparseWords(unsigned index, std::optional<uint32_t> omittedWord) const;
    uint32_t findMostFrequentWord(Vector<uint32_t>&&) const;
    void writeBlock(std::span<uint8_t> section, const FaultTableLayout&, std::span<const uint32_t> firstWords, size_t block) const;

    std::span<const CollectedSite> m_sites;
    const ParallelLoops& m_loops;
    Vector<Block> m_blocks;
    Vector<FaultCheckpoint> m_checkpoints;
    size_t m_sizeOfSteps { 0 };
    uint32_t m_numberOfNullChecks { 0 };
};

FaultTableEncoder::FaultTableEncoder(std::span<const CollectedSite> sites, const ParallelLoops& loops)
    : m_sites(sites)
    , m_loops(loops)
    , m_blocks((sites.size() + faultSitesBetweenCheckpoints - 1) / faultSitesBetweenCheckpoints)
{
    forEachIndexInParallel(m_loops, m_blocks.size(), [&](unsigned block) {
        m_blocks[block] = encodeBlock(block);
    });
    m_checkpoints.reserveInitialCapacity(m_blocks.size());
    for (size_t block = 0; block < m_blocks.size(); ++block) {
        m_checkpoints.append(FaultCheckpoint { siteOffsetBefore(block), static_cast<uint32_t>(m_sizeOfSteps), m_numberOfNullChecks });
        m_sizeOfSteps += m_blocks[block].steps.size();
        m_numberOfNullChecks += m_blocks[block].numberOfNullChecks;
    }
}

std::span<const CollectedSite> FaultTableEncoder::sitesOf(size_t block) const
{
    size_t first = block * faultSitesBetweenCheckpoints;
    return m_sites.subspan(first, std::min<size_t>(faultSitesBetweenCheckpoints, m_sites.size() - first));
}

uint32_t FaultTableEncoder::siteOffsetBefore(size_t block) const
{
    return block ? m_sites[block * faultSitesBetweenCheckpoints - 1].offset : 0;
}

FaultTableEncoder::Block FaultTableEncoder::encodeBlock(size_t block) const
{
    std::span sites = sitesOf(block);
    Block encoded;
    encoded.steps.reserveInitialCapacity(sites.size() * maxStepSize);
    uint32_t previousOffset = siteOffsetBefore(block);
    for (const CollectedSite& site : sites) {
        RELEASE_ASSERT(site.offset > previousOffset);
        appendVarint(encoded.steps, site.stepFrom(previousOffset));
        previousOffset = site.offset;
        encoded.numberOfNullChecks += site.isNullCheck();
    }
    return encoded;
}

Vector<uint32_t> FaultTableEncoder::collectWords(unsigned index) const
{
    return collectInParallel<uint32_t>(m_loops, m_sites.size(), [&](unsigned site, Vector<uint32_t>& words) {
        if (index < m_sites[site].numberOfWords)
            words.append(m_sites[site].words[index]);
    });
}

Vector<FaultSiteWord> FaultTableEncoder::collectSparseWords(unsigned index, std::optional<uint32_t> omittedWord) const
{
    return collectInParallel<FaultSiteWord>(m_loops, m_blocks.size(), [&](unsigned block, Vector<FaultSiteWord>& words) {
        uint32_t nullCheck = m_checkpoints[block].firstNullCheck;
        for (const CollectedSite& site : sitesOf(block)) {
            if (index < site.numberOfWords && site.words[index] != omittedWord)
                words.append(FaultSiteWord { nullCheck, site.words[index] });
            nullCheck += site.isNullCheck();
        }
    });
}

uint32_t FaultTableEncoder::findMostFrequentWord(Vector<uint32_t>&& words) const
{
    parallelSort(m_loops, words.mutableSpan(), std::less<uint32_t> { });
    uint32_t mostFrequentWord = 0;
    size_t highestCount = 0;
    for (size_t begin = 0; begin < words.size();) {
        size_t end = begin + 1;
        while (end < words.size() && words[end] == words[begin])
            ++end;
        if (end - begin > highestCount) {
            mostFrequentWord = words[begin];
            highestCount = end - begin;
        }
        begin = end;
    }
    return mostFrequentWord;
}

void FaultTableEncoder::writeBlock(std::span<uint8_t> section, const FaultTableLayout& layout, std::span<const uint32_t> firstWords, size_t block) const
{
    const FaultCheckpoint& checkpoint = m_checkpoints[block];
    memcpySpan(section.subspan(layout.stepsOffset + checkpoint.stepsPosition), m_blocks[block].steps.span());

    uint8_t* wordNumbers = section.data() + layout.wordNumbersOffset;
    uint32_t nullCheck = checkpoint.firstNullCheck;
    for (const CollectedSite& site : sitesOf(block)) {
        if (!site.isNullCheck())
            continue;
        size_t number = std::ranges::lower_bound(firstWords, site.words[0]) - firstWords.begin();
        if (layout.hasNarrowWordNumbers)
            reinterpret_cast<uint16_t*>(wordNumbers)[nullCheck] = static_cast<uint16_t>(number);
        else
            reinterpret_cast<uint32_t*>(wordNumbers)[nullCheck] = static_cast<uint32_t>(number);
        ++nullCheck;
    }
}

Vector<uint8_t> FaultTableEncoder::encode() const
{
    Vector<uint32_t> firstWords = collectWords(0);
    sortAndRemoveDuplicates(m_loops, firstWords);
    uint32_t defaultSecondWord = findMostFrequentWord(collectWords(1));
    Vector<FaultSiteWord> secondWords = collectSparseWords(1, defaultSecondWord);
    Vector<FaultSiteWord> thirdWords = collectSparseWords(2, std::nullopt);

    FaultTableHeader header {
        .numberOfSites = static_cast<uint32_t>(m_sites.size()),
        .numberOfNullChecks = m_numberOfNullChecks,
        .numberOfFirstWords = static_cast<uint32_t>(firstWords.size()),
        .numberOfSecondWords = static_cast<uint32_t>(secondWords.size()),
        .defaultSecondWord = defaultSecondWord,
        .numberOfThirdWords = static_cast<uint32_t>(thirdWords.size()),
    };
    FaultTableLayout layout(header);

    Vector<uint8_t> bytes(FillWith { }, layout.sizeWithSteps(m_sizeOfSteps), 0);
    std::span<uint8_t> section = bytes.mutableSpan();
    memcpySpan(section, asByteSpan(header));
    memcpySpan(section.subspan(sizeof(FaultTableHeader)), asBytes(m_checkpoints.span()));
    memcpySpan(section.subspan(layout.firstWordsOffset), asBytes(firstWords.span()));
    memcpySpan(section.subspan(layout.secondWordsOffset), asBytes(secondWords.span()));
    memcpySpan(section.subspan(layout.thirdWordsOffset), asBytes(thirdWords.span()));
    forEachIndexInParallel(m_loops, m_blocks.size(), [&](unsigned block) {
        writeBlock(section, layout, firstWords.span(), block);
    });
    return bytes;
}

struct FaultTableView {
    explicit FaultTableView(const uint8_t* bytes)
        : header(*reinterpret_cast<const FaultTableHeader*>(bytes))
        , layout(header)
        , checkpoints(reinterpret_cast<const FaultCheckpoint*>(bytes + sizeof(FaultTableHeader)), layout.numberOfCheckpoints)
        , firstWords(reinterpret_cast<const uint32_t*>(bytes + layout.firstWordsOffset), header.numberOfFirstWords)
        , secondWords(reinterpret_cast<const FaultSiteWord*>(bytes + layout.secondWordsOffset), header.numberOfSecondWords)
        , thirdWords(reinterpret_cast<const FaultSiteWord*>(bytes + layout.thirdWordsOffset), header.numberOfThirdWords)
        , wordNumbers(bytes + layout.wordNumbersOffset)
        , steps(bytes + layout.stepsOffset)
    {
    }

    static std::optional<uint32_t> findWord(std::span<const FaultSiteWord> words, uint32_t nullCheck)
    {
        auto entry = std::ranges::lower_bound(words, nullCheck, { }, &FaultSiteWord::nullCheck);
        if (entry == words.end() || entry->nullCheck != nullCheck)
            return std::nullopt;
        return entry->word;
    }

    std::array<uint32_t, FaultSite::maxThrowSiteWords> throwSiteOf(uint32_t nullCheck) const
    {
        uint32_t number = layout.hasNarrowWordNumbers ? reinterpret_cast<const uint16_t*>(wordNumbers)[nullCheck] : reinterpret_cast<const uint32_t*>(wordNumbers)[nullCheck];
        return { firstWords[number], findWord(secondWords, nullCheck).value_or(header.defaultSecondWord), findWord(thirdWords, nullCheck).value_or(0) };
    }

    const FaultTableHeader& header;
    FaultTableLayout layout;
    std::span<const FaultCheckpoint> checkpoints;
    std::span<const uint32_t> firstWords;
    std::span<const FaultSiteWord> secondWords;
    std::span<const FaultSiteWord> thirdWords;
    const uint8_t* wordNumbers;
    const uint8_t* steps;
};

} // anonymous namespace

Vector<uint8_t> encodeFaultSites(const LinkedFunctions& functions, std::span<const uint32_t> bodyStarts, BuildContext& context)
{
    RELEASE_ASSERT(bodyStarts.size() == functions.size());
    uint64_t numberOfNullChecks = 0;
    uint64_t numberOfLoopPolls = 0;
    for (const CompiledCode& code : functions) {
        numberOfNullChecks += code.sites.faultingAccesses.size();
        numberOfLoopPolls += code.sites.pollSites.size();
    }
    uint64_t numberOfSites = numberOfNullChecks + numberOfLoopPolls;
    if (!numberOfSites)
        return { };

    // Once a Vector can hold this many bytes, no list of the encoder overflows and every count fits 32 bits.
    uint64_t maxSectionSize = FaultTableLayout(numberOfSites, numberOfNullChecks, numberOfNullChecks, numberOfNullChecks, numberOfNullChecks).sizeWithSteps(numberOfSites * maxStepSize);
    if (!checkSectionSize(std::max<uint64_t>(maxSectionSize, numberOfSites * sizeof(CollectedSite)), context))
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<CollectedSite> sites = collectInParallel<CollectedSite>(loops, functions.size(), [&](unsigned position, Vector<CollectedSite>& collected) {
        addFaultSites(collected, functions[position], bodyStarts[position]);
    });
    return FaultTableEncoder(sites.span(), loops).encode();
}

std::optional<FaultSite> Image::faultSiteAt(const void* pc) const
{
    std::span bytes = section<uint8_t>(ImageSection::FaultSites);
    if (bytes.empty())
        return std::nullopt;
    uintptr_t offset = std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(code().data());
    if (offset >= header().code.functionsEnd)
        return std::nullopt;

    FaultTableView table(bytes.data());
    auto nextCheckpoint = std::ranges::lower_bound(table.checkpoints, static_cast<uint32_t>(offset), { }, &FaultCheckpoint::previousSiteOffset);
    if (nextCheckpoint == table.checkpoints.begin())
        return std::nullopt;
    auto checkpoint = std::prev(nextCheckpoint);

    uint32_t firstSite = static_cast<uint32_t>(checkpoint - table.checkpoints.begin()) * faultSitesBetweenCheckpoints;
    uint32_t numberOfSteps = std::min(faultSitesBetweenCheckpoints, table.header.numberOfSites - firstSite);
    const uint8_t* cursor = table.steps + checkpoint->stepsPosition;
    uint64_t currentOffset = checkpoint->previousSiteOffset;
    uint32_t nullCheck = checkpoint->firstNullCheck;
    for (uint32_t i = 0; i < numberOfSteps; ++i) {
        uint64_t step = readVarint(cursor);
        bool isLoopPoll = !!(step & loopPollStepBit);
        currentOffset += step >> stepDistanceShift;
        if (currentOffset < offset) {
            nullCheck += !isLoopPoll;
            continue;
        }
        if (currentOffset > offset)
            return std::nullopt;
        bool hasFrame = !(step & framelessStepBit);
        if (isLoopPoll)
            return FaultSite { FaultSiteKind::LoopPoll, hasFrame, { } };
        return FaultSite { FaultSiteKind::NullCheck, hasFrame, table.throwSiteOf(nullCheck) };
    }
    return std::nullopt;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
