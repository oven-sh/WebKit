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
#include "PythonPickle.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonImport.h"
#include "PythonOperations.h"

// What the two halves of _pickle have in common, and the module. See PythonPickle.h.

namespace JSC { namespace Python {

PickleModuleState& pickleModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<PickleModuleState>(); }

void appendDottedPath(JSGlobalObject* globalObject, JSValue name, MarkedArgumentBuffer& path)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = stringIn(name);
    if (!string) {
        raiseTypeError(globalObject, scope, concatenate("must be str, not "_s, typeName(globalObject, name)));
        return;
    }
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    size_t start = 0;
    while (true) {
        size_t dot = text.find('.', start);
        // All of it is what was given.
        if (dot == notFound && !start)
            path.append(string);
        else
            path.append(jsString(vm, text.substring(start, dot == notFound ? text.length() - start : dot - start)));
        if (dot == notFound)
            return;
        start = dot + 1;
    }
}

JSValue getAttributeByPath(JSGlobalObject* globalObject, JSValue object, const MarkedArgumentBuffer& names, bool raises)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    for (size_t i = 0; i < names.size(); ++i) {
        auto name = asString(names.at(i))->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        object = raises ? getAttribute(globalObject, object, name) : getAttributeIfPresent(globalObject, object, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!object)
            return { };
    }
    return object;
}

// _Pickle_InitState()
static void initializeState(JSGlobalObject* globalObject, PickleModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue getattr = getBuiltin(globalObject, "getattr"_s);
    RETURN_IF_EXCEPTION(scope, void());
    state.getattr.set(vm, realm, getattr);

    auto takeDict = [&] (WriteBarrier<PyDict>& slot, JSValue module, ASCIILiteral moduleName, ASCIILiteral name) {
        JSValue value = getAttribute(globalObject, module, Identifier::fromString(vm, name));
        RETURN_IF_EXCEPTION(scope, void());
        if (!isExactly(globalObject, value, realm->typeDict())) {
            raise(globalObject, scope, BuiltinType::RuntimeError, concatenate(moduleName, '.', name, " should be a dict, not "_s, typeName(globalObject, value)));
            return;
        }
        slot.set(vm, realm, asDict(value));
    };
    JSValue copyreg = importModule(globalObject, "copyreg"_s);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto [slot, name] : { std::pair { &state.dispatchTable, "dispatch_table"_s }, std::pair { &state.extensionRegistry, "_extension_registry"_s }, std::pair { &state.invertedRegistry, "_inverted_registry"_s },
        std::pair { &state.extensionCache, "_extension_cache"_s } }) {
        takeDict(*slot, copyreg, "copyreg"_s, name);
        RETURN_IF_EXCEPTION(scope, void());
    }
    JSValue compatibility = importModule(globalObject, "_compat_pickle"_s);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto [slot, name] : { std::pair { &state.nameMapping2To3, "NAME_MAPPING"_s }, std::pair { &state.importMapping2To3, "IMPORT_MAPPING"_s }, std::pair { &state.nameMapping3To2, "REVERSE_NAME_MAPPING"_s },
        std::pair { &state.importMapping3To2, "REVERSE_IMPORT_MAPPING"_s } }) {
        takeDict(*slot, compatibility, "_compat_pickle"_s, name);
        RETURN_IF_EXCEPTION(scope, void());
    }

    JSValue encode = importModuleAttribute(globalObject, "codecs"_s, "encode"_s);
    RETURN_IF_EXCEPTION(scope, void());
    if (!isCallable(globalObject, encode)) {
        raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("codecs.encode should be a callable, not "_s, typeName(globalObject, encode)));
        return;
    }
    state.codecsEncode.set(vm, realm, encode);
    JSValue partial = importModuleAttribute(globalObject, "functools"_s, "partial"_s);
    RETURN_IF_EXCEPTION(scope, void());
    state.partial.set(vm, realm, partial);
}

// _pickle_exec()
JSObject* createPickleModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = pickleModuleState(globalObject);
    if (!state.picklerType) {
        initializePickleBufferType(globalObject);
        initializePickler(globalObject);
        initializeUnpickler(globalObject);
        PyType* pickleError = newException(globalObject, "_pickle"_s, "PickleError"_s, realm->type(BuiltinType::Exception));
        RETURN_IF_EXCEPTION(scope, nullptr);
        PyType* picklingError = newException(globalObject, "_pickle"_s, "PicklingError"_s, pickleError);
        RETURN_IF_EXCEPTION(scope, nullptr);
        PyType* unpicklingError = newException(globalObject, "_pickle"_s, "UnpicklingError"_s, pickleError);
        RETURN_IF_EXCEPTION(scope, nullptr);
        state.pickleError.set(vm, realm, pickleError);
        state.picklingError.set(vm, realm, picklingError);
        state.unpicklingError.set(vm, realm, unpicklingError);
    }

    JSObject* module = newBuiltinModule(globalObject, "_pickle"_s);
    addPicklerFunctions(globalObject, module);
    addUnpicklerFunctions(globalObject, module);
    auto add = [&] (ASCIILiteral name, PyType* type) { module->putDirect(vm, Identifier::fromString(vm, name), type->object()); };
    add("PickleBuffer"_s, state.pickleBufferType.get());
    add("Pickler"_s, state.picklerType.get());
    add("Unpickler"_s, state.unpicklerType.get());
    add("PickleError"_s, state.pickleError.get());
    add("PicklingError"_s, state.picklingError.get());
    add("UnpicklingError"_s, state.unpicklingError.get());

    initializeState(globalObject, state);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return module;
}

} } // namespace JSC::Python
