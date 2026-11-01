/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFaultTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCheckedFrameCells.h"
#include "AOTCodeSites.h"
#include "AOTFaultHandler.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTOperationData.h"
#include "AOTSectionWriter.h"
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
#include <bit>
#include <functional>
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/threads/Signals.h>

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
    bool isNullCheck() const { return !isLoopPoll; }
    FaultStep stepFrom(uint32_t previousOffset) const { return { .isFrameless = isFrameless, .isLoopPoll = isLoopPoll, .distance = offset - previousOffset }; }

    uint32_t offset;
    bool isFrameless;
    bool isLoopPoll;
    uint8_t numberOfWords;
    std::array<uint32_t, FaultSite::maxThrowSiteWords> words;
};

void addFaultSites(Vector<CollectedSite>& sites, const CompiledCode& code, uint32_t bodyStart)
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
        sites.append(CollectedSite { static_cast<uint32_t>(offset), isFrameless, isLoopPoll, static_cast<uint8_t>(words.size()), { } });
        std::ranges::copy(words, sites.last().words.begin());
    };
    std::span accesses = code.sites.faultingAccesses.span();
    std::span polls = code.sites.pollSites.span();
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
}

uint32_t findMostFrequentWord(const ParallelLoops& loops, Vector<uint32_t>&& words)
{
    parallelSort(loops, words.mutableSpan(), std::less<uint32_t> { });
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

Vector<uint8_t> encodeFaultTable(std::span<const CollectedSite> sites, const ParallelLoops& loops)
{
    auto numberOfBlocks = static_cast<unsigned>((sites.size() + faultSitesBetweenCheckpoints - 1) / faultSitesBetweenCheckpoints);
    auto sitesOf = [&](size_t block) {
        size_t first = block * faultSitesBetweenCheckpoints;
        return sites.subspan(first, std::min<size_t>(faultSitesBetweenCheckpoints, sites.size() - first));
    };

    Vector<Vector<uint8_t>> steps(numberOfBlocks);
    Vector<FaultCheckpoint> checkpoints(numberOfBlocks);
    forEachIndexInParallel(loops, numberOfBlocks, [&](unsigned block) {
        uint32_t previousOffset = block ? sites[block * faultSitesBetweenCheckpoints - 1].offset : 0;
        uint32_t offsetBeforeBlock = previousOffset;
        uint32_t nullChecksInBlock = 0;
        steps[block].reserveInitialCapacity(sitesOf(block).size() * FaultStep::maxSize);
        for (const CollectedSite& site : sitesOf(block)) {
            RELEASE_ASSERT(site.offset > previousOffset);
            site.stepFrom(previousOffset).append(steps[block]);
            previousOffset = site.offset;
            nullChecksInBlock += site.isNullCheck();
        }
        // Until the sums are made, firstNullCheck counts the null checks of the block.
        checkpoints[block] = { offsetBeforeBlock, 0, nullChecksInBlock };
    });
    size_t sizeOfSteps = 0;
    uint32_t numberOfNullChecks = 0;
    for (unsigned block = 0; block < numberOfBlocks; ++block) {
        checkpoints[block].stepsPosition = static_cast<uint32_t>(sizeOfSteps);
        sizeOfSteps += steps[block].size();
        uint32_t nullChecksInBlock = std::exchange(checkpoints[block].firstNullCheck, numberOfNullChecks);
        numberOfNullChecks += nullChecksInBlock;
    }

    auto collectWords = [&](unsigned index) {
        return collectInParallel<uint32_t>(loops, sites.size(), [&](unsigned site, Vector<uint32_t>& words) {
            if (index < sites[site].numberOfWords)
                words.append(sites[site].words[index]);
        });
    };
    auto collectSparseWords = [&](unsigned index, std::optional<uint32_t> omittedWord) {
        return collectInParallel<FaultSiteWord>(loops, numberOfBlocks, [&](unsigned block, Vector<FaultSiteWord>& words) {
            uint32_t nullCheck = checkpoints[block].firstNullCheck;
            for (const CollectedSite& site : sitesOf(block)) {
                if (index < site.numberOfWords && site.words[index] != omittedWord)
                    words.append(FaultSiteWord { nullCheck, site.words[index] });
                nullCheck += site.isNullCheck();
            }
        });
    };
    Vector<uint32_t> firstWords = collectWords(0);
    sortAndRemoveDuplicates(loops, firstWords);
    // Nobody reads the registerNumber of a call site that leaves it zero. The site takes the next word where that differs in nothing else.
    for (size_t i = 0; i + 1 < firstWords.size(); ++i) {
        ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(firstWords[i]);
        if (word.isCallSite() && !word.registerNumber && word.differsOnlyInRegisterNumber(std::bit_cast<ThrowSiteWord>(firstWords[i + 1])))
            firstWords[i] = firstWords[i + 1];
    }
    removeRepeatedElements(firstWords);
    uint32_t defaultSecondWord = findMostFrequentWord(loops, collectWords(1));
    Vector<FaultSiteWord> secondWords = collectSparseWords(1, defaultSecondWord);
    Vector<FaultSiteWord> thirdWords = collectSparseWords(2, std::nullopt);

    FaultTableHeader header {
        .numberOfSites = static_cast<uint32_t>(sites.size()),
        .numberOfNullChecks = numberOfNullChecks,
        .numberOfFirstWords = static_cast<uint32_t>(firstWords.size()),
        .numberOfSecondWords = static_cast<uint32_t>(secondWords.size()),
        .defaultSecondWord = defaultSecondWord,
        .numberOfThirdWords = static_cast<uint32_t>(thirdWords.size()),
    };
    FaultTableLayout layout(header);

    Vector<uint8_t> bytes(FillWith { }, layout.sizeWithSteps(sizeOfSteps), 0);
    std::span<uint8_t> section = bytes.mutableSpan();
    memcpySpan(section, asByteSpan(header));
    memcpySpan(section.subspan(sizeof(FaultTableHeader)), asBytes(checkpoints.span()));
    memcpySpan(section.subspan(layout.firstWordsOffset), asBytes(firstWords.span()));
    memcpySpan(section.subspan(layout.secondWordsOffset), asBytes(secondWords.span()));
    memcpySpan(section.subspan(layout.thirdWordsOffset), asBytes(thirdWords.span()));
    forEachIndexInParallel(loops, numberOfBlocks, [&](unsigned block) {
        memcpySpan(section.subspan(layout.stepsOffset + checkpoints[block].stepsPosition), steps[block].span());
        uint8_t* wordNumbers = section.data() + layout.wordNumbersOffset;
        uint32_t nullCheck = checkpoints[block].firstNullCheck;
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
    uint64_t maxSectionSize = FaultTableLayout(numberOfSites, numberOfNullChecks, numberOfNullChecks, numberOfNullChecks, numberOfNullChecks).sizeWithSteps(numberOfSites * FaultStep::maxSize);
    if (!checkSectionSize(std::max<uint64_t>(maxSectionSize, numberOfSites * sizeof(CollectedSite)), context))
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<CollectedSite> sites = collectInParallel<CollectedSite>(loops, functions.size(), [&](unsigned position, Vector<CollectedSite>& collected) {
        addFaultSites(collected, functions[position], bodyStarts[position]);
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
        FaultStep step = FaultStep::read(cursor);
        currentOffset += step.distance;
        if (currentOffset < offset) {
            nullCheck += !step.isLoopPoll;
            continue;
        }
        if (currentOffset > offset)
            return std::nullopt;
        bool hasFrame = !step.isFrameless;
        if (step.isLoopPoll)
            return FaultSite { FaultSiteKind::LoopPoll, hasFrame, { } };
        return FaultSite { FaultSiteKind::NullCheck, hasFrame, table.throwSiteOf(nullCheck) };
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
    // A null 32-bit reference decodes to the base of the cell cage.
    if (fault.address >= FaultSite::reach && fault.address - registerOf(registers, cellCageBaseGPR) >= FaultSite::reach)
        return false;
    void* target = findStub(fault.image, throwAtSite(ThrowSiteSource::FaultSite, fault.callerFrame));
    if (!target)
        return false;

    // The call-site table and ThrowAtSite treat the faulting instruction as a near call, whatever its length.
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

    // The stub is entered as if the load had called it, from a function with a frame record.
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
    if (CheckedFrameCells::isInRange(faultingAddress) && CheckedFrameCells::isInUse()) [[unlikely]] {
        CheckedFrameCells::report(faultingAddress);
        return false;
    }
    auto instructionPointer = MachineContext::instructionPointer(registers);
    if (!instructionPointer)
        return false;
    if (!Image::isRegistered())
        return false;
    // This may run on another thread than the code it is about, so the registers say which image that code belongs to.
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

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
