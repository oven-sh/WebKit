/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral staticMethodsOfClassEvaluatedOnce = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = String string="a"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = PropGet v5 key="call"
    v9 = Convert v8 declared={type=function flags=[IsExpected]}
    v10 = Integer integer=1
    v11 = Call v9, v5, v10 calleeKey="call" calleeName="call"
    v12 = Convert v11 declared={type=string flags=[IsExpected]}
    v13 = PropGet v5 key="apply"
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Integer integer=2
    v16 = Call v14, v5, v15 calleeKey="apply" calleeName="apply"
    v17 = Convert v16 declared={type=string flags=[IsExpected]}
    v18 = PropGet v5 key="bind"
    v19 = Convert v18 declared={type=function flags=[IsExpected]}
    v20 = Integer integer=3
    v21 = Call v19, v5, v20 calleeKey="bind" calleeName="bind"
    v22 = Convert v21 declared={type=string flags=[IsExpected]}
    v23 = PropGet v5 key="toString"
    v24 = Convert v23 declared={type=function flags=[IsExpected]}
    v25 = Call v24, v5 calleeKey="toString" calleeName="toString"
    v26 = ToString v5
    v27 = Call v7, -, v12, v17, v22, v25, v26 calleeName="print"
    v28 = Builtin builtin=GlobalThis
    v29 = PropGet v28 key="print" [isFreeName]
    v30 = PropGet v5 key="call"
    v31 = Convert v30 declared={type=function flags=[IsExpected]}
    v32 = Builtin builtin=GlobalThis
    v33 = PropGet v32 key="Function" [isFreeName]
    v34 = Convert v33 declared={type=builtin(Function) flags=[IsExpected]}
    v35 = PropGet v34 key="prototype"
    v36 = Convert v35 declared={type=builtin(FunctionPrototype) flags=[IsExpected]}
    v37 = PropGet v36 key="call"
    v38 = Convert v37 declared={type=builtin(FunctionPrototypeCall) flags=[IsExpected]}
    v39 = StrictEq v31, v38
    v40 = Builtin builtin=GlobalThis
    v41 = PropGet v40 key="Function" [isFreeName]
    v42 = Convert v41 declared={type=builtin(Function) flags=[IsExpected]}
    v43 = PropGet v42 key="prototype"
    v44 = Convert v43 declared={type=builtin(FunctionPrototype) flags=[IsExpected]}
    v45 = PropGet v44 key="call"
    v46 = Convert v45 declared={type=builtin(FunctionPrototypeCall) flags=[IsExpected]}
    v47 = PropGet v46 key="call"
    v48 = Convert v47 declared={type=function flags=[IsExpected]}
    v49 = PropGet v5 key="call"
    v50 = Convert v49 declared={type=function flags=[IsExpected]}
    v51 = Null
    v52 = Integer integer=4
    v53 = Call v48, v46, v50, v51, v52 calleeKey="call" calleeName="call"
    v54 = Call v29, -, v39, v53 calleeName="print"
    v55 = Undefined
    Return v55
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f2
    v2 = NewClass v1 layout=L0
    v3 = NewClosure v0 function=f3
    DefineMethod v2, v3 layout=L0 side=Static key="call"
    v5 = NewClosure v0 function=f4
    DefineMethod v2, v5 layout=L0 side=Static index=1 key="apply"
    v7 = NewClosure v0 function=f5
    DefineMethod v2, v7 layout=L0 side=Static index=2 key="bind"
    v9 = NewClosure v0 function=f6
    DefineMethod v2, v9 layout=L0 side=Static index=3 key="toString"
    v11 = FinishClass v2 layout=L0
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    Return v12
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="call"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f4 name="apply" module=m0 position=79 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="apply"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f5 name="bind" module=m0 position=124 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="bind"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f6 name="toString" module=m0 position=167 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Convert v2 declared={type=Array<anything> flags=[IsFromCompiler, IsClaim]}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f8
runtimefunction id=12 function=f7
)IR"_s;

