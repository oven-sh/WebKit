/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyBytes.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyStr.h"
#include "PyTextSearch.h"
#include "PyTypes.h"
#include <wtf/Scope.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// bytes and bytearray. (Objects/bytesobject.c, bytearrayobject.c, bytes_methods.c and stringlib/ in CPython are the reference.)
//
// The bytes of a bytes object are in its cell, so they stay where they are for as long as it lives. Those of a bytearray move
// when it is resized, which anything that runs Python code may do: no span of one is kept across a call to __index__ and the like.

PY_DEFINE_CELL(PyBytes);
PY_DEFINE_CELL(PyByteArray);
const ClassInfo PyByteStorage::s_info = { "PyByteStorage"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(PyByteStorage) };

// Nothing longer is even asked of the allocator.
static constexpr size_t bytesMaxLength = (static_cast<size_t>(1) << 40) - 64;
static constexpr int64_t bytesMaxSsize = std::numeric_limits<int64_t>::max();

// ---- PyBytes ----

PyBytes::PyBytes(VM& vm, Structure* structure, PyType* type, size_t length)
    : Base(vm, structure, type)
    , m_length(length)
{
    mutableData()[length] = 0;
}

PyBytes* PyBytes::createUninitialized(VM& vm, Structure* structure, PyType* type, size_t length)
{
    auto* bytes = new (NotNull, allocateCell<PyBytes>(vm, allocationSize(length))) PyBytes(vm, structure, type, length);
    bytes->finishCreation(vm);
    return bytes;
}

PyBytes* PyBytes::tryCreateUninitialized(VM& vm, Structure* structure, PyType* type, size_t length)
{
    if (length > bytesMaxLength)
        return nullptr;
    void* cell = tryAllocateCell<PyBytes>(vm, allocationSize(length));
    if (!cell)
        return nullptr;
    auto* bytes = new (NotNull, cell) PyBytes(vm, structure, type, length);
    bytes->finishCreation(vm);
    return bytes;
}

PyBytes* PyBytes::createUninitialized(JSGlobalObject* globalObject, size_t length)
{
    Runtime& rt = runtime(globalObject);
    if (!length)
        return rt.emptyBytes();
    return createUninitialized(rt.vm(), rt.structure<PyBytes>(), rt.bytesType(), length);
}

PyBytes* PyBytes::create(JSGlobalObject* globalObject, std::span<const uint8_t> source)
{
    PyBytes* bytes = createUninitialized(globalObject, source.size());
    if (!source.empty())
        memcpy(bytes->mutableData(), source.data(), source.size());
    return bytes;
}

// ---- PyByteStorage ----

PyByteStorage::PyByteStorage(VM& vm, Structure* structure, size_t capacity)
    : Base(vm, structure)
    , m_capacity(capacity)
{
}

PyByteStorage* PyByteStorage::create(VM& vm, Structure* structure, size_t capacity)
{
    auto* storage = new (NotNull, allocateCell<PyByteStorage>(vm, sizeof(PyByteStorage) + capacity)) PyByteStorage(vm, structure, capacity);
    storage->finishCreation(vm);
    return storage;
}

PyByteStorage* PyByteStorage::tryCreate(VM& vm, Structure* structure, size_t capacity)
{
    void* cell = tryAllocateCell<PyByteStorage>(vm, sizeof(PyByteStorage) + capacity);
    if (!cell)
        return nullptr;
    auto* storage = new (NotNull, cell) PyByteStorage(vm, structure, capacity);
    storage->finishCreation(vm);
    return storage;
}

// ---- PyByteArray ----

PyByteArray::PyByteArray(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyByteArray* PyByteArray::create(VM& vm, Structure* structure, PyType* type)
{
    auto* array = new (NotNull, allocateCell<PyByteArray>(vm)) PyByteArray(vm, structure, type);
    array->finishCreation(vm);
    return array;
}

PyByteArray* PyByteArray::create(JSGlobalObject* globalObject, std::span<const uint8_t> source)
{
    Runtime& rt = runtime(globalObject);
    PyByteArray* array = create(rt.vm(), rt.structure<PyByteArray>(), rt.bytearrayType());
    if (!array->resize(globalObject, source.size()))
        return nullptr;
    if (!source.empty())
        memcpy(array->m_storage->data(), source.data(), source.size());
    return array;
}

template<typename Visitor>
void PyByteArray::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyByteArray>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_storage);
}

DEFINE_VISIT_CHILDREN(PyByteArray);

bool PyByteArray::resize(JSGlobalObject* globalObject, size_t length)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (length == m_length)
        return true;
    if (isStillExported(globalObject, this, [&] { return m_exports; })) {
        raiseBufferError(globalObject, scope, "Existing exports of data: object cannot be re-sized"_s);
        return false;
    }
    if (!length) {
        m_storage.clear();
        m_length = 0;
        return true;
    }

    size_t capacity = this->capacity();
    size_t needed = length + 1;
    size_t newCapacity = needed;
    if (needed <= capacity) {
        // Unless it shrinks by more than half, it stays where it is.
        if (needed >= capacity / 2) {
            uint8_t* data = m_storage->data();
            if (length > m_length)
                memset(data + m_length, 0, length - m_length);
            data[length] = 0;
            m_length = length;
            return true;
        }
    } else if (capacity && needed <= capacity + capacity / 8 + 8) {
        // It is growing little by little: leave room for more.
        newCapacity = needed + (needed >> 3) + (needed < 9 ? 3 : 6);
    }

    PyByteStorage* storage = length <= bytesMaxLength ? PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), newCapacity) : nullptr;
    if (!storage) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    size_t kept = std::min(m_length, length);
    if (kept)
        memcpy(storage->data(), m_storage->data(), kept);
    memset(storage->data() + kept, 0, newCapacity - kept);
    m_storage.set(vm, this, storage);
    m_length = length;
    return true;
}

void PyByteArray::append(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return;
    size_t oldLength = m_length;
    // They may be bytes of this very array, which is about to move.
    uintptr_t own = m_storage ? std::bit_cast<uintptr_t>(m_storage->data()) : 0;
    uintptr_t given = std::bit_cast<uintptr_t>(bytes.data());
    bool isOwn = own && given >= own && given < own + oldLength;
    if (!resize(globalObject, oldLength + bytes.size()))
        return;
    uint8_t* data = m_storage->data();
    memmove(data + oldLength, isOwn ? data + (given - own) : bytes.data(), bytes.size());
}

// ---- What has bytes to show ----

bool isBytesLike(JSValue value)
{
    return hasBuffer(value);
}

bool getBytesLike(JSGlobalObject* globalObject, JSValue value, std::span<const uint8_t>& result)
{
    // A class derived from bytes may have a __buffer__() of its own.
    if (value && value.isCell() && value.asCell()->type() == PyBytesType) {
        auto* bytes = uncheckedDowncast<PyBytes>(value.asCell());
        if (!bytes->type()->has(TypeFlag::Heap)) [[likely]] {
            result = bytes->span();
            return true;
        }
    }
    return getBytesLikeSlow(globalObject, value, result);
}

String bytesRepr(std::span<const uint8_t> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    bool hasSingleQuote = false;
    bool hasDoubleQuote = false;
    for (uint8_t byte : bytes) {
        hasSingleQuote |= byte == '\'';
        hasDoubleQuote |= byte == '"';
    }
    char quote = hasSingleQuote && !hasDoubleQuote ? '"' : '\'';
    StrBuilder builder;
    builder.reserveCapacity(bytes.size() + 3);
    builder.append('b', quote);
    for (uint8_t byte : bytes) {
        if (byte == quote || byte == '\\')
            builder.append('\\', static_cast<char>(byte));
        else if (byte == '\t')
            builder.append("\\t"_s);
        else if (byte == '\n')
            builder.append("\\n"_s);
        else if (byte == '\r')
            builder.append("\\r"_s);
        else if (byte < ' ' || byte >= 0x7f)
            builder.append("\\x"_s, digits[byte >> 4], digits[byte & 15]);
        else
            builder.append(static_cast<char>(byte));
    }
    builder.append(quote);
    return builder.toString();
}

// That of the str with these code points, as in CPython, where both are the hash of the same bytes in memory.
// FIXME: Not keyed, so a program can be fed keys that all collide; and not remembered, so it is computed at every lookup.
int64_t hashOfBytes(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return 0;
    return StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(bytes));
}

// ---- Helpers ----

static ALWAYS_INLINE bool bytesIsSpace(uint8_t byte) { return byte == ' ' || (byte >= '\t' && byte <= '\r'); }
static ALWAYS_INLINE bool bytesIsLower(uint8_t byte) { return byte >= 'a' && byte <= 'z'; }
static ALWAYS_INLINE bool bytesIsUpper(uint8_t byte) { return byte >= 'A' && byte <= 'Z'; }
static ALWAYS_INLINE bool bytesIsAlpha(uint8_t byte) { return bytesIsLower(byte | 0x20); }
static ALWAYS_INLINE bool bytesIsDecimal(uint8_t byte) { return byte >= '0' && byte <= '9'; }
static ALWAYS_INLINE bool bytesIsAlphanumeric(uint8_t byte) { return bytesIsAlpha(byte) || bytesIsDecimal(byte); }
static ALWAYS_INLINE uint8_t bytesToLower(uint8_t byte) { return bytesIsUpper(byte) ? byte | 0x20 : byte; }
static ALWAYS_INLINE uint8_t bytesToUpper(uint8_t byte) { return bytesIsLower(byte) ? byte & ~0x20 : byte; }

static ALWAYS_INLINE JSValue bytesNumber(int64_t value) { return jsNumber(static_cast<double>(value)); }

// `self` is a bytes or a bytearray, or an instance of a class derived from one.
static ALWAYS_INLINE bool bytesIsArray(JSValue self) { return self.asCell()->type() == PyByteArrayType; }

static ALWAYS_INLINE std::span<const uint8_t> bytesSpanOf(JSValue self)
{
    JSCell* cell = self.asCell();
    if (cell->type() == PyBytesType)
        return uncheckedDowncast<PyBytes>(cell)->span();
    ASSERT(cell->type() == PyByteArrayType);
    return uncheckedDowncast<PyByteArray>(cell)->span();
}

// While there is one of these, a bytearray cannot be resized: what tries to gets a BufferError.
class BytesExportScope {
    WTF_MAKE_NONCOPYABLE(BytesExportScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit BytesExportScope(JSValue self)
        : m_array(PyByteArray::tryFrom(self))
    {
        if (m_array)
            m_array->addExport();
    }
    ~BytesExportScope()
    {
        if (m_array)
            m_array->removeExport();
    }

private:
    PyByteArray* m_array;
};

static ALWAYS_INLINE bool bytesIsExact(JSGlobalObject* globalObject, JSValue value)
{
    return PyBytes::is(value) && uncheckedDowncast<PyBytes>(value.asCell())->type() == runtime(globalObject).bytesType();
}

// As the %T of PyErr_Format().
static String bytesFullTypeName(JSGlobalObject* globalObject, JSValue value)
{
    String name = qualifiedTypeName(globalObject, typeOf(globalObject, value));
    if (name.startsWith("__main__."_s))
        return name.substring(9);
    return name;
}

// As _PyArg_BadArgument() names what it was given.
static String bytesArgumentTypeName(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? "None"_str : typeName(globalObject, value);
}

static NEVER_INLINE JSValue bytesRaiseNoMemory(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseMemoryError(globalObject, scope, String());
}

// A new bytes or bytearray of that length, for the caller to fill. False: there is no room for it, and an exception says so.
static bool bytesAllocate(JSGlobalObject* globalObject, bool asArray, size_t length, JSValue& object, uint8_t*& data)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (asArray) {
        PyByteArray* array = PyByteArray::create(vm, rt.structure<PyByteArray>(), rt.bytearrayType());
        if (!array->resize(globalObject, length))
            return false;
        object = array;
        data = array->mutableSpan().data();
        return true;
    }
    if (!length) {
        object = rt.emptyBytes();
        data = nullptr;
        return true;
    }
    PyBytes* bytes = PyBytes::tryCreateUninitialized(vm, rt.structure<PyBytes>(), rt.bytesType(), length);
    if (!bytes) {
        bytesRaiseNoMemory(globalObject);
        return false;
    }
    object = bytes;
    data = bytes->mutableData();
    return true;
}

static JSValue bytesMake(JSGlobalObject* globalObject, bool asArray, std::span<const uint8_t> source)
{
    JSValue object;
    uint8_t* data;
    if (!bytesAllocate(globalObject, asArray, source.size(), object, data))
        return { };
    if (!source.empty())
        memcpy(data, source.data(), source.size());
    return object;
}

// What a method returns when there is nothing to change: the object itself where that cannot be told from a copy.
static JSValue bytesSelfOrCopy(JSGlobalObject* globalObject, JSValue self)
{
    if (bytesIsExact(globalObject, self))
        return self;
    return bytesMake(globalObject, bytesIsArray(self), bytesSpanOf(self));
}

static JSValue bytesPart(JSGlobalObject* globalObject, JSValue self, size_t from, size_t to)
{
    auto span = bytesSpanOf(self);
    ASSERT(from <= to && to <= span.size());
    if (!from && to == span.size())
        return bytesSelfOrCopy(globalObject, self);
    return bytesMake(globalObject, bytesIsArray(self), span.subspan(from, to - from));
}

// An argument that Argument Clinic has as a Py_ssize_t.
static bool bytesSsizeArgument(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        return false;
    }
    result = *converted;
    return true;
}

// One that it has as an int.
static bool bytesIntArgument(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted || *converted < std::numeric_limits<int>::min() || *converted > std::numeric_limits<int>::max()) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
        return false;
    }
    result = static_cast<int>(*converted);
    return true;
}

