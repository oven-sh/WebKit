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
#include "PythonBytes.h"

#include "JSArrayBuffer.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "PythonBuiltins.h"
#include "TopExceptionScope.h"
#include <wtf/text/StringBuilder.h>

// bytes, bytearray and memoryview.

namespace JSC { namespace Python {

// ---- What they are

BytesKind bytesKindOf(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return BytesKind::None;
    JSObject* object = asObject(value);
    JSValue prototype = object->structure()->storedPrototype(object);
    if (isType(prototype))
        return asType(prototype)->hasFlag(PyType::IsBytes) ? BytesKind::Bytes : BytesKind::ByteArray;
    // One of JavaScript's, unless it is an instance of a class of JavaScript's that is derived from one of Python's.
    if (!object->structure()->typeInfo().overloadsOperators()) [[likely]]
        return BytesKind::ByteArray;
    return typeOf(object->globalObject(), value)->hasFlag(PyType::IsBytes) ? BytesKind::Bytes : BytesKind::ByteArray;
}

static JSUint8Array* asView(JSValue value) { return uncheckedDowncast<JSUint8Array>(value.asCell()); }

static std::span<const uint8_t> spanOf(JSUint8Array* view)
{
    return view->isDetached() ? std::span<const uint8_t>() : std::span<const uint8_t>(view->typedSpan());
}

static std::span<uint8_t> mutableSpanOf(JSUint8Array* view)
{
    return view->isDetached() ? std::span<uint8_t>() : view->typedSpan();
}

static JSUint8Array* newView(JSGlobalObject* globalObject, Structure* structure, std::span<const uint8_t> content)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSUint8Array* view = JSUint8Array::createUninitialized(globalObject, structure, content.size());
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!content.empty())
        memcpy(view->typedVector(), content.data(), content.size());
    return view;
}

JSUint8Array* newBytes(JSGlobalObject* globalObject, std::span<const uint8_t> content)
{
    return newView(globalObject, globalObject->pyRealm()->structureFor(BuiltinType::Bytes), content);
}

JSUint8Array* newByteArray(JSGlobalObject* globalObject, std::span<const uint8_t> content)
{
    return newView(globalObject, globalObject->pyRealm()->structureFor(BuiltinType::ByteArray), content);
}

// A bytes from a method of bytes, and a bytearray from one of bytearray.
static JSValue newLike(JSGlobalObject* globalObject, JSValue self, std::span<const uint8_t> content)
{
    return isBytes(self) ? newBytes(globalObject, content) : newByteArray(globalObject, content);
}

std::optional<std::span<const uint8_t>> builtinBufferOf(JSValue value)
{
    if (!value || !value.isCell())
        return std::nullopt;
    JSCell* cell = value.asCell();
    if (auto* view = dynamicDowncast<JSArrayBufferView>(cell)) {
        if (view->isDetached())
            return std::span<const uint8_t>();
        return std::span<const uint8_t>(view->span());
    }
    if (auto* buffer = dynamicDowncast<JSArrayBuffer>(cell))
        return std::span<const uint8_t>(buffer->impl()->span());
    if (auto* memory = dynamicDowncast<PyMemoryView>(cell))
        return memory->contiguousSpan();
    return std::nullopt;
}

// The __buffer__() or __release_buffer__() that a program has given a class. Empty if it has none, or has only what a built-in class gives it.
static JSValue bufferMethodOfProgram(JSGlobalObject* globalObject, JSValue value, const Identifier& name)
{
    if (!value.isObject())
        return { };
    PyType* type = typeOf(globalObject, value);
    if (!type->hasFlag(PyType::IsHeapType))
        return { };
    JSValue method = type->lookup(globalObject->vm(), name);
    return method && !dynamicDowncast<PyNativeFunction>(method) ? method : JSValue();
}

bool hasBuffer(JSGlobalObject* globalObject, JSValue value)
{
    return builtinBufferOf(value) || dynamicDowncast<PyMemoryView>(value) || bufferMethodOfProgram(globalObject, value, globalObject->vm().pythonNames().dunder_buffer);
}

static PyMemoryView* memoryViewOf(JSGlobalObject*, JSValue, int flags);

Buffer tryBufferOf(JSGlobalObject* globalObject, JSValue value, int flags)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Buffer buffer;
    if (bufferMethodOfProgram(globalObject, value, vm.pythonNames().dunder_buffer)) [[unlikely]] {
        PyMemoryView* view = memoryViewOf(globalObject, value, flags);
        RETURN_IF_EXCEPTION(scope, { });
        buffer.m_view = view;
        buffer.m_globalObject = globalObject;
        value = view;
    }

    if (auto* memory = dynamicDowncast<PyMemoryView>(value)) {
        if (memory->isReleased()) {
            raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
            return { };
        }
        if ((flags & WritableBuffer) && memory->isReadOnly()) {
            raise(globalObject, scope, BuiltinType::BufferError, "memoryview: underlying buffer is not writable"_s);
            return { };
        }
        if (!memory->isContiguous() && (flags & StridedBuffer) != StridedBuffer) {
            raise(globalObject, scope, BuiltinType::BufferError, "memoryview: underlying buffer is not C-contiguous"_s);
            return { };
        }
        buffer.m_object = value;
        return buffer;
    }
    if (!builtinBufferOf(value))
        return { };
    if ((flags & WritableBuffer) && isBytes(value)) {
        raise(globalObject, scope, BuiltinType::BufferError, "Object is not writable."_s);
        return { };
    }
    buffer.m_object = value;
    return buffer;
}

// exporter.__release_buffer__(view). What it raises goes nowhere, and what has been raised already is still raised afterwards.
void releaseBufferOfProgram(JSGlobalObject* globalObject, JSValue exporter, JSValue view)
{
    VM& vm = globalObject->vm();
    JSValue method = bufferMethodOfProgram(globalObject, exporter, vm.pythonNames().dunder_release_buffer);
    if (!method)
        return;
    Exception* raised;
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        raised = scope.exception();
        scope.clearException();
        call(globalObject, method, exporter, view);
        // FIXME: It is for sys.unraisablehook to be told, when there is one.
        scope.clearException();
    }
    if (raised) {
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwException(globalObject, scope, raised);
    }
}

void Buffer::release()
{
    if (auto* view = std::exchange(m_view, nullptr))
        view->release(m_globalObject);
}

Buffers::~Buffers()
{
    for (size_t i = 0; i < m_views.size(); ++i)
        uncheckedDowncast<PyMemoryView>(m_views.at(i).asCell())->release(m_globalObject);
}

JSValue Buffer::object() const
{
    return m_view ? m_view->exporter() : m_object;
}

void Buffer::appendTo(ByteVector& result) const
{
    auto* memory = dynamicDowncast<PyMemoryView>(m_object);
    if (!memory || memory->isContiguous()) {
        result.append(span());
        return;
    }
    for (int64_t i = 0; i < memory->length(); ++i)
        result.append(memory->item(i));
}

Buffer bufferOf(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Buffer buffer = tryBufferOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!buffer)
        raiseTypeError(globalObject, scope, makeString("a bytes-like object is required, not '"_s, typeName(globalObject, value), '\''));
    return buffer;
}

String reprOfBytes(std::span<const uint8_t> content)
{
    bool hasSingle = false;
    bool hasDouble = false;
    for (uint8_t byte : content) {
        hasSingle |= byte == '\'';
        hasDouble |= byte == '"';
    }
    char quote = hasSingle && !hasDouble ? '"' : '\'';
    StringBuilder builder;
    builder.append('b', quote);
    for (uint8_t byte : content) {
        if (byte == quote || byte == '\\')
            builder.append('\\', static_cast<char>(byte));
        else if (byte == '\t')
            builder.append("\\t"_s);
        else if (byte == '\n')
            builder.append("\\n"_s);
        else if (byte == '\r')
            builder.append("\\r"_s);
        else if (byte < ' ' || byte >= 0x7F)
            builder.append("\\x"_s, hex(byte, 2, Lowercase));
        else
            builder.append(static_cast<char>(byte));
    }
    builder.append(quote);
    return builder.toString();
}

// The same as of a str with the same characters, as it is in CPython.
int64_t hashOfBytes(std::span<const uint8_t> content)
{
    return hashOfString(String(byteCast<Latin1Character>(content)));
}

// ---- Changing the length of a bytearray

// False if it raised.
static bool resize(JSGlobalObject* globalObject, JSUint8Array* view, size_t newLength)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    size_t length = view->length();
    if (newLength == length)
        return true;
    if (!view->ownsStorage()) {
        // JavaScript has its ArrayBuffer, which is not to change under it.
        raise(globalObject, scope, BuiltinType::BufferError, "Existing exports of data: object cannot be re-sized"_s);
        return false;
    }
    // How much room there is is not something that a Uint8Array knows. One that has never been resized has none to spare.
    auto& capacityName = vm.pythonNames().private_capacity;
    JSValue known = view->getDirect(vm, capacityName);
    size_t capacity = known ? static_cast<size_t>(known.asNumber()) : length;

    if (newLength <= capacity && newLength >= capacity / 2) {
        if (newLength > length)
            memset(view->typedVector() + length, 0, newLength - length);
        view->setLengthWithinOwnedStorage(newLength);
        return true;
    }
    // As CPython's bytearray does: an eighth more than was asked for if it is growing a little, and no more if it is growing a lot.
    size_t newCapacity = newLength;
    if (newLength > capacity && newLength <= capacity + capacity / 8)
        newCapacity = newLength + (newLength >> 3) + (newLength < 9 ? 3 : 6);
    if (!view->reallocateOwnedStorage(vm, newLength, newLength, newCapacity)) {
        raise(globalObject, scope, BuiltinType::MemoryError, JSValue());
        return false;
    }
    view->putDirect(vm, capacityName, jsNumber(static_cast<double>(newCapacity)));
    return true;
}

// Puts `replacement` in place of `count` bytes at `start`. False if it raised.
static bool replaceRange(JSGlobalObject* globalObject, JSUint8Array* view, size_t start, size_t count, std::span<const uint8_t> replacement)
{
    // It may be part of the view itself.
    ByteVector copy;
    copy.append(replacement);
    size_t length = view->length();
    size_t tail = length - start - count;
    if (copy.size() > count) {
        if (!resize(globalObject, view, length + copy.size() - count))
            return false;
        uint8_t* data = view->typedVector();
        memmove(data + start + copy.size(), data + start + count, tail);
    } else if (copy.size() < count) {
        if (!view->ownsStorage())
            return resize(globalObject, view, length - 1);
        uint8_t* data = view->typedVector();
        memmove(data + start + copy.size(), data + start + count, tail);
        if (!resize(globalObject, view, length - (count - copy.size())))
            return false;
    }
    if (!copy.isEmpty())
        memcpy(view->typedVector() + start, copy.span().data(), copy.size());
    return true;
}

// ---- Arguments

#define BYTES_PROLOGUE(method) \
    NATIVE_PROLOGUE(); \
    if (bytesKindOf(args.at(0)) == BytesKind::None) \
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("descriptor '"_s, method ""_s, "' requires a bytes-like object but received a '"_s, typeName(globalObject, args.at(0) ? args.at(0) : jsUndefined()), '\''))); \
    JSValue selfValue = args[0]; \
    [[maybe_unused]] JSUint8Array* self = asView(selfValue); \
    [[maybe_unused]] Buffer content { selfValue };

// One byte, from an int. Nothing if it raised.
static std::optional<uint8_t> byteFrom(JSGlobalObject* globalObject, JSValue value, ASCIILiteral rangeMessage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto index = toIndex(globalObject, value, true);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*index < 0 || *index > 255) {
        raiseValueError(globalObject, scope, rangeMessage);
        return std::nullopt;
    }
    return static_cast<uint8_t>(*index);
}

// What find(), count(), `in` and the like look for: some bytes, or one given as an int. False if it raised.
static bool needleFrom(JSGlobalObject* globalObject, JSValue value, ByteVector& needle)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto buffer = tryBufferOf(globalObject, value)) {
        needle.append(*buffer);
        return true;
    }
    RETURN_IF_EXCEPTION(scope, false);
    if (!classify(value).isInt() && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index)) {
        raiseTypeError(globalObject, scope, makeString("argument should be integer or bytes-like object, not '"_s, typeName(globalObject, value), '\''));
        return false;
    }
    auto byte = byteFrom(globalObject, value, "byte must be in range(0, 256)"_s);
    RETURN_IF_EXCEPTION(scope, false);
    needle.append(*byte);
    return true;
}

