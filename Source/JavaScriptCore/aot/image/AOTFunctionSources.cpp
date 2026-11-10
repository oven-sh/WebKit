/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "ResourceExhaustion.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT {

static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) <= Number24::max);

namespace {

struct ClosureFunctionWithSource {
    FunctionID function;
    FunctionID closureEntry;
};

struct RetainedSourceRange {
    static uint64_t locationOf(ModuleID module, uint32_t offset) { return static_cast<uint64_t>(module.index()) << 32 | offset; }
    uint64_t location() const { return locationOf(module, start); }
    uint32_t size() const { return end - start; }

    ModuleID module;
    uint32_t start;
    uint32_t end;
    uint32_t offsetInRetainedSource { 0 };
};

struct ModuleRetainedRanges {
    ModuleID module;
    uint32_t first;
    uint32_t end;
};

struct HashedSourceRun {
    uint64_t hashAndSize;
    uint32_t run;
};

struct SourceRunLayout {
    bool storesOwnBytes(size_t run) const { return firstEqualRun[run] == run; }

    Vector<uint32_t> offsets;
    Vector<uint32_t> firstEqualRun;
    uint64_t size { 0 };
};

} // anonymous namespace

static SourceRunLayout computeSourceRunLayout(const ParallelLoops& loops, std::span<const std::span<const uint8_t>> runs)
{
    SourceRunLayout runLayout;
    runLayout.offsets.grow(runs.size());
    runLayout.firstEqualRun.grow(runs.size());
    Vector<HashedSourceRun> hashed;
    hashed.grow(runs.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(runs.size()), [&](unsigned run) {
        std::span<const uint8_t> bytes = runs[run];
        hashed[run] = { static_cast<uint64_t>(StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(bytes))) << 32 | bytes.size(), run };
        runLayout.firstEqualRun[run] = run;
    });
    parallelSort(loops, hashed.mutableSpan(), [](const HashedSourceRun& a, const HashedSourceRun& b) {
        return a.hashAndSize != b.hashAndSize ? a.hashAndSize < b.hashAndSize : a.run < b.run;
    });
    forEachRangeOfIndicesInParallel(loops, hashed.size(), [&](size_t at, size_t end) {
        while (at && at < end && hashed[at].hashAndSize == hashed[at - 1].hashAndSize)
            ++at;
        while (at < end) {
            HashedSourceRun first = hashed[at];
            for (++at; at < hashed.size() && hashed[at].hashAndSize == first.hashAndSize; ++at) {
                if (equalSpans(runs[first.run], runs[hashed[at].run]))
                    runLayout.firstEqualRun[hashed[at].run] = first.run;
            }
        }
    });
    for (size_t run = 0; run < runs.size(); ++run) {
        if (!runLayout.storesOwnBytes(run))
            continue;
        runLayout.offsets[run] = static_cast<uint32_t>(runLayout.size);
        runLayout.size += runs[run].size();
    }
    forEachRangeOfIndicesInParallel(loops, runs.size(), [&](size_t first, size_t end) {
        for (size_t run = first; run < end; ++run) {
            if (!runLayout.storesOwnBytes(run))
                runLayout.offsets[run] = runLayout.offsets[runLayout.firstEqualRun[run]];
        }
    });
    return runLayout;
}

static void writeSourceRuns(const ParallelLoops& loops, std::span<const std::span<const uint8_t>> runs, const SourceRunLayout& runLayout, std::span<uint8_t> text)
{
    RELEASE_ASSERT(text.size() == runLayout.size);
    forEachIndexInParallel(loops, static_cast<unsigned>(runs.size()), [&](unsigned run) {
        if (runLayout.storesOwnBytes(run))
            memcpySpan(text.subspan(runLayout.offsets[run]), runs[run]);
    });
}

