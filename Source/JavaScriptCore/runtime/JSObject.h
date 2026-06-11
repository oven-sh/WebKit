/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2024 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "ArrayConventions.h"
#include "ArrayStorage.h"
#include "Butterfly.h"
#include "CPU.h"
#include "CagedBarrierPtr.h"
#include "CallFrame.h"
#include "ClassInfo.h"
#include "ConcurrentButterfly.h"
#include "CustomGetterSetter.h"
#include "DOMAttributeGetterSetter.h"
#include "DeletePropertySlot.h"
#include "Heap.h"
#include "IndexingHeaderInlines.h"
#include "Intrinsic.h"
#include "JSCast.h"
#include "MathCommon.h"
#include "PropertySlot.h"
#include "PropertyStorage.h"
#include "PutDirectIndexMode.h"
#include "PutPropertySlot.h"
#include "Structure.h"
#include "StructureTransitionTable.h"
#include <JavaScriptCore/JSCJSValueCell.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace DOMJIT {
class Signature;
}

inline JSCell* getJSFunction(JSValue); // Defined in JSObjectInlines.h

// SPEC-api 5.7/9.2-6 Thread.restrict choke point (defined in
// runtime/ThreadManager.cpp; duplicate of the runtime/ThreadManager.h
// declaration so generic-path headers need not include it). Returns true if
// the access is allowed; otherwise throws ConcurrentAccessError and returns
// false. Callers gate on isUncacheableDictionary() first (5.7.3).
JS_EXPORT_PRIVATE bool threadRestrictCheck(JSGlobalObject*, JSObject*);

// Same-library seam redeclaration (the threadRestrictCheck pattern above):
// owner is bytecode/JSThreadsSafepoint.h — generic-path headers need not
// include it. Consumed by the M7(c) stabilization loop in
// getOwnNonIndexPropertySlot below so a reader spinning on a torn
// (attributes, value) pair stays park-capable per W1/D9 (the in-flight
// writer may itself be parked by, or be the conductor of, a §A.3 window —
// spinning with heap access held would wedge that window into the 30s
// watchdog fail-stop).
namespace JSThreadsSafepoint {
JS_EXPORT_PRIVATE bool parkSitePollAndParkForStopTheWorld(VM&);
}

class ArrayProfile;
class Exception;
class GetterSetter;
class InternalFunction;
class JSFunction;
class JSString;
class LLIntOffsetsExtractor;
class MarkedBlock;
class ObjectInitializationScope;
class PropertyDescriptor;
class PropertyNameArrayBuilder;
class Structure;
class ThrowScope;
class VM;
struct HashTable;
struct HashTableValue;

JS_EXPORT_PRIVATE Exception* throwTypeError(JSGlobalObject*, ThrowScope&, const String&);
extern JS_EXPORT_PRIVATE const ASCIILiteral NonExtensibleObjectPropertyDefineError;
extern JS_EXPORT_PRIVATE const ASCIILiteral ReadonlyPropertyWriteError;
extern JS_EXPORT_PRIVATE const ASCIILiteral ReadonlyPropertyChangeError;
extern JS_EXPORT_PRIVATE const ASCIILiteral UnableToDeletePropertyError;
extern JS_EXPORT_PRIVATE const ASCIILiteral UnconfigurablePropertyChangeAccessMechanismError;
extern JS_EXPORT_PRIVATE const ASCIILiteral UnconfigurablePropertyChangeConfigurabilityError;
extern JS_EXPORT_PRIVATE const ASCIILiteral UnconfigurablePropertyChangeEnumerabilityError;
extern JS_EXPORT_PRIVATE const ASCIILiteral UnconfigurablePropertyChangeWritabilityError;
extern JS_EXPORT_PRIVATE const ASCIILiteral PrototypeValueCanOnlyBeAnObjectOrNullTypeError;

class JSFinalObject;

#if ASSERT_ENABLED
#define JS_EXPORT_PRIVATE_IF_ASSERT_ENABLED JS_EXPORT_PRIVATE
#else
#define JS_EXPORT_PRIVATE_IF_ASSERT_ENABLED
#endif

#if USE(JSVALUE64)
// SPEC-ungil ANNEX C1 / OM §9.5 atomic slot accessors (U-T10; defined in
// ConcurrentButterfly.cpp). One request descriptor + status word shared by
// the named and indexed §9.5 entry points below. Callers (ThreadAtomics.cpp
// bodies, SPEC-api 4.5 ops; §C.3(a)'s pre-enqueue atomic load) own the
// outer probe loop: every Restart re-runs the WHOLE own-property probe
// (I33-bounded by the forward-only shape order); a completed CAS/RMW is
// NEVER re-applied.
enum class AtomicSlotOperation : uint8_t {
    Load, // seq_cst slot load; never writes (still converts CoW/Int32/Double on the indexed path - first atomic ACCESS converts, §C.1)
    Exchange, // unconditional swap; also serves Atomics.store on an existing slot (value-only, no attribute transition)
    CompareExchangeSVZ, // SameValueZero compare (allocation-free; rope strings bounce out as NeedsStringResolution)
    Add,
    Sub,
    And,
    Or,
    Xor,
};

enum class AtomicSlotStatus : uint8_t {
    Applied, // op done (Load: value read; writes: slot updated + barrier emitted)
    NotEqual, // CompareExchangeSVZ compared unequal; no write; returned value = the value read
    NotNumber, // arithmetic RMW read a non-number; no write; returned value = the value read
    NeedsStringResolution, // CAS needs a rope resolved; no write; caller resolves OUTSIDE any lock (§N.2 single-flight) and restarts the probe
    Restart, // validation failed (structure/shape/offset moved, slot vanished); caller re-runs the whole probe
    LockedRevalidate, // INTERNAL to the §9.5 accessors (U-T10 amend): a named lock-free loop read jsUndefined, which may be a D1 delete-quarantine sentinel (I30); the accessor re-validates under the cell lock. Never escapes to ThreadAtomics callers.
};

struct AtomicSlotRequest {
    AtomicSlotOperation operation { AtomicSlotOperation::Load };
    JSValue expected; // CompareExchangeSVZ
    JSValue replacement; // CompareExchangeSVZ / Exchange
    double operandNumber { 0 }; // Add / Sub
    int32_t operandInt { 0 }; // And / Or / Xor
};
#endif

class JSObject : public JSCell {
    friend class BatchedTransitionOptimizer;
    friend class JIT;
    friend class JSCell;
    friend class JSFinalObject;
    friend class JSObjectWithButterfly;
    friend class MarkedBlock;

    enum PutMode : uint8_t {
        PutModePut,
        PutModeDefineOwnProperty,
    };

public:
    using Base = JSCell;

    DECLARE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE);

    JS_EXPORT_PRIVATE static size_t estimatedSize(JSCell*, VM&);
    JS_EXPORT_PRIVATE static void analyzeHeap(JSCell*, HeapAnalyzer&);

    JS_EXPORT_PRIVATE static String calculatedClassName(JSObject*);

    // This is the fully virtual [[GetPrototypeOf]] internal function defined
    // in the ECMAScript 6 specification. Use this when doing a [[GetPrototypeOf]] 
    // operation as dictated in the specification.
    JSValue getPrototype(JSGlobalObject*);
    JS_EXPORT_PRIVATE static JSValue getPrototype(JSObject*, JSGlobalObject*);
    // This gets the prototype directly off of the structure. This does not do
    // dynamic dispatch on the getPrototype method table method. It is not valid 
    // to use this when performing a [[GetPrototypeOf]] operation in the specification.
    // It is valid to use though when you know that you want to directly get it
    // without consulting the method table. This is akin to getting the [[Prototype]]
    // internal field directly as described in the specification.
    JSValue getPrototypeDirect() const;

    // This sets the prototype without checking for cycles and without
    // doing dynamic dispatch on [[SetPrototypeOf]] operation in the specification.
    // It is not valid to use this when performing a [[SetPrototypeOf]] operation in
    // the specification. It is valid to use though when you know that you want to directly
    // set it without consulting the method table and when you definitely won't
    // introduce a cycle in the prototype chain. This is akin to setting the
    // [[Prototype]] internal field directly as described in the specification.
    JS_EXPORT_PRIVATE void setPrototypeDirect(VM&, JSValue prototype);
private:
    // This is OrdinarySetPrototypeOf in the specification. Section 9.1.2.1
    // https://tc39.github.io/ecma262/#sec-ordinarysetprototypeof
    JS_EXPORT_PRIVATE bool setPrototypeWithCycleCheck(VM&, JSGlobalObject*, JSValue prototype, bool shouldThrowIfCantSet);
public:
    // This is the fully virtual [[SetPrototypeOf]] internal function defined
    // in the ECMAScript 6 specification. Use this when doing a [[SetPrototypeOf]] 
    // operation as dictated in the specification.
    bool setPrototype(VM&, JSGlobalObject*, JSValue prototype, bool shouldThrowIfCantSet = false);
    JS_EXPORT_PRIVATE static bool setPrototype(JSObject*, JSGlobalObject*, JSValue prototype, bool shouldThrowIfCantSet);
        
    inline bool mayInterceptIndexedAccesses();

    inline JSValue get(JSGlobalObject*, PropertyName) const; // Defined in JSObjectInlines.h
    inline JSValue get(JSGlobalObject*, unsigned propertyName) const; // Defined in JSObjectInlines.h
    JSValue get(JSGlobalObject*, uint64_t propertyName) const;

    template<typename T, typename PropertyNameType>
    inline T getAs(JSGlobalObject*, PropertyNameType) const; // Defined in JSObjectInlines.h

    template<bool checkNullStructure = false>
    bool getPropertySlot(JSGlobalObject*, PropertyName, PropertySlot&);
    bool getPropertySlot(JSGlobalObject*, unsigned propertyName, PropertySlot&);
    bool getPropertySlot(JSGlobalObject*, uint64_t propertyName, PropertySlot&);
    template<typename CallbackWhenNoException> typename std::invoke_result<CallbackWhenNoException, bool, PropertySlot&>::type getPropertySlot(JSGlobalObject*, PropertyName, CallbackWhenNoException) const;
    template<typename CallbackWhenNoException> typename std::invoke_result<CallbackWhenNoException, bool, PropertySlot&>::type getPropertySlot(JSGlobalObject*, PropertyName, PropertySlot&, CallbackWhenNoException) const;

    template<typename PropertyNameType> JSValue getIfPropertyExists(JSGlobalObject*, const PropertyNameType&);
    bool noSideEffectMayHaveNonIndexProperty(VM&, PropertyName);

    enum class SortMode { Default, Ascending };
    template<SortMode mode = SortMode::Default, typename Functor>
    void forEachOwnIndexedProperty(JSGlobalObject*, const Functor&);

