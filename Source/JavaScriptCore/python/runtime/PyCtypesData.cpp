/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "PyList.h"
#include "PyRange.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

// ---- Where the memory is ----

uint8_t* PyCData::memoryOfView(JSGlobalObject* globalObject) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = uncheckedDowncast<PyMemoryView>(m_owner.get());
    if (view->isReleased()) {
        raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
        return nullptr;
    }
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // An empty bytearray need not have memory at all.
    static uint64_t nothing;
    if (!data)
        return reinterpret_cast<uint8_t*>(&nothing);
    return data + m_address;
}

void PyCData::setStorage(VM& vm, PyByteStorage* storage, size_t offset)
{
    m_owner.set(vm, this, storage);
    m_address = std::bit_cast<uintptr_t>(storage->data() + offset);
    m_isView = false;
}

void PyCData::setView(VM& vm, PyMemoryView* view, int64_t offset)
{
    m_owner.set(vm, this, view);
    m_address = static_cast<uintptr_t>(offset);
    m_isView = true;
}

void PyCData::setAddress(const void* address)
{
    m_owner.clear();
    m_address = std::bit_cast<uintptr_t>(address);
    m_isView = false;
}

void PyCData::setMemoryOf(VM& vm, const PyCData* other, int64_t offset)
{
    m_owner.setMayBeNull(vm, this, other->m_owner.get());
    m_address = other->m_address + static_cast<uintptr_t>(offset);
    m_isView = other->m_isView;
}

uint8_t* Place::resolve(JSGlobalObject* globalObject, int64_t size) const
{
    if (address)
        return address;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A CField can be asked about an instance of any ctypes type.
    if (offset < 0 || size < 0 || offset > object->size || size > object->size - offset) {
        raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
        return nullptr;
    }
    uint8_t* memory = object->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!memory) {
        raiseValueError(globalObject, scope, "NULL pointer access"_s);
        return nullptr;
    }
    return memory + offset;
}

// ---- Making instances ----

static PyCData* allocateData(JSGlobalObject* globalObject, PyType* type, PyCTypeInfo* info)
{
    VM& vm = getVM(globalObject);
    State& state = *info->state.get();
    info->isFinal = true;
    PyCData* data = info->kind == TypeKind::Function ? PyCFuncPtr::create(vm, state.functionStructure.get(), type) : PyCData::create(vm, state.dataStructure.get(), type);
    data->length = info->length;
    data->size = info->size;
    if (type->isHeapType() && type->lookup(Special::Del)) [[unlikely]]
        runtime(globalObject).registerFinalizable(data);
    return data;
}

bool allocateMemory(JSGlobalObject* globalObject, PyCData* data, int64_t size)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // In CPython a small instance has its memory in itself: 16 bytes, zeroed, whatever it needs of them. Programs have come to rely on what
    // follows being zero: a c_wchar or a full (c_char * 3) passed where C expects a string that ends. The cell is a multiple of 16
    // bytes anyway, and what is left over would otherwise hold what some dead object left there.
    static constexpr int64_t granule = 16;
    PyByteStorage* storage = nullptr;
    if (size <= std::numeric_limits<int64_t>::max() - granule) {
        size_t capacity = std::max<size_t>(roundUpToMultipleOf<granule>(static_cast<size_t>(size)), granule);
        storage = PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), capacity);
    }
    if (!storage) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    memset(storage->data(), 0, storage->capacity());
    data->setStorage(vm, storage, 0);
    data->size = size;
    data->ownsMemory = true;
    return true;
}

static PyCTypeInfo* concreteInfoOf(JSGlobalObject* globalObject, PyType* type)
{
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, "abstract class"_s);
    }
    return info;
}

PyCData* newData(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCData* data = allocateData(globalObject, type, info);
    if (failed(scope, allocateMemory(globalObject, data, info->size)))
        return nullptr;
    return data;
}

// What a pointer points to is, as a rule, the memory of an instance that the pointer keeps alive. Then it is known how far that
// memory goes, and who owns it.
struct Provenance {
    JSCell* owner { nullptr }; // A PyByteStorage or a PyMemoryView. Null: nothing is known.
    const uint8_t* begin { nullptr };
    const uint8_t* end { nullptr };
};

static bool findProvenance(JSGlobalObject* globalObject, JSValue kept, const uint8_t* address, Provenance& result, unsigned depth = 0)
{
    if (!kept || !kept.isCell() || depth > 4)
        return false;
    if (auto* other = PyCData::tryFrom(kept)) {
        if (!other->owner())
            return false;
        const uint8_t* begin;
        size_t size;
        if (other->isView()) {
            auto* view = uncheckedDowncast<PyMemoryView>(other->owner());
            if (view->isReleased())
                return false;
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
            begin = view->data(globalObject);
            if (scope.exception()) {
                dropPendingException(globalObject, scope);
                return false;
            }
            size = static_cast<size_t>(view->length);
        } else {
            auto* storage = uncheckedDowncast<PyByteStorage>(other->owner());
            begin = storage->data();
            size = storage->capacity();
        }
        if (!begin || address < begin || address > begin + size)
            return false;
        result = { other->owner(), begin, begin + size };
        return true;
    }
    if (auto* tuple = PyTuple::tryFrom(kept)) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (findProvenance(globalObject, tuple->at(i), address, result, depth + 1))
                return true;
        }
        return false;
    }
    if (auto* dict = PyDict::tryFrom(kept)) {
        JSValue key;
        JSValue value;
        unsigned seen = 0;
        for (unsigned position = 0; dict->next(position, key, value) && seen < 32; ++seen) {
            if (findProvenance(globalObject, value, address, result, depth + 1))
                return true;
        }
    }
    return false;
}

