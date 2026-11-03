/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCodeCache.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include "PyVirtualFiles.h"

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
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "name must be string, not "_s, typeName(globalObject, nameValue));
    JSString* name = internString(globalObject, strValue(nameValue));
    RETURN_IF_EXCEPTION(scope, { });
    String text = stringOf(name);
    if (text.isEmpty())
        return raiseValueError(globalObject, scope, "name must not be empty"_s);
    if (text == "sys"_s)
        return rt.sysModule();
    if (text == "builtins"_s)
        return rt.builtinsModule();
    auto* def = findNativeModule(text);
    if (!def) {
        PyException* exception = newException(globalObject, rt.ModuleNotFoundError(), makeString("No module named "_s, strRepr(text)));
        RETURN_IF_EXCEPTION(scope, { });
        exception->setField(vm, ExceptionField::importErrorName, name);
        return raiseException(globalObject, scope, exception);
    }
    RELEASE_AND_RETURN(scope, nativeModuleInstance(globalObject, name, *def));
}

static JSValue impExecBuiltin(JSGlobalObject*, JSValue) { return jsNumber(0); }

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

// ---- Frozen modules ----
//
// CPython has the code of some modules compiled into the executable: the ones the import system is made of, the ones every
// start needs, and a few to test the mechanism with. importlib's FrozenImporter finds and loads them, and it shows: their
// __spec__.origin is "frozen", and the file name in their code, and so in tracebacks, is "<frozen os>". All that is the same here.
// What differs is where the code comes from: it is compiled from the file in the standard library, and kept in the .pyc file
// of that, as for any import, with much less to do on the way. A module whose file is not there is not frozen.

enum class FrozenGroup : uint8_t {
    Bootstrap, // Always.
    Library, // Unless -X frozen_modules=off.
    Test,
};

struct FrozenModule {
    ASCIILiteral name;
    ASCIILiteral file; // In the standard library. Null: `source`.
    ASCIILiteral frozenAs; // The code says it is from "<frozen X>".
    ASCIILiteral originalName; // What find_frozen() reports: see FrozenImporter._resolve_filename(). Null: None.
    bool isPackage { false };
    FrozenGroup group { FrozenGroup::Library };
    ASCIILiteral source { };
};

static constexpr FrozenModule frozenModules[] = {
    { "_frozen_importlib"_s, "importlib/_bootstrap.py"_s, "importlib._bootstrap"_s, "importlib._bootstrap"_s, false, FrozenGroup::Bootstrap },
    { "_frozen_importlib_external"_s, "importlib/_bootstrap_external.py"_s, "importlib._bootstrap_external"_s, "importlib._bootstrap_external"_s, false, FrozenGroup::Bootstrap },
    { "zipimport"_s, "zipimport.py"_s, "zipimport"_s, "zipimport"_s, false, FrozenGroup::Bootstrap },
    { "abc"_s, "abc.py"_s, "abc"_s, "abc"_s },
    { "codecs"_s, "codecs.py"_s, "codecs"_s, "codecs"_s },
    { "io"_s, "io.py"_s, "io"_s, "io"_s },
    { "_collections_abc"_s, "_collections_abc.py"_s, "_collections_abc"_s, "_collections_abc"_s },
    { "_sitebuiltins"_s, "_sitebuiltins.py"_s, "_sitebuiltins"_s, "_sitebuiltins"_s },
    { "genericpath"_s, "genericpath.py"_s, "genericpath"_s, "genericpath"_s },
    { "ntpath"_s, "ntpath.py"_s, "ntpath"_s, "ntpath"_s },
    { "posixpath"_s, "posixpath.py"_s, "posixpath"_s, "posixpath"_s },
    { "os"_s, "os.py"_s, "os"_s, "os"_s },
    { "site"_s, "site.py"_s, "site"_s, "site"_s },
    { "stat"_s, "stat.py"_s, "stat"_s, "stat"_s },
    { "linecache"_s, "linecache.py"_s, "linecache"_s, "linecache"_s },
    { "importlib.util"_s, "importlib/util.py"_s, "importlib.util"_s, "importlib.util"_s },
    { "importlib.machinery"_s, "importlib/machinery.py"_s, "importlib.machinery"_s, "importlib.machinery"_s },
    { "runpy"_s, "runpy.py"_s, "runpy"_s, "runpy"_s },
    { "__hello__"_s, "__hello__.py"_s, "__hello__"_s, "__hello__"_s, false, FrozenGroup::Test },
    { "__hello_alias__"_s, "__hello__.py"_s, "__hello__"_s, "__hello__"_s, false, FrozenGroup::Test },
    { "__phello_alias__"_s, "__hello__.py"_s, "__hello__"_s, "__hello__"_s, true, FrozenGroup::Test },
    { "__phello_alias__.spam"_s, "__hello__.py"_s, "__hello__"_s, "__hello__"_s, false, FrozenGroup::Test },
    { "__phello__"_s, "__phello__/__init__.py"_s, "__phello__"_s, "__phello__"_s, true, FrozenGroup::Test },
    { "__phello__.__init__"_s, "__phello__/__init__.py"_s, "__phello__"_s, "<__phello__"_s, false, FrozenGroup::Test },
    { "__phello__.ham"_s, "__phello__/ham/__init__.py"_s, "__phello__.ham"_s, "__phello__.ham"_s, true, FrozenGroup::Test },
    { "__phello__.ham.__init__"_s, "__phello__/ham/__init__.py"_s, "__phello__.ham"_s, "<__phello__.ham"_s, false, FrozenGroup::Test },
    { "__phello__.ham.eggs"_s, "__phello__/ham/eggs.py"_s, "__phello__.ham.eggs"_s, "__phello__.ham.eggs"_s, false, FrozenGroup::Test },
    { "__phello__.spam"_s, "__phello__/spam.py"_s, "__phello__.spam"_s, "__phello__.spam"_s, false, FrozenGroup::Test },
    // The one that is in no library.
    { "__hello_only__"_s, { }, "frozen_only"_s, { }, false, FrozenGroup::Test, "initialized = True\nprint(\"Hello world!\")\n"_s },
};

