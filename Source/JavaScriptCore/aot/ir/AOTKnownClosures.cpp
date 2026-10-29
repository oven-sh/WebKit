/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKnownClosures.h"

#if ENABLE(AOT)

#include "AOTSchemaResultRules.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SameValueRules {
    using Result = ValueID;

    ValueID sameAs(const Instruction& instruction, uint32_t operandIndex) { return instruction.operand(operandIndex); }
    ValueID cast(const Value& value, uint32_t operandIndex, TypeID = { }) { return value.operand(operandIndex); }
    ValueID cast(const Value& value, uint32_t operandIndex, ExpectedType) { return value.operand(operandIndex); }

    ValueID none(const Instruction&) { return { }; }
    ValueID constant(const Value&) { return { }; }
    ValueID unknown(const Instruction&) { return { }; }
    ValueID declaredParameter(const Value&) { return { }; }
    ValueID natural(const Instruction&, const auto&) { return { }; }
    ValueID allocation(const Value&, const auto&...) { return { }; }
    ValueID declared(const Instruction&, const auto&...) { return { }; }
    ValueID declaredContents(const Value&, const auto&...) { return { }; }
    ValueID declaredResult(const Value&, const auto&...) { return { }; }
    ValueID resultOperand(const Value&, uint32_t) { return { }; }
};

} // anonymous namespace

ValueID sameValueOperandOf(const Value& value)
{
    SameValueRules rules;
    return applyResultRule(rules, value, 0, ResultSide::Unproven);
}

ValueID definitionThroughSameValues(const Function& function, ValueID id)
{
    while (ValueID operand = sameValueOperandOf(function.at(id)))
        id = operand;
    return id;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
