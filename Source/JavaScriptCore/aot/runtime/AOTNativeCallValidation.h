/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeCallExpectation.h"
#include "AOTRuntimeValidation.h"
#include "ArgList.h"
#include "CodeSpecializationKind.h"
#include "JSCJSValue.h"
#include "NativeFunction.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct NativeCall {
    JSGlobalObject* realm;
    JSObject* callee;
    CodeSpecializationKind kind;
    JSValue thisValue;
    ArgList arguments;
};

// Compares what a native function does to its operands with what the table of built-ins declares and with what the compiler believed about the call.
class NativeCallValidationScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(NativeCallValidationScope);
public:
    // Takes what the caller left for the callee.
    ALWAYS_INLINE NativeCallValidationScope(VM& vm, const NativeCall& call)
    {
        if (validatesRuntime(vm)) [[unlikely]]
            open(vm, call, NativeCallExpectation::take(call.realm, call.callee));
    }

    ALWAYS_INLINE NativeCallValidationScope(VM& vm, const NativeCall& call, const NativeCallExpectation& expectation)
    {
        if (validatesRuntime(vm)) [[unlikely]]
            open(vm, call, expectation);
    }

    ALWAYS_INLINE ~NativeCallValidationScope()
    {
        if (m_checker) [[unlikely]]
            close();
    }

    ALWAYS_INLINE void didReturn(JSValue result)
    {
        if (m_checker) [[unlikely]]
            m_result = result;
    }

    // Ends the scope early. Whether whoever called the function has run code by that: a function of the table that has run none itself is no code of the program.
    // A built-in calls such a function on its own: behind Object.freeze(Array.prototype) concat calls the getter of Array[Symbol.species] and then Array. What it changes is held against its caller like the caller's own doing.
    ALWAYS_INLINE bool finishAndAskWhetherCodeRan()
    {
        return !m_checker || close();
    }

private:
    class Checker;

    JS_EXPORT_PRIVATE void open(VM&, const NativeCall&, const NativeCallExpectation&);
    JS_EXPORT_PRIVATE bool close();

    Checker* m_checker { nullptr };
    JSValue m_result;
};

JSC_DECLARE_HOST_FUNCTION(callNativeFunctionUnderValidation);
JSC_DECLARE_HOST_FUNCTION(constructNativeFunctionUnderValidation);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
