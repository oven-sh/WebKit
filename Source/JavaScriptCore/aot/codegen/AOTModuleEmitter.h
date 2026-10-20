/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ModuleEmitter final : public OperationEmitter {
public:
    explicit ModuleEmitter(CodeGenerator&);

    void generateModuleNamespace(const ValueContext&);
    void generateImportModule(const ValueContext&);
    void generateRequireModule(const ValueContext&);
    void generateResolveModule(const ValueContext&);
    void generateThrowModuleFailure(const ValueContext&);
    void generateImportDynamic(const ValueContext&);
    void generateRequireDynamic(const ValueContext&);
    void generateResolveDynamic(const ValueContext&);
    void generateGetRequiredValue(const ValueContext&);

private:
    Word32 moduleIndex(ModuleID);
    Boxed loadModuleValue(ModuleID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
