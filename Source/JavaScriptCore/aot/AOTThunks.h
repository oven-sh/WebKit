/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"

namespace JSC {

class CCallHelpers;
class VM;

namespace AOT {

void installOperationFrontEnds(VM&, void** runtimeTableEntries);

template<typename Functor>
void forEachOperationFrontEnd(const Functor& functor)
{
#define AOT_FRONT_END(name) functor(Entry::operationAOT##name, Entry::Raw##name, Stub::FrontEnd##name);
#define AOT_HELPER(name, operation) functor(Entry::operation, Entry::name##SlowPath, Stub::name##WithFastPath);
    AOT_FRONT_END(GetById)
    AOT_FRONT_END(GetByVal)
    AOT_FRONT_END(PutById)
    AOT_FRONT_END(PutByVal)
    AOT_FRONT_END(NewObjectLiteral)
    FOR_EACH_AOT_OPERATION_WITH_HELPER(AOT_HELPER)
    AOT_FRONT_END(CompareStrictEq)
    AOT_FRONT_END(CompareEq)
    AOT_FRONT_END(TypedPutExtra)
    AOT_FRONT_END(TypedGetExtra)
    AOT_FRONT_END(TypedGetByNameChecked)
    AOT_FRONT_END(TypedGetByNameCheckedAsNumber)
    AOT_FRONT_END(TypedHasOwnPropertyByName)
    AOT_FRONT_END(TypedToString)
    AOT_FRONT_END(TypedBitNot)
    AOT_FRONT_END(TypedConstruct)
    AOT_FRONT_END(TypedCloneObject)
    AOT_FRONT_END(TypedCloneObjectAndExtend)
    AOT_FRONT_END(TypedCloneObjectAndExtendWithNumber)
    AOT_FRONT_END(TypedCloneObjectWithSubtypeCheck)
    AOT_FRONT_END(TypedCloneObjectAs)
    AOT_FRONT_END(TypedCoerce)
    AOT_FRONT_END(TypedConstructSuper)
#undef AOT_HELPER
#undef AOT_FRONT_END
}

#if CPU(ARM64) || CPU(X86_64)
#define AOT_DECLARE_FRONT_END(name) void generateFrontEnd##name(CCallHelpers&);
FOR_EACH_AOT_OPERATION_WITH_FRONT_END(AOT_DECLARE_FRONT_END)
#undef AOT_DECLARE_FRONT_END
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
