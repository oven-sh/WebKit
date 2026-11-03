/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyList.h"

#include "GCMemoryOperations.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyJSView.h"
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
    return Structure::create(vm, globalObject, prototype, TypeInfo(DerivedArrayType, StructureFlags), info(), ArrayWithUndecided);
}

PyListObject* PyListObject::create(VM& vm, Structure* structure, PyType* type)
{
    // As JSArray::tryCreate(): an array always has a butterfly, which is where its length is.
    unsigned outOfLineStorage = structure->outOfLineCapacity();
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(structure, 0);
    void* base = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, outOfLineStorage, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::Assert);
    Butterfly* butterfly = Butterfly::fromBase(base, 0, outOfLineStorage);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(0);
    Butterfly::clearRange(structure->indexingType(), butterfly, 0, vectorLength);
    auto* list = new (NotNull, allocateCell<PyListObject>(vm)) PyListObject(vm, structure, type, butterfly);
    list->finishCreation(vm);
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
    bool grow(unsigned newLength)
    {
        if (newLength > MAX_STORAGE_VECTOR_LENGTH)
            return false;
        return m_list->ensureLength(m_vm, newLength);
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

private:
    VM& m_vm;
    JSArray* m_list;
    bool m_isContiguous { false };
};

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

JSArray* newList(JSGlobalObject* globalObject, std::span<const JSValue> items)
{
    VM& vm = getVM(globalObject);
    JSArray* list = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), items.size());
    if (!list) [[unlikely]] {
        raiseListTooLong(globalObject);
        return nullptr;
    }
    WriteBarrier<Unknown>* data = list->butterfly()->contiguous().data();
    for (size_t i = 0; i < items.size(); ++i)
        data[i].setWithoutWriteBarrier(items[i]);
    return list;
}

void listSet(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    VM& vm = getVM(globalObject);
    if (ListStorage storage { vm, list }) [[likely]] {
        ASSERT(index < storage.length());
        storage.set(index, value);
        return;
    }
    list->putDirectIndex(globalObject, index, value);
}

void listAppend(JSGlobalObject* globalObject, JSArray* list, JSValue value)
{
    list->push(globalObject, value);
}

