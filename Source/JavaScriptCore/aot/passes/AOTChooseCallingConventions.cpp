/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChooseCallingConventionsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConvention.h"
#include "AOTDerivedFunctions.h"
#include "AOTDiagnostics.h"
#include "AOTPassRunner.h"
#include "AOTRelocation.h"
#include <algorithm>
#include <limits>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(FunctionConvention) == 4);
static_assert(defaultLimitValue(LimitID::MaxReservedStackArguments, hostArchitecture()) <= std::numeric_limits<decltype(FunctionConvention::reservedStackWords)>::max());

CallingConventions::CallingConventions()
{
    m_tables.registers = maxConventionRegisters();
}

CallingConventions::CallingConventions(CallingConventionTables&& tables)
    : m_tables(WTF::move(tables))
{
}

FunctionConvention CallingConventions::of(const Program& program, FunctionID id) const
{
    const Function& function = program.at(id);
    if (function.role == FunctionRole::Continuation) {
        FunctionConvention origin = of(program, function.creator.as<CreatorRecord::Continuation>().origin);
        return { origin.resultIsBoxed, false, origin.reservedStackWords };
    }

    FunctionConvention convention = id.index() < m_tables.conventions.size() ? m_tables.conventions[id.index()] : FunctionConvention();
    bool didChooseConventions = !m_tables.conventions.isEmpty();
    if (!didChooseConventions && function.results.size() == 1 && function.hasOrdinaryFrame())
        convention.resultIsBoxed = true;
    convention.spillsArgumentRegisters = entryConventionOf(function) == EntryConvention::GenericBuffer;
    return convention;
}

Representation CallingConventions::parameterRepresentation(const Program& program, FunctionID id, unsigned entryParameterIndex) const
{
    return parameterRepresentationOf(program, program.at(id), entryParameterIndex);
}

Representation CallingConventions::resultRepresentation(const Program& program, FunctionID id, unsigned resultIndex) const
{
    return resultRepresentationOf(program, program.at(id), of(program, id), resultIndex);
}

ArgumentLocations CallingConventions::locationsOf(const Program& program, FunctionID id) const
{
    return computeArgumentLocations(program, program.at(id), of(program, id), m_tables.registers);
}

uint32_t CallingConventions::stackWordsReservedByCallersOf(const Program& program, FunctionID callee) const
{
    return std::max<uint32_t>(locationsOf(program, callee).numberOfStackWords(), of(program, callee).reservedStackWords);
}

std::span<const SecondaryEntry> CallingConventions::secondaryEntriesOf(FunctionID id) const
{
    size_t index = id.index();
    if (index + 1 >= m_tables.firstSecondaryEntry.size())
        return { };
    uint32_t first = m_tables.firstSecondaryEntry[index];
    return m_tables.secondaryEntries.subspan(first, m_tables.firstSecondaryEntry[index + 1] - first);
}

SecondaryEntry CallingConventions::findEntryOmittingArguments(FunctionID callee, unsigned firstUndefinedGPR, unsigned numberOfGPRArguments) const
{
    RELEASE_ASSERT(firstUndefinedGPR <= numberOfGPRArguments && numberOfGPRArguments <= m_tables.registers.numberOfGPRs);
    for (SecondaryEntry entry : secondaryEntriesOf(callee)) {
        if (entry.firstOmittedGPR >= firstUndefinedGPR)
            return entry;
    }
    return { primaryEntry, static_cast<uint8_t>(numberOfGPRArguments) };
}

FunctionID CallingConventions::valueEntryOf(FunctionID genericEntrypoint) const
{
    return genericEntrypoint.index() < m_tables.valueEntries.size() ? m_tables.valueEntries[genericEntrypoint.index()] : FunctionID();
}

ClosureCells CallingConventions::closureCellsOf(FunctionID groupEntrypoint) const
{
    return groupEntrypoint.index() < m_tables.closureCells.size() ? m_tables.closureCells[groupEntrypoint.index()] : ClosureCells::Unknown;
}

static void dumpLocations(PrintStream& out, std::span<const ArgumentLocation> locations)
{
    CommaPrinter comma;
    out.print("(");
    for (const ArgumentLocation& location : locations) {
        out.print(comma);
        dump(out, location);
    }
    out.print(")");
}

