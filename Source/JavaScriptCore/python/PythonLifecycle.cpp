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
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonCodecs.h"
#include "PythonCompiler.h"
#include "PythonConfiguration.h"
#include "PythonGenerators.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "PythonPlatform.h"
#include "PythonPosix.h"
#include "PythonSignals.h"
#include "TopExceptionScope.h"
#include <wtf/URL.h>
#if OS(UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    auto isAbout = [&] (ASCIILiteral function, ASCIILiteral failure) {
        state.startingFunction = function;
        state.startingFailure = failure;
    };

    // _PyConfig_InitImportConfig(), and then _PySys_UpdateConfig()
    isAbout({ }, "error evaluating path"_s);
#if OS(UNIX)
    if (realm->configuration().computesPaths) {
        computePathConfiguration(globalObject, realm->mutableConfiguration());
        RETURN_IF_EXCEPTION(scope, void());
        updateSysFromConfiguration(globalObject, realm->sysModule());
    }
#endif
    auto& configuration = realm->configuration();

    isAbout("init_importlib_external"_s, "external importer setup failed"_s);
    initializeExternalImport(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    // _PyUnicode_InitEncodings(). Importing `encodings` registers what finds a codec by name: _PyCodec_InitRegistry()
    isAbout({ }, "Failed to import encodings module"_s);
    importModule(globalObject, "encodings"_s);
    RETURN_IF_EXCEPTION(scope, void());
    isAbout("init_fs_encoding"_s, "failed to get the Python codec of the filesystem encoding"_s);
    codecNameOf(globalObject, "utf-8"_s);
    RETURN_IF_EXCEPTION(scope, void());
    state.hasCodecRegistry = true;
    isAbout("init_stdio_encoding"_s, "failed to get the Python codec name of the stdio encoding"_s);
    String streamEncoding = codecNameOf(globalObject, configuration.standardStreamEncoding);
    RETURN_IF_EXCEPTION(scope, void());

    isAbout("init_interp_main"_s, "can't initialize signals"_s);
    initializeSignals(globalObject, configuration.installsSignalHandlers);
    RETURN_IF_EXCEPTION(scope, void());

    isAbout("init_sys_streams"_s, "can't initialize sys standard streams"_s);
    initializeStandardStreams(globalObject, streamEncoding);
    RETURN_IF_EXCEPTION(scope, void());

    // init_set_builtins_open()
    isAbout("init_set_builtins_open"_s, "can't initialize io.open"_s);
    JSValue open = importModuleAttribute(globalObject, "_io"_s, "open"_s);
    RETURN_IF_EXCEPTION(scope, void());
    setAttribute(globalObject, realm->builtinsModule(), Identifier::fromString(vm, "open"_s), open);
    RETURN_IF_EXCEPTION(scope, void());

    isAbout("add_main_module"_s, "can't create __main__ module"_s);
    addMainModule(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    JSValue options = sysAttribute(globalObject, "warnoptions"_s);
    if (options && isInstance(globalObject, options, realm->typeList()) && asList(options)->length()) {
        importModule(globalObject, "warnings"_s);
        if (scope.exception()) [[unlikely]] {
            // It is gone on without.
            fputs("'import warnings' failed; traceback:\n", stderr);
            Exception* raised = takeRaisedException(vm);
            RETURN_IF_EXCEPTION(scope, void());
            reportUncaughtException(globalObject, raised->value());
        }
    }

    if (configuration.importsSite) {
        isAbout("init_import_site"_s, "Failed to import the site module"_s);
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

void flushStandardStreams(JSGlobalObject* globalObject)
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

static int printExceptionAndGetStatus(JSGlobalObject*);

// _Py_HandleSystemExitAndKeyboardInterrupt(): whether the exception was SystemExit, and so the program has ended with that status.
static bool handleSystemExit(JSGlobalObject* globalObject, JSValue exception, int& status)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (isInstance(globalObject, exception, realm->type(BuiltinType::KeyboardInterrupt))) {
        importState(globalObject).hasUnhandledKeyboardInterrupt = true;
        return false;
    }
    // With -i it is not the end, until statements are being typed.
    if (realm->configuration().inspect)
        return false;
    if (!isInstance(globalObject, exception, realm->typeSystemExit()))
        return false;
    fflush(stdout);
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
        callMethodNamed(globalObject, file, vm.pythonNames().attribute_write, jsString(vm, message));
        scope.clearException();
    } else {
        CString encoded = message.utf8();
        fputs(encoded.data(), stderr);
        fflush(stderr);
    }
    writeToStandardError(globalObject, "\n"_s);
    status = 1;
    return true;
}

std::optional<int> exitStatusOfSystemExit(JSGlobalObject* globalObject, JSValue thrown)
{
    int status = 0;
    if (!handleSystemExit(globalObject, thrown, status))
        return std::nullopt;
    return status;
}

void printRaisedException(JSGlobalObject* globalObject)
{
    printExceptionAndGetStatus(globalObject);
}

// pymain_exit_err_print(), of the exception that has been raised.
static int printExceptionAndGetStatus(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue value = scope.exception()->value();
    scope.clearException();
    importState(globalObject).mainHasRaised = true;
    int status = 1;
    if (handleSystemExit(globalObject, value, status))
        return status;
    reportUncaughtException(globalObject, value);
    return 1;
}

bool mainHasRaised(JSGlobalObject* globalObject)
{
    return importState(globalObject).mainHasRaised;
}

// Py_ExitStatusException(), and so fatal_error(), of what stopped Python from starting. What CPython says of this it says of a process that it is about to end.
static int reportThatPythonCouldNotStart(JSGlobalObject* globalObject)
{
    auto& state = importState(globalObject);
    fflush(stdout);
    if (state.startingFunction.isNull())
        fprintf(stderr, "Fatal Python error: %s\n", state.startingFailure.characters());
    else
        fprintf(stderr, "Fatal Python error: %s: %s\n", state.startingFunction.characters(), state.startingFailure.characters());
    fputs("Python runtime state: core initialized\n", stderr);
    int status = printExceptionAndGetStatus(globalObject);
    // _Py_FatalError_DumpTracebacks(): nothing of Python's was running.
#if OS(UNIX)
    fprintf(stderr, "\nCurrent thread 0x%016lx (most recent call first):\n  <no Python frame>\n", static_cast<unsigned long>(std::bit_cast<uintptr_t>(pthread_self())));
#endif
    return status;
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
        // maybe_pyc_file()
        bool isCompiled = filename.endsWith(".pyc"_s) || (bytes.size() >= 2 && (bytes[0] | bytes[1] << 8) == (pycMagicNumberToken & 0xFFFF));
        // set_main_loader(). What comes in on the standard input is not from anywhere that anything could load it from.
        if (filename != "<stdin>"_s) {
            JSValue external = getAttribute(globalObject, importState(globalObject).importlib.get(), Identifier::fromString(vm, "_bootstrap_external"_s));
            RETURN_IF_EXCEPTION(scope, void());
            JSValue loader = callMethodNamed(globalObject, external, Identifier::fromString(vm, isCompiled ? "SourcelessFileLoader"_s : "SourceFileLoader"_s), jsNontrivialString(vm, "__main__"_s), jsString(vm, filename));
            RETURN_IF_EXCEPTION(scope, void());
            putStoredAttribute(vm, module, names.dunder_loader, loader);
        }

        if (isCompiled) {
            // run_pyc_file()
            auto wordAt = [&] (size_t offset) { return bytes[offset] | bytes[offset + 1] << 8 | bytes[offset + 2] << 16 | bytes[offset + 3] << 24; };
            if (bytes.size() < 4 || wordAt(0) != pycMagicNumberToken) {
                raise(globalObject, scope, BuiltinType::RuntimeError, "Bad magic number in .pyc file"_s);
                return;
            }
            JSValue code = bytes.size() >= 16 ? marshalLoads(globalObject, bytes.subspan(16)) : JSValue();
            if (scope.exception() && !scope.tryClearException())
                return;
            if (!code || !isCode(globalObject, code)) {
                raise(globalObject, scope, BuiltinType::RuntimeError, "Bad code object in .pyc file"_s);
                return;
            }
            JSValue exec = getStoredAttribute(vm, globalObject->pyRealm()->builtinsModule(), Identifier::fromString(vm, "exec"_s));
            scope.release();
            call(globalObject, exec, code, PyDict::backedBy(globalObject, module));
            return;
        }

        // What reads a file a line at a time says this of the first line that has one in it, and has for the line as much as comes before it: tok_nextc(), and _syntaxerror_range()
        if (size_t zero = WTF::find(bytes, static_cast<uint8_t>(0)); zero != notFound) {
            size_t lineStart = zero;
            while (lineStart && bytes[lineStart - 1] != '\n')
                --lineStart;
            int line = 1 + std::ranges::count(bytes.first(lineStart), '\n');
            JSValue text = jsString(vm, String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(bytes.subspan(lineStart, zero - lineStart))));
            JSValue details = PyTuple::create(globalObject, { jsString(vm, filename), jsNumber(line), jsNumber(0), text, jsNumber(line), jsNumber(0) });
            JSValue exception = call(globalObject, globalObject->pyRealm()->type(BuiltinType::SyntaxError)->object(), jsNontrivialString(vm, "source code cannot contain null bytes"_s), details);
            RETURN_IF_EXCEPTION(scope, void());
            throwException(globalObject, scope, exception);
            return;
        }

        SourceCode source = makeSource(globalObject, bytes, origin, filename);
        RETURN_IF_EXCEPTION(scope, void());
        JSFunction* function = compileModule(globalObject, source, module);
        if (scope.exception()) [[unlikely]] {
            scope.release();
            forgetSourceOfSyntaxError(globalObject);
            return;
        }
        scope.release();
        runModuleBody(globalObject, function);
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
    importState(globalObject).hasUnhandledKeyboardInterrupt = false;
    importState(globalObject).mainHasRaised = false;
    startPython(globalObject);
    if (scope.exception()) {
        return reportThatPythonCouldNotStart(globalObject);
    }
    if (!audit(globalObject, "cpython.run_file"_s, jsString(vm, filename)))
        return printExceptionAndGetStatus(globalObject);
    runInMainModule(globalObject, bytes, origin, filename);
    if (scope.exception())
        return printExceptionAndGetStatus(globalObject);
    return 0;
}

