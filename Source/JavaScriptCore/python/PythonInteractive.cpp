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
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonCompiler.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonImportState.h"
#include "PythonLexer.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonSymbolTable.h"
#include "TopExceptionScope.h"
#if OS(UNIX)
#include <unistd.h>
#endif

// What is typed: input(), and the prompt at which statements are typed and run one at a time. Parser/myreadline.c of CPython, builtin_input() of its Python/bltinmodule.c, tok_underflow_interactive() of its
// Parser/tokenizer/file_tokenizer.c, and what has to do with a prompt in its Python/pythonrun.c.

namespace JSC { namespace Python {

// ---- A line

// PyOS_StdioReadline(), and my_fgets(): the prompt, and then a line of the process's own standard input, with what ends it. Empty at the end of the input. Nothing if it raised, as when it is interrupted.
//
// It is by way of stdio, as in CPython, and not by way of sys.stdin, which holds on to whatever it has read ahead. So which of the two gets what, of input that is not typed, is the same here as there.
static std::optional<Vector<char>> readTypedLine(JSGlobalObject* globalObject, const CString& prompt)
{
    fflush(stdout);
    if (!prompt.isNull())
        fputs(prompt.data(), stderr);
    fflush(stderr);

    Vector<char> line;
    do {
        char piece[BUFSIZ];
        errno = 0;
        clearerr(stdin);
        if (!fgets(piece, sizeof(piece), stdin)) {
            int error = errno;
            if (feof(stdin)) {
                clearerr(stdin);
                break;
            }
            if (error != EINTR)
                break;
            if (!checkSignals(globalObject))
                return std::nullopt;
            continue;
        }
        // As far as the first zero, if it has one in it, since that is where a string ends in C.
        line.append(unsafeSpan(piece));
    } while (line.isEmpty() || line.last() != '\n');
    return line;
}

static bool isTerminal(int descriptor)
{
#if OS(UNIX)
    return isatty(descriptor);
#else
    UNUSED_PARAM(descriptor);
    return false;
#endif
}

// ---- input()

// _PyFile_Flush(), of which nothing is made if it fails
static void flushAndForget(JSGlobalObject* globalObject, JSValue file)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    callMethodNamed(globalObject, file, vm.pythonNames().attribute_flush);
    if (scope.exception())
        catchException(globalObject, BuiltinType::BaseException);
}

// Whether file.fileno() is that, and that is a terminal. Nothing if it raised.
static std::optional<bool> isTerminalOfProcess(JSGlobalObject* globalObject, JSValue file, int descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue number = callMethodNamed(globalObject, file, Identifier::fromString(vm, "fileno"_s));
    if (scope.exception()) {
        if (!catchException(globalObject, BuiltinType::BaseException))
            return std::nullopt;
        return false;
    }
    auto given = toCLong(globalObject, number);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return *given == descriptor && isTerminal(descriptor);
}

// What input() does if what is typed comes straight from a terminal and what is printed goes straight to one. Empty, with nothing raised, if it turns out that it cannot be done this way after all.
static JSValue inputFromTerminal(JSGlobalObject* globalObject, JSValue input, JSValue output, JSValue prompt)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier encodingName = Identifier::fromString(vm, "encoding"_s);
    Identifier errorsName = Identifier::fromString(vm, "errors"_s);
    // Both are streams of text, so they have these. If not, it is done the other way.
    auto textAttribute = [&] (JSValue file, const Identifier& name) -> String {
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue value = getAttribute(globalObject, file, name);
        if (scope.exception()) {
            catchException(globalObject, BuiltinType::BaseException);
            return { };
        }
        if (!stringIn(value))
            return { };
        RELEASE_AND_RETURN(scope, textOfString(globalObject, value));
    };
    String inputEncoding = textAttribute(input, encodingName);
    RETURN_IF_EXCEPTION(scope, { });
    if (inputEncoding.isNull())
        return { };
    String inputErrors = textAttribute(input, errorsName);
    RETURN_IF_EXCEPTION(scope, { });
    if (inputErrors.isNull())
        return { };
    flushAndForget(globalObject, output);
    RETURN_IF_EXCEPTION(scope, { });

