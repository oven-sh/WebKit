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
#include "PythonIO.h"

#include "TopExceptionScope.h"
#include <errno.h>

// _io: Modules/_io/_iomodule.c and iobase.c of CPython, and the abstract classes that begin its bufferedio.c and textio.c.
//
// FIXME: What is not closed is closed, and what has been written to it and not sent on is sent on, when it is found that nothing refers to it any longer. That waits on when objects are finalized. See README.md.

namespace JSC { namespace Python {

// ---- What the parts share

const FileOperations* fileOperations(JSGlobalObject* globalObject)
{
    return globalObject->pyRealm()->configuration().files;
}

JSValue raiseUnsupportedOperation(JSGlobalObject* globalObject, ThrowScope& scope, const String& message)
{
    JSObject* exception = createException(globalObject, ioState(globalObject).unsupportedOperation.get(), message);
    RETURN_IF_EXCEPTION(scope, { });
    raiseObject(globalObject, scope, exception);
    return { };
}

JSValue callMethodNamed(JSGlobalObject* globalObject, JSValue object, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue self;
    JSValue method = loadMethod(globalObject, object, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, self ? callMethod(globalObject, method, self) : call(globalObject, method));
}

JSValue callMethodNamed(JSGlobalObject* globalObject, JSValue object, const Identifier& name, JSValue argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue self;
    JSValue method = loadMethod(globalObject, object, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, self ? callMethod(globalObject, method, self, argument) : call(globalObject, method, argument));
}

JSValue callMethodNamed(JSGlobalObject* globalObject, JSValue object, const Identifier& name, JSValue first, JSValue second)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue self;
    JSValue method = loadMethod(globalObject, object, name, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, self ? callMethod(globalObject, method, self, first, second) : call(globalObject, method, first, second));
}

Exception* takeRaisedException(VM& vm)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* raised = scope.exception();
    if (!raised || vm.isTerminationException(raised))
        return nullptr;
    scope.clearException();
    return raised;
}

void restoreRaisedException(JSGlobalObject* globalObject, Exception* taken)
{
    if (!taken)
        return;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // If the thread is to do no more, that is not to be got out of.
    RETURN_IF_EXCEPTION(scope, void());
    throwException(globalObject, scope, taken);
}

void chainRaisedExceptions(JSGlobalObject* globalObject, Exception* taken)
{
    if (!taken)
        return;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* raised = scope.exception();
    if (!raised) {
        throwException(globalObject, scope, taken);
        return;
    }
    if (vm.isTerminationException(raised))
        return;
    // PyException_SetContext()
    if (raised->value().isObject() && raised->value() != taken->value())
        asObject(raised->value())->putDirect(vm, vm.pythonNames().private_context, taken->value());
}

bool trapInterruptedError(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* raised = scope.exception();
    if (!raised || vm.isTerminationException(raised) || !isInstance(globalObject, raised->value(), globalObject->pyRealm()->typeOSError()))
        return false;
    JSValue number = asObject(raised->value())->getDirect(vm, vm.pythonNames().field_errorNumber);
    if (!number || !number.isInt32() || number.asInt32() != EINTR)
        return false;
    scope.clearException();
    return true;
}

bool checkSignals(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (vm.hasPythonWork()) [[unlikely]]
        doPendingWork(globalObject);
    return !scope.exception();
}

std::optional<int64_t> toOffset(JSGlobalObject* globalObject, JSValue item, std::optional<BuiltinType> overflow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = toInt(globalObject, item);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (auto result = tryInt64(value))
        return result;
    if (!overflow)
        return compareInts(value, jsNumber(0)) < 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    raise(globalObject, scope, *overflow, concatenate("cannot fit '"_s, typeName(globalObject, item), "' into an offset-sized integer"_s));
    return std::nullopt;
}

Buffer writableBufferArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Buffer buffer = tryBufferOf(globalObject, value, WritableBuffer);
    if (scope.exception() && !scope.tryClearException())
        return { };
    if (!buffer)
        raiseTypeError(globalObject, scope, concatenate(function, "() "_s, argument, " must be read-write bytes-like object, not "_s, typeNameOfArgument(globalObject, value)));
    return buffer;
}

bool toBoolMember(JSGlobalObject* globalObject, JSValue value, bool& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value || !value.isBoolean()) {
        raiseTypeError(globalObject, scope, value ? "attribute value type must be bool"_s : "can't delete numeric/char attribute"_s);
        return false;
    }
    result = value.asBoolean();
    return true;
}

