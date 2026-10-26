/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTStubDescriptor.h"
#include "AOTUnitPlacement.h"
#include <cstdlib>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT {

AOT_THRESHOLD(size_t, maxRunSize, 1 * MB);

// Image::locate() runs where the cache of call sites has no entry. It walks the starts of functions from the first one in the granule, and sixteen of them lie in a cache line.
unsigned chooseCodeGranuleShift(const LinkedFunctions& functions, BuildContext& context)
{
    AOT_THRESHOLD(size_t, maxStartsPassedOnAverage, 8);
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

static constexpr size_t functionsInChunk = 1024;
static constexpr unsigned shardBits = 6;
static constexpr unsigned numberOfShards = 1 << shardBits;

using ArgumentMovesList = Vector<const ArgumentMoves*>;
using ShardedArgumentMoves = std::array<ArgumentMovesList, numberOfShards>;

static unsigned numberOfChunks(const LinkedFunctions& functions)
{
    return static_cast<unsigned>((functions.size() + functionsInChunk - 1) / functionsInChunk);
}

static std::span<const CompiledCode> chunkOf(const LinkedFunctions& functions, unsigned chunk)
{
    return functions.span().subspan(chunk * functionsInChunk, std::min(functionsInChunk, functions.size() - chunk * functionsInChunk));
}

static std::optional<StubKey> targetStubOf(const Relocation& relocation, const Vector<StubKey>& stubKeys, const Vector<ArgumentMoves>& argumentMoves)
{
    switch (relocation.space) {
    case SymbolSpace::Stub:
        return stubKeys[relocation.symbol];
    case SymbolSpace::ArgumentThunk:
        return StubKey::fromBits(argumentMoves[relocation.symbol][0]);
    case SymbolSpace::Function:
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::SiteCache:
    case SymbolSpace::RegExp:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// False where a stub that is called does not exist.
[[nodiscard]] static bool appendTargetStubs(Vector<StubKey>& keys, const Vector<Relocation>& relocations, const Vector<StubKey>& stubKeys, const Vector<ArgumentMoves>& argumentMoves)
{
    bool everyStubExists = true;
    for (const Relocation& relocation : relocations) {
        std::optional<StubKey> key = targetStubOf(relocation, stubKeys, argumentMoves);
        if (!key)
            continue;
        if (exists(*key))
            keys.append(*key);
        else
            everyStubExists = false;
    }
    return everyStubExists;
}

static Vector<StubKey> collectCalledKeys(const LinkedFunctions& functions, const OutlinedCode& outlinedCode, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<Vector<StubKey>> keysByChunk(numberOfChunks(functions));
    forEachIndexInParallel(loops, keysByChunk.size(), [&](unsigned chunk) {
        for (auto& function : chunkOf(functions, chunk)) {
            if (!appendTargetStubs(keysByChunk[chunk], function.body.relocations, function.body.stubKeys, function.body.argumentMoves))
                context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated, .function = function.function });
        }
        sortAndRemoveDuplicates(loops, keysByChunk[chunk]);
    });
    Vector<StubKey> keys;
    for (auto& chunkKeys : keysByChunk)
        keys.appendVector(chunkKeys);
    for (auto* bodies : { &outlinedCode.sharedBodies, &outlinedCode.ownBodies }) {
        if (!appendTargetStubs(keys, bodies->relocations, outlinedCode.stubKeys, outlinedCode.argumentMoves))
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated });
    }
    return keys;
}

static bool isLessThan(const ArgumentMoves* a, const ArgumentMoves* b)
{
    return std::ranges::lexicographical_compare(*a, *b);
}

static bool isEqual(const ArgumentMoves* a, const ArgumentMoves* b)
{
    return *a == *b;
}

static unsigned shardOf(const ArgumentMoves& moves)
{
    uint64_t hash = moves.size();
    for (uint64_t word : moves)
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ULL;
    return static_cast<unsigned>(hash >> (64 - shardBits));
}

static ArgumentMovesList collectArgumentMoves(const LinkedFunctions& functions, const OutlinedCode& outlinedCode, const ParallelLoops& loops)
{
    Vector<ShardedArgumentMoves> shardsByChunk(numberOfChunks(functions));
    forEachIndexInParallel(loops, shardsByChunk.size(), [&](unsigned chunk) {
        ArgumentMovesList chunkMoves;
        for (auto& function : chunkOf(functions, chunk)) {
            for (const Relocation& relocation : function.body.relocations) {
                if (relocation.space == SymbolSpace::ArgumentThunk)
                    chunkMoves.append(&function.body.argumentMoves[relocation.symbol]);
            }
        }
        sortAndRemoveDuplicates(loops, chunkMoves, isLessThan, isEqual);
        for (auto* moves : chunkMoves)
            shardsByChunk[chunk][shardOf(*moves)].append(moves);
    });
    ShardedArgumentMoves mergedShards;
    forEachIndexInParallel(loops, numberOfShards, [&](unsigned shard) {
        for (auto& shards : shardsByChunk)
            mergedShards[shard].appendVector(shards[shard]);
        sortAndRemoveDuplicates(loops, mergedShards[shard], isLessThan, isEqual);
    });
    ArgumentMovesList argumentMoves;
    for (auto& shardMoves : mergedShards)
        argumentMoves.appendVector(shardMoves);
    for (auto& moves : outlinedCode.argumentMoves)
        argumentMoves.append(&moves);
    sortAndRemoveDuplicates(loops, argumentMoves, isLessThan, isEqual);
    return argumentMoves;
}

