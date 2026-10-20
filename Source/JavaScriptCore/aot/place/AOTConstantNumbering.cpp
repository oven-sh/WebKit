/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowResults.h"
#include "AOTThreads.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

BitVector computeTypesNeededAtRunTime(const PlacementContext& context, const PlacedSites& sites)
{
    const Program& program = context.program;
    BitVector neededTypes(program.types.size());
    BitVector visitedLayouts(program.layouts.size());
    Vector<TypeID> worklist;
    auto addType = [&](TypeID type) {
        if (type && neededTypes.add(type.index()))
            worklist.append(type);
    };
    auto addFieldTypes = [&](LayoutID id) {
        if (!visitedLayouts.add(id.index()))
            return;
        FieldIndex numberOfFields = program.at(id).fields.size();
        for (FieldIndex index = 0; index < numberOfFields; ++index)
            addType(fieldAt(program, declarationOf(program, { id, index })).declared.type);
    };

    addType(TypeTable::anything);
    addType(TypeTable::number);
    for (size_t index : sites.typesInRunTimePayloads)
        addType(TypeID(index));
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        // A variable that is described to the run time always has storage in the plan.
        if (context.slots().variable(VariableID(index)).storage != VariableStorage::None)
            addType(program.variables[index].declared.type);
    }
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (context.receivesShape(id) || layout.kind == LayoutKind::Instance || layout.isEnvironment())
            addFieldTypes(id);
    }
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        for (const ClassPart& part : type.classes) {
            for (TypeID contents : part.contents)
                addType(contents);
        }
        for (LayoutID layout : membersOf(program, type))
            addFieldTypes(layout);
    }
    return neededTypes;
}

static void appendCellsOfClass(const BuiltinClass& builtinClass, Vector<BuiltinCellKey>& cells)
{
    if (builtinClass.constructor)
        cells.append(BuiltinCellKey::object(builtinClass.constructor));
    if (builtinClass.prototype)
        cells.append(BuiltinCellKey::object(builtinClass.prototype));
}

static void sortAndRemoveKeysWithoutCell(const BuiltinTable& builtins, Vector<BuiltinCellKey>& cells)
{
    std::ranges::sort(cells, { }, &BuiltinCellKey::encoded);
    removeRepeatedElements(cells);
    cells.removeAllMatching([&](BuiltinCellKey key) {
        return !key.hasCell(builtins);
    });
}

// The run time finds a cell by the names on its path, from the nearest holder that start-up found: the holder of a locked member, or an eager cell.
static std::optional<BuiltinCellKey> findFirstReplaceableStep(const PlacementContext& context, BuiltinCellKey key)
{
    const BuiltinTable& builtins = context.program.builtins();
    for (BuiltinCellKey step = key; step.builtin(); step = BuiltinCellKey::object(builtins.at(step.builtin()).holder)) {
        if (mayBeReplaced(context.program, context.inputs.flow, step.builtin()))
            return step;
        if (isLockedLocation(builtins, step.builtin()))
            break;
    }
    return std::nullopt;
}

static Vector<BuiltinCellKey> findEagerCells(const PlacementContext& context, std::span<const BuiltinCellKey> cellsInCode)
{
    Vector<BuiltinCellKey> eagerCells;
    for (BuiltinCellKey key : cellsInCode) {
        if (std::optional<BuiltinCellKey> step = findFirstReplaceableStep(context, key))
            eagerCells.append(*step);
    }
    sortAndRemoveKeysWithoutCell(context.program.builtins(), eagerCells);
    return eagerCells;
}

static void numberBuiltinCells(const PlacementContext& context, PlacedSites& sites, const BitVector& typesNeededAtRunTime, ConstantNumbering& numbering)
{
    const Program& program = context.program;
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinCellKey> cells = WTF::move(sites.builtinCells);
    for (size_t index : sites.typesInRunTimePayloads) {
        for (const ClassPart& part : program.at(TypeID(index)).classes)
            appendCellsOfClass(builtins.at(part.builtinClass), cells);
    }
    for (size_t index : typesNeededAtRunTime) {
        for (BuiltinID id : program.at(TypeID(index)).builtins)
            cells.append(BuiltinCellKey::object(id));
    }
    sortAndRemoveKeysWithoutCell(builtins, cells);
    Vector<BuiltinCellKey> eagerCells = findEagerCells(context, cells.span());

    cells.appendVector(eagerCells);
    for (const BuiltinUseRecord& use : context.inputs.flow.builtinUses())
        cells.append(BuiltinCellKey::object(BuiltinID(use.builtin)));
    sortAndRemoveKeysWithoutCell(builtins, cells);

    numbering.eagerBuiltinCells = eagerCells.map([&](BuiltinCellKey key) {
        return static_cast<uint32_t>(std::ranges::lower_bound(cells, key.encoded(), { }, &BuiltinCellKey::encoded) - cells.begin());
    });
    numbering.builtinCells = WTF::move(cells);
}

ConstantNumbering numberConstants(const PlacementContext& context, PlacedSites& sites, const BitVector& typesNeededAtRunTime)
{
    ConstantNumbering numbering;
    numberBuiltinCells(context, sites, typesNeededAtRunTime, numbering);
    numbering.bigIntConstants = WTF::move(sites.bigIntConstants);
    numbering.requireEdges = WTF::move(sites.requireEdges);
    numbering.requireEdgeSpecifiers = WTF::move(sites.requireEdgeSpecifiers);
    return numbering;
}

static BitVector findFunctionsDeclaredByVariables(const Program& program)
{
    BitVector declared(program.functions.size());
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function)
            declared.quickSet(variable.function.index());
    }
    return declared;
}

static ClosureIdentity closureIdentityOf(const Program& program, const PlacedSites& sites, const BitVector& declaredByVariables, FunctionID id)
{
    bool isCreatedByTable = declaredByVariables.quickGet(id.index()) || hasSingletonClosure(program, id);
    if (!isCreatedByTable || !program.at(id).captures.isEmpty() || sites.functionsWithAllocatedClosures.get(id.index()))
        return ClosureIdentity::Unknown;
    return ClosureIdentity::TableCell;
}

FunctionNumbering numberFunctions(const PlacementContext& context, const PlacedSites& sites)
{
    const Program& program = context.program;
    unsigned numberOfFunctions = program.functions.size();
    BitVector declaredByVariables = findFunctionsDeclaredByVariables(program);
    FunctionNumbering numbering;
    numbering.kinds.grow(numberOfFunctions);
    numbering.constructedClasses.grow(numberOfFunctions);
    numbering.closureIdentities.grow(numberOfFunctions);
    forEachIndexInParallel(context.build.parallelLoops(), numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        numbering.kinds[index] = functionKindOf(program, context.slots(), id);
        numbering.constructedClasses[index] = program.at(id).constructedClass;
        numbering.closureIdentities[index] = closureIdentityOf(program, sites, declaredByVariables, id);
    });
    numbering.closureFunctions = closureFunctionRangeOf(program);
    return numbering;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
