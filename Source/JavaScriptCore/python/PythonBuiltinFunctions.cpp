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
#include "PythonGenerators.h"
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
    PyInstance* exception = PyInstance::create(vm, asType(args[0])->instanceStructure());
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
    if (!args.at(1) || !args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("note must be a str, not '"_s, typeName(globalObject, args.at(1)), '\'')));
    JSObject* self = asObject(args[0]);
    JSValue notes = self->getDirect(vm, names.dunder_notes);
    if (!notes) {
        notes = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        self->putDirect(vm, names.dunder_notes, notes);
    }
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

EXCEPTION_PROPERTY(getTraceback, setTraceback, private_traceback, jsUndefined())
EXCEPTION_PROPERTY(getContext, setContext, private_context, jsUndefined())
EXCEPTION_PROPERTY(getSuppressContext, setSuppressContext, private_suppressContext, jsBoolean(false))

static JSValue getCause(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = self.isObject() ? asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_cause) : JSValue();
    return value ? value : jsUndefined();
}

static void setCause(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    asObject(self)->putDirect(vm, vm.pythonNames().private_cause, value ? value : jsUndefined());
    asObject(self)->putDirect(vm, vm.pythonNames().private_suppressContext, jsBoolean(true));
}

static JSValue getArgs(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = self.isObject() ? asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_args) : JSValue();
    return value ? value : JSValue(globalObject->pyRealm()->emptyTuple());
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

// StopIteration.value and SystemExit.code: the first argument.
static JSValue getFirstArgument(JSGlobalObject* globalObject, JSValue self)
{
    auto* tuple = uncheckedDowncast<PyTuple>(getArgs(globalObject, self).asCell());
    return tuple->length() ? tuple->at(0) : jsUndefined();
}

// OSError(errno, strerror, filename)
PYTHON_NATIVE(osErrorInit)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    bool hasParts = args.size() >= 3;
    self->putDirect(vm, Identifier::fromString(vm, "errno"_s), hasParts ? args[1] : jsUndefined());
    self->putDirect(vm, Identifier::fromString(vm, "strerror"_s), hasParts ? args[2] : jsUndefined());
    self->putDirect(vm, Identifier::fromString(vm, "filename"_s), args.size() >= 4 ? args[3] : jsUndefined());
    if (args.size() >= 4)
        self->putDirect(vm, names.private_args, PyTuple::create(globalObject, { args[1], args[2] }));
    RETURN_NONE();
}

PYTHON_NATIVE(osErrorStr)
{
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args.at(0));
    JSValue errorNumber = self->getDirect(vm, Identifier::fromString(vm, "errno"_s));
    JSValue message = self->getDirect(vm, Identifier::fromString(vm, "strerror"_s));
    JSValue filename = self->getDirect(vm, Identifier::fromString(vm, "filename"_s));
    if (!errorNumber || isNone(errorNumber) || !message) {
        String text = strOfException(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsString(vm, text));
    }
    String numberText = str(globalObject, errorNumber);
    RETURN_IF_EXCEPTION(scope, { });
    String messageText = str(globalObject, message);
    RETURN_IF_EXCEPTION(scope, { });
    if (filename && !isNone(filename)) {
        String filenameText = repr(globalObject, filename);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsString(vm, makeString("[Errno "_s, numberText, "] "_s, messageText, ": "_s, filenameText)));
    }
    return JSValue::encode(jsString(vm, makeString("[Errno "_s, numberText, "] "_s, messageText)));
}

// Exceptions that take some of their attributes as keywords: ImportError(msg, name=, path=), AttributeError(msg, name=, obj=).
PYTHON_NATIVE(exceptionInitWithKeywords)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSObject* self = asObject(args[0]);
    self->putDirect(vm, names.private_args, argumentsAfterFirst(globalObject, args));
    for (unsigned i = 0; i < args.keywordCount(); ++i)
        self->putDirect(vm, args.keywordName(i)->toIdentifier(globalObject), args.keywordValue(i));
    RETURN_NONE();
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
        self->putDirect(vm, Identifier::fromString(vm, "msg"_s), args[1]);
    if (args.size() == 3) {
        MarkedArgumentBuffer details;
        collect(globalObject, args[2], details);
        RETURN_IF_EXCEPTION(scope, { });
        if (details.size() < 4 || details.size() > 7)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("function takes "_s, details.size() < 4 ? "at least 4"_s : "at most 7"_s, " arguments ("_s, details.size(), " given)"_s)));
        if (details.size() == 5)
            return JSValue::encode(raiseTypeError(globalObject, scope, "end_offset must be provided when end_lineno is provided"_s));
        static constexpr ASCIILiteral attributes[] = { "filename"_s, "lineno"_s, "offset"_s, "text"_s, "end_lineno"_s, "end_offset"_s };
        for (unsigned i = 0; i < std::min<unsigned>(details.size(), 6); ++i)
            self->putDirect(vm, Identifier::fromString(vm, attributes[i]), details.at(i));
    }
    RETURN_NONE();
}

