/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFaultTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSites.h"
#include "AOTFaultHandler.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTOperationData.h"
#include "AOTPrefixCode.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "AOTStubKey.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPolls.h"
#include "AOTThreads.h"
#include "AOTUnwinding.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include "MachineContext.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <functional>
#include <limits>
#include <optional>
#include <wtf/MainThread.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/threads/Signals.h>

#if OS(UNIX)
#include <signal.h>
#include <sys/mman.h>
#endif

#if OS(DARWIN)
#include <dlfcn.h>
#elif OS(UNIX)
extern "C" char __executable_start;
extern "C" char etext;
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

constexpr uint32_t faultSitesBetweenCheckpoints = 64;

struct FaultTableHeader {
    uint32_t numberOfSites;
    uint32_t numberOfFirstWords;
    uint32_t defaultSecondWord;
    uint32_t sizeOfCodes;
};

struct FaultCheckpoint {
    uint32_t previousSiteOffset;
    uint32_t stepsPosition;
};

static_assert(sizeof(FaultTableHeader) == 16 && sizeof(FaultCheckpoint) == 8);

struct FaultTableLayout {
    FaultTableLayout(uint64_t numberOfSites, uint64_t numberOfFirstWords, uint64_t sizeOfCodes)
        : numberOfCheckpoints((numberOfSites + faultSitesBetweenCheckpoints - 1) / faultSitesBetweenCheckpoints)
        , firstWordsOffset(sizeof(FaultTableHeader) + numberOfCheckpoints * sizeof(FaultCheckpoint))
        , codesOffset(firstWordsOffset + numberOfFirstWords * sizeof(uint32_t))
        , stepsOffset(codesOffset + sizeOfCodes)
    {
    }

    explicit FaultTableLayout(const FaultTableHeader& header)
        : FaultTableLayout(header.numberOfSites, header.numberOfFirstWords, header.sizeOfCodes)
    {
    }

    uint64_t sizeWithSteps(uint64_t sizeOfSteps) const { return roundUpToMultipleOf<sizeof(uint32_t)>(stepsOffset + sizeOfSteps); }

    uint64_t numberOfCheckpoints;
    uint64_t firstWordsOffset;
    uint64_t codesOffset;
    uint64_t stepsOffset;
};

struct CollectedSite {
    bool isNullCheck() const { return !isLoopPoll; }
    FaultStep stepFrom(uint32_t previousOffset) const { return { .isFrameless = isFrameless, .isLoopPoll = isLoopPoll, .distance = offset - previousOffset }; }

    uint32_t offset;
    bool isFrameless;
    bool isLoopPoll;
    uint8_t numberOfWords;
    uint8_t unused;
    std::array<uint32_t, FaultSite::maxThrowSiteWords> words;
};

enum SiteByte : unsigned { StepStart, StepContinuation, NumberStart, NumberContinuation, LaterWordsCode, ShortWordLow, ShortWordHigh, LaterWordsVarint, NumberOfSiteBytes };
using FaultSiteCodes = PrefixCodes<NumberOfSiteBytes>;

class LaterWordsOfBlock {
public:
    static constexpr size_t maxSize = 1 + 2 * sizeOfVarint(UINT32_MAX);

    static constexpr bool isShort(uint8_t code) { return code >= firstShort; }
    static constexpr unsigned numberOfVarints(uint8_t code) { return code == codeOfTwoWords ? 2 : code == codeOfWide; }

    explicit LaterWordsOfBlock(uint32_t defaultWord)
        : m_defaultWord(defaultWord)
    {
    }

