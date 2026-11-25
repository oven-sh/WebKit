/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCallSiteCache.h"
#include "AOTCompiledCaller.h"
#include "AOTCompiledFrame.h"
#include "AOTDriver.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "AOTRealmTestSupport.h"
#include "AOTShellProcess.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTShownFrameFilter.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "ExecutableAllocator.h"
#include "JSCInlines.h"
#include "LLIntPCRanges.h"
#include "SamplingProfiler.h"
#include "StackAlignment.h"
#include "VMEntryRecord.h"
#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/PageBlock.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    v2 = NewClosure function=f1
    PropSet v1, v2 key="outerRead"
    v4 = NewClosure function=f3
    PropSet v1, v4 key="outerHost"
    v6 = NewClosure function=f5
    PropSet v1, v6 key="callsBack"
    v8 = NewClosure function=f6
    PropSet v1, v8 key="toJSON"
    v10 = NewClosure function=f7
    PropSet v1, v10 key="callsClosure"
    v12 = NewClosure function=f8
    PropSet v1, v12 key="tailCaller"
    v14 = NewClosure function=f9
    PropSet v1, v14 key="outerOfInlinedTail"
    v16 = NewClosure function=f11
    PropSet v1, v16 key="outerOfInlinedMethodTail"
    v18 = NewClosure function=f13
    PropSet v1, v18 key="outerOfInlinedDirectTail"
    v20 = NewClosure function=f14
    PropSet v1, v20 key="tailCallerOfBuiltin"
    v22 = NewClosure function=f15
    PropSet v1, v22 key="callerOfBuiltin"
    Return
}
function f1 name="outerRead" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = CallDirect v0 function=f2 @10
    Return v1
}
function f2 name="innerRead" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="missing" @30
    Return v1
}
function f3 name="outerHost" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = CallDirect v0, v1 function=f4 @40
    Return v2
}
function f4 name="innerHost" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0, -, v1 @50
    Return v2
}
function f5 name="callsBack" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Call v0 @60
    Return v1
}
function f6 name="toJSON" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="leaf" [isFreeName]
    v2 = Call v1 @70
    Return v2
}
function f7 name="callsClosure" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Call v0, -, v1, v2 @80
    Return v3
}
function f8 name="tailCaller" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0, -, v1 @90
    ReturnCall v2
}
function f9 name="outerOfInlinedTail" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = CallDirect v0, v1 function=f8 @100
    Return v2
}
function f10 name="tailCallerOfMethod" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = PropGet v0 key="run" @110
    v3 = Call v2, v0, v1 calleeKey="run" @112
    ReturnCall v3
}
function f11 name="outerOfInlinedMethodTail" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = CallDirect v0, v1 function=f10 @120
    Return v2
}
function f12 name="tailCallerOfKnownFunction" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = CallDirect v0 function=f2 @130
    ReturnCall v1
}
function f13 name="outerOfInlinedDirectTail" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = CallDirect v0 function=f12 @140
    Return v1
}
function f14 name="tailCallerOfBuiltin" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=BigInt
    v2 = Call v1, -, v0 @150
    ReturnCall v2
}
function f15 name="callerOfBuiltin" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=BigInt
    v2 = Call v1, -, v0 @160
    Return v2
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.registry = { };
function names(stack) {
    let result = "";
    let text = String(stack);
    for (let start = 0; start <= text.length;) {
        let end = text.indexOf("\n", start);
        if (end < 0)
            end = text.length;
        let line = text.substring(start, end);
        let at = line.indexOf("@");
        let name = at < 0 ? line : line.substring(0, at);
        result += (start ? " < " : "") + name;
        if (name === "driver")
            break;
        start = end + 1;
    }
    return result;
}
function driver(run) {
    try {
        return "returned " + run();
    } catch (error) {
        return error.constructor.name + ": " + names(error.stack);
    }
}
globalThis.leaf = function leaf() { return names(new Error().stack); };
)js"_s;

static String runInShell(ASCIILiteral way, ASCIILiteral referencesWord, ASCIILiteral statements)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(programText, { referencesWord }, way);
    });
    std::span<const uint32_t> entryModules = output->records<uint32_t>(Wire::ResultSection::EntryModuleIds);
    AOT_TEST_CHECK(entryModules.size() == 1);

    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, entryModules[0], ").then(() => {"_s, statements, "}).catch(error => print(\"the script threw \" + error));"_s),
    };
    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkShellOutput(ASCIILiteral statements, ASCIILiteral expectedOutput)
{
    for (ASCIILiteral way : waysToCompilePrograms) {
        for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
            String output = runInShell(way, referencesWord, statements);
            if (output != expectedOutput)
                dataLog("with ", way, " ", referencesWord, " the shell printed\n", output, "expected\n", expectedOutput);
            AOT_TEST_CHECK(output == expectedOutput);
        }
    }
}

static void testTraceTakenInAnOperation()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.outerRead(undefined); }));
    print(driver(function run() { return registry.outerRead(null); }));
    print(driver(function run() { return registry.outerRead({ missing: "found" }); }));
)js"_s, R"(TypeError: innerRead < outerRead < run < driver
TypeError: innerRead < outerRead < run < driver
returned found
)"_s);
}

static void testTraceTakenInAHostFunction()
{
    checkShellOutput(R"js(
    let holder = { };
    print(driver(function run() { return registry.outerHost(Error.captureStackTrace, holder); }));
    print(names(holder.stack));
    print(driver(function run() { return registry.outerHost(BigInt, 1.5); }));
)js"_s, R"(returned undefined
innerHost < outerHost < run < driver
RangeError: BigInt < innerHost < outerHost < run < driver
)"_s);
}

