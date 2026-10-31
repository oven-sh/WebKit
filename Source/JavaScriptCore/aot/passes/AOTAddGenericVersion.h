/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <span>
#include "AOTBitVector.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

// Flow gives a position back where it sees no value outside the type arrive.
enum class TaintedPositionsRun : uint8_t { BeforeFlow, AfterFlow };
void guardTaintedPositions(Program&, BuildContext&, TaintedPositionsRun);
void guardTaintedPositionsAfterFlow(Program&, const FlowResults&, BuildContext&);
// The types of the fields are only expected from here on. guardTaintedPositionsAfterFlow() puts the checks at their reads.
void expectFields(Program&, std::span<const FieldReference>, BuildContext&);
// The body is typed without its checks where it stands, and is left as it was.
bool mayReturnTaintedValue(const Program&, FunctionID, Function& body, BuildContext&);
bool mayTurnValueAway(const Program&, const Function&, const Value& check);
// A type that holds any object names no layout: code that has it proven reads the object by name, as code that knows nothing does. A second version for the values that it turns away stands beside the same code.
bool changesCodeWhereProven(const Type& checked);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
