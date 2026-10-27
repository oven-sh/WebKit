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

    Operand convert(const OperationContext&, Opcode conversion, ValueID sourceID, Operand source, const Type&, Representation to);

private:
    void generate(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
