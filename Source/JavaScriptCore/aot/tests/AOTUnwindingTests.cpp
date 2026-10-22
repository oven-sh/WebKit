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
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
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
    PropSet v1, v2 key="catchesOwn"
    v4 = NewClosure function=f2
    PropSet v1, v4 key="catchesCallee"
    v6 = NewClosure function=f3
    PropSet v1, v6 key="throws"
    v8 = NewClosure function=f4
    PropSet v1, v8 key="passes"
    v10 = NewClosure function=f5
    PropSet v1, v10 key="keepsValues"
    v12 = NewClosure function=f6
    PropSet v1, v12 key="spins"
    v14 = NewClosure function=f7
    PropSet v1, v14 key="cleansUp"
    v16 = NewClosure function=f8
    PropSet v1, v16 key="spinsWithoutCalls"
    v18 = NewClosure function=f9
    PropSet v1, v18 key="asyncCalls"
    v20 = NewClosure function=f10
    PropSet v1, v20 key="catchesDirectCallee"
    v22 = NewClosure function=f11
    PropSet v1, v22 key="throwsNewError"
    v24 = NewClosure function=f12
    PropSet v1, v24 key="ignoresCallee"
    v26 = NewClosure function=f13
    PropSet v1, v26 key="ignoresNamedCallee"
    v28 = NewClosure function=f14
    PropSet v1, v28 key="catchesNamedCallee"
    v30 = NewClosure function=f16
    PropSet v1, v30 key="ignoresDirectCallees"
    v32 = NewClosure function=f17
    PropSet v1, v32 key="catchesDirectCallees"
    Return
}
function f1 name="catchesOwn" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything) handler=b2():
    v1 = PropGet v0 key="missing" @10
    Jump -> next b1(v1)
  b1(v2):
    Return v2
  b2() role=CatchHandler:
    v3 = CatchValue
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="describe" [isFreeName]
    v6 = String string="caught in the same frame: "
    v7 = Call v5, -, v6, v3
    Return v7
}
function f2 name="catchesCallee" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything) handler=b2():
    v1 = Call v0 @20
    Jump -> next b1(v1)
  b1(v2):
    Return v2
  b2() role=CatchHandler:
    v3 = CatchValue
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="describe" [isFreeName]
    v6 = String string="caught in a caller: "
    v7 = Call v5, -, v6, v3
    Return v7
}
function f3 name="throws" results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="thrown" [isFreeName]
    Throw v1 @30
}
function f4 name="passes" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Call v0 @40
    Return v1
}
function f5 name="keepsValues" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Check v1 castType=number
    v3 = Integer integer=1
    v4 = Add v2, v3
    v5 = Add v4, v3
    v6 = Add v5, v3
    v7 = Add v6, v3
    v8 = Add v7, v3
    v9 = Add v8, v3
    v10 = Add v9, v3
    v11 = Add v10, v3
    v12 = Add v11, v3
    v13 = Add v12, v3
    Jump -> next b1()
  b1() handler=b3(v2, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13):
    v14 = Call v0 @50
    Jump -> next b2(v14)
  b2(v15):
    Return v15
  b3(v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26) role=CatchHandler:
    v27 = CatchValue
    v28 = Add v16, v17
    v29 = Add v28, v18
    v30 = Add v29, v19
    v31 = Add v30, v20
    v32 = Add v31, v21
    v33 = Add v32, v22
    v34 = Add v33, v23
    v35 = Add v34, v24
    v36 = Add v35, v25
    v37 = Add v36, v26
    Return v37
}
function f6 name="spins" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1() handler=b2():
    LoopHint
    v3 = Call v0 @60
    Jump -> next b1()
  b2() role=CatchHandler:
    v4 = CatchValue
    v5 = Call v1, -, v4 @65
    Return v5
}
function f7 name="cleansUp" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything) handler=b2():
    v2 = Call v0 @70
    Jump -> next b1(v2)
  b1(v3):
    v4 = String string="left normally"
    v5 = Call v1, -, v4 @75
    Return v3
  b2() role=CleanupHandler:
    v6 = CatchException
    v7 = String string="left by a throw"
    v8 = Call v1, -, v7 @80
    Rethrow v6
}
function f8 name="spinsWithoutCalls" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1() handler=b2():
    LoopHint
    Jump -> next b1()
  b2() role=CatchHandler:
    v2 = CatchValue
    v3 = Call v0, -, v2 @90
    Return v3
}
function f9 name="asyncCalls" flags=[IsAsync] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Await v0 @100
    v3 = Call v1 @105
    Return v3
}
function f10 name="catchesDirectCallee" results=[{type=anything}] {
  b0() handler=b2():
    v0 = CallDirect function=f3 @110
    Jump -> next b1(v0)
  b1(v1):
    Return v1
  b2() role=CatchHandler:
    v2 = CatchValue
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="describe" [isFreeName]
    v5 = String string="caught around a direct call: "
    v6 = Call v4, -, v5, v2
    Return v6
}
function f11 name="throwsNewError" results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=RangeError
    v1 = String string="made to be thrown"
    v2 = Construct v0, -, v1 @120
    Throw v2 @125
}
function f12 name="ignoresCallee" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything) handler=b2():
    v2 = Call v0, -, v1 @130
    Jump -> next b1(v2)
  b1(v3):
    Return v3
  b2() role=CatchHandler:
    v4 = String string="ignored what was thrown"
    Return v4
}
function f13 name="ignoresNamedCallee" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything) handler=b2():
    v1 = Call v0 calleeName="work" @140
    Jump -> next b1(v1)
  b1(v2):
    Return v2
  b2() role=CatchHandler:
    v3 = String string="ignored what was thrown"
    Return v3
}
function f14 name="catchesNamedCallee" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything) handler=b2():
    v1 = Call v0 calleeName="work" @150
    Jump -> next b1(v1)
  b1(v2):
    Return v2
  b2() role=CatchHandler:
    v3 = CatchValue
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="describe" [isFreeName]
    v6 = String string="caught in the same frame: "
    v7 = Call v5, -, v6, v3
    Return v7
}
function f15 name="throwsBelow" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer
    v2 = StrictEq v0, v1
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = Builtin builtin=RangeError
    v4 = String string="made far below"
    v5 = Construct v3, -, v4 @160
    Throw v5 @165
  b2():
    v6 = Integer integer=1
    v7 = Sub v0, v6
    v8 = CallDirect v7 function=f15 @170
    v9 = Add v8, v6
    Return v9
}
function f16 name="ignoresDirectCallees" results=[{type=anything}] {
  b0() handler=b2():
    v0 = Integer integer=3
    v1 = CallDirect v0 function=f15 @180
    Jump -> next b1(v1)
  b1(v2):
    Return v2
  b2() role=CatchHandler:
    v3 = String string="ignored what was thrown"
    Return v3
}
function f17 name="catchesDirectCallees" results=[{type=anything}] {
  b0() handler=b2():
    v0 = Integer integer=3
    v1 = CallDirect v0 function=f15 @190
    Jump -> next b1(v1)
  b1(v2):
    Return v2
  b2() role=CatchHandler:
    v3 = CatchValue
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="describe" [isFreeName]
    v6 = String string="caught around direct calls: "
    v7 = Call v5, -, v6, v3
    Return v7
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.registry = { };
globalThis.thrown = "the value";
globalThis.describe = function describe(label, value) { return label + (value instanceof Error ? value.constructor.name : value); };
globalThis.errorsNotCreated = function errorsNotCreated() { return Number(/(\d+)\tcounter:ThrownErrorsNotCreated/.exec(aotRuntimeCounters())[1]); };
globalThis.printWithErrorsNotCreated = function printWithErrorsNotCreated(work) {
    // The first throw at a site puts its frames into the call site cache, which is all that a later throw looks at.
    work();
    let before = errorsNotCreated();
    let result = work();
    print(result + "; errors not created: " + (errorsNotCreated() - before));
};
)js"_s;

