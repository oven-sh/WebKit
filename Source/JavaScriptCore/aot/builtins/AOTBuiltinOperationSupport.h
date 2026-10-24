/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTInstance.h"
#include "JSBigInt.h"
#include "JSCJSValue.h"
#include "MarkedBlock.h"

namespace JSC {

class JSObject;
class Structure;

namespace AOT {

inline constexpr size_t oneDigitBigIntSize = roundUpToMultipleOf<MarkedBlock::atomSize>(JSBigInt::allocationSize(1));

// The receiver is the object that holds the function.
JSValue callInEngine(Instance*, BuiltinID function, JSValue first, JSValue second = JSValue(), JSValue third = JSValue());
JSValue callMethodInEngine(Instance*, BuiltinID function, JSValue receiver, JSValue argument);

void markPristineIfOriginal(Instance&, JSObject*);

void prepareFinalObjectAllocation(Instance*, InlineAllocation, Structure*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
