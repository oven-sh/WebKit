/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBodyVerifier.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTProgramFile.h"
#include "AOTProgramVerifier.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static Vector<StringView> splitIntoLines(StringView text)
{
    Vector<StringView> lines;
    for (StringView line : text.splitAllowingEmptyEntries('\n'))
        lines.append(line);
    return lines;
}

static StringView lineAtOrEmpty(const Vector<StringView>& lines, size_t index)
{
    return index < lines.size() ? lines[index] : StringView();
}

static void printRoundTripComparison(PrintStream& out, const String& firstText, const String& secondText)
{
    if (firstText == secondText) {
        out.println("round trip: same"_s);
        return;
    }
    Vector<StringView> first = splitIntoLines(firstText);
    Vector<StringView> second = splitIntoLines(secondText);
    size_t numberOfSharedLines = std::min(first.size(), second.size());
    size_t index = 0;
    while (index < numberOfSharedLines && first[index] == second[index])
        ++index;
    out.println("round trip: differs at line "_s, index + 1);
    out.println("first: "_s, lineAtOrEmpty(first, index));
    out.println("second: "_s, lineAtOrEmpty(second, index));
}

static void printRoundTripComparisonOfBytes(PrintStream& out, std::span<const uint8_t> first, std::span<const uint8_t> second)
{
    size_t numberOfSharedBytes = std::min(first.size(), second.size());
    size_t offset = 0;
    while (offset < numberOfSharedBytes && first[offset] == second[offset])
        ++offset;
    if (offset == first.size() && offset == second.size()) {
        out.println("round trip of bytes: same"_s);
        return;
    }
    out.println("round trip of bytes: differs at byte "_s, offset, " of "_s, first.size(), " and "_s, second.size());
}

static void runRoundTripIRCommand(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    PrintedFields fields = PrintedFields::All;
    if (!words.empty() && words.front() == "wire"_s) {
        fields = PrintedFields::WireOnly;
        words = words.subspan(1);
    }
    if (!words.empty()) {
        printBadArgument(context.out, words.front());
        return;
    }

    const Program& program = *context.program;
    StringPrintStream first;
    printProgram(first, program, fields);
    String firstText = first.toString();
    printProgram(context.out, program, fields);

    std::unique_ptr<Program> again = parseProgram(firstText, context.build);
    if (!again) {
        context.out.println("round trip: does not parse"_s);
        return;
    }
    StringPrintStream second;
    printProgram(second, *again, fields);
    printRoundTripComparison(context.out, firstText, second.toString());
    if (fields == PrintedFields::All)
        printRoundTripComparisonOfBytes(context.out, program.buffer(), again->buffer());
}

static void runPrintIRCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    if (context.arguments.empty()) {
        printProgram(context.out, program);
        return;
    }
    std::optional<Vector<FunctionID>> functions = functionsNamedBy(program, context.arguments, context.out);
    if (!functions)
        return;
    for (FunctionID id : *functions)
        printFunction(context.out, program, id);
}

static void runVerifyIRCommand(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments.front());
        return;
    }

    const Program& program = *context.program;
    if (verifyRecords(program, context.build) && verifyBodies(program, context.build)) {
        context.out.println("verified"_s);
        return;
    }

    context.build.diagnostics().finish(&program);
    printDiagnostics(context.build.diagnostics(), context.out);
    reopenDiagnostics(context.build);
}

namespace {

template<typename IDType>
struct IDOrNone {
    ASCIILiteral prefix;
    IDType id;

    void dump(PrintStream& out) const
    {
        if (id)
            out.print(prefix, id.index());
        else
            out.print("none"_s);
    }
};

} // anonymous namespace

static IDOrNone<ValueID> idOrNone(ValueID id) { return { "v"_s, id }; }
static IDOrNone<BlockID> idOrNone(BlockID id) { return { "b"_s, id }; }
static IDOrNone<LoopID> idOrNone(LoopID id) { return { "loop"_s, id }; }

