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

#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonNumbers.h"
#include <wtf/Scope.h>

// marshal: Python/marshal.c of CPython.
//
// What is written is what CPython writes and can read, and what CPython writes can be read here, but for code. That is written in the same form, and what is in it is this engine's: see PythonCode.cpp for what
// co_code is.
//
// From version 3 on, what is come upon twice is written once. CPython marks what may be come upon again, which is what has more than one reference to it. There is no telling that here, so everything is marked
// that could be. It is read the same.

namespace JSC { namespace Python {

namespace {

constexpr int currentVersion = 5; // Py_MARSHAL_VERSION
constexpr int maximumDepth = 2000; // MAX_MARSHAL_STACK_DEPTH
constexpr int64_t largestSize = 0x7FFFFFFF; // SIZE32_MAX

enum Type : uint8_t {
    TypeNull = '0',
    TypeNone = 'N',
    TypeFalse = 'F',
    TypeTrue = 'T',
    TypeStopIteration = 'S',
    TypeEllipsis = '.',
    TypeBinaryFloat = 'g',
    TypeBinaryComplex = 'y',
    TypeLong = 'l',
    TypeBytes = 's',
    TypeTuple = '(',
    TypeList = '[',
    TypeDict = '{',
    TypeCode = 'c',
    TypeUnicode = 'u',
    TypeUnknown = '?',
    TypeSet = '<',
    TypeFrozenSet = '>',
    TypeSlice = ':',
    TypeInterned = 't',
    TypeASCII = 'a',
    TypeASCIIInterned = 'A',
    TypeShortASCII = 'z',
    TypeShortASCIIInterned = 'Z',
    TypeInt = 'i',
    TypeSmallTuple = ')',
    TypeComplex = 'x',
    TypeFloat = 'f',
    TypeInt64 = 'I',
    TypeReference = 'r',
};
constexpr uint8_t flagReference = 0x80;

// An int is written in digits of this many bits.
constexpr unsigned digitShift = 15;
constexpr unsigned digitMask = (1 << digitShift) - 1;

// The kinds of variable that co_localspluskinds tells apart: CO_FAST_LOCAL and so on, of CPython's Include/internal/pycore_code.h.
constexpr uint8_t kindLocal = 0x20;
constexpr uint8_t kindCell = 0x40;
constexpr uint8_t kindFree = 0x80;

// ---- Writing

class Writer {
public:
    enum class Error : uint8_t { None, Unmarshallable, NestedTooDeep, NoMemory, CodeNotAllowed };

    Writer(JSGlobalObject* globalObject, int version, bool allowsCode)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_realm(globalObject->pyRealm())
        , m_version(version)
        , m_allowsCode(allowsCode)
    {
    }

    void writeObject(JSValue);
    Error error() const { return m_out.hasOverflowed() && m_error == Error::None ? Error::NoMemory : m_error; }
    const ByteVector& bytes() const { return m_out; }

private:
    void writeByte(uint8_t byte) { m_out.append(byte); }
    void writeShort(unsigned value) { m_out.appendList({ static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8) }); }
    void writeLong(int64_t value) { m_out.appendList({ static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24) }); }
    // W_SIZE()
    bool writeSize(size_t size)
    {
        if (size > static_cast<size_t>(largestSize)) {
            --m_depth;
            m_error = Error::Unmarshallable;
            return false;
        }
        writeLong(static_cast<int64_t>(size));
        return true;
    }
    void writeSized(std::span<const uint8_t> bytes)
    {
        if (writeSize(bytes.size()))
            m_out.append(bytes);
    }
    void writeShortSized(std::span<const uint8_t> bytes)
    {
        writeByte(static_cast<uint8_t>(bytes.size()));
        m_out.append(bytes);
    }
    void writeBinaryFloat(double value)
    {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        for (unsigned i = 0; i < 8; ++i)
            writeByte(static_cast<uint8_t>(bits >> (i * 8)));
    }
    void writeFloatAsText(double);
    void writeBigInt(JSBigInt*, uint8_t flag);
    void writeMagnitude(uint64_t magnitude, bool isNegative);
    void writeString(JSString*, uint8_t flag);
    void writeSet(PySet*, uint8_t flag, bool isFrozen);
    void writeCode(JSValue, uint8_t flag);
    bool writeReference(JSValue, uint8_t& flag);
    void complete(JSValue);
    void writeComplexObject(JSValue, uint8_t flag);
    JSValue attribute(JSValue object, ASCIILiteral name) { return getAttribute(m_globalObject, object, Identifier::fromString(m_vm, name)); }

