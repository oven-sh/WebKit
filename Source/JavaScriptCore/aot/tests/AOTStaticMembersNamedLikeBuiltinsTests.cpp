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
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = Predict v1 predicted=function
    v3 = String string="a"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Predict v4 predicted=function
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = PropGet v5 key="call"
    v9 = Predict v8 predicted=function
    v10 = Integer integer=1
    v11 = Call v9, v5, v10 calleeKey="call" calleeName="call"
    v12 = Predict v11 predicted=string
    v13 = PropGet v5 key="apply"
    v14 = Predict v13 predicted=function
    v15 = Integer integer=2
    v16 = Call v14, v5, v15 calleeKey="apply" calleeName="apply"
    v17 = Predict v16 predicted=string
    v18 = PropGet v5 key="bind"
    v19 = Predict v18 predicted=function
    v20 = Integer integer=3
    v21 = Call v19, v5, v20 calleeKey="bind" calleeName="bind"
    v22 = Predict v21 predicted=string
    v23 = PropGet v5 key="toString"
    v24 = Predict v23 predicted=function
    v25 = Call v24, v5 calleeKey="toString" calleeName="toString"
    v26 = ToString v5
    v27 = Call v7, -, v12, v17, v22, v25, v26 calleeName="print"
    v28 = Builtin builtin=GlobalThis
    v29 = PropGet v28 key="print" [isFreeName]
    v30 = PropGet v5 key="call"
    v31 = Predict v30 predicted=function
    v32 = Builtin builtin=GlobalThis
    v33 = PropGet v32 key="Function" [isFreeName]
    v34 = Predict v33 predicted=builtin(Function)
    v35 = PropGet v34 key="prototype"
    v36 = Predict v35 predicted=builtin(FunctionPrototype)
    v37 = PropGet v36 key="call"
    v38 = Predict v37 predicted=builtin(FunctionPrototypeCall)
    v39 = StrictEq v31, v38
    v40 = Builtin builtin=GlobalThis
    v41 = PropGet v40 key="Function" [isFreeName]
    v42 = Predict v41 predicted=builtin(Function)
    v43 = PropGet v42 key="prototype"
    v44 = Predict v43 predicted=builtin(FunctionPrototype)
    v45 = PropGet v44 key="call"
    v46 = Predict v45 predicted=builtin(FunctionPrototypeCall)
    v47 = PropGet v46 key="call"
    v48 = Predict v47 predicted=function
    v49 = PropGet v5 key="call"
    v50 = Predict v49 predicted=function
    v51 = Null
    v52 = Integer integer=4
    v53 = Call v48, v46, v50, v51, v52 calleeKey="call" calleeName="call"
    v54 = Call v29, -, v39, v53 calleeName="print"
    v55 = Undefined
    Return v55
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything):
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
    v12 = ClaimDeclared v11 declared={type=anything predicted=function}
    Return v12
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="call"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f4 name="apply" module=m0 position=79 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="apply"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f5 name="bind" module=m0 position=124 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="bind"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f6 name="toString" module=m0 position=167 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Return v0
}
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: Array<anything>):
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = ClaimDeclared v2 declared={type=Array<anything>}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f8
runtimefunction id=12 function=f7
)IR"_s;

