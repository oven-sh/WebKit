/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct Type;

struct EffectDescription {
    EffectsOnEdges own { EffectsOnEdges::none() };
    Vector<FunctionID, 2> functions;
    // The entry may enter these too. The rest of the description bounds what they do with the operands of this entry, so their summaries add nothing.
    Vector<FunctionID, 2> slowPathFunctions;
    bool callsUnknownCode { false };

    void addOwn(const Effects& effects) { own.merge(EffectsOnEdges::onBothEdges(effects)); }
    void addThrow();
    void addFunction(FunctionID);
    void addSlowPathFunction(FunctionID);
    void addUnknownCode() { callsUnknownCode = true; }
};

EffectDescription describeEffects(const Program&, const Function&, const Value&);
EffectDescription describeEffects(const Program&, const Function&, const Terminator&);
EffectsOnEdges resolveEffects(const EffectContext&, const EffectDescription&);

void describeDeclaredEntry(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeOperator(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeOperator(const Program&, Opcode, std::span<const Type> operandTypes, const Type& result, EffectDescription&);
void describeAccess(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeCall(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeBuiltinCall(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeInlineBuiltin(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeInlineBuiltin(const Program&, const Function&, const Terminator&, const Effects& declared, EffectDescription&);

void describeBound(const Effects& declared, EffectDescription&);
void describeCallees(const Program&, const CalleeList&, BuiltinCallMode, EffectDescription&);
void describeCallees(const Program&, const CalleeList&, BuiltinCallMode, const EnteringOperands&, EffectDescription&);
void describeCallOf(const Program&, BuiltinID, BuiltinCallMode, EffectDescription&);
void describeOperandsOfCall(const Program&, BuiltinID, BuiltinCallMode, BuiltinCallForm, const Type& receiver, std::span<const TypeID> arguments, EffectDescription&);
void describeOverload(const Program&, BuiltinOverloadID, const Type* receiver, std::span<const TypeID> arguments, EffectDescription&);
// What a call of the overload with this expected result does beyond the overload. Nothing without an expected result.
void describeTypedResult(const Program&, BuiltinOverloadID, ExpectedResultID, EffectDescription&);
// A call without receiver that passes one argument of the type.
void describeOverload(const Program&, BuiltinOverloadID, const Type& argument, EffectDescription&);
void describeOperandCode(const Program&, const Type& operand, const OperandBehavior&, EffectDescription&);

CalleeList calleesOfCall(const Program&, const Value& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
