//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useConcurrentJIT=1", "--dumpDFGDisassembly=1", "--poisonDeadOSRExitVariables=0")

// The collector's end phase jettisons optimized code whose weak references died and, with DFG dumps on, prints the
// CodeBlock (CodeBlock::dump -> FunctionExecutable::inferredNameForTools) from the mutator thread while its atom string
// table is cleared. For a function decoded from the bytecode cache whose name nobody asked for yet, that must not
// materialize (atomize) the name.

function makeGetter() { return function getXNeverIntrospected(o) { return o.x + 1; }; }
let getX = makeGetter();
function churn() {
    class Tmp { constructor(i) { this.x = i; this.y = 2; } }
    let s = 0;
    for (let i = 0; i < testLoopCount * 20; ++i)
        s += getX(new Tmp(i));
    return s;
}
churn();
churn = null;
// Tmp and its Structure are garbage now; getX's optimized code holds them weakly and is jettisoned during this GC.
fullGC();
fullGC();
if (getX({ x: 1, z: 3 }) !== 2)
    throw new Error("bad result");
