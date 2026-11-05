/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallingConventions;
class Facts;
class Plan;
class Program;

struct TailCallPlan {
    // By function: the Body saves every register that it writes but those of StubBodyConvention::registersClobberedByCalls(). Empty: none.
    BitVector functionsThatSaveRegistersOfCallers;

    bool savesRegistersOfCallers(CodeUnit unit) const { return unit.role == CodeRole::Body && functionsThatSaveRegistersOfCallers.get(unit.function.index()); }
};

// A PROOF, off linked(), the summaries and the rows. By function. Set: each call that the function makes in tail position enters native code that comes back, or ONE listed function outside the function's own ring of listed tail calls of which the same holds.
// So whoever calls it in tail position may keep his frame until it returns, at the price of one frame. Clear claims nothing: the call has to be a jump.
// A call in tail position that Link does not list ends nothing. Nor does a built-in whose row forwards the call to an operand: the walk puts the operand's call in its place. A function on a ring, be it of itself alone, does not end.
JS_EXPORT_PRIVATE BitVector findFunctionsThatEndTheirTailCalls(const Program&, const Facts&);
// checkPlan(), where the build validates. By a graph of its own, off linked() and the summaries: round a ring of calls in tail position that each name their one callee all bodies return in one form,
// and in a raw one only where each of those calls is written as a direct one: a unit that returns a raw value cannot leave by a call of a value. Else InternalErrorKind::PlacementCheckFailed, with the caller and the call.
void checkFormsRoundRingsOfTailCalls(const Program&, const Facts&, const Plan&, BuildContext&);
// TailCallPlan::functionsThatSaveRegistersOfCallers. The conventions are whole: guards are set. It asks stackWordsReservedByCallersOf().
BitVector findFunctionsThatSaveRegistersOfCallers(const Program&, const Facts&, const CallingConventions&);
// planProgram(), behind the guards and in front of the fused entries. In this order: ends of tail calls > registers > stack tests. With DirectCalls withheld: nothing.
// The ends of tail calls go by Facts alone: planProgram() asks findFunctionsThatEndTheirTailCalls() beside its first lanes and sets them right in front of this. checkStackTests() runs wherever checkPlan() runs.
void planTailCallsAndStackTests(const Program&, const Facts&, Plan&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
