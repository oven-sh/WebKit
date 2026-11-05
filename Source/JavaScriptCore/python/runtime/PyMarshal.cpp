/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCodeCache.h"
#include "PyCodecs.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <wtf/HashMap.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module marshal. (Python/marshal.c in CPython is the reference, and what it writes is what this reads.)
//
// But for code objects: their bytecode is JSC's, so they have a type code and a form of their own (PyCodeCache.h), which only
// the build that wrote them reads.

static constexpr int marshalVersion = 6;
static constexpr unsigned marshalMaxDepth = 2000;
static constexpr uint8_t marshalFlagRef = 0x80;
static constexpr unsigned marshalDigitBits = 15;

enum MarshalType : uint8_t {
    MarshalNull = '0',
    MarshalNone = 'N',
    MarshalFalse = 'F',
    MarshalTrue = 'T',
    MarshalStopIteration = 'S',
    MarshalEllipsis = '.',
    MarshalInt = 'i',
    MarshalInt64 = 'I',
    MarshalFloat = 'f',
    MarshalBinaryFloat = 'g',
    MarshalComplex = 'x',
    MarshalBinaryComplex = 'y',
    MarshalLong = 'l',
    MarshalBytes = 's',
    MarshalInterned = 't',
    MarshalRef = 'r',
    MarshalTuple = '(',
    MarshalList = '[',
    MarshalDict = '{',
    MarshalFrozenDict = '}',
    MarshalCode = 'c',
    MarshalBunCode = marshalTypeOfCode,
    MarshalUnicode = 'u',
    MarshalUnknown = '?',
    MarshalSet = '<',
    MarshalFrozenSet = '>',
    MarshalASCII = 'a',
    MarshalASCIIInterned = 'A',
    MarshalSmallTuple = ')',
    MarshalShortASCII = 'z',
    MarshalShortASCIIInterned = 'Z',
    MarshalSlice = ':',
};

// ---- Writing ----

// From version 3 on, an object that occurs twice is written once, and referred to by its number the second time. CPython
// numbers every object that has more than one reference of any kind, which is more than it needs and is not known here. This
// numbers the objects that do occur twice in what is being written, which takes a first pass to find out, and those of which
// CPython has only one, so that it always numbers them: small ints, interned strs, the empty tuple, bytes of at most one byte.
class MarshalWriter {
public:
    enum class Error : uint8_t { None, Unmarshallable, NestedTooDeep, CodeNotAllowed, SetChangedSize, Exception };

    MarshalWriter(JSGlobalObject* globalObject, int version, bool allowCode)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_version(version)
        , m_allowCode(allowCode)
    {
    }