    static constexpr uint32_t isBeingWritten = 0x80000000;

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    PyRealm* m_realm;
    ByteVector m_out;
    // What has been written, and the how-manyeth it was of what is marked. What is only made to be written is kept until the end, or something else might be made where it was and be taken for it.
    UncheckedKeyHashMap<EncodedJSValue, uint32_t, EncodedJSValueHash, EncodedJSValueHashTraits> m_references;
    MarkedArgumentBuffer m_kept;
    int m_version;
    int m_depth { 0 };
    bool m_allowsCode;
    Error m_error { Error::None };
};

void Writer::writeFloatAsText(double value)
{
    // PyOS_double_to_string(value, 'g', 17, 0, NULL)
    FormatSpecification specification;
    specification.precision = 17;
    specification.type = 'g';
    String text = formatFloat(m_globalObject, value, specification);
    if (text.isNull()) {
        m_error = Error::NoMemory;
        return;
    }
    writeShortSized(text.span8());
}

void Writer::writeMagnitude(uint64_t magnitude, bool isNegative)
{
    int64_t count = 0;
    for (uint64_t rest = magnitude; rest; rest >>= digitShift)
        ++count;
    writeLong(isNegative ? -count : count);
    for (uint64_t rest = magnitude; rest; rest >>= digitShift)
        writeShort(rest & digitMask);
}

// w_PyLong()
void Writer::writeBigInt(JSBigInt* value, uint8_t flag)
{
    writeByte(TypeLong | flag);
    unsigned length = value->length();
    while (length && !value->digit(length - 1))
        --length;
    if (!length) {
        writeLong(0);
        return;
    }
    uint64_t bits = static_cast<uint64_t>(length - 1) * JSBigInt::digitBits + (JSBigInt::digitBits - clz(value->digit(length - 1)));
    uint64_t count = (bits + digitShift - 1) / digitShift;
    if (count > static_cast<uint64_t>(largestSize)) {
        --m_depth;
        m_error = Error::Unmarshallable;
        return;
    }
    writeLong(value->sign() ? -static_cast<int64_t>(count) : static_cast<int64_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t position = i * digitShift;
        unsigned index = static_cast<unsigned>(position / JSBigInt::digitBits);
        unsigned shift = position % JSBigInt::digitBits;
        uint64_t digit = value->digit(index) >> shift;
        if (shift + digitShift > JSBigInt::digitBits && index + 1 < length)
            digit |= value->digit(index + 1) << (JSBigInt::digitBits - shift);
        writeShort(digit & digitMask);
    }
}

void Writer::writeString(JSString* string, uint8_t flag)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    String text = string->value(m_globalObject);
    if (scope.exception()) [[unlikely]] {
        m_error = Error::NoMemory;
        return;
    }
    bool isInterned = m_realm->isInterned(m_globalObject, string);
    if (m_version >= 4 && text.containsOnlyASCII()) {
        Vector<uint8_t> narrowed;
        std::span<const uint8_t> characters;
        if (text.is8Bit())
            characters = text.span8();
        else {
            narrowed = Vector<uint8_t>(text.length(), [&] (size_t i) { return static_cast<uint8_t>(text[i]); });
            characters = narrowed.span();
        }
        if (characters.size() < 256) {
            writeByte((isInterned ? TypeShortASCIIInterned : TypeShortASCII) | flag);
            writeShortSized(characters);
        } else {
            writeByte((isInterned ? TypeASCIIInterned : TypeASCII) | flag);
            writeSized(characters);
        }
        return;
    }
    auto encoded = encodeString(m_globalObject, string, "utf-8"_s, "surrogatepass"_s);
    if (scope.exception() || !encoded || encoded->hasOverflowed()) [[unlikely]] {
        --m_depth;
        m_error = Error::Unmarshallable;
        return;
    }
    writeByte((m_version >= 3 && isInterned ? TypeInterned : TypeUnicode) | flag);
    writeSized(encoded->span());
}

// w_ref(): true if there is no more to be done, because it has been written before and this says which it was, or because something is wrong.
bool Writer::writeReference(JSValue value, uint8_t& flag)
{
    if (m_version < 3)
        return false;
    auto result = m_references.add(JSValue::encode(value), 0);
    if (!result.isNewEntry) {
        uint32_t index = result.iterator->value;
        if (index & isBeingWritten) {
            m_error = Error::Unmarshallable;
            return true;
        }
        writeByte(TypeReference);
        writeLong(index);
        return true;
    }
    uint32_t index = m_references.size() - 1;
    if (index >= 0x7FFFFFFF) {
        m_error = Error::Unmarshallable;
        return true;
    }
    // These are not there to be referred to until all of them has been read.
    if (isCode(m_globalObject, value) || value.inherits<PySlice>())
        index |= isBeingWritten;
    result.iterator->value = index;
    m_kept.append(value);
    flag |= flagReference;
    return false;
}

// w_complete()
void Writer::complete(JSValue value)
{
    if (m_version < 3)
        return;
    auto found = m_references.find(JSValue::encode(value));
    if (found != m_references.end())
        found->value &= ~isBeingWritten;
}

// w_object()
void Writer::writeObject(JSValue value)
{
    if (m_error != Error::None)
        return;
    uint8_t flag = 0;
    ++m_depth;
    if (m_depth > maximumDepth || !m_vm.isSafeToRecurse())
        m_error = Error::NestedTooDeep;
    else if (!value)
        writeByte(TypeNull);
    else if (isNone(value))
        writeByte(TypeNone);
    else if (value == JSValue(m_realm->type(BuiltinType::StopIteration)))
        writeByte(TypeStopIteration);
    else if (value == JSValue(m_realm->ellipsis()))
        writeByte(TypeEllipsis);
    else if (value.isFalse())
        writeByte(TypeFalse);
    else if (value.isTrue())
        writeByte(TypeTrue);
    else if (!writeReference(value, flag))
        writeComplexObject(value, flag);
    --m_depth;
}

static JSValue dumpToBytes(JSGlobalObject*, JSValue, int version, bool allowsCode);

void Writer::writeSet(PySet* set, uint8_t flag, bool isFrozen)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    writeByte((isFrozen ? TypeFrozenSet : TypeSet) | flag);
    if (!writeSize(set->size()))
        return;
    // So that what is written does not depend on how things happen to be hashed, they are in the order of sorted(set, key=marshal.dumps).
    MarkedArgumentBuffer items;
    MarkedArgumentBuffer dumps;
    for (unsigned i = 0; i < set->entryCount(); ++i) {
        if (JSValue item = set->keyAt(i))
            items.append(item);
    }
    for (unsigned i = 0; i < items.size(); ++i) {
        JSValue dump = dumpToBytes(m_globalObject, items.at(i), m_version, m_allowsCode);
        if (scope.exception()) [[unlikely]] {
            m_error = Error::Unmarshallable;
            return;
        }
        dumps.append(dump);
    }
    Vector<unsigned> order(items.size(), [] (size_t i) { return static_cast<unsigned>(i); });
    std::ranges::stable_sort(order, [&] (unsigned a, unsigned b) {
        return std::ranges::lexicographical_compare(uncheckedDowncast<JSUint8Array>(dumps.at(a).asCell())->span(), uncheckedDowncast<JSUint8Array>(dumps.at(b).asCell())->span());
    });
    for (unsigned i : order)
        writeObject(items.at(i));
}