uint32_t SharedCode::thunkOffset(const ArgumentMoves& moves) const
{
    auto found = std::ranges::lower_bound(argumentMoves, &moves, isLessThan);
    RELEASE_ASSERT(found != argumentMoves.end() && **found == moves);
    return argumentThunks.offsets[found - argumentMoves.begin()];
}

SharedCode selectSharedCode(const LinkedFunctions& functions, const OutlinedCode& outlinedCode, const StubCatalog& catalog, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    Vector<StubKey> keys = collectCalledKeys(functions, outlinedCode, context);
    if (context.diagnostics().hasErrors())
        return { };

    SharedCode result;
    result.argumentMoves = collectArgumentMoves(functions, outlinedCode, loops);
    auto argumentThunks = generateArgumentThunks(context, catalog, result.argumentMoves);
    if (!argumentThunks) {
        reportLinkFailure(argumentThunks.error(), { }, context);
        return { };
    }
    result.argumentThunks = WTF::move(*argumentThunks);
    keys.appendVector(result.argumentThunks.stubKeys);
    sortAndRemoveDuplicates(loops, keys);

    auto blob = buildStubBlob(context, catalog, keys.span(), entriesUsedByFunctions);
    if (!blob) {
        reportLinkFailure(blob.error(), { }, context);
        return { };
    }
    result.blob = WTF::move(*blob);

    uint64_t blobSize = result.blob.code.bytes.size();
    uint64_t argumentThunksSize = result.argumentThunks.code.bytes.size();
    uint64_t argumentThunksOffset = WTF::roundUpToMultipleOf<imageStubsAlignment>(blobSize);
    uint64_t sharedBodiesOffset = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(argumentThunksSize ? argumentThunksOffset + argumentThunksSize : blobSize);
    uint64_t veneersOffset = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(sharedBodiesOffset + outlinedCode.sharedBodies.bytes.size());
    if (veneersOffset > std::min<uint64_t>(context.limit(LimitID::CodeBytes), UINT32_MAX)) {
        context.reportLimitExceeded(LimitID::CodeBytes, veneersOffset);
        return { };
    }
    result.argumentThunksOffset = static_cast<uint32_t>(argumentThunksOffset);
    result.sharedBodiesOffset = static_cast<uint32_t>(sharedBodiesOffset);
    result.veneersOffset = static_cast<uint32_t>(veneersOffset);
    return result;
}

static bool endsInJumpToNextBody(const LinkedFunctions& functions, const ResolvedCallees& callees, const OutlinedCode& outlinedCode, uint32_t position)
{
    const CodeBody& body = functions[position].body;
    if (body.relocations.isEmpty())
        return false;
    const Relocation& jump = body.relocations.last();
    if (jump.kind != RelocationKind::Jump || jump.space != SymbolSpace::Function || jump.entry != primaryEntry || jump.offset + sizeOfJump != body.bytes.size())
        return false;
    ResolvedCallee callee = callees.at(position, jump.symbol);
    return callee.body == OptionalIndex { position + 1 } && !callee.alias
        && !outlinedCode.ownBodiesSize(position) && !functions[position + 1].body.primaryEntryOffset();
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

Vector<FinalJump> findFinalJumps(const LinkedFunctions& functions, const ResolvedCallees& callees, const OutlinedCode& outlinedCode, BuildContext& context)
{
    Vector<FinalJump> finalJumps(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& function = functions[position];
        finalJumps[position].sizeWithoutJump = safeCast<uint32_t>(function.body.bytes.size());
        if (position + 1 == functions.size() || !endsInJumpToNextBody(functions, callees, outlinedCode, position))
            return;
        if (std::optional<size_t> nopSize = computeNopSize(function))
            finalJumps[position] = { safeCast<uint32_t>(function.body.relocations.last().offset + *nopSize), true };
    });
    return finalJumps;
}