static constexpr ASCIILiteral staticMethodsOfClassEvaluatedTwice = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = String string="a"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = String string="b"
    v7 = Call v2, -, v6 calleeName="make"
    v8 = Convert v7 declared={type=function flags=[IsExpected]}
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = PropGet v5 key="call"
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    v13 = Integer integer=1
    v14 = Call v12, v5, v13 calleeKey="call" calleeName="call"
    v15 = Convert v14 declared={type=string flags=[IsExpected]}
    v16 = PropGet v8 key="call"
    v17 = Convert v16 declared={type=function flags=[IsExpected]}
    v18 = Integer integer=2
    v19 = Call v17, v8, v18 calleeKey="call" calleeName="call"
    v20 = Convert v19 declared={type=string flags=[IsExpected]}
    v21 = PropGet v5 key="apply"
    v22 = Convert v21 declared={type=function flags=[IsExpected]}
    v23 = Integer integer=3
    v24 = Call v22, v5, v23 calleeKey="apply" calleeName="apply"
    v25 = Convert v24 declared={type=string flags=[IsExpected]}
    v26 = PropGet v8 key="apply"
    v27 = Convert v26 declared={type=function flags=[IsExpected]}
    v28 = Integer integer=4
    v29 = Call v27, v8, v28 calleeKey="apply" calleeName="apply"
    v30 = Convert v29 declared={type=string flags=[IsExpected]}
    v31 = PropGet v5 key="bind"
    v32 = Convert v31 declared={type=function flags=[IsExpected]}
    v33 = Integer integer=5
    v34 = Call v32, v5, v33 calleeKey="bind" calleeName="bind"
    v35 = Convert v34 declared={type=string flags=[IsExpected]}
    v36 = PropGet v8 key="bind"
    v37 = Convert v36 declared={type=function flags=[IsExpected]}
    v38 = Integer integer=6
    v39 = Call v37, v8, v38 calleeKey="bind" calleeName="bind"
    v40 = Convert v39 declared={type=string flags=[IsExpected]}
    v41 = PropGet v5 key="toString"
    v42 = Convert v41 declared={type=function flags=[IsExpected]}
    v43 = Call v42, v5 calleeKey="toString" calleeName="toString"
    v44 = ToString v8
    v45 = Call v10, -, v15, v20, v25, v30, v35, v40, v43, v44 calleeName="print"
    v46 = Builtin builtin=GlobalThis
    v47 = PropGet v46 key="print" [isFreeName]
    v48 = PropGet v5 key="call"
    v49 = Convert v48 declared={type=function flags=[IsExpected]}
    v50 = PropGet v8 key="call"
    v51 = Convert v50 declared={type=function flags=[IsExpected]}
    v52 = StrictEq v49, v51
    v53 = String string="a"
    v54 = Call v2, -, v53 calleeName="make"
    v55 = Convert v54 declared={type=function flags=[IsExpected]}
    v56 = PropGet v55 key="call"
    v57 = Convert v56 declared={type=function flags=[IsExpected]}
    v58 = String string="b"
    v59 = Call v2, -, v58 calleeName="make"
    v60 = Convert v59 declared={type=function flags=[IsExpected]}
    v61 = PropGet v60 key="call"
    v62 = Convert v61 declared={type=function flags=[IsExpected]}
    v63 = StrictEq v57, v62
    v64 = PropGet v5 key="call"
    v65 = Convert v64 declared={type=function flags=[IsExpected]}
    v66 = PropGet v5 key="call"
    v67 = Convert v66 declared={type=function flags=[IsExpected]}
    v68 = StrictEq v65, v67
    v69 = PropGet v5 key="call"
    v70 = Convert v69 declared={type=function flags=[IsExpected]}
    v71 = Builtin builtin=GlobalThis
    v72 = PropGet v71 key="Function" [isFreeName]
    v73 = Convert v72 declared={type=builtin(Function) flags=[IsExpected]}
    v74 = PropGet v73 key="prototype"
    v75 = Convert v74 declared={type=builtin(FunctionPrototype) flags=[IsExpected]}
    v76 = PropGet v75 key="call"
    v77 = Convert v76 declared={type=builtin(FunctionPrototypeCall) flags=[IsExpected]}
    v78 = StrictEq v70, v77
    v79 = Call v47, -, v52, v63, v68, v78 calleeName="print"
    v80 = Builtin builtin=GlobalThis
    v81 = PropGet v80 key="print" [isFreeName]
    v82 = Builtin builtin=GlobalThis
    v83 = PropGet v82 key="Function" [isFreeName]
    v84 = Convert v83 declared={type=builtin(Function) flags=[IsExpected]}
    v85 = PropGet v84 key="prototype"
    v86 = Convert v85 declared={type=builtin(FunctionPrototype) flags=[IsExpected]}
    v87 = PropGet v86 key="call"
    v88 = Convert v87 declared={type=builtin(FunctionPrototypeCall) flags=[IsExpected]}
    v89 = PropGet v88 key="call"
    v90 = Convert v89 declared={type=function flags=[IsExpected]}
    v91 = PropGet v5 key="call"
    v92 = Convert v91 declared={type=function flags=[IsExpected]}
    v93 = Null
    v94 = Integer integer=7
    v95 = Call v90, v88, v92, v93, v94 calleeKey="call" calleeName="call"
    v96 = Builtin builtin=GlobalThis
    v97 = PropGet v96 key="Function" [isFreeName]
    v98 = Convert v97 declared={type=builtin(Function) flags=[IsExpected]}
    v99 = PropGet v98 key="prototype"
    v100 = Convert v99 declared={type=builtin(FunctionPrototype) flags=[IsExpected]}
    v101 = PropGet v100 key="call"
    v102 = Convert v101 declared={type=builtin(FunctionPrototypeCall) flags=[IsExpected]}
    v103 = PropGet v102 key="call"
    v104 = Convert v103 declared={type=function flags=[IsExpected]}
    v105 = String string="c"
    v106 = Call v2, -, v105 calleeName="make"
    v107 = Convert v106 declared={type=function flags=[IsExpected]}
    v108 = PropGet v107 key="call"
    v109 = Convert v108 declared={type=function flags=[IsExpected]}
    v110 = Null
    v111 = Integer integer=8
    v112 = Call v104, v102, v109, v110, v111 calleeKey="call" calleeName="call"
    v113 = Call v81, -, v95, v112 calleeName="print"
    v114 = Undefined
    Return v114
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f2
    v2 = NewClass v1 layout=L0
    v3 = NewClosure v0 function=f3
    DefineMethod v2, v3 layout=L0 side=Static key="call"
    v5 = NewClosure v0 function=f4
    DefineMethod v2, v5 layout=L0 side=Static index=1 key="apply"
    v7 = NewClosure v0 function=f5
    DefineMethod v2, v7 layout=L0 side=Static index=2 key="bind"
    v9 = NewClosure v0 function=f6
    DefineMethod v2, v9 layout=L0 side=Static index=3 key="toString"
    v11 = FinishClass v2 layout=L0
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    Return v12
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="call"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f4 name="apply" module=m0 position=79 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="apply"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f5 name="bind" module=m0 position=124 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="bind"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f6 name="toString" module=m0 position=167 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Convert v2 declared={type=Array<anything> flags=[IsFromCompiler, IsClaim]}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f8
runtimefunction id=12 function=f7
)IR"_s;

