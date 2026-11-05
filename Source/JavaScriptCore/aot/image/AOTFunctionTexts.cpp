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

struct ClosureFunctionWithText {
    FunctionID function;
    FunctionID closureEntry;
};

struct KeptFunctionText {
    static uint64_t placeOf(ModuleID module, uint32_t offset) { return static_cast<uint64_t>(module.index()) << 32 | offset; }
    uint64_t place() const { return placeOf(module, start); }
    uint32_t size() const { return end - start; }

    ModuleID module;
    uint32_t start;
    uint32_t end;
    uint32_t offsetInKeptText { 0 };
};

struct KeptTextsOfModule {
    ModuleID module;
    uint32_t first;
    uint32_t end;
};

struct HashedRunOfText {
    uint64_t hashAndSize;
    uint32_t run;
};

struct PlacedRunsOfText {
    bool storesOwnBytes(size_t run) const { return firstEqualRun[run] == run; }

    Vector<uint32_t> offsets;
    Vector<uint32_t> firstEqualRun;
    uint64_t size { 0 };
};

} // anonymous namespace

static PlacedRunsOfText placeRunsOfText(const ParallelLoops& loops, std::span<const std::span<const uint8_t>> runs)
{
    PlacedRunsOfText placed;
    placed.offsets.grow(runs.size());
    placed.firstEqualRun.grow(runs.size());
    Vector<HashedRunOfText> hashed;
    hashed.grow(runs.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(runs.size()), [&](unsigned run) {
        std::span<const uint8_t> bytes = runs[run];
        hashed[run] = { static_cast<uint64_t>(StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(bytes))) << 32 | bytes.size(), run };
        placed.firstEqualRun[run] = run;
    });
    parallelSort(loops, hashed.mutableSpan(), [](const HashedRunOfText& a, const HashedRunOfText& b) {
        return a.hashAndSize != b.hashAndSize ? a.hashAndSize < b.hashAndSize : a.run < b.run;
    });
    forEachRangeOfIndicesInParallel(loops, hashed.size(), [&](size_t at, size_t end) {
        while (at && at < end && hashed[at].hashAndSize == hashed[at - 1].hashAndSize)
            ++at;
        while (at < end) {
            HashedRunOfText first = hashed[at];
            for (++at; at < hashed.size() && hashed[at].hashAndSize == first.hashAndSize; ++at) {
                if (equalSpans(runs[first.run], runs[hashed[at].run]))
                    placed.firstEqualRun[hashed[at].run] = first.run;
            }
        }
    });
    for (size_t run = 0; run < runs.size(); ++run) {
        if (!placed.storesOwnBytes(run))
            continue;
        placed.offsets[run] = static_cast<uint32_t>(placed.size);
        placed.size += runs[run].size();
    }
    forEachRangeOfIndicesInParallel(loops, runs.size(), [&](size_t first, size_t end) {
        for (size_t run = first; run < end; ++run) {
            if (!placed.storesOwnBytes(run))
                placed.offsets[run] = placed.offsets[placed.firstEqualRun[run]];
        }
    });
    return placed;
}

static void writeRunsOfText(const ParallelLoops& loops, std::span<const std::span<const uint8_t>> runs, const PlacedRunsOfText& placed, std::span<uint8_t> text)
{
    RELEASE_ASSERT(text.size() == placed.size);
    forEachIndexInParallel(loops, static_cast<unsigned>(runs.size()), [&](unsigned run) {
        if (placed.storesOwnBytes(run))
            memcpySpan(text.subspan(placed.offsets[run]), runs[run]);
    });
}

static bool leavesOutFunctionTexts(const TableEncoder& encoder)
{
    return encoder.obfuscates() || encoder.build.recordedOptions().omitsFunctionSource;
}

