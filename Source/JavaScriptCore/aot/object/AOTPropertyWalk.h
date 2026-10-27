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
class Symbol;
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
// Elements have no place in the order of creation. A walk that leaves them out still refuses an element that is an accessor.
enum class Elements : bool { NeedGenericPath, AreLeftOut };

ALWAYS_INLINE bool isCompiledObject(const JSCell* cell) { return cell->type() == CompiledObjectType; }
// What hasAnyArrayStorage(indexingType) answers for the ordinary object that would stand in the place of this compiled one.
JS_EXPORT_PRIVATE bool wouldHaveArrayStorage(JSObject* compiledObject);
JS_EXPORT_PRIVATE bool convertsToOrdinaryObject(const ClassInfo* from, const ClassInfo* to);
JS_EXPORT_PRIVATE Result forEachOwnProperty(VM&, JSObject*, Filter, const ScopedLambda<IterationStatus(const OwnProperty&)>&);
// Every own property but the private names, strings and symbols alike, enumerable or not, in the order of their creation: what the Structure of the ordinary object that would stand in the place of this compiled one lists.
JS_EXPORT_PRIVATE Result forEachOwnPropertyAsCreated(VM&, JSObject*, const ScopedLambda<IterationStatus(const OwnProperty&)>&, Elements = Elements::NeedGenericPath);
JS_EXPORT_PRIVATE void materializeLazyProperties(VM&, JSObject*);
// The library keeps a field under the symbol in a form of its own. Whoever looks under the symbol with a key that is a value first calls the function on the compiled object that has the field.
JS_EXPORT_PRIVATE void prepareHoldersBeforeLookupsUnder(JSGlobalObject*, Symbol*, JSObject* function);
// Whether the compiled object has an own enumerable property, under a string or a symbol. The answer shows no name and no order, so it is no listing of the keys. Runs no code.
JS_EXPORT_PRIVATE bool hasOwnEnumerableProperty(JSGlobalObject*, JSObject* compiledObject);
JSCellButterfly* tryGetCachedOwnKeys(VM&, JSObject*, Filter);
Presence hasOwnProperty(JSObject*, UniquedStringImpl*);
Presence hasPropertyOnChain(Structure*, UniquedStringImpl*);
// A compiled object whose Structure says nothing about its extra properties gets the Structure that lists them. Whether it has another Structure now. Collects nothing.
bool listExtraPropertiesInStructure(VM&, JSObject*);
JS_EXPORT_PRIVATE JSValue getDirect(VM&, const JSObject*, PropertyName, unsigned* attributesOrNull);
// What a read by name finds on the compiled object itself.
JS_EXPORT_PRIVATE Presence tryGetOwnValue(JSObject* compiledObject, UniquedStringImpl*, JSValue&);
JS_EXPORT_PRIVATE void putDirect(VM&, JSObject*, PropertyName, JSValue, unsigned attributes);
void setIntegrity(VM&, JSObject*, Integrity);
Presence testIntegrity(JSObject*, Integrity);
bool tryAssign(VM&, JSObject* compiledTarget, JSObject* source);
// What canPerformFastPutInlineExcludingProto() and isStructureExtensible() would answer for the target if an ordinary object stood in the place of each compiled one, of the target and of those above it.
bool wouldPerformFastPut(VM&, JSObject* target);
JS_EXPORT_PRIVATE void didBecomePrototype(VM&, JSObject*);
JS_EXPORT_PRIVATE void willPutDirectOnClosure(VM&, JSObject* closure, UniquedStringImpl*);
JS_EXPORT_PRIVATE void didPutDirectOnClosure(VM&, JSObject* closure, UniquedStringImpl*);
bool hasIndexedProperties(JSObject*);
JSObject* objectHoldingIndexedProperties(const JSObject*);

} } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