private:
    static bool getOwnPropertySlotImpl(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
public:
    JS_EXPORT_PRIVATE_IF_ASSERT_ENABLED static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);

    JS_EXPORT_PRIVATE static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned propertyName, PropertySlot&);
    bool getOwnPropertySlotInline(JSGlobalObject*, PropertyName, PropertySlot&);

    // The key difference between this and getOwnPropertySlot is that getOwnPropertySlot
    // currently returns incorrect results for the DOM window (with non-own properties)
    // being returned. Once this is fixed we should migrate code & remove this method.
    JS_EXPORT_PRIVATE bool getOwnPropertyDescriptor(JSGlobalObject*, PropertyName, PropertyDescriptor&);

    static bool getPrivateFieldSlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    inline bool hasPrivateField(JSGlobalObject*, PropertyName);
    inline bool getPrivateField(JSGlobalObject*, PropertyName, PropertySlot&);
    inline void setPrivateField(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    inline void definePrivateField(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    inline bool hasPrivateBrand(JSGlobalObject*, JSValue brand);
    inline void checkPrivateBrand(JSGlobalObject*, JSValue brand);
    inline void setPrivateBrand(JSGlobalObject*, JSValue brand);

    unsigned getArrayLength() const
    {
        if (!hasIndexedProperties(indexingType()))
            return 0;
#if USE(JSVALUE64)
        // SPEC-objectmodel §Q length() dispatch (JSArray::length() routes here):
        // Segmented -> segmentedPublicLength(spine); else one masked load.
        // AS staleness is legal under AS-COPY (§4.6). E5 "None first" (review
        // round 4): the indexed-type check above and this word load are two
        // unfenced loads - the N3 first install is lock-free, so a stale
        // word==0 can pair with a fresh indexed type (arm64 load-load
        // reordering can even satisfy the word load first). Dispatch on the
        // word: None reads as length 0 (the pre-install truth), never a
        // null-8 deref.
        if (Options::useJSThreads()) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            if (isSegmentedButterfly(word)) [[unlikely]]
                return segmentedPublicLength(butterflySpine(word));
            if (!(word & butterflyPointerMask)) [[unlikely]]
                return 0;
            return untaggedButterfly(word)->publicLength();
        }
#endif
        return butterfly()->publicLength();
    }

    unsigned getVectorLength()
    {
        if (!hasIndexedProperties(indexingType()))
            return 0;
#if USE(JSVALUE64)
        // SPEC-objectmodel C4: the loaded spine's vectorLength is authoritative.
        // E5 "None first" (round 4): see getArrayLength above - a racing N3
        // first install can pair word==0 with a fresh indexed type.
        if (Options::useJSThreads()) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            if (isSegmentedButterfly(word)) [[unlikely]]
                return segmentedVectorLength(butterflySpine(word));
            if (!(word & butterflyPointerMask)) [[unlikely]]
                return 0;
            return untaggedButterfly(word)->vectorLength();
        }
#endif
        return butterfly()->vectorLength();
    }
    
    inline bool canHaveExistingOwnIndexedGetterSetterProperties(); // Defined in RenderObjectInlines.h

    // This is only valid after using canPerformFastPropertyEnumerationCommon().
    // This code is not checking getOwnPropertySlot override etc.
    inline unsigned canHaveExistingOwnIndexedProperties() const; // Defined in RenderObjectInlines.h

    static bool putInlineForJSObject(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    
    JS_EXPORT_PRIVATE static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool NODELETE mightBeSpecialProperty(VM&, JSType, UniquedStringImpl*);
    JS_EXPORT_PRIVATE NEVER_INLINE static bool definePropertyOnReceiver(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    // putByIndex assumes that the receiver is this JSCell object.
    JS_EXPORT_PRIVATE static bool putByIndex(JSCell*, JSGlobalObject*, unsigned propertyName, JSValue, bool shouldThrow);
        
    // This performs the ECMAScript Set() operation.
    ALWAYS_INLINE bool putByIndexInline(JSGlobalObject* globalObject, unsigned propertyName, JSValue value, bool shouldThrow)
    {
        VM& vm = getVM(globalObject);
        if (trySetIndexQuickly(vm, propertyName, value))
            return true;
        return methodTable()->putByIndex(this, globalObject, propertyName, value, shouldThrow);
    }

    ALWAYS_INLINE bool putByIndexInline(JSGlobalObject* globalObject, uint64_t propertyName, JSValue value, bool shouldThrow)
    {
        VM& vm = getVM(globalObject);
        if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
            return putByIndexInline(globalObject, static_cast<uint32_t>(propertyName), value, shouldThrow);

        ASSERT(propertyName <= maxSafeInteger());
        PutPropertySlot slot(this, shouldThrow);
        return methodTable()->put(this, globalObject, Identifier::from(vm, propertyName), value, slot);
    }
        
    // This is similar to the putDirect* methods:
    //  - the prototype chain is not consulted
    //  - accessors are not called.
    //  - it will ignore extensibility and read-only properties if PutDirectIndexLikePutDirect is passed as the mode (the default).
    // This method creates a property with attributes writable, enumerable and configurable all set to true if attributes is zero,
    // otherwise, it creates a property with the provided attributes. Semantically, this is performing defineOwnProperty.
    bool putDirectIndex(JSGlobalObject* globalObject, unsigned propertyName, JSValue value, unsigned attributes, PutDirectIndexMode mode)
    {
        ASSERT(!value.isCustomGetterSetterSlow());
        auto canSetIndexQuicklyForPutDirect = [&] () -> bool {
            switch (indexingMode()) {
            case ALL_BLANK_INDEXING_TYPES:
            case ALL_UNDECIDED_INDEXING_TYPES:
                return false;
            case ALL_WRITABLE_INT32_INDEXING_TYPES:
            case ALL_WRITABLE_DOUBLE_INDEXING_TYPES:
            case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES:
            case ALL_ARRAY_STORAGE_INDEXING_TYPES:
#if USE(JSVALUE64)
                // SPEC-objectmodel §10.7/§Q (review round 1): flag-on, this
                // bound read needs the regime dispatch - butterfly() is the
                // FLAT-only accessor, so a segmented word would read the
                // spine as an IndexingHeader (type-confused bound). Segmented
                // and AS words report "not quickly": the slow path
                // (putDirectIndexSlowOrBeyondVectorLength) runs the routed /
                // I31-locked generic logic, and the segmented setIndexQuickly
                // shape-conversion hazard never arises from here. Flat words
                // read the bound from the SAME loaded word.
                if (Options::useJSThreads()) [[unlikely]] {
                    uint64_t word = taggedButterflyWord();
                    if (isSegmentedButterfly(word) || hasAnyArrayStorage(indexingType()))
                        return false;
                    if (!(word & butterflyPointerMask)) [[unlikely]]
                        return false; // E5 None-first (round 4): racing N3 first install => slow path.
                    return propertyName < untaggedButterfly(word)->vectorLength();
                }
#endif
                return propertyName < butterfly()->vectorLength();
            default:
                if (isCopyOnWrite(indexingMode()))
                    return false;
                RELEASE_ASSERT_NOT_REACHED();
                return false;
            }
        };
        
        if (!attributes && canSetIndexQuicklyForPutDirect()) {
            setIndexQuickly(getVM(globalObject), propertyName, value);
            return true;
        }
        return putDirectIndexSlowOrBeyondVectorLength(globalObject, propertyName, value, attributes, mode);
    }
    // This is semantically equivalent to performing defineOwnProperty(propertyName, {configurable:true, writable:true, enumerable:true, value:value}).
    bool putDirectIndex(JSGlobalObject* globalObject, unsigned propertyName, JSValue value)
    {
        return putDirectIndex(globalObject, propertyName, value, 0, PutDirectIndexLikePutDirect);
    }

    ALWAYS_INLINE bool putDirectIndex(JSGlobalObject* globalObject, uint64_t propertyName, JSValue value, unsigned attributes, PutDirectIndexMode mode)
    {
        if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
            return putDirectIndex(globalObject, static_cast<uint32_t>(propertyName), value, attributes, mode);
        return putDirect(getVM(globalObject), Identifier::from(getVM(globalObject), propertyName), value, attributes);
    }

    // A generally non-throwing version of putDirect and putDirectIndex.
    // However, it's only guaranteed to not throw based on what the receiver is.
    // For example, if the receiver is a ProxyObject, this is not guaranteed, since
    // it may call into arbitrary JS code. It's the responsibility of the user of
    // this API to ensure that the receiver object is a well known type if they
    // want to ensure that this won't throw an exception.
    JS_EXPORT_PRIVATE bool putDirectMayBeIndex(JSGlobalObject*, PropertyName, JSValue);
        
    bool hasIndexingHeader() const
    {
        return structure()->hasIndexingHeader(this);
    }

    bool canGetIndexQuicklyForTypedArray(unsigned) const;
    JSValue getIndexQuicklyForTypedArray(unsigned, ArrayProfile* = nullptr) const;
    
    // SPEC-objectmodel §Q (history §15.6): the quickly-family calls butterfly()
    // inside this owned header and is invisible to the §10.7 guard grep, so the
    // regime dispatch is INTERNAL and flag-on only: Segmented -> bounds-checked
    // fragment slots (C4/I33); ArrayStorage shape -> can*Quickly report false so
    // callers fall to their generic paths (E5 dispatch = §4.6 cell lock); Flat ->
    // mask + today's code, with the vectorLength bound read from the SAME loaded
    // butterfly. Flag-off => identity (I22).
    bool canGetIndexQuickly(unsigned i) const
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return canGetIndexQuicklyConcurrent(i);
#endif
        const Butterfly* butterfly = this->butterfly();
        switch (indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
            return canGetIndexQuicklyForTypedArray(i);
        case ALL_UNDECIDED_INDEXING_TYPES:
            return false;
        case ALL_INT32_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            return i < butterfly->vectorLength() && butterfly->contiguous().at(this, i);
        case ALL_DOUBLE_INDEXING_TYPES: {
            if (i >= butterfly->vectorLength())
                return false;
            double value = butterfly->contiguousDouble().at(this, i);
            if (value != value)
                return false;
            return true;
        }
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            return i < butterfly->arrayStorage()->vectorLength() && butterfly->arrayStorage()->m_vector[i];
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return false;
        }
    }

    bool canGetIndexQuickly(uint64_t i) const
    {
        ASSERT(i <= maxSafeInteger());
        if (i <= MAX_ARRAY_INDEX) [[likely]]
            return canGetIndexQuickly(static_cast<uint32_t>(i));
        return false;
    }
        
    JSValue getIndexQuickly(unsigned i) const
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return getIndexQuicklyConcurrent(i);
#endif
        const Butterfly* butterfly = this->butterfly();
        switch (indexingType()) {
        case ALL_INT32_INDEXING_TYPES:
            return jsNumber(butterfly->contiguous().at(this, i).get().asInt32());
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            return butterfly->contiguous().at(this, i).get();
        case ALL_DOUBLE_INDEXING_TYPES:
            return JSValue(JSValue::EncodeAsDouble, butterfly->contiguousDouble().at(this, i));
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            return butterfly->arrayStorage()->m_vector[i].get();
        case ALL_BLANK_INDEXING_TYPES:
            return getIndexQuicklyForTypedArray(i);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return JSValue();
        }
    }

    // Uses the (optional) array profile to set the m_mayBeLargeTypedArray bit when relevant
    JSValue tryGetIndexQuickly(unsigned i, ArrayProfile* arrayProfile = nullptr) const
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return tryGetIndexQuicklyConcurrent(i, arrayProfile);
#endif
        const Butterfly* butterfly = this->butterfly();
        switch (indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
            if (canGetIndexQuicklyForTypedArray(i))
                return getIndexQuicklyForTypedArray(i, arrayProfile);
            break;
        case ALL_UNDECIDED_INDEXING_TYPES:
            break;
        case ALL_INT32_INDEXING_TYPES:
            if (i < butterfly->publicLength()) {
                JSValue result = butterfly->contiguous().at(this, i).get();
                ASSERT(result.isInt32() || !result);
                return result;
            }
            break;
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            if (i < butterfly->publicLength())
                return butterfly->contiguous().at(this, i).get();
            break;
        case ALL_DOUBLE_INDEXING_TYPES: {
            if (i >= butterfly->publicLength())
                break;
            double result = butterfly->contiguousDouble().at(this, i);
            if (result != result)
                break;
            return JSValue(JSValue::EncodeAsDouble, result);
        }
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            if (i < butterfly->arrayStorage()->vectorLength())
                return butterfly->arrayStorage()->m_vector[i].get();
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        return JSValue();
    }

    JSValue tryGetIndexQuickly(uint64_t i) const
    {
        ASSERT(i <= maxSafeInteger());
        if (i <= MAX_ARRAY_INDEX) [[likely]]
            return tryGetIndexQuickly(static_cast<uint32_t>(i));
        return JSValue();
    }
        
    JSValue getDirectIndex(JSGlobalObject* globalObject, unsigned i)
    {
        if (JSValue result = tryGetIndexQuickly(i))
            return result;
        PropertySlot slot(this, PropertySlot::InternalMethodType::Get);
        if (methodTable()->getOwnPropertySlotByIndex(this, globalObject, i, slot))
            return slot.getValue(globalObject, i);
        return JSValue();
    }
        
    JSValue getIndex(JSGlobalObject* globalObject, uint64_t i) const
    {
        if (JSValue result = tryGetIndexQuickly(i))
            return result;
        return get(globalObject, i);
    }

    void setIndexQuicklyForTypedArray(unsigned, JSValue);
    void setIndexQuicklyForArrayStorageIndexingType(VM&, unsigned, JSValue);

    // Return true to indicate success
    // Use the (optional) array profile to set the m_mayBeLargeTypedArray bit when relevant
    bool trySetIndexQuicklyForTypedArray(unsigned, JSValue, ArrayProfile*);
    bool trySetIndexQuickly(VM& vm, unsigned i, JSValue v, ArrayProfile* arrayProfile = nullptr)
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return trySetIndexQuicklyConcurrent(vm, i, v, arrayProfile);
#endif
        Butterfly* butterfly = this->butterfly();
        switch (indexingMode()) {
        case ALL_BLANK_INDEXING_TYPES:
            return trySetIndexQuicklyForTypedArray(i, v, arrayProfile);
        case ALL_UNDECIDED_INDEXING_TYPES:
            return false;
        case ALL_WRITABLE_INT32_INDEXING_TYPES: {
            if (i >= butterfly->vectorLength())
                return false;
            if (!v.isInt32()) {
                convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(vm, i, v);
                return true;
            }
            [[fallthrough]];
        }
        case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES: {
            if (i >= butterfly->vectorLength())
                return false;
            butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
            if (i >= butterfly->publicLength())
                butterfly->setPublicLength(i + 1);
            vm.writeBarrier(this, v);
            return true;
        }
        case ALL_WRITABLE_DOUBLE_INDEXING_TYPES: {
            if (i >= butterfly->vectorLength())
                return false;
            if (!v.isNumber()) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return true;
            }
            double value = v.asNumber();
            if (value != value) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return true;
            }
            butterfly->contiguousDouble().at(this, i) = value;
            if (i >= butterfly->publicLength())
                butterfly->setPublicLength(i + 1);
            return true;
        }
        case NonArrayWithArrayStorage:
        case ArrayWithArrayStorage:
            if (i >= butterfly->vectorLength())
                return false;
            setIndexQuicklyForArrayStorageIndexingType(vm, i, v);
            return true;
        case NonArrayWithSlowPutArrayStorage:
        case ArrayWithSlowPutArrayStorage:
            if (i >= butterfly->arrayStorage()->vectorLength() || !butterfly->arrayStorage()->m_vector[i])
                return false;
            setIndexQuicklyForArrayStorageIndexingType(vm, i, v);
            return true;
        default:
            RELEASE_ASSERT(isCopyOnWrite(indexingMode()));
            return false;
        }
    }

    void setIndexQuickly(VM& vm, unsigned i, JSValue v)
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            setIndexQuicklyConcurrent(vm, i, v);
            return;
        }