static void testTraceTakenInInterpretedCode()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.callsBack(leaf); }));
    print(driver(function run() { return registry.callsBack(function thrower() { throw new Error("thrown"); }); }));
)js"_s, R"(returned leaf < callsBack < run < driver
Error: thrower < callsBack < run < driver
)"_s);
}

static void testTraceAcrossTwoEntryFrames()
{
    checkShellOutput(R"js(
    print(driver(function run() {
        return registry.callsBack(function viaHost() { return JSON.parse(JSON.stringify({ toJSON: registry.toJSON })); });
    }));
    print(driver(function run() {
        return registry.callsBack(function viaHost() { return JSON.stringify({ toJSON() { return registry.outerRead(undefined); } }); });
    }));
)js"_s, R"(returned leaf < toJSON < stringify < viaHost < callsBack < run < driver
TypeError: innerRead < outerRead < toJSON < stringify < viaHost < callsBack < run < driver
)"_s);
}

static void testTailCallThatThrowsBeforeTheCalleeIsEntered()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.callsClosure(registry.tailCaller, undefined, 1); }));
    print(driver(function run() { return registry.callsClosure(registry.tailCaller, 5, 1); }));
    print(driver(function run() { return registry.callsClosure(registry.tailCaller, { }, 1); }));
    print(driver(function run() { return registry.tailCaller(undefined, 1); }));
    print(driver(function run() { return registry.tailCaller({ }, 1); }));
)js"_s, R"(TypeError: tailCaller < callsClosure < run < driver
TypeError: tailCaller < callsClosure < run < driver
TypeError: tailCaller < callsClosure < run < driver
TypeError: tailCaller < run < driver
TypeError: tailCaller < run < driver
)"_s);
}

static void testTailCallThatThrowsInsideItsCallee()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.callsClosure(registry.tailCaller, BigInt, 1.5); }));
    print(driver(function run() { return registry.tailCaller(BigInt, 1.5); }));
    print(driver(function run() { return registry.callsClosure(registry.tailCaller, leaf, 1); }));
    print(driver(function run() { return registry.callsClosure(registry.tailCaller, registry.outerRead, undefined); }));
)js"_s, R"(RangeError: BigInt < callsClosure < run < driver
RangeError: BigInt < run < driver
returned leaf < callsClosure < run < driver
TypeError: innerRead < outerRead < callsClosure < run < driver
)"_s);
}

static void testInlinedTailCallThatThrowsBeforeTheCalleeIsEntered()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.outerOfInlinedTail(undefined, 1); }));
    print(driver(function run() { return registry.outerOfInlinedTail(5, 1); }));
    print(driver(function run() { return registry.outerOfInlinedTail({ }, 1); }));
)js"_s, R"(TypeError: tailCaller < outerOfInlinedTail < run < driver
TypeError: tailCaller < outerOfInlinedTail < run < driver
TypeError: tailCaller < outerOfInlinedTail < run < driver
)"_s);
}

static void testInlinedTailCallThatThrowsInsideItsCallee()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.outerOfInlinedTail(BigInt, 1.5); }));
    print(driver(function run() { return registry.outerOfInlinedTail(leaf, 1); }));
    print(driver(function run() { return registry.outerOfInlinedTail(registry.outerRead, undefined); }));
)js"_s, R"(RangeError: BigInt < outerOfInlinedTail < run < driver
returned leaf < outerOfInlinedTail < run < driver
TypeError: innerRead < outerRead < outerOfInlinedTail < run < driver
)"_s);
}

static void testInlinedTailCallOfAMethodAndOfAKnownFunction()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.outerOfInlinedMethodTail({ run: leaf }, 1); }));
    print(driver(function run() { return registry.outerOfInlinedMethodTail({ run: BigInt }, 1.5); }));
    print(driver(function run() { return registry.outerOfInlinedMethodTail({ }, 1); }));
    print(driver(function run() { return registry.outerOfInlinedDirectTail(undefined); }));
)js"_s, R"(returned leaf < outerOfInlinedMethodTail < run < driver
RangeError: BigInt < outerOfInlinedMethodTail < run < driver
TypeError: tailCallerOfMethod < outerOfInlinedMethodTail < run < driver
TypeError: innerRead < outerOfInlinedDirectTail < run < driver
)"_s);
}

static void testTailCallOfABuiltinThatThrows()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.tailCallerOfBuiltin(1.5); }));
    print(driver(function run() { return registry.callerOfBuiltin(1.5); }));
    print(driver(function run() { return registry.callsClosure(registry.tailCallerOfBuiltin, 1.5); }));
)js"_s, R"(RangeError: BigInt < run < driver
RangeError: BigInt < callerOfBuiltin < run < driver
RangeError: BigInt < callsClosure < run < driver
)"_s);
}

static void testNoFrameIsCompiledWithoutAnImage()
{
    Realms realms;
    VM& vm = realms.vm();
    AOT_TEST_CHECK(!vm.hasCompiledCode());
    CallerFrameAndPC record { nullptr, nullptr };
    auto* frame = reinterpret_cast<CallFrame*>(&record);
    AOT_TEST_CHECK(!isOperationFrameRecord(vm, nullptr) && !isOperationFrameRecord(vm, frame));
    AOT_TEST_CHECK(!isTopFrameCompiled(vm));
    AOT_TEST_CHECK(!CompiledFrameIterator::tryCreateAtTop(vm));
    AOT_TEST_CHECK(!CompiledFrameIterator::tryCreate(vm, nullptr, nullptr, nullptr));
    AOT_TEST_CHECK(!CompiledFrameIterator::tryCreate(vm, frame, &record, nullptr));
}

