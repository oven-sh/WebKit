/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "JSCJSValue.h"
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;
class VM;

namespace AOT {

struct LayoutData;
struct LayoutSetData;
struct ProgramTables;
struct RuntimeType;
struct State;

enum class ValueDetail : uint8_t {
    KindOnly,
    WithContents,
};

JS_EXPORT_PRIVATE String describe(JSValue, ValueDetail = ValueDetail::WithContents);
String describe(const ProgramTables&, const RuntimeType&);
String describe(const ProgramTables&, const RuntimeType&, Typed::TypeSiteID);
String describeKinds(State&, uint32_t kinds, uint32_t name);
String nameOf(const ProgramTables&, const LayoutData&);
String nameOf(const ProgramTables&, const LayoutSetData&);

String mismatchMessage(State&, VM&, JSValue, const RuntimeType& expected, Typed::TypeSiteID = { });
String explainMismatch(State&, VM&, JSValue, const RuntimeType&);
String nullishMessage(JSValue, const String& property, bool isWrite);
ASCIILiteral nullishIterationMessage(JSValue);
String nullishMessage(JSGlobalObject*, JSValue, JSValue key, bool isWrite);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
