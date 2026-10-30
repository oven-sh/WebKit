/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeTemplates.h"
#include "AOTFlowResults.h"
#include "AOTFlowUnseenCode.h"
#include "AOTFunctionTable.h"
#include "AOTHeat.h"
#include "AOTIRProgram.h"
#include "AOTImage.h"
#include "AOTPhaseTimes.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeTable.h"
#include "AOTSectionWriter.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreads.h"
#include "MarkedBlockCage.h"
#include <algorithm>
#include <array>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::CreatorRecord;
using Typed::FunctionRole;
using Typed::HeatClass;

void reportLinkFailure(LinkFailure failure, FunctionID function, BuildContext& context)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode, .function = function });
        return;
    case LinkFailure::OutOfPCRelativeReach:
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = function });
        return;
    case LinkFailure::OutOfMemory:
        context.reportLimitExceeded(LimitID::VectorBytes, 0, { .function = function });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

size_t OutlinedCode::ownBodiesSize(uint32_t caller) const
{
    size_t size = 0;
    for (uint32_t number : ownBodiesByCaller[caller])
        size += ownBodies.sizeOf(number);
    return size;
}

namespace {

enum class CodeBand : uint8_t {
    Hot,
    Warm,
    RunsOnce,
    Cold,
};

struct CodeOrderKey {
    friend auto operator<=>(const CodeOrderKey&, const CodeOrderKey&) = default;

    CodeBand band;
    uint32_t leader;
    // The leader and what follows it have the greatest. Of those in front of the leader, what leads to it in fewer steps lies nearer.
    uint8_t nearness;
    uint32_t function;
    uint32_t position;
};

} // anonymous namespace

static FunctionID bodyBehind(const Typed::Program& program, FunctionID id)
{
    const Typed::Function& function = program.at(id);
    switch (function.role) {
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
        return function.creator.as<CreatorRecord::Entrypoint>().target;
    case FunctionRole::EntryChecks:
        return function.creator.as<CreatorRecord::EntryChecks>().body;
    case FunctionRole::ParameterWrapper:
        return function.creator.as<CreatorRecord::ParameterWrapper>().body;
    case FunctionRole::ResultWrapper:
        return function.creator.as<CreatorRecord::ResultWrapper>().worker;
    case FunctionRole::CoroutineRamp:
        return function.creator.as<CreatorRecord::Coroutine>().body;
    case FunctionRole::CallSiteVersion: {
        FunctionID proven = function.creator.as<CreatorRecord::Version>().provenVersion;
        return program.at(proven).role == FunctionRole::EntryChecks ? proven : FunctionID();
    }
    case FunctionRole::TableLookup:
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWorker:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static CodeBand bandOf(Typed::Expected<HeatClass> heatClass)
{
    switch (heatClass.predicted()) {
    case HeatClass::Hot:
        return CodeBand::Hot;
    case HeatClass::Warm:
    case HeatClass::Unknown:
        return CodeBand::Warm;
    case HeatClass::RunsOnce:
        return CodeBand::RunsOnce;
    case HeatClass::Cold:
        return CodeBand::Cold;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return CodeBand::Warm;
}

static CodeOrderKey codeOrderKeyOf(FunctionID function, uint32_t position, const Typed::Program& program, const Typed::Heat& heat)
{
    FunctionID leader = function;
    uint8_t nearness = std::numeric_limits<uint8_t>::max();
    for (FunctionID body = bodyBehind(program, leader); body && nearness; body = bodyBehind(program, leader)) {
        --nearness;
        leader = body;
    }
    // An outlined body is shared by sites of several functions, all in cold blocks, so it has no heat of its own.
    CodeBand band = program.at(leader).role == FunctionRole::OutlinedBody ? CodeBand::Cold : bandOf(heat.of(program, leader));
    if (program.at(leader).isModuleBody())
        band = CodeBand::RunsOnce;
    return { band, leader.index(), nearness, function.index(), position };
}

void orderFunctions(LinkedFunctions& functions, const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    Vector<CodeOrderKey> keys(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        keys[position] = codeOrderKeyOf(functions[position].function, position, *pipeline.program, *pipeline.heat);
    });
    uint64_t bytesInHotAndWarmBands = 0;
    uint64_t bytesOfHotBlocksInHotAndWarmBands = 0;
    for (const CodeOrderKey& key : keys) {
        if (key.band != CodeBand::Hot && key.band != CodeBand::Warm)
            continue;
        bytesInHotAndWarmBands += functions[key.position].body.bytes.size();
        bytesOfHotBlocksInHotAndWarmBands += functions[key.position].body.sizeOfHotBlocks;
    }
    context.statistics().count(StatisticID::BytesGeneratedInHotAndWarmBands, bytesInHotAndWarmBands);
    context.statistics().count(StatisticID::BytesGeneratedOutsideColdBlocksInHotAndWarmBands, bytesOfHotBlocksInHotAndWarmBands);
    parallelSort(context.parallelLoops(), keys.mutableSpan(), std::less<CodeOrderKey> { });
    Vector<uint32_t> order = keys.map([](const CodeOrderKey& key) {
        return key.position;
    });
    functions.reorder(order.span(), context);
}

static void reportCalleesWithoutCode(const CompiledCode& code, const Vector<ResolvedCallee, 0>& resolved, BuildContext& context)
{
    BitVector needsCode(resolved.size());
    for (const Relocation& relocation : code.body.relocations) {
        if (relocation.space == SymbolSpace::Function)
            needsCode.set(relocation.symbol);
    }
    for (size_t callee : needsCode) {
        if (!resolved[callee].body)
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToFunctionWithoutCode, .function = code.function, .count = code.body.callees[callee].index() });
    }
}

ResolvedCallees resolveCallees(const LinkedFunctions& functions, const CodeIndex& index, std::span<const EntryAlias> aliases, BuildContext& context)
{
    AliasIndex aliasIndex(aliases, &context.parallelLoops());
    for (auto& alias : aliases)
        RELEASE_ASSERT(alias.function && !index.indexOf(alias.function) && index.indexOf(alias.body));
    ResolvedCallees result;
    result.callees.grow(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& code = functions[position];
        auto& resolved = result.callees[position];
        resolved = WTF::map(code.body.callees, [&](FunctionID callee) -> ResolvedCallee {
            if (auto body = index.indexOf(callee))
                return { .body = { *body } };
            OptionalIndex alias = aliasIndex.aliasOf(callee);
            if (!alias)
                return { };
            return { .body = { *index.indexOf(aliases[*alias].body) }, .alias = alias };
        });
        bool hasCalleeWithoutCode = std::ranges::any_of(resolved, [](const ResolvedCallee& callee) {
            return !callee.body;
        });
        if (hasCalleeWithoutCode)
            reportCalleesWithoutCode(code, resolved, context);
    });
    return result;
}