static void testWhichFramesAreShown()
{
    ShownFrameFilter filter;
    AOT_TEST_CHECK(!filter.hasShownFrame());
    AOT_TEST_CHECK(!filter.shows(TraceVisibility::Hidden, false) && !filter.hasShownFrame());
    AOT_TEST_CHECK(!filter.shows(TraceVisibility::Visible, true) && !filter.hasShownFrame());
    AOT_TEST_CHECK(!filter.shows(TraceVisibility::VisibleOnlyOnTop, true) && !filter.hasShownFrame());
    AOT_TEST_CHECK(filter.shows(TraceVisibility::VisibleOnlyOnTop, false) && filter.hasShownFrame());
    AOT_TEST_CHECK(!filter.shows(TraceVisibility::VisibleOnlyOnTop, false));
    AOT_TEST_CHECK(filter.shows(TraceVisibility::Visible, false) && filter.shows(TraceVisibility::Visible, false));

    ShownFrameFilter belowShownFrame(true);
    AOT_TEST_CHECK(!belowShownFrame.shows(TraceVisibility::VisibleOnlyOnTop, false) && belowShownFrame.shows(TraceVisibility::Visible, false));

    ShownFrameFilter withWhateverIsOnTop = ShownFrameFilter::showingWhateverIsOnTop(false);
    AOT_TEST_CHECK(!withWhateverIsOnTop.shows(TraceVisibility::Hidden, true) && !withWhateverIsOnTop.hasShownFrame());
    AOT_TEST_CHECK(withWhateverIsOnTop.shows(TraceVisibility::Hidden, false) && withWhateverIsOnTop.hasShownFrame());
    AOT_TEST_CHECK(!withWhateverIsOnTop.shows(TraceVisibility::Hidden, false) && withWhateverIsOnTop.shows(TraceVisibility::Visible, false));
    AOT_TEST_CHECK(!ShownFrameFilter::showingWhateverIsOnTop(true).shows(TraceVisibility::Hidden, false));
}

struct CallSiteCacheSlots {
    static constexpr uintptr_t firstAddress = 0x100000000000;

    const void* addressWith(size_t primary, std::initializer_list<size_t> secondaries)
    {
        for (uintptr_t address = nextAddress + (primary << 2);; address += CallSiteCache::initialNumberOfEntries << 2) {
            size_t secondary = address * 0x9e3779b97f4a7c15ull >> (64 - std::countr_zero(CallSiteCache::initialNumberOfEntries));
            if (std::ranges::find(secondaries, secondary) != secondaries.end()) {
                nextAddress = (address & ~((CallSiteCache::initialNumberOfEntries << 2) - 1)) + (CallSiteCache::initialNumberOfEntries << 2);
                return std::bit_cast<const void*>(address);
            }
        }
    }

    unsigned askForAll(std::span<const void* const> addresses)
    {
        unsigned numberNotFound = 0;
        for (const void* address : addresses) {
            if (cache.find(address))
                continue;
            ++numberNotFound;
            cache.slotForNewEntry(address).address = address;
        }
        return numberNotFound;
    }

    size_t numberOfEntries() const { return cache.m_entries.size(); }

    CallSiteCache cache;
    uintptr_t nextAddress { firstAddress };
};

static void testCallSiteCacheMakesRoomInTheOtherSlot()
{
    CallSiteCacheSlots slots;
    const void* addresses[] = { slots.addressWith(5, { 5 }), slots.addressWith(9, { 70 }), slots.addressWith(9, { 5 }) };
    for (unsigned turn = 0; turn < 8; ++turn)
        slots.askForAll(addresses);
    AOT_TEST_CHECK(!slots.askForAll(addresses));
    AOT_TEST_CHECK(slots.numberOfEntries() == CallSiteCache::initialNumberOfEntries);
}

static void testCallSiteCacheGrowsWhereEntriesPushEachOtherOut()
{
    CallSiteCacheSlots slots;
    const void* addresses[] = { slots.addressWith(5, { 9 }), slots.addressWith(9, { 5 }), slots.addressWith(5, { 9 }) };
    AOT_TEST_CHECK(slots.askForAll(addresses) == 3);
    AOT_TEST_CHECK(slots.askForAll(addresses));
    for (unsigned turn = 0; turn < 8 * CallSiteCache::maxNumberOfEntries; ++turn)
        slots.askForAll(addresses);
    AOT_TEST_CHECK(!slots.askForAll(addresses));
    AOT_TEST_CHECK(slots.numberOfEntries() > CallSiteCache::initialNumberOfEntries);
}

