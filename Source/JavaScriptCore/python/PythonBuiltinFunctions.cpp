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

#include "JSLexicalEnvironment.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonGenerators.h"
#include <wtf/SafeStrerror.h>
#include <wtf/text/StringBuilder.h>

// The exceptions, and the functions of the builtins module.

namespace JSC { namespace Python {

bool sortValues(JSGlobalObject*, MarkedArgumentBuffer& values, JSValue keyFunction, bool reverse, MarkedArgumentBuffer& sorted);

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
    if (!args.size() || !isType(args[0]))
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

PYTHON_NATIVE(exceptionAddNote)
{
    NATIVE_PROLOGUE();
    if (args.size() != 2 || args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("BaseException.add_note() takes exactly one argument ("_s, args.size() - 1, " given)"_s)));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("add_note() argument must be str, not "_s, isNone(args[1]) ? "None"_str : typeName(globalObject, args[1]))));
    JSValue notes = getAttributeIfPresent(globalObject, args[0], names.dunder_notes);
    RETURN_IF_EXCEPTION(scope, { });
    if (!notes) {
        notes = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        setAttribute(globalObject, args[0], names.dunder_notes, notes);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (!isList(notes))
        return JSValue::encode(raiseTypeError(globalObject, scope, "Cannot add note: __notes__ is not a list"_s));
    listAppend(globalObject, asList(notes), args[1]);
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

// OSError(errno, strerror[, filename[, winerror[, filename2]]]). With two to five arguments they are those. Otherwise they are only args.
static void fillOSError(JSGlobalObject* globalObject, JSObject* self, const NativeArguments& args)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    unsigned count = args.size() - 1;
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    if (count < 2 || count > 5)
        return;
    self->putDirect(vm, names.field_errorNumber, args[1]);
    self->putDirect(vm, names.field_errorText, args[2]);
    if (count >= 3) {
        self->putDirect(vm, names.field_filename, args[3]);
        self->putDirect(vm, vm.pythonNames().private_args, PyTuple::create(globalObject, { args[1], args[2] }));
    }
    if (count == 5)
        self->putDirect(vm, names.field_filename2, args[5]);
}

PYTHON_NATIVE(osErrorNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    PyType* type = asType(args.at(0));
    if (type == realm->typeOSError() && args.size() >= 3 && args.size() <= 6 && args[1].isInt32()) {
        if (auto specific = osErrorTypeFor(args[1].asInt32()))
            type = realm->type(*specific);
    }
    PyException* exception = PyException::create(vm, type);
    fillOSError(globalObject, exception, args);
    return JSValue::encode(exception);
}

PYTHON_NATIVE(osErrorInit)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    // __new__ has done it, unless a class derived from this one has a __new__ of its own.
    PyType* type = typeOf(globalObject, args[0]);
    JSValue constructor = type->lookup(vm, names.dunder_new);
    JSValue own = realm->typeOSError()->lookupOwn(vm, names.dunder_new);
    if (constructor != own)
        fillOSError(globalObject, asObject(args[0]), args);
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
    if (filename && !isNone(filename)) {
        String number = text(errorNumber, false);
        RETURN_IF_EXCEPTION(scope, { });
        String reason = text(message, false);
        RETURN_IF_EXCEPTION(scope, { });
        String first = text(filename, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (filename2 && !isNone(filename2)) {
            String second = text(filename2, true);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(jsString(vm, makeString("[Errno "_s, number, "] "_s, reason, ": "_s, first, " -> "_s, second)));
        }
        return JSValue::encode(jsString(vm, makeString("[Errno "_s, number, "] "_s, reason, ": "_s, first)));
    }
    if (errorNumber && message && !isNone(errorNumber) && !isNone(message)) {
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
    if (!path.isString() && !tryBufferOf(path)) {
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
        bytes = *tryBufferOf(path);
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
        const Identifier* fields[] = { &names.field_filename, &names.field_line, &names.field_offset, &names.field_text, &names.field_endLine, &names.field_endOffset };
        for (unsigned i = 0; i < std::min<unsigned>(details.size(), 6); ++i)
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

// UnicodeEncodeError(encoding, object, start, end, reason), and UnicodeDecodeError the same.
PYTHON_NATIVE(unicodeErrorInit)
{
    bool isDecode = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    ASCIILiteral name = isDecode ? "UnicodeDecodeError"_s : "UnicodeEncodeError"_s;
    if (args.size() != 6)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("function takes exactly 5 arguments ("_s, args.size() - 1, " given)"_s)));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, "() argument 1 must be str, not "_s, typeName(globalObject, args[1]))));
    if (isDecode ? !tryBufferOf(args[2]) : !args[2].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, isDecode ? makeString("a bytes-like object is required, not '"_s, typeName(globalObject, args[2]), '\'') : makeString(name, "() argument 2 must be str, not "_s, typeName(globalObject, args[2]))));
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    const Identifier* fields[] = { &names.field_encoding, &names.field_subject, &names.field_start, &names.field_end, &names.field_reason };
    for (unsigned i = 0; i < 5; ++i)
        self->putDirect(vm, *fields[i], args[i + 1]);
    RETURN_NONE();
}