// Optional start and end arguments. They are looked at first, which can run a program's code, and what they select is worked out last, when it is known how much
// there is to select from.
struct RangeArguments {
    int64_t start { 0 };
    int64_t end { std::numeric_limits<int64_t>::max() };

    // False if it raised.
    bool parse(JSGlobalObject* globalObject, JSValue startValue, JSValue endValue)
    {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        for (auto [value, bound] : { std::pair { startValue, &start }, std::pair { endValue, &end } }) {
            if (!value || isNone(value))
                continue;
            if (!classify(value).isInt() && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index)) {
                raiseTypeError(globalObject, scope, "slice indices must be integers or None or have an __index__ method"_s);
                return false;
            }
            auto index = toIndex(globalObject, value, true);
            RETURN_IF_EXCEPTION(scope, false);
            *bound = *index;
        }
        return true;
    }

    // Nothing if it starts beyond the end, which is not the same as selecting nothing.
    std::optional<std::span<const uint8_t>> select(std::span<const uint8_t> content, size_t& offset) const
    {
        int64_t size = content.size();
        int64_t last = end > size ? size : end < 0 ? std::max<int64_t>(end + size, 0) : end;
        int64_t first = start < 0 ? std::max<int64_t>(start + size, 0) : start;
        if (first > size)
            return std::nullopt;
        offset = first;
        return content.subspan(first, std::max(last, first) - first);
    }
};

static size_t findIn(std::span<const uint8_t> haystack, std::span<const uint8_t> needle, size_t from = 0)
{
    if (needle.size() > haystack.size())
        return notFound;
    for (size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        if (!needle.size() || !memcmp(haystack.data() + i, needle.data(), needle.size()))
            return i;
    }
    return notFound;
}

static size_t reverseFindIn(std::span<const uint8_t> haystack, std::span<const uint8_t> needle)
{
    if (needle.size() > haystack.size())
        return notFound;
    for (size_t i = haystack.size() - needle.size() + 1; i--;) {
        if (!needle.size() || !memcmp(haystack.data() + i, needle.data(), needle.size()))
            return i;
    }
    return notFound;
}

// ---- Making them

// What bytes(source, encoding, errors) and bytearray(...) hold. False if it raised.
static bool contentFrom(JSGlobalObject* globalObject, const NativeArguments& args, bool isByteArray, ByteVector& content)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASCIILiteral typeText = isByteArray ? "bytearray"_s : "bytes"_s;
    if (args.size() > 4) {
        raiseTypeError(globalObject, scope, makeString(typeText, "() takes at most 3 arguments ("_s, args.size() - 1, " given)"_s));
        return false;
    }
    JSValue source = args.at(1);
    JSValue encodingValue = args.at(2);
    JSValue errorsValue = args.at(3);
    auto textOf = [&] (JSValue value, ASCIILiteral name, unsigned position) -> String {
        if (!value)
            return { };
        if (!value.isString()) {
            raiseTypeError(globalObject, scope, makeString(typeText, "() argument "_s, position, " must be str, not "_s, typeName(globalObject, value)));
            UNUSED_PARAM(name);
            return { };
        }
        return asString(value)->value(globalObject);
    };
    String encoding = textOf(encodingValue, "encoding"_s, 2);
    RETURN_IF_EXCEPTION(scope, false);
    String errors = textOf(errorsValue, "errors"_s, 3);
    RETURN_IF_EXCEPTION(scope, false);

    if (!source) {
        if (encodingValue || errorsValue) {
            raiseTypeError(globalObject, scope, encodingValue ? "encoding without a string argument"_s : "errors without a string argument"_s);
            return false;
        }
        return true;
    }
    JSValue plain = source;
    if (auto* boxed = tryBoxedValue(plain))
        plain = boxed->value();
    if (plain.isString()) {
        if (!encodingValue) {
            raiseTypeError(globalObject, scope, "string argument without an encoding"_s);
            return false;
        }
        auto encoded = encodeString(globalObject, plain, encoding, errors);
        RETURN_IF_EXCEPTION(scope, false);
        content = WTF::move(*encoded);
        return true;
    }
    if (encodingValue || errorsValue) {
        raiseTypeError(globalObject, scope, encodingValue ? "encoding without a string argument"_s : "errors without a string argument"_s);
        return false;
    }
    if (!isByteArray && source.isObject()) {
        JSValue self;
        JSValue method = lookupSpecial(globalObject, source, vm.pythonNames().dunder_bytes, self);
        RETURN_IF_EXCEPTION(scope, false);
        if (method) {
            JSValue result = callMethod(globalObject, method, self);
            RETURN_IF_EXCEPTION(scope, false);
            if (!isBytes(result)) {
                raiseTypeError(globalObject, scope, makeString("__bytes__ returned non-bytes (type "_s, typeName(globalObject, result), ')'));
                return false;
            }
            content.append(spanOf(asView(result)));
            return true;
        }
    }
    if (auto buffer = tryBufferOf(globalObject, source, FullReadOnlyBuffer)) {
        buffer.appendTo(content);
        return true;
    }
    RETURN_IF_EXCEPTION(scope, false);
    // So many zeros.
    if (classify(source).isInt() || typeOf(globalObject, source)->lookup(vm, vm.pythonNames().dunder_index)) {
        auto count = toIndex(globalObject, source);
        RETURN_IF_EXCEPTION(scope, false);
        if (*count < 0) {
            raiseValueError(globalObject, scope, "negative count"_s);
            return false;
        }
        content.grow(*count);
        memset(content.mutableSpan().data(), 0, *count);
        return true;
    }
    if (!typeOf(globalObject, source)->lookup(vm, vm.pythonNames().dunder_iter) && !typeOf(globalObject, source)->lookup(vm, vm.pythonNames().dunder_getitem)) {
        raiseTypeError(globalObject, scope, makeString("cannot convert '"_s, typeName(globalObject, source), "' object to "_s, typeText));
        return false;
    }
    forEach(globalObject, source, [&] (JSValue item) {
        auto byte = byteFrom(globalObject, item, isByteArray ? "byte must be in range(0, 256)"_s : "bytes must be in range(0, 256)"_s);
        if (!byte)
            return false;
        content.append(*byte);
        return true;
    });
    return !scope.exception();
}

PYTHON_NATIVE(bytesNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args.at(0));
    // There is no need for another of what cannot change.
    if (type == realm->typeBytes() && args.size() == 2 && !args.keywordCount() && typeOf(globalObject, args[1]) == type)
        return JSValue::encode(args[1]);
    ByteVector content;
    contentFrom(globalObject, args, false, content);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newView(globalObject, type->instanceStructure(), content.span())));
}

PYTHON_NATIVE(byteArrayNew)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(newView(globalObject, asType(args.at(0))->instanceStructure(), { })));
}

PYTHON_NATIVE(byteArrayInit)
{
    BYTES_PROLOGUE("__init__");
    ByteVector newContent;
    contentFrom(globalObject, args, true, newContent);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    replaceRange(globalObject, self, 0, self->length(), newContent.span());
    RETURN_NONE();
}

static int hexDigit(char16_t c)
{
    return isASCIIHexDigit(c) ? toASCIIHexValue(c) : -1;
}

// bytes.fromhex('2ef0 f1')
PYTHON_NATIVE(bytesFromHex)
{
    NATIVE_PROLOGUE();
    String text;
    if (args[1].isString())
        text = asString(args[1])->value(globalObject);
    else if (auto buffer = tryBufferOf(globalObject, args[1]))
        text = String(byteCast<Latin1Character>(*buffer));
    else {
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("fromhex() argument must be str or bytes-like, not "_s, typeName(globalObject, args[1]))));
    }
    ByteVector content;
    unsigned length = text.length();
    for (unsigned i = 0; i < length;) {
        if (isUnicodeCompatibleASCIIWhitespace(text[i])) {
            ++i;
            continue;
        }
        int high = hexDigit(text[i]);
        if (high < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, makeString("non-hexadecimal number found in fromhex() arg at position "_s, i)));
        if (i + 1 >= length)
            return JSValue::encode(raiseValueError(globalObject, scope, "fromhex() arg must contain an even number of hexadecimal digits"_s));
        int low = hexDigit(text[i + 1]);
        if (low < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, makeString("non-hexadecimal number found in fromhex() arg at position "_s, i + 1)));
        content.append(static_cast<uint8_t>(high << 4 | low));
        i += 2;
    }
    PyType* type = asType(args[0]);
    JSValue result = type->hasFlag(PyType::IsBytes) ? newBytes(globalObject, content.span()) : newByteArray(globalObject, content.span());
    RETURN_IF_EXCEPTION(scope, { });
    if (type == realm->typeBytes() || type == realm->typeByteArray())
        return JSValue::encode(result);
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, type, result)));
}

// ---- Special methods

PYTHON_NATIVE(bytesRepr)
{
    BYTES_PROLOGUE("__repr__");
    String text = reprOfBytes(content);
    if (isBytes(selfValue))
        return JSValue::encode(jsString(vm, text));
    return JSValue::encode(jsString(vm, makeString(typeName(globalObject, selfValue), '(', text, ')')));
}

PYTHON_NATIVE(bytesHash)
{
    BYTES_PROLOGUE("__hash__");
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, hashOfBytes(content))));
}

PYTHON_NATIVE(bytesLen)
{
    BYTES_PROLOGUE("__len__");
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, content.size())));
}

PYTHON_NATIVE(bytesBytes)
{
    BYTES_PROLOGUE("__bytes__");
    if (typeOf(globalObject, selfValue) == realm->typeBytes())
        return JSValue::encode(selfValue);
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, content)));
}

PYTHON_NATIVE(bytesIter)
{
    BYTES_PROLOGUE("__iter__");
    return JSValue::encode(PyIterator::create(globalObject, typeOf(globalObject, selfValue)->hasFlag(PyType::IsBytes) ? PyIterator::Kind::Bytes : PyIterator::Kind::ByteArray, selfValue));
}

PYTHON_NATIVE(bytesGetItem)
{
    BYTES_PROLOGUE("__getitem__");
    ASCIILiteral typeText = isBytes(selfValue) ? "byte"_s : "bytearray"_s;
    if (auto* slice = trySlice(args[1])) {
        auto indices = slice->indices(globalObject, [&] { return content.size(); });
        RETURN_IF_EXCEPTION(scope, { });
        if (indices->step == 1)
            RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, content.subspan(indices->start, indices->length))));
        ByteVector selected;
        for (int64_t i = 0, at = indices->start; i < indices->length; ++i, at += indices->step)
            selected.append(content[at]);
        RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, selected.span())));
    }
    if (!classify(args[1]).isInt() && !typeOf(globalObject, args[1])->lookup(vm, names.dunder_index))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeText, " indices must be integers or slices, not "_s, typeName(globalObject, args[1]))));
    auto index = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t at = *index < 0 ? *index + static_cast<int64_t>(content.size()) : *index;
    if (at < 0 || at >= static_cast<int64_t>(content.size()))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, isBytes(selfValue) ? "index out of range"_s : "bytearray index out of range"_s));
    return JSValue::encode(jsNumber(content[at]));
}

PYTHON_NATIVE(bytesContains)
{
    BYTES_PROLOGUE("__contains__");
    ByteVector needle;
    if (auto buffer = tryBufferOf(globalObject, args[1]))
        needle.append(*buffer);
    else {
        RETURN_IF_EXCEPTION(scope, { });
        if (!classify(args[1]).isInt() && !typeOf(globalObject, args[1])->lookup(vm, names.dunder_index))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("a bytes-like object is required, not '"_s, typeName(globalObject, args[1]), '\'')));
        auto byte = byteFrom(globalObject, args[1], "byte must be in range(0, 256)"_s);
        RETURN_IF_EXCEPTION(scope, { });
        needle.append(*byte);
    }
    return JSValue::encode(jsBoolean(findIn(spanOf(self), needle.span()) != notFound));
}