#if OS(LINUX) && ENABLE(SAMPLING_PROFILER)
static constexpr ASCIILiteral textOfSampledProgram = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 module=m0 fields=[{key="q" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1
layout L1 kind=Instance name="Class" module=m0 constructor=f28 fields=[{key="x" declared={type=anything}}]
variable g0 name="registry" module=m0 declared={type=anything} initialization=Undefined flags=[IsImmutable]
data d0 root=Object{layout=L0} module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    PutModuleVariable v1 variable=g0
    v3 = NewClosure function=f1
    PropSet v1, v3 key="run"
    v5 = NewClosure function=f2
    PropSet v1, v5 key="small"
    v7 = NewClosure function=f3
    PropSet v1, v7 key="smallAroundSmall"
    v9 = NewClosure function=f4
    PropSet v1, v9 key="keeps"
    v11 = NewClosure function=f5
    PropSet v1, v11 key="passesSix"
    v13 = NewClosure function=f6
    PropSet v1, v13 key="tailCalls"
    v15 = NewClosure function=f7
    PropSet v1, v15 key="gets"
    v17 = NewClosure function=f8
    PropSet v1, v17 key="sets"
    v19 = NewClosure function=f9
    PropSet v1, v19 key="traps"
    v21 = NewClosure function=f10
    PropSet v1, v21 key="rest"
    v23 = NewClosure function=f11
    PropSet v1, v23 key="count"
    v25 = NewClosure function=f12
    PropSet v1, v25 key="restCalls"
    v27 = NewClosure function=f13
    PropSet v1, v27 key="six"
    v29 = NewClosure function=f14
    PropSet v1, v29 key="ping"
    v31 = NewClosure function=f15
    PropSet v1, v31 key="pong"
    v33 = NewClosure function=f16
    PropSet v1, v33 key="constructs"
    v35 = NewClosure function=f17
    PropSet v1, v35 key="Plain"
    v37 = NewClosure function=f18
    PropSet v1, v37 key="same"
    v39 = NewClosure function=f19
    PropSet v1, v39 key="compares"
    v41 = NewClosure function=f20
    PropSet v1, v41 key="power"
    v43 = NewClosure function=f21
    PropSet v1, v43 key="adds"
    v45 = NewClosure function=f22
    PropSet v1, v45 key="parses"
    v47 = NewClosure function=f23
    PropSet v1, v47 key="fromCharCode"
    v49 = NewClosure function=f24
    PropSet v1, v49 key="passesList"
    v51 = NewClosure function=f25
    PropSet v1, v51 key="constructsPlainWithList"
    v53 = NewClosure function=f26
    PropSet v1, v53 key="constructsClassWithList"
    v55 = NewClosure function=f27
    PropSet v1, v55 key="PlainCaller"
    v57 = NewClosure function=f28
    v58 = NewClass v57 layout=L1
    v59 = FinishClass v58 layout=L1
    PropSet v1, v59 key="Class"
    Return
}
function f1 name="run" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Integer integer=3
    v2 = CallDirect v0, v1 function=f3
    v3 = CallDirect v0, v1 function=f4
    v4 = Call v0, -, v1
    v5 = Add v2, v3
    v6 = Add v5, v4
    Return v6
}
function f2 name="small" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0, -, v1, v1
    v3 = Integer integer=1
    v4 = Add v2, v3
    Return v4
}
function f3 name="smallAroundSmall" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = GetModuleVariable variable=g0
    v3 = PropGet v2 key="small"
    v4 = Call v3, v2, v0, v1 calleeKey="small"
    v5 = Integer integer=1
    v6 = Add v4, v5
    Return v6
}
function f4 name="keeps" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0, -, v1
    v3 = Call v0, -, v1
    v4 = Add v2, v3
    v5 = Add v4, v1
    Return v5
}
function f5 name="passesSix" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="callee"
    v3 = Call v2, v1, v0, v0, v0, v0, v0, v0 calleeKey="callee"
    v4 = Add v3, v0
    Return v4
}
function f6 name="tailCalls" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="callee"
    v3 = Call v2, v1, v0, v0 calleeKey="callee"
    ReturnCall v3
}
function f7 name="gets" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="holder"
    v3 = PropGet v2 key="accessor"
    v4 = Add v3, v0
    Return v4
}
function f8 name="sets" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="holder"
    PropSet v2, v0 key="accessor"
    Return v0
}
function f9 name="traps" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="trapped"
    v3 = PropGet v2 key="a"
    v4 = Add v3, v0
    Return v4
}
function f10 name="rest" flags=[HasRestParameter] parameters=[{type=Array flags=[IsFromCompiler]}] results=[{type=anything}] {
    v1 = PropGet v0 key="length"
    Return v1
}
function f11 name="count" flags=[HasArgumentCount] results=[{type=anything}] {
  b0(v0: int[0, 4294967295]):
    Return v0
}
function f12 name="restCalls" flags=[HasRestParameter] parameters=[{type=Array flags=[IsFromCompiler]}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="count"
    v3 = Call v2, v1, v0 calleeKey="count"
    v4 = Integer integer=1
    v5 = Add v3, v4
    Return v5
}
function f13 name="six" parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v6 = Add v0, v1
    v7 = Add v6, v4
    v8 = Add v7, v5
    Return v8
}
function f14 name="ping" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    v2 = Less v1, v0
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = Sub v0, v1
    v4 = GetModuleVariable variable=g0
    v5 = PropGet v4 key="pong"
    v6 = Call v5, v4, v3 calleeKey="pong"
    v7 = Add v6, v1
    Return v7
  b2():
    Return v0
}
function f15 name="pong" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1() handler=b2():
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="ping"
    v3 = Call v2, v1, v0 calleeKey="ping"
    v4 = Add v3, v0
    Return v4
  b2() role=CatchHandler:
    v5 = CatchValue
    Return v5
}
function f16 name="constructs" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="Plain"
    v3 = Construct v2, -, v0
    v4 = PropGet v3 key="x"
    v5 = Add v4, v0
    Return v5
}
function f17 name="Plain" flags=[HasThis] parameters=[{type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="x"
    v3 = Undefined
    Return v3
}
function f18 name="same" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = StrictEq v0, v1
    Return v2
}
function f19 name="compares" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="text"
    v3 = Add v2, v0
    v4 = Add v2, v0
    v5 = PropGet v1 key="same"
    v6 = Call v5, -, v3, v4
    v7 = Add v6, v0
    Return v7
}
function f20 name="power" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Double number=1.5
    v2 = Pow v0, v1
    Return v2
}
function f21 name="adds" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Data data=d0
    v2 = String string="p"
    v3 = Add v2, v0
    ElemSet v1, v3, v0
    PropSet v1, v0 key="q"
    v6 = PropGet v1 key="q"
    Return v6
}
function f22 name="parses" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=ParseInt
    v2 = String string="12"
    v3 = Add v2, v0
    v4 = Call v1, -, v3
    v5 = Add v4, v0
    Return v5
}
function f23 name="fromCharCode" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=String
    v2 = Builtin builtin=StringFromCharCode
    v3 = Integer integer=66
    v4 = Call v2, v1, v0, v3
    v5 = PropGet v4 key="length"
    Return v5
}
function f24 name="passesList" flags=[HasRestParameter] parameters=[{type=Array flags=[IsFromCompiler]}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="callee"
    v3 = CallList v2, v1, v0 calleeKey="callee"
    v4 = Integer integer=1
    v5 = Add v3, v4
    Return v5
}
function f25 name="constructsPlainWithList" flags=[HasRestParameter] parameters=[{type=Array flags=[IsFromCompiler]}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="PlainCaller"
    v3 = ConstructList v2, -, v0
    v4 = PropGet v3 key="x"
    Return v4
}
function f26 name="constructsClassWithList" flags=[HasRestParameter] parameters=[{type=Array flags=[IsFromCompiler]}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = PropGet v1 key="Class"
    v3 = ConstructList v2, -, v0
    v4 = PropGet v3 key="x"
    Return v4
}
function f27 name="PlainCaller" flags=[HasThis] parameters=[{type=anything}] results=[{type=anything}] {
    v2 = GetModuleVariable variable=g0
    v3 = PropGet v2 key="callee"
    v4 = Call v3, v2, v1 calleeKey="callee"
    PropSet v0, v4 key="x"
    v6 = Undefined
    Return v6
}
function f28 name="Class" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=anything}] results=[{type=object flags=[IsFromCompiler]}] constructedClass=L1 {
    v2 = GetConstructPrototype v0 layout=L1
    v3 = NewObject v2 layout=L1
    v4 = GetModuleVariable variable=g0
    v5 = PropGet v4 key="callee"
    v6 = Call v5, v4, v1 calleeKey="callee"
    PropSet v3, v6 key="x"
    Return v3
}
)IR"_s;