static constexpr ASCIILiteral staticMethodsOfClassEvaluatedTwice = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = Predict v1 predicted=function
    v3 = String string="a"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Predict v4 predicted=function
    v6 = String string="b"
    v7 = Call v2, -, v6 calleeName="make"
    v8 = Predict v7 predicted=function
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = PropGet v5 key="call"
    v12 = Predict v11 predicted=function
    v13 = Integer integer=1
    v14 = Call v12, v5, v13 calleeKey="call" calleeName="call"
    v15 = Predict v14 predicted=string
    v16 = PropGet v8 key="call"
    v17 = Predict v16 predicted=function
    v18 = Integer integer=2
    v19 = Call v17, v8, v18 calleeKey="call" calleeName="call"
    v20 = Predict v19 predicted=string
    v21 = PropGet v5 key="apply"
    v22 = Predict v21 predicted=function
    v23 = Integer integer=3
    v24 = Call v22, v5, v23 calleeKey="apply" calleeName="apply"
    v25 = Predict v24 predicted=string
    v26 = PropGet v8 key="apply"
    v27 = Predict v26 predicted=function
    v28 = Integer integer=4
    v29 = Call v27, v8, v28 calleeKey="apply" calleeName="apply"
    v30 = Predict v29 predicted=string
    v31 = PropGet v5 key="bind"
    v32 = Predict v31 predicted=function
    v33 = Integer integer=5
    v34 = Call v32, v5, v33 calleeKey="bind" calleeName="bind"
    v35 = Predict v34 predicted=string
    v36 = PropGet v8 key="bind"
    v37 = Predict v36 predicted=function
    v38 = Integer integer=6
    v39 = Call v37, v8, v38 calleeKey="bind" calleeName="bind"
    v40 = Predict v39 predicted=string
    v41 = PropGet v5 key="toString"
    v42 = Predict v41 predicted=function
    v43 = Call v42, v5 calleeKey="toString" calleeName="toString"
    v44 = ToString v8
    v45 = Call v10, -, v15, v20, v25, v30, v35, v40, v43, v44 calleeName="print"
    v46 = Builtin builtin=GlobalThis
    v47 = PropGet v46 key="print" [isFreeName]
    v48 = PropGet v5 key="call"
    v49 = Predict v48 predicted=function
    v50 = PropGet v8 key="call"
    v51 = Predict v50 predicted=function
    v52 = StrictEq v49, v51
    v53 = String string="a"
    v54 = Call v2, -, v53 calleeName="make"
    v55 = Predict v54 predicted=function
    v56 = PropGet v55 key="call"
    v57 = Predict v56 predicted=function
    v58 = String string="b"
    v59 = Call v2, -, v58 calleeName="make"
    v60 = Predict v59 predicted=function
    v61 = PropGet v60 key="call"
    v62 = Predict v61 predicted=function
    v63 = StrictEq v57, v62
    v64 = PropGet v5 key="call"
    v65 = Predict v64 predicted=function
    v66 = PropGet v5 key="call"
    v67 = Predict v66 predicted=function
    v68 = StrictEq v65, v67
    v69 = PropGet v5 key="call"
    v70 = Predict v69 predicted=function
    v71 = Builtin builtin=GlobalThis
    v72 = PropGet v71 key="Function" [isFreeName]
    v73 = Predict v72 predicted=builtin(Function)
    v74 = PropGet v73 key="prototype"
    v75 = Predict v74 predicted=builtin(FunctionPrototype)
    v76 = PropGet v75 key="call"
    v77 = Predict v76 predicted=builtin(FunctionPrototypeCall)
    v78 = StrictEq v70, v77
    v79 = Call v47, -, v52, v63, v68, v78 calleeName="print"
    v80 = Builtin builtin=GlobalThis
    v81 = PropGet v80 key="print" [isFreeName]
    v82 = Builtin builtin=GlobalThis
    v83 = PropGet v82 key="Function" [isFreeName]
    v84 = Predict v83 predicted=builtin(Function)
    v85 = PropGet v84 key="prototype"
    v86 = Predict v85 predicted=builtin(FunctionPrototype)
    v87 = PropGet v86 key="call"
    v88 = Predict v87 predicted=builtin(FunctionPrototypeCall)
    v89 = PropGet v88 key="call"
    v90 = Predict v89 predicted=function
    v91 = PropGet v5 key="call"
    v92 = Predict v91 predicted=function
    v93 = Null
    v94 = Integer integer=7
    v95 = Call v90, v88, v92, v93, v94 calleeKey="call" calleeName="call"
    v96 = Builtin builtin=GlobalThis
    v97 = PropGet v96 key="Function" [isFreeName]
    v98 = Predict v97 predicted=builtin(Function)
    v99 = PropGet v98 key="prototype"
    v100 = Predict v99 predicted=builtin(FunctionPrototype)
    v101 = PropGet v100 key="call"
    v102 = Predict v101 predicted=builtin(FunctionPrototypeCall)
    v103 = PropGet v102 key="call"
    v104 = Predict v103 predicted=function
    v105 = String string="c"
    v106 = Call v2, -, v105 calleeName="make"
    v107 = Predict v106 predicted=function
    v108 = PropGet v107 key="call"
    v109 = Predict v108 predicted=function
    v110 = Null
    v111 = Integer integer=8
    v112 = Call v104, v102, v109, v110, v111 calleeKey="call" calleeName="call"
    v113 = Call v81, -, v95, v112 calleeName="print"
    v114 = Undefined
    Return v114
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything):
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
    v12 = ClaimDeclared v11 declared={type=anything predicted=function}
    Return v12
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="call"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f4 name="apply" module=m0 position=79 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="apply"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f5 name="bind" module=m0 position=124 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="bind"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f6 name="toString" module=m0 position=167 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Return v0
}
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: Array<anything>):
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = ClaimDeclared v2 declared={type=Array<anything>}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f8
runtimefunction id=12 function=f7
)IR"_s;