void Writer::writeCode(JSValue code, uint8_t flag)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    auto failed = [&] {
        if (!scope.exception())
            return false;
        m_error = Error::NoMemory;
        return true;
    };
    auto writeLongAttribute = [&] (ASCIILiteral name) {
        JSValue value = attribute(code, name);
        if (!failed())
            writeLong(value.asInt32());
    };
    auto writeAttribute = [&] (ASCIILiteral name) {
        JSValue value = attribute(code, name);
        if (!failed())
            writeObject(value);
    };
    writeByte(TypeCode | flag);
    for (auto name : { "co_argcount"_s, "co_posonlyargcount"_s, "co_kwonlyargcount"_s, "co_stacksize"_s, "co_flags"_s }) {
        writeLongAttribute(name);
        if (m_error != Error::None)
            return;
    }
    for (auto name : { "co_code"_s, "co_consts"_s, "co_names"_s }) {
        writeAttribute(name);
        if (m_error != Error::None)
            return;
    }

    // co_localsplusnames and co_localspluskinds: the variables, then the cells that are not among those, then what is from further out.
    JSValue variables = attribute(code, "co_varnames"_s);
    if (failed())
        return;
    JSValue cells = attribute(code, "co_cellvars"_s);
    if (failed())
        return;
    JSValue free = attribute(code, "co_freevars"_s);
    if (failed())
        return;
    auto textOf = [&] (JSValue name) { return asString(name)->value(m_globalObject).data; };
    auto contains = [&] (PyTuple* tuple, JSValue name) {
        String text = textOf(name);
        for (auto& item : tuple->span()) {
            if (textOf(item.get()) == text)
                return true;
        }
        return false;
    };
    MarkedArgumentBuffer names;
    ByteVector kinds;
    for (auto& name : asTuple(variables)->span()) {
        names.append(name.get());
        kinds.append(kindLocal | (contains(asTuple(cells), name.get()) ? kindCell : 0));
    }
    for (auto& name : asTuple(cells)->span()) {
        if (contains(asTuple(variables), name.get()))
            continue;
        names.append(name.get());
        kinds.append(kindCell);
    }
    for (auto& name : asTuple(free)->span()) {
        names.append(name.get());
        kinds.append(kindFree);
    }
    if (failed())
        return;
    JSValue namesTuple = PyTuple::createFromArguments(m_globalObject, names);
    if (failed())
        return;
    writeObject(namesTuple);
    JSValue kindsBytes = newBytes(m_globalObject, kinds);
    if (failed())
        return;
    writeObject(kindsBytes);

    for (auto name : { "co_filename"_s, "co_name"_s, "co_qualname"_s }) {
        writeAttribute(name);
        if (m_error != Error::None)
            return;
    }
    writeLongAttribute("co_firstlineno"_s);
    for (auto name : { "co_linetable"_s, "co_exceptiontable"_s }) {
        if (m_error != Error::None)
            return;
        writeAttribute(name);
    }
    complete(code);
}

// w_complex_object()
void Writer::writeComplexObject(JSValue value, uint8_t flag)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    PyType* type = typeOf(m_globalObject, value);
    if (type == m_realm->typeInt()) {
        Number number = classify(value);
        if (number.kind == Number::Kind::Small) {
            writeByte(TypeInt | flag);
            writeLong(number.small);
        } else
            writeBigInt(number.big, flag);
        return;
    }
    if (type == m_realm->typeFloat()) {
        double number = classify(value).real;
        if (m_version > 1) {
            writeByte(TypeBinaryFloat | flag);
            writeBinaryFloat(number);
        } else {
            writeByte(TypeFloat | flag);
            writeFloatAsText(number);
        }
        return;
    }
    if (type == m_realm->typeComplex()) {
        auto* number = uncheckedDowncast<PyComplex>(value.asCell());
        if (m_version > 1) {
            writeByte(TypeBinaryComplex | flag);
            writeBinaryFloat(number->real());
            writeBinaryFloat(number->imaginary());
        } else {
            writeByte(TypeComplex | flag);
            writeFloatAsText(number->real());
            writeFloatAsText(number->imaginary());
        }
        return;
    }
    if (type == m_realm->typeBytes()) {
        writeByte(TypeBytes | flag);
        writeSized(uncheckedDowncast<JSUint8Array>(value.asCell())->span());
        return;
    }
    if (type == m_realm->typeStr())
        return writeString(asString(value), flag);
    if (type == m_realm->typeTuple()) {
        PyTuple* tuple = asTuple(value);
        unsigned length = tuple->length();
        if (m_version >= 4 && length < 256) {
            writeByte(TypeSmallTuple | flag);
            writeByte(static_cast<uint8_t>(length));
        } else {
            writeByte(TypeTuple | flag);
            if (!writeSize(length))
                return;
        }
        for (unsigned i = 0; i < length; ++i)
            writeObject(tuple->at(i));
        return;
    }
    if (type == m_realm->typeList()) {
        JSArray* list = asList(value);
        unsigned length = list->length();
        writeByte(TypeList | flag);
        if (!writeSize(length))
            return;
        for (unsigned i = 0; i < length; ++i) {
            // What tells of what is being written can run a program's code, and so can an array that JavaScript has done something to.
            JSValue item = i < list->length() ? listGet(m_globalObject, list, i) : jsUndefined();
            if (scope.exception()) [[unlikely]] {
                m_error = Error::Unmarshallable;
                return;
            }
            writeObject(item);
        }
        return;
    }
    if (type == m_realm->typeDict()) {
        writeByte(TypeDict | flag);
        asDict(value)->forEach(m_globalObject, [&] (JSValue key, JSValue item) {
            writeObject(key);
            writeObject(item);
            return true;
        });
        // What comes after the last is nothing at all.
        writeObject(JSValue());
        return;
    }
    if (type == m_realm->typeSet() || type == m_realm->typeFrozenSet())
        return writeSet(uncheckedDowncast<PySet>(value.asCell()), flag, type == m_realm->typeFrozenSet());
    if (isCode(m_globalObject, value)) {
        if (!m_allowsCode) {
            m_error = Error::CodeNotAllowed;
            return;
        }
        return writeCode(value, flag);
    }
    if (hasBuffer(m_globalObject, value)) {
        // Whatever else has bytes to show is written as bytes.
        Buffer buffer = tryBufferOf(m_globalObject, value);
        if (scope.exception() || !buffer) [[unlikely]] {
            writeByte(TypeUnknown);
            --m_depth;
            m_error = Error::Unmarshallable;
            return;
        }
        writeByte(TypeBytes | flag);
        writeSized(buffer.span());
        return;
    }
    if (value.inherits<PySlice>()) {
        if (m_version < 5) {
            writeByte(TypeUnknown);
            m_error = Error::Unmarshallable;
            return;
        }
        auto* slice = uncheckedDowncast<PySlice>(value.asCell());
        writeByte(TypeSlice | flag);
        writeObject(slice->start());
        writeObject(slice->stop());
        writeObject(slice->step());
        complete(value);
        return;
    }
    writeByte(TypeUnknown | flag);
    m_error = Error::Unmarshallable;
}

