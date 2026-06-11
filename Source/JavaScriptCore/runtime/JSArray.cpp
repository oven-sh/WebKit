/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003-2021 Apple Inc. All rights reserved.
 *  Copyright (C) 2003 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2006 Alexey Proskuryakov (ap@nypop.com)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "JSArray.h"

#include "ArrayPrototypeInlines.h"
#include "GCDeferralContextInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "PropertyNameArray.h"
#include "ResourceExhaustion.h"
#include "ScopedArguments.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include <wtf/Assertions.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSArray);

template<typename ElementType>
static ALWAYS_INLINE bool tryGrowAndShiftButterflyRight(JSObject* object, VM& vm, Butterfly* butterfly, unsigned oldLength, unsigned newLength, unsigned startIndex, unsigned count)
{
    ASSERT(newLength > butterfly->vectorLength());
    ASSERT(newLength <= MAX_STORAGE_VECTOR_LENGTH);

#if USE(JSVALUE64)
    // SPEC-objectmodel §4.4 T1 (Task 8; GT10's JSArray.cpp:96 site): flag-on,
    // a lock-free COPYING flat resize is legal ONLY with expected tag exactly
    // (currentButterflyTID(), 0) (I27). Foreign/SW=1/segmented words return
    // false - the callers' fallback is the ArrayStorage route, whose
    // vector-moving relayout is the §4.6 AS-COPY sanctioned for shared
    // objects (an unshift MOVES elements, which T2 spine growth never does -
    // I27 forbids copying element storage from a non-(currentTID, 0)
    // butterfly outside STW).
    uint64_t expectedTaggedWord = 0;
    if (Options::useJSThreads()) [[unlikely]] {
        expectedTaggedWord = static_cast<JSObjectWithButterfly*>(object)->taggedButterflyWord();
        if (isSegmentedButterfly(expectedTaggedWord)
            || butterflySharedWrite(expectedTaggedWord)
            || butterflyTID(expectedTaggedWord) != currentButterflyTID()
            || untaggedButterfly(expectedTaggedWord) != butterfly)
            return false;
    }
#endif

    Structure* structure = object->structure();
    unsigned propertyCapacity = structure->outOfLineCapacity();
    unsigned oldVectorLength = butterfly->vectorLength();

    unsigned availableOldLength = Butterfly::availableContiguousVectorLength(propertyCapacity, oldVectorLength);
    if (availableOldLength >= newLength)
        return false;

    void* theBase = butterfly->base(0, propertyCapacity);
    bool canReallocInPlace = !propertyCapacity && !vm.heap.mutatorShouldBeFenced() && std::bit_cast<HeapCell*>(theBase)->isPreciseAllocation();
    if (canReallocInPlace)
        return false;

    unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, std::min<size_t>(nextLength(newLength), MAX_STORAGE_VECTOR_LENGTH));

    size_t newPayloadSize = newVectorLength * sizeof(ElementType);

    GCDeferralContext deferralContext(vm);
    AssertNoGC assertNoGC;
    constexpr unsigned preCapacity = 0;
    Butterfly* newButterfly = Butterfly::tryCreateUninitialized(vm, object, preCapacity, propertyCapacity, true, newPayloadSize, &deferralContext);
    if (!newButterfly) [[unlikely]]
        return false;

    // Copy property storage + indexing header (the "prefix" that lives before the array data).
    size_t prefixSize = propertyCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader);
    memcpy(newButterfly->base(0, propertyCapacity), theBase, prefixSize);

    ElementType* oldData = butterfly->indexingPayload<ElementType>();
    ElementType* newData = newButterfly->indexingPayload<ElementType>();

    // This butterfly is not yet visible to GC markers, so a plain memcpy is safe.
    if (startIndex)
        memcpy(newData, oldData, startIndex * sizeof(ElementType));

    for (unsigned i = startIndex; i < startIndex + count; ++i)
        clearElement(newData[i]);

    if (unsigned moveCount = oldLength - startIndex)
        memcpy(newData + startIndex + count, oldData + startIndex, moveCount * sizeof(ElementType));

    for (unsigned i = oldLength + count; i < newVectorLength; ++i)
        clearElement(newData[i]);

    newButterfly->setVectorLength(newVectorLength);
    newButterfly->setPublicLength(newLength);

#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // §4.4: ONE 64-bit CAS on the tagged word, new tag identical
        // (currentTID, 0) (T1/I27/I16). Failure = an SW flip (or conversion)
        // won mid-resize: NEVER re-copy (the foreign store into the old
        // butterfly would be lost, I21) - return false and let the caller
        // re-dispatch (its ArrayStorage fallback = the T2-side route).
        WTF::storeStoreFence(); // Contents before publication.
        if (!casButterfly(static_cast<JSObjectWithButterfly*>(object), expectedTaggedWord,
                encodeButterfly(newButterfly, currentButterflyTID(), false)))
            return false;
        vm.writeBarrier(object);
        return true;
    }
#endif
    object->setButterfly(vm, newButterfly);
    return true;
}

const ClassInfo JSArray::s_info = { "Array"_s, &JSNonFinalObject::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSArray) };

JSArray* JSArray::tryCreateUninitializedRestricted(ObjectInitializationScope& scope, GCDeferralContext* deferralContext, Structure* structure, unsigned initialLength)
{
    VM& vm = scope.vm();

    if (initialLength > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
        return nullptr;

    unsigned outOfLineStorage = structure->outOfLineCapacity();
    Butterfly* butterfly;
    IndexingType indexingType = structure->indexingType();
    if (!hasAnyArrayStorage(indexingType)) [[likely]] {
        ASSERT(
            hasUndecided(indexingType)
            || hasInt32(indexingType)
            || hasDouble(indexingType)
            || hasContiguous(indexingType));

        unsigned vectorLength = Butterfly::optimalContiguousVectorLength(structure, initialLength);
        void* temp = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(0, outOfLineStorage, true, vectorLength * sizeof(EncodedJSValue)),
            deferralContext, AllocationFailureMode::ReturnNull);
        if (!temp) [[unlikely]]
            return nullptr;
        butterfly = Butterfly::fromBase(temp, 0, outOfLineStorage);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(initialLength);
        if (hasDouble(indexingType)) {
            for (unsigned i = initialLength; i < vectorLength; ++i)
                butterfly->contiguousDouble().atUnsafe(i) = PNaN;
        } else {
            for (unsigned i = initialLength; i < vectorLength; ++i)
                butterfly->contiguous().atUnsafe(i).clear();
        }
    } else {
        ASSERT(
            indexingType == ArrayWithSlowPutArrayStorage
            || indexingType == ArrayWithArrayStorage);
        static constexpr unsigned indexBias = 0;
        unsigned vectorLength = ArrayStorage::optimalVectorLength(indexBias, structure, initialLength);
        void* temp = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(indexBias, outOfLineStorage, true, ArrayStorage::sizeFor(vectorLength)),
            deferralContext, AllocationFailureMode::ReturnNull);
        if (!temp) [[unlikely]]
            return nullptr;
        butterfly = Butterfly::fromBase(temp, indexBias, outOfLineStorage);
        *butterfly->indexingHeader() = indexingHeaderForArrayStorage(initialLength, vectorLength);
        ArrayStorage* storage = butterfly->arrayStorage();
        storage->m_indexBias = indexBias;
        storage->m_sparseMap.clear();
        storage->m_numValuesInVector = initialLength;
        for (unsigned i = initialLength; i < vectorLength; ++i)
            storage->m_vector[i].clear();
    }

    JSArray* result = createWithButterfly(vm, deferralContext, structure, butterfly);

    scope.notifyAllocated(result);
    return result;
}

void JSArray::eagerlyInitializeButterfly(ObjectInitializationScope& scope, JSArray* array, unsigned initialLength)
{
    Structure* structure = array->structure();
    IndexingType indexingType = structure->indexingType();
    Butterfly* butterfly = array->butterfly();

    // This function only serves as a companion to tryCreateUninitializedRestricted()
    // in the event that we really can't defer initialization of the butterfly after all.
    // tryCreateUninitializedRestricted() already initialized the elements between
    // initialLength and vector length. We just need to do 0 - initialLength.
    // ObjectInitializationScope::notifyInitialized() will verify that all elements are
    // initialized.
    if (!hasAnyArrayStorage(indexingType)) [[likely]] {
        if (hasDouble(indexingType)) {
            for (unsigned i = 0; i < initialLength; ++i)
                butterfly->contiguousDouble().atUnsafe(i) = PNaN;
        } else {
            for (unsigned i = 0; i < initialLength; ++i)
                butterfly->contiguous().atUnsafe(i).clear();
        }
    } else {
        ArrayStorage* storage = butterfly->arrayStorage();
        for (unsigned i = 0; i < initialLength; ++i)
            storage->m_vector[i].clear();
    }
    scope.notifyInitialized(array);
}

void JSArray::setLengthWritable(JSGlobalObject* globalObject, bool writable)
{
    ASSERT(isLengthWritable() || !writable);
    if (!isLengthWritable() || writable)
        return;

    enterDictionaryIndexingMode(globalObject->vm());

    SparseArrayValueMap* map = arrayStorage()->m_sparseMap.get();
    ASSERT(map);
    map->setLengthIsReadOnly();
}

// https://tc39.es/ecma262/#sec-array-exotic-objects-defineownproperty-p-desc
bool JSArray::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool throwException)
{
    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
        return false;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSArray* array = uncheckedDowncast<JSArray>(object);

    // 2. If P is "length", then
    // https://tc39.es/ecma262/#sec-arraysetlength
    if (propertyName == vm.propertyNames->length) {
        // FIXME: Nothing prevents this from being called on a RuntimeArray, and the length function will always return 0 in that case.
        unsigned newLength = array->length();
        if (descriptor.value()) {
            newLength = descriptor.value().toUInt32(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            double valueAsNumber = descriptor.value().toNumber(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (valueAsNumber != static_cast<double>(newLength)) {
                throwRangeError(globalObject, scope, "Invalid array length"_s);
                return false;
            }
        }

        // OrdinaryDefineOwnProperty (https://tc39.es/ecma262/#sec-validateandapplypropertydescriptor) at steps 1.a, 11.a, and 15 is now performed:
        // 4. If current.[[Configurable]] is false, then
        // 4.a. If Desc.[[Configurable]] is present and its value is true, return false.
        if (descriptor.configurablePresent() && descriptor.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeConfigurabilityError);
        // 4.b. If Desc.[[Enumerable]] is present and SameValue(Desc.[[Enumerable]], current.[[Enumerable]]) is false, return false.
        if (descriptor.enumerablePresent() && descriptor.enumerable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeEnumerabilityError);
        // 6. Else if SameValue(IsDataDescriptor(current), IsDataDescriptor(Desc)) is false, then
        // 6.a. If current.[[Configurable]] is false, return false.
        if (descriptor.isAccessorDescriptor())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeAccessMechanismError);
        // 7. Else if IsDataDescriptor(current) and IsDataDescriptor(Desc) are both true, then
        // 7.a. If current.[[Configurable]] is false and current.[[Writable]] is false, then
        if (!array->isLengthWritable()) {
            // 7.a.i. If Desc.[[Writable]] is present and Desc.[[Writable]] is true, return false.
            // This check is unaffected by steps 13-14 of ArraySetLength as they change non-writable descriptors only.
            if (descriptor.writablePresent() && descriptor.writable())
                return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeWritabilityError);
            // 7.a.ii. If Desc.[[Value]] is present and SameValue(Desc.[[Value]], current.[[Value]]) is false, return false.
            // This check also covers step 12 of ArraySetLength, which is only reachable if newLen < oldLen.
            if (newLength != array->length())
                return typeError(globalObject, scope, throwException, ReadonlyPropertyChangeError);
        }

        // setLength() clears indices >= newLength and sets correct "length" value if [[Delete]] fails (step 17.b.i)
        bool success = true;
        if (newLength != array->length()) {
            success = array->setLength(globalObject, newLength, throwException);
            EXCEPTION_ASSERT(!scope.exception() || !success);
        }
        if (descriptor.writablePresent())
            array->setLengthWritable(globalObject, descriptor.writable());
        return success;
    }

    // 4. Else if P is an array index (15.4), then
    // a. Let index be ToUint32(P).
    if (std::optional<uint32_t> optionalIndex = parseIndex(propertyName)) {
        // b. Reject if index >= oldLen and oldLenDesc.[[Writable]] is false.
        uint32_t index = optionalIndex.value();
        // FIXME: Nothing prevents this from being called on a RuntimeArray, and the length function will always return 0 in that case.
        if (index >= array->length() && !array->isLengthWritable())
            return typeError(globalObject, scope, throwException, "Attempting to define numeric property on array with non-writable length property."_s);
        // c. Let succeeded be the result of calling the default [[DefineOwnProperty]] internal method (8.12.9) on A passing P, Desc, and false as arguments.
        // d. Reject if succeeded is false.
        // e. If index >= oldLen
        // e.i. Set oldLenDesc.[[Value]] to index + 1.
        // e.ii. Call the default [[DefineOwnProperty]] internal method (8.12.9) on A passing "length", oldLenDesc, and false as arguments. This call will always return true.
        // f. Return true.
        RELEASE_AND_RETURN(scope, array->defineOwnIndexedProperty(globalObject, index, descriptor, throwException));
    }

    RELEASE_AND_RETURN(scope, array->JSObject::defineOwnNonIndexProperty(globalObject, propertyName, descriptor, throwException));
}

bool JSArray::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSArray* thisObject = uncheckedDowncast<JSArray>(object);
    if (propertyName == vm.propertyNames->length) {
        unsigned attributes = thisObject->isLengthWritable() ? PropertyAttribute::DontDelete | PropertyAttribute::DontEnum : PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly;
        slot.setValue(thisObject, attributes, jsNumber(thisObject->length()));
        return true;
    }

    return JSObject::getOwnPropertySlot(thisObject, globalObject, propertyName, slot);
}

// https://tc39.es/ecma262/#sec-array-exotic-objects-defineownproperty-p-desc
bool JSArray::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSArray* thisObject = uncheckedDowncast<JSArray>(cell);
    thisObject->ensureWritable(vm);

    if (propertyName == vm.propertyNames->length) {
        if (!thisObject->isLengthWritable()) {
            if (slot.isStrictMode())
                throwTypeError(globalObject, scope, "Array length is not writable"_s);
            return false;
        }

        if (slot.thisValue() != thisObject) [[unlikely]]
            RELEASE_AND_RETURN(scope, JSObject::definePropertyOnReceiver(globalObject, propertyName, value, slot));

        unsigned newLength = value.toUInt32(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        double valueAsNumber = value.toNumber(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (valueAsNumber != static_cast<double>(newLength)) [[unlikely]] {
            throwException(globalObject, scope, createRangeError(globalObject, "Invalid array length"_s));
            return false;
        }
        RELEASE_AND_RETURN(scope, thisObject->setLength(globalObject, newLength, slot.isStrictMode()));
    }

    RELEASE_AND_RETURN(scope, JSObject::put(thisObject, globalObject, propertyName, value, slot));
}

bool JSArray::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSArray* thisObject = uncheckedDowncast<JSArray>(cell);

    if (propertyName == vm.propertyNames->length)
        return false;

    return JSObject::deleteProperty(thisObject, globalObject, propertyName, slot);
}

static int NODELETE compareKeysForQSort(const void* a, const void* b)
{
    unsigned da = *static_cast<const unsigned*>(a);
    unsigned db = *static_cast<const unsigned*>(b);
    return (da > db) - (da < db);
}

void JSArray::getOwnSpecialPropertyNames(JSObject*, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    if (mode == DontEnumPropertiesMode::Include)
        propertyNames.add(vm.propertyNames->length);
}

