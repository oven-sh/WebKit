/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCodecs.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PyRange.h"
#include <wtf/FlipBytes.h>
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module array. (Modules/arraymodule.c in CPython is the reference.)

// ---- The kinds of item ----

enum class ArrayKind : uint8_t {
    SignedChar,
    UnsignedChar,
    WideChar,
    CodePoint,
    Short,
    UnsignedShort,
    Int,
    UnsignedInt,
    Long,
    UnsignedLong,
    LongLong,
    UnsignedLongLong,
    Half,
    Float,
    Double,
    ComplexFloat,
    ComplexDouble,
};

// How pickles say what the bytes of an array are.
enum ArrayMachineFormat : int {
    ArrayUnsignedInt8 = 0,
    ArraySignedInt8 = 1,
    ArrayUnsignedInt16LE = 2,
    ArraySignedInt64BE = 13,
    ArrayFloatLE = 14,
    ArrayFloatBE = 15,
    ArrayDoubleLE = 16,
    ArrayDoubleBE = 17,
    ArrayUTF16LE = 18,
    ArrayUTF16BE = 19,
    ArrayUTF32LE = 20,
    ArrayUTF32BE = 21,
    ArrayComplexFloatLE = 22,
    ArrayComplexFloatBE = 23,
    ArrayComplexDoubleLE = 24,
    ArrayComplexDoubleBE = 25,
    ArrayHalfLE = 26,
    ArrayHalfBE = 27,
    ArrayMachineFormatMax = 27,
};

// Of an integer of that size on this machine, which is little endian, as is every machine JSC runs on.
static constexpr int arrayIntegerFormat(size_t size, bool isSigned)
{
    return size == 1 ? (isSigned ? ArraySignedInt8 : ArrayUnsignedInt8) : (size == 2 ? 2 : size == 4 ? 6 : 10) + 2 * isSigned;
}

struct ArrayDescriptor {
    ASCIILiteral typecode;
    uint8_t itemSize;
    bool isInteger;
    bool isSigned;
    int machineFormat;
};

static constexpr ArrayDescriptor arrayDescriptors[] = {
    { "b"_s, 1, true, true, ArraySignedInt8 },
    { "B"_s, 1, true, false, ArrayUnsignedInt8 },
    { "u"_s, sizeof(wchar_t), false, false, sizeof(wchar_t) == 2 ? ArrayUTF16LE : ArrayUTF32LE },
    { "w"_s, 4, false, false, ArrayUTF32LE },
    { "h"_s, sizeof(short), true, true, arrayIntegerFormat(sizeof(short), true) },
    { "H"_s, sizeof(short), true, false, arrayIntegerFormat(sizeof(short), false) },
    { "i"_s, sizeof(int), true, true, arrayIntegerFormat(sizeof(int), true) },
    { "I"_s, sizeof(int), true, false, arrayIntegerFormat(sizeof(int), false) },
    { "l"_s, sizeof(long), true, true, arrayIntegerFormat(sizeof(long), true) },
    { "L"_s, sizeof(long), true, false, arrayIntegerFormat(sizeof(long), false) },
    { "q"_s, sizeof(long long), true, true, arrayIntegerFormat(sizeof(long long), true) },
    { "Q"_s, sizeof(long long), true, false, arrayIntegerFormat(sizeof(long long), false) },
    { "e"_s, 2, false, false, ArrayHalfLE },
    { "f"_s, 4, false, false, ArrayFloatLE },
    { "d"_s, 8, false, false, ArrayDoubleLE },
    { "Zf"_s, 8, false, false, ArrayComplexFloatLE },
    { "Zd"_s, 16, false, false, ArrayComplexDoubleLE },
};
static constexpr unsigned arrayMaxItemSize = 16;

static std::optional<ArrayKind> arrayKindForTypecode(StringView typecode)
{
    for (unsigned i = 0; i < std::size(arrayDescriptors); ++i) {
        if (typecode == arrayDescriptors[i].typecode)
            return static_cast<ArrayKind>(i);
    }
    return std::nullopt;
}

static ALWAYS_INLINE bool arrayKindIsUnicode(ArrayKind kind) { return kind == ArrayKind::WideChar || kind == ArrayKind::CodePoint; }

// ---- The object ----

enum ArrayField : unsigned {
    ArrayStorage, // A PyByteStorage, or None if nothing is allocated.
    ArrayKindOfItems,
    ArrayExports, // How many buffers others hold on the storage, which has to stay as it is meanwhile.
    ArrayFieldCount,
};
// PyRecord::number is the length, in items.

static constexpr int64_t arrayMaxBytes = static_cast<int64_t>(1) << 40;

extern const TypeDef arrayTypeDef;
extern const TypeDef arrayIteratorTypeDef;

static PyType* arrayType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(arrayTypeDef); }

static ALWAYS_INLINE PyRecord* arrayOf(JSValue value) { return PyRecord::from(value); }
static ALWAYS_INLINE PyRecord* arrayTryFrom(JSGlobalObject* globalObject, JSValue value) { return PyRecord::tryFrom(value, arrayType(globalObject)); }
static ALWAYS_INLINE ArrayKind arrayKind(PyRecord* array) { return static_cast<ArrayKind>(array->get(ArrayKindOfItems).asInt32()); }
static ALWAYS_INLINE const ArrayDescriptor& arrayDescriptor(PyRecord* array) { return arrayDescriptors[array->get(ArrayKindOfItems).asInt32()]; }
static ALWAYS_INLINE int64_t arrayItemSize(PyRecord* array) { return arrayDescriptor(array).itemSize; }
static ALWAYS_INLINE int64_t arrayLength(PyRecord* array) { return array->number; }

static ALWAYS_INLINE PyByteStorage* arrayStorage(PyRecord* array)
{
    JSValue storage = array->get(ArrayStorage);
    return storage.isCell() ? uncheckedDowncast<PyByteStorage>(storage.asCell()) : nullptr;
}

static ALWAYS_INLINE uint8_t* arrayData(PyRecord* array)
{
    PyByteStorage* storage = arrayStorage(array);
    return storage ? storage->data() : nullptr;
}

// How many items there is room for.
static ALWAYS_INLINE int64_t arrayAllocated(PyRecord* array)
{
    PyByteStorage* storage = arrayStorage(array);
    return storage ? static_cast<int64_t>(storage->capacity()) / arrayItemSize(array) : 0;
}

static std::span<uint8_t> arrayBytes(JSValue value)
{
    PyRecord* array = arrayOf(value);
    return { arrayData(array), static_cast<size_t>(arrayLength(array) * arrayItemSize(array)) };
}

static NEVER_INLINE void arrayRaiseNoMemory(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseMemoryError(globalObject, scope, String());
}

// Room for that many items, all zero. Null: a MemoryError.
static PyByteStorage* arrayAllocate(JSGlobalObject* globalObject, int64_t count, int64_t itemSize)
{
    Runtime& rt = runtime(globalObject);
    PyByteStorage* storage = count <= arrayMaxBytes / itemSize ? PyByteStorage::tryCreate(rt.vm(), rt.structure<PyByteStorage>(), count * itemSize) : nullptr;
    if (!storage) {
        arrayRaiseNoMemory(globalObject);
        return nullptr;
    }
    memset(storage->data(), 0, count * itemSize);
    return storage;
}

// An array of that many zeros. Null: a MemoryError.
static PyRecord* arrayCreate(JSGlobalObject* globalObject, PyType* type, ArrayKind kind, int64_t length)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyByteStorage* storage = nullptr;
    if (length) {
        storage = arrayAllocate(globalObject, length, arrayDescriptors[static_cast<unsigned>(kind)].itemSize);
        if (!storage)
            return nullptr;
    }
    PyRecord* array = PyRecord::create(vm, rt.structure<PyRecord>(), type, ArrayFieldCount);
    array->set(vm, ArrayStorage, storage ? JSValue(storage) : jsUndefined());
    array->set(vm, ArrayKindOfItems, jsNumber(static_cast<int32_t>(kind)));
    array->set(vm, ArrayExports, jsNumber(0));
    array->number = length;
    return array;
}

// False: a BufferError.
static bool arrayCheckResizable(JSGlobalObject* globalObject, PyRecord* array)
{
    if (!isStillExported(globalObject, array, [&] { return array->get(ArrayExports).asInt32() > 0; })) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseBufferError(globalObject, scope, "cannot resize an array that is exporting buffers"_s);
    return false;
}

