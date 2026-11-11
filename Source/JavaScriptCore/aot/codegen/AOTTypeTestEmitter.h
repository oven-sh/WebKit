/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTTypeTestPlan.h"
#include <array>
#include <memory>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct TypeTestPlansKeptAcrossUnits {
    using Plans = UncheckedKeyHashMap<uint64_t, std::unique_ptr<TypeTestPlan>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    static constexpr unsigned mostPlansOfRepresentation = 512;

    const Plan* plan { nullptr };
    const TypeTable* table { nullptr };
    std::array<Plans, numberOfEnumerators<Representation>> plans;
    KindsInLayoutSets kindsInSets;
};

class TypeTestEmitter final : public OperationEmitter {
public:
    explicit TypeTestEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    const TypeTestPlan& planFor(TypeID from, Representation, TypeID to);

    void emitBranch(Operand, const TypeTestPlan&, BranchTargets);
    void emitPerFunctionBranch(Operand, const TypeTestPlan&, BranchTargets);
    Condition emitBoolean(Operand, const TypeTestPlan&);

    Condition passesCellTest(CellPointer, const CellTest&, const Type& from, MarkedObjects = MarkedObjects::Pass);
    Condition isCallable(CellPointer);
    Condition inheritsClassInfo(CellPointer, BuiltinClassID);
    Condition isPristine(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition nothingWasWrittenAbove(BuiltinClassID);
    Condition isAsItsClassMadeIt(CellPointer, BuiltinClassID, OptionSet<PristineAspects>);
    Condition hasSharingMode(CellPointer, ArrayBufferSharingMode);
    Condition isNamespaceOf(CellPointer, ModuleID);
    Condition isIntegerIn(Number, Range);
    Condition isInRange(Integer, Range);

private:
    using PlanMap = UncheckedKeyHashMap<uint64_t, std::unique_ptr<TypeTestPlan>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    void leaveUnless(Condition, BlockEdge failure);
    void leave(BlockEdge failure);
    void jump(BlockEdge);

    std::optional<Condition> tryGetSingleCondition(Operand, const TypeTestPlan&);
    std::optional<Condition> tryGetSingleCondition(Boxed, const TypeTestPlan&);
    std::optional<Condition> tryGetSingleCondition(CellPointer, const TypeTestPlan&);
    std::optional<Condition> tryGetNonCellCondition(Boxed, const TypeTestPlan&);
    std::optional<Condition> tryGetConditionWithAbsent(Boxed, const TypeTestPlan&);

    void forEachNonCellCase(Boxed, const TypeTestPlan&, const auto& visit);
    void forEachCellCase(CellPointer, const TypeTestPlan&, const auto& visit);
    void testCases(BlockEdge failure, const auto& forEachCase);

    void test(Operand, const TypeTestPlan&, BlockEdge failure);
    void testBoxed(Boxed, const TypeTestPlan&, BlockEdge failure);
    void testNonCell(Boxed, const TypeTestPlan&, BlockEdge failure);
    enum class BoxedValue : uint8_t { IsNumber, MayBeNoNumber };
    void testBoxedNumber(Boxed, BoxedValue, const TypeTestPlan&, BlockEdge failure);
    void testCell(CellPointer, const TypeTestPlan&, BlockEdge failure);
    void testWithMarkers(Operand, const TypeTestPlan&, BlockEdge failure, NOESCAPE const Invocable<void()> auto& testPayload);

    Condition isMarker(Operand);
    Condition isMarker(Operand, uint16_t primitive);
    Condition passesByMarker(Operand, const TypeTestPlan&, bool payloadPasses);
    Condition doublePasses(Number, const TypeTestPlan&);
    Condition integerPasses(Integer, const TypeTestPlan&, Range heldByForm = Range::full());
    Condition isExactIntegerIn(Number, Integer truncated, Range);
    Condition cellPasses(CellPointer, const TypeTestPlan&);
    Condition stringPasses(CellPointer, const TypeTestPlan&);
    Condition isCallableByCallData(CellPointer);
    std::optional<BuiltinID> onlyBuiltinThatPasses(const TypeTestPlan&) const;

    TypeTestPlansKeptAcrossUnits& whatIsKeptAcrossUnits();
    TypeTestPlansKeptAcrossUnits::Plans& plansKeptAcrossUnits(Representation);

    TypeTestPlansKeptAcrossUnits* m_plansKeptAcrossUnits { nullptr };
    std::array<PlanMap, numberOfEnumerators<Representation>> m_plans;
    bool m_casesAreBranchedOn { true };
    bool m_nextCaseIsUnlikely { false };
    bool m_isEmittingPerFunctionBranch { false };
};

void TypeTestEmitter::testCases(BlockEdge failure, const auto& forEachCase)
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
        Condition passes = emitTest();
        m_out.branch(passes.value(), normalEdge(continuation), std::exchange(m_nextCaseIsUnlikely, false) ? unlikelyEdge(nextCase) : normalEdge(nextCase));
        m_out.appendTo(nextCase);
    });
    if (continuation) {
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
