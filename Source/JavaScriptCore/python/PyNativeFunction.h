/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "JSFunction.h"

namespace JSC {

// A function of Python's that is written in C++.
//
// Its arguments are its Python arguments. If it was given keywords, their values come after the positional arguments and `this` is
// their names, in a JSCellButterfly, which is nothing that Python or JavaScript code can get hold of. Python::NativeArguments reads them.
class PyNativeFunction final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    enum class Kind : uint8_t {
        Function, // len. As an attribute of a class it is what it is.
        Method, // list.append. Got from an instance it is bound to it, and its first argument is the instance.
        ClassMethod, // dict.fromkeys. It is bound to the class, and its first argument is the class.
        New, // list.__new__. Like Function, and its first argument is a class that is the owner or derived from it.
    };

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyNativeFunctionSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    // `owner` is the class that it is a method of, or the module that it is a function of.
    // `data` is for the function itself: one function in C++ can be many in Python that differ only by it, as __lt__ and __gt__ do.
    JS_EXPORT_PRIVATE static PyNativeFunction* create(VM&, JSGlobalObject*, unsigned length, const String& name, NativeFunction, Kind = Kind::Function, JSObject* owner = nullptr, unsigned data = 0);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    Kind kind() const { return m_kind; }
    JSObject* owner() const { return m_owner.get(); }
    unsigned data() const { return m_data; }

private:
    PyNativeFunction(VM& vm, NativeExecutable* executable, JSGlobalObject* globalObject, Structure* structure, Kind kind, JSObject* owner, unsigned data)
        : Base(vm, executable, globalObject, structure)
        , m_kind(kind)
        , m_data(data)
        , m_owner(owner, WriteBarrierEarlyInit)
    {
    }

    Kind m_kind;
    unsigned m_data;
    WriteBarrier<JSObject> m_owner;
};

} // namespace JSC
