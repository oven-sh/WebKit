/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "CApiHooks.h"
#include "HeapIterationScope.h"
#include "HeapUtil.h"
#include "JSBigIntInlines.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

JSValue makeCapsuleWithMemory(JSGlobalObject*, ASCIILiteral name, JSCell* memory, const void* pointer); // PyTypesModule.cpp

namespace Ctypes {

// ---- The simple types ----

template<typename T>
static constexpr ForeignKind foreignKindOfInteger()
{
    constexpr bool isSigned = std::is_signed_v<T>;
    if constexpr (sizeof(T) == 1)
        return isSigned ? ForeignKind::SInt8 : ForeignKind::UInt8;
    else if constexpr (sizeof(T) == 2)
        return isSigned ? ForeignKind::SInt16 : ForeignKind::UInt16;
    else if constexpr (sizeof(T) == 4)
        return isSigned ? ForeignKind::SInt32 : ForeignKind::UInt32;
    else
        return isSigned ? ForeignKind::SInt64 : ForeignKind::UInt64;
}

template<typename T>
static constexpr CodeDescription describeInteger(ASCIILiteral text, ASCIILiteral format)
{
    return { text, std::is_signed_v<T> ? Code::SignedInt : Code::UnsignedInt, sizeof(T), alignof(T), foreignKindOfInteger<T>(), true, format };
}

struct ComplexFloat {
    float real;
    float imaginary;
};
struct ComplexDouble {
    double real;
    double imaginary;
};
struct ComplexLongDouble {
    long double real;
    long double imaginary;
};

// In a buffer, "l" and "L" are four bytes whatever the machine makes of long.
static constexpr CodeDescription codeDescriptions[] = {
    describeInteger<signed char>("b"_s, "b"_s),
    describeInteger<unsigned char>("B"_s, "B"_s),
    describeInteger<short>("h"_s, "h"_s),
    describeInteger<unsigned short>("H"_s, "H"_s),
    describeInteger<int>("i"_s, "i"_s),
    describeInteger<unsigned>("I"_s, "I"_s),
    describeInteger<long>("l"_s, sizeof(long) == 8 ? "q"_s : "l"_s),
    describeInteger<unsigned long>("L"_s, sizeof(long) == 8 ? "Q"_s : "L"_s),
    describeInteger<long long>("q"_s, "q"_s),
    describeInteger<unsigned long long>("Q"_s, "Q"_s),
    { "f"_s, Code::Float, sizeof(float), alignof(float), ForeignKind::Float, true, "f"_s },
    { "d"_s, Code::Double, sizeof(double), alignof(double), ForeignKind::Double, true, "d"_s },
    { "g"_s, Code::LongDouble, sizeof(long double), alignof(long double), ForeignKind::LongDouble, false, "g"_s },
    { "Zf"_s, Code::ComplexFloat, sizeof(ComplexFloat), alignof(ComplexFloat), ForeignKind::ComplexFloat, true, "Zf"_s },
    { "Zd"_s, Code::ComplexDouble, sizeof(ComplexDouble), alignof(ComplexDouble), ForeignKind::ComplexDouble, true, "Zd"_s },
    { "Zg"_s, Code::ComplexLongDouble, sizeof(ComplexLongDouble), alignof(ComplexLongDouble), ForeignKind::ComplexLongDouble, false, "Zg"_s },
    { "?"_s, Code::Bool, sizeof(bool), alignof(bool), ForeignKind::UInt8, true, "?"_s },
    { "v"_s, Code::VariantBool, sizeof(short), alignof(short), ForeignKind::SInt16, false, "v"_s },
    { "c"_s, Code::Char, 1, 1, ForeignKind::SInt8, false, "c"_s },
    { "u"_s, Code::WChar, sizeof(wchar_t), alignof(wchar_t), foreignKindOfInteger<std::make_signed_t<wchar_t>>(), false, "u"_s },
    { "z"_s, Code::CharPointer, sizeof(void*), alignof(void*), ForeignKind::Pointer, false, "z"_s },
    { "Z"_s, Code::WCharPointer, sizeof(void*), alignof(void*), ForeignKind::Pointer, false, "Z"_s },
    { "P"_s, Code::VoidPointer, sizeof(void*), alignof(void*), ForeignKind::Pointer, false, "P"_s },
    { "O"_s, Code::Object, sizeof(void*), alignof(void*), ForeignKind::Pointer, false, "O"_s },
    // These two are for fields that are arrays of characters. CPython lets a class have them for a _type_ all the same.
    { "s"_s, Code::CharArray, sizeof(void*), alignof(void*), ForeignKind::Pointer, false, "s"_s },
    { "U"_s, Code::WCharArray, sizeof(void*), alignof(void*), ForeignKind::Pointer, false, "U"_s },
};

const CodeDescription* describeCode(StringView text)
{
    for (auto& description : codeDescriptions) {
        if (text == description.text)
            return &description;
    }
    return nullptr;
}

// ---- Integers, pointers, text ----

uint64_t lowBitsOfInt(JSValue value)
{
    ASSERT(isExactInt(value));
    if (isSmallInt(value))
        return static_cast<uint64_t>(smallIntValue(value));
    return JSBigInt::toBigUInt64(value);
}

bool pointerFromInt(JSGlobalObject* globalObject, JSValue value, uintptr_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isInt(value)) {
        raiseTypeError(globalObject, scope, "an integer is required"_s);
        return false;
    }
    JSValue exact = intValue(value);
    if (intSign(exact) < 0) {
        auto converted = tryInt64(exact);
        if (!converted) {
            raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
            return false;
        }
        result = static_cast<uintptr_t>(*converted);
        return true;
    }
    auto converted = tryUInt64(exact);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C unsigned long"_s);
        return false;
    }
    result = static_cast<uintptr_t>(*converted);
    return true;
}