String standardLibraryDirectory(); // PySys.cpp
JSValue readMarshalled(JSGlobalObject*, std::span<const uint8_t>); // PyMarshal.cpp

// _imp._override_frozen_modules_for_tests(): above 0 they are on, below 0 off, whatever the configuration says.
static int s_frozenModulesOverride;

static bool usesFrozenModules()
{
    return s_frozenModulesOverride ? s_frozenModulesOverride > 0 : configuration().useFrozenModules;
}

static String fileOfFrozenModule(const FrozenModule& module)
{
    String library = standardLibraryDirectory();
    return library.isNull() ? String() : makeString(library, '/', module.file);
}

static const FrozenModule* findFrozenModule(JSValue name)
{
    if (!isStr(name))
        return nullptr;
    auto* impl = strValue(name)->tryGetValueImpl();
    if (!impl)
        return nullptr;
    StringView text(*impl);
    for (auto& module : frozenModules) {
        if (text != module.name)
            continue;
        if (module.group != FrozenGroup::Bootstrap && !usesFrozenModules())
            return nullptr;
        if (!module.file.isNull() && kindOfFile(fileOfFrozenModule(module)) != VirtualFileKind::File)
            return nullptr;
        return &module;
    }
    return nullptr;
}

static JSValue raiseFrozenError(JSGlobalObject* globalObject, ASCIILiteral before, JSValue name, ASCIILiteral after = ""_s)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = newException(globalObject, rt.ImportError(), makeString(before, reprForMessage(globalObject, name), after));
    RETURN_IF_EXCEPTION(scope, { });
    exception->setField(vm, ExceptionField::importErrorName, name);
    return raiseException(globalObject, scope, exception);
}

static JSValue raiseNoSuchFrozenObject(JSGlobalObject* globalObject, JSValue name)
{
    return raiseFrozenError(globalObject, "No such frozen object named "_s, name);
}

static JSValue codeOfFrozenModule(JSGlobalObject* globalObject, const FrozenModule& module, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String shownFileName = makeString("<frozen "_s, module.frozenAs, '>');
    if (module.file.isNull())
        RELEASE_AND_RETURN(scope, compileSource(globalObject, byteCast<uint8_t>(module.source.span8()), shownFileName, "exec"_s, 0, -1));
    JSValue code = codeOfSourceFile(globalObject, fileOfFrozenModule(module), shownFileName);
    RETURN_IF_EXCEPTION(scope, { });
    if (!code)
        RELEASE_AND_RETURN(scope, raiseNoSuchFrozenObject(globalObject, name));
    return code;
}

static bool checkFrozenName(JSGlobalObject* globalObject, JSValue name, ASCIILiteral function, bool hasMoreArguments = false)
{
    if (isStr(name))
        return true;
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    raiseTypeError(globalObject, scope, function, hasMoreArguments ? "() argument 1 must be str, not "_s : "() argument must be str, not "_s, typeNameOfArgument(globalObject, name));
    return false;
}

