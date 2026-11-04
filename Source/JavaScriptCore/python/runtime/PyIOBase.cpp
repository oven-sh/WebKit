/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyIO.h"

#include "Exception.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "PyCodeCache.h"
#include "PyCodecs.h"
#include "PyCommandLine.h"
#include "PyDict.h"
#include "PyExec.h"
#include "PyModuleHelpers.h"
#include "PyType.h"
#include "TopExceptionScope.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _io itself, what its files share, and the abstract classes: _IOBase, _RawIOBase, _BufferedIOBase, _TextIOBase.
// The methods of these call those a subclass is to provide by looking them up on the object, in the order CPython does.

// ---- The types and the names ----

PyTuple* ioMakeState(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer state;
    for (unsigned i = 0; i < static_cast<unsigned>(IOType::Count) + static_cast<unsigned>(IOName::Count); ++i)
        state.append(jsUndefined());
    state.append(newList(globalObject));
    state.append(newList(globalObject));

    auto add = [&](IOType id, const TypeDef& def, PyType* base) {
        PyType* type = rt.typeFor(def, base);
        valuesOf(state)[static_cast<unsigned>(id)] = type;
        return type;
    };
    // object.__new__ makes the instances of the abstract classes.
    auto addAbstract = [&](IOType id, const TypeDef& def, PyType* base) {
        PyType* type = add(id, def, base);
        type->remove(TypeFlag::DisallowInstantiation);
        return type;
    };
    PyType* ioBase = addAbstract(IOType::IOBase, ioIOBaseTypeDef, nullptr);
    PyType* rawIOBase = addAbstract(IOType::RawIOBase, ioRawIOBaseTypeDef, ioBase);
    PyType* bufferedIOBase = addAbstract(IOType::BufferedIOBase, ioBufferedIOBaseTypeDef, ioBase);
    PyType* textIOBase = addAbstract(IOType::TextIOBase, ioTextIOBaseTypeDef, ioBase);
    add(IOType::FileIO, ioFileIOTypeDef, rawIOBase);
    add(IOType::BytesIO, ioBytesIOTypeDef, bufferedIOBase);
    add(IOType::BytesIOBuffer, ioBytesIOBufferTypeDef, nullptr);
    add(IOType::StringIO, ioStringIOTypeDef, textIOBase);
    add(IOType::BufferedReader, ioBufferedReaderTypeDef, bufferedIOBase);
    add(IOType::BufferedWriter, ioBufferedWriterTypeDef, bufferedIOBase);
    add(IOType::BufferedRWPair, ioBufferedRWPairTypeDef, bufferedIOBase);
    add(IOType::BufferedRandom, ioBufferedRandomTypeDef, bufferedIOBase);
    add(IOType::IncrementalNewlineDecoder, ioIncrementalNewlineDecoderTypeDef, nullptr);
    add(IOType::TextIOWrapper, ioTextIOWrapperTypeDef, textIOBase);
    add(IOType::BuiltinDecoder, ioBuiltinDecoderTypeDef, nullptr);

    PyDict* namespaceDict = PyDict::create(globalObject);
    namespaceDict->set(vm, rt.names.dModule(), makeStr(vm, "io"_s));
    JSValue unsupportedOperation = call(globalObject, rt.typeType(), makeStr(vm, "UnsupportedOperation"_s), PyTuple::create(globalObject, { rt.OSError(), rt.ValueError() }), namespaceDict);
    scope.assertNoException();
    RELEASE_ASSERT(unsupportedOperation);
    valuesOf(state)[static_cast<unsigned>(IOType::UnsupportedOperation)] = unsupportedOperation;

    unsigned index = static_cast<unsigned>(IOType::Count);
#define PY_ADD_NAME(id, text) valuesOf(state)[index++] = internString(vm, text ""_s);
    FOR_EACH_IO_NAME(PY_ADD_NAME)
#undef PY_ADD_NAME

    PyTuple* tuple = PyTuple::create(globalObject, spanOf(state));
    setHiddenField(globalObject, ioBase, 0, tuple);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return tuple;
}

// ---- Errors ----

JSValue ioRaiseUnsupported(JSGlobalObject* globalObject, ThrowScope& scope, const String& message)
{
    return raise(globalObject, scope, ioType(globalObject, IOType::UnsupportedOperation), message);
}

JSValue ioRaiseClosed(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raiseValueError(globalObject, scope, "I/O operation on closed file."_s);
}

void ioRestoreException(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwPythonException(globalObject, scope, exception);
}

void ioChainExceptions(JSGlobalObject* globalObject, JSValue saved)
{
    if (!saved)
        return;
    JSValue current = takePendingException(globalObject);
    if (!current)
        return ioRestoreException(globalObject, saved);
    if (current != saved)
        PyException::from(current)->setContext(getVM(globalObject), saved);
    ioRestoreException(globalObject, current);
}

bool ioTrapEINTR(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    if (!pendingExceptionMatches(globalObject, rt.OSError()))
        return false;
    JSValue number = PyException::from(pendingException(globalObject))->field(ExceptionField::osErrorErrno);
    if (!number || !number.isInt32() || number.asInt32() != EINTR)
        return false;
    takePendingException(globalObject);
    return true;
}

JSValue ioCannotPickle(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "cannot pickle '"_s, typeOf(globalObject, self)->name(), "' instances"_s);
}

bool ioToFinalizing(JSGlobalObject* globalObject, JSValue value, bool& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete numeric/char attribute"_s);
        return false;
    }
    if (!value.isBoolean()) {
        raiseTypeError(globalObject, scope, "attribute value type must be bool"_s);
        return false;
    }
    result = value.asBoolean();
    return true;
}

// Hands what propagates to sys.unraisablehook, as something that went wrong while the stream was finalized.
static void ioReportWhileFinalizing(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return;
    // At exit: a daemon thread, which never runs again, is in a call on the stream and holds its lock. CPython leaves such a stream
    // alone, as the frame of the thread refers to it.
    if (typeOf(globalObject, exception) == runtime(globalObject).PythonFinalizationError())
        return;
    String text = reprForMessage(globalObject, self);
    ioRestoreException(globalObject, exception);
    EXCEPTION_ASSERT_UNUSED(scope, scope.exception());
    reportUnraisable(globalObject, makeString("Exception ignored while finalizing file "_s, text), { });
}

