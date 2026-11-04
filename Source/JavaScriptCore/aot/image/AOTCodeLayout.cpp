/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTSort.h"
#include "AOTUnitPlacement.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT {

// What the instruction reaches: 128 MB on ARM64, 2 GB on x86-64.
static constexpr int64_t reachOfNearCall = MacroAssembler::relocationReach(RelocationKind::Call);
static_assert(MacroAssembler::relocationReach(RelocationKind::Jump) == reachOfNearCall);

// No copy of the shared code can lie between bodies that fall into each other, and each of them has to reach one.
static constexpr size_t maxSizeOfRunOfBodies = 1 * MB;
static_assert(static_cast<int64_t>(maxSizeOfRunOfBodies) * 8 <= reachOfNearCall);

// Image::locate() runs where the cache of call sites has no entry. It walks the starts of functions from the first one in the granule, and sixteen of them lie in a cache line.
unsigned chooseCodeGranuleShift(const LinkedFunctions& functions, BuildContext& context)
{
    constexpr size_t maxStartsPassedOnAverage = 8;
    Vector<size_t> functionEnds(functions.size());
    Vector<uint8_t> isReturnedInto(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned index) {
        auto& body = functions[index].body;
        functionEnds[index] = body.bytes.size();
        isReturnedInto[index] = !body.bytes.isEmpty() && !body.returnSites.isEmpty();
    });
    for (size_t index = 1; index < functionEnds.size(); ++index)
        functionEnds[index] += WTF::roundUpToMultipleOf<imageFunctionAlignment>(functionEnds[index - 1]);
    auto walkIsShort = [&](unsigned shift) {
        size_t startsPassed = 0;
        size_t lookups = 0;
        size_t granuleStart = 0;
        for (size_t index = 0; index < functionEnds.size(); ++index) {
            if (!isReturnedInto[index])
                continue;
            size_t startOfGranule = (functionEnds[index] - 1) >> shift << shift;
            while (functionEnds[granuleStart] <= startOfGranule)
                ++granuleStart;
            startsPassed += index - granuleStart;
            ++lookups;
        }
        return startsPassed <= maxStartsPassedOnAverage * lookups;
    };
    unsigned codeGranuleShift = maxCodeGranuleShift;
    while (codeGranuleShift > minCodeGranuleShift && !walkIsShort(codeGranuleShift))
        --codeGranuleShift;
    return codeGranuleShift;
}

static bool endsInJumpToNextBody(const LinkedFunctions& functions, const ResolvedCallees& callees, uint32_t position)
{
    const CodeBody& body = functions[position].body;
    if (body.relocations.isEmpty())
        return false;
    const Relocation& jump = body.relocations.last();
    if (jump.kind != RelocationKind::Jump || jump.space != SymbolSpace::Function || jump.entry != primaryEntry || jump.offset + sizeOfJump != body.bytes.size())
        return false;
    return jump.symbol < body.callees.size() && callees.at(position, jump.symbol) == OptionalIndex { position + 1 }
        && !functions[position + 1].body.primaryEntryOffset();
}

static std::optional<size_t> computeNopSize(const CompiledCode& function)
{
    // The layout aligns the primary entry, so the first byte of the body lies this far past a multiple of the alignment.
    size_t startMisalignment = (imageFunctionAlignment - function.body.primaryEntryOffset() % imageFunctionAlignment) % imageFunctionAlignment;
    auto roundUpToAlignedEnd = [&](size_t size) {
        return WTF::roundUpToMultipleOf<imageFunctionAlignment>(size + startMisalignment) - startMisalignment;
    };
    // Image::locate would attribute a return address at the very end of a body to the next body.
    size_t offset = function.body.relocations.last().offset;
    std::optional<uint32_t> lastReturnSite = endOfLastReturnSite(function);
    size_t newSize = std::max(roundUpToAlignedEnd(offset), lastReturnSite ? roundUpToAlignedEnd(static_cast<size_t>(*lastReturnSite) + 1) : 0);
    if (!newSize || newSize - offset >= sizeOfJump)
        return std::nullopt;
    return newSize - offset;
}

