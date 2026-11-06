/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "PropertySlot.h"
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
struct FieldData;

// A star is what compilers make of `export * from` in a CommonJS module: each enumerable name of the source but "default" that the target lacks becomes an accessor of the target, whose getter reads the name from the source.

// The bits of the argument `rule` of RuntimeOperation::TryReExportFields.
static constexpr uint32_t starRuleKeyCaptureSlot = 1 << 0;
// An accessor of a source that is marked as an ES module is taken over as it is, as TypeScript's `__createBinding` does from version 4.6 on.
static constexpr uint32_t starRuleReusesDescriptors = 1 << 1;
// The getter stores its key as a literal number if the string has one.
static constexpr uint32_t starRuleKeyIsLiteralStringWord = 1 << 3;

// False where the star has to be carried out now. Nothing has been done then. Runs no code of the program.
bool tryAddPendingStar(VM&, Object& target, Object& source, FunctionID getter, uint32_t rule);
bool hasPendingStars(const Object&);
// A read of a name that a pending star binds gets what the getter of the binding would read: the name, from this object. Null for a name that no pending star binds: the object has no other extra property.
// Nothing for an object without pending stars, and for one whose stars have been searched so often that they are better carried out.
std::optional<Object*> sourceOfPendingBinding(VM&, const Object& target, UniquedStringImpl* name);
// What a binding of a star has once it is made.
static constexpr unsigned attributesOfStarBinding = PropertyAttribute::Accessor | PropertyAttribute::DontDelete;
// What the getters of the pending bindings of an object would answer now, kept without making any of them: the values of the fields of every source.
JSCellButterfly* snapshotOfPendingStars(VM&, const Object& target);
JSValue valueInSnapshotOfPendingStars(VM&, const Object& target, JSCellButterfly* snapshot, UniquedStringImpl* name);
void forEachBindingInSnapshotOfPendingStars(VM&, const Object& target, JSCellButterfly* snapshot, const ScopedLambda<void(UniquedStringImpl*, JSValue)>&);
// How many keys the pending stars will give the object, for whoever lists a field behind them. Makes none of them. Nothing for an object without pending stars. A key under the name of a field of the object counts like the others: it gets a place of its own in the order when the stars are carried out.
std::optional<uint32_t> numberOfKeysOfPendingStars(VM&, const Object& target);
// The keys that the pending stars will give the object, in their order, each with the attributes that it will have. Makes none of them.
void forEachKeyOfPendingStars(VM&, const Object& target, const ScopedLambda<void(UniquedStringImpl*, unsigned attributes)>&);
// Whether the accessor of a pending star will lie under the name of the field. Nothing for an object without pending stars.
std::optional<bool> isBoundByPendingStar(const Object& target, const FieldData&);
// For whoever must not allocate, as during a walk over the heap. Nothing for an object with pending stars.
JSObject* extraPropertiesIfMade(const Object&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
