/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowPropertyState.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include <memory>
#include <span>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOf(FlowConstructionRootKind kind)
{
    switch (kind) {
    case FlowConstructionRootKind::EntryParameter:
        return "EntryParameter"_s;
    case FlowConstructionRootKind::Allocation:
        return "Allocation"_s;
    case FlowConstructionRootKind::SuperConstruct:
        return "SuperConstruct"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(FlowConstructionTracking tracking)
{
    switch (tracking) {
    case FlowConstructionTracking::Unknown:
        return "Unknown"_s;
    case FlowConstructionTracking::Tracked:
        return "Tracked"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(FlowConstructionEventKind kind)
{
    switch (kind) {
    case FlowConstructionEventKind::Leak:
        return "Leak"_s;
    case FlowConstructionEventKind::Store:
        return "Store"_s;
    case FlowConstructionEventKind::Read:
        return "Read"_s;
    case FlowConstructionEventKind::Call:
        return "Call"_s;
    case FlowConstructionEventKind::Exit:
        return "Exit"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(FlowConstructionStores stores)
{
    switch (stores) {
    case FlowConstructionStores::Unknown:
        return "Unknown"_s;
    case FlowConstructionStores::OnlyInitialOrder:
        return "OnlyInitialOrder"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printName(PrintStream& out, const Program& program, const FlowConstructionName& name)
{
    if (name.key) {
        printPropertyKey(out, program, name.key);
        return;
    }
    out.print("L"_s, name.field.layout.index(), "."_s, name.field.field);
}

static void printNames(PrintStream& out, const Program& program, ASCIILiteral label, std::span<const FlowConstructionName> names)
{
    out.print(" "_s, label);
    for (const FlowConstructionName& name : names) {
        out.print(" "_s);
        printName(out, program, name);
    }
}

static void printLeak(PrintStream& out, ThisLeak leak, const Witness& witness)
{
    out.print(nameOf(leak));
    if (!hasFunction(witness))
        return;
    out.print(" at f"_s, witness.function);
    if (witness.value != Wire::none)
        out.print(":v"_s, witness.value);
}

static void printIndexes(PrintStream& out, const auto& indexes)
{
    out.print("["_s);
    CommaPrinter comma;
    for (auto index : indexes)
        out.print(comma, index);
    out.print("]"_s);
}

static void printEvent(PrintStream& out, const Program& program, const FlowConstructionSummary& summary, const FlowConstructionEvent& event)
{
    out.print("  b"_s, event.point.block.index(), ":"_s, event.point.index, " "_s, nameOf(event.kind));
    if (event.isExact)
        out.print(" exact"_s);
    if (event.name) {
        out.print(" "_s);
        printName(out, program, summary.names[*event.name]);
    }
    if (event.origin)
        out.print(" v"_s, event.origin.index());
    if (event.kind == FlowConstructionEventKind::Call)
        out.print(" operand "_s, event.operandIndex);
    if (event.storesSelfReference)
        out.print(" self-reference"_s);
    if (event.holder)
        out.print(" holder v"_s, event.holder.index());
    out.println();
}

static void printEvents(TestCommandContext& context)
{
    const Program& program = *context.program;
    PrintStream& out = context.out;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        if (!program.at(function).hasBody())
            continue;

        std::unique_ptr<FlowFunctionConstruction> construction = buildFunctionConstruction(program, context.build, function);
        for (const FlowConstructionSummary& summary : construction->summaries) {
            out.println("events f"_s, index, " root v"_s, summary.root.definition.index(), " "_s, nameOf(summary.root.kind), " "_s, nameOf(summary.tracking));
            for (const FlowConstructionEvent& event : summary.events)
                printEvent(out, program, summary, event);
        }
    }
}

static void printReadsFromPrototype(PrintStream& out, const FlowSolution& solution, const FlowSolvedConstructionEntry& entry)
{
    const FlowConstructionState& state = solution.propertyState.constructions;
    FunctionID function = solution.graph.at(entry.instance).function;
    const FlowConstructionSummary* summary = state.functions[function.index()]->find(entry.root.definition);
    if (!summary)
        return;

    bool isFirst = true;
    for (const FlowConstructionEvent& event : summary->events) {
        if (!isReadOfCreatedObject(solution.program.at(function), entry.root, event) || lookupStartOf(state, function, event.origin) != FlowLookupStart::Prototype)
            continue;
        if (std::exchange(isFirst, false))
            out.print(" reads-from-prototype"_s);
        out.print(" v"_s, event.origin.index());
    }
}

static void printConstructors(PrintStream& out, const FlowSolution& solution)
{
    const Program& program = solution.program;
    for (const FlowSolvedConstructionEntry& entry : solution.propertyState.constructions.solvedConstructors) {
        const FlowSolvedConstruction& solved = entry.solved;
        out.print("construction f"_s, solution.graph.at(entry.instance).function.index(), "@i"_s, entry.instance.index());
        out.print(" root v"_s, entry.root.definition.index(), ": "_s);
        printLeak(out, solved.leak, solved.leakWitness);
        printNames(out, program, "before-leak"_s, solved.storedBeforeLeak.span());
        printNames(out, program, "at-exit"_s, solved.storedAtExit.span());
        printNames(out, program, "read-early"_s, solved.readBeforeStore.span());
        printNames(out, program, "initial-order"_s, solved.initialOrder.span());
        out.print(" stores "_s, nameOf(solved.stores));
        if (!solved.selfReferenceNames.isEmpty())
            printNames(out, program, "self-references"_s, solved.selfReferenceNames.span());
        printReadsFromPrototype(out, solution, entry);
        if (!solved.observedDefinitions.isEmpty())
            printNames(out, program, "observed-definitions"_s, solved.observedDefinitions.span());
        out.println();
    }
}

static void printFieldDefinitions(PrintStream& out, const FlowSolution& solution)
{
    const Vector<std::unique_ptr<FlowFunctionConstruction>>& functions = solution.propertyState.constructions.functions;
    for (size_t index = 0; index < functions.size(); ++index) {
        if (!functions[index] || functions[index]->fieldDefinitions.isEmpty())
            continue;
        out.print("definitions f"_s, index, ":"_s);
        for (const FlowFieldDefinition& definition : functions[index]->fieldDefinitions)
            out.print(" v"_s, definition.definition.index(), definition.isObserved ? " observed"_s : " unobserved"_s);
        out.println();
    }
}

static bool isUnknown(const FlowConstructionResult& result)
{
    return result.thisLeak == ThisLeak::Unknown
        && result.fieldCreation == FieldCreation::Unknown
        && result.constructorOrders.isEmpty()
        && result.fieldsSetAtCreation.isEmpty()
        && !hasFunction(result.leakWitness);
}

static void printLayouts(PrintStream& out, const FlowSolution& solution)
{
    const Program& program = solution.program;
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID layout(index);
        if (program.at(layout).kind != LayoutKind::Instance)
            continue;

        FlowConstructionResult result = constructionOf(solution, layout);
        if (isUnknown(result))
            continue;

        out.print("layout L"_s, index, ": "_s);
        printLeak(out, result.thisLeak, result.leakWitness);
        out.print(" "_s, nameOf(result.fieldCreation), " set "_s);
        printIndexes(out, result.fieldsSetAtCreation);
        out.print(" orders"_s);
        for (const FieldOrder& order : result.constructorOrders) {
            out.print(" "_s);
            printIndexes(out, order.fields);
        }
        out.println();
    }
}

static void printFunctions(PrintStream& out, const FlowSolution& solution)
{
    out.print("functions that may hold an unfinished object:"_s);
    for (size_t index = 0; index < solution.program.functions.size(); ++index) {
        if (!seesOnlyFinishedObjects(solution, FunctionID(index)))
            out.print(" f"_s, index);
    }
    out.println();
}

void runFlowConstructionTestCommand(TestCommandContext& context)
{
    bool printsEvents = false;
    bool printsConstructors = context.arguments.empty();
    bool printsLayouts = context.arguments.empty();
    bool printsFunctions = false;
    bool printsDefinitions = false;
    for (const String& word : context.arguments) {
        if (word == "events"_s)
            printsEvents = true;
        else if (word == "constructors"_s)
            printsConstructors = true;
        else if (word == "layouts"_s)
            printsLayouts = true;
        else if (word == "functions"_s)
            printsFunctions = true;
        else if (word == "definitions"_s)
            printsDefinitions = true;
        else
            return printBadArgument(context.out, word);
    }

    if (printsEvents)
        printEvents(context);
    if (!printsConstructors && !printsLayouts && !printsFunctions && !printsDefinitions)
        return;

    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(*context.program, context.program->types, context.build);
    FlowSolution solution = solved->solution();
    if (printsConstructors)
        printConstructors(context.out, solution);
    if (printsLayouts)
        printLayouts(context.out, solution);
    if (printsFunctions)
        printFunctions(context.out, solution);
    if (printsDefinitions)
        printFieldDefinitions(context.out, solution);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