void ioWarnUnclosed(JSGlobalObject* globalObject, JSValue self, JSValue source)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // An exception that is propagating is set aside.
    SuspendExceptionScope suspended(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSString* text = repr(globalObject, source);
    if (!scope.exception()) {
        String string = text->value(globalObject);
        if (!scope.exception())
            warnAbout(globalObject, rt.ResourceWarning(), makeString("unclosed file "_s, string), 1, source);
    }
    if (!scope.exception())
        return;
    if (pendingExceptionMatches(globalObject, rt.Warning()))
        ioReportWhileFinalizing(globalObject, self);
    else
        takePendingException(globalObject);
}

// ---- Arguments ----

bool ioToInt(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto number = tryInt64(index);
    if (!number || *number < std::numeric_limits<int>::min() || *number > std::numeric_limits<int>::max()) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
        return false;
    }
    result = static_cast<int>(*number);
    return true;
}

bool ioToOptionalSize(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    if (!value || isNone(value))
        return true;
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasIndex(globalObject, value)) {
        raiseTypeError(globalObject, scope, "argument should be integer or None, not '"_s, typeName(globalObject, value), '\'');
        return false;
    }
    int64_t size = toSsize(globalObject, value, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, false);
    result = size;
    return true;
}

int64_t ioToOffset(JSGlobalObject* globalObject, JSValue value, PyType* overflowError)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (auto result = tryInt64(index))
        return *result;
    if (!overflowError)
        return intSign(index) < 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    raise(globalObject, scope, overflowError, makeString("cannot fit '"_s, typeName(globalObject, value), "' into an offset-sized integer"_s));
    return -1;
}

bool ioCheckUTF8(JSGlobalObject* globalObject, JSString* string)
{
    if (string->is8Bit())
        return true;
    return !!codecEncodeUTF8(globalObject, string, "strict"_s);
}

bool ioToOptionalString(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral argument, String& result)
{
    if (!value || isNone(value))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, function, "() argument '"_s, argument, "' must be str or None, not "_s, typeName(globalObject, value));
        return false;
    }
    String string = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (failed(scope, ioCheckUTF8(globalObject, strValue(value))))
        return false;
    if (string.contains(static_cast<char16_t>(0))) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    result = string.isNull() ? emptyString() : string;
    return true;
}

JSValue ioFSPath(JSGlobalObject* globalObject, JSValue path)
{
    if (isStr(path) || PyBytes::is(path))
        return path;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = typeOf(globalObject, path)->lookup(Special::FsPath);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, "expected str, bytes or os.PathLike object, not "_s, typeOf(globalObject, path)->name());
    JSValue result = callSpecial(globalObject, method, path);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(result) && !PyBytes::is(result))
        return raiseTypeError(globalObject, scope, "expected "_s, typeOf(globalObject, path)->name(), ".__fspath__() to return str or bytes, not "_s, typeOf(globalObject, result)->name());
    return result;
}

bool IOBuffer::acquire(JSGlobalObject* globalObject, JSValue value)
{
    ASSERT(!m_globalObject && !m_buffer.owner);
    if (auto* bytes = PyBytes::tryFrom(value); bytes && !bytes->type()->isHeapType()) [[likely]] {
        m_buffer.owner = bytes;
        m_buffer.data = bytes->mutableData();
        m_buffer.length = bytes->length();
        return true;
    }
    m_globalObject = globalObject;
    return getBuffer(globalObject, value, m_buffer, BufferSimple);
}

bool IOBuffer::acquireWritable(JSGlobalObject* globalObject, ASCIILiteral function, JSValue value, ASCIILiteral argument)
{
    ASSERT(!m_globalObject && !m_buffer.owner);
    m_globalObject = globalObject;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!failed(scope, getBuffer(globalObject, value, m_buffer, BufferWritable)))
        return true;
    takePendingException(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    raiseTypeError(globalObject, scope, function, "() "_s, argument, " must be read-write bytes-like object, not "_s, isNone(value) ? "None"_str : typeName(globalObject, value));
    return false;
}

PyByteArray* ioNewByteArray(JSGlobalObject* globalObject, size_t size)
{
    Runtime& rt = runtime(globalObject);
    PyByteArray* result = PyByteArray::create(rt.vm(), rt.structure<PyByteArray>(), rt.bytearrayType());
    if (size && !result->resize(globalObject, size))
        return nullptr;
    return result;
}

static constexpr size_t ioMaxAllocation = static_cast<size_t>(1) << 48;

PyBytes* ioTryNewBytes(JSGlobalObject* globalObject, size_t length)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!length)
        return rt.emptyBytes();
    PyBytes* result = length <= ioMaxAllocation ? PyBytes::tryCreateUninitialized(vm, rt.structure<PyBytes>(), rt.bytesType(), length) : nullptr;
    if (!result)
        raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
    return result;
}

PyByteStorage* ioTryNewStorage(JSGlobalObject* globalObject, size_t capacity)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyByteStorage* result = capacity <= ioMaxAllocation ? PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), capacity) : nullptr;
    if (!result)
        raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
    return result;
}

// ---- What can be asked of any stream ----

bool ioCheckClosed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What the class of the object says, which is seldom what _IOBase itself keeps track of.
    JSValue closed = getAttrIfPresent(globalObject, self, ioName(globalObject, IOName::closed));
    RETURN_IF_EXCEPTION(scope, false);
    if (!closed)
        return true;
    bool isClosed = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, false);
    if (isClosed) {
        ioRaiseClosed(globalObject, scope);
        return false;
    }
    return true;
}

static bool ioCheckCapability(JSGlobalObject* globalObject, JSValue self, IOName method, ASCIILiteral message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = callMethod(globalObject, self, ioName(globalObject, method));
    RETURN_IF_EXCEPTION(scope, false);
    if (!result.isTrue()) {
        ioRaiseUnsupported(globalObject, scope, message);
        return false;
    }
    return true;
}

