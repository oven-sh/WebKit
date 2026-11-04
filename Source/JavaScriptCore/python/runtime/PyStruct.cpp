/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCodecs.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _struct. (Modules/_struct.c in CPython is the reference.)

// ---- The formats ----

enum class StructKind : uint8_t {
    Pad,
    Char,
    String,
    Pascal,
    Signed,
    Unsigned,
    Bool,
    Half,
    Float,
    Double,
    ComplexFloat,
    ComplexDouble,
    Pointer,
};

struct StructFormat {
    ASCIILiteral code;
    StructKind kind;
    // With '@', which is the default.
    uint8_t nativeSize;
    uint8_t nativeAlignment;
    // With '=', '<', '>' and '!', which do not align. 0: there is no such format then.
    uint8_t standardSize;
};

static constexpr StructFormat structFormats[] = {
    { "x"_s, StructKind::Pad, 1, 0, 1 },
    { "b"_s, StructKind::Signed, 1, 0, 1 },
    { "B"_s, StructKind::Unsigned, 1, 0, 1 },
    { "c"_s, StructKind::Char, 1, 0, 1 },
    { "s"_s, StructKind::String, 1, 0, 1 },
    { "p"_s, StructKind::Pascal, 1, 0, 1 },
    { "h"_s, StructKind::Signed, sizeof(short), alignof(short), 2 },
    { "H"_s, StructKind::Unsigned, sizeof(short), alignof(short), 2 },
    { "i"_s, StructKind::Signed, sizeof(int), alignof(int), 4 },
    { "I"_s, StructKind::Unsigned, sizeof(int), alignof(int), 4 },
    { "l"_s, StructKind::Signed, sizeof(long), alignof(long), 4 },
    { "L"_s, StructKind::Unsigned, sizeof(long), alignof(long), 4 },
    { "n"_s, StructKind::Signed, sizeof(size_t), alignof(size_t), 0 },
    { "N"_s, StructKind::Unsigned, sizeof(size_t), alignof(size_t), 0 },
    { "q"_s, StructKind::Signed, sizeof(long long), alignof(long long), 8 },
    { "Q"_s, StructKind::Unsigned, sizeof(long long), alignof(long long), 8 },
    { "?"_s, StructKind::Bool, sizeof(bool), alignof(bool), 1 },
    { "e"_s, StructKind::Half, 2, alignof(short), 2 },
    { "f"_s, StructKind::Float, 4, alignof(float), 4 },
    { "d"_s, StructKind::Double, 8, alignof(double), 8 },
    { "F"_s, StructKind::ComplexFloat, 8, alignof(float), 8 },
    { "D"_s, StructKind::ComplexDouble, 16, alignof(double), 16 },
    { "Zf"_s, StructKind::ComplexFloat, 8, alignof(float), 8 },
    { "Zd"_s, StructKind::ComplexDouble, 16, alignof(double), 16 },
    { "P"_s, StructKind::Pointer, sizeof(void*), alignof(void*), 0 },
};

// A run of items of one format.
struct StructCode {
    int64_t offset;
    int64_t size; // Of one item.
    int64_t repeat;
    uint8_t format; // An index in structFormats.
};

// What a format string compiles to. It is the content of a PyByteStorage.
struct StructLayout {
    int64_t size; // In bytes.
    int64_t length; // How many values are packed.
    uint32_t codeCount;
    bool isBigEndian;

    StructCode* codes() { return storageBehind<StructCode>(this); }
};

// ---- The object ----

enum StructField : unsigned {
    StructFormatString, // A str. Empty until the object has a format.
    StructLayoutStorage,
    StructInitCalled,
    StructFieldCount,
};

extern const TypeDef structTypeDef;
extern const TypeDef structIteratorTypeDef;
static constexpr TypeDef structErrorTypeDef { "struct.error"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };

static PyType* structType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(structTypeDef); }

static PyType* structError(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(structErrorTypeDef, rt.Exception());
}

template<typename... Parts>
static NEVER_INLINE JSValue structRaise(JSGlobalObject* globalObject, Parts... parts)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, structError(globalObject), makeString(parts...));
}

static ALWAYS_INLINE PyByteStorage* structStorage(PyRecord* object)
{
    JSValue storage = object->get(StructLayoutStorage);
    return storage && storage.isCell() ? uncheckedDowncast<PyByteStorage>(storage.asCell()) : nullptr;
}

static ALWAYS_INLINE StructLayout* structLayoutOf(PyByteStorage* storage) { return reinterpret_cast<StructLayout*>(storage->data()); }

// Null: a RuntimeError. What it returns must be kept alive for as long as the layout is looked at: __init__() may replace it.
static PyByteStorage* structReadyStorage(JSGlobalObject* globalObject, JSValue self)
{
    PyByteStorage* storage = structStorage(PyRecord::from(self));
    if (!storage) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseRuntimeError(globalObject, scope, "Struct object is not initialized"_s);
    }
    return storage;
}

// ---- Compiling a format ----

static const StructFormat* structFindFormat(std::span<const Latin1Character> text, bool isNative)
{
    for (auto& format : structFormats) {
        if (!isNative && !format.standardSize)
            continue;
        if (text.size() >= format.code.length() && !memcmp(text.data(), format.code.characters(), format.code.length()))
            return &format;
    }
    return nullptr;
}