PYTHON_NATIVE(unicodeErrorStr)
{
    bool isDecode = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args.at(0));
    auto get = [&] (const Identifier& field) { return self->getDirect(vm, field); };
    JSValue object = get(names.field_subject);
    if (!object)
        return JSValue::encode(jsEmptyString(vm));
    String encoding = str(globalObject, get(names.field_encoding));
    RETURN_IF_EXCEPTION(scope, { });
    String reason = str(globalObject, get(names.field_reason));
    RETURN_IF_EXCEPTION(scope, { });
    auto start = toIndex(globalObject, get(names.field_start), true);
    RETURN_IF_EXCEPTION(scope, { });
    auto end = toIndex(globalObject, get(names.field_end), true);
    RETURN_IF_EXCEPTION(scope, { });
    ASCIILiteral verb = isDecode ? "decode"_s : "encode"_s;
    if (*end == *start + 1 && *start >= 0) {
        if (isDecode) {
            auto buffer = tryBufferOf(object);
            if (buffer && static_cast<size_t>(*start) < buffer->size())
                return JSValue::encode(jsString(vm, makeString('\'', encoding, "' codec can't decode byte 0x"_s, hex((*buffer)[*start], 2, Lowercase), " in position "_s, *start, ": "_s, reason)));
        } else if (object.isString() && *start < static_cast<int64_t>(stringLength(globalObject, asString(object)))) {
            JSValue character = stringGetItem(globalObject, asString(object), jsNumber(static_cast<int32_t>(*start)));
            RETURN_IF_EXCEPTION(scope, { });
            char32_t c = *asString(character)->view(globalObject)->codePoints().begin();
            String escaped = c <= 0xFF ? makeString("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase)) : c <= 0xFFFF ? makeString("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase)) : makeString("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
            return JSValue::encode(jsString(vm, makeString('\'', encoding, "' codec can't encode character '"_s, escaped, "' in position "_s, *start, ": "_s, reason)));
        }
    }
    return JSValue::encode(jsString(vm, makeString('\'', encoding, "' codec can't "_s, verb, isDecode ? " bytes in position "_s : " characters in position "_s, *start, '-', *end - 1, ": "_s, reason)));
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
    addFields(realm->typeSyntaxError(), { &names.field_message, &names.field_filename, &names.field_line, &names.field_offset, &names.field_text, &names.field_endLine, &names.field_endOffset, &names.field_printFileAndLine });
    addMethods(globalObject, realm->typeUnicodeEncodeError(), {
        { "__init__"_s, unicodeErrorInit, Kind::Method, pack(false) },
        { "__str__"_s, unicodeErrorStr, Kind::Method, pack(false) },
    });
    addMethods(globalObject, realm->typeUnicodeDecodeError(), {
        { "__init__"_s, unicodeErrorInit, Kind::Method, pack(true) },
        { "__str__"_s, unicodeErrorStr, Kind::Method, pack(true) },
    });
    addMethods(globalObject, realm->typeImportError(), {
        { "__init__"_s, exceptionInitWithKeywords, Kind::Method, pack(KeywordException::Import) },
        { "__str__"_s, importErrorStr },
    });
    addFields(realm->typeImportError(), { &names.field_message, &names.field_name, &names.field_path, &names.field_nameFrom });
    addMethods(globalObject, realm->typeAttributeError(), { { "__init__"_s, exceptionInitWithKeywords, Kind::Method, pack(KeywordException::Attribute) } });
    addFields(realm->typeAttributeError(), { &names.field_name, &names.field_object });
    addMethods(globalObject, realm->typeNameError(), { { "__init__"_s, exceptionInitWithKeywords, Kind::Method, pack(KeywordException::Name) } });
    addFields(realm->typeNameError(), { &names.field_name });
    addFields(realm->typeUnicodeEncodeError(), { &names.field_encoding, &names.field_subject, &names.field_start, &names.field_end, &names.field_reason });
    addFields(realm->typeUnicodeDecodeError(), { &names.field_encoding, &names.field_subject, &names.field_start, &names.field_end, &names.field_reason });
}

