/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyExec.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyCodecs.h"
#include "PyCommandLine.h"
#include "PyCompiler.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyGenerator.h"
#include "PyIO.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyParser.h"
#include "PyStdlibModuleNames.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyVirtualFiles.h"
#include "TopExceptionScope.h"
#include <signal.h>
#include <unistd.h>
#include <wtf/FileSystem.h>

namespace JSC {
namespace Python {

// From source text to running code; and what the interpreter does around a program: Python/pythonrun.c in CPython.

// ---- Compiling ----

// A line of a file as a SyntaxError shows it (PyErr_ProgramTextObject() in CPython): what fits a buffer of 1000 bytes, decoded as
// `encoding` says or, if that is null, as the file itself declares. A null string: there is no such line.
static String lineOfProgramText(JSGlobalObject* globalObject, std::span<const uint8_t> file, int lineno, ASCIICString encoding)
{
    static constexpr size_t mostBytes = 999;
    VM& vm = getVM(globalObject);
    if (lineno <= 0)
        return { };
    Vector<uint8_t> line;
    std::span<const uint8_t> rest = file;
    for (int current = 1; current <= lineno; ++current) {
        if (rest.empty())
            return { };
        size_t length = 0;
        while (length < rest.size() && rest[length] != '\n' && rest[length] != '\r')
            ++length;
        bool hasEnding = length < rest.size();
        if (current == lineno) {
            line.append(rest.first(std::min(length, mostBytes)));
            if (hasEnding && length < mostBytes)
                line.append('\n');
        }
        rest = rest.subspan(length + (hasEnding ? (rest[length] == '\r' && length + 1 < rest.size() && rest[length + 1] == '\n' ? 2 : 1) : 0));
    }
    std::span<const uint8_t> bytes = line.span();
    if (lineno == 1 && bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        bytes = bytes.subspan(3);

    if (encoding.isNull())
        encoding = declaredEncoding(file);
    if (encoding.isNull() || encoding == "utf-8"_s)
        return String::fromUTF8ReplacingInvalidSequences(bytes);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue text = callMethod(globalObject, PyBytes::create(globalObject, bytes), internString(vm, "decode"_s), makeStr(vm, String(byteCast<Latin1Character>(encoding.span()))), makeStr(vm, "replace"_s));
    String result;
    if (!scope.exception())
        result = strValue(text)->value(globalObject);
    if (scope.exception()) {
        dropPendingException(globalObject, scope);
        return { };
    }
    return result;
}

namespace {

class RuntimeParserHost final : public ParserHost {
public:
    // Whoever makes one keeps the module alive.
    RuntimeParserHost(JSGlobalObject* globalObject, const String& filename, JSValue module)
        : m_globalObject(globalObject)
        , m_filename(filename)
        , m_module(module)
    {
    }

    WarningResult warn(const Warning& warning) final
    {
        Runtime& rt = runtime(m_globalObject);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
        PyType* category = warning.category == WarningCategory::SyntaxWarning ? rt.SyntaxWarning() : rt.DeprecationWarning();
        if (!failed(scope, warnExplicit(m_globalObject, category, warning.message, m_filename, warning.lineno, m_module)))
            return WarningResult::Reported;
        if (clearPendingExceptionIfMatches(m_globalObject, category))
            return WarningResult::BecameException;
        return WarningResult::OtherException;
    }

    bool lineOfFile(int lineno, const ASCIICString& encoding, Vector<uint8_t>& line) final
    {
        if (!m_hasReadFile) {
            m_hasReadFile = true;
            if (!m_filename.startsWith('<'))
                m_file = readFile(m_filename);
        }
        if (!m_file)
            return false;
        String text = lineOfProgramText(m_globalObject, m_file->span(), lineno, encoding);
        if (text.isNull())
            return false;
        line.append(byteCast<uint8_t>(text.utf8().span()));
        return true;
    }

    int maxStrDigits() final { return runtime(m_globalObject).maxStrDigits; }

    bool hasCharacterNames() final
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(m_globalObject));
        if (!failed(scope, codecLoadCharacterNames(m_globalObject)))
            return true;
        dropPendingException(m_globalObject, scope);
        return false;
    }

    // bytes.decode(encoding)
    UTF8CString decodeSource(std::span<const uint8_t> bytes, const ASCIICString& encoding, String& error, bool& unknownEncoding) final
    {
        Runtime& rt = runtime(m_globalObject);
        VM& vm = rt.vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSValue text = callMethod(m_globalObject, PyBytes::create(m_globalObject, bytes), internString(vm, "decode"_s), makeStr(vm, String(byteCast<Latin1Character>(encoding.span()))));
        String result;
        if (!scope.exception())
            result = strValue(text)->value(m_globalObject);
        if (scope.exception()) {
            // Whatever went wrong is a SyntaxError that says the same.
            JSValue exception = takePendingException(m_globalObject);
            unknownEncoding = rt.typeOf(exception)->isSubtypeOf(rt.LookupError());
            JSString* message = str(m_globalObject, exception);
            error = scope.exception() ? "encoding problem"_str : message->value(m_globalObject);
            dropPendingException(m_globalObject, scope);
            return { };
        }
        // (String::utf8() goes through a Vector with room for three bytes to the unit.)
        auto length = lengthInUTF8(result);
        if (length ? *length > SourceText::maxLength : result.length() > SourceText::maxLength / 3) [[unlikely]]
            return { };
        if (!length)
            return result.utf8();
        std::span<char8_t> room;
        auto encoded = UTF8CString::newUninitialized(*length, room);
        writeUTF8(result, byteCast<uint8_t>(room));
        return encoded;
    }

private:
    JSGlobalObject* m_globalObject;
    String m_filename;
    JSValue m_module;
    std::optional<Vector<uint8_t>> m_file;
    bool m_hasReadFile { false };
};

}

std::unique_ptr<ParserHost> makeParserHost(JSGlobalObject* globalObject, const String& filename, JSValue module)
{
    return makeUniqueWithoutFastMallocCheck<RuntimeParserHost>(globalObject, filename, module);
}

JSValue raiseSyntaxErrorInfo(JSGlobalObject* globalObject, const SyntaxErrorInfo& info, const String& filename)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Type = SyntaxErrorInfo::Type;
    PyType* type = nullptr;
    switch (info.type) {
    case Type::None:
        RELEASE_ASSERT_NOT_REACHED();
    case Type::HostException:
        ASSERT(scope.exception());
        return { };
    case Type::SyntaxError:
        type = rt.SyntaxError();
        break;
    case Type::IndentationError:
        type = rt.IndentationError();
        break;
    case Type::TabError:
        type = rt.TabError();
        break;
    case Type::IncompleteInputError:
        type = rt.IncompleteInputError();
        break;
    case Type::ValueError:
        return raiseValueError(globalObject, scope, info.message);
    case Type::OverflowError:
        return raiseOverflowError(globalObject, scope, info.message);
    case Type::MemoryError:
        if (info.message.isNull()) {
            raiseMemoryError(globalObject, scope, String());
            return { };
        }
        return raiseWithArgs(globalObject, scope, rt.MemoryError(), { makeStr(vm, info.message) });
    case Type::RecursionError:
        return raiseRecursionError(globalObject, scope, info.message);
    case Type::SystemError:
        return raiseSystemError(globalObject, scope, info.message);
    case Type::UnicodeEncodeError:
        RELEASE_AND_RETURN(scope, codecEncodeUTF8(globalObject, makeStr(vm, info.message), "strict"_s));
    }