static Vector<Vector<uint8_t>> compressSourceBlocks(const ParallelLoops& loops, const HostFunctionSources& host, std::span<const uint8_t> text)
{
    Vector<Vector<uint8_t>> blocks((text.size() + sourceBlockSize - 1) / sourceBlockSize);
    forEachIndexInParallel(loops, static_cast<unsigned>(blocks.size()), [&](unsigned index) {
        size_t offset = static_cast<size_t>(index) * sourceBlockSize;
        std::span<const uint8_t> block = text.subspan(offset, std::min<size_t>(sourceBlockSize, text.size() - offset));
        Vector<uint8_t>& compressed = blocks[index];
        compressed.grow(host.maxCompressedSize(block.size()));
        size_t written = host.compress(compressed.mutableSpan(), block);
        RELEASE_ASSERT_RESOURCE_AVAILABLE(written, MemoryExhaustion, "The host could not compress a block of source.");
        compressed.shrink(written);
    });
    return blocks;
}

struct CompressedSourceBlocks {
    bool isCompressed() const { return !blocks.isEmpty(); }
    size_t storedSize(const SourceRunLayout& runLayout) const { return isCompressed() ? blockEnds.last() : runLayout.size; }

    Vector<Vector<uint8_t>> blocks;
    Vector<uint32_t> blockEnds;
};

static CompressedSourceBlocks compressSourceRuns(const ParallelLoops& loops, const HostFunctionSources& host, std::span<const std::span<const uint8_t>> runs, const SourceRunLayout& runLayout)
{
    CompressedSourceBlocks stored;
    if (!host.maxCompressedSize || !host.compress)
        return stored;
    Vector<uint8_t> text;
    text.grow(runLayout.size);
    writeSourceRuns(loops, runs, runLayout, text.mutableSpan());
    stored.blocks = compressSourceBlocks(loops, host, text.span());
    uint64_t end = 0;
    for (const Vector<uint8_t>& block : stored.blocks) {
        end += block.size();
        stored.blockEnds.append(static_cast<uint32_t>(end));
    }
    if (end + stored.blockEnds.sizeInBytes() + sizeof(uint32_t) >= runLayout.size)
        return { };
    return stored;
}

static void writeStoredSource(const ParallelLoops& loops, std::span<const std::span<const uint8_t>> runs, const SourceRunLayout& runLayout, const CompressedSourceBlocks& stored, std::span<uint8_t> bytes)
{
    if (!stored.isCompressed()) {
        writeSourceRuns(loops, runs, runLayout, bytes);
        return;
    }
    forEachIndexInParallel(loops, static_cast<unsigned>(stored.blocks.size()), [&](unsigned index) {
        memcpySpan(bytes.subspan(index ? stored.blockEnds[index - 1] : 0), stored.blocks[index].span());
    });
}

static bool shouldOmitFunctionSources(const TableEncoder& encoder)
{
    return encoder.obfuscates() || encoder.build.recordedOptions().omitsFunctionSource;
}

static bool shouldEmbedFunctionSource(const Typed::Program& program, const Typed::Function& function)
{
    if (!function.module)
        return false;
    const Typed::Module& module = program.at(function.module);
    if (Typed::isBundlerHelperModule(program, module))
        return true;
    return Typed::showsSourceTextOf(function.role) && !function.enclosingRuntimeFunction && module.kind != ModuleKind::RuntimeSource;
}

static Vector<size_t> measureModuleFunctionSources(const Typed::Program& program, const HostFunctionSources& host)
{
    return Vector<size_t>(program.modules.size(), [&](size_t index) {
        size_t size = program.modules[index].functionSource.size();
        if (!size && host.size && host.copy)
            size = host.size(host.owner, static_cast<uint32_t>(index));
        return size;
    });
}

static Vector<ClosureFunctionWithSource> collectClosureFunctionsWithSource(const TableEncoder& encoder, std::span<const size_t> functionSourceSizes)
{
    const Typed::Program& program = encoder.program;
    const FunctionNumbers& numbers = encoder.numbers.functions();
    return collectInParallel<ClosureFunctionWithSource>(encoder.build.parallelLoops(), static_cast<unsigned>(program.functions.size()), [&](unsigned index, Vector<ClosureFunctionWithSource>& functions) {
        const Typed::Function& function = program.functions[index];
        if (function.sourceRange.start >= function.sourceRange.end || !shouldEmbedFunctionSource(program, function))
            return;
        FunctionID id(index);
        std::optional<FunctionID> closureEntry = encoder.findEntrypoint(id);
        if (!closureEntry || *closureEntry < numbers.firstClosureFunction() || *closureEntry >= numbers.endOfClosureFunctions())
            return;
        if (function.sourceRange.end > functionSourceSizes[function.module.index()]) {
            encoder.fail("FunctionSourceOutOfBounds"_s, id);
            return;
        }
        functions.append({ id, *closureEntry });
    });
}

