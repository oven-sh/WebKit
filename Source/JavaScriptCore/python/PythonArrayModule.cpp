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

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"
#include "PythonText.h"

// The module array: Modules/arraymodule.c of CPython.

namespace JSC { namespace Python {

namespace {

static_assert(std::endian::native == std::endian::little);

struct ArrayState;

// What there is to know about a kind of item
struct ArrayDescriptor {
    char typecode;
    unsigned itemSize;
    JSValue (*getItem)(JSGlobalObject*, const uint8_t*); // Empty if it raised.
    // If the index is -1, whether the value will do is all that is found out. False if it raised.
    bool (*setItem)(JSGlobalObject*, ArrayState&, int64_t index, JSValue);
    int (*compareItems)(const uint8_t*, const uint8_t*, size_t count); // Null if they are not compared as they lie.
    bool isIntegerType;
    bool isSigned;
};

struct ArrayModuleState final : NativeState {
    PYTHON_NATIVE_STATE(ArrayModuleState);
    WriteBarrier<PyType> array;
    WriteBarrier<PyType> iterator;
    WriteBarrier<Unknown> reconstructor;
};

template<typename Visitor>
void ArrayModuleState::visit(Visitor& visitor)
{
    visitor.append(array);
    visitor.append(iterator);
    visitor.append(reconstructor);
}

ArrayModuleState& moduleStateOf(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<ArrayModuleState>(); }

struct ArrayState final : NativeState {
    PYTHON_NATIVE_STATE(ArrayState);
    ArrayState(const ArrayDescriptor& descriptor)
        : descriptor(descriptor)
    {
    }

    const ArrayDescriptor& descriptor;
    // The items, one after another, and nothing else. No program ever has hold of it.
    WriteBarrier<JSUint8Array> storage;
    // How many CPython would have room for, which shows in __sizeof__(). None at all is when it has nowhere to keep any, which shows in buffer_info().
    int64_t allocated { 0 };

    // Where they are now. It is not to be kept while anything is run that could be a program's.
    std::span<uint8_t> bytes() const { return storage->typedSpan(); }
    int64_t size() const { return static_cast<int64_t>(storage->length() / descriptor.itemSize); }
    uint8_t* at(int64_t index) const { return bytes().subspan(static_cast<size_t>(index) * descriptor.itemSize, descriptor.itemSize).data(); }

    std::optional<ExportedBytes> exportedBytes() const final
    {
        // A wchar_t is as wide as any character where it is four bytes, and then CPython says so.
        char format = descriptor.typecode == 'u' && sizeof(wchar_t) == 4 ? 'w' : descriptor.typecode;
        return ExportedBytes { storage.get(), format, descriptor.itemSize };
    }
};

template<typename Visitor> void ArrayState::visit(Visitor& visitor) { visitor.append(storage); }

constexpr int64_t most = std::numeric_limits<int64_t>::max(); // PY_SSIZE_T_MAX

bool isPythonArray(JSGlobalObject* globalObject, JSValue value) { return tryStateOf<ArrayState>(value) && isInstance(globalObject, value, moduleStateOf(globalObject).array.get()); }

// array_resize(). False if it raised.
bool resize(JSGlobalObject* globalObject, ArrayState& self, int64_t newSize)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t size = self.size();
    int64_t allocated;
    if (self.allocated >= newSize && size < newSize + 16 && self.allocated)
        allocated = self.allocated; // There is room, and not so much as to be worth giving back.
    else if (!newSize)
        allocated = 0;
    else {
        // Somewhat more than is asked for, so that adding one at a time does not take long: 0, 4, 8, 16, 25, 34, 46, 56, 67, 79, ...
        allocated = (newSize >> 4) + (size < 8 ? 3 : 7) + newSize;
        if (static_cast<uint64_t>(allocated) > std::numeric_limits<uint64_t>::max() / self.descriptor.itemSize) {
            raiseMemoryError(globalObject, scope);
            return false;
        }
    }
    if (static_cast<uint64_t>(newSize) > static_cast<uint64_t>(most) / self.descriptor.itemSize) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    resizeByteArray(globalObject, self.storage.get(), static_cast<size_t>(newSize) * self.descriptor.itemSize);
    RETURN_IF_EXCEPTION(scope, false);
    self.allocated = allocated;
    return true;
}

// newarrayobject(). What is in it is zeros. Null if it raised.
PyStateObject* newArray(JSGlobalObject* globalObject, PyType* type, int64_t size, const ArrayDescriptor& descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (size > most / static_cast<int64_t>(descriptor.itemSize)) {
        raiseMemoryError(globalObject, scope);
        return nullptr;
    }
    JSUint8Array* storage = newByteArray(globalObject, std::span<const uint8_t>());
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (size) {
        resizeByteArray(globalObject, storage, static_cast<size_t>(size) * descriptor.itemSize);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<ArrayState>(descriptor));
    auto& state = object->state<ArrayState>();
    state.storage.set(vm, object, storage);
    state.allocated = size;
    return object;
}

PyStateObject* newArray(JSGlobalObject* globalObject, int64_t size, const ArrayDescriptor& descriptor) { return newArray(globalObject, moduleStateOf(globalObject).array.get(), size, descriptor); }

// ---- What is done for each kind of item

template<typename T>
T loadItem(const uint8_t* p)
{
    T value;
    memcpy(&value, p, sizeof(T));
    return value;
}

// CHECK_ARRAY_BOUNDS(): making a number of what was given can run anything, and there may be less in the array than there was. False if it raised.
bool checkBounds(JSGlobalObject* globalObject, ArrayState& self, int64_t index)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (index >= 0 && index >= self.size()) {
        raise(globalObject, scope, BuiltinType::IndexError, "array assignment index out of range"_s);
        return false;
    }
    return true;
}

template<typename T>
bool storeItem(ArrayState& self, int64_t index, T value)
{
    if (index >= 0)
        memcpy(self.at(index), &value, sizeof(T));
    return true;
}

bool raiseOverflow(JSGlobalObject* globalObject, ASCIILiteral message)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raise(globalObject, scope, BuiltinType::OverflowError, message);
    return false;
}

template<typename T>
JSValue getInteger(JSGlobalObject* globalObject, const uint8_t* p)
{
    if constexpr (std::is_signed_v<T>)
        return intFromInt64(globalObject, loadItem<T>(p));
    else
        return intFromUInt64(globalObject, loadItem<T>(p));
}

// The "h" of PyArg_Parse(). Nothing if it raised.
std::optional<short> toCShort(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto number = toCLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*number < std::numeric_limits<short>::min()) {
        raiseOverflow(globalObject, "signed short integer is less than minimum"_s);
        return std::nullopt;
    }
    if (*number > std::numeric_limits<short>::max()) {
        raiseOverflow(globalObject, "signed short integer is greater than maximum"_s);
        return std::nullopt;
    }
    return static_cast<short>(*number);
}

// b_setitem()
bool setSignedChar(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCShort(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    if (*x < -128)
        return raiseOverflow(globalObject, "signed char is less than minimum"_s);
    if (*x > 127)
        return raiseOverflow(globalObject, "signed char is greater than maximum"_s);
    return storeItem(self, index, static_cast<int8_t>(*x));
}

// BB_setitem(), with the "b" of PyArg_Parse()
bool setUnsignedChar(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (*x < 0)
        return raiseOverflow(globalObject, "unsigned byte integer is less than minimum"_s);
    if (*x > 255)
        return raiseOverflow(globalObject, "unsigned byte integer is greater than maximum"_s);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<uint8_t>(*x));
}

// h_setitem()
bool setShort(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCShort(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, *x);
}

