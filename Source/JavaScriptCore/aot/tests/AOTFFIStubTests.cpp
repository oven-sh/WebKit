/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTVector.h"
#include "FFIStubs.h"

#if FFI_STUBS_SUPPORTED

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTFFIStubs.h"
#include "AOTIRParser.h"
#include "AOTIRProgram.h"
#include "AOTImageFormat.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include <algorithm>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
  b0():
    Return
}
)IR"_s;

constexpr ASCIILiteral scriptStart = R"js(
const fixture = name => $vm.ffiFixture(name);
const bind = (name, args, ret) => $vm.ffiFunction({ args, returns: ret }, fixture(name), name);
const callback = (args, ret, fn) => $vm.ffiCallback({ args, returns: ret }, fn);
)js"_s;

constexpr ASCIILiteral callsAndCallbacks = R"js(
function weighted(...values) {
    let sum = 0;
    for (let i = 0; i < values.length; ++i)
        sum += (i + 1) * Number(values[i]);
    return sum;
}
function repeated(type, count) {
    let types = [];
    for (let i = 0; i < count; ++i)
        types.push(type);
    return types;
}
function withFunction(types) {
    let result = ["function"];
    for (let i = 0; i < types.length; ++i)
        result.push(types[i]);
    return result;
}

print(bind("ffi_add_i32", ["i32", "i32"], "i32")(40, 2));
print(bind("ffi_add_i64", ["i64", "i64"], "i64")(2n ** 40n, 2n));
print(bind("ffi_sum_f64_9", repeated("f64", 9), "f64")(0.5, 1, 2, 4, 8, 16, 32, 64, 128));

const callCbI32 = bind("ffi_call_cb_i32", ["function", "i32"], "i32");
const timesThreePlusOne = callback(["i32"], "i32", x => x * 3 + 1);
print(callCbI32(timesThreePlusOne, 7) + " " + callCbI32(timesThreePlusOne, -1) + " " + callCbI32(timesThreePlusOne, 0) + " " + callCbI32(timesThreePlusOne, 2147483647));
const i32x9 = callback(repeated("i32", 9), "i64", weighted);
print(bind("ffi_call_cb_i32_x9", withFunction(repeated("i32", 9)), "i64")(i32x9, 1, -2, 3, -4, 5, -6, 7, -8, 2147483647));
const f64x9 = callback(repeated("f64", 9), "f64", weighted);
print(bind("ffi_call_cb_f64_x9", withFunction(repeated("f64", 9)), "f64")(f64x9, 0.5, -0.25, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 1e10));
const u8x10 = callback(repeated("u8", 10), "i64", weighted);
print(bind("ffi_call_cb_u8_x10", withFunction(repeated("u8", 10)), "i64")(u8x10, 255, 0, 128, 1, 200, 17, 254, 3, 99, 250));
const mix = callback(["i32", "f64", "i64_fast", "f32", "ptr"], "f64", weighted);
print(bind("ffi_call_cb_mix", ["function", "i32", "f64", "i64", "f32", "ptr"], "f64")(mix, -5, 2.5, 4503599627370496, 1.25, 8192));

const throws = callback(["i32"], "i32", x => { throw new RangeError("thrown by the callback for " + x); });
try {
    print("returned " + callCbI32(throws, 13));
} catch (error) {
    print(error.constructor.name + ": " + error.message);
}
print(callCbI32(timesThreePlusOne, 1));

timesThreePlusOne.close();
i32x9.close();
f64x9.close();
u8x10.close();
mix.close();
throws.close();

let open = [];
let failure = "none";
try {
    while (open.length < 100000)
        open.push(callback(["i32"], "i32", x => x + open.length));
} catch (error) {
    failure = error.constructor.name;
}
print(open.length + " " + failure);
print(callCbI32(open[0], 1) + " " + callCbI32(open[255], 2));
open[100].close();
open[100] = callback(["i32"], "i32", x => x - 1);
print(callCbI32(open[100], 10) + " " + callCbI32(open[99], 3) + " " + callCbI32(open[101], 4));
try {
    callback(["i32"], "i32", x => x);
    print("another one was opened");
} catch (error) {
    print(error.constructor.name);
}
for (let i = 0; i < open.length; ++i)
    open[i].close();