// `pointer` holds `target`, and `size` bytes at `address` are about to be used. If it is known what the pointer points into, they
// have to be within that: the type of a pointer need not be that of what it was made to point to. False: an exception.
static bool checkPointerAccess(JSGlobalObject* globalObject, PyCData* pointer, const uint8_t* target, const uint8_t* address, int64_t size, Provenance& provenance)
{
    PyCData* root = pointer;
    while (root->base)
        root = root->base.get();
    if (!findProvenance(globalObject, root->objects.get(), target, provenance))
        return true;
    if (address >= provenance.begin && address <= provenance.end && size <= provenance.end - address)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "access through a pointer beyond the memory of the object it points into"_s);
    return false;
}

PyCData* dataAt(JSGlobalObject* globalObject, PyType* type, const Place& place, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!place.address && (place.offset < 0 || place.offset > place.object->size || info->size > place.object->size - place.offset)) {
        raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
        return nullptr;
    }
    PyCData* data = allocateData(globalObject, type, info);
    data->base.set(vm, data, place.object);
    data->index = index;
    if (!place.address)
        data->setMemoryOf(vm, place.object, place.offset);
    else {
        // The pointer may be made to point elsewhere while this instance is in use: it holds on to the memory itself.
        Provenance provenance;
        if (failed(scope, checkPointerAccess(globalObject, place.object, place.address, place.address, info->size, provenance)))
            return nullptr;
        if (!provenance.owner)
            data->setAddress(place.address);
        else if (auto* view = dynamicDowncast<PyMemoryView>(provenance.owner))
            data->setView(vm, view, place.address - provenance.begin);
        else
            data->setStorage(vm, uncheckedDowncast<PyByteStorage>(provenance.owner), place.address - provenance.begin);
    }
    return data;
}

PyCData* dataAtAddress(JSGlobalObject* globalObject, PyType* type, const void* address)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool ok = audit(globalObject, "ctypes.cdata"_s, intFromPointer(globalObject, address));
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(ok, ok);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCData* data = allocateData(globalObject, type, info);
    data->setAddress(address);
    return data;
}

bool checkRoomFor(JSGlobalObject* globalObject, PyCTypeInfo* info, int64_t length, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (info->size <= length - offset)
        return true;
    raiseValueError(globalObject, scope, "Buffer size too small ("_s, length, " instead of at least "_s, static_cast<int64_t>(static_cast<uint64_t>(info->size) + static_cast<uint64_t>(offset)), " bytes)"_s);
    return false;
}

PyCData* dataInView(JSGlobalObject* globalObject, PyType* type, PyMemoryView* view, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint8_t* memory = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool ok = audit(globalObject, "ctypes.cdata"_s, intFromPointer(globalObject, memory + offset));
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(ok, ok);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (failed(scope, checkRoomFor(globalObject, info, view->length, offset)))
        return nullptr;
    PyCData* data = allocateData(globalObject, type, info);
    data->setView(vm, view, offset);
    return data;
}

PyCData* dataFromCopy(JSGlobalObject* globalObject, PyType* type, const void* source, int64_t size)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = newData(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    memcpy(data->memory(globalObject), source, static_cast<size_t>(std::min(size, data->size)));
    return data;
}

bool isSimpleInstanceType(State& state, PyType* type)
{
    return type->base() == state.simpleType.get();
}

// ---- What keeps what alive ----

static PyCData* containerOf(JSGlobalObject* globalObject, PyCData* data)
{
    VM& vm = getVM(globalObject);
    while (data->base)
        data = data->base.get();
    if (!data->objects)
        data->objects.set(vm, data, data->length ? JSValue(PyDict::create(globalObject)) : none());
    return data;
}

JSValue keptObjects(JSGlobalObject* globalObject, PyCData* data)
{
    return containerOf(globalObject, data)->objects.get();
}

// The way from the container to the part, from the inside out, in hexadecimal. Null: it is too long.
static String keyOfPart(PyCData* target, int64_t index)
{
    StringBuilder key;
    key.append(hex(static_cast<uint32_t>(index), Lowercase));
    for (PyCData* part = target; part->base; part = part->base.get()) {
        if (key.length() > 239)
            return String();
        key.append(':', hex(static_cast<uint32_t>(part->index), Lowercase));
    }
    return key.toString();
}

bool keepAlive(JSGlobalObject* globalObject, PyCData* target, int64_t index, JSValue keep)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(keep))
        return true;
    PyCData* container = containerOf(globalObject, target);
    auto* dict = PyDict::tryFrom(container->objects.get());
    if (!dict || dict->type() != rt.dictType()) {
        container->objects.set(vm, container, keep);
        return true;
    }
    String key = keyOfPart(target, index);
    if (key.isNull()) {
        raiseValueError(globalObject, scope, "ctypes object structure too deep"_s);
        return false;
    }
    dict->set(globalObject, makeStr(vm, key), keep);
    return !scope.exception();
}