#endif
        Butterfly* butterfly = this->butterfly();
        ASSERT(!isCopyOnWrite(indexingMode()));
        switch (indexingType()) {
        case ALL_INT32_INDEXING_TYPES: {
            ASSERT(i < butterfly->vectorLength());
            if (!v.isInt32()) {
                convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            [[fallthrough]];
        }
        case ALL_CONTIGUOUS_INDEXING_TYPES: {
            ASSERT(i < butterfly->vectorLength());
            butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
            if (i >= butterfly->publicLength())
                butterfly->setPublicLength(i + 1);
            vm.writeBarrier(this, v);
            break;
        }
        case ALL_DOUBLE_INDEXING_TYPES: {
            ASSERT(i < butterfly->vectorLength());
            if (!v.isNumber()) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            double value = v.asNumber();
            if (value != value) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            butterfly->contiguousDouble().at(this, i) = value;
            if (i >= butterfly->publicLength())
                butterfly->setPublicLength(i + 1);
            break;
        }
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            setIndexQuicklyForArrayStorageIndexingType(vm, i, v);
            break;
        case ALL_BLANK_INDEXING_TYPES:
            setIndexQuicklyForTypedArray(i, v);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    inline void initializeIndex(ObjectInitializationScope&, unsigned, JSValue); // Defined in JSObjectInlines.h

    // NOTE: Clients of this method may call it more than once for any index, and this is supposed
    // to work.
    ALWAYS_INLINE void initializeIndex(ObjectInitializationScope&, unsigned, JSValue, IndexingType); // Defined in JSObjectInlines.h

    inline void initializeIndexWithoutBarrier(ObjectInitializationScope&, unsigned, JSValue); // Defined in JSObjectInlines.h

    // This version of initializeIndex is for cases where you know that you will not need any
    // barriers. This implies not having any data format conversions.
    ALWAYS_INLINE void initializeIndexWithoutBarrier(ObjectInitializationScope&, unsigned, JSValue, IndexingType); // Defined in JSObjectInlines.h
        
    bool hasSparseMap()
    {
        switch (indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
        case ALL_UNDECIDED_INDEXING_TYPES:
        case ALL_INT32_INDEXING_TYPES:
        case ALL_DOUBLE_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            return false;
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            return !!butterfly()->arrayStorage()->m_sparseMap;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return false;
        }
    }
        
    bool inSparseIndexingMode()
    {
        switch (indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
        case ALL_UNDECIDED_INDEXING_TYPES:
        case ALL_INT32_INDEXING_TYPES:
        case ALL_DOUBLE_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            return false;
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            return butterfly()->arrayStorage()->inSparseMode();
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return false;
        }
    }
        
    void enterDictionaryIndexingMode(VM&);

    // putDirect is effectively an unchecked vesion of 'defineOwnProperty':
    //  - the prototype chain is not consulted
    //  - accessors are not called.
    //  - attributes will be respected (after the call the property will exist with the given attributes)
    //  - the property name is assumed to not be an index.
    bool putDirect(VM&, PropertyName, JSValue, unsigned attributes = 0);
    bool putDirect(VM&, PropertyName, JSValue, unsigned attributes, PutPropertySlot&);
    bool putDirect(VM&, PropertyName, JSValue, PutPropertySlot&);
    void putDirectWithoutTransition(VM&, PropertyName, JSValue, unsigned attributes = 0);
    bool putDirectNonIndexAccessor(VM&, PropertyName, GetterSetter*, unsigned attributes);
    void putDirectNonIndexAccessorWithoutTransition(VM&, PropertyName, GetterSetter*, unsigned attributes);
    bool putDirectAccessor(JSGlobalObject*, PropertyName, GetterSetter*, unsigned attributes);
    JS_EXPORT_PRIVATE bool putDirectCustomAccessor(VM&, PropertyName, JSValue, unsigned attributes);
    void putDirectCustomGetterSetterWithoutTransition(VM&, PropertyName, JSValue, unsigned attributes);

    bool putGetter(JSGlobalObject*, PropertyName, JSValue, unsigned attributes);
    bool putSetter(JSGlobalObject*, PropertyName, JSValue, unsigned attributes);

    JS_EXPORT_PRIVATE bool hasProperty(JSGlobalObject*, PropertyName) const;
    JS_EXPORT_PRIVATE bool hasProperty(JSGlobalObject*, unsigned propertyName) const;
    bool hasProperty(JSGlobalObject*, uint64_t propertyName) const;
    bool hasEnumerableProperty(JSGlobalObject*, PropertyName) const;
    bool hasEnumerableProperty(JSGlobalObject*, unsigned propertyName) const;
    bool hasOwnProperty(JSGlobalObject*, PropertyName, PropertySlot&) const;
    bool hasOwnProperty(JSGlobalObject*, PropertyName) const;
    bool hasOwnProperty(JSGlobalObject*, unsigned) const;

    JS_EXPORT_PRIVATE static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    JS_EXPORT_PRIVATE static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned propertyName);
    bool deleteProperty(JSGlobalObject*, PropertyName);
    bool deleteProperty(JSGlobalObject*, uint32_t propertyName);
    bool deleteProperty(JSGlobalObject*, uint64_t propertyName);

    JSValue ordinaryToPrimitive(JSGlobalObject*, PreferredPrimitiveType) const;

    JS_EXPORT_PRIVATE bool hasInstance(JSGlobalObject*, JSValue value, JSValue hasInstanceValue);
    JS_EXPORT_PRIVATE bool hasInstance(JSGlobalObject*, JSValue);
    static bool defaultHasInstance(JSGlobalObject*, JSValue, JSValue prototypeProperty);

    static constexpr unsigned maximumPrototypeChainDepth = 40000;
    JS_EXPORT_PRIVATE void getPropertyNames(JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE static void NODELETE getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE void getOwnIndexedPropertyNames(JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE void getOwnNonIndexPropertyNames(JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    void getNonReifiedStaticPropertyNames(VM&, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

    JS_EXPORT_PRIVATE uint32_t getEnumerableLength();

    JS_EXPORT_PRIVATE JSValue toPrimitive(JSGlobalObject*, PreferredPrimitiveType = NoPreference) const;
    JS_EXPORT_PRIVATE double toNumber(JSGlobalObject*) const;
    JS_EXPORT_PRIVATE JSString* toString(JSGlobalObject*) const;

    // This get function only looks at the property map.
    JSValue getDirect(VM& vm, PropertyName propertyName) const
    {
        Structure* structure = this->structure();
        PropertyOffset offset = structure->get(vm, propertyName);
        checkOffset(offset, structure->inlineCapacity());
        return offset != invalidOffset ? getDirect(offset) : JSValue();
    }
    
    JSValue getDirect(VM& vm, PropertyName propertyName, unsigned& attributes) const
    {
        Structure* structure = this->structure();
        PropertyOffset offset = structure->get(vm, propertyName, attributes);
        checkOffset(offset, structure->inlineCapacity());
        return offset != invalidOffset ? getDirect(offset) : JSValue();
    }

    PropertyOffset getDirectOffset(VM& vm, PropertyName propertyName)
    {
        Structure* structure = this->structure();
        PropertyOffset offset = structure->get(vm, propertyName);
        checkOffset(offset, structure->inlineCapacity());
        return offset;
    }

    PropertyOffset getDirectOffset(VM& vm, PropertyName propertyName, unsigned& attributes)
    {
        Structure* structure = this->structure();
        PropertyOffset offset = structure->get(vm, propertyName, attributes);
        checkOffset(offset, structure->inlineCapacity());
        return offset;
    }

    bool hasInlineStorage() const { return structure()->hasInlineStorage(); }
    ConstPropertyStorage inlineStorageUnsafe() const
    {
        return std::bit_cast<ConstPropertyStorage>(std::bit_cast<const char*>(this) + offsetOfInlineStorage());
    }
    PropertyStorage inlineStorageUnsafe()
    {
        return std::bit_cast<PropertyStorage>(std::bit_cast<char*>(this) + offsetOfInlineStorage());
    }
    ConstPropertyStorage inlineStorage() const
    {
        ASSERT(hasInlineStorage());
        return inlineStorageUnsafe();
    }
    PropertyStorage inlineStorage()
    {
        ASSERT(hasInlineStorage());
        return inlineStorageUnsafe();
    }

    const Butterfly* butterfly() const LIFETIME_BOUND
    {
        return const_cast<JSObject*>(this)->butterfly();
    }

    Butterfly* butterfly() LIFETIME_BOUND
    {
        // Access m_butterfly field of JSObjectWithButterfly regardless of whether this object is a derived class of JSObjectWithButterfly.
        // This is safe as atom of GC heap allocation is 16 bytes, thus the butterfly field, offset from 8 byte, is always accessible.
        // We intentionally load it regardless to make this function branchless. This is critical to keep this fast while we have butterfly-less objects.
#if USE(JSVALUE64)
        // SPEC-objectmodel §9.5: with Options::useJSThreads() the butterfly word
        // carries the §2 tag (bit 63 = SW, bits 62..48 = installing thread's TID)
        // in its high 16 bits; butterfly() masks the tag off on load. CONTRACT —
        // callers must have established flatness via one of: flag off | a
        // dominating §2/§3/E5 regime dispatch | the class never segments
        // (ArrayStorage I31, CopyOnWrite I35) | a §10.7 mayBeSegmentedButterfly()
        // guard. Segmented words (TID == notTTLTID, payload = ButterflySpine*)
        // must never be dereferenced through this accessor.
        if (Options::useJSThreads()) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            ASSERT(!isSegmentedButterfly(word));
            if (verifyConcurrentButterflyEnabled()) [[unlikely]]
                RELEASE_ASSERT(!isSegmentedButterfly(word));
            return untaggedButterfly(word);
        }
#endif
        auto* b = *std::bit_cast<Butterfly**>(std::bit_cast<char*>(this) + butterflyOffset());
        if (type() == WebAssemblyGCObjectType) [[unlikely]]
            b = nullptr;
        return b;
    }

    // SPEC-objectmodel §9.5 word-level accessors. Flag-off every flat tag is
    // all-zero (I22), so taggedButterflyWord() == the raw pointer bits.
    // TSAN-TRIAGE §3.15 (butterfly-words): this load races concurrent installs
    // (AuxiliaryBarrier::setWithoutBarrier / setButterflyConcurrent DCAS). The
    // value race is spec-blessed (C4 bounds, §3 re-dispatch on divergence) but
    // a plain C++ load is UB, so it is a RELAXED atomic load — identical
    // codegen to the plain load on x86-64/arm64 (flag-off unchanged).
    ALWAYS_INLINE uint64_t taggedButterflyWord() const // raw 64-bit load, never masked
    {
#if USE(JSVALUE64)
        uint64_t word = butterflyConcurrentLoad(std::bit_cast<const uint64_t*>(std::bit_cast<const char*>(this) + butterflyOffset()));
        if (type() == WebAssemblyGCObjectType) [[unlikely]]
            word = 0;
        return word;
#else
        return static_cast<uint64_t>(std::bit_cast<uintptr_t>(const_cast<JSObject*>(this)->butterfly()));
#endif
    }

    ButterflyRegime butterflyRegime() const { return butterflyRegimeForWord(taggedButterflyWord()); }

    ALWAYS_INLINE bool mayBeSegmentedButterfly() const // one load + compare; constant false flag-off (I22)
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return isSegmentedButterfly(taggedButterflyWord());
#endif
        return false;
    }

    ALWAYS_INLINE bool isSharedArrayStorage() const // SW=1 && AS shape; §4.6 dispatch keys on shape ANY SW
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return butterflySharedWrite(taggedButterflyWord()) && hasAnyArrayStorage(indexingType());
#endif
        return false;
    }

    // SPEC-objectmodel §Q: outOfLineStorage() is FLAT-ONLY (butterfly() contract);
    // regime-safe out-of-line access goes through locationForOffset().
    ConstPropertyStorage outOfLineStorage() const { return butterfly()->propertyStorage(); }
    PropertyStorage outOfLineStorage() { return butterfly()->propertyStorage(); }

    // SPEC-objectmodel §Q: flag-on, out-of-line offsets dispatch on the butterfly
    // regime (M7(d) loadLoadFence + I33 bound, in locationForOutOfLineOffsetConcurrent),
    // which makes the whole getDirect/getDirectOffset/putDirectOffset/
    // putDirectWithoutBarrier family regime-safe and I24-conforming for ALL
    // callers, with no per-call-site guards. Flag-off the branch is dead (I22).
    // Inline offsets never touch the butterfly and are atomic for free.
    ALWAYS_INLINE const WriteBarrierBase<Unknown>* locationForOffset(PropertyOffset offset) const
    {
        if (isInlineOffset(offset))
            return &inlineStorage()[offsetInInlineStorage(offset)];
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return locationForOutOfLineOffsetConcurrent(offset);
#endif
        return &outOfLineStorage()[offsetInOutOfLineStorage(offset)];
    }

    ALWAYS_INLINE WriteBarrierBase<Unknown>* locationForOffset(PropertyOffset offset)
    {
        if (isInlineOffset(offset))
            return &inlineStorage()[offsetInInlineStorage(offset)];
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]]
            return const_cast<WriteBarrierBase<Unknown>*>(locationForOutOfLineOffsetConcurrent(offset));
#endif
        return &outOfLineStorage()[offsetInOutOfLineStorage(offset)];
    }

    void transitionTo(VM&, Structure*);

    bool hasCustomProperties() { return structure()->didTransition(); }

    bool hasNonReifiedStaticProperties()
    {
        return TypeInfo::hasStaticPropertyTable(inlineTypeFlags()) && !structure()->staticPropertiesReified();
    }

    // putOwnDataProperty has 'put' like semantics, however this method:
    //  - assumes the object contains no own getter/setter properties.
    //  - provides no special handling for __proto__
    //  - does not walk the prototype chain (to check for accessors or non-writable properties).
    // This is used by JSLexicalEnvironment.
    bool putOwnDataProperty(VM&, PropertyName, JSValue, PutPropertySlot&);
    bool putOwnDataPropertyMayBeIndex(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);

    void putOwnDataPropertyBatching(VM&, UniquedStringImpl**, const EncodedJSValue*, unsigned size);
