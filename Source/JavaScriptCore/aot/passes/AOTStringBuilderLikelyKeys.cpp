/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPass.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTValueIdentity.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr OpcodeSet keyedAccesses { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };

class LikelyKeyMarker {
public:
    explicit LikelyKeyMarker(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_useLists(context.analyses.useLists())
    {
    }

    bool run();

private:
    bool isOnlyUsedAsKey(ValueID string);

    FunctionPassContext& m_context;
    Function& m_function;
    const UseLists& m_useLists;
    BitVector m_visitedValues;
    Vector<ValueID, 8> m_worklist;
};

bool LikelyKeyMarker::run()
{
    bool didChange = false;
    for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (!canBeMarkedAsLikelyKey(m_context.program, m_function, m_function.at(id)) || !isOnlyUsedAsKey(id))
                continue;
            markAsLikelyKey(m_function, id);
            didChange = true;
        }
    }
    return didChange;
}

bool LikelyKeyMarker::isOnlyUsedAsKey(ValueID string)
{
    m_visitedValues.clearAll();
    m_visitedValues.set(string.index());
    m_worklist.shrink(0);
    m_worklist.append(string);
    bool isUsedAsKey = false;
    while (!m_worklist.isEmpty()) {
        std::span<const Use> uses = m_useLists.usesOf(m_worklist.takeLast());
        if (uses.empty())
            return false;
        for (const Use& use : uses) {
            if (isKeyOperand(m_context.program, m_function, use)) {
                isUsedAsKey = true;
                continue;
            }
            // As the call by name that stands in where a Map is not what its type says.
            if (m_function.at(use.block).frequency != BlockFrequency::Normal)
                continue;

            ValueID sameString;
            if (use.kind == Use::Kind::EdgeArgument)
                sameString = m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex];
            else if (use.kind == Use::Kind::Operand && isCast(m_function.at(use.user).opcode))
                sameString = use.user;
            else
                return false;
            if (!m_visitedValues.set(sameString.index()))
                m_worklist.append(sameString);
        }
    }
    return isUsedAsKey;
}

} // anonymous namespace

// Add converts an object with another hint than Strcat does.
bool canBeMarkedAsLikelyKey(const Program& program, const Function& function, const Value& value)
{
    if (Value::opcodesWithIsLikelyKey.contains(value.opcode))
        return !value.isLikelyKey();
    if (value.opcode != Opcode::Add || !isStringConcatenation(program, function, value))
        return false;
    return isStringBuilderPart(program.typeOf(function, value.leftOperand())) && isStringBuilderPart(program.typeOf(function, value.rightOperand()));
}

void markAsLikelyKey(Function& function, ValueID id)
{
    Value& string = function.edit(id);
    if (string.opcode == Opcode::Add) {
        Vector<ValueID, 2> parts { string.leftOperand(), string.rightOperand() };
        string.become(Opcode::Strcat);
        string.setVariadicOperands(parts.span());
    }
    string.setIsLikelyKey(true);
}

bool isKeyOperand(const Program& program, const Function& function, const Use& use)
{
    if (use.kind != Use::Kind::Operand)
        return false;
    const Value& user = function.at(use.user);
    if (keyedAccesses.contains(user.opcode))
        return use.operandIndex == OperandPosition::property(user.opcode);
    if (user.opcode != Opcode::CallBuiltin || builtinOperandAt(user, use.operandIndex) != BuiltinOperand::argument(0))
        return false;
    std::optional<InlineBuiltin> builtin = program.builtins().at(user.overload()).code.implementation.inlineBuiltin;
    return builtin && familyOf(*builtin) == InlineBuiltinFamily::HashTableBuiltin;
}

bool markLikelyKeys(FunctionPassContext& context)
{
    if (!LikelyKeyMarker(context).run())
        return false;
    context.analyses.didChangeValues();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
