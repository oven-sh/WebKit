/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTOperationEmitter.h"
#include "AOTTypeTestPlan.h"
#include <array>
#include <memory>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeTestEmitter final : public OperationEmitter {
public:
    explicit TypeTestEmitter(CodeGenerator&);

    const TypeTestPlan& planFor(TypeID from, Representation, TypeID to);

    void emitBranch(Operand, const TypeTestPlan&, BranchTargets);
    Condition emitBoolean(Operand, const TypeTestPlan&);
    void emitThrow(const OperationContext&, Operand, const TypeTestPlan&, TypeSiteID);

    Condition passesCellTest(CellPointer, const CellTest&, const Type& from);
    Condition isCallable(CellPointer);
    Condition inheritsClassInfo(CellPointer, BuiltinClassID);
    Condition isPristine(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition hasSharingMode(CellPointer, ArrayBufferSharingMode);
    Condition isNamespaceOf(CellPointer, ModuleID);
    Condition isIntegerIn(Number, Range);

private:
    struct Failure {
        const OperationContext* thrower { nullptr };
        ThrowSite throwSite { ThrowKind::TypeMismatch, 0, std::nullopt };
        Operand thrown;
        B3::FrequentedBlock target;
    };
    using PlanMap = UncheckedKeyHashMap<uint64_t, std::unique_ptr<TypeTestPlan>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    void leaveUnless(Condition, const Failure&);
    void leave(const Failure&);
    void jump(B3::FrequentedBlock);

    std::optional<Condition> tryGetSingleCondition(Operand, const TypeTestPlan&);
    std::optional<Condition> tryGetSingleCondition(Boxed, const TypeTestPlan&);
    std::optional<Condition> tryGetSingleCondition(CellPointer, const TypeTestPlan&);
    std::optional<Condition> tryGetNonCellCondition(Boxed, const TypeTestPlan&);
    std::optional<Condition> tryGetConditionWithAbsent(Boxed, const TypeTestPlan&);

    // A value passes when one of its cases does. visit takes a function that emits the test of one case, likeliest case first.
    void forEachNonCellCase(Boxed, const TypeTestPlan&, const auto& visit);
    void forEachCellCase(CellPointer, const TypeTestPlan&, const auto& visit);
    void testCases(const Failure&, const auto& forEachCase);

    void test(Operand, const TypeTestPlan&, const Failure&);
    void testBoxed(Boxed, const TypeTestPlan&, const Failure&);
    void testNonCell(Boxed, const TypeTestPlan&, const Failure&);
    void testBoxedNumber(Boxed number, const TypeTestPlan&, const Failure&);
    void testCell(CellPointer, const TypeTestPlan&, const Failure&);
    void testNarrowReference(NarrowReference, const TypeTestPlan&, const Failure&);
    void testNumber(Number, const TypeTestPlan&, const Failure&);
    void testInteger(Integer, const TypeTestPlan&, const Failure&);
    void testWord32(Word32, const TypeTestPlan&, const Failure&);
    void testCondition(Condition, const TypeTestPlan&, const Failure&);
    void testOptionalNumber(OptionalNumber, const TypeTestPlan&, const Failure&);
    void testOptionalInteger(OptionalInteger, const TypeTestPlan&, const Failure&);
    void testLiteralString(LiteralStringWord, const TypeTestPlan&, const Failure&);
    void testWithMarkers(Operand, const TypeTestPlan&, const Failure&, NOESCAPE const Invocable<void()> auto& testPayload);

    Condition isMarker(Operand);
    Condition isMarker(Operand, uint16_t primitive);
    Condition passesByMarker(Operand, const TypeTestPlan&, bool payloadPasses);
    Condition nonCellPasses(Boxed, const TypeTestPlan&);
    Condition boxedNumberPasses(Boxed, const TypeTestPlan&);
    Condition doublePasses(Number, const TypeTestPlan&);
    Condition integerPasses(Integer, const TypeTestPlan&);
    Condition booleanPasses(Condition, const TypeTestPlan&);
    Condition isInRange(Integer, Range);
    Condition isExactIntegerIn(Number, Integer truncated, Range);
    Condition cellPasses(CellPointer, const TypeTestPlan&);
    Condition stringPasses(CellPointer, const TypeTestPlan&);
    Condition isCallableByCallData(CellPointer);
    Condition isOneOfStringLiterals(CellPointer string, const TypeTestPlan&);
    Condition isOneOfLiteralStringWords(LiteralStringWord, const TypeTestPlan&);

    std::array<PlanMap, numberOfEnumerators<Representation>> m_plans;
};

void TypeTestEmitter::testCases(const Failure& failure, const auto& forEachCase)
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