// HH_setitem()
bool setUnsignedShort(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCIntOfFormat(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (*x < 0)
        return raiseOverflow(globalObject, "unsigned short is less than minimum"_s);
    if (*x > std::numeric_limits<unsigned short>::max())
        return raiseOverflow(globalObject, "unsigned short is greater than maximum"_s);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<unsigned short>(*x));
}

// i_setitem()
bool setInt(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCIntOfFormat(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, *x);
}

// PyLong_AsUnsignedLong() and PyLong_AsUnsignedLongLong(), of an int or of what has __index__(). Nothing if it raised.
std::optional<uint64_t> toUnsigned(JSGlobalObject* globalObject, JSValue value, ASCIILiteral tooLarge, ASCIILiteral negative = "can't convert negative value to unsigned int"_s)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (compareInts(integer, jsNumber(0)) < 0) {
        raiseOverflow(globalObject, negative);
        return std::nullopt;
    }
    uint64_t bits = lowBitsOfInt(integer);
    if (compareInts(integer, intFromUInt64(globalObject, bits))) {
        raiseOverflow(globalObject, tooLarge);
        return std::nullopt;
    }
    return bits;
}

// II_setitem()
bool setUnsignedInt(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toUnsigned(globalObject, value, "Python int too large to convert to C unsigned long"_s);
    RETURN_IF_EXCEPTION(scope, false);
    if (*x > std::numeric_limits<unsigned>::max())
        return raiseOverflow(globalObject, "unsigned int is greater than maximum"_s);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<unsigned>(*x));
}

// l_setitem()
bool setLong(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<long>(*x));
}

// LL_setitem()
bool setUnsignedLong(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toUnsigned(globalObject, value, "Python int too large to convert to C unsigned long"_s);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<unsigned long>(*x));
}

// q_setitem()
bool setLongLong(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toCLongLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, *x);
}

// QQ_setitem()
bool setUnsignedLongLong(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toUnsigned(globalObject, value, "int too big to convert"_s, "can't convert negative int to unsigned"_s);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<unsigned long long>(*x));
}

JSValue getFloat(JSGlobalObject*, const uint8_t* p) { return floatFromDouble(static_cast<double>(loadItem<float>(p))); }
JSValue getDouble(JSGlobalObject*, const uint8_t* p) { return floatFromDouble(loadItem<double>(p)); }

// f_setitem() and d_setitem()
template<typename T>
bool setFloating(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toDouble(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!checkBounds(globalObject, self, index))
        return false;
    return storeItem(self, index, static_cast<T>(*x));
}

JSValue strOfCharacter(JSGlobalObject* globalObject, char32_t character)
{
    TextBuilder builder;
    builder.append(character);
    return jsString(globalObject->vm(), builder.toString());
}

// u_getitem() and w_getitem(), with PyUnicode_FromOrdinal()
template<typename T>
JSValue getCharacter(JSGlobalObject* globalObject, const uint8_t* p)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto ordinal = static_cast<int64_t>(loadItem<T>(p));
    if (ordinal < 0 || ordinal > 0x10ffff)
        return raiseValueError(globalObject, scope, "chr() arg not in range(0x110000)"_s);
    return strOfCharacter(globalObject, static_cast<char32_t>(ordinal));
}

// u_setitem() and w_setitem()
template<typename T>
bool setCharacter(JSGlobalObject* globalObject, ArrayState& self, int64_t index, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSString* string = stringIn(value);
    if (!string) {
        raiseTypeError(globalObject, scope, concatenate("array item must be a unicode character, not "_s, typeName(globalObject, value)));
        return false;
    }
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    size_t length = 0;
    char32_t character = 0;
    for (char32_t c : view->codePoints()) {
        if (!length++)
            character = c;
    }
    if (length != 1) {
        raiseTypeError(globalObject, scope, concatenate("array item must be a unicode character, not a string of length "_s, length));
        return false;
    }
    if (character > static_cast<char32_t>(std::numeric_limits<T>::max())) {
        // Where a wchar_t is two bytes
        String shown = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        raiseTypeError(globalObject, scope, concatenate("string "_s, escapeNonASCII(shown), " cannot be converted to a single wchar_t character"_s));
        return false;
    }
    return storeItem(self, index, static_cast<T>(character));
}

template<typename T>
int compareItems(const uint8_t* left, const uint8_t* right, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        T a = loadItem<T>(left + i * sizeof(T));
        T b = loadItem<T>(right + i * sizeof(T));
        if (a != b)
            return a < b ? -1 : 1;
    }
    return 0;
}

constexpr ArrayDescriptor descriptors[] = {
    { 'b', 1, getInteger<int8_t>, setSignedChar, compareItems<int8_t>, true, true },
    { 'B', 1, getInteger<uint8_t>, setUnsignedChar, compareItems<uint8_t>, true, false },
    { 'u', sizeof(wchar_t), getCharacter<wchar_t>, setCharacter<wchar_t>, compareItems<wchar_t>, false, false },
    { 'w', sizeof(char32_t), getCharacter<char32_t>, setCharacter<char32_t>, compareItems<char32_t>, false, false },
    { 'h', sizeof(short), getInteger<short>, setShort, compareItems<short>, true, true },
    { 'H', sizeof(short), getInteger<unsigned short>, setUnsignedShort, compareItems<unsigned short>, true, false },
    { 'i', sizeof(int), getInteger<int>, setInt, compareItems<int>, true, true },
    { 'I', sizeof(int), getInteger<unsigned>, setUnsignedInt, compareItems<unsigned>, true, false },
    { 'l', sizeof(long), getInteger<long>, setLong, compareItems<long>, true, true },
    { 'L', sizeof(long), getInteger<unsigned long>, setUnsignedLong, compareItems<unsigned long>, true, false },
    { 'q', sizeof(long long), getInteger<long long>, setLongLong, compareItems<long long>, true, true },
    { 'Q', sizeof(long long), getInteger<unsigned long long>, setUnsignedLongLong, compareItems<unsigned long long>, true, false },
    { 'f', sizeof(float), getFloat, setFloating<float>, nullptr, false, false },
    { 'd', sizeof(double), getDouble, setFloating<double>, nullptr, false, false },
};

const ArrayDescriptor* descriptorFor(char32_t typecode)
{
    for (auto& descriptor : descriptors) {
        if (static_cast<char32_t>(descriptor.typecode) == typecode)
            return &descriptor;
    }
    return nullptr;
}

// getarrayitem()
JSValue getItem(JSGlobalObject* globalObject, const ArrayState& self, int64_t index) { return self.descriptor.getItem(globalObject, self.at(index)); }

// PyObject_RichCompareBool(item, value, Py_EQ), of an item that has just been made and so is nothing else: a NaN in an array is not to be found in it.
bool itemEquals(JSGlobalObject* globalObject, JSValue item, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = compare(globalObject, ComparisonOperator::Eq, item, value);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
}

// ins1(). False if it raised.
bool insert(JSGlobalObject* globalObject, ArrayState& self, int64_t where, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t n = self.size();
    self.descriptor.setItem(globalObject, self, -1, value);
    RETURN_IF_EXCEPTION(scope, false);
    // Finding that out can have run anything. It is how many there were that is gone by, as in CPython. What is added is zeros.
    resize(globalObject, self, n + 1);
    RETURN_IF_EXCEPTION(scope, false);
    if (where < 0)
        where = std::max<int64_t>(where + n, 0);
    where = std::min(where, n);
    if (where != n) {
        size_t itemSize = self.descriptor.itemSize;
        auto bytes = self.bytes();
        memmove(bytes.data() + (where + 1) * itemSize, bytes.data() + where * itemSize, (n - where) * itemSize);
    }
    RELEASE_AND_RETURN(scope, self.descriptor.setItem(globalObject, self, where, value));
}