    JSValue message = makeStr(vm, info.message);
    if (!info.hasLocation)
        return raiseWithArgs(globalObject, scope, type, { message });
    String line = info.text;
    // What the symbol table and the compiler find wrong: they know where, and the text is looked up in the file, if there is one.
    if (line.isNull() && !filename.startsWith('<')) {
        if (auto file = readFile(filename))
            line = lineOfProgramText(globalObject, file->span(), info.lineno, { });
    }
    JSValue text = line.isNull() ? jsUndefined() : JSValue(makeStr(vm, line));
    JSValue file = makeStr(vm, filename);
    if (info.locationIsNotAnArgument) {
        PyException* exception = newException(globalObject, type, info.message);
        RETURN_IF_EXCEPTION(scope, { });
        auto set = [&](ASCIILiteral name, JSValue value) {
            setAttr(globalObject, exception, internString(vm, name), value);
            if (scope.exception())
                takePendingException(globalObject);
        };
        set("lineno"_s, jsNumber(info.lineno));
        set("offset"_s, jsNumber(info.offset));
        set("end_lineno"_s, info.hasEnd ? jsNumber(info.endLineno) : jsUndefined());
        set("end_offset"_s, info.hasEnd ? jsNumber(info.endOffset) : jsUndefined());
        set("filename"_s, file);
        if (!line.isNull())
            set("text"_s, text);
        return raiseException(globalObject, scope, exception);
    }
    PyTuple* details = info.hasEnd
        ? PyTuple::create(globalObject, { file, jsNumber(info.lineno), jsNumber(info.offset), text, jsNumber(info.endLineno), jsNumber(info.endOffset) })
        : PyTuple::create(globalObject, { file, jsNumber(info.lineno), jsNumber(info.offset), text });
    return raiseWithArgs(globalObject, scope, type, { message, details });
}

