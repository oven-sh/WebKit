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

#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonSequences.h"
#include "PythonStrings.h"
#include <wtf/SafeStrerror.h>
#include <wtf/text/StringBuilder.h>

// BaseException and the classes derived from it. This is CPython's Objects/exceptions.c.

namespace JSC { namespace Python {

// ---- BaseException

static PyTuple* argumentsAfterFirst(JSGlobalObject* globalObject, const NativeArguments& args)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = PyTuple::create(globalObject, args.size() ? args.size() - 1 : 0);
    for (unsigned i = 1; i < args.size(); ++i)
        tuple->initializeAt(vm, i - 1, args[i]);
    return tuple;
}

PYTHON_NATIVE(exceptionNew)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isClass(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "BaseException.__new__(X): X is not a type object"_s));
    PyException* exception = PyException::create(vm, asType(args[0]));
    exception->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    return JSValue::encode(exception);
}

PYTHON_NATIVE(exceptionInit)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeName(globalObject, args[0]), "() takes no keyword arguments"_s)));
    asObject(args[0])->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    RETURN_NONE();
}

PYTHON_NATIVE(exceptionStr)
{
    NATIVE_PROLOGUE();
    String text = strOfException(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(exceptionWithTraceback)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    asObject(args.at(0))->putDirect(vm, names.private_traceback, args.at(1));
    return JSValue::encode(args[0]);
}

// False if it raised.
static bool addNote(JSGlobalObject* globalObject, JSValue exception, JSValue note)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSValue notes = getAttributeIfPresent(globalObject, exception, names.dunder_notes);
    RETURN_IF_EXCEPTION(scope, false);
    if (!notes) {
        notes = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        setAttribute(globalObject, exception, names.dunder_notes, notes);
        RETURN_IF_EXCEPTION(scope, false);
    } else if (!isList(notes)) {
        raiseTypeError(globalObject, scope, "Cannot add note: __notes__ is not a list"_s);
        return false;
    }
    listAppend(globalObject, asList(notes), note);
    return true;
}

void addNoteToRaised(JSGlobalObject* globalObject, const String& note)
{
    addNoteToRaised(globalObject, [&] { return note; });
}

void addNoteToRaised(JSGlobalObject* globalObject, const ScopedLambda<String()>& makeNote)
{
    VM& vm = globalObject->vm();
    Exception* raised;
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        raised = scope.exception();
        if (!raised || vm.isTerminationException(raised) || !isInstance(globalObject, raised->value(), globalObject->pyRealm()->typeBaseException()))
            return;
        scope.clearException();
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What goes wrong with that is raised in its stead.
    String note = makeNote();
    RETURN_IF_EXCEPTION(scope, void());
    if (addNote(globalObject, raised->value(), jsString(vm, note)))
        throwException(globalObject, scope, raised);
}

PYTHON_NATIVE(exceptionAddNote)
{
    NATIVE_PROLOGUE();
    if (args.size() != 2 || args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("BaseException.add_note() takes exactly one argument ("_s, args.size() - 1, " given)"_s)));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("add_note() argument must be str, not "_s, isNone(args[1]) ? "None"_str : typeName(globalObject, args[1]))));
    addNote(globalObject, args[0], args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// An attribute of an exception that CPython keeps in a field of a C struct. It is None until it is set.
#define EXCEPTION_PROPERTY(getterName, setterName, privateName, whenAbsent) \
    static JSValue getterName(JSGlobalObject* globalObject, JSValue self) \
    { \
        JSValue value = self.isObject() ? asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().privateName) : JSValue(); \
        return value ? value : whenAbsent; \
    } \
    static void setterName(JSGlobalObject* globalObject, JSValue self, JSValue value) \
    { \
        asObject(self)->putDirect(globalObject->vm(), globalObject->vm().pythonNames().privateName, value ? value : jsUndefined()); \
    }