PYTHON_NATIVE(syntaxErrorStr)
{
    NATIVE_PROLOGUE();
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
    static constexpr ASCIILiteral attributes[] = { "encoding"_s, "object"_s, "start"_s, "end"_s, "reason"_s };
    for (unsigned i = 0; i < 5; ++i)
        self->putDirect(vm, Identifier::fromString(vm, attributes[i]), args[i + 1]);
    RETURN_NONE();
}

PYTHON_NATIVE(unicodeErrorStr)
{
    bool isDecode = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args.at(0));
    auto get = [&] (ASCIILiteral name) { return self->getDirect(vm, Identifier::fromString(vm, name)); };
    JSValue object = get("object"_s);
    if (!object)
        return JSValue::encode(jsEmptyString(vm));
    String encoding = str(globalObject, get("encoding"_s));
    RETURN_IF_EXCEPTION(scope, { });
    String reason = str(globalObject, get("reason"_s));
    RETURN_IF_EXCEPTION(scope, { });
    auto start = toIndex(globalObject, get("start"_s), true);
    RETURN_IF_EXCEPTION(scope, { });
    auto end = toIndex(globalObject, get("end"_s), true);
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
        { "__new__"_s, exceptionNew, Kind::Function },
        { "__init__"_s, exceptionInit },
        { "__str__"_s, exceptionStr },
        { "__repr__"_s, nativeRepr },
        { "with_traceback"_s, exceptionWithTraceback },
        { "add_note"_s, exceptionAddNote },
    });
    addGetSet(globalObject, base, "args"_s, getArgs, setArgs);
    addGetSet(globalObject, base, "__traceback__"_s, getTraceback, setTraceback);
    addGetSet(globalObject, base, "__cause__"_s, getCause, setCause);
    addGetSet(globalObject, base, "__context__"_s, getContext, setContext);
    addGetSet(globalObject, base, "__suppress_context__"_s, getSuppressContext, setSuppressContext);

    addGetSet(globalObject, realm->typeStopIteration(), "value"_s, getFirstArgument);
    addGetSet(globalObject, realm->typeSystemExit(), "code"_s, getFirstArgument);
    addMethods(globalObject, realm->typeOSError(), {
        { "__init__"_s, osErrorInit },
        { "__str__"_s, osErrorStr },
    });
    addMethods(globalObject, realm->typeSyntaxError(), {
        { "__init__"_s, syntaxErrorInit },
        { "__str__"_s, syntaxErrorStr },
    });
    for (ASCIILiteral name : { "msg"_s, "filename"_s, "lineno"_s, "offset"_s, "text"_s, "end_lineno"_s, "end_offset"_s, "print_file_and_line"_s })
        realm->typeSyntaxError()->putDirect(vm, Identifier::fromString(vm, name), jsUndefined());
    addMethods(globalObject, realm->typeUnicodeEncodeError(), {
        { "__init__"_s, unicodeErrorInit, Kind::Method, pack(false) },
        { "__str__"_s, unicodeErrorStr, Kind::Method, pack(false) },
    });
    addMethods(globalObject, realm->typeUnicodeDecodeError(), {
        { "__init__"_s, unicodeErrorInit, Kind::Method, pack(true) },
        { "__str__"_s, unicodeErrorStr, Kind::Method, pack(true) },
    });
    for (PyType* type : { realm->typeImportError(), realm->typeAttributeError(), realm->typeNameError() }) {
        addMethods(globalObject, type, { { "__init__"_s, exceptionInitWithKeywords } });
        type->putDirect(vm, Identifier::fromString(vm, "name"_s), jsUndefined());
    }
    realm->typeImportError()->putDirect(vm, Identifier::fromString(vm, "path"_s), jsUndefined());
    realm->typeAttributeError()->putDirect(vm, Identifier::fromString(vm, "obj"_s), jsUndefined());
}

