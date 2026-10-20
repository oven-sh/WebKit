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

// class C { a = x; constructor(x) { this.b = x + 1 } }
// class B { constructor(x) { this.q = x + 1; this.p = x } }
// class D extends B { constructor(x) { super(x); this.c = x + 2; this.d = x + 3 } }
// class E { constructor(x, flag) { if (flag) { this.y = x + 1; this.x = x } else { this.x = x; this.y = x + 1 } } }
// class H { constructor(x) { this.n = x; helper(this, x); this.m = x + 2; this.n = x + 9 } bump(x) { this.k = x + 20 } }, helper(h, x) { h.k = x + 1 }
// function F(x) { this.u = x; this.v = x + 1 }
// The layouts of B, D, H and F declare the fields in another order than the constructors create them.
static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="C" constructor=f1 fields=[{key="a" declared={type=number} flags=[IsRequired]}, {key="b" declared={type=number} flags=[IsRequired]}]
layout L1 kind=Instance name="B" constructor=f3 fields=[{key="p" declared={type=number} flags=[IsRequired]}, {key="q" declared={type=number} flags=[IsRequired]}]
layout L2 kind=Instance name="D" predictedBase=L1 constructor=f4 fields=[{key="d" declared={type=number} flags=[IsRequired]}, {key="c" declared={type=number} flags=[IsRequired]}] flags=[HasHeritage]
layout L3 kind=Instance name="E" constructor=f5 fields=[{key="x" declared={type=number} flags=[IsRequired]}, {key="y" declared={type=number} flags=[IsRequired]}]
layout L4 kind=Instance name="H" constructor=f6 fields=[{key="m" declared={type=number} flags=[IsRequired]}, {key="k" declared={type=number} flags=[IsRequired]}, {key="n" declared={type=number} flags=[IsRequired]}]
layout L5 kind=Instance name="F" constructor=f9 fields=[{key="v" declared={type=number} flags=[IsRequired]}, {key="u" declared={type=number} flags=[IsRequired]}] flags=[IsConstructorFunction]
layout L6 name="Copy"
variable g0 name="B" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = Builtin builtin=MathRandom
    v3 = Call v2
    v4 = ToNumber v3
    v5 = Builtin builtin=MathFloor
    v6 = Call v5, -, v4
    v7 = ToNumber v6
    v8 = Integer integer=1
    v9 = Add v7, v8
    v10 = Integer integer=2
    v11 = Less v4, v10
    v12 = Less v10, v4
    v13 = NewClosure function=f1
    v14 = NewClass v13 layout=L0
    v15 = FinishClass v14 layout=L0
    v16 = Construct v15, -, v9
    v17 = String string="C"
    v18 = Call v1, -, v17, v16
    v19 = NewClosure function=f3
    v20 = NewClass v19 layout=L1
    v21 = FinishClass v20 layout=L1
    PutModuleVariable v21 variable=g0
    v23 = ResolveHeritage v21
    v24 = NewClosure function=f4
    v25 = NewClass v24, v21, v23 layout=L2
    v26 = FinishClass v25 layout=L2
    v27 = Construct v21, -, v9
    v28 = String string="B"
    v29 = Call v1, -, v28, v27
    v30 = Construct v26, -, v9
    v31 = String string="D"
    v32 = Call v1, -, v31, v30
    v33 = NewClosure function=f5
    v34 = NewClass v33 layout=L3
    v35 = FinishClass v34 layout=L3
    v36 = Construct v35, -, v9, v11
    v37 = String string="E(true)"
    v38 = Call v1, -, v37, v36
    v39 = Construct v35, -, v9, v12
    v40 = String string="E(false)"
    v41 = Call v1, -, v40, v39
    v42 = NewClosure function=f6
    v43 = NewClass v42 layout=L4
    v44 = NewClosure function=f8
    DefineMethod v43, v44 layout=L4 key="bump"
    v46 = FinishClass v43 layout=L4
    v47 = Construct v46, -, v9
    v48 = PropGet v47 key="bump"
    v49 = Call v48, v47, v9 calleeKey="bump"
    v50 = String string="H"
    v51 = Call v1, -, v50, v47
    v52 = NewClosure function=f9
    v53 = Construct v52, -, v9
    v54 = String string="F"
    v55 = Call v1, -, v54, v53
    v56 = CallDirect v16, v30, v36, v39, v47, v53 function=f10
    Return
}
function f1 name="C" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = CallDirect v3, v1 function=f2
    v5 = Integer integer=1
    v6 = Add v1, v5
    PropSet v3, v6 key="b"
    Return v3
}
function f2 name="initialize" kind=Initializer flags=[HasThis] thisType={type=L0} parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    PropDefine v0, v1 key="a"
    Return
}
function f3 name="B" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    v2 = GetConstructPrototype v0 layout=L1
    v3 = NewObject v2 layout=L1
    v4 = Integer integer=1
    v5 = Add v1, v4
    PropSet v3, v5 key="q"
    PropSet v3, v1 key="p"
    Return v3
}
function f4 name="D" kind=DerivedConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    v2 = GetModuleVariable variable=g0
    v3 = SuperConstruct v2, v0, v1 layout=L2
    v4 = Integer integer=2
    v5 = Add v1, v4
    PropSet v3, v5 key="c"
    v7 = Integer integer=3
    v8 = Add v1, v7
    PropSet v3, v8 key="d"
    Return v3
}
function f5 name="E" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=number}, {type=boolean}] results=[{type=anything}] {
  b0(v0: anything, v1: number, v2: boolean):
    v3 = GetConstructPrototype v0 layout=L3
    v4 = NewObject v3 layout=L3
    v5 = Integer integer=1
    v6 = Add v1, v5
    v7 = ToBoolean v2
    Branch v7 -> taken b1(), notTaken b2()
  b1():
    PropSet v4, v6 key="y"
    PropSet v4, v1 key="x"
    Return v4
  b2():
    PropSet v4, v1 key="x"
    PropSet v4, v6 key="y"
    Return v4
}
function f6 name="H" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    v2 = GetConstructPrototype v0 layout=L4
    v3 = NewObject v2 layout=L4
    PropSet v3, v1 key="n"
    v5 = CallDirect v3, v1 function=f7
    v6 = Integer integer=2
    v7 = Add v1, v6
    PropSet v3, v7 key="m"
    v9 = Integer integer=9
    v10 = Add v1, v9
    PropSet v3, v10 key="n"
    Return v3
}
function f7 name="helper" parameters=[{type=L4}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L4, v1: number):
    v2 = Integer integer=1
    v3 = Add v1, v2
    PropSet v0, v3 key="k"
    Return
}
function f8 name="bump" kind=Method flags=[HasThis] thisType={type=L4} parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: object ? L4, v1: number):
    v2 = Integer integer=20
    v3 = Add v1, v2
    PropSet v0, v3 key="k"
    Return
}
function f9 name="F" flags=[HasThis] thisType={type=L5} parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: object ? L5, v1: number):
    PropSet v0, v1 key="u"
    v3 = Integer integer=1
    v4 = Add v1, v3
    PropSet v0, v4 key="v"
    Return
}
function f10 name="compiled" parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = CallDirect v0 function=f11
    v7 = CallDirect v1 function=f11
    v8 = CallDirect v2 function=f11
    v9 = CallDirect v3 function=f11
    v10 = CallDirect v4 function=f11
    v11 = CallDirect v5 function=f11
    Return
}
function f11 name="walk" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="showCompiled" [isFreeName]
    v3 = PropGet v1 key="noteKey" [isFreeName]
    v4 = Builtin builtin=ObjectKeys
    v5 = Call v4, -, v0
    v6 = Builtin builtin=ObjectValues
    v7 = Call v6, -, v0
    v8 = NewObject layout=L6 fields=[]
    CopyDataProperties v8, v0
    v10 = FinishObject v8
    v11 = ForInOpen v0
    Jump -> next b1()
  b1():
    LoopHint
    v13 = ForInStep v11, v0
    v14 = ResultAt v13 index=0
    v15 = ResultAt v13 index=1
    Branch v14 -> taken b3(), notTaken b2()
  b2():
    v16 = Call v3, -, v15
    Jump -> next b1()
  b3():
    v17 = Call v2, -, v5, v7, v10
    Return
}
)IR"_s;