// This method makes room in the vector, but leaves the new space for count slots uncleared.
bool JSArray::unshiftCountSlowCase(const AbstractLocker&, VM& vm, DeferGC&, bool addToFront, unsigned count)
{
    ASSERT(cellLock().isLocked());

    ArrayStorage* storage = ensureArrayStorage(vm);
    Butterfly* butterfly = storage->butterfly();
    Structure* structure = this->structure();
    unsigned propertyCapacity = structure->outOfLineCapacity();
    unsigned propertySize = structure->outOfLineSize();
    
    // If not, we should have handled this on the fast path.
    ASSERT(!addToFront || count > storage->m_indexBias);

    // Step 1:
    // Gather 4 key metrics:
    //  * usedVectorLength - how many entries are currently in the vector (conservative estimate - fewer may be in use in sparse vectors).
    //  * requiredVectorLength - how many entries are will there be in the vector, after allocating space for 'count' more.
    //  * currentCapacity - what is the current size of the vector, including any pre-capacity.
    //  * desiredCapacity - how large should we like to grow the vector to - based on 2x requiredVectorLength.

    unsigned length = storage->length();
    unsigned oldVectorLength = storage->vectorLength();
    unsigned usedVectorLength = std::min(oldVectorLength, length);
    ASSERT(usedVectorLength <= MAX_STORAGE_VECTOR_LENGTH);
    // Check that required vector length is possible, in an overflow-safe fashion.
    if (count > MAX_STORAGE_VECTOR_LENGTH - usedVectorLength)
        return false;
    unsigned requiredVectorLength = usedVectorLength + count;
    ASSERT(requiredVectorLength <= MAX_STORAGE_VECTOR_LENGTH);
    // The sum of m_vectorLength and m_indexBias will never exceed MAX_STORAGE_VECTOR_LENGTH.
    ASSERT(storage->vectorLength() <= MAX_STORAGE_VECTOR_LENGTH && (MAX_STORAGE_VECTOR_LENGTH - storage->vectorLength()) >= storage->m_indexBias);
    unsigned currentCapacity = storage->vectorLength() + storage->m_indexBias;
    // The calculation of desiredCapacity won't overflow, due to the range of MAX_STORAGE_VECTOR_LENGTH.
    // FIXME: This code should be fixed to avoid internal fragmentation. It's not super high
    // priority since increaseVectorLength() will "fix" any mistakes we make, but it would be cool
    // to get this right eventually.
    unsigned desiredCapacity = std::min(MAX_STORAGE_VECTOR_LENGTH, std::max(BASE_ARRAY_STORAGE_VECTOR_LEN, requiredVectorLength) << 1);

    // Step 2:
    // We're either going to choose to allocate a new ArrayStorage, or we're going to reuse the existing one.

    void* newAllocBase = nullptr;
    unsigned newStorageCapacity;
    bool allocatedNewStorage;
    bool canReuseExistingStorage = currentCapacity > desiredCapacity && isDenseEnoughForVector(currentCapacity, requiredVectorLength);
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.6 AS-COPY (Task 8): flag-on, AS innards are never
    // relaid out in place - any vector move / indexBias / vectorLength change
    // allocates a fresh AS butterfly under the cell lock (held by the caller;
    // its DeferGC is O1's sanctioned back-edge) and publishes via casButterfly.
    if (Options::useJSThreads()) [[unlikely]]
        canReuseExistingStorage = false;
#endif
    // If the current storage array is sufficiently large (but not too large!) then just keep using it.
    if (canReuseExistingStorage) {
        newAllocBase = butterfly->base(structure);
        newStorageCapacity = currentCapacity;
        allocatedNewStorage = false;
    } else {
        const unsigned preCapacity = 0;
        Butterfly* newButterfly = Butterfly::tryCreateUninitialized(vm, this, preCapacity, propertyCapacity, true, ArrayStorage::sizeFor(desiredCapacity));
        if (!newButterfly)
            return false;
        newAllocBase = newButterfly->base(preCapacity, propertyCapacity);
        newStorageCapacity = desiredCapacity;
        allocatedNewStorage = true;
    }

    // Step 3:
    // Work out where we're going to move things to.

    // Determine how much of the vector to use as pre-capacity, and how much as post-capacity.
    // If we're adding to the end, we'll add all the new space to the end.
    // If the vector had no free post-capacity (length >= m_vectorLength), don't give it any.
    // If it did, we calculate the amount that will remain based on an atomic decay - leave the
    // vector with half the post-capacity it had previously.
    unsigned postCapacity = 0;
    if (!addToFront)
        postCapacity = newStorageCapacity - requiredVectorLength;
    else if (length < storage->vectorLength()) {
        // Atomic decay, + the post-capacity cannot be greater than what is available.
        postCapacity = std::min((storage->vectorLength() - length) >> 1, newStorageCapacity - requiredVectorLength);
        // If we're moving contents within the same allocation, the post-capacity is being reduced.
        ASSERT(newAllocBase != butterfly->base(structure) || postCapacity < storage->vectorLength() - length);
    }

    unsigned newVectorLength = requiredVectorLength + postCapacity;
    RELEASE_ASSERT(newVectorLength <= MAX_STORAGE_VECTOR_LENGTH);
    unsigned preCapacity = newStorageCapacity - newVectorLength;

    Butterfly* newButterfly = Butterfly::fromBase(newAllocBase, preCapacity, propertyCapacity);

    {
        // When moving Butterfly's head to adjust property-storage, we must take a structure lock.
        // Otherwise, concurrent JIT compiler accesses to a property storage which is half-baked due to move for shift / unshift.
        // If the butterfly is newly allocated one, we do not need to take a lock since this is not changing the old butterfly.
        ConcurrentJSLocker structureLock(allocatedNewStorage ? nullptr : &structure->lock());
        if (addToFront) {
            ASSERT(count + usedVectorLength <= newVectorLength);
            gcSafeMemmove(newButterfly->arrayStorage()->m_vector + count, storage->m_vector, sizeof(JSValue) * usedVectorLength);
            gcSafeMemmove(newButterfly->propertyStorage() - propertySize, butterfly->propertyStorage() - propertySize, sizeof(JSValue) * propertySize + sizeof(IndexingHeader) + ArrayStorage::sizeFor(0));

            // We don't need to zero the pre-capacity for the concurrent GC because it is not available to use as property storage.
            gcSafeZeroMemory(static_cast<JSValue*>(newButterfly->base(0, propertyCapacity)), (propertyCapacity - propertySize) * sizeof(JSValue));

            if (allocatedNewStorage) {
                // We will set the vectorLength to newVectorLength. We populated requiredVectorLength
                // (usedVectorLength + count), which is less. Clear the difference.
                for (unsigned i = requiredVectorLength; i < newVectorLength; ++i)
                    newButterfly->arrayStorage()->m_vector[i].clear();
            }
        } else if ((newAllocBase != butterfly->base(structure)) || (preCapacity != storage->m_indexBias)) {
            gcSafeMemmove(newButterfly->propertyStorage() - propertyCapacity, butterfly->propertyStorage() - propertyCapacity, sizeof(JSValue) * propertyCapacity + sizeof(IndexingHeader) + ArrayStorage::sizeFor(0));
            gcSafeMemmove(newButterfly->arrayStorage()->m_vector, storage->m_vector, sizeof(JSValue) * usedVectorLength);
            
            for (unsigned i = requiredVectorLength; i < newVectorLength; i++)
                newButterfly->arrayStorage()->m_vector[i].clear();
        }

        newButterfly->arrayStorage()->setVectorLength(newVectorLength);
        newButterfly->arrayStorage()->m_indexBias = preCapacity;

#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // GT10: this site stays cell-locked (the caller's locker), with
            // casButterfly as the publication form only (T3/I17); the storage
            // is always freshly allocated flag-on (AS-COPY above).
            ASSERT(allocatedNewStorage);
            publishArrayStorageButterflyLocked(vm, this, newButterfly);
        } else
#endif
            setButterfly(vm, newButterfly);
    }

    return true;
}

bool JSArray::setLengthWithArrayStorage(JSGlobalObject* globalObject, unsigned newLength, bool throwException, ArrayStorage* storage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(JSVALUE64)
    // SPEC-objectmodel I31/L3 (Task 8): flag-on, every runtime AS access -
    // sparse-map structural edits included (sparse maps stay in place,
    // runtime-only, locked on both sides, §4.6) - holds the cell lock; the
    // storage is re-read under it (AS-COPY republishes). The lock is dropped
    // before every throw (typeError allocates - O1).
    std::optional<Locker<JSCellLock>> threadsLocker;
    if (Options::useJSThreads()) [[unlikely]] {
        threadsLocker.emplace(cellLock());
        storage = arrayStorage();
    }
#endif

    unsigned length = storage->length();

    // If the length is read only then we enter sparse mode, so should enter the following 'if'.
    ASSERT(isLengthWritable() || storage->m_sparseMap);

    if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
        // Fail if the length is not writable.
        if (map->lengthIsReadOnly()) {
#if USE(JSVALUE64)
            threadsLocker.reset();
#endif
            return typeError(globalObject, scope, throwException, ReadonlyPropertyWriteError);
        }

        if (newLength < length) {
            // Copy any keys we might be interested in into a vector.
            Vector<unsigned, 0, UnsafeVectorOverflow> keys;
            keys.reserveInitialCapacity(std::min(map->size(), static_cast<size_t>(length - newLength)));
            SparseArrayValueMap::const_iterator end = map->end();
            for (SparseArrayValueMap::const_iterator it = map->begin(); it != end; ++it) {
                unsigned index = static_cast<unsigned>(it->key);
                if (index < length && index >= newLength)
                    keys.append(index);
            }

            // Check if the array is in sparse mode. If so there may be non-configurable
            // properties, so we have to perform deletion with caution, if not we can
            // delete values in any order.
            if (map->sparseMode()) {
                qsort(keys.begin(), keys.size(), sizeof(unsigned), compareKeysForQSort);
                unsigned i = keys.size();
                while (i) {
                    unsigned index = keys[--i];
                    SparseArrayValueMap::iterator it = map->find(index);
                    ASSERT(it != map->notFound());
                    if (it->value.attributes() & PropertyAttribute::DontDelete) {
                        storage->setLength(index + 1);
#if USE(JSVALUE64)
                        threadsLocker.reset();
#endif
                        return typeError(globalObject, scope, throwException, UnableToDeletePropertyError);
                    }
                    map->remove(it);
                }
            } else {
                for (unsigned i = 0; i < keys.size(); ++i)
                    map->remove(keys[i]);
                if (map->isEmpty())
                    deallocateSparseIndexMap();
            }
        }
    }

    if (newLength < length) {
        // Delete properties from the vector.
        unsigned usedVectorLength = std::min(length, storage->vectorLength());
        for (unsigned i = newLength; i < usedVectorLength; ++i) {
            WriteBarrier<Unknown>& valueSlot = storage->m_vector[i];
            bool hadValue = !!valueSlot;
            valueSlot.clear();
            storage->m_numValuesInVector -= hadValue;
        }
    }

    storage->setLength(newLength);

    return true;
}

#if USE(JSVALUE64)
// Review round 4 sweep (same blocker class as the appendMemcpy/fastSlice
// findings): flag-on single-snapshot probe for the remaining JSArray fast
// paths that deref butterfly() as flat. Loads the tagged word ONCE and
// produces that snapshot's flat view. Returns false - the caller bails to its
// generic route - when flag-on the word is Segmented (the flat accessors
// would garbage-decode the spine pointer), when the shape is ArrayStorage
// (these paths read/write AS innards without the I31 cell lock), or, for
// callers that mutate storage IN PLACE, when the word is not exclusively
// owned (currentTID, SW=0): a foreign SW=0 write needs the F1 fire (I12) and
// an SW=1 in-place fill/move loses racing foreign stores undetectably
// (I21/I27). Contract: every subsequent deref in the caller goes through the
// returned snapshot pointer - NEVER a butterfly() re-load (a foreign §4.2
// conversion needs no stop once the TTL sets are fired, so the regime can
// change mid-function; the snapshot itself stays sound - conversions alias
// the same memory and superseded flat snapshots are frozen + conservatively
// pinned, I7) - and reads stay within the SNAPSHOT's own vectorLength (the
// shared publicLength slot can be bumped past a superseded snapshot's
// storage by an aliased T2 grow). Flag-off this is exactly butterfly() (all
// tag bits zero - I22).
enum class JSThreadsFastPathIntent : uint8_t { Read, InPlaceWrite };
static ALWAYS_INLINE bool jsThreadsFlatSnapshot(JSObject* object, JSThreadsFastPathIntent intent, Butterfly*& butterfly)
{
    uint64_t word = object->taggedButterflyWord();
    if (Options::useJSThreads()) [[unlikely]] {
        if (isSegmentedButterfly(word))
            return false;
        if (!(word & butterflyPointerMask))
            return false; // E5 None-first: a racing N3 first install can pair a stale null word with a fresh indexed type.
        if (hasAnyArrayStorage(object->indexingType()))
            return false; // I31: AS access is cell-locked flag-on.
        if (intent == JSThreadsFastPathIntent::InPlaceWrite
            && (word & butterflyPointerMask)
            && (butterflySharedWrite(word) || butterflyWriterIsForeign(word))) // incl. §9.6 forceButterflySWBit
            return false;
    }
    butterfly = untaggedButterfly(word);
    return true;
}
#endif

bool JSArray::fastFill(VM& vm, unsigned startIndex, unsigned endIndex, JSValue value)
{
    if (isCopyOnWrite(indexingMode()))
        convertFromCopyOnWrite(vm);

    IndexingType type = indexingType();
    if (!(type & IsArray) || hasAnyArrayStorage(type))
        return false;
    IndexingType nextType = leastUpperBoundOfIndexingTypeAndValue(type, value);
    if (hasArrayStorage(nextType))
        return false;
    convertToIndexingTypeIfNeeded(vm, nextType);

    ASSERT(nextType == indexingType());

#if USE(JSVALUE64)
    Butterfly* butterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::InPlaceWrite, butterfly))
        return false; // round 4: segmented / shared / foreign / AS => generic fill.
    if (Options::useJSThreads() && endIndex > butterfly->vectorLength()) [[unlikely]]
        return false; // round 4: aliased publicLength can race past this snapshot's storage.
#else
    Butterfly* butterfly = this->butterfly();
#endif

    // There is a chance that endIndex is beyond the length. If it is, let's just fail.
    if (endIndex > butterfly->publicLength())
        return false;

    if (nextType == ArrayWithDouble) {
        auto* data = butterfly->contiguousDouble().data();
        double pattern = value.asNumber();
#if OS(DARWIN)
        memset_pattern8(data + startIndex, &pattern, sizeof(double) * (endIndex - startIndex));
#else
        std::fill(data + startIndex, data + endIndex, pattern);
#endif
    } else if (nextType == ArrayWithInt32) {
        auto* data = butterfly->contiguous().data();
        auto pattern = std::bit_cast<const WriteBarrier<Unknown>>(JSValue::encode(value));
#if OS(DARWIN)
        memset_pattern8(data + startIndex, &pattern, sizeof(JSValue) * (endIndex - startIndex));
#else
        std::fill(data + startIndex, data + endIndex, pattern);
#endif
        vm.writeBarrier(this);
    } else {
        // FIXME: https://bugs.webkit.org/show_bug.cgi?id=283786
        auto contiguousStorage = butterfly->contiguous();
        for (unsigned i = startIndex; i < endIndex; ++i)
            contiguousStorage.at(this, i).setWithoutWriteBarrier(value);
        vm.writeBarrier(this);
    }

    return true;
}

JSArray* JSArray::fastToReversed(JSGlobalObject* globalObject, uint64_t length)
{
    ASSERT(length <= std::numeric_limits<uint32_t>::max());

    VM& vm = globalObject->vm();

#if USE(JSVALUE64)
    Butterfly* sourceButterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, sourceButterfly))
        return nullptr; // round 4: segmented / flag-on AS => generic toReversed.
#else
    Butterfly* sourceButterfly = this->butterfly();
#endif

    auto sourceType = indexingType();
    switch (sourceType) {
    case ArrayWithInt32:
    case ArrayWithContiguous:
    case ArrayWithDouble: {
        if (length > sourceButterfly->vectorLength()) [[unlikely]]
            return nullptr;
        if (holesMustForwardToPrototype()) [[unlikely]]
            return nullptr;

        IndexingType resultType = sourceType;
        if (sourceType == ArrayWithDouble) {
            auto* buffer = sourceButterfly->contiguousDouble().data();
            if (containsHole(buffer, length)) [[unlikely]]
                resultType = ArrayWithContiguous;
        } else if (sourceType == ArrayWithInt32) {
            auto* buffer = sourceButterfly->contiguousInt32().data();
            if (containsHole(buffer, length)) [[unlikely]]
                resultType = ArrayWithContiguous;
        }

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(resultType);
        IndexingType indexingType = resultStructure->indexingType();
        if (hasAnyArrayStorage(indexingType)) [[unlikely]]
            return nullptr;
        ASSERT(!globalObject->isHavingABadTime());

        auto vectorLength = Butterfly::optimalContiguousVectorLength(resultStructure, length);
        void* memory = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)),
            nullptr, AllocationFailureMode::ReturnNull);
        if (!memory) [[unlikely]]
            return nullptr;
        auto* butterfly = Butterfly::fromBase(memory, 0, 0);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(length);

        if (hasDouble(indexingType)) {
            ASSERT(!containsHole(sourceButterfly->contiguousDouble().data(), length));
            auto* sourceBuffer = sourceButterfly->contiguousDouble().data();
            auto* resultBuffer = butterfly->contiguousDouble().data();
            copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            std::reverse(resultBuffer, resultBuffer + length);
        } else if (hasInt32(indexingType)) {
            ASSERT(!containsHole(sourceButterfly->contiguous().data(), length));
            auto* sourceBuffer = sourceButterfly->contiguous().data();
            auto* resultBuffer = butterfly->contiguous().data();
            copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            std::reverse(resultBuffer, resultBuffer + length);
        } else {
            auto* resultBuffer = butterfly->contiguous().data();
            if (sourceType == ArrayWithDouble) {
                auto* sourceBuffer = sourceButterfly->contiguousDouble().data();
                copyArrayElements<ArrayFillMode::Undefined, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            } else {
                auto* sourceBuffer = sourceButterfly->contiguous().data();
                copyArrayElements<ArrayFillMode::Undefined, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            }
            std::reverse(resultBuffer, resultBuffer + length);
        }
        Butterfly::clearRange(resultType, butterfly, length, vectorLength);
        return createWithButterfly(vm, nullptr, resultStructure, butterfly);
    }
    case ArrayWithArrayStorage: {
        // Flag-on this leg is unreachable (the round-4 snapshot probe bails for AS shapes - I31).
        auto& storage = *sourceButterfly->arrayStorage();
        if (storage.m_sparseMap.get())
            return nullptr;
        if (length > storage.vectorLength())
            return nullptr;
        if (storage.hasHoles())
            return nullptr;

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
        if (hasAnyArrayStorage(resultStructure->indexingType())) [[unlikely]]
            return nullptr;

        ASSERT(!globalObject->isHavingABadTime());
        ObjectInitializationScope scope(vm);
        JSArray* resultArray = JSArray::tryCreateUninitializedRestricted(scope, resultStructure, length);
        if (!resultArray) [[unlikely]]
            return nullptr;
        gcSafeMemcpy(resultArray->butterfly()->contiguous().data(), sourceButterfly->arrayStorage()->m_vector, sizeof(JSValue) * static_cast<uint32_t>(length));
        ASSERT(resultArray->butterfly()->publicLength() == length);

        auto data = resultArray->butterfly()->contiguous().data();
        std::reverse(data, data + length);
        vm.writeBarrier(resultArray);

        return resultArray;
    }
    default:
        return nullptr;
    }
}

