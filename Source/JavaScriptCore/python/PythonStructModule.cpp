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
#include "PyDict.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonIO.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"

// The module _struct: Modules/_struct.c of CPython.

namespace JSC { namespace Python {

namespace {

static_assert(std::endian::native == std::endian::little);

struct StructModuleState final : NativeState {
    PYTHON_NATIVE_STATE(StructModuleState);
    WriteBarrier<PyDict> cache; // A Struct for each format that the functions of the module have been given
    WriteBarrier<PyType> structType;
    WriteBarrier<PyType> unpackIterator;
    WriteBarrier<PyType> error;
};

template<typename Visitor>
void StructModuleState::visit(Visitor& visitor)
{
    visitor.append(cache);
    visitor.append(structType);
    visitor.append(unpackIterator);
    visitor.append(error);
}

StructModuleState& structModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<StructModuleState>(); }

// struct.error
JSValue raiseStructError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message)
{
    JSObject* exception = createException(globalObject, structModuleState(globalObject).error.get(), jsString(globalObject->vm(), message));
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

// ---- What is done for each character of a format

struct FormatDefinition {
    char format;
    ptrdiff_t size;
    ptrdiff_t alignment;
    JSValue (*unpack)(JSGlobalObject*, const uint8_t*, const FormatDefinition&);
    bool (*pack)(JSGlobalObject*, uint8_t*, JSValue, const FormatDefinition&); // False if it raised.
};

// _range_error()
bool raiseRangeError(JSGlobalObject* globalObject, ThrowScope& scope, const FormatDefinition& f, bool isUnsigned)
{
    uint64_t largestUnsigned = std::numeric_limits<uint64_t>::max() >> ((8 - f.size) * 8);
    if (isUnsigned)
        raiseStructError(globalObject, scope, concatenate('\'', f.format, "' format requires 0 <= number <= "_s, largestUnsigned));
    else {
        int64_t largest = static_cast<int64_t>(largestUnsigned >> 1);
        raiseStructError(globalObject, scope, concatenate('\'', f.format, "' format requires "_s, ~largest, " <= number <= "_s, largest));
    }
    return false;
}

// get_pylong(): an int, from an int or from what has __index__(). Empty if it raised.
JSValue toPyLong(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!classify(value).isInt() && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index))
        return raiseStructError(globalObject, scope, "required argument is not an integer"_s);
    RELEASE_AND_RETURN(scope, toInt(globalObject, value));
}

void store(uint8_t* p, uint64_t bits, ptrdiff_t size, bool isLittleEndian)
{
    for (ptrdiff_t i = 0; i < size; ++i, bits >>= 8)
        p[isLittleEndian ? i : size - 1 - i] = static_cast<uint8_t>(bits);
}

uint64_t load(const uint8_t* p, ptrdiff_t size, bool isLittleEndian)
{
    uint64_t bits = 0;
    for (ptrdiff_t i = 0; i < size; ++i)
        bits = bits << 8 | p[isLittleEndian ? size - 1 - i : i];
    return bits;
}

// The ints, of every size, of either kind, either way round: np_byte() to np_ulonglong(), bp_int() to bp_ulonglong() and lp_int() to lp_ulonglong(). They differ in how they come by it and not in what they come by, or in what
// they say of what there is no room for.
template<bool isSigned, bool isLittleEndian>
bool packInteger(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition& f)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toPyLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    uint64_t bits;
    if constexpr (isSigned) {
        auto number = tryInt64(integer);
        int64_t largest = static_cast<int64_t>((std::numeric_limits<uint64_t>::max() >> ((8 - f.size) * 8)) >> 1);
        if (!number || *number > largest || *number < ~largest)
            return raiseRangeError(globalObject, scope, f, false);
        bits = static_cast<uint64_t>(*number);
    } else {
        bits = lowBitsOfInt(integer);
        if (compareInts(integer, jsNumber(0)) < 0 || compareInts(integer, intFromUInt64(globalObject, bits)) || bits > std::numeric_limits<uint64_t>::max() >> ((8 - f.size) * 8))
            return raiseRangeError(globalObject, scope, f, true);
    }
    store(p, bits, f.size, isLittleEndian);
    return true;
}

template<bool isSigned, bool isLittleEndian>
JSValue unpackInteger(JSGlobalObject* globalObject, const uint8_t* p, const FormatDefinition& f)
{
    uint64_t bits = load(p, f.size, isLittleEndian);
    if constexpr (!isSigned)
        return intFromUInt64(globalObject, bits);
    unsigned unused = static_cast<unsigned>((8 - f.size) * 8);
    return intFromInt64(globalObject, static_cast<int64_t>(bits << unused) >> unused);
}

// nu_char() and np_char()
JSValue unpackChar(JSGlobalObject* globalObject, const uint8_t* p, const FormatDefinition&) { return newBytes(globalObject, std::span(p, 1)); }

bool packChar(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition&)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isInstance(globalObject, value, globalObject->pyRealm()->typeBytes())) {
        Buffer buffer(value);
        if (buffer.size() == 1) {
            *p = buffer[0];
            return true;
        }
    }
    raiseStructError(globalObject, scope, "char format requires a bytes object of length 1"_s);
    return false;
}

// nu_bool() and bu_bool(), and np_bool() and bp_bool()
JSValue unpackBool(JSGlobalObject*, const uint8_t* p, const FormatDefinition&) { return jsBoolean(*p); }

