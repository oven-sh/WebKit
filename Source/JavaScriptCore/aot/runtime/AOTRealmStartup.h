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

enum class RejectedWrite : uint8_t;
struct Instance;

bool runsCompiledProgram(JSGlobalObject*);
bool locksBuiltins(JSGlobalObject*);
JS_EXPORT_PRIVATE void lockBuiltinsWithoutImage();
JS_EXPORT_PRIVATE void lockBuiltinsOfTable(JSGlobalObject*);
using RefusedChangeListener = void (*)(JSGlobalObject*, const String& refusal);
JS_EXPORT_PRIVATE void setRefusedChangeListener(RefusedChangeListener);
void didRejectWrite(JSGlobalObject*, JSObject*, UniquedStringImpl* keyOrNull, RejectedWrite);
String lockedObjectErrorMessage(JSGlobalObject*, JSObject*, UniquedStringImpl* key, RejectedWrite);
// The words of the store that a spy would make, said of the spy. Null: the object says nothing of that key.
JS_EXPORT_PRIVATE String refusedSpyMessage(JSGlobalObject*, JSObject*, UniquedStringImpl* key);
// In a test run a refusal is an error for everybody: a test that takes it for a replacement that was made goes on to measure nothing.
bool reflectThrowsOnLockedProperty(JSObject* lockedObject);
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
// For the constructor of a class whose Structure was reported so. An instance leads to it, and nobody need have read the name of the class by then.
JS_EXPORT_PRIVATE void hostDidCreateConstructor(JSGlobalObject*, JSObject* constructor);
// No name of the realm leads to the object of node:module, and the table locks the hooks of the loader on it.
JS_EXPORT_PRIVATE void hostDidCreateNodeModule(JSGlobalObject*, JSObject* constructor);
// For an object that the global object names only from the first read of the name on. It is locked from its creation: there are other ways to it.
JS_EXPORT_PRIVATE void hostDidCreateObjectOfGlobal(JSGlobalObject*, UniquedStringImpl* key, JSObject*);
// process.versions.node: members of an ordinary object that the host fills. The table ends above them, and the build folds their reads. `holder` is how a program writes the object.
JS_EXPORT_PRIVATE void hostDidFillObjectWithFoldedMembers(JSGlobalObject*, JSObject*, ASCIILiteral holder, std::span<const ASCIILiteral> names);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