bool ioCheckReadable(JSGlobalObject* globalObject, JSValue self) { return ioCheckCapability(globalObject, self, IOName::readable, "File or stream is not readable."_s); }
bool ioCheckWritable(JSGlobalObject* globalObject, JSValue self) { return ioCheckCapability(globalObject, self, IOName::writable, "File or stream is not writable."_s); }
bool ioCheckSeekable(JSGlobalObject* globalObject, JSValue self) { return ioCheckCapability(globalObject, self, IOName::seekable, "File or stream is not seekable."_s); }

bool ioFlush(JSGlobalObject* globalObject, JSValue stream)
{
    return !!callMethod(globalObject, stream, runtime(globalObject).names.flush());
}

// ---- _IOBase ----

// Whether _IOBase.close() has been called: not the `closed` attribute, which subclasses replace.
static bool ioBaseIsClosed(JSGlobalObject* globalObject, JSValue self)
{
    return !!getAttrIfPresent(globalObject, self, ioName(globalObject, IOName::ioBaseClosed));
}

static JSValue ioBaseSeek(JSGlobalObject* globalObject, JSValue, JSValue offset, JSValue whence)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int ignored;
    if (failed(scope, ioToInt(globalObject, offset, ignored)) || (whence && failed(scope, ioToInt(globalObject, whence, ignored))))
        return { };
    return ioRaiseUnsupported(globalObject, scope, "seek"_s);
}

static JSValue ioBaseTell(JSGlobalObject* globalObject, JSValue self)
{
    return callMethod(globalObject, self, ioName(globalObject, IOName::seek), jsNumber(0), jsNumber(1));
}

static JSValue ioBaseTruncate(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return ioRaiseUnsupported(globalObject, scope, "truncate"_s);
}

static JSValue ioBaseFlush(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isClosed = ioBaseIsClosed(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isClosed)
        return ioRaiseClosed(globalObject, scope);
    return jsUndefined();
}

static JSValue ioBaseGetClosed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isClosed = ioBaseIsClosed(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(isClosed);
}

static JSValue ioBaseGetDict(JSGlobalObject* globalObject, JSValue self)
{
    return instanceDict(globalObject, self, true);
}

JSValue ioBaseClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isClosed = ioBaseIsClosed(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (isClosed)
        return jsUndefined();

    JSValue flushError;
    if (failed(scope, ioFlush(globalObject, self)))
        flushError = takePendingException(globalObject);
    setAttr(globalObject, self, ioName(globalObject, IOName::ioBaseClosed), jsBoolean(true));
    if (scope.exception() || flushError)
        ioChainExceptions(globalObject, flushError);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue ioBaseCheckClosed(JSGlobalObject* globalObject, JSValue self) { return ioCheckClosed(globalObject, self) ? jsUndefined() : JSValue(); }
static JSValue ioBaseCheckSeekable(JSGlobalObject* globalObject, JSValue self) { return ioCheckSeekable(globalObject, self) ? jsBoolean(true) : JSValue(); }
static JSValue ioBaseCheckReadable(JSGlobalObject* globalObject, JSValue self) { return ioCheckReadable(globalObject, self) ? jsBoolean(true) : JSValue(); }
static JSValue ioBaseCheckWritable(JSGlobalObject* globalObject, JSValue self) { return ioCheckWritable(globalObject, self) ? jsBoolean(true) : JSValue(); }

static JSValue ioBaseFalse(JSGlobalObject*, JSValue) { return jsBoolean(false); }

static JSValue ioBaseEnter(JSGlobalObject* globalObject, JSValue self) { return ioCheckClosed(globalObject, self) ? self : JSValue(); }
static JSValue ioBaseExit(JSGlobalObject* globalObject, JSValue self, const CallArgs&) { return callMethod(globalObject, self, runtime(globalObject).names.close()); }

static JSValue ioBaseFileno(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return ioRaiseUnsupported(globalObject, scope, "fileno"_s);
}

static JSValue ioBaseIsATTY(JSGlobalObject* globalObject, JSValue self) { return ioCheckClosed(globalObject, self) ? jsBoolean(false) : JSValue(); }

// One byte at a time, unless the stream has a peek() that shows where the line ends.
static JSValue ioBaseReadline(JSGlobalObject* globalObject, JSValue self, JSValue size)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t limit = -1;
    if (failed(scope, ioToOptionalSize(globalObject, size, limit)))
        return { };

    JSValue peek = getAttrIfPresent(globalObject, self, ioName(globalObject, IOName::peek));
    RETURN_IF_EXCEPTION(scope, { });

    Vector<uint8_t, 256> line;
    while (limit < 0 || static_cast<int64_t>(line.size()) < limit) {
        int64_t readAhead = 1;
        if (peek) {
            JSValue ahead = call(globalObject, peek, jsNumber(1));
            if (failed(scope, ahead)) {
                if (ioTrapEINTR(globalObject))
                    continue;
                return { };
            }
            auto* bytes = PyBytes::tryFrom(ahead);
            if (!bytes)
                return raise(globalObject, scope, rt.OSError(), makeString("peek() should have returned a bytes object, not '"_s, typeName(globalObject, ahead), '\''));
            if (bytes->length()) {
                size_t available = bytes->length();
                if (limit >= 0)
                    available = std::min<size_t>(available, limit - line.size());
                auto* newline = static_cast<const uint8_t*>(memchr(bytes->data(), '\n', available));
                readAhead = newline ? newline - bytes->data() + 1 : available;
            }
        }
        JSValue data = callMethod(globalObject, self, rt.names.read(), makeInt(globalObject, readAhead));
        if (failed(scope, data)) {
            if (ioTrapEINTR(globalObject))
                continue;
            return { };
        }
        auto* bytes = PyBytes::tryFrom(data);
        if (!bytes)
            return raise(globalObject, scope, rt.OSError(), makeString("read() should have returned a bytes object, not '"_s, typeName(globalObject, data), '\''));
        if (!bytes->length())
            break;
        line.append(bytes->span());
        if (line.last() == '\n')
            break;
    }
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, line.span()));
}

static JSValue ioBaseIter(JSGlobalObject* globalObject, JSValue self) { return ioCheckClosed(globalObject, self) ? self : JSValue(); }

