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
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral stepsOfCollections = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="Map" [isFreeName]
    v3 = Construct v2
    v4 = Integer integer=1
    v5 = String string="one"
    v6 = PropGet v3 key="set"
    v7 = Call v6, v3, v4, v5 calleeKey="set"
    v8 = Integer integer=2
    v9 = String string="two"
    v10 = Call v6, v3, v8, v9 calleeKey="set"
    v11 = String string="keys"
    v12 = Call v1, -, v11
    v13 = PropGet v3 key="keys"
    v14 = Call v13, v3 calleeKey="keys"
    Jump -> next b1()
  b1():
    LoopHint
    v16 = PropGet v14 key="next"
    v17 = Call v16, v14 calleeKey="next"
    v18 = PropGet v17 key="done"
    v19 = ToBoolean v18
    Branch v19 -> taken b3(), notTaken b2()
  b2():
    v20 = PropGet v17 key="value"
    v21 = Call v1, -, v20
    Jump -> next b1()
  b3():
    v22 = PropGet v17 key="value"
    v23 = PropGet v17 key="done"
    v24 = Call v1, -, v22, v23
    v25 = String string="values"
    v26 = Call v1, -, v25
    v27 = PropGet v3 key="values"
    v28 = Call v27, v3 calleeKey="values"
    Jump -> next b4()
  b4():
    LoopHint
    v30 = PropGet v28 key="next"
    v31 = Call v30, v28 calleeKey="next"
    v32 = PropGet v31 key="done"
    v33 = ToBoolean v32
    Branch v33 -> taken b6(), notTaken b5()
  b5():
    v34 = PropGet v31 key="value"
    v35 = Call v1, -, v34
    Jump -> next b4()
  b6():
    v36 = String string="entries"
    v37 = Call v1, -, v36
    v38 = PropGet v3 key="entries"
    v39 = Call v38, v3 calleeKey="entries"
    Jump -> next b7()
  b7():
    LoopHint
    v41 = PropGet v39 key="next"
    v42 = Call v41, v39 calleeKey="next"
    v43 = PropGet v42 key="done"
    v44 = ToBoolean v43
    Branch v44 -> taken b9(), notTaken b8()
  b8():
    v45 = PropGet v42 key="value"
    v46 = Integer integer=0
    v47 = ElemGet v45, v46
    v48 = Integer integer=1
    v49 = ElemGet v45, v48
    v50 = Call v1, -, v47, v49
    Jump -> next b7()
  b9():
    v51 = String string="set"
    v52 = Call v1, -, v51
    v53 = PropGet v0 key="Set" [isFreeName]
    v54 = Construct v53
    v55 = PropGet v54 key="add"
    v56 = String string="a"
    v57 = Call v55, v54, v56 calleeKey="add"
    v58 = String string="b"
    v59 = Call v55, v54, v58 calleeKey="add"
    v60 = PropGet v54 key="values"
    v61 = Call v60, v54 calleeKey="values"
    Jump -> next b10()
  b10():
    LoopHint
    v63 = PropGet v61 key="next"
    v64 = Call v63, v61 calleeKey="next"
    v65 = PropGet v64 key="done"
    v66 = ToBoolean v65
    Branch v66 -> taken b12(), notTaken b11()
  b11():
    v67 = PropGet v64 key="value"
    v68 = Call v1, -, v67
    Jump -> next b10()
  b12():
    v69 = String string="string"
    v70 = Call v1, -, v69
    v71 = String string="xyz"
    v72 = Builtin builtin=SymbolIterator
    v73 = ElemGet v71, v72
    v74 = Call v73, v71
    Jump -> next b13()
  b13():
    LoopHint
    v76 = PropGet v74 key="next"
    v77 = Call v76, v74 calleeKey="next"
    v78 = PropGet v77 key="done"
    v79 = ToBoolean v78
    Branch v79 -> taken b15(), notTaken b14()
  b14():
    v80 = PropGet v77 key="value"
    v81 = Call v1, -, v80
    Jump -> next b13()
  b15():
    v82 = PropGet v77 key="value"
    v83 = PropGet v77 key="done"
    v84 = Call v1, -, v82, v83
    Return
}
)IR"_s;