// False: an exception.
static bool structCompile(JSGlobalObject* globalObject, PyRecord* object, JSString* formatString)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr int64_t maxSize = std::numeric_limits<int64_t>::max();

    String format = formatString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!format.containsOnlyASCII()) {
        raiseValueError(globalObject, scope, "non-ASCII character in struct format"_s);
        return false;
    }
    if (format.contains(static_cast<char16_t>(0))) {
        scope.release();
        structRaise(globalObject, "embedded null character"_s);
        return false;
    }
    if (!format.is8Bit())
        format = String::make8Bit(format.span16());
    std::span<const Latin1Character> text = format.span8();

    bool isNative = true;
    bool isBigEndian = false;
    if (!text.empty()) {
        switch (text[0]) {
        case '>':
        case '!':
            isBigEndian = true;
            [[fallthrough]];
        case '<':
        case '=':
            isNative = false;
            [[fallthrough]];
        case '@':
            text = text.subspan(1);
            break;
        default:
            break;
        }
    }

    auto tooLong = [&] {
        scope.release();
        structRaise(globalObject, "total struct size too long"_s);
        return false;
    };
    Vector<StructCode, 16> codes;
    int64_t size = 0;
    int64_t length = 0;
    while (!text.empty()) {
        Latin1Character character = text[0];
        if (character == ' ' || (character >= '\t' && character <= '\r')) {
            text = text.subspan(1);
            continue;
        }
        int64_t count = 1;
        if (isASCIIDigit(character)) {
            count = 0;
            while (!text.empty() && isASCIIDigit(text[0])) {
                int digit = text[0] - '0';
                if (count > (maxSize - digit) / 10)
                    return tooLong();
                count = count * 10 + digit;
                text = text.subspan(1);
            }
            if (text.empty()) {
                scope.release();
                structRaise(globalObject, "repeat count given without format specifier"_s);
                return false;
            }
        }
        const StructFormat* entry = structFindFormat(text, isNative);
        if (!entry) {
            scope.release();
            structRaise(globalObject, "bad char in struct format"_s);
            return false;
        }
        text = text.subspan(entry->code.length());

        bool isBytes = entry->kind == StructKind::String || entry->kind == StructKind::Pascal;
        if (entry->kind != StructKind::Pad) {
            int64_t values = isBytes ? 1 : count;
            if (values > maxSize - length)
                return tooLong();
            length += values;
        }
        int64_t itemSize = isNative ? entry->nativeSize : entry->standardSize;
        if (isNative && entry->nativeAlignment && size > 0) {
            int64_t padding = (entry->nativeAlignment - 1) - (size - 1) % entry->nativeAlignment;
            if (padding > maxSize - size)
                return tooLong();
            size += padding;
        }
        if (count > (maxSize - size) / itemSize)
            return tooLong();
        uint8_t index = static_cast<uint8_t>(entry - structFormats);
        if (isBytes)
            codes.append({ size, count, 1, index });
        else if (entry->kind != StructKind::Pad && count)
            codes.append({ size, itemSize, count, index });
        size += count * itemSize;
    }

    PyByteStorage* storage = PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), sizeof(StructLayout) + codes.size() * sizeof(StructCode));
    if (!storage) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    StructLayout* layout = structLayoutOf(storage);
    layout->size = size;
    layout->length = length;
    layout->codeCount = codes.size();
    layout->isBigEndian = isBigEndian;
    if (!codes.isEmpty())
        memcpy(layout->codes(), codes.span().data(), codes.size() * sizeof(StructCode));
    object->set(vm, StructLayoutStorage, storage);
    object->set(vm, StructFormatString, formatString);
    object->number = size;
    return true;
}