// ---- Running what `python` was asked to: Modules/main.c of CPython


// _PyUnicode_Dedent(), of UTF-8: without whatever blanks all of its lines begin with, but for those that are nothing but blanks
static Vector<uint8_t> dedent(std::span<const uint8_t> source)
{
    auto isBlank = [] (uint8_t c) { return c == ' ' || c == '\t'; };
    // search_longest_common_leading_whitespace()
    std::optional<std::span<const uint8_t>> common;
    for (size_t i = 0; i < source.size(); ++i) {
        size_t lineStart = i;
        std::optional<size_t> endOfBlanks;
        for (; i < source.size() && source[i] != '\n'; ++i) {
            if (!endOfBlanks && !isBlank(source[i])) {
                if (i == lineStart)
                    return Vector<uint8_t>(source);
                endOfBlanks = i;
            }
        }
        if (!endOfBlanks)
            continue;
        auto blanks = source.subspan(lineStart, *endOfBlanks - lineStart);
        if (!common) {
            common = blanks;
            continue;
        }
        size_t length = 0;
        while (length < common->size() && length < blanks.size() && (*common)[length] == blanks[length])
            ++length;
        if (!length)
            return Vector<uint8_t>(source);
        common = common->first(length);
    }
    if (!common)
        return Vector<uint8_t>(source);

    Vector<uint8_t> result;
    for (size_t i = 0; i < source.size(); ++i) {
        size_t lineStart = i;
        bool isAllBlanks = true;
        for (; i < source.size() && source[i] != '\n'; ++i) {
            if (!isBlank(source[i]))
                isAllBlanks = false;
        }
        bool endsInNewline = i < source.size();
        if (isAllBlanks && endsInNewline) {
            result.append('\n');
            continue;
        }
        result.append(source.subspan(lineStart + common->size(), i - lineStart - common->size()));
        if (endsInNewline)
            result.append('\n');
    }
    return result;
}

