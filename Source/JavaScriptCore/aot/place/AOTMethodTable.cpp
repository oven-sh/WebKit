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
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using TableFunctions = MethodTableFunctions;

struct RowEntry {
    uint32_t shape { 0 };
    FunctionID target;
    GuardBitID guard;
};

struct Row {
    MethodSelector selector;
    FunctionID lookup;
    // In ascending order of the shapes, each once.
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
    const Program& program = context.program;
    const Function& entrypoint = program.at(id);
    if (entrypoint.blocks.size() != 1)
        return { };
    const Block& block = entrypoint.at(entryBlock);
    if (block.terminator.kind != TerminatorKind::ReturnCall || block.values.isEmpty() || block.terminator.callOperand() != block.values.last())
        return { };
    const Value& call = entrypoint.at(block.values.last());
    if (call.opcode != Opcode::CallDirect || call.argumentsOperands().size() != block.parameters.size())
        return { };
    for (ValueID id : block.values.span().first(block.values.size() - 1)) {
        if (!isCast(entrypoint.at(id).opcode))
            return { };
    }
    for (size_t index = 0; index < block.parameters.size(); ++index) {
        if (sameObjectThroughCasts(entrypoint, call.argumentsOperands()[index]) != block.parameters[index])
            return { };
    }

    const CallingConventions& conventions = context.inputs.callingConventions;
    ArgumentLocations given = conventions.locationsOf(program, id);
    ArgumentLocations taken = conventions.locationsOf(program, call.function());
    if (given.convention != taken.convention || given.parameters != taken.parameters || given.results != taken.results || conventions.stackWordsReservedByCallersOf(program, call.function()))
        return { };
    return call.function();
}

MethodTableFunctions collectMethodTableFunctions(const PlacementContext& context)
{
    const Program& program = context.program;
    TableFunctions functions;
    auto tableFunctions = findIndicesInParallel(context.build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        return function.hasBody() && (function.role == FunctionRole::TableEntrypoint || function.role == FunctionRole::TableLookup);
    });
    for (uint32_t index : tableFunctions) {
        const Function& function = program.functions[index];
        if (function.role == FunctionRole::TableEntrypoint) {
            FunctionID body = findBodyEnteredAsGiven(context, FunctionID(index));
            functions.entrypoints.append({ function.derivation.source, selectorOfTableFunction(function), body ? body : FunctionID(index) });
        }
        else if (function.role == FunctionRole::TableLookup)
            functions.lookups.append({ selectorOfTableFunction(function), FunctionID(index) });
    }
    std::ranges::sort(functions.entrypoints);
    std::ranges::sort(functions.lookups);
    return functions;
}

static void appendEntries(const ShapeTable& shapes, std::span<const LayoutID> layouts, FunctionID target, GuardBitID guard, Row& row)
{
    for (LayoutID layout : layouts) {
        ShapeRange range = shapes.shapesOf(layout);
        for (uint32_t shape = range.first; shape < range.end(); ++shape)
            row.entries.append({ shape, target, guard });
    }
}

// What the class definitions say holds while the bit of the arm's conditions is set. Conditions without a bit never hold.
static void appendEntriesOf(const PlacementContext& context, const TableCallUse& use, const TableFunctions& functions, const ShapeTable& shapes, const GuardBitTable& guardBits, Row& row)
{
    PropertyAccessContext access { context.program, &context.inputs.flow };
    MemberPartition partition = partitionMembers(access, context.program.at(use.receivers).members.span(), use.selector.key, Access::Read);
    for (const MemberArm& arm : partition.arms) {
        FunctionID target = arm.answer.kind == MemberKind::Method && arm.answer.receiverProofs.isEmpty() ? functions.entrypointOf(arm.answer.method, use.selector) : FunctionID();
        GuardBitID guard;
        if (target && !arm.answer.conditions.isEmpty()) {
            guard = guardBits.find(guardsOf(arm.answer.conditions.span()).span());
            if (!guard)
                target = { };
        }
        if (!target) {
            appendEntries(shapes, arm.layouts.span(), row.lookup, { }, row);
            continue;
        }
        LayoutList receivers = receiversOfTableEntrypoint(context.program, use.selector.key, arm.answer.method);
        for (LayoutID layout : arm.layouts) {
            bool isExpectedByEntrypoint = receivers.contains(layout);
            appendEntries(shapes, singleElementSpan(layout), isExpectedByEntrypoint ? target : row.lookup, isExpectedByEntrypoint ? guard : GuardBitID(), row);
        }
    }
    appendEntries(shapes, partition.rest.span(), row.lookup, { }, row);
}

static Vector<Row> createRows(const PlacementContext& context, const PlacedSites& sites, const ShapeTable& shapes, const GuardBitTable& guardBits, const TableFunctions& functions)
{
    Vector<Row> rows;
    for (const TableCallUse& use : sites.tableCalls) {
        if (rows.isEmpty() || rows.last().selector != use.selector)
            rows.append(Row { use.selector, functions.lookupOf(use.selector), { } });
        RELEASE_ASSERT(rows.last().lookup);
        appendEntriesOf(context, use, functions, shapes, guardBits, rows.last());
    }
    for (Row& row : rows) {
        std::ranges::stable_sort(row.entries, { }, &RowEntry::shape);
        row.entries.shrink(row.entries.size() - std::ranges::unique(row.entries, { }, &RowEntry::shape).size());
    }
    return rows;
}

static bool fits(const BitVector& isOccupied, const Row& row, uint32_t offset)
{
    return std::ranges::none_of(row.entries, [&](const RowEntry& entry) {
        return isOccupied.get(offset - entry.shape);
    });
}

MethodTable placeMethodTable(const PlacementContext& context, const PlacedSites& sites, const ShapeTable& shapes, const GuardBitTable& guardBits, const MethodTableFunctions& functions)
{
    MethodTable table;
    Vector<Row> rows = createRows(context, sites, shapes, guardBits, functions);

    // The longest rows first: the short ones fill what those leave open.
    Vector<uint32_t> order(rows.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::stable_sort(order, [&](uint32_t a, uint32_t b) {
        return rows[a].entries.size() > rows[b].entries.size();
    });

    BitVector isOccupied;
    Vector<uint32_t> offsets(FillWith { }, rows.size(), 0u);
    for (uint32_t index : order) {
        const Row& row = rows[index];
        if (row.entries.isEmpty())
            continue;
        uint32_t offset = row.entries.last().shape;
        while (!fits(isOccupied, row, offset))
            ++offset;
        offsets[index] = offset;
        for (const RowEntry& entry : row.entries)
            isOccupied.set(offset - entry.shape);
    }

    for (uint32_t index = 0; index < rows.size(); ++index) {
        const Row& row = rows[index];
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
    std::ranges::sort(table.dependents);

    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::MethodTableSelectors, table.selectors.size());
    statistics.count(StatisticID::MethodTableBytes, table.sizeInBytes());
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
