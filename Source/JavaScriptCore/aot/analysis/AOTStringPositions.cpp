/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringPositions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"

namespace JSC { namespace AOT { namespace Typed {

ValueID stringMeasuredBy(const Program& program, const Function& function, const Value& value)
{
    ValueID string;
    if (value.opcode == Opcode::GetStringLength)
        string = value.stringOperand();
    else if (value.opcode == Opcode::CallBuiltin && value.builtinCallForm() == BuiltinCallForm::Whole && program.builtins().at(value.overload()).builtin == Builtins::StringPrototypeLength)
        string = value.receiverOperand();
    if (!string || !program.typeOf(function, string).isAnyString())
        return { };
    return string;
}

StringPositions::StringPositions(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_relations(analyses)
{
}

void StringPositions::collectLengths()
{
    const Program& program = m_analyses.context().program;
    const Function& function = m_analyses.function();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (ValueID string = stringMeasuredBy(program, function, function.at(id)))
                m_lengthsOfString.ensure(m_analyses.identityOf(string), [] { return Lengths(); }).iterator->value.append(id);
        }
    }
}

bool StringPositions::isInside(ValueID string, ValueID position, BlockID at)
{
    if (m_relations.isAtLeast(position, 0, at) != IntegerRelations::Answer::Proven)
        return false;
    if (!std::exchange(m_didCollectLengths, true))
        collectLengths();

    auto lengths = m_lengthsOfString.find(m_analyses.identityOf(string));
    if (lengths == m_lengthsOfString.end())
        return false;
    return std::ranges::any_of(lengths->value, [&](ValueID length) {
        return m_relations.isAtMost(position, length, -1, at) == IntegerRelations::Answer::Proven;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