// Keeps track of the calls that are made, and holds what the sampling profiler would find in front of each instruction against them.
class SteppingSampler {
    WTF_MAKE_NONCOPYABLE(SteppingSampler);
public:
    explicit SteppingSampler(VM& vm)
        : m_vm(vm)
        , m_image(*vm.aotState().image)
        , m_stack { Thread::currentSingleton().stack() }
        , m_signalStack(1 * MB)
    {
        s_current = this;
        stack_t signalStack { m_signalStack.mutableSpan().data(), 0, m_signalStack.size() };
        AOT_TEST_CHECK(!sigaltstack(&signalStack, &m_previousSignalStack));
        struct sigaction action { };
        action.sa_sigaction = [](int, siginfo_t*, void* context) {
            s_current->didTrap(static_cast<ucontext_t*>(context)->uc_mcontext);
        };
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        AOT_TEST_CHECK(!sigaction(SIGTRAP, &action, &m_previousAction));
    }

    ~SteppingSampler()
    {
        AOT_TEST_CHECK(!sigaction(SIGTRAP, &m_previousAction, nullptr));
        AOT_TEST_CHECK(!sigaltstack(&m_previousSignalStack, nullptr));
#if CPU(ARM64)
        AOT_TEST_CHECK(!munmap(m_copies, pageSize()));
#endif
    }

#if CPU(X86_64)
    static ALWAYS_INLINE void start() { asm volatile("subq $128, %%rsp\n\tpushfq\n\torq $0x100, (%%rsp)\n\tpopfq\n\taddq $128, %%rsp" ::: "memory", "cc"); }
    static ALWAYS_INLINE void stop() { asm volatile("subq $128, %%rsp\n\tpushfq\n\tandq $~0x100, (%%rsp)\n\tpopfq\n\taddq $128, %%rsp" ::: "memory", "cc"); }
#else
    static ALWAYS_INLINE void start() { asm volatile("brk #1" ::: "memory"); }
    static ALWAYS_INLINE void stop() { asm volatile("brk #2" ::: "memory"); }
#endif

    unsigned numberOfSamples() const { return m_numberOfSamples; }
    const std::optional<CodeLocation>& firstMistake() const { return m_firstMistake; }

private:
    using Functions = Vector<uint32_t, 32>;
    struct Call {
        void** stackPointerOfCallee;
        void* returnAddress;
    };

#if CPU(X86_64)
    static constexpr greg_t trapFlag = 0x100;
    __attribute__((naked)) static void resumeStepping() { asm("int3"); }