static bool mayShowFunctionText(const Typed::Program& program, const Typed::Function& function)
{
    if (!function.module)
        return false;
    const Typed::Module& module = program.at(function.module);
    if (Typed::isBundlerHelperModule(program, module))
        return true;
    return Typed::showsSourceTextOf(function.role) && !function.enclosingRuntimeFunction && module.kind != ModuleKind::RuntimeSource;
}

static Vector<size_t> measureFunctionTextsOfModules(const Typed::Program& program, const HostFunctionTexts& host)
{
    return Vector<size_t>(program.modules.size(), [&](size_t index) {
        size_t size = program.modules[index].functionText.size();
        if (!size && host.size && host.copy)
            size = host.size(host.owner, static_cast<uint32_t>(index));
        return size;
    });
}

static Vector<ClosureFunctionWithText> collectClosureFunctionsWithText(const TableEncoder& encoder, std::span<const size_t> sizesOfTexts)
{
    const Typed::Program& program = encoder.program;
    const FunctionNumbers& numbers = encoder.numbers.functions();
    return collectInParallel<ClosureFunctionWithText>(encoder.build.parallelLoops(), static_cast<unsigned>(program.functions.size()), [&](unsigned index, Vector<ClosureFunctionWithText>& functions) {
        const Typed::Function& function = program.functions[index];
        if (function.text.start >= function.text.end || !mayShowFunctionText(program, function))
            return;
        FunctionID id(index);
        std::optional<FunctionID> closureEntry = encoder.findEntrypoint(id);
        if (!closureEntry || *closureEntry < numbers.firstClosureFunction() || *closureEntry >= numbers.endOfClosureFunctions())
            return;
        if (function.text.end > sizesOfTexts[function.module.index()]) {
            encoder.fail("FunctionTextOutsideTextOfModule"_s, id);
            return;
        }
        functions.append({ id, *closureEntry });
    });
}

static void mergeKeptFunctionTexts(const ParallelLoops& loops, Vector<KeptFunctionText>& kept)
{
    parallelSort(loops, kept.mutableSpan(), [](const KeptFunctionText& a, const KeptFunctionText& b) {
        return a.place() < b.place();
    });
    size_t count = 0;
    for (const KeptFunctionText& next : kept) {
        if (count && kept[count - 1].module == next.module && next.start <= kept[count - 1].end)
            kept[count - 1].end = std::max(kept[count - 1].end, next.end);
        else
            kept[count++] = next;
    }
    kept.shrink(count);
}

static Vector<KeptTextsOfModule> groupKeptFunctionTextsByModule(std::span<const KeptFunctionText> kept)
{
    Vector<KeptTextsOfModule> modules;
    for (uint32_t index = 0; index < kept.size(); ++index) {
        if (modules.isEmpty() || modules.last().module != kept[index].module)
            modules.append({ kept[index].module, index, index });
        modules.last().end = index + 1;
    }
    return modules;
}

static const KeptFunctionText& findKeptFunctionText(const Vector<KeptFunctionText>& kept, const Typed::Function& function)
{
    auto next = std::ranges::upper_bound(kept, KeptFunctionText::placeOf(function.module, function.text.start), { }, &KeptFunctionText::place);
    const KeptFunctionText& range = kept[static_cast<size_t>(next - kept.begin()) - 1];
    RELEASE_ASSERT(range.module == function.module && function.text.end <= range.end);
    return range;
}

static void setEmptyFunctionTexts(TableEncoder& encoder)
{
    encoder.part.setTable(ProgramTableIndex::functionTexts, Vector<FunctionTexts> { });
}

namespace {

struct QuotedTextAtPlace {
    uint32_t line;
    uint32_t column;
    uint32_t start;
    uint32_t end;
};

struct OutermostQuotedText {
    uint32_t start;
    uint32_t end;
};

struct PlacesOfQuotedTexts {
    static uint64_t placeOf(uint32_t line, uint32_t column) { return static_cast<uint64_t>(line) << 32 | column; }
    std::span<const uint64_t> placesOf(size_t module) const { return places.span().subspan(firstOfModule[module], firstOfModule[module + 1] - firstOfModule[module]); }