// New items are zero. False: a MemoryError, or a BufferError.
static bool arrayResize(JSGlobalObject* globalObject, PyRecord* array, int64_t newLength)
{
    VM& vm = getVM(globalObject);
    int64_t length = arrayLength(array);
    int64_t itemSize = arrayItemSize(array);
    PyByteStorage* storage = arrayStorage(array);
    if (newLength != length && !arrayCheckResizable(globalObject, array))
        return false;
    // What is allocated will do, unless much of it would go to waste.
    if (storage && arrayAllocated(array) >= newLength && length < newLength + 16) {
        if (newLength > length)
            memset(storage->data() + length * itemSize, 0, (newLength - length) * itemSize);
        array->number = newLength;
        return true;
    }
    if (!newLength) {
        array->set(vm, ArrayStorage, jsUndefined());
        array->number = 0;
        return true;
    }
    if (newLength > arrayMaxBytes) {
        arrayRaiseNoMemory(globalObject);
        return false;
    }
    // A sixteenth more than is asked for, so that a series of appends takes linear time.
    int64_t allocated = (newLength >> 4) + (length < 8 ? 3 : 7) + newLength;
    PyByteStorage* newStorage = arrayAllocate(globalObject, allocated, itemSize);
    if (!newStorage)
        return false;
    if (storage)
        memcpy(newStorage->data(), storage->data(), std::min(length, newLength) * itemSize);
    array->set(vm, ArrayStorage, newStorage);
    array->number = newLength;
    return true;
}

// ---- Items ----

template<typename T>
static ALWAYS_INLINE T arrayLoad(const uint8_t* address)
{
    T value;
    memcpy(&value, address, sizeof(T));
    return value;
}

template<typename T>
static ALWAYS_INLINE void arrayStore(uint8_t* address, T value)
{
    memcpy(address, &value, sizeof(T));
}

// The index is in range. Empty: an exception, which only an array of characters made from bytes can cause.
static JSValue arrayGet(JSGlobalObject* globalObject, PyRecord* array, int64_t index)
{
    VM& vm = getVM(globalObject);
    const uint8_t* address = arrayData(array) + index * arrayItemSize(array);
    switch (arrayKind(array)) {
    case ArrayKind::SignedChar:
        return jsNumber(static_cast<int32_t>(arrayLoad<int8_t>(address)));
    case ArrayKind::UnsignedChar:
        return jsNumber(static_cast<int32_t>(*address));
    case ArrayKind::WideChar:
    case ArrayKind::CodePoint: {
        uint32_t character = arrayKind(array) == ArrayKind::WideChar && sizeof(wchar_t) == 2 ? arrayLoad<uint16_t>(address) : arrayLoad<uint32_t>(address);
        if (character > 0x10ffff) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            return raiseValueError(globalObject, scope, "chr() arg not in range(0x110000)"_s);
        }
        return makeStr(vm, stringFromCodePoint(character));
    }
    case ArrayKind::Short:
        return jsNumber(static_cast<int32_t>(arrayLoad<short>(address)));
    case ArrayKind::UnsignedShort:
        return jsNumber(static_cast<int32_t>(arrayLoad<unsigned short>(address)));
    case ArrayKind::Int:
        return jsNumber(arrayLoad<int>(address));
    case ArrayKind::UnsignedInt:
        return jsNumber(arrayLoad<unsigned>(address));
    case ArrayKind::Long:
        return makeInt(globalObject, static_cast<int64_t>(arrayLoad<long>(address)));
    case ArrayKind::UnsignedLong:
        return makeInt(globalObject, static_cast<uint64_t>(arrayLoad<unsigned long>(address)));
    case ArrayKind::LongLong:
        return makeInt(globalObject, static_cast<int64_t>(arrayLoad<long long>(address)));
    case ArrayKind::UnsignedLongLong:
        return makeInt(globalObject, static_cast<uint64_t>(arrayLoad<unsigned long long>(address)));
    case ArrayKind::Half:
        return makeFloatExactly(globalObject, unpackFloat16(arrayLoad<uint16_t>(address)));
    case ArrayKind::Float:
        return makeFloatExactly(globalObject, arrayLoad<float>(address));
    case ArrayKind::Double:
        return makeFloatExactly(globalObject, arrayLoad<double>(address));
    case ArrayKind::ComplexFloat:
        return PyComplex::create(globalObject, arrayLoad<float>(address), arrayLoad<float>(address + 4));
    case ArrayKind::ComplexDouble:
        return PyComplex::create(globalObject, arrayLoad<double>(address), arrayLoad<double>(address + 8));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// int(value.__index__()) as a C long.
static bool arrayConvertLong(JSGlobalObject* globalObject, JSValue value, int64_t& result)
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
    if (!converted || *converted < std::numeric_limits<long>::min() || *converted > std::numeric_limits<long>::max()) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
        return false;
    }
    result = *converted;
    return true;
}

// The same, within the bounds of a narrower type, which messages call `name`.
static bool arrayConvertNarrow(JSGlobalObject* globalObject, JSValue value, int64_t minimum, int64_t maximum, ASCIILiteral name, int64_t& result)
{
    if (!arrayConvertLong(globalObject, value, result))
        return false;
    if (result >= minimum && result <= maximum) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseOverflowError(globalObject, scope, name, result < minimum ? " is less than minimum"_s : " is greater than maximum"_s);
    return false;
}

static bool arrayConvertUnsigned(JSGlobalObject* globalObject, JSValue value, uint64_t maximum, ASCIILiteral negativeMessage, ASCIILiteral tooLargeMessage, uint64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        raiseOverflowError(globalObject, scope, negativeMessage);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted || *converted > maximum) {
        raiseOverflowError(globalObject, scope, tooLargeMessage);
        return false;
    }
    result = *converted;
    return true;
}

