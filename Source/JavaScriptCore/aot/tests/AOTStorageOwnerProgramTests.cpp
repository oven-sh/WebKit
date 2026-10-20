/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// function lex(src) { const out = []; for (let i = 0; i + 3 <= src.length; i += 3) out.push({ text: src.slice(i, i + 3) }); return out }
// function count(toks) { const m = new Map(); for (const t of toks) m.set(t.text, (m.get(t.text) ?? 0) + 1); return m }
// let src = ""; for (let i = 0; i < 200; i++) src += "ab" + (i % 10); let n = 0; for (let r = 0; r < 10; r++) n += count(lex(src)).size; print(n)
// Nothing in the loop of `count` changes the storage of `toks`, so its address is read once in front of the loop, and nobody but `count` holds the array.
static constexpr ASCIILiteral arrayReadInLoopThatCollects = R"IR(layout L0 name="Token" fields=[{key="text" declared={type=anything} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = ClaimDeclared v0 declared={type=anything predicted=function}
    v2 = NewClosure function=f2
    v3 = ClaimDeclared v2 declared={type=anything predicted=function}
    v4 = String string=""
    v5 = Predict v4 predicted=string
    v6 = Integer
    v7 = Predict v6 predicted=number
    Jump -> next b2(v5, v7)
  b2(v8, v9):
    LoopHint
    v11 = Integer integer=200
    v12 = Less v9, v11
    Branch v12 -> taken b3(| 0), notTaken b6(| 0)
  b3(v13):
    Jump -> next b4()
  b4():
    v14 = String string="ab"
    v15 = Integer integer=10
    v16 = Mod v13, v15
    v17 = Add v14, v16
    v18 = Add v8, v17
    v19 = ClaimDeclared v18 declared={type=anything predicted=string}
    Jump -> next b5()
  b5():
    v20 = Increment v13
    v21 = ClaimDeclared v20 declared={type=anything predicted=number}
    Jump -> next b2(v19, v21)
  b6(v22):
    Jump -> next b7()
  b7():
    v23 = Integer
    v24 = Predict v23 predicted=number
    v25 = Integer
    v26 = Predict v25 predicted=number
    Jump -> next b8(v24, v26)
  b8(v27, v28):
    LoopHint
    v30 = Integer integer=10
    v31 = Less v28, v30
    Branch v31 -> taken b9(| 0), notTaken b12(| 0)
  b9(v32):
    Jump -> next b10()
  b10():
    v33 = Predict v3 predicted=function
    v34 = Predict v1 predicted=function
    v35 = Call v34, -, v8 calleeName="lex"
    v36 = Predict v35 predicted=Array<L0>
    v37 = Call v33, -, v36 calleeName="count"
    v38 = Predict v37 predicted=Map
    v39 = PropGet v38 key="size"
    v40 = Predict v39 predicted=number
    v41 = Add v27, v40
    v42 = ClaimDeclared v41 declared={type=anything predicted=number}
    Jump -> next b11()
  b11():
    v43 = Increment v32
    v44 = ClaimDeclared v43 declared={type=anything predicted=number}
    Jump -> next b8(v42, v44)
  b12(v45):
    Jump -> next b13()
  b13():
    v46 = Builtin builtin=GlobalThis
    v47 = PropGet v46 key="print" [isFreeName]
    v48 = Call v47, -, v27 calleeName="print"
    v49 = Undefined
    Return v49
}
function f1 name="lex" length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=Array<L0>}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = NewArray contents=anything
    v2 = Predict v1 predicted=Array<never>
    v3 = Predict v2 predicted=Array
    v4 = Integer
    v5 = Predict v4 predicted=number
    Jump -> next b2(v5)
  b2(v6):
    LoopHint
    v8 = Integer integer=3
    v9 = Add v6, v8
    v10 = PropGet v0 key="length"
    v11 = LessEq v9, v10
    Branch v11 -> taken b3(), notTaken b5()
  b3():
    v12 = PropGet v3 key="push"
    v13 = Predict v12 predicted=function
    v14 = PropGet v0 key="slice"
    v15 = Integer integer=3
    v16 = Add v6, v15
    v17 = Call v14, v0, v6, v16 calleeKey="slice" calleeName="slice"
    v18 = NewObject -, v17 layout=L0 fields=[0]
    v19 = FinishObject v18
    v20 = Call v13, v3, v19 calleeKey="push" calleeName="push"
    Jump -> next b4()
  b4():
    v21 = Integer integer=3
    v22 = Add v6, v21
    v23 = ClaimDeclared v22 declared={type=anything predicted=number}
    Jump -> next b2(v23)
  b5():
    v24 = Predict v3 predicted=Array<L0>
    Return v24
}
function f2 name="count" length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything predicted=Map}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="Map" [isFreeName]
    v3 = Predict v2 predicted=builtin(Map)
    v4 = Construct v3 calleeName="Map"
    v5 = Predict v4 predicted=Map
    v6 = IteratorOpen v0
    Jump -> next b2()
  b2():
    LoopHint
    v8 = IteratorResume v6
    v9 = ResultAt v8
    v10 = ResultAt v8 index=1
    Branch v9 -> taken b3(), notTaken b4()
  b3():
    Return v5
  b4():
    Jump -> next b5()
  b5() handler=b10():
    v11 = PropGet v5 key="set"
    v12 = Predict v11 predicted=function
    v13 = PropGet v10 key="text"
    v14 = PropGet v5 key="get"
    v15 = Predict v14 predicted=function
    v16 = PropGet v10 key="text"
    v17 = Call v15, v5, v16 calleeKey="get" calleeName="get"
    v18 = TypeTest v17 testedType=undefined|null
    Branch v18 -> taken b6(| 0), notTaken b8(| 0)
  b6(v19) handler=b10():
    Jump -> next b7()
  b7() handler=b10():
    v20 = Integer
    Jump -> next b9(v20)
  b8(v21) handler=b10():
    Jump -> next b9(v21)
  b9(v22) handler=b10():
    v23 = Integer integer=1
    v24 = Add v22, v23
    v25 = Call v12, v5, v13, v24 calleeKey="set" calleeName="set"
    Jump -> next b2()
  b10() role=IterationCleanup subject=v6:
    v26 = CatchException
    IteratorClose v6 completion=Throw
    Rethrow v26
}
entry
)IR"_s;

static void testArrayReadInLoopThatCollects()
{
    for (ASCIILiteral stress : { "--slowPathAllocsBetweenGCs=1"_s, "--slowPathAllocsBetweenGCs=3"_s, "--collectContinuously=1"_s }) {
        checkOutputOfProgramInShell(arrayReadInLoopThatCollects, "100\n"_s, { }, ""_s, { stress });
        checkOutputOfProgramInShell(arrayReadInLoopThatCollects, "100\n"_s, { "heat=EverythingIsHot"_s }, ""_s, { stress });
    }
}

std::span<const NativeTest> storageOwnerProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "storage-owner-programs/array-read-in-loop-that-collects"_s, testArrayReadInLoopThatCollects },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