bool parseSource(JSGlobalObject* globalObject, ParseTree& tree, ParserHost& host, std::span<const uint8_t> source, const String& filename, ASCIILiteral mode, int flags, int featureVersion)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ParseMode parseMode = mode == "eval"_s ? ParseMode::Eval : mode == "single"_s ? ParseMode::Single : mode == "func_type"_s ? ParseMode::FunctionType : ParseMode::File;
    OptionSet<ParserFlag> parserFlags;
    if (flags & CompileTypeComments)
        parserFlags.add(ParserFlag::TypeComments);
    if (flags & CompileDontImplyDedent)
        parserFlags.add(ParserFlag::DontImplyDedent);
    if (flags & CompileAllowIncompleteInput)
        parserFlags.add(ParserFlag::AllowIncompleteInput);
    if (flags & static_cast<int>(CodeFlag::FutureBarryAsBDFL))
        parserFlags.add(ParserFlag::BarryAsBDFL);

    // Bytes are in the encoding they declare (PEP 263), whatever CompileSourceIsUTF8 says: CPython's parser does not look at it.
    SyntaxErrorInfo error;
    String fileToName = flags & CompileSourceIsFile ? filename : String();
    bool parsed = featureVersion < 0
        ? parse(vm, tree, host, source, fileToName, parseMode, parserFlags, flags & CompileIgnoreCookie, error)
        : parse(vm, tree, host, source, fileToName, parseMode, parserFlags, flags & CompileIgnoreCookie, error, featureVersion);
    if (parsed)
        return true;
    raiseSyntaxErrorInfo(globalObject, error, filename);
    EXCEPTION_ASSERT(scope.exception());
    if (error.hasMetadata) {
        // The text, unless it came from a file, where whoever wants it can read it.
        JSValue text = jsUndefined();
        if (!(flags & CompileSourceIsFile)) {
            if (String decoded = String::fromUTF8(tree.source.bytes()); !decoded.isNull())
                text = makeStr(vm, decoded);
        }
        if (auto* exception = PyException::tryFrom(pendingException(globalObject)))
            exception->setField(vm, ExceptionField::syntaxErrorMetadata, PyTuple::create(globalObject, { jsNumber(error.metadataLineno), jsNumber(error.metadataColOffset), text }));
    }
    scope.release();
    return false;
}

JSValue compileTree(JSGlobalObject* globalObject, ParseTree& tree, ParserHost& host, const String& filename, int flags, int optimize)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // The positions in the tree are offsets in the bytes the lexer read, so those are what the code keeps as its source.
    String text = String(byteCast<Latin1Character>(tree.source.bytes()));
    // exec(''): JSC has no function without source.
    if (text.isEmpty())
        text = "\n"_s;
    SourceCode sourceCode(StringSourceProvider::create(text, SourceOrigin(), filename, SourceTaintedOrigin::Untainted));

    CompilerOptions options;
    options.sourceIsShown = flags & CompileSourceIsShown;
    options.optimize = optimize < 0 ? rt.optimizationLevel : optimize;
    options.allowTopLevelAwait = flags & CompileAllowTopLevelAwait;
    options.futureAnnotations = flags & static_cast<int>(CodeFlag::FutureAnnotations);
    options.futureBarryAsBDFL = flags & static_cast<int>(CodeFlag::FutureBarryAsBDFL);
    Compiler compiler(globalObject, tree, host, sourceCode, filename, options);
    UnlinkedFunctionExecutable* unlinked = compiler.compile();
    RETURN_IF_EXCEPTION(scope, { });
    if (!unlinked)
        RELEASE_AND_RETURN(scope, raiseSyntaxErrorInfo(globalObject, compiler.error(), filename));
    RELEASE_AND_RETURN(scope, PyCode::of(globalObject, unlinked->link(vm, nullptr, sourceCode)));
}

JSValue compileSource(JSGlobalObject* globalObject, std::span<const uint8_t> source, const String& filename, ASCIILiteral mode, int flags, int optimize, JSValue module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RuntimeParserHost host(globalObject, filename, module);
    ParseTree tree;
    if (failed(scope, parseSource(globalObject, tree, host, source, filename, mode, flags)))
        return { };
    RELEASE_AND_RETURN(scope, compileTree(globalObject, tree, host, filename, flags, optimize));
}

// ---- Running ----

bool ensureBuiltins(JSGlobalObject* globalObject, PyDict* globals)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (globals->get(rt.names.dBuiltins()))
        return true;
    if (isFrozenDict(globals)) {
        raiseTypeError(globalObject, scope, "cannot assign __builtins__ to frozendict globals"_s);
        return false;
    }
    globals->set(vm, rt.names.dBuiltins(), currentBuiltins(globalObject));
    return true;
}