// An int that is to be one of the bytes of a bytearray.
static bool bytesByteValue(JSGlobalObject* globalObject, JSValue value, uint8_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted || *converted < 0 || *converted > 255) {
        raiseValueError(globalObject, scope, "byte must be in range(0, 256)"_s);
        return false;
    }
    result = static_cast<uint8_t>(*converted);
    return true;
}

static bool bytesOptionalStr(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral name, JSString*& result)
{
    result = nullptr;
    if (!value)
        return true;
    if (!isStr(value)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, function, "() argument '"_s, name, "' must be str, not "_s, bytesArgumentTypeName(globalObject, value));
        return false;
    }
    result = strValue(value);
    return true;
}

// As PyNumber_Check().
static bool bytesIsNumberLike(JSGlobalObject* globalObject, JSValue value)
{
    if (isInt(value) || isFloat(value) || PyComplex::is(value))
        return true;
    PyType* type = typeOf(globalObject, value);
    return type->lookup(Special::Index) || type->lookup(Special::Int) || type->lookup(Special::Float);
}

// Where the slice falls in `self`.
static bool bytesSliceIndices(JSGlobalObject* globalObject, JSValue self, PySlice* slice, PySlice::Indices& indices)
{
    if (!slice->unpack(globalObject, indices))
        return false;
    PySlice::adjust(bytesSpanOf(self).size(), indices);
    return true;
}

static size_t bytesSearch(std::span<const uint8_t> haystack, std::span<const uint8_t> needle)
{
    if (needle.size() > haystack.size())
        return notFound;
    if (needle.empty())
        return 0;
    if (needle.size() >= shortestNeedleForTwoWaySearch)
        return twoWaySearch<false>(haystack, needle);
    const uint8_t* start = haystack.data();
    size_t last = haystack.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        auto* found = static_cast<const uint8_t*>(memchr(start + i, needle[0], last - i + 1));
        if (!found)
            return notFound;
        i = found - start;
        if (!memcmp(found + 1, needle.data() + 1, needle.size() - 1))
            return i;
    }
    return notFound;
}

static size_t bytesSearchBackward(std::span<const uint8_t> haystack, std::span<const uint8_t> needle)
{
    if (needle.size() > haystack.size())
        return notFound;
    if (needle.empty())
        return haystack.size();
    if (needle.size() >= shortestNeedleForTwoWaySearch)
        return twoWaySearch<true>(haystack, needle);
    for (size_t i = haystack.size() - needle.size() + 1; i--;) {
        if (haystack[i] == needle[0] && !memcmp(haystack.data() + i + 1, needle.data() + 1, needle.size() - 1))
            return i;
    }
    return notFound;
}

static void bytesRepeatInto(uint8_t* data, std::span<const uint8_t> source, size_t count)
{
    if (!count || source.empty())
        return;
    if (source.size() == 1) {
        memset(data, source[0], count);
        return;
    }
    size_t total = source.size() * count;
    memmove(data, source.data(), source.size());
    for (size_t done = source.size(); done < total;) {
        size_t chunk = std::min(done, total - done);
        memcpy(data + done, data, chunk);
        done += chunk;
    }
}

// ---- From other objects ----

static bool bytesItemValue(JSGlobalObject* globalObject, JSValue item, uint8_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t value = toSsize(globalObject, item, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    if (value < 0 || value >= 256) {
        raiseValueError(globalObject, scope, "bytes must be in range(0, 256)"_s);
        return false;
    }
    result = static_cast<uint8_t>(value);
    return true;
}

// bytes(object), for what is neither a str nor an int and has no __bytes__.
static JSValue bytesFromBufferOrIterable(JSGlobalObject* globalObject, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (bytesIsExact(globalObject, object))
        return object;
    if (isBytesLike(object)) {
        // The items of a memoryview need not follow one another.
        BufferHolder holder(globalObject);
        if (!holder.get(object, BufferFullRO))
            return { };
        JSValue result;
        uint8_t* data;
        if (!bytesAllocate(globalObject, false, holder.buffer.length, result, data))
            return { };
        copyBufferToContiguous(data, holder.buffer, 'C');
        return result;
    }

    Vector<uint8_t, 64> buffer;
    uint8_t byte;
    if (isExactList(object)) {
        JSArray* list = asList(object);
        for (unsigned i = 0; i < listLength(list); ++i) {
            if (!bytesItemValue(globalObject, listGet(list, i), byte))
                return { };
            buffer.append(byte);
        }
        RELEASE_AND_RETURN(scope, bytesMake(globalObject, false, buffer.span()));
    }
    if (auto* tuple = PyTuple::tryFrom(object); tuple && tuple->type() == rt.tupleType()) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (!bytesItemValue(globalObject, tuple->at(i), byte))
                return { };
            buffer.append(byte);
        }
        RELEASE_AND_RETURN(scope, bytesMake(globalObject, false, buffer.span()));
    }
    if (!isStr(object)) {
        JSValue iterator = getIter(globalObject, object);
        if (iterator) {
            int64_t hint = lengthHint(globalObject, object, 64);
            RETURN_IF_EXCEPTION(scope, { });
            if (hint > 0 && static_cast<uint64_t>(hint) <= (1u << 20))
                buffer.reserveCapacity(hint);
            while (true) {
                JSValue item = iterNext(globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, { });
                if (!item)
                    break;
                if (!bytesItemValue(globalObject, item, byte))
                    return { };
                buffer.append(byte);
            }
            RELEASE_AND_RETURN(scope, bytesMake(globalObject, false, buffer.span()));
        }
        if (!clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return { };
    }
    return raiseTypeError(globalObject, scope, "cannot convert '"_s, typeName(globalObject, object), "' object to bytes"_s);
}

// object.__bytes__(). Empty without an exception: it has no such method.
static JSValue bytesCallBytesMethod(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue method = typeOf(globalObject, object)->lookup(Special::Bytes);
    if (!method)
        return { };
    JSValue result = callSpecial(globalObject, method, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyBytes::is(result))
        return raiseTypeError(globalObject, scope, bytesFullTypeName(globalObject, object), ".__bytes__() must return a bytes, not "_s, bytesFullTypeName(globalObject, result));
    return result;
}

JSValue bytesFromObject(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (bytesIsExact(globalObject, object))
        return object;
    JSValue result = bytesCallBytesMethod(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    RELEASE_AND_RETURN(scope, bytesFromBufferOrIterable(globalObject, object));
}

namespace {

struct BytesConstructorArguments {
    JSValue source;
    JSString* encoding { nullptr };
    JSString* errors { nullptr };
};

}

// (source=..., encoding=..., errors=...)
static bool bytesParseConstructorArguments(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, BytesConstructorArguments& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount() > 3) {
        raiseTypeError(globalObject, scope, function, "() takes at most 3 arguments ("_s, args.totalCount(), " given)"_s);
        return false;
    }
    JSValue values[3] = { args.get(0), args.get(1), args.get(2) };
    static constexpr ASCIILiteral names[] = { "source"_s, "encoding"_s, "errors"_s };
    JSValue byName[3];
    String unexpected;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String keyword = stringOf(args.keywordName(i));
        unsigned index = 0;
        while (index < 3 && keyword != names[index])
            ++index;
        if (index < 3)
            byName[index] = args.keywordValue(i);
        else if (unexpected.isNull())
            unexpected = keyword;
    }
    for (unsigned index = 0; index < 3; ++index) {
        if (!byName[index])
            continue;
        if (values[index]) {
            raiseTypeError(globalObject, scope, "argument for "_s, function, "() given by name ('"_s, names[index], "') and position ("_s, index + 1, ')');
            return false;
        }
        values[index] = byName[index];
    }
    if (!unexpected.isNull()) {
        String suggestion = calculateSuggestion({ names[0], names[1], names[2] }, unexpected);
        if (suggestion.isNull())
            raiseTypeError(globalObject, scope, function, "() got an unexpected keyword argument '"_s, unexpected, '\'');
        else
            raiseTypeError(globalObject, scope, function, "() got an unexpected keyword argument '"_s, unexpected, "'. Did you mean '"_s, suggestion, "'?"_s);
        return false;
    }
    result.source = values[0];
    return bytesOptionalStr(globalObject, values[1], function, "encoding"_s, result.encoding) && bytesOptionalStr(globalObject, values[2], function, "errors"_s, result.errors);
}

static JSValue bytesNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    BytesConstructorArguments arguments;
    if (!bytesParseConstructorArguments(globalObject, "bytes"_s, args, arguments))
        return { };
    JSValue source = arguments.source;

    JSValue bytes;
    if (!source) {
        if (arguments.encoding || arguments.errors)
            return raiseTypeError(globalObject, scope, arguments.encoding ? "encoding without a string argument"_s : "errors without a string argument"_s);
        bytes = rt.emptyBytes();
    } else if (arguments.encoding) {
        if (!isStr(source))
            return raiseTypeError(globalObject, scope, "encoding without a string argument"_s);
        bytes = strEncode(globalObject, strValue(source), arguments.encoding, arguments.errors);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (arguments.errors)
        return raiseTypeError(globalObject, scope, isStr(source) ? "string argument without an encoding"_s : "errors without a string argument"_s);
    else {
        bytes = bytesCallBytesMethod(globalObject, source);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!bytes) {
        if (isStr(source))
            return raiseTypeError(globalObject, scope, "string argument without an encoding"_s);
        if (hasIndex(globalObject, source)) {
            int64_t size = toSsize(globalObject, source, rt.OverflowError());
            if (scope.exception()) {
                // An __index__ that only raises TypeError does not make the object an int.
                if (!clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                    return { };
            } else {
                if (size < 0)
                    return raiseValueError(globalObject, scope, "negative count"_s);
                uint8_t* data;
                if (!bytesAllocate(globalObject, false, size, bytes, data))
                    return { };
                if (size)
                    memset(data, 0, size);
            }
        }
        if (!bytes) {
            bytes = bytesFromBufferOrIterable(globalObject, source);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    if (type == rt.bytesType())
        return bytes;
    auto span = PyBytes::from(bytes)->span();
    PyBytes* result = PyBytes::tryCreateUninitialized(vm, rt.structure<PyBytes>(), type, span.size());
    if (!result)
        RELEASE_AND_RETURN(scope, bytesRaiseNoMemory(globalObject));
    if (!span.empty())
        memcpy(result->mutableData(), span.data(), span.size());
    ensureStillAliveHere(bytes);
    return result;
}

static bool byteArrayCanResize(JSGlobalObject* globalObject, PyByteArray* array)
{
    if (!isStillExported(globalObject, array, [&] { return array->exportCount(); })) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseBufferError(globalObject, scope, "Existing exports of data: object cannot be re-sized"_s);
    return false;
}

// bytearray.__init__. The source may be empty, the others null.
static bool byteArrayInitialize(JSGlobalObject* globalObject, PyByteArray* array, JSValue source, JSString* encoding, JSString* errors)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!byteArrayCanResize(globalObject, array) || !array->resize(globalObject, 0))
        return false;

    auto raiseWithoutString = [&] {
        raiseTypeError(globalObject, scope, encoding ? "encoding without a string argument"_s : "errors without a string argument"_s);
        return false;
    };
    if (!source)
        return encoding || errors ? raiseWithoutString() : true;
    if (isStr(source)) {
        if (!encoding) {
            raiseTypeError(globalObject, scope, "string argument without an encoding"_s);
            return false;
        }
        JSValue encoded = strEncode(globalObject, strValue(source), encoding, errors);
        RETURN_IF_EXCEPTION(scope, false);
        array->append(globalObject, PyBytes::from(encoded)->span());
        ensureStillAliveHere(encoded);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }
    if (encoding || errors)
        return raiseWithoutString();

    if (hasIndex(globalObject, source)) {
        int64_t count = toSsize(globalObject, source, rt.OverflowError());
        if (scope.exception()) {
            if (!clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                return false;
        } else {
            if (count < 0) {
                raiseValueError(globalObject, scope, "negative count"_s);
                return false;
            }
            RELEASE_AND_RETURN(scope, array->resize(globalObject, count));
        }
    }

    if (isBytesLike(source)) {
        BufferHolder holder(globalObject);
        if (!holder.get(source, BufferFullRO) || !array->resize(globalObject, holder.buffer.length))
            return false;
        copyBufferToContiguous(array->mutableSpan().data(), holder.buffer, 'C');
        return true;
    }

    // A list or a tuple of nothing but ints runs no code, so nothing can see the array grow.
    auto* tuple = PyTuple::tryFrom(source);
    if (isExactList(source) || (tuple && tuple->type() == rt.tupleType())) {
        unsigned size = tuple ? tuple->length() : listLength(asList(source));
        Vector<uint8_t, 64> buffer;
        buffer.reserveCapacity(size);
        for (unsigned i = 0; i < size; ++i) {
            JSValue item = tuple ? tuple->at(i) : listGet(asList(source), i);
            if (!isExactInt(item))
                break;
            uint8_t byte;
            if (!bytesByteValue(globalObject, item, byte))
                return false;
            buffer.append(byte);
        }
        if (buffer.size() == size) {
            array->append(globalObject, buffer.span());
            RETURN_IF_EXCEPTION(scope, false);
            return true;
        }
    }

    JSValue iterator = getIter(globalObject, source);
    if (!iterator) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            raiseTypeError(globalObject, scope, "cannot convert '"_s, typeName(globalObject, source), "' object to bytearray"_s);
        return false;
    }
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        uint8_t byte;
        if (!bytesByteValue(globalObject, item, byte))
            return false;
        // What is iterated over may be looking at the array, so it grows as it goes.
        if (!array->resize(globalObject, array->length() + 1))
            return false;
        array->mutableSpan().back() = byte;
    }
}