template<typename Range>
static void printIDList(PrintStream& out, const Range& ids)
{
    out.print("["_s);
    CommaPrinter blank(" "_s);
    for (auto id : ids)
        out.print(blank, idOrNone(id));
    out.print("]"_s);
}

static void printSaturatingCount(PrintStream& out, uint32_t count, uint32_t saturatedCount)
{
    if (count == saturatedCount)
        out.print("many"_s);
    else
        out.print(count);
}

static void printOnlyUser(PrintStream& out, const std::optional<BodyUser>& user)
{
    if (!user) {
        out.print("none"_s);
        return;
    }
    switch (user->kind) {
    case BodyUseKind::Operand:
        out.print(idOrNone(user->value));
        return;
    case BodyUseKind::TerminatorOperand:
        out.print("terminator of "_s, idOrNone(user->block));
        return;
    case BodyUseKind::EdgeArgument:
        out.print("edge of "_s, idOrNone(user->block));
        return;
    case BodyUseKind::EdgeProof:
        out.print("proof of "_s, idOrNone(user->block));
        return;
    case BodyUseKind::HandlerArgument:
        out.print("handler edge of "_s, idOrNone(user->block));
        return;
    case BodyUseKind::BlockSubject:
        out.print("subject of "_s, idOrNone(user->block));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printBlockOfBodyIndex(PrintStream& out, const BodyIndex& index, BlockID id)
{
    out.print(idOrNone(id), " dominator: "_s, idOrNone(index.immediateDominator(id)));
    if (index.isReachable(id))
        out.print(" dominator number: "_s, index.dominatorNumber(id), " last below: "_s, index.lastDominatorNumberBelow(id));
    else
        out.print(" dominator number: none last below: none"_s);
    out.print(" loop: "_s, idOrNone(index.loopOf(id)), " depth: "_s, index.loopDepthOf(id), " incoming edges: "_s);
    printSaturatingCount(out, index.numberOfIncomingEdges(id), BodyIndex::maxCountedIncomingEdges);

    out.print(" flags: ["_s);
    CommaPrinter blank(" "_s);
    if (index.isReachable(id))
        out.print(blank, "Reachable"_s);
    if (index.isLoopHead(id))
        out.print(blank, "LoopHead"_s);
    if (index.isEnteredByHandlerEdge(id))
        out.print(blank, "EnteredByHandlerEdge"_s);
    out.println("]"_s);
}

static void printValueOfBodyIndex(PrintStream& out, const BodyIndex& index, ValueID id)
{
    out.print(idOrNone(id), " "_s, nameOf(index.opcodeOf(id)), " block: "_s, idOrNone(index.blockOf(id)), " uses: "_s);
    printSaturatingCount(out, index.useCount(id), BodyIndex::maxCountedUses);
    out.print(" only user: "_s);
    printOnlyUser(out, index.onlyUser(id));
    out.println();
}

static void printLoopsNestedIn(PrintStream& out, const BodyIndex& index, LoopID parent)
{
    for (uint32_t number = parent ? parent.index() + 1 : 0; number < index.numberOfLoops(); ++number) {
        LoopID id(number);
        const BodyLoop& loop = index.loop(id);
        if (loop.parent != parent)
            continue;

        Vector<BlockID> members;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            if (index.isInLoop(BlockID(block), id))
                members.append(BlockID(block));
        }
        for (uint32_t depth = 1; depth < loop.depth; ++depth)
            out.print("  "_s);
        out.print(idOrNone(id), " head: "_s, idOrNone(loop.head), " last: "_s, idOrNone(loop.last), " parent: "_s, idOrNone(loop.parent), " depth: "_s, loop.depth, " blocks: "_s);
        printIDList(out, members);
        out.println();
        printLoopsNestedIn(out, index, id);
    }
}

static void printBodyIndex(PrintStream& out, const BodyIndex& index)
{
    out.println("irreducible flow: "_s, index.hasIrreducibleFlow() ? "yes"_s : "no"_s);
    out.print("dominator order: "_s);
    printIDList(out, index.blocksInDominatorOrder());
    out.println();

    out.print("opcodes: ["_s);
    CommaPrinter blank(" "_s);
    index.opcodes().forEach([&](Opcode opcode) {
        out.print(blank, nameOf(opcode));
    });
    out.println("]"_s);

    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block)
        printBlockOfBodyIndex(out, index, BlockID(block));
    for (uint32_t value = 0; value < index.numberOfValues(); ++value)
        printValueOfBodyIndex(out, index, ValueID(value));

    if (index.hasLoops())
        printLoopsNestedIn(out, index, LoopID());
    else
        out.println("no loops"_s);
}