    // False: an exception.
    bool write(JSValue value, bool isInSet = false)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (m_version >= 3)
            count(value, isInSet);
        if (m_error == Error::None)
            writeObject(value);
        switch (m_error) {
        case Error::None:
            return true;
        case Error::Exception:
            EXCEPTION_ASSERT(scope.exception());
            return false;
        case Error::NestedTooDeep:
            takePendingException(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            raiseValueError(m_globalObject, scope, "object too deeply nested to marshal"_s);
            return false;
        case Error::CodeNotAllowed:
            takePendingException(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            raiseValueError(m_globalObject, scope, "marshalling code objects is disallowed"_s);
            return false;
        case Error::SetChangedSize:
            takePendingException(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            raiseRuntimeError(m_globalObject, scope, "set changed size during iteration"_s);
            return false;
        case Error::Unmarshallable:
            takePendingException(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            raiseValueError(m_globalObject, scope, "unmarshallable object"_s);
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Vector<uint8_t>& bytes() { return m_bytes; }

    // The order of what each is written as on its own, which does not depend on hashes. False: m_error says why not.
    bool putInOrder(PySet* set, MarkedArgumentBuffer& items, Vector<unsigned>& order)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        size_t size = set->size();
        JSValue item;
        for (size_t position = 0; set->next(position, item);)
            items.append(item);
        struct Entry {
            Vector<uint8_t> dump;
            unsigned index;
        };
        Vector<Entry> entries;
        entries.reserveInitialCapacity(size);
        for (unsigned i = 0; i < items.size(); ++i) {
            MarshalWriter writer(m_globalObject, m_version, m_allowCode);
            writer.m_depth = m_depth;
            // (Numbered here too, as in CPython, which holds a reference of its own to the item meanwhile.)
            if (failed(scope, writer.write(items.at(i), true))) {
                m_error = Error::Unmarshallable;
                return false;
            }
            entries.append(Entry { WTF::move(writer.m_bytes), i });
        }
        if (set->size() != size) {
            m_error = Error::SetChangedSize;
            return false;
        }
        std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
            if (auto order = std::lexicographical_compare_three_way(a.dump.begin(), a.dump.end(), b.dump.begin(), b.dump.end()); order != 0)
                return order < 0;
            return a.index < b.index;
        });
        order = entries.map([](auto& entry) { return entry.index; });
        return true;
    }

private:
    using Key = uint64_t;
    template<typename Value> using KeyMap = UncheckedKeyHashMap<Key, Value, DefaultHash<Key>, WTF::UnsignedWithZeroKeyHashTraits<Key>>;

    // No JSValue has only bit 48 set of its upper 16.
    static constexpr Key singletonKeys = 1ull << 48;
    static constexpr Key emptyTupleKey = singletonKeys | 0x20000;
    static Key keyOfShortStr(unsigned lengthAndCharacter) { return singletonKeys | lengthAndCharacter; }
    static Key keyOfShortBytes(unsigned lengthAndByte) { return singletonKeys | 0x10000 | lengthAndByte; }

    enum class Kind : uint8_t { Constant, Int, Float, Complex, Bytes, Str, Tuple, List, Dict, FrozenDict, Set, FrozenSet, Code, Buffer, Slice, Unknown };

    Kind kindOf(JSValue value) const
    {
        if (isNone(value) || value.isBoolean())
            return Kind::Constant;
        if (value.isNumber())
            return isSmallInt(value) ? Kind::Int : Kind::Float;
        if (!value.isCell())
            return Kind::Unknown;
        JSCell* cell = value.asCell();
        if (cell->isString())
            return Kind::Str;
        if (cell->isHeapBigInt())
            return Kind::Int;
        if (isExactList(value))
            return Kind::List;
        if (value == m_runtime.ellipsis() || value == JSValue(m_runtime.StopIteration()))
            return Kind::Constant;
        if (isPyObject(cell)) {
            PyType* type = uncheckedDowncast<PyObject>(cell)->type();
            switch (cell->type()) {
            case PyFloatType:
                if (type == m_runtime.floatType())
                    return Kind::Float;
                break;
            case PyComplexType:
                if (type == m_runtime.complexType())
                    return Kind::Complex;
                break;
            case PyBytesType:
                if (type == m_runtime.bytesType())
                    return Kind::Bytes;
                break;
            case PyTupleType:
                if (type == m_runtime.tupleType())
                    return Kind::Tuple;
                break;
            case PyDictType:
                if (type == m_runtime.dictType())
                    return Kind::Dict;
                if (type == m_runtime.frozendictType())
                    return Kind::FrozenDict;
                break;
            case PySetType:
                if (type == m_runtime.setType())
                    return Kind::Set;
                if (type == m_runtime.frozensetType())
                    return Kind::FrozenSet;
                break;
            case PySliceType:
                return Kind::Slice;
            default:
                if (PyCode::is(value))
                    return Kind::Code;
                break;
            }
        }
        return hasBuffer(value) ? Kind::Buffer : Kind::Unknown;
    }

    static bool isInterned(JSString* string)
    {
        if (string->length() <= 1) {
            // CPython has one str for each of these.
            const StringImpl* impl = string->tryGetValueImpl();
            return !string->length() || (impl && (*impl)[0] < 0x100);
        }
        const StringImpl* impl = string->tryGetValueImpl();
        return impl && impl->isAtom();
    }

    // `isAlwaysNumbered`: whether or not it occurs again.
    Key keyOf(JSValue value, Kind kind, bool& isAlwaysNumbered) const
    {
        isAlwaysNumbered = false;
        switch (kind) {
        case Kind::Int: {
            if (!isSmallInt(value))
                break;
            // The same number may be encoded in two ways.
            int64_t number = smallIntValue(value);
            isAlwaysNumbered = number >= -5 && number <= 1024;
            return JSValue::encode(smallInt(number));
        }
        case Kind::Str: {
            JSString* string = asString(value);
            if (!isInterned(string))
                break;
            isAlwaysNumbered = true;
            const StringImpl* impl = string->tryGetValueImpl();
            if (string->length() <= 1)
                return keyOfShortStr(string->length() ? 0x100 | (*impl)[0] : 0);
            return std::bit_cast<uintptr_t>(impl);
        }
        case Kind::Bytes: {
            auto span = uncheckedDowncast<PyBytes>(value.asCell())->span();
            if (span.size() > 1)
                break;
            isAlwaysNumbered = true;
            return keyOfShortBytes(span.size() ? 0x100 | span[0] : 0);
        }
        case Kind::Tuple:
            if (uncheckedDowncast<PyTuple>(value.asCell())->length())
                break;
            isAlwaysNumbered = true;
            return emptyTupleKey;
        default:
            break;
        }
        return JSValue::encode(value);
    }

    class DepthScope {
    public:
        explicit DepthScope(MarshalWriter& writer)
            : m_writer(writer)
        {
            if (++writer.m_depth > marshalMaxDepth || !writer.m_runtime.vm().isSafeToRecurseSoft())
                writer.m_error = Error::NestedTooDeep;
        }
        ~DepthScope() { --m_writer.m_depth; }

    private:
        MarshalWriter& m_writer;
    };

    // The first pass: which objects occur more than once.
    void count(JSValue value, bool isInSet = false)
    {
        if (m_error != Error::None)
            return;
        DepthScope depth(*this);
        if (m_error != Error::None)
            return;
        Kind kind = kindOf(value);
        if (kind == Kind::Constant || kind == Kind::Unknown)
            return;
        bool isAlwaysNumbered;
        Key key = keyOf(value, kind, isAlwaysNumbered);
        if (!m_isRepeated.add(key, isAlwaysNumbered || isInSet).isNewEntry) {
            m_isRepeated.set(key, true);
            return;
        }
        switch (kind) {
        case Kind::Tuple:
            countTuple(uncheckedDowncast<PyTuple>(value.asCell()));
            break;
        case Kind::List:
            countList(asList(value));
            break;
        case Kind::Dict:
        case Kind::FrozenDict:
            countDict(uncheckedDowncast<PyDict>(value.asCell()));
            break;
        case Kind::Set:
        case Kind::FrozenSet:
            countSet(uncheckedDowncast<PySet>(value.asCell()));
            break;
        case Kind::Slice:
            countSlice(uncheckedDowncast<PySlice>(value.asCell()));
            break;
        default:
            break;
        }
    }

    // Each kind of container has a function of its own, here and below, to keep the frames of the recursion small: 2000 levels
    // have to fit on the stack, in a debug build too.
    NEVER_INLINE void countTuple(PyTuple* tuple)
    {
        for (unsigned i = 0; i < tuple->length(); ++i)
            count(tuple->at(i));
    }

    NEVER_INLINE void countList(JSArray* list)
    {
        for (unsigned i = 0; i < listLength(list); ++i)
            count(listGet(list, i));
    }

    NEVER_INLINE void countDict(PyDict* dict)
    {
        JSValue itemKey, itemValue;
        for (unsigned position = 0; dict->next(position, itemKey, itemValue);) {
            count(itemKey);
            count(itemValue);
        }
    }

    NEVER_INLINE void countSet(PySet* set)
    {
        // CPython numbers them all: the list it sorts them in holds a second reference to each.
        JSValue item;
        for (size_t position = 0; set->next(position, item);)
            count(item, true);
    }

    NEVER_INLINE void countSlice(PySlice* slice)
    {
        count(slice->start());
        count(slice->stop());
        count(slice->step());
    }

    void writeByte(uint8_t byte) { m_bytes.append(byte); }
    void writeShort(unsigned value)
    {
        writeByte(static_cast<uint8_t>(value));
        writeByte(static_cast<uint8_t>(value >> 8));
    }
    void writeLong(uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            writeByte(static_cast<uint8_t>(value >> shift));
    }
    void writeSpan(std::span<const uint8_t> span) { m_bytes.append(span); }

    // False: too long for the format.
    bool writeSize(size_t size)
    {
        if (size > 0x7fffffff) {
            m_error = Error::Unmarshallable;
            return false;
        }
        writeLong(static_cast<uint32_t>(size));
        return true;
    }

    void writeSizedSpan(std::span<const uint8_t> span)
    {
        if (writeSize(span.size()))
            writeSpan(span);
    }

    void writeBinaryDouble(double value)
    {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        for (unsigned shift = 0; shift < 64; shift += 8)
            writeByte(static_cast<uint8_t>(bits >> shift));
    }

    void writeTextDouble(double value)
    {
        auto text = formatDouble(value, 'g', 17, 0).ascii();
        writeByte(static_cast<uint8_t>(text.length()));
        writeSpan(byteCast<uint8_t>(text.span()));
    }

    void writeDouble(double value)
    {
        if (m_version > 1)
            writeBinaryDouble(value);
        else
            writeTextDouble(value);
    }

    NEVER_INLINE void writeInt(JSValue value, uint8_t flag)
    {
        if (isSmallInt(value)) {
            int64_t number = smallIntValue(value);
            if (number >= std::numeric_limits<int32_t>::min() && number <= std::numeric_limits<int32_t>::max()) {
                writeByte(MarshalInt | flag);
                writeLong(static_cast<uint32_t>(static_cast<int32_t>(number)));
                return;
            }
            uint64_t magnitude = number < 0 ? -static_cast<uint64_t>(number) : static_cast<uint64_t>(number);
            writeDigits(std::span<const uint64_t> { &magnitude, 1 }, number < 0, flag);
            return;
        }
        const JSBigInt* bigInt = value.asHeapBigInt();
        writeDigits(bigInt->digits(), bigInt->sign(), flag);
    }

    // In digits of 15 bits, the least significant first.
    void writeDigits(std::span<const uint64_t> words, bool isNegative, uint8_t flag)
    {
        while (!words.empty() && !words.back())
            words = words.first(words.size() - 1);
        size_t bits = words.empty() ? 0 : words.size() * 64 - std::countl_zero(words.back());
        size_t digits = (bits + marshalDigitBits - 1) / marshalDigitBits;
        writeByte(MarshalLong | flag);
        if (digits > 0x7fffffff) {
            m_error = Error::Unmarshallable;
            return;
        }
        writeLong(static_cast<uint32_t>(isNegative ? -static_cast<int32_t>(digits) : static_cast<int32_t>(digits)));
        for (size_t i = 0; i < digits; ++i) {
            size_t bit = i * marshalDigitBits;
            uint64_t digit = words[bit / 64] >> (bit % 64);
            if (bit % 64 > 64 - marshalDigitBits && bit / 64 + 1 < words.size())
                digit |= words[bit / 64 + 1] << (64 - bit % 64);
            writeShort(static_cast<unsigned>(digit & ((1u << marshalDigitBits) - 1)));
        }
    }

    NEVER_INLINE void writeStr(JSString* string, uint8_t flag)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        bool interned = isInterned(string);
        auto view = string->view(m_globalObject);
        if (scope.exception()) [[unlikely]] {
            m_error = Error::Exception;
            return;
        }
        if (m_version >= 4 && view->containsOnlyASCII()) {
            unsigned length = view->length();
            if (length < 256) {
                writeByte((interned ? MarshalShortASCIIInterned : MarshalShortASCII) | flag);
                writeByte(static_cast<uint8_t>(length));
            } else {
                writeByte((interned ? MarshalASCIIInterned : MarshalASCII) | flag);
                writeLong(length);
            }
            for (unsigned i = 0; i < length; ++i)
                writeByte(static_cast<uint8_t>(view[i]));
            return;
        }
        JSValue encoded = codecEncodeUTF8(m_globalObject, string, "surrogatepass"_s);
        if (scope.exception()) [[unlikely]] {
            m_error = Error::Unmarshallable;
            return;
        }
        writeByte((m_version >= 3 && interned ? MarshalInterned : MarshalUnicode) | flag);
        writeSizedSpan(PyBytes::from(encoded)->span());
    }

    NEVER_INLINE void writeSet(PySet* set)
    {
        if (!writeSize(set->size()))
            return;
        MarkedArgumentBuffer items;
        Vector<unsigned> order;
        if (!putInOrder(set, items, order))
            return;
        for (unsigned index : order)
            writeObject(items.at(index));
    }

    NEVER_INLINE void writeTuple(PyTuple* tuple, uint8_t flag)
    {
        if (m_version >= 4 && tuple->length() < 256) {
            writeByte(MarshalSmallTuple | flag);
            writeByte(static_cast<uint8_t>(tuple->length()));
        } else {
            writeByte(MarshalTuple | flag);
            writeLong(tuple->length());
        }
        for (unsigned i = 0; i < tuple->length(); ++i)
            writeObject(tuple->at(i));
    }

    NEVER_INLINE void writeList(JSArray* list, uint8_t flag)
    {
        unsigned length = listLength(list);
        writeByte(MarshalList | flag);
        if (!writeSize(length))
            return;
        // Writing an item may run code, which may shorten the list.
        for (unsigned i = 0; i < length; ++i)
            writeObject(i < listLength(list) ? listGet(list, i) : jsUndefined());
    }

    NEVER_INLINE void writeDict(PyDict* dict)
    {
        JSValue itemKey, itemValue;
        for (unsigned position = 0; dict->next(position, itemKey, itemValue);) {
            writeObject(itemKey);
            writeObject(itemValue);
        }
        writeObject({ });
    }

    NEVER_INLINE void writeSlice(PySlice* slice)
    {
        writeObject(slice->start());
        writeObject(slice->stop());
        writeObject(slice->step());
    }

    NEVER_INLINE void writeBuffer(JSValue value, uint8_t flag)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        BufferHolder holder(m_globalObject);
        if (failed(scope, holder.get(value))) {
            writeByte(MarshalUnknown);
            m_error = Error::Unmarshallable;
            return;
        }
        writeByte(MarshalBytes | flag);
        writeSizedSpan(holder.buffer.span());
    }