// pymain_run_command()
static int runCommand(JSGlobalObject* globalObject, const String& command)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!audit(globalObject, "cpython.run_command"_s, jsString(vm, command)))
        return printExceptionAndGetStatus(globalObject);
    auto encoded = encodeString(globalObject, jsString(vm, command), "utf-8"_s, String());
    if (scope.exception()) {
        fputs("Unable to decode the command from the command line:\n", stderr);
        return printExceptionAndGetStatus(globalObject);
    }
    Vector<uint8_t> dedented = dedent(encoded->span());
    String text = String::fromUTF8(byteCast<char8_t>(dedented.span()));

    // _PyRun_SimpleString(), by a name, and so run_mod() with the source, which is kept for what shows where something went wrong
    auto run = [&] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSObject* module = addModule(globalObject, jsNontrivialString(vm, "__main__"_s));
        RETURN_IF_EXCEPTION(scope, void());
        if (text.contains(static_cast<char16_t>(0))) {
            raise(globalObject, scope, BuiltinType::SyntaxError, "source code string cannot contain null bytes"_s);
            return;
        }
        JSString* filename = jsNontrivialString(vm, "<string>"_s);
        JSFunction* function = compileModule(globalObject, makeSource(text, SourceOrigin(), "<string>"_s), module);
        RETURN_IF_EXCEPTION(scope, void());
        JSObject* code = codeObjectFor(globalObject, function->jsExecutable());
        JSValue registerCode = importModuleAttribute(globalObject, "linecache"_s, "_register_code"_s);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isCallable(globalObject, registerCode)) {
            raiseValueError(globalObject, scope, "linecache._register_code is not callable"_s);
            return;
        }
        call(globalObject, registerCode, code, jsString(vm, text), filename);
        RETURN_IF_EXCEPTION(scope, void());
        if (!audit(globalObject, "exec"_s, code))
            return;
        scope.release();
        runModuleBody(globalObject, function);
    };
    run();
    return scope.exception() ? printExceptionAndGetStatus(globalObject) : 0;
}

