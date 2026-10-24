/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class PrivateAccessEmitter final : public OperationEmitter {
public:
    explicit PrivateAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generatePrivateGet(const ValueContext&);
    void generatePrivateSet(const ValueContext&);
    void generatePrivateDefine(const ValueContext&);
    void generatePrivateHas(const ValueContext&);

private:
    uint16_t indexInLayoutRecord(const ValueContext&);
    void storePrivateField(const ValueContext&, RuntimeOperation);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