// ---- print()

static Vector<char, 4096>& outputBuffer()
{
    static NeverDestroyed<Vector<char, 4096>> buffer;
    return buffer;
}

void flushStandardOutput(JSGlobalObject*)
{
    auto& buffer = outputBuffer();
    if (!buffer.isEmpty())
        fwrite(buffer.span().data(), 1, buffer.size(), stdout);
    buffer.shrink(0);
    fflush(stdout);
}

void writeToStandardOutput(StringView text)
{
    auto& buffer = outputBuffer();
    auto utf8 = text.utf8(); // FIXME: A lone surrogate is an error.
    buffer.append(utf8.span());
    if (buffer.size() >= 4096) {
        fwrite(buffer.span().data(), 1, buffer.size(), stdout);
        buffer.shrink(0);
    }
}

// print(*objects, sep=' ', end='\n', file=None, flush=False)
PYTHON_NATIVE(builtinPrint)
{
    NATIVE_PROLOGUE();
    String separator = " "_s;
    String end = "\n"_s;
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
            (name == "sep"_s ? separator : end) = asString(value)->value(globalObject);
        } else if (name == "file"_s)
            file = isNone(value) ? JSValue() : value;
        else if (name == "flush"_s) {
            flush = isTrue(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
        } else
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("print() got an unexpected keyword argument '"_s, name, '\'')));
    }

    StringBuilder builder;
    for (unsigned i = 0; i < args.size(); ++i) {
        if (i)
            builder.append(separator);
        String text = str(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(text);
    }
    builder.append(end);

    if (file) {
        JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
        RETURN_IF_EXCEPTION(scope, { });
        call(globalObject, write, jsString(vm, builder.toString()));
        RETURN_IF_EXCEPTION(scope, { });
        if (flush) {
            JSValue flushMethod = getAttribute(globalObject, file, Identifier::fromString(vm, "flush"_s));
            RETURN_IF_EXCEPTION(scope, { });
            call(globalObject, flushMethod);
        }
        RETURN_NONE();
    }
    writeToStandardOutput(builder.toString());
    if (flush)
        flushStandardOutput(globalObject);
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
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("format() argument 2 must be str, not "_s, typeName(globalObject, args[1]))));
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
    if (args[0].isCell() && args[0].asCell()->type() == JSGeneratorType && args.size() == 1)
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
        total = args.keyword(globalObject, "start"_s);
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
PYTHON_NATIVE(builtinSorted)
{
    NATIVE_PROLOGUE();
    if (args.size() != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sorted expected 1 argument, got "_s, args.size())));
    JSValue reverseValue = args.keyword(globalObject, "reverse"_s);
    bool reverse = reverseValue && isTrue(globalObject, reverseValue);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer values;
    collect(globalObject, args[0], values);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer sorted;
    sortValues(globalObject, values, args.keyword(globalObject, "key"_s), reverse, sorted);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, sorted)));
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
    if (!args.size() || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "round() missing required argument 'number' (pos 1)"_s));
    JSValue digits = args.at(1);
    if (!digits)
        digits = args.keyword(globalObject, "ndigits"_s);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_round, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type "_s, typeName(globalObject, args[0]), " doesn't define __round__ method"_s)));
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
    if (args.size() < 2 || args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, "pow() missing required argument 'exp' (pos 2)"_s));
    JSValue modulus = args.at(2);
    if (!modulus)
        modulus = args.keyword(globalObject, "mod"_s);
    RELEASE_AND_RETURN(scope, JSValue::encode(power(globalObject, args[0], args[1], modulus ? modulus : jsUndefined())));
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
    if (!fromList)
        fromList = args.keyword(globalObject, "fromlist"_s);
    JSValue levelValue = args.at(4);
    if (!levelValue)
        levelValue = args.keyword(globalObject, "level"_s);
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
    add("min"_s, builtinMinOrMax, pack(false));
    add("max"_s, builtinMinOrMax, pack(true));
    add("sorted"_s, builtinSorted);
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
