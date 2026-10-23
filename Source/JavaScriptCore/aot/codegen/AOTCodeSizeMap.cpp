/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeSizeMap.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBlockFrequencies.h"
#include "AOTBranchCounterNumbers.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeGenerator.h"
#include "AOTCompileOptions.h"
#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include "AOTIRPrinter.h"
#include "AOTRelocation.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "B3PCToOriginMap.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Label {
    Opcode opcode { Opcode::Undefined };
    BuiltinOverloadID overload;
    bool hasValue { false };
    bool isInlined { false };

    static Label labelOf(const Function&, ValueID);
    void print(PrintStream&, const Program&) const;
};

struct LabelTotal {
    Label label;
    uint64_t bytes { 0 };
};

struct EmittedRange {
    ValueID value;
    uint32_t offset { 0 };
    uint32_t size { 0 };
};

} // anonymous namespace

Label Label::labelOf(const Function& function, ValueID id)
{
    if (!id)
        return { };
    const Value& value = function.at(id);
    return Label {
        .opcode = value.opcode,
        .overload = Value::opcodesWithOverload.contains(value.opcode) ? value.overload() : BuiltinOverloadID(),
        .hasValue = true,
        .isInlined = !!value.inlineFrame,
    };
}

void Label::print(PrintStream& out, const Program& program) const
{
    if (!hasValue) {
        out.print("-");
        return;
    }
    out.print(nameOf(opcode));
    if (overload)
        out.print(".", program.builtins().tokenOf(overload));
    if (isInlined)
        out.print("+");
}

static ASCIILiteral operandClassOf(const Function& function, ValueID id)
{
    if (!id)
        return "-"_s;
    const Value& value = function.at(id);
    if (!value.numberOfOperands() || !value.operand(0))
        return "-"_s;
    return nameOf(function.at(value.operand(0)).representation);
}

static Vector<EmittedRange> collectCodeRanges(const Function& function, const B3::PCToOriginMap& map, ImageLinkBuffer& buffer)
{
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    auto& ranges = map.ranges();
    uint32_t codeSize = static_cast<uint32_t>(buffer.bytes().size());
    auto offsetOf = [&](size_t i) {
        return i < ranges.size() ? std::min(linkBuffer.offsetOf(ranges[i].label), codeSize) : codeSize;
    };
    Vector<EmittedRange> result;
    result.reserveInitialCapacity(ranges.size() + 1);
    uint32_t start = offsetOf(0);
    if (start)
        result.append(EmittedRange { .size = start });
    for (size_t i = 0; i < ranges.size(); ++i) {
        uint32_t end = offsetOf(i + 1);
        if (end <= start)
            continue;
        ValueID value = valueOfOrigin(ranges[i].origin);
        if (value && value.index() >= function.values.size())
            value = { };
        result.append(EmittedRange { .value = value, .offset = start, .size = end - start });
        start = end;
    }
    return result;
}

static void printTotals(PrintStream& out, const Program& program, const Function& function, const Vector<EmittedRange>& ranges)
{
    auto keyOf = [](const Label& label) -> uint64_t {
        if (!label.hasValue)
            return 0;
        uint64_t overload = label.overload ? label.overload.index() + 1 : 0;
        return 1 + (static_cast<uint64_t>(label.opcode) << 34 | overload << 1 | label.isInlined);
    };
    UncheckedKeyHashMap<uint64_t, LabelTotal, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> totals;
    for (const EmittedRange& range : ranges) {
        Label label = Label::labelOf(function, range.value);
        totals.add(keyOf(label), LabelTotal { label }).iterator->value.bytes += range.size;
    }
    Vector<std::pair<uint64_t, LabelTotal>> sorted;
    for (auto& entry : totals)
        sorted.append({ entry.key, entry.value });
    AOT::sort(sorted, { }, &std::pair<uint64_t, LabelTotal>::first);
    for (auto& [key, total] : sorted) {
        out.print("\t");
        total.label.print(out, program);
        out.print("\t", total.bytes);
    }
}

static void printRanges(PrintStream& out, const Program& program, const Function& function, const BlockCycles& cycles, const UseLists& useLists, const Vector<EmittedRange>& ranges)
{
    out.print("\nS");
    for (const EmittedRange& range : ranges) {
        out.print("\t");
        Label::labelOf(function, range.value).print(out, program);
        BlockID block = range.value ? useLists.blockOf(range.value) : BlockID();
        if (block && cycles.isInCycle(block))
            out.print("*");
        out.print("\t");
        if (range.value)
            out.print(range.value.index());
        else
            out.print("-");
        out.print("\t", operandClassOf(function, range.value), "\t", range.offset, "\t", range.size);
    }
}