static void printEdgeResult(PrintStream& out, const BodyIndex& index, const BodyTerminator& terminator, const BodyEdgeResult& result)
{
    out.print(idOrNone(terminator.block()), " -> "_s, idOrNone(index.blockOf(result.parameter)), " proof: "_s, nameOf(result.proof), " test: "_s);
    if (result.test)
        out.print(idOrNone(result.test));
    else
        out.print("terminator"_s);
    out.print(" subject: "_s, idOrNone(result.subject), " operand index: "_s, result.operandIndex);

    if (!result.caseIndex)
        out.print(" side: "_s, result.isOnTakenEdge ? "taken"_s : "notTaken"_s);
    else if (terminator.hasCases() && *result.caseIndex == terminator.numberOfCases())
        out.print(" case: otherwise"_s);
    else
        out.print(" case: "_s, *result.caseIndex);
    out.println(" parameter: "_s, idOrNone(result.parameter));
}

static void printEdgeResults(PrintStream& out, const BodyIndex& index)
{
    bool hasEdgeResults = false;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        BodyTerminator terminator = index.terminator(BlockID(block));
        index.forEachEdgeResult(BlockID(block), [&](const BodyEdgeResult& result) {
            hasEdgeResults = true;
            printEdgeResult(out, index, terminator, result);
        });
    }
    if (!hasEdgeResults)
        out.println("no edge results"_s);
}

static void printBodyLiveness(PrintStream& out, const BodyIndex& index, BodyLiveness& liveness)
{
    if (!liveness.isComputedFor(index))
        liveness.compute(index);
    BodyLiveness secondAsker;
    secondAsker.compute(index);

    RELEASE_ASSERT(secondAsker.answersFromSameDataAs(liveness));

    Vector<ValueID> live;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        BlockID id(block);
        live.shrink(0);
        liveness.forEachLiveAtHead(id, [&](ValueID value) {
            live.append(value);
        });
        out.print(idOrNone(id), " live at head: "_s);
        printIDList(out, live);
        out.println(" terminator at: "_s, idOrNone(index.block(id).endOfValues()));
    }

    for (uint32_t value = 0; value < index.numberOfValues(); ++value) {
        ValueID id(value);
        Opcode opcode = index.opcodeOf(id);
        ValueID lastUse = liveness.lastUse(id);
        out.print(idOrNone(id), " "_s, nameOf(opcode), " last use: "_s, idOrNone(lastUse == id ? ValueID() : lastUse));
        if (maySuspend(opcode)) {
            Vector<ValueID> liveAcross;
            liveness.forEachLiveAcross(id, index.blockOf(id), [&](ValueID live) {
                liveAcross.append(live);
            });
            out.print(" live across: "_s);
            printIDList(out, liveAcross);
        }
        out.println();
    }
}

