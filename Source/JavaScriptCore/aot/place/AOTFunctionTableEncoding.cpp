/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCreateEntrypoints.h"
#include "AOTDerivedFunctions.h"
#include "AOTFunctionKinds.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <iterator>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxCaptures, hostArchitecture()) <= UINT8_MAX);

namespace {

struct RetainedTextRange {
    uint32_t start;
    uint32_t end;
    uint32_t textOffset { 0 };
};

struct FunctionWithText {
    FunctionID function;
    FunctionID entrypoint;
};

} // anonymous namespace

static StringID functionNameOf(const Program& program, FunctionID id, const FunctionTraceRecord& trace)
{
    StringID written = program.at(id).name;
    std::optional<BuiltinID> builtin = trace.implements();
    if (!builtin || !written || program.builtins().at(*builtin).kind == BuiltinKind::Accessor)
        return written;
    BuiltinName builtinName = program.builtins().at(*builtin).name;
    StringID name = builtinName ? program.stringOf(builtinName) : StringID();
    return name ? name : written;
}

static uint16_t encodedLengthOf(const TableEncoder& encoder, FunctionID id)
{
    const Program& program = encoder.program;
    uint32_t length = program.at(closureTargetOf(program, id)).length;
    if (length > UINT16_MAX) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        return 0;
    }
    return static_cast<uint16_t>(length);
}

static CaptureCounts captureCountsOf(const TableEncoder& encoder, FunctionID id)
{
    const Program& program = encoder.program;
    if (program.at(id).role != FunctionRole::GenericEntrypoint)
        return { 0, 0 };
    ClosureCaptureCounts counts = closureCaptureCountsOf(program, program.at(closureTargetOf(program, id)));
    if (counts.captures > UINT8_MAX) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        return { 0, 0 };
    }
    return { static_cast<uint8_t>(counts.references), static_cast<uint8_t>(counts.captures) };
}

// Fails unless the numbers are unique, dense and start at 1.
static uint32_t countClosureCells(const TableEncoder& encoder, ClosureFunctionRange range)
{
    uint32_t numberOfFunctions = encoder.program.functions.size();
    BitVector usedNumbers(numberOfFunctions + 1);
    uint32_t numberOfCells = 1;
    uint32_t maxNumber = 0;
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(index);
        uint32_t number = encoder.placement.closureCellNumber(id);
        if (!number)
            continue;
        if (id < range.first || id >= range.end || number > numberOfFunctions || usedNumbers.quickSet(number)) {
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
            return numberOfCells;
        }
        maxNumber = std::max(maxNumber, number);
        ++numberOfCells;
    }
    if (maxNumber + 1 != numberOfCells)
        encoder.fail(ProgramTableRule::TableOutOfBounds);
    return numberOfCells;
}

static void appendClosureFunctionTables(TableEncoder& encoder)
{
    ClosureFunctionRange range = encoder.placement.closureFunctions();
    uint32_t first = range.first.index();
    size_t count = range.end.index() - first;
    encoder.setTable(ProgramTableIndex::closureFunctionKinds, Vector<FunctionKindData>(count, [&](size_t index) {
        return encoder.placement.functionKind(FunctionID(first + index));
    }));
    Vector<uint8_t> lengths;
    Vector<uint8_t> captureCounts;
    Vector<FunctionWithLargeCountsData> functionsWithLargeCounts;
    lengths.reserveInitialCapacity(count);
    captureCounts.reserveInitialCapacity(count);
    for (size_t index = 0; index < count; ++index) {
        FunctionID function(first + index);
        uint16_t length = encodedLengthOf(encoder, function);
        CaptureCounts counts = captureCountsOf(encoder, function);
        lengths.append(length < countsAreListed ? static_cast<uint8_t>(length) : countsAreListed);
        captureCounts.append(counts.encoded());
        if (lengths.last() == countsAreListed || captureCounts.last() == countsAreListed)
            functionsWithLargeCounts.append({ Number24(function.index()), counts, { static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8) } });
    }
    encoder.setTable(ProgramTableIndex::functionLengths, lengths);
    encoder.setTable(ProgramTableIndex::closureCaptureCounts, captureCounts);
    encoder.setTable(ProgramTableIndex::functionsWithLargeCounts, functionsWithLargeCounts);
    encoder.setTable(ProgramTableIndex::closureCellNumbers, Vector<Number24>(count, [&](size_t index) {
        return Number24(encoder.placement.closureCellNumber(FunctionID(first + index)));
    }));
    encoder.header.numberOfClosureCells = countClosureCells(encoder, range);
}