// _PyMarshal_WriteObjectToString()
JSValue dumpToBytes(JSGlobalObject* globalObject, JSValue value, int version, bool allowsCode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!audit(globalObject, "marshal.dumps"_s, value, jsNumber(version)))
        return { };
    Writer writer(globalObject, version, allowsCode);
    writer.writeObject(value);
    auto error = writer.error();
    // Whatever was raised on the way is lost, and it is said in these words.
    if (scope.exception() && (error == Writer::Error::None || !scope.tryClearException()))
        return { };
    switch (error) {
    case Writer::Error::None:
        RELEASE_AND_RETURN(scope, newBytes(globalObject, writer.bytes()));
    case Writer::Error::NoMemory:
        return raiseMemoryError(globalObject, scope);
    case Writer::Error::NestedTooDeep:
        return raiseValueError(globalObject, scope, "object too deeply nested to marshal"_s);
    case Writer::Error::CodeNotAllowed:
        return raiseValueError(globalObject, scope, "marshalling code objects is disallowed"_s);
    case Writer::Error::Unmarshallable:
        return raiseValueError(globalObject, scope, "unmarshallable object"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Reading

class Reader {
public:
    Reader(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, bool allowsCode)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_realm(globalObject->pyRealm())
        , m_bytes(bytes)
        , m_allowsCode(allowsCode)
    {
    }

    Reader(JSGlobalObject* globalObject, JSValue readable, bool allowsCode)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_realm(globalObject->pyRealm())
        , m_readable(readable)
        , m_allowsCode(allowsCode)
    {
    }

    // Empty, with nothing raised, is a thing that can be read: it is what comes after the last of a dict.
    JSValue readObject();

private:
    // Nothing if it raised.
    std::optional<std::span<const uint8_t>> readBytes(size_t count);
    std::optional<uint8_t> readByte();
    std::optional<int> readShort();
    std::optional<int64_t> readLong();
    // A size, which is not to be less than nothing. `what` is what it is the size of.
    std::optional<size_t> readSize(ASCIILiteral what);
    std::optional<double> readBinaryFloat();
    std::optional<double> readFloatAsText();
    JSValue readBigInt();
    JSValue readCode(bool isMarked);
    JSValue readItem(ASCIILiteral container);
    // Each thing takes a byte at the least. So if something is said to have more in it than there are bytes left, reading it is going to fail, and there is no call to make room for so many first.
    bool isMoreThanThereCanBe(size_t count) const { return !m_readable && count > m_bytes.size(); }
    // It fails as it would have: `standIn` is what is there to be referred to meanwhile.
    JSValue readUntilItFails(JSValue standIn, bool isMarked, ASCIILiteral container);

    // r_ref_reserve(), r_ref_insert() and r_ref()
    std::optional<size_t> reserveReference(bool isMarked);
    void insertReference(JSValue value, size_t index, bool isMarked)
    {
        if (value && isMarked)
            m_references.at(index) = value;
    }
    void addReference(JSValue value, bool isMarked)
    {
        if (value && isMarked)
            m_references.append(value);
    }

    template<typename... Arguments>
    JSValue fail(BuiltinType type, Arguments... message)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        return raise(m_globalObject, scope, type, concatenate(message...));
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    PyRealm* m_realm;
    std::span<const uint8_t> m_bytes;
    JSValue m_readable;
    ByteVector m_buffer;
    // What was marked, in the order that it was come upon. None is what has not been read to the end yet.
    MarkedVector<JSValue> m_references;
    int m_depth { 0 };
    bool m_allowsCode;
};

// r_string()
std::optional<std::span<const uint8_t>> Reader::readBytes(size_t count)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    if (!m_readable) {
        if (m_bytes.size() < count) {
            fail(BuiltinType::EOFError, "marshal data too short"_s);
            return std::nullopt;
        }
        auto result = m_bytes.first(count);
        m_bytes = m_bytes.subspan(count);
        return result;
    }
    ByteVector zeros;
    zeros.appendFill(0, count);
    JSValue array = newByteArray(m_globalObject, zeros);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    JSValue view = call(m_globalObject, m_realm->typeMemoryView(), array);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    JSValue readInto = getAttribute(m_globalObject, m_readable, Identifier::fromString(m_vm, "readinto"_s));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    JSValue result = call(m_globalObject, readInto, view);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    // PyNumber_AsSsize_t(result, PyExc_ValueError)
    JSValue integer = toInt(m_globalObject, result);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto read = tryInt64(integer);
    if (!read) {
        fail(BuiltinType::ValueError, "cannot fit '"_s, typeName(m_globalObject, result), "' into an index-sized integer"_s);
        return std::nullopt;
    }
    if (*read != static_cast<int64_t>(count)) {
        if (*read > static_cast<int64_t>(count))
            fail(BuiltinType::ValueError, "read() returned too much data: "_s, count, " bytes requested, "_s, *read, " returned"_s);
        else
            fail(BuiltinType::EOFError, "EOF read where not expected"_s);
        return std::nullopt;
    }
    m_buffer = ByteVector();
    m_buffer.append(uncheckedDowncast<JSUint8Array>(array.asCell())->span().first(std::min(count, uncheckedDowncast<JSUint8Array>(array.asCell())->length())));
    if (m_buffer.hasOverflowed() || m_buffer.size() != count) {
        raiseMemoryError(m_globalObject, scope);
        return std::nullopt;
    }
    return m_buffer.span();
}

std::optional<uint8_t> Reader::readByte()
{
    if (!m_readable) {
        if (m_bytes.empty()) {
            fail(BuiltinType::EOFError, "EOF read where not expected"_s);
            return std::nullopt;
        }
        uint8_t byte = m_bytes[0];
        m_bytes = m_bytes.subspan(1);
        return byte;
    }
    auto bytes = readBytes(1);
    if (!bytes)
        return std::nullopt;
    return (*bytes)[0];
}

std::optional<int> Reader::readShort()
{
    auto bytes = readBytes(2);
    if (!bytes)
        return std::nullopt;
    return static_cast<int16_t>((*bytes)[0] | (*bytes)[1] << 8);
}

std::optional<int64_t> Reader::readLong()
{
    auto bytes = readBytes(4);
    if (!bytes)
        return std::nullopt;
    return static_cast<int32_t>(static_cast<uint32_t>((*bytes)[0]) | static_cast<uint32_t>((*bytes)[1]) << 8 | static_cast<uint32_t>((*bytes)[2]) << 16 | static_cast<uint32_t>((*bytes)[3]) << 24);
}

std::optional<size_t> Reader::readSize(ASCIILiteral what)
{
    auto size = readLong();
    if (!size)
        return std::nullopt;
    if (*size < 0 || *size > largestSize) {
        fail(BuiltinType::ValueError, "bad marshal data ("_s, what, " size out of range)"_s);
        return std::nullopt;
    }
    return static_cast<size_t>(*size);
}

std::optional<double> Reader::readBinaryFloat()
{
    auto bytes = readBytes(8);
    if (!bytes)
        return std::nullopt;
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i)
        bits |= static_cast<uint64_t>((*bytes)[i]) << (i * 8);
    return std::bit_cast<double>(bits);
}

