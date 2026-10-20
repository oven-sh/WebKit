/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTFlowIteration.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct FlowOperandApplication {
    FlowStatementSite site;
    ValueID origin;
    FlowNodeID operand;
    FlowNodeID retainer;
    FlowNodeID result;
    FlowNodeID thrown;
    // What the protocols find goes back into the operand and nowhere else.
    bool resultStaysInOperand { false };
};

inline bool claimsEveryUse(const OperandBehavior& behavior) { return behavior.uses == OperandUses::all(); }

void applyOperandBehavior(FlowSolver&, const FlowOperandApplication&, const OperandBehavior&);
void applyOperandBehaviorTo(FlowSolver&, const FlowOperandApplication&, const OperandBehavior&, const FlowNewObjects&);
void connectOperand(FlowSolver&, const FlowOperandApplication&, const OperandBehavior&);
void retainInObjects(FlowSolver&, const FlowOperandApplication&, FlowNodeID retained);
void applyContentUses(FlowSolver&, const FlowOperandApplication&, const OperandBehavior&, FlowObjectID);
void applyMutatingUses(FlowSolver&, const FlowOperandApplication&, OperandUses, const FlowNewObjects&);
void noteIntegrityChange(FlowSolver&, const FlowObjectSet&, const Witness&);
void applyEveryUseToUnknownObject(FlowSolver&, const FlowOperandApplication&, const BuiltinParameter&);
void applyEveryUseThroughUnknownObject(FlowSolver&, const FlowOperandApplication&);
void callOperandObjects(FlowSolver&, const FlowOperandApplication&, OperandUses, const FlowNewObjects&);
FlowNodeID retainedContentsTargetOf(FlowSolver&, const FlowOperandApplication&, const OperandBehavior&);
BuiltinCallbackTimes callbackTimesOf(OperandUses);

// All that an object in native code holds is in native code too, and the unknown object stands for it.
enum class FlowEscapedObjects : uint8_t {
    AreFollowed,
    AreUnknown,
};

FlowNodeID transitiveContentsOf(FlowSolver&, FlowStatementSite, FlowNodeID operand, FlowEscapedObjects = FlowEscapedObjects::AreFollowed);

// Native code reads, calls and passes on whatever has reached it, once for the program, and answers with the unknown value.
// A statement that runs only such protocols leaves a receiver in native code to that rule: the unknown object takes its place.
bool areRunByNativeCode(Protocols, bool resultIsRead);
bool isReadByProgram(const FlowSolver&, FlowNodeID result);

inline bool mayActOnPrimitives(Protocols protocols)
{
    return protocols.containsAny({ Protocol::Thenable, Protocol::Iterate, Protocol::ToJSON });
}

// Where this is false, the protocols do nothing before the first object arrives.
inline bool actOnPrimitives(Protocols protocols, uint16_t primitives)
{
    return (primitives && protocols.contains(Protocol::Thenable))
        || ((primitives & Primitive::AnyString) && protocols.contains(Protocol::Iterate))
        || ((primitives & Primitive::BigInt) && protocols.contains(Protocol::ToJSON));
}

inline FlowWakeUp wakeUpAtFirstReceiver(Protocols protocols)
{
    return mayActOnPrimitives(protocols) ? FlowWakeUp::Continue : FlowWakeUp::ContinueAtObject;
}

void runProtocolsOn(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runProtocolsOnPrimitives(FlowSolver&, const FlowProtocolRequest&, uint16_t primitives);
void runOwnKeys(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runOwnValues(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void readInheritedEnumerableValues(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runToPrimitive(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runThenable(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runIterate(FlowSolver&, const FlowProtocolRequest&);
void runSpecies(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runToJSON(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runHasInstance(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runNamedMembers(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);
void runCalledMembers(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects&);

struct FlowMemberCall {
    FlowKeySet keys;
    std::span<const FlowNodeID> arguments;
    FlowNodeID result;
};

void callProtocolMember(FlowSolver&, const FlowProtocolRequest&, const FlowNewObjects& receivers, const FlowMemberCall&);
FlowKeySet keysReadBy(const Program&, Protocol);
FlowKeySet keysOfNames(const Program&, std::span<const BuiltinName>);

void runOperationWithoutOperand(FlowSolver&, FlowStatementSite, const FlowOperandUse&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