JSValue runCode(JSGlobalObject* globalObject, JSValue codeValue, PyDict* globals, JSValue locals, PyTuple* closure)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* code = PyCode::from(codeValue);
    if (failed(scope, ensureBuiltins(globalObject, globals)))
        return { };
    PyFunction* function = PyFunction::create(globalObject, code->executable(), globals);
    if (closure && closure->length())
        function->setClosure(vm, closure);
    if (!code->codeInfo().takesNamespace)
        RELEASE_AND_RETURN(scope, call(globalObject, function));
    RELEASE_AND_RETURN(scope, call(globalObject, function, locals ? locals : JSValue(globals)));
}

// ---- Output ----

void writeToFile(JSGlobalObject* globalObject, JSValue file, JSString* text)
{
    if (!file || isNone(file))
        return;
    callMethod(globalObject, file, runtime(globalObject).names.write(), text);
}

static void writeRaw(FILE* file, const String& text)
{
    auto utf8 = text.utf8(ConversionMode::LenientConversion);
    fwrite(utf8.data(), 1, utf8.length(), file);
    fflush(file);
}

// What sys.stderr raises is dropped: there may be nobody left to tell.
void writeToStderr(JSGlobalObject* globalObject, const String& text)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (vm.hasPendingTerminationException()) [[unlikely]]
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue file = rt.sysDict()->get(rt.names.stderr_());
    if (!file || isNone(file)) {
        writeRaw(stderr, text);
        return;
    }
    writeToFile(globalObject, file, makeStr(vm, text));
    if (scope.exception() && scope.tryClearException())
        writeRaw(stderr, text);
}

static void flushStandardStream(JSGlobalObject* globalObject, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (vm.hasPendingTerminationException()) [[unlikely]]
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue file = rt.sysDict()->get(name);
    if (file && !isNone(file))
        callMethod(globalObject, file, rt.names.flush());
    dropPendingException(globalObject, scope);
}

// ---- A program ----

// If the exception is a SystemExit, the status it asks for.
static std::optional<int> exitStatusOf(JSGlobalObject* globalObject, JSValue exceptionValue)
{
    Runtime& rt = runtime(globalObject);
    auto* exception = PyException::tryFrom(exceptionValue);
    if (!exception || !exception->type()->isSubtypeOf(rt.SystemExit()))
        return std::nullopt;
    // -i: the program is looked into when it has ended, however it ended.
    if (configuration().inspect)
        return std::nullopt;
    JSValue code = exception->field(ExceptionField::systemExitCode);
    if (!code || isNone(code))
        return 0;
    // All that gets out of the process is a byte. (And a negative status means something else: see handleUncaughtException().)
    if (isInt(code))
        return static_cast<int>(tryInt64(intValue(code)).value_or(-1) & 0xff);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSString* text = str(globalObject, code);
    String message = scope.exception() ? String() : text->value(globalObject);
    dropPendingException(globalObject, scope);
    writeToStderr(globalObject, makeString(message, '\n'));
    return 1;
}

static int handleUncaughtException(JSGlobalObject*, JSValue exception);

int handleUncaughtException(JSGlobalObject* globalObject)
{
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return 0;
    return handleUncaughtException(globalObject, exception);
}

int handleUncaughtExceptionOfFile(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    JSValue exception = takePendingException(globalObject);
    flushStandardStream(globalObject, rt.names.stderr_());
    flushStandardStream(globalObject, rt.names.stdout_());
    if (!exception)
        return 0;
    return handleUncaughtException(globalObject, exception);
}

bool reportUncaughtException(JSGlobalObject* globalObject, JSValue exception)
{
    if (!PyException::is(exception) || runtime(globalObject).isFinalized)
        return false;
    flushStandardStream(globalObject, runtime(globalObject).names.stdout_());
    handleUncaughtException(globalObject, exception);
    return true;
}