static bool livenessIsWhatPlainComputationFinds(PrintStream& out, const BodyIndex& index, const BodyLiveness& liveness)
{
    uint32_t numberOfBlocks = index.numberOfBlocks();
    uint32_t numberOfValues = index.numberOfValues();
    auto forEachRead = [&](const BodyBlock& block, const auto& functor) {
        uint32_t end = block.endOfValues().index();
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            for (ValueID argument : target.arguments()) {
                if (argument)
                    functor(argument, end);
            }
        });
        if (block.terminator().isConditionalBranch()) {
            index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
                functor(result.subject, end);
            });
        }
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            functor(operand, end);
        });
        for (BodyValue value : block.values()) {
            value.forEachOperand([&](ValueID operand, uint32_t) {
                functor(operand, value.id().index());
            });
        }
        if (ValueID subject = block.subject())
            functor(subject, block.firstValue().index());
    };

    Vector<BitVector> live(numberOfBlocks, [&](size_t) { return BitVector(numberOfValues); });
    Vector<uint32_t> lastUses(numberOfValues, [](size_t value) { return static_cast<uint32_t>(value); });
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (uint32_t blockIndex = numberOfBlocks; blockIndex--;) {
            if (!index.isReachable(BlockID(blockIndex)))
                continue;
            BodyBlock block = index.block(BlockID(blockIndex));
            BitVector reached(numberOfValues);
            block.forEachOutgoingEdge([&](BodyTarget target, bool) {
                reached.merge(live[target.block().index()]);
            });
            forEachRead(block, [&](ValueID value, uint32_t position) {
                reached.quickSet(value.index());
                lastUses[value.index()] = std::max(lastUses[value.index()], position);
            });
            BitVector found(numberOfValues);
            for (size_t value : reached) {
                if (value < block.firstParameter().index())
                    found.quickSet(value);
            }
            if (found != live[blockIndex]) {
                live[blockIndex] = WTF::move(found);
                hasChanged = true;
            }
        }
    }
    auto raise = [&](BlockID head, BlockID last) {
        for (size_t value : live[head.index()])
            lastUses[value] = std::max(lastUses[value], index.block(last).endOfValues().index());
    };
    if (!index.hasIrreducibleFlow()) {
        for (uint32_t loop = 0; loop < index.numberOfLoops(); ++loop)
            raise(index.loop(LoopID(loop)).head, index.loop(LoopID(loop)).last);
    } else {
        for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            if (!index.isReachable(BlockID(blockIndex)))
                continue;
            index.block(BlockID(blockIndex)).forEachOutgoingEdge([&](BodyTarget target, bool) {
                if (target.block().index() <= blockIndex)
                    raise(target.block(), BlockID(blockIndex));
            });
        }
    }

    bool agrees = true;
    auto differ = [&](ASCIILiteral what, uint32_t block, uint32_t value) {
        if (agrees)
            out.println("DIFFERS: "_s, what, " b"_s, block, " v"_s, value);
        agrees = false;
    };
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID id(blockIndex);
        size_t numberFound = 0;
        uint32_t former = 0;
        liveness.forEachLiveAtHead(id, [&](ValueID value) {
            if (!live[blockIndex].quickGet(value.index()))
                differ("the plain way does not find live at the head of"_s, blockIndex, value.index());
            if (numberFound++ && value.index() <= former)
                differ("not ascending at the head of"_s, blockIndex, value.index());
            former = value.index();
        });
        if (numberFound != live[blockIndex].bitCount())
            differ("fewer are live than the plain way finds at the head of"_s, blockIndex, 0);
        uint32_t first = numberOfValues / 3;
        uint32_t end = numberOfValues - numberOfValues / 4;
        size_t numberInStretch = 0;
        liveness.forEachLiveAtHead(id, ValueID(first), ValueID(end), [&](ValueID value) {
            ++numberInStretch;
            if (value.index() < first || value.index() >= end)
                differ("outside the stretch asked for at the head of"_s, blockIndex, value.index());
        });
        for (size_t value : live[blockIndex]) {
            numberInStretch -= value >= first && value < end;
            if (!liveness.isLiveAtHead(ValueID(static_cast<uint32_t>(value)), id))
                differ("asked alone, not live at the head of"_s, blockIndex, static_cast<uint32_t>(value));
        }
        if (numberInStretch)
            differ("the stretch holds another number of values at the head of"_s, blockIndex, first);
    }
    for (uint32_t value = 0; value < numberOfValues; ++value) {
        if (liveness.lastUse(ValueID(value)).index() != lastUses[value])
            differ("the last use, by the plain way in the place of the block,"_s, lastUses[value], value);
    }
    return agrees;
}

