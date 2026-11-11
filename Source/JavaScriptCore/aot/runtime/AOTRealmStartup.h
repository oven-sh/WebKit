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
JS_EXPORT_PRIVATE void unlockNameWithoutImage(uint32_t holder, std::span<const uint8_t> name);
JS_EXPORT_PRIVATE void lockTableBuiltins(JSGlobalObject*);
using RejectedChangeListener = void (*)(JSGlobalObject*, const String& refusal);
JS_EXPORT_PRIVATE void setRejectedChangeListener(RejectedChangeListener);
void notifyRejectedChangeListener(JSGlobalObject*, const String& refusal);
void didRejectWrite(JSGlobalObject*, JSObject*, UniquedStringImpl* keyOrNull, RejectedWrite);
String lockedObjectErrorMessage(JSGlobalObject*, JSObject*, UniquedStringImpl* key, RejectedWrite);
JS_EXPORT_PRIVATE String rejectedSpyMessage(JSGlobalObject*, JSObject*, UniquedStringImpl* key);
JS_EXPORT_PRIVATE bool rejectsOwnPropertyOnLibraryFunction(JSObject*, UniquedStringImpl* key);
JS_EXPORT_PRIVATE bool isMemberUnlockedForTests(JSObject*, UniquedStringImpl* key);
JS_EXPORT_PRIVATE bool isMemberFoldedAsConstant(JSObject*, UniquedStringImpl* key);
bool reflectThrowsOnLockedProperty(JSObject* lockedObject);
bool isReportedAsReadOnly(JSObject* lockedObject, UniquedStringImpl* key);
JS_EXPORT_PRIVATE ASCIILiteral rejectedEvaluationMessage();
void rejectEvaluationUnlessAllowed(JSGlobalObject*);
Instance* reserveInstance(JSGlobalObject*);
void makeBuiltinsImmutable(Instance&);
void markOriginalStructuresPristine(VM&, JSGlobalObject*);
void didInstallBuiltins(Instance&);
JS_EXPORT_PRIVATE void hostDidCreateRealm(JSGlobalObject*);
void didCreateClass(JSGlobalObject*, const LazyClassStructure&, Structure*, JSObject* prototype, JSObject* constructorOrNull);
void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue);
JSObject* takePreallocatedStaticProperty(JSObject* holder, UniquedStringImpl* key);
bool reportsCreationOfMember(JSObject* holder, UniquedStringImpl* key);
JS_EXPORT_PRIVATE void hostDidCreateStructure(JSGlobalObject*, Structure*);
using BufferStructureProvider = Structure* (*)(JSGlobalObject*);
JS_EXPORT_PRIVATE void setBufferStructureProvider(BufferStructureProvider);
JS_EXPORT_PRIVATE void hostDidCreateConstructor(JSGlobalObject*, JSObject* constructor);
JS_EXPORT_PRIVATE void hostDidCreateNodeModule(JSGlobalObject*, JSObject* constructor);
JS_EXPORT_PRIVATE void acceptForeignBinaryData(JSGlobalObject*);
JS_EXPORT_PRIVATE void hostDidCreateGlobalMember(JSGlobalObject*, UniquedStringImpl* key, JSObject*);
JS_EXPORT_PRIVATE void hostDidFillObjectWithFoldedMembers(JSGlobalObject*, JSObject*, ASCIILiteral holder, std::span<const ASCIILiteral> names);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
