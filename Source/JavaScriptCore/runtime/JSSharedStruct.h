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

class Symbol;

// An instance of a `shared struct` (https://github.com/tc39/proposal-structs): an object with a null prototype
// that is not extensible and has the fixed fields of its class chain as writable, enumerable, non-configurable data
// properties. It is a view over a SharedStructBlock, the thread-safe native storage that every agent holding the
// struct shares. Every field access goes through the block's sequentially consistent atomic slots, and only values
// that can be shared (see canBeShared) can be stored.
//
// The fields are real own properties of the object's Structure, one transition chain per field list, whose
// values are a CustomGetterSetter (attribute CustomValue) that reads or writes the slot of the block. This is
// what lets the inline caches of every JIT tier cache field loads and stores: the cache holds the Structure and
// the native accessor, and the accessor gets the object that has the property as its this value. Nothing about
// the object is exotic except that the values live in the block.
//
// Each global object has one wrapper per block, so reading a field that holds another shared struct returns the
// same object every time. Wrappers made while reading an object are created in the global object of that object
// (its realm), never in a foreign one, whichever code reads it.
class JSSharedStruct final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | IsImmutablePrototypeExoticObject;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell* cell) { static_cast<JSSharedStruct*>(cell)->JSSharedStruct::~JSSharedStruct(); }

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.sharedStructSpace<mode>();
    }

    // Use wrapSharedStructBlock() to get the one wrapper of a block.
    static JSSharedStruct* create(JSGlobalObject*, Ref<SharedStructBlock>&&);
    // A null prototype, extensible until the fields have been defined.
    static Structure* createStructure(VM&, JSGlobalObject*);

    DECLARE_INFO;

    SharedStructBlock& block() const LIFETIME_BOUND { return m_block.get(); }

    // The index of the field a property key names, or none.
    std::optional<unsigned> fieldIndex(PropertyName) const;
    JSValue loadField(unsigned index);
    // Throws a TypeError, and returns false, if the value cannot be shared.
    bool storeField(JSGlobalObject*, unsigned index, JSValue);

    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    // A field named by an array index is not a property of the structure: these serve it.
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    // [[Set]] ignores the receiver and writes the own field, [[Delete]] is false for every key.
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);

private:
    JSSharedStruct(VM&, Structure*, Ref<SharedStructBlock>&&);
    DECLARE_DEFAULT_FINISH_CREATION;

    // Adds the fields as properties and makes the object non-extensible.
    void defineFieldProperties(JSGlobalObject*);

    Ref<SharedStructBlock> m_block;
};

// The wrappers a global object has made for blocks and for the state of mutexes and conditions, so that one
// block or one mutex is one object in a global object for as long as it is alive. Wrappers are weak.
class SharedStructWrapperCache {
public:
    SharedStructWrapperCache();
    ~SharedStructWrapperCache();

    JSObject* find(const void* key) const;
    void add(VM&, const void* key, JSObject*);
    // The names of the fields of a shape that are properties (not array indices), in order, as identifiers of the
    // global object that asks (a symbol is that global object's symbol). Shapes live for the whole process.
    const Vector<Identifier>& fieldNames(JSGlobalObject*, const SharedStructShape&);

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};

// The SharedSymbol of a symbol of this global object, made when it is first shared.
JS_EXPORT_PRIVATE Ref<SharedSymbol> sharedSymbolOf(JSGlobalObject*, Symbol*);

// Whether `value` can be stored in a field of a shared struct and passed to another agent: a primitive other
// than a symbol, a shared struct, a SharedArray, an Atomics.Mutex or an Atomics.Condition. Ordinary objects,
// functions and symbols cannot.
JS_EXPORT_PRIVATE bool canBeShared(VM&, JSValue);

// The JavaScript value of a slot, or an empty value with an exception pending. Shared by shared structs and
// SharedArrays, whose slots hold the same kinds of values.
// Objects it has to make (wrappers) are created in `realm`.
JSValue sharedSlotToJS(JSGlobalObject* realm, const SharedSlotValue&);
// The slot value of a JavaScript value. Throws a TypeError, and returns nothing, if it cannot be shared.
std::optional<SharedSlotValue> sharedSlotFromJS(JSGlobalObject*, JSValue);
// The block behind a shared struct or a SharedArray, or null for any other value.
SharedStructBlock* sharedBlockOfValue(JSValue);

// The atomic operations on slot `index` of a block, for Atomics.load/store/exchange/compareExchange on a shared
// struct field or a SharedArray element. Errors are thrown in `globalObject`; the objects they make for the
// result are created in `realm`, the global object of the struct or array that is operated on, so that the result
// is the same object as a plain read of it gives.
EncodedJSValue sharedSlotAtomicLoad(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock&, unsigned index);
EncodedJSValue sharedSlotAtomicStore(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock&, unsigned index, JSValue);
EncodedJSValue sharedSlotAtomicExchange(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock&, unsigned index, JSValue);
EncodedJSValue sharedSlotAtomicCompareExchange(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock&, unsigned index, JSValue expected, JSValue replacement);

// The embedder API. A structured clone passes the block of a shared struct to the other agent by reference
// and wraps it again there.
//
// The block of a shared struct, or null if `value` is not one.
JS_EXPORT_PRIVATE RefPtr<SharedStructBlock> sharedStructBlockOf(JSValue);
// The shared struct object of `block` in `globalObject`: the same object every time for one block. A block that
// belongs to a SharedArray gets a SharedArray view, so this also serves wrapSharedArrayBlock().
JS_EXPORT_PRIVATE JSObject* wrapSharedStructBlock(JSGlobalObject*, Ref<SharedStructBlock>&&);

// Atomics.load/store/exchange/compareExchange(sharedStruct, "field", ...), called by AtomicsObject.cpp.
EncodedJSValue sharedStructAtomicLoad(JSGlobalObject*, JSSharedStruct*, JSValue key);
EncodedJSValue sharedStructAtomicStore(JSGlobalObject*, JSSharedStruct*, JSValue key, JSValue value);
EncodedJSValue sharedStructAtomicExchange(JSGlobalObject*, JSSharedStruct*, JSValue key, JSValue value);
EncodedJSValue sharedStructAtomicCompareExchange(JSGlobalObject*, JSSharedStruct*, JSValue key, JSValue expected, JSValue replacement);

} // namespace JSC