JSArray* JSArray::fastWith(JSGlobalObject* globalObject, uint32_t index, JSValue value, uint64_t length)
{
    ASSERT(length <= std::numeric_limits<uint32_t>::max());

    VM& vm = globalObject->vm();

#if USE(JSVALUE64)
    Butterfly* sourceButterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, sourceButterfly))
        return nullptr; // round 4: segmented / flag-on AS => generic with().
#else
    Butterfly* sourceButterfly = this->butterfly();
#endif

    auto sourceType = indexingType();
    switch (sourceType) {
    case ArrayWithInt32:
    case ArrayWithContiguous:
    case ArrayWithDouble: {
        if (length > sourceButterfly->vectorLength()) [[unlikely]]
            return nullptr;
        if (holesMustForwardToPrototype()) [[unlikely]]
            return nullptr;

        IndexingType resultType = leastUpperBoundOfIndexingTypeAndValue(sourceType, value);
        if (sourceType == ArrayWithDouble) {
            auto* buffer = sourceButterfly->contiguousDouble().data();
            if (containsHole(buffer, length)) [[unlikely]]
                resultType = ArrayWithContiguous;
        } else if (sourceType == ArrayWithInt32) {
            auto* buffer = sourceButterfly->contiguousInt32().data();
            if (containsHole(buffer, length)) [[unlikely]]
                resultType = ArrayWithContiguous;
        }

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(resultType);
        IndexingType indexingType = resultStructure->indexingType();
        if (hasAnyArrayStorage(indexingType)) [[unlikely]]
            return nullptr;
        ASSERT(!globalObject->isHavingABadTime());

        auto vectorLength = Butterfly::optimalContiguousVectorLength(resultStructure, length);
        void* memory = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)),
            nullptr, AllocationFailureMode::ReturnNull);
        if (!memory) [[unlikely]]
            return nullptr;
        auto* butterfly = Butterfly::fromBase(memory, 0, 0);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(length);

        if (hasDouble(indexingType)) {
            auto* resultBuffer = butterfly->contiguousDouble().data();
            if (sourceType == ArrayWithDouble) {
                auto* sourceBuffer = sourceButterfly->contiguousDouble().data();
                copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            } else {
                ASSERT(sourceType == ArrayWithInt32);
                auto* sourceBuffer = sourceButterfly->contiguous().data();
                copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            }
            resultBuffer[index] = value.asNumber();
        } else if (hasInt32(indexingType)) {
            ASSERT(sourceType == ArrayWithInt32);
            auto* sourceBuffer = sourceButterfly->contiguous().data();
            auto* resultBuffer = butterfly->contiguous().data();
            copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            resultBuffer[index].setWithoutWriteBarrier(value);
        } else {
            auto* resultBuffer = butterfly->contiguous().data();
            if (sourceType == ArrayWithDouble) {
                auto* sourceBuffer = sourceButterfly->contiguousDouble().data();
                copyArrayElements<ArrayFillMode::Undefined, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            } else {
                auto* sourceBuffer = sourceButterfly->contiguous().data();
                copyArrayElements<ArrayFillMode::Undefined, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, length, sourceType);
            }
            resultBuffer[index].setWithoutWriteBarrier(value);
        }

        Butterfly::clearRange(indexingType, butterfly, length, vectorLength);
        return createWithButterfly(vm, nullptr, resultStructure, butterfly);
    }
    case ArrayWithArrayStorage: {
        auto& storage = *sourceButterfly->arrayStorage();
        if (storage.m_sparseMap.get())
            return nullptr;
        if (length > storage.vectorLength())
            return nullptr;
        if (storage.hasHoles())
            return nullptr;

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
        if (hasAnyArrayStorage(resultStructure->indexingType())) [[unlikely]]
            return nullptr;

        ASSERT(!globalObject->isHavingABadTime());
        ObjectInitializationScope scope(vm);
        JSArray* resultArray = JSArray::tryCreateUninitializedRestricted(scope, resultStructure, length);
        if (!resultArray) [[unlikely]]
            return nullptr;
        gcSafeMemcpy(resultArray->butterfly()->contiguous().data(), sourceButterfly->arrayStorage()->m_vector, sizeof(JSValue) * static_cast<uint32_t>(length));
        ASSERT(resultArray->butterfly()->publicLength() == length);

        resultArray->butterfly()->contiguous().at(resultArray, index).setWithoutWriteBarrier(value);
        vm.writeBarrier(resultArray);

        return resultArray;
    }
    default:
        return nullptr;
    }
}

std::optional<bool> JSArray::fastIncludes(JSGlobalObject* globalObject, JSValue searchElement, uint64_t index64, uint64_t length64)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool canDoFastPath = this->canDoFastIndexedAccess()
        && this->getArrayLength() == length64 // The effects in getting `index` could have changed the length of this array.
        && static_cast<uint32_t>(index64) == index64;
    if (!canDoFastPath)
        return std::nullopt;

    uint32_t length = static_cast<uint32_t>(length64);
    uint32_t index = static_cast<uint32_t>(index64);

#if USE(JSVALUE64)
    Butterfly* snapshotButterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, snapshotButterfly))
        return std::nullopt; // round 4: segmented / flag-on AS => generic includes.
    if (Options::useJSThreads() && length > snapshotButterfly->vectorLength()) [[unlikely]]
        return std::nullopt; // round 4: racing length growth past this snapshot's storage.
#else
    Butterfly* snapshotButterfly = this->butterfly();
#endif

    switch (this->indexingType()) {
    case ArrayWithInt32: {
        auto& butterfly = *snapshotButterfly;
        auto data = butterfly.contiguous().data();

        int32_t int32Value = 0;
        if (searchElement.isInt32AsAnyInt())
            int32Value = searchElement.asInt32AsAnyInt();
        else if (searchElement.isUndefined()) [[unlikely]]
            return containsHole(data + index, length - index);
        else if (!searchElement.isNumber() || searchElement.asNumber() != 0.0) [[unlikely]]
            return false;

        EncodedJSValue encodedSearchElement = JSValue::encode(jsNumber(int32Value));
        auto* result = WTF::find64(std::bit_cast<const uint64_t*>(data + index), encodedSearchElement, length - index);
        return static_cast<bool>(result);
    }
    case ArrayWithContiguous: {
        auto& butterfly = *snapshotButterfly;
        auto data = butterfly.contiguous().data();

        if (searchElement.isObject()) {
            auto* result = std::bit_cast<const WriteBarrier<Unknown>*>(WTF::find64(std::bit_cast<const uint64_t*>(data + index), JSValue::encode(searchElement), length - index));
            if (result)
                return true;
            return false;
        }

        bool searchElementIsUndefined = searchElement.isUndefined();
        for (; index < length; ++index) {
            JSValue value = data[index].get();
            if (!value) {
                if (searchElementIsUndefined)
                    return true;
                continue;
            }
            bool isEqual = sameValueZero(globalObject, searchElement, value);
            RETURN_IF_EXCEPTION(scope, { });
            if (isEqual)
                return true;
        }
        return false;
    }
    case ALL_DOUBLE_INDEXING_TYPES: {
        auto& butterfly = *snapshotButterfly;
        auto data = butterfly.contiguousDouble().data();

        if (searchElement.isUndefined())
            return containsHole(data + index, length - index);
        if (!searchElement.isNumber())
            return false;

        double searchNumber = searchElement.asNumber();
        for (; index < length; ++index) {
            if (data[index] == searchNumber)
                return true;
        }
        return false;
    }
    default:
        return std::nullopt;
    }
}

bool JSArray::fastCopyWithin(JSGlobalObject* globalObject, uint64_t from64, uint64_t to64, uint64_t count64, uint64_t length64)
{
    VM& vm = globalObject->vm();

    uint32_t from = static_cast<uint32_t>(from64);
    uint32_t to = static_cast<uint32_t>(to64);
    uint32_t count = static_cast<uint32_t>(count64);
    uint32_t length = static_cast<uint32_t>(length64);

    ASSERT(from + count <= length);
    ASSERT(to + count <= length);

    bool canDoFastPath = this->canDoFastIndexedAccess()
        && this->getArrayLength() == length
        && from64 == from
        && to64 == to
        && count64 == count;

    if (!canDoFastPath)
        return false;

    if (isCopyOnWrite(indexingMode()))
        convertFromCopyOnWrite(vm);

#if USE(JSVALUE64)
    Butterfly* snapshotButterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::InPlaceWrite, snapshotButterfly))
        return false; // round 4: segmented / shared / foreign / AS => generic copyWithin.
    if (Options::useJSThreads() && length > snapshotButterfly->vectorLength()) [[unlikely]]
        return false; // round 4: racing length growth past this snapshot's storage.
#else
    Butterfly* snapshotButterfly = this->butterfly();
#endif

    auto type = this->indexingType();
    switch (type) {
    case ArrayWithInt32:
    case ArrayWithContiguous: {
        auto data = snapshotButterfly->contiguous().data();

        if (containsHole(data, length))
            return false;

        std::span<WriteBarrier<Unknown>> destination { data + to, count };
        std::span<const WriteBarrier<Unknown>> source { data + from, count };

        if (type == ArrayWithInt32)
            memmoveSpan(destination, source);
        else {
            ASSERT(type == ArrayWithContiguous);
            gcSafeMemmove(destination.data(), source.data(), count * sizeof(JSValue));
            vm.writeBarrier(this);
        }
        return true;
    }
    case ArrayWithDouble: {
        auto data = snapshotButterfly->contiguousDouble().data();

        if (containsHole(data, length))
            return false;

        std::span<double> destination { data + to, count };
        std::span<double> source { data + from, count };

        memmoveSpan(destination, source);
        return true;
    }
    case ArrayWithArrayStorage: {
        auto& storage = *snapshotButterfly->arrayStorage();
        if (storage.m_sparseMap.get())
            return false;
        if (length > storage.vectorLength())
            return false;
        if (storage.hasHoles())
            return false;
        ASSERT(!globalObject->isHavingABadTime());

        auto vector = snapshotButterfly->arrayStorage()->m_vector;
        gcSafeMemmove(vector + to, vector + from, count * sizeof(JSValue));
        vm.writeBarrier(this);

        return true;
    }
    default:
        return false;
    }
}

JSArray* JSArray::fastToSpliced(JSGlobalObject* globalObject, CallFrame* callFrame, uint64_t length, uint64_t newLength, uint64_t start, uint64_t deleteCount, uint64_t insertCount)
{
    VM& vm = globalObject->vm();

    IndexingType sourceType = indexingType();
    switch (sourceType) {
    case ArrayWithInt32:
    case ArrayWithContiguous:
    case ArrayWithDouble: {
        if (newLength > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
            return nullptr;

        if (newLength >= MIN_SPARSE_ARRAY_INDEX) [[unlikely]]
            return nullptr;

#if USE(JSVALUE64)
        Butterfly* sourceButterfly;
        if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, sourceButterfly))
            return nullptr; // round 4: segmented (flag-on) => generic splice.
#else
        Butterfly* sourceButterfly = this->butterfly();
#endif

        if (length > sourceButterfly->vectorLength()) [[unlikely]]
            return nullptr;

        if (hasDouble(sourceType)) {
            if (containsHole(sourceButterfly->contiguousDouble().data(), static_cast<uint32_t>(length)))
                return nullptr;
        } else if (containsHole(sourceButterfly->contiguous().data(), static_cast<uint32_t>(length)))
            return nullptr;

        IndexingType insertedItemsIndexingType = sourceType;
        for (uint64_t i = 0; i < insertCount; i++)
            insertedItemsIndexingType = leastUpperBoundOfIndexingTypeAndValue(insertedItemsIndexingType, callFrame->uncheckedArgument(i + 2));

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(insertedItemsIndexingType);
        IndexingType resultIndexingType = resultStructure->indexingType();

        if (hasAnyArrayStorage(resultIndexingType)) [[unlikely]]
            return nullptr;
        ASSERT(!globalObject->isHavingABadTime());

        auto vectorLength = Butterfly::optimalContiguousVectorLength(resultStructure, newLength);
        void* memory = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)),
            nullptr, AllocationFailureMode::ReturnNull);
        if (!memory) [[unlikely]]
            return nullptr;
        auto* resultButterfly = Butterfly::fromBase(memory, 0, 0);
        resultButterfly->setVectorLength(vectorLength);
        resultButterfly->setPublicLength(newLength);

        auto copyArrayPrefixElements = [&]<typename T, typename U>(T* resultBuffer, U* sourceBuffer) {
            copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, 0, sourceBuffer, 0, start, sourceType);
        };
        auto copyArraySuffixElements = [&]<typename T, typename U>(T* resultBuffer, U* sourceBuffer) {
            copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(resultBuffer, start + insertCount, sourceBuffer, start + deleteCount, length - start - deleteCount, sourceType);
        };

        // round 4: single-snapshot - sourceButterfly was derived from ONE
        // loaded word above; never re-load butterfly() mid-function.
        if (hasDouble(resultIndexingType)) {
            ASSERT(sourceType == ArrayWithInt32 || sourceType == ArrayWithDouble);
            double* resultBuffer = resultButterfly->contiguousDouble().data();
            if (sourceType == ArrayWithDouble)
                copyArrayPrefixElements(resultBuffer, sourceButterfly->contiguousDouble().data());
            else
                copyArrayPrefixElements(resultBuffer, sourceButterfly->contiguous().data());
            for (uint64_t i = 0; i < insertCount; ++i) {
                JSValue value = callFrame->uncheckedArgument(i + 2);
                ASSERT(value.isNumber());
                resultButterfly->contiguousDouble().atUnsafe(start + i) = value.asNumber();
            }
            if (sourceType == ArrayWithDouble)
                copyArraySuffixElements(resultBuffer, sourceButterfly->contiguousDouble().data());
            else
                copyArraySuffixElements(resultBuffer, sourceButterfly->contiguous().data());
        } else if (hasInt32(resultIndexingType) || hasContiguous(resultIndexingType)) {
            auto* resultBuffer = resultButterfly->contiguous().data();
            if (sourceType == ArrayWithDouble)
                copyArrayPrefixElements(resultBuffer, sourceButterfly->contiguousDouble().data());
            else
                copyArrayPrefixElements(resultBuffer, sourceButterfly->contiguous().data());
            for (uint64_t i = 0; i < insertCount; ++i) {
                JSValue value = callFrame->uncheckedArgument(i + 2);
                resultButterfly->contiguous().atUnsafe(start + i).setWithoutWriteBarrier(value);
            }
            if (sourceType == ArrayWithDouble)
                copyArraySuffixElements(resultBuffer, sourceButterfly->contiguousDouble().data());
            else
                copyArraySuffixElements(resultBuffer, sourceButterfly->contiguous().data());
        } else
            RELEASE_ASSERT_NOT_REACHED();

        Butterfly::clearRange(resultIndexingType, resultButterfly, newLength, vectorLength);
        return createWithButterfly(vm, nullptr, resultStructure, resultButterfly);
    }
    default: {
        return nullptr;
    }
    }
}

JSString* JSArray::fastToString(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    unsigned length = this->length();

    StringRecursionChecker checker(globalObject, this);
    EXCEPTION_ASSERT(!scope.exception() || checker.earlyReturnValue());
    if (JSValue earlyReturnValue = checker.earlyReturnValue())
        return jsEmptyString(vm);

    if (canUseFastArrayJoin(this)) [[likely]] {
        const Latin1Character comma = ',';

        bool isCoW = isCopyOnWrite(this->indexingMode());
        JSCellButterfly* immutableButterfly = nullptr;
        if (isCoW) {
            immutableButterfly = JSCellButterfly::fromButterfly(this->butterfly());
            auto iter = vm.heap.immutableButterflyToStringCache.find(immutableButterfly);
            if (iter != vm.heap.immutableButterflyToStringCache.end())
                return iter->value;
        }

        bool sawHoles = false;
        bool genericCase = false;
        JSString* result = fastArrayJoin(globalObject, this, span(comma), length, sawHoles, genericCase);
        RETURN_IF_EXCEPTION(scope, { });

        if (!sawHoles && !genericCase && result && isCoW) {
            ASSERT(JSCellButterfly::fromButterfly(this->butterfly()) == immutableButterfly);
            vm.heap.immutableButterflyToStringCache.add(immutableButterfly, result);
        }

        return result;
    }

    JSStringJoiner joiner(","_s);
    for (unsigned i = 0; i < length; ++i) {
        JSValue element = this->tryGetIndexQuickly(i);
        if (!element) {
            element = this->get(globalObject, i);
            RETURN_IF_EXCEPTION(scope, { });
        }
        joiner.append(globalObject, element);
        RETURN_IF_EXCEPTION(scope, { });
    }

    RELEASE_AND_RETURN(scope, joiner.join(globalObject));
}