// From what is given for a format, a str or bytes.
static bool structSetFormat(JSGlobalObject* globalObject, PyRecord* object, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string;
    if (isStr(format))
        string = strValue(format);
    else if (PyBytes::is(format)) {
        JSValue decoded = codecDecodeASCII(globalObject, PyBytes::from(format)->span(), "surrogateescape"_s);
        RETURN_IF_EXCEPTION(scope, false);
        string = asString(decoded);
    } else {
        raiseTypeError(globalObject, scope, "Struct() argument 1 must be a str or bytes object, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, format)));
        return false;
    }
    RELEASE_AND_RETURN(scope, structCompile(globalObject, object, string));
}

// ---- Struct(format) ----

static PyRecord* structAllocate(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* object = PyRecord::create(vm, rt.structure<PyRecord>(), type, StructFieldCount);
    object->set(vm, StructFormatString, JSValue());
    object->set(vm, StructLayoutStorage, jsUndefined());
    object->set(vm, StructInitCalled, jsBoolean(false));
    object->number = -1;
    return object;
}

static bool structParseFormatArgument(JSGlobalObject* globalObject, const CallArgs& args, JSValue& format)
{
    static constexpr ASCIILiteral keywords[] = { "format"_s };
    return unpackKeywordArguments(globalObject, "Struct"_s, keywords, 1, 1, false, args, &format);
}

static bool structHasOwnInit(JSGlobalObject* globalObject, PyType* type)
{
    return type->lookup(Special::Init) != structType(globalObject)->lookup(Special::Init);
}

static JSValue structNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    JSValue format;
    if (type->lookup(Special::New) != structType(globalObject)->lookup(Special::New)) {
        // Called by the __new__() of a class derived from Struct.
        if (failed(scope, structParseFormatArgument(globalObject, args, format)))
            return { };
        PyRecord* object = structAllocate(globalObject, type);
        if (failed(scope, structSetFormat(globalObject, object, format)))
            return { };
        return object;
    }

    // Otherwise __init__() is about to be given the same arguments, and it is for that to complain about them.
    if (args.size() == 1 && !args.hasKeywords())
        format = args[0];
    else if (!args.size() && args.keywordCount() == 1)
        format = args.keyword("format"_s);
    bool hasOwnInit = structHasOwnInit(globalObject, type);
    if (!format && hasOwnInit) {
        unsigned count = args.size() + args.keywordCount();
        bool proceed = warn(globalObject, rt.DeprecationWarning(), count > 1 ? makeString("Struct() takes at most 1 argument ("_s, count, " given)"_s) : "Struct() missing required argument 'format' (pos 1)"_str);
        RETURN_IF_EXCEPTION(scope, { });
        if (!proceed)
            return { };
    }
    PyRecord* object = structAllocate(globalObject, type);
    if (format && failed(scope, structSetFormat(globalObject, object, format))) {
        if (!hasOwnInit)
            return { };
        JSValue exception = takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        JSString* text = str(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, { });
        String message = text->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        bool proceed = warn(globalObject, rt.DeprecationWarning(), makeString("Invalid 'format' argument for Struct.__new__(): "_s, message));
        RETURN_IF_EXCEPTION(scope, { });
        if (!proceed)
            return { };
    }
    return object;
}

static bool structIsSameFormat(JSGlobalObject* globalObject, PyRecord* object, JSValue format)
{
    String current = asString(object->get(StructFormatString))->value(globalObject);
    if (isStr(format))
        return strValue(format)->value(globalObject) == current;
    if (PyBytes::is(format))
        return current == String(byteCast<Latin1Character>(PyBytes::from(format)->span()));
    return false;
}

static JSValue structInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* object = PyRecord::from(self);
    bool wasCalled = object->get(StructInitCalled).asBoolean();
    if (!wasCalled && !structHasOwnInit(globalObject, object->type()) && object->get(StructFormatString)) {
        // Called by Struct(format), after __new__() did it all.
        object->set(vm, StructInitCalled, jsBoolean(true));
        return jsUndefined();
    }
    JSValue format;
    if (failed(scope, structParseFormatArgument(globalObject, args, format)))
        return { };
    if (object->get(StructFormatString)) {
        bool isSame = structIsSameFormat(globalObject, object, format);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame) {
            object->set(vm, StructInitCalled, jsBoolean(true));
            return jsUndefined();
        }
        bool proceed = warn(globalObject, rt.FutureWarning(), wasCalled
            ? "Re-initialization of Struct by calling the __init__() method will not work in future Python versions"_str
            : "Different format arguments for __new__() and __init__() methods of Struct"_str);
        RETURN_IF_EXCEPTION(scope, { });
        if (!proceed)
            return { };
    }
    if (failed(scope, structSetFormat(globalObject, object, format)))
        return { };
    object->set(vm, StructInitCalled, jsBoolean(true));
    return jsUndefined();
}

// ---- Unpacking ----

static ALWAYS_INLINE uint64_t structLoadBits(const uint8_t* address, unsigned size, bool isBigEndian)
{
    uint64_t bits = 0;
    if (isBigEndian) {
        for (unsigned i = 0; i < size; ++i)
            bits = (bits << 8) | address[i];
    } else {
        for (unsigned i = size; i--;)
            bits = (bits << 8) | address[i];
    }
    return bits;
}

static ALWAYS_INLINE void structStoreBits(uint8_t* address, unsigned size, bool isBigEndian, uint64_t bits)
{
    for (unsigned i = 0; i < size; ++i, bits >>= 8)
        address[isBigEndian ? size - 1 - i : i] = static_cast<uint8_t>(bits);
}

// In the order of this machine a C float goes through the floating point unit, which makes a signaling NaN a quiet one.
static ALWAYS_INLINE double structUnpackFloat(uint32_t bits, bool isBigEndian)
{
    return isBigEndian ? unpackFloat32(bits) : static_cast<double>(std::bit_cast<float>(bits));
}