JSValue intFromPointer(JSGlobalObject* globalObject, const void* pointer)
{
    return makeInt(globalObject, static_cast<uint64_t>(std::bit_cast<uintptr_t>(pointer)));
}

PyByteStorage* wideCharactersOf(JSGlobalObject* globalObject, JSString* string, size_t& length)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    StrView view(globalObject, string);
    RETURN_IF_EXCEPTION(scope, nullptr);
    length = sizeof(wchar_t) == 2 ? view.unitLength() : view.length();
    PyByteStorage* storage = PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), (length + 1) * sizeof(wchar_t));
    if (!storage) {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    auto* out = reinterpret_cast<wchar_t*>(storage->data());
    StringView text = view.view();
    if (sizeof(wchar_t) == 2 || view.isSimple()) {
        for (unsigned i = 0; i < text.length(); ++i)
            *out++ = static_cast<wchar_t>(text[i]);
    } else {
        for (unsigned i = 0; i < text.length(); ++i) {
            char32_t unit = text[i];
            if (U16_IS_LEAD(unit) && i + 1 < text.length() && U16_IS_TRAIL(text[i + 1]))
                unit = U16_GET_SUPPLEMENTARY(unit, text[++i]);
            *out++ = static_cast<wchar_t>(unit);
        }
    }
    *out = 0;
    return storage;
}

JSValue strFromWideCharacters(JSGlobalObject* globalObject, const wchar_t* characters, size_t length)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    StrBuilder builder;
    for (size_t i = 0; i < length; ++i) {
        char32_t character = static_cast<char32_t>(characters[i]);
        if (sizeof(wchar_t) == 2)
            builder.append(static_cast<char16_t>(character));
        else if (character > 0x10ffff)
            return raiseValueError(globalObject, scope, "character U+"_s, hex(static_cast<uint32_t>(character), Lowercase), " is not in range [U+0000; U+10ffff]"_s);
        else
            appendCodePoint(builder, character);
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

JSValue capsuleForMemory(JSGlobalObject* globalObject, PyByteStorage* storage)
{
    return makeCapsuleWithMemory(globalObject, "_ctypes/cfield.c pymem"_s, storage, storage->data());
}

// ---- Objects ----

JSValue objectFromBits(JSGlobalObject* globalObject, uint64_t bits, JSValue hint)
{
    if (hint && identityBits(hint) == bits) [[likely]]
        return hint;
    if (JSValue object = CApi::objectOfSymbol(globalObject, bits))
        return object;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = JSValue::decode(static_cast<EncodedJSValue>(bits));
    bool isValid;
    JSCell* cellOfAnotherKind = nullptr;
    if (!value)
        isValid = false;
    else if (!value.isCell())
        isValid = value.isNumber() || value.isBoolean() || value.isUndefinedOrNull();
    else {
        // Anything may be in the memory. It is taken for an object only if the heap has a live cell there, of a kind that
        // programs get to see. What C owns a reference to is one, and that is so of every result of a function of C's that keeps the rules: the heap is not asked.
        isValid = CApi::isSetUpFor(runtime(globalObject)) && CApi::isAddressOfCellThatCOwns(value.asCell());
        if (!isValid) {
            // (A block that cells are being allocated from cannot tell which of its cells are in use: allocation is suspended.)
            vm.heap.objectSpace().enablePreciseAllocationTracking();
            HeapIterationScope iterationScope(vm.heap);
            isValid = HeapUtil::isPointerGCObjectJSCell(vm.heap, vm.heap.objectSpace().blocks().filter(), value.asCell());
        }
        if (isValid) {
            JSCell* cell = value.asCell();
            if (auto* view = PyMemoryView::tryFrom(cell); view && view->isInternal)
                isValid = false;
            else if (isPyObject(cell))
                isValid = cell->type() != PyKwNamesType && !uncheckedDowncast<PyObject>(cell)->type()->has(TypeFlag::Internal);
            else if (auto* type = PyType::tryFrom(cell))
                isValid = !type->has(TypeFlag::Internal);
            else
                isValid = cell->isString() || cell->isHeapBigInt() || (cell->isObject() && !cell->inherits<PyCTable>() && cell != stateOf(globalObject).records.get());
            if (!isValid)
                cellOfAnotherKind = cell;
        } else {
            // The id() of a str is where its characters are (see isIdentical()), which is no cell and cannot be looked at. A str that has them there is looked for.
            HeapIterationScope iterationScope(vm.heap);
            vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* heapCell, HeapCell::Kind kind) {
                if (!isJSCellKind(kind))
                    return IterationStatus::Continue;
                auto* cell = static_cast<JSCell*>(heapCell);
                if (!cell->structureID() || !cell->isString() || std::bit_cast<uintptr_t>(asString(cell)->tryGetValueImpl()) != bits)
                    return IterationStatus::Continue;
                value = cell;
                isValid = true;
                return IterationStatus::Done;
            });
        }
    }
    // The address of an instance of the type of an extension is not that of the object.
    if (cellOfAnotherKind) {
        value = CApi::objectOfBody(runtime(globalObject), cellOfAnotherKind);
        isValid = !!value;
    }
    if (!isValid)
        return raiseValueError(globalObject, scope, "the memory does not hold a reference to an object"_s);
    return value;
}