    void append(PrefixSymbolWriter& symbols, std::span<const uint32_t> words)
    {
        uint32_t word = words[0];
        if (words.size() > 1) {
            symbols.append(LaterWordsCode, codeOfTwoWords);
            symbols.appendVarint(LaterWordsVarint, LaterWordsVarint, word);
            symbols.appendVarint(LaterWordsVarint, LaterWordsVarint, words[1]);
            return;
        }
        if (word == m_defaultWord) {
            symbols.append(LaterWordsCode, codeOfDefault);
            return;
        }
        for (uint8_t index = 0; index < m_size; ++index) {
            if (m_words[index] == word) {
                symbols.append(LaterWordsCode, static_cast<uint8_t>(firstRepeated + index));
                return;
            }
        }
        if (word < endOfShortWords) {
            symbols.append(LaterWordsCode, static_cast<uint8_t>(firstShort + (word >> 16)));
            symbols.append(ShortWordLow, static_cast<uint8_t>(word));
            symbols.append(ShortWordHigh, static_cast<uint8_t>(word >> 8));
        } else {
            symbols.append(LaterWordsCode, codeOfWide);
            symbols.appendVarint(LaterWordsVarint, LaterWordsVarint, word);
        }
        note(word);
    }

    std::array<uint32_t, 2> read(const uint8_t*& cursor)
    {
        uint8_t code = *cursor++;
        if (code == codeOfDefault)
            return { m_defaultWord, 0 };
        if (code == codeOfTwoWords) {
            uint32_t first = static_cast<uint32_t>(readVarint(cursor));
            return { first, static_cast<uint32_t>(readVarint(cursor)) };
        }
        if (code < codeOfWide)
            return { m_words[code - firstRepeated], 0 };
        uint32_t word;
        if (code == codeOfWide)
            word = static_cast<uint32_t>(readVarint(cursor));
        else {
            word = static_cast<uint32_t>(code - firstShort) << 16 | static_cast<uint32_t>(cursor[1]) << 8 | cursor[0];
            cursor += 2;
        }
        note(word);
        return { word, 0 };
    }

private:
    static constexpr uint8_t codeOfDefault = 0;
    static constexpr uint8_t firstRepeated = 1;
    static constexpr uint8_t codeOfWide = 62;
    static constexpr uint8_t codeOfTwoWords = 63;
    static constexpr uint8_t firstShort = 64;
    static constexpr uint32_t endOfShortWords = (256u - firstShort) << 16;

    void note(uint32_t word)
    {
        if (m_size < m_words.size())
            m_words[m_size++] = word;
    }

    uint32_t m_defaultWord;
    uint8_t m_size { 0 };
    std::array<uint32_t, codeOfWide - firstRepeated> m_words;
};

void writeFaultSites(std::span<CollectedSite> sites, const CompiledCode& code, uint32_t bodyStart)
{
    RELEASE_ASSERT(code.frame.hasFrame() || !code.frame.calleeSaveRegisters.registerCount());
    std::span frameRanges = code.frame.frameRanges.span();
    auto hasFrameAt = [&](uint32_t ascendingOffset) {
        while (!frameRanges.empty() && frameRanges.front().end <= ascendingOffset)
            frameRanges = frameRanges.subspan(1);
        return !frameRanges.empty() && frameRanges.front().begin <= ascendingOffset;
    };
    auto add = [&](uint32_t offsetInBody, bool isFrameless, bool isLoopPoll, std::span<const uint32_t> words) {
        uint64_t offset = static_cast<uint64_t>(bodyStart) + offsetInBody;
        RELEASE_ASSERT(offset <= std::numeric_limits<uint32_t>::max() && words.size() <= FaultSite::maxThrowSiteWords);
        sites.front() = CollectedSite { static_cast<uint32_t>(offset), isFrameless, isLoopPoll, static_cast<uint8_t>(words.size()), 0, { } };
        std::ranges::copy(words, sites.front().words.begin());
        sites = sites.subspan(1);
    };
    std::span accesses = code.sites.faultingAccesses.span();
    std::span polls = code.sites.rareLists->pollSites.span();
    while (!accesses.empty() || !polls.empty()) {
        if (polls.empty() || (!accesses.empty() && accesses.front().codeOffset < polls.front().offset)) {
            const FaultingAccess& access = accesses.front();
            RELEASE_ASSERT(!access.throwSite.isEmpty());
            add(access.codeOffset, !hasFrameAt(access.codeOffset), false, access.throwSite.span());
            accesses = accesses.subspan(1);
            continue;
        }
        const PollSite& poll = polls.front();
        RELEASE_ASSERT(poll.callerIsFrameless == !hasFrameAt(poll.offset));
        add(poll.offset, poll.callerIsFrameless, true, { });
        polls = polls.subspan(1);
    }
    RELEASE_ASSERT(sites.empty());
}