static char letterOf(CodeShape shape)
{
    switch (shape) {
    case CodeShape::Unchosen:
        return 'u';
    case CodeShape::Compact:
        return 'c';
    case CodeShape::Balanced:
        return 'b';
    case CodeShape::Fast:
        return 'f';
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Bytes by operation and by the code shape of its block.
static void printShapes(PrintStream& out, const Function& function, const UseLists& useLists, const Vector<EmittedRange>& ranges)
{
    constexpr unsigned shapeBits = 2;
    static_assert(static_cast<unsigned>(CodeShape::Fast) < 1u << shapeBits);
    Vector<std::pair<uint32_t, uint32_t>> bytesByOperationAndShape;
    for (const EmittedRange& range : ranges) {
        BlockID block = range.value ? useLists.blockOf(range.value) : BlockID();
        if (block)
            bytesByOperationAndShape.append({ static_cast<uint32_t>(function.at(range.value).opcode) << shapeBits | static_cast<uint32_t>(function.at(block).shape), range.size });
    }
    AOT::sort(bytesByOperationAndShape);
    out.print("\nH");
    for (size_t first = 0; first < bytesByOperationAndShape.size();) {
        uint32_t key = bytesByOperationAndShape[first].first;
        uint64_t bytes = 0;
        for (; first < bytesByOperationAndShape.size() && bytesByOperationAndShape[first].first == key; ++first)
            bytes += bytesByOperationAndShape[first].second;
        out.print("\t", nameOf(static_cast<Opcode>(key >> shapeBits)), "\t", letterOf(static_cast<CodeShape>(key & ((1u << shapeBits) - 1))), "\t", bytes);
    }
}

static void printFieldKeys(PrintStream& out, const Layout& layout, std::span<const uint32_t> fields)
{
    if (fields.empty()) {
        out.print("-");
        return;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        out.print(i ? "," : "");
        if (PropertyKeyID key = layout.fields[fields[i]].key)
            out.print(key.index());
        else
            out.print("?");
    }
}

static void printLiterals(PrintStream& out, const Program& program, const Function& function, const CFG& cfg, const BlockCycles& cycles)
{
    bool hasLiterals = false;
    for (unsigned index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        if (!cfg.isReachable(block))
            continue;
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::NewObject)
                continue;
            const Layout& layout = program.at(value.layout());
            if (layout.kind != LayoutKind::Record)
                continue;
            if (!std::exchange(hasLiterals, true))
                out.print("\nV");
            unsigned computedOperands = 0;
            for (ValueID operand : value.valuesOperands())
                computedOperands += operand && !isRematerializable(function.at(operand));
            out.print("\t", id.index(), "\to", cycles.isInCycle(block) ? "l" : "", "\t", computedOperands, "\t");
            printFieldKeys(out, layout, value.fields());
        }
    }
}

static String pathOf(const Program& program, ModuleID module)
{
    Vector<uint8_t> path(program.string(program.at(module).path));
    for (uint8_t& byte : path) {
        if (byte < 0x20)
            byte = '?';
    }
    return String::fromUTF8ReplacingInvalidSequences(path.span());
}

static void printHints(PrintStream& out, const Program& program, const Function& function, bool ignoresBranchHints)
{
    CFG cfg(function);
    for (unsigned index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        if (!cfg.isReachable(block))
            continue;
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.targets.size() < 2 || !terminator.hasPosition())
            continue;
        ModuleID module = program.sourceFunctionOf(function, terminator).module;
        if (!module)
            continue;
        forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
            if (edge.isHandlerEdge() || recordedFrequencyUseOf(function, cfg, target.block, ignoresBranchHints) != RecordedFrequencyUse::Applied)
                return;
            out.print("\nW\t", pathOf(program, module), "\t", terminator.position, "\t", edge.targetIndex, "\t", nameOf(function.at(target.block).frequency));
        });
    }
}

struct OriginOfTestedValue {
    ASCIILiteral place { "Operation"_s };
    Opcode opcode { Opcode::Undefined };
    uint32_t earlierTypeTests { 0 };
    uint32_t earlierOtherTests { 0 };
    uint32_t merges { 0 };
};

static ValueID argumentOnEdge(const Function& function, Edge edge, uint32_t index)
{
    if (edge.isHandlerEdge())
        return { };
    const Terminator& terminator = function.at(edge.from).terminator;
    if (edge.targetIndex >= terminator.targets.size() || index >= terminator.targets[edge.targetIndex].arguments.size())
        return { };
    return terminator.targets[edge.targetIndex].arguments[index];
}