// ---- print()

// file.write(str(value))
static void writeTo(JSGlobalObject* globalObject, JSValue file, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, void());
    if (!value.isString()) {
        String text = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
        value = jsString(vm, text);
    }
    scope.release();
    call(globalObject, write, value);
}

// print(*objects, sep=' ', end='\n', file=None, flush=False)
PYTHON_NATIVE(builtinPrint)
{
    NATIVE_PROLOGUE();
    JSValue separator;
    JSValue end;
    JSValue file;
    bool flush = false;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String name = args.keywordName(i)->value(globalObject);
        JSValue value = args.keywordValue(i);
        if (name == "sep"_s || name == "end"_s) {
            if (isNone(value))
                continue;
            if (!value.isString())
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, " must be None or a string, not "_s, typeName(globalObject, value))));
            (name == "sep"_s ? separator : end) = value;
        } else if (name == "file"_s)
            file = isNone(value) ? JSValue() : value;
        else if (name == "flush"_s) {
            flush = isTrue(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
        } else
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("print() got an unexpected keyword argument '"_s, name, '\'')));
    }

    if (!file) {
        file = sysAttribute(globalObject, "stdout"_s);
        if (!file)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "lost sys.stdout"_s));
        // There may be none, as in a program with no console.
        if (isNone(file))
            RETURN_NONE();
    }

    // Each thing is written by itself, which whatever the file is can tell.
    for (unsigned i = 0; i < args.size(); ++i) {
        if (i) {
            writeTo(globalObject, file, separator ? separator : JSValue(vm.smallStrings.singleCharacterString(' ')));
            RETURN_IF_EXCEPTION(scope, { });
        }
        writeTo(globalObject, file, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    writeTo(globalObject, file, end ? end : JSValue(vm.smallStrings.singleCharacterString('\n')));
    RETURN_IF_EXCEPTION(scope, { });

    if (flush) {
        JSValue flushMethod = getAttribute(globalObject, file, Identifier::fromString(vm, "flush"_s));
        RETURN_IF_EXCEPTION(scope, { });
        call(globalObject, flushMethod);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- What every object can be asked

PYTHON_NATIVE(builtinRepr_)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "repr"_s, 1, 1))
        return { };
    String text = repr(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(builtinAscii)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "ascii"_s, 1, 1))
        return { };
    String text = repr(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder builder;
    for (char32_t c : StringView(text).codePoints()) {
        if (c < 0x80)
            builder.append(static_cast<Latin1Character>(c));
        else if (c <= 0xFF)
            builder.append("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase));
        else if (c <= 0xFFFF)
            builder.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
        else
            builder.append("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
    }
    return JSValue::encode(jsString(vm, builder.toString()));
}

PYTHON_NATIVE(builtinLen)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "len"_s, 1, 1))
        return { };
    int64_t size = length(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, size)));
}

PYTHON_NATIVE(builtinHash_)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "hash"_s, 1, 1))
        return { };
    int64_t result = hash(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, result)));
}

PYTHON_NATIVE(builtinId)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "id"_s, 1, 1))
        return { };
    // What is not a cell has no address, so its bits will have to do.
    int64_t identity = args[0].isCell() ? static_cast<int64_t>(std::bit_cast<uintptr_t>(args[0].asCell())) : static_cast<int64_t>(JSValue::encode(args[0]) & 0x7FFFFFFFFFFFFFFFLL);
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, identity)));
}

PYTHON_NATIVE(builtinCallable)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "callable"_s, 1, 1))
        return { };
    return JSValue::encode(jsBoolean(isCallable(globalObject, args[0])));
}

PYTHON_NATIVE(builtinFormat_)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "format"_s, 1, 2))
        return { };
    String specification = emptyString();
    if (args.size() > 1) {
        if (!args[1].isString())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("format() argument 2 must be str, not "_s, isNone(args[1]) ? "None"_str : typeName(globalObject, args[1]))));
        specification = asString(args[1])->value(globalObject);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(format(globalObject, args[0], specification)));
}

// ---- Attributes

static std::optional<Identifier> attributeNameArgument(JSGlobalObject* globalObject, ThrowScope& scope, JSValue name)
{
    if (!name.isString()) {
        raiseTypeError(globalObject, scope, makeString("attribute name must be string, not '"_s, typeName(globalObject, name), '\''));
        return std::nullopt;
    }
    return asString(name)->toIdentifier(globalObject);
}

