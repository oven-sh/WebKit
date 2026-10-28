/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsTestCommands.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"
#include "AOTVector.h"
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EffectsTestInputs {
    FlowResults flow;
    EffectSummaries summaries;
    bool hasSummaries { false };
};

enum class BeyondOperands : uint8_t { Omit, Print };

class LocatedAccessCollector {
public:
    void reads(const MemoryLocation& location) { add(location); }
    void writes(const MemoryLocation& location) { add(location); }
    void readsAllHeaps() { }
    void throws() { }
    void collects() { }
    void suspends() { }
    void terminates() { }
    void unknownCode() { }

    std::span<const MemoryLocation> locations() const LIFETIME_BOUND { return m_locations.span(); }

private:
    static constexpr AbstractHeapSet comparedHeaps { AbstractHeap::ShapeWords, AbstractHeap::Elements };

    void add(const MemoryLocation& location)
    {
        if (!location.isEntireHeap() && comparedHeaps.contains(location.heap()))
            m_locations.add(location);
    }

    SortedSet<MemoryLocation> m_locations;
};

enum class OverlapSubjects : uint8_t { ExactAccesses, StretchTests };

class ReadLocationCollector {
public:
    void reads(const MemoryLocation& location) { m_locations.append(location); }
    void writes(const MemoryLocation&) { }
    void readsAllHeaps() { }
    void throws() { }
    void collects() { }
    void suspends() { }
    void terminates() { }
    void unknownCode() { }

    std::span<const MemoryLocation> locations() const LIFETIME_BOUND { return m_locations.span(); }

private:
    Vector<MemoryLocation, 4> m_locations;
};

struct ProtocolsArgument {
    Protocols protocols;
    Vector<BuiltinName> namedMembers;
    Vector<BuiltinName> calledMembers;
};

} // anonymous namespace

static std::optional<Vector<FunctionID>> parseFunctionWords(TestCommandContext& context, std::span<const String> words)
{
    const Program& program = *context.program;
    Vector<FunctionID> functions;
    if (words.empty()) {
        for (size_t index = 0; index < program.functions.size(); ++index)
            functions.append(FunctionID(index));
        return functions;
    }
    auto parseFunction = idParser<FunctionID>('f', program.functions.size());
    for (const String& word : words) {
        std::optional<FunctionID> function = parseFunction(word);
        if (!function) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        functions.append(*function);
    }
    return functions;
}

static std::optional<ProtocolsArgument> parseProtocolsWord(const BuiltinTable& table, StringView word)
{
    ProtocolsArgument argument;
    for (StringView part : word.splitAllowingEmptyEntries('+')) {
        size_t colon = part.find(':');
        std::optional<Protocol> protocol = parseEnum<Protocol>(colon == notFound ? part : part.left(colon));
        if (!protocol)
            return std::nullopt;
        argument.protocols.add(*protocol);
        if (colon == notFound)
            continue;
        if (*protocol != Protocol::Named && *protocol != Protocol::CallsNamed)
            return std::nullopt;
        Vector<BuiltinName>& members = *protocol == Protocol::Named ? argument.namedMembers : argument.calledMembers;
        for (StringView text : part.substring(colon + 1).splitAllowingEmptyEntries(',')) {
            BuiltinName name = table.nameOf(byteCast<uint8_t>(text.utf8().span()));
            if (!name)
                return std::nullopt;
            members.append(name);
        }
    }
    return argument;
}

