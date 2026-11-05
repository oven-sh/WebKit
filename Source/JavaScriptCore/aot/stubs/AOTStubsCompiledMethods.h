/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTStubDescriptor.h"
#include "AOTStubWords.h"
#include <optional>

namespace JSC { namespace AOT {

enum class CompiledMethodOfArray : uint8_t { At, Every, Filter, Find, FindIndex, FindLast, FindLastIndex, FlatMap, ForEach, Map, Reduce, ReduceRight, Some };
constexpr unsigned numberOfCompiledMethodsOfArray = static_cast<unsigned>(CompiledMethodOfArray::Some) + 1;
static_assert(numberOfCompiledMethodsOfArray == StubWords::numberOfCompiledMethodsOfArray);
static_assert(descriptorOf(StubFamily::CallCompiledMethodOfArray).numberOfForms == numberOfCompiledMethodsOfArray);
constexpr unsigned maxArgumentCountOfCompiledMethodOfArray = descriptorOf(StubFamily::CallCompiledMethodOfArray).maxCount;
static_assert(descriptorOf(StubFamily::CallCompiledMethodOfArrayLending).numberOfForms == numberOfCompiledMethodsOfArray && descriptorOf(StubFamily::CallCompiledMethodOfArrayLending).maxCount == maxArgumentCountOfCompiledMethodOfArray);
constexpr unsigned onlyFirstArgumentIsLent = 1;

JS_EXPORT_PRIVATE std::optional<CompiledMethodOfArray> compiledMethodOfArray(BuiltinID builtin);

constexpr StubKey callCompiledMethodOfArray(CompiledMethodOfArray method, unsigned count, unsigned lentArguments = 0)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(!lentArguments || lentArguments == onlyFirstArgumentIsLent);
    return StubKey(lentArguments ? StubFamily::CallCompiledMethodOfArrayLending : StubFamily::CallCompiledMethodOfArray).withForm(static_cast<uint8_t>(method)).withCount(count);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