static constexpr ASCIILiteral staticMethodsOfClassEvaluatedInLoop = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Integer
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v3)
  b2(v4):
    LoopHint
    v6 = Integer integer=3
    v7 = Less v4, v6
    Branch v7 -> taken b3(| 0), notTaken b6(| 0)
  b3(v8):
    Jump -> next b4()
  b4():
    v9 = Convert v1 declared={type=function flags=[IsExpected]}
    v10 = String string="t"
    v11 = Add v10, v8
    v12 = Call v9, -, v11 calleeName="make"
    v13 = Convert v12 declared={type=function flags=[IsExpected]}
    v14 = Builtin builtin=GlobalThis
    v15 = PropGet v14 key="print" [isFreeName]
    v16 = PropGet v13 key="call"
    v17 = Convert v16 declared={type=function flags=[IsExpected]}
    v18 = Call v17, v13, v8 calleeKey="call" calleeName="call"
    v19 = Convert v18 declared={type=string flags=[IsExpected]}
    v20 = PropGet v13 key="apply"
    v21 = Convert v20 declared={type=function flags=[IsExpected]}
    v22 = Call v21, v13, v8 calleeKey="apply" calleeName="apply"
    v23 = Convert v22 declared={type=string flags=[IsExpected]}
    v24 = PropGet v13 key="bind"
    v25 = Convert v24 declared={type=function flags=[IsExpected]}
    v26 = Call v25, v13, v8 calleeKey="bind" calleeName="bind"
    v27 = Convert v26 declared={type=string flags=[IsExpected]}
    v28 = PropGet v13 key="toString"
    v29 = Convert v28 declared={type=function flags=[IsExpected]}
    v30 = Call v29, v13 calleeKey="toString" calleeName="toString"
    v31 = ToString v13
    v32 = Call v15, -, v19, v23, v27, v30, v31 calleeName="print"
    Jump -> next b5()
  b5():
    v33 = Increment v8
    v34 = Convert v33 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v34)
  b6(v35):
    Jump -> next b7()
  b7():
    v36 = Undefined
    Return v36
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f2
    v2 = NewClass v1 layout=L0
    v3 = NewClosure v0 function=f3
    DefineMethod v2, v3 layout=L0 side=Static key="call"
    v5 = NewClosure v0 function=f4
    DefineMethod v2, v5 layout=L0 side=Static index=1 key="apply"
    v7 = NewClosure v0 function=f5
    DefineMethod v2, v7 layout=L0 side=Static index=2 key="bind"
    v9 = NewClosure v0 function=f6
    DefineMethod v2, v9 layout=L0 side=Static index=3 key="toString"
    v11 = FinishClass v2 layout=L0
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    Return v12
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="call"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f4 name="apply" module=m0 position=79 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="apply"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f5 name="bind" module=m0 position=124 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="bind"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
function f6 name="toString" module=m0 position=167 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Convert v2 declared={type=Array<anything> flags=[IsFromCompiler, IsClaim]}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f8
runtimefunction id=12 function=f7
)IR"_s;