static JSValue impFindFrozen(JSGlobalObject* globalObject, JSValue name, JSValue withData)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkFrozenName(globalObject, name, "find_frozen"_s))
        return { };
    bool wantsData = withData && isTrue(globalObject, withData);
    RETURN_IF_EXCEPTION(scope, { });
    auto* module = findFrozenModule(name);
    if (!module)
        return jsUndefined();
    JSValue data = jsUndefined();
    if (wantsData) {
        JSValue code = codeOfFrozenModule(globalObject, *module, name);
        RETURN_IF_EXCEPTION(scope, { });
        Vector<uint8_t> bytes;
        bytes.append(marshalTypeOfCode);
        if (!writeMarshalledCode(globalObject, PyCode::from(code), bytes))
            RELEASE_AND_RETURN(scope, raiseFrozenError(globalObject, "Frozen object named "_s, name, " is invalid"_s));
        data = makeMemoryView(globalObject, PyBytes::create(globalObject, bytes.span()));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return PyTuple::create(globalObject, { data, jsBoolean(module->isPackage), module->originalName.isNull() ? jsUndefined() : JSValue(makeStr(vm, module->originalName)) });
}

static JSValue impGetFrozenObject(JSGlobalObject* globalObject, JSValue name, JSValue data)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkFrozenName(globalObject, name, "get_frozen_object"_s, true))
        return { };
    if (!data || isNone(data)) {
        auto* module = findFrozenModule(name);
        if (!module)
            RELEASE_AND_RETURN(scope, raiseNoSuchFrozenObject(globalObject, name));
        RELEASE_AND_RETURN(scope, codeOfFrozenModule(globalObject, *module, name));
    }
    if (!hasBuffer(data))
        return raiseTypeError(globalObject, scope, "get_frozen_object() argument 2 must be bytes, not "_s, typeNameOfArgument(globalObject, data));
    BufferHolder holder(globalObject);
    if (!holder.get(data))
        return { };
    JSValue code;
    if (!holder.buffer.span().empty()) {
        code = readMarshalled(globalObject, holder.buffer.span());
        if (!code)
            takePendingException(globalObject);
    }
    if (!code)
        RELEASE_AND_RETURN(scope, raiseFrozenError(globalObject, "Frozen object named "_s, name, " is invalid"_s));
    if (!PyCode::is(code))
        return raiseTypeError(globalObject, scope, "frozen object "_s, reprForMessage(globalObject, name), " is not a code object"_s);
    return code;
}

static JSValue impIsFrozen(JSGlobalObject* globalObject, JSValue name)
{
    if (!checkFrozenName(globalObject, name, "is_frozen"_s))
        return { };
    return jsBoolean(!!findFrozenModule(name));
}

static JSValue impIsFrozenPackage(JSGlobalObject* globalObject, JSValue name)
{
    if (!checkFrozenName(globalObject, name, "is_frozen_package"_s))
        return { };
    auto* module = findFrozenModule(name);
    if (!module)
        return raiseNoSuchFrozenObject(globalObject, name);
    return jsBoolean(module->isPackage);
}

static JSValue impFrozenModuleNames(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    JSArray* names = newList(globalObject);
    for (auto& module : frozenModules) {
        if (module.group == FrozenGroup::Bootstrap || usesFrozenModules())
            listAppend(globalObject, names, makeStr(vm, module.name));
    }
    return names;
}

static JSValue impOverrideFrozenModules(JSGlobalObject* globalObject, JSValue override)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t value = toInt64(globalObject, override);
    RETURN_IF_EXCEPTION(scope, { });
    s_frozenModulesOverride = value > 0 ? 1 : value < 0 ? -1 : 0;
    return jsUndefined();
}

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
    PY_FUNCTION("is_frozen", impIsFrozen, "(name, /)"),
    PY_FUNCTION("is_frozen_package", impIsFrozenPackage, "(name, /)"),
    PY_FUNCTION("get_frozen_object", impGetFrozenObject, "(name, data=None, /)"),
    PY_FUNCTION("find_frozen", impFindFrozen, "(name, /, *, withdata=False)"),
    PY_FUNCTION("_frozen_module_names", impFrozenModuleNames, "()"),
    PY_FUNCTION("create_dynamic", impNoDynamic, "(*args)"),
    PY_FUNCTION("exec_dynamic", impNoDynamic, "!(mod, /)"),
    PY_FUNCTION("extension_suffixes", impEmptyList, "()"),
    PY_FUNCTION("source_hash", impSourceHash, "(key, source)"),
    PY_FUNCTION("_fix_co_filename", impFixCoFilename, "(code, path, /)"),
    PY_FUNCTION("_set_lazy_attributes", impSetLazyAttributes, "(modobj, name, /)"),
    PY_FUNCTION("_override_frozen_modules_for_tests", impOverrideFrozenModules, "(override, /)"),
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
