/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSCell.h>
#include <JavaScriptCore/PropertyName.h>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

class JSCellButterfly;
class JSGlobalObject;
class JSObject;
class Structure;
class VM;
struct ClassInfo;

namespace AOT::PropertyWalk {

struct OwnProperty {
    UniquedStringImpl* name;
    JSValue value;
    unsigned attributes;
};

enum class Filter : uint8_t { EnumerableStrings, Strings, StringsAndSymbols };
enum class Result : uint8_t { Completed, Stopped, NeedsGenericPath };
enum class Presence : uint8_t { Present, Absent, Unknown };
enum class Integrity : uint8_t { NonExtensible, Sealed, Frozen };

ALWAYS_INLINE bool isCompiledObject(const JSCell* cell) { return cell->type() == CompiledObjectType; }
// What hasAnyArrayStorage(indexingType) answers for the ordinary object that would stand in the place of this compiled one.
JS_EXPORT_PRIVATE bool wouldHaveArrayStorage(JSObject* compiledObject);
JS_EXPORT_PRIVATE bool convertsToOrdinaryObject(const ClassInfo* from, const ClassInfo* to);
JS_EXPORT_PRIVATE Result forEachOwnProperty(VM&, JSObject*, Filter, const ScopedLambda<IterationStatus(const OwnProperty&)>&);
// Every own property but the private names, strings and symbols alike, enumerable or not, in the order of their creation:
// what the Structure of the ordinary object that would stand in the place of this compiled one lists.
JS_EXPORT_PRIVATE Result forEachOwnPropertyAsCreated(VM&, JSObject*, const ScopedLambda<IterationStatus(const OwnProperty&)>&);
JS_EXPORT_PRIVATE void materializeLazyProperties(VM&, JSObject*);
// Whether the compiled object has an own enumerable property, under a string or a symbol. The answer shows no name and no order, so it is no listing of the keys. Runs no code.
JS_EXPORT_PRIVATE bool hasOwnEnumerableProperty(JSGlobalObject*, JSObject* compiledObject);
JSCellButterfly* tryGetCachedOwnKeys(VM&, JSObject*, Filter);
Presence hasOwnProperty(JSObject*, UniquedStringImpl*);
Presence hasPropertyOnChain(Structure*, UniquedStringImpl*);
// A compiled object whose Structure says nothing about its extra properties gets the Structure that lists them. Whether it has another Structure now. Collects nothing.
bool listExtraPropertiesInStructure(VM&, JSObject*);
JS_EXPORT_PRIVATE JSValue getDirect(VM&, const JSObject*, PropertyName, unsigned* attributesOrNull);
// What a read by name finds on the compiled object itself. Present: a plain value, which is handed over. Absent: the object has no such property.
// Unknown: getOwnPropertySlot() answers. Runs no code.
JS_EXPORT_PRIVATE Presence tryGetOwnValue(JSObject* compiledObject, UniquedStringImpl*, JSValue&);
JS_EXPORT_PRIVATE void putDirect(VM&, JSObject*, PropertyName, JSValue, unsigned attributes);
void setIntegrity(VM&, JSObject*, Integrity);
Presence testIntegrity(JSObject*, Integrity);
bool tryAssign(VM&, JSObject* compiledTarget, JSObject* source);
JS_EXPORT_PRIVATE void didBecomePrototype(VM&, JSObject*);
JS_EXPORT_PRIVATE void didAddPropertyToClosure(VM&, JSObject* closure, UniquedStringImpl*);
bool hasIndexedProperties(JSObject*);
JSObject* objectHoldingIndexedProperties(const JSObject*);

} } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