static JSValue byteArrayNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    return PyByteArray::create(rt.vm(), rt.structure<PyByteArray>(), PyType::from(cls));
}

static JSValue byteArrayInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    BytesConstructorArguments arguments;
    if (!bytesParseConstructorArguments(globalObject, "bytearray"_s, args, arguments))
        return { };
    if (!byteArrayInitialize(globalObject, PyByteArray::from(self), arguments.source, arguments.encoding, arguments.errors))
        return { };
    return jsUndefined();
}

// ---- The operations of any object ----

static JSValue bytesReprMethod(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto span = bytesSpanOf(self);
    bool isArray = bytesIsArray(self);
    if (span.size() > (String::MaxLength - 1024) / 4)
        return raiseOverflowError(globalObject, scope, isArray ? "bytearray object is too large to make repr"_s : "bytes object is too large to make repr"_s);
    String text = bytesRepr(span);
    if (!isArray)
        return makeStr(vm, text);
    return makeStr(vm, makeString(typeName(globalObject, self), '(', text, ')'));
}

static JSValue bytesHashMethod(JSGlobalObject* globalObject, JSValue self)
{
    return makeInt(globalObject, hashOfBytes(bytesSpanOf(self)));
}

template<CompareOperator op>
static JSValue bytesCompareMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    // bytes are compared with bytes; a bytearray, with anything that has bytes to show.
    if (bytesIsArray(self) ? !isBytesLike(other) : !PyBytes::is(other))
        return runtime(globalObject).notImplemented();
    std::span<const uint8_t> a;
    std::span<const uint8_t> b;
    bool selfRunsCode = bytesIsArray(self) && getBufferRunsCode(self);
    if ((selfRunsCode && !getBytesLike(globalObject, self, a)) || !getBytesLike(globalObject, other, b)) {
        takePendingException(globalObject);
        return runtime(globalObject).notImplemented();
    }
    if (!selfRunsCode)
        a = bytesSpanOf(self);
    if constexpr (op == CompareOperator::Eq || op == CompareOperator::NotEq) {
        bool equal = a.size() == b.size() && (a.empty() || !memcmp(a.data(), b.data(), a.size()));
        return jsBoolean(equal == (op == CompareOperator::Eq));
    }
    size_t common = std::min(a.size(), b.size());
    int order = common ? memcmp(a.data(), b.data(), common) : 0;
    if (!order)
        order = a.size() < b.size() ? -1 : a.size() > b.size();
    switch (op) {
    case CompareOperator::Lt:
        return jsBoolean(order < 0);
    case CompareOperator::LtE:
        return jsBoolean(order <= 0);
    case CompareOperator::Gt:
        return jsBoolean(order > 0);
    default:
        return jsBoolean(order >= 0);
    }
}

// ---- The operations of a sequence ----

static JSValue bytesLenMethod(JSGlobalObject*, JSValue self) { return bytesNumber(bytesSpanOf(self).size()); }

static JSValue bytesIter(JSGlobalObject* globalObject, JSValue self)
{
    return PyIterator::create(globalObject, bytesIsArray(self) ? PyIterator::Kind::ByteArray : PyIterator::Kind::Bytes, self);
}

static JSValue bytesGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isArray = bytesIsArray(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        auto span = bytesSpanOf(self);
        if (index < 0)
            index += span.size();
        if (index < 0 || index >= static_cast<int64_t>(span.size()))
            return raiseIndexError(globalObject, scope, isArray ? "bytearray index out of range"_s : "index out of range"_s);
        return jsNumber(span[index]);
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, isArray ? "bytearray"_s : "byte"_s, " indices must be integers or slices, not "_s, typeName(globalObject, key));
    PySlice::Indices indices;
    if (!bytesSliceIndices(globalObject, self, slice, indices))
        return { };
    if (indices.length <= 0)
        RELEASE_AND_RETURN(scope, bytesMake(globalObject, isArray, { }));
    if (indices.step == 1)
        RELEASE_AND_RETURN(scope, bytesPart(globalObject, self, indices.start, indices.start + indices.length));
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, isArray, indices.length, result, data))
        return { };
    auto span = bytesSpanOf(self);
    for (int64_t i = 0; i < indices.length; ++i)
        data[i] = span[indices.start + i * indices.step];
    return result;
}

static JSValue bytesContains(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasIndex(globalObject, item)) {
        int64_t value;
        {
            BytesExportScope exportScope(self);
            value = toSsize(globalObject, item, nullptr);
        }
        if (!scope.exception()) {
            if (value < 0 || value >= 256)
                return raiseValueError(globalObject, scope, "byte must be in range(0, 256)"_s);
            auto span = bytesSpanOf(self);
            return jsBoolean(!span.empty() && memchr(span.data(), static_cast<int>(value), span.size()));
        }
        // Whatever went wrong, it may still have bytes to look for.
        takePendingException(globalObject);
    }
    std::span<const uint8_t> needle;
    if (!getBytesLike(globalObject, item, needle))
        return { };
    return jsBoolean(bytesSearch(bytesSpanOf(self), needle) != notFound);
}

static JSValue bytesAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const uint8_t> a;
    std::span<const uint8_t> b;
    bool selfRunsCode = getBufferRunsCode(self);
    if ((selfRunsCode && !getBytesLike(globalObject, self, a)) || !isBytesLike(other) || !getBytesLike(globalObject, other, b)) {
        takePendingException(globalObject);
        return raiseTypeError(globalObject, scope, "can't concat "_s, typeName(globalObject, other), " to "_s, typeName(globalObject, self));
    }
    bool isArray = bytesIsArray(self);
    if (!selfRunsCode)
        a = bytesSpanOf(self);
    if (!isArray) {
        if (a.empty() && bytesIsExact(globalObject, other))
            return other;
        if (b.empty() && bytesIsExact(globalObject, self))
            return self;
    }
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, isArray, a.size() + b.size(), result, data))
        return { };
    if (!a.empty())
        memcpy(data, a.data(), a.size());
    if (!b.empty())
        memcpy(data + a.size(), b.data(), b.size());
    return result;
}

static JSValue bytesMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = toSsize(globalObject, countValue, rt.OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    count = std::max<int64_t>(count, 0);
    bool isArray = bytesIsArray(self);
    auto span = bytesSpanOf(self);
    if (count && span.size() > static_cast<uint64_t>(bytesMaxSsize / count)) {
        if (isArray)
            RELEASE_AND_RETURN(scope, bytesRaiseNoMemory(globalObject));
        return raiseOverflowError(globalObject, scope, "repeated bytes are too long"_s);
    }
    size_t total = span.size() * count;
    if (total == span.size() && bytesIsExact(globalObject, self))
        return self;
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, isArray, total, result, data))
        return { };
    bytesRepeatInto(data, span, count);
    return result;
}

static JSValue bytesMod(JSGlobalObject* globalObject, JSValue self, JSValue arguments)
{
    return bytesFormatPercent(globalObject, bytesSpanOf(self), arguments, bytesIsArray(self));
}

static JSValue bytesRMod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    bool isArray = bytesIsArray(self);
    if (isArray ? !PyByteArray::is(other) : !PyBytes::is(other))
        return runtime(globalObject).notImplemented();
    return bytesFormatPercent(globalObject, bytesSpanOf(other), self, isArray);
}

static JSValue bytesBytesMethod(JSGlobalObject* globalObject, JSValue self) { return bytesSelfOrCopy(globalObject, self); }

static JSValue bytesGetNewArgs(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue copy = bytesMake(globalObject, false, bytesSpanOf(self));
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { copy });
}

// ---- Searching ----

// The `sub` argument of find() and the like: bytes, or one byte given as an int, which is then kept in `byte`.
// What to look for in `self`.
static bool bytesSubArgument(JSGlobalObject* globalObject, JSValue self, JSValue value, std::span<const uint8_t>& sub, uint8_t& byte)
{
    BytesExportScope exportScope(self);
    if (isBytesLike(value))
        return getBytesLike(globalObject, value, sub);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasIndex(globalObject, value)) {
        raiseTypeError(globalObject, scope, "argument should be integer or bytes-like object, not '"_s, typeName(globalObject, value), '\'');
        return false;
    }
    int64_t number = toSsize(globalObject, value, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    if (number < 0 || number > 255) {
        raiseValueError(globalObject, scope, "byte must be in range(0, 256)"_s);
        return false;
    }
    byte = static_cast<uint8_t>(number);
    sub = { &byte, 1 };
    return true;
}

template<bool reverse, bool raises>
static JSValue bytesFindMethod(JSGlobalObject* globalObject, JSValue self, JSValue subValue, JSValue start, JSValue end)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t from;
    int64_t to;
    if (!unpackSliceBounds(globalObject, start, end, from, to))
        return { };
    std::span<const uint8_t> sub;
    uint8_t byte;
    if (!bytesSubArgument(globalObject, self, subValue, sub, byte))
        return { };
    auto whole = bytesSpanOf(self);
    adjustSliceBounds(whole.size(), from, to);
    int64_t result = -1;
    if (to - from >= static_cast<int64_t>(sub.size())) {
        auto part = whole.subspan(from, to - from);
        size_t position = reverse ? bytesSearchBackward(part, sub) : bytesSearch(part, sub);
        if (position != notFound)
            result = from + position;
    }
    if (raises && result < 0)
        return raiseValueError(globalObject, scope, "subsection not found"_s);
    return bytesNumber(result);
}

static JSValue bytesCount(JSGlobalObject* globalObject, JSValue self, JSValue subValue, JSValue start, JSValue end)
{
    int64_t from;
    int64_t to;
    if (!unpackSliceBounds(globalObject, start, end, from, to))
        return { };
    std::span<const uint8_t> sub;
    uint8_t byte;
    if (!bytesSubArgument(globalObject, self, subValue, sub, byte))
        return { };
    auto whole = bytesSpanOf(self);
    adjustSliceBounds(whole.size(), from, to);
    if (to < from)
        return jsNumber(0);
    if (sub.empty())
        return bytesNumber(to - from + 1);
    auto part = whole.subspan(from, to - from);
    int64_t count = 0;
    while (true) {
        size_t position = bytesSearch(part, sub);
        if (position == notFound)
            break;
        ++count;
        part = part.subspan(position + sub.size());
    }
    return bytesNumber(count);
}

template<bool atEnd>
static bool bytesTailMatches(std::span<const uint8_t> whole, std::span<const uint8_t> affix, int64_t from, int64_t to)
{
    int64_t length = whole.size();
    int64_t affixLength = affix.size();
    to = std::min(to, length);
    if constexpr (atEnd) {
        if (to - from < affixLength || from > length)
            return false;
        from = std::max(from, to - affixLength);
    } else if (from > length - affixLength)
        return false;
    if (to - from < affixLength)
        return false;
    return !affixLength || !memcmp(whole.data() + from, affix.data(), affixLength);
}

template<bool atEnd>
static JSValue bytesStartsOrEndsWith(JSGlobalObject* globalObject, JSValue self, JSValue affixValue, JSValue start, JSValue end)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t from;
    int64_t to;
    if (!unpackSliceBounds(globalObject, start, end, from, to))
        return { };
    adjustSliceBounds(bytesSpanOf(self).size(), from, to);
    std::span<const uint8_t> affix;
    if (auto* tuple = PyTuple::tryFrom(affixValue)) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (!getBytesLike(globalObject, tuple->at(i), affix))
                return { };
            if (bytesTailMatches<atEnd>(bytesSpanOf(self), affix, from, to))
                return jsBoolean(true);
        }
        return jsBoolean(false);
    }
    if (!isBytesLike(affixValue))
        return raiseTypeError(globalObject, scope, atEnd ? "endswith"_s : "startswith"_s, " first arg must be bytes or a tuple of bytes, not "_s, typeName(globalObject, affixValue));
    if (!getBytesLike(globalObject, affixValue, affix))
        return { };
    return jsBoolean(bytesTailMatches<atEnd>(bytesSpanOf(self), affix, from, to));
}

static JSValue bytesRemovePrefix(JSGlobalObject* globalObject, JSValue self, JSValue prefixValue)
{
    std::span<const uint8_t> prefix;
    if (!getBytesLike(globalObject, prefixValue, prefix))
        return { };
    auto span = bytesSpanOf(self);
    if (span.size() >= prefix.size() && (prefix.empty() || !memcmp(span.data(), prefix.data(), prefix.size())))
        return bytesPart(globalObject, self, prefix.size(), span.size());
    return bytesSelfOrCopy(globalObject, self);
}

static JSValue bytesRemoveSuffix(JSGlobalObject* globalObject, JSValue self, JSValue suffixValue)
{
    std::span<const uint8_t> suffix;
    if (!getBytesLike(globalObject, suffixValue, suffix))
        return { };
    auto span = bytesSpanOf(self);
    if (span.size() >= suffix.size() && (suffix.empty() || !memcmp(span.data() + span.size() - suffix.size(), suffix.data(), suffix.size())))
        return bytesPart(globalObject, self, 0, span.size() - suffix.size());
    return bytesSelfOrCopy(globalObject, self);
}

// ---- Splitting and joining ----

using BytesRanges = Vector<std::pair<size_t, size_t>, 16>;

