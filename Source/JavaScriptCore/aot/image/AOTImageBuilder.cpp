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
#include "AOTCallSiteTable.h"
#include "AOTCodeIndex.h"
#include "AOTCodeTemplates.h"
#include "AOTFFIStubs.h"
#include "AOTFaultTable.h"
#include "AOTFlowResults.h"
#include "AOTFlowUnseenCode.h"
#include "AOTFunctionTable.h"
#include "AOTHandlerTable.h"
#include "AOTHeat.h"
#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTPositionTable.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRecordEncoder.h"
#include "AOTRuntimeTable.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>
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

enum class GroupSide : uint8_t {
    BeforeLeader,
    FromLeader,
};

struct CodeOrderKey {
    friend auto operator<=>(const CodeOrderKey&, const CodeOrderKey&) = default;

    CodeBand band;
    uint32_t leader;
    GroupSide side;
    // Of those in front of the leader, what leads to it in fewer steps lies nearer.
    uint8_t nearness;
    uint32_t function;
    uint32_t position;
};

} // anonymous namespace

// What the engine has put in front of a body does little and goes on into the body: it lies right in front of it, on the same lines and the same page.
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
        // An entrance with fewer tests.
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
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static CodeBand bandOf(HeatClass heatClass)
{
    switch (heatClass) {
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
    GroupSide side = GroupSide::FromLeader;
    uint8_t nearness = std::numeric_limits<uint8_t>::max();
    for (FunctionID body = bodyBehind(program, leader); body && nearness; body = bodyBehind(program, leader)) {
        side = GroupSide::BeforeLeader;
        --nearness;
        leader = body;
    }
    // An outlined body is shared by sites of several functions, all in cold blocks, so it has no heat of its own.
    CodeBand band = program.at(leader).role == FunctionRole::OutlinedBody ? CodeBand::Cold : bandOf(heat.of(program, leader));
    if (program.at(leader).isModuleBody())
        band = CodeBand::RunsOnce;
    return { band, leader.index(), side, nearness, function.index(), position };
}

void orderFunctions(LinkedFunctions& functions, const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    const Typed::Program& program = *pipeline.program;
    const Typed::Heat& heat = *pipeline.heat;
    Vector<CodeOrderKey> keys(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        keys[position] = codeOrderKeyOf(functions[position].function, position, program, heat);
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

namespace {

class CalleeResolver {
public:
    CalleeResolver(const CodeIndex&, std::span<const EntryAlias>);

    ResolvedCallee resolve(FunctionID) const;

private:
    const CodeIndex& m_index;
    std::span<const EntryAlias> m_aliases;
    AliasIndex m_aliasIndex;
};

} // anonymous namespace

CalleeResolver::CalleeResolver(const CodeIndex& index, std::span<const EntryAlias> aliases)
    : m_index(index)
    , m_aliases(aliases)
    , m_aliasIndex(aliases)
{
    for (auto& alias : aliases)
        RELEASE_ASSERT(alias.function && !index.indexOf(alias.function) && index.indexOf(alias.body));
}

ResolvedCallee CalleeResolver::resolve(FunctionID callee) const
{
    if (auto body = m_index.indexOf(callee))
        return { .body = { *body } };
    OptionalIndex alias = m_aliasIndex.aliasOf(callee);
    if (!alias)
        return { };
    return { .body = { *m_index.indexOf(m_aliases[*alias].body) }, .alias = alias };
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
    CalleeResolver resolver(index, aliases);
    ResolvedCallees result;
    result.callees.grow(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& code = functions[position];
        auto& resolved = result.callees[position];
        resolved = WTF::map(code.body.callees, [&](FunctionID callee) {
            return resolver.resolve(callee);
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
        addReturnOffset(access.codeOffset + static_cast<uint32_t>(sizeOfNearCall));
    for (const PollSite& poll : code.sites.pollSites)
        addReturnOffset(poll.returnOffset());
    AOT::sort(offsets);
    return offsets;
}

static void encodeFrameRanges(Vector<uint8_t>& bytes, const CompiledCode& code)
{
    if (!code.frame.hasFrame())
        return;
    bool hasFrame = false;
    uint32_t previousChange = 0;
    uint32_t endOfLastUnitAsked = 0;
    for (uint32_t offset : collectOffsetsAskedForFrame(code)) {
        if (code.frame.hasFrameAt(offset) != hasFrame) {
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
    Vector<uint64_t> firstWordOf(functions.size());
    uint64_t numberOfWords = 0;
    for (size_t position = 0; position < functions.size(); ++position) {
        firstWordOf[position] = numberOfWords;
        numberOfWords += functions[position].sites.frameMapWords.size();
    }
    uint64_t sizeOfFrameMaps = numberOfWords * sizeof(uint32_t);
    static_assert(WTF::isValidCapacityForVector<uint32_t>(defaultLimitValue(LimitID::VectorBytes, hostArchitecture()) / sizeof(uint32_t)));
    if (!checkSectionSize(sizeOfFrameMaps, context))
        return { };

    std::optional<FrameTable> frameTable = collectFrames(functions, context);
    if (!frameTable)
        return { };

    bool hasPositions = !context.options().obfuscates();
    SideTables result;
    result.frames = WTF::move(frameTable->frames);
    result.frameMapWords.grow(numberOfWords);
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
            memcpySpan(result.frameMapWords.mutableSpan().subspan(frameMapRange->first), sites.frameMapWords.span());
        }
        Vector<uint8_t> handlers;
        encodeHandlers(handlers, sites.handlers.span());
        if (hasPositions) {
            encodePositions(positions[position], sites, position % PositionIndex::functionsBetween ? OwnPosition::of(functions[position - 1].sites) : OwnPosition { });
            numberOfPositions.exchangeAdd(sites.positions.size(), std::memory_order_relaxed);
        }
        SideTableParts::append(streams[position], {
            .frameRanges = frameRanges.span(),
            .frame = frameTable->frameOf[position],
            .frameMapRange = frameMapRange,
            .handlers = handlers.span(),
            .callSites = callSites[position].span(),
        });
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
        // A report of poisoned memory names the stub it comes from.
        if (AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE || hasAddressInImage(descriptorOf(placement.key.family()).linkage))
            addresses.append({ placement.key.bits(), placement.offset, 0 });
    }
    AOT::sort(addresses, { }, &StubAddressInImage::key);
    return bytesOf(addresses);
}

static Vector<uint8_t> encodeSlotHandlers(const StubBlob& blob)
{
    Vector<uint32_t> offsets;
    offsets.reserveInitialCapacity(numberOfSlotHandlerTables * numberOfSlotKinds);
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : allSlotKinds)
            offsets.append(blob.offsetOf(slotHandlerFor(table, kind)).value_or(offsetOfAbsentStub));
    }
    return bytesOf(offsets);
}

static Vector<uint8_t> encodeSharedCodeCopies(const CodeLayout& layout)
{
    return bytesOf(Vector<uint32_t>(layout.sharedCodeCopies));
}

namespace {

struct FunctionLinks {
    CodeIndex index;
    ResolvedCallees callees;
};

struct SiteTables {
    Vector<Vector<uint8_t>> callSites;
    SideTables sideTables;
    Vector<uint8_t> faultSites;
    RegExpSections regExps;
};

} // anonymous namespace

static FunctionLinks mergeFunctions(BuiltImage& built, const Typed::PipelineOutput& pipeline, const CodeTemplates& templates, PhaseTimes& times, BuildContext& context)
{
    orderFunctions(built.functions, pipeline, context);
    times.didFinish("order functions"_s);
    removeJumpOnlyFunctions(built.functions, built.aliases, templates, context);
    times.didFinish("remove jump-only functions"_s);
    built.foldedFunctions = foldIdenticalFunctions(built.functions, built.aliases, *pipeline.program, context);
    times.didFinish("fold identical functions"_s);
    FunctionLinks links { CodeIndex(built.functions.span()), { } };
    links.callees = resolveCallees(built.functions, links.index, built.aliases.span(), context);
    times.didFinish("resolve callees"_s);
    return links;
}

static CodeLayoutInputs createLayoutInputs(const BuiltImage& built, const ImageInputs& inputs, const ResolvedCallees& callees, const CodeTemplates& templates, std::span<const FinalJump> finalJumps, unsigned codeGranuleShift)
{
    return {
        .functions = built.functions,
        .callees = callees,
        .aliases = built.aliases.span(),
        .outlinedCode = built.outlinedCode,
        .sharedCode = built.sharedCode,
        .regExpSection = built.regExpSection,
        .templates = templates,
        .finalJumps = finalJumps,
        .codeGranuleShift = codeGranuleShift,
        .webAssemblyCodeSize = inputs.webAssembly.code.size(),
        .webAssemblyCodeAlignment = inputs.webAssembly.codeAlignment,
        .constantsSize = built.constants.bytes.size(),
        .ffiCodeSize = inputs.ffiStubs.code.size(),
        .ffiCodeAlignment = inputs.ffiStubs.codeAlignment,
    };
}

static SiteTables encodeSiteTables(const BuiltImage& built, const ImageInputs& inputs, PhaseTimes& times, BuildContext& context)
{
    const LinkedFunctions& functions = built.functions;
    SiteTables tables;
    tables.callSites = encodeCallSites(functions, context);
    times.didFinish("encode call sites"_s);
    if (context.diagnostics().hasErrors())
        return tables;

    runTasksInParallel(context.parallelLoops(), {
        [&] { tables.sideTables = encodeSideTables(functions, tables.callSites.span(), context); },
        [&] { tables.faultSites = encodeFaultSites(functions, built.layout.bodyStarts.span(), context); },
        [&] { tables.regExps = encodeRegExpRecords({ inputs.regExpRequests, inputs.regExps, built.regExpSection }, context); },
    });
    times.didFinish("encode side tables, fault sites, regular expression records"_s);
    return tables;
}

static ImageSections collectSections(EncodedFunctionTables&& functionTables, SiteTables&& siteTables, EncodedText&& text, ImageInputs& inputs, const BuiltImage& built)
{
    ImageSections sections;
    sections[ImageSection::Functions] = WTF::move(functionTables.functions);
    sections[ImageSection::FunctionStarts] = WTF::move(functionTables.functionStarts);
    sections[ImageSection::CodeGranules] = WTF::move(functionTables.codeGranules);
    sections[ImageSection::InnerEntries] = WTF::move(functionTables.innerEntries);
    sections[ImageSection::Frames] = bytesOf(siteTables.sideTables.frames);
    sections[ImageSection::FrameMaps] = bytesOf(siteTables.sideTables.frameMapWords);
    sections[ImageSection::ClosureWords] = WTF::move(functionTables.closureWords);
    sections[ImageSection::Positions] = WTF::move(siteTables.sideTables.positions);
    sections[ImageSection::FaultSites] = WTF::move(siteTables.faultSites);
    sections[ImageSection::SideTables] = WTF::move(siteTables.sideTables.bytes);
    sections[ImageSection::SharedCodeCopies] = encodeSharedCodeCopies(built.layout);
    sections[ImageSection::Strings] = WTF::move(text.strings);
    sections[ImageSection::LongStrings] = WTF::move(text.longStrings);
    sections[ImageSection::Text] = WTF::move(text.text);
    sections[ImageSection::Identifiers] = WTF::move(text.identifiers);
    sections[ImageSection::StringConstants] = WTF::move(text.stringConstants);
    sections[ImageSection::RegExps] = WTF::move(siteTables.regExps.records);
    sections[ImageSection::RegExpHashes] = WTF::move(siteTables.regExps.hashes);
    sections[ImageSection::RegExpDetails] = WTF::move(siteTables.regExps.details);
    sections[ImageSection::WebAssemblyDirectory] = WTF::move(inputs.webAssembly.directory);
    sections[ImageSection::ProgramTables] = WTF::move(inputs.programTables);
    sections[ImageSection::StubAddresses] = encodeStubAddresses(built.sharedCode.blob);
    sections[ImageSection::StubReturnSites] = bytesOf(built.sharedCode.blob.returnSites);
    sections[ImageSection::SlotHandlers] = encodeSlotHandlers(built.sharedCode.blob);
    sections[ImageSection::MethodLookups] = WTF::move(functionTables.methodLookups);
    sections[ImageSection::MethodTable] = WTF::move(functionTables.methodTable);
    return sections;
}

static Vector<ReservedRange, 1> collectReservedRanges(const FFIStubs& ffiStubs, const CodeLayout& layout)
{
    if (!ffiStubs.callbackContextSize)
        return { };
    return { ReservedRange { static_cast<uint64_t>(layout.ffiCodeOffset) + ffiStubs.callbackContextOffset, ffiStubs.callbackContextSize } };
}

OptionSet<ImageFlag> flagsForUnseenCode(const Typed::PipelineOutput& pipeline)
{
    const Typed::FlowResults& flow = *pipeline.flow;
    if (!flow.hasResults())
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName, ImageFlag::AdmitsLibraryText };

    OptionSet<ImageFlag> flags;
    for (const auto& record : flow.unseenCode()) {
        if (Typed::admitsUnknownJavaScript(record.cause))
            flags.add(ImageFlag::AdmitsUnknownJavaScript);
        if (Typed::loadsModulesByName(record.cause))
            flags.add(ImageFlag::LoadsModulesByName);
        if (Typed::admitsLibraryText(record.cause))
            flags.add(ImageFlag::AdmitsLibraryText);
    }
    return flags;
}

static OptionSet<ImageFlag> collectImageFlags(const Typed::PipelineOutput& pipeline, const BuildContext& context)
{
    OptionSet<ImageFlag> flags = flagsForUnseenCode(pipeline);
    if (context.options().obfuscates())
        flags.add(ImageFlag::IsObfuscated);
    if (Typed::namesWaysToModuleObjects(*pipeline.program))
        flags.add(ImageFlag::ShowsWaysBetweenModuleObjects);
    return flags;
}

namespace {

struct ShapeSetsOfTables {
    Vector<uint8_t> bytes;
    Vector<ConstantRange> parts;
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

BuiltImage buildImage(ImageInputs&& inputs, BuildContext& context)
{
    Diagnostics& diagnostics = context.diagnostics();
    PhaseTimes times(context, Stage::BuildImage);
    BuiltImage result;

    std::expected<CodeTemplates, LinkFailure> createdTemplates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!createdTemplates) {
        reportLinkFailure(createdTemplates.error(), { }, context);
        return result;
    }
    const CodeTemplates& templates = *createdTemplates;
    result.functions = WTF::move(inputs.functions);
    times.didFinish("create code templates"_s);

    FunctionLinks links = mergeFunctions(result, inputs.pipeline, templates, times, context);
    const ResolvedCallees& callees = links.callees;
    if (diagnostics.hasErrors())
        return result;

    unsigned numberOfSharedCodeCopies = countSharedCodeCopies(result.functions, context);
    times.didFinish("count shared code copies"_s);
    ShapeSetsOfTables shapeSets = takeShapeSets(inputs.programTables, inputs.pipeline.placement->shapes());
    runTasksInParallel(context.parallelLoops(), {
        [&] { checkUncheckedStackUsage(result.functions, callees, result.aliases.span(), links.index, context); },
        [&] {
            if (std::optional<RegExpSection> section = layOutRegExpSection(inputs.regExps.results.span(), context.assemblerPolicy(), templates, &context))
                result.regExpSection = WTF::move(*section);
        },
        [&] { result.outlining = chooseOutlinedSequences({ result.functions, callees, result.aliases.span(), templates, numberOfSharedCodeCopies }, context); },
        [&] { result.constants = buildConstantPool(result.functions, context, { shapeSets.bytes.span(), shapeSets.parts.span() }); },
    });
    times.didFinish("check stack usage, lay out regular expressions, choose outlined sequences, build constant pool"_s);
    if (diagnostics.hasErrors())
        return result;

    result.outlinedCode = applyOutlining(result.functions, result.aliases, { links.index, callees, result.outlining, templates }, context);
    times.didFinish("apply outlining"_s);
    unsigned codeGranuleShift = chooseCodeGranuleShift(result.functions, context);
    times.didFinish("choose code granule shift"_s);
    Vector<FinalJump> finalJumps;
    runTasksInParallel(context.parallelLoops(), {
        [&] { result.sharedCode = selectSharedCode(result.functions, result.outlinedCode, inputs.stubCatalog, inputs.entriesUsedByFunctions, context); },
        [&] { finalJumps = findFinalJumps(result.functions, callees, result.outlinedCode, context); },
    });
    times.didFinish("select shared code, find final jumps"_s);
    if (diagnostics.hasErrors())
        return result;

    CodeLayoutInputs layoutInputs = createLayoutInputs(result, inputs, callees, templates, finalJumps.span(), codeGranuleShift);
    result.layout = layOutCode(layoutInputs, context);
    times.didFinish("lay out code"_s);
    if (diagnostics.hasErrors())
        return result;
    removeFinalJumps(result.functions, result.layout.fallsIntoNext, finalJumps.span(), context);
    times.didFinish("remove final jumps"_s);

    SiteTables siteTables = encodeSiteTables(result, inputs, times, context);
    if (diagnostics.hasErrors())
        return result;
    RELEASE_ASSERT(inputs.programTables.size() >= sizeof(ProgramTables));
    auto& programTables = *reinterpret_cast<const ProgramTables*>(inputs.programTables.span().data());
    EncodedFunctionTables functionTables = encodeFunctionTables({ result.functions, result.aliases, result.layout, links.index, siteTables.callSites.span(), siteTables.sideTables.offsets.span(), programTables, !context.options().obfuscates() }, context);
    context.destroyLater(WTF::move(siteTables.callSites));
    times.didFinish("encode function tables"_s);
    if (diagnostics.hasErrors())
        return result;

    ImageCounts counts { static_cast<uint32_t>(result.functions.size()), siteTables.regExps.maxOutputSize };
    ImageSections sections = collectSections(WTF::move(functionTables), WTF::move(siteTables), WTF::move(inputs.text), inputs, result);
    result.entriesUsed = inputs.entriesUsedByFunctions;
    result.entriesUsed.merge(result.sharedCode.blob.entriesUsed);
    times.didFinish("collect sections"_s);
    CodeLinkerInputs linker { layoutInputs, result.layout, result.outlining, result.constants, inputs.instanceSizes };
    Vector<ReservedRange, 1> reservedRanges = collectReservedRanges(inputs.ffiStubs, result.layout);
    result.image = writeImage({
        .linker = linker,
        .sections = sections,
        .webAssemblyCode = inputs.webAssembly.code.span(),
        .ffiCode = inputs.ffiStubs.code.span(),
        .reservedRanges = reservedRanges.span(),
        .entriesUsed = result.entriesUsed,
        .counts = counts,
        .flags = collectImageFlags(inputs.pipeline, context),
    }, context);
    if (diagnostics.hasErrors())
        result.image.clear();
    times.didFinish("write image"_s);

    context.statistics().count(StatisticID::ImageFunctionsEmitted, result.functions.size());
    context.destroyLater(std::tuple { WTF::move(links.callees), WTF::move(sections), WTF::move(finalJumps) });
    times.didFinish("release intermediate results"_s);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
