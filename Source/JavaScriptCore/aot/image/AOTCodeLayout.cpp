/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT {

AOT_THRESHOLD(size_t, maxRunSize, 1 * MB);

namespace {

struct BodyExtents {
    Vector<size_t> sizes;
    Vector<size_t> alignedOffsets;
};

struct BodyPlacementInputs {
    std::span<const size_t> unitSizes;
    std::span<const size_t> alignedOffsets;
    const BitVector& fallsIntoNext;
    size_t sharedCodeSize;
    unsigned codeGranuleShift;
};

struct Veneers {
    Vector<Vector<VeneerTarget, 0>> byBody;
    Vector<VeneerTarget> ofSharedCode;
};

struct CodeParts {
    size_t functionsEnd { 0 };
    size_t webAssemblyCodeOffset { 0 };
    size_t ffiCodeOffset { 0 };
    size_t regExpCodeOffset { 0 };
    size_t executableSize { 0 };
    size_t regExpCodeEnd { 0 };
    size_t constantsOffset { 0 };
    size_t ffiDirectoryOffset { 0 };
    size_t codeSize { 0 };
};

struct PlacedCode {
    BitVector fallsIntoNext;
    Vector<size_t> unitSizes;
    size_t sharedCodeSize { 0 };
    UnitPlacement placement;
    CodeParts parts;
};

class FarTransferFinder {
public:
    FarTransferFinder(const CodeLayoutInputs& inputs, const PlacedCode& code, const CodePositions& positions, int64_t reach)
        : m_inputs(inputs)
        , m_code(code)
        , m_positions(positions)
        , m_reach(reach)
    {
    }

    bool addVeneersOfBodies(Veneers&, BuildContext&) const;
    bool addVeneersOfSharedCode(Veneers&) const;

private:
    std::optional<VeneerTarget> veneerTargetOf(const Relocation&, std::span<const ResolvedCallee>) const;
    bool addVeneers(size_t start, std::span<const Relocation>, std::span<const ResolvedCallee>, Vector<VeneerTarget>&) const;
    bool addVeneersOfBody(uint32_t position, Vector<VeneerTarget>&) const;

    const CodeLayoutInputs& m_inputs;
    const PlacedCode& m_code;
    const CodePositions m_positions;
    const int64_t m_reach;
};

} // anonymous namespace

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

static int64_t callReach(LimitID limit, BuildContext& context)
{
    return std::min<int64_t>(context.limit(limit), MacroAssembler::relocationReach(RelocationKind::Call));
}

static size_t firstFunctionShift(uint32_t layoutSeed)
{
    static_assert(!(sizeof(uint32_t) % imageFunctionAlignment));
    return layoutSeed ? sizeof(uint32_t) * WeakRandom(~layoutSeed).getUint32(16) : 0;
}

static BodyExtents collectBodyExtents(const LinkedFunctions& functions, BuildContext& context)
{
    BodyExtents extents { Vector<size_t>(functions.size()), Vector<size_t>(functions.size()) };
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CodeBody& body = functions[position].body;
        extents.sizes[position] = body.bytes.size();
        extents.alignedOffsets[position] = body.primaryEntryOffset();
    });
    return extents;
}

