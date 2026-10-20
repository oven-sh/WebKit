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
JS_EXPORT_PRIVATE bool convertsToOrdinaryObject(const ClassInfo* from, const ClassInfo* to);
JS_EXPORT_PRIVATE Result forEachOwnProperty(VM&, JSObject*, Filter, const ScopedLambda<IterationStatus(const OwnProperty&)>&);
JS_EXPORT_PRIVATE void materializeLazyProperties(VM&, JSObject*);
JSCellButterfly* tryGetCachedOwnKeys(VM&, JSObject*, Filter);
Presence hasOwnProperty(JSObject*, UniquedStringImpl*);
Presence hasPropertyOnChain(Structure*, UniquedStringImpl*);
JSValue getDirect(VM&, const JSObject*, PropertyName, unsigned* attributesOrNull);
void putDirect(VM&, JSObject*, PropertyName, JSValue, unsigned attributes);
JS_EXPORT_PRIVATE void convertToOrdinaryObject(VM&, JSObject*);
void setIntegrity(VM&, JSObject*, Integrity);
Presence testIntegrity(JSObject*, Integrity);
bool tryAssign(VM&, JSObject* compiledTarget, JSObject* source);
JS_EXPORT_PRIVATE void didBecomePrototype(VM&, JSObject*);
bool hasIndexedProperties(JSObject*);
JSObject* objectHoldingIndexedProperties(const JSObject*);

} } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