    Vector<size_t> firstOfModule;
    Vector<uint64_t> places;
    Vector<uint8_t> isReported;
};

struct QuotedTextsOfModule {
    Vector<QuotedTextAtPlace> quoted;
    Vector<OutermostQuotedText> outermost;
    size_t firstRun { 0 };
    Vector<QuotedTextGroupData> groups;
    Vector<uint8_t> rows;
    size_t firstGroup { 0 };
    uint64_t offsetOfRows { 0 };
};

} // anonymous namespace

static std::optional<PlacesOfQuotedTexts> findPlacesOfQuotedTexts(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    size_t numberOfModules = program.modules.size();
    PlacesOfQuotedTexts result;
    result.firstOfModule.grow(numberOfModules + 1);
    size_t count = 0;
    for (size_t index = 0; index < numberOfModules; ++index) {
        result.firstOfModule[index] = count;
        count += program.modules[index].positions.quotedTexts.size();
    }
    result.firstOfModule[numberOfModules] = count;
    if (!checkListSize(count, sizeof(uint64_t), encoder.build))
        return std::nullopt;

    result.places.grow(count);
    result.isReported.grow(count);
    std::atomic<bool> placesAscend { true };
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfModules), [&](unsigned index) {
        std::span quotedTexts = program.modules[index].positions.quotedTexts.span();
        std::span<uint64_t> places = result.places.mutableSpan().subspan(result.firstOfModule[index], quotedTexts.size());
        zeroSpan(result.isReported.mutableSpan().subspan(result.firstOfModule[index], quotedTexts.size()));
        for (size_t text = 0; text < quotedTexts.size(); ++text) {
            LineColumn lineColumn = program.lineColumnFor(ModuleID(index), quotedTexts[text].position);
            places[text] = PlacesOfQuotedTexts::placeOf(lineColumn.line, lineColumn.column);
        }
        if (std::ranges::is_sorted(places))
            return;
        encoder.fail("PlacesOfQuotedTextsDoNotAscend"_s, ModuleID(index));
        placesAscend.store(false, std::memory_order_relaxed);
    });
    if (!placesAscend.load(std::memory_order_relaxed))
        return std::nullopt;
    return result;
}

static bool markQuotedTextsThatCodeReports(const TableEncoder& encoder, PlacesOfQuotedTexts& texts)
{
    const LinkedFunctions& functions = encoder.inputs.functions;
    size_t numberOfModules = encoder.program.modules.size();
    std::atomic<bool> reportsPlace { false };
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(functions.size()), [&](unsigned position) {
        size_t lastModule = numberOfModules;
        uint64_t lastPlace = 0;
        auto report = [&](size_t module, uint64_t place) {
            if (module >= numberOfModules || (module == lastModule && place == lastPlace))
                return;
            lastModule = module;
            lastPlace = place;
            std::span<const uint64_t> places = texts.placesOf(module);
            for (size_t at = std::ranges::lower_bound(places, place) - places.begin(); at < places.size() && places[at] == place; ++at) {
                AtomicRef isReported(texts.isReported[texts.firstOfModule[module] + at]);
                if (!isReported.load(std::memory_order_relaxed))
                    isReported.store(1, std::memory_order_relaxed);
            }
        };
        bool hasPlace = false;
        const SiteDescription& sites = functions[position].sites;
        for (const SitePosition& site : sites.positions) {
            if (!site.source)
                continue;
            hasPlace = true;
            report(site.source.index(), PlacesOfQuotedTexts::placeOf(site.line, site.column));
        }
        for (const SiteConstructionStart& start : sites.rareLists->constructionStarts) {
            ModuleID source = sites.positions[start.site].source;
            if (source && start.line)
                report(source.index(), PlacesOfQuotedTexts::placeOf(start.line, start.column));
        }
        if (hasPlace && !reportsPlace.load(std::memory_order_relaxed))
            reportsPlace.store(true, std::memory_order_relaxed);
    });
    return reportsPlace.load(std::memory_order_relaxed);
}