static void mergeRetainedSourceRanges(const ParallelLoops& loops, Vector<RetainedSourceRange>& retained)
{
    parallelSort(loops, retained.mutableSpan(), [](const RetainedSourceRange& a, const RetainedSourceRange& b) {
        return a.location() < b.location();
    });
    size_t count = 0;
    for (const RetainedSourceRange& next : retained) {
        if (count && retained[count - 1].module == next.module && next.start <= retained[count - 1].end)
            retained[count - 1].end = std::max(retained[count - 1].end, next.end);
        else
            retained[count++] = next;
    }
    retained.shrink(count);
}

static Vector<ModuleRetainedRanges> groupRetainedRangesByModule(std::span<const RetainedSourceRange> retained)
{
    Vector<ModuleRetainedRanges> modules;
    for (uint32_t index = 0; index < retained.size(); ++index) {
        if (modules.isEmpty() || modules.last().module != retained[index].module)
            modules.append({ retained[index].module, index, index });
        modules.last().end = index + 1;
    }
    return modules;
}

static const RetainedSourceRange& findRetainedSourceRange(const Vector<RetainedSourceRange>& retained, const Typed::Function& function)
{
    auto next = std::ranges::upper_bound(retained, RetainedSourceRange::locationOf(function.module, function.sourceRange.start), { }, &RetainedSourceRange::location);
    const RetainedSourceRange& range = retained[static_cast<size_t>(next - retained.begin()) - 1];
    RELEASE_ASSERT(range.module == function.module && function.sourceRange.end <= range.end);
    return range;
}

static void setEmptyFunctionSources(TableEncoder& encoder)
{
    encoder.part.setTable(ProgramTableIndex::functionSources, Vector<FunctionSources> { });
}

namespace {

struct LocatedQuotedSource {
    uint32_t line;
    uint32_t column;
    uint32_t start;
    uint32_t end;
};

struct OutermostQuotedSource {
    uint32_t start;
    uint32_t end;
};

struct QuotedSourceLocations {
    static uint64_t locationOf(uint32_t line, uint32_t column) { return static_cast<uint64_t>(line) << 32 | column; }
    std::span<const uint64_t> locationsOf(size_t module) const { return locations.span().subspan(firstOfModule[module], firstOfModule[module + 1] - firstOfModule[module]); }

    Vector<size_t> firstOfModule;
    Vector<uint64_t> locations;
    Vector<uint8_t> isReportable;
};

struct ModuleQuotedSources {
    Vector<LocatedQuotedSource> quoted;
    Vector<OutermostQuotedSource> outermost;
    size_t firstRun { 0 };
    Vector<QuotedSourceGroupData> groups;
    Vector<uint8_t> rows;
    size_t firstGroup { 0 };
    uint64_t offsetOfRows { 0 };
};

} // anonymous namespace

static std::optional<QuotedSourceLocations> findQuotedSourceLocations(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    size_t numberOfModules = program.modules.size();
    QuotedSourceLocations result;
    result.firstOfModule.grow(numberOfModules + 1);
    size_t count = 0;
    for (size_t index = 0; index < numberOfModules; ++index) {
        result.firstOfModule[index] = count;
        count += program.modules[index].positions.quotedSources.size();
    }
    result.firstOfModule[numberOfModules] = count;
    if (!checkListSize(count, sizeof(uint64_t), encoder.build))
        return std::nullopt;

    result.locations.grow(count);
    result.isReportable.grow(count);
    std::atomic<bool> areLocationsSorted { true };
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfModules), [&](unsigned index) {
        std::span quotedSources = program.modules[index].positions.quotedSources.span();
        std::span<uint64_t> locations = result.locations.mutableSpan().subspan(result.firstOfModule[index], quotedSources.size());
        zeroSpan(result.isReportable.mutableSpan().subspan(result.firstOfModule[index], quotedSources.size()));
        for (size_t text = 0; text < quotedSources.size(); ++text) {
            LineColumn lineColumn = program.lineColumnFor(ModuleID(index), quotedSources[text].position);
            locations[text] = QuotedSourceLocations::locationOf(lineColumn.line, lineColumn.column);
        }
        if (std::ranges::is_sorted(locations))
            return;
        encoder.fail("QuotedSourcesNotSorted"_s, ModuleID(index));
        areLocationsSorted.store(false, std::memory_order_relaxed);
    });
    if (!areLocationsSorted.load(std::memory_order_relaxed))
        return std::nullopt;
    return result;
}