// A list of those parts of `self`.
static JSValue bytesListOfParts(JSGlobalObject* globalObject, JSValue self, const BytesRanges& ranges, bool reversed)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = newList(globalObject, static_cast<unsigned>(std::min<size_t>(ranges.size(), 1u << 20)));
    RETURN_IF_EXCEPTION(scope, { });
    for (size_t i = 0; i < ranges.size(); ++i) {
        auto& range = ranges[reversed ? ranges.size() - 1 - i : i];
        JSValue part = bytesPart(globalObject, self, range.first, range.second);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, part);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

// The arguments of split() and rsplit(). An empty span with `onWhitespace` unset is never returned.
static bool bytesSplitArguments(JSGlobalObject* globalObject, JSValue separatorValue, JSValue maxSplitValue, bool& onWhitespace, std::span<const uint8_t>& separator, int64_t& maxSplit)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    maxSplit = -1;
    if (maxSplitValue && !bytesSsizeArgument(globalObject, maxSplitValue, maxSplit))
        return false;
    if (maxSplit < 0)
        maxSplit = bytesMaxSsize;
    onWhitespace = !separatorValue || isNone(separatorValue);
    if (onWhitespace)
        return true;
    if (!getBytesLike(globalObject, separatorValue, separator))
        return false;
    if (separator.empty()) {
        raiseValueError(globalObject, scope, "empty separator"_s);
        return false;
    }
    return true;
}

static JSValue bytesSplit(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue, JSValue maxSplitValue)
{
    bool onWhitespace;
    std::span<const uint8_t> separator;
    int64_t maxSplit;
    if (!bytesSplitArguments(globalObject, separatorValue, maxSplitValue, onWhitespace, separator, maxSplit))
        return { };
    auto span = bytesSpanOf(self);
    size_t length = span.size();
    BytesRanges ranges;
    size_t i = 0;
    if (onWhitespace) {
        while (maxSplit-- > 0) {
            while (i < length && bytesIsSpace(span[i]))
                ++i;
            if (i == length)
                break;
            size_t start = i++;
            while (i < length && !bytesIsSpace(span[i]))
                ++i;
            ranges.append({ start, i });
        }
        if (i < length) {
            // There are to be no more splits. What is left is one part, without the whitespace before it.
            while (i < length && bytesIsSpace(span[i]))
                ++i;
            if (i != length)
                ranges.append({ i, length });
        }
    } else {
        while (maxSplit-- > 0) {
            size_t position = bytesSearch(span.subspan(i), separator);
            if (position == notFound)
                break;
            ranges.append({ i, i + position });
            i += position + separator.size();
        }
        ranges.append({ i, length });
    }
    return bytesListOfParts(globalObject, self, ranges, false);
}

static JSValue bytesRSplit(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue, JSValue maxSplitValue)
{
    bool onWhitespace;
    std::span<const uint8_t> separator;
    int64_t maxSplit;
    if (!bytesSplitArguments(globalObject, separatorValue, maxSplitValue, onWhitespace, separator, maxSplit))
        return { };
    auto span = bytesSpanOf(self);
    BytesRanges ranges;
    size_t end = span.size();
    if (onWhitespace) {
        while (maxSplit-- > 0) {
            while (end && bytesIsSpace(span[end - 1]))
                --end;
            if (!end)
                break;
            size_t stop = end--;
            while (end && !bytesIsSpace(span[end - 1]))
                --end;
            ranges.append({ end, stop });
        }
        if (end) {
            while (end && bytesIsSpace(span[end - 1]))
                --end;
            if (end)
                ranges.append({ 0, end });
        }
    } else {
        while (maxSplit-- > 0) {
            size_t position = bytesSearchBackward(span.first(end), separator);
            if (position == notFound)
                break;
            ranges.append({ position + separator.size(), end });
            end = position;
        }
        ranges.append({ 0, end });
    }
    return bytesListOfParts(globalObject, self, ranges, true);
}

static JSValue bytesSplitLines(JSGlobalObject* globalObject, JSValue self, JSValue keepEndsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool keepEnds = false;
    if (keepEndsValue) {
        keepEnds = isTrue(globalObject, keepEndsValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto span = bytesSpanOf(self);
    size_t length = span.size();
    BytesRanges ranges;
    for (size_t i = 0; i < length;) {
        size_t start = i;
        while (i < length && span[i] != '\n' && span[i] != '\r')
            ++i;
        size_t stop = i;
        if (i < length) {
            i += span[i] == '\r' && i + 1 < length && span[i + 1] == '\n' ? 2 : 1;
            if (keepEnds)
                stop = i;
        }
        ranges.append({ start, stop });
    }
    RELEASE_AND_RETURN(scope, bytesListOfParts(globalObject, self, ranges, false));
}

template<bool reverse>
static JSValue bytesPartition(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const uint8_t> separator;
    if (!getBytesLike(globalObject, separatorValue, separator))
        return { };
    if (separator.empty())
        return raiseValueError(globalObject, scope, "empty separator"_s);
    bool isArray = bytesIsArray(self);
    auto span = bytesSpanOf(self);
    size_t position = reverse ? bytesSearchBackward(span, separator) : bytesSearch(span, separator);

    JSValue parts[3];
    if (position == notFound) {
        // bytes gives itself, even where it is an instance of a derived class.
        JSValue whole = isArray ? bytesMake(globalObject, true, span) : self;
        RETURN_IF_EXCEPTION(scope, { });
        JSValue first = bytesMake(globalObject, isArray, { });
        RETURN_IF_EXCEPTION(scope, { });
        JSValue second = bytesMake(globalObject, isArray, { });
        RETURN_IF_EXCEPTION(scope, { });
        parts[reverse ? 2 : 0] = whole;
        parts[1] = first;
        parts[reverse ? 0 : 2] = second;
    } else {
        size_t separatorLength = separator.size();
        parts[0] = bytesMake(globalObject, isArray, span.first(position));
        RETURN_IF_EXCEPTION(scope, { });
        parts[1] = isArray ? bytesMake(globalObject, true, separator) : separatorValue;
        RETURN_IF_EXCEPTION(scope, { });
        parts[2] = bytesMake(globalObject, isArray, span.subspan(position + separatorLength));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return PyTuple::create(globalObject, std::span<const JSValue> { parts, 3 });
}

// For when the __buffer__() of an item is written in Python. It may do anything to the other items, so the memory of each is
// held until the end.
static JSValue bytesJoinHoldingBuffers(JSGlobalObject* globalObject, JSValue self, const MarkedArgumentBuffer& items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = items.size();
    Vector<Buffer> buffers(count);
    // What the buffers refer to: the collector does not look into the vector.
    MarkedArgumentBuffer referents;
    auto releaseAll = [&] {
        for (auto& buffer : buffers)
            releaseBuffer(globalObject, buffer);
    };
    for (unsigned i = 0; i < count; ++i) {
        Buffer& buffer = buffers[i];
        if (!getBuffer(globalObject, items.at(i), buffer)) {
            takePendingException(globalObject);
            releaseAll();
            return raiseTypeError(globalObject, scope, "sequence item "_s, i, ": expected a bytes-like object, "_s, typeName(globalObject, items.at(i)), " found"_s);
        }
        referents.append(buffer.object);
        referents.append(buffer.view ? buffer.view : jsUndefined());
        referents.append(buffer.owner ? JSValue(buffer.owner) : jsUndefined());
    }
    auto separator = bytesSpanOf(self);
    uint64_t total = 0;
    JSValue result;
    uint8_t* data = nullptr;
    for (unsigned i = 0; i < count; ++i) {
        total += buffers[i].length + (i ? separator.size() : 0);
        if (!refreshBuffer(globalObject, buffers[i]) || total > static_cast<uint64_t>(bytesMaxSsize)) {
            if (!scope.exception())
                raiseOverflowError(globalObject, scope, "join() result is too long"_s);
            releaseAll();
            return { };
        }
    }
    if (!bytesAllocate(globalObject, bytesIsArray(self), total, result, data)) {
        releaseAll();
        return { };
    }
    for (unsigned i = 0; i < count; ++i) {
        if (i && !separator.empty()) {
            memcpy(data, separator.data(), separator.size());
            data += separator.size();
        }
        if (buffers[i].length) {
            memcpy(data, buffers[i].data, buffers[i].length);
            data += buffers[i].length;
        }
    }
    releaseAll();
    return result;
}

static JSValue bytesJoin(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isArray = bytesIsArray(self);

    // The code of an iterator cannot resize the separator.
    if (isArray)
        PyByteArray::from(self)->addExport();
    auto removeExport = makeScopeExit([&] {
        if (isArray)
            PyByteArray::from(self)->removeExport();
    });

    auto* tuple = PyTuple::tryFrom(iterable);
    if (tuple && tuple->type() != rt.tupleType())
        tuple = nullptr;
    JSArray* list = isExactList(iterable) ? asList(iterable) : nullptr;
    if (!tuple && !list) {
        JSValue iterator = getIter(globalObject, iterable);
        if (!iterator) {
            if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, "can only join an iterable"_s);
            return { };
        }
        list = listFromIterable(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
    }
    unsigned count = tuple ? tuple->length() : listLength(list);
    auto itemAt = [&](unsigned index) { return tuple ? tuple->at(index) : listGet(list, index); };
    auto raiseNotBytesLike = [&](unsigned index, JSValue item) {
        takePendingException(globalObject);
        return raiseTypeError(globalObject, scope, "sequence item "_s, index, ": expected a bytes-like object, "_s, typeName(globalObject, item), " found"_s);
    };
    bool runsCode = false;
    for (unsigned i = 0; i < count && !runsCode; ++i)
        runsCode = getBufferRunsCode(itemAt(i));
    if (runsCode) {
        MarkedArgumentBuffer items;
        for (unsigned i = 0; i < count; ++i)
            items.append(itemAt(i));
        RELEASE_AND_RETURN(scope, bytesJoinHoldingBuffers(globalObject, self, items));
    }
    // Nothing from here on runs any code, so neither the items nor their bytes change.
    if (!count)
        RELEASE_AND_RETURN(scope, bytesMake(globalObject, isArray, { }));
    if (count == 1 && !isArray && bytesIsExact(globalObject, itemAt(0)))
        return itemAt(0);

    auto separator = bytesSpanOf(self);
    size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        JSValue item = itemAt(i);
        std::span<const uint8_t> span;
        if (!isBytesLike(item) || !getBytesLike(globalObject, item, span))
            return raiseNotBytesLike(i, item);
        total += span.size() + (i ? separator.size() : 0);
        if (total > static_cast<uint64_t>(bytesMaxSsize))
            return raiseOverflowError(globalObject, scope, "join() result is too long"_s);
    }
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, isArray, total, result, data))
        return { };
    for (unsigned i = 0; i < count; ++i) {
        if (i && !separator.empty()) {
            memcpy(data, separator.data(), separator.size());
            data += separator.size();
        }
        std::span<const uint8_t> span;
        bool ok = getBytesLike(globalObject, itemAt(i), span);
        ASSERT_UNUSED(ok, ok);
        if (!span.empty()) {
            memcpy(data, span.data(), span.size());
            data += span.size();
        }
    }
    return result;
}

// ---- Replacing ----

static JSValue bytesReplace(JSGlobalObject* globalObject, JSValue self, JSValue oldValue, JSValue newValue, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const uint8_t> from;
    std::span<const uint8_t> to;
    if (!getBytesLike(globalObject, oldValue, from) || !getBytesLike(globalObject, newValue, to))
        return { };
    int64_t maxCount = -1;
    if (countValue) {
        if (!bytesSsizeArgument(globalObject, countValue, maxCount))
            return { };
        // That may have run code.
        if (!getBytesLike(globalObject, oldValue, from) || !getBytesLike(globalObject, newValue, to))
            return { };
    }
    bool isArray = bytesIsArray(self);
    auto span = bytesSpanOf(self);
    if (span.size() < from.size() || !maxCount || (from.empty() && to.empty()))
        RELEASE_AND_RETURN(scope, bytesSelfOrCopy(globalObject, self));
    uint64_t limit = maxCount < 0 ? bytesMaxSsize : maxCount;

    JSValue result;
    uint8_t* data;
    auto write = [&](std::span<const uint8_t> bytes) {
        if (bytes.empty())
            return;
        memcpy(data, bytes.data(), bytes.size());
        data += bytes.size();
    };

    if (from.empty()) {
        // b"ab".replace(b"", b".") is b".a.b."
        size_t count = std::min<uint64_t>(span.size() + 1, limit);
        if (to.size() > (static_cast<uint64_t>(bytesMaxSsize) - span.size()) / count)
            return raiseOverflowError(globalObject, scope, "replace bytes is too long"_s);
        if (!bytesAllocate(globalObject, isArray, span.size() + count * to.size(), result, data))
            return { };
        write(to);
        for (size_t i = 0; i + 1 < count; ++i) {
            *data++ = span[i];
            write(to);
        }
        write(span.subspan(count - 1));
        return result;
    }

    size_t count = 0;
    for (auto rest = span; count < limit; ++count) {
        size_t position = bytesSearch(rest, from);
        if (position == notFound)
            break;
        rest = rest.subspan(position + from.size());
    }
    if (!count)
        RELEASE_AND_RETURN(scope, bytesSelfOrCopy(globalObject, self));
    if (to.size() > from.size() && to.size() - from.size() > (static_cast<uint64_t>(bytesMaxSsize) - span.size()) / count)
        return raiseOverflowError(globalObject, scope, "replace bytes is too long"_s);
    if (!bytesAllocate(globalObject, isArray, span.size() - count * from.size() + count * to.size(), result, data))
        return { };
    auto rest = span;
    for (size_t i = 0; i < count; ++i) {
        size_t position = bytesSearch(rest, from);
        write(rest.first(position));
        write(to);
        rest = rest.subspan(position + from.size());
    }
    write(rest);
    return result;
}