static void collectQuotedTextsOfModule(const TableEncoder& encoder, const PlacesOfQuotedTexts& texts, size_t index, QuotedTextsOfModule& result)
{
    const Typed::SourcePositions& positions = encoder.program.modules[index].positions;
    std::span quotedTexts = positions.quotedTexts.span();
    std::span<const uint8_t> isReported = texts.isReported.span().subspan(texts.firstOfModule[index], quotedTexts.size());
    for (size_t text = 0; text < quotedTexts.size(); ++text) {
        const Typed::QuotedText& quoted = quotedTexts[text];
        if (quoted.start > quoted.end || quoted.end > positions.textOfQuotes.size()) {
            encoder.fail("QuotedTextOutsideTextOfModule"_s, ModuleID(index));
            continue;
        }
        if (!isReported[text])
            continue;
        LineColumn lineColumn = encoder.program.lineColumnFor(ModuleID(index), quoted.position);
        result.quoted.append({ lineColumn.line, lineColumn.column, quoted.start, quoted.end });
    }
    result.outermost = Vector<OutermostQuotedText>(result.quoted.size(), [&](size_t number) {
        return OutermostQuotedText { result.quoted[number].start, result.quoted[number].end };
    });
    AOT::sortInLine(result.outermost, { }, &OutermostQuotedText::start);
    size_t count = 0;
    for (const OutermostQuotedText& next : result.outermost) {
        if (count && next.start < result.outermost[count - 1].end)
            result.outermost[count - 1].end = std::max(result.outermost[count - 1].end, next.end);
        else
            result.outermost[count++] = next;
    }
    result.outermost.shrink(count);
}

