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

namespace Python {
class NativeSignature;
struct BuiltinDescription;
}

// A function of Python's that is written in C++.
//
// Its arguments are its Python arguments. If it was given keywords, their values come after the positional arguments and `this` is
// their names, in a JSCellButterfly, which is nothing that Python or JavaScript code can get hold of. Python::NativeArguments reads them.
//
// It has a signature, which its arguments are checked against before it is called: see PythonSignatures.h.
class PyNativeFunction final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    enum class Kind : uint8_t {
        Function, // len. As an attribute of a class it is what it is.
        Method, // list.append. Got from an instance it is bound to it, and its first argument is the instance.
        Wrapper, // list.__add__. The same, and one of those that an operator or a built-in function comes down to. It differs in what Python calls it.
        ClassMethod, // dict.fromkeys. It is bound to the class, and its first argument is the class.
        New, // list.__new__. Like Function, and its first argument is a class that is the owner or derived from it.
        StaticMethod, // bytes.maketrans. Like Function, but for being in a class, where it is inside a staticmethod.
    };

    enum class Arguments : uint8_t {
        AreChecked,
        // A few of CPython's have a signature for the sake of saying something, and take whatever they are given.
        AreNotChecked,
        // A __new__ or an __init__ says that it takes anything. The one of the two that does something with what a class is called with takes what
        // the class says that it is called with, and goes by the name of the class when that is not what it is given.
        AreThoseOfTheClass,
        // The same, for one that has something of its own to say first. It calls Python::checkArgumentsSlow() when it has.
        AreThoseOfTheClassButNotChecked,
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
    // `signature` is for what CPython does not have. What it has is looked up by its owner and its name, and has CPython's. A Method that is a
    // slot wrapper in CPython is made a Wrapper.
    JS_EXPORT_PRIVATE static PyNativeFunction* create(VM&, JSGlobalObject*, unsigned length, const String& name, NativeFunction, Kind = Kind::Function, JSObject* owner = nullptr, unsigned data = 0, ImplementationVisibility = ImplementationVisibility::Public, ASCIILiteral signature = { }, Arguments = Arguments::AreChecked);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    Kind kind() const { return m_kind; }
    // Whether its first argument is an instance or a class that whoever calls it does not think of as an argument.
    bool hasImplicitFirst() const { return m_kind != Kind::Function && m_kind != Kind::StaticMethod; }
    JSObject* owner() const { return m_owner.get(); }
    unsigned data() const { return m_data; }

    // Null if it has none.
    const Python::NativeSignature* signature() const { return m_signature; }
    bool checksArguments() const { return m_checksArguments; }
    bool takesArgumentsOfTheClass() const { return m_takesArgumentsOfTheClass; }
    // Null if CPython has no such function.
    const Python::BuiltinDescription* description() const { return m_description; }
    // Whether so many arguments, none of them given by name, are as many as it takes. They include the instance or the class that comes first.
    bool takes(unsigned count) const { return count >= m_minimumArguments && count <= m_maximumArguments; }

private:
    PyNativeFunction(VM& vm, NativeExecutable* executable, JSGlobalObject* globalObject, Structure* structure, Kind kind, JSObject* owner, unsigned data)
        : Base(vm, executable, globalObject, structure)
        , m_kind(kind)
        , m_data(data)
        , m_owner(owner, WriteBarrierEarlyInit)
    {
    }

    void setSignature(const Python::NativeSignature*, Arguments);

    Kind m_kind;
    bool m_checksArguments { false };
    bool m_takesArgumentsOfTheClass { false };
    unsigned m_data;
    unsigned m_minimumArguments { 0 };
    unsigned m_maximumArguments { std::numeric_limits<unsigned>::max() };
    const Python::NativeSignature* m_signature { nullptr };
    const Python::BuiltinDescription* m_description { nullptr };
    WriteBarrier<JSObject> m_owner;
};

} // namespace JSC