// ---- Values into bytes ----

template<typename T>
static ALWAYS_INLINE void packAs(Packed& result, T value)
{
    static_assert(sizeof(T) <= sizeof(result.bytes));
    memcpy(result.bytes, &value, sizeof(T));
}

static bool packCharacter(JSGlobalObject* globalObject, JSValue value, Packed& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr auto expected = "one character bytes, bytearray, or an integer in range(256) expected, not "_s;
    if (auto* bytes = PyBytes::tryFrom(value)) {
        if (bytes->length() != 1) {
            raiseTypeError(globalObject, scope, expected, "bytes of length "_s, bytes->length());
            return false;
        }
        result.bytes[0] = bytes->data()[0];
        return true;
    }
    if (auto* array = PyByteArray::tryFrom(value)) {
        if (array->length() != 1) {
            raiseTypeError(globalObject, scope, expected, "bytearray of length "_s, array->length());
            return false;
        }
        result.bytes[0] = array->span()[0];
        return true;
    }
    if (isInt(value)) {
        auto number = tryInt64(intValue(value));
        if (!number || *number < 0 || *number >= 256) {
            raiseTypeError(globalObject, scope, "integer not in range(256)"_s);
            return false;
        }
        result.bytes[0] = static_cast<uint8_t>(*number);
        return true;
    }
    raiseTypeError(globalObject, scope, expected, typeNameForMessage(globalObject, typeOf(globalObject, value)));
    return false;
}

// How many bytes are in the other order at a time, and how many such parts there are.
static ALWAYS_INLINE std::pair<size_t, size_t> partsOf(Code code)
{
    switch (code) {
    case Code::Float:
        return { sizeof(float), 1 };
    case Code::Double:
        return { sizeof(double), 1 };
    case Code::LongDouble:
        return { sizeof(long double), 1 };
    case Code::ComplexFloat:
        return { sizeof(float), 2 };
    case Code::ComplexDouble:
        return { sizeof(double), 2 };
    case Code::ComplexLongDouble:
        return { sizeof(long double), 2 };
    case Code::Bool:
    case Code::Char:
        return { 1, 1 };
    case Code::VariantBool:
        return { sizeof(short), 1 };
    case Code::WChar:
        return { sizeof(wchar_t), 1 };
    default:
        return { sizeof(void*), 1 };
    }
}

// Whether so many bytes are what load() and store() touch for that code. They are, whenever accessor and size are those of one info.
static ALWAYS_INLINE bool hasRoomFor(Code code, size_t size)
{
    switch (code) {
    case Code::None:
        return false;
    case Code::SignedInt:
    case Code::UnsignedInt:
        return size >= 1 && size <= 8;
    case Code::CharArray:
    case Code::WCharArray:
        return true;
    default: {
        auto [partSize, count] = partsOf(code);
        return size >= partSize * count;
    }
    }
}

