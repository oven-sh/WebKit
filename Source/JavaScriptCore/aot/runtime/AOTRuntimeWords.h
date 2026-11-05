/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeRecords.h"
#include "JSCJSValue.h"
#include "PropertyOffset.h"
#include "StructureID.h"
#include <array>
#include <bit>
#include <tuple>
#include <type_traits>
#include <wtf/StdLibExtras.h>

namespace JSC {

class JSCell;
class Structure;
template<typename> class RememberedSlot;

namespace AOT {

struct ExceptionWords {
    EncodedJSValue thrownValue;
    void* stackPointerOfHandler;
    uint32_t pollLeavesOwedError;
};
static_assert(sizeof(ExceptionWords) == 24);

struct GlobalBinding {
    EncodedJSValue constant;
    uint32_t structureID;
    uint32_t lexicalBindingEpoch;
    int64_t location;
};
static_assert(sizeof(GlobalBinding) == 24);
static_assert(!OBJECT_OFFSETOF(GlobalBinding, constant) && OBJECT_OFFSETOF(GlobalBinding, structureID) == 8 && OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch) == 12 && OBJECT_OFFSETOF(GlobalBinding, location) == 16);

struct BindingWords {
    GlobalBinding* globalBindings;
};

struct ConstantCellWords {
    void* stringAllocator;
    uint8_t* cardsOfStringCells;
    uint8_t* nextStringHeader;
    uint8_t* endOfStringHeaders;
    RememberedSlot<JSCell*>* wideStringCells;
    uint32_t* keyCells;
    JSCell** wideKeyCells;
    static constexpr uint32_t numberOfCachedFunctionNames = 256;
    uint64_t* functionNameCache;
    JSCell** bigIntCells;
};

struct EffectsNetWords {
    uint64_t entriesIntoCode;
    uint8_t* exercised;
    uint64_t callHeldToThrowNothing;
    uint64_t rowsOfCallHeldToThrowNothing;
};

struct RuntimeWords {
    ExceptionWords exceptions;
    BindingWords bindings;
    ConstantCellWords constants;
};
static_assert(std::is_trivially_default_constructible_v<RuntimeWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