// array_slice(). Null if it raised.
PyStateObject* slice(JSGlobalObject* globalObject, const ArrayState& self, int64_t low, int64_t high)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    low = std::clamp<int64_t>(low, 0, self.size());
    high = std::clamp<int64_t>(high, low, self.size());
    auto* result = newArray(globalObject, high - low, self.descriptor);
    RETURN_IF_EXCEPTION(scope, nullptr);
    size_t itemSize = self.descriptor.itemSize;
    if (high > low)
        memcpy(result->state<ArrayState>().bytes().data(), self.bytes().data() + low * itemSize, (high - low) * itemSize);
    return result;
}

// array_del_slice(). False if it raised.
bool deleteSlice(JSGlobalObject* globalObject, ArrayState& self, int64_t low, int64_t high)
{
    low = std::clamp<int64_t>(low, 0, self.size());
    high = std::clamp<int64_t>(high, low, self.size());
    int64_t count = high - low;
    if (count <= 0)
        return true;
    size_t itemSize = self.descriptor.itemSize;
    auto bytes = self.bytes();
    memmove(bytes.data() + low * itemSize, bytes.data() + high * itemSize, (self.size() - high) * itemSize);
    return resize(globalObject, self, self.size() - count);
}

// array_iter_extend(). False if it raised.
bool extendFromIterable(JSGlobalObject* globalObject, ArrayState& self, JSValue iterable)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue iterator = getIterator(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, false);
    for (;;) {
        JSValue value = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return true;
        insert(globalObject, self, self.size(), value);
        RETURN_IF_EXCEPTION(scope, false);
    }
}

// array_do_extend(). False if it raised.
bool extend(JSGlobalObject* globalObject, ArrayState& self, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isPythonArray(globalObject, other))
        RELEASE_AND_RETURN(scope, extendFromIterable(globalObject, self, other));
    auto& b = stateOf<ArrayState>(other);
    if (&self.descriptor != &b.descriptor) {
        raiseTypeError(globalObject, scope, "can only extend with array of same kind"_s);
        return false;
    }
    int64_t oldSize = self.size();
    // It may be this one.
    int64_t otherSize = b.size();
    if (oldSize > most - otherSize || oldSize + otherSize > most / static_cast<int64_t>(self.descriptor.itemSize)) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    resize(globalObject, self, oldSize + otherSize);
    RETURN_IF_EXCEPTION(scope, false);
    size_t itemSize = self.descriptor.itemSize;
    if (otherSize > 0)
        memcpy(self.bytes().data() + oldSize * itemSize, b.bytes().data(), otherSize * itemSize);
    return true;
}

// _PyBytes_Repeat(), where what is to be repeated is at the beginning of where it is to go
void repeatInPlace(std::span<uint8_t> destination, size_t length)
{
    for (size_t copied = length; copied < destination.size();) {
        size_t count = std::min(copied, destination.size() - copied);
        memcpy(destination.data() + copied, destination.data(), count);
        copied += count;
    }
}

// frombytes(). The bytes are not this array's own. False if it raised.
bool appendBytes(JSGlobalObject* globalObject, ArrayState& self, std::span<const uint8_t> bytes)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    size_t itemSize = self.descriptor.itemSize;
    if (bytes.size() % itemSize) {
        raiseValueError(globalObject, scope, "bytes length not a multiple of item size"_s);
        return false;
    }
    int64_t n = static_cast<int64_t>(bytes.size() / itemSize);
    if (n <= 0)
        return true;
    int64_t oldSize = self.size();
    if (n > most - oldSize || oldSize + n > most / static_cast<int64_t>(itemSize)) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    resize(globalObject, self, oldSize + n);
    RETURN_IF_EXCEPTION(scope, false);
    memcpy(self.bytes().data() + oldSize * itemSize, bytes.data(), bytes.size());
    return true;
}

// array.frombytes(buffer). False if it raised.
bool fromBytes(JSGlobalObject* globalObject, ArrayState& self, JSValue source)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Buffer buffer = bufferOf(globalObject, source);
    RETURN_IF_EXCEPTION(scope, false);
    if (itemSizeOfBuffer(buffer) != 1) {
        raiseTypeError(globalObject, scope, "a bytes-like object is required"_s);
        return false;
    }
    // They may be this array's own, which is about to be moved.
    Vector<uint8_t> copy;
    if (!copy.tryAppend(buffer.span())) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    RELEASE_AND_RETURN(scope, appendBytes(globalObject, self, copy.span()));
}

// The characters of a str, added to an array of them. False if it raised.
bool appendCharacters(JSGlobalObject* globalObject, ArrayState& self, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    Vector<uint8_t> bytes;
    bool hasRoom = true;
    auto append = [&] (auto unit) { hasRoom = hasRoom && bytes.tryAppend(asByteSpan(unit)); };
    if (self.descriptor.itemSize == 4) {
        for (char32_t c : view->codePoints())
            append(c);
    } else {
        // Where a wchar_t is two bytes, it is what a str is made of here.
        for (unsigned i = 0; i < view->length(); ++i)
            append(static_cast<char16_t>(view.data[i]));
    }
    if (!hasRoom) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    RELEASE_AND_RETURN(scope, appendBytes(globalObject, self, bytes.span()));
}

// array_array_tolist_impl(). Empty if it raised.
JSValue toList(JSGlobalObject* globalObject, const ArrayState& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    MarkedArgumentBuffer items;
    for (int64_t i = 0; i < self.size(); ++i) {
        JSValue item = getItem(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        items.append(item);
    }
    RELEASE_AND_RETURN(scope, newList(globalObject, items));
}

// array_array_tounicode_impl(). Empty if it raised.
JSValue toUnicode(JSGlobalObject* globalObject, const ArrayState& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    char typecode = self.descriptor.typecode;
    if (typecode != 'u' && typecode != 'w')
        return raiseValueError(globalObject, scope, "tounicode() may only be called on unicode type arrays ('u' or 'w')"_s);
    if (typecode == 'w') {
        // PyUnicode_DecodeUTF32(), in whichever order the bytes are in natively, unless they begin by saying which
        String text = decodeBytes(globalObject, self.bytes(), "utf-32"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, text));
    }
    // PyUnicode_FromWideChar()
    TextBuilder builder;
    for (int64_t i = 0; i < self.size(); ++i) {
        auto character = static_cast<uint32_t>(loadItem<wchar_t>(self.at(i)));
        if (character > 0x10ffff)
            return raiseValueError(globalObject, scope, concatenate("character U+"_s, hex(character, Lowercase), " is not in range [U+0000; U+10ffff]"_s));
        builder.append(static_cast<char32_t>(character));
    }
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, builder.toString()));
}

// ---- What goes through one

struct ArrayIteratorState final : NativeState {
    PYTHON_NATIVE_STATE(ArrayIteratorState);
    WriteBarrier<Unknown> array; // Empty when there is no more
    int64_t index { 0 };
};

template<typename Visitor> void ArrayIteratorState::visit(Visitor& visitor) { visitor.append(array); }

} // anonymous namespace

#define ARRAY_PROLOGUE() \
    NATIVE_PROLOGUE(); \
    auto& self = stateOf<ArrayState>(args[0])

