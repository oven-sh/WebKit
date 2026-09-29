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


#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PythonOperations.h"

// PyCapsule: Objects/capsule.c of CPython. It is how one module written in C hands another a pointer. Nothing here is handed one that way, but a program can see that a module has one, and what it is called.

namespace JSC { namespace Python {

namespace {

struct CapsuleTypeState final : NativeState {
    PYTHON_NATIVE_STATE(CapsuleTypeState);
    WriteBarrier<PyType> type;
};

template<typename Visitor> void CapsuleTypeState::visit(Visitor& visitor) { visitor.append(type); }

struct CapsuleState final : NativeState {
    PYTHON_NATIVE_STATE(CapsuleState);
    explicit CapsuleState(ASCIILiteral name)
        : name(name)
    {
    }
    ASCIILiteral name;
};

template<typename Visitor> void CapsuleState::visit(Visitor&) { }

} // anonymous namespace

// capsule_repr()
PYTHON_NATIVE(capsuleRepr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<capsule object \""_s, stateOf<CapsuleState>(args[0]).name, "\" at "_s, addressOf(args[0].asCell()), '>'))));
}

JSValue newCapsule(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<CapsuleTypeState>();
    if (!state.type) {
        PyType* type = createBuiltinType(globalObject, "PyCapsule"_s, realm->typeObject(), PyType::Layout::Native, 0);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.type.set(vm, realm, type);
        addMethods(globalObject, type, { { "__repr__"_s, capsuleRepr } });
    }
    return PyStateObject::create(vm, state.type->instanceStructure(), makeUnique<CapsuleState>(name));
}

} } // namespace JSC::Python
