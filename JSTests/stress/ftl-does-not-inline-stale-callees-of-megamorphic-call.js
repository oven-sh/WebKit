//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault

// A call site calls two callees while it runs in LLInt and Baseline, so their call IC is a stub that counts those two.
// In the DFG code the call site calls twelve callees, and the call IC of that code goes virtual. It names no callee.
// The two callees that Baseline counted are then not known to be all of them, or even the frequent ones. The FTL must
// not switch over them. It emits the call.

if (!jscOptions().useFTLJIT)
    quit();

const iterations = 100;
var callsInFTL = new Array(12).fill(0);
var loopCallsInFTL = 0;

// Twelve functions that do not share an executable. Each gets to the FTL only as part of a caller.
let callees = [
    function (i) { callsInFTL[0] += $vm.ftlTrue() ? 1 : 0; return i + 0; },
    function (i) { callsInFTL[1] += $vm.ftlTrue() ? 1 : 0; return i + 1; },
    function (i) { callsInFTL[2] += $vm.ftlTrue() ? 1 : 0; return i + 2; },
    function (i) { callsInFTL[3] += $vm.ftlTrue() ? 1 : 0; return i + 3; },
    function (i) { callsInFTL[4] += $vm.ftlTrue() ? 1 : 0; return i + 4; },
    function (i) { callsInFTL[5] += $vm.ftlTrue() ? 1 : 0; return i + 5; },
    function (i) { callsInFTL[6] += $vm.ftlTrue() ? 1 : 0; return i + 6; },
    function (i) { callsInFTL[7] += $vm.ftlTrue() ? 1 : 0; return i + 7; },
    function (i) { callsInFTL[8] += $vm.ftlTrue() ? 1 : 0; return i + 8; },
    function (i) { callsInFTL[9] += $vm.ftlTrue() ? 1 : 0; return i + 9; },
    function (i) { callsInFTL[10] += $vm.ftlTrue() ? 1 : 0; return i + 10; },
    function (i) { callsInFTL[11] += $vm.ftlTrue() ? 1 : 0; return i + 11; },
];
for (let callee of callees)
    noFTL(callee);

function loop(callee) {
    loopCallsInFTL += $vm.ftlTrue() ? 1 : 0;
    let sum = 0;
    for (let i = 0; i < iterations; i++)
        sum += callee(i);
    return sum;
}
noInline(loop);

const triangle = iterations * (iterations - 1) / 2;
function check(index) {
    let result = loop(callees[index]);
    if (result !== triangle + iterations * index)
        throw new Error("bad result " + result + " with callee " + index);
}

// Two callees, until the DFG has compiled the loop.
for (let i = 0; !numberOfDFGCompiles(loop); i++) {
    if (i > 100000)
        throw new Error("the DFG did not compile the loop");
    check(i & 1);
}

// Then all twelve in turn, until the loop runs in the FTL.
for (let i = 0; i < 100000 && !loopCallsInFTL; i++)
    check(i % callees.length);
if (!loopCallsInFTL)
    throw new Error("the FTL did not compile the loop");

for (let i = 0; i < 120; i++)
    check(i % callees.length);
for (let index = 0; index < callees.length; index++) {
    if (callsInFTL[index])
        throw new Error("the FTL inlined callee " + index);
}