    // True: it has been written before, and a reference to that has been written now (or it cannot be).
    NEVER_INLINE bool writeReference(JSValue value, Kind kind, uint8_t& flag, Key& key)
    {
        bool isAlwaysNumbered;
        key = keyOf(value, kind, isAlwaysNumbered);
        if (!m_isRepeated.get(key))
            return false;
        // The number of one of these cannot be used before all of it has been read back.
        bool isNumberedWhenComplete = kind == Kind::Slice || kind == Kind::FrozenDict;
        auto result = m_numbers.add(key, Number { static_cast<uint32_t>(m_numbers.size()), !isNumberedWhenComplete });
        if (result.isNewEntry) {
            flag = marshalFlagRef;
            return false;
        }
        if (!result.iterator->value.isComplete) {
            m_error = Error::Unmarshallable;
            return true;
        }
        writeByte(MarshalRef);
        writeLong(result.iterator->value.number);
        return true;
    }

    NEVER_INLINE void didComplete(Key key)
    {
        if (auto it = m_numbers.find(key); it != m_numbers.end())
            it->value.isComplete = true;
    }

    void writeObject(JSValue value)
    {
        if (m_error != Error::None)
            return;
        DepthScope depth(*this);
        if (m_error != Error::None)
            return;

        if (!value) {
            writeByte(MarshalNull);
            return;
        }
        Kind kind = kindOf(value);
        if (kind == Kind::Constant) {
            if (isNone(value))
                writeByte(MarshalNone);
            else if (value.isBoolean())
                writeByte(value.asBoolean() ? MarshalTrue : MarshalFalse);
            else
                writeByte(value == m_runtime.ellipsis() ? MarshalEllipsis : MarshalStopIteration);
            return;
        }

        uint8_t flag = 0;
        Key key = 0;
        if (kind == Kind::Code && !m_allowCode) {
            m_error = Error::CodeNotAllowed;
            return;
        }
        if (m_version >= 3 && kind != Kind::Unknown && writeReference(value, kind, flag, key))
            return;

        switch (kind) {
        case Kind::Constant:
            RELEASE_ASSERT_NOT_REACHED();
        case Kind::Int:
            writeInt(value, flag);
            break;
        case Kind::Float:
            writeByte((m_version > 1 ? MarshalBinaryFloat : MarshalFloat) | flag);
            writeDouble(floatValue(value));
            break;
        case Kind::Complex:
            writeByte((m_version > 1 ? MarshalBinaryComplex : MarshalComplex) | flag);
            writeDouble(uncheckedDowncast<PyComplex>(value.asCell())->real());
            writeDouble(uncheckedDowncast<PyComplex>(value.asCell())->imaginary());
            break;
        case Kind::Bytes:
            writeByte(MarshalBytes | flag);
            writeSizedSpan(uncheckedDowncast<PyBytes>(value.asCell())->span());
            break;
        case Kind::Str:
            writeStr(asString(value), flag);
            break;
        case Kind::Tuple:
            writeTuple(uncheckedDowncast<PyTuple>(value.asCell()), flag);
            break;
        case Kind::List:
            writeList(asList(value), flag);
            break;
        case Kind::Dict:
        case Kind::FrozenDict:
            if (kind == Kind::FrozenDict && m_version < 6) {
                writeByte(MarshalUnknown);
                m_error = Error::Unmarshallable;
                break;
            }
            writeByte((kind == Kind::Dict ? MarshalDict : MarshalFrozenDict) | flag);
            writeDict(uncheckedDowncast<PyDict>(value.asCell()));
            break;
        case Kind::Set:
        case Kind::FrozenSet:
            writeByte((kind == Kind::Set ? MarshalSet : MarshalFrozenSet) | flag);
            writeSet(uncheckedDowncast<PySet>(value.asCell()));
            break;
        case Kind::Code:
            writeByte(MarshalBunCode | flag);
            if (!writeMarshalledCode(m_globalObject, PyCode::from(value), m_bytes))
                m_error = Error::Unmarshallable;
            break;
        case Kind::Buffer:
            writeBuffer(value, flag);
            break;
        case Kind::Slice:
            if (m_version < 5) {
                writeByte(MarshalUnknown);
                m_error = Error::Unmarshallable;
                break;
            }
            writeByte(MarshalSlice | flag);
            writeSlice(uncheckedDowncast<PySlice>(value.asCell()));
            break;
        case Kind::Unknown:
            writeByte(MarshalUnknown);
            m_error = Error::Unmarshallable;
            break;
        }

        if (flag && (kind == Kind::Slice || kind == Kind::FrozenDict))
            didComplete(key);
    }

