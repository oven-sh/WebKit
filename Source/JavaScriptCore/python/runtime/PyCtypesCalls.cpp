/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "CApiHooks.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "PyList.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

#if !OS(WINDOWS)
#include <dlfcn.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

// ---- CArgObject ----

static JSValue argRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCArg* argument = PyCArg::from(self);
    auto read = [&]<typename T>(T) {
        T value;
        memcpy(&value, argument->value, sizeof(T));
        return value;
    };
    auto withNumber = [&](int64_t number) { return makeStr(globalObject, makeString("<cparam '"_s, argument->tag, "' ("_s, number, ")>"_s)); };
    if (!argument->tag2) {
        switch (argument->tag) {
        case 'b':
        case 'B':
            return withNumber(read(static_cast<signed char>(0)));
        case 'h':
        case 'H':
            return withNumber(read(short { }));
        case 'i':
        case 'I':
            return withNumber(read(int { }));
        case 'l':
        case 'L':
            return withNumber(read(long { }));
        case 'q':
        case 'Q':
            return withNumber(read(static_cast<long long>(0)));
        case 'd':
        case 'f': {
            double number = argument->tag == 'f' ? read(float { }) : read(double { });
            return makeStr(globalObject, makeString("<cparam '"_s, argument->tag, "' ("_s, floatRepr(number), ")>"_s));
        }
        case 'c': {
            uint8_t character = argument->value[0];
            if (character < 128 && isASCIIPrintable(character) && character != '\\' && character != '\'')
                return makeStr(globalObject, makeString("<cparam 'c' ('"_s, static_cast<char>(character), "')>"_s));
            return makeStr(globalObject, makeString("<cparam 'c' ('\\x"_s, hex(character, 2, Lowercase), "')>"_s));
        }
        case 'z':
        case 'Z':
        case 'P': {
            uintptr_t pointer = std::bit_cast<uintptr_t>(argument->pointer());
            String text = pointer ? makeString("0x"_s, hex(pointer, Lowercase)) : "(nil)"_str;
            return makeStr(globalObject, makeString("<cparam '"_s, argument->tag, "' ("_s, text, ")>"_s));
        }
        default:
            break;
        }
    }
    String tag = argument->tag2 ? makeString(argument->tag, argument->tag2) : makeString(argument->tag);
    String address = addressForRepr(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString("<cparam '"_s, tag, "' at "_s, address, '>')));
}

static JSValue argObject(JSGlobalObject*, JSValue self)
{
    JSValue object = PyCArg::from(self)->object.get();
    return object ? object : none();
}

static constexpr NativeDef argMethods[] = {
    PY_SLOT("__repr__", argRepr, "($self, /)"),
};

static constexpr NativeGetSet argGetSets[] = {
    PY_MEMBER("_obj", argObject),
};

const TypeDef argTypeDef { "_ctypes.CArgObject"_s, { TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, argMethods, argGetSets, { } };

JSValue makeReference(JSGlobalObject* globalObject, JSValue object, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::tryFrom(object);
    if (!data)
        return raiseTypeError(globalObject, scope, "expected CData instance"_s);
    uint8_t* memory = data->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PyCArg* argument = PyCArg::create(globalObject);
    argument->tag = 'P';
    argument->foreignKind = ForeignKind::Pointer;
    argument->object.set(vm, argument, object);
    argument->isReference = true;
    argument->offset = offset;
    argument->setPointer(std::bit_cast<void*>(std::bit_cast<uintptr_t>(memory) + static_cast<uintptr_t>(offset)));
    return argument;
}

static void* addressOfReference(JSGlobalObject* globalObject, PyCArg* argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(argument->object.get());
    uint8_t* memory = data->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    void* address = std::bit_cast<void*>(std::bit_cast<uintptr_t>(memory) + static_cast<uintptr_t>(argument->offset));
    // As for an access through a pointer: within what the memory of the instance is a part of, or just behind it. Where nothing is known to own the memory, nothing can be said.
    if (argument->offset && data->owner() && !liesInMemoryOf(globalObject, data, address)) {
        raiseValueError(globalObject, scope, "byref() with an offset beyond the memory of the object"_s);
        return nullptr;
    }
    return address;
}

// What the _obj of a structure passed by value is. (Field 0: the copy. Field 1: the instance.)
static constexpr TypeDef structParamTypeDef { "_ctypes.StructParam_Type"_s, { TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, { }, { }, { } };

PyCArg* argumentFor(JSGlobalObject* globalObject, PyCData* data)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOfInstance(globalObject, data);
    ASSERT(info);
    auto memory = Place { data }.resolve(globalObject, info->size);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCArg* argument = PyCArg::create(globalObject);
    argument->object.set(vm, argument, data);
    switch (info->kind) {
    case TypeKind::None:
        RELEASE_ASSERT_NOT_REACHED();
    case TypeKind::Simple: {
        String code = stringOf(strValue(info->proto.get()));
        argument->tag = code[0];
        argument->tag2 = code.length() > 1 ? code[1] : 0;
        argument->foreignKind = info->foreignKind;
        memcpy(argument->value, memory.data(), std::min(memory.size(), sizeof(argument->value)));
        break;
    }
    case TypeKind::Pointer:
    case TypeKind::Function:
        argument->tag = 'P';
        argument->foreignKind = ForeignKind::Pointer;
        memcpy(argument->value, memory.data(), std::min(memory.size(), sizeof(void*)));
        break;
    case TypeKind::Array:
        argument->tag = 'P';
        argument->foreignKind = ForeignKind::Pointer;
        argument->isReference = true;
        argument->setPointer(memory.data());
        break;
    case TypeKind::Structure:
    case TypeKind::Union: {
        // By value: what is passed is what the instance holds now. The copy is bytes, not an instance: no __del__ is owed for it.
        Runtime& rt = runtime(globalObject);
        PyByteStorage* copy = PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), memory.size());
        if (!copy) {
            raiseMemoryError(globalObject, scope, String());
            return nullptr;
        }
        memcpy(copy->data(), memory.data(), memory.size());
        PyRecord* holder = PyRecord::create(globalObject, rt.typeFor(structParamTypeDef), 2);
        holder->set(vm, 0, copy);
        holder->set(vm, 1, data);
        argument->tag = 'V';
        argument->foreignKind = ForeignKind::Aggregate;
        argument->size = memory.size();
        argument->setPointer(copy->data());
        argument->object.set(vm, argument, holder);
        argument->keep.set(vm, argument, info);
        break;
    }
    }
    return argument;
}

// ---- The functions that Lib/ctypes calls by address ----

int& privateErrno()
{
    static thread_local int value;
    return value;
}

// They are never entered: what counts is that each has an address of its own.
extern "C" {
static void* ctypesStringAtEntry() { return std::bit_cast<void*>(&ctypesStringAtEntry); }
static void* ctypesWStringAtEntry() { return std::bit_cast<void*>(&ctypesWStringAtEntry); }
static void* ctypesMemoryViewAtEntry() { return std::bit_cast<void*>(&ctypesMemoryViewAtEntry); }
static void* ctypesCastEntry() { return std::bit_cast<void*>(&ctypesCastEntry); }
}