// ---- _IOBase

// iobase_is_closed(): what it has of its own to say so, and not the `closed` that a class derived from this may have. Nothing if it raised.
static std::optional<bool> isIOBaseClosed(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue closed = getAttributeIfPresent(globalObject, self, globalObject->vm().pythonNames().attribute_ioBaseClosed);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return !!closed;
}

bool checkIsNotClosed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // This is the one that a class derived from this may have.
    JSValue closed = getAttributeIfPresent(globalObject, self, vm.pythonNames().attribute_closed);
    RETURN_IF_EXCEPTION(scope, false);
    if (!closed)
        return true;
    bool isClosed = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, false);
    if (isClosed)
        raiseValueError(globalObject, scope, "I/O operation on closed file."_s);
    return !isClosed;
}

static bool checkSaysTrue(JSGlobalObject* globalObject, JSValue self, const Identifier& method, ASCIILiteral complaint)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = callMethodNamed(globalObject, self, method);
    RETURN_IF_EXCEPTION(scope, false);
    if (result.isTrue())
        return true;
    raiseUnsupportedOperation(globalObject, scope, complaint);
    return false;
}

bool checkIsReadable(JSGlobalObject* globalObject, JSValue self) { return checkSaysTrue(globalObject, self, globalObject->vm().pythonNames().attribute_readable, "File or stream is not readable."_s); }
bool checkIsWritable(JSGlobalObject* globalObject, JSValue self) { return checkSaysTrue(globalObject, self, globalObject->vm().pythonNames().attribute_writable, "File or stream is not writable."_s); }
bool checkIsSeekable(JSGlobalObject* globalObject, JSValue self) { return checkSaysTrue(globalObject, self, globalObject->vm().pythonNames().attribute_seekable, "File or stream is not seekable."_s); }

PYTHON_SHARED_NATIVE(ioCannotPickle)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cannot pickle '"_s, typeOf(globalObject, args[0])->nameWithoutModule(globalObject), "' instances"_s)));
}

enum class Unsupported : uint8_t { Seek, Truncate, Fileno, Detach, Read, Read1, ReadLine, Write };

// What is not to be had of it. The arguments are looked at all the same, as far as CPython takes them for something.
PYTHON_NATIVE(ioUnsupported)
{
    NATIVE_PROLOGUE();
    auto which = unpack<Unsupported>(callFrame, 0);
    bool isText = unpack<bool>(callFrame, 1);
    switch (which) {
    case Unsupported::Seek:
    case Unsupported::Read:
    case Unsupported::Read1:
    case Unsupported::ReadLine:
        for (unsigned i = 1; i < args.size(); ++i) {
            toCInt(globalObject, args[i]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        break;
    case Unsupported::Write:
        if (isText) {
            toTextArgument(globalObject, args[1], "write"_s, "argument 1"_s);
            RETURN_IF_EXCEPTION(scope, { });
        }
        break;
    case Unsupported::Truncate:
    case Unsupported::Fileno:
    case Unsupported::Detach:
        break;
    }
    static constexpr ASCIILiteral messages[] = { "seek"_s, "truncate"_s, "fileno"_s, "detach"_s, "read"_s, "read1"_s, "readline"_s, "write"_s };
    return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, messages[static_cast<unsigned>(which)]));
}

PYTHON_NATIVE(ioBaseTell)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_seek, jsNumber(0), jsNumber(1))));
}

PYTHON_NATIVE(ioBaseFlush)
{
    NATIVE_PROLOGUE();
    auto isClosed = isIOBaseClosed(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*isClosed)
        return JSValue::encode(raiseValueError(globalObject, scope, "I/O operation on closed file."_s));
    RETURN_NONE();
}

static JSValue getIOBaseClosed(JSGlobalObject* globalObject, JSValue self)
{
    auto isClosed = isIOBaseClosed(globalObject, self);
    return isClosed ? jsBoolean(*isClosed) : JSValue();
}

enum class Check : uint8_t { Closed, Seekable, Readable, Writable };