// pymain_run_module()
static int runModule(JSGlobalObject* globalObject, const String& name, bool setsFirstArgument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!audit(globalObject, "cpython.run_module"_s, jsString(vm, name)))
        return printExceptionAndGetStatus(globalObject);
    JSValue run = importModuleAttribute(globalObject, "runpy"_s, "_run_module_as_main"_s);
    if (scope.exception()) {
        fputs("Could not import runpy._run_module_as_main\n", stderr);
        return printExceptionAndGetStatus(globalObject);
    }
    call(globalObject, run, jsString(vm, name), jsBoolean(setsFirstArgument));
    return scope.exception() ? printExceptionAndGetStatus(globalObject) : 0;
}

#if OS(UNIX)

static CString encodedForSystem(const String& text)
{
    // What could not be decoded when it was read is put back as it was.
    Vector<char> bytes;
    for (char32_t character : StringView(text).codePoints()) {
        if (character >= 0xDC80 && character <= 0xDCFF) {
            bytes.append(static_cast<char>(character - 0xDC00));
            continue;
        }
        uint8_t buffer[U8_MAX_LENGTH];
        size_t length = 0;
        U8_APPEND_UNSAFE(buffer, length, character);
        bytes.append(byteCast<char>(std::span(buffer).first(length)));
    }
    return CString(bytes.span());
}

static bool readAll(FILE* file, Vector<uint8_t>& bytes)
{
    uint8_t buffer[16384];
    while (size_t count = fread(buffer, 1, sizeof(buffer), file))
        bytes.append(std::span(buffer).first(count));
    return !ferror(file);
}

