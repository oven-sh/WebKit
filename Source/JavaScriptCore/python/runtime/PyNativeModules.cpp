/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// The modules written in C++, besides builtins and sys, which are always there.

static constexpr NativeDef stringModuleFunctions[] = {
    PY_FUNCTION("formatter_parser", formatterParser, "(format_string, /)"),
    PY_FUNCTION("formatter_field_name_split", formatterFieldNameSplit, "(field_name, /)"),
};

static void initializeStringModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, stringModuleFunctions);
}

// _suggestions: Lib/traceback.py asks it what name may have been meant.
static JSValue generateSuggestions(JSGlobalObject* globalObject, JSValue candidates, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(item))
        return raiseTypeError(globalObject, scope, "_generate_suggestions() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, item));
    if (!isExactList(candidates))
        return raiseTypeError(globalObject, scope, "candidates must be a list"_s);
    JSArray* list = asList(candidates);
    Vector<String> names;
    for (unsigned i = 0; i < listLength(list); ++i) {
        JSValue candidate = listGet(list, i);
        if (!isStr(candidate))
            return raiseTypeError(globalObject, scope, "all elements in 'candidates' must be strings"_s);
        names.append(strValue(candidate)->value(globalObject));
        RETURN_IF_EXCEPTION(scope, { });
    }
    String name = strValue(item)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String suggestion = calculateSuggestion(names, name);
    return suggestion.isNull() ? jsUndefined() : JSValue(makeStr(vm, suggestion));
}

static constexpr NativeDef suggestionsModuleFunctions[] = {
    PY_FUNCTION("_generate_suggestions", generateSuggestions, "(candidates, item, /)"),
};

static void initializeSuggestionsModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, suggestionsModuleFunctions);
}

// _missing_stdlib_info: in CPython a file that the build writes, about the modules that build does not have. Lib/traceback.py
// words a ModuleNotFoundError after it.
static void initializeMissingStdlibInfoModule(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    PyDict* messages = PyDict::create(globalObject);
    [[maybe_unused]] auto add = [&](ASCIILiteral name, ASCIILiteral what) {
        messages->set(vm, internString(vm, name), makeStr(vm, makeString("Unsupported platform for "_s, what, "standard library module '"_s, name, '\'')));
    };
#if !OS(WINDOWS)
    for (auto name : { "_overlapped"_s, "_testconsole"_s, "_winapi"_s, "_wmi"_s, "msvcrt"_s, "nt"_s, "winreg"_s, "winsound"_s })
        add(name, "Windows-only "_s);
#endif
#if !OS(DARWIN)
    add("_scproxy"_s, ""_s);
#endif
    module->add(globalObject, "_MISSING_STDLIB_MODULE_MESSAGES"_s, messages);
}

void initializeAsyncioModule(JSGlobalObject*, PyModule*); // PyAsyncio.cpp
void initializeCodecsCNModule(JSGlobalObject*, PyModule*); // PyCJKCodecs.cpp, as the five that follow
void initializeCodecsHKModule(JSGlobalObject*, PyModule*);
void initializeCodecsISO2022Module(JSGlobalObject*, PyModule*);
void initializeCodecsJPModule(JSGlobalObject*, PyModule*);
void initializeCodecsKRModule(JSGlobalObject*, PyModule*);
void initializeCodecsTWModule(JSGlobalObject*, PyModule*);
void initializeCtypesModule(JSGlobalObject*, PyModule*); // PyCtypes.cpp
void initializeDateTimeModule(JSGlobalObject*, PyModule*); // PyDateTime.cpp
void initializeMultibyteCodecModule(JSGlobalObject*, PyModule*); // PyMultibyteCodec.cpp

