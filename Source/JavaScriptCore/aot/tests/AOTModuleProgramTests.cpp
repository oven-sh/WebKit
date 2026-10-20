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
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using OptionWords = std::initializer_list<ASCIILiteral>;

static constexpr ASCIILiteral mainThatRequires = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/function.cjs" displayPath="function.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f1
module m2 path="/filled.cjs" displayPath="filled.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f2
module m3 path="/chained.cjs" displayPath="chained.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f3
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = RequireModule module=m1 referrer=m0 specifier="./function.cjs"
    v3 = Call v2
    v4 = String string="module.exports = function, called"
    v5 = Call v1, -, v4, v3
    v6 = PropGet v2 key="tag"
    v7 = String string="a property of that function"
    v8 = Call v1, -, v7, v6
    v9 = RequireModule module=m2 referrer=m0 specifier="./filled.cjs"
    v10 = PropGet v9 key="a"
    v11 = String string="exports.a"
    v12 = Call v1, -, v11, v10
    v13 = PropGet v9 key="b"
    v14 = String string="this.b"
    v15 = Call v1, -, v14, v13
    v16 = RequireModule module=m3 referrer=m0 specifier="./chained.cjs"
    v17 = PropGet v16 key="sum"
    v18 = String string="exports.a of the other module + 1"
    v19 = Call v1, -, v18, v17
    v20 = RequireModule module=m3 referrer=m0 specifier="./chained.cjs"
    v21 = StrictEq v20, v16
    v22 = String string="a second require answers the same object"
    v23 = Call v1, -, v22, v21
    Return
}
)IR"_s;

static constexpr ASCIILiteral mainThatImports = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1}, {target=m2}, {target=m3}]
module m1 path="/function.cjs" displayPath="function.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f1 syntheticExports=[{name="default" variable=g0}, {name="tag" variable=g1}]
module m2 path="/filled.cjs" displayPath="filled.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f2 syntheticExports=[{name="a" variable=g2}, {name="default" variable=g3}]
module m3 path="/chained.cjs" displayPath="chained.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f3 syntheticExports=[{name="sum" variable=g4}]
entry module=m0
variable g0 name="default" module=m1 declared={type=anything}
variable g1 name="tag" module=m1 declared={type=anything}
variable g2 name="a" module=m2 declared={type=anything}
variable g3 name="default" module=m2 declared={type=anything}
variable g4 name="sum" module=m3 declared={type=anything}
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = GetModuleVariable variable=g0
    v3 = Call v2
    v4 = String string="the default import of module.exports = function, called"
    v5 = Call v1, -, v4, v3
    v6 = GetModuleVariable variable=g1
    v7 = String string="a property of that function, imported by name"
    v8 = Call v1, -, v7, v6
    v9 = GetModuleVariable variable=g2
    v10 = String string="exports.a, imported by name"
    v11 = Call v1, -, v10, v9
    v12 = GetModuleVariable variable=g3
    v13 = PropGet v12 key="a"
    v14 = String string="the default import of exports, .a"
    v15 = Call v1, -, v14, v13
    v16 = PropGet v12 key="b"
    v17 = String string=".b"
    v18 = Call v1, -, v17, v16
    v19 = GetModuleVariable variable=g4
    v20 = String string="exports.a of the other module + 1, imported by name"
    v21 = Call v1, -, v20, v19
    Return
}
)IR"_s;

