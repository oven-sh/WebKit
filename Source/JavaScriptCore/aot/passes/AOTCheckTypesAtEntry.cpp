/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCheckTypesAtEntry.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTIRBuilder.h"
#include "AOTThreads.h"

namespace JSC { namespace AOT { namespace Typed {

static bool hasCheckedTypeFromSource(const TypeTable& types, const DeclaredType& declared)
{
    if (!declared.type || declared.flags.containsAny({ PositionFlags::IsFromCompiler, PositionFlags::IsExpected }))
        return false;
    const Type& type = types.at(declared.type);
    return type.isJavaScriptValue() && !type.isAnything();
}

static bool isPassedByCaller(const Function& function, unsigned entryParameterIndex)
{
    switch (function.entryParameterKind(entryParameterIndex)) {
    case EntryParameter::This:
        return true;
    case EntryParameter::Parameter:
        return entryParameterIndex - function.firstParameterIndex() < function.numberOfPositionalParameters();
    case EntryParameter::Capture:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const DeclaredType* findTypeCheckedAtEntry(const TypeTable& types, const Function& function, unsigned entryParameterIndex)
{
    if (!isPassedByCaller(function, entryParameterIndex))
        return nullptr;
    const DeclaredType* declared = function.declaredEntryParameter(entryParameterIndex);
    return hasCheckedTypeFromSource(types, *declared) ? declared : nullptr;
}

static bool hasTypeCheckedAtEntry(const Program& program, const BuildContext& build, FunctionID id, const Function& function, bool hasBody)
{
    bool isApplicationOrLibraryCode = function.role == FunctionRole::Source || function.role == FunctionRole::Library;
    if (!hasBody || !isApplicationOrLibraryCode || !build.isEnabled(PassID::CheckTypesAtEntry, findFunctionAsRead(&program, id)))
        return false;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        const DeclaredType* declared = findTypeCheckedAtEntry(program.types, function, index);
        if (declared && declared->site)
            return true;
    }
    return false;
}

static DeclaredType withExpectedType(const DeclaredType& declared)
{
    DeclaredType expected = declared;
    expected.flags.add(PositionFlags::IsExpected);
    expected.flags.remove(PositionFlags::IsInt32Promise);
    return expected;
}

static BlockID findFirstBlockWithValues(const Function& function)
{
    BlockID block = entryBlock;
    for (size_t steps = 0; steps < function.blocks.size(); ++steps) {
        const Block& current = function.at(block);
        const Terminator& terminator = current.terminator;
        if (!current.values.isEmpty() || current.handler || terminator.kind != TerminatorKind::Jump || !terminator.targets[0].arguments.isEmpty())
            return block;
        block = terminator.targets[0].block;
    }
    return entryBlock;
}

static ProgramPoint findPointBehindOwnAllocation(const Function& function)
{
    ProgramPoint head = ProgramPoint::atHeadOf(entryBlock);
    std::optional<unsigned> newTargetIndex = function.newTargetParameterIndex();
    if (function.kind != FunctionKind::BaseConstructor || !newTargetIndex)
        return head;
    BlockID block = findFirstBlockWithValues(function);
    const BlockValues& values = function.at(block).values;
    if (values.size() < 2)
        return head;
    const Value& prototype = function.at(values[0]);
    if (prototype.opcode != Opcode::GetConstructPrototype || prototype.newTargetOperand() != function.at(entryBlock).parameters[*newTargetIndex])
        return head;
    const Value& allocation = function.at(values[1]);
    if (allocation.opcode != Opcode::NewObject || allocation.numberOfOperands() != 1 || allocation.operand(0) != values[0])
        return head;
    return { block, 2 };
}

static Vector<unsigned, 8> findCheckedParameters(const Program& program, const Function& function)
{
    Vector<unsigned, 8> checkedParameters;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (findTypeCheckedAtEntry(program.types, function, index))
            checkedParameters.append(index);
    }
    return checkedParameters;
}

Vector<unsigned, 8> findParametersCheckedAtEntry(const Program& program, const BuildContext& build, FunctionID id, const Function& record, bool hasBody)
{
    if (!hasTypeCheckedAtEntry(program, build, id, record, hasBody))
        return { };
    return findCheckedParameters(program, record);
}

void appendChecksAtEntry(const Program& program, BuildContext& build, Function& function, std::span<const unsigned> checkedParameters)
{
    ProgramPoint point = findPointBehindOwnAllocation(function);
    BlockValues checks;
    function.editAllValues().reserveCapacity(function.values.size() + checkedParameters.size());
    for (unsigned index : checkedParameters) {
        DeclaredType& declared = *function.declaredEntryParameter(index);
        Value convert = IRBuilder::make(Opcode::Convert, { function.at(entryBlock).parameters[index] });
        convert.setDeclared(DeclaredType { .type = declared.type, .site = declared.site });
        convert.setIllTypedCause(IllTypedCause::UnseenCaller);
        convert.setOrigin({ declared.site ? program.at(declared.site).position : function.position, 0 });
        declared = withExpectedType(declared);

        checks.append(ValueID(function.values.size()));
        function.add(WTF::move(convert));
    }
    function.edit(point.block, BlockEdit::Values).values.insertVector(point.index, checks);
    build.statistics().count(StatisticID::TypesCheckedAtEntry, checkedParameters.size());
}

static void addChecksAtEntry(const Program& program, BuildContext& build, Function& function)
{
    Vector<unsigned, 8> checkedParameters = findCheckedParameters(program, function);

    // Every use of a checked parameter becomes a use of its check. The parameters of the entry have the lowest numbers, so the table is short.
    Vector<ValueID, 16> checkOfValue;
    uint32_t numberOfValues = function.values.size();
    for (unsigned position = 0; position < checkedParameters.size(); ++position) {
        ValueID parameter = function.at(entryBlock).parameters[checkedParameters[position]];
        if (checkOfValue.size() <= parameter.index())
            checkOfValue.grow(parameter.index() + 1);
        checkOfValue[parameter.index()] = ValueID(numberOfValues + position);
    }
    auto rewrite = [&](ValueID& value) {
        if (value.index() < checkOfValue.size() && checkOfValue[value.index()])
            value = checkOfValue[value.index()];
    };
    for (Value& value : function.editAllValues())
        value.forEachChild(rewrite);
    for (Block& block : function.editAllBlocks())
        block.forEachChildOutsideValues(rewrite);
    appendChecksAtEntry(program, build, function, checkedParameters.span());
}

void checkTypesAtEntry(ProgramEditor& editor, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> functions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return hasTypeCheckedAtEntry(program, build, FunctionID(index), program.functions[index], program.functions[index].hasBody());
    });
    forEachIndexInParallel(loops, functions.size(), [&](unsigned position) {
        addChecksAtEntry(program, build, editor.at(FunctionID(functions[position])));
    });
    build.statistics().count(StatisticID::FunctionsWithTypesCheckedAtEntry, functions.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