PYTHON_NATIVE(bytesCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    BYTES_PROLOGUE("__eq__");
    // A bytearray is compared with anything that has bytes to show, and bytes with bytes alone.
    if (typeOf(globalObject, selfValue)->hasFlag(PyType::IsBytes) && !typeOf(globalObject, args[1])->hasFlag(PyType::IsBytes))
        RETURN_NOT_IMPLEMENTED();
    auto other = tryBufferOf(globalObject, args[1]);
    // What cannot be got at is something that this cannot be compared with.
    if (scope.exception())
        catchException(globalObject, BuiltinType::BaseException);
    if (!other)
        RETURN_NOT_IMPLEMENTED();
    size_t common = std::min(content.size(), other->size());
    int order = common ? memcmp(content.data(), other->data(), common) : 0;
    if (!order)
        order = content.size() == other->size() ? 0 : content.size() < other->size() ? -1 : 1;
    bool result = false;
    switch (op) {
    case ComparisonOperator::Eq:
        result = !order;
        break;
    case ComparisonOperator::NotEq:
        result = order;
        break;
    case ComparisonOperator::Lt:
        result = order < 0;
        break;
    case ComparisonOperator::LtE:
        result = order <= 0;
        break;
    case ComparisonOperator::Gt:
        result = order > 0;
        break;
    default:
        result = order >= 0;
        break;
    }
    return JSValue::encode(jsBoolean(result));
}

PYTHON_NATIVE(bytesAdd)
{
    BYTES_PROLOGUE("__add__");
    auto other = tryBufferOf(globalObject, args[1]);
    // Whatever is wrong with it, this is what is said.
    if (scope.exception())
        catchException(globalObject, BuiltinType::BaseException);
    if (!other)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("can't concat "_s, typeName(globalObject, args.at(1)), " to "_s, typeName(globalObject, selfValue))));
    ByteVector joined;
    joined.append(content.span());
    joined.append(*other);
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, joined.span())));
}

static bool repeated(JSGlobalObject* globalObject, ThrowScope& scope, const Buffer& content, JSValue countValue, ByteVector& result)
{
    auto count = toIndex(globalObject, countValue);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < *count; ++i)
        result.append(content.span());
    return true;
}

// b * n and n * b
PYTHON_NATIVE(bytesMultiply)
{
    BYTES_PROLOGUE("__mul__");
    if (!classify(args.at(1)).isInt() && !typeOf(globalObject, args.at(1))->lookup(vm, names.dunder_index))
        RETURN_NOT_IMPLEMENTED();
    ByteVector result;
    repeated(globalObject, scope, content, args[1], result);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

// ---- Searching

// find, rfind, index and rindex(sub, start, end)
PYTHON_NATIVE(bytesFind)
{
    bool fromRight = unpack<bool>(callFrame, 0);
    bool raises = unpack<bool>(callFrame, 1);
    BYTES_PROLOGUE("find");
    RangeArguments range;
    range.parse(globalObject, args.at(2), args.at(3));
    RETURN_IF_EXCEPTION(scope, { });
    ByteVector needle;
    needleFrom(globalObject, args[1], needle);
    RETURN_IF_EXCEPTION(scope, { });
    size_t start = 0;
    auto part = range.select(content, start);
    size_t found = !part ? notFound : fromRight ? reverseFindIn(*part, needle.span()) : findIn(*part, needle.span());
    if (found == notFound) {
        if (raises)
            return JSValue::encode(raiseValueError(globalObject, scope, "subsection not found"_s));
        return JSValue::encode(jsNumber(-1));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, start + found)));
}

PYTHON_NATIVE(bytesCount)
{
    BYTES_PROLOGUE("count");
    RangeArguments range;
    range.parse(globalObject, args.at(2), args.at(3));
    RETURN_IF_EXCEPTION(scope, { });
    ByteVector needle;
    needleFrom(globalObject, args[1], needle);
    RETURN_IF_EXCEPTION(scope, { });
    size_t start = 0;
    auto part = range.select(content, start);
    if (!part)
        return JSValue::encode(jsNumber(0));
    if (needle.isEmpty())
        RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, part->size() + 1)));
    int64_t count = 0;
    for (size_t at = findIn(*part, needle.span()); at != notFound; at = findIn(*part, needle.span(), at + needle.size()))
        ++count;
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, count)));
}

// startswith and endswith(prefix or a tuple of them, start, end)
PYTHON_NATIVE(bytesStartsOrEndsWith)
{
    bool atStart = unpack<bool>(callFrame, 0);
    BYTES_PROLOGUE("startswith");
    ASCIILiteral method = atStart ? "startswith"_s : "endswith"_s;
    RangeArguments range;
    range.parse(globalObject, args.at(2), args.at(3));
    RETURN_IF_EXCEPTION(scope, { });
    auto matches = [&] (std::span<const uint8_t> affix) {
        size_t start = 0;
        auto part = range.select(content, start);
        if (!part || affix.size() > part->size())
            return false;
        return !affix.size() || !memcmp(atStart ? part->data() : part->data() + part->size() - affix.size(), affix.data(), affix.size());
    };
    if (isTuple(args[1])) {
        for (auto& entry : uncheckedDowncast<PyTuple>(args[1].asCell())->span()) {
            auto affix = bufferOf(globalObject, entry.get());
            RETURN_IF_EXCEPTION(scope, { });
            if (matches(*affix))
                return JSValue::encode(jsBoolean(true));
        }
        return JSValue::encode(jsBoolean(false));
    }
    auto affix = tryBufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!affix)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(method, " first arg must be bytes or a tuple of bytes, not "_s, typeName(globalObject, args[1]))));
    return JSValue::encode(jsBoolean(matches(*affix)));
}

// replace(old, new, count=-1)
PYTHON_NATIVE(bytesReplace)
{
    BYTES_PROLOGUE("replace");
    if (args.size() < 3 || args.size() > 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("replace expected at least 2 arguments, got "_s, args.size() - 1)));
    auto from = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto to = bufferOf(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue countValue = args.at(3);
    int64_t limit = -1;
    if (countValue) {
        auto index = toIndex(globalObject, countValue, true);
        RETURN_IF_EXCEPTION(scope, { });
        limit = *index;
    }
    if (limit < 0)
        limit = std::numeric_limits<int64_t>::max();
    ByteVector result;
    if (from->empty()) {
        int64_t done = 0;
        for (uint8_t byte : content) {
            if (done++ < limit)
                result.append(*to);
            result.append(byte);
        }
        if (done < limit)
            result.append(*to);
    } else {
        size_t start = 0;
        for (int64_t done = 0; done < limit; ++done) {
            size_t found = findIn(content, *from, start);
            if (found == notFound)
                break;
            result.append(content.subspan(start, found - start));
            result.append(*to);
            start = found + from->size();
        }
        result.append(content.subspan(start));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

// ---- Splitting and joining

static bool isByteSpace(uint8_t byte) { return byte == ' ' || (byte >= '\t' && byte <= '\r'); }

PYTHON_NATIVE(bytesJoin)
{
    BYTES_PROLOGUE("join");
    MarkedArgumentBuffer items;
    collect(globalObject, args[1], items);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, "can only join an iterable"_s);
        return { };
    }
    Buffers buffers(globalObject);
    for (unsigned i = 0; i < items.size(); ++i) {
        auto item = tryBufferOf(globalObject, items.at(i));
        // Whatever is wrong with it, this is what is said.
        if (scope.exception())
            catchException(globalObject, BuiltinType::BaseException);
        if (!item)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sequence item "_s, i, ": expected a bytes-like object, "_s, typeName(globalObject, items.at(i)), " found"_s)));
        buffers.append(WTF::move(item));
    }
    ByteVector result;
    for (unsigned i = 0; i < buffers.size(); ++i) {
        if (i)
            result.append(content.span());
        result.append(buffers.at(i));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

// split and rsplit(sep=None, maxsplit=-1)
PYTHON_NATIVE(bytesSplit)
{
    bool fromRight = unpack<bool>(callFrame, 0);
    BYTES_PROLOGUE("split");
    JSValue separatorValue = args.at(1);
    JSValue limitValue = args.at(2);
    int64_t limit = -1;
    if (limitValue) {
        auto index = toIndex(globalObject, limitValue, true);
        RETURN_IF_EXCEPTION(scope, { });
        limit = *index;
    }
    if (limit < 0)
        limit = std::numeric_limits<int64_t>::max();

    Buffer separator;
    bool splitsAtBlanks = !separatorValue || isNone(separatorValue);
    if (!splitsAtBlanks) {
        separator = bufferOf(globalObject, separatorValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (separator->empty())
            return JSValue::encode(raiseValueError(globalObject, scope, "empty separator"_s));
    }

    size_t length = content.size();
    Vector<std::pair<size_t, size_t>, 16> pieces;
    auto count = [&] { return static_cast<int64_t>(pieces.size()); };
    if (splitsAtBlanks) {
        if (!fromRight) {
            size_t i = 0;
            while (true) {
                while (i < length && isByteSpace(content[i]))
                    ++i;
                if (i >= length)
                    break;
                if (count() == limit) {
                    pieces.append({ i, length });
                    break;
                }
                size_t start = i;
                while (i < length && !isByteSpace(content[i]))
                    ++i;
                pieces.append({ start, i });
            }
        } else {
            size_t i = length;
            while (true) {
                while (i && isByteSpace(content[i - 1]))
                    --i;
                if (!i)
                    break;
                if (count() == limit) {
                    pieces.append({ 0, i });
                    break;
                }
                size_t end = i;
                while (i && !isByteSpace(content[i - 1]))
                    --i;
                pieces.append({ i, end });
            }
            pieces.reverse();
        }
    } else {
        if (!fromRight) {
            size_t start = 0;
            while (count() < limit) {
                size_t found = findIn(content, *separator, start);
                if (found == notFound)
                    break;
                pieces.append({ start, found });
                start = found + separator->size();
            }
            pieces.append({ start, length });
        } else {
            size_t end = length;
            while (count() < limit) {
                size_t found = reverseFindIn(content.first(end), *separator);
                if (found == notFound)
                    break;
                pieces.append({ found + separator->size(), end });
                end = found;
            }
            pieces.append({ 0, end });
            pieces.reverse();
        }
    }
    MarkedArgumentBuffer result;
    for (auto [start, end] : pieces) {
        result.append(newLike(globalObject, selfValue, spanOf(self).subspan(start, end - start)));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, result)));
}

PYTHON_NATIVE(bytesSplitLines)
{
    BYTES_PROLOGUE("splitlines");
    JSValue keepValue = args.at(1);
    bool keepEnds = keepValue && isTrue(globalObject, keepValue);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer result;
    size_t length = content.size();
    for (size_t i = 0; i < length;) {
        size_t start = i;
        while (i < length && content[i] != '\n' && content[i] != '\r')
            ++i;
        size_t end = i;
        if (i < length) {
            if (content[i] == '\r' && i + 1 < length && content[i + 1] == '\n')
                ++i;
            ++i;
        }
        result.append(newLike(globalObject, selfValue, content.subspan(start, (keepEnds ? i : end) - start)));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, result)));
}

PYTHON_NATIVE(bytesPartition)
{
    bool fromRight = unpack<bool>(callFrame, 0);
    BYTES_PROLOGUE("partition");
    // A bytearray makes a bytearray of it, as bytearray() would.
    auto separator = tryBufferOf(globalObject, args[1], isBytes(selfValue) ? SimpleBuffer : FullReadOnlyBuffer);
    RETURN_IF_EXCEPTION(scope, { });
    if (!separator)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("a bytes-like object is required, not '"_s, typeName(globalObject, args[1]), '\'')));
    if (separator->empty())
        return JSValue::encode(raiseValueError(globalObject, scope, "empty separator"_s));
    size_t found = fromRight ? reverseFindIn(content, *separator) : findIn(content, *separator);
    auto make = [&] (std::span<const uint8_t> part) { return newLike(globalObject, selfValue, part); };
    std::span<const uint8_t> nothing;
    if (found == notFound)
        return JSValue::encode(fromRight ? PyTuple::create(globalObject, { make(nothing), make(nothing), make(content) }) : PyTuple::create(globalObject, { make(content), make(nothing), make(nothing) }));
    return JSValue::encode(PyTuple::create(globalObject, { make(content.first(found)), isBytes(selfValue) ? separator.object() : make(*separator), make(content.subspan(found + separator->size())) }));
}

// ---- Trimming and padding