// type Record = { a: number, b?: number, c?: number }. addB(o, x) { o.b = x + 1 }, addC(o, x) { o.c = x + 2 }, addBoth(o, x) { o.b = x + 3; o.c = x + 3 }
static constexpr ASCIILiteral programCreatingFieldsLater = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Record" fields=[{key="a" declared={type=number} flags=[IsRequired]}, {key="b" declared={type=number}}, {key="c" declared={type=number}}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = Builtin builtin=MathRandom
    v3 = Call v2
    v4 = ToNumber v3
    v5 = Builtin builtin=MathFloor
    v6 = Call v5, -, v4
    v7 = ToNumber v6
    v8 = Integer integer=1
    v9 = Add v7, v8
    v10 = NewObject -, v9 layout=L0 fields=[0]
    v11 = FinishObject v10
    v12 = NewObject -, v9 layout=L0 fields=[0]
    v13 = FinishObject v12
    v14 = NewObject -, v9, v9 layout=L0 fields=[0, 1]
    v15 = FinishObject v14
    v16 = NewObject -, v9 layout=L0 fields=[0]
    v17 = FinishObject v16
    v18 = NewObject -, v9, v9 layout=L0 fields=[0, 2]
    v19 = FinishObject v18
    v20 = CallDirect v11, v9 function=f1
    v21 = CallDirect v11, v9 function=f2
    v22 = CallDirect v13, v9 function=f2
    v23 = CallDirect v13, v9 function=f1
    v24 = CallDirect v15, v9 function=f1
    v25 = CallDirect v15, v9 function=f2
    v26 = CallDirect v17, v9 function=f3
    v27 = CallDirect v19, v9 function=f1
    v28 = CallDirect v19, v9 function=f3
    v29 = String string="b then c"
    v30 = Call v1, -, v29, v11
    v31 = String string="c then b"
    v32 = Call v1, -, v31, v13
    v33 = String string="b listed, then c"
    v34 = Call v1, -, v33, v15
    v35 = String string="b and c in one run"
    v36 = Call v1, -, v35, v17
    v37 = String string="c listed, then b, then the run"
    v38 = Call v1, -, v37, v19
    Return
}
function f1 name="addB" parameters=[{type=L0}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    v2 = Integer integer=1
    v3 = Add v1, v2
    PropSet v0, v3 key="b"
    Return
}
function f2 name="addC" parameters=[{type=L0}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    v2 = Integer integer=2
    v3 = Add v1, v2
    PropSet v0, v3 key="c"
    Return
}
function f3 name="addBoth" parameters=[{type=L0}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    v2 = Integer integer=3
    v3 = Add v1, v2
    PropSet v0, v3 key="b"
    PropSet v0, v3 key="c"
    Return
}
)IR"_s;