static JSValue bytesTranslate(JSGlobalObject* globalObject, JSValue self, JSValue tableValue, JSValue deleteValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What each byte becomes. -1: nothing.
    std::array<int16_t, 256> map;
    if (isNone(tableValue)) {
        for (unsigned i = 0; i < 256; ++i)
            map[i] = i;
    } else {
        std::span<const uint8_t> table;
        if (!getBytesLike(globalObject, tableValue, table))
            return { };
        if (table.size() != 256)
            return raiseValueError(globalObject, scope, "translation table must be 256 characters long"_s);
        for (unsigned i = 0; i < 256; ++i)
            map[i] = table[i];
    }
    if (deleteValue) {
        std::span<const uint8_t> deleted;
        if (!getBytesLike(globalObject, deleteValue, deleted))
            return { };
        for (uint8_t byte : deleted)
            map[byte] = -1;
    }

    auto span = bytesSpanOf(self);
    size_t length = 0;
    bool changed = false;
    for (uint8_t byte : span) {
        length += map[byte] >= 0;
        changed |= map[byte] != byte;
    }
    if (!changed && bytesIsExact(globalObject, self))
        return self;
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, bytesIsArray(self), length, result, data))
        return { };
    for (uint8_t byte : span) {
        if (map[byte] >= 0)
            *data++ = static_cast<uint8_t>(map[byte]);
    }
    return result;
}

static JSValue bytesMakeTrans(JSGlobalObject* globalObject, JSValue fromValue, JSValue toValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const uint8_t> from;
    std::span<const uint8_t> to;
    if (!getBytesLike(globalObject, fromValue, from) || !getBytesLike(globalObject, toValue, to))
        return { };
    if (from.size() != to.size())
        return raiseValueError(globalObject, scope, "maketrans arguments must have same length"_s);
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, false, 256, result, data))
        return { };
    for (unsigned i = 0; i < 256; ++i)
        data[i] = i;
    for (size_t i = 0; i < from.size(); ++i)
        data[from[i]] = to[i];
    return result;
}

// ---- Stripping and padding ----

enum class BytesStripSide : uint8_t { Left, Right, Both };

template<BytesStripSide side>
static JSValue bytesStrip(JSGlobalObject* globalObject, JSValue self, JSValue bytesValue)
{
    std::array<bool, 256> strips { };
    if (!bytesValue || isNone(bytesValue)) {
        for (unsigned i = 0; i < 256; ++i)
            strips[i] = bytesIsSpace(i);
    } else {
        std::span<const uint8_t> bytes;
        if (!getBytesLike(globalObject, bytesValue, bytes))
            return { };
        for (uint8_t byte : bytes)
            strips[byte] = true;
    }
    auto span = bytesSpanOf(self);
    size_t from = 0;
    size_t to = span.size();
    if constexpr (side != BytesStripSide::Right) {
        while (from < to && strips[span[from]])
            ++from;
    }
    if constexpr (side != BytesStripSide::Left) {
        while (to > from && strips[span[to - 1]])
            --to;
    }
    return bytesPart(globalObject, self, from, to);
}

static JSValue bytesPad(JSGlobalObject* globalObject, JSValue self, int64_t left, int64_t right, uint8_t fill)
{
    left = std::max<int64_t>(left, 0);
    right = std::max<int64_t>(right, 0);
    if (!left && !right)
        return bytesSelfOrCopy(globalObject, self);
    auto span = bytesSpanOf(self);
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, bytesIsArray(self), span.size() + static_cast<size_t>(left) + static_cast<size_t>(right), result, data))
        return { };
    memset(data, fill, left);
    if (!span.empty())
        memcpy(data + left, span.data(), span.size());
    memset(data + left + span.size(), fill, right);
    return result;
}

enum class BytesJustify : uint8_t { Left, Right, Center };

template<BytesJustify justify>
static JSValue bytesJustifyMethod(JSGlobalObject* globalObject, JSValue self, JSValue widthValue, JSValue fillValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral function = justify == BytesJustify::Left ? "ljust"_s : justify == BytesJustify::Right ? "rjust"_s : "center"_s;
    int64_t width;
    if (!bytesSsizeArgument(globalObject, widthValue, width))
        return { };
    uint8_t fill = ' ';
    if (fillValue) {
        if (!PyBytes::is(fillValue) && !PyByteArray::is(fillValue))
            return raiseTypeError(globalObject, scope, function, "() argument 2 must be a byte string of length 1, not "_s, bytesArgumentTypeName(globalObject, fillValue));
        auto span = bytesSpanOf(fillValue);
        if (span.size() != 1)
            return raiseTypeError(globalObject, scope, function, "(): argument 2 must be a byte string of length 1, not a "_s, bytesIsArray(fillValue) ? "bytearray"_s : "bytes"_s, " object of length "_s, span.size());
        fill = span[0];
    }
    int64_t margin = width - static_cast<int64_t>(bytesSpanOf(self).size());
    if (margin <= 0)
        RELEASE_AND_RETURN(scope, bytesSelfOrCopy(globalObject, self));
    scope.release();
    if constexpr (justify == BytesJustify::Left)
        return bytesPad(globalObject, self, 0, margin, fill);
    if constexpr (justify == BytesJustify::Right)
        return bytesPad(globalObject, self, margin, 0, fill);
    int64_t left = margin / 2 + (margin & width & 1);
    return bytesPad(globalObject, self, left, margin - left, fill);
}

static JSValue bytesZFill(JSGlobalObject* globalObject, JSValue self, JSValue widthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t width;
    if (!bytesSsizeArgument(globalObject, widthValue, width))
        return { };
    int64_t fill = width - static_cast<int64_t>(bytesSpanOf(self).size());
    if (fill <= 0)
        RELEASE_AND_RETURN(scope, bytesSelfOrCopy(globalObject, self));
    JSValue result = bytesPad(globalObject, self, fill, 0, '0');
    RETURN_IF_EXCEPTION(scope, { });
    // The sign stays in front. The result is new, so it can be written to whichever kind it is.
    uint8_t* data = const_cast<uint8_t*>(bytesSpanOf(result).data());
    if (data[fill] == '+' || data[fill] == '-') {
        data[0] = data[fill];
        data[fill] = '0';
    }
    return result;
}

static JSValue bytesExpandTabs(JSGlobalObject* globalObject, JSValue self, JSValue tabSizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int tabSize = 8;
    if (tabSizeValue && !bytesIntArgument(globalObject, tabSizeValue, tabSize))
        return { };
    auto span = bytesSpanOf(self);
    // The length of the lines that have ended, and the column in the one that has not.
    uint64_t total = 0;
    uint64_t column = 0;
    for (uint8_t byte : span) {
        if (byte == '\t') {
            if (tabSize > 0)
                column += tabSize - column % tabSize;
        } else {
            ++column;
            if (byte == '\n' || byte == '\r') {
                total += column;
                column = 0;
            }
        }
        if (total + column > static_cast<uint64_t>(bytesMaxSsize))
            return raiseOverflowError(globalObject, scope, "result too long"_s);
    }
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, bytesIsArray(self), total + column, result, data))
        return { };
    column = 0;
    for (uint8_t byte : span) {
        if (byte == '\t') {
            if (tabSize > 0) {
                uint64_t count = tabSize - column % tabSize;
                column += count;
                memset(data, ' ', count);
                data += count;
            }
        } else {
            ++column;
            *data++ = byte;
            if (byte == '\n' || byte == '\r')
                column = 0;
        }
    }
    return result;
}

// ---- Case, which only ASCII letters have ----

// A copy with each byte replaced by function(byte, index).
template<typename Function>
static JSValue bytesMapped(JSGlobalObject* globalObject, JSValue self, const Function& function)
{
    auto span = bytesSpanOf(self);
    JSValue result;
    uint8_t* data;
    if (!bytesAllocate(globalObject, bytesIsArray(self), span.size(), result, data))
        return { };
    for (size_t i = 0; i < span.size(); ++i)
        data[i] = function(span[i], i);
    return result;
}

static JSValue bytesLower(JSGlobalObject* globalObject, JSValue self)
{
    return bytesMapped(globalObject, self, [](uint8_t byte, size_t) { return bytesToLower(byte); });
}

static JSValue bytesUpper(JSGlobalObject* globalObject, JSValue self)
{
    return bytesMapped(globalObject, self, [](uint8_t byte, size_t) { return bytesToUpper(byte); });
}

static JSValue bytesSwapCase(JSGlobalObject* globalObject, JSValue self)
{
    return bytesMapped(globalObject, self, [](uint8_t byte, size_t) -> uint8_t { return bytesIsAlpha(byte) ? byte ^ 0x20 : byte; });
}

static JSValue bytesCapitalize(JSGlobalObject* globalObject, JSValue self)
{
    return bytesMapped(globalObject, self, [](uint8_t byte, size_t index) { return index ? bytesToLower(byte) : bytesToUpper(byte); });
}

static JSValue bytesTitle(JSGlobalObject* globalObject, JSValue self)
{
    bool previousIsCased = false;
    return bytesMapped(globalObject, self, [&](uint8_t byte, size_t) {
        bool isCased = bytesIsAlpha(byte);
        uint8_t result = !isCased ? byte : previousIsCased ? bytesToLower(byte) : bytesToUpper(byte);
        previousIsCased = isCased;
        return result;
    });
}

// Whether there are bytes and the predicate holds of them all.
template<bool (*predicate)(uint8_t)>
static JSValue bytesAll(JSGlobalObject*, JSValue self)
{
    auto span = bytesSpanOf(self);
    for (uint8_t byte : span) {
        if (!predicate(byte))
            return jsBoolean(false);
    }
    return jsBoolean(!span.empty());
}

static bool bytesSpacePredicate(uint8_t byte) { return bytesIsSpace(byte); }
static bool bytesAlphaPredicate(uint8_t byte) { return bytesIsAlpha(byte); }
static bool bytesDecimalPredicate(uint8_t byte) { return bytesIsDecimal(byte); }
static bool bytesAlphanumericPredicate(uint8_t byte) { return bytesIsAlphanumeric(byte); }

static JSValue bytesIsASCIIMethod(JSGlobalObject*, JSValue self)
{
    for (uint8_t byte : bytesSpanOf(self)) {
        if (byte >= 0x80)
            return jsBoolean(false);
    }
    return jsBoolean(true);
}

template<bool upper>
static JSValue bytesIsOfCase(JSGlobalObject*, JSValue self)
{
    bool hasCased = false;
    for (uint8_t byte : bytesSpanOf(self)) {
        if (upper ? bytesIsLower(byte) : bytesIsUpper(byte))
            return jsBoolean(false);
        hasCased |= bytesIsAlpha(byte);
    }
    return jsBoolean(hasCased);
}

static JSValue bytesIsTitle(JSGlobalObject*, JSValue self)
{
    bool hasCased = false;
    bool previousIsCased = false;
    for (uint8_t byte : bytesSpanOf(self)) {
        if (bytesIsUpper(byte) ? previousIsCased : bytesIsLower(byte) && !previousIsCased)
            return jsBoolean(false);
        previousIsCased = bytesIsAlpha(byte);
        hasCased |= previousIsCased;
    }
    return jsBoolean(hasCased);
}

// ---- To and from text ----

static JSValue bytesDecodeMethod(JSGlobalObject* globalObject, JSValue self, JSValue encodingValue, JSValue errorsValue)
{
    JSString* encoding;
    JSString* errors;
    if (!bytesOptionalStr(globalObject, encodingValue, "decode"_s, "encoding"_s, encoding) || !bytesOptionalStr(globalObject, errorsValue, "decode"_s, "errors"_s, errors))
        return { };
    return bytesDecode(globalObject, self, encoding, errors);
}

