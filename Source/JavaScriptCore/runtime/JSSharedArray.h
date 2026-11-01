/*
 * Copyright (C) 2026 Jordan Hall. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSObject.h"
#include "SharedStructBlock.h"
#include "VM.h"

namespace JSC {

// An instance of `SharedArray` (https://github.com/tc39/proposal-structs): an exotic object with a null prototype
// that is not extensible, has a read-only, non-configurable own `length` and holds `length` integer-indexed
// elements that are writable, enumerable, non-configurable data properties. Like JSSharedStruct it is a view over
// a SharedStructBlock, here a block without a shape whose slots are the elements, so every agent that holds the
// array shares its elements through the block's sequentially consistent atomic slots. Only values that can be
// shared (see canBeShared) can be stored. An index outside the range is not a property: reading it gives
// undefined and writing it fails the way a write to a frozen array does.
//
// A global object has one wrapper per block (see SharedStructWrapperCache), so an array read from an element
// of another shared array or struct is the same object every time.
class JSSharedArray final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | GetOwnPropertySlotIsImpureForPropertyAbsence | IsImmutablePrototypeExoticObject;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell* cell) { static_cast<JSSharedArray*>(cell)->JSSharedArray::~JSSharedArray(); }

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.sharedArraySpace<mode>();
    }

    // Use wrapSharedArrayBlock() to get the one wrapper of a block.
    static JSSharedArray* create(VM&, Structure*, Ref<SharedStructBlock>&&);
    // A null prototype and no extensions.
    static Structure* createStructure(VM&, JSGlobalObject*);

    DECLARE_INFO;

    SharedStructBlock& block() const LIFETIME_BOUND { return m_block.get(); }
    unsigned length() const { return m_block->fieldCount(); }

    // Objects made for the value (wrappers) are created in the global object of the array.
    JSValue loadElement(unsigned index);
    // Throws a TypeError, and returns false, if the value cannot be shared.
    bool storeElement(JSGlobalObject*, unsigned index, JSValue);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

private:
    JSSharedArray(VM&, Structure*, Ref<SharedStructBlock>&&);
    DECLARE_DEFAULT_FINISH_CREATION;

    Ref<SharedStructBlock> m_block;
};

// The SharedArray global: `new SharedArray(length)`. It has no prototype property, because instances have a null
// prototype, and it throws without new.
JSObject* createSharedArrayConstructor(VM&, JSGlobalObject*);

// The embedder API, next to sharedStructBlockOf() and wrapSharedStructBlock() in JSSharedStruct.h. A structured
// clone passes the block of a SharedArray to the other agent by reference and wraps it again there.
//
// The block of a SharedArray, or null if `value` is not one.
JS_EXPORT_PRIVATE RefPtr<SharedStructBlock> sharedArrayBlockOf(JSValue);
// The SharedArray of `block` in `globalObject`: the same object every time for one block.
JS_EXPORT_PRIVATE JSObject* wrapSharedArrayBlock(JSGlobalObject*, Ref<SharedStructBlock>&&);

// Atomics.load/store/exchange/compareExchange(sharedArray, index, ...), called by AtomicsObject.cpp.
EncodedJSValue sharedArrayAtomicLoad(JSGlobalObject*, JSSharedArray*, JSValue key);
EncodedJSValue sharedArrayAtomicStore(JSGlobalObject*, JSSharedArray*, JSValue key, JSValue value);
EncodedJSValue sharedArrayAtomicExchange(JSGlobalObject*, JSSharedArray*, JSValue key, JSValue value);
EncodedJSValue sharedArrayAtomicCompareExchange(JSGlobalObject*, JSSharedArray*, JSValue key, JSValue expected, JSValue replacement);

} // namespace JSC
