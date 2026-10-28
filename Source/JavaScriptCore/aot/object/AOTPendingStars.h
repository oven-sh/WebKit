/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include <optional>
#include <wtf/ScopedLambda.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSCellButterfly;
class JSObject;
class JSValue;
class VM;

namespace AOT {

class Object;

// A star is what compilers make of `export * from` in a CommonJS module: each enumerable name of the source but "default" that the target lacks becomes an accessor of the target, whose getter reads the name from the source.

// The bits of the argument `rule` of RuntimeOperation::TryReExportFields.
static constexpr uint32_t starRuleKeyCaptureSlot = 1 << 0;
// An accessor of a source that is marked as an ES module is taken over as it is, as TypeScript's `__createBinding` does from version 4.6 on.
static constexpr uint32_t starRuleReusesDescriptors = 1 << 1;
// The getter holds its key as the number of a literal where the text has one.
static constexpr uint32_t starRuleKeyIsLiteralStringWord = 1 << 3;

// False where the star has to be carried out now. Nothing has been done then. Runs no code of the program.
bool tryAddPendingStar(VM&, Object& target, Object& source, FunctionID getter, uint32_t rule);
bool hasPendingStars(const Object&);
// A read of a name that a pending star binds gets what the getter of the binding would read: the name, from this object.
std::optional<Object*> sourceOfPendingBinding(VM&, const Object& target, UniquedStringImpl* name);
// What the getters of the pending bindings of an object would answer now, kept without making any of them: the values of the fields of every source.
JSCellButterfly* snapshotOfPendingStars(VM&, const Object& target);
JSValue valueInSnapshotOfPendingStars(VM&, const Object& target, JSCellButterfly* snapshot, UniquedStringImpl* name);
void forEachBindingInSnapshotOfPendingStars(VM&, const Object& target, JSCellButterfly* snapshot, const ScopedLambda<void(UniquedStringImpl*, JSValue)>&);
// For whoever must not allocate, as during a walk over the heap. Nothing for an object with pending stars.
JSObject* extraPropertiesIfMade(const Object&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