static std::span<const String> takeInputWords(TestCommandContext& context, std::span<const String> words, EffectsTestInputs& inputs)
{
    if (!words.empty() && words.front() == "flow"_s) {
        inputs.flow = analyzeValueFlow(*context.program, context.program->types, context.build);
        words = words.subspan(1);
    }
    if (!words.empty() && words.front() == "summaries"_s) {
        inputs.summaries = computeEffectSummaries(*context.program, inputs.flow, context.build);
        inputs.hasSummaries = true;
        words = words.subspan(1);
    }
    if (!words.empty() && words.front() == "summaries-by-types-of-operands"_s) {
        inputs.summaries = computeEffectSummariesByTypesOfOperands(*context.program, inputs.flow, context.build);
        inputs.hasSummaries = true;
        words = words.subspan(1);
    }
    constexpr ASCIILiteral summariesBefore = "summaries-before=f"_s;
    if (!words.empty() && words.front().startsWith(summariesBefore)) {
        unsigned numberOfFunctions = parseInteger<unsigned>(StringView(words.front()).substring(summariesBefore.length())).value_or(0);
        EffectSummaries summariesOfAll = computeEffectSummaries(*context.program, inputs.flow, context.build);
        Vector<FunctionSummary> summariesOfFirst(numberOfFunctions, [&](size_t index) {
            return summariesOfAll.of(FunctionID(index)).expanded();
        });
        inputs.summaries = EffectSummaries(summariesOfFirst.span(), summariesOfAll.unknownCode());
        inputs.hasSummaries = true;
        words = words.subspan(1);
    }
    return words;
}

static void forEachFunctionWithBody(TestCommandContext& context, std::span<const FunctionID> functions, NOESCAPE const Invocable<void(const Function&)> auto& functor)
{
    for (FunctionID id : functions) {
        const Function& function = context.program->at(id);
        if (!function.hasBody())
            continue;
        context.out.println("function f"_s, id.index());
        functor(function);
    }
}

static void forEachListedValue(const Function& function, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            functor(id);
    }
}

static void runOnFunctions(TestCommandContext& context, NOESCAPE const Invocable<void(const EffectContext&, const Function&)> auto& print)
{
    EffectsTestInputs inputs;
    std::span<const String> words = takeInputWords(context, context.arguments, inputs);
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;

    EffectContext effectContext { *context.program, inputs.summaries };
    forEachFunctionWithBody(context, functions->span(), [&](const Function& function) {
        print(effectContext, function);
    });
}

static void printValidIfNoErrors(TestCommandContext& context)
{
    if (!context.build.diagnostics().hasErrors())
        context.out.println("valid"_s);
}

static void printValueEffects(PrintStream& out, const EffectContext& context, const Function& function, ValueID id, BeyondOperands beyondOperands)
{
    const Value& value = function.at(id);
    EffectsOnEdges effects = effectsOf(context, function, value);
    out.print("v"_s, id.index(), " "_s, nameOf(value.opcode), ": "_s, effects);
    if (beyondOperands == BeyondOperands::Print) {
        EffectsOnEdges beyond = effectsBeyondOperands(context, function, value, effects);
        if (beyond != effects)
            out.print(" | beyond operands: "_s, beyond);
    }
    out.print("\n"_s);
}

static void printBlockEffects(PrintStream& out, const EffectContext& context, const Function& function, size_t blockIndex, BeyondOperands beyondOperands)
{
    const Block& block = function.blocks[blockIndex];
    for (ValueID id : block.values)
        printValueEffects(out, context, function, id, beyondOperands);
    out.println("b"_s, blockIndex, " "_s, nameOf(block.terminator.kind), ": "_s, effectsOf(context, function, block.terminator));
}

static void runEffects(TestCommandContext& context)
{
    EffectsTestInputs inputs;
    std::span<const String> words = takeInputWords(context, context.arguments, inputs);
    BeyondOperands beyondOperands = BeyondOperands::Omit;
    if (!words.empty() && words.front() == "beyond"_s) {
        beyondOperands = BeyondOperands::Print;
        words = words.subspan(1);
    }
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;

    const Program& program = *context.program;
    PrintStream& out = context.out;
    if (inputs.hasSummaries)
        inputs.summaries.dump(out, program);
    EffectContext effectContext { program, inputs.summaries };
    forEachFunctionWithBody(context, functions->span(), [&](const Function& function) {
        for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex)
            printBlockEffects(out, effectContext, function, blockIndex, beyondOperands);
    });
}

static void printMemoryAccess(PrintStream& out, const Program& program, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    MemoryAccess access = memoryAccessOf(function, value);
    out.print("v"_s, id.index(), " "_s, nameOf(value.opcode), ": "_s);
    switch (access.kind) {
    case MemoryAccess::Kind::NotExact:
        out.println("not exact"_s);
        return;
    case MemoryAccess::Kind::Read:
        out.print("read "_s);
        break;
    case MemoryAccess::Kind::Write:
        out.print("write "_s);
        break;
    }
    out.print(MemoryLocationDump { program, access.location });
    if (access.base)
        out.print(" base=v"_s, access.base.index());
    if (access.stored)
        out.print(" stored=v"_s, access.stored.index());
    out.println(" stability="_s, enumName(stabilityOf(program, value, access)));
}