JSValue keptFor(JSGlobalObject* globalObject, PyCData* target, int64_t index)
{
    Runtime& rt = runtime(globalObject);
    PyCData* container = target;
    while (container->base)
        container = container->base.get();
    JSValue objects = container->objects.get();
    auto* dict = PyDict::tryFrom(objects);
    if (!dict || dict->type() != rt.dictType())
        return objects;
    String key = keyOfPart(target, index);
    if (key.isNull())
        return { };
    return dict->get(makeStr(rt.vm(), key));
}

// ---- Reading and writing parts ----

void extendError(JSGlobalObject* globalObject, PyType* exceptionType, const String& prefix)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue exception = takePendingException(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    String name = typeOf(globalObject, exception)->name();
    JSString* text = str(globalObject, exception);
    String message;
    if (scope.exception()) {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        message = "???"_s;
    } else
        message = stringOf(text);
    raise(globalObject, scope, exceptionType, makeString(prefix, name, ": "_s, message));
}

JSValue getData(JSGlobalObject* globalObject, PyType* type, Accessor accessor, const Place& place, int64_t index, int64_t size, Bits bits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!accessor) {
        PyCTypeInfo* info = infoOf(globalObject, type);
        if (info && info->accessor && isSimpleInstanceType(*info->state.get(), type))
            accessor = info->accessor;
    }
    if (!accessor)
        RELEASE_AND_RETURN(scope, dataAt(globalObject, type, place, index));
    uint8_t* memory = place.resolve(globalObject, size);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue hint = accessor.code == Code::Object ? keptFor(globalObject, place.object, index) : JSValue();
    RELEASE_AND_RETURN(scope, load(globalObject, accessor, memory, size, bits, hint));
}

// What has to be kept alive for the part from now on. Empty: an exception.
static JSValue storeData(JSGlobalObject* globalObject, PyType* type, Accessor accessor, const Place& place, JSValue value, int64_t size, Bits bits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);

    auto storeWith = [&](Accessor accessor) -> JSValue {
        Packed packed;
        bool ok = pack(globalObject, accessor, value, size, packed);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(ok, ok);
        uint8_t* memory = place.resolve(globalObject, size);
        RETURN_IF_EXCEPTION(scope, { });
        store(memory, accessor, packed, size, bits);
        ensureStillAliveHere(value);
        return packed.keep;
    };

    if (accessor)
        RELEASE_AND_RETURN(scope, storeWith(accessor));

    PyCData* source = PyCData::tryFrom(value);
    if (!source) {
        PyCTypeInfo* info = infoOf(globalObject, type);
        if (info && info->accessor)
            RELEASE_AND_RETURN(scope, storeWith(info->accessor));
        // A tuple stands for the arguments to make an instance with.
        if (auto* tuple = PyTuple::tryFrom(value)) {
            JSValue object = callWithTupleAndDict(globalObject, type, tuple, nullptr);
            if (scope.exception()) [[unlikely]] {
                scope.release();
                extendError(globalObject, runtime(globalObject).RuntimeError(), makeString('(', type->nameInMessages(), ") "_s));
                return { };
            }
            if (!PyCData::is(object))
                return raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), " instance, got "_s, typeName(globalObject, object));
            RELEASE_AND_RETURN(scope, storeData(globalObject, type, accessor, place, object, size, bits));
        }
        if (isNone(value) && isTypeOfMeta(type, state.pointerMeta.get())) {
            uint8_t* memory = place.resolve(globalObject, sizeof(void*));
            RETURN_IF_EXCEPTION(scope, { });
            memset(memory, 0, sizeof(void*));
            return none();
        }
        return raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), " instance, got "_s, typeName(globalObject, value));
    }

    bool isOfType = isInstance(globalObject, value, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType) {
        // __instancecheck__ may say anything.
        if (source->size < size)
            return raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
        uint8_t* from = Place { source }.resolve(globalObject, size);
        RETURN_IF_EXCEPTION(scope, { });
        uint8_t* to = place.resolve(globalObject, size);
        RETURN_IF_EXCEPTION(scope, { });
        memmove(to, from, static_cast<size_t>(size));
        return keptObjects(globalObject, source);
    }

    if (isTypeOfMeta(type, state.pointerMeta.get()) && source->type()->isSubtypeOf(state.arrayType.get())) {
        PyCTypeInfo* arrayInfo = infoOfInstance(globalObject, source);
        PyCTypeInfo* pointerInfo = infoOf(globalObject, type);
        if (arrayInfo && pointerInfo && arrayInfo->proto.get() == pointerInfo->proto.get()) {
            uint8_t* from = source->memory(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            uint8_t* to = place.resolve(globalObject, sizeof(void*));
            RETURN_IF_EXCEPTION(scope, { });
            memcpy(to, &from, sizeof(void*));
            // The array itself has to live, not only what it keeps alive.
            return PyTuple::create(globalObject, { keptObjects(globalObject, source), source });
        }
    }
    return raiseTypeError(globalObject, scope, "incompatible types, "_s, typeName(globalObject, value), " instance instead of "_s, type->nameInMessages(), " instance"_s);
}

bool setData(JSGlobalObject* globalObject, PyType* type, Accessor accessor, const Place& place, JSValue value, int64_t index, int64_t size, Bits bits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue keep = storeData(globalObject, type, accessor, place, value, size, bits);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, keepAlive(globalObject, place.object, index, keep));
}

// ---- _CData ----

static JSValue dataNew(JSGlobalObject* globalObject, JSValue type, const CallArgs&)
{
    return newData(globalObject, PyType::from(type));
}

static JSValue dataHash(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "unhashable type"_s);
}

