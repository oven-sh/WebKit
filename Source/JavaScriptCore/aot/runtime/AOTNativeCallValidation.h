/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeCallExpectation.h"
#include "AOTRemovableFeature.h"
#include "AOTRuntimeValidation.h"
#include "AOTVector.h"
#include "ArgList.h"
#include "CodeSpecializationKind.h"
#include "NativeFunction.h"
#include <optional>
#include <wtf/CommaPrinter.h>
#include <wtf/Noncopyable.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT {

struct NativeCall {
    JSGlobalObject* realm;
    JSObject* callee;
    CodeSpecializationKind kind;
    JSValue thisValue;
    ArgList arguments;
};

// The table of built-ins leaves an executable together with the code generator. The other validators of the run time need neither.
ALWAYS_INLINE bool validatesNativeCalls(VM& vm) { return validatesRuntime(vm) && !isFeatureRemoved(RemovableFeature::ImageCompiler); }

// Compares what a native function does to its operands with what the table of built-ins declares and with what the compiler believed about the call.
class NativeCallValidationScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(NativeCallValidationScope);
public:
    ALWAYS_INLINE NativeCallValidationScope(VM& vm, const NativeCall& call)
    {
        if (validatesNativeCalls(vm)) [[unlikely]]
            open(vm, call, NativeCallExpectation::take(call.realm, call.callee));
    }

    ALWAYS_INLINE NativeCallValidationScope(VM& vm, const NativeCall& call, const NativeCallExpectation& expectation)
    {
        if (validatesNativeCalls(vm)) [[unlikely]]
            open(vm, call, expectation);
    }

    ALWAYS_INLINE ~NativeCallValidationScope()
    {
        if (m_checker) [[unlikely]]
            close();
    }

    ALWAYS_INLINE void didReturn(JSValue result)
    {
        if (m_checker) [[unlikely]]
            m_result = result;
    }

    ALWAYS_INLINE bool finishAndAskWhetherCodeRan()
    {
        return !m_checker || close();
    }

private:
    class Checker;

    JS_EXPORT_PRIVATE void open(VM&, const NativeCall&, const NativeCallExpectation&);
    JS_EXPORT_PRIVATE bool close();

    Checker* m_checker { nullptr };
    JSValue m_result;
};

// For tests of the table of built-ins. Where BUN_AOT_CHECKED_NATIVE_CALLS names a file, a line for each entry whose calls were checked is appended to it, at exit and in front of the crash of a failed validation:
// the entry, then how many of its calls returned, threw, changed an operand, and came with what the compiler believed about the call, separated by tabs.
// A hardened build does not read the variable and writes no such file.
void writeCheckedNativeCalls();

JSC_DECLARE_HOST_FUNCTION(callNativeFunctionUnderValidation);
JSC_DECLARE_HOST_FUNCTION(constructNativeFunctionUnderValidation);

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
    // Of an element that changed.
    std::optional<uint32_t> index { };
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