private:
    void validatePutOwnDataProperty(VM&, PropertyName, JSValue);
public:

    // Fast access to known property offsets.
    ALWAYS_INLINE JSValue getDirect(PropertyOffset offset) const { return locationForOffset(offset)->get(); }
    JSValue getDirect(Locker<JSCellLock>&, Concurrency, Structure* expectedStructure, PropertyOffset) const;
    JSValue getDirectConcurrently(Locker<JSCellLock>&, Structure* expectedStructure, PropertyOffset) const;
    void putDirectOffset(VM& vm, PropertyOffset offset, JSValue value) { locationForOffset(offset)->set(vm, this, value); }
    void putDirectWithoutBarrier(PropertyOffset offset, JSValue value) { locationForOffset(offset)->setWithoutWriteBarrier(value); }

#if USE(JSVALUE64)
    // SPEC-ungil ANNEX C1 / OM §9.5 (U-T10; defined in ConcurrentButterfly.cpp):
    // atomic CAS/RMW/load on plain structure/butterfly-backed own NAMED data
    // slots + the indexed pair. Arms (§C.1):
    //   - lock-free (inline, flat out-of-line, segmented fragment): seq_cst
    //     64-bit CAS/RMW loop on the EncodedJSValue slot word; NO cell lock on
    //     the segmented arm (a lock-held RMW would not serialize vs lock-free
    //     fragment stores, U5);
    //   - flat-path SW discipline: foreign writer on an SW=0 flat word runs
    //     the §2/§3 SW-set DCAS (ensureSharedWriteBit) FIRST, then re-validates
    //     structureID + butterfly per I34 before the slot CAS; validation
    //     failure => Restart (whole-probe);
    //   - third arm (OM-locked regimes): dictionary (I19/L3) and AS-shape
    //     (§4.6) probe + CAS/RMW UNDER the JSCellLock, with the AS PRE-LOCK SW
    //     protocol (r8 item 6) for foreign writers while SW=0; dictionary-ness
    //     and the offset are re-checked under the lock (dictionary delete is
    //     I34-blind, U5);
    //   - indexed, by shape (8g re-freeze): CoW materializes first (§4.8/I35);
    //     Int32/Double CONVERT to Contiguous on first atomic access (owner
    //     direct, foreign SW-set DCAS first); Contiguous = flat arm verbatim;
    //     ArrayStorage/dict-indexed = third arm.
    // Write barrier after success, as §9.5 orders. expectedStructureID is the
    // caller's probe provenance (named form only; the indexed form re-dispatches
    // on the live shape). D7: writability is re-validated inside the locked arm
    // (Restart on mismatch); the lock-free arms pin it via expectedStructureID.
    JS_EXPORT_PRIVATE JSValue atomicSlotReadModifyWrite(JSGlobalObject*, UniquedStringImpl*, PropertyOffset, StructureID expectedStructureID, const AtomicSlotRequest&, AtomicSlotStatus&);
    JS_EXPORT_PRIVATE JSValue atomicSlotReadModifyWriteAtIndex(JSGlobalObject*, unsigned index, const AtomicSlotRequest&, AtomicSlotStatus&);
    JSValue atomicSlotCompareExchange(JSGlobalObject* globalObject, UniquedStringImpl* uid, PropertyOffset offset, StructureID expectedStructureID, JSValue expected, JSValue replacement, AtomicSlotStatus& status)
    {
        AtomicSlotRequest request;
        request.operation = AtomicSlotOperation::CompareExchangeSVZ;
        request.expected = expected;
        request.replacement = replacement;
        return atomicSlotReadModifyWrite(globalObject, uid, offset, expectedStructureID, request, status);
    }
    JSValue atomicSlotCompareExchangeAtIndex(JSGlobalObject* globalObject, unsigned index, JSValue expected, JSValue replacement, AtomicSlotStatus& status)
    {
        AtomicSlotRequest request;
        request.operation = AtomicSlotOperation::CompareExchangeSVZ;
        request.expected = expected;
        request.replacement = replacement;
        return atomicSlotReadModifyWriteAtIndex(globalObject, index, request, status);
    }
    // U-T10 amend (§C.2 Missing arm): conditional add for Atomics.store on a
    // probed-Missing NAMED key. PutModePut semantics through the flag-on
    // concurrent putDirectInternal machinery: the add publishes via the E4
    // structureID CAS against the loop-iteration structure, extensibility is
    // re-checked in the SAME iteration, and a property that materialized
    // since the probe is never attribute-clobbered - an accessor/ReadOnly
    // racer returns a non-null error (caller restarts its probe, which then
    // throws the precise D3/D7/non-extensible TypeError); a plain writable
    // data racer takes the value-only replace leg (attributes preserved -
    // putExistingOwnDataPropertyForAtomics' hazard cannot occur). Defined in
    // ConcurrentButterfly.cpp (U-T10-owned).
    JS_EXPORT_PRIVATE ASCIILiteral putDirectForAtomicsMissingAdd(VM&, PropertyName, JSValue, PutPropertySlot&);
    // U-T10 amend (§C.2 Missing arm, INDEXED leg): the indexed twin of the
    // helper above; closes the KNOWN RESIDUAL recorded in INTEGRATE-ungil
    // (U-T10 amend item 3). Same contract: null on success; a non-null error
    // means the add LOST a race with a concurrent indexed define/remove/
    // reshape — the caller restarts its probe, which re-classifies on
    // settled state and throws the precise TypeError or takes the value-only
    // Exchange leg. Never writes a sparse-map entry it did not freshly
    // create; the conditional add and the value publish share ONE
    // object-cellLock window, the same lock defineOwnIndexedProperty holds
    // around its own map->add, so isNewEntry alone decides the winner. CAN
    // throw (exotic-receiver generic fallback, conversion OOM): callers must
    // check for an exception before testing the returned error. Defined in
    // ThreadAtomics.cpp (its only caller; ConcurrentButterfly.cpp is owned
    // by the object-model workstream).
    JS_EXPORT_PRIVATE ASCIILiteral putDirectIndexForAtomicsMissingAdd(JSGlobalObject*, uint32_t index, JSValue);
#endif

    JS_EXPORT_PRIVATE bool putDirectNativeIntrinsicGetter(VM&, JSGlobalObject*, Identifier, NativeFunction, Intrinsic, unsigned attributes);
    JS_EXPORT_PRIVATE void putDirectNativeIntrinsicGetterWithoutTransition(VM&, JSGlobalObject*, Identifier, NativeFunction, Intrinsic, unsigned attributes);
    JS_EXPORT_PRIVATE bool putDirectNativeFunction(VM&, JSGlobalObject*, const PropertyName&, unsigned functionLength, NativeFunction, ImplementationVisibility, Intrinsic, unsigned attributes);
    JS_EXPORT_PRIVATE bool putDirectNativeFunction(VM&, JSGlobalObject*, const PropertyName&, unsigned functionLength, NativeFunction, ImplementationVisibility, Intrinsic, const DOMJIT::Signature*, unsigned attributes);
    JS_EXPORT_PRIVATE void putDirectNativeFunctionWithoutTransition(VM&, JSGlobalObject*, const PropertyName&, unsigned functionLength, NativeFunction, ImplementationVisibility, Intrinsic, unsigned attributes);

    JS_EXPORT_PRIVATE JSFunction* putDirectBuiltinFunction(VM&, JSGlobalObject*, const PropertyName&, FunctionExecutable*, unsigned attributes);
    JSFunction* putDirectBuiltinFunctionWithoutTransition(VM&, JSGlobalObject*, const PropertyName&, FunctionExecutable*, unsigned attributes);

    JS_EXPORT_PRIVATE static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    bool createDataProperty(JSGlobalObject*, PropertyName, JSValue, bool shouldThrow);

    bool isEnvironment() const;
    bool isGlobalObject() const;
    bool isJSLexicalEnvironment() const;
    bool isGlobalLexicalEnvironment() const;
    bool isStrictEvalActivation() const;
    bool isWithScope() const;

    bool isErrorInstance() const;

    JS_EXPORT_PRIVATE void seal(VM&);
    JS_EXPORT_PRIVATE void freeze(VM&);
    JS_EXPORT_PRIVATE static bool preventExtensions(JSObject*, JSGlobalObject*);
    JS_EXPORT_PRIVATE static bool NODELETE isExtensible(JSObject*, JSGlobalObject*);
    bool isSealed(VM& vm) { return structure()->isSealed(vm); }
    bool isFrozen(VM& vm) { return structure()->isFrozen(vm); }

    JS_EXPORT_PRIVATE bool NODELETE anyObjectInChainMayInterceptIndexedAccesses() const;
    bool NODELETE needsSlowPutIndexing() const;

private:
    TransitionKind NODELETE suggestedArrayStorageTransition() const;
public:
    // You should only call isStructureExtensible() when:
    // - Performing this check in a way that isn't described in the specification 
    //   as calling the virtual [[IsExtensible]] trap.
    // - When you're guaranteed that object->methodTable()->isExtensible isn't
    //   overridden.
    ALWAYS_INLINE bool isStructureExtensible() { return structure()->isStructureExtensible(); }
    // You should call this when performing [[IsExtensible]] trap in a place
    // that is described in the specification. This performs the fully virtual
    // [[IsExtensible]] trap.
    bool isExtensible(JSGlobalObject*);
    bool indexingShouldBeSparse()
    {
        return !isStructureExtensible()
            || structure()->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero();
    }

    bool staticPropertiesReified() { return structure()->staticPropertiesReified(); }
    void reifyAllStaticProperties(JSGlobalObject*);

    JS_EXPORT_PRIVATE Butterfly* allocateMoreOutOfLineStorage(VM&, size_t oldSize, size_t newSize);

    // Call this when you do not need to change the structure.
    inline void setButterfly(VM&, Butterfly*); // Defined in JSObjectInlines.h

    // Call this if you do need to change the structure, or if you changed something about a structure
    // in-place.
    inline void nukeStructureAndSetButterfly(VM&, StructureID oldStructureID, Butterfly*); // Defined in JSObjectInlines.h

    void setStructure(VM&, Structure*);

    JS_EXPORT_PRIVATE void convertToDictionary(VM&);
    JS_EXPORT_PRIVATE void convertToUncacheableDictionary(VM&);

    void flattenDictionaryObject(VM& vm)
    {
        structure()->flattenDictionaryStructure(vm, this);
    }
    void shiftButterflyAfterFlattening(const GCSafeConcurrentJSLocker&, VM&, Structure* structure, size_t outOfLineCapacityAfter);

    JSGlobalObject* realmMayBeNull() const
    {
        return structure()->realm();
    }

#if USE(BUN_JSC_ADDITIONS)
    // Compat alias: upstream renamed JSObject::globalObject() -> realm() in 7d4583947a7b.
    // Preserves old (nullable) semantics for Bun's existing call sites.
    JSGlobalObject* globalObject() const { return realmMayBeNull(); }