let sum = 0;
for (let i = 0; i < 1000; ++i) {
    let once = callback(["i32"], "i32", x => x + i);
    sum += callCbI32(once, 1);
    once.close();
}
print(sum);
)js"_s;

constexpr ASCIILiteral outputOfCallsAndCallbacks = R"(42
1099511627778
255.5
22 -2 1 2147483646
19327352787
90000000215.5
6938
13510798882152452
RangeError: thrown by the callback for 13
4
256 RangeError
257 258
9 259 260
RangeError
500500
)"_s;

constexpr ASCIILiteral firstUses = R"js(
try {
    print("returned " + bind("ffi_add_i32", ["i32", "i32"], "i32")(40, 2));
} catch (error) {
    print(error.constructor.name + ": " + error.message);
}
try {
    callback(["i32"], "i32", x => x);
    print("a callback was opened");
} catch (error) {
    print(error.constructor.name + ": " + error.message);
}
)js"_s;

} // anonymous namespace

static Wire::CompileOptions optionsWith(std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    return options;
}

static FFIStubs generateWith(std::initializer_list<ASCIILiteral> optionWords)
{
    BuildContext context(optionsWith(optionWords));
    FFIStubs stubs = generateFFIStubs(context);
    context.diagnostics().finish(nullptr);
    AOT_TEST_CHECK(context.diagnostics().userDiagnostics().isEmpty() && context.diagnostics().internalErrors().isEmpty());
    return stubs;
}

static const FFI::StubDirectory& directoryOf(const FFIStubs& stubs)
{
    AOT_TEST_CHECK(stubs.code.size() >= sizeof(FFI::StubDirectory));
    return *reinterpret_cast<const FFI::StubDirectory*>(stubs.code.span().data());
}

static std::unique_ptr<CompileOutput> compileEmptyProgram(std::initializer_list<ASCIILiteral> optionWords)
{
    BuildContext build(optionsWith(optionWords));
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);
    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty());
    return output;
}

static const ImageHeader& headerOf(const CompileOutput& output)
{
    AOT_TEST_CHECK(output.image.size() >= sizeof(ImageHeader));
    return *reinterpret_cast<const ImageHeader*>(output.image.span().data());
}

static void checkLayout(std::initializer_list<ASCIILiteral> optionWords, unsigned expectedNumberOfEntries)
{
    BuildContext context(optionsWith(optionWords));
    AOT_TEST_CHECK(context.limit(LimitID::ForeignCallbackStubs) == expectedNumberOfEntries);
    FFIStubs stubs = generateWith(optionWords);
    const FFI::StubDirectory& directory = directoryOf(stubs);

    AOT_TEST_CHECK(stubs.codeAlignment == imagePageSize);
    AOT_TEST_CHECK(!stubs.code.isEmpty() && !(stubs.code.size() % imagePageSize));
    AOT_TEST_CHECK(directory.numberOfCallbackEntries == expectedNumberOfEntries);
    AOT_TEST_CHECK(directory.callbackContextSize == imagePageSize);
    AOT_TEST_CHECK(!(directory.callbackContext % imagePageSize));
    AOT_TEST_CHECK(directory.callbackContext + directory.callbackContextSize == stubs.code.size());
    AOT_TEST_CHECK(stubs.callbackContextOffset == directory.callbackContext && stubs.callbackContextSize == directory.callbackContextSize);
    AOT_TEST_CHECK(directory.invoke >= FFI::StubDirectory::sizeWithEntries(expectedNumberOfEntries) && !(directory.invoke % 16));

    std::span<const uint32_t> entries = directory.callbackEntries();
    AOT_TEST_CHECK(entries.size() == expectedNumberOfEntries && directory.invoke < entries[0]);
    for (size_t index = 1; index < entries.size(); ++index)
        AOT_TEST_CHECK(entries[index - 1] < entries[index]);
    AOT_TEST_CHECK(entries.back() < directory.callbackContext);
    AOT_TEST_CHECK(std::ranges::all_of(stubs.code.span().subspan(directory.callbackContext), [](uint8_t byte) { return !byte; }));
    AOT_TEST_CHECK(std::ranges::any_of(stubs.code.span().subspan(directory.invoke, entries[0] - directory.invoke), [](uint8_t byte) { return !!byte; }));
}