bool packBool(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition&)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    bool truth = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    *p = truth;
    return true;
}

// PyFloat_AsDouble(), whatever goes wrong with which is put the one way. Nothing if it raised.
std::optional<double> toFloatArgument(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toDouble(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::BaseException))
            raiseStructError(globalObject, scope, "required argument is not a float"_s);
        return std::nullopt;
    }
    return x;
}

template<unsigned size, bool isLittleEndian>
bool packFloat(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition&)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto x = toFloatArgument(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    std::span<uint8_t, size> bytes(p, size);
    if constexpr (size == 2)
        RELEASE_AND_RETURN(scope, packFloat2(globalObject, *x, bytes, isLittleEndian));
    else if constexpr (size == 4)
        RELEASE_AND_RETURN(scope, packFloat4(globalObject, *x, bytes, isLittleEndian));
    else {
        packFloat8(*x, bytes, isLittleEndian);
        return true;
    }
}

template<unsigned size, bool isLittleEndian>
double unpackDouble(const uint8_t* p)
{
    std::span<const uint8_t, size> bytes(p, size);
    if constexpr (size == 2)
        return unpackFloat2(bytes, isLittleEndian);
    else if constexpr (size == 4)
        return unpackFloat4(bytes, isLittleEndian);
    else
        return unpackFloat8(bytes, isLittleEndian);
}

template<unsigned size, bool isLittleEndian>
JSValue unpackFloat(JSGlobalObject*, const uint8_t* p, const FormatDefinition&) { return floatFromDouble(unpackDouble<size, isLittleEndian>(p)); }

// nu_float(): as C makes a double of a float, which is not quite as PyFloat_Unpack4() does. A signalling NaN does not stay one.
double nativeFloat(const uint8_t* p)
{
    float x;
    memcpy(&x, p, sizeof(x));
    return static_cast<double>(x);
}

JSValue unpackNativeFloat(JSGlobalObject*, const uint8_t* p, const FormatDefinition&) { return floatFromDouble(nativeFloat(p)); }

std::optional<std::pair<double, double>> toComplexArgument(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto parts = toComplexParts(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::BaseException))
            raiseStructError(globalObject, scope, "required argument is not a complex"_s);
        return std::nullopt;
    }
    return parts;
}

// bp_float_complex() and bp_double_complex()
template<unsigned size, bool isLittleEndian>
bool packComplex(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition&)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto parts = toComplexArgument(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if constexpr (size == 4) {
        packFloat4(globalObject, parts->first, std::span<uint8_t, 4>(p, 4), isLittleEndian);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, packFloat4(globalObject, parts->second, std::span<uint8_t, 4>(p + 4, 4), isLittleEndian));
    } else {
        packFloat8(parts->first, std::span<uint8_t, 8>(p, 8), isLittleEndian);
        packFloat8(parts->second, std::span<uint8_t, 8>(p + 8, 8), isLittleEndian);
        return true;
    }
}

template<unsigned size, bool isLittleEndian>
JSValue unpackComplex(JSGlobalObject* globalObject, const uint8_t* p, const FormatDefinition&)
{
    return PyComplex::create(globalObject, unpackDouble<size, isLittleEndian>(p), unpackDouble<size, isLittleEndian>(p + size));
}

// np_float_complex(): as C makes a float of a double, so that what there is no room for is an infinity and nothing is said.
bool packNativeFloatComplex(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition&)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto parts = toComplexArgument(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    float x[2] = { static_cast<float>(parts->first), static_cast<float>(parts->second) };
    memcpy(p, x, sizeof(x));
    return true;
}

JSValue unpackNativeFloatComplex(JSGlobalObject* globalObject, const uint8_t* p, const FormatDefinition&) { return PyComplex::create(globalObject, nativeFloat(p), nativeFloat(p + 4)); }

// np_void_p(), with PyLong_AsVoidPtr()
bool packPointer(JSGlobalObject* globalObject, uint8_t* p, JSValue value, const FormatDefinition&)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toPyLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    uint64_t bits = lowBitsOfInt(integer);
    bool isNegative = compareInts(integer, jsNumber(0)) < 0;
    if (isNegative ? !tryInt64(integer) : !!compareInts(integer, intFromUInt64(globalObject, bits))) {
        raise(globalObject, scope, BuiltinType::OverflowError, isNegative ? "Python int too large to convert to C long"_s : "Python int too large to convert to C unsigned long"_s);
        return false;
    }
    store(p, bits, sizeof(void*), true);
    return true;
}

template<typename T> constexpr ptrdiff_t alignmentOf = alignof(T);