// B and D as above. No object reaches other code, so the stores of B have both classes as proven receivers.
static constexpr ASCIILiteral programKeepingObjects = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="B" constructor=f1 fields=[{key="p" declared={type=number} flags=[IsRequired]}, {key="q" declared={type=number} flags=[IsRequired]}]
layout L1 kind=Instance name="D" predictedBase=L0 constructor=f2 fields=[{key="d" declared={type=number} flags=[IsRequired]}, {key="c" declared={type=number} flags=[IsRequired]}] flags=[HasHeritage]
variable g0 name="B" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="showList" [isFreeName]
    v2 = Builtin builtin=MathRandom
    v3 = Call v2
    v4 = ToNumber v3
    v5 = Builtin builtin=MathFloor
    v6 = Call v5, -, v4
    v7 = ToNumber v6
    v8 = Integer integer=1
    v9 = Add v7, v8
    v10 = Builtin builtin=ObjectKeys
    v11 = Builtin builtin=ObjectValues
    v12 = NewClosure function=f1
    v13 = NewClass v12 layout=L0
    v14 = FinishClass v13 layout=L0
    PutModuleVariable v14 variable=g0
    v16 = ResolveHeritage v14
    v17 = NewClosure function=f2
    v18 = NewClass v17, v14, v16 layout=L1
    v19 = FinishClass v18 layout=L1
    v20 = Construct v14, -, v9
    v21 = Construct v19, -, v9
    v22 = Call v10, -, v20
    v23 = Call v11, -, v20
    v24 = Call v10, -, v21
    v25 = Call v11, -, v21
    v26 = String string="B"
    v27 = Call v1, -, v26, v22, v23
    v28 = String string="D"
    v29 = Call v1, -, v28, v24, v25
    Return
}
function f1 name="B" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Integer integer=1
    v5 = Add v1, v4
    PropSet v3, v5 key="q"
    PropSet v3, v1 key="p"
    Return v3
}
function f2 name="D" kind=DerivedConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    v2 = GetModuleVariable variable=g0
    v3 = SuperConstruct v2, v0, v1 layout=L1
    v4 = Integer integer=2
    v5 = Add v1, v4
    PropSet v3, v5 key="c"
    v7 = Integer integer=3
    v8 = Add v1, v7
    PropSet v3, v8 key="d"
    Return v3
}
)IR"_s;

