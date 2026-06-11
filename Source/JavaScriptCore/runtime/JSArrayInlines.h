/*
 *  Copyright (C) 2016-2022 Apple Inc. All rights reserved.
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

#pragma once

#include "ArrayPrototype.h"
#include "ButterflyInlines.h"
#include "ClonedArguments.h"
#include "DirectArguments.h"
#include "Error.h"
#include "JSArray.h"
#include "JSCellInlines.h"
#include "JSObjectInlines.h"
#include "ScopedArguments.h"
#include "Structure.h"
#include "StructureArrayStorageInlines.h"
#include <JavaScriptCore/ResourceExhaustion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

inline JSArray* JSArray::tryCreate(VM& vm, Structure* structure, unsigned initialLength, unsigned vectorLengthHint)
{
    ASSERT(vectorLengthHint >= initialLength);
    unsigned outOfLineStorage = structure->outOfLineCapacity();

    Butterfly* butterfly;
    IndexingType indexingType = structure->indexingType();
    if (!hasAnyArrayStorage(indexingType)) [[likely]] {
        ASSERT(
            hasUndecided(indexingType)
            || hasInt32(indexingType)
            || hasDouble(indexingType)
            || hasContiguous(indexingType));

        if (vectorLengthHint > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]]
            return nullptr;

        unsigned vectorLength = Butterfly::optimalContiguousVectorLength(structure, vectorLengthHint);
        void* temp = vm.auxiliarySpace().allocate(
            vm,
            Butterfly::totalSize(0, outOfLineStorage, true, vectorLength * sizeof(EncodedJSValue)),
            nullptr, AllocationFailureMode::ReturnNull);
        if (!temp)
            return nullptr;
        butterfly = Butterfly::fromBase(temp, 0, outOfLineStorage);
        butterfly->setVectorLength(vectorLength);
        butterfly->setPublicLength(initialLength);
        Butterfly::clearRange(indexingType, butterfly, 0, vectorLength);
    } else {
        ASSERT(
            indexingType == ArrayWithSlowPutArrayStorage
            || indexingType == ArrayWithArrayStorage);
        butterfly = tryCreateArrayButterfly(vm, nullptr, initialLength);
        if (!butterfly)
            return nullptr;
        for (unsigned i = 0; i < BASE_ARRAY_STORAGE_VECTOR_LEN; ++i)
            butterfly->arrayStorage()->m_vector[i].clear();
    }

    return createWithButterfly(vm, nullptr, structure, butterfly);
}

inline JSArray* JSArray::tryCreate(VM& vm, Structure* structure, unsigned initialLength)
{
    return tryCreate(vm, structure, initialLength, initialLength);
}

inline JSArray* JSArray::create(VM& vm, Structure* structure, unsigned initialLength)
{
    JSArray* result = JSArray::tryCreate(vm, structure, initialLength);
    RELEASE_ASSERT_RESOURCE_AVAILABLE(result, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
    return result;
}

inline Structure* JSArray::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, IndexingType indexingType)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ArrayType, StructureFlags), info(), indexingType);
}

inline IndexingType JSArray::mergeIndexingTypeForCopying(IndexingType other, bool allowPromotion)
{
    IndexingType type = indexingType();
    if (!(type & IsArray && other & IsArray))
        return NonArray;

    if (hasAnyArrayStorage(type) || hasAnyArrayStorage(other))
        return NonArray;

    if (type == ArrayWithUndecided)
        return other;

    if (other == ArrayWithUndecided)
        return type;

    // We can memcpy an Int32 and a Contiguous into a Contiguous array since
    // both share the same memory layout for Int32 numbers.
    if ((type == ArrayWithInt32 || type == ArrayWithContiguous)
        && (other == ArrayWithInt32 || other == ArrayWithContiguous)) {
        if (other == ArrayWithContiguous)
            return other;
        return type;
    }

    if (allowPromotion) {
        if ((type == ArrayWithInt32 || type == ArrayWithDouble) && (other == ArrayWithInt32 || other == ArrayWithDouble)) {
            if (type == other)
                return type;
            return ArrayWithDouble;
        }
    }

    if (type != other)
        return NonArray;

    return type;
}

ALWAYS_INLINE bool JSArray::holesMustForwardToPrototype() const
{
    Structure* structure = this->structure();
    if (type() == ArrayType) [[likely]] {
        JSGlobalObject* globalObject = structure->realm();
        if (structure->hasMonoProto() && structure->storedPrototype() == globalObject->arrayPrototype() && globalObject->arrayPrototypeChainIsSane()) [[likely]]
            return false;
    }
    return structure->holesMustForwardToPrototype(const_cast<JSArray*>(this));
}

inline bool JSArray::canFastCopy(JSArray* otherArray) const
{
    // SPEC-objectmodel round 4 NOTE: this predicate only excludes ArrayStorage
    // SHAPES - it deliberately does NOT probe the flag-on butterfly regime.
    // Every owned consumer (appendMemcpy, fastSlice) carries its own flag-on
    // single-snapshot probe (segmented / shared / foreign / null dispositions)
    // at its memcpy site; unowned ArrayPrototype consumers are covered by the
    // §10.7 integrator guard list (INTEGRATE-objectmodel §44).
    if (hasAnyArrayStorage(indexingType()) || hasAnyArrayStorage(otherArray->indexingType()))
        return false;
    if (holesMustForwardToPrototype() || otherArray->holesMustForwardToPrototype())
        return false;
    return true;
}

inline bool JSArray::canFastAppend(JSArray* otherArray) const
{
    // Append can modify itself, thus, we cannot do fast-append if |this| and otherArray are the same.
    if (otherArray == this)
        return false;
    return canFastCopy(otherArray);
}

inline bool JSArray::canDoFastIndexedAccess() const
{
    JSGlobalObject* globalObject = this->realm();
    if (!globalObject->arrayPrototypeChainIsSane())
        return false;

    Structure* structure = this->structure();
    // This is the fast case. Many arrays will be an original array.
    if (globalObject->isOriginalArrayStructure(structure))
        return true;

    if (structure->mayInterceptIndexedAccesses())
        return false;

    if (getPrototypeDirect() != globalObject->arrayPrototype())
        return false;

    return true;
}

ALWAYS_INLINE bool JSArray::definitelyNegativeOneMiss() const
{
    JSGlobalObject* globalObject = this->realm();
    if (!globalObject->arrayPrototypeChainIsSane())
        return false;

    if (!globalObject->arrayNegativeOneWatchpointSet().isStillValid())
        return false;

    Structure* structure = this->structure();
    // This is the fast case. Many arrays will be an original array.
    if (globalObject->isOriginalArrayStructure(structure))
        return true;

    if (getPrototypeDirect() != globalObject->arrayPrototype())
        return false;

    if (structure->seenProperties().bits())
        return false;

    return true;
}


ALWAYS_INLINE uint64_t toLength(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isJSArray(object)) [[likely]]
        return uncheckedDowncast<JSArray>(object)->length();

    switch (object->type()) {
    case DirectArgumentsType:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<DirectArguments>(object)->length(globalObject));
    case ScopedArgumentsType:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<ScopedArguments>(object)->length(globalObject));
    case ClonedArgumentsType:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<ClonedArguments>(object)->length(globalObject));
    default:
        break;
    }
    JSValue lengthValue = object->get(globalObject, vm.propertyNames->length);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, lengthValue.toLength(globalObject));
}

ALWAYS_INLINE void JSArray::pushInline(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ensureWritable(vm);

#if USE(JSVALUE64)
    // SPEC-objectmodel Task 8 (§4.4) + review round 3 (§3 foreign-first-write
    // protocol, I12/I21): the flat in-place fast paths below mutate a FLAT
    // word (element store + plain setPublicLength) and are sound only for the
    // word's exclusive owner - (currentTID, 0). Everything else routes through
    // the §9.5 indexed driver, which carries the gates the fast paths elide:
    //   - segmented words: spine-addressed stores, CAS-max length bumps (the
    //     flat accessors would garbage-decode the spine);
    //   - foreign SW=0 words: ensureSharedWriteBit fires F1 and flips SW
    //     BEFORE the store (otherwise writeThreadLocal(S) stays valid while a
    //     foreign write lands - unsounding E2/E4 elision - and an owner T1
    //     copying resize could silently drop the store, I21);
    //   - flat SW=1 words: the publicLength bump must be the monotone CAS-max
    //     (updatePublicLengthAfterDenseStoreConcurrent inside the driver) -
    //     a read-then-plain-store racing another pusher regresses the
    //     winner's bump and hides its element (the round-2 racing-growers
    //     fix, reintroduced here in round 2's audit gap);
    //   - AS words with a foreign SW=0 writer: the §4.6 per-event SW stop.
    // Residue (shape transition on a shared word, sparse territory) takes the
    // generic putByIndex protocol.
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word)
            || ((word & butterflyPointerMask)
                && (butterflySharedWrite(word) || butterflyWriterIsForeign(word)))) {
            unsigned oldLength = length();
            if (oldLength > MAX_ARRAY_INDEX) [[unlikely]] {
                methodTable()->putByIndex(this, globalObject, oldLength, value, true);
                if (!scope.exception())
                    throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
                return;
            }
            if (putIndexConcurrent(vm, oldLength, value))
                return;
            scope.release();
            methodTable()->putByIndex(this, globalObject, oldLength, value, true);
            return;
        }
    }
#endif

    Butterfly* butterfly = this->butterfly();

    switch (indexingMode()) {
    case ArrayClass: {
        createInitialUndecided(vm, 0);
        [[fallthrough]];
    }

    case ArrayWithUndecided: {
        convertUndecidedForValue(vm, value);
        scope.release();
        push(globalObject, value);
        return;
    }

    case ArrayWithInt32: {
        if (!value.isInt32()) {
            convertInt32ForValue(vm, value);
            scope.release();
            push(globalObject, value);
            return;
        }

        unsigned length = butterfly->publicLength();
        ASSERT(length <= butterfly->vectorLength());
        if (length < butterfly->vectorLength()) {
            butterfly->contiguousInt32().at(this, length).setWithoutWriteBarrier(value);
            butterfly->setPublicLength(length + 1);
            return;
        }

        if (length > MAX_ARRAY_INDEX) [[unlikely]] {
            methodTable()->putByIndex(this, globalObject, length, value, true);
            if (!scope.exception())
                throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
            return;
        }

        scope.release();
        putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(globalObject, length, value);
        return;
    }

    case ArrayWithContiguous: {
        unsigned length = butterfly->publicLength();
        ASSERT(length <= butterfly->vectorLength());
        if (length < butterfly->vectorLength()) {
            butterfly->contiguous().at(this, length).setWithoutWriteBarrier(value);
            butterfly->setPublicLength(length + 1);
            vm.writeBarrier(this, value);
            return;
        }

        if (length > MAX_ARRAY_INDEX) [[unlikely]] {
            methodTable()->putByIndex(this, globalObject, length, value, true);
            if (!scope.exception())
                throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
            return;
        }

        scope.release();
        putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(globalObject, length, value);
        return;
    }

    case ArrayWithDouble: {
        ASSERT(Options::allowDoubleShape());
        if (!value.isNumber()) {
            convertDoubleToContiguous(vm);
            scope.release();
            push(globalObject, value);
            return;
        }
        double valueAsDouble = value.asNumber();
        if (valueAsDouble != valueAsDouble) {
            convertDoubleToContiguous(vm);
            scope.release();
            push(globalObject, value);
            return;
        }

        unsigned length = butterfly->publicLength();
        ASSERT(length <= butterfly->vectorLength());
        if (length < butterfly->vectorLength()) {
            butterfly->contiguousDouble().at(this, length) = valueAsDouble;
            butterfly->setPublicLength(length + 1);
            return;
        }

        if (length > MAX_ARRAY_INDEX) [[unlikely]] {
            methodTable()->putByIndex(this, globalObject, length, value, true);
            if (!scope.exception())
                throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
            return;
        }

        scope.release();
        putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(globalObject, length, value);
        return;
    }

    case ArrayWithSlowPutArrayStorage: {
        unsigned oldLength = length();
        bool putResult = false;
        bool result = attemptToInterceptPutByIndexOnHole(globalObject, oldLength, value, true, putResult);
        RETURN_IF_EXCEPTION(scope, void());
        if (result) {
            if (oldLength < 0xFFFFFFFFu) {
                scope.release();
                setLength(globalObject, oldLength + 1, true);
            }
            return;
        }
        [[fallthrough]];
    }

    case ArrayWithArrayStorage: {
#if USE(JSVALUE64)
        // SPEC-objectmodel I31/L5 (Task 8): flag-on, every runtime AS access
        // is cell-locked; the in-vector fast push re-reads the storage under
        // the lock (AS-COPY republishes) - its element/length/
        // m_numValuesInVector stores are the in-place stores §4.6 sanctions
        // under the lock. The beyond-vector paths run unlocked here; their
        // mutation sites (increaseVectorLength, sparse map) lock themselves.
        if (Options::useJSThreads()) [[unlikely]] {
            {
                Locker locker { cellLock() };
                ArrayStorage* lockedStorage = this->butterfly()->arrayStorage();
                unsigned lockedLength = lockedStorage->length();
                if (lockedLength < lockedStorage->vectorLength()) {
                    lockedStorage->m_vector[lockedLength].set(vm, this, value);
                    lockedStorage->setLength(lockedLength + 1);
                    ++lockedStorage->m_numValuesInVector;
                    return;
                }
            }
            ArrayStorage* unlockedStorage = this->butterfly()->arrayStorage();
            if (unlockedStorage->length() > MAX_ARRAY_INDEX) [[unlikely]] {
                methodTable()->putByIndex(this, globalObject, unlockedStorage->length(), value, true);
                if (!scope.exception())
                    throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
                return;
            }
            scope.release();
            putByIndexBeyondVectorLengthWithArrayStorage(globalObject, unlockedStorage->length(), value, true, unlockedStorage);
            return;
        }
#endif
        ArrayStorage* storage = butterfly->arrayStorage();

        // Fast case - push within vector, always update m_length & m_numValuesInVector.
        unsigned length = storage->length();
        if (length < storage->vectorLength()) {
            storage->m_vector[length].set(vm, this, value);
            storage->setLength(length + 1);
            ++storage->m_numValuesInVector;
            return;
        }

        // Pushing to an array of invalid length (2^31-1) stores the property, but throws a range error.
        if (storage->length() > MAX_ARRAY_INDEX) [[unlikely]] {
            methodTable()->putByIndex(this, globalObject, storage->length(), value, true);
            // Per ES5.1 15.4.4.7 step 6 & 15.4.5.1 step 3.d.
            if (!scope.exception())
                throwException(globalObject, scope, createRangeError(globalObject, LengthExceededTheMaximumArrayLengthError));
            return;
        }

        // Handled the same as putIndex.
        scope.release();
        putByIndexBeyondVectorLengthWithArrayStorage(globalObject, storage->length(), value, true, storage);
        return;
    }

    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

ALWAYS_INLINE JSValue getProperty(JSGlobalObject* globalObject, JSObject* object, uint64_t index)
{
    if (JSValue result = object->tryGetIndexQuickly(index))
        return result;

    // Don't return undefined if the property is not found.
    return object->getIfPropertyExists(globalObject, index);
}

ALWAYS_INLINE bool isHole(double value)
{
    return std::isnan(value);
}

template<typename T>
ALWAYS_INLINE bool containsHole(const T* data, unsigned length)
{
    if constexpr (std::is_same_v<T, double>)
        return WTF::findNaN(data, length);
    else
        return WTF::find64(std::bit_cast<const uint64_t*>(data), JSValue::encode(JSValue()), length);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