uintptr_t addressOfMemmove() { return std::bit_cast<uintptr_t>(&memmove); }
uintptr_t addressOfMemset() { return std::bit_cast<uintptr_t>(&memset); }
uintptr_t addressOfStringAt() { return std::bit_cast<uintptr_t>(&ctypesStringAtEntry); }
uintptr_t addressOfWStringAt() { return std::bit_cast<uintptr_t>(&ctypesWStringAtEntry); }
uintptr_t addressOfMemoryViewAt() { return std::bit_cast<uintptr_t>(&ctypesMemoryViewAtEntry); }
uintptr_t addressOfCast() { return std::bit_cast<uintptr_t>(&ctypesCastEntry); }

static JSValue stringAt(JSGlobalObject* globalObject, const char* pointer, int size)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    audit(globalObject, "ctypes.string_at"_s, intFromPointer(globalObject, pointer), jsNumber(size));
    RETURN_IF_EXCEPTION(scope, { });
    if (size < -1)
        return raiseSystemError(globalObject, scope, "Negative size passed to PyBytes_FromStringAndSize"_s);
    size_t length = size == -1 ? strlen(pointer) : static_cast<size_t>(size);
    PyBytes* result = newBytes(globalObject, length);
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(result->mutableData(), pointer, length);
    return result;
}

static JSValue wstringAt(JSGlobalObject* globalObject, const wchar_t* pointer, int size)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    audit(globalObject, "ctypes.wstring_at"_s, intFromPointer(globalObject, pointer), jsNumber(size));
    RETURN_IF_EXCEPTION(scope, { });
    if (size < -1)
        return raiseSystemError(globalObject, scope, "bad argument to internal function"_s);
    RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, pointer, size == -1 ? wcslen(pointer) : static_cast<size_t>(size)));
}

static JSValue memoryViewAt(JSGlobalObject* globalObject, void* pointer, int64_t size, bool isReadOnly)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    audit(globalObject, "ctypes.memoryview_at"_s, intFromPointer(globalObject, pointer), makeInt(globalObject, size), jsNumber(isReadOnly ? 1 : 0));
    RETURN_IF_EXCEPTION(scope, { });
    if (size < 0)
        return raiseValueError(globalObject, scope, "memoryview_at: size is negative (or overflowed): "_s, size);
    if (static_cast<uint64_t>(size) > MAX_ARRAY_BUFFER_SIZE)
        return raiseBufferError(globalObject, scope, "the memory is too large to be exported"_s);
    auto arrayBuffer = ArrayBuffer::createFromBytes(std::span<const uint8_t> { static_cast<const uint8_t*>(pointer), static_cast<size_t>(size) }, createSharedTask<void(void*)>([](void*) { }));
    JSCell* owner = JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTF::move(arrayBuffer));
    RELEASE_AND_RETURN(scope, PyMemoryView::createForMemory(globalObject, owner, 0, static_cast<size_t>(size), isReadOnly));
}

// isinstance(value, metaclass). It says which __init__ may have run, and a metaclass derived from two says yes twice: what the class is, the kind of its info says.
// So this decides who is turned away with which words, as in CPython, and never how many bytes are touched or what is read from an info.
static bool isTypeOfMeta(JSValue value, PyType* meta)
{
    PyType* type = PyType::tryFrom(value);
    return type && type->type()->isSubtypeOf(meta);
}