static constexpr ASCIILiteral programWithLongRuns = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Wide" fields=[{key="a" declared={type=number}}, {key="b" declared={type=number}}, {key="c" declared={type=number}}, {key="d" declared={type=number}}, {key="e" declared={type=number}}, {key="f" declared={type=number}}, {key="g" declared={type=number}}, {key="h" declared={type=number}}, {key="i" declared={type=number}}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = Builtin builtin=MathRandom
    v3 = Call v2
    v4 = ToNumber v3
    v5 = Builtin builtin=MathFloor
    v6 = Call v5, -, v4
    v7 = ToNumber v6
    v8 = Integer integer=1
    v9 = Add v7, v8
    v10 = NewObject layout=L0 fields=[]
    v11 = FinishObject v10
    v12 = NewObject -, v9 layout=L0 fields=[5]
    v13 = FinishObject v12
    v14 = NewObject -, v9, v9 layout=L0 fields=[8, 4]
    v15 = FinishObject v14
    v16 = NewObject layout=L0 fields=[]
    v17 = FinishObject v16
    v18 = NewObject layout=L0 fields=[]
    v19 = FinishObject v18
    v20 = NewObject layout=L0 fields=[]
    v21 = FinishObject v20
    v22 = CallDirect v11, v9 function=f1
    v23 = CallDirect v13, v9 function=f1
    v24 = CallDirect v15, v9 function=f1
    v25 = CallDirect v17, v9 function=f2
    v26 = CallDirect v17, v9 function=f1
    v27 = CallDirect v19, v9 function=f3
    v28 = CallDirect v19, v9 function=f1
    v29 = CallDirect v21, v9 function=f1
    v30 = CallDirect v21, v9 function=f3
    v31 = CallDirect v21, v9 function=f2
    v32 = String string="nothing listed"
    v33 = Call v1, -, v32, v11
    v34 = String string="f listed"
    v35 = Call v1, -, v34, v13
    v36 = String string="i and e listed"
    v37 = Call v1, -, v36, v15
    v38 = String string="the middle, then all"
    v39 = Call v1, -, v38, v17
    v40 = String string="backwards, then all"
    v41 = Call v1, -, v40, v19
    v42 = String string="all, then backwards, then the middle"
    v43 = Call v1, -, v42, v21
    Return
}
function f1 name="fillAll" parameters=[{type=L0}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    v2 = Integer integer=10
    v3 = Add v1, v2
    PropSet v0, v3 key="a"
    v5 = Integer integer=11
    v6 = Add v1, v5
    PropSet v0, v6 key="b"
    v8 = Integer integer=12
    v9 = Add v1, v8
    PropSet v0, v9 key="c"
    v11 = Integer integer=13
    v12 = Add v1, v11
    PropSet v0, v12 key="d"
    v14 = Integer integer=14
    v15 = Add v1, v14
    PropSet v0, v15 key="e"
    v17 = Integer integer=15
    v18 = Add v1, v17
    PropSet v0, v18 key="f"
    v20 = Integer integer=16
    v21 = Add v1, v20
    PropSet v0, v21 key="g"
    v23 = Integer integer=17
    v24 = Add v1, v23
    PropSet v0, v24 key="h"
    v26 = Integer integer=18
    v27 = Add v1, v26
    PropSet v0, v27 key="i"
    Return
}
function f2 name="fillMiddle" parameters=[{type=L0}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    v2 = Integer integer=30
    v3 = Add v1, v2
    PropSet v0, v3 key="c"
    v5 = Integer integer=31
    v6 = Add v1, v5
    PropSet v0, v6 key="d"
    v8 = Integer integer=32
    v9 = Add v1, v8
    PropSet v0, v9 key="e"
    v11 = Integer integer=33
    v12 = Add v1, v11
    PropSet v0, v12 key="f"
    v14 = Integer integer=34
    v15 = Add v1, v14
    PropSet v0, v15 key="g"
    Return
}
function f3 name="fillBackwards" parameters=[{type=L0}, {type=number}] results=[{type=anything}] {
  b0(v0: object ? L0, v1: number):
    v2 = Integer integer=50
    v3 = Add v1, v2
    PropSet v0, v3 key="i"
    v5 = Integer integer=51
    v6 = Add v1, v5
    PropSet v0, v6 key="h"
    v8 = Integer integer=52
    v9 = Add v1, v8
    PropSet v0, v9 key="g"
    v11 = Integer integer=53
    v12 = Add v1, v11
    PropSet v0, v12 key="f"
    v14 = Integer integer=54
    v15 = Add v1, v14
    PropSet v0, v15 key="e"
    v17 = Integer integer=55
    v18 = Add v1, v17
    PropSet v0, v18 key="d"
    v20 = Integer integer=56
    v21 = Add v1, v20
    PropSet v0, v21 key="c"
    v23 = Integer integer=57
    v24 = Add v1, v23
    PropSet v0, v24 key="b"
    v26 = Integer integer=58
    v27 = Add v1, v26
    PropSet v0, v27 key="a"
    Return
}
)IR"_s;

