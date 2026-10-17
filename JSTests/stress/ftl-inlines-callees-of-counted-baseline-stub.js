//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault
//@ runDefault("--useGlobalInliningPlanner=false")

// A call site calls two callees while it runs in LLInt and Baseline. Their call IC becomes a stub that counts the
// calls to each callee. The call site then calls one of them only. The DFG does not inline a call with two callees,
// so its own call IC sees that one callee and has no stub. The FTL takes what both ICs have seen. Both callees come
// from counted calls, so the FTL switches over them and inlines both. Before, the joined status was not "based on a
// stub", and the FTL emitted a call that it never replaced.

if (!jscOptions().useFTLJIT)
    quit();

const iterations = 100;
let callsInFTL = [0, 0];

class First {
    f(i) {
        callsInFTL[0] += $vm.ftlTrue() ? 1 : 0;
        return i + 1;
    }
}
class Second {
    f(i) {
        callsInFTL[1] += $vm.ftlTrue() ? 1 : 0;
        return i + 2;
    }
}
// The callees get to the FTL only as part of a caller.
noFTL(First.prototype.f);
noFTL(Second.prototype.f);

function loop(receiver) {
    let sum = 0;
    for (let i = 0; i < iterations; i++)
        sum += receiver.f(i);
    return sum;
}
noInline(loop);

let receivers = [new First, new Second];
const triangle = iterations * (iterations - 1) / 2;
function check(index) {
    let result = loop(receivers[index]);
    if (result !== triangle + iterations * (index + 1))
        throw new Error("bad result " + result + " with receiver " + index);
}

// Both callees, while the loop runs in LLInt and Baseline.
for (let i = 0; i < 10; i++)
    check(i & 1);
if (numberOfDFGCompiles(loop))
    throw new Error("the DFG compiled the loop before the first callee ran alone");

// Then the first callee only. The DFG code never calls the second one.
for (let i = 0; i < 20000 && !callsInFTL[0]; i++)
    check(0);
if (!callsInFTL[0])
    throw new Error("the FTL did not inline the callee of the call site");

// The FTL code has the second callee inline too.
let compiles = numberOfDFGCompiles(loop);
check(1);
if (!callsInFTL[1])
    throw new Error("the FTL did not inline the callee that only the Baseline stub has");
if (numberOfDFGCompiles(loop) !== compiles)
    throw new Error("the loop was compiled again for the callee that only the Baseline stub has");