    CString promptBytes = ""_span;
    if (prompt) {
        // As the output would encode it
        String outputEncoding = textAttribute(output, encodingName);
        RETURN_IF_EXCEPTION(scope, { });
        if (outputEncoding.isNull())
            return { };
        String outputErrors = textAttribute(output, errorsName);
        RETURN_IF_EXCEPTION(scope, { });
        if (outputErrors.isNull())
            return { };
        JSValue text = strObject(globalObject, prompt);
        RETURN_IF_EXCEPTION(scope, { });
        auto encoded = encodeString(globalObject, text, outputEncoding, outputErrors);
        RETURN_IF_EXCEPTION(scope, { });
        if (WTF::find(encoded->span(), static_cast<uint8_t>(0)) != notFound)
            return raiseValueError(globalObject, scope, "input: prompt string cannot contain null characters"_s);
        promptBytes = CString(byteCast<char>(encoded->span()));
    }

    auto line = readTypedLine(globalObject, promptBytes);
    RETURN_IF_EXCEPTION(scope, { });
    if (!line)
        return raise(globalObject, scope, BuiltinType::KeyboardInterrupt, JSValue());
    if (line->isEmpty())
        return raise(globalObject, scope, BuiltinType::EOFError, JSValue());
    // Without what ends it
    auto bytes = line->span().first(line->size() - 1);
    if (!bytes.empty() && bytes.back() == '\r')
        bytes = bytes.first(bytes.size() - 1);
    JSValue result = decodeBytesToObject(globalObject, byteCast<uint8_t>(bytes), inputEncoding, inputErrors);
    RETURN_IF_EXCEPTION(scope, { });
    if (!audit(globalObject, "builtins.input/result"_s, result))
        return { };
    return result;
}

// input(prompt='', /)
PYTHON_SHARED_NATIVE(builtinInput)
{
    NATIVE_PROLOGUE();
    JSValue prompt = args.at(0);
    JSValue files[3];
    unsigned index = 0;
    for (ASCIILiteral name : { "stdin"_s, "stdout"_s, "stderr"_s }) {
        JSValue file = sysAttribute(globalObject, name);
        if (!file || isNone(file))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("lost sys."_s, name)));
        files[index++] = file;
    }
    auto [input, output, errors] = files;
    if (!audit(globalObject, "builtins.input"_s, prompt ? prompt : jsUndefined()))
        return { };
    flushAndForget(globalObject, errors);
    RETURN_IF_EXCEPTION(scope, { });

    // The line is only read from the process's own input if that is what sys.stdin and sys.stdout are.
    auto isTyped = isTerminalOfProcess(globalObject, input, fileno(stdin));
    RETURN_IF_EXCEPTION(scope, { });
    if (*isTyped) {
        isTyped = isTerminalOfProcess(globalObject, output, fileno(stdout));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (*isTyped) {
        JSValue result = inputFromTerminal(globalObject, input, output, prompt);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return JSValue::encode(result);
    }

    if (prompt) {
        // PyFile_WriteObject(), with Py_PRINT_RAW
        JSValue text = strObject(globalObject, prompt);
        RETURN_IF_EXCEPTION(scope, { });
        callMethodNamed(globalObject, output, names.attribute_write, text);
        RETURN_IF_EXCEPTION(scope, { });
    }
    flushAndForget(globalObject, output);
    RETURN_IF_EXCEPTION(scope, { });

    // PyFile_GetLine(), with less than nothing for how much
    JSValue line = callMethodNamed(globalObject, input, Identifier::fromString(vm, "readline"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (typeOf(globalObject, line)->hasFlag(PyType::IsBytes)) {
        auto bytes = *builtinBufferOf(line);
        if (bytes.empty())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::EOFError, "EOF when reading a line"_s));
        if (bytes.back() != '\n')
            return JSValue::encode(line);
        RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, bytes.first(bytes.size() - 1))));
    }
    if (!stringIn(line))
        return JSValue::encode(raiseTypeError(globalObject, scope, "object.readline() returned non-string"_s));
    String text = textOfString(globalObject, line);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.isEmpty())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::EOFError, "EOF when reading a line"_s));
    if (text[text.length() - 1] != '\n')
        return JSValue::encode(line);
    return JSValue::encode(jsString(vm, text.left(text.length() - 1)));
}

// ---- The prompt

namespace {

// tok_underflow_interactive()
class PromptedLines final : public LineSource {
public:
    PromptedLines(JSGlobalObject* globalObject, const String& encoding, CString&& prompt, CString&& nextPrompt)
        : m_globalObject(globalObject)
        , m_encoding(encoding)
        , m_prompt(WTF::move(prompt))
        , m_nextPrompt(WTF::move(nextPrompt))
    {
    }