PYTHON_NATIVE(builtinGetAttr)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "getattr"_s, 2, 3))
        return { };
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 2)
        RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, args[0], *name)));
    JSValue value = getAttributeIfPresent(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value ? value : args[2]);
}

PYTHON_NATIVE(builtinHasAttr)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "hasattr"_s, 2, 2))
        return { };
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = getAttributeIfPresent(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(!!value));
}

PYTHON_NATIVE(builtinSetAttr)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "setattr"_s, 3, 3))
        return { };
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    setAttribute(globalObject, args[0], *name, args[2]);
    RETURN_NONE();
}

PYTHON_NATIVE(builtinDelAttr)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "delattr"_s, 2, 2))
        return { };
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    deleteAttribute(globalObject, args[0], *name);
    RETURN_NONE();
}

PYTHON_NATIVE(builtinIsInstance)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "isinstance"_s, 2, 2))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isInstanceOf(globalObject, args[0], args[1]))));
}

PYTHON_NATIVE(builtinIsSubclass)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "issubclass"_s, 2, 2))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isSubclassOf(globalObject, args[0], args[1]))));
}

PYTHON_NATIVE(builtinDir)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "dir"_s, 0, 1))
        return { };
    if (!args.size()) {
        JSValue locals = localsOfFrame(globalObject, callerOf(callFrame));
        MarkedArgumentBuffer keys;
        collect(globalObject, locals, keys);
        RETURN_IF_EXCEPTION(scope, { });
        MarkedArgumentBuffer sortedKeys;
        sortValues(globalObject, keys, JSValue(), false, sortedKeys);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, sortedKeys)));
    }
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_dir, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue found = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer values;
    collect(globalObject, found, values);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer sorted;
    sortValues(globalObject, values, JSValue(), false, sorted);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, sorted)));
}

PYTHON_NATIVE(builtinVars)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "vars"_s, 0, 1))
        return { };
    if (!args.size())
        return JSValue::encode(localsOfFrame(globalObject, callerOf(callFrame)));
    JSValue dict = getAttributeIfPresent(globalObject, args[0], names.dunder_dict);
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        return JSValue::encode(raiseTypeError(globalObject, scope, "vars() argument must have __dict__ attribute"_s));
    return JSValue::encode(dict);
}

// ---- Iteration

PYTHON_NATIVE(builtinIter)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "iter"_s, 1, 2))
        return { };
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(getIterator(globalObject, args[0])));
    if (!isCallable(globalObject, args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "iter(v, w): v must be callable"_s));
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::Callable, args[0], args[1]));
}

PYTHON_NATIVE(builtinNext)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "next"_s, 1, 2))
        return { };
    // What a generator returns is carried by the StopIteration.
    if (args[0].isCell() && args[0].asCell()->type() == JSGeneratorType && args.size() == 1 && generatorKindOf(globalObject, asGenerator(args[0])) == GeneratorKind::Generator)
        RELEASE_AND_RETURN(scope, JSValue::encode(generatorSend(globalObject, uncheckedDowncast<JSGenerator>(args[0].asCell()), jsUndefined())));
    JSValue value = iteratorNext(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    if (args.size() > 1)
        return JSValue::encode(args[1]);
    return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
}

PYTHON_NATIVE(builtinAnyOrAll)
{
    auto isAny = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, isAny ? "any"_s : "all"_s, 1, 1))
        return { };
    bool result = !isAny;
    forEach(globalObject, args[0], [&] (JSValue value) {
        if (isTrue(globalObject, value) != isAny)
            return true;
        result = isAny;
        return false;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result));
}