static JSValue ioBaseNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue line = callMethod(globalObject, self, runtime(globalObject).names.readline());
    RETURN_IF_EXCEPTION(scope, { });
    int64_t size = length(globalObject, line);
    RETURN_IF_EXCEPTION(scope, { });
    if (size <= 0)
        return raiseStopIteration(globalObject, scope);
    return line;
}

static JSValue ioBaseReadlines(JSGlobalObject* globalObject, JSValue self, JSValue hintValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hint = -1;
    if (failed(scope, ioToOptionalSize(globalObject, hintValue, hint)))
        return { };
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (hint <= 0) {
        listExtend(globalObject, result, self);
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }
    int64_t total = 0;
    forEach(globalObject, self, [&](JSValue line) {
        listAppend(globalObject, result, line);
        RETURN_IF_EXCEPTION(scope, false);
        int64_t size = length(globalObject, line);
        RETURN_IF_EXCEPTION(scope, false);
        if (size > hint - total)
            return false;
        total += size;
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue ioBaseWritelines(JSGlobalObject* globalObject, JSValue self, JSValue lines)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, ioCheckClosed(globalObject, self)))
        return { };
    forEach(globalObject, lines, [&](JSValue line) {
        JSValue result;
        do {
            result = callMethod(globalObject, self, rt.names.write(), line);
        } while (!result && ioTrapEINTR(globalObject));
        return !!result;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

// ---- Finalization ----

static JSValue ioBaseDel(JSGlobalObject*, JSValue self);

static bool ioHasOwnFinalizer(PyType* type)
{
    auto* native = PyNativeFunction::tryFrom(type->lookup(Special::Del));
    return !native || native->def().pointer.f1 != ioBaseDel;
}

void ioDidAllocate(JSGlobalObject* globalObject, PyRecord* stream)
{
    if (ioHasOwnFinalizer(stream->type())) [[unlikely]]
        runtime(globalObject).registerFinalizable(stream);
}

void ioDidOpen(JSGlobalObject* globalObject, PyRecord* stream)
{
    runtime(globalObject).registerFinalizable(stream);
}

void ioDidClose(JSGlobalObject* globalObject, PyRecord* stream)
{
    if (!ioHasOwnFinalizer(stream->type())) [[likely]]
        runtime(globalObject).unregisterFinalizable(stream);
}

void ioDidWrap(JSGlobalObject* globalObject, JSValue stream)
{
    if (isPyObject(stream) && !ioHasOwnFinalizer(typeOf(globalObject, stream)))
        runtime(globalObject).unregisterFinalizable(stream.asCell());
}

// The standard streams that can be written to, each followed by its buffer.
static JSArray* ioStandardStreams(JSGlobalObject* globalObject)
{
    return asArray(ioState(globalObject)->at(static_cast<unsigned>(IOType::Count) + static_cast<unsigned>(IOName::Count)));
}

static JSArray* ioStreamsWithPendingOutput(JSGlobalObject* globalObject)
{
    return asArray(ioState(globalObject)->at(static_cast<unsigned>(IOType::Count) + static_cast<unsigned>(IOName::Count) + 1));
}

void ioNotePendingOutput(JSGlobalObject* globalObject, PyRecord* stream)
{
    JSArray* standardStreams = ioStandardStreams(globalObject);
    for (unsigned i = 0; i < listLength(standardStreams); ++i) {
        if (listGet(standardStreams, i) == JSValue(stream))
            return;
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    listAppend(globalObject, ioStreamsWithPendingOutput(globalObject), stream);
    scope.clearException();
    runtime(globalObject).setMayHavePendingOutput(true);
}

void writeOutPendingOutputSlow(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    if (scope.exception())
        return;
    rt.setMayHavePendingOutput(false);
    JSArray* list = ioStreamsWithPendingOutput(globalObject);
    PyType* textIOWrapper = ioType(globalObject, IOType::TextIOWrapper);
    // What a TextIOWrapper holds goes to its buffer, which then is in the list.
    for (unsigned round = 0; round < 2 && listLength(list); ++round) {
        MarkedArgumentBuffer streams;
        for (unsigned i = 0; i < listLength(list); ++i)
            streams.append(listGet(list, i));
        listTruncate(globalObject, list, 0);
        for (unsigned i = 0; i < streams.size(); ++i) {
            auto* stream = PyRecord::from(streams.at(i));
            if (stream->type() == textIOWrapper)
                textIOWriteOutPending(globalObject, stream);
            else
                bufferedWriteOutPending(globalObject, stream);
        }
    }
}

void ioDidUnwrap(JSGlobalObject* globalObject, JSValue stream)
{
    if (!isPyObject(stream) || ioHasOwnFinalizer(typeOf(globalObject, stream)))
        return;
    // See flushAllOpenFiles().
    JSArray* standardStreams = ioStandardStreams(globalObject);
    for (unsigned i = 0; i < listLength(standardStreams); ++i) {
        if (listGet(standardStreams, i) == stream)
            return;
    }
    runtime(globalObject).registerFinalizable(stream.asCell());
}

// True only if the stream says so.
static bool ioSaysItIsClosed(JSGlobalObject* globalObject, JSValue stream, bool ifItCannotTell)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue closed = getAttrIfPresent(globalObject, stream, ioName(globalObject, IOName::closed));
    bool result = ifItCannotTell;
    if (closed && !scope.exception())
        result = isTrue(globalObject, closed);
    if (scope.exception()) {
        scope.clearException();
        return ifItCannotTell;
    }
    return result;
}

static JSValue ioBaseDel(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    // One that cannot tell whether it is closed is in no state to be closed.
    if (!ioSaysItIsClosed(globalObject, self, true)) {
        // So that close() knows who is calling.
        setAttr(globalObject, self, ioName(globalObject, IOName::finalizing), jsBoolean(true));
        scope.clearException();
        callMethod(globalObject, self, rt.names.close());
        if (scope.exception())
            ioReportWhileFinalizing(globalObject, self);
    }

    // If that has left what the stream wraps open, it is on its own again.
    if (auto* record = PyRecord::tryFrom(self)) {
        std::pair<JSValue, JSValue> wrapped;
        PyType* type = record->type();
        if (type->isSubtypeOf(ioType(globalObject, IOType::TextIOWrapper)))
            wrapped.first = textIOWrappedStream(record);
        else if (type->isSubtypeOf(ioType(globalObject, IOType::BufferedReader)) || type->isSubtypeOf(ioType(globalObject, IOType::BufferedWriter)) || type->isSubtypeOf(ioType(globalObject, IOType::BufferedRandom)))
            wrapped.first = bufferedWrappedStream(record);
        else if (type->isSubtypeOf(ioType(globalObject, IOType::BufferedRWPair)))
            wrapped = bufferedRWPairWrappedStreams(record);
        for (JSValue stream : { wrapped.first, wrapped.second }) {
            if (stream && !ioSaysItIsClosed(globalObject, stream, false))
                ioDidUnwrap(globalObject, stream);
        }
    }
    return jsUndefined();
}

static constexpr NativeDef ioBaseMethods[] = {
    PY_METHOD("seek", ioBaseSeek, "($self, offset, whence=0, /)"),
    PY_METHOD("tell", ioBaseTell, "($self, /)"),
    PY_METHOD("truncate", ioBaseTruncate, "($self, size=None, /)"),
    PY_METHOD("flush", ioBaseFlush, "($self, /)"),
    PY_METHOD("close", ioBaseClose, "($self, /)"),
    PY_METHOD("seekable", ioBaseFalse, "($self, /)"),
    PY_METHOD("readable", ioBaseFalse, "($self, /)"),
    PY_METHOD("writable", ioBaseFalse, "($self, /)"),
    PY_METHOD("_checkClosed", ioBaseCheckClosed, "($self, /)"),
    PY_METHOD("_checkSeekable", ioBaseCheckSeekable, "($self, /)"),
    PY_METHOD("_checkReadable", ioBaseCheckReadable, "($self, /)"),
    PY_METHOD("_checkWritable", ioBaseCheckWritable, "($self, /)"),
    PY_METHOD("fileno", ioBaseFileno, "($self, /)"),
    PY_METHOD("isatty", ioBaseIsATTY, "($self, /)"),
    PY_METHOD("__enter__", ioBaseEnter, "($self, /)"),
    PY_METHOD("__exit__", ioBaseExit, "($self, /, *args)"),
    PY_METHOD("readline", ioBaseReadline, "($self, size=-1, /)"),
    PY_METHOD("readlines", ioBaseReadlines, "($self, hint=-1, /)"),
    PY_METHOD("writelines", ioBaseWritelines, "($self, lines, /)"),
    PY_SLOT("__iter__", ioBaseIter, "($self, /)"),
    PY_SLOT("__next__", ioBaseNext, "($self, /)"),
    PY_SLOT("__del__", ioBaseDel, "($self, /)"),
};

static constexpr NativeGetSet ioBaseGetSets[] = {
    { "__dict__"_s, ioBaseGetDict },
    { "closed"_s, ioBaseGetClosed },
};

constexpr TypeDef ioIOBaseTypeDef { "_io._IOBase"_s, { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::HasWeakref }, ioBaseMethods, ioBaseGetSets,
    "The abstract base class of all streams.\n\n"
    "It provides the methods that make sense for any of them, and versions of the others that say the operation is not\n"
    "supported. A stream is a context manager, which closes it, and an iterator over its lines."_s };

// ---- _RawIOBase ----

static JSValue rawIOBaseRead(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (sizeValue && failed(scope, ssizeArgument(globalObject, sizeValue, size)))
        return { };
    if (size < 0)
        RELEASE_AND_RETURN(scope, callMethod(globalObject, self, ioName(globalObject, IOName::readall)));

    PyByteArray* buffer = ioNewByteArray(globalObject, size);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = callMethod(globalObject, self, ioName(globalObject, IOName::readinto), buffer);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(result))
        return result;
    int64_t filled = toSsize(globalObject, result, rt.ValueError());
    RETURN_IF_EXCEPTION(scope, { });
    if (filled < 0 || filled > size)
        return raiseValueError(globalObject, scope, "readinto returned "_s, filled, " outside buffer size "_s, size);
    // CPython turns the bytearray into the result, which it cannot do while readinto() holds on to its memory.
    if (isStillExported(globalObject, buffer, [&] { return buffer->exportCount(); }))
        return raise(globalObject, scope, rt.BufferError(), "Existing exports of data: object cannot be re-sized"_s);
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, buffer->span().first(std::min<size_t>(filled, buffer->length()))));
}