bool JSArray::appendMemcpy(JSGlobalObject* globalObject, VM& vm, unsigned startIndex, IndexingType otherType, std::span<const EncodedJSValue> values)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isCopyOnWrite(indexingMode()))
        convertFromCopyOnWrite(vm);

#if USE(JSVALUE64)
    // Review round 4 (blocker fix, span overload): destination-side regime
    // guard - the in-place stores below are exclusive-owner-flat only (the
    // source is a local buffer, so no source guard is needed). See the
    // JSArray* overload below for the full rationale; the destination is
    // re-probed after ensureLength.
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t selfWord = taggedButterflyWord();
        if (isSegmentedButterfly(selfWord)
            || ((selfWord & butterflyPointerMask)
                && (butterflySharedWrite(selfWord) || butterflyWriterIsForeign(selfWord)))) // incl. §9.6 forceButterflySWBit
            return false;
    }
#endif

    IndexingType type = indexingType();
    bool allowPromotion = false;
    IndexingType copyType = mergeIndexingTypeForCopying(otherType, allowPromotion);
    if (type == ArrayWithUndecided && copyType != NonArray) {
        if (copyType == ArrayWithInt32)
            convertUndecidedToInt32(vm);
        else if (copyType == ArrayWithDouble)
            convertUndecidedToDouble(vm);
        else if (copyType == ArrayWithContiguous)
            convertUndecidedToContiguous(vm);
        else {
            ASSERT(copyType == ArrayWithUndecided);
            return true;
        }
    } else if (type != copyType)
        return false;

    if (values.size() >= MIN_SPARSE_ARRAY_INDEX)
        return false;

    CheckedUint32 checkedNewLength = startIndex;
    checkedNewLength += values.size();

    if (checkedNewLength.hasOverflowed()) {
        throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
        return false;
    }
    unsigned newLength = checkedNewLength;

    if (newLength >= MIN_SPARSE_ARRAY_INDEX)
        return false;

    if (!ensureLength(vm, newLength)) {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    ASSERT(copyType == indexingType());

    // Review round 4 (blocker fix): re-probe + single-snapshot after
    // ensureLength - see the JSArray* overload below.
    Butterfly* selfButterfly;
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t selfWord = taggedButterflyWord();
        if (isSegmentedButterfly(selfWord)
            || !(selfWord & butterflyPointerMask)
            || butterflySharedWrite(selfWord) || butterflyWriterIsForeign(selfWord))
            return false; // Destination left the exclusive-owner flat regime mid-call.
        selfButterfly = untaggedButterfly(selfWord);
        if (newLength > selfButterfly->vectorLength()) [[unlikely]]
            return false; // Defensive: never write past this snapshot's storage.
    } else
        selfButterfly = this->butterfly();
#else
    selfButterfly = this->butterfly();
#endif

    if (otherType == ArrayWithUndecided) [[unlikely]] {
        auto* butterfly = selfButterfly;
        if (type == ArrayWithDouble) {
            for (unsigned i = startIndex; i < newLength; ++i)
                butterfly->contiguousDouble().at(this, i) = PNaN;
        } else {
            for (unsigned i = startIndex; i < newLength; ++i)
                butterfly->contiguousInt32().at(this, i).setWithoutWriteBarrier(JSValue());
        }
    } else if (type == ArrayWithDouble) {
        auto data = selfButterfly->contiguousDouble().data();
        unsigned index = startIndex;
        for (EncodedJSValue encodedDouble : values)
            data[index++] = JSValue::decode(encodedDouble).asNumber();
    } else if (type == ArrayWithInt32)
        memcpy(selfButterfly->contiguous().data() + startIndex, std::bit_cast<const WriteBarrier<Unknown>*>(values.data()), sizeof(JSValue) * values.size());
    else {
        gcSafeMemcpy(selfButterfly->contiguous().data() + startIndex, std::bit_cast<const WriteBarrier<Unknown>*>(values.data()), sizeof(JSValue) * values.size());
        vm.writeBarrier(this);
    }

    return true;
}

bool JSArray::appendMemcpy(JSGlobalObject* globalObject, VM& vm, unsigned startIndex, JSC::JSArray* otherArray)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!canFastAppend(otherArray))
        return false;

#if USE(JSVALUE64)
    // Review round 4 (blocker fix): this fast path memcpy-reads otherArray's
    // flat payload and memcpy-writes our own IN PLACE, with no flag-on regime
    // dispatch - canFastAppend only excludes ArrayStorage shapes. Flag-on:
    //   - a SEGMENTED word on either side would be garbage-decoded through the
    //     flat accessors (the spine pointer read as a Butterfly*);
    //   - in-place destination stores are only sound for the word's EXCLUSIVE
    //     owner (currentTID, SW=0): a foreign SW=0 write needs the F1 fire
    //     (I12) and an SW=1 in-place memcpy would overwrite racing foreign
    //     stores with no CAS detection (I21/I27).
    // Bail to the caller's generic append path otherwise. The destination is
    // re-probed after ensureLength below (it can leave THIS array segmented).
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t selfWord = taggedButterflyWord();
        uint64_t otherWord = otherArray->taggedButterflyWord();
        if (isSegmentedButterfly(selfWord) || isSegmentedButterfly(otherWord))
            return false;
        if ((selfWord & butterflyPointerMask)
            && (butterflySharedWrite(selfWord) || butterflyWriterIsForeign(selfWord))) // incl. §9.6 forceButterflySWBit
            return false;
    }
#endif

    IndexingType type = indexingType();
    IndexingType otherType = otherArray->indexingType();
    bool allowPromotion = false;
    IndexingType copyType = mergeIndexingTypeForCopying(otherType, allowPromotion);
    if (type == ArrayWithUndecided && copyType != NonArray) {
        if (copyType == ArrayWithInt32)
            convertUndecidedToInt32(vm);
        else if (copyType == ArrayWithDouble)
            convertUndecidedToDouble(vm);
        else if (copyType == ArrayWithContiguous)
            convertUndecidedToContiguous(vm);
        else {
            ASSERT(copyType == ArrayWithUndecided);
            return true;
        }
    } else if (type != copyType)
        return false;

    unsigned otherLength = otherArray->length();
    CheckedUint32 checkedNewLength = startIndex;
    checkedNewLength += otherLength;

    if (checkedNewLength.hasOverflowed()) {
        throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
        return false;
    }
    unsigned newLength = checkedNewLength;

    if (newLength >= MIN_SPARSE_ARRAY_INDEX)
        return false;

    if (!ensureLength(vm, newLength)) {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    ASSERT(copyType == indexingType());

    // Review round 4 (blocker fix): flag-on, derive BOTH flat views from
    // single loaded words (never the flat-only butterfly() accessor), and
    // re-probe the destination AFTER ensureLength - its flag-on driver
    // (ensureLengthSlowConcurrent / forceSegmentedButterflies) can legally
    // leave THIS array segmented, and a racing foreign conversion can land at
    // any time once the TTL sets are fired. The source read is bounded by the
    // SAME snapshot's vectorLength (a racing grow republishes fresh storage;
    // the stale snapshot stays readable - conservative scan, I7 - but only up
    // to its own bound). Bailing here is safe: ensureLength only grew
    // storage/publicLength, and the caller's generic append stores every
    // element it still owes.
    Butterfly* selfButterfly;
    Butterfly* otherButterfly = nullptr;
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t selfWord = taggedButterflyWord();
        if (isSegmentedButterfly(selfWord)
            || !(selfWord & butterflyPointerMask)
            || butterflySharedWrite(selfWord) || butterflyWriterIsForeign(selfWord))
            return false; // Destination left the exclusive-owner flat regime mid-call.
        selfButterfly = untaggedButterfly(selfWord);
        if (newLength > selfButterfly->vectorLength()) [[unlikely]]
            return false; // Defensive: never write past this snapshot's storage.
        if (otherType != ArrayWithUndecided) {
            uint64_t otherWord = otherArray->taggedButterflyWord();
            if (isSegmentedButterfly(otherWord) || !(otherWord & butterflyPointerMask))
                return false;
            otherButterfly = untaggedButterfly(otherWord);
            if (otherLength > otherButterfly->vectorLength()) [[unlikely]]
                return false; // Snapshot bound: a racing grow republished bigger storage.
        }
    } else {
        selfButterfly = this->butterfly();
        otherButterfly = otherArray->butterfly();
    }
#else
    selfButterfly = this->butterfly();
    otherButterfly = otherArray->butterfly();
#endif

    if (otherType == ArrayWithUndecided) [[unlikely]] {
        auto* butterfly = selfButterfly;
        if (type == ArrayWithDouble) {
            for (unsigned i = startIndex; i < newLength; ++i)
                butterfly->contiguousDouble().at(this, i) = PNaN;
        } else {
            for (unsigned i = startIndex; i < newLength; ++i)
                butterfly->contiguousInt32().at(this, i).setWithoutWriteBarrier(JSValue());
        }
    } else if (type == ArrayWithDouble) {
        // Double array storage do not need to be safe against GC since they are not scanned.
        memcpy(selfButterfly->contiguousDouble().data() + startIndex, otherButterfly->contiguousDouble().data(), sizeof(double) * otherLength);
    } else if (type == ArrayWithInt32)
        memcpy(selfButterfly->contiguous().data() + startIndex, otherButterfly->contiguous().data(), sizeof(JSValue) * otherLength);
    else {
        gcSafeMemcpy(selfButterfly->contiguous().data() + startIndex, otherButterfly->contiguous().data(), sizeof(JSValue) * otherLength);
        vm.writeBarrier(this);
    }

    return true;
}

bool JSArray::setLength(JSGlobalObject* globalObject, unsigned newLength, bool throwException)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(JSVALUE64)
    // SPEC-objectmodel Task 8: segmented words cannot take the flat paths
    // below (butterfly() is flat-only). Sparse/overlong lengths go to
    // ArrayStorage (the conversion materializes flat under a §4.6 stop);
    // growth goes through ensureLength (T2); truncation through the segmented
    // branch of the reallocateAndShrinkButterfly driver (publicLength is the
    // shared fragment-0 slot, C4).
    if (mayBeSegmentedButterfly()) [[unlikely]] {
        if (newLength > MAX_STORAGE_VECTOR_LENGTH || newLength >= MIN_SPARSE_ARRAY_INDEX) {
            RELEASE_AND_RETURN(scope, setLengthWithArrayStorage(
                globalObject, newLength, throwException, ensureArrayStorage(vm)));
        }
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word)) {
            uint32_t publicLength = segmentedPublicLength(butterflySpine(word));
            if (newLength == publicLength)
                return true;
            if (newLength > publicLength) {
                if (!ensureLength(vm, newLength)) {
                    throwOutOfMemoryError(globalObject, scope);
                    return false;
                }
                return true;
            }
            reallocateAndShrinkButterfly(vm, newLength); // Flag-on: the segmented truncation driver.
            return true;
        }
        // The probe raced a publication and the word is no longer segmented:
        // fall through to the flat paths on the fresh state.
    }

    // Review round 3 (§3/I12): the flat truncation below clears slots and
    // plain-stores publicLength - butterfly WRITES. A FOREIGN caller on an
    // SW=0 flat word must fire F1 and flip SW first (otherwise the writes land
    // while writeThreadLocal(S) is still valid - unsounding E2/E4 elision -
    // and an owner T1 copying resize whose (t,0) CAS still succeeds silently
    // drops them, I21). After the flip the in-place truncation is the §3
    // "owner or SW=1" store form; the §4.5 GC visit bounds shared flat words
    // by vectorLength, so a dense store racing the truncation can never hide a
    // live element from the marker. (This also covers the AS and CoW cases
    // via ensureSharedWriteBit's §4.6/§4.8 carve-outs, BEFORE the stale
    // butterfly/indexingMode reads below.)
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if ((word & butterflyPointerMask) && !isSegmentedButterfly(word)
            && !butterflySharedWrite(word) && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, this);
            // Re-dispatch on the settled state (the carve-outs may have gone
            // segmented / materialized; never foreign-SW=0 again, so this
            // recursion is bounded).
            RELEASE_AND_RETURN(scope, setLength(globalObject, newLength, throwException));
        }
    }
#endif

    // Review round 4 (TOCTOU blocker fix): single-snapshot dispatch - a
    // foreign §4.2 conversion needs no stop once the TTL sets are fired, so a
    // butterfly() re-load after the guards above could decode a spine as flat.
    // See JSArray::pop for the full rationale (incl. why post-snapshot
    // conversions keep the in-place writes below sound: the spine aliases this
    // flat memory).
#if USE(JSVALUE64)
    Butterfly* butterfly;
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t snapshotWord = taggedButterflyWord();
        if (isSegmentedButterfly(snapshotWord)) [[unlikely]]
            RELEASE_AND_RETURN(scope, setLength(globalObject, newLength, throwException)); // RESTART the full dispatch.
        butterfly = untaggedButterfly(snapshotWord);
    } else
        butterfly = this->butterfly();
#else
    Butterfly* butterfly = this->butterfly();
#endif
    switch (indexingMode()) {
    case ArrayClass:
        if (!newLength)
            return true;
        if (newLength >= MIN_SPARSE_ARRAY_INDEX) {
            RELEASE_AND_RETURN(scope, setLengthWithArrayStorage(
                globalObject, newLength, throwException,
                ensureArrayStorage(vm)));
        }
        if (!createInitialUndecided(vm, newLength)) {
#if USE(JSVALUE64)
            // Review round 2 (N3): flag-on, a racing first install won (or the
            // publication went segmented); re-dispatch on the settled state.
            // Indexing types only move away from blank, so this recursion
            // terminates.
            ASSERT(Options::useJSThreads());
            RELEASE_AND_RETURN(scope, setLength(globalObject, newLength, throwException));
#endif
        }
        return true;

    case CopyOnWriteArrayWithInt32:
    case CopyOnWriteArrayWithDouble:
    case CopyOnWriteArrayWithContiguous:
        if (newLength == butterfly->publicLength())
            return true;
        convertFromCopyOnWrite(vm);
#if USE(JSVALUE64)
        // Round 4: flag-on the materialization may have been won by a foreign
        // thread and the word may already have moved on (even segmented) -
        // a plain butterfly() re-load is the TOCTOU above all over again.
        // RESTART the full dispatch on the settled regime (CoW -> non-CoW is
        // monotone, so this recursion terminates).
        if (Options::useJSThreads()) [[unlikely]]
            RELEASE_AND_RETURN(scope, setLength(globalObject, newLength, throwException));
#endif
        butterfly = this->butterfly();
        [[fallthrough]];

    case ArrayWithUndecided:
    case ArrayWithInt32:
    case ArrayWithDouble:
    case ArrayWithContiguous: {
        if (newLength == butterfly->publicLength())
            return true;
        if (newLength > MAX_STORAGE_VECTOR_LENGTH // This check ensures that we can do fast push.
            || (newLength >= MIN_SPARSE_ARRAY_INDEX
                && !isDenseEnoughForVector(newLength, countElements()))) {
            RELEASE_AND_RETURN(scope, setLengthWithArrayStorage(
                globalObject, newLength, throwException,
                ensureArrayStorage(vm)));
        }
        if (newLength > butterfly->publicLength()) {
            if (!ensureLength(vm, newLength)) {
                throwOutOfMemoryError(globalObject, scope);
                return false;
            }
            return true;
        }

        unsigned lengthToClear = butterfly->publicLength() - newLength;
        unsigned costToAllocateNewButterfly = 64; // a heuristic.
        if (lengthToClear > newLength && lengthToClear > costToAllocateNewButterfly) {
            reallocateAndShrinkButterfly(vm, newLength);
            return true;
        }

        unsigned clearFrom = butterfly->publicLength();
#if USE(JSVALUE64)
        // Round 4: bound the clear loop by THIS snapshot's vectorLength - on a
        // flat word that a racing conversion + T2 grow superseded, the aliased
        // publicLength slot can race past the snapshot's storage (see pop).
        if (Options::useJSThreads()) [[unlikely]]
            clearFrom = std::min(clearFrom, butterfly->vectorLength());
#endif
        if (indexingType() == ArrayWithDouble) {
            for (unsigned i = clearFrom; i-- > newLength;)
                butterfly->contiguousDouble().at(this, i) = PNaN;
        } else {
            for (unsigned i = clearFrom; i-- > newLength;)
                butterfly->contiguous().at(this, i).clear();
        }
        butterfly->setPublicLength(newLength);
        return true;
    }
        
    case ArrayWithArrayStorage:
    case ArrayWithSlowPutArrayStorage:
        RELEASE_AND_RETURN(scope, setLengthWithArrayStorage(globalObject, newLength, throwException, arrayStorage()));
        
    default:
        CRASH();
        return false;
    }
}