EXCEPTION_PROPERTY(getTraceback, setTracebackUnchecked, private_traceback, jsUndefined())
EXCEPTION_PROPERTY(getContext, setContextUnchecked, private_context, jsUndefined())
EXCEPTION_PROPERTY(getSuppressContext, setSuppressContextUnchecked, private_suppressContext, jsBoolean(false))

static bool isExceptionOrNone(JSGlobalObject* globalObject, JSValue value)
{
    return value && (isNone(value) || typeOf(globalObject, value)->isExceptionType());
}

static void setTraceback(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "__traceback__ may not be deleted"_s);
        return;
    }
    if (!isNone(value) && typeOf(globalObject, value) != globalObject->pyRealm()->typeTraceback()) {
        raiseTypeError(globalObject, scope, "__traceback__ must be a traceback or None"_s);
        return;
    }
    setTracebackUnchecked(globalObject, self, value);
}

static void setContextChecked(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isExceptionOrNone(globalObject, value)) {
        raiseTypeError(globalObject, scope, value ? "exception context must be None or derive from BaseException"_s : "__context__ may not be deleted"_s);
        return;
    }
    setContextUnchecked(globalObject, self, value);
}

static void setSuppressContext(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !value.isBoolean()) {
        raiseTypeError(globalObject, scope, value ? "attribute value type must be bool"_s : "can't delete numeric/char attribute"_s);
        return;
    }
    setSuppressContextUnchecked(globalObject, self, value);
}

static JSValue getCause(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = self.isObject() ? asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_cause) : JSValue();
    return value ? value : jsUndefined();
}

static void setCause(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isExceptionOrNone(globalObject, value)) {
        raiseTypeError(globalObject, scope, value ? "exception cause must be None or derive from BaseException"_s : "__cause__ may not be deleted"_s);
        return;
    }
    asObject(self)->putDirect(vm, vm.pythonNames().private_cause, value ? value : jsUndefined());
    asObject(self)->putDirect(vm, vm.pythonNames().private_suppressContext, jsBoolean(true));
}

static JSValue getArgs(JSGlobalObject* globalObject, JSValue self)
{
    return exceptionArguments(globalObject, self);
}

static void setArgs(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "args may not be deleted"_s);
        return;
    }
    PyTuple* tuple = tupleFromIterable(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    asObject(self)->putDirect(vm, vm.pythonNames().private_args, tuple);
}

// StopIteration(value): .value is the first argument.
PYTHON_NATIVE(stopIterationInit)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeName(globalObject, args[0]), "() takes no keyword arguments"_s)));
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    self->putDirect(vm, names.field_value, args.size() > 1 ? args[1] : jsUndefined());
    RETURN_NONE();
}

// SystemExit(code): .code is the argument, or all of them if there are several.
PYTHON_NATIVE(systemExitInit)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeName(globalObject, args[0]), "() takes no keyword arguments"_s)));
    JSObject* self = asObject(args[0]);
    PyTuple* arguments = argumentsAfterFirst(globalObject, args);
    self->putDirect(vm, names.private_args, arguments);
    if (args.size() > 1)
        self->putDirect(vm, names.field_code, args.size() == 2 ? args[1] : JSValue(arguments));
    RETURN_NONE();
}

PYTHON_NATIVE(keyErrorStr)
{
    NATIVE_PROLOGUE();
    // A key that was not found is shown as it would be written, so that '' and ' ' can be told apart.
    auto* arguments = uncheckedDowncast<PyTuple>(getArgs(globalObject, args.at(0)).asCell());
    String text = arguments->length() == 1 ? repr(globalObject, arguments->at(0)) : strOfException(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

enum class KeywordException : uint8_t { Import, Attribute, Name };

// ImportError(msg, name=, path=, name_from=), AttributeError(msg, name=, obj=) and NameError(msg, name=)
PYTHON_NATIVE(exceptionInitWithKeywords)
{
    auto which = unpack<KeywordException>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String name = args.keywordName(i)->value(globalObject);
        const Identifier* field = nullptr;
        if (name == "name"_s)
            field = &names.field_name;
        else if (which == KeywordException::Import && name == "path"_s)
            field = &names.field_path;
        else if (which == KeywordException::Import && name == "name_from"_s)
            field = &names.field_nameFrom;
        else if (which == KeywordException::Attribute && name == "obj"_s)
            field = &names.field_object;
        if (!field)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString(which == KeywordException::Import ? "ImportError"_s : which == KeywordException::Attribute ? "AttributeError"_s : "NameError"_s, "() got an unexpected keyword argument '"_s, name, '\'')));
        self->putDirect(vm, *field, args.keywordValue(i));
    }
    if (which == KeywordException::Import && args.size() == 2)
        self->putDirect(vm, names.field_message, args[1]);
    RETURN_NONE();
}