constexpr FormatDefinition nativeTable[] = {
    { 'x', sizeof(char), 0, nullptr, nullptr },
    { 'b', sizeof(char), 0, unpackInteger<true, true>, packInteger<true, true> },
    { 'B', sizeof(char), 0, unpackInteger<false, true>, packInteger<false, true> },
    { 'c', sizeof(char), 0, unpackChar, packChar },
    { 's', sizeof(char), 0, nullptr, nullptr },
    { 'p', sizeof(char), 0, nullptr, nullptr },
    { 'h', sizeof(short), alignmentOf<short>, unpackInteger<true, true>, packInteger<true, true> },
    { 'H', sizeof(short), alignmentOf<short>, unpackInteger<false, true>, packInteger<false, true> },
    { 'i', sizeof(int), alignmentOf<int>, unpackInteger<true, true>, packInteger<true, true> },
    { 'I', sizeof(int), alignmentOf<int>, unpackInteger<false, true>, packInteger<false, true> },
    { 'l', sizeof(long), alignmentOf<long>, unpackInteger<true, true>, packInteger<true, true> },
    { 'L', sizeof(long), alignmentOf<long>, unpackInteger<false, true>, packInteger<false, true> },
    { 'n', sizeof(size_t), alignmentOf<size_t>, unpackInteger<true, true>, packInteger<true, true> },
    { 'N', sizeof(size_t), alignmentOf<size_t>, unpackInteger<false, true>, packInteger<false, true> },
    { 'q', sizeof(long long), alignmentOf<long long>, unpackInteger<true, true>, packInteger<true, true> },
    { 'Q', sizeof(long long), alignmentOf<long long>, unpackInteger<false, true>, packInteger<false, true> },
    { '?', sizeof(bool), alignmentOf<bool>, unpackBool, packBool },
    { 'e', sizeof(short), alignmentOf<short>, unpackFloat<2, true>, packFloat<2, true> },
    { 'f', sizeof(float), alignmentOf<float>, unpackNativeFloat, packFloat<4, true> },
    { 'd', sizeof(double), alignmentOf<double>, unpackFloat<8, true>, packFloat<8, true> },
    { 'F', 2 * sizeof(float), alignmentOf<float>, unpackNativeFloatComplex, packNativeFloatComplex },
    { 'D', 2 * sizeof(double), alignmentOf<double>, unpackComplex<8, true>, packComplex<8, true> },
    { 'P', sizeof(void*), alignmentOf<void*>, unpackInteger<false, true>, packPointer },
    { 0, 0, 0, nullptr, nullptr },
};

constexpr FormatDefinition bigEndianTable[] = {
    { 'x', 1, 0, nullptr, nullptr },
    { 'b', 1, 0, unpackInteger<true, false>, packInteger<true, false> },
    { 'B', 1, 0, unpackInteger<false, false>, packInteger<false, false> },
    { 'c', 1, 0, unpackChar, packChar },
    { 's', 1, 0, nullptr, nullptr },
    { 'p', 1, 0, nullptr, nullptr },
    { 'h', 2, 0, unpackInteger<true, false>, packInteger<true, false> },
    { 'H', 2, 0, unpackInteger<false, false>, packInteger<false, false> },
    { 'i', 4, 0, unpackInteger<true, false>, packInteger<true, false> },
    { 'I', 4, 0, unpackInteger<false, false>, packInteger<false, false> },
    { 'l', 4, 0, unpackInteger<true, false>, packInteger<true, false> },
    { 'L', 4, 0, unpackInteger<false, false>, packInteger<false, false> },
    { 'q', 8, 0, unpackInteger<true, false>, packInteger<true, false> },
    { 'Q', 8, 0, unpackInteger<false, false>, packInteger<false, false> },
    { '?', 1, 0, unpackBool, packBool },
    { 'e', 2, 0, unpackFloat<2, false>, packFloat<2, false> },
    { 'f', 4, 0, unpackFloat<4, false>, packFloat<4, false> },
    { 'd', 8, 0, unpackFloat<8, false>, packFloat<8, false> },
    { 'F', 8, 0, unpackComplex<4, false>, packComplex<4, false> },
    { 'D', 16, 0, unpackComplex<8, false>, packComplex<8, false> },
    { 0, 0, 0, nullptr, nullptr },
};

// As init_endian_tables() leaves it where the least byte comes first: what is the same size as it is natively is done as it is natively, all but 'f', 'd' and '?'. It shows for 'F', which then says nothing of what there is no
// room for.
constexpr FormatDefinition littleEndianTable[] = {
    { 'x', 1, 0, nullptr, nullptr },
    { 'b', 1, 0, unpackInteger<true, true>, packInteger<true, true> },
    { 'B', 1, 0, unpackInteger<false, true>, packInteger<false, true> },
    { 'c', 1, 0, unpackChar, packChar },
    { 's', 1, 0, nullptr, nullptr },
    { 'p', 1, 0, nullptr, nullptr },
    { 'h', 2, 0, unpackInteger<true, true>, packInteger<true, true> },
    { 'H', 2, 0, unpackInteger<false, true>, packInteger<false, true> },
    { 'i', 4, 0, unpackInteger<true, true>, packInteger<true, true> },
    { 'I', 4, 0, unpackInteger<false, true>, packInteger<false, true> },
    { 'l', 4, 0, unpackInteger<true, true>, packInteger<true, true> },
    { 'L', 4, 0, unpackInteger<false, true>, packInteger<false, true> },
    { 'q', 8, 0, unpackInteger<true, true>, packInteger<true, true> },
    { 'Q', 8, 0, unpackInteger<false, true>, packInteger<false, true> },
    { '?', 1, 0, unpackBool, packBool },
    { 'e', 2, 0, unpackFloat<2, true>, packFloat<2, true> },
    { 'f', 4, 0, unpackFloat<4, true>, packFloat<4, true> },
    { 'd', 8, 0, unpackFloat<8, true>, packFloat<8, true> },
    { 'F', 8, 0, unpackNativeFloatComplex, packNativeFloatComplex },
    { 'D', 16, 0, unpackComplex<8, true>, packComplex<8, true> },
    { 0, 0, 0, nullptr, nullptr },
};

