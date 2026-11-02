/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class CastEmitter final : public OperationEmitter {
public:
    explicit CastEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateUpcast(const ValueContext& value) { define(value, get(value->valueOperand())); }
    void generateExpect(const ValueContext& value) { define(value, get(value->valueOperand())); }
    void generateNarrowByField(const ValueContext& value) { define(value, get(value->valueOperand())); }
    void generateAssumeType(const ValueContext&);
    void generateReportMismatch(const ValueContext&);
    void generateCheckInitialized(const ValueContext&);
    void generateRequireObjectCoercible(const ValueContext&);
    void requireObjectCoercible(const ValueContext&);
    void generateRequireObject(const ValueContext& value) { requireType(value, TypeTable::object); }
    void generateRequireType(const ValueContext& value) { requireType(value, value->requiredType()); }
    void generateTypeTest(const ValueContext&);

private:
    const TypeTestPlan& planFor(ValueID operand, TypeID to) { return typeTestPlanFor(m_function.at(operand).type, representationOf(operand), to); }
    void requireType(const ValueContext&, TypeID);
    void testAheadOfNullishCheck(const ValueContext& test, Operand);
    void testAtLandingOfMisfit(const ValueContext& test, ValueID call, const CodeGenerator::LandingOfMisfit&, const TypeTestPlan&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
