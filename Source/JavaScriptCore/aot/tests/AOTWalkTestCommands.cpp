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
#include "AOTSumOverRecords.h"
#include "AOTSweep.h"
#include "AOTTypeText.h"
#include "AOTWalk.h"
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class VerifiedWalks {
    WTF_MAKE_NONCOPYABLE(VerifiedWalks);
public:
    explicit VerifiedWalks(TestCommandContext& context)
        : m_stage(context.stage())
    {
        m_inputs = m_stage.prepareInputsForVerification();
        if (!m_inputs)
            return;
        m_worker.emplace(*m_inputs, context.build, m_stage.program().types);
    }

    bool isReady() const { return m_worker.has_value(); }

    const WalkInputs& inputs() const { return m_inputs->walk; }
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
    std::unique_ptr<CompileInputs> m_inputs;
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
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Vector<Walker::ArrivalHeld> held;
    for (const String& word : context.arguments) {
        Vector<String> parts = word.split(':');
        std::optional<uint32_t> callee = parts.size() == 3 && parts[0].startsWith('f') ? parseInteger<uint32_t>(StringView(parts[0]).substring(1)) : std::nullopt;
        std::optional<uint32_t> entryParameter = callee ? parseInteger<uint32_t>(parts[1]) : std::nullopt;
        unsigned offset = 0;
        String error;
        std::optional<TypeID> left = entryParameter ? parseType(walks.types(), parts[2], offset, error) : std::nullopt;
        if (!left || *callee >= walks.program().functions.size()) {
            context.out.println("bad argument "_s, word);
            return;
        }
        held.append({ FunctionID(*callee), *entryParameter, *left, Walker::ArrivalHeld::Kind::Proven });
    }
    Code code;
    if (!readCodeOfSection(context, walks, code))
        return;
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::Unknown };
    String brokenRule = verifyCode(walks.contextOfVerifier(), code, &kindOfBrokenRule);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::TypeOutOfDate;
        brokenRule = verifyTypesOfCode(TypingContext { walks.program(), walks.types(), walks.walker().positionsOf(code.role()) }, walks.plan().conventions(), walks.arrivals(), code);
    }
    if (brokenRule.isNull())
        brokenRule = findCallThatHandsOverWhatItsUnitHasNotLeft(walks.inputs(), walks.types(), code, held.span(), &walks.walker().positionsKnowingWhatFunctionsHandBack());
    if (brokenRule.isNull())
        context.out.println("verified"_s);
    else
        context.out.println(nameOf(kindOfBrokenRule), ": "_s, brokenRule);
}

static std::optional<std::pair<StringView, ValueID>> valueNamedByWordOfWalkCommand(const Code& code, const String& word)
{
    size_t equals = word.find('=');
    if (equals == notFound || word.length() < equals + 3 || word[equals + 1] != 'v')
        return std::nullopt;
    std::optional<uint32_t> number = parseInteger<uint32_t>(StringView(word).substring(equals + 2));
    if (!number || *number >= code.numberOfValues())
        return std::nullopt;
    return std::pair { StringView(word).left(equals), ValueID(*number) };
}

static void runPositionInsideCommand(TestCommandContext& context)
{
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Code code;
    if (!readCodeOfSection(context, walks, code))
        return;
    GroundsOfPosition grounds;
    for (const String& word : context.arguments) {
        auto named = valueNamedByWordOfWalkCommand(code, word);
        ValueID GroundsOfPosition::* place = !named ? nullptr
            : named->first == "position"_s ? &GroundsOfPosition::position
            : named->first == "length"_s ? &GroundsOfPosition::length
            : named->first == "below"_s || named->first == "not-below"_s ? &GroundsOfPosition::isBelow
            : named->first == "from-zero-up"_s ? &GroundsOfPosition::isFromZeroUp
            : named->first == "counter"_s ? &GroundsOfPosition::counter
            : named->first == "start"_s ? &GroundsOfPosition::start
            : named->first == "length-holds-bound"_s ? &GroundsOfPosition::limitWithinLengthCheck
            : named->first == "lowered-counter"_s ? &GroundsOfPosition::loweredCounter
            : named->first == "lowered-from"_s ? &GroundsOfPosition::loweredFrom
            : nullptr;
        if (!place) {
            context.out.println("bad argument "_s, word);
            return;
        }
        grounds.*place = named->second;
        if (named->first == "not-below"_s)
            grounds.isBelowResult = false;
    }
    bool isShown = grounds.position && groundsShowPositionInside(code, walks.types(), grounds.position, grounds, scopedLambda<bool(ValueID, bool)>([](ValueID, bool) {
        return true;
    }));
    context.out.println(isShown ? "shown"_s : "not shown"_s);
}