JSValue dataBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    PyCTypeInfo* itemInfo = info;
    while (itemInfo->kind == TypeKind::Array) {
        PyCTypeInfo* next = infoOf(globalObject, itemInfo->proto.get());
        if (!next)
            break;
        itemInfo = next;
    }

    Buffer buffer;
    buffer.object = self;
    buffer.length = data->size;
    buffer.itemSize = itemInfo->size;
    buffer.isReadOnly = false;
    buffer.format = info->format ? stringOf(info->format.get()) : "B"_str;
    PyTuple* shape = info->shape.get();
    buffer.dimensions = shape ? shape->length() : 0;
    CheckedInt64 sizeOfShape = buffer.itemSize;
    bool isEmpty = false;
    for (unsigned i = 0; i < buffer.dimensions; ++i) {
        buffer.shape.append(tryInt64(shape->at(i)).value_or(0));
        sizeOfShape *= buffer.shape.last();
        isEmpty |= !buffer.shape.last();
    }
    // The shape and the size of an item are the type's, as it is now; the memory is the instance's. If the type has outgrown the instance (S * 4, and S is given its fields
    // afterwards; __class__ is assigned to), the buffer is the bytes of the instance and no more is said of them.
    if (!isEmpty && (sizeOfShape.hasOverflowed() || sizeOfShape.value() > data->size)) {
        buffer.itemSize = 1;
        buffer.format = "B"_str;
        buffer.dimensions = 1;
        buffer.shape = { data->size };
    }

    if (data->isView()) {
        auto* view = uncheckedDowncast<PyMemoryView>(data->owner());
        if (view->isReleased())
            return raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
        buffer.owner = view->managedBuffer()->owner();
        buffer.offset = view->offset + data->offsetInView();
        buffer.requiredSize = view->managedBuffer()->requiredSize;
    } else if (data->owner()) {
        auto* storage = uncheckedDowncast<PyByteStorage>(data->owner());
        buffer.owner = storage;
        buffer.offset = data->memory(globalObject) - storage->data();
        buffer.requiredSize = buffer.offset + data->size;
    } else {
        // Nothing is known about the memory. A buffer wants an owner: an ArrayBuffer that frees nothing.
        uint8_t* memory = data->memory(globalObject);
        if (static_cast<uint64_t>(data->size) > MAX_ARRAY_BUFFER_SIZE)
            return raiseBufferError(globalObject, scope, "the memory is too large to be exported"_s);
        auto arrayBuffer = ArrayBuffer::createFromBytes(std::span<const uint8_t> { memory, static_cast<size_t>(data->size) }, createSharedTask<void(void*)>([](void*) { }));
        buffer.owner = JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTF::move(arrayBuffer));
        buffer.offset = 0;
        buffer.requiredSize = data->size;
    }
    RELEASE_AND_RETURN(scope, exportBuffer(globalObject, flags, buffer));
}

static JSValue dataFromOutParam(JSGlobalObject*, JSValue self) { return self; }

static JSValue dataReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    if (info->flags & (IsPointer | HasPointer))
        return raiseValueError(globalObject, scope, "ctypes objects containing pointers cannot be pickled"_s);
    JSValue dict = getAttr(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    uint8_t* memory = Place { data }.resolve(globalObject, data->size);
    RETURN_IF_EXCEPTION(scope, { });
    PyBytes* bytes = newBytes(globalObject, static_cast<size_t>(data->size));
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(bytes->mutableData(), memory, static_cast<size_t>(data->size));
    JSValue state = PyTuple::create(globalObject, { dict, bytes });
    JSValue arguments = PyTuple::create(globalObject, { data->type(), state });
    return PyTuple::create(globalObject, { info->state->unpickle.get(), arguments });
}