static constexpr ASCIILiteral propertyDescriptors = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Point" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Holder" fields=[]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=5
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = FinishObject v3
    v5 = PropGet v0 key="Object" [isFreeName]
    v6 = PropGet v5 key="getOwnPropertyDescriptor"
    v7 = String string="a"
    v8 = Call v6, v5, v4, v7 calleeKey="getOwnPropertyDescriptor"
    v9 = CallDirect v8 function=f1
    v10 = String string="b"
    v11 = Call v6, v5, v4, v10 calleeKey="getOwnPropertyDescriptor"
    v12 = CallDirect v11 function=f1
    v13 = NewObject layout=L1 fields=[]
    v14 = NewClosure function=f2
    DefineGetter v13, v14 key="a"
    v16 = FinishObject v13
    v17 = Call v6, v5, v16, v7 calleeKey="getOwnPropertyDescriptor"
    v18 = CallDirect v17 function=f1
    v19 = PropGet v0 key="Reflect" [isFreeName]
    v20 = PropGet v19 key="getOwnPropertyDescriptor"
    v21 = Call v20, v19, v4, v7 calleeKey="getOwnPropertyDescriptor"
    v22 = CallDirect v21 function=f1
    Return
}
function f1 name="describe" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = ToBoolean v0
    Branch v3 -> taken b1(), notTaken b2()
  b1():
    v4 = PropGet v0 key="value"
    v5 = PropGet v0 key="writable"
    v6 = PropGet v0 key="get"
    v7 = TypeOf v6
    v8 = PropGet v0 key="set"
    v9 = TypeOf v8
    v10 = PropGet v0 key="enumerable"
    v11 = PropGet v0 key="configurable"
    v12 = Call v2, -, v4, v5, v7, v9, v10, v11
    Return
  b2():
    v13 = String string="none"
    v14 = Call v2, -, v13
    Return
}
function f2 name="a" flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=1
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral countsAndCapability = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="Uint8Array" [isFreeName]
    v3 = Integer integer=8
    v4 = Construct v2, -, v3
    v5 = PropGet v4 key="setFromBase64"
    v6 = String string="AQID"
    v7 = Call v5, v4, v6 calleeKey="setFromBase64"
    v8 = PropGet v7 key="read"
    v9 = PropGet v7 key="written"
    v10 = Add v8, v9
    v11 = Call v1, -, v8, v9, v10
    v12 = PropGet v4 key="setFromHex"
    v13 = String string="0a0b"
    v14 = Call v12, v4, v13 calleeKey="setFromHex"
    v15 = PropGet v14 key="read"
    v16 = PropGet v14 key="written"
    v17 = Call v1, -, v15, v16
    v18 = PropGet v0 key="Promise" [isFreeName]
    v19 = PropGet v18 key="withResolvers"
    v20 = Call v19, v18 calleeKey="withResolvers"
    v21 = PropGet v20 key="promise"
    v22 = PropGet v20 key="resolve"
    v23 = PropGet v20 key="reject"
    v24 = TypeOf v21
    v25 = TypeOf v22
    v26 = TypeOf v23
    v27 = Call v1, -, v24, v25, v26
    v28 = NewClosure function=f1
    v29 = PropGet v21 key="then"
    v30 = Call v29, v21, v28 calleeKey="then"
    v31 = Integer integer=42
    v32 = Call v22, -, v31
    Return
}
function f1 name="settled" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = String string="resolved with"
    v4 = Call v2, -, v3, v0
    Return
}
)IR"_s;