static NEVER_INLINE void raiseNoRoom(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
}

bool pack(JSGlobalObject* globalObject, Accessor accessor, JSValue value, int64_t size, Packed& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    memset(result.bytes, 0, sizeof(result.bytes));
    result.keep = none();
    result.owner = nullptr;
    result.text = { };
    if (size < 0 || !hasRoomFor(accessor.code, static_cast<size_t>(size))) [[unlikely]] {
        scope.release();
        raiseNoRoom(globalObject);
        return false;
    }

    auto packComplex = [&]<typename T>(T) {
        double real;
        double imaginary;
        bool ok = complexArgument(globalObject, value, real, imaginary);
        RETURN_IF_EXCEPTION(scope, false);
        ASSERT_UNUSED(ok, ok);
        T converted;
        converted.real = static_cast<decltype(converted.real)>(real);
        converted.imaginary = static_cast<decltype(converted.imaginary)>(imaginary);
        // Only this one minds.
        if (accessor.isSwapped && std::is_same_v<T, ComplexFloat> && ((std::isinf(converted.real) && !std::isinf(real)) || (std::isinf(converted.imaginary) && !std::isinf(imaginary)))) {
            raiseOverflowError(globalObject, scope, "float too large to pack with f format"_s);
            return false;
        }
        packAs(result, converted);
        return true;
    };

    switch (accessor.code) {
    case Code::None:
        RELEASE_ASSERT_NOT_REACHED();
    case Code::SignedInt:
    case Code::UnsignedInt: {
        JSValue index = isInt(value) ? value : toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        packAs(result, lowBitsOfInt(intValue(index)));
        return true;
    }
    case Code::Float: {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        packAs(result, static_cast<float>(number));
        return true;
    }
    case Code::Double: {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        packAs(result, number);
        return true;
    }
    case Code::LongDouble: {
        double number = toDouble(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        packAs(result, static_cast<long double>(number));
        return true;
    }
    case Code::ComplexFloat:
        RELEASE_AND_RETURN(scope, packComplex(ComplexFloat { }));
    case Code::ComplexDouble:
        RELEASE_AND_RETURN(scope, packComplex(ComplexDouble { }));
    case Code::ComplexLongDouble:
        RELEASE_AND_RETURN(scope, packComplex(ComplexLongDouble { }));
    case Code::Bool: {
        bool truth = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        result.bytes[0] = truth;
        return true;
    }
    case Code::VariantBool: {
        bool truth = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        packAs(result, static_cast<short>(truth ? -1 : 0));
        return true;
    }
    case Code::Char:
        RELEASE_AND_RETURN(scope, packCharacter(globalObject, value, result));
    case Code::WChar: {
        if (!isStr(value)) {
            raiseTypeError(globalObject, scope, "a unicode character expected, not instance of "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
            return false;
        }
        StrView view(globalObject, strValue(value));
        RETURN_IF_EXCEPTION(scope, false);
        unsigned length = sizeof(wchar_t) == 2 ? view.unitLength() : view.length();
        if (length != 1) {
            raiseTypeError(globalObject, scope, "a unicode character expected, not a string of length "_s, length);
            return false;
        }
        packAs(result, static_cast<wchar_t>(sizeof(wchar_t) == 2 ? view.view()[0] : view.at(0)));
        return true;
    }
    case Code::CharPointer:
        if (isNone(value))
            return true;
        if (auto* bytes = PyBytes::tryFrom(value)) {
            packAs(result, bytes->data());
            result.keep = value;
            result.owner = bytes;
            return true;
        }
        if (isInt(value)) {
            packAs(result, static_cast<uintptr_t>(lowBitsOfInt(intValue(value))));
            return true;
        }
        raiseTypeError(globalObject, scope, "bytes or integer address expected instead of "_s, typeName(globalObject, value), " instance"_s);
        return false;
    case Code::WCharPointer: {
        if (isNone(value))
            return true;
        if (isInt(value)) {
            packAs(result, static_cast<uintptr_t>(lowBitsOfInt(intValue(value))));
            return true;
        }
        if (!isStr(value)) {
            raiseTypeError(globalObject, scope, "unicode string or integer address expected instead of "_s, typeName(globalObject, value), " instance"_s);
            return false;
        }
        size_t length;
        PyByteStorage* storage = wideCharactersOf(globalObject, strValue(value), length);
        RETURN_IF_EXCEPTION(scope, false);
        packAs(result, storage->data());
        result.keep = capsuleForMemory(globalObject, storage);
        result.owner = storage;
        return true;
    }
    case Code::VoidPointer:
        if (isNone(value))
            return true;
        if (!isInt(value)) {
            raiseTypeError(globalObject, scope, "cannot be converted to pointer"_s);
            return false;
        }
        packAs(result, static_cast<uintptr_t>(lowBitsOfInt(intValue(value))));
        return true;
    case Code::Object:
        packAs(result, identityBits(value));
        result.keep = value;
        return true;
    case Code::CharArray: {
        auto* bytes = PyBytes::tryFrom(value);
        if (!bytes) {
            raiseTypeError(globalObject, scope, "expected bytes, "_s, typeName(globalObject, value), " found"_s);
            return false;
        }
        // Up to the first NUL, and with it if there is room.
        int64_t length = static_cast<int64_t>(strlen(reinterpret_cast<const char*>(bytes->data())));
        if (length > size) {
            raiseValueError(globalObject, scope, "bytes too long ("_s, length, ", maximum length "_s, size, ')');
            return false;
        }
        result.text = { bytes->data(), static_cast<size_t>(length < size ? length + 1 : length) };
        result.holder = bytes;
        return true;
    }
    case Code::WCharArray: {
        if (!isStr(value)) {
            raiseTypeError(globalObject, scope, "unicode string expected instead of "_s, typeName(globalObject, value), " instance"_s);
            return false;
        }
        size_t length;
        PyByteStorage* storage = wideCharactersOf(globalObject, strValue(value), length);
        RETURN_IF_EXCEPTION(scope, false);
        size_t room = static_cast<size_t>(size) / sizeof(wchar_t);
        if (length > room) {
            raiseValueError(globalObject, scope, "string too long ("_s, length, ", maximum length "_s, room, ')');
            return false;
        }
        result.text = { storage->data(), (length < room ? length + 1 : length) * sizeof(wchar_t) };
        result.holder = storage;
        result.keep = value; // For no reason but that CPython does.
        return true;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE void copyReversed(uint8_t* destination, const uint8_t* source, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        destination[i] = source[size - 1 - i];
}

static ALWAYS_INLINE uint64_t loadInteger(std::span<const uint8_t> source, bool isSwapped)
{
    uint64_t bits = 0;
    ASSERT(source.size() >= 1 && source.size() <= 8);
    if (isSwapped)
        copyReversed(reinterpret_cast<uint8_t*>(&bits), source.data(), source.size());
    else
        memcpy(&bits, source.data(), source.size());
    return bits;
}

static ALWAYS_INLINE void storeInteger(std::span<uint8_t> destination, bool isSwapped, uint64_t bits)
{
    if (isSwapped)
        copyReversed(destination.data(), reinterpret_cast<const uint8_t*>(&bits), destination.size());
    else
        memcpy(destination.data(), &bits, destination.size());
}

static_assert(std::endian::native == std::endian::little);

void store(std::span<uint8_t> destination, Accessor accessor, const Packed& value, Bits bits)
{
    // pack() has looked, for the same size.
    RELEASE_ASSERT(hasRoomFor(accessor.code, destination.size()) && value.text.size() <= destination.size());
    switch (accessor.code) {
    case Code::None:
        RELEASE_ASSERT_NOT_REACHED();
    case Code::SignedInt:
    case Code::UnsignedInt: {
        uint64_t number;
        memcpy(&number, value.bytes, sizeof(number));
        if (bits.size) {
            uint64_t mask = bits.size >= 64 ? ~0ull : (1ull << bits.size) - 1;
            uint64_t old = loadInteger(destination, accessor.isSwapped);
            number = (old & ~(mask << bits.offset)) | ((number & mask) << bits.offset);
        }
        storeInteger(destination, accessor.isSwapped, number);
        return;
    }
    case Code::CharArray:
    case Code::WCharArray:
        memcpy(destination.data(), value.text.data(), value.text.size());
        ensureStillAliveHere(value.holder);
        return;
    default: {
        auto [partSize, count] = partsOf(accessor.code);
        if (!accessor.isSwapped) {
            memcpy(destination.data(), value.bytes, partSize * count);
            return;
        }
        for (size_t i = 0; i < count; ++i)
            copyReversed(destination.data() + i * partSize, value.bytes + i * partSize, partSize);
        return;
    }
    }
}

// ---- Bytes into values ----

JSValue load(JSGlobalObject* globalObject, Accessor accessor, std::span<const uint8_t> bytesOfSource, Bits bits, JSValue hint)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasRoomFor(accessor.code, bytesOfSource.size())) [[unlikely]] {
        scope.release();
        raiseNoRoom(globalObject);
        return { };
    }
    const uint8_t* source = bytesOfSource.data();
    size_t size = bytesOfSource.size();

    alignas(16) uint8_t bytes[32];
    auto read = [&]<typename T>(T) {
        T result;
        auto [partSize, count] = partsOf(accessor.code);
        ASSERT(partSize * count == sizeof(T));
        if (!accessor.isSwapped)
            memcpy(bytes, source, sizeof(T));
        else {
            for (size_t i = 0; i < count; ++i)
                copyReversed(bytes + i * partSize, source + i * partSize, partSize);
        }
        memcpy(&result, bytes, sizeof(T));
        return result;
    };

    switch (accessor.code) {
    case Code::None:
        RELEASE_ASSERT_NOT_REACHED();
    case Code::SignedInt: {
        uint64_t number = loadInteger(bytesOfSource, accessor.isSwapped);
        unsigned width = bits.size ? bits.size : static_cast<unsigned>(size * 8);
        unsigned low = bits.size ? bits.offset : 0;
        int64_t result = static_cast<int64_t>(number << (64 - low - width)) >> (64 - width);
        return makeInt(globalObject, result);
    }
    case Code::UnsignedInt: {
        uint64_t number = loadInteger(bytesOfSource, accessor.isSwapped);
        if (bits.size)
            number = (number << (64 - bits.offset - bits.size)) >> (64 - bits.size);
        return makeInt(globalObject, number);
    }
    case Code::Float:
        return makeFloat(globalObject, read(float { }));
    case Code::Double:
        return makeFloat(globalObject, read(double { }));
    case Code::LongDouble:
        return makeFloat(globalObject, static_cast<double>(read(static_cast<long double>(0))));
    case Code::ComplexFloat: {
        auto value = read(ComplexFloat { });
        return PyComplex::create(globalObject, value.real, value.imaginary);
    }
    case Code::ComplexDouble: {
        auto value = read(ComplexDouble { });
        return PyComplex::create(globalObject, value.real, value.imaginary);
    }
    case Code::ComplexLongDouble: {
        auto value = read(ComplexLongDouble { });
        return PyComplex::create(globalObject, static_cast<double>(value.real), static_cast<double>(value.imaginary));
    }
    case Code::Bool:
        return jsBoolean(!!*source);
    case Code::VariantBool:
        return jsBoolean(!!read(short { }));
    case Code::Char:
        return PyBytes::create(globalObject, std::span<const uint8_t> { source, 1 });
    case Code::WChar: {
        wchar_t character = read(wchar_t { });
        RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, &character, 1));
    }
    case Code::CharPointer: {
        auto* text = read(static_cast<const char*>(nullptr));
        if (!text)
            return none();
        size_t length = strlen(text);
        PyBytes* result = newBytes(globalObject, length);
        RETURN_IF_EXCEPTION(scope, { });
        memcpy(result->mutableData(), text, length);
        return result;
    }
    case Code::WCharPointer: {
        auto* text = read(static_cast<const wchar_t*>(nullptr));
        if (!text)
            return none();
        RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, text, wcslen(text)));
    }
    case Code::VoidPointer: {
        auto* pointer = read(static_cast<const void*>(nullptr));
        if (!pointer)
            return none();
        return intFromPointer(globalObject, pointer);
    }
    case Code::Object: {
        uint64_t objectBits = read(uint64_t { });
        if (!objectBits)
            return raiseValueError(globalObject, scope, "PyObject is NULL"_s);
        RELEASE_AND_RETURN(scope, objectFromBits(globalObject, objectBits, hint));
    }
    case Code::CharArray: {
        size_t length = strnlen(reinterpret_cast<const char*>(source), size);
        // The memory may be that of a bytearray or the like, and making the bytes may collect.
        PyBytes* result = newBytes(globalObject, length);
        RETURN_IF_EXCEPTION(scope, { });
        memcpy(result->mutableData(), source, length);
        return result;
    }
    case Code::WCharArray: {
        auto* text = reinterpret_cast<const wchar_t*>(source);
        size_t room = size / sizeof(wchar_t);
        size_t length = 0;
        while (length < room && text[length])
            ++length;
        RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, text, length));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- CField ----