#endif

    JSGlobalObject* realm() const
    {
        SUPPRESS_FORWARD_DECL_ARG auto* result = realmMayBeNull();
        RELEASE_ASSERT(result, "Do not call JSObject::realm() on objects with realmless structures (e.g., WebAssembly GC objects)");
        ASSERT(!isGlobalObject() || ((JSObject*)result) == this);
        return result;
    }

    void switchToSlowPutArrayStorage(VM&);
        
    // The receiver is the prototype in this case. The following:
    //
    // asObject(foo->structure()->storedPrototype())->attemptToInterceptPutByIndexOnHoleForPrototype(...)
    //
    // is equivalent to:
    //
    // foo->attemptToInterceptPutByIndexOnHole(...);
    bool attemptToInterceptPutByIndexOnHoleForPrototype(JSGlobalObject*, JSValue thisValue, unsigned propertyName, JSValue, bool shouldThrow, bool& putResult);
        
    // Returns 0 if int32 storage cannot be created - either because
    // indexing should be sparse, we're having a bad time, or because
    // we already have a more general form of storage (double,
    // contiguous, array storage).
    ContiguousJSValues tryMakeWritableInt32(VM& vm)
    {
        if (hasInt32(indexingType()) && !isCopyOnWrite(indexingMode())) [[likely]] {
#if USE(JSVALUE64)
            // SPEC-objectmodel §Q/§9.5: owned butterfly() caller — dispatch
            // INTERNALLY flag-on, like the sibling families above
            // (getArrayLength/getVectorLength/canSetIndexQuicklyForPutDirect).
            // Segmented => empty sentinel, matching the slow forms (see
            // convertUndecidedToInt32); callers fall to their generic paths.
            // E5 "None first" (round 4): a racing N3 first install can pair
            // word==0 with a fresh indexed type — None must also return the
            // empty sentinel here, NOT fall to tryMakeWritableInt32Slow,
            // whose switch CRASH()es on ALL_INT32_INDEXING_TYPES.
            // Flat => mask + today's code. Flag-off bit-identical (I22).
            // FIXME(threads): this closes a flat-only-reader conformance hole
            // but is NOT the fix for the i03-i37 abort in
            // JSObjectWithButterfly::butterfly(); that caller needs re-triage
            // outside JSObject.h/Structure.*/ConcurrentButterfly.*.
            if (Options::useJSThreads()) [[unlikely]] {
                uint64_t word = taggedButterflyWord();
                if (isSegmentedButterfly(word)) [[unlikely]]
                    return ContiguousJSValues();
                if (!(word & butterflyPointerMask)) [[unlikely]]
                    return ContiguousJSValues();
                return untaggedButterfly(word)->contiguousInt32();
            }
#endif
            return butterfly()->contiguousInt32();
        }
        return tryMakeWritableInt32Slow(vm);
    }
        
    // Returns 0 if double storage cannot be created - either because
    // indexing should be sparse, we're having a bad time, or because
    // we already have a more general form of storage (contiguous,
    // or array storage).
    ContiguousDoubles tryMakeWritableDouble(VM& vm)
    {
        if (hasDouble(indexingType()) && !isCopyOnWrite(indexingMode())) [[likely]] {
#if USE(JSVALUE64)
            // SPEC-objectmodel §Q/§9.5 + E5 "None first": see tryMakeWritableInt32.
            if (Options::useJSThreads()) [[unlikely]] {
                uint64_t word = taggedButterflyWord();
                if (isSegmentedButterfly(word)) [[unlikely]]
                    return ContiguousDoubles();
                if (!(word & butterflyPointerMask)) [[unlikely]]
                    return ContiguousDoubles();
                return untaggedButterfly(word)->contiguousDouble();
            }
#endif
            return butterfly()->contiguousDouble();
        }
        return tryMakeWritableDoubleSlow(vm);
    }
        
    // Returns 0 if contiguous storage cannot be created - either because
    // indexing should be sparse or because we're having a bad time.
    ContiguousJSValues tryMakeWritableContiguous(VM& vm)
    {
        if (hasContiguous(indexingType()) && !isCopyOnWrite(indexingMode())) [[likely]] {
#if USE(JSVALUE64)
            // SPEC-objectmodel §Q/§9.5 + E5 "None first": see tryMakeWritableInt32.
            if (Options::useJSThreads()) [[unlikely]] {
                uint64_t word = taggedButterflyWord();
                if (isSegmentedButterfly(word)) [[unlikely]]
                    return ContiguousJSValues();
                if (!(word & butterflyPointerMask)) [[unlikely]]
                    return ContiguousJSValues();
                return untaggedButterfly(word)->contiguous();
            }
#endif
            return butterfly()->contiguous();
        }
        return tryMakeWritableContiguousSlow(vm);
    }

    // Ensure that the object is in a mode where it has array storage. Use
    // this if you're about to perform actions that would have required the
    // object to be converted to have array storage, if it didn't have it
    // already.
    ArrayStorage* ensureArrayStorage(VM& vm)
    {
        if (hasAnyArrayStorage(indexingType())) [[likely]]
            return butterfly()->arrayStorage();

        return ensureArrayStorageSlow(vm);
    }

    void ensureWritable(VM& vm)
    {
        if (isCopyOnWrite(indexingMode()))
            convertFromCopyOnWrite(vm);
    }

    static constexpr size_t offsetOfInlineStorage();

    static constexpr ptrdiff_t butterflyOffset()
    {
        return sizeof(JSObject);
    }

    void* butterflyAddress()
    {
        return std::bit_cast<char*>(this) + butterflyOffset();
    }

    JS_EXPORT_PRIVATE JSValue getMethod(JSGlobalObject*, CallData&, const Identifier&, const String& errorMessage);

    bool canPerformFastPutInline(VM&, PropertyName);
    bool canPerformFastPutInlineExcludingProto();

    bool mayBePrototype() const;
    void didBecomePrototype(VM&);

    std::optional<Structure::PropertyHashEntry> findPropertyHashEntry(PropertyName) const;

    DECLARE_EXPORT_INFO;

    bool getOwnNonIndexPropertySlot(VM&, Structure*, PropertyName, PropertySlot&);
    bool getNonIndexPropertySlot(JSGlobalObject*, PropertyName, PropertySlot&);

    JS_EXPORT_PRIVATE NEVER_INLINE bool putInlineSlow(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    JS_EXPORT_PRIVATE NEVER_INLINE bool putInlineFastReplacingStaticPropertyIfNeeded(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    bool putInlineFast(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);

protected:
#if ASSERT_ENABLED
    void finishCreation(VM& vm)
    {
        Base::finishCreation(vm);
        ASSERT(classInfo());
        ASSERT(structure()->isObject());
        ASSERT(structure()->hasPolyProto() || getPrototypeDirect().isNull() || Heap::heap(this) == Heap::heap(getPrototypeDirect()));
        ASSERT(is<JSObject>(*this));
    }
#endif

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    // To instantiate objects you likely want JSFinalObject, below.
    // To create derived types you likely want JSNonFinalObject, below.
    JSObject(VM&, Structure*);

    // Returns reference to butterfly field storage. Only valid for objects that have butterfly storage
    // (JSObjectWithButterfly subclasses). Used by JSObject methods that manipulate butterfly storage.
    ALWAYS_INLINE AuxiliaryBarrier<Butterfly*>& butterflyRef()
    {
        ASSERT(type() != WebAssemblyGCObjectType);
        return *std::bit_cast<AuxiliaryBarrier<Butterfly*>*>(std::bit_cast<char*>(this) + butterflyOffset());
    }

    JSObject(CreatingWellDefinedBuiltinCellTag, StructureID structureID, int32_t blob)
        : JSCell(CreatingWellDefinedBuiltinCell, structureID, blob)
    {
    }

    // Call this if you know that the object is in a mode where it has array
    // storage. This will assert otherwise.
    ArrayStorage* arrayStorage()
    {
        ASSERT(hasAnyArrayStorage(indexingType()));
        return butterfly()->arrayStorage();
    }
        
    // Call this if you want to predicate some actions on whether or not the
    // object is in a mode where it has array storage.
    ArrayStorage* arrayStorageOrNull()
    {
        switch (indexingType()) {
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            return butterfly()->arrayStorage();
                
        default:
            return nullptr;
        }
    }
        
    size_t butterflyTotalSize();
    size_t butterflyPreCapacity();

    // Flag-on (useJSThreads), createInitialUndecided may return nullptr and
    // createInitialInt32/Double/Contiguous may return EMPTY storage: either a
    // racing first install won (N3 loser - the caller must re-dispatch on the
    // settled state) or the publication ended SEGMENTED (not flat-addressable).
    // Flag-off behavior is unchanged (never null/empty). See
    // createInitialIndexedStorageConcurrent (review round 2).
    Butterfly* createInitialUndecided(VM&, unsigned length);
    ContiguousJSValues createInitialInt32(VM&, unsigned length);
    ContiguousDoubles createInitialDouble(VM&, unsigned length);
    ContiguousJSValues createInitialContiguous(VM&, unsigned length);
#if USE(JSVALUE64)
    // SPEC-objectmodel review round 2 (N3/I10/I21): race-safe publication for
    // the createInitial* family. Returns the published FLAT butterfly, or
    // nullptr when the storage is not flat-addressable (racing winner, or a
    // segmented publication). Never traps on a legal race.
    Butterfly* createInitialIndexedStorageConcurrent(VM&, TransitionKind, unsigned length);
    // Returns false when the caller must re-dispatch its full put path (the
    // value may not have been stored).
    bool tryCreateInitialForValueAndSetConcurrent(VM&, unsigned index, JSValue);
#endif

    void convertUndecidedForValue(VM&, JSValue);
    void createInitialForValueAndSet(VM&, unsigned index, JSValue);
    void convertInt32ForValue(VM&, JSValue);
    void convertDoubleForValue(VM&, JSValue);
    void convertFromCopyOnWrite(VM&);

    static Butterfly* createArrayStorageButterfly(VM&, JSObject* intendedOwner, Structure*, unsigned length, unsigned vectorLength, Butterfly* oldButterfly = nullptr);
    static Butterfly* tryCreateArrayStorageButterfly(VM&, JSObject* intendedOwner, Structure*, unsigned length, unsigned vectorLength, Butterfly* oldButterfly = nullptr);

    ArrayStorage* createArrayStorage(VM&, unsigned length, unsigned vectorLength);
    ArrayStorage* createInitialArrayStorage(VM&);
        
    ContiguousJSValues convertUndecidedToInt32(VM&);
    ContiguousDoubles convertUndecidedToDouble(VM&);
    ContiguousJSValues convertUndecidedToContiguous(VM&);
    ArrayStorage* convertUndecidedToArrayStorage(VM&, TransitionKind);
    ArrayStorage* convertUndecidedToArrayStorage(VM&);
        
    ContiguousDoubles convertInt32ToDouble(VM&);
    ContiguousJSValues convertInt32ToContiguous(VM&);
    ArrayStorage* convertInt32ToArrayStorage(VM&, TransitionKind);
    ArrayStorage* convertInt32ToArrayStorage(VM&);

    ContiguousJSValues convertDoubleToContiguous(VM&);
    ArrayStorage* convertDoubleToArrayStorage(VM&, TransitionKind);
    ArrayStorage* convertDoubleToArrayStorage(VM&);
        
    ArrayStorage* convertContiguousToArrayStorage(VM&, TransitionKind);
    ArrayStorage* convertContiguousToArrayStorage(VM&);

    void convertToIndexingTypeIfNeeded(VM&, IndexingType);
        
    ArrayStorage* ensureArrayStorageExistsAndEnterDictionaryIndexingMode(VM&);
        
    bool defineOwnNonIndexProperty(JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool throwException);

    template<IndexingType indexingShape>
    bool putByIndexBeyondVectorLengthWithoutAttributes(JSGlobalObject*, unsigned propertyName, JSValue);
    bool putByIndexBeyondVectorLengthWithArrayStorage(JSGlobalObject*, unsigned propertyName, JSValue, bool shouldThrow, ArrayStorage*);

    bool increaseVectorLength(VM&, unsigned newLength);
    void NODELETE deallocateSparseIndexMap();
    bool defineOwnIndexedProperty(JSGlobalObject*, unsigned, const PropertyDescriptor&, bool throwException);
    SparseArrayValueMap* allocateSparseIndexMap(VM&);
        
    void notifyPresenceOfIndexedAccessors(VM&);
        
    bool attemptToInterceptPutByIndexOnHole(JSGlobalObject*, unsigned index, JSValue, bool shouldThrow, bool& putResult);
        
    // Call this if you want setIndexQuickly to succeed and you're sure that
    // the array is contiguous. Defined below JSObjectWithButterfly (the
    // flag-on F1 leg casts to it, which needs the complete type).
    [[nodiscard]] inline bool ensureLength(VM&, unsigned length);
        
    // Call this if you want to shrink the butterfly backing store, and you're
    // sure that the array is contiguous.
    void reallocateAndShrinkButterfly(VM&, unsigned length);
    
    template<IndexingType indexingShape>
    unsigned NODELETE countElements(Butterfly*);
        
    // This is relevant to undecided, int32, double, and contiguous.
    unsigned countElements();

private:
    friend class LLIntOffsetsExtractor;
    friend class VMInspector;

    // Nobody should ever ask any of these questions on something already known to be a JSObject.
    using JSCell::isAPIValueWrapper;
    using JSCell::isGetterSetter;
    void getObject();
    void getString(JSGlobalObject* globalObject);
    void isObject();
    void isString();
        
    Butterfly* createInitialIndexedStorage(VM&, unsigned length);
        
    ArrayStorage* enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(VM&, ArrayStorage*);
        
    template<PutMode>
    ASCIILiteral putDirectInternal(VM&, PropertyName, JSValue, unsigned attr, PutPropertySlot&);

    JS_EXPORT_PRIVATE NEVER_INLINE ASCIILiteral putDirectToDictionaryWithoutExtensibility(VM&, PropertyName, JSValue, PutPropertySlot&);
    JS_EXPORT_PRIVATE void fillGetterPropertySlot(VM&, PropertySlot&, JSCell*, unsigned, PropertyOffset);
    void fillCustomGetterPropertySlot(PropertySlot&, CustomGetterSetter*, unsigned, Structure*);

    JS_EXPORT_PRIVATE bool getOwnStaticPropertySlot(VM&, PropertyName, PropertySlot&);
        
    bool putByIndexBeyondVectorLength(JSGlobalObject*, unsigned propertyName, JSValue, bool shouldThrow);
    bool putDirectIndexBeyondVectorLengthWithArrayStorage(JSGlobalObject*, unsigned propertyName, JSValue, unsigned attributes, PutDirectIndexMode, ArrayStorage*);
    JS_EXPORT_PRIVATE bool putDirectIndexSlowOrBeyondVectorLength(JSGlobalObject*, unsigned propertyName, JSValue, unsigned attributes, PutDirectIndexMode);
        
    unsigned getNewVectorLength(unsigned indexBias, unsigned currentVectorLength, unsigned currentLength, unsigned desiredLength);
    unsigned getNewVectorLength(unsigned desiredLength);

    ArrayStorage* constructConvertedArrayStorageWithoutCopyingElements(VM&, unsigned neededLength);
        
    JS_EXPORT_PRIVATE void setIndexQuicklyToUndecided(VM&, unsigned index, JSValue);
    JS_EXPORT_PRIVATE void convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(VM&, unsigned index, JSValue);
    JS_EXPORT_PRIVATE void convertDoubleToContiguousWhilePerformingSetIndex(VM&, unsigned index, JSValue);
        
    bool ensureLengthSlow(VM&, unsigned length);

#if USE(JSVALUE64)
    // SPEC-objectmodel Task 2 regime-dispatching slow paths (flag-on only;
    // defined in JSObject.cpp). These implement the §Q dispatch for the
    // quickly-family and locationForOffset; the full §2-dispatch *Concurrent
    // accessors of §9.5 (getDirectConcurrent & friends) land with Task 6.
    JS_EXPORT_PRIVATE const WriteBarrierBase<Unknown>* locationForOutOfLineOffsetConcurrent(PropertyOffset) const;
    JS_EXPORT_PRIVATE bool canGetIndexQuicklyConcurrent(unsigned) const;
    JS_EXPORT_PRIVATE JSValue getIndexQuicklyConcurrent(unsigned) const;
    JS_EXPORT_PRIVATE JSValue tryGetIndexQuicklyConcurrent(unsigned, ArrayProfile*) const;
    JS_EXPORT_PRIVATE bool trySetIndexQuicklyConcurrent(VM&, unsigned, JSValue, ArrayProfile*);
    JS_EXPORT_PRIVATE void setIndexQuicklyConcurrent(VM&, unsigned, JSValue);

    // Flag-on butterfly installs/replacements: stamp the installing thread's
    // TID (§2.1 N3); defined in JSObjectInlines.h.
    inline void setButterflyConcurrent(VM&, Butterfly*);
    inline void storeTaggedButterflyWordConcurrent(VM&, Butterfly*);
    inline void nukeStructureAndSetButterflyConcurrent(VM&, StructureID, Butterfly*);
#endif

    ContiguousJSValues tryMakeWritableInt32Slow(VM&);
    ContiguousDoubles tryMakeWritableDoubleSlow(VM&);
    ContiguousJSValues tryMakeWritableContiguousSlow(VM&);
    JS_EXPORT_PRIVATE ArrayStorage* ensureArrayStorageSlow(VM&);

#if USE(JSVALUE64)
    // SPEC-objectmodel §4.6 "stops" (Task 8, I31/I10): flag-on, every
    // transition INTO ArrayStorage (convert*ToArrayStorage) plans + allocates
    // outside a §10.6 per-event stop (O4) and copies + publishes INSIDE it -
    // firing both TTL sets (F2) when the trigger is shared (foreign tag,
    // SW=1, or segmented) and publishing a FLAT butterfly tagged
    // (currentButterflyTID(), 1) for shared triggers (AS never segments).
    // Defined in JSObject.cpp.
    ArrayStorage* convertToArrayStorageConcurrent(VM&, TransitionKind);

    // SPEC-objectmodel §4.7/I28 (review round 1): flag-on, the in-place
    // indexing-shape RELABELS (convertUndecidedTo*, convertInt32ToDouble,
    // convertInt32ToContiguous, convertDoubleToContiguous) rewrite element
    // lanes of the CURRENT storage between boxed-JSValue and raw-double
    // interpretations, so they run as per-event §10.6 stops (firing F2 for
    // shared triggers) instead of racing lock-free readers. Works on flat AND
    // segmented words (the butterfly word itself is untouched - I16).
    // Defined in JSObject.cpp.
    void relabelIndexingShapeConcurrent(VM&, TransitionKind);

    // SPEC-objectmodel §4.6/I31 (review round 3): flag-on route for the
    // blank-indexing transition INTO ArrayStorage (createArrayStorage). Plans +
    // allocates outside a §10.6 per-event stop (O4), re-verifies + copies
    // out-of-line properties (flat OR segmented source) + publishes FLAT
    // (currentButterflyTID(), shared ? 1 : 0) inside it, firing both TTL sets
    // (F2) for shared triggers. Loser re-dispatch, never a plain nuke+store.
    // Defined in JSObject.cpp.
    ArrayStorage* createArrayStorageConcurrent(VM&, unsigned length, unsigned vectorLength);
#endif

    PropertyOffset prepareToPutDirectWithoutTransition(VM&, PropertyName, unsigned attributes, StructureID, Structure*);

#if USE(JSVALUE64)
    // SPEC-objectmodel §6 L3/L4 (review round 1): flag-on form of the
    // "without transition" add (pinned-table / dictionary-style: structure and
    // object mutate in tandem). Serialized against deletes/flatten/other adds
    // by the cell lock, with the VALUE stored inside the same critical
    // section as the table edit (no holes - I9; dictionary readers are
    // cell-locked, L3). Returns the offset. Defined in JSObjectInlines.h.
    inline PropertyOffset putDirectWithoutTransitionConcurrent(VM&, PropertyName, JSValue, unsigned attributes);

    // SPEC-objectmodel E4/I29 + §4.3/N2 (review round 1): flag-on publication
    // of a named-property transition from putDirectInternal. E4-eligible
    // (owner instance, SW=0, both source TTL sets valid+watched, not PA):
    // today's lock-free sequence with the I29 allocate->revalidate->poll-free
    // publication discipline (value stored BEFORE the new StructureID - no
    // holes, I9). Otherwise: the locked protocols (trySegmentedTransition for
    // out-of-line offsets, tryStructureOnlyTransition for inline/butterfly-
    // untouched ones). Returns false = RESTART: the caller re-enters its
    // WHOLE operation from a fresh structureID/tag (§2 dispatch). Defined in
    // JSObjectInlines.h.
    inline bool tryPutDirectTransitionConcurrent(VM&, Structure* expectedSource, StructureID sourceID, Structure* newStructure, PropertyOffset, JSValue);

    // SPEC-objectmodel §6 (review round 3): regime guard for the cell-locked
    // dictionary / without-transition adds. The under-lock classification
    // decides whether the locked add (and in particular its out-of-line
    // capacity GROWTH, which copies the flat payload) is sound for the loaded
    // word, or whether the caller must release the cell lock, run the returned
    // slow action, and RESTART its §2 loop:
    //   - FireSharedWriteBit: foreign writer on an SW=0 word - F1/flip first
    //     (I12; also covers the §4.6 AS per-event stop and §4.8 CoW via
    //     ensureSharedWriteBit's carve-outs).
    //   - ConvertToSegmented: flat word that may need growth but is SW=1 or
    //     has a fired writeThreadLocal set - a lock-free F1 flip could land
    //     between the copy and the publication (§4.3(b2)/I21/I27), so the
    //     object goes segmented first; segmented growth appends fragments
    //     without relocating shared storage.
    //   - GrowSegmentedOutOfLine: segmented word whose out-of-line fragment
    //     coverage cannot absorb a fresh offset - pre-grow via a replacement
    //     spine (coverage is monotone, so it stays sufficient).
    //   - MaterializeCopyOnWrite: §4.8 materialize-first.
    // Proceed (true) is returned only for regimes whose growth leg is sound
    // under the cell lock: None (with the caller's ID-lane pre-nuke),
    // owner-(t,0) flat with writeThreadLocal still valid, AS (every access and
    // transition cell-locked - I31 + the E4 AS exclusion), segmented with
    // sufficient coverage, and non-growing SW=1 stores. Defined in
    // JSObjectInlines.h.
    enum class ConcurrentLockedAddSlowAction : uint8_t {
        None,
        FireSharedWriteBit,
        ConvertToSegmented,
        GrowSegmentedOutOfLine,
        MaterializeCopyOnWrite,
    };
    inline bool classifyConcurrentLockedAdd(Structure*, ConcurrentLockedAddSlowAction&);
    inline void performConcurrentLockedAddSlowAction(VM&, ConcurrentLockedAddSlowAction);

    // The regime-dispatched growth leg the two §6 cell-locked add lambdas
    // share (review round 3): segmented => maxOffset bump only (coverage
    // pre-grown), AS shared/foreign => tag-preserving AS-COPY publication,
    // None / owner-safe flat => today's nuke-bracketed copy. Sound ONLY after
    // classifyConcurrentLockedAdd returned Proceed under the same cell lock.
    // Defined in JSObjectInlines.h.
    inline void growOutOfLineStorageForConcurrentLockedAdd(VM&, StructureID, Structure*, PropertyOffset newMaxOffset, unsigned oldOutOfLineCapacity, unsigned newOutOfLineCapacity);
#endif
};

// JSObjectWithButterfly is a JSObject that has out-of-line property storage (butterfly).
// All normal JS objects go through this class. Wasm GC objects inherit JSObject directly
// without butterfly to save 8 bytes per allocation.
class JSObjectWithButterfly : public JSObject {
    friend class JSObject;
    friend class JSFinalObject;
    friend class LLIntOffsetsExtractor;

public:
    using Base = JSObject;

    DECLARE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE);

    DECLARE_EXPORT_INFO;

    // SPEC-objectmodel §9.5: same mask-on-load + flatness CONTRACT as
    // JSObject::butterfly() (see comment there). Flag-off, identity (I22).
    const Butterfly* butterfly() const LIFETIME_BOUND { return const_cast<JSObjectWithButterfly*>(this)->butterfly(); }
    Butterfly* butterfly() LIFETIME_BOUND
    {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            ASSERT(!isSegmentedButterfly(word));
            if (verifyConcurrentButterflyEnabled()) [[unlikely]]
                RELEASE_ASSERT(!isSegmentedButterfly(word));
            return untaggedButterfly(word);
        }
#endif
        return m_butterfly.get();
    }

    ALWAYS_INLINE uint64_t taggedButterflyWord() const // raw 64-bit load, never masked (§9.5)
    {
#if USE(JSVALUE64)
        // TSAN-TRIAGE §3.15: relaxed atomic load — see JSObject::taggedButterflyWord().
        return butterflyConcurrentLoad(std::bit_cast<const uint64_t*>(&m_butterfly));
#else
        return static_cast<uint64_t>(std::bit_cast<uintptr_t>(m_butterfly.get()));
#endif
    }

    // NOTE (SPEC-objectmodel Task 2 audit): flag-on the loaded pointer is the
    // TAGGED word; callers (visitButterflyImpl) must mask it with
    // untaggedButterfly()/check isSegmentedButterfly() before dereferencing.
    Dependency fencedButterfly(Butterfly*& butterfly)
    {
        return Dependency::loadAndFence(static_cast<Butterfly**>(butterflyAddress()), butterfly);
    }

    // Flat-only, like JSObject::outOfLineStorage() (§Q); routed through the
    // masking butterfly() accessor rather than raw m_butterfly.
    ConstPropertyStorage outOfLineStorage() const { return butterfly()->propertyStorage(); }
    PropertyStorage outOfLineStorage() { return butterfly()->propertyStorage(); }

