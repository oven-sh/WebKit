/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTRepresentationRules.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct ConversionStep {
    Opcode opcode { Opcode::Box };
    Representation to { Representation::JSValue };

    friend bool operator==(const ConversionStep&, const ConversionStep&) = default;
};

using ConversionPath = Vector<ConversionStep, 2>;

std::optional<ConversionPath> findConversion(const Type&, Representation from, Representation to);

class ConversionInserter {
    WTF_MAKE_NONCOPYABLE(ConversionInserter);
public:
    ConversionInserter(FunctionPassContext&, IRBuilder&);

    void convertOperand(ValueID user, uint32_t operandIndex, Representation wanted, ProgramPoint pointOfUser);
    void convertTerminatorOperand(BlockID, uint32_t operandIndex, Representation wanted);

    uint32_t numberOfConversions() const { return m_numberOfConversions; }
    void finish();

private:
    ValueID convert(ValueID source, Representation wanted, ProgramPoint before);
    void reportUnsupportedConversion(ValueID source, Representation wanted, BlockID);

    FunctionPassContext& m_pass;
    IRBuilder& m_builder;
    uint32_t m_numberOfConversions { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