static JSValue fieldNew(JSGlobalObject* globalObject, JSValue typeValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyType* type = PyType::from(typeValue);

    static constexpr ASCIILiteral names[] = { "name"_s, "type"_s, "byte_size"_s, "byte_offset"_s, "index"_s, "_internal_use"_s, "bit_size"_s, "bit_offset"_s };
    JSValue values[std::size(names)];
    if (args.size())
        return raiseTypeError(globalObject, scope, "CField() takes no positional arguments"_s);
    bool ok = parseKeywordArguments(globalObject, "CField"_s, args, names, values);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    for (unsigned i = 0; i < 6; ++i) {
        if (!values[i])
            return raiseTypeError(globalObject, scope, "CField() missing required argument '"_s, names[i], "' (pos "_s, i + 1, ')');
    }
    JSValue name = values[0];
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "CField() argument 'name' must be str, not "_s, typeNameOfArgument(globalObject, name));
    JSValue proto = values[1];
    int64_t byteSize;
    int64_t byteOffset;
    int64_t index;
    if (failed(scope, ssizeArgument(globalObject, values[2], byteSize)) || failed(scope, ssizeArgument(globalObject, values[3], byteOffset)) || failed(scope, ssizeArgument(globalObject, values[4], index)))
        return { };
    bool isInternalUse = isTrue(globalObject, values[5]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue bitSizeValue = values[6] ? values[6] : none();
    JSValue bitOffsetValue = values[7] ? values[7] : none();

    if (!isInternalUse)
        return raiseTypeError(globalObject, scope, "cannot create "_s, typeNameForMessage(globalObject, type->type()), " object"_s);
    String nameRepr = reprForMessage(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (byteSize < 0)
        return raiseValueError(globalObject, scope, "byte size of field "_s, nameRepr, " must not be negative, got "_s, byteSize);
    PyCTypeInfo* info = infoOf(globalObject, proto);
    if (!info)
        return raiseTypeError(globalObject, scope, "type of field "_s, nameRepr, " must be a C type"_s);
    if (byteSize != info->size)
        return raiseValueError(globalObject, scope, "byte size of field "_s, nameRepr, " ("_s, byteSize, ") does not match type size ("_s, info->size, ')');

    int64_t bitSize = 0;
    int64_t bitOffset = 0;
    if (!isNone(bitSizeValue)) {
        bool isAllowed = info->accessor.code == Code::SignedInt || info->accessor.code == Code::UnsignedInt || info->accessor.code == Code::Bool;
        if (!isAllowed)
            return raiseTypeError(globalObject, scope, "bit fields not allowed for type "_s, PyType::from(proto)->nameInMessages());
        if (byteSize > 100)
            return raiseValueError(globalObject, scope, "bit field "_s, nameRepr, " size too large, got "_s, byteSize);
        if (!isInt(bitSizeValue) || !isInt(bitOffsetValue))
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        bitSize = toSsize(globalObject, bitSizeValue, runtime(globalObject).OverflowError());
        RETURN_IF_EXCEPTION(scope, { });
        if (bitSize <= 0 || bitSize > 255)
            return raiseValueError(globalObject, scope, "bit size of field "_s, nameRepr, " out of range, got "_s, bitSize);
        bitOffset = toSsize(globalObject, bitOffsetValue, runtime(globalObject).OverflowError());
        RETURN_IF_EXCEPTION(scope, { });
        if (bitOffset < 0 || bitOffset > 255)
            return raiseValueError(globalObject, scope, "bit offset of field "_s, nameRepr, " out of range, got "_s, bitOffset);
        if (bitSize + bitOffset > byteSize * 8)
            return raiseValueError(globalObject, scope, "bit field "_s, nameRepr, " overflows its type ("_s, bitOffset, " + "_s, bitSize, " > "_s, byteSize * 8, ')');
    } else if (!isNone(bitOffsetValue))
        return raiseValueError(globalObject, scope, "field "_s, nameRepr, ": bit_offset must be specified if bit_size is"_s);

    PyCField* field = PyCField::create(vm, state.fieldStructure.get(), type);
    field->name.set(vm, field, strValue(name));
    field->proto.set(vm, field, PyType::from(proto));
    field->byteSize = byteSize;
    field->byteOffset = byteOffset;
    field->bits = { static_cast<uint8_t>(bitSize), static_cast<uint8_t>(bitOffset) };
    field->index = index;
    // A field that is an array of characters reads as bytes or str.
    if (info->kind == TypeKind::Array) {
        if (PyCTypeInfo* itemInfo = infoOf(globalObject, info->proto.get())) {
            if (itemInfo->accessor == Accessor { Code::Char })
                field->accessor = { Code::CharArray };
            else if (itemInfo->accessor == Accessor { Code::WChar })
                field->accessor = { Code::WCharArray };
        }
    }
    return field;
}

static JSValue fieldGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCField* field = PyCField::from(self);
    if (!instance || isNone(instance))
        return self;
    PyCData* source = PyCData::tryFrom(instance);
    if (!source)
        return raiseTypeError(globalObject, scope, "not a ctype instance"_s);
    RELEASE_AND_RETURN(scope, getData(globalObject, field->proto.get(), field->accessor, { source, field->byteOffset }, field->index, field->byteSize, field->bits));
}