static JSValue structUnpackItem(JSGlobalObject* globalObject, StructKind kind, const uint8_t* address, int64_t size, bool isBigEndian)
{
    switch (kind) {
    case StructKind::Char:
        return PyBytes::create(globalObject, std::span<const uint8_t>(address, 1));
    case StructKind::String:
        return PyBytes::create(globalObject, std::span<const uint8_t>(address, static_cast<size_t>(size)));
    case StructKind::Pascal: {
        // The first byte is the length.
        int64_t count = size ? std::min<int64_t>(*address, size - 1) : 0;
        return PyBytes::create(globalObject, std::span<const uint8_t>(address + 1, static_cast<size_t>(count)));
    }
    case StructKind::Signed: {
        uint64_t bits = structLoadBits(address, size, isBigEndian);
        unsigned unused = 64 - 8 * static_cast<unsigned>(size);
        return makeInt(globalObject, static_cast<int64_t>(bits << unused) >> unused);
    }
    case StructKind::Unsigned:
    case StructKind::Pointer:
        return makeInt(globalObject, structLoadBits(address, size, isBigEndian));
    case StructKind::Bool:
        return jsBoolean(!!structLoadBits(address, size, isBigEndian));
    case StructKind::Half:
        return makeFloatExactly(globalObject, unpackFloat16(static_cast<uint16_t>(structLoadBits(address, 2, isBigEndian))));
    case StructKind::Float:
        return makeFloatExactly(globalObject, structUnpackFloat(static_cast<uint32_t>(structLoadBits(address, 4, isBigEndian)), isBigEndian));
    case StructKind::Double:
        return makeFloatExactly(globalObject, std::bit_cast<double>(structLoadBits(address, 8, isBigEndian)));
    case StructKind::ComplexFloat:
        return PyComplex::create(globalObject, structUnpackFloat(static_cast<uint32_t>(structLoadBits(address, 4, isBigEndian)), isBigEndian), structUnpackFloat(static_cast<uint32_t>(structLoadBits(address + 4, 4, isBigEndian)), isBigEndian));
    case StructKind::ComplexDouble:
        return PyComplex::create(globalObject, std::bit_cast<double>(structLoadBits(address, 8, isBigEndian)), std::bit_cast<double>(structLoadBits(address + 8, 8, isBigEndian)));
    case StructKind::Pad:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// There are layout->size bytes at the address. No Python code runs, so they stay there.
static JSValue structUnpackAt(JSGlobalObject* globalObject, PyByteStorage* storage, const uint8_t* start)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    StructLayout* layout = structLayoutOf(storage);
    if (layout->length > std::numeric_limits<int32_t>::max())
        return raiseMemoryError(globalObject, scope, String());
    PyTuple* result = PyTuple::create(globalObject, static_cast<unsigned>(layout->length));
    unsigned index = 0;
    for (unsigned i = 0; i < layout->codeCount; ++i) {
        const StructCode& code = layout->codes()[i];
        StructKind kind = structFormats[code.format].kind;
        const uint8_t* address = start + code.offset;
        for (int64_t j = 0; j < code.repeat; ++j, address += code.size) {
            JSValue value = structUnpackItem(globalObject, kind, address, code.size, layout->isBigEndian);
            RETURN_IF_EXCEPTION(scope, { });
            result->initializeAt(vm, index++, value);
        }
    }
    ensureStillAliveHere(storage);
    return result;
}

static JSValue structUnpackImpl(JSGlobalObject* globalObject, JSValue self, JSValue buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(buffer)))
        return { };
    PyByteStorage* storage = structReadyStorage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!storage)
        return { };
    int64_t size = structLayoutOf(storage)->size;
    if (holder.buffer.length != size)
        RELEASE_AND_RETURN(scope, structRaise(globalObject, "unpack requires a buffer of "_s, size, " bytes"_s));
    RELEASE_AND_RETURN(scope, structUnpackAt(globalObject, storage, holder.buffer.data));
}

static JSValue structUnpackFromImpl(JSGlobalObject* globalObject, JSValue self, JSValue buffer, JSValue offsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(buffer)))
        return { };
    int64_t offset = 0;
    if (offsetValue && (failed(scope, ssizeArgument(globalObject, offsetValue, offset)) || failed(scope, refreshBuffer(globalObject, holder.buffer))))
        return { };
    PyByteStorage* storage = structReadyStorage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!storage)
        return { };
    int64_t size = structLayoutOf(storage)->size;
    int64_t available = holder.buffer.length;
    if (offset < 0) {
        if (offset + size > 0)
            RELEASE_AND_RETURN(scope, structRaise(globalObject, "not enough data to unpack "_s, size, " bytes at offset "_s, offset));
        if (offset + available < 0)
            RELEASE_AND_RETURN(scope, structRaise(globalObject, "offset "_s, offset, " out of range for "_s, available, "-byte buffer"_s));
        offset += available;
    }
    if (available - offset < size) {
        scope.release();
        return structRaise(globalObject, "unpack_from requires a buffer of at least "_s, static_cast<uint64_t>(size) + static_cast<uint64_t>(offset), " bytes for unpacking "_s, size,
            " bytes at offset "_s, offset, " (actual buffer size is "_s, available, ')');
    }
    RELEASE_AND_RETURN(scope, structUnpackAt(globalObject, storage, holder.buffer.data + offset));
}

// ---- The iterator of iter_unpack() ----

enum StructIteratorField : unsigned {
    StructIteratorStruct, // None once it is exhausted.
    StructIteratorLayout,
    StructIteratorView, // A memoryview, which keeps the buffer as it is.
    StructIteratorFieldCount,
};
// PyRecord::number is the offset of what comes next.

