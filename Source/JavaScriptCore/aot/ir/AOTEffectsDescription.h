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

class EffectSummaries;
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
    void add(const EffectDescription&);

    friend bool operator==(const EffectDescription&, const EffectDescription&) = default;
};

// A table that is given keeps what is found out about a long list of callees.
EffectDescription describeEffects(const Program&, const Function&, const Value&, const EffectSummaries* = nullptr, UntestedStorage = UntestedStorage::MakesReadUnknown);
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
void describeAccess(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&, UntestedStorage);
void describeCall(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&, const EffectSummaries* = nullptr);
void describeBuiltinCall(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
EffectDescription describeUnmatchedCallByOperandTypes(const Program&, const Function&, const Value&);
void describeInlineBuiltin(const Program&, const Function&, const Value&, const Effects& declared, EffectDescription&);
void describeInlineBuiltin(const Program&, const Function&, const Terminator&, const Effects& declared, EffectDescription&);

void describeBound(const Effects& declared, EffectDescription&);
void describeCallees(const Program&, const CalleeList&, BuiltinCallMode, EffectDescription&);
void describeCallees(const Program&, const CalleeList&, BuiltinCallMode, const EnteringOperands&, EffectDescription&, const EffectSummaries* = nullptr);
void describeCallOf(const Program&, BuiltinID, BuiltinCallMode, EffectDescription&);
void describeOperandsOfCall(const Program&, BuiltinID, BuiltinCallMode, BuiltinCallForm, TypeID receiver, std::span<const TypeID> arguments, EffectDescription&);
void describeOverload(const Program&, BuiltinOverloadID, TypeID receiver, std::span<const TypeID> arguments, EffectDescription&);
void describeTypedResult(const Program&, BuiltinOverloadID, ExpectedResultID, EffectDescription&);
void describeOverload(const Program&, BuiltinOverloadID, const Type& argument, EffectDescription&);
void describeOperandCode(const Program&, const Type& operand, const OperandBehavior&, EffectDescription&);
void describeOperandCode(const Program&, TypeID operand, const OperandBehavior&, EffectDescription&);

CalleeList calleesOfCall(const Program&, const Value& call);
// Empty for every other operation. The call enters them, so it keeps them alive, which a stamp does not.
std::span<const FunctionID> entrypointsOfTableCall(const Program&, const Value& call);
inline std::span<const FunctionID> entrypointsOfTableCall(const Program&, const Terminator&) { return { }; }
// The set that calleesOfCall() copies its answer from, if it is one. Reading the set costs nothing per callee.
CalleeSetID calleeSetOfCall(const Program&, const Value& call);
// The long set whose functions are the functions that describeCodeRunBy() lists for the call, if there is one.
CalleeSetID longCalleeSetRunBy(const Program&, const Value& call);
inline CalleeSetID longCalleeSetRunBy(const Program&, const Terminator&) { return { }; }
// What describeCodeRunBy() says of such a call, without the functions of the set.
EffectDescription describeCodeRunBesideLongCalleeSet(const Program&, const Value& call, CalleeSetID);
CalleeList calleesThatBoundEffectsOf(const Program&, const Value& call);
// A call of what is no function throws, and so does a call of a class and `new` of what is no constructor. A list of callees says what is entered, not that anything is.
bool mayThrowBeforeEntry(const Program&, const Type& callee, BuiltinCallMode);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
