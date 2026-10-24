/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTRecordTypes.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSCJSValue.h"
#include "JSType.h"
#include <wtf/CommaPrinter.h>
#include <wtf/RefPtr.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSObject;
class VM;

namespace AOT {

enum class OperandChange : uint8_t {
    PropertyAdded, PropertyRemoved, PropertyStored, AttributesChanged, IntegrityChanged, PrototypeChanged, ShapeLost,
    PristineStateLost, ElementsChanged, ArrayStorageChanged, InternalStateChanged, BytesChanged, ByteStorageChanged,
};

struct OperandChangeRow {
    ASCIILiteral name;
    // Any one of these uses declares the change. Empty: the change is one of representation, which only the written heaps declare.
    OperandUses uses;
    // A call that makes the change writes at least one of these heaps. Internal state has the heaps of the operand's class.
    AbstractHeaps heaps;
};

const OperandChangeRow& rowOf(OperandChange);

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

AbstractHeaps heapsOfClassOf(JSObject*);
String describe(const OperandDifference&);

String describeMembers(const auto& set)
{
    StringPrintStream out;
    CommaPrinter comma("|"_s);
    for (auto member : set)
        out.print(comma, nameOf(member));
    return out.toString();
}

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
