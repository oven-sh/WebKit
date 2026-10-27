/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

// Type::singleFunction() ignores undefined and null: mayBeNullish says whether the argument's type has them.
DerivedFunctionKey calleeVersionKey(FunctionID function, uint32_t entryParameter, FunctionID callee, bool mayBeNullish);
// A built-in function that is passed is a known callee as a function of the program is: `list.filter(Boolean)`, `texts.map(Number)`.
DerivedFunctionKey calleeVersionKeyForBuiltin(FunctionID function, uint32_t entryParameter, BuiltinID callee, bool mayBeNullish);
// The one built-in function that the type lists, undefined and null aside.
BuiltinID singleBuiltinFunction(const Program&, const Type&);
// One copy for whatever function is passed: in it the parameter is known to hold a function.
DerivedFunctionKey calleeVersionKeyForAnyFunction(FunctionID function, uint32_t entryParameter, bool mayBeNullish);
bool canHaveCalleeVersion(const Program&, FunctionID, uint32_t entryParameter);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
