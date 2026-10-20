/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "JSCJSValue.h"
#include <span>

namespace JSC {

class JSFinalObject;
class JSGlobalObject;
class JSObject;
class VM;

namespace AOT {

class Object;

Object* tryCloneAs(VM&, JSObject* source, const LayoutData&, std::span<const EncodedJSValue> replacedFields);
// The keys that a copy leaves out, as a run of identifier numbers in the image. The number of the run stands for the list.
struct ExcludedRun {
    uint32_t numberPlusOne { 0 };
    std::span<const uint32_t> identifiers;
};

// An ordinary object with the enumerable own properties of a compiled object that has nothing but fields, or of an ordinary object that has nothing but data properties under names.
JSFinalObject* tryCopyAsOrdinaryObject(VM&, JSGlobalObject*, JSObject* source, ExcludedRun);
bool isPlainDataObject(JSValue);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