PYTHON_NATIVE(importErrorStr)
{
    NATIVE_PROLOGUE();
    JSValue message = asObject(args.at(0))->getDirect(vm, names.field_message);
    String text = message && message.isString() ? String(asString(message)->value(globalObject)) : strOfException(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

// The class for an error number, if there is one of its own.
static std::optional<BuiltinType> osErrorTypeFor(int errorNumber)
{
    switch (errorNumber) {
    case EAGAIN:
    case EALREADY:
    case EINPROGRESS:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return BuiltinType::BlockingIOError;
    case EPIPE:
    case ESHUTDOWN:
        return BuiltinType::BrokenPipeError;
    case ECHILD:
        return BuiltinType::ChildProcessError;
    case ECONNABORTED:
        return BuiltinType::ConnectionAbortedError;
    case ECONNREFUSED:
        return BuiltinType::ConnectionRefusedError;
    case ECONNRESET:
        return BuiltinType::ConnectionResetError;
    case EEXIST:
        return BuiltinType::FileExistsError;
    case ENOENT:
        return BuiltinType::FileNotFoundError;
    case EISDIR:
        return BuiltinType::IsADirectoryError;
    case ENOTDIR:
        return BuiltinType::NotADirectoryError;
    case EINTR:
        return BuiltinType::InterruptedError;
    case EACCES:
    case EPERM:
        return BuiltinType::PermissionError;
    case ESRCH:
        return BuiltinType::ProcessLookupError;
    case ETIMEDOUT:
        return BuiltinType::TimeoutError;
    default:
        return std::nullopt;
    }
}

// If a class derived from OSError has an __init__() of its own and not a __new__(), whatever else it is given is not for __new__() to make anything of. So it is
// all left to OSError.__init__(), for that __init__() to call with what is meant for it.
static bool leavesItToInit(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyType* osError = globalObject->pyRealm()->typeOSError();
    return type->lookup(vm, names.dunder_init) != osError->lookupOwn(vm, names.dunder_init) && type->lookup(vm, names.dunder_new) == osError->lookupOwn(vm, names.dunder_new);
}

// OSError(errno, strerror[, filename[, winerror[, filename2]]]). With two to five arguments they are those. Otherwise they are only args.
static void fillOSError(JSGlobalObject* globalObject, JSObject* self, const NativeArguments& args)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    unsigned count = args.size() - 1;
    JSValue arguments = argumentsAfterFirst(globalObject, args);
    bool areFields = count >= 2 && count <= 5;
    JSValue filename = areFields && count >= 3 ? args[3] : JSValue();
    JSValue filename2 = areFields && count == 5 ? args[5] : JSValue();

    if (filename && !isNone(filename)) {
        PyType* type = typeOf(globalObject, filename);
        bool isNumber = type->lookup(vm, names.dunder_index) || type->lookup(vm, names.dunder_int) || type->lookup(vm, names.dunder_float) || tryComplex(filename);
        if (isExactly(globalObject, self, globalObject->pyRealm()->typeBlockingIOError()) && isNumber) {
            // The third argument of a BlockingIOError can be how many characters were written.
            auto written = toIndex(globalObject, filename);
            if (scope.exception()) {
                if (catchException(globalObject, BuiltinType::IndexError))
                    raiseValueError(globalObject, scope, makeString("cannot fit '"_s, type->nameString(globalObject), "' into an index-sized integer"_s));
                return;
            }
            if (*written != -1)
                self->putDirect(vm, names.field_written, intFromInt64(globalObject, *written));
        } else {
            self->putDirect(vm, names.field_filename, filename);
            if (filename2 && !isNone(filename2))
                self->putDirect(vm, names.field_filename2, filename2);
            // The file names are not among the arguments.
            arguments = PyTuple::create(globalObject, { args[1], args[2] });
        }
    }
    if (areFields) {
        self->putDirect(vm, names.field_errorNumber, args[1]);
        self->putDirect(vm, names.field_errorText, args[2]);
    }
    self->putDirect(vm, names.private_args, arguments);
}

PYTHON_NATIVE(osErrorNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    bool isLeftToInit = leavesItToInit(globalObject, type);
    if (!isLeftToInit) {
        if (args.keywordCount())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString(type->nameString(globalObject), "() takes no keyword arguments"_s)));
        if (type == realm->typeOSError() && args.size() >= 3 && args.size() <= 6 && isInstance(globalObject, args[1], realm->typeInt())) {
            auto number = toIndex(globalObject, args[1], true);
            if (auto specific = osErrorTypeFor(static_cast<int>(std::clamp<int64_t>(*number, -1, std::numeric_limits<int>::max()))))
                type = realm->type(*specific);
        }
    }
    PyException* exception = PyException::create(vm, type);
    if (isLeftToInit) {
        exception->putDirect(vm, names.private_args, PyTuple::create(globalObject, { }));
        return JSValue::encode(exception);
    }
    fillOSError(globalObject, exception, args);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(exception);
}