static void printLocationOverlaps(PrintStream& out, const Program& program, std::span<const MemoryLocation> locations)
{
    for (size_t first = 0; first < locations.size(); ++first) {
        for (size_t second = first + 1; second < locations.size(); ++second) {
            LocationOverlap overlap = overlapOf(program, locations[first], locations[second]);
            out.println("overlap "_s, MemoryLocationDump { program, locations[first] }, " "_s, MemoryLocationDump { program, locations[second] }, ": "_s, enumName(overlap));
        }
    }
}

static void runLocations(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    const Program& program = *context.program;
    PrintStream& out = context.out;
    forEachFunctionWithBody(context, functions->span(), [&](const Function& function) {
        LocatedAccessCollector collector;
        forEachListedValue(function, [&](ValueID id) {
            printMemoryAccess(out, program, function, id);
            forEachDeclaredEffect(function.at(id), collector);
        });
        printLocationOverlaps(out, program, collector.locations());
    });
}

static Vector<Effects> effectsOfValues(const EffectContext& context, const Function& function)
{
    Vector<Effects> effects;
    effects.reserveInitialCapacity(function.values.size());
    for (const Value& value : function.values)
        effects.append(effectsOf(context, function, value).onAnyEdge());
    return effects;
}

static void printWriteOverlapsOnTest(PrintStream& out, const EffectContext& context, const Function& function, std::span<const Effects> effects, size_t testIndex, const MemoryLocation& location)
{
    for (size_t writerIndex = 0; writerIndex < function.values.size(); ++writerIndex) {
        if (writerIndex == testIndex)
            continue;
        LocationOverlap overlap = overlapOfWrites(context.program, function, function.values[writerIndex], effects[writerIndex], location);
        if (overlap == LocationOverlap::None && effects[writerIndex].writes.isEmpty())
            continue;
        out.println("v"_s, writerIndex, " on v"_s, testIndex, " "_s, MemoryLocationDump { context.program, location }, ": "_s, enumName(overlap));
    }
}

static void printWriteOverlapsOnTests(PrintStream& out, const EffectContext& context, const Function& function)
{
    Vector<Effects> effects = effectsOfValues(context, function);
    for (size_t testIndex = 0; testIndex < function.values.size(); ++testIndex) {
        const Value& test = function.values[testIndex];
        if (lastsOf(provesOf(test.opcode)) != ProofLifetime::Stretch)
            continue;
        ReadLocationCollector collector;
        forEachDeclaredEffect(test, collector);
        for (const MemoryLocation& location : collector.locations())
            printWriteOverlapsOnTest(out, context, function, effects.span(), testIndex, location);
    }
}

static void printWriteOverlaps(PrintStream& out, const EffectContext& context, const Function& function)
{
    Vector<Effects> effects = effectsOfValues(context, function);
    for (size_t accessIndex = 0; accessIndex < function.values.size(); ++accessIndex) {
        MemoryAccess access = memoryAccessOf(function, function.values[accessIndex]);
        if (!access)
            continue;
        for (size_t writerIndex = 0; writerIndex < function.values.size(); ++writerIndex) {
            if (writerIndex == accessIndex)
                continue;
            LocationOverlap overlap = overlapOfWrites(context.program, function, function.values[writerIndex], effects[writerIndex], access.location);
            if (overlap == LocationOverlap::None && effects[writerIndex].writes.isEmpty())
                continue;
            out.println("v"_s, writerIndex, " on v"_s, accessIndex, ": "_s, enumName(overlap));
        }
    }
}