    struct Number {
        uint32_t number;
        bool isComplete;
    };

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    int m_version;
    bool m_allowCode;
    Error m_error { Error::None };
    unsigned m_depth { 0 };
    Vector<uint8_t> m_bytes;
    KeyMap<bool> m_isRepeated;
    KeyMap<Number> m_numbers;
};

// ---- Reading ----

class MarshalReader {
public:
    // From memory.
    MarshalReader(JSGlobalObject* globalObject, std::span<const uint8_t> data, bool allowCode)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_data(data)
        , m_allowCode(allowCode)
    {
    }

    // From an object with a method readinto().
    MarshalReader(JSGlobalObject* globalObject, JSValue file, bool allowCode)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_file(file)
        , m_allowCode(allowCode)
    {
    }

    JSValue read()
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        m_references = newList(m_globalObject);
        JSValue result = readObject();
        RETURN_IF_EXCEPTION(scope, { });
        if (!result)
            RELEASE_AND_RETURN(scope, raiseTypeError(m_globalObject, scope, "NULL object in marshal data for object"_s));
        return result;
    }

private:
    // Valid until the next read. False: an exception.
    bool readBytes(size_t count, std::span<const uint8_t>& result)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_file) {
            if (m_data.size() < count) {
                raiseEOFError(m_globalObject, scope, "marshal data too short"_s);
                return false;
            }
            result = m_data.first(count);
            m_data = m_data.subspan(count);
            return true;
        }

        Vector<uint8_t> zeros;
        zeros.fill(0, count);
        PyByteArray* array = PyByteArray::create(m_globalObject, zeros.span());
        RETURN_IF_EXCEPTION(scope, false);
        JSValue view = call(m_globalObject, m_runtime.memoryviewType(), array);
        RETURN_IF_EXCEPTION(scope, false);
        JSValue answer = callMethod(m_globalObject, m_file, internString(vm, "readinto"_s), view);
        RETURN_IF_EXCEPTION(scope, false);
        int64_t read = toIndexAsInt64(answer);
        RETURN_IF_EXCEPTION(scope, false);
        callMethod(m_globalObject, view, internString(vm, "release"_s));
        RETURN_IF_EXCEPTION(scope, false);
        if (read != static_cast<int64_t>(count)) {
            if (read > static_cast<int64_t>(count))
                raiseValueError(m_globalObject, scope, "read() returned too much data: "_s, count, " bytes requested, "_s, read, " returned"_s);
            else
                raiseEOFError(m_globalObject, scope, "EOF read where not expected"_s);
            return false;
        }
        m_chunk = array;
        result = array->span();
        return true;
    }

    int64_t toIndexAsInt64(JSValue value)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue index = toIndex(m_globalObject, value);
        RETURN_IF_EXCEPTION(scope, 0);
        auto number = tryInt64(index);
        if (!number) {
            raiseValueError(m_globalObject, scope, "cannot fit '"_s, typeName(m_globalObject, value), "' into an index-sized integer"_s);
            return 0;
        }
        return *number;
    }

    // -1: an exception.
    int readByte()
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_file) {
            if (m_data.empty()) {
                raiseEOFError(m_globalObject, scope, "EOF read where not expected"_s);
                return -1;
            }
            int byte = m_data[0];
            m_data = m_data.subspan(1);
            return byte;
        }
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(1, bytes)))
            return -1;
        return bytes[0];
    }

    // With an exception, -1.
    int32_t readLong()
    {
        std::span<const uint8_t> bytes;
        if (!readBytes(4, bytes))
            return -1;
        return static_cast<int32_t>(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<uint32_t>(bytes[3]) << 24);
    }

    // A size that may not be negative. -1: an exception.
    int64_t readSize(ASCIILiteral what)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int32_t size = readLong();
        RETURN_IF_EXCEPTION(scope, -1);
        if (size < 0) {
            raiseValueError(m_globalObject, scope, "bad marshal data ("_s, what, " size out of range)"_s);
            return -1;
        }
        return size;
    }

    double readBinaryDouble()
    {
        std::span<const uint8_t> bytes;
        if (!readBytes(8, bytes))
            return -1;
        uint64_t bits = 0;
        for (unsigned i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        return std::bit_cast<double>(bits);
    }

    double readTextDouble()
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int length = readByte();
        RETURN_IF_EXCEPTION(scope, -1);
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(length, bytes)))
            return -1;
        // What follows a NUL is not looked at.
        if (size_t end = WTF::find(bytes, static_cast<uint8_t>(0)); end != notFound)
            bytes = bytes.first(end);
        auto number = parseDouble(byteCast<Latin1Character>(bytes));
        if (!number) {
            raiseValueError(m_globalObject, scope, "could not convert string to float: '"_s, String(byteCast<Latin1Character>(bytes)), '\'');
            return -1;
        }
        return *number;
    }

    JSValue readBigInt()
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int32_t size = readLong();
        RETURN_IF_EXCEPTION(scope, { });
        if (size == std::numeric_limits<int32_t>::min())
            RELEASE_AND_RETURN(scope, raiseValueError(m_globalObject, scope, "bad marshal data (long size out of range)"_s));
        bool isNegative = size < 0;
        size_t digits = isNegative ? -static_cast<int64_t>(size) : size;
        if (!digits)
            return jsNumber(0);
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(digits * 2, bytes)))
            return { };
        Vector<uint64_t, 8> words;
        words.fill(0, (digits * marshalDigitBits + 63) / 64);
        for (size_t i = 0; i < digits; ++i) {
            unsigned digit = bytes[2 * i] | bytes[2 * i + 1] << 8;
            if (digit >= 1u << marshalDigitBits)
                RELEASE_AND_RETURN(scope, raiseValueError(m_globalObject, scope, "bad marshal data (digit out of range in long)"_s));
            if (!digit && i == digits - 1)
                RELEASE_AND_RETURN(scope, raiseValueError(m_globalObject, scope, "bad marshal data (unnormalized long data)"_s));
            size_t bit = i * marshalDigitBits;
            words[bit / 64] |= static_cast<uint64_t>(digit) << (bit % 64);
            if (bit % 64 > 64 - marshalDigitBits)
                words[bit / 64 + 1] |= static_cast<uint64_t>(digit) >> (64 - bit % 64);
        }
        while (!words.isEmpty() && !words.last())
            words.removeLast();
        JSBigInt* result = JSBigInt::createFromWords(m_globalObject, words.span(), isNegative);
        RETURN_IF_EXCEPTION(scope, { });
        return normalizeInt(result);
    }

    // Gives the object the next number, if it is to have one.
    JSValue remember(JSValue object, bool isNumbered)
    {
        if (!object || !isNumbered)
            return object;
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        listAppend(m_globalObject, m_references, object);
        RETURN_IF_EXCEPTION(scope, { });
        return object;
    }

    // The same in two steps, for what cannot be referred to until it is complete.
    unsigned reserveNumber(bool isNumbered)
    {
        if (!isNumbered)
            return 0;
        listAppend(m_globalObject, m_references, jsUndefined());
        return listLength(m_references) - 1;
    }

    JSValue rememberAt(JSValue object, unsigned number, bool isNumbered)
    {
        if (object && isNumbered)
            listSet(m_globalObject, m_references, number, object);
        return object;
    }

    // There cannot be more items than there are bytes left. (CPython finds out by running out of memory or of data.)
    bool checkItemCount(int64_t count)
    {
        if (m_file || static_cast<uint64_t>(count) <= m_data.size())
            return true;
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseEOFError(m_globalObject, scope, "EOF read where object expected"_s);
        return false;
    }

    JSValue readStr(std::span<const uint8_t> bytes, bool isASCII, bool isInterned)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue result = isASCII ? codecDecodeLatin1(m_globalObject, bytes) : codecDecodeUTF8(m_globalObject, bytes, "surrogatepass"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (isInterned)
            RELEASE_AND_RETURN(scope, internString(m_globalObject, asString(result)));
        return result;
    }

    NEVER_INLINE JSValue readInt(bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int32_t number = readLong();
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, remember(jsNumber(number), isNumbered));
    }

    NEVER_INLINE JSValue readInt64(bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(8, bytes)))
            return { };
        uint64_t bits = 0;
        for (unsigned i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        JSValue number = makeInt(m_globalObject, static_cast<int64_t>(bits));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, remember(number, isNumbered));
    }

    NEVER_INLINE JSValue readBigIntObject(bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue number = readBigInt();
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, remember(number, isNumbered));
    }

    NEVER_INLINE JSValue readFloat(bool isText, bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        double number = isText ? readTextDouble() : readBinaryDouble();
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, remember(makeFloat(m_globalObject, number), isNumbered));
    }

    NEVER_INLINE JSValue readComplex(bool isText, bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        double real = isText ? readTextDouble() : readBinaryDouble();
        RETURN_IF_EXCEPTION(scope, { });
        double imaginary = isText ? readTextDouble() : readBinaryDouble();
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, remember(PyComplex::create(m_globalObject, real, imaginary), isNumbered));
    }

    NEVER_INLINE JSValue readBytesObject(bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int64_t size = readSize("bytes object"_s);
        RETURN_IF_EXCEPTION(scope, { });
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(size, bytes)))
            return { };
        RELEASE_AND_RETURN(scope, remember(PyBytes::create(m_globalObject, bytes), isNumbered));
    }

    NEVER_INLINE JSValue readStrObject(int type, bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        bool isShort = type == MarshalShortASCII || type == MarshalShortASCIIInterned;
        bool isInterned = type == MarshalASCIIInterned || type == MarshalShortASCIIInterned || type == MarshalInterned;
        int64_t size = isShort ? readByte() : readSize("string"_s);
        RETURN_IF_EXCEPTION(scope, { });
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(size, bytes)))
            return { };
        JSValue string = readStr(bytes, type != MarshalUnicode && type != MarshalInterned, isInterned);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, remember(string, isNumbered));
    }

    NEVER_INLINE JSValue readTuple(bool isSmall, bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int64_t size = isSmall ? readByte() : readSize("tuple"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, checkItemCount(size)))
            return { };
        PyTuple* tuple = size ? PyTuple::create(m_globalObject, static_cast<unsigned>(size)) : PyTuple::empty(m_globalObject);
        remember(tuple, isNumbered);
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 0; i < size; ++i) {
            JSValue item = readObject();
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                RELEASE_AND_RETURN(scope, raiseTypeError(m_globalObject, scope, "NULL object in marshal data for tuple"_s));
            tuple->initializeAt(vm, i, item);
        }
        return tuple;
    }

    NEVER_INLINE JSValue readList(bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int64_t size = readSize("list"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, checkItemCount(size)))
            return { };
        JSArray* list = newList(m_globalObject);
        remember(list, isNumbered);
        RETURN_IF_EXCEPTION(scope, { });
        for (int64_t i = 0; i < size; ++i) {
            JSValue item = readObject();
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                RELEASE_AND_RETURN(scope, raiseTypeError(m_globalObject, scope, "NULL object in marshal data for list"_s));
            listAppend(m_globalObject, list, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return list;
    }

    NEVER_INLINE JSValue readDict(bool isFrozen, bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        PyDict* dict = PyDict::create(m_globalObject);
        unsigned number = 0;
        if (isFrozen)
            number = reserveNumber(isNumbered);
        else
            remember(dict, isNumbered);
        RETURN_IF_EXCEPTION(scope, { });
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
        if (!isFrozen)
            return dict;
        // Nothing else has seen it.
        dict->setType(vm, m_runtime.frozendictType());
        RELEASE_AND_RETURN(scope, rememberAt(dict, number, isNumbered));
    }

    NEVER_INLINE JSValue readSet(bool isFrozen, bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int64_t size = readSize("set"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, checkItemCount(size)))
            return { };
        PySet* set = isFrozen ? PySet::createFrozen(m_globalObject) : PySet::create(m_globalObject);
        unsigned number = 0;
        if (isFrozen && size)
            number = reserveNumber(isNumbered);
        else
            remember(set, isNumbered);
        RETURN_IF_EXCEPTION(scope, { });
        for (int64_t i = 0; i < size; ++i) {
            JSValue item = readObject();
            RETURN_IF_EXCEPTION(scope, { });
            if (!item)
                RELEASE_AND_RETURN(scope, raiseTypeError(m_globalObject, scope, "NULL object in marshal data for set"_s));
            set->add(m_globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (isFrozen && size)
            RELEASE_AND_RETURN(scope, rememberAt(set, number, isNumbered));
        return set;
    }

    NEVER_INLINE JSValue readReference()
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int32_t number = readLong();
        RETURN_IF_EXCEPTION(scope, { });
        if (number < 0 || static_cast<unsigned>(number) >= listLength(m_references))
            RELEASE_AND_RETURN(scope, raiseValueError(m_globalObject, scope, "bad marshal data (invalid reference)"_s));
        JSValue object = listGet(m_references, number);
        if (isNone(object))
            RELEASE_AND_RETURN(scope, raiseValueError(m_globalObject, scope, "bad marshal data (invalid reference)"_s));
        return object;
    }

    NEVER_INLINE JSValue readSlice(bool isNumbered)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        unsigned number = reserveNumber(isNumbered);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue parts[3];
        for (auto& part : parts) {
            part = readObject();
            RETURN_IF_EXCEPTION(scope, { });
            if (!part)
                return { };
        }
        RELEASE_AND_RETURN(scope, rememberAt(PySlice::create(m_globalObject, parts[0], parts[1], parts[2]), number, isNumbered));
    }

    NEVER_INLINE JSValue readCode(bool isNumbered)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_allowCode)
            RELEASE_AND_RETURN(scope, raiseBadData("unmarshalling code objects is disallowed"_s));
        std::span<const uint8_t> bytes;
        if (failed(scope, readBytes(MarshalledCodeHeader::size, bytes)))
            return { };
        auto header = MarshalledCodeHeader::read(bytes.first<MarshalledCodeHeader::size>());
        if (!isOfThisBuild(header))
            RELEASE_AND_RETURN(scope, raiseBadData("bad marshal data (code object of another build)"_s));
        if (failed(scope, readBytes(header.payloadSize, bytes)))
            return { };
        PyCode* code = readMarshalledCode(m_globalObject, header, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (!code)
            RELEASE_AND_RETURN(scope, raiseBadData("bad marshal data (damaged code object)"_s));
        RELEASE_AND_RETURN(scope, remember(code, isNumbered));
    }

    NEVER_INLINE JSValue raiseBadData(ASCIILiteral message)
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        RELEASE_AND_RETURN(scope, raiseValueError(m_globalObject, scope, message));
    }

    // -1: an exception.
    NEVER_INLINE int readTypeCode()
    {
        VM& vm = m_runtime.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        int code = readByte();
        EXCEPTION_ASSERT(!!scope.exception() == (code < 0));
        if (code < 0 && clearPendingExceptionIfMatches(m_globalObject, m_runtime.EOFError()))
            raiseEOFError(m_globalObject, scope, "EOF read where object expected"_s);
        return code;
    }

    // Empty without an exception: the null object, which ends a dict.
    // Each kind has a function of its own to keep the frames of the recursion small: 2000 levels have to fit on the stack, in a
    // debug build too.
    JSValue readObject()
    {
        auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
        int code = readTypeCode();
        if (failed(scope, code >= 0))
            return { };
        if (m_depth >= marshalMaxDepth || !m_runtime.vm().isSafeToRecurseSoft())
            RELEASE_AND_RETURN(scope, raiseBadData("recursion limit exceeded"_s));
        ++m_depth;
        scope.release();
        JSValue result = readObjectOfType(code & ~marshalFlagRef, code & marshalFlagRef);
        --m_depth;
        return result;
    }

    JSValue readObjectOfType(int type, bool isNumbered)
    {
        switch (type) {
        case MarshalNull:
            return { };
        case MarshalNone:
            return jsUndefined();
        case MarshalStopIteration:
            return m_runtime.StopIteration();
        case MarshalEllipsis:
            return m_runtime.ellipsis();
        case MarshalFalse:
            return jsBoolean(false);
        case MarshalTrue:
            return jsBoolean(true);
        case MarshalInt:
            return readInt(isNumbered);
        case MarshalInt64:
            return readInt64(isNumbered);
        case MarshalLong:
            return readBigIntObject(isNumbered);
        case MarshalFloat:
        case MarshalBinaryFloat:
            return readFloat(type == MarshalFloat, isNumbered);
        case MarshalComplex:
        case MarshalBinaryComplex:
            return readComplex(type == MarshalComplex, isNumbered);
        case MarshalBytes:
            return readBytesObject(isNumbered);
        case MarshalASCIIInterned:
        case MarshalShortASCIIInterned:
        case MarshalInterned:
        case MarshalASCII:
        case MarshalShortASCII:
        case MarshalUnicode:
            return readStrObject(type, isNumbered);
        case MarshalSmallTuple:
        case MarshalTuple:
            return readTuple(type == MarshalSmallTuple, isNumbered);
        case MarshalList:
            return readList(isNumbered);
        case MarshalDict:
        case MarshalFrozenDict:
            return readDict(type == MarshalFrozenDict, isNumbered);
        case MarshalSet:
        case MarshalFrozenSet:
            return readSet(type == MarshalFrozenSet, isNumbered);
        case MarshalCode:
            return raiseBadData(m_allowCode ? "bad marshal data (the bytecode of CPython does not run here)"_s : "unmarshalling code objects is disallowed"_s);
        case MarshalBunCode:
            return readCode(isNumbered);
        case MarshalRef:
            return readReference();
        case MarshalSlice:
            return readSlice(isNumbered);
        default:
            return raiseBadData("bad marshal data (unknown type code)"_s);
        }
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    std::span<const uint8_t> m_data;
    JSValue m_file;
    bool m_allowCode;
    unsigned m_depth { 0 };
    // These objects live on the stack, which is what keeps the cells alive.
    JSArray* m_references { nullptr };
    PyByteArray* m_chunk { nullptr };
};