uint32_t findMostFrequentWord(const ParallelLoops& loops, Vector<uint32_t>&& words)
{
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, words.mutableSpan(), [](uint32_t word) {
        return word;
    });
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

constexpr size_t maxSizeOfSite = FaultStep::maxSize + sizeOfVarint(uint64_t(UINT32_MAX) << 1) + LaterWordsOfBlock::maxSize;

struct StepSymbols {
    friend void appendVarint(StepSymbols& step, uint64_t value) { step.symbols.appendVarint(StepStart, StepContinuation, value); }

    PrefixSymbolWriter& symbols;
};

template<typename NextByte>
void readSiteBytes(const NextByte& nextByte)
{
    uint8_t byte = nextByte(StepStart);
    bool isLoopPoll = byte & 2;
    while (byte & 0x80)
        byte = nextByte(StepContinuation);
    if (isLoopPoll)
        return;
    byte = nextByte(NumberStart);
    bool hasLaterWords = byte & 1;
    while (byte & 0x80)
        byte = nextByte(NumberContinuation);
    if (!hasLaterWords)
        return;
    uint8_t code = nextByte(LaterWordsCode);
    if (LaterWordsOfBlock::isShort(code)) {
        nextByte(ShortWordLow);
        nextByte(ShortWordHigh);
        return;
    }
    for (unsigned left = LaterWordsOfBlock::numberOfVarints(code); left; --left) {
        do {
            byte = nextByte(LaterWordsVarint);
        } while (byte & 0x80);
    }
}