static constexpr ASCIILiteral staticAccessorsNameAndLength = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = String string="o"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = PropGet v5 key="name"
    v9 = Convert v8 declared={type=string flags=[IsExpected]}
    v10 = PropGet v5 key="length"
    v11 = Convert v10 declared={type=string flags=[IsExpected]}
    v12 = Call v7, -, v9, v11 calleeName="print"
    v13 = String string="a"
    v14 = Call v2, -, v13 calleeName="make"
    v15 = Convert v14 declared={type=function flags=[IsExpected]}
    v16 = String string="b"
    v17 = Call v2, -, v16 calleeName="make"
    v18 = Convert v17 declared={type=function flags=[IsExpected]}
    v19 = Builtin builtin=GlobalThis
    v20 = PropGet v19 key="print" [isFreeName]
    v21 = PropGet v15 key="name"
    v22 = Convert v21 declared={type=string flags=[IsExpected]}
    v23 = PropGet v18 key="name"
    v24 = Convert v23 declared={type=string flags=[IsExpected]}
    v25 = PropGet v15 key="length"
    v26 = Convert v25 declared={type=string flags=[IsExpected]}
    v27 = PropGet v18 key="length"
    v28 = Convert v27 declared={type=string flags=[IsExpected]}
    v29 = Call v20, -, v22, v24, v26, v28 calleeName="print"
    v30 = Integer
    v31 = Convert v30 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v31)
  b2(v32):
    LoopHint
    v34 = Integer integer=2
    v35 = Less v32, v34
    Branch v35 -> taken b3(| 0), notTaken b6(| 0)
  b3(v36):
    Jump -> next b4()
  b4():
    v37 = String string="t"
    v38 = Add v37, v36
    v39 = Call v2, -, v38 calleeName="make"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Builtin builtin=GlobalThis
    v42 = PropGet v41 key="print" [isFreeName]
    v43 = PropGet v40 key="name"
    v44 = Convert v43 declared={type=string flags=[IsExpected]}
    v45 = PropGet v40 key="length"
    v46 = Convert v45 declared={type=string flags=[IsExpected]}
    v47 = Call v42, -, v44, v46 calleeName="print"
    Jump -> next b5()
  b5():
    v48 = Increment v36
    v49 = Convert v48 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v49)
  b6(v50):
    Jump -> next b7()
  b7():
    v51 = Undefined
    Return v51
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f2
    v2 = NewClass v1 layout=L0
    v3 = NewClosure v0 function=f3
    DefineMethod v2, v3 layout=L0 side=Static methodKind=Getter key="name"
    v5 = NewClosure v0 function=f4
    DefineMethod v2, v5 layout=L0 side=Static methodKind=Getter index=1 key="length"
    v7 = FinishClass v2 layout=L0
    v8 = Convert v7 declared={type=function flags=[IsExpected]}
    Return v8
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="name" module=m0 position=36 kind=Getter flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = String string="name"
    v2 = Add v0, v1
    v3 = Convert v2 declared={type=string flags=[IsExpected]}
    Return v3
}
function f4 name="length" module=m0 position=78 kind=Getter flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = String string="length"
    v2 = Add v0, v1
    v3 = Convert v2 declared={type=string flags=[IsExpected]}
    Return v3
}
entry module=m0
function f5 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f6 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Convert v2 declared={type=Array<anything> flags=[IsFromCompiler, IsClaim]}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f6
runtimefunction id=12 function=f5
)IR"_s;

