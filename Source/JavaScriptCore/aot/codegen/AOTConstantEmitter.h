/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTFunctionData.h"
#include "AOTOperationEmitter.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class ConstantEmitter final : public OperationEmitter {
public:
    explicit ConstantEmitter(CodeGenerator&);

    void generateUndefined(const ValueContext&);
    void generateNull(const ValueContext&);
    void generateAbsent(const ValueContext&);
    void generateBoolean(const ValueContext&);
    void generateInteger(const ValueContext&);
    void generateDouble(const ValueContext&);
    void generateString(const ValueContext&);
    void generateBigInt(const ValueContext&);
    void generateBuiltin(const ValueContext&);
    void generateModuleBuiltin(const ValueContext&);
    void generateData(const ValueContext&);
    void generateNewClosure(const ValueContext&);
    void generateKeyConstant(const ValueContext&);
    void generateUnspecified(const ValueContext&);
    void generateEmptyArgumentList(const ValueContext&);

    CellPointer stringConstant(const OperationContext&, StringID);
    CellPointer keyConstant(const OperationContext&, PropertyKeyID);
    CellPointer bigIntConstant(const OperationContext&, StringID digits);
    CellPointer builtinCell(const OperationContext&, BuiltinID);
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    CellPointer moduleBuiltinCell(const OperationContext&, BuiltinID, ModuleID);
    CellPointer singletonClosure(const OperationContext&, FunctionID);
    // For a type test, which has no context. Placement has numbered the cell.
    CellPointer keyConstant(PropertyKeyID);
    CellPointer builtinCell(BuiltinID);
    void branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID, BranchTargets);
    Operand constantOf(JSValue, Representation);
    Operand zeroOf(Representation);

private:
    struct NewClosureRequest {
        FunctionID function;
        FunctionID entrypoint;
        FunctionKindData kind;
        AllocationID allocation;
        AllocationStorage storage;
        ClosureForm form;
        std::span<const ValueID> captures;
    };

    void defineCell(const ValueContext&, CellPointer);
    void failAndDefineZero(const ValueContext&, InternalErrorKind);
    CellPointer failWithoutCell(const OperationContext&, InternalErrorKind);

    TypedPointer cellWordInTable(const B3::AbstractHeap&, ptrdiff_t offsetOfTable, uint32_t number);
    TypedPointer builtinCellWord(uint32_t number);
    template<typename Create> CellPointer permanentCell(TypedPointer cellWord, bool isNarrow, const Create&);
    CellPointer newCopyOnWriteArray(const ValueContext&, Request);

    bool canUseClosureStub(const NewClosureRequest&) const;
    CellPointer newFullClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newSlimClosure(const ValueContext&, const NewClosureRequest&);
    void initializeSlimClosure(CellPointer, const NewClosureRequest&);
    CellPointer newClosureByStub(const ValueContext&, const NewClosureRequest&);
    CellPointer callClosureStub(const ValueContext&, const NewClosureRequest&, uint64_t requestWord, size_t cellSize);
    Operand captureOf(const NewClosureRequest&, unsigned index) const;
    Vector<LValue, 4> captureWordsBySlot(const NewClosureRequest&);
    void storeCaptures(CellPointer closure, const NewClosureRequest&, B3::IndexedAbstractHeap&);
    Integer closureWordOf(FunctionID entrypoint);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