#if USE(JSVALUE64)
    // SPEC-objectmodel §9.5 (Task 6; defined in ConcurrentButterfly.cpp):
    // full-§2-dispatch interpreter/runtime slow paths for existing direct
    // slots. M7-conforming (the (d) loadLoadFence orders the caller's
    // structureID/offset provenance before the tagged-word load - I24), M5
    // nuke-tolerant (they never decode a possibly nuked StructureID - dispatch
    // keys on the tagged word + indexing byte), poll-free between slot
    // resolution and access (I34), and AS-shape accesses cell-locked (I31).
    JS_EXPORT_PRIVATE JSValue getDirectConcurrent(PropertyOffset) const;
    JS_EXPORT_PRIVATE void putDirectConcurrent(VM&, PropertyOffset, JSValue);
    // §9.5 indexed forms (Task 8, §4.4/§4.6; defined in ConcurrentButterfly.cpp):
    // full §2 dispatch; AS accesses cell-locked (I31/L5); foreign first writes
    // through ensureSharedWriteBit (F1); in-shape dense growth through the
    // §4.4 resize drivers. Empty/false => caller's generic path (sparse maps,
    // shape transitions, length semantics).
    JS_EXPORT_PRIVATE JSValue getIndexConcurrent(unsigned) const;
    JS_EXPORT_PRIVATE bool putIndexConcurrent(VM&, unsigned, JSValue);
#endif

    void* butterflyAddress()
    {
        return &m_butterfly;
    }

    // Visits the butterfly unless there is a race. Returns the structure if there was no race.
    template<typename Visitor> Structure* visitButterfly(Visitor&);
    template<typename Visitor> Structure* visitButterflyImpl(Visitor&);
    template<typename Visitor> void markAuxiliaryAndVisitOutOfLineProperties(Visitor&, Butterfly*, Structure*, PropertyOffset maxOffset);

protected:
    JSObjectWithButterfly(VM& vm, Structure* structure, Butterfly* butterfly = nullptr)
        : JSObject(vm, structure)
        , m_butterfly(butterfly, WriteBarrierEarlyInit)
    {
#if USE(JSVALUE64)
        // SPEC-objectmodel §2.1/Task 2: stamp the allocating thread's TID at
        // first install. Pre-escape (object not yet visible to other threads),
        // so a plain store is the sanctioned E4-eligible install form (N3).
        if (Options::useJSThreads() && butterfly) [[unlikely]]
            m_butterfly.setWithoutBarrier(std::bit_cast<Butterfly*>(encodeButterfly(butterfly, currentButterflyTID(), false)));
#endif
    }

    JSObjectWithButterfly(CreatingWellDefinedBuiltinCellTag, StructureID structureID, int32_t blob)
        : JSObject(CreatingWellDefinedBuiltinCell, structureID, blob)
        , m_butterfly(nullptr, WriteBarrierEarlyInit)
    {
    }

private:
    AuxiliaryBarrier<Butterfly*> m_butterfly;
#if CPU(ADDRESS32)
    unsigned m_32BitPadding;
#endif
};

constexpr size_t JSObject::offsetOfInlineStorage()
{
    return sizeof(JSObjectWithButterfly);
}