static Vector<uint32_t, 32> collectOffsetsAskedForFrame(const CompiledCode& code)
{
    Vector<uint32_t, 32> offsets;
    auto addReturnOffset = [&](uint32_t returnOffset) {
        RELEASE_ASSERT(returnOffset);
        offsets.append(returnOffset - 1);
    };
    for (const ReturnSite& site : code.body.returnSites)
        addReturnOffset(site.offset);
    for (const FaultingAccess& access : code.sites.faultingAccesses)
        offsets.append(access.codeOffset);
    for (const PollSite& poll : code.sites.pollSites)
        addReturnOffset(poll.returnOffset());
    AOT::sort(offsets);
    return offsets;
}

static void encodeFrameRanges(Vector<uint8_t>& bytes, const CompiledCode& code)
{
    if (!code.frame.hasFrame())
        return;
    // A function may set its frame up in thousands of places. The offsets ascend, so one walk through the sorted ranges answers for all of them.
    Vector<CodeRange, 1> ranges = code.frame.frameRanges;
    AOT::sortInLine(ranges, { }, &CodeRange::begin);
    size_t nextRange = 0;
    uint32_t endOfRangesPassed = 0;
    auto hasFrameAt = [&](uint32_t offset) {
        for (; nextRange < ranges.size() && ranges[nextRange].begin <= offset; ++nextRange)
            endOfRangesPassed = std::max(endOfRangesPassed, ranges[nextRange].end);
        return offset < endOfRangesPassed;
    };
    bool hasFrame = false;
    uint32_t previousChange = 0;
    uint32_t endOfLastUnitAsked = 0;
    for (uint32_t offset : collectOffsetsAskedForFrame(code)) {
        if (hasFrameAt(offset) != hasFrame) {
            SideTableParts::appendFrameChange(bytes, previousChange, endOfLastUnitAsked);
            previousChange = endOfLastUnitAsked;
            hasFrame = !hasFrame;
        }
        endOfLastUnitAsked = (offset / codeOffsetUnit + 1) * codeOffsetUnit;
    }
}

namespace {

struct SideTablePartSizes {
    uint32_t frameRanges { 0 };
    uint32_t handlers { 0 };
    uint32_t callSites { 0 };
};

} // anonymous namespace

// Equal streams are stored once, in the order of the functions, so a stream is stored where the streams in front of it end.
static void countSideTableParts(std::span<const Vector<uint8_t>> streams, std::span<const uint32_t> offsets, std::span<const SideTablePartSizes> partSizes, BuildContext& context)
{
    uint64_t sizeOfStreams = 0;
    uint64_t sizeOfFrameRanges = 0;
    uint64_t sizeOfHandlers = 0;
    uint64_t sizeOfCallSites = 0;
    for (size_t index = 0; index < streams.size(); ++index) {
        if (streams[index].isEmpty() || offsets[index] != sizeOfStreams)
            continue;
        sizeOfStreams += streams[index].size();
        sizeOfFrameRanges += partSizes[index].frameRanges;
        sizeOfHandlers += partSizes[index].handlers;
        sizeOfCallSites += partSizes[index].callSites;
    }
    Statistics& statistics = context.statistics();
    statistics.count(StatisticID::SideTableBytesFrameRanges, sizeOfFrameRanges);
    statistics.count(StatisticID::SideTableBytesHandlers, sizeOfHandlers);
    statistics.count(StatisticID::CallSiteBytesEmitted, sizeOfCallSites);
    statistics.count(StatisticID::SideTableBytesReferences, sizeOfStreams - sizeOfFrameRanges - sizeOfHandlers - sizeOfCallSites);
}

static Vector<uint8_t> joinPositions(std::span<const Vector<uint8_t>> positions, BuildContext& context)
{
    size_t sizeOfIndex = PositionIndex::numberOfEntriesFor(positions.size()) * sizeof(uint32_t);
    uint64_t size = sizeOfIndex;
    for (const Vector<uint8_t>& bytes : positions)
        size += bytes.size();
    if (!checkSectionSize(size, context))
        return { };

    Vector<uint8_t> result;
    result.reserveInitialCapacity(size);
    result.grow(sizeOfIndex);
    for (size_t function = 0; function < positions.size(); ++function) {
        if (!(function % PositionIndex::functionsBetween)) {
            uint32_t place = static_cast<uint32_t>(result.size() - sizeOfIndex);
            memcpySpan(result.mutableSpan().subspan(function / PositionIndex::functionsBetween * sizeof(uint32_t)), asByteSpan(place));
        }
        result.appendVector(positions[function]);
    }
    return result;
}