// r_float_str()
std::optional<double> Reader::readFloatAsText()
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    auto length = readByte();
    if (!length)
        return std::nullopt;
    auto bytes = readBytes(*length);
    if (!bytes)
        return std::nullopt;
    // It is as much as comes before a zero, as it is to C.
    auto text = bytes->first(std::min<size_t>(find(*bytes, 0), bytes->size()));
    // PyOS_string_to_double(), which is float() but that it will have nothing around the number or between the digits of it.
    bool isPlain = !text.empty() && std::ranges::all_of(text, [] (uint8_t c) { return isASCII(c) && !isASCIIWhitespace(c) && c != '_' && c != '\v'; });
    JSValue result;
    if (isPlain) {
        result = call(m_globalObject, m_realm->typeFloat(), jsString(m_vm, String(text)));
        if (scope.exception() && !catchException(m_globalObject, BuiltinType::ValueError))
            return std::nullopt;
    }
    if (!result || scope.exception()) {
        // As "%.200s" takes them.
        fail(BuiltinType::ValueError, "could not convert string to float: '"_s, String::fromUTF8ReplacingInvalidSequences(text.first(std::min<size_t>(text.size(), 200))), '\'');
        return std::nullopt;
    }
    return classify(result).real;
}

// r_PyLong()
JSValue Reader::readBigInt()
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    auto given = readLong();
    if (!given)
        return { };
    if (*given < -largestSize || *given > largestSize)
        return fail(BuiltinType::ValueError, "bad marshal data (long size out of range)"_s);
    if (!*given)
        return jsNumber(0);
    uint64_t count = static_cast<uint64_t>(std::abs(*given));
    // Room is made for them as they come, since there may be nothing like as many as it says.
    Vector<uint64_t> digits;
    for (uint64_t i = 0; i < count; ++i) {
        auto digit = readShort();
        if (!digit)
            return { };
        if (*digit < 0 || *digit > (1 << digitShift))
            return fail(BuiltinType::ValueError, "bad marshal data (digit out of range in long)"_s);
        if (!*digit && i == count - 1)
            return fail(BuiltinType::ValueError, "bad marshal data (unnormalized long data)"_s);
        uint64_t position = i * digitShift;
        size_t index = static_cast<size_t>(position / JSBigInt::digitBits);
        unsigned shift = position % JSBigInt::digitBits;
        UInt128 wide = static_cast<UInt128>(static_cast<uint64_t>(*digit)) << shift;
        for (; wide; ++index) {
            while (index >= digits.size()) {
                if (!digits.tryAppend(0))
                    return raiseMemoryError(m_globalObject, scope);
            }
            wide += digits[index];
            digits[index] = static_cast<uint64_t>(wide);
            wide >>= JSBigInt::digitBits;
        }
    }
    RELEASE_AND_RETURN(scope, intFromDigits(m_globalObject, digits.span(), *given < 0));
}

