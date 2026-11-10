/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStubKey.h"

namespace JSC { namespace AOT { namespace Typed {

class CastEmitter final : public OperationEmitter {
public:
    explicit CastEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateNarrowed(const ValueContext& value) { defineNarrowed(value, get(value->valueOperand())); }
    void generateCheckInitialized(const ValueContext&);
    void generateRequireObjectCoercible(const ValueContext&);
    void requireObjectCoercible(const ValueContext&);
    void generateRequireObject(const ValueContext&);
    void generateTypeTest(const ValueContext&);
    void generateCheckProof(const ValueContext&);
    struct QuestionForStub {
        StubKey stub;
        uint32_t kinds;
        std::optional<uint32_t> mentionAboveKinds;
        std::optional<uint32_t> numberInRegister;
    };
    std::optional<QuestionForStub> questionForStubThatTells(TypeID);
    std::optional<Operand> takeIntegerHandedOnTo(ValueID unboxing);

private:
    const TypeTestPlan& planFor(ValueID operand, TypeID to) { return typeTestPlanFor(typeIDOf(operand), representationOf(operand), to); }
    ValueID unboxingBehindTestForInt32(const ValueContext& test);
    bool tryBranchOnInt32AndHandItOn(const ValueContext& test, Operand);
    ValueID unboxingToDoubleBehindTestForNumber(const ValueContext& test);
    bool tryBranchOnNumberAndHandItOn(const ValueContext& test, Operand);

    UncheckedKeyHashMap<uint32_t, Operand, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_integersHandedOn;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