// ---- Struct

struct FormatCode {
    const FormatDefinition* definition;
    ptrdiff_t offset;
    ptrdiff_t size;
    ptrdiff_t repeat;
};

struct StructState final : NativeState {
    PYTHON_NATIVE_STATE(StructState);
    ptrdiff_t size { -1 };
    ptrdiff_t length { -1 };
    bool isReady { false }; // s_codes is not null
    Vector<FormatCode> codes;
    WriteBarrier<Unknown> format; // bytes, or None until there is one
};

template<typename Visitor> void StructState::visit(Visitor& visitor) { visitor.append(format); }

// ENSURE_STRUCT_IS_READY()
bool ensureIsReady(JSGlobalObject* globalObject, ThrowScope& scope, const StructState& self)
{
    if (self.isReady)
        return true;
    raise(globalObject, scope, BuiltinType::RuntimeError, "Struct object is not initialized"_s);
    return false;
}

// align(). Nothing if there is no room.
std::optional<ptrdiff_t> align(ptrdiff_t size, const FormatDefinition& e)
{
    if (e.alignment && size > 0) {
        ptrdiff_t extra = (e.alignment - 1) - (size - 1) % e.alignment;
        if (extra > std::numeric_limits<ptrdiff_t>::max() - size)
            return std::nullopt;
        size += extra;
    }
    return size;
}

// prepare_s(). Nothing of the Struct is changed unless all of the format will do.
void prepare(JSGlobalObject* globalObject, JSCell* owner, StructState& self, JSValue format)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr ptrdiff_t most = std::numeric_limits<ptrdiff_t>::max();
    Vector<char, 32> text;
    {
        Buffer buffer(format);
        text.append(byteCast<char>(buffer.span()));
    }
    if (text.contains('\0')) {
        raiseStructError(globalObject, scope, "embedded null character"_s);
        return;
    }
    text.append('\0');
    auto overflow = [&] { raiseStructError(globalObject, scope, "total struct size too long"_s); };

    // whichtable()
    const char* s = text.span().data();
    const FormatDefinition* table = nativeTable;
    switch (*s) {
    case '<':
    case '=': // In the order that it is natively, and not lined up as it is natively
        table = littleEndianTable;
        ++s;
        break;
    case '>':
    case '!':
        table = bigEndianTable;
        ++s;
        break;
    case '@':
        ++s;
        break;
    }

    ptrdiff_t size = 0;
    ptrdiff_t length = 0;
    Vector<FormatCode> codes;
    char c;
    while ((c = *s++)) {
        // Py_ISSPACE()
        if (c == ' ' || (c >= '\t' && c <= '\r'))
            continue;
        ptrdiff_t count = 1;
        if (isASCIIDigit(c)) {
            count = c - '0';
            while (isASCIIDigit(c = *s++)) {
                if (count >= most / 10 && (count > most / 10 || (c - '0') > most % 10))
                    return overflow();
                count = count * 10 + (c - '0');
            }
            if (!c) {
                raiseStructError(globalObject, scope, "repeat count given without format specifier"_s);
                return;
            }
        }
        // getentry()
        const FormatDefinition* e = table;
        while (e->format && e->format != c)
            ++e;
        if (!e->format) {
            raiseStructError(globalObject, scope, "bad char in struct format"_s);
            return;
        }
        if (c == 's' || c == 'p') {
            if (length == most)
                return overflow();
            ++length;
        } else if (c != 'x') {
            if (count > most - length)
                return overflow();
            length += count;
        }
        auto aligned = align(size, *e);
        if (!aligned)
            return overflow();
        size = *aligned;
        if (count > (most - size) / e->size)
            return overflow();
        if (c == 's' || c == 'p')
            codes.append({ e, size, count, 1 });
        else if (c != 'x' && count)
            codes.append({ e, size, e->size, count });
        size += count * e->size;
    }
    self.codes = WTF::move(codes);
    self.isReady = true;
    self.size = size;
    self.length = length;
    self.format.set(vm, owner, format);
}

// s_unpack_internal(). Nothing of a program's is run meanwhile, so the bytes stay where they are.
JSValue unpackFrom(JSGlobalObject* globalObject, const StructState& self, const uint8_t* start)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (self.length > static_cast<ptrdiff_t>(std::numeric_limits<int32_t>::max()))
        return raiseMemoryError(globalObject, scope);
    MarkedArgumentBuffer values;
    for (auto& code : self.codes) {
        const FormatDefinition& e = *code.definition;
        const uint8_t* p = start + code.offset;
        for (ptrdiff_t j = code.repeat; j--; p += code.size) {
            JSValue value;
            if (e.format == 's')
                value = newBytes(globalObject, std::span(p, static_cast<size_t>(code.size)));
            else if (e.format == 'p') {
                // The first byte says how many of the rest count.
                ptrdiff_t n = code.size ? std::min<ptrdiff_t>(*p, code.size - 1) : 0;
                value = newBytes(globalObject, std::span(p + 1, static_cast<size_t>(n)));
            } else
                value = e.unpack(globalObject, p, e);
            RETURN_IF_EXCEPTION(scope, { });
            values.append(value);
        }
    }
    if (values.hasOverflowed())
        return raiseMemoryError(globalObject, scope);
    return PyTuple::createFromArguments(globalObject, values);
}