static JSValue dataSetState(JSGlobalObject* globalObject, JSValue self, JSValue dict, JSValue bytes)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    if (!isDict(dict))
        return raiseTypeError(globalObject, scope, "__setstate__() argument 1 must be dict, not "_s, typeNameOfArgument(globalObject, dict));
    {
        ByteBuilder encoded;
        BufferHolder holder(globalObject);
        std::span<const uint8_t> source;
        if (isStr(bytes)) {
            StrView view(globalObject, strValue(bytes));
            RETURN_IF_EXCEPTION(scope, { });
            if (!appendUTF8(encoded, view.view()))
                return raiseValueError(globalObject, scope, "surrogates not allowed"_s);
            if (encoded.hasOverflowed()) [[unlikely]]
                return raiseMemoryError(globalObject, scope, String());
            source = encoded.span();
        } else {
            if (!hasBuffer(bytes))
                return raiseTypeError(globalObject, scope, "a bytes-like object is required, not '"_s, typeName(globalObject, bytes), '\'');
            bool ok = holder.get(bytes);
            RETURN_IF_EXCEPTION(scope, { });
            ASSERT_UNUSED(ok, ok);
            source = holder.buffer.span();
        }
        size_t count = std::min(source.size(), static_cast<size_t>(data->size));
        uint8_t* memory = Place { data }.resolve(globalObject, count);
        RETURN_IF_EXCEPTION(scope, { });
        memmove(memory, source.data(), count);
    }
    JSValue own = getAttr(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDict(own))
        return raiseTypeError(globalObject, scope, data->type()->nameInMessages(), ".__dict__ must be a dictionary, not "_s, typeName(globalObject, own));
    JSValue key;
    JSValue value;
    for (unsigned position = 0; PyDict::from(dict)->next(position, key, value);) {
        PyDict::from(own)->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue dataBase(JSGlobalObject*, JSValue self)
{
    PyCData* base = PyCData::from(self)->base.get();
    return base ? JSValue(base) : none();
}
static JSValue dataNeedsFree(JSGlobalObject*, JSValue self) { return jsNumber(PyCData::from(self)->ownsMemory ? 1 : 0); }
static JSValue dataObjects(JSGlobalObject*, JSValue self)
{
    JSValue objects = PyCData::from(self)->objects.get();
    return objects ? objects : none();
}

static constexpr NativeDef dataMethods[] = {
    PY_SLOT("__hash__", dataHash, "($self, /)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_METHOD("__ctypes_from_outparam__", dataFromOutParam, "($self, /)"),
    PY_METHOD("__setstate__", dataSetState, "($self, dict, data, /)"),
    PY_METHOD("__reduce__", dataReduce, "($self, /)"),
};

static constexpr NativeGetSet dataGetSets[] = {
    PY_MEMBER("_b_base_", dataBase),
    PY_MEMBER("_b_needsfree_", dataNeedsFree),
    PY_MEMBER("_objects", dataObjects),
};

const TypeDef dataTypeDef { "_ctypes._CData"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, dataMethods, dataGetSets, "XXX to be provided"_s };

// ---- _SimpleCData ----

static JSValue simpleGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info || !info->accessor)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    uint8_t* memory = Place { data }.resolve(globalObject, info->size);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue hint = info->accessor.code == Code::Object ? keptFor(globalObject, data, 0) : JSValue();
    RELEASE_AND_RETURN(scope, load(globalObject, info->accessor, memory, info->size, { }, hint));
}

static void simpleSetValue(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete attribute"_s);
        return;
    }
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info || !info->accessor) {
        raiseTypeError(globalObject, scope, "abstract class"_s);
        return;
    }
    Packed packed;
    bool ok = pack(globalObject, info->accessor, value, info->size, packed);
    RETURN_IF_EXCEPTION(scope, void());
    ASSERT_UNUSED(ok, ok);
    uint8_t* memory = Place { data }.resolve(globalObject, info->size);
    RETURN_IF_EXCEPTION(scope, void());
    store(memory, info->accessor, packed, info->size);
    RELEASE_AND_RETURN(scope, void(keepAlive(globalObject, data, 0, packed.keep)));
}

static JSValue simpleInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "__init__"_s, args, 0, 1)))
        return { };
    if (args.size()) {
        simpleSetValue(globalObject, self, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue simpleFromOutParam(JSGlobalObject* globalObject, JSValue self)
{
    if (!isSimpleInstanceType(stateOf(globalObject), PyCData::from(self)->type()))
        return self;
    return simpleGetValue(globalObject, self);
}

static JSValue dataIsNonZero(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    uint8_t* memory = Place { data }.resolve(globalObject, data->size);
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0; i < data->size; ++i) {
        if (memory[i])
            return jsBoolean(true);
    }
    return jsBoolean(false);
}

static JSValue simpleRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    if (!isSimpleInstanceType(stateOf(globalObject), data->type())) {
        String address = addressForRepr(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString('<', data->type()->nameInMessages(), " object at "_s, address, '>')));
    }
    JSValue value = simpleGetValue(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(globalObject, makeString(data->type()->nameInMessages(), '(', stringOf(text), ')'));
}