static constexpr ASCIILiteral staticMethodsOfClassEvaluatedInLoop = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = Integer
    v3 = Predict v2 predicted=number
    Jump -> next b2(v3)
  b2(v4):
    LoopHint
    v6 = Integer integer=3
    v7 = Less v4, v6
    Branch v7 -> taken b3(| 0), notTaken b6(| 0)
  b3(v8):
    Jump -> next b4()
  b4():
    v9 = Predict v1 predicted=function
    v10 = String string="t"
    v11 = Add v10, v8
    v12 = Call v9, -, v11 calleeName="make"
    v13 = Predict v12 predicted=function
    v14 = Builtin builtin=GlobalThis
    v15 = PropGet v14 key="print" [isFreeName]
    v16 = PropGet v13 key="call"
    v17 = Predict v16 predicted=function
    v18 = Call v17, v13, v8 calleeKey="call" calleeName="call"
    v19 = Predict v18 predicted=string
    v20 = PropGet v13 key="apply"
    v21 = Predict v20 predicted=function
    v22 = Call v21, v13, v8 calleeKey="apply" calleeName="apply"
    v23 = Predict v22 predicted=string
    v24 = PropGet v13 key="bind"
    v25 = Predict v24 predicted=function
    v26 = Call v25, v13, v8 calleeKey="bind" calleeName="bind"
    v27 = Predict v26 predicted=string
    v28 = PropGet v13 key="toString"
    v29 = Predict v28 predicted=function
    v30 = Call v29, v13 calleeKey="toString" calleeName="toString"
    v31 = ToString v13
    v32 = Call v15, -, v19, v23, v27, v30, v31 calleeName="print"
    Jump -> next b5()
  b5():
    v33 = Increment v8
    v34 = ClaimDeclared v33 declared={type=anything predicted=number}
    Jump -> next b2(v34)
  b6(v35):
    Jump -> next b7()
  b7():
    v36 = Undefined
    Return v36
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything):
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
    v12 = ClaimDeclared v11 declared={type=anything predicted=function}
    Return v12
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="call"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f4 name="apply" module=m0 position=79 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="apply"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f5 name="bind" module=m0 position=124 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="bind"
    v3 = Add v0, v2
    v4 = Add v3, v1
    v5 = ClaimDeclared v4 declared={type=anything predicted=string}
    Return v5
}
function f6 name="toString" module=m0 position=167 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Return v0
}
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: Array<anything>):
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = ClaimDeclared v2 declared={type=Array<anything>}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f8
runtimefunction id=12 function=f7
)IR"_s;