// sum(iterable, /, start=0)
PYTHON_NATIVE(builtinSum)
{
    NATIVE_PROLOGUE();
    if (!args.size() || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sum() takes at most 2 arguments ("_s, args.size(), " given)"_s)));
    JSValue total = args.at(1);
    if (!total)
        total = jsNumber(0);
    if (total.isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "sum() can't sum strings [use ''.join(seq) instead]"_s));
    forEach(globalObject, args[0], [&] (JSValue value) {
        total = binaryOperation(globalObject, BinaryOperator::Add, false, total, value);
        return !!total;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(total);
}

// min(iterable, *, key=None, default=...) and min(a, b, ..., key=None)
PYTHON_NATIVE(builtinMinOrMax)
{
    auto isMax = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    ASCIILiteral name = isMax ? "max"_s : "min"_s;
    if (!args.size())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, " expected at least 1 argument, got 0"_s)));
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    JSValue keyFunction = args.keyword(globalObject, "key"_s);
    if (keyFunction && isNone(keyFunction))
        keyFunction = { };
    JSValue defaultValue = args.keyword(globalObject, "default"_s);
    if (defaultValue && args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Cannot specify a default for "_s, name, "() with multiple positional arguments"_s)));

    JSValue best;
    JSValue bestKey;
    auto consider = [&] (JSValue value) -> bool {
        JSValue key = value;
        if (keyFunction) {
            key = call(globalObject, keyFunction, value);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (best) {
            JSValue isBetter = compare(globalObject, isMax ? ComparisonOperator::Gt : ComparisonOperator::Lt, key, bestKey);
            RETURN_IF_EXCEPTION(scope, false);
            bool replace = isTrue(globalObject, isBetter);
            RETURN_IF_EXCEPTION(scope, false);
            if (!replace)
                return true;
        }
        best = value;
        bestKey = key;
        return true;
    };
    if (args.size() == 1)
        forEach(globalObject, args[0], consider);
    else {
        for (unsigned i = 0; i < args.size() && consider(args[i]); ++i) { }
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (best)
        return JSValue::encode(best);
    if (defaultValue)
        return JSValue::encode(defaultValue);
    return JSValue::encode(raiseValueError(globalObject, scope, makeString(name, "() iterable argument is empty"_s)));
}

// sorted(iterable, /, *, key=None, reverse=False)
// sorted(iterable, /, *, key=None, reverse=False): a list of what is in it, sorted as list.sort() does it, which is given the rest.
PYTHON_NATIVE(builtinSorted)
{
    NATIVE_PROLOGUE();
    if (args.size() != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sorted expected 1 argument, got "_s, args.size())));
    MarkedArgumentBuffer values;
    collect(globalObject, args[0], values);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = newList(globalObject, values);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(list);
    for (unsigned i = 0; i < args.keywordCount(); ++i)
        arguments.append(args.keywordValue(i));
    callWithKeywords(globalObject, realm->typeList()->lookup(vm, Identifier::fromString(vm, "sort"_s)), arguments, args.keywordNames());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(list);
}

// ---- Numbers and characters

PYTHON_NATIVE(builtinAbs)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "abs"_s, 1, 1))
        return { };
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_abs, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("bad operand type for abs(): '"_s, typeName(globalObject, args[0]), '\'')));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

// round(number, ndigits=None)
PYTHON_NATIVE(builtinRound)
{
    NATIVE_PROLOGUE();
    JSValue number = args.at(0);
    JSValue digits = args.at(1);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, number, names.dunder_round, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type "_s, typeName(globalObject, number), " doesn't define __round__ method"_s)));
    if (!digits || isNone(digits))
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self, digits)));
}

PYTHON_NATIVE(builtinDivmod)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "divmod"_s, 2, 2))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(divmod(globalObject, args[0], args[1])));
}

// pow(base, exp, mod=None)
PYTHON_NATIVE(builtinPow)
{
    NATIVE_PROLOGUE();
    JSValue modulus = args.at(2);
    RELEASE_AND_RETURN(scope, JSValue::encode(power(globalObject, args.at(0), args.at(1), modulus ? modulus : jsUndefined())));
}

PYTHON_NATIVE(builtinChr)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "chr"_s, 1, 1))
        return { };
    auto code = toIndex(globalObject, args[0], true);
    RETURN_IF_EXCEPTION(scope, { });
    if (*code < 0 || *code > 0x10FFFF)
        return JSValue::encode(raiseValueError(globalObject, scope, "chr() arg not in range(0x110000)"_s));
    StringBuilder builder;
    builder.append(static_cast<char32_t>(*code));
    return JSValue::encode(jsString(vm, builder.toString()));
}

PYTHON_NATIVE(builtinOrd)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "ord"_s, 1, 1))
        return { };
    if (!args[0].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("ord() expected string of length 1, but "_s, typeName(globalObject, args[0]), " found"_s)));
    unsigned count = stringLength(globalObject, asString(args[0]));
    if (count != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("ord() expected a character, but string of length "_s, count, " found"_s)));
    auto view = asString(args[0])->view(globalObject);
    return JSValue::encode(jsNumber(static_cast<int32_t>(*view->codePoints().begin())));
}