// strip, lstrip and rstrip(bytes=None)
PYTHON_NATIVE(bytesStrip)
{
    bool left = unpack<bool>(callFrame, 0);
    bool right = unpack<bool>(callFrame, 1);
    BYTES_PROLOGUE("strip");
    std::optional<std::span<const uint8_t>> set;
    if (args.size() > 1 && !isNone(args[1])) {
        set = bufferOf(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto shouldStrip = [&] (uint8_t byte) { return set ? std::ranges::find(*set, byte) != set->end() : isByteSpace(byte); };
    size_t start = 0;
    size_t end = content.size();
    if (left) {
        while (start < end && shouldStrip(content[start]))
            ++start;
    }
    if (right) {
        while (end > start && shouldStrip(content[end - 1]))
            --end;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, content.subspan(start, end - start))));
}

// ljust, rjust and center(width, fillbyte=b' ')
PYTHON_NATIVE(bytesJustify)
{
    char align = unpack<char>(callFrame, 0);
    BYTES_PROLOGUE("center");
    ASCIILiteral method = align == '<' ? "ljust"_s : align == '>' ? "rjust"_s : "center"_s;
    auto width = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    uint8_t fill = ' ';
    if (args.size() > 2) {
        auto given = bytesKindOf(args[2]) == BytesKind::None ? std::nullopt : builtinBufferOf(args[2]);
        if (!given || given->size() != 1) {
            if (given)
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString(method, "(): argument 2 must be a byte string of length 1, not a "_s, typeName(globalObject, args[2]), " object of length "_s, given->size())));
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString(method, "() argument 2 must be a byte string of length 1, not "_s, typeName(globalObject, args[2]))));
        }
        fill = (*given)[0];
    }
    int64_t length = content.size();
    int64_t padding = std::max<int64_t>(*width - length, 0);
    int64_t before = align == '<' ? 0 : align == '>' ? padding : padding / 2 + (padding & *width & 1);
    ByteVector result;
    result.appendUsingFunctor(before, [&] (size_t) { return fill; });
    result.append(content.span());
    result.appendUsingFunctor(padding - before, [&] (size_t) { return fill; });
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

PYTHON_NATIVE(bytesZfill)
{
    BYTES_PROLOGUE("zfill");
    auto width = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    ByteVector result;
    size_t start = 0;
    if (!content.empty() && (content[0] == '+' || content[0] == '-'))
        result.append(content[start++]);
    for (int64_t i = content.size(); i < *width; ++i)
        result.append('0');
    result.append(content.subspan(start));
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

PYTHON_NATIVE(bytesExpandTabs)
{
    BYTES_PROLOGUE("expandtabs");
    JSValue sizeValue = args.at(1);
    int64_t tabSize = 8;
    if (sizeValue) {
        auto index = toIndex(globalObject, sizeValue);
        RETURN_IF_EXCEPTION(scope, { });
        tabSize = *index;
    }
    ByteVector result;
    int64_t column = 0;
    for (uint8_t byte : content) {
        if (byte == '\t') {
            if (tabSize > 0) {
                int64_t spaces = tabSize - column % tabSize;
                result.appendUsingFunctor(spaces, [] (size_t) -> uint8_t { return ' '; });
                column += spaces;
            }
            continue;
        }
        result.append(byte);
        column = byte == '\n' || byte == '\r' ? 0 : column + 1;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

PYTHON_NATIVE(bytesRemoveAffix)
{
    bool isPrefix = unpack<bool>(callFrame, 0);
    BYTES_PROLOGUE("removeprefix");
    auto affix = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    std::span<const uint8_t> result = content;
    if (affix->size() <= content.size() && !affix->empty()) {
        if (isPrefix && !memcmp(content.data(), affix->data(), affix->size()))
            result = content.subspan(affix->size());
        else if (!isPrefix && !memcmp(content.data() + content.size() - affix->size(), affix->data(), affix->size()))
            result = content.first(content.size() - affix->size());
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result)));
}

// ---- Case, which is ASCII's

enum class CaseChange : uint8_t { Upper, Lower, Swap, Capitalize, Title };

PYTHON_NATIVE(bytesChangeCase)
{
    auto change = unpack<CaseChange>(callFrame, 0);
    BYTES_PROLOGUE("upper");
    ByteVector result;
    bool previousIsCased = false;
    for (size_t i = 0; i < content.size(); ++i) {
        uint8_t byte = content[i];
        switch (change) {
        case CaseChange::Upper:
            byte = toASCIIUpper(byte);
            break;
        case CaseChange::Lower:
            byte = toASCIILower(byte);
            break;
        case CaseChange::Swap:
            byte = isASCIIUpper(byte) ? toASCIILower(byte) : toASCIIUpper(byte);
            break;
        case CaseChange::Capitalize:
            byte = i ? toASCIILower(byte) : toASCIIUpper(byte);
            break;
        case CaseChange::Title:
            byte = previousIsCased ? toASCIILower(byte) : toASCIIUpper(byte);
            previousIsCased = isASCIIAlpha(byte);
            break;
        }
        result.append(byte);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

enum class ByteClass : uint8_t { Alpha, Digit, Alphanumeric, Space, ASCII };

// True if there is at least one byte and all of them are of the class. isascii() is true of none at all.
PYTHON_NATIVE(bytesAll)
{
    auto byteClass = unpack<ByteClass>(callFrame, 0);
    BYTES_PROLOGUE("isalpha");
    if (content.empty())
        return JSValue::encode(jsBoolean(byteClass == ByteClass::ASCII));
    for (uint8_t byte : content) {
        bool holds = byteClass == ByteClass::Alpha ? isASCIIAlpha(byte) : byteClass == ByteClass::Digit ? isASCIIDigit(byte) : byteClass == ByteClass::Alphanumeric ? isASCIIAlphanumeric(byte)
            : byteClass == ByteClass::Space ? isByteSpace(byte) : byte < 0x80;
        if (!holds)
            return JSValue::encode(jsBoolean(false));
    }
    return JSValue::encode(jsBoolean(true));
}

PYTHON_NATIVE(bytesIsCase)
{
    bool upper = unpack<bool>(callFrame, 0);
    BYTES_PROLOGUE("isupper");
    bool hasCased = false;
    for (uint8_t byte : content) {
        if (upper ? isASCIILower(byte) : isASCIIUpper(byte))
            return JSValue::encode(jsBoolean(false));
        hasCased |= isASCIIAlpha(byte);
    }
    return JSValue::encode(jsBoolean(hasCased));
}

PYTHON_NATIVE(bytesIsTitle)
{
    BYTES_PROLOGUE("istitle");
    bool hasCased = false;
    bool previousIsCased = false;
    for (uint8_t byte : content) {
        if (isASCIIUpper(byte)) {
            if (previousIsCased)
                return JSValue::encode(jsBoolean(false));
            previousIsCased = true;
            hasCased = true;
        } else if (isASCIILower(byte)) {
            if (!previousIsCased)
                return JSValue::encode(jsBoolean(false));
            hasCased = true;
        } else
            previousIsCased = false;
    }
    return JSValue::encode(jsBoolean(hasCased));
}

// ---- To and from text

// The digits of the bytes, with a separator after every so many, counted from the right, or from the left if the number is negative.
static String hexOf(std::span<const uint8_t> content, std::optional<char> separator, int64_t bytesPerGroup)
{
    StringBuilder builder;
    size_t size = content.size();
    size_t group = bytesPerGroup < 0 ? -bytesPerGroup : bytesPerGroup;
    for (size_t i = 0; i < size; ++i) {
        if (i && separator && group) {
            size_t position = bytesPerGroup > 0 ? size - i : i;
            if (!(position % group))
                builder.append(*separator);
        }
        builder.append(hex(content[i], 2, Lowercase));
    }
    String result = builder.toString();
    return result.isNull() ? emptyString() : result;
}

// hex(sep=<none>, bytes_per_sep=1). Null if it raised.
// `content` is asked for what is in it when the arguments have been looked at, since looking at them can change it.
template<typename Content>
static String hexWithArguments(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, const Content& content)
{
    JSValue separatorValue = args.at(1);
    JSValue groupValue = args.at(2);
    std::optional<char> separator;
    if (separatorValue) {
        String text;
        if (separatorValue.isString())
            text = asString(separatorValue)->value(globalObject);
        else if (auto buffer = bytesKindOf(separatorValue) == BytesKind::None ? std::nullopt : builtinBufferOf(separatorValue))
            text = String(byteCast<Latin1Character>(*buffer));
        else {
            length(globalObject, separatorValue);
            RETURN_IF_EXCEPTION(scope, { });
            raiseTypeError(globalObject, scope, "sep must be str or bytes."_s);
            return { };
        }
        if (text.length() != 1) {
            raiseValueError(globalObject, scope, "sep must be length 1."_s);
            return { };
        }
        if (text[0] >= 0x80) {
            raiseValueError(globalObject, scope, "sep must be ASCII."_s);
            return { };
        }
        separator = static_cast<char>(text[0]);
    }
    int64_t group = 1;
    if (groupValue) {
        auto index = toIndex(globalObject, groupValue);
        RETURN_IF_EXCEPTION(scope, { });
        group = *index;
    }
    return hexOf(content.span(), separator, group);
}

PYTHON_NATIVE(bytesHex)
{
    BYTES_PROLOGUE("hex");
    String text = hexWithArguments(globalObject, scope, args, content);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

static String optionalText(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, unsigned position, ASCIILiteral name, ASCIILiteral function)
{
    JSValue value = args.at(position);
    if (!value)
        value = args.keyword(globalObject, name);
    if (!value)
        return { };
    if (!value.isString()) {
        raiseTypeError(globalObject, scope, makeString(function, "() argument '"_s, name, "' must be str, not "_s, typeName(globalObject, value)));
        return { };
    }
    return asString(value)->value(globalObject);
}

// decode(encoding='utf-8', errors='strict')
PYTHON_NATIVE(bytesDecode)
{
    BYTES_PROLOGUE("decode");
    String encoding = optionalText(globalObject, scope, args, 1, "encoding"_s, "decode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String errors = optionalText(globalObject, scope, args, 2, "errors"_s, "decode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String text = decodeBytes(globalObject, selfValue, content, encoding, errors);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

// str.encode(encoding='utf-8', errors='strict')
PYTHON_NATIVE(strEncode)
{
    NATIVE_PROLOGUE();
    JSValue self = args.at(0);
    if (auto* boxed = tryBoxedValue(self))
        self = boxed->value();
    String encoding = optionalText(globalObject, scope, args, 1, "encoding"_s, "encode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    String errors = optionalText(globalObject, scope, args, 2, "errors"_s, "encode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto encoded = encodeString(globalObject, self, encoding, errors);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, encoded->span())));
}

// other % self. It is the class of what is on the left that says what % means, and it is for that class to be this one.
PYTHON_NATIVE(bytesReflectedModulo)
{
    NATIVE_PROLOGUE();
    bool isBytes = typeOf(globalObject, args[0])->hasFlag(PyType::IsBytes);
    auto* other = dynamicDowncast<JSUint8Array>(args[1]);
    if (!other || typeOf(globalObject, other)->hasFlag(PyType::IsBytes) != isBytes)
        RETURN_NOT_IMPLEMENTED();
    String text = bytesPercentFormat(globalObject, other->typedSpan(), args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, other, text.is8Bit() ? byteCast<uint8_t>(text.span8()) : std::span<const uint8_t>())));
}

// How many bytes CPython would have set aside: room for what is in it and for a zero after that.
PYTHON_NATIVE(byteArrayAlloc)
{
    BYTES_PROLOGUE("__alloc__");
    return JSValue::encode(intFromInt64(globalObject, content.size() ? content.size() + 1 : 0));
}

PYTHON_NATIVE(bytesModulo)
{
    BYTES_PROLOGUE("__mod__");
    String text = bytesPercentFormat(globalObject, content, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, text.is8Bit() ? byteCast<uint8_t>(text.span8()) : std::span<const uint8_t>())));
}

// translate(table, /, delete=b'')
PYTHON_NATIVE(bytesTranslate)
{
    BYTES_PROLOGUE("translate");
    if (args.size() < 2 || args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, "translate() takes at least 1 positional argument"_s));
    std::optional<std::span<const uint8_t>> table;
    if (!isNone(args[1])) {
        table = bufferOf(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (table->size() != 256)
            return JSValue::encode(raiseValueError(globalObject, scope, "translation table must be 256 characters long"_s));
    }
    JSValue deleteValue = args.at(2);
    std::array<bool, 256> isDeleted { };
    if (deleteValue) {
        auto deleted = bufferOf(globalObject, deleteValue);
        RETURN_IF_EXCEPTION(scope, { });
        for (uint8_t byte : *deleted)
            isDeleted[byte] = true;
    }
    ByteVector result;
    for (uint8_t byte : content) {
        if (!isDeleted[byte])
            result.append(table ? (*table)[byte] : byte);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newLike(globalObject, selfValue, result.span())));
}