static constexpr ASCIILiteral staticAccessorsNameAndLength = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f2
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = Predict v1 predicted=function
    v3 = String string="o"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Predict v4 predicted=function
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = PropGet v5 key="name"
    v9 = Predict v8 predicted=string
    v10 = PropGet v5 key="length"
    v11 = Predict v10 predicted=string
    v12 = Call v7, -, v9, v11 calleeName="print"
    v13 = String string="a"
    v14 = Call v2, -, v13 calleeName="make"
    v15 = Predict v14 predicted=function
    v16 = String string="b"
    v17 = Call v2, -, v16 calleeName="make"
    v18 = Predict v17 predicted=function
    v19 = Builtin builtin=GlobalThis
    v20 = PropGet v19 key="print" [isFreeName]
    v21 = PropGet v15 key="name"
    v22 = Predict v21 predicted=string
    v23 = PropGet v18 key="name"
    v24 = Predict v23 predicted=string
    v25 = PropGet v15 key="length"
    v26 = Predict v25 predicted=string
    v27 = PropGet v18 key="length"
    v28 = Predict v27 predicted=string
    v29 = Call v20, -, v22, v24, v26, v28 calleeName="print"
    v30 = Integer
    v31 = Predict v30 predicted=number
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
    v40 = Predict v39 predicted=function
    v41 = Builtin builtin=GlobalThis
    v42 = PropGet v41 key="print" [isFreeName]
    v43 = PropGet v40 key="name"
    v44 = Predict v43 predicted=string
    v45 = PropGet v40 key="length"
    v46 = Predict v45 predicted=string
    v47 = Call v42, -, v44, v46 calleeName="print"
    Jump -> next b5()
  b5():
    v48 = Increment v36
    v49 = ClaimDeclared v48 declared={type=anything predicted=number}
    Jump -> next b2(v49)
  b6(v50):
    Jump -> next b7()
  b7():
    v51 = Undefined
    Return v51
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f2
    v2 = NewClass v1 layout=L0
    v3 = NewClosure v0 function=f3
    DefineMethod v2, v3 layout=L0 side=Static methodKind=Getter key="name"
    v5 = NewClosure v0 function=f4
    DefineMethod v2, v5 layout=L0 side=Static methodKind=Getter index=1 key="length"
    v7 = FinishClass v2 layout=L0
    v8 = ClaimDeclared v7 declared={type=anything predicted=function}
    Return v8
}
function f2 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="name" module=m0 position=36 kind=Getter flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything predicted=string}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = String string="name"
    v2 = Add v0, v1
    v3 = ClaimDeclared v2 declared={type=anything predicted=string}
    Return v3
}
function f4 name="length" module=m0 position=78 kind=Getter flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything predicted=string}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = String string="length"
    v2 = Add v0, v1
    v3 = ClaimDeclared v2 declared={type=anything predicted=string}
    Return v3
}
entry module=m0
function f5 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: Array<anything>):
    v3 = CallList v0, v1, v2
    Return v3
}
function f6 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = ClaimDeclared v2 declared={type=Array<anything>}
    v4 = CallList v0, v1, v3
    Return v4
}
runtimefunction id=4 function=f6
runtimefunction id=12 function=f5
)IR"_s;

