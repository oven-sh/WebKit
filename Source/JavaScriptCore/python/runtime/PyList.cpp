/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyList.h"

#include "DeferTermination.h"
#include "GCMemoryOperations.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyJSView.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// list. (Objects/listobject.c in CPython is the reference.)

const ClassInfo PyListObject::s_info = { "PyListObject"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyListObject) };

PyListObject::PyListObject(VM& vm, Structure* structure, PyType* type, Butterfly* butterfly)
    : Base(vm, structure, butterfly)
    , m_type(type, WriteBarrierEarlyInit)
{
}

Structure* PyListObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    // As the structures of the realm's own arrays: see JSGlobalObject::haveABadTime().
    return Structure::create(vm, globalObject, prototype, TypeInfo(DerivedArrayType, StructureFlags), info(), globalObject->isHavingABadTime() ? ArrayWithSlowPutArrayStorage : ArrayWithUndecided);
}

PyListObject* PyListObject::create(VM& vm, Structure* structure, PyType* type)
{
    // As JSArray::tryCreate(): an array always has a butterfly, which is where its length is.
    Butterfly* butterfly;
    bool hasArrayStorage = hasAnyArrayStorage(structure->indexingType());
    if (!hasArrayStorage) [[likely]] {
        unsigned outOfLineStorage = structure->outOfLineCapacity();
        unsigned vectorLength = Butterfly::optimalContiguousVectorLength(structure, 0);
        void* base = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, outOfLineStorage, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::Assert);
        butterfly = Butterfly::fromBase(base, 0, outOfLineStorage);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(0);
        Butterfly::clearRange(structure->indexingType(), butterfly, 0, vectorLength);
    } else {
        butterfly = tryCreateArrayButterfly(vm, nullptr, 0);
        RELEASE_ASSERT_RESOURCE_AVAILABLE(butterfly, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        for (unsigned i = 0; i < BASE_ARRAY_STORAGE_VECTOR_LEN; ++i)
            butterfly->arrayStorage()->m_vector[i].clear();
    }
    auto* list = new (NotNull, allocateCell<PyListObject>(vm)) PyListObject(vm, structure, type, butterfly);
    list->finishCreation(vm);
    // The structure is from before the realm began to have a bad time: as every array of such a realm.
    if (!hasArrayStorage && structure->realm()->isHavingABadTime()) [[unlikely]]
        list->switchToSlowPutArrayStorage(vm);
    return list;
}

// An element, the length, or a property only JS knows of.
static ALWAYS_INLINE bool listIsPropertyOfArray(VM& vm, PropertyName propertyName)
{
    return propertyName.isSymbol() || propertyName == vm.propertyNames->length || parseIndex(propertyName);
}

bool PyListObject::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!listIsPropertyOfArray(vm, propertyName)) {
        bool isFound = JSView::getOwnPropertySlotOfListSubclass(object, propertyName, slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (isFound)
            return true;
    }
    RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(object, globalObject, propertyName, slot));
}

bool PyListObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    if (listIsPropertyOfArray(getVM(globalObject), propertyName))
        return Base::put(cell, globalObject, propertyName, value, slot);
    if (slot.thisValue() != JSValue(cell)) [[unlikely]]
        return JSObject::definePropertyOnReceiver(globalObject, propertyName, value, slot);
    return JSView::put(asObject(cell), propertyName, value, slot.isStrictMode());
}

bool PyListObject::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    if (listIsPropertyOfArray(getVM(globalObject), propertyName))
        return Base::deleteProperty(cell, globalObject, propertyName, slot);
    return JSView::deleteProperty(asObject(cell), propertyName);
}

void PyListObject::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& builder, DontEnumPropertiesMode mode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Base::getOwnPropertyNames(object, globalObject, builder, mode);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, JSView::getOwnPropertyNamesOfListSubclass(object, builder));
}

void PyListObject::setType(VM& vm, PyType* type)
{
    m_type.set(vm, this, type);
}

template<typename Visitor>
void PyListObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyListObject>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
}

DEFINE_VISIT_CHILDREN(PyListObject);

// ---- Storage ----

// The items of a JSArray as plain values in a row, which is how nearly every list is stored. One that is not (JS made it sparse,
// say) is handled an item at a time, through what any JS code would use.
class ListStorage {
public:
    ListStorage(VM& vm, JSArray* list)
        : m_vm(vm)
        , m_list(list)
    {
        if (!list->butterfly()) {
            // An instance of a subclass that has never held anything.
            if (!list->ensureLength(vm, 0))
                return;
        }
        m_isContiguous = !!list->tryMakeWritableContiguous(vm).data();
    }

    explicit operator bool() const { return m_isContiguous; }

    unsigned length() const { return m_list->butterfly()->publicLength(); }
    WriteBarrier<Unknown>* items() const { return m_list->butterfly()->contiguous().data(); }

    // The new items are holes until they are set. False: out of memory.
    bool grow(uint64_t newLength)
    {
        if (newLength > MAX_STORAGE_VECTOR_LENGTH)
            return false;
        return m_list->ensureLength(m_vm, static_cast<unsigned>(newLength));
    }

    void shrink(unsigned newLength)
    {
        Butterfly* butterfly = m_list->butterfly();
        WriteBarrier<Unknown>* data = butterfly->contiguous().data();
        for (unsigned i = butterfly->publicLength(); i-- > newLength;)
            data[i].clear();
        butterfly->setPublicLength(newLength);
    }

    void move(unsigned to, unsigned from, unsigned count)
    {
        if (!count)
            return;
        WriteBarrier<Unknown>* data = items();
        gcSafeMemmove(data + to, data + from, count * sizeof(JSValue));
        // What has moved may have passed the collector by.
        m_vm.writeBarrier(m_list);
    }

    void set(unsigned index, JSValue value) { items()[index].set(m_vm, m_list, value); }

    // The values are not in this list. An empty one is a hole of the list they are in, which reads as None.
    void set(unsigned index, std::span<const JSValue> values)
    {
        WriteBarrier<Unknown>* data = items() + index;
        for (JSValue value : values)
            (data++)->setWithoutWriteBarrier(value ? value : jsUndefined());
        m_vm.writeBarrier(m_list);
    }

    // Room for so many items, which does not change the length. It is no error if there is none.
    void reserve(uint64_t capacity)
    {
        unsigned length = this->length();
        if (capacity > length && grow(capacity))
            m_list->butterfly()->setPublicLength(length);
    }

private:
    VM& m_vm;
    JSArray* m_list;
    bool m_isContiguous { false };
};

// As values or as doubles.
static ALWAYS_INLINE bool hasItemsInARow(JSArray* list)
{
    IndexingType indexingType = list->indexingType();
    return hasContiguous(indexingType) || hasInt32(indexingType) || hasDouble(indexingType);
}

// So many items of such a list, as values, to where they are not. A hole stays one. The caller sees to the barrier.
static ALWAYS_INLINE void copyItems(WriteBarrier<Unknown>* to, JSArray* list, unsigned start, unsigned count)
{
    ASSERT(hasItemsInARow(list) && start + count <= list->butterfly()->publicLength());
    if (hasDouble(list->indexingType())) {
        const double* from = list->butterfly()->contiguousDouble().data() + start;
        for (unsigned i = 0; i < count; ++i)
            to[i].setWithoutWriteBarrier(from[i] == from[i] ? JSValue(JSValue::EncodeAsDouble, from[i]) : JSValue());
        return;
    }
    gcSafeMemcpy(to, list->butterfly()->contiguous().data() + start, count * sizeof(JSValue));
}

void listCopyTo(JSValue* to, JSArray* list)
{
    unsigned length = list->length();
    if (auto values = listValues(list); values.data()) {
        for (unsigned i = 0; i < length; ++i)
            to[i] = values[i] ? values[i] : jsUndefined();
        return;
    }
    if (hasDouble(list->indexingType())) {
        const double* from = list->butterfly()->contiguousDouble().data();
        for (unsigned i = 0; i < length; ++i)
            to[i] = from[i] == from[i] ? JSValue(JSValue::EncodeAsDouble, from[i]) : jsUndefined();
        return;
    }
    for (unsigned i = 0; i < length; ++i)
        to[i] = listGet(list, i);
}

static NEVER_INLINE void raiseListTooLong(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseMemoryError(globalObject, scope, String());
}

JSArray* newList(JSGlobalObject* globalObject, unsigned capacity)
{
    VM& vm = getVM(globalObject);
    JSArray* list = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 0, capacity);
    if (!list) [[unlikely]]
        raiseListTooLong(globalObject);
    return list;
}

// A new list of so many items. nextItem() is asked for them in turn, and allocates nothing. Null: it is too long, which has been raised.
template<typename Functor>
static ALWAYS_INLINE JSArray* newListOf(JSGlobalObject* globalObject, uint64_t length, const Functor& nextItem)
{
    VM& vm = getVM(globalObject);
    JSArray* list = nullptr;
    if (length <= MAX_STORAGE_VECTOR_LENGTH) [[likely]] {
        // Not the structure that was asked for, if code in JS has given Array.prototype an accessor for an index.
        Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
        IndexingType indexingType = structure->indexingType();
        ObjectInitializationScope initialization(vm);
        list = JSArray::tryCreateUninitializedRestricted(initialization, structure, static_cast<unsigned>(length));
        if (list && hasContiguous(indexingType)) [[likely]] {
            WriteBarrier<Unknown>* data = list->butterfly()->contiguous().data();
            for (unsigned i = 0; i < length; ++i)
                data[i].setWithoutWriteBarrier(nextItem());
        } else if (list) {
            for (unsigned i = 0; i < length; ++i)
                list->initializeIndexWithoutBarrier(initialization, i, nextItem(), indexingType);
        }
    }
    if (!list) [[unlikely]]
        raiseListTooLong(globalObject);
    return list;
}

JSArray* newList(JSGlobalObject* globalObject, std::span<const JSValue> items)
{
    const JSValue* item = items.data();
    return newListOf(globalObject, items.size(), [&] ALWAYS_INLINE_LAMBDA { return *item++; });
}