static void testLayout()
{
    checkLayout({ }, 256);
    checkLayout({ "limit=ForeignCallbackStubs:4"_s }, 4);
    checkLayout({ "limit=ForeignCallbackStubs:1"_s }, 1);
}

static void testSameBytesEveryTime()
{
    FFIStubs first = generateWith({ });
    AOT_TEST_CHECK(generateWith({ }).code == first.code);
    AOT_TEST_CHECK(generateWith({ "threads=1"_s }).code == first.code);
    AOT_TEST_CHECK(generateWith({ "limit=ForeignCallbackStubs:4"_s }).code != first.code);
}

static void testClosedProgramHasNone()
{
    std::unique_ptr<CompileOutput> output = compileEmptyProgram({ });
    AOT_TEST_CHECK(!headerOf(*output).code.ffiCodeSize);
}

static void testOpenProgramHasStubs()
{
    std::unique_ptr<CompileOutput> output = compileEmptyProgram({ "admit=Inspector"_s });
    const ImageHeader& header = headerOf(*output);
    FFIStubs stubs = generateWith({ "admit=Inspector"_s });
    AOT_TEST_CHECK(header.code.ffiCodeSize == stubs.code.size());
    AOT_TEST_CHECK(!(header.code.ffiCodeOffset % imagePageSize));
    AOT_TEST_CHECK(static_cast<uint64_t>(header.code.ffiCodeOffset) + header.code.ffiCodeSize <= header.codeSize);
    AOT_TEST_CHECK(header.codeOffset + header.codeSize <= output->image.size());
    AOT_TEST_CHECK(equalSpans(output->image.span().subspan(header.codeOffset + header.code.ffiCodeOffset, header.code.ffiCodeSize), stubs.code.span()));
}

static String runInShell(const CompileOutput& output, ASCIILiteral statements)
{
    TemporaryFile image(".image"_s, output.image.span());
    String source = makeString(scriptStart, statements);
    TemporaryFile script(".js"_s, byteCast<uint8_t>(source.span8()));
    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), "--useJIT=false"_s, "--useDollarVM=1"_s, script.path() };
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shellPath(), " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void testShellCallsAndCallbacks()
{
    String printed = runInShell(*compileEmptyProgram({ "admit=Inspector"_s }), callsAndCallbacks);
    if (printed != outputOfCallsAndCallbacks)
        dataLog("the shell printed\n", printed, "expected\n", outputOfCallsAndCallbacks);
    AOT_TEST_CHECK(printed == outputOfCallsAndCallbacks);
}

static void testShellWithoutStubs()
{
    String printed = runInShell(*compileEmptyProgram({ }), firstUses);
    Vector<String> lines = printed.split('\n');
    if (lines.size() != 2)
        dataLog("the shell printed\n", printed);
    AOT_TEST_CHECK(lines.size() == 2);
    for (const String& line : lines)
        AOT_TEST_CHECK(line.startsWith("TypeError: "_s) && line.contains("bun:ffi requires the JIT"_s));

    AOT_TEST_CHECK(runInShell(*compileEmptyProgram({ "admit=Inspector"_s }), firstUses) == "returned 42\na callback was opened\n"_s);
}

std::span<const NativeTest> ffiStubTests()
{
    static constexpr NativeTest tests[] = {
        { "ffi-stubs/layout"_s, testLayout },
        { "ffi-stubs/same-bytes-every-time"_s, testSameBytesEveryTime },
        { "ffi-stubs/closed-program-has-none"_s, testClosedProgramHasNone },
        { "ffi-stubs/open-program-has-stubs"_s, testOpenProgramHasStubs },
        { "ffi-stubs/shell-calls-and-callbacks"_s, testShellCallsAndCallbacks },
        { "ffi-stubs/shell-without-stubs"_s, testShellWithoutStubs },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#else

namespace JSC { namespace AOT {

std::span<const NativeTest> ffiStubTests()
{
    return { };
}

} } // namespace JSC::AOT

#endif // FFI_STUBS_SUPPORTED

#endif // ENABLE(AOT)