// array_richcompare()
PYTHON_NATIVE(arrayCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!isPythonArray(globalObject, args[0]) || !isPythonArray(globalObject, args[1]))
        RETURN_NOT_IMPLEMENTED();
    auto& v = stateOf<ArrayState>(args[0]);
    auto& w = stateOf<ArrayState>(args[1]);
    if (v.size() != w.size() && isEquality(op))
        return JSValue::encode(jsBoolean(op == ComparisonOperator::NotEq));
    auto answer = [&] (auto left, auto right) {
        switch (op) {
        case ComparisonOperator::Lt:
            return left < right;
        case ComparisonOperator::LtE:
            return left <= right;
        case ComparisonOperator::Eq:
            return left == right;
        case ComparisonOperator::NotEq:
            return left != right;
        case ComparisonOperator::Gt:
            return left > right;
        case ComparisonOperator::GtE:
            return left >= right;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    if (&v.descriptor == &w.descriptor && v.descriptor.compareItems) {
        // Of the same kind, they can be compared as they lie.
        int result = v.descriptor.compareItems(v.bytes().data(), w.bytes().data(), static_cast<size_t>(std::min(v.size(), w.size())));
        return JSValue::encode(jsBoolean(result ? answer(result, 0) : answer(v.size(), w.size())));
    }
    // The first place at which they differ
    for (int64_t i = 0; i < v.size() && i < w.size(); ++i) {
        JSValue vi = getItem(globalObject, v, i);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue wi = getItem(globalObject, w, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = itemEquals(globalObject, vi, wi);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            continue;
        if (isEquality(op))
            return JSValue::encode(jsBoolean(op == ComparisonOperator::NotEq));
        RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, vi, wi)));
    }
    return JSValue::encode(jsBoolean(answer(v.size(), w.size())));
}

PYTHON_NATIVE(arrayLen)
{
    return JSValue::encode(intFromInt64(globalObject, stateOf<ArrayState>(callFrame->uncheckedArgument(0)).size()));
}

PYTHON_NATIVE(arrayClear)
{
    ARRAY_PROLOGUE();
    resize(globalObject, self, 0);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// __copy__() and __deepcopy__(unused, /)
PYTHON_NATIVE(arrayCopy)
{
    ARRAY_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(slice(globalObject, self, 0, self.size())));
}

// array_concat()
PYTHON_NATIVE(arrayAdd)
{
    ARRAY_PROLOGUE();
    if (!isPythonArray(globalObject, args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("can only append array (not \""_s, typeName(globalObject, args[1]), "\") to array"_s)));
    auto& other = stateOf<ArrayState>(args[1]);
    if (&self.descriptor != &other.descriptor)
        return JSValue::encode(raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s));
    if (self.size() > most - other.size())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    auto* result = newArray(globalObject, self.size() + other.size(), self.descriptor);
    RETURN_IF_EXCEPTION(scope, { });
    auto bytes = result->state<ArrayState>().bytes();
    memcpySpan(bytes, self.bytes());
    memcpySpan(bytes.subspan(self.bytes().size()), other.bytes());
    return JSValue::encode(result);
}

