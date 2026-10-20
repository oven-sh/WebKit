/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTStackTrace.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "JSCInlines.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    v2 = NewClosure function=f1
    PropSet v1, v2 key="recurse"
    v4 = NewClosure function=f2
    PropSet v1, v4 key="outerCapture"
    v6 = NewClosure function=f3
    PropSet v1, v6 key="middleCapture"
    v8 = NewClosure function=f4
    PropSet v1, v8 key="innerCapture"
    v10 = NewClosure function=f5
    PropSet v1, v10 key="callsBack"
    v12 = NewClosure function=f6
    PropSet v1, v12 key="asyncOuter"
    v14 = NewClosure function=f8
    PropSet v1, v14 key="reads"
    v16 = NewClosure function=f9
    PropSet v1, v16 key="recurseAndRead"
    Return
}
function f1 name="recurse" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Claim v0 castType=i32
    v3 = Integer integer=0
    v4 = StrictEq v2, v3
    Branch v4 -> taken b1(), notTaken b2()
  b1():
    v5 = Call v1 @10
    Return v5
  b2():
    v6 = Integer integer=1
    v7 = Sub v2, v6
    v8 = CallDirect v7, v1 function=f1 @20
    Return v8
}
function f2 name="outerCapture" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="registry" [isFreeName]
    v4 = PropGet v3 key="middleCapture"
    v5 = Call v4, v3, v0, v1 @30
    Return v5
}
function f3 name="middleCapture" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="registry" [isFreeName]
    v4 = PropGet v3 key="innerCapture"
    v5 = Call v4, v3, v0, v1 @40
    Return v5
}
function f4 name="innerCapture" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="capture" [isFreeName]
    v4 = Call v3, -, v0, v1 @50
    Return v4
}
function f5 name="callsBack" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Call v0 @60
    Return v1
}
function f6 name="asyncOuter" flags=[IsAsync, HasExplicitName] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = CallDirect v0 function=f7 @70
    v2 = Await v1 @75
    Return v2
}
function f7 name="asyncInner" flags=[IsAsync, HasExplicitName] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    v2 = Await v1 @80
    v3 = Call v0 @85
    Return v3
}
function f8 name="reads" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="missing" @90
    Return v1
}
function f9 name="recurseAndRead" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Claim v0 castType=i32
    v3 = Integer integer=0
    v4 = StrictEq v2, v3
    Branch v4 -> taken b1(), notTaken b2()
  b1():
    v5 = PropGet v1 key="missing" @100
    Return v5
  b2():
    v6 = Integer integer=1
    v7 = Sub v2, v6
    v8 = CallDirect v7, v1 function=f9 @110
    Return v8
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.registry = { };
globalThis.capture = Error.captureStackTrace;
function names(stack) {
    if (typeof stack !== "string")
        return "no stack: " + stack;
    let result = "";
    for (let start = 0; start <= stack.length;) {
        let end = stack.indexOf("\n", start);
        if (end < 0)
            end = stack.length;
        let line = stack.substring(start, end);
        let at = line.indexOf("@");
        let name = at < 0 ? line : line.substring(0, at);
        result += (start ? " < " : "") + name;
        if (name === "driver" || name === "async driver")
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
function withLimit(limit, run) {
    Error.stackTraceLimit = limit;
    try {
        return driver(run);
    } finally {
        Error.stackTraceLimit = 100;
    }
}
globalThis.leaf = function leaf() { return names(new Error().stack); };
)js"_s;

static std::unique_ptr<CompileOutput> compileWithValidation(ASCIILiteral referencesWord)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        AOT_TEST_CHECK(!applyOptionWord(options, "validate"_s));
        // The script of the shell hands the program its own objects, closures and Error.captureStackTrace.
        AOT_TEST_CHECK(!applyOptionWord(options, "admit=RuntimeCode"_s));
        AOT_TEST_CHECK(!applyOptionWord(options, referencesWord));
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        if (!program) {
            build.diagnostics().finish(nullptr);
            Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
        }
        AOT_TEST_CHECK(program);
        output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(!output->image.isEmpty() && output->entryModuleIDs.size() == 1);
    });
    return output;
}