Vector<FinalJump> findFinalJumps(const LinkedFunctions& functions, const ResolvedCallees& callees, BuildContext& context)
{
    Vector<FinalJump> finalJumps(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& function = functions[position];
        finalJumps[position].sizeWithoutJump = safeCast<uint32_t>(function.body.bytes.size());
        if (position + 1 == functions.size() || !endsInJumpToNextBody(functions, callees, position))
            return;
        if (std::optional<size_t> nopSize = computeNopSize(function))
            finalJumps[position] = { safeCast<uint32_t>(function.body.relocations.last().offset + *nopSize), true };
    });
    return finalJumps;
}

Vector<FarInstanceWords> findFarInstanceWords(const LinkedFunctions& functions, const ImageNumbers& numbers, BuildContext& context)
{
    if (!isARM64())
        return { };

    Vector<FarInstanceWords> wordsByBody(functions.size());
    std::atomic<bool> hasFarWords { false };
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CodeBody& body = functions[position].body;
        FarInstanceWords& words = wordsByBody[position];
        Vector<uint32_t, 16> sites;
        for (const Relocation& relocation : body.relocations) {
            // A relocation that names nothing is the linker's to report.
            if (relocation.space != SymbolSpace::InstanceWord || relocation.symbol >= body.mentions.size())
                continue;
            std::optional<uint32_t> distance = numbers.valueOf(body.mentions[relocation.symbol], body.mentionWords.span(), true);
            if (!distance || !isFarInstanceWord(static_cast<int32_t>(*distance)))
                continue;
            words.distances.append(static_cast<int32_t>(*distance));
            sites.append(relocation.offset);
        }
        if (sites.isEmpty())
            return;
        AOT::sortInLine(words.distances);
        removeRepeatedElements(words.distances);
        for (uint32_t site : sites)
            words.numberOfDetours += words.takesDetour(body.bytes.size(), site);
        hasFarWords.store(true, std::memory_order_relaxed);
    });
    if (!hasFarWords.load(std::memory_order_relaxed))
        return { };
    return wordsByBody;
}

void removeFinalJumps(LinkedFunctions& functions, const BitVector& fallsIntoNext, std::span<const FinalJump> finalJumps, BuildContext& context)
{
    RELEASE_ASSERT(finalJumps.size() == functions.size());
    forEachRangeOfIndicesInParallel(context.parallelLoops(), functions.size(), [&](size_t first, size_t end) {
        uint64_t bytesRemoved = 0;
        for (size_t position = first; position < end; ++position) {
            CompiledCode& function = functions[position];
            CodeBody& body = function.body;
            if (fallsIntoNext.get(position)) {
                RELEASE_ASSERT(finalJumps[position].jumpsToNextBody);
                bytesRemoved += body.bytes.size() - finalJumps[position].sizeWithoutJump;
                size_t offset = body.relocations.takeLast().offset;
                body.bytes.shrink(offset);
                body.bytes.grow(finalJumps[position].sizeWithoutJump);
                fillWithNops(body.bytes.mutableSpan().subspan(offset));
            }
            std::optional<uint32_t> lastReturnSite = endOfLastReturnSite(function);
            RELEASE_ASSERT_WITH_MESSAGE(!lastReturnSite || *lastReturnSite < body.bytes.size(), "AOT: a function ends in a call: its return address would be taken for the next function's");
        }
        if (bytesRemoved)
            context.statistics().count(StatisticID::FinalJumpBytesRemoved, bytesRemoved);
    });
}

static std::array<Vector<size_t>, 2> collectSizesAndAlignedOffsets(const LinkedFunctions& functions, BuildContext& context)
{
    std::array<Vector<size_t>, 2> result { Vector<size_t>(functions.size()), Vector<size_t>(functions.size()) };
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CodeBody& body = functions[position].body;
        result[0][position] = body.bytes.size();
        result[1][position] = body.primaryEntryOffset();
    });
    return result;
}

static BitVector findFallThroughs(const CodeLayoutInputs& inputs, std::span<const size_t> bodySizes)
{
    BitVector fallsIntoNext(bodySizes.size());
    for (size_t index = 0, sizeOfRun = 0; index + 1 < bodySizes.size(); ++index) {
        sizeOfRun += bodySizes[index];
        if (!inputs.finalJumps[index].jumpsToNextBody || inputs.sizeOfFarInstanceWords(index) || sizeOfRun > maxSizeOfRunOfBodies) {
            sizeOfRun = 0;
            continue;
        }
        fallsIntoNext.quickSet(index);
    }
    return fallsIntoNext;
}

