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

    // One step of a conversion that is no operation of the function: the context is that of the operation that takes the result. The source has no id where an earlier step has made it.
    Operand convert(const OperationContext&, Opcode conversion, ValueID sourceID, Operand source, const Type&, Representation to);

private:
    void generate(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