// The bytes of an item that holds the value. False: an exception. Runs Python code, after which the array may be another size.
static bool arrayConvert(JSGlobalObject* globalObject, ArrayKind kind, JSValue value, uint8_t* item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t integer;
    uint64_t natural;
    static constexpr ASCIILiteral shortName = "signed short integer"_s;
    static constexpr ASCIILiteral intName = "signed integer"_s;
    switch (kind) {
    case ArrayKind::SignedChar:
        scope.release();
        if (!arrayConvertNarrow(globalObject, value, std::numeric_limits<short>::min(), std::numeric_limits<short>::max(), shortName, integer)
            || !arrayConvertNarrow(globalObject, jsNumber(static_cast<int32_t>(integer)), -128, 127, "signed char"_s, integer))
            return false;
        arrayStore<int8_t>(item, integer);
        return true;
    case ArrayKind::UnsignedChar:
        scope.release();
        if (!arrayConvertNarrow(globalObject, value, 0, 255, "unsigned byte integer"_s, integer))
            return false;
        *item = static_cast<uint8_t>(integer);
        return true;
    case ArrayKind::WideChar:
    case ArrayKind::CodePoint: {
        if (!isStr(value)) {
            raiseTypeError(globalObject, scope, "array item must be a unicode character, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
            return false;
        }
        StrView view(globalObject, strValue(value));
        RETURN_IF_EXCEPTION(scope, false);
        if (view.length() != 1) {
            raiseTypeError(globalObject, scope, "array item must be a unicode character, not a string of length "_s, view.length());
            return false;
        }
        char32_t character = view.at(0);
        if (kind == ArrayKind::WideChar && sizeof(wchar_t) == 2) {
            if (character > 0xffff) {
                raiseTypeError(globalObject, scope, "string "_s, asciiEscape(strRepr(view.view())), " cannot be converted to a single wchar_t character"_s);
                return false;
            }
            arrayStore<uint16_t>(item, character);
            return true;
        }
        arrayStore<uint32_t>(item, character);
        return true;
    }
    case ArrayKind::Short:
        scope.release();
        if (!arrayConvertNarrow(globalObject, value, std::numeric_limits<short>::min(), std::numeric_limits<short>::max(), shortName, integer))
            return false;
        arrayStore<short>(item, integer);
        return true;
    case ArrayKind::UnsignedShort:
        scope.release();
        if (!arrayConvertNarrow(globalObject, value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), intName, integer)
            || !arrayConvertNarrow(globalObject, jsNumber(static_cast<int32_t>(integer)), 0, std::numeric_limits<unsigned short>::max(), "unsigned short"_s, integer))
            return false;
        arrayStore<unsigned short>(item, integer);
        return true;
    case ArrayKind::Int:
        scope.release();
        if (!arrayConvertNarrow(globalObject, value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), intName, integer))
            return false;
        arrayStore<int>(item, integer);
        return true;
    case ArrayKind::UnsignedInt:
    case ArrayKind::UnsignedLong:
        if (!arrayConvertUnsigned(globalObject, value, std::numeric_limits<unsigned long>::max(), "can't convert negative value to unsigned int"_s, "Python int too large to convert to C unsigned long"_s, natural))
            return false;
        if (kind == ArrayKind::UnsignedLong) {
            arrayStore<unsigned long>(item, natural);
            return true;
        }
        if (natural > std::numeric_limits<unsigned>::max()) {
            raiseOverflowError(globalObject, scope, "unsigned int is greater than maximum"_s);
            return false;
        }
        arrayStore<unsigned>(item, natural);
        return true;
    case ArrayKind::Long:
        scope.release();
        if (!arrayConvertLong(globalObject, value, integer))
            return false;
        arrayStore<long>(item, integer);
        return true;
    case ArrayKind::LongLong: {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        auto converted = tryInt64(index);
        if (!converted) {
            raiseOverflowError(globalObject, scope, "int too big to convert"_s);
            return false;
        }
        arrayStore<long long>(item, *converted);
        return true;
    }
    case ArrayKind::UnsignedLongLong:
        scope.release();
        if (!arrayConvertUnsigned(globalObject, value, std::numeric_limits<uint64_t>::max(), "can't convert negative int to unsigned"_s, "int too big to convert"_s, natural))
            return false;
        arrayStore<unsigned long long>(item, natural);
        return true;
    case ArrayKind::Half: {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        // By way of a C float, as in CPython: what is too large for that is an infinity, and no error.
        auto bits = packFloat16(static_cast<float>(number));
        if (!bits) {
            raiseOverflowError(globalObject, scope, "float too large to pack with e format"_s);
            return false;
        }
        arrayStore<uint16_t>(item, *bits);
        return true;
    }
    case ArrayKind::Float: {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        // A conversion in C: what is too large is an infinity.
        arrayStore<float>(item, static_cast<float>(number));
        return true;
    }
    case ArrayKind::Double: {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        arrayStore<double>(item, number);
        return true;
    }
    case ArrayKind::ComplexFloat:
    case ArrayKind::ComplexDouble: {
        double real;
        double imaginary;
        scope.release();
        if (!complexArgument(globalObject, value, real, imaginary))
            return false;
        if (kind == ArrayKind::ComplexDouble) {
            arrayStore<double>(item, real);
            arrayStore<double>(item + 8, imaginary);
            return true;
        }
        arrayStore<float>(item, static_cast<float>(real));
        arrayStore<float>(item + 4, static_cast<float>(imaginary));
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// array[index] = value, for an index that was in range.
static bool arraySet(JSGlobalObject* globalObject, PyRecord* array, int64_t index, JSValue value)
{
    uint8_t item[arrayMaxItemSize];
    if (!arrayConvert(globalObject, arrayKind(array), value, item))
        return false;
    if (index < 0 || index >= arrayLength(array)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseIndexError(globalObject, scope, "array assignment index out of range"_s);
        return false;
    }
    int64_t itemSize = arrayItemSize(array);
    memcpy(arrayData(array) + index * itemSize, item, itemSize);
    return true;
}

// Whether converting the value to an item runs no code of the program.
static ALWAYS_INLINE bool arrayConvertsQuietly(JSValue value)
{
    return !value.isCell() || value.isString() || value.isHeapBigInt();
}

static bool arrayInsertAt(JSGlobalObject* globalObject, PyRecord* array, int64_t where, JSValue value)
{
    uint8_t item[arrayMaxItemSize];
    int64_t length = arrayLength(array);
    // The value is checked before room is made for it.
    if (!arrayConvert(globalObject, arrayKind(array), value, item))
        return false;
    if (!arrayResize(globalObject, array, length + 1))
        return false;
    if (where < 0)
        where = std::max<int64_t>(where + length, 0);
    where = std::min(where, length);
    int64_t itemSize = arrayItemSize(array);
    uint8_t* data = arrayData(array);
    memmove(data + (where + 1) * itemSize, data + where * itemSize, (length - where) * itemSize);
    // CPython converts the value again to store it, which a program can tell if the conversion is its own.
    if (!arrayConvertsQuietly(value))
        return arraySet(globalObject, array, where, value);
    memcpy(data + where * itemSize, item, itemSize);
    return true;
}

static ALWAYS_INLINE bool arrayAppendItem(JSGlobalObject* globalObject, PyRecord* array, JSValue value)
{
    return arrayInsertAt(globalObject, array, std::numeric_limits<int64_t>::max(), value);
}

// del array[low:high], for bounds in range.
static bool arrayDeleteRange(JSGlobalObject* globalObject, PyRecord* array, int64_t low, int64_t high)
{
    if (high <= low)
        return true;
    if (!arrayCheckResizable(globalObject, array))
        return false;
    int64_t itemSize = arrayItemSize(array);
    uint8_t* data = arrayData(array);
    memmove(data + low * itemSize, data + high * itemSize, (arrayLength(array) - high) * itemSize);
    return arrayResize(globalObject, array, arrayLength(array) - (high - low));
}

// array[low:high], for bounds in range: an array, whatever class this one is of.
static PyRecord* arrayCopyRange(JSGlobalObject* globalObject, PyRecord* array, int64_t low, int64_t high)
{
    PyRecord* result = arrayCreate(globalObject, arrayType(globalObject), arrayKind(array), high - low);
    if (result && high > low) {
        int64_t itemSize = arrayItemSize(array);
        memcpy(arrayData(result), arrayData(array) + low * itemSize, (high - low) * itemSize);
    }
    return result;
}

// item == value, where the item was made for the comparison: it is never the same object as the value, so a NaN equals nothing.
static bool arrayItemEquals(JSGlobalObject* globalObject, JSValue item, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = richCompare(globalObject, item, value, CompareOperator::Eq);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

// ---- Filling ----

static bool arrayExtendFromIterator(JSGlobalObject* globalObject, PyRecord* array, JSValue iterator)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        if (!arrayAppendItem(globalObject, array, item))
            return false;
    }
}

static bool arrayExtendWith(JSGlobalObject* globalObject, PyRecord* array, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* source = arrayTryFrom(globalObject, other);
    if (!source) {
        JSValue iterator = getIter(globalObject, other);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, arrayExtendFromIterator(globalObject, array, iterator));
    }
    if (arrayKind(source) != arrayKind(array)) {
        raiseTypeError(globalObject, scope, "can only extend with array of same kind"_s);
        return false;
    }
    int64_t length = arrayLength(array);
    int64_t added = arrayLength(source);
    if (!arrayResize(globalObject, array, length + added))
        return false;
    // The source may be the array itself, which has just moved.
    int64_t itemSize = arrayItemSize(array);
    if (added)
        memcpy(arrayData(array) + length * itemSize, arrayData(source), added * itemSize);
    return true;
}

static bool arrayExtendFromBytes(JSGlobalObject* globalObject, PyRecord* array, JSValue buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferHolder holder(globalObject);
    if (!holder.get(buffer))
        return false;
    if (holder.buffer.itemSize != 1) {
        raiseTypeError(globalObject, scope, "a bytes-like object is required"_s);
        return false;
    }
    std::span<const uint8_t> bytes = holder.buffer.span();
    int64_t itemSize = arrayItemSize(array);
    if (bytes.size() % itemSize) {
        raiseValueError(globalObject, scope, "bytes length not a multiple of item size"_s);
        return false;
    }
    if (bytes.empty())
        return true;
    int64_t length = arrayLength(array);
    if (!arrayResize(globalObject, array, length + static_cast<int64_t>(bytes.size()) / itemSize))
        return false;
    memcpy(arrayData(array) + length * itemSize, bytes.data(), bytes.size());
    return true;
}

// How many items of an array of characters the str makes.
static ALWAYS_INLINE int64_t arrayItemCountOfStr(ArrayKind kind, const StrView& view)
{
    return kind == ArrayKind::WideChar && sizeof(wchar_t) == 2 ? view.unitLength() : view.length();
}

// Stores them from that index on, where there is room for them.
static void arrayStoreStr(PyRecord* array, int64_t index, const StrView& view)
{
    StringView units = view.view();
    if (arrayKind(array) == ArrayKind::WideChar && sizeof(wchar_t) == 2) {
        uint8_t* address = arrayData(array) + index * 2;
        for (unsigned i = 0; i < units.length(); ++i, address += 2)
            arrayStore<uint16_t>(address, units[i]);
        return;
    }
    uint8_t* address = arrayData(array) + index * 4;
    for (unsigned i = 0; i < units.length(); address += 4) {
        char32_t character = units[i++];
        if (U16_IS_LEAD(character) && i < units.length() && U16_IS_TRAIL(units[i]))
            character = U16_GET_SUPPLEMENTARY(character, units[i++]);
        arrayStore<uint32_t>(address, character);
    }
}