    SUPPRESS_ASAN void didTrap(mcontext_t& context)
    {
        auto& registers = context.gregs;
        if (void* returnAddress = std::exchange(m_returnAddressOfCallSteppedOver, nullptr)) {
            registers[REG_RIP] = std::bit_cast<greg_t>(returnAddress);
            registers[REG_EFL] |= trapFlag;
            m_isBehindCall = false;
        }
        auto* pc = std::bit_cast<uint8_t*>(registers[REG_RIP]);
        auto** stackPointer = std::bit_cast<void**>(registers[REG_RSP]);
        auto* framePointer = std::bit_cast<void*>(registers[REG_RBP]);
        if (m_isBehindCall) {
            if (stepsOverCallReturningTo(*stackPointer)) {
                m_returnAddressOfCallSteppedOver = std::exchange(*stackPointer, std::bit_cast<void*>(&resumeStepping));
                registers[REG_EFL] &= ~trapFlag;
                return;
            }
            didCall({ stackPointer, *stackPointer }, framePointer);
        }
        // A return address may be popped, to be pushed somewhere else.
        while (!m_calls.isEmpty() && m_calls.last().stackPointerOfCallee + !m_isBehindReturn < stackPointer)
            m_calls.removeLast();
        const uint8_t* opcode = pc + (*pc >> 4 == 0x4);
        m_isBehindCall = *opcode == 0xe8 || (*opcode == 0xff && (opcode[1] >> 3 & 7) == 2);
        m_isBehindReturn = *opcode == 0xc3 || *opcode == 0xc2;
        checkSample({ framePointer, pc, nullptr, stackPointer, std::bit_cast<void*>(registers[REG_R11]) });
    }
#else
    __attribute__((naked)) static void resumeStepping() { asm("brk #0"); }
    static constexpr uint32_t breakpoint(unsigned number) { return 0xd4200000 | number << 5; }
    static constexpr unsigned linkRegister = 30;

    // There is no trap flag. What depends on the program counter is carried out here, and the rest is copied in front of a breakpoint.
    SUPPRESS_ASAN void didTrap(mcontext_t& registers)
    {
        auto* pc = std::bit_cast<uint32_t*>(registers.pc);
        if (void* returnAddress = std::exchange(m_returnAddressOfCallSteppedOver, nullptr)) {
            registers.regs[linkRegister] = std::bit_cast<uintptr_t>(returnAddress);
            pc = static_cast<uint32_t*>(returnAddress);
        } else
            pc = *pc == breakpoint(1) ? pc + 1 : m_exits[pc - m_firstBreakpoint];
        while (pc)
            pc = step(registers, pc);
    }

    static uint32_t* targetOf(uint32_t* pc, unsigned firstBit, unsigned numberOfBits)
    {
        return pc + (static_cast<int32_t>(*pc >> firstBit << (32 - numberOfBits)) >> (32 - numberOfBits));
    }

    // Null if the thread has to run.
    SUPPRESS_ASAN uint32_t* step(mcontext_t& registers, uint32_t* pc)
    {
        uint32_t instruction = *pc;
        if (instruction == breakpoint(2)) {
            registers.pc = std::bit_cast<uintptr_t>(pc + 1);
            return nullptr;
        }
        auto** stackPointer = std::bit_cast<void**>(registers.sp);
        auto* framePointer = std::bit_cast<void*>(registers.regs[29]);
        bool isImmediateBranch = (instruction & 0x7c000000) == 0x14000000;
        bool isRegisterBranch = (instruction & 0xff9ffc1f) == 0xd61f0000;
        auto* destination = isImmediateBranch ? targetOf(pc, 0, 26) : isRegisterBranch ? std::bit_cast<uint32_t*>(registers.regs[instruction >> 5 & 31]) : nullptr;
        // See generateCreateConstantAtSite().
        bool skipsWordBehindCall = isRegisterBranch && !m_calls.isEmpty() && destination - 1 == m_calls.last().returnAddress;
        if (!skipsWordBehindCall)
            checkSample({ framePointer, pc, std::bit_cast<void*>(registers.regs[linkRegister]), stackPointer });

        if ((instruction & 0x1f000000) == 0x10000000) {
            intptr_t distance = (targetOf(pc, 5, 19) - pc) * 4 + (instruction >> 29 & 3);
            bool countsPages = instruction >> 31;
            registers.regs[instruction & 31] = countsPages ? roundDownToMultipleOf<4 * KB>(std::bit_cast<uintptr_t>(pc)) + distance * 4 * KB : std::bit_cast<uintptr_t>(pc) + distance;
            return pc + 1;
        }
        if (!destination) {
            RELEASE_ASSERT((instruction & 0xfe000000) != 0xd6000000);
            copyInFrontOfBreakpoints(pc);
            registers.pc = std::bit_cast<uintptr_t>(m_copies);
            return nullptr;
        }
        if (bool isCall = isImmediateBranch ? instruction >> 31 : (instruction >> 21 & 3) == 1; !isCall) {
            size_t callLeft = m_calls.reverseFindIf([&](const Call& call) {
                return call.returnAddress == destination - skipsWordBehindCall;
            });
            if (callLeft != notFound)
                m_calls.shrink(callLeft);
            return destination;
        }
        if (stepsOverCallReturningTo(pc + 1)) {
            m_returnAddressOfCallSteppedOver = pc + 1;
            registers.regs[linkRegister] = std::bit_cast<uintptr_t>(&resumeStepping);
            registers.pc = std::bit_cast<uintptr_t>(destination);
            return nullptr;
        }
        registers.regs[linkRegister] = std::bit_cast<uintptr_t>(pc + 1);
        didCall({ stackPointer, pc + 1 }, framePointer);
        return destination;
    }

    // A trap makes an exclusive store fail.
    static unsigned numberOfInstructionsToRunAtOnce(const uint32_t* pc)
    {
        constexpr uint32_t mask = 0x3fe00000;
        constexpr unsigned maxInstructions = 16;
        for (unsigned count = 1; (*pc & mask) == 0x08400000 && count <= maxInstructions; ++count) {
            if ((pc[count - 1] & mask) == 0x08000000)
                return count;
        }
        return 1;
    }

