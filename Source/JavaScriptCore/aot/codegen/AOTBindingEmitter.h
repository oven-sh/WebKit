/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class BindingEmitter final : public OperationEmitter {
public:
    explicit BindingEmitter(CodeGenerator&);

    void generateGetModuleVariable(const ValueContext&);
    void generateGetEncodedModuleVariable(const ValueContext&);
    void generatePutModuleVariable(const ValueContext&);
    void generateGetEnvironmentField(const ValueContext&);
    void generatePutEnvironmentField(const ValueContext&);
    void generateScopeHasBinding(const ValueContext&);
    void generateParameter(const ValueContext&);

private:
    void failAndDefineZero(const ValueContext&, InternalErrorKind);
    void defineSingletonClosure(const ValueContext&, FunctionID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