// `holders`: what the conversion of the arguments left to be kept alive for the call. The address was taken from one of them.
static JSValue castTo(JSGlobalObject* globalObject, void* pointer, JSValue source, JSValue type, const MarkedArgumentBuffer& holders)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);

    if (!isTypeOfMeta(type, state.metaType.get()))
        return raiseTypeError(globalObject, scope, "cast() argument 2 must be a pointer type"_s);
    bool isPointerType = isTypeOfMeta(type, state.pointerMeta.get()) || isTypeOfMeta(type, state.functionMeta.get());
    if (!isPointerType) {
        PyCTypeInfo* info = infoOf(globalObject, type);
        isPointerType = info && info->kind == TypeKind::Simple && info->size == sizeof(void*) && info->foreignKind == ForeignKind::Pointer;
    }
    if (!isPointerType)
        return raiseTypeError(globalObject, scope, "cast() argument 2 must be a pointer type, not "_s, PyType::is(type) ? PyType::from(type)->nameInMessages() : typeName(globalObject, type));

    // What the source is, its class says, not what it has kept so far. (In front of the call below, which runs code of the program. So have the conversions of the arguments, behind the
    // taking of the address: this is for what `_objects` looks like. What has to be kept does not go by it, see below.)
    PyCData* data = PyCData::tryFrom(source);
    Kept kept = Kept::ForWhole;
    if (data) {
        PyCTypeInfo* info = infoOfInstance(globalObject, data);
        if (info && info->kind != TypeKind::Simple)
            kept = Kept::ForPart;
    }

    // Who owns what the result will point into. That keeps it alive and says how far it goes, whatever `_objects` comes to look like.
    JSValue entry;
    for (unsigned i = 0; i < holders.size() && !entry; ++i)
        entry = entryForAddressFrom(globalObject, holders.at(i), pointer);

    JSValue resultValue = call(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    PyCData* result = PyCData::tryFrom(resultValue);
    if (!result)
        return raiseTypeError(globalObject, scope, "expected a ctypes instance, got "_s, typeName(globalObject, resultValue));

    // The result keeps alive what the source does, and the source. (Not the source if that is of a simple type and keeps something: then the result points where the value of the source
    // points, into what it keeps, and `_objects` is that, as in CPython.) A result that points into the source itself keeps it, whatever class the source has had at which moment, also where the memory of the source has gone meanwhile: by the source it is known that it has.
    if (data) {
        if (liesInMemoryOf(globalObject, data, pointer))
            kept = Kept::ForPart;
        keptObjects(globalObject, data, kept);
        if (!data->objects || isNone(data->objects.get()))
            data->objects.set(vm, data, PyDict::create(globalObject));
        result->objects.set(vm, result, data->objects.get());
        if (auto* dict = PyDict::tryFrom(result->objects.get()); dict && dict->type() == rt.dictType()) {
            JSValue identity = identityOf(globalObject, source);
            RETURN_IF_EXCEPTION(scope, { });
            dict->set(globalObject, identity, source);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    Home home;
    {
        auto memory = Place { result }.resolve(globalObject, sizeof(void*), &home);
        RETURN_IF_EXCEPTION(scope, { });
        memcpy(memory.data(), &pointer, memory.size());
    }
    setEntry(globalObject, home, entry);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// ---- Arguments ----

unsigned& AsParameterScope::depth()
{
    static thread_local unsigned value;
    return value;
}

bool AsParameterScope::isTooDeep(JSGlobalObject* globalObject) const
{
    VM& vm = getVM(globalObject);
    if (depth() <= 1000 && vm.isSafeToRecurseSoft()) [[likely]]
        return false;
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded while processing _as_parameter_"_s);
    return true;
}

// (As ForeignValue: all of it is written.)
PY_BEGIN_WITHOUT_PADDING
struct Argument {
    ForeignKind kind { ForeignKind::SInt32 };
    uint8_t unused[7] { };
    PyCTypeInfo* aggregate { nullptr };
    void* aggregateMemory { nullptr };
    size_t aggregateSize { 0 };
    JSValue object; // What a PyObject* is believed to refer to.
    PyCArg* reference { nullptr }; // It is a byref(): the value is yet to be worked out, see convertArguments().
    alignas(16) uint8_t value[32] { };

    template<typename T>
    T as() const
    {
        T result;
        memcpy(&result, value, sizeof(T));
        return result;
    }
    template<typename T>
    void set(T newValue) { memcpy(value, &newValue, sizeof(T)); }
};
PY_END_WITHOUT_PADDING

// `keeps` holds what has to live until the call is over. False: an exception.
static bool convertArgument(JSGlobalObject* globalObject, JSValue object, unsigned index, Argument& result, MarkedArgumentBuffer& keeps)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);

    PyCArg* argument = nullptr;
    if (PyCData* data = PyCData::tryFrom(object); data && infoOfInstance(globalObject, data)) {
        argument = argumentFor(globalObject, data);
        RETURN_IF_EXCEPTION(scope, false);
    } else
        argument = PyCArg::tryFrom(object);
    if (argument) {
        keeps.append(argument);
        result.kind = argument->foreignKind;
        memcpy(result.value, argument->value, sizeof(result.value));
        if (argument->isReference)
            result.reference = argument;
        if (argument->tag == 'O') {
            JSValue object = argument->object.get();
            auto* holder = PyCData::tryFrom(object);
            result.object = holder && identityBits(object) != result.as<uint64_t>() ? keptFor(globalObject, holder, 0) : object;
        }
        if (result.kind == ForeignKind::Aggregate) {
            result.aggregate = uncheckedDowncast<PyCTypeInfo>(argument->keep.get().asCell());
            result.aggregateMemory = argument->pointer();
            result.aggregateSize = static_cast<size_t>(argument->size);
        }
        return true;
    }
    if (isNone(object)) {
        result.kind = ForeignKind::Pointer;
        result.set<void*>(nullptr);
        return true;
    }
    if (isInt(object)) {
        JSValue exact = intValue(object);
        result.kind = ForeignKind::SInt32;
        if (!tryUInt64(exact) && !tryInt64(exact)) {
            raiseOverflowError(globalObject, scope, "int too long to convert"_s);
            return false;
        }
        result.set<int>(static_cast<int>(lowBitsOfInt(exact)));
        return true;
    }
    if (auto* bytes = PyBytes::tryFrom(object)) {
        keeps.append(object);
        result.kind = ForeignKind::Pointer;
        result.set<const void*>(bytes->data());
        return true;
    }
    if (isStr(object)) {
        size_t length;
        PyByteStorage* storage = wideCharactersOf(globalObject, strValue(object), length);
        RETURN_IF_EXCEPTION(scope, false);
        keeps.append(storage);
        result.kind = ForeignKind::Pointer;
        result.set<void*>(storage->data());
        return true;
    }
    JSValue asParameter = getAttrIfPresent(globalObject, object, state.asParameterName.get());
    RETURN_IF_EXCEPTION(scope, false);
    if (asParameter) {
        AsParameterScope nesting;
        if (failed(scope, !nesting.isTooDeep(globalObject)))
            return false;
        RELEASE_AND_RETURN(scope, convertArgument(globalObject, asParameter, index, result, keeps));
    }
    raiseTypeError(globalObject, scope, "Don't know how to convert parameter "_s, index);
    return false;
}

// ---- Results ----

static bool isLoadedAsObject(PyCTypeInfo* info, JSValue type)
{
    return info && info->accessor.code == Code::Object && isSimpleInstanceType(*info->state.get(), PyType::from(type));
}

static void takeOverReferenceIn(const uint8_t* memory, bool programGetsObject)
{
    uint64_t bits;
    memcpy(&bits, memory, sizeof(bits));
    CApi::takeOverReference(bits, programGetsObject);
}

// What becomes of the reference that a result which is an object comes with. (As wide as a word: see ResultOfCall.)
enum class ReferenceOfResult : uintptr_t { None, ForProgram, Dropped };

// `memory`: the room that there is. `size`: so much of it was made for a result that is an instance. (The type may have been given its fields during the call.)
static NEVER_INLINE JSValue makeResult(JSGlobalObject* globalObject, JSValue resultType, std::span<const uint8_t> memory, size_t size, JSValue checker, JSValue hint, ReferenceOfResult reference)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number;
    memcpy(&number, memory.data(), sizeof(number));
    if (!resultType)
        return jsNumber(number);
    if (isNone(resultType))
        return none();
    PyCTypeInfo* info = infoOf(globalObject, resultType);
    if (!info)
        RELEASE_AND_RETURN(scope, call(globalObject, resultType, jsNumber(number)));
    JSValue result;
    if (info->accessor && isSimpleInstanceType(*info->state.get(), PyType::from(resultType))) {
        result = load(globalObject, info->accessor, memory.first(std::min<size_t>(info->size, memory.size())), { }, hint);
        // (Also if the bits are not those of an object that the program may have.)
        if (reference != ReferenceOfResult::None)
            takeOverReferenceIn(memory.data(), result && reference == ReferenceOfResult::ForProgram);
    } else
        result = dataFromCopy(globalObject, PyType::from(resultType), memory.first(size));
    RETURN_IF_EXCEPTION(scope, { });
    if (!checker)
        return result;
    RELEASE_AND_RETURN(scope, call(globalObject, checker, result));
}

// ---- The call ----

static bool isOwnFunction(uintptr_t address)
{
    return address == addressOfMemmove() || address == addressOfMemset() || address == addressOfCast() || address == addressOfMemoryViewAt() || address == addressOfStringAt() || address == addressOfWStringAt();
}

