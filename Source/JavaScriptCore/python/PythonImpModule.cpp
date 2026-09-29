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
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "SourceProvider.h"

// The module _imp: the end of Python/import.c of CPython.

namespace JSC { namespace Python {

// A name, which clinic has as `unicode`. Empty if it raised.
static JSValue nameArgument(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, ASCIILiteral function, ASCIILiteral argument = "argument"_s)
{
    if (stringIn(value))
        return value;
    return raiseTypeError(globalObject, scope, concatenate(function, "() "_s, argument, " must be str, not "_s, typeNameOfArgument(globalObject, value)));
}

PYTHON_NATIVE(impLockHeld)
{
    return JSValue::encode(jsBoolean(importState(globalObject).lockDepth));
}

PYTHON_NATIVE(impAcquireLock)
{
    ++importState(globalObject).lockDepth;
    RETURN_NONE();
}

PYTHON_NATIVE(impReleaseLock)
{
    NATIVE_PROLOGUE();
    auto& state = importState(globalObject);
    if (!state.lockDepth)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "not holding the import lock"_s));
    --state.lockDepth;
    RETURN_NONE();
}

// _fix_co_filename(code, path)
PYTHON_NATIVE(impFixCodeFilename)
{
    NATIVE_PROLOGUE();
    if (typeOf(globalObject, args[0]) != realm->typeCode())
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_fix_co_filename() argument 1 must be code, not "_s, typeNameOfArgument(globalObject, args[0]))));
    JSValue path = nameArgument(globalObject, scope, args[1], "_fix_co_filename"_s, "argument 2"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String text = stringIn(path)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    // update_compiled_module() gives the name to the code and to all the code in it that has the name that it had. The name of the file goes with the source here, and all of that is from the one source.
    SourceProvider* provider = executableOfCode(args[0])->source().provider();
    if (provider->sourceURL() != text)
        provider->setSourceURL(text);
    RETURN_NONE();
}

PYTHON_NATIVE(impCreateBuiltin)
{
    NATIVE_PROLOGUE();
    JSValue name = getAttribute(globalObject, args[0], names.attribute_name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(name))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("name must be string, not "_s, typeName(globalObject, name))));
    RELEASE_AND_RETURN(scope, JSValue::encode(createBuiltinModule(globalObject, name)));
}

// All that there is to making one has been done by the time that it has been made.
PYTHON_NATIVE(impExecBuiltin)
{
    return JSValue::encode(jsNumber(0));
}

// There is no loading what is written in C.
PYTHON_NATIVE(impExtensionSuffixes)
{
    return JSValue::encode(newList(globalObject, MarkedArgumentBuffer()));
}

