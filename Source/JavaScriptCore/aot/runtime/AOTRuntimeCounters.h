/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTRuntimeWords.h"
#include "CodeType.h"
#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include <array>
#include <memory>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace AOT {

enum class Entry : uint16_t;
class Image;
struct Instance;

struct RuntimeCounters {
    void add(const CounterWords&);

    std::array<uint64_t, numberOfRuntimeCounters> values { };
    std::array<uint64_t, numberOfLimits> limitHits { };
    // No stub and no compiled code counts it, so no instance has a word for it.
    uint64_t stackListingsAfterGivenUpCaptures { 0 };
    uint64_t ropesFlattenedForPrefixTests { 0 };
    uint64_t nativeTextsHashed { 0 };

    // How often the places of a file ran, in a program that counts them, by the compiler's numbers.
    using CountedFile = Vector<uint64_t>;
    // By what the compiler says of the file. A file that runs again, in another realm or from another image, goes on counting where it was.
    UncheckedKeyHashMap<String, std::unique_ptr<CountedFile>> countedFiles;
    // By Image::position(), then by the number of a module of that image. Null until the module counts for the first time.
    Vector<Vector<CountedFile*>, 1> countedFilesOfImages;
};

JS_EXPORT_PRIVATE void count(VM&, RuntimeCounterID);
void count(Instance&, RuntimeCounterID);
JS_EXPORT_PRIVATE void countLimitHit(VM&, LimitID);
// tryCaptureStackTrace() has walked the stack and keeps nothing of it: its caller lists the frames one by one.
void countStackListingAfterGivenUpCapture(VM&);
void countRopeFlattenedForPrefixTest(VM&);
void countNativeTextHashed(VM&);

JS_EXPORT_PRIVATE uint64_t valueOf(VM&, RuntimeCounterID);
JS_EXPORT_PRIVATE uint64_t limitHitsOf(VM&, LimitID);
JS_EXPORT_PRIVATE uint64_t runtimeLimitValue(VM&, LimitID);
uint64_t runtimeLimitValue(const Image*, LimitID);

constexpr RuntimeCounterID counterFor(CodeType codeType)
{
    using enum RuntimeCounterID;
    return std::array { InterpretedGlobalCode, InterpretedEvalCode, InterpretedFunctionCode, InterpretedModuleCode }[codeType];
}
static_assert(counterFor(GlobalCode) == RuntimeCounterID::InterpretedGlobalCode && counterFor(EvalCode) == RuntimeCounterID::InterpretedEvalCode);
static_assert(counterFor(FunctionCode) == RuntimeCounterID::InterpretedFunctionCode && counterFor(ModuleCode) == RuntimeCounterID::InterpretedModuleCode);

constexpr bool countsStubExit(RuntimeCounterID counter)
{
    using enum RuntimeCounterID;
    return counter == StubCannotReadField || counter == StubCannotStoreField || counter == StubCannotCreateField || counter == StubCannotCallGetter || counter == StubCannotCallSetter || counter == StubCannotGrowStorage;
}

JS_EXPORT_PRIVATE void dumpCounts(VM&, PrintStream&);

// What arrived at the accesses that go by name, in an executable built with --aot-count-arrivals. One per instance, created by the first arrival.
class ArrivalCounts {
    WTF_MAKE_TZONE_ALLOCATED(ArrivalCounts);
    WTF_MAKE_NONCOPYABLE(ArrivalCounts);
public:
    // By site and what arrived there. The compiler gives no site the largest number.
    using Counts = UncheckedKeyHashMap<std::pair<uint32_t, String>, uint64_t>;

    JS_EXPORT_PRIVATE ArrivalCounts();
    JS_EXPORT_PRIVATE ~ArrivalCounts();

    // Allocates no cell and runs no JavaScript.
    JS_EXPORT_PRIVATE void record(JSValue receiver, uint32_t site);

    // An access by name whose stub finds its answer calls nothing.
    JS_EXPORT_PRIVATE void didCallOperation(Entry);

    // "<count> TAB arrival:<site> TAB <what arrived>", by site and then with the largest count first, and behind them "<count> TAB left:<site> TAB <operation>" in the same order.
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    Counts m_arrivals;
    Counts m_operationsBehindArrivals;
    uint32_t m_lastSite { 0 };
    bool m_hasCalledNothingSinceArrival { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