// Runs one of them. The result is in `memory`, unless there is an exception.
static NEVER_INLINE void callOwnFunction(JSGlobalObject* globalObject, uintptr_t address, std::span<const Argument> arguments, const MarkedArgumentBuffer& keeps, uint8_t* memory, JSValue& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(isOwnFunction(address));
    unsigned expected = address == addressOfStringAt() || address == addressOfWStringAt() ? 2 : 3;
    bool fits = arguments.size() == expected;
    for (auto& argument : arguments)
        fits &= argument.kind == ForeignKind::Pointer || (argument.kind >= ForeignKind::SInt8 && argument.kind <= ForeignKind::UInt64);
    if (!fits) {
        raiseTypeError(globalObject, scope, "this function takes "_s, expected, " arguments that are integers or pointers"_s);
        return;
    }
    // An int is passed as 32 bits. A size_t or a pointer has all 64.
    auto wide = [&](unsigned index) -> int64_t {
        const Argument& argument = arguments[index];
        switch (argument.kind) {
        case ForeignKind::SInt8:
            return argument.as<int8_t>();
        case ForeignKind::UInt8:
            return argument.as<uint8_t>();
        case ForeignKind::SInt16:
            return argument.as<int16_t>();
        case ForeignKind::UInt16:
            return argument.as<uint16_t>();
        case ForeignKind::SInt32:
            return argument.as<int32_t>();
        case ForeignKind::UInt32:
            return argument.as<uint32_t>();
        default:
            return argument.as<int64_t>();
        }
    };
    auto pointer = [&](unsigned index) { return std::bit_cast<void*>(static_cast<uintptr_t>(wide(index))); };
    auto object = [&](unsigned index) -> JSValue { return objectFromBits(globalObject, static_cast<uint64_t>(wide(index)), arguments[index].object); };

    if (address == addressOfMemmove() || address == addressOfMemset()) {
        void* destination = pointer(0);
        if (address == addressOfMemmove())
            memmove(destination, pointer(1), static_cast<size_t>(wide(2)));
        else
            memset(destination, static_cast<int>(wide(1)), static_cast<size_t>(wide(2)));
        memcpy(memory, &destination, sizeof(void*));
        return;
    }
    if (address == addressOfStringAt())
        result = stringAt(globalObject, static_cast<const char*>(pointer(0)), static_cast<int>(wide(1)));
    else if (address == addressOfWStringAt())
        result = wstringAt(globalObject, static_cast<const wchar_t*>(pointer(0)), static_cast<int>(wide(1)));
    else if (address == addressOfMemoryViewAt())
        result = memoryViewAt(globalObject, pointer(0), wide(1), !!wide(2));
    else {
        JSValue source = object(1);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue type = object(2);
        RETURN_IF_EXCEPTION(scope, void());
        result = castTo(globalObject, pointer(0), source, type, keeps);
    }
    RETURN_IF_EXCEPTION(scope, void());
    uint64_t bits = identityBits(result);
    memcpy(memory, &bits, sizeof(bits));
}

// Room for what a call is given: in the frame for up to eight. All eight are made with it, so that there is nothing in the frame that is not
// written to: see callFunction().
template<typename T>
class RoomForArguments {
    WTF_MAKE_NONCOPYABLE(RoomForArguments);
public:
    RoomForArguments() = default;

    std::span<T> take(unsigned count)
    {
        if (count <= m_few.size()) [[likely]]
            return std::span { m_few }.first(count);
        m_more.grow(count);
        return m_more.mutableSpan();
    }

private:
    std::array<T, 8> m_few { };
    Vector<T> m_more;
};

// (What raises has frames of its own: the texts are put together in room that a call that goes well never writes to.)
static NEVER_INLINE JSValue raiseNoMemoryForResult(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseMemoryError(globalObject, scope, String());
}

static NEVER_INLINE JSValue raiseNullPointerAccess(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseValueError(globalObject, scope, "NULL pointer access"_s);
}

// False: an exception.
static NEVER_INLINE bool convertArguments(JSGlobalObject* globalObject, void* function, PyTuple* arguments, PyTuple* converters, RoomForArguments<Argument>& room, std::span<Argument>& converted, MarkedArgumentBuffer& keeps)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);

    audit(globalObject, "ctypes.call_function"_s, intFromPointer(globalObject, function), arguments);
    RETURN_IF_EXCEPTION(scope, false);
    unsigned count = arguments->length();
    if (count > 1024) {
        raise(globalObject, scope, state.argumentError.get(), makeString("too many arguments ("_s, count, "), maximum is 1024"_s));
        return false;
    }
    unsigned converterCount = converters ? converters->length() : 0;
    converted = room.take(count);
    auto blameArgument = [&](unsigned i) {
        scope.release();
        // Where the stack is at its end nothing can be converted, not even by from_param() of c_long, which is called like any function. That is no fault of the argument, and whoever is
        // there to catch a RecursionError is to get one. (What an _as_parameter_ or a from_param() of the program's raises with room to spare is an ArgumentError, as in CPython.)
        if (!isSafeToRecurseWithMargin(vm, 16 * KB) && pendingExceptionMatches(globalObject, runtime(globalObject).RecursionError())) [[unlikely]]
            return false;
        extendError(globalObject, state.argumentError.get(), makeString("argument "_s, i + 1, ": "_s));
        return false;
    };
    for (unsigned i = 0; i < count; ++i) {
        JSValue argument = arguments->at(i);
        if (i < converterCount) {
            argument = call(globalObject, converters->at(i), argument);
            if (scope.exception()) [[unlikely]]
                return blameArgument(i);
            keeps.append(argument);
        }
        convertArgument(globalObject, argument, i + 1, converted[i], keeps);
        if (scope.exception()) [[unlikely]]
            return blameArgument(i);
    }
    // Behind all that runs code of the program, which may move the memory of an instance or take it away.
    for (unsigned i = 0; i < count; ++i) {
        if (PyCArg* reference = converted[i].reference) {
            void* address = addressOfReference(globalObject, reference);
            if (scope.exception()) [[unlikely]]
                return blameArgument(i);
            converted[i].set(address);
        }
    }
    return true;
}