static bool markReportableQuotedSources(const TableEncoder& encoder, QuotedSourceLocations& sources)
{
    const LinkedFunctions& functions = encoder.inputs.functions;
    size_t numberOfModules = encoder.program.modules.size();
    std::atomic<bool> reportsLocation { false };
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(functions.size()), [&](unsigned position) {
        size_t lastModule = numberOfModules;
        uint64_t lastLocation = 0;
        auto report = [&](size_t module, uint64_t location) {
            if (module >= numberOfModules || (module == lastModule && location == lastLocation))
                return;
            lastModule = module;
            lastLocation = location;
            std::span<const uint64_t> locations = sources.locationsOf(module);
            for (size_t at = std::ranges::lower_bound(locations, location) - locations.begin(); at < locations.size() && locations[at] == location; ++at) {
                AtomicRef isReportable(sources.isReportable[sources.firstOfModule[module] + at]);
                if (!isReportable.load(std::memory_order_relaxed))
                    isReportable.store(1, std::memory_order_relaxed);
            }
        };
        bool hasLocation = false;
        const SiteDescription& sites = functions[position].sites;
        for (const SitePosition& site : sites.positions) {
            if (!site.source)
                continue;
            hasLocation = true;
            report(site.source.index(), QuotedSourceLocations::locationOf(site.line, site.column));
        }
        for (const SiteConstructionStart& start : sites.rareLists->constructionStarts) {
            ModuleID source = sites.positions[start.site].source;
            if (source && start.line)
                report(source.index(), QuotedSourceLocations::locationOf(start.line, start.column));
        }
        if (hasLocation && !reportsLocation.load(std::memory_order_relaxed))
            reportsLocation.store(true, std::memory_order_relaxed);
    });
    return reportsLocation.load(std::memory_order_relaxed);
}

static void collectModuleQuotedSources(const TableEncoder& encoder, const QuotedSourceLocations& sources, size_t index, ModuleQuotedSources& result)
{
    const Typed::SourcePositions& positions = encoder.program.modules[index].positions;
    std::span quotedSources = positions.quotedSources.span();
    std::span<const uint8_t> isReportable = sources.isReportable.span().subspan(sources.firstOfModule[index], quotedSources.size());
    for (size_t text = 0; text < quotedSources.size(); ++text) {
        const Typed::QuotedSource& quoted = quotedSources[text];
        if (quoted.start > quoted.end || quoted.end > positions.quotedSourceText.size()) {
            encoder.fail("QuotedSourceOutOfBounds"_s, ModuleID(index));
            continue;
        }
        if (!isReportable[text])
            continue;
        LineColumn lineColumn = encoder.program.lineColumnFor(ModuleID(index), quoted.position);
        result.quoted.append({ lineColumn.line, lineColumn.column, quoted.start, quoted.end });
    }
    result.outermost = Vector<OutermostQuotedSource>(result.quoted.size(), [&](size_t number) {
        return OutermostQuotedSource { result.quoted[number].start, result.quoted[number].end };
    });
    AOT::sortInLine(result.outermost, { }, &OutermostQuotedSource::start);
    size_t count = 0;
    for (const OutermostQuotedSource& next : result.outermost) {
        if (count && next.start < result.outermost[count - 1].end)
            result.outermost[count - 1].end = std::max(result.outermost[count - 1].end, next.end);
        else
            result.outermost[count++] = next;
    }
    result.outermost.shrink(count);
}