static void runWriteOverlap(TestCommandContext& context)
{
    EffectsTestInputs inputs;
    std::span<const String> words = takeInputWords(context, context.arguments, inputs);
    OverlapSubjects subjects = OverlapSubjects::ExactAccesses;
    if (!words.empty() && words.front() == "tests"_s) {
        subjects = OverlapSubjects::StretchTests;
        words = words.subspan(1);
    }
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;

    EffectContext effectContext { *context.program, inputs.summaries };
    forEachFunctionWithBody(context, functions->span(), [&](const Function& function) {
        switch (subjects) {
        case OverlapSubjects::ExactAccesses:
            printWriteOverlaps(context.out, effectContext, function);
            return;
        case OverlapSubjects::StretchTests:
            printWriteOverlapsOnTests(context.out, effectContext, function);
            return;
        }
    });
}

static void printOperandSummary(PrintStream& out, const OperandSummary& summary)
{
    out.print("["_s);
    CommaPrinter comma;
    // "Every use" is written with every bit of the word set.
    for (OperandUse use : allMembersOf<OperandUse>) {
        if (summary.uses.contains(use))
            out.print(comma, nameOf(use));
    }
    out.print("]"_s);
    if (!summary.uses.contains(OperandUse::Retains))
        return;
    switch (summary.retainedIn) {
    case RetainedIn::UnknownMemory:
        return;
    case RetainedIn::Result:
        out.print(" in result"_s);
        return;
    case RetainedIn::Operand:
        out.print(" in operand "_s, summary.retainingOperand);
        return;
    }
}

static void printValueOperands(PrintStream& out, const EffectContext& context, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    EffectsOnEdges effects = effectsOf(context, function, value);
    value.forEachOperand([&](ValueID, uint32_t index) {
        out.print("v"_s, id.index(), " "_s, nameOf(value.opcode), " "_s, index, ": "_s);
        printOperandSummary(out, summarizeOperand(context, function, value, index));
        OperandMemory memory = memoryAccessedThrough(context, function, value, index, effects);
        if (!memory.reads.isEmpty())
            out.print(" reads("_s, memory.reads, ")"_s);
        if (!memory.writes.isEmpty())
            out.print(" writes("_s, memory.writes, ")"_s);
        out.print("\n"_s);
    });
}

static void printTerminatorOperands(PrintStream& out, const EffectContext& context, const Function& function, size_t blockIndex)
{
    const Terminator& terminator = function.blocks[blockIndex].terminator;
    terminator.forEachOperand([&](ValueID, uint32_t index) {
        out.print("b"_s, blockIndex, " "_s, nameOf(terminator.kind), " "_s, index, ": "_s);
        printOperandSummary(out, summarizeOperand(context, function, terminator, index));
        out.print("\n"_s);
    });
}

static void runOperands(TestCommandContext& context)
{
    runOnFunctions(context, [&](const EffectContext& effectContext, const Function& function) {
        for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (ValueID id : function.blocks[blockIndex].values)
                printValueOperands(context.out, effectContext, function, id);
            printTerminatorOperands(context.out, effectContext, function, blockIndex);
        }
    });
}

static ASCIILiteral textOf(ResultFreshness freshness)
{
    switch (freshness) {
    case ResultFreshness::Unknown:
        return "unknown"_s;
    case ResultFreshness::Unaliased:
        return "unaliased"_s;
    case ResultFreshness::Fresh:
        return "fresh"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void runResultFreshness(TestCommandContext& context)
{
    runOnFunctions(context, [&](const EffectContext& effectContext, const Function& function) {
        forEachListedValue(function, [&](ValueID id) {
            const Value& value = function.at(id);
            if (resultRuleOf(value.opcode) == ResultRule::None)
                return;
            context.out.println("v"_s, id.index(), " "_s, nameOf(value.opcode), ": "_s, textOf(freshnessOf(effectContext, function, value)));
        });
    });
}

static void runMembersRunBy(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    if (words.empty()) {
        printMissingArgument(context.out);
        return;
    }
    const Program& program = *context.program;
    std::optional<ProtocolsArgument> argument = parseProtocolsWord(program.builtins(), words.front());
    if (!argument) {
        printBadArgument(context.out, words.front());
        return;
    }
    words = words.subspan(1);
    NamedMembers namedMemberUse = NamedMembers::AreOnlyRead;
    if (!words.empty() && words.front() == "only-inspected"_s) {
        namedMemberUse = NamedMembers::AreOnlyInspected;
        words = words.subspan(1);
    }
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;

    forEachFunctionWithBody(context, functions->span(), [&](const Function& function) {
        for (ValueID parameter : function.blocks[0].parameters) {
            CalleeList members = membersRunBy(program, program.typeOf(function, parameter), argument->protocols, argument->namedMembers.span(), argument->calledMembers.span(), namedMemberUse);
            context.out.println("v"_s, parameter.index(), ": "_s, members);
        }
    });
}

static void runCheckEffects(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments.front());
        return;
    }
    checkDeclaredEffects(context.build.diagnostics());
    printValidIfNoErrors(context);
}