static constexpr ASCIILiteral script = R"js(
let namesOfCompiledLoop = [];
globalThis.noteKey = name => { namesOfCompiledLoop[namesOfCompiledLoop.length] = name; };
globalThis.showCompiled = (keys, values, spread) => {
    print("compiled: keys " + JSON.stringify(keys) + " for-in " + JSON.stringify(namesOfCompiledLoop) + " values " + JSON.stringify(values) + " spread " + JSON.stringify(spread));
    namesOfCompiledLoop = [];
};
globalThis.showList = (name, keys, values) => print(name + ": keys " + JSON.stringify(keys) + " values " + JSON.stringify(values));
globalThis.show = (name, object) => {
    let namesOfLoop = [];
    for (let name in object)
        namesOfLoop[namesOfLoop.length] = name;
    print(name + ": keys " + JSON.stringify(Object.keys(object)) + " for-in " + JSON.stringify(namesOfLoop) + " values " + JSON.stringify(Object.values(object))
        + " entries " + JSON.stringify(Object.entries(object)) + " spread " + JSON.stringify({ ...object }) + " assign " + JSON.stringify(Object.assign({ }, object))
        + " JSON " + JSON.stringify(object));
};
)js"_s;

static constexpr ASCIILiteral expectedOutput = R"(C: keys ["a","b"] for-in ["a","b"] values [1,2] entries [["a",1],["b",2]] spread {"a":1,"b":2} assign {"a":1,"b":2} JSON {"a":1,"b":2}
B: keys ["q","p"] for-in ["q","p"] values [2,1] entries [["q",2],["p",1]] spread {"q":2,"p":1} assign {"q":2,"p":1} JSON {"q":2,"p":1}
D: keys ["q","p","c","d"] for-in ["q","p","c","d"] values [2,1,3,4] entries [["q",2],["p",1],["c",3],["d",4]] spread {"q":2,"p":1,"c":3,"d":4} assign {"q":2,"p":1,"c":3,"d":4} JSON {"q":2,"p":1,"c":3,"d":4}
E(true): keys ["y","x"] for-in ["y","x"] values [2,1] entries [["y",2],["x",1]] spread {"y":2,"x":1} assign {"y":2,"x":1} JSON {"y":2,"x":1}
E(false): keys ["x","y"] for-in ["x","y"] values [1,2] entries [["x",1],["y",2]] spread {"x":1,"y":2} assign {"x":1,"y":2} JSON {"x":1,"y":2}
H: keys ["n","k","m"] for-in ["n","k","m"] values [10,21,3] entries [["n",10],["k",21],["m",3]] spread {"n":10,"k":21,"m":3} assign {"n":10,"k":21,"m":3} JSON {"n":10,"k":21,"m":3}
F: keys ["u","v"] for-in ["u","v"] values [1,2] entries [["u",1],["v",2]] spread {"u":1,"v":2} assign {"u":1,"v":2} JSON {"u":1,"v":2}
compiled: keys ["a","b"] for-in ["a","b"] values [1,2] spread {"a":1,"b":2}
compiled: keys ["q","p","c","d"] for-in ["q","p","c","d"] values [2,1,3,4] spread {"q":2,"p":1,"c":3,"d":4}
compiled: keys ["y","x"] for-in ["y","x"] values [2,1] spread {"y":2,"x":1}
compiled: keys ["x","y"] for-in ["x","y"] values [1,2] spread {"x":1,"y":2}
compiled: keys ["n","k","m"] for-in ["n","k","m"] values [10,21,3] spread {"n":10,"k":21,"m":3}
compiled: keys ["u","v"] for-in ["u","v"] values [1,2] spread {"u":1,"v":2}
)"_s;

