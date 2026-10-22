/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class CFG;
class UseLists;

// The values that the generic version of the function computes anew: the checks, and whatever takes one of them. There the type of such a value says nothing.
BitVector computeTaintedValues(const Function&, const UseLists&, std::span<const ValueID> checks);
// The same values, for whoever has no use lists: to make them costs ten times what it costs to walk the body in the order in which the marks are handed on.
BitVector computeTaintedValues(const Function&, const CFG&, std::span<const ValueID> checks);

// Flow gives a position back where it sees no value outside the type arrive.
enum class TaintedPositionsRun : uint8_t { BeforeFlow, AfterFlow };
void guardTaintedPositions(Program&, BuildContext&, TaintedPositionsRun);
void guardTaintedPositionsAfterFlow(Program&, const FlowResults&, BuildContext&);
bool mayReturnTaintedValue(const Program&, FunctionID, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
