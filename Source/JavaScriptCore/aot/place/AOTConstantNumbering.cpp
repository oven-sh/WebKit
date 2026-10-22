/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTDefinitions.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowResults.h"
#include "AOTFunctionKinds.h"
#include "AOTNameClass.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

BitVector computeTypesNeededAtRunTime(const PlacementContext& context, const PlacedSites& sites, TypesTestedByCode typesTestedByCode)
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
        for (FieldIndex index = 0; index < numberOfFields; ++index) {
            const Field& declaration = fieldAt(program, declarationOf(program, { id, index }));
            // What FieldData::type holds: typeHeldBy() in AOTLayoutTableEncoding.cpp.
            if (declaration.form.contains(FieldForm::IsAccessor))
                addType(declaration.slotType);
            else if (!declaration.declared.flags.contains(PositionFlags::IsExpected))
                addType(declaration.slotType ? declaration.slotType : declaration.declared.type);
        }
    };

    addType(TypeTable::anything);
    addType(TypeTable::number);
    for (size_t index : sites.typesInRunTimePayloads)
        addType(TypeID(index));
    if (typesTestedByCode == TypesTestedByCode::Include) {
        for (size_t index : sites.typesTestedByCode)
            addType(TypeID(index));
    }
    // The tables name the type of a variable in two places: a static field of a class, and a binding that a direct eval sees.
    for (const Layout& layout : program.layouts) {
        for (const StaticField& field : layout.staticFields)
            addType(program.at(field.variable).declared.type);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                addType(program.at(binding.variable).declared.type);
            else if (binding.layout && binding.field && binding.kind != EvalBindingKind::MappedArgument)
                addType(fieldAt(program, declarationOf(program, { binding.layout, *binding.field })).declared.type);
        }
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
    // The word of a value that the host makes is filled at its first read and never at start: where the member may be replaced, the test of its location answers for the word.
    BuiltinCellKey first = key.kind() == BuiltinCellKey::Kind::Value ? BuiltinCellKey::object(builtins.at(key.builtin()).holder) : key;
    for (BuiltinCellKey step = first; step.builtin(); step = BuiltinCellKey::object(builtins.at(step.builtin()).holder)) {
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

static void numberBuiltinCells(const PlacementContext& context, PlacedSites& sites, const BitVector& typesWithCells, ConstantNumbering& numbering)
{
    const Program& program = context.program;
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinCellKey> cells = WTF::move(sites.builtinCells);
    for (const BitVector* types : { &sites.typesInRunTimePayloads, &sites.typesTestedByCode }) {
        for (size_t index : *types) {
            for (const ClassPart& part : program.at(TypeID(index)).classes)
                appendCellsOfClass(builtins.at(part.builtinClass), cells);
        }
    }
    for (size_t index : typesWithCells) {
        for (BuiltinID id : program.at(TypeID(index)).builtins)
            cells.append(BuiltinCellKey::object(id));
    }
    for (const Layout& layout : program.layouts) {
        if (BuiltinID parent = builtinParentNamedByClassRecord(program, layout))
            cells.append(BuiltinCellKey::object(parent));
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

// The code of these operations hands the cell of their key to the run time.
static constexpr OpcodeSet opcodesNamingKeyCell {
    Opcode::KeyConstant, Opcode::PropHasOwn, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::DefineMethod, Opcode::SuperSet,
};
static_assert((Value::opcodesWithKey | opcodesNamingKeyCell) == Value::opcodesWithKey);
static_assert(Value::opcodesWithKeys.contains(Opcode::DefineAccessors));

// A private name is no value, so it has no cell.
static bool hasKeyCell(PropertyKeyKind kind)
{
    return kind == PropertyKeyKind::Text || isSymbolKind(kind);
}

void collectKeyCellUses(const Program& program, FunctionID id, Vector<KeyCellUse>& uses)
{
    const Function& function = program.at(id);
    FunctionID source = firstSourceOf(program, id);
    ModuleID module = program.at(source).module;
    auto append = [&](PropertyKeyID key) {
        if (key && hasKeyCell(program.at(key).kind))
            uses.append(KeyCellUse { key, module ? module.index() : UINT32_MAX, source.index() });
    };
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (opcodesNamingKeyCell.contains(value.opcode))
                append(value.key());
            else if (value.opcode == Opcode::DefineAccessors)
                std::ranges::for_each(value.keys(), append);
        }
    }
}

// Few of the names of a program are ever values, so only those have a word in the table of key cells. The words are in the order of the program's text:
// most modules and functions of a large program never run, and what they name then shares no page with what the others name.
// A type test compares with the cell of a symbol, and the type it tests for can be the type of any value. There are few symbols, so each has a cell.
static void numberKeyCells(const PlacementContext& context, const KeyCellUsesOfBodies* usesOfBodies, ConstantNumbering& numbering)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    auto uses = collectInParallel<KeyCellUse>(loops, program.functions.size(), [&](unsigned index, Vector<KeyCellUse>& found) {
        if (!program.functions[index].hasBody())
            return;
        if (usesOfBodies)
            found.appendVector((*usesOfBodies)(FunctionID(index)));
        else
            collectKeyCellUses(program, FunctionID(index), found);
    });
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        if (isSymbolKind(program.at(PropertyKeyID(index)).kind))
            uses.append(KeyCellUse { .key = PropertyKeyID(index) });
    }

    parallelSort(loops, uses.mutableSpan(), std::less<KeyCellUse> { });
    removeRepeatedElements(uses, [](const KeyCellUse& first, const KeyCellUse& later) {
        return first.key == later.key;
    });
    std::ranges::sort(uses, { }, [](const KeyCellUse& use) {
        return std::tuple(use.module, use.sourceFunction, use.key);
    });

    numbering.keyCells = uses.map([](const KeyCellUse& use) {
        return use.key;
    });
    numbering.keyCellNumbersPlusOne.fill(0, numberOfKeys);
    for (uint32_t number = 0; number < uses.size(); ++number)
        numbering.keyCellNumbersPlusOne[uses[number].key.index()] = number + 1;
}

ConstantNumbering numberConstants(const PlacementContext& context, PlacedSites& sites, const BitVector& typesWithCells, const KeyCellUsesOfBodies* keyCellUsesOfBodies)
{
    ConstantNumbering numbering;
    numberBuiltinCells(context, sites, typesWithCells, numbering);
    numberKeyCells(context, keyCellUsesOfBodies, numbering);
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
    numbering.closureCellNumbers = numberClosureCells(program, context.build.parallelLoops());
    numbering.closureFunctions = closureFunctionRangeOf(program);
    return numbering;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