// For an array of characters.
static bool arrayExtendFromStr(JSGlobalObject* globalObject, PyRecord* array, JSString* string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    StrView view(globalObject, string);
    RETURN_IF_EXCEPTION(scope, false);
    int64_t length = arrayLength(array);
    if (!arrayResize(globalObject, array, length + arrayItemCountOfStr(arrayKind(array), view)))
        return false;
    arrayStoreStr(array, length, view);
    return true;
}

// ---- array(typecode[, initializer]) ----

static JSValue arrayConstruct(JSGlobalObject* globalObject, PyType* type, JSValue typecodeValue, JSValue initial)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(typecodeValue))
        return raiseTypeError(globalObject, scope, "array() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, typecodeValue));
    String typecode = strValue(typecodeValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (typecode.contains(static_cast<char16_t>(0)))
        return raiseValueError(globalObject, scope, "embedded null character"_s);
    if (!audit(globalObject, "array.__new__"_s, typecodeValue, initial))
        return { };

    if (typecode == "u"_s) {
        bool proceed = warn(globalObject, rt.DeprecationWarning(), "The 'u' type code is deprecated and will be removed in Python 3.16"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (!proceed)
            return { };
    }
    bool isUnicode = typecode == "u"_s || typecode == "w"_s;
    PyRecord* initialArray = initial ? arrayTryFrom(globalObject, initial) : nullptr;
    if (initial && !isUnicode) {
        if (isStr(initial))
            return raiseTypeError(globalObject, scope, "cannot use a str to initialize an array with typecode '"_s, typecode, '\'');
        if (initialArray && arrayKindIsUnicode(arrayKind(initialArray)))
            return raiseTypeError(globalObject, scope, "cannot use a unicode array to initialize an array with typecode '"_s, typecode, '\'');
    }

    // Anything but these is gone through with an iterator.
    JSValue iterator;
    bool isBytes = initial && (PyBytes::is(initial) || PyByteArray::is(initial));
    bool isSequence = initial && (isList(initial) || PyTuple::is(initial));
    if (initial && !isSequence && !isBytes && !(isUnicode && isStr(initial)) && !(initialArray && typecode == arrayDescriptor(initialArray).typecode)) {
        iterator = getIter(globalObject, initial);
        RETURN_IF_EXCEPTION(scope, { });
        initial = JSValue();
        initialArray = nullptr;
    }

    auto kind = arrayKindForTypecode(typecode);
    if (!kind)
        return raiseValueError(globalObject, scope, "bad typecode (must be b, B, u, w, h, H, i, I, l, L, q, Q, e, f, d, Zf or Zd)"_s);

    int64_t length = 0;
    StrView initialStr;
    if (isSequence)
        length = isList(initial) ? listLength(asList(initial)) : PyTuple::from(initial)->length();
    else if (initialArray)
        length = arrayLength(initialArray);
    else if (initial && !isBytes) {
        initialStr = StrView(globalObject, strValue(initial));
        RETURN_IF_EXCEPTION(scope, { });
        length = arrayItemCountOfStr(*kind, initialStr);
    }
    PyRecord* array = arrayCreate(globalObject, type, *kind, length);
    if (!array)
        return { };

    if (isSequence) {
        for (int64_t i = 0; i < length; ++i) {
            JSValue item = getItem(globalObject, initial, jsNumber(static_cast<int32_t>(i)));
            RETURN_IF_EXCEPTION(scope, { });
            if (!arraySet(globalObject, array, i, item))
                return { };
        }
    } else if (isBytes) {
        if (!arrayExtendFromBytes(globalObject, array, initial))
            return { };
    } else if (initialArray) {
        if (length)
            memcpy(arrayData(array), arrayData(initialArray), length * arrayItemSize(array));
    } else if (initial)
        arrayStoreStr(array, 0, initialStr);
    if (iterator && !arrayExtendFromIterator(globalObject, array, iterator))
        return { };
    return array;
}

static JSValue arrayNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    PyType* exactType = arrayType(globalObject);
    // A class derived from array whose __init__() takes keywords may be given them.
    if ((type == exactType || type->lookup(Special::Init) == exactType->lookup(Special::Init)) && !rejectKeywords(globalObject, "array.array"_s, args))
        return { };
    if (!args.size() || args.size() > 2)
        return raiseTypeError(globalObject, scope, "array() takes at "_s, args.size() ? "most 2 arguments ("_s : "least 1 argument ("_s, args.size(), " given)"_s);
    RELEASE_AND_RETURN(scope, arrayConstruct(globalObject, type, args[0], args.get(1)));
}

// ---- Methods ----

// The arguments of a method that in CPython is told which class defines it, and words its complaints in its own way for that.
static bool arrayCheckArguments(JSGlobalObject* globalObject, ASCIILiteral method, const CallArgs& args, unsigned count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned total = args.size() + args.keywordCount();
    if (!count && total) {
        raiseTypeError(globalObject, scope, method, "() takes no arguments"_s);
        return false;
    }
    if (total > count) {
        raiseTypeError(globalObject, scope, method, "() takes at most "_s, count, count == 1 ? " argument ("_s : " arguments ("_s, total, " given)"_s);
        return false;
    }
    if (args.size() < count) {
        raiseTypeError(globalObject, scope, method, "() takes exactly "_s, count, count == 1 ? " positional argument ("_s : " positional arguments ("_s, args.size(), " given)"_s);
        return false;
    }
    return true;
}

static JSValue arrayAppend(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (!arrayAppendItem(globalObject, arrayOf(self), value))
        return { };
    return jsUndefined();
}

static JSValue arrayInsert(JSGlobalObject* globalObject, JSValue self, JSValue indexValue, JSValue value)
{
    int64_t index;
    if (!ssizeArgument(globalObject, indexValue, index) || !arrayInsertAt(globalObject, arrayOf(self), index, value))
        return { };
    return jsUndefined();
}

static JSValue arrayExtend(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    if (!arrayCheckArguments(globalObject, "extend"_s, args, 1) || !arrayExtendWith(globalObject, arrayOf(self), args[0]))
        return { };
    return jsUndefined();
}

static JSValue arrayClear(JSGlobalObject* globalObject, JSValue self)
{
    if (!arrayResize(globalObject, arrayOf(self), 0))
        return { };
    return jsUndefined();
}

static JSValue arrayCopy(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* copy = arrayCopyRange(globalObject, arrayOf(self), 0, arrayLength(arrayOf(self)));
    return copy ? JSValue(copy) : JSValue();
}

static JSValue arrayDeepCopy(JSGlobalObject* globalObject, JSValue self, JSValue) { return arrayCopy(globalObject, self); }

static JSValue arrayBufferInfo(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* array = arrayOf(self);
    return PyTuple::create(globalObject, { makeInt(globalObject, static_cast<uint64_t>(std::bit_cast<uintptr_t>(arrayData(array)))), makeInt(globalObject, arrayLength(array)) });
}

static JSValue arrayCount(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t count = 0;
    for (int64_t i = 0; i < arrayLength(array); ++i) {
        JSValue item = arrayGet(globalObject, array, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool equal = arrayItemEquals(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, { });
        count += equal;
    }
    return makeInt(globalObject, count);
}

// The index of the first item from `start` on and before `stop` that equals the value. -1: none. -2: an exception.
static int64_t arrayFind(JSGlobalObject* globalObject, PyRecord* array, JSValue value, int64_t start, int64_t stop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (int64_t i = start; i < stop && i < arrayLength(array); ++i) {
        JSValue item = arrayGet(globalObject, array, i);
        RETURN_IF_EXCEPTION(scope, -2);
        bool equal = arrayItemEquals(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, -2);
        if (equal)
            return i;
    }
    return -1;
}

static JSValue arrayIndex(JSGlobalObject* globalObject, JSValue self, JSValue value, JSValue startValue, JSValue stopValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t start = 0;
    int64_t stop = std::numeric_limits<int64_t>::max();
    if (startValue && !sliceIndexArgument(globalObject, startValue, start))
        return { };
    if (stopValue && !sliceIndexArgument(globalObject, stopValue, stop))
        return { };
    if (start < 0)
        start = std::max<int64_t>(start + arrayLength(array), 0);
    if (stop < 0)
        stop += arrayLength(array);
    int64_t index = arrayFind(globalObject, array, value, start, stop);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "array.index(x): x not in array"_s);
    return makeInt(globalObject, index);
}

static JSValue arrayContains(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t index = arrayFind(globalObject, arrayOf(self), value, 0, std::numeric_limits<int64_t>::max());
    if (index == -2)
        return { };
    return jsBoolean(index >= 0);
}

static JSValue arrayRemove(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t index = arrayFind(globalObject, array, value, 0, std::numeric_limits<int64_t>::max());
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "array.remove(x): x not in array"_s);
    // The comparison may have left it shorter.
    if (index < arrayLength(array) && !arrayDeleteRange(globalObject, array, index, index + 1))
        return { };
    return jsUndefined();
}

