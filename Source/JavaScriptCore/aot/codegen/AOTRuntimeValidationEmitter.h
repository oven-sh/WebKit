/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameAllocationPointers.h"
#include "AOTOperationEmitter.h"
#include "AOTRuntimeValidation.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct FieldAccess;

class RuntimeValidationEmitter final : public OperationEmitter {
public:
    explicit RuntimeValidationEmitter(CodeGenerator&);

    struct OperandCell {
        ValueID operation;
        ValueID operand;
        Pointer address;
        Integer header;
        Integer wordBehindHeader;
    };
    using OperandCells = Vector<OperandCell, 2>;

    void validateParameters(BlockID, const Block&);
    void validateOperands(const OperationContext&);
    void validateResult(const ValueContext&);
    OperandCells captureOperandCells(const OperationContext&);
    void checkCellsWereNotConverted(const OperandCells& beforeCall);
    // A call that runs no other code is judged by what it does to the operands of its operation: each is photographed in front of the call and compared behind it.
    using PhotographedOperands = Vector<ValueID, 4>;
    PhotographedOperands photographOperands(const OperationContext&, const Effects& effectsOfCall);
    void compareWithPhotographs(const OperationContext&, const PhotographedOperands&, uint32_t callee);
    // Around a call that collects nothing by what its operation is proven to do, while its callee may collect where less is known.
    Integer countStopsForCollector();
    void checkCollectorDidNotStop(const OperationContext&, Integer stopsBeforeCall, CalledCode::Kind, uint32_t callee);
    void noteFailedGuard(Opcode, uint32_t ordinal);
    void validateExit(const TerminatorContext&);

    void generateValidateType(const ValueContext&);
    void generateValidateCallee(const ValueContext&);
    void generateValidateLookup(const ValueContext&);
    void generateValidateFirstRun(const ValueContext&);
    void generateValidateSameValue(const ValueContext&);
    void generateShouldFailPrediction(const ValueContext&);
    void reportUnreachable(ValueID);
    void checkSlotAccessWithoutGuard(const OperationContext&);
    void checkAssumedType(const ValueContext&);

    void checkPrecondition(const ValueContext&, Condition);
    void checkBindingIsStored(const ValueContext&, Condition);
    // A short form of a test answers as the whole test does.
    void checkTypeTestsAgree(Condition, CellPointer tested);
    void checkGuardIsAvailable(const OperationContext&, ValueID guard, uint32_t operandIndex);
    void checkIsNotFrameCell(const OperationContext&, Operand, std::optional<Operand> retainer);
    void checkIsNotReleased(const OperationContext&, CellPointer environment, LayoutID);
    void poisonFrameCells(ProgramPoint);
    void poisonEnvironment(CellPointer, LayoutID);

private:
    template<typename User> void validateOperandsOf(const OperationContext&, const User&, ValueID ownedValue);
    void validateSlotAccess(const OperationContext&, const Value&);
    void checkRetainedOperand(const OperationContext&, const Value& user, ValueID operand, uint32_t operandIndex);
    void checkRetainedOperand(const OperationContext&, const Terminator& user, ValueID operand, uint32_t operandIndex);
    void checkIsNoFrameArray(const OperationContext&, const Value& user, ValueID operand, uint32_t operandIndex);
    void checkIsNoFrameArray(const OperationContext&, const Terminator& user, ValueID operand, uint32_t operandIndex);

    void failUnless(Condition, ValidationFailureKind, Operand found, ValueID);
    void askOperationUnless(Condition, ValidationFailureKind, Operand found, ValueID);
    void reportFailure(ValidationFailureKind, Operand found, ValueID);
    void callReport(ValidationFailureKind, Operand found, ValueID);
    Boxed reportedValueOf(Operand found);

    void checkValue(ValueID, Operand);
    void checkIsSpecified(ValueID);
    void checkHasType(ValueID, Operand, const Type&, ValidationFailureKind);
    Condition hasShapeAmong(CellPointer, LayoutSetID, std::span<const uint32_t> ascendingShapes);
    void checkReceiverOfSlotAccess(const OperationContext&, const Value&);
    void checkShapeBehindGuard(const OperationContext&, const Value&);
    void checkRawNumberSlot(const OperationContext&, CellPointer, const FieldAccess&);
    void checkClassProofs(ValueID, Operand, const Type&);
    void checkClassProofs(ValueID, CellPointer, const ClassPart&);
    bool hasTestableBits(ValueID);
    Pointer addressOfObjectOrOfCode(Operand);
    Integer headerAt(Pointer);
    Integer wordBehindHeaderAt(Pointer);
    void checkEnvironmentShape(CellPointer, LayoutID, ValueID);
    Type testableTypeOf(const Type&);
    void poisonFields(CellPointer, LayoutID, AbstractHeap);
    void poisonFrameArray(CellPointer);
    bool isSuspensionPoint(ValueID);
    bool maySuspendBetween(ValueID allocation, ProgramPoint);
    Condition isInStack(Pointer);
    Pointer stackPointer();

    FramePointers m_framePointers;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