Vector<uint8_t> encodeFaultTable(std::span<const CollectedSite> sites, const ParallelLoops& loops)
{
    auto numberOfBlocks = static_cast<unsigned>((sites.size() + faultSitesBetweenCheckpoints - 1) / faultSitesBetweenCheckpoints);
    auto sitesOf = [&](size_t block) {
        size_t first = block * faultSitesBetweenCheckpoints;
        return sites.subspan(first, std::min<size_t>(faultSitesBetweenCheckpoints, sites.size() - first));
    };

    Vector<uint32_t> everyFirstWord = collectInParallel<uint32_t>(loops, numberOfBlocks, [&](unsigned block, Vector<uint32_t>& words) {
        for (const CollectedSite& site : sitesOf(block)) {
            if (site.isNullCheck()) {
                RELEASE_ASSERT(site.numberOfWords);
                words.append(site.words[0]);
            }
        }
    });
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, everyFirstWord.mutableSpan(), [](uint32_t word) {
        return word;
    });
    Vector<uint32_t> firstWords;
    for (uint32_t word : everyFirstWord) {
        if (firstWords.isEmpty() || firstWords.last() != word)
            firstWords.append(word);
    }
    for (size_t i = 0; i + 1 < firstWords.size(); ++i) {
        ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(firstWords[i]);
        if (word.isCallSite() && !word.registerNumber && word.differsOnlyInRegisterNumber(std::bit_cast<ThrowSiteWord>(firstWords[i + 1])))
            firstWords[i] = firstWords[i + 1];
    }
    removeRepeatedElements(firstWords);

    Vector<uint32_t> usesOfFirstWord(FillWith { }, firstWords.size(), 0u);
    size_t indexOfFirstWord = 0;
    for (uint32_t word : everyFirstWord) {
        while (firstWords[indexOfFirstWord] < word)
            ++indexOfFirstWord;
        ++usesOfFirstWord[indexOfFirstWord];
    }
    Vector<uint32_t> mostUsedFirst(firstWords.size());
    for (size_t index = 0; index < firstWords.size(); ++index)
        mostUsedFirst[index] = static_cast<uint32_t>(index);
    AOT::sort(mostUsedFirst, [&](uint32_t a, uint32_t b) {
        return usesOfFirstWord[a] != usesOfFirstWord[b] ? usesOfFirstWord[a] > usesOfFirstWord[b] : a < b;
    });
    Vector<uint32_t> numberOfFirstWord(firstWords.size());
    for (size_t number = 0; number < mostUsedFirst.size(); ++number)
        numberOfFirstWord[mostUsedFirst[number]] = static_cast<uint32_t>(number);

    uint32_t defaultSecondWord = findMostFrequentWord(loops, collectInParallel<uint32_t>(loops, sites.size(), [&](unsigned site, Vector<uint32_t>& words) {
        if (sites[site].numberOfWords > 1)
            words.append(sites[site].words[1]);
    }));

    Vector<Vector<PrefixSymbol>> symbolsOfBlocks(numberOfBlocks);
    Vector<FaultCheckpoint> checkpoints(numberOfBlocks);
    forEachIndexInParallel(loops, numberOfBlocks, [&](unsigned block) {
        uint32_t previousOffset = block ? sites[block * faultSitesBetweenCheckpoints - 1].offset : 0;
        checkpoints[block] = { previousOffset, 0 };
        symbolsOfBlocks[block].grow(sitesOf(block).size() * maxSizeOfSite);
        PrefixSymbol* firstSymbol = symbolsOfBlocks[block].mutableSpan().data();
        PrefixSymbolWriter symbols(firstSymbol);
        StepSymbols stepSymbols { symbols };
        LaterWordsOfBlock laterWords(defaultSecondWord);
        for (const CollectedSite& site : sitesOf(block)) {
            RELEASE_ASSERT(site.offset > previousOffset);
            site.stepFrom(previousOffset).append(stepSymbols);
            previousOffset = site.offset;
            if (!site.isNullCheck())
                continue;
            size_t index = std::ranges::lower_bound(firstWords, site.words[0]) - firstWords.begin();
            bool hasLaterWords = site.numberOfWords > 1;
            symbols.appendVarint(NumberStart, NumberContinuation, static_cast<uint64_t>(numberOfFirstWord[index]) << 1 | hasLaterWords);
            if (hasLaterWords)
                laterWords.append(symbols, std::span<const uint32_t>(site.words).subspan(1, site.numberOfWords - 1));
        }
        symbolsOfBlocks[block].shrink(symbols.end() - firstSymbol);
    });

    FaultSiteCodes::Counts counts { };
    for (const Vector<PrefixSymbol>& symbols : symbolsOfBlocks)
        FaultSiteCodes::count(counts, symbols.span());
    FaultSiteCodes codes(counts);
    Vector<Vector<uint8_t>> steps(numberOfBlocks);
    forEachIndexInParallel(loops, numberOfBlocks, [&](unsigned block) {
        std::span symbols = symbolsOfBlocks[block].span();
        steps[block].grow(symbols.size() * PrefixCode::maxLength / 8 + 1);
        uint8_t* firstByte = steps[block].mutableSpan().data();
        steps[block].shrink(codes.write(firstByte, symbols) - firstByte);
    });

    size_t sizeOfSteps = 0;
    for (unsigned block = 0; block < numberOfBlocks; ++block) {
        checkpoints[block].stepsPosition = static_cast<uint32_t>(sizeOfSteps);
        sizeOfSteps += steps[block].size();
    }

    FaultTableHeader header {
        .numberOfSites = static_cast<uint32_t>(sites.size()),
        .numberOfFirstWords = static_cast<uint32_t>(firstWords.size()),
        .defaultSecondWord = defaultSecondWord,
        .sizeOfCodes = static_cast<uint32_t>(codes.bytes().size()),
    };
    FaultTableLayout layout(header);

    Vector<uint8_t> bytes;
    bytes.grow(layout.sizeWithSteps(sizeOfSteps));
    std::span<uint8_t> section = bytes.mutableSpan();
    zeroSpan(section.subspan(layout.stepsOffset + sizeOfSteps));
    memcpySpan(section, asByteSpan(header));
    memcpySpan(section.subspan(sizeof(FaultTableHeader)), asBytes(checkpoints.span()));
    auto* firstWordsByNumber = reinterpret_cast<uint32_t*>(section.data() + layout.firstWordsOffset);
    for (size_t number = 0; number < mostUsedFirst.size(); ++number)
        firstWordsByNumber[number] = firstWords[mostUsedFirst[number]];
    memcpySpan(section.subspan(layout.codesOffset), codes.bytes().span());
    forEachIndexInParallel(loops, numberOfBlocks, [&](unsigned block) {
        memcpySpan(section.subspan(layout.stepsOffset + checkpoints[block].stepsPosition), steps[block].span());
    });
    return bytes;
}