static JSValue structIterUnpackImpl(JSGlobalObject* globalObject, JSValue self, JSValue buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyByteStorage* storage = structReadyStorage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!storage)
        return { };
    int64_t size = structLayoutOf(storage)->size;
    if (!size)
        RELEASE_AND_RETURN(scope, structRaise(globalObject, "cannot iteratively unpack with a struct of length 0"_s));
    if (!hasBuffer(buffer))
        return raiseTypeError(globalObject, scope, "a bytes-like object is required, not '"_s, typeName(globalObject, buffer), '\'');
    JSValue viewValue = makeMemoryView(globalObject, buffer, BufferSimple);
    RETURN_IF_EXCEPTION(scope, { });
    auto* view = PyMemoryView::from(viewValue);
    if (view->length % size) {
        view->release(globalObject);
        RELEASE_AND_RETURN(scope, structRaise(globalObject, "iterative unpacking requires a buffer of a multiple of "_s, size, " bytes"_s));
    }
    PyRecord* iterator = PyRecord::create(globalObject, runtime(globalObject).typeFor(structIteratorTypeDef), StructIteratorFieldCount);
    iterator->set(vm, StructIteratorStruct, self);
    iterator->set(vm, StructIteratorLayout, storage);
    iterator->set(vm, StructIteratorView, view);
    return iterator;
}

static JSValue structIteratorSelf(JSGlobalObject*, JSValue self) { return self; }

static JSValue structIteratorLengthHint(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* iterator = PyRecord::from(self);
    if (isNone(iterator->get(StructIteratorStruct)))
        return jsNumber(0);
    int64_t size = structLayoutOf(uncheckedDowncast<PyByteStorage>(iterator->get(StructIteratorLayout).asCell()))->size;
    return makeInt(globalObject, (PyMemoryView::from(iterator->get(StructIteratorView))->length - iterator->number) / size);
}

static JSValue structIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    if (isNone(iterator->get(StructIteratorStruct)))
        return raiseStopIteration(globalObject, scope);
    auto* storage = uncheckedDowncast<PyByteStorage>(iterator->get(StructIteratorLayout).asCell());
    int64_t size = structLayoutOf(storage)->size;
    auto* view = PyMemoryView::from(iterator->get(StructIteratorView));
    if (iterator->number >= view->length) {
        iterator->set(vm, StructIteratorStruct, jsUndefined());
        iterator->set(vm, StructIteratorView, jsUndefined());
        view->release(globalObject);
        return raiseStopIteration(globalObject, scope);
    }
    const uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    const uint8_t* address = data + iterator->number;
    iterator->number += size;
    RELEASE_AND_RETURN(scope, structUnpackAt(globalObject, storage, address));
}

// ---- Packing ----

// The int that the value is, or that its __index__() returns. Empty: an exception.
static JSValue structToInt(JSGlobalObject* globalObject, JSValue value)
{
    if (isInt(value)) [[likely]]
        return intValue(value);
    if (!hasIndex(globalObject, value))
        return structRaise(globalObject, "required argument is not an integer"_s);
    return toIndex(globalObject, value);
}

