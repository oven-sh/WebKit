/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkStrings.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static void findModulesWhosePathNoOperationReadsForFactsOfStrings(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    size_t numberOfModules = program.modules.size();
    if (summaries.size() != program.functions.size() || linked.functions.size() != program.functions.size())
        return;
    BitVector modules(numberOfModules);
    for (size_t index = 0; index < numberOfModules; ++index)
        modules.quickSet(index);
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        if (!linked.function(function).isReached || !program.at(function).hasBody())
            continue;
        const ThinSummary& summary = summaries.at(function);
        if (summary.mayReadPathOfAnyModule)
            return;
        for (ModuleID module : summary.modulesWhosePathIsRead)
            modules.quickClear(module.index());
    }
    tables.modulesWhosePathNoOperationReads = WTF::move(modules);
}

static void findLayoutsThatParsersMayMakeForFactsOfStrings(const Program& program, const LinkTables& linked, FactTables& tables)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    if (linked.layouts.size() != numberOfLayouts)
        return;
    BitVector layouts(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const LinkedLayout& layout = linked.layouts[index];
        if (!layout.isAllocated || !layout.reachesUnseenCode)
            continue;
        if (parsersCouldMakeRecordsOf(program, LayoutID(index)))
            layouts.quickSet(index);
    }
    tables.layoutsThatParsersMayMake = WTF::move(layouts);
}

static void findFunctionsWhoseResultIsReadAsKeyForFactsOfStrings(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, FactTables& tables)
{
    if (summaries.size() != program.functions.size() || linked.functions.size() != program.functions.size())
        return;
    BitVector functions;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID caller(index);
        if (!summaries.at(caller).someResultIsReadAsKey || !linked.function(caller).isReached)
            continue;
        for (const SummaryCall& call : summaries.at(caller).calls) {
            if (!call.resultIsReadAsKey)
                continue;
            FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : linked.calleeAt(caller, call.site);
            if (!callee)
                callee = held.calleeAt(caller, call.site);
            if (callee)
                functions.set(callee.index());
        }
    }
    tables.functionsWhoseResultIsReadAsKey = WTF::move(functions);
}

void fillTablesOfStringsForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, FactTables& tables)
{
    findModulesWhosePathNoOperationReadsForFactsOfStrings(program, summaries, linked, tables);
    findLayoutsThatParsersMayMakeForFactsOfStrings(program, linked, tables);
    findFunctionsWhoseResultIsReadAsKeyForFactsOfStrings(program, summaries, linked, held, tables);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