// The function may wait for as long as it likes, and what earlier frames left in room of this frame that nothing is written to is under the eyes of
// the collector until it returns. So the room for the arguments is written whether there are so many or not, and what comes before and after
// the call has frames of its own.
JSValue callFunction(JSGlobalObject* globalObject, void* function, PyTuple* arguments, unsigned flags, JSValue convertersValue, JSValue resultType, JSValue checker, ResultOfCall resultOfCall)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* converters = PyTuple::tryFrom(convertersValue);
    unsigned converterCount = converters ? converters->length() : 0;
    RoomForArguments<Argument> roomForConverted;
    std::span<Argument> converted;
    MarkedArgumentBuffer keeps;
    if (failed(scope, convertArguments(globalObject, function, arguments, converters, roomForConverted, converted, keeps)))
        return { };
    unsigned count = converted.size();

    ForeignValue result;
    PyCTypeInfo* resultInfo = resultType && !isNone(resultType) ? infoOf(globalObject, resultType) : nullptr;
    if (resultType && isNone(resultType))
        result.kind = ForeignKind::Void;
    else if (!resultInfo)
        result.kind = ForeignKind::SInt32;
    else {
        result.kind = resultInfo->foreignKind;
        result.aggregate = resultInfo->foreignKind == ForeignKind::Aggregate ? resultInfo : nullptr;
    }
    // In memory of the collector if it is large: a structure may be of any size.
    alignas(16) uint8_t smallResult[64] { };
    std::span<uint8_t> resultMemory { smallResult };
    int64_t resultSize = resultInfo ? resultInfo->size : 0;
    if (resultSize > static_cast<int64_t>(sizeof(smallResult))) {
        Runtime& rt = runtime(globalObject);
        PyByteStorage* storage = PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), static_cast<size_t>(resultSize));
        if (!storage)
            RELEASE_AND_RETURN(scope, raiseNoMemoryForResult(globalObject));
        keeps.append(storage);
        resultMemory = { storage->data(), static_cast<size_t>(resultSize) };
        memset(resultMemory.data(), 0, resultMemory.size());
    }
    result.memory = resultMemory.data();

    JSValue ownResult;
    bool returnsReference = false;
    if (isOwnFunction(std::bit_cast<uintptr_t>(function))) {
        callOwnFunction(globalObject, std::bit_cast<uintptr_t>(function), converted, keeps, resultMemory.data(), ownResult);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        RoomForArguments<ForeignValue> roomForValues;
        std::span<ForeignValue> values = roomForValues.take(count);
        for (unsigned i = 0; i < count; ++i) {
            values[i].kind = converted[i].kind;
            values[i].aggregate = converted[i].aggregate;
            values[i].memory = converted[i].kind == ForeignKind::Aggregate ? converted[i].aggregateMemory : converted[i].value;
            values[i].size = converted[i].aggregateSize;
        }
        if (!function)
            RELEASE_AND_RETURN(scope, raiseNullPointerAccess(globalObject));
        Runtime& rt = runtime(globalObject);
        bool returnsObject = isLoadedAsObject(resultInfo, resultType);
        bool callbackGivesReference = false;
        bool isCallback = (returnsObject || ((flags & FunctionPythonAPI) && !CApi::isSetUpFor(rt))) && isCallbackCode(function, callbackGivesReference);
        // A function of a PyDLL may call the C API, if it is C's: the prototype of a callback has the flag too.
        if ((flags & FunctionPythonAPI) && !isCallback && failed(scope, CApi::ensure(globalObject)))
            return { };
        // Code that returns an object returns a reference with it. C's does, and so does a callback, if its interpreter counts (runCallback()). What counts there is the prototype that the
        // callback was made with, not the one that it is called by here: if that says "a number", no reference comes with the number, whatever this caller takes it for.
        returnsReference = returnsObject && (isCallback ? callbackGivesReference : CApi::tryEnsure(globalObject));
        // With more arguments than argtypes, the rest are those of a variadic function.
        unsigned fixedCount = converterCount && count > converterCount ? converterCount : count;
        {
            CApi::CRunsFor running(rt);
            callForeignFunction(globalObject, function, values, fixedCount, result, flags);
        }
        // However the call ends, neither the reference nor what C raised outlives it. (For an interpreter that the API does not serve, C can raise nothing.)
        bool isEnded = scope.exception(); // The host is ending what runs here, or no such call can be made.
        bool isServed = CApi::isSetUpFor(rt);
        if (isEnded && isServed)
            CApi::forgetWhatCRaised();
        if (isEnded || (isServed && failed(scope, CApi::throwWhatCRaised(globalObject)))) [[unlikely]] {
            if (returnsReference)
                takeOverReferenceIn(resultMemory.data(), false);
            return { };
        }
    }
    RELEASE_AND_RETURN(scope, makeResult(globalObject, resultType, resultMemory, static_cast<size_t>(resultSize), checker, ownResult, !returnsReference ? ReferenceOfResult::None : resultOfCall == ResultOfCall::Dropped ? ReferenceOfResult::Dropped : ReferenceOfResult::ForProgram));
}

// ---- Callbacks ----

static PyCThunk* makeThunk(JSGlobalObject* globalObject, JSValue callable, JSValue argTypesValue, JSValue resultType, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyTuple* argTypes = tupleFromIterable(globalObject, argTypesValue);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCThunk* thunk = PyCThunk::create(vm, state.thunkStructure.get(), state.thunkType.get());
    thunk->flags = flags;
    thunk->callable.set(vm, thunk, callable);
    thunk->argTypes.set(vm, thunk, argTypes);
    thunk->globalObject = globalObject;
    if (resultType && !isNone(resultType)) {
        PyCTypeInfo* info = infoOf(globalObject, resultType);
        if (!info || !info->accessor) {
            raiseTypeError(globalObject, scope, "invalid result type for callback function"_s);
            return nullptr;
        }
        thunk->resultType.set(vm, thunk, resultType);
        thunk->givesReference = info->accessor.code == Code::Object && CApi::isFor(runtime(globalObject));
    }
    if (failed(scope, allocateCallbackCode(globalObject, thunk)))
        return nullptr;
    runtime(globalObject).registerFinalizable(thunk);
    return thunk;
}

static void reportCallbackError(JSGlobalObject* globalObject, const String& what, JSValue callable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue exception = takePendingException(globalObject)) {
        String text = reprForMessage(globalObject, callable);
        raiseException(globalObject, scope, exception);
        reportUnraisable(globalObject, makeString("Exception ignored while "_s, what, " ctypes callback function "_s, text), { });
    }
    // Nothing propagates through the frames of C.
    dropPendingException(globalObject, scope);
}

void runCallback(PyCThunk* thunk, void** arguments, void* resultMemory)
{
    JSGlobalObject* globalObject = thunk->globalObject;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue callable = thunk->callable.get();
    PyTuple* argTypes = thunk->argTypes.get();

    MarkedArgumentBuffer values;
    for (unsigned i = 0; i < argTypes->length(); ++i) {
        JSValue type = argTypes->at(i);
        PyCTypeInfo* info = infoOf(globalObject, type);
        JSValue value;
        // What C passes is a register's worth at most: an array comes as the pointer to it.
        std::span<const uint8_t> passed { static_cast<const uint8_t*>(arguments[i]), info ? std::min<size_t>(info->size, sizeof(uint64_t)) : 0 };
        if (info && info->accessor && isSimpleInstanceType(*info->state.get(), PyType::from(type)))
            value = load(globalObject, info->accessor, passed);
        else if (info) {
            value = call(globalObject, type);
            if (!scope.exception()) {
                PyCData* data = PyCData::tryFrom(value);
                if (!data || data->size < info->size) {
                    String text = reprForMessage(globalObject, type);
                    if (!scope.exception())
                        raiseTypeError(globalObject, scope, text, " returned unexpected result of type "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)));
                } else if (auto memory = Place { data }.resolve(globalObject, passed.size()); memory.data())
                    memcpy(memory.data(), passed.data(), memory.size());
            }
        } else {
            String text = reprForMessage(globalObject, type);
            if (!scope.exception())
                raiseTypeError(globalObject, scope, "cannot build parameter of type "_s, text);
        }
        if (scope.exception()) [[unlikely]] {
            scope.release();
            reportCallbackError(globalObject, makeString("creating argument "_s, i, " for"_s), callable);
            return;
        }
        values.append(value);
    }

    bool swapsErrno = thunk->flags & FunctionUseErrno;
    if (swapsErrno)
        std::swap(errno, privateErrno());
    Vector<JSValue, 8> plain;
    for (unsigned i = 0; i < values.size(); ++i)
        plain.append(values.at(i));
    JSValue result = call(globalObject, callable, CallArgs(plain.span()));
    if (scope.exception()) [[unlikely]] {
        reportCallbackError(globalObject, "calling"_s, callable);
        scope.assertNoException();
        result = { };
    }
    if (swapsErrno)
        std::swap(errno, privateErrno());

    if (!result || !thunk->resultType)
        return;
    PyCTypeInfo* info = infoOf(globalObject, thunk->resultType.get());
    Packed packed;
    pack(globalObject, info->accessor, result, info->size, packed);
    if (scope.exception()) [[unlikely]] {
        scope.release();
        reportCallbackError(globalObject, "converting result of"_s, callable);
        return;
    }
    store({ static_cast<uint8_t*>(resultMemory), static_cast<size_t>(info->size) }, info->accessor, packed);
    if (thunk->givesReference && CApi::tryEnsure(globalObject)) {
        uint64_t bits;
        memcpy(&bits, resultMemory, sizeof(bits));
        CApi::giveReference(bits);
    }
    // C now has a pointer into it, for nobody knows how long: it is never let go. That is what CPython does, and warns of.
    if (info->accessor.code != Code::Object && !isNone(packed.keep)) {
        holdObject(globalObject, packed.keep);
        if (!scope.exception())
            warn(globalObject, runtime(globalObject).RuntimeWarning(), "memory leak in callback function."_s);
        if (scope.exception()) [[unlikely]] {
            scope.release();
            reportCallbackError(globalObject, "converting result of"_s, callable);
        }
    }
}