enum class Validation : bool { Off, On };

static std::unique_ptr<CompileOutput> compile(ASCIILiteral referencesWord, Validation validation)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        if (validation == Validation::On)
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

static ChildProcessResult runInShell(ASCIILiteral referencesWord, ASCIILiteral statements, std::initializer_list<ASCIILiteral> shellOptions, Validation validation = Validation::On)
{
    std::unique_ptr<CompileOutput> output = compile(referencesWord, validation);
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()) };
    for (ASCIILiteral option : shellOptions)
        arguments.append(String(option));
    arguments.append(String("-e"_s));
    arguments.append(makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").then(async () => {"_s, statements, "}).catch(error => print(\"the script threw \" + error));"_s));
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    result.output = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
    return result;
}

static void checkShellOutput(ASCIILiteral statements, ASCIILiteral expectedOutput, Validation validation = Validation::On)
{
    for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
        ChildProcessResult result = runInShell(referencesWord, statements, { }, validation);
        if (!result.exitedWith(EXIT_SUCCESS) || result.output != expectedOutput)
            dataLog("with ", referencesWord, " the shell ", result.description(), " and printed\n", result.output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS) && result.output == expectedOutput);
    }
}

static void checkOutputOfTerminatedShell(ASCIILiteral statements, ASCIILiteral expectedOutput)
{
    for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
        ChildProcessResult result = runInShell(referencesWord, statements, { "--watchdog=300"_s });
        bool wasTerminated = result.outcome == ChildProcessOutcome::Exited && result.code != EXIT_SUCCESS;
        if (!wasTerminated || result.output != expectedOutput)
            dataLog("with ", referencesWord, " the shell ", result.description(), " and printed\n", result.output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(wasTerminated && result.output == expectedOutput);
    }
}