PYTHON_NATIVE(ioBaseCheck)
{
    NATIVE_PROLOGUE();
    scope.release();
    switch (unpack<Check>(callFrame, 0)) {
    case Check::Closed:
        checkIsNotClosed(globalObject, args[0]);
        RETURN_NONE();
    case Check::Seekable:
        return JSValue::encode(checkIsSeekable(globalObject, args[0]) ? jsBoolean(true) : JSValue());
    case Check::Readable:
        return JSValue::encode(checkIsReadable(globalObject, args[0]) ? jsBoolean(true) : JSValue());
    case Check::Writable:
        return JSValue::encode(checkIsWritable(globalObject, args[0]) ? jsBoolean(true) : JSValue());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(ioBaseClose)
{
    NATIVE_PROLOGUE();
    auto isClosed = isIOBaseClosed(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*isClosed)
        RETURN_NONE();
    // _PyFile_Flush()
    callMethodNamed(globalObject, args[0], names.attribute_flush);
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, { });
    setAttribute(globalObject, args[0], names.attribute_ioBaseClosed, jsBoolean(true));
    chainRaisedExceptions(globalObject, raised);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// iobase_finalize()
PYTHON_NATIVE(ioBaseDel)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    // If it cannot say whether it is closed it is in no state to be closed.
    JSValue closed = getAttributeIfPresent(globalObject, self, names.attribute_closed);
    bool isOpen = false;
    if (!scope.exception() && closed)
        isOpen = !isTrue(globalObject, closed);
    if (scope.exception()) {
        if (!scope.tryClearException())
            return { };
        isOpen = false;
    }
    if (!isOpen)
        RETURN_NONE();
    // So that close() knows what it is being called for.
    setAttribute(globalObject, self, names.attribute__finalizing, jsBoolean(true));
    if (scope.exception() && !scope.tryClearException())
        return { };
    callMethodNamed(globalObject, self, names.attribute_close);
    if (scope.exception()) {
        reportUnraisableShowing(globalObject, "Exception ignored while finalizing file"_s, self);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(ioBaseFalse)
{
    return JSValue::encode(jsBoolean(false));
}

PYTHON_NATIVE(ioBaseIsATTY)
{
    NATIVE_PROLOGUE();
    scope.release();
    if (!checkIsNotClosed(globalObject, args[0]))
        return { };
    return JSValue::encode(jsBoolean(false));
}

// __enter__ and __iter__
PYTHON_NATIVE(ioBaseSelfIfOpen)
{
    NATIVE_PROLOGUE();
    scope.release();
    if (!checkIsNotClosed(globalObject, args[0]))
        return { };
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(ioBaseExit)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "_IOBase.__exit__"_s))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_close)));
}

PYTHON_NATIVE(ioBaseReadLine)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto limit = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue peek = getAttributeIfPresent(globalObject, self, names.attribute_peek);
    RETURN_IF_EXCEPTION(scope, { });
    ByteVector buffer;
    while (*limit < 0 || static_cast<int64_t>(buffer.size()) < *limit) {
        int64_t readAhead = 1;
        if (peek) {
            JSValue ahead = call(globalObject, peek, jsNumber(1));
            if (scope.exception()) [[unlikely]] {
                if (trapInterruptedError(globalObject))
                    continue;
                return { };
            }
            if (!isInstance(globalObject, ahead, realm->typeBytes()))
                return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, concatenate("peek() should have returned a bytes object, not '"_s, typeName(globalObject, ahead), '\'')));
            auto bytes = *builtinBufferOf(ahead);
            if (!bytes.empty()) {
                size_t count = 0;
                while (count < bytes.size() && (*limit < 0 || static_cast<int64_t>(count) < *limit)) {
                    if (bytes[count++] == '\n')
                        break;
                }
                readAhead = static_cast<int64_t>(count);
            }
        }
        JSValue read = callMethodNamed(globalObject, self, names.attribute_read, intFromInt64(globalObject, readAhead));
        if (scope.exception()) [[unlikely]] {
            if (trapInterruptedError(globalObject))
                continue;
            return { };
        }
        if (!isInstance(globalObject, read, realm->typeBytes()))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, concatenate("read() should have returned a bytes object, not '"_s, typeName(globalObject, read), '\'')));
        auto bytes = *builtinBufferOf(read);
        if (bytes.empty())
            break;
        buffer.append(bytes);
        if (buffer.hasOverflowed())
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        if (buffer.last() == '\n')
            break;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, buffer)));
}

PYTHON_NATIVE(ioBaseNext)
{
    NATIVE_PROLOGUE();
    JSValue line = callMethodNamed(globalObject, args[0], names.attribute_readline);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t size = length(globalObject, line);
    RETURN_IF_EXCEPTION(scope, { });
    if (size <= 0)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(line);
}