    Result readLine(Vector<char16_t>& buffer) final
    {
        VM& vm = m_globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto line = readTypedLine(m_globalObject, m_prompt);
        m_prompt = m_nextPrompt;
        if (!line || line->isEmpty()) {
            // The cursor is still after the prompt.
            Exception* raised = scope.exception() ? takeRaisedException(vm) : nullptr;
            RETURN_IF_EXCEPTION(scope, Result::Failed);
            writeToStandardError(m_globalObject, "\n"_s);
            if (line)
                return Result::Line;
            if (raised)
                restoreRaisedException(m_globalObject, raised);
            else
                raise(m_globalObject, scope, BuiltinType::KeyboardInterrupt, JSValue());
            return Result::Failed;
        }

        // _PyTokenizer_translate_newlines(): however a line is ended, it is ended with '\n'.
        Vector<uint8_t> translated;
        bool skipsNewline = false;
        for (char c : *line) {
            if (std::exchange(skipsNewline, false) && c == '\n')
                continue;
            if (c == '\r') {
                skipsNewline = true;
                c = '\n';
            }
            translated.append(c);
        }
        String text = decodeBytes(m_globalObject, translated.span(), m_encoding, String());
        if (scope.exception()) [[unlikely]] {
            // _Pypegen_raise_decode_error()
            PyRealm* realm = m_globalObject->pyRealm();
            JSValue exception = scope.exception()->value();
            ASCIILiteral kind = isInstance(m_globalObject, exception, realm->type(BuiltinType::UnicodeError)) ? "unicode error"_s : isInstance(m_globalObject, exception, realm->type(BuiltinType::ValueError)) ? "value error"_s : ASCIILiteral();
            if (kind.isNull() || !takeRaisedException(vm))
                return Result::Failed;
            String message = str(m_globalObject, exception);
            RETURN_IF_EXCEPTION(scope, Result::Failed);
            whyNotText = concatenate('(', kind, ") "_s, message);
            return Result::IsNotText;
        }
        for (char16_t unit : StringView(text).codeUnits())
            buffer.append(unit);
        return Result::Line;
    }

private:
    JSGlobalObject* m_globalObject;
    String m_encoding;
    CString m_prompt;
    CString m_nextPrompt;
};

enum class Typed : uint8_t { WasRun, WasTheEnd, Raised };

} // anonymous namespace

// str(sys.ps1), or nothing at all if there is none or that cannot be had
static CString promptFrom(JSGlobalObject* globalObject, ASCIILiteral name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    JSValue value = sysAttribute(globalObject, name);
    if (!value)
        return ""_span;
    String text = str(globalObject, value);
    if (scope.exception()) {
        scope.clearException();
        return ""_span;
    }
    return text.utf8();
}

