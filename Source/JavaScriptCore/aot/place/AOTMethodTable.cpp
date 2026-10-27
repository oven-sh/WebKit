/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChooseCallingConventions.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RowEntry {
    uint32_t shape { 0 };
    FunctionID target;
    GuardBitID guard;
};

struct MethodTableRow {
    MethodSelector selector;
    FunctionID lookup;
    Vector<RowEntry> entries;
};

} // anonymous namespace

FunctionID MethodTableFunctions::entrypointOf(FunctionID method, MethodSelector selector) const
{
    auto found = std::ranges::lower_bound(entrypoints, std::tuple(method, selector), { }, [](const EntrypointOfMethod& entry) {
        return std::tuple(entry.method, entry.selector);
    });
    return found != entrypoints.end() && found->method == method && found->selector == selector ? found->entrypoint : FunctionID();
}

FunctionID MethodTableFunctions::lookupOf(MethodSelector selector) const
{
    auto found = std::ranges::lower_bound(lookups, selector, { }, &LookupOfSelector::selector);
    return found != lookups.end() && found->selector == selector ? found->lookup : FunctionID();
}

std::optional<uint32_t> MethodTable::offsetOf(MethodSelector selector) const
{
    auto found = std::ranges::lower_bound(selectors, selector, { }, &PlacedSelector::selector);
    if (found == selectors.end() || found->selector != selector)
        return std::nullopt;
    return found->offset;
}

// An entrypoint that has nothing to test hands what it was given to the body, where it was given it, and returns what the body returns. The table names the body then.
static FunctionID findBodyEnteredAsGiven(const PlacementContext& context, FunctionID id)
{
    const Function& entrypoint = context.program.at(id);
    if (entrypoint.blocks.size() != 1)
        return { };
    const Block& block = entrypoint.at(entryBlock);
    if (block.terminator.kind != TerminatorKind::ReturnCall || block.values.isEmpty() || block.terminator.callOperand() != block.values.last())
        return { };
    const Value& call = entrypoint.at(block.values.last());
    if (call.opcode != Opcode::CallDirect || call.argumentsOperands().size() != block.parameters.size())
        return { };
    for (ValueID id : block.values.span().first(block.values.size() - 1)) {
        if (!isCast(entrypoint.at(id).opcode) && !makesFact(entrypoint.at(id).opcode))
            return { };
    }
    for (size_t index = 0; index < block.parameters.size(); ++index) {
        if (sameObjectThroughCasts(entrypoint, call.argumentsOperands()[index]) != block.parameters[index])
            return { };
    }

    const CallingConventions& conventions = context.inputs.callingConventions;
    ArgumentLocations given = conventions.locationsOf(context.program, id);
    ArgumentLocations taken = conventions.locationsOf(context.program, call.function());
    if (given.convention != taken.convention || given.parameters != taken.parameters || given.results != taken.results || conventions.stackWordsReservedByCallersOf(context.program, call.function()))
        return { };
    return call.function();
}

MethodTableFunctions collectMethodTableFunctions(const PlacementContext& context)
{
    MethodTableFunctions functions;
    auto tableFunctions = findIndicesInParallel(context.build.parallelLoops(), context.program.functions.size(), [&](unsigned index) {
        const Function& function = context.program.functions[index];
        return function.hasBody() && (function.role == FunctionRole::TableEntrypoint || function.role == FunctionRole::TableLookup);
    });
    for (uint32_t index : tableFunctions) {
        const Function& function = context.program.functions[index];
        if (function.role == FunctionRole::TableEntrypoint) {
            FunctionID body = findBodyEnteredAsGiven(context, FunctionID(index));
            functions.entrypoints.append({ function.derivation.source, selectorOfTableFunction(function), body ? body : FunctionID(index) });
        } else
            functions.lookups.append({ selectorOfTableFunction(function), FunctionID(index) });
    }
    AOT::sort(functions.entrypoints);
    AOT::sort(functions.lookups);
    return functions;
}

