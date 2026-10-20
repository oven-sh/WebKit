/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"

namespace JSC { namespace AOT { namespace Typed {

// The rule of every statement. The solver calls nothing else.
void runValueFlowRule(const ValueFlowStatement&, ValueFlowRuleContext&);

// Every primitive and the unknown object.
ValueSetID unknownValue(ValueFlowRuleContext&);
// Whoever fills a group of kind Call or Construct says from which position on it passes nothing: a parameter that every caller of the group fills is not undefined.
void passNoFurtherArguments(ValueFlowRuleContext&, const ValueFlowCallGroup&, unsigned numberOfArguments);

// What holds for an operation without one is read from the schema and depends on no set: the bodies turn it into edges and seeds.
bool hasRuleOfItsOwn(Opcode);

void runCall(const ValueFlowStatement&, ValueFlowRuleContext&);
void runCallGroup(const ValueFlowStatement&, ValueFlowRuleContext&);
void runEscape(const ValueFlowStatement&, ValueFlowRuleContext&);

void runNewClass(const ValueFlowStatement&, ValueFlowRuleContext&);
void runDefineMethod(const ValueFlowStatement&, ValueFlowRuleContext&);
void runReadOfPrototypeProperty(const ValueFlowStatement&, ValueFlowRuleContext&);

// A call that no operation spells out: native code runs a protocol of the language. A function of the program is entered, and the entry of a built-in is applied with the operands of this call alone.
// Where something else may be called, `this` and the arguments escape and the result is unknown. What the callees throw escapes.
struct ValueFlowProtocolCall {
    ClimbingSet callees;
    ClimbingSet thisValue;
    // What the protocol passes, position by position.
    std::span<const ValueFlowNodeID> arguments { };
    // What native code makes up and passes at any position.
    ValueSetID otherArguments { ValueSets::nothing };
    // None where native code drops the result.
    ValueFlowNodeID result { };
};
void callInProtocol(ValueFlowRuleContext&, const ValueFlowProtocolCall&);

// IteratorOpen, IteratorResume, IteratorClose, AppendSpread.
void runIterationOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
// StepArrayIterator, MapNext, SetNext.
void runStepOfLibrary(const ValueFlowStatement&, ValueFlowRuleContext&);
// ResumeGenerator, StepGenerator.
void runGeneratorOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runOpenedIterators(const ValueFlowStatement&, ValueFlowRuleContext&);
void runIteratedValues(const ValueFlowStatement&, ValueFlowRuleContext&);
void runIteratorResults(const ValueFlowStatement&, ValueFlowRuleContext&);
// What native code does with what it holds beside reading and calling it: it steps an iterator through the methods that it finds on it, of which a built-in one is in use from then on,
// and it settles a promise through the functions that were made for the executor.
void letNativeCodeRunProtocols(ValueFlowRuleContext&, ClimbingSet escaped);
void runAwait(const ValueFlowStatement&, ValueFlowRuleContext&);
void runAwaitedValue(const ValueFlowStatement&, ValueFlowRuleContext&);
// CreatePromise, ResolvePromise, RejectPromise.
void runPromiseOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runCoroutine(const ValueFlowStatement&, ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
