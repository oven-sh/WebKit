/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTRuntimeWords.h"
#include "CodeType.h"
#include "JSExportMacros.h"
#include <array>
#include <wtf/Assertions.h>
#include <wtf/Forward.h>

namespace JSC {

class VM;

namespace AOT {

struct Instance;

struct RuntimeCounters {
    void add(const CounterWords&);

    std::array<uint64_t, numberOfRuntimeCounters> values { };
    std::array<uint64_t, numberOfLimits> limitHits { };
};

JS_EXPORT_PRIVATE void count(VM&, RuntimeCounterID);
void count(Instance&, RuntimeCounterID);
JS_EXPORT_PRIVATE void countLimitHit(VM&, LimitID);

JS_EXPORT_PRIVATE uint64_t valueOf(VM&, RuntimeCounterID);
JS_EXPORT_PRIVATE uint64_t limitHitsOf(VM&, LimitID);
JS_EXPORT_PRIVATE uint64_t runtimeLimitValue(VM&, LimitID);

constexpr RuntimeCounterID counterFor(CodeType codeType)
{
    switch (codeType) {
    case GlobalCode:
        return RuntimeCounterID::InterpretedGlobalCode;
    case EvalCode:
        return RuntimeCounterID::InterpretedEvalCode;
    case FunctionCode:
        return RuntimeCounterID::InterpretedFunctionCode;
    case ModuleCode:
        return RuntimeCounterID::InterpretedModuleCode;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}
static_assert(counterFor(EvalCode) == RuntimeCounterID::InterpretedEvalCode);

constexpr bool countsStubExit(RuntimeCounterID counter)
{
    switch (counter) {
    case RuntimeCounterID::StubCannotReadField:
    case RuntimeCounterID::StubCannotStoreField:
    case RuntimeCounterID::StubCannotCreateField:
    case RuntimeCounterID::StubCannotCallGetter:
    case RuntimeCounterID::StubCannotCallSetter:
    case RuntimeCounterID::StubCannotGrowStorage:
        return true;
    default:
        return false;
    }
}
static_assert(countsStubExit(RuntimeCounterID::StubCannotCallSetter) && !countsStubExit(RuntimeCounterID::ChainDependentsDropped) && !countsStubExit(RuntimeCounterID::TypedParsesFellBackPerValue));

JS_EXPORT_PRIVATE void dumpCounts(VM&, PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
