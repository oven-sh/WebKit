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
class JSObject;
class PropertyName;
class PropertyRun;
class PutPropertySlot;
class ThrowScope;

namespace AOT {

const PropertyRun* propertyRunAt(Instance*, uint32_t number);

void copyDataProperties(Instance*, JSGlobalObject*, JSObject* target, JSValue source);
JS_EXPORT_PRIVATE JSObject* withAllPropertiesInline(JSGlobalObject*, JSObject*);

void defineDataProperty(Instance*, JSObject*, PropertyName, JSValue, PutPropertySlot&);

void throwNotAFunctionError(JSGlobalObject*, ThrowScope&, JSValue callee);
void throwNotAFunctionError(JSGlobalObject*, ThrowScope&, JSValue callee, const Identifier& name);

JSValue valueOfDecodedReference(const Instance*, uint64_t bits);
void throwTypeMismatch(Instance*, JSGlobalObject*, ThrowScope&, JSValue value, uint32_t type, Typed::TypeSiteID = { });
JSObject* createRuntimeError(JSGlobalObject*, Typed::RuntimeErrorKind, const Identifier* name);
ErrorType errorTypeFor(Typed::ErrorKind);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
