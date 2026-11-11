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

static constexpr uint32_t starRuleKeyCaptureSlot = 1 << 0;
static constexpr uint32_t starRuleReusesDescriptors = 1 << 1;

bool tryAddPendingStar(VM&, Object& target, Object& source, FunctionID getter, uint32_t rule);
bool hasPendingStars(const Object&);
std::optional<Object*> sourceOfPendingBinding(VM&, const Object& target, UniquedStringImpl* name);
static constexpr unsigned attributesOfStarBinding = PropertyAttribute::Accessor | PropertyAttribute::DontDelete;
JSCellButterfly* snapshotOfPendingStars(VM&, const Object& target);
JSValue valueInSnapshotOfPendingStars(VM&, const Object& target, JSCellButterfly* snapshot, UniquedStringImpl* name);
void forEachBindingInSnapshotOfPendingStars(VM&, const Object& target, JSCellButterfly* snapshot, const ScopedLambda<void(UniquedStringImpl*, JSValue)>&);
std::optional<uint32_t> numberOfKeysOfPendingStars(VM&, const Object& target);
std::optional<bool> isBoundByPendingStar(const Object& target, const FieldData&);
JSObject* extraPropertiesIfCreated(const Object&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