static bool showsSourceText(FunctionRole role)
{
    switch (role) {
    case FunctionRole::Source:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
        return true;
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
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
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasFunctionText(const Program& program, const Function& function)
{
    if (!function.module || function.text.start >= function.text.end)
        return false;
    return showsSourceText(function.role) && !function.enclosingRuntimeFunction && program.at(function.module).kind != ModuleKind::RuntimeSource;
}

static Vector<FunctionWithText> collectFunctionsWithText(const TableEncoder& encoder)
{
    Vector<FunctionWithText> functions;
    if (encoder.obfuscates())
        return functions;
    const Program& program = encoder.program;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!hasFunctionText(program, function))
            continue;
        FunctionID id(index);
        FunctionID entrypoint = encoder.findEntrypoint(id);
        if (!entrypoint)
            continue;
        if (function.text.end > program.at(function.module).functionText.size()) {
            encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
            continue;
        }
        functions.append({ id, entrypoint });
    }
    return functions;
}

static void mergeTextRanges(Vector<RetainedTextRange>& ranges)
{
    AOT::sort(ranges, [](const RetainedTextRange& a, const RetainedTextRange& b) {
        return a.start != b.start ? a.start < b.start : a.end > b.end;
    });
    size_t count = 0;
    for (const RetainedTextRange& next : ranges) {
        if (count && next.start < ranges[count - 1].end)
            ranges[count - 1].end = std::max(ranges[count - 1].end, next.end);
        else
            ranges[count++] = next;
    }
    ranges.shrink(count);
}

static void appendFunctionTexts(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<FunctionWithText> functions = collectFunctionsWithText(encoder);
    if (functions.isEmpty()) {
        encoder.setTable(ProgramTableIndex::functionTexts, Vector<FunctionTexts> { });
        return;
    }

    Vector<Vector<RetainedTextRange>> moduleRanges(program.modules.size());
    for (const FunctionWithText& entry : functions) {
        const Function& function = program.at(entry.function);
        moduleRanges[function.module.index()].append({ function.text.start, function.text.end });
    }

    Vector<uint8_t> text;
    for (size_t module = 0; module < moduleRanges.size(); ++module) {
        mergeTextRanges(moduleRanges[module]);
        for (RetainedTextRange& range : moduleRanges[module]) {
            range.textOffset = static_cast<uint32_t>(text.size());
            text.append(program.modules[module].functionText.subspan(range.start, range.end - range.start));
        }
    }

    Vector<FunctionTextEntry> entries(functions.size(), [&](size_t index) {
        const Function& function = program.at(functions[index].function);
        auto& ranges = moduleRanges[function.module.index()];
        auto next = std::ranges::upper_bound(ranges, function.text.start, { }, &RetainedTextRange::start);
        const RetainedTextRange& range = *std::prev(next);
        uint32_t start = range.textOffset + function.text.start - range.start;
        return FunctionTextEntry { functions[index].entrypoint.index(), start, start + function.text.end - function.text.start };
    });
    AOT::sort(entries, { }, &FunctionTextEntry::function);

    uint32_t entriesOffset = encoder.writer.append(entries);
    uint32_t textOffset = encoder.writer.append(text);
    encoder.setTable(ProgramTableIndex::functionTexts, Vector<FunctionTexts, 1> { FunctionTexts { static_cast<uint32_t>(entries.size()), entriesOffset, textOffset } });
}

static void appendConstructEntrypoints(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<ConstructEntrypointData> constructEntrypoints;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (function.role != FunctionRole::GenericEntrypoint || !function.hasBody())
            continue;
        FunctionID constructEntrypoint = function.creator.as<CreatorRecord::Entrypoint>().constructEntrypoint;
        if (!constructEntrypoint)
            continue;
        FunctionID id(index);
        if (!program.at(constructEntrypoint).hasBody()) {
            encoder.fail(ProgramTableRule::FunctionWithoutEntrypoint, { }, id);
            continue;
        }
        bool allocatesThis = program.at(constructEntrypoint).derivation.parameter == allocatingConstructEntrypointParameter;
        constructEntrypoints.append({ id.index(), constructEntrypoint.index() | (allocatesThis ? ConstructEntrypointData::allocatesThis : 0) });
    }
    encoder.setTable(ProgramTableIndex::constructEntrypoints, constructEntrypoints);
}

static size_t findFirstDerivedFunction(std::span<const FunctionTraceRecord> traces)
{
    size_t firstDerived = 0;
    while (firstDerived < traces.size() && traces[firstDerived].sourceFunctionIndex == firstDerived)
        ++firstDerived;
    return firstDerived;
}

