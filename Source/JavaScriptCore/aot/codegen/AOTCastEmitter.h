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

    void generateNarrowed(const ValueContext& value) { defineNarrowed(value, get(value->valueOperand())); }
    void generateAssumeType(const ValueContext& value) { defineNarrowed(value, get(value->valueOperand())); }
    void generateCheckInitialized(const ValueContext&);
    void generateRequireObjectCoercible(const ValueContext&);
    void requireObjectCoercible(const ValueContext&);
    void generateRequireObject(const ValueContext&);
    void generateTypeTest(const ValueContext&);
    void generateCheckProof(const ValueContext&);

private:
    const TypeTestPlan& planFor(ValueID operand, TypeID to) { return typeTestPlanFor(typeIDOf(operand), representationOf(operand), to); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