static int handleUncaughtException(JSGlobalObject* globalObject, JSValue exception)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (auto status = exitStatusOf(globalObject, exception))
        return *status;

    JSValue traceback = PyException::from(exception)->traceback();
    rt.sysDict()->set(vm, internString(vm, "last_exc"_s), exception);
    rt.sysDict()->set(vm, internString(vm, "last_type"_s), rt.typeOf(exception));
    rt.sysDict()->set(vm, internString(vm, "last_value"_s), exception);
    rt.sysDict()->set(vm, internString(vm, "last_traceback"_s), traceback ? traceback : jsUndefined());
    JSValue hook = rt.sysDict()->get(internString(vm, "excepthook"_s));
    if (failed(scope, audit(globalObject, "sys.excepthook"_s, hook, rt.typeOf(exception), exception, traceback))) {
        // A hook may have the report suppressed.
        if (clearPendingExceptionIfMatches(globalObject, rt.RuntimeError()))
            return 1;
        reportUnraisable(globalObject, "Exception ignored in audit hook"_s, { });
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return 1;
    }
    if (!hook) {
        writeToStderr(globalObject, "sys.excepthook is missing\n"_s);
        printException(globalObject, exception);
        return 1;
    }
    call(globalObject, hook, rt.typeOf(exception), exception, traceback ? traceback : jsUndefined());
    if (scope.exception()) {
        JSValue second = takePendingException(globalObject);
        if (!second) [[unlikely]] {
            std::ignore = scope.tryClearException();
            return 1;
        }
        if (auto status = exitStatusOf(globalObject, second))
            return *status;
        writeToStderr(globalObject, "Error in sys.excepthook:\n"_s);
        printException(globalObject, second);
        writeToStderr(globalObject, "\nOriginal exception was:\n"_s);
        printException(globalObject, exception);
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return 1;
    }
    if (rt.typeOf(exception)->isSubtypeOf(rt.KeyboardInterrupt()))
        return -SIGINT;
    return 1;
}

bool endsProgram(JSGlobalObject* globalObject, JSValue exception)
{
    if (!PyException::tryFrom(exception))
        return false;
    Runtime& rt = runtime(globalObject);
    PyType* type = rt.typeOf(exception);
    return type->isSubtypeOf(rt.SystemExit()) || type->isSubtypeOf(rt.KeyboardInterrupt());
}

void dieIfStatusIsSignal(int status)
{
    if (status >= 0)
        return;
    fflush(stdout);
    fflush(stderr);
    signal(-status, SIG_DFL);
    kill(getpid(), -status);
    // It may be blocked.
    _exit(128 - status);
}

static ASCIILiteral s_eventLoopModule;

void setEventLoopModule(ASCIILiteral name)
{
    s_eventLoopModule = name;
}

JSValue eventLoopFunction(JSGlobalObject* globalObject, ASCIILiteral name)
{
    if (s_eventLoopModule.isNull())
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, s_eventLoopModule);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttr(globalObject, module, internString(vm, name)));
}

// A module that awaits at the top level is a coroutine. This is for a host without an event loop: what the module awaits can
// only be what needs none.
static void runTopLevelCoroutine(JSGlobalObject* globalObject, PyGenerator* coroutine)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    while (true) {
        JSValue result;
        JSValue yielded = generatorSend(globalObject, coroutine, jsUndefined(), result);
        RETURN_IF_EXCEPTION(scope, void());
        if (!yielded)
            return;
        if (!isNone(yielded)) {
            raiseRuntimeError(globalObject, scope, "await at the top level of a module needs an event loop, which is not there yet"_s);
            return;
        }
    }
}

// sys.stdout and sys.stderr, unless they are closed. False: one of them could not be flushed, and if it is sys.stdout and
// `reports`, sys.stderr has been told.
static bool flushStandardStreamsAtExit(JSGlobalObject* globalObject, bool reports)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool succeeded = true;
    for (JSString* name : { rt.names.stdout_(), rt.names.stderr_() }) {
        JSValue file = rt.sysDict()->get(name);
        if (!file || isNone(file))
            continue;
        JSValue closed = getAttr(globalObject, file, internString(vm, "closed"_s));
        bool isClosed = closed && isTrue(globalObject, closed);
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return succeeded;
        if (scope.exception()) {
            scope.clearException();
            isClosed = false;
        }
        if (isClosed)
            continue;
        callMethod(globalObject, file, rt.names.flush());
        if (!scope.exception())
            continue;
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return succeeded;
        succeeded = false;
        if (reports && name == rt.names.stdout_())
            reportUnraisable(globalObject, "Exception ignored while flushing sys.stdout"_s, { });
        TRY_CLEAR_EXCEPTION(scope, succeeded);
    }
    return succeeded;
}

static NEVER_INLINE void runExitHandlersOnWipedStack(JSGlobalObject*);
static NEVER_INLINE bool finalizeInterpreterOnWipedStack(JSGlobalObject*);

// The collections at exit are to find dead what only the frames of the program held, and those lay where the frames of what follows
// lie now. See wipeStackBelow() for why these two have a frame with nothing in it that it does not write.
void runExitHandlers(JSGlobalObject* globalObject)
{
    if (!globalObject->pythonRuntime())
        return;
    size_t size = 128 * KB;
    wipeStackBelow(globalObject, size);
    runExitHandlersOnWipedStack(globalObject);
}

