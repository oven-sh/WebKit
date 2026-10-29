/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationData.h"
#include "AOTRuntimeRecords.h"
#include "ArgList.h"
#include "ErrorType.h"
#include <span>

namespace JSC {

class Identifier;
class JSArray;
class JSGlobalObject;
class JSMap;
class JSObject;
class JSPropertyNameEnumerator;
class JSSet;
class JSString;
class PropertyName;
class PropertyRun;
class PutPropertySlot;
class ThrowScope;

namespace AOT {

const PropertyRun* propertyRunAt(Instance*, uint32_t number);

JS_EXPORT_PRIVATE JSObject* withAllPropertiesInline(JSGlobalObject*, JSObject*);

void defineDataProperty(Instance*, JSObject*, PropertyName, JSValue, PutPropertySlot&);
// False: nothing was done.
bool tryDefineElementOfCompiledObject(Instance*, JSObject*, uint32_t index, JSValue);
// An accessor that is found gets the receiver for `this`.
JSValue getWithoutCaches(Instance*, JSValue base, const Identifier&, JSValue receiver);
inline JSValue getWithoutCaches(Instance* instance, JSValue base, const Identifier& identifier) { return getWithoutCaches(instance, base, identifier, base); }

void throwNotAFunctionError(JSGlobalObject*, ThrowScope&, JSValue callee);

unsigned pushToFullVector(VM&, JSArray*, JSValue);

JSValue valueOfDecodedReference(const Instance*, uint64_t bits);
JSObject* createRuntimeError(JSGlobalObject*, Typed::RuntimeErrorKind, const Identifier* name);
ErrorType errorTypeFor(Typed::ErrorKind);

JSValue constantOfDataWord(State&, uint64_t word);
JSObject* tryBuildData(JSGlobalObject*, State&, const DataRequest&);
JSSet* tryBuildSet(JSGlobalObject*, State&, const CollectionDataRequest&);
JSMap* tryBuildMap(JSGlobalObject*, State&, const CollectionDataRequest&);

struct EnumeratedKey {
    JSString* key { nullptr };
    uint64_t position { 0 };
};

JSPropertyNameEnumerator* enumeratorFor(JSGlobalObject*, JSValue object);
EnumeratedKey findEnumeratedKey(JSGlobalObject*, JSPropertyNameEnumerator*, JSValue object, uint64_t position);

void initializeIterationWords(Instance&);
JS_EXPORT_PRIVATE size_t numberOfElementsSpreadFrom(JSGlobalObject*, JSString*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
