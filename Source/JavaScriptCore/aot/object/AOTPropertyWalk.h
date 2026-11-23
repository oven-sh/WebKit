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
#include <span>
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

class Object;
struct CachedPropertyList;

bool definePropertiesInOrder(VM&, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values);
bool storePropertiesInOrder(VM&, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values);

} // namespace AOT

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
enum class Elements : bool { NeedGenericPath, Skip };
enum class Preparations : bool { NeedGenericPath, AreDone };
enum class IdenticalValues : bool { Skip, Append };

ALWAYS_INLINE bool isCompiledObject(const JSCell* cell) { return cell->type() == CompiledObjectType; }
JS_EXPORT_PRIVATE bool wouldHaveArrayStorage(JSObject* compiledObject);
JS_EXPORT_PRIVATE bool convertsToOrdinaryObject(const ClassInfo* from, const ClassInfo* to);
JS_EXPORT_PRIVATE Result forEachOwnProperty(VM&, JSObject*, Filter, const ScopedLambda<IterationStatus(const OwnProperty&)>&, Elements = Elements::NeedGenericPath);
Result forEachOwnDescriptor(VM&, JSObject*, const ScopedLambda<IterationStatus(const OwnProperty&)>&);
JS_EXPORT_PRIVATE Result forEachOwnPropertyAsCreated(VM&, JSObject*, const ScopedLambda<IterationStatus(const OwnProperty&)>&, Elements = Elements::NeedGenericPath, Preparations = Preparations::NeedGenericPath);
JS_EXPORT_PRIVATE Result forEachOwnPropertyAsCreatedAfterPreparing(JSGlobalObject*, JSObject*, const ScopedLambda<IterationStatus(const OwnProperty&)>&, Elements = Elements::NeedGenericPath);
struct GetterWalkHooks {
    const ScopedLambda<void()>* willRunCode { nullptr };
    const ScopedLambda<bool()>* mayReadAhead { nullptr };
    const ScopedLambda<bool(UniquedStringImpl*)>* isLeftOut { nullptr };
    bool refusesProtoName { false };
};
bool hasGetterToCall(JSObject* compiledObject);
enum class NewObjectTakes : bool { Definitions, Stores };
bool tryListCallingGetters(JSGlobalObject*, JSObject* compiledObject, MarkedArgumentBuffer* keys, MarkedArgumentBuffer& values);
JSObject* tryCopyWithGetters(JSGlobalObject*, JSObject* compiledObject, uint32_t excludedRunNumberPlusOne = 0, std::span<UniquedStringImpl* const> excludedNames = { });
JSObject* tryCopyCallingGetters(JSGlobalObject*, JSObject* compiledObject, NewObjectTakes, const ScopedLambda<bool(UniquedStringImpl*)>* isLeftOut = nullptr, JSObject* unseenTarget = nullptr);
Result readOwnPropertiesCallingGetters(JSGlobalObject*, JSObject*, Vector<RefPtr<UniquedStringImpl>, 32>& heldNames, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values, GetterWalkHooks = { });
JS_EXPORT_PRIVATE void materializeLazyProperties(VM&, JSObject*);
JS_EXPORT_PRIVATE void prepareHoldersBeforeLookupsUnder(JSGlobalObject*, Symbol*, JSObject* function);
JS_EXPORT_PRIVATE bool hasOwnEnumerableProperty(JSGlobalObject*, JSObject* compiledObject);
JSCellButterfly* tryGetCachedOwnKeys(VM&, JSObject*, Filter);
bool keepsSymbolsInExtraProperties(JSObject*, JSObject*& extraProperties);
Presence hasOwnProperty(JSObject*, UniquedStringImpl*);
const CachedPropertyList* tryGetOwnPropertyList(VM&, JSObject* compiledObject, JSObject*& extraProperties);
Presence hasPropertyOnChain(Structure*, UniquedStringImpl*);
bool ensureExtraPropertiesStructure(VM&, JSObject*);
JS_EXPORT_PRIVATE bool structureGuaranteesAbsence(Structure*, UniquedStringImpl*);
JS_EXPORT_PRIVATE bool appendDifferingValues(JSObject*, JSObject*, IdenticalValues, MarkedArgumentBuffer&);
JS_EXPORT_PRIVATE JSValue getDirect(VM&, const JSObject*, PropertyName, unsigned* attributesOrNull);
JS_EXPORT_PRIVATE bool ordinaryGetOwnPropertySlot(JSGlobalObject*, JSObject*, PropertyName, PropertySlot&);
JS_EXPORT_PRIVATE bool isOrdinaryPrototypeChain(JSObject* first, JSObject* last);
JS_EXPORT_PRIVATE UniquedStringImpl* tryFindName(VM&, std::span<const Latin1Character>);
JS_EXPORT_PRIVATE Presence tryGetOwnValue(JSObject* compiledObject, UniquedStringImpl*, JSValue&);
using DescriptorMembers = std::array<JSValue, 6>;
JS_EXPORT_PRIVATE bool tryReadDescriptorMembers(JSObject* compiledObject, DescriptorMembers&);
JS_EXPORT_PRIVATE void putDirect(VM&, JSObject*, PropertyName, JSValue, unsigned attributes);
JS_EXPORT_PRIVATE unsigned countPrivateFieldsOfCompiledClasses(JSGlobalObject*, JSObject*);
JS_EXPORT_PRIVATE bool hasOwnPrivateField(JSGlobalObject*, JSObject*);
JS_EXPORT_PRIVATE bool isRuntimeProperty(VM&, UniquedStringImpl*, JSValue);
JS_EXPORT_PRIVATE bool matchPrivateFieldsOfCompiledClasses(JSGlobalObject*, JSObject* object, JSObject* pattern, const ScopedLambda<bool(JSValue ofObject, JSValue ofPattern)>& matches);
void prepareForFreeze(JSObject* compiledObject);
void setIntegrity(VM&, JSObject*, Integrity);
bool tryFreezeFields(VM&, JSObject*);
Presence testIntegrity(JSObject*, Integrity);
bool tryAssign(VM&, JSObject* compiledTarget, JSObject* source);
bool tryAssignCallingGetters(JSGlobalObject*, JSObject* target, JSObject* source);
bool tryAssignThroughCaches(JSGlobalObject*, JSObject* target, JSObject* source);
bool wouldPerformFastPut(VM&, JSObject* target);
JS_EXPORT_PRIVATE void didBecomePrototype(VM&, JSObject*);
JS_EXPORT_PRIVATE void willPutDirectOnClosure(VM&, JSObject* closure, UniquedStringImpl*);
JS_EXPORT_PRIVATE void didPutDirectOnClosure(VM&, JSObject* closure, UniquedStringImpl*);
bool hasIndexedProperties(JSObject*);
JSObject* objectHoldingIndexedProperties(const JSObject*);

} } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