bool finalizeInterpreter(JSGlobalObject* globalObject)
{
    if (!globalObject->pythonRuntime())
        return true;
    size_t size = 128 * KB;
    wipeStackBelow(globalObject, size);
    return finalizeInterpreterOnWipedStack(globalObject);
}

static NEVER_INLINE void runExitHandlersOnWipedStack(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // One of them may end the process itself.
    if (rt.isFinalized || rt.isRunningExitHandlers || vm.hasPendingTerminationException())
        return;
    SetForScope isRunning(rt.isRunningExitHandlers, true);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // No loop will run what they schedule.
    Runtime::EventLoopPositionScope position(rt, false);
    // A callback may start a thread, and a thread register a callback.
    do {
        if (JSValue threading = rt.modules()->get(internString(vm, "threading"_s)); threading && !isNone(threading)) {
            callMethod(globalObject, threading, internString(vm, "_shutdown"_s));
        } else
            waitForThreadsAtExit(globalObject);
        if (scope.exception() && !vm.hasPendingTerminationException()) [[unlikely]] {
            stopWaitingForThreadsAtExit(globalObject);
            reportUnraisable(globalObject, "Exception ignored on threading shutdown"_s, { });
        }
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return;
        wipeStackBelow(globalObject);
        rt.finalizeWhatReferenceCountsWouldHaveFreed();
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return;
        runAtexitCallbacks(globalObject);
        if (scope.exception() && vm.hasPendingTerminationException()) [[unlikely]]
            return;
        scope.assertNoException();
    } while (hasThreadsOrAtexitCallbacks(globalObject));
}

static NEVER_INLINE bool finalizeInterpreterOnWipedStack(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (rt.isFinalized)
        return true;
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    runExitHandlersOnWipedStack(globalObject);
    rt.isFinalized = true;
    // All of this is code of the program, which the host may end at any point of it, as it ends any other: a worker is terminated
    // while it exits. Then none of what is left runs, and what ended it goes on to whoever called this.
    auto isTerminated = [&] {
        if (!vm.hasPendingTerminationException()) [[likely]]
            return false;
        rt.threads().didFinalize();
        finalizeSignals(globalObject);
        return true;
    };
    if (isTerminated())
        return true;
    Runtime::EventLoopPositionScope position(rt, false);
    // The threads that are left are daemons. They have run their last.
    rt.threads().didFinalize();
    // Before the finalizers, which may close what the standard streams write to, and after them, as they may print.
    bool flushed = flushStandardStreamsAtExit(globalObject, true);
    if (isTerminated())
        return true;
    flushAllOpenFiles(globalObject);
    if (isTerminated())
        return true;
    finalizeSignals(globalObject);
    wipeStackBelow(globalObject);
    rt.finalizeAllAtExit();
    if (isTerminated())
        return true;
    flushed &= flushStandardStreamsAtExit(globalObject, flushed);
    if (isTerminated())
        return true;
    flushAllOpenFiles(globalObject);
    return flushed;
}

// Code compiled ahead of time was compiled in the belief that an arithmetic operator gives a number or a BigInt, and nothing tells it
// otherwise, as a watchpoint tells the code of the JITs.
static bool refuseBesideCompiledCode(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    if (!vm.hasCompiledCode() || globalObject->pythonRuntime()) [[likely]]
        return false;
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwTypeError(globalObject, scope, "Python cannot be used in a program that was compiled ahead of time"_s);
    return true;
}