// pymain_run_file_obj()
static int runFile(JSGlobalObject* globalObject, const Configuration& configuration)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    const String& filename = configuration.runFilename;
    if (!audit(globalObject, "cpython.run_file"_s, jsString(vm, filename)))
        return printExceptionAndGetStatus(globalObject);
    auto complain = [&] (const String& afterName) {
        String shown = repr(globalObject, jsString(vm, filename));
        scope.clearException();
        writeToStandardError(globalObject, concatenate(configuration.programName, ": "_s, afterName.startsWith(' ') ? shown : concatenate("can't open file "_s, shown), afterName, '\n'));
    };
    CString path = encodedForSystem(filename);
    FILE* file = fopen(path.data(), "rb");
    if (!file) {
        int error = errno;
        complain(concatenate(": [Errno "_s, error, "] "_s, String::fromUTF8(strerror(error))));
        return 2;
    }
    struct stat information;
    if (!fstat(fileno(file), &information) && S_ISDIR(information.st_mode)) {
        complain(" is a directory, cannot continue"_s);
        fclose(file);
        return 1;
    }
    Vector<uint8_t> bytes;
    readAll(file, bytes);
    fclose(file);
    std::span<const uint8_t> source = bytes.span();
    // The end of the first line is kept, so that the lines are numbered as they were.
    if (configuration.skipsFirstLineOfSource) {
        size_t newline = WTF::find(source, static_cast<uint8_t>('\n'));
        source = newline == notFound ? source.last(0) : source.subspan(newline);
    }
    if (!checkSignals(globalObject))
        return printExceptionAndGetStatus(globalObject);
    runInMainModule(globalObject, source, SourceOrigin { URL::fileURLWithFileSystemPath(filename) }, filename);
    return scope.exception() ? printExceptionAndGetStatus(globalObject) : 0;
}

// _PyPathConfig_ComputeSysPath0(): what sys.path is to begin with. Null if it is to be left as it is.
static String computeFirstSearchPath(const Vector<String>& arguments)
{
    if (arguments.isEmpty())
        return { };
    const String& first = arguments[0];
    bool hasModule = first == "-m"_s;
    bool hasScript = !hasModule && first != "-c"_s;
    char buffer[PATH_MAX];
    if (hasModule) {
        if (!getcwd(buffer, sizeof(buffer)))
            return { };
        return String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(unsafeSpan(buffer)));
    }
    // Nothing was named, and it is what comes in that is run: wherever the process is at the time.
    if (!hasScript || first.isEmpty())
        return emptyString();

    CString path = encodedForSystem(first);
    char link[PATH_MAX + 1];
    ssize_t length = readlink(path.data(), link, PATH_MAX);
    if (length > 0) {
        link[length] = '\0';
        if (link[0] == '/')
            path = CString(unsafeSpan(link));
        else if (strchr(link, '/')) {
            // Beside what is a link to it
            const char* separator = strrchr(path.data(), '/');
            if (!separator)
                path = CString(unsafeSpan(link));
            else {
                Vector<char> joined;
                joined.append(path.span().first(separator + 1 - path.data()));
                joined.append(unsafeSpan(link));
                path = CString(joined.span());
            }
        }
    }
    if (realpath(path.data(), buffer))
        path = CString(unsafeSpan(buffer));
    const char* separator = strrchr(path.data(), '/');
    size_t count = 0;
    if (separator) {
        count = separator + 1 - path.data();
        if (count > 1)
            --count;
    }
    return String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(path.span().first(count)));
}

// pymain_set_inspect()
static void setInspect(JSGlobalObject* globalObject, bool inspect)
{
    PyRealm* realm = globalObject->pyRealm();
    realm->mutableConfiguration().inspect = inspect;
    updateSysFlagsFromConfiguration(globalObject, realm->sysModule());
}

// pymain_run_startup(). If it is the end, that is the status.
static std::optional<int> runStartupFile(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!globalObject->pyRealm()->configuration().usesEnvironment)
        return std::nullopt;
    const char* variable = getenv("PYTHONSTARTUP");
    if (!variable || !variable[0])
        return std::nullopt;
    // pymain_err_print(): it is only the end if it was SystemExit.
    auto print = [&] () -> std::optional<int> {
        JSValue value = scope.exception()->value();
        scope.clearException();
        int status = 1;
        if (handleSystemExit(globalObject, value, status))
            return status;
        reportUncaughtException(globalObject, value);
        return std::nullopt;
    };
    JSValue startup = decodeFileSystemBytes(globalObject, unsafeSpan(variable));
    if (scope.exception() || !audit(globalObject, "cpython.run_startup"_s, startup))
        return print();
    FILE* file = fopen(variable, "r");
    if (!file) {
        int error = errno;
        writeToStandardError(globalObject, "Could not open PYTHONSTARTUP\n"_s);
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        raiseOSError(globalObject, throwScope, error, startup);
        return print();
    }
    Vector<uint8_t> bytes;
    readAll(file, bytes);
    fclose(file);
    String filename = stringIn(startup)->value(globalObject);
    runInMainModule(globalObject, bytes.span(), SourceOrigin { URL::fileURLWithFileSystemPath(filename) }, filename);
    return scope.exception() ? print() : std::nullopt;
}