static void runCheckReplacement(TestCommandContext& context)
{
    EffectsTestInputs inputs;
    std::span<const String> words = takeInputWords(context, context.arguments, inputs);
    if (words.size() < 3) {
        printMissingArgument(context.out);
        return;
    }
    const Program& program = *context.program;
    std::optional<FunctionID> id = idParser<FunctionID>('f', program.functions.size())(words.front());
    if (!id) {
        printBadArgument(context.out, words.front());
        return;
    }
    const Function& function = program.at(*id);
    auto parseValue = idParser<ValueID>('v', function.values.size());
    Vector<ValueID> values;
    for (const String& word : words.subspan(1)) {
        std::optional<ValueID> value = parseValue(word);
        if (!value) {
            printBadArgument(context.out, word);
            return;
        }
        values.append(*value);
    }

    EffectContext effectContext { program, inputs.summaries };
    ValueID replacement = values.first();
    ReplacedEffects replaced;
    for (ValueID value : values.span().subspan(1))
        replaced.add(effectContext, function, function.at(value));
    checkReplacement(effectContext, function, { *id, BlockID(), replacement }, function.at(replacement), replaced, context.build.diagnostics());
    printValidIfNoErrors(context);
}

static void runCheckSummaries(TestCommandContext& context)
{
    EffectsTestInputs inputs;
    std::span<const String> words = takeInputWords(context, context.arguments, inputs);
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;

    const Program& program = *context.program;
    if (!inputs.hasSummaries)
        inputs.summaries = computeEffectSummaries(program, inputs.flow, context.build);
    EffectContext effectContext { program, inputs.summaries };
    EffectSummaries seed = createSummariesFromFlow(program, inputs.flow, context.build);
    EffectContext contextOfSeed { program, seed };
    for (FunctionID id : *functions) {
        const Function& function = program.at(id);
        if (!function.hasBody())
            continue;
        std::unique_ptr<FunctionAnalyses> analyses = createAnalysesForSummary(context.build, contextOfSeed, inputs.flow, id, function);
        checkSummaryCoversBody(effectContext, id, *analyses, context.build.diagnostics());
    }
    printValidIfNoErrors(context);
}

static void runCheckEntrypoints(TestCommandContext& context)
{
    const Program& program = *context.program;
    for (const Function& function : program.functions) {
        if (function.role == FunctionRole::GenericEntrypoint && function.hasBody())
            checkCallByValueCoversEntrypoint(program, function.creator.as<CreatorRecord::Entrypoint>().target, function, context.build.diagnostics());
    }
    printValidIfNoErrors(context);
}

std::span<const TestCommand> effectsTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "effects"_s, IRForm::Prepared, runEffects, ProgramAsRead::TypedAndVerified },
        { "locations"_s, IRForm::Prepared, runLocations, ProgramAsRead::TypedAndVerified },
        { "write-overlap"_s, IRForm::Prepared, runWriteOverlap, ProgramAsRead::TypedAndVerified },
        { "operands"_s, IRForm::Prepared, runOperands, ProgramAsRead::TypedAndVerified },
        { "result-freshness"_s, IRForm::Prepared, runResultFreshness, ProgramAsRead::TypedAndVerified },
        { "members-run-by"_s, IRForm::Prepared, runMembersRunBy, ProgramAsRead::TypedAndVerified },
        { "check-effects"_s, std::nullopt, runCheckEffects },
        { "check-replacement"_s, IRForm::Prepared, runCheckReplacement, ProgramAsRead::TypedAndVerified },
        { "check-summaries"_s, IRForm::Prepared, runCheckSummaries, ProgramAsRead::TypedAndVerified },
        { "check-entrypoints"_s, IRForm::Prepared, runCheckEntrypoints, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