static void encodeRowsOfQuotedTextsOfModule(size_t index, std::span<const uint32_t> offsetsOfRuns, QuotedTextsOfModule& module)
{
    uint32_t lastLine = 0;
    uint32_t lastColumn = 0;
    uint32_t lastStart = 0;
    for (size_t number = 0; number < module.quoted.size(); ++number) {
        const QuotedTextAtPlace& quoted = module.quoted[number];
        size_t run = std::ranges::upper_bound(module.outermost, quoted.start, { }, &OutermostQuotedText::start) - module.outermost.begin() - 1;
        uint32_t start = offsetsOfRuns[module.firstRun + run] + (quoted.start - module.outermost[run].start);
        if (!(number % rowsInGroupOfQuotedTexts)) {
            module.groups.append({ static_cast<uint32_t>(index), quoted.line, quoted.column, static_cast<uint32_t>(module.rows.size()) });
            lastLine = quoted.line;
            lastColumn = quoted.column;
            lastStart = 0;
        }
        QuotedTextRow row { .isOnSameLine = quoted.line == lastLine };
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

static void setEmptyQuotedTexts(TableEncoder& encoder)
{
    encoder.part.setTableRange(ProgramTableIndex::quotedTextGroups, encoder.part.allocate(0, alignof(QuotedTextGroupData)), 0);
    encoder.part.setTableRange(ProgramTableIndex::quotedTextRows, encoder.part.allocate(0), 0);
    encoder.part.setTableRange(ProgramTableIndex::textOfQuotes, encoder.part.allocate(0), 0);
}

static void encodeQuotedTexts(TableEncoder& encoder, StepTimes& times)
{
    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    if (leavesOutFunctionTexts(encoder)) {
        setEmptyQuotedTexts(encoder);
        return;
    }
    std::optional<PlacesOfQuotedTexts> places = findPlacesOfQuotedTexts(encoder);
    times.didFinish("find the places of quoted texts"_s);
    if (!places || !markQuotedTextsThatCodeReports(encoder, *places)) {
        setEmptyQuotedTexts(encoder);
        return;
    }
    times.didFinish("mark the quoted texts that code reports"_s);

    unsigned numberOfModules = static_cast<unsigned>(program.modules.size());
    Vector<QuotedTextsOfModule> modules(numberOfModules);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        collectQuotedTextsOfModule(encoder, *places, index, modules[index]);
    });
    *places = { };
    size_t numberOfRuns = 0;
    for (QuotedTextsOfModule& module : modules) {
        module.firstRun = numberOfRuns;
        numberOfRuns += module.outermost.size();
    }
    if (!checkListSize(numberOfRuns, sizeof(std::span<const uint8_t>), encoder.build)) {
        setEmptyQuotedTexts(encoder);
        return;
    }
    Vector<std::span<const uint8_t>> runs;
    runs.grow(numberOfRuns);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        std::span<const uint8_t> textOfModule = program.modules[index].positions.textOfQuotes;
        const QuotedTextsOfModule& module = modules[index];
        for (size_t run = 0; run < module.outermost.size(); ++run)
            runs[module.firstRun + run] = textOfModule.subspan(module.outermost[run].start, module.outermost[run].end - module.outermost[run].start);
    });
    times.didFinish("collect quoted texts"_s);

    PlacedRunsOfText placed = placeRunsOfText(loops, runs.span());
    times.didFinish("place quoted texts"_s);
    if (!checkSectionSize(placed.size, encoder.build)) {
        setEmptyQuotedTexts(encoder);
        return;
    }

    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        encodeRowsOfQuotedTextsOfModule(index, placed.offsets.span(), modules[index]);
    });
    size_t numberOfGroups = 0;
    uint64_t sizeOfRows = 0;
    for (QuotedTextsOfModule& module : modules) {
        module.firstGroup = numberOfGroups;
        module.offsetOfRows = sizeOfRows;
        numberOfGroups += module.groups.size();
        sizeOfRows += module.rows.size();
    }
    uint64_t sizeOfGroups = static_cast<uint64_t>(numberOfGroups) * sizeof(QuotedTextGroupData);
    if (!checkSectionSize(sizeOfGroups + sizeOfRows + placed.size, encoder.build)) {
        setEmptyQuotedTexts(encoder);
        return;
    }

    encoder.part.reserveRoomFor(sizeOfGroups + sizeOfRows + placed.size + 4 * sizeof(uint64_t));
    uint32_t offsetOfGroups = encoder.part.allocateUnfilled(sizeOfGroups, alignof(QuotedTextGroupData));
    uint32_t offsetOfRows = encoder.part.allocateUnfilled(sizeOfRows);
    uint32_t offsetOfText = encoder.part.allocateUnfilled(placed.size);
    encoder.part.setTableRange(ProgramTableIndex::quotedTextGroups, offsetOfGroups, static_cast<uint32_t>(numberOfGroups));
    encoder.part.setTableRange(ProgramTableIndex::quotedTextRows, offsetOfRows, static_cast<uint32_t>(sizeOfRows));
    encoder.part.setTableRange(ProgramTableIndex::textOfQuotes, offsetOfText, static_cast<uint32_t>(placed.size));
    std::span<uint8_t> groups = encoder.part.bytesAt(offsetOfGroups, sizeOfGroups);
    std::span<uint8_t> rows = encoder.part.bytesAt(offsetOfRows, sizeOfRows);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        QuotedTextsOfModule& module = modules[index];
        for (QuotedTextGroupData& group : module.groups)
            group.offsetOfRows += static_cast<uint32_t>(module.offsetOfRows);
        memcpySpan(groups.subspan(module.firstGroup * sizeof(QuotedTextGroupData)), asBytes(module.groups.span()));
        memcpySpan(rows.subspan(module.offsetOfRows), module.rows.span());
        module = { };
    });
    writeRunsOfText(loops, runs.span(), placed, encoder.part.bytesAt(offsetOfText, placed.size));
    times.didFinish("write quoted texts"_s);
}

