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
    explicit CastEmitter(CodeGenerator&);

    void generateUpcast(const ValueContext&);
    void generateNarrowByField(const ValueContext&);
    void generateAssumeType(const ValueContext&);
    void generateReadAgain(const ValueContext&);
    void generateBorrowedParameter(const ValueContext&);
    void generateReportMismatch(const ValueContext&);
    void generateCheckInitialized(const ValueContext&);
    void generateRequireObjectCoercible(const ValueContext&);
    void requireObjectCoercible(const ValueContext&);
    void generateRequireObject(const ValueContext&);
    void generateTypeTest(const ValueContext&);

private:
    const TypeTestPlan& planFor(ValueID operand, TypeID to);
    void testAheadOfNullishCheck(const ValueContext& test, Operand);
    void testAtLandingOfMisfit(const ValueContext& test, ValueID call, const CodeGenerator::LandingOfMisfit&, const TypeTestPlan&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