static bool structPackItem(JSGlobalObject* globalObject, const StructFormat& format, uint8_t* address, unsigned size, bool isBigEndian, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Whatever went wrong is put this way.
    auto notA = [&](ASCIILiteral what) {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        scope.release();
        structRaise(globalObject, "required argument is not a "_s, what);
        return false;
    };
    auto tooLarge = [&](char code) {
        raiseOverflowError(globalObject, scope, "float too large to pack with "_s, code, " format"_s);
        return false;
    };
    // CPython takes an OverflowError out of __index__() for one of its own, but where it packs eight bytes in the other byte order.
    auto overflowMeansOutOfRange = [&] {
        return !(isBigEndian && size == 8) && clearPendingExceptionIfMatches(globalObject, rt.OverflowError());
    };
    switch (format.kind) {
    case StructKind::Char: {
        auto* bytes = PyBytes::tryFrom(value);
        if (!bytes || bytes->length() != 1) {
            scope.release();
            structRaise(globalObject, "char format requires a bytes object of length 1"_s);
            return false;
        }
        *address = bytes->data()[0];
        return true;
    }
    case StructKind::Signed: {
        JSValue integer = structToInt(globalObject, value);
        EXCEPTION_ASSERT(!!scope.exception() == !integer);
        if (!integer && !overflowMeansOutOfRange())
            return false;
        int64_t largest = static_cast<int64_t>(std::numeric_limits<uint64_t>::max() >> (65 - 8 * size));
        auto converted = integer ? tryInt64(integer) : std::nullopt;
        if (!converted || *converted > largest || *converted < ~largest) {
            scope.release();
            structRaise(globalObject, '\'', format.code, "' format requires "_s, ~largest, " <= number <= "_s, largest);
            return false;
        }
        structStoreBits(address, size, isBigEndian, static_cast<uint64_t>(*converted));
        return true;
    }
    case StructKind::Unsigned: {
        JSValue integer = structToInt(globalObject, value);
        EXCEPTION_ASSERT(!!scope.exception() == !integer);
        if (!integer && !overflowMeansOutOfRange())
            return false;
        uint64_t largest = std::numeric_limits<uint64_t>::max() >> (64 - 8 * size);
        auto converted = !integer || intSign(integer) < 0 ? std::nullopt : tryUInt64(integer);
        if (!converted || *converted > largest) {
            scope.release();
            structRaise(globalObject, '\'', format.code, "' format requires 0 <= number <= "_s, largest);
            return false;
        }
        structStoreBits(address, size, isBigEndian, *converted);
        return true;
    }
    case StructKind::Pointer: {
        JSValue integer = structToInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        uint64_t bits;
        if (intSign(integer) < 0) {
            auto converted = tryInt64(integer);
            if (!converted) {
                raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
                return false;
            }
            bits = static_cast<uint64_t>(*converted);
        } else {
            auto converted = tryUInt64(integer);
            if (!converted) {
                raiseOverflowError(globalObject, scope, "Python int too large to convert to C unsigned long"_s);
                return false;
            }
            bits = *converted;
        }
        structStoreBits(address, size, isBigEndian, bits);
        return true;
    }
    case StructKind::Bool: {
        bool truth = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        structStoreBits(address, size, isBigEndian, truth);
        return true;
    }
    case StructKind::Half:
    case StructKind::Float:
    case StructKind::Double: {
        double number = toDouble(globalObject, value);
        if (scope.exception()) [[unlikely]]
            return notA("float"_s);
        if (format.kind == StructKind::Double) {
            structStoreBits(address, 8, isBigEndian, std::bit_cast<uint64_t>(number));
            return true;
        }
        if (format.kind == StructKind::Half) {
            auto bits = packFloat16(number);
            if (!bits)
                return tooLarge('e');
            structStoreBits(address, 2, isBigEndian, *bits);
            return true;
        }
        auto bits = packFloat32(number);
        if (!bits)
            return tooLarge('f');
        structStoreBits(address, 4, isBigEndian, *bits);
        return true;
    }
    case StructKind::ComplexFloat:
    case StructKind::ComplexDouble: {
        double parts[2];
        if (failed(scope, complexArgument(globalObject, value, parts[0], parts[1])))
            return notA("complex"_s);
        for (unsigned i = 0; i < 2; ++i) {
            if (format.kind == StructKind::ComplexDouble) {
                structStoreBits(address + 8 * i, 8, isBigEndian, std::bit_cast<uint64_t>(parts[i]));
                continue;
            }
            // In the order of this machine it is a conversion in C, where what is too large is an infinity.
            auto bits = isBigEndian ? packFloat32(parts[i]) : std::bit_cast<uint32_t>(static_cast<float>(parts[i]));
            if (!bits)
                return tooLarge('f');
            structStoreBits(address + 4 * i, 4, isBigEndian, *bits);
        }
        return true;
    }
    case StructKind::Pad:
    case StructKind::String:
    case StructKind::Pascal:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// There is room for layout->size bytes at the address, which stays where it is whatever Python code runs. There are
// layout->length values. False: an exception.
static bool structPackAt(JSGlobalObject* globalObject, PyByteStorage* storage, std::span<const JSValue> values, uint8_t* start)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    StructLayout* layout = structLayoutOf(storage);
    memset(start, 0, layout->size);
    size_t index = 0;
    for (unsigned i = 0; i < layout->codeCount; ++i) {
        const StructCode& code = layout->codes()[i];
        const StructFormat& format = structFormats[code.format];
        uint8_t* address = start + code.offset;
        for (int64_t j = 0; j < code.repeat; ++j, address += code.size) {
            JSValue value = values[index++];
            if (format.kind == StructKind::String || format.kind == StructKind::Pascal) {
                bool isPascal = format.kind == StructKind::Pascal;
                std::span<const uint8_t> bytes;
                if (auto* object = PyBytes::tryFrom(value))
                    bytes = object->span();
                else if (auto* array = PyByteArray::tryFrom(value))
                    bytes = array->span();
                else {
                    scope.release();
                    structRaise(globalObject, "argument for '"_s, format.code, "' must be a bytes object"_s);
                    return false;
                }
                int64_t room = isPascal ? std::max<int64_t>(code.size - 1, 0) : code.size;
                int64_t count = std::min<int64_t>(bytes.size(), room);
                if (count > 0) {
                    memmove(address + isPascal, bytes.data(), count);
                    if (isPascal)
                        *address = static_cast<uint8_t>(std::min<int64_t>(count, 255));
                }
                continue;
            }
            if (failed(scope, structPackItem(globalObject, format, address, static_cast<unsigned>(code.size), layout->isBigEndian, value))) {
                if (isInt(value) && clearPendingExceptionIfMatches(globalObject, rt.OverflowError())) {
                    scope.release();
                    structRaise(globalObject, "int too large to convert"_s);
                }
                return false;
            }
        }
    }
    ensureStillAliveHere(storage);
    return true;
}

static JSValue structPackImpl(JSGlobalObject* globalObject, JSValue self, std::span<const JSValue> values)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyByteStorage* storage = structReadyStorage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!storage)
        return { };
    StructLayout* layout = structLayoutOf(storage);
    if (static_cast<int64_t>(values.size()) != layout->length)
        RELEASE_AND_RETURN(scope, structRaise(globalObject, "pack expected "_s, layout->length, " items for packing (got "_s, values.size(), ')'));
    // Of no size, it may still have values to look at ('0s').
    if (!layout->size && values.empty())
        RELEASE_AND_RETURN(scope, runtime(globalObject).emptyBytes());
    PyBytes* result = newBytes(globalObject, layout->size);
    if (failed(scope, result) || failed(scope, structPackAt(globalObject, storage, values, result->mutableData())))
        return { };
    return result;
}

