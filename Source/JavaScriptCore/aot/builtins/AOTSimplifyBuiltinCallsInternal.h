/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTPass.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

unsigned simplifyIntrinsics(FunctionPassContext&);
unsigned simplifyOwnPropertyTests(FunctionPassContext&);
unsigned simplifyPromiseAllOfLiterals(FunctionPassContext&);
unsigned simplifyPromiseResolve(FunctionPassContext&);
unsigned simplifyPrimitiveCalls(FunctionPassContext&);
unsigned simplifyInt32Conversions(FunctionPassContext&);
unsigned simplifyStringReads(FunctionPassContext&);
unsigned simplifyConcatenatingCalls(FunctionPassContext&);
unsigned simplifyMatches(FunctionPassContext&);
unsigned simplifyLastPiecesOfSplits(FunctionPassContext&);

bool keepsTypeOf(FunctionPassContext&, ValueID original, const Value& rewritten);
void replaceKeepingType(FunctionPassContext&, ValueID original, Value&& rewritten);
std::optional<ValueID> findSingleUseDefinition(FunctionPassContext&, ValueID operand, ValueID user);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
