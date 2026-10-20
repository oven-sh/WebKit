/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionTestCommands.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTRawWordCheck.h"
#include "AOTRawWordWebs.h"

namespace JSC { namespace AOT { namespace Typed {

static OptimizationInputs createTestInputs(Pipeline& pipeline)
{
    OptionSet<ProgramResult> needed = inputsOf(FunctionSchedule::Select) | inputsOf(FunctionSchedule::Represent);
    computeResultsForTesting(pipeline, needed);
    return pipeline.createInputs(needed);
}

SelectionTestSession::SelectionTestSession(TestCommandContext& context, FunctionID id)
    : m_runner(context.build)
    , m_pipeline(*context.program, m_runner)
    , m_inputs(createTestInputs(m_pipeline))
    , m_arenas(*context.program)
    , m_analyses(AnalysisContext { context.build, *context.program, &m_inputs.flow, &m_inputs.effects, id }, context.program->at(id))
    , m_pass { *context.program, m_inputs, id, context.program->at(id), m_analyses, m_arenas.types, m_arenas.strings, context.build }
{
}

std::optional<FunctionID> parseFunctionWithBody(const Program& program, StringView text)
{
    return functionParser(program)(text);
}

static bool isListed(const Function& function, ValueID id)
{
    return function.blocks.containsIf([&](const Block& block) {
        return block.parameters.contains(id) || block.values.contains(id);
    });
}

std::optional<ValueID> parseListedValue(const Function& function, StringView text)
{
    std::optional<ValueID> id = idParser<ValueID>('v', function.values.size())(text);
    if (!id || !isListed(function, *id))
        return std::nullopt;
    return id;
}

std::optional<BlockID> parseBlock(const Function& function, StringView text)
{
    return blockParser(function)(text);
}

std::optional<StringView> valueForKey(StringView word, ASCIILiteral key)
{
    if (word.find('=') != key.length() || !word.startsWith(key))
        return std::nullopt;
    return word.substring(key.length() + 1);
}

static void forEachFunctionWithBody(const Program& program, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            functor(id);
    }
}

static void printRawWord(PrintStream& out, const RawWordWebs& webs, ValueID word)
{
    Vector<RawWordProduction, 2> productions;
    ProductionReach reach = webs.collectProductions(word, productions);

    dumpID(out, word);
    out.print(" ", nameOf(*webs.rawKindOf(word)), ": productions");
    switch (reach) {
    case ProductionReach::Unknown:
        out.println(" unknown; bases");
        return;
    case ProductionReach::OnlyProductions:
        break;
    }

    Vector<ValueID, 2> bases;
    for (const RawWordProduction& production : productions) {
        out.print(" ");
        dumpID(out, production.producer);
        out.print(".", production.resultIndex);

        ValueID base = webs.baseOf(production);
        if (!bases.contains(base))
            bases.append(base);
    }

    out.print("; bases");
    for (ValueID base : bases) {
        out.print(" ");
        dumpID(out, base);
    }
    out.println();
}

static void runRawWordsTestCommand(TestCommandContext& context)
{
    CommandWords words(context);
    if (!words.finish())
        return;

    const Program& program = *context.program;
    forEachFunctionWithBody(program, [&](FunctionID id) {
        const Function& function = program.at(id);
        FunctionAnalyses analyses(AnalysisContext { context.build, program, nullptr, nullptr, id }, function);
        context.out.println("function f", id.index());

        RawWordWebs webs(program, function, analyses);
        for (size_t index : webs.rawWords())
            printRawWord(context.out, webs, ValueID(index));

        Vector<InternalError> errors;
        checkRawWords(program, id, function, analyses, errors);
        if (errors.isEmpty())
            context.out.println("valid");
        for (const InternalError& error : errors)
            dumpLine(context.out, "error "_s, nameOf(error.kind), " "_s, error.block, " "_s, error.value);
    });
}

std::span<const TestCommand> selectionTestCommands()
{
    static constexpr TestCommand commands[] {
        { "claim"_s, IRForm::Prepared, runClaimTestCommand },
        { "loop-entry"_s, IRForm::Prepared, runLoopEntryTestCommand },
        { "comparison-chains"_s, IRForm::Prepared, runComparisonChainsTestCommand },
        { "code-shapes"_s, IRForm::Prepared, runCodeShapesTestCommand },
        { "operator-forms"_s, IRForm::Prepared, runOperatorFormsTestCommand },
        { "inline-forms"_s, IRForm::Prepared, runInlineFormsTestCommand },
        { "raw-words"_s, IRForm::Selected, runRawWordsTestCommand, RawWordCheck::Skip },
        { "representation-rules"_s, IRForm::Placed, runRepresentationRulesTestCommand },
        { "conversions"_s, IRForm::Placed, runConversionsTestCommand },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