// pymain_run_interactive_hook(). If it is the end, that is the status.
static std::optional<int> runInteractiveHook(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue hook = sysAttribute(globalObject, "__interactivehook__"_s);
    if (!hook)
        return std::nullopt;
    if (audit(globalObject, "cpython.run_interactivehook"_s, hook))
        call(globalObject, hook);
    if (!scope.exception())
        return std::nullopt;
    JSValue value = scope.exception()->value();
    scope.clearException();
    writeToStandardError(globalObject, "Failed calling sys.__interactivehook__\n"_s);
    int status = 1;
    if (handleSystemExit(globalObject, value, status))
        return status;
    reportUncaughtException(globalObject, value);
    return std::nullopt;
}

void runStandardInput(JSGlobalObject* globalObject)
{
    // _Py_FdIsInteractive()
    if (isatty(fileno(stdin)) || globalObject->pyRealm()->configuration().interactive) {
        runInteractiveLoop(globalObject);
        return;
    }
    Vector<uint8_t> bytes;
    readAll(stdin, bytes);
    runInMainModule(globalObject, bytes.span(), SourceOrigin(), "<stdin>"_s);
}

// _pymain_run_repl()
static int runWhatComesIn(JSGlobalObject* globalObject, bool runsStartupFile)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto& configuration = globalObject->pyRealm()->configuration();
    if (!checkSignals(globalObject) || !audit(globalObject, "cpython.run_stdin"_s))
        return printExceptionAndGetStatus(globalObject);
    const char* wantsBasic = configuration.usesEnvironment ? getenv("PYTHON_BASIC_REPL") : nullptr;
    if (isatty(fileno(stdin)) && !(wantsBasic && wantsBasic[0])) {
        importModule(globalObject, "_pyrepl"_s);
        if (!scope.exception()) {
            // pymain_run_pyrepl()
            JSValue console = importModuleAttribute(globalObject, "_pyrepl.main"_s, "interactive_console"_s);
            if (scope.exception()) {
                fputs("Could not import _pyrepl.main\n", stderr);
                return printExceptionAndGetStatus(globalObject);
            }
            JSObject* module = addModule(globalObject, jsNontrivialString(vm, "__main__"_s));
            if (!scope.exception()) {
                PyDict* keywords = PyDict::create(globalObject);
                keywords->setString(globalObject, "mainmodule"_s, module);
                keywords->setString(globalObject, "pythonstartup"_s, jsBoolean(runsStartupFile));
                MarkedArgumentBuffer none;
                callWithKeywordDict(globalObject, console, none, keywords);
            }
            return scope.exception() ? printExceptionAndGetStatus(globalObject) : 0;
        }
        if (!catchException(globalObject, BuiltinType::ModuleNotFoundError)) {
            fputs("Could not import _pyrepl.main\n", stderr);
            return printExceptionAndGetStatus(globalObject);
        }
    }

    runStandardInput(globalObject);
    return scope.exception() ? printExceptionAndGetStatus(globalObject) : 0;
}

