/*
 * Copyright (C) 2026 Anthropic PBC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "JSFunction.h"
#include "JSObject.h"
#include "JSString.h"

namespace JSC {

class JSPromise;

JSC_DECLARE_HOST_FUNCTION(tracedFunctionCallGeneric);

// A function that runs the embedder's enter/leave hooks (VM::TracedFunctionHooks)
// around a call, forwarding `this` and the arguments. Calls go through a JIT
// thunk (tracedFunctionCallGenerator) that calls the target directly, so the
// hooks are the only cost over a plain call.
//
// Two shapes share the class:
//  - Wrap: the target is fixed (`m_targetFunction`); every call is traced.
//  - CallLast: the target is the last argument of each call and receives the
//    hook's span value as its only argument (`span(name, attributes?, fn)`).
class JSTracedFunction final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    enum class Shape : uint8_t { Wrap, CallLast };

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.tracedFunctionSpace<mode>();
    }

    // `target` null for Shape::CallLast. `data` is opaque to JSC (the embedder's span name / scope).
    JS_EXPORT_PRIVATE static JSTracedFunction* create(VM&, JSGlobalObject*, Shape, JSObject* target, JSValue data, const String& name, unsigned length);

    Shape shape() const { return m_shape; }
    JSObject* targetFunction() LIFETIME_BOUND { return m_targetFunction.get(); }
    JSValue data() const { return m_data.get(); }
    JSString* nameMayBeNull() const LIFETIME_BOUND { return m_name.get(); }
    double length(VM&) const { return m_length; }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static constexpr ptrdiff_t offsetOfTargetFunction() { return OBJECT_OFFSETOF(JSTracedFunction, m_targetFunction); }
    static constexpr ptrdiff_t offsetOfShape() { return OBJECT_OFFSETOF(JSTracedFunction, m_shape); }

    // The frame local the thunk keeps the enter hook's value in (read back on
    // unwind), valid once the frame's CallSiteIndex reads spanLocalValidCallSiteIndex.
    static constexpr int spanLocal = 0;
    static constexpr int numberOfFrameLocals = 1;
    static constexpr uint32_t spanLocalValidCallSiteIndex = 1;

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

private:
    JSTracedFunction(VM&, NativeExecutable*, JSGlobalObject*, Structure*, Shape, JSObject* target, JSValue data, double length);
    void finishCreation(VM&, const String& name);

    WriteBarrier<JSObject> m_targetFunction;
    WriteBarrier<Unknown> m_data;
    WriteBarrier<JSString> m_name;
    double m_length;
    Shape m_shape;
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