JSValue JSArray::pop(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ensureWritable(vm);

#if USE(JSVALUE64)
    // SPEC-objectmodel Task 8: segmented words cannot take the flat paths
    // below (butterfly() is flat-only). Dense segmented pops go through the
    // §9.5 accessors; holes fall to the generic get/delete/setLength protocol
    // (setLength is segmented-safe since Task 8).
    if (mayBeSegmentedButterfly()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word)) {
            ButterflySpine* spine = butterflySpine(word);
            uint32_t length = segmentedPublicLength(spine);
            if (!length)
                return jsUndefined();
            unsigned index = length - 1;
            JSValue element = getIndexConcurrent(index);
            if (element) {
                // Clear the slot (shape-keyed: Double = raw PNaN hole, §4.7;
                // bound by the SAME loaded spine, C4/I33) and truncate the
                // shared publicLength slot. Review round 3: a dense store
                // racing this truncation is a program-level shrink-vs-grow
                // race; it stays GC-visible because the §4.5 segmented visit
                // value-bounds elements by the spine's vectorLength (storage
                // bound), not publicLength.
                if (WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(spine, index)) {
                    if (hasDouble(indexingType()))
                        *std::bit_cast<double*>(slot) = PNaN;
                    else
                        slot->clear();
                }
                setSegmentedPublicLength(spine, index);
                return element;
            }
            JSValue slowElement = get(globalObject, index);
            RETURN_IF_EXCEPTION(scope, JSValue());
            bool success = deletePropertyByIndex(this, globalObject, index);
            RETURN_IF_EXCEPTION(scope, JSValue());
            if (!success) {
                throwTypeError(globalObject, scope, UnableToDeletePropertyError);
                return jsUndefined();
            }
            scope.release();
            setLength(globalObject, index, true);
            return slowElement;
        }
        // The word settled to flat between the probe and the load: fall through.
    }

    // Review round 3 (§3/I12): the flat pop branches below clear a slot and
    // plain-store publicLength - butterfly WRITES. A FOREIGN caller on an SW=0
    // flat word must fire F1 and flip SW first (I12/I21: otherwise an owner T1
    // copying resize can silently drop the pop's writes). After the flip the
    // in-place pop is the §3 "owner or SW=1" store form; the §4.5 GC visit
    // bounds shared flat words by vectorLength, so a dense store racing the
    // truncation cannot hide a live element from the marker. Re-dispatch after
    // the flip (the carve-outs may have gone segmented; never foreign-SW=0
    // again, so the recursion is bounded).
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if ((word & butterflyPointerMask) && !isSegmentedButterfly(word)
            && !butterflySharedWrite(word) && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, this);
            RELEASE_AND_RETURN(scope, pop(globalObject));
        }
    }
#endif

    // Review round 4 (TOCTOU blocker fix): once a shape family's TTL sets are
    // fired, a foreign flat->segmented conversion (§4.2) needs only the cell
    // lock + DCAS - NO stop - so it can land between the guards above and a
    // butterfly() re-load, whose flat-only decode would then read the
    // ButterflySpine* payload as a flat Butterfly* (publicLength at spine-8).
    // Flat fast paths therefore derive their Butterfly* from ONE loaded word
    // (single-snapshot dispatch, like the *Concurrent accessors); a segmented
    // word here RESTARTs the full dispatch. Post-snapshot conversions stay
    // sound for the in-place writes below: the §4.2 spine aliases this flat
    // allocation's memory, so stores through the stale flat view land in the
    // live fragments.
#if USE(JSVALUE64)
    Butterfly* butterfly;
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t snapshotWord = taggedButterflyWord();
        if (isSegmentedButterfly(snapshotWord)) [[unlikely]]
            RELEASE_AND_RETURN(scope, pop(globalObject)); // RESTART the full dispatch (segmented branch above).
        butterfly = untaggedButterfly(snapshotWord);
    } else
        butterfly = this->butterfly();
#else
    Butterfly* butterfly = this->butterfly();
#endif

    switch (indexingType()) {
    case ArrayClass:
        return jsUndefined();

    case ArrayWithUndecided:
        if (!butterfly->publicLength())
            return jsUndefined();
        // We have nothing but holes. So, drop down to the slow version.
        break;

    case ArrayWithInt32:
    case ArrayWithContiguous: {
        unsigned length = butterfly->publicLength();

        if (!length--)
            return jsUndefined();

#if USE(JSVALUE64)
        // Round 4: on a flat word that a racing conversion + T2 grow
        // superseded, the ALIASED publicLength slot can race past this
        // snapshot's storage - bail to the generic path instead of asserting
        // (no crashes from races).
        if (Options::useJSThreads() && length >= butterfly->vectorLength()) [[unlikely]]
            break;
#endif
        RELEASE_ASSERT(length < butterfly->vectorLength());
        JSValue value = butterfly->contiguous().at(this, length).get();
        if (value) {
            butterfly->contiguous().at(this, length).clear();
            butterfly->setPublicLength(length);
            return value;
        }
        break;
    }

    case ArrayWithDouble: {
        unsigned length = butterfly->publicLength();

        if (!length--)
            return jsUndefined();

#if USE(JSVALUE64)
        if (Options::useJSThreads() && length >= butterfly->vectorLength()) [[unlikely]] // round 4: aliased publicLength can race past this snapshot
            break;
#endif
        RELEASE_ASSERT(length < butterfly->vectorLength());
        double value = butterfly->contiguousDouble().at(this, length);
        if (value == value) {
            butterfly->contiguousDouble().at(this, length) = PNaN;
            butterfly->setPublicLength(length);
            return JSValue(JSValue::EncodeAsDouble, value);
        }
        break;
    }
        
    case ARRAY_WITH_ARRAY_STORAGE_INDEXING_TYPES: {
#if USE(JSVALUE64)
        // SPEC-objectmodel I31/L5 (Task 8): flag-on, every runtime AS access
        // is cell-locked; re-read the butterfly under the lock (AS-COPY
        // republishes). In-place element/length/m_numValuesInVector stores in
        // the installed AS are legal under the lock (§4.6). The lock drops at
        // the case-block exit (before the generic slow path below) and before
        // any throw (allocation - O1).
        std::optional<Locker<JSCellLock>> threadsLocker;
        if (Options::useJSThreads()) [[unlikely]] {
            threadsLocker.emplace(cellLock());
            butterfly = this->butterfly();
        }
#endif
        ArrayStorage* storage = butterfly->arrayStorage();

        unsigned length = storage->length();
        if (!length) {
#if USE(JSVALUE64)
            threadsLocker.reset();
#endif
            if (!isLengthWritable())
                throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
            return jsUndefined();
        }

        unsigned index = length - 1;
        if (index < storage->vectorLength()) {
            WriteBarrier<Unknown>& valueSlot = storage->m_vector[index];
            if (valueSlot) {
                --storage->m_numValuesInVector;
                JSValue element = valueSlot.get();
                valueSlot.clear();
            
                RELEASE_ASSERT(isLengthWritable());
                storage->setLength(index);
                return element;
            }
        }
        break;
    }
        
    default:
        CRASH();
        return JSValue();
    }
    
    unsigned index = getArrayLength() - 1;
    // Let element be the result of calling the [[Get]] internal method of O with argument indx.
    JSValue element = get(globalObject, index);
    RETURN_IF_EXCEPTION(scope, JSValue());
    // Call the [[Delete]] internal method of O with arguments indx and true.
    bool success = deletePropertyByIndex(this, globalObject, index);
    RETURN_IF_EXCEPTION(scope, JSValue());
    if (!success) {
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
        return jsUndefined();
    }
    // Call the [[Put]] internal method of O with arguments "length", indx, and true.
    scope.release();
    setLength(globalObject, index, true);
    // Return element.
    return element;
}

JSValue JSArray::fastShift(VM& vm)
{
    ensureWritable(vm);

#if USE(JSVALUE64)
    // Review round 3: the in-place memmove + slot clear + plain
    // setPublicLength below are owner-(currentTID, 0)-only mutations (§3/I27 -
    // moving shared element storage in place outside any lock or stop loses
    // racing foreign stores with no failure detection, and a segmented word
    // would garbage-decode through the flat accessors). Shared, foreign, or
    // segmented words bail to the caller's generic shift path ({} = the
    // existing not-fast sentinel).
    Butterfly* butterfly;
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word)
            || ((word & butterflyPointerMask)
                && (butterflySharedWrite(word) || butterflyWriterIsForeign(word))))
            return { };
        // Review round 4 (TOCTOU blocker fix): derive the flat view from the
        // SAME loaded word - a foreign §4.2 conversion needs no stop once the
        // TTL sets are fired, so a butterfly() re-load could decode a spine as
        // flat. See JSArray::pop for the full single-snapshot rationale.
        butterfly = untaggedButterfly(word);
    } else
        butterfly = this->butterfly();
#else
    Butterfly* butterfly = this->butterfly();
#endif

    auto indexingType = this->indexingType();

    constexpr unsigned shiftThreshold = 128;

    switch (indexingType) {
    case ArrayClass:
        return jsUndefined();

    case ArrayWithInt32:
    case ArrayWithContiguous: {
        unsigned length = butterfly->publicLength();

        if (!length)
            return jsUndefined();

        if (length > shiftThreshold) [[unlikely]]
            return { };

#if USE(JSVALUE64)
        if (Options::useJSThreads() && length > butterfly->vectorLength()) [[unlikely]]
            return { }; // round 4: aliased publicLength raced past this snapshot's storage (see pop).
#endif
        JSValue result = butterfly->contiguous().at(this, 0).get();
        if (!result)
            return { };

        unsigned moveCount = length - 1;
        if (moveCount) {
            if (holesMustForwardToPrototype()) [[unlikely]]
                return { };
            if (indexingType == ArrayWithInt32)
                memmove(butterfly->contiguous().data(), butterfly->contiguous().data() + 1, sizeof(JSValue) * moveCount);
            else
                gcSafeMemmove(butterfly->contiguous().data(), butterfly->contiguous().data() + 1, sizeof(JSValue) * moveCount);
        }
        butterfly->contiguous().at(this, moveCount).clear();
        butterfly->setPublicLength(moveCount);
        if (indexingType == ArrayWithContiguous)
            vm.writeBarrier(this);
        return result;
    }

    case ArrayWithDouble: {
        unsigned length = butterfly->publicLength();

        if (!length)
            return jsUndefined();

        if (length > shiftThreshold) [[unlikely]]
            return { };

#if USE(JSVALUE64)
        if (Options::useJSThreads() && length > butterfly->vectorLength()) [[unlikely]]
            return { }; // round 4: aliased publicLength raced past this snapshot's storage.
#endif
        double result = butterfly->contiguousDouble().at(this, 0);
        if (result != result)
            return { };

        unsigned moveCount = length - 1;
        if (moveCount) {
            if (holesMustForwardToPrototype()) [[unlikely]]
                return { };
            memmove(butterfly->contiguousDouble().data(), butterfly->contiguousDouble().data() + 1, sizeof(double) * moveCount);
        }
        butterfly->contiguousDouble().at(this, moveCount) = PNaN;
        butterfly->setPublicLength(moveCount);
        return JSValue(JSValue::EncodeAsDouble, result);
    }

    default:
        return { };
    }
}

// Push & putIndex are almost identical, with two small differences.
//  - we always are writing beyond the current array bounds, so it is always necessary to update m_length & m_numValuesInVector.
//  - pushing to an array of length 2^32-1 stores the property, but throws a range error.
NEVER_INLINE void JSArray::push(JSGlobalObject* globalObject, JSValue value)
{
    pushInline(globalObject, value);
}

JSArray* JSArray::fastSlice(JSGlobalObject* globalObject, JSObject* source, uint64_t startIndex, uint64_t count)
{
    VM& vm = globalObject->vm();

    Structure* sourceStructure = source->structure();
    if (sourceStructure->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero()) {
        // We do not need to have ClonedArgumentsType here since it does not have interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero.
        switch (source->type()) {
        case DirectArgumentsType:
            return DirectArguments::fastSlice(globalObject, uncheckedDowncast<DirectArguments>(source), startIndex, count);
        case ScopedArgumentsType:
            return ScopedArguments::fastSlice(globalObject, uncheckedDowncast<ScopedArguments>(source), startIndex, count);
        default:
            return nullptr;
        }
        return nullptr;
    }

#if USE(JSVALUE64)
    // Review round 4 (blocker fix): this fast path memcpy-reads the source's
    // flat payload with no flag-on regime dispatch. Flag-on: a SEGMENTED
    // source word would be garbage-decoded through the flat accessors (the
    // spine pointer read as a Butterfly*; vectorLength at spine-4), and the
    // ArrayStorage leg below reads AS innards that are cell-locked-only (I31).
    // Bail to the caller's generic slice path for both; for flat words, every
    // source deref below goes through ONE loaded word (single-snapshot - a
    // butterfly() re-load after the result allocation could decode a
    // mid-call conversion's spine as flat), bounded by that snapshot's own
    // vectorLength (the existing startIndex + count check). The stale
    // snapshot stays readable across the allocation: the local pointer pins
    // it for the conservative scan (I7), and a racing grow never mutates it
    // in place (flat vectorLengths are immutable flag-on).
    uint64_t sourceWord = 0;
    if (Options::useJSThreads()) [[unlikely]] {
        sourceWord = source->taggedButterflyWord();
        if (isSegmentedButterfly(sourceWord))
            return nullptr;
        if (hasAnyArrayStorage(source->indexingType()))
            return nullptr; // I31: AS reads are cell-locked; generic path.
    }
#endif

    auto arrayType = source->indexingType() | IsArray;
    switch (arrayType) {
    case ArrayWithDouble:
    case ArrayWithInt32:
    case ArrayWithContiguous: {
        if (count >= MIN_SPARSE_ARRAY_INDEX || sourceStructure->holesMustForwardToPrototype(source))
            return nullptr;

        Butterfly* sourceButterfly;
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            sourceButterfly = untaggedButterfly(sourceWord);
            if (!sourceButterfly) [[unlikely]]
                return nullptr; // E5 None-first: racing N3 first install (round 4).
        } else
            sourceButterfly = source->butterfly();
#else
        sourceButterfly = source->butterfly();
#endif

        if (startIndex + count > sourceButterfly->vectorLength())
            return nullptr;

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(arrayType);
        IndexingType indexingType = resultStructure->indexingType();
        if (hasAnyArrayStorage(indexingType)) [[unlikely]]
            return nullptr;

        if (isCopyOnWrite(source->indexingMode())) {
            if (!startIndex && count == sourceButterfly->publicLength())
                return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(source->indexingMode()), sourceButterfly);
        }

        ASSERT(!globalObject->isHavingABadTime());
        if (count > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
            return nullptr;

        ASSERT(!resultStructure->outOfLineCapacity()); // JSArray's initial Structure should not have any properties.
        uint32_t initialLength = static_cast<uint32_t>(count);
        unsigned vectorLength = Butterfly::optimalContiguousVectorLength(resultStructure, initialLength);
        void* memory = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
        if (!memory) [[unlikely]]
            return nullptr;

        auto* butterfly = Butterfly::fromBase(memory, 0, 0);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(initialLength);
        // We initialize Butterfly first before setting it to JSArray. In that case, butterfly is not scannoed so that we can safely use memcpy here.
        // Round 4: the source read uses the pre-allocation SNAPSHOT (never a
        // butterfly() re-load), bounded by the snapshot's vectorLength above.
        memcpy(butterfly->contiguous().data(), sourceButterfly->contiguous().data() + startIndex, sizeof(JSValue) * initialLength);

        Butterfly::clearRange(indexingType, butterfly, initialLength, vectorLength);
        return createWithButterfly(vm, nullptr, resultStructure, butterfly);
    }
    case ArrayWithArrayStorage: {
        // Flag-on this leg is unreachable (the round-4 AS guard above bailed);
        // flag-off it is byte-for-byte today's code (I22).
        if (count >= MIN_SPARSE_ARRAY_INDEX || sourceStructure->holesMustForwardToPrototype(source))
            return nullptr;

        if (startIndex + count > source->butterfly()->arrayStorage()->vectorLength())
            return nullptr;

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
        if (hasAnyArrayStorage(resultStructure->indexingType())) [[unlikely]]
            return nullptr;

        ASSERT(!globalObject->isHavingABadTime());
        ObjectInitializationScope scope(vm);
        JSArray* resultArray = JSArray::tryCreateUninitializedRestricted(scope, resultStructure, static_cast<uint32_t>(count));
        if (!resultArray) [[unlikely]]
            return nullptr;
        gcSafeMemcpy(resultArray->butterfly()->contiguous().data(), source->butterfly()->arrayStorage()->m_vector + startIndex, sizeof(JSValue) * static_cast<uint32_t>(count));
        ASSERT(resultArray->butterfly()->publicLength() == count);
        return resultArray;
    }
    case ArrayWithUndecided: {
        if (count)
            return nullptr;
        return constructEmptyArray(globalObject, nullptr);
    }
    default:
        return nullptr;
    }
}