static JSValue fieldSet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCField* field = PyCField::from(self);
    PyCData* destination = PyCData::tryFrom(instance);
    if (!destination)
        return raiseTypeError(globalObject, scope, "not a ctype instance"_s);
    setData(globalObject, field->proto.get(), field->accessor, { destination, field->byteOffset }, value, field->index, field->byteSize, field->bits);
    RETURN_IF_EXCEPTION(scope, { });
    return none();
}

static JSValue fieldDelete(JSGlobalObject* globalObject, JSValue, JSValue instance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyCData::is(instance))
        return raiseTypeError(globalObject, scope, "not a ctype instance"_s);
    return raiseTypeError(globalObject, scope, "can't delete attribute"_s);
}

static JSValue fieldRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCField* field = PyCField::from(self);
    String name = reprForMessage(globalObject, field->name.get());
    RETURN_IF_EXCEPTION(scope, { });
    String start = makeString('<', typeNameForMessage(globalObject, field->type()), ' ', name, " type="_s, field->proto->nameInMessages(), ", ofs="_s, field->byteOffset);
    if (field->bits.size)
        return makeStr(globalObject, makeString(start, ", bit_size="_s, field->bits.size, ", bit_offset="_s, field->bits.offset, '>'));
    return makeStr(globalObject, makeString(start, ", size="_s, field->byteSize, '>'));
}