static constexpr ASCIILiteral expectedOutputOfFieldsCreatedLater = R"(b then c: keys ["a","b","c"] for-in ["a","b","c"] values [1,2,3] entries [["a",1],["b",2],["c",3]] spread {"a":1,"b":2,"c":3} assign {"a":1,"b":2,"c":3} JSON {"a":1,"b":2,"c":3}
c then b: keys ["a","c","b"] for-in ["a","c","b"] values [1,3,2] entries [["a",1],["c",3],["b",2]] spread {"a":1,"c":3,"b":2} assign {"a":1,"c":3,"b":2} JSON {"a":1,"c":3,"b":2}
b listed, then c: keys ["a","b","c"] for-in ["a","b","c"] values [1,2,3] entries [["a",1],["b",2],["c",3]] spread {"a":1,"b":2,"c":3} assign {"a":1,"b":2,"c":3} JSON {"a":1,"b":2,"c":3}
b and c in one run: keys ["a","b","c"] for-in ["a","b","c"] values [1,4,4] entries [["a",1],["b",4],["c",4]] spread {"a":1,"b":4,"c":4} assign {"a":1,"b":4,"c":4} JSON {"a":1,"b":4,"c":4}
c listed, then b, then the run: keys ["a","c","b"] for-in ["a","c","b"] values [1,4,4] entries [["a",1],["c",4],["b",4]] spread {"a":1,"c":4,"b":4} assign {"a":1,"c":4,"b":4} JSON {"a":1,"c":4,"b":4}
)"_s;

