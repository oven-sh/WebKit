/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTFrameAllocationPlanner.h"
#include "AOTFreshResults.h"
#include "AOTHeat.h"
#include "AOTIRPrinter.h"
#include "AOTPipeline.h"
#include "AOTSinkAllocationsInternal.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using FunctionPrinter = void (*)(TestCommandContext&, FunctionID, FunctionAnalyses&);

} // anonymous namespace

template<FunctionPrinter print>
static void runOnFunctions(TestCommandContext& context)
{
    Program& program = *context.program;
    CommandWords words(context);
    FlowPreparation preparation = prepareFlow(context, words);
    if (preparation.flow) {
        preparation.flow->setBorrowedParameters(computeBorrowedParameters(program, context.build));
        preparation.flow->setFrameArrayParameters(computeFrameArrayParameters(program, context.build));
    }
    std::optional<FunctionID> onlyFunction = words.parseOptional("function"_s, functionParser(program));
    if (!words.finish())
        return;

    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody() || (onlyFunction && *onlyFunction != id))
            continue;

        context.out.println("function f", id.index());
        FunctionAnalyses analyses(AnalysisContext { context.build, program, preparation.flowOrNull(), preparation.effectsOrNull(), id }, function);
        print(context, id, analyses);
    }
}

template<typename T>
static void printList(PrintStream& out, ASCIILiteral label, std::span<const T> items, NOESCAPE const Invocable<void(PrintStream&, const T&)> auto& printItem)
{
    out.print(label, ": ");
    dumpList(out, items, printItem);
    out.print("\n");
}

static void printLimit(PrintStream& out, LimitID limit)
{
    if (limit != LimitID::None)
        out.print(" limit=", nameOf(limit));
}

static std::optional<bool> mayLeakThrough(const Program& program, const FlowResults* flow, const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return mayLeakFramePointer(program, flow, function, function.at(use.user), use.operandIndex);
    case Use::Kind::TerminatorOperand:
        return mayLeakFramePointer(program, flow, function, function.at(use.block).terminator, use.operandIndex);
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printFramePointers(TestCommandContext& context, FunctionID, FunctionAnalyses& analyses)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const FlowResults* flow = analyses.context().flow;
    PrintStream& out = context.out;

    FramePointers pointers(program, function);
    Vector<ValueID> framePointers = idsIn<ValueID>(pointers.values());
    auto printValueID = [](PrintStream& out, ValueID value) { dumpID(out, value); };
    printList(out, "allocations"_s, pointers.allocations(), printValueID);
    printList(out, "frame pointers"_s, framePointers.span(), printValueID);
    auto printPlaces = [&](ASCIILiteral label, CellPlace::Kind kind) {
        Vector<CellPlace> places(pointers.placesWithPointers());
        places.removeAllMatching([&](const CellPlace& place) {
            return place.kind != kind;
        });
        printList(out, label, places.span(), [&](PrintStream& out, const CellPlace& place) {
            if (kind == CellPlace::Kind::Key)
                printPropertyKey(out, program, PropertyKeyID(place.owner));
            else
                out.print(kind == CellPlace::Kind::Field ? "L" : "f", place.owner, ".", place.index);
        });
    };
    printPlaces("environment fields"_s, CellPlace::Kind::Field);
    printPlaces("keys"_s, CellPlace::Kind::Key);
    printPlaces("captures"_s, CellPlace::Kind::Capture);

    for (ValueID pointer : framePointers) {
        for (const Use& use : analyses.useLists().usesOf(pointer)) {
            if (std::optional<bool> mayLeak = mayLeakThrough(program, flow, function, use))
                dumpLine(out, pointer, " "_s, use, " may leak: "_s, *mayLeak);
        }
    }

    Vector<InternalError> found;
    verifyFramePointers(analyses, found);
    context.build.diagnostics().append(WTF::move(found));
}

static std::optional<ASCIILiteral> forcingOperationOf(const AllocationRemark& remark)
{
    if (remark.forcingOpcode)
        return nameOf(*remark.forcingOpcode);
    if (hasFlag(remark.reasons, AllocationEscapeReason::Returned))
        return "Return"_s;
    if (hasFlag(remark.reasons, AllocationEscapeReason::Thrown))
        return "Throw"_s;
    return std::nullopt;
}