static JSValue fieldName(JSGlobalObject*, JSValue self) { return PyCField::from(self)->name.get(); }
static JSValue fieldType(JSGlobalObject*, JSValue self) { return PyCField::from(self)->proto.get(); }
static JSValue fieldByteOffset(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, PyCField::from(self)->byteOffset); }
static JSValue fieldByteSize(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, PyCField::from(self)->byteSize); }
static JSValue fieldBitOffset(JSGlobalObject*, JSValue self) { return jsNumber(PyCField::from(self)->bits.offset); }
static JSValue fieldIsBitField(JSGlobalObject*, JSValue self) { return jsBoolean(!!PyCField::from(self)->bits.size); }
static JSValue fieldIsAnonymous(JSGlobalObject*, JSValue self) { return jsBoolean(PyCField::from(self)->isAnonymous); }

static JSValue fieldLegacySize(JSGlobalObject* globalObject, JSValue self)
{
    PyCField* field = PyCField::from(self);
    if (field->bits.size)
        return jsNumber((field->bits.size << 16) | field->bits.offset);
    return makeInt(globalObject, field->byteSize);
}

static JSValue fieldBitSize(JSGlobalObject* globalObject, JSValue self)
{
    PyCField* field = PyCField::from(self);
    if (field->bits.size)
        return jsNumber(field->bits.size);
    return intMultiply(globalObject, makeInt(globalObject, field->byteSize), jsNumber(8));
}