    SUPPRESS_ASAN void copyInFrontOfBreakpoints(uint32_t* pc)
    {
        constexpr unsigned firstBit = 5;
        constexpr unsigned indexOfLiteral = 64;
        constexpr unsigned maxSizeOfLiteral = 16;
        unsigned count = numberOfInstructionsToRunAtOnce(pc);
        m_firstBreakpoint = m_copies + count;
        m_exits.shrink(0);
        m_exits.append(pc + count);
        for (unsigned i = 0; i < count; ++i) {
            uint32_t instruction = pc[i];
            auto pointTo = [&](unsigned index, unsigned numberOfBits) {
                instruction = (instruction & ~(((1u << numberOfBits) - 1) << firstBit)) | (index - i) << firstBit;
                return targetOf(pc + i, firstBit, numberOfBits);
            };
            if ((instruction & 0xff000000) == 0x54000000 || (instruction & 0x7e000000) == 0x34000000)
                m_exits.append(pointTo(count + m_exits.size(), 19));
            else if ((instruction & 0x7e000000) == 0x36000000)
                m_exits.append(pointTo(count + m_exits.size(), 14));
            else if ((instruction & 0x3b000000) == 0x18000000)
                memcpy(m_copies + indexOfLiteral, pointTo(indexOfLiteral, 19), maxSizeOfLiteral);
            m_copies[i] = instruction;
        }
        std::fill_n(m_firstBreakpoint, m_exits.size(), breakpoint(0));
        __builtin___clear_cache(reinterpret_cast<char*>(m_copies), reinterpret_cast<char*>(m_firstBreakpoint + m_exits.size()));
    }
#endif

    bool stepsOverCallReturningTo(void* returnAddress) const { return isNativeCode(returnAddress) && runsCompiledCode() && isNativeCode(m_calls.last().returnAddress); }

    void didCall(const Call& call, void* framePointer)
    {
        m_calls.append(call);
        fillWithRemainsOfCalls(call, framePointer);
    }

    void checkSample(const SampledRegisters& registers)
    {
        Functions expected;
        appendIfFunction(expected, m_image.locate(registers.pc));
        for (const Call& call : m_calls | std::views::reverse)
            appendIfFunction(expected, m_image.locate(call.returnAddress));
        if (expected.isEmpty() || vmEntryRecord(m_vm.topEntryFrame)->prevTopEntryFrame())
            return;
        ++m_numberOfSamples;
        if (!m_firstMistake && sample(registers) != expected)
            m_firstMistake = m_image.locate(registers.pc);
    }

    SUPPRESS_ASAN void fillWithRemainsOfCalls(const Call& call, void* framePointer)
    {
        if (CodeLocation site = m_image.locate(call.returnAddress); site.kind == CodeKind::Function || site.stubFrameKind != StubFrameKind::Plain)
            m_returnAddressesSeen[m_numberOfReturnAddressesSeen++ % m_returnAddressesSeen.size()] = call.returnAddress;
        bool leavesFrameRecords = m_random.returnTrueWithProbability(0.5);
        for (void** word = call.stackPointerOfCallee - 64; word < call.stackPointerOfCallee; ++word) {
            bool isAligned = !(std::bit_cast<uintptr_t>(word) % stackAlignmentBytes());
            *word = leavesFrameRecords && isAligned ? framePointer : m_returnAddressesSeen[m_random.getUint32(m_returnAddressesSeen.size())];
        }
    }

    bool isNativeCode(void* pc) const { return m_image.locate(pc).kind == CodeKind::NotInImage && !isJITPC(pc) && !LLInt::isLLIntPC(pc); }

    bool runsCompiledCode() const
    {
        return m_calls.containsIf([&](const Call& call) {
            return m_image.locate(call.returnAddress).runsCompiledCode();
        });
    }

    static void appendIfFunction(Functions& functions, const CodeLocation& code)
    {
        if (code.kind == CodeKind::Function)
            functions.append(code.function);
    }

    // As SamplingProfiler does.
    SUPPRESS_ASAN std::optional<Functions> sample(SampledRegisters registers)
    {
        bool runsInFrameOfEngine = isJITPC(registers.pc) || LLInt::isLLIntPC(registers.pc);
        if (runsInFrameOfEngine)
            registers.linkRegister = nullptr;
        std::optional<SampledTop> top = findSampledTop(m_image, registers, runsInFrameOfEngine ? static_cast<CallFrame*>(registers.framePointer) : m_vm.topCallFrame, m_vm.topEntryFrame, m_stack.bounds);
        if (!top)
            return std::nullopt;
        Functions functions;
        auto [callFrame, pc, framelessTopCallerPC, framePointer] = *top;
        if (framelessTopCallerPC)
            appendIfFunction(functions, m_image.locate(std::exchange(pc, framelessTopCallerPC)));
        while (static_cast<void*>(callFrame) != m_vm.topEntryFrame) {
            for (MachineFrameIterator frames(m_image, callFrame, pc, m_stack, std::exchange(framePointer, nullptr)); frames && (*frames).code.runsCompiledCode(); ++frames) {
                appendIfFunction(functions, (*frames).code);
                callFrame = (*frames).callFrame;
            }
            if (static_cast<void*>(callFrame) == m_vm.topEntryFrame)
                break;
            auto* record = reinterpret_cast<const CallerFrameAndPC*>(callFrame);
            if (!m_stack.contains(record) || record->callerFrame <= callFrame)
                return std::nullopt;
            pc = removeReturnAddressSignature(record->returnPC);
            callFrame = record->callerFrame;
        }
        return functions;
    }