static JSValue bytesHex(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue, JSValue bytesPerSeparatorValue)
{
    static constexpr char digits[] = "0123456789abcdef";
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t group = 1;
    if (bytesPerSeparatorValue && !bytesSsizeArgument(globalObject, bytesPerSeparatorValue, group))
        return { };
    uint8_t separator = 0;
    if (separatorValue) {
        int64_t separatorLength = length(globalObject, separatorValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (separatorLength != 1)
            return raiseValueError(globalObject, scope, "sep must be length 1."_s);
        char32_t character;
        if (isStr(separatorValue)) {
            StrView view(globalObject, strValue(separatorValue));
            RETURN_IF_EXCEPTION(scope, { });
            character = view.at(0);
        } else if (PyBytes::is(separatorValue))
            character = PyBytes::from(separatorValue)->span()[0];
        else
            return raiseTypeError(globalObject, scope, "sep must be str or bytes."_s);
        if (character > 127)
            return raiseValueError(globalObject, scope, "sep must be ASCII."_s);
        separator = static_cast<uint8_t>(character);
    } else
        group = 0;

    auto span = bytesSpanOf(self);
    size_t size = span.size();
    // Counted from the end if positive, from the start if negative.
    bool fromStart = group < 0;
    uint64_t groupSize = group < 0 ? -static_cast<uint64_t>(group) : group;
    if (groupSize >= size)
        groupSize = 0;
    uint64_t total = size * 2 + (groupSize ? (size - 1) / groupSize : 0);
    if (total > String::MaxLength)
        RELEASE_AND_RETURN(scope, bytesRaiseNoMemory(globalObject));
    if (!total)
        return jsEmptyString(vm);
    std::span<Latin1Character> buffer;
    String result = String::tryCreateUninitialized(total, buffer);
    if (result.isNull())
        RELEASE_AND_RETURN(scope, bytesRaiseNoMemory(globalObject));
    size_t out = 0;
    for (size_t i = 0; i < size; ++i) {
        if (groupSize && i && !((fromStart ? i : size - i) % groupSize))
            buffer[out++] = static_cast<Latin1Character>(separator);
        buffer[out++] = static_cast<Latin1Character>(digits[span[i] >> 4]);
        buffer[out++] = static_cast<Latin1Character>(digits[span[i] & 15]);
    }
    ASSERT(out == total);
    return makeStr(vm, result);
}

static JSValue bytesFromHexText(JSGlobalObject* globalObject, JSValue source, bool asArray)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto raiseNotHex = [&](size_t position) {
        return raiseValueError(globalObject, scope, "non-hexadecimal number found in fromhex() arg at position "_s, position);
    };

    Vector<uint8_t, 128> text;
    std::span<const uint8_t> input;
    if (isStr(source)) {
        String string = strValue(source)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        text.reserveCapacity(string.length());
        for (unsigned i = 0; i < string.length(); ++i) {
            char16_t unit = string[i];
            if (unit >= 128)
                return raiseNotHex(i);
            text.append(static_cast<uint8_t>(unit));
        }
        input = text.span();
    } else if (isBytesLike(source)) {
        if (!getBytesLike(globalObject, source, input))
            return { };
    } else
        return raiseTypeError(globalObject, scope, "fromhex() argument must be str or bytes-like, not "_s, bytesFullTypeName(globalObject, source));

    Vector<uint8_t, 64> result;
    result.reserveCapacity(input.size() / 2);
    size_t size = input.size();
    for (size_t i = 0; i < size;) {
        if (bytesIsSpace(input[i])) {
            ++i;
            continue;
        }
        unsigned top = alphanumericDigitValue(input[i]);
        if (top >= 16)
            return raiseNotHex(i);
        if (++i >= size)
            return raiseValueError(globalObject, scope, "fromhex() arg must contain an even number of hexadecimal digits"_s);
        unsigned bottom = alphanumericDigitValue(input[i]);
        if (bottom >= 16)
            return raiseNotHex(i);
        ++i;
        result.append(static_cast<uint8_t>(top << 4 | bottom));
    }
    RELEASE_AND_RETURN(scope, bytesMake(globalObject, asArray, result.span()));
}

static JSValue bytesFromHexMethod(JSGlobalObject* globalObject, JSValue cls, JSValue source)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A derived class is called with bytes, whichever it derives from.
    JSValue result = bytesFromHexText(globalObject, source, cls == JSValue(rt.bytearrayType()));
    RETURN_IF_EXCEPTION(scope, { });
    if (cls == JSValue(rt.bytesType()) || cls == JSValue(rt.bytearrayType()))
        return result;
    RELEASE_AND_RETURN(scope, call(globalObject, cls, result));
}

// ---- format % arguments ----

namespace {

enum BytesFormatFlag : uint8_t {
    BytesLeftJustify = 1 << 0,
    BytesAlwaysSign = 1 << 1,
    BytesBlankSign = 1 << 2,
    BytesAlternate = 1 << 3,
    BytesZeroPad = 1 << 4,
};

struct BytesFormatter {
    JSGlobalObject* globalObject;
    JSValue arguments;
    // As CPython counts: the length is -1 for a single value that is not a tuple, and below that for one found by key.
    int64_t argumentCount;
    int64_t argumentIndex;
    JSValue key; // That the argument being formatted was found by, if it was.

    String errorPrefix() const
    {
        if (key)
            return makeString("format argument "_s, reprForMessage(globalObject, key), ": "_s);
        if (argumentIndex >= 0)
            return makeString("format argument "_s, argumentIndex, ": "_s);
        return "format argument: "_s;
    }

    JSValue nextArgument(bool allowSingle)
    {
        if (argumentIndex < argumentCount) {
            int64_t index = argumentIndex++;
            if (argumentCount >= 0)
                return PyTuple::from(arguments)->at(static_cast<unsigned>(index));
            if (allowSingle)
                return arguments;
        }
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseTypeError(globalObject, scope, "not enough arguments for format string (got "_s, argumentCount < 0 ? 1 : argumentCount, ')');
    }
};

}

// For %b: an object whose bytes are the text.
static JSValue bytesFormatObject(BytesFormatter& f, JSValue value)
{
    JSGlobalObject* globalObject = f.globalObject;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyBytes::is(value) || PyByteArray::is(value))
        return value;
    JSValue result = bytesCallBytesMethod(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    if (isBytesLike(value))
        RELEASE_AND_RETURN(scope, bytesFromBufferOrIterable(globalObject, value));
    return raiseTypeError(globalObject, scope, f.errorPrefix(), "%b requires a bytes-like object, or an object that implements __bytes__, not "_s, bytesFullTypeName(globalObject, value));
}

// For %d and the like: "-"? ("0x" | "0X" | "0o")? digit+, with at least `precision` digits. Null: an exception.
static String bytesFormatInteger(BytesFormatter& f, JSValue value, bool alternate, int precision, char code)
{
    JSGlobalObject* globalObject = f.globalObject;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool needsInteger = code == 'o' || code == 'x' || code == 'X';
    JSValue integer;
    if (isInt(value))
        integer = intValue(value);
    else if (bytesIsNumberLike(globalObject, value)) {
        integer = needsInteger ? toIndex(globalObject, value) : toInt(globalObject, value);
        if (!integer && !clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return { };
    }
    if (!integer) {
        raiseTypeError(globalObject, scope, f.errorPrefix(), '%', code, needsInteger ? " requires an integer, not "_s : " requires a real number, not "_s, bytesFullTypeName(globalObject, value));
        return { };
    }
    if (precision > std::numeric_limits<int>::max() - 3) {
        raiseOverflowError(globalObject, scope, "precision too large"_s);
        return { };
    }
    unsigned base = code == 'o' ? 8 : needsInteger ? 16 : 10;
    String digits = intToString(globalObject, intValue(integer), base);
    RETURN_IF_EXCEPTION(scope, { });
    bool isNegative = digits[0] == '-';
    StringView magnitude = StringView(digits).substring(isNegative);
    StrBuilder builder;
    if (isNegative)
        builder.append('-');
    if (alternate && base != 10)
        builder.append('0', code);
    for (int64_t i = magnitude.length(); i < precision; ++i)
        builder.append('0');
    if (code == 'X')
        builder.append(magnitude.convertToASCIIUppercase());
    else
        builder.append(magnitude);
    if (builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope, String());
        return { };
    }
    return builder.toString();
}

static bool bytesFormatCharacter(BytesFormatter& f, JSValue value, uint8_t& result)
{
    JSGlobalObject* globalObject = f.globalObject;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral requirement = "%c requires an integer in range(256) or a single byte, not "_s;
    if (PyBytes::is(value) || PyByteArray::is(value)) {
        auto span = bytesSpanOf(value);
        if (span.size() != 1) {
            raiseTypeError(globalObject, scope, f.errorPrefix(), requirement, bytesIsArray(value) ? "a bytearray"_s : "a bytes"_s, " object of length "_s, span.size());
            return false;
        }
        result = span[0];
        return true;
    }
    if (!hasIndex(globalObject, value)) {
        raiseTypeError(globalObject, scope, f.errorPrefix(), requirement, bytesFullTypeName(globalObject, value));
        return false;
    }
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto number = tryInt64(index);
    if (!number || *number < 0 || *number > 255) {
        raiseOverflowError(globalObject, scope, f.errorPrefix(), "%c argument not in range(256)"_s);
        return false;
    }
    result = static_cast<uint8_t>(*number);
    return true;
}

JSValue bytesFormatPercent(JSGlobalObject* globalObject, std::span<const uint8_t> formatBytes, JSValue arguments, bool makeByteArray)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // What is formatted runs code, which may change the bytearray the format is.
    Vector<uint8_t, 128> format;
    format.append(formatBytes);
    Vector<uint8_t, 256> out;
    auto appendFill = [&](uint8_t fill, int64_t count) {
        size_t size = out.size();
        if (static_cast<uint64_t>(count) > bytesMaxLength || !out.tryGrow(size + count)) {
            raiseMemoryError(globalObject, scope, String());
            return false;
        }
        memset(out.mutableSpan().data() + size, fill, count);
        return true;
    };

    BytesFormatter f { globalObject, arguments, -1, -2, { } };
    JSValue mapping;
    if (auto* tuple = PyTuple::tryFrom(arguments)) {
        f.argumentCount = tuple->length();
        f.argumentIndex = 0;
    } else if (!PyBytes::is(arguments) && !PyByteArray::is(arguments) && !isStr(arguments) && rt.typeOf(arguments)->lookup(Special::GetItem))
        mapping = arguments;

    size_t end = format.size();
    size_t position = 0;
    while (position < end) {
        if (format[position] != '%') {
            size_t start = position;
            auto* next = static_cast<const uint8_t*>(memchr(format.span().data() + position, '%', end - position));
            position = next ? next - format.span().data() : end;
            out.append(format.subspan(start, position - start));
            continue;
        }
        size_t percent = position++;
        if (position < end && format[position] == '%') {
            ++position;
            out.append('%');
            continue;
        }

        unsigned flags = 0;
        int64_t width = -1;
        int precision = -1;
        uint8_t code = 0;
        bool exhausted = false;
        auto advance = [&] {
            if (position < end) {
                code = format[position++];
                return true;
            }
            exhausted = true;
            return false;
        };

        f.key = JSValue();
        if (position < end && format[position] == '(') {
            if (!mapping)
                return raiseTypeError(globalObject, scope, "format requires a mapping, not "_s, bytesFullTypeName(globalObject, f.arguments));
            size_t keyStart = ++position;
            unsigned depth = 1;
            while (depth && position < end) {
                uint8_t byte = format[position++];
                if (byte == ')')
                    --depth;
                else if (byte == '(')
                    ++depth;
            }
            if (depth)
                return raiseValueError(globalObject, scope, "stray % or incomplete format key at position "_s, percent);
            f.key = PyBytes::create(globalObject, format.subspan(keyStart, position - 1 - keyStart));
            f.arguments = getItem(globalObject, mapping, f.key);
            RETURN_IF_EXCEPTION(scope, { });
            f.argumentCount = -3;
            f.argumentIndex = -4;
        } else if (f.argumentCount < -1)
            return raiseValueError(globalObject, scope, "format requires a parenthesised mapping key at position "_s, percent);

        while (advance()) {
            switch (code) {
            case '-':
                flags |= BytesLeftJustify;
                continue;
            case '+':
                flags |= BytesAlwaysSign;
                continue;
            case ' ':
                flags |= BytesBlankSign;
                continue;
            case '#':
                flags |= BytesAlternate;
                continue;
            case '0':
                flags |= BytesZeroPad;
                continue;
            }
            break;
        }

        // The int that stands for a `*`.
        auto starArgument = [&]() -> JSValue {
            if (f.argumentCount < -1)
                return raiseValueError(globalObject, scope, "* cannot be used with a parenthesised mapping key at position "_s, percent);
            JSValue value = f.nextArgument(false);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isInt(value))
                return raiseTypeError(globalObject, scope, f.errorPrefix(), "* requires int, not "_s, bytesFullTypeName(globalObject, value));
            return intValue(value);
        };

        if (!exhausted && code == '*') {
            JSValue value = starArgument();
            RETURN_IF_EXCEPTION(scope, { });
            auto given = tryInt64(value);
            if (!given || *given == std::numeric_limits<int64_t>::min())
                return raiseOverflowError(globalObject, scope, f.errorPrefix(), "too big for width"_s);
            width = *given;
            if (width < 0) {
                flags |= BytesLeftJustify;
                width = -width;
            }
            advance();
        } else if (!exhausted && bytesIsDecimal(code)) {
            width = code - '0';
            while (advance()) {
                if (!bytesIsDecimal(code))
                    break;
                int digit = code - '0';
                if (width > (bytesMaxSsize - digit) / 10)
                    return raiseValueError(globalObject, scope, "width too big at position "_s, percent);
                width = width * 10 + digit;
            }
        }

        if (!exhausted && code == '.') {
            precision = 0;
            advance();
            if (!exhausted && code == '*') {
                JSValue value = starArgument();
                RETURN_IF_EXCEPTION(scope, { });
                auto given = tryInt64(value);
                if (!given || *given > std::numeric_limits<int>::max() || *given < std::numeric_limits<int>::min())
                    return raiseOverflowError(globalObject, scope, f.errorPrefix(), "too big for precision"_s);
                precision = std::max<int>(static_cast<int>(*given), 0);
                advance();
            } else if (!exhausted && bytesIsDecimal(code)) {
                precision = code - '0';
                while (advance()) {
                    if (!bytesIsDecimal(code))
                        break;
                    int digit = code - '0';
                    if (precision > (std::numeric_limits<int>::max() - digit) / 10)
                        return raiseValueError(globalObject, scope, "precision too big at position "_s, percent);
                    precision = precision * 10 + digit;
                }
            }
        }

        // As C has them, and they mean nothing.
        if (!exhausted && (code == 'h' || code == 'l' || code == 'L'))
            advance();
        if (exhausted)
            return raiseValueError(globalObject, scope, "stray % at position "_s, percent);

        JSValue value = f.nextArgument(true);
        RETURN_IF_EXCEPTION(scope, { });

        // The text, before it is padded: in `scratch`, or the bytes of `holder`.
        Vector<uint8_t, 64> scratch;
        JSValue holder;
        std::span<const uint8_t> text;
        bool isNumber = false;
        auto useString = [&](const String& string) {
            scratch.reserveCapacity(string.length());
            for (unsigned i = 0; i < string.length(); ++i)
                scratch.append(static_cast<uint8_t>(string[i]));
            text = scratch.span();
        };
        switch (code) {
        case 'r':
        case 'a': {
            JSString* string = ascii(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            String characters = string->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            useString(characters);
            break;
        }
        case 's':
        case 'b':
            holder = bytesFormatObject(f, value);
            RETURN_IF_EXCEPTION(scope, { });
            text = bytesSpanOf(holder);
            break;
        case 'i':
        case 'd':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            String digits = bytesFormatInteger(f, value, flags & BytesAlternate, precision, static_cast<char>(code));
            RETURN_IF_EXCEPTION(scope, { });
            useString(digits);
            isNumber = true;
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            double number = toDouble(globalObject, value);
            if (scope.exception()) {
                if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                    raiseTypeError(globalObject, scope, f.errorPrefix(), '%', static_cast<char>(code), " requires a real number, not "_s, bytesFullTypeName(globalObject, value));
                return { };
            }
            if (precision > std::numeric_limits<int>::max() - 1024)
                return raiseValueError(globalObject, scope, "precision too big"_s);
            useString(formatDouble(number, static_cast<char>(code), precision < 0 ? 6 : precision, flags & BytesAlternate ? FloatAlternate : 0));
            isNumber = true;
            break;
        }
        case 'c': {
            uint8_t byte;
            if (!bytesFormatCharacter(f, value, byte))
                return { };
            scratch.append(byte);
            text = scratch.span();
            break;
        }
        default: {
            size_t at = position - 1;
            if (bytesIsAlpha(code))
                return raiseValueError(globalObject, scope, "unsupported format %"_s, static_cast<char>(code), " at position "_s, percent);
            if (code == '\'')
                return raiseValueError(globalObject, scope, "stray % at position "_s, percent, " or unexpected format character \"'\" at position "_s, at);
            if (code >= 32 && code < 127)
                return raiseValueError(globalObject, scope, "stray % at position "_s, percent, " or unexpected format character '"_s, static_cast<char>(code), "' at position "_s, at);
            static constexpr char digits[] = "0123456789abcdef";
            return raiseValueError(globalObject, scope, "stray % at position "_s, percent, " or unexpected format character with code 0x"_s, digits[code >> 4], digits[code & 15], " at position "_s, at);
        }
        }
        if (!isNumber && code != 'c' && precision >= 0 && text.size() > static_cast<size_t>(precision))
            text = text.first(precision);

        uint8_t fill = isNumber && (flags & BytesZeroPad) ? '0' : ' ';
        uint8_t sign = 0;
        if (isNumber) {
            if (text[0] == '-' || text[0] == '+') {
                sign = text[0];
                text = text.subspan(1);
            } else if (flags & BytesAlwaysSign)
                sign = '+';
            else if (flags & BytesBlankSign)
                sign = ' ';
        }
        int64_t textLength = text.size();
        width = std::max(width, textLength);
        if (sign) {
            if (fill != ' ')
                out.append(sign);
            if (width > textLength)
                --width;
        }
        // "0x" and the like come before the zeros, and after the spaces.
        bool hasPrefix = (flags & BytesAlternate) && (code == 'o' || code == 'x' || code == 'X');
        if (hasPrefix) {
            if (fill != ' ') {
                out.append(text.first(2));
                text = text.subspan(2);
            }
            width = std::max<int64_t>(width - 2, 0);
            textLength -= 2;
        }
        if (width > textLength && !(flags & BytesLeftJustify)) {
            if (!appendFill(fill, width - textLength))
                return { };
            width = textLength;
        }
        if (fill == ' ' && sign)
            out.append(sign);
        out.append(text);
        if (width > textLength && !appendFill(' ', width - textLength))
            return { };
        ensureStillAliveHere(holder);
    }

    if (f.argumentIndex < f.argumentCount && !mapping)
        return raiseTypeError(globalObject, scope, "not all arguments converted during bytes formatting (required "_s, f.argumentCount < 0 ? 0 : f.argumentIndex, ", got "_s, f.argumentCount < 0 ? 1 : f.argumentCount, ')');
    RELEASE_AND_RETURN(scope, bytesMake(globalObject, makeByteArray, out.span()));
}