PYTHON_NATIVE(impInitFrozen)
{
    NATIVE_PROLOGUE();
    JSValue name = nameArgument(globalObject, scope, args[0], "init_frozen"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto wasFound = importFrozenModule(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!*wasFound)
        RETURN_NONE();
    RELEASE_AND_RETURN(scope, JSValue::encode(addModule(globalObject, name)));
}

// find_frozen(name, /, *, withdata=False)
PYTHON_NATIVE(impFindFrozen)
{
    NATIVE_PROLOGUE();
    JSValue name = nameArgument(globalObject, scope, args.at(0), "find_frozen"_s, "argument 1"_s);
    RETURN_IF_EXCEPTION(scope, { });
    bool wantsData = false;
    if (JSValue value = args.at(1)) {
        wantsData = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    FrozenInfo info;
    FrozenStatus status = findFrozen(globalObject, name, info);
    if (status == FrozenStatus::NotFound || status == FrozenStatus::Disabled || status == FrozenStatus::BadName)
        RETURN_NONE();
    if (status != FrozenStatus::Okay) {
        scope.release();
        raiseFrozenError(globalObject, status, name);
        return { };
    }
    JSValue data = jsUndefined();
    if (wantsData) {
        // What get_frozen_object() takes: the code, as marshal writes it.
        FunctionExecutable* code = compileFrozen(globalObject, info);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue bytes = marshalDumps(globalObject, codeObjectFor(globalObject, code));
        RETURN_IF_EXCEPTION(scope, { });
        data = call(globalObject, realm->typeMemoryView(), bytes);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(PyTuple::create(globalObject, { data, jsBoolean(info.module->isPackage), info.originalName.isEmpty() ? jsUndefined() : JSValue(jsString(vm, info.originalName)) }));
}

// get_frozen_object(name, data=None, /)
PYTHON_NATIVE(impGetFrozenObject)
{
    NATIVE_PROLOGUE();
    JSValue name = nameArgument(globalObject, scope, args[0], "get_frozen_object"_s, "argument 1"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue data = args.at(1);
    if (!data || isNone(data)) {
        FrozenInfo info;
        FrozenStatus status = findFrozen(globalObject, name, info);
        if (status != FrozenStatus::Okay) {
            scope.release();
            raiseFrozenError(globalObject, status, name);
            return { };
        }
        FunctionExecutable* code = compileFrozen(globalObject, info);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(codeObjectFor(globalObject, code));
    }
    if (!hasBuffer(globalObject, data))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("get_frozen_object() argument 2 must be bytes, not "_s, typeNameOfArgument(globalObject, data))));
    Buffer buffer = bufferOf(globalObject, data);
    RETURN_IF_EXCEPTION(scope, { });
    auto invalid = [&] {
        scope.release();
        raiseFrozenError(globalObject, FrozenStatus::Invalid, name);
        return EncodedJSValue();
    };
    if (buffer.span().empty())
        return invalid();
    // unmarshal_frozen_code()
    JSValue code = marshalLoads(globalObject, buffer.span());
    if (scope.exception()) {
        if (!scope.tryClearException())
            return { };
        return invalid();
    }
    if (typeOf(globalObject, code) != realm->typeCode()) {
        String shown = repr(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("frozen object "_s, shown, " is not a code object"_s)));
    }
    return JSValue::encode(code);
}

PYTHON_NATIVE(impIsFrozenPackage)
{
    NATIVE_PROLOGUE();
    JSValue name = nameArgument(globalObject, scope, args[0], "is_frozen_package"_s);
    RETURN_IF_EXCEPTION(scope, { });
    FrozenInfo info;
    FrozenStatus status = findFrozen(globalObject, name, info);
    if (status != FrozenStatus::Okay && status != FrozenStatus::Excluded) {
        scope.release();
        raiseFrozenError(globalObject, status, name);
        return { };
    }
    return JSValue::encode(jsBoolean(info.module->isPackage));
}

PYTHON_NATIVE(impIsBuiltin)
{
    NATIVE_PROLOGUE();
    JSValue name = nameArgument(globalObject, scope, args[0], "is_builtin"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String text = stringIn(name)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(isBuiltinModule(globalObject, text)));
}

PYTHON_NATIVE(impIsFrozen)
{
    NATIVE_PROLOGUE();
    JSValue name = nameArgument(globalObject, scope, args[0], "is_frozen"_s);
    RETURN_IF_EXCEPTION(scope, { });
    FrozenInfo info;
    return JSValue::encode(jsBoolean(findFrozen(globalObject, name, info) == FrozenStatus::Okay));
}

PYTHON_NATIVE(impFrozenModuleNames)
{
    return JSValue::encode(frozenModuleNames(globalObject));
}

PYTHON_NATIVE(impOverrideFrozenModules)
{
    NATIVE_PROLOGUE();
    auto override = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    importState(globalObject).overrideOfFrozenModules = *override;
    RETURN_NONE();
}

PYTHON_NATIVE(impOverrideMultiInterpreterCheck)
{
    NATIVE_PROLOGUE();
    toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "_imp._override_multi_interp_extensions_check() cannot be used in the main interpreter"_s));
}

