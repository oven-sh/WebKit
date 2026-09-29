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
#include "PythonLifecycle.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonCodecs.h"
#include "PythonCompiler.h"
#include "PythonConfiguration.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "TopExceptionScope.h"

// What is done before a program is run, running one, and what is done afterwards: of CPython's Python/pylifecycle.c, Python/pythonrun.c and Modules/main.c, what is not to do with there being one interpreter to a
// process, or with the command line, which is the host's.

namespace JSC { namespace Python {

static void setSysAttribute(JSGlobalObject* globalObject, ASCIILiteral name, JSValue value)
{
    VM& vm = globalObject->vm();
    putStoredAttribute(vm, globalObject->pyRealm()->sysModule(), Identifier::fromString(vm, name), value);
}

// ---- Before

// config_get_codec_name(): what Python calls an encoding. Null if it raised.
static String codecNameOf(JSGlobalObject* globalObject, const String& encoding)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue codec = lookupCodec(globalObject, encoding);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name = getAttribute(globalObject, codec, vm.pythonNames().attribute_name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, str(globalObject, name));
}

// _Py_IsValidFD()
static bool isValidDescriptor(JSGlobalObject* globalObject, int descriptor)
{
    const FileOperations* files = fileOperations(globalObject);
    FileStatus status;
    return files && files->status(descriptor, status) >= 0;
}

// create_stdio(): None if there is nothing open there. Empty if it raised.
static JSValue createStandardStream(JSGlobalObject* globalObject, int descriptor, bool isForWriting, ASCIILiteral name, const String& encoding, const String& errors)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!isValidDescriptor(globalObject, descriptor))
        return jsUndefined();
    bool isBuffered = globalObject->pyRealm()->configuration().buffersStandardStreams;

    auto create = [&] () -> JSValue {
        // What is read is buffered whatever is asked for: it should make no difference, and TextIOWrapper wants read1(), which only what is buffered has.
        int buffering = !isBuffered && isForWriting ? 0 : -1;
        JSValue buffer = openFile(globalObject, jsNumber(descriptor), isForWriting ? "wb"_s : "rb"_s, buffering, String(), String(), String(), false, jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
        JSValue raw = buffer;
        if (buffering) {
            raw = getAttribute(globalObject, buffer, names.attribute_raw);
            RETURN_IF_EXCEPTION(scope, { });
        }
        setAttribute(globalObject, raw, names.attribute_name, jsString(vm, String(name)));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue answer = callMethodNamed(globalObject, raw, names.attribute_isatty);
        RETURN_IF_EXCEPTION(scope, { });
        bool isTerminal = isTrue(globalObject, answer);
        RETURN_IF_EXCEPTION(scope, { });

        // Lines end at "\n" in what is read, and "\n" is written as it is.
        MarkedArgumentBuffer arguments;
        arguments.append(buffer);
        arguments.append(jsString(vm, encoding));
        arguments.append(jsString(vm, errors));
        arguments.append(jsString(vm, String("\n"_s)));
        arguments.append(jsBoolean(isBuffered && (isTerminal || descriptor == 2)));
        arguments.append(jsBoolean(!isBuffered));
        JSValue stream = call(globalObject, ioState(globalObject).textIOWrapper.get(), arguments);
        RETURN_IF_EXCEPTION(scope, { });
        setAttribute(globalObject, stream, names.attribute_mode, jsString(vm, String(isForWriting ? "w"_s : "r"_s)));
        RETURN_IF_EXCEPTION(scope, { });
        return stream;
    };
    JSValue stream = create();
    if (scope.exception()) [[unlikely]] {
        // It may have been closed since it was looked at.
        if (!isValidDescriptor(globalObject, descriptor) && catchException(globalObject, BuiltinType::OSError))
            return jsUndefined();
        return { };
    }
    return stream;
}

// init_sys_streams()
static void initializeStandardStreams(JSGlobalObject* globalObject, const String& encoding)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& configuration = globalObject->pyRealm()->configuration();
    static constexpr uint32_t typeMask = 0170000;
    static constexpr uint32_t directoryType = 0040000;
    FileStatus status;
    if (const FileOperations* files = fileOperations(globalObject); files && files->status(0, status) >= 0 && (status.mode & typeMask) == directoryType) {
        raise(globalObject, scope, BuiltinType::RuntimeError, "<stdin> is a directory, cannot continue"_s);
        return;
    }
    struct Stream {
        int descriptor;
        ASCIILiteral attribute;
        ASCIILiteral original;
        ASCIILiteral name;
    };
    for (auto& stream : { Stream { 0, "stdin"_s, "__stdin__"_s, "<stdin>"_s }, Stream { 1, "stdout"_s, "__stdout__"_s, "<stdout>"_s }, Stream { 2, "stderr"_s, "__stderr__"_s, "<stderr>"_s } }) {
        JSValue file = createStandardStream(globalObject, stream.descriptor, !!stream.descriptor, stream.name, encoding, stream.descriptor == 2 ? "backslashreplace"_str : configuration.standardStreamErrors);
        RETURN_IF_EXCEPTION(scope, void());
        setSysAttribute(globalObject, stream.original, file);
        setSysAttribute(globalObject, stream.attribute, file);
    }
}