static constexpr ASCIILiteral commonJSBodies = R"IR(function f1 name="functionModule" module=m1 kind=CommonJsModule flags=[IsSloppy, HasThis] parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Predict v3 predicted=builtin(ModuleObject)
    v7 = NewClosure function=f4
    v8 = String string="tagged"
    PropSet v7, v8 key="tag"
    PropSet v6, v7 key="exports"
    Return
}
function f2 name="filledModule" module=m2 kind=CommonJsModule flags=[IsSloppy, HasThis] parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Predict v1 predicted=builtin(ModuleObjectExports)
    v7 = Integer integer=41
    PropSet v6, v7 key="a"
    v9 = Integer integer=1
    PropSet v0, v9 key="b"
    Return
}
function f3 name="chainedModule" module=m3 kind=CommonJsModule flags=[IsSloppy, HasThis] parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="report" [isFreeName]
    v8 = RequireModule module=m2 referrer=m3 specifier="./filled.cjs"
    v9 = PropGet v8 key="a"
    v10 = Integer integer=1
    v11 = Add v9, v10
    PropSet v1, v11 key="sum"
    v13 = Predict v2 predicted=builtin(ModuleRequire)
    v14 = String string="/function.cjs"
    v15 = Call v13, -, v14
    v16 = Call v15
    v17 = String string="require called as a value"
    v18 = Call v7, -, v17, v16
    v19 = RequireModule module=m1 referrer=m3 specifier="./function.cjs"
    v20 = StrictEq v19, v15
    v21 = String string="answers what require(\"./function.cjs\") answers"
    v22 = Call v7, -, v21, v20
    v23 = PropGet v3 key="exports"
    v24 = StrictEq v23, v1
    v25 = String string="module.exports === exports"
    v26 = Call v7, -, v25, v24
    v27 = StrictEq v0, v1
    v28 = String string="this === exports"
    v29 = Call v7, -, v28, v27
    v30 = String string="__filename"
    v31 = Call v7, -, v30, v4
    v32 = String string="__dirname"
    v33 = Call v7, -, v32, v5
    v34 = TypeOf v2
    v35 = String string="typeof require"
    v36 = Call v7, -, v35, v34
    v37 = TypeOf v3
    v38 = String string="typeof module"
    v39 = Call v7, -, v38, v37
    v40 = ModuleBuiltin builtin=ModuleRequire module=m3
    v41 = StrictEq v2, v40
    v42 = String string="require is the one of this module"
    v43 = Call v7, -, v42, v41
    v44 = ModuleBuiltin builtin=ModuleObject module=m3
    v45 = StrictEq v3, v44
    v46 = String string="module is the one of this module"
    v47 = Call v7, -, v46, v45
    Return
}
function f4 name="exported" module=m1 results=[{type=anything}] {
  b0():
    v0 = String string="called"
    Return v0
}
)IR"_s;

static constexpr ASCIILiteral commonJSEntryPoint = R"IR(module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f0
module m1 path="/library.cjs" displayPath="library.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f1
entry module=m0
function f0 name="main" kind=CommonJsModule flags=[IsSloppy, HasThis] parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="report" [isFreeName]
    v8 = RequireModule module=m1 referrer=m0 specifier="./library.cjs"
    v9 = PropGet v8 key="a"
    v10 = String string="exports.a of the required module"
    v11 = Call v7, -, v10, v9
    v12 = String string="__filename"
    v13 = Call v7, -, v12, v4
    v14 = StrictEq v0, v1
    v15 = String string="this === exports"
    v16 = Call v7, -, v15, v14
    v17 = PropGet v3 key="exports"
    v18 = StrictEq v17, v1
    v19 = String string="module.exports === exports"
    v20 = Call v7, -, v19, v18
    Return
}
function f1 name="library" module=m1 kind=CommonJsModule flags=[IsSloppy, HasThis] parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Integer integer=41
    PropSet v1, v6 key="a"
    Return
}
)IR"_s;

static constexpr ASCIILiteral importMetaKept = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1}]
module m1 path="/other.js" displayPath="other.js" body=f2 exports=[{name="otherMeta" variable=g1}]
entry module=m0
variable g0 name="meta" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="otherMeta" module=m1 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    PutModuleVariable v2 variable=g0
    v4 = NewClosure function=f1
    v5 = PropGet v0 key="register" [isFreeName]
    v6 = Call v5, -, v4
    v7 = ModuleBuiltin builtin=ImportMeta module=m0
    v8 = StrictEq v2, v7
    v9 = String string="import.meta === import.meta"
    v10 = Call v1, -, v9, v8
    v11 = GetModuleVariable variable=g0
    v12 = StrictEq v11, v7
    v13 = String string="the variable === import.meta"
    v14 = Call v1, -, v13, v12
    v15 = TypeOf v11
    v16 = String string="typeof the variable"
    v17 = Call v1, -, v16, v15
    v18 = GetModuleVariable variable=g1
    v19 = StrictEq v11, v18
    v20 = String string="the variable === import.meta of another module"
    v21 = Call v1, -, v20, v19
    v22 = TypeOf v18
    v23 = String string="typeof that one"
    v24 = Call v1, -, v23, v22
    Return
}
function f1 name="comparesMeta" results=[{type=anything}] {
  b0():
    v0 = GetModuleVariable variable=g0
    v1 = ModuleBuiltin builtin=ImportMeta module=m0
    v2 = StrictEq v0, v1
    Return v2
}
function f2 name="other" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = ModuleBuiltin builtin=ImportMeta module=m1
    PutModuleVariable v0 variable=g1
    Return
}
)IR"_s;