SideTables encodeSideTables(const LinkedFunctions& functions, std::span<const Vector<uint8_t>> callSites, BuildContext& context)
{
    RELEASE_ASSERT(callSites.size() == functions.size());
    // Most functions that wait have the frame maps of many others. Equal ones are held once.
    Vector<uint64_t> firstWordOf(functions.size());
    BitVector hasFrameMapsOfEarlierFunction(functions.size());
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> firstWithHashAndSize;
    uint64_t numberOfWords = 0;
    for (size_t position = 0; position < functions.size(); ++position) {
        std::span words = functions[position].sites.frameMapWords.span();
        firstWordOf[position] = numberOfWords;
        if (words.empty())
            continue;
        uint64_t hashAndSize = static_cast<uint64_t>(StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(asBytes(words)))) << 32 | words.size();
        uint32_t first = firstWithHashAndSize.add(hashAndSize, static_cast<uint32_t>(position)).iterator->value;
        if (first != position && equalSpans(functions[first].sites.frameMapWords.span(), words)) {
            firstWordOf[position] = firstWordOf[first];
            hasFrameMapsOfEarlierFunction.quickSet(position);
            continue;
        }
        numberOfWords += words.size();
    }
    static_assert(WTF::isValidCapacityForVector<uint32_t>(defaultLimitValue(LimitID::VectorBytes, hostArchitecture()) / sizeof(uint32_t)));
    if (!checkSectionSize(numberOfWords * sizeof(uint32_t), context))
        return { };

    std::optional<FrameTable> frameTable = collectFrames(functions, context);
    if (!frameTable)
        return { };

    bool hasPositions = !context.options().obfuscates();
    SideTables result;
    result.frames = WTF::move(frameTable->frames);
    result.frameMapWords.grow(numberOfWords);
    result.flags.grow(functions.size());
    Vector<Vector<uint8_t>> streams(functions.size());
    Vector<Vector<uint8_t>> positions(functions.size());
    Vector<SideTablePartSizes> partSizes(functions.size());
    Atomic<uint64_t> numberOfPositions { 0 };
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const SiteDescription& sites = functions[position].sites;
        Vector<uint8_t> frameRanges;
        encodeFrameRanges(frameRanges, functions[position]);
        std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
        if (!sites.frameMapWords.isEmpty()) {
            frameMapRange.emplace(static_cast<uint32_t>(firstWordOf[position]), static_cast<uint32_t>(sites.frameMapWords.size()));
            if (!hasFrameMapsOfEarlierFunction.quickGet(position))
                memcpySpan(result.frameMapWords.mutableSpan().subspan(frameMapRange->first), sites.frameMapWords.span());
        }
        Vector<uint8_t> handlers;
        encodeHandlers(handlers, sites.handlers.span());
        if (hasPositions) {
            encodePositions(positions[position], sites, callSites[position].span().data(), position % PositionIndex::functionsBetween ? OwnPosition::of(functions[position - 1].sites) : OwnPosition { });
            numberOfPositions.exchangeAdd(sites.positions.size(), std::memory_order_relaxed);
        }
        Vector<std::pair<uint32_t, uint32_t>> constructedCallees(sites.constructedCallees.size(), [&](size_t index) {
            const ConstructedCallee& row = sites.constructedCallees[index];
            return std::pair { row.site, functions[position].body.callees[row.callee].index() };
        });
        SideTableInputs inputs {
            .frameRanges = frameRanges.span(),
            .frame = frameTable->frameOf[position],
            .frameMapRange = frameMapRange,
            .handlers = handlers.span(),
            .calleesOfSites = sites.calleesOfSites.span(),
            .constructedCallees = constructedCallees.span(),
            .callSites = callSites[position].span(),
        };
        SideTableParts::append(streams[position], inputs);
        result.flags[position] = inputs.flags();
        partSizes[position] = { static_cast<uint32_t>(frameRanges.size()), static_cast<uint32_t>(handlers.size()), static_cast<uint32_t>(callSites[position].size()) };
    });
    context.statistics().count(StatisticID::SitePositionsEmitted, numberOfPositions.loadRelaxed());

    SectionWriter writer(context);
    result.offsets = writer.appendAll(streams.span());
    countSideTableParts(streams.span(), result.offsets.span(), partSizes.span(), context);
    result.bytes = writer.takeBytes();
    result.positions = joinPositions(positions.span(), context);
    context.destroyLater(WTF::move(streams));
    context.destroyLater(WTF::move(positions));
    return result;
}

static bool hasAddressInImage(StubLinkage linkage)
{
    switch (linkage) {
    case StubLinkage::EnteredByRuntime:
    case StubLinkage::FrontEnd:
    case StubLinkage::Handler:
        return true;
    case StubLinkage::CalledByCode:
    case StubLinkage::InternalEntry:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

static Vector<uint8_t> encodeStubAddresses(const StubBlob& blob)
{
    Vector<StubAddressInImage> addresses;
    for (auto& placement : blob.placements) {
        StubLinkage linkage = descriptorOf(placement.key.family()).linkage;
        // Only needed where a call pushes its return address: there, walking a stopped thread's stack has to look up the target of a call.
        bool usesCallConvention = isX86_64() && linkage == StubLinkage::CalledByCode && signatureOf(placement.key).usesCallConvention;
        if (AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE || usesCallConvention || hasAddressInImage(linkage))
            addresses.append({ placement.key.bits(), placement.offset, usesCallConvention });
    }
    AOT::sortInLine(addresses, [](const StubAddressInImage& a, const StubAddressInImage& b) {
        bool isFirstPart = isFoundByKey(StubKey::fromBits(a.key).family());
        if (isFirstPart != isFoundByKey(StubKey::fromBits(b.key).family()))
            return isFirstPart;
        return a.key < b.key;
    });
    return bytesOf(addresses);
}

static Vector<uint8_t> encodeSlotHandlers(const StubBlob& blob)
{
    Vector<uint32_t> offsets;
    offsets.reserveInitialCapacity(numberOfSlotHandlerTables * numberOfSlotKindsWithHandlers);
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : slotKindsWithHandlers)
            offsets.append(blob.offsetOf(slotHandlerFor(table, kind)).value_or(offsetOfAbsentStub));
    }
    return bytesOf(offsets);
}

OptionSet<ImageFlag> flagsForUnseenCode(const Typed::PipelineOutput& pipeline, const CompileOptions& options)
{
    const Typed::FlowResults& flow = *pipeline.flow;
    if (!flow.hasResults()) {
        OptionSet<ImageFlag> flags { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName, ImageFlag::AdmitsLibraryText, ImageFlag::FindsDataFilesByName };
        // Any load may name a file outside the image, so only the options say whether one is loaded.
        if (!options.excludesCodeOutsideImage())
            flags.add(ImageFlag::LoadsCodeOutsideImage);
        return flags;
    }

    OptionSet<ImageFlag> flags;
    if (flow.findsFilesOfProgramByName())
        flags.add(ImageFlag::FindsDataFilesByName);
    for (const auto& record : flow.unseenCode()) {
        if (Typed::admitsUnknownJavaScript(record.cause))
            flags.add(ImageFlag::AdmitsUnknownJavaScript);
        if (Typed::loadsModulesByName(record.cause))
            flags.add(ImageFlag::LoadsModulesByName);
        if (Typed::admitsLibraryText(record.cause))
            flags.add(ImageFlag::AdmitsLibraryText);
        if (Typed::loadsCodeOutsideImage(record.cause))
            flags.add(ImageFlag::LoadsCodeOutsideImage);
    }
    return flags;
}

