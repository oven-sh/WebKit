/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/MarkedVector.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSCell.h>
#include <JavaScriptCore/PropertyName.h>
#include <array>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

class JSCellButterfly;
class JSGlobalObject;
class JSObject;
class PropertySlot;
class Structure;
class Symbol;
class VM;
struct ClassInfo;

namespace AOT {
struct CachedPropertyList;
}

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
// All enumerable own properties of a compiled object without elements, in order, getters called. NeedsGenericPath before anything has run; Stopped if a getter threw.
// A getter may delete a name and collect, and the object may be all that holds the name. heldNames keeps what names points to.
// willRunCode is told before code of the program runs: whoever stores what has been read so far does it there. Behind a getter mayReadAhead says whether several properties may still be read before the first of them is stored:
// where it says no, willRunCode is told before each further read. A name that isLeftOut answers for is not read.
struct GetterWalkHooks {
    const ScopedLambda<void()>* willRunCode { nullptr };
    const ScopedLambda<bool()>* mayReadAhead { nullptr };
    const ScopedLambda<bool(UniquedStringImpl*)>* isLeftOut { nullptr };
    bool refusesProtoName { false };
};
bool hasGetterToCall(JSObject* compiledObject);
Result readOwnPropertiesCallingGetters(JSGlobalObject*, JSObject*, Vector<RefPtr<UniquedStringImpl>, 32>& heldNames, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values, GetterWalkHooks = { });
JS_EXPORT_PRIVATE void materializeLazyProperties(VM&, JSObject*);
// The library keeps a field under the symbol in a form of its own. Whoever looks under the symbol with a key that is a value first calls the function on the compiled object that has the field.
JS_EXPORT_PRIVATE void prepareHoldersBeforeLookupsUnder(JSGlobalObject*, Symbol*, JSObject* function);
// Whether the compiled object has an own enumerable property, under a string or a symbol. The answer shows no name and no order, so it is no listing of the keys. Runs no code.
JS_EXPORT_PRIVATE bool hasOwnEnumerableProperty(JSGlobalObject*, JSObject* compiledObject);
JSCellButterfly* tryGetCachedOwnKeys(VM&, JSObject*, Filter);
Presence hasOwnProperty(JSObject*, UniquedStringImpl*);
const CachedPropertyList* tryGetListOfOwnProperties(VM&, JSObject* compiledObject, JSObject*& extraProperties);
Presence hasPropertyOnChain(Structure*, UniquedStringImpl*);
// A compiled object whose Structure says nothing about its extra properties gets the Structure that lists them. Whether it has another Structure now. Collects nothing.
bool listExtraPropertiesInStructure(VM&, JSObject*);
JS_EXPORT_PRIVATE JSValue getDirect(VM&, const JSObject*, PropertyName, unsigned* attributesOrNull);
// What JSObject::getOwnPropertySlot() finds on the ordinary object that would stand in the place of this one. Runs no code.
JS_EXPORT_PRIVATE bool ordinaryGetOwnPropertySlot(JSGlobalObject*, JSObject*, PropertyName, PropertySlot&);
// What a read by name finds on the compiled object itself.
JS_EXPORT_PRIVATE Presence tryGetOwnValue(JSObject* compiledObject, UniquedStringImpl*, JSValue&);
// The results of reading enumerable, configurable, value, writable, get and set on the compiled object itself, in that order. An absent property leaves its value empty. Returns false if any result is unknown.
using DescriptorMembers = std::array<JSValue, 6>;
JS_EXPORT_PRIVATE bool tryReadDescriptorMembers(JSObject* compiledObject, DescriptorMembers&);
JS_EXPORT_PRIVATE void putDirect(VM&, JSObject*, PropertyName, JSValue, unsigned attributes);
// May run code and throw. Object.freeze() calls it before setIntegrity(), which does neither.
void prepareForFreeze(JSObject* compiledObject);
void setIntegrity(VM&, JSObject*, Integrity);
Presence testIntegrity(JSObject*, Integrity);
// Calls the getters of a compiled source, which may throw.
bool tryAssign(VM&, JSObject* compiledTarget, JSObject* source);
// Object.assign(target, source) of a compiled source with a getter, in the steps of the language. False before anything has run.
bool tryAssignCallingGetters(JSGlobalObject*, JSObject* target, JSObject* source);
// What canPerformFastPutInlineExcludingProto() and isStructureExtensible() would answer for the target if an ordinary object stood in the place of each compiled one, of the target and of those above it.
bool wouldPerformFastPut(VM&, JSObject* target);
JS_EXPORT_PRIVATE void didBecomePrototype(VM&, JSObject*);
JS_EXPORT_PRIVATE void willPutDirectOnClosure(VM&, JSObject* closure, UniquedStringImpl*);
JS_EXPORT_PRIVATE void didPutDirectOnClosure(VM&, JSObject* closure, UniquedStringImpl*);
bool hasIndexedProperties(JSObject*);
JSObject* objectHoldingIndexedProperties(const JSObject*);

} } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
