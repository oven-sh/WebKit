/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSValue;
class LazyClassStructure;
class Structure;
class VM;

namespace AOT {

enum class RefusedChange : uint8_t;
struct Instance;

bool runsCompiledProgram(JSGlobalObject*);
bool locksBuiltins(JSGlobalObject*);
JS_EXPORT_PRIVATE void lockBuiltinsWithoutImage();
JS_EXPORT_PRIVATE void lockBuiltinsOfTable(JSGlobalObject*);
using RefusedChangeListener = void (*)(JSGlobalObject*, const String& refusal);
JS_EXPORT_PRIVATE void setRefusedChangeListener(RefusedChangeListener);
void didRefuseChange(JSGlobalObject*, JSObject*, UniquedStringImpl* keyOrNull, RefusedChange);
String messageOfRefusalByLockedObject(JSGlobalObject*, JSObject*, UniquedStringImpl* key, RefusedChange);
JS_EXPORT_PRIVATE ASCIILiteral evaluationRefusedMessage();
void refuseEvaluationUnlessAdmitted(JSGlobalObject*);
Instance* reserveInstance(JSGlobalObject*);
void makeBuiltinsImmutable(Instance&);
void markOriginalStructuresPristine(VM&, JSGlobalObject*);
void didInstallBuiltins(Instance&);
JS_EXPORT_PRIVATE void hostDidCreateRealm(JSGlobalObject*);
// Start-up locks the built-in objects that are there.
void didCreateClass(JSGlobalObject*, const LazyClassStructure&, Structure*, JSObject* prototype, JSObject* constructorOrNull);
void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue);
// The holder is about to create the member. Compiled code may hold the object already: then the holder adds that one. Null otherwise.
JSObject* takeStaticPropertyMadeAhead(JSObject* holder, UniquedStringImpl* key);
// Whether the holder has not created the member yet and didCreateStaticProperty() will be told when it does.
bool reportsCreationOfMember(JSObject* holder, UniquedStringImpl* key);
// For a Structure of instances that a host does not keep in a LazyClassStructure.
JS_EXPORT_PRIVATE void hostDidCreateStructure(JSGlobalObject*, Structure*);
// No name of the realm leads to the object of node:module, and the table locks the hooks of the loader on it.
JS_EXPORT_PRIVATE void hostDidCreateNodeModule(JSGlobalObject*, JSObject* constructor);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