PYTHON_NATIVE(ioBaseReadLines)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto hint = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (*hint <= 0) {
        listExtend(globalObject, result, self);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(result);
    }
    JSValue iterator = getIterator(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t total = 0;
    while (true) {
        JSValue line = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!line)
            break;
        listAppend(globalObject, result, line);
        RETURN_IF_EXCEPTION(scope, { });
        int64_t size = length(globalObject, line);
        RETURN_IF_EXCEPTION(scope, { });
        if (size > *hint - total)
            break;
        total += size;
    }
    return JSValue::encode(result);
}

PYTHON_NATIVE(ioBaseWriteLines)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    if (!checkIsNotClosed(globalObject, self))
        return { };
    JSValue iterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        JSValue line = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!line)
            break;
        do {
            callMethodNamed(globalObject, self, names.attribute_write, line);
        } while (scope.exception() && trapInterruptedError(globalObject));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- _RawIOBase

PYTHON_NATIVE(rawIOBaseRead)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    int64_t count = -1;
    if (JSValue value = args.at(1)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        count = *given;
    }
    if (count < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, self, names.attribute_readall)));
    ByteVector zeros;
    zeros.appendFill(0, static_cast<size_t>(count));
    JSValue array = newByteArray(globalObject, zeros);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = callMethodNamed(globalObject, self, names.attribute_readinto, array);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(result))
        RETURN_NONE();
    // PyNumber_AsSsize_t(result, PyExc_ValueError)
    JSValue integer = toInt(globalObject, result);
    RETURN_IF_EXCEPTION(scope, { });
    auto filled = tryInt64(integer);
    if (!filled)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("cannot fit '"_s, typeName(globalObject, result), "' into an index-sized integer"_s)));
    if (*filled < 0 || *filled > count)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("readinto returned "_s, *filled, " outside buffer size "_s, count)));
    auto bytes = *builtinBufferOf(array);
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, bytes.first(std::min<size_t>(bytes.size(), static_cast<size_t>(*filled))))));
}

PYTHON_NATIVE(rawIOBaseReadAll)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    ByteVector all;
    bool hasRead = false;
    while (true) {
        JSValue data = callMethodNamed(globalObject, self, names.attribute_read, jsNumber(static_cast<int32_t>(defaultBufferSize)));
        if (scope.exception()) [[unlikely]] {
            if (trapInterruptedError(globalObject))
                continue;
            return { };
        }
        if (isNone(data)) {
            if (!hasRead)
                RETURN_NONE();
            break;
        }
        if (!isInstance(globalObject, data, realm->typeBytes()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "read() should return bytes"_s));
        auto bytes = *builtinBufferOf(data);
        if (bytes.empty())
            break;
        hasRead = true;
        all.append(bytes);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, all)));
}

PYTHON_NATIVE(ioNotImplemented)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::NotImplementedError, JSValue()));
}

// ---- _BufferedIOBase

// _bufferediobase_readinto_generic()
PYTHON_NATIVE(bufferedIOBaseReadInto)
{
    NATIVE_PROLOGUE();
    bool isReadInto1 = unpack<bool>(callFrame, 0);
    Buffer buffer = writableBufferArgument(globalObject, args[1], isReadInto1 ? "readinto1"_s : "readinto"_s);
    RETURN_IF_EXCEPTION(scope, { });
    size_t wanted = buffer.size();
    JSValue data = callMethodNamed(globalObject, args[0], isReadInto1 ? names.attribute_read1 : names.attribute_read, intFromInt64(globalObject, static_cast<int64_t>(wanted)));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance(globalObject, data, realm->typeBytes()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "read() should return bytes"_s));
    auto bytes = *builtinBufferOf(data);
    if (bytes.size() > wanted)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("read() returned too much data: "_s, wanted, " bytes requested, "_s, bytes.size(), " returned"_s)));
    auto destination = mutableSpanOf(buffer);
    memcpySpan(destination.first(std::min(destination.size(), bytes.size())), bytes.first(std::min(destination.size(), bytes.size())));
    return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(bytes.size())));
}

// ---- The module

