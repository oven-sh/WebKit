/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
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
#include "AOTPositionTable.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRecordEncoder.h"
#include "AOTSectionWriter.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreads.h"
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

enum class Adjacency : uint8_t {
    None,
    AfterOrigin,
    BeforeTarget,
};

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
    uint32_t rank;
    uint32_t leader;
    GroupSide side;
    uint32_t part;
    uint32_t function;
    uint32_t position;
};

} // anonymous namespace

static Adjacency adjacencyOf(FunctionRole role)
{
    switch (role) {
    case FunctionRole::Continuation:
        return Adjacency::AfterOrigin;
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::GroupEntrypoint:
        return Adjacency::BeforeTarget;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return Adjacency::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return Adjacency::None;
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

static CodeOrderKey codeOrderKeyOf(FunctionID function, uint32_t position, const Typed::Program& program, const Typed::Heat& heat, const Vector<uint32_t>& rankByFunction)
{
    FunctionID leader = function;
    uint32_t part = 0;
    if (adjacencyOf(program.at(leader).role) == Adjacency::AfterOrigin) {
        auto& record = program.at(leader).creator.as<CreatorRecord::Continuation>();
        part = 1 + record.index;
        leader = record.origin;
    }
    GroupSide side = GroupSide::FromLeader;
    if (adjacencyOf(program.at(leader).role) == Adjacency::BeforeTarget) {
        side = GroupSide::BeforeLeader;
        leader = program.at(leader).creator.as<CreatorRecord::Entrypoint>().target;
    }
    // An outlined body is shared by sites of several functions, all in cold blocks, so it has no heat of its own.
    CodeBand band = program.at(leader).role == FunctionRole::OutlinedBody ? CodeBand::Cold : bandOf(heat.of(program, leader));
    uint32_t rank = band == CodeBand::Hot ? rankByFunction[leader.index()] : 0;
    return { band, rank, leader.index(), side, part, function.index(), position };
}

void orderFunctions(LinkedFunctions& functions, const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    const Typed::Program& program = *pipeline.program;
    const Typed::Heat& heat = *pipeline.heat;
    Vector<uint32_t> rankByFunction(FillWith { }, program.functions.size(), std::numeric_limits<uint32_t>::max());
    uint32_t rank = 0;
    for (FunctionID function : heat.hottestFirst())
        rankByFunction[function.index()] = rank++;

    Vector<CodeOrderKey> keys(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        keys[position] = codeOrderKeyOf(functions[position].function, position, program, heat, rankByFunction);
    });
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

static void encodeFrameRanges(Vector<uint8_t>& bytes, std::span<const CodeRange> ranges)
{
    uint32_t previousEnd = 0;
    for (const CodeRange& range : ranges) {
        SideTableParts::appendFrameRange(bytes, previousEnd, range.begin, range.end);
        previousEnd = range.end;
    }
}

SideTables encodeSideTables(const LinkedFunctions& functions, std::span<const uint32_t> callSiteOffsets, BuildContext& context)
{
    RELEASE_ASSERT(callSiteOffsets.size() == functions.size());
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

    bool hasPositions = !context.options().obfuscates();
    SideTables result;
    result.frameMapWords.grow(numberOfWords);
    Vector<Vector<uint8_t>> streams(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const SiteDescription& sites = functions[position].sites;
        Vector<uint8_t> frameRanges;
        encodeFrameRanges(frameRanges, functions[position].frame.frameRanges.span());
        std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
        if (!sites.frameMapWords.isEmpty()) {
            frameMapRange.emplace(static_cast<uint32_t>(firstWordOf[position]), static_cast<uint32_t>(sites.frameMapWords.size()));
            memcpySpan(result.frameMapWords.mutableSpan().subspan(frameMapRange->first), sites.frameMapWords.span());
        }
        Vector<uint8_t> handlers;
        encodeHandlers(handlers, sites.handlers.span());
        Vector<uint8_t> positions;
        if (hasPositions)
            encodePositions(positions, sites);
        SideTableParts::append(streams[position], {
            .frameRanges = frameRanges.span(),
            .frameMapRange = frameMapRange,
            .callSites = callSiteOffsets[position] ? std::optional(callSiteOffsets[position]) : std::nullopt,
            .handlers = handlers.span(),
            .positions = positions.span(),
        });
    });

    SectionWriter writer(context);
    result.offsets = writer.appendAll(streams.span());
    result.bytes = writer.takeBytes();
    context.destroyLater(WTF::move(streams));
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
        if (hasAddressInImage(descriptorOf(placement.key.family()).linkage))
            addresses.append({ placement.key.bits(), placement.offset, 0 });
    }
    std::ranges::sort(addresses, { }, &StubAddressInImage::key);
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
    CallSiteTable callSites;
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
        [&] { tables.sideTables = encodeSideTables(functions, tables.callSites.offsets.span(), context); },
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
    sections[ImageSection::FunctionsByID] = WTF::move(functionTables.functionsByID);
    sections[ImageSection::InnerEntries] = WTF::move(functionTables.innerEntries);
    sections[ImageSection::Frames] = WTF::move(functionTables.frames);
    sections[ImageSection::FrameMaps] = bytesOf(siteTables.sideTables.frameMapWords);
    sections[ImageSection::ClosureWords] = WTF::move(functionTables.closureWords);
    sections[ImageSection::CallSites] = WTF::move(siteTables.callSites.bytes);
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
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::LoadsModulesByName, ImageFlag::ReplacesLoaderAnswers, ImageFlag::AdmitsLibraryText };

    OptionSet<ImageFlag> flags;
    for (const auto& record : flow.unseenCode()) {
        if (Typed::admitsUnknownJavaScript(record.cause))
            flags.add(ImageFlag::AdmitsUnknownJavaScript);
        if (Typed::loadsModulesByName(record.cause))
            flags.add(ImageFlag::LoadsModulesByName);
        if (Typed::replacesLoaderAnswers(record))
            flags.add(ImageFlag::ReplacesLoaderAnswers);
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
    return flags;
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
    runTasksInParallel(context.parallelLoops(), {
        [&] { checkUncheckedStackUsage(result.functions, callees, result.aliases.span(), links.index, context); },
        [&] {
            if (std::optional<RegExpSection> section = layOutRegExpSection(inputs.regExps.results.span(), context.assemblerPolicy(), templates, &context))
                result.regExpSection = WTF::move(*section);
        },
        [&] { result.outlining = chooseOutlinedSequences({ result.functions, callees, result.aliases.span(), templates, numberOfSharedCodeCopies }, context); },
    });
    times.didFinish("check stack usage, lay out regular expressions, choose outlined sequences"_s);
    if (diagnostics.hasErrors())
        return result;

    result.outlinedCode = applyOutlining(result.functions, result.aliases, { links.index, callees, result.outlining, templates }, context);
    times.didFinish("apply outlining"_s);
    unsigned codeGranuleShift = chooseCodeGranuleShift(result.functions, context);
    times.didFinish("choose code granule shift"_s);
    Vector<FinalJump> finalJumps;
    runTasksInParallel(context.parallelLoops(), {
        [&] { result.sharedCode = selectSharedCode(result.functions, result.outlinedCode, inputs.stubCatalog, inputs.entriesUsedByFunctions, context); },
        [&] { result.constants = buildConstantPool(result.functions, context); },
        [&] { finalJumps = findFinalJumps(result.functions, callees, result.outlinedCode, context); },
    });
    times.didFinish("select shared code, build constant pool, find final jumps"_s);
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
    EncodedFunctionTables functionTables = encodeFunctionTables({ result.functions, result.aliases, result.layout, links.index, siteTables.callSites.offsets.span(), siteTables.sideTables.offsets.span(), programTables, !context.options().obfuscates() }, context);
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
    context.statistics().count(StatisticID::CallSiteBytesEmitted, sections[ImageSection::CallSites].size());
    context.destroyLater(std::tuple { WTF::move(links.callees), WTF::move(sections), WTF::move(finalJumps) });
    times.didFinish("release intermediate results"_s);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