static constexpr ASCIILiteral ownAndInheritedMembersAtOneSite = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f3
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = NewClosure function=f2
    v3 = Convert v2 declared={type=function flags=[IsExpected]}
    v4 = Convert v1 declared={type=function flags=[IsExpected]}
    v5 = String string="a"
    v6 = Call v4, -, v5 calleeName="make"
    v7 = Convert v6 declared={type=function flags=[IsExpected]}
    v8 = Convert v3 declared={type=function flags=[IsExpected]}
    v9 = String string="b"
    v10 = Call v4, -, v9 calleeName="make"
    v11 = Convert v10 declared={type=function flags=[IsExpected]}
    v12 = NewArray v7, v8, v11, v8 contents=anything
    v13 = Convert v12 declared={type=Array<function> flags=[IsExpected]}
    v14 = Integer
    v15 = Convert v14 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v15)
  b2(v16):
    LoopHint
    v18 = Integer integer=4
    v19 = Less v16, v18
    Branch v19 -> taken b3(| 0), notTaken b6(| 0)
  b3(v20):
    Jump -> next b4()
  b4():
    v21 = Builtin builtin=GlobalThis
    v22 = PropGet v21 key="print" [isFreeName]
    v23 = ElemGet v13, v20
    v24 = Convert v23 declared={type=function flags=[IsExpected]}
    v25 = PropGet v24 key="call"
    v26 = Convert v25 declared={type=function flags=[IsExpected]}
    v27 = Integer integer=1
    v28 = Integer integer=2
    v29 = Integer integer=3
    v30 = Call v26, v24, v27, v28, v29 calleeKey="call" calleeName="call"
    v31 = ElemGet v13, v20
    v32 = Convert v31 declared={type=function flags=[IsExpected]}
    v33 = PropGet v32 key="apply"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = Integer integer=1
    v36 = Data data=d0
    v37 = Convert v36 declared={type=Array<number> flags=[IsExpected]}
    v38 = Call v34, v32, v35, v37 calleeKey="apply" calleeName="apply"
    v39 = ElemGet v13, v20
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = PropGet v40 key="bind"
    v42 = Convert v41 declared={type=function flags=[IsExpected]}
    v43 = Integer integer=1
    v44 = Integer integer=2
    v45 = Integer integer=3
    v46 = Call v42, v40, v43, v44, v45 calleeKey="bind" calleeName="bind"
    v47 = Convert v46 declared={type=function flags=[IsExpected]}
    v48 = Call v47
    v49 = Call v22, -, v30, v38, v48 calleeName="print"
    Jump -> next b5()
  b5():
    v50 = Increment v20
    v51 = Convert v50 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v51)
  b6(v52):
    Jump -> next b7()
  b7():
    v53 = Undefined
    Return v53
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f3
    v2 = NewClass v1 layout=L0
    v3 = NewClosure v0 function=f4
    DefineMethod v2, v3 layout=L0 side=Static key="call"
    v5 = NewClosure v0 function=f5
    DefineMethod v2, v5 layout=L0 side=Static index=1 key="apply"
    v7 = NewClosure v0 function=f6
    DefineMethod v2, v7 layout=L0 side=Static index=2 key="bind"
    v9 = FinishClass v2 layout=L0
    v10 = Convert v9 declared={type=function flags=[IsExpected]}
    Return v10
}
function f2 name="plain" module=m0 position=163 flags=[HasThis, HasExplicitName] length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v3 = String string="plain"
    v4 = Add v3, v0
    v5 = Add v4, v1
    v6 = Add v5, v2
    v7 = Convert v6 declared={type=string flags=[IsExpected]}
    Return v7
}
function f3 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f4 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
function f5 name="apply" module=m0 position=70 kind=Method flags=[HasExplicitName] length=2 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v3 = Add v0, v1
    v4 = PropGet v2 key="length"
    v5 = Add v3, v4
    Return v5
}
function f6 name="bind" module=m0 position=119 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = NewClosure v0, v1 function=f7
    v3 = Convert v2 declared={type=function flags=[IsExpected]}
    Return v3
}
function f7 module=m0 position=143 kind=Arrow captures=[{type=anything}, {type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
data d0 root=Array{contents=anything elements=[Number{value=2}, Number{value=3}]} module=m0
entry module=m0
function f8 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f9 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Convert v2 declared={type=Array<anything> flags=[IsFromCompiler, IsClaim]}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f9
runtimefunction id=12 function=f8
)IR"_s;

static constexpr ASCIILiteral classWithoutTheMember = R"IR(layout L0 kind=Instance module=m0 position=39 constructor=f2
layout L1 kind=Instance module=m0 position=85 constructor=f4
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = String string="a"
    v4 = Boolean boolean=true
    v5 = Call v2, -, v3, v4 calleeName="make"
    v6 = Convert v5 declared={type=function flags=[IsExpected]}
    v7 = String string="b"
    v8 = Boolean
    v9 = Call v2, -, v7, v8 calleeName="make"
    v10 = Convert v9 declared={type=function flags=[IsExpected]}
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="print" [isFreeName]
    v13 = PropGet v6 key="call"
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Integer integer=1
    v16 = Call v14, v6, v15 calleeKey="call" calleeName="call"
    v17 = Call v12, -, v16 calleeName="print"
    Jump -> next b2()
  b2() handler=b4():
    v18 = Builtin builtin=GlobalThis
    v19 = PropGet v18 key="print" [isFreeName]
    v20 = PropGet v10 key="call"
    v21 = Convert v20 declared={type=function flags=[IsExpected]}
    v22 = Integer integer=2
    v23 = Call v21, v10, v22 calleeKey="call" calleeName="call"
    v24 = Call v19, -, v23 calleeName="print"
    Jump -> next b3()
  b3():
    Jump -> next b5()
  b4() role=CatchHandler:
    v25 = CatchValue
    v26 = Builtin builtin=GlobalThis
    v27 = PropGet v26 key="print" [isFreeName]
    v28 = Builtin builtin=GlobalThis
    v29 = PropGet v28 key="TypeError" [isFreeName]
    v30 = Convert v29 declared={type=builtin(TypeError) flags=[IsExpected]}
    v31 = InstanceOf v25, v30
    v32 = Call v27, -, v31 calleeName="print"
    Jump -> next b5()
  b5():
    v33 = Builtin builtin=GlobalThis
    v34 = PropGet v33 key="print" [isFreeName]
    v35 = PropGet v10 key="other"
    v36 = Integer integer=3
    v37 = Call v35, v10, v36 calleeKey="other" calleeName="other"
    v38 = Call v34, -, v37 calleeName="print"
    v39 = Undefined
    Return v39
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = ToBoolean v1
    Branch v2 -> taken b2(| 0), notTaken b4(| 0)
  b2(v3):
    Jump -> next b3()
  b3():
    v4 = NewClosure function=f2
    v5 = NewClass v4 layout=L0
    v6 = NewClosure v0 function=f3
    DefineMethod v5, v6 layout=L0 side=Static key="call"
    v8 = FinishClass v5 layout=L0
    Jump -> next b6(v8)
  b4(v9):
    Jump -> next b5()
  b5():
    v10 = NewClosure function=f4
    v11 = NewClass v10 layout=L1
    v12 = NewClosure v0 function=f5
    DefineMethod v11, v12 layout=L1 side=Static key="other"
    v14 = FinishClass v11 layout=L1
    Jump -> next b6(v14)
  b6(v15):
    v16 = Convert v15 declared={type=function flags=[IsExpected]}
    Return v16
}
function f2 module=m0 position=39 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=47 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
function f4 module=m0 position=85 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f5 name="other" module=m0 position=93 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
entry module=m0
function f6 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f7 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Convert v2 declared={type=Array<anything> flags=[IsFromCompiler, IsClaim]}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f7
runtimefunction id=12 function=f6
)IR"_s;