static void encodeQuotedSourceRows(size_t index, std::span<const uint32_t> runOffsets, ModuleQuotedSources& module)
{
    uint32_t lastLine = 0;
    uint32_t lastColumn = 0;
    uint32_t lastStart = 0;
    for (size_t number = 0; number < module.quoted.size(); ++number) {
        const LocatedQuotedSource& quoted = module.quoted[number];
        size_t run = std::ranges::upper_bound(module.outermost, quoted.start, { }, &OutermostQuotedSource::start) - module.outermost.begin() - 1;
        uint32_t start = runOffsets[module.firstRun + run] + (quoted.start - module.outermost[run].start);
        if (!(number % quotedSourceGroupSize)) {
            module.groups.append({ static_cast<uint32_t>(index), quoted.line, quoted.column, static_cast<uint32_t>(module.rows.size()) });
            lastLine = quoted.line;
            lastColumn = quoted.column;
            lastStart = 0;
        }
        QuotedSourceRow row { .isOnSameLine = quoted.line == lastLine };
        if (row.isOnSameLine)
            row.columnChange = quoted.column - lastColumn;
        else {
            row.lineChange = quoted.line - lastLine;
            row.column = quoted.column;
        }
        row.startChange = static_cast<int64_t>(start) - static_cast<int64_t>(lastStart);
        row.length = quoted.end - quoted.start;
        row.append(module.rows);
        lastLine = quoted.line;
        lastColumn = quoted.column;
        lastStart = start;
    }
}

static void setEmptyQuotedSources(TableEncoder& encoder)
{
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceGroups, encoder.part.allocate(0, alignof(QuotedSourceGroupData)), 0);
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceRows, encoder.part.allocate(0), 0);
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceText, encoder.part.allocate(0), 0);
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceBlocks, encoder.part.allocate(0, alignof(uint32_t)), 0);
}