// Defined here (not in-class) because the flag-on F1 leg casts `this` to
// JSObjectWithButterfly, which must be complete.
ALWAYS_INLINE bool JSObject::ensureLength(VM& vm, unsigned length)
{
    RELEASE_ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(hasContiguous(indexingType()) || hasInt32(indexingType()) || hasDouble(indexingType()) || hasUndecided(indexingType()));

#if USE(JSVALUE64)
    // SPEC-objectmodel §4.4 (Task 8): flag-on, dispatch on the tagged word
    // (§2) - segmented words use the loaded spine's vectorLength (C4) and
    // the SHARED publicLength slot; growth (incl. a mid-call T2
    // conversion) goes through ensureLengthSlow's concurrent driver.
    if (Options::useJSThreads()) [[unlikely]] {
        uint64_t word = taggedButterflyWord();
        bool needsSlow = isCopyOnWrite(indexingMode());
        if (isSegmentedButterfly(word))
            needsSlow |= segmentedVectorLength(butterflySpine(word)) < length;
        else
            needsSlow |= untaggedButterfly(word)->vectorLength() < length;
        if (needsSlow) {
            if (!ensureLengthSlow(vm, length))
                return false;
            word = taggedButterflyWord(); // The slow path may have republished (T1/T2/§4.8).
        }
        // §3 F1 (review round 1): the publicLength bump below is a WRITE into
        // the IndexingHeader of the butterfly. A foreign plain store on an
        // SW=0 flat word would land while writeThreadLocal is still valid
        // (I12) and could be silently dropped by an owner T1 copying resize
        // whose (t,0) CAS still succeeds - so fire F1/flip SW first, then
        // re-load. (Segmented words are SW=1 by I3; the bump there targets
        // the SHARED fragment-0 slot. After ensureLengthSlow the word may
        // already be SW=1/segmented/owner-materialized.)
        if ((word & butterflyPointerMask) && !isSegmentedButterfly(word)
            && !butterflySharedWrite(word) && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
            ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
            word = taggedButterflyWord();
        }
        // Review round 3 (I21; the i03-t5 racing-growers scenario): on SHARED
        // words the bump must be the monotone CAS-max - a read-then-plain-store
        // by a loser could REGRESS a racing grower's bump and hide its element
        // behind the min(publicLength, vectorLength) read bound. Segmented
        // words are shared by definition (notTTLTID); flat SW=1 words race
        // other §3 writers. Only the owner-exclusive (currentTID, 0) flat word
        // keeps the plain store (SW=0 => no foreign write has ever landed on
        // this instance, I12).
        if (isSegmentedButterfly(word)) {
            ButterflySpine* spine = butterflySpine(word);
            spine->bumpPublicLengthToAtLeast(length);
        } else {
            Butterfly* flatButterfly = untaggedButterfly(word);
            if (butterflySharedWrite(word))
                flatButterfly->bumpPublicLengthToAtLeast(length);
            else if (flatButterfly->publicLength() < length)
                flatButterfly->setPublicLength(length);
        }
        return true;
    }
#endif

    if (butterfly()->vectorLength() < length || isCopyOnWrite(indexingMode())) {
        if (!ensureLengthSlow(vm, length))
            return false;
    }

    if (butterfly()->publicLength() < length)
        butterfly()->setPublicLength(length);
    return true;
}

// JSNonFinalObject is a type of JSObject that has some internal storage,
// but also preserves some space in the collector cell for additional
// data members in derived types.
class JSNonFinalObject : public JSObjectWithButterfly {
    friend class JSObject;

public:
    typedef JSObjectWithButterfly Base;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

protected:
    explicit JSNonFinalObject(VM& vm, Structure* structure, Butterfly* butterfly = nullptr)
        : JSObjectWithButterfly(vm, structure, butterfly)
    {
    }

#if ASSERT_ENABLED
    void finishCreation(VM& vm)
    {
        Base::finishCreation(vm);
        ASSERT(!this->structure()->hasInlineStorage());
        ASSERT(classInfo());
    }
#endif
};

// JSFinalObject is a type of JSObject that contains sufficient internal
// storage to fully make use of the collector cell containing it.
class JSFinalObject final : public JSObjectWithButterfly {
    friend class JSObject;
public:
    using Base = JSObjectWithButterfly;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM&);

    static size_t allocationSize(Checked<size_t> inlineCapacity)
    {
        return sizeof(JSObjectWithButterfly) + inlineCapacity * sizeof(WriteBarrierBase<Unknown>);
    }

    static inline constexpr TypeInfo typeInfo() { return TypeInfo(FinalObjectType, StructureFlags); }
    static constexpr IndexingType defaultIndexingType = NonArray;
    static constexpr uint32_t defaultTypeInfoBlob()
    {
        return TypeInfoBlob::typeInfoBlob(defaultIndexingType, typeInfo().type(), typeInfo().inlineTypeFlags());
    }

    static constexpr unsigned defaultSizeInBytes = 64;
    static constexpr unsigned defaultInlineCapacity = (defaultSizeInBytes - sizeof(JSObjectWithButterfly)) / sizeof(WriteBarrier<Unknown>);
    static_assert(defaultInlineCapacity < firstOutOfLineOffset);

    static constexpr unsigned maxSizeInBytes = 512;
    static constexpr unsigned maxInlineCapacity = (maxSizeInBytes - sizeof(JSObjectWithButterfly)) / sizeof(WriteBarrier<Unknown>);
    static_assert(maxInlineCapacity < firstOutOfLineOffset);

    static JSFinalObject* create(VM&, Structure*);
    static JSFinalObject* createWithButterfly(VM&, Structure*, Butterfly*);
    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue, unsigned);

    static JSFinalObject* createDefaultEmptyObject(JSGlobalObject*);

    DECLARE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE);

    DECLARE_EXPORT_INFO;

private:
    friend class LLIntOffsetsExtractor;

    explicit JSFinalObject(VM& vm, Structure* structure, Butterfly* butterfly, size_t inlineCapacity)
        : JSObjectWithButterfly(vm, structure, butterfly)
    {
        // We do not need to use gcSafeMemcpy since this object is not exposed yet.
        memset(inlineStorageUnsafe(), 0, inlineCapacity * sizeof(EncodedJSValue));
    }

    explicit JSFinalObject(CreatingWellDefinedBuiltinCellTag, StructureID structureID)
        : JSObjectWithButterfly(CreatingWellDefinedBuiltinCell, structureID, defaultTypeInfoBlob())
    {
        // We do not need to use gcSafeMemcpy since this object is not exposed yet.
        memset(inlineStorageUnsafe(), 0, defaultInlineCapacity * sizeof(EncodedJSValue));
     }

#if ASSERT_ENABLED
    void finishCreation(VM& vm)
    {
        Base::finishCreation(vm);
        ASSERT(butterfly() || structure()->totalStorageCapacity() == structure()->inlineCapacity());
        ASSERT(classInfo());
    }
#endif
};

JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(objectPrivateFuncInstanceOf);

inline JSFinalObject* JSFinalObject::createWithButterfly(VM& vm, Structure* structure, Butterfly* butterfly)
{
    size_t inlineCapacity = structure->inlineCapacity();
    JSFinalObject* finalObject = new (
        NotNull,
        allocateCell<JSFinalObject>(vm, allocationSize(inlineCapacity))
    ) JSFinalObject(vm, structure, butterfly, inlineCapacity);
    finalObject->finishCreation(vm);
    return finalObject;
}

inline JSFinalObject* JSFinalObject::create(VM& vm, Structure* structure)
{
    return createWithButterfly(vm, structure, nullptr);
}

inline bool JSObject::isGlobalObject() const
{
    return type() == GlobalObjectType;
}

inline bool JSObject::isJSLexicalEnvironment() const
{
    return type() == LexicalEnvironmentType || type() == ModuleEnvironmentType;
}

inline bool JSObject::isGlobalLexicalEnvironment() const
{
    return type() == GlobalLexicalEnvironmentType;
}

inline bool JSObject::isStrictEvalActivation() const
{
    return type() == StrictEvalActivationType;
}

inline bool JSObject::isEnvironment() const
{
    bool result = GlobalObjectType <= type() && type() <= StrictEvalActivationType;
    ASSERT((isGlobalObject() || isJSLexicalEnvironment() || isGlobalLexicalEnvironment() || isStrictEvalActivation()) == result);
    return result;
}

inline bool JSObject::isErrorInstance() const
{
    return type() == ErrorInstanceType;
}

inline bool JSObject::isWithScope() const
{
    return type() == WithScopeType;
}

inline void JSObject::setStructure(VM& vm, Structure* structure)
{
    ASSERT(structure);
    // SPEC-objectmodel §9.5: butterfly() is flat-only, but setStructure is
    // legitimately reached with a segmented or foreign-tagged word (e.g. the
    // §4.7/I28 STW relabel closure in relabelIndexingShapeConcurrent, or the
    // dictionary attribute-change legs of putDirectInternal). The assert only
    // needs storage PRESENCE, so test the raw tagged word's payload instead of
    // dereferencing through the flat-only accessor - a segmented spine payload
    // counts as "has storage", which is exactly what shape consistency wants.
    // Deliberately does NOT verify flatness: segmented words are legal here
    // (I16/I28), so do not "restore" a butterfly() call.
    ASSERT(!(taggedButterflyWord() & butterflyPointerMask) == !(structure->outOfLineCapacity() || structure->hasIndexingHeader(this)));
    JSCell::setStructure(vm, structure);
}

inline JSObject* asObject(JSCell* cell)
{
    ASSERT(cell);
    ASSERT(cell->isObjectSlow());
    return uncheckedDowncast<JSObject>(cell);
}

inline JSObject* asObject(JSValue value)
{
    return asObject(value.asCell());
}

inline JSObject::JSObject(VM& vm, Structure* structure)
    : JSCell(vm, structure)
{
}

inline JSValue JSObject::getPrototypeDirect() const
{
    return structure()->storedPrototype(this);
}

inline JSValue JSObject::getPrototype(JSGlobalObject* globalObject)
{
    if (!structure()->typeInfo().overridesGetPrototype()) [[likely]]
        return getPrototypeDirect();
    return methodTable()->getPrototype(this, globalObject);
}

// Normally, we never shrink the butterfly so if we know an offset is valid for some
// past structure then it should be valid for any new structure. However, we may sometimes
// shrink the butterfly when we are holding the Structure's ConcurrentJSLock, such as when we
// flatten an object.
IGNORE_RETURN_TYPE_WARNINGS_BEGIN
ALWAYS_INLINE JSValue JSObject::getDirect(Locker<JSCellLock>& cellLock, Concurrency concurrency, Structure* expectedStructure, PropertyOffset offset) const
{
    switch (concurrency) {
    case Concurrency::MainThread:
        ASSERT(!isCompilationThread() && !Thread::mayBeGCThread());
        return getDirect(offset);
    case Concurrency::ConcurrentThread:
        return getDirectConcurrently(cellLock, expectedStructure, offset);
    }
}
IGNORE_RETURN_TYPE_WARNINGS_END

inline JSValue JSObject::getDirectConcurrently(Locker<JSCellLock>&, Structure* expectedStructure, PropertyOffset offset) const
{
    // We always take the cell lock before the structure lock.
    // We must take the cell lock to prevent places like JSArray::unshiftCountWithArrayStorage
    // from changing the butterfly out from under us.
    ConcurrentJSLocker locker { expectedStructure->lock() };
    if (!expectedStructure->isValidOffset(offset))
        return { };
    return getDirect(offset);
}

// It is safe to call this method with a PropertyName that is actually an index,
// but if so will always return false (doesn't search index storage).
ALWAYS_INLINE bool JSObject::getOwnNonIndexPropertySlot(VM& vm, Structure* structure, PropertyName propertyName, PropertySlot& slot)
{
    unsigned attributes;
    PropertyOffset offset = structure->get(vm, propertyName, attributes);
    if (!isValidOffset(offset)) {
        if (!TypeInfo::hasStaticPropertyTable(inlineTypeFlags()))
            return false;
        return getOwnStaticPropertySlot(vm, propertyName, slot);
    }
    
    // getPropertySlot relies on this method never returning index properties!
    ASSERT(!parseIndex(propertyName));

    JSValue value = getDirect(offset);

    // UNGIL (SPEC-objectmodel I9/M7(c)): under useJSThreads a foreign
    // defineProperty publishes the slot VALUE (release) before the new
    // structureID, so a racing reader can observe a torn pair — its sampled
    // structure's attributes (plain data) with the freshly stored
    // GetterSetter/CustomGetterSetter cell (or the reverse, or a cleared
    // slot). Interpreting the value under mismatched attributes either trips
    // the single-mutator asserts below or leaks the raw GetterSetter cell as
    // the property value — a heap state no sequential interleaving produces
    // (the §C.2 Missing-arm contract). Stabilize per M7(c): re-derive
    // (structure, attributes, offset, value) from the CURRENT structureID
    // until one self-consistent pair is observed under an unchanged
    // structureID; the in-flight writer publishes its structureID promptly,
    // so the spin is bounded by the write's publication. Flag-off: branch
    // dead, behavior byte-identical.
    if (Options::useJSThreads()) [[unlikely]] {
        for (;;) {
            bool valueIsGetterSetter = !!value && value.isCell() && value.asCell()->type() == GetterSetterType;
            bool valueIsCustomGetterSetter = !!value && value.isCell() && value.asCell()->type() == CustomGetterSetterType;
            bool consistent = !!value
                && (valueIsGetterSetter == !!(attributes & PropertyAttribute::Accessor))
                && (!valueIsCustomGetterSetter || (attributes & PropertyAttribute::CustomAccessorOrValue));
            if (consistent) {
                WTF::loadLoadFence(); // M7(c): order the structureID re-load after the value load.
                if (structureID().decode() == structure)
                    break;
            }
            // Park-capability (W1/D9): the in-flight writer may be parked by
            // — or be the conductor of — a §A.3 stop window; spinning here
            // with heap access held would wedge that window (watchdog
            // fail-stop). Release/park/re-acquire across any pending window,
            // then re-sample.
            JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm);
            Thread::yield();
            structure = structureID().decode();
            offset = structure->get(vm, propertyName, attributes);
            if (!isValidOffset(offset)) {
                if (!TypeInfo::hasStaticPropertyTable(inlineTypeFlags()))
                    return false;
                return getOwnStaticPropertySlot(vm, propertyName, slot);
            }
            value = getDirect(offset);
        }
    }

    if (value.isCell()) {
        ASSERT(value);
        JSCell* cell = value.asCell();
        JSType type = cell->type();
        switch (type) {
        case GetterSetterType:
            ASSERT(attributes & PropertyAttribute::Accessor);
            fillGetterPropertySlot(vm, slot, cell, attributes, offset);
            return true;
        case CustomGetterSetterType:
            ASSERT(attributes & PropertyAttribute::CustomAccessorOrValue);
            fillCustomGetterPropertySlot(slot, uncheckedDowncast<CustomGetterSetter>(cell), attributes, structure);
            return true;
        default:
            break;
        }
    }

    slot.setValue(this, attributes, value, offset);
    return true;
}