// bytes.maketrans(from, to)
PYTHON_NATIVE(bytesMakeTranslation)
{
    NATIVE_PROLOGUE();
    auto from = bufferOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto to = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (from->size() != to->size())
        return JSValue::encode(raiseValueError(globalObject, scope, "maketrans arguments must have same length"_s));
    std::array<uint8_t, 256> table;
    for (unsigned i = 0; i < 256; ++i)
        table[i] = i;
    for (size_t i = 0; i < from->size(); ++i)
        table[(*from)[i]] = (*to)[i];
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, table)));
}

// ---- What only a bytearray does

// The bytes of an argument to extend(), += or a slice assignment: anything with bytes in it, or an iterable of ints. False if it raised.
static bool bytesToInsert(JSGlobalObject* globalObject, JSValue value, ByteVector& result, ASCIILiteral complaint, int flags = FullReadOnlyBuffer)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto buffer = tryBufferOf(globalObject, value, flags)) {
        buffer.appendTo(result);
        return true;
    }
    RETURN_IF_EXCEPTION(scope, false);
    if (value.isString() || (!typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_iter) && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_getitem))) {
        raiseTypeError(globalObject, scope, makeString(complaint, typeName(globalObject, value)));
        return false;
    }
    forEach(globalObject, value, [&] (JSValue item) {
        auto byte = byteFrom(globalObject, item, "byte must be in range(0, 256)"_s);
        if (!byte)
            return false;
        result.append(*byte);
        return true;
    });
    return !scope.exception();
}

PYTHON_NATIVE(byteArraySetItem)
{
    BYTES_PROLOGUE("__setitem__");
    // With no value, it is __delitem__.
    JSValue value = args.at(2);
    if (auto* slice = trySlice(args.at(1))) {
        ByteVector replacement;
        if (value) {
            if (classify(value).isInt() || value.isString())
                return JSValue::encode(raiseTypeError(globalObject, scope, "can assign only bytes, buffers, or iterables of ints in range(0, 256)"_s));
            if (!hasBuffer(globalObject, value) && !typeOf(globalObject, value)->lookup(vm, names.dunder_iter) && !typeOf(globalObject, value)->lookup(vm, names.dunder_getitem))
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("cannot convert '"_s, typeName(globalObject, value), "' object to bytearray"_s)));
            bytesToInsert(globalObject, value, replacement, ""_s);
            RETURN_IF_EXCEPTION(scope, { });
        }
        auto indices = slice->indices(globalObject, [&] { return self->length(); });
        RETURN_IF_EXCEPTION(scope, { });
        if (indices->step == 1) {
            scope.release();
            replaceRange(globalObject, self, indices->start, indices->length, replacement.span());
            RETURN_NONE();
        }
        if (value) {
            if (static_cast<int64_t>(replacement.size()) != indices->length)
                return JSValue::encode(raiseValueError(globalObject, scope, makeString("attempt to assign bytes of size "_s, replacement.size(), " to extended slice of size "_s, indices->length)));
            auto data = mutableSpanOf(self);
            for (int64_t i = 0, at = indices->start; i < indices->length; ++i, at += indices->step)
                data[at] = replacement[i];
            RETURN_NONE();
        }
        // What is left when every so many are taken out.
        ByteVector kept;
        auto data = spanOf(self);
        int64_t low = indices->step > 0 ? indices->start : indices->start + (indices->length - 1) * indices->step;
        int64_t step = std::abs(indices->step);
        for (int64_t i = 0; i < static_cast<int64_t>(data.size()); ++i) {
            bool isSelected = indices->length && i >= low && !((i - low) % step) && (i - low) / step < indices->length;
            if (!isSelected)
                kept.append(data[i]);
        }
        scope.release();
        replaceRange(globalObject, self, 0, self->length(), kept.span());
        RETURN_NONE();
    }
    if (!classify(args.at(1)).isInt() && !typeOf(globalObject, args.at(1))->lookup(vm, names.dunder_index))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("bytearray indices must be integers or slices, not "_s, typeName(globalObject, args.at(1)))));
    auto index = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t length = self->length();
    int64_t at = *index < 0 ? *index + length : *index;
    if (at < 0 || at >= length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "bytearray index out of range"_s));
    if (!value) {
        scope.release();
        replaceRange(globalObject, self, at, 1, { });
        RETURN_NONE();
    }
    auto byte = byteFrom(globalObject, value, "byte must be in range(0, 256)"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (at < static_cast<int64_t>(self->length()))
        mutableSpanOf(self)[at] = *byte;
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayAppend)
{
    BYTES_PROLOGUE("append");
    auto byte = byteFrom(globalObject, args[1], "byte must be in range(0, 256)"_s);
    RETURN_IF_EXCEPTION(scope, { });
    size_t length = self->length();
    if (!resize(globalObject, self, length + 1))
        return { };
    self->typedVector()[length] = *byte;
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayExtend)
{
    BYTES_PROLOGUE("extend");
    ByteVector added;
    bytesToInsert(globalObject, args[1], added, "can't extend bytearray with "_s, SimpleBuffer);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    replaceRange(globalObject, self, self->length(), 0, added.span());
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayInPlaceAdd)
{
    BYTES_PROLOGUE("__iadd__");
    auto other = tryBufferOf(globalObject, args[1]);
    // Whatever is wrong with it, this is what is said.
    if (scope.exception())
        catchException(globalObject, BuiltinType::BaseException);
    if (!other)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("can't concat "_s, typeName(globalObject, args.at(1)), " to bytearray"_s)));
    replaceRange(globalObject, self, self->length(), 0, *other);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(selfValue);
}

PYTHON_NATIVE(byteArrayInPlaceMultiply)
{
    BYTES_PROLOGUE("__imul__");
    if (!classify(args.at(1)).isInt() && !typeOf(globalObject, args.at(1))->lookup(vm, names.dunder_index))
        RETURN_NOT_IMPLEMENTED();
    ByteVector result;
    repeated(globalObject, scope, content, args[1], result);
    RETURN_IF_EXCEPTION(scope, { });
    replaceRange(globalObject, self, 0, self->length(), result.span());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(selfValue);
}

PYTHON_NATIVE(byteArrayInsert)
{
    BYTES_PROLOGUE("insert");
    auto index = toIndex(globalObject, args[1], true);
    RETURN_IF_EXCEPTION(scope, { });
    auto byte = byteFrom(globalObject, args[2], "byte must be in range(0, 256)"_s);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t length = self->length();
    int64_t at = *index < 0 ? std::max<int64_t>(*index + length, 0) : std::min(*index, length);
    scope.release();
    replaceRange(globalObject, self, at, 0, std::span<const uint8_t>(&*byte, 1));
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayPop)
{
    BYTES_PROLOGUE("pop");
    int64_t at = -1;
    if (args.size() > 1) {
        auto index = toIndex(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        at = *index;
    }
    int64_t length = self->length();
    if (!length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop from empty bytearray"_s));
    if (at < 0)
        at += length;
    if (at < 0 || at >= length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop index out of range"_s));
    uint8_t byte = spanOf(self)[at];
    replaceRange(globalObject, self, at, 1, { });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(byte));
}

PYTHON_NATIVE(byteArrayRemove)
{
    BYTES_PROLOGUE("remove");
    auto byte = byteFrom(globalObject, args[1], "byte must be in range(0, 256)"_s);
    RETURN_IF_EXCEPTION(scope, { });
    size_t found = findIn(spanOf(self), std::span<const uint8_t>(&*byte, 1));
    if (found == notFound)
        return JSValue::encode(raiseValueError(globalObject, scope, "value not found in bytearray"_s));
    scope.release();
    replaceRange(globalObject, self, found, 1, { });
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayReverse)
{
    BYTES_PROLOGUE("reverse");
    std::ranges::reverse(mutableSpanOf(self));
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayClear)
{
    BYTES_PROLOGUE("clear");
    scope.release();
    resize(globalObject, self, 0);
    RETURN_NONE();
}

PYTHON_NATIVE(byteArrayCopy)
{
    BYTES_PROLOGUE("copy");
    RELEASE_AND_RETURN(scope, JSValue::encode(newByteArray(globalObject, content)));
}

// resize(size)
PYTHON_NATIVE(byteArrayResize)
{
    BYTES_PROLOGUE("resize");
    auto size = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*size < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, makeString("Can only resize to positive sizes, got "_s, *size)));
    scope.release();
    resize(globalObject, self, *size);
    RETURN_NONE();
}

// ---- int.to_bytes() and int.from_bytes()

// Whether the byte order is big endian. Nothing if it raised.
static std::optional<bool> byteOrderFrom(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (!value)
        return true;
    if (!value.isString()) {
        raiseTypeError(globalObject, scope, makeString("to_bytes() argument 'byteorder' must be str, not "_s, typeName(globalObject, value)));
        return std::nullopt;
    }
    String text = asString(value)->value(globalObject);
    if (text == "big"_s)
        return true;
    if (text == "little"_s)
        return false;
    raiseValueError(globalObject, scope, "byteorder must be either 'little' or 'big'"_s);
    return std::nullopt;
}

// to_bytes(length=1, byteorder='big', *, signed=False)
PYTHON_NATIVE(intToBytes)
{
    NATIVE_PROLOGUE();
    Number number = classify(args.at(0));
    JSValue lengthValue = args.at(1);
    JSValue orderValue = args.at(2);
    JSValue signedValue = args.keyword(globalObject, "signed"_s);
    int64_t length = 1;
    if (lengthValue) {
        auto index = toIndex(globalObject, lengthValue);
        RETURN_IF_EXCEPTION(scope, { });
        length = *index;
        if (length < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "length argument must be non-negative"_s));
    }
    auto isBigEndian = byteOrderFrom(globalObject, scope, orderValue);
    RETURN_IF_EXCEPTION(scope, { });
    bool isSigned = signedValue && isTrue(globalObject, signedValue);
    RETURN_IF_EXCEPTION(scope, { });

    // How big it is, least byte first.
    ByteVector magnitude;
    bool isNegative;
    if (number.kind == Number::Kind::Small) {
        isNegative = number.small < 0;
        for (uint64_t rest = isNegative ? -static_cast<int64_t>(number.small) : number.small; rest; rest >>= 8)
            magnitude.append(static_cast<uint8_t>(rest));
    } else {
        isNegative = number.big->sign();
        for (unsigned i = 0; i < number.big->length(); ++i) {
            uint64_t digit = number.big->digit(i);
            for (unsigned k = 0; k < 8; ++k)
                magnitude.append(static_cast<uint8_t>(digit >> (8 * k)));
        }
        while (!magnitude.isEmpty() && !magnitude.last())
            magnitude.removeLast();
    }
    if (isNegative && !isSigned)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative int to unsigned"_s));
    auto tooBig = [&] { return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "int too big to convert"_s)); };
    if (static_cast<int64_t>(magnitude.size()) > length)
        return tooBig();

    ByteVector result;
    result.append(magnitude.span());
    result.appendUsingFunctor(length - magnitude.size(), [] (size_t) -> uint8_t { return 0; });
    if (isNegative) {
        // Two's complement: every bit turned over, and one added.
        unsigned carry = 1;
        for (auto& byte : result) {
            unsigned sum = static_cast<uint8_t>(~byte) + carry;
            byte = static_cast<uint8_t>(sum);
            carry = sum >> 8;
        }
    }
    // The top bit is the sign's.
    if (isSigned && length && (result.last() >> 7) != isNegative)
        return tooBig();
    if (isSigned && !length && !magnitude.isEmpty())
        return tooBig();
    if (*isBigEndian)
        result.reverse();
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, result.span())));
}