namespace {

struct ShapeSetsOfTables {
    Vector<uint8_t> bytes;
    Vector<ConstantRange> parts;
};

struct ImageSections {
    Vector<uint8_t>& operator[](ImageSection section) { return bytes[static_cast<size_t>(section)]; }
    const Vector<uint8_t>& operator[](ImageSection section) const { return bytes[static_cast<size_t>(section)]; }
    const ProgramTables& programTables() const
    {
        RELEASE_ASSERT((*this)[ImageSection::ProgramTables].size() >= sizeof(ProgramTables));
        return *reinterpret_cast<const ProgramTables*>((*this)[ImageSection::ProgramTables].span().data());
    }

    std::array<Vector<uint8_t>, numberOfImageSections> bytes;
};

struct ImageExtent {
    ImageSectionRange operator[](ImageSection section) const { return sections[static_cast<size_t>(section)]; }

    std::array<ImageSectionRange, numberOfImageSections> sections;
    uint64_t codeOffset { 0 };
    uint64_t size { 0 };
};

struct TextRecords {
    std::span<const ImageString> strings;
    std::span<const ImageLongString> longStrings;
    std::span<const ImageIdentifier> identifiers;
};

struct StringHeaderNumbers {
    bool hasRecords() const { return form == ImageForm::PositionIndependent; }
    uint64_t sizeOfHeaders() const { return static_cast<uint64_t>(count) * (hasRecords() ? sizeof(ImageStringRecord) : sizeof(ImageStringHeader)); }
    uint64_t sizeOfList() const { return static_cast<uint64_t>(numberOfListedWords) * sizeof(uint32_t); }

    ImageForm form { ImageForm::Absolute };
    // The number of the header of a string that is no identifier's text, or the first word in the image's list of a string without a header.
    Vector<uint32_t> byString;
    Vector<uint32_t> listedWordIndexes;
    Vector<uint32_t> literalStringWords;
    uint32_t count { 0 };
    uint32_t numberOfListedWords { 0 };
};

struct ImageWriterInputs {
    const ImageInputs& inputs;
    const CodeLinkerInputs& linker;
    const ImageSections& sections;
    const WTF::BitSet<numberOfEntries>& entriesUsed;
    ImageCounts counts;
    std::span<const StubReturnSite> stubReturnSites;
};

} // anonymous namespace

// The sets of shapes that stubs and C++ read are the last thing in the program tables.
static ShapeSetsOfTables takeShapeSets(Vector<uint8_t>& programTables, const Typed::ShapeTable& shapes)
{
    RELEASE_ASSERT(programTables.size() >= sizeof(ProgramTables));
    auto& tables = *reinterpret_cast<ProgramTables*>(programTables.mutableSpan().data());
    TableRange& range = tables.ranges[static_cast<unsigned>(ProgramTableIndex::shapeSetBits)];
    ShapeSetsOfTables sets;
    if (!range.count)
        return sets;
    std::span<const uint8_t> bytes = asBytes(tables.shapeSetBits());
    RELEASE_ASSERT(bytes.size() == shapes.shapeSetWords().size_bytes());
    if (range.offset + bytes.size() != programTables.size())
        return sets;
    sets.bytes = Vector<uint8_t>(bytes);
    sets.parts = Vector<ConstantRange>(shapes.shapeSetRanges().size(), [&](size_t index) {
        Typed::ShapeSetRange set = shapes.shapeSetRanges()[index];
        return ConstantRange { static_cast<uint32_t>(set.firstWord * sizeof(uint64_t)), static_cast<uint32_t>(set.numberOfWords * sizeof(uint64_t)) };
    });
    uint32_t offset = range.offset;
    range = { 0, 0 };
    programTables.shrink(offset);
    return sets;
}

static std::optional<StringHeaderNumbers> numberStringHeaders(const TextRecords& text, std::span<const uint32_t> literalStrings, ImageForm form, BuildContext& context)
{
    StringHeaderNumbers numbers;
    numbers.form = form;
    numbers.byString.fill(0, text.strings.size());
    if (numbers.hasRecords()) {
        numbers.listedWordIndexes.fill(0, text.strings.size());
        numbers.literalStringWords.fill(0, text.strings.size());
        for (uint32_t number = 0; number < literalStrings.size(); ++number)
            numbers.literalStringWords[literalStrings[number]] = literalStringWord(number);
    }
    uint64_t count = text.identifiers.size();
    uint64_t numberOfListedWords = 0;
    for (size_t index = 0; index < text.strings.size(); ++index) {
        const ImageString& string = text.strings[index];
        bool needsRecord = numbers.hasRecords() && string.hasHeader() && string.range(text.longStrings).length;
        if (needsRecord && (numbers.literalStringWords[index] || !ImageListedString::fitsOneWord(string.range(text.longStrings)))) {
            numbers.listedWordIndexes[index] = static_cast<uint32_t>(numberOfListedWords);
            numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + !!numbers.literalStringWords[index];
        }
        if (!string.hasText() || string.hasIdentifierNumber())
            continue;
        if (string.hasHeader()) {
            numbers.byString[index] = static_cast<uint32_t>(count++);
            continue;
        }
        numbers.byString[index] = static_cast<uint32_t>(numberOfListedWords);
        numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + string.hasListedHash();
    }
    // Not context.limit(): a test that lowers it means the list of long strings.
    static_assert(defaultLimitValue(LimitID::MaxListedStrings, hostArchitecture()) == ImageString::hashMask + 1);
    if (std::max(count, numberOfListedWords) > ImageString::hashMask + 1) {
        context.reportLimitExceeded(LimitID::MaxListedStrings, std::max(count, numberOfListedWords));
        return std::nullopt;
    }
    numbers.count = static_cast<uint32_t>(count);
    numbers.numberOfListedWords = static_cast<uint32_t>(numberOfListedWords);
    return numbers;
}

