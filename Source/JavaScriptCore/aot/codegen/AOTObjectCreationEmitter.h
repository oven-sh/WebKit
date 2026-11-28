/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ObjectCreationEmitter final : public OperationEmitter {
public:
    explicit ObjectCreationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateDefineOwnProperty(const ValueContext&);
    void generateDefineAccessors(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
