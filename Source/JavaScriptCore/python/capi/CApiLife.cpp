/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApiInstance.h"

#include "CApiType.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "PyAbstract.h"

#if PYTHON_HAS_CAPI

// Memory, how an instance of the type of an extension comes to be, and how C gets at what it keeps there. How it ends: CApiEnd.cpp.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(Life)

// ---- Memory that is not an object's ----

// Never less than this: PyObject_Free() tells memory from cells by the address, and an allocator may hand out smaller blocks at 8 more than a multiple of 16 (mimalloc does).
static constexpr size_t leastSize = 16;

static ALWAYS_INLINE void* allocateMemory(size_t size)
{
    void* result = nullptr;
    return tryFastMalloc(std::max(size, leastSize)).getValue(result) ? result : nullptr;
}

static ALWAYS_INLINE void* allocateZeroedMemory(size_t count, size_t size)
{
    CheckedSize total = count;
    total *= size;
    void* result = nullptr;
    return !total.hasOverflowed() && tryFastZeroedMalloc(std::max(total.value(), leastSize)).getValue(result) ? result : nullptr;
}

static ALWAYS_INLINE void* reallocateMemory(void* memory, size_t size)
{
    void* result = nullptr;
    return WTF::tryFastRealloc(memory, std::max(size, leastSize)).getValue(result) ? result : nullptr;
}

// Most of what C allocates hangs on an object, whose tp_dealloc frees it, and that waits for a collection. So collections have to come as this memory grows, not only as cells are
// made: the heap is told, as an embedder tells it of memory of which it cannot say on which cell it hangs. (It forgets at each full collection. Nobody tells it of what is freed.)
// By who has the GIL, which these functions demand, but for the Raw ones. Nobody is refused. Each thread adds up what it has allocated, small blocks too, and tells when that has come to something.
static constinit thread_local size_t t_bytesNotToldOf = 0;
static constexpr size_t leastToTellOf = 8 * KB;

static NEVER_INLINE void tellHeap()
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    size_t bytes = std::exchange(t_bytesNotToldOf, 0);
    // (Then it is not told at all: most of it may have been freed by the time somebody could tell.)
    if (!hasGIL(current))
        return;
    // It does not collect here and now, but where a cell is next asked for. C may be half way through a change to an object (the new length is stored, the memory is being asked
    // for), and counts on it that nobody calls its tp_traverse meanwhile.
    DeferGCForAWhile deferGC(current->vm);
    current->vm.heap.deprecatedReportExtraMemory(bytes);
}

static ALWAYS_INLINE void* tellingHeap(void* memory, size_t size)
{
    if (memory) [[likely]] {
        t_bytesNotToldOf += size;
        if (t_bytesNotToldOf >= leastToTellOf) [[unlikely]]
            tellHeap();
    }
    return memory;
}

// A buffer that grows by small steps is reallocated thousands of times, and is not that many times its size. How large a block was nobody can tell here (fastMallocSize() knows on Darwin only). So
// it counts when it has moved: an allocator moves a growing block when it has outgrown its size class, and those grow by a factor, so what is told adds up to a few times what is held.
static ALWAYS_INLINE void* reallocateTellingHeap(void* memory, size_t size)
{
    void* result = reallocateMemory(memory, size);
    return tellingHeap(result, result != memory ? size : 0);
}

PY_CAPI void* PyMem_Malloc(size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return tellingHeap(allocateMemory(size), size);
}

PY_CAPI void* PyMem_Calloc(size_t count, size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return tellingHeap(allocateZeroedMemory(count, size), count * size);
}

PY_CAPI void* PyMem_Realloc(void* memory, size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return reallocateTellingHeap(memory, size);
}

PY_CAPI void* PyMem_RawMalloc(size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return allocateMemory(size);
}

PY_CAPI void* PyMem_RawCalloc(size_t count, size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return allocateZeroedMemory(count, size);
}

PY_CAPI void* PyMem_RawRealloc(void* memory, size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return reallocateMemory(memory, size);
}

PY_CAPI void* PyObject_Malloc(size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return tellingHeap(allocateMemory(size), size);
}

PY_CAPI void* PyObject_Calloc(size_t count, size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return tellingHeap(allocateZeroedMemory(count, size), count * size);
}

PY_CAPI void* PyObject_Realloc(void* memory, size_t size)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    return reallocateTellingHeap(memory, size);
}