static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral optionWords[] = {
        "validate"_s,
        "check=VerifyEachPass"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:ResolveCalls"_s,
        "heat=EverythingIsHot"_s,
        "references=narrow"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    for (ASCIILiteral word : optionWords)
        checkOutputOfProgramInShell(programText, expectedOutput, { word });
}

static void testStaticMethodsOfClassEvaluatedOnce()
{
    checkUnderEveryOption(staticMethodsOfClassEvaluatedOnce, "acall1 aapply2 abind3 a a\nfalse acall4\n"_s);
}

static void testStaticMethodsOfClassEvaluatedTwice()
{
    checkUnderEveryOption(staticMethodsOfClassEvaluatedTwice, "acall1 bcall2 aapply3 bapply4 abind5 bbind6 a b\nfalse false true false\nacall7 ccall8\n"_s);
}

static void testStaticMethodsOfClassEvaluatedInLoop()
{
    checkUnderEveryOption(staticMethodsOfClassEvaluatedInLoop, "t0call0 t0apply0 t0bind0 t0 t0\nt1call1 t1apply1 t1bind1 t1 t1\nt2call2 t2apply2 t2bind2 t2 t2\n"_s);
}

static void testStaticAccessorsNameAndLength()
{
    checkUnderEveryOption(staticAccessorsNameAndLength, "oname olength\naname bname alength blength\nt0name t0length\nt1name t1length\n"_s);
}