static void printFunctionHeading(IRPrinter& printer, FunctionID id)
{
    printer.beginLine("function"_s, IDPrefix::Function, id.index());
    if (StringID name = printer.program().at(id).name)
        printer.field("name"_s, name);
    printer.endLine();
}

template<typename Functor>
static void forEachBodyIndexNamedBy(TestCommandContext& context, const Functor& functor)
{
    const Program& program = *context.program;
    std::optional<Vector<FunctionID>> functions = functionsNamedBy(program, context.arguments, context.out);
    if (!functions)
        return;

    IRPrinter printer(context.out, program);
    BodyIndexTable indexes(static_cast<uint32_t>(program.functions.size()), 1u);
    for (FunctionID id : *functions) {
        printFunctionHeading(printer, id);
        const Function& function = program.at(id);
        if (!function.hasBody()) {
            context.out.println("no body"_s);
            continue;
        }
        const BodyIndex* index = indexes.find(id);
        functor(index ? *index : indexes.build(id, function, 0));
    }
}

static void runBodyIndexCommand(TestCommandContext& context)
{
    forEachBodyIndexNamedBy(context, [&](const BodyIndex& index) {
        printBodyIndex(context.out, index);
    });
}

static void runEdgeResultsCommand(TestCommandContext& context)
{
    forEachBodyIndexNamedBy(context, [&](const BodyIndex& index) {
        printEdgeResults(context.out, index);
    });
}

static void runBodyLivenessCommand(TestCommandContext& context)
{
    BodyLiveness liveness;
    forEachBodyIndexNamedBy(context, [&](const BodyIndex& index) {
        printBodyLiveness(context.out, index, liveness);
    });
}

static void runLivenessAgainstPlainComputationCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    std::optional<Vector<FunctionID>> functions = functionsNamedBy(program, context.arguments, context.out);
    if (!functions)
        return;
    constexpr uint64_t mostBits = uint64_t(1) << 33;
    BodyIndexTable indexes(static_cast<uint32_t>(program.functions.size()), 1u);
    BodyLiveness liveness;
    unsigned numberThatAgree = 0;
    unsigned numberTooLarge = 0;
    for (FunctionID id : *functions) {
        const Function& function = program.at(id);
        if (!function.hasBody())
            continue;
        const BodyIndex& index = indexes.build(id, function, 0);
        liveness.compute(index);
        if (static_cast<uint64_t>(index.numberOfBlocks()) * index.numberOfValues() > mostBits) {
            ++numberTooLarge;
            continue;
        }
        if (livenessIsWhatPlainComputationFinds(context.out, index, liveness))
            ++numberThatAgree;
        else
            context.out.println("in f"_s, id.index());
    }
    context.out.println(numberThatAgree, " bodies: the liveness is what the plain computation finds"_s);
    if (numberTooLarge)
        context.out.println(numberTooLarge, " bodies are too large for the plain computation"_s);
}

std::span<const TestCommand> irTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "print"_s, runPrintIRCommand, ProgramOfCommand::AsWritten, "[f..]"_s },
        { "round-trip"_s, runRoundTripIRCommand, ProgramOfCommand::AsWritten, "[wire]"_s },
        { "verify"_s, runVerifyIRCommand, ProgramOfCommand::AsWritten, ""_s },
        { "body-index"_s, runBodyIndexCommand, ProgramOfCommand::AsWritten, "[f..]"_s },
        { "edge-results"_s, runEdgeResultsCommand, ProgramOfCommand::AsWritten, "[f..]"_s },
        { "liveness"_s, runBodyLivenessCommand, ProgramOfCommand::AsWritten, "[f..]"_s },
        { "liveness-against-plain-computation"_s, runLivenessAgainstPlainComputationCommand, ProgramOfCommand::AsWritten, "[f..]"_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
