/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct Type;

struct EffectDescription {
    EffectsOnEdges own { EffectsOnEdges::none() };
    Vector<FunctionID, 2> functions;
    // The entry may enter these too. The rest of the description bounds what they do with the operands of this entry, so their summaries add nothing.
    Vector<FunctionID, 2> slowPathFunctions;
    bool callsUnknownCode { false };
    // The description went from the layout of an operand to the layouts that its fields hold, which the entry does not name.
    bool followsFieldsOfOperands { false };
    // Nobody will read `own`. See describeCodeRunBy().
    bool leavesOutOwnEffects { false };

    void addOwn(const Effects& effects)
    {
        if (leavesOutOwnEffects)
            return;
        own.normal.merge(effects);
        if (effects.mayThrow)
            own.handler.merge(effects);
    }
    void addThrow();
    void addFunction(FunctionID);
    void addFunctions(std::span<const FunctionID>);
    void addSlowPathFunction(FunctionID);
    void addUnknownCode() { callsUnknownCode = true; }
};

EffectDescription describeEffects(const Program&, const Function&, const Value&);
EffectDescription describeEffects(const Program&, const Function&, const Terminator&);
// The same, but that EffectDescription::own says nothing: for whoever asks which code the entry runs, and not what the entry does by itself.
EffectDescription describeCodeRunBy(const Program&, const Function&, const Value&);
EffectDescription describeCodeRunBy(const Program&, const Function&, const Terminator&);
EffectsOnEdges resolveEffects(const EffectContext&, const EffectDescription&);

// Where describeDeclaredEntry() does not take the row as it stands: it looks at the types, the payload or the module of the value, and may find that less happens.
bool isRefinedByValue(Opcode, const Effects& bound);
void describeDeclaredEntry(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
// Every primitive but a symbol has a text, and no code runs to make it.
bool hasTextByItself(const Type&);
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
// The lookup and the entrypoints of a table call run what the receiver finds under the name. Where the call lists that, the list stands for them.
CalleeList calleesThatBoundEffectsOf(const Program&, const Value& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
