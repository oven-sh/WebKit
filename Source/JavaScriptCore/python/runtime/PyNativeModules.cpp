/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyInternals.h"

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

void initializeAsyncioModule(JSGlobalObject*, PyModule*); // PyAsyncio.cpp

static constexpr NativeModuleDef nativeModuleTable[] = {
    { "_abc"_s, initializeAbcModule },
    { "_ast"_s, initializeAstModule },
    { "_asyncio"_s, initializeAsyncioModule },
    { "_bisect"_s, initializeBisectModule },
    { "_codecs"_s, initializeCodecsModule },
    { "_collections"_s, initializeCollectionsModule },
    { "_contextvars"_s, initializeContextVarsModule },
    { "_csv"_s, initializeCsvModule },
    { "_functools"_s, initializeFunctoolsModule },
    { "_heapq"_s, initializeHeapqModule },
    { "_imp"_s, initializeImpModule },
    { "_io"_s, initializeIOModule },
    { "_js"_s, initializeJSModule },
    { "_json"_s, initializeJsonModule },
    { "_lsprof"_s, initializeLsprofModule },
    { "_math_integer"_s, initializeMathIntegerModule },
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