static constexpr NativeDef simpleMethods[] = {
    PY_NEW(dataNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", simpleRepr, "($self, /)"),
    PY_SLOT("__init__", simpleInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__bool__", dataIsNonZero, "($self, /)"),
    PY_METHOD("__ctypes_from_outparam__", simpleFromOutParam, "($self, /)"),
};

static constexpr NativeGetSet simpleGetSets[] = {
    { "value"_s, simpleGetValue, simpleSetValue },
};

const TypeDef simpleTypeDef { "_ctypes._SimpleCData"_s, { TypeFlag::Base, TypeFlag::Immutable }, simpleMethods, simpleGetSets, { } };

// ---- Structure, Union ----

// Sets the fields of the class, after those of its bases, from the positional arguments. Returns how many fields there are so
// far, or -1 for an exception.
static int64_t structInitFromPositional(JSGlobalObject* globalObject, JSValue self, PyType* type, const CallArgs& args, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    if (type->base() && infoOf(globalObject, type->base())) {
        index = structInitFromPositional(globalObject, self, type->base(), args, index);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info)
        return index;
    JSValue fields = type->dict()->get(state.fieldsName.get());
    if (!fields)
        return index;
    for (int64_t i = index; i < info->length && i < static_cast<int64_t>(args.size()); ++i) {
        JSValue pair = getItem(globalObject, fields, makeInt(globalObject, i - index));
        RETURN_IF_EXCEPTION(scope, -1);
        JSValue name = getItem(globalObject, pair, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, -1);
        JSString* attribute = attributeName(globalObject, name);
        RETURN_IF_EXCEPTION(scope, -1);
        if (args.hasKeywords() && args.keyword(attribute)) {
            String text = reprForMessage(globalObject, name);
            RETURN_IF_EXCEPTION(scope, -1);
            raiseTypeError(globalObject, scope, "duplicate values for field "_s, text);
            return -1;
        }
        setAttr(globalObject, self, attribute, args[static_cast<unsigned>(i)]);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    return info->length;
}

static JSValue structInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size()) {
        int64_t count = structInitFromPositional(globalObject, self, PyCData::from(self)->type(), args, 0);
        RETURN_IF_EXCEPTION(scope, { });
        if (count < static_cast<int64_t>(args.size()))
            return raiseTypeError(globalObject, scope, "too many initializers"_s);
    }
    PyTuple* keys = args.keywordCount() ? keywordKeys(globalObject, args.keywordNames()) : nullptr;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        setAttrAsGiven(globalObject, self, args.keywordName(i), keys ? keys->at(i) : JSValue(args.keywordName(i)), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static constexpr NativeDef structMethods[] = {
    PY_NEW(dataNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", structInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
};

const TypeDef structTypeDef { "_ctypes.Structure"_s, { TypeFlag::Base, TypeFlag::Immutable }, structMethods, { }, "Structure base class"_s };
const TypeDef unionTypeDef { "_ctypes.Union"_s, { TypeFlag::Base, TypeFlag::Immutable }, structMethods, { }, "Union base class"_s };

// ---- Array ----

static JSValue arrayInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; i < args.size(); ++i) {
        setItem(globalObject, self, jsNumber(i), args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue arrayItem(JSGlobalObject* globalObject, PyCData* array, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (index < 0 || index >= array->length)
        return raiseIndexError(globalObject, scope, "invalid index"_s);
    PyCTypeInfo* info = infoOfInstance(globalObject, array);
    if (!info || !info->length)
        return raiseIndexError(globalObject, scope, "invalid index"_s);
    int64_t size = info->size / info->length;
    RELEASE_AND_RETURN(scope, getData(globalObject, PyType::from(info->proto.get()), { }, { array, index * size }, index, size));
}

// The characters a slice selects, as bytes or str. `memory` is where item 0 is.
static JSValue sliceOfCharacters(JSGlobalObject* globalObject, Code code, const uint8_t* memory, int64_t start, int64_t step, int64_t count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (code == Code::Char) {
        if (count <= 0)
            return runtime(globalObject).emptyBytes();
        PyBytes* result = newBytes(globalObject, static_cast<size_t>(count));
        RETURN_IF_EXCEPTION(scope, { });
        uint8_t* out = result->mutableData();
        for (int64_t i = 0, current = start; i < count; ++i, current += step)
            out[i] = memory[current];
        return result;
    }
    if (count <= 0)
        return jsEmptyString(vm);
    auto* text = reinterpret_cast<const wchar_t*>(memory);
    if (step == 1)
        RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, text + start, static_cast<size_t>(count)));
    Vector<wchar_t> selected;
    if (!selected.tryReserveInitialCapacity(static_cast<size_t>(count)))
        return raiseMemoryError(globalObject, scope, String());
    for (int64_t i = 0, current = start; i < count; ++i, current += step)
        selected.append(text[current]);
    RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, selected.span().data(), selected.size()));
}

static JSValue arrayGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            index += array->length;
        RELEASE_AND_RETURN(scope, arrayItem(globalObject, array, index));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "indices must be integers"_s);
    PySlice::Indices indices;
    bool ok = slice->indices(globalObject, array->length, indices);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    PyCTypeInfo* itemInfo = info ? infoOf(globalObject, info->proto.get()) : nullptr;
    if (!itemInfo)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    if (itemInfo->accessor == Accessor { Code::Char } || itemInfo->accessor == Accessor { Code::WChar }) {
        uint8_t* memory = Place { array }.resolve(globalObject, info->size);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, sliceOfCharacters(globalObject, itemInfo->accessor.code, memory, indices.start, indices.step, indices.length));
    }
    JSArray* result = newList(globalObject, static_cast<unsigned>(indices.length));
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0, current = indices.start; i < indices.length; ++i, current += indices.step) {
        JSValue item = arrayItem(globalObject, array, current);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static bool arraySetItemAt(JSGlobalObject* globalObject, PyCData* array, int64_t index, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOfInstance(globalObject, array);
    if (!info || index < 0 || index >= info->length) {
        raiseIndexError(globalObject, scope, "invalid index"_s);
        return false;
    }
    int64_t size = info->size / info->length;
    RELEASE_AND_RETURN(scope, setData(globalObject, PyType::from(info->proto.get()), { }, { array, index * size }, value, index, size));
}

static JSValue arraySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            index += array->length;
        arraySetItemAt(globalObject, array, index, value);
        RETURN_IF_EXCEPTION(scope, { });
        return none();
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "indices must be integer"_s);
    PySlice::Indices indices;
    bool ok = slice->indices(globalObject, array->length, indices);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    int64_t otherLength = length(globalObject, value);
    // What has no length has not the right one.
    if (scope.exception())
        takePendingException(globalObject);
    if (otherLength != indices.length)
        return raiseValueError(globalObject, scope, "Can only assign sequence of same size"_s);
    for (int64_t i = 0, current = indices.start; i < otherLength; ++i, current += indices.step) {
        JSValue item = getItem(globalObject, value, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        arraySetItemAt(globalObject, array, current, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue arrayDelItem(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Array does not support item deletion"_s);
}

static JSValue arrayLength(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, PyCData::from(self)->length); }

static constexpr NativeDef arrayMethods[] = {
    PY_NEW(dataNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", arrayInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__len__", arrayLength, "($self, /)"),
    NativeDef(NativeKind::SlotWrapper, "__getitem__"_s, arrayGetItem, "($self, key, /)"_s, ItemOfSequence),
    PY_SLOT("__setitem__", arraySetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", arrayDelItem, "($self, key, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

const TypeDef arrayTypeDef { "_ctypes.Array"_s, { TypeFlag::Base, TypeFlag::Immutable }, arrayMethods, { }, { } };

// What an array of c_char has besides.

static JSValue charArrayGetRaw(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    uint8_t* memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, { });
    PyBytes* result = newBytes(globalObject, static_cast<size_t>(array->size));
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(result->mutableData(), memory, static_cast<size_t>(array->size));
    return result;
}

static void charArraySetRaw(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (!value) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString("attribute 'raw' of '"_s, array->type()->nameInMessages(), "' objects cannot be deleted"_s));
        return;
    }
    BufferHolder holder(globalObject);
    bool ok = holder.get(value);
    RETURN_IF_EXCEPTION(scope, void());
    ASSERT_UNUSED(ok, ok);
    if (holder.buffer.length > array->size) {
        raiseValueError(globalObject, scope, "byte string too long"_s);
        return;
    }
    uint8_t* memory = Place { array }.resolve(globalObject, holder.buffer.length);
    RETURN_IF_EXCEPTION(scope, void());
    memmove(memory, holder.buffer.data, static_cast<size_t>(holder.buffer.length));
}

static JSValue charArrayGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    uint8_t* memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, load(globalObject, { Code::CharArray }, memory, array->size));
}