static void testHandlerInTheSameFrame()
{
    checkShellOutput(R"js(
    print(registry.catchesOwn(undefined));
    print(registry.catchesOwn({ get missing() { throw "thrown by a getter"; } }));
    print(registry.catchesOwn({ missing: "not thrown" }));
)js"_s, R"(caught in the same frame: TypeError
caught in the same frame: thrown by a getter
not thrown
)"_s);
}

static void testHandlerInACaller()
{
    checkShellOutput(R"js(
    print(registry.catchesCallee(registry.throws));
    print(registry.catchesDirectCallee());
    print(registry.catchesCallee(function returns() { return "not thrown"; }));
    print(registry.catchesCallee(function deep() { return registry.passes(function deeper() { return registry.passes(registry.throws); }); }));
    print(registry.catchesCallee(function interpretedThrow() { throw "from interpreted code"; }));
    print(registry.catchesCallee(function hostThrow() { return BigInt(1.5); }));
    print(registry.catchesCallee(function inner() { return registry.catchesCallee(registry.throws) + ", then returned"; }));
)js"_s, R"(caught in a caller: the value
caught around a direct call: the value
not thrown
caught in a caller: the value
caught in a caller: from interpreted code
caught in a caller: RangeError
caught in a caller: the value, then returned
)"_s);
}

static void testHandlerAcrossAnEntryFrame()
{
    checkShellOutput(R"js(
    print(registry.catchesCallee(function viaHost() { return JSON.stringify({ toJSON: registry.throws }); }));
    print(registry.catchesCallee(function viaHost() {
        return JSON.stringify({ toJSON() { return registry.passes(function again() { return JSON.stringify({ toJSON: registry.throws }); }); } });
    }));
)js"_s, R"(caught in a caller: the value
caught in a caller: the value
)"_s);
}

static void testHandlerInInterpretedCode()
{
    checkShellOutput(R"js(
    try {
        registry.passes(registry.throws);
    } catch (value) {
        print("caught in interpreted code: " + value);
    }
    try {
        registry.passes(function viaHost() { return JSON.stringify({ toJSON: registry.throws }); });
    } catch (value) {
        print("caught in interpreted code across an entry frame: " + value);
    }
    try {
        registry.passes(function between() { try { return registry.throws(); } finally { print("an interpreted finally block ran"); } });
    } catch (value) {
        print("caught below the finally block: " + value);
    }
)js"_s, R"(caught in interpreted code: the value
caught in interpreted code across an entry frame: the value
an interpreted finally block ran
caught below the finally block: the value
)"_s);
}