    static inline SteppingSampler* s_current;
    VM& m_vm;
    const Image& m_image;
    SuspendedThreadStack m_stack;
    Vector<uint8_t> m_signalStack;
    stack_t m_previousSignalStack;
    struct sigaction m_previousAction { };
    Vector<Call, 256> m_calls;
    void* m_returnAddressOfCallSteppedOver { nullptr };
    std::array<void*, 7> m_returnAddressesSeen { };
    unsigned m_numberOfReturnAddressesSeen { 0 };
    WeakRandom m_random { 1 };
#if CPU(X86_64)
    bool m_isBehindCall { false };
    bool m_isBehindReturn { false };
#else
    uint32_t* m_copies { static_cast<uint32_t*>(mmap(nullptr, pageSize(), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) };
    uint32_t* m_firstBreakpoint { nullptr };
    Vector<uint32_t*, 16> m_exits;
#endif
    unsigned m_numberOfSamples { 0 };
    std::optional<CodeLocation> m_firstMistake;
};

static void checkSamplesAtEveryInstruction(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    JSObject* registry = program.realms().object(realm, R"js(globalThis.registry = {
        text: "a text that is too long to be compared in place, as long as it is made of two",
        interpreted() { return arguments.length; },
        interpretedCaller(x) { return registry.callee(x) + 1; },
        host: Object.is,
        proxy: new Proxy(Object.is, { }),
        setCallee(callee) {
            registry.callee = callee;
            registry.holder = Object.defineProperty({ }, "accessor", { get: callee, set: callee });
            registry.trapped = new Proxy(function () { }, { get: callee });
        },
    })js"_s);
    evaluateModuleSynchronously(realm, ModuleID { 0u });
    DeferGC deferGC(vm);
    Ref samplingProfiler = vm.createSamplingProfiler(Stopwatch::create());
    auto registered = [&](ASCIILiteral name) {
        return registry->getDirect(vm, Identifier::fromString(vm, name));
    };
    auto call = [&](JSValue function, JSValue argument) {
        MarkedArgumentBuffer arguments;
        arguments.append(argument);
        JSC::call(realm, function, JSC::getCallData(function), jsUndefined(), arguments);
    };

    for (ASCIILiteral name : { "rest"_s, "count"_s, "restCalls"_s, "six"_s, "ping"_s, "constructs"_s, "compares"_s, "power"_s, "adds"_s, "parses"_s, "fromCharCode"_s, "interpreted"_s, "host"_s, "proxy"_s }) {
        JSValue callee = registered(name);
        call(registered("setCallee"_s), callee);
        SteppingSampler sampler(vm);
        SteppingSampler::start();
        call(registered("run"_s), callee);
        call(callee, jsNumber(3));
        for (ASCIILiteral caller : { "interpretedCaller"_s, "passesSix"_s, "passesList"_s, "tailCalls"_s, "gets"_s, "sets"_s, "traps"_s, "constructsPlainWithList"_s, "constructsClassWithList"_s })
            call(registered("run"_s), registered(caller));
        SteppingSampler::stop();
        if (auto mistake = sampler.firstMistake())
            dataLogLn("kind of code ", static_cast<unsigned>(mistake->kind), ", function ", mistake->function, ", offset ", mistake->offset);
        AOT_TEST_CHECK_FOR_SOURCE(!sampler.firstMistake(), name);
        AOT_TEST_CHECK_FOR_SOURCE(sampler.numberOfSamples() > 1000, name);
    }
    samplingProfiler->shutdown();
}
#endif

std::span<const NativeTest> compiledFrameTests()
{
    static constexpr NativeTest tests[] = {
        { "compiled-frames/trace-taken-in-an-operation"_s, testTraceTakenInAnOperation },
        { "compiled-frames/trace-taken-in-a-host-function"_s, testTraceTakenInAHostFunction },
        { "compiled-frames/trace-taken-in-interpreted-code"_s, testTraceTakenInInterpretedCode },
        { "compiled-frames/trace-across-two-entry-frames"_s, testTraceAcrossTwoEntryFrames },
        { "compiled-frames/tail-call-that-throws-before-the-callee-is-entered"_s, testTailCallThatThrowsBeforeTheCalleeIsEntered },
        { "compiled-frames/tail-call-that-throws-inside-its-callee"_s, testTailCallThatThrowsInsideItsCallee },
        { "compiled-frames/inlined-tail-call-that-throws-before-the-callee-is-entered"_s, testInlinedTailCallThatThrowsBeforeTheCalleeIsEntered },
        { "compiled-frames/inlined-tail-call-that-throws-inside-its-callee"_s, testInlinedTailCallThatThrowsInsideItsCallee },
        { "compiled-frames/inlined-tail-call-of-a-method-and-of-a-known-function"_s, testInlinedTailCallOfAMethodAndOfAKnownFunction },
        { "compiled-frames/tail-call-of-a-builtin-that-throws"_s, testTailCallOfABuiltinThatThrows },
        { "compiled-frames/no-frame-is-compiled-without-an-image"_s, testNoFrameIsCompiledWithoutAnImage },
        { "compiled-frames/which-frames-are-shown"_s, testWhichFramesAreShown },
#if OS(LINUX) && ENABLE(SAMPLING_PROFILER)
        { "compiled-frames/sampled-at-every-instruction"_s, runNativeTestWithLoadedProgram<compiledFrameTests, textOfSampledProgram, checkSamplesAtEveryInstruction> },
#endif
        { "compiled-frames/call-site-cache-makes-room-in-the-other-slot"_s, testCallSiteCacheMakesRoomInTheOtherSlot },
        { "compiled-frames/call-site-cache-grows-where-entries-push-each-other-out"_s, testCallSiteCacheGrowsWhereEntriesPushEachOtherOut },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