static constexpr NativeDef fieldMethods[] = {
    PY_NEW(fieldNew, "!($type, *, name, type, byte_size, byte_offset, index, _internal_use, bit_size=None, bit_offset=None)"),
    PY_SLOT("__repr__", fieldRepr, "($self, /)"),
    PY_SLOT("__get__", fieldGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__set__", fieldSet, "($self, instance, value, /)"),
    PY_SLOT("__delete__", fieldDelete, "($self, instance, /)"),
};

static constexpr NativeGetSet fieldGetSets[] = {
    PY_MEMBER("name", fieldName),
    PY_MEMBER("type", fieldType),
    PY_MEMBER("offset", fieldByteOffset),
    PY_MEMBER("byte_offset", fieldByteOffset),
    PY_MEMBER("byte_size", fieldByteSize),
    PY_MEMBER("bit_offset", fieldBitOffset),
    { "size"_s, fieldLegacySize },
    { "bit_size"_s, fieldBitSize },
    { "is_bitfield"_s, fieldIsBitField },
    { "is_anonymous"_s, fieldIsAnonymous },
};

const TypeDef fieldTypeDef { "ctypes.CField"_s, { TypeFlag::OwnLayout, TypeFlag::Immutable }, fieldMethods, fieldGetSets, "Structure/Union member"_s };

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