static constexpr ASCIILiteral importMetaPassedOn = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    v3 = PropGet v0 key="id" [isFreeName]
    v4 = Call v3, -, v2
    v5 = StrictEq v4, v2
    v6 = String string="id(import.meta) === import.meta"
    v7 = Call v1, -, v6, v5
    Return
}
)IR"_s;

static constexpr ASCIILiteral moduleBuiltinsInEveryPosition = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/library.cjs" displayPath="library.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f1
entry module=m0
layout L0 name="Holder" fields=[{key="meta" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Wrapper" fields=[{key="exports" declared={type=anything} flags=[IsRequired]}, {key="require" declared={type=anything} flags=[IsRequired]}, {key="module" declared={type=anything} flags=[IsRequired]}]
variable g0 name="meta" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="holder" module=m0 declared={type=anything} initialization=Undefined
variable g2 name="callback" module=m0 declared={type=anything} initialization=Undefined
variable g3 name="received" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    PutModuleVariable v2 variable=g0
    v4 = NewObject -, v2 layout=L0 fields=[0]
    v5 = FinishObject v4
    PutModuleVariable v5 variable=g1
    v7 = PropGet v0 key="input" [isFreeName]
    v8 = ToBoolean v7
    Branch v8 -> taken b1(), notTaken b2()
  b1():
    v9 = NewClosure function=f2
    PutModuleVariable v9 variable=g2
    Jump -> next b3()
  b2():
    v11 = NewClosure function=f3
    PutModuleVariable v11 variable=g2
    Jump -> next b3()
  b3():
    v13 = GetModuleVariable variable=g2
    v14 = Call v13, -, v2
    v15 = String string="which of two callbacks was called by value"
    v16 = Call v1, -, v15, v14
    v17 = GetModuleVariable variable=g3
    v18 = StrictEq v17, v2
    v19 = String string="its parameter was import.meta"
    v20 = Call v1, -, v19, v18
    v21 = GetModuleVariable variable=g1
    v22 = PropGet v21 key="meta"
    v23 = StrictEq v22, v2
    v24 = String string="the field holds import.meta"
    v25 = Call v1, -, v24, v23
    v26 = NewClosure function=f4
    v27 = PropGet v0 key="register" [isFreeName]
    v28 = Call v27, -, v26
    v29 = RequireModule module=m1 referrer=m0 specifier="./library.cjs"
    v30 = Call v29
    v31 = String string="typeof exports, require, module, module.exports, read from fields"
    v32 = Call v1, -, v31, v30
    Return
}
function f1 name="library" module=m1 kind=CommonJsModule flags=[IsSloppy, HasThis] parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = NewObject -, v1, v2, v3 layout=L1 fields=[0, 1, 2]
    v7 = FinishObject v6
    v8 = NewClosure v7 function=f5
    PropSet v3, v8 key="exports"
    Return
}
function f2 name="first" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    PutModuleVariable v0 variable=g3
    v2 = Integer integer=1
    Return v2
}
function f3 name="second" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    PutModuleVariable v0 variable=g3
    v2 = Integer integer=2
    Return v2
}
function f4 name="comparesField" results=[{type=anything}] {
  b0():
    v0 = GetModuleVariable variable=g1
    v1 = PropGet v0 key="meta"
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    v3 = StrictEq v1, v2
    Return v3
}
function f5 name="describesWrapper" module=m1 captures=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="exports"
    v2 = PropGet v0 key="require"
    v3 = PropGet v0 key="module"
    v4 = TypeOf v1
    v5 = TypeOf v2
    v6 = TypeOf v3
    v7 = String string=" "
    v8 = Add v4, v7
    v9 = Add v8, v5
    v10 = Add v9, v7
    v11 = Add v10, v6
    v12 = Add v11, v7
    v13 = PropGet v3 key="exports"
    v14 = TypeOf v13
    v15 = Add v12, v14
    Return v15
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.report = (label, value) => print(label + ": " + value);
globalThis.id = value => value;
globalThis.input = 1;
globalThis.register = closure => { globalThis.registered = closure; };
)js"_s;