// text_encoding(encoding, stacklevel=2, /)
PYTHON_NATIVE(ioTextEncoding)
{
    NATIVE_PROLOGUE();
    JSValue encoding = args.at(0);
    int stackLevel = 2;
    if (JSValue level = args.at(1)) {
        auto given = toCInt(globalObject, level);
        RETURN_IF_EXCEPTION(scope, { });
        stackLevel = *given;
    }
    if (!isNone(encoding))
        return JSValue::encode(encoding);
    if (realm->configuration().warnsOfDefaultEncoding) {
        warn(globalObject, BuiltinType::EncodingWarning, "'encoding' argument not specified"_s, stackLevel);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(realm->configuration().usesUTF8Mode ? jsNontrivialString(vm, "utf-8"_s) : jsNontrivialString(vm, "locale"_s));
}

static void initializeBaseClasses(JSGlobalObject* globalObject, IOModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    constexpr auto withDefiningClass = PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass;
    auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, PyType* base) {
        PyType* type = createBuiltinType(globalObject, name, base, PyType::Layout::Object, PyType::IsBaseType);
        slot.set(vm, realm, type);
        return type;
    };

    PyType* ioBase = make(state.ioBase, "_io._IOBase"_s, realm->typeObject());
    addMethods(globalObject, ioBase, {
        { "seek"_s, ioUnsupported, Kind::Method, pack(Unsupported::Seek), { }, withDefiningClass },
        { "tell"_s, ioBaseTell },
        { "truncate"_s, ioUnsupported, Kind::Method, pack(Unsupported::Truncate), { }, withDefiningClass },
        { "flush"_s, ioBaseFlush },
        { "close"_s, ioBaseClose },
        { "seekable"_s, ioBaseFalse },
        { "readable"_s, ioBaseFalse },
        { "writable"_s, ioBaseFalse },
        { "_checkClosed"_s, ioBaseCheck, Kind::Method, pack(Check::Closed) },
        { "_checkSeekable"_s, ioBaseCheck, Kind::Method, pack(Check::Seekable) },
        { "_checkReadable"_s, ioBaseCheck, Kind::Method, pack(Check::Readable) },
        { "_checkWritable"_s, ioBaseCheck, Kind::Method, pack(Check::Writable) },
        { "fileno"_s, ioUnsupported, Kind::Method, pack(Unsupported::Fileno), { }, withDefiningClass },
        { "isatty"_s, ioBaseIsATTY },
        { "__enter__"_s, ioBaseSelfIfOpen },
        { "__exit__"_s, ioBaseExit, Kind::Method, 0, "($self, /, *args)"_s, notChecked },
        { "readline"_s, ioBaseReadLine },
        { "readlines"_s, ioBaseReadLines },
        { "writelines"_s, ioBaseWriteLines },
        { "__iter__"_s, ioBaseSelfIfOpen },
        { "__next__"_s, ioBaseNext },
        { "__del__"_s, ioBaseDel },
    });
    addGetSet(globalObject, ioBase, "__dict__"_s, getInstanceDict);
    addGetSet(globalObject, ioBase, "closed"_s, getIOBaseClosed);

    PyType* textIOBase = make(state.textIOBase, "_io._TextIOBase"_s, ioBase);
    addMethods(globalObject, textIOBase, {
        { "detach"_s, ioUnsupported, Kind::Method, pack(Unsupported::Detach, true), { }, withDefiningClass },
        { "read"_s, ioUnsupported, Kind::Method, pack(Unsupported::Read, true), { }, withDefiningClass },
        { "readline"_s, ioUnsupported, Kind::Method, pack(Unsupported::ReadLine, true), { }, withDefiningClass },
        { "write"_s, ioUnsupported, Kind::Method, pack(Unsupported::Write, true), { }, withDefiningClass },
    });
    for (auto name : { "encoding"_s, "newlines"_s, "errors"_s })
        addGetSet(globalObject, textIOBase, name, [] (JSGlobalObject*, JSValue) -> JSValue { return jsUndefined(); });

    PyType* bufferedIOBase = make(state.bufferedIOBase, "_io._BufferedIOBase"_s, ioBase);
    addMethods(globalObject, bufferedIOBase, {
        { "detach"_s, ioUnsupported, Kind::Method, pack(Unsupported::Detach), { }, withDefiningClass },
        { "read"_s, ioUnsupported, Kind::Method, pack(Unsupported::Read), { }, withDefiningClass },
        { "read1"_s, ioUnsupported, Kind::Method, pack(Unsupported::Read1), { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
        { "readinto"_s, bufferedIOBaseReadInto, Kind::Method, pack(false) },
        { "readinto1"_s, bufferedIOBaseReadInto, Kind::Method, pack(true) },
        { "write"_s, ioUnsupported, Kind::Method, pack(Unsupported::Write), { }, withDefiningClass },
    });

    PyType* rawIOBase = make(state.rawIOBase, "_io._RawIOBase"_s, ioBase);
    addMethods(globalObject, rawIOBase, {
        { "read"_s, rawIOBaseRead },
        { "readall"_s, rawIOBaseReadAll },
        { "readinto"_s, ioNotImplemented, Kind::Method, 0, "($self, /, *args)"_s, notChecked },
        { "write"_s, ioNotImplemented, Kind::Method, 0, "($self, /, *args)"_s, notChecked },
    });
}

void OwedOutput::noteSlow(JSGlobalObject* globalObject, bool owes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // This is where a stream has got to whether or not what it was doing came off.
    Exception* raised = scope.exception() ? takeRaisedException(vm) : nullptr;
    RETURN_IF_EXCEPTION(scope, void());
    auto& io = ioState(globalObject);
    if (owes) {
        if (!io.streamsThatOweOutput)
            io.streamsThatOweOutput.set(vm, globalObject->pyRealm(), newList(globalObject));
        JSArray* list = io.streamsThatOweOutput.get();
        unsigned place = list->length();
        listAppend(globalObject, list, m_stream);
        if (!scope.exception())
            m_place = place;
    } else {
        // The last takes its place.
        JSArray* list = io.streamsThatOweOutput.get();
        unsigned last = list->length() - 1;
        if (m_place != last) {
            JSValue moved = listGet(globalObject, list, last);
            listSet(globalObject, list, m_place, moved);
            uncheckedDowncast<PyStateObject>(moved.asCell())->owedOutput()->m_place = m_place;
        }
        listRemoveRange(globalObject, list, last, 1);
        m_place = nowhere;
    }
    if (raised && !scope.exception())
        restoreRaisedException(globalObject, raised);
}

void writeOffOwedOutput(JSGlobalObject* globalObject, JSValue stream)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    while (stream && stream.isCell() && stream.asCell()->inherits<PyStateObject>()) {
        OwedOutput* owed = uncheckedDowncast<PyStateObject>(stream.asCell())->owedOutput();
        if (!owed)
            return;
        owed->note(globalObject, false);
        stream = getAttributeIfPresent(globalObject, stream, Identifier::fromString(vm, "buffer"_s));
        if (scope.exception()) {
            scope.clearExceptionExceptTermination();
            return;
        }
    }
}

void passOnOwedOutput(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto& io = ioState(globalObject);
    // Passing on what one owes can leave another owing it.
    while (io.streamsThatOweOutput && io.streamsThatOweOutput->length()) {
        JSArray* list = io.streamsThatOweOutput.get();
        JSCell* stream = listGet(globalObject, list, list->length() - 1).asCell();
        // iobase_finalize(), but that close() is not told that nothing had closed it, which is for it to warn of. That would be said of these and of no others.
        JSValue closed = getAttributeIfPresent(globalObject, stream, Identifier::fromString(vm, "closed"_s));
        bool isOpen = !scope.exception() && closed && !isTrue(globalObject, closed);
        if (scope.exception()) {
            if (!scope.clearExceptionExceptTermination())
                return;
            isOpen = false;
        }
        if (isOpen) {
            callMethodNamed(globalObject, stream, Identifier::fromString(vm, "close"_s));
            if (scope.exception())
                reportUnraisableShowing(globalObject, "Exception ignored while finalizing file"_s, stream);
            if (scope.exception())
                return;
        }
        // If it could not be passed on it never will be.
        uncheckedDowncast<PyStateObject>(stream)->owedOutput()->note(globalObject, false);
    }
}

IOModuleState& ioState(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    IOModuleState& state = realm->ioModule();
    if (state.unsupportedOperation) [[likely]]
        return state;

    // type("UnsupportedOperation", (OSError, ValueError), {})
    MarkedArgumentBuffer arguments;
    arguments.append(jsNontrivialString(vm, "UnsupportedOperation"_s));
    arguments.append(PyTuple::create(globalObject, { realm->typeOSError(), realm->type(BuiltinType::ValueError) }));
    arguments.append(PyDict::create(globalObject));
    JSValue unsupportedOperation = call(globalObject, realm->typeType(), arguments);
    state.unsupportedOperation.set(vm, realm, asType(unsupportedOperation));
    setAttribute(globalObject, unsupportedOperation, vm.pythonNames().dunder_module, jsNontrivialString(vm, "io"_s));

    initializeBaseClasses(globalObject, state);
    initializeFileIO(globalObject, state);
    initializeBytesIO(globalObject, state);
    initializeBufferedIO(globalObject, state);
    initializeTextIO(globalObject, state);
    initializeStringIO(globalObject, state);
    return state;
}

// _io_open_impl(). A null String is None.
JSValue openFile(JSGlobalObject* globalObject, JSValue file, const String& mode, int buffering, const String& encoding, const String& errors, const String& newline, bool closesDescriptor, JSValue opener)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    IOModuleState& io = ioState(globalObject);
    JSValue pathOrDescriptor = file;
    if (!isNumber(globalObject, file)) {
        pathOrDescriptor = fileSystemPathOf(globalObject, file);
        RETURN_IF_EXCEPTION(scope, { });
    }

    bool isCreating = false;
    bool isReading = false;
    bool isWriting = false;
    bool isAppending = false;
    bool isUpdating = false;
    bool isText = false;
    bool isBinary = false;
    for (unsigned i = 0; i < mode.length(); ++i) {
        char16_t c = mode[i];
        bool* flag = c == 'x' ? &isCreating : c == 'r' ? &isReading : c == 'w' ? &isWriting : c == 'a' ? &isAppending : c == '+' ? &isUpdating : c == 't' ? &isText : c == 'b' ? &isBinary : nullptr;
        // None of them twice.
        if (!flag || *flag)
            return raiseValueError(globalObject, scope, concatenate("invalid mode: '"_s, mode, '\''));
        *flag = true;
    }
    StringBuilder rawMode;
    if (isCreating)
        rawMode.append('x');
    if (isReading)
        rawMode.append('r');
    if (isWriting)
        rawMode.append('w');
    if (isAppending)
        rawMode.append('a');
    if (isUpdating)
        rawMode.append('+');

    if (isText && isBinary)
        return raiseValueError(globalObject, scope, "can't have text and binary mode at once"_s);
    if (isCreating + isReading + isWriting + isAppending > 1)
        return raiseValueError(globalObject, scope, "must have exactly one of create/read/write/append mode"_s);
    if (isBinary && !encoding.isNull())
        return raiseValueError(globalObject, scope, "binary mode doesn't take an encoding argument"_s);
    if (isBinary && !errors.isNull())
        return raiseValueError(globalObject, scope, "binary mode doesn't take an errors argument"_s);
    if (isBinary && !newline.isNull())
        return raiseValueError(globalObject, scope, "binary mode doesn't take a newline argument"_s);
    if (isBinary && buffering == 1 && !warn(globalObject, BuiltinType::RuntimeWarning, "line buffering (buffering=1) isn't supported in binary mode, the default buffer size will be used"_s))
        return { };

    MarkedArgumentBuffer rawArguments;
    rawArguments.append(pathOrDescriptor);
    rawArguments.append(jsString(vm, rawMode.isEmpty() ? emptyString() : rawMode.toString()));
    rawArguments.append(jsBoolean(closesDescriptor));
    rawArguments.append(opener);
    JSValue result = call(globalObject, io.fileIO.get(), rawArguments);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue raw = result;

    // From here on what has been opened is closed if anything goes wrong.
    auto finish = [&] () -> JSValue {
        bool isTerminal = false;
        if (buffering < 0) {
            JSValue answer = callMethodNamed(globalObject, raw, names.attribute__isatty_open_only);
            RETURN_IF_EXCEPTION(scope, { });
            isTerminal = isTrue(globalObject, answer);
            RETURN_IF_EXCEPTION(scope, { });
        }
        bool isLineBuffered = buffering == 1 || isTerminal;
        if (isLineBuffered)
            buffering = -1;
        if (buffering < 0) {
            JSValue blockSize = getAttribute(globalObject, raw, names.attribute__blksize);
            RETURN_IF_EXCEPTION(scope, { });
            auto size = toCLong(globalObject, blockSize);
            RETURN_IF_EXCEPTION(scope, { });
            buffering = static_cast<int>(std::max<int64_t>(std::min<int64_t>(static_cast<int>(*size), 8192 * 1024), defaultBufferSize));
        }
        if (buffering < 0)
            return raiseValueError(globalObject, scope, "invalid buffering size"_s);
        if (!buffering) {
            if (!isBinary)
                return raiseValueError(globalObject, scope, "can't have unbuffered text I/O"_s);
            return result;
        }
        PyType* bufferedClass = isUpdating ? io.bufferedRandom.get() : isCreating || isWriting || isAppending ? io.bufferedWriter.get() : isReading ? io.bufferedReader.get() : nullptr;
        if (!bufferedClass)
            return raiseValueError(globalObject, scope, concatenate("unknown mode: '"_s, mode, '\''));
        JSValue buffer = call(globalObject, bufferedClass, raw, jsNumber(buffering));
        RETURN_IF_EXCEPTION(scope, { });
        result = buffer;
        if (isBinary)
            return result;
        auto orNone = [&] (const String& text) -> JSValue { return text.isNull() ? jsUndefined() : JSValue(jsString(vm, text)); };
        MarkedArgumentBuffer wrapperArguments;
        wrapperArguments.append(buffer);
        wrapperArguments.append(orNone(encoding));
        wrapperArguments.append(orNone(errors));
        wrapperArguments.append(orNone(newline));
        wrapperArguments.append(jsBoolean(isLineBuffered));
        JSValue wrapper = call(globalObject, io.textIOWrapper.get(), wrapperArguments);
        RETURN_IF_EXCEPTION(scope, { });
        result = wrapper;
        setAttribute(globalObject, wrapper, names.attribute_mode, jsString(vm, mode));
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    };
    JSValue opened = finish();
    if (!scope.exception())
        return opened;
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, { });
    callMethodNamed(globalObject, result, names.attribute_close);
    scope.release();
    chainRaisedExceptions(globalObject, raised);
    return { };
}