static constexpr NativeModuleDef nativeModuleTable[] = {
    { "_abc"_s, initializeAbcModule },
    { "_ast"_s, initializeAstModule },
    { "_asyncio"_s, initializeAsyncioModule },
    { "_bisect"_s, initializeBisectModule },
    { "_codecs"_s, initializeCodecsModule },
    { "_codecs_cn"_s, initializeCodecsCNModule },
    { "_codecs_hk"_s, initializeCodecsHKModule },
    { "_codecs_iso2022"_s, initializeCodecsISO2022Module },
    { "_codecs_jp"_s, initializeCodecsJPModule },
    { "_codecs_kr"_s, initializeCodecsKRModule },
    { "_codecs_tw"_s, initializeCodecsTWModule },
    { "_collections"_s, initializeCollectionsModule },
    { "_contextvars"_s, initializeContextVarsModule },
    { "_csv"_s, initializeCsvModule },
    { "_ctypes"_s, initializeCtypesModule },
    { "_datetime"_s, initializeDateTimeModule },
    { "_functools"_s, initializeFunctoolsModule },
    { "_heapq"_s, initializeHeapqModule },
    { "_imp"_s, initializeImpModule },
    { "_io"_s, initializeIOModule },
    { "_js"_s, initializeJSModule },
    { "_json"_s, initializeJsonModule },
    { "_lsprof"_s, initializeLsprofModule },
    { "_math_integer"_s, initializeMathIntegerModule },
    { "_missing_stdlib_info"_s, initializeMissingStdlibInfoModule },
    { "_multibytecodec"_s, initializeMultibyteCodecModule },
    { "_opcode"_s, initializeOpcodeModule },
    { "_operator"_s, initializeOperatorModule },
    { "_pickle"_s, initializePickleModule },
    { "_queue"_s, initializeQueueModule },
    { "_random"_s, initializeRandomModule },
    { "_sre"_s, initializeSreModule },
    { "_stat"_s, initializeStatModule },
    { "_statistics"_s, initializeStatisticsModule },
    { "_string"_s, initializeStringModule },
    { "_struct"_s, initializeStructModule },
    { "_suggestions"_s, initializeSuggestionsModule },
    { "_symtable"_s, initializeSymtableModule },
    { "_sysconfig"_s, initializeSysconfigModule },
    { "_thread"_s, initializeThreadModule },
    { "_tokenize"_s, initializeTokenizeModule },
    { "_tracemalloc"_s, initializeTracemallocModule },
    { "_types"_s, initializeTypesModule },
    { "_typing"_s, initializeTypingModule },
    { "_warnings"_s, initializeWarningsModule },
    { "_weakref"_s, initializeWeakrefModule },
    { "array"_s, initializeArrayModule },
    { "atexit"_s, initializeAtexitModule },
    { "binascii"_s, initializeBinasciiModule },
    { "cmath"_s, initializeCMathModule },
    { "errno"_s, initializeErrnoModule },
    { "gc"_s, initializeGCModule },
    { "itertools"_s, initializeItertoolsModule },
    { "marshal"_s, initializeMarshalModule },
    { "math"_s, initializeMathModule },
    { "pyexpat"_s, initializeExpatModule },
    { "time"_s, initializeTimeModule },
    { "unicodedata"_s, initializeUnicodeDataModule },
#if !OS(WINDOWS)
    { "_locale"_s, initializeLocaleModule },
    { "_multiprocessing"_s, initializeMultiprocessingModule },
    { "_posixshmem"_s, initializePosixShmemModule },
    { "_posixsubprocess"_s, initializePosixSubprocessModule },
    { "_remote_debugging"_s, initializeRemoteDebuggingModule },
    { "_signal"_s, initializeSignalModule },
    { "_socket"_s, initializeSocketModule },
    // sysconfig: f"_sysconfigdata_{sys.abiflags}_{sys.platform}_{sys.implementation._multiarch}"
#if OS(DARWIN)
    { "_sysconfigdata__darwin_"_s, initializeSysconfigDataModule },
#else
    { "_sysconfigdata__linux_"_s, initializeSysconfigDataModule },
#endif
    { "faulthandler"_s, initializeFaultHandlerModule },
    { "fcntl"_s, initializeFcntlModule },
    { "grp"_s, initializeGrpModule },
    { "mmap"_s, initializeMmapModule },
    { "posix"_s, initializePosixModule },
    { "pwd"_s, initializePwdModule },
    { "resource"_s, initializeResourceModule },
    { "select"_s, initializeSelectModule },
    { "syslog"_s, initializeSyslogModule },
    { "termios"_s, initializeTermiosModule },
#endif
};

std::span<const NativeModuleDef> nativeModules()
{
    return nativeModuleTable;
}

} // namespace Python
} // namespace JSC