PYTHON_NATIVE(osErrorInit)
{
    NATIVE_PROLOGUE();
    // Otherwise __new__() has done it.
    if (!leavesItToInit(globalObject, typeOf(globalObject, args[0])))
        RETURN_NONE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeName(globalObject, args[0]), "() takes no keyword arguments"_s)));
    fillOSError(globalObject, asObject(args[0]), args);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(osErrorStr)
{
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args.at(0));
    JSValue errorNumber = self->getDirect(vm, names.field_errorNumber);
    JSValue message = self->getDirect(vm, names.field_errorText);
    JSValue filename = self->getDirect(vm, names.field_filename);
    JSValue filename2 = self->getDirect(vm, names.field_filename2);
    auto text = [&] (JSValue value, bool asRepr) -> String { return asRepr ? repr(globalObject, value) : str(globalObject, value ? value : jsUndefined()); };
    if (filename) {
        String number = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String reason = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        String first = text(filename, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (filename2) {
            String second = text(filename2, true);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(jsString(vm, makeString("[Errno "_s, number, "] "_s, reason, ": "_s, first, " -> "_s, second)));
        }
        return JSValue::encode(jsString(vm, makeString("[Errno "_s, number, "] "_s, reason, ": "_s, first)));
    }
    if (errorNumber && message) {
        String number = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String reason = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsString(vm, makeString("[Errno "_s, number, "] "_s, reason)));
    }
    String plain = strOfException(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, plain));
}

JSValue raiseOSError(JSGlobalObject* globalObject, ThrowScope& scope, int errorNumber, JSValue filename)
{
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(errorNumber));
    arguments.append(jsString(vm, String::fromUTF8(safeStrerror(errorNumber).span())));
    if (filename)
        arguments.append(filename);
    JSValue exception = call(globalObject, globalObject->pyRealm()->typeOSError(), arguments);
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

