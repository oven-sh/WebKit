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
    void generateClaim(const ValueContext&);
    void generateAssumeType(const ValueContext&);
    void generateReportMismatch(const ValueContext&);
    void generateCheckInitialized(const ValueContext&);
    void generateRequireObjectCoercible(const ValueContext&);
    void generateRequireObject(const ValueContext&);
    void generateTypeTest(const ValueContext&);

private:
    const TypeTestPlan& planFor(ValueID operand, TypeID to);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