// bin(), oct() and hex()
PYTHON_NATIVE(builtinInRadix)
{
    auto radix = unpack<unsigned>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, radix == 2 ? "bin"_s : radix == 8 ? "oct"_s : "hex"_s, 1, 1))
        return { };
    Number number = classify(args[0]);
    if (!number.isInt()) {
        JSValue self;
        JSValue method = number ? JSValue() : lookupSpecial(globalObject, args[0], names.dunder_index, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!method)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, args[0]), "' object cannot be interpreted as an integer"_s)));
        JSValue index = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, { });
        number = classify(index);
    }
    String digits = reprOfInt(globalObject, number, radix);
    RETURN_IF_EXCEPTION(scope, { });
    ASCIILiteral prefix = radix == 2 ? "0b"_s : radix == 8 ? "0o"_s : "0x"_s;
    if (digits.startsWith('-'))
        return JSValue::encode(jsString(vm, makeString('-', prefix, StringView(digits).substring(1))));
    return JSValue::encode(jsString(vm, makeString(prefix, digits)));
}

// ---- Classes and modules

// __build_class__(function, name, *bases, **keywords)
PYTHON_NATIVE(builtinBuildClass)
{
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "__build_class__: not enough arguments"_s));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "__build_class__: name is not a string"_s));
    PyTuple* bases = PyTuple::create(globalObject, args.size() - 2);
    for (unsigned i = 2; i < args.size(); ++i)
        bases->initializeAt(vm, i - 2, args[i]);
    PyDict* keywords = nullptr;
    if (args.keywordCount()) {
        keywords = PyDict::create(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i)
            keywords->set(globalObject, args.keywordName(i), args.keywordValue(i));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(buildClass(globalObject, args[0], asString(args[1]), bases, keywords)));
}

// __import__(name, globals=None, locals=None, fromlist=(), level=0)
PYTHON_NATIVE(builtinImport)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !args[0].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "__import__() argument 1 must be str"_s));
    JSValue fromList = args.at(3);
    JSValue levelValue = args.at(4);
    unsigned level = levelValue && levelValue.isInt32() ? levelValue.asInt32() : 0;
    JSObject* globals = globalsOfFrame(globalObject, callerOf(callFrame));
    RELEASE_AND_RETURN(scope, JSValue::encode(importModule(globalObject, globals, asString(args[0])->value(globalObject), fromList ? fromList : jsUndefined(), level, false)));
}

void initializeBuiltinFunctions(JSGlobalObject* globalObject, JSObject* namespaceObject)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, namespaceObject, name, function, data); };
    add("print"_s, builtinPrint);
    add("repr"_s, builtinRepr_);
    add("ascii"_s, builtinAscii);
    add("len"_s, builtinLen);
    add("hash"_s, builtinHash_);
    add("id"_s, builtinId);
    add("callable"_s, builtinCallable);
    add("format"_s, builtinFormat_);
    add("getattr"_s, builtinGetAttr);
    add("hasattr"_s, builtinHasAttr);
    add("setattr"_s, builtinSetAttr);
    add("delattr"_s, builtinDelAttr);
    add("isinstance"_s, builtinIsInstance);
    add("issubclass"_s, builtinIsSubclass);
    add("dir"_s, builtinDir);
    add("vars"_s, builtinVars);
    add("iter"_s, builtinIter);
    add("next"_s, builtinNext);
    add("any"_s, builtinAnyOrAll, pack(true));
    add("all"_s, builtinAnyOrAll, pack(false));
    add("sum"_s, builtinSum);
    // These have something of their own to say before their keywords are looked at.
    addFunction(globalObject, namespaceObject, "min"_s, builtinMinOrMax, pack(false), "(*args, key=None, default=None)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, namespaceObject, "max"_s, builtinMinOrMax, pack(true), "(*args, key=None, default=None)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, namespaceObject, "sorted"_s, builtinSorted, 0, { }, PyNativeFunction::Arguments::AreNotChecked);
    add("abs"_s, builtinAbs);
    add("round"_s, builtinRound);
    add("divmod"_s, builtinDivmod);
    add("pow"_s, builtinPow);
    add("chr"_s, builtinChr);
    add("ord"_s, builtinOrd);
    add("bin"_s, builtinInRadix, pack(2));
    add("oct"_s, builtinInRadix, pack(8));
    add("hex"_s, builtinInRadix, pack(16));
    add("__build_class__"_s, builtinBuildClass);
    add("__import__"_s, builtinImport);
}

} } // namespace JSC::Python