std::optional<CString> toFileSystemPath(JSGlobalObject* globalObject, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue path = given;
    if (!path.isString() && bytesKindOf(path) != BytesKind::Bytes) {
        JSValue self;
        JSValue method = lookupSpecial(globalObject, path, Identifier::fromString(vm, "__fspath__"_s), self);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (!method) {
            raiseTypeError(globalObject, scope, makeString("expected str, bytes or os.PathLike object, not "_s, typeName(globalObject, path)));
            return std::nullopt;
        }
        path = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (!path.isString() && bytesKindOf(path) != BytesKind::Bytes) {
            raiseTypeError(globalObject, scope, makeString("expected "_s, typeName(globalObject, given), ".__fspath__() to return str or bytes, not "_s, typeName(globalObject, path)));
            return std::nullopt;
        }
    }
    std::optional<ByteVector> encoded;
    std::span<const uint8_t> bytes;
    if (path.isString()) {
        // What could not be decoded when the name was read is put back as it was.
        encoded = encodeString(globalObject, path, "utf-8"_s, "surrogateescape"_s);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        bytes = encoded->span();
    } else
        bytes = *builtinBufferOf(path);
    if (WTF::find(bytes, static_cast<uint8_t>(0)) != notFound) {
        raiseValueError(globalObject, scope, path.isString() ? "embedded null character"_s : "embedded null byte"_s);
        return std::nullopt;
    }
    return CString(byteCast<char>(bytes));
}

// SyntaxError(msg, (filename, lineno, offset, text, end_lineno, end_offset))
PYTHON_NATIVE(syntaxErrorInit)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeName(globalObject, args[0]), "() takes no keyword arguments"_s)));
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    if (args.size() >= 2)
        self->putDirect(vm, names.field_message, args[1]);
    if (args.size() == 3) {
        MarkedArgumentBuffer details;
        collect(globalObject, args[2], details);
        RETURN_IF_EXCEPTION(scope, { });
        if (details.size() < 4 || details.size() > 7)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("function takes "_s, details.size() < 4 ? "at least 4"_s : "at most 7"_s, " arguments ("_s, details.size(), " given)"_s)));
        if (details.size() == 5)
            return JSValue::encode(raiseTypeError(globalObject, scope, "end_offset must be provided when end_lineno is provided"_s));
        const Identifier* fields[] = { &names.field_filename, &names.field_line, &names.field_offset, &names.field_text, &names.field_endLine, &names.field_endOffset, &names.field_metadata };
        for (unsigned i = 0; i < details.size(); ++i)
            self->putDirect(vm, *fields[i], details.at(i));
    }
    RETURN_NONE();
}

PYTHON_NATIVE(syntaxErrorStr)
{
    NATIVE_PROLOGUE();
    if (!args[0].asCell()->inherits<PyException>()) {
        // One of JavaScript's has only a message.
        String text = strOfException(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsString(vm, text));
    }
    auto get = [&] (ASCIILiteral name) -> JSValue {
        JSValue value = getAttributeIfPresent(globalObject, args.at(0), Identifier::fromString(vm, name));
        return value ? value : jsUndefined();
    };
    JSValue message = get("msg"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue filename = get("filename"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue line = get("lineno"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String text = str(globalObject, message);
    RETURN_IF_EXCEPTION(scope, { });
    bool hasLine = classify(line).isInt() && !line.isBoolean();
    String lineText = hasLine ? str(globalObject, line) : String();
    if (filename.isString()) {
        String path = asString(filename)->value(globalObject);
        size_t slash = path.reverseFind('/');
        if (slash != notFound)
            path = path.substring(slash + 1);
        if (hasLine)
            return JSValue::encode(jsString(vm, makeString(text, " ("_s, path, ", line "_s, lineText, ')')));
        return JSValue::encode(jsString(vm, makeString(text, " ("_s, path, ')')));
    }
    if (hasLine)
        return JSValue::encode(jsString(vm, makeString(text, " (line "_s, lineText, ')')));
    return JSValue::encode(jsString(vm, text));
}

// ---- UnicodeEncodeError, UnicodeDecodeError and UnicodeTranslateError

enum class UnicodeError : uint8_t { Encode, Decode, Translate };

// start and end, which in CPython are numbers in the struct: 0 until they are set, and nothing but an int can be put there.
template<bool isEnd>
static JSValue getUnicodeErrorBound(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    JSValue value = asObject(self)->getDirect(vm, isEnd ? vm.pythonNames().field_end : vm.pythonNames().field_start);
    return value ? value : jsNumber(0);
}

template<bool isEnd>
static void setUnicodeErrorBound(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete numeric/char attribute"_s);
        return;
    }
    if (!isInstance(globalObject, value, globalObject->pyRealm()->typeInt())) {
        raiseTypeError(globalObject, scope, "an integer is required"_s);
        return;
    }
    auto index = toIndex(globalObject, value);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::IndexError))
            raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C ssize_t"_s);
        return;
    }
    asObject(self)->putDirect(vm, isEnd ? vm.pythonNames().field_end : vm.pythonNames().field_start, intFromInt64(globalObject, *index));
}