std::optional<size_t> Reader::reserveReference(bool isMarked)
{
    if (!isMarked)
        return 0;
    size_t index = m_references.size();
    if (index >= 0x7FFFFFFE) {
        fail(BuiltinType::ValueError, "bad marshal data (index list too large)"_s);
        return std::nullopt;
    }
    m_references.append(jsUndefined());
    return index;
}

// One of what is in something, which is not to be nothing at all.
JSValue Reader::readItem(ASCIILiteral container)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    JSValue item = readObject();
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        return fail(BuiltinType::TypeError, "NULL object in marshal data for "_s, container);
    return item;
}

JSValue Reader::readUntilItFails(JSValue standIn, bool isMarked, ASCIILiteral container)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    RETURN_IF_EXCEPTION(scope, { });
    addReference(standIn, isMarked);
    while (true) {
        readItem(container);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

JSValue Reader::readCode(bool isMarked)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    if (!m_allowsCode)
        return fail(BuiltinType::ValueError, "unmarshalling code objects is disallowed"_s);
    auto index = reserveReference(isMarked);
    if (!index)
        return { };
    std::array<int32_t, 5> counts; // argcount, posonlyargcount, kwonlyargcount, stacksize, flags
    for (auto& count : counts) {
        auto value = readLong();
        if (!value)
            return { };
        count = static_cast<int32_t>(*value);
    }
    MarkedArgumentBuffer objects; // code, consts, names, localsplusnames, localspluskinds, filename, name, qualname
    for (unsigned i = 0; i < 8; ++i) {
        JSValue object = readItem("code object"_s);
        RETURN_IF_EXCEPTION(scope, { });
        objects.append(object);
    }
    auto firstLine = readLong();
    if (!firstLine)
        return { };
    JSValue lineTable = readItem("code object"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue exceptionTable = readItem("code object"_s);
    RETURN_IF_EXCEPTION(scope, { });

    // _PyCode_Validate(), as far as what is done here goes. The rest is seen to by what makes it.
    JSValue names = objects.at(3);
    JSValue kinds = objects.at(4);
    if (!isTuple(names) || !isBytes(kinds) || asTuple(names)->length() != uncheckedDowncast<JSUint8Array>(kinds.asCell())->length())
        return fail(BuiltinType::SystemError, "bad argument to internal function"_s);
    MarkedArgumentBuffer variables;
    MarkedArgumentBuffer cells;
    MarkedArgumentBuffer free;
    auto kindsSpan = uncheckedDowncast<JSUint8Array>(kinds.asCell())->span();
    for (unsigned i = 0; i < asTuple(names)->length(); ++i) {
        JSValue name = asTuple(names)->at(i);
        if (kindsSpan[i] & kindLocal)
            variables.append(name);
        if (kindsSpan[i] & kindCell)
            cells.append(name);
        if (kindsSpan[i] & kindFree)
            free.append(name);
    }
    // In the order that code() takes them.
    MarkedArgumentBuffer parts;
    parts.append(jsNumber(counts[0]));
    parts.append(jsNumber(counts[1]));
    parts.append(jsNumber(counts[2]));
    parts.append(intFromUInt64(m_globalObject, variables.size()));
    parts.append(jsNumber(counts[3]));
    parts.append(jsNumber(counts[4]));
    parts.append(objects.at(0));
    parts.append(objects.at(1));
    parts.append(objects.at(2));
    parts.append(PyTuple::createFromArguments(m_globalObject, variables));
    parts.append(objects.at(5));
    parts.append(objects.at(6));
    parts.append(objects.at(7));
    parts.append(jsNumber(static_cast<int32_t>(*firstLine)));
    parts.append(lineTable);
    parts.append(exceptionTable);
    parts.append(PyTuple::createFromArguments(m_globalObject, free));
    parts.append(PyTuple::createFromArguments(m_globalObject, cells));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue code = newCodeFromParts(m_globalObject, parts);
    RETURN_IF_EXCEPTION(scope, { });
    insertReference(code, *index, isMarked);
    return code;
}

// r_object()
JSValue Reader::readObject()
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    auto code = readByte();
    if (!code) {
        if (catchException(m_globalObject, BuiltinType::EOFError))
            fail(BuiltinType::EOFError, "EOF read where object expected"_s);
        return { };
    }
    if (m_depth + 1 > maximumDepth || !m_vm.isSafeToRecurse())
        return fail(BuiltinType::ValueError, "recursion limit exceeded"_s);
    ++m_depth;
    auto leave = makeScopeExit([&] { --m_depth; });
    bool isMarked = *code & flagReference;
    bool isInterned = false;
    auto finish = [&] (JSValue value) {
        addReference(value, isMarked);
        return value;
    };
    auto finishString = [&] (JSString* string) -> JSValue {
        if (isInterned)
            string = m_realm->intern(m_globalObject, string);
        return finish(string);
    };

    switch (*code & ~flagReference) {
    case TypeNull:
        return { };
    case TypeNone:
        return jsUndefined();
    case TypeStopIteration:
        return m_realm->type(BuiltinType::StopIteration);
    case TypeEllipsis:
        return m_realm->ellipsis();
    case TypeFalse:
        return jsBoolean(false);
    case TypeTrue:
        return jsBoolean(true);
    case TypeInt: {
        auto value = readLong();
        if (!value)
            return { };
        return finish(jsNumber(static_cast<int32_t>(*value)));
    }
    case TypeInt64: {
        auto bytes = readBytes(8);
        if (!bytes)
            return { };
        uint64_t bits = 0;
        for (unsigned i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>((*bytes)[i]) << (i * 8);
        return finish(intFromInt64(m_globalObject, static_cast<int64_t>(bits)));
    }
    case TypeLong: {
        JSValue value = readBigInt();
        RETURN_IF_EXCEPTION(scope, { });
        return finish(value);
    }
    case TypeFloat: {
        auto value = readFloatAsText();
        if (!value)
            return { };
        return finish(floatFromDouble(*value));
    }
    case TypeBinaryFloat: {
        auto value = readBinaryFloat();
        if (!value)
            return { };
        return finish(floatFromDouble(*value));
    }
    case TypeComplex: {
        auto real = readFloatAsText();
        if (!real)
            return { };
        auto imaginary = readFloatAsText();
        if (!imaginary)
            return { };
        return finish(PyComplex::create(m_globalObject, *real, *imaginary));
    }
    case TypeBinaryComplex: {
        auto real = readBinaryFloat();
        if (!real)
            return { };
        auto imaginary = readBinaryFloat();
        if (!imaginary)
            return { };
        return finish(PyComplex::create(m_globalObject, *real, *imaginary));
    }
    case TypeBytes: {
        auto size = readSize("bytes object"_s);
        if (!size)
            return { };
        auto bytes = readBytes(*size);
        if (!bytes)
            return { };
        JSValue value = newBytes(m_globalObject, *bytes);
        RETURN_IF_EXCEPTION(scope, { });
        return finish(value);
    }
    case TypeASCIIInterned:
    case TypeASCII:
    case TypeShortASCIIInterned:
    case TypeShortASCII: {
        uint8_t type = *code & ~flagReference;
        isInterned = type == TypeASCIIInterned || type == TypeShortASCIIInterned;
        size_t size;
        if (type == TypeASCII || type == TypeASCIIInterned) {
            auto given = readSize("string"_s);
            if (!given)
                return { };
            size = *given;
        } else {
            auto given = readByte();
            if (!given)
                return { };
            size = *given;
        }
        auto bytes = readBytes(size);
        if (!bytes)
            return { };
        // A character for each, whether or not it is ASCII: PyUnicode_FromKindAndData(PyUnicode_1BYTE_KIND, ...).
        JSValue string = strOrMemoryError(m_globalObject, String(*bytes));
        RETURN_IF_EXCEPTION(scope, { });
        return finishString(asString(string));
    }
    case TypeInterned:
    case TypeUnicode: {
        isInterned = (*code & ~flagReference) == TypeInterned;
        auto size = readSize("string"_s);
        if (!size)
            return { };
        auto bytes = readBytes(*size);
        if (!bytes)
            return { };
        // What is said if they will not do has them in it.
        JSValue object = newBytes(m_globalObject, *bytes);
        RETURN_IF_EXCEPTION(scope, { });
        String text = decodeBytes(m_globalObject, uncheckedDowncast<JSUint8Array>(object.asCell())->span(), "utf-8"_s, "surrogatepass"_s);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue string = strOrMemoryError(m_globalObject, text);
        RETURN_IF_EXCEPTION(scope, { });
        return finishString(asString(string));
    }
    case TypeSmallTuple:
    case TypeTuple: {
        size_t size;
        if ((*code & ~flagReference) == TypeSmallTuple) {
            auto given = readByte();
            if (!given)
                return { };
            size = *given;
        } else {
            auto given = readSize("tuple"_s);
            if (!given)
                return { };
            size = *given;
        }
        if (isMoreThanThereCanBe(size))
            RELEASE_AND_RETURN(scope, readUntilItFails(m_realm->emptyTuple(), isMarked, "tuple"_s));
        PyTuple* tuple = PyTuple::tryCreate(m_globalObject, static_cast<unsigned>(size));
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 0; i < size; ++i)
            tuple->initializeAt(m_vm, i, jsUndefined());
        addReference(tuple, isMarked);
        for (unsigned i = 0; i < size; ++i) {
            JSValue item = readItem("tuple"_s);
            RETURN_IF_EXCEPTION(scope, { });
            tuple->initializeAt(m_vm, i, item);
        }
        return tuple;
    }
    case TypeList: {
        auto size = readSize("list"_s);
        if (!size)
            return { };
        if (isMoreThanThereCanBe(*size))
            RELEASE_AND_RETURN(scope, readUntilItFails(newList(m_globalObject), isMarked, "list"_s));
        if (*size > maxListLength)
            return raiseMemoryError(m_globalObject, scope);
        JSArray* list = newList(m_globalObject, static_cast<unsigned>(*size));
        RETURN_IF_EXCEPTION(scope, { });
        addReference(list, isMarked);
        for (unsigned i = 0; i < *size; ++i) {
            JSValue item = readItem("list"_s);
            RETURN_IF_EXCEPTION(scope, { });
            listSet(m_globalObject, list, i, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return list;
    }
    case TypeDict: {
        PyDict* dict = PyDict::create(m_globalObject);
        addReference(dict, isMarked);
        while (true) {
            JSValue key = readObject();
            RETURN_IF_EXCEPTION(scope, { });
            if (!key)
                break;
            JSValue value = readObject();
            RETURN_IF_EXCEPTION(scope, { });
            if (!value)
                break;
            dict->set(m_globalObject, key, value);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return dict;
    }
    case TypeSet:
    case TypeFrozenSet: {
        bool isFrozen = (*code & ~flagReference) == TypeFrozenSet;
        auto size = readSize("set"_s);
        if (!size)
            return { };
        if (!*size && isFrozen) {
            JSValue empty = call(m_globalObject, m_realm->typeFrozenSet());
            RETURN_IF_EXCEPTION(scope, { });
            return finish(empty);
        }
        PySet* set = PySet::create(m_vm, (isFrozen ? m_realm->typeFrozenSet() : m_realm->typeSet())->instanceStructure());
        // A frozenset is not there to be referred to until it has all that is in it.
        size_t index = 0;
        if (isFrozen) {
            auto reserved = reserveReference(isMarked);
            if (!reserved)
                return { };
            index = *reserved;
        } else
            addReference(set, isMarked);
        for (size_t i = 0; i < *size; ++i) {
            JSValue item = readItem("set"_s);
            RETURN_IF_EXCEPTION(scope, { });
            set->add(m_globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (isFrozen)
            insertReference(set, index, isMarked);
        return set;
    }
    case TypeCode:
        RELEASE_AND_RETURN(scope, readCode(isMarked));
    case TypeReference: {
        auto index = readLong();
        if (!index)
            return { };
        if (*index < 0 || static_cast<size_t>(*index) >= m_references.size() || isNone(m_references.at(*index)))
            return fail(BuiltinType::ValueError, "bad marshal data (invalid reference)"_s);
        return m_references.at(*index);
    }
    case TypeSlice: {
        auto index = reserveReference(isMarked);
        if (!index)
            return { };
        // If any of them is nothing at all, so is this.
        JSValue start = readObject();
        RETURN_IF_EXCEPTION(scope, { });
        if (!start)
            return { };
        JSValue stop = readObject();
        RETURN_IF_EXCEPTION(scope, { });
        if (!stop)
            return { };
        JSValue step = readObject();
        RETURN_IF_EXCEPTION(scope, { });
        if (!step)
            return { };
        JSValue slice = PySlice::create(m_globalObject, start, stop, step);
        insertReference(slice, *index, isMarked);
        return slice;
    }
    default:
        return fail(BuiltinType::ValueError, "bad marshal data (unknown type code)"_s);
    }
}

// The end of read_object()
JSValue readWhole(JSGlobalObject* globalObject, Reader& reader)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue value = reader.readObject();
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return raiseTypeError(globalObject, scope, "NULL object in marshal data for object"_s);
    return value;
}

struct Options {
    int version { currentVersion };
    bool allowsCode { true };
};

// The version, if it is taken, and allow_code. Nothing if it raised.
std::optional<Options> optionsFrom(JSGlobalObject* globalObject, ThrowScope& scope, JSValue version, JSValue allowsCode)
{
    Options options;
    if (version) {
        auto given = toCInt(globalObject, version);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        options.version = *given;
    }
    if (allowsCode) {
        options.allowsCode = isTrue(globalObject, allowsCode);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
    }
    return options;
}

} // anonymous namespace

JSValue marshalDumps(JSGlobalObject* globalObject, JSValue value)
{
    return dumpToBytes(globalObject, value, currentVersion, true);
}

JSValue marshalLoads(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    Reader reader(globalObject, bytes, true);
    return readWhole(globalObject, reader);
}

// dumps(value, version=version, /, *, allow_code=True)
PYTHON_NATIVE(marshalDumpsFunction)
{
    NATIVE_PROLOGUE();
    auto options = optionsFrom(globalObject, scope, args.at(1), args.at(2));
    if (!options)
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(dumpToBytes(globalObject, args.at(0), options->version, options->allowsCode)));
}

// dump(value, file, version=version, /, *, allow_code=True)
PYTHON_NATIVE(marshalDumpFunction)
{
    NATIVE_PROLOGUE();
    auto options = optionsFrom(globalObject, scope, args.at(2), args.at(3));
    if (!options)
        return { };
    JSValue bytes = dumpToBytes(globalObject, args.at(0), options->version, options->allowsCode);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue write = getAttribute(globalObject, args.at(1), Identifier::fromString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, write, bytes)));
}

