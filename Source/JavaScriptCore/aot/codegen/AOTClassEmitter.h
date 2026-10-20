/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ClassEmitter final : public OperationEmitter {
public:
    explicit ClassEmitter(CodeGenerator&);

    void generateResolveHeritage(const ValueContext&);
    void generateNewClass(const ValueContext&);
    void generateGetClassPrototype(const ValueContext&);
    void generateDefineMethod(const ValueContext&);
    void generateFinishClass(const ValueContext&);
    void generateNewClassToken(const ValueContext&);
    void generateGetConstructPrototype(const ValueContext&);
    void generateSuperConstruct(const ValueContext&);
    void generateSuperConstructList(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
