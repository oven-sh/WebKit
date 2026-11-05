/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTCompileUnit.h"
#include "AOTStages.h"
#include "AOTSweep.h"
#include "AOTWalk.h"
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class VerifiedWalks {
    WTF_MAKE_NONCOPYABLE(VerifiedWalks);
public:
    explicit VerifiedWalks(TestCommandContext& context)
        : m_stage(context.stage())
    {
        const PreparedProgram* prepared = m_stage.prepared();
        if (!prepared)
            return;
        m_inputs.emplace(*prepared->compileInputs);
        m_inputs->walk.options.verifiesCode = true;
        m_worker.emplace(*m_inputs, context.build, m_stage.program().types);
    }

    bool isReady() const { return m_worker.has_value(); }

    const Program& program() const { return m_inputs->walk.program; }
    const Facts& facts() const { return m_inputs->walk.facts; }
    const Arrivals& arrivals() const { return m_inputs->walk.arrivals; }
    const Plan& plan() const { return m_inputs->walk.plan; }
    Walker& walker() { return m_worker->walker(); }
    TypeArena& types() { return walker().types(); }
    CodeTextContext textContext() { return { program(), types() }; }
    CodeTextContext contextOfVerifier() { return { program(), types(), &plan(), &facts() }; }

private:
    StagedBuild m_stage;
    std::optional<CompileInputs> m_inputs;
    std::optional<CompileWorker> m_worker;
};

} // anonymous namespace

static constexpr ASCIILiteral sectionReadByWalkCommands = "code"_s;

static bool refuseArgumentsOfWalkCommand(TestCommandContext& context)
{
    if (context.arguments.empty())
        return false;
    context.out.println("bad argument "_s, context.arguments.front());
    return true;
}

static std::optional<StringView> readCodeOfSection(TestCommandContext& context, VerifiedWalks& walks, Code& code)
{
    const TestSection* section = context.findSection(sectionReadByWalkCommands);
    if (!section) {
        context.out.println("error: the file has no section `"_s, sectionReadByWalkCommands, "`"_s);
        return std::nullopt;
    }

    StringView text = section->text;
    String error = parseCode(walks.textContext(), text, code);
    if (!error.isNull()) {
        context.out.println("error in the Code's text, which starts at line "_s, section->firstLine, " of the file: "_s, error);
        return std::nullopt;
    }
    return text;
}

static Vector<StringView> linesOfCodeText(StringView text)
{
    Vector<StringView> lines;
    for (StringView line : text.splitAllowingEmptyEntries('\n'))
        lines.append(line);
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

static void printCodeRoundTripComparison(PrintStream& out, StringView readText, StringView printedText)
{
    Vector<StringView> read = linesOfCodeText(readText);
    Vector<StringView> printed = linesOfCodeText(printedText);
    size_t index = 0;
    while (index < read.size() && index < printed.size() && read[index] == printed[index])
        ++index;
    if (index == read.size() && index == printed.size()) {
        out.println("round trip: same"_s);
        return;
    }
    out.println("round trip: differs at line "_s, index + 1);
    out.println("read: "_s, index < read.size() ? read[index] : StringView());
    out.println("printed: "_s, index < printed.size() ? printed[index] : StringView());
}

static void runCodeRoundTripCommand(TestCommandContext& context)
{
    if (refuseArgumentsOfWalkCommand(context))
        return;
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Code code;
    std::optional<StringView> text = readCodeOfSection(context, walks, code);
    if (!text)
        return;
    StringPrintStream printed;
    printCode(printed, walks.textContext(), code);
    String printedText = printed.toString();
    context.out.print(printedText);
    printCodeRoundTripComparison(context.out, *text, printedText);
}

static void runVerifyCodeCommand(TestCommandContext& context)
{
    if (refuseArgumentsOfWalkCommand(context))
        return;
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Code code;
    if (!readCodeOfSection(context, walks, code))
        return;
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::Unknown };
    String brokenRule = verifyCode(walks.contextOfVerifier(), code, &kindOfBrokenRule);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::TypeOutOfDate;
        brokenRule = verifyTypesOfCode(TypingContext { walks.program(), walks.types(), walks.walker().positions() }, walks.plan().conventions(), walks.arrivals(), code);
    }
    if (brokenRule.isNull())
        context.out.println("verified"_s);
    else
        context.out.println(nameOf(kindOfBrokenRule), ": "_s, brokenRule);
}

static void runSweepCodeCommand(TestCommandContext& context)
{
    if (refuseArgumentsOfWalkCommand(context))
        return;
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Code code;
    if (!readCodeOfSection(context, walks, code))
        return;
    Sweep sweep;
    SweepTotals totals = sweep.run(SweepInputs { walks.program(), walks.facts(), walks.plan(), walks.types() }, code);
    printCode(context.out, walks.textContext(), code);
    context.out.println("# visits of operations: "_s, totals.operationsSwept);
    if (!code.holdsFrameCells())
        return;
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::Unknown };
    String brokenRule = verifyFrameCellsOfSweptCode(walks.contextOfVerifier(), code, &kindOfBrokenRule);
    if (brokenRule.isNull())
        context.out.println("# cells of the frame: verified"_s);
    else
        context.out.println("# cells of the frame: "_s, nameOf(kindOfBrokenRule), ": "_s, brokenRule);
}

static void runWalkCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> units = unitsInOrderOfWords(*context.program, context.arguments, context.out);
    if (!units)
        return;

    StagedBuild stage = context.stage();
    context.out.print("mechanisms "_s);
    printMechanisms(context.out, stage.mechanisms());
    context.out.println();
    for (CodeUnit unit : *units) {
        if (const Plan* plan = stage.plan(); plan && !plan->conventions().exists(unit)) {
            context.out.print("code "_s);
            printCodeUnit(context.out, unit);
            context.out.println();
            context.out.println("the plan has no such unit"_s);
            continue;
        }
        const Code* code = stage.code(unit);
        if (!code) {
            context.out.print("the walk of "_s);
            printCodeUnit(context.out, unit);
            context.out.println(" failed"_s);
            return;
        }
        printCode(context.out, CodeTextContext { stage.program(), stage.types() }, *code);
    }
}

static void runWhyClosedCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> units = unitsInOrderOfWords(*context.program, context.arguments, context.out);
    if (!units || units->isEmpty())
        return;

    StagedBuild stage = context.stage();
    if (!stage.code(units->first()))
        return;
    CompileInputs inputs(*stage.prepared()->compileInputs);
    inputs.walk.options.explainsClosedAccessesTo = &context.out;
    CompileWorker worker(inputs, context.build, stage.program().types);
    for (CodeUnit unit : *units) {
        if (worker.walkAndSweep(unit))
            continue;
        context.out.print("the walk of "_s);
        printCodeUnit(context.out, unit);
        context.out.println(" failed"_s);
        return;
    }
}

std::span<const TestCommand> walkTestCommands()
{
    static constexpr TestCommand commands[] {
        { "walk"_s, runWalkCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "why-closed"_s, runWhyClosedCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "code-round-trip"_s, runCodeRoundTripCommand },
        { "verify-code"_s, runVerifyCodeCommand },
        { "sweep-code"_s, runSweepCodeCommand },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