static void encodeQuotedSources(TableEncoder& encoder, StepTimes& times)
{
    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    if (shouldOmitFunctionSources(encoder)) {
        setEmptyQuotedSources(encoder);
        return;
    }
    std::optional<QuotedSourceLocations> locations = findQuotedSourceLocations(encoder);
    times.didFinish("find the locations of quoted sources"_s);
    if (!locations || !markReportableQuotedSources(encoder, *locations)) {
        setEmptyQuotedSources(encoder);
        return;
    }
    times.didFinish("mark the reportable quoted sources"_s);

    unsigned numberOfModules = static_cast<unsigned>(program.modules.size());
    Vector<ModuleQuotedSources> modules(numberOfModules);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        collectModuleQuotedSources(encoder, *locations, index, modules[index]);
    });
    *locations = { };
    size_t numberOfRuns = 0;
    for (ModuleQuotedSources& module : modules) {
        module.firstRun = numberOfRuns;
        numberOfRuns += module.outermost.size();
    }
    if (!checkListSize(numberOfRuns, sizeof(std::span<const uint8_t>), encoder.build)) {
        setEmptyQuotedSources(encoder);
        return;
    }
    Vector<std::span<const uint8_t>> runs;
    runs.grow(numberOfRuns);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        std::span<const uint8_t> moduleSourceText = program.modules[index].positions.quotedSourceText;
        const ModuleQuotedSources& module = modules[index];
        for (size_t run = 0; run < module.outermost.size(); ++run)
            runs[module.firstRun + run] = moduleSourceText.subspan(module.outermost[run].start, module.outermost[run].end - module.outermost[run].start);
    });
    times.didFinish("collect quoted sources"_s);

    SourceRunLayout runLayout = computeSourceRunLayout(loops, runs.span());
    times.didFinish("lay out quoted sources"_s);
    if (!checkSectionSize(runLayout.size, encoder.build)) {
        setEmptyQuotedSources(encoder);
        return;
    }

    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        encodeQuotedSourceRows(index, runLayout.offsets.span(), modules[index]);
    });
    size_t numberOfGroups = 0;
    uint64_t sizeOfRows = 0;
    for (ModuleQuotedSources& module : modules) {
        module.firstGroup = numberOfGroups;
        module.offsetOfRows = sizeOfRows;
        numberOfGroups += module.groups.size();
        sizeOfRows += module.rows.size();
    }
    uint64_t sizeOfGroups = static_cast<uint64_t>(numberOfGroups) * sizeof(QuotedSourceGroupData);
    if (!checkSectionSize(sizeOfGroups + sizeOfRows + runLayout.size, encoder.build)) {
        setEmptyQuotedSources(encoder);
        return;
    }

    const HostFunctionSources& host = encoder.build.functionSources();
    CompressedSourceBlocks stored = compressSourceRuns(loops, host, runs.span(), runLayout);
    if (host.maxCompressedSize && host.compress)
        times.didFinish("compress quoted sources"_s);
    size_t storedSize = stored.storedSize(runLayout);
    Vector<uint32_t> sourceBlocks;
    if (stored.isCompressed()) {
        sourceBlocks.append(static_cast<uint32_t>(runLayout.size));
        sourceBlocks.appendVector(stored.blockEnds);
    }

    encoder.part.reserveRoomFor(sizeOfGroups + sizeOfRows + storedSize + sourceBlocks.sizeInBytes() + 5 * sizeof(uint64_t));
    uint32_t offsetOfGroups = encoder.part.allocateUnfilled(sizeOfGroups, alignof(QuotedSourceGroupData));
    uint32_t offsetOfBlocks = encoder.part.appendUnshared(asBytes(sourceBlocks.span()), alignof(uint32_t));
    uint32_t offsetOfRows = encoder.part.allocateUnfilled(sizeOfRows);
    uint32_t storedSourceOffset = encoder.part.allocateUnfilled(storedSize);
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceBlocks, offsetOfBlocks, static_cast<uint32_t>(sourceBlocks.size()));
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceGroups, offsetOfGroups, static_cast<uint32_t>(numberOfGroups));
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceRows, offsetOfRows, static_cast<uint32_t>(sizeOfRows));
    encoder.part.setTableRange(ProgramTableIndex::quotedSourceText, storedSourceOffset, static_cast<uint32_t>(storedSize));
    std::span<uint8_t> groups = encoder.part.bytesAt(offsetOfGroups, sizeOfGroups);
    std::span<uint8_t> rows = encoder.part.bytesAt(offsetOfRows, sizeOfRows);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        ModuleQuotedSources& module = modules[index];
        for (QuotedSourceGroupData& group : module.groups)
            group.offsetOfRows += static_cast<uint32_t>(module.offsetOfRows);
        memcpySpan(groups.subspan(module.firstGroup * sizeof(QuotedSourceGroupData)), asBytes(module.groups.span()));
        memcpySpan(rows.subspan(module.offsetOfRows), module.rows.span());
        module = { };
    });
    writeStoredSource(loops, runs.span(), runLayout, stored, encoder.part.bytesAt(storedSourceOffset, storedSize));
    times.didFinish("write quoted sources"_s);
}