// open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, closefd=True, opener=None)
PYTHON_NATIVE(ioOpen)
{
    NATIVE_PROLOGUE();
    String mode = "r"_s;
    if (JSValue value = args.at(1)) {
        auto given = toTextArgument(globalObject, value, "open"_s, "argument 'mode'"_s);
        RETURN_IF_EXCEPTION(scope, { });
        mode = *given;
    }
    int buffering = -1;
    if (JSValue value = args.at(2)) {
        auto given = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        buffering = *given;
    }
    String texts[3];
    static constexpr ASCIILiteral textNames[] = { "argument 'encoding'"_s, "argument 'errors'"_s, "argument 'newline'"_s };
    for (unsigned i = 0; i < 3; ++i) {
        if (JSValue value = args.at(3 + i)) {
            auto given = toTextArgument(globalObject, value, "open"_s, textNames[i], true);
            RETURN_IF_EXCEPTION(scope, { });
            texts[i] = *given;
        }
    }
    bool closesDescriptor = true;
    if (JSValue value = args.at(6)) {
        closesDescriptor = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue opener = args.at(7);
    RELEASE_AND_RETURN(scope, JSValue::encode(openFile(globalObject, args.at(0), mode, buffering, texts[0], texts[1], texts[2], closesDescriptor, opener ? opener : jsUndefined())));
}

// PyFile_OpenCodeObject()
JSValue openCode(JSGlobalObject* globalObject, JSValue path)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!stringIn(path))
        return raiseTypeError(globalObject, scope, concatenate("'path' must be 'str', not '"_s, typeName(globalObject, path), '\''));
    if (auto hook = globalObject->pyRealm()->configuration().openCode)
        RELEASE_AND_RETURN(scope, hook(globalObject, path));
    RELEASE_AND_RETURN(scope, openFile(globalObject, path, "rb"_s, -1, String(), String(), String(), true, jsUndefined()));
}