static JSValue rawIOBaseReadall(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<uint8_t> all;
    while (true) {
        JSValue data = callMethod(globalObject, self, rt.names.read(), jsNumber(static_cast<int32_t>(ioDefaultBufferSize)));
        if (failed(scope, data)) {
            if (ioTrapEINTR(globalObject))
                continue;
            return { };
        }
        if (isNone(data)) {
            if (all.isEmpty())
                return data;
            break;
        }
        auto* bytes = PyBytes::tryFrom(data);
        if (!bytes)
            return raiseTypeError(globalObject, scope, "read() should return bytes"_s);
        if (!bytes->length())
            break;
        all.append(bytes->span());
    }
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, all.span()));
}

static JSValue rawIOBaseNotImplemented(JSGlobalObject* globalObject, JSValue, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, runtime(globalObject).NotImplementedError(), { });
}

static constexpr NativeDef rawIOBaseMethods[] = {
    PY_METHOD("read", rawIOBaseRead, "($self, size=-1, /)"),
    PY_METHOD("readall", rawIOBaseReadall, "($self, /)"),
    PY_METHOD("readinto", rawIOBaseNotImplemented, "($self, /, *args)"),
    PY_METHOD("write", rawIOBaseNotImplemented, "($self, /, *args)"),
};

constexpr TypeDef ioRawIOBaseTypeDef { "_io._RawIOBase"_s, { TypeFlag::Base }, rawIOBaseMethods, { }, "Base class for raw binary I/O."_s };

// ---- _BufferedIOBase ----