// ---- What only a bytearray can do ----

// array[lo:hi] = bytes, which are not bytes of the array itself.
static bool byteArrayReplaceRange(JSGlobalObject* globalObject, PyByteArray* array, size_t lo, size_t hi, std::span<const uint8_t> bytes)
{
    size_t size = array->length();
    ASSERT(lo <= hi && hi <= size);
    size_t removed = hi - lo;
    if (bytes.size() < removed) {
        if (!byteArrayCanResize(globalObject, array))
            return false;
        uint8_t* data = array->mutableSpan().data();
        memmove(data + lo + bytes.size(), data + hi, size - hi);
        if (!array->resize(globalObject, size - (removed - bytes.size())))
            return false;
    } else if (bytes.size() > removed) {
        if (!array->resize(globalObject, size + (bytes.size() - removed)))
            return false;
        uint8_t* data = array->mutableSpan().data();
        memmove(data + lo + bytes.size(), data + hi, size - hi);
    }
    if (!bytes.empty())
        memcpy(array->mutableSpan().data() + lo, bytes.data(), bytes.size());
    return true;
}

// array[key] = values, or, with no values, del array[key].
static JSValue byteArrayAssign(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue values)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* array = PyByteArray::from(self);

    PySlice::Indices indices;
    auto* slice = PySlice::tryFrom(key);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        uint8_t byte = 0;
        if (values && !bytesByteValue(globalObject, values, byte))
            return { };
        // Only now: either conversion may have resized the array.
        int64_t size = array->length();
        if (index < 0)
            index += size;
        if (index < 0 || index >= size)
            return raiseIndexError(globalObject, scope, "bytearray index out of range"_s);
        if (values) {
            array->mutableSpan()[index] = byte;
            return jsUndefined();
        }
        indices = { index, index + 1, 1, 1 };
    } else if (slice) {
        if (!bytesSliceIndices(globalObject, self, slice, indices))
            return { };
    } else
        return raiseTypeError(globalObject, scope, "bytearray indices must be integers or slices, not "_s, typeName(globalObject, key));

    JSValue holder;
    std::span<const uint8_t> bytes;
    if (values) {
        holder = values;
        if (values == self || !PyByteArray::is(values)) {
            if (bytesIsNumberLike(globalObject, values) || isStr(values))
                return raiseTypeError(globalObject, scope, "can assign only bytes, buffers, or iterables of ints in range(0, 256)"_s);
            PyByteArray* copy = PyByteArray::create(vm, rt.structure<PyByteArray>(), rt.bytearrayType());
            holder = copy;
            if (!byteArrayInitialize(globalObject, copy, values, nullptr, nullptr))
                return { };
            // Iterating may have resized the array.
            if (!bytesSliceIndices(globalObject, self, slice, indices))
                return { };
        }
        bytes = PyByteArray::from(holder)->span();
    }

    int64_t start = indices.start;
    int64_t stop = indices.stop;
    int64_t step = indices.step;
    int64_t count = indices.length;
    // array[5:2] = ... inserts before 5, not before 2.
    if ((step < 0 && start < stop) || (step > 0 && start > stop))
        stop = start;

    if (step == 1) {
        bool done = byteArrayReplaceRange(globalObject, array, start, stop, bytes);
        ensureStillAliveHere(holder);
        return done ? jsUndefined() : JSValue();
    }

    if (bytes.empty()) {
        if (!byteArrayCanResize(globalObject, array))
            return { };
        if (!count)
            return jsUndefined();
        if (step < 0) {
            stop = start + 1;
            start = stop + step * (count - 1) - 1;
            step = -step;
        }
        // Each run between two of the bytes that go moves down by as many as have gone before it.
        uint8_t* data = array->mutableSpan().data();
        int64_t size = array->length();
        for (int64_t i = 0; i < count; ++i) {
            int64_t current = start + i * step;
            // The step may be anything up to the largest int there is, so it is not added to anything.
            int64_t run = i + 1 < count ? step - 1 : size - current - 1;
            memmove(data + current - i, data + current + 1, run);
        }
        if (!array->resize(globalObject, size - count))
            return { };
        return jsUndefined();
    }

    if (static_cast<int64_t>(bytes.size()) != count)
        return raiseValueError(globalObject, scope, "attempt to assign bytes of size "_s, bytes.size(), " to extended slice of size "_s, count);
    uint8_t* data = array->mutableSpan().data();
    for (int64_t i = 0; i < count; ++i)
        data[start + i * step] = bytes[i];
    ensureStillAliveHere(holder);
    return jsUndefined();
}

static JSValue byteArraySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value) { return byteArrayAssign(globalObject, self, key, value); }
static JSValue byteArrayDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key) { return byteArrayAssign(globalObject, self, key, { }); }

static JSValue byteArrayIAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const uint8_t> bytes;
    if (!isBytesLike(other) || !getBytesLike(globalObject, other, bytes)) {
        takePendingException(globalObject);
        return raiseTypeError(globalObject, scope, "can't concat "_s, typeName(globalObject, other), " to "_s, typeName(globalObject, self));
    }
    auto* array = PyByteArray::from(self);
    // As in CPython, where the array cannot be resized while its own bytes are being looked at.
    if (other == self && !bytes.empty())
        return raiseBufferError(globalObject, scope, "Existing exports of data: object cannot be re-sized"_s);
    array->append(globalObject, bytes);
    RETURN_IF_EXCEPTION(scope, { });
    return self;
}

static JSValue byteArrayIMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // So that `*=` goes on to complain as `*` does.
    if (!hasIndex(globalObject, countValue))
        return rt.notImplemented();
    int64_t count = toSsize(globalObject, countValue, rt.OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    count = std::max<int64_t>(count, 0);
    if (count == 1)
        return self;
    auto* array = PyByteArray::from(self);
    size_t size = array->length();
    if (count && size > static_cast<uint64_t>(bytesMaxSsize / count))
        RELEASE_AND_RETURN(scope, bytesRaiseNoMemory(globalObject));
    if (!array->resize(globalObject, size * count))
        return { };
    if (count) {
        uint8_t* data = array->mutableSpan().data();
        bytesRepeatInto(data, { data, size }, count);
    }
    return self;
}

static JSValue byteArrayAppend(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    uint8_t byte;
    if (!bytesByteValue(globalObject, item, byte))
        return { };
    auto* array = PyByteArray::from(self);
    if (!array->resize(globalObject, array->length() + 1))
        return { };
    array->mutableSpan().back() = byte;
    return jsUndefined();
}

static JSValue byteArrayInsert(JSGlobalObject* globalObject, JSValue self, JSValue indexValue, JSValue item)
{
    int64_t index;
    uint8_t byte;
    if (!bytesSsizeArgument(globalObject, indexValue, index) || !bytesByteValue(globalObject, item, byte))
        return { };
    auto* array = PyByteArray::from(self);
    int64_t size = array->length();
    if (!array->resize(globalObject, size + 1))
        return { };
    if (index < 0)
        index = std::max<int64_t>(index + size, 0);
    index = std::min(index, size);
    uint8_t* data = array->mutableSpan().data();
    memmove(data + index + 1, data + index, size - index);
    data[index] = byte;
    return jsUndefined();
}

