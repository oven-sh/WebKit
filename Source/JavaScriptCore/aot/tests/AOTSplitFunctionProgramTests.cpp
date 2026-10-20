/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// class Rec { constructor(name, n) { this.name = name; this.n = n } }  const spare = new Rec("spare", 0); let left = 0, fresh = false
// function a() { if (left === 0) return "a()"; left--; return b(fresh ? new Rec("f", left) : spare, fresh ? "s" + left : "s", left + 0.5) }
// function b(p0, p1, p2) { if (left === 0) return "b(" + p0.name + p0.n + "," + p1 + "," + p2 + ")"; left--; return a() }
// for (const [hops, makes] of [[1, false], [3, true], [5, false], [1001, true]]) { left = hops; fresh = makes; print(a()) }
// The new Rec is sunk: behind the merge there is a state, and a cell that is Unspecified where the object was not built. SplitFunctionsAtCalls cuts behind the merge.
static constexpr ASCIILiteral unspecifiedCellBehindMerge = R"IR(layout L0 kind=Instance name="Rec" constructor=f3 fields=[{key="name" declared={type=anything}}, {key="n" declared={type=anything}}]
variable g0 name="a" declared={type=function} initialization=Function function=f1 flags=[IsImmutable]
variable g1 name="b" declared={type=function} initialization=Function function=f2 flags=[IsImmutable]
variable g2 name="Rec" declared={type=function|absent} flags=[IsImmutable]
variable g3 name="spare" declared={type=anything|absent predicted=L0} flags=[IsImmutable]
variable g4 name="left" declared={type=anything|absent predicted=number}
variable g5 name="fresh" declared={type=anything|absent predicted=boolean}
variable g6 declared={type=function} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f3
    v1 = NewClass v0 layout=L0
    PutModuleVariable v1 variable=g6
    v3 = FinishClass v1 layout=L0
    v4 = ClaimDeclared v3 declared={type=anything predicted=function}
    v5 = ClaimDeclared v3 declared={type=anything predicted=function}
    PutModuleVariable v5 variable=g2
    v7 = String string="spare"
    v8 = Integer
    v9 = Construct v5, -, v7, v8 calleeName="Rec"
    v10 = Predict v9 predicted=L0
    PutModuleVariable v10 variable=g3
    v12 = Integer
    v13 = Predict v12 predicted=number
    PutModuleVariable v13 variable=g4
    v15 = Boolean
    v16 = Predict v15 predicted=boolean
    PutModuleVariable v16 variable=g5
    v18 = Data data=d0
    v19 = Predict v18 predicted=Array<Array<boolean|number>>
    v20 = IteratorOpen v19
    Jump -> next b2()
  b2():
    LoopHint
    v22 = IteratorResume v20
    v23 = ResultAt v22
    v24 = ResultAt v22 index=1
    Branch v23 -> taken b3(), notTaken b4()
  b3():
    v25 = Undefined
    Return v25
  b4():
    Jump -> next b5()
  b5() handler=b12():
    v26 = IteratorOpen v24 elementCount=2 [isDestructuring]
    Jump -> next b6()
  b6() handler=b11():
    v27 = IteratorResume v26 stepIndex=0
    v28 = ResultAt v27
    v29 = ResultAt v27 index=1
    v30 = Predict v29 predicted=boolean|number
    Jump -> next b7()
  b7() handler=b11():
    v31 = IteratorResume v26 stepIndex=1
    v32 = ResultAt v31
    v33 = ResultAt v31 index=1
    v34 = Predict v33 predicted=boolean|number
    Jump -> next b8()
  b8() handler=b11():
    Jump -> next b9()
  b9() handler=b12():
    IteratorClose v26
    Jump -> next b10()
  b10() handler=b12():
    v36 = GetModuleVariable variable=g4
    v37 = CheckInitialized v36 name="left"
    v38 = ClaimDeclared v30 declared={type=anything predicted=number}
    PutModuleVariable v38 variable=g4
    v40 = GetModuleVariable variable=g5
    v41 = CheckInitialized v40 name="fresh"
    v42 = ClaimDeclared v34 declared={type=anything predicted=boolean}
    PutModuleVariable v42 variable=g5
    v44 = Builtin builtin=GlobalThis
    v45 = PropGet v44 key="print" [isFreeName]
    v46 = GetModuleVariable variable=g0
    v47 = Predict v46 predicted=function
    v48 = Call v47 calleeName="a"
    v49 = Call v45, -, v48 calleeName="print"
    Jump -> next b2()
  b11() role=IterationCleanup subject=v26 handler=b12():
    v50 = CatchException
    IteratorClose v26 completion=Throw
    Rethrow v50
  b12() role=IterationCleanup subject=v20:
    v52 = CatchException
    IteratorClose v20 completion=Throw
    Rethrow v52
}
function f1 name="a" flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = GetModuleVariable variable=g4
    v1 = CheckInitialized v0 name="left"
    v2 = Integer
    v3 = StrictEq v1, v2
    Branch v3 -> taken b2(), notTaken b3()
  b2():
    v4 = String string="a()"
    Return v4
  b3():
    v5 = GetModuleVariable variable=g4
    v6 = CheckInitialized v5 name="left"
    v7 = Decrement v6
    v8 = ClaimDeclared v7 declared={type=anything predicted=number}
    v9 = GetModuleVariable variable=g4
    v10 = CheckInitialized v9 name="left"
    PutModuleVariable v8 variable=g4
    v12 = GetModuleVariable variable=g1
    v13 = Predict v12 predicted=function
    v14 = GetModuleVariable variable=g5
    v15 = CheckInitialized v14 name="fresh"
    v16 = ToBoolean v15
    Branch v16 -> taken b4(| 0), notTaken b6(| 0)
  b4(v17):
    Jump -> next b5()
  b5():
    v18 = GetModuleVariable variable=g2
    v19 = CheckInitialized v18 name="Rec"
    v20 = String string="f"
    v21 = GetModuleVariable variable=g4
    v22 = CheckInitialized v21 name="left"
    v23 = Construct v19, -, v20, v22 calleeName="Rec"
    v24 = Predict v23 predicted=L0
    Jump -> next b8(v24)
  b6(v25):
    Jump -> next b7()
  b7():
    v26 = GetModuleVariable variable=g3
    v27 = CheckInitialized v26 name="spare"
    Jump -> next b8(v27)
  b8(v28):
    v29 = GetModuleVariable variable=g5
    v30 = CheckInitialized v29 name="fresh"
    v31 = ToBoolean v30
    Branch v31 -> taken b9(| 0), notTaken b11(| 0)
  b9(v32):
    Jump -> next b10()
  b10():
    v33 = String string="s"
    v34 = GetModuleVariable variable=g4
    v35 = CheckInitialized v34 name="left"
    v36 = Add v33, v35
    Jump -> next b13(v36)
  b11(v37):
    Jump -> next b12()
  b12():
    v38 = String string="s"
    Jump -> next b13(v38)
  b13(v39):
    v40 = GetModuleVariable variable=g4
    v41 = CheckInitialized v40 name="left"
    v42 = Double number=0.5
    v43 = Add v41, v42
    v44 = Call v13, -, v28, v39, v43 calleeName="b"
    ReturnCall v44
}
function f2 name="b" flags=[HasExplicitName] length=3 thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    Jump -> next b1()
  b1():
    v3 = GetModuleVariable variable=g4
    v4 = CheckInitialized v3 name="left"
    v5 = Integer
    v6 = StrictEq v4, v5
    Branch v6 -> taken b2(), notTaken b3()
  b2():
    v7 = String string="b("
    v8 = PropGet v0 key="name"
    v9 = Add v7, v8
    v10 = PropGet v0 key="n"
    v11 = Add v9, v10
    v12 = String string=","
    v13 = Add v11, v12
    v14 = Add v13, v1
    v15 = String string=","
    v16 = Add v14, v15
    v17 = Add v16, v2
    v18 = String string=")"
    v19 = Add v17, v18
    Return v19
  b3():
    v20 = GetModuleVariable variable=g4
    v21 = CheckInitialized v20 name="left"
    v22 = Decrement v21
    v23 = ClaimDeclared v22 declared={type=anything predicted=number}
    v24 = GetModuleVariable variable=g4
    v25 = CheckInitialized v24 name="left"
    PutModuleVariable v23 variable=g4
    v27 = GetModuleVariable variable=g0
    v28 = Predict v27 predicted=function
    v29 = Call v28 calleeName="a"
    ReturnCall v29
}
function f3 name="Rec" kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything, v2: anything):
    Jump -> next b1()
  b1():
    v3 = GetConstructPrototype v0 layout=L0
    v4 = NewObject v3 layout=L0
    v5 = Predict v4 predicted=L0
    PropSet v5, v1 key="name"
    v7 = Predict v4 predicted=L0
    PropSet v7, v2 key="n"
    v9 = Undefined
    v10 = TypeTest v9 testedType=object
    Branch v10 -> taken b2(| 0), notTaken b3(| 0)
  b2(v11):
    Jump -> next b4(v11)
  b3(v12):
    Jump -> next b4(v4)
  b4(v13):
    Return v13
}
data d0 root=Array{contents=anything elements=[Array{contents=anything elements=[Number{value=1}, Boolean{}]}, Array{contents=anything elements=[Number{value=3}, Boolean{value=true}]}, Array{contents=anything elements=[Number{value=5}, Boolean{}]}, Array{contents=anything elements=[Number{value=1001}, Boolean{value=true}]}]}
entry module=m0
)IR"_s;

static void testUnspecifiedCellBehindMerge()
{
    constexpr ASCIILiteral expected = "b(spare0,s,0.5)\nb(f0,s0,0.5)\nb(spare0,s,0.5)\nb(f0,s0,0.5)\n"_s;
    checkOutputOfProgramInShell(unspecifiedCellBehindMerge, expected, { });
    for (ASCIILiteral option : { "validate"_s, "check=VerifyEachPass"_s, "disable=pass:SplitFunctionsAtCalls"_s, "disable=pass:SinkAllocations"_s })
        checkOutputOfProgramInShell(unspecifiedCellBehindMerge, expected, { option });
}

std::span<const NativeTest> splitFunctionProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "split-function-programs/unspecified-cell-behind-merge"_s, testUnspecifiedCellBehindMerge },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
