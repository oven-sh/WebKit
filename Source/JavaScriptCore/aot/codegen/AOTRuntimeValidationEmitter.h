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
        std::optional<Integer> header;
        Integer wordBehindHeader;
    };
    using OperandCells = Vector<OperandCell, 2>;

    void validateParameters(BlockID, const Block&);
    void validateOperands(const OperationContext&);
    void validateResult(const ValueContext&);
    // Objects: the header and the following word of every operand that is an object. Arrays: the word after the header of every operand that is an array.
    enum class ComparedCells : uint8_t { Objects, Arrays };
    OperandCells captureOperandCells(const OperationContext&, ComparedCells);
    void checkCellsWereNotConverted(const OperandCells& beforeCall);
    // A call that runs no other code is judged by what it does to the operands of its operation: each is photographed in front of the call and compared behind it.
    using PhotographedOperands = Vector<ValueID, 4>;
    PhotographedOperands photographOperands(const OperationContext&, const Effects& effectsOfCall, ValueID alsoPerformed = { });
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
    void reportUnreachable(ValueID, PassID cutBy = PassID::None);
    void checkSlotAccessWithoutGuard(const OperationContext&);
    void forgetPassedTypeTests() { m_passedTypeTests.shrink(0); }
    void forgetPassedTypeTestsBehind(const OperationContext&);
    void runOutOfMemoryWhenCounted(const ValueContext&);
    void checkAssumedType(const ValueContext&);

    void checkPrecondition(const ValueContext&, Condition);
    // For code that several operations share: the report names the operation that is being generated.
    void checkClaim(std::optional<CellPointer> about, Condition);
    void checkIsCompiledObjectOf(CellPointer, LayoutSetID);
    void checkBindingIsStored(const ValueContext&, Condition);
    // A short form of a test answers as the whole test does.
    void checkTypeTestsAgree(Condition, CellPointer tested);
    void checkGuardIsAvailable(const OperationContext&, ValueID guard, uint32_t operandIndex);
    // Whether an operation may receive an array kept in a frame and whether it may receive any other frame cell are separate questions. The operand determines at run time which one applies.
    enum class FrameCellFilter : uint8_t { All, Arrays, AllButArrays };
    void checkIsNotFrameCell(const OperationContext&, Operand, std::optional<Operand> retainer, FrameCellFilter = FrameCellFilter::All);
    void checkIsNotReleased(const OperationContext&, CellPointer environment, LayoutID);
    void poisonFrameCells(ProgramPoint);
    void poisonEnvironment(CellPointer, LayoutID);

private:
    template<typename User> void validateOperandsOf(const OperationContext&, const User&, ValueID ownedValue);
    void inheritCheckedClassProofs(BlockID);
    void validateSlotAccess(const OperationContext&, const Value&);
    bool checkRetainedOperand(const OperationContext&, const Value& user, ValueID operand, uint32_t operandIndex, bool refusesFrameArray);
    bool checkRetainedOperand(const OperationContext&, const Terminator& user, ValueID operand, uint32_t operandIndex, bool refusesFrameArray);
    bool refusesFrameArray(const Value& user, ValueID operand, uint32_t operandIndex);
    bool refusesFrameArray(const Terminator& user, ValueID operand, uint32_t operandIndex);
    static constexpr FrameCellFilter filterOfRefusedFrameArray(const Value&) { return FrameCellFilter::Arrays; }
    static constexpr FrameCellFilter filterOfRefusedFrameArray(const Terminator&) { return FrameCellFilter::All; }

    // A test of the program may refuse a changed closure. What is validated is asked what it is.
    Condition hasType(Operand, const TypeTestPlan&);
    void branchOnType(Operand, const TypeTestPlan&, BranchTargets);
    void failUnless(Condition, ValidationFailureKind, Operand found, ValueID);
    void failUnlessHasType(Operand, const TypeTestPlan&, ValidationFailureKind, ValueID, TypeID typeThatRunTimeTests);
    void askOperationUnless(Condition, std::span<const ValidationFailureKind>, Operand found, ValueID);
    void reportFailure(ValidationFailureKind, Operand found, ValueID);
    void callReport(ValidationFailureKind, Operand found, ValueID);
    Word32 reportedFailure(ValidationFailureKind, Operand found);
    Boxed reportedValueOf(Operand found);

    bool mayBeEmptyWhereMade(const Value& definition);
    void checkValue(ValueID, Operand);
    void checkIsSpecified(ValueID);
    void checkNumberIsSpecified(ValueID);
    void checkHasType(ValueID, Operand, TypeID, ValidationFailureKind, ValueID tested);
    void checkHasType(ValueID, Operand, const Type&, ValidationFailureKind, ValueID tested, TypeID numberOfType = { });
    void checkLiteralStringWord(ValueID, LiteralStringWord);
    Condition hasShapeAmong(CellPointer, LayoutSetID, std::span<const uint32_t> ascendingShapes);
    void checkReceiverOfSlotAccess(const OperationContext&, const Value&);
    void checkShapeBehindGuard(const OperationContext&, const Value&);
    void checkRawNumberSlot(const OperationContext&, CellPointer, const FieldAccess&);
    void checkClassProofs(ValueID, Operand, const Type&);
    void checkClassProofs(ValueID, CellPointer, const ClassPart&);
    bool hasTestableBits(ValueID);
    Pointer addressOfObjectOrOfCode(Operand, ComparedCells);
    Integer headerAt(Pointer);
    Integer wordBehindHeaderAt(Pointer);
    void checkEnvironmentShape(CellPointer, LayoutID, ValueID);
    Type testableTypeOf(const Type&);
    void poisonFields(CellPointer, LayoutID, AbstractHeap);
    void poisonFrameArray(CellPointer);
    bool isSuspensionPoint(ValueID);
    bool maySuspendBetween(ValueID allocation, ProgramPoint);
    Condition isInStack(Pointer);

    struct PassedTypeTest {
        ValueID value;
        TypeID type;
        bool mustBeThere;

        friend bool operator==(const PassedTypeTest&, const PassedTypeTest&) = default;
    };

    RefPtr<B3::StackmapGenerator> m_reportGenerator;
    FramePointers m_framePointers;
    BlockID m_blockOfCheckedClassProofs;
    Vector<ValueID, 4> m_checkedClassProofs;
    Vector<Vector<ValueID, 4>> m_checkedClassProofsAtEnd;
    // In the block of m_blockOfCheckedClassProofs, since the last operation that may have written to the heap.
    Vector<PassedTypeTest, 16> m_passedTypeTests;
    UncheckedKeyHashMap<uint64_t, Vector<BlockID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_blocksOfLastingTypeTests;
    BlockID m_blockOfAssumedType;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