// array_repeat()
PYTHON_NATIVE(arrayMultiply)
{
    ARRAY_PROLOGUE();
    auto count = toIndexOrOverflow(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = std::max<int64_t>(*count, 0);
    int64_t length = self.size();
    if (length && n > most / length)
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    auto* result = newArray(globalObject, length * n, self.descriptor);
    RETURN_IF_EXCEPTION(scope, { });
    if (length * n) {
        auto bytes = result->state<ArrayState>().bytes();
        memcpySpan(bytes, self.bytes());
        repeatInPlace(bytes, self.bytes().size());
    }
    return JSValue::encode(result);
}

// array_inplace_concat()
PYTHON_NATIVE(arrayInPlaceAdd)
{
    ARRAY_PROLOGUE();
    if (!isPythonArray(globalObject, args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("can only extend array with array (not \""_s, typeName(globalObject, args[1]), "\")"_s)));
    extend(globalObject, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(args[0]);
}

// array_inplace_repeat()
PYTHON_NATIVE(arrayInPlaceMultiply)
{
    ARRAY_PROLOGUE();
    auto count = toIndexOrOverflow(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = *count;
    int64_t length = self.size();
    if (length > 0 && n != 1) {
        n = std::max<int64_t>(n, 0);
        int64_t byteLength = length * static_cast<int64_t>(self.descriptor.itemSize);
        if (n > 0 && byteLength > most / n)
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        resize(globalObject, self, n * length);
        RETURN_IF_EXCEPTION(scope, { });
        repeatInPlace(self.bytes(), static_cast<size_t>(byteLength));
    }
    return JSValue::encode(args[0]);
}

// count(v, /)
PYTHON_NATIVE(arrayCount)
{
    ARRAY_PROLOGUE();
    int64_t count = 0;
    for (int64_t i = 0; i < self.size(); ++i) {
        JSValue item = getItem(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = itemEquals(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        count += isSame;
    }
    return JSValue::encode(intFromInt64(globalObject, count));
}

// index(v, start=0, stop=sys.maxsize, /)
PYTHON_NATIVE(arrayIndex)
{
    ARRAY_PROLOGUE();
    int64_t start = 0;
    int64_t stop = most;
    if (JSValue given = args.at(2)) {
        auto index = toSliceIndex(globalObject, given, false);
        RETURN_IF_EXCEPTION(scope, { });
        start = *index;
    }
    if (JSValue given = args.at(3)) {
        auto index = toSliceIndex(globalObject, given, false);
        RETURN_IF_EXCEPTION(scope, { });
        stop = *index;
    }
    if (start < 0)
        start = std::max<int64_t>(start + self.size(), 0);
    if (stop < 0)
        stop += self.size();
    for (int64_t i = start; i < stop && i < self.size(); ++i) {
        JSValue item = getItem(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = itemEquals(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            return JSValue::encode(intFromInt64(globalObject, i));
    }
    return JSValue::encode(raiseValueError(globalObject, scope, "array.index(x): x not in array"_s));
}

PYTHON_NATIVE(arrayContains)
{
    ARRAY_PROLOGUE();
    for (int64_t i = 0; i < self.size(); ++i) {
        JSValue item = getItem(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = itemEquals(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            return JSValue::encode(jsBoolean(true));
    }
    return JSValue::encode(jsBoolean(false));
}

// remove(v, /)
PYTHON_NATIVE(arrayRemove)
{
    ARRAY_PROLOGUE();
    for (int64_t i = 0; i < self.size(); ++i) {
        JSValue item = getItem(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool isSame = itemEquals(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame) {
            deleteSlice(globalObject, self, i, i + 1);
            RETURN_IF_EXCEPTION(scope, { });
            RETURN_NONE();
        }
    }
    return JSValue::encode(raiseValueError(globalObject, scope, "array.remove(x): x not in array"_s));
}

// pop(i=-1, /)
PYTHON_NATIVE(arrayPop)
{
    ARRAY_PROLOGUE();
    int64_t i = -1;
    if (JSValue given = args.at(1)) {
        auto index = toSsize(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        i = *index;
    }
    if (!self.size())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop from empty array"_s));
    if (i < 0)
        i += self.size();
    if (i < 0 || i >= self.size())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop index out of range"_s));
    JSValue item = getItem(globalObject, self, i);
    RETURN_IF_EXCEPTION(scope, { });
    deleteSlice(globalObject, self, i, i + 1);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(item);
}

// extend(bb, /)
PYTHON_NATIVE(arrayExtend)
{
    ARRAY_PROLOGUE();
    extend(globalObject, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// insert(i, v, /)
PYTHON_NATIVE(arrayInsert)
{
    ARRAY_PROLOGUE();
    auto index = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    insert(globalObject, self, *index, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// append(v, /)
PYTHON_NATIVE(arrayAppend)
{
    ARRAY_PROLOGUE();
    insert(globalObject, self, self.size(), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(arrayBufferInfo)
{
    ARRAY_PROLOGUE();
    UNUSED_PARAM(scope);
    // Where they are now. It is for handing to what is written in C, and there is none of that here.
    uint64_t address = self.allocated ? std::bit_cast<uintptr_t>(self.bytes().data()) : 0;
    return JSValue::encode(PyTuple::create(globalObject, { intFromUInt64(globalObject, address), intFromInt64(globalObject, self.size()) }));
}

PYTHON_NATIVE(arrayByteSwap)
{
    ARRAY_PROLOGUE();
    UNUSED_PARAM(scope);
    size_t itemSize = self.descriptor.itemSize;
    auto bytes = self.bytes();
    for (size_t offset = 0; offset + itemSize <= bytes.size(); offset += itemSize)
        std::ranges::reverse(bytes.subspan(offset, itemSize));
    RETURN_NONE();
}

PYTHON_NATIVE(arrayReverse)
{
    ARRAY_PROLOGUE();
    UNUSED_PARAM(scope);
    size_t itemSize = self.descriptor.itemSize;
    auto bytes = self.bytes();
    for (size_t low = 0, high = bytes.size(); low + 2 * itemSize <= high; low += itemSize, high -= itemSize)
        std::swap_ranges(bytes.begin() + low, bytes.begin() + low + itemSize, bytes.begin() + high - itemSize);
    RETURN_NONE();
}

// fromfile(f, n, /)
PYTHON_NATIVE(arrayFromFile)
{
    ARRAY_PROLOGUE();
    auto count = toSsize(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = *count;
    int64_t itemSize = self.descriptor.itemSize;
    if (n < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "negative count"_s));
    if (n > most / itemSize)
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    int64_t byteCount = n * itemSize;
    JSValue read = getAttribute(globalObject, args[1], Identifier::fromString(vm, "read"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue data = call(globalObject, read, intFromInt64(globalObject, byteCount));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance(globalObject, data, realm->typeBytes()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "read() didn't return bytes"_s));
    bool isNotEnough = static_cast<int64_t>(Buffer(data).size()) != byteCount;
    fromBytes(globalObject, self, data);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNotEnough)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::EOFError, "read() didn't return enough bytes"_s));
    RETURN_NONE();
}

// tofile(f, /)
PYTHON_NATIVE(arrayToFile)
{
    ARRAY_PROLOGUE();
    constexpr size_t blockSize = 64 * 1024;
    size_t byteCount = self.bytes().size();
    for (size_t offset = 0; offset < byteCount; offset += blockSize) {
        // What is written to can do anything, and there may be less here than there was.
        auto bytes = self.bytes();
        if (offset >= bytes.size())
            break;
        JSValue block = newBytes(globalObject, bytes.subspan(offset, std::min({ blockSize, byteCount - offset, bytes.size() - offset })));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue write = getAttribute(globalObject, args[1], Identifier::fromString(vm, "write"_s));
        RETURN_IF_EXCEPTION(scope, { });
        call(globalObject, write, block);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// fromlist(list, /)
PYTHON_NATIVE(arrayFromList)
{
    ARRAY_PROLOGUE();
    JSArray* list = tryList(args[1]);
    if (!list)
        return JSValue::encode(raiseTypeError(globalObject, scope, "arg must be list"_s));
    int64_t n = list->length();
    if (n <= 0)
        RETURN_NONE();
    int64_t oldSize = self.size();
    resize(globalObject, self, oldSize + n);
    RETURN_IF_EXCEPTION(scope, { });
    auto undo = [&] {
        Exception* raised = takeRaisedException(vm);
        resize(globalObject, self, oldSize);
        if (scope.exception() && !scope.tryClearException())
            return;
        restoreRaisedException(globalObject, raised);
    };
    for (int64_t i = 0; i < n; ++i) {
        self.descriptor.setItem(globalObject, self, self.size() - n + i, list->getIndexQuickly(static_cast<unsigned>(i)));
        if (scope.exception()) [[unlikely]] {
            undo();
            return { };
        }
        if (n != static_cast<int64_t>(list->length())) {
            raise(globalObject, scope, BuiltinType::RuntimeError, "list changed size during iteration"_s);
            undo();
            return { };
        }
    }
    RETURN_NONE();
}

PYTHON_NATIVE(arrayToList)
{
    ARRAY_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(toList(globalObject, self)));
}

// frombytes(buffer, /)
PYTHON_NATIVE(arrayFromBytes)
{
    ARRAY_PROLOGUE();
    fromBytes(globalObject, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(arrayToBytes)
{
    ARRAY_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, self.bytes())));
}

// fromunicode(ustr, /)
PYTHON_NATIVE(arrayFromUnicode)
{
    ARRAY_PROLOGUE();
    JSString* string = stringIn(args[1]);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("fromunicode() argument must be str, not "_s, typeNameOfArgument(globalObject, args[1]))));
    if (self.descriptor.typecode != 'u' && self.descriptor.typecode != 'w')
        return JSValue::encode(raiseValueError(globalObject, scope, "fromunicode() may only be called on unicode type arrays ('u' or 'w')"_s));
    appendCharacters(globalObject, self, string);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(arrayToUnicode)
{
    ARRAY_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(toUnicode(globalObject, self)));
}

PYTHON_NATIVE(arraySizeOf)
{
    ARRAY_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(intFromInt64(globalObject, typeOf(globalObject, args[0])->basicSize() + self.allocated * static_cast<int64_t>(self.descriptor.itemSize)));
}

PYTHON_NATIVE(arrayRepr)
{
    ARRAY_PROLOGUE();
    String name = typeOf(globalObject, args[0])->nameWithoutModule(globalObject);
    char typecode = self.descriptor.typecode;
    if (!self.size())
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, "('"_s, typecode, "')"_s))));
    JSValue contents = typecode == 'u' || typecode == 'w' ? toUnicode(globalObject, self) : toList(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    String shown = repr(globalObject, contents);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, "('"_s, typecode, "', "_s, shown, ')'))));
}

static bool isIndexObject(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    return classify(value).isInt() || typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index);
}

// array_subscr()
PYTHON_NATIVE(arrayGetItem)
{
    ARRAY_PROLOGUE();
    JSValue key = args[1];
    if (isIndexObject(globalObject, key)) {
        auto index = toIndex(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        int64_t i = *index < 0 ? *index + self.size() : *index;
        if (i < 0 || i >= self.size())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "array index out of range"_s));
        RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, self, i)));
    }
    auto* keySlice = trySlice(key);
    if (!keySlice)
        return JSValue::encode(raiseTypeError(globalObject, scope, "array indices must be integers"_s));
    auto indices = keySlice->indices(globalObject, [&] { return self.size(); });
    RETURN_IF_EXCEPTION(scope, { });
    auto* result = newArray(globalObject, std::max<int64_t>(indices->length, 0), self.descriptor);
    RETURN_IF_EXCEPTION(scope, { });
    size_t itemSize = self.descriptor.itemSize;
    uint8_t* to = result->state<ArrayState>().bytes().data();
    const uint8_t* from = self.bytes().data();
    if (indices->length > 0 && indices->step == 1)
        memcpy(to, from + indices->start * itemSize, indices->length * itemSize);
    else {
        // A step can be as large as a number can be, and one more step than there are items to take goes past that. So where it has got to is counted without a sign, as in CPython.
        uint64_t current = static_cast<uint64_t>(indices->start);
        for (int64_t i = 0; i < indices->length; ++i, current += static_cast<uint64_t>(indices->step))
            memcpy(to + i * itemSize, from + current * itemSize, itemSize);
    }
    return JSValue::encode(result);
}

// array_ass_subscr(). `value` is empty to delete. False if it raised.
static bool assignSubscript(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& self = stateOf<ArrayState>(object);
    int64_t start;
    int64_t stop;
    int64_t step;
    int64_t sliceLength;
    if (isIndexObject(globalObject, key)) {
        auto index = toIndex(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        int64_t i = *index < 0 ? *index + self.size() : *index;
        if (i < 0 || i >= self.size()) {
            raise(globalObject, scope, BuiltinType::IndexError, "array assignment index out of range"_s);
            return false;
        }
        if (value)
            RELEASE_AND_RETURN(scope, self.descriptor.setItem(globalObject, self, i, value));
        start = i;
        stop = i + 1;
        step = 1;
        sliceLength = 1;
    } else if (auto* keySlice = trySlice(key)) {
        auto indices = keySlice->indices(globalObject, [&] { return self.size(); });
        RETURN_IF_EXCEPTION(scope, false);
        start = indices->start;
        stop = indices->stop;
        step = indices->step;
        sliceLength = indices->length;
    } else {
        raiseTypeError(globalObject, scope, "array indices must be integers"_s);
        return false;
    }
    const ArrayState* other = nullptr;
    int64_t needed = 0;
    if (value) {
        if (!isPythonArray(globalObject, value)) {
            raiseTypeError(globalObject, scope, concatenate("can only assign array (not \""_s, typeName(globalObject, value), "\") to array slice"_s));
            return false;
        }
        other = &stateOf<ArrayState>(value);
        needed = other->size();
        if (other == &self) {
            // a[i:j] = a: a copy of it is what is assigned.
            auto* copy = slice(globalObject, self, 0, needed);
            RETURN_IF_EXCEPTION(scope, false);
            RELEASE_AND_RETURN(scope, assignSubscript(globalObject, object, key, copy));
        }
        if (&other->descriptor != &self.descriptor) {
            raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
            return false;
        }
    }
    size_t itemSize = self.descriptor.itemSize;
    // For a[2:1] = ..., it goes in where the slice begins.
    if ((step > 0 && stop < start) || (step < 0 && stop > start))
        stop = start;
    if (step == 1) {
        if (sliceLength > needed) {
            uint8_t* items = self.bytes().data();
            memmove(items + (start + needed) * itemSize, items + stop * itemSize, (self.size() - stop) * itemSize);
            resize(globalObject, self, self.size() + needed - sliceLength);
            RETURN_IF_EXCEPTION(scope, false);
        } else if (sliceLength < needed) {
            resize(globalObject, self, self.size() + needed - sliceLength);
            RETURN_IF_EXCEPTION(scope, false);
            uint8_t* items = self.bytes().data();
            memmove(items + (start + needed) * itemSize, items + stop * itemSize, (self.size() - start - needed) * itemSize);
        }
        if (needed > 0)
            memcpy(self.bytes().data() + start * itemSize, other->bytes().data(), needed * itemSize);
        return true;
    }
    if (!needed) {
        // What is between those that go is moved down, a stretch at a time.
        if (sliceLength <= 0)
            return true;
        if (step < 0) {
            stop = start + 1;
            start = stop + step * (sliceLength - 1) - 1;
            step = -step;
        }
        uint8_t* items = self.bytes().data();
        // Without a sign, as in CPython: `del a[9::1 << 333]` has a step to which nothing can be added.
        uint64_t size = static_cast<uint64_t>(self.size());
        uint64_t stride = static_cast<uint64_t>(step);
        uint64_t current = static_cast<uint64_t>(start);
        for (uint64_t i = 0; i < static_cast<uint64_t>(sliceLength); ++i, current += stride) {
            uint64_t count = current + stride >= size ? size - current - 1 : stride - 1;
            memmove(items + (current - i) * itemSize, items + (current + 1) * itemSize, count * itemSize);
        }
        current = static_cast<uint64_t>(start) + static_cast<uint64_t>(sliceLength) * stride;
        if (current < size)
            memmove(items + (current - static_cast<uint64_t>(sliceLength)) * itemSize, items + current * itemSize, (size - current) * itemSize);
        RELEASE_AND_RETURN(scope, resize(globalObject, self, static_cast<int64_t>(size) - sliceLength));
    }
    if (needed != sliceLength) {
        raiseValueError(globalObject, scope, concatenate("attempt to assign array of size "_s, needed, " to extended slice of size "_s, sliceLength));
        return false;
    }
    uint8_t* items = self.bytes().data();
    const uint8_t* from = other->bytes().data();
    uint64_t current = static_cast<uint64_t>(start);
    for (int64_t i = 0; i < sliceLength; ++i, current += static_cast<uint64_t>(step))
        memcpy(items + current * itemSize, from + i * itemSize, itemSize);
    return true;
}

// __setitem__(key, value) and __delitem__(key)
PYTHON_NATIVE(arraySetItem)
{
    NATIVE_PROLOGUE();
    assignSubscript(globalObject, args[0], args[1], args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// ---- Pickling

enum MachineFormatCode : int {
    UnsignedInt8 = 0,
    SignedInt8 = 1,
    UnsignedInt16LE = 2,
    UnsignedInt32LE = 6,
    UnsignedInt64LE = 10,
    SignedInt64BE = 13,
    IEEE754FloatLE = 14,
    IEEE754FloatBE = 15,
    IEEE754DoubleLE = 16,
    IEEE754DoubleBE = 17,
    UTF16LE = 18,
    UTF16BE = 19,
    UTF32LE = 20,
    UTF32BE = 21,
};

struct MachineFormat {
    size_t size;
    bool isSigned;
    bool isBigEndian;
};

// The sizes of the last four are as they are in CPython, and are not the sizes of the items.
static constexpr MachineFormat machineFormats[] = {
    { 1, false, false }, { 1, true, false },
    { 2, false, false }, { 2, false, true }, { 2, true, false }, { 2, true, true },
    { 4, false, false }, { 4, false, true }, { 4, true, false }, { 4, true, true },
    { 8, false, false }, { 8, false, true }, { 8, true, false }, { 8, true, true },
    { 4, false, false }, { 4, false, true },
    { 8, false, false }, { 8, false, true },
    { 4, false, false }, { 4, false, true },
    { 8, false, false }, { 8, false, true },
};

// typecode_to_mformat_code()
static int machineFormatOf(const ArrayDescriptor& descriptor)
{
    switch (descriptor.typecode) {
    case 'b':
        return SignedInt8;
    case 'B':
        return UnsignedInt8;
    case 'u':
        return sizeof(wchar_t) == 2 ? UTF16LE : UTF32LE;
    case 'w':
        return UTF32LE;
    case 'f':
        return IEEE754FloatLE;
    case 'd':
        return IEEE754DoubleLE;
    default:
        break;
    }
    int first = descriptor.itemSize == 2 ? UnsignedInt16LE : descriptor.itemSize == 4 ? UnsignedInt32LE : UnsignedInt64LE;
    return first + 2 * descriptor.isSigned;
}

static JSValue makeArray(JSGlobalObject*, PyType*, char32_t typecode, JSValue initial);

// _array_reconstructor(arraytype, typecode, mformat_code, items, /)
PYTHON_NATIVE(arrayReconstructor)
{
    NATIVE_PROLOGUE();
    // The arguments are all made what they are to be before any is looked at.
    JSString* typecodeString = stringIn(args[1]);
    if (!typecodeString)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_array_reconstructor() argument 2 must be a unicode character, not "_s, typeNameOfArgument(globalObject, args[1]))));
    auto typecodeView = typecodeString->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    size_t typecodeLength = 0;
    char32_t typecode = 0;
    for (char32_t c : typecodeView->codePoints()) {
        if (!typecodeLength++)
            typecode = c;
    }
    if (typecodeLength != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_array_reconstructor(): argument 2 must be a unicode character, not a string of length "_s, typecodeLength)));
    auto code = toCInt(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    int format = *code;
    JSValue items = args[3];

    PyType* arrayType = moduleStateOf(globalObject).array.get();
    if (!isClass(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("first argument must be a type object, not "_s, typeName(globalObject, args[0]))));
    PyType* type = asType(args[0]);
    if (!type->isSubtypeOf(arrayType))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), " is not a subtype of "_s, arrayType->nameString(globalObject))));
    const ArrayDescriptor* descriptor = descriptorFor(typecode);
    if (!descriptor)
        return JSValue::encode(raiseValueError(globalObject, scope, "second argument must be a valid type code"_s));
    if (format < 0 || format > UTF32BE)
        return JSValue::encode(raiseValueError(globalObject, scope, "third argument must be a valid machine format code."_s));
    if (!isInstance(globalObject, items, realm->typeBytes()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("fourth argument should be bytes, not "_s, typeName(globalObject, items))));
    // They are as they would be here.
    if (format == machineFormatOf(*descriptor))
        RELEASE_AND_RETURN(scope, JSValue::encode(makeArray(globalObject, type, typecode, items)));

    // They were pickled by a machine of another kind.
    Vector<uint8_t> bytes;
    if (!bytes.tryAppend(Buffer(items).span()))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    auto& machineFormat = machineFormats[format];
    if (bytes.size() % machineFormat.size)
        return JSValue::encode(raiseValueError(globalObject, scope, "string length not a multiple of item size"_s));
    JSValue converted;
    switch (format) {
    case IEEE754FloatLE:
    case IEEE754FloatBE:
    case IEEE754DoubleLE:
    case IEEE754DoubleBE: {
        MarkedArgumentBuffer values;
        bool isLittleEndian = !machineFormat.isBigEndian;
        for (size_t offset = 0; offset < bytes.size(); offset += machineFormat.size) {
            auto item = bytes.span().subspan(offset);
            values.append(floatFromDouble(machineFormat.size == 4 ? unpackFloat4(item.first<4>(), isLittleEndian) : unpackFloat8(item.first<8>(), isLittleEndian)));
        }
        converted = newList(globalObject, values);
        break;
    }
    case UTF16LE:
    case UTF16BE:
    case UTF32LE:
    case UTF32BE: {
        ASCIILiteral encoding = format == UTF16LE ? "utf-16-le"_s : format == UTF16BE ? "utf-16-be"_s : format == UTF32LE ? "utf-32-le"_s : "utf-32-be"_s;
        String text = decodeBytes(globalObject, bytes.span(), encoding, "strict"_s);
        RETURN_IF_EXCEPTION(scope, { });
        converted = strOrMemoryError(globalObject, text);
        break;
    }
    default: {
        // With whichever kind of item fits best, which may be narrower or wider than it was: the last that does.
        for (auto& candidate : descriptors) {
            if (candidate.isIntegerType && candidate.itemSize == machineFormat.size && candidate.isSigned == machineFormat.isSigned)
                typecode = static_cast<char32_t>(candidate.typecode);
        }
        MarkedArgumentBuffer values;
        for (size_t offset = 0; offset < bytes.size(); offset += machineFormat.size) {
            uint64_t bits = 0;
            for (size_t i = 0; i < machineFormat.size; ++i)
                bits = bits << 8 | bytes[offset + (machineFormat.isBigEndian ? i : machineFormat.size - 1 - i)];
            unsigned unused = static_cast<unsigned>((8 - machineFormat.size) * 8);
            values.append(machineFormat.isSigned ? intFromInt64(globalObject, static_cast<int64_t>(bits << unused) >> unused) : intFromUInt64(globalObject, bits));
        }
        converted = newList(globalObject, values);
        break;
    }
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(makeArray(globalObject, type, typecode, converted)));
}

// __reduce_ex__(value, /)
PYTHON_NATIVE(arrayReduceEx)
{
    ARRAY_PROLOGUE();
    auto& state = moduleStateOf(globalObject);
    if (!state.reconstructor) {
        JSValue reconstructor = importModuleAttribute(globalObject, "array"_s, "_array_reconstructor"_s);
        RETURN_IF_EXCEPTION(scope, { });
        state.reconstructor.set(vm, realm, reconstructor);
    }
    if (!classify(args[1]).isInt())
        return JSValue::encode(raiseTypeError(globalObject, scope, "__reduce_ex__ argument should be an integer"_s));
    auto protocol = toCLong(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue dict = getAttributeIfPresent(globalObject, args[0], names.dunder_dict);
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        dict = jsUndefined();
    JSValue type = typeOf(globalObject, args[0])->object();
    JSValue typecode = strOfCharacter(globalObject, static_cast<char32_t>(self.descriptor.typecode));
    if (*protocol < 3) {
        // As a list, since what Python 2 pickles as a str is unpickled as text.
        JSValue list = toList(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { typecode, list }), dict }));
    }
    JSValue bytes = newBytes(globalObject, self.bytes());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { state.reconstructor.get(), PyTuple::create(globalObject, { type, typecode, jsNumber(machineFormatOf(self.descriptor)), bytes }), dict }));
}

