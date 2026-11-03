/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyCodeCache.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"

namespace JSC {
namespace Python {

// The module _imp: what importlib needs of the interpreter. (Python/import.c in CPython is the reference.)

// ---- _imp ----

static JSString* impName(JSGlobalObject* globalObject, JSValue name, ASCIILiteral function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(name)) {
        raiseTypeError(globalObject, scope, function, "() argument must be str, not "_s, typeNameOfArgument(globalObject, name));
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, internString(globalObject, strValue(name)));
}

// 1: a module written in C++. -1: one that cannot be made a second time.
static JSValue impIsBuiltin(JSGlobalObject* globalObject, JSValue nameValue)
{
    JSString* name = impName(globalObject, nameValue, "is_builtin"_s);
    if (!name)
        return { };
    String text = stringOf(name);
    if (text == "sys"_s || text == "builtins"_s)
        return jsNumber(-1);
    return jsNumber(findNativeModule(text) ? 1 : 0);
}

static JSValue impCreateBuiltin(JSGlobalObject* globalObject, JSValue spec)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue nameValue = getAttr(globalObject, spec, rt.names.name());
    RETURN_IF_EXCEPTION(scope, { });
    JSString* name = impName(globalObject, nameValue, "create_builtin"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String text = stringOf(name);
    if (text == "sys"_s)
        return rt.sysModule();
    if (text == "builtins"_s)
        return rt.builtinsModule();
    auto* def = findNativeModule(text);
    if (!def)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, nativeModuleInstance(globalObject, name, *def));
}

static JSValue impExecBuiltin(JSGlobalObject*, JSValue) { return jsNumber(0); }
static JSValue impFalse(JSGlobalObject*, JSValue) { return jsBoolean(false); }

static JSValue impLockHeld(JSGlobalObject* globalObject) { return jsBoolean(runtime(globalObject).threads().importLock.isHeld()); }

static JSValue impAcquireLock(JSGlobalObject* globalObject)
{
    if (!runtime(globalObject).threads().importLock.acquire(globalObject))
        return { };
    return jsUndefined();
}

static JSValue impReleaseLock(JSGlobalObject* globalObject)
{
    auto& lock = runtime(globalObject).threads().importLock;
    if (!lock.isHeldByThisThread(globalObject)) {
        auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
        return raiseRuntimeError(globalObject, scope, "not holding the import lock"_s);
    }
    lock.release(globalObject);
    return jsUndefined();
}

static JSValue impEmptyList(JSGlobalObject* globalObject) { return newList(globalObject); }
static JSValue impFindFrozen(JSGlobalObject*, JSValue, JSValue) { return jsUndefined(); }

static JSValue impFixCoFilename(JSGlobalObject* globalObject, JSValue codeValue, JSValue path)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* code = PyCode::tryFrom(codeValue);
    if (!code)
        return raiseTypeError(globalObject, scope, "_fix_co_filename() argument 1 must be code, not "_s, typeNameOfArgument(globalObject, codeValue));
    if (!isStr(path))
        return raiseTypeError(globalObject, scope, "_fix_co_filename() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, path));
    String text = strValue(path)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    setCodeFilename(globalObject, code, text);
    return jsUndefined();
}

static JSValue impSetLazyAttributes(JSGlobalObject* globalObject, JSValue, JSValue nameValue)
{
    JSString* name = impName(globalObject, nameValue, "_set_lazy_attributes"_s);
    if (!name)
        return { };
    didImportLazyModule(globalObject, name);
    return jsUndefined();
}
static JSValue impOverride(JSGlobalObject*, JSValue) { return jsNumber(0); }

static JSValue impNoFrozen(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseImportError(globalObject, scope, "No such frozen object named "_s, args.size() ? reprForMessage(globalObject, args[0]) : "?"_str);
}

static JSValue impNoDynamic(JSGlobalObject* globalObject, const CallArgs&)
{
    // FIXME: Extension modules, through the stable ABI.
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseImportError(globalObject, scope, "extension modules cannot be loaded"_s);
}

static JSValue impSourceHash(JSGlobalObject* globalObject, JSValue key, JSValue source)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t seed = toInt64(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    std::span<const uint8_t> bytes;
    if (!getBytesLike(globalObject, source, bytes))
        return { };
    uint64_t hash = hashOfSource(static_cast<uint64_t>(seed), bytes);
    uint8_t result[8];
    for (unsigned i = 0; i < 8; ++i)
        result[i] = static_cast<uint8_t>(hash >> (8 * i));
    return PyBytes::create(globalObject, std::span<const uint8_t> { result, 8 });
}

static constexpr NativeDef impFunctions[] = {
    PY_FUNCTION("acquire_lock", impAcquireLock, "()"),
    PY_FUNCTION("release_lock", impReleaseLock, "()"),
    PY_FUNCTION("lock_held", impLockHeld, "()"),
    PY_FUNCTION("is_builtin", impIsBuiltin, "(name, /)"),
    PY_FUNCTION("create_builtin", impCreateBuiltin, "(spec, /)"),
    PY_FUNCTION("exec_builtin", impExecBuiltin, "(mod, /)"),
    PY_FUNCTION("is_frozen", impFalse, "(name, /)"),
    PY_FUNCTION("is_frozen_package", impNoFrozen, "(*args)"),
    PY_FUNCTION("get_frozen_object", impNoFrozen, "(*args)"),
    PY_FUNCTION("init_frozen", impNoFrozen, "(*args)"),
    PY_FUNCTION("find_frozen", impFindFrozen, "(name, /, *, withdata=False)"),
    PY_FUNCTION("_frozen_module_names", impEmptyList, "()"),
    PY_FUNCTION("create_dynamic", impNoDynamic, "(*args)"),
    PY_FUNCTION("exec_dynamic", impNoDynamic, "(*args)"),
    PY_FUNCTION("extension_suffixes", impEmptyList, "()"),
    PY_FUNCTION("source_hash", impSourceHash, "(key, source)"),
    PY_FUNCTION("_fix_co_filename", impFixCoFilename, "(code, path, /)"),
    PY_FUNCTION("_set_lazy_attributes", impSetLazyAttributes, "(modobj, name, /)"),
    PY_FUNCTION("_override_frozen_modules_for_tests", impOverride, "(override, /)"),
    PY_FUNCTION("_override_multi_interp_extensions_check", impOverride, "(override, /)"),
};

void initializeImpModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, impFunctions);
    module->add(globalObject, "check_hash_based_pycs"_s, makeStr(getVM(globalObject), configuration().checkHashPycsMode));
    module->add(globalObject, "pyc_magic_number_token"_s, jsNumber(pycMagicNumber()));
}

} // namespace Python
} // namespace JSC