// int.from_bytes(bytes, byteorder='big', *, signed=False)
PYTHON_NATIVE(intFromBytes)
{
    NATIVE_PROLOGUE();
    JSValue source = args.at(1);
    if (!source)
        return JSValue::encode(raiseTypeError(globalObject, scope, "from_bytes() missing required argument 'bytes' (pos 1)"_s));
    JSValue orderValue = args.at(2);
    JSValue signedValue = args.keyword(globalObject, "signed"_s);
    auto isBigEndian = byteOrderFrom(globalObject, scope, orderValue);
    RETURN_IF_EXCEPTION(scope, { });
    bool isSigned = signedValue && isTrue(globalObject, signedValue);
    RETURN_IF_EXCEPTION(scope, { });

    ByteVector content;
    if (source.isString() || (!hasBuffer(globalObject, source) && !typeOf(globalObject, source)->lookup(vm, names.dunder_iter) && !typeOf(globalObject, source)->lookup(vm, names.dunder_getitem)))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("cannot convert '"_s, typeName(globalObject, source), "' object to bytes"_s)));
    bytesToInsert(globalObject, source, content, ""_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!*isBigEndian)
        content.reverse();
    // Most significant first, now.
    bool isNegative = isSigned && !content.isEmpty() && content[0] >> 7;
    if (isNegative) {
        unsigned carry = 1;
        for (size_t i = content.size(); i--;) {
            unsigned sum = static_cast<uint8_t>(~content[i]) + carry;
            content[i] = static_cast<uint8_t>(sum);
            carry = sum >> 8;
        }
    }
    String digits = hexOf(content.span(), std::nullopt, 0);
    JSValue result = jsNumber(0);
    if (!digits.isEmpty()) {
        result = JSBigInt::parseInt(globalObject, vm, StringView(digits), 16, JSBigInt::ErrorParseMode::IgnoreExceptions, JSBigInt::ParseIntSign::Unsigned);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNegative && result.isHeapBigInt())
            result = JSBigInt::unaryMinus(globalObject, result.asHeapBigInt());
        RETURN_IF_EXCEPTION(scope, { });
        result = normalizeBigInt(result);
    }
    PyType* type = asType(args.at(0));
    if (type == realm->typeInt())
        return JSValue::encode(result);
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, type, result)));
}

} // namespace Python

// ---- memoryview

const ClassInfo PyMemoryView::s_info = { "memoryview"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyMemoryView) };

template<typename Visitor>
void PyMemoryView::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyMemoryView>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_object);
    visitor.append(thisObject->m_exporter);
}

DEFINE_VISIT_CHILDREN(PyMemoryView);

Structure* PyMemoryView::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

// What stands between a memoryview and an object whose class has a __buffer__(): the memoryview that that returned, the object, and how many memoryviews there are of
// it that have not been released. The last of them to be released has __release_buffer__() called.
namespace BufferWrapperField {
enum Field : unsigned { View, Object, Count };
}

PyMemoryView* PyMemoryView::create(JSGlobalObject* globalObject, JSValue object, char format, unsigned itemSize, int64_t offset, int64_t length, int64_t stride, bool isReadOnly, JSValue exporter)
{
    VM& vm = globalObject->vm();
    auto* view = new (NotNull, allocateCell<PyMemoryView>(vm)) PyMemoryView(vm, globalObject->pyRealm()->structureFor(BuiltinType::MemoryView), object, format, itemSize, offset, length, stride, isReadOnly, exporter);
    view->finishCreation(vm);
    if (exporter) {
        auto* wrapper = uncheckedDowncast<PyNativeObject>(exporter.asCell());
        wrapper->setField(vm, BufferWrapperField::Count, jsNumber(wrapper->field(BufferWrapperField::Count).asInt32() + 1));
    }
    return view;
}

PyMemoryView* PyMemoryView::derive(JSGlobalObject* globalObject, char format, unsigned itemSize, int64_t offset, int64_t length, int64_t stride, bool isReadOnly) const
{
    return create(globalObject, m_object.get(), format, itemSize, offset, length, stride, isReadOnly, m_exporter.get());
}

void PyMemoryView::release(JSGlobalObject* globalObject)
{
    if (isReleased())
        return;
    VM& vm = globalObject->vm();
    m_object.clear();
    JSValue exporter = m_exporter.get();
    if (!exporter)
        return;
    m_exporter.clear();
    auto* wrapper = uncheckedDowncast<PyNativeObject>(exporter.asCell());
    int32_t left = wrapper->field(BufferWrapperField::Count).asInt32() - 1;
    wrapper->setField(vm, BufferWrapperField::Count, jsNumber(left));
    if (!left)
        Python::releaseBufferOfProgram(globalObject, wrapper->field(BufferWrapperField::Object), wrapper->field(BufferWrapperField::View));
}

std::optional<std::span<const uint8_t>> PyMemoryView::contiguousSpan() const
{
    if (isReleased() || !isContiguous())
        return std::nullopt;
    auto whole = Python::builtinBufferOf(m_object.get());
    size_t size = static_cast<size_t>(m_length) * m_itemSize;
    if (!whole || static_cast<size_t>(m_offset) + size > whole->size())
        return std::span<const uint8_t>();
    return whole->subspan(m_offset, size);
}

std::span<uint8_t> PyMemoryView::item(int64_t index) const
{
    if (isReleased())
        return { };
    auto whole = Python::builtinBufferOf(m_object.get());
    int64_t at = m_offset + index * m_stride;
    if (!whole || at < 0 || static_cast<size_t>(at) + m_itemSize > whole->size())
        return { };
    // Whether it may be written to is for whoever asks to have checked.
    return { const_cast<uint8_t*>(whole->data()) + at, m_itemSize };
}

namespace Python {

static PyMemoryView* asMemory(JSValue value) { return uncheckedDowncast<PyMemoryView>(value.asCell()); }

#define MEMORY_PROLOGUE() \
    NATIVE_PROLOGUE(); \
    PyMemoryView* self = asMemory(args.at(0)); \
    if (self->isReleased()) \
        return JSValue::encode(raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s));

static unsigned itemSizeOf(char format)
{
    switch (format) {
    case 'b':
    case 'B':
    case 'c':
    case '?':
        return 1;
    case 'h':
    case 'H':
    case 'e':
        return 2;
    case 'i':
    case 'I':
    case 'f':
        return 4;
    case 'l':
    case 'L':
    case 'q':
    case 'Q':
    case 'n':
    case 'N':
    case 'P':
    case 'd':
        return 8;
    default:
        return 0;
    }
}

static char formatOf(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return 'b';
    case TypeInt16:
        return 'h';
    case TypeUint16:
        return 'H';
    case TypeInt32:
        return 'i';
    case TypeUint32:
        return 'I';
    case TypeFloat16:
        return 'e';
    case TypeFloat32:
        return 'f';
    case TypeFloat64:
        return 'd';
    case TypeBigInt64:
        return 'q';
    case TypeBigUint64:
        return 'Q';
    default:
        return 'B';
    }
}

template<typename T>
static T load(std::span<const uint8_t> bytes)
{
    T value;
    memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

static JSValue unpackItem(JSGlobalObject* globalObject, char format, std::span<const uint8_t> bytes)
{
    switch (format) {
    case 'b':
        return jsNumber(load<int8_t>(bytes));
    case 'B':
        return jsNumber(bytes[0]);
    case 'c':
        return newBytes(globalObject, bytes);
    case '?':
        return jsBoolean(bytes[0]);
    case 'h':
        return jsNumber(load<int16_t>(bytes));
    case 'H':
        return jsNumber(load<uint16_t>(bytes));
    case 'i':
        return jsNumber(load<int32_t>(bytes));
    case 'I':
        return intFromInt64(globalObject, load<uint32_t>(bytes));
    case 'f':
        return floatFromDouble(load<float>(bytes));
    case 'd':
        return floatFromDouble(load<double>(bytes));
    case 'l':
    case 'q':
    case 'n':
        return intFromInt64(globalObject, load<int64_t>(bytes));
    default: {
        uint64_t value = load<uint64_t>(bytes);
        if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return intFromInt64(globalObject, value);
        return JSBigInt::createFrom(globalObject, value);
    }
    }
}

// False if it raised.
static bool packItem(JSGlobalObject* globalObject, char format, std::span<uint8_t> bytes, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto invalidType = [&] {
        raiseTypeError(globalObject, scope, makeString("memoryview: invalid type for format '"_s, format, '\''));
        return false;
    };
    auto invalidValue = [&] {
        raiseValueError(globalObject, scope, makeString("memoryview: invalid value for format '"_s, format, '\''));
        return false;
    };
    auto store = [&] (auto number) {
        memcpy(bytes.data(), &number, sizeof(number));
        return true;
    };
    if (format == 'f' || format == 'd') {
        if (!classify(value))
            return invalidType();
        auto real = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        return format == 'f' ? store(static_cast<float>(*real)) : store(*real);
    }
    if (format == 'c') {
        auto buffer = isBytes(value) ? builtinBufferOf(value) : std::nullopt;
        if (!buffer)
            return invalidType();
        if (buffer->size() != 1)
            return invalidValue();
        return store((*buffer)[0]);
    }
    if (format == '?') {
        bool truth = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        return store(static_cast<uint8_t>(truth));
    }
    Number number = classify(value);
    if (!number.isInt())
        return invalidType();
    if (number.kind == Number::Kind::Big) {
        // Only the widest have room for one.
        bool isUnsigned = format == 'Q' || format == 'L' || format == 'N' || format == 'P';
        if (bytes.size() != 8 || number.big->length() > 1)
            return invalidValue();
        uint64_t digit = number.big->digit(0);
        if (isUnsigned)
            return number.big->sign() ? invalidValue() : store(digit);
        if (number.big->sign() ? digit > (1ULL << 63) : digit >= (1ULL << 63))
            return invalidValue();
        return store(number.big->sign() ? static_cast<int64_t>(0 - digit) : static_cast<int64_t>(digit));
    }
    int64_t small = number.small;
    auto storeInRange = [&] <typename T> (T) {
        if (small < static_cast<int64_t>(std::numeric_limits<T>::min()) || (small > 0 && static_cast<uint64_t>(small) > static_cast<uint64_t>(std::numeric_limits<T>::max())))
            return invalidValue();
        return store(static_cast<T>(small));
    };
    switch (format) {
    case 'b':
        return storeInRange(int8_t());
    case 'B':
        return storeInRange(uint8_t());
    case 'h':
        return storeInRange(int16_t());
    case 'H':
        return storeInRange(uint16_t());
    case 'i':
        return storeInRange(int32_t());
    case 'I':
        return storeInRange(uint32_t());
    case 'l':
    case 'q':
    case 'n':
        return storeInRange(int64_t());
    default:
        return storeInRange(uint64_t());
    }
}

// All that it is a view of, in order, whether or not it skips any.
static ByteVector bytesOfMemory(PyMemoryView* memory)
{
    ByteVector result;
    if (auto span = memory->contiguousSpan()) {
        result.append(*span);
        return result;
    }
    for (int64_t i = 0; i < memory->length(); ++i)
        result.append(memory->item(i));
    return result;
}

// memoryview(object): PyMemoryView_FromObjectAndFlags() of CPython's Objects/memoryobject.c. Null if it raised.
static PyMemoryView* memoryViewOf(JSGlobalObject* globalObject, JSValue object, int flags)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue method = bufferMethodOfProgram(globalObject, object, vm.pythonNames().dunder_buffer)) {
        JSValue given = call(globalObject, method, object, jsNumber(flags));
        RETURN_IF_EXCEPTION(scope, nullptr);
        auto* inner = dynamicDowncast<PyMemoryView>(given);
        if (!inner) {
            raiseTypeError(globalObject, scope, "__buffer__ returned non-memoryview object"_s);
            return nullptr;
        }
        if (inner->isReleased()) {
            raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
            return nullptr;
        }
        if ((flags & WritableBuffer) && inner->isReadOnly()) {
            raise(globalObject, scope, BuiltinType::BufferError, "memoryview: underlying buffer is not writable"_s);
            return nullptr;
        }
        JSValue wrapper = PyNativeObject::create(globalObject, BuiltinType::BufferWrapper, inner, object, jsNumber(0));
        return PyMemoryView::create(globalObject, inner->object(), inner->format(), inner->itemSize(), inner->offset(), inner->length(), inner->stride(), inner->isReadOnly(), wrapper);
    }
    if (auto* other = dynamicDowncast<PyMemoryView>(object)) {
        if (other->isReleased()) {
            raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
            return nullptr;
        }
        return other->derive(globalObject, other->format(), other->itemSize(), other->offset(), other->length(), other->stride(), other->isReadOnly());
    }
    auto buffer = builtinBufferOf(object);
    if (!buffer) {
        raiseTypeError(globalObject, scope, makeString("memoryview: a bytes-like object is required, not '"_s, typeName(globalObject, object), '\''));
        return nullptr;
    }
    if ((flags & WritableBuffer) && isBytes(object)) {
        raise(globalObject, scope, BuiltinType::BufferError, "Object is not writable."_s);
        return nullptr;
    }
    char format = 'B';
    if (auto* view = dynamicDowncast<JSArrayBufferView>(object))
        format = formatOf(typedArrayType(view->type()));
    unsigned itemSize = itemSizeOf(format);
    return PyMemoryView::create(globalObject, object, format, itemSize, 0, buffer->size() / itemSize, itemSize, isBytes(object));
}