// What the class definitions say holds while the bit of the arm's conditions is set. Conditions without a bit never hold.
static Vector<MethodTableRow> createRows(const PlacementContext& context, const PlacedSites& sites, const ShapeTable& shapes, const GuardBitTable& guardBits, const MethodTableFunctions& functions)
{
    Vector<MethodTableRow> rows;
    for (const TableCallUse& use : sites.tableCalls) {
        if (rows.isEmpty() || rows.last().selector != use.selector)
            rows.append(MethodTableRow { use.selector, functions.lookupOf(use.selector), { } });
        MethodTableRow& row = rows.last();
        RELEASE_ASSERT(row.lookup);
        auto append = [&](LayoutID layout, FunctionID target, GuardBitID guard) {
            ShapeRange range = shapes.shapesOf(layout);
            for (uint32_t shape = range.first; shape < range.end(); ++shape)
                row.entries.append({ shape, target, guard });
        };
        PropertyAccessContext access { context.program, &context.inputs.flow };
        MemberPartition partition = partitionMembers(access, context.program.at(use.receivers).completeMembers(), use.selector.key, Access::Read);
        for (const MemberArm& arm : partition.arms) {
            FunctionID target = arm.answer.kind == MemberKind::Method && arm.answer.receiverProofs.isEmpty() ? functions.entrypointOf(arm.answer.method, use.selector) : FunctionID();
            GuardBitID guard;
            if (target && !arm.answer.conditions.isEmpty()) {
                guard = guardBits.find(guardsOf(arm.answer.conditions.span()).span());
                if (!guard)
                    target = { };
            }
            LayoutList receivers = target ? receiversOfTableEntrypoint(context.program, use.selector.key, arm.answer.method) : LayoutList();
            for (LayoutID layout : arm.layouts) {
                bool isExpectedByEntrypoint = target && receivers.contains(layout);
                append(layout, isExpectedByEntrypoint ? target : row.lookup, isExpectedByEntrypoint ? guard : GuardBitID());
            }
        }
        for (LayoutID layout : partition.rest)
            append(layout, row.lookup, { });
    }
    for (MethodTableRow& row : rows) {
        AOT::stableSort(row.entries, { }, &RowEntry::shape);
        row.entries.shrink(row.entries.size() - std::ranges::unique(row.entries, { }, &RowEntry::shape).size());
    }
    return rows;
}

MethodTable placeMethodTable(const PlacementContext& context, const PlacedSites& sites, const ShapeTable& shapes, const GuardBitTable& guardBits, const MethodTableFunctions& functions)
{
    MethodTable table;
    Vector<MethodTableRow> rows = createRows(context, sites, shapes, guardBits, functions);

    // The longest rows first: the short ones fill what those leave open.
    Vector<uint32_t> order(rows.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::stableSort(order, [&](uint32_t a, uint32_t b) {
        return rows[a].entries.size() > rows[b].entries.size();
    });

    BitVector isOccupied;
    Vector<uint32_t> offsets(FillWith { }, rows.size(), 0u);
    for (uint32_t index : order) {
        const MethodTableRow& row = rows[index];
        if (row.entries.isEmpty())
            continue;
        uint32_t offset = row.entries.last().shape;
        while (std::ranges::any_of(row.entries, [&](const RowEntry& entry) { return isOccupied.get(offset - entry.shape); }))
            ++offset;
        offsets[index] = offset;
        for (const RowEntry& entry : row.entries)
            isOccupied.set(offset - entry.shape);
    }

    for (uint32_t index = 0; index < rows.size(); ++index) {
        const MethodTableRow& row = rows[index];
        table.selectors.append({ row.selector, offsets[index], row.lookup });
        for (const RowEntry& entry : row.entries) {
            uint32_t position = offsets[index] - entry.shape;
            if (position >= table.entries.size())
                table.entries.grow(position + 1);
            table.entries[position] = entry.target;
            if (entry.guard)
                table.dependents.append({ entry.guard, position, index });
        }
    }
    AOT::sort(table.dependents);

    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::MethodTableSelectors, table.selectors.size());
    statistics.count(StatisticID::MethodTableBytes, table.sizeInBytes());
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