// add_main_module()
static void addMainModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = addModule(globalObject, jsNontrivialString(vm, "__main__"_s));
    RETURN_IF_EXCEPTION(scope, void());
    Identifier builtins = Identifier::fromString(vm, "__builtins__"_s);
    if (!getStoredAttribute(vm, module, builtins))
        putStoredAttribute(vm, module, builtins, realm->builtinsModule());
    // For want of anything better. If it comes to be known where it is from, it is given a loader that says so.
    JSValue loader = getStoredAttribute(vm, module, vm.pythonNames().dunder_loader);
    if (!loader || isNone(loader)) {
        loader = getAttribute(globalObject, importState(globalObject).importlib.get(), Identifier::fromString(vm, "BuiltinImporter"_s));
        RETURN_IF_EXCEPTION(scope, void());
        putStoredAttribute(vm, module, vm.pythonNames().dunder_loader, loader);
    }
}

void startPython(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = importState(globalObject);
    if (state.isStarted)
        return;
    state.isStarted = true;
    auto& configuration = realm->configuration();

    initializeExternalImport(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    // _PyUnicode_InitEncodings(). Importing `encodings` registers what finds a codec by name: _PyCodec_InitRegistry()
    importModule(globalObject, "encodings"_s);
    RETURN_IF_EXCEPTION(scope, void());
    codecNameOf(globalObject, "utf-8"_s);
    RETURN_IF_EXCEPTION(scope, void());
    String streamEncoding = codecNameOf(globalObject, configuration.standardStreamEncoding);
    RETURN_IF_EXCEPTION(scope, void());

    initializeStandardStreams(globalObject, streamEncoding);
    RETURN_IF_EXCEPTION(scope, void());

    // init_set_builtins_open()
    JSValue open = importModuleAttribute(globalObject, "_io"_s, "open"_s);
    RETURN_IF_EXCEPTION(scope, void());
    setAttribute(globalObject, realm->builtinsModule(), Identifier::fromString(vm, "open"_s), open);
    RETURN_IF_EXCEPTION(scope, void());

    addMainModule(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    JSValue options = sysAttribute(globalObject, "warnoptions"_s);
    if (options && isInstance(globalObject, options, realm->typeList()) && asList(options)->length()) {
        importModule(globalObject, "warnings"_s);
        RETURN_IF_EXCEPTION(scope, void());
    }

    if (configuration.importsSite) {
        importModule(globalObject, "site"_s);
        RETURN_IF_EXCEPTION(scope, void());
    }

    // It goes at the front once `site` has been over the rest: pymain_sys_path_add_path0()
    if (!configuration.firstSearchPath.isNull()) {
        JSValue path = sysAttribute(globalObject, "path"_s);
        if (!path) {
            raise(globalObject, scope, BuiltinType::RuntimeError, "unable to get sys.path"_s);
            return;
        }
        scope.release();
        callMethodNamed(globalObject, path, Identifier::fromString(vm, "insert"_s), jsNumber(0), jsString(vm, configuration.firstSearchPath));
    }
}

// ---- Running a program

// flush_io()
static void flushStandardStreams(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* raised = scope.exception() ? takeRaisedException(vm) : nullptr;
    RETURN_IF_EXCEPTION(scope, void());
    for (ASCIILiteral name : { "stderr"_s, "stdout"_s }) {
        if (JSValue file = sysAttribute(globalObject, name)) {
            callMethodNamed(globalObject, file, vm.pythonNames().attribute_flush);
            if (scope.exception() && !scope.tryClearException())
                return;
        }
    }
    if (raised)
        throwException(globalObject, scope, raised);
}

// _Py_HandleSystemExitAndKeyboardInterrupt(): whether the exception was SystemExit, and so the program has ended with that status.
static bool handleSystemExit(JSGlobalObject* globalObject, JSValue exception, int& status)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (!isInstance(globalObject, exception, realm->typeSystemExit()))
        return false;
    JSValue toPrint = exception;
    JSValue code = getAttribute(globalObject, exception, Identifier::fromString(vm, "code"_s));
    if (scope.exception())
        scope.clearException();
    else {
        // parse_exit_code()
        if (isNone(code)) {
            status = 0;
            return true;
        }
        if (isInstance(globalObject, code, realm->typeInt())) {
            auto number = tryInt64(toInt(globalObject, code));
            scope.clearException();
            status = number ? static_cast<int>(*number) : -1;
            return true;
        }
        toPrint = code;
    }
    // What is neither is a message.
    String message = str(globalObject, toPrint);
    if (scope.exception()) {
        scope.clearException();
        message = emptyString();
    }
    JSValue file = sysAttribute(globalObject, "stderr"_s);
    if (file && !isNone(file)) {
        for (const String& text : { message, "\n"_str }) {
            callMethodNamed(globalObject, file, vm.pythonNames().attribute_write, jsString(vm, text));
            scope.clearException();
        }
    }
    status = 1;
    return true;
}

// pymain_exit_err_print(), of the exception that has been raised.
static int printExceptionAndGetStatus(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue value = scope.exception()->value();
    scope.clearException();
    int status = 1;
    if (handleSystemExit(globalObject, value, status))
        return status;
    reportUncaughtException(globalObject, value);
    return 1;
}

// _PyRun_SimpleFile()
static void runInMainModule(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const SourceOrigin& origin, const String& filename)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSObject* module = addModule(globalObject, jsNontrivialString(vm, "__main__"_s));
    RETURN_IF_EXCEPTION(scope, void());
    Identifier cached = Identifier::fromString(vm, "__cached__"_s);
    bool setsFileName = !getStoredAttribute(vm, module, names.dunder_file);
    if (setsFileName) {
        putStoredAttribute(vm, module, names.dunder_file, jsString(vm, filename));
        putStoredAttribute(vm, module, cached, jsUndefined());
    }
    auto run = [&] {
        // set_main_loader()
        JSValue external = getAttribute(globalObject, importState(globalObject).importlib.get(), Identifier::fromString(vm, "_bootstrap_external"_s));
        RETURN_IF_EXCEPTION(scope, void());
        JSValue loader = callMethodNamed(globalObject, external, Identifier::fromString(vm, "SourceFileLoader"_s), jsNontrivialString(vm, "__main__"_s), jsString(vm, filename));
        RETURN_IF_EXCEPTION(scope, void());
        putStoredAttribute(vm, module, names.dunder_loader, loader);

        SourceCode source = makeSource(globalObject, bytes, origin, filename);
        RETURN_IF_EXCEPTION(scope, void());
        JSFunction* function = compileModule(globalObject, source, module);
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        call(globalObject, function);
    };
    run();
    flushStandardStreams(globalObject);
    if (setsFileName) {
        Exception* raised = scope.exception() ? takeRaisedException(vm) : nullptr;
        RETURN_IF_EXCEPTION(scope, void());
        auto* dict = PyDict::backedBy(globalObject, module);
        dict->remove(globalObject, jsString(vm, names.dunder_file.string()));
        dict->remove(globalObject, jsString(vm, cached.string()));
        if (raised)
            throwException(globalObject, scope, raised);
    }
}