PYTHON_NATIVE(memoryNew)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(memoryViewOf(globalObject, args.at(1), FullReadOnlyBuffer)));
}

static std::optional<int> bufferFlagsFrom(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto flags = toIndex(globalObject, value);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::IndexError))
            raise(globalObject, scope, BuiltinType::OverflowError, makeString("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
        return std::nullopt;
    }
    if (*flags > std::numeric_limits<int>::max() || *flags < std::numeric_limits<int>::min()) {
        raise(globalObject, scope, BuiltinType::OverflowError, "buffer flags out of range"_s);
        return std::nullopt;
    }
    return static_cast<int>(*flags);
}

// bytes.__buffer__(flags), and the same of a bytearray and of a memoryview
PYTHON_NATIVE(builtinGetBuffer)
{
    NATIVE_PROLOGUE();
    auto flags = bufferFlagsFrom(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // Not what a class derived from it may have put in the way.
    JSValue self = args[0];
    if (auto* memory = dynamicDowncast<PyMemoryView>(self)) {
        if (memory->isReleased())
            return JSValue::encode(raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s));
        if ((*flags & WritableBuffer) && memory->isReadOnly())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::BufferError, "memoryview: underlying buffer is not writable"_s));
        return JSValue::encode(memory->derive(globalObject, memory->format(), memory->itemSize(), memory->offset(), memory->length(), memory->stride(), memory->isReadOnly()));
    }
    if ((*flags & WritableBuffer) && isBytes(self))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::BufferError, "Object is not writable."_s));
    return JSValue::encode(PyMemoryView::create(globalObject, self, 'B', 1, 0, builtinBufferOf(self)->size(), 1, isBytes(self)));
}

// bytearray.__release_buffer__(view), and the same of a memoryview
PYTHON_NATIVE(builtinReleaseBuffer)
{
    NATIVE_PROLOGUE();
    auto* view = dynamicDowncast<PyMemoryView>(args[1]);
    if (!view)
        return JSValue::encode(raiseTypeError(globalObject, scope, "expected a memoryview object"_s));
    if (view->isReleased())
        return JSValue::encode(raiseValueError(globalObject, scope, "memoryview's buffer has already been released"_s));
    JSValue self = args[0];
    if (auto* memory = dynamicDowncast<PyMemoryView>(self))
        self = memory->object();
    if (view->object() != self)
        return JSValue::encode(raiseValueError(globalObject, scope, "memoryview's buffer is not this object"_s));
    view->release(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// memoryview._from_flags(object, flags)
PYTHON_NATIVE(memoryFromFlags)
{
    NATIVE_PROLOGUE();
    auto flags = toIndex(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(memoryViewOf(globalObject, args[1], static_cast<int>(*flags))));
}

PYTHON_NATIVE(memoryRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsString(vm, makeString(asMemory(args[0])->isReleased() ? "<released memory at "_s : "<memory at "_s, addressOf(args[0].asCell()), '>')));
}

PYTHON_NATIVE(memoryCount)
{
    MEMORY_PROLOGUE();
    int64_t count = 0;
    forEach(globalObject, self, [&] (JSValue item) {
        bool isSame = isIdentical(item, args[1]) || isEqual(globalObject, item, args[1]);
        count += isSame && !scope.exception();
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, count));
}

// memoryview.index(value, start=0, stop=sys.maxsize)
PYTHON_NATIVE(memoryIndex)
{
    NATIVE_PROLOGUE();
    // The bounds are looked at first.
    int64_t bounds[2] = { 0, std::numeric_limits<int64_t>::max() };
    for (unsigned i = 0; i < 2; ++i) {
        if (JSValue given = args.at(i + 2)) {
            if (!typeOf(globalObject, given)->lookup(vm, names.dunder_index))
                return JSValue::encode(raiseTypeError(globalObject, scope, "slice indices must be integers or have an __index__ method"_s));
            auto index = toIndex(globalObject, given, true);
            RETURN_IF_EXCEPTION(scope, { });
            bounds[i] = *index;
        }
    }
    PyMemoryView* self = asMemory(args[0]);
    if (self->isReleased())
        return JSValue::encode(raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s));
    int64_t size = self->length();
    auto [start, stop] = bounds;
    if (start < 0)
        start = std::max<int64_t>(start + size, 0);
    if (stop < 0)
        stop = std::max<int64_t>(stop + size, 0);
    stop = std::min(stop, size);
    start = std::min(start, stop);
    for (int64_t index = start; index < stop; ++index) {
        JSValue item = getItem(globalObject, self, intFromInt64(globalObject, index));
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = isIdentical(item, args[1]) || isEqual(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            return JSValue::encode(intFromInt64(globalObject, index));
    }
    return JSValue::encode(raiseValueError(globalObject, scope, "memoryview.index(x): x not found"_s));
}

PYTHON_NATIVE(memoryLen)
{
    MEMORY_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, self->length())));
}

PYTHON_NATIVE(memoryGetItem)
{
    MEMORY_PROLOGUE();
    JSValue key = args.at(1);
    if (auto* slice = trySlice(key)) {
        auto indices = slice->indices(globalObject, [&] { return self->length(); });
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(self->derive(globalObject, self->format(), self->itemSize(), self->offset() + indices->start * self->stride(), indices->length, self->stride() * indices->step, self->isReadOnly()));
    }
    if (!classify(key).isInt() && !typeOf(globalObject, key)->lookup(vm, names.dunder_index))
        return JSValue::encode(raiseTypeError(globalObject, scope, "memoryview: invalid slice key"_s));
    auto index = toIndex(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t at = *index < 0 ? *index + self->length() : *index;
    auto bytes = at < 0 || at >= self->length() ? std::span<uint8_t>() : self->item(at);
    if (bytes.empty())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "index out of bounds on dimension 1"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(unpackItem(globalObject, self->format(), bytes)));
}

PYTHON_NATIVE(memorySetItem)
{
    MEMORY_PROLOGUE();
    if (self->isReadOnly())
        return JSValue::encode(raiseTypeError(globalObject, scope, "cannot modify read-only memory"_s));
    JSValue key = args.at(1);
    JSValue value = args.at(2);
    if (!value)
        return JSValue::encode(raiseTypeError(globalObject, scope, "cannot delete memory"_s));
    if (auto* slice = trySlice(key)) {
        auto indices = slice->indices(globalObject, [&] { return self->length(); });
        RETURN_IF_EXCEPTION(scope, { });
        ByteVector source;
        unsigned sourceItemSize = 1;
        char sourceFormat = 'B';
        if (auto* other = dynamicDowncast<PyMemoryView>(value)) {
            source = bytesOfMemory(other);
            sourceItemSize = other->itemSize();
            sourceFormat = other->format();
        } else {
            auto buffer = bufferOf(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            source.append(*buffer);
        }
        if (sourceFormat != self->format() || sourceItemSize != self->itemSize() || static_cast<int64_t>(source.size() / sourceItemSize) != indices->length)
            return JSValue::encode(raiseValueError(globalObject, scope, "memoryview assignment: lvalue and rvalue have different structures"_s));
        for (int64_t i = 0; i < indices->length; ++i) {
            auto target = self->item(indices->start + i * indices->step);
            if (!target.empty())
                memcpy(target.data(), source.span().data() + i * sourceItemSize, sourceItemSize);
        }
        RETURN_NONE();
    }
    if (!classify(key).isInt() && !typeOf(globalObject, key)->lookup(vm, names.dunder_index))
        return JSValue::encode(raiseTypeError(globalObject, scope, "memoryview: invalid slice key"_s));
    auto index = toIndex(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t at = *index < 0 ? *index + self->length() : *index;
    if (at < 0 || at >= self->length())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "index out of bounds on dimension 1"_s));
    // What is to be written is worked out first, since working it out can run anything, and where it goes is looked up after that.
    std::array<uint8_t, 8> packed;
    packItem(globalObject, self->format(), std::span(packed).first(self->itemSize()), value);
    RETURN_IF_EXCEPTION(scope, { });
    if (self->isReleased())
        return JSValue::encode(raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s));
    auto bytes = self->item(at);
    if (bytes.empty())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "index out of bounds on dimension 1"_s));
    memcpy(bytes.data(), packed.data(), bytes.size());
    RETURN_NONE();
}

PYTHON_NATIVE(memoryToBytes)
{
    MEMORY_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, bytesOfMemory(self).span())));
}

PYTHON_NATIVE(memoryToList)
{
    MEMORY_PROLOGUE();
    MarkedArgumentBuffer items;
    for (int64_t i = 0; i < self->length(); ++i) {
        auto bytes = self->item(i);
        if (bytes.empty())
            break;
        items.append(unpackItem(globalObject, self->format(), bytes));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, items)));
}

PYTHON_NATIVE(memoryHex)
{
    MEMORY_PROLOGUE();
    String text = hexWithArguments(globalObject, scope, args, bytesOfMemory(self));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(memoryRelease)
{
    NATIVE_PROLOGUE();
    asMemory(args[0])->release(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(memoryEnter)
{
    MEMORY_PROLOGUE();
    return JSValue::encode(self);
}

PYTHON_NATIVE(memoryToReadOnly)
{
    MEMORY_PROLOGUE();
    return JSValue::encode(self->derive(globalObject, self->format(), self->itemSize(), self->offset(), self->length(), self->stride(), true));
}

// cast(format): the same bytes, taken as items of another kind. One of the two kinds has to be bytes.
PYTHON_NATIVE(memoryCast)
{
    MEMORY_PROLOGUE();
    JSValue formatValue = args.at(1);
    if (!formatValue || !formatValue.isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "cast() argument 'format' must be str"_s));
    String text = asString(formatValue)->value(globalObject);
    if (text.startsWith('@'))
        text = text.substring(1);
    unsigned itemSize = text.length() == 1 ? itemSizeOf(static_cast<char>(text[0])) : 0;
    if (!itemSize)
        return JSValue::encode(raiseValueError(globalObject, scope, "memoryview: destination format must be a native single character format prefixed with an optional '@'"_s));
    char format = static_cast<char>(text[0]);
    auto isByteFormat = [] (char c) { return c == 'b' || c == 'B' || c == 'c'; };
    if (!isByteFormat(format) && !isByteFormat(self->format()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "memoryview: cannot cast between two non-byte formats"_s));
    if (!self->isContiguous())
        return JSValue::encode(raiseTypeError(globalObject, scope, "memoryview: casts are restricted to C-contiguous views"_s));
    int64_t size = self->length() * self->itemSize();
    if (size % itemSize)
        return JSValue::encode(raiseTypeError(globalObject, scope, "memoryview: length is not a multiple of itemsize"_s));
    return JSValue::encode(self->derive(globalObject, format, itemSize, self->offset(), size / itemSize, itemSize, self->isReadOnly()));
}

PYTHON_NATIVE(memoryEq)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    bool wantsEqual = op == ComparisonOperator::Eq;
    NATIVE_PROLOGUE();
    if (!isEquality(op))
        RETURN_NOT_IMPLEMENTED();
    PyMemoryView* self = asMemory(args.at(0));
    JSValue other = args.at(1);
    if (self->isReleased())
        return JSValue::encode(jsBoolean((other == JSValue(self)) == wantsEqual));
    // By value, so that an int and a float that are equal are.
    PyMemoryView* otherMemory = dynamicDowncast<PyMemoryView>(other);
    struct Releaser {
        JSGlobalObject* globalObject;
        PyMemoryView* view { nullptr };
        ~Releaser()
        {
            if (view)
                view->release(globalObject);
        }
    } releaser { globalObject };
    if (!otherMemory) {
        if (!hasBuffer(globalObject, other))
            RETURN_NOT_IMPLEMENTED();
        otherMemory = memoryViewOf(globalObject, other, FullReadOnlyBuffer);
        // What cannot be got at is something that this cannot be compared with.
        if (scope.exception()) {
            catchException(globalObject, BuiltinType::BaseException);
            RETURN_NOT_IMPLEMENTED();
        }
        releaser.view = otherMemory;
    } else if (otherMemory->isReleased())
        return JSValue::encode(jsBoolean(!wantsEqual));
    bool same = self->length() == otherMemory->length();
    for (int64_t i = 0; same && i < self->length(); ++i) {
        auto a = self->item(i);
        auto b = otherMemory->item(i);
        if (a.empty() || b.empty()) {
            same = false;
            break;
        }
        same = isEqual(globalObject, unpackItem(globalObject, self->format(), a), unpackItem(globalObject, otherMemory->format(), b));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(jsBoolean(same == wantsEqual));
}

PYTHON_NATIVE(memoryHash)
{
    MEMORY_PROLOGUE();
    if (!self->isReadOnly())
        return JSValue::encode(raiseValueError(globalObject, scope, "cannot hash writable memoryview object"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, hashOfBytes(bytesOfMemory(self).span()))));
}

PYTHON_NATIVE(memoryIter)
{
    MEMORY_PROLOGUE();
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::Memory, self, JSValue(), 0, self->length()));
}