// ---- The module ----

static bool marshalOptions(JSGlobalObject* globalObject, JSValue versionValue, JSValue allowCodeValue, int& version, bool& allowCode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    version = marshalVersion;
    if (versionValue) {
        version = toCInt(globalObject, versionValue);
        RETURN_IF_EXCEPTION(scope, false);
    }
    allowCode = !allowCodeValue || isTrue(globalObject, allowCodeValue);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static JSValue marshalDumps(JSGlobalObject* globalObject, JSValue value, JSValue versionValue, JSValue allowCodeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int version;
    bool allowCode;
    if (failed(scope, marshalOptions(globalObject, versionValue, allowCodeValue, version, allowCode)))
        return { };
    if (failed(scope, audit(globalObject, "marshal.dumps"_s, value, jsNumber(version))))
        return { };
    MarshalWriter writer(globalObject, version, allowCode);
    if (failed(scope, writer.write(value)))
        return { };
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, writer.bytes().span()));
}

static JSValue marshalDump(JSGlobalObject* globalObject, JSValue value, JSValue file, JSValue versionValue, JSValue allowCodeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue bytes = marshalDumps(globalObject, value, versionValue, allowCodeValue);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, file, internString(vm, "write"_s), bytes));
}

static JSValue marshalLoads(JSGlobalObject* globalObject, JSValue bytes, JSValue allowCodeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(bytes)))
        return { };
    bool allowCode = !allowCodeValue || isTrue(globalObject, allowCodeValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (isAudited(globalObject) && failed(scope, audit(globalObject, "marshal.loads"_s, PyBytes::create(globalObject, holder.buffer.span()))))
        return { };
    MarshalReader reader(globalObject, holder.buffer.span(), allowCode);
    RELEASE_AND_RETURN(scope, reader.read());
}