static JSValue bufferedIOBaseReadIntoGeneric(JSGlobalObject* globalObject, JSValue self, JSValue target, bool isReadInto1)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    IOBuffer buffer;
    if (failed(scope, buffer.acquireWritable(globalObject, isReadInto1 ? "readinto1"_s : "readinto"_s, target)))
        return { };
    JSValue data = callMethod(globalObject, self, isReadInto1 ? ioName(globalObject, IOName::read1) : rt.names.read(), makeInt(globalObject, static_cast<uint64_t>(buffer.size())));
    RETURN_IF_EXCEPTION(scope, { });
    auto* bytes = PyBytes::tryFrom(data);
    if (!bytes)
        return raiseTypeError(globalObject, scope, "read() should return bytes"_s);
    if (bytes->length() > buffer.size())
        return raiseValueError(globalObject, scope, "read() returned too much data: "_s, buffer.size(), " bytes requested, "_s, bytes->length(), " returned"_s);
    if (failed(scope, buffer.refresh()))
        return { };
    if (bytes->length())
        memcpy(buffer.mutableData(), bytes->data(), bytes->length());
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<uint64_t>(bytes->length())));
}

static JSValue bufferedIOBaseReadInto(JSGlobalObject* globalObject, JSValue self, JSValue buffer) { return bufferedIOBaseReadIntoGeneric(globalObject, self, buffer, false); }
static JSValue bufferedIOBaseReadInto1(JSGlobalObject* globalObject, JSValue self, JSValue buffer) { return bufferedIOBaseReadIntoGeneric(globalObject, self, buffer, true); }

static JSValue ioUnsupportedWithSize(JSGlobalObject* globalObject, JSValue size, ASCIILiteral operation)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int ignored;
    if (size && failed(scope, ioToInt(globalObject, size, ignored)))
        return { };
    return ioRaiseUnsupported(globalObject, scope, operation);
}

static JSValue ioBaseUnsupportedDetach(JSGlobalObject* globalObject, JSValue) { return ioUnsupportedWithSize(globalObject, { }, "detach"_s); }
static JSValue ioBaseUnsupportedRead(JSGlobalObject* globalObject, JSValue, JSValue size) { return ioUnsupportedWithSize(globalObject, size, "read"_s); }
static JSValue ioBaseUnsupportedRead1(JSGlobalObject* globalObject, JSValue, JSValue size) { return ioUnsupportedWithSize(globalObject, size, "read1"_s); }
static JSValue ioBaseUnsupportedReadline(JSGlobalObject* globalObject, JSValue, JSValue size) { return ioUnsupportedWithSize(globalObject, size, "readline"_s); }
static JSValue ioBaseUnsupportedWrite(JSGlobalObject* globalObject, JSValue, JSValue) { return ioUnsupportedWithSize(globalObject, { }, "write"_s); }

static constexpr NativeDef bufferedIOBaseMethods[] = {
    PY_METHOD("detach", ioBaseUnsupportedDetach, "($self, /)"),
    PY_METHOD("read", ioBaseUnsupportedRead, "($self, size=-1, /)"),
    PY_METHOD("read1", ioBaseUnsupportedRead1, "($self, size=-1, /)"),
    PY_METHOD("readinto", bufferedIOBaseReadInto, "($self, buffer, /)"),
    PY_METHOD("readinto1", bufferedIOBaseReadInto1, "($self, buffer, /)"),
    PY_METHOD("write", ioBaseUnsupportedWrite, "($self, b, /)"),
};

constexpr TypeDef ioBufferedIOBaseTypeDef { "_io._BufferedIOBase"_s, { TypeFlag::Base }, bufferedIOBaseMethods, { },
    "Base class for buffered IO objects.\n\n"
    "Unlike a raw stream, read(), readinto() and write() do not stop short of what was asked for unless the end of the\n"
    "stream is reached, and never return None: a stream that would block raises BlockingIOError."_s };

// ---- _TextIOBase ----

static JSValue textIOBaseWrite(JSGlobalObject* globalObject, JSValue, JSValue text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(text))
        return raiseTypeError(globalObject, scope, "write() argument 1 must be str, not "_s, isNone(text) ? "None"_str : typeName(globalObject, text));
    return ioRaiseUnsupported(globalObject, scope, "write"_s);
}

static JSValue textIOBaseNone(JSGlobalObject*, JSValue) { return jsUndefined(); }

static constexpr NativeDef textIOBaseMethods[] = {
    PY_METHOD("detach", ioBaseUnsupportedDetach, "($self, /)"),
    PY_METHOD("read", ioBaseUnsupportedRead, "($self, size=-1, /)"),
    PY_METHOD("readline", ioBaseUnsupportedReadline, "($self, size=-1, /)"),
    PY_METHOD("write", textIOBaseWrite, "($self, s, /)"),
};

static constexpr NativeGetSet textIOBaseGetSets[] = {
    { "encoding"_s, textIOBaseNone },
    { "newlines"_s, textIOBaseNone },
    { "errors"_s, textIOBaseNone },
};

constexpr TypeDef ioTextIOBaseTypeDef { "_io._TextIOBase"_s, { TypeFlag::Base }, textIOBaseMethods, textIOBaseGetSets,
    "Base class for text I/O.\n\n"
    "A stream of characters and lines. There is no readinto(), as a str cannot be written into."_s };

// ---- open() ----

static bool ioIsNumber(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNumber() || isInt(value))
        return true;
    PyType* type = typeOf(globalObject, value);
    return type->lookup(Special::Index) || type->lookup(Special::Int) || type->lookup(Special::Float) || type->isSubtypeOf(runtime(globalObject).complexType());
}

