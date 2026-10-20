/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowNames.h"
#include "AOTFlowObjectsInternal.h"
#include "AOTSupportTestCommands.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename IDType>
static void printOptionalID(PrintStream& out, ASCIILiteral prefix, IDType id)
{
    if (!id) {
        out.print("-"_s);
        return;
    }
    out.print(prefix, id.index());
}

static void printFunctionID(PrintStream& out, const Program& program, FunctionID function)
{
    if (function.index() >= program.functions.size()) {
        out.print("host"_s);
        return;
    }
    out.print("f"_s, function.index());
}

static void printObject(PrintStream& out, const FlowObjectTable& table, FlowObjectID object)
{
    if (!object) {
        out.print("-"_s);
        return;
    }
    out.print("o"_s, object.index(), "("_s);
    table.dump(out, object);
    out.print(")"_s);
}

static void printAllocatedType(PrintStream& out, const BuiltinTable& builtins, LayoutID layout, BuiltinClassID builtinClass, FunctionID closureFunction)
{
    if (layout)
        out.print(" layout=L"_s, layout.index());
    if (builtinClass)
        out.print(" builtinClass="_s, builtins.tokenOf(builtinClass));
    if (closureFunction)
        out.print(" closure=f"_s, closureFunction.index());
}

static void printSites(PrintStream& out, const FlowObjectTable& table)
{
    const Program& program = table.program();
    for (unsigned index = 0; index < table.numberOfSites(); ++index) {
        const FlowSite& site = table.at(AllocationSiteID(index));
        const FlowSiteDescription& description = site.description;

        out.print("site s"_s, index, " function="_s);
        printFunctionID(out, program, site.function);
        out.print(" value="_s);
        printOptionalID(out, "v"_s, description.value);
        out.print(" kind="_s, nameOf(description.kind), " origin="_s, nameOf(description.origin));
        printAllocatedType(out, program.builtins(), description.layout, description.builtinClass, description.closureFunction);
        if (description.numberOfPositions)
            out.print(" positions="_s, description.numberOfPositions);
        if (description.ordinal)
            out.print(" ordinal="_s, description.ordinal);

        out.print(" class="_s);
        printOptionalID(out, "A"_s, site.sharedObject ? table.at(site.sharedObject).allocationClass : AllocationClassID());
        out.print(" object="_s);
        printObject(out, table, site.sharedObject);
        out.println();
    }
}

static void printClasses(PrintStream& out, const FlowObjectTable& table)
{
    const BuiltinTable& builtins = table.program().builtins();
    for (unsigned index = 0; index < table.numberOfAllocationClasses(); ++index) {
        const FlowAllocationClassMembers& members = table.at(AllocationClassID(index));

        out.print("class A"_s, index);
        printAllocatedType(out, builtins, members.layout, members.builtinClass, members.closureFunction);

        out.print(" sites=["_s);
        CommaPrinter siteComma;
        for (AllocationSiteID site : members.sites)
            out.print(siteComma, "s"_s, site.index());
        out.print("] shared="_s);
        printObject(out, table, members.sharedObject);

        if (!members.copies.isEmpty()) {
            out.print(" copies=["_s);
            CommaPrinter copyComma;
            for (FlowObjectID copy : members.copies) {
                out.print(copyComma);
                printObject(out, table, copy);
            }
            out.print("]"_s);
        }

        if (table.at(members.sharedObject).standsForLayout)
            out.print(" layout-object"_s);
        out.println();
    }
}

static void printObjects(PrintStream& out, const FlowObjectTable& table, const FlowRecords& records)
{
    for (unsigned index = unknownFlowObject.index() + 1; index < table.size(); ++index) {
        FlowObjectID object(index);
        const FlowObject& record = table.at(object);

        out.print("object o"_s, index, " "_s);
        table.dump(out, object);
        out.print(" kind="_s, nameOf(record.kind), " parts="_s, record.numberOfDeclaredParts, " positions="_s, record.numberOfPositions);
        out.print(" undeclared="_s, yesOrNo(record.hasUndeclaredPart), " events="_s);
        dumpFlags(out, records.eventsOf(object));
        out.println();
    }
}

static void printAllocationClasses(TestCommandContext& context)
{
    Vector<FlowSummary> summaries = buildFlowSummaries(*context.program, context.build);
    FlowObjectTable table(*context.program);
    createAllocationClasses(table, summaries.span(), context.build);

    printSites(context.out, table);
    printClasses(context.out, table);
}

static void printSolvedProgram(TestCommandContext& context)
{
    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(*context.program, context.program->types, context.build);

    printSites(context.out, *solved->objects);
    printClasses(context.out, *solved->objects);
    printObjects(context.out, *solved->objects, solved->records);
}

void runFlowSitesTestCommand(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    if (words.empty())
        return printAllocationClasses(context);

    bool isSolved = words[0] == "solved"_s;
    if (!isSolved && words[0] != "classes"_s)
        return printBadArgument(context.out, words[0]);
    if (words.size() > 1)
        return printBadArgument(context.out, words[1]);

    if (isSolved)
        return printSolvedProgram(context);
    printAllocationClasses(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