struct Fault {
    const Image& image;
    uintptr_t pc;
    uintptr_t address;
    CallerFrame callerFrame;
};

struct FaultTableView {
    explicit FaultTableView(std::span<const uint8_t> bytes)
        : header(*reinterpret_cast<const FaultTableHeader*>(bytes.data()))
        , layout(header)
        , checkpoints(reinterpret_cast<const FaultCheckpoint*>(bytes.data() + sizeof(FaultTableHeader)), layout.numberOfCheckpoints)
        , firstWords(reinterpret_cast<const uint32_t*>(bytes.data() + layout.firstWordsOffset), header.numberOfFirstWords)
        , codes(bytes.data() + layout.codesOffset)
        , steps(bytes.subspan(layout.stepsOffset))
    {
    }

    std::array<uint32_t, FaultSite::maxThrowSiteWords> readThrowSite(const uint8_t*& cursor, LaterWordsOfBlock& laterWords) const
    {
        uint64_t number = readVarint(cursor);
        std::array<uint32_t, 2> later { header.defaultSecondWord, 0 };
        if (number & 1)
            later = laterWords.read(cursor);
        return { firstWords[number >> 1], later[0], later[1] };
    }

    const FaultTableHeader& header;
    FaultTableLayout layout;
    std::span<const FaultCheckpoint> checkpoints;
    std::span<const uint32_t> firstWords;
    const uint8_t* codes;
    std::span<const uint8_t> steps;
};

} // anonymous namespace

Vector<uint8_t> encodeFaultSites(const LinkedFunctions& functions, std::span<const uint32_t> bodyStarts, BuildContext& context)
{
    RELEASE_ASSERT(bodyStarts.size() == functions.size());
    const ParallelLoops& loops = context.parallelLoops();
    auto numberOfSitesOf = [](const CompiledCode& code) {
        return code.sites.faultingAccesses.size() + code.sites.rareLists->pollSites.size();
    };
    size_t numberOfRanges = numberOfRangesOfIndices(functions.size());
    Vector<uint64_t> firstSiteOfRange(FillWith { }, numberOfRanges + 1, 0);
    Vector<uint64_t> numberOfNullChecksInRange(FillWith { }, numberOfRanges, 0);
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        uint64_t numberOfSitesInRange = 0;
        uint64_t numberOfNullChecks = 0;
        for (size_t position = first; position < end; ++position) {
            numberOfSitesInRange += numberOfSitesOf(functions[position]);
            numberOfNullChecks += functions[position].sites.faultingAccesses.size();
        }
        size_t range = first / indicesInRangeOfLoop;
        firstSiteOfRange[range + 1] = numberOfSitesInRange;
        numberOfNullChecksInRange[range] = numberOfNullChecks;
    });
    uint64_t numberOfNullChecks = 0;
    for (size_t range = 0; range < numberOfRanges; ++range) {
        firstSiteOfRange[range + 1] += firstSiteOfRange[range];
        numberOfNullChecks += numberOfNullChecksInRange[range];
    }
    uint64_t numberOfSites = firstSiteOfRange.last();
    if (!numberOfSites)
        return { };

    uint64_t maxSectionSize = FaultTableLayout(numberOfSites, numberOfNullChecks, FaultSiteCodes::maxSize).sizeWithSteps(numberOfSites * maxSizeOfSite * sizeof(PrefixSymbol));
    if (!checkSectionSize(std::max<uint64_t>(maxSectionSize, numberOfSites * sizeof(CollectedSite)), context))
        return { };

    Vector<CollectedSite> sites;
    sites.grow(numberOfSites);
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        size_t firstSite = firstSiteOfRange[first / indicesInRangeOfLoop];
        for (size_t position = first; position < end; ++position) {
            size_t numberOfSitesOfBody = numberOfSitesOf(functions[position]);
            writeFaultSites(sites.mutableSpan().subspan(firstSite, numberOfSitesOfBody), functions[position], bodyStarts[position]);
            firstSite += numberOfSitesOfBody;
        }
    });
    return encodeFaultTable(sites.span(), loops);
}