// pymain_run_python()
int runMain(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    importState(globalObject).hasUnhandledKeyboardInterrupt = false;
    importState(globalObject).mainHasRaised = false;
    startPython(globalObject);
    if (scope.exception()) {
        return reportThatPythonCouldNotStart(globalObject);
    }
    const Configuration& configuration = realm->configuration();
    bool runsCode = !configuration.runCommand.isNull() || !configuration.runFilename.isNull() || !configuration.runModule.isNull(); // config_run_code()
    bool isStandardInputInteractive = isatty(fileno(stdin)) || configuration.interactive; // stdin_is_interactive()

    // If it is a directory or an archive that has a __main__.py in it, that is what is run, and it is what modules are looked for in first: pymain_get_importer()
    bool isImportPath = false;
    if (!configuration.runFilename.isNull()) {
        JSValue importer = pathImporterFor(globalObject, jsString(vm, configuration.runFilename));
        if (scope.exception()) {
            fputs("Failed checking if argv[0] is an import path entry\n", stderr);
            return printExceptionAndGetStatus(globalObject);
        }
        isImportPath = !isNone(importer);
    }

    // pymain_import_readline(): before there is anything of the program's to be found in place of them
    if (!checkSignals(globalObject))
        return printExceptionAndGetStatus(globalObject);
    if (!configuration.isIsolated && (configuration.inspect || !runsCode) && isatty(fileno(stdin))) {
        for (ASCIILiteral name : { "readline"_s, "rlcompleter"_s }) {
            importModule(globalObject, name);
            if (scope.exception() && !scope.tryClearException())
                return 1;
        }
    }

    String first;
    if (isImportPath)
        first = configuration.runFilename;
    else if (!configuration.hasSafePath)
        first = computeFirstSearchPath(configuration.arguments);
    if (!first.isNull()) {
        realm->mutableConfiguration().firstSearchPath = first;
        // pymain_sys_path_add_path0()
        JSValue path = sysAttribute(globalObject, "path"_s);
        if (!path) {
            auto throwScope = DECLARE_THROW_SCOPE(vm);
            raise(globalObject, throwScope, BuiltinType::RuntimeError, "unable to get sys.path"_s);
        } else
            callMethodNamed(globalObject, path, Identifier::fromString(vm, "insert"_s), jsNumber(0), jsString(vm, first));
        if (scope.exception())
            return printExceptionAndGetStatus(globalObject);
    }

    // pymain_header()
    if (!configuration.quiet && (configuration.verbose || (!runsCode && isStandardInputInteractive))) {
        fprintf(stderr, "Python %s on %s\n", PYTHON_FULL_VERSION_STRING, PYTHON_PLATFORM);
        if (configuration.importsSite)
            fputs("Type \"help\", \"copyright\", \"credits\" or \"license\" for more information.\n", stderr);
    }

    if (!checkSignals(globalObject))
        return printExceptionAndGetStatus(globalObject);
    int status;
    if (!configuration.runCommand.isNull())
        status = runCommand(globalObject, configuration.runCommand);
    else if (!configuration.runModule.isNull())
        status = runModule(globalObject, configuration.runModule, true);
    else if (isImportPath)
        status = runModule(globalObject, "__main__"_s, false);
    else if (!configuration.runFilename.isNull())
        status = runFile(globalObject, configuration);
    else {
        // pymain_run_stdin()
        if (isStandardInputInteractive) {
            // From here on SystemExit is the end.
            setInspect(globalObject, false);
            if (auto ended = runStartupFile(globalObject))
                return *ended;
            if (auto ended = runInteractiveHook(globalObject))
                return *ended;
        }
        status = runWhatComesIn(globalObject, false);
    }
    if (!checkSignals(globalObject))
        return printExceptionAndGetStatus(globalObject);

    // pymain_repl(). The environment is looked at now, so that the program can have asked for this.
    if (!configuration.inspect && configuration.usesEnvironment) {
        if (const char* variable = getenv("PYTHONINSPECT"); variable && variable[0])
            setInspect(globalObject, true);
    }
    if (!(configuration.inspect && isStandardInputInteractive && runsCode))
        return status;
    setInspect(globalObject, false);
    if (auto ended = runInteractiveHook(globalObject))
        return *ended;
    return runWhatComesIn(globalObject, true);
}

#endif // OS(UNIX)

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
    if (!globalObject->pyRealmIfExists() || !importState(globalObject).isStarted)
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
    // In CPython the two that Python started with are sent on when they are let go of, which is when everything is. Nothing is let go of here, so they are seen to with the two that are there now, which are mostly the same two.
    Vector<JSValue, 4> seen;
    for (ASCIILiteral name : { "stdout"_s, "stderr"_s, "__stdout__"_s, "__stderr__"_s }) {
        JSValue file = sysAttribute(globalObject, name);
        if (!file || isNone(file) || seen.contains(file))
            continue;
        seen.append(file);
        if (isFileClosed(globalObject, file))
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

int finalizeMain(JSGlobalObject* globalObject, int status)
{
    if (!finalizePython(globalObject))
        status = 120;
    if (globalObject->pyRealmIfExists() && importState(globalObject).hasUnhandledKeyboardInterrupt)
        status = exitByInterrupt();
    return status;
}

} } // namespace JSC::Python