// For the runtime's own use: nobody is told.
void appendItemsInMarshalOrder(JSGlobalObject* globalObject, PySet* set, Vector<JSValue, 8>& result)
{
    MarshalWriter writer(globalObject, marshalVersion, false);
    MarkedArgumentBuffer items;
    Vector<unsigned> order;
    bool isInOrder = writer.putInOrder(set, items, order);
    for (unsigned i = 0; i < items.size(); ++i)
        result.append(items.at(isInOrder ? order[i] : i));
}

JSValue readMarshalled(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    MarshalReader reader(globalObject, bytes, true);
    return reader.read();
}

static JSValue marshalLoad(JSGlobalObject* globalObject, JSValue file, JSValue allowCodeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool allowCode = !allowCodeValue || isTrue(globalObject, allowCodeValue);
    RETURN_IF_EXCEPTION(scope, { });
    // So that what is not a binary file is found out before anything is read.
    JSValue nothing = callMethod(globalObject, file, internString(vm, "read"_s), jsNumber(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyBytes::is(nothing))
        return raiseTypeError(globalObject, scope, "file.read() returned not bytes but "_s, typeName(globalObject, nothing));
    if (failed(scope, audit(globalObject, "marshal.load"_s)))
        return { };
    MarshalReader reader(globalObject, file, allowCode);
    RELEASE_AND_RETURN(scope, reader.read());
}

static constexpr NativeDef marshalFunctions[] = {
    PY_FUNCTION("dump", marshalDump, "(value, file, version=6, /, *, allow_code=True)"),
    PY_FUNCTION("load", marshalLoad, "(file, /, *, allow_code=True)"),
    PY_FUNCTION("dumps", marshalDumps, "(value, version=6, /, *, allow_code=True)"),
    PY_FUNCTION("loads", marshalLoads, "(bytes, /, *, allow_code=True)"),
};

void initializeMarshalModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, marshalFunctions);
    module->add(globalObject, "version"_s, jsNumber(marshalVersion));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