void encodeFunctionTexts(TableEncoder& encoder)
{
    StepTimes times(encoder.build, "image: function texts"_s);
    encodeQuotedTexts(encoder, times);
    if (leavesOutFunctionTexts(encoder)) {
        setEmptyFunctionTexts(encoder);
        return;
    }

    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    const HostFunctionTexts& host = encoder.build.functionTexts();
    Vector<size_t> sizesOfTexts = measureFunctionTextsOfModules(program, host);
    Vector<ClosureFunctionWithText> functions = collectClosureFunctionsWithText(encoder, sizesOfTexts.span());
    Vector<KeptFunctionText> kept(functions.size(), [&](size_t index) {
        const Typed::Function& function = program.at(functions[index].function);
        return KeptFunctionText { function.module, function.text.start, function.text.end };
    });
    mergeKeptFunctionTexts(loops, kept);

    uint64_t sizeBeforeSharing = 0;
    for (const KeptFunctionText& range : kept)
        sizeBeforeSharing += range.size();
    times.didFinish("collect the functions with text"_s);
    if (functions.isEmpty() || !checkSectionSize(sizeBeforeSharing, encoder.build)) {
        setEmptyFunctionTexts(encoder);
        return;
    }

    Vector<KeptTextsOfModule> modules = groupKeptFunctionTextsByModule(kept.span());
    Vector<Vector<uint8_t>> textsOfHost(modules.size());
    Vector<std::span<const uint8_t>> runs;
    runs.grow(kept.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(modules.size()), [&](unsigned index) {
        const KeptTextsOfModule& module = modules[index];
        std::span<const uint8_t> text = program.at(module.module).functionText;
        if (text.empty()) {
            textsOfHost[index].grow(sizesOfTexts[module.module.index()]);
            host.copy(host.owner, module.module.index(), textsOfHost[index].mutableSpan().data());
            text = textsOfHost[index].span();
        }
        for (uint32_t range = module.first; range < module.end; ++range)
            runs[range] = text.subspan(kept[range].start, kept[range].size());
    });
    times.didFinish("take the texts of modules"_s);
    PlacedRunsOfText placed = placeRunsOfText(loops, runs.span());
    for (size_t range = 0; range < kept.size(); ++range)
        kept[range].offsetInKeptText = placed.offsets[range];
    times.didFinish("place function texts"_s);

    Vector<FunctionTextEntry> entries;
    entries.grow(functions.size());
    forEachRangeOfIndicesInParallel(loops, functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const Typed::Function& function = program.at(functions[index].function);
            const KeptFunctionText& range = findKeptFunctionText(kept, function);
            uint32_t start = range.offsetInKeptText + (function.text.start - range.start);
            entries[index] = FunctionTextEntry { Number24(functions[index].closureEntry.index()), Number32(start), Number32(start + (function.text.end - function.text.start)) };
        }
    });

    encoder.part.reserveRoomFor(entries.sizeInBytes() + placed.size + sizeof(FunctionTexts) + 4 * sizeof(uint64_t));
    uint32_t entriesOffset = encoder.part.appendUnshared(asBytes(entries.span()), alignof(FunctionTextEntry));
    uint32_t textOffset = encoder.part.allocateUnfilled(placed.size, 1);
    writeRunsOfText(loops, runs.span(), placed, encoder.part.bytesAt(textOffset, placed.size));
    uint32_t record = encoder.part.setTable(ProgramTableIndex::functionTexts, Vector<FunctionTexts, 1> { FunctionTexts { static_cast<uint32_t>(entries.size()), entriesOffset, textOffset } });
    encoder.part.link(record, &FunctionTexts::entriesOffset);
    encoder.part.link(record, &FunctionTexts::textOffset);
    forEachIndexInParallel(loops, static_cast<unsigned>(textsOfHost.size()), [&](unsigned index) {
        textsOfHost[index] = { };
    });
    times.didFinish("write function texts"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
