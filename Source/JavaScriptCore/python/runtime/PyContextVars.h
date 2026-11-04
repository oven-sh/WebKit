/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

namespace JSC {
namespace Python {

// contextvars.Context, for the C++ of other modules.

// contextvars.copy_context()
JSValue copyCurrentContext(JSGlobalObject*);
bool isContext(JSGlobalObject*, JSValue);
// What context.run() does before and after the call. False: an exception (the context is entered already; it is not the
// one the thread is in).
bool enterContext(JSGlobalObject*, JSValue context);
bool exitContext(JSGlobalObject*, JSValue context);

// contextvars.ContextVar(name)
JSValue newContextVariable(JSGlobalObject*, ASCIILiteral name);
// variable.get(), but for the default. Empty, without an exception: it has no value in the context the thread is in.
JSValue contextVariableValue(JSGlobalObject*, JSValue variable);
// variable.set(value), without the token. False: an exception.
bool setContextVariable(JSGlobalObject*, JSValue variable, JSValue value);

} // namespace Python
} // namespace JSC