int runMain(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const SourceOrigin& origin, const String& filename)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    startPython(globalObject);
    if (scope.exception()) {
        // What CPython says of this it says of a process that it is about to end.
        dataLogLn("Fatal Python error: Python could not be started");
        return printExceptionAndGetStatus(globalObject);
    }
    if (!audit(globalObject, "cpython.run_file"_s, jsString(vm, filename)))
        return printExceptionAndGetStatus(globalObject);
    runInMainModule(globalObject, bytes, origin, filename);
    if (scope.exception())
        return printExceptionAndGetStatus(globalObject);
    return 0;
}

// ---- Afterwards

// file_is_closed()
static bool isFileClosed(JSGlobalObject* globalObject, JSValue file)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue closed = getAttribute(globalObject, file, vm.pythonNames().attribute_closed);
    if (scope.exception()) {
        scope.clearException();
        return false;
    }
    bool result = isTrue(globalObject, closed);
    if (scope.exception()) {
        scope.clearException();
        return false;
    }
    return result;
}

bool finalizePython(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!importState(globalObject).isStarted)
        return true;

    // wait_for_thread_shutdown()
    JSValue threading = getImportedModule(globalObject, jsNontrivialString(vm, "threading"_s));
    if (!scope.exception() && threading)
        callMethodNamed(globalObject, threading, Identifier::fromString(vm, "_shutdown"_s));
    if (scope.exception())
        reportUnraisable(globalObject, "Exception ignored on threading shutdown"_s);

    callAtExitFunctions(globalObject);

    // flush_std_files()
    bool succeeded = true;
    for (ASCIILiteral name : { "stdout"_s, "stderr"_s }) {
        JSValue file = sysAttribute(globalObject, name);
        if (!file || isNone(file) || isFileClosed(globalObject, file))
            continue;
        callMethodNamed(globalObject, file, vm.pythonNames().attribute_flush);
        if (!scope.exception())
            continue;
        succeeded = false;
        if (name == "stdout"_s)
            reportUnraisable(globalObject, "Exception ignored while flushing sys.stdout"_s);
        else
            scope.clearException();
    }
    return succeeded;
}

} } // namespace JSC::Python