static void testOwnAndInheritedMembersAtOneSite()
{
    checkUnderEveryOption(ownAndInheritedMembersAtOneSite, "a1 a12 a1\nplain123 plain123 plain123\nb1 b12 b1\nplain123 plain123 plain123\n"_s);
}

static void testClassWithoutTheMember()
{
    checkUnderEveryOption(classWithoutTheMember, "a1\ntrue\nb3\n"_s);
}

std::span<const NativeTest> staticMembersNamedLikeBuiltinsTests()
{
    static constexpr NativeTest tests[] = {
        { "static-members-named-like-builtins/static-methods-of-class-evaluated-once"_s, testStaticMethodsOfClassEvaluatedOnce },
        { "static-members-named-like-builtins/static-methods-of-class-evaluated-twice"_s, testStaticMethodsOfClassEvaluatedTwice },
        { "static-members-named-like-builtins/static-methods-of-class-evaluated-in-loop"_s, testStaticMethodsOfClassEvaluatedInLoop },
        { "static-members-named-like-builtins/static-accessors-name-and-length"_s, testStaticAccessorsNameAndLength },
        { "static-members-named-like-builtins/own-and-inherited-members-at-one-site"_s, testOwnAndInheritedMembersAtOneSite },
        { "static-members-named-like-builtins/class-without-the-member"_s, testClassWithoutTheMember },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