void removeFinalJumps(LinkedFunctions& functions, const BitVector& fallsIntoNext, std::span<const FinalJump> finalJumps, BuildContext& context)
{
    RELEASE_ASSERT(finalJumps.size() == functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        CompiledCode& function = functions[position];
        CodeBody& body = function.body;
        if (fallsIntoNext.get(position)) {
            RELEASE_ASSERT(finalJumps[position].jumpsToNextBody);
            size_t offset = body.relocations.takeLast().offset;
            body.bytes.shrink(offset);
            body.bytes.grow(finalJumps[position].sizeWithoutJump);
            fillWithNops(body.bytes.mutableSpan().subspan(offset));
            body.paddingStart = finalJumps[position].sizeWithoutJump;
        }
        std::optional<uint32_t> lastReturnSite = endOfLastReturnSite(function);
        RELEASE_ASSERT_WITH_MESSAGE(!lastReturnSite || *lastReturnSite < body.bytes.size(), "AOT: a function ends in a call: its return address would be taken for the next function's");
    });
    context.statistics().count(StatisticID::JumpsToNextFunctionRemoved, fallsIntoNext.bitCount());
}

static int64_t callReach(LimitID limit, BuildContext& context)
{
    return std::min<int64_t>(context.limit(limit), MacroAssembler::relocationReach(RelocationKind::Call));
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

static UnitPlacement placeBodies(std::span<const size_t> unitSizes, std::span<const size_t> alignedOffsets, const BitVector& fallsIntoNext, size_t sharedCodeSize, unsigned codeGranuleShift, BuildContext& context)
{
    static_assert(!((1 << minCodeGranuleShift) % imageFunctionAlignment) && !(sizeof(uint32_t) % imageFunctionAlignment));
    const size_t numberOfBodies = unitSizes.size();
    const size_t granuleSize = static_cast<size_t>(1) << codeGranuleShift;
    const uint32_t layoutSeed = context.recordedOptions().layoutSeed;
    const size_t shiftOfFirstBody = layoutSeed ? sizeof(uint32_t) * WeakRandom(~layoutSeed).getUint32(16) : 0;
    const int64_t reach = callReach(LimitID::DirectCallReach, context);

    Vector<CopyableUse> uses(numberOfBodies);
    Vector<uint32_t> firstUseByBody(numberOfBodies + 1);
    forEachIndexInParallel(context.parallelLoops(), firstUseByBody.size(), [&](unsigned index) {
        firstUseByBody[index] = index;
        if (index < numberOfBodies)
            uses[index] = { 0, 0, clampTo<uint32_t>(unitSizes[index]), reach };
    });

    size_t codeSizeBound = sharedCodeSize + imageStubsAlignment + granuleSize + shiftOfFirstBody;
    for (size_t size : unitSizes)
        codeSizeBound += WTF::roundUpToMultipleOf<imageFunctionAlignment>(size) + imageFunctionAlignment;
    bool oneCopyReachesAllCode = static_cast<int64_t>(codeSizeBound) <= reach;

    const CopyableUnit sharedCode { sharedCodeSize, imageStubsAlignment };
    const std::array<uint32_t, 2> firstUseBySharedCode { 0, 0 };
    UnitPlacement placement = placeUnits({
        .sizes = unitSizes,
        .alignedOffsets = alignedOffsets,
        .alignments = singleElementSpan(imageFunctionAlignment),
        .fallsIntoNext = fallsIntoNext,
        .copyables = singleElementSpan(sharedCode),
        .uses = uses.span(),
        .firstUseByUnit = firstUseByBody.span(),
        .usesOfCopyables = { },
        .firstUseByCopyable = firstUseBySharedCode,
        .alignmentAfterCopies = granuleSize,
        // A closure word of 0 means "no code", so no body starts at offset 0.
        .start = oneCopyReachesAllCode && sharedCodeSize ? 0 : granuleSize,
        .shiftOfFirstUnit = shiftOfFirstBody,
        .policy = oneCopyReachesAllCode ? CopyPolicy::BeforeUser : CopyPolicy::AfterUsers,
    });
    // The run time enters stubs of every image, even of one without bodies.
    if (placement.copies.isEmpty()) {
        size_t offset = WTF::roundUpToMultipleOf<imageStubsAlignment>(placement.end);
        placement.copies.append(PlacedCopy { 0, offset, { } });
        placement.end = WTF::roundUpToMultipleOf(granuleSize, offset + sharedCodeSize);
    }
    return placement;
}

unsigned countSharedCodeCopies(const LinkedFunctions& functions, BuildContext& context)
{
    auto [sizes, alignedOffsets] = collectSizesAndAlignedOffsets(functions, context);
    return placeBodies(sizes.span(), alignedOffsets.span(), BitVector(functions.size()), 0, maxCodeGranuleShift, context).copies.size();
}

// Nothing for a relocation that names nothing, which the linker reports.
static std::optional<VeneerTarget> targetOf(const CodeLayoutInputs& inputs, const Relocation& relocation, std::span<const ResolvedCallee> callees)
{
    switch (relocation.space) {
    case SymbolSpace::Function: {
        if (relocation.symbol >= callees.size() || !callees[relocation.symbol].body)
            return std::nullopt;
        ResolvedCallee callee = callees[relocation.symbol];
        return VeneerTarget { SymbolSpace::Function, relocation.entry, *callee.body, callee.alias };
    }
    case SymbolSpace::RegExp: {
        const auto& entries = inputs.regExpSection.entries;
        if (relocation.symbol >= entries.size() || relocation.entry >= entries[relocation.symbol].size())
            return std::nullopt;
        return VeneerTarget { SymbolSpace::RegExp, relocation.entry, relocation.symbol, { } };
    }
    case SymbolSpace::Stub:
    case SymbolSpace::ArgumentThunk:
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::SiteCache:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static size_t offsetOf(const CodeLayoutInputs& inputs, const CodeLayout& layout, const VeneerTarget& target)
{
    switch (target.space) {
    case SymbolSpace::Function: {
        size_t bodyStart = layout.bodyStarts[target.index];
        if (target.alias) {
            if (std::optional<uint32_t> innerEntryOffset = inputs.aliases[*target.alias].innerEntryOffset()) {
                RELEASE_ASSERT(target.entry == primaryEntry);
                return bodyStart + *innerEntryOffset;
            }
        }
        std::optional<uint32_t> entryOffset = inputs.functions[target.index].body.offsetOfEntry(target.entry);
        RELEASE_ASSERT(entryOffset);
        return bodyStart + *entryOffset;
    }
    case SymbolSpace::RegExp: {
        const auto& entries = inputs.regExpSection.entries[target.index];
        RELEASE_ASSERT(target.entry < entries.size());
        return static_cast<size_t>(layout.regExpCodeOffset) + entries[target.entry];
    }
    case SymbolSpace::Stub:
    case SymbolSpace::ArgumentThunk:
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::SiteCache:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachOwnBody(const OutlinedCode& outlinedCode, uint32_t caller, size_t endOfUnit, const Functor& functor)
{
    size_t offset = endOfUnit - outlinedCode.ownBodiesSize(caller);
    for (uint32_t number : outlinedCode.ownBodiesByCaller[caller]) {
        functor(number, offset);
        offset += outlinedCode.ownBodies.sizeOf(number);
    }
}

static bool addVeneers(const CodeLayoutInputs& inputs, const CodeLayout& layout, int64_t reach, size_t start, std::span<const Relocation> relocations, std::span<const ResolvedCallee> callees, Vector<VeneerTarget>& veneers)
{
    bool didAdd = false;
    for (const Relocation& relocation : relocations) {
        std::optional<VeneerTarget> target = transfersControl(relocation.kind) ? targetOf(inputs, relocation, callees) : std::nullopt;
        if (!target)
            continue;
        int64_t distance = static_cast<int64_t>(offsetOf(inputs, layout, *target)) - static_cast<int64_t>(start + relocation.offset);
        if (std::abs(distance) < reach || veneers.contains(*target))
            continue;
        veneers.append(*target);
        didAdd = true;
    }
    return didAdd;
}

static bool addVeneersForFarTransfers(const CodeLayoutInputs& inputs, CodeLayout& layout, std::span<const size_t> unitSizes, int64_t reach, BuildContext& context)
{
    const OutlinedCode& outlinedCode = inputs.outlinedCode;
    std::span<const ResolvedCallee> calleesOfOutlinedCode = outlinedCode.callees.span();
    std::atomic<bool> didAdd { false };
    forEachIndexInParallel(context.parallelLoops(), layout.veneersByBody.size(), [&](unsigned position) {
        Vector<VeneerTarget>& veneers = layout.veneersByBody[position];
        size_t start = layout.bodyStarts[position];
        bool didAddToBody = addVeneers(inputs, layout, reach, start, inputs.functions[position].body.relocations.span(), inputs.callees.callees[position].span(), veneers);
        forEachOwnBody(outlinedCode, position, start + unitSizes[position], [&](uint32_t number, size_t offset) {
            // The difference may wrap around: only its sum with an offset of the list is a position.
            didAddToBody |= addVeneers(inputs, layout, reach, offset - outlinedCode.ownBodies.offsets[number], outlinedCode.ownBodies.relocationsOf(number), calleesOfOutlinedCode, veneers);
        });
        if (didAddToBody)
            didAdd.store(true, std::memory_order_relaxed);
    });
    for (size_t copy : layout.sharedCodeCopies) {
        if (addVeneers(inputs, layout, reach, copy, inputs.sharedCode.blob.code.relocations.span(), { }, layout.sharedCodeVeneers))
            didAdd = true;
        if (addVeneers(inputs, layout, reach, copy + inputs.sharedCode.sharedBodiesOffset, outlinedCode.sharedBodies.relocations.span(), calleesOfOutlinedCode, layout.sharedCodeVeneers))
            didAdd = true;
    }
    return didAdd;
}

static BitVector findFallThroughs(std::span<const FinalJump> finalJumps, std::span<const size_t> bodySizes, const CodeLayout& layout)
{
    BitVector fallsIntoNext(bodySizes.size());
    for (size_t index = 0, sizeOfRun = 0; index + 1 < bodySizes.size(); ++index) {
        sizeOfRun += bodySizes[index];
        if (!finalJumps[index].jumpsToNextBody || !layout.veneersByBody[index].isEmpty() || sizeOfRun > maxRunSize) {
            sizeOfRun = 0;
            continue;
        }
        fallsIntoNext.quickSet(index);
    }
    return fallsIntoNext;
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

static void countLayoutStatisticsAndLimitHits(const LinkedFunctions& functions, const CodeLayout& layout, BuildContext& context)
{
    uint64_t numberOfVeneers = static_cast<uint64_t>(layout.sharedCodeVeneers.size()) * layout.sharedCodeCopies.size();
    for (auto& veneers : layout.veneersByBody)
        numberOfVeneers += veneers.size();
    context.statistics().count(StatisticID::Veneers, numberOfVeneers);
    context.statistics().count(StatisticID::StubCopiesEmitted, layout.sharedCodeCopies.size());
    context.statistics().count(StatisticID::CodeBytesEmitted, layout.codeSize);
    context.statistics().count(StatisticID::SharedCodeBytesEmitted, static_cast<uint64_t>(layout.sharedCodeSize) * layout.sharedCodeCopies.size());
    context.statistics().count(StatisticID::RegExpCodeBytesEmitted, layout.constantsOffset - layout.regExpCodeOffset);

    for (size_t copy = 1; copy < layout.sharedCodeCopies.size(); ++copy)
        context.gaveUp(LimitID::DirectCallReach);
    for (size_t veneer = 0; veneer < layout.sharedCodeVeneers.size(); ++veneer)
        context.gaveUp(LimitID::FunctionCallReach);
    for (size_t position = 0; position < layout.veneersByBody.size(); ++position) {
        for (size_t veneer = 0; veneer < layout.veneersByBody[position].size(); ++veneer)
            context.gaveUp(LimitID::FunctionCallReach, { .function = functions[position].function });
    }
}

CodeLayout layOutCode(const CodeLayoutInputs& inputs, BuildContext& context)
{
    RELEASE_ASSERT(inputs.finalJumps.size() == inputs.functions.size());
    const ParallelLoops& loops = context.parallelLoops();
    const OutlinedCode& outlinedCode = inputs.outlinedCode;
    const size_t numberOfBodies = inputs.functions.size();
    const uint64_t codeSizeLimit = std::min<uint64_t>(context.limit(LimitID::CodeBytes), std::numeric_limits<uint32_t>::max());
    const int64_t functionReach = callReach(LimitID::FunctionCallReach, context);
    const auto [bodySizes, alignedOffsets] = collectSizesAndAlignedOffsets(inputs.functions, context);

    CodeLayout layout;
    layout.codeGranuleShift = inputs.codeGranuleShift;
    layout.veneersByBody.grow(numberOfBodies);
    layout.bodyStarts.grow(numberOfBodies);
    Vector<size_t> unitSizes(numberOfBodies);
    UnitPlacement placement;
    for (;;) {
        layout.fallsIntoNext = findFallThroughs(inputs.finalJumps, bodySizes.span(), layout);
        forEachIndexInParallel(loops, numberOfBodies, [&](unsigned position) {
            size_t size = layout.fallsIntoNext.quickGet(position) ? inputs.finalJumps[position].sizeWithoutJump : bodySizes[position];
            if (size_t numberOfVeneers = layout.veneersByBody[position].size())
                size = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(size) + numberOfVeneers * inputs.templates.sizeOfVeneer();
            unitSizes[position] = size + outlinedCode.ownBodiesSize(position);
        });
        size_t sharedCodeSize = inputs.sharedCode.veneersOffset + layout.sharedCodeVeneers.size() * inputs.templates.sizeOfVeneer();
        placement = placeBodies(unitSizes.span(), alignedOffsets.span(), layout.fallsIntoNext, sharedCodeSize, inputs.codeGranuleShift, context);
        layOutWhatFollowsFunctions(inputs, layout, placement.end, codeSizeLimit);
        if (layout.codeSize > codeSizeLimit) {
            context.reportLimitExceeded(LimitID::CodeBytes, layout.codeSize);
            return { };
        }
        layout.sharedCodeSize = safeCast<uint32_t>(sharedCodeSize);
        layout.sharedCodeCopies.shrink(0);
        for (const PlacedCopy& copy : placement.copies)
            layout.sharedCodeCopies.append(safeCast<uint32_t>(copy.offset));
        forEachIndexInParallel(loops, numberOfBodies, [&](unsigned position) {
            layout.bodyStarts[position] = safeCast<uint32_t>(placement.unitStarts[position]);
        });
        if (static_cast<int64_t>(layout.regExpCodeOffset + inputs.regExpSection.bytes.size()) <= functionReach || !addVeneersForFarTransfers(inputs, layout, unitSizes.span(), functionReach, context))
            break;
    }

    layout.sharedCodeCopyByBody.grow(numberOfBodies);
    layout.ownBodyOffsets = Vector<uint32_t>(FillWith { }, outlinedCode.ownBodies.offsets.size(), 0);
    forEachIndexInParallel(loops, numberOfBodies, [&](unsigned position) {
        layout.sharedCodeCopyByBody[position] = safeCast<uint32_t>(placement.copyOffsetByUse[position]);
        forEachOwnBody(outlinedCode, position, layout.bodyStarts[position] + unitSizes[position], [&](uint32_t number, size_t offset) {
            layout.ownBodyOffsets[number] = safeCast<uint32_t>(offset);
        });
    });
    countLayoutStatisticsAndLimitHits(inputs.functions, layout, context);
    return layout;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace {

struct LinkScope {
    FunctionID caller;
    size_t sharedCodeCopy;
    std::span<const VeneerTarget> veneers;
    size_t firstVeneer;
    OptionalIndex position;
    std::span<const StubKey> stubKeys;
    std::span<const ArgumentMoves> argumentMoves;
    std::span<const ResolvedCallee> callees;
};

struct CodeLinker {
    void copyCode(size_t offset, std::span<const uint8_t> bytes) const
    {
        if (bytes.empty())
            return;
        RELEASE_ASSERT(isSumSmallerThanOrEqual(offset, bytes.size(), code.size()));
        memcpySpan(code.subspan(offset), bytes);
    }

    std::nullopt_t reportNotApplied(const Relocation& relocation, const LinkScope& scope) const
    {
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = scope.caller, .count = relocation.offset });
        return std::nullopt;
    }

    std::optional<size_t> resolve(const Relocation& relocation, const LinkScope& scope) const
    {
        const SharedCode& sharedCode = inputs.sharedCode;
        const OutlinedCode& outlinedCode = inputs.outlinedCode;
        uint32_t symbol = relocation.symbol;
        if ((relocation.kind == RelocationKind::InstanceOffset) != (relocation.space == SymbolSpace::SiteCache))
            return reportNotApplied(relocation, scope);

        switch (relocation.space) {
        case SymbolSpace::Function:
        case SymbolSpace::RegExp: {
            std::optional<VeneerTarget> target = targetOf(inputs, relocation, scope.callees);
            if (!target)
                return reportNotApplied(relocation, scope);
            if (transfersControl(relocation.kind)) {
                auto veneer = std::ranges::find(scope.veneers, *target);
                if (veneer != scope.veneers.end())
                    return scope.firstVeneer + (veneer - scope.veneers.begin()) * inputs.templates.sizeOfVeneer();
            }
            return offsetOf(inputs, layout, *target);
        }
        case SymbolSpace::Stub: {
            if (symbol >= scope.stubKeys.size())
                return reportNotApplied(relocation, scope);
            if (std::optional<uint32_t> offset = sharedCode.blob.offsetOf(scope.stubKeys[symbol]))
                return scope.sharedCodeCopy + *offset;
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated, .function = scope.caller });
            return std::nullopt;
        }
        case SymbolSpace::ArgumentThunk:
            if (symbol >= scope.argumentMoves.size())
                return reportNotApplied(relocation, scope);
            return scope.sharedCodeCopy + sharedCode.argumentThunksOffset + sharedCode.thunkOffset(scope.argumentMoves[symbol]);
        case SymbolSpace::OutlinedBody: {
            if (symbol >= linkerInputs.outlining.bodies.size())
                return reportNotApplied(relocation, scope);
            uint32_t number = outlinedCode.indexWithinKind[symbol];
            if (linkerInputs.outlining.bodies[symbol].singleCaller)
                return layout.ownBodyOffsets[number];
            return scope.sharedCodeCopy + sharedCode.sharedBodiesOffset + outlinedCode.sharedBodies.offsets[number];
        }
        case SymbolSpace::Constant:
        case SymbolSpace::JumpTable: {
            auto& offsetsByBody = relocation.space == SymbolSpace::Constant ? linkerInputs.constants.constantOffsetsByBody : linkerInputs.constants.jumpTableOffsetsByBody;
            if (!scope.position || symbol >= offsetsByBody[*scope.position].size())
                return reportNotApplied(relocation, scope);
            return static_cast<size_t>(layout.constantsOffset) + offsetsByBody[*scope.position][symbol];
        }
        case SymbolSpace::SiteCache:
            if (symbol >= linkerInputs.instanceSizes.numberOfSiteCaches)
                return reportNotApplied(relocation, scope);
            return InstanceLayout::offsetOfSiteCache(linkerInputs.instanceSizes, symbol);
        case SymbolSpace::WebAssembly:
        case SymbolSpace::FFI:
            return reportNotApplied(relocation, scope);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    uint64_t link(size_t start, size_t end, std::span<const Relocation> relocations, const LinkScope& scope) const
    {
        auto resolveInsideCode = [&](const Relocation& relocation) -> std::optional<size_t> {
            size_t size = MacroAssembler::relocationSize(relocation.kind);
            if (!size || !isSumSmallerThanOrEqual<size_t>(relocation.offset, size, end))
                return std::nullopt;
            return resolve(relocation, scope);
        };
        auto reportOutOfReach = [&](const Relocation& relocation) {
            reportNotApplied(relocation, scope);
        };
        return applyRelocations(code, start, relocations, resolveInsideCode, reportOutOfReach);
    }

    bool writeVeneer(size_t at, size_t target) const
    {
        const RelocatableCode& veneer = inputs.templates.veneer;
        copyCode(at, veneer.bytes.span());
        return applyRelocation(code, at, veneer.relocations[0], target);
    }

    uint64_t writeVeneers(size_t firstVeneer, std::span<const VeneerTarget> targets) const
    {
        uint64_t numberApplied = 0;
        for (size_t index = 0; index < targets.size(); ++index)
            numberApplied += writeVeneer(firstVeneer + index * inputs.templates.sizeOfVeneer(), offsetOf(inputs, layout, targets[index]));
        return numberApplied;
    }

    uint64_t linkSharedCode(size_t copy) const
    {
        const SharedCode& sharedCode = inputs.sharedCode;
        const OutlinedCode& outlinedCode = inputs.outlinedCode;
        std::span<const VeneerTarget> veneers = layout.sharedCodeVeneers.span();
        size_t firstVeneer = copy + sharedCode.veneersOffset;
        RELEASE_ASSERT(sharedCode.veneersOffset + veneers.size() * inputs.templates.sizeOfVeneer() <= layout.sharedCodeSize);

        auto copyAndLink = [&](size_t start, const Vector<uint8_t>& bytes, const Vector<Relocation>& relocations, std::span<const StubKey> stubKeys, std::span<const ArgumentMoves> argumentMoves, std::span<const ResolvedCallee> callees) {
            copyCode(start, bytes.span());
            return link(start, bytes.size(), relocations.span(), { { }, copy, veneers, firstVeneer, { }, stubKeys, argumentMoves, callees });
        };
        uint64_t numberApplied = writeVeneers(firstVeneer, veneers);
        numberApplied += copyAndLink(copy, sharedCode.blob.code.bytes, sharedCode.blob.code.relocations, { }, { }, { });
        if (size_t firstCopy = layout.sharedCodeCopies[0]; copy != firstCopy) {
            for (const StubEntryRange& entry : sharedCode.blob.entriesInFrontOfSingleReturnSites) {
                RELEASE_ASSERT(entry.offset + inputs.templates.sizeOfVeneer() <= entry.end);
                numberApplied += writeVeneer(copy + entry.offset, firstCopy + entry.offset);
            }
        }
        numberApplied += copyAndLink(copy + sharedCode.argumentThunksOffset, sharedCode.argumentThunks.code.bytes, sharedCode.argumentThunks.code.relocations, sharedCode.argumentThunks.stubKeys.span(), { }, { });
        numberApplied += copyAndLink(copy + sharedCode.sharedBodiesOffset, outlinedCode.sharedBodies.bytes, outlinedCode.sharedBodies.relocations, outlinedCode.stubKeys.span(), outlinedCode.argumentMoves.span(), outlinedCode.callees.span());
        return numberApplied;
    }

    void writeJumpTables(uint32_t position) const
    {
        const CompiledCode& function = inputs.functions[position];
        const Vector<uint32_t, 0>& tableOffsets = linkerInputs.constants.jumpTableOffsetsByBody[position];
        RELEASE_ASSERT(tableOffsets.size() == function.body.jumpTables.size());
        int64_t bodyStart = layout.bodyStarts[position];
        for (size_t index = 0; index < tableOffsets.size(); ++index) {
            std::span<const uint32_t> targets = function.body.jumpTables[index].targets.span();
            size_t table = static_cast<size_t>(layout.constantsOffset) + tableOffsets[index];
            RELEASE_ASSERT(isSumSmallerThanOrEqual(table, targets.size_bytes(), code.size()));
            std::span<uint8_t> words = code.subspan(table, targets.size_bytes());
            for (uint32_t target : targets) {
                int64_t distance = bodyStart + target - static_cast<int64_t>(table);
                if (!isRepresentableAs<int32_t>(distance)) {
                    context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = function.function, .count = target });
                    return;
                }
                WTF::unalignedStore<int32_t>(words.data(), static_cast<int32_t>(distance));
                words = words.subspan(sizeof(int32_t));
            }
        }
    }

    uint64_t linkBody(uint32_t position) const
    {
        const CompiledCode& function = inputs.functions[position];
        const CodeBody& body = function.body;
        const OutlinedCode& outlinedCode = inputs.outlinedCode;
        const OutlinedBodies& ownBodies = outlinedCode.ownBodies;
        std::span<const VeneerTarget> veneers = layout.veneersByBody[position].span();
        size_t start = layout.bodyStarts[position];
        size_t sharedCodeCopy = layout.sharedCodeCopyByBody[position];
        size_t firstVeneer = layout.firstVeneerOffset(position, body.bytes.size());

        copyCode(start, body.bytes.span());
        uint64_t numberApplied = writeVeneers(firstVeneer, veneers);
        numberApplied += link(start, body.bytes.size(), body.relocations.span(), { function.function, sharedCodeCopy, veneers, firstVeneer, { position }, body.stubKeys.span(), body.argumentMoves.span(), inputs.callees.callees[position].span() });
        LinkScope ownBodyScope { function.function, sharedCodeCopy, veneers, firstVeneer, { }, outlinedCode.stubKeys.span(), outlinedCode.argumentMoves.span(), outlinedCode.callees.span() };
        for (uint32_t number : outlinedCode.ownBodiesByCaller[position]) {
            size_t begin = ownBodies.offsets[number];
            size_t end = begin + ownBodies.sizeOf(number);
            size_t ownBodyStart = layout.ownBodyOffsets[number];
            copyCode(ownBodyStart, ownBodies.bytes.subspan(begin, end - begin));
            // The difference may wrap around: only its sum with an offset of the list is a position.
            numberApplied += link(ownBodyStart - begin, end, ownBodies.relocationsOf(number), ownBodyScope);
        }
        writeJumpTables(position);
        return numberApplied;
    }

    uint64_t countExpectedLinks() const
    {
        const SharedCode& sharedCode = inputs.sharedCode;
        uint64_t linksInSharedCode = sharedCode.blob.code.relocations.size() + sharedCode.argumentThunks.code.relocations.size() + inputs.outlinedCode.sharedBodies.relocations.size() + layout.sharedCodeVeneers.size();
        uint64_t expected = linksInSharedCode * layout.sharedCodeCopies.size() + inputs.outlinedCode.ownBodies.relocations.size();
        expected += sharedCode.blob.entriesInFrontOfSingleReturnSites.size() * (layout.sharedCodeCopies.size() - 1);
        for (size_t position = 0; position < inputs.functions.size(); ++position)
            expected += inputs.functions[position].body.relocations.size() + layout.veneersByBody[position].size();
        return expected;
    }

    const CodeLinkerInputs& linkerInputs;
    const CodeLayoutInputs& inputs;
    const CodeLayout& layout;
    std::span<uint8_t> code;
    BuildContext& context;
};

} // anonymous namespace

void linkCode(const CodeLinkerInputs& inputs, std::span<uint8_t> code, BuildContext& context)
{
    constexpr uint32_t bodiesPerChunk = 64;
    const ParallelLoops& loops = context.parallelLoops();
    const CodeLayout& layout = inputs.layout;
    RELEASE_ASSERT(code.size() == layout.codeSize);
    CodeLinker linker { inputs, inputs.layoutInputs, layout, code, context };
    // linkBody writes the jump tables over the zeros that the pool holds for them.
    linker.copyCode(layout.constantsOffset, inputs.constants.bytes.span());

    uint32_t numberOfBodies = static_cast<uint32_t>(inputs.layoutInputs.functions.size());
    uint64_t expected = 0;
    std::atomic<uint64_t> applied { 0 };
    runTasksInParallel(loops, {
        [&] { expected = linker.countExpectedLinks(); },
        [&] {
            forEachIndexInParallel(loops, layout.sharedCodeCopies.size(), [&](unsigned index) {
                applied += linker.linkSharedCode(layout.sharedCodeCopies[index]);
            });
        },
        [&] {
            forEachIndexInParallel(loops, (numberOfBodies + bodiesPerChunk - 1) / bodiesPerChunk, [&](unsigned chunk) {
                uint64_t numberApplied = 0;
                for (uint32_t position = chunk * bodiesPerChunk; position < std::min((chunk + 1) * bodiesPerChunk, numberOfBodies); ++position)
                    numberApplied += linker.linkBody(position);
                applied += numberApplied;
            });
        },
    });

    // A relocation that got a diagnostic of its own was not applied either.
    if (!context.diagnostics().hasErrors() && expected != applied)
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = clampTo<uint32_t>(expected > applied ? expected - applied : applied - expected) });
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