static void charArraySetValue(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete attribute"_s);
        return;
    }
    auto* bytes = PyBytes::tryFrom(value);
    if (!bytes) {
        raiseTypeError(globalObject, scope, "bytes expected instead of "_s, typeName(globalObject, value), " instance"_s);
        return;
    }
    int64_t size = static_cast<int64_t>(bytes->length());
    if (size > array->size) {
        raiseValueError(globalObject, scope, "byte string too long"_s);
        return;
    }
    uint8_t* memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, void());
    memcpy(memory, bytes->data(), static_cast<size_t>(size));
    if (size < array->size)
        memory[size] = 0;
}

static JSValue wcharArrayGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    uint8_t* memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, load(globalObject, { Code::WCharArray }, memory, array->size));
}

static void wcharArraySetValue(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete attribute"_s);
        return;
    }
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, "unicode string expected instead of "_s, typeName(globalObject, value), " instance"_s);
        return;
    }
    size_t length;
    PyByteStorage* storage = wideCharactersOf(globalObject, strValue(value), length);
    RETURN_IF_EXCEPTION(scope, void());
    size_t room = static_cast<size_t>(array->size) / sizeof(wchar_t);
    if (length > room) {
        raiseValueError(globalObject, scope, "string too long"_s);
        return;
    }
    uint8_t* memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, void());
    memcpy(memory, storage->data(), (length < room ? length + 1 : length) * sizeof(wchar_t));
    ensureStillAliveHere(storage);
}

static constexpr NativeGetSet charArrayGetSetTable[] = {
    { "raw"_s, charArrayGetRaw, charArraySetRaw, false, "value"_s },
    { "value"_s, charArrayGetValue, charArraySetValue, false, "string value"_s },
};

static constexpr NativeGetSet wcharArrayGetSetTable[] = {
    { "value"_s, wcharArrayGetValue, wcharArraySetValue, false, "string value"_s },
};

std::span<const NativeGetSet> charArrayGetSets() { return charArrayGetSetTable; }
std::span<const NativeGetSet> wcharArrayGetSets() { return wcharArrayGetSetTable; }

// ---- _Pointer ----

// What the pointer holds. False: an exception; a null pointer is one.
static bool pointerTarget(JSGlobalObject* globalObject, PyCData* pointer, uint8_t*& target)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint8_t* memory = Place { pointer }.resolve(globalObject, sizeof(void*));
    RETURN_IF_EXCEPTION(scope, false);
    memcpy(&target, memory, sizeof(void*));
    if (!target) {
        raiseValueError(globalObject, scope, "NULL pointer access"_s);
        return false;
    }
    return true;
}

// The type of what it points to, with what that knows. False: an exception.
static bool pointerItemType(JSGlobalObject* globalObject, PyCData* pointer, PyType*& type, PyCTypeInfo*& itemInfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOfInstance(globalObject, pointer);
    type = info ? PyType::tryFrom(info->proto.get()) : nullptr;
    itemInfo = type ? infoOf(globalObject, type) : nullptr;
    if (!itemInfo) {
        raiseTypeError(globalObject, scope, "abstract class"_s);
        return false;
    }
    return true;
}

static JSValue pointerItem(JSGlobalObject* globalObject, PyCData* pointer, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint8_t* target;
    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerTarget(globalObject, pointer, target)) || failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    Provenance provenance;
    if (failed(scope, checkPointerAccess(globalObject, pointer, target, target + index * itemInfo->size, itemInfo->size, provenance)))
        return { };
    RELEASE_AND_RETURN(scope, getData(globalObject, type, { }, { pointer, 0, target + index * itemInfo->size }, index, itemInfo->size));
}