static void testCleanupHandlerRethrows()
{
    checkShellOutput(R"js(
    print(registry.catchesCallee(function cleaned() { return registry.cleansUp(registry.throws, print); }));
    print(registry.cleansUp(function returns() { return "returned"; }, print));
    try {
        registry.cleansUp(function nested() {
            return registry.cleansUp(registry.throws, function inner(text) { print("inner " + text); });
        }, function outer(text) { print("outer " + text); });
    } catch (value) {
        print("caught after both: " + value);
    }
    let error = new Error("kept");
    thrown = error;
    try {
        registry.cleansUp(registry.throws, print);
    } catch (value) {
        print("the same error: " + (value === error));
    }
)js"_s, R"(left by a throw
caught in a caller: the value
left normally
returned
inner left by a throw
outer left by a throw
caught after both: the value
left by a throw
the same error: true
)"_s);
}

static void testThrowInAsyncFunctionRejectsItsPromise()
{
    checkShellOutput(R"js(
    let promise = registry.asyncCalls(1, registry.throws);
    print("a promise came back: " + (promise instanceof Promise));
    try {
        await promise;
    } catch (value) {
        print("rejected with: " + value);
    }
    try {
        await registry.asyncCalls(Promise.reject("the awaited rejection"), registry.throws);
    } catch (value) {
        print("rejected with: " + value);
    }
    print("fulfilled with: " + await registry.asyncCalls(1, function returns() { return "a result"; }));
    print(registry.catchesCallee(function startsAsync() { registry.asyncCalls(1, registry.throws).catch(() => { }); return "nothing was thrown to the caller"; }));
)js"_s, R"(a promise came back: true
rejected with: the value
rejected with: the awaited rejection
fulfilled with: a result
nothing was thrown to the caller
)"_s);
}

static void testIgnoredErrorIsNotCreated()
{
    checkShellOutput(R"js(
    printWithErrorsNotCreated(() => registry.ignoresDirectCallees());
    printWithErrorsNotCreated(() => registry.ignoresCallee(registry.throwsNewError));
    printWithErrorsNotCreated(() => registry.ignoresCallee(registry.passes, registry.throwsNewError));
    printWithErrorsNotCreated(() => registry.ignoresNamedCallee(5));
    printWithErrorsNotCreated(() => registry.ignoresCallee(function returns() { return "not thrown"; }));
)js"_s, R"(ignored what was thrown; errors not created: 1
ignored what was thrown; errors not created: 1
ignored what was thrown; errors not created: 1
ignored what was thrown; errors not created: 0
not thrown; errors not created: 0
)"_s, Validation::Off);
}

static void testObservableErrorIsCreated()
{
    checkShellOutput(R"js(
    printWithErrorsNotCreated(() => registry.catchesDirectCallees());
    printWithErrorsNotCreated(() => registry.catchesCallee(registry.throwsNewError));
    printWithErrorsNotCreated(() => registry.catchesNamedCallee(5));
    printWithErrorsNotCreated(() => registry.ignoresCallee(registry.catchesCallee, registry.throwsNewError));
    printWithErrorsNotCreated(() => registry.ignoresCallee(function interpreted() { return registry.throwsNewError(); }));
    printWithErrorsNotCreated(() => registry.ignoresCallee(function viaHost() { return JSON.stringify({ toJSON: registry.throwsNewError }); }));
    printWithErrorsNotCreated(() => registry.ignoresCallee(function cleaned() { return registry.cleansUp(registry.throwsNewError, print); }));
    printWithErrorsNotCreated(() => {
        try {
            registry.throwsNewError();
        } catch (error) {
            return "caught in interpreted code: " + error.name + ": " + error.message + ", with a stack: " + (typeof error.stack);
        }
    });
    await registry.asyncCalls(1, registry.throwsNewError).catch(() => { });
    let before = errorsNotCreated();
    try {
        await registry.asyncCalls(1, registry.throwsNewError);
    } catch (error) {
        print("rejected with: " + error.name + ": " + error.message + "; errors not created: " + (errorsNotCreated() - before));
    }
)js"_s, R"(caught around direct calls: RangeError; errors not created: 0
caught in a caller: RangeError; errors not created: 0
caught in the same frame: TypeError; errors not created: 0
caught in a caller: RangeError; errors not created: 0
ignored what was thrown; errors not created: 0
ignored what was thrown; errors not created: 0
left by a throw
left by a throw
ignored what was thrown; errors not created: 0
caught in interpreted code: RangeError: made to be thrown, with a stack: string; errors not created: 0
rejected with: RangeError: made to be thrown; errors not created: 0
)"_s);
}