static constexpr ASCIILiteral expectedOutputOfLongRuns = R"(nothing listed: keys ["a","b","c","d","e","f","g","h","i"] for-in ["a","b","c","d","e","f","g","h","i"] values [11,12,13,14,15,16,17,18,19] entries [["a",11],["b",12],["c",13],["d",14],["e",15],["f",16],["g",17],["h",18],["i",19]] spread {"a":11,"b":12,"c":13,"d":14,"e":15,"f":16,"g":17,"h":18,"i":19} assign {"a":11,"b":12,"c":13,"d":14,"e":15,"f":16,"g":17,"h":18,"i":19} JSON {"a":11,"b":12,"c":13,"d":14,"e":15,"f":16,"g":17,"h":18,"i":19}
f listed: keys ["f","a","b","c","d","e","g","h","i"] for-in ["f","a","b","c","d","e","g","h","i"] values [16,11,12,13,14,15,17,18,19] entries [["f",16],["a",11],["b",12],["c",13],["d",14],["e",15],["g",17],["h",18],["i",19]] spread {"f":16,"a":11,"b":12,"c":13,"d":14,"e":15,"g":17,"h":18,"i":19} assign {"f":16,"a":11,"b":12,"c":13,"d":14,"e":15,"g":17,"h":18,"i":19} JSON {"f":16,"a":11,"b":12,"c":13,"d":14,"e":15,"g":17,"h":18,"i":19}
i and e listed: keys ["i","e","a","b","c","d","f","g","h"] for-in ["i","e","a","b","c","d","f","g","h"] values [19,15,11,12,13,14,16,17,18] entries [["i",19],["e",15],["a",11],["b",12],["c",13],["d",14],["f",16],["g",17],["h",18]] spread {"i":19,"e":15,"a":11,"b":12,"c":13,"d":14,"f":16,"g":17,"h":18} assign {"i":19,"e":15,"a":11,"b":12,"c":13,"d":14,"f":16,"g":17,"h":18} JSON {"i":19,"e":15,"a":11,"b":12,"c":13,"d":14,"f":16,"g":17,"h":18}
the middle, then all: keys ["c","d","e","f","g","a","b","h","i"] for-in ["c","d","e","f","g","a","b","h","i"] values [13,14,15,16,17,11,12,18,19] entries [["c",13],["d",14],["e",15],["f",16],["g",17],["a",11],["b",12],["h",18],["i",19]] spread {"c":13,"d":14,"e":15,"f":16,"g":17,"a":11,"b":12,"h":18,"i":19} assign {"c":13,"d":14,"e":15,"f":16,"g":17,"a":11,"b":12,"h":18,"i":19} JSON {"c":13,"d":14,"e":15,"f":16,"g":17,"a":11,"b":12,"h":18,"i":19}
backwards, then all: keys ["i","h","g","f","e","d","c","b","a"] for-in ["i","h","g","f","e","d","c","b","a"] values [19,18,17,16,15,14,13,12,11] entries [["i",19],["h",18],["g",17],["f",16],["e",15],["d",14],["c",13],["b",12],["a",11]] spread {"i":19,"h":18,"g":17,"f":16,"e":15,"d":14,"c":13,"b":12,"a":11} assign {"i":19,"h":18,"g":17,"f":16,"e":15,"d":14,"c":13,"b":12,"a":11} JSON {"i":19,"h":18,"g":17,"f":16,"e":15,"d":14,"c":13,"b":12,"a":11}
all, then backwards, then the middle: keys ["a","b","c","d","e","f","g","h","i"] for-in ["a","b","c","d","e","f","g","h","i"] values [59,58,31,32,33,34,35,52,51] entries [["a",59],["b",58],["c",31],["d",32],["e",33],["f",34],["g",35],["h",52],["i",51]] spread {"a":59,"b":58,"c":31,"d":32,"e":33,"f":34,"g":35,"h":52,"i":51} assign {"a":59,"b":58,"c":31,"d":32,"e":33,"f":34,"g":35,"h":52,"i":51} JSON {"a":59,"b":58,"c":31,"d":32,"e":33,"f":34,"g":35,"h":52,"i":51}
)"_s;

