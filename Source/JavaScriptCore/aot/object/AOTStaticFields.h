/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSCell;
class VM;

namespace AOT {

class Closure;

namespace StaticFields {

bool ownsVariables(const Closure& classObject);
const FieldData* findOwnField(const Closure& classObject, UniquedStringImpl*);
bool isInVariable(const Closure& classObject, const FieldData&);
bool canEnterVariable(VM&, const Closure& classObject, const FieldData&);
void ensureProperties(VM&, Closure& classObject);
void moveToProperty(VM&, Closure& classObject, const FieldData&);
void copyToProperties(VM&, Closure& classObject);
void didDeleteProperty(VM&, Closure& classObject, const FieldData&);
const FieldData* findFieldForCache(VM&, JSCell* classObject, UniquedStringImpl*);

} // namespace StaticFields

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