static constexpr ASCIILiteral scriptEnd = R"js(.then(() => {
    if (globalThis.registered)
        report("the closure, called later", registered());
}).catch(error => print("the script threw " + error));
)js"_s;

static std::unique_ptr<CompileOutput> compileOnThisThread(const String& programText, ASCIILiteral referencesWord, OptionWords optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    AOT_TEST_CHECK(!applyOptionWord(options, referencesWord));
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(programText), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty() && output->entryModuleIDs.size() == 1);
    return output;
}

static String runInShell(const String& programText, ASCIILiteral referencesWord, OptionWords optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(programText, referencesWord, optionWords);
    });
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ')', scriptEnd),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkShellOutput(const String& programText, OptionWords optionWords, ASCIILiteral expectedOutput)
{
    for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
        String output = runInShell(programText, referencesWord, optionWords);
        if (output != expectedOutput)
            dataLog("with ", referencesWord, " the shell printed\n", output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(output == expectedOutput);
    }
}

static void testRequiredCommonJSModules()
{
    checkShellOutput(makeString(mainThatRequires, commonJSBodies), { "validate"_s }, R"(module.exports = function, called: called
a property of that function: tagged
exports.a: 41
this.b: 1
require called as a value: called
answers what require("./function.cjs") answers: true
module.exports === exports: true
this === exports: true
__filename: /chained.cjs
__dirname: /
typeof require: function
typeof module: object
require is the one of this module: true
module is the one of this module: true
exports.a of the other module + 1: 42
a second require answers the same object: true
)"_s);
}

static void testImportedCommonJSModules()
{
    // The loader reads the exports under every name, so the image would list members of the host's classes as constants, and the realm of the shell has no such class.
    checkShellOutput(makeString(mainThatImports, commonJSBodies), { "validate"_s, "disable=flow:ConstantBuiltinLocations"_s }, R"(require called as a value: called
answers what require("./function.cjs") answers: true
module.exports === exports: true
this === exports: true
__filename: /chained.cjs
__dirname: /
typeof require: function
typeof module: object
require is the one of this module: true
module is the one of this module: true
the default import of module.exports = function, called: called
a property of that function, imported by name: tagged
exports.a, imported by name: 41
the default import of exports, .a: 41
.b: 1
exports.a of the other module + 1, imported by name: 42
)"_s);
}

static void testCommonJSEntryPoint()
{
    checkShellOutput(commonJSEntryPoint, { "validate"_s }, R"(exports.a of the required module: 41
__filename: /main.cjs
this === exports: true
module.exports === exports: true
)"_s);
}

static void testImportMetaKept()
{
    checkShellOutput(importMetaKept, { "validate"_s }, R"(import.meta === import.meta: true
the variable === import.meta: true
typeof the variable: object
the variable === import.meta of another module: false
typeof that one: object
the closure, called later: true
)"_s);
}

static void testImportMetaPassedOn()
{
    // Whoever holds import.meta can load any module of the host, and validation looks for all of them in the realm.
    checkShellOutput(importMetaPassedOn, { }, R"(id(import.meta) === import.meta: true
)"_s);
}

static void testModuleBuiltinsInEveryPosition()
{
    checkShellOutput(moduleBuiltinsInEveryPosition, { "validate"_s }, R"(which of two callbacks was called by value: 1
its parameter was import.meta: true
the field holds import.meta: true
typeof exports, require, module, module.exports, read from fields: object function object function
the closure, called later: true
)"_s);
}

std::span<const NativeTest> moduleProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "module-programs/required-commonjs-modules"_s, testRequiredCommonJSModules },
        { "module-programs/imported-commonjs-modules"_s, testImportedCommonJSModules },
        { "module-programs/commonjs-entry-point"_s, testCommonJSEntryPoint },
        { "module-programs/import-meta-kept"_s, testImportMetaKept },
        { "module-programs/import-meta-passed-on"_s, testImportMetaPassedOn },
        { "module-programs/module-builtins-in-every-position"_s, testModuleBuiltinsInEveryPosition },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