struct GroundsWrittenInTests {
    static PassedTest make(std::span<const PassedTest::Side> sides, const std::optional<GroundsOfPosition>& grounds)
    {
        PassedTest passed;
        for (const PassedTest::Side& side : sides)
            passed.add(side);
        passed.m_groundsOfPosition = grounds;
        return passed;
    }
};

static void runVerifyGroundsCommand(TestCommandContext& context)
{
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Code code;
    if (!readCodeOfSection(context, walks, code))
        return;
    code.setKeepsTestsPassed(true);
    ValueID access;
    Vector<PassedTest::Side, 4> sides;
    std::optional<GroundsOfPosition> grounds;
    auto noteGroundsOfAccess = [&] {
        if (access)
            code.noteTestsPassedFor(access, GroundsWrittenInTests::make(sides.span(), grounds));
        sides.shrink(0);
        grounds = std::nullopt;
    };
    for (const String& word : context.arguments) {
        auto named = valueNamedByWordOfWalkCommand(code, word);
        if (named && named->first == "access"_s) {
            noteGroundsOfAccess();
            access = named->second;
            continue;
        }
        if (named && access && (named->first == "passed"_s || named->first == "failed"_s)) {
            sides.append({ named->second, named->first == "passed"_s });
            continue;
        }
        ValueID GroundsOfPosition::* place = !named || !access ? nullptr
            : named->first == "view"_s ? &GroundsOfPosition::view
            : named->first == "position"_s ? &GroundsOfPosition::position
            : named->first == "length"_s ? &GroundsOfPosition::length
            : named->first == "below"_s ? &GroundsOfPosition::isBelow
            : named->first == "from-zero-up"_s ? &GroundsOfPosition::isFromZeroUp
            : named->first == "counter"_s ? &GroundsOfPosition::counter
            : named->first == "start"_s ? &GroundsOfPosition::start
            : named->first == "length-holds-bound"_s ? &GroundsOfPosition::limitWithinLengthCheck
            : named->first == "lowered-counter"_s ? &GroundsOfPosition::loweredCounter
            : named->first == "lowered-from"_s ? &GroundsOfPosition::loweredFrom
            : nullptr;
        if (!place) {
            context.out.println("bad argument "_s, word);
            return;
        }
        if (!grounds)
            grounds = GroundsOfPosition { };
        (*grounds).*place = named->second;
    }
    noteGroundsOfAccess();
    String brokenRule = verifyCode(walks.contextOfVerifier(), code);
    context.out.println(brokenRule.isNull() ? "verified"_s : brokenRule);
}

static void runElementPresentCommand(TestCommandContext& context)
{
    VerifiedWalks walks(context);
    if (!walks.isReady())
        return;
    Code code;
    if (!readCodeOfSection(context, walks, code))
        return;
    ValueID list;
    ValueID position;
    Vector<PassedTest::Side, 4> sides;
    for (const String& word : context.arguments) {
        auto named = valueNamedByWordOfWalkCommand(code, word);
        if (named && named->first == "list"_s)
            list = named->second;
        else if (named && named->first == "position"_s)
            position = named->second;
        else if (named && (named->first == "passed"_s || named->first == "failed"_s))
            sides.append({ named->second, named->first == "passed"_s });
        else {
            context.out.println("bad argument "_s, word);
            return;
        }
    }
    context.out.println(list && position && !sides.isEmpty() && testsShowElementPresent(code, list, position, sides.span()) ? "shown"_s : "not shown"_s);
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
    RoomsOfSweep rooms;
    Sweep sweep(rooms);
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
        if (!inputs.walk.plan.conventions().exists(unit) || worker.walkAndSweep(unit))
            continue;
        context.out.print("the walk of "_s);
        printCodeUnit(context.out, unit);
        context.out.println(" failed"_s);
        return;
    }
}