static UnitPlacement placeBodies(std::span<const size_t> unitSizes, std::span<const size_t> alignedOffsets, const BitVector& fallsIntoNext, size_t sharedCodeSize, unsigned codeGranuleShift, const ParallelLoops& loops)
{
    static_assert(!((1 << minCodeGranuleShift) % imageFunctionAlignment) && !(sizeof(uint32_t) % imageFunctionAlignment));
    const size_t numberOfBodies = unitSizes.size();
    const size_t granuleSize = static_cast<size_t>(1) << codeGranuleShift;

    Vector<CopyableUse> uses(numberOfBodies);
    Vector<uint32_t> firstUseByBody(numberOfBodies + 1);
    forEachIndexInParallel(loops, firstUseByBody.size(), [&](unsigned index) {
        firstUseByBody[index] = index;
        if (index < numberOfBodies)
            uses[index] = { 0, 0, clampTo<uint32_t>(unitSizes[index]), reachOfNearCall };
    });

    size_t codeSizeBound = sharedCodeSize + imageStubsAlignment + granuleSize;
    for (size_t size : unitSizes)
        codeSizeBound += WTF::roundUpToMultipleOf<imageFunctionAlignment>(size) + imageFunctionAlignment;
    bool oneCopyReachesAllCode = static_cast<int64_t>(codeSizeBound) <= reachOfNearCall;

    const CopyableUnit sharedCode { sharedCodeSize, imageStubsAlignment };
    UnitPlacement placement = placeUnits({
        .sizes = unitSizes,
        .alignedOffsets = alignedOffsets,
        .alignments = singleElementSpan(imageFunctionAlignment),
        .fallsIntoNext = fallsIntoNext,
        .copyables = singleElementSpan(sharedCode),
        .uses = uses.span(),
        .firstUseByUnit = firstUseByBody.span(),
        .alignmentAfterCopies = granuleSize,
        // A closure word of 0 means "no code", so no body starts at offset 0.
        .start = oneCopyReachesAllCode && sharedCodeSize ? 0 : granuleSize,
        .policy = oneCopyReachesAllCode ? CopyPolicy::BeforeUser : CopyPolicy::AfterUsers,
    });
    // The run time enters stubs of every image, even of one without bodies.
    if (placement.copies.isEmpty()) {
        size_t offset = WTF::roundUpToMultipleOf<imageStubsAlignment>(placement.end);
        placement.copies.append(PlacedCopy { 0, offset });
        placement.end = WTF::roundUpToMultipleOf(granuleSize, offset + sharedCodeSize);
    }
    return placement;
}

// Leaves all but the size as it is where the code is larger than the limit, which is at most what 32 bits hold.
static void layOutWhatFollowsFunctions(const CodeLayoutInputs& inputs, CodeLayout& layout, size_t functionsEnd, uint64_t codeSizeLimit)
{
    static_assert(!(imagePageSize % RegExpSection::alignment));
    const RegExpSection& regExpSection = inputs.regExpSection;
    size_t webAssemblyCodeOffset = WTF::roundUpToMultipleOf(inputs.webAssemblyCodeAlignment, functionsEnd);
    size_t ffiCodeOffset = WTF::roundUpToMultipleOf(inputs.ffiCodeAlignment, webAssemblyCodeOffset + inputs.webAssemblyCodeSize);
    // The section of the regular expressions lies across the end of what is executed: its code is the last of the code, and its tables are the first of the data.
    size_t executableSize = WTF::roundUpToMultipleOf<imagePageSize>(ffiCodeOffset + inputs.ffiCodeSize + regExpSection.dataOffset);
    size_t regExpCodeOffset = executableSize - regExpSection.dataOffset;
    size_t constantsOffset = WTF::roundUpToMultipleOf<imageConstantsAlignment>(regExpCodeOffset + regExpSection.bytes.size());
    size_t ffiDirectoryOffset = constantsOffset + inputs.constantsSize;
    if (inputs.ffiCodeSize)
        ffiDirectoryOffset = WTF::roundUpToMultipleOf<imageFFIDirectoryAlignment>(ffiDirectoryOffset);
    layout.codeSize = ffiDirectoryOffset + inputs.ffiDirectorySize;
    if (layout.codeSize > codeSizeLimit)
        return;
    layout.functionsEnd = safeCast<uint32_t>(functionsEnd);
    layout.webAssemblyCodeOffset = safeCast<uint32_t>(webAssemblyCodeOffset);
    layout.ffiCodeOffset = safeCast<uint32_t>(ffiCodeOffset);
    layout.ffiCodeSize = safeCast<uint32_t>(inputs.ffiCodeSize);
    layout.regExpCodeOffset = safeCast<uint32_t>(regExpCodeOffset);
    layout.executableSize = safeCast<uint32_t>(executableSize);
    layout.constantsOffset = safeCast<uint32_t>(constantsOffset);
    layout.ffiDirectoryOffset = safeCast<uint32_t>(ffiDirectoryOffset);
}

