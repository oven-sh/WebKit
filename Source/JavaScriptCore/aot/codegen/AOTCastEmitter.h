/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureTestEmitter.h"
#include "AOTOperationEmitter.h"
#include "AOTStubKey.h"

namespace JSC { namespace AOT { namespace Typed {

class CastEmitter final : public OperationEmitter {
public:
    explicit CastEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateNarrowed(const ValueContext&);
    void generateCheckInitialized(const ValueContext&);
    void generateRequireObjectCoercible(const ValueContext&);
    void requireObjectCoercible(const ValueContext&);
    void generateRequireObject(const ValueContext&);
    void generateTypeTest(const ValueContext&);
    const Operation* throwAfterFailedTest(const ValueContext& test);
    bool tryBranchOnTypeOfCell(const ValueContext& test, Operand);
    void generateIsUnlockedInstance(const ValueContext&);
    void generateCheckProof(const ValueContext&);
    TypeID typeOfCellsToCheck(const ValueContext&);
    void checkBooleanOrBranch(const ValueContext& test, ValueID check);
    struct QuestionForStub {
        StubKey stub;
        uint32_t kinds;
        std::optional<uint32_t> mentionAboveKinds;
        std::optional<uint32_t> numberInRegister;
    };
    std::optional<QuestionForStub> questionForStubThatTells(TypeID);
    std::optional<Operand> takeIntegerHandedOnTo(ValueID unboxing);
    struct DeferredNumberCheck {
        Boxed value;
        BlockEdge failing;
    };
    Vector<DeferredNumberCheck, 2> takeNumberChecksOf(ValueID sum);

private:
    const TypeTestPlan& planFor(ValueID operand, TypeID to) { return typeTestPlanFor(typeIDOf(operand), representationOf(operand), to); }
    bool asksForRangeOfInt32(const ValueContext& test);
    ValueID unboxingBehindTestForInt32(const ValueContext& test);
    bool tryBranchOnInt32AndHandItOn(const ValueContext& test, Operand);
    ValueID unboxingToDoubleBehindTestForNumber(const ValueContext& test);
    bool tryBranchOnNumberAndHandItOn(const ValueContext& test, Operand);
    ValueID conversionAfterInt32Test(const ValueContext& test);
    bool tryBranchOnTruncatedInt32(const ValueContext& test, Operand);
    ValueID integerAfterDoubleTest(const ValueContext& test);
    bool tryBranchOnWholeDouble(const ValueContext& test, Operand);
    ValueID sumAfterNumberTest(const ValueContext& test);
    bool tryDeferTestToSum(const ValueContext& test, Operand);
    bool tryTestSlotWord(const ValueContext& test, Boxed, const TypeTestPlan&);
    FunctionID singleTestedClosure(ValueID test);
    bool tryBeginClosureChain(const ValueContext& test);
    bool tryContinueClosureChain(const ValueContext& test);

    struct ClosureChain {
        CellPointer closure;
        Word32 word;
        Vector<ClosureTestEmitter::ClosureCase, 4> cases;
    };
    struct ClosureChainPosition {
        uint32_t chain;
        uint32_t place;
    };
    Vector<ClosureChain> m_closureChains;
    UncheckedKeyHashMap<uint32_t, ClosureChainPosition, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_closureChainPositions;

    UncheckedKeyHashMap<uint32_t, Operand, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_integersHandedOn;
    UncheckedKeyHashMap<uint32_t, Vector<DeferredNumberCheck, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numberChecksOfSums;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