// siphash13() of CPython's Python/pyhash.c
static uint64_t keyedHash(uint64_t key, std::span<const uint8_t> source)
{
    uint64_t v0 = key ^ 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = key ^ 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;
    auto rotate = [] (uint64_t x, unsigned bits) { return (x << bits) | (x >> (64 - bits)); };
    auto halfRound = [&] (uint64_t& a, uint64_t& b, uint64_t& c, uint64_t& d, unsigned s, unsigned t) {
        a += b;
        c += d;
        b = rotate(b, s) ^ a;
        d = rotate(d, t) ^ c;
        a = rotate(a, 32);
    };
    auto round = [&] {
        halfRound(v0, v1, v2, v3, 13, 16);
        halfRound(v2, v1, v0, v3, 17, 21);
    };
    auto littleEndian = [] (std::span<const uint8_t> bytes) {
        uint64_t result = 0;
        for (size_t i = 0; i < bytes.size(); ++i)
            result |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        return result;
    };
    uint64_t last = static_cast<uint64_t>(source.size()) << 56;
    while (source.size() >= 8) {
        uint64_t word = littleEndian(source.first(8));
        source = source.subspan(8);
        v3 ^= word;
        round();
        v0 ^= word;
    }
    last |= littleEndian(source);
    v3 ^= last;
    round();
    v0 ^= last;
    v2 ^= 0xff;
    round();
    round();
    round();
    return (v0 ^ v1) ^ (v2 ^ v3);
}

// source_hash(key, source)
PYTHON_NATIVE(impSourceHash)
{
    NATIVE_PROLOGUE();
    auto key = toCLong(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    Buffer source = bufferOf(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    uint64_t hash = keyedHash(static_cast<uint64_t>(*key), source.span());
    std::array<uint8_t, 8> bytes;
    for (unsigned i = 0; i < 8; ++i)
        bytes[i] = static_cast<uint8_t>(hash >> (8 * i));
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, std::span<const uint8_t>(bytes))));
}

JSObject* createImpModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_imp"_s);
    addFunction(globalObject, module, "extension_suffixes"_s, impExtensionSuffixes);
    addFunction(globalObject, module, "lock_held"_s, impLockHeld);
    addFunction(globalObject, module, "acquire_lock"_s, impAcquireLock);
    addFunction(globalObject, module, "release_lock"_s, impReleaseLock);
    addFunction(globalObject, module, "find_frozen"_s, impFindFrozen);
    addFunction(globalObject, module, "get_frozen_object"_s, impGetFrozenObject);
    addFunction(globalObject, module, "is_frozen_package"_s, impIsFrozenPackage);
    addFunction(globalObject, module, "create_builtin"_s, impCreateBuiltin);
    addFunction(globalObject, module, "init_frozen"_s, impInitFrozen);
    addFunction(globalObject, module, "is_builtin"_s, impIsBuiltin);
    addFunction(globalObject, module, "is_frozen"_s, impIsFrozen);
    addFunction(globalObject, module, "_frozen_module_names"_s, impFrozenModuleNames);
    addFunction(globalObject, module, "_override_frozen_modules_for_tests"_s, impOverrideFrozenModules);
    addFunction(globalObject, module, "_override_multi_interp_extensions_check"_s, impOverrideMultiInterpreterCheck);
    addFunction(globalObject, module, "exec_builtin"_s, impExecBuiltin);
    addFunction(globalObject, module, "_fix_co_filename"_s, impFixCodeFilename);
    addFunction(globalObject, module, "source_hash"_s, impSourceHash);
    module->putDirect(vm, Identifier::fromString(vm, "check_hash_based_pycs"_s), jsNontrivialString(vm, "default"_s));
    module->putDirect(vm, Identifier::fromString(vm, "pyc_magic_number_token"_s), jsNumber(pycMagicNumberToken));
    return module;
}

} } // namespace JSC::Python