// The bytes are copied out first. It costs little, and then it does not matter what making the values does.
JSValue unpackFrom(JSGlobalObject* globalObject, const StructState& self, std::span<const uint8_t> bytes)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ASSERT(bytes.size() == static_cast<size_t>(self.size));
    Vector<uint8_t, 64> copy;
    if (!copy.tryAppend(bytes))
        return raiseMemoryError(globalObject, scope);
    // 'p' with nothing to it looks one past where it begins, and takes nothing from there.
    copy.append(0);
    RELEASE_AND_RETURN(scope, unpackFrom(globalObject, self, copy.span().data()));
}

// s_pack_internal(), into bytes of its own: turning what is given into numbers can run anything, and so can move what the bytes are wanted for. `packed` is as far as it got, if it raised.
void packInto(JSGlobalObject* globalObject, const StructState& self, const NativeArguments& args, unsigned first, Vector<uint8_t, 64>& packed)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyRealm* realm = globalObject->pyRealm();
    if (!packed.tryGrow(static_cast<size_t>(self.size) + 1)) {
        raiseMemoryError(globalObject, scope);
        return;
    }
    packed.fill(0);
    unsigned i = first;
    for (auto& code : self.codes) {
        const FormatDefinition& e = *code.definition;
        uint8_t* p = packed.mutableSpan().data() + code.offset;
        for (ptrdiff_t j = code.repeat; j--; p += code.size) {
            JSValue value = args[i++];
            if (e.format == 's' || e.format == 'p') {
                if (!isInstance(globalObject, value, realm->typeBytes()) && !isInstance(globalObject, value, realm->typeByteArray())) {
                    raiseStructError(globalObject, scope, concatenate("argument for '"_s, e.format, "' must be a bytes object"_s));
                    return;
                }
                Buffer buffer(value);
                auto bytes = buffer.span();
                ptrdiff_t n = static_cast<ptrdiff_t>(bytes.size());
                if (e.format == 's') {
                    n = std::min(n, code.size);
                    if (n > 0)
                        memcpy(p, bytes.data(), static_cast<size_t>(n));
                    continue;
                }
                n = code.size ? std::min(n, code.size - 1) : 0;
                if (n > 0)
                    memcpy(p + 1, bytes.data(), static_cast<size_t>(n));
                // With nothing to it there is nowhere to say how many, and CPython says so all the same, in the byte that comes next.
                *p = static_cast<uint8_t>(std::min<ptrdiff_t>(n, 255));
                continue;
            }
            e.pack(globalObject, p, value, e);
            if (scope.exception()) [[unlikely]] {
                if (classify(value).isInt() && catchException(globalObject, BuiltinType::OverflowError))
                    raiseStructError(globalObject, scope, "int too large to convert"_s);
                return;
            }
        }
    }
}

// s_pack()
JSValue pack(JSGlobalObject* globalObject, const StructState& self, const NativeArguments& args, unsigned first)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!ensureIsReady(globalObject, scope, self))
        return { };
    ptrdiff_t count = static_cast<ptrdiff_t>(args.size() - first);
    if (count != self.length)
        return raiseStructError(globalObject, scope, concatenate("pack expected "_s, static_cast<int64_t>(self.length), " items for packing (got "_s, static_cast<int64_t>(count), ')'));
    Vector<uint8_t, 64> packed;
    packInto(globalObject, self, args, first, packed);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newBytes(globalObject, packed.span().first(static_cast<size_t>(self.size))));
}

