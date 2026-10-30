/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTFunctionData.h"
#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class ConstantEmitter final : public OperationEmitter {
public:
    explicit ConstantEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateUndefined(const ValueContext& value) { define(value, constantOf(jsUndefined(), value.representation())); }
    void generateNull(const ValueContext& value) { define(value, constantOf(jsNull(), value.representation())); }
    void generateAbsent(const ValueContext& value) { define(value, constantOf(JSValue(), value.representation())); }
    void generateBoolean(const ValueContext& value) { define(value, constantOf(jsBoolean(value->boolean()), value.representation())); }
    void generateInteger(const ValueContext&);
    void generateDouble(const ValueContext& value) { define(value, constantOf(jsNumber(withoutPayload(value->number())), value.representation())); }
    void generateString(const ValueContext&);
    void generateBigInt(const ValueContext& value) { defineCell(value, bigIntConstant(value, value->digits())); }
    void generateBuiltin(const ValueContext&);
    void generateModuleBuiltin(const ValueContext&);
    void generateData(const ValueContext&);
    void generateNewClosure(const ValueContext&);
    void generateKeyConstant(const ValueContext&);
    void generateUnspecified(const ValueContext& value) { define(value, zeroOf(value.representation())); }
    void generateEmptyArgumentList(const ValueContext& value) { defineCell(value, CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.emptyArgumentList)).value())); }

    CellPointer stringConstant(const OperationContext&, StringID);
    CellPointer keyConstant(const OperationContext&, PropertyKeyID);
    CellPointer bigIntConstant(const OperationContext&, StringID digits);
    CellPointer builtinCell(const OperationContext& context, BuiltinID id) { return builtinCell(context, BuiltinCellKey::object(id)); }
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    CellPointer singletonClosure(const OperationContext&, FunctionID);
    // Creates the closure that a slot of the object deferred and stores it there. Null if the stub for closures without a function object does not create this function's closures.
    std::optional<CellPointer> tryMakeClosureOfField(const OperationContext&, FunctionID, CellPointer object, uint32_t offsetOfSlot, bool capturesObject);
    // For a type test, which has no context. Placement has numbered the cell.
    CellPointer keyConstant(PropertyKeyID);
    CellPointer builtinCell(BuiltinID);
    void branchOnIsBuiltinCell(LValue boxedValueOrCell, BuiltinID, BranchTargets);
    Operand constantOf(JSValue, Representation);
    Operand zeroOf(Representation);
    // The cell in a word that, once filled, stays as it is.
    CellPointer permanentCell(TypedPointer cellWord, bool isNarrow, NOESCAPE const Invocable<CellPointer()> auto& create);

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
    CellPointer cellMadeByStub(const OperationContext&, StubKey, CallOptions, bool isNarrow, NOESCAPE const Invocable<TypedPointer()> auto& cellWord);
    CellPointer newCopyOnWriteArray(const ValueContext&, Request);

    CellPointer closureCreatedBy(const ValueContext&);
    bool canUseClosureStub(const NewClosureRequest&) const;
    CellPointer newFullClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newSlimClosure(const ValueContext&, const NewClosureRequest&);
    CellPointer newClosureByStub(const ValueContext&, const NewClosureRequest&);
    CellPointer callClosureStub(const ValueContext&, const NewClosureRequest&, uint64_t requestWord, size_t cellSize);
    Operand captureOf(const NewClosureRequest&, unsigned index) const;
    Vector<LValue, 4> captureWordsBySlot(const NewClosureRequest&);
    void storeCaptures(CellPointer closure, const NewClosureRequest&, B3::IndexedAbstractHeap&);
    Integer closureWordOf(FunctionID entrypoint);
};

CellPointer ConstantEmitter::permanentCell(TypedPointer cellWord, bool isNarrow, NOESCAPE const Invocable<CellPointer()> auto& create)
{
    if (isNarrow) {
        NarrowReference reference(m_out.load32(cellWord));
        CellPointer cell = decode(reference);
        return either<CellPointer>(Condition(m_out.notZero32(reference.value())), [&] {
            return cell;
        }, create);
    }
    CellPointer cell(m_out.load64(cellWord));
    return either<CellPointer>(Condition(m_out.notZero64(cell.value())), [&] {
        return cell;
    }, create);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