// A new list of so many items, which fill() writes all at once, allocating nothing. Null, with nothing raised: it cannot be made so.
template<typename Functor>
static ALWAYS_INLINE JSArray* tryNewListFilledBy(JSGlobalObject* globalObject, uint64_t length, const Functor& fill)
{
    if (length > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
        return nullptr;
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    if (!hasContiguous(structure->indexingType())) [[unlikely]]
        return nullptr;
    ObjectInitializationScope initialization(getVM(globalObject));
    JSArray* list = JSArray::tryCreateUninitializedRestricted(initialization, structure, static_cast<unsigned>(length));
    if (list) [[likely]]
        fill(list->butterfly()->contiguous().data());
    return list;
}

// The first so many items are there. They are written again behind themselves until they are there `count` times.
static void repeatItems(WriteBarrier<Unknown>* items, unsigned length, uint64_t count)
{
    if (length == 1) {
        JSValue item = items[0].get();
        for (uint64_t i = 1; i < count; ++i)
            items[i].setWithoutWriteBarrier(item);
        return;
    }
    // Twice as many each time.
    uint64_t total = length * count;
    for (uint64_t done = length; done < total;) {
        uint64_t more = std::min(done, total - done);
        gcSafeMemcpy(items + done, items, more * sizeof(JSValue));
        done += more;
    }
}

JSValue listGetSlow(JSArray* list, unsigned index)
{
    if (!hasAnyArrayStorage(list->indexingType()))
        return jsUndefined();
    SparseArrayValueMap* map = list->butterfly()->arrayStorage()->m_sparseMap.get();
    if (!map)
        return jsUndefined();
    auto entry = map->find(index);
    if (entry == map->notFound() || (entry->attributes() & PropertyAttribute::Accessor))
        return jsUndefined();
    return entry->get();
}

// Once a prototype of the array has an accessor for an index (for Array.prototype that goes for every array of the realm), `a[i] = v`
// and push() of JS ask it first where the array has nothing of its own: the setter gets the value, and the array keeps a hole. The
// items of a list are nobody else's: they are defined.
// (An array that takes no more items says so in its own way.)
static ALWAYS_INLINE bool prototypeIsAskedAboutHoles(JSArray* list)
{
    return hasSlowPutArrayStorage(list->indexingType()) && list->structure()->isStructureExtensible();
}

static NEVER_INLINE void listDefine(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    list->putDirectIndex(globalObject, index, value, 0, PutDirectIndexShouldThrow);
}

// list[index] = value in strict code.
static void listSetSlow(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    if (prototypeIsAskedAboutHoles(list)) [[unlikely]] {
        PropertySlot slot(list, PropertySlot::InternalMethodType::GetOwnProperty);
        if (!JSArray::getOwnPropertySlotByIndex(list, globalObject, index, slot))
            return listDefine(globalObject, list, index, value);
    }
    list->methodTable()->putByIndex(list, globalObject, index, value, true);
}

static ALWAYS_INLINE void listPush(JSGlobalObject* globalObject, JSArray* list, JSValue value)
{
    if (prototypeIsAskedAboutHoles(list) && list->length() <= MAX_ARRAY_INDEX) [[unlikely]]
        return listDefine(globalObject, list, list->length(), value);
    list->pushInline(globalObject, value);
}

void listSet(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    VM& vm = getVM(globalObject);
    if (ListStorage storage { vm, list }) [[likely]] {
        ASSERT(index < storage.length());
        storage.set(index, value);
        return;
    }
    // An item that is there, in storage of the other kind.
    if (list->trySetIndexQuickly(vm, index, value)) [[likely]]
        return;
    listSetSlow(globalObject, list, index, value);
}

void listAppend(JSGlobalObject* globalObject, JSArray* list, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listPush(globalObject, list, value);
    RETURN_IF_EXCEPTION(scope, void());
}

void listInsertGap(JSGlobalObject* globalObject, JSArray* list, unsigned index, unsigned count)
{
    if (!count)
        return;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = list->length();
    ASSERT(index <= length);
    uint64_t newLength = static_cast<uint64_t>(length) + count;
    if (ListStorage storage { vm, list }) [[likely]] {
        if (!storage.grow(newLength))
            RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        storage.move(index + count, index, length - index);
        for (unsigned i = 0; i < count && index + i < length; ++i)
            storage.items()[index + i].setWithoutWriteBarrier(jsUndefined());
        for (unsigned i = std::max(index, length); i < index + count; ++i)
            storage.items()[i].setWithoutWriteBarrier(jsUndefined());
        return;
    }
    // JS can give an array any length that fits 32 bits.
    if (newLength > MAX_ARRAY_INDEX + 1ull)
        RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
    for (unsigned i = length; i-- > index;) {
        listSetSlow(globalObject, list, i + count, listGet(list, i));
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < count; ++i) {
        listSetSlow(globalObject, list, index + i, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void listInsert(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (index == list->length())
        RELEASE_AND_RETURN(scope, listAppend(globalObject, list, value));
    listInsertGap(globalObject, list, index, 1);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, listSet(globalObject, list, index, value));
}

void listRemove(JSGlobalObject* globalObject, JSArray* list, unsigned index, unsigned count)
{
    if (!count)
        return;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = list->length();
    ASSERT(index + count <= length);
    if (ListStorage storage { vm, list }) [[likely]] {
        storage.move(index, index + count, length - index - count);
        storage.shrink(length - count);
        return;
    }
    for (unsigned i = index + count; i < length; ++i) {
        listSetSlow(globalObject, list, i - count, listGet(list, i));
        RETURN_IF_EXCEPTION(scope, void());
    }
    scope.release();
    list->setLength(globalObject, length - count, true);
}

void listTruncate(JSGlobalObject* globalObject, JSArray* list, unsigned length)
{
    if (length >= list->length())
        return;
    VM& vm = getVM(globalObject);
    if (ListStorage storage { vm, list }) [[likely]] {
        storage.shrink(length);
        return;
    }
    list->setLength(globalObject, length, true);
}

// Appends items that nothing else can change meanwhile.
static void listAppendSpan(JSGlobalObject* globalObject, JSArray* list, std::span<const JSValue> items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = list->length();
    if (ListStorage storage { vm, list }) [[likely]] {
        if (!storage.grow(length + static_cast<uint64_t>(items.size())))
            RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        for (size_t i = 0; i < items.size(); ++i)
            storage.items()[length + i].setWithoutWriteBarrier(items[i]);
        vm.writeBarrier(list);
        return;
    }
    for (JSValue item : items) {
        listPush(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// What list.extend() asks of an iterable once it has the iterator: how many items there may be. -1: an exception.
static int64_t lengthHintForList(JSGlobalObject* globalObject, JSValue iterable, int64_t length)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t hint = lengthHint(globalObject, iterable, 0);
    RETURN_IF_EXCEPTION(scope, -1);
    // CPython makes room for so many, unless the sum overflows: then it takes the hint for a lie.
    constexpr int64_t maxSsize = std::numeric_limits<int64_t>::max();
    if (hint <= maxSsize - length && hint + length > maxSsize / static_cast<int64_t>(sizeof(void*))) [[unlikely]] {
        raiseMemoryError(globalObject, scope, String());
        return -1;
    }
    return hint;
}

void listExtend(JSGlobalObject* globalObject, JSArray* list, JSValue iterable, LengthHint lengthHint)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == runtime(globalObject).tupleType())
        RELEASE_AND_RETURN(scope, listAppendSpan(globalObject, list, tuple->span()));
    // It may be the list itself, and then its class is not asked for an iterator either.
    if (isExactList(iterable) || (iterable.isCell() && iterable.asCell() == list)) {
        JSArray* other = asList(iterable);
        unsigned count = other->length();
        // (An array that JS has made sparse may say that it has thousands of millions of items.)
        if (count > MAX_STORAGE_VECTOR_LENGTH - std::min<unsigned>(list->length(), MAX_STORAGE_VECTOR_LENGTH)) [[unlikely]]
            RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        if (ListStorage storage { vm, list }; storage && hasItemsInARow(other)) [[likely]] {
            unsigned length = storage.length();
            if (!storage.grow(length + count))
                RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
            // (It may be the other one: its items are looked for now that it has grown.)
            copyItems(storage.items() + length, other, 0, count);
            vm.writeBarrier(list);
            return;
        }
        for (unsigned i = 0; i < count; ++i) {
            listPush(globalObject, list, listGet(other, i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }
    Items items(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, void());
    // We make room if it is one of our own types that says how many there are. What a class of the program's says is a guess.
    bool isOfTheProgram = typeOf(globalObject, iterable)->isHeapType();
    if (!isOfTheProgram || lengthHint == LengthHint::Asked) {
        int64_t hint = lengthHintForList(globalObject, iterable, list->length());
        RETURN_IF_EXCEPTION(scope, void());
        if (hint && !isOfTheProgram) {
            if (ListStorage storage { vm, list })
                storage.reserve(storage.length() + static_cast<uint64_t>(hint));
        }
    }
    while (true) {
        JSValue item = items.next();
        RETURN_IF_EXCEPTION(scope, void());
        if (!item)
            return;
        listPush(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSArray* listFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned capacity = 0;
    if (isExactList(iterable))
        capacity = asList(iterable)->length();
    else if (auto* tuple = PyTuple::tryFrom(iterable))
        capacity = tuple->length();
    JSArray* list = newList(globalObject, capacity);
    RETURN_IF_EXCEPTION(scope, nullptr);
    listExtend(globalObject, list, iterable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return list;
}

void AllItems::take(JSGlobalObject* globalObject, JSValue iterable, ASCIILiteral messageIfNotIterable, WhileInUse whileInUse)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == rt.tupleType()) {
        m_owner = tuple;
        m_span = tuple->span();
        return;
    }
    if (isExactList(iterable)) {
        JSArray* list = asList(iterable);
        if (auto values = listValues(list); values.data() && whileInUse == WhileInUse::NothingRuns) [[likely]] {
            m_owner = list;
            m_span = values;
            return;
        }
        unsigned length = list->length();
        if (length > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
            RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        for (unsigned i = 0; i < length; ++i)
            m_kept.append(listGet(list, i));
    } else {
        JSValue iterator = getIter(globalObject, iterable);
        if (failed(scope, iterator)) {
            if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
                raiseTypeError(globalObject, scope, messageIfNotIterable);
            return;
        }
        Items items(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        lengthHintForList(globalObject, iterator, 0);
        RETURN_IF_EXCEPTION(scope, void());
        while (true) {
            JSValue item = items.next();
            RETURN_IF_EXCEPTION(scope, void());
            if (!item)
                break;
            m_kept.append(item);
            // (There may be no end to them.)
            if (m_kept.hasOverflowed() || m_kept.size() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
                RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        }
    }
    if (m_kept.hasOverflowed()) [[unlikely]]
        RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
    m_span = spanOf(m_kept);
}

PyTuple* tupleFromList(JSGlobalObject* globalObject, JSArray* list)
{
    VM& vm = getVM(globalObject);
    unsigned length = list->length();
    if (length > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        raiseListTooLong(globalObject);
        return nullptr;
    }
    if (auto values = listValues(list); values.data() && std::ranges::find(values, JSValue()) == values.end()) [[likely]] {
        PyTuple* tuple = PyTuple::create(globalObject, values);
        ensureStillAliveHere(list);
        return tuple;
    }
    PyTuple* tuple = PyTuple::create(globalObject, length);
    for (unsigned i = 0; i < length; ++i)
        tuple->initializeAt(vm, i, listGet(list, i));
    return tuple;
}

PyTuple* tupleFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == runtime(globalObject).tupleType())
        return tuple;
    if (isExactList(iterable))
        RELEASE_AND_RETURN(scope, tupleFromList(globalObject, asList(iterable)));
    // Unlike a list, a tuple does not ask how many there will be.
    Items items(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    MarkedArgumentBuffer taken;
    while (true) {
        JSValue item = items.next();
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!item)
            break;
        taken.append(item);
        // (There may be no end to them.)
        if (taken.hasOverflowed() || taken.size() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
            scope.release();
            raiseListTooLong(globalObject);
            return nullptr;
        }
    }
    return PyTuple::create(globalObject, spanOf(taken));
}

// ---- Sorting ----

namespace {

// The sort of CPython's lists ("timsort", Objects/listsort.txt there): a stable merge sort that takes the runs the data already
// has, lengthens short ones by binary insertion, merges them in the order "powersort" says, and gallops through a run that
// keeps winning. Programs can see in which order their __lt__ is called, so each step compares what CPython's compares.
// It sorts keys, and the values that go with them if they are not the keys themselves.
// `keys`: what is known of all the keys beforehand. Comparing numbers or strs runs no code of the program's and cannot fail.
enum class SortedKeys : uint8_t { Any, Int32s, Numbers, Strings };

template<SortedKeys sortedKeys>
class Sorter {
public:
    // All four are arrays of `count` values that the collector knows of. `values` and its scratch space may be null.
    Sorter(JSGlobalObject* globalObject, JSValue* keys, JSValue* values, JSValue* scratchKeys, JSValue* scratchValues, size_t count)
        : m_globalObject(globalObject)
        , m_vm(getVM(globalObject))
        , m_all { keys, values }
        , m_scratch { scratchKeys, scratchValues }
        , m_count(count)
    {
    }

    // False: a comparison threw. The items are all there nonetheless, in some order.
    bool sort()
    {
        if (m_count < 2)
            return true;
        if constexpr (sortedKeys == SortedKeys::Any)
            chooseComparison();

        // Every run is made at least so long. The lengths vary by one, so that they add up to a count that halves evenly.
        unsigned shift = 0;
        while (m_count >> shift >= maxMinimumRun)
            ++shift;
        size_t mask = (static_cast<size_t>(1) << shift) - 1;
        size_t accumulated = 0;

        Slice rest = m_all;
        ptrdiff_t remaining = m_count;
        do {
            ptrdiff_t length = countRun(rest, remaining);
            if (length < 0)
                return false;
            accumulated += m_count;
            ptrdiff_t minimum = accumulated >> shift;
            accumulated &= mask;
            if (length < minimum) {
                ptrdiff_t forced = std::min(remaining, minimum);
                if (!binaryInsertionSort(rest, forced, length))
                    return false;
                length = forced;
            }
            // A short list, or one that was in order.
            if (length == static_cast<ptrdiff_t>(m_count))
                return true;
            if (!mergeBefore(length))
                return false;
            m_runs.append(Run { rest, length, 0 });
            rest.advance(length);
            remaining -= length;
        } while (remaining);

        while (m_runs.size() > 1) {
            size_t at = m_runs.size() - 2;
            if (at > 0 && m_runs[at - 1].length < m_runs[at + 1].length)
                --at;
            if (!mergeAt(at))
                return false;
        }
        return true;
    }

    static constexpr size_t maxMinimumRun = 64;

private:
    static constexpr ptrdiff_t initialMinimumGallop = 7;

    struct Slice {
        JSValue* keys;
        JSValue* values;

        void advance(ptrdiff_t n)
        {
            keys += n;
            if (values)
                values += n;
        }
        void reverse(ptrdiff_t n)
        {
            std::reverse(keys, keys + n);
            if (values)
                std::reverse(values, values + n);
        }
        // this[i] = other[j], and so on for n items. The ranges may overlap.
        void copy(ptrdiff_t i, const Slice& other, ptrdiff_t j, ptrdiff_t n = 1)
        {
            memmove(static_cast<void*>(keys + i), other.keys + j, n * sizeof(JSValue));
            if (values)
                memmove(static_cast<void*>(values + i), other.values + j, n * sizeof(JSValue));
        }
    };

    struct Run {
        Slice start;
        ptrdiff_t length;
        int power;
    };

    enum class Comparison : uint8_t { Generic, Number, String, SameType };

    Comparison comparisonFor(const auto& keyAt)
    {
        bool allNumbers = true;
        bool allStrings = true;
        bool allSameType = true;
        Runtime& rt = runtime(m_globalObject);
        PyType* type = rt.typeOf(keyAt(0));
        for (size_t i = 0; i < m_count && (allNumbers || allStrings || allSameType); ++i) {
            JSValue key = keyAt(i);
            allNumbers &= key.isNumber();
            allStrings &= key.isString() && !asString(key)->isRope();
            allSameType = allSameType && rt.typeOf(key) == type;
        }
        return allNumbers ? Comparison::Number : allStrings ? Comparison::String : allSameType ? Comparison::SameType : Comparison::Generic;
    }

    // What the keys have in common, found once, saves finding out at each comparison how to compare.
    void chooseComparison()
    {
        Runtime& rt = runtime(m_globalObject);
        auto isFilledTuple = [&](JSValue key) {
            auto* tuple = PyTuple::tryFrom(key);
            return tuple && tuple->type() == rt.tupleType() && tuple->length();
        };
        m_keysAreTuples = true;
        for (size_t i = 0; i < m_count && m_keysAreTuples; ++i)
            m_keysAreTuples = isFilledTuple(m_all.keys[i]);
        if (!m_keysAreTuples) {
            m_comparison = comparisonFor([&](size_t i) { return m_all.keys[i]; });
            return;
        }
        // Most comparisons of tuples are decided by the first items.
        m_comparison = comparisonFor([&](size_t i) { return PyTuple::from(m_all.keys[i])->at(0); });
        if (m_comparison == Comparison::SameType && PyTuple::is(PyTuple::from(m_all.keys[0])->at(0)))
            m_comparison = Comparison::Generic;
    }

    static bool stringLess(JSString* a, JSString* b) { return compareStrings(a->tryGetValue().data, b->tryGetValue().data) < 0; }

    // These say whether a < b. After a failure they say no, and m_failed is set. That is what the sorter looks at: the exception
    // stays pending for whoever called sort(), and nothing else runs before it gets there.
    bool genericLess(JSValue a, JSValue b)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        bool result = richCompareBool(m_globalObject, a, b, CompareOperator::Lt);
        if (scope.exception()) [[unlikely]] {
            m_failed = true;
            return false;
        }
        return result;
    }

    bool itemLess(JSValue a, JSValue b)
    {
        switch (m_comparison) {
        case Comparison::Number:
            return a.asNumber() < b.asNumber();
        case Comparison::String:
            return stringLess(asString(a), asString(b));
        case Comparison::SameType: {
            // There is no reflected method to think of, unless this one declines.
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
            Runtime& rt = runtime(m_globalObject);
            JSValue method = rt.typeOf(a)->lookup(Special::Lt);
            if (!method)
                break;
            JSValue result = callSpecial(m_globalObject, method, a, b);
            if (!scope.exception() && result == rt.notImplemented())
                break;
            bool isLess = !scope.exception() && (result.isBoolean() ? result.asBoolean() : isTrue(m_globalObject, result));
            if (scope.exception()) [[unlikely]] {
                m_failed = true;
                return false;
            }
            return isLess;
        }
        case Comparison::Generic:
            break;
        }
        return genericLess(a, b);
    }

    ALWAYS_INLINE bool hasFailed() const { return sortedKeys == SortedKeys::Any && m_failed; }

    ALWAYS_INLINE bool less(JSValue a, JSValue b)
    {
        if constexpr (sortedKeys == SortedKeys::Int32s)
            return a.asInt32() < b.asInt32();
        else if constexpr (sortedKeys == SortedKeys::Numbers)
            return a.asNumber() < b.asNumber();
        else if constexpr (sortedKeys == SortedKeys::Strings)
            return stringLess(asString(a), asString(b));
        else
            return lessOfAnyKind(a, b);
    }

    bool lessOfAnyKind(JSValue a, JSValue b)
    {
        if (m_failed)
            return false;
        if (!m_keysAreTuples)
            return itemLess(a, b);

        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        auto* first = PyTuple::from(a);
        auto* second = PyTuple::from(b);
        unsigned common = std::min(first->length(), second->length());
        unsigned i = 0;
        for (; i < common; ++i) {
            bool equal = richCompareBool(m_globalObject, first->at(i), second->at(i), CompareOperator::Eq);
            if (scope.exception()) [[unlikely]] {
                m_failed = true;
                return false;
            }
            if (!equal)
                break;
        }
        if (i == common)
            return first->length() < second->length();
        return i ? genericLess(first->at(i), second->at(i)) : itemLess(first->at(i), second->at(i));
    }

    // Sorts the first n items, of which the first `sorted` are in order already.
    bool binaryInsertionSort(Slice slice, ptrdiff_t n, ptrdiff_t sorted)
    {
        JSValue* keys = slice.keys;
        for (ptrdiff_t next = std::max<ptrdiff_t>(sorted, 1); next < n; ++next) {
            JSValue pivot = keys[next];
            // It goes after whatever equals it.
            ptrdiff_t low = 0;
            ptrdiff_t high = next;
            do {
                ptrdiff_t middle = (low + high) >> 1;
                bool isLess = less(pivot, keys[middle]);
                if (hasFailed())
                    return false;
                if (isLess)
                    high = middle;
                else
                    low = middle + 1;
            } while (low < high);
            JSValue value = slice.values ? slice.values[next] : JSValue();
            slice.copy(low + 1, slice, low, next - low);
            keys[low] = pivot;
            if (slice.values)
                slice.values[low] = value;
        }
        return true;
    }

    // The length of the run at the start of the slice, which is ascending when this returns: a descending one is turned
    // round, in a way that leaves equal items in their order. -1: failure.
    ptrdiff_t countRun(Slice slice, ptrdiff_t remaining)
    {
        JSValue* keys = slice.keys;
        ptrdiff_t n = 1;
        auto nextIsSmaller = [&] { return less(keys[n], keys[n - 1]); };
        auto extendAscending = [&] {
            for (; n < remaining; ++n) {
                if (nextIsSmaller() || hasFailed())
                    break;
            }
        };

        extendAscending();
        if (hasFailed())
            return -1;
        if (n == remaining)
            return n;

        // keys[n] is smaller. What came before went up somewhere, which ends the run, or is n equal items, with which a
        // descending run begins.
        if (n > 1) {
            bool wentUp = less(keys[0], keys[n - 1]);
            if (hasFailed())
                return -1;
            if (wentUp)
                return n;
            slice.reverse(n);
        }
        ++n;

        // Each stretch of equal items is turned round on the way, and turned back when the whole run is.
        ptrdiff_t equal = 0;
        auto reverseEqualItems = [&] {
            if (!equal)
                return;
            Slice stretch = slice;
            stretch.advance(n - (equal + 1));
            stretch.reverse(equal + 1);
            equal = 0;
        };
        for (; n < remaining; ++n) {
            bool smaller = nextIsSmaller();
            if (hasFailed())
                return -1;
            if (smaller) {
                reverseEqualItems();
                continue;
            }
            bool larger = less(keys[n - 1], keys[n]);
            if (hasFailed())
                return -1;
            if (larger)
                break;
            ++equal;
        }
        reverseEqualItems();
        slice.reverse(n);

        // What follows may well go on upwards from there.
        extendAscending();
        return hasFailed() ? -1 : n;
    }

    // Where the key belongs among n sorted items: before (Left) or after (Right) those equal to it. The search starts at
    // `hint` and moves away from it in steps that double, then halves the last step. -1: failure.
    enum class Side : bool { Left, Right };
    template<Side side>
    ptrdiff_t gallop(JSValue key, JSValue* items, ptrdiff_t n, ptrdiff_t hint)
    {
        // Whether the key belongs to the right of the item.
        auto isAfter = [&](JSValue item) {
            return side == Side::Left ? less(item, key) : !less(key, item);
        };
        ptrdiff_t last = 0;
        ptrdiff_t offset = 1;
        bool goesRight = isAfter(items[hint]);
        if (hasFailed())
            return -1;
        if (goesRight) {
            ptrdiff_t limit = n - hint;
            while (offset < limit) {
                bool further = isAfter(items[hint + offset]);
                if (hasFailed())
                    return -1;
                if (!further)
                    break;
                last = offset;
                offset = (offset << 1) + 1;
            }
            offset = std::min(offset, limit);
            last += hint;
            offset += hint;
        } else {
            ptrdiff_t limit = hint + 1;
            while (offset < limit) {
                bool found = isAfter(items[hint - offset]);
                if (hasFailed())
                    return -1;
                if (found)
                    break;
                last = offset;
                offset = (offset << 1) + 1;
            }
            offset = std::min(offset, limit);
            ptrdiff_t nearer = last;
            last = hint - offset;
            offset = hint - nearer;
        }

        // It belongs to the right of `last` and no further right than `offset`.
        ++last;
        while (last < offset) {
            ptrdiff_t middle = last + ((offset - last) >> 1);
            bool after = isAfter(items[middle]);
            if (hasFailed())
                return -1;
            if (after)
                last = middle + 1;
            else
                offset = middle;
        }
        return offset;
    }

    // Merges two adjacent runs, a (not the longer) and b, from the left. The last of a is the last of all.
    // a steps aside, so that no item is ever in neither place and a failure loses none.
    bool mergeLow(Slice a, ptrdiff_t countA, Slice b, ptrdiff_t countB)
    {
        m_scratch.copy(0, a, 0, countA);
        Slice out = a;
        a = m_scratch;
        auto take = [&](Slice& from, ptrdiff_t n = 1) {
            out.copy(0, from, 0, n);
            out.advance(n);
            from.advance(n);
        };
        auto finish = [&](bool succeeded) {
            if (countA)
                out.copy(0, a, 0, countA);
            return succeeded;
        };
        auto finishWithLastOfA = [&] {
            out.copy(0, b, 0, countB);
            out.copy(countB, a, 0);
            return true;
        };

        take(b);
        if (!--countB)
            return finish(true);
        if (countA == 1)
            return finishWithLastOfA();

        ptrdiff_t minimumGallop = m_minimumGallop;
        while (true) {
            ptrdiff_t wonByA = 0;
            ptrdiff_t wonByB = 0;
            // One at a time, until one of the two keeps winning.
            while (true) {
                bool takesB = less(b.keys[0], a.keys[0]);
                if (hasFailed())
                    return finish(false);
                if (takesB) {
                    take(b);
                    ++wonByB;
                    wonByA = 0;
                    if (!--countB)
                        return finish(true);
                    if (wonByB >= minimumGallop)
                        break;
                } else {
                    take(a);
                    ++wonByA;
                    wonByB = 0;
                    if (--countA == 1)
                        return finishWithLastOfA();
                    if (wonByA >= minimumGallop)
                        break;
                }
            }

            // Then by leaps, for as long as that pays.
            ++minimumGallop;
            do {
                minimumGallop -= minimumGallop > 1;
                m_minimumGallop = minimumGallop;
                wonByA = gallop<Side::Right>(b.keys[0], a.keys, countA, 0);
                if (wonByA < 0)
                    return finish(false);
                if (wonByA) {
                    take(a, wonByA);
                    countA -= wonByA;
                    if (countA == 1)
                        return finishWithLastOfA();
                    // Only comparisons that contradict each other get here.
                    if (!countA)
                        return finish(true);
                }
                take(b);
                if (!--countB)
                    return finish(true);

                wonByB = gallop<Side::Left>(a.keys[0], b.keys, countB, 0);
                if (wonByB < 0)
                    return finish(false);
                if (wonByB) {
                    take(b, wonByB);
                    countB -= wonByB;
                    if (!countB)
                        return finish(true);
                }
                take(a);
                if (--countA == 1)
                    return finishWithLastOfA();
            } while (wonByA >= initialMinimumGallop || wonByB >= initialMinimumGallop);
            ++minimumGallop;
            m_minimumGallop = minimumGallop;
        }
    }

    // The same from the right, for when b is the shorter: b steps aside. The first of b is the first of all.
    bool mergeHigh(Slice a, ptrdiff_t countA, Slice b, ptrdiff_t countB)
    {
        m_scratch.copy(0, b, 0, countB);
        Slice startOfA = a;
        Slice startOfB = m_scratch;
        // These three are at the last item of what is left of each.
        Slice out = b;
        out.advance(countB - 1);
        b = m_scratch;
        b.advance(countB - 1);
        a.advance(countA - 1);
        auto take = [&](Slice& from, ptrdiff_t n = 1) {
            out.copy(1 - n, from, 1 - n, n);
            out.advance(-n);
            from.advance(-n);
        };
        auto finish = [&](bool succeeded) {
            if (countB)
                out.copy(1 - countB, startOfB, 0, countB);
            return succeeded;
        };
        auto finishWithFirstOfB = [&] {
            take(a, countA);
            out.copy(0, b, 0);
            return true;
        };

        take(a);
        if (!--countA)
            return finish(true);
        if (countB == 1)
            return finishWithFirstOfB();

        ptrdiff_t minimumGallop = m_minimumGallop;
        while (true) {
            ptrdiff_t wonByA = 0;
            ptrdiff_t wonByB = 0;
            while (true) {
                bool takesA = less(b.keys[0], a.keys[0]);
                if (hasFailed())
                    return finish(false);
                if (takesA) {
                    take(a);
                    ++wonByA;
                    wonByB = 0;
                    if (!--countA)
                        return finish(true);
                    if (wonByA >= minimumGallop)
                        break;
                } else {
                    take(b);
                    ++wonByB;
                    wonByA = 0;
                    if (--countB == 1)
                        return finishWithFirstOfB();
                    if (wonByB >= minimumGallop)
                        break;
                }
            }

            ++minimumGallop;
            do {
                minimumGallop -= minimumGallop > 1;
                m_minimumGallop = minimumGallop;
                ptrdiff_t position = gallop<Side::Right>(b.keys[0], startOfA.keys, countA, countA - 1);
                if (position < 0)
                    return finish(false);
                wonByA = countA - position;
                if (wonByA) {
                    take(a, wonByA);
                    countA -= wonByA;
                    if (!countA)
                        return finish(true);
                }
                take(b);
                if (--countB == 1)
                    return finishWithFirstOfB();

                position = gallop<Side::Left>(a.keys[0], startOfB.keys, countB, countB - 1);
                if (position < 0)
                    return finish(false);
                wonByB = countB - position;
                if (wonByB) {
                    take(b, wonByB);
                    countB -= wonByB;
                    if (countB == 1)
                        return finishWithFirstOfB();
                    if (!countB)
                        return finish(true);
                }
                take(a);
                if (!--countA)
                    return finish(true);
            } while (wonByA >= initialMinimumGallop || wonByB >= initialMinimumGallop);
            ++minimumGallop;
            m_minimumGallop = minimumGallop;
        }
    }

    // Merges the runs at `index` and after it.
    bool mergeAt(size_t index)
    {
        Slice a = m_runs[index].start;
        ptrdiff_t countA = m_runs[index].length;
        Slice b = m_runs[index + 1].start;
        ptrdiff_t countB = m_runs[index + 1].length;
        m_runs[index].length = countA + countB;
        m_runs.removeAt(index + 1);

        // What of a comes before all of b is in place, as is what of b comes after all of a.
        ptrdiff_t inPlace = gallop<Side::Right>(b.keys[0], a.keys, countA, 0);
        if (inPlace < 0)
            return false;
        a.advance(inPlace);
        countA -= inPlace;
        if (!countA)
            return true;
        countB = gallop<Side::Left>(a.keys[countA - 1], b.keys, countB, countB - 1);
        if (countB <= 0)
            return !countB;
        return countA <= countB ? mergeLow(a, countA, b, countB) : mergeHigh(a, countA, b, countB);
    }

    // How deep in a balanced tree of merges the boundary lies between the run of n1 items at s1 and the n2 items after it:
    // the first binary digit in which the midpoints of the two, as fractions of the whole, differ.
    int powerOf(ptrdiff_t s1, ptrdiff_t n1, ptrdiff_t n2) const
    {
        ptrdiff_t n = m_count;
        ptrdiff_t a = 2 * s1 + n1;
        ptrdiff_t b = a + n1 + n2;
        int result = 0;
        while (true) {
            ++result;
            if (a >= n) {
                a -= n;
                b -= n;
            } else if (b >= n)
                return result;
            a <<= 1;
            b <<= 1;
        }
    }

    // A run of `length` items has been found after those on the stack: merges those that are not to wait for it.
    bool mergeBefore(ptrdiff_t length)
    {
        if (m_runs.isEmpty())
            return true;
        int power = powerOf(m_runs.last().start.keys - m_all.keys, m_runs.last().length, length);
        while (m_runs.size() > 1 && m_runs[m_runs.size() - 2].power > power) {
            if (!mergeAt(m_runs.size() - 2))
                return false;
        }
        m_runs.last().power = power;
        return true;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    Slice m_all;
    Slice m_scratch;
    size_t m_count;
    // Not in the frame: the comparisons, which are code of the program's, run under it, and room that is not written to holds whatever
    // old words lie there for the collector to find.
    Vector<Run> m_runs;
    ptrdiff_t m_minimumGallop { initialMinimumGallop };
    Comparison m_comparison { Comparison::Generic };
    bool m_keysAreTuples { false };
    bool m_failed { false };
};

}

static SortedKeys whatAllAre(std::span<const WriteBarrier<Unknown>> items)
{
    bool allInt32s = true;
    bool allNumbers = true;
    bool allStrings = true;
    for (auto& slot : items) {
        JSValue item = slot.get();
        if (!item) [[unlikely]]
            return SortedKeys::Any;
        allInt32s &= item.isInt32();
        allNumbers &= item.isNumber();
        allStrings &= item.isString() && !asString(item)->isRope();
        if (!allNumbers && !allStrings)
            return SortedKeys::Any;
    }
    return allInt32s ? SortedKeys::Int32s : allNumbers ? SortedKeys::Numbers : allStrings ? SortedKeys::Strings : SortedKeys::Any;
}

// Nothing gets to see the list while such items are sorted, so it keeps them meanwhile, and the collector need not know of the copy.
template<SortedKeys sortedKeys>
static NEVER_INLINE bool sortItemsThatAreAll(JSGlobalObject* globalObject, JSArray* list, std::span<WriteBarrier<Unknown>> items, bool reverse)
{
    // A list that is a single run is not merged.
    constexpr size_t shortList = Sorter<sortedKeys>::maxMinimumRun;
    Vector<JSValue, shortList> values;
    Vector<JSValue, 0> scratch;
    if (!values.tryReserveInitialCapacity(items.size()) || (items.size() >= shortList && !scratch.tryGrow(items.size())))
        return false;
    for (auto& slot : items)
        values.append(slot.get());
    // Reversing before and after keeps equal items in their original order.
    if (reverse)
        values.reverse();
    Sorter<sortedKeys>(globalObject, values.mutableSpan().data(), nullptr, scratch.mutableSpan().data(), nullptr, values.size()).sort();
    if (reverse)
        values.reverse();
    for (size_t i = 0; i < items.size(); ++i)
        items[i].setWithoutWriteBarrier(values[i]);
    getVM(globalObject).writeBarrier(list);
    return true;
}

// In a frame of its own, which is not there yet while the key function is called.
static NEVER_INLINE bool sortItemsOfAnyKind(JSGlobalObject* globalObject, JSValue* keys, JSValue* values, JSValue* scratchKeys, JSValue* scratchValues, size_t count)
{
    return Sorter<SortedKeys::Any>(globalObject, keys, values, scratchKeys, scratchValues, count).sort();
}

void listSort(JSGlobalObject* globalObject, JSArray* list, JSValue key, bool reverse)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = list->length();
    if (count > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
        RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));

    // All the room that it takes, while the list is still as it was.
    bool hasKey = key && !isNone(key);
    MarkedArgumentBuffer values;
    MarkedArgumentBuffer keys;
    MarkedArgumentBuffer scratch;
    MarkedArgumentBuffer secondScratch;
    values.ensureCapacity(count);
    scratch.fill(vm, count, [](JSValue* slots) { UNUSED_PARAM(slots); });
    if (hasKey) {
        keys.ensureCapacity(count);
        secondScratch.fill(vm, count, [](JSValue* slots) { UNUSED_PARAM(slots); });
    }
    if (values.hasOverflowed() || keys.hasOverflowed() || scratch.hasOverflowed() || secondScratch.hasOverflowed()) [[unlikely]]
        RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));

    if (ListStorage storage { vm, list }; storage && (!key || isNone(key))) {
        std::span items { storage.items(), count };
        bool isSorted = false;
        switch (whatAllAre(items)) {
        case SortedKeys::Int32s:
            isSorted = sortItemsThatAreAll<SortedKeys::Int32s>(globalObject, list, items, reverse);
            break;
        case SortedKeys::Numbers:
            isSorted = sortItemsThatAreAll<SortedKeys::Numbers>(globalObject, list, items, reverse);
            break;
        case SortedKeys::Strings:
            isSorted = sortItemsThatAreAll<SortedKeys::Strings>(globalObject, list, items, reverse);
            break;
        case SortedKeys::Any:
            break;
        }
        if (isSorted)
            return;
    }

    // The list is empty for as long as it is being sorted, so that what the comparisons do to it shows.
    for (unsigned i = 0; i < count; ++i)
        values.append(listGet(list, i));
    // Not one that JS has forbidden to grow, which could not be filled again.
    bool staysFilled = !list->structure()->isStructureExtensible();
    if (!staysFilled) {
        // From here on the items are nowhere but in `values`. It is not here that the host ends this, but in what is called below.
        DeferTerminationForAWhile deferred(vm);
        listTruncate(globalObject, list, 0);
        RETURN_IF_EXCEPTION(scope, void());
    }
    JSValue* valueData = reinterpret_cast<JSValue*>(const_cast<EncodedJSValue*>(ArgList(values).data()));

    // Even if it is empty again afterwards. Whatever puts an item in the list writes to the first slot of its storage, and whatever
    // takes it out again empties the slot: a mark left there, beyond the end of the list, does not survive that.
    Butterfly* markedButterfly = list->butterfly();
    IndexingType markedShape = list->indexingType() & IndexingShapeMask;
    bool isMarked = !staysFilled && markedButterfly && markedButterfly->vectorLength() && (markedShape == Int32Shape || markedShape == ContiguousShape);
    if (isMarked)
        markedButterfly->contiguous().atUnsafe(0).setWithoutWriteBarrier(jsNumber(0));

    auto reverseAll = [&](JSValue* data) {
        if (data)
            std::reverse(data, data + count);
    };

    JSValue* keyData = nullptr;
    bool failed = false;
    if (hasKey) {
        for (unsigned i = 0; i < count; ++i) {
            JSValue computed = call(globalObject, key, valueData[i]);
            if (scope.exception()) [[unlikely]] {
                failed = true;
                break;
            }
            keys.append(computed);
        }
        keyData = reinterpret_cast<JSValue*>(const_cast<EncodedJSValue*>(ArgList(keys).data()));
    }

    if (!failed) {
        // Reversing before and after keeps equal items in their original order.
        if (reverse) {
            reverseAll(valueData);
            reverseAll(keyData);
        }
        auto dataOf = [](MarkedArgumentBuffer& buffer) { return reinterpret_cast<JSValue*>(const_cast<EncodedJSValue*>(ArgList(buffer).data())); };
        failed = !sortItemsOfAnyKind(globalObject, keyData ? keyData : valueData, keyData ? valueData : nullptr, dataOf(scratch), keyData ? dataOf(secondScratch) : nullptr, count);
        if (reverse)
            reverseAll(valueData);
    }

    bool wasModified = staysFilled ? list->length() != count : list->length();
    if (isMarked && !wasModified) {
        wasModified = list->butterfly() != markedButterfly || (list->indexingType() & IndexingShapeMask) != markedShape || markedButterfly->contiguous().atUnsafe(0).get() != jsNumber(0);
        if (!wasModified)
            markedButterfly->contiguous().atUnsafe(0).clear();
    }
    JSValue pending;
    if (failed)
        pending = takePendingException(globalObject);
    if (staysFilled) {
        // It has its items still, if the host has ended the sort. What is put into it may go to a setter of JS, which can be ended; nothing else
        // ends this halfway.
        RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
        for (unsigned i = 0; i < count && i < list->length(); ++i) {
            listSet(globalObject, list, i, valueData[i]);
            RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
        }
    } else {
        // The items go back even if the host has ended the sort, or is about to: the program may go on.
        std::optional<ExceptionSetAside> suspended;
        if (failed && !pending) [[unlikely]]
            suspended.emplace(globalObject);
        DeferTerminationForAWhile deferred(vm);
        listTruncate(globalObject, list, 0);
        RETURN_IF_EXCEPTION(scope, void());
        listAppendSpan(globalObject, list, { valueData, count });
        RETURN_IF_EXCEPTION(scope, void());
    }
    RETURN_IF_EXCEPTION(scope, void());
    if (pending) {
        raiseException(globalObject, scope, pending);
        return;
    }
    if (wasModified)
        raiseValueError(globalObject, scope, "list modified during sort"_s);
}

// ---- What list and tuple share ----

namespace {

struct ListItems {
    static constexpr bool lengthsAreComparedFirst = true;
    JSArray* list;
    ALWAYS_INLINE unsigned length() const { return list->length(); }
    ALWAYS_INLINE JSValue at(unsigned index) const { return listGet(list, index); }
    ALWAYS_INLINE std::span<const JSValue> values() const { return listValues(list); }
    // The items as they lie in memory, if those that are the same there are the same items. Until something changes the list.
    ALWAYS_INLINE std::span<const uint64_t> words(bool& areDoubles) const
    {
        if (!hasItemsInARow(list))
            return { };
        areDoubles = hasDouble(list->indexingType());
        Butterfly* butterfly = list->butterfly();
        return { std::bit_cast<const uint64_t*>(butterfly->contiguous().data()), butterfly->publicLength() };
    }
};

struct TupleItems {
    static constexpr bool lengthsAreComparedFirst = false;
    PyTuple* tuple;
    ALWAYS_INLINE unsigned length() const { return tuple->length(); }
    ALWAYS_INLINE JSValue at(unsigned index) const { return tuple->at(index); }
    ALWAYS_INLINE std::span<const JSValue> values() const { return tuple->span(); }
    ALWAYS_INLINE std::span<const uint64_t> words(bool&) const { return { std::bit_cast<const uint64_t*>(tuple->span().data()), tuple->length() }; }
};

}

template<typename Items>
static JSValue compareSequences(JSGlobalObject* globalObject, CompareOperator op, Items first, Items second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // (Two tuples are not asked for their lengths first.)
    if (Items::lengthsAreComparedFirst && first.length() != second.length() && (op == CompareOperator::Eq || op == CompareOperator::NotEq))
        return jsBoolean(op == CompareOperator::NotEq);

    // The first pair of items that differ decides. The lengths may change as the items are compared.
    unsigned i = 0;
    for (;; ++i) {
        // Nothing is asked about a pair of items that are one and the same, so nothing changes while those are passed over.
        bool firstAreDoubles = false;
        bool secondAreDoubles = false;
        auto firstWords = first.words(firstAreDoubles);
        auto secondWords = second.words(secondAreDoubles);
        size_t common = firstAreDoubles == secondAreDoubles ? std::min(firstWords.size(), secondWords.size()) : 0;
        while (i < common && firstWords[i] == secondWords[i])
            ++i;
        if (i >= first.length() || i >= second.length())
            break;
        JSValue a = first.at(i);
        JSValue b = second.at(i);
        bool equal = isEqual(globalObject, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            continue;
        // If the comparison took the items away, the lengths decide after all.
        if (i >= first.length() || i >= second.length())
            break;
        if (op == CompareOperator::Eq)
            return jsBoolean(false);
        if (op == CompareOperator::NotEq)
            return jsBoolean(true);
        // (What is there now: the comparison may have put other items in their place.)
        RELEASE_AND_RETURN(scope, richCompare(globalObject, first.at(i), second.at(i), op));
    }
    unsigned a = first.length();
    unsigned b = second.length();
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(a == b);
    case CompareOperator::NotEq:
        return jsBoolean(a != b);
    case CompareOperator::Lt:
        return jsBoolean(a < b);
    case CompareOperator::LtE:
        return jsBoolean(a <= b);
    case CompareOperator::Gt:
        return jsBoolean(a > b);
    default:
        return jsBoolean(a >= b);
    }
}

// The index of the first item from `from` on and before `to` that equals the value. -1: there is none, or an exception.
template<typename Items>
static int64_t findEqual(JSGlobalObject* globalObject, Items items, JSValue value, int64_t from = 0, int64_t to = std::numeric_limits<int64_t>::max())
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The length is looked at each time: a list may grow or shrink as it is searched.
    for (int64_t i = from;; ++i) {
        if (value.isInt32()) {
            // Nothing is asked about two ints, so nothing changes while the other ints are passed over.
            auto values = items.values();
            int64_t end = std::min<int64_t>(values.size(), to);
            while (i < end && values[i].isInt32() && values[i] != value)
                ++i;
        }
        if (i >= to || i >= items.length())
            return -1;
        bool equal = isEqual(globalObject, items.at(i), value);
        RETURN_IF_EXCEPTION(scope, -1);
        if (equal)
            return i;
    }
}

template<typename Items>
static JSValue countEqual(JSGlobalObject* globalObject, Items items, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = 0;
    for (int64_t i = 0;; ++i, ++count) {
        i = findEqual(globalObject, items, value, i);
        RETURN_IF_EXCEPTION(scope, { });
        if (i < 0)
            return jsNumber(count);
    }
}

// `length` is set to what the function returns once the __index__ of the count, which may change a list, has been called.
template<typename LengthFunction>
static bool repeatCount(JSGlobalObject* globalObject, JSValue countValue, const LengthFunction& lengthFunction, unsigned& length, uint64_t& count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t given = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, false);
    length = lengthFunction();
    count = given < 0 ? 0 : given;
    if (length && count > MAX_STORAGE_VECTOR_LENGTH / length) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    return true;
}

// ---- The methods of list ----

static JSValue listNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    PyType* type = PyType::from(cls);
    if (type == rt.listType())
        return newList(globalObject);
    return PyListObject::create(rt.vm(), rt.listSubclassStructure(), type);
}

static JSValue listInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywordsInInit(globalObject, "list"_s, self, runtime(globalObject).listType(), args)))
        return { };
    if (failed(scope, checkPositional(globalObject, "list"_s, args, 0, 1)))
        return { };
    JSArray* list = asList(self);
    listTruncate(globalObject, list, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size()) {
        listExtend(globalObject, list, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue listRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    if (!list->length())
        return makeStr(vm, "[]"_s);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, "[...]"_s);
    StrBuilder builder;
    builder.append('[');
    for (unsigned i = 0; i < list->length(); ++i) {
        if (i)
            builder.append(", "_s);
        JSString* item = repr(globalObject, listGet(list, i));
        RETURN_IF_EXCEPTION(scope, { });
        auto view = item->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(view.data);
    }
    builder.append(']');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

template<CompareOperator op>
static JSValue listCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!isList(other))
        return runtime(globalObject).notImplemented();
    return compareSequences(globalObject, op, ListItems { asList(self) }, ListItems { asList(other) });
}

