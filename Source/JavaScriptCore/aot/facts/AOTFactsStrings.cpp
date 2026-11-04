/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkStrings.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// ---- Facts::isPathReadByOperation()

// The table is made of what the lists do not name, and only a scan that came to the end of a body is known to have filled them. Code is made for what Link has reached and for nothing else.
// Nobody knows what stands in a body that the scan has left, and a name that is made when the program runs may be that of any file: then the table stays empty, which keeps the path of every module.
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

// ---- Facts::parsersMayMake()

// A parser leaves out what the text leaves out, lists the keys as the text has them and adds the members that the layout does not declare. No text makes a function.
static bool parsersCouldMakeRecordsOfLayoutForFactsOfStrings(const Program& program, LayoutID id)
{
    constexpr OptionSet<LayoutFlags> flagsOfOtherLayouts { LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction, LayoutFlags::HasMembersWithoutLayout, LayoutFlags::IsTypeParameter, LayoutFlags::IsNeverAllocated, LayoutFlags::IsWrittenUnderDictionaryType, LayoutFlags::IsDerived, LayoutFlags::IsOfCreatedObject };
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || layout.constructor || layout.base || layout.flags.containsAny(flagsOfOtherLayouts) || layout.fields.isEmpty())
        return false;
    return std::ranges::all_of(layout.fields, [&](const Field& field) {
        if (!field.key)
            return false;
        TypeID written = field.declared.written();
        if (!written)
            return true;
        Type type = program.at(written);
        return type.isAnything() || !type.mayBeFunction();
    });
}

// ONLY A LAYOUT THAT linked() HAS GIVEN UP: some object of it reaches code that nobody has seen, so every field holds anything, every change is possible, and no read goes without a look.
// A maker that nobody has seen adds nothing to that. Of a layout that linked() holds closed a store that does not fit ends the process.
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
        if (parsersCouldMakeRecordsOfLayoutForFactsOfStrings(program, LayoutID(index)))
            layouts.quickSet(index);
    }
    tables.layoutsThatParsersMayMake = WTF::move(layouts);
}

// ---- Facts::resultIsReadAsKey()

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

// ---- LayoutFact::ObjectsAreDictionaries of constructor functions

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