#if USE(JSVALUE64)
// SPEC-objectmodel §4.6 AS-COPY (Task 8; GT10's JSArray.cpp:1650 site, I31):
// flag-on, the whole shift runs under the cell lock (every AS access is
// cell-locked, reads included - L5), AS innards are NEVER relaid out in place
// (neither the indexBias-bumping Butterfly::shift() nor the in-place
// element-compaction branch of the flag-off path), and the relayout allocates
// a fresh AS butterfly published with casButterfly, tag (including SW)
// preserved verbatim (T3/I17). In-place scalar stores (length /
// m_numValuesInVector) in the installed AS stay legal under the lock (§4.6).
bool JSArray::shiftCountWithArrayStorageConcurrent(VM& vm, unsigned startIndex, unsigned count)
{
    ASSERT(Options::useJSThreads());
    // Review round 3 (§4.6/I12): a shift is a WRITE; the first FOREIGN write
    // to an SW=0 AS word runs the per-event SW stop (fires writeThreadLocal,
    // publishes (installerTID, 1)) BEFORE any lock (veneer caller contract
    // GT11; I10b). Without it, writeThreadLocal(S) stays valid while our
    // locked writes land, and an owner E4 transition could copy the AS
    // payload lock-free against them (now also excluded structurally by the
    // E4 AS-shape exclusion - belt and braces).
    {
        uint64_t probeWord = taggedButterflyWord();
        if ((probeWord & butterflyPointerMask) && !butterflySharedWrite(probeWord)
            && butterflyWriterIsForeign(probeWord)) // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, this);
    }
    DeferGC deferGC(vm); // O1's sanctioned pre-lock back-edge: the fresh AS allocation happens under the cell lock.
    Locker locker { cellLock() }; // I31/L5

    uint64_t word = taggedButterflyWord();
    RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31: AS never segments.
    Butterfly* butterfly = untaggedButterfly(word);
    ArrayStorage* storage = butterfly->arrayStorage();

    unsigned oldLength = storage->length();
    RELEASE_ASSERT(count <= oldLength);

    // Abnormal states use the generic algorithm in ArrayPrototype (its sparse
    // map edits are locked at their own L3 sites).
    if (storage->hasHoles() || storage->m_sparseMap || shouldUseSlowPut(indexingType()))
        return false;

    if (!oldLength)
        return true;

    unsigned length = oldLength - count;

    // In-place scalar updates: legal under the lock (§4.6).
    storage->m_numValuesInVector -= count;
    storage->setLength(length);

    unsigned vectorLength = storage->vectorLength();
    if (!vectorLength)
        return true;
    if (startIndex >= vectorLength)
        return true;

    if (startIndex + count > vectorLength)
        count = vectorLength - startIndex;

    unsigned usedVectorLength = std::min(vectorLength, oldLength);
    ASSERT(startIndex + count <= usedVectorLength);

    // Fresh AS butterfly: same capacity, indexBias 0, elements compacted with
    // [startIndex, startIndex + count) removed. The new butterfly is invisible
    // to GC markers until publication, so plain memcpy is safe.
    unsigned propertyCapacity = structure()->outOfLineCapacity();
    Butterfly* newButterfly = Butterfly::tryCreateUninitialized(vm, this, 0, propertyCapacity, true, ArrayStorage::sizeFor(vectorLength));
    if (!newButterfly) [[unlikely]]
        return false; // OOM: the generic path makes its own attempt.
    butterflyConcurrentCopyWords(newButterfly->propertyStorage() - propertyCapacity, butterfly->propertyStorage() - propertyCapacity,
        propertyCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader) + ArrayStorage::sizeFor(0)); // TSAN-visible word copy (recycled-address pairing).
    ArrayStorage* newStorage = newButterfly->arrayStorage();
    newStorage->m_indexBias = 0;
    newStorage->setVectorLength(vectorLength);
    butterflyConcurrentCopyWords(newStorage->m_vector, storage->m_vector, startIndex * sizeof(JSValue));
    butterflyConcurrentCopyWords(newStorage->m_vector + startIndex, storage->m_vector + startIndex + count,
        (usedVectorLength - (startIndex + count)) * sizeof(JSValue));
    for (unsigned i = usedVectorLength - count; i < vectorLength; ++i)
        newStorage->m_vector[i].clear();

    publishArrayStorageButterflyLocked(vm, this, newButterfly); // T3/I17; superseded storage never written again (I7).
    return true;
}
#endif // USE(JSVALUE64)

bool JSArray::shiftCountWithArrayStorage(VM& vm, unsigned startIndex, unsigned count, ArrayStorage* storage)
{
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]]
        return shiftCountWithArrayStorageConcurrent(vm, startIndex, count); // §4.6 AS-COPY; re-reads the storage under the cell lock.
#endif
    unsigned oldLength = storage->length();
    RELEASE_ASSERT(count <= oldLength);
    
    // If the array contains holes or is otherwise in an abnormal state,
    // use the generic algorithm in ArrayPrototype.
    if (storage->hasHoles() 
        || hasSparseMap() 
        || shouldUseSlowPut(indexingType())) {
        return false;
    }

    if (!oldLength)
        return true;
    
    unsigned length = oldLength - count;
    
    storage->m_numValuesInVector -= count;
    storage->setLength(length);
    
    unsigned vectorLength = storage->vectorLength();
    if (!vectorLength)
        return true;
    
    if (startIndex >= vectorLength)
        return true;
    
    AssertNoGC assertNoGC;
    Locker locker { cellLock() };
    
    if (startIndex + count > vectorLength)
        count = vectorLength - startIndex;
    
    unsigned usedVectorLength = std::min(vectorLength, oldLength);
    
    unsigned numElementsBeforeShiftRegion = startIndex;
    unsigned firstIndexAfterShiftRegion = startIndex + count;
    unsigned numElementsAfterShiftRegion = usedVectorLength - firstIndexAfterShiftRegion;
    ASSERT(numElementsBeforeShiftRegion + count + numElementsAfterShiftRegion == usedVectorLength);

    // The point of this comparison seems to be to minimize the amount of elements that have to 
    // be moved during a shift operation.
    if (numElementsBeforeShiftRegion < numElementsAfterShiftRegion) {
        // The number of elements before the shift region is less than the number of elements
        // after the shift region, so we move the elements before to the right.
        if (numElementsBeforeShiftRegion) {
            RELEASE_ASSERT(count + startIndex <= vectorLength);
            gcSafeMemmove(storage->m_vector + count,
                storage->m_vector,
                sizeof(JSValue) * startIndex);
        }
        {
            // When moving Butterfly's head to adjust property-storage, we must take a structure lock.
            // Otherwise, concurrent JIT compiler accesses to a property storage which is half-baked due to move for shift / unshift.
            Structure* structure = this->structure();
            ConcurrentJSLocker structureLock(structure->lock());
            // Adjust the Butterfly and the index bias. We only need to do this here because we're changing
            // the start of the Butterfly, which needs to point at the first indexed property in the used
            // portion of the vector.
            Butterfly* butterfly = this->butterfly()->shift(structure, count);
            storage = butterfly->arrayStorage();
            storage->m_indexBias += count;

            // Since we're consuming part of the vector by moving its beginning to the left,
            // we need to modify the vector length appropriately.
            storage->setVectorLength(vectorLength - count);
            setButterfly(vm, butterfly);
        }
    } else {
        // The number of elements before the shift region is greater than or equal to the number 
        // of elements after the shift region, so we move the elements after the shift region to the left.
        gcSafeMemmove(storage->m_vector + startIndex,
            storage->m_vector + firstIndexAfterShiftRegion,
            sizeof(JSValue) * numElementsAfterShiftRegion);

        // Clear the slots of the elements we just moved.
        unsigned startOfEmptyVectorTail = usedVectorLength - count;
        for (unsigned i = startOfEmptyVectorTail; i < usedVectorLength; ++i)
            storage->m_vector[i].clear();
        // We don't modify the index bias or the Butterfly pointer in this case because we're not changing 
        // the start of the Butterfly, which needs to point at the first indexed property in the used 
        // portion of the vector. We also don't modify the vector length because we're not actually changing
        // its length; we're just using less of it.
    }
    
    return true;
}

bool JSArray::shiftCountWithAnyIndexingType(JSGlobalObject* globalObject, unsigned& startIndex, unsigned count, unsigned shiftThreshold)
{
    VM& vm = globalObject->vm();
    RELEASE_ASSERT(count > 0);

    ensureWritable(vm);

#if USE(JSVALUE64)
    // SPEC-objectmodel Task 8 + review round 3 (I27 carve-out): the flat
    // branches below relocate element storage IN PLACE (gcSafeMemmove + slot
    // clears + plain setPublicLength). That is sound only for the word's
    // exclusive owner - (currentTID, 0): on a segmented word the flat
    // accessors garbage-decode the spine; on an SW=1 word the lock-free move
    // overwrites racing foreign stores with stale moved values (no CAS
    // detects it - unlike T1) and a racing flat->segmented conversion would
    // alias the half-shifted memory; on a foreign SW=0 word the writes land
    // without the F1 fire (I12). All of those route through ArrayStorage: the
    // conversion runs under a §4.6 per-event stop (firing F2, which subsumes
    // F1) and the relayout is cell-locked AS-COPY.
    Butterfly* butterfly;
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word)
            || ((word & butterflyPointerMask)
                && (butterflySharedWrite(word) || butterflyWriterIsForeign(word))))
            return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm));
        // Review round 4 (TOCTOU blocker fix): derive the flat view from the
        // SAME loaded word - a foreign §4.2 conversion needs no stop once the
        // TTL sets are fired, so a butterfly() re-load could decode a spine as
        // flat. See JSArray::pop for the full single-snapshot rationale.
        butterfly = untaggedButterfly(word);
    } else
        butterfly = this->butterfly();
#else
    Butterfly* butterfly = this->butterfly();
#endif

    auto indexingType = this->indexingType();
    switch (indexingType) {
    case ArrayClass:
        return true;

    case ArrayWithUndecided:
        // Don't handle this because it's confusing and it shouldn't come up.
        return false;

    case ArrayWithInt32:
    case ArrayWithContiguous: {
        unsigned oldLength = butterfly->publicLength();
        RELEASE_ASSERT(count <= oldLength);

        // We may have to walk the entire array to do the shift. We're willing to do
        // so only if it's not horribly slow.
        if (oldLength - (startIndex + count) >= MIN_SPARSE_ARRAY_INDEX || oldLength > shiftThreshold)
            return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm));

#if USE(JSVALUE64)
        if (Options::useJSThreads() && oldLength > butterfly->vectorLength()) [[unlikely]]
            return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm)); // round 4: aliased publicLength raced past this snapshot's storage.
#endif

        // Storing to a hole is fine since we're still having a good time. But reading from a hole
        // is totally not fine, since we might have to read from the proto chain.
        // We have to check for holes before we start moving things around so that we don't get halfway
        // through shifting and then realize we should have been in ArrayStorage mode.
        unsigned end = oldLength - count;
        unsigned moveCount = end - startIndex;
        if (moveCount) {
            if (holesMustForwardToPrototype()) [[unlikely]] {
                for (unsigned i = startIndex; i < end; ++i) {
                    JSValue v = butterfly->contiguous().at(this, i + count).get();
                    if (!v) [[unlikely]] {
                        startIndex = i;
                        return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm));
                    }
                    butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
                }
            } else {
                gcSafeMemmove(butterfly->contiguous().data() + startIndex,
                    butterfly->contiguous().data() + startIndex + count,
                    sizeof(JSValue) * moveCount);
            }
        }

        for (unsigned i = end; i < oldLength; ++i)
            butterfly->contiguous().at(this, i).clear();

        butterfly->setPublicLength(oldLength - count);

        // Our memmoving of values around in the array could have concealed some of them from
        // the collector. Let's make sure that the collector scans this object again.
        if (indexingType == ArrayWithContiguous)
            vm.writeBarrier(this);

        return true;
    }
        
    case ArrayWithDouble: {
        unsigned oldLength = butterfly->publicLength();
        RELEASE_ASSERT(count <= oldLength);

        // We may have to walk the entire array to do the shift. We're willing to do
        // so only if it's not horribly slow.
        if (oldLength - (startIndex + count) >= MIN_SPARSE_ARRAY_INDEX || oldLength > shiftThreshold)
            return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm));

#if USE(JSVALUE64)
        if (Options::useJSThreads() && oldLength > butterfly->vectorLength()) [[unlikely]]
            return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm)); // round 4: aliased publicLength raced past this snapshot's storage.
#endif

        // Storing to a hole is fine since we're still having a good time. But reading from a hole 
        // is totally not fine, since we might have to read from the proto chain.
        // We have to check for holes before we start moving things around so that we don't get halfway 
        // through shifting and then realize we should have been in ArrayStorage mode.
        unsigned end = oldLength - count;
        unsigned moveCount = end - startIndex;
        if (moveCount) {
            if (holesMustForwardToPrototype()) [[unlikely]] {
                for (unsigned i = startIndex; i < end; ++i) {
                    double v = butterfly->contiguousDouble().at(this, i + count);
                    if (v != v) [[unlikely]] {
                        startIndex = i;
                        return shiftCountWithArrayStorage(vm, startIndex, count, ensureArrayStorage(vm));
                    }
                    butterfly->contiguousDouble().at(this, i) = v;
                }
            } else {
                gcSafeMemmove(butterfly->contiguousDouble().data() + startIndex,
                    butterfly->contiguousDouble().data() + startIndex + count,
                    sizeof(double) * moveCount);
            }
        }
        for (unsigned i = end; i < oldLength; ++i)
            butterfly->contiguousDouble().at(this, i) = PNaN;
        
        butterfly->setPublicLength(oldLength - count);
        return true;
    }
        
    case ArrayWithArrayStorage:
    case ArrayWithSlowPutArrayStorage:
        return shiftCountWithArrayStorage(vm, startIndex, count, arrayStorage());
        
    default:
        CRASH();
        return false;
    }
}

#if USE(JSVALUE64)
// SPEC-objectmodel §4.6 AS-COPY (Task 8; GT10's JSArray.cpp:1818 site, I31):
// the unshift analogue of shiftCountWithArrayStorageConcurrent. One fresh AS
// butterfly (indexBias 0) is built with the gap [startIndex, startIndex +
// count) already opened and cleared - replacing both the indexBias-consuming
// Butterfly::unshift() head move and the unshiftCountSlowCase relayout - and
// published with casButterfly under the cell lock, tag preserved verbatim
// (T3/I17). Returns true with the gap ready (the caller stores into it),
// false for the generic fallback; OOM throws, like the flag-off path.
bool JSArray::unshiftCountWithArrayStorageConcurrent(JSGlobalObject* globalObject, unsigned startIndex, unsigned count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(Options::useJSThreads());

    // Review round 3 (§4.6/I12): first FOREIGN write to an SW=0 AS word runs
    // the per-event SW stop before any lock - see
    // shiftCountWithArrayStorageConcurrent for the full rationale.
    {
        uint64_t probeWord = taggedButterflyWord();
        if ((probeWord & butterflyPointerMask) && !butterflySharedWrite(probeWord)
            && butterflyWriterIsForeign(probeWord)) // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, this);
    }

    DeferGC deferGC(vm); // O1's sanctioned pre-lock back-edge (cf. the flag-off path's DeferGC before its cell lock).
    bool outOfMemory = false;
    bool handled = false;
    {
        Locker locker { cellLock() }; // I31/L5
        uint64_t word = taggedButterflyWord();
        RELEASE_ASSERT(!isSegmentedButterfly(word)); // I31
        Butterfly* butterfly = untaggedButterfly(word);
        ArrayStorage* storage = butterfly->arrayStorage();

        unsigned length = storage->length();
        RELEASE_ASSERT(startIndex <= length);

        if (!(storage->hasHoles() || storage->inSparseMode() || shouldUseSlowPut(indexingType()))) {
            unsigned vectorLength = storage->vectorLength();
            unsigned usedVectorLength = std::min(vectorLength, length);
            ASSERT(usedVectorLength <= MAX_STORAGE_VECTOR_LENGTH);
            if (count > MAX_STORAGE_VECTOR_LENGTH - usedVectorLength)
                outOfMemory = true;
            else {
                unsigned requiredVectorLength = usedVectorLength + count;
                // Same atomic-decay growth policy as unshiftCountSlowCase.
                unsigned desiredCapacity = std::min(MAX_STORAGE_VECTOR_LENGTH, std::max(BASE_ARRAY_STORAGE_VECTOR_LEN, requiredVectorLength) << 1);
                unsigned propertyCapacity = structure()->outOfLineCapacity();
                Butterfly* newButterfly = Butterfly::tryCreateUninitialized(vm, this, 0, propertyCapacity, true, ArrayStorage::sizeFor(desiredCapacity));
                if (!newButterfly) [[unlikely]]
                    outOfMemory = true;
                else {
                    // Prefix (out-of-line properties + IndexingHeader + AS
                    // header); invisible to GC until publication => memcpy.
                    memcpy(newButterfly->propertyStorage() - propertyCapacity, butterfly->propertyStorage() - propertyCapacity,
                        propertyCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader) + ArrayStorage::sizeFor(0));
                    ArrayStorage* newStorage = newButterfly->arrayStorage();
                    newStorage->m_indexBias = 0;
                    newStorage->setVectorLength(desiredCapacity);
                    memcpy(newStorage->m_vector, storage->m_vector, startIndex * sizeof(JSValue));
                    for (unsigned i = 0; i < count; ++i)
                        newStorage->m_vector[startIndex + i].clear();
                    memcpy(newStorage->m_vector + startIndex + count, storage->m_vector + startIndex,
                        (usedVectorLength - startIndex) * sizeof(JSValue));
                    for (unsigned i = usedVectorLength + count; i < desiredCapacity; ++i)
                        newStorage->m_vector[i].clear();

                    publishArrayStorageButterflyLocked(vm, this, newButterfly); // T3/I17
                    handled = true;
                }
            }
        }
    }
    if (outOfMemory) {
        throwOutOfMemoryError(globalObject, scope); // After unlock (allocation under the cell lock only via the DeferGC exemption; throwing allocates).
        return true;
    }
    return handled;
}
#endif // USE(JSVALUE64)

