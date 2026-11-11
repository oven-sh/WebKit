/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTInstance.h"
#include "JSArray.h"
#include "JSBigInt.h"
#include "JSCJSValue.h"
#include "MarkedBlock.h"

namespace JSC {

class ArgList;
class JSObject;
class JSString;
class Structure;

namespace AOT {

inline constexpr size_t oneDigitBigIntSize = roundUpToMultipleOf<MarkedBlock::atomSize>(JSBigInt::allocationSize(1));
inline constexpr unsigned maxLengthOfInsertionSortedArray = 64;

JSValue callInEngine(Instance*, BuiltinID function, JSValue first, JSValue second = JSValue(), JSValue third = JSValue());
JSValue callMethodInEngine(Instance*, BuiltinID function, JSValue receiver, JSValue first = JSValue(), JSValue second = JSValue());
JSValue callMethodInEngine(Instance*, BuiltinID function, JSValue receiver, const ArgList&);
JSValue constructInEngine(Instance*, BuiltinID constructor, JSValue argument);

void markPristineIfOriginal(Instance&, JSObject*);

JSString* tryConvertListToString(JSGlobalObject*, JSArray*);

ALWAYS_INLINE bool mayRejectOrInterceptWrites(JSArray* array)
{
    IndexingType type = array->indexingType();
    if (hasAnyArrayStorage(type))
        return hasSlowPutArrayStorage(type) || array->butterfly()->arrayStorage()->m_sparseMap;
    return isCopyOnWrite(array->indexingMode()) && array->hasFrozenElementsInPlace();
}

void prepareFinalObjectAllocation(Instance*, InlineAllocation, Structure*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