PY_CAPI void PyMem_Free(void* memory)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    fastFree(memory);
}

PY_CAPI void PyMem_RawFree(void* memory)
{
    CAPI_QUIET_AND_ASKS_NOBODY();
    fastFree(memory);
}

// Whether that is the address of a cell, for an address that is either that or memory from PyObject_Malloc(), which is 16-byte aligned.
static bool isCell(VM& vm, void* memory)
{
    if (PreciseAllocation::isPreciseAllocation(static_cast<HeapCell*>(memory))) {
        vm.heap.objectSpace().enablePreciseAllocationTracking();
        return vm.heap.objectSpace().preciseAllocationSet()->contains(static_cast<HeapCell*>(memory));
    }
    return vm.heap.objectSpace().blocks().set().contains(MarkedBlock::blockFor(memory));
}

// The memory of an object is the collector's to free. In a tp_dealloc there is nothing to do. Elsewhere (a constructor that gives up) this is the end of the object without a tp_dealloc, and
// with it go the references that C thought it had to it.
static void freeObject(JSCell* cell)
{
    Body* body = Body::tryFromCellOfHandle(cell);
    if (!body || body->life() >= Body::Life::BeingDestroyed)
        return;
    Interpreter& current = interpreter();
    didSeeSomebodyTake(body);
    body->setLife(Body::Life::Destroyed);
    current.counts.forget(body);
    // What follows in C is Py_DECREF(type): in CPython each instance owns a reference to its type. Here one whose type has no tp_dealloc owns none, because nobody would give it back.
    if (!ownsReferenceToType(madeByExtension(body->type())))
        retain(current, toC(body->type()));
}

// tp_free
PY_CAPI void PyObject_GC_Del(void* object)
{
    CAPI_QUIET();
    if (object)
        freeObject(static_cast<JSCell*>(object));
}

// tp_free of a type without Py_TPFLAGS_HAVE_GC, and the counterpart of PyObject_Malloc().
PY_CAPI void PyObject_Free(void* memory)
{
    CAPI_QUIET(); // (Who may not come in cannot tell. It is not freed.)
    if (!memory)
        return;
    if (isCell(interpreter().vm, memory))
        freeObject(static_cast<JSCell*>(memory));
    else
        fastFree(memory);
}

// ---- Birth ----

static Face* tryMake(JSGlobalObject* globalObject, PyType* type, MadeByExtension* extension)
{
    Interpreter& current = interpreter();
    Body* body = Body::tryCreate(current.vm, current.state->bodyStructure.get(), type, extension->dataEnd);
    if (!body)
        return nullptr;
    Face* face = Face::create(current.vm, current.state->faceStructure.get(), type, body);
    body->setFace(current.vm, face);
    body->thread = current.runtime.currentThread();
    body->isTracked = extension->flags & HaveGC;
    if (body->isTracked || extension->hasSomethingToDestroy())
        current.youngBodies.append(body);
    if (ownsReferenceToType(extension))
        retain(current, toC(type));
    didMake(globalObject, face, extension);
    return face;
}

// For object.__new__(), which is who makes the instances (PyType::maker()): such a type takes the maker of its base like any class, and there is no slot for another yet. How an instance is laid
// out is for the type to say.
JSValue allocateInstance(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    MadeByExtension* extension = madeByExtension(type);
    ASSERT(extension);
    if (Face* face = tryMake(globalObject, type, extension))
        return face;
    return raiseMemoryError(globalObject, scope, String());
}

Face* faceOfSlow(Body* body)
{
    Interpreter& current = interpreter();
    ASSERT(current.vm.currentThreadIsHoldingAPILock() && !t_isInTraverse);
    // Whoever is given it may keep it.
    didSeeSomebodyTake(body);
    if (Face* face = body->face())
        return face;
    // It has died: the instance was garbage, and C has come by it all the same (a pointer that it does not own), or hands it on in the middle of its end. What went with the
    // face is gone: the attributes, the weak references.
    Face* face = Face::create(current.vm, current.state->faceStructure.get(), body->type(), body);
    body->setFace(current.vm, face);
    return face;
}

JSValue objectOfBody(Runtime& runtime, JSCell* cell)
{
    if (!isSetUpFor(runtime))
        return { };
    Body* body = Body::tryFromCell(cell);
    return body ? faceOf(body) : JSValue();
}