// Returns true if the unshift can be handled, false to fallback.
bool JSArray::unshiftCountWithArrayStorage(JSGlobalObject* globalObject, unsigned startIndex, unsigned count, ArrayStorage* storage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]]
        RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorageConcurrent(globalObject, startIndex, count)); // §4.6 AS-COPY
#endif

    unsigned length = storage->length();

    RELEASE_ASSERT(startIndex <= length);

    // If the array contains holes or is otherwise in an abnormal state,
    // use the generic algorithm in ArrayPrototype.
    if (storage->hasHoles() || storage->inSparseMode() || shouldUseSlowPut(indexingType()))
        return false;

    bool moveFront = !startIndex || startIndex < length / 2;

    unsigned vectorLength = storage->vectorLength();

    // Need to have GC deferred around the unshiftCountSlowCase(), since that leaves the butterfly in
    // a weird state: some parts of it will be left uninitialized, which we will fill in here.
    DeferGC deferGC(vm);
    Locker locker { cellLock() };
    
    if (moveFront && storage->m_indexBias >= count) {
        // When moving Butterfly's head to adjust property-storage, we must take a structure lock.
        // Otherwise, concurrent JIT compiler accesses to a property storage which is half-baked due to move for shift / unshift.
        Structure* structure = this->structure();
        ConcurrentJSLocker structureLock(structure->lock());
        Butterfly* newButterfly = storage->butterfly()->unshift(structure, count);

        storage = newButterfly->arrayStorage();
        storage->m_indexBias -= count;
        storage->setVectorLength(vectorLength + count);
        setButterfly(vm, newButterfly);
    } else if (!moveFront && vectorLength - length >= count)
        storage = storage->butterfly()->arrayStorage();
    else if (unshiftCountSlowCase(locker, vm, deferGC, moveFront, count))
        storage = arrayStorage();
    else {
        throwOutOfMemoryError(globalObject, scope);
        return true;
    }

    WriteBarrier<Unknown>* vector = storage->m_vector;

    if (startIndex) {
        if (moveFront)
            gcSafeMemmove(vector, vector + count, startIndex * sizeof(JSValue));
        else if (length - startIndex)
            gcSafeMemmove(vector + startIndex + count, vector + startIndex, (length - startIndex) * sizeof(JSValue));
    }

    for (unsigned i = 0; i < count; i++)
        vector[i + startIndex].clear();
    
    return true;
}

bool JSArray::unshiftCountWithAnyIndexingType(JSGlobalObject* globalObject, unsigned startIndex, unsigned count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ensureWritable(vm);

#if USE(JSVALUE64)
    // SPEC-objectmodel Task 8 + review round 3: as in
    // shiftCountWithAnyIndexingType - segmented, SW=1, and foreign words all
    // route through the ArrayStorage path (§4.6 stop-routed conversion +
    // cell-locked AS-COPY); the flat in-place gcSafeMemmove below is
    // owner-(currentTID, 0)-only (I27/I12).
    Butterfly* butterfly;
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word)
            || ((word & butterflyPointerMask)
                && (butterflySharedWrite(word) || butterflyWriterIsForeign(word))))
            RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));
        // Review round 4 (TOCTOU blocker fix): derive the flat view from the
        // SAME loaded word - see JSArray::pop for the single-snapshot
        // rationale (a foreign §4.2 conversion needs no stop once the TTL
        // sets are fired, so a butterfly() re-load could decode a spine as
        // flat).
        butterfly = untaggedButterfly(word);
    } else
        butterfly = this->butterfly();
#else
    Butterfly* butterfly = this->butterfly();
#endif

    switch (indexingType()) {
    case ArrayClass:
    case ArrayWithUndecided:
        // We could handle this. But it shouldn't ever come up, so we won't.
        return false;

    case ArrayWithInt32:
    case ArrayWithContiguous: {
        unsigned oldLength = butterfly->publicLength();

        // We may have to walk the entire array to do the unshift. We're willing to do so
        // only if it's not horribly slow.
        unsigned moveCount = oldLength - startIndex;
        if (moveCount >= MIN_SPARSE_ARRAY_INDEX)
            RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));

        CheckedUint32 checkedLength(oldLength);
        checkedLength += count;
        if (checkedLength.hasOverflowed()) {
            throwOutOfMemoryError(globalObject, scope);
            return true;
        }
        unsigned newLength = checkedLength;
        if (newLength > MAX_STORAGE_VECTOR_LENGTH)
            return false;

        // We have to check for holes before we start moving things around so that we don't get halfway
        // through shifting and then realize we should have been in ArrayStorage mode.
#if USE(JSVALUE64)
        if (Options::useJSThreads() && oldLength > butterfly->vectorLength()) [[unlikely]]
            RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm))); // round 4: aliased publicLength raced past this snapshot's storage.
#endif
        if (moveCount && holesMustForwardToPrototype()) [[unlikely]] {
            auto* buffer = butterfly->contiguous().data() + startIndex;
            if (containsHole(buffer, moveCount)) [[unlikely]]
                RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));
        }

        if (newLength > butterfly->vectorLength()) {
            if (tryGrowAndShiftButterflyRight<WriteBarrier<Unknown>>(this, vm, butterfly, oldLength, newLength, startIndex, count))
                return true;
        }

        if (!ensureLength(vm, newLength)) {
            throwOutOfMemoryError(globalObject, scope);
            return true;
        }
#if USE(JSVALUE64)
        // SPEC-objectmodel Task 8 + review round 3: flag-on, ensureLength may
        // have gone segmented (T2) or a foreign F1 flip may have raced the
        // growth (SW=1). The in-place move below is owner-(currentTID, 0)
        // flat-only (I27) - re-dispatch to the ArrayStorage route otherwise.
        if (Options::useJSThreads()) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            if (isSegmentedButterfly(word)
                || ((word & butterflyPointerMask)
                    && (butterflySharedWrite(word) || butterflyWriterIsForeign(word))))
                RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));
            // Round 4: single-snapshot - the flat view below must come from
            // the SAME word this re-check dispatched on (see JSArray::pop).
            butterfly = untaggedButterfly(word);
        } else
            butterfly = this->butterfly();
#else
        butterfly = this->butterfly();
#endif

        if (moveCount)
            gcSafeMemmove(butterfly->contiguous().data() + startIndex + count, butterfly->contiguous().data() + startIndex, moveCount * sizeof(EncodedJSValue));

        // Our memmoving of values around in the array could have concealed some of them from
        // the collector. Let's make sure that the collector scans this object again.
        vm.writeBarrier(this);

        // NOTE: we're leaving being garbage in the part of the array that we shifted out
        // of. This is fine because the caller is required to store over that area, and
        // in contiguous mode storing into a hole is guaranteed to behave exactly the same
        // as storing over an existing element.

        return true;
    }

    case ArrayWithDouble: {
        unsigned oldLength = butterfly->publicLength();

        // We may have to walk the entire array to do the unshift. We're willing to do so
        // only if it's not horribly slow.
        unsigned moveCount = oldLength - startIndex;
        if (moveCount >= MIN_SPARSE_ARRAY_INDEX)
            RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));

        CheckedUint32 checkedLength(oldLength);
        checkedLength += count;
        if (checkedLength.hasOverflowed()) {
            throwOutOfMemoryError(globalObject, scope);
            return true;
        }
        unsigned newLength = checkedLength;
        if (newLength > MAX_STORAGE_VECTOR_LENGTH)
            return false;

        // We have to check for holes before we start moving things around so that we don't get halfway
        // through shifting and then realize we should have been in ArrayStorage mode.
#if USE(JSVALUE64)
        if (Options::useJSThreads() && oldLength > butterfly->vectorLength()) [[unlikely]]
            RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm))); // round 4: aliased publicLength raced past this snapshot's storage.
#endif
        if (moveCount && holesMustForwardToPrototype()) [[unlikely]] {
            for (unsigned i = oldLength; i-- > startIndex;) {
                double v = butterfly->contiguousDouble().at(this, i);
                if (v != v) [[unlikely]]
                    RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));
            }
        }

        if (newLength > butterfly->vectorLength()) {
            if (tryGrowAndShiftButterflyRight<double>(this, vm, butterfly, oldLength, newLength, startIndex, count))
                return true;
        }

        if (!ensureLength(vm, newLength)) {
            throwOutOfMemoryError(globalObject, scope);
            return true;
        }
#if USE(JSVALUE64)
        // SPEC-objectmodel Task 8 + review round 3: flag-on, ensureLength may
        // have gone segmented (T2) or a foreign F1 flip may have raced the
        // growth (SW=1). The in-place move below is owner-(currentTID, 0)
        // flat-only (I27) - re-dispatch to the ArrayStorage route otherwise.
        if (Options::useJSThreads()) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            if (isSegmentedButterfly(word)
                || ((word & butterflyPointerMask)
                    && (butterflySharedWrite(word) || butterflyWriterIsForeign(word))))
                RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, ensureArrayStorage(vm)));
            // Round 4: single-snapshot (see JSArray::pop).
            butterfly = untaggedButterfly(word);
        } else
            butterfly = this->butterfly();
#else
        butterfly = this->butterfly();
#endif

        if (moveCount)
            gcSafeMemmove(butterfly->contiguousDouble().data() + startIndex + count, butterfly->contiguousDouble().data() + startIndex, moveCount * sizeof(double));

        // NOTE: we're leaving being garbage in the part of the array that we shifted out
        // of. This is fine because the caller is required to store over that area, and
        // in contiguous mode storing into a hole is guaranteed to behave exactly the same
        // as storing over an existing element.

        return true;
    }
        
    case ArrayWithArrayStorage:
    case ArrayWithSlowPutArrayStorage:
        RELEASE_AND_RETURN(scope, unshiftCountWithArrayStorage(globalObject, startIndex, count, arrayStorage()));
        
    default:
        CRASH();
        return false;
    }
}

void JSArray::fillArgList(JSGlobalObject* globalObject, MarkedArgumentBuffer& args)
{
    unsigned i = 0;
    unsigned vectorEnd;
    WriteBarrier<Unknown>* vector;

#if USE(JSVALUE64)
    // Round 4 sweep: single-snapshot dispatch; segmented / flag-on AS / null
    // words skip the flat vector scan entirely - the trailing get() loop
    // below is the generic (regime-safe) path and covers every index.
    Butterfly* butterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, butterfly)) {
        for (; i < length(); ++i)
            args.append(get(globalObject, i));
        return;
    }
#else
    Butterfly* butterfly = this->butterfly();
#endif

    switch (indexingType()) {
    case ArrayClass:
        return;
        
    case ArrayWithUndecided: {
        vector = nullptr;
        vectorEnd = 0;
        break;
    }
        
    case ArrayWithInt32:
    case ArrayWithContiguous: {
        vectorEnd = butterfly->publicLength();
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            vectorEnd = std::min(vectorEnd, butterfly->vectorLength()); // round 4: snapshot bound.
#endif
        vector = butterfly->contiguous().data();
        break;
    }
        
    case ArrayWithDouble: {
        vector = nullptr;
        vectorEnd = 0;
        unsigned doubleEnd = butterfly->publicLength();
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            doubleEnd = std::min(doubleEnd, butterfly->vectorLength()); // round 4: snapshot bound.
#endif
        for (; i < doubleEnd; ++i) {
            double v = butterfly->contiguousDouble().at(this, i);
            if (v != v)
                break;
            args.append(JSValue(JSValue::EncodeAsDouble, v));
        }
        break;
    }
    
    case ARRAY_WITH_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = butterfly->arrayStorage();
        
        vector = storage->m_vector;
        vectorEnd = std::min(storage->length(), storage->vectorLength());
        break;
    }
        
    default:
        CRASH();
#if COMPILER_QUIRK(CONSIDERS_UNREACHABLE_CODE)
        vector = 0;
        vectorEnd = 0;
        break;
#endif
    }
    
    for (; i < vectorEnd; ++i) {
        WriteBarrier<Unknown>& v = vector[i];
        if (!v)
            break;
        args.append(v.get());
    }

    // FIXME: What prevents this from being called with a RuntimeArray? The length function will always return 0 in that case.
    for (; i < length(); ++i)
        args.append(get(globalObject, i));
}

void JSArray::copyToArguments(JSGlobalObject* globalObject, JSValue* firstElementDest, unsigned offset, unsigned length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    unsigned i = offset;
    WriteBarrier<Unknown>* vector;
    unsigned vectorEnd;
    length += offset; // We like to think of the length as being our length, rather than the output length.

    // FIXME: What prevents this from being called with a RuntimeArray? The length function will always return 0 in that case.
    ASSERT(length == this->length());

#if USE(JSVALUE64)
    // Round 4 sweep: single-snapshot dispatch; segmented / flag-on AS / null
    // words skip the flat vector scan - the trailing get() loop below is the
    // generic (regime-safe) path and covers every index.
    Butterfly* butterfly;
    if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, butterfly)) {
        for (; i < length; ++i) {
            firstElementDest[i - offset] = get(globalObject, i);
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }
#else
    Butterfly* butterfly = this->butterfly();
#endif
    switch (indexingType()) {
    case ArrayClass:
        return;
        
    case ArrayWithUndecided: {
        vector = nullptr;
        vectorEnd = 0;
        break;
    }
        
    case ArrayWithInt32:
    case ArrayWithContiguous: {
        vector = butterfly->contiguous().data();
        vectorEnd = butterfly->publicLength();
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            vectorEnd = std::min(vectorEnd, butterfly->vectorLength()); // round 4: snapshot bound.
#endif
        break;
    }
        
    case ArrayWithDouble: {
        vector = nullptr;
        vectorEnd = 0;
        unsigned doubleEnd = butterfly->publicLength();
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            doubleEnd = std::min(doubleEnd, butterfly->vectorLength()); // round 4: snapshot bound.
#endif
        for (; i < doubleEnd; ++i) {
            ASSERT(i < butterfly->vectorLength());
            double v = butterfly->contiguousDouble().at(this, i);
            if (v != v)
                break;
            firstElementDest[i - offset] = JSValue(JSValue::EncodeAsDouble, v);
        }
        break;
    }
        
    case ARRAY_WITH_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = butterfly->arrayStorage();
        vector = storage->m_vector;
        vectorEnd = std::min(length, storage->vectorLength());
        break;
    }
        
    default:
        CRASH();
#if COMPILER_QUIRK(CONSIDERS_UNREACHABLE_CODE)
        vector = 0;
        vectorEnd = 0;
        break;
#endif
    }
    
    for (; i < vectorEnd; ++i) {
        WriteBarrier<Unknown>& v = vector[i];
        if (!v)
            break;
        firstElementDest[i - offset] = v.get();
    }
    
    for (; i < length; ++i) {
        firstElementDest[i - offset] = get(globalObject, i);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool JSArray::isIteratorProtocolFastAndNonObservable()
{
    JSGlobalObject* globalObject = this->realm();
    if (!globalObject->isArrayPrototypeIteratorProtocolFastAndNonObservable())
        return false;

    VM& vm = globalObject->vm();
    Structure* structure = this->structure();
    // This is the fast case. Many arrays will be an original array.
    if (globalObject->isOriginalArrayStructure(structure))
        return true;

    if (structure->mayInterceptIndexedAccesses())
        return false;

    if (getPrototypeDirect() != globalObject->arrayPrototype())
        return false;

    if (getDirectOffset(vm, vm.propertyNames->iteratorSymbol) != invalidOffset)
        return false;

    return true;
}

bool JSArray::isToPrimitiveFastAndNonObservable()
{
    JSGlobalObject* globalObject = this->realm();
    if (!globalObject->arrayPrototypeChainIsSane()) [[unlikely]]
        return false;
    if (!globalObject->arrayToStringWatchpointSet().isStillValid()) [[unlikely]]
        return false;
    if (!globalObject->arraySymbolToPrimitiveWatchpointSet().isStillValid()) [[unlikely]]
        return false;
    if (!globalObject->arrayJoinWatchpointSet().isStillValid()) [[unlikely]]
        return false;
    if (!globalObject->objectPrototypeValueOfWatchpointSet().isStillValid()) [[unlikely]]
        return false;
    if (!globalObject->arrayPrototypeValueOfWatchpointSet().isStillValid()) [[unlikely]]
        return false;

    Structure* structure = this->structure();
    return globalObject->isOriginalArrayStructure(structure);
}

template<AllocationFailureMode failureMode>
inline JSArray* constructArray(ObjectInitializationScope& scope, Structure* arrayStructure, unsigned length)
{
    JSArray* array = JSArray::tryCreateUninitializedRestricted(scope, arrayStructure, length);

    // FIXME: we should probably throw an out of memory error here, but
    // when making this change we should check that all clients of this
    // function will correctly handle an exception being thrown from here.
    // https://bugs.webkit.org/show_bug.cgi?id=169786
    if constexpr (failureMode == AllocationFailureMode::Assert)
        RELEASE_ASSERT_RESOURCE_AVAILABLE(array, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
    else if (!array)
        return nullptr;

    // FIXME: We only need this for subclasses of Array because we might need to allocate a new structure to change
    // indexing types while initializing. If this triggered a GC then we might scan our currently uninitialized
    // array and crash. https://bugs.webkit.org/show_bug.cgi?id=186811
    if (!arrayStructure->realm()->isOriginalArrayStructure(arrayStructure))
        JSArray::eagerlyInitializeButterfly(scope, array, length);

    return array;
}

JSArray* constructArray(JSGlobalObject* globalObject, Structure* arrayStructure, const ArgList& values)
{
    VM& vm = globalObject->vm();
    unsigned length = values.size();
    ObjectInitializationScope scope(vm);

    JSArray* array = constructArray<AllocationFailureMode::Assert>(scope, arrayStructure, length);
    for (unsigned i = 0; i < length; ++i)
        array->initializeIndex(scope, i, values.at(i));
    return array;
}

JSArray* constructArray(JSGlobalObject* globalObject, Structure* arrayStructure, const JSValue* values, unsigned length)
{
    VM& vm = globalObject->vm();
    ObjectInitializationScope scope(vm);

    JSArray* array = constructArray<AllocationFailureMode::Assert>(scope, arrayStructure, length);
    for (unsigned i = 0; i < length; ++i)
        array->initializeIndex(scope, i, values[i]);
    return array;
}

JSArray* constructArrayNegativeIndexed(JSGlobalObject* globalObject, Structure* arrayStructure, const JSValue* values, unsigned length)
{
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    ObjectInitializationScope scope(vm);

    JSArray* array = constructArray<AllocationFailureMode::ReturnNull>(scope, arrayStructure, length);
    if (!array) [[unlikely]] {
        throwOutOfMemoryError(globalObject, throwScope);
        return nullptr;
    }

    for (int i = 0; i < static_cast<int>(length); ++i)
        array->initializeIndex(scope, i, values[-i]);
    return array;
}

JSArray* constructArrayPair(JSGlobalObject* globalObject, JSValue first, JSValue second)
{
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    ObjectInitializationScope initializationScope(vm);

    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);

    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, structure, 2);
    if (!array) [[unlikely]] {
        throwOutOfMemoryError(globalObject, throwScope);
        return nullptr;
    }
    array->initializeIndex(initializationScope, 0, first);
    array->initializeIndex(initializationScope, 1, second);

    return array;
}