PYTHON_NATIVE(ioOpenCode)
{
    NATIVE_PROLOGUE();
    JSValue path = args.at(0);
    if (!stringIn(path))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("open_code() argument 'path' must be str, not "_s, typeNameOfArgument(globalObject, path))));
    RELEASE_AND_RETURN(scope, JSValue::encode(openCode(globalObject, path)));
}

JSObject* createIOModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    IOModuleState& state = ioState(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "_io"_s);
    auto put = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    put("DEFAULT_BUFFER_SIZE"_s, jsNumber(static_cast<int32_t>(defaultBufferSize)));
    put("UnsupportedOperation"_s, state.unsupportedOperation.get());
    put("BlockingIOError"_s, realm->type(BuiltinType::BlockingIOError));
    for (PyType* type : { state.ioBase.get(), state.textIOBase.get(), state.bufferedIOBase.get(), state.rawIOBase.get(), state.fileIO.get(), state.bytesIO.get(), state.bytesIOBuffer.get(), state.bufferedWriter.get(), state.bufferedReader.get(), state.bufferedRWPair.get(), state.bufferedRandom.get(), state.incrementalNewlineDecoder.get(), state.stringIO.get(), state.textIOWrapper.get() })
        module->putDirect(vm, Identifier::fromString(vm, type->nameWithoutModule(globalObject)), type);
    addFunction(globalObject, module, "open"_s, ioOpen);
    addFunction(globalObject, module, "open_code"_s, ioOpenCode);
    addFunction(globalObject, module, "text_encoding"_s, ioTextEncoding);
    return module;
}

} } // namespace JSC::Python