static std::optional<ImageExtent> computeImageExtent(const ImageSections& sections, const StringHeaderNumbers& stringHeaders, uint64_t codeSize, BuildContext& context)
{
    RELEASE_ASSERT(sections[ImageSection::StringHeaders].isEmpty());
    ImageExtent extent;
    uint64_t tablesEnd = sizeof(ImageHeader);
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        uint64_t size = sections.bytes[index].size();
        if (static_cast<ImageSection>(index) == ImageSection::StringHeaders)
            size = stringHeaders.sizeOfHeaders();
        else if (static_cast<ImageSection>(index) == ImageSection::Strings)
            size = size / sizeof(ImageString) * sizeof(ImageStringWord);
        else if (static_cast<ImageSection>(index) == ImageSection::LongStrings)
            size = stringHeaders.sizeOfList();
        else if (static_cast<ImageSection>(index) == ImageSection::Identifiers)
            size = size / sizeof(ImageIdentifier) * sizeof(ImageIdentifierWord);
        tablesEnd = WTF::roundUpToMultipleOf<imageSectionAlignment>(tablesEnd);
        if (static_cast<ImageSection>(index) == ImageSection::MethodTable && size)
            tablesEnd = WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd);
        // An offset that does not fit is never read: the image is then too large for one of the two tests below.
        extent.sections[index] = { static_cast<uint32_t>(tablesEnd), static_cast<uint32_t>(size) };
        tablesEnd += size;
    }
    if (tablesEnd > context.limit(LimitID::TableBytes)) {
        context.reportLimitExceeded(LimitID::TableBytes, tablesEnd);
        return std::nullopt;
    }
    extent.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd);
    extent.size = WTF::roundUpToMultipleOf<imagePageSize>(extent.codeOffset + codeSize);
    static_assert(WTF::isValidCapacityForVector<uint8_t>(defaultLimitValue(LimitID::VectorBytes, hostArchitecture())));
    if (extent.size > context.limit(LimitID::VectorBytes)) {
        context.reportLimitExceeded(LimitID::VectorBytes, extent.size);
        return std::nullopt;
    }
    return extent;
}

// The statistics of the sections add up to TableBytesEmitted: the header and the padding count as other bytes.
static void countTableBytes(const ImageExtent& extent, BuildContext& context)
{
    static constexpr StatisticID statistics[] = {
#define AOT_STATISTIC_OF_SECTION(name, Element, statistic) StatisticID::statistic,
        FOR_EACH_AOT_IMAGE_SECTION(AOT_STATISTIC_OF_SECTION)
#undef AOT_STATISTIC_OF_SECTION
    };
    uint64_t sizeOfSections = 0;
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        context.statistics().count(statistics[index], extent.sections[index].size);
        sizeOfSections += extent.sections[index].size;
    }
    context.statistics().count(StatisticID::OtherTableBytesEmitted, extent.codeOffset - sizeOfSections);
    context.statistics().count(StatisticID::TableBytesEmitted, extent.codeOffset);
}

static constexpr size_t bytesPerChunk = MB;

static Vector<uint8_t> allocateImage(const ImageExtent& extent, size_t executableSize, const CodeTemplates& templates, const ParallelLoops& loops)
{
    static_assert(!(bytesPerChunk % codeOffsetUnit) && !(imagePageSize % codeOffsetUnit));
    Vector<uint8_t> image;
    image.grow(extent.size);
    forEachIndexInParallel(loops, static_cast<unsigned>((image.size() + bytesPerChunk - 1) / bytesPerChunk), [&](unsigned i) {
        size_t start = i * bytesPerChunk;
        size_t end = std::min(start + bytesPerChunk, image.size());
        size_t codeStart = std::clamp<size_t>(extent.codeOffset, start, end);
        size_t codeEnd = std::clamp<size_t>(extent.codeOffset + executableSize, start, end);
        zeroSpan(image.mutableSpan().subspan(start, codeStart - start));
        templates.fillWithBreakpoints(image.mutableSpan().subspan(codeStart, codeEnd - codeStart));
        zeroSpan(image.mutableSpan().subspan(codeEnd, end - codeEnd));
    });
    return image;
}

template<typename T>
static constexpr bool hasNoPadding = std::has_unique_object_representations_v<T>;

static void writeHeader(ImageHeader& header, const ImageWriterInputs& parts, const ImageExtent& extent, BuildContext& context)
{
    static_assert(hasNoPadding<RecordedOptions> && hasNoPadding<ImageSectionRange> && hasNoPadding<ImageCounts> && hasNoPadding<ImageCodeRanges>, "padding in a member assigned as a whole would make the bytes of the image differ from build to build");
    static_assert(sizeof(ImageHeapOptions) == sizeof(double) + 2 * sizeof(uint32_t), "a double has no unique object representation, so the sizes say that there is no padding");
    const CodeLayout& layout = parts.linker.layout;
    header.magic = imageMagic;
    header.identity = context.options().executableIdentity;
    header.stamp = imageStamp(context.targetFeatures(), header.identity, context.recordedOptions().form());
    header.size = extent.size;
    header.codeOffset = extent.codeOffset;
    header.codeSize = layout.codeSize;
    header.address = 0;
    header.executableSize = layout.executableSize;
    header.formatVersion = imageFormatVersion;
    header.options = context.recordedOptions();
    header.heap = { context.options().sizeClassProgression, MarkedBlockCage::shift, 0 };
    std::ranges::copy(extent.sections, header.sections);
    header.counts = parts.counts;
    header.code = {
        .functionsEnd = layout.functionsEnd,
        .codeGranuleShift = layout.codeGranuleShift,
        .sharedCodeSize = layout.sharedCodeSize,
        .webAssemblyCodeOffset = layout.webAssemblyCodeOffset,
        .webAssemblyCodeSize = safeCast<uint32_t>(parts.inputs.webAssembly.code.size()),
        .ffiCodeOffset = layout.ffiCodeOffset,
        .ffiCodeSize = safeCast<uint32_t>(parts.inputs.ffiStubs.code.size()),
        .regExpCodeOffset = layout.regExpCodeOffset,
        .constantsOffset = layout.constantsOffset,
        .constantsSize = safeCast<uint32_t>(parts.linker.constants.bytes.size()),
    };
    OptionSet<ImageFlag> flags = flagsForUnseenCode(parts.inputs.pipeline, context.options());
    if (context.options().obfuscates())
        flags.add(ImageFlag::IsObfuscated);
    if (Typed::namesWaysToModuleObjects(*parts.inputs.pipeline.program))
        flags.add(ImageFlag::ShowsWaysBetweenModuleObjects);
    header.flags = flags.toRaw();
    for (size_t entry : parts.entriesUsed)
        header.entriesUsed[entry / 32] |= 1u << entry % 32;
    for (const StubReturnSite& site : parts.stubReturnSites) {
        if (!hasSingleReturnSite(site.kind))
            continue;
        uint32_t& offset = header.singleReturnSites[static_cast<unsigned>(site.kind)];
        RELEASE_ASSERT(!offset && site.offset);
        offset = site.offset;
    }
    header.numberOfStubsFoundByKey = static_cast<uint32_t>(std::ranges::count_if(spanReinterpretCast<const StubAddressInImage>(parts.sections[ImageSection::StubAddresses].span()), [](const StubAddressInImage& address) {
        return isFoundByKey(StubKey::fromBits(address.key).family());
    }));
}