static JSValue listLen(JSGlobalObject*, JSValue self) { return jsNumber(asList(self)->length()); }

static JSArray* listSlice(JSGlobalObject* globalObject, JSArray* list, const PySlice::Indices& indices)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (indices.step == 1 && indices.length > 0 && hasItemsInARow(list)) [[likely]] {
        JSArray* result = tryNewListFilledBy(globalObject, indices.length, [&](WriteBarrier<Unknown>* items) ALWAYS_INLINE_LAMBDA {
            copyItems(items, list, indices.start, indices.length);
        });
        if (result) [[likely]]
            return result;
    }
    // Unsigned: one step beyond the last item may be beyond what an int64_t holds.
    uint64_t position = indices.start;
    if (auto values = listValues(list); values.data()) {
        RELEASE_AND_RETURN(scope, newListOf(globalObject, indices.length, [&] ALWAYS_INLINE_LAMBDA {
            JSValue item = values[position];
            position += static_cast<uint64_t>(indices.step);
            return item ? item : jsUndefined();
        }));
    }
    RELEASE_AND_RETURN(scope, newListOf(globalObject, indices.length, [&] ALWAYS_INLINE_LAMBDA {
        JSValue item = listGet(list, static_cast<unsigned>(position));
        position += static_cast<uint64_t>(indices.step);
        return item;
    }));
}

