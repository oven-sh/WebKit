/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTDerivedFunctions.h"
#include "AOTFunctionKinds.h"
#include <algorithm>
#include <iterator>
#include <optional>

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
    if (std::optional<BuiltinID> builtin = trace.implements()) {
        BuiltinName builtinName = program.builtins().at(*builtin).name;
        if (StringID name = builtinName ? program.stringOf(builtinName) : StringID())
            return name;
    }
    return program.at(id).name;
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

static uint16_t encodedCaptureCountsOf(const TableEncoder& encoder, FunctionID id)
{
    const Program& program = encoder.program;
    if (program.at(id).role != FunctionRole::GenericEntrypoint)
        return 0;
    ClosureCaptureCounts counts = closureCaptureCountsOf(program, program.at(closureTargetOf(program, id)));
    if (counts.captures > UINT8_MAX) {
        encoder.fail(ProgramTableRule::TableOutOfBounds, { }, id);
        return 0;
    }
    return CaptureCounts { static_cast<uint8_t>(counts.references), static_cast<uint8_t>(counts.captures) }.encoded();
}

static void appendClosureFunctionTables(TableEncoder& encoder)
{
    ClosureFunctionRange range = encoder.placement.closureFunctions();
    uint32_t first = range.first.index();
    size_t count = range.end.index() - first;
    encoder.setTable(ProgramTableIndex::functionLengths, Vector<uint16_t>(count, [&](size_t index) {
        return encodedLengthOf(encoder, FunctionID(first + index));
    }));
    encoder.setTable(ProgramTableIndex::closureCaptureCounts, Vector<uint16_t>(count, [&](size_t index) {
        return encodedCaptureCountsOf(encoder, FunctionID(first + index));
    }));
}

static bool showsSourceText(FunctionRole role)
{
    switch (role) {
    case FunctionRole::Source:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ParameterWrapper:
        return true;
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::GroupEntrypoint:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CoroutineBody:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWorker:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Continuation:
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
    std::ranges::sort(ranges, [](const RetainedTextRange& a, const RetainedTextRange& b) {
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
    std::ranges::sort(entries, { }, &FunctionTextEntry::function);

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
        constructEntrypoints.append({ id.index(), constructEntrypoint.index() });
    }
    encoder.setTable(ProgramTableIndex::constructEntrypoints, constructEntrypoints);
}

static void appendFunctionTraces(TableEncoder& encoder, std::span<const FunctionTraceRecord> traces)
{
    encoder.setTable(ProgramTableIndex::functionTraces, Vector<FunctionTraceData>(traces.size(), [&](size_t index) -> FunctionTraceData {
        return traces[index];
    }));

    size_t firstDerived = 0;
    while (firstDerived < traces.size() && traces[firstDerived].sourceFunctionIndex == firstDerived)
        ++firstDerived;
    encoder.setTable(ProgramTableIndex::functionSources, Vector<uint32_t>(traces.size() - firstDerived, [&](size_t index) {
        return traces[firstDerived + index].sourceFunctionIndex;
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
    encoder.setTable(ProgramTableIndex::functionNames, Vector<uint32_t>(count, [&](size_t index) {
        return encoder.stringNumber(encoder.obfuscates() ? StringID() : functionNameOf(program, FunctionID(index), traces[index]));
    }));
    encoder.setTable(ProgramTableIndex::functionKinds, Vector<FunctionKindData>(count, [&](size_t index) {
        return encoder.placement.functionKind(FunctionID(index));
    }));
    appendFunctionTraces(encoder, traces.span());
    appendClosureFunctionTables(encoder);
    appendFunctionTexts(encoder);
    appendConstructEntrypoints(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
