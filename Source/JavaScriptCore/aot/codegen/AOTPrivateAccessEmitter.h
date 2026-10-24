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
    explicit PrivateAccessEmitter(CodeGenerator&);

    void generatePrivateGet(const ValueContext&);
    void generatePrivateSet(const ValueContext&);
    void generatePrivateDefine(const ValueContext&);
    void generatePrivateHas(const ValueContext&);

private:
    Word32 indexInLayoutRecord(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