// An __index__ method may change the list: its length is looked at after all of them have been called.

static bool listSubscriptIndex(JSGlobalObject* globalObject, JSArray* list, JSValue key, ASCIILiteral message, int64_t& index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t number = toSsize(globalObject, key, runtime(globalObject).IndexError());
    RETURN_IF_EXCEPTION(scope, false);
    int64_t length = list->length();
    if (number < 0)
        number += length;
    if (number < 0 || number >= length) {
        raiseIndexError(globalObject, scope, message);
        return false;
    }
    index = number;
    return true;
}

static bool listSliceIndices(JSGlobalObject* globalObject, JSArray* list, PySlice* slice, PySlice::Indices& indices)
{
    if (!slice->unpack(globalObject, indices))
        return false;
    PySlice::adjust(list->length(), indices);
    return true;
}

JSValue listGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    if (hasIndex(globalObject, key)) {
        int64_t index;
        if (failed(scope, listSubscriptIndex(globalObject, list, key, "list index out of range"_s, index)))
            return { };
        return listGet(list, index);
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "list indices must be integers or slices, not "_s, typeName(globalObject, key));
    PySlice::Indices indices;
    if (failed(scope, listSliceIndices(globalObject, list, slice, indices)))
        return { };
    RELEASE_AND_RETURN(scope, listSlice(globalObject, list, indices));
}