static void copySections(std::span<uint8_t> image, const ImageWriterInputs& parts, const ImageExtent& extent, const ParallelLoops& loops)
{
    struct CopyRange {
        size_t offset;
        std::span<const uint8_t> source;
    };
    const CodeLayout& layout = parts.linker.layout;
    Vector<CopyRange> chunks;
    auto addChunks = [&](uint64_t offset, std::span<const uint8_t> source) {
        RELEASE_ASSERT(isSumSmallerThanOrEqual<uint64_t>(offset, source.size(), image.size()));
        for (size_t start = 0; start < source.size(); start += bytesPerChunk)
            chunks.append({ static_cast<size_t>(offset) + start, source.subspan(start, std::min(bytesPerChunk, source.size() - start)) });
    };
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        // writeStrings() makes the three of the encoder's records.
        if (static_cast<ImageSection>(index) != ImageSection::Strings && static_cast<ImageSection>(index) != ImageSection::LongStrings && static_cast<ImageSection>(index) != ImageSection::Identifiers)
            addChunks(extent.sections[index].offset, parts.sections.bytes[index].span());
    }
    addChunks(extent.codeOffset + layout.regExpCodeOffset, parts.linker.layoutInputs.regExpSection.bytes.span());
    addChunks(extent.codeOffset + layout.webAssemblyCodeOffset, parts.inputs.webAssembly.code.span());
    addChunks(extent.codeOffset + layout.ffiCodeOffset, parts.inputs.ffiStubs.code.span());
    addChunks(extent.codeOffset + layout.ffiDirectoryOffset, parts.inputs.ffiStubs.directory.span());
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned index) {
        memcpySpan(image.subspan(chunks[index].offset), chunks[index].source);
    });
}

template<typename T>
static std::span<T> sectionIn(std::span<uint8_t> image, const ImageExtent& extent, ImageSection section)
{
    return spanReinterpretCast<T>(image.subspan(extent[section].offset, extent[section].size));
}

static void writeStrings(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent, const ParallelLoops& loops)
{
    std::span<ImageStringRecord> records;
    std::span<ImageStringHeader> headers;
    if (numbers.hasRecords())
        records = sectionIn<ImageStringRecord>(image, extent, ImageSection::StringHeaders);
    else
        headers = sectionIn<ImageStringHeader>(image, extent, ImageSection::StringHeaders);
    std::span stringsInImage = sectionIn<ImageStringWord>(image, extent, ImageSection::Strings);
    std::span listedStrings = sectionIn<uint32_t>(image, extent, ImageSection::LongStrings);
    std::span identifiersInImage = sectionIn<ImageIdentifierWord>(image, extent, ImageSection::Identifiers);
    for (size_t number = 0; number < text.identifiers.size(); ++number)
        identifiersInImage[number] = text.identifiers[number].inImage();
    forEachIndexInParallel(loops, static_cast<unsigned>(text.strings.size()), [&](unsigned index) {
        const ImageString& string = text.strings[index];
        stringsInImage[index] = string.inImage(numbers.byString[index]);
        if (!string.hasText())
            return;
        ImageLongString range = string.range(text.longStrings);
        if (!string.hasHeader()) {
            ImageListedString::write(listedStrings.subspan(numbers.byString[index]), range);
            if (string.hasListedHash())
                listedStrings[numbers.byString[index] + ImageListedString::numberOfWords(range)] = string.hashOrIdentifierNumber();
            return;
        }
        bool isIdentifier = string.hasIdentifierNumber();
        uint32_t number = isIdentifier ? string.hashOrIdentifierNumber() : numbers.byString[index];
        if (isIdentifier)
            RELEASE_ASSERT(text.identifiers[number].kind() == PropertyKeyKind::Text && text.identifiers[number].text() == StringID(index));
        uint32_t hash = isIdentifier ? text.identifiers[number].hashOrSymbol() : string.hashOrIdentifierNumber();
        if (!numbers.hasRecords()) {
            // The atom with no characters is the engine's own.
            if (range.length || !isIdentifier)
                headers[number] = ImageStringHeader::create(static_cast<uint64_t>(extent[ImageSection::Text].offset) + range.offset, range.length, hash, string.is16Bit(), isIdentifier ? StringImpl::StringAtom : StringImpl::StringNormal);
            return;
        }
        if (!range.length)
            return;
        uint32_t literalStringWord = numbers.literalStringWords[index];
        uint32_t rangeOrIndex = numbers.listedWordIndexes[index] << ImageString::lengthBits;
        if (!literalStringWord && ImageListedString::fitsOneWord(range))
            ImageListedString::write(std::span { &rangeOrIndex, 1 }, range);
        else {
            std::span words = listedStrings.subspan(numbers.listedWordIndexes[index]);
            ImageListedString::write(words, range);
            if (literalStringWord)
                words[ImageListedString::numberOfWords(range)] = literalStringWord;
        }
        records[number] = ImageStringRecord::create(rangeOrIndex, hash, string.is16Bit(), isIdentifier);
        if (literalStringWord)
            records[number].hashAndFlags |= ImageStringRecord::isLiteralStringFlag;
    });
}

static void writeLiteralStringWords(std::span<ImageStringHeader> headers, const TextRecords& text, const StringHeaderNumbers& numbers, std::span<const uint32_t> literalStrings)
{
    RELEASE_ASSERT(!headers.empty() && !headers[0].length);
    headers[0].literalStringWord = static_cast<uint32_t>(headers.size_bytes());
    for (uint32_t number = 0; number < literalStrings.size(); ++number) {
        const ImageString& string = text.strings[literalStrings[number]];
        if (!string.hasText() || !string.hasHeader())
            continue;
        ImageStringHeader& header = headers[string.hasIdentifierNumber() ? string.hashOrIdentifierNumber() : numbers.byString[literalStrings[number]]];
        if (header.length)
            header.literalStringWord = literalStringWord(number);
    }
}