static void runWalkWhereHandlerEdgesTakeStandInsCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> units = unitsInOrderOfWords(*context.program, context.arguments, context.out);
    StagedBuild stage = context.stage();
    std::unique_ptr<CompileInputs> prepared = units ? stage.prepareInputsForVerification() : nullptr;
    if (!prepared)
        return;
    CompileInputs& inputs = *prepared;
    inputs.walk.options.handlerEdgesTakeWhatStandsIn = true;
    CompileWorker worker(inputs, context.build, stage.program().types);
    for (CodeUnit unit : *units) {
        context.out.print("the walk of "_s);
        printCodeUnit(context.out, unit);
        context.out.println(worker.walkAndSweep(unit) ? " was built"_s : " failed"_s);
    }
}

static void runWalkWhereEveryClosureStandsBehindCopiesCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> units = unitsInOrderOfWords(*context.program, context.arguments, context.out);
    StagedBuild stage = context.stage();
    std::unique_ptr<CompileInputs> prepared = units ? stage.prepareInputsForVerification() : nullptr;
    if (!prepared)
        return;
    CompileInputs& inputs = *prepared;
    inputs.walk.options.everyClosureStandsBehindCopies = true;
    CompileWorker worker(inputs, context.build, stage.program().types);
    for (CodeUnit unit : *units) {
        context.out.print("the walk of "_s);
        printCodeUnit(context.out, unit);
        context.out.println(worker.walkAndSweep(unit) ? " was built"_s : " failed"_s);
    }
}

static void runWalkWhileKeptListsAreDroppedCommand(TestCommandContext& context)
{
    static constexpr unsigned enoughListsDropped = 2000;
    static constexpr unsigned mostWalks = 20000;
    std::optional<Vector<CodeUnit>> units = unitsInOrderOfWords(*context.program, context.arguments, context.out);
    if (!units)
        return;
    StagedBuild stage = context.stage();
    const Facts* facts = stage.prepared() ? stage.facts() : nullptr;
    if (!facts)
        return;
    auto textOf = [&](CodeUnit unit) {
        StringPrintStream text;
        if (const Code* code = stage.code(unit))
            printCode(text, CodeTextContext { stage.program(), stage.types() }, *code);
        return text.toString();
    };
    for (CodeUnit unit : *units) {
        String text = textOf(unit);
        Vector<std::unique_ptr<const KeptWithBodyIndex>> dropped;
        std::atomic<unsigned> numberOfListsDropped { 0 };
        std::atomic<bool> walksAreDone { false };
        Ref<Thread> dropsLists = Thread::create("AOT walk test"_s, [&] {
            while (!walksAreDone.load()) {
                for (uint32_t function = 0; function < stage.program().functions.size(); ++function) {
                    const BodyIndex* body = facts->bodies().find(FunctionID(function));
                    std::unique_ptr<const KeptWithBodyIndex> kept = body ? body->takeKeptByWalk() : nullptr;
                    if (!kept)
                        continue;
                    dropped.append(WTF::move(kept));
                    numberOfListsDropped.store(dropped.size());
                }
            }
        });
        bool someWalkMadeOtherCode = false;
        for (unsigned walk = 0; walk < mostWalks && numberOfListsDropped.load() < enoughListsDropped && !someWalkMadeOtherCode; ++walk)
            someWalkMadeOtherCode = textOf(unit) != text;
        walksAreDone.store(true);
        dropsLists->waitForCompletion();
        context.out.print("code "_s);
        printCodeUnit(context.out, unit);
        context.out.println(text.isEmpty() ? ": the walk failed"_s : someWalkMadeOtherCode ? ": a walk made other code"_s : ": every walk made the same code"_s);
    }
}