// `position` is that of the buffer among the arguments.
static JSValue structPackIntoImpl(JSGlobalObject* globalObject, JSValue self, unsigned position, JSValue buffer, JSValue offsetValue, std::span<const JSValue> values)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(buffer, BufferWritable))) {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return raiseTypeError(globalObject, scope, "pack_into() argument "_s, position, " must be read-write bytes-like object, not "_s, typeNameOfArgument(globalObject, buffer));
    }
    int64_t offset;
    if (failed(scope, ssizeArgument(globalObject, offsetValue, offset)))
        return { };
    PyByteStorage* storage = structReadyStorage(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!storage)
        return { };
    StructLayout* layout = structLayoutOf(storage);
    if (static_cast<int64_t>(values.size()) != layout->length)
        RELEASE_AND_RETURN(scope, structRaise(globalObject, "pack_into expected "_s, layout->length, " items for packing (got "_s, values.size(), ')'));
    int64_t size = layout->size;
    int64_t available = holder.buffer.length;
    if (offset < 0) {
        if (offset + size > 0)
            RELEASE_AND_RETURN(scope, structRaise(globalObject, "no space to pack "_s, size, " bytes at offset "_s, offset));
        if (offset + available < 0)
            RELEASE_AND_RETURN(scope, structRaise(globalObject, "offset "_s, offset, " out of range for "_s, available, "-byte buffer"_s));
        offset += available;
    }
    if (available - offset < size) {
        scope.release();
        return structRaise(globalObject, "pack_into requires a buffer of at least "_s, static_cast<uint64_t>(size) + static_cast<uint64_t>(offset), " bytes for packing "_s, size,
            " bytes at offset "_s, offset, " (actual buffer size is "_s, available, ')');
    }
    if (failed(scope, refreshBuffer(globalObject, holder.buffer)))
        return { };
    if (size && failed(scope, structPackAt(globalObject, storage, values, holder.buffer.data + offset)))
        return { };
    return jsUndefined();
}

// ---- The methods of Struct ----

static JSValue structPack(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "Struct.pack"_s, args)))
        return { };
    RELEASE_AND_RETURN(scope, structPackImpl(globalObject, self, args.positional()));
}

static JSValue structPackInto(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "Struct.pack_into"_s, args)) || failed(scope, checkPositional(globalObject, "pack_into"_s, args, 2, std::numeric_limits<unsigned>::max())))
        return { };
    RELEASE_AND_RETURN(scope, structPackIntoImpl(globalObject, self, 1, args[0], args[1], args.positional().subspan(2)));
}

static JSValue structUnpack(JSGlobalObject* globalObject, JSValue self, JSValue buffer) { return structUnpackImpl(globalObject, self, buffer); }
static JSValue structUnpackFrom(JSGlobalObject* globalObject, JSValue self, JSValue buffer, JSValue offset) { return structUnpackFromImpl(globalObject, self, buffer, offset); }
static JSValue structIterUnpack(JSGlobalObject* globalObject, JSValue self, JSValue buffer) { return structIterUnpackImpl(globalObject, self, buffer); }

static JSValue structSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    PyByteStorage* storage = structReadyStorage(globalObject, self);
    if (!storage)
        return { };
    // As if each code took the 32 bytes it takes in CPython, and one more ended them.
    return makeInt(globalObject, static_cast<int64_t>(64 + 32 * (static_cast<int64_t>(structLayoutOf(storage)->codeCount) + 1)));
}

static JSValue structRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, structReadyStorage(globalObject, self)))
        return { };
    PyRecord* object = PyRecord::from(self);
    String format = asString(object->get(StructFormatString))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(object->type()->name(), '(', strRepr(format), ')'));
}

static JSValue structGetFormat(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue format = PyRecord::from(self)->get(StructFormatString))
        return format;
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString('\'', typeName(globalObject, self), "' object has no attribute 'format'"_s));
}

static JSValue structGetSize(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, PyRecord::from(self)->number); }

// ---- The functions of the module ----

// The dict of the Struct objects made for the functions of the module, by format. The type Struct holds it.
static PyDict* structCache(JSGlobalObject* globalObject)
{
    PyType* type = structType(globalObject);
    if (JSValue cache = hiddenField(globalObject, type, 0))
        return PyDict::from(cache);
    PyDict* cache = PyDict::create(globalObject);
    setHiddenField(globalObject, type, 0, cache);
    return cache;
}

// Struct(format), or the one made last time. Empty: an exception.
static JSValue structForFormat(JSGlobalObject* globalObject, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr unsigned maxCached = 100;
    JSValue key = format;
    if (PyBytes::is(format)) {
        key = codecDecodeASCII(globalObject, PyBytes::from(format)->span(), "surrogateescape"_s);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyDict* cache = structCache(globalObject);
    JSValue object = cache->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (object)
        return object;
    object = call(globalObject, structType(globalObject), key);
    RETURN_IF_EXCEPTION(scope, { });
    if (cache->size() >= maxCached)
        cache->clear(vm);
    cache->set(globalObject, key, object);
    if (scope.exception()) [[unlikely]] {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return object;
}

static JSValue structModuleClearCache(JSGlobalObject* globalObject)
{
    structCache(globalObject)->clear(getVM(globalObject));
    return jsUndefined();
}

static JSValue structModuleCalcSize(JSGlobalObject* globalObject, JSValue format)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue object = structForFormat(globalObject, format);
    if (failed(scope, object))
        return { };
    RELEASE_AND_RETURN(scope, structGetSize(globalObject, object));
}

static JSValue structModulePack(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "_struct.pack"_s, args)) || failed(scope, checkPositional(globalObject, "pack"_s, args, 1, std::numeric_limits<unsigned>::max())))
        return { };
    JSValue object = structForFormat(globalObject, args[0]);
    if (failed(scope, object))
        return { };
    RELEASE_AND_RETURN(scope, structPackImpl(globalObject, object, args.positional().subspan(1)));
}