static UnitPlacement placeBodies(const BodyPlacementInputs& inputs, BuildContext& context)
{
    static_assert(!((1 << minCodeGranuleShift) % imageFunctionAlignment));
    const size_t numberOfBodies = inputs.unitSizes.size();
    const size_t granuleSize = static_cast<size_t>(1) << inputs.codeGranuleShift;
    const size_t shiftOfFirstBody = firstFunctionShift(context.recordedOptions().layoutSeed);
    const int64_t reach = callReach(LimitID::DirectCallReach, context);

    Vector<CopyableUse> uses(numberOfBodies);
    Vector<uint32_t> firstUseByBody(numberOfBodies + 1);
    forEachIndexInParallel(context.parallelLoops(), firstUseByBody.size(), [&](unsigned index) {
        firstUseByBody[index] = index;
        if (index < numberOfBodies)
            uses[index] = { 0, 0, clampTo<uint32_t>(inputs.unitSizes[index]), reach };
    });

    size_t codeSizeBound = inputs.sharedCodeSize + imageStubsAlignment + granuleSize + shiftOfFirstBody;
    for (size_t size : inputs.unitSizes)
        codeSizeBound += WTF::roundUpToMultipleOf<imageFunctionAlignment>(size) + imageFunctionAlignment;
    bool oneCopyReachesAllCode = static_cast<int64_t>(codeSizeBound) <= reach;

    const CopyableUnit sharedCode { inputs.sharedCodeSize, imageStubsAlignment };
    const std::array<uint32_t, 2> firstUseBySharedCode { 0, 0 };
    UnitPlacement placement = placeUnits({
        .sizes = inputs.unitSizes,
        .alignedOffsets = inputs.alignedOffsets,
        .alignments = singleElementSpan(imageFunctionAlignment),
        .fallsIntoNext = inputs.fallsIntoNext,
        .copyables = singleElementSpan(sharedCode),
        .uses = uses.span(),
        .firstUseByUnit = firstUseByBody.span(),
        .usesOfCopyables = { },
        .firstUseByCopyable = firstUseBySharedCode,
        .alignmentAfterCopies = granuleSize,
        // A closure word of 0 means "no code", so no body starts at offset 0.
        .start = oneCopyReachesAllCode && inputs.sharedCodeSize ? 0 : granuleSize,
        .shiftOfFirstUnit = shiftOfFirstBody,
        .policy = oneCopyReachesAllCode ? CopyPolicy::BeforeUser : CopyPolicy::AfterUsers,
    });
    // The run time enters stubs of every image, even of one without bodies.
    if (placement.copies.isEmpty()) {
        size_t offset = WTF::roundUpToMultipleOf<imageStubsAlignment>(placement.end);
        placement.copies.append(PlacedCopy { 0, offset, { } });
        placement.end = WTF::roundUpToMultipleOf(granuleSize, offset + inputs.sharedCodeSize);
    }
    return placement;
}

unsigned countSharedCodeCopies(const LinkedFunctions& functions, BuildContext& context)
{
    BodyExtents extents = collectBodyExtents(functions, context);
    BitVector fallsIntoNext(functions.size());
    return placeBodies({ extents.sizes.span(), extents.alignedOffsets.span(), fallsIntoNext, 0, maxCodeGranuleShift }, context).copies.size();
}