static JSValue arrayPop(JSGlobalObject* globalObject, JSValue self, JSValue indexValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t index = -1;
    if (indexValue && !ssizeArgument(globalObject, indexValue, index))
        return { };
    if (!arrayLength(array))
        return raiseIndexError(globalObject, scope, "pop from empty array"_s);
    if (index < 0)
        index += arrayLength(array);
    if (index < 0 || index >= arrayLength(array))
        return raiseIndexError(globalObject, scope, "pop index out of range"_s);
    JSValue item = arrayGet(globalObject, array, index);
    RETURN_IF_EXCEPTION(scope, { });
    if (!arrayDeleteRange(globalObject, array, index, index + 1))
        return { };
    return item;
}

static JSValue arrayReverse(JSGlobalObject*, JSValue self)
{
    PyRecord* array = arrayOf(self);
    int64_t itemSize = arrayItemSize(array);
    if (arrayLength(array) < 2)
        return jsUndefined();
    uint8_t item[arrayMaxItemSize];
    for (uint8_t *low = arrayData(array), *high = low + (arrayLength(array) - 1) * itemSize; low < high; low += itemSize, high -= itemSize) {
        memcpy(item, low, itemSize);
        memcpy(low, high, itemSize);
        memcpy(high, item, itemSize);
    }
    return jsUndefined();
}

static JSValue arrayByteSwap(JSGlobalObject*, JSValue self)
{
    PyRecord* array = arrayOf(self);
    // The two parts of a complex number stay in their order.
    bool isComplex = arrayKind(array) == ArrayKind::ComplexFloat || arrayKind(array) == ArrayKind::ComplexDouble;
    int64_t width = isComplex ? arrayItemSize(array) / 2 : arrayItemSize(array);
    uint8_t* data = arrayData(array);
    uint8_t* end = data + arrayLength(array) * arrayItemSize(array);
    if (width > 1) {
        for (; data < end; data += width)
            std::reverse(data, data + width);
    }
    return jsUndefined();
}

static JSValue arrayToList(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t length = arrayLength(array);
    JSArray* list = newList(globalObject, static_cast<unsigned>(std::min<int64_t>(length, std::numeric_limits<int32_t>::max())));
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = arrayGet(globalObject, array, i);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

static JSValue arrayFromList(JSGlobalObject* globalObject, JSValue self, JSValue listValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    if (!isList(listValue))
        return raiseTypeError(globalObject, scope, "arg must be list"_s);
    JSArray* list = asList(listValue);
    int64_t count = listLength(list);
    if (!count)
        return jsUndefined();
    int64_t oldLength = arrayLength(array);
    if (!arrayResize(globalObject, array, oldLength + count))
        return { };
    // All or nothing.
    for (int64_t i = 0; i < count; ++i) {
        bool stored = arraySet(globalObject, array, arrayLength(array) - count + i, listGet(list, static_cast<unsigned>(i)));
        if (stored && count != listLength(list)) {
            raiseRuntimeError(globalObject, scope, "list changed size during iteration"_s);
            stored = false;
        }
        if (!stored) {
            JSValue exception = takePendingException(globalObject);
            if (!arrayResize(globalObject, array, oldLength))
                takePendingException(globalObject);
            return raiseException(globalObject, scope, exception);
        }
    }
    return jsUndefined();
}

static JSValue arrayToBytes(JSGlobalObject* globalObject, JSValue self)
{
    return PyBytes::create(globalObject, std::span<const uint8_t>(arrayBytes(self)));
}

static JSValue arrayFromBytes(JSGlobalObject* globalObject, JSValue self, JSValue buffer)
{
    if (!arrayExtendFromBytes(globalObject, arrayOf(self), buffer))
        return { };
    return jsUndefined();
}

static JSValue arrayFromFile(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t count;
    if (!arrayCheckArguments(globalObject, "fromfile"_s, args, 2) || !ssizeArgument(globalObject, args[1], count))
        return { };
    JSValue file = args[0];
    if (count < 0)
        return raiseValueError(globalObject, scope, "n cannot be negative"_s);
    int64_t itemSize = arrayItemSize(array);
    if (count > std::numeric_limits<int64_t>::max() / itemSize)
        return raiseMemoryError(globalObject, scope, String());
    JSValue data = callMethod(globalObject, file, internString(vm, "read"_s), makeInt(globalObject, count * itemSize));
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyBytes::is(data))
        return raiseTypeError(globalObject, scope, "read() didn't return bytes"_s);
    bool isShort = static_cast<int64_t>(PyBytes::from(data)->length()) != count * itemSize;
    if (!arrayExtendFromBytes(globalObject, array, data))
        return { };
    if (isShort)
        return raiseEOFError(globalObject, scope, "read() didn't return enough bytes"_s);
    return jsUndefined();
}

static JSValue arrayToFile(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr size_t blockSize = 64 * 1024;
    if (!arrayCheckArguments(globalObject, "tofile"_s, args, 1))
        return { };
    JSValue file = args[0];
    JSString* write = internString(vm, "write"_s);
    // write() may do anything to the array.
    for (size_t offset = 0; offset < arrayBytes(self).size(); offset += blockSize) {
        auto bytes = arrayBytes(self).subspan(offset);
        JSValue block = PyBytes::create(globalObject, std::span<const uint8_t>(bytes.first(std::min(bytes.size(), blockSize))));
        RETURN_IF_EXCEPTION(scope, { });
        callMethod(globalObject, file, write, block);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue arrayFromUnicode(JSGlobalObject* globalObject, JSValue self, JSValue string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(string))
        return raiseTypeError(globalObject, scope, "fromunicode() argument must be str, not "_s, typeNameOfArgument(globalObject, string));
    if (!arrayKindIsUnicode(arrayKind(arrayOf(self))))
        return raiseValueError(globalObject, scope, "fromunicode() may only be called on unicode type arrays ('u' or 'w')"_s);
    if (!arrayExtendFromStr(globalObject, arrayOf(self), strValue(string)))
        return { };
    return jsUndefined();
}

static JSValue arrayToUnicode(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    if (!arrayKindIsUnicode(arrayKind(array)))
        return raiseValueError(globalObject, scope, "tounicode() may only be called on unicode type arrays ('u' or 'w')"_s);
    if (arrayKind(array) == ArrayKind::CodePoint) {
        int byteOrder = 0;
        RELEASE_AND_RETURN(scope, codecDecodeUTF32(globalObject, arrayBytes(self), String(), &byteOrder));
    }
    // wchar_t: surrogates are let through.
    StrBuilder builder;
    const uint8_t* address = arrayData(array);
    for (int64_t i = 0; i < arrayLength(array); ++i, address += sizeof(wchar_t)) {
        uint32_t character = sizeof(wchar_t) == 2 ? arrayLoad<uint16_t>(address) : arrayLoad<uint32_t>(address);
        if (character > 0x10ffff)
            return raiseValueError(globalObject, scope, "character U+"_s, hex(character, Lowercase), " is not in range [U+0000; U+10ffff]"_s);
        appendCodePoint(builder, character);
    }
    return makeStr(globalObject, builder);
}

static JSValue arraySizeOf(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* array = arrayOf(self);
    return makeInt(globalObject, 64 + arrayAllocated(array) * arrayItemSize(array));
}

static JSValue arrayLen(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, arrayLength(arrayOf(self))); }

static JSValue arrayRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    ASCIILiteral typecode = arrayDescriptor(array).typecode;
    if (!arrayLength(array))
        return makeStr(vm, makeString(array->type()->name(), "('"_s, typecode, "')"_s));
    JSValue items = arrayKindIsUnicode(arrayKind(array)) ? arrayToUnicode(globalObject, self) : arrayToList(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* itemsRepr = repr(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    String text = itemsRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(array->type()->name(), "('"_s, typecode, "', "_s, text, ')'));
}

// ---- Comparison ----

template<typename T>
static int arrayCompareItemsAs(const uint8_t* left, const uint8_t* right, int64_t count)
{
    for (int64_t i = 0; i < count; ++i, left += sizeof(T), right += sizeof(T)) {
        T a = arrayLoad<T>(left);
        T b = arrayLoad<T>(right);
        if (a != b)
            return a < b ? -1 : 1;
    }
    return 0;
}

