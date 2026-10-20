/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class RepresentationEmitter final : public OperationEmitter {
public:
    explicit RepresentationEmitter(CodeGenerator&);

    void generateBox(const ValueContext&);
    void generateUnbox(const ValueContext&);
    void generateIntegerToNumber(const ValueContext&);
    void generateNumberToInteger(const ValueContext&);
    void generateWidenInteger(const ValueContext&);
    void generateNarrowInteger(const ValueContext&);
    void generateDecode(const ValueContext&);
    void generateCompress(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