// s_pack_into()
JSValue packIntoBuffer(JSGlobalObject* globalObject, const StructState& self, const NativeArguments& args, unsigned first)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!ensureIsReady(globalObject, scope, self))
        return { };
    ptrdiff_t count = static_cast<ptrdiff_t>(args.size() - first);
    if (count != self.length + 2) {
        if (!count)
            return raiseStructError(globalObject, scope, "pack_into expected buffer argument"_s);
        if (count == 1)
            return raiseStructError(globalObject, scope, "pack_into expected offset argument"_s);
        return raiseStructError(globalObject, scope, concatenate("pack_into expected "_s, static_cast<int64_t>(self.length), " items for packing (got "_s, static_cast<int64_t>(count - 2), ')'));
    }
    // "w*"
    Buffer buffer = tryBufferOf(globalObject, args[first], WritableBuffer);
    if (scope.exception() && !scope.tryClearException())
        return { };
    if (!buffer)
        return raiseTypeError(globalObject, scope, concatenate("argument must be read-write bytes-like object, not "_s, typeNameOfArgument(globalObject, args[first])));
    auto given = toIndex(globalObject, args[first + 1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t offset = *given;
    int64_t size = self.size;
    int64_t available = static_cast<int64_t>(buffer.size());
    if (offset < 0) {
        // It has to be far enough from the end for what is to go there, and not further than the beginning.
        if (offset + size > 0)
            return raiseStructError(globalObject, scope, concatenate("no space to pack "_s, size, " bytes at offset "_s, offset));
        if (offset + available < 0)
            return raiseStructError(globalObject, scope, concatenate("offset "_s, offset, " out of range for "_s, available, "-byte buffer"_s));
        offset += available;
    }
    if (available - offset < size)
        return raiseStructError(globalObject, scope, concatenate("pack_into requires a buffer of at least "_s, static_cast<uint64_t>(size) + static_cast<uint64_t>(offset), " bytes for packing "_s, size, " bytes at offset "_s, offset, " (actual buffer size is "_s, available, ')'));

    Vector<uint8_t, 64> packed;
    packInto(globalObject, self, args, first + 2, packed);
    // What was got through before anything went wrong is written, as it is in CPython, which writes as it goes. CPython keeps the buffer as it is meanwhile. Here it may have been made shorter, and then nothing is written.
    Exception* raised = scope.exception() ? takeRaisedException(globalObject->vm()) : nullptr;
    auto target = mutableSpanOf(buffer);
    bool isStillThere = static_cast<int64_t>(target.size()) - offset >= size && packed.size() > static_cast<size_t>(size);
    if (isStillThere && size)
        memcpy(target.data() + offset, packed.span().data(), static_cast<size_t>(size));
    if (raised) {
        restoreRaisedException(globalObject, raised);
        return { };
    }
    if (!isStillThere)
        return raise(globalObject, scope, BuiltinType::BufferError, "the buffer was made shorter while it was being packed into"_s);
    return jsUndefined();
}

JSValue unpack(JSGlobalObject* globalObject, const StructState& self, JSValue source)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Buffer buffer = bufferOf(globalObject, source);
    RETURN_IF_EXCEPTION(scope, { });
    if (!ensureIsReady(globalObject, scope, self))
        return { };
    if (static_cast<int64_t>(buffer.size()) != self.size)
        return raiseStructError(globalObject, scope, concatenate("unpack requires a buffer of "_s, static_cast<int64_t>(self.size), " bytes"_s));
    RELEASE_AND_RETURN(scope, unpackFrom(globalObject, self, buffer.span()));
}

JSValue unpackFromOffset(JSGlobalObject* globalObject, const StructState& self, JSValue source, JSValue givenOffset)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Buffer buffer = bufferOf(globalObject, source);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t offset = 0;
    if (givenOffset) {
        auto converted = toSsize(globalObject, givenOffset);
        RETURN_IF_EXCEPTION(scope, { });
        offset = *converted;
    }
    if (!ensureIsReady(globalObject, scope, self))
        return { };
    int64_t size = self.size;
    int64_t available = static_cast<int64_t>(buffer.size());
    if (offset < 0) {
        if (offset + size > 0)
            return raiseStructError(globalObject, scope, concatenate("not enough data to unpack "_s, size, " bytes at offset "_s, offset));
        if (offset + available < 0)
            return raiseStructError(globalObject, scope, concatenate("offset "_s, offset, " out of range for "_s, available, "-byte buffer"_s));
        offset += available;
    }
    if (available - offset < size)
        return raiseStructError(globalObject, scope, concatenate("unpack_from requires a buffer of at least "_s, static_cast<uint64_t>(size) + static_cast<uint64_t>(offset), " bytes for unpacking "_s, size, " bytes at offset "_s, offset, " (actual buffer size is "_s, available, ')'));
    RELEASE_AND_RETURN(scope, unpackFrom(globalObject, self, buffer.span().subspan(static_cast<size_t>(offset), static_cast<size_t>(size))));
}

// ---- What goes through a buffer, a Struct at a time

struct UnpackIteratorState final : NativeState {
    PYTHON_NATIVE_STATE(UnpackIteratorState);
    WriteBarrier<Unknown> structObject; // Empty when there is no more
    WriteBarrier<Unknown> source;
    int64_t length { 0 }; // How many bytes there were to go through when it began
    int64_t index { 0 };
};

template<typename Visitor>
void UnpackIteratorState::visit(Visitor& visitor)
{
    visitor.append(structObject);
    visitor.append(source);
}

JSValue iterateUnpacking(JSGlobalObject* globalObject, JSValue structObject, JSValue source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& self = stateOf<StructState>(structObject);
    if (!ensureIsReady(globalObject, scope, self))
        return { };
    if (!self.size)
        return raiseStructError(globalObject, scope, "cannot iteratively unpack with a struct of length 0"_s);
    Buffer buffer = bufferOf(globalObject, source);
    RETURN_IF_EXCEPTION(scope, { });
    if (buffer.size() % static_cast<size_t>(self.size))
        return raiseStructError(globalObject, scope, concatenate("iterative unpacking requires a buffer of a multiple of "_s, static_cast<int64_t>(self.size), " bytes"_s));
    auto* iterator = PyStateObject::create(vm, structModuleState(globalObject).unpackIterator->instanceStructure(), makeUnique<UnpackIteratorState>());
    auto& state = iterator->state<UnpackIteratorState>();
    state.structObject.set(vm, iterator, structObject);
    state.source.set(vm, iterator, buffer.object());
    state.length = static_cast<int64_t>(buffer.size());
    return iterator;
}