static constexpr ASCIILiteral ownAndInheritedMembersAtOneSite = R"IR(layout L0 kind=Instance module=m0 position=28 constructor=f3
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = NewClosure function=f2
    v3 = ClaimDeclared v2 declared={type=anything predicted=function}
    v4 = Predict v1 predicted=function
    v5 = String string="a"
    v6 = Call v4, -, v5 calleeName="make"
    v7 = Predict v6 predicted=function
    v8 = Predict v3 predicted=function
    v9 = String string="b"
    v10 = Call v4, -, v9 calleeName="make"
    v11 = Predict v10 predicted=function
    v12 = NewArray v7, v8, v11, v8 contents=anything
    v13 = Predict v12 predicted=Array<function>
    v14 = Integer
    v15 = Predict v14 predicted=number
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
    v24 = Predict v23 predicted=function
    v25 = PropGet v24 key="call"
    v26 = Predict v25 predicted=function
    v27 = Integer integer=1
    v28 = Integer integer=2
    v29 = Integer integer=3
    v30 = Call v26, v24, v27, v28, v29 calleeKey="call" calleeName="call"
    v31 = ElemGet v13, v20
    v32 = Predict v31 predicted=function
    v33 = PropGet v32 key="apply"
    v34 = Predict v33 predicted=function
    v35 = Integer integer=1
    v36 = Data data=d0
    v37 = Predict v36 predicted=Array<number>
    v38 = Call v34, v32, v35, v37 calleeKey="apply" calleeName="apply"
    v39 = ElemGet v13, v20
    v40 = Predict v39 predicted=function
    v41 = PropGet v40 key="bind"
    v42 = Predict v41 predicted=function
    v43 = Integer integer=1
    v44 = Integer integer=2
    v45 = Integer integer=3
    v46 = Call v42, v40, v43, v44, v45 calleeKey="bind" calleeName="bind"
    v47 = Predict v46 predicted=function
    v48 = Call v47
    v49 = Call v22, -, v30, v38, v48 calleeName="print"
    Jump -> next b5()
  b5():
    v50 = Increment v20
    v51 = ClaimDeclared v50 declared={type=anything predicted=number}
    Jump -> next b2(v51)
  b6(v52):
    Jump -> next b7()
  b7():
    v53 = Undefined
    Return v53
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything):
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
    v10 = ClaimDeclared v9 declared={type=anything predicted=function}
    Return v10
}
function f2 name="plain" module=m0 position=163 flags=[HasThis, HasExplicitName] length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything predicted=string}] {
  b0(v0: anything, v1: anything, v2: anything):
    Jump -> next b1()
  b1():
    v3 = String string="plain"
    v4 = Add v3, v0
    v5 = Add v4, v1
    v6 = Add v5, v2
    v7 = ClaimDeclared v6 declared={type=anything predicted=string}
    Return v7
}
function f3 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f4 name="call" module=m0 position=36 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
function f5 name="apply" module=m0 position=70 kind=Method flags=[HasExplicitName] length=2 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    Jump -> next b1()
  b1():
    v3 = Add v0, v1
    v4 = PropGet v2 key="length"
    v5 = Add v3, v4
    Return v5
}
function f6 name="bind" module=m0 position=119 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = NewClosure v0, v1 function=f7
    v3 = ClaimDeclared v2 declared={type=anything predicted=function}
    Return v3
}
function f7 module=m0 position=143 kind=Arrow captures=[{type=anything}, {type=anything}] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
data d0 root=Array{contents=anything elements=[Number{value=2}, Number{value=3}]} module=m0
entry module=m0
function f8 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: Array<anything>):
    v3 = CallList v0, v1, v2
    Return v3
}
function f9 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = ClaimDeclared v2 declared={type=Array<anything>}
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
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = Predict v1 predicted=function
    v3 = String string="a"
    v4 = Boolean boolean=true
    v5 = Call v2, -, v3, v4 calleeName="make"
    v6 = Predict v5 predicted=function
    v7 = String string="b"
    v8 = Boolean
    v9 = Call v2, -, v7, v8 calleeName="make"
    v10 = Predict v9 predicted=function
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="print" [isFreeName]
    v13 = PropGet v6 key="call"
    v14 = Predict v13 predicted=function
    v15 = Integer integer=1
    v16 = Call v14, v6, v15 calleeKey="call" calleeName="call"
    v17 = Call v12, -, v16 calleeName="print"
    Jump -> next b2()
  b2() handler=b4():
    v18 = Builtin builtin=GlobalThis
    v19 = PropGet v18 key="print" [isFreeName]
    v20 = PropGet v10 key="call"
    v21 = Predict v20 predicted=function
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
    v30 = Predict v29 predicted=builtin(TypeError)
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
function f1 name="make" module=m0 flags=[HasExplicitName] length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything predicted=function}] {
  b0(v0: anything, v1: anything):
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
    v16 = ClaimDeclared v15 declared={type=anything predicted=function}
    Return v16
}
function f2 module=m0 position=39 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="call" module=m0 position=47 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
function f4 module=m0 position=85 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f5 name="other" module=m0 position=93 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
entry module=m0
function f6 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=12 thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: Array<anything>):
    v3 = CallList v0, v1, v2
    Return v3
}
function f7 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=4 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = ClaimDeclared v2 declared={type=Array<anything>}
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