JSValue importModuleFromJS(JSGlobalObject* globalObject, const String& name)
{
    if (refuseBesideCompiledCode(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isFirstUse = !globalObject->pythonRuntime();
    Runtime& rt = Runtime::ensure(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (isFirstUse) {
        JSArray* arguments = newList(globalObject, std::initializer_list<JSValue> { rt.names.empty() });
        RETURN_IF_EXCEPTION(scope, { });
        rt.sysDict()->set(vm, internString(vm, "argv"_s), arguments);
        if (auto* searchPath = tryAsList(rt.sysDict()->get(rt.names.path()))) {
            listInsert(globalObject, searchPath, 0, rt.names.empty());
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (name == "__python__"_s)
        return rt.jsViewObject(JSViewObject::Namespace);
    RELEASE_AND_RETURN(scope, importModule(globalObject, internString(vm, name)));
}

JSValue importModuleFromJS(JSGlobalObject* globalObject, const String& name, ASCIILiteral codeIfNotFound)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModuleFromJS(globalObject, name);
    if (!scope.exception() || !globalObject->pythonRuntime()) [[likely]]
        return module;
    Runtime& rt = runtime(globalObject);
    if (!pendingExceptionMatches(globalObject, rt.ModuleNotFoundError()))
        return { };
    // Not one that the module itself ran into, importing something else.
    JSValue exception = takePendingException(globalObject);
    JSValue missing = PyException::from(exception)->field(ExceptionField::importErrorName);
    if (missing && isStr(missing)) {
        String missingName = strValue(missing)->value(globalObject);
        if (!scope.exception() && (missingName == name || name.startsWith(makeString(missingName, '.'))))
            setAttr(globalObject, exception, internString(vm, "code"_s), makeStr(vm, codeIfNotFound));
    }
    (void)scope.tryClearException();
    throwPythonException(globalObject, scope, exception);
    return { };
}

bool isStandardLibraryModuleName(StringView name)
{
    StringView first = name.left(name.find('.'));
    return std::ranges::any_of(stdlibModuleNames, [&](ASCIILiteral candidate) {
        return first == candidate;
    });
}

// Not FileSystem::realPath(): that frees what the C library allocated, and in Bun the two do not have the same allocator.
String realPath(const String& path)
{
    char resolved[PATH_MAX];
    auto bytes = encodeFilesystem(path);
    bytes.append(0);
    return realpath(byteCast<char>(bytes.span()).data(), resolved) ? decodeFilesystem(byteCast<uint8_t>(unsafeSpan(resolved))) : String();
}

String absolutePath(const String& path)
{
    Vector<String> parts;
    if (!path.startsWith('/')) {
        char buffer[PATH_MAX];
        if (!getcwd(buffer, sizeof(buffer)))
            return path;
        String directory = decodeFilesystem(byteCast<uint8_t>(unsafeSpan(buffer)));
        for (auto part : StringView(directory).split('/'))
            parts.append(part.toString());
    }
    for (auto part : StringView(path).split('/')) {
        if (part == "."_s)
            continue;
        if (part == ".."_s) {
            if (!parts.isEmpty())
                parts.removeLast();
            continue;
        }
        parts.append(part.toString());
    }
    StrBuilder result;
    for (auto& part : parts)
        result.append('/', part);
    return parts.isEmpty() ? "/"_str : result.toString();
}

// A file is the module that `import` finds it as. The directories above it that are packages (they have an __init__.py) are
// part of its name, and the first that is not is where the search has to start, so it is put on sys.path if it is not there.
JSValue importFileFromJS(JSGlobalObject* globalObject, const String& givenPath, const String& source)
{
    if (refuseBesideCompiledCode(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isFirstUse = !globalObject->pythonRuntime();
    Runtime& rt = Runtime::ensure(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (isFirstUse) {
        JSArray* arguments = newList(globalObject, std::initializer_list<JSValue> { rt.names.empty() });
        RETURN_IF_EXCEPTION(scope, { });
        rt.sysDict()->set(vm, internString(vm, "argv"_s), arguments);
    }

    String path = absolutePath(givenPath);
    JSValue importFile = importSystemFunction(globalObject, "import_file"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (importFile)
        RELEASE_AND_RETURN(scope, call(globalObject, importFile, makeStr(vm, path), source.isNull() ? jsUndefined() : JSValue(makeStr(vm, source))));
    if (!source.isNull())
        return raise(globalObject, scope, rt.ImportError(), "source that is in no file cannot be imported"_s);

    String directory = FileSystem::parentPath(path);
    String name = FileSystem::pathFileName(path);
    if (size_t dot = name.reverseFind('.'); dot != notFound)
        name = name.left(dot);
    if (name == "__init__"_s) {
        name = FileSystem::pathFileName(directory);
        directory = FileSystem::parentPath(directory);
    }
    while (kindOfFile(FileSystem::pathByAppendingComponent(directory, "__init__.py"_s)) == VirtualFileKind::File) {
        name = makeString(FileSystem::pathFileName(directory), '.', name);
        directory = FileSystem::parentPath(directory);
    }

    // The program itself is not run a second time under another name.
    if (JSValue main = rt.modules()->get(rt.names.dMain()); main && PyModule::is(main)) {
        JSValue file = PyModule::from(main)->dict()->get(rt.names.dFile());
        if (file && file.isString() && asString(file)->tryGetValue() == path)
            return main;
    }

    if (auto* searchPath = tryAsList(rt.sysDict()->get(rt.names.path()))) {
        JSString* entry = makeStr(vm, directory);
        bool isOnPath = contains(globalObject, searchPath, entry);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isOnPath) {
            listInsert(globalObject, searchPath, 0, entry);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    RELEASE_AND_RETURN(scope, importModule(globalObject, internString(vm, name)));
}

int runMainFile(JSGlobalObject* globalObject, const String& path, std::span<const uint8_t> source, const Vector<String>& arguments)
{
    int status = runMainModule(globalObject, path, source, arguments).value_or(0);
    bool flushed = finalizeInterpreter(globalObject);
    dieIfStatusIsSignal(status);
    return flushed ? status : statusIfNotFlushed;
}

// The module traceback has the lines of such source from there.
void registerSourceWithLinecache(JSGlobalObject* globalObject, JSValue code, std::span<const uint8_t> source, const String& filename)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue linecache = importModule(globalObject, "linecache"_s);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    callMethod(globalObject, linecache, internString(vm, "_register_code"_s), code, makeStr(vm, String::fromUTF8ReplacingInvalidSequences(source)), makeStr(vm, filename));
}

void runSourceInMain(JSGlobalObject* globalObject, std::span<const uint8_t> source, const String& filename, int flags, JSValue* unfinished)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The filters of warnings may go by the name of the module.
    JSValue code = compileSource(globalObject, source, filename, "exec"_s, flags, -1, ensureMainModule(globalObject)->dict()->get(runtime(globalObject).names.dName()));
    RETURN_IF_EXCEPTION(scope, void());
    if (flags & CompileSourceIsShown) {
        registerSourceWithLinecache(globalObject, code, source, filename);
        RETURN_IF_EXCEPTION(scope, void());
    }
    RELEASE_AND_RETURN(scope, runCodeInMain(globalObject, code, unfinished));
}

void runCodeInMain(JSGlobalObject* globalObject, JSValue code, JSValue* unfinished)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* globals = ensureMainModule(globalObject)->dict();
    JSValue result = runCode(globalObject, code, globals, globals);
    RETURN_IF_EXCEPTION(scope, void());
    if (!PyCode::from(code)->flags().contains(CodeFlag::Coroutine))
        return;
    JSValue run = unfinished ? eventLoopFunction(globalObject, "run"_s) : JSValue();
    RETURN_IF_EXCEPTION(scope, void());
    if (!run)
        RELEASE_AND_RETURN(scope, runTopLevelCoroutine(globalObject, PyGenerator::from(result)));
    JSValue promise = call(globalObject, run, result);
    RETURN_IF_EXCEPTION(scope, void());
    *unfinished = promise;
}

std::optional<int> mainModuleEnded(JSGlobalObject* globalObject, JSValue exception)
{
    Runtime& rt = runtime(globalObject);
    auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    std::optional<int> status;
    if (exception) {
        {
            auto scope = DECLARE_THROW_SCOPE(rt.vm());
            JSValue raised = exceptionFromThrownValue(globalObject, exception);
            if (!scope.exception())
                throwPythonException(globalObject, scope, raised);
        }
        EXCEPTION_ASSERT_UNUSED(topScope, topScope.exception());
        status = handleUncaughtExceptionOfFile(globalObject);
    }
    flushStandardStream(globalObject, rt.names.stderr_());
    flushStandardStream(globalObject, rt.names.stdout_());
    return status;
}

std::optional<int> runMainModule(JSGlobalObject* globalObject, const String& givenPath, std::span<const uint8_t> source, const Vector<String>& arguments, JSValue* unfinished)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Runtime& rt = Runtime::ensure(globalObject);
    if (scope.exception()) [[unlikely]]
        return handleUncaughtException(globalObject);
    String path = absolutePath(givenPath);

    MarkedArgumentBuffer argv;
    argv.append(makeStr(vm, givenPath));
    for (auto& argument : arguments)
        argv.append(makeStr(vm, argument));
    rt.sysDict()->set(vm, internString(vm, "argv"_s), newList(globalObject, spanOf(argv)));
    if (configuration().originalArguments.isEmpty())
        rt.sysDict()->set(vm, internString(vm, "orig_argv"_s), newList(globalObject, spanOf(argv)));

    // The directory of the script comes first on sys.path.
    if (auto* searchPath = tryAsList(rt.sysDict()->get(rt.names.path())); searchPath && !configuration().safePath) {
        String real = realPath(path);
        configuration().firstSearchPath = FileSystem::parentPath(real.isEmpty() ? path : real);
        listInsert(globalObject, searchPath, 0, makeStr(vm, configuration().firstSearchPath));
        scope.assertNoException();
    }

    std::optional<int> status;
    installSignalHandlers(globalObject);
    if (!scope.exception()) {
        Runtime::EventLoopPositionScope position(rt, false);
        runFileInMain(globalObject, path, source, unfinished);
    }
    if (!scope.exception() && unfinished && *unfinished)
        return std::nullopt;
    if (scope.exception())
        status = handleUncaughtExceptionOfFile(globalObject);
    flushStandardStream(globalObject, rt.names.stderr_());
    flushStandardStream(globalObject, rt.names.stdout_());
    return status;
}

} // namespace Python
} // namespace JSC