// cache_struct_converter(): the Struct for a format. Empty if it raised.
JSValue structFor(JSGlobalObject* globalObject, JSValue format)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = structModuleState(globalObject);
    // It is kept under a str, so that a str is never compared with bytes.
    JSValue key = format;
    if (isInstance(globalObject, format, globalObject->pyRealm()->typeBytes())) {
        Buffer buffer(format);
        String text = decodeBytes(globalObject, buffer.span(), "ascii"_s, "surrogateescape"_s);
        RETURN_IF_EXCEPTION(scope, { });
        key = strOrMemoryError(globalObject, text);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue cached = state.cache->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (cached)
        return cached;
    JSValue made = call(globalObject, state.structType.get(), key);
    RETURN_IF_EXCEPTION(scope, { });
    // MAXCACHE
    if (state.cache->size() >= 100)
        state.cache->clear(globalObject);
    state.cache->set(globalObject, key, made);
    if (scope.exception() && !scope.tryClearException())
        return { };
    return made;
}

} // anonymous namespace

// s_new()
PYTHON_NATIVE(structNew)
{
    VM& vm = globalObject->vm();
    auto* object = PyStateObject::create(vm, asType(callFrame->uncheckedArgument(0))->instanceStructure(), makeUnique<StructState>());
    object->state<StructState>().format.set(vm, object, jsUndefined());
    return JSValue::encode(object);
}

// Struct(format)
PYTHON_NATIVE(structInit)
{
    NATIVE_PROLOGUE();
    JSValue format = args.at(1);
    if (stringIn(format)) {
        // PyUnicode_AsASCIIString()
        format = encodeStringToObject(globalObject, format, "ascii"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isInstance(globalObject, format, realm->typeBytes()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Struct() argument 1 must be a str or bytes object, not "_s, typeOf(globalObject, format)->nameWithoutModule(globalObject))));
    prepare(globalObject, args[0].asCell(), stateOf<StructState>(args[0]), format);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// unpack(buffer, /)
PYTHON_NATIVE(structUnpack)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(unpack(globalObject, stateOf<StructState>(args[0]), args[1])));
}

// unpack_from(buffer, offset=0)
PYTHON_NATIVE(structUnpackFrom)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(unpackFromOffset(globalObject, stateOf<StructState>(args[0]), args.at(1), args.at(2))));
}

// iter_unpack(buffer, /)
PYTHON_NATIVE(structIterUnpack)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(iterateUnpacking(globalObject, args[0], args[1])));
}

// pack(v1, v2, ...)
PYTHON_NATIVE(structPack)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "Struct.pack"_s))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(pack(globalObject, stateOf<StructState>(args[0]), args, 1)));
}

// pack_into(buffer, offset, v1, v2, ...)
PYTHON_NATIVE(structPackInto)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "Struct.pack_into"_s))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(packIntoBuffer(globalObject, stateOf<StructState>(args[0]), args, 1)));
}

PYTHON_NATIVE(structSizeOf)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<StructState>(args[0]);
    if (!ensureIsReady(globalObject, scope, self))
        return { };
    // sizeof(formatcode) in CPython, for each of them and for what comes after the last
    constexpr int64_t sizeOfFormatCode = 32;
    return JSValue::encode(intFromInt64(globalObject, typeOf(globalObject, args[0])->basicSize() + sizeOfFormatCode * (static_cast<int64_t>(self.codes.size()) + 1)));
}

static JSValue formatOfStruct(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = stateOf<StructState>(self);
    if (!ensureIsReady(globalObject, scope, state))
        return { };
    // PyUnicode_FromStringAndSize(), which takes it for UTF-8
    Buffer buffer(state.format.get());
    String text = decodeBytes(globalObject, buffer.span(), "utf-8"_s, "strict"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, text));
}

PYTHON_NATIVE(structRepr)
{
    NATIVE_PROLOGUE();
    JSValue format = formatOfStruct(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    String shown = repr(globalObject, format);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(typeOf(globalObject, args[0])->nameWithoutModule(globalObject), '(', shown, ')'))));
}

PYTHON_NATIVE(unpackIteratorSelf)
{
    UNUSED_PARAM(globalObject);
    return JSValue::encode(callFrame->uncheckedArgument(0));
}

PYTHON_NATIVE(unpackIteratorLengthHint)
{
    auto& self = stateOf<UnpackIteratorState>(callFrame->uncheckedArgument(0));
    if (!self.structObject)
        return JSValue::encode(jsNumber(0));
    return JSValue::encode(intFromInt64(globalObject, (self.length - self.index) / stateOf<StructState>(self.structObject.get()).size));
}

PYTHON_NATIVE(unpackIteratorNext)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<UnpackIteratorState>(args[0]);
    auto stop = [&] { return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue())); };
    if (!self.structObject)
        return stop();
    auto finish = [&] {
        self.structObject.clear();
        self.source.clear();
    };
    if (self.index >= self.length) {
        finish();
        return stop();
    }
    auto& structState = stateOf<StructState>(self.structObject.get());
    // CPython keeps the buffer as it is until this is done with. Here it may have been made shorter, and then that is the end of it.
    Buffer buffer(self.source.get());
    auto bytes = buffer.span();
    if (static_cast<int64_t>(bytes.size()) - self.index < structState.size) {
        finish();
        return JSValue::encode(raise(globalObject, scope, BuiltinType::BufferError, "the buffer was made shorter while it was being unpacked"_s));
    }
    JSValue result = unpackFrom(globalObject, structState, bytes.subspan(static_cast<size_t>(self.index), static_cast<size_t>(structState.size)));
    RETURN_IF_EXCEPTION(scope, { });
    self.index += structState.size;
    return JSValue::encode(result);
}