static void writeRangeOfShapeSets(std::span<uint8_t> image, const CodeLinkerInputs& linker, const ImageExtent& extent)
{
    uint32_t size = linker.constants.sizeOfSharedConstants;
    if (!size)
        return;
    ImageSectionRange section = extent[ImageSection::ProgramTables];
    RELEASE_ASSERT(section.size >= sizeof(ProgramTables));
    auto& tables = *reinterpret_cast<ProgramTables*>(image.subspan(section.offset).data());
    uint64_t distance = extent.codeOffset + linker.layout.constantsOffset - section.offset;
    RELEASE_ASSERT(distance <= std::numeric_limits<uint32_t>::max() && !(distance % sizeof(uint64_t)));
    tables.ranges[static_cast<unsigned>(ProgramTableIndex::shapeSetBits)] = { static_cast<uint32_t>(distance), static_cast<uint32_t>(size / sizeof(uint64_t)) };
}

// The pages of an image are shared between processes: none is written, and none but those of the string headers holds an address (see ImageAddressPlaces). The run time maps memory of its own over the callback context.
static void checkImageIsReadOnly(std::span<const uint8_t> code, const FFIStubs& ffiStubs, const CodeLayout& layout, BuildContext& context)
{
    uint64_t offset = static_cast<uint64_t>(layout.ffiCodeOffset) + ffiStubs.callbackContextOffset;
    uint64_t size = ffiStubs.callbackContextSize;
    if (!size)
        return;
    bool isEmptyPageRange = !(offset % imagePageSize) && !(size % imagePageSize) && isSumSmallerThanOrEqual<uint64_t>(offset, size, code.size())
        && std::ranges::all_of(code.subspan(offset, size), [](uint8_t byte) { return !byte; });
    if (!isEmptyPageRange)
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::WritableSectionInImage, .count = clampTo<uint32_t>(offset) });
}

static Vector<uint8_t> writeImage(const ImageWriterInputs& parts, BuildContext& context)
{
    const CodeLayout& layout = parts.linker.layout;
    const ImageSections& sections = parts.sections;
    TextRecords text {
        spanReinterpretCast<const ImageString>(sections[ImageSection::Strings].span()),
        spanReinterpretCast<const ImageLongString>(sections[ImageSection::LongStrings].span()),
        spanReinterpretCast<const ImageIdentifier>(sections[ImageSection::Identifiers].span()),
    };
    std::span<const uint32_t> literalStrings = sections.programTables().literalStrings();
    std::optional<StringHeaderNumbers> stringHeaders = numberStringHeaders(text, literalStrings, context.recordedOptions().form(), context);
    if (!stringHeaders)
        return { };
    std::optional<ImageExtent> extent = computeImageExtent(sections, *stringHeaders, layout.codeSize, context);
    if (!extent)
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<uint8_t> image = allocateImage(*extent, layout.executableSize, parts.linker.layoutInputs.templates, loops);
    writeHeader(*reinterpret_cast<ImageHeader*>(image.mutableSpan().data()), parts, *extent, context);
    runTasksInParallel(loops, {
        [&] {
            copySections(image.mutableSpan(), parts, *extent, loops);
            writeStrings(image.mutableSpan(), text, *stringHeaders, *extent, loops);
            if (!stringHeaders->hasRecords())
                writeLiteralStringWords(sectionIn<ImageStringHeader>(image.mutableSpan(), *extent, ImageSection::StringHeaders), text, *stringHeaders, literalStrings);
            writeRangeOfShapeSets(image.mutableSpan(), parts.linker, *extent);
        },
        [&] { linkCode(parts.linker, image.mutableSubspan(extent->codeOffset, layout.codeSize), context); },
    });
    checkImageIsReadOnly(image.subspan(extent->codeOffset, layout.codeSize), parts.inputs.ffiStubs, layout, context);
    if (context.diagnostics().hasErrors())
        return { };
    countTableBytes(*extent, context);
    return image;
}