static constexpr ASCIILiteral linkTimeConstants = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Item" fields=[{key="x" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Builtin builtin=LinkTimeConstantToIntegerOrInfinity
    v3 = Double number=-2.5
    v4 = Call v2, -, v3
    v5 = String string="7.9"
    v6 = Call v2, -, v5
    v7 = Call v1, -, v4, v6
    v8 = Integer integer=5
    v9 = NewObject -, v8 layout=L0 fields=[0]
    v10 = NewArray v9 contents=anything
    v11 = Builtin builtin=LinkTimeConstantArrayFromFastWithoutMapFn
    v12 = Builtin builtin=Array
    v13 = Call v11, -, v12, v10
    v14 = Integer integer=0
    v15 = ElemGet v13, v14
    v16 = PropGet v15 key="x"
    v17 = StrictEq v13, v10
    v18 = Call v11, -, v12, v8
    v19 = Call v11, -, v0, v10
    v20 = Call v1, -, v16, v17, v18, v19
    v21 = Builtin builtin=Uint8Array
    v22 = Integer integer=3
    v23 = Construct v21, -, v22
    v24 = Builtin builtin=LinkTimeConstantTypedArrayFromFast
    v25 = Call v24, -, v21, v23
    v26 = Builtin builtin=LinkTimeConstantTypedArrayLength
    v27 = Call v26, -, v25
    v28 = Builtin builtin=LinkTimeConstantIsTypedArrayOutOfBounds
    v29 = Call v28, -, v25
    v30 = Integer integer=1
    v31 = ElemGet v25, v30
    v32 = Call v24, -, v21, v10
    v33 = Call v1, -, v27, v29, v31, v32
    v34 = Builtin builtin=LinkTimeConstantInstanceOf
    v35 = Builtin builtin=ArrayPrototype
    v36 = Call v34, -, v10, v35
    v37 = Call v34, -, v9, v35
    v38 = Call v34, -, v8, v35
    v39 = Call v1, -, v36, v37, v38
    v40 = Builtin builtin=LinkTimeConstantMakeTypeError
    v41 = String string="not this"
    v42 = Call v40, -, v41
    v43 = PropGet v42 key="name"
    v44 = PropGet v42 key="message"
    v45 = Call v1, -, v43, v44
    v46 = NewClosure function=f1
    v47 = NewClosure function=f2
    v48 = Builtin builtin=Promise
    v49 = Builtin builtin=LinkTimeConstantPromiseResolve
    v50 = Call v49, -, v48, v9
    v51 = PropGet v50 key="then"
    v52 = Call v51, v50, v46 calleeKey="then"
    v53 = Call v49, -, v48, v50
    v54 = StrictEq v53, v50
    v55 = Call v1, -, v54
    v56 = Builtin builtin=LinkTimeConstantNewResolvedPromise
    v57 = Call v56, -, v50
    v58 = PropGet v57 key="then"
    v59 = Call v58, v57, v46 calleeKey="then"
    v60 = Builtin builtin=LinkTimeConstantPromiseReject
    v61 = Call v60, -, v48, v9
    v62 = PropGet v61 key="then"
    v63 = Call v62, v61, v46, v47 calleeKey="then"
    v64 = Builtin builtin=LinkTimeConstantNewRejectedPromise
    v65 = Call v64, -, v9
    v66 = PropGet v65 key="then"
    v67 = Call v66, v65, v46, v47 calleeKey="then"
    v68 = PropGet v10 key="values"
    v69 = Call v68, v10 calleeKey="values"
    v70 = PropGet v69 key="next"
    v71 = Builtin builtin=LinkTimeConstantWrapForValidIteratorCreate
    v72 = Call v71, -, v69, v70
    v73 = TypeOf v72
    v74 = Builtin builtin=LinkTimeConstantAsyncFromSyncIteratorCreate
    v75 = Call v74, -, v69
    v76 = TypeOf v75
    v77 = Builtin builtin=LinkTimeConstantIteratorHelperCreate
    v78 = Call v77, -, v69, v69
    v79 = TypeOf v78
    v80 = Call v1, -, v73, v76, v79
    v81 = Builtin builtin=LinkTimeConstantIsArray
    v82 = Call v81, -, v10
    v83 = Call v81, -, v9
    v84 = Builtin builtin=ArrayIsArray
    v85 = StrictEq v81, v84
    v86 = Call v1, -, v82, v83, v85
    Return
}
function f1 name="fulfilled" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = String string="fulfilled with"
    v4 = PropGet v0 key="x"
    v5 = Call v2, -, v3, v4
    Return
}
function f2 name="rejected" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = String string="rejected with"
    v4 = PropGet v0 key="x"
    v5 = Call v2, -, v3, v4
    Return
}
)IR"_s;

static constexpr ASCIILiteral settlementRecords = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="Promise" [isFreeName]
    v2 = PropGet v1 key="resolve"
    v3 = Integer integer=1
    v4 = Call v2, v1, v3 calleeKey="resolve"
    v5 = PropGet v1 key="reject"
    v6 = Integer integer=2
    v7 = Call v5, v1, v6 calleeKey="reject"
    v8 = NewArray v4, v7 contents=anything
    v9 = PropGet v1 key="allSettled"
    v10 = Call v9, v1, v8 calleeKey="allSettled"
    v11 = NewClosure function=f1
    v12 = PropGet v10 key="then"
    v13 = Call v12, v10, v11 calleeKey="then"
    Return
}
function f1 name="settled" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = Integer integer=0
    v4 = ElemGet v0, v3
    v5 = PropGet v4 key="status"
    v6 = PropGet v4 key="value"
    v7 = PropGet v4 key="reason"
    v8 = Call v2, -, v5, v6, v7
    v9 = Integer integer=1
    v10 = ElemGet v0, v9
    v11 = PropGet v10 key="status"
    v12 = PropGet v10 key="value"
    v13 = PropGet v10 key="reason"
    v14 = Call v2, -, v11, v12, v13
    Return
}
)IR"_s;