// UnicodeEncodeError(encoding, object, start, end, reason), UnicodeDecodeError the same, and UnicodeTranslateError(object, start, end, reason)
PYTHON_NATIVE(unicodeErrorInit)
{
    auto kind = unpack<UnicodeError>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeName(globalObject, args[0]), "() takes no keyword arguments"_s)));
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));

    bool hasEncoding = kind != UnicodeError::Translate;
    unsigned count = hasEncoding ? 5 : 4;
    if (args.size() - 1 != count)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("function takes exactly "_s, count, " arguments ("_s, args.size() - 1, " given)"_s)));
    auto checkString = [&] (unsigned position) {
        if (stringIn(args[position]))
            return true;
        raiseTypeError(globalObject, scope, makeString("argument "_s, position, " must be str, not "_s, isNone(args[position]) ? "None"_s : typeName(globalObject, args[position])));
        return false;
    };
    unsigned position = 1;
    if (hasEncoding && !checkString(position++))
        return { };
    JSValue object = args[position];
    if (kind != UnicodeError::Decode && !checkString(position))
        return { };
    ++position;
    int64_t range[2];
    for (auto& bound : range) {
        auto index = toIndex(globalObject, args[position++]);
        if (scope.exception()) {
            if (catchException(globalObject, BuiltinType::IndexError))
                raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C ssize_t"_s);
            return { };
        }
        bound = *index;
    }
    if (!checkString(position))
        return { };
    if (kind == UnicodeError::Decode && !typeOf(globalObject, object)->hasFlag(PyType::IsBytes)) {
        // What is kept is a copy, that nothing will change.
        auto buffer = bufferOf(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        object = newBytes(globalObject, *buffer);
    }
    if (hasEncoding)
        self->putDirect(vm, names.field_encoding, args[1]);
    self->putDirect(vm, names.field_subject, object);
    self->putDirect(vm, names.field_start, intFromInt64(globalObject, range[0]));
    self->putDirect(vm, names.field_end, intFromInt64(globalObject, range[1]));
    self->putDirect(vm, names.field_reason, args[position]);
    RETURN_NONE();
}