template<>
void clearElement(double& element)
{
    element = PNaN;
}

template<ArrayFillMode fillMode>
JSArray* tryCloneArrayFromFast(JSGlobalObject* globalObject, JSValue arrayValue)
{
    ASSERT(isJSArray(arrayValue));

    VM& vm = globalObject->vm();

    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* array = uncheckedDowncast<JSArray>(arrayValue);
    if (!array->isIteratorProtocolFastAndNonObservable()) [[unlikely]]
        return nullptr;

    IndexingType sourceType = array->indexingType();
    if (shouldUseSlowPut(sourceType) || sourceType == ArrayClass) [[unlikely]]
        return nullptr;

    Butterfly* butterfly= array->butterfly();
    unsigned resultSize = butterfly->publicLength();
    if (hasAnyArrayStorage(sourceType) || resultSize >= MIN_SPARSE_ARRAY_INDEX) [[unlikely]] {
        JSArray* result = constructEmptyArray(globalObject, nullptr, resultSize);
        RETURN_IF_EXCEPTION(scope, { });

        scope.release();
        moveArrayElements<fillMode>(globalObject, vm, result, 0, array, resultSize);
        return result;
    }

    ASSERT(sourceType == ArrayWithDouble || sourceType == ArrayWithInt32 || sourceType == ArrayWithContiguous || sourceType == ArrayWithUndecided);

    if (!globalObject->isHavingABadTime()) [[likely]] {
        if constexpr (fillMode == ArrayFillMode::Empty) {
            if (isCopyOnWrite(array->indexingMode()))
                return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(array->indexingMode()), array->butterfly());
        }

        if (!resultSize)
            RELEASE_AND_RETURN(scope, constructEmptyArray(globalObject, nullptr));
    }

    IndexingType resultType = sourceType;
    if constexpr (fillMode == ArrayFillMode::Undefined)  {
        if (sourceType == ArrayWithDouble) {
            double* buffer = butterfly->contiguousDouble().data();
            if (containsHole(buffer, resultSize)) [[unlikely]]
                resultType = ArrayWithContiguous;
        } else if (sourceType == ArrayWithInt32) {
            auto* buffer = butterfly->contiguous().data();
            if (containsHole(buffer, resultSize)) [[unlikely]]
                resultType = ArrayWithContiguous;
        } else if (sourceType == ArrayWithUndecided && resultSize)
            resultType = ArrayWithContiguous;
    }

    Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(resultType);
    if (hasAnyArrayStorage(resultStructure->indexingType())) [[unlikely]]
        return nullptr;

    ASSERT(!globalObject->isHavingABadTime());
    auto vectorLength = Butterfly::optimalContiguousVectorLength(resultStructure, resultSize);
    if (vectorLength > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
        return { };

    ASSERT(!resultStructure->outOfLineCapacity());
    void* memory = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!memory) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    auto* resultButterfly = Butterfly::fromBase(memory, 0, 0);
    resultButterfly->setVectorLength(vectorLength);
    resultButterfly->setPublicLength(resultSize);

    switch (resultType) {
    case ArrayWithUndecided:
        if constexpr (fillMode == ArrayFillMode::Empty) {
            auto* buffer = resultButterfly->contiguous().data();
            copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(buffer, 0, butterfly->contiguous().data(), 0, resultSize, ArrayWithUndecided);
            break;
        }
        ASSERT(!resultSize);
        break;
    case ArrayWithDouble: {
        ASSERT(sourceType == ArrayWithDouble);
        double* buffer = resultButterfly->contiguousDouble().data();
        copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(buffer, 0, butterfly->contiguousDouble().data(), 0, resultSize, ArrayWithDouble);
        break;
    }
    case ArrayWithInt32: {
        ASSERT(sourceType == ArrayWithInt32);
        auto* buffer = resultButterfly->contiguous().data();
        copyArrayElements<ArrayFillMode::Empty, NeedsGCSafeOps::No>(buffer, 0, butterfly->contiguous().data(), 0, resultSize, ArrayWithInt32);
        break;
    }
    case ArrayWithContiguous: {
        auto* buffer = resultButterfly->contiguous().data();
        if (sourceType == ArrayWithDouble)
            copyArrayElements<fillMode, NeedsGCSafeOps::No>(buffer, 0, butterfly->contiguousDouble().data(), 0, resultSize, ArrayWithDouble);
        else
            copyArrayElements<fillMode, NeedsGCSafeOps::No>(buffer, 0, butterfly->contiguous().data(), 0, resultSize, sourceType);
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Butterfly::clearRange(resultType, resultButterfly, resultSize, vectorLength);
    return JSArray::createWithButterfly(vm, nullptr, resultStructure, resultButterfly);
}

template JSArray* tryCloneArrayFromFast<ArrayFillMode::Undefined>(JSGlobalObject*, JSValue);
template JSArray* tryCloneArrayFromFast<ArrayFillMode::Empty>(JSGlobalObject*, JSValue);

static uint64_t calculateFlattenedLength(JSGlobalObject* globalObject, JSArray* sourceArray, uint64_t sourceLength, uint64_t depth)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return std::numeric_limits<uint64_t>::max();
    }

    CheckedUint64 resultLength = 0;
    auto lengthExceeded = [&]() -> bool {
        return resultLength.hasOverflowed() || resultLength > maxSafeIntegerAsUInt64();
    };

    IndexingType sourceType = sourceArray->indexingType();

#if USE(JSVALUE64)
    // Round 4 sweep: this recurses into arbitrarily NESTED shared arrays -
    // every level needs its own single-snapshot probe (segmented / flag-on AS
    // => bail to the generic flatten), and sourceLength (read via the
    // regime-safe length()) must be re-bounded by THIS snapshot's
    // vectorLength.
    Butterfly* sourceButterfly;
    if (!jsThreadsFlatSnapshot(sourceArray, JSThreadsFastPathIntent::Read, sourceButterfly))
        return std::numeric_limits<uint64_t>::max();
    if (Options::useJSThreads() && sourceLength > sourceButterfly->vectorLength()) [[unlikely]]
        return std::numeric_limits<uint64_t>::max();
#else
    Butterfly* sourceButterfly = sourceArray->butterfly();
#endif

    switch (sourceType) {
    case ArrayWithInt32: {
        auto* sourceBuffer = sourceButterfly->contiguous().data();
        for (uint64_t i = 0; i < sourceLength; ++i) {
            JSValue element = sourceBuffer[i].get();
            if (!element) [[unlikely]]
                continue;
            resultLength++;
            if (lengthExceeded()) [[unlikely]] {
                throwTypeError(globalObject, scope, "flatten array exceeds 2**52 - 1");
                return std::numeric_limits<uint64_t>::max();
            }
        }
        break;
    }
    case ArrayWithContiguous: {
        auto* sourceBuffer = sourceButterfly->contiguous().data();
        for (uint64_t i = 0; i < sourceLength; ++i) {
            JSValue element = sourceBuffer[i].get();
            if (!element) [[unlikely]]
                continue;
            if (depth > 0 && isJSArray(element)) {
                JSArray* elementArray = uncheckedDowncast<JSArray>(element);
                uint64_t newDepth = (depth == std::numeric_limits<uint64_t>::max()) ? depth : depth - 1;
                uint64_t flatLength = calculateFlattenedLength(globalObject, elementArray, elementArray->length(), newDepth);
                RETURN_IF_EXCEPTION(scope, flatLength);
                if (flatLength == std::numeric_limits<uint64_t>::max()) [[unlikely]]
                    return flatLength;
                resultLength += flatLength;
                if (lengthExceeded()) [[unlikely]] {
                    throwTypeError(globalObject, scope, "flatten array exceeds 2**52 - 1");
                    return std::numeric_limits<uint64_t>::max();
                }
            } else {
                if (element.isObject()) {
                    JSType type = asObject(element)->type();
                    if (type == ProxyObjectType || type == DerivedArrayType) [[unlikely]]
                        return std::numeric_limits<uint64_t>::max();
                }
                resultLength++;
                if (lengthExceeded()) [[unlikely]] {
                    throwTypeError(globalObject, scope, "flatten array exceeds 2**52 - 1");
                    return std::numeric_limits<uint64_t>::max();
                }
            }
        }
        break;
    }
    case ArrayWithDouble: {
        auto* sourceBuffer = sourceButterfly->contiguousDouble().data();
        for (uint64_t i = 0; i < sourceLength; ++i) {
            double value = sourceBuffer[i];
            if (std::isnan(value)) [[unlikely]]
                continue;
            resultLength++;
            if (lengthExceeded()) [[unlikely]] {
                throwTypeError(globalObject, scope, "flatten array exceeds 2**52 - 1");
                return std::numeric_limits<uint64_t>::max();
            }
        }
        break;
    }
    default:
        return std::numeric_limits<uint64_t>::max();
    }

    return resultLength;
}

template<typename T>
static uint64_t fastFlatIntoBuffer(JSGlobalObject* globalObject, T* resultBuffer, uint64_t& resultIndex, JSArray* sourceArray, uint64_t sourceLength, uint64_t depth, uint64_t vectorLength)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return std::numeric_limits<uint64_t>::max();
    }

    IndexingType sourceType = sourceArray->indexingType();

#if USE(JSVALUE64)
    // Round 4 sweep: per-level single-snapshot probe + bound, as in
    // calculateFlattenedLength above (a max() return makes every caller bail
    // to the generic flatten; the result array's clearRange covers any
    // shortfall).
    Butterfly* sourceButterfly;
    if (!jsThreadsFlatSnapshot(sourceArray, JSThreadsFastPathIntent::Read, sourceButterfly))
        return std::numeric_limits<uint64_t>::max();
    if (Options::useJSThreads() && sourceLength > sourceButterfly->vectorLength()) [[unlikely]]
        return std::numeric_limits<uint64_t>::max();
#else
    Butterfly* sourceButterfly = sourceArray->butterfly();
#endif

    switch (sourceType) {
    case ArrayWithInt32: {
        auto* sourceBuffer = sourceButterfly->contiguous().data();
        for (uint64_t i = 0; i < sourceLength; ++i) {
            if (resultIndex >= vectorLength) [[unlikely]]
                return std::numeric_limits<uint64_t>::max();
            JSValue element = sourceBuffer[i].get();
            if (!element) [[unlikely]]
                continue;
            if constexpr (std::is_same_v<T, double>)
                resultBuffer[resultIndex] = element.asNumber();
            else
                resultBuffer[resultIndex].setWithoutWriteBarrier(element);
            ++resultIndex;
        }
        break;
    }
    case ArrayWithContiguous: {
        auto* sourceBuffer = sourceButterfly->contiguous().data();
        for (uint64_t i = 0; i < sourceLength; ++i) {
            if (resultIndex >= vectorLength) [[unlikely]]
                return std::numeric_limits<uint64_t>::max();
            JSValue element = sourceBuffer[i].get();
            if (!element) [[unlikely]]
                continue;
            if (depth > 0 && isJSArray(element)) {
                JSArray* elementArray = uncheckedDowncast<JSArray>(element);
                uint64_t newDepth = (depth == std::numeric_limits<uint64_t>::max()) ? depth : depth - 1;
                resultIndex = fastFlatIntoBuffer(globalObject, resultBuffer, resultIndex, elementArray, elementArray->length(), newDepth, vectorLength);
                RETURN_IF_EXCEPTION(scope, resultIndex);
                if (resultIndex == std::numeric_limits<uint64_t>::max())
                    return std::numeric_limits<uint64_t>::max();
            } else {
                if constexpr (std::is_same_v<T, double>)
                    resultBuffer[resultIndex] = element.asNumber();
                else
                    resultBuffer[resultIndex].setWithoutWriteBarrier(element);
                ++resultIndex;
            }
        }
        break;
    }
    case ArrayWithDouble: {
        auto* sourceBuffer = sourceButterfly->contiguousDouble().data();
        for (uint64_t i = 0; i < sourceLength; ++i) {
            if (resultIndex >= vectorLength) [[unlikely]]
                return std::numeric_limits<uint64_t>::max();
            double value = sourceBuffer[i];
            if (std::isnan(value)) [[unlikely]]
                continue;
            if constexpr (std::is_same_v<T, double>)
                resultBuffer[resultIndex] = value;
            else
                resultBuffer[resultIndex].setWithoutWriteBarrier(JSValue(value));
            ++resultIndex;
        }
        break;
    }
    default: {
        RELEASE_ASSERT_NOT_REACHED();
        return std::numeric_limits<uint64_t>::max();
    }
    }
    return resultIndex;
}

JSArray* JSArray::fastFlat(JSGlobalObject* globalObject, uint64_t depth, uint64_t length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    IndexingType sourceType = this->indexingType();

    switch (sourceType) {
    case ArrayWithInt32:
    case ArrayWithDouble:
    case ArrayWithContiguous: {
#if USE(JSVALUE64)
        // Round 4 sweep: probe + snapshot bound (the per-level probes inside
        // the helpers re-check every nested array, including `this` again).
        {
            Butterfly* selfButterfly;
            if (!jsThreadsFlatSnapshot(this, JSThreadsFastPathIntent::Read, selfButterfly))
                return nullptr;
            if (length > selfButterfly->vectorLength()) [[unlikely]]
                return nullptr;
        }
#else
        if (length > this->butterfly()->vectorLength()) [[unlikely]]
            return nullptr;
#endif

        if (holesMustForwardToPrototype()) [[unlikely]]
            return nullptr;

        uint64_t flattenedLength = calculateFlattenedLength(globalObject, this, length, depth);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (flattenedLength == std::numeric_limits<uint64_t>::max()) [[unlikely]]
            return nullptr;

        Structure* resultStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(sourceType);

        IndexingType indexingType = resultStructure->indexingType();
        if (hasAnyArrayStorage(indexingType)) [[unlikely]]
            return nullptr;
        ASSERT(!globalObject->isHavingABadTime());

        auto vectorLength = Butterfly::optimalContiguousVectorLength(resultStructure, flattenedLength);

        void* memory = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)),
            nullptr, AllocationFailureMode::ReturnNull);
        if (!memory) [[unlikely]]
            return nullptr;

        auto* butterfly = Butterfly::fromBase(memory, 0, 0);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(flattenedLength);

        uint64_t resultIndex = 0;
        if (indexingType == ArrayWithDouble) {
            auto* resultBuffer = butterfly->contiguousDouble().data();
            resultIndex = fastFlatIntoBuffer(globalObject, resultBuffer, resultIndex, this, length, depth, vectorLength);
        } else {
            auto* resultBuffer = butterfly->contiguous().data();
            resultIndex = fastFlatIntoBuffer(globalObject, resultBuffer, resultIndex, this, length, depth, vectorLength);
        }
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (resultIndex == std::numeric_limits<uint64_t>::max())
            return nullptr;

        Butterfly::clearRange(indexingType, butterfly, resultIndex, vectorLength);
        return createWithButterfly(vm, nullptr, resultStructure, butterfly);
    }
    default: {
        return nullptr;
    }
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