// ---- array(typecode, initializer=<none>)

// array_new(), when its arguments have been taken apart. Empty if it raised.
static JSValue makeArray(JSGlobalObject* globalObject, PyType* type, char32_t typecode, JSValue initial)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (!audit(globalObject, "array.__new__"_s, strOfCharacter(globalObject, typecode), initial ? initial : jsUndefined()))
        return { };
    if (typecode == 'u') {
        warn(globalObject, BuiltinType::DeprecationWarning, "The 'u' type code is deprecated and will be removed in Python 3.16"_s);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool isUnicode = typecode == 'u' || typecode == 'w';
    // "%c"
    auto shownTypecode = [&] {
        TextBuilder builder;
        builder.append(typecode);
        return builder.toString();
    };
    bool initialIsArray = initial && isPythonArray(globalObject, initial);
    if (initial && !isUnicode) {
        if (stringIn(initial))
            return raiseTypeError(globalObject, scope, concatenate("cannot use a str to initialize an array with typecode '"_s, shownTypecode(), '\''));
        if (initialIsArray) {
            char other = stateOf<ArrayState>(initial).descriptor.typecode;
            if (other == 'u' || other == 'w')
                return raiseTypeError(globalObject, scope, concatenate("cannot use a unicode array to initialize an array with typecode '"_s, shownTypecode(), '\''));
        }
    }
    bool isBytesLike = initial && (isInstance(globalObject, initial, realm->typeBytes()) || isInstance(globalObject, initial, realm->typeByteArray()));
    bool isListOrTuple = initial && (isList(initial) || isTuple(initial));
    bool isSameKindOfArray = initialIsArray && static_cast<char32_t>(stateOf<ArrayState>(initial).descriptor.typecode) == typecode;
    JSValue iterator;
    if (initial && !isListOrTuple && !isBytesLike && !(isUnicode && stringIn(initial)) && !isSameKindOfArray) {
        iterator = getIterator(globalObject, initial);
        RETURN_IF_EXCEPTION(scope, { });
        initial = { };
        initialIsArray = false;
    }
    const ArrayDescriptor* descriptor = descriptorFor(typecode);
    if (!descriptor)
        return raiseValueError(globalObject, scope, "bad typecode (must be b, B, u, w, h, H, i, I, l, L, q, Q, f or d)"_s);

    int64_t length = 0;
    // How many there are in it, and not how many a class derived from it may say that there are
    if (initial && isListOrTuple)
        length = isList(initial) ? asList(initial)->length() : asTuple(initial)->length();
    else if (initialIsArray)
        length = stateOf<ArrayState>(initial).size();
    auto* object = newArray(globalObject, type, length, *descriptor);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = object->state<ArrayState>();
    if (length > 0 && !initialIsArray) {
        for (int64_t i = 0; i < length; ++i) {
            // PySequence_GetItem(): making numbers of those before it can have run anything.
            JSValue item = getItem(globalObject, initial, intFromInt64(globalObject, i));
            RETURN_IF_EXCEPTION(scope, { });
            if (i >= self.size())
                return raise(globalObject, scope, BuiltinType::IndexError, "array assignment index out of range"_s);
            descriptor->setItem(globalObject, self, i, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else if (isBytesLike) {
        fromBytes(globalObject, self, initial);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (initial && stringIn(initial)) {
        appendCharacters(globalObject, self, stringIn(initial));
        RETURN_IF_EXCEPTION(scope, { });
        // CPython has just as much room as it takes.
        self.allocated = self.size();
    } else if (initialIsArray && length > 0)
        memcpySpan(self.bytes(), stateOf<ArrayState>(initial).bytes().first(self.bytes().size()));
    if (iterator) {
        extendFromIterable(globalObject, self, iterator);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return object;
}

PYTHON_NATIVE(arrayNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    PyType* arrayType = moduleStateOf(globalObject).array.get();
    // A class derived from it that has an __init__() of its own may be given anything by name, for that to see.
    if ((type == arrayType || type->lookup(vm, names.dunder_init) == arrayType->lookup(vm, names.dunder_init)) && !args.checkNoKeywords(globalObject, scope, "array.array"_s))
        return { };
    // "C|O:array"
    size_t count = args.size() - 1;
    if (count < 1 || count > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("array() takes at "_s, count < 1 ? "least 1"_s : "most 2"_s, count < 1 ? " argument ("_s : " arguments ("_s, count, " given)"_s)));
    JSString* string = stringIn(args[1]);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("array() argument 1 must be a unicode character, not "_s, typeNameOfArgument(globalObject, args[1]))));
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    size_t length = 0;
    char32_t typecode = 0;
    for (char32_t c : view->codePoints()) {
        if (!length++)
            typecode = c;
    }
    if (length != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("array() argument 1 must be a unicode character, not a string of length "_s, length)));
    RELEASE_AND_RETURN(scope, JSValue::encode(makeArray(globalObject, type, typecode, args.at(2))));
}

// ---- What goes through one

PYTHON_NATIVE(arrayIter)
{
    VM& vm = globalObject->vm();
    auto* iterator = PyStateObject::create(vm, moduleStateOf(globalObject).iterator->instanceStructure(), makeUnique<ArrayIteratorState>());
    iterator->state<ArrayIteratorState>().array.set(vm, iterator, callFrame->uncheckedArgument(0));
    return JSValue::encode(iterator);
}

PYTHON_NATIVE(arrayIteratorSelf)
{
    UNUSED_PARAM(globalObject);
    return JSValue::encode(callFrame->uncheckedArgument(0));
}

PYTHON_NATIVE(arrayIteratorNext)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ArrayIteratorState>(args[0]);
    if (self.array) {
        auto& array = stateOf<ArrayState>(self.array.get());
        if (self.index < array.size())
            RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, array, self.index++)));
        self.array.clear();
    }
    return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
}