// list[start:stop] = items, which are not in the list. An empty one is None.
static void listReplaceRange(JSGlobalObject* globalObject, JSArray* list, unsigned start, unsigned stop, std::span<const JSValue> items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned removed = stop - start;
    if (items.size() > removed)
        listInsertGap(globalObject, list, stop, items.size() - removed);
    else
        listRemove(globalObject, list, start + items.size(), removed - items.size());
    RETURN_IF_EXCEPTION(scope, void());
    if (ListStorage storage { vm, list }) [[likely]]
        return storage.set(start, items);
    for (size_t i = 0; i < items.size(); ++i) {
        listSetSlow(globalObject, list, start + i, items[i] ? items[i] : jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// An empty value deletes.
static void listAssignSubscript(JSGlobalObject* globalObject, JSArray* list, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasIndex(globalObject, key)) {
        int64_t index;
        if (failed(scope, listSubscriptIndex(globalObject, list, key, "list assignment index out of range"_s, index)))
            return;
        scope.release();
        if (value)
            listSet(globalObject, list, index, value);
        else
            listRemove(globalObject, list, index);
        return;
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice) {
        raiseTypeError(globalObject, scope, "list indices must be integers or slices, not "_s, typeName(globalObject, key));
        return;
    }

    PySlice::Indices indices;
    if (failed(scope, slice->unpack(globalObject, indices)))
        return;

    // The items are collected before the length of the list is looked at: working them out may change the list, and they may be the list.
    JSValue source = value ? value : JSValue(runtime(globalObject).emptyTuple());
    if (source.isCell() && source.asCell() == list) {
        // Whatever its class.
        source = listSlice(globalObject, list, { 0, list->length(), 1, list->length() });
        RETURN_IF_EXCEPTION(scope, void());
    }
    // To store into a list whose items are not in a row may run code of JS.
    AllItems items(globalObject, source, "must assign iterable to extended slice"_s, ListStorage { vm, list } ? AllItems::WhileInUse::NothingRuns : AllItems::WhileInUse::CodeMayRun);
    RETURN_IF_EXCEPTION(scope, void());
    std::span<const JSValue> itemSpan = items.span();

    PySlice::adjust(list->length(), indices);
    if (indices.step == 1) {
        int64_t stop = std::max(indices.start, indices.stop);
        RELEASE_AND_RETURN(scope, listReplaceRange(globalObject, list, indices.start, stop, itemSpan));
    }

    if (!value) {
        if (indices.length <= 0)
            return;
        // In one pass: what stays moves down past what goes.
        int64_t next = indices.start;
        int64_t step = indices.step;
        if (step < 0) {
            next += (indices.length - 1) * step;
            step = -step;
        }
        int64_t left = indices.length;
        unsigned length = list->length();
        unsigned kept = next;
        if (ListStorage storage { vm, list }) [[likely]] {
            WriteBarrier<Unknown>* data = storage.items();
            for (unsigned i = next; i < length; ++i) {
                if (left && i == next) {
                    next += step;
                    --left;
                    continue;
                }
                data[kept++].setWithoutWriteBarrier(data[i].get());
            }
            // What has moved may have passed the collector by.
            vm.writeBarrier(list);
            return storage.shrink(kept);
        }
        for (unsigned i = next; i < length; ++i) {
            if (left && i == next) {
                next += step;
                --left;
                continue;
            }
            listSetSlow(globalObject, list, kept++, listGet(list, i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        RELEASE_AND_RETURN(scope, listTruncate(globalObject, list, kept));
    }
    if (static_cast<int64_t>(itemSpan.size()) != indices.length) {
        raiseValueError(globalObject, scope, "attempt to assign sequence of size "_s, itemSpan.size(), " to extended slice of size "_s, indices.length);
        return;
    }
    int64_t position = indices.start;
    if (ListStorage storage { vm, list }) [[likely]] {
        for (JSValue item : itemSpan) {
            storage.items()[position].setWithoutWriteBarrier(item ? item : jsUndefined());
            position += indices.step;
        }
        vm.writeBarrier(list);
        return;
    }
    for (JSValue item : itemSpan) {
        listSetSlow(globalObject, list, position, item ? item : jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
        position += indices.step;
    }
}

static JSValue listSetItemMethod(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listAssignSubscript(globalObject, asList(self), key, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listDelItemMethod(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listAssignSubscript(globalObject, asList(self), key, { });
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listIter(JSGlobalObject* globalObject, JSValue self) { return PyIterator::create(globalObject, PyIterator::Kind::List, self); }

static JSValue listReversed(JSGlobalObject* globalObject, JSValue self)
{
    PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::ListReverse, self);
    iterator->position = static_cast<int64_t>(asList(self)->length()) - 1;
    return iterator;
}

bool listContains(JSGlobalObject* globalObject, JSArray* list, JSValue item)
{
    return findEqual(globalObject, ListItems { list }, item) >= 0;
}

static JSValue listContainsMethod(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found = listContains(globalObject, asList(self), item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static JSValue listAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isList(other))
        return raiseTypeError(globalObject, scope, "can only concatenate list (not \""_s, typeName(globalObject, other), "\") to list"_s);
    JSArray* a = asList(self);
    JSArray* b = asList(other);
    unsigned lengthA = a->length();
    if (hasItemsInARow(a) && hasItemsInARow(b)) [[likely]] {
        JSArray* result = tryNewListFilledBy(globalObject, static_cast<uint64_t>(lengthA) + b->length(), [&](WriteBarrier<Unknown>* items) ALWAYS_INLINE_LAMBDA {
            copyItems(items, a, 0, lengthA);
            copyItems(items + lengthA, b, 0, b->length());
        });
        if (result) [[likely]]
            return result;
    }
    unsigned i = 0;
    RELEASE_AND_RETURN(scope, newListOf(globalObject, static_cast<uint64_t>(lengthA) + b->length(), [&] ALWAYS_INLINE_LAMBDA {
        JSValue item = i < lengthA ? listGet(a, i) : listGet(b, i - lengthA);
        ++i;
        return item;
    }));
}

static JSValue listIAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listExtend(globalObject, asList(self), other);
    RETURN_IF_EXCEPTION(scope, { });
    return self;
}

static JSValue listMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    unsigned length;
    uint64_t count;
    if (failed(scope, repeatCount(globalObject, countValue, [&] { return list->length(); }, length, count)))
        return { };
    if (length && count && hasItemsInARow(list)) [[likely]] {
        JSArray* result = tryNewListFilledBy(globalObject, length * count, [&](WriteBarrier<Unknown>* items) ALWAYS_INLINE_LAMBDA {
            copyItems(items, list, 0, length);
            repeatItems(items, length, count);
        });
        if (result) [[likely]]
            return result;
    }
    unsigned i = 0;
    RELEASE_AND_RETURN(scope, newListOf(globalObject, length * count, [&] ALWAYS_INLINE_LAMBDA {
        JSValue item = listGet(list, i);
        if (++i == length)
            i = 0;
        return item;
    }));
}

static JSValue listIMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    unsigned length;
    uint64_t count;
    if (failed(scope, repeatCount(globalObject, countValue, [&] { return list->length(); }, length, count)))
        return { };
    if (!count) {
        listTruncate(globalObject, list, 0);
        RETURN_IF_EXCEPTION(scope, { });
        return self;
    }
    if (!length)
        return self;
    if (ListStorage storage { vm, list }) [[likely]] {
        if (!storage.grow(length * count)) {
            scope.release();
            raiseListTooLong(globalObject);
            return { };
        }
        repeatItems(storage.items(), length, count);
        vm.writeBarrier(list);
        return self;
    }
    for (uint64_t i = 1; i < count; ++i) {
        for (unsigned j = 0; j < length; ++j) {
            listPush(globalObject, list, listGet(list, j));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return self;
}

static JSValue listAppendMethod(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listPush(globalObject, asList(self), item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listExtendMethod(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    GeneratorArgumentScope argument(globalObject, iterable);
    listExtend(globalObject, asList(self), iterable);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listInsertMethod(JSGlobalObject* globalObject, JSValue self, JSValue indexValue, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    int64_t index;
    if (failed(scope, ssizeArgument(globalObject, indexValue, index)))
        return { };
    int64_t length = list->length();
    if (index < 0)
        index = std::max<int64_t>(index + length, 0);
    index = std::min(index, length);
    listInsert(globalObject, list, index, item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listPop(JSGlobalObject* globalObject, JSValue self, JSValue indexValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    int64_t index = -1;
    if (indexValue && failed(scope, ssizeArgument(globalObject, indexValue, index)))
        return { };
    int64_t length = list->length();
    if (!length)
        return raiseIndexError(globalObject, scope, "pop from empty list"_s);
    if (index < 0)
        index += length;
    if (index < 0 || index >= length)
        return raiseIndexError(globalObject, scope, "pop index out of range"_s);
    JSValue item = listGet(list, index);
    listRemove(globalObject, list, index);
    RETURN_IF_EXCEPTION(scope, { });
    return item;
}

static JSValue listRemoveMethod(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    int64_t index = findEqual(globalObject, ListItems { list }, item);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "list.remove(x): x not in list"_s);
    // (The comparison may have taken it away.)
    if (index < list->length()) {
        listRemove(globalObject, list, index);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue listClear(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listTruncate(globalObject, asList(self), 0);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listCopy(JSGlobalObject* globalObject, JSValue self)
{
    JSArray* list = asList(self);
    return listSlice(globalObject, list, { 0, list->length(), 1, list->length() });
}

static JSValue listCount(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    return countEqual(globalObject, ListItems { asList(self) }, item);
}

static JSValue listIndex(JSGlobalObject* globalObject, JSValue self, JSValue item, JSValue start, JSValue stop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    int64_t from = 0;
    int64_t to = std::numeric_limits<int64_t>::max();
    if ((start && failed(scope, sliceIndexArgument(globalObject, start, from))) || (stop && failed(scope, sliceIndexArgument(globalObject, stop, to))))
        return { };
    // Not clipped to the length: the list may grow as it is searched.
    if (from < 0)
        from = std::max<int64_t>(from + list->length(), 0);
    if (to < 0)
        to = std::max<int64_t>(to + list->length(), 0);
    int64_t index = findEqual(globalObject, ListItems { list }, item, from, to);
    RETURN_IF_EXCEPTION(scope, { });
    if (index >= 0)
        return jsNumber(static_cast<double>(index));
    return raiseValueError(globalObject, scope, "list.index(x): x not in list"_s);
}

static JSValue listReverse(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    if (ListStorage storage { vm, list }) [[likely]] {
        WriteBarrier<Unknown>* items = storage.items();
        for (unsigned i = 0, j = storage.length(); i + 1 < j; ++i) {
            JSValue item = items[i].get();
            items[i].setWithoutWriteBarrier(items[--j].get());
            items[j].setWithoutWriteBarrier(item);
        }
        // What has moved may have passed the collector by.
        vm.writeBarrier(list);
        return jsUndefined();
    }
    unsigned length = list->length();
    for (unsigned i = 0; i < length / 2; ++i) {
        JSValue first = listGet(list, i);
        listSetSlow(globalObject, list, i, listGet(list, length - 1 - i));
        RETURN_IF_EXCEPTION(scope, { });
        listSetSlow(globalObject, list, length - 1 - i, first);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue listSortMethod(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue reverseValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool reverse = false;
    if (reverseValue) {
        reverse = isTrue(globalObject, reverseValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    listSort(globalObject, asList(self), key, reverse);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listSizeOf(JSGlobalObject*, JSValue self)
{
    return jsNumber(static_cast<double>(56 + 8 * static_cast<uint64_t>(asList(self)->length())));
}

JSValue classGetItem(JSGlobalObject* globalObject, JSValue cls, JSValue item)
{
    return makeGenericAlias(globalObject, cls, item);
}

static constexpr NativeDef listMethods[] = {
    PY_NEW(listNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", listInit, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", listRepr, "($self, /)"),
    PY_SLOT("__eq__", listCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", listCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", listCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", listCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", listCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", listCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", listIter, "($self, /)"),
    PY_SLOT("__len__", listLen, "($self, /)"),
    PY_METHOD("__getitem__", listGetItem, "($self, index, /)"),
    PY_SLOT("__setitem__", listSetItemMethod, "($self, key, value, /)"),
    PY_SLOT("__delitem__", listDelItemMethod, "($self, key, /)"),
    PY_SLOT("__contains__", listContainsMethod, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__add__", listAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__mul__", listMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", listMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__iadd__", listIAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__imul__", listIMul, "($self, value, /)"),
    PY_METHOD("__reversed__", listReversed, "($self, /)"),
    PY_METHOD("__sizeof__", listSizeOf, "($self, /)"),
    PY_METHOD("clear", listClear, "($self, /)"),
    PY_METHOD("copy", listCopy, "($self, /)"),
    PY_METHOD("append", listAppendMethod, "($self, object, /)"),
    PY_METHOD("insert", listInsertMethod, "($self, index, object, /)"),
    PY_METHOD_WITH_FLAGS("extend", listExtendMethod, "($self, iterable, /)", ClosesGeneratorArgument),
    PY_METHOD("pop", listPop, "($self, index=-1, /)"),
    PY_METHOD("remove", listRemoveMethod, "($self, value, /)"),
    PY_METHOD("index", listIndex, "($self, value, start=0, stop=sys.maxsize, /)"),
    PY_METHOD("count", listCount, "($self, value, /)"),
    PY_METHOD("reverse", listReverse, "($self, /)"),
    PY_METHOD("sort", listSortMethod, "($self, /, *, key=None, reverse=False)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

void initializeListTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.listType()->initializeBuiltin(globalObject, { "list"_s, { TypeFlag::Base, TypeFlag::ListSubclass, TypeFlag::Sequence, TypeFlag::MatchSelf, TypeFlag::OwnLayout, TypeFlag::GoesThroughArgument }, listMethods, { }, "Built-in mutable sequence.\n\nIf no argument is given, the constructor creates a new empty list.\nThe argument must be an iterable if specified."_s }, rt.typeType(), rt.objectType());
    rt.listType()->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    rt.listType()->didChange(vm);
}

// ---- tuple ----

int64_t hashOfTuple(JSGlobalObject* globalObject, PyTuple* tuple)
{
    if (int64_t cached = tuple->cachedHash(); cached != -1)
        return cached;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isSafeToRecurseWithMargin(vm, 64 * KB)) [[unlikely]] {
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded while hashing a tuple"_s);
        return -1;
    }
    TupleHasher hasher;
    unsigned length = tuple->length();
    for (unsigned i = 0; i < length; ++i) {
        int64_t itemHash = hash(globalObject, tuple->at(i));
        RETURN_IF_EXCEPTION(scope, -1);
        hasher.add(itemHash);
    }
    int64_t result = hasher.finish();
    tuple->setCachedHash(result);
    return result;
}

JSValue tupleNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (failed(scope, rejectKeywordsInNew(globalObject, "tuple"_s, type, rt.tupleType(), args)))
        return { };
    if (failed(scope, checkPositional(globalObject, "tuple"_s, args, 0, 1)))
        return { };
    PyTuple* items = rt.emptyTuple();
    if (args.size()) {
        items = tupleFromIterable(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (type == rt.tupleType())
        return items;
    PyTuple* result = PyTuple::createUninitialized(vm, rt.structure<PyTuple>(), type, items->length());
    for (unsigned i = 0; i < items->length(); ++i)
        result->initializeAt(vm, i, items->at(i));
    return result;
}

static JSValue tupleRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    if (!tuple->length())
        return makeStr(vm, "()"_s);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, "(...)"_s);
    StrBuilder builder;
    builder.append('(');
    for (unsigned i = 0; i < tuple->length(); ++i) {
        if (i)
            builder.append(", "_s);
        JSString* item = repr(globalObject, tuple->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        auto view = item->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(view.data);
    }
    builder.append(tuple->length() == 1 ? ",)"_s : ")"_s);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static int64_t tupleHash(JSGlobalObject* globalObject, JSValue self) { return hashOfTuple(globalObject, PyTuple::from(self)); }

template<CompareOperator op>
static JSValue tupleCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!PyTuple::is(other))
        return runtime(globalObject).notImplemented();
    return compareSequences(globalObject, op, TupleItems { PyTuple::from(self) }, TupleItems { PyTuple::from(other) });
}

static JSValue tupleLen(JSGlobalObject*, JSValue self) { return jsNumber(PyTuple::from(self)->length()); }

JSValue tupleGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index;
        if (failed(scope, normalizeIndex(globalObject, key, tuple->length(), "tuple index out of range"_s, index)))
            return { };
        return tuple->at(index);
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "tuple indices must be integers or slices, not "_s, typeName(globalObject, key));
    PySlice::Indices indices;
    if (failed(scope, slice->indices(globalObject, tuple->length(), indices)))
        return { };
    if (indices.length <= 0)
        return rt.emptyTuple();
    if (!indices.start && indices.step == 1 && indices.length == tuple->length() && tuple->type() == rt.tupleType())
        return tuple;
    if (indices.step == 1) {
        PyTuple* result = PyTuple::create(globalObject, tuple->span().subspan(indices.start, indices.length));
        ensureStillAliveHere(tuple);
        return result;
    }
    PyTuple* result = PyTuple::create(globalObject, indices.length);
    int64_t position = indices.start;
    for (int64_t i = 0; i < indices.length; ++i, position += indices.step)
        result->initializeAt(vm, i, tuple->at(position));
    return result;
}

static JSValue tupleIter(JSGlobalObject* globalObject, JSValue self) { return PyIterator::create(globalObject, PyIterator::Kind::Tuple, self); }

bool tupleContains(JSGlobalObject* globalObject, PyTuple* tuple, JSValue item)
{
    return findEqual(globalObject, TupleItems { tuple }, item) >= 0;
}

static JSValue tupleContainsMethod(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found = tupleContains(globalObject, PyTuple::from(self), item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static JSValue tupleAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyTuple::is(other))
        return raiseTypeError(globalObject, scope, "can only concatenate tuple (not \""_s, typeName(globalObject, other), "\") to tuple"_s);
    PyTuple* a = PyTuple::from(self);
    PyTuple* b = PyTuple::from(other);
    if (!b->length() && a->type() == rt.tupleType())
        return a;
    if (!a->length() && b->type() == rt.tupleType())
        return b;
    uint64_t total = static_cast<uint64_t>(a->length()) + b->length();
    if (total > MAX_STORAGE_VECTOR_LENGTH)
        return raiseMemoryError(globalObject, scope, String());
    PyTuple* result = PyTuple::create(globalObject, total);
    for (unsigned i = 0; i < a->length(); ++i)
        result->initializeAt(vm, i, a->at(i));
    for (unsigned i = 0; i < b->length(); ++i)
        result->initializeAt(vm, a->length() + i, b->at(i));
    return result;
}

static JSValue tupleMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    unsigned length;
    uint64_t count;
    if (failed(scope, repeatCount(globalObject, countValue, [&] { return tuple->length(); }, length, count)))
        return { };
    if ((!length || count == 1) && tuple->type() == rt.tupleType())
        return tuple;
    if (!length || !count)
        return rt.emptyTuple();
    PyTuple* result = PyTuple::create(globalObject, length * count);
    for (uint64_t i = 0; i < count; ++i) {
        for (unsigned j = 0; j < length; ++j)
            result->initializeAt(vm, i * length + j, tuple->at(j));
    }
    return result;
}

static JSValue tupleCount(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    return countEqual(globalObject, TupleItems { PyTuple::from(self) }, item);
}

static JSValue tupleIndex(JSGlobalObject* globalObject, JSValue self, JSValue item, JSValue start, JSValue stop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    int64_t from = 0;
    int64_t to = std::numeric_limits<int64_t>::max();
    if ((start && failed(scope, sliceIndexArgument(globalObject, start, from))) || (stop && failed(scope, sliceIndexArgument(globalObject, stop, to))))
        return { };
    adjustSliceBounds(tuple->length(), from, to);
    int64_t index = findEqual(globalObject, TupleItems { tuple }, item, from, to);
    RETURN_IF_EXCEPTION(scope, { });
    if (index >= 0)
        return jsNumber(static_cast<double>(index));
    return raiseValueError(globalObject, scope, "tuple.index(x): x not in tuple"_s);
}

static JSValue tupleGetNewArgs(JSGlobalObject* globalObject, JSValue self)
{
    PyTuple* tuple = PyTuple::from(self);
    return PyTuple::create(globalObject, { JSValue(PyTuple::create(globalObject, tuple->span())) });
}

static constexpr NativeDef tupleMethods[] = {
    PY_NEW(tupleNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", tupleRepr, "($self, /)"),
    PY_HASH(tupleHash),
    PY_SLOT("__eq__", tupleCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", tupleCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", tupleCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", tupleCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", tupleCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", tupleCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", tupleIter, "($self, /)"),
    PY_SLOT("__len__", tupleLen, "($self, /)"),
    PY_SLOT("__getitem__", tupleGetItem, "($self, key, /)"),
    PY_SLOT("__contains__", tupleContainsMethod, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__add__", tupleAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__mul__", tupleMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", tupleMul, "($self, value, /)"),
    PY_METHOD("__getnewargs__", tupleGetNewArgs, "($self, /)"),
    PY_METHOD("index", tupleIndex, "($self, value, start=0, stop=sys.maxsize, /)"),
    PY_METHOD("count", tupleCount, "($self, value, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

void initializeTupleTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.tupleType()->initializeBuiltin(globalObject, { "tuple"_s, { TypeFlag::Base, TypeFlag::TupleSubclass, TypeFlag::Sequence, TypeFlag::MatchSelf, TypeFlag::OwnLayout, TypeFlag::GoesThroughArgument }, tupleMethods, { }, "Built-in immutable sequence.\n\nIf no argument is given, the constructor returns an empty tuple.\nIf iterable is specified the tuple is initialized from iterable's items.\n\nIf the argument is a tuple, the return value is the same object."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