void listInsertGap(JSGlobalObject* globalObject, JSArray* list, unsigned index, unsigned count)
{
    if (!count)
        return;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = list->length();
    ASSERT(index <= length);
    if (count > MAX_STORAGE_VECTOR_LENGTH - length)
        RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
    if (ListStorage storage { vm, list }) [[likely]] {
        if (!storage.grow(length + count))
            RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        storage.move(index + count, index, length - index);
        for (unsigned i = 0; i < count && index + i < length; ++i)
            storage.items()[index + i].setWithoutWriteBarrier(jsUndefined());
        for (unsigned i = std::max(index, length); i < index + count; ++i)
            storage.items()[i].setWithoutWriteBarrier(jsUndefined());
        return;
    }
    for (unsigned i = length; i-- > index;) {
        list->putDirectIndex(globalObject, i + count, listGet(list, i));
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < count; ++i) {
        list->putDirectIndex(globalObject, index + i, jsUndefined());
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
        list->putDirectIndex(globalObject, i - count, listGet(list, i));
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
    if (items.size() > MAX_STORAGE_VECTOR_LENGTH - length)
        RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
    if (ListStorage storage { vm, list }) [[likely]] {
        if (!storage.grow(length + items.size()))
            RELEASE_AND_RETURN(scope, raiseListTooLong(globalObject));
        for (size_t i = 0; i < items.size(); ++i)
            storage.items()[length + i].setWithoutWriteBarrier(items[i]);
        vm.writeBarrier(list);
        return;
    }
    for (JSValue item : items) {
        list->push(globalObject, item);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void listExtend(JSGlobalObject* globalObject, JSArray* list, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == runtime(globalObject).tupleType())
        RELEASE_AND_RETURN(scope, listAppendSpan(globalObject, list, tuple->span()));
    if (isExactList(iterable)) {
        // It may be the list itself.
        JSArray* other = asList(iterable);
        unsigned count = other->length();
        for (unsigned i = 0; i < count; ++i) {
            list->push(globalObject, listGet(other, i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, void());
    // Only asked so that what is wrong with the answer is reported: the array grows as it needs to.
    lengthHint(globalObject, iterable, 8);
    RETURN_IF_EXCEPTION(scope, void());
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!item)
            return;
        list->push(globalObject, item);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSArray* listFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    listExtend(globalObject, list, iterable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return list;
}

PyTuple* tupleFromList(JSGlobalObject* globalObject, JSArray* list)
{
    VM& vm = getVM(globalObject);
    unsigned length = list->length();
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
        return tupleFromList(globalObject, asList(iterable));
    JSArray* list = listFromIterable(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return tupleFromList(globalObject, list);
}

// ---- Sorting ----

namespace {

// The sort of CPython's lists ("timsort", Objects/listsort.txt there): a stable merge sort that takes the runs the data already
// has, lengthens short ones by binary insertion, merges them in the order "powersort" says, and gallops through a run that
// keeps winning. Programs can see in which order their __lt__ is called, so each step compares what CPython's compares.
// It sorts keys, and the values that go with them if they are not the keys themselves.
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

private:
    static constexpr size_t maxMinimumRun = 64;
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

    static bool stringLess(JSString* a, JSString* b)
    {
        StringView x = a->tryGetValue().data;
        StringView y = b->tryGetValue().data;
        unsigned common = std::min(x.length(), y.length());
        for (unsigned i = 0; i < common; ++i) {
            unsigned c = x[i];
            unsigned d = y[i];
            if (c == d)
                continue;
            if (c >= 0xd800 && d >= 0xd800) {
                c = c >= 0xe000 ? c - 0x800 : c + 0x2000;
                d = d >= 0xe000 ? d - 0x800 : d + 0x2000;
            }
            return c < d;
        }
        return x.length() < y.length();
    }

    // These say whether a < b. After a failure they say no, and m_failed is set.
    bool genericLess(JSValue a, JSValue b)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
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
            auto scope = DECLARE_THROW_SCOPE(m_vm);
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

    bool less(JSValue a, JSValue b)
    {
        if (m_failed)
            return false;
        if (!m_keysAreTuples)
            return itemLess(a, b);

        auto scope = DECLARE_THROW_SCOPE(m_vm);
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
        RELEASE_AND_RETURN(scope, i ? genericLess(first->at(i), second->at(i)) : itemLess(first->at(i), second->at(i)));
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
                if (m_failed)
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
                if (nextIsSmaller() || m_failed)
                    break;
            }
        };

        extendAscending();
        if (m_failed)
            return -1;
        if (n == remaining)
            return n;

        // keys[n] is smaller. What came before went up somewhere, which ends the run, or is n equal items, with which a
        // descending run begins.
        if (n > 1) {
            bool wentUp = less(keys[0], keys[n - 1]);
            if (m_failed)
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
            if (m_failed)
                return -1;
            if (smaller) {
                reverseEqualItems();
                continue;
            }
            bool larger = less(keys[n - 1], keys[n]);
            if (m_failed)
                return -1;
            if (larger)
                break;
            ++equal;
        }
        reverseEqualItems();
        slice.reverse(n);

        // What follows may well go on upwards from there.
        extendAscending();
        return m_failed ? -1 : n;
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
        if (m_failed)
            return -1;
        if (goesRight) {
            ptrdiff_t limit = n - hint;
            while (offset < limit) {
                bool further = isAfter(items[hint + offset]);
                if (m_failed)
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
                if (m_failed)
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
            if (m_failed)
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
                if (m_failed)
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
                if (m_failed)
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
    Vector<Run, 64> m_runs;
    ptrdiff_t m_minimumGallop { initialMinimumGallop };
    Comparison m_comparison { Comparison::Generic };
    bool m_keysAreTuples { false };
    bool m_failed { false };
};

}

void listSort(JSGlobalObject* globalObject, JSArray* list, JSValue key, bool reverse)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = list->length();

    // The list is empty for as long as it is being sorted, so that what the comparisons do to it shows.
    MarkedArgumentBuffer values;
    values.ensureCapacity(count);
    for (unsigned i = 0; i < count; ++i)
        values.append(listGet(list, i));
    listTruncate(globalObject, list, 0);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue* valueData = reinterpret_cast<JSValue*>(const_cast<EncodedJSValue*>(ArgList(values).data()));

    // Even if it is empty again afterwards. Whatever puts an item in the list writes to the first slot of its storage, and whatever
    // takes it out again empties the slot: a mark left there, beyond the end of the list, does not survive that.
    Butterfly* markedButterfly = list->butterfly();
    IndexingType markedShape = list->indexingType() & IndexingShapeMask;
    bool isMarked = markedButterfly && markedButterfly->vectorLength() && (markedShape == Int32Shape || markedShape == ContiguousShape);
    if (isMarked)
        markedButterfly->contiguous().atUnsafe(0).setWithoutWriteBarrier(jsNumber(0));

    auto reverseAll = [&](JSValue* data) {
        if (data)
            std::reverse(data, data + count);
    };

    MarkedArgumentBuffer keys;
    JSValue* keyData = nullptr;
    bool failed = false;
    if (key && !isNone(key)) {
        keys.ensureCapacity(count);
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
        MarkedArgumentBuffer scratch;
        MarkedArgumentBuffer secondScratch;
        scratch.fill(vm, count, [](JSValue* slots) { UNUSED_PARAM(slots); });
        if (keyData)
            secondScratch.fill(vm, count, [](JSValue* slots) { UNUSED_PARAM(slots); });
        auto dataOf = [](MarkedArgumentBuffer& buffer) { return reinterpret_cast<JSValue*>(const_cast<EncodedJSValue*>(ArgList(buffer).data())); };
        Sorter sorter(globalObject, keyData ? keyData : valueData, keyData ? valueData : nullptr, dataOf(scratch), keyData ? dataOf(secondScratch) : nullptr, count);
        failed = !sorter.sort();
        if (reverse)
            reverseAll(valueData);
    }

    bool wasModified = list->length();
    if (isMarked && !wasModified) {
        wasModified = list->butterfly() != markedButterfly || (list->indexingType() & IndexingShapeMask) != markedShape || markedButterfly->contiguous().atUnsafe(0).get() != jsNumber(0);
        if (!wasModified)
            markedButterfly->contiguous().atUnsafe(0).clear();
    }
    JSValue pending;
    if (failed)
        pending = takePendingException(globalObject);
    listTruncate(globalObject, list, 0);
    listAppendSpan(globalObject, list, { valueData, count });
    if (pending) {
        raiseException(globalObject, scope, pending);
        return;
    }
    RETURN_IF_EXCEPTION(scope, void());
    if (wasModified)
        raiseValueError(globalObject, scope, "list modified during sort"_s);
}

// ---- What list and tuple share ----

namespace {

struct ListItems {
    JSArray* list;
    unsigned length() const { return list->length(); }
    JSValue at(unsigned index) const { return listGet(list, index); }
};

struct TupleItems {
    PyTuple* tuple;
    unsigned length() const { return tuple->length(); }
    JSValue at(unsigned index) const { return tuple->at(index); }
};

}

template<typename Items>
static JSValue compareSequences(JSGlobalObject* globalObject, CompareOperator op, Items first, Items second)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (first.length() != second.length() && (op == CompareOperator::Eq || op == CompareOperator::NotEq))
        return jsBoolean(op == CompareOperator::NotEq);

    // The first pair of items that differ decides. The lengths may change as the items are compared.
    unsigned i = 0;
    for (; i < first.length() && i < second.length(); ++i) {
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
        RELEASE_AND_RETURN(scope, richCompare(globalObject, a, b, op));
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
    if (!rejectKeywordsInInit(globalObject, "list"_s, self, runtime(globalObject).listType(), args))
        return { };
    if (!checkPositional(globalObject, "list"_s, args, 0, 1))
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
    return makeStr(globalObject, builder);
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
    JSArray* result = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), indices.length);
    if (!result) {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    WriteBarrier<Unknown>* data = result->butterfly()->contiguous().data();
    int64_t position = indices.start;
    for (int64_t i = 0; i < indices.length; ++i, position += indices.step)
        data[i].setWithoutWriteBarrier(listGet(list, position));
    return result;
}

// An __index__ method may change the list: its length is looked at after all of them have been called.

static bool listSubscriptIndex(JSGlobalObject* globalObject, JSArray* list, JSValue key, ASCIILiteral message, int64_t& index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!key.isInt32()) [[unlikely]] {
        int64_t number = toSsize(globalObject, key, runtime(globalObject).IndexError());
        RETURN_IF_EXCEPTION(scope, false);
        // (One that is not exact as a double is out of range anyway.)
        key = jsNumber(static_cast<double>(number));
    }
    RELEASE_AND_RETURN(scope, normalizeIndex(globalObject, key, list->length(), message, index));
}

static bool listSliceIndices(JSGlobalObject* globalObject, JSArray* list, PySlice* slice, PySlice::Indices& indices)
{
    if (!slice->unpack(globalObject, indices))
        return false;
    PySlice::adjust(list->length(), indices);
    return true;
}

static JSValue listGetItemMethod(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    if (hasIndex(globalObject, key)) {
        int64_t index;
        if (!listSubscriptIndex(globalObject, list, key, "list index out of range"_s, index))
            return { };
        return listGet(list, index);
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "list indices must be integers or slices, not "_s, typeName(globalObject, key));
    PySlice::Indices indices;
    if (!listSliceIndices(globalObject, list, slice, indices))
        return { };
    RELEASE_AND_RETURN(scope, listSlice(globalObject, list, indices));
}

// list[start:stop] = items
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
    for (size_t i = 0; i < items.size(); ++i) {
        listSet(globalObject, list, start + i, items[i]);
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
        if (!listSubscriptIndex(globalObject, list, key, "list assignment index out of range"_s, index))
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

    // The items are collected first: working them out may change the list, and they may be the list.
    MarkedArgumentBuffer items;
    if (value) {
        PyType* type = typeOf(globalObject, value);
        if (!type->lookup(Special::Iter) && !type->lookup(Special::GetItem)) {
            raiseTypeError(globalObject, scope, "must assign iterable to extended slice"_s);
            return;
        }
        forEach(globalObject, value, [&](JSValue item) {
            items.append(item);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, void());
    }
    std::span<const JSValue> itemSpan { reinterpret_cast<const JSValue*>(ArgList(items).data()), items.size() };

    PySlice::Indices indices;
    if (!listSliceIndices(globalObject, list, slice, indices))
        return;
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
        for (unsigned i = next; i < length; ++i) {
            if (left && i == next) {
                next += step;
                --left;
                continue;
            }
            listSet(globalObject, list, kept++, listGet(list, i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        RELEASE_AND_RETURN(scope, listTruncate(globalObject, list, kept));
    }
    if (static_cast<int64_t>(items.size()) != indices.length) {
        raiseValueError(globalObject, scope, "attempt to assign sequence of size "_s, items.size(), " to extended slice of size "_s, indices.length);
        return;
    }
    int64_t position = indices.start;
    for (int64_t i = 0; i < indices.length; ++i, position += indices.step) {
        listSet(globalObject, list, position, itemSpan[i]);
        RETURN_IF_EXCEPTION(scope, void());
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

static JSValue listContains(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    for (unsigned i = 0; i < list->length(); ++i) {
        bool equal = isEqual(globalObject, listGet(list, i), item);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            return jsBoolean(true);
    }
    return jsBoolean(false);
}

static JSValue listAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isList(other))
        return raiseTypeError(globalObject, scope, "can only concatenate list (not \""_s, typeName(globalObject, other), "\") to list"_s);
    JSArray* a = asList(self);
    JSArray* b = asList(other);
    uint64_t total = static_cast<uint64_t>(a->length()) + b->length();
    if (total > MAX_STORAGE_VECTOR_LENGTH)
        return raiseMemoryError(globalObject, scope, String());
    JSArray* result = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), total);
    if (!result)
        return raiseMemoryError(globalObject, scope, String());
    WriteBarrier<Unknown>* data = result->butterfly()->contiguous().data();
    unsigned lengthA = a->length();
    for (unsigned i = 0; i < lengthA; ++i)
        data[i].setWithoutWriteBarrier(listGet(a, i));
    for (unsigned i = 0; i < b->length(); ++i)
        data[lengthA + i].setWithoutWriteBarrier(listGet(b, i));
    return result;
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
    if (!repeatCount(globalObject, countValue, [&] { return list->length(); }, length, count))
        return { };
    JSArray* result = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length * count);
    if (!result)
        return raiseMemoryError(globalObject, scope, String());
    WriteBarrier<Unknown>* data = result->butterfly()->contiguous().data();
    for (uint64_t i = 0; i < count; ++i) {
        for (unsigned j = 0; j < length; ++j)
            data[i * length + j].setWithoutWriteBarrier(listGet(list, j));
    }
    return result;
}

static JSValue listIMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    unsigned length;
    uint64_t count;
    if (!repeatCount(globalObject, countValue, [&] { return list->length(); }, length, count))
        return { };
    if (!count) {
        listTruncate(globalObject, list, 0);
        RETURN_IF_EXCEPTION(scope, { });
        return self;
    }
    for (uint64_t i = 1; i < count; ++i) {
        for (unsigned j = 0; j < length; ++j) {
            list->push(globalObject, listGet(list, j));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return self;
}

static JSValue listAppendMethod(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    asList(self)->push(globalObject, item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listExtendMethod(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    listExtend(globalObject, asList(self), iterable);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue listInsertMethod(JSGlobalObject* globalObject, JSValue self, JSValue indexValue, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    int64_t index = toSsize(globalObject, indexValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
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
    if (indexValue) {
        index = toSsize(globalObject, indexValue, runtime(globalObject).OverflowError());
        RETURN_IF_EXCEPTION(scope, { });
    }
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
    for (unsigned i = 0; i < list->length(); ++i) {
        bool equal = isEqual(globalObject, listGet(list, i), item);
        RETURN_IF_EXCEPTION(scope, { });
        if (!equal)
            continue;
        if (i < list->length()) {
            listRemove(globalObject, list, i);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return jsUndefined();
    }
    return raiseValueError(globalObject, scope, "list.remove(x): x not in list"_s);
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    unsigned count = 0;
    for (unsigned i = 0; i < list->length(); ++i) {
        bool equal = isEqual(globalObject, listGet(list, i), item);
        RETURN_IF_EXCEPTION(scope, { });
        count += equal;
    }
    return jsNumber(count);
}

static JSValue listIndex(JSGlobalObject* globalObject, JSValue self, JSValue item, JSValue start, JSValue stop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    int64_t from, to;
    if (!unpackSliceBounds(globalObject, start, stop, from, to))
        return { };
    // Not clipped to the length: the list may grow as it is searched.
    if (from < 0)
        from = std::max<int64_t>(from + list->length(), 0);
    if (to < 0)
        to = std::max<int64_t>(to + list->length(), 0);
    for (int64_t i = from; i < to && i < list->length(); ++i) {
        bool equal = isEqual(globalObject, listGet(list, i), item);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            return jsNumber(static_cast<double>(i));
    }
    return raiseValueError(globalObject, scope, "list.index(x): x not in list"_s);
}

static JSValue listReverse(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asList(self);
    unsigned length = list->length();
    for (unsigned i = 0; i < length / 2; ++i) {
        JSValue first = listGet(list, i);
        listSet(globalObject, list, i, listGet(list, length - 1 - i));
        RETURN_IF_EXCEPTION(scope, { });
        listSet(globalObject, list, length - 1 - i, first);
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
    PY_METHOD("__getitem__", listGetItemMethod, "($self, index, /)"),
    PY_SLOT("__setitem__", listSetItemMethod, "($self, key, value, /)"),
    PY_SLOT("__delitem__", listDelItemMethod, "($self, key, /)"),
    PY_SLOT("__contains__", listContains, "($self, key, /)"),
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
    PY_METHOD("extend", listExtendMethod, "($self, iterable, /)"),
    PY_METHOD("pop", listPop, "($self, index=-1, /)"),
    PY_METHOD("remove", listRemoveMethod, "($self, value, /)"),
    PY_METHOD("index", listIndex, "($self, value, start=0, stop=9223372036854775807, /)"),
    PY_METHOD("count", listCount, "($self, value, /)"),
    PY_METHOD("reverse", listReverse, "($self, /)"),
    PY_METHOD("sort", listSortMethod, "($self, /, *, key=None, reverse=False)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

void initializeListTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.listType()->initializeBuiltin(globalObject, { "list"_s, { TypeFlag::Base, TypeFlag::ListSubclass, TypeFlag::Sequence, TypeFlag::MatchSelf, TypeFlag::OwnLayout }, listMethods, { }, "Built-in mutable sequence.\n\nIf no argument is given, the constructor creates a new empty list.\nThe argument must be an iterable if specified."_s }, rt.typeType(), rt.objectType());
    rt.listType()->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
    rt.listType()->didChange(vm);
}

// ---- tuple ----

int64_t hashOfTuple(JSGlobalObject* globalObject, PyTuple* tuple)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isSafeToRecurseWithMargin(vm, 64 * KB)) [[unlikely]] {
        raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded while hashing a tuple"_s);
        return -1;
    }
    // xxHash, simplified.
    static constexpr uint64_t prime1 = 11400714785074694791ULL;
    static constexpr uint64_t prime2 = 14029467366897019727ULL;
    static constexpr uint64_t prime5 = 2870177450012600261ULL;
    uint64_t accumulator = prime5;
    unsigned length = tuple->length();
    for (unsigned i = 0; i < length; ++i) {
        int64_t lane = hash(globalObject, tuple->at(i));
        RETURN_IF_EXCEPTION(scope, -1);
        accumulator += static_cast<uint64_t>(lane) * prime2;
        accumulator = (accumulator << 31) | (accumulator >> 33);
        accumulator *= prime1;
    }
    accumulator += length ^ (prime5 ^ 3527539UL);
    if (accumulator == static_cast<uint64_t>(-1))
        return 1546275796;
    return static_cast<int64_t>(accumulator);
}

static JSValue tupleNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    if (!rejectKeywordsInNew(globalObject, "tuple"_s, type, rt.tupleType(), args))
        return { };
    if (!checkPositional(globalObject, "tuple"_s, args, 0, 1))
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
    return makeStr(globalObject, builder);
}

static JSValue tupleHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = hashOfTuple(globalObject, PyTuple::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    return makeInt(globalObject, result);
}

template<CompareOperator op>
static JSValue tupleCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!PyTuple::is(other))
        return runtime(globalObject).notImplemented();
    return compareSequences(globalObject, op, TupleItems { PyTuple::from(self) }, TupleItems { PyTuple::from(other) });
}

static JSValue tupleLen(JSGlobalObject*, JSValue self) { return jsNumber(PyTuple::from(self)->length()); }

static JSValue tupleGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index;
        if (!normalizeIndex(globalObject, key, tuple->length(), "tuple index out of range"_s, index))
            return { };
        return tuple->at(index);
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "tuple indices must be integers or slices, not "_s, typeName(globalObject, key));
    PySlice::Indices indices;
    if (!slice->indices(globalObject, tuple->length(), indices))
        return { };
    if (indices.length <= 0)
        return rt.emptyTuple();
    if (!indices.start && indices.step == 1 && indices.length == tuple->length() && tuple->type() == rt.tupleType())
        return tuple;
    PyTuple* result = PyTuple::create(globalObject, indices.length);
    int64_t position = indices.start;
    for (int64_t i = 0; i < indices.length; ++i, position += indices.step)
        result->initializeAt(vm, i, tuple->at(position));
    return result;
}

static JSValue tupleIter(JSGlobalObject* globalObject, JSValue self) { return PyIterator::create(globalObject, PyIterator::Kind::Tuple, self); }

static JSValue tupleContains(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    for (unsigned i = 0; i < tuple->length(); ++i) {
        bool equal = isEqual(globalObject, tuple->at(i), item);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            return jsBoolean(true);
    }
    return jsBoolean(false);
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
    PyTuple* tuple = PyTuple::from(self);
    unsigned length;
    uint64_t count;
    if (!repeatCount(globalObject, countValue, [&] { return tuple->length(); }, length, count))
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
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    unsigned count = 0;
    for (unsigned i = 0; i < tuple->length(); ++i) {
        bool equal = isEqual(globalObject, tuple->at(i), item);
        RETURN_IF_EXCEPTION(scope, { });
        count += equal;
    }
    return jsNumber(count);
}

static JSValue tupleIndex(JSGlobalObject* globalObject, JSValue self, JSValue item, JSValue start, JSValue stop)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    int64_t from, to;
    if (!sliceBounds(globalObject, start, stop, tuple->length(), from, to))
        return { };
    for (int64_t i = from; i < to; ++i) {
        bool equal = isEqual(globalObject, tuple->at(i), item);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            return jsNumber(static_cast<double>(i));
    }
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
    PY_SLOT("__hash__", tupleHash, "($self, /)"),
    PY_SLOT("__eq__", tupleCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", tupleCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", tupleCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", tupleCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", tupleCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", tupleCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", tupleIter, "($self, /)"),
    PY_SLOT("__len__", tupleLen, "($self, /)"),
    PY_SLOT("__getitem__", tupleGetItem, "($self, key, /)"),
    PY_SLOT("__contains__", tupleContains, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__add__", tupleAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__mul__", tupleMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", tupleMul, "($self, value, /)"),
    PY_METHOD("__getnewargs__", tupleGetNewArgs, "($self, /)"),
    PY_METHOD("index", tupleIndex, "($self, value, start=0, stop=9223372036854775807, /)"),
    PY_METHOD("count", tupleCount, "($self, value, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

void initializeTupleTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.tupleType()->initializeBuiltin(globalObject, { "tuple"_s, { TypeFlag::Base, TypeFlag::TupleSubclass, TypeFlag::Sequence, TypeFlag::MatchSelf, TypeFlag::OwnLayout }, tupleMethods, { }, "Built-in immutable sequence.\n\nIf no argument is given, the constructor returns an empty tuple.\nIf iterable is specified the tuple is initialized from iterable's items.\n\nIf the argument is a tuple, the return value is the same object."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
