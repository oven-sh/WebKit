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
    Vector<FunctionID, 2> slowPathFunctions;
    bool callsUnknownCode { false };
    bool followsFieldsOfOperands { false };
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
EffectDescription describeCodeRunBy(const Program&, const Function&, const Value&);
EffectDescription describeCodeRunBy(const Program&, const Function&, const Terminator&);
EffectsOnEdges resolveEffects(const EffectContext&, const EffectDescription&);

bool isRefinedByValue(Opcode, const Effects& bound);
void describeDeclaredEntry(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
bool hasTextByItself(const Type&);
uint64_t longestTextOf(const Program&, const Type& primitives);

// The texts of values that one string is made of. A string longer than a string may be is a RangeError.
class JoinedTexts {
public:
    void add(const Program&, const Type&);
    bool fitInString() const;

private:
    uint64_t m_longest { 0 };
    bool m_haveAllTextByThemselves { true };
};

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
void describeTypedResult(const Program&, BuiltinOverloadID, ExpectedResultID, EffectDescription&);
void describeOverload(const Program&, BuiltinOverloadID, const Type& argument, EffectDescription&);
void describeOperandCode(const Program&, const Type& operand, const OperandBehavior&, EffectDescription&);

CalleeList calleesOfCall(const Program&, const Value& call);
CalleeList calleesThatBoundEffectsOf(const Program&, const Value& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