static JSValue ioOpen(JSGlobalObject* globalObject, JSValue file, JSValue modeValue, JSValue bufferingValue, JSValue encodingValue, JSValue errorsValue, JSValue newlineValue, JSValue closefdValue, JSValue opener)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    String mode = "r"_s;
    if (modeValue) {
        if (!isStr(modeValue))
            return raiseTypeError(globalObject, scope, "open() argument 'mode' must be str, not "_s, isNone(modeValue) ? "None"_str : typeName(globalObject, modeValue));
        if (failed(scope, ioToOptionalString(globalObject, modeValue, "open"_s, "mode"_s, mode)))
            return { };
    }
    int buffering = -1;
    if (bufferingValue && failed(scope, ioToInt(globalObject, bufferingValue, buffering)))
        return { };
    String encoding, errors, newline;
    if (failed(scope, ioToOptionalString(globalObject, encodingValue, "open"_s, "encoding"_s, encoding))
        || failed(scope, ioToOptionalString(globalObject, errorsValue, "open"_s, "errors"_s, errors))
        || failed(scope, ioToOptionalString(globalObject, newlineValue, "open"_s, "newline"_s, newline)))
        return { };
    bool closefd = !closefdValue || isTrue(globalObject, closefdValue);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue pathOrFd = file;
    if (!ioIsNumber(globalObject, file)) {
        pathOrFd = ioFSPath(globalObject, file);
        RETURN_IF_EXCEPTION(scope, { });
    }

    bool creating = false, reading = false, writing = false, appending = false, updating = false, text = false, binary = false;
    for (unsigned i = 0; i < mode.length(); ++i) {
        char16_t character = mode[i];
        bool* flag = nullptr;
        switch (character) {
        case 'x':
            flag = &creating;
            break;
        case 'r':
            flag = &reading;
            break;
        case 'w':
            flag = &writing;
            break;
        case 'a':
            flag = &appending;
            break;
        case '+':
            flag = &updating;
            break;
        case 't':
            flag = &text;
            break;
        case 'b':
            flag = &binary;
            break;
        }
        if (!flag || *flag)
            return raiseValueError(globalObject, scope, "invalid mode: '"_s, mode, '\'');
        *flag = true;
    }

    if (text && binary)
        return raiseValueError(globalObject, scope, "can't have text and binary mode at once"_s);
    if (creating + reading + writing + appending > 1)
        return raiseValueError(globalObject, scope, "must have exactly one of create/read/write/append mode"_s);
    if (binary && !encoding.isNull())
        return raiseValueError(globalObject, scope, "binary mode doesn't take an encoding argument"_s);
    if (binary && !errors.isNull())
        return raiseValueError(globalObject, scope, "binary mode doesn't take an errors argument"_s);
    if (binary && !newline.isNull())
        return raiseValueError(globalObject, scope, "binary mode doesn't take a newline argument"_s);
    if (binary && buffering == 1) {
        if (failed(scope, warn(globalObject, rt.RuntimeWarning(), "line buffering (buffering=1) isn't supported in binary mode, the default buffer size will be used"_s, 1)))
            return { };
    }

    StrBuilder rawMode;
    if (creating)
        rawMode.append('x');
    if (reading)
        rawMode.append('r');
    if (writing)
        rawMode.append('w');
    if (appending)
        rawMode.append('a');
    if (updating)
        rawMode.append('+');

    JSValue raw = call(globalObject, ioType(globalObject, IOType::FileIO), pathOrFd, makeStr(vm, rawMode.toString()), jsBoolean(closefd), opener ? opener : jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });

    // The outermost stream made so far, which is closed if the next cannot be made.
    JSValue result = raw;
    auto finish = [&]() -> JSValue {
        bool isATTY = false;
        if (buffering < 0) {
            JSValue answer = callMethod(globalObject, raw, ioName(globalObject, IOName::isattyOpenOnly));
            RETURN_IF_EXCEPTION(scope, { });
            isATTY = isTrue(globalObject, answer);
            RETURN_IF_EXCEPTION(scope, { });
        }
        bool lineBuffering = buffering == 1 || isATTY;
        if (lineBuffering)
            buffering = -1;
        if (buffering < 0) {
            JSValue blockSize = getAttr(globalObject, raw, ioName(globalObject, IOName::blksize));
            RETURN_IF_EXCEPTION(scope, { });
            int64_t size = toInt64(globalObject, blockSize);
            RETURN_IF_EXCEPTION(scope, { });
            buffering = static_cast<int>(std::max<int64_t>(std::min<int64_t>(size, 8192 * 1024), ioDefaultBufferSize));
        }
        if (!buffering) {
            if (!binary)
                return raiseValueError(globalObject, scope, "can't have unbuffered text I/O"_s);
            return result;
        }

        IOType bufferedType;
        if (updating)
            bufferedType = IOType::BufferedRandom;
        else if (creating || writing || appending)
            bufferedType = IOType::BufferedWriter;
        else if (reading)
            bufferedType = IOType::BufferedReader;
        else
            return raiseValueError(globalObject, scope, "unknown mode: '"_s, mode, '\'');
        JSValue buffer = call(globalObject, ioType(globalObject, bufferedType), raw, jsNumber(buffering));
        RETURN_IF_EXCEPTION(scope, { });
        result = buffer;
        if (binary)
            return result;

        auto orNone = [&](const String& string) -> JSValue { return string.isNull() ? jsUndefined() : JSValue(makeStr(vm, string)); };
        JSValue wrapper = call(globalObject, ioType(globalObject, IOType::TextIOWrapper), buffer, orNone(encoding), orNone(errors), orNone(newline), jsBoolean(lineBuffering));
        RETURN_IF_EXCEPTION(scope, { });
        result = wrapper;
        setAttr(globalObject, wrapper, ioName(globalObject, IOName::mode), makeStr(vm, mode));
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    };

    JSValue stream = finish();
    if (!failed(scope, stream))
        return stream;
    JSValue error = takePendingException(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue closed = callMethod(globalObject, result, rt.names.close());
    EXCEPTION_ASSERT_UNUSED(closed, !!scope.exception() == !closed);
    scope.release();
    ioChainExceptions(globalObject, error);
    return { };
}

static JSValue ioOpenCode(JSGlobalObject* globalObject, JSValue path)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(path))
        return raiseTypeError(globalObject, scope, "open_code() argument 'path' must be str, not "_s, isNone(path) ? "None"_str : typeName(globalObject, path));
    // This is where an interpreter has its say about the files that code is loaded from (PEP 578). Here it is that compiled
    // code which is damaged cannot be read, which importlib takes as it takes a .pyc file that is not there: it compiles the
    // source and writes the file anew. Left to find out for itself, it would give up on the module.
    String name = strValue(path)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (name.endsWith(".pyc"_s) && !name.contains('\0')) {
        PyBytes* contents = readPycFile(globalObject, name);
        if (!contents)
            return raiseOSError(globalObject, scope, errno, path);
        RELEASE_AND_RETURN(scope, call(globalObject, ioType(globalObject, IOType::BytesIO), contents));
    }
    RELEASE_AND_RETURN(scope, ioOpen(globalObject, path, makeStr(vm, "rb"_s), { }, { }, { }, { }, { }, { }));
}