static OriginOfTestedValue findOriginOfTestedValue(const Function& function, const CFG& cfg, const UseLists& useLists, ValueID value)
{
    OriginOfTestedValue origin;
    for (uint32_t steps = 0; steps < 64; ++steps) {
        value = sameObjectThroughCasts(function, value);
        ValueLocation location = useLists.locationOf(value);
        if (location.placement != ValuePlacement::Parameter) {
            origin.opcode = function.at(value).opcode;
            return origin;
        }
        if (!location.block.index()) {
            origin.place = "Parameter"_s;
            return origin;
        }
        std::span<const Edge> incoming = cfg.incomingEdges(location.block);
        if (incoming.empty()) {
            origin.place = "Handler"_s;
            return origin;
        }
        Edge edge = incoming[0];
        if (edge.isHandlerEdge()) {
            origin.place = "Handler"_s;
            return origin;
        }
        uint32_t firstResult = firstEdgeResultParameter(function, edge.from, edge.targetIndex);
        if (incoming.size() == 1 && location.index >= firstResult) {
            std::optional<EdgeResult> result = tryGetEdgeResult(function, { edge.from, edge.targetIndex, location.index - firstResult });
            if (!result || result->kind != EdgeResultKind::NarrowedOperand || !result->subject) {
                origin.place = "ResultOfTerminator"_s;
                return origin;
            }
            if (result->test && function.at(result->test).opcode == Opcode::TypeTest)
                ++origin.earlierTypeTests;
            else
                ++origin.earlierOtherTests;
            value = result->subject;
            continue;
        }
        if (incoming.size() > 1) {
            ValueID only;
            bool isSame = true;
            for (Edge other : incoming) {
                ValueID argument = argumentOnEdge(function, other, location.index);
                if (!argument) {
                    isSame = false;
                    break;
                }
                argument = sameObjectThroughCasts(function, argument);
                if (argument == value)
                    continue;
                if (only && only != argument)
                    isSame = false;
                only = argument;
            }
            if (!isSame || !only) {
                origin.place = "Merge"_s;
                return origin;
            }
            ++origin.merges;
            value = only;
            continue;
        }
        value = argumentOnEdge(function, edge, location.index);
        if (!value) {
            origin.place = "Merge"_s;
            return origin;
        }
    }
    origin.place = "Merge"_s;
    return origin;
}

static void printTestOfCounters(PrintStream& out, const Program& program, const Function& function, const CFG& cfg, const UseLists& useLists, const CountedBranch& branch, uint32_t trueSide)
{
    const Terminator& terminator = function.at(branch.block).terminator;
    if (terminator.kind != TerminatorKind::Branch && terminator.kind != TerminatorKind::TestAndBranch)
        return;
    const Value* test = &function.at(terminator.conditionOperand());
    while (test->opcode == Opcode::LogicalNot)
        test = &function.at(test->valueOperand());
    BlockID targetWhenTrue = terminator.targets[branch.isNegated ? 1 : 0].block;
    BlockID targetWhenFalse = terminator.targets[branch.isNegated ? 0 : 1].block;
    out.print("\nq\t", trueSide, "\t", nameOf(test->opcode), "\t", nameOf(function.at(targetWhenTrue).frequency), "\t", nameOf(function.at(targetWhenFalse).frequency), "\t", nameOf(function.at(branch.block).frequency));
    if (!test->numberOfOperands() || !test->operand(0))
        return;
    OriginOfTestedValue origin = findOriginOfTestedValue(function, cfg, useLists, test->operand(0));
    out.print("\t", origin.place, "\t", nameOf(origin.opcode), "\t", origin.earlierTypeTests, "\t", origin.earlierOtherTests, "\t", origin.merges, "\t");
    printType(out, program, program.typeOf(function, test->operand(0)));
    out.print("\t");
    if (test->opcode == Opcode::TypeTest)
        printType(out, program, test->testedType());
}

static void printCounters(PrintStream& out, const CodeGenerationInputs& inputs, FunctionID id, const Function& function)
{
    if (!inputs.branchCounters.isCounting())
        return;
    if (std::optional<uint32_t> entries = inputs.branchCounters.counterOfEntries(id); entries && function.module)
        out.print("\nQE\t", *entries, "\t", function.module.index(), "\t", function.position, "\t", pathOf(inputs.program, function.module));
    Vector<CountedBranch> branches = findCountedBranches(inputs.program, function);
    CFG cfg(function);
    UseLists useLists(function, cfg);
    for (uint32_t ordinal = 0; ordinal < branches.size(); ++ordinal) {
        std::optional<uint32_t> taken = inputs.branchCounters.counterFor(id, ordinal, 0);
        std::optional<uint32_t> notTaken = inputs.branchCounters.counterFor(id, ordinal, 1);
        if (!taken || !notTaken)
            continue;
        const CountedBranch& branch = branches[ordinal];
        uint32_t trueSide = branch.isNegated ? *notTaken : *taken;
        uint32_t falseSide = branch.isNegated ? *taken : *notTaken;
        out.print("\nQ\t", trueSide, "\t", falseSide, "\t", branch.module.index(), "\t", branch.position, "\t", pathOf(inputs.program, branch.module));
        printTestOfCounters(out, inputs.program, function, cfg, useLists, branch, trueSide);
    }
}

String describeCodeSize(const Program& program, const Function& function, const B3::PCToOriginMap& map, ImageLinkBuffer& buffer)
{
    CFG cfg(function);
    BlockCycles cycles(cfg);
    UseLists useLists(function, cfg);
    Vector<EmittedRange> ranges = collectCodeRanges(function, map, buffer);
    StringPrintStream out;
    printTotals(out, program, function, ranges);
    printRanges(out, program, function, cycles, useLists, ranges);
    printShapes(out, function, useLists, ranges);
    printLiterals(out, program, function, cfg, cycles);
    return out.toString();
}

String describeBranches(const CodeGenerationInputs& inputs, FunctionID id, const Function& function, const RecordedOptions& options)
{
    StringPrintStream out;
    printHints(out, inputs.program, function, options.has(TestingSwitch::IgnoreBranchHints));
    printCounters(out, inputs, id, function);
    return out.toString();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
