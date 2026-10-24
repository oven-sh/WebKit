/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTCompiledCaller.h"
#include "AOTCompiledFrame.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTRealmTestSupport.h"
#include "AOTShellProcess.h"
#include "AOTShownFrameFilter.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
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
  b0(v0: anything):
    v1 = CallDirect v0 function=f2 @10
    Return v1
}
function f2 name="innerRead" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="missing" @30
    Return v1
}
function f3 name="outerHost" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = CallDirect v0, v1 function=f4 @40
    Return v2
}
function f4 name="innerHost" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Call v0, -, v1 @50
    Return v2
}
function f5 name="callsBack" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Call v0 @60
    Return v1
}
function f6 name="toJSON" results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="leaf" [isFreeName]
    v2 = Call v1 @70
    Return v2
}
function f7 name="callsClosure" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = Call v0, -, v1, v2 @80
    Return v3
}
function f8 name="tailCaller" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Call v0, -, v1 @90
    ReturnCall v2
}
function f9 name="outerOfInlinedTail" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = CallDirect v0, v1 function=f8 [isAlwaysInlined] @100
    Return v2
}
function f10 name="tailCallerOfMethod" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = PropGet v0 key="run" @110
    v3 = Call v2, v0, v1 calleeKey="run" @112
    ReturnCall v3
}
function f11 name="outerOfInlinedMethodTail" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = CallDirect v0, v1 function=f10 [isAlwaysInlined] @120
    Return v2
}
function f12 name="tailCallerOfKnownFunction" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = CallDirect v0 function=f2 @130
    ReturnCall v1
}
function f13 name="outerOfInlinedDirectTail" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = CallDirect v0 function=f12 [isAlwaysInlined] @140
    Return v1
}
function f14 name="tailCallerOfBuiltin" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BigInt
    v2 = Call v1, -, v0 @150
    ReturnCall v2
}
function f15 name="callerOfBuiltin" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
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

static std::unique_ptr<CompileOutput> compileWithValidation(ASCIILiteral referencesWord)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        AOT_TEST_CHECK(!applyOptionWord(options, "validate"_s));
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
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").then(() => {"_s, statements, "}).catch(error => print(\"the script threw \" + error));"_s),
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
}

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
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