// Without what rounds the code of the regular expressions up to RegExpSection::dataOffset: layOutRegExpSection() counts that.
static uint64_t paddingAfterFunctions(const CodeLayoutInputs& inputs, const CodeLayout& layout)
{
    uint64_t padding = layout.webAssemblyCodeOffset - layout.functionsEnd;
    padding += layout.ffiCodeOffset - (layout.webAssemblyCodeOffset + inputs.webAssemblyCodeSize);
    padding += layout.regExpCodeOffset - (layout.ffiCodeOffset + inputs.ffiCodeSize);
    padding += layout.constantsOffset - (layout.regExpCodeOffset + inputs.regExpSection.bytes.size());
    padding += layout.ffiDirectoryOffset - (layout.constantsOffset + inputs.constantsSize);
    return padding;
}

static size_t sizeOfUnitWithVeneers(size_t bodySize, size_t sizeOfFarInstanceWords, size_t numberOfVeneers, const CodeTemplates& templates)
{
    return FarInstanceWords::offsetBehind(bodySize) + sizeOfFarInstanceWords + numberOfVeneers * templates.sizeOfVeneer();
}

static size_t sharedCodeSizeWithVeneers(const CodeLayoutInputs& inputs, const CodeLayout& layout)
{
    return inputs.sharedCode.veneersOffset + layout.sharedCodeVeneers.size() * inputs.templates.sizeOfVeneer();
}

// With the veneers and the fall-throughs that the layout has so far. Nothing where the code is larger than the limit, which is reported.
static std::optional<UnitPlacement> placeCodeOfLayout(const CodeLayoutInputs& inputs, std::span<const size_t> bodySizes, std::span<const size_t> alignedOffsets, Vector<size_t>& unitSizes, CodeLayout& layout, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    const uint64_t codeSizeLimit = std::min<uint64_t>(context.limit(LimitID::CodeBytes), std::numeric_limits<uint32_t>::max());
    forEachIndexInParallel(loops, bodySizes.size(), [&](unsigned position) {
        size_t sizeOfFarInstanceWords = inputs.sizeOfFarInstanceWords(position);
        size_t numberOfVeneers = layout.veneersByBody[position].size();
        if (sizeOfFarInstanceWords || numberOfVeneers)
            unitSizes[position] = sizeOfUnitWithVeneers(bodySizes[position], sizeOfFarInstanceWords, numberOfVeneers, inputs.templates);
        else
            unitSizes[position] = layout.fallsIntoNext.quickGet(position) ? inputs.finalJumps[position].sizeWithoutJump : bodySizes[position];
    });
    size_t sharedCodeSize = sharedCodeSizeWithVeneers(inputs, layout);
    UnitPlacement placement = placeBodies(unitSizes.span(), alignedOffsets, layout.fallsIntoNext, sharedCodeSize, inputs.codeGranuleShift, loops);
    layOutWhatFollowsFunctions(inputs, layout, placement.end, codeSizeLimit);
    if (layout.codeSize > codeSizeLimit) {
        context.reportLimitExceeded(LimitID::CodeBytes, layout.codeSize);
        return std::nullopt;
    }
    layout.sharedCodeSize = safeCast<uint32_t>(sharedCodeSize);
    return placement;
}