std::optional<FaultSite> Image::faultSiteAt(const void* pc) const
{
    std::span bytes = section<uint8_t>(ImageSection::FaultSites);
    if (bytes.empty())
        return std::nullopt;
    uintptr_t offset = std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(code().data());
    if (offset >= header().code.functionsEnd)
        return std::nullopt;

    FaultTableView table(bytes);
    auto nextCheckpoint = std::ranges::lower_bound(table.checkpoints, static_cast<uint32_t>(offset), { }, &FaultCheckpoint::previousSiteOffset);
    if (nextCheckpoint == table.checkpoints.begin())
        return std::nullopt;
    auto checkpoint = std::prev(nextCheckpoint);

    uint32_t firstSite = static_cast<uint32_t>(checkpoint - table.checkpoints.begin()) * faultSitesBetweenCheckpoints;
    uint32_t numberOfSteps = std::min(faultSitesBetweenCheckpoints, table.header.numberOfSites - firstSite);
    PrefixCodeReader reader(table.steps.data() + checkpoint->stepsPosition, table.steps.data() + table.steps.size());
    std::array<uint8_t, maxSizeOfSite> siteBytes;
    uint64_t currentOffset = checkpoint->previousSiteOffset;
    LaterWordsOfBlock laterWords(table.header.defaultSecondWord);
    for (uint32_t i = 0; i < numberOfSteps; ++i) {
        size_t size = 0;
        readSiteBytes([&](unsigned code) {
            RELEASE_ASSERT(size < siteBytes.size());
            return siteBytes[size++] = static_cast<uint8_t>(reader.read(FaultSiteCodes::code(table.codes, code)));
        });
        const uint8_t* cursor = siteBytes.data();
        FaultStep step = FaultStep::read(cursor);
        currentOffset += step.distance;
        if (currentOffset > offset)
            return std::nullopt;
        bool isFound = currentOffset == offset;
        bool hasFrame = !step.isFrameless;
        if (step.isLoopPoll) {
            if (isFound)
                return FaultSite { FaultSiteKind::LoopPoll, hasFrame, { } };
            continue;
        }
        std::array throwSite = table.readThrowSite(cursor, laterWords);
        if (isFound)
            return FaultSite { FaultSiteKind::NullCheck, hasFrame, throwSite };
    }
    return std::nullopt;
}

#if CPU(ARM64)
static uintptr_t& registerOf(PlatformRegisters& registers, GPRReg reg)
{
    RELEASE_ASSERT(reg == faultCallerLinkGPR || reg == ARM64Registers::lr || reg == ARM64Registers::fp || reg == ARM64Registers::sp || reg == cellCageBaseGPR || reg == instanceGPR);
    unsigned number = static_cast<unsigned>(reg);
#if OS(DARWIN)
    if (reg == ARM64Registers::sp)
        return reinterpret_cast<uintptr_t&>(registers.__sp);
    if (reg == ARM64Registers::fp)
        return reinterpret_cast<uintptr_t&>(registers.__fp);
    return reg == ARM64Registers::lr ? reinterpret_cast<uintptr_t&>(registers.__lr) : reinterpret_cast<uintptr_t&>(registers.__x[number]);
#elif OS(WINDOWS)
    return reinterpret_cast<uintptr_t&>(reg == ARM64Registers::sp ? registers.Sp : registers.X[number]);
#elif OS(FREEBSD)
    if (reg == ARM64Registers::sp)
        return reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_sp);
    return reg == ARM64Registers::lr ? reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_lr) : reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_x[number]);
#else
    return reinterpret_cast<uintptr_t&>(reg == ARM64Registers::sp ? registers.machineContext.sp : registers.machineContext.regs[number]);