void encodeFunctionSources(TableEncoder& encoder)
{
    StepTimes times(encoder.build, "image: function sources"_s);
    encodeQuotedSources(encoder, times);
    if (shouldOmitFunctionSources(encoder)) {
        setEmptyFunctionSources(encoder);
        return;
    }

    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    const HostFunctionSources& host = encoder.build.functionSources();
    Vector<size_t> functionSourceSizes = measureModuleFunctionSources(program, host);
    Vector<ClosureFunctionWithSource> functions = collectClosureFunctionsWithSource(encoder, functionSourceSizes.span());
    Vector<RetainedSourceRange> retained(functions.size(), [&](size_t index) {
        const Typed::Function& function = program.at(functions[index].function);
        return RetainedSourceRange { function.module, function.sourceRange.start, function.sourceRange.end };
    });
    mergeRetainedSourceRanges(loops, retained);

    uint64_t sizeBeforeSharing = 0;
    for (const RetainedSourceRange& range : retained)
        sizeBeforeSharing += range.size();
    times.didFinish("collect the functions with source"_s);
    if (functions.isEmpty() || !checkSectionSize(sizeBeforeSharing, encoder.build)) {
        setEmptyFunctionSources(encoder);
        return;
    }

    Vector<ModuleRetainedRanges> modules = groupRetainedRangesByModule(retained.span());
    Vector<Vector<uint8_t>> hostFunctionSources(modules.size());
    Vector<std::span<const uint8_t>> runs;
    runs.grow(retained.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(modules.size()), [&](unsigned index) {
        const ModuleRetainedRanges& module = modules[index];
        std::span<const uint8_t> text = program.at(module.module).functionSource;
        if (text.empty()) {
            hostFunctionSources[index].grow(functionSourceSizes[module.module.index()]);
            host.copy(host.owner, module.module.index(), hostFunctionSources[index].mutableSpan().data());
            text = hostFunctionSources[index].span();
        }
        for (uint32_t range = module.first; range < module.end; ++range)
            runs[range] = text.subspan(retained[range].start, retained[range].size());
    });
    times.didFinish("copy the function sources of modules"_s);
    SourceRunLayout runLayout = computeSourceRunLayout(loops, runs.span());
    for (size_t range = 0; range < retained.size(); ++range)
        retained[range].offsetInRetainedSource = runLayout.offsets[range];
    times.didFinish("lay out function sources"_s);

    Vector<FunctionSourceEntry> entries;
    entries.grow(functions.size());
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const Typed::Function& function = program.at(functions[index].function);
            const RetainedSourceRange& range = findRetainedSourceRange(retained, function);
            uint32_t start = range.offsetInRetainedSource + (function.sourceRange.start - range.start);
            entries[index] = FunctionSourceEntry { Number24(functions[index].closureEntry.index()), Number32(start), Number32(start + (function.sourceRange.end - function.sourceRange.start)) };
        }
    });
    if (AOT::hasFlag(encoder.build.options().injectedFaults, Wire::FaultInjection::ReverseFunctionSourceEntries))
        entries.reverse();
    if (!std::ranges::is_sorted(entries, { }, [](const FunctionSourceEntry& entry) -> uint32_t { return entry.function; }))
        encoder.fail("TableNotSorted"_s, { }, "functionSources"_s);

    CompressedSourceBlocks stored = compressSourceRuns(loops, host, runs.span(), runLayout);
    if (host.maxCompressedSize && host.compress)
        times.didFinish("compress function sources"_s);
    const Vector<uint32_t>& blockEnds = stored.blockEnds;
    size_t storedSize = stored.storedSize(runLayout);

    encoder.part.reserveRoomFor(entries.sizeInBytes() + blockEnds.sizeInBytes() + storedSize + sizeof(FunctionSources) + 4 * sizeof(uint64_t));
    uint32_t entriesOffset = encoder.part.appendUnshared(asBytes(entries.span()), alignof(FunctionSourceEntry));
    uint32_t blockEndsOffset = !stored.isCompressed() ? 0 : encoder.part.appendUnshared(asBytes(blockEnds.span()), alignof(uint32_t));
    uint32_t sourceOffset = encoder.part.allocateUnfilled(storedSize, 1);
    writeStoredSource(loops, runs.span(), runLayout, stored, encoder.part.bytesAt(sourceOffset, storedSize));
    uint32_t record = encoder.part.setTable(ProgramTableIndex::functionSources, Vector<FunctionSources, 1> { FunctionSources { static_cast<uint32_t>(entries.size()), entriesOffset, sourceOffset, static_cast<uint32_t>(stored.blocks.size()), blockEndsOffset, static_cast<uint32_t>(runLayout.size) } });
    encoder.part.link(record, &FunctionSources::entriesOffset);
    encoder.part.link(record, &FunctionSources::sourceOffset);
    if (stored.isCompressed())
        encoder.part.link(record, &FunctionSources::blockEndsOffset);
    forEachIndexInParallel(loops, static_cast<unsigned>(hostFunctionSources.size()), [&](unsigned index) {
        hostFunctionSources[index] = { };
    });
    times.didFinish("write function sources"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