// Stubs and argument thunks lie in the copy of the shared code that the placement keeps in reach of the body.
// The code of a function takes the address of the code of a regular expression and has no call or jump there: StubCalls makes none, and the symbol would be a mention, whose number is not known here.
// A relocation that names nothing is the linker's to report.
static void forEachTransferToOtherBody(const CodeLayoutInputs& inputs, uint32_t position, const Invocable<void(const Relocation&, uint32_t callee)> auto& functor)
{
    uint32_t firstCallee = inputs.callees.firstCallee[position];
    std::span<const OptionalIndex> callees = inputs.callees.positions.span().subspan(firstCallee, inputs.callees.firstCallee[position + 1] - firstCallee);
    for (const Relocation& relocation : inputs.functions[position].body.relocations) {
        if (!transfersControl(relocation.kind) || relocation.space != SymbolSpace::Function || relocation.symbol >= callees.size())
            continue;
        OptionalIndex callee = callees[relocation.symbol];
        if (callee && *callee != position)
            functor(relocation, *callee);
    }
}

static void appendTransfersToRegExpCode(Vector<VeneerTarget>& targets, std::span<const Relocation> relocationsOfSharedCode, const RegExpSection& section)
{
    for (const Relocation& relocation : relocationsOfSharedCode) {
        if (!transfersControl(relocation.kind) || relocation.space != SymbolSpace::RegExp)
            continue;
        if (relocation.symbol < section.entries.size() && relocation.entry < section.entries[relocation.symbol].size())
            targets.append(VeneerTarget { SymbolSpace::RegExp, relocation.entry, relocation.symbol });
    }
}

static void sortVeneerTargetsAndRemoveDuplicates(Vector<VeneerTarget>& targets)
{
    AOT::sortInLine(targets, [](const VeneerTarget& a, const VeneerTarget& b) {
        return std::tie(a.space, a.index, a.entry) < std::tie(b.space, b.index, b.entry);
    });
    removeRepeatedElements(targets);
}

// The starts of the first placement, each moved up by what the bodies in front of it could grow by: a veneer for every transfer, the final jump back, and the most that alignment takes.
// However many veneers there are in the end, two bodies lie no further apart than here, but for copies of the shared code that come to lie between them.
static Vector<size_t> computeLatestStartsOfBodies(const CodeLayoutInputs& inputs, std::span<const size_t> bodySizes, std::span<const size_t> unitSizes, std::span<const size_t> unitStarts, const ParallelLoops& loops)
{
    Vector<size_t> latestStarts(bodySizes.size());
    forEachIndexInParallel(loops, bodySizes.size(), [&](unsigned position) {
        size_t numberOfTransfers = 0;
        forEachTransferToOtherBody(inputs, position, [&](const Relocation&, uint32_t) {
            ++numberOfTransfers;
        });
        size_t largestSize = numberOfTransfers ? sizeOfUnitWithVeneers(bodySizes[position], inputs.sizeOfFarInstanceWords(position), numberOfTransfers, inputs.templates) : unitSizes[position];
        latestStarts[position] = largestSize - unitSizes[position] + imageFunctionAlignment - 1;
    });
    size_t growthInFront = 0;
    for (size_t position = 0; position < latestStarts.size(); ++position) {
        size_t growth = latestStarts[position];
        latestStarts[position] = unitStarts[position] + growthInFront;
        growthInFront += growth;
    }
    return latestStarts;
}

// Whether there are any.
static bool addVeneersToLayout(const CodeLayoutInputs& inputs, std::span<const size_t> bodySizes, std::span<const size_t> unitSizes, const UnitPlacement& firstPlacement, CodeLayout& layout, const ParallelLoops& loops)
{
    // The code of the regular expressions is the last of the code, and a copy of the shared code may come to lie anywhere in front of it.
    appendTransfersToRegExpCode(layout.sharedCodeVeneers, inputs.sharedCode.blob.code.relocations.span(), inputs.regExpSection);
    appendTransfersToRegExpCode(layout.sharedCodeVeneers, inputs.sharedCode.argumentThunks.code.relocations.span(), inputs.regExpSection);
    sortVeneerTargetsAndRemoveDuplicates(layout.sharedCodeVeneers);

    // A copy lies as far behind the last one as the bodies between them allow, which is further than a near call reaches. So at most one comes to lie between a transfer and a target that are in reach of each other.
    const size_t roomOfCopy = sharedCodeSizeWithVeneers(inputs, layout) + imageStubsAlignment + (static_cast<size_t>(1) << inputs.codeGranuleShift);
    const Vector<size_t> latestStarts = computeLatestStartsOfBodies(inputs, bodySizes, unitSizes, firstPlacement.unitStarts.span(), loops);
    std::atomic<bool> hasVeneers { !layout.sharedCodeVeneers.isEmpty() };
    forEachIndexInParallel(loops, bodySizes.size(), [&](unsigned position) {
        Vector<VeneerTarget>& veneers = layout.veneersByBody[position];
        forEachTransferToOtherBody(inputs, position, [&](const Relocation& relocation, uint32_t callee) {
            size_t site = latestStarts[position] + relocation.offset;
            size_t longestDistance = callee > position ? latestStarts[callee] + bodySizes[callee] - site : site - latestStarts[callee];
            if (static_cast<int64_t>(longestDistance + roomOfCopy) >= reachOfNearCall)
                veneers.append(VeneerTarget { SymbolSpace::Function, relocation.entry, callee });
        });
        if (veneers.isEmpty())
            return;
        sortVeneerTargetsAndRemoveDuplicates(veneers);
        hasVeneers.store(true, std::memory_order_relaxed);
    });
    for (size_t position = 0; position < bodySizes.size(); ++position) {
        if (!layout.veneersByBody[position].isEmpty())
            layout.fallsIntoNext.quickClear(position);
    }
    return hasVeneers.load(std::memory_order_relaxed);
}