static ASCIILiteral nameOfWalkNoteKind(WalkNoteKind kind)
{
    switch (kind) {
    case WalkNoteKind::ResolvedOperation:
        return "ResolvedOperation"_s;
    case WalkNoteKind::GenericOperation:
        return "GenericOperation"_s;
    case WalkNoteKind::TestThatRejoins:
        return "TestThatRejoins"_s;
    case WalkNoteKind::TestThatLeaves:
        return "TestThatLeaves"_s;
    case WalkNoteKind::TestThatThrows:
        return "TestThatThrows"_s;
    case WalkNoteKind::CallNotInlined:
        return "CallNotInlined"_s;
    case WalkNoteKind::ObjectBuilt:
        return "ObjectBuilt"_s;
    case WalkNoteKind::ObjectNotInFrame:
        return "ObjectNotInFrame"_s;
    case WalkNoteKind::ObjectStayedVirtual:
        return "ObjectStayedVirtual"_s;
    case WalkNoteKind::LoopWalkedAgain:
        return "LoopWalkedAgain"_s;
    case WalkNoteKind::WayEnds:
        return "WayEnds"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void runNotesCommand(TestCommandContext& context)
{
    std::optional<Vector<CodeUnit>> units = unitsInOrderOfWords(*context.program, context.arguments, context.out);
    if (!units || units->isEmpty())
        return;

    StagedBuild stage = context.stage();
    if (!stage.code(units->first()))
        return;
    CompileInputs inputs(*stage.prepared()->compileInputs);
    inputs.walk.options.takesNotes = true;
    CompileWorker worker(inputs, context.build, stage.program().types);
    for (CodeUnit unit : *units) {
        size_t numberOfUnitsWithNotes = worker.notes().size();
        if (!worker.walkAndSweep(unit)) {
            context.out.print("the walk of "_s);
            printCodeUnit(context.out, unit);
            context.out.println(" failed"_s);
            return;
        }
        context.out.print("notes of "_s);
        printCodeUnit(context.out, unit);
        context.out.println();
        if (worker.notes().size() == numberOfUnitsWithNotes)
            continue;
        for (const WalkNote& note : worker.notes().last().notes) {
            context.out.print(nameOfWalkNoteKind(note.kind), " @"_s, note.origin.position, " in f"_s, note.writtenIn.index());
            if (note.layout)
                context.out.print(" layout=L"_s, note.layout.index());
            context.out.println();
        }
    }
}

std::span<const TestCommand> walkTestCommands()
{
    static constexpr TestCommand commands[] {
        { "walk"_s, runWalkCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "why-closed"_s, runWhyClosedCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "walk-where-handler-edges-take-stand-ins"_s, runWalkWhereHandlerEdgesTakeStandInsCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "walk-where-every-closure-stands-behind-copies"_s, runWalkWhereEveryClosureStandsBehindCopiesCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "walk-while-kept-lists-are-dropped"_s, runWalkWhileKeptListsAreDroppedCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "notes"_s, runNotesCommand, ProgramOfCommand::AsWritten, "unit.."_s },
        { "code-round-trip"_s, runCodeRoundTripCommand },
        { "verify-code"_s, runVerifyCodeCommand },
        { "sweep-code"_s, runSweepCodeCommand },
        { "position-inside"_s, runPositionInsideCommand, ProgramOfCommand::AsWritten, "position=v<n> length=v<n> { below=v<n> | not-below=v<n> | from-zero-up=v<n> | counter=v<n> | start=v<n> | length-holds-bound=v<n> | lowered-counter=v<n> | lowered-from=v<n> }"_s },
        { "element-present"_s, runElementPresentCommand, ProgramOfCommand::AsWritten, "list=v<n> position=v<n> { passed=v<n> | failed=v<n> }"_s },
        { "verify-grounds"_s, runVerifyGroundsCommand, ProgramOfCommand::AsWritten, "{ access=v<n> { view=v<n> | position=v<n> | length=v<n> | below=v<n> | from-zero-up=v<n> | length-holds-bound=v<n> | lowered-counter=v<n> | lowered-from=v<n> | passed=v<n> | failed=v<n> } }"_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