// For integers and characters.
static int arrayCompareItems(ArrayKind kind, const uint8_t* left, const uint8_t* right, int64_t count)
{
    switch (kind) {
    case ArrayKind::SignedChar:
        return arrayCompareItemsAs<int8_t>(left, right, count);
    case ArrayKind::UnsignedChar:
        return arrayCompareItemsAs<uint8_t>(left, right, count);
    case ArrayKind::WideChar:
        return arrayCompareItemsAs<wchar_t>(left, right, count);
    case ArrayKind::CodePoint:
        return arrayCompareItemsAs<uint32_t>(left, right, count);
    case ArrayKind::Short:
        return arrayCompareItemsAs<short>(left, right, count);
    case ArrayKind::UnsignedShort:
        return arrayCompareItemsAs<unsigned short>(left, right, count);
    case ArrayKind::Int:
        return arrayCompareItemsAs<int>(left, right, count);
    case ArrayKind::UnsignedInt:
        return arrayCompareItemsAs<unsigned>(left, right, count);
    case ArrayKind::Long:
        return arrayCompareItemsAs<long>(left, right, count);
    case ArrayKind::UnsignedLong:
        return arrayCompareItemsAs<unsigned long>(left, right, count);
    case ArrayKind::LongLong:
        return arrayCompareItemsAs<long long>(left, right, count);
    case ArrayKind::UnsignedLongLong:
        return arrayCompareItemsAs<unsigned long long>(left, right, count);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

template<CompareOperator op, typename T>
static ALWAYS_INLINE bool arrayOrderHolds(T left, T right)
{
    switch (op) {
    case CompareOperator::Lt:
        return left < right;
    case CompareOperator::LtE:
        return left <= right;
    case CompareOperator::Eq:
        return left == right;
    case CompareOperator::NotEq:
        return left != right;
    case CompareOperator::Gt:
        return left > right;
    default:
        return left >= right;
    }
}

template<CompareOperator op>
static JSValue arrayCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* left = arrayOf(self);
    PyRecord* right = arrayTryFrom(globalObject, other);
    if (!right)
        return runtime(globalObject).notImplemented();
    if ((op == CompareOperator::Eq || op == CompareOperator::NotEq) && arrayLength(left) != arrayLength(right))
        return jsBoolean(op == CompareOperator::NotEq);

    int64_t common = std::min(arrayLength(left), arrayLength(right));
    if (arrayKind(left) == arrayKind(right) && (arrayDescriptor(left).isInteger || arrayKindIsUnicode(arrayKind(left)))) {
        int order = arrayCompareItems(arrayKind(left), arrayData(left), arrayData(right), common);
        if (order)
            return jsBoolean(arrayOrderHolds<op>(order, 0));
        return jsBoolean(arrayOrderHolds<op>(arrayLength(left), arrayLength(right)));
    }

    // The first place where the items differ decides.
    for (int64_t i = 0; i < common; ++i) {
        JSValue x = arrayGet(globalObject, left, i);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue y = arrayGet(globalObject, right, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool equal = arrayItemEquals(globalObject, x, y);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            continue;
        if (op == CompareOperator::Eq || op == CompareOperator::NotEq)
            return jsBoolean(op == CompareOperator::NotEq);
        RELEASE_AND_RETURN(scope, richCompare(globalObject, x, y, op));
    }
    return jsBoolean(arrayOrderHolds<op>(arrayLength(left), arrayLength(right)));
}

// ---- + and * ----

static JSValue arrayAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* left = arrayOf(self);
    PyRecord* right = arrayTryFrom(globalObject, other);
    if (!right)
        return raiseTypeError(globalObject, scope, "can only append array (not \""_s, typeName(globalObject, other), "\") to array"_s);
    if (arrayKind(left) != arrayKind(right))
        return raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
    PyRecord* result = arrayCreate(globalObject, arrayType(globalObject), arrayKind(left), arrayLength(left) + arrayLength(right));
    if (!result)
        return { };
    int64_t itemSize = arrayItemSize(left);
    if (arrayLength(left))
        memcpy(arrayData(result), arrayData(left), arrayLength(left) * itemSize);
    if (arrayLength(right))
        memcpy(arrayData(result) + arrayLength(left) * itemSize, arrayData(right), arrayLength(right) * itemSize);
    return result;
}

static JSValue arrayIAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!arrayTryFrom(globalObject, other))
        return raiseTypeError(globalObject, scope, "can only extend array with array (not \""_s, typeName(globalObject, other), "\")"_s);
    if (!arrayExtendWith(globalObject, arrayOf(self), other))
        return { };
    return self;
}

// Fills `total` bytes with copies of the first `unit` of them.
static void arrayRepeatBytes(uint8_t* data, int64_t unit, int64_t total)
{
    for (int64_t filled = unit; filled < total;) {
        int64_t chunk = std::min(filled, total - filled);
        memcpy(data + filled, data, chunk);
        filled += chunk;
    }
}

static JSValue arrayMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t count = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    count = std::max<int64_t>(count, 0);
    int64_t length = arrayLength(array);
    if (length && count > std::numeric_limits<int64_t>::max() / length)
        return raiseMemoryError(globalObject, scope, String());
    PyRecord* result = arrayCreate(globalObject, arrayType(globalObject), arrayKind(array), length * count);
    if (!result)
        return { };
    if (length * count) {
        int64_t unit = length * arrayItemSize(array);
        memcpy(arrayData(result), arrayData(array), unit);
        arrayRepeatBytes(arrayData(result), unit, unit * count);
    }
    return result;
}

static JSValue arrayIMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    int64_t count = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    int64_t length = arrayLength(array);
    if (!length || count == 1)
        return self;
    count = std::max<int64_t>(count, 0);
    int64_t unit = length * arrayItemSize(array);
    if (count && unit > std::numeric_limits<int64_t>::max() / count)
        return raiseMemoryError(globalObject, scope, String());
    if (!arrayResize(globalObject, array, length * count))
        return { };
    if (count)
        arrayRepeatBytes(arrayData(array), unit, unit * count);
    return self;
}

// ---- Subscripts ----

// What the slice selects of the array.
static bool arraySliceIndices(JSGlobalObject* globalObject, PyRecord* array, PySlice* slice, PySlice::Indices& indices)
{
    if (!slice->unpack(globalObject, indices))
        return false;
    PySlice::adjust(arrayLength(array), indices);
    return true;
}

static JSValue arrayGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, runtime(globalObject).IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            index += arrayLength(array);
        if (index < 0 || index >= arrayLength(array))
            return raiseIndexError(globalObject, scope, "array index out of range"_s);
        RELEASE_AND_RETURN(scope, arrayGet(globalObject, array, index));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "array indices must be integers"_s);
    PySlice::Indices indices;
    if (!arraySliceIndices(globalObject, array, slice, indices))
        return { };
    if (indices.length <= 0 || indices.step == 1) {
        PyRecord* result = indices.length <= 0 ? arrayCreate(globalObject, arrayType(globalObject), arrayKind(array), 0) : arrayCopyRange(globalObject, array, indices.start, indices.start + indices.length);
        return result ? JSValue(result) : JSValue();
    }
    PyRecord* result = arrayCreate(globalObject, arrayType(globalObject), arrayKind(array), indices.length);
    if (!result)
        return { };
    int64_t itemSize = arrayItemSize(array);
    uint8_t* target = arrayData(result);
    const uint8_t* source = arrayData(array);
    for (int64_t i = 0; i < indices.length; ++i)
        memcpy(target + i * itemSize, source + (indices.start + i * indices.step) * itemSize, itemSize);
    return result;
}