static String runInShell(ASCIILiteral referencesWord, ASCIILiteral statements)
{
    std::unique_ptr<CompileOutput> output = compileWithValidation(referencesWord);
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").then(async () => {"_s, statements, "}).catch(error => print(\"the script threw \" + error));"_s),
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
    for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
        String output = runInShell(referencesWord, statements);
        if (output != expectedOutput)
            dataLog("with ", referencesWord, " the shell printed\n", output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(output == expectedOutput);
    }
}

static void testLimitOfTraceTakenInCompiledCode()
{
    checkShellOutput(R"js(
    for (let limit of [0, 1, 10, 100])
        print(limit + ": " + withLimit(limit, function run() { return registry.recurseAndRead(12, undefined); }));
)js"_s, R"(0: TypeError: no stack: undefined
1: TypeError: recurseAndRead
10: TypeError: recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead
100: TypeError: recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < recurseAndRead < run < driver
)"_s);
}

static void testLimitOfTraceTakenInInterpretedCode()
{
    checkShellOutput(R"js(
    for (let limit of [0, 1, 2, 10, 100])
        print(limit + ": " + withLimit(limit, function run() { return registry.recurse(12, leaf); }));
)js"_s, R"(0: returned no stack: undefined
1: returned leaf
2: returned leaf < recurse
10: returned leaf < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse
100: returned leaf < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < recurse < run < driver
)"_s);
}

static void testLimitThatEndsWhereCompiledFramesEnd()
{
    checkShellOutput(R"js(
    for (let limit of [3, 4, 5])
        print(limit + ": " + withLimit(limit, function run() { return registry.recurseAndRead(2, undefined); }));
)js"_s, R"(3: TypeError: recurseAndRead < recurseAndRead < recurseAndRead
4: TypeError: recurseAndRead < recurseAndRead < recurseAndRead < run
5: TypeError: recurseAndRead < recurseAndRead < recurseAndRead < run < driver
)"_s);
}

static void testCaptureBelowCompiledFunction()
{
    checkShellOutput(R"js(
    function captured(below) {
        let holder = { };
        driver(function run() { return registry.outerCapture(holder, below); });
        return typeof holder.stack + " [" + names(holder.stack) + "]";
    }
    print("everything: " + captured(undefined));
    print("below the innermost: " + captured(registry.innerCapture));
    print("below the middle: " + captured(registry.middleCapture));
    print("below the outermost: " + captured(registry.outerCapture));
    print("below a compiled function that is not running: " + captured(registry.callsBack));
    print("below an interpreted function that is not running: " + captured(leaf));
)js"_s, R"(everything: string [innerCapture < middleCapture < outerCapture < run < driver]
below the innermost: string [middleCapture < outerCapture < run < driver]
below the middle: string [outerCapture < run < driver]
below the outermost: string [run < driver]
below a compiled function that is not running: string []
below an interpreted function that is not running: string []
)"_s);
}

static void testCaptureBelowInterpretedFunctionUnderCompiledFrames()
{
    checkShellOutput(R"js(
    let holder = { };
    function between() { return registry.outerCapture(holder, between); }
    driver(function run() { return registry.callsBack(between); });
    print(names(holder.stack));
)js"_s, R"(callsBack < run < driver
)"_s);
}

static void testTraceThatLeavesAndEntersCompiledCode()
{
    checkShellOutput(R"js(
    print(driver(function run() { return registry.callsBack(function between() { return registry.callsBack(leaf); }); }));
    print(driver(function run() { return registry.callsBack(function between() { return registry.reads(undefined); }); }));
    print(driver(function run() {
        return registry.callsBack(function first() { return registry.recurse(2, function second() { return registry.callsBack(function third() { return registry.reads(undefined); }); }); });
    }));
)js"_s, R"(returned leaf < callsBack < between < callsBack < run < driver
TypeError: reads < between < callsBack < run < driver
TypeError: reads < third < callsBack < second < recurse < recurse < recurse < first < callsBack < run < driver
)"_s);
}

static void testAsyncFramesBelow()
{
    checkShellOutput(R"js(
    {
        async function driver() { return await registry.asyncOuter(function raw() { return new Error().stack; }); }
        print(names(await driver()));
    }
    {
        async function driver() {
            try {
                await registry.asyncOuter(function thrower() { return registry.reads(undefined); });
            } catch (error) {
                return error.stack;
            }
        }
        print(names(await driver()));
    }
)js"_s, R"(raw < asyncInner < async asyncOuter < async driver
reads < thrower < asyncInner < async asyncOuter < async driver
)"_s);
}