size_t targetOffset(const CodeLayoutInputs& inputs, const CodePositions& positions, const VeneerTarget& target)
{
    switch (target.space) {
    case SymbolSpace::Function: {
        size_t bodyStart = positions.bodyStarts[target.index];
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
        return static_cast<size_t>(positions.regExpCodeOffset) + entries[target.entry];
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

// The linker reports a relocation that names nothing.
std::optional<VeneerTarget> FarTransferFinder::veneerTargetOf(const Relocation& relocation, std::span<const ResolvedCallee> callees) const
{
    if (!transfersControl(relocation.kind))
        return std::nullopt;
    switch (relocation.space) {
    case SymbolSpace::Function: {
        if (relocation.symbol >= callees.size() || !callees[relocation.symbol].body)
            return std::nullopt;
        ResolvedCallee callee = callees[relocation.symbol];
        return VeneerTarget { SymbolSpace::Function, relocation.entry, *callee.body, callee.alias };
    }
    case SymbolSpace::RegExp: {
        const auto& entries = m_inputs.regExpSection.entries;
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

template<typename Functor>
static void forEachOwnBody(const OutlinedCode& outlinedCode, uint32_t caller, size_t endOfUnit, const Functor& functor)
{
    size_t offset = endOfUnit - outlinedCode.ownBodiesSize(caller);
    for (uint32_t number : outlinedCode.ownBodiesByCaller[caller]) {
        functor(number, offset);
        offset += outlinedCode.ownBodies.sizeOf(number);
    }
}

bool FarTransferFinder::addVeneers(size_t start, std::span<const Relocation> relocations, std::span<const ResolvedCallee> callees, Vector<VeneerTarget>& veneers) const
{
    bool didAdd = false;
    for (const Relocation& relocation : relocations) {
        std::optional<VeneerTarget> target = veneerTargetOf(relocation, callees);
        if (!target)
            continue;
        int64_t distance = static_cast<int64_t>(targetOffset(m_inputs, m_positions, *target)) - static_cast<int64_t>(start + relocation.offset);
        if (std::abs(distance) < m_reach || veneers.contains(*target))
            continue;
        veneers.append(*target);
        didAdd = true;
    }
    return didAdd;
}

bool FarTransferFinder::addVeneersOfBody(uint32_t position, Vector<VeneerTarget>& veneers) const
{
    const OutlinedCode& outlinedCode = m_inputs.outlinedCode;
    size_t start = m_positions.bodyStarts[position];
    bool didAdd = addVeneers(start, m_inputs.functions[position].body.relocations.span(), m_inputs.callees.callees[position].span(), veneers);
    forEachOwnBody(outlinedCode, position, start + m_code.unitSizes[position], [&](uint32_t number, size_t offset) {
        // The difference may wrap around: only its sum with an offset of the list is a position.
        size_t startOfList = offset - outlinedCode.ownBodies.offsets[number];
        didAdd |= addVeneers(startOfList, outlinedCode.ownBodies.relocationsOf(number), outlinedCode.callees.span(), veneers);
    });
    return didAdd;
}

bool FarTransferFinder::addVeneersOfBodies(Veneers& veneers, BuildContext& context) const
{
    std::atomic<bool> didAdd { false };
    forEachIndexInParallel(context.parallelLoops(), veneers.byBody.size(), [&](unsigned position) {
        if (addVeneersOfBody(position, veneers.byBody[position]))
            didAdd.store(true, std::memory_order_relaxed);
    });
    return didAdd.load(std::memory_order_relaxed);
}

bool FarTransferFinder::addVeneersOfSharedCode(Veneers& veneers) const
{
    const SharedCode& sharedCode = m_inputs.sharedCode;
    const OutlinedCode& outlinedCode = m_inputs.outlinedCode;
    bool didAdd = false;
    for (const PlacedCopy& copy : m_code.placement.copies) {
        didAdd |= addVeneers(copy.offset, sharedCode.blob.code.relocations.span(), { }, veneers.ofSharedCode);
        didAdd |= addVeneers(copy.offset + sharedCode.sharedBodiesOffset, outlinedCode.sharedBodies.relocations.span(), outlinedCode.callees.span(), veneers.ofSharedCode);
    }
    return didAdd;
}

static BitVector findFallThroughs(std::span<const FinalJump> finalJumps, std::span<const size_t> bodySizes, const Veneers& veneers)
{
    BitVector fallsIntoNext(bodySizes.size());
    for (size_t index = 0, sizeOfRun = 0; index + 1 < bodySizes.size(); ++index) {
        sizeOfRun += bodySizes[index];
        if (!finalJumps[index].jumpsToNextBody || !veneers.byBody[index].isEmpty() || sizeOfRun > maxRunSize) {
            sizeOfRun = 0;
            continue;
        }
        fallsIntoNext.quickSet(index);
    }
    return fallsIntoNext;
}

static Vector<size_t> computeUnitSizes(const CodeLayoutInputs& inputs, std::span<const size_t> bodySizes, const Veneers& veneers, const BitVector& fallsIntoNext, BuildContext& context)
{
    Vector<size_t> unitSizes(bodySizes.size());
    forEachIndexInParallel(context.parallelLoops(), unitSizes.size(), [&](unsigned position) {
        size_t size = fallsIntoNext.quickGet(position) ? inputs.finalJumps[position].sizeWithoutJump : bodySizes[position];
        if (size_t numberOfVeneers = veneers.byBody[position].size())
            size = WTF::roundUpToMultipleOf<sizeof(uint32_t)>(size) + numberOfVeneers * inputs.templates.sizeOfVeneer();
        unitSizes[position] = size + inputs.outlinedCode.ownBodiesSize(position);
    });
    return unitSizes;
}

static CodeParts layOutCodeParts(const CodeLayoutInputs& inputs, size_t functionsEnd)
{
    static_assert(!(imagePageSize % RegExpSection::alignment));
    const RegExpSection& regExpSection = inputs.regExpSection;
    CodeParts parts;
    parts.functionsEnd = functionsEnd;
    parts.webAssemblyCodeOffset = WTF::roundUpToMultipleOf(inputs.webAssemblyCodeAlignment, functionsEnd);
    parts.ffiCodeOffset = WTF::roundUpToMultipleOf(inputs.ffiCodeAlignment, parts.webAssemblyCodeOffset + inputs.webAssemblyCodeSize);
    // The section of the regular expressions lies across the end of what is executed: its code is the last of the code, and its tables are the first of the data.
    parts.executableSize = WTF::roundUpToMultipleOf<imagePageSize>(parts.ffiCodeOffset + inputs.ffiCodeSize + regExpSection.dataOffset);
    parts.regExpCodeOffset = parts.executableSize - regExpSection.dataOffset;
    parts.regExpCodeEnd = parts.regExpCodeOffset + regExpSection.bytes.size();
    parts.constantsOffset = WTF::roundUpToMultipleOf<imageConstantsAlignment>(parts.regExpCodeEnd);
    parts.ffiDirectoryOffset = parts.constantsOffset + inputs.constantsSize;
    if (inputs.ffiCodeSize)
        parts.ffiDirectoryOffset = WTF::roundUpToMultipleOf<imageFFIDirectoryAlignment>(parts.ffiDirectoryOffset);
    parts.codeSize = parts.ffiDirectoryOffset + inputs.ffiDirectorySize;
    return parts;
}

static PlacedCode placeCode(const CodeLayoutInputs& inputs, const BodyExtents& extents, const Veneers& veneers, BuildContext& context)
{
    PlacedCode code;
    code.fallsIntoNext = findFallThroughs(inputs.finalJumps, extents.sizes.span(), veneers);
    code.unitSizes = computeUnitSizes(inputs, extents.sizes.span(), veneers, code.fallsIntoNext, context);
    code.sharedCodeSize = inputs.sharedCode.veneersOffset + veneers.ofSharedCode.size() * inputs.templates.sizeOfVeneer();
    code.placement = placeBodies({ code.unitSizes.span(), extents.alignedOffsets.span(), code.fallsIntoNext, code.sharedCodeSize, inputs.codeGranuleShift }, context);
    code.parts = layOutCodeParts(inputs, code.placement.end);
    return code;
}

static Vector<uint32_t> narrowOffsets(std::span<const size_t> offsets, BuildContext& context)
{
    Vector<uint32_t> narrowedOffsets(offsets.size());
    forEachIndexInParallel(context.parallelLoops(), offsets.size(), [&](unsigned index) {
        narrowedOffsets[index] = safeCast<uint32_t>(offsets[index]);
    });
    return narrowedOffsets;
}

static CodeLayout createLayout(const CodeLayoutInputs& inputs, PlacedCode&& code, Vector<uint32_t>&& bodyStarts, Veneers&& veneers, BuildContext& context)
{
    const OutlinedCode& outlinedCode = inputs.outlinedCode;
    CodeLayout layout;
    layout.codeGranuleShift = inputs.codeGranuleShift;
    layout.bodyStarts = WTF::move(bodyStarts);
    layout.sharedCodeCopyByBody.grow(layout.bodyStarts.size());
    layout.ownBodyOffsets = Vector<uint32_t>(FillWith { }, outlinedCode.ownBodies.offsets.size(), 0);
    forEachIndexInParallel(context.parallelLoops(), layout.bodyStarts.size(), [&](unsigned position) {
        layout.sharedCodeCopyByBody[position] = safeCast<uint32_t>(code.placement.copyOffsetByUse[position]);
        forEachOwnBody(outlinedCode, position, layout.bodyStarts[position] + code.unitSizes[position], [&](uint32_t number, size_t offset) {
            layout.ownBodyOffsets[number] = safeCast<uint32_t>(offset);
        });
    });
    for (const PlacedCopy& copy : code.placement.copies)
        layout.sharedCodeCopies.append(safeCast<uint32_t>(copy.offset));
    layout.veneersByBody = WTF::move(veneers.byBody);
    layout.sharedCodeVeneers = WTF::move(veneers.ofSharedCode);
    layout.fallsIntoNext = WTF::move(code.fallsIntoNext);
    layout.sharedCodeSize = safeCast<uint32_t>(code.sharedCodeSize);
    layout.functionsEnd = safeCast<uint32_t>(code.parts.functionsEnd);
    layout.webAssemblyCodeOffset = safeCast<uint32_t>(code.parts.webAssemblyCodeOffset);
    layout.ffiCodeOffset = safeCast<uint32_t>(code.parts.ffiCodeOffset);
    layout.ffiCodeSize = safeCast<uint32_t>(inputs.ffiCodeSize);
    layout.regExpCodeOffset = safeCast<uint32_t>(code.parts.regExpCodeOffset);
    layout.executableSize = safeCast<uint32_t>(code.parts.executableSize);
    layout.constantsOffset = safeCast<uint32_t>(code.parts.constantsOffset);
    layout.ffiDirectoryOffset = safeCast<uint32_t>(code.parts.ffiDirectoryOffset);
    layout.codeSize = code.parts.codeSize;
    return layout;
}

static void countStatistics(const CodeLayout& layout, BuildContext& context)
{
    uint64_t numberOfVeneers = static_cast<uint64_t>(layout.sharedCodeVeneers.size()) * layout.sharedCodeCopies.size();
    for (auto& veneers : layout.veneersByBody)
        numberOfVeneers += veneers.size();
    context.statistics().count(StatisticID::Veneers, numberOfVeneers);
    context.statistics().count(StatisticID::StubCopiesEmitted, layout.sharedCodeCopies.size());
    context.statistics().count(StatisticID::CodeBytesEmitted, layout.codeSize);
    context.statistics().count(StatisticID::SharedCodeBytesEmitted, static_cast<uint64_t>(layout.sharedCodeSize) * layout.sharedCodeCopies.size());
    context.statistics().count(StatisticID::RegExpCodeBytesEmitted, layout.constantsOffset - layout.regExpCodeOffset);
}

static void recordLimitHits(const LinkedFunctions& functions, const CodeLayout& layout, BuildContext& context)
{
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
    const uint64_t codeSizeLimit = std::min<uint64_t>(context.limit(LimitID::CodeBytes), std::numeric_limits<uint32_t>::max());
    const int64_t functionReach = callReach(LimitID::FunctionCallReach, context);
    const BodyExtents extents = collectBodyExtents(inputs.functions, context);

    Veneers veneers;
    veneers.byBody.grow(inputs.functions.size());
    PlacedCode code;
    Vector<uint32_t> bodyStarts;
    for (;;) {
        code = placeCode(inputs, extents, veneers, context);
        if (code.parts.codeSize > codeSizeLimit) {
            context.reportLimitExceeded(LimitID::CodeBytes, code.parts.codeSize);
            return { };
        }
        bodyStarts = narrowOffsets(code.placement.unitStarts.span(), context);
        if (static_cast<int64_t>(code.parts.regExpCodeEnd) <= functionReach)
            break;
        FarTransferFinder finder(inputs, code, { bodyStarts.span(), safeCast<uint32_t>(code.parts.regExpCodeOffset) }, functionReach);
        bool didAdd = finder.addVeneersOfBodies(veneers, context);
        didAdd |= finder.addVeneersOfSharedCode(veneers);
        if (!didAdd)
            break;
    }

    CodeLayout layout = createLayout(inputs, WTF::move(code), WTF::move(bodyStarts), WTF::move(veneers), context);
    countStatistics(layout, context);
    recordLimitHits(inputs.functions, layout, context);
    return layout;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