// ---- CFuncPtr ----

static bool isOutArgumentType(JSGlobalObject* globalObject, State& state, JSValue type, unsigned index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isTypeOfMeta(type, state.pointerMeta.get()) || isTypeOfMeta(type, state.arrayMeta.get()))
        return true;
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (info && info->kind == TypeKind::Simple && (info->letter == 'P' || info->letter == 'z' || info->letter == 'Z'))
        return true;
    raiseTypeError(globalObject, scope, "'out' parameter "_s, index, " must be a pointer type, not "_s, PyType::is(type) ? PyType::from(type)->nameInMessages() : typeName(globalObject, type));
    return false;
}

enum ParamFlag : unsigned {
    ParamIn = 1,
    ParamOut = 2,
    ParamLCID = 4,
};

// `argTypes` may be empty: those of the type. False: an exception.
static bool validateParamFlags(JSGlobalObject* globalObject, PyType* type, JSValue paramFlags, JSValue argTypes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info) {
        raiseTypeError(globalObject, scope, "abstract class"_s);
        return false;
    }
    if (!argTypes)
        argTypes = info->argTypes.get();
    if (!paramFlags || !argTypes)
        return true;
    auto* flags = PyTuple::tryFrom(paramFlags);
    if (!flags) {
        raiseTypeError(globalObject, scope, "paramflags must be a tuple or None"_s);
        return false;
    }
    PyTuple* types = tupleFromIterable(globalObject, argTypes);
    RETURN_IF_EXCEPTION(scope, false);
    if (flags->length() != types->length()) {
        raiseValueError(globalObject, scope, "paramflags must have the same length as argtypes"_s);
        return false;
    }
    for (unsigned i = 0; i < flags->length(); ++i) {
        auto* item = PyTuple::tryFrom(flags->at(i));
        bool isValid = item && item->length() >= 1 && item->length() <= 3 && isInt(item->at(0)) && tryInt64(intValue(item->at(0)))
            && (item->length() < 2 || isNone(item->at(1)) || isStr(item->at(1)));
        if (!isValid) {
            raiseTypeError(globalObject, scope, "paramflags must be a sequence of (int [,string [,value]]) tuples"_s);
            return false;
        }
        int64_t flag = *tryInt64(intValue(item->at(0)));
        switch (flag & (ParamIn | ParamOut | ParamLCID)) {
        case 0:
        case ParamIn:
        case ParamIn | ParamLCID:
        case ParamIn | ParamOut:
            break;
        case ParamOut:
            if (failed(scope, isOutArgumentType(globalObject, state, types->at(i), i + 1)))
                return false;
            break;
        default:
            raiseTypeError(globalObject, scope, "paramflag value "_s, flag, " not supported"_s);
            return false;
        }
    }
    return true;
}

static bool setFunctionPointer(JSGlobalObject* globalObject, PyCData* function, const void* address)
{
    auto memory = Place { function }.resolve(globalObject, sizeof(void*));
    if (!memory.data())
        return false;
    memcpy(memory.data(), &address, memory.size());
    return true;
}

static JSValue functionFromLibrary(JSGlobalObject* globalObject, PyType* type, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (failed(scope, checkPositional(globalObject, "function"_s, args, 1, 2)))
        return { };
    JSValue paramFlags = args.get(1);
    if (paramFlags && isNone(paramFlags))
        paramFlags = { };
    auto* spec = PyTuple::from(args[0]);
    if (spec->length() != 2)
        return raiseTypeError(globalObject, scope, "illegal func_spec argument"_s);
    JSValue nameValue = spec->at(0);
    JSValue library = spec->at(1);
    Vector<uint8_t> name;
    if (auto* bytes = PyBytes::tryFrom(nameValue))
        name.append(bytes->span());
    else if (isStr(nameValue)) {
        if (failed(scope, encodeUTF8(globalObject, strValue(nameValue), name)))
            return { };
    } else
        return raiseTypeError(globalObject, scope, "function name must be string, bytes object or integer"_s);
    // It is a C string from here on: what follows a NUL is not looked for, so not reported either.
    name.append(0);
    String nameText = decodeUTF8Lenient(name.span().first(name.find(0)));
    audit(globalObject, "ctypes.dlsym"_s, library, makeStr(globalObject, nameText));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue handleValue = getAttr(globalObject, library, state.handleName.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInt(handleValue))
        return raiseTypeError(globalObject, scope, "the _handle attribute of the second argument must be an integer"_s);
    uintptr_t handle;
    pointerFromInt(globalObject, handleValue, handle);
    if (scope.exception()) {
        takePendingException(globalObject);
        return raiseValueError(globalObject, scope, "could not convert the _handle attribute to a pointer"_s);
    }
    PyType* attributeError = runtime(globalObject).AttributeError();
#if OS(WINDOWS)
    UNUSED_VARIABLE(handle);
    void* address = nullptr;
#else
    dlerror();
    void* address = dlsym(std::bit_cast<void*>(handle), byteCast<char>(name.span()).data());
    if (!address) {
        if (const char* message = dlerror())
            return raise(globalObject, scope, attributeError, decodeFilesystem(byteCast<uint8_t>(unsafeSpan(message))));
    }
#endif
    if (!address)
        return raise(globalObject, scope, attributeError, makeString("function '"_s, nameText, "' not found"_s));
    if (failed(scope, validateParamFlags(globalObject, type, paramFlags, { })))
        return { };
    PyCData* data = newData(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    auto* function = uncheckedDowncast<PyCFuncPtr>(data);
    if (paramFlags)
        function->paramFlags.set(vm, function, paramFlags);
    if (failed(scope, setFunctionPointer(globalObject, function, address)) || failed(scope, keepAlive(globalObject, function, 0, library)))
        return { };
    function->callable.set(vm, function, function);
    return function;
}

static JSValue functionNew(JSGlobalObject* globalObject, JSValue typeValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(typeValue);
    if (!args.size())
        RELEASE_AND_RETURN(scope, newData(globalObject, type));
    if (PyTuple::is(args[0]))
        RELEASE_AND_RETURN(scope, functionFromLibrary(globalObject, type, args));
    if (args.size() == 1 && isInt(args[0])) {
        uintptr_t address;
        if (failed(scope, pointerFromInt(globalObject, args[0], address)))
            return { };
        PyCData* function = newData(globalObject, type);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, setFunctionPointer(globalObject, function, std::bit_cast<void*>(address))))
            return { };
        return function;
    }
    if (failed(scope, checkPositional(globalObject, "function"_s, args, 1, 1)))
        return { };
    JSValue callable = args[0];
    if (!isCallable(globalObject, callable))
        return raiseTypeError(globalObject, scope, "argument must be callable or integer function address"_s);
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info || !info->argTypes)
        return raiseTypeError(globalObject, scope, "cannot construct instance of this class: no argtypes"_s);
    PyCThunk* thunk = makeThunk(globalObject, callable, info->argTypes.get(), info->resultType.get(), info->flags);
    RETURN_IF_EXCEPTION(scope, { });
    PyCData* data = newData(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    auto* function = uncheckedDowncast<PyCFuncPtr>(data);
    function->callable.set(vm, function, callable);
    function->thunk.set(vm, function, thunk);
    if (failed(scope, setFunctionPointer(globalObject, function, thunk->code)) || failed(scope, keepAlive(globalObject, function, 0, thunk)))
        return { };
    return function;
}