PYTHON_NATIVE(unicodeErrorStr)
{
    auto kind = unpack<UnicodeError>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args[0]);
    // As CPython writes what has been deleted.
    auto text = [&] (const Identifier& field) -> String {
        JSValue value = self->getDirect(vm, field);
        return value ? str(globalObject, value) : "<NULL>"_str;
    };
    // It has not been through __init__().
    JSValue object = self->getDirect(vm, names.field_subject);
    if (!object)
        return JSValue::encode(jsEmptyString(vm));

    // Any of them may have been set to anything since.
    String reason = text(names.field_reason);
    RETURN_IF_EXCEPTION(scope, { });
    String prefix;
    if (kind != UnicodeError::Translate) {
        String encoding = text(names.field_encoding);
        RETURN_IF_EXCEPTION(scope, { });
        prefix = makeString('\'', encoding, "' codec "_s);
    }
    bool isDecode = kind == UnicodeError::Decode;
    if (isDecode ? !typeOf(globalObject, object)->hasFlag(PyType::IsBytes) : !stringIn(object))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("UnicodeError 'object' attribute must be a "_s, isDecode ? "bytes"_s : "string"_s)));
    int64_t size = length(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t start = *tryInt64(getUnicodeErrorBound<false>(globalObject, self));
    int64_t end = *tryInt64(getUnicodeErrorBound<true>(globalObject, self));
    ASCIILiteral verb = kind == UnicodeError::Encode ? "encode"_s : isDecode ? "decode"_s : "translate"_s;

    if (start < 0 || start >= size || end < 0 || end > size || end != start + 1)
        return JSValue::encode(jsString(vm, makeString(prefix, "can't "_s, verb, isDecode ? " bytes"_s : " characters"_s, " in position "_s, start, '-', end - 1, ": "_s, reason)));
    if (isDecode)
        return JSValue::encode(jsString(vm, makeString(prefix, "can't decode byte 0x"_s, hex((*builtinBufferOf(object))[start], 2, Lowercase), " in position "_s, start, ": "_s, reason)));
    JSValue character = stringGetItem(globalObject, stringIn(object), intFromInt64(globalObject, start));
    RETURN_IF_EXCEPTION(scope, { });
    auto c = static_cast<unsigned>(*asString(character)->view(globalObject)->codePoints().begin());
    String escaped = c <= 0xFF ? makeString("\\x"_s, hex(c, 2, Lowercase)) : c <= 0xFFFF ? makeString("\\u"_s, hex(c, 4, Lowercase)) : makeString("\\U"_s, hex(c, 8, Lowercase));
    return JSValue::encode(jsString(vm, makeString(prefix, "can't "_s, verb, " character '"_s, escaped, "' in position "_s, start, ": "_s, reason)));
}

void initializeExceptionTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    PyType* base = realm->typeBaseException();
    addMethods(globalObject, base, {
        { "__new__"_s, exceptionNew, Kind::New },
        { "__init__"_s, exceptionInit },
        { "__str__"_s, exceptionStr },
        { "__repr__"_s, nativeRepr },
        { "with_traceback"_s, exceptionWithTraceback },
        { "add_note"_s, exceptionAddNote },
    });
    addGetSet(globalObject, base, "args"_s, getArgs, setArgs);
    addGetSet(globalObject, base, "__traceback__"_s, getTraceback, setTraceback);
    addGetSet(globalObject, base, "__cause__"_s, getCause, setCause);
    addGetSet(globalObject, base, "__context__"_s, getContext, setContextChecked);
    addGetSet(globalObject, base, "__suppress_context__"_s, getSuppressContext, setSuppressContext);

    // What in CPython is a field of the exception's struct: it is None until it is set, and is not in __dict__.
    auto& names = vm.pythonNames();
    auto addFields = [&] (PyType* type, std::initializer_list<const Identifier*> fields) {
        for (const Identifier* field : fields) {
            // The name of the field says what the attribute is called.
            String attribute = field->string();
            type->putDirect(vm, Identifier::fromString(vm, attribute), createMemberDescriptor(globalObject, type, jsString(vm, attribute), field, jsUndefined()));
        }
    };
    addMethods(globalObject, realm->typeStopIteration(), { { "__init__"_s, stopIterationInit } });
    addFields(realm->typeStopIteration(), { &names.field_value });
    addMethods(globalObject, realm->typeSystemExit(), { { "__init__"_s, systemExitInit } });
    addFields(realm->typeSystemExit(), { &names.field_code });
    addMethods(globalObject, realm->typeKeyError(), { { "__str__"_s, keyErrorStr } });
    addMethods(globalObject, realm->typeOSError(), {
        { "__new__"_s, osErrorNew, Kind::New },
        { "__init__"_s, osErrorInit },
        { "__str__"_s, osErrorStr },
    });
    addFields(realm->typeOSError(), { &names.field_errorNumber, &names.field_errorText, &names.field_filename, &names.field_filename2 });
    addMethods(globalObject, realm->typeSyntaxError(), {
        { "__init__"_s, syntaxErrorInit },
        { "__str__"_s, syntaxErrorStr },
    });
    addFields(realm->typeSyntaxError(), { &names.field_message, &names.field_filename, &names.field_line, &names.field_offset, &names.field_text, &names.field_endLine, &names.field_endOffset, &names.field_printFileAndLine, &names.field_metadata });
    for (auto [type, kind] : { std::pair { realm->typeUnicodeEncodeError(), UnicodeError::Encode }, std::pair { realm->typeUnicodeDecodeError(), UnicodeError::Decode }, std::pair { realm->typeUnicodeTranslateError(), UnicodeError::Translate } }) {
        addMethods(globalObject, type, {
            { "__init__"_s, unicodeErrorInit, Kind::Method, pack(kind) },
            { "__str__"_s, unicodeErrorStr, Kind::Method, pack(kind) },
        });
        addFields(type, { &names.field_encoding, &names.field_subject });
        addMember(globalObject, type, "start"_s, getUnicodeErrorBound<false>, setUnicodeErrorBound<false>);
        addMember(globalObject, type, "end"_s, getUnicodeErrorBound<true>, setUnicodeErrorBound<true>);
        addFields(type, { &names.field_reason });
    }
    addMethods(globalObject, realm->typeImportError(), {
        { "__init__"_s, exceptionInitWithKeywords, Kind::Method, pack(KeywordException::Import) },
        { "__str__"_s, importErrorStr },
    });
    addFields(realm->typeImportError(), { &names.field_message, &names.field_name, &names.field_path, &names.field_nameFrom });
    addMethods(globalObject, realm->typeAttributeError(), { { "__init__"_s, exceptionInitWithKeywords, Kind::Method, pack(KeywordException::Attribute) } });
    addFields(realm->typeAttributeError(), { &names.field_name, &names.field_object });
    addMethods(globalObject, realm->typeNameError(), { { "__init__"_s, exceptionInitWithKeywords, Kind::Method, pack(KeywordException::Name) } });
    addFields(realm->typeNameError(), { &names.field_name });

    // How many characters were written before a BlockingIOError. It is not there unless it has been said.
    addGetSet(globalObject, realm->typeOSError(), "characters_written"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue written = asObject(self)->getDirect(vm, vm.pythonNames().field_written);
        return written ? written : raise(globalObject, scope, BuiltinType::AttributeError, "characters_written"_s);
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        const Identifier& field = vm.pythonNames().field_written;
        if (!value) {
            if (!asObject(self)->getDirect(vm, field)) {
                raise(globalObject, scope, BuiltinType::AttributeError, "characters_written"_s);
                return;
            }
            scope.release();
            JSCell::deleteProperty(asObject(self), globalObject, field);
            return;
        }
        auto written = toIndex(globalObject, value);
        if (scope.exception()) {
            if (catchException(globalObject, BuiltinType::IndexError))
                raiseValueError(globalObject, scope, makeString("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
            return;
        }
        // In CPython -1 is how it is said that there is none.
        if (*written == -1) {
            scope.release();
            JSCell::deleteProperty(asObject(self), globalObject, field);
            return;
        }
        asObject(self)->putDirect(vm, field, intFromInt64(globalObject, *written));
    });

    // Most of them have a __new__ of their own, which is BaseException's.
#define ADD_NEW(name, pythonName, base, layout, flags) \
    if (!realm->type##name()->lookupOwn(vm, names.dunder_new)) \
        addMethodsThatCPythonHas(globalObject, realm->type##name(), { { "__new__"_s, exceptionNew, Kind::New } });
    FOR_EACH_PYTHON_EXCEPTION_TYPE(ADD_NEW)
#undef ADD_NEW
}

} } // namespace JSC::Python
