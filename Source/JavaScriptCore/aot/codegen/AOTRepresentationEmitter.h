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

    void generateBox(const ValueContext& value) { generate(value); }
    void generateUnbox(const ValueContext& value) { generate(value); }
    void generateIntegerToNumber(const ValueContext& value) { generate(value); }
    void generateNumberToInteger(const ValueContext& value) { generate(value); }
    void generateWidenInteger(const ValueContext& value) { generate(value); }
    void generateNarrowInteger(const ValueContext& value) { generate(value); }
    void generateDecode(const ValueContext& value) { generate(value); }
    void generateCompress(const ValueContext& value) { generate(value); }

    // One step of findConversion(). The type must be proven for the source value, not merely expected, and `to` must be a legal representation for that type: no step tests that the value fits.
    Operand convert(const OperationContext&, Opcode conversion, Operand source, const Type&, Representation to);

private:
    void generate(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