// _PyRun_InteractiveOne()
static Typed runOneTypedStatement(JSGlobalObject* globalObject, unsigned& futureFeatures)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto filename = "<stdin>"_s;

    // pyrun_one_parse_ast()
    String encoding;
    if (JSValue input = sysAttribute(globalObject, "stdin"_s); input && !isNone(input)) {
        JSValue value = getAttributeIfPresent(globalObject, input, Identifier::fromString(vm, "encoding"_s));
        if (scope.exception()) {
            if (!catchException(globalObject, BuiltinType::BaseException))
                return Typed::Raised;
        } else if (value && stringIn(value)) {
            encoding = textOfString(globalObject, value);
            RETURN_IF_EXCEPTION(scope, Typed::Raised);
        }
    }
    CString prompt = promptFrom(globalObject, "ps1"_s);
    CString nextPrompt = promptFrom(globalObject, "ps2"_s);
    PromptedLines lines(globalObject, encoding, WTF::move(prompt), WTF::move(nextPrompt));
    String text;
    switch (readTypedStatement(globalObject, lines, filename, futureFeatures, text)) {
    case TypedStatement::IsTheEnd:
        return Typed::WasTheEnd;
    case TypedStatement::Raised:
        return Typed::Raised;
    case TypedStatement::IsRead:
        break;
    }

    JSObject* module = addModule(globalObject, jsNontrivialString(vm, "__main__"_s));
    RETURN_IF_EXCEPTION(scope, Typed::Raised);

    // run_mod(), with the source. Each thing that is typed goes by a name of its own, so that what shows where something went wrong can find the source of it: get_interactive_filename()
    String ownFilename = concatenate("<stdin-"_s, importState(globalObject).typedStatementCount++, '>');
    auto run = [&] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        FunctionExecutable* executable = compileSource(globalObject, makeSource(text, SourceOrigin(), ownFilename), CodeKind::Interactive, false, futureFeatures | IsTypedAtPrompt);
        RETURN_IF_EXCEPTION(scope, void());
        // What `from __future__ import` asks for goes on being so.
        futureFeatures |= infoOfExecutable(executable).futureFeatures & FutureFeaturesMask;
        JSObject* code = codeObjectFor(globalObject, executable);
        JSValue registerCode = importModuleAttribute(globalObject, "linecache"_s, "_register_code"_s);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isCallable(globalObject, registerCode)) {
            raiseValueError(globalObject, scope, "linecache._register_code is not callable"_s);
            return;
        }
        call(globalObject, registerCode, code, jsString(vm, text), jsNontrivialString(vm, filename));
        RETURN_IF_EXCEPTION(scope, void());
        if (!audit(globalObject, "exec"_s, code))
            return;
        scope.release();
        call(globalObject, bindToGlobals(globalObject, executable, module));
    };
    run();
    if (!scope.exception()) {
        flushStandardStreams(globalObject);
        RETURN_IF_EXCEPTION(scope, Typed::Raised);
        return Typed::WasRun;
    }

    // If it is that something is wrong with the source, the line is the one that was typed.
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, Typed::Raised);
    JSValue exception = raised->value();
    if (isInstance(globalObject, exception, globalObject->pyRealm()->type(BuiltinType::SyntaxError))) {
        [&] {
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            JSValue lineNumber = getAttribute(globalObject, exception, Identifier::fromString(vm, "lineno"_s));
            auto number = scope.exception() ? std::nullopt : toCInt(globalObject, lineNumber);
            if (!scope.exception() && *number > 0) {
                JSValue splitLines = callMethodNamed(globalObject, jsString(vm, text), Identifier::fromString(vm, "splitlines"_s), jsBoolean(true));
                if (!scope.exception() && static_cast<unsigned>(*number) <= asList(splitLines)->length())
                    setAttribute(globalObject, exception, Identifier::fromString(vm, "text"_s), listGet(globalObject, asList(splitLines), *number - 1));
            }
            scope.clearException();
        }();
    }
    restoreRaisedException(globalObject, raised);
    return Typed::Raised;
}

// _PyRun_InteractiveLoop()
void runInteractiveLoop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    for (auto [name, prompt] : { std::pair { "ps1"_s, ">>> "_s }, std::pair { "ps2"_s, "... "_s } }) {
        if (!sysAttribute(globalObject, name))
            putStoredAttribute(vm, realm->sysModule(), Identifier::fromString(vm, name), jsNontrivialString(vm, prompt));
    }

    unsigned futureFeatures = 0;
    unsigned memoryErrorsInARow = 0;
    Typed outcome;
    do {
        outcome = runOneTypedStatement(globalObject, futureFeatures);
        if (outcome != Typed::Raised) {
            memoryErrorsInARow = 0;
            continue;
        }
        if (vm.hasPendingTerminationException())
            return;
        JSValue exception = scope.exception()->value();
        // One statement may run out of memory. If they all do there is no going on.
        if (isInstance(globalObject, exception, realm->type(BuiltinType::MemoryError))) {
            if (++memoryErrorsInARow > 16)
                return;
        } else
            memoryErrorsInARow = 0;
        if (!realm->configuration().inspect && isInstance(globalObject, exception, realm->typeSystemExit()))
            return;
        printRaisedException(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    } while (outcome != Typed::WasTheEnd);
}

// sys._baserepl()
PYTHON_SHARED_NATIVE(sysBaseREPL)
{
    NATIVE_PROLOGUE();
    // PyRun_AnyFileExFlags(), which sees to whatever is raised
    runStandardInput(globalObject);
    if (scope.exception()) {
        // PyErr_Print() ends the process there and then if it is SystemExit. Here that is left to whoever has the ending of it.
        if (vm.hasPendingTerminationException() || (!realm->configuration().inspect && isInstance(globalObject, scope.exception()->value(), realm->typeSystemExit())))
            return { };
        printRaisedException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

} } // namespace JSC::Python
