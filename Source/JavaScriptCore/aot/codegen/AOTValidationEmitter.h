/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ValidationEmitter final : public OperationEmitter {
public:
    explicit ValidationEmitter(CodeGenerator&);

    void generateIsPlainDataObject(const ValueContext&);
    void generateGetOwnDataProperty(const ValueContext&);
    void generateCloneAs(const ValueContext&);
    void generateGetPreparedPlans(const ValueContext&);
    void generateIsThrownValueIgnored(const ValueContext&);
    void generateMatchesUnrecorded(const ValueContext&);
    void generateExpectRows(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
