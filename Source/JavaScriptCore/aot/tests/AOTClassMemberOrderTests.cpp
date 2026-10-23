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
#include "AOTVector.h"
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Descending" module=m0 constructor=f1
layout L1 kind=Instance name="Ascending" module=m0 constructor=f9
layout L2 kind=Instance name="ComputedInTheMiddle" module=m0 constructor=f14
layout L3 kind=Instance name="MethodThenGetter" module=m0 constructor=f20
layout L4 kind=Instance name="GetterThenMethod" module=m0 constructor=f25
layout L5 kind=Instance name="MethodTwice" module=m0 constructor=f30
layout L6 kind=Instance name="StaticName" module=m0 constructor=f34
layout L7 kind=Instance name="Patched" module=m0 constructor=f38
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = PropGet v0 key="showValue" [isFreeName]
    v3 = PropGet v0 key="key" [isFreeName]
    v4 = ToPropertyKey v3
    v5 = NewClosure function=f1
    v6 = NewClass v5 layout=L0
    v7 = NewClosure function=f2
    DefineMethod v6, v7 layout=L0 key="zebra"
    v9 = NewClosure function=f3
    DefineMethod v6, v9 layout=L0 index=1 key="yak"
    v11 = NewClosure function=f4
    DefineMethod v6, v11 layout=L0 side=Static index=2 key="walrus"
    v13 = NewClosure function=f5
    DefineMethod v6, v13 layout=L0 methodKind=Getter index=3 key="viper"
    v15 = NewClosure function=f6
    DefineMethod v6, v15 layout=L0 methodKind=Setter index=4 key="viper"
    v17 = NewClosure function=f7
    DefineMethod v6, v17 layout=L0 index=5 key="ant"
    v19 = NewClosure function=f8
    DefineMethod v6, v19 layout=L0 side=Static index=6 key="bee"
    v21 = FinishClass v6 layout=L0
    v22 = NewClosure function=f9
    v23 = NewClass v22 layout=L1
    v24 = NewClosure function=f10
    DefineMethod v23, v24 layout=L1 key="ant"
    v26 = NewClosure function=f11
    DefineMethod v23, v26 layout=L1 index=1 key="zebra"
    v28 = NewClosure function=f12
    DefineMethod v23, v28 layout=L1 side=Static index=2 key="bee"
    v30 = NewClosure function=f13
    DefineMethod v23, v30 layout=L1 side=Static index=3 key="walrus"
    v32 = FinishClass v23 layout=L1
    v33 = NewClosure function=f14
    v34 = NewClass v33 layout=L2
    v35 = NewClosure function=f15
    DefineMethod v34, v35 layout=L2 key="zebra"
    v37 = NewClosure function=f16
    DefineMethod v34, v37, v4 layout=L2 index=1
    v39 = NewClosure function=f17
    DefineMethod v34, v39 layout=L2 index=2 key="ant"
    v41 = NewClosure function=f18
    DefineMethod v34, v41 layout=L2 side=Static index=3 key="zebra"
    v43 = NewClosure function=f19
    DefineMethod v34, v43 layout=L2 side=Static index=4 key="ant"
    v45 = FinishClass v34 layout=L2
    v46 = NewClosure function=f20
    v47 = NewClass v46 layout=L3
    v48 = NewClosure function=f21
    DefineMethod v47, v48 layout=L3 key="first"
    v50 = NewClosure function=f22
    DefineMethod v47, v50 layout=L3 index=1 key="member"
    v52 = NewClosure function=f23
    DefineMethod v47, v52 layout=L3 index=2 key="last"
    v54 = NewClosure function=f24
    DefineMethod v47, v54 layout=L3 methodKind=Getter index=3 key="member"
    v56 = FinishClass v47 layout=L3
    v57 = NewClosure function=f25
    v58 = NewClass v57 layout=L4
    v59 = NewClosure function=f26
    DefineMethod v58, v59 layout=L4 key="first"
    v61 = NewClosure function=f27
    DefineMethod v58, v61 layout=L4 methodKind=Getter index=1 key="member"
    v63 = NewClosure function=f28
    DefineMethod v58, v63 layout=L4 index=2 key="last"
    v65 = NewClosure function=f29
    DefineMethod v58, v65 layout=L4 index=3 key="member"
    v67 = FinishClass v58 layout=L4
    v68 = NewClosure function=f30
    v69 = NewClass v68 layout=L5
    v70 = NewClosure function=f31
    DefineMethod v69, v70 layout=L5 key="member"
    v72 = NewClosure function=f32
    DefineMethod v69, v72 layout=L5 index=1 key="other"
    v74 = NewClosure function=f33
    DefineMethod v69, v74 layout=L5 index=2 key="member"
    v76 = FinishClass v69 layout=L5
    v77 = NewClosure function=f34
    v78 = NewClass v77 layout=L6
    v79 = NewClosure function=f35
    DefineMethod v78, v79 layout=L6 side=Static key="zebra"
    v81 = NewClosure function=f36
    DefineMethod v78, v81 layout=L6 side=Static index=1 key="name"
    v83 = NewClosure function=f37
    DefineMethod v78, v83 layout=L6 side=Static index=2 key="ant"
    v85 = FinishClass v78 layout=L6
    v86 = NewClosure function=f38
    v87 = NewClass v86 layout=L7
    v88 = NewClosure function=f39
    DefineMethod v87, v88 layout=L7 key="zebra"
    v90 = NewClosure function=f40
    DefineMethod v87, v90 layout=L7 index=1 key="ant"
    v92 = FinishClass v87 layout=L7
    v93 = PropGet v21 key="prototype"
    v94 = String string="Descending.prototype"
    v95 = Call v1, -, v94, v93
    v96 = String string="Descending"
    v97 = Call v1, -, v96, v21
    v98 = PropGet v32 key="prototype"
    v99 = String string="Ascending.prototype"
    v100 = Call v1, -, v99, v98
    v101 = String string="Ascending"
    v102 = Call v1, -, v101, v32
    v103 = PropGet v45 key="prototype"
    v104 = String string="ComputedInTheMiddle.prototype"
    v105 = Call v1, -, v104, v103
    v106 = String string="ComputedInTheMiddle"
    v107 = Call v1, -, v106, v45
    v108 = PropGet v56 key="prototype"
    v109 = String string="MethodThenGetter.prototype"
    v110 = Call v1, -, v109, v108
    v111 = String string="MethodThenGetter"
    v112 = Call v1, -, v111, v56
    v113 = PropGet v67 key="prototype"
    v114 = String string="GetterThenMethod.prototype"
    v115 = Call v1, -, v114, v113
    v116 = String string="GetterThenMethod"
    v117 = Call v1, -, v116, v67
    v118 = PropGet v76 key="prototype"
    v119 = String string="MethodTwice.prototype"
    v120 = Call v1, -, v119, v118
    v121 = String string="MethodTwice"
    v122 = Call v1, -, v121, v76
    v123 = PropGet v85 key="prototype"
    v124 = String string="StaticName.prototype"
    v125 = Call v1, -, v124, v123
    v126 = String string="StaticName"
    v127 = Call v1, -, v126, v85
    v128 = PropGet v92 key="prototype"
    v129 = String string="Patched.prototype"
    v130 = Call v1, -, v129, v128
    v131 = String string="Patched"
    v132 = Call v1, -, v131, v92
    v133 = Construct v56
    v134 = PropGet v133 key="member"
    v135 = String string="method then getter"
    v136 = Call v2, -, v135, v134
    v137 = Construct v67
    v138 = PropGet v137 key="member"
    v139 = Call v138, v137 calleeKey="member"
    v140 = String string="getter then method"
    v141 = Call v2, -, v140, v139
    v142 = Construct v76
    v143 = PropGet v142 key="member"
    v144 = Call v143, v142 calleeKey="member"
    v145 = String string="method twice"
    v146 = Call v2, -, v145, v144
    v147 = Construct v45
    v148 = PropGet v147 key="computed"
    v149 = Call v148, v147 calleeKey="computed"
    v150 = String string="computed"
    v151 = Call v2, -, v150, v149
    v152 = PropGet v85 key="name"
    v153 = Call v152, v85 calleeKey="name"
    v154 = String string="static name"
    v155 = Call v2, -, v154, v153
    v156 = Construct v92
    v157 = CallDirect v156 function=f41
    v158 = String string="before the stores"
    v159 = Call v2, -, v158, v157
    v160 = PropGet v92 key="prototype"
    v161 = NewClosure function=f42
    PropSet v160, v161 key="extra" mode=Strict
    PropSet v160, v161 key="zebra" mode=Strict
    v164 = CallDirect v156 function=f41
    v165 = String string="after the stores"
    v166 = Call v2, -, v165, v164
    v167 = String string="Patched.prototype after the stores"
    v168 = Call v1, -, v167, v160
    Return
}
function f1 name="Descending" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="zebra" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Descending prototype zebra 0"
    Return v1
}
function f3 name="yak" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Descending prototype yak 1"
    Return v1
}
function f4 name="walrus" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Descending static walrus 2"
    Return v1
}
function f5 name="viper" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Descending prototype viper 3"
    Return v1
}
function f6 name="viper" kind=Setter flags=[HasThis] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = String string="Descending prototype viper 4"
    Return v2
}
function f7 name="ant" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Descending prototype ant 5"
    Return v1
}
function f8 name="bee" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Descending static bee 6"
    Return v1
}
function f9 name="Ascending" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f10 name="ant" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Ascending prototype ant 0"
    Return v1
}
function f11 name="zebra" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Ascending prototype zebra 1"
    Return v1
}
function f12 name="bee" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Ascending static bee 2"
    Return v1
}
function f13 name="walrus" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Ascending static walrus 3"
    Return v1
}
function f14 name="ComputedInTheMiddle" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    Return v2
}
function f15 name="zebra" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="ComputedInTheMiddle prototype zebra 0"
    Return v1
}
function f16 name="computed" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="ComputedInTheMiddle prototype computed 1"
    Return v1
}
function f17 name="ant" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="ComputedInTheMiddle prototype ant 2"
    Return v1
}
function f18 name="zebra" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="ComputedInTheMiddle static zebra 3"
    Return v1
}
function f19 name="ant" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="ComputedInTheMiddle static ant 4"
    Return v1
}
function f20 name="MethodThenGetter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L3
    v2 = NewObject v1 layout=L3
    Return v2
}
function f21 name="first" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodThenGetter prototype first 0"
    Return v1
}
function f22 name="member" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodThenGetter prototype member 1"
    Return v1
}
function f23 name="last" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodThenGetter prototype last 2"
    Return v1
}
function f24 name="member" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodThenGetter prototype member 3"
    Return v1
}
function f25 name="GetterThenMethod" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L4
    v2 = NewObject v1 layout=L4
    Return v2
}
function f26 name="first" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="GetterThenMethod prototype first 0"
    Return v1
}
function f27 name="member" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="GetterThenMethod prototype member 1"
    Return v1
}
function f28 name="last" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="GetterThenMethod prototype last 2"
    Return v1
}
function f29 name="member" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="GetterThenMethod prototype member 3"
    Return v1
}
function f30 name="MethodTwice" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L5
    v2 = NewObject v1 layout=L5
    Return v2
}
function f31 name="member" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodTwice prototype member 0"
    Return v1
}
function f32 name="other" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodTwice prototype other 1"
    Return v1
}
function f33 name="member" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="MethodTwice prototype member 2"
    Return v1
}
function f34 name="StaticName" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L6
    v2 = NewObject v1 layout=L6
    Return v2
}
function f35 name="zebra" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="StaticName static zebra 0"
    Return v1
}
function f36 name="name" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="StaticName static name 1"
    Return v1
}
function f37 name="ant" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="StaticName static ant 2"
    Return v1
}
function f38 name="Patched" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L7
    v2 = NewObject v1 layout=L7
    Return v2
}
function f39 name="zebra" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Patched prototype zebra 0"
    Return v1
}
function f40 name="ant" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="Patched prototype ant 1"
    Return v1
}
function f41 name="callMembers" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="zebra"
    v2 = Call v1, v0 calleeKey="zebra"
    v3 = PropGet v0 key="ant"
    v4 = Call v3, v0 calleeKey="ant"
    v5 = PropGet v0 key="extra"
    v6 = TypeOf v5
    v7 = NewArray v2, v4, v6 contents=anything holes=[]
    Return v7
}
function f42 name="patch" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="patched"
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral script = R"js(
globalThis.key = "computed";
const createdWithFunction = ["length", "name", "prototype"];
globalThis.show = (label, holder) => {
    let line = label + ":";
    for (let key of Reflect.ownKeys(holder)) {
        if (typeof holder === "function" && createdWithFunction.includes(key))
            continue;
        let descriptor = Object.getOwnPropertyDescriptor(holder, key);
        line += " " + key + "=" + ("value" in descriptor ? typeof descriptor.value : (descriptor.get ? "get" : "") + (descriptor.set ? "set" : ""));
    }
    print(line);
};
globalThis.showValue = (label, value) => { print(label + ": " + value); };
)js"_s;