ALWAYS_INLINE void JSObject::fillCustomGetterPropertySlot(PropertySlot& slot, CustomGetterSetter* customGetterSetter, unsigned attributes, Structure* structure)
{
    ASSERT(attributes & PropertyAttribute::CustomAccessorOrValue);
    if (customGetterSetter->inherits<DOMAttributeGetterSetter>()) {
        auto* domAttribute = uncheckedDowncast<DOMAttributeGetterSetter>(customGetterSetter);
        if (structure->isUncacheableDictionary())
            slot.setCustom(this, attributes, domAttribute->getter(), domAttribute->setter(), domAttribute->domAttribute());
        else
            slot.setCacheableCustom(this, attributes, domAttribute->getter(), domAttribute->setter(), domAttribute->domAttribute());
        return;
    }

    if (structure->isUncacheableDictionary())
        slot.setCustom(this, attributes, customGetterSetter->getter(), customGetterSetter->setter());
    else
        slot.setCacheableCustom(this, attributes, customGetterSetter->getter(), customGetterSetter->setter());
}

// It may seem crazy to inline a function this large, especially a virtual function,
// but it makes a big difference to property lookup that derived classes can inline their
// base class call to this.
ALWAYS_INLINE bool JSObject::getOwnPropertySlotImpl(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    Structure* structure = object->structure();
    if (Options::useJSThreads() && structure->isUncacheableDictionary() && !slot.isVMInquiry() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
        return false;
    if (object->getOwnNonIndexPropertySlot(vm, structure, propertyName, slot))
        return true;
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return getOwnPropertySlotByIndex(object, globalObject, index.value(), slot);
    return false;
}

#if !ASSERT_ENABLED
ALWAYS_INLINE bool JSObject::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    return getOwnPropertySlotImpl(object, globalObject, propertyName, slot);
}
#endif

// It may seem crazy to inline a function this large but it makes a big difference
// since this is function very hot in variable lookup
template<bool checkNullStructure>
ALWAYS_INLINE bool JSObject::getPropertySlot(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    JSObject* object = this;
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags())) [[unlikely]] {
            // If propertyName is an index then we may have missed it (as this loop is using
            // getOwnNonIndexPropertySlot), so we cannot safely call the overridden getOwnPropertySlot
            // (lest we return a property from a prototype that is shadowed). Check now for an index,
            // if so we need to start afresh from this object.
            if (std::optional<uint32_t> index = parseIndex(propertyName))
                return getPropertySlot(globalObject, index.value(), slot);
            // Safe to continue searching from current position; call getNonIndexPropertySlot to avoid
            // parsing the int again.
            return object->getNonIndexPropertySlot(globalObject, propertyName, slot);
        }
        ASSERT(object->type() != ProxyObjectType);
        Structure* structure = object->structureID().decode();
#if USE(JSVALUE64)
        if (checkNullStructure) {
            if (!structure) [[unlikely]]
                CRASH_WITH_INFO(object->type(), object->structureID().bits());
        }
#endif
        if (Options::useJSThreads() && structure->isUncacheableDictionary() && !slot.isVMInquiry() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
            return false;
        if (object->getOwnNonIndexPropertySlot(vm, structure, propertyName, slot))
            return true;
        // FIXME: This doesn't look like it's following the specification:
        // https://bugs.webkit.org/show_bug.cgi?id=172572
        JSValue prototype = structure->storedPrototype(object);
        if (!prototype.isObject())
            break;
        object = asObject(prototype);
    }

    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return getPropertySlot(globalObject, index.value(), slot);
    return false;
}

inline bool JSObject::putDirect(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(!value.isGetterSetterSlow() && !(attributes & PropertyAttribute::Accessor));
    ASSERT(!value.isCustomGetterSetterSlow() && !(attributes & PropertyAttribute::CustomAccessorOrValue));
    PutPropertySlot slot(this);
    return putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, value, attributes, slot).isNull();
}

inline bool JSObject::putDirect(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes, PutPropertySlot& slot)
{
    ASSERT(!value.isGetterSetterSlow());
    ASSERT(!value.isCustomGetterSetterSlow());
    return putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, value, attributes, slot).isNull();
}

inline bool JSObject::putDirect(VM& vm, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!value.isGetterSetterSlow());
    ASSERT(!value.isCustomGetterSetterSlow());
    return putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, value, 0, slot).isNull();
}

constexpr inline intptr_t offsetInButterfly(PropertyOffset offset)
{
    return offsetInOutOfLineStorage(offset) + Butterfly::indexOfPropertyStorage();
}

inline size_t JSObject::butterflyPreCapacity()
{
    if (hasIndexingHeader()) [[unlikely]]
        return butterfly()->indexingHeader()->preCapacity(structure());
    return 0;
}

inline size_t JSObject::butterflyTotalSize()
{
    Structure* structure = this->structure();
    Butterfly* butterfly = this->butterfly();
    size_t preCapacity;
    size_t indexingPayloadSizeInBytes;
    bool hasIndexingHeader = this->hasIndexingHeader();

    if (hasIndexingHeader) [[unlikely]] {
        preCapacity = butterfly->indexingHeader()->preCapacity(structure);
        indexingPayloadSizeInBytes = butterfly->indexingHeader()->indexingPayloadSizeInBytes(structure);
    } else {
        preCapacity = 0;
        indexingPayloadSizeInBytes = 0;
    }

    return Butterfly::totalSize(preCapacity, structure->outOfLineCapacity(), hasIndexingHeader, indexingPayloadSizeInBytes);
}

inline int indexRelativeToBase(PropertyOffset offset)
{
    if (isOutOfLineOffset(offset))
        return offsetInOutOfLineStorage(offset) + Butterfly::indexOfPropertyStorage();
    static_assert(!(JSObject::offsetOfInlineStorage() % sizeof(EncodedJSValue)));
    return JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offsetInInlineStorage(offset);
}

inline int offsetRelativeToBase(PropertyOffset offset)
{
    if (isOutOfLineOffset(offset))
        return offsetInOutOfLineStorage(offset) * sizeof(EncodedJSValue) + Butterfly::offsetOfPropertyStorage();
    return JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue);
}

// Returns the maximum offset (away from zero) a load instruction will encode.
inline size_t maxOffsetRelativeToBase(PropertyOffset offset)
{
    ptrdiff_t addressOffset = offsetRelativeToBase(offset);
#if USE(JSVALUE32_64)
    if (addressOffset >= 0)
        return static_cast<size_t>(addressOffset) + OBJECT_OFFSETOF(EncodedValueDescriptor, asBits.tag);
#endif
    return static_cast<size_t>(addressOffset);
}

static_assert(!(sizeof(JSObjectWithButterfly) % sizeof(WriteBarrierBase<Unknown>)), "JSObject inline storage has correct alignment");
static_assert(sizeof(JSObject) == sizeof(JSCell), "JSObject should be the same size as JSCell (no butterfly)");
static_assert(JSObject::butterflyOffset() == sizeof(JSObject), "butterfly offset must be right after JSObject");

ALWAYS_INLINE Identifier makeIdentifier(VM& vm, ASCIILiteral literal)
{
    return Identifier::fromString(vm, literal);
}

ALWAYS_INLINE Identifier makeIdentifier(VM&, const Identifier& name)
{
    return name;
}

bool validateAndApplyPropertyDescriptor(JSGlobalObject*, JSObject*, PropertyName, bool isExtensible,
    const PropertyDescriptor& descriptor, bool isCurrentDefined, const PropertyDescriptor& current, bool throwException);

JS_EXPORT_PRIVATE NEVER_INLINE bool ordinarySetSlow(JSGlobalObject*, JSObject*, PropertyName, JSValue, JSValue receiver, bool shouldThrow);
JS_EXPORT_PRIVATE NEVER_INLINE bool ordinarySetWithOwnDescriptor(JSGlobalObject*, JSObject*, PropertyName, JSValue, JSValue receiver, PropertyDescriptor&& ownDescriptor, bool shouldThrow);

bool setterThatIgnoresPrototypeProperties(JSGlobalObject*, JSValue thisValue, JSObject* homeObject, PropertyName, JSValue, bool shouldThrow);

// Helper for defining native functions, if you're not using a static hash table.
// Use this macro from within finishCreation() methods in prototypes. This assumes
// you've defined variables called globalObject, globalObject, and vm, and they
// have the expected meanings.
#define JSC_NATIVE_INTRINSIC_FUNCTION(jsName, cppName, attributes, length, implementationVisibility, intrinsic) \
    putDirectNativeFunction(\
        vm, globalObject, makeIdentifier(vm, (jsName)), (length), cppName, \
        (implementationVisibility), (intrinsic), (attributes))

#define JSC_NATIVE_INTRINSIC_FUNCTION_WITHOUT_TRANSITION(jsName, cppName, attributes, length, implementationVisibility, intrinsic) \
    putDirectNativeFunctionWithoutTransition(\
        vm, globalObject, makeIdentifier(vm, (jsName)), (length), cppName, \
        (implementationVisibility), (intrinsic), (attributes))

// As above, but this assumes that the function you're defining doesn't have an
// intrinsic.
#define JSC_NATIVE_FUNCTION(jsName, cppName, attributes, length, implementationVisibility) \
    JSC_NATIVE_INTRINSIC_FUNCTION(jsName, cppName, (attributes), (length), (implementationVisibility), JSC::NoIntrinsic)

#define JSC_NATIVE_FUNCTION_WITHOUT_TRANSITION(jsName, cppName, attributes, length, implementationVisibility) \
    JSC_NATIVE_INTRINSIC_FUNCTION_WITHOUT_TRANSITION(jsName, cppName, (attributes), (length), (implementationVisibility), JSC::NoIntrinsic)

// Identical helpers but for builtins. Note that currently, we don't support builtins that are
// also intrinsics, but we probably will do that eventually.
#define JSC_BUILTIN_FUNCTION(jsName, generatorName, attributes) \
    putDirectBuiltinFunction(\
        vm, globalObject, makeIdentifier(vm, (jsName)), (generatorName)(vm), (attributes))

#define JSC_BUILTIN_FUNCTION_WITHOUT_TRANSITION(jsName, generatorName, attributes) \
    putDirectBuiltinFunctionWithoutTransition(\
        vm, globalObject, makeIdentifier(vm, (jsName)), (generatorName)(vm), (attributes))

#define JSC_TO_STRING_TAG_WITHOUT_TRANSITION() \
    putDirectWithoutTransition(vm, vm.propertyNames->toStringTagSymbol, \
        jsNontrivialString(vm, info()->className), JSC::PropertyAttribute::DontEnum | JSC::PropertyAttribute::ReadOnly)

// Helper for defining native getters on properties.
#define JSC_NATIVE_INTRINSIC_GETTER(jsName, cppName, attributes, intrinsic)  \
    putDirectNativeIntrinsicGetter(\
        vm, globalObject, makeIdentifier(vm, (jsName)), (cppName), \
        (intrinsic), ((attributes) | JSC::PropertyAttribute::Accessor))

#define JSC_NATIVE_INTRINSIC_GETTER_WITHOUT_TRANSITION(jsName, cppName, attributes, intrinsic)  \
    putDirectNativeIntrinsicGetterWithoutTransition(\
        vm, globalObject, makeIdentifier(vm, (jsName)), (cppName), \
        (intrinsic), ((attributes) | JSC::PropertyAttribute::Accessor))

#define JSC_NATIVE_GETTER(jsName, cppName, attributes) \
    JSC_NATIVE_INTRINSIC_GETTER((jsName), (cppName), (attributes), JSC::NoIntrinsic)

#define JSC_NATIVE_GETTER_WITHOUT_TRANSITION(jsName, cppName, attributes) \
    JSC_NATIVE_INTRINSIC_GETTER_WITHOUT_TRANSITION((jsName), (cppName), (attributes), JSC::NoIntrinsic)


#define STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(DerivedClass, BaseClass) \
    static_assert(sizeof(DerivedClass) == sizeof(BaseClass)); \
    static_assert(DerivedClass::destroy == BaseClass::destroy);

// Defined here rather than in JSCJSValue.h because they need JSObject to be complete.
inline bool JSValue::put(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    if (!isCell()) [[unlikely]]
        return putToPrimitive(globalObject, propertyName, value, slot);

    return asCell()->methodTable()->put(asCell(), globalObject, propertyName, value, slot);
}

ALWAYS_INLINE bool JSValue::putInline(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    if (!isCell()) [[unlikely]]
        return putToPrimitive(globalObject, propertyName, value, slot);
    return asCell()->putInline(globalObject, propertyName, value, slot);
}

inline bool JSValue::putByIndex(JSGlobalObject* globalObject, unsigned propertyName, JSValue value, bool shouldThrow)
{
    if (!isCell()) [[unlikely]]
        return putToPrimitiveByIndex(globalObject, propertyName, value, shouldThrow);

    return asCell()->methodTable()->putByIndex(asCell(), globalObject, propertyName, value, shouldThrow);
}

ALWAYS_INLINE JSValue JSValue::getPrototype(JSGlobalObject* globalObject) const
{
    if (isObject())
        return asObject(asCell())->getPrototype(globalObject);
    return synthesizePrototype(globalObject);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
