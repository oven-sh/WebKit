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

// For what runs on top of whatever the thread happens to be doing, and is not called by that: for its length the thread is in the context in which it was before it entered any. What is set there is
// there for the next such, and for the program outside every context.
class OutermostContextScope {
    WTF_MAKE_NONCOPYABLE(OutermostContextScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit OutermostContextScope(JSGlobalObject*);
    ~OutermostContextScope();

private:
    JSGlobalObject* m_globalObject;
    JSValue m_current; // Empty: the thread has entered none.
};

// contextvars.ContextVar(name)
JSValue newContextVariable(JSGlobalObject*, ASCIILiteral name);
// variable.get(), but for the default. Empty, without an exception: it has no value in the context the thread is in.
JSValue contextVariableValue(JSGlobalObject*, JSValue variable);
// variable.set(value), without the token. False: an exception.
bool setContextVariable(JSGlobalObject*, JSValue variable, JSValue value);

} // namespace Python
} // namespace JSC