CodeLayout layOutCode(const CodeLayoutInputs& inputs, BuildContext& context)
{
    RELEASE_ASSERT(inputs.finalJumps.size() == inputs.functions.size());
    RELEASE_ASSERT(inputs.farInstanceWords.empty() || inputs.farInstanceWords.size() == inputs.functions.size());
    const ParallelLoops& loops = context.parallelLoops();
    const size_t numberOfBodies = inputs.functions.size();
    const std::array<Vector<size_t>, 2> sizesAndAlignedOffsets = collectSizesAndAlignedOffsets(inputs.functions, context);
    std::span<const size_t> bodySizes = sizesAndAlignedOffsets[0].span();
    std::span<const size_t> alignedOffsets = sizesAndAlignedOffsets[1].span();

    CodeLayout layout;
    layout.codeGranuleShift = inputs.codeGranuleShift;
    layout.veneersByBody.grow(numberOfBodies);
    layout.fallsIntoNext = findFallThroughs(inputs, bodySizes);
    Vector<size_t> unitSizes(numberOfBodies);

    std::optional<UnitPlacement> placement = placeCodeOfLayout(inputs, bodySizes, alignedOffsets, unitSizes, layout, context);
    // Every transfer and every target lie in what is executed.
    if (placement && layout.executableSize > reachOfNearCall && addVeneersToLayout(inputs, bodySizes, unitSizes.span(), *placement, layout, loops))
        placement = placeCodeOfLayout(inputs, bodySizes, alignedOffsets, unitSizes, layout, context);
    if (!placement)
        return { };

    for (const PlacedCopy& copy : placement->copies)
        layout.sharedCodeCopies.append(safeCast<uint32_t>(copy.offset));
    layout.bodyStarts.grow(numberOfBodies);
    layout.sharedCodeCopyByBody.grow(numberOfBodies);
    std::atomic<uint64_t> numberOfVeneers { static_cast<uint64_t>(layout.sharedCodeVeneers.size()) * layout.sharedCodeCopies.size() };
    forEachIndexInParallel(loops, numberOfBodies, [&](unsigned position) {
        layout.bodyStarts[position] = safeCast<uint32_t>(placement->unitStarts[position]);
        layout.sharedCodeCopyByBody[position] = safeCast<uint32_t>(placement->copyOffsetByUse[position]);
        if (size_t numberOfVeneersOfBody = layout.veneersByBody[position].size())
            numberOfVeneers.fetch_add(numberOfVeneersOfBody, std::memory_order_relaxed);
    });
    context.statistics().count(StatisticID::Veneers, numberOfVeneers.load(std::memory_order_relaxed));
    context.statistics().count(StatisticID::StubCopiesEmitted, layout.sharedCodeCopies.size());
    context.statistics().count(StatisticID::FFICodeBytesEmitted, inputs.ffiCodeSize);
    context.statistics().count(StatisticID::FFIDirectoryBytesEmitted, inputs.ffiDirectorySize);
    context.statistics().count(StatisticID::PaddingBytesBetweenCodeParts, paddingAfterFunctions(inputs, layout));
    return layout;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