// array[key] = value, or del array[key] if there is no value.
static JSValue arrayAssign(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    PySlice::Indices indices;
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, runtime(globalObject).IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            index += arrayLength(array);
        if (index < 0 || index >= arrayLength(array))
            return raiseIndexError(globalObject, scope, "array assignment index out of range"_s);
        if (value) {
            if (!arraySet(globalObject, array, index, value))
                return { };
            return jsUndefined();
        }
        indices = { index, index + 1, 1, 1 };
    } else if (auto* slice = PySlice::tryFrom(key)) {
        if (!arraySliceIndices(globalObject, array, slice, indices))
            return { };
    } else
        return raiseTypeError(globalObject, scope, "array indices must be integers"_s);

    PyRecord* other = nullptr;
    int64_t needed = 0;
    if (value) {
        other = arrayTryFrom(globalObject, value);
        if (!other)
            return raiseTypeError(globalObject, scope, "can only assign array (not \""_s, typeName(globalObject, value), "\") to array slice"_s);
        if (other == array) {
            // array[i:j] = array
            other = arrayCopyRange(globalObject, array, 0, arrayLength(array));
            if (!other)
                return { };
        }
        if (arrayKind(other) != arrayKind(array))
            return raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
        needed = arrayLength(other);
    }

    int64_t itemSize = arrayItemSize(array);
    int64_t length = arrayLength(array);
    int64_t start = indices.start;
    int64_t step = indices.step;
    int64_t selected = std::max<int64_t>(indices.length, 0);
    // Where a slice with a step of 1 ends. For array[2:1] = ..., what is inserted goes at the start.
    int64_t stop = start + selected;
    if ((!needed || selected != needed) && !arrayCheckResizable(globalObject, array))
        return { };

    if (step == 1) {
        if (selected > needed) {
            uint8_t* data = arrayData(array);
            memmove(data + (start + needed) * itemSize, data + stop * itemSize, (length - stop) * itemSize);
            if (!arrayResize(globalObject, array, length + needed - selected))
                return { };
        } else if (selected < needed) {
            if (!arrayResize(globalObject, array, length + needed - selected))
                return { };
            uint8_t* data = arrayData(array);
            memmove(data + (start + needed) * itemSize, data + stop * itemSize, (length - stop) * itemSize);
        }
        if (needed)
            memcpy(arrayData(array) + start * itemSize, arrayData(other), needed * itemSize);
        return jsUndefined();
    }

    if (!needed) {
        // Whether it is `del array[::2]` or `array[::2] = array('i')`: what is between the items that go closes up.
        if (selected <= 0)
            return jsUndefined();
        if (step < 0) {
            start += step * (selected - 1);
            step = -step;
        }
        uint8_t* data = arrayData(array);
        int64_t target = start;
        for (int64_t i = 0; i < selected; ++i) {
            // What follows the item stays, up to the next one that goes, or to the end. (The step may be huge.)
            int64_t position = start + i * step;
            int64_t kept = i + 1 < selected ? step - 1 : length - position - 1;
            memmove(data + target * itemSize, data + (position + 1) * itemSize, kept * itemSize);
            target += kept;
        }
        if (!arrayResize(globalObject, array, length - selected))
            return { };
        return jsUndefined();
    }

    if (needed != selected)
        return raiseValueError(globalObject, scope, "attempt to assign array of size "_s, needed, " to extended slice of size "_s, selected);
    uint8_t* data = arrayData(array);
    const uint8_t* source = arrayData(other);
    for (int64_t i = 0; i < selected; ++i)
        memcpy(data + (start + i * step) * itemSize, source + i * itemSize, itemSize);
    return jsUndefined();
}

static JSValue arraySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value) { return arrayAssign(globalObject, self, key, value); }
static JSValue arrayDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key) { return arrayAssign(globalObject, self, key, JSValue()); }

// ---- Pickling ----