static JSValue pointerGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, pointerItem(globalObject, pointer, index));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "Pointer indices must be integer"_s);
    // A pointer has no length, so nothing counts from the end.
    int64_t step = 1;
    if (!isNone(slice->step())) {
        step = toSsize(globalObject, slice->step(), rt.ValueError());
        RETURN_IF_EXCEPTION(scope, { });
        if (!step)
            return raiseValueError(globalObject, scope, "slice step cannot be zero"_s);
    }
    int64_t start = 0;
    if (isNone(slice->start())) {
        if (step < 0)
            return raiseValueError(globalObject, scope, "slice start is required for step < 0"_s);
    } else {
        start = toSsize(globalObject, slice->start(), rt.ValueError());
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isNone(slice->stop()))
        return raiseValueError(globalObject, scope, "slice stop is required"_s);
    int64_t stop = toSsize(globalObject, slice->stop(), rt.ValueError());
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count;
    if ((step > 0 && start > stop) || (step < 0 && start < stop))
        count = 0;
    else if (step > 0)
        count = (stop - start - 1) / step + 1;
    else
        count = (stop - start + 1) / step + 1;

    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    if (itemInfo->accessor == Accessor { Code::Char } || itemInfo->accessor == Accessor { Code::WChar }) {
        uint8_t* target = nullptr;
        if (count > 0 && failed(scope, pointerTarget(globalObject, pointer, target)))
            return { };
        if (count > 0) {
            int64_t first = std::min(start, start + (count - 1) * step);
            int64_t last = std::max(start, start + (count - 1) * step);
            Provenance provenance;
            if (failed(scope, checkPointerAccess(globalObject, pointer, target, target + first * itemInfo->size, (last - first + 1) * itemInfo->size, provenance)))
                return { };
        }
        RELEASE_AND_RETURN(scope, sliceOfCharacters(globalObject, itemInfo->accessor.code, target, start, step, count));
    }
    if (count > std::numeric_limits<int32_t>::max())
        return raiseMemoryError(globalObject, scope, String());
    JSArray* result = newList(globalObject, static_cast<unsigned>(count));
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0, current = start; i < count; ++i, current += step) {
        JSValue item = pointerItem(globalObject, pointer, current);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue pointerSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    int64_t index = toSsize(globalObject, key, rt.IndexError());
    RETURN_IF_EXCEPTION(scope, { });
    uint8_t* target;
    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerTarget(globalObject, pointer, target)) || failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    Provenance provenance;
    if (failed(scope, checkPointerAccess(globalObject, pointer, target, target + index * itemInfo->size, itemInfo->size, provenance)))
        return { };
    setData(globalObject, type, { }, { pointer, 0, target + index * itemInfo->size }, value, index, itemInfo->size);
    // Converting the value may have run code that made the pointer point elsewhere.
    ensureStillAliveHere(provenance.owner);
    RETURN_IF_EXCEPTION(scope, { });
    return none();
}

static JSValue pointerDelItem(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Pointer does not support item deletion"_s);
}

static JSValue pointerGetContents(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    uint8_t* target;
    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerTarget(globalObject, pointer, target)) || failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    RELEASE_AND_RETURN(scope, dataAt(globalObject, type, { pointer, 0, target }, 0));
}

static void pointerSetContents(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "Pointer does not support item deletion"_s);
        return;
    }
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    PyCData* target = PyCData::tryFrom(value);
    if (!info || !target) {
        PyType* proto = info ? PyType::tryFrom(info->proto.get()) : nullptr;
        raiseTypeError(globalObject, scope, "expected "_s, proto ? proto->nameInMessages() : "ctypes"_str, " instead of "_s, typeName(globalObject, value));
        return;
    }
    uint8_t* address = target->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    uint8_t* memory = Place { pointer }.resolve(globalObject, sizeof(void*));
    RETURN_IF_EXCEPTION(scope, void());
    memcpy(memory, &address, sizeof(void*));
    // What it points to has to live, and so has what that keeps alive.
    if (failed(scope, keepAlive(globalObject, pointer, 1, value)))
        return;
    RELEASE_AND_RETURN(scope, void(keepAlive(globalObject, pointer, 0, keptObjects(globalObject, target))));
}

static JSValue pointerNew(JSGlobalObject* globalObject, JSValue type, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info || !info->proto)
        return raiseTypeError(globalObject, scope, "Cannot create instance: has no _type_"_s);
    RELEASE_AND_RETURN(scope, newData(globalObject, PyType::from(type)));
}

static JSValue pointerInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "POINTER"_s, args, 0, 1)))
        return { };
    if (args.size()) {
        pointerSetContents(globalObject, self, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue pointerIsNotNull(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint8_t* memory = Place { PyCData::from(self) }.resolve(globalObject, sizeof(void*));
    RETURN_IF_EXCEPTION(scope, { });
    void* target;
    memcpy(&target, memory, sizeof(void*));
    return jsBoolean(!!target);
}

static constexpr NativeDef pointerMethods[] = {
    PY_NEW(pointerNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", pointerInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__bool__", pointerIsNotNull, "($self, /)"),
    NativeDef(NativeKind::SlotWrapper, "__getitem__"_s, pointerGetItem, "($self, key, /)"_s, ItemOfSequence),
    PY_SLOT("__setitem__", pointerSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", pointerDelItem, "($self, key, /)"),
};

static constexpr NativeGetSet pointerGetSets[] = {
    { "contents"_s, pointerGetContents, pointerSetContents, false, "the object this pointer points to (read-write)"_s },
};

const TypeDef pointerTypeDef { "_ctypes._Pointer"_s, { TypeFlag::Base, TypeFlag::Immutable }, pointerMethods, pointerGetSets, { } };

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