static JSValue byteArrayExtend(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* array = PyByteArray::from(self);
    if (isBytesLike(iterable)) {
        std::span<const uint8_t> bytes;
        if (!getBytesLike(globalObject, iterable, bytes)) {
            takePendingException(globalObject);
            return raiseTypeError(globalObject, scope, "can't set bytearray slice from "_s, typeName(globalObject, iterable));
        }
        array->append(globalObject, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    JSValue iterator = getIter(globalObject, iterable);
    if (!iterator) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            raiseTypeError(globalObject, scope, "can't extend bytearray with "_s, typeName(globalObject, iterable));
        return { };
    }
    int64_t hint = lengthHint(globalObject, iterable, 32);
    RETURN_IF_EXCEPTION(scope, { });
    // All or nothing: the array does not change unless every item is a byte.
    Vector<uint8_t, 64> buffer;
    if (hint > 0 && static_cast<uint64_t>(hint) <= (1u << 20))
        buffer.reserveCapacity(hint);
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        uint8_t byte;
        if (!bytesByteValue(globalObject, item, byte)) {
            if (isStr(iterable) && clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, "expected iterable of integers; got: 'str'"_s);
            return { };
        }
        buffer.append(byte);
    }
    array->append(globalObject, buffer.span());
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue byteArrayPop(JSGlobalObject* globalObject, JSValue self, JSValue indexValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t index = -1;
    if (indexValue && !bytesSsizeArgument(globalObject, indexValue, index))
        return { };
    auto* array = PyByteArray::from(self);
    int64_t size = array->length();
    if (!size)
        return raiseIndexError(globalObject, scope, "pop from empty bytearray"_s);
    if (index < 0)
        index += size;
    if (index < 0 || index >= size)
        return raiseIndexError(globalObject, scope, "pop index out of range"_s);
    uint8_t byte = array->span()[index];
    if (!byteArrayReplaceRange(globalObject, array, index, index + 1, { }))
        return { };
    return jsNumber(byte);
}

static JSValue byteArrayRemove(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint8_t byte;
    if (!bytesByteValue(globalObject, value, byte))
        return { };
    auto* array = PyByteArray::from(self);
    size_t position = bytesSearch(array->span(), { &byte, 1 });
    if (position == notFound)
        return raiseValueError(globalObject, scope, "value not found in bytearray"_s);
    if (!byteArrayReplaceRange(globalObject, array, position, position + 1, { }))
        return { };
    return jsUndefined();
}

static JSValue byteArrayReverse(JSGlobalObject*, JSValue self)
{
    auto span = PyByteArray::from(self)->mutableSpan();
    std::reverse(span.begin(), span.end());
    return jsUndefined();
}

static JSValue byteArrayClear(JSGlobalObject* globalObject, JSValue self)
{
    if (!PyByteArray::from(self)->resize(globalObject, 0))
        return { };
    return jsUndefined();
}

static JSValue byteArrayCopy(JSGlobalObject* globalObject, JSValue self) { return bytesMake(globalObject, true, bytesSpanOf(self)); }

static JSValue byteArrayResize(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size;
    if (!bytesSsizeArgument(globalObject, sizeValue, size))
        return { };
    if (size < 0)
        return raiseValueError(globalObject, scope, "Can only resize to positive sizes, got "_s, size);
    if (!PyByteArray::from(self)->resize(globalObject, size))
        return { };
    return jsUndefined();
}

static JSValue byteArrayTakeBytes(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* array = PyByteArray::from(self);
    int64_t size = array->length();
    int64_t count = size;
    if (countValue && !isNone(countValue)) {
        if (!hasIndex(globalObject, countValue))
            return raiseTypeError(globalObject, scope, "n must be an integer or None"_s);
        count = toSsize(globalObject, countValue, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        size = array->length();
        if (count < 0)
            count += size;
    }
    if (count < 0 || count > size)
        return raiseIndexError(globalObject, scope, "can't take "_s, count, " bytes outside size "_s, size);
    if (!byteArrayCanResize(globalObject, array))
        return { };
    if (!count)
        return rt.emptyBytes();
    // FIXME: CPython hands over the buffer itself when all of it, or most, is taken.
    JSValue result = bytesMake(globalObject, false, array->span().first(count));
    RETURN_IF_EXCEPTION(scope, { });
    if (!byteArrayReplaceRange(globalObject, array, 0, count, { }))
        return { };
    return result;
}

static JSValue byteArrayAlloc(JSGlobalObject*, JSValue self) { return bytesNumber(PyByteArray::from(self)->capacity()); }
static JSValue byteArraySizeOf(JSGlobalObject*, JSValue self) { return bytesNumber(sizeof(PyByteArray) + PyByteArray::from(self)->capacity()); }

static JSValue byteArrayReduceForProtocol(JSGlobalObject* globalObject, JSValue self, int protocol)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue state = callMethod(globalObject, self, rt.names.dGetState());
    RETURN_IF_EXCEPTION(scope, { });
    auto* array = PyByteArray::from(self);
    auto span = array->span();
    JSValue arguments;
    if (span.empty())
        arguments = rt.emptyTuple();
    else if (protocol < 3) {
        // As text, which is all the older protocols have.
        if (span.size() > String::MaxLength)
            RELEASE_AND_RETURN(scope, bytesRaiseNoMemory(globalObject));
        arguments = PyTuple::create(globalObject, { makeStr(vm, String(byteCast<Latin1Character>(span))), makeStr(vm, "latin-1"_s) });
    } else {
        JSValue bytes = bytesMake(globalObject, false, span);
        RETURN_IF_EXCEPTION(scope, { });
        arguments = PyTuple::create(globalObject, { bytes });
    }
    return PyTuple::create(globalObject, { array->type(), arguments, state });
}

JSValue hexOfBytes(JSGlobalObject* globalObject, JSValue bytesOrByteArray, JSValue separator, JSValue bytesPerSeparator)
{
    return bytesHex(globalObject, bytesOrByteArray, separator, bytesPerSeparator);
}

static JSValue byteArrayReduce(JSGlobalObject* globalObject, JSValue self) { return byteArrayReduceForProtocol(globalObject, self, 2); }

static JSValue byteArrayReduceEx(JSGlobalObject* globalObject, JSValue self, JSValue protocolValue)
{
    int protocol = 0;
    if (protocolValue && !bytesIntArgument(globalObject, protocolValue, protocol))
        return { };
    return byteArrayReduceForProtocol(globalObject, self, protocol);
}

// ---- The types ----

#define PY_BYTES_COMMON_METHODS \
    PY_SLOT("__buffer__", bufferMethod, "($self, flags, /)"), \
    PY_SLOT("__repr__", bytesReprMethod, "($self, /)"), \
    PY_SLOT("__str__", bytesReprMethod, "($self, /)"), \
    PY_SLOT("__eq__", bytesCompareMethod<CompareOperator::Eq>, "($self, value, /)"), \
    PY_SLOT("__ne__", bytesCompareMethod<CompareOperator::NotEq>, "($self, value, /)"), \
    PY_SLOT("__lt__", bytesCompareMethod<CompareOperator::Lt>, "($self, value, /)"), \
    PY_SLOT("__le__", bytesCompareMethod<CompareOperator::LtE>, "($self, value, /)"), \
    PY_SLOT("__gt__", bytesCompareMethod<CompareOperator::Gt>, "($self, value, /)"), \
    PY_SLOT("__ge__", bytesCompareMethod<CompareOperator::GtE>, "($self, value, /)"), \
    PY_SLOT("__iter__", bytesIter, "($self, /)"), \
    PY_SLOT("__len__", bytesLenMethod, "($self, /)"), \
    PY_SLOT("__getitem__", bytesGetItem, "($self, key, /)"), \
    PY_SLOT("__contains__", bytesContains, "($self, key, /)"), \
    PY_SEQUENCE_SLOT("__add__", bytesAdd, "($self, value, /)"), \
    PY_SEQUENCE_SLOT("__mul__", bytesMul, "($self, value, /)"), \
    PY_SEQUENCE_SLOT("__rmul__", bytesMul, "($self, value, /)"), \
    PY_SLOT("__mod__", bytesMod, "($self, value, /)"), \
    PY_SLOT("__rmod__", bytesRMod, "($self, value, /)"), \
    PY_METHOD("capitalize", bytesCapitalize, "($self, /)"), \
    PY_METHOD("center", bytesJustifyMethod<BytesJustify::Center>, "($self, width, fillchar=b' ', /)"), \
    PY_METHOD("count", bytesCount, "($self, sub, start=None, end=None, /)"), \
    PY_METHOD("decode", bytesDecodeMethod, "($self, /, encoding='utf-8', errors='strict')"), \
    PY_METHOD("endswith", bytesStartsOrEndsWith<true>, "($self, suffix, start=None, end=None, /)"), \
    PY_METHOD("expandtabs", bytesExpandTabs, "($self, /, tabsize=8)"), \
    PY_METHOD("find", (bytesFindMethod<false, false>), "($self, sub, start=None, end=None, /)"), \
    PY_CLASSMETHOD("fromhex", bytesFromHexMethod, "($type, string, /)"), \
    PY_METHOD("hex", bytesHex, "($self, /, sep=<unrepresentable>, bytes_per_sep=1)"), \
    PY_METHOD("index", (bytesFindMethod<false, true>), "($self, sub, start=None, end=None, /)"), \
    PY_METHOD("isalnum", bytesAll<bytesAlphanumericPredicate>, "($self, /)"), \
    PY_METHOD("isalpha", bytesAll<bytesAlphaPredicate>, "($self, /)"), \
    PY_METHOD("isascii", bytesIsASCIIMethod, "($self, /)"), \
    PY_METHOD("isdigit", bytesAll<bytesDecimalPredicate>, "($self, /)"), \
    PY_METHOD("islower", bytesIsOfCase<false>, "($self, /)"), \
    PY_METHOD("isspace", bytesAll<bytesSpacePredicate>, "($self, /)"), \
    PY_METHOD("istitle", bytesIsTitle, "($self, /)"), \
    PY_METHOD("isupper", bytesIsOfCase<true>, "($self, /)"), \
    PY_METHOD("join", bytesJoin, "($self, iterable_of_bytes, /)"), \
    PY_METHOD("ljust", bytesJustifyMethod<BytesJustify::Left>, "($self, width, fillchar=b' ', /)"), \
    PY_METHOD("lower", bytesLower, "($self, /)"), \
    PY_METHOD("lstrip", bytesStrip<BytesStripSide::Left>, "($self, bytes=None, /)"), \
    PY_STATICMETHOD("maketrans", bytesMakeTrans, "(frm, to, /)"), \
    PY_METHOD("partition", bytesPartition<false>, "($self, sep, /)"), \
    PY_METHOD("removeprefix", bytesRemovePrefix, "($self, prefix, /)"), \
    PY_METHOD("removesuffix", bytesRemoveSuffix, "($self, suffix, /)"), \
    PY_METHOD("replace", bytesReplace, "($self, old, new, /, count=-1)"), \
    PY_METHOD("rfind", (bytesFindMethod<true, false>), "($self, sub, start=None, end=None, /)"), \
    PY_METHOD("rindex", (bytesFindMethod<true, true>), "($self, sub, start=None, end=None, /)"), \
    PY_METHOD("rjust", bytesJustifyMethod<BytesJustify::Right>, "($self, width, fillchar=b' ', /)"), \
    PY_METHOD("rpartition", bytesPartition<true>, "($self, sep, /)"), \
    PY_METHOD("rsplit", bytesRSplit, "($self, /, sep=None, maxsplit=-1)"), \
    PY_METHOD("rstrip", bytesStrip<BytesStripSide::Right>, "($self, bytes=None, /)"), \
    PY_METHOD("split", bytesSplit, "($self, /, sep=None, maxsplit=-1)"), \
    PY_METHOD("splitlines", bytesSplitLines, "($self, /, keepends=False)"), \
    PY_METHOD("startswith", bytesStartsOrEndsWith<false>, "($self, prefix, start=None, end=None, /)"), \
    PY_METHOD("strip", bytesStrip<BytesStripSide::Both>, "($self, bytes=None, /)"), \
    PY_METHOD("swapcase", bytesSwapCase, "($self, /)"), \
    PY_METHOD("title", bytesTitle, "($self, /)"), \
    PY_METHOD("translate", bytesTranslate, "($self, table, /, delete=b'')"), \
    PY_METHOD("upper", bytesUpper, "($self, /)"), \
    PY_METHOD("zfill", bytesZFill, "($self, width, /)")

static constexpr NativeDef bytesMethods[] = {
    PY_NEW(bytesNew, "($type, *args, **kwargs)"),
    PY_SLOT("__hash__", bytesHashMethod, "($self, /)"),
    PY_METHOD("__bytes__", bytesBytesMethod, "($self, /)"),
    PY_METHOD("__getnewargs__", bytesGetNewArgs, "($self, /)"),
    PY_BYTES_COMMON_METHODS,
};

static constexpr NativeDef byteArrayMethods[] = {
    PY_NEW(byteArrayNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", byteArrayInit, "($self, *args, **kwargs)"),
    PY_SLOT("__setitem__", byteArraySetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", byteArrayDelItem, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__iadd__", byteArrayIAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__imul__", byteArrayIMul, "($self, value, /)"),
    PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
    PY_METHOD("__alloc__", byteArrayAlloc, "($self, /)"),
    PY_METHOD("__reduce__", byteArrayReduce, "($self, /)"),
    PY_METHOD("__reduce_ex__", byteArrayReduceEx, "($self, proto=0, /)"),
    PY_METHOD("__sizeof__", byteArraySizeOf, "($self, /)"),
    PY_METHOD("append", byteArrayAppend, "($self, item, /)"),
    PY_METHOD("clear", byteArrayClear, "($self, /)"),
    PY_METHOD("copy", byteArrayCopy, "($self, /)"),
    PY_METHOD("extend", byteArrayExtend, "($self, iterable_of_ints, /)"),
    PY_METHOD("insert", byteArrayInsert, "($self, index, item, /)"),
    PY_METHOD("pop", byteArrayPop, "($self, index=-1, /)"),
    PY_METHOD("remove", byteArrayRemove, "($self, value, /)"),
    PY_METHOD("resize", byteArrayResize, "($self, size, /)"),
    PY_METHOD("reverse", byteArrayReverse, "($self, /)"),
    PY_METHOD("take_bytes", byteArrayTakeBytes, "($self, n=None, /)"),
    PY_BYTES_COMMON_METHODS,
};

#undef PY_BYTES_COMMON_METHODS

void initializeBytesTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.bytesType()->initializeBuiltin(globalObject, { "bytes"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::BytesSubclass, TypeFlag::MatchSelf }, bytesMethods, { }, "bytes(iterable_of_ints) -> bytes\nbytes(string, encoding[, errors]) -> bytes\nbytes(bytes_or_buffer) -> immutable copy of bytes_or_buffer\nbytes(int) -> bytes object of size given by the parameter initialized with null bytes\nbytes() -> empty bytes object\n\nConstruct an immutable array of bytes from:\n  - an iterable yielding integers in range(256)\n  - a text string encoded using the specified encoding\n  - any object implementing the buffer API.\n  - an integer"_s }, rt.typeType(), rt.objectType());
    rt.bytearrayType()->initializeBuiltin(globalObject, { "bytearray"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::MatchSelf }, byteArrayMethods, { }, "bytearray(iterable_of_ints) -> bytearray\nbytearray(string, encoding[, errors]) -> bytearray\nbytearray(bytes_or_buffer) -> mutable copy of bytes_or_buffer\nbytearray(int) -> bytes array of size given by the parameter initialized with null bytes\nbytearray() -> empty bytes array\n\nConstruct a mutable bytearray object from:\n  - an iterable yielding integers in range(256)\n  - a text string encoded using the specified encoding\n  - a bytes or a buffer object\n  - any object implementing the buffer API.\n  - an integer"_s }, rt.typeType(), rt.objectType());
    // What can change cannot be a key.
    rt.bytearrayType()->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    rt.bytearrayType()->didChange(vm);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