static constexpr ASCIILiteral expectedOutput = R"(Descending.prototype: constructor=function zebra=function yak=function viper=getset ant=function
Descending: walrus=function bee=function
Ascending.prototype: constructor=function ant=function zebra=function
Ascending: bee=function walrus=function
ComputedInTheMiddle.prototype: constructor=function zebra=function computed=function ant=function
ComputedInTheMiddle: zebra=function ant=function
MethodThenGetter.prototype: constructor=function first=function member=get last=function
MethodThenGetter:
GetterThenMethod.prototype: constructor=function first=function member=function last=function
GetterThenMethod:
MethodTwice.prototype: constructor=function member=function other=function
MethodTwice:
StaticName.prototype: constructor=function
StaticName: zebra=function ant=function
Patched.prototype: constructor=function zebra=function ant=function
Patched:
method then getter: MethodThenGetter prototype member 3
getter then method: GetterThenMethod prototype member 3
method twice: MethodTwice prototype member 2
computed: ComputedInTheMiddle prototype computed 1
static name: StaticName static name 1
before the stores: Patched prototype zebra 0,Patched prototype ant 1,undefined
after the stores: patched,Patched prototype ant 1,function
Patched.prototype after the stores: constructor=function zebra=function ant=function extra=function
)"_s;

static std::unique_ptr<CompileOutput> compileOnThisThread(std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
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
    AOT_TEST_CHECK(output->entryModuleIDs.size() == 1);
    return output;
}

static String runInShell(std::initializer_list<ASCIILiteral> optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(optionWords);
    });

    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(script, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").catch(error => print(\"the program threw \" + error));"_s),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkMemberOrder(std::initializer_list<ASCIILiteral> optionWords)
{
    String output = runInShell(optionWords);
    if (output != expectedOutput)
        dataLog(output);
    AOT_TEST_CHECK(output == expectedOutput);
}

static void testEveryPass()
{
    checkMemberOrder({ });
    checkMemberOrder({ "references=narrow"_s });
}

static void testWithoutInlining()
{
    checkMemberOrder({ "disable=pass:InlineCalls"_s });
}

static void testEveryMemberAtDefinition()
{
    checkMemberOrder({ "disable=flow-results"_s });
}

std::span<const NativeTest> classMemberOrderTests()
{
    static constexpr NativeTest tests[] = {
        { "class-member-order/every-pass"_s, testEveryPass },
        { "class-member-order/without-inlining"_s, testWithoutInlining },
        { "class-member-order/every-member-at-definition"_s, testEveryMemberAtDefinition },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