static constexpr ASCIILiteral stepsOfArrayIterator = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="internal:library" displayPath="internal:library" kind=RuntimeSource flags=[IsLibrary] exportRule=RuntimeModule
entry module=m0
runtimefunction id=68 function=f1
variable g0 name="kept" module=m0 declared={type=anything} initialization=Uninitialized
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="array"
    v3 = Call v1, -, v2
    v4 = Integer integer=10
    v5 = Integer integer=20
    v6 = NewArray v4, v5 contents=anything
    v7 = PropGet v6 key="values"
    v8 = Call v7, v6 calleeKey="values"
    Jump -> next b1()
  b1():
    LoopHint
    v10 = PropGet v8 key="next"
    v11 = Call v10, v8 calleeKey="next"
    PutModuleVariable v11 variable=g0
    v13 = PropGet v11 key="done"
    v14 = ToBoolean v13
    Branch v14 -> taken b3(), notTaken b2()
  b2():
    v15 = PropGet v11 key="value"
    v16 = Call v1, -, v15
    Jump -> next b1()
  b3():
    v17 = PropGet v11 key="value"
    v18 = PropGet v11 key="done"
    v19 = Call v1, -, v17, v18
    v20 = CallDirect function=f2
    Return
}
function f1 name="next" module=m1 role=Library flags=[HasThis] enclosingRuntimeFunction=68 results=[{type=anything}] {
    v1 = Convert v0 declared={type=object flags=[IsFromCompiler, IsClaim]}
    v2 = GetInternalField v1
    v3 = Check v2 expectedType=i32
    v4 = Integer integer=-1
    v5 = StrictEq v3, v4
    Branch v5 -> taken b1(), notTaken b2()
  b1():
    v6 = Undefined
    v7 = Boolean boolean=true
    v8 = CreateIteratorResult v6, v7
    Return v8
  b2():
    v9 = GetInternalField v1 index=1
    v10 = Check v9 expectedType=Array<anything>
    v11 = PropGet v10 key="length"
    v12 = ToLength v11
    v13 = Less v3, v12
    Branch v13 -> taken b3(), notTaken b4()
  b3():
    v14 = Integer integer=1
    v15 = Add v3, v14
    v16 = PutInternalField v1, v15
    v17 = ElemGet v10, v3
    v18 = Boolean boolean=false
    v19 = CreateIteratorResult v17, v18
    Return v19
  b4():
    v20 = PutInternalField v1, v4
    v21 = Undefined
    v22 = Boolean boolean=true
    v23 = CreateIteratorResult v21, v22
    Return v23
}
function f2 name="readKept" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = GetModuleVariable variable=g0
    v3 = PropGet v2 key="value"
    v4 = PropGet v2 key="done"
    v5 = String string="kept"
    v6 = Call v1, -, v5, v3, v4
    Return
}
)IR"_s;