static void testValuesLiveAcrossTheCallAreRestored()
{
    checkShellOutput(R"js(
    print(registry.keepsValues(registry.throws, 100));
    print(registry.keepsValues(function deep() {
        return registry.passes(function deeper() { return registry.keepsValues(registry.throws, 5) + registry.throws(); });
    }, 1000));
    print(registry.keepsValues(function viaHost() { return JSON.stringify({ toJSON: registry.throws }); }, 10));
    print(registry.keepsValues(function returns() { return "not thrown"; }, 1));
    let a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    try {
        registry.passes(function deep() { return registry.keepsValues(registry.throws, 7) + registry.throws(); });
    } catch (value) {
        print(a + b + c + d + e + f);
    }
)js"_s, R"(1155
11055
165
not thrown
21
)"_s);
}

static void testTerminationPassesEveryHandler()
{
    checkOutputOfTerminatedShell(R"js(
    print("spinning");
    try {
        registry.catchesCallee(function outer() {
            return registry.cleansUp(function inner() {
                return registry.spins(function work() { }, function handler(value) { print("a compiled catch handler ran: " + value); });
            }, function cleanup(text) { print("a compiled cleanup handler ran: " + text); });
        });
        print("a compiled catch handler in a caller ran");
    } catch (error) {
        print("an interpreted handler ran: " + error);
    } finally {
        print("an interpreted finally block ran");
    }
    print("the script went on");
)js"_s, R"(spinning
)"_s);
}

static void testTerminationOfLoopWithoutCalls()
{
    checkOutputOfTerminatedShell(R"js(
    print("spinning");
    try {
        registry.spinsWithoutCalls(function handler(value) { print("a compiled catch handler ran: " + value); });
    } catch (error) {
        print("an interpreted handler ran: " + error);
    }
    print("the script went on");
)js"_s, R"(spinning
)"_s);
}

std::span<const NativeTest> unwindingTests()
{
    static constexpr NativeTest tests[] = {
        { "unwinding/handler-in-the-same-frame"_s, testHandlerInTheSameFrame },
        { "unwinding/handler-in-a-caller"_s, testHandlerInACaller },
        { "unwinding/handler-across-an-entry-frame"_s, testHandlerAcrossAnEntryFrame },
        { "unwinding/handler-in-interpreted-code"_s, testHandlerInInterpretedCode },
        { "unwinding/cleanup-handler-rethrows"_s, testCleanupHandlerRethrows },
        { "unwinding/throw-in-async-function-rejects-its-promise"_s, testThrowInAsyncFunctionRejectsItsPromise },
        { "unwinding/ignored-error-is-not-created"_s, testIgnoredErrorIsNotCreated },
        { "unwinding/observable-error-is-created"_s, testObservableErrorIsCreated },
        { "unwinding/values-live-across-the-call-are-restored"_s, testValuesLiveAcrossTheCallAreRestored },
        { "unwinding/termination-passes-every-handler"_s, testTerminationPassesEveryHandler },
        { "unwinding/termination-of-loop-without-calls"_s, testTerminationOfLoopWithoutCalls },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
