/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
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
class PropertyRun;
class Structure;
template<typename> class RememberedSlot;

namespace AOT {

class CallSiteOverride;

using RuntimeCounterID = Wire::RuntimeCounterID;
inline constexpr unsigned numberOfRuntimeCounters = numberOfEnumerators<RuntimeCounterID>;

struct ExceptionWords {
    EncodedJSValue thrownValue;
    const CallSiteOverride* callSiteOverrides;
    // One more than the identifier number of the method that FindMethodById read, from there to the call of what it read, if that is not a cell. Zero otherwise.
    uint32_t nameOfUncallableMethod;
};

struct CounterWords {
    static constexpr ptrdiff_t offsetOfValue(RuntimeCounterID counter) { return static_cast<ptrdiff_t>(static_cast<size_t>(counter) * sizeof(uint64_t)); }

    uint64_t values[numberOfRuntimeCounters];
    uint64_t* branchCounts;
};
static_assert(!OBJECT_OFFSETOF(CounterWords, values));

struct ConstantCellWords {
    RememberedSlot<JSCell*>* wideStringCells;
    JSCell** literalStrings;
    uint64_t* literalStringCache;
    JSCell** keyCells;
    JSCell** bigIntCells;
};

struct ObjectLiteralWords {
    const PropertyRun** propertyRuns;
};

struct RuntimeWords {
    ObjectLiteralWords literals;
    ExceptionWords exceptions;
    CounterWords counters;
    ConstantCellWords constants;
};
static_assert(std::is_trivially_default_constructible_v<RuntimeWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
