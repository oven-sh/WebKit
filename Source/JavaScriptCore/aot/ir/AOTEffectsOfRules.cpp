/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfRules.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

EffectsOnEdges effectsOfDeclaredEntry(const EffectContext& context, const Function& function, const Value& value, const Effects& declared)
{
    EffectDescription description;
    describeDeclaredEntry(context.program, function, value, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfOperator(const EffectContext& context, const Function& function, const Value& value, const Effects& declared)
{
    EffectDescription description;
    describeOperator(context.program, function, value, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfOperator(const EffectContext& context, Opcode opcode, std::span<const Type> operandTypes, const Type& result)
{
    EffectDescription description;
    describeOperator(context.program, opcode, operandTypes, result, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfAccess(const EffectContext& context, const Function& function, const Value& value, const Effects& declared)
{
    EffectDescription description;
    describeAccess(context.program, function, value, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfCall(const EffectContext& context, const Function& function, const Value& value, const Effects& declared)
{
    EffectDescription description;
    describeCall(context.program, function, value, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfBuiltinCall(const EffectContext& context, const Function& function, const Value& value, const Effects& declared)
{
    EffectDescription description;
    describeBuiltinCall(context.program, function, value, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfInlineBuiltin(const EffectContext& context, const Function& function, const Value& value, const Effects& declared)
{
    EffectDescription description;
    describeInlineBuiltin(context.program, function, value, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfInlineBuiltin(const EffectContext& context, const Function& function, const Terminator& terminator, const Effects& declared)
{
    EffectDescription description;
    describeInlineBuiltin(context.program, function, terminator, declared, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfUnknownCode(const EffectContext& context)
{
    return EffectsOnEdges::onBothEdges(context.summaries.unknownCode());
}

EffectsOnEdges effectsOfCalling(const EffectContext& context, FunctionID function)
{
    return CalleeSummary(context, function)->onCall();
}

EffectsOnEdges effectsOfCalling(const EffectContext& context, BuiltinID builtin, BuiltinCallMode mode)
{
    EffectDescription description;
    describeCallOf(context.program, builtin, mode, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfCallees(const EffectContext& context, const CalleeList& callees, BuiltinCallMode mode)
{
    EffectDescription description;
    describeCallees(context.program, callees, mode, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfCallees(const EffectContext& context, const CalleeList& callees, BuiltinCallMode mode, const EnteringOperands& operands)
{
    EffectDescription description;
    describeCallees(context.program, callees, mode, operands, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfOverload(const EffectContext& context, BuiltinOverloadID overload, const Type* receiver, std::span<const TypeID> arguments)
{
    EffectDescription description;
    describeOverload(context.program, overload, receiver, arguments, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfTypedResult(const EffectContext& context, BuiltinOverloadID overload, ExpectedResultID expectedResult)
{
    EffectDescription description;
    describeTypedResult(context.program, overload, expectedResult, description);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsOfOverload(const EffectContext& context, BuiltinOverloadID overload, BuiltinCallForm form, const Type* receiver, std::span<const TypeID> arguments)
{
    EffectDescription description;
    describeOverload(context.program, overload, form, receiver, arguments, description);
    return resolveEffects(context, description);
}

void addOperandCode(const EffectContext& context, const Type& operand, const OperandBehavior& behavior, EffectsOnEdges& effects)
{
    EffectDescription description;
    describeOperandCode(context.program, operand, behavior, description);
    effects.merge(resolveEffects(context, description));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
