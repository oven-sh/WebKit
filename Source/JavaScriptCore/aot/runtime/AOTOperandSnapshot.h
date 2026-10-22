/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTRecordTypes.h"
#include "IndexingType.h"
#include "JSCJSValue.h"
#include "JSType.h"
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSObject;
class VM;

namespace AOT {

enum class OperandChange : uint8_t {
    PropertyAdded,
    PropertyRemoved,
    PropertyStored,
    AttributesChanged,
    IntegrityChanged,
    PrototypeChanged,
    ShapeLost,
    PristineStateLost,
    ElementsChanged,
    ArrayStorageChanged,
    InternalStateChanged,
    BytesChanged,
    ByteStorageChanged,
};

ASCIILiteral nameOf(OperandChange);

inline constexpr OperandUses usesThatStoreProperties { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor };
inline constexpr OperandUses usesThatWriteElements = usesThatStoreProperties | OperandUses { OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::DeletesByKey };

// Any one of these uses declares the change. Empty: the change is one of representation, which only the written heaps declare.
constexpr OperandUses usesDeclaring(OperandChange change)
{
    switch (change) {
    case OperandChange::PropertyAdded:
    case OperandChange::PropertyStored:
        return usesThatStoreProperties;
    case OperandChange::PropertyRemoved:
        return { OperandUse::DeletesByKey, OperandUse::MovesElements };
    case OperandChange::AttributesChanged:
        return { OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity };
    case OperandChange::IntegrityChanged:
        return { OperandUse::ChangesIntegrity };
    case OperandChange::PrototypeChanged:
        return { OperandUse::ChangesPrototype };
    case OperandChange::ShapeLost:
        return usesThatStoreProperties | OperandUses { OperandUse::DeletesByKey, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype };
    case OperandChange::ElementsChanged:
        return usesThatWriteElements;
    case OperandChange::ArrayStorageChanged:
        // An array that is frozen in place shares its elements from then on, and one whose prototype may hold indexed accessors asks it at every store.
        return usesThatWriteElements | OperandUses { OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype };
    case OperandChange::InternalStateChanged:
    case OperandChange::BytesChanged:
        return { OperandUse::WritesContents, OperandUse::MovesElements };
    case OperandChange::PristineStateLost:
    case OperandChange::ByteStorageChanged:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// A call that makes the change writes at least one of these heaps. Internal state has the heaps of the operand's class.
constexpr AbstractHeaps heapsHolding(OperandChange change)
{
    switch (change) {
    case OperandChange::PropertyAdded:
    case OperandChange::PropertyRemoved:
    case OperandChange::PropertyStored:
    case OperandChange::AttributesChanged:
        return { AbstractHeap::NamedProperties, AbstractHeap::ObjectFields };
    case OperandChange::IntegrityChanged:
    case OperandChange::ShapeLost:
        return { AbstractHeap::NamedProperties, AbstractHeap::ObjectFields, AbstractHeap::OrderWords, AbstractHeap::ShapeWords };
    case OperandChange::PrototypeChanged:
        return { AbstractHeap::PrototypeLinks };
    case OperandChange::PristineStateLost:
        return { AbstractHeap::PrototypeLinks, AbstractHeap::NamedProperties, AbstractHeap::ArrayStorage };
    case OperandChange::ElementsChanged:
        return { AbstractHeap::Elements };
    case OperandChange::ArrayStorageChanged:
        return { AbstractHeap::ArrayStorage };
    case OperandChange::InternalStateChanged:
        return { };
    case OperandChange::BytesChanged:
        return { AbstractHeap::TypedArrayElements };
    case OperandChange::ByteStorageChanged:
        return { AbstractHeap::TypedArrayStorage };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct CapturedProperty {
    RefPtr<UniquedStringImpl> key;
    unsigned attributes { 0 };
    EncodedJSValue value { 0 };
};

struct OperandSnapshot {
    JSType type { CellType };
    bool isExtensible { true };
    bool isPristine { false };
    bool hasTooManyProperties { false };
    bool createsPropertiesLazily { false };
    bool isClassPrototypeWithoutMembers { false };
    IndexingType indexingMode { NonArray };
    EncodedJSValue prototype { 0 };
    uint32_t orderWord { 0 };
    uint32_t shapeWord { 0 };
    uint32_t length { 0 };
    const void* bytes { nullptr };
    size_t byteLength { 0 };
    uint64_t hashOfBytes { 0 };
    Vector<CapturedProperty> properties;
    Vector<uint64_t> fields;
    Vector<uint64_t> elements;
    Vector<uint64_t, 4> internalState;
};

struct OperandDifference {
    OperandChange change;
    String detail;
    RefPtr<UniquedStringImpl> key { };
    EncodedJSValue arrived { 0 };
};

// The heaps that hold the internal state of an instance of the object's class.
AbstractHeaps heapsOfClassOf(JSObject*);
String describe(AbstractHeaps);

// A proxy runs code when it is looked at, and a global object creates its properties on demand.
bool canCapture(JSValue operand);
// Allocates no cell, runs no code and creates no property.
OperandSnapshot capture(JSObject*);
Vector<OperandDifference> differencesBetween(VM&, JSObject*, const OperandSnapshot& before, const OperandSnapshot& after);

// What compiled code calls in the place of an operation does to the operands no more than the operation is said to do: the passes and B3 keep whatever lies in a heap that the call does not write.
JS_EXPORT_PRIVATE void photographOperand(VM&, JSValue operand, AbstractHeaps written, unsigned position);
JS_EXPORT_PRIVATE void compareOperandWithPhotograph(VM&, JSValue operand, uint32_t positionAndCallee, FunctionID, Typed::ValueID);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