static JSValue arrayReduceEx(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* array = arrayOf(self);
    if (!arrayCheckArguments(globalObject, "__reduce_ex__"_s, args, 1))
        return { };
    JSValue protocolValue = args[0];
    JSValue module = importModule(globalObject, "array"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue reconstructor = getAttr(globalObject, module, internString(vm, "_array_reconstructor"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInt(protocolValue))
        return raiseTypeError(globalObject, scope, "__reduce_ex__ argument should be an integer"_s);
    int64_t protocol;
    if (!arrayConvertLong(globalObject, protocolValue, protocol))
        return { };
    JSValue dict = getAttrIfPresent(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        dict = jsUndefined();
    JSValue typecode = makeStr(vm, arrayDescriptor(array).typecode);
    if (protocol < 3) {
        JSValue list = arrayToList(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        return PyTuple::create(globalObject, { JSValue(array->type()), PyTuple::create(globalObject, { typecode, list }), dict });
    }
    JSValue bytes = arrayToBytes(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue arguments = PyTuple::create(globalObject, { JSValue(array->type()), typecode, jsNumber(arrayDescriptor(array).machineFormat), bytes });
    return PyTuple::create(globalObject, { reconstructor, arguments, dict });
}

// The size of an item in each machine format, as CPython has them: those of the four Unicode formats are twice what they should be.
static constexpr uint8_t arrayMachineFormatSizes[] = { 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 8, 8, 8, 8, 4, 4, 8, 8, 4, 4, 8, 8, 8, 8, 16, 16, 2, 2 };
static_assert(std::size(arrayMachineFormatSizes) == ArrayMachineFormatMax + 1);

template<typename T>
static ALWAYS_INLINE T arrayLoadOrdered(const uint8_t* address, bool isBigEndian)
{
    T value = arrayLoad<T>(address);
    return isBigEndian ? flipBytes(value) : value;
}

static JSValue arrayReconstructor(JSGlobalObject* globalObject, JSValue typeValue, JSValue typecodeValue, JSValue formatValue, JSValue items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(typecodeValue))
        return raiseTypeError(globalObject, scope, "_array_reconstructor() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, typecodeValue));
    String typecode = strValue(typecodeValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (typecode.contains(static_cast<char16_t>(0)))
        return raiseValueError(globalObject, scope, "embedded null character"_s);
    int format = toCInt(globalObject, formatValue);
    RETURN_IF_EXCEPTION(scope, { });

    if (!PyType::is(typeValue))
        return raiseTypeError(globalObject, scope, "first argument must be a type object, not "_s, typeName(globalObject, typeValue));
    PyType* type = PyType::from(typeValue);
    if (!type->isSubtypeOf(arrayType(globalObject)))
        return raiseTypeError(globalObject, scope, type->name(), " is not a subtype of array.array"_s);
    auto kind = arrayKindForTypecode(typecode);
    if (!kind)
        return raiseValueError(globalObject, scope, "second argument must be a valid type code"_s);
    if (format < 0 || format > ArrayMachineFormatMax)
        return raiseValueError(globalObject, scope, "third argument must be a valid machine format code."_s);
    if (!PyBytes::is(items))
        return raiseTypeError(globalObject, scope, "fourth argument should be bytes, not "_s, typeName(globalObject, items));

    // The bytes are as this machine has them.
    if (format == arrayDescriptors[static_cast<unsigned>(*kind)].machineFormat)
        RELEASE_AND_RETURN(scope, arrayConstruct(globalObject, type, typecodeValue, items));

    std::span<const uint8_t> bytes = PyBytes::from(items)->span();
    if (bytes.size() % arrayMachineFormatSizes[format])
        return raiseValueError(globalObject, scope, "string length not a multiple of item size"_s);

    bool isBigEndian = format >= ArrayUnsignedInt16LE && (format & 1);
    JSValue converted;
    if (format >= ArrayUTF16LE && format <= ArrayUTF32BE) {
        int byteOrder = isBigEndian ? 1 : -1;
        converted = format <= ArrayUTF16BE ? codecDecodeUTF16(globalObject, bytes, "strict"_s, &byteOrder) : codecDecodeUTF32(globalObject, bytes, "strict"_s, &byteOrder);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        size_t size = arrayMachineFormatSizes[format];
        JSArray* list = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSigned = false;
        if (format <= ArraySignedInt64BE) {
            isSigned = size == 1 ? format == ArraySignedInt8 : (format - ArrayUnsignedInt16LE) & 2;
            // The last kind of integer of that size and signedness, whatever the typecode given.
            for (auto& descriptor : arrayDescriptors) {
                if (descriptor.isInteger && descriptor.itemSize == size && descriptor.isSigned == isSigned)
                    typecodeValue = makeStr(vm, descriptor.typecode);
            }
        }
        for (size_t offset = 0; offset < bytes.size(); offset += size) {
            const uint8_t* address = bytes.data() + offset;
            JSValue item;
            switch (format) {
            case ArrayHalfLE:
            case ArrayHalfBE:
                item = makeFloatExactly(globalObject, unpackFloat16(arrayLoadOrdered<uint16_t>(address, isBigEndian)));
                break;
            case ArrayFloatLE:
            case ArrayFloatBE:
                item = makeFloatExactly(globalObject, unpackFloat32(arrayLoadOrdered<uint32_t>(address, isBigEndian)));
                break;
            case ArrayDoubleLE:
            case ArrayDoubleBE:
                item = makeFloatExactly(globalObject, std::bit_cast<double>(arrayLoadOrdered<uint64_t>(address, isBigEndian)));
                break;
            case ArrayComplexFloatLE:
            case ArrayComplexFloatBE:
                item = PyComplex::create(globalObject, unpackFloat32(arrayLoadOrdered<uint32_t>(address, isBigEndian)), unpackFloat32(arrayLoadOrdered<uint32_t>(address + 4, isBigEndian)));
                break;
            case ArrayComplexDoubleLE:
            case ArrayComplexDoubleBE:
                item = PyComplex::create(globalObject, std::bit_cast<double>(arrayLoadOrdered<uint64_t>(address, isBigEndian)), std::bit_cast<double>(arrayLoadOrdered<uint64_t>(address + 8, isBigEndian)));
                break;
            default:
                item = intFromByteSpan(globalObject, bytes.subspan(offset, size), !isBigEndian, isSigned);
                RETURN_IF_EXCEPTION(scope, { });
            }
            listAppend(globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        converted = list;
    }
    ensureStillAliveHere(items);
    RELEASE_AND_RETURN(scope, arrayConstruct(globalObject, type, typecodeValue, converted));
}

// ---- The buffer protocol ----

static void arrayChangeExports(JSGlobalObject* globalObject, JSValue self, int change)
{
    PyRecord* array = arrayOf(self);
    array->set(getVM(globalObject), ArrayExports, jsNumber(array->get(ArrayExports).asInt32() + change));
}

static void arrayReleaseExport(JSGlobalObject* globalObject, JSValue self) { arrayChangeExports(globalObject, self, -1); }

static JSValue arrayBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flags)
{
    PyRecord* array = arrayOf(self);
    // A wchar_t of four bytes is what the module struct knows as 'w'.
    ASCIILiteral format = arrayKind(array) == ArrayKind::WideChar && sizeof(wchar_t) >= 4 ? "w"_s : arrayDescriptor(array).typecode;
    JSValue view = exportBuffer(globalObject, self, flags, { arrayStorage(array), 0, arrayBytes(self).size(), false, static_cast<size_t>(arrayItemSize(array)), format, arrayReleaseExport });
    if (view)
        arrayChangeExports(globalObject, self, 1);
    return view;
}

static JSValue arrayGetTypecode(JSGlobalObject* globalObject, JSValue self) { return makeStr(getVM(globalObject), arrayDescriptor(arrayOf(self)).typecode); }
static JSValue arrayGetItemSize(JSGlobalObject*, JSValue self) { return jsNumber(static_cast<int32_t>(arrayItemSize(arrayOf(self)))); }

// ---- The iterator ----

enum ArrayIteratorField : unsigned {
    ArrayIteratorArray, // None once it is exhausted.
    ArrayIteratorFieldCount,
};
// PyRecord::number is the index of the next item.

static JSValue arrayIter(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* iterator = PyRecord::create(globalObject, runtime(globalObject).typeFor(arrayIteratorTypeDef), ArrayIteratorFieldCount);
    iterator->set(getVM(globalObject), ArrayIteratorArray, self);
    return iterator;
}

static JSValue arrayIteratorSelf(JSGlobalObject*, JSValue self) { return self; }

static JSValue arrayIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    JSValue array = iterator->get(ArrayIteratorArray);
    if (isNone(array))
        return raiseStopIteration(globalObject, scope);
    if (iterator->number < arrayLength(arrayOf(array)))
        RELEASE_AND_RETURN(scope, arrayGet(globalObject, arrayOf(array), iterator->number++));
    iterator->set(vm, ArrayIteratorArray, jsUndefined());
    return raiseStopIteration(globalObject, scope);
}

static JSValue arrayIteratorReduce(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    if (!arrayCheckArguments(globalObject, "__reduce__"_s, args, 0))
        return { };
    JSValue iter = builtinNamed(globalObject, "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue array = iterator->get(ArrayIteratorArray);
    if (isNone(array))
        return PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { JSValue(rt.emptyTuple()) }) });
    return PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { array }), makeInt(globalObject, iterator->number) });
}

static JSValue arrayIteratorSetState(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    if (!isInt(state))
        return raiseTypeError(globalObject, scope, "an integer is required"_s);
    auto index = tryInt64(intValue(state));
    if (!index)
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    JSValue array = iterator->get(ArrayIteratorArray);
    if (!isNone(array))
        iterator->number = std::clamp<int64_t>(*index, 0, arrayLength(arrayOf(array)));
    return jsUndefined();
}

// ---- The types and the module ----

static constexpr NativeDef arrayMethods[] = {
    PY_NEW(arrayNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", arrayRepr, "($self, /)"),
    PY_SLOT("__eq__", arrayCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", arrayCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", arrayCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", arrayCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", arrayCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", arrayCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", arrayIter, "($self, /)"),
    PY_SLOT("__len__", arrayLen, "($self, /)"),
    PY_SLOT("__getitem__", arrayGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", arraySetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", arrayDelItem, "($self, key, /)"),
    PY_SLOT("__contains__", arrayContains, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__add__", arrayAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__mul__", arrayMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", arrayMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__iadd__", arrayIAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__imul__", arrayIMul, "($self, value, /)"),
    PY_SLOT("__buffer__", arrayBuffer, "($self, flags, /)"),
    PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
    PY_METHOD("append", arrayAppend, "($self, v, /)"),
    PY_METHOD("buffer_info", arrayBufferInfo, "($self, /)"),
    PY_METHOD("byteswap", arrayByteSwap, "($self, /)"),
    PY_METHOD("clear", arrayClear, "($self, /)"),
    PY_METHOD("__copy__", arrayCopy, "($self, /)"),
    PY_METHOD("count", arrayCount, "($self, v, /)"),
    PY_METHOD("__deepcopy__", arrayDeepCopy, "($self, unused, /)"),
    PY_METHOD("extend", arrayExtend, "($self, /, *args, **kwargs)"),
    PY_METHOD("fromfile", arrayFromFile, "($self, /, *args, **kwargs)"),
    PY_METHOD("fromlist", arrayFromList, "($self, list, /)"),
    PY_METHOD("frombytes", arrayFromBytes, "($self, buffer, /)"),
    PY_METHOD("fromunicode", arrayFromUnicode, "($self, ustr, /)"),
    PY_METHOD("index", arrayIndex, "($self, v, start=0, stop=sys.maxsize, /)"),
    PY_METHOD("insert", arrayInsert, "($self, i, v, /)"),
    PY_METHOD("pop", arrayPop, "($self, i=-1, /)"),
    PY_METHOD("__reduce_ex__", arrayReduceEx, "($self, /, *args, **kwargs)"),
    PY_METHOD("remove", arrayRemove, "($self, v, /)"),
    PY_METHOD("reverse", arrayReverse, "($self, /)"),
    PY_METHOD("tofile", arrayToFile, "($self, /, *args, **kwargs)"),
    PY_METHOD("tolist", arrayToList, "($self, /)"),
    PY_METHOD("tobytes", arrayToBytes, "($self, /)"),
    PY_METHOD("tounicode", arrayToUnicode, "($self, /)"),
    PY_METHOD("__sizeof__", arraySizeOf, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet arrayGetSets[] = {
    { "typecode"_s, arrayGetTypecode },
    { "itemsize"_s, arrayGetItemSize },
};

constexpr TypeDef arrayTypeDef { "array.array"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::Sequence, TypeFlag::HasWeakref }, arrayMethods, arrayGetSets,
    "array(typecode [, initializer]) -> array\n\n"
    "Return a new array whose items are restricted by typecode, and\n"
    "initialized from the optional initializer value, which must be a list,\n"
    "string or iterable over elements of the appropriate type.\n"_s };

static constexpr NativeDef arrayIteratorMethods[] = {
    PY_SLOT("__iter__", arrayIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", arrayIteratorNext, "($self, /)"),
    PY_METHOD("__reduce__", arrayIteratorReduce, "($self, /, *args, **kwargs)"),
    PY_METHOD("__setstate__", arrayIteratorSetState, "($self, state, /)"),
};

constexpr TypeDef arrayIteratorTypeDef { "array.arrayiterator"_s, { TypeFlag::Immutable }, arrayIteratorMethods, { }, { } };

static constexpr NativeDef arrayFunctions[] = {
    PY_FUNCTION("_array_reconstructor", arrayReconstructor, "(arraytype, typecode, mformat_code, items, /)"),
};

void initializeArrayModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    module->add(globalObject, "__doc__"_s, makeStr(vm,
        "This module defines an object type which can efficiently represent\n"
        "an array of basic values: characters, integers, floating-point\n"
        "numbers, complex numbers.  Arrays are sequence types and behave very much like lists,\n"
        "except that the type of objects stored in them is constrained.\n"_s));
    module->addFunctions(globalObject, arrayFunctions);

    PyType* type = arrayType(globalObject);
    if (!type->dict()->get(rt.name(Special::Hash))) {
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    }
    module->add(globalObject, "ArrayType"_s, type);
    module->add(globalObject, "array"_s, type);

    PyTuple* typecodes = PyTuple::create(globalObject, std::size(arrayDescriptors));
    for (unsigned i = 0; i < std::size(arrayDescriptors); ++i)
        typecodes->initializeAt(vm, i, makeStr(vm, arrayDescriptors[i].typecode));
    module->add(globalObject, "typecodes"_s, typecodes);

    // collections.abc.MutableSequence.register(array)
    JSValue abstractClasses = importModule(globalObject, "collections.abc"_s);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue mutableSequence = getAttr(globalObject, abstractClasses, internString(vm, "MutableSequence"_s));
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    callMethod(globalObject, mutableSequence, internString(vm, "register"_s), type);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