// ---- The functions of the module

PYTHON_NATIVE(structClearCache)
{
    UNUSED_PARAM(callFrame);
    structModuleState(globalObject).cache->clear(globalObject);
    RETURN_NONE();
}

// calcsize(format, /)
PYTHON_NATIVE(structCalcSize)
{
    NATIVE_PROLOGUE();
    JSValue object = structFor(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, stateOf<StructState>(object).size));
}

// pack(format, v1, v2, ...) and pack_into(format, buffer, offset, v1, v2, ...)
PYTHON_NATIVE(structModulePack)
{
    bool isInto = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, isInto ? "_struct.pack_into"_s : "_struct.pack"_s))
        return { };
    if (!args.size())
        return JSValue::encode(raiseTypeError(globalObject, scope, "missing format argument"_s));
    JSValue object = structFor(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = stateOf<StructState>(object);
    RELEASE_AND_RETURN(scope, JSValue::encode(isInto ? packIntoBuffer(globalObject, self, args, 1) : pack(globalObject, self, args, 1)));
}

// unpack(format, buffer, /)
PYTHON_NATIVE(structModuleUnpack)
{
    NATIVE_PROLOGUE();
    JSValue object = structFor(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(unpack(globalObject, stateOf<StructState>(object), args[1])));
}

// unpack_from(format, /, buffer, offset=0)
PYTHON_NATIVE(structModuleUnpackFrom)
{
    NATIVE_PROLOGUE();
    JSValue object = structFor(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(unpackFromOffset(globalObject, stateOf<StructState>(object), args.at(1), args.at(2))));
}

// iter_unpack(format, buffer, /)
PYTHON_NATIVE(structModuleIterUnpack)
{
    NATIVE_PROLOGUE();
    JSValue object = structFor(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(iterateUnpacking(globalObject, object, args[1])));
}

JSObject* createStructModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = structModuleState(globalObject);
    using Kind = PyNativeFunction::Kind;
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    constexpr auto any = "($self, /, *args)"_s;
    if (!state.structType) {
        state.cache.set(vm, realm, PyDict::create(globalObject));
        PyType* type = createBuiltinType(globalObject, "_struct.Struct"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType | PyType::HasWeakReferences);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.structType.set(vm, realm, type);
        addGenericGetAttribute(globalObject, type);
        addGenericSetAttribute(globalObject, type);
        addMethods(globalObject, type, {
            { "__new__"_s, structNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, notChecked },
            { "__init__"_s, structInit, Kind::Wrapper, 0, "(format)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "__repr__"_s, structRepr },
            { "iter_unpack"_s, structIterUnpack },
            { "pack"_s, structPack, Kind::Method, 0, any, notChecked },
            { "pack_into"_s, structPackInto, Kind::Method, 0, any, notChecked },
            { "unpack"_s, structUnpack },
            { "unpack_from"_s, structUnpackFrom },
            { "__sizeof__"_s, structSizeOf },
        });
        addGetSet(globalObject, type, "format"_s, formatOfStruct);
        addGetSet(globalObject, type, "size"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return intFromInt64(globalObject, stateOf<StructState>(self).size); });

        PyType* iterator = createBuiltinType(globalObject, "_struct.unpack_iterator"_s, realm->typeObject(), PyType::Layout::Native, 0);
        iterator->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, iterator));
        state.unpackIterator.set(vm, realm, iterator);
        addGenericGetAttribute(globalObject, iterator);
        addMethods(globalObject, iterator, {
            { "__iter__"_s, unpackIteratorSelf },
            { "__next__"_s, unpackIteratorNext },
            { "__length_hint__"_s, unpackIteratorLengthHint, Kind::Method, 0, "($self, /)"_s },
        });

        // PyErr_NewException("struct.error", NULL, NULL)
        PyDict* contents = PyDict::create(globalObject);
        contents->setString(globalObject, "__module__"_s, jsNontrivialString(vm, "struct"_s));
        JSValue error = newType(globalObject, realm->typeType(), jsNontrivialString(vm, "error"_s), PyTuple::create(globalObject, { realm->type(BuiltinType::Exception) }), contents, nullptr);
        RETURN_IF_EXCEPTION(scope, nullptr);
        state.error.set(vm, realm, asType(error));
    }
    JSObject* module = newBuiltinModule(globalObject, "_struct"_s);
    constexpr auto anyOfModule = "($module, /, *args)"_s;
    addFunction(globalObject, module, "_clearcache"_s, structClearCache);
    addFunction(globalObject, module, "calcsize"_s, structCalcSize);
    addFunction(globalObject, module, "iter_unpack"_s, structModuleIterUnpack);
    addFunction(globalObject, module, "pack"_s, structModulePack, pack(false), anyOfModule, notChecked);
    addFunction(globalObject, module, "pack_into"_s, structModulePack, pack(true), anyOfModule, notChecked);
    addFunction(globalObject, module, "unpack"_s, structModuleUnpack);
    addFunction(globalObject, module, "unpack_from"_s, structModuleUnpackFrom);
    module->putDirect(vm, Identifier::fromString(vm, "Struct"_s), state.structType.get());
    module->putDirect(vm, Identifier::fromString(vm, "error"_s), state.error.get());
    return module;
}

} } // namespace JSC::Python