// The argument for a parameter that is not an output. Empty: an exception.
static JSValue takeArgument(JSGlobalObject* globalObject, unsigned& index, JSValue name, JSValue defaultValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (index < args.size())
        return args[index++];
    if (args.hasKeywords() && name && isStr(name)) {
        if (JSValue value = args.keyword(strValue(name))) {
            ++index;
            return value;
        }
    }
    if (defaultValue)
        return defaultValue;
    if (name && !isNone(name)) {
        JSString* text = str(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        return raiseTypeError(globalObject, scope, "required argument '"_s, stringOf(text), "' missing"_s);
    }
    return raiseTypeError(globalObject, scope, "not enough arguments"_s);
}

// Which of the arguments are results as well. (CPython has a bit for each, 32 of them, and takes the thirty-third for the first.)
struct OutputArguments {
    enum class Kind : uint8_t { None, Out, InOut };
    Vector<Kind> kinds;
    unsigned count { 0 };
};

// This, what follows, and what raises below have frames of their own, so that what they need room for is not in the frame of functionCall(), under
// which the function may wait for as long as it likes: see callFunction().
static NEVER_INLINE PyTuple* buildCallArguments(JSGlobalObject* globalObject, PyCFuncPtr* function, JSValue argTypesValue, const CallArgs& args, OutputArguments& outputs)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* paramFlags = PyTuple::tryFrom(function->paramFlags.get());
    auto* argTypes = PyTuple::tryFrom(argTypesValue);
    if (!argTypes || !paramFlags || !argTypes->length() || paramFlags->length() != argTypes->length())
        return PyTuple::create(globalObject, args.positional());

    unsigned count = argTypes->length();
    PyTuple* result = PyTuple::create(globalObject, count);
    outputs.kinds.fill(OutputArguments::Kind::None, count);
    unsigned consumed = 0;
    for (unsigned i = 0; i < count; ++i) {
        // validateParamFlags() has looked at them only if there were argtypes to hold them against, and those of the class that the function had then.
        auto* item = PyTuple::tryFrom(paramFlags->at(i));
        if (!item || !item->length() || !isInt(item->at(0))) {
            raiseTypeError(globalObject, scope, "paramflags must be a sequence of (int [,string [,value]]) tuples"_s);
            return nullptr;
        }
        unsigned flag = static_cast<unsigned>(lowBitsOfInt(intValue(item->at(0))));
        JSValue name = item->length() > 1 ? item->at(1) : JSValue();
        JSValue defaultValue = item->length() > 2 ? item->at(2) : JSValue();
        switch (flag & (ParamIn | ParamOut | ParamLCID)) {
        case ParamIn | ParamLCID:
            result->initializeAt(vm, i, defaultValue ? defaultValue : jsNumber(0));
            break;
        case ParamIn | ParamOut:
            outputs.kinds[i] = OutputArguments::Kind::InOut;
            ++outputs.count;
            [[fallthrough]];
        case 0:
        case ParamIn: {
            JSValue value = takeArgument(globalObject, consumed, name, defaultValue, args);
            RETURN_IF_EXCEPTION(scope, nullptr);
            result->initializeAt(vm, i, value);
            break;
        }
        case ParamOut: {
            outputs.kinds[i] = OutputArguments::Kind::Out;
            ++outputs.count;
            if (defaultValue) {
                result->initializeAt(vm, i, defaultValue);
                break;
            }
            JSValue type = argTypes->at(i);
            PyCTypeInfo* info = infoOf(globalObject, type);
            if (!info) {
                raiseRuntimeError(globalObject, scope, "NULL stginfo unexpected"_s);
                return nullptr;
            }
            if (info->kind == TypeKind::Simple) {
                raiseTypeError(globalObject, scope, PyType::from(type)->nameInMessages(), " 'out' parameter must be passed as default value"_s);
                return nullptr;
            }
            // An instance of what the pointer points to: from_param() will pass it by reference.
            JSValue target = info->kind == TypeKind::Array ? type : info->kind == TypeKind::Pointer ? info->proto.get() : JSValue();
            if (!target) {
                raiseTypeError(globalObject, scope, PyType::from(type)->nameInMessages(), " 'out' parameter points to no type"_s);
                return nullptr;
            }
            JSValue value = call(globalObject, target);
            RETURN_IF_EXCEPTION(scope, nullptr);
            result->initializeAt(vm, i, value);
            break;
        }
        default:
            raiseValueError(globalObject, scope, "paramflag "_s, flag, " not yet implemented"_s);
            return nullptr;
        }
    }
    unsigned given = args.size() + args.keywordCount();
    if (given != consumed) {
        raiseTypeError(globalObject, scope, "call takes exactly "_s, consumed, " arguments ("_s, given, " given)"_s);
        return nullptr;
    }
    return result;
}