static void dumpSecondaryEntries(PrintStream& out, std::span<const SecondaryEntry> entries)
{
    if (entries.empty())
        return;
    CommaPrinter comma;
    out.print(" entries=[");
    for (SecondaryEntry entry : entries)
        out.print(comma, entry.id, "@", entry.firstOmittedGPR);
    out.print("]");
}

void CallingConventions::dump(PrintStream& out, const Program& program) const
{
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            continue;

        FunctionID id(index);
        FunctionConvention convention = of(program, id);
        ArgumentLocations locations = locationsOf(program, id);
        out.print("f", index, " ", enumName(locations.convention), " ");
        dumpLocations(out, locations.parameters.span());
        out.print(" -> ");
        dumpLocations(out, locations.results.span());
        if (convention.resultIsBoxed)
            out.print(" boxed-result");
        if (convention.spillsArgumentRegisters)
            out.print(" spills");
        if (convention.reservedStackWords)
            out.print(" reserved=", convention.reservedStackWords);
        dumpSecondaryEntries(out, secondaryEntriesOf(id));
        if (FunctionID valueEntry = valueEntryOf(id))
            out.print(" value-entry=f", valueEntry.index());
        if (function.role == FunctionRole::GroupEntrypoint)
            out.print(" closures=", enumName(closureCellsOf(id)));
        out.println();
    }
}

bool ConventionInputs::isEnabledFor(FunctionID function) const
{
    return build.isEnabled(PassID::ChooseCallingConventions, findFunctionAsRead(&program(), function));
}

std::span<const FunctionID> ConventionInputs::entrypointsOfSameGroup(FunctionID groupEntrypoint) const
{
    const Function& function = program().at(groupEntrypoint);
    RELEASE_ASSERT(function.role == FunctionRole::GroupEntrypoint);
    CallGroupID group = groups.groupOf(function.creator.as<CreatorRecord::Entrypoint>().target);
    return groupEntrypoints[group.index()].all.span();
}

static uint64_t countClosuresNeverCreated(const Program& program, const DerivedFunctions& derivedFunctions)
{
    uint64_t count = 0;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !hasSourceBodyOf(function.role) || !canBeCalledByValueOf(function.role) || function.hasOnlyDirectCallers())
            continue;
        if (!derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, FunctionID(index))))
            ++count;
    }
    return count;
}

CallingConventions chooseCallingConventions(ProgramEditor& editor, const OptimizationInputs& inputs, const CallGroups& groups, const PassRunner& runner)
{
    RELEASE_ASSERT(inputs.derivedFunctions);
    const Program& program = editor.program();
    BuildContext& build = runner.build();
    Vector<GroupEntrypoints> groupEntrypoints = findGroupEntrypoints(groups, *inputs.derivedFunctions);
    ConventionInputs context { editor, inputs, groups, runner, build, groupEntrypoints };
    BitVector changed;
    unsigned count = program.functions.size();

    CallingConventionTables tables;
    tables.registers.numberOfGPRs = static_cast<unsigned>(std::min<uint64_t>(build.limit(LimitID::MaxArgumentRegisters), numberOfDirectArgumentGPRs));
    tables.registers.numberOfFPRs = numberOfDirectArgumentFPRs;
    tables.conventions.grow(count);
    tables.valueEntries.grow(count);
    tables.closureCells.fill(ClosureCells::Unknown, count);

    convertCallsToGroupCalls(context, tables, changed);

    Vector<TailCallSite> sites = collectTailCallSites(context);
    DirectCallUse use = collectDirectCallUse(context);
    chooseStubBodies(context, sites.span(), use, tables.registers);
    chooseResultRepresentations(context, sites.span(), use, tables);
    demoteMismatchedTailCalls(context, sites, tables, changed);
    reserveStackWords(context, sites.span(), tables);
    chooseArgumentSpills(context, tables);
    chooseSecondaryEntries(context, tables);

    RELEASE_ASSERT(program.functions.size() == count);
    build.statistics().count(StatisticID::ClosuresNeverCreated, countClosuresNeverCreated(program, *inputs.derivedFunctions));
    build.statistics().countChanges(PassID::ChooseCallingConventions, changed.bitCount());
    return CallingConventions(WTF::move(tables));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