#endif
}
#elif CPU(X86_64)
static uintptr_t& registerOf(PlatformRegisters& registers, GPRReg reg)
{
    static_assert(MacroAssembler::s_scratchRegister == X86Registers::r11 && registerWrittenAtFaults == X86Registers::r11 && instanceGPR == X86Registers::r13 && cellCageBaseGPR == X86Registers::r14);
    RELEASE_ASSERT(reg == registerWrittenAtFaults || reg == instanceGPR || reg == cellCageBaseGPR);
#if OS(DARWIN)
    return reinterpret_cast<uintptr_t&>(reg == X86Registers::r11 ? registers.__r11 : reg == X86Registers::r13 ? registers.__r13 : registers.__r14);
#elif OS(WINDOWS)
    return reinterpret_cast<uintptr_t&>(reg == X86Registers::r11 ? registers.R11 : reg == X86Registers::r13 ? registers.R13 : registers.R14);
#elif OS(FREEBSD)
    return reinterpret_cast<uintptr_t&>(reg == X86Registers::r11 ? registers.machineContext.mc_r11 : reg == X86Registers::r13 ? registers.machineContext.mc_r13 : registers.machineContext.mc_r14);
#else
    return reinterpret_cast<uintptr_t&>(registers.machineContext.gregs[reg == X86Registers::r11 ? REG_R11 : reg == X86Registers::r13 ? REG_R13 : REG_R14]);
#endif
}
#endif

static void* findStub(const Image& image, StubKey key)
{
    if (!exists(key))
        return nullptr;
    return const_cast<void*>(image.stubAddress(key));
}

static bool handleNullCheck(const Fault& fault, PlatformRegisters& registers)
{
    if (fault.address >= FaultSite::reach && fault.address - registerOf(registers, cellCageBaseGPR) >= FaultSite::reach)
        return false;
    void* target = findStub(fault.image, throwAtSite(ThrowSiteSource::FaultSite, fault.callerFrame));
    if (!target)
        return false;

    uintptr_t returnAddress = fault.pc + sizeOfNearCall;
    uint64_t& registerBeforeFault = std::bit_cast<Instance*>(registerOf(registers, instanceGPR))->state->registerBeforeFault;
#if CPU(ARM64)
    registerBeforeFault = registerOf(registers, faultCallerLinkGPR);
    if (fault.callerFrame == CallerFrame::Frameless)
        registerOf(registers, faultCallerLinkGPR) = registerOf(registers, ARM64Registers::lr);
    registerOf(registers, ARM64Registers::lr) = returnAddress;
#elif CPU(X86_64)
    registerBeforeFault = registerOf(registers, registerWrittenAtFaults);
    registerOf(registers, registerWrittenAtFaults) = returnAddress;
#endif
    MachineContext::setInstructionPointer(registers, target);
    return true;
}

#if AOT_LOOP_POLLS_FAULT
static bool handleLoopPoll(const Fault& fault, PlatformRegisters& registers)
{
    bool isFrameless = fault.callerFrame == CallerFrame::Frameless;
    uintptr_t instance = registerOf(registers, instanceGPR);
    uintptr_t address = instance + InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0);
    bool leavesOwedError = fault.address == address + offsetOfPollLeavingOwedErrorInLoopPollPage;
    if (fault.address != address && !leavesOwedError)
        return false;
    void* target = findStub(fault.image, pollAtLoop(fault.callerFrame));
    if (!target)
        return false;

    if (leavesOwedError)
        std::bit_cast<Instance*>(instance)->runtime.exceptions.pollLeavesOwedError = 1;

    uintptr_t returnAddress = fault.pc + sizeOfLoopPoll;
#if CPU(ARM64)
    uintptr_t& link = registerOf(registers, ARM64Registers::lr);
    if (isFrameless) {
        uintptr_t& stackPointer = registerOf(registers, ARM64Registers::sp);
        uintptr_t& framePointer = registerOf(registers, ARM64Registers::fp);
        auto* top = std::bit_cast<uintptr_t*>(stackPointer);
        *--top = link;
        *--top = framePointer;
        framePointer = stackPointer = std::bit_cast<uintptr_t>(top);
    }
    link = returnAddress;
