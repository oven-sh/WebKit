/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;

// The operation does not look at the operand and puts it nowhere but where such a word may lie: a cast that tests nothing, a change of representation, the list of what a function returns,
// the store back into the variable that the word was read from, the list of what a suspended function holds. A validating build compares words.
bool handsOperandOn(const Program&, const Value& user, uint32_t operandIndex);

// The value behind a cast that names the fact, with the type that is left without `unspecified`.
Value makeCastBehindFact(const Function&, TypeArena&, ValueID, ValueID fact);

// A pass fills a position with Unspecified on the ways that make no use of it. So every use in the function stands where the position has been filled.
// The parameters and casts that such a word gets to take the type that says so. An operation that does more than hand the word on takes it through a cast, and the cast names a fact that stands in front of the operation.
// Says whether the function has changed.
bool nameFactsWhereUnspecifiedValuesAreUsed(IRBuilder&, FunctionAnalyses&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
