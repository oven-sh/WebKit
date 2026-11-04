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

// The methods of lists whose bodies are functions of the library, which are compiled with the program. In the order of the forms of StubFamily::CallCompiledMethodOfArray.
enum class CompiledMethodOfArray : uint8_t { At, Every, Filter, Find, FindIndex, FindLast, FindLastIndex, FlatMap, ForEach, Map, Reduce, ReduceRight, Some };
constexpr unsigned numberOfCompiledMethodsOfArray = static_cast<unsigned>(CompiledMethodOfArray::Some) + 1;
static_assert(numberOfCompiledMethodsOfArray == StubWords::numberOfCompiledMethodsOfArray);
static_assert(descriptorOf(StubFamily::CallCompiledMethodOfArray).numberOfForms == numberOfCompiledMethodsOfArray);
constexpr unsigned maxArgumentCountOfCompiledMethodOfArray = descriptorOf(StubFamily::CallCompiledMethodOfArray).maxCount;

// `builtin`: canonical.
JS_EXPORT_PRIVATE std::optional<CompiledMethodOfArray> compiledMethodOfArray(BuiltinID builtin);

// `list.some(test)` as one call: takes what CallMethodById takes, the name too. A list of this realm that is as its class makes it finds the built-in under the name, whose body is compiled:
// the stub enters that code as a call by value of the built-in's function would, with nothing in the register of the callee, which such a function does not read.
// Every other receiver goes on to CallMethodById, and so does every receiver where the image holds no code for the function: StubWords::wordsOfCompiledMethodsOfArray.
constexpr StubKey callCompiledMethodOfArray(CompiledMethodOfArray method, unsigned count) { return StubKey(StubFamily::CallCompiledMethodOfArray).withForm(static_cast<uint8_t>(method)).withCount(count); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