#else
    void*& stackPointer = MachineContext::stackPointerImpl(registers);
    auto* top = static_cast<uintptr_t*>(stackPointer);
    if (isFrameless) {
        void*& framePointer = MachineContext::framePointerImpl(registers);
        *--top = std::bit_cast<uintptr_t>(framePointer);
        framePointer = top;
    }
    *--top = returnAddress;
    stackPointer = top;
#endif
    MachineContext::setInstructionPointer(registers, target);
    return true;
}
#endif

bool handleAccessFault(void* faultingAddress, PlatformRegisters& registers)
{
    auto instructionPointer = MachineContext::instructionPointer(registers);
    if (!instructionPointer)
        return false;
    if (!Image::isRegistered())
        return false;
    void* pc = instructionPointer->untaggedPtr();
    const Image* image = Image::findByCodeAddress(pc);
    if (!image)
        return false;
    std::optional<FaultSite> site = image->faultSiteAt(pc);
    if (!site)
        return false;

    Fault fault {
        .image = *image,
        .pc = std::bit_cast<uintptr_t>(pc),
        .address = std::bit_cast<uintptr_t>(faultingAddress),
        .callerFrame = site->hasFrame ? CallerFrame::HasFrame : CallerFrame::Frameless,
    };
    switch (site->kind) {
    case FaultSiteKind::NullCheck:
        return handleNullCheck(fault, registers);
    case FaultSiteKind::LoopPoll:
#if AOT_LOOP_POLLS_FAULT
        return handleLoopPoll(fault, registers);
#else
        return false;
#endif
    }
    return false;
}

void installFaultHandler()
{
    addSignalHandler(Signal::AccessFault, [](Signal, SigInfo& info, PlatformRegisters& registers) {
        return !info.isSentByProcess && handleAccessFault(info.faultingAddress, registers) ? SignalAction::Handled : SignalAction::NotHandled;
    });
}

#if OS(UNIX)
namespace {

struct SignalStackOfThread {
    static constexpr size_t size = 512 * KB;

    ~SignalStackOfThread()
    {
        if (!memory)
            return;
        stack_t none { };
        none.ss_flags = SS_DISABLE;
        sigaltstack(&none, nullptr);
        munmap(memory, size);
    }

    void map()
    {
        void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED)
            return;
        stack_t stack { };
        stack.ss_sp = mapped;
        stack.ss_size = size;
        if (sigaltstack(&stack, nullptr)) {
            munmap(mapped, size);
            return;
        }
        memory = mapped;
    }

    void* memory { nullptr };
};

} // anonymous namespace

static bool isCodeOfThisProgram(const void* address)
{
#if OS(DARWIN)
    Dl_info theirs { };
    Dl_info ours { };
    return dladdr(address, &theirs) && dladdr(reinterpret_cast<const void*>(&takeFaultsOfThisThreadOnAnotherStack), &ours) && theirs.dli_fbase == ours.dli_fbase;
#else
    return address >= &__executable_start && address < &etext;
#endif
}

void takeFaultsOfThisThreadOnAnotherStack()
{
    stack_t current { };
    if (!isMainThread() && !sigaltstack(nullptr, &current) && (current.ss_flags & SS_DISABLE)) {
        WTF_THREAD_LOCAL_ALWAYS_DESTROY static thread_local SignalStackOfThread stack;
        stack.map();
    }
    static std::atomic<bool> areActionsChanged { false };
    if (areActionsChanged.exchange(true, std::memory_order_relaxed))
        return;
#if OS(DARWIN)
    constexpr std::array signalsOfGuardPage { SIGSEGV, SIGBUS };
#else
    constexpr std::array signalsOfGuardPage { SIGSEGV };
#endif
    for (int signal : signalsOfGuardPage) {
        struct sigaction action;
        if (sigaction(signal, nullptr, &action) || !(action.sa_flags & SA_SIGINFO) || (action.sa_flags & SA_ONSTACK))
            continue;
        if (!isCodeOfThisProgram(reinterpret_cast<const void*>(action.sa_sigaction)))
            continue;
        action.sa_flags |= SA_ONSTACK;
        sigaction(signal, &action, nullptr);
    }
}
#else
void takeFaultsOfThisThreadOnAnotherStack() { }
#endif

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
