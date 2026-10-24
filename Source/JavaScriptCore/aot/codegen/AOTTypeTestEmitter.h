/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTTypeTestPlan.h"
#include <array>
#include <memory>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeTestEmitter final : public OperationEmitter {
public:
    explicit TypeTestEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    const TypeTestPlan& planFor(TypeID from, Representation, TypeID to);

    void emitBranch(Operand, const TypeTestPlan&, BranchTargets, CodeShape);
    Condition emitBoolean(Operand, const TypeTestPlan&, CodeShape);

    Condition passesCellTest(CellPointer, const CellTest&, const Type& from);
    Condition isCallable(CellPointer);
    Condition inheritsClassInfo(CellPointer, BuiltinClassID);
    Condition isPristine(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition hasSharingMode(CellPointer, ArrayBufferSharingMode);
    Condition isNamespaceOf(CellPointer, ModuleID);
    Condition isIntegerIn(Number, Range);

private:
    using PlanMap = UncheckedKeyHashMap<uint64_t, std::unique_ptr<TypeTestPlan>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    void leaveUnless(Condition, B3::FrequentedBlock failure);
    void leave(B3::FrequentedBlock failure);
    void jump(B3::FrequentedBlock);

    std::optional<Condition> tryGetSingleCondition(Operand, const TypeTestPlan&);
    std::optional<Condition> tryGetSingleCondition(Boxed, const TypeTestPlan&);
    std::optional<Condition> tryGetSingleCondition(CellPointer, const TypeTestPlan&);
    std::optional<Condition> tryGetNonCellCondition(Boxed, const TypeTestPlan&);
    std::optional<Condition> tryGetConditionWithAbsent(Boxed, const TypeTestPlan&);

    // A value passes when one of its cases does. visit takes a function that emits the test of one case, likeliest case first.
    void forEachNonCellCase(Boxed, const TypeTestPlan&, const auto& visit);
    void forEachCellCase(CellPointer, const TypeTestPlan&, const auto& visit);
    void testCases(B3::FrequentedBlock failure, const auto& forEachCase);

    void test(Operand, const TypeTestPlan&, B3::FrequentedBlock failure);
    void testBoxed(Boxed, const TypeTestPlan&, B3::FrequentedBlock failure);
    void testNonCell(Boxed, const TypeTestPlan&, B3::FrequentedBlock failure);
    enum class BoxedValue : uint8_t { IsNumber, MayBeNoNumber };
    void testBoxedNumber(Boxed, BoxedValue, const TypeTestPlan&, B3::FrequentedBlock failure);
    void testCell(CellPointer, const TypeTestPlan&, B3::FrequentedBlock failure);
    void testWithMarkers(Operand, const TypeTestPlan&, B3::FrequentedBlock failure, NOESCAPE const Invocable<void()> auto& testPayload);

    Condition isMarker(Operand);
    Condition isMarker(Operand, uint16_t primitive);
    Condition passesByMarker(Operand, const TypeTestPlan&, bool payloadPasses);
    Condition doublePasses(Number, const TypeTestPlan&);
    // heldByForm: what the form that the integer was taken out of can hold.
    Condition integerPasses(Integer, const TypeTestPlan&, Range heldByForm = Range::full());
    Condition isInRange(Integer, Range);
    Condition isExactIntegerIn(Number, Integer truncated, Range);
    Condition cellPasses(CellPointer, const TypeTestPlan&);
    Condition stringPasses(CellPointer, const TypeTestPlan&);
    Condition isCallableByCallData(CellPointer);
    bool asksStubWhetherCallable() const { return m_shape != CodeShape::Fast; }
    bool isDecidedByCallableStub(const TypeTestPlan&) const;
    std::optional<BuiltinID> onlyBuiltinThatPasses(const TypeTestPlan&) const;
    std::optional<FunctionID> functionDecidedByClosureStub(const TypeTestPlan&);
    Condition isCallableByStub(LValue cellOrOtherValue);
    Condition isOneOfLiteralStringWords(LiteralStringWord, const TypeTestPlan&);

    std::array<PlanMap, numberOfEnumerators<Representation>> m_plans;
    CodeShape m_shape { CodeShape::Fast };
};

void TypeTestEmitter::testCases(B3::FrequentedBlock failure, const auto& forEachCase)
{
    unsigned numberOfCasesLeft = 0;
    forEachCase([&](const auto&) {
        ++numberOfCasesLeft;
    });
    if (!numberOfCasesLeft) {
        leave(failure);
        return;
    }

    LBasicBlock continuation = numberOfCasesLeft > 1 ? newBlock() : nullptr;
    forEachCase([&](const auto& emitTest) {
        if (!--numberOfCasesLeft) {
            leaveUnless(emitTest(), failure);
            return;
        }
        LBasicBlock nextCase = newBlock();
        m_out.branch(emitTest().value(), normalEdge(continuation), normalEdge(nextCase));
        m_out.appendTo(nextCase);
    });
    if (continuation) {
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