static void testSizeOfTrace()
{
    AOT_TEST_CHECK(ReturnAddressTrace::numberOfFlagWords(0) == 0);
    AOT_TEST_CHECK(ReturnAddressTrace::numberOfFlagWords(1) == 1);
    AOT_TEST_CHECK(ReturnAddressTrace::numberOfFlagWords(ReturnAddressTrace::returnAddressesPerFlagWord) == 1);
    AOT_TEST_CHECK(ReturnAddressTrace::numberOfFlagWords(ReturnAddressTrace::returnAddressesPerFlagWord + 1) == 2);
    AOT_TEST_CHECK(ReturnAddressTrace::allocationSize(0) == sizeof(ReturnAddressTrace));
    AOT_TEST_CHECK(ReturnAddressTrace::allocationSize(100) == sizeof(ReturnAddressTrace) + (100 + ReturnAddressTrace::numberOfFlagWords(100)) * sizeof(uint32_t));
    AOT_TEST_CHECK(ReturnAddressTrace::allocationSize(100) - sizeof(ReturnAddressTrace) < 100 * 5);

    constexpr size_t count = 40;
    Vector<uint64_t> storage(FillWith { }, ReturnAddressTrace::allocationSize(count) / sizeof(uint64_t) + 1, static_cast<uint64_t>(0));
    auto& trace = *reinterpret_cast<ReturnAddressTrace*>(storage.mutableSpan().data());
    trace.numberOfReturnAddresses = count;
    AOT_TEST_CHECK(trace.sizeInBytes() == ReturnAddressTrace::allocationSize(count) && trace.returnAddresses().size() == count);
    using Flag = ReturnAddressTrace::Flag;
    trace.addFlagsAt(0, Flag::IsConstructCall);
    trace.addFlagsAt(15, Flag::IsAtFirstReadSite);
    trace.addFlagsAt(16, { Flag::IsConstructCall, Flag::IsAtFirstReadSite });
    trace.addFlagsAt(39, Flag::IsConstructCall);
    for (size_t index = 0; index < count; ++index) {
        OptionSet<Flag> expected;
        if (!index || index == 16 || index == 39)
            expected.add(Flag::IsConstructCall);
        if (index == 15 || index == 16)
            expected.add(Flag::IsAtFirstReadSite);
        AOT_TEST_CHECK(trace.flagsAt(index) == expected);
        AOT_TEST_CHECK(!trace.returnAddresses()[index]);
    }

    uintptr_t codeStart = 0x100000000;
    for (uintptr_t offset : { uintptr_t { 0 }, uintptr_t { codeOffsetUnit }, uintptr_t { 1000 * codeOffsetUnit }, static_cast<uintptr_t>(ReturnAddressTrace::encodableCodeSize - codeOffsetUnit) }) {
        const void* address = std::bit_cast<const void*>(codeStart + offset);
        AOT_TEST_CHECK(ReturnAddressTrace::decode(ReturnAddressTrace::encode(address, codeStart), codeStart) == address);
    }
}

std::span<const NativeTest> stackTraceTests()
{
    static constexpr NativeTest tests[] = {
        { "stack-traces/limit-of-trace-taken-in-compiled-code"_s, testLimitOfTraceTakenInCompiledCode },
        { "stack-traces/limit-of-trace-taken-in-interpreted-code"_s, testLimitOfTraceTakenInInterpretedCode },
        { "stack-traces/limit-that-ends-where-compiled-frames-end"_s, testLimitThatEndsWhereCompiledFramesEnd },
        { "stack-traces/capture-below-compiled-function"_s, testCaptureBelowCompiledFunction },
        { "stack-traces/capture-below-interpreted-function-under-compiled-frames"_s, testCaptureBelowInterpretedFunctionUnderCompiledFrames },
        { "stack-traces/trace-that-leaves-and-enters-compiled-code"_s, testTraceThatLeavesAndEntersCompiledCode },
        { "stack-traces/async-frames-below"_s, testAsyncFramesBelow },
        { "stack-traces/size-of-trace"_s, testSizeOfTrace },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
