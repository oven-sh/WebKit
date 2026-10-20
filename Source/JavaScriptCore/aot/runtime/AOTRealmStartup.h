/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSValue;
class LazyClassStructure;
class Structure;
class VM;

namespace AOT {

struct Instance;

bool runsCompiledProgram(JSGlobalObject*);
JS_EXPORT_PRIVATE ASCIILiteral evaluationRefusedMessage();
void refuseEvaluationUnlessAdmitted(JSGlobalObject*);
Instance* reserveInstance(JSGlobalObject*);
void makeBuiltinsImmutable(Instance&);
void markOriginalStructuresPristine(VM&, JSGlobalObject*);
void didInstallBuiltins(Instance&);
JS_EXPORT_PRIVATE void hostDidCreateRealm(JSGlobalObject*);
// Start-up locks the built-in objects that are there. The realm creates most of them at their first use, and whoever creates one reports it here before the program can reach it.
// A class is reported with its Structure, and once more with its constructor if it has one.
void didCreateClass(JSGlobalObject*, const LazyClassStructure&, Structure*, JSObject* prototype, JSObject* constructorOrNull);
void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue);
// For a Structure of instances that a host does not keep in a LazyClassStructure.
JS_EXPORT_PRIVATE void hostDidCreateStructure(JSGlobalObject*, Structure*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
