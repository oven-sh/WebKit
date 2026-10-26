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

// Flow gives a position back where it sees no value outside the type arrive.
enum class TaintedPositionsRun : uint8_t { BeforeFlow, AfterFlow };
void guardTaintedPositions(Program&, BuildContext&, TaintedPositionsRun);
void guardTaintedPositionsAfterFlow(Program&, const FlowResults&, BuildContext&);
// The body is typed without its checks where it stands, and is left as it was.
bool mayReturnTaintedValue(const Program&, FunctionID, Function& body, BuildContext&);
bool mayTurnValueAway(const Program&, const Function&, const Value& check);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