static void printOutcome(PrintStream& out, const AllocationRemark& remark)
{
    switch (remark.outcome) {
    case AllocationOutcome::Unknown:
        out.print("unknown");
        return;
    case AllocationOutcome::Removed:
        out.print("removed");
        return;
    case AllocationOutcome::Frame:
        out.print("frame");
        return;
    case AllocationOutcome::FrameUntilMaterialized:
        out.print("frame until materialized");
        return;
    case AllocationOutcome::Slim:
        out.print("slim");
        return;
    case AllocationOutcome::Deferred:
        out.print("deferred");
        return;
    case AllocationOutcome::Heap:
        if (remark.copies == 1)
            out.print("heap");
        else
            out.print("allocated at ", remark.copies, " sites");
        if (std::optional<ASCIILiteral> operation = forcingOperationOf(remark))
            out.print(": ", *operation);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printRemark(PrintStream& out, const AllocationRemark& remark)
{
    printOutcome(out, remark);
    if (remark.cause != AllocationHeapCause::None)
        out.print(" cause=", nameOf(remark.cause));
    printLimit(out, remark.limit);
    out.print("\n");
}

static void printFramePlan(TestCommandContext& context, FunctionID, FunctionAnalyses& analyses)
{
    PrintStream& out = context.out;
    // The heat of a program that nobody has estimated: each function is entered once.
    Heat heat;
    FramePlan plan = planFrame(analyses, heat);
    for (const FrameDecision& decision : plan.decisions) {
        dumpID(out, decision.allocation);
        out.print(": ", nameOf(decision.storage));
        if (decision.storage == AllocationStorage::Heap)
            out.print(" ", nameOf(decision.cause));
        if (decision.forcedBy) {
            out.print(" by ");
            dumpUse(out, *decision.forcedBy);
        }
        printLimit(out, decision.limit);
        out.print("\n");
        if (context.build.options().collectsAllocationRemarks()) {
            out.print("  remark: ");
            printRemark(out, remarkOf(analyses.function(), decision));
        }
    }
}

static void printDescribedObject(PrintStream& out, ValueID allocation, const DescribedObject& described)
{
    dumpID(out, allocation);
    out.print(": ", nameOf(described.support));
    if (described.support == VirtualObjectSupport::Supported) {
        const VirtualObjectDescription& description = described.description;
        out.print(" ", nameOf(description.kind));
        if (description.layout)
            out.print(" L", description.layout.index());
        if (description.function)
            out.print(" f", description.function.index());
    }
    printLimit(out, described.limit);
    out.print("\n");
}

static void printSunkAllocation(PrintStream& out, const SunkAllocation& allocation)
{
    dumpID(out, allocation.allocation);
    out.print(": ", enumName(allocation.outcome));
    if (allocation.outcome == SunkAllocationOutcome::Sunk)
        out.print(" builds=", allocation.numberOfBuilds);
    printLimit(out, allocation.limit);
    out.print("\n");
}

static void printVirtualObjects(TestCommandContext& context, FunctionID, FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (interpretedOf(function.at(id).opcode) == Interpretation::Allocation)
                printDescribedObject(context.out, id, describeVirtualObject(analyses, id));
        }
    }
    for (const SunkAllocation& allocation : predictSinking(analyses))
        printSunkAllocation(context.out, allocation);
}

static void printCandidate(PrintStream& out, VirtualObjectID object, const Candidate& candidate)
{
    out.print("o", object.index(), ": ");
    dumpID(out, candidate.definition);
    out.print(" ");
    dumpID(out, candidate.block);
    out.print(" family=", candidate.family.index());
    if (candidate.isJoin)
        out.print(" join");
    if (candidate.isAbandoned)
        out.print(" abandoned");
    printLimit(out, candidate.abandonedBy);
    out.print("\n");
}

static void printSinkingStates(TestCommandContext& context, FunctionID, FunctionAnalyses& analyses)
{
    PrintStream& out = context.out;
    AllocationSinking sinking(analyses, SinkingPolicy { ReturnedObjects::AreBuilt, false });
    if (!sinking.collectCandidates()) {
        out.println("no candidates");
        return;
    }
    sinking.analyze();

    for (uint32_t index = 0; index < sinking.numberOfCandidates(); ++index)
        printCandidate(out, VirtualObjectID(index), sinking.at(VirtualObjectID(index)));

    BitVector printedFamilies;
    for (uint32_t index = 0; index < sinking.numberOfCandidates(); ++index) {
        VirtualObjectID object(index);
        uint32_t family = sinking.at(object).family.index();
        if (!printedFamilies.add(family))
            continue;
        out.println("family ", family, ":");
        sinking.familyOf(object).dump(out);
    }

    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const SinkingState* head = sinking.headOf(block);
        if (!head)
            continue;
        dumpLine(out, block, ":"_s);
        head->dump(out);
    }
}