void didChangeClass(JSCell* cell, PyType* oldType, PyType* newType)
{
    Face* face = Face::tryFromCell(cell);
    if (!face)
        return;
    UNUSED_PARAM(oldType);
    Interpreter& current = interpreter();
    Body* body = face->body();
    // What has been destroyed has given its reference back.
    if (body->life() >= Body::Life::BeingDestroyed) {
        body->setType(current.vm, newType);
        return;
    }
    // Whoever assigns has the face in hand. (A body that was let go of is of the face's class again by that, which is the new one.)
    bodyOfFaceInHand(face);
    // tp_dealloc gives back a reference to the type that the instance has then.
    moveToClass(current, body, newType, &current.vm);
}

static Handle allocate(Entry& entry, Handle typeHandle)
{
    PyType* type = PyType::from(toJS(typeHandle));
    MadeByExtension* extension = madeByExtension(type);
    if (!extension) {
        entry.raise(entry.runtime().SystemError(), makeString("the C API cannot allocate an instance of '"_s, type->name(), '\''));
        return nullptr;
    }
    Face* face = tryMake(entry.globalObject, type, extension);
    if (!face) {
        entry.raise(entry.runtime().MemoryError(), String());
        return nullptr;
    }
    return entry.newReference(face);
}

// tp_alloc
PY_CAPI Handle PyType_GenericAlloc(Handle type, ssize_t)
{
    CAPI_ENTER(nullptr);
    return allocate(entry, type);
}

// PyObject_New()
PY_CAPI Handle _PyObject_New(Handle type)
{
    CAPI_ENTER(nullptr);
    return allocate(entry, type);
}

// PyObject_GC_New(): it is for the caller to say when the object is filled in.
PY_CAPI Handle _PyObject_GC_New(Handle type)
{
    CAPI_ENTER(nullptr);
    Handle result = allocate(entry, type);
    if (result)
        bodyOrNull(result)->isTracked = false;
    return result;
}

// ---- The data ----

static ALWAYS_INLINE void* typeData(Handle object, Handle typeHandle)
{
    Body* body = bodyOrNull(object);
    if (!body || !body->hasData()) [[unlikely]]
        return nullptr;
    PyType* type = PyType::tryFrom(toLookAt(typeHandle));
    auto* made = type ? MadeByExtension::of(type) : nullptr;
    if (!made) [[unlikely]]
        return nullptr;
    // Never memory that the object has not got. (That the object is an instance of the type is for the caller to know, as in CPython.)
    if (made->dataEnd > madeByExtension(body->type())->dataEnd) [[unlikely]]
        return nullptr;
    return static_cast<uint8_t*>(body->data()) + made->dataOffset;
}

// (Who may read may not therefore raise: a tp_traverse.)
static void* raiseThatThereIsNoData(Handle object)
{
    CAPI_ENTER(nullptr);
    Body* body = bodyOrNull(object);
    if (body && !body->hasData())
        entry.raise(entry.runtime().SystemError(), makeString("the object of type '"_s, body->type()->name(), "' has been destroyed"_s));
    else
        entry.raise(entry.runtime().TypeError(), "PyObject_GetTypeData: the object has no data of that type"_s);
    return nullptr;
}

PY_CAPI void* PyObject_GetTypeData(Handle object, Handle type)
{
    CAPI_QUIET_AND_ONLY_READS(nullptr);
    if (void* data = typeData(object, type)) [[likely]]
        return data;
    return raiseThatThereIsNoData(object);
}

// For tp_traverse: no exception.
PY_CAPI void* PyObject_GetTypeData_DuringGC(Handle object, Handle type)
{
    CAPI_QUIET_AND_ONLY_READS(nullptr);
    return typeData(object, type);
}

// ---- Whether tp_traverse may be called ----

PY_CAPI void PyObject_GC_Track(void* object)
{
    CAPI_QUIET();
    // (What is tracked has a tp_traverse.)
    if (Body* body = bodyOrNull(static_cast<Handle>(object)))
        body->isTracked = madeByExtension(body->type())->flags & HaveGC;
}

PY_CAPI void PyObject_GC_UnTrack(void* object)
{
    CAPI_QUIET();
    if (Body* body = bodyOrNull(static_cast<Handle>(object)))
        body->isTracked = false;
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