static std::unique_ptr<CompileOutput> compileOnThisThread(ASCIILiteral text, std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(text)), Typed::IRForm::AsRead, build);
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

static String runInShell(ASCIILiteral text, std::initializer_list<ASCIILiteral> optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(text, optionWords);
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

static void checkOutput(ASCIILiteral text, ASCIILiteral expected, std::initializer_list<ASCIILiteral> optionWords)
{
    String output = runInShell(text, optionWords);
    if (output != expected)
        dataLog(output);
    AOT_TEST_CHECK(output == expected);
}

static void checkKeyOrders(std::initializer_list<ASCIILiteral> optionWords)
{
    checkOutput(programText, expectedOutput, optionWords);
}

static void testEveryPass()
{
    checkKeyOrders({ });
    checkKeyOrders({ "references=narrow"_s });
}

static void testWithoutInlining()
{
    checkKeyOrders({ "disable=pass:InlineCalls"_s });
    checkKeyOrders({ "disable=pass:InlineCalls"_s, "references=narrow"_s });
}

static void testWithoutSinking()
{
    checkKeyOrders({ "disable=pass:SinkAllocations"_s });
}

static void testSlotStoresOnly()
{
    checkKeyOrders({ "only-passes=ResolveAccesses"_s });
    checkKeyOrders({ "only-passes=ResolveAccesses,ExpandOwnKeys"_s, "heat=EverythingIsHot"_s });
}

static void testStoresByName()
{
    checkKeyOrders({ "disable=pass:ResolveAccesses"_s });
    checkKeyOrders({ "disable=flow-results"_s });
}

static void testFieldsCreatedLater()
{
    for (ASCIILiteral word : { "check=Validate"_s, "disable=pass:InlineCalls"_s, "only-passes=ResolveAccesses"_s, "disable=pass:ResolveAccesses"_s, "references=narrow"_s })
        checkOutput(programCreatingFieldsLater, expectedOutputOfFieldsCreatedLater, { word });
}

static void testStoresWithTwoReceivers()
{
    constexpr ASCIILiteral expected = "B: keys [\"q\",\"p\"] values [2,1]\nD: keys [\"q\",\"p\",\"c\",\"d\"] values [2,1,3,4]\n"_s;
    for (ASCIILiteral word : { "check=Validate"_s, "disable=pass:InlineCalls"_s, "only-passes=ResolveAccesses"_s, "disable=pass:ResolveAccesses"_s, "references=narrow"_s })
        checkOutput(programKeepingObjects, expected, { word });
}

static void testLongRuns()
{
    for (ASCIILiteral word : { "check=Validate"_s, "references=narrow"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s, "limit=MaxSelectionGroupMembers:4"_s, "disable=rule:SlotStoreRun"_s })
        checkOutput(programWithLongRuns, expectedOutputOfLongRuns, { "disable=pass:InlineCalls"_s, word });
}

std::span<const NativeTest> instanceKeyOrderTests()
{
    static constexpr NativeTest tests[] = {
        { "instance-key-order/every-pass"_s, testEveryPass },
        { "instance-key-order/without-inlining"_s, testWithoutInlining },
        { "instance-key-order/without-sinking"_s, testWithoutSinking },
        { "instance-key-order/slot-stores-only"_s, testSlotStoresOnly },
        { "instance-key-order/stores-by-name"_s, testStoresByName },
        { "instance-key-order/fields-created-later"_s, testFieldsCreatedLater },
        { "instance-key-order/stores-with-two-receivers"_s, testStoresWithTwoReceivers },
        { "instance-key-order/long-runs"_s, testLongRuns },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