PYTHON_NATIVE(arrayIteratorReduce)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ArrayIteratorState>(args[0]);
    JSValue iter = getBuiltin(globalObject, "iter"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!self.array)
        return JSValue::encode(PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { PyTuple::create(globalObject, 0) }) }));
    return JSValue::encode(PyTuple::create(globalObject, { iter, PyTuple::create(globalObject, { self.array.get() }), intFromInt64(globalObject, self.index) }));
}

// __setstate__(state, /)
PYTHON_NATIVE(arrayIteratorSetState)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<ArrayIteratorState>(args[0]);
    auto index = toSsizeOfInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (self.array)
        self.index = std::clamp<int64_t>(*index, 0, stateOf<ArrayState>(self.array.get()).size());
    RETURN_NONE();
}

JSObject* createArrayModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& names = vm.pythonNames();
    auto& state = moduleStateOf(globalObject);
    using Kind = PyNativeFunction::Kind;
    constexpr auto withDefiningClass = PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass;
    if (!state.array) {
        PyType* type = createBuiltinType(globalObject, "array.array"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType | PyType::IsSequence | PyType::HasWeakReferences | PyType::AddsAsSequence);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.array.set(vm, realm, type);
        addGenericGetAttribute(globalObject, type);
        addComparisons(globalObject, type, arrayCompare);
        type->putDirect(vm, names.dunder_hash, jsUndefined());
        addBufferMethods(globalObject, type);
        addClassGetItemIfGeneric(globalObject, type);
        addMethods(globalObject, type, {
            { "__new__"_s, arrayNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked },
            { "__repr__"_s, arrayRepr },
            { "__len__"_s, arrayLen },
            { "__getitem__"_s, arrayGetItem },
            { "__setitem__"_s, arraySetItem },
            { "__delitem__"_s, arraySetItem },
            { "__contains__"_s, arrayContains },
            { "__iter__"_s, arrayIter },
            { "__add__"_s, arrayAdd },
            { "__iadd__"_s, arrayInPlaceAdd },
            { "__mul__"_s, arrayMultiply },
            { "__rmul__"_s, arrayMultiply },
            { "__imul__"_s, arrayInPlaceMultiply },
            { "__copy__"_s, arrayCopy },
            { "__deepcopy__"_s, arrayCopy },
            { "__reduce_ex__"_s, arrayReduceEx, Kind::Method, 0, { }, withDefiningClass },
            { "__sizeof__"_s, arraySizeOf },
            { "append"_s, arrayAppend },
            { "buffer_info"_s, arrayBufferInfo },
            { "byteswap"_s, arrayByteSwap },
            { "clear"_s, arrayClear },
            { "count"_s, arrayCount },
            { "extend"_s, arrayExtend, Kind::Method, 0, { }, withDefiningClass },
            { "fromfile"_s, arrayFromFile, Kind::Method, 0, { }, withDefiningClass },
            { "fromlist"_s, arrayFromList },
            { "frombytes"_s, arrayFromBytes },
            { "fromunicode"_s, arrayFromUnicode },
            { "index"_s, arrayIndex },
            { "insert"_s, arrayInsert },
            { "pop"_s, arrayPop },
            { "remove"_s, arrayRemove },
            { "reverse"_s, arrayReverse },
            { "tofile"_s, arrayToFile, Kind::Method, 0, { }, withDefiningClass },
            { "tolist"_s, arrayToList },
            { "tobytes"_s, arrayToBytes },
            { "tounicode"_s, arrayToUnicode },
        });
        addGetSet(globalObject, type, "typecode"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return strOfCharacter(globalObject, static_cast<char32_t>(stateOf<ArrayState>(self).descriptor.typecode)); });
        addGetSet(globalObject, type, "itemsize"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<ArrayState>(self).descriptor.itemSize); });

        PyType* iterator = createBuiltinType(globalObject, "array.arrayiterator"_s, realm->typeObject(), PyType::Layout::Native, 0);
        iterator->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, iterator));
        state.iterator.set(vm, realm, iterator);
        addGenericGetAttribute(globalObject, iterator);
        addMethods(globalObject, iterator, {
            { "__iter__"_s, arrayIteratorSelf },
            { "__next__"_s, arrayIteratorNext },
            { "__reduce__"_s, arrayIteratorReduce, Kind::Method, 0, { }, withDefiningClass },
            { "__setstate__"_s, arrayIteratorSetState },
        });
    }
    JSObject* module = newBuiltinModule(globalObject, "array"_s);
    addFunction(globalObject, module, "_array_reconstructor"_s, arrayReconstructor);
    module->putDirect(vm, Identifier::fromString(vm, "ArrayType"_s), state.array.get());
    // collections.abc.MutableSequence.register(array)
    JSValue mutableSequence = importModuleAttribute(globalObject, "collections.abc"_s, "MutableSequence"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSValue registerMethod = getAttribute(globalObject, mutableSequence, Identifier::fromString(vm, "register"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    call(globalObject, registerMethod, state.array.get());
    RETURN_IF_EXCEPTION(scope, nullptr);
    module->putDirect(vm, Identifier::fromString(vm, "array"_s), state.array.get());
    TextBuilder typecodes;
    for (auto& descriptor : descriptors)
        typecodes.append(descriptor.typecode);
    module->putDirect(vm, Identifier::fromString(vm, "typecodes"_s), jsString(vm, typecodes.toString()));
    return module;
}

} } // namespace JSC::Python