static constexpr ASCIILiteral stepsOfGenerator = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="internal:library" displayPath="internal:library" kind=RuntimeSource flags=[IsLibrary] exportRule=RuntimeModule
entry module=m0
runtimefunction id=69 function=f2
variable g0 name="kept" module=m0 declared={type=anything} initialization=Uninitialized
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="generator"
    v3 = Call v1, -, v2
    v4 = NewClosure function=f1
    v5 = Call v4
    Jump -> next b1()
  b1():
    LoopHint
    v7 = PropGet v5 key="next"
    v8 = Call v7, v5 calleeKey="next"
    PutModuleVariable v8 variable=g0
    v10 = PropGet v8 key="done"
    v11 = ToBoolean v10
    Branch v11 -> taken b3(), notTaken b2()
  b2():
    v12 = PropGet v8 key="value"
    v13 = Call v1, -, v12
    Jump -> next b1()
  b3():
    v14 = PropGet v8 key="value"
    v15 = PropGet v8 key="done"
    v16 = Call v1, -, v14, v15
    v17 = CallDirect function=f3
    Return
}
function f1 name="g" flags=[IsGenerator, TakesCallee] results=[{type=anything}] {
    InitialYield
    v2 = String string="first"
    v3 = Yield v2
    v4 = String string="second"
    v5 = Yield v4
    v6 = String string="returned"
    Return v6
}
function f2 name="next" module=m1 role=Library flags=[HasThis] enclosingRuntimeFunction=69 parameters=[{type=anything}] results=[{type=anything}] {
    v2 = StepGenerator v0, v1 check=IsGenerator
    v3 = ResultAt v2
    v4 = ResultAt v2 index=1
    v5 = Integer integer=2
    v6 = StrictEq v4, v5
    Branch v6 -> taken b1(), notTaken b2()
  b1():
    v7 = String string="not stepped"
    v8 = Boolean boolean=true
    v9 = CreateIteratorResult v7, v8
    Return v9
  b2():
    v10 = Integer integer=1
    v11 = StrictEq v4, v10
    v12 = CreateIteratorResult v3, v11
    Return v12
}
function f3 name="readKept" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = GetModuleVariable variable=g0
    v3 = PropGet v2 key="value"
    v4 = PropGet v2 key="done"
    v5 = String string="kept"
    v6 = Call v1, -, v5, v3, v4
    Return
}
)IR"_s;

static std::unique_ptr<CompileOutput> compile(ASCIILiteral programText, ASCIILiteral optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        for (StringView word : StringView(optionWords).split(' '))
            AOT_TEST_CHECK(!applyOptionWord(options, word));
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

static String runInShell(ASCIILiteral programText, ASCIILiteral optionWords)
{
    std::unique_ptr<CompileOutput> output = compile(programText, optionWords);
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString("runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").catch(error => print(\"the program threw \" + error)); drainMicrotasks();"_s),
    };
    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

// An optional pass may not change an answer, and validation stops the program at a value that is not of the type flow proved.
static void checkShellOutput(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral builds[] {
        "references=wide"_s,
        "references=narrow"_s,
        "references=wide validate"_s,
        "references=narrow validate"_s,
        "references=narrow validate disable=pass:SimplifyBuiltinCalls"_s,
        "references=narrow validate disable=pass:SinkAllocations"_s,
    };
    for (ASCIILiteral optionWords : builds) {
        String output = runInShell(programText, optionWords);
        if (output != expectedOutput)
            dataLog("with ", optionWords, " the shell printed\n", output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(output == expectedOutput);
    }
}

static void testStepsOfCollections()
{
    checkShellOutput(stepsOfCollections, R"(keys
1
2
undefined true
values
one
two
entries
1 one
2 two
set
a
b
string
x
y
z
undefined true
)"_s);
}

static void testPropertyDescriptors()
{
    checkShellOutput(propertyDescriptors, R"(5 true undefined undefined true true
none
undefined undefined function undefined true true
5 true undefined undefined true true
)"_s);
}

static void testCountsAndCapability()
{
    checkShellOutput(countsAndCapability, R"(4 3 7
4 2
object function function
resolved with 42
)"_s);
}

static void testLinkTimeConstants()
{
    checkShellOutput(linkTimeConstants, R"(-2 7
5 false undefined undefined
3 false 0 undefined
true false false
TypeError not this
true
object object object
true false false
fulfilled with 5
rejected with 5
rejected with 5
fulfilled with 5
)"_s);
}

static void testSettlementRecords()
{
    checkShellOutput(settlementRecords, R"(fulfilled 1 undefined
rejected undefined 2
)"_s);
}

static void testStepsOfArrayIterator()
{
    checkShellOutput(stepsOfArrayIterator, R"(array
10
20
undefined true
kept undefined true
)"_s);
}

static void testStepsOfGenerator()
{
    checkShellOutput(stepsOfGenerator, R"(generator
first
second
returned true
kept returned true
)"_s);
}

std::span<const NativeTest> builtinResultObjectTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-result-objects/steps-of-collections"_s, testStepsOfCollections },
        { "builtin-result-objects/property-descriptors"_s, testPropertyDescriptors },
        { "builtin-result-objects/counts-and-capability"_s, testCountsAndCapability },
        { "builtin-result-objects/link-time-constants"_s, testLinkTimeConstants },
        { "builtin-result-objects/settlement-records"_s, testSettlementRecords },
        { "builtin-result-objects/steps-of-array-iterator"_s, testStepsOfArrayIterator },
        { "builtin-result-objects/steps-of-generator"_s, testStepsOfGenerator },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