static JSValue structModulePackInto(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "_struct.pack_into"_s, args)) || failed(scope, checkPositional(globalObject, "pack_into"_s, args, 3, std::numeric_limits<unsigned>::max())))
        return { };
    JSValue object = structForFormat(globalObject, args[0]);
    if (failed(scope, object))
        return { };
    RELEASE_AND_RETURN(scope, structPackIntoImpl(globalObject, object, 2, args[1], args[2], args.positional().subspan(3)));
}

static JSValue structModuleUnpack(JSGlobalObject* globalObject, JSValue format, JSValue buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue object = structForFormat(globalObject, format);
    if (failed(scope, object))
        return { };
    RELEASE_AND_RETURN(scope, structUnpackImpl(globalObject, object, buffer));
}

static JSValue structModuleUnpackFrom(JSGlobalObject* globalObject, JSValue format, JSValue buffer, JSValue offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue object = structForFormat(globalObject, format);
    if (failed(scope, object))
        return { };
    RELEASE_AND_RETURN(scope, structUnpackFromImpl(globalObject, object, buffer, offset));
}

static JSValue structModuleIterUnpack(JSGlobalObject* globalObject, JSValue format, JSValue buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue object = structForFormat(globalObject, format);
    if (failed(scope, object))
        return { };
    RELEASE_AND_RETURN(scope, structIterUnpackImpl(globalObject, object, buffer));
}

// ---- The types and the module ----

static constexpr NativeDef structMethods[] = {
    PY_NEW(structNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", structInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", structRepr, "($self, /)"),
    PY_METHOD("iter_unpack", structIterUnpack, "($self, buffer, /)"),
    PY_METHOD("pack", structPack, "($self, /, *values)"),
    PY_METHOD("pack_into", structPackInto, "($self, buffer, offset, /, *values)"),
    PY_METHOD("unpack", structUnpack, "($self, buffer, /)"),
    PY_METHOD("unpack_from", structUnpackFrom, "($self, /, buffer, offset=0)"),
    PY_METHOD("__sizeof__", structSizeOf, "($self, /)"),
};

static constexpr NativeGetSet structGetSets[] = {
    PY_MEMBER("format", structGetFormat),
    PY_MEMBER("size", structGetSize),
};

constexpr TypeDef structTypeDef { "_struct.Struct"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::HasWeakref }, structMethods, structGetSets,
    "Create a compiled struct object.\n\n"
    "Return a new Struct object which writes and reads binary data according\n"
    "to the format string.  See help(struct) for more on format strings."_s };

static constexpr NativeDef structIteratorMethods[] = {
    PY_SLOT("__iter__", structIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", structIteratorNext, "($self, /)"),
    PY_METHOD("__length_hint__", structIteratorLengthHint, "($self, /)"),
};

constexpr TypeDef structIteratorTypeDef { "_struct.unpack_iterator"_s, { TypeFlag::Immutable }, structIteratorMethods, { }, { } };

static constexpr NativeDef structFunctions[] = {
    PY_FUNCTION("_clearcache", structModuleClearCache, "()"),
    PY_FUNCTION("calcsize", structModuleCalcSize, "(format, /)"),
    PY_FUNCTION("iter_unpack", structModuleIterUnpack, "(format, buffer, /)"),
    PY_FUNCTION("pack", structModulePack, "(format, /, *values)"),
    PY_FUNCTION("pack_into", structModulePackInto, "(format, buffer, offset, /, *values)"),
    PY_FUNCTION("unpack", structModuleUnpack, "(format, buffer, /)"),
    PY_FUNCTION("unpack_from", structModuleUnpackFrom, "(format, /, buffer, offset=0)"),
};

void initializeStructModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "__doc__"_s, makeStr(getVM(globalObject),
        "Functions to convert between Python values and C structs.\n"
        "Python bytes objects are used to hold the data representing the C struct\n"
        "and also as format strings (explained below) to describe the layout of data\n"
        "in the C struct.\n\n"
        "The optional first format char indicates byte order, size and alignment:\n"
        "  @: native order, size & alignment (default)\n"
        "  =: native order, std. size & alignment\n"
        "  <: little-endian, std. size & alignment\n"
        "  >: big-endian, std. size & alignment\n"
        "  !: same as >\n\n"
        "The variable struct.error is an exception raised on errors.\n"_s));
    module->addFunctions(globalObject, structFunctions);
    module->add(globalObject, "Struct"_s, structType(globalObject));
    module->add(globalObject, "error"_s, structError(globalObject));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
