/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameAllocationPointers.h"
#include "AOTOperationEmitter.h"
#include "AOTRuntimeValidation.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class RuntimeValidationEmitter final : public OperationEmitter {
public:
    explicit RuntimeValidationEmitter(CodeGenerator&);

    void validateParameters(BlockID, const Block&);
    void validateOperands(const OperationContext&);
    void validateResult(const ValueContext&);
    void validateExit(const TerminatorContext&);

    void generateValidateType(const ValueContext&);
    void generateValidateCallee(const ValueContext&);
    void generateValidateFirstRun(const ValueContext&);
    void generateValidateSameValue(const ValueContext&);
    void reportUnreachable(ValueID);

    void checkValue(const OperationContext&, ValueID, Operand);
    void checkPrecondition(const ValueContext&, Condition);
    void checkBindingIsStored(const ValueContext&, Condition);
    void checkGuardIsAvailable(const OperationContext&, ValueID guard, uint32_t operandIndex);
    void checkIsNotFrameCell(const OperationContext&, Operand, std::optional<Operand> retainer);
    void checkIsNotReleased(const OperationContext&, CellPointer environment, LayoutID);
    void poisonFrameCells(ProgramPoint);
    void poisonEnvironment(CellPointer, LayoutID);

private:
    template<typename User> void validateOperandsOf(const OperationContext&, const User&, ValueID ownedValue);
    void checkRetainedOperand(const OperationContext&, const Value& user, ValueID operand, uint32_t operandIndex);
    void checkRetainedOperand(const OperationContext&, const Terminator& user, ValueID operand, uint32_t operandIndex);

    void failUnless(Condition, ValidationFailureKind, Operand found, ValueID);
    void askOperationUnless(Condition, ValidationFailureKind, Operand found, ValueID);
    void reportFailure(ValidationFailureKind, Operand found, ValueID);
    void callReport(ValidationFailureKind, Operand found, ValueID);
    Boxed reportedValueOf(Operand found);

    void checkValue(ValueID, Operand);
    void checkHasType(ValueID, Operand, const Type&, ValidationFailureKind);
    void checkReceiverOfSlotAccess(const OperationContext&, const Value&);
    void checkClassProofs(ValueID, Operand, const Type&);
    void checkClassProofs(ValueID, CellPointer, const ClassPart&);
    bool hasTestableBits(ValueID);
    void unlessInStack(Operand, NOESCAPE const Invocable<void()> auto& emitTest);
    void checkEnvironmentShape(CellPointer, LayoutID, ValueID);
    Type testableTypeOf(const Type&);
    void poisonFields(CellPointer, LayoutID, AbstractHeap);
    bool isSuspensionPoint(ValueID);
    bool maySuspendBetween(ValueID allocation, ProgramPoint);
    Condition isInStack(Pointer);
    Pointer stackPointer();

    FramePointers m_framePointers;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