static NEVER_INLINE JSValue buildCallResult(JSGlobalObject* globalObject, JSValue result, PyTuple* arguments, const OutputArguments& outputs)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (!outputs.count)
        return result;
    PyTuple* tuple = outputs.count > 1 ? PyTuple::create(globalObject, outputs.count) : nullptr;
    unsigned index = 0;
    for (unsigned i = 0; i < outputs.kinds.size(); ++i) {
        JSValue value;
        if (outputs.kinds[i] == OutputArguments::Kind::InOut)
            value = arguments->at(i);
        else if (outputs.kinds[i] == OutputArguments::Kind::Out) {
            value = callMethod(globalObject, arguments->at(i), state.fromOutParamName.get());
            RETURN_IF_EXCEPTION(scope, { });
        } else
            continue;
        if (!tuple)
            return value;
        tuple->initializeAt(vm, index++, value);
        if (index == outputs.count)
            break;
    }
    return tuple;
}

static NEVER_INLINE JSValue raiseWrongArgumentCount(JSGlobalObject* globalObject, ASCIILiteral atLeast, unsigned required, unsigned actual)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseTypeError(globalObject, scope, "this function takes "_s, atLeast, required, " argument"_s, required == 1 ? ""_s : "s"_s, " ("_s, actual, " given)"_s);
}

static JSValue functionCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCFuncPtr* function = PyCFuncPtr::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    auto choose = [](JSValue own, JSValue ofType) { return own ? own : ofType; };
    JSValue resultType = choose(function->resultType.get(), info->resultType.get());
    JSValue converters = choose(function->converters.get(), info->converters.get());
    JSValue checker = choose(function->checker.get(), info->checker.get());
    JSValue argTypes = choose(function->argTypes.get(), info->argTypes.get());
    JSValue errorCheck = function->errorCheck.get();

    void* address;
    {
        auto memory = Place { function }.resolve(globalObject, sizeof(void*));
        RETURN_IF_EXCEPTION(scope, { });
        memcpy(&address, memory.data(), sizeof(void*));
    }

    OutputArguments outputs;
    PyTuple* arguments = buildCallArguments(globalObject, function, argTypes, args, outputs);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* tuple = PyTuple::tryFrom(converters)) {
        unsigned required = tuple->length();
        unsigned actual = arguments->length();
        if (info->flags & FunctionCdecl) {
            if (required > actual)
                RELEASE_AND_RETURN(scope, raiseWrongArgumentCount(globalObject, "at least "_s, required, actual));
        } else if (required != actual)
            RELEASE_AND_RETURN(scope, raiseWrongArgumentCount(globalObject, ""_s, required, actual));
    }
    // The value of a call with out parameters is theirs (buildCallResult()). Then only these two see what the function returned.
    bool resultIsDropped = outputs.count && !checker && !errorCheck;
    JSValue result = callFunction(globalObject, address, arguments, info->flags, converters, resultType, checker, resultIsDropped ? ResultOfCall::Dropped : ResultOfCall::GivenOn);
    RETURN_IF_EXCEPTION(scope, { });
    if (errorCheck) {
        JSValue checked = call(globalObject, errorCheck, result, self, arguments);
        RETURN_IF_EXCEPTION(scope, { });
        // Anything but the arguments as they were is the result.
        if (checked != JSValue(arguments))
            return checked;
    }
    RELEASE_AND_RETURN(scope, buildCallResult(globalObject, result, arguments, outputs));
}

static JSValue functionRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    String address = addressForRepr(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString('<', PyCData::from(self)->type()->nameInMessages(), " object at "_s, address, '>')));
}

static JSValue functionIsNotNull(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto memory = Place { PyCData::from(self) }.resolve(globalObject, sizeof(void*));
    RETURN_IF_EXCEPTION(scope, { });
    void* address;
    memcpy(&address, memory.data(), sizeof(void*));
    return jsBoolean(!!address);
}

static JSValue functionGetErrorCheck(JSGlobalObject*, JSValue self)
{
    JSValue value = PyCFuncPtr::from(self)->errorCheck.get();
    return value ? value : none();
}

static void functionSetErrorCheck(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCFuncPtr* function = PyCFuncPtr::from(self);
    if (!value) {
        function->errorCheck.clear();
        return;
    }
    if (!isCallable(globalObject, value)) {
        raiseTypeError(globalObject, scope, "the errcheck attribute must be callable"_s);
        return;
    }
    function->errorCheck.set(vm, function, value);
}

static JSValue functionGetResultType(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue value = PyCFuncPtr::from(self)->resultType.get())
        return value;
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    return info && info->resultType ? info->resultType.get() : none();
}

static void functionSetResultType(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCFuncPtr* function = PyCFuncPtr::from(self);
    if (!value) {
        function->resultType.clear();
        function->checker.clear();
        return;
    }
    if (!isNone(value) && !infoOf(globalObject, value) && !isCallable(globalObject, value)) {
        raiseTypeError(globalObject, scope, "restype must be a type, a callable, or None"_s);
        return;
    }
    JSValue checker = getAttrIfPresent(globalObject, value, stateOf(globalObject).checkRetvalName.get());
    RETURN_IF_EXCEPTION(scope, void());
    if (checker)
        function->checker.set(vm, function, checker);
    else
        function->checker.clear();
    function->resultType.set(vm, function, value);
}

static JSValue functionGetArgTypes(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue value = PyCFuncPtr::from(self)->argTypes.get())
        return value;
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    return info && info->argTypes ? info->argTypes.get() : none();
}

static void functionSetArgTypes(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCFuncPtr* function = PyCFuncPtr::from(self);
    if (!value || isNone(value)) {
        function->argTypes.clear();
        function->converters.clear();
        return;
    }
    JSValue converters = convertersFor(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    if (failed(scope, validateParamFlags(globalObject, function->type(), function->paramFlags.get(), value)))
        return;
    function->converters.set(vm, function, converters);
    function->argTypes.set(vm, function, value);
}

static constexpr NativeDef functionMethods[] = {
    PY_NEW(functionNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", functionRepr, "($self, /)"),
    PY_SLOT("__call__", functionCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__bool__", functionIsNotNull, "($self, /)"),
};

static constexpr NativeGetSet functionGetSets[] = {
    { "errcheck"_s, functionGetErrorCheck, functionSetErrorCheck, false, "a function to check for errors"_s },
    { "restype"_s, functionGetResultType, functionSetResultType, false, "specify the result type"_s },
    { "argtypes"_s, functionGetArgTypes, functionSetArgTypes, false, "specify the argument types"_s },
};

const TypeDef functionTypeDef { "_ctypes.CFuncPtr"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable }, functionMethods, functionGetSets, "Function Pointer"_s };

// ---- CThunkObject ----

static JSValue thunkFinalize(JSGlobalObject*, JSValue self)
{
    releaseCallbackCode(PyCThunk::from(self));
    return none();
}

static constexpr NativeDef thunkMethods[] = {
    PY_SLOT("__del__", thunkFinalize, "($self, /)"),
};

const TypeDef thunkTypeDef { "_ctypes.CThunkObject"_s, { TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, thunkMethods, { }, "CThunkObject"_s };

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