// An attribute, which a view that has been released does not have.
template<JSValue (*get)(JSGlobalObject*, PyMemoryView*)>
static JSValue memoryAttribute(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (asMemory(self)->isReleased())
        return raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
    RELEASE_AND_RETURN(scope, get(globalObject, asMemory(self)));
}

// ---- Setting them up

void initializeBytesTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    PyType* bytes = realm->typeBytes();
    PyType* byteArray = realm->typeByteArray();

    // A bytearray is a Uint8Array like any other. A bytes is one whose prototype is the class, as with every other instance, and beyond
    // the class JavaScript finds what all Uint8Arrays have.
    Structure* plain = globalObject->typedArrayStructure(TypeUint8, false);
    byteArray->setInstanceStructure(vm, plain);
    // What JavaScript can do with a Uint8Array it can do with these, and with instances of classes derived from them.
    bytes->setPrototypeDirect(vm, plain->storedPrototype());
    byteArray->setPrototypeDirect(vm, plain->storedPrototype());
    bytes->setInstanceStructure(vm, JSUint8Array::createStructure(vm, globalObject, bytes));

    for (PyType* type : { bytes, byteArray }) {
        addMethods(globalObject, type, {
            { "__repr__"_s, bytesRepr },
            { "__len__"_s, bytesLen },
            { "__iter__"_s, bytesIter },
            { "__getitem__"_s, bytesGetItem },
            { "__contains__"_s, bytesContains },
            { "__eq__"_s, bytesCompare, Kind::Method, pack(ComparisonOperator::Eq) },
            { "__ne__"_s, bytesCompare, Kind::Method, pack(ComparisonOperator::NotEq) },
            { "__lt__"_s, bytesCompare, Kind::Method, pack(ComparisonOperator::Lt) },
            { "__le__"_s, bytesCompare, Kind::Method, pack(ComparisonOperator::LtE) },
            { "__gt__"_s, bytesCompare, Kind::Method, pack(ComparisonOperator::Gt) },
            { "__ge__"_s, bytesCompare, Kind::Method, pack(ComparisonOperator::GtE) },
            { "__add__"_s, bytesAdd },
            { "__mul__"_s, bytesMultiply },
            { "__rmul__"_s, bytesMultiply },
            { "__mod__"_s, bytesModulo },
            { "__rmod__"_s, bytesReflectedModulo },
            { "__str__"_s, bytesRepr },
            { "fromhex"_s, bytesFromHex, Kind::ClassMethod },
            { "maketrans"_s, bytesMakeTranslation, Kind::Function },
            { "find"_s, bytesFind, Kind::Method, pack(false, false) },
            { "rfind"_s, bytesFind, Kind::Method, pack(true, false) },
            { "index"_s, bytesFind, Kind::Method, pack(false, true) },
            { "rindex"_s, bytesFind, Kind::Method, pack(true, true) },
            { "count"_s, bytesCount },
            { "startswith"_s, bytesStartsOrEndsWith, Kind::Method, pack(true) },
            { "endswith"_s, bytesStartsOrEndsWith, Kind::Method, pack(false) },
            { "replace"_s, bytesReplace },
            { "join"_s, bytesJoin },
            { "split"_s, bytesSplit, Kind::Method, pack(false) },
            { "rsplit"_s, bytesSplit, Kind::Method, pack(true) },
            { "splitlines"_s, bytesSplitLines },
            { "partition"_s, bytesPartition, Kind::Method, pack(false) },
            { "rpartition"_s, bytesPartition, Kind::Method, pack(true) },
            { "strip"_s, bytesStrip, Kind::Method, pack(true, true) },
            { "lstrip"_s, bytesStrip, Kind::Method, pack(true, false) },
            { "rstrip"_s, bytesStrip, Kind::Method, pack(false, true) },
            { "ljust"_s, bytesJustify, Kind::Method, pack('<') },
            { "rjust"_s, bytesJustify, Kind::Method, pack('>') },
            { "center"_s, bytesJustify, Kind::Method, pack('^') },
            { "zfill"_s, bytesZfill },
            { "expandtabs"_s, bytesExpandTabs },
            { "removeprefix"_s, bytesRemoveAffix, Kind::Method, pack(true) },
            { "removesuffix"_s, bytesRemoveAffix, Kind::Method, pack(false) },
            { "upper"_s, bytesChangeCase, Kind::Method, pack(CaseChange::Upper) },
            { "lower"_s, bytesChangeCase, Kind::Method, pack(CaseChange::Lower) },
            { "swapcase"_s, bytesChangeCase, Kind::Method, pack(CaseChange::Swap) },
            { "capitalize"_s, bytesChangeCase, Kind::Method, pack(CaseChange::Capitalize) },
            { "title"_s, bytesChangeCase, Kind::Method, pack(CaseChange::Title) },
            { "isalpha"_s, bytesAll, Kind::Method, pack(ByteClass::Alpha) },
            { "isdigit"_s, bytesAll, Kind::Method, pack(ByteClass::Digit) },
            { "isalnum"_s, bytesAll, Kind::Method, pack(ByteClass::Alphanumeric) },
            { "isspace"_s, bytesAll, Kind::Method, pack(ByteClass::Space) },
            { "isascii"_s, bytesAll, Kind::Method, pack(ByteClass::ASCII) },
            { "isupper"_s, bytesIsCase, Kind::Method, pack(true) },
            { "islower"_s, bytesIsCase, Kind::Method, pack(false) },
            { "istitle"_s, bytesIsTitle },
            { "hex"_s, bytesHex },
            { "decode"_s, bytesDecode },
            { "translate"_s, bytesTranslate },
        });
    }
    addMethods(globalObject, bytes, {
        { "__new__"_s, bytesNew, Kind::New, 0, "(source=b'', encoding='utf-8', errors='strict')"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__hash__"_s, bytesHash },
        { "__bytes__"_s, bytesBytes },
    });
    addMethods(globalObject, byteArray, {
        { "__alloc__"_s, byteArrayAlloc },
        { "__new__"_s, byteArrayNew, Kind::New },
        { "__init__"_s, byteArrayInit, Kind::Method, 0, "(source=b'', encoding='utf-8', errors='strict')"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__setitem__"_s, byteArraySetItem },
        { "__delitem__"_s, byteArraySetItem },
        { "__iadd__"_s, byteArrayInPlaceAdd },
        { "__imul__"_s, byteArrayInPlaceMultiply },
        { "append"_s, byteArrayAppend },
        { "extend"_s, byteArrayExtend },
        { "insert"_s, byteArrayInsert },
        { "pop"_s, byteArrayPop },
        { "remove"_s, byteArrayRemove },
        { "reverse"_s, byteArrayReverse },
        { "clear"_s, byteArrayClear },
        { "copy"_s, byteArrayCopy },
        { "resize"_s, byteArrayResize },
    });
    byteArray->putDirect(vm, names.dunder_hash, jsUndefined());

    addMethods(globalObject, realm->typeStr(), { { "encode"_s, strEncode } });
    addMethods(globalObject, realm->typeInt(), {
        { "to_bytes"_s, intToBytes },
        { "from_bytes"_s, intFromBytes, Kind::ClassMethod },
    });

    PyType* memory = realm->typeMemoryView();
    memory->setInstanceStructure(vm, PyMemoryView::createStructure(vm, globalObject, memory));
    addComparisons(globalObject, memory, memoryEq);
    addMethods(globalObject, memory, {
        { "__new__"_s, memoryNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__len__"_s, memoryLen },
        { "__getitem__"_s, memoryGetItem },
        { "__setitem__"_s, memorySetItem },
        { "__delitem__"_s, memorySetItem },
        { "__iter__"_s, memoryIter },
        { "__hash__"_s, memoryHash },
        { "__repr__"_s, memoryRepr },
        { "count"_s, memoryCount },
        { "index"_s, memoryIndex },
        { "_from_flags"_s, memoryFromFlags, Kind::ClassMethod },
        { "__enter__"_s, memoryEnter },
        { "__exit__"_s, memoryRelease },
        { "release"_s, memoryRelease },
        { "tobytes"_s, memoryToBytes },
        { "tolist"_s, memoryToList },
        { "hex"_s, memoryHex },
        { "cast"_s, memoryCast },
        { "toreadonly"_s, memoryToReadOnly },
    });
    addGetSet(globalObject, memory, "obj"_s, memoryAttribute<[] (JSGlobalObject*, PyMemoryView* self) { return self->exporter() ? self->exporter() : self->object(); }>);
    realm->typeBufferWrapper()->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, realm->typeBufferWrapper()));
    for (PyType* type : { realm->typeBytes(), byteArray, memory })
        addMethods(globalObject, type, { { "__buffer__"_s, builtinGetBuffer } });
    for (PyType* type : { byteArray, memory, realm->typeBufferWrapper() })
        addMethods(globalObject, type, { { "__release_buffer__"_s, builtinReleaseBuffer } });
    addGetSet(globalObject, memory, "nbytes"_s, memoryAttribute<[] (JSGlobalObject* globalObject, PyMemoryView* self) { return intFromInt64(globalObject, self->length() * self->itemSize()); }>);
    addGetSet(globalObject, memory, "readonly"_s, memoryAttribute<[] (JSGlobalObject*, PyMemoryView* self) -> JSValue { return jsBoolean(self->isReadOnly()); }>);
    addGetSet(globalObject, memory, "itemsize"_s, memoryAttribute<[] (JSGlobalObject*, PyMemoryView* self) -> JSValue { return jsNumber(self->itemSize()); }>);
    addGetSet(globalObject, memory, "format"_s, memoryAttribute<[] (JSGlobalObject* globalObject, PyMemoryView* self) -> JSValue { return jsSingleCharacterString(globalObject->vm(), static_cast<Latin1Character>(self->format())); }>);
    addGetSet(globalObject, memory, "ndim"_s, memoryAttribute<[] (JSGlobalObject*, PyMemoryView*) -> JSValue { return jsNumber(1); }>);
    addGetSet(globalObject, memory, "shape"_s, memoryAttribute<[] (JSGlobalObject* globalObject, PyMemoryView* self) -> JSValue { return PyTuple::create(globalObject, { intFromInt64(globalObject, self->length()) }); }>);
    addGetSet(globalObject, memory, "strides"_s, memoryAttribute<[] (JSGlobalObject* globalObject, PyMemoryView* self) -> JSValue { return PyTuple::create(globalObject, { intFromInt64(globalObject, self->stride()) }); }>);
    addGetSet(globalObject, memory, "suboffsets"_s, memoryAttribute<[] (JSGlobalObject* globalObject, PyMemoryView*) -> JSValue { return globalObject->pyRealm()->emptyTuple(); }>);
    for (ASCIILiteral name : { "contiguous"_s, "c_contiguous"_s, "f_contiguous"_s })
        addGetSet(globalObject, memory, name, memoryAttribute<[] (JSGlobalObject*, PyMemoryView* self) -> JSValue { return jsBoolean(self->isContiguous()); }>);
}

} } // namespace JSC::Python