static void appendFunctionNames(TableEncoder& encoder, std::span<const FunctionTraceRecord> traces)
{
    // Not context.limit(): a test that lowers it means the list of long strings.
    bool numbersFit = encoder.program.strings.size() <= Number24::max;
    if (!numbersFit)
        encoder.build.reportLimitExceeded(LimitID::MaxListedStrings, encoder.program.strings.size());
    auto nameOf = [&](size_t index) {
        uint32_t number = encoder.shownStringNumber(encoder.obfuscates() ? StringID() : functionNameOf(encoder.program, FunctionID(index), traces[index]));
        return Number24(numbersFit ? number : 0);
    };
    size_t firstDerived = findFirstDerivedFunction(traces);
    Vector<Number24> names;
    Vector<uint32_t> groups(FillWith { }, ProgramTables::numberOfFunctionNameGroupWords(firstDerived), 0);
    for (size_t index = 0; index < firstDerived; ++index) {
        if (!(index % 32))
            groups[index / 32 * 2 + 1] = names.size();
        Number24 name = nameOf(index);
        if (!name)
            continue;
        groups[index / 32 * 2] |= 1u << index % 32;
        names.append(name);
    }
    encoder.setTable(ProgramTableIndex::functionNames, names);
    encoder.setTable(ProgramTableIndex::functionNameGroups, groups);

    Vector<FunctionNameRunData> runs;
    for (size_t index = firstDerived; index < traces.size(); ++index) {
        if (traces[index].sourceFunctionIndex != index)
            continue;
        uint32_t name = nameOf(index);
        if (runs.isEmpty() || runs.last().name != name)
            runs.append({ static_cast<uint32_t>(index), name });
    }
    encoder.setTable(ProgramTableIndex::functionNameRuns, runs);
}

// The most common pairs get the numbers. The order depends on nothing but the pairs and how often each occurs.
NumberedFunctionRecords numberFunctionRecords(std::span<const FunctionRecordData> recordsOfFunctions)
{
    struct DistinctRecord {
        uint32_t bits;
        uint32_t count;
        uint32_t number;
    };
    Vector<uint32_t> sortedBits(recordsOfFunctions.size(), [&](size_t index) {
        return std::bit_cast<uint32_t>(recordsOfFunctions[index]);
    });
    AOT::sort(sortedBits);
    Vector<DistinctRecord> distinct;
    for (uint32_t bits : sortedBits) {
        if (distinct.isEmpty() || distinct.last().bits != bits)
            distinct.append({ bits, 0, 0 });
        ++distinct.last().count;
    }
    AOT::stableSort(distinct, std::ranges::greater { }, &DistinctRecord::count);
    for (uint32_t number = 0; number < distinct.size(); ++number)
        distinct[number].number = number;

    NumberedFunctionRecords result;
    result.records = Vector<FunctionRecordData>(std::min<size_t>(distinct.size(), functionRecordIsListedByFunction), [&](size_t number) {
        return std::bit_cast<FunctionRecordData>(distinct[number].bits);
    });
    AOT::sort(distinct, { }, &DistinctRecord::bits);
    result.numbers = Vector<uint8_t>(recordsOfFunctions.size(), [&](size_t index) {
        uint32_t number = std::ranges::lower_bound(distinct, std::bit_cast<uint32_t>(recordsOfFunctions[index]), { }, &DistinctRecord::bits)->number;
        if (number < functionRecordIsListedByFunction)
            return static_cast<uint8_t>(number);
        result.recordsByFunction.append({ static_cast<uint32_t>(index), recordsOfFunctions[index] });
        return functionRecordIsListedByFunction;
    });
    return result;
}

static void appendFunctionRecords(TableEncoder& encoder, std::span<const FunctionTraceRecord> traces)
{
    NumberedFunctionRecords numbered = numberFunctionRecords(Vector<FunctionRecordData>(traces.size(), [&](size_t index) {
        return FunctionRecordData { encoder.placement.functionKind(FunctionID(index)), traces[index] };
    }).span());
    encoder.setTable(ProgramTableIndex::functionRecordNumbers, numbered.numbers);
    encoder.setTable(ProgramTableIndex::functionRecords, numbered.records);
    encoder.setTable(ProgramTableIndex::functionRecordsByFunction, numbered.recordsByFunction);
}

static void appendFunctionSources(TableEncoder& encoder, std::span<const FunctionTraceRecord> traces)
{
    size_t firstDerived = findFirstDerivedFunction(traces);
    encoder.setTable(ProgramTableIndex::functionSources, Vector<Number24>(traces.size() - firstDerived, [&](size_t index) {
        return Number24(traces[firstDerived + index].sourceFunctionIndex);
    }));

    Vector<ImplementedBuiltinData> implementedBuiltins;
    for (uint32_t index = 0; index < traces.size(); ++index) {
        if (std::optional<BuiltinID> builtin = traces[index].implements())
            implementedBuiltins.append({ index, builtin->index() });
    }
    encoder.setTable(ProgramTableIndex::implementedBuiltins, implementedBuiltins);
}

void encodeFunctionTables(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    size_t count = program.functions.size();
    Vector<FunctionTraceRecord> traces(count, [&](size_t index) {
        return functionTraceOf(program, FunctionID(index));
    });
    appendFunctionNames(encoder, traces.span());
    appendFunctionRecords(encoder, traces.span());
    appendFunctionSources(encoder, traces.span());
    appendClosureFunctionTables(encoder);
    appendFunctionTexts(encoder);
    appendConstructEntrypoints(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