static JSValue ioTextEncoding(JSGlobalObject* globalObject, JSValue encoding, JSValue stackLevel)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int level = 2;
    if (stackLevel && failed(scope, ioToInt(globalObject, stackLevel, level)))
        return { };
    if (!isNone(encoding))
        return encoding;
    if (configuration().warnDefaultEncoding && failed(scope, warn(globalObject, runtime(globalObject).EncodingWarning(), "'encoding' argument not specified"_s, level)))
        return { };
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), configuration().utf8Mode ? "utf-8"_s : "locale"_s));
}

// ---- The standard streams ----

JSValue makeStandardStream(JSGlobalObject* globalObject, int fd, bool writable, ASCIILiteral name, ASCIILiteral errors)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (fcntl(fd, F_GETFD) < 0 || (fd >= 0 && fd < 3 && configuration().standardStreamWasClosed[fd]))
        return jsUndefined();

    // -u: what is written goes straight to the descriptor.
    bool isBuffered = configuration().bufferedStdio || !writable;
    JSValue buffer = ioOpen(globalObject, jsNumber(fd), makeStr(vm, writable ? "wb"_s : "rb"_s), jsNumber(isBuffered ? -1 : 0), { }, { }, { }, jsBoolean(false), { });
    RETURN_IF_EXCEPTION(scope, { });
    JSValue raw = isBuffered ? getAttr(globalObject, buffer, ioName(globalObject, IOName::raw)) : buffer;
    RETURN_IF_EXCEPTION(scope, { });
    setAttr(globalObject, raw, rt.names.name(), makeStr(vm, name));
    RETURN_IF_EXCEPTION(scope, { });
    bool lineBuffering = configuration().bufferedStdio && (isatty(fd) || fd == STDERR_FILENO);

    JSValue stream = call(globalObject, ioType(globalObject, IOType::TextIOWrapper), buffer, makeStr(vm, "utf-8"_s), makeStr(vm, errors), makeStr(vm, "\n"_s), jsBoolean(lineBuffering), jsBoolean(!configuration().bufferedStdio));
    RETURN_IF_EXCEPTION(scope, { });
    setAttr(globalObject, stream, ioName(globalObject, IOName::mode), makeStr(vm, writable ? "w"_s : "r"_s));
    RETURN_IF_EXCEPTION(scope, { });
    rt.unregisterFinalizable(stream.asCell());
    if (writable) {
        listAppend(globalObject, ioStandardStreams(globalObject), stream);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, ioStandardStreams(globalObject), buffer);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return stream;
}

void flushAllOpenFiles(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    JSArray* streams = ioStandardStreams(globalObject);
    for (unsigned i = 0; i < listLength(streams); ++i) {
        // One that the program closed or detached says so.
        callMethod(globalObject, listGet(streams, i), rt.names.flush());
        if (!scope.tryClearException()) [[unlikely]]
            return;
    }
}

void flushStandardStreamsIfPending(JSGlobalObject* globalObject)
{
    JSArray* streams = ioStandardStreams(globalObject);
    for (unsigned i = 0; i + 1 < listLength(streams); i += 2) {
        PyRecord* buffer = ioTryExact(globalObject, listGet(streams, i + 1), IOType::BufferedWriter);
        if (textIOHasPendingOutput(PyRecord::from(listGet(streams, i))) || (buffer && bufferedHasPendingOutput(buffer))) {
            flushAllOpenFiles(globalObject);
            return;
        }
    }
}

void flushStandardStreams(VM& vm)
{
    for (Runtime* runtime : Vector<Runtime*, 1>(vm.heap.pythonRuntimes())) {
        if (runtime->threads().isMainThread()) {
            flushStandardStreamsIfPending(runtime->globalObject());
            writeOutPendingOutput(runtime->globalObject());
        }
    }
}

// ---- The module ----

static constexpr NativeDef ioFunctions[] = {
    PY_FUNCTION("open", ioOpen, "(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, closefd=True, opener=None)"),
    PY_FUNCTION("open_code", ioOpenCode, "(path)"),
    PY_FUNCTION("text_encoding", ioTextEncoding, "(encoding, stacklevel=2, /)"),
};

void initializeIOModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, ioFunctions);
    module->add(globalObject, "DEFAULT_BUFFER_SIZE"_s, jsNumber(static_cast<int32_t>(ioDefaultBufferSize)));
    module->add(globalObject, "BlockingIOError"_s, rt.BlockingIOError());
    module->add(globalObject, "UnsupportedOperation"_s, ioType(globalObject, IOType::UnsupportedOperation));
    module->add(globalObject, "_IOBase"_s, ioType(globalObject, IOType::IOBase));
    module->add(globalObject, "_RawIOBase"_s, ioType(globalObject, IOType::RawIOBase));
    module->add(globalObject, "_BufferedIOBase"_s, ioType(globalObject, IOType::BufferedIOBase));
    module->add(globalObject, "_TextIOBase"_s, ioType(globalObject, IOType::TextIOBase));
    module->add(globalObject, "_BytesIOBuffer"_s, ioType(globalObject, IOType::BytesIOBuffer));
    module->add(globalObject, "FileIO"_s, ioType(globalObject, IOType::FileIO));
    module->add(globalObject, "BytesIO"_s, ioType(globalObject, IOType::BytesIO));
    module->add(globalObject, "StringIO"_s, ioType(globalObject, IOType::StringIO));
    module->add(globalObject, "BufferedReader"_s, ioType(globalObject, IOType::BufferedReader));
    module->add(globalObject, "BufferedWriter"_s, ioType(globalObject, IOType::BufferedWriter));
    module->add(globalObject, "BufferedRWPair"_s, ioType(globalObject, IOType::BufferedRWPair));
    module->add(globalObject, "BufferedRandom"_s, ioType(globalObject, IOType::BufferedRandom));
    module->add(globalObject, "IncrementalNewlineDecoder"_s, ioType(globalObject, IOType::IncrementalNewlineDecoder));
    module->add(globalObject, "TextIOWrapper"_s, ioType(globalObject, IOType::TextIOWrapper));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