BuiltImage buildImage(ImageInputs&& inputs, BuildContext& context)
{
    Diagnostics& diagnostics = context.diagnostics();
    const ParallelLoops& loops = context.parallelLoops();
    PhaseTimes times(context, Stage::BuildImage);
    BuiltImage result;

    std::expected<CodeTemplates, LinkFailure> createdTemplates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!createdTemplates) {
        reportLinkFailure(createdTemplates.error(), { }, context);
        return result;
    }
    const CodeTemplates& templates = *createdTemplates;
    result.functions = WTF::move(inputs.functions);
    const LinkedFunctions& functions = result.functions;
    times.didFinish("create code templates"_s);

    orderFunctions(result.functions, inputs.pipeline, context);
    times.didFinish("order functions"_s);
    removeJumpOnlyFunctions(result.functions, result.aliases, templates, context);
    times.didFinish("remove jump-only functions"_s);
    result.foldedFunctions = foldIdenticalFunctions(result.functions, result.aliases, *inputs.pipeline.program, context);
    times.didFinish("fold identical functions"_s);
    CodeIndex index(functions.span(), &loops);
    ResolvedCallees callees = resolveCallees(functions, index, result.aliases.span(), context);
    times.didFinish("resolve callees"_s);
    if (diagnostics.hasErrors())
        return result;

    unsigned numberOfSharedCodeCopies = countSharedCodeCopies(functions, context);
    times.didFinish("count shared code copies"_s);
    ShapeSetsOfTables shapeSets = takeShapeSets(inputs.programTables, inputs.pipeline.placement->shapes());
    runTasksInParallel(loops, {
        [&] { checkUncheckedStackUsage(functions, callees, result.aliases.span(), index, context); },
        [&] {
            if (std::optional<RegExpSection> section = layOutRegExpSection(inputs.regExps.results.span(), context.assemblerPolicy(), templates, &context))
                result.regExpSection = WTF::move(*section);
        },
        [&] { result.outlining = chooseOutlinedSequences({ functions, callees, result.aliases.span(), templates, numberOfSharedCodeCopies }, context); },
        [&] { result.constants = buildConstantPool(functions, context, { shapeSets.bytes.span(), shapeSets.parts.span() }); },
    });
    times.didFinish("check stack usage, lay out regular expressions, choose outlined sequences, build constant pool"_s);
    if (diagnostics.hasErrors())
        return result;

    result.outlinedCode = applyOutlining(result.functions, result.aliases, { index, callees, result.outlining, templates }, context);
    times.didFinish("apply outlining"_s);
    unsigned codeGranuleShift = chooseCodeGranuleShift(functions, context);
    times.didFinish("choose code granule shift"_s);
    Vector<FinalJump> finalJumps;
    runTasksInParallel(loops, {
        [&] { result.sharedCode = selectSharedCode(functions, result.outlinedCode, inputs.stubCatalog, inputs.entriesUsedByFunctions, context); },
        [&] { finalJumps = findFinalJumps(functions, callees, result.outlinedCode, context); },
    });
    times.didFinish("select shared code, find final jumps"_s);
    if (diagnostics.hasErrors())
        return result;

    CodeLayoutInputs layoutInputs {
        .functions = functions,
        .callees = callees,
        .aliases = result.aliases.span(),
        .outlinedCode = result.outlinedCode,
        .sharedCode = result.sharedCode,
        .regExpSection = result.regExpSection,
        .templates = templates,
        .finalJumps = finalJumps.span(),
        .codeGranuleShift = codeGranuleShift,
        .webAssemblyCodeSize = inputs.webAssembly.code.size(),
        .webAssemblyCodeAlignment = inputs.webAssembly.codeAlignment,
        .constantsSize = result.constants.bytes.size(),
        .ffiCodeSize = inputs.ffiStubs.code.size(),
        .ffiCodeAlignment = inputs.ffiStubs.codeAlignment,
        .ffiDirectorySize = inputs.ffiStubs.directory.size(),
    };
    result.layout = layOutCode(layoutInputs, context);
    times.didFinish("lay out code"_s);
    if (diagnostics.hasErrors())
        return result;
    removeFinalJumps(result.functions, result.layout.fallsIntoNext, finalJumps.span(), context);
    times.didFinish("remove final jumps"_s);

    Vector<Vector<uint8_t>> callSites = encodeCallSites(functions, reinterpret_cast<const ProgramTables*>(inputs.programTables.span().data())->numberOfFunctions(), context);
    times.didFinish("encode call sites"_s);
    if (diagnostics.hasErrors())
        return result;
    SideTables sideTables;
    RegExpSections regExps;
    ImageSections sections;
    runTasksInParallel(loops, {
        [&] { sideTables = encodeSideTables(functions, callSites.span(), context); },
        [&] { sections[ImageSection::FaultSites] = encodeFaultSites(functions, result.layout.bodyStarts.span(), context); },
        [&] { regExps = encodeRegExpRecords({ inputs.regExpRequests, inputs.regExps, result.regExpSection }, context); },
    });
    times.didFinish("encode side tables, fault sites, regular expression records"_s);
    if (diagnostics.hasErrors())
        return result;
    sections[ImageSection::ProgramTables] = WTF::move(inputs.programTables);
    EncodedFunctionTables functionTables = encodeFunctionTables({ functions, result.aliases, result.layout, index, sideTables.flags.span(), sideTables.offsets.span(), sections.programTables(), !context.options().obfuscates() }, context);
    context.destroyLater(WTF::move(callSites));
    times.didFinish("encode function tables"_s);
    if (diagnostics.hasErrors())
        return result;

    const StubBlob& stubs = result.sharedCode.blob;
    sections[ImageSection::Functions] = WTF::move(functionTables.functions);
    sections[ImageSection::FunctionStarts] = WTF::move(functionTables.functionStarts);
    sections[ImageSection::CodeGranules] = WTF::move(functionTables.codeGranules);
    sections[ImageSection::InnerEntries] = WTF::move(functionTables.innerEntries);
    sections[ImageSection::Frames] = bytesOf(sideTables.frames);
    sections[ImageSection::FrameMaps] = bytesOf(sideTables.frameMapWords);
    sections[ImageSection::ClosureWords] = WTF::move(functionTables.closureWords);
    sections[ImageSection::Positions] = WTF::move(sideTables.positions);
    sections[ImageSection::SideTables] = WTF::move(sideTables.bytes);
    sections[ImageSection::SharedCodeCopies] = bytesOf(Vector<uint32_t>(result.layout.sharedCodeCopies));
    sections[ImageSection::Strings] = WTF::move(inputs.text.strings);
    sections[ImageSection::LongStrings] = WTF::move(inputs.text.longStrings);
    sections[ImageSection::Text] = WTF::move(inputs.text.text);
    sections[ImageSection::Identifiers] = WTF::move(inputs.text.identifiers);
    sections[ImageSection::StringConstants] = WTF::move(inputs.text.stringConstants);
    sections[ImageSection::RegExps] = WTF::move(regExps.records);
    sections[ImageSection::RegExpHashes] = WTF::move(regExps.hashes);
    sections[ImageSection::RegExpDetails] = WTF::move(regExps.details);
    sections[ImageSection::WebAssemblyDirectory] = WTF::move(inputs.webAssembly.directory);
    sections[ImageSection::StubAddresses] = encodeStubAddresses(stubs);
    sections[ImageSection::StubReturnSites] = bytesOf(stubs.returnSites);
    sections[ImageSection::SlotHandlers] = encodeSlotHandlers(stubs);
    sections[ImageSection::MethodLookups] = WTF::move(functionTables.methodLookups);
    sections[ImageSection::MethodTable] = WTF::move(functionTables.methodTable);
    result.entriesUsed = inputs.entriesUsedByFunctions;
    result.entriesUsed.merge(stubs.entriesUsed);
    times.didFinish("collect sections"_s);

    CodeLinkerInputs linker { layoutInputs, result.layout, result.outlining, result.constants, inputs.instanceSizes };
    result.image = writeImage({ inputs, linker, sections, result.entriesUsed, { static_cast<uint32_t>(functions.size()), regExps.maxOutputSize }, stubs.returnSites.span() }, context);
    if (diagnostics.hasErrors())
        result.image.clear();
    times.didFinish("write image"_s);

    context.statistics().count(StatisticID::ImageFunctionsEmitted, functions.size());
    context.destroyLater(std::tuple { WTF::move(callees), WTF::move(sections), WTF::move(finalJumps) });
    times.didFinish("release intermediate results"_s);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