static void printFreshResults(TestCommandContext& context, FunctionID, FunctionAnalyses& analyses)
{
    context.out.println(computeFreshResults(analyses));
}

static ASCIILiteral describeBorrowing(const BorrowedParameters& borrowed, FunctionID id, uint32_t entryParameter)
{
    if (!borrowed.isBorrowed(id, entryParameter))
        return "leaks"_s;
    return borrowed.borrowsContents(id, entryParameter) ? "borrowed"_s : "borrowed contents-leak"_s;
}

static void runBorrowedParameters(TestCommandContext& context)
{
    CommandWords words(context);
    prepareFlow(context, words);
    if (!words.finish())
        return;

    const Program& program = *context.program;
    BorrowedParameters borrowed = computeBorrowedParameters(program, context.build);
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody())
            continue;

        context.out.print("f", id.index(), ":");
        CommaPrinter separator(", "_s, " "_s);
        uint32_t numberOfParameters = function.at(entryBlock).parameters.size();
        for (uint32_t entryParameter = 0; entryParameter < numberOfParameters; ++entryParameter)
            context.out.print(separator, entryParameter, " ", describeBorrowing(borrowed, id, entryParameter));
        context.out.print("\n");
    }
}

static void runFrameArrayParameters(TestCommandContext& context)
{
    CommandWords words(context);
    prepareFlow(context, words);
    if (!words.finish())
        return;

    const Program& program = *context.program;
    FrameArrayParameters table = computeFrameArrayParameters(program, context.build);
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody())
            continue;

        context.out.print("f", id.index(), ": [");
        CommaPrinter separator;
        uint32_t numberOfParameters = function.at(entryBlock).parameters.size();
        for (uint32_t entryParameter = 0; entryParameter < numberOfParameters; ++entryParameter) {
            if (table.mayTake(id, entryParameter))
                context.out.print(separator, entryParameter);
        }
        context.out.println("]");
    }
}

static void printLocation(PrintStream& out, const Program& program, SourceLocation location)
{
    if (!hasModule(location)) {
        out.print("none");
        return;
    }
    const Module& module = program.modules[location.module];
    StringID path = module.displayPath ? module.displayPath : module.path;
    out.print(byteCast<char8_t>(program.string(path)), ":", program.lineColumnFor(ModuleID(location.module), location.position).line);
}

static void runAllocationRemarks(TestCommandContext& context)
{
    CommandWords words(context);
    if (!words.finish())
        return;
    if (context.form != IRForm::AsRead) {
        context.out.println("error: allocation-remarks starts from AsRead");
        return;
    }
    if (!context.build.options().collectsAllocationRemarks()) {
        context.out.println("error: allocation-remarks needs the option report=Allocations");
        return;
    }

    std::unique_ptr<PipelineOutput> output = runPipelineUntil(PassID::AllocateInFrames, WTF::move(context.program), context.build);
    if (!output) {
        reopenDiagnostics(context.build);
        return;
    }
    context.program = WTF::move(output->program);
    context.form = context.program->form();

    for (const AllocationRemark& remark : context.build.takeAllocationRemarks()) {
        printLocation(context.out, *context.program, remark.location);
        context.out.print(": ");
        printRemark(context.out, remark);
    }
}

std::span<const TestCommand> allocationTestCommands()
{
    static constexpr TestCommand commands[] {
        { "frame-pointers"_s, IRForm::Prepared, runOnFunctions<printFramePointers>, ProgramAsRead::TypedAndVerified },
        { "allocation-remarks"_s, IRForm::AsRead, runAllocationRemarks, ProgramAsRead::WithLibrary },
        { "virtual-objects"_s, IRForm::Prepared, runOnFunctions<printVirtualObjects>, ProgramAsRead::TypedAndVerified },
        { "sinking-states"_s, IRForm::Prepared, runOnFunctions<printSinkingStates>, ProgramAsRead::TypedAndVerified },
        { "fresh-results"_s, IRForm::Prepared, runOnFunctions<printFreshResults>, ProgramAsRead::TypedAndVerified },
        { "frame-plan"_s, IRForm::Prepared, runOnFunctions<printFramePlan>, ProgramAsRead::TypedAndVerified },
        { "borrowed-parameters"_s, IRForm::Prepared, runBorrowedParameters, ProgramAsRead::TypedAndVerified },
        { "frame-array-parameters"_s, IRForm::Prepared, runFrameArrayParameters, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