// loads(bytes, /, *, allow_code=True)
PYTHON_NATIVE(marshalLoadsFunction)
{
    NATIVE_PROLOGUE();
    auto options = optionsFrom(globalObject, scope, JSValue(), args.at(1));
    if (!options)
        return { };
    Buffer buffer = bufferOf(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (globalObject->pyRealm()->auditHooks()) [[unlikely]] {
        JSValue copy = newBytes(globalObject, buffer.span());
        RETURN_IF_EXCEPTION(scope, { });
        if (!audit(globalObject, "marshal.loads"_s, copy))
            return { };
    }
    Reader reader(globalObject, buffer.span(), options->allowsCode);
    RELEASE_AND_RETURN(scope, JSValue::encode(readWhole(globalObject, reader)));
}

// load(file, /, *, allow_code=True)
PYTHON_NATIVE(marshalLoadFunction)
{
    NATIVE_PROLOGUE();
    auto options = optionsFrom(globalObject, scope, JSValue(), args.at(1));
    if (!options)
        return { };
    JSValue file = args.at(0);
    // To see that it can be read from, and that it is bytes that come of it.
    JSValue read = getAttribute(globalObject, file, Identifier::fromString(vm, "read"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue data = call(globalObject, read, jsNumber(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance(globalObject, data, realm->typeBytes()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("file.read() returned not bytes but "_s, typeName(globalObject, data))));
    if (!audit(globalObject, "marshal.load"_s))
        return { };
    Reader reader(globalObject, file, options->allowsCode);
    RELEASE_AND_RETURN(scope, JSValue::encode(readWhole(globalObject, reader)));
}

JSObject* createMarshalModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "marshal"_s);
    addFunction(globalObject, module, "dump"_s, marshalDumpFunction);
    addFunction(globalObject, module, "load"_s, marshalLoadFunction);
    addFunction(globalObject, module, "dumps"_s, marshalDumpsFunction);
    addFunction(globalObject, module, "loads"_s, marshalLoadsFunction);
    module->putDirect(vm, Identifier::fromString(vm, "version"_s), jsNumber(currentVersion));
    return module;
}

} } // namespace JSC::Python
